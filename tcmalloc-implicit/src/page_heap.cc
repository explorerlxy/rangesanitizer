// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright (c) 2008, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// ---
// Author: Sanjay Ghemawat <opensource@google.com>

#include "config.h"

#include <inttypes.h>                   // for PRIuPTR
#include <errno.h>                      // for ENOMEM, errno

#include <algorithm>
#include <limits>

#include <sys/mman.h>

#include "gperftools/malloc_extension.h"      // for MallocRange, etc
#include "base/basictypes.h"
#include "base/commandlineflags.h"
#include "internal_logging.h"  // for ASSERT, TCMalloc_Printer, etc
#include "page_heap_allocator.h"  // for PageHeapAllocator
#include "static_vars.h"       // for Static
#include "system-alloc.h"      // for TCMalloc_SystemAlloc, etc

#ifdef BASEBOUNDS_X86_STACK
#include "thread_cache.h"      // for ThreadCache
#endif

#include <fcntl.h>

DEFINE_double(tcmalloc_release_rate,
              EnvToDouble("TCMALLOC_RELEASE_RATE", 1.0),
              "Rate at which we release unused memory to the system.  "
              "Zero means we never release memory back to the system.  "
              "Increase this flag to return memory faster; decrease it "
              "to return memory slower.  Reasonable rates are in the "
              "range [0,10]");

DEFINE_int64(tcmalloc_heap_limit_mb,
              EnvToInt("TCMALLOC_HEAP_LIMIT_MB", 0),
              "Limit total size of the process heap to the "
              "specified number of MiB. "
              "When we approach the limit the memory is released "
              "to the system more aggressively (more minor page faults). "
              "Zero means to allocate as long as system allows.");

namespace tcmalloc {

#ifdef BASEBOUNDS_X86
// void* tc_sc_end_map[BB_TAG_SHIFT];

struct mapinfo
{
    uintptr_t start, end;
};

#define LOC_START 1
#define LOC_END 2
#define LOC_REM 3
int get_proc_maps(struct mapinfo *maps, int maps_size)
{
    char buf[4096];
    int fd;
    ssize_t bytes;
    int i;
    int num_maps = 0;
    int loc = LOC_START;
    char addrbuf[32];
    int addrbuf_pos = 0;

    fd = open("/proc/self/maps", O_RDONLY);
    if (fd == -1) {
        perror("get_proc_maps");
        fprintf(stderr, "Could not open /proc/self/maps!\n");
        abort();
    }

    /* The easiest solution would be to use strtol directly (and use its endptr)
     * twice, followed by a strchr for the \n. However, we are dealing with
     * chunks complicating that scheme a bit (as an addr might be split over
     * chunks). */
    while ((bytes = read(fd, buf, sizeof(buf))) > 0) {
        // fprintf(stderr, "%s\n", buf);
        for (i = 0; i < bytes; i++) {
            if (loc == LOC_START || loc == LOC_END) {
                if (buf[i] == '-') {
                    if (num_maps == maps_size) {
                        fprintf(stderr, "Error: more than %d maps\n",
                                maps_size);
                        return num_maps;
                    }
                    addrbuf[addrbuf_pos] = '\0';
                    maps[num_maps].start = strtoll(addrbuf, NULL, 16);
                    addrbuf_pos = 0;
                    loc = LOC_END;
                } else if (buf[i] == ' ') {
                    addrbuf[addrbuf_pos] = '\0';
                    maps[num_maps].end = strtoll(addrbuf, NULL, 16);
                    addrbuf_pos = 0;
                    loc = LOC_REM;
                    num_maps++;
                } else
                    addrbuf[addrbuf_pos++] = buf[i];
            } else if (loc == LOC_REM) {
                if (buf[i] == '\n')
                    loc = LOC_START;
            }
        }
    }
    close(fd);
    return num_maps;
}

void dump_maps(struct mapinfo *maps, int num_maps)
{
    int i;
    fprintf(stderr, "-MAPPINGS- (%d total)\n", num_maps);
    for (i = 0; i < num_maps; i++)
        fprintf(stderr, " 0x%lx - 0x%lx\n", maps[i].start, maps[i].end);
}

#endif

struct SCOPED_LOCKABLE PageHeap::LockingContext {
  PageHeap * const heap;
  size_t grown_by = 0;

