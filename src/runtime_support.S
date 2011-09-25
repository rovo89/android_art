#include "asm_support.h"

#if defined(__arm__)

    .balign 4

    /* Deliver the given exception */
    .extern artDeliverExceptionFromCode
    /* Deliver an exception pending on a thread */
    .extern artDeliverPendingException

    .global art_deliver_exception_from_code
    /*
     * Called by managed code, saves mosts registers (forms basis of long jump context) and passes
     * the bottom of the stack. artDeliverExceptionFromCode will place the callee save Method* at
     * the bottom of the thread. On entry r0 holds Throwable*
     */
art_deliver_exception_from_code:
    stmdb  sp!, {r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, lr}
    sub sp, #16                     @ 4 words of space, bottom word will hold Method*
    mov r1, r9                      @ pass Thread::Current
    mov r2, sp                      @ pass SP
    b   artDeliverExceptionFromCode @ artDeliverExceptionFromCode(Throwable*, Thread*, SP)

    .global art_throw_null_pointer_exception_from_code
    .extern artThrowNullPointerExceptionFromCode
    /*
     * Called by managed code to create and deliver a NullPointerException
     */
art_throw_null_pointer_exception_from_code:
    stmdb  sp!, {r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, lr}
    sub sp, #16                              @ 4 words of space, bottom word will hold Method*
    mov r0, r9                               @ pass Thread::Current
    mov r1, sp                               @ pass SP
    b   artThrowNullPointerExceptionFromCode @ artThrowNullPointerExceptionFromCode(Thread*, SP)

    .global art_throw_div_zero_from_code
    .extern artThrowDivZeroFromCode
    /*
     * Called by managed code to create and deliver an ArithmeticException
     */
art_throw_div_zero_from_code:
    stmdb  sp!, {r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, lr}
    sub sp, #16                 @ 4 words of space, bottom word will hold Method*
    mov r0, r9                  @ pass Thread::Current
    mov r1, sp                  @ pass SP
    b   artThrowDivZeroFromCode @ artThrowDivZeroFromCode(Thread*, SP)

    .global art_throw_array_bounds_from_code
    .extern artThrowArrayBoundsFromCode
    /*
     * Called by managed code to create and deliver an ArrayIndexOutOfBoundsException
     */
art_throw_array_bounds_from_code:
    stmdb  sp!, {r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, lr}
    sub sp, #16                     @ 4 words of space, bottom word will hold Method*
    mov r2, r9                      @ pass Thread::Current
    mov r3, sp                      @ pass SP
    b   artThrowArrayBoundsFromCode @ artThrowArrayBoundsFromCode(index, limit, Thread*, SP)

    .global art_throw_stack_overflow_from_code
    .extern artThrowStackOverflowFromCode
art_throw_stack_overflow_from_code:
    stmdb  sp!, {r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, lr}
    sub sp, #16                       @ 4 words of space, bottom word will hold Method*
    mov r1, r9                        @ pass Thread::Current
    mov r2, sp                        @ pass SP
    b   artThrowStackOverflowFromCode @ artThrowStackOverflowFromCode(method, Thread*, SP)

    .global art_throw_neg_array_size_from_code
    .extern artThrowNegArraySizeFromCode
art_throw_neg_array_size_from_code:
    stmdb  sp!, {r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, lr}
    sub sp, #16                       @ 4 words of space, bottom word will hold Method*
    mov r1, r9                        @ pass Thread::Current
    mov r2, sp                        @ pass SP
    b   artThrowNegArraySizeFromCode  @ artThrowNegArraySizeFromCode(size, Thread*, SP)

    .global art_throw_internal_error_from_code
    .extern artThrowInternalErrorFromCode
art_throw_internal_error_from_code:
    stmdb  sp!, {r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, lr}
    sub sp, #16                       @ 4 words of space, bottom word will hold Method*
    mov r1, r9                        @ pass Thread::Current
    mov r2, sp                        @ pass SP
    b   artThrowInternalErrorFromCode @ artThrowInternalErrorFromCode(errnum, Thread*, SP)

    .global art_throw_no_such_method_from_code
    .extern artThrowNoSuchMethodFromCode
art_throw_no_such_method_from_code:
    stmdb  sp!, {r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, lr}
    sub sp, #16                       @ 4 words of space, bottom word will hold Method*
    mov r1, r9                        @ pass Thread::Current
    mov r2, sp                        @ pass SP
    b   artThrowNoSuchMethodFromCode  @ artThrowNoSuchMethodFromCode(method_idx, Thread*, SP)

    .global art_throw_runtime_exception_from_code
    .extern artThrowRuntimeExceptionFromCode
