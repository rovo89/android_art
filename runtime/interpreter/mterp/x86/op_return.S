/*
 * Return a 32-bit value.
 *
 * for: return, return-object
 */
    /* op vAA */
    .extern MterpThreadFenceForConstructor
    call    MterpThreadFenceForConstructor
    movl    rSELF, %eax
    testl   $$(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST), THREAD_FLAGS_OFFSET(%eax)
    jz      1f
    movl    %eax, OUT_ARG0(%esp)
    call    MterpSuspendCheck
1:
    GET_VREG %eax rINST                     # eax <- vAA
    xorl    %ecx, %ecx
    jmp     MterpReturn
