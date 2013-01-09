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
    #define MACRO3(macro_name, macro_arg1, macro_args2, macro_args3) .macro macro_name
    #define END_MACRO .endmacro

    // Mac OS' as(1) uses $0, $1, and so on for macro arguments, and function names
    // are mangled with an extra underscore prefix. The use of $x for arguments
    // mean that literals need to be represented with $$x in macros.
    #define SYMBOL(name) _ ## name
    #define VAR(name,index) SYMBOL($index)
    #define CALL_MACRO(name,index) $index
    #define LITERAL(value) $value
    #define MACRO_LITERAL(value) $$value
#else
    // Regular gas(1) lets you name macro parameters.
    #define MACRO0(macro_name) .macro macro_name
    #define MACRO1(macro_name, macro_arg1) .macro macro_name macro_arg1
    #define MACRO2(macro_name, macro_arg1, macro_arg2) .macro macro_name macro_arg1, macro_arg2
    #define MACRO3(macro_name, macro_arg1, macro_arg2, macro_arg3) .macro macro_name macro_arg1, macro_arg2, macro_arg3
    #define END_MACRO .endm

    // Regular gas(1) uses \argument_name for macro arguments.
    // We need to turn on alternate macro syntax so we can use & instead or the preprocessor
    // will screw us by inserting a space between the \ and the name. Even in this mode there's
    // no special meaning to $, so literals are still just $x.
    .altmacro
    #define SYMBOL(name) name
    #define VAR(name,index) name&
    #define CALL_MACRO(name,index) name&
    #define LITERAL(value) $value
    #define MACRO_LITERAL(value) $value
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
    subl  MACRO_LITERAL(16), %esp  // Grow stack by 4 words, bottom word will hold Method*
END_MACRO

    /*
     * Macro that sets up the callee save frame to conform with
     * Runtime::CreateCalleeSaveMethod(kRefsOnly)
     */
MACRO0(SETUP_REF_ONLY_CALLEE_SAVE_FRAME)
    pushl %edi  // Save callee saves (ebx is saved/restored by the upcall)
    pushl %esi
    pushl %ebp
    subl  MACRO_LITERAL(16), %esp  // Grow stack by 4 words, bottom word will hold Method*
END_MACRO

MACRO0(RESTORE_REF_ONLY_CALLEE_SAVE_FRAME)
    addl MACRO_LITERAL(28), %esp  // Unwind stack up to return address
END_MACRO

    /*
     * Macro that sets up the callee save frame to conform with
     * Runtime::CreateCalleeSaveMethod(kRefsAndArgs)
     */
MACRO0(SETUP_REF_AND_ARGS_CALLEE_SAVE_FRAME)
    pushl %edi  // Save callee saves
    pushl %esi
    pushl %ebp
    pushl %ebx  // Save args
    pushl %edx
    pushl %ecx
    pushl %eax  // Align stack, eax will be clobbered by Method*
END_MACRO

MACRO0(RESTORE_REF_AND_ARGS_CALLEE_SAVE_FRAME)
    addl MACRO_LITERAL(4), %esp  // Remove padding
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
    subl  MACRO_LITERAL(8), %esp             // Alignment padding
    pushl %ecx                               // pass SP
    pushl %fs:THREAD_SELF_OFFSET             // pass Thread::Current()
    call SYMBOL(artDeliverPendingExceptionFromCode)  // artDeliverPendingExceptionFromCode(Thread*, SP)
    int3                                     // unreached
END_MACRO

MACRO2(NO_ARG_RUNTIME_EXCEPTION, c_name, cxx_name)
    .globl VAR(c_name, 0)
    ALIGN_FUNCTION_ENTRY
VAR(c_name, 0):
    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME  // save all registers as basis for long jump context
    mov %esp, %ecx
    // Outgoing argument set up
    subl  MACRO_LITERAL(8), %esp  // alignment padding
    pushl %ecx                    // pass SP
    pushl %fs:THREAD_SELF_OFFSET  // pass Thread::Current()
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
    pushl %eax                    // alignment padding
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
    subl MACRO_LITERAL(12), %esp  // alignment padding
    pushl %edx                    // pass SP
    pushl %fs:THREAD_SELF_OFFSET  // pass Thread::Current()
    pushl 32(%edx)                // pass caller Method*
    pushl %ecx                    // pass arg2
    pushl %eax                    // pass arg1
    call VAR(cxx_name, 1)         // cxx_name(arg1, arg2, arg3, Thread*, SP)
    movl %edx, %edi               // save code pointer in EDI
    addl MACRO_LITERAL(36), %esp  // Pop arguments skip eax
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
    addl MACRO_LITERAL(4), %esp   // Pop code pointer off stack
    DELIVER_PENDING_EXCEPTION
