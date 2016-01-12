    .extern MterpThreadFenceForConstructor
    bl      MterpThreadFenceForConstructor
    mov    r0, #0
    mov    r1, #0
    b      MterpReturn
