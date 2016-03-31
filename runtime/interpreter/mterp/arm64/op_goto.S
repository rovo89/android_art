    /*
     * Unconditional branch, 8-bit offset.
     *
     * The branch distance is a signed code-unit offset, which we need to
     * double to get a byte offset.
     */
    /* goto +AA */
    /* tuning: use sbfx for 6t2+ targets */
    lsl     w0, wINST, #16              // w0<- AAxx0000
    asr     wINST, w0, #24              // wINST<- ssssssAA (sign-extended)
#if MTERP_PROFILE_BRANCHES
    EXPORT_PC
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
