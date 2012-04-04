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
    #define SYMBOL(name) _ ## name
    #define VAR(name,index) SYMBOL($index)
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
    #define SYMBOL(name) name
    #define VAR(name,index) name&
    #define LITERAL(value) $value
#endif

    /* Cache alignment for function entry */
MACRO0(ALIGN_FUNCTION_ENTRY)
    .balign 16
END_MACRO

MACRO1(DEFINE_FUNCTION,c_name)
    .globl VAR(c_name, 0)
    ALIGN_FUNCTION_ENTRY
VAR(c_name, 0):
END_MACRO

    /*
     * Macro that sets up the callee save frame to conform with
     * Runtime::CreateCalleeSaveMethod(kSaveAll)
     */
MACRO0(SETUP_SAVE_ALL_CALLEE_SAVE_FRAME)
    pushl %edi  // Save callee saves (ebx is saved/restored by the upcall)
    pushl %esi
    pushl %ebp
    subl  LITERAL(16), %esp  // Grow stack by 4 words, bottom word will hold Method*
END_MACRO

    /*
     * Macro that sets up the callee save frame to conform with
     * Runtime::CreateCalleeSaveMethod(kRefsOnly)
     */
MACRO0(SETUP_REF_ONLY_CALLEE_SAVE_FRAME)
    pushl %edi  // Save callee saves (ebx is saved/restored by the upcall)
    pushl %esi
    pushl %ebp
    subl  LITERAL(16), %esp  // Grow stack by 4 words, bottom word will hold Method*
END_MACRO

MACRO0(RESTORE_REF_ONLY_CALLEE_SAVE_FRAME)
    addl LITERAL(28), %esp  // Unwind stack up to return address
END_MACRO

    /*
     * Macro that sets up the callee save frame to conform with
     * Runtime::CreateCalleeSaveMethod(kRefsAndArgs)
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
    addl LITERAL(4), %esp  // Remove padding
    popl %ecx  // Restore args except eax
    popl %edx
    popl %ebx
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
    call SYMBOL(artDeliverPendingExceptionFromCode)  // artDeliverExceptionFromCode(Thread*, SP)
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
    pushl %ecx                    // pass arg2
    pushl %eax                    // pass arg1
    call VAR(cxx_name, 1)         // cxx_name(arg1, arg2, Thread*, SP)
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
    // Set up the callee save frame to conform with Runtime::CreateCalleeSaveMethod(kRefsAndArgs)
    // return address
    pushl %edi
    pushl %esi
    pushl %ebp
    pushl %ebx
    pushl %edx
    pushl %ecx
    pushl %eax  // <-- callee save Method* to go here
    movl %esp, %edx  // remember SP
    // Outgoing argument set up
    subl LITERAL(12), %esp        // alignment padding
    pushl %edx                    // pass SP
    pushl %fs:THREAD_SELF_OFFSET  // pass Thread::Current()
    pushl 32(%edx)                // pass caller Method*
    pushl %ecx                    // pass arg2
    pushl %eax                    // pass arg1
    call VAR(cxx_name, 1)         // cxx_name(arg1, arg2, arg3, Thread*, SP)
    movl %edx, %edi               // save code pointer in EDI
    addl LITERAL(36), %esp        // Pop arguments skip eax
    popl %ecx                     // Restore args
    popl %edx
    popl %ebx
    popl %ebp  // Restore callee saves.
    popl %esi
    // Swap EDI callee save with code pointer.
    xchgl %edi, (%esp)
    testl %eax, %eax              // Branch forward if exception pending.
    jz    1f
    // Tail call to intended method.
    ret
1:
    DELIVER_PENDING_EXCEPTION
END_MACRO

INVOKE_TRAMPOLINE art_invoke_interface_trampoline, artInvokeInterfaceTrampoline
INVOKE_TRAMPOLINE art_invoke_interface_trampoline_with_access_check, artInvokeInterfaceTrampolineWithAccessCheck

INVOKE_TRAMPOLINE art_invoke_static_trampoline_with_access_check, artInvokeStaticTrampolineWithAccessCheck
INVOKE_TRAMPOLINE art_invoke_direct_trampoline_with_access_check, artInvokeDirectTrampolineWithAccessCheck
INVOKE_TRAMPOLINE art_invoke_super_trampoline_with_access_check, artInvokeSuperTrampolineWithAccessCheck
INVOKE_TRAMPOLINE art_invoke_virtual_trampoline_with_access_check, artInvokeVirtualTrampolineWithAccessCheck

MACRO2(TWO_ARG_ALLOC, c_name, cxx_name)
    .globl VAR(c_name, 0)
    ALIGN_FUNCTION_ENTRY
VAR(c_name, 0):
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME  // save ref containing registers for GC
    mov %esp, %edx                // remember SP
    // Outgoing argument set up
    pushl %edx                    // pass SP
    pushl %fs:THREAD_SELF_OFFSET  // pass Thread::Current()
    pushl %ecx                    // pass arg2
    pushl %eax                    // pass arg1
    call VAR(cxx_name, 1)         // cxx_name(arg1, arg2, arg3, Thread*, SP)
    addl LITERAL(16), %esp        // pop arguments
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME  // restore frame up to return address
    testl %eax, %eax               // eax == 0 ?
    jz  1f
    ret
1:
    DELIVER_PENDING_EXCEPTION
END_MACRO

MACRO2(THREE_ARG_ALLOC, c_name, cxx_name)
    .globl VAR(c_name, 0)
    ALIGN_FUNCTION_ENTRY
VAR(c_name, 0):
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME  // save ref containing registers for GC
    mov %esp, %ebx                // remember SP
    // Outgoing argument set up
    subl LITERAL(12), %esp        // alignment padding
    pushl %ebx                    // pass SP
    pushl %fs:THREAD_SELF_OFFSET  // pass Thread::Current()
    pushl %edx                    // pass arg3
    pushl %ecx                    // pass arg2
    pushl %eax                    // pass arg1
    call VAR(cxx_name, 1)         // cxx_name(arg1, arg2, Thread*, SP)
    addl LITERAL(32), %esp        // pop arguments
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME  // restore frame up to return address
    testl %eax, %eax               // eax == 0 ?
    jz  1f
    ret
1:
    DELIVER_PENDING_EXCEPTION
END_MACRO

TWO_ARG_ALLOC art_alloc_object_from_code, artAllocObjectFromCode
TWO_ARG_ALLOC art_alloc_object_from_code_with_access_check, artAllocObjectFromCodeWithAccessCheck
THREE_ARG_ALLOC art_alloc_array_from_code, artAllocArrayFromCode
THREE_ARG_ALLOC art_alloc_array_from_code_with_access_check, artAllocArrayFromCodeWithAccessCheck
THREE_ARG_ALLOC art_check_and_alloc_array_from_code, artCheckAndAllocArrayFromCode
THREE_ARG_ALLOC art_check_and_alloc_array_from_code_with_access_check, artCheckAndAllocArrayFromCodeWithAccessCheck

TWO_ARG_ALLOC art_resolve_string_from_code, artResolveStringFromCode
TWO_ARG_ALLOC art_initialize_static_storage_from_code, artInitializeStaticStorageFromCode

DEFINE_FUNCTION art_lock_object_from_code
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME  // save ref containing registers for GC
    mov %esp, %edx                // remember SP
    // Outgoing argument set up
    pushl %eax                    // alignment padding
    pushl %edx                    // pass SP
    pushl %fs:THREAD_SELF_OFFSET  // pass Thread::Current()
    pushl %eax                    // pass arg1
    call SYMBOL(artLockObjectFromCode)    // (Object*, Thread*, SP)
    addl 16, %esp        // pop arguments
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME  // restore frame up to return address
    ret

DEFINE_FUNCTION art_unlock_object_from_code
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME  // save ref containing registers for GC
    mov %esp, %edx                // remember SP
    // Outgoing argument set up
    pushl %eax                    // alignment padding
    pushl %edx                    // pass SP
    pushl %fs:THREAD_SELF_OFFSET  // pass Thread::Current()
    pushl %eax                    // pass arg1
    call SYMBOL(artUnlockObjectFromCode)  // (Object*, Thread*, SP)
    addl 16, %esp                 // pop arguments
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME  // restore frame up to return address
    testl %eax, %eax              // eax == 0 ?
    jnz 1f
    ret
1:
    DELIVER_PENDING_EXCEPTION

DEFINE_FUNCTION art_handle_fill_data_from_code
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME  // save ref containing registers for GC
    mov %esp, %edx                // remember SP
    // Outgoing argument set up
    pushl %edx                    // pass SP
    pushl %fs:THREAD_SELF_OFFSET  // pass Thread::Current()
    pushl %ecx                    // pass arg2
    pushl %eax                    // pass arg1
    call SYMBOL(artHandleFillArrayDataFromCode)  // (Array* array, const uint16_t* table, Thread*, SP)
    addl 16, %esp        // pop arguments
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME  // restore frame up to return address
    testl %eax, %eax               // eax == 0 ?
    jnz 1f
    ret
1:
    DELIVER_PENDING_EXCEPTION

DEFINE_FUNCTION art_is_assignable_from_code
    pushl %eax                    // alignment padding
    pushl %ecx                    // pass arg2
    pushl %eax                    // pass arg1
    call SYMBOL(artIsAssignableFromCode)  // (Class* a, Class* b, Thread*, SP)
    addl 12, %esp                 // pop arguments
    ret

DEFINE_FUNCTION art_memcpy
    pushl %edx                    // pass arg3
    pushl %ecx                    // pass arg2
    pushl %eax                    // pass arg1
    call SYMBOL(memcpy)           // (void*, const void*, size_t)
    addl 12, %esp                 // pop arguments
    ret

DEFINE_FUNCTION art_check_cast_from_code
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME  // save ref containing registers for GC
    mov %esp, %edx                // remember SP
    // Outgoing argument set up
    pushl %edx                    // pass SP
    pushl %fs:THREAD_SELF_OFFSET  // pass Thread::Current()
    pushl %ecx                    // pass arg2
    pushl %eax                    // pass arg1
    call SYMBOL(artCheckCastFromCode)  // (Class* a, Class* b, Thread*, SP)
    addl 16, %esp                 // pop arguments
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME  // restore frame up to return address
    testl %eax, %eax              // eax == 0 ?
    jnz 1f
    ret
1:
    DELIVER_PENDING_EXCEPTION

DEFINE_FUNCTION art_idiv_from_code
    cdq         // edx:eax = sign extend eax
    idiv %ecx   // (edx,eax) = (edx:eax % ecx, edx:eax / ecx)
    ret

DEFINE_FUNCTION art_idivmod_from_code
    cdq         // edx:eax = sign extend eax
    idiv %ecx   // (edx,eax) = (edx:eax % ecx, edx:eax / ecx)
    movl %eax, %edx
    ret

DEFINE_FUNCTION art_can_put_array_element_from_code
    test %eax, %eax               // Null is trivially storable
    jz   1f
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME  // save ref containing registers for GC
    mov %esp, %edx                // remember SP
    // Outgoing argument set up
    pushl %edx                    // pass SP
    pushl %fs:THREAD_SELF_OFFSET  // pass Thread::Current()
    pushl %ecx                    // pass arg2
    pushl %eax                    // pass arg1
    call SYMBOL(artCanPutArrayElementFromCode)  // (Object* element, Class* array_class, Thread*, SP)
    addl 16, %esp                 // pop arguments
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME  // restore frame up to return address
    testl %eax, %eax              // eax == 0 ?
    jnz 2f
1:
    ret
2:
    DELIVER_PENDING_EXCEPTION

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
UNIMPLEMENTED art_initialize_type_and_verify_access_from_code
UNIMPLEMENTED art_initialize_type_from_code
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
UNIMPLEMENTED art_indexof
UNIMPLEMENTED art_memcmp16
UNIMPLEMENTED art_string_compareto
