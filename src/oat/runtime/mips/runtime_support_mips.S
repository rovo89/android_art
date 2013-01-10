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

    .set noreorder
    .balign 4

    /* Deliver the given exception */
    .extern artDeliverExceptionFromCode
    /* Deliver an exception pending on a thread */
    .extern artDeliverPendingExceptionFromCode

    /* Cache alignment for function entry */
.macro ALIGN_FUNCTION_ENTRY
    .balign 16
.endm

    /* Generates $gp for function calls */
.macro GENERATE_GLOBAL_POINTER
    .cpload $t9
.endm

    /*
     * Macro that sets up the callee save frame to conform with
     * Runtime::CreateCalleeSaveMethod(kSaveAll)
     * callee-save: $s0-$s8 + $ra, 10 total + 4 words
     */
.macro SETUP_SAVE_ALL_CALLEE_SAVE_FRAME
    addiu  $sp, $sp, -64
    sw     $ra, 60($sp)
    sw     $s8, 56($sp)
    sw     $s7, 52($sp)
    sw     $s6, 48($sp)
    sw     $s5, 44($sp)
    sw     $s4, 40($sp)
    sw     $s3, 36($sp)
    sw     $s2, 32($sp)
    sw     $s1, 28($sp)
    sw     $s0, 24($sp)
    # 2 words for alignment, 4 open words for args $a0-$a3, bottom will hold Method*
.endm

    /*
     * Macro that sets up the callee save frame to conform with
     * Runtime::CreateCalleeSaveMethod(kRefsOnly). Restoration assumes non-moving GC.
     * Does not include rSUSPEND or rSELF
     * callee-save: $s2-$s8 + $ra, 8 total + 4 words + extra args + gp
     */
.macro SETUP_REF_ONLY_CALLEE_SAVE_FRAME
    addiu  $sp, $sp, -64
    sw     $ra, 60($sp)
    sw     $s8, 56($sp)
    sw     $s7, 52($sp)
    sw     $s6, 48($sp)
    sw     $s5, 44($sp)
    sw     $s4, 40($sp)
    sw     $s3, 36($sp)
    sw     $s2, 32($sp)
    sw     $gp, 28($sp)
    # 3 words for alignment and extra args, 4 open words for args $a0-$a3, bottom will hold Method*
.endm

.macro RESTORE_REF_ONLY_CALLEE_SAVE_FRAME
    lw     $gp, 28($sp)
    lw     $ra, 60($sp)
    addiu  $sp, $sp, 64
.endm

.macro RESTORE_REF_ONLY_CALLEE_SAVE_FRAME_AND_RETURN
    lw     $ra, 60($sp)
    jr     $ra
    addiu  $sp, $sp, 64
.endm

    /*
     * Macro that sets up the callee save frame to conform with
     * Runtime::CreateCalleeSaveMethod(kRefsAndArgs). Restoration assumes non-moving GC.
     * $a1-$a3, $s2-$s8, $ra, 11 total + Method*
     */
.macro SETUP_REF_AND_ARGS_CALLEE_SAVE_FRAME
    addiu  $sp, $sp, -48
    sw     $ra, 44($sp)
    sw     $s8, 40($sp)
    sw     $s7, 36($sp)
    sw     $s6, 32($sp)
    sw     $s5, 28($sp)
    sw     $s4, 24($sp)
    sw     $s3, 20($sp)
    sw     $s2, 16($sp)
    sw     $a3, 12($sp)
    sw     $a2, 8($sp)
    sw     $a1, 4($sp)
    # bottom will hold Method*
.endm

.macro RESTORE_REF_AND_ARGS_CALLEE_SAVE_FRAME
    lw     $ra, 44($sp)           # restore $ra
    lw     $a1, 4($sp)            # restore non-callee save $a1
    lw     $a2, 8($sp)            # restore non-callee save $a2
    lw     $a3, 12($sp)           # restore non-callee save $a3
    addiu  $sp, $sp, 48           # strip frame
.endm

    /*
     * Macro that set calls through to artDeliverPendingExceptionFromCode, where the pending
     * exception is Thread::Current()->exception_
     */
.macro DELIVER_PENDING_EXCEPTION
    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME     # save callee saves for throw
    move    $a0, rSELF                   # pass Thread::Current
    la      $t9, artDeliverPendingExceptionFromCode
    jr      $t9                          # artDeliverPendingExceptionFromCode(Thread*, $sp)
    move    $a1, $sp                     # pass $sp
.endm

.macro RETURN_IF_NO_EXCEPTION
    lw     $t0, THREAD_EXCEPTION_OFFSET(rSELF) # load Thread::Current()->exception_
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME
    bnez   $t0, 1f                       # success if no exception is pending
    nop
    jr     $ra
    nop
1:
    DELIVER_PENDING_EXCEPTION
.endm

.macro RETURN_IF_ZERO
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME
    bnez   $v0, 1f                       # success?
    nop
    jr     $ra                           # return on success
    nop
1:
    DELIVER_PENDING_EXCEPTION
.endm

.macro RETURN_IF_NONZERO
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME
    beqz   $v0, 1f                       # success?
    nop
    jr     $ra                           # return on success
    nop
1:
    DELIVER_PENDING_EXCEPTION
.endm

    .global art_update_debugger
    .extern artUpdateDebuggerFromCode
    /*
     * On entry, $a0 and $a1 must be preserved, $a2 is dex PC
     */
    ALIGN_FUNCTION_ENTRY