END_MACRO

INVOKE_TRAMPOLINE art_invoke_interface_trampoline, artInvokeInterfaceTrampoline
INVOKE_TRAMPOLINE art_invoke_interface_trampoline_with_access_check, artInvokeInterfaceTrampolineWithAccessCheck

INVOKE_TRAMPOLINE art_invoke_static_trampoline_with_access_check, artInvokeStaticTrampolineWithAccessCheck
INVOKE_TRAMPOLINE art_invoke_direct_trampoline_with_access_check, artInvokeDirectTrampolineWithAccessCheck
INVOKE_TRAMPOLINE art_invoke_super_trampoline_with_access_check, artInvokeSuperTrampolineWithAccessCheck
INVOKE_TRAMPOLINE art_invoke_virtual_trampoline_with_access_check, artInvokeVirtualTrampolineWithAccessCheck

MACRO3(NO_ARG_DOWNCALL, c_name, cxx_name, return_macro)
    .globl VAR(c_name, 0)
    ALIGN_FUNCTION_ENTRY
VAR(c_name, 0):
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME  // save ref containing registers for GC
    mov %esp, %edx                // remember SP
    // Outgoing argument set up
    subl MACRO_LITERAL(8), %esp   // push padding
    pushl %edx                    // pass SP
    pushl %fs:THREAD_SELF_OFFSET  // pass Thread::Current()
    call VAR(cxx_name, 1)         // cxx_name(Thread*, SP)
    addl MACRO_LITERAL(16), %esp  // pop arguments
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME  // restore frame up to return address
    CALL_MACRO(return_macro, 2)   // return or deliver exception
END_MACRO

MACRO3(ONE_ARG_DOWNCALL, c_name, cxx_name, return_macro)
    .globl VAR(c_name, 0)
    ALIGN_FUNCTION_ENTRY
VAR(c_name, 0):
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME  // save ref containing registers for GC
    mov %esp, %edx                // remember SP
    // Outgoing argument set up
    pushl %eax                    // push padding
    pushl %edx                    // pass SP
    pushl %fs:THREAD_SELF_OFFSET  // pass Thread::Current()
    pushl %eax                    // pass arg1
    call VAR(cxx_name, 1)         // cxx_name(arg1, Thread*, SP)
    addl MACRO_LITERAL(16), %esp  // pop arguments
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME  // restore frame up to return address
    CALL_MACRO(return_macro, 2)   // return or deliver exception
END_MACRO

MACRO3(TWO_ARG_DOWNCALL, c_name, cxx_name, return_macro)
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
    call VAR(cxx_name, 1)         // cxx_name(arg1, arg2, Thread*, SP)
    addl MACRO_LITERAL(16), %esp  // pop arguments
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME  // restore frame up to return address
    CALL_MACRO(return_macro, 2)   // return or deliver exception
END_MACRO

MACRO3(THREE_ARG_DOWNCALL, c_name, cxx_name, return_macro)
    .globl VAR(c_name, 0)
    ALIGN_FUNCTION_ENTRY
VAR(c_name, 0):
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME  // save ref containing registers for GC
    mov %esp, %ebx                // remember SP
    // Outgoing argument set up
    subl MACRO_LITERAL(12), %esp  // alignment padding
    pushl %ebx                    // pass SP
    pushl %fs:THREAD_SELF_OFFSET  // pass Thread::Current()
    pushl %edx                    // pass arg3
    pushl %ecx                    // pass arg2
    pushl %eax                    // pass arg1
    call VAR(cxx_name, 1)         // cxx_name(arg1, arg2, arg3, Thread*, SP)
    addl MACRO_LITERAL(32), %esp  // pop arguments
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME  // restore frame up to return address
    CALL_MACRO(return_macro, 2)   // return or deliver exception
END_MACRO

MACRO0(RETURN_IF_EAX_NOT_ZERO)
    testl %eax, %eax               // eax == 0 ?
    jz  1f                         // if eax == 0 goto 1
    ret                            // return
1:                                 // deliver exception on current thread
    DELIVER_PENDING_EXCEPTION
END_MACRO

MACRO0(RETURN_IF_EAX_ZERO)
    testl %eax, %eax               // eax == 0 ?
    jnz  1f                        // if eax != 0 goto 1
    ret                            // return
1:                                 // deliver exception on current thread
    DELIVER_PENDING_EXCEPTION
END_MACRO

MACRO0(RETURN_OR_DELIVER_PENDING_EXCEPTION)
    mov %fs:THREAD_EXCEPTION_OFFSET, %ebx // get exception field
    testl %ebx, %ebx               // ebx == 0 ?
    jnz 1f                         // if ebx != 0 goto 1
    ret                            // return
1:                                 // deliver exception on current thread
    DELIVER_PENDING_EXCEPTION
END_MACRO

TWO_ARG_DOWNCALL art_alloc_object_from_code, artAllocObjectFromCode, RETURN_IF_EAX_NOT_ZERO
TWO_ARG_DOWNCALL art_alloc_object_from_code_with_access_check, artAllocObjectFromCodeWithAccessCheck, RETURN_IF_EAX_NOT_ZERO
THREE_ARG_DOWNCALL art_alloc_array_from_code, artAllocArrayFromCode, RETURN_IF_EAX_NOT_ZERO
THREE_ARG_DOWNCALL art_alloc_array_from_code_with_access_check, artAllocArrayFromCodeWithAccessCheck, RETURN_IF_EAX_NOT_ZERO
THREE_ARG_DOWNCALL art_check_and_alloc_array_from_code, artCheckAndAllocArrayFromCode, RETURN_IF_EAX_NOT_ZERO
THREE_ARG_DOWNCALL art_check_and_alloc_array_from_code_with_access_check, artCheckAndAllocArrayFromCodeWithAccessCheck, RETURN_IF_EAX_NOT_ZERO

TWO_ARG_DOWNCALL art_resolve_string_from_code, artResolveStringFromCode, RETURN_IF_EAX_NOT_ZERO
TWO_ARG_DOWNCALL art_initialize_static_storage_from_code, artInitializeStaticStorageFromCode, RETURN_IF_EAX_NOT_ZERO
TWO_ARG_DOWNCALL art_initialize_type_from_code, artInitializeTypeFromCode, RETURN_IF_EAX_NOT_ZERO
TWO_ARG_DOWNCALL art_initialize_type_and_verify_access_from_code, artInitializeTypeAndVerifyAccessFromCode, RETURN_IF_EAX_NOT_ZERO

DEFINE_FUNCTION art_update_debugger
    mov  %eax, %ebx               // stash away eax so that it's saved as if it were an argument
    SETUP_REF_AND_ARGS_CALLEE_SAVE_FRAME
    pushl %esp                    // pass arg2 (sp)
    pushl %fs:THREAD_SELF_OFFSET  // pass Thread::Current()
    pushl %edx                    // pass arg0 (dex pc)
    call SYMBOL(artUpdateDebuggerFromCode) // artUpdateDebuggerFromCode(int32_t, Thread*, Method**)
    RESTORE_REF_AND_ARGS_CALLEE_SAVE_FRAME
    mov  %ebx, %eax               // restore original eax
    ret

DEFINE_FUNCTION art_get_and_clear_exception
    subl LITERAL(8), %esp         // alignment padding
    pushl %fs:THREAD_SELF_OFFSET  // pass Thread::Current()
    call SYMBOL(GetAndClearException)  // (Thread*)
    addl LITERAL(12), %esp        // pop arguments
    ret

ONE_ARG_DOWNCALL art_lock_object_from_code, artLockObjectFromCode, ret
ONE_ARG_DOWNCALL art_unlock_object_from_code, artUnlockObjectFromCode, RETURN_IF_EAX_ZERO

TWO_ARG_DOWNCALL art_handle_fill_data_from_code, artHandleFillArrayDataFromCode, RETURN_IF_EAX_ZERO

DEFINE_FUNCTION art_is_assignable_from_code
    pushl %eax                    // alignment padding
    pushl %ecx                    // pass arg2
    pushl %eax                    // pass arg1
    call SYMBOL(artIsAssignableFromCode)  // (Class* a, Class* b, Thread*, SP)
    addl LITERAL(12), %esp        // pop arguments
    ret

DEFINE_FUNCTION art_memcpy
    pushl %edx                    // pass arg3
    pushl %ecx                    // pass arg2
    pushl %eax                    // pass arg1
    call SYMBOL(memcpy)           // (void*, const void*, size_t)
    addl LITERAL(12), %esp        // pop arguments
    ret

TWO_ARG_DOWNCALL art_check_cast_from_code, artCheckCastFromCode, RETURN_IF_EAX_ZERO
TWO_ARG_DOWNCALL art_can_put_array_element_from_code, artCanPutArrayElementFromCode, RETURN_IF_EAX_ZERO

NO_ARG_DOWNCALL art_test_suspend, artTestSuspendFromCode, ret

DEFINE_FUNCTION art_fmod_from_code
    subl LITERAL(12), %esp        // alignment padding
    pushl %ebx                    // pass arg4 b.hi
    pushl %edx                    // pass arg3 b.lo
    pushl %ecx                    // pass arg2 a.hi
    pushl %eax                    // pass arg1 a.lo
    call SYMBOL(fmod)             // (jdouble a, jdouble b)
    fstpl (%esp)                  // pop return value off fp stack
    movsd (%esp), %xmm0           // place into %xmm0
    addl LITERAL(28), %esp        // pop arguments
    ret

DEFINE_FUNCTION art_fmodf_from_code
    pushl %eax                    // alignment padding
    pushl %ecx                    // pass arg2 b
    pushl %eax                    // pass arg1 a
    call SYMBOL(fmodf)            // (jfloat a, jfloat b)
    fstps (%esp)                  // pop return value off fp stack
    movss (%esp), %xmm0           // place into %xmm0
    addl LITERAL(12), %esp        // pop arguments
    ret

DEFINE_FUNCTION art_l2d_from_code
    pushl %eax                    // alignment padding
    pushl %ecx                    // pass arg2 a.hi
    pushl %eax                    // pass arg1 a.lo
    call SYMBOL(art_l2d)          // (jlong a)
    fstpl (%esp)                  // pop return value off fp stack
    movsd (%esp), %xmm0           // place into %xmm0
    addl LITERAL(12), %esp        // pop arguments
    ret

DEFINE_FUNCTION art_l2f_from_code
    pushl %eax                    // alignment padding
    pushl %ecx                    // pass arg2 a.hi
    pushl %eax                    // pass arg1 a.lo
    call SYMBOL(art_l2f)          // (jlong a)
    fstps (%esp)                  // pop return value off fp stack
    movss (%esp), %xmm0           // place into %xmm0
    addl LITERAL(12), %esp        // pop arguments
    ret

DEFINE_FUNCTION art_d2l_from_code
    pushl %eax                    // alignment padding
    pushl %ecx                    // pass arg2 a.hi
    pushl %eax                    // pass arg1 a.lo
    call SYMBOL(art_d2l)          // (jdouble a)
    addl LITERAL(12), %esp        // pop arguments
    ret

DEFINE_FUNCTION art_f2l_from_code
    subl LITERAL(8), %esp         // alignment padding
    pushl %eax                    // pass arg1 a
    call SYMBOL(art_f2l)          // (jfloat a)
    addl LITERAL(12), %esp        // pop arguments
    ret

DEFINE_FUNCTION art_idivmod_from_code
    cmpl LITERAL(0x80000000), %eax
    je check_arg2  // special case
args_ok:
    cdq         // edx:eax = sign extend eax
    idiv %ecx   // (edx,eax) = (edx:eax % ecx, edx:eax / ecx)
    ret
check_arg2:
    cmpl LITERAL(-1), %ecx
    jne args_ok
    xorl %edx, %edx
    ret         // eax already holds min int

DEFINE_FUNCTION art_ldiv_from_code
    subl LITERAL(12), %esp        // alignment padding
    pushl %ebx                    // pass arg4 b.hi
    pushl %edx                    // pass arg3 b.lo
    pushl %ecx                    // pass arg2 a.hi
    pushl %eax                    // pass arg1 a.lo
    call SYMBOL(artLdivFromCode)  // (jlong a, jlong b)
    addl LITERAL(28), %esp        // pop arguments
    ret

DEFINE_FUNCTION art_ldivmod_from_code
    subl LITERAL(12), %esp        // alignment padding
    pushl %ebx                    // pass arg4 b.hi
    pushl %edx                    // pass arg3 b.lo
    pushl %ecx                    // pass arg2 a.hi
    pushl %eax                    // pass arg1 a.lo
    call SYMBOL(artLdivmodFromCode) // (jlong a, jlong b)
    addl LITERAL(28), %esp        // pop arguments
    ret

DEFINE_FUNCTION art_lmul_from_code
    subl LITERAL(12), %esp        // alignment padding
    pushl %ebx                    // pass arg4 b.hi
    pushl %edx                    // pass arg3 b.lo
    pushl %ecx                    // pass arg2 a.hi
    pushl %eax                    // pass arg1 a.lo
    call SYMBOL(artLmulFromCode)  // (jlong a, jlong b)
    addl LITERAL(28), %esp        // pop arguments
    ret

DEFINE_FUNCTION art_lshl_from_code
    // ecx:eax << edx
    xchg %edx, %ecx
    shld %cl,%eax,%edx
    shl  %cl,%eax
    test LITERAL(32), %cl
    jz  1f
    mov %eax, %edx
    xor %eax, %eax
1:
    ret

DEFINE_FUNCTION art_lshr_from_code
    // ecx:eax >> edx
    xchg %edx, %ecx
    shrd %cl,%edx,%eax
    sar  %cl,%edx
    test LITERAL(32),%cl
    jz  1f
    mov %edx, %eax
    sar LITERAL(31), %edx
1:
    ret

DEFINE_FUNCTION art_lushr_from_code
    // ecx:eax >>> edx
    xchg %edx, %ecx
    shrd %cl,%edx,%eax
    shr  %cl,%edx
    test LITERAL(32),%cl
    jz  1f
    mov %edx, %eax
    xor %edx, %edx
1:
    ret

DEFINE_FUNCTION art_set32_instance_from_code
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME       // save ref containing registers for GC
    mov %esp, %ebx                // remember SP
    subl LITERAL(8), %esp         // alignment padding
    pushl %ebx                    // pass SP
    pushl %fs:THREAD_SELF_OFFSET  // pass Thread::Current()
    mov 32(%ebx), %ebx            // get referrer
    pushl %ebx                    // pass referrer
    pushl %edx                    // pass new_val
    pushl %ecx                    // pass object
    pushl %eax                    // pass field_idx
    call SYMBOL(artSet32InstanceFromCode)  // (field_idx, Object*, new_val, referrer, Thread*, SP)
    addl LITERAL(32), %esp        // pop arguments
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME     // restore frame up to return address
    RETURN_IF_EAX_ZERO            // return or deliver exception

DEFINE_FUNCTION art_set64_instance_from_code
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME       // save ref containing registers for GC
    subl LITERAL(8), %esp         // alignment padding
    pushl %esp                    // pass SP-8
    addl LITERAL(8), (%esp)       // fix SP on stack by adding 8
    pushl %fs:THREAD_SELF_OFFSET  // pass Thread::Current()
    pushl %ebx                    // pass high half of new_val
    pushl %edx                    // pass low half of new_val
    pushl %ecx                    // pass object
    pushl %eax                    // pass field_idx
    call SYMBOL(artSet64InstanceFromCode)  // (field_idx, Object*, new_val, Thread*, SP)
    addl LITERAL(32), %esp        // pop arguments
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME     // restore frame up to return address
    RETURN_IF_EAX_ZERO            // return or deliver exception

DEFINE_FUNCTION art_set_obj_instance_from_code
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME       // save ref containing registers for GC
    mov %esp, %ebx                // remember SP
    subl LITERAL(8), %esp         // alignment padding
    pushl %ebx                    // pass SP
    pushl %fs:THREAD_SELF_OFFSET  // pass Thread::Current()
    mov 32(%ebx), %ebx            // get referrer
    pushl %ebx                    // pass referrer
    pushl %edx                    // pass new_val
    pushl %ecx                    // pass object
    pushl %eax                    // pass field_idx
    call SYMBOL(artSetObjInstanceFromCode) // (field_idx, Object*, new_val, referrer, Thread*, SP)
    addl LITERAL(32), %esp        // pop arguments
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME     // restore frame up to return address
    RETURN_IF_EAX_ZERO            // return or deliver exception

DEFINE_FUNCTION art_get32_instance_from_code
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME       // save ref containing registers for GC
    mov %esp, %ebx                // remember SP
    mov 32(%esp), %edx            // get referrer
    subl LITERAL(12), %esp        // alignment padding
    pushl %ebx                    // pass SP
    pushl %fs:THREAD_SELF_OFFSET  // pass Thread::Current()
    pushl %edx                    // pass referrer
    pushl %ecx                    // pass object
    pushl %eax                    // pass field_idx
    call SYMBOL(artGet32InstanceFromCode)  // (field_idx, Object*, referrer, Thread*, SP)
    addl LITERAL(32), %esp        // pop arguments
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME     // restore frame up to return address
    RETURN_OR_DELIVER_PENDING_EXCEPTION    // return or deliver exception

DEFINE_FUNCTION art_get64_instance_from_code
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME       // save ref containing registers for GC
    mov %esp, %ebx                // remember SP
    mov 32(%esp), %edx            // get referrer
    subl LITERAL(12), %esp        // alignment padding
    pushl %ebx                    // pass SP
    pushl %fs:THREAD_SELF_OFFSET  // pass Thread::Current()
    pushl %edx                    // pass referrer
    pushl %ecx                    // pass object
    pushl %eax                    // pass field_idx
    call SYMBOL(artGet64InstanceFromCode)  // (field_idx, Object*, referrer, Thread*, SP)
    addl LITERAL(32), %esp        // pop arguments
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME     // restore frame up to return address
    RETURN_OR_DELIVER_PENDING_EXCEPTION    // return or deliver exception

DEFINE_FUNCTION art_get_obj_instance_from_code
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME       // save ref containing registers for GC
    mov %esp, %ebx                // remember SP
    mov 32(%esp), %edx            // get referrer
    subl LITERAL(12), %esp        // alignment padding
    pushl %ebx                    // pass SP
    pushl %fs:THREAD_SELF_OFFSET  // pass Thread::Current()
    pushl %edx                    // pass referrer
    pushl %ecx                    // pass object
    pushl %eax                    // pass field_idx
    call SYMBOL(artGetObjInstanceFromCode) // (field_idx, Object*, referrer, Thread*, SP)
    addl LITERAL(32), %esp        // pop arguments
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME     // restore frame up to return address
    RETURN_OR_DELIVER_PENDING_EXCEPTION    // return or deliver exception

DEFINE_FUNCTION art_set32_static_from_code
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME       // save ref containing registers for GC
    mov %esp, %ebx                // remember SP
    mov 32(%esp), %edx            // get referrer
    subl LITERAL(12), %esp        // alignment padding
    pushl %ebx                    // pass SP
    pushl %fs:THREAD_SELF_OFFSET  // pass Thread::Current()
    pushl %edx                    // pass referrer
    pushl %ecx                    // pass new_val
    pushl %eax                    // pass field_idx
    call SYMBOL(artSet32StaticFromCode)    // (field_idx, new_val, referrer, Thread*, SP)
    addl LITERAL(32), %esp        // pop arguments
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME     // restore frame up to return address
    RETURN_IF_EAX_ZERO            // return or deliver exception

DEFINE_FUNCTION art_set64_static_from_code
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME       // save ref containing registers for GC
    mov %esp, %ebx                // remember SP
    subl LITERAL(8), %esp         // alignment padding
    pushl %ebx                    // pass SP
    pushl %fs:THREAD_SELF_OFFSET  // pass Thread::Current()
    mov 32(%ebx), %ebx            // get referrer
    pushl %edx                    // pass high half of new_val
    pushl %ecx                    // pass low half of new_val
    pushl %ebx                    // pass referrer
    pushl %eax                    // pass field_idx
    call SYMBOL(artSet64StaticFromCode)    // (field_idx, referrer, new_val, Thread*, SP)
    addl LITERAL(32), %esp        // pop arguments
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME     // restore frame up to return address
    RETURN_IF_EAX_ZERO            // return or deliver exception

DEFINE_FUNCTION art_set_obj_static_from_code
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME       // save ref containing registers for GC
    mov %esp, %ebx                // remember SP
    mov 32(%esp), %edx            // get referrer
    subl LITERAL(12), %esp        // alignment padding
    pushl %ebx                    // pass SP
    pushl %fs:THREAD_SELF_OFFSET  // pass Thread::Current()
    pushl %edx                    // pass referrer
    pushl %ecx                    // pass new_val
    pushl %eax                    // pass field_idx
    call SYMBOL(artSetObjStaticFromCode)   // (field_idx, new_val, referrer, Thread*, SP)
    addl LITERAL(32), %esp        // pop arguments
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME     // restore frame up to return address
    RETURN_IF_EAX_ZERO            // return or deliver exception

DEFINE_FUNCTION art_get32_static_from_code
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME       // save ref containing registers for GC
    mov %esp, %edx                // remember SP
    mov 32(%esp), %ecx            // get referrer
    pushl %edx                    // pass SP
    pushl %fs:THREAD_SELF_OFFSET  // pass Thread::Current()
    pushl %ecx                    // pass referrer
    pushl %eax                    // pass field_idx
    call SYMBOL(artGet32StaticFromCode)    // (field_idx, referrer, Thread*, SP)
    addl LITERAL(16), %esp        // pop arguments
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME     // restore frame up to return address
    RETURN_OR_DELIVER_PENDING_EXCEPTION    // return or deliver exception

DEFINE_FUNCTION art_get64_static_from_code
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME       // save ref containing registers for GC
    mov %esp, %edx                // remember SP
    mov 32(%esp), %ecx            // get referrer
    pushl %edx                    // pass SP
    pushl %fs:THREAD_SELF_OFFSET  // pass Thread::Current()
    pushl %ecx                    // pass referrer
    pushl %eax                    // pass field_idx
    call SYMBOL(artGet64StaticFromCode)    // (field_idx, referrer, Thread*, SP)
    addl LITERAL(16), %esp        // pop arguments
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME     // restore frame up to return address
    RETURN_OR_DELIVER_PENDING_EXCEPTION    // return or deliver exception

DEFINE_FUNCTION art_get_obj_static_from_code
    SETUP_REF_ONLY_CALLEE_SAVE_FRAME       // save ref containing registers for GC
    mov %esp, %edx                // remember SP
    mov 32(%esp), %ecx            // get referrer
    pushl %edx                    // pass SP
    pushl %fs:THREAD_SELF_OFFSET  // pass Thread::Current()
    pushl %ecx                    // pass referrer
    pushl %eax                    // pass field_idx
    call SYMBOL(artGetObjStaticFromCode)   // (field_idx, referrer, Thread*, SP)
    addl LITERAL(16), %esp        // pop arguments
    RESTORE_REF_ONLY_CALLEE_SAVE_FRAME     // restore frame up to return address
    RETURN_OR_DELIVER_PENDING_EXCEPTION    // return or deliver exception

DEFINE_FUNCTION art_proxy_invoke_handler
    SETUP_REF_AND_ARGS_CALLEE_SAVE_FRAME   // save frame
    lea 8(%esp), %ebx             // pointer to r2/r3/LR/caller's Method**/out-args as second arg
    pushl %ebx                    // pass args
    pushl %fs:THREAD_SELF_OFFSET  // pass Thread::Current()
    pushl %ecx                    // pass receiver
    pushl %eax                    // pass proxy method
    call SYMBOL(artProxyInvokeHandler)     // (proxy method, receiver, Thread*, args...)
    mov 24(%esp), %eax            // get ret0 which was written into r2 on the stack
    mov 28(%esp), %edx            // get ret1 which was written into r3 on the stack
    movsd 24(%esp), %xmm0         // get ret0/ret1 from stack for floating point
    addl LITERAL(44), %esp        // pop arguments
    RETURN_OR_DELIVER_PENDING_EXCEPTION    // return or deliver exception

DEFINE_FUNCTION art_instrumentation_entry_from_code
    ret
DEFINE_FUNCTION art_instrumentation_exit_from_code
    ret

DEFINE_FUNCTION art_deoptimize
    ret

    /*
     * String's indexOf.
     *
     * On entry:
     *    eax:   string object (known non-null)
     *    ecx:   char to match (known <= 0xFFFF)
     *    edx:   Starting offset in string data
     */
DEFINE_FUNCTION art_indexof
    pushl %edi                    // push callee save reg
    mov STRING_COUNT_OFFSET(%eax), %ebx
    mov STRING_VALUE_OFFSET(%eax), %edi
    mov STRING_OFFSET_OFFSET(%eax), %eax
    testl %edx, %edx              // check if start < 0
    jl   clamp_min
clamp_done:
    cmpl %ebx, %edx               // check if start >= count
    jge  not_found
    lea  STRING_DATA_OFFSET(%edi, %eax, 2), %edi  // build a pointer to the start of string data
    mov  %edi, %eax               // save a copy in eax to later compute result
    lea  (%edi, %edx, 2), %edi    // build pointer to start of data to compare
    subl  %edx, %ebx              // compute iteration count
    /*
     * At this point we have:
     *   eax: original start of string data
     *   ecx: char to compare
     *   ebx: length to compare
     *   edi: start of data to test
     */
    mov  %eax, %edx
    mov  %ecx, %eax               // put char to match in %eax
    mov  %ebx, %ecx               // put length to compare in %ecx
    repne scasw                   // find %ax, starting at [%edi], up to length %ecx
    jne  not_found
    subl %edx, %edi
    sar  LITERAL(1), %edi
    decl %edi                     // index = ((curr_ptr - orig_ptr) / 2) - 1
    mov  %edi, %eax
    popl %edi                     // pop callee save reg
    ret
    .balign 16
not_found:
    mov  LITERAL(-1), %eax        // return -1 (not found)
    popl %edi                     // pop callee save reg
    ret
clamp_min:
    xor  %edx, %edx               // clamp start to 0
    jmp  clamp_done

    /*
     * String's compareTo.
     *
     * On entry:
     *    eax:   this string object (known non-null)
     *    ecx:   comp string object (known non-null)
     */
DEFINE_FUNCTION art_string_compareto
    pushl %esi                    // push callee save reg
    pushl %edi                    // push callee save reg
    mov STRING_COUNT_OFFSET(%eax), %edx
    mov STRING_COUNT_OFFSET(%ecx), %ebx
    mov STRING_VALUE_OFFSET(%eax), %esi
    mov STRING_VALUE_OFFSET(%ecx), %edi
    mov STRING_OFFSET_OFFSET(%eax), %eax
    mov STRING_OFFSET_OFFSET(%ecx), %ecx
    /* Build pointers to the start of string data */
    lea  STRING_DATA_OFFSET(%esi, %eax, 2), %esi
    lea  STRING_DATA_OFFSET(%edi, %ecx, 2), %edi
    /* Calculate min length and count diff */
    mov   %edx, %ecx
    mov   %edx, %eax
    subl  %ebx, %eax
    cmovg %ebx, %ecx
    /*
     * At this point we have:
     *   eax: value to return if first part of strings are equal
     *   ecx: minimum among the lengths of the two strings
     *   esi: pointer to this string data
     *   edi: pointer to comp string data
     */
    repe cmpsw                    // find nonmatching chars in [%esi] and [%edi], up to length %ecx
    jne not_equal
    popl  %edi                    // pop callee save reg
    popl  %esi                    // pop callee save reg
    ret
    .balign 16
not_equal:
    movzwl  -2(%esi), %eax        // get last compared char from this string
    movzwl  -2(%edi), %ecx        // get last compared char from comp string
    subl  %ecx, %eax              // return the difference
    popl  %edi                    // pop callee save reg
    popl  %esi                    // pop callee save reg
    ret

MACRO1(UNIMPLEMENTED,name)
    .globl VAR(name, 0)
    ALIGN_FUNCTION_ENTRY
VAR(name, 0):
    int3
END_MACRO

    // TODO: implement these!
UNIMPLEMENTED art_memcmp16
