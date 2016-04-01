    /*
     * Unconditional branch, 32-bit offset.
     *
     * The branch distance is a signed code-unit offset, which we need to
     * double to get a byte offset.
     *
     * Unlike most opcodes, this one is allowed to branch to itself, so
     * our "backward branch" test must be "<=0" instead of "<0".  Because
     * we need the V bit set, we'll use an adds to convert from Dalvik
     * offset to byte offset.
     */
    /* goto/32 +AAAAAAAA */
#if MTERP_PROFILE_BRANCHES
    FETCH r0, 1                         @ r0<- aaaa (lo)
    FETCH r1, 2                         @ r1<- AAAA (hi)
    orr     rINST, r0, r1, lsl #16      @ rINST<- AAAAaaaa
    EXPORT_PC
    mov     r0, rSELF
    add     r1, rFP, #OFF_FP_SHADOWFRAME
    mov     r2, rINST
    bl      MterpProfileBranch          @ (self, shadow_frame, offset)
    cmp     r0, #0
    bne     MterpOnStackReplacement     @ Note: offset must be in rINST
    ldr     lr, [rSELF, #THREAD_FLAGS_OFFSET]
    adds    r1, rINST, rINST            @ r1<- byte offset
    FETCH_ADVANCE_INST_RB r1            @ update rPC, load rINST
    ble     MterpCheckSuspendAndContinue
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction
#else
    FETCH r0, 1                         @ r0<- aaaa (lo)
    FETCH r1, 2                         @ r1<- AAAA (hi)
    orr     rINST, r0, r1, lsl #16      @ rINST<- AAAAaaaa
    ldr     lr, [rSELF, #THREAD_FLAGS_OFFSET]
    adds    r1, rINST, rINST            @ r1<- byte offset
    FETCH_ADVANCE_INST_RB r1            @ update rPC, load rINST
    ble     MterpCheckSuspendAndContinue
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction
#endif
