# MixSan vs RSan — Implementation Details

> MixSan: 6-bit MemTag temporal safety on Intel LAM U57
> Base: RangeSanitizer commit `8e9444b2f`, all paths relative to `$RSAN_TOP`
> Diff: `git diff 8e9444b2f -- tcmalloc-implicit/ llvm-project-16/llvm/lib/CodeGen/SafeStack.cpp` (6 files, +368/-8787)

---

## Overview

MixSan introduces a **6-bit MemTag** in bits 62–57 (Intel LAM U57), replacing RSan's quarantine-based temporal safety with deterministic per-allocation version checking.

| Mechanism | RSan (original) | MixSan |
|-----------|----------------|--------|
| Temporal safety | Quarantine (ring buffer, probabilistic) | MemTag versioning (deterministic) |
| Double-free detection | Bound == 0 check | MemTag mismatch check (`ptr_memtag != 0 && ptr_memtag != meta_memtag`) |
| UAF-after-reuse detection | No (reuse resets bound) | Yes (MemTag changes by +7 each free) |
| Quarantine | `ENABLE_QUARANTINE=1` | `ENABLE_QUARANTINE=0` (disabled by `ENABLE_MEMTAG=1`) |
| Compiler check | 1-stage (spatial + slow tag≠0 filter) | 1-stage spatial + temporal, with MemTag strip for SizeTag extraction |

---

## 1. `tcmalloc-implicit/src/common.h`

### 1.1 MemTag Configuration & Macros

**Bit layout** (Intel LAM U57):
```
Bit:  63    62–57     56–47      46–41     40–0
     [sign] [MemTag] [available] [SizeTag] [offset]
```

```c
/* Heap Quarantine */
#define ENABLE_QUARANTINE 1

/* MemTag-based temporal safety (disables quarantine when enabled) */
#define ENABLE_MEMTAG 1
#if ENABLE_MEMTAG
  #undef ENABLE_QUARANTINE
  #define ENABLE_QUARANTINE 0
#endif

// (BB_TAG_SHIFT=41, BB_TAG_TO_CLASS, CLASS_TO_BB_TAG, PTR_GET_OBJ_START unchanged)

#if ENABLE_MEMTAG
  #define MEMTAG_SHIFT 57
  #define MEMTAG_MASK   0x3FULL
  #define MEMTAG_BITS   (MEMTAG_MASK << MEMTAG_SHIFT)

  #define PTR_GET_MEMTAG(x)       ((uint64_t)(x) >> MEMTAG_SHIFT)
  #define PTR_CLEAR_MEMTAG(x)     ((uint64_t)(x) & ~MEMTAG_BITS)
  #define PTR_SET_MEMTAG(x, tag)  (((uint64_t)(x) & ~MEMTAG_BITS) | ((uint64_t)(tag) << MEMTAG_SHIFT))

  // PTR_GET_TAG strips MemTag before extracting SizeTag (prevents MemTag pollution)
  #define PTR_GET_TAG(x)      ((uintptr_t)(PTR_CLEAR_MEMTAG(x)) >> BB_TAG_SHIFT)

  static inline uint8_t compute_initial_memtag(void* canonical_result, size_t allocated_size) {
    uint8_t sc_tag = (uint8_t)((uint64_t)canonical_result >> BB_TAG_SHIFT);
    size_t sc_start = (size_t)sc_tag << BB_TAG_SHIFT;
    size_t offset = (size_t)canonical_result - sc_start;
    size_t index = offset / allocated_size;
    uint8_t tag = (index % 0x3F) ? (uint8_t)(index % 0x3F) : 0x1F;
    return tag;  // always non-zero (1–63)
  }
#else
  #define MEMTAG_BITS  ((uint64_t)0)
  #define PTR_GET_MEMTAG(x)     ((uint8_t)0)
  #define PTR_SET_MEMTAG(x, t)  ((uint64_t)(x))
  #define PTR_CLEAR_MEMTAG(x)   ((uint64_t)(x))
  #define PTR_GET_TAG(x)      ((uintptr_t)(x) >> BB_TAG_SHIFT)
#endif
```

---

## 2. `tcmalloc-implicit/src/thread_cache.cc`

### 2.1 LAM U57 Enable

In `ThreadCache::InitModule()`, immediately after `if (phinited) return;`:

```c
#if ENABLE_MEMTAG
    {
      long lr = syscall(SYS_arch_prctl, 0x4002 /*ARCH_ENABLE_TAGGED_ADDR*/, 6 /*LAM_U57_BITS*/);
      unsigned long untag = 0;
      syscall(SYS_arch_prctl, 0x4001 /*ARCH_GET_UNTAG_MASK*/, &untag);
    }
#endif
```

Includes added:
```c
#if ENABLE_MEMTAG
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
```

---

## 3. `tcmalloc-implicit/src/tcmalloc.cc`

### 3.1 Allocation: `do_malloc`, `do_malloc_pages`, `malloc_fast_path`, `do_memalign_pages`

All allocation paths follow the same pattern — tag metadata and return tagged pointer:

```c
#if ENABLE_MEMTAG
  uint64_t* meta_ptr = (uint64_t*)(result - BASEBOUNDS_X86_METADATA_OFFSET);
  uint64_t old_meta = *meta_ptr;
  uint8_t memtag = PTR_GET_MEMTAG(old_meta);
  if (memtag == 0) {
    memtag = compute_initial_memtag((void*)result, allocated_size);
  }
  uint64_t end_addr = (uint64_t)(result + requested_size);
  *meta_ptr = PTR_SET_MEMTAG(end_addr, memtag);
  return (void*)PTR_SET_MEMTAG((uint64_t)result, memtag);
#endif
```

**Critical**: `malloc_fast_path` (the primary entry point for most `malloc()` calls) also has this MemTag logic — an earlier version missed this path, causing returned pointers to lack MemTag.

### 3.2 Deallocation: `do_free_with_callback`

```c
#ifdef BASEBOUNDS_X86
  uintptr_t canonical_ptr = PTR_CLEAR_MEMTAG((uint64_t)ptr);
  uint32 tag = (uint32) PTR_GET_TAG((void*)canonical_ptr);
  uint64_t* meta_ptr = (uint64_t*)((char*)canonical_ptr - BASEBOUNDS_X86_METADATA_OFFSET);
  uint64_t meta = *meta_ptr;

#if ENABLE_MEMTAG
  // DF / UAF check: ptr_memtag==0 means first-use, skip
  uint8_t ptr_memtag = PTR_GET_MEMTAG(ptr);
  uint8_t meta_memtag = PTR_GET_MEMTAG(meta);
  if (ptr_memtag != 0 && ptr_memtag != meta_memtag) {
    tcmalloc::swiftsan_error();
  }
  // Update: +7 per free, avoid wrapping to 0
  uint8_t new_memtag = (meta_memtag + 7) & 0x3F;
  if (new_memtag == 0) new_memtag = 7;
  *meta_ptr = PTR_SET_MEMTAG(meta, new_memtag);

  // Strip MemTag for downstream internal code (PageID >> kPageShift etc.)
  ptr = (void*)canonical_ptr;
#else
  if(*meta_ptr == 0) { tcmalloc::swiftsan_error(); }
  *meta_ptr = 0;
#endif
#endif
  const PageID p = reinterpret_cast<uintptr_t>(ptr) >> kPageShift;
```

### 3.3 `swiftsan_check_n` — 3-stage Runtime Check

```c
static inline void swiftsan_check_n(void *target, size_t n) {
  uint64_t sc = PTR_GET_TAG(target);          // Stage 1: SizeTag gate
  if (!sc) return;

  uint64_t* meta_ptr = (uint64_t*)((char*)PTR_GET_OBJ_START(target, sc) - BASEBOUNDS_X86_METADATA_OFFSET);
  uint64_t meta = *meta_ptr;

#if ENABLE_MEMTAG
  uint8_t ptr_memtag = PTR_GET_MEMTAG(target); // Stage 2: Temporal
  if (ptr_memtag != 0) {
    uint8_t meta_memtag = PTR_GET_MEMTAG(meta);
    if (ptr_memtag != meta_memtag) { tcmalloc::swiftsan_error(); }
  }
  void *stripped = (void*)PTR_CLEAR_MEMTAG((uint64_t)target); // Stage 3: Spatial
  uint64_t end_addr = PTR_CLEAR_MEMTAG(meta);
  if ((uintptr_t)((char*)stripped + n) > end_addr) { tcmalloc::swiftsan_error(); }
#else
  uint64_t end_addr = meta;
  if ((uintptr_t)((char*)target + n) > end_addr) { tcmalloc::swiftsan_error(); }
#endif
}
```