art_update_debugger:
    GENERATE_GLOBAL_POINTER
    move    $a3, $a0        # stash away $a0 so that it's saved as if it were an argument
    SETUP_REF_AND_ARGS_CALLEE_SAVE_FRAME
    move    $a0, $a2        # arg0 is dex PC
    move    $a1, rSELF      # arg1 is Thread*
    move    $a2, $sp        # arg2 is $sp
    jal     artUpdateDebuggerFromCode      # artUpdateDebuggerFromCode(int32_t, Thread*, Method**)
    nop
    RESTORE_REF_AND_ARGS_CALLEE_SAVE_FRAME
    jr      $ra
    move    $a0, $a3        # restore original $a0

    .global art_do_long_jump
    /*
     * On entry $a0 is uint32_t* gprs_ and $a1 is uint32_t* fprs_
     * FIXME: just guessing about the shape of the jmpbuf.  Where will pc be?
     */
    ALIGN_FUNCTION_ENTRY
art_do_long_jump:
    l.s     $f0, 0($a1)
    l.s     $f1, 4($a1)
    l.s     $f2, 8($a1)
    l.s     $f3, 12($a1)
    l.s     $f4, 16($a1)
    l.s     $f5, 20($a1)
    l.s     $f6, 24($a1)
    l.s     $f7, 28($a1)
    l.s     $f8, 32($a1)
    l.s     $f9, 36($a1)
    l.s     $f10, 40($a1)
    l.s     $f11, 44($a1)
    l.s     $f12, 48($a1)
    l.s     $f13, 52($a1)
    l.s     $f14, 56($a1)
    l.s     $f15, 60($a1)
    l.s     $f16, 64($a1)
    l.s     $f17, 68($a1)
    l.s     $f18, 72($a1)
    l.s     $f19, 76($a1)
    l.s     $f20, 80($a1)
    l.s     $f21, 84($a1)
    l.s     $f22, 88($a1)
    l.s     $f23, 92($a1)
    l.s     $f24, 96($a1)
    l.s     $f25, 100($a1)
    l.s     $f26, 104($a1)
    l.s     $f27, 108($a1)
    l.s     $f28, 112($a1)
    l.s     $f29, 116($a1)
    l.s     $f30, 120($a1)
    l.s     $f31, 124($a1)
    lw      $at, 4($a0)
    lw      $v0, 8($a0)
    lw      $v1, 12($a0)
    lw      $a1, 20($a0)
    lw      $a2, 24($a0)
    lw      $a3, 28($a0)
    lw      $t0, 32($a0)
    lw      $t1, 36($a0)
    lw      $t2, 40($a0)
    lw      $t3, 44($a0)
    lw      $t4, 48($a0)
    lw      $t5, 52($a0)
    lw      $t6, 56($a0)
    lw      $t7, 60($a0)
    lw      $s0, 64($a0)
    lw      $s1, 68($a0)
    lw      $s2, 72($a0)
    lw      $s3, 76($a0)
    lw      $s4, 80($a0)
    lw      $s5, 84($a0)
    lw      $s6, 88($a0)
    lw      $s7, 92($a0)
    lw      $t8, 96($a0)
    lw      $t9, 100($a0)
    lw      $k0, 104($a0)
    lw      $k1, 108($a0)
    lw      $gp, 112($a0)
    lw      $sp, 116($a0)
    lw      $fp, 120($a0)
    lw      $ra, 124($a0)
    lw      $a0, 16($a0)
    move    $v0, $zero          # clear result registers r0 and r1
    jr      $ra                 # do long jump
    move    $v1, $zero

    .global art_deliver_exception_from_code
    /*
     * Called by managed code, saves most registers (forms basis of long jump context) and passes
     * the bottom of the stack. artDeliverExceptionFromCode will place the callee save Method* at
     * the bottom of the thread. On entry r0 holds Throwable*
     */
    ALIGN_FUNCTION_ENTRY
art_deliver_exception_from_code:
    GENERATE_GLOBAL_POINTER
    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME
    move $a1, rSELF                 # pass Thread::Current
    la   $t9, artDeliverExceptionFromCode
    jr   $t9                        # artDeliverExceptionFromCode(Throwable*, Thread*, $sp)
    move $a2, $sp                   # pass $sp

    .global art_throw_null_pointer_exception_from_code
    .extern artThrowNullPointerExceptionFromCode
    /*
     * Called by managed code to create and deliver a NullPointerException
     */
    ALIGN_FUNCTION_ENTRY
art_throw_null_pointer_exception_from_code:
    GENERATE_GLOBAL_POINTER
    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME
    move $a0, rSELF                 # pass Thread::Current
    la   $t9, artThrowNullPointerExceptionFromCode
    jr   $t9                        # artThrowNullPointerExceptionFromCode(Thread*, $sp)
    move $a1, $sp                   # pass $sp

    .global art_throw_div_zero_from_code
    .extern artThrowDivZeroFromCode
    /*
     * Called by managed code to create and deliver an ArithmeticException
     */
    ALIGN_FUNCTION_ENTRY
