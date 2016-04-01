/*
 * ===========================================================================
 *  Common subroutines and data
 * ===========================================================================
 */


/*
 * We've detected a condition that will result in an exception, but the exception
 * has not yet been thrown.  Just bail out to the reference interpreter to deal with it.
 * TUNING: for consistency, we may want to just go ahead and handle these here.
 */
common_errDivideByZero:
    EXPORT_PC
#if MTERP_LOGGING
    mov  x0, xSELF
    add  x1, xFP, #OFF_FP_SHADOWFRAME
    bl MterpLogDivideByZeroException
#endif
    b MterpCommonFallback

common_errArrayIndex:
    EXPORT_PC
#if MTERP_LOGGING
    mov  x0, xSELF
    add  x1, xFP, #OFF_FP_SHADOWFRAME
    bl MterpLogArrayIndexException
#endif
    b MterpCommonFallback

common_errNegativeArraySize:
    EXPORT_PC
#if MTERP_LOGGING
    mov  x0, xSELF
    add  x1, xFP, #OFF_FP_SHADOWFRAME
    bl MterpLogNegativeArraySizeException
#endif
    b MterpCommonFallback

common_errNoSuchMethod:
    EXPORT_PC
#if MTERP_LOGGING
    mov  x0, xSELF
    add  x1, xFP, #OFF_FP_SHADOWFRAME
    bl MterpLogNoSuchMethodException
#endif
    b MterpCommonFallback

common_errNullObject:
    EXPORT_PC
#if MTERP_LOGGING
    mov  x0, xSELF
    add  x1, xFP, #OFF_FP_SHADOWFRAME
    bl MterpLogNullObjectException
#endif
    b MterpCommonFallback

common_exceptionThrown:
    EXPORT_PC
#if MTERP_LOGGING
    mov  x0, xSELF
    add  x1, xFP, #OFF_FP_SHADOWFRAME
    bl MterpLogExceptionThrownException
#endif
    b MterpCommonFallback

MterpSuspendFallback:
    EXPORT_PC
#if MTERP_LOGGING
    mov  x0, xSELF
    add  x1, xFP, #OFF_FP_SHADOWFRAME
    ldr  x2, [xSELF, #THREAD_FLAGS_OFFSET]
    bl MterpLogSuspendFallback
#endif
    b MterpCommonFallback

/*
 * If we're here, something is out of the ordinary.  If there is a pending
 * exception, handle it.  Otherwise, roll back and retry with the reference
 * interpreter.
 */
MterpPossibleException:
    ldr     x0, [xSELF, #THREAD_EXCEPTION_OFFSET]
    cbz     x0, MterpFallback                       // If not, fall back to reference interpreter.
    /* intentional fallthrough - handle pending exception. */
/*
 * On return from a runtime helper routine, we've found a pending exception.
 * Can we handle it here - or need to bail out to caller?
 *
 */
MterpException:
    mov     x0, xSELF
    add     x1, xFP, #OFF_FP_SHADOWFRAME
    bl      MterpHandleException                    // (self, shadow_frame)
    cbz     w0, MterpExceptionReturn                // no local catch, back to caller.
    ldr     x0, [xFP, #OFF_FP_CODE_ITEM]
    ldr     w1, [xFP, #OFF_FP_DEX_PC]
    ldr     xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]
    add     xPC, x0, #CODEITEM_INSNS_OFFSET
    add     xPC, xPC, x1, lsl #1                    // generate new dex_pc_ptr
    /* Do we need to switch interpreters? */
    bl      MterpShouldSwitchInterpreters
    cbnz    w0, MterpFallback
    /* resume execution at catch block */
    EXPORT_PC
    FETCH_INST
    GET_INST_OPCODE ip
    GOTO_OPCODE ip
    /* NOTE: no fallthrough */

/*
 * Check for suspend check request.  Assumes wINST already loaded, xPC advanced and
 * still needs to get the opcode and branch to it, and flags are in lr.
 */
MterpCheckSuspendAndContinue:
    ldr     xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]  // refresh xIBASE
    ands    w7, w7, #(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST)
    b.ne    check1
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction
check1:
    EXPORT_PC
    mov     x0, xSELF
    bl      MterpSuspendCheck           // (self)
    cbnz    x0, MterpFallback           // Something in the environment changed, switch interpreters
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction

/*
 * On-stack replacement has happened, and now we've returned from the compiled method.
 */
MterpOnStackReplacement:
#if MTERP_LOGGING
    mov  x0, xSELF
    add  x1, xFP, #OFF_FP_SHADOWFRAME
    sbfm x2, xINST, 0, 31
    bl MterpLogOSR
#endif
    mov  x0, #1                         // Signal normal return
    b    MterpDone

/*
 * Bail out to reference interpreter.
 */
MterpFallback:
    EXPORT_PC
#if MTERP_LOGGING
    mov  x0, xSELF
    add  x1, xFP, #OFF_FP_SHADOWFRAME
    bl MterpLogFallback
#endif
MterpCommonFallback:
    mov     x0, #0                                  // signal retry with reference interpreter.
    b       MterpDone

/*
 * We pushed some registers on the stack in ExecuteMterpImpl, then saved
 * SP and LR.  Here we restore SP, restore the registers, and then restore
 * LR to PC.
 *
 * On entry:
 *  uint32_t* xFP  (should still be live, pointer to base of vregs)
 */
MterpExceptionReturn:
    mov     x0, #1                                  // signal return to caller.
    b MterpDone
MterpReturn:
    ldr     x2, [xFP, #OFF_FP_RESULT_REGISTER]
    ldr     lr, [xSELF, #THREAD_FLAGS_OFFSET]
    str     x0, [x2]
    mov     x0, xSELF
    ands    lr, lr, #(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST)
    b.eq    check2
    bl      MterpSuspendCheck                       // (self)
check2:
    mov     x0, #1                                  // signal return to caller.
MterpDone:
    ldp     fp, lr, [sp, #48]
    ldp     xPC, xFP, [sp, #32]
    ldp     xSELF, xINST, [sp, #16]
    ldp     xIBASE, xREFS, [sp], #64
    ret

    .cfi_endproc
    .size   ExecuteMterpImpl, .-ExecuteMterpImpl

