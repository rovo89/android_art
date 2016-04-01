/*
 * Generic one-operand compare-and-branch operation.  Provide a "revcmp"
 * fragment that specifies the *reverse* comparison to perform, e.g.
 * for "if-le" you would use "gt".
 *
 * for: if-eqz, if-nez, if-ltz, if-gez, if-gtz, if-lez
 */
    /* if-cmp vAA, +BBBB */
    cmpl    $$0, VREG_ADDRESS(rINST)        # compare (vA, 0)
    movl    $$2, rINST
    j${revcmp}   1f
    movswl  2(rPC), rINST                   # fetch signed displacement
1:
    MTERP_PROFILE_BRANCH
    addl    rINST, rINST                    # eax <- AA * 2
    leal    (rPC, rINST), rPC
    FETCH_INST
    jle     MterpCheckSuspendAndContinue    # AA * 2 <= 0 => suspend check
    GOTO_NEXT
