/*
 * Unconditional branch, 16-bit offset.
 *
 * The branch distance is a signed code-unit offset, which we need to
 * double to get a byte offset.
 */
    /* goto/16 +AAAA */
    movswq  2(rPC), %rax                    # rax <- ssssAAAA
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
