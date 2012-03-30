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

#if defined(__APPLE__)
    // Mac OS' as(1) doesn't let you name macro parameters.
    #define MACRO0(macro_name) .macro macro_name
    #define MACRO1(macro_name, macro_arg1) .macro macro_name
    #define MACRO2(macro_name, macro_arg1, macro_args2) .macro macro_name
    #define END_MACRO .endmacro

    // Mac OS' as(1) uses $0, $1, and so on for macro arguments, and function names
    // are mangled with an extra underscore prefix. The use of $x for arguments
    // mean that literals need to be represented with $$x in macros.
    #define VAR(name,index) _$index
    #define LITERAL(value) $$value
#else
    // Regular gas(1) lets you name macro parameters.
    #define MACRO0(macro_name) .macro macro_name
    #define MACRO1(macro_name, macro_arg1) .macro macro_name macro_arg1
    #define MACRO2(macro_name, macro_arg1, macro_arg2) .macro macro_name macro_arg1, macro_arg2
    #define END_MACRO .endm

    // Regular gas(1) uses \argument_name for macro arguments.
    // We need to turn on alternate macro syntax so we can use & instead or the preprocessor
    // will screw us by inserting a space between the \ and the name. Even in this mode there's
    // no special meaning to $, so literals are still just $x.
    .altmacro
    #define VAR(name,index) name&
    #define LITERAL(value) $value
#endif

    /* Cache alignment for function entry */
MACRO0(ALIGN_FUNCTION_ENTRY)
    .balign 16
END_MACRO

    /*
     * Macro that sets up the callee save frame to conform with
     * Runtime::CreateCalleeSaveMethod(...)
     */
MACRO0(SETUP_SAVE_ALL_CALLEE_SAVE_FRAME)
    pushl %edi  // Save callee saves (ebx is saved/restored by the upcall)
    pushl %esi
    pushl %ebp
    subl  LITERAL(16), %esp  // Grow stack by 4 words, bottom word will hold Method*
END_MACRO

MACRO0(RESTORE_CALLEE_SAVE_FRAME)
    addl LITERAL(16), %esp  // Remove padding
    popl %ebp  // Restore callee saves
    popl %esi
    popl %edi
END_MACRO

    /*
     * Macro that sets up the callee save frame to conform with
     * Runtime::CreateCalleeSaveMethod(...)
     */
MACRO0(SETUP_REF_AND_ARG_CALLEE_SAVE_FRAME)
    pushl %edi  // Save callee saves
    pushl %esi
    pushl %ebp
    pushl %ebx  // Save args
    pushl %edx
    pushl %ecx
    pushl %eax  // Align stack, eax will be clobbered by Method*
END_MACRO

MACRO0(RESTORE_REF_AND_ARG_CALLEE_SAVE_FRAME)
    addl LITERAL(16), %esp  // Remove padding
    popl %ebp  // Restore callee saves
    popl %esi
    popl %edi
END_MACRO

    /*
     * Macro that set calls through to artDeliverPendingExceptionFromCode, where the pending
     * exception is Thread::Current()->exception_.
     */
MACRO0(DELIVER_PENDING_EXCEPTION)
    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME         // save callee saves for throw
    mov %esp, %ecx
    // Outgoing argument set up
    subl  LITERAL(8), %esp                   // Alignment padding
    pushl %ecx                               // pass SP
    pushl %fs:THREAD_SELF_OFFSET             // pass Thread::Current()
    call artDeliverPendingExceptionFromCode  // artDeliverExceptionFromCode(Thread*, SP)
    int3
END_MACRO

MACRO2(NO_ARG_RUNTIME_EXCEPTION, c_name, cxx_name)
    .globl VAR(c_name, 0)
    ALIGN_FUNCTION_ENTRY
VAR(c_name, 0):
    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME  // save all registers as basis for long jump context
    mov %esp, %ecx
    // Outgoing argument set up
    subl  LITERAL(8), %esp        // alignment padding
    pushl %fs:THREAD_SELF_OFFSET  // pass Thread::Current()
    pushl %ecx                    // pass SP
    call VAR(cxx_name, 1)         // cxx_name(Thread*, SP)
    int3                          // unreached
END_MACRO

MACRO2(ONE_ARG_RUNTIME_EXCEPTION, c_name, cxx_name)
    .globl VAR(c_name, 0)
    ALIGN_FUNCTION_ENTRY
VAR(c_name, 0):
    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME  // save all registers as basis for long jump context
    mov %esp, %ecx
    // Outgoing argument set up
    pushl LITERAL(0)              // alignment padding
    pushl %ecx                    // pass SP
    pushl %fs:THREAD_SELF_OFFSET  // pass Thread::Current()
    pushl %eax                    // pass arg1
    call VAR(cxx_name, 1)         // cxx_name(arg1, Thread*, SP)
    int3                          // unreached
END_MACRO

MACRO2(TWO_ARG_RUNTIME_EXCEPTION, c_name, cxx_name)
    .globl VAR(c_name, 0)
    ALIGN_FUNCTION_ENTRY
