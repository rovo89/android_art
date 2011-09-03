#if defined(__arm__)

    .balign 4

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

#endif
