/*
 * ===========================================================================
 *  Common subroutines and data
 * ===========================================================================
 */

    .text
    .align  2

/*
 * We've detected a condition that will result in an exception, but the exception
 * has not yet been thrown.  Just bail out to the reference interpreter to deal with it.
 * TUNING: for consistency, we may want to just go ahead and handle these here.
 */
#define MTERP_LOGGING 0
common_errDivideByZero:
    EXPORT_PC
#if MTERP_LOGGING
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpLogDivideByZeroException)
#endif
    jmp     MterpCommonFallback

common_errArrayIndex:
    EXPORT_PC
#if MTERP_LOGGING
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpLogArrayIndexException)
#endif
    jmp     MterpCommonFallback

common_errNegativeArraySize:
    EXPORT_PC
#if MTERP_LOGGING
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpLogNegativeArraySizeException)
#endif
    jmp     MterpCommonFallback

common_errNoSuchMethod:
    EXPORT_PC
#if MTERP_LOGGING
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpLogNoSuchMethodException)
#endif
    jmp     MterpCommonFallback

common_errNullObject:
    EXPORT_PC
#if MTERP_LOGGING
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpLogNullObjectException)
#endif
    jmp     MterpCommonFallback

common_exceptionThrown:
    EXPORT_PC
#if MTERP_LOGGING
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpLogExceptionThrownException)
#endif
    jmp     MterpCommonFallback

MterpSuspendFallback:
    EXPORT_PC
#if MTERP_LOGGING
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    movl    THREAD_FLAGS_OFFSET(rSELF), OUT_32_ARG2
    call    SYMBOL(MterpLogSuspendFallback)
#endif
    jmp     MterpCommonFallback

/*
 * If we're here, something is out of the ordinary.  If there is a pending
 * exception, handle it.  Otherwise, roll back and retry with the reference
 * interpreter.
 */
MterpPossibleException:
    cmpq    $$0, THREAD_EXCEPTION_OFFSET(rSELF)
    jz      MterpFallback
    /* intentional fallthrough - handle pending exception. */

/*
 * On return from a runtime helper routine, we've found a pending exception.
 * Can we handle it here - or need to bail out to caller?
 *
 */
MterpException:
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpHandleException)
    testb   %al, %al
    jz      MterpExceptionReturn
    REFRESH_IBASE
    movq    OFF_FP_CODE_ITEM(rFP), %rax
    mov     OFF_FP_DEX_PC(rFP), %ecx
    leaq    CODEITEM_INSNS_OFFSET(%rax), rPC
    leaq    (rPC, %rcx, 2), rPC
    movq    rPC, OFF_FP_DEX_PC_PTR(rFP)
    /* resume execution at catch block */
    FETCH_INST
    GOTO_NEXT
    /* NOTE: no fallthrough */

/*
 * Check for suspend check request.  Assumes rINST already loaded, rPC advanced and
 * still needs to get the opcode and branch to it, and flags are in lr.
 */
MterpCheckSuspendAndContinue:
    REFRESH_IBASE
    testl   $$(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST), THREAD_FLAGS_OFFSET(rSELF)
    jz      1f
    EXPORT_PC
    movq    rSELF, OUT_ARG0
    call    SYMBOL(MterpSuspendCheck)
1:
    GOTO_NEXT

/*
 * Bail out to reference interpreter.
 */
MterpFallback:
    EXPORT_PC
#if MTERP_LOGGING
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpLogFallback)
#endif
MterpCommonFallback:
    xorl    %eax, %eax
    jmp     MterpDone

/*
 * On entry:
 *  uint32_t* rFP  (should still be live, pointer to base of vregs)
 */
MterpExceptionReturn:
    movl    $$1, %eax
    jmp     MterpDone
MterpReturn:
    movq    OFF_FP_RESULT_REGISTER(rFP), %rdx
    movq    %rax, (%rdx)
    movl    $$1, %eax
MterpDone:
    /* pop up frame */
    addq    $$FRAME_SIZE, %rsp
    .cfi_adjust_cfa_offset -FRAME_SIZE

    /* Restore callee save register */
    POP %r15
    POP %r14
    POP %r13
    POP %r12
    POP %rbp
    POP %rbx
    ret
    .cfi_endproc
    SIZE(ExecuteMterpImpl,ExecuteMterpImpl)