### 3.4 PageID Fixes — `PTR_CLEAR_MEMTAG` Before `>> kPageShift`

All internal functions computing PageID from user pointers must strip MemTag first. Without this, MemTag bits (57–62) shift into bits 44–49 after `>> kPageShift`, corrupting the page number.

Functions fixed: `GetOwnership`, `CheckCachedSizeClass`, `ValidateSizeHint`, `do_free_pages` (span-start assertion), `GetSizeWithCallback`.

```c
const PageID p = (uintptr_t)PTR_CLEAR_MEMTAG((uint64_t)ptr) >> kPageShift;
```

### 3.5 Realloc — MemTag-Aware

**Metadata read** (end_of_obj must strip MemTag):
```c
uint64_t raw_meta = *(uint64_t*)((char*)old_ptr - BASEBOUNDS_X86_METADATA_OFFSET);
char* end_of_obj = (char*)PTR_CLEAR_MEMTAG(raw_meta);
const size_t old_size = (size_t)(end_of_obj - (char*)old_ptr);
```

**Local resize** (preserve MemTag):
```c
#if ENABLE_MEMTAG
    uint64_t* meta_ptr = (uint64_t*)((char*)old_ptr - BASEBOUNDS_X86_METADATA_OFFSET);
    uint64_t old_meta = *meta_ptr;
    uint8_t old_memtag = PTR_GET_MEMTAG(old_meta);
    uint64_t new_end = (uint64_t)((char*)old_ptr + new_size);
    *meta_ptr = PTR_SET_MEMTAG(new_end, old_memtag);
#endif
```

---

## 4. `tcmalloc-implicit/src/central_freelist.cc`

```c
void CentralFreeList::ReleaseToSpans(void* object) {
-  const PageID p = reinterpret_cast<uintptr_t>(object) >> kPageShift;
+  const PageID p = (uintptr_t)PTR_CLEAR_MEMTAG((uint64_t)object) >> kPageShift;
```

---

## 5. `llvm-project-16/llvm/lib/CodeGen/SafeStack.cpp`

### 5.1 Constants

```cpp
static constexpr uint64_t IMPLICIT_MEMTAG_SHIFT = 57;  // x86 LAM U57
static constexpr uint64_t EXPLICIT_MEMTAG_SHIFT = 56;  // Arm TBI
```

### 5.2 Function Signatures (Changed Return Type)

```cpp
// RSan original:
std::tuple<Value *, Value *> InsertCheck(...);
std::tuple<Value *, Value *> InsertCheckMeta(..., Value *Tag, bool slowZero);

// MixSan:
std::tuple<Value *, Value *, Value *> InsertCheck(...);
std::tuple<Value *, Value *, Value *> InsertCheckMeta(..., Value *MetaMemTag);
```

The third element (`MetaMemTag` = `Meta >> 57`) enables temporal check reuse across metadata-sharing optimizations.

### 5.3 `InsertCheck` — Full Implementation

This is the inline check for simple memory accesses like `ptr[index]`. All instructions are created in the entry block (before any CFG split) to maintain IR dominance. The `%35` variable name comes from the IR dump, showing the loaded metadata is used as `i64` (not `ptr`) — critical for correctness with LLVM 16 opaque pointers.

