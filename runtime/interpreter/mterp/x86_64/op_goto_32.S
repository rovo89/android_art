/*
 * Unconditional branch, 32-bit offset.
 *
 * The branch distance is a signed code-unit offset, which we need to
 * double to get a byte offset.
 *
 *  Because we need the SF bit set, we'll use an adds
 * to convert from Dalvik offset to byte offset.
 */
    /* goto/32 +AAAAAAAA */
    movslq  2(rPC), %rax                    # rax <- AAAAAAAA
    addq    %rax, %rax                      # rax <- AA * 2
    leaq    (rPC, %rax), rPC
    FETCH_INST
    jg      1f                              # AA * 2 > 0 => no suspend check
#if MTERP_SUSPEND
    REFRESH_IBASE
#else
    jmp     MterpCheckSuspendAndContinue
#endif
1:
    GOTO_NEXT
