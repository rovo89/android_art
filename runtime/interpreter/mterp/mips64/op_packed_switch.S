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
    /* op vAA, +BBBBBBBB */
    .extern $func
    lh      a0, 2(rPC)                  # a0 <- bbbb (lo)
    lh      a1, 4(rPC)                  # a1 <- BBBB (hi)
    srl     a3, rINST, 8                # a3 <- AA
    ins     a0, a1, 16, 16              # a0 <- BBBBbbbb
    GET_VREG a1, a3                     # a1 <- vAA
    dlsa    a0, a0, rPC, 1              # a0 <- PC + BBBBbbbb*2
    jal     $func                       # v0 <- code-unit branch offset
    dlsa    rPC, v0, rPC, 1             # rPC <- rPC + offset * 2
    FETCH_INST                          # load rINST
#if MTERP_SUSPEND
    bgtz    v0, 1f                      # offset * 2 > 0 => no suspend check
    REFRESH_IBASE
1:
#else
    lw      ra, THREAD_FLAGS_OFFSET(rSELF)  # Preload flags for MterpCheckSuspendAndContinue
    blez    v0, MterpCheckSuspendAndContinue
#endif
    GET_INST_OPCODE v0                  # extract opcode from rINST
    GOTO_OPCODE v0                      # jump to next instruction
