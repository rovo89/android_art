/*
 * Unlock an object.
 *
 * Exceptions that occur when unlocking a monitor need to appear as
 * if they happened at the following instruction.  See the Dalvik
 * instruction spec.
 */
    /* monitor-exit vAA */
    EXPORT_PC
    GET_VREG %ecx, rINST
    movl    %ecx, OUT_ARG0(%esp)
    movl    rSELF, %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(artUnlockObjectFromCode) # (object, self)
    REFRESH_IBASE
    testl   %eax, %eax
    jnz     MterpException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
