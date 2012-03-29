#include "asm_support.h"

#if defined(__APPLE__)
    // Mac OS X mangles the functions with an underscore prefix
    #define art_deliver_exception_from_code _art_deliver_exception_from_code
    #define art_proxy_invoke_handler _art_proxy_invoke_handler
    #define artDeliverExceptionFromCode _artDeliverExceptionFromCode
#endif

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
     * Runtime::CreateCalleeSaveMethod(...)
     */
.macro SETUP_SAVE_ALL_CALLEE_SAVE_FRAME
    pushl %edi  // Save callee saves (ebx is saved/restored by the upcall)
    pushl %esi
    pushl %ebp
    subl  $16, %esp  // Grow stack by 4 words, bottom word will hold Method*
.endm

.macro RESTORE_CALLEE_SAVE_FRAME
    addl $16, %esp  // Remove padding
    popl %ebp  // Restore callee saves
    popl %esi
    popl %edi
.endm

    /*
     * Macro that sets up the callee save frame to conform with
     * Runtime::CreateCalleeSaveMethod(...)
     */
.macro SETUP_REF_AND_ARG_CALLEE_SAVE_FRAME
    pushl %edi  // Save callee saves
    pushl %esi
    pushl %ebp
    pushl %ebx  // Save args
    pushl %edx
    pushl %ecx
    pushl %eax  // Align stack, eax will be clobbered by Method*
.endm

.macro RESTORE_REF_AND_ARG_CALLEE_SAVE_FRAME
    addl $16, %esp  // Remove padding
    popl %ebp  // Restore callee saves
    popl %esi
    popl %edi
.endm

    /*
     * Macro that set calls through to artDeliverPendingExceptionFromCode, where the pending
     * exception is Thread::Current()->exception_.
     */
.macro DELIVER_PENDING_EXCEPTION
    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME         // save callee saves for throw
    mov %esp, %ecx
    // Outgoing argument set up
    subl  $8, %esp                           // Alignment padding
    pushl %ecx                               // pass SP
    pushl %fs:THREAD_SELF_OFFSET             // pass Thread::Current()
    call artDeliverPendingExceptionFromCode  // artDeliverExceptionFromCode(Thread*, SP)
    int3
.endm

.macro NO_ARG_RUNTIME_EXCEPTION c_name, cxx_name
    .global \c_name
    .extern \cxx_name
    ALIGN_FUNCTION_ENTRY
\c_name:
    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME  // save all registers as basis for long jump context
    mov %esp, %ecx
    // Outgoing argument set up
    subl  $8, %esp                // alignment padding
    pushl %fs:THREAD_SELF_OFFSET  // pass Thread::Current()
    pushl %ecx                    // pass SP
    call \cxx_name                // \cxx_name(Thread*, SP)
    int3                          // unreached
.endm

.macro ONE_ARG_RUNTIME_EXCEPTION c_name, cxx_name
    .global \c_name
    .extern \cxx_name
    ALIGN_FUNCTION_ENTRY
\c_name:
    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME  // save all registers as basis for long jump context
    mov %esp, %ecx
    // Outgoing argument set up
    pushl $0                      // alignment padding
    pushl %ecx                    // pass SP
    pushl %fs:THREAD_SELF_OFFSET  // pass Thread::Current()
    pushl %eax                    // pass arg1
    call \cxx_name                // \cxx_name(arg1, Thread*, SP)
    int3                          // unreached
.endm

.macro TWO_ARG_RUNTIME_EXCEPTION c_name, cxx_name
    .global \c_name
    .extern \cxx_name
    ALIGN_FUNCTION_ENTRY
\c_name:
    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME  // save all registers as basis for long jump context
    mov %esp, %edx
    // Outgoing argument set up
    pushl %edx                    // pass SP
    pushl %fs:THREAD_SELF_OFFSET  // pass Thread::Current()
    pushl %eax                    // pass arg1
    pushl %ecx                    // pass arg2
    call \cxx_name                // \cxx_name(arg1, Thread*, SP)
    int3                          // unreached
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
    int3
