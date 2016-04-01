    /*
     * Generic two-operand compare-and-branch operation.  Provide a "revcmp"
     * fragment that specifies the *reverse* comparison to perform, e.g.
     * for "if-le" you would use "gt".
     *
     * For: if-eq, if-ne, if-lt, if-ge, if-gt, if-le
     */
    /* if-cmp vA, vB, +CCCC */
    lsr     w1, wINST, #12              // w1<- B
    ubfx    w0, wINST, #8, #4           // w0<- A
    GET_VREG w3, w1                     // w3<- vB
    GET_VREG w2, w0                     // w2<- vA
    FETCH_S w1, 1                       // w1<- branch offset, in code units
    mov     w0, #2                      // Offset if branch not taken
    cmp     w2, w3                      // compare (vA, vB)
    csel    wINST, w1, w0, ${condition} // Branch if true, stashing result in callee save reg.
#if MTERP_PROFILE_BRANCHES
    // TUINING: once measurements are complete, remove #if and hand-schedule.
    EXPORT_PC
    mov     x0, xSELF
    add     x1, xFP, #OFF_FP_SHADOWFRAME
    sbfm    x2, xINST, 0, 31            // Sign extend branch offset
    bl      MterpProfileBranch          // (self, shadow_frame, offset)
    cbnz    w0, MterpOnStackReplacement // Note: offset must be in xINST
#endif
    ldr     w7, [xSELF, #THREAD_FLAGS_OFFSET]
    adds    w2, wINST, wINST            // convert to bytes, check sign
    FETCH_ADVANCE_INST_RB w2            // update rPC, load wINST
    b.mi     MterpCheckSuspendAndContinue
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction
