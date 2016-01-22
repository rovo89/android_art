/*
 * Return a 64-bit value.
 */
    /* return-wide vAA */
    .extern MterpThreadFenceForConstructor
    call    MterpThreadFenceForConstructor
    GET_VREG %eax rINST                     # eax <- v[AA+0]
    GET_VREG_HIGH %ecx rINST                # ecx <- v[AA+1]
    jmp     MterpReturn
