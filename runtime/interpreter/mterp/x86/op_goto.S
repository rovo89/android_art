/*
 * Unconditional branch, 8-bit offset.
 *
 * The branch distance is a signed code-unit offset, which we need to
 * double to get a byte offset.
 */
    /* goto +AA */
    movsbl  rINSTbl, rINST                  # rINST <- ssssssAA
    MTERP_PROFILE_BRANCH
    addl    rINST, rINST                    # rINST <- AA * 2
    leal    (rPC, rINST), rPC
    FETCH_INST
    jle      MterpCheckSuspendAndContinue   # AA * 2 <= 0 => suspend check
    GOTO_NEXT
