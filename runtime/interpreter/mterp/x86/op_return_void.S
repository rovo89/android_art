    .extern MterpThreadFenceForConstructor
    call    MterpThreadFenceForConstructor
    xorl    %eax, %eax
    xorl    %ecx, %ecx
    jmp     MterpReturn
