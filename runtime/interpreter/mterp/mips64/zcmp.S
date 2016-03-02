    /*
     * Generic one-operand compare-and-branch operation.  Provide a "condition"
     * fragment that specifies the comparison to perform, e.g. for
     * "if-lez" you would use "le".
     *
     * For: if-eqz, if-nez, if-ltz, if-gez, if-gtz, if-lez
     */
    /* if-cmp vAA, +BBBB */
    lh      a4, 2(rPC)                  # a4 <- sign-extended BBBB
    srl     a2, rINST, 8                # a2 <- AA
    GET_VREG a0, a2                     # a0 <- vAA

    b${condition}zc a0, 1f
    li      a4, 2                       # offset if branch not taken
1:

    dlsa    rPC, a4, rPC, 1             # rPC <- rPC + BBBB * 2
    FETCH_INST                          # load rINST

#if MTERP_SUSPEND
    bgez    a4, 2f                      # BBBB * 2 >= 0 => no suspend check
    REFRESH_IBASE
2:
#else
    lw      ra, THREAD_FLAGS_OFFSET(rSELF)  # Preload flags for MterpCheckSuspendAndContinue
    bltz    a4, MterpCheckSuspendAndContinue
#endif

    GET_INST_OPCODE v0                  # extract opcode from rINST
    GOTO_OPCODE v0                      # jump to next instruction
