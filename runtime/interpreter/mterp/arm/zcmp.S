    /*
     * Generic one-operand compare-and-branch operation.  Provide a "revcmp"
     * fragment that specifies the *reverse* comparison to perform, e.g.
     * for "if-le" you would use "gt".
     *
     * for: if-eqz, if-nez, if-ltz, if-gez, if-gtz, if-lez
     */
    /* if-cmp vAA, +BBBB */
#if MTERP_PROFILE_BRANCHES
    mov     r0, rINST, lsr #8           @ r0<- AA
    GET_VREG r2, r0                     @ r2<- vAA
    FETCH_S rINST, 1                    @ rINST<- branch offset, in code units
    ldr     lr, [rSELF, #THREAD_FLAGS_OFFSET]
    cmp     r2, #0                      @ compare (vA, 0)
    b${revcmp} .L_${opcode}_not_taken
    EXPORT_PC
    mov     r0, rSELF
    add     r1, rFP, #OFF_FP_SHADOWFRAME
    mov     r2, rINST
    bl      MterpProfileBranch          @ (self, shadow_frame, offset)
    cmp     r0, #0
    bne     MterpOnStackReplacement     @ Note: offset must be in rINST
    adds    r1, rINST, rINST            @ convert to bytes & set flags
    FETCH_ADVANCE_INST_RB r1            @ update rPC, load rINST
    bmi     MterpCheckSuspendAndContinue
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction
.L_${opcode}_not_taken:
    FETCH_ADVANCE_INST 2                @ update rPC, load rINST
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction
#else
    mov     r0, rINST, lsr #8           @ r0<- AA
    GET_VREG r2, r0                     @ r2<- vAA
    FETCH_S rINST, 1                    @ rINST<- branch offset, in code units
    ldr     lr, [rSELF, #THREAD_FLAGS_OFFSET]
    cmp     r2, #0                      @ compare (vA, 0)
    mov${revcmp} rINST, #2              @ rINST<- inst branch dist for not-taken
    adds    r1, rINST, rINST            @ convert to bytes & set flags
    FETCH_ADVANCE_INST_RB r1            @ update rPC, load rINST
    bmi     MterpCheckSuspendAndContinue
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction
#endif
