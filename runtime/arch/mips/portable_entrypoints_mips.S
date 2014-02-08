/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "asm_support_mips.S"

    .set noreorder
    .balign 4

    .extern artPortableProxyInvokeHandler
ENTRY art_portable_proxy_invoke_handler
    GENERATE_GLOBAL_POINTER
    # Fake callee save ref and args frame set up, note portable doesn't use callee save frames.
    # TODO: just save the registers that are needed in artPortableProxyInvokeHandler.
    addiu  $sp, $sp, -64
    .cfi_adjust_cfa_offset 64
    sw     $ra, 60($sp)
    .cfi_rel_offset 31, 60
    sw     $s8, 56($sp)
    .cfi_rel_offset 30, 56
    sw     $gp, 52($sp)
    .cfi_rel_offset 28, 52
    sw     $s7, 48($sp)
    .cfi_rel_offset 23, 48
    sw     $s6, 44($sp)
    .cfi_rel_offset 22, 44
    sw     $s5, 40($sp)
    .cfi_rel_offset 21, 40
    sw     $s4, 36($sp)
    .cfi_rel_offset 20, 36
    sw     $s3, 32($sp)
    .cfi_rel_offset 19, 32
    sw     $s2, 28($sp)
    .cfi_rel_offset 18, 28
    sw     $a3, 12($sp)
    .cfi_rel_offset 7, 12
    sw     $a2, 8($sp)
    .cfi_rel_offset 6, 8
    sw     $a1, 4($sp)
    .cfi_rel_offset 5, 4
    # Begin argument set up.
    sw      $a0, 0($sp)            # place proxy method at bottom of frame
    move    $a2, rSELF             # pass Thread::Current
    jal     artPortableProxyInvokeHandler  # (Method* proxy method, receiver, Thread*, SP)
    move    $a3, $sp               # pass $sp
    lw      $ra, 60($sp)           # restore $ra
    jr      $ra
    addiu   $sp, $sp, 64           # pop frame
    .cfi_adjust_cfa_offset -64
END art_portable_proxy_invoke_handler

    /*
     * Invocation stub for portable code.
     * On entry:
     *   a0 = method pointer
     *   a1 = argument array or NULL for no argument methods
     *   a2 = size of argument array in bytes
     *   a3 = (managed) thread pointer
     *   [sp + 16] = JValue* result
     *   [sp + 20] = result type char
     */
ENTRY art_portable_invoke_stub
    GENERATE_GLOBAL_POINTER
    sw    $a0, 0($sp)           # save out a0
    addiu $sp, $sp, -16         # spill s0, s1, fp, ra
    .cfi_adjust_cfa_offset 16
    sw    $ra, 12($sp)
    .cfi_rel_offset 31, 12
    sw    $fp, 8($sp)
    .cfi_rel_offset 30, 8
    sw    $s1, 4($sp)
    .cfi_rel_offset 17, 4
    sw    $s0, 0($sp)
    .cfi_rel_offset 16, 0
    move  $fp, $sp              # save sp in fp
    .cfi_def_cfa_register 30
    move  $s1, $a3              # move managed thread pointer into s1
    addiu $s0, $zero, SUSPEND_CHECK_INTERVAL  # reset s0 to suspend check interval
    addiu $t0, $a2, 16          # create space for method pointer in frame
    srl   $t0, $t0, 3           # shift the frame size right 3
    sll   $t0, $t0, 3           # shift the frame size left 3 to align to 16 bytes
    subu  $sp, $sp, $t0         # reserve stack space for argument array
    addiu $a0, $sp, 4           # pass stack pointer + method ptr as dest for memcpy
    jal   memcpy                # (dest, src, bytes)
    addiu $sp, $sp, -16         # make space for argument slots for memcpy
    addiu $sp, $sp, 16          # restore stack after memcpy
    lw    $a0, 16($fp)          # restore method*
    lw    $a1, 4($sp)           # copy arg value for a1
    lw    $a2, 8($sp)           # copy arg value for a2
    lw    $a3, 12($sp)          # copy arg value for a3
    lw    $t9, METHOD_PORTABLE_CODE_OFFSET($a0)  # get pointer to the code
    jalr  $t9                   # call the method
    sw    $zero, 0($sp)         # store NULL for method* at bottom of frame
    move  $sp, $fp              # restore the stack
    lw    $s0, 0($sp)
    .cfi_restore 16
    lw    $s1, 4($sp)
    .cfi_restore 17
    lw    $fp, 8($sp)
    .cfi_restore 30
    lw    $ra, 12($sp)
    .cfi_restore 31
    addiu $sp, $sp, 16
    .cfi_adjust_cfa_offset -16
    lw    $t0, 16($sp)          # get result pointer
    lw    $t1, 20($sp)          # get result type char
    li    $t2, 68               # put char 'D' into t2
    beq   $t1, $t2, 1f          # branch if result type char == 'D'
    li    $t3, 70               # put char 'F' into t3
    beq   $t1, $t3, 1f          # branch if result type char == 'F'
    sw    $v0, 0($t0)           # store the result
    jr    $ra
    sw    $v1, 4($t0)           # store the other half of the result
1:
    s.s   $f0, 0($t0)           # store floating point result
    jr    $ra
    s.s   $f1, 4($t0)           # store other half of floating point result
END art_portable_invoke_stub

UNIMPLEMENTED art_portable_resolution_trampoline
UNIMPLEMENTED art_portable_to_interpreter_bridge
