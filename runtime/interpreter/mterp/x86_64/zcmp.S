/*
 * Generic one-operand compare-and-branch operation.  Provide a "revcmp"
 * fragment that specifies the *reverse* comparison to perform, e.g.
 * for "if-le" you would use "gt".
 *
 * for: if-eqz, if-nez, if-ltz, if-gez, if-gtz, if-lez
 */
    /* if-cmp vAA, +BBBB */
    cmpl    $$0, VREG_ADDRESS(rINSTq)       # compare (vA, 0)
    movl    $$2, rINST                      # assume branch not taken
    j${revcmp}   1f
    movswq  2(rPC), rINSTq                  # fetch signed displacement
1:
    MTERP_PROFILE_BRANCH
    addq    rINSTq, rINSTq                  # rINSTq <- AA * 2
    leaq    (rPC, rINSTq), rPC
    FETCH_INST
    jle     MterpCheckSuspendAndContinue    # AA * 2 <= 0 => suspend check
    GOTO_NEXT