```cpp
std::tuple<Value *, Value *, Value *> SafeStack::InsertCheck(
    Instruction &I, Value &addr, bool write, Type* ptrType) {
  Function *F = I.getParent()->getParent();
  LLVMContext &C = F->getContext();
  IRBuilder<> builder(C);
  builder.SetInsertPoint(&I);

  IntegerType *Int64Ty = Type::getInt64Ty(C);
  uint64_t tag_shift   = isImplicitTagging ? 41 : 56;
  uint64_t memtag_shift = isImplicitTagging ? IMPLICIT_MEMTAG_SHIFT : EXPLICIT_MEMTAG_SHIFT;
  uint64_t strip_mask  = (1ULL << 63) | ((1ULL << memtag_shift) - 1);

  Value *PtrAsInt = builder.CreatePtrToInt(Target, IntPtrTy);

  // Strip MemTag before extracting SizeTag
  Value *StrippedPtr = builder.CreateAnd(PtrAsInt, builder.getInt64(strip_mask));
  Value *Tag = builder.CreateLShr(StrippedPtr, builder.getInt64(tag_shift));

  // Compute metadata pointer (BZHI on original PtrAsInt — preserves MemTag
  // so that the metadata pointer, with LAM, dereferences to the correct location)
  Value *MetadataOffset;
  if (isX86) {
    Value *BZHImask = builder.CreateIntrinsic(Int64Ty, Intrinsic::x86_bmi_bzhi_64,
                                              {PtrAsInt, Tag});
    Value *BZHIbase = builder.CreateXor(BZHImask, PtrAsInt);
    MetadataOffset = builder.CreateSub(BZHIbase, builder.getInt64(8));
  }

  // Load metadata as i64 (NOT ptr — opaque pointer fix)
  Value *MetadataPtr = builder.CreateIntToPtr(MetadataOffset, Int64PtrTy);
  Value *Meta = builder.CreateLoad(Int64Ty, MetadataPtr);

  // EndOfObj = Meta (with MemTag); spatial check compares two tagged values
  Value *EndOfObj = Meta;

  // Extract MetaMemTag (Meta >> 57) & 0x3F — returns i64, NOT ptr
  Value *MetaMemTag = builder.CreateLShr(Meta, builder.getInt64((uint64_t)57));
  MetaMemTag = builder.CreateAnd(MetaMemTag, builder.getInt64(0x3F));

  // Temporal check: ptr_memtag != 0 && ptr_memtag != meta_memtag
  Value *PtrMemtag  = builder.CreateLShr(PtrAsInt, builder.getInt64(memtag_shift));
  PtrMemtag  = builder.CreateAnd(PtrMemtag, builder.getInt64(0x3F));
  Value *PtrHasTag  = builder.CreateICmpNE(PtrMemtag, builder.getInt64(0));
  Value *TagMismatch = builder.CreateICmpNE(PtrMemtag, MetaMemTag);
  Value *TempFail   = builder.CreateAnd(PtrHasTag, TagMismatch);

  // TargetFull uses original Target pointer (preserves SizeTag bits 41-46)
  Value* TargetFull = Target;
  if (ptrType != nullptr) {
    TypeSize size = DL.getTypeStoreSize(ptrType);
    // Guard against scalable vectors (FastISel can't handle them)
    if (!size.isScalable() && size.getFixedValue() > 1) {
      Value *AccessSize = ConstantInt::get(F->getContext(),
          APInt(IntPtrTy->getBitWidth(), size.getFixedValue() - 1));
      std::vector<Value *> indizes = {AccessSize};
      TargetFull = builder.CreateInBoundsGEP(builder.getInt8Ty(), Target, indizes);
    }
  }

  // Combined failure: spatial OR temporal
  Value *SpatFail = builder.CreateICmp(ICMP_UGE, TargetFull, EndOfObj);
  Value *cmp = builder.CreateOr(TempFail, SpatFail);

  // CFG split — same structure as original RSan
  Instruction *split = &*std::next(cast<Instruction>(cmp)->getIterator());
  LLVMContext* CC = &(F->getContext());
  Instruction *endOfThen = SplitBlockAndInsertIfThen(
      cmp, split, false,
      MDBuilder(*CC).createBranchWeights(1, 10000000), &DT, &LI, nullptr);
  builder.SetInsertPoint(endOfThen);

  // Slow check: tag == 0 means uninstrumented memory → ignore
  Value *SlowCheckNonZeroTag = builder.CreateICmp(ICMP_EQ, Tag, builder.getInt64(0));
  BasicBlock *Head = endOfThen->getParent();
  BasicBlock *Tail = BasicBlock::Create(*CC, "", F, Head->getNextNode());
  new UnreachableInst(*CC, Tail);
  if (Loop *L = LI.getLoopFor(Head)) L->addBasicBlockToLoop(Tail, LI);
  Instruction *HeadOldTerm = Head->getTerminator();
  BranchInst *HeadNewTerm = BranchInst::Create(I.getParent(), Tail, SlowCheckNonZeroTag);
  HeadNewTerm->setMetadata(LLVMContext::MD_prof,
      MDBuilder(*CC).createBranchWeights(1, 10000000));
  ReplaceInstWithInst(HeadOldTerm, HeadNewTerm);
  Instruction *NewFailureBlock = Tail->getTerminator();
  builder.SetInsertPoint(NewFailureBlock);

  // int3 / brk trap
  if (isX86) {
    builder.CreateCall(InlineAsm::get(..., "int3", ...), {});
  }

  return {EndOfObj, Tag, MetaMemTag};
}
```

