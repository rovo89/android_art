/*
 * Generic two-operand compare-and-branch operation.  Provide a "revcmp"
 * fragment that specifies the *reverse* comparison to perform, e.g.
 * for "if-le" you would use "gt".
 *
 * For: if-eq, if-ne, if-lt, if-ge, if-gt, if-le
 */
    /* if-cmp vA, vB, +CCCC */
    movzx   rINSTbl, %ecx                   # ecx <- A+
    andb    $$0xf, %cl                      # ecx <- A
    GET_VREG %eax, %ecx                     # eax <- vA
    sarl    $$4, rINST                      # rINST <- B
    cmpl    VREG_ADDRESS(rINST), %eax       # compare (vA, vB)
    movl    $$2, %eax                       # assume not taken
    j${revcmp}   1f
    movswl  2(rPC),%eax                     # Get signed branch offset
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
