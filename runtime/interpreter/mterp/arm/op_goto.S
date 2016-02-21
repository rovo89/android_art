    /*
     * Unconditional branch, 8-bit offset.
     *
     * The branch distance is a signed code-unit offset, which we need to
     * double to get a byte offset.
     */
    /* goto +AA */
    /* tuning: use sbfx for 6t2+ targets */
#if MTERP_PROFILE_BRANCHES
    mov     r0, rINST, lsl #16          @ r0<- AAxx0000
    movs    rINST, r0, asr #24          @ rINST<- ssssssAA (sign-extended)
    EXPORT_PC
    mov     r0, rSELF
    add     r1, rFP, #OFF_FP_SHADOWFRAME
    mov     r2, rINST
    bl      MterpProfileBranch          @ (self, shadow_frame, offset)
    cmp     r0, #0
    bne     MterpOnStackReplacement     @ Note: offset must be in rINST
    ldr     lr, [rSELF, #THREAD_FLAGS_OFFSET]
    adds    r2, rINST, rINST            @ r2<- byte offset, set flags
    FETCH_ADVANCE_INST_RB r2            @ update rPC, load rINST
       @ If backwards branch refresh rIBASE
    bmi     MterpCheckSuspendAndContinue
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction
#else
    mov     r0, rINST, lsl #16          @ r0<- AAxx0000
    movs    rINST, r0, asr #24          @ rINST<- ssssssAA (sign-extended)
    ldr     lr, [rSELF, #THREAD_FLAGS_OFFSET]
    adds    r2, rINST, rINST            @ r2<- byte offset, set flags
    FETCH_ADVANCE_INST_RB r2            @ update rPC, load rINST
       @ If backwards branch refresh rIBASE
    bmi     MterpCheckSuspendAndContinue
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction
#endif
