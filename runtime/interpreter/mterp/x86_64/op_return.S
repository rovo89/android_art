/*
 * Return a 32-bit value.
 *
 * for: return, return-object
 */
    /* op vAA */
    .extern MterpThreadFenceForConstructor
    call    SYMBOL(MterpThreadFenceForConstructor)
    testl   $$(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST), THREAD_FLAGS_OFFSET(rSELF)
    jz      1f
    movq    rSELF, OUT_ARG0
    call    SYMBOL(MterpSuspendCheck)
1:
    GET_VREG %eax, rINSTq                   # eax <- vAA
    jmp     MterpReturn
