    /* For: iget-object-quick */
    /* op vA, vB, offset@CCCC */
    mov     r2, rINST, lsr #12          @ r2<- B
    FETCH r1, 1                         @ r1<- field byte offset
    GET_VREG r0, r2                     @ r0<- object we're operating on
    cmp     r0, #0                      @ check object for null
    beq     common_errNullObject        @ object was null
    bl      artIGetObjectFromMterp      @ (obj, offset)
    ldr     r3, [rSELF, #THREAD_EXCEPTION_OFFSET]
    ubfx    r2, rINST, #8, #4           @ r2<- A
    PREFETCH_INST 2
    cmp     r3, #0
    bne     MterpPossibleException      @ bail out
    SET_VREG_OBJECT r0, r2              @ fp[A]<- r0
    ADVANCE 2                           @ advance rPC
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction
