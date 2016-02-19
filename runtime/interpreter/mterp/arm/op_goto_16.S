    /*
     * Unconditional branch, 16-bit offset.
     *
     * The branch distance is a signed code-unit offset, which we need to
     * double to get a byte offset.
     */
    /* goto/16 +AAAA */
#if MTERP_PROFILE_BRANCHES
    FETCH_S rINST, 1                    @ rINST<- ssssAAAA (sign-extended)
    EXPORT_PC
    mov     r0, rSELF
    add     r1, rFP, #OFF_FP_SHADOWFRAME
    mov     r2, rINST
    bl      MterpProfileBranch          @ (self, shadow_frame, offset)
    cmp     r0, #0
    bne     MterpOnStackReplacement     @ Note: offset must be in rINST
    ldr     lr, [rSELF, #THREAD_FLAGS_OFFSET]
    adds    r1, rINST, rINST            @ r1<- byte offset, flags set
    FETCH_ADVANCE_INST_RB r1            @ update rPC, load rINST
    bmi     MterpCheckSuspendAndContinue
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction
#else
    FETCH_S rINST, 1                    @ rINST<- ssssAAAA (sign-extended)
    ldr     lr, [rSELF, #THREAD_FLAGS_OFFSET]
    adds    r1, rINST, rINST            @ r1<- byte offset, flags set
    FETCH_ADVANCE_INST_RB r1            @ update rPC, load rINST
    bmi     MterpCheckSuspendAndContinue
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction
#endif
