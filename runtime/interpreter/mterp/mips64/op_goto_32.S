    /*
     * Unconditional branch, 32-bit offset.
     *
     * The branch distance is a signed code-unit offset, which we need to
     * double to get a byte offset.
     *
     * Unlike most opcodes, this one is allowed to branch to itself, so
     * our "backward branch" test must be "<=0" instead of "<0".
     */
    /* goto/32 +AAAAAAAA */
    lh      a0, 2(rPC)                  # a0 <- aaaa (low)
    lh      a1, 4(rPC)                  # a1 <- AAAA (high)
    ins     a0, a1, 16, 16              # a0 = sign-extended AAAAaaaa
    dlsa    rPC, a0, rPC, 1             # rPC <- rPC + AAAAAAAA * 2
    FETCH_INST                          # load rINST

#if MTERP_SUSPEND
    bgtz    a0, 1f                      # AA * 2 > 0 => no suspend check
    REFRESH_IBASE
1:
#else
    lw      ra, THREAD_FLAGS_OFFSET(rSELF)  # Preload flags for MterpCheckSuspendAndContinue
    blez    a0, MterpCheckSuspendAndContinue
#endif

    GET_INST_OPCODE v0                  # extract opcode from rINST
    GOTO_OPCODE v0                      # jump to next instruction
