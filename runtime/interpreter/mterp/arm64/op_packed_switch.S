%default { "func":"MterpDoPackedSwitch" }
    /*
     * Handle a packed-switch or sparse-switch instruction.  In both cases
     * we decode it and hand it off to a helper function.
     *
     * We don't really expect backward branches in a switch statement, but
     * they're perfectly legal, so we check for them here.
     *
     * for: packed-switch, sparse-switch
     */
    /* op vAA, +BBBB */
    FETCH w0, 1                         // w0<- bbbb (lo)
    FETCH w1, 2                         // w1<- BBBB (hi)
    lsr     w3, wINST, #8               // w3<- AA
    orr     w0, w0, w1, lsl #16         // w0<- BBBBbbbb
    GET_VREG w1, w3                     // w1<- vAA
    add     x0, xPC, w0, lsl #1         // w0<- PC + BBBBbbbb*2
    bl      $func                       // w0<- code-unit branch offset
    sbfm    xINST, x0, 0, 31
#if MTERP_PROFILE_BRANCHES
    EXPORT_PC
    mov     x0, xSELF
    add     x1, xFP, #OFF_FP_SHADOWFRAME
    mov     x2, xINST
    bl      MterpProfileBranch          // (self, shadow_frame, offset)
    cbnz    w0, MterpOnStackReplacement
#endif
    ldr     w7, [xSELF, #THREAD_FLAGS_OFFSET]
    adds    w1, wINST, wINST            // w1<- byte offset; clear V
    FETCH_ADVANCE_INST_RB w1            // update rPC, load wINST
    b.le    MterpCheckSuspendAndContinue
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction
