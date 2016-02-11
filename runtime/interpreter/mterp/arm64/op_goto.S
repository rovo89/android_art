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
    ldr     w7, [xSELF, #THREAD_FLAGS_OFFSET]  // Preload flags for MterpCheckSuspendAndContinue
    lsl     w0, wINST, #16              // w0<- AAxx0000
    asr     w0, w0, #24                 // w0<- ssssssAA (sign-extended)
    adds    w1, w0, w0                  // Convert dalvik offset to byte offset, setting flags
    FETCH_ADVANCE_INST_RB w1            // load wINST and advance xPC
       // If backwards branch refresh rIBASE
    b.mi     MterpCheckSuspendAndContinue
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction
#endif
