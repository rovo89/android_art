    /*
     * Generic two-operand compare-and-branch operation.  Provide a "revcmp"
     * fragment that specifies the *reverse* comparison to perform, e.g.
     * for "if-le" you would use "gt".
     *
     * For: if-eq, if-ne, if-lt, if-ge, if-gt, if-le
     */
    /* if-cmp vA, vB, +CCCC */
#if MTERP_SUSPEND
    mov     r1, rINST, lsr #12          @ r1<- B
    ubfx    r0, rINST, #8, #4           @ r0<- A
    GET_VREG r3, r1                     @ r3<- vB
    GET_VREG r2, r0                     @ r2<- vA
    FETCH_S r1, 1                       @ r1<- branch offset, in code units
    cmp     r2, r3                      @ compare (vA, vB)
    mov${revcmp} r1, #2                 @ r1<- BYTE branch dist for not-taken
    adds    r2, r1, r1                  @ convert to bytes, check sign
    FETCH_ADVANCE_INST_RB r2            @ update rPC, load rINST
    ldrmi   rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]  @ refresh rIBASE
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction
#else
    mov     r1, rINST, lsr #12          @ r1<- B
    ubfx    r0, rINST, #8, #4           @ r0<- A
    GET_VREG r3, r1                     @ r3<- vB
    GET_VREG r2, r0                     @ r2<- vA
    ldr     lr, [rSELF, #THREAD_FLAGS_OFFSET]
    FETCH_S r1, 1                       @ r1<- branch offset, in code units
    cmp     r2, r3                      @ compare (vA, vB)
    mov${revcmp} r1, #2                 @ r1<- BYTE branch dist for not-taken
    adds    r2, r1, r1                  @ convert to bytes, check sign
    FETCH_ADVANCE_INST_RB r2            @ update rPC, load rINST
    bmi     MterpCheckSuspendAndContinue
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction
#endif
