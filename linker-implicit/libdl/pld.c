/* Syscall support. */
int _pld_errno;
#define SYS_ERRNO _pld_errno
#include "linux_syscall_support.h"

/* Printf support. */
#include "printf.h"
void _putchar(char character)
{
    sys_write(2, &character, 1);
}

/* Assert support. */
void __assert_fail(const char * assertion, const char * file, unsigned int line, const char * function)
{
    printf("Assertion \"%s\" failed in %s() (%s:%d)\n", assertion, function, file, line);
    sys_raise(SIGABRT);
    while(1);
}

/* Headers. */
#include <sys/mman.h>
#include <fcntl.h>
#include <assert.h>
#include <elf.h>

#include "pld.h"

#ifndef DEBUG
#define DEBUG 0
#endif

llong_type strtoll_no_libc(const char *nptr, char **endptr, register int base)
{
	register const char *s = nptr;
	register ullong_type acc;
	register int c;
	register ullong_type cutoff;
	register int neg = 0, any, cutlim;

	/*
	 * Skip white space and pick up leading +/- sign if any.
	 * If base is 0, allow 0x for hex and 0 for octal, else
	 * assume decimal; if base is already 16, allow 0x.
	 */
	do {
		c = *s++;
	} while (ISSPACE(c));
	if (c == '-') {
		neg = 1;
		c = *s++;
	} else if (c == '+')
		c = *s++;
	if ((base == 0 || base == 16) &&
	    c == '0' && (*s == 'x' || *s == 'X')) {
		c = s[1];
		s += 2;
		base = 16;
	}
	if (base == 0)
		base = c == '0' ? 8 : 10;

	/*
	 * Compute the cutoff value between legal numbers and illegal
	 * numbers.  That is the largest legal value, divided by the
	 * base.  An input number that is greater than this value, if
	 * followed by a legal input character, is too big.  One that
	 * is equal to this value may be valid or not; the limit
	 * between valid and invalid numbers is then based on the last
	 * digit.  For instance, if the range for longs is
	 * [-2147483648..2147483647] and the input base is 10,
	 * cutoff will be set to 214748364 and cutlim to either
	 * 7 (neg==0) or 8 (neg==1), meaning that if we have accumulated
	 * a value > 214748364, or equal but the next digit is > 7 (or 8),
	 * the number is too big, and we will return a range error.
	 *
	 * Set any if any `digits' consumed; make it negative to indicate
	 * overflow.
	 */
	cutoff = neg ? -(ullong_type)LLONG_MIN : LLONG_MAX;
	cutlim = cutoff % (ullong_type)base;
	cutoff /= (ullong_type)base;
	for (acc = 0, any = 0;; c = *s++) {
		if (ISDIGIT(c))
			c -= '0';
		else if (ISALPHA(c))
			c -= ISUPPER(c) ? 'A' - 10 : 'a' - 10;
		else
			break;
		if (c >= base)
			break;
		if (any < 0 || acc > cutoff || (acc == cutoff && c > cutlim))
			any = -1;
		else {
			any = 1;
			acc *= base;
			acc += c;
		}
	}
	if (any < 0) {
		acc = neg ? LLONG_MIN : LLONG_MAX;
	} else if (neg)
		acc = -acc;
	if (endptr != 0)
		*endptr = (char *) (any ? s - 1 : nptr);
	return (acc);
}

