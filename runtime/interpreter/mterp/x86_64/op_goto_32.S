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
    movslq  2(rPC), rINSTq                  # rINSTq <- AAAAAAAA
    MTERP_PROFILE_BRANCH
    addq    rINSTq, rINSTq                  # rINSTq <- AA * 2
    leaq    (rPC, rINSTq), rPC
    FETCH_INST
    jle     MterpCheckSuspendAndContinue    # AA * 2 <= 0 => suspend check
    GOTO_NEXT
