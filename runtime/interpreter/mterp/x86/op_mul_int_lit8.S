    /* mul/lit8 vAA, vBB, #+CC */
    movzbl  2(rPC), %eax                    # eax <- BB
    movsbl  3(rPC), %ecx                    # ecx <- ssssssCC
    GET_VREG  %eax, %eax                    # eax <- rBB
    mov     rIBASE, LOCAL0(%esp)
    imull   %ecx, %eax                      # trashes rIBASE/edx
    mov     LOCAL0(%esp), rIBASE
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