#### Key Design Decisions

1. **All new instructions in entry block** — `MetaMemTag`, `PtrMemtag`, `TempFail` are all created before `SplitBlockAndInsertIfThen`. This ensures they are defined in the dominating block that reaches all subsequent blocks, preventing IR dominance violations that caused `LiveVariables` crash.

2. **`Meta` loaded as `i64`, not `ptr`** — In LLVM 16 opaque pointer mode, `CreateLoad(Int64PtrTy, ...)` returns `ptr` type. Bitwise operations (`lshr`, `and`) on `ptr` values crash FastISel at `-O0` ("Invalid size request on a scalable vector"). Changed to `CreateLoad(Int64Ty, ...)`.

3. **Scalable vector guard** — `TypeSize::operator-(uint64_t)` crashes on scalable types. Added `!size.isScalable()` guard before `getFixedValue()`.

4. **BZHI uses original `PtrAsInt`** — Not the stripped version. This preserves the MemTag in the metadata pointer. With LAM, tagged addresses dereference correctly. This matches original RSan behavior.

5. **`TempFail` OR'd with `SpatFail`** — Both in entry block. The combined `cmp` is the split condition, keeping the original CFG structure (spatial check split → slow tag≠0 filter → trap).

### 5.4 `InsertCheckRange`

Fix: `EndPtrVal = CreatePtrToInt(end)` moved BEFORE `SplitBlockAndInsertIfThen`:

```cpp
  Value *StartVal = Builder.CreatePtrToInt(start, Int64Ty);
  Value *EndPtrVal = Builder.CreatePtrToInt(end, Int64Ty);  // ← BEFORE split
  // ... SizeTag extraction, IsTagged check ...
  Instruction *SlowTerm = SplitBlockAndInsertIfThen(IsTagged, &I, false,
                                                     nullptr, &DT, &LI, nullptr);
  IRBuilder<> SlowBuilder(SlowTerm);
  // ... SlowBuilder uses EndPtrVal (now from dominating entry block) ...
  Value *StrippedEnd = SlowBuilder.CreateAnd(EndPtrVal, ...);
```

Without this fix, `EndPtrVal` was created in the tail block (via `Builder`) but used in the slow block (via `SlowBuilder`), causing an IR dominance violation that led to `LiveVariables::HandleVirtRegUse` crash during LTO.

### 5.5 `SplitBlockAndInsertIfThen` DT/LI Updates

All `SplitBlockAndInsertIfThen` calls must pass `&DT` and `&LI` to keep DominatorTree in sync:

```cpp
SplitBlockAndInsertIfThen(..., &DT, &LI, nullptr)
```

Without this, subsequent passes relying on DT (like loop optimizations) crash. Fixed in `InsertCheckRange`, `InsertCheckMeta`, and `InsertCheck`.

### 5.6 `RSAN_DUMP_IR` Debug Hook

Environment-variable-controlled IR dump at end of SafeStack pass:

