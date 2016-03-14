/*
 * Unconditional branch, 16-bit offset.
 *
 * The branch distance is a signed code-unit offset, which we need to
 * double to get a byte offset.
 */
    /* goto/16 +AAAA */
    movswq  2(rPC), rINSTq                  # rINSTq <- ssssAAAA
    MTERP_PROFILE_BRANCH
    addq    rINSTq, rINSTq                  # rINSTq <- AA * 2
    leaq    (rPC, rINSTq), rPC
    FETCH_INST
    jle     MterpCheckSuspendAndContinue    # AA * 2 <= 0 => suspend check
    GOTO_NEXT
