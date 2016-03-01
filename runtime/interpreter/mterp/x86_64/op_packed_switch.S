%default { "func":"MterpDoPackedSwitch" }
/*
 * Handle a packed-switch or sparse-switch instruction.  In both cases
 * we decode it and hand it off to a helper function.
 *
 * We don't really expect backward branches in a switch statement, but
 * they're perfectly legal, so we check for them here.
 *
 * for: packed-switch, sparse-switch
 */
    /* op vAA, +BBBB */
    movslq  2(rPC), OUT_ARG0                # rcx <- BBBBbbbb
    leaq    (rPC,OUT_ARG0,2), OUT_ARG0      # rcx <- PC + BBBBbbbb*2
    GET_VREG OUT_32_ARG1, rINSTq            # eax <- vAA
    call    SYMBOL($func)
    addl    %eax, %eax
    movslq  %eax, %rax
    leaq    (rPC, %rax), rPC
    FETCH_INST
    jg      1f
#if MTERP_SUSPEND
    REFRESH_IBASE
#else
    jmp     MterpCheckSuspendAndContinue
#endif
1:
    GOTO_NEXT
