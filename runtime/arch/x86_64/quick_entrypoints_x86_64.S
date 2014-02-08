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

#include "asm_support_x86_64.S"

// For x86, the CFA is esp+4, the address above the pushed return address on the stack.

    /*
     * Macro that sets up the callee save frame to conform with
     * Runtime::CreateCalleeSaveMethod(kSaveAll)
     */
MACRO0(SETUP_SAVE_ALL_CALLEE_SAVE_FRAME)
    int3
    int3
END_MACRO

    /*
     * Macro that sets up the callee save frame to conform with
     * Runtime::CreateCalleeSaveMethod(kRefsOnly)
     */
MACRO0(SETUP_REF_ONLY_CALLEE_SAVE_FRAME)
    int3
    int3
END_MACRO

MACRO0(RESTORE_REF_ONLY_CALLEE_SAVE_FRAME)
    int3
    int3
END_MACRO

    /*
     * Macro that sets up the callee save frame to conform with
     * Runtime::CreateCalleeSaveMethod(kRefsAndArgs)
     */
MACRO0(SETUP_REF_AND_ARGS_CALLEE_SAVE_FRAME)
    int3
    int3
END_MACRO

MACRO0(RESTORE_REF_AND_ARGS_CALLEE_SAVE_FRAME)
    int3
END_MACRO

    /*
     * Macro that set calls through to artDeliverPendingExceptionFromCode, where the pending
     * exception is Thread::Current()->exception_.
     */
MACRO0(DELIVER_PENDING_EXCEPTION)
    int3
    int3
END_MACRO

MACRO2(NO_ARG_RUNTIME_EXCEPTION, c_name, cxx_name)
    DEFINE_FUNCTION VAR(c_name, 0)
    int3
    int3
    END_FUNCTION VAR(c_name, 0)
END_MACRO

MACRO2(ONE_ARG_RUNTIME_EXCEPTION, c_name, cxx_name)
    DEFINE_FUNCTION VAR(c_name, 0)
    int3
    int3
    END_FUNCTION VAR(c_name, 0)
END_MACRO

MACRO2(TWO_ARG_RUNTIME_EXCEPTION, c_name, cxx_name)
    DEFINE_FUNCTION VAR(c_name, 0)
    int3
    int3
    END_FUNCTION VAR(c_name, 0)
END_MACRO

    /*
     * Called by managed code to create and deliver a NullPointerException.
     */
NO_ARG_RUNTIME_EXCEPTION art_quick_throw_null_pointer_exception, artThrowNullPointerExceptionFromCode

    /*
     * Called by managed code to create and deliver an ArithmeticException.
     */
NO_ARG_RUNTIME_EXCEPTION art_quick_throw_div_zero, artThrowDivZeroFromCode

    /*
     * Called by managed code to create and deliver a StackOverflowError.
     */
NO_ARG_RUNTIME_EXCEPTION art_quick_throw_stack_overflow, artThrowStackOverflowFromCode

    /*
     * Called by managed code, saves callee saves and then calls artThrowException
     * that will place a mock Method* at the bottom of the stack. Arg1 holds the exception.
     */
ONE_ARG_RUNTIME_EXCEPTION art_quick_deliver_exception, artDeliverExceptionFromCode

    /*
     * Called by managed code to create and deliver a NoSuchMethodError.
     */
ONE_ARG_RUNTIME_EXCEPTION art_quick_throw_no_such_method, artThrowNoSuchMethodFromCode

    /*
     * Called by managed code to create and deliver an ArrayIndexOutOfBoundsException. Arg1 holds
     * index, arg2 holds limit.
     */
TWO_ARG_RUNTIME_EXCEPTION art_quick_throw_array_bounds, artThrowArrayBoundsFromCode

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
MACRO2(INVOKE_TRAMPOLINE, c_name, cxx_name)
    DEFINE_FUNCTION VAR(c_name, 0)
    int3
    int3
    END_FUNCTION VAR(c_name, 0)
END_MACRO

INVOKE_TRAMPOLINE art_quick_invoke_interface_trampoline, artInvokeInterfaceTrampoline
INVOKE_TRAMPOLINE art_quick_invoke_interface_trampoline_with_access_check, artInvokeInterfaceTrampolineWithAccessCheck

