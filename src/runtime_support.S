#include "asm_support.h"

#if defined(__arm__)

    .balign 4

    .global art_deliver_exception
    .extern artDeliverExceptionHelper
    /*
     * Called by managed code, saves mosts registers (forms basis of long jump context).
     * artThrowExceptionHelper will place a mock Method* at the bottom of the thread.
     * r0 holds Throwable
     */
art_deliver_exception:
    stmdb  sp!, {r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, lr}
    sub sp, #16                  @ 4 words of space, bottom word will hold Method*
    mov r1, r9                   @ pass Thread::Current
    mov r2, sp                   @ pass SP
    b artDeliverExceptionHelper  @ artDeliverExceptionHelper(Throwable*, Thread*, SP)

    .global art_throw_null_pointer_exception_from_code
    .extern artThrowNullPointerExceptionFromCodeHelper
    /*
     * Create NPE and deliver
     */
art_throw_null_pointer_exception_from_code:
    stmdb  sp!, {r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, lr}
    sub sp, #16                  @ 4 words of space, bottom word will hold Method*
    mov r0, r9                   @ pass Thread::Current
    mov r1, sp                   @ pass SP
    b artThrowNullPointerExceptionFromCodeHelper @ artThrowNullPointerExceptionFromCodeHelper(Thread*, SP)

    .global art_throw_div_zero_from_code
    .extern artThrowDivZeroFromCodeHelper
    /*
     * Create ArithmeticException and deliver
     */
art_throw_div_zero_from_code:
    stmdb  sp!, {r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, lr}
    sub sp, #16                  @ 4 words of space, bottom word will hold Method*
    mov r0, r9                   @ pass Thread::Current
    mov r1, sp                   @ pass SP
    b artThrowDivZeroFromCodeHelper @ artThrowDivZeroFromCodeHelper(Thread*, SP)

    .global art_throw_array_bounds_from_code
    .extern artThrowArrayBoundsFromCodeHelper
    /*
     * Create ArrayIndexOutOfBoundsException and deliver
     */
art_throw_array_bounds_from_code:
    stmdb  sp!, {r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, lr}
    sub sp, #16                  @ 4 words of space, bottom word will hold Method*
    mov r2, r9                   @ pass Thread::Current
    mov r3, sp                   @ pass SP
    b artThrowArrayBoundsFromCodeHelper @ artThrowArrayBoundsFromCodeHelper(index, limit, Thread*, SP)

    .global art_invoke_interface_trampoline
    .extern artFindInterfaceMethodInCache
    .extern artFailedInvokeInterface
