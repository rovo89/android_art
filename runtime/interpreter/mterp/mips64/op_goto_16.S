    /*
     * Unconditional branch, 16-bit offset.
     *
     * The branch distance is a signed code-unit offset, which we need to
     * double to get a byte offset.
     */
    /* goto/16 +AAAA */
    lh      a0, 2(rPC)                  # a0 <- sign-extended AAAA
    dlsa    rPC, a0, rPC, 1             # rPC <- rPC + AAAA * 2
    FETCH_INST                          # load rINST

#if MTERP_SUSPEND
    bgez    a0, 1f                      # AA * 2 >= 0 => no suspend check
    REFRESH_IBASE
1:
#else
    lw      ra, THREAD_FLAGS_OFFSET(rSELF)  # Preload flags for MterpCheckSuspendAndContinue
    bltz    a0, MterpCheckSuspendAndContinue
#endif

    GET_INST_OPCODE v0                  # extract opcode from rINST
    GOTO_OPCODE v0                      # jump to next instruction