INVOKE_TRAMPOLINE art_quick_invoke_static_trampoline_with_access_check, artInvokeStaticTrampolineWithAccessCheck
INVOKE_TRAMPOLINE art_quick_invoke_direct_trampoline_with_access_check, artInvokeDirectTrampolineWithAccessCheck
INVOKE_TRAMPOLINE art_quick_invoke_super_trampoline_with_access_check, artInvokeSuperTrampolineWithAccessCheck
INVOKE_TRAMPOLINE art_quick_invoke_virtual_trampoline_with_access_check, artInvokeVirtualTrampolineWithAccessCheck

    /*
     * Quick invocation stub.
     */
DEFINE_FUNCTION art_quick_invoke_stub
    int3
    int3
END_FUNCTION art_quick_invoke_stub

MACRO3(NO_ARG_DOWNCALL, c_name, cxx_name, return_macro)
    DEFINE_FUNCTION VAR(c_name, 0)
    int3
    int3
    END_FUNCTION VAR(c_name, 0)
END_MACRO

MACRO3(ONE_ARG_DOWNCALL, c_name, cxx_name, return_macro)
    DEFINE_FUNCTION VAR(c_name, 0)
    int3
    int3
    END_FUNCTION VAR(c_name, 0)
END_MACRO

MACRO3(TWO_ARG_DOWNCALL, c_name, cxx_name, return_macro)
    DEFINE_FUNCTION VAR(c_name, 0)
    int3
    int3
    END_FUNCTION VAR(c_name, 0)
END_MACRO

MACRO3(THREE_ARG_DOWNCALL, c_name, cxx_name, return_macro)
    DEFINE_FUNCTION VAR(c_name, 0)
    int3
    int3
    END_FUNCTION VAR(c_name, 0)
END_MACRO

MACRO0(RETURN_IF_RESULT_IS_NON_ZERO)
    int3
    testl %eax, %eax               // eax == 0 ?
    jz  1f                         // if eax == 0 goto 1
    ret                            // return
1:                                 // deliver exception on current thread
    DELIVER_PENDING_EXCEPTION
END_MACRO

MACRO0(RETURN_IF_EAX_ZERO)
    int3
    testl %eax, %eax               // eax == 0 ?
    jnz  1f                        // if eax != 0 goto 1
    ret                            // return
1:                                 // deliver exception on current thread
    DELIVER_PENDING_EXCEPTION
END_MACRO

MACRO0(RETURN_OR_DELIVER_PENDING_EXCEPTION)
    int3
    int3
    DELIVER_PENDING_EXCEPTION
END_MACRO

// Generate the allocation entrypoints for each allocator.
// TODO: use arch/quick_alloc_entrypoints.S. Currently we don't as we need to use concatenation
// macros to work around differences between OS/X's as and binutils as (OS/X lacks named arguments
// to macros and the VAR macro won't concatenate arguments properly), this also breaks having
// multi-line macros that use each other (hence using 1 macro per newline below).
#define GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT(c_suffix, cxx_suffix) \
  TWO_ARG_DOWNCALL art_quick_alloc_object ## c_suffix, artAllocObjectFromCode ## cxx_suffix, RETURN_IF_RESULT_IS_NON_ZERO
#define GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT_RESOLVED(c_suffix, cxx_suffix) \
  TWO_ARG_DOWNCALL art_quick_alloc_object_resolved ## c_suffix, artAllocObjectFromCodeResolved ## cxx_suffix, RETURN_IF_RESULT_IS_NON_ZERO
#define GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT_INITIALIZED(c_suffix, cxx_suffix) \
  TWO_ARG_DOWNCALL art_quick_alloc_object_initialized ## c_suffix, artAllocObjectFromCodeInitialized ## cxx_suffix, RETURN_IF_RESULT_IS_NON_ZERO
#define GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT_WITH_ACCESS_CHECK(c_suffix, cxx_suffix) \
  TWO_ARG_DOWNCALL art_quick_alloc_object_with_access_check ## c_suffix, artAllocObjectFromCodeWithAccessCheck ## cxx_suffix, RETURN_IF_RESULT_IS_NON_ZERO
#define GENERATE_ALLOC_ENTRYPOINTS_ALLOC_ARRAY(c_suffix, cxx_suffix) \
  THREE_ARG_DOWNCALL art_quick_alloc_array ## c_suffix, artAllocArrayFromCode ## cxx_suffix, RETURN_IF_RESULT_IS_NON_ZERO
