    /* move-exception vAA */
    movl    THREAD_EXCEPTION_OFFSET(rSELF), %eax
    SET_VREG_OBJECT %eax, rINSTq            # fp[AA] <- exception object
    movl    $$0, THREAD_EXCEPTION_OFFSET(rSELF)
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
