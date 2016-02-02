    /*
     * Generic one-operand compare-and-branch operation.  Provide a "revcmp"
     * fragment that specifies the *reverse* comparison to perform, e.g.
     * for "if-le" you would use "gt".
     *
     * for: if-eqz, if-nez, if-ltz, if-gez, if-gtz, if-lez
     */
    /* if-cmp vAA, +BBBB */
#if MTERP_SUSPEND
    mov     w0, wINST, lsr #8           // w0<- AA
    GET_VREG w2, w0                     // w2<- vAA
    FETCH_S w1, 1                       // w1<- branch offset, in code units
    cmp     w2, #0                      // compare (vA, 0)
    mov${condition} w1, #2                 // w1<- inst branch dist for not-taken
    adds    w1, w1, w1                  // convert to bytes & set flags
    FETCH_ADVANCE_INST_RB w1            // update rPC, load wINST
    ldrmi   rIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]   // refresh table base
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction
#else
    lsr     w0, wINST, #8               // w0<- AA
    GET_VREG w2, w0                     // w2<- vAA
    FETCH_S w1, 1                       // w1<- branch offset, in code units
    ldr     w7, [xSELF, #THREAD_FLAGS_OFFSET]
    mov     w0, #2                      // Branch offset if not taken
    cmp     w2, #0                      // compare (vA, 0)
    csel    w1, w1, w0, ${condition}    // Branch if true
    adds    w2, w1, w1                  // convert to bytes & set flags
    FETCH_ADVANCE_INST_RB w2            // update rPC, load wINST
    b.mi    MterpCheckSuspendAndContinue
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction
#endif
