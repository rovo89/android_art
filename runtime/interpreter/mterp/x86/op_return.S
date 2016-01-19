/*
 * Return a 32-bit value.
 *
 * for: return, return-object
 */
    /* op vAA */
    .extern MterpThreadFenceForConstructor
    call    MterpThreadFenceForConstructor
    GET_VREG %eax rINST                     # eax <- vAA
    xorl    %ecx, %ecx
    jmp     MterpReturn
