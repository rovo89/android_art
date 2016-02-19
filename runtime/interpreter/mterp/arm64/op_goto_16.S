    /*
     * Unconditional branch, 16-bit offset.
     *
     * The branch distance is a signed code-unit offset, which we need to
     * double to get a byte offset.
     */
    /* goto/16 +AAAA */
    FETCH_S wINST, 1                    // wINST<- ssssAAAA (sign-extended)
#if MTERP_PROFILE_BRANCHES
    EXPORT_PC
    mov     x0, xSELF
    add     x1, xFP, #OFF_FP_SHADOWFRAME
    sbfm    x2, xINST, 0, 31
    bl      MterpProfileBranch          // (self, shadow_frame, offset)
    cbnz    w0, MterpOnStackReplacement // Note: offset must be in xINST
#endif
    ldr     w7, [xSELF, #THREAD_FLAGS_OFFSET]
    adds    w1, wINST, wINST            // w1<- byte offset, flags set
    FETCH_ADVANCE_INST_RB w1            // update rPC, load rINST
    b.mi    MterpCheckSuspendAndContinue
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction
