    /*
     * Generic two-operand compare-and-branch operation.  Provide a "condition"
     * fragment that specifies the comparison to perform, e.g. for
     * "if-le" you would use "le".
     *
     * For: if-eq, if-ne, if-lt, if-ge, if-gt, if-le
     */
    /* if-cmp vA, vB, +CCCC */
    lh      a4, 2(rPC)                  # a4 <- sign-extended CCCC
    ext     a2, rINST, 8, 4             # a2 <- A
    ext     a3, rINST, 12, 4            # a3 <- B
    GET_VREG a0, a2                     # a0 <- vA
    GET_VREG a1, a3                     # a1 <- vB

    b${condition}c a0, a1, 1f
    li      a4, 2                       # offset if branch not taken
1:

    dlsa    rPC, a4, rPC, 1             # rPC <- rPC + CCCC * 2
    FETCH_INST                          # load rINST

#if MTERP_SUSPEND
    bgez    a4, 2f                      # CCCC * 2 >= 0 => no suspend check
    REFRESH_IBASE
2:
#else
    lw      ra, THREAD_FLAGS_OFFSET(rSELF)  # Preload flags for MterpCheckSuspendAndContinue
    bltz    a4, MterpCheckSuspendAndContinue
#endif

    GET_INST_OPCODE v0                  # extract opcode from rINST
    GOTO_OPCODE v0                      # jump to next instruction