art_throw_div_zero_from_code:
    GENERATE_GLOBAL_POINTER
    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME
    move $a0, rSELF                 # pass Thread::Current
    la   $t9, artThrowDivZeroFromCode
    jr   $t9                        # artThrowDivZeroFromCode(Thread*, $sp)
    move $a1, $sp                   # pass $sp

    .global art_throw_array_bounds_from_code
    .extern artThrowArrayBoundsFromCode
    /*
     * Called by managed code to create and deliver an ArrayIndexOutOfBoundsException
     */
    ALIGN_FUNCTION_ENTRY
art_throw_array_bounds_from_code:
    GENERATE_GLOBAL_POINTER
    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME
    move $a2, rSELF                 # pass Thread::Current
    la   $t9, artThrowArrayBoundsFromCode
    jr   $t9                        # artThrowArrayBoundsFromCode(index, limit, Thread*, $sp)
    move $a3, $sp                   # pass $sp

    .global art_throw_stack_overflow_from_code
    .extern artThrowStackOverflowFromCode
    /*
     * Called by managed code to create and deliver a StackOverflowError.
     */
    ALIGN_FUNCTION_ENTRY
art_throw_stack_overflow_from_code:
    GENERATE_GLOBAL_POINTER
    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME
    move $a0, rSELF                 # pass Thread::Current
    la   $t9, artThrowStackOverflowFromCode
    jr   $t9                        # artThrowStackOverflowFromCode(Thread*, $sp)
    move $a1, $sp                   # pass $sp

    .global art_throw_no_such_method_from_code
    .extern artThrowNoSuchMethodFromCode
    /*
     * Called by managed code to create and deliver a NoSuchMethodError.
     */
    ALIGN_FUNCTION_ENTRY
art_throw_no_such_method_from_code:
    GENERATE_GLOBAL_POINTER
    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME
    move $a1, rSELF                 # pass Thread::Current
    la   $t9, artThrowNoSuchMethodFromCode
    jr   $t9                        # artThrowNoSuchMethodFromCode(method_idx, Thread*, $sp)
    move $a2, $sp                   # pass $sp

    /*
     * All generated callsites for interface invokes and invocation slow paths will load arguments
     * as usual - except instead of loading arg0/$a0 with the target Method*, arg0/$a0 will contain
     * the method_idx.  This wrapper will save arg1-arg3, load the caller's Method*, align the
     * stack and call the appropriate C helper.
     * NOTE: "this" is first visable argument of the target, and so can be found in arg1/$a1.
     *
     * The helper will attempt to locate the target and return a 64-bit result in $v0/$v1 consisting
     * of the target Method* in $v0 and method->code_ in $v1.
     *
     * If unsuccessful, the helper will return NULL/NULL. There will be a pending exception in the
     * thread and we branch to another stub to deliver it.
     *
     * On success this wrapper will restore arguments and *jump* to the target, leaving the lr
     * pointing back to the original caller.
     */
.macro INVOKE_TRAMPOLINE c_name, cxx_name
    .global \c_name
    .extern \cxx_name
\c_name:
    GENERATE_GLOBAL_POINTER
    SETUP_REF_AND_ARGS_CALLEE_SAVE_FRAME  # save callee saves in case allocation triggers GC
    lw    $a2, 48($sp)                    # pass caller Method*
    move  $t0, $sp                        # save $sp
    addiu $sp, $sp, -16                   # make space for extra args
    move  $a3, rSELF                      # pass Thread::Current
    sw    $gp, 12($sp)                    # save $gp
    jal   \cxx_name                       # (method_idx, this, caller, Thread*, $sp)
    sw    $t0, 16($sp)                    # pass $sp
    lw    $gp, 12($sp)                    # restore $gp
    addiu $sp, $sp, 16                    # release out args
    move  $a0, $v0                        # save target Method*
    move  $t9, $v1                        # save $v0->code_
    RESTORE_REF_AND_ARGS_CALLEE_SAVE_FRAME
    beqz  $v0, 1f
    nop
    jr    $t9
    nop
1:
    DELIVER_PENDING_EXCEPTION
.endm

INVOKE_TRAMPOLINE art_invoke_interface_trampoline, artInvokeInterfaceTrampoline
INVOKE_TRAMPOLINE art_invoke_interface_trampoline_with_access_check, artInvokeInterfaceTrampolineWithAccessCheck

INVOKE_TRAMPOLINE art_invoke_static_trampoline_with_access_check, artInvokeStaticTrampolineWithAccessCheck
INVOKE_TRAMPOLINE art_invoke_direct_trampoline_with_access_check, artInvokeDirectTrampolineWithAccessCheck
INVOKE_TRAMPOLINE art_invoke_super_trampoline_with_access_check, artInvokeSuperTrampolineWithAccessCheck
INVOKE_TRAMPOLINE art_invoke_virtual_trampoline_with_access_check, artInvokeVirtualTrampolineWithAccessCheck

    .global art_work_around_app_jni_bugs
    .extern artWorkAroundAppJniBugs
    /*
     * Entry point of native methods when JNI bug compatibility is enabled.
     */
    ALIGN_FUNCTION_ENTRY
