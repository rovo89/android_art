/*
 * Generic one-operand compare-and-branch operation.  Provide a "revcmp"
 * fragment that specifies the *reverse* comparison to perform, e.g.
 * for "if-le" you would use "gt".
 *
 * for: if-eqz, if-nez, if-ltz, if-gez, if-gtz, if-lez
 */
    /* if-cmp vAA, +BBBB */
    cmpl    $$0, VREG_ADDRESS(rINST)        # compare (vA, 0)
    movl    $$2, %eax                       # assume branch not taken
    j${revcmp}   1f
    movswl  2(rPC),%eax                     # fetch signed displacement
1:
    addl    %eax, %eax                      # eax <- AA * 2
    leal    (rPC, %eax), rPC
    FETCH_INST
    jg      2f                              # AA * 2 > 0 => no suspend check
#if MTERP_SUSPEND
    REFRESH_IBASE
#else
    jmp     MterpCheckSuspendAndContinue
#endif
2:
    GOTO_NEXT
