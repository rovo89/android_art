/*
 * Throw an exception object in the current thread.
 */
    /* throw vAA */
    EXPORT_PC
    GET_VREG %eax, rINSTq                   # eax<- vAA (exception object)
    testb   %al, %al
    jz      common_errNullObject
    movq    %rax, THREAD_EXCEPTION_OFFSET(rSELF)
    jmp     MterpException