art_work_around_app_jni_bugs:
    GENERATE_GLOBAL_POINTER
    # save registers that may contain arguments and LR that will be crushed by a call
    addiu    $sp, $sp, -32
    sw       $ra, 28($sp)
    sw       $a3, 24($sp)
    sw       $a2, 20($sp)
    sw       $a1, 16($sp)
    sw       $a0, 12($sp)
    move     $a0, rSELF       # pass Thread::Current
    jal      artWorkAroundAppJniBugs  # (Thread*, $sp)
    move     $a1, $sp         # pass $sp
    move     $t9, $v0         # save target address
    lw       $a0, 12($sp)
    lw       $a1, 16($sp)
    lw       $a2, 20($sp)
    lw       $a3, 24($sp)
    lw       $ra, 28($sp)
    jr       $t9              # tail call into JNI routine
    addiu    $sp, $sp, 32

    .global art_handle_fill_data_from_code
    .extern artHandleFillArrayDataFromCode
    /*
     * Entry from managed code that calls artHandleFillArrayDataFromCode and delivers exception on
     * failure.
     */
    ALIGN_FUNCTION_ENTRY
art_handle_fill_data_from_code:
    GENERATE_GLOBAL_POINTER
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME  # save callee saves in case exception allocation triggers GC
    move    $a2, rSELF                         # pass Thread::Current
    jal     artHandleFillArrayDataFromCode     # (Array*, const DexFile::Payload*, Thread*, $sp)
    move    $a3, $sp                           # pass $sp
    RETURN_IF_ZERO

    .global art_lock_object_from_code
    .extern artLockObjectFromCode
    /*
     * Entry from managed code that calls artLockObjectFromCode, may block for GC.
     */
    ALIGN_FUNCTION_ENTRY
art_lock_object_from_code:
    GENERATE_GLOBAL_POINTER
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME      # save callee saves in case we block
    move    $a1, rSELF                    # pass Thread::Current
    jal     artLockObjectFromCode         # (Object* obj, Thread*, $sp)
    move    $a2, $sp                      # pass $sp
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME_AND_RETURN

    .global art_unlock_object_from_code
    .extern artUnlockObjectFromCode
    /*
     * Entry from managed code that calls artUnlockObjectFromCode and delivers exception on failure.
     */
    ALIGN_FUNCTION_ENTRY
art_unlock_object_from_code:
    GENERATE_GLOBAL_POINTER
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME  # save callee saves in case exception allocation triggers GC
    move    $a1, rSELF                # pass Thread::Current
    jal     artUnlockObjectFromCode   # (Object* obj, Thread*, $sp)
    move    $a2, $sp                  # pass $sp
    RETURN_IF_ZERO

    .global art_check_cast_from_code
    .extern artCheckCastFromCode
    /*
     * Entry from managed code that calls artCheckCastFromCode and delivers exception on failure.
     */
    ALIGN_FUNCTION_ENTRY
art_check_cast_from_code:
    GENERATE_GLOBAL_POINTER
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME  # save callee saves in case exception allocation triggers GC
    move    $a2, rSELF                # pass Thread::Current
    jal     artCheckCastFromCode      # (Class* a, Class* b, Thread*, $sp)
    move    $a3, $sp                  # pass $sp
    RETURN_IF_ZERO

    .global art_can_put_array_element_from_code
    .extern artCanPutArrayElementFromCode
    /*
     * Entry from managed code that calls artCanPutArrayElementFromCode and delivers exception on
     * failure.
     */
    ALIGN_FUNCTION_ENTRY
art_can_put_array_element_from_code:
    GENERATE_GLOBAL_POINTER
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME    # save callee saves in case exception allocation triggers GC
    move    $a2, rSELF                     # pass Thread::Current
    jal     artCanPutArrayElementFromCode  # (Object* element, Class* array_class, Thread*, $sp)
    move    $a3, $sp                       # pass $sp
    RETURN_IF_ZERO

    .global art_initialize_static_storage_from_code
    .extern artInitializeStaticStorageFromCode
    /*
     * Entry from managed code when uninitialized static storage, this stub will run the class
     * initializer and deliver the exception on error. On success the static storage base is
     * returned.
     */
    ALIGN_FUNCTION_ENTRY
art_initialize_static_storage_from_code:
    GENERATE_GLOBAL_POINTER
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME            # save callee saves in case of GC
    move    $a2, rSELF                          # pass Thread::Current
    # artInitializeStaticStorageFromCode(uint32_t type_idx, Method* referrer, Thread*, $sp)
    jal     artInitializeStaticStorageFromCode
    move    $a3, $sp                            # pass $sp
    RETURN_IF_NONZERO

    .global art_initialize_type_from_code
    .extern artInitializeTypeFromCode
    /*
     * Entry from managed code when dex cache misses for a type_idx.
     */
    ALIGN_FUNCTION_ENTRY
art_initialize_type_from_code:
    GENERATE_GLOBAL_POINTER
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME           # save callee saves in case of GC
    move    $a2, rSELF                         # pass Thread::Current
    # artInitializeTypeFromCode(uint32_t type_idx, Method* referrer, Thread*, $sp)
    jal     artInitializeTypeFromCode
    move    $a3, $sp                           # pass $sp
    RETURN_IF_NONZERO

    .global art_initialize_type_and_verify_access_from_code
    .extern artInitializeTypeAndVerifyAccessFromCode
    /*
     * Entry from managed code when type_idx needs to be checked for access and dex cache may also
     * miss.
     */
    ALIGN_FUNCTION_ENTRY
