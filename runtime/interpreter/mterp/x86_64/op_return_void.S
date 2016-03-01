    .extern MterpThreadFenceForConstructor
    call    SYMBOL(MterpThreadFenceForConstructor)
    testl   $$(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST), THREAD_FLAGS_OFFSET(rSELF)
    jz      1f
    movq    rSELF, OUT_ARG0
    call    SYMBOL(MterpSuspendCheck)
1:
    xorq    %rax, %rax
    jmp     MterpReturn