art_throw_runtime_exception_from_code:
    stmdb  sp!, {r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, lr}
    sub sp, #16                          @ 4 words of space, bottom word will hold Method*
    mov r1, r9                           @ pass Thread::Current
    mov r2, sp                           @ pass SP
    b   artThrowRuntimeExceptionFromCode @ artThrowRuntimeExceptionFromCode(errnum, Thread*, SP)

    .global art_throw_verification_error_from_code
    .extern artThrowVerificationErrorFromCode
art_throw_verification_error_from_code:
    stmdb  sp!, {r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, lr}
    sub sp, #16                           @ 4 words of space, bottom word will hold Method*
    mov r2, r9                            @ pass Thread::Current
    mov r3, sp                            @ pass SP
    b   artThrowVerificationErrorFromCode @ artThrowVerificationErrorFromCode(src1, ref, Thread*, SP)

    .global art_invoke_interface_trampoline
    .extern artFindInterfaceMethodInCacheFromCode
    /*
     * All generated callsites for interface invokes will load arguments as usual - except instead
     * of loading arg0/r0 with the target Method*, arg0/r0 will contain the method_idx.  This
     * wrapper will save arg1-arg3, load the caller's Method*, align the stack and call the helper
     * artFindInterfaceMethodInCacheFromCode(idx, this, method);
     * NOTE: "this" is first visable argument of the target, and so can be found in arg1/r1.
     *
     * artFindInterfaceMethodInCacheFromCode will attempt to locate the target and return a 64-bit
     * result in r0/r1 consisting of the target Method* in r0 and method->code_ in r1.
     *
     * If unsuccessful, artFindInterfaceMethodInCacheFromCode will return NULL/NULL. There will be
     * a pending exception in the thread and we branch to another stub to deliver it.
     *
     * On success this wrapper will restore arguments and *jump* to the target, leaving the lr
     * pointing back to the original caller.
     */
