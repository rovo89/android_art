/*
 * Synchronize on an object.
 */
    /* monitor-enter vAA */
    EXPORT_PC
    GET_VREG %ecx, rINST
    movl    %ecx, OUT_ARG0(%esp)
    movl    rSELF, %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(artLockObjectFromCode)   # (object, self)
    REFRESH_IBASE
    testl   %eax, %eax
    jnz     MterpException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
