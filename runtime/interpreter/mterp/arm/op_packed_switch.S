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
    /* op vAA, +BBBB */
#if MTERP_SUSPEND
    FETCH r0, 1                         @ r0<- bbbb (lo)
    FETCH r1, 2                         @ r1<- BBBB (hi)
    mov     r3, rINST, lsr #8           @ r3<- AA
    orr     r0, r0, r1, lsl #16         @ r0<- BBBBbbbb
    GET_VREG r1, r3                     @ r1<- vAA
    add     r0, rPC, r0, lsl #1         @ r0<- PC + BBBBbbbb*2
    bl      $func                       @ r0<- code-unit branch offset
    adds    r1, r0, r0                  @ r1<- byte offset; clear V
    ldrle   rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET] @ refresh handler base
    FETCH_ADVANCE_INST_RB r1            @ update rPC, load rINST
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction
#else
    FETCH r0, 1                         @ r0<- bbbb (lo)
    FETCH r1, 2                         @ r1<- BBBB (hi)
    mov     r3, rINST, lsr #8           @ r3<- AA
    orr     r0, r0, r1, lsl #16         @ r0<- BBBBbbbb
    GET_VREG r1, r3                     @ r1<- vAA
    add     r0, rPC, r0, lsl #1         @ r0<- PC + BBBBbbbb*2
    bl      $func                       @ r0<- code-unit branch offset
    ldr     lr, [rSELF, #THREAD_FLAGS_OFFSET]
    adds    r1, r0, r0                  @ r1<- byte offset; clear V
    FETCH_ADVANCE_INST_RB r1            @ update rPC, load rINST
    ble     MterpCheckSuspendAndContinue
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction
#endif