#define GENERATE_ALLOC_ENTRYPOINTS_ALLOC_ARRAY_RESOLVED(c_suffix, cxx_suffix) \
  THREE_ARG_DOWNCALL art_quick_alloc_array_resolved ## c_suffix, artAllocArrayFromCodeResolved ## cxx_suffix, RETURN_IF_RESULT_IS_NON_ZERO
#define GENERATE_ALLOC_ENTRYPOINTS_ALLOC_ARRAY_WITH_ACCESS_CHECK(c_suffix, cxx_suffix) \
  THREE_ARG_DOWNCALL art_quick_alloc_array_with_access_check ## c_suffix, artAllocArrayFromCodeWithAccessCheck ## cxx_suffix, RETURN_IF_RESULT_IS_NON_ZERO
#define GENERATE_ALLOC_ENTRYPOINTS_CHECK_AND_ALLOC_ARRAY(c_suffix, cxx_suffix) \
  THREE_ARG_DOWNCALL art_quick_check_and_alloc_array ## c_suffix, artCheckAndAllocArrayFromCode ## cxx_suffix, RETURN_IF_RESULT_IS_NON_ZERO
#define GENERATE_ALLOC_ENTRYPOINTS_CHECK_AND_ALLOC_ARRAY_WITH_ACCESS_CHECK(c_suffix, cxx_suffix) \
  THREE_ARG_DOWNCALL art_quick_check_and_alloc_array_with_access_check ## c_suffix, artCheckAndAllocArrayFromCodeWithAccessCheck ## cxx_suffix, RETURN_IF_RESULT_IS_NON_ZERO

GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT(_dlmalloc, DlMalloc)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT_RESOLVED(_dlmalloc, DlMalloc)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT_INITIALIZED(_dlmalloc, DlMalloc)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT_WITH_ACCESS_CHECK(_dlmalloc, DlMalloc)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_ARRAY(_dlmalloc, DlMalloc)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_ARRAY_RESOLVED(_dlmalloc, DlMalloc)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_ARRAY_WITH_ACCESS_CHECK(_dlmalloc, DlMalloc)
GENERATE_ALLOC_ENTRYPOINTS_CHECK_AND_ALLOC_ARRAY(_dlmalloc, DlMalloc)
GENERATE_ALLOC_ENTRYPOINTS_CHECK_AND_ALLOC_ARRAY_WITH_ACCESS_CHECK(_dlmalloc, DlMalloc)

GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT(_dlmalloc_instrumented, DlMallocInstrumented)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT_RESOLVED(_dlmalloc_instrumented, DlMallocInstrumented)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT_INITIALIZED(_dlmalloc_instrumented, DlMallocInstrumented)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT_WITH_ACCESS_CHECK(_dlmalloc_instrumented, DlMallocInstrumented)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_ARRAY(_dlmalloc_instrumented, DlMallocInstrumented)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_ARRAY_RESOLVED(_dlmalloc_instrumented, DlMallocInstrumented)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_ARRAY_WITH_ACCESS_CHECK(_dlmalloc_instrumented, DlMallocInstrumented)
GENERATE_ALLOC_ENTRYPOINTS_CHECK_AND_ALLOC_ARRAY(_dlmalloc_instrumented, DlMallocInstrumented)
GENERATE_ALLOC_ENTRYPOINTS_CHECK_AND_ALLOC_ARRAY_WITH_ACCESS_CHECK(_dlmalloc_instrumented, DlMallocInstrumented)

GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT(_rosalloc, RosAlloc)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT_RESOLVED(_rosalloc, RosAlloc)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT_INITIALIZED(_rosalloc, RosAlloc)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT_WITH_ACCESS_CHECK(_rosalloc, RosAlloc)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_ARRAY(_rosalloc, RosAlloc)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_ARRAY_RESOLVED(_rosalloc, RosAlloc)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_ARRAY_WITH_ACCESS_CHECK(_rosalloc, RosAlloc)
GENERATE_ALLOC_ENTRYPOINTS_CHECK_AND_ALLOC_ARRAY(_rosalloc, RosAlloc)
GENERATE_ALLOC_ENTRYPOINTS_CHECK_AND_ALLOC_ARRAY_WITH_ACCESS_CHECK(_rosalloc, RosAlloc)

GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT(_rosalloc_instrumented, RosAllocInstrumented)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT_RESOLVED(_rosalloc_instrumented, RosAllocInstrumented)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT_INITIALIZED(_rosalloc_instrumented, RosAllocInstrumented)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT_WITH_ACCESS_CHECK(_rosalloc_instrumented, RosAllocInstrumented)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_ARRAY(_rosalloc_instrumented, RosAllocInstrumented)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_ARRAY_RESOLVED(_rosalloc_instrumented, RosAllocInstrumented)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_ARRAY_WITH_ACCESS_CHECK(_rosalloc_instrumented, RosAllocInstrumented)
GENERATE_ALLOC_ENTRYPOINTS_CHECK_AND_ALLOC_ARRAY(_rosalloc_instrumented, RosAllocInstrumented)
GENERATE_ALLOC_ENTRYPOINTS_CHECK_AND_ALLOC_ARRAY_WITH_ACCESS_CHECK(_rosalloc_instrumented, RosAllocInstrumented)

GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT(_bump_pointer, BumpPointer)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT_RESOLVED(_bump_pointer, BumpPointer)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT_INITIALIZED(_bump_pointer, BumpPointer)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT_WITH_ACCESS_CHECK(_bump_pointer, BumpPointer)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_ARRAY(_bump_pointer, BumpPointer)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_ARRAY_RESOLVED(_bump_pointer, BumpPointer)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_ARRAY_WITH_ACCESS_CHECK(_bump_pointer, BumpPointer)
GENERATE_ALLOC_ENTRYPOINTS_CHECK_AND_ALLOC_ARRAY(_bump_pointer, BumpPointer)
GENERATE_ALLOC_ENTRYPOINTS_CHECK_AND_ALLOC_ARRAY_WITH_ACCESS_CHECK(_bump_pointer, BumpPointer)

GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT(_bump_pointer_instrumented, BumpPointerInstrumented)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT_RESOLVED(_bump_pointer_instrumented, BumpPointerInstrumented)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT_INITIALIZED(_bump_pointer_instrumented, BumpPointerInstrumented)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT_WITH_ACCESS_CHECK(_bump_pointer_instrumented, BumpPointerInstrumented)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_ARRAY(_bump_pointer_instrumented, BumpPointerInstrumented)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_ARRAY_RESOLVED(_bump_pointer_instrumented, BumpPointerInstrumented)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_ARRAY_WITH_ACCESS_CHECK(_bump_pointer_instrumented, BumpPointerInstrumented)
GENERATE_ALLOC_ENTRYPOINTS_CHECK_AND_ALLOC_ARRAY(_bump_pointer_instrumented, BumpPointerInstrumented)
GENERATE_ALLOC_ENTRYPOINTS_CHECK_AND_ALLOC_ARRAY_WITH_ACCESS_CHECK(_bump_pointer_instrumented, BumpPointerInstrumented)

GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT(_tlab, TLAB)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT_RESOLVED(_tlab, TLAB)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT_INITIALIZED(_tlab, TLAB)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT_WITH_ACCESS_CHECK(_tlab, TLAB)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_ARRAY(_tlab, TLAB)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_ARRAY_RESOLVED(_tlab, TLAB)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_ARRAY_WITH_ACCESS_CHECK(_tlab, TLAB)
GENERATE_ALLOC_ENTRYPOINTS_CHECK_AND_ALLOC_ARRAY(_tlab, TLAB)
GENERATE_ALLOC_ENTRYPOINTS_CHECK_AND_ALLOC_ARRAY_WITH_ACCESS_CHECK(_tlab, TLAB)

GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT(_tlab_instrumented, TLABInstrumented)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT_RESOLVED(_tlab_instrumented, TLABInstrumented)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT_INITIALIZED(_tlab_instrumented, TLABInstrumented)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT_WITH_ACCESS_CHECK(_tlab_instrumented, TLABInstrumented)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_ARRAY(_tlab_instrumented, TLABInstrumented)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_ARRAY_RESOLVED(_tlab_instrumented, TLABInstrumented)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_ARRAY_WITH_ACCESS_CHECK(_tlab_instrumented, TLABInstrumented)
GENERATE_ALLOC_ENTRYPOINTS_CHECK_AND_ALLOC_ARRAY(_tlab_instrumented, TLABInstrumented)
GENERATE_ALLOC_ENTRYPOINTS_CHECK_AND_ALLOC_ARRAY_WITH_ACCESS_CHECK(_tlab_instrumented, TLABInstrumented)