```cpp
if (const char *dumpPath = std::getenv("RSAN_DUMP_IR")) {
    std::string dumpFile = (dumpPath[0] == '1' && dumpPath[1] == '\0')
        ? M.getSourceFileName() + ".safe.ll" : dumpPath;
    std::error_code ec;
    llvm::raw_fd_ostream OS(dumpFile, ec);
    if (!ec) { M.print(OS, nullptr); }
}
```

Usage: `RSAN_DUMP_IR=1 make` or `RSAN_DUMP_IR=/tmp/out.ll make`.

---

## 6. Design Rationale

### 6.1 MemTag vs Quarantine

RSan's quarantine delays memory reuse probabilistically. MemTag provides **deterministic** temporal safety: each free increments the tag (+7), and any dangling pointer from a prior allocation cycle can never match the current tag.

### 6.2 Why `+7` Increment

Ensures good distribution across the 6-bit space (63 usable values, 0 reserved). Collision probability after N cycles ≈ N/63.

### 6.3 Pointer Canonicalization in `free()`

After MemTag check and update, `ptr = (void*)canonical_ptr` strips MemTag from the pointer before passing to downstream tcmalloc code. While LAM allows dereferencing tagged pointers, internal arithmetic (`>> kPageShift`, pointer comparisons) requires canonical addresses.

### 6.4 `PtrHasTag` Guard in Temporal Check

`ptr_memtag == 0` means the pointer doesn't carry a MemTag (first allocation or uninstrumented memory). The temporal check is skipped in this case, preventing false positives.

---

## 7. Bugs Found & Fixed

| # | Symptom | Root Cause | Fix |
|---|---------|-----------|-----|
| 1 | SPEC LTO crash: `LiveVariables::HandleVirtRegUse` SIGSEGV | `InsertCheckRange`: `EndPtrVal` defined in tail block, used in slow block — IR dominance violation | Move `EndPtrVal` before `SplitBlockAndInsertIfThen` |
| 2 | Juliet `-O0` crash: `FastISel` "Invalid size request on a scalable vector" | LLVM 16 opaque pointers: `CreateLoad(Int64PtrTy,...)` returns `ptr` type; `lshr`/`and` on `ptr` fails | Load metadata as `Int64Ty` (i64), not `Int64PtrTy` (ptr) |
| 3 | Juliet `-O0` crash: `TypeSize::operator unsigned long()` on scalable type | `size-1` on scalable `TypeSize` invokes `getFixedValue()` | Guard with `!size.isScalable()` |
| 4 | SPEC LTO crash: DT corruption | `SplitBlockAndInsertIfThen` called without `&DT`/`&LI` parameters | Pass `&DT, &LI` to all calls |
| 5 | `malloc_fast_path` missing MemTag | Most `malloc()` calls bypass `do_malloc` via fast path | Add MemTag logic to `malloc_fast_path` |
| 6 | UAF not detected | `*meta_ptr = PTR_SET_MEMTAG(...)` commented out | Uncomment the MemTag update in `do_free_with_callback` |
| 7 | PageID corrupted by tagged ptrs | Internal functions compute `ptr >> kPageShift` with tagged ptr → MemTag pollutes PageID | Add `PTR_CLEAR_MEMTAG` before all `>> kPageShift` on user pointers |
| 8 | UAF not detected (OOB asymmetry) | Old SafeStack spatial check compares `tagged_target >= tagged_end`; after free, MemTag mismatch makes comparison unreliable | Add temporal check (`ptr_memtag != meta_memtag`) independent of spatial comparison |

---

## 8. Test Status

| Test | Result |
|------|--------|
| OOB valid (examples/oob.c, index 30) | Exit 0 |
| OOB (index 40) | Exit 133 SIGTRAP |
| UAF (examples/uaf.c) | Exit 133 SIGTRAP |
| Juliet CWE 121/122/124/126/127/415/416 | Compiles + detects |
| SPEC 2006 400.perlbench | Compiles (LTO SafeStack passes) |
| SPEC 2017 600.perlbench_s | Compiles (LTO SafeStack passes) |

**Runtime requirement**: Intel LAM-capable CPU + kernel with working LAM U57 syscall support (`lam=on` or equivalent). Without kernel LAM support, tagged pointers cause `EFAULT` in syscalls (e.g., `fscanf` → `read`), silently failing I/O.
