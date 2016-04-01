/*
 * Return a 64-bit value.
 */
    /* return-wide vAA */
    .extern MterpThreadFenceForConstructor
    call    SYMBOL(MterpThreadFenceForConstructor)
    testl   $$(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST), THREAD_FLAGS_OFFSET(rSELF)
    jz      1f
    movq    rSELF, OUT_ARG0
    call    SYMBOL(MterpSuspendCheck)
1:
    GET_WIDE_VREG %rax, rINSTq              # eax <- v[AA]
    jmp     MterpReturn