TWO_ARG_DOWNCALL art_quick_resolve_string, artResolveStringFromCode, RETURN_IF_RESULT_IS_NON_ZERO
TWO_ARG_DOWNCALL art_quick_initialize_static_storage, artInitializeStaticStorageFromCode, RETURN_IF_RESULT_IS_NON_ZERO
TWO_ARG_DOWNCALL art_quick_initialize_type, artInitializeTypeFromCode, RETURN_IF_RESULT_IS_NON_ZERO
TWO_ARG_DOWNCALL art_quick_initialize_type_and_verify_access, artInitializeTypeAndVerifyAccessFromCode, RETURN_IF_RESULT_IS_NON_ZERO

TWO_ARG_DOWNCALL art_quick_handle_fill_data, artHandleFillArrayDataFromCode, RETURN_IF_EAX_ZERO

DEFINE_FUNCTION art_quick_lock_object
    int3
    int3
END_FUNCTION art_quick_lock_object

DEFINE_FUNCTION art_quick_unlock_object
    int3
    int3
END_FUNCTION art_quick_unlock_object

DEFINE_FUNCTION art_quick_is_assignable
    int3
    int3
END_FUNCTION art_quick_is_assignable

DEFINE_FUNCTION art_quick_check_cast
    int3
    int3
END_FUNCTION art_quick_check_cast

    /*
     * Entry from managed code for array put operations of objects where the value being stored
     * needs to be checked for compatibility.
     * eax = array, ecx = index, edx = value
     */
UNIMPLEMENTED art_quick_aput_obj_with_null_and_bound_check
UNIMPLEMENTED art_quick_aput_obj_with_bound_check
UNIMPLEMENTED art_quick_aput_obj
UNIMPLEMENTED art_quick_memcpy

NO_ARG_DOWNCALL art_quick_test_suspend, artTestSuspendFromCode, ret

UNIMPLEMENTED art_quick_fmod
UNIMPLEMENTED art_quick_fmodf
UNIMPLEMENTED art_quick_l2d
UNIMPLEMENTED art_quick_l2f
UNIMPLEMENTED art_quick_d2l
UNIMPLEMENTED art_quick_f2l
UNIMPLEMENTED art_quick_idivmod
UNIMPLEMENTED art_quick_ldiv
UNIMPLEMENTED art_quick_lmod
UNIMPLEMENTED art_quick_lmul
UNIMPLEMENTED art_quick_lshl
UNIMPLEMENTED art_quick_lshr
UNIMPLEMENTED art_quick_lushr
UNIMPLEMENTED art_quick_set32_instance
UNIMPLEMENTED art_quick_set64_instance
UNIMPLEMENTED art_quick_set_obj_instance
UNIMPLEMENTED art_quick_get32_instance
UNIMPLEMENTED art_quick_get64_instance
UNIMPLEMENTED art_quick_get_obj_instance
UNIMPLEMENTED art_quick_set32_static
UNIMPLEMENTED art_quick_set64_static
UNIMPLEMENTED art_quick_set_obj_static
UNIMPLEMENTED art_quick_get32_static
UNIMPLEMENTED art_quick_get64_static
UNIMPLEMENTED art_quick_get_obj_static
UNIMPLEMENTED art_quick_proxy_invoke_handler

    /*
     * Called to resolve an imt conflict.
     */
UNIMPLEMENTED art_quick_imt_conflict_trampoline
UNIMPLEMENTED art_quick_resolution_trampoline
UNIMPLEMENTED art_quick_to_interpreter_bridge

    /*
     * Routine that intercepts method calls and returns.
     */
UNIMPLEMENTED art_quick_instrumentation_entry
UNIMPLEMENTED art_quick_instrumentation_exit

    /*
     * Instrumentation has requested that we deoptimize into the interpreter. The deoptimization
     * will long jump to the upcall with a special exception of -1.
     */
UNIMPLEMENTED art_quick_deoptimize

UNIMPLEMENTED art_quick_indexof
UNIMPLEMENTED art_quick_string_compareto
UNIMPLEMENTED art_quick_memcmp16