art_initialize_type_and_verify_access_from_code:
    GENERATE_GLOBAL_POINTER
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME           # save callee saves in case of GC
    move    $a2, rSELF                         # pass Thread::Current
    # artInitializeTypeFromCode(uint32_t type_idx, Method* referrer, Thread*, $sp)
    jal     artInitializeTypeAndVerifyAccessFromCode
    move    $a3, $sp                           # pass $sp
    RETURN_IF_NONZERO

    .global art_get32_static_from_code
    .extern artGet32StaticFromCode
    /*
     * Called by managed code to resolve a static field and load a 32-bit primitive value.
     */
    ALIGN_FUNCTION_ENTRY
art_get32_static_from_code:
    GENERATE_GLOBAL_POINTER
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME     # save callee saves in case of GC
    lw     $a1, 64($sp)                  # pass referrer's Method*
    move   $a2, rSELF                    # pass Thread::Current
    jal    artGet32StaticFromCode        # (uint32_t field_idx, const Method* referrer, Thread*, $sp)
    move   $a3, $sp                      # pass $sp
    RETURN_IF_NO_EXCEPTION

    .global art_get64_static_from_code
    .extern artGet64StaticFromCode
    /*
     * Called by managed code to resolve a static field and load a 64-bit primitive value.
     */
    ALIGN_FUNCTION_ENTRY
art_get64_static_from_code:
    GENERATE_GLOBAL_POINTER
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME     # save callee saves in case of GC
    lw     $a1, 64($sp)                  # pass referrer's Method*
    move   $a2, rSELF                    # pass Thread::Current
    jal    artGet64StaticFromCode        # (uint32_t field_idx, const Method* referrer, Thread*, $sp)
    move   $a3, $sp                      # pass $sp
    RETURN_IF_NO_EXCEPTION

    .global art_get_obj_static_from_code
    .extern artGetObjStaticFromCode
    /*
     * Called by managed code to resolve a static field and load an object reference.
     */
    ALIGN_FUNCTION_ENTRY
art_get_obj_static_from_code:
    GENERATE_GLOBAL_POINTER
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME     # save callee saves in case of GC
    lw     $a1, 64($sp)                  # pass referrer's Method*
    move   $a2, rSELF                    # pass Thread::Current
    jal    artGetObjStaticFromCode       # (uint32_t field_idx, const Method* referrer, Thread*, $sp)
    move   $a3, $sp                      # pass $sp
    RETURN_IF_NO_EXCEPTION

    .global art_get32_instance_from_code
    .extern artGet32InstanceFromCode
    /*
     * Called by managed code to resolve an instance field and load a 32-bit primitive value.
     */
    ALIGN_FUNCTION_ENTRY
art_get32_instance_from_code:
    GENERATE_GLOBAL_POINTER
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME     # save callee saves in case of GC
    lw     $a2, 64($sp)                  # pass referrer's Method*
    move   $a3, rSELF                    # pass Thread::Current
    jal    artGet32InstanceFromCode      # (field_idx, Object*, referrer, Thread*, $sp)
    sw     $sp, 16($sp)                  # pass $sp
    RETURN_IF_NO_EXCEPTION

    .global art_get64_instance_from_code
    .extern artGet64InstanceFromCode
    /*
     * Called by managed code to resolve an instance field and load a 64-bit primitive value.
     */
    ALIGN_FUNCTION_ENTRY
art_get64_instance_from_code:
    GENERATE_GLOBAL_POINTER
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME     # save callee saves in case of GC
    lw     $a2, 64($sp)                  # pass referrer's Method*
    move   $a3, rSELF                    # pass Thread::Current
    jal    artGet64InstanceFromCode      # (field_idx, Object*, referrer, Thread*, $sp)
    sw     $sp, 16($sp)                  # pass $sp
    RETURN_IF_NO_EXCEPTION

    .global art_get_obj_instance_from_code
    .extern artGetObjInstanceFromCode
    /*
     * Called by managed code to resolve an instance field and load an object reference.
     */
    ALIGN_FUNCTION_ENTRY
art_get_obj_instance_from_code:
    GENERATE_GLOBAL_POINTER
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME     # save callee saves in case of GC
    lw     $a2, 64($sp)                  # pass referrer's Method*
    move   $a3, rSELF                    # pass Thread::Current
    jal    artGetObjInstanceFromCode     # (field_idx, Object*, referrer, Thread*, $sp)
    sw     $sp, 16($sp)                  # pass $sp
    RETURN_IF_NO_EXCEPTION

    .global art_set32_static_from_code
    .extern artSet32StaticFromCode
    /*
     * Called by managed code to resolve a static field and store a 32-bit primitive value.
     */
    ALIGN_FUNCTION_ENTRY
art_set32_static_from_code:
    GENERATE_GLOBAL_POINTER
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME     # save callee saves in case of GC
    lw     $a2, 64($sp)                  # pass referrer's Method*
    move   $a3, rSELF                    # pass Thread::Current
    jal    artSet32StaticFromCode        # (field_idx, new_val, referrer, Thread*, $sp)
    sw     $sp, 16($sp)                  # pass $sp
    RETURN_IF_ZERO

    .global art_set64_static_from_code
    .extern artSet32StaticFromCode
    /*
     * Called by managed code to resolve a static field and store a 64-bit primitive value.
     */
    ALIGN_FUNCTION_ENTRY
