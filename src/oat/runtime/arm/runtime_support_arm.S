/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "asm_support.h"

    /* Deliver the given exception */
    .extern artDeliverExceptionFromCode
    /* Deliver an exception pending on a thread */
    .extern artDeliverPendingException

    /* Cache alignment for function entry */
.macro ALIGN_FUNCTION_ENTRY
    .balign 16
.endm

    /*
     * Macro that sets up the callee save frame to conform with
     * Runtime::CreateCalleeSaveMethod(kSaveAll)
     */
.macro SETUP_SAVE_ALL_CALLEE_SAVE_FRAME
    push {r4-r11, lr} @ 9 words of callee saves
    vpush {s0-s31}
    sub sp, #12       @ 3 words of space, bottom word will hold Method*
.endm

    /*
     * Macro that sets up the callee save frame to conform with
     * Runtime::CreateCalleeSaveMethod(kRefsOnly). Restoration assumes non-moving GC.
     */
.macro SETUP_REF_ONLY_CALLEE_SAVE_FRAME
    push {r5-r8, r10-r11, lr} @ 7 words of callee saves
    sub sp, #4                @ bottom word will hold Method*
.endm

.macro RESTORE_REF_ONLY_CALLEE_SAVE_FRAME
    ldr lr, [sp, #28]  @ restore lr for return
    add sp, #32        @ unwind stack
.endm

.macro RESTORE_REF_ONLY_CALLEE_SAVE_FRAME_AND_RETURN
    ldr lr, [sp, #28]  @ restore lr for return
    add sp, #32        @ unwind stack
    bx  lr             @ return
.endm

    /*
     * Macro that sets up the callee save frame to conform with
     * Runtime::CreateCalleeSaveMethod(kRefsAndArgs). Restoration assumes non-moving GC.
     */
.macro SETUP_REF_AND_ARGS_CALLEE_SAVE_FRAME
    push {r1-r3, r5-r8, r10-r11, lr}  @ 10 words of callee saves
    sub sp, #8                        @ 2 words of space, bottom word will hold Method*
.endm

.macro RESTORE_REF_AND_ARGS_CALLEE_SAVE_FRAME
    ldr  r1, [sp, #8]          @ restore non-callee save r1
    ldrd r2, [sp, #12]         @ restore non-callee saves r2-r3
    ldr  lr, [sp, #44]         @ restore lr
    add  sp, #48               @ rewind sp
.endm

    /*
     * Macro that set calls through to artDeliverPendingExceptionFromCode, where the pending
     * exception is Thread::Current()->exception_
     */
.macro DELIVER_PENDING_EXCEPTION
    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME           @ save callee saves for throw
    mov    r0, r9                              @ pass Thread::Current
    mov    r1, sp                              @ pass SP
    b      artDeliverPendingExceptionFromCode  @ artDeliverPendingExceptionFromCode(Thread*, SP)
.endm

.macro NO_ARG_RUNTIME_EXCEPTION c_name, cxx_name
    .global \c_name
    .extern \cxx_name
    ALIGN_FUNCTION_ENTRY
\c_name:
    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME  // save all registers as basis for long jump context
    mov r0, r9                      @ pass Thread::Current
    mov r1, sp                      @ pass SP
    b   \cxx_name                   @ \cxx_name(Thread*, SP)
.endm

.macro ONE_ARG_RUNTIME_EXCEPTION c_name, cxx_name
    .global \c_name
    .extern \cxx_name
    ALIGN_FUNCTION_ENTRY
\c_name:
    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME  // save all registers as basis for long jump context
    mov r1, r9                      @ pass Thread::Current
    mov r2, sp                      @ pass SP
    b   \cxx_name                   @ \cxx_name(Thread*, SP)
.endm

.macro TWO_ARG_RUNTIME_EXCEPTION c_name, cxx_name
    .global \c_name
    .extern \cxx_name
    ALIGN_FUNCTION_ENTRY
\c_name:
    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME  // save all registers as basis for long jump context
    mov r2, r9                      @ pass Thread::Current
    mov r3, sp                      @ pass SP
    b   \cxx_name                   @ \cxx_name(Thread*, SP)
.endm

    /*
     * Called by managed code, saves callee saves and then calls artThrowException
     * that will place a mock Method* at the bottom of the stack. Arg1 holds the exception.
     */
ONE_ARG_RUNTIME_EXCEPTION art_deliver_exception_from_code, artDeliverExceptionFromCode

    /*
     * Called by managed code to create and deliver a NullPointerException.
     */
NO_ARG_RUNTIME_EXCEPTION art_throw_null_pointer_exception_from_code, artThrowNullPointerExceptionFromCode

    /*
     * Called by managed code to create and deliver an ArithmeticException.
     */
NO_ARG_RUNTIME_EXCEPTION art_throw_div_zero_from_code, artThrowDivZeroFromCode

    /*
     * Called by managed code to create and deliver an ArrayIndexOutOfBoundsException. Arg1 holds
     * index, arg2 holds limit.
     */
TWO_ARG_RUNTIME_EXCEPTION art_throw_array_bounds_from_code, artThrowArrayBoundsFromCode

    /*
     * Called by managed code to create and deliver a StackOverflowError.
     */
NO_ARG_RUNTIME_EXCEPTION art_throw_stack_overflow_from_code, artThrowStackOverflowFromCode

    /*
     * Called by managed code to create and deliver a NoSuchMethodError.
     */
ONE_ARG_RUNTIME_EXCEPTION art_throw_no_such_method_from_code, artThrowNoSuchMethodFromCode

    /*
     * Called by managed code to create and deliver verification errors. Arg1 is kind, arg2 is ref.
     */
TWO_ARG_RUNTIME_EXCEPTION art_throw_verification_error_from_code, artThrowVerificationErrorFromCode

    /*
     * All generated callsites for interface invokes and invocation slow paths will load arguments
     * as usual - except instead of loading arg0/r0 with the target Method*, arg0/r0 will contain
     * the method_idx.  This wrapper will save arg1-arg3, load the caller's Method*, align the
     * stack and call the appropriate C helper.
     * NOTE: "this" is first visible argument of the target, and so can be found in arg1/r1.
     *
     * The helper will attempt to locate the target and return a 64-bit result in r0/r1 consisting
     * of the target Method* in r0 and method->code_ in r1.
     *
     * If unsuccessful, the helper will return NULL/NULL. There will bea pending exception in the
     * thread and we branch to another stub to deliver it.
     *
     * On success this wrapper will restore arguments and *jump* to the target, leaving the lr
     * pointing back to the original caller.
     */
.macro INVOKE_TRAMPOLINE c_name, cxx_name
    .global \c_name
    .extern \cxx_name
    ALIGN_FUNCTION_ENTRY
\c_name:
    SETUP_REF_AND_ARGS_CALLEE_SAVE_FRAME  @ save callee saves in case allocation triggers GC
    ldr    r2, [sp, #48]                  @ pass caller Method*
    mov    r3, r9                         @ pass Thread::Current
    mov    r12, sp
    str    r12, [sp, #-16]!               @ expand the frame and pass SP
    bl     \cxx_name                      @ (method_idx, this, caller, Thread*, SP)
    add    sp, #16                        @ strip the extra frame
    mov    r12, r1                        @ save Method*->code_
    RESTORE_REF_AND_ARGS_CALLEE_SAVE_FRAME
    cmp    r0, #0                         @ did we find the target?
    bxne   r12                            @ tail call to target if so
    DELIVER_PENDING_EXCEPTION
.endm

INVOKE_TRAMPOLINE art_invoke_interface_trampoline, artInvokeInterfaceTrampoline
INVOKE_TRAMPOLINE art_invoke_interface_trampoline_with_access_check, artInvokeInterfaceTrampolineWithAccessCheck

INVOKE_TRAMPOLINE art_invoke_static_trampoline_with_access_check, artInvokeStaticTrampolineWithAccessCheck
INVOKE_TRAMPOLINE art_invoke_direct_trampoline_with_access_check, artInvokeDirectTrampolineWithAccessCheck
INVOKE_TRAMPOLINE art_invoke_super_trampoline_with_access_check, artInvokeSuperTrampolineWithAccessCheck
INVOKE_TRAMPOLINE art_invoke_virtual_trampoline_with_access_check, artInvokeVirtualTrampolineWithAccessCheck

    .global art_update_debugger
    .extern artUpdateDebuggerFromCode
    /*
     * On entry, r0 and r1 must be preserved, r2 is dex PC
     */
     ALIGN_FUNCTION_ENTRY
art_update_debugger:
    mov    r3, r0         @ stash away r0 so that it's saved as if it were an argument
    SETUP_REF_AND_ARGS_CALLEE_SAVE_FRAME
    mov    r0, r2         @ arg0 is dex PC
    mov    r1, rSELF      @ arg1 is Thread*
    mov    r2, sp         @ arg2 is sp
    bl     artUpdateDebuggerFromCode      @ artUpdateDebuggerFromCode(int32_t, Thread*, Method**)
    RESTORE_REF_AND_ARGS_CALLEE_SAVE_FRAME
    mov    r0, r3         @ restore original r0
    bx     lr

    .global art_do_long_jump
    /*
     * On entry r0 is uint32_t* gprs_ and r1 is uint32_t* fprs_
     */
     ALIGN_FUNCTION_ENTRY
art_do_long_jump:
    vldm r1, {s0-s31}     @ load all fprs from argument fprs_
    ldr  r2, [r0, #60]    @ r2 = r15 (PC from gprs_ 60=4*15)
    add  r0, r0, #12      @ increment r0 to skip gprs_[0..2] 12=4*3
    ldm  r0, {r3-r14}     @ load remaining gprs from argument gprs_
    mov  r0, #0           @ clear result registers r0 and r1
    mov  r1, #0
    bx   r2               @ do long jump

    .global art_work_around_app_jni_bugs
    .extern artWorkAroundAppJniBugs
    /*
     * Entry point of native methods when JNI bug compatibility is enabled.
     */
    ALIGN_FUNCTION_ENTRY
art_work_around_app_jni_bugs:
    @ save registers that may contain arguments and LR that will be crushed by a call
    push {r0-r3, lr}
    sub sp, #12      @ 3 words of space for alignment
    mov r0, r9       @ pass Thread::Current
    mov r1, sp       @ pass SP
    bl  artWorkAroundAppJniBugs  @ (Thread*, SP)
    add sp, #12      @ rewind stack
    mov r12, r0      @ save target address
    pop {r0-r3, lr}  @ restore possibly modified argument registers
    bx  r12          @ tail call into JNI routine

    .global art_handle_fill_data_from_code
    .extern artHandleFillArrayDataFromCode
    /*
     * Entry from managed code that calls artHandleFillArrayDataFromCode and delivers exception on
     * failure.
     */
    ALIGN_FUNCTION_ENTRY
art_handle_fill_data_from_code:
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME  @ save callee saves in case exception allocation triggers GC
    mov    r2, r9                          @ pass Thread::Current
    mov    r3, sp                          @ pass SP
    bl     artHandleFillArrayDataFromCode  @ (Array*, const DexFile::Payload*, Thread*, SP)
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME
    cmp    r0, #0                          @ success?
    bxeq   lr                              @ return on success
    DELIVER_PENDING_EXCEPTION

    .global art_lock_object_from_code
    .extern artLockObjectFromCode
    /*
     * Entry from managed code that calls artLockObjectFromCode, may block for GC.
     */
    ALIGN_FUNCTION_ENTRY
art_lock_object_from_code:
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME  @ save callee saves in case we block
    mov    r1, r9                     @ pass Thread::Current
    mov    r2, sp                     @ pass SP
    bl     artLockObjectFromCode      @ (Object* obj, Thread*, SP)
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME_AND_RETURN

    .global art_unlock_object_from_code
    .extern artUnlockObjectFromCode
    /*
     * Entry from managed code that calls artUnlockObjectFromCode and delivers exception on failure.
     */
    ALIGN_FUNCTION_ENTRY
art_unlock_object_from_code:
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME  @ save callee saves in case exception allocation triggers GC
    mov    r1, r9                   @ pass Thread::Current
    mov    r2, sp                   @ pass SP
    bl     artUnlockObjectFromCode  @ (Object* obj, Thread*, SP)
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME
    cmp    r0, #0                   @ success?
    bxeq   lr                       @ return on success
    DELIVER_PENDING_EXCEPTION

    .global art_check_cast_from_code
    .extern artCheckCastFromCode
    /*
     * Entry from managed code that calls artCheckCastFromCode and delivers exception on failure.
     */
    ALIGN_FUNCTION_ENTRY
art_check_cast_from_code:
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME    @ save callee saves in case exception allocation triggers GC
    mov    r2, r9                       @ pass Thread::Current
    mov    r3, sp                       @ pass SP
    bl     artCheckCastFromCode         @ (Class* a, Class* b, Thread*, SP)
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME
    cmp    r0, #0                       @ success?
    bxeq   lr                           @ return on success
    DELIVER_PENDING_EXCEPTION

    .global art_can_put_array_element_from_code
    .extern artCanPutArrayElementFromCode
    /*
     * Entry from managed code that calls artCanPutArrayElementFromCode and delivers exception on
     * failure.
     */
    ALIGN_FUNCTION_ENTRY
art_can_put_array_element_from_code:
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME    @ save callee saves in case exception allocation triggers GC
    mov    r2, r9                         @ pass Thread::Current
    mov    r3, sp                         @ pass SP
    bl     artCanPutArrayElementFromCode  @ (Object* element, Class* array_class, Thread*, SP)
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME
    cmp    r0, #0                         @ success?
    bxeq   lr                             @ return on success
    DELIVER_PENDING_EXCEPTION

    .global art_initialize_static_storage_from_code
    .extern artInitializeStaticStorageFromCode
    /*
     * Entry from managed code when uninitialized static storage, this stub will run the class
     * initializer and deliver the exception on error. On success the static storage base is
     * returned.
     */
    ALIGN_FUNCTION_ENTRY
art_initialize_static_storage_from_code:
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME           @ save callee saves in case of GC
    mov    r2, r9                              @ pass Thread::Current
    mov    r3, sp                              @ pass SP
    @ artInitializeStaticStorageFromCode(uint32_t type_idx, Method* referrer, Thread*, SP)
    bl     artInitializeStaticStorageFromCode
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME
    cmp    r0, #0                              @ success if result is non-null
    bxne   lr                                  @ return on success
    DELIVER_PENDING_EXCEPTION

    .global art_initialize_type_from_code
    .extern artInitializeTypeFromCode
    /*
     * Entry from managed code when dex cache misses for a type_idx
     */
    ALIGN_FUNCTION_ENTRY
art_initialize_type_from_code:
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME           @ save callee saves in case of GC
    mov    r2, r9                              @ pass Thread::Current
    mov    r3, sp                              @ pass SP
    @ artInitializeTypeFromCode(uint32_t type_idx, Method* referrer, Thread*, SP)
    bl     artInitializeTypeFromCode
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME
    cmp    r0, #0                              @ success if result is non-null
    bxne   lr                                  @ return on success
    DELIVER_PENDING_EXCEPTION

    .global art_initialize_type_and_verify_access_from_code
    .extern artInitializeTypeAndVerifyAccessFromCode
    /*
     * Entry from managed code when type_idx needs to be checked for access and dex cache may also
     * miss.
     */
    ALIGN_FUNCTION_ENTRY
art_initialize_type_and_verify_access_from_code:
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME           @ save callee saves in case of GC
    mov    r2, r9                              @ pass Thread::Current
    mov    r3, sp                              @ pass SP
    @ artInitializeTypeAndVerifyAccessFromCode(uint32_t type_idx, Method* referrer, Thread*, SP)
    bl     artInitializeTypeAndVerifyAccessFromCode
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME
    cmp    r0, #0                              @ success if result is non-null
    bxne   lr                                  @ return on success
    DELIVER_PENDING_EXCEPTION

    .global art_get32_static_from_code
    .extern artGet32StaticFromCode
    /*
     * Called by managed code to resolve a static field and load a 32-bit primitive value.
     */
    ALIGN_FUNCTION_ENTRY
art_get32_static_from_code:
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME     @ save callee saves in case of GC
    ldr    r1, [sp, #32]                 @ pass referrer
    mov    r2, r9                        @ pass Thread::Current
    mov    r3, sp                        @ pass SP
    bl     artGet32StaticFromCode        @ (uint32_t field_idx, const Method* referrer, Thread*, SP)
    ldr    r12, [r9, #THREAD_EXCEPTION_OFFSET]  @ load Thread::Current()->exception_
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME
    cmp    r12, #0                       @ success if no exception is pending
    bxeq   lr                            @ return on success
    DELIVER_PENDING_EXCEPTION

    .global art_get64_static_from_code
    .extern artGet64StaticFromCode
    /*
     * Called by managed code to resolve a static field and load a 64-bit primitive value.
     */
    ALIGN_FUNCTION_ENTRY
art_get64_static_from_code:
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME     @ save callee saves in case of GC
    ldr    r1, [sp, #32]                 @ pass referrer
    mov    r2, r9                        @ pass Thread::Current
    mov    r3, sp                        @ pass SP
    bl     artGet64StaticFromCode        @ (uint32_t field_idx, const Method* referrer, Thread*, SP)
    ldr    r12, [r9, #THREAD_EXCEPTION_OFFSET]  @ load Thread::Current()->exception_
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME
    cmp    r12, #0                       @ success if no exception is pending
    bxeq    lr                           @ return on success
    DELIVER_PENDING_EXCEPTION

    .global art_get_obj_static_from_code
    .extern artGetObjStaticFromCode
    /*
     * Called by managed code to resolve a static field and load an object reference.
     */
    ALIGN_FUNCTION_ENTRY
art_get_obj_static_from_code:
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME     @ save callee saves in case of GC
    ldr    r1, [sp, #32]                 @ pass referrer
    mov    r2, r9                        @ pass Thread::Current
    mov    r3, sp                        @ pass SP
    bl     artGetObjStaticFromCode       @ (uint32_t field_idx, const Method* referrer, Thread*, SP)
    ldr    r12, [r9, #THREAD_EXCEPTION_OFFSET]  @ load Thread::Current()->exception_
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME
    cmp    r12, #0                       @ success if no exception is pending
    bxeq   lr                            @ return on success
    DELIVER_PENDING_EXCEPTION

    .global art_get32_instance_from_code
    .extern artGet32InstanceFromCode
    /*
     * Called by managed code to resolve an instance field and load a 32-bit primitive value.
     */
    ALIGN_FUNCTION_ENTRY
art_get32_instance_from_code:
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME     @ save callee saves in case of GC
    ldr    r2, [sp, #32]                 @ pass referrer
    mov    r3, r9                        @ pass Thread::Current
    mov    r12, sp
    str    r12, [sp, #-16]!              @ expand the frame and pass SP
    bl     artGet32InstanceFromCode      @ (field_idx, Object*, referrer, Thread*, SP)
    add    sp, #16                       @ strip the extra frame
    ldr    r12, [r9, #THREAD_EXCEPTION_OFFSET]  @ load Thread::Current()->exception_
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME
    cmp    r12, #0                       @ success if no exception is pending
    bxeq   lr                            @ return on success
    DELIVER_PENDING_EXCEPTION

    .global art_get64_instance_from_code
    .extern artGet64InstanceFromCode
    /*
     * Called by managed code to resolve an instance field and load a 64-bit primitive value.
     */
    ALIGN_FUNCTION_ENTRY
art_get64_instance_from_code:
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME     @ save callee saves in case of GC
    ldr    r2, [sp, #32]                 @ pass referrer
    mov    r3, r9                        @ pass Thread::Current
    mov    r12, sp
    str    r12, [sp, #-16]!              @ expand the frame and pass SP
    bl     artGet64InstanceFromCode      @ (field_idx, Object*, referrer, Thread*, SP)
    add    sp, #16                       @ strip the extra frame
    ldr    r12, [r9, #THREAD_EXCEPTION_OFFSET]  @ load Thread::Current()->exception_
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME
    cmp    r12, #0                       @ success if no exception is pending
    bxeq    lr                           @ return on success
    DELIVER_PENDING_EXCEPTION

    .global art_get_obj_instance_from_code
    .extern artGetObjInstanceFromCode
    /*
     * Called by managed code to resolve an instance field and load an object reference.
     */
    ALIGN_FUNCTION_ENTRY
art_get_obj_instance_from_code:
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME     @ save callee saves in case of GC
    ldr    r2, [sp, #32]                 @ pass referrer
    mov    r3, r9                        @ pass Thread::Current
    mov    r12, sp
    str    r12, [sp, #-16]!              @ expand the frame and pass SP
    bl     artGetObjInstanceFromCode     @ (field_idx, Object*, referrer, Thread*, SP)
    add    sp, #16                       @ strip the extra frame
    ldr    r12, [r9, #THREAD_EXCEPTION_OFFSET]  @ load Thread::Current()->exception_
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME
    cmp    r12, #0                       @ success if no exception is pending
    bxeq   lr                            @ return on success
    DELIVER_PENDING_EXCEPTION

    .global art_set32_static_from_code
    .extern artSet32StaticFromCode
    /*
     * Called by managed code to resolve a static field and store a 32-bit primitive value.
     */
    ALIGN_FUNCTION_ENTRY
art_set32_static_from_code:
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME     @ save callee saves in case of GC
    ldr    r2, [sp, #32]                 @ pass referrer
    mov    r3, r9                        @ pass Thread::Current
    mov    r12, sp
    str    r12, [sp, #-16]!              @ expand the frame and pass SP
    bl     artSet32StaticFromCode        @ (field_idx, new_val, referrer, Thread*, SP)
    add    sp, #16                       @ strip the extra frame
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME
    cmp    r0, #0                        @ success if result is 0
    bxeq   lr                            @ return on success
    DELIVER_PENDING_EXCEPTION

    .global art_set64_static_from_code
    .extern artSet64StaticFromCode
    /*
     * Called by managed code to resolve a static field and store a 64-bit primitive value.
     * On entry r0 holds field index, r1:r2 hold new_val
     */
    ALIGN_FUNCTION_ENTRY
art_set64_static_from_code:
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME     @ save callee saves in case of GC
    mov    r3, r2                        @ pass one half of wide argument
    mov    r2, r1                        @ pass other half of wide argument
    ldr    r1, [sp, #32]                 @ pass referrer
    mov    r12, sp                       @ save SP
    sub    sp, #8                        @ grow frame for alignment with stack args
    push   {r9, r12}                     @ pass Thread::Current and SP
    bl     artSet64StaticFromCode        @ (field_idx, referrer, new_val, Thread*, SP)
    add    sp, #16                       @ release out args
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME   @ TODO: we can clearly save an add here
    cmp    r0, #0                        @ success if result is 0
    bxeq   lr                            @ return on success
    DELIVER_PENDING_EXCEPTION

    .global art_set_obj_static_from_code
    .extern artSetObjStaticFromCode
    /*
     * Called by managed code to resolve a static field and store an object reference.
     */
    ALIGN_FUNCTION_ENTRY
art_set_obj_static_from_code:
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME     @ save callee saves in case of GC
    ldr    r2, [sp, #32]                 @ pass referrer
    mov    r3, r9                        @ pass Thread::Current
    mov    r12, sp
    str    r12, [sp, #-16]!              @ expand the frame and pass SP
    bl     artSetObjStaticFromCode       @ (field_idx, new_val, referrer, Thread*, SP)
    add    sp, #16                       @ strip the extra frame
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME
    cmp    r0, #0                        @ success if result is 0
    bxeq   lr                            @ return on success
    DELIVER_PENDING_EXCEPTION

    .global art_set32_instance_from_code
    .extern artSet32InstanceFromCode
    /*
     * Called by managed code to resolve an instance field and store a 32-bit primitive value.
     */
    ALIGN_FUNCTION_ENTRY
art_set32_instance_from_code:
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME     @ save callee saves in case of GC
    ldr    r3, [sp, #32]                 @ pass referrer
    mov    r12, sp                       @ save SP
    sub    sp, #8                        @ grow frame for alignment with stack args
    push   {r9, r12}                     @ pass Thread::Current and SP
    bl     artSet32InstanceFromCode      @ (field_idx, Object*, new_val, referrer, Thread*, SP)
    add    sp, #16                       @ release out args
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME   @ TODO: we can clearly save an add here
    cmp    r0, #0                        @ success if result is 0
    bxeq   lr                            @ return on success
    DELIVER_PENDING_EXCEPTION

    .global art_set64_instance_from_code
    .extern artSet32InstanceFromCode
    /*
     * Called by managed code to resolve an instance field and store a 64-bit primitive value.
     */
    ALIGN_FUNCTION_ENTRY
art_set64_instance_from_code:
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME     @ save callee saves in case of GC
    mov    r12, sp                       @ save SP
    sub    sp, #8                        @ grow frame for alignment with stack args
    push   {r9, r12}                     @ pass Thread::Current and SP
    bl     artSet64InstanceFromCode      @ (field_idx, Object*, new_val, Thread*, SP)
    add    sp, #16                       @ release out args
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME   @ TODO: we can clearly save an add here
    cmp    r0, #0                        @ success if result is 0
    bxeq   lr                            @ return on success
    DELIVER_PENDING_EXCEPTION

    .global art_set_obj_instance_from_code
    .extern artSetObjInstanceFromCode
    /*
     * Called by managed code to resolve an instance field and store an object reference.
     */
    ALIGN_FUNCTION_ENTRY
art_set_obj_instance_from_code:
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME     @ save callee saves in case of GC
    ldr    r3, [sp, #32]                 @ pass referrer
    mov    r12, sp                       @ save SP
    sub    sp, #8                        @ grow frame for alignment with stack args
    push   {r9, r12}                     @ pass Thread::Current and SP
    bl     artSetObjInstanceFromCode     @ (field_idx, Object*, new_val, referrer, Thread*, SP)
    add    sp, #16                       @ release out args
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME   @ TODO: we can clearly save an add here
    cmp    r0, #0                        @ success if result is 0
    bxeq   lr                            @ return on success
    DELIVER_PENDING_EXCEPTION

    .global art_resolve_string_from_code
    .extern artResolveStringFromCode
    /*
     * Entry from managed code to resolve a string, this stub will allocate a String and deliver an
     * exception on error. On success the String is returned. R0 holds the referring method,
     * R1 holds the string index. The fast path check for hit in strings cache has already been
     * performed.
     */
    ALIGN_FUNCTION_ENTRY
art_resolve_string_from_code:
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME  @ save callee saves in case of GC
    mov    r2, r9                     @ pass Thread::Current
    mov    r3, sp                     @ pass SP
    @ artResolveStringFromCode(Method* referrer, uint32_t string_idx, Thread*, SP)
    bl     artResolveStringFromCode
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME
    cmp    r0, #0                     @ success if result is non-null
    bxne   lr                         @ return on success
    DELIVER_PENDING_EXCEPTION

    .global art_alloc_object_from_code
    .extern artAllocObjectFromCode
    /*
     * Called by managed code to allocate an object
     */
    ALIGN_FUNCTION_ENTRY
art_alloc_object_from_code:
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME  @ save callee saves in case of GC
    mov    r2, r9                     @ pass Thread::Current
    mov    r3, sp                     @ pass SP
    bl     artAllocObjectFromCode     @ (uint32_t type_idx, Method* method, Thread*, SP)
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME
    cmp    r0, #0                     @ success if result is non-null
    bxne   lr                         @ return on success
    DELIVER_PENDING_EXCEPTION

    .global art_alloc_object_from_code_with_access_check
    .extern artAllocObjectFromCodeWithAccessCheck
    /*
     * Called by managed code to allocate an object when the caller doesn't know whether it has
     * access to the created type.
     */
    ALIGN_FUNCTION_ENTRY
art_alloc_object_from_code_with_access_check:
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME  @ save callee saves in case of GC
    mov    r2, r9                     @ pass Thread::Current
    mov    r3, sp                     @ pass SP
    bl     artAllocObjectFromCodeWithAccessCheck  @ (uint32_t type_idx, Method* method, Thread*, SP)
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME
    cmp    r0, #0                     @ success if result is non-null
    bxne   lr                         @ return on success
    DELIVER_PENDING_EXCEPTION

    .global art_alloc_array_from_code
    .extern artAllocArrayFromCode
    /*
     * Called by managed code to allocate an array.
     */
    ALIGN_FUNCTION_ENTRY
art_alloc_array_from_code:
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME  @ save callee saves in case of GC
    mov    r3, r9                     @ pass Thread::Current
    mov    r12, sp
    str    r12, [sp, #-16]!           @ expand the frame and pass SP
    @ artAllocArrayFromCode(uint32_t type_idx, Method* method, int32_t component_count, Thread*, SP)
    bl     artAllocArrayFromCode
    add    sp, #16                    @ strip the extra frame
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME
    cmp    r0, #0                     @ success if result is non-null
    bxne   lr                         @ return on success
    DELIVER_PENDING_EXCEPTION

    .global art_alloc_array_from_code_with_access_check
    .extern artAllocArrayFromCodeWithAccessCheck
    /*
     * Called by managed code to allocate an array when the caller doesn't know whether it has
     * access to the created type.
     */
    ALIGN_FUNCTION_ENTRY
art_alloc_array_from_code_with_access_check:
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME  @ save callee saves in case of GC
    mov    r3, r9                     @ pass Thread::Current
    mov    r12, sp
    str    r12, [sp, #-16]!           @ expand the frame and pass SP
    @ artAllocArrayFromCodeWithAccessCheck(type_idx, method, component_count, Thread*, SP)
    bl     artAllocArrayFromCodeWithAccessCheck
    add    sp, #16                    @ strip the extra frame
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME
    cmp    r0, #0                     @ success if result is non-null
    bxne   lr                         @ return on success
    DELIVER_PENDING_EXCEPTION

    .global art_check_and_alloc_array_from_code
    .extern artCheckAndAllocArrayFromCode
    /*
     * Called by managed code to allocate an array in a special case for FILLED_NEW_ARRAY.
     */
    ALIGN_FUNCTION_ENTRY
art_check_and_alloc_array_from_code:
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME  @ save callee saves in case of GC
    mov    r3, r9                     @ pass Thread::Current
    mov    r12, sp
    str    r12, [sp, #-16]!           @ expand the frame and pass SP
    @ artCheckAndAllocArrayFromCode(uint32_t type_idx, Method* method, int32_t count, Thread* , SP)
    bl     artCheckAndAllocArrayFromCode
    add    sp, #16                    @ strip the extra frame
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME
    cmp    r0, #0                     @ success if result is non-null
    bxne   lr                         @ return on success
    DELIVER_PENDING_EXCEPTION

    .global art_check_and_alloc_array_from_code_with_access_check
    .extern artCheckAndAllocArrayFromCodeWithAccessCheck
    /*
     * Called by managed code to allocate an array in a special case for FILLED_NEW_ARRAY.
     */
    ALIGN_FUNCTION_ENTRY
art_check_and_alloc_array_from_code_with_access_check:
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME  @ save callee saves in case of GC
    mov    r3, r9                     @ pass Thread::Current
    mov    r12, sp
    str    r12, [sp, #-16]!           @ expand the frame and pass SP
    @ artCheckAndAllocArrayFromCodeWithAccessCheck(type_idx, method, count, Thread* , SP)
    bl     artCheckAndAllocArrayFromCodeWithAccessCheck
    add    sp, #16                    @ strip the extra frame
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME
    cmp    r0, #0                     @ success if result is non-null
    bxne   lr                         @ return on success
    DELIVER_PENDING_EXCEPTION

    .global art_test_suspend
    .extern artTestSuspendFromCode
    /*
     * Called by managed code when the value in rSUSPEND has been decremented to 0.
     */
    ALIGN_FUNCTION_ENTRY
art_test_suspend:
    ldr    r0, [rSELF, #THREAD_SUSPEND_COUNT_OFFSET]
    mov    rSUSPEND, #SUSPEND_CHECK_INTERVAL  @ reset rSUSPEND to SUSPEND_CHECK_INTERVAL
    cmp    r0, #0                             @ check Thread::Current()->suspend_count_ == 0
    bxeq   rLR                                @ return if suspend_count_ == 0
    mov    r0, rSELF
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME          @ save callee saves for stack crawl
    mov    r1, sp
    bl     artTestSuspendFromCode             @ (Thread*, SP)
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME_AND_RETURN

    .global art_proxy_invoke_handler
    .extern artProxyInvokeHandler
    /*
     * Called by managed code that is attempting to call a method on a proxy class. On entry
     * r0 holds the proxy method; r1, r2 and r3 may contain arguments.
     */
    ALIGN_FUNCTION_ENTRY
art_proxy_invoke_handler:
    SETUP_REF_AND_ARGS_CALLEE_SAVE_FRAME
    str     r0, [sp, #0]           @ place proxy method at bottom of frame
    mov     r2, r9                 @ pass Thread::Current
    add     r3, sp, #12            @ pointer to r2/r3/LR/caller's Method**/out-args as second arg
    blx     artProxyInvokeHandler  @ (Method* proxy method, receiver, Thread*, args...)
    ldr     r12, [r9, #THREAD_EXCEPTION_OFFSET]  @ load Thread::Current()->exception_
    ldr     lr,  [sp, #44]         @ restore lr
    ldrd    r0,  [sp, #12]         @ load r0/r1 from r2/r3 that were overwritten with the out args
    add     sp,  #48               @ pop frame
    cmp     r12, #0                @ success if no exception is pending
    bxeq    lr                     @ return on success
    DELIVER_PENDING_EXCEPTION

    .global art_trace_entry_from_code
    .global art_trace_exit_from_code
    .extern artTraceMethodEntryFromCode
    .extern artTraceMethodExitFromCode
    /*
     * Routine that intercepts method calls and returns.
     */
    ALIGN_FUNCTION_ENTRY
art_trace_entry_from_code:
    push  {r0-r3}        @ save arguments (4 words)
    mov   r1, r9         @ pass Thread::Current
    mov   r2, lr         @ pass LR
    blx   artTraceMethodEntryFromCode  @ (Method*, Thread*, LR)
    mov   r12, r0        @ r12 holds reference to code
    pop   {r0-r3}        @ restore arguments
    blx   r12            @ call method
art_trace_exit_from_code:
    push  {r0-r1}        @ save return value
    blx   artTraceMethodExitFromCode  @ ()
    mov   lr, r0         @ restore link register
    pop   {r0, r1}       @ restore return value
    bx    lr             @ return

    .global art_shl_long
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
    ALIGN_FUNCTION_ENTRY
art_shl_long:
    and     r2, r2, #63                 @ r2<- r2 & 0x3f
    mov     r1, r1, asl r2              @  r1<- r1 << r2
    rsb     r3, r2, #32                 @  r3<- 32 - r2
    orr     r1, r1, r0, lsr r3          @  r1<- r1 | (r0 << (32-r2))
    subs    ip, r2, #32                 @  ip<- r2 - 32
    movpl   r1, r0, asl ip              @  if r2 >= 32, r1<- r0 << (r2-32)
    mov     r0, r0, asl r2              @  r0<- r0 << r2
    bx      lr

    .global art_shr_long
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
    ALIGN_FUNCTION_ENTRY
art_shr_long:
    and     r2, r2, #63                 @ r0<- r0 & 0x3f
    mov     r0, r0, lsr r2              @  r0<- r2 >> r2
    rsb     r3, r2, #32                 @  r3<- 32 - r2
    orr     r0, r0, r1, asl r3          @  r0<- r0 | (r1 << (32-r2))
    subs    ip, r2, #32                 @  ip<- r2 - 32
    movpl   r0, r1, asr ip              @  if r2 >= 32, r0<-r1 >> (r2-32)
    mov     r1, r1, asr r2              @  r1<- r1 >> r2
    bx      lr

    .global art_ushr_long
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
    ALIGN_FUNCTION_ENTRY
art_ushr_long:
    and     r2, r2, #63                 @ r0<- r0 & 0x3f
    mov     r0, r0, lsr r2              @  r0<- r2 >> r2
    rsb     r3, r2, #32                 @  r3<- 32 - r2
    orr     r0, r0, r1, asl r3          @  r0<- r0 | (r1 << (32-r2))
    subs    ip, r2, #32                 @  ip<- r2 - 32
    movpl   r0, r1, lsr ip              @  if r2 >= 32, r0<-r1 >>> (r2-32)
    mov     r1, r1, lsr r2              @  r1<- r1 >>> r2
    bx      lr

    .balign 4
    .global art_indexof
art_indexof:
    /*
     * String's indexOf.
     *
     * On entry:
     *    r0:   string object (known non-null)
     *    r1:   char to match (known <= 0xFFFF)
     *    r2:   Starting offset in string data
     */

    push {r4, r10-r11, lr} @ 4 words of callee saves
    ldr   r3, [r0, #STRING_COUNT_OFFSET]
    ldr   r12, [r0, #STRING_OFFSET_OFFSET]
    ldr   r0, [r0, #STRING_VALUE_OFFSET]

    /* Clamp start to [0..count] */
    cmp   r2, #0
    movlt r2, #0
    cmp   r2, r3
    movgt r2, r3

    /* Build a pointer to the start of string data */
    add   r0, #STRING_DATA_OFFSET
    add   r0, r0, r12, lsl #1

    /* Save a copy in r12 to later compute result */
    mov   r12, r0

    /* Build pointer to start of data to compare and pre-bias */
    add   r0, r0, r2, lsl #1
    sub   r0, #2

    /* Compute iteration count */
    sub   r2, r3, r2

    /*
     * At this point we have:
     *   r0: start of data to test
     *   r1: char to compare
     *   r2: iteration count
     *   r12: original start of string data
     *   r3, r4, r10, r11 available for loading string data
     */

    subs  r2, #4
    blt   indexof_remainder

indexof_loop4:
    ldrh  r3, [r0, #2]!
    ldrh  r4, [r0, #2]!
    ldrh  r10, [r0, #2]!
    ldrh  r11, [r0, #2]!
    cmp   r3, r1
    beq   match_0
    cmp   r4, r1
    beq   match_1
    cmp   r10, r1
    beq   match_2
    cmp   r11, r1
    beq   match_3
    subs  r2, #4
    bge   indexof_loop4

indexof_remainder:
    adds    r2, #4
    beq     indexof_nomatch

indexof_loop1:
    ldrh  r3, [r0, #2]!
    cmp   r3, r1
    beq   match_3
    subs  r2, #1
    bne   indexof_loop1

indexof_nomatch:
    mov   r0, #-1
    pop {r4, r10-r11, pc}

match_0:
    sub   r0, #6
    sub   r0, r12
    asr   r0, r0, #1
    pop {r4, r10-r11, pc}
match_1:
    sub   r0, #4
    sub   r0, r12
    asr   r0, r0, #1
    pop {r4, r10-r11, pc}
match_2:
    sub   r0, #2
    sub   r0, r12
    asr   r0, r0, #1
    pop {r4, r10-r11, pc}
match_3:
    sub   r0, r12
    asr   r0, r0, #1
    pop {r4, r10-r11, pc}


   /*
     * String's compareTo.
     *
     * Requires rARG0/rARG1 to have been previously checked for null.  Will
     * return negative if this's string is < comp, 0 if they are the
     * same and positive if >.
     *
     * On entry:
     *    r0:   this object pointer
     *    r1:   comp object pointer
     *
     */

    .balign 4
    .global art_string_compareto
    .extern __memcmp16
art_string_compareto:
    mov    r2, r0         @ this to r2, opening up r0 for return value
    subs   r0, r2, r1     @ Same?
    bxeq   lr

    push {r4, r7-r12, lr} @ 8 words - keep alignment

    ldr    r4, [r2, #STRING_OFFSET_OFFSET]
    ldr    r9, [r1, #STRING_OFFSET_OFFSET]
    ldr    r7, [r2, #STRING_COUNT_OFFSET]
    ldr    r10, [r1, #STRING_COUNT_OFFSET]
    ldr    r2, [r2, #STRING_VALUE_OFFSET]
    ldr    r1, [r1, #STRING_VALUE_OFFSET]

    /*
     * At this point, we have:
     *    value:  r2/r1
     *    offset: r4/r9
     *    count:  r7/r10
     * We're going to compute
     *    r11 <- countDiff
     *    r10 <- minCount
     */
     subs  r11, r7, r10
     movls r10, r7

     /* Now, build pointers to the string data */
     add   r2, r2, r4, lsl #1
     add   r1, r1, r9, lsl #1
     /*
      * Note: data pointers point to previous element so we can use pre-index
      * mode with base writeback.
      */
     add   r2, #STRING_DATA_OFFSET-2   @ offset to contents[-1]
     add   r1, #STRING_DATA_OFFSET-2   @ offset to contents[-1]

     /*
      * At this point we have:
      *   r2: *this string data
      *   r1: *comp string data
      *   r10: iteration count for comparison
      *   r11: value to return if the first part of the string is equal
      *   r0: reserved for result
      *   r3, r4, r7, r8, r9, r12 available for loading string data
      */

    subs  r10, #2
    blt   do_remainder2

      /*
       * Unroll the first two checks so we can quickly catch early mismatch
       * on long strings (but preserve incoming alignment)
       */

    ldrh  r3, [r2, #2]!
    ldrh  r4, [r1, #2]!
    ldrh  r7, [r2, #2]!
    ldrh  r8, [r1, #2]!
    subs  r0, r3, r4
    subeqs  r0, r7, r8
    bne   done
    cmp   r10, #28
    bgt   do_memcmp16
    subs  r10, #3
    blt   do_remainder

loopback_triple:
    ldrh  r3, [r2, #2]!
    ldrh  r4, [r1, #2]!
    ldrh  r7, [r2, #2]!
    ldrh  r8, [r1, #2]!
    ldrh  r9, [r2, #2]!
    ldrh  r12,[r1, #2]!
    subs  r0, r3, r4
    subeqs  r0, r7, r8
    subeqs  r0, r9, r12
    bne   done
    subs  r10, #3
    bge   loopback_triple

do_remainder:
    adds  r10, #3
    beq   returnDiff

loopback_single:
    ldrh  r3, [r2, #2]!
    ldrh  r4, [r1, #2]!
    subs  r0, r3, r4
    bne   done
    subs  r10, #1
    bne     loopback_single

returnDiff:
    mov   r0, r11
    pop   {r4, r7-r12, pc}

do_remainder2:
    adds  r10, #2
    bne   loopback_single
    mov   r0, r11
    pop   {r4, r7-r12, pc}

    /* Long string case */
do_memcmp16:
    mov   r7, r11
    add   r0, r2, #2
    add   r1, r1, #2
    mov   r2, r10
    bl    __memcmp16
    cmp   r0, #0
    moveq r0, r7
done:
    pop   {r4, r7-r12, pc}
