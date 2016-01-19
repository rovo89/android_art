    EXPORT_PC
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG0(%esp)
    movl    rPC, OUT_ARG1(%esp)
    REFRESH_INST ${opnum}
    movl    rINST, OUT_ARG2(%esp)
    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG3(%esp)
    call    MterpSputObject
    testl   %eax, %eax
    jz      MterpException
    REFRESH_IBASE
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