art_set64_static_from_code:
    GENERATE_GLOBAL_POINTER
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME     # save callee saves in case of GC
    lw     $a1, 64($sp)                  # pass referrer's Method*
    sw     rSELF, 16($sp)                # pass Thread::Current
    jal    artSet64StaticFromCode        # (field_idx, referrer, new_val, Thread*, $sp)
    sw     $sp, 20($sp)                  # pass $sp
    RETURN_IF_ZERO

    .global art_set_obj_static_from_code
    .extern artSetObjStaticFromCode
    /*
     * Called by managed code to resolve a static field and store an object reference.
     */
    ALIGN_FUNCTION_ENTRY
art_set_obj_static_from_code:
    GENERATE_GLOBAL_POINTER
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME     # save callee saves in case of GC
    lw     $a2, 64($sp)                  # pass referrer's Method*
    move   $a3, rSELF                    # pass Thread::Current
    jal    artSetObjStaticFromCode       # (field_idx, new_val, referrer, Thread*, $sp)
    sw     $sp, 16($sp)                  # pass $sp
    RETURN_IF_ZERO

    .global art_set32_instance_from_code
    .extern artSet32InstanceFromCode
    /*
     * Called by managed code to resolve an instance field and store a 32-bit primitive value.
     */
    ALIGN_FUNCTION_ENTRY
art_set32_instance_from_code:
    GENERATE_GLOBAL_POINTER
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME     # save callee saves in case of GC
    lw     $a3, 64($sp)                  # pass referrer's Method*
    sw     rSELF, 16($sp)                # pass Thread::Current
    jal    artSet32InstanceFromCode      # (field_idx, Object*, new_val, referrer, Thread*, $sp)
    sw     $sp, 20($sp)                  # pass $sp
    RETURN_IF_ZERO

    .global art_set64_instance_from_code
    .extern artSet32InstanceFromCode
    /*
     * Called by managed code to resolve an instance field and store a 64-bit primitive value.
     */
    ALIGN_FUNCTION_ENTRY
art_set64_instance_from_code:
    GENERATE_GLOBAL_POINTER
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME     # save callee saves in case of GC
    sw     rSELF, 16($sp)                # pass Thread::Current
    jal    artSet64InstanceFromCode      # (field_idx, Object*, new_val, Thread*, $sp)
    sw     $sp, 20($sp)                  # pass $sp
    RETURN_IF_ZERO

    .global art_set_obj_instance_from_code
    .extern artSetObjInstanceFromCode
    /*
     * Called by managed code to resolve an instance field and store an object reference.
     */
    ALIGN_FUNCTION_ENTRY
art_set_obj_instance_from_code:
    GENERATE_GLOBAL_POINTER
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME     # save callee saves in case of GC
    lw     $a3, 64($sp)                  # pass referrer's Method*
    sw     rSELF, 16($sp)                # pass Thread::Current
    jal    artSetObjInstanceFromCode     # (field_idx, Object*, new_val, referrer, Thread*, $sp)
    sw     $sp, 20($sp)                  # pass $sp
    RETURN_IF_ZERO

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
    GENERATE_GLOBAL_POINTER
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME  # save callee saves in case of GC
    move    $a2, rSELF                # pass Thread::Current
    # artResolveStringFromCode(Method* referrer, uint32_t string_idx, Thread*, $sp)
    jal     artResolveStringFromCode
    move    $a3, $sp                  # pass $sp
    RETURN_IF_NONZERO

    .global art_alloc_object_from_code
    .extern artAllocObjectFromCode
    /*
     * Called by managed code to allocate an object.
     */
    ALIGN_FUNCTION_ENTRY
art_alloc_object_from_code:
    GENERATE_GLOBAL_POINTER
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME  # save callee saves in case of GC
    move    $a2, rSELF                # pass Thread::Current
    jal     artAllocObjectFromCode    # (uint32_t type_idx, Method* method, Thread*, $sp)
    move    $a3, $sp                  # pass $sp
    RETURN_IF_NONZERO

    .global art_alloc_object_from_code_with_access_check
    .extern artAllocObjectFromCodeWithAccessCheck
    /*
     * Called by managed code to allocate an object when the caller doesn't know whether it has
     * access to the created type.
     */
    ALIGN_FUNCTION_ENTRY
art_alloc_object_from_code_with_access_check:
    GENERATE_GLOBAL_POINTER
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME  # save callee saves in case of GC
    move    $a2, rSELF                # pass Thread::Current
    jal     artAllocObjectFromCodeWithAccessCheck  # (uint32_t type_idx, Method* method, Thread*, $sp)
    move    $a3, $sp                  # pass $sp
    RETURN_IF_NONZERO

    .global art_alloc_array_from_code
    .extern artAllocArrayFromCode
    /*
     * Called by managed code to allocate an array.
     */
    ALIGN_FUNCTION_ENTRY
art_alloc_array_from_code:
    GENERATE_GLOBAL_POINTER
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME  # save callee saves in case of GC
    move    $a3, rSELF                # pass Thread::Current
    # artAllocArrayFromCode(uint32_t type_idx, Method* method, int32_t component_count, Thread*, $sp)
    jal     artAllocArrayFromCode
    sw      $sp, 16($sp)              # pass $sp
    RETURN_IF_NONZERO

    .global art_alloc_array_from_code_with_access_check
    .extern artAllocArrayFromCodeWithAccessCheck
    /*
     * Called by managed code to allocate an array when the caller doesn't know whether it has
     * access to the created type.
     */
    ALIGN_FUNCTION_ENTRY