  explicit LockingContext(PageHeap* heap, SpinLock* lock) EXCLUSIVE_LOCK_FUNCTION(lock)
      : heap(heap) {
    lock->Lock();
  }
  ~LockingContext() UNLOCK_FUNCTION() {
    heap->HandleUnlock(this);
  }
};

PageHeap::PageHeap(Length smallest_span_size)
    : smallest_span_size_(smallest_span_size),
      pagemap_(MetaDataAlloc),
      scavenge_counter_(0),
      // Start scavenging at kMaxPages list
      release_index_(kMaxPages),
      aggressive_decommit_(false) {
  COMPILE_ASSERT(kClassSizesMax <= (1 << PageMapCache::kValuebits), valuebits);
  // smallest_span_size needs to be power of 2.
  CHECK_CONDITION((smallest_span_size_ & (smallest_span_size_-1)) == 0);
#ifdef BASEBOUNDS_X86
  for (int t = 0; t < BB_TAG_SHIFT; t++) {
    for (int i = 0; i < kMaxPages; i++) {
      DLL_Init(&class_free[t].free_[i].normal);
      DLL_Init(&class_free[t].free_[i].returned);
    }
  }

  struct mapinfo maps[512];
  size_t num_maps;
  num_maps = get_proc_maps(maps, 512);
  // dump_maps(maps, num_maps);

  // alignment for internal expectations, like central_freelist expecting 
  // some minimum number of pages to work with
  #define ROUND_UP(x) ( (((uintptr_t)(x)) + 0x4000-1)  & (~(0x4000-1)) ) 
 
  // assign the virtual address range for implicit tags
  // pre-initialize the freelists with their complete ranges (NORESERVE)
  // skip freelist 0 for internal allocations -- it can use GrowHeap() per usual

  for (int t = 1; t < BB_TAG_SHIFT; t++) {
    void* start = (void*)(((uintptr_t)t << BB_TAG_SHIFT));

    // mmapings are already done by pld
    // the mappings need to be done really early to avoid anything in between
    // allocating in reserved areas (upper address range gets plugged by pld)
    // set up the source metadata to use for tcmalloc here

    for(size_t m = 0; m < num_maps; m++){
        // we need to find the 'large map' (i.e., not globals) for this class
        if(maps[m].start == (uintptr_t)start){
            start = (void*)(maps[m].start); // start at the beginning of this mapping
            size_t offset = 1;
            while(m+offset < num_maps){
                // check if there is a same-tag section directly afterwards
                size_t tag_start = PTR_GET_TAG(start);
                void* next = (void*)(maps[m+offset].start);
                if(tag_start == PTR_GET_TAG(next)){
                  // if the END of that section has a different tag, it cannot be globals,
                  // so this must be the start address of the large map we are looking for
                  if(tag_start != PTR_GET_TAG(maps[m+offset].end)){
                    start = (void*)(ROUND_UP(maps[m+offset].start)); // start of map
                    break;
                  }
                  // keep scanning forward
                }
                else{
                  break;
                }
                offset++;
            }
            break;
        }
    }
    
    class_free[t].source = (char*)start;
    class_free[t].size_remaining = ((uintptr_t)(t+1) << BB_TAG_SHIFT) - (uintptr_t)start;
  }

#else
  for (int i = 0; i < kMaxPages; i++) {
    DLL_Init(&free_[i].normal);
    DLL_Init(&free_[i].returned);
  }
#endif
}
#ifdef BASEBOUNDS_X86
Span* PageHeap::SearchFreeAndLargeLists(Length n, uint32 bb_tag) {
#else
Span* PageHeap::SearchFreeAndLargeLists(Length n) {
#endif
  ASSERT(lock_.IsHeld());
  ASSERT(Check());
  ASSERT(n > 0);

  // Find first size >= n that has a non-empty list
  for (Length s = n; s <= kMaxPages; s++) {
#ifdef BASEBOUNDS_X86
    Span* ll = &class_free[bb_tag].free_[s - 1].normal;
#else
    Span* ll = &free_[s - 1].normal;
#endif
    // If we're lucky, ll is non-empty, meaning it has a suitable span.
    if (!DLL_IsEmpty(ll)) {
      ASSERT(ll->next->location == Span::ON_NORMAL_FREELIST);
      return Carve(ll->next, n);
    }
    // Alternatively, maybe there's a usable returned span.
#ifdef BASEBOUNDS_X86
    ll = &class_free[bb_tag].free_[s - 1].returned;
#else
    ll = &free_[s - 1].returned;
#endif
    if (!DLL_IsEmpty(ll)) {
      // We did not call EnsureLimit before, to avoid releasing the span
      // that will be taken immediately back.
      // Calling EnsureLimit here is not very expensive, as it fails only if
      // there is no more normal spans (and it fails efficiently)
      // or SystemRelease does not work (there is probably no returned spans).
#ifdef BASEBOUNDS_X86
      if (EnsureLimit(n, bb_tag)) {
#else
      if (EnsureLimit(n)) {
#endif
        // ll may have became empty due to coalescing
        if (!DLL_IsEmpty(ll)) {
          ASSERT(ll->next->location == Span::ON_RETURNED_FREELIST);
          return Carve(ll->next, n);
        }
      }
    }
  }
  // No luck in free lists, our last chance is in a larger class.
#ifdef BASEBOUNDS_X86
  return AllocLarge(n, bb_tag);  // May be NULL
#else
  return AllocLarge(n);  // May be NULL
#endif
}

static const size_t kForcedCoalesceInterval = 128*1024*1024;

Length PageHeap::RoundUpSize(Length n) {
  Length rounded_n = (n + smallest_span_size_ - 1) & ~(smallest_span_size_ - 1);
  if (rounded_n < n) {
    // Overflow happened. So make sure we oom by asking for biggest
    // amount possible.
    return std::numeric_limits<Length>::max() & ~(smallest_span_size_ - 1);
  }

  return rounded_n;
}

void PageHeap::HandleUnlock(LockingContext* context) {
  StackTrace* t = nullptr;
  if (context->grown_by) {
    t = Static::stacktrace_allocator()->New();
    t->size = context->grown_by;
  }

  lock_.Unlock();

  if (t) {
    t->depth = GetStackTrace(t->stack, kMaxStackDepth-1, 0);
    Static::push_growth_stack(t);
  }
}

#ifdef BASEBOUNDS_X86
Span* PageHeap::NewWithSizeClass(Length n, uint32 sizeclass, uint32 bb_tag) {
#else
Span* PageHeap::NewWithSizeClass(Length n, uint32 sizeclass) {
#endif
  LockingContext context{this, &lock_};
#ifdef BASEBOUNDS_X86
  Span* span = NewLocked(n, &context, bb_tag);
#else
  Span* span = NewLocked(n, &context);
#endif
  if (!span) {
    return span;
  }

  InvalidateCachedSizeClass(span->start);
  if (sizeclass) {
    RegisterSizeClass(span, sizeclass);
  }
  return span;
}
#ifdef BASEBOUNDS_X86
Span* PageHeap::NewLocked(Length n, LockingContext* context, uint32 bb_tag) {
#else
Span* PageHeap::NewLocked(Length n, LockingContext* context) {
#endif
  ASSERT(lock_.IsHeld());
  ASSERT(Check());
  n = RoundUpSize(n);

#ifdef BASEBOUNDS_X86
  Span* result = SearchFreeAndLargeLists(n, bb_tag);
#else
  Span* result = SearchFreeAndLargeLists(n);
#endif
  if (result != NULL)
    return result;

  if (stats_.free_bytes != 0 && stats_.unmapped_bytes != 0
      && stats_.free_bytes + stats_.unmapped_bytes >= stats_.system_bytes / 4
      && (stats_.system_bytes / kForcedCoalesceInterval
          != (stats_.system_bytes + (n << kPageShift)) / kForcedCoalesceInterval)) {
    // We're about to grow heap, but there are lots of free pages.
    // tcmalloc's design decision to keep unmapped and free spans
    // separately and never coalesce them means that sometimes there
    // can be free pages span of sufficient size, but it consists of
    // "segments" of different type so page heap search cannot find
    // it. In order to prevent growing heap and wasting memory in such
    // case we're going to unmap all free pages. So that all free
    // spans are maximally coalesced.
    //
    // We're also limiting 'rate' of going into this path to be at
    // most once per 128 megs of heap growth. Otherwise programs that
    // grow heap frequently (and that means by small amount) could be
    // penalized with higher count of minor page faults.
    //
    // See also large_heap_fragmentation_unittest.cc and
    // https://github.com/gperftools/gperftools/issues/371
#ifdef BASEBOUNDS_X86
    ReleaseAtLeastNPages(static_cast<Length>(0x7fffffff), bb_tag);
#else
    ReleaseAtLeastNPages(static_cast<Length>(0x7fffffff));
#endif

    // then try again. If we are forced to grow heap because of large
    // spans fragmentation and not because of problem described above,
    // then at the very least we've just unmapped free but
    // insufficiently big large spans back to OS. So in case of really
    // unlucky memory fragmentation we'll be consuming virtual address
    // space, but not real memory
#ifdef BASEBOUNDS_X86
    result = SearchFreeAndLargeLists(n, bb_tag);
#else
    result = SearchFreeAndLargeLists(n);
#endif
    if (result != NULL) return result;
  }

  // Grow the heap and try again.
#ifdef BASEBOUNDS_X86
  if (!GrowHeap(n, context, bb_tag)) {
#else
  if (!GrowHeap(n, context)) {
#endif
    ASSERT(stats_.unmapped_bytes+ stats_.committed_bytes==stats_.system_bytes);
    ASSERT(Check());
    // underlying SysAllocator likely set ENOMEM but we can get here
    // due to EnsureLimit so we set it here too.
    //
    // Setting errno to ENOMEM here allows us to avoid dealing with it
    // in fast-path.
    errno = ENOMEM;
    return NULL;
  }
#ifdef BASEBOUNDS_X86
  return SearchFreeAndLargeLists(n, bb_tag);
#else
  return SearchFreeAndLargeLists(n);
#endif
}


#ifdef BASEBOUNDS_X86
// here 'n' is in pages, and 'alignment' is in bytes
// this New() variant is useful for a complete sizeclass,
// where a guard page is not needed because two fake objects are used
Span* PageHeap::NewAlignedWithSizeClass(Length n, Length alignment, uint32 sizeclass, uint32 bb_tag) {
  n = RoundUpSize(n);

  // minimal alignment is kPageSize
  if(alignment < kPageSize) alignment = kPageSize;

  // Allocate extra pages and carve off an aligned portion
  const Length align_pages = tcmalloc::pages(alignment);
  const Length alloc = n + align_pages;
  if (alloc < n || alloc < align_pages) {
    // overflow means we asked huge amounts, so lets trigger normal
    // oom handling by asking enough to trigger oom.
#ifdef BASEBOUNDS_X86
    Span* span = New(std::numeric_limits<Length>::max(), bb_tag);
#else
    Span* span = New(std::numeric_limits<Length>::max());
#endif
    CHECK_CONDITION(span == nullptr);
    return nullptr;
  }

  LockingContext context{this, &lock_};
#ifdef BASEBOUNDS_X86
  Span* span = NewLocked(alloc, &context, bb_tag);
#else
  Span* span = NewLocked(alloc, &context);
#endif
  if (PREDICT_FALSE(span == nullptr)) return nullptr;

  // calculate adjustment of the returned memory so it is aligned
  uintptr_t ptr = reinterpret_cast<uintptr_t>(span->start << kPageShift);
  size_t adjust = 0;
  if ((ptr & (alignment - 1)) != 0) {
    adjust = alignment - (ptr & (alignment - 1));
  }

  // adjust in bytes -> pages
  adjust >>= kPageShift;

  // split the adjustment delta off, span becomes the remainder
  ASSERT(adjust < alloc);
  if (adjust > 0) {
    Span* rest = Split(span, adjust);
    DeleteLocked(span);
    span = rest;
  }

  // split excess trailing bytes off, span does not change
  ASSERT(span->length >= n);
  if (span->length > n) {
    Span* trailer = Split(span, n);
    DeleteLocked(trailer);
  }
  InvalidateCachedSizeClass(span->start);
  if (sizeclass) {
    RegisterSizeClass(span, sizeclass);
  }
  return span;
}

// here 'n' is in pages, and 'alignment' is in bytes
// this New() variant is useful if a guard page is needed in front
// e.g. for large allocations
Span* PageHeap::NewAlignedWithSizeClassPrefix(Length n, Length alignment, uint32 sizeclass, uint32 bb_tag) {
  n = RoundUpSize(n);

  // here we want to make sure some memory is included in the span
  // before the start of the aligned object, such that we can set up
  // a GUARD PAGE there.
  // if the metadata of a previous object is then read, it will trap
  // since the object is always padded to ensure alignment,
  // there should always be space in front for a guard page.

  // minimal alignment is kPageSize
  if(alignment < kPageSize) alignment = kPageSize;

  // Allocate extra pages and carve off an aligned portion
  const Length align_pages = tcmalloc::pages(alignment);
  const Length alloc = n + align_pages;
  if (alloc < n || alloc < align_pages) {
    // overflow means we asked huge amounts, so lets trigger normal
    // oom handling by asking enough to trigger oom.
#ifdef BASEBOUNDS_X86
    Span* span = New(std::numeric_limits<Length>::max(), bb_tag);
#else
    Span* span = New(std::numeric_limits<Length>::max());
#endif
    CHECK_CONDITION(span == nullptr);
    return nullptr;
  }

  LockingContext context{this, &lock_};
#ifdef BASEBOUNDS_X86
  Span* span = NewLocked(alloc, &context, bb_tag);
#else
  Span* span = NewLocked(alloc, &context);
#endif
  if (PREDICT_FALSE(span == nullptr)) return nullptr;

  // calculate adjustment of the returned memory so it is aligned
  uintptr_t ptr = reinterpret_cast<uintptr_t>(span->start << kPageShift);
  size_t adjust = 0;
  if ((ptr & (alignment - 1)) != 0) {
    adjust = alignment - (ptr & (alignment - 1));
  }
  else{
    // ptr is already aligned at the base of the span, but we need room for
    // the prefix metadata: shift the entire object by the alignment (this should fit)
    adjust = alignment;
  }

  // adjust in bytes -> pages
  adjust >>= kPageShift;

  // save one kPageSize for guard (kPageSize is 2 pages by default)
  adjust--;

  // split the adjustment delta off, span becomes the remainder
  ASSERT(adjust < alloc);
  if (adjust > 0) {
    Span* rest = Split(span, adjust);
    DeleteLocked(span);
    span = rest;
  }

  // create guard (4kB on x86 but kPageSize is 2 pages in tcmalloc by default)
  uintptr_t guard = span->start << kPageShift;
  mprotect((void*)guard, kPageSize, PROT_NONE);
  span->meta_guard_type = Span::GUARD_REGULAR;
  span->meta_guard_addr = guard; // set addr
  // move the start PageID with the equivalent of one kPageSize
  span->start++;
  span->length--;
  // notify the span-page map of the new span start address
  RecordSpan(span);

  // split excess trailing bytes off, span does not change
  ASSERT(span->length >= n);
  if (span->length > n) {
    Span* trailer = Split(span, n);
    DeleteLocked(trailer);
  }
  InvalidateCachedSizeClass(span->start);
  if (sizeclass) {
    RegisterSizeClass(span, sizeclass);
  }
  return span;
}


// here 'n' is in pages, and 'alignment' is in bytes
// this New() variant is useful if a guard page is needed in front
// of a very huge span
Span* PageHeap::NewAlignedPrefixHuge(Length n, Length alignment, uint32 bb_tag) {
  n = RoundUpSize(n);

  // here we want to create a guard page for a huge span
  // since the span concerns a huge size class, creating a complete fake object
  // wastes a lot of memory
  // instead, we set up the memory like this, e.g. for a 512 kB huge obj:
  // [16kB guard]---[512kB]---[512kB]
  // and then, the middle 512 kB, we can unmap the portion up until the last page
  // [16kB guard]---[496kB FREE]---[16kb Metadata]---[512kB object]

  // minimal alignment is kPageSize
  if(alignment < kPageSize) alignment = kPageSize;

  // Allocate extra pages and carve off an aligned portion
  const Length align_pages = tcmalloc::pages(alignment);
  const Length alloc = n + align_pages*2;
  if (alloc < n || alloc < align_pages) {
    // overflow means we asked huge amounts, so lets trigger normal
    // oom handling by asking enough to trigger oom.
#ifdef BASEBOUNDS_X86
    Span* span = New(std::numeric_limits<Length>::max(), bb_tag);
#else
    Span* span = New(std::numeric_limits<Length>::max());
#endif
    CHECK_CONDITION(span == nullptr);
    return nullptr;
  }

  LockingContext context{this, &lock_};
#ifdef BASEBOUNDS_X86
  Span* span = NewLocked(alloc, &context, bb_tag);
#else
  Span* span = NewLocked(alloc, &context);
#endif
  if (PREDICT_FALSE(span == nullptr)) return nullptr;

  // calculate adjustment of the returned memory so it is aligned
  uintptr_t ptr = reinterpret_cast<uintptr_t>(span->start << kPageShift);
  size_t adjust = 0;
  if ((ptr & (alignment - 1)) != 0) {
    adjust = alignment - (ptr & (alignment - 1));
  }
  else{
    // ptr is already aligned at the base of the span, but we need room for
    // the prefix metadata: shift the entire object by the alignment (this should fit)
    adjust = alignment;
  }

  // adjust in bytes -> pages
  adjust >>= kPageShift;

  // save one kPageSize for guard (kPageSize is 2 pages by default)
  adjust--;

  // split the adjustment delta off, span becomes the remainder
  ASSERT(adjust < alloc);
  if (adjust > 0) {
    Span* rest = Split(span, adjust);
    DeleteLocked(span);
    span = rest;
  }


  // create guard (4kB on x86 but kPageSize is 2 pages in tcmalloc by default)
  uintptr_t guard = span->start << kPageShift;
  mprotect((void*)guard, kPageSize, PROT_NONE);

  // split off the single guard page into its own span (do not free it)
  Span *free_chunk_post_guard = Split(span, 1);

  // this split should give us a new start with room for a metadata page, and then
  // a new aligned start: align_pages-1 == start one kPageSize before aligned
  Span *resulting;
  if(align_pages == 1){
    // there is no free chunk
    resulting = free_chunk_post_guard;
  }
  else{
    resulting = Split(free_chunk_post_guard, align_pages-1);
    DeleteLocked(free_chunk_post_guard);
  }
  span = resulting;

  // span points to the metadata page before the true desired start, so offset it.
  // move the start PageID with the equivalent of one kPageSize
  span->start++;
  span->length--;
  span->meta_guard_type = Span::GUARD_HUGE;
  span->meta_guard_addr = guard; // set addr
  // notify the span-page map of the new span start address
  RecordSpan(span);

  // split excess trailing bytes off, span does not change
  ASSERT(span->length >= n);
  if (span->length > n) {
    Span* trailer = Split(span, n);
    DeleteLocked(trailer);
  }
  InvalidateCachedSizeClass(span->start);
  return span;
}
#endif

#ifdef BASEBOUNDS_X86
Span* PageHeap::NewAligned(Length n, Length align_pages, uint32 bb_tag) {
#else
Span* PageHeap::NewAligned(Length n, Length align_pages) {
#endif
  n = RoundUpSize(n);

  // Allocate extra pages and carve off an aligned portion
  const Length alloc = n + align_pages;
  if (alloc < n || alloc < align_pages) {
    // overflow means we asked huge amounts, so lets trigger normal
    // oom handling by asking enough to trigger oom.
#ifdef BASEBOUNDS_X86
    Span* span = New(std::numeric_limits<Length>::max(), bb_tag);
#else
    Span* span = New(std::numeric_limits<Length>::max());
#endif
    CHECK_CONDITION(span == nullptr);
    return nullptr;
  }

  LockingContext context{this, &lock_};
#ifdef BASEBOUNDS_X86
  Span* span = NewLocked(alloc, &context, bb_tag);
#else
  Span* span = NewLocked(alloc, &context);
#endif
  if (PREDICT_FALSE(span == nullptr)) return nullptr;

  // Skip starting portion so that we end up aligned
  Length skip = 0;
  size_t align_bytes = align_pages << kPageShift;
  while ((((span->start+skip) << kPageShift) & (align_bytes - 1)) != 0) {
    skip++;
  }
  ASSERT(skip < alloc);
  if (skip > 0) {
    Span* rest = Split(span, skip);
    DeleteLocked(span);
    span = rest;
  }

  ASSERT(span->length >= n);
  if (span->length > n) {
    Span* trailer = Split(span, n);
    DeleteLocked(trailer);
  }
  InvalidateCachedSizeClass(span->start);
  return span;
}

#ifdef BASEBOUNDS_X86
Span* PageHeap::AllocLarge(Length n, uint32 bb_tag) {
#else
Span* PageHeap::AllocLarge(Length n) {
#endif
  ASSERT(lock_.IsHeld());
  Span *best = NULL;
  Span *best_normal = NULL;

  // Create a Span to use as an upper bound.
  Span bound;
  bound.start = 0;
  bound.length = n;

  // First search the NORMAL spans..
#ifdef BASEBOUNDS_X86
  SpanSet::iterator place = class_free[bb_tag].large_normal_.upper_bound(SpanPtrWithLength(&bound));
  if (place != class_free[bb_tag].large_normal_.end()) {
#else
  SpanSet::iterator place = large_normal_.upper_bound(SpanPtrWithLength(&bound));
  if (place != large_normal_.end()) {
#endif
    best = place->span;
    best_normal = best;
    ASSERT(best->location == Span::ON_NORMAL_FREELIST);
  }

  // Try to find better fit from RETURNED spans.
#ifdef BASEBOUNDS_X86
  place = class_free[bb_tag].large_returned_.upper_bound(SpanPtrWithLength(&bound));
  if (place != class_free[bb_tag].large_returned_.end()) {
#else
  place = large_returned_.upper_bound(SpanPtrWithLength(&bound));
  if (place != large_returned_.end()) {
#endif
    Span *c = place->span;
    ASSERT(c->location == Span::ON_RETURNED_FREELIST);
    if (best_normal == NULL
        || c->length < best->length
        || (c->length == best->length && c->start < best->start))
      best = place->span;
  }

  if (best == best_normal) {
    return best == NULL ? NULL : Carve(best, n);
  }

  // best comes from RETURNED set.
#ifdef BASEBOUNDS_X86
  if (EnsureLimit(n, bb_tag, false)) {
#else
  if (EnsureLimit(n, false)) {
#endif
    return Carve(best, n);
  }

#ifdef BASEBOUNDS_X86
  if (EnsureLimit(n, bb_tag, true)) {
#else
  if (EnsureLimit(n, true)) {
#endif
    // best could have been destroyed by coalescing.
    // best_normal is not a best-fit, and it could be destroyed as well.
    // We retry, the limit is already ensured:
#ifdef BASEBOUNDS_X86
    return AllocLarge(n, bb_tag);
#else
    return AllocLarge(n);
#endif
  }

  // If best_normal existed, EnsureLimit would succeeded:
  ASSERT(best_normal == NULL);
  // We are not allowed to take best from returned list.
  return NULL;
}

Span* PageHeap::Split(Span* span, Length n) {
  ASSERT(lock_.IsHeld());
  ASSERT(0 < n);
  ASSERT(n < span->length);
  ASSERT(span->location == Span::IN_USE);
  ASSERT(span->sizeclass == 0);

  const int extra = span->length - n;
#ifdef BASEBOUNDS_X86
  Span* leftover = NewSpan(span->start + n, extra, span->bb_tag);
#else
  Span* leftover = NewSpan(span->start + n, extra);
#endif
  ASSERT(leftover->location == Span::IN_USE);
  RecordSpan(leftover);
  pagemap_.set(span->start + n - 1, span); // Update map from pageid to span
  span->length = n;

  return leftover;
}

void PageHeap::CommitSpan(Span* span) {
  ++stats_.commit_count;

  TCMalloc_SystemCommit(reinterpret_cast<void*>(span->start << kPageShift),
                        static_cast<size_t>(span->length << kPageShift));
  stats_.committed_bytes += span->length << kPageShift;
  stats_.total_commit_bytes += (span->length << kPageShift);
}

bool PageHeap::DecommitSpan(Span* span) {
  ++stats_.decommit_count;

  bool rv = TCMalloc_SystemRelease(reinterpret_cast<void*>(span->start << kPageShift),
                                   static_cast<size_t>(span->length << kPageShift));
  if (rv) {
    stats_.committed_bytes -= span->length << kPageShift;
    stats_.total_decommit_bytes += (span->length << kPageShift);
  }

  return rv;
}

Span* PageHeap::Carve(Span* span, Length n) {
  ASSERT(n > 0);
  ASSERT(span->location != Span::IN_USE);
  const int old_location = span->location;
  RemoveFromFreeList(span);
  span->location = Span::IN_USE;

  const int extra = span->length - n;
  ASSERT(extra >= 0);
  if (extra > 0) {
#ifdef BASEBOUNDS_X86
    Span* leftover = NewSpan(span->start + n, extra, span->bb_tag);
#else
    Span* leftover = NewSpan(span->start + n, extra);
#endif
    leftover->location = old_location;
    RecordSpan(leftover);

    // The previous span of |leftover| was just splitted -- no need to
    // coalesce them. The next span of |leftover| was not previously coalesced
    // with |span|, i.e. is NULL or has got location other than |old_location|.
#ifndef NDEBUG
    const PageID p = leftover->start;
    const Length len = leftover->length;
    Span* next = GetDescriptor(p+len);
    ASSERT (next == NULL ||
            next->location == Span::IN_USE ||
            next->location != leftover->location);
#endif

    PrependToFreeList(leftover);  // Skip coalescing - no candidates possible
    span->length = n;
    pagemap_.set(span->start + n - 1, span);
  }
  ASSERT(Check());
  if (old_location == Span::ON_RETURNED_FREELIST) {
    // We need to recommit this address space.
    CommitSpan(span);
  }
  ASSERT(span->location == Span::IN_USE);
  ASSERT(span->length == n);
  ASSERT(stats_.unmapped_bytes+ stats_.committed_bytes==stats_.system_bytes);
  return span;
}

void PageHeap::Delete(Span* span) {
  SpinLockHolder h(&lock_);
  DeleteLocked(span);
}

void PageHeap::DeleteLocked(Span* span) {
  ASSERT(lock_.IsHeld());
  ASSERT(Check());
  ASSERT(span->location == Span::IN_USE);
  ASSERT(span->length > 0);
  ASSERT(GetDescriptor(span->start) == span);
  ASSERT(GetDescriptor(span->start + span->length - 1) == span);
#ifdef BASEBOUNDS_X86
  // when cleaning up huge allocations, deal with the guard page
  if(span->meta_guard_type == Span::GUARD_REGULAR){
      // here the guard page was a part of the current span. clean up.
      span->start--;
      span->length++;
      mprotect((void*)span->meta_guard_addr, kPageSize, PROT_READ|PROT_WRITE);
      span->meta_guard_addr = 0;
      span->meta_guard_type = 0;
  }
  else if(span->meta_guard_type == Span::GUARD_HUGE){
      // first, offset the span back to its proper start (incl. prefix page)
      span->start--;
      span->length++;
      // the guard addr lives in its own 1-kpagesize span
      mprotect((void*)span->meta_guard_addr, kPageSize, PROT_READ|PROT_WRITE);
      Span* guard_span = Static::pageheap()->GetDescriptor(span->meta_guard_addr >> kPageShift);
      // delete the span belonging to the guard page (we already have the spin lock)
      DeleteLocked(guard_span);
      // clean up the current span
      span->meta_guard_addr = 0;
      span->meta_guard_type = Span::GUARD_NONE;
  }
#endif
#ifdef BASEBOUNDS_X86_STACK
  if (span->is_stack) {
    span->is_stack = 0;
    span->stack_objsize = 0;
  }
#endif
  const Length n = span->length;
  span->sizeclass = 0;
  span->sample = 0;
  span->location = Span::ON_NORMAL_FREELIST;
  MergeIntoFreeList(span);  // Coalesces if possible
#ifdef BASEBOUNDS_X86
  IncrementalScavenge(n, span->bb_tag);
#else
  IncrementalScavenge(n);
#endif
  ASSERT(stats_.unmapped_bytes+ stats_.committed_bytes==stats_.system_bytes);
  ASSERT(Check());
}

// Given span we're about to free and other span (still on free list),
// checks if 'other' span is mergable with 'span'. If it is, removes
// other span from free list, performs aggressive decommit if
// necessary and returns 'other' span. Otherwise 'other' span cannot
// be merged and is left untouched. In that case NULL is returned.
Span* PageHeap::CheckAndHandlePreMerge(Span* span, Span* other) {
  if (other == NULL) {
    return other;
  }

#ifdef BASEBOUNDS_X86
  // do not allow merging of spans across bb size class ranges
  if(other->bb_tag != span->bb_tag){
    return NULL;
  }
#endif

  // if we're in aggressive decommit mode and span is decommitted,
  // then we try to decommit adjacent span.
  if (aggressive_decommit_ && other->location == Span::ON_NORMAL_FREELIST
      && span->location == Span::ON_RETURNED_FREELIST) {
    bool worked = DecommitSpan(other);
    if (!worked) {
      return NULL;
    }
  } else if (other->location != span->location) {
    return NULL;
  }

  RemoveFromFreeList(other);
  return other;
}

void PageHeap::MergeIntoFreeList(Span* span) {
  ASSERT(lock_.IsHeld());
  ASSERT(span->location != Span::IN_USE);

  // Coalesce -- we guarantee that "p" != 0, so no bounds checking
  // necessary.  We do not bother resetting the stale pagemap
  // entries for the pieces we are merging together because we only
  // care about the pagemap entries for the boundaries.
  //
  // Note: depending on aggressive_decommit_ mode we allow only
  // similar spans to be coalesced.
  //
  // The following applies if aggressive_decommit_ is enabled:
  //
  // TODO(jar): "Always decommit" causes some extra calls to commit when we are
  // called in GrowHeap() during an allocation :-/.  We need to eval the cost of
  // that oscillation, and possibly do something to reduce it.

  // TODO(jar): We need a better strategy for deciding to commit, or decommit,
  // based on memory usage and free heap sizes.

  const PageID p = span->start;
  const Length n = span->length;

  if (aggressive_decommit_ && span->location == Span::ON_NORMAL_FREELIST) {
    if (DecommitSpan(span)) {
      span->location = Span::ON_RETURNED_FREELIST;
    }
  }

  Span* prev = CheckAndHandlePreMerge(span, GetDescriptor(p-1));
  if (prev != NULL) {
    // Merge preceding span into this span
    ASSERT(prev->start + prev->length == p);
    const Length len = prev->length;
    DeleteSpan(prev);
    span->start -= len;
    span->length += len;
    pagemap_.set(span->start, span);
  }
  Span* next = CheckAndHandlePreMerge(span, GetDescriptor(p+n));
  if (next != NULL) {
    // Merge next span into this span
    ASSERT(next->start == p+n);
    const Length len = next->length;
    DeleteSpan(next);
    span->length += len;
    pagemap_.set(span->start + span->length - 1, span);
  }

  PrependToFreeList(span);
}

void PageHeap::PrependToFreeList(Span* span) {
  ASSERT(lock_.IsHeld());
  ASSERT(span->location != Span::IN_USE);
  if (span->location == Span::ON_NORMAL_FREELIST)
    stats_.free_bytes += (span->length << kPageShift);
  else
    stats_.unmapped_bytes += (span->length << kPageShift);

  if (span->length > kMaxPages) {
#ifdef BASEBOUNDS_X86
    SpanSet *set = &class_free[span->bb_tag].large_normal_;
    if (span->location == Span::ON_RETURNED_FREELIST)
      set = &class_free[span->bb_tag].large_returned_;
#else
    SpanSet *set = &large_normal_;
    if (span->location == Span::ON_RETURNED_FREELIST)
      set = &large_returned_;
#endif
    std::pair<SpanSet::iterator, bool> p =
        set->insert(SpanPtrWithLength(span));
    ASSERT(p.second); // We never have duplicates since span->start is unique.
    span->SetSpanSetIterator(p.first);
    return;
  }

#ifdef BASEBOUNDS_X86
  SpanList* list = &class_free[span->bb_tag].free_[span->length - 1];
#else
  SpanList* list = &free_[span->length - 1];
#endif
  if (span->location == Span::ON_NORMAL_FREELIST) {
    DLL_Prepend(&list->normal, span);
  } else {
    DLL_Prepend(&list->returned, span);
  }
}

void PageHeap::RemoveFromFreeList(Span* span) {
  ASSERT(lock_.IsHeld());
  ASSERT(span->location != Span::IN_USE);
  if (span->location == Span::ON_NORMAL_FREELIST) {
    stats_.free_bytes -= (span->length << kPageShift);
  } else {
    stats_.unmapped_bytes -= (span->length << kPageShift);
  }
  if (span->length > kMaxPages) {
#ifdef BASEBOUNDS_X86
    SpanSet *set = &class_free[span->bb_tag].large_normal_;
    if (span->location == Span::ON_RETURNED_FREELIST)
      set = &class_free[span->bb_tag].large_returned_;
#else
    SpanSet *set = &large_normal_;
    if (span->location == Span::ON_RETURNED_FREELIST)
      set = &large_returned_;
#endif
    SpanSet::iterator iter = span->ExtractSpanSetIterator();
    ASSERT(iter->span == span);
    ASSERT(set->find(SpanPtrWithLength(span)) == iter);
    set->erase(iter);
  } else {
    DLL_Remove(span);
  }
}

#ifdef BASEBOUNDS_X86
void PageHeap::IncrementalScavenge(Length n, uint32 bb_tag) {
#else
void PageHeap::IncrementalScavenge(Length n) {
#endif
  ASSERT(lock_.IsHeld());
  // Fast path; not yet time to release memory
  scavenge_counter_ -= n;
  if (scavenge_counter_ >= 0) return;  // Not yet time to scavenge

  const double rate = FLAGS_tcmalloc_release_rate;
  if (rate <= 1e-6) {
    // Tiny release rate means that releasing is disabled.
    scavenge_counter_ = kDefaultReleaseDelay;
    return;
  }

  ++stats_.scavenge_count;

#ifdef BASEBOUNDS_X86
  Length released_pages = ReleaseAtLeastNPages(1, bb_tag);
#else
  Length released_pages = ReleaseAtLeastNPages(1);
#endif

  if (released_pages == 0) {
    // Nothing to scavenge, delay for a while.
    scavenge_counter_ = kDefaultReleaseDelay;
  } else {
    // Compute how long to wait until we return memory.
    // FLAGS_tcmalloc_release_rate==1 means wait for 1000 pages
    // after releasing one page.
    const double mult = 1000.0 / rate;
    double wait = mult * static_cast<double>(released_pages);
    if (wait > kMaxReleaseDelay) {
      // Avoid overflow and bound to reasonable range.
      wait = kMaxReleaseDelay;
    }
    scavenge_counter_ = static_cast<int64_t>(wait);
  }
}

Length PageHeap::ReleaseSpan(Span* s) {
  ASSERT(s->location == Span::ON_NORMAL_FREELIST);

  if (DecommitSpan(s)) {
    RemoveFromFreeList(s);
    const Length n = s->length;
    s->location = Span::ON_RETURNED_FREELIST;
    MergeIntoFreeList(s);  // Coalesces if possible.
    return n;
  }

  return 0;
}

#ifdef BASEBOUNDS_X86
Length PageHeap::ReleaseFromIndex(uint32 bb_tag) {
#else
Length PageHeap::ReleaseFromIndex(){
#endif
  Span *s;
  if (release_index_ > kMaxPages) release_index_ = 0;

  if (release_index_ == kMaxPages) {
#ifdef BASEBOUNDS_X86
    if (class_free[bb_tag].large_normal_.empty()) {
      return 0;
    }
    s = (class_free[bb_tag].large_normal_.begin())->span;
#else
    if (large_normal_.empty()) {
      return 0;
    }
    s = (large_normal_.begin())->span;
#endif
  } else {
#ifdef BASEBOUNDS_X86
    SpanList* slist = &class_free[bb_tag].free_[release_index_];
#else
    SpanList* slist = &free_[release_index_];
#endif
    if (DLL_IsEmpty(&slist->normal)) {
      return 0;
    }
    s = slist->normal.prev;
  }
  // TODO(todd) if the remaining number of pages to release
  // is significantly smaller than s->length, and s is on the
  // large freelist, should we carve s instead of releasing?
  // the whole thing?
  Length released_len = ReleaseSpan(s);
  // Some systems do not support release
  if (released_len == 0) return (Length)-1;
  return released_len;
}

#ifdef BASEBOUNDS_X86
Length PageHeap::ReleaseAtLeastNPages(Length num_pages, uint32 bb_tag) {
#else
Length PageHeap::ReleaseAtLeastNPages(Length num_pages) {
#endif
  ASSERT(lock_.IsHeld());
  Length released_pages = 0;

#ifdef BASEBOUNDS_X86
  // first try to release from the specific bb_tag
  // useful for example when releasing 1 page through DeleteLocked()-->IncrementalScavenge()
  // if the release is sufficient, the while condition released_pages < num_pages will return
  for (int i = 0; i < kMaxPages+1 && released_pages < num_pages; i++, release_index_++) {
      Length released_len = ReleaseFromIndex(bb_tag);
      released_pages += released_len;
  }
#endif

  // Round robin through the lists of free spans, releasing a
  // span from each list.  Stop after releasing at least num_pages
  // or when there is nothing more to release.
  while (released_pages < num_pages && stats_.free_bytes > 0) {
    for (int i = 0; i < kMaxPages+1 && released_pages < num_pages;
         i++, release_index_++) {
      Length released_len = 0;
#ifdef BASEBOUNDS_X86
      // here: release from all the class ranges
      for(uint32 t = 0; t < BB_TAG_SHIFT; t++) {
        released_len += ReleaseFromIndex(t);
      }
#else
      released_len = ReleaseFromIndex();
#endif
      if (released_len == (Length)-1) return released_pages;
      released_pages += released_len;
    }
  }
  return released_pages;
}
#ifdef BASEBOUNDS_X86
bool PageHeap::EnsureLimit(Length n, uint32 bb_tag, bool withRelease) {
#else
bool PageHeap::EnsureLimit(Length n, bool withRelease) {
#endif
  ASSERT(lock_.IsHeld());
  Length limit = (FLAGS_tcmalloc_heap_limit_mb*1024*1024) >> kPageShift;
  if (limit == 0) return true; //there is no limit

  // We do not use stats_.system_bytes because it does not take
  // MetaDataAllocs into account.
  Length takenPages = TCMalloc_SystemTaken >> kPageShift;
  //XXX takenPages may be slightly bigger than limit for two reasons:
  //* MetaDataAllocs ignore the limit (it is not easy to handle
  //  out of memory there)
  //* sys_alloc may round allocation up to huge page size,
  //  although smaller limit was ensured

  ASSERT(takenPages >= stats_.unmapped_bytes >> kPageShift);
  takenPages -= stats_.unmapped_bytes >> kPageShift;

  if (takenPages + n > limit && withRelease) {
#ifdef BASEBOUNDS_X86
    takenPages -= ReleaseAtLeastNPages(takenPages + n - limit, bb_tag);
#else
    takenPages -= ReleaseAtLeastNPages(takenPages + n - limit);
#endif
  }

  return takenPages + n <= limit;
}

void PageHeap::RegisterSizeClass(Span* span, uint32 sc) {
  // Associate span object with all interior pages as well
  ASSERT(span->location == Span::IN_USE);
  ASSERT(GetDescriptor(span->start) == span);
  ASSERT(GetDescriptor(span->start+span->length-1) == span);
  span->sizeclass = sc;
  for (Length i = 1; i < span->length-1; i++) {
    pagemap_.set(span->start+i, span);
  }
}

void PageHeap::GetSmallSpanStatsLocked(SmallSpanStats* result) {
  ASSERT(lock_.IsHeld());
#ifndef BASEBOUNDS_X86
  // for simplicity, just disable these stats under bb-x86
  for (int i = 0; i < kMaxPages; i++) {
    result->normal_length[i] = DLL_Length(&free_[i].normal);
    result->returned_length[i] = DLL_Length(&free_[i].returned);
  }
#endif
}

void PageHeap::GetLargeSpanStatsLocked(LargeSpanStats* result) {
  ASSERT(lock_.IsHeld());
  result->spans = 0;
  result->normal_pages = 0;
  result->returned_pages = 0;
#ifndef BASEBOUNDS_X86
  // for simplicity, just disable these stats under bb-x86
  for (SpanSet::iterator it = large_normal_.begin(); it != large_normal_.end(); ++it) {
    result->normal_pages += it->length;
    result->spans++;
  }
  for (SpanSet::iterator it = large_returned_.begin(); it != large_returned_.end(); ++it) {
    result->returned_pages += it->length;
    result->spans++;
  }
#endif
}

bool PageHeap::GetNextRange(PageID start, base::MallocRange* r) {
  ASSERT(lock_.IsHeld());
  Span* span = reinterpret_cast<Span*>(pagemap_.Next(start));
  if (span == NULL) {
    return false;
  }
  r->address = span->start << kPageShift;
  r->length = span->length << kPageShift;
  r->fraction = 0;
  switch (span->location) {
    case Span::IN_USE:
      r->type = base::MallocRange::INUSE;
      r->fraction = 1;
      if (span->sizeclass > 0) {
        // Only some of the objects in this span may be in use.
        const size_t osize = Static::sizemap()->class_to_size(span->sizeclass);
        r->fraction = (1.0 * osize * span->refcount) / r->length;
      }
      break;
    case Span::ON_NORMAL_FREELIST:
      r->type = base::MallocRange::FREE;
      break;
    case Span::ON_RETURNED_FREELIST:
      r->type = base::MallocRange::UNMAPPED;
      break;
    default:
      r->type = base::MallocRange::UNKNOWN;
      break;
  }
  return true;
}

#ifdef BASEBOUNDS_X86
bool PageHeap::GrowHeap(Length n, LockingContext* context, uint32 bb_tag) {
#else
bool PageHeap::GrowHeap(Length n, LockingContext* context) {
#endif
  ASSERT(lock_.IsHeld());
  ASSERT(kMaxPages >= kMinSystemAlloc);
  if (n > kMaxValidPages) return false;
  Length ask = (n>kMinSystemAlloc) ? n : static_cast<Length>(kMinSystemAlloc);
  size_t actual_size;
  void* ptr = NULL;

#ifdef BASEBOUNDS_X86
  if(bb_tag > 0) {
    // carve the range from the source ptr and return it
    size_t requested_bytes = ask << kPageShift;
    SpanClass *scl = &class_free[bb_tag];
    if(scl->size_remaining >= requested_bytes){
      ptr = (void*)class_free[bb_tag].source;
      // adjust src and size
      scl->source += requested_bytes;
      scl->size_remaining -= requested_bytes;
      actual_size = requested_bytes;
    }
    // class source OOM 
    if (ptr == NULL) return false;
  }
  else
#endif
  {
  #ifdef BASEBOUNDS_X86
    if (EnsureLimit(ask, bb_tag)) {
  #else
    if (EnsureLimit(ask)) {
  #endif
        ptr = TCMalloc_SystemAlloc(ask << kPageShift, &actual_size, kPageSize);
    }
    if (ptr == NULL) {
      if (n < ask) {
        // Try growing just "n" pages
        ask = n;
  #ifdef BASEBOUNDS_X86
        if (EnsureLimit(ask, bb_tag)) {
  #else
        if (EnsureLimit(ask)) {
  #endif
          ptr = TCMalloc_SystemAlloc(ask << kPageShift, &actual_size, kPageSize);
        }
      }
      if (ptr == NULL) return false;
    }
  }
  ask = actual_size >> kPageShift;
  context->grown_by += ask << kPageShift;

  ++stats_.reserve_count;
  ++stats_.commit_count;

  uint64_t old_system_bytes = stats_.system_bytes;
  stats_.system_bytes += (ask << kPageShift);
  stats_.committed_bytes += (ask << kPageShift);

  stats_.total_commit_bytes += (ask << kPageShift);
  stats_.total_reserve_bytes += (ask << kPageShift);

  const PageID p = reinterpret_cast<uintptr_t>(ptr) >> kPageShift;
  ASSERT(p > 0);

  // If we have already a lot of pages allocated, just pre allocate a bunch of
  // memory for the page map. This prevents fragmentation by pagemap metadata
  // when a program keeps allocating and freeing large blocks.

  if (old_system_bytes < kPageMapBigAllocationThreshold
      && stats_.system_bytes >= kPageMapBigAllocationThreshold) {
    pagemap_.PreallocateMoreMemory();
  }

  // Make sure pagemap_ has entries for all of the new pages.
  // Plus ensure one before and one after so coalescing code
  // does not need bounds-checking.
  if (pagemap_.Ensure(p-1, ask+2)) {
    // Pretend the new area is allocated and then Delete() it to cause
    // any necessary coalescing to occur.
#ifdef BASEBOUNDS_X86
    Span* span = NewSpan(p, ask, bb_tag);
#else
    Span* span = NewSpan(p, ask);
#endif
    RecordSpan(span);
    DeleteLocked(span);
    ASSERT(stats_.unmapped_bytes+ stats_.committed_bytes==stats_.system_bytes);
    ASSERT(Check());
    return true;
  } else {
    // We could not allocate memory within "pagemap_"
    // TODO: Once we can return memory to the system, return the new span
    return false;
  }
}

bool PageHeap::Check() {
  ASSERT(lock_.IsHeld());
  return true;
}

bool PageHeap::CheckExpensive() {
  bool result = Check();
#ifndef BASEBOUNDS_X86
  // for simplicity, just disable these stats under bb-x86
  CheckSet(&large_normal_, kMaxPages + 1, Span::ON_NORMAL_FREELIST);
  CheckSet(&large_returned_, kMaxPages + 1, Span::ON_RETURNED_FREELIST);
  for (int s = 1; s <= kMaxPages; s++) {
    CheckList(&free_[s - 1].normal, s, s, Span::ON_NORMAL_FREELIST);
    CheckList(&free_[s - 1].returned, s, s, Span::ON_RETURNED_FREELIST);
  }
#endif
  return result;
}

bool PageHeap::CheckList(Span* list, Length min_pages, Length max_pages,
                         int freelist) {
  for (Span* s = list->next; s != list; s = s->next) {
    CHECK_CONDITION(s->location == freelist);  // NORMAL or RETURNED
    CHECK_CONDITION(s->length >= min_pages);
    CHECK_CONDITION(s->length <= max_pages);
    CHECK_CONDITION(GetDescriptor(s->start) == s);
    CHECK_CONDITION(GetDescriptor(s->start+s->length-1) == s);
  }
  return true;
}

bool PageHeap::CheckSet(SpanSet* spanset, Length min_pages,int freelist) {
  for (SpanSet::iterator it = spanset->begin(); it != spanset->end(); ++it) {
    Span* s = it->span;
    CHECK_CONDITION(s->length == it->length);
    CHECK_CONDITION(s->location == freelist);  // NORMAL or RETURNED
    CHECK_CONDITION(s->length >= min_pages);
    CHECK_CONDITION(GetDescriptor(s->start) == s);
    CHECK_CONDITION(GetDescriptor(s->start+s->length-1) == s);
  }
  return true;
}

#ifdef BASEBOUNDS_X86
#ifdef BASEBOUNDS_X86_STACK
extern "C" void *swiftsan_alloc_stack(size_t size, size_t sizeclass) {
  // Make sure the page heap is initialized.
  if (PREDICT_FALSE(!Static::IsInited())){
    ThreadCache::InitModule();
  }

  // align to size class for finding base addrs
  size_t align = sizeclass;

  // Allocate stack span (with guard page).
  size_t po2 = __builtin_ctzl(sizeclass);
  Span* span = Static::pageheap()->NewAlignedWithSizeClassPrefix(tcmalloc::pages(size), align, 0, po2);
  // fprintf(stderr, "Stacky: span for sz %lu class %lu with guard allocated at %p\n", size, sizeclass, (void*)(span->start << kPageShift));

  ASSERT(span->location == Span::IN_USE);
  span->is_stack = 1;

  // Set nonzero sizeclass, we reserve zero for large heap allocations.
  span->sizeclass = 1;

  // Stack size classes may differ from the dynamically computed tcmalloc
  // sizeclasses so we cannot use the sizeclass ID's used by from
  // Static::sizemap. Instead we store the size class directly.
  span->stack_objsize = static_cast<uint32_t>(sizeclass);

  ASSERT(span->stack_objsize == sizeclass);

  size_t span_len = span->length << kPageShift;
  char *start = reinterpret_cast<char*>(span->start << kPageShift);
  // offset by size class to create a fake object for first obj metadata (redundant?)
  // the guard page of the allocation detects underflows
  char *end = start + (span_len);// - sizeclass;

  //fprintf(stderr, "Stacky: resulting end addr stack %p (align? %lu)\n", (void*)end, (uintptr_t)end % sizeclass);
  return end;
}
#endif

// extern "C" void bb_fill_sc_end_map(uint32 t, void* end){
//   // alignment for internal expectations, like central_freelist expecting 
//   // some minimum number of pages to work with
//   #define ROUND_UP(x) ( (((uintptr_t)(x)) + 0x4000-1)  & (~(0x4000-1)) ) 
//   tc_sc_end_map[t] = (void*)ROUND_UP(end);
//   // fprintf(stderr, "TCMalloc received: %u end %p\n", t, end);
// }
#endif
}  // namespace tcmalloc
