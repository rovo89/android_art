    /*
     * Unconditional branch, 8-bit offset.
     *
     * The branch distance is a signed code-unit offset, which we need to
     * double to get a byte offset.
     */
    /* goto +AA */
    /* tuning: use sbfx for 6t2+ targets */
#if MTERP_SUSPEND
    mov     r0, rINST, lsl #16          @ r0<- AAxx0000
    movs    r1, r0, asr #24             @ r1<- ssssssAA (sign-extended)
    add     r2, r1, r1                  @ r2<- byte offset, set flags
       @ If backwards branch refresh rIBASE
    ldrmi   rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET] @ refresh handler base
    FETCH_ADVANCE_INST_RB r2            @ update rPC, load rINST
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction
#else
    ldr     lr, [rSELF, #THREAD_FLAGS_OFFSET]
    mov     r0, rINST, lsl #16          @ r0<- AAxx0000
    movs    r1, r0, asr #24             @ r1<- ssssssAA (sign-extended)
    add     r2, r1, r1                  @ r2<- byte offset, set flags
    FETCH_ADVANCE_INST_RB r2            @ update rPC, load rINST
       @ If backwards branch refresh rIBASE
    bmi     MterpCheckSuspendAndContinue
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction
#endif
