    /*
     * Return a 64-bit value.
     */
    /* return-wide vAA */
    .extern MterpThreadFenceForConstructor
    bl      MterpThreadFenceForConstructor
    mov     r2, rINST, lsr #8           @ r2<- AA
    add     r2, rFP, r2, lsl #2         @ r2<- &fp[AA]
    ldmia   r2, {r0-r1}                 @ r0/r1 <- vAA/vAA+1
    b       MterpReturn