art_alloc_array_from_code_with_access_check:
    GENERATE_GLOBAL_POINTER
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME  # save callee saves in case of GC
    move    $a3, rSELF                # pass Thread::Current
    # artAllocArrayFromCodeWithAccessCheck(type_idx, method, component_count, Thread*, $sp)
    jal     artAllocArrayFromCodeWithAccessCheck
    sw      $sp, 16($sp)              # pass $sp
    RETURN_IF_NONZERO

    .global art_check_and_alloc_array_from_code
    .extern artCheckAndAllocArrayFromCode
    /*
     * Called by managed code to allocate an array in a special case for FILLED_NEW_ARRAY.
     */
    ALIGN_FUNCTION_ENTRY
art_check_and_alloc_array_from_code:
    GENERATE_GLOBAL_POINTER
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME  # save callee saves in case of GC
    move    $a3, rSELF                # pass Thread::Current
    # artCheckAndAllocArrayFromCode(uint32_t type_idx, Method* method, int32_t count, Thread* , $sp)
    jal     artCheckAndAllocArrayFromCode
    sw      $sp, 16($sp)              # pass $sp
    RETURN_IF_NONZERO

    .global art_check_and_alloc_array_from_code_with_access_check
    .extern artCheckAndAllocArrayFromCodeWithAccessCheck
    /*
     * Called by managed code to allocate an array in a special case for FILLED_NEW_ARRAY.
     */
    ALIGN_FUNCTION_ENTRY
art_check_and_alloc_array_from_code_with_access_check:
    GENERATE_GLOBAL_POINTER
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME  # save callee saves in case of GC
    move    $a3, rSELF                # pass Thread::Current
    # artCheckAndAllocArrayFromCodeWithAccessCheck(type_idx, method, count, Thread* , $sp)
    jal     artCheckAndAllocArrayFromCodeWithAccessCheck
    sw      $sp, 16($sp)              # pass $sp
    RETURN_IF_NONZERO

    .global art_test_suspend
    .extern artTestSuspendFromCode
    /*
     * Called by managed code when the value in rSUSPEND has been decremented to 0.
     */
    ALIGN_FUNCTION_ENTRY
art_test_suspend:
    GENERATE_GLOBAL_POINTER
    lh     $a0, THREAD_FLAGS_OFFSET(rSELF)
    bnez   $a0, 1f
    addi  rSUSPEND, $zero, SUSPEND_CHECK_INTERVAL   # reset rSUSPEND to SUSPEND_CHECK_INTERVAL
    jr     $ra
    nop
1:
    move   $a0, rSELF
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME          # save callee saves for stack crawl
    jal    artTestSuspendFromCode             # (Thread*, $sp)
    move   $a1, $sp
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME_AND_RETURN

    .global art_proxy_invoke_handler
    .extern artProxyInvokeHandler
    /*
     * Called by managed code that is attempting to call a method on a proxy class. On entry
     * r0 holds the proxy method; r1, r2 and r3 may contain arguments.
     */
    ALIGN_FUNCTION_ENTRY
art_proxy_invoke_handler:
    GENERATE_GLOBAL_POINTER
    SETUP_REF_AND_ARGS_CALLEE_SAVE_FRAME
    sw      $a0, 0($sp)            # place proxy method at bottom of frame
    move    $a2, rSELF             # pass Thread::Current
    jal     artProxyInvokeHandler  # (Method* proxy method, receiver, Thread*, args...)
    addiu   $a3, $sp, 8            # pointer to a2/a3/ra/caller's Method**/out-args as second arg
    lw      $t0, THREAD_EXCEPTION_OFFSET(rSELF) # load Thread::Current()->exception_
    lw      $ra, 44($sp)           # restore $ra
    lw      $v0, 8($sp)
    lw      $v1, 12($sp)
    bnez    $t0, 1f
    addiu   $sp, $sp, 48           # pop frame
    jr      $ra
    nop
1:
    DELIVER_PENDING_EXCEPTION

    .global art_instrumentation_entry_from_code
    .global art_instrumentation_exit_from_code
    .extern artInstrumentationMethodEntryFromCode
    .extern artInstrumentationMethodExitFromCode
    /*
     * Routine that intercepts method calls and returns.
     */
    ALIGN_FUNCTION_ENTRY
art_instrumentation_entry_from_code:
    GENERATE_GLOBAL_POINTER
    move     $t0, $sp       # remember bottom of caller's frame
    addiu    $sp, $sp, -16  # save arguments (4 words)
    sw       $a0, 0($sp)
    sw       $a1, 4($sp)
    sw       $a2, 8($sp)
    sw       $a3, 12($sp)
    move     $a3, $ra       # pass $ra
    move     $a2, $t0       # pass $sp
    jal      artInstrumentationMethodEntryFromCode  # (Method*, Thread*, SP, LR)
    move     $a1, rSELF     # pass Thread::Current
    move     $t9, $v0       # $t9 holds reference to code
    lw       $a0, 0($sp)
    lw       $a1, 4($sp)
    lw       $a2, 8($sp)
    lw       $a3, 12($sp)
    jalr     $t9            # call method
    addiu    $sp, $sp, 16
    /* intentional fallthrough */
