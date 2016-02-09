    /* const/class vAA, Class@BBBB */
    EXPORT_PC
    movzwl  2(rPC), %eax                    # eax<- BBBB
    movl    %eax, OUT_ARG0(%esp)
    movl    rINST, OUT_ARG1(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG2(%esp)
    movl    rSELF, %eax
    movl    %eax, OUT_ARG3(%esp)
    call    SYMBOL(MterpConstClass)         # (index, tgt_reg, shadow_frame, self)
    REFRESH_IBASE
    testl   %eax, %eax
    jnz     MterpPossibleException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
