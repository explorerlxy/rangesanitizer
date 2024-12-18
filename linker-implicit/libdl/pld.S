.globl  _pld_start
.globl  _pld_init

_pld_start:
    mov %rsp, %rdi
    # save dl_fini for exit routines
    push %rdx
    call _pld_init
    # override rsp with the result
    movq %rax, %rsp
    # restore dl_fini
    pop %rdx
    # jump to ld entry point
    mov $LD_ENTRY, %rax
    jmp *%rax
