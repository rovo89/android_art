/*
 * Unconditional branch, 32-bit offset.
 *
 * The branch distance is a signed code-unit offset, which we need to
 * double to get a byte offset.
 *
 * Unlike most opcodes, this one is allowed to branch to itself, so
 * our "backward branch" test must be "<=0" instead of "<0".  Because
 * we need the V bit set, we'll use an adds to convert from Dalvik
 * offset to byte offset.
 */
    /* goto/32 +AAAAAAAA */
    movl    2(rPC), rINST                   # rINST <- AAAAAAAA
    MTERP_PROFILE_BRANCH
    addl    rINST, rINST                    # rINST <- AA * 2
    leal    (rPC, rINST), rPC
    FETCH_INST
    jle     MterpCheckSuspendAndContinue    # AA * 2 <= 0 => suspend check
    GOTO_NEXT
