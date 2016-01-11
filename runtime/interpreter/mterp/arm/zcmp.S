    /*
     * Generic one-operand compare-and-branch operation.  Provide a "revcmp"
     * fragment that specifies the *reverse* comparison to perform, e.g.
     * for "if-le" you would use "gt".
     *
     * for: if-eqz, if-nez, if-ltz, if-gez, if-gtz, if-lez
     */
    /* if-cmp vAA, +BBBB */
#if MTERP_SUSPEND
    mov     r0, rINST, lsr #8           @ r0<- AA
    GET_VREG r2, r0                     @ r2<- vAA
    FETCH_S r1, 1                       @ r1<- branch offset, in code units
    cmp     r2, #0                      @ compare (vA, 0)
    mov${revcmp} r1, #2                 @ r1<- inst branch dist for not-taken
    adds    r1, r1, r1                  @ convert to bytes & set flags
    FETCH_ADVANCE_INST_RB r1            @ update rPC, load rINST
    ldrmi   rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]   @ refresh table base
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction
#else
    mov     r0, rINST, lsr #8           @ r0<- AA
    GET_VREG r2, r0                     @ r2<- vAA
    FETCH_S r1, 1                       @ r1<- branch offset, in code units
    ldr     lr, [rSELF, #THREAD_FLAGS_OFFSET]
    cmp     r2, #0                      @ compare (vA, 0)
    mov${revcmp} r1, #2                 @ r1<- inst branch dist for not-taken
    adds    r1, r1, r1                  @ convert to bytes & set flags
    FETCH_ADVANCE_INST_RB r1            @ update rPC, load rINST
    bmi     MterpCheckSuspendAndContinue
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction
#endif
