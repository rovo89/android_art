    /*
     * Unconditional branch, 8-bit offset.
     *
     * The branch distance is a signed code-unit offset, which we need to
     * double to get a byte offset.
     */
    /* goto +AA */
    /* tuning: use sbfx for 6t2+ targets */
#if MTERP_SUSPEND
    mov     w0, wINST, lsl #16          // w0<- AAxx0000
    movs    w1, w0, asr #24             // w1<- ssssssAA (sign-extended)
    add     w2, w1, w1                  // w2<- byte offset, set flags
       // If backwards branch refresh rIBASE
    ldrmi   rIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET] // refresh handler base
    FETCH_ADVANCE_INST_RB w2            // update rPC, load wINST
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction
#else
    lsl     w0, wINST, #16              // w0<- AAxx0000
    asr     wINST, w0, #24              // wINST<- ssssssAA (sign-extended)
#if MTERP_PROFILE_BRANCHES
    mov     x0, xSELF
    add     x1, xFP, #OFF_FP_SHADOWFRAME
    sbfm    x2, xINST, 0, 31
    bl      MterpProfileBranch          // (self, shadow_frame, offset)
    cbnz    w0, MterpOnStackReplacement // Note: offset must be in wINST
#endif
    ldr     w7, [xSELF, #THREAD_FLAGS_OFFSET]  // Preload flags for MterpCheckSuspendAndContinue
    adds    w1, wINST, wINST            // Convert dalvik offset to byte offset, setting flags
    FETCH_ADVANCE_INST_RB w1            // load wINST and advance xPC
       // If backwards branch refresh rIBASE
    b.mi     MterpCheckSuspendAndContinue
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction
#endif
