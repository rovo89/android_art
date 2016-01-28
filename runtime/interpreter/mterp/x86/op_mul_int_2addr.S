    /* mul vA, vB */
    movzx   rINSTbl, %ecx                   # ecx <- A+
    sarl    $$4, rINST                      # rINST <- B
    GET_VREG %eax, rINST                    # eax <- vB
    andb    $$0xf, %cl                      # ecx <- A
    mov     rIBASE, LOCAL0(%esp)
    imull   (rFP,%ecx,4), %eax              # trashes rIBASE/edx
    mov     LOCAL0(%esp), rIBASE
    SET_VREG %eax, %ecx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
