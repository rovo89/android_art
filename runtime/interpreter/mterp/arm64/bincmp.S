    /*
     * Generic two-operand compare-and-branch operation.  Provide a "revcmp"
     * fragment that specifies the *reverse* comparison to perform, e.g.
     * for "if-le" you would use "gt".
     *
     * For: if-eq, if-ne, if-lt, if-ge, if-gt, if-le
     */
    /* if-cmp vA, vB, +CCCC */
#if MTERP_SUSPEND
    mov     w1, wINST, lsr #12          // w1<- B
    ubfx    w0, wINST, #8, #4           // w0<- A
    GET_VREG w3, w1                     // w3<- vB
    GET_VREG w2, w0                     // w2<- vA
    FETCH_S w1, 1                       // w1<- branch offset, in code units
    cmp     w2, w3                      // compare (vA, vB)
    mov${condition} w1, #2                 // w1<- BYTE branch dist for not-taken
    adds    w2, w1, w1                  // convert to bytes, check sign
    FETCH_ADVANCE_INST_RB w2            // update rPC, load wINST
    ldrmi   rIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]  // refresh rIBASE
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction
#else
    lsr     w1, wINST, #12              // w1<- B
    ubfx    w0, wINST, #8, #4           // w0<- A
    GET_VREG w3, w1                     // w3<- vB
    GET_VREG w2, w0                     // w2<- vA
    FETCH_S w1, 1                       // w1<- branch offset, in code units
    ldr     w7, [xSELF, #THREAD_FLAGS_OFFSET]
    mov     w0, #2                      // Offset if branch not taken
    cmp     w2, w3                      // compare (vA, vB)
    csel    w1, w1, w0, ${condition}    // Branch if true
    adds    w2, w1, w1                  // convert to bytes, check sign
    FETCH_ADVANCE_INST_RB w2            // update rPC, load wINST
    b.mi     MterpCheckSuspendAndContinue
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction
#endif
