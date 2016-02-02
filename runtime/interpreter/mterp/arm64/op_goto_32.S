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
#if MTERP_SUSPEND
    FETCH w0, 1                         // w0<- aaaa (lo)
    FETCH w1, 2                         // w1<- AAAA (hi)
    orr     w0, w0, w1, lsl #16         // w0<- AAAAaaaa
    adds    w1, w0, w0                  // w1<- byte offset
    FETCH_ADVANCE_INST_RB w1            // update rPC, load xINST
    ldrle   xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET] // refresh handler base
    GET_INST_OPCODE ip                  // extract opcode from xINST
    GOTO_OPCODE ip                      // jump to next instruction
#else
    FETCH w0, 1                         // w0<- aaaa (lo)
    FETCH w1, 2                         // w1<- AAAA (hi)
    ldr     w7, [xSELF, #THREAD_FLAGS_OFFSET]
    orr     w0, w0, w1, lsl #16         // w0<- AAAAaaaa
    adds    w1, w0, w0                  // w1<- byte offset
    FETCH_ADVANCE_INST_RB w1            // update rPC, load xINST
    b.le    MterpCheckSuspendAndContinue
    GET_INST_OPCODE ip                  // extract opcode from xINST
    GOTO_OPCODE ip                      // jump to next instruction
#endif
