    /*
     * Unconditional branch, 16-bit offset.
     *
     * The branch distance is a signed code-unit offset, which we need to
     * double to get a byte offset.
     */
    /* goto/16 +AAAA */
#if MTERP_SUSPEND
    FETCH_S w0, 1                       // w0<- ssssAAAA (sign-extended)
    adds    w1, w0, w0                  // w1<- byte offset, flags set
    FETCH_ADVANCE_INST_RB w1            // update rPC, load rINST
    ldrmi   xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET] // refresh handler base
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction
#else
    FETCH_S wINST, 1                    // wINST<- ssssAAAA (sign-extended)
#if MTERP_PROFILE_BRANCHES
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
#endif