.endm

INVOKE_TRAMPOLINE art_invoke_interface_trampoline, artInvokeInterfaceTrampoline
INVOKE_TRAMPOLINE art_invoke_interface_trampoline_with_access_check, artInvokeInterfaceTrampolineWithAccessCheck

INVOKE_TRAMPOLINE art_invoke_static_trampoline_with_access_check, artInvokeStaticTrampolineWithAccessCheck
INVOKE_TRAMPOLINE art_invoke_direct_trampoline_with_access_check, artInvokeDirectTrampolineWithAccessCheck
INVOKE_TRAMPOLINE art_invoke_super_trampoline_with_access_check, artInvokeSuperTrampolineWithAccessCheck
INVOKE_TRAMPOLINE art_invoke_virtual_trampoline_with_access_check, artInvokeVirtualTrampolineWithAccessCheck

    // TODO
    .globl art_proxy_invoke_handler
art_proxy_invoke_handler:
    int3

    .globl art_update_debugger
art_update_debugger:
    int3

   .globl art_test_suspend
art_test_suspend:
    int3

   .globl art_alloc_object_from_code
art_alloc_object_from_code:
    int3

   .globl art_alloc_object_from_code_with_access_check
art_alloc_object_from_code_with_access_check:
    int3

   .globl art_alloc_array_from_code
art_alloc_array_from_code:
    int3

   .globl art_alloc_array_from_code_with_access_check
art_alloc_array_from_code_with_access_check:
    int3

   .globl art_check_and_alloc_array_from_code
art_check_and_alloc_array_from_code:
    int3

   .globl art_check_and_alloc_array_from_code_with_access_check
art_check_and_alloc_array_from_code_with_access_check:
    int3

   .globl art_can_put_array_element_from_code
art_can_put_array_element_from_code:
    int3

   .globl art_check_cast_from_code
art_check_cast_from_code:
    int3

   .globl art_initialize_static_storage_from_code
art_initialize_static_storage_from_code:
    int3

   .globl art_initialize_type_and_verify_access_from_code
art_initialize_type_and_verify_access_from_code:
    int3

   .globl art_initialize_type_from_code
art_initialize_type_from_code:
    int3

   .globl art_resolve_string_from_code
art_resolve_string_from_code:
    int3

   .globl art_set32_instance_from_code
art_set32_instance_from_code:
    int3

   .globl art_set64_instance_from_code
art_set64_instance_from_code:
    int3

   .globl art_set_obj_instance_from_code
art_set_obj_instance_from_code:
    int3

   .globl art_get32_instance_from_code
art_get32_instance_from_code:
    int3

   .globl art_get64_instance_from_code
art_get64_instance_from_code:
    int3

   .globl art_get_obj_instance_from_code
art_get_obj_instance_from_code:
    int3

   .globl art_set32_static_from_code
art_set32_static_from_code:
    int3

   .globl art_set64_static_from_code
art_set64_static_from_code:
    int3

   .globl art_set_obj_static_from_code
art_set_obj_static_from_code:
    int3

   .globl art_get32_static_from_code
art_get32_static_from_code:
    int3

   .globl art_get64_static_from_code
art_get64_static_from_code:
    int3

   .globl art_get_obj_static_from_code
art_get_obj_static_from_code:
    int3

    .globl art_handle_fill_data_from_code
art_handle_fill_data_from_code:
    int3

    .globl art_lock_object_from_code
art_lock_object_from_code:
    int3

    .globl art_unlock_object_from_code
art_unlock_object_from_code:
    int3

    .globl art_indexof
art_indexof:
    int3

    .globl __memcmp16
__memcmp16:
    int3

    .globl art_string_compareto
art_string_compareto:
    int3