art_instrumentation_exit_from_code:
    addiu    $t9, $ra, 4    # put current address into $t9 to rebuild $gp
    GENERATE_GLOBAL_POINTER
    move     $t0, $sp       # remember bottom of caller's frame
    addiu    $sp, $sp, -16  # save return values
    sw       $v0, 0($sp)
    sw       $v1, 4($sp)
    move     $a1, $t0       # pass $sp
    jal      artInstrumentationMethodExitFromCode  # (Thread*, SP)
    move     $a0, rSELF     # pass Thread::Current
    move     $t0, $v0       # set aside returned link register
    move     $ra, $v1       # set link register for deoptimization
    lw       $v0, 0($sp)
    lw       $v1, 4($sp)
    jr       $t0            # return
    addiu    $sp, $sp, 16

    .global art_deoptimize
    .extern artDeoptimize
    .extern artEnterInterpreterFromDeoptimize
    /*
     * The thread's enter interpreter flag is set and so we should transition to the interpreter
     * rather than allow execution to continue in the frame below. There may be live results in
     * registers depending on how complete the operation is when we safepoint - for example, a
     * set operation may have completed while a get operation needs writing back into the vregs.
     */
    ALIGN_FUNCTION_ENTRY
art_deoptimize:
    GENERATE_GLOBAL_POINTER
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME
    move     $a0, $v0       # pass first half of return value
    move     $a1, $v1       # pass second half of return value
    move     $a2, rSELF     # pass Thread::current
    jal      artDeoptimize  # artDeoptimize(return value, Thread*, SP)
                            # Returns caller method's frame size.
    move     $a3, $sp       # pass $sp
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME
    beqz     $v0, 1f        # Return if caller was upcall.
    add      $t9, $sp, $v0  # $t9 == bottom of caller's frame.
    lw       $ra, -4($t9)   # Restore $ra.
    move     $sp, $t9       # Remove frame.
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME
    jal      artEnterInterpreterFromDeoptimize  # Enter interpreter, callee-save ends stack fragment.
    nop
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME
1:
    jr       $ra            # Return to caller.
    nop

    .global art_shl_long
    /*
     * Long integer shift.  This is different from the generic 32/64-bit
     * binary operations because vAA/vBB are 64-bit but vCC (the shift
     * distance) is 32-bit.  Also, Dalvik requires us to ignore all but the low
     * 6 bits.
     * On entry:
     *   $a0: low word
     *   $a1: high word
     *   $a2: shift count
     */
    ALIGN_FUNCTION_ENTRY
art_shl_long:
    /* shl-long vAA, vBB, vCC */
    sll     $v0, $a0, $a2                    #  rlo<- alo << (shift&31)
    not     $v1, $a2                         #  rhi<- 31-shift  (shift is 5b)
    srl     $a0, 1
    srl     $a0, $v1                         #  alo<- alo >> (32-(shift&31))
    sll     $v1, $a1, $a2                    #  rhi<- ahi << (shift&31)
    or      $v1, $a0                         #  rhi<- rhi | alo
    andi    $a2, 0x20                        #  shift< shift & 0x20
    movn    $v1, $v0, $a2                    #  rhi<- rlo (if shift&0x20)
    jr      $ra
    movn    $v0, $zero, $a2                  #  rlo<- 0  (if shift&0x20)

    .global art_shr_long
    /*
     * Long integer shift.  This is different from the generic 32/64-bit
     * binary operations because vAA/vBB are 64-bit but vCC (the shift
     * distance) is 32-bit.  Also, Dalvik requires us to ignore all but the low
     * 6 bits.
     * On entry:
     *   $a0: low word
     *   $a1: high word
     *   $a2: shift count
     */
    ALIGN_FUNCTION_ENTRY
art_shr_long:
    sra     $v1, $a1, $a2                    #  rhi<- ahi >> (shift&31)
    srl     $v0, $a0, $a2                    #  rlo<- alo >> (shift&31)
    sra     $a3, $a1, 31                     #  $a3<- sign(ah)
    not     $a0, $a2                         #  alo<- 31-shift (shift is 5b)
    sll     $a1, 1
    sll     $a1, $a0                         #  ahi<- ahi << (32-(shift&31))
    or      $v0, $a1                         #  rlo<- rlo | ahi
    andi    $a2, 0x20                        #  shift & 0x20
    movn    $v0, $v1, $a2                    #  rlo<- rhi (if shift&0x20)
    jr      $ra
    movn    $v1, $a3, $a2                    #  rhi<- sign(ahi) (if shift&0x20)

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
    srl     $v1, $a1, $a2                    #  rhi<- ahi >> (shift&31)
    srl     $v0, $a0, $a2                    #  rlo<- alo >> (shift&31)
    not     $a0, $a2                         #  alo<- 31-shift (shift is 5b)
    sll     $a1, 1
    sll     $a1, $a0                         #  ahi<- ahi << (32-(shift&31))
    or      $v0, $a1                         #  rlo<- rlo | ahi
    andi    $a2, 0x20                        #  shift & 0x20
    movn    $v0, $v1, $a2                    #  rlo<- rhi (if shift&0x20)
    jr      $ra
    movn    $v1, $zero, $a2                  #  rhi<- 0 (if shift&0x20)

    .global art_indexof
    ALIGN_FUNCTION_ENTRY
art_indexof:
    jr $ra
    nop

    .global art_string_compareto
    ALIGN_FUNCTION_ENTRY
art_string_compareto:
    jr $ra
    nop
