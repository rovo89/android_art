    /* mul/lit16 vA, vB, #+CCCC */
    /* Need A in rINST, ssssCCCC in ecx, vB in eax */
    movzbl  rINSTbl, %eax                   # eax <- 000000BA
    sarl    $$4, %eax                       # eax <- B
    GET_VREG %eax, %eax                     # eax <- vB
    movswl  2(rPC), %ecx                    # ecx <- ssssCCCC
    andb    $$0xf, rINSTbl                  # rINST <- A
    mov     rIBASE, LOCAL0(%esp)
    imull   %ecx, %eax                      # trashes rIBASE/edx
    mov     LOCAL0(%esp), rIBASE
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