VAR(c_name, 0):
    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME  // save all registers as basis for long jump context
    mov %esp, %edx
    // Outgoing argument set up
    pushl %edx                    // pass SP
    pushl %fs:THREAD_SELF_OFFSET  // pass Thread::Current()
    pushl %eax                    // pass arg1
    pushl %ecx                    // pass arg2
    call VAR(cxx_name, 1)         // cxx_name(arg2, arg1, Thread*, SP)
    int3                          // unreached
END_MACRO

    /*
     * Called by managed code to create and deliver a NullPointerException.
     */
NO_ARG_RUNTIME_EXCEPTION art_throw_null_pointer_exception_from_code, artThrowNullPointerExceptionFromCode

    /*
     * Called by managed code to create and deliver an ArithmeticException.
     */
NO_ARG_RUNTIME_EXCEPTION art_throw_div_zero_from_code, artThrowDivZeroFromCode

    /*
     * Called by managed code to create and deliver a StackOverflowError.
     */
NO_ARG_RUNTIME_EXCEPTION art_throw_stack_overflow_from_code, artThrowStackOverflowFromCode

    /*
     * Called by managed code, saves callee saves and then calls artThrowException
     * that will place a mock Method* at the bottom of the stack. Arg1 holds the exception.
     */
ONE_ARG_RUNTIME_EXCEPTION art_deliver_exception_from_code, artDeliverExceptionFromCode

    /*
     * Called by managed code to create and deliver a NoSuchMethodError.
     */
ONE_ARG_RUNTIME_EXCEPTION art_throw_no_such_method_from_code, artThrowNoSuchMethodFromCode

    /*
     * Called by managed code to create and deliver an ArrayIndexOutOfBoundsException. Arg1 holds
     * index, arg2 holds limit.
     */
TWO_ARG_RUNTIME_EXCEPTION art_throw_array_bounds_from_code, artThrowArrayBoundsFromCode

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
MACRO2(INVOKE_TRAMPOLINE, c_name, cxx_name)
    .globl VAR(c_name, 0)
    ALIGN_FUNCTION_ENTRY
VAR(c_name, 0):
    int3
END_MACRO

INVOKE_TRAMPOLINE art_invoke_interface_trampoline, artInvokeInterfaceTrampoline
INVOKE_TRAMPOLINE art_invoke_interface_trampoline_with_access_check, artInvokeInterfaceTrampolineWithAccessCheck

INVOKE_TRAMPOLINE art_invoke_static_trampoline_with_access_check, artInvokeStaticTrampolineWithAccessCheck
INVOKE_TRAMPOLINE art_invoke_direct_trampoline_with_access_check, artInvokeDirectTrampolineWithAccessCheck
INVOKE_TRAMPOLINE art_invoke_super_trampoline_with_access_check, artInvokeSuperTrampolineWithAccessCheck
INVOKE_TRAMPOLINE art_invoke_virtual_trampoline_with_access_check, artInvokeVirtualTrampolineWithAccessCheck

MACRO1(UNIMPLEMENTED,name)
    .globl VAR(name, 0)
    ALIGN_FUNCTION_ENTRY
VAR(name, 0):
    int3
END_MACRO

    // TODO: implement these!
UNIMPLEMENTED art_proxy_invoke_handler
UNIMPLEMENTED art_update_debugger
UNIMPLEMENTED art_test_suspend
UNIMPLEMENTED art_alloc_object_from_code
UNIMPLEMENTED art_alloc_object_from_code_with_access_check
UNIMPLEMENTED art_alloc_array_from_code
UNIMPLEMENTED art_alloc_array_from_code_with_access_check
UNIMPLEMENTED art_check_and_alloc_array_from_code
UNIMPLEMENTED art_check_and_alloc_array_from_code_with_access_check
UNIMPLEMENTED art_can_put_array_element_from_code
UNIMPLEMENTED art_check_cast_from_code
UNIMPLEMENTED art_initialize_static_storage_from_code
UNIMPLEMENTED art_initialize_type_and_verify_access_from_code
UNIMPLEMENTED art_initialize_type_from_code
UNIMPLEMENTED art_resolve_string_from_code
UNIMPLEMENTED art_set32_instance_from_code
UNIMPLEMENTED art_set64_instance_from_code
UNIMPLEMENTED art_set_obj_instance_from_code
UNIMPLEMENTED art_get32_instance_from_code
UNIMPLEMENTED art_get64_instance_from_code
UNIMPLEMENTED art_get_obj_instance_from_code
UNIMPLEMENTED art_set32_static_from_code
UNIMPLEMENTED art_set64_static_from_code
UNIMPLEMENTED art_set_obj_static_from_code
UNIMPLEMENTED art_get32_static_from_code
UNIMPLEMENTED art_get64_static_from_code
UNIMPLEMENTED art_get_obj_static_from_code
UNIMPLEMENTED art_handle_fill_data_from_code
UNIMPLEMENTED art_lock_object_from_code
UNIMPLEMENTED art_unlock_object_from_code
UNIMPLEMENTED art_indexof
UNIMPLEMENTED __memcmp16
UNIMPLEMENTED art_string_compareto
