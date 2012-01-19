#include "asm_support.h"

#if defined(__APPLE__)
    // Mac OS X mangles the functions with an underscore prefix
    #define art_deliver_exception_from_code _art_deliver_exception_from_code
    #define art_proxy_invoke_handler _art_proxy_invoke_handler
    #define artDeliverExceptionFromCode _artDeliverExceptionFromCode
#endif

    .globl art_deliver_exception_from_code
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

    // TODO
    .globl art_proxy_invoke_handler
art_proxy_invoke_handler:
    int3
