    /*
     * Return a 32-bit value.
     *
     * for: return, return-object
     */
    /* op vAA */
    .extern MterpThreadFenceForConstructor
    bl      MterpThreadFenceForConstructor
    mov     r2, rINST, lsr #8           @ r2<- AA
    GET_VREG r0, r2                     @ r0<- vAA
    mov     r1, #0
    b       MterpReturn