art_invoke_interface_trampoline:
    /*
     * All generated callsites for interface invokes will load arguments
     * as usual - except instead of loading arg0/r0 with the target
     * Method*, arg0/r0 will contain the method_idx.  This wrapper will
     * save arg1-arg3, load the caller's Method*, align the stack and
     * call the helper artFindInterfaceMethodInCache(idx, this, method);
     * NOTE: "this" is first visable argument of the target, and so can be
     * found in arg1/r1.
     *
     * artFindInterfaceMethodInCache will attempt to locate the target
     * and return a 64-bit result in r0/r1 consisting of the target
     * Method* in r0 and method->code_ in r1.
     *
     * If unsuccessful, artFindInterfaceMethodInCache will return
     * NULL/NULL.  This is somewhat different than the usual
     * mechanism of helper routines performing the unwind & throw.
     * The reason is that this trampoline is not unwindable.  In the
     * event artFindInterfaceMethodInCache fails to resolve, the wrapper
     * will prepare an unwindable environment and jump to another helper
     * to do unwind/throw.
     *
     * On success this wrapper will restore arguments and *jump* to the
     * target, leaving the lr pointing back to the original caller.
     */
    stmdb  sp!, {r1, r2, r3, lr}
    ldr    r2, [sp, #16]                 @ load caller's Method*
    bl     artFindInterfaceMethodInCache @ (method_idx, this, callerMethod)
    mov    r12, r1                       @ save r0->code_
    ldmia  sp!, {r1, r2, r3, lr}         @ restore arguments
    cmp    r0, #0                        @ did we find the target?
    bxne   r12                           @ tail call to target if so
    b      artFailedInvokeInterface      @ Will appear as if called directly

    .global art_shl_long
art_shl_long:
    /*
     * Long integer shift.  This is different from the generic 32/64-bit
     * binary operations because vAA/vBB are 64-bit but vCC (the shift
     * distance) is 32-bit.  Also, Dalvik requires us to ignore all but the low
     * 6 bits.
     * On entry:
     *   r0: low word
     *   r1: high word
     *   r2: shift count
     */
    /* shl-long vAA, vBB, vCC */
    and     r2, r2, #63                 @ r2<- r2 & 0x3f
    mov     r1, r1, asl r2              @  r1<- r1 << r2
    rsb     r3, r2, #32                 @  r3<- 32 - r2
    orr     r1, r1, r0, lsr r3          @  r1<- r1 | (r0 << (32-r2))
    subs    ip, r2, #32                 @  ip<- r2 - 32
    movpl   r1, r0, asl ip              @  if r2 >= 32, r1<- r0 << (r2-32)
    mov     r0, r0, asl r2              @  r0<- r0 << r2
    bx      lr

    .balign 4
    .global art_shr_long
art_shr_long:
    /*
     * Long integer shift.  This is different from the generic 32/64-bit
     * binary operations because vAA/vBB are 64-bit but vCC (the shift
     * distance) is 32-bit.  Also, Dalvik requires us to ignore all but the low
     * 6 bits.
     * On entry:
     *   r0: low word
     *   r1: high word
     *   r2: shift count
     */
    /* shr-long vAA, vBB, vCC */
    and     r2, r2, #63                 @ r0<- r0 & 0x3f
    mov     r0, r0, lsr r2              @  r0<- r2 >> r2
    rsb     r3, r2, #32                 @  r3<- 32 - r2
    orr     r0, r0, r1, asl r3          @  r0<- r0 | (r1 << (32-r2))
    subs    ip, r2, #32                 @  ip<- r2 - 32
    movpl   r0, r1, asr ip              @  if r2 >= 32, r0<-r1 >> (r2-32)
    mov     r1, r1, asr r2              @  r1<- r1 >> r2
    bx      lr

    .balign 4
    .global art_ushr_long
art_ushr_long:
    /*
     * Long integer shift.  This is different from the generic 32/64-bit
     * binary operations because vAA/vBB are 64-bit but vCC (the shift
     * distance) is 32-bit.  Also, Dalvik requires us to ignore all but the low
     * 6 bits.
     * On entry:
     *   r0: low word
     *   r1: high word
     *   r2: shift count
     */
    /* ushr-long vAA, vBB, vCC */
    and     r2, r2, #63                 @ r0<- r0 & 0x3f
    mov     r0, r0, lsr r2              @  r0<- r2 >> r2
    rsb     r3, r2, #32                 @  r3<- 32 - r2
    orr     r0, r0, r1, asl r3          @  r0<- r0 | (r1 << (32-r2))
    subs    ip, r2, #32                 @  ip<- r2 - 32
    movpl   r0, r1, lsr ip              @  if r2 >= 32, r0<-r1 >>> (r2-32)
    mov     r1, r1, lsr r2              @  r1<- r1 >>> r2
    bx      lr

    .balign 4
    .global art_test_suspend
    .extern artCheckSuspendFromCode
art_test_suspend:
    /*
     * Check to see if there's a pending suspend request on our thread.
     * reset rSUSPEND to SUSPEND_CHECK_INTERVAL.
     * On entry, rSUSPEND holds the suspend request value
     * [TUNING: move load of suspend check value into this stub.
     */
    cmp    rSUSPEND, #0
    mov    rSUSPEND, #SUSPEND_CHECK_INTERVAL
    bxeq   rLR
    mov    r0, rSELF
    b      artCheckSuspendFromCode


#endif

#if defined(__i386__)

    .global art_deliver_exception
    .extern artDeliverExceptionHelper
    /*
     * Called by managed code, saves callee saves and then calls artThrowExceptionHelper
     * that will place a mock Method* at the bottom of the stack.
     * EAX holds the exception.
     */
art_deliver_exception:
    // Create frame
    pushl %edi  // Save callee saves
    pushl %esi
    pushl %ebp
    pushl %ebx
    pushl $0
    pushl $0
    pushl $0   // Will be clobbered to be Method*
    mov %esp, %ecx
    // Outgoing argument set up
    pushl $0    // Alignment padding
    pushl %ecx                      // pass SP
    pushl %fs:THREAD_SELF_OFFSET    // pass fs:offsetof(Thread,self_)
    pushl %eax                      // pass Throwable*
    call artDeliverExceptionHelper  // artDeliverExceptionHelper(Throwable*, Thread*, SP)
    int3

#endif