art_invoke_interface_trampoline:
    str    sp, [R9, #THREAD_TOP_OF_MANAGED_STACK_OFFSET]    @ record top of stack and pc in case of
    str    lr, [R9, #THREAD_TOP_OF_MANAGED_STACK_PC_OFFSET] @ walking stack
    stmdb  sp!, {r1, r2, r3, lr}
    ldr    r2, [sp, #16]                         @ load caller's Method*
    bl     artFindInterfaceMethodInCacheFromCode @ (method_idx, this, callerMethod)
    mov    r12, r1                               @ save r0->code_
    ldmia  sp!, {r1, r2, r3, lr}                 @ restore arguments
    cmp    r0, #0                                @ did we find the target?
    bxne   r12                                   @ tail call to target if so
                                                 @ set up for throwing exception
    stmdb  sp!, {r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, lr}
    sub    sp, #16                               @ 4 words of space, bottom word will hold Method*
    mov    r0, r9                                @ pass Thread::Current
    mov    r1, sp                                @ pass SP
    b      artDeliverPendingExceptionFromCode    @ artDeliverPendingExceptionFromCode(Thread*, SP)

    .global art_handle_fill_data_from_code
    .extern artHandleFillArrayDataFromCode
    /*
     * Entry from managed code that calls artHandleFillArrayDataFromCode and delivers exception on
     * failure.
     */
art_handle_fill_data_from_code:
    str    sp, [R9, #THREAD_TOP_OF_MANAGED_STACK_OFFSET]    @ record top of stack and pc in case of
    str    lr, [R9, #THREAD_TOP_OF_MANAGED_STACK_PC_OFFSET] @ walking stack
    stmdb  sp!, {lr}                          @ Save LR
    sub    sp, #12                            @ Align stack
    bl     artHandleFillArrayDataFromCode     @ (Array* array, const uint16_t* table)
    add    sp, #12
    ldmia  sp!, {lr}                          @ restore LR
    cmp    r0, #0                             @ success?
    moveq  pc, lr                             @ return on success
                                              @ set up for throwing exception
    stmdb  sp!, {r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, lr}
    sub    sp, #16                            @ 4 words of space, bottom word will hold Method*
    mov    r0, r9                             @ pass Thread::Current
    mov    r1, sp                             @ pass SP
    b      artDeliverPendingExceptionFromCode @ artDeliverPendingExceptionFromCode(Thread*, SP)

    .global art_unlock_object_from_code
    .extern artUnlockObjectFromCode
    /*
     * Entry from managed code that calls artUnlockObjectFromCode and delivers exception on failure.
     */
art_unlock_object_from_code:
    str    sp, [R9, #THREAD_TOP_OF_MANAGED_STACK_OFFSET]    @ record top of stack and pc in case of
    str    lr, [R9, #THREAD_TOP_OF_MANAGED_STACK_PC_OFFSET] @ walking stack
    stmdb  sp!, {lr}                          @ Save LR
    sub    sp, #12                            @ Align stack
    bl     artUnlockObjectFromCode            @ (Thread* thread, Object* obj)
    add    sp, #12
    ldmia  sp!, {lr}                          @ restore LR
    cmp    r0, #0                             @ success?
    moveq  pc, lr                             @ return on success
                                              @ set up for throwing exception
    stmdb  sp!, {r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, lr}
    sub    sp, #16                            @ 4 words of space, bottom word will hold Method*
    mov    r0, r9                             @ pass Thread::Current
    mov    r1, sp                             @ pass SP
    b      artDeliverPendingExceptionFromCode @ artDeliverPendingExceptionFromCode(Thread*, SP)

    .global art_check_cast_from_code
    .extern artCheckCastFromCode
    /*
     * Entry from managed code that calls artCheckCastFromCode and delivers exception on failure.
     */
art_check_cast_from_code:
    str    sp, [R9, #THREAD_TOP_OF_MANAGED_STACK_OFFSET]    @ record top of stack and pc in case of
    str    lr, [R9, #THREAD_TOP_OF_MANAGED_STACK_PC_OFFSET] @ walking stack
    stmdb  sp!, {lr}                          @ Save LR
    sub    sp, #12                            @ Align stack
    bl     artCheckCastFromCode               @ (Class* a, Class* b)
    add    sp, #12
    ldmia  sp!, {lr}                          @ restore LR
    cmp    r0, #0                             @ success?
    moveq  pc, lr                             @ return on success
                                              @ set up for throwing exception
    stmdb  sp!, {r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, lr}
    sub    sp, #16                            @ 4 words of space, bottom word will hold Method*
    mov    r0, r9                             @ pass Thread::Current
    mov    r1, sp                             @ pass SP
    b      artDeliverPendingExceptionFromCode @ artDeliverPendingExceptionFromCode(Thread*, SP)

    .global art_can_put_array_element_from_code
    .extern artCanPutArrayElementFromCode
    /*
     * Entry from managed code that calls artCanPutArrayElementFromCode and delivers exception on
     * failure.
     */
art_can_put_array_element_from_code:
    cmp    r0, #0                             @ return if element == NULL
    moveq  pc, lr
    str    sp, [R9, #THREAD_TOP_OF_MANAGED_STACK_OFFSET]    @ record top of stack and pc in case of
    str    lr, [R9, #THREAD_TOP_OF_MANAGED_STACK_PC_OFFSET] @ walking stack
    stmdb  sp!, {lr}                          @ Save LR
    sub    sp, #12                            @ Align stack
    bl     artCanPutArrayElementFromCode      @ (Object* element, Class* array_class)
    add    sp, #12
    ldmia  sp!, {lr}                          @ restore LR
    cmp    r0, #0                             @ success?
    moveq  pc, lr                             @ return on success
                                              @ set up for throwing exception
    stmdb  sp!, {r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, lr}
    sub    sp, #16                            @ 4 words of space, bottom word will hold Method*
    mov    r0, r9                             @ pass Thread::Current
    mov    r1, sp                             @ pass SP
    b      artDeliverPendingExceptionFromCode @ artDeliverPendingExceptionFromCode(Thread*, SP)

    .global art_initialize_static_storage_from_code
    .extern _ZN3art11ClassLinker31InitializeStaticStorageFromCodeEjPKNS_6MethodE
    /*
     * Entry from managed code when uninitialized static storage, this stub will run the class
     * initializer and deliver the exception on error. On success the static storage base is
     * returned.
     */
art_initialize_static_storage_from_code:
    str    sp, [R9, #THREAD_TOP_OF_MANAGED_STACK_OFFSET]    @ record top of stack and pc in case of
    str    lr, [R9, #THREAD_TOP_OF_MANAGED_STACK_PC_OFFSET] @ walking stack
    stmdb  sp!, {lr}                          @ Save LR
    sub    sp, #12                            @ Align stack
    @ ClassLinker::InitializeStaticStorageFromCode(uint32_t type_idx, Method* referrer)
    bl     _ZN3art11ClassLinker31InitializeStaticStorageFromCodeEjPKNS_6MethodE
    add    sp, #12
    ldmia  sp!, {lr}                          @ restore LR
    cmp    r0, #0                             @ success if result is non-null
    movne  pc, lr                             @ return on success
                                              @ set up for throwing exception
    stmdb  sp!, {r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, lr}
    sub    sp, #16                            @ 4 words of space, bottom word will hold Method*
    mov    r0, r9                             @ pass Thread::Current
    mov    r1, sp                             @ pass SP
    b      artDeliverPendingExceptionFromCode @ artDeliverPendingExceptionFromCode(Thread*, SP)

    .global art_alloc_object_from_code
    .extern artAllocObjectFromCode
    /*
     * Called by managed code to allocate an object
     */
art_alloc_object_from_code:
    str    sp, [R9, #THREAD_TOP_OF_MANAGED_STACK_OFFSET]    @ record top of stack and pc in case of
    str    lr, [R9, #THREAD_TOP_OF_MANAGED_STACK_PC_OFFSET] @ walking stack
    stmdb  sp!, {lr}               @ Save LR
    sub    sp, #12                 @ Align stack
    bl     artAllocObjectFromCode  @ (uint32_t type_idx, Method* method)
    add    sp, #12
    ldmia  sp!, {lr}               @ restore LR
    cmp    r0, #0                  @ success if result is non-null
    movne  pc, lr                  @ return on success
                                   @ set up for throwing exception
    stmdb  sp!, {r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, lr}
    sub    sp, #16                 @ 4 words of space, bottom word will hold Method*
    mov    r0, r9                  @ pass Thread::Current
    mov    r1, sp                  @ pass SP
    b      artDeliverPendingExceptionFromCode  @ artDeliverPendingExceptionFromCode(Thread*, SP)

    .global art_array_alloc_from_code
    .extern artArrayAllocFromCode
    /*
     * Called by managed code to allocate an array
     */
art_array_alloc_from_code:
    str    sp, [R9, #THREAD_TOP_OF_MANAGED_STACK_OFFSET]    @ record top of stack and pc in case of
    str    lr, [R9, #THREAD_TOP_OF_MANAGED_STACK_PC_OFFSET] @ walking stack
    stmdb  sp!, {lr}              @ Save LR
    sub    sp, #12                @ Align stack
    bl     artArrayAllocFromCode  @ (uint32_t type_idx, Method* method, int32_t component_count)
    add    sp, #12
    ldmia  sp!, {lr}              @ restore LR
    cmp    r0, #0                 @ success if result is non-null
    movne  pc, lr                 @ return on success
                                  @ set up for throwing exception
    stmdb  sp!, {r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, lr}
    sub    sp, #16                @ 4 words of space, bottom word will hold Method*
    mov    r0, r9                 @ pass Thread::Current
    mov    r1, sp                 @ pass SP
    b      artDeliverPendingExceptionFromCode  @ artDeliverPendingExceptionFromCode(Thread*, SP)

    .global art_check_and_array_alloc_from_code
    .extern artCheckAndArrayAllocFromCode
    /*
     * Called by managed code to allocate an array
     */
art_check_and_array_alloc_from_code:
    str    sp, [R9, #THREAD_TOP_OF_MANAGED_STACK_OFFSET]    @ record top of stack and pc in case of
    str    lr, [R9, #THREAD_TOP_OF_MANAGED_STACK_PC_OFFSET] @ walking stack
    stmdb  sp!, {lr}                      @ Save LR
    sub    sp, #12                        @ Align stack
    bl     artCheckAndArrayAllocFromCode  @ (uint32_t type_idx, Method* method, int32_t count)
    add    sp, #12
    ldmia  sp!, {lr}                      @ restore LR
    cmp    r0, #0                         @ success if result is non-null
    movne  pc, lr                         @ return on success
                                          @ set up for throwing exception
    stmdb  sp!, {r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, lr}
    sub    sp, #16                        @ 4 words of space, bottom word will hold Method*
    mov    r0, r9                         @ pass Thread::Current
    mov    r1, sp                         @ pass SP
    b      artDeliverPendingExceptionFromCode  @ artDeliverPendingExceptionFromCode(Thread*, SP)

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

    .global art_deliver_exception_from_code
    .extern artDeliverExceptionFromCode
    /*
     * Called by managed code, saves callee saves and then calls artThrowException
     * that will place a mock Method* at the bottom of the stack.
     * EAX holds the exception.
     */
art_deliver_exception_from_code:
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
    pushl $0  // Alignment padding
    pushl %ecx                        // pass SP
    pushl %fs:THREAD_SELF_OFFSET      // pass fs:offsetof(Thread,self_)
    pushl %eax                        // pass Throwable*
    call artDeliverExceptionFromCode  // artDeliverExceptionFromCode(Throwable*, Thread*, SP)
    int3

#endif