#define LOC_START 1
#define LOC_END 2
#define LOC_REM 3
int get_proc_maps_no_libc(struct mapinfo *maps, int maps_size)
{
    char buf[4096];
    int fd;
    ssize_t bytes;
    int i;
    int num_maps = 0;
    int loc = LOC_START;
    char addrbuf[32];
    int addrbuf_pos = 0;

    fd = sys_open("/proc/self/maps", O_RDONLY, S_IRUSR);
    if (fd == -1) {
        printf("Could not open /proc/self/maps!\n");
    }

    /* The easiest solution would be to use strtol directly (and use its endptr)
     * twice, followed by a strchr for the \n. However, we are dealing with
     * chunks complicating that scheme a bit (as an addr might be split over
     * chunks). */
    while ((bytes = sys_read(fd, buf, sizeof(buf))) > 0) {
        for (i = 0; i < bytes; i++) {
            if (loc == LOC_START || loc == LOC_END) {
                if (buf[i] == '-') {
                    if (num_maps == maps_size) {
                        printf("Error: more than %d maps\n", maps_size);
                        return num_maps;
                    }
                    addrbuf[addrbuf_pos] = '\0';
                    maps[num_maps].start = strtoll_no_libc(addrbuf, NULL, 16);
                    addrbuf_pos = 0;
                    loc = LOC_END;
                } else if (buf[i] == ' ') {
                    addrbuf[addrbuf_pos] = '\0';
                    maps[num_maps].end = strtoll_no_libc(addrbuf, NULL, 16);
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
    sys_close(fd);
    return num_maps;
}
#undef LOC_START
#undef LOC_END
#undef LOC_REM

void dump_maps(struct mapinfo *maps, int num_maps)
{
    int i;
    printf("-MAPPINGS- (%d total)\n", num_maps);
    for (i = 0; i < num_maps; i++)
        printf(" 0x%lx - 0x%lx\n", maps[i].start, maps[i].end);
}

char* read_maps(char *buffer, size_t size)
{
    static int fd;
    static ssize_t count;
    fd = sys_open("/proc/self/maps", O_RDONLY, 0);
    count = sys_read(fd, buffer, size);
    assert(count > 0);
    buffer[count] = '\0';
    sys_close(fd);
}

void print_maps()
{
    static char buffer[8192];
    read_maps(buffer, sizeof(buffer));
    printf("*** Dumping maps:\n%s", buffer);
}

/* Moving the stack into low address range */
uintptr_t move_stack(uintptr_t og_rsp) {

    // here RSP is inflated: it contains the return address to _pld_start, and the saved %rdx from _pld_start (dl_fini)
    // but og_rsp points to argc

    size_t og_stack_size = DEFAULT_STACK_SIZE;

    struct kernel_rlimit stacklimit;
    if(sys_getrlimit(RLIMIT_STACK, &stacklimit) == 0){
        if (stacklimit.rlim_cur != RLIM_INFINITY)
            og_stack_size = stacklimit.rlim_cur;
    }

    // map the stack halfway through the available space
    char *target_stack_addr = (char*)(STACK_END) - og_stack_size;
    char* new_stack = sys_mmap(target_stack_addr, og_stack_size, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK | MAP_FIXED |
            MAP_GROWSDOWN, 0, 0);

    uintptr_t new_stack_top = (uintptr_t)new_stack + og_stack_size;

    /*
    * ------------------------------
    *  [ 0 ]  <-- top of the stack
    *  [ envp strings ]
    *  [ argv strings ] <-- ubp_av
    *  [ 0 ]
    *  [ auxv ]
    *  [ 0 ]
    *  [ envp ]
    *  [ 0 ]
    *  [ argv ]
    *  [ argc ] <-- stack pointer
    * -----------------------------
    *
    * First we will try to locate the top of the stack (starting from argc),
    * then we fix the argv/envp pointers, and finally copy over all data on the
    * stack to the new mapping.
    */
   
    char *last_string = 0; // find the last string in argv/envp
    char* stack_top_tmp = (char*)og_rsp; // points to argc
    stack_top_tmp += 8; // now points to argv
    char* argvps = stack_top_tmp;

    // we read the argv/envp pointers and find the 'lowest' pointer
    // which is the one closest to the top of the stack
    while(*(size_t*)(stack_top_tmp) != 0){
        if (last_string < (char*)(*(size_t*)(stack_top_tmp))) {
            last_string = (char*)(*(size_t*)(stack_top_tmp));
        }
        stack_top_tmp += 8; // point to next argv pointer
    }
    // now points to the NULL after argv
    stack_top_tmp += 8; // points to envp
    char* envps = stack_top_tmp;
    
    while(*(size_t*)(stack_top_tmp) != 0){
        if (last_string < (char*)(*(size_t*)(stack_top_tmp))) {
            last_string = (char*)(*(size_t*)(stack_top_tmp));
        }
        stack_top_tmp += 8; // point to next envp pointer
    }

    /* Go to end of last string.*/
    while (*last_string++ != 0);
    /* There seems to be another string on the stack: filename? */
    while (*last_string++ != 0);

    uintptr_t top_of_stack = (uintptr_t)last_string;
    top_of_stack = (top_of_stack + 0x3) & ~(0x3);

    /* Check for top stack marker */
    if (*(uintptr_t *)top_of_stack != 0) {
        // something is wrong, force error without libc calls
        asm volatile("int3");
    }

    /* Distance between the old and new stack, to modify any ptr to the stack
    * easily with a simply subtraction. */
    uintptr_t stack_offset = top_of_stack - new_stack_top + sizeof(uintptr_t);

    /* Fix the pointers to the new argv and env values */
    char* cur = argvps;
    while(*(size_t*)(cur) != 0){
        *(size_t*)(cur) -= stack_offset;
        cur += 8; // next argv pointer
    }
    cur = envps;

    while(*(size_t*)(cur) != 0){
        *(size_t*)(cur) -= stack_offset;
        cur += 8; // next env pointer
    }

    /* Copy over all stack contents */
    uintptr_t from = (uintptr_t)og_rsp - 8; // argc - 8 includes the previously saved rdx
    size_t stack_size = top_of_stack - from;

    uintptr_t *newstack = (void *)((uintptr_t)new_stack_top - stack_size);
    // // ensure 16-byte alignment _after_ we pop RDX
    if((uintptr_t)newstack-8 % 16 != 0){
        newstack--;
    }

    // memcpy
    for(size_t i = 0; i < stack_size; i++){
        ((uint8_t*)newstack)[i] = ((uint8_t*)from)[i];
    }

    // if we update RSP here now, then the function epilogue 
    // can still change RSP, instead return it and let _my_start override RSP
    // so we cannot do: asm volatile ("movq %%rax, %%rsp"::"a"(newstack));
    return (uintptr_t)newstack;
}

/*
 * Walks through the address space mappings and fills any hole above our reduced
 * address space with an empty mmap.
 * This forces future mmap() calls to allocate in the shrunk address space.
 */
void fill_high_holes(uintptr_t oldstackptr)
{
    struct mapinfo maps[256];
    size_t num_maps;
    size_t i;

    struct mapinfo existing_maps[256];
    num_maps = get_proc_maps_no_libc(existing_maps, 256);
    // dump_maps(existing_maps, num_maps);

#if 0
    // unmap the old stack: but not before we return here...
    for (int i = 0; i < num_maps; i++){
        if (existing_maps[i].start <= oldstackptr && oldstackptr < existing_maps[i].end){
            // printf("Found old stack mapping: %016lx-%016lx\n", existing_maps[i].start, existing_maps[i].end);
            // PROT_NONE
            sys_mmap((void*)maps[i].start, maps[i].end - maps[i].start, PROT_NONE, MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
            break;
        }
    }
#endif

    // first we have to allocate the regions for size classes in bits 41-47,
    // such that we do not conflict with the mmap plugging now and tcmalloc later
    // allocate the virtual address range for implicit tags
    // skip freelist 0 for uninstrumented memory
    for (int t = 1; t < BB_TAG_SHIFT; t++) {
        // if start addr is one of the existing maps, globals live there
        // take the end addr of the next entry as starting point
        // otherwise map the whole area: no globals will be loaded there    

        // effectively: start = (t << BB_TAG_SHIFT) + size_of_globals_class
        void* start = (void*)(((uintptr_t)t << BB_TAG_SHIFT));

        for(size_t m = 0; m < num_maps; m++){
            if(existing_maps[m].start == (uintptr_t)start){
                start = (void*)(existing_maps[m].end); // start at the end of this mapping
                if(m+1 < num_maps){
                    // check if there is a 'const' section directly afterwards
                    void* next = (void*)(existing_maps[m+1].start);
                    size_t tag1 = PTR_GET_TAG(start);
                    size_t tag2 = PTR_GET_TAG(next);
                    if(tag1 == tag2){
                        // we are looking at 'mut' and 'const' pointers
                        start = (void*)(existing_maps[m+1].end); // start at the end of 'const'
                    }
                }
                break;
            }
        }

        size_t size_for_class = ((uintptr_t)(t+1) << BB_TAG_SHIFT) - (uintptr_t)start;
        // printf("Mapping class %d: start = %p size = %llx\n", t, start, size_for_class);

        char* map = (char*)sys_mmap(start, size_for_class, PROT_READ|PROT_WRITE, MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0);
        if(map == (void*)-1) {
            printf("Fatal: could not map at start %p\n", start);
            return;
        }
    }

    uintptr_t prev_end = UNTAGGED_END;
    num_maps = get_proc_maps_no_libc(maps, 256);
    // dump_maps(maps, num_maps);

    for (i = 0; i < num_maps; i++){
        if (maps[i].start >= UNTAGGED_END &&
            maps[i].start < KERN_ADDRSPACE) {

            size_t sz = maps[i].start - prev_end;
            if (prev_end && sz) {
                void *tmp;

                // printf("Found hole at %lx-%lx of size %zx\n", prev_end, maps[i].start, sz);
                tmp = sys_mmap((void*)prev_end, sz, PROT_NONE,
                        MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

            }
            prev_end = maps[i].end;
        }
    }
}

uintptr_t _pld_init(char* rsp)
{
    static char *p, *saddr, *eaddr;
    static off_t off;
    static char **rp;
    static Elf64_auxv_t *ap;
    static int i, ld_fd, flags, prot, is_file;

#if DEBUG
    /* Print maps to show address space layout. */
    // print_maps();
#endif

    // save a stack pointer to the old stack range to later unmap it
    size_t oldstackptr;
    asm volatile ("mov %%rsp, %0\n\t" : "=r"(oldstackptr));

    // TODO: move [vdso] -> update elf auxv
    rsp = (char*)move_stack((uintptr_t)rsp);

    // plug address space holes
    fill_high_holes(oldstackptr);

    /* Map LD VMAs. */
    #define R PROT_READ
    #define RW PROT_READ|PROT_WRITE
    #define RE PROT_READ|PROT_EXEC
    #define F MAP_PRIVATE
    #define A MAP_PRIVATE|MAP_ANONYMOUS
    static unsigned long ld_maps[] = { LD_MAPS, 0 };
    #undef R
    #undef RW
    #undef RE
    #undef F
    #undef A

#if DEBUG
    assert(sizeof(ld_maps)/sizeof(ld_maps[0]) % 5 == 1);
#endif

    ld_fd = sys_open(LD_SO, O_RDONLY, 0);
#if DEBUG
    assert(ld_fd > 0);
#endif
    for (i=0; ld_maps[i]; i+= 5) {
        saddr = (char*) ld_maps[i];
        eaddr = (char*) ld_maps[i+1];
        off = (off_t) ld_maps[i+2];
        prot = (int) ld_maps[i+3];
        flags = (int) ld_maps[i+4];
        p = sys_mmap(saddr, eaddr-saddr, prot, flags|MAP_FIXED_NOREPLACE, ld_fd, off);

        if(p != saddr){
            printf("p=%p, saddr=%p, should be equal. could not map in this area?\n", p, saddr);
            // the target program appears to be mapping around 
        }

#if DEBUG
        assert(p == saddr);
#endif
    }
    sys_close(ld_fd);

    /* Fix up auxv to point to libdl base address. */
    rp = (char**) rsp;
    rp++; // argc
    while (*rp++); //argv
    while (*rp++); //envp
    for (ap = (Elf64_auxv_t*)rp; ap->a_type != AT_NULL; ap++) {
        if (ap->a_type == AT_BASE) {
#if DEBUG
            // printf("*** AT_BASE: 0x%p --> 0x%p\n", ap->a_un.a_val, ld_maps[0]);
#endif
            ap->a_un.a_val = ld_maps[0];
            break;
        }
    }

    return (uintptr_t)rsp;
}
