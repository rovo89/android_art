/*
 * Copyright (C) 2014 The Android Open Source Project
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

#include "asm_support_mips64.S"

#include "arch/quick_alloc_entrypoints.S"

    .set noreorder
    .balign 16

    /* Deliver the given exception */
    .extern artDeliverExceptionFromCode
    /* Deliver an exception pending on a thread */
    .extern artDeliverPendingExceptionFromCode

    /*
     * Macro that sets up the callee save frame to conform with
     * Runtime::CreateCalleeSaveMethod(kSaveAll)
     * callee-save: padding + $f24-$f31 + $s0-$s7 + $gp + $ra + $s8 = 19 total + 1x8 bytes padding
     */
.macro SETUP_SAVE_ALL_CALLEE_SAVE_FRAME
    daddiu $sp, $sp, -160
    .cfi_adjust_cfa_offset 160

     // Ugly compile-time check, but we only have the preprocessor.
#if (FRAME_SIZE_SAVE_ALL_CALLEE_SAVE != 160)
#error "SAVE_ALL_CALLEE_SAVE_FRAME(MIPS64) size not as expected."
#endif

    sd     $ra, 152($sp)
    .cfi_rel_offset 31, 152
    sd     $s8, 144($sp)
    .cfi_rel_offset 30, 144
    sd     $gp, 136($sp)
    .cfi_rel_offset 28, 136
    sd     $s7, 128($sp)
    .cfi_rel_offset 23, 128
    sd     $s6, 120($sp)
    .cfi_rel_offset 22, 120
    sd     $s5, 112($sp)
    .cfi_rel_offset 21, 112
    sd     $s4, 104($sp)
    .cfi_rel_offset 20, 104
    sd     $s3,  96($sp)
    .cfi_rel_offset 19, 96
    sd     $s2,  88($sp)
    .cfi_rel_offset 18, 88
    sd     $s1,  80($sp)
    .cfi_rel_offset 17, 80
    sd     $s0,  72($sp)
    .cfi_rel_offset 16, 72

    // FP callee-saves
    s.d    $f31, 64($sp)
    s.d    $f30, 56($sp)
    s.d    $f29, 48($sp)
    s.d    $f28, 40($sp)
    s.d    $f27, 32($sp)
    s.d    $f26, 24($sp)
    s.d    $f25, 16($sp)
    s.d    $f24,  8($sp)

    # load appropriate callee-save-method
    ld      $v0, %got(_ZN3art7Runtime9instance_E)($gp)
    ld      $v0, 0($v0)
    THIS_LOAD_REQUIRES_READ_BARRIER
    ld      $v0, RUNTIME_SAVE_ALL_CALLEE_SAVE_FRAME_OFFSET($v0)
    sw      $v0, 0($sp)                                # Place Method* at bottom of stack.
    sd      $sp, THREAD_TOP_QUICK_FRAME_OFFSET(rSELF)  # Place sp in Thread::Current()->top_quick_frame.
.endm

    /*
     * Macro that sets up the callee save frame to conform with
     * Runtime::CreateCalleeSaveMethod(kRefsOnly). Restoration assumes
     * non-moving GC.
     * Does not include rSUSPEND or rSELF
     * callee-save: padding + $s2-$s7 + $gp + $ra + $s8 = 9 total + 1x8 bytes padding
     */
.macro SETUP_REFS_ONLY_CALLEE_SAVE_FRAME
    daddiu $sp, $sp, -80
    .cfi_adjust_cfa_offset 80

    // Ugly compile-time check, but we only have the preprocessor.
#if (FRAME_SIZE_REFS_ONLY_CALLEE_SAVE != 80)
#error "REFS_ONLY_CALLEE_SAVE_FRAME(MIPS64) size not as expected."
#endif

    sd     $ra, 72($sp)
    .cfi_rel_offset 31, 72
    sd     $s8, 64($sp)
    .cfi_rel_offset 30, 64
    sd     $gp, 56($sp)
    .cfi_rel_offset 28, 56
    sd     $s7, 48($sp)
    .cfi_rel_offset 23, 48
    sd     $s6, 40($sp)
    .cfi_rel_offset 22, 40
    sd     $s5, 32($sp)
    .cfi_rel_offset 21, 32
    sd     $s4, 24($sp)
    .cfi_rel_offset 20, 24
    sd     $s3, 16($sp)
    .cfi_rel_offset 19, 16
    sd     $s2, 8($sp)
    .cfi_rel_offset 18, 8
    # load appropriate callee-save-method
    ld      $v0, %got(_ZN3art7Runtime9instance_E)($gp)
    ld      $v0, 0($v0)
    THIS_LOAD_REQUIRES_READ_BARRIER
    ld      $v0, RUNTIME_REFS_ONLY_CALLEE_SAVE_FRAME_OFFSET($v0)
    sw      $v0, 0($sp)                                # Place Method* at bottom of stack.
    sd      $sp, THREAD_TOP_QUICK_FRAME_OFFSET(rSELF)  # Place sp in Thread::Current()->top_quick_frame.
.endm

.macro RESTORE_REFS_ONLY_CALLEE_SAVE_FRAME
    ld     $ra, 72($sp)
    .cfi_restore 31
    ld     $s8, 64($sp)
    .cfi_restore 30
    ld     $gp, 56($sp)
    .cfi_restore 28
    ld     $s7, 48($sp)
    .cfi_restore 23
    ld     $s6, 40($sp)
    .cfi_restore 22
    ld     $s5, 32($sp)
    .cfi_restore 21
    ld     $s4, 24($sp)
    .cfi_restore 20
    ld     $s3, 16($sp)
    .cfi_restore 19
    ld     $s2, 8($sp)
    .cfi_restore 18
    daddiu $sp, $sp, 80
    .cfi_adjust_cfa_offset -80
.endm

.macro RESTORE_REFS_ONLY_CALLEE_SAVE_FRAME_AND_RETURN
    ld     $ra, 72($sp)
    .cfi_restore 31
    ld     $s8, 64($sp)
    .cfi_restore 30
    ld     $gp, 56($sp)
    .cfi_restore 28
    ld     $s7, 48($sp)
    .cfi_restore 23
    ld     $s6, 40($sp)
    .cfi_restore 22
    ld     $s5, 32($sp)
    .cfi_restore 21
    ld     $s4, 24($sp)
    .cfi_restore 20
    ld     $s3, 16($sp)
    .cfi_restore 19
    ld     $s2, 8($sp)
    .cfi_restore 18
    jalr   $zero, $ra
    daddiu $sp, $sp, 80
    .cfi_adjust_cfa_offset -80
.endm

// This assumes the top part of these stack frame types are identical.
#define REFS_AND_ARGS_MINUS_REFS_SIZE (FRAME_SIZE_REFS_AND_ARGS_CALLEE_SAVE - FRAME_SIZE_REFS_ONLY_CALLEE_SAVE)

    /*
     * Macro that sets up the callee save frame to conform with
     * Runtime::CreateCalleeSaveMethod(kRefsAndArgs). Restoration assumes
     * non-moving GC.
     * callee-save: padding + $f12-$f19 + $a1-$a7 + $s2-$s7 + $gp + $ra + $s8 = 24 total + 1 words padding + Method*
     */
.macro SETUP_REFS_AND_ARGS_CALLEE_SAVE_FRAME_INTERNAL
    daddiu  $sp, $sp, -208
    .cfi_adjust_cfa_offset 208

    // Ugly compile-time check, but we only have the preprocessor.
#if (FRAME_SIZE_REFS_AND_ARGS_CALLEE_SAVE != 208)
#error "REFS_AND_ARGS_CALLEE_SAVE_FRAME(MIPS64) size not as expected."
#endif

    sd     $ra, 200($sp)           # = kQuickCalleeSaveFrame_RefAndArgs_LrOffset
    .cfi_rel_offset 31, 200
    sd     $s8, 192($sp)
    .cfi_rel_offset 30, 192
    sd     $gp, 184($sp)
    .cfi_rel_offset 28, 184
    sd     $s7, 176($sp)
    .cfi_rel_offset 23, 176
    sd     $s6, 168($sp)
    .cfi_rel_offset 22, 168
    sd     $s5, 160($sp)
    .cfi_rel_offset 21, 160
    sd     $s4, 152($sp)
    .cfi_rel_offset 20, 152
    sd     $s3, 144($sp)
    .cfi_rel_offset 19, 144
    sd     $s2, 136($sp)
    .cfi_rel_offset 18, 136

    sd     $a7, 128($sp)
    .cfi_rel_offset 11, 128
    sd     $a6, 120($sp)
    .cfi_rel_offset 10, 120
    sd     $a5, 112($sp)
    .cfi_rel_offset 9, 112
    sd     $a4, 104($sp)
    .cfi_rel_offset 8, 104
    sd     $a3,  96($sp)
    .cfi_rel_offset 7, 96
    sd     $a2,  88($sp)
    .cfi_rel_offset 6, 88
    sd     $a1,  80($sp)           # = kQuickCalleeSaveFrame_RefAndArgs_Gpr1Offset
    .cfi_rel_offset 5, 80

    s.d    $f19, 72($sp)
    s.d    $f18, 64($sp)
    s.d    $f17, 56($sp)
    s.d    $f16, 48($sp)
    s.d    $f15, 40($sp)
    s.d    $f14, 32($sp)
    s.d    $f13, 24($sp)           # = kQuickCalleeSaveFrame_RefAndArgs_Fpr1Offset
    s.d    $f12, 16($sp)           # This isn't necessary to store.

    # 1x8 bytes paddig + Method*
    ld      $v0, %got(_ZN3art7Runtime9instance_E)($gp)
    ld      $v0, 0($v0)
    THIS_LOAD_REQUIRES_READ_BARRIER
    ld      $v0, RUNTIME_REFS_ONLY_CALLEE_SAVE_FRAME_OFFSET($v0)
    sw      $v0, 0($sp)                                # Place Method* at bottom of stack.
    sd      $sp, THREAD_TOP_QUICK_FRAME_OFFSET(rSELF)  # Place sp in Thread::Current()->top_quick_frame.
.endm

.macro SETUP_REFS_AND_ARGS_CALLEE_SAVE_FRAME
    SETUP_REFS_AND_ARGS_CALLEE_SAVE_FRAME_INTERNAL
    # load appropriate callee-save-method
    ld      $v0, %got(_ZN3art7Runtime9instance_E)($gp)
    ld      $v0, 0($v0)
    THIS_LOAD_REQUIRES_READ_BARRIER
    ld      $v0, RUNTIME_REFS_AND_ARGS_CALLEE_SAVE_FRAME_OFFSET($v0)
    sw      $v0, 0($sp)                                # Place Method* at bottom of stack.
    sd      $sp, THREAD_TOP_QUICK_FRAME_OFFSET(rSELF)  # Place sp in Thread::Current()->top_quick_frame.
.endm

.macro RESTORE_REFS_AND_ARGS_CALLEE_SAVE_FRAME
    ld     $ra, 200($sp)
    .cfi_restore 31
    ld     $s8, 192($sp)
    .cfi_restore 30
    ld     $gp, 184($sp)
    .cfi_restore 28
    ld     $s7, 176($sp)
    .cfi_restore 23
    ld     $s6, 168($sp)
    .cfi_restore 22
    ld     $s5, 160($sp)
    .cfi_restore 21
    ld     $s4, 152($sp)
    .cfi_restore 20
    ld     $s3, 144($sp)
    .cfi_restore 19
    ld     $s2, 136($sp)
    .cfi_restore 18

    ld     $a7, 128($sp)
    .cfi_restore 11
    ld     $a6, 120($sp)
    .cfi_restore 10
    ld     $a5, 112($sp)
    .cfi_restore 9
    ld     $a4, 104($sp)
    .cfi_restore 8
    ld     $a3,  96($sp)
    .cfi_restore 7
    ld     $a2,  88($sp)
    .cfi_restore 6
    ld     $a1,  80($sp)
    .cfi_restore 5

    l.d    $f19, 72($sp)
    l.d    $f18, 64($sp)
    l.d    $f17, 56($sp)
    l.d    $f16, 48($sp)
    l.d    $f15, 40($sp)
    l.d    $f14, 32($sp)
    l.d    $f13, 24($sp)
    l.d    $f12, 16($sp)

    daddiu $sp, $sp, 208
    .cfi_adjust_cfa_offset -208
.endm

    /*
     * Macro that set calls through to artDeliverPendingExceptionFromCode,
     * where the pending
     * exception is Thread::Current()->exception_
     */
.macro DELIVER_PENDING_EXCEPTION
    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME     # save callee saves for throw
    dla     $t9, artDeliverPendingExceptionFromCode
    jalr    $zero, $t9                   # artDeliverPendingExceptionFromCode(Thread*)
    move    $a0, rSELF                   # pass Thread::Current
.endm

.macro RETURN_IF_NO_EXCEPTION
    ld     $t0, THREAD_EXCEPTION_OFFSET(rSELF) # load Thread::Current()->exception_
    RESTORE_REFS_ONLY_CALLEE_SAVE_FRAME
    bne    $t0, $zero, 1f                      # success if no exception is pending
    nop
    jalr   $zero, $ra
    nop
1:
    DELIVER_PENDING_EXCEPTION
.endm

.macro RETURN_IF_ZERO
    RESTORE_REFS_ONLY_CALLEE_SAVE_FRAME
    bne    $v0, $zero, 1f                # success?
    nop
    jalr   $zero, $ra                    # return on success
    nop
1:
    DELIVER_PENDING_EXCEPTION
.endm

.macro RETURN_IF_RESULT_IS_NON_ZERO_OR_DELIVER
    RESTORE_REFS_ONLY_CALLEE_SAVE_FRAME
    beq    $v0, $zero, 1f                # success?
    nop
    jalr   $zero, $ra                    # return on success
    nop
1:
    DELIVER_PENDING_EXCEPTION
.endm

    /*
     * On entry $a0 is uint32_t* gprs_ and $a1 is uint32_t* fprs_
     * FIXME: just guessing about the shape of the jmpbuf.  Where will pc be?
     */
ENTRY art_quick_do_long_jump
    l.d     $f0, 0($a1)
    l.d     $f1, 8($a1)
    l.d     $f2, 16($a1)
    l.d     $f3, 24($a1)
    l.d     $f4, 32($a1)
    l.d     $f5, 40($a1)
    l.d     $f6, 48($a1)
    l.d     $f7, 56($a1)
    l.d     $f8, 64($a1)
    l.d     $f9, 72($a1)
    l.d     $f10, 80($a1)
    l.d     $f11, 88($a1)
    l.d     $f12, 96($a1)
    l.d     $f13, 104($a1)
    l.d     $f14, 112($a1)
    l.d     $f15, 120($a1)
    l.d     $f16, 128($a1)
    l.d     $f17, 136($a1)
    l.d     $f18, 144($a1)
    l.d     $f19, 152($a1)
    l.d     $f20, 160($a1)
    l.d     $f21, 168($a1)
    l.d     $f22, 176($a1)
    l.d     $f23, 184($a1)
    l.d     $f24, 192($a1)
    l.d     $f25, 200($a1)
    l.d     $f26, 208($a1)
    l.d     $f27, 216($a1)
    l.d     $f28, 224($a1)
    l.d     $f29, 232($a1)
    l.d     $f30, 240($a1)
    l.d     $f31, 248($a1)
    .set push
    .set nomacro
    .set noat
# no need to load zero
    ld      $at, 8($a0)
    .set pop
    ld      $v0, 16($a0)
    ld      $v1, 24($a0)
# a0 has to be loaded last
    ld      $a1, 40($a0)
    ld      $a2, 48($a0)
    ld      $a3, 56($a0)
    ld      $a4, 64($a0)
    ld      $a5, 72($a0)
    ld      $a6, 80($a0)
    ld      $a7, 88($a0)
    ld      $t0, 96($a0)
    ld      $t1, 104($a0)
    ld      $t2, 112($a0)
    ld      $t3, 120($a0)
    ld      $s0, 128($a0)
    ld      $s1, 136($a0)
    ld      $s2, 144($a0)
    ld      $s3, 152($a0)
    ld      $s4, 160($a0)
    ld      $s5, 168($a0)
    ld      $s6, 176($a0)
    ld      $s7, 184($a0)
    ld      $t8, 192($a0)
    ld      $t9, 200($a0)
# no need to load k0, k1
    ld      $gp, 224($a0)
    ld      $sp, 232($a0)
    ld      $s8, 240($a0)
    ld      $ra, 248($a0)
    ld      $a0, 32($a0)
    move    $v0, $zero          # clear result registers v0 and v1
    jalr    $zero, $ra          # do long jump
    move    $v1, $zero
END art_quick_do_long_jump

UNIMPLEMENTED art_quick_deliver_exception
UNIMPLEMENTED art_quick_throw_null_pointer_exception
UNIMPLEMENTED art_quick_throw_div_zero
UNIMPLEMENTED art_quick_throw_array_bounds
UNIMPLEMENTED art_quick_throw_stack_overflow
UNIMPLEMENTED art_quick_throw_no_such_method

UNIMPLEMENTED art_quick_invoke_interface_trampoline
UNIMPLEMENTED art_quick_invoke_interface_trampoline_with_access_check

UNIMPLEMENTED art_quick_invoke_static_trampoline_with_access_check
UNIMPLEMENTED art_quick_invoke_direct_trampoline_with_access_check
UNIMPLEMENTED art_quick_invoke_super_trampoline_with_access_check
UNIMPLEMENTED art_quick_invoke_virtual_trampoline_with_access_check

    # On entry:
    #   t0 = shorty
    #   t1 = ptr to arg_array
    #   t2 = number of argument bytes remain
    #   v0 = ptr to stack frame where to copy arg_array
    # This macro modifies t3, t9 and v0
.macro LOOP_OVER_SHORTY_LOADING_REG gpu, fpu, label
    lbu    $t3, 0($t0)           # get argument type from shorty
    beqz   $t3, \label
    daddiu $t0, 1
    li     $t9, 68               # put char 'D' into t9
    beq    $t9, $t3, 1f          # branch if result type char == 'D'
    li     $t9, 70               # put char 'F' into t9
    beq    $t9, $t3, 2f          # branch if result type char == 'F'
    li     $t9, 74               # put char 'J' into t9
    beq    $t9, $t3, 3f          # branch if result type char == 'J'
    nop
    lwu    $\gpu, 0($t1)
    sw     $\gpu, 0($v0)
    daddiu $v0, 4
    daddiu $t1, 4
    b      4f
    daddiu $t2, -4               # delay slot

1:  # found double
    lwu    $t3, 0($t1)
    mtc1   $t3, $\fpu
    sw     $t3, 0($v0)
    lwu    $t3, 4($t1)
    mthc1  $t3, $\fpu
    sw     $t3, 4($v0)
    daddiu $v0, 8
    daddiu $t1, 8
    b      4f
    daddiu $t2, -8               # delay slot

2:  # found float
    lwu    $t3, 0($t1)
    mtc1   $t3, $\fpu
    sw     $t3, 0($v0)
    daddiu $v0, 4
    daddiu $t1, 4
    b      4f
    daddiu $t2, -4               # delay slot

3:  # found long (8 bytes)
    lwu    $t3, 0($t1)
    sw     $t3, 0($v0)
    lwu    $t9, 4($t1)
    sw     $t9, 4($v0)
    dsll   $t9, $t9, 32
    or     $\gpu, $t9, $t3
    daddiu $v0, 8
    daddiu $t1, 8
    daddiu $t2, -8
4:
.endm

    /*
     * Invocation stub for quick code.
     * On entry:
     *   a0 = method pointer
     *   a1 = argument array that must at least contain the this ptr.
     *   a2 = size of argument array in bytes
     *   a3 = (managed) thread pointer
     *   a4 = JValue* result
     *   a5 = shorty
     */
ENTRY art_quick_invoke_stub
    # push a4, a5, s0(rSUSPEND), s1(rSELF), s8, ra onto the stack
    daddiu $sp, $sp, -48
    .cfi_adjust_cfa_offset 48
    sd     $ra, 40($sp)
    .cfi_rel_offset 31, 40
    sd     $s8, 32($sp)
    .cfi_rel_offset 30, 32
    sd     $s1, 24($sp)
    .cfi_rel_offset 17, 24
    sd     $s0, 16($sp)
    .cfi_rel_offset 16, 16
    sd     $a5, 8($sp)
    .cfi_rel_offset 9, 8
    sd     $a4, 0($sp)
    .cfi_rel_offset 8, 0

    daddiu $s0, $zero, SUSPEND_CHECK_INTERVAL   # reset rSUSPEND to SUSPEND_CHECK_INTERVAL
    move   $s1, $a3              # move managed thread pointer into s1 (rSELF)
    move   $s8, $sp              # save sp in s8 (fp)

    daddiu $t3, $a2, 20          # add 4 for method* and 16 for stack alignment
    dsrl   $t3, $t3, 4           # shift the frame size right 4
    dsll   $t3, $t3, 4           # shift the frame size left 4 to align to 16 bytes
    dsubu  $sp, $sp, $t3         # reserve stack space for argument array

    daddiu $t0, $a5, 1           # t0 = shorty[1] (skip 1 for return type)
    daddiu $t1, $a1, 4           # t1 = ptr to arg_array[4] (skip this ptr)
    daddiu $t2, $a2, -4          # t2 = number of argument bytes remain (skip this ptr)
    daddiu $v0, $sp, 8           # v0 points to where to copy arg_array
    LOOP_OVER_SHORTY_LOADING_REG a2, f14, call_fn
    LOOP_OVER_SHORTY_LOADING_REG a3, f15, call_fn
    LOOP_OVER_SHORTY_LOADING_REG a4, f16, call_fn
    LOOP_OVER_SHORTY_LOADING_REG a5, f17, call_fn
    LOOP_OVER_SHORTY_LOADING_REG a6, f18, call_fn
    LOOP_OVER_SHORTY_LOADING_REG a7, f19, call_fn

    # copy arguments onto stack (t2 should be multiples of 4)
    ble    $t2, $zero, call_fn   # t2 = number of argument bytes remain
1:
    lw     $t3, 0($t1)           # load from argument array
    daddiu $t1, $t1, 4
    sw     $t3, 0($v0)           # save to stack
    daddiu $t2, -4
    bgt    $t2, $zero, 1b        # t2 = number of argument bytes remain
    daddiu $v0, $v0, 4

call_fn:
    # call method (a0 and a1 have been untouched)
    lwu    $a1, 0($a1)           # make a1 = this ptr
    sw     $a1, 4($sp)           # copy this ptr (skip 4 bytes for method*)
    sw     $zero, 0($sp)         # store NULL for method* at bottom of frame
    ld     $t9, MIRROR_ART_METHOD_QUICK_CODE_OFFSET_64($a0)  # get pointer to the code
    jalr   $t9                   # call the method
    nop
    move   $sp, $s8              # restore sp

    # pop a4, a5, s1(rSELF), s8, ra off of the stack
    ld     $a4, 0($sp)
    .cfi_restore 8
    ld     $a5, 8($sp)
    .cfi_restore 9
    ld     $s0, 16($sp)
    .cfi_restore 16
    ld     $s1, 24($sp)
    .cfi_restore 17
    ld     $s8, 32($sp)
    .cfi_restore 30
    ld     $ra, 40($sp)
    .cfi_restore 31
    daddiu $sp, $sp, 48
    .cfi_adjust_cfa_offset -48

    # a4 = JValue* result
    # a5 = shorty string
    lbu   $t1, 0($a5)           # get result type from shorty
    li    $t2, 68               # put char 'D' into t2
    beq   $t1, $t2, 1f          # branch if result type char == 'D'
    li    $t3, 70               # put char 'F' into t3
    beq   $t1, $t3, 1f          # branch if result type char == 'F'
    sw    $v0, 0($a4)           # store the result
    dsrl  $v1, $v0, 32
    jalr  $zero, $ra
    sw    $v1, 4($a4)           # store the other half of the result
1:
    mfc1  $v0, $f0
    mfhc1 $v1, $f0
    sw    $v0, 0($a4)           # store the result
    jalr  $zero, $ra
    sw    $v1, 4($a4)           # store the other half of the result
END art_quick_invoke_stub

    /*
     * Invocation static stub for quick code.
     * On entry:
     *   a0 = method pointer
     *   a1 = argument array that must at least contain the this ptr.
     *   a2 = size of argument array in bytes
     *   a3 = (managed) thread pointer
     *   a4 = JValue* result
     *   a5 = shorty
     */
ENTRY art_quick_invoke_static_stub

    # push a4, a5, s0(rSUSPEND), s1(rSELF), s8, ra, onto the stack
    daddiu $sp, $sp, -48
    .cfi_adjust_cfa_offset 48
    sd     $ra, 40($sp)
    .cfi_rel_offset 31, 40
    sd     $s8, 32($sp)
    .cfi_rel_offset 30, 32
    sd     $s1, 24($sp)
    .cfi_rel_offset 17, 24
    sd     $s0, 16($sp)
    .cfi_rel_offset 16, 16
    sd     $a5, 8($sp)
    .cfi_rel_offset 9, 8
    sd     $a4, 0($sp)
    .cfi_rel_offset 8, 0

    daddiu $s0, $zero, SUSPEND_CHECK_INTERVAL   # reset rSUSPEND to SUSPEND_CHECK_INTERVAL
    move   $s1, $a3              # move managed thread pointer into s1 (rSELF)
    move   $s8, $sp              # save sp in s8 (fp)

    daddiu $t3, $a2, 20          # add 4 for method* and 16 for stack alignment
    dsrl   $t3, $t3, 4           # shift the frame size right 4
    dsll   $t3, $t3, 4           # shift the frame size left 4 to align to 16 bytes
    dsubu  $sp, $sp, $t3         # reserve stack space for argument array

    daddiu $t0, $a5, 1           # t0 = shorty[1] (skip 1 for return type)
    move   $t1, $a1              # t1 = arg_array
    move   $t2, $a2              # t2 = number of argument bytes remain
    daddiu $v0, $sp, 4           # v0 points to where to copy arg_array
    LOOP_OVER_SHORTY_LOADING_REG a1, f13, call_sfn
    LOOP_OVER_SHORTY_LOADING_REG a2, f14, call_sfn
    LOOP_OVER_SHORTY_LOADING_REG a3, f15, call_sfn
    LOOP_OVER_SHORTY_LOADING_REG a4, f16, call_sfn
    LOOP_OVER_SHORTY_LOADING_REG a5, f17, call_sfn
    LOOP_OVER_SHORTY_LOADING_REG a6, f18, call_sfn
    LOOP_OVER_SHORTY_LOADING_REG a7, f19, call_sfn

    # copy arguments onto stack (t2 should be multiples of 4)
    ble    $t2, $zero, call_sfn  # t2 = number of argument bytes remain
1:
    lw     $t3, 0($t1)           # load from argument array
    daddiu $t1, $t1, 4
    sw     $t3, 0($v0)           # save to stack
    daddiu $t2, -4
    bgt    $t2, $zero, 1b        # t2 = number of argument bytes remain
    daddiu $v0, $v0, 4

call_sfn:
    # call method (a0 has been untouched)
    sw     $zero, 0($sp)         # store NULL for method* at bottom of frame
    ld     $t9, MIRROR_ART_METHOD_QUICK_CODE_OFFSET_64($a0)  # get pointer to the code
    jalr   $t9                   # call the method
    nop
    move   $sp, $s8              # restore sp

    # pop a4, a5, s0(rSUSPEND), s1(rSELF), s8, ra off of the stack
    ld     $a4, 0($sp)
    .cfi_restore 8
    ld     $a5, 8($sp)
    .cfi_restore 9
    ld     $s0, 16($sp)
    .cfi_restore 16
    ld     $s1, 24($sp)
    .cfi_restore 17
    ld     $s8, 32($sp)
    .cfi_restore 30
    ld     $ra, 40($sp)
    .cfi_restore 31
    daddiu $sp, $sp, 48
    .cfi_adjust_cfa_offset -48

    # a4 = JValue* result
    # a5 = shorty string
    lbu   $t1, 0($a5)           # get result type from shorty
    li    $t2, 68               # put char 'D' into t2
    beq   $t1, $t2, 1f          # branch if result type char == 'D'
    li    $t3, 70               # put char 'F' into t3
    beq   $t1, $t3, 1f          # branch if result type char == 'F'
    sw    $v0, 0($a4)           # store the result
    dsrl  $v1, $v0, 32
    jalr  $zero, $ra
    sw    $v1, 4($a4)           # store the other half of the result
1:
    mfc1  $v0, $f0
    mfhc1 $v1, $f0
    sw    $v0, 0($a4)           # store the result
    jalr  $zero, $ra
    sw    $v1, 4($a4)           # store the other half of the result
END art_quick_invoke_static_stub



UNIMPLEMENTED art_quick_handle_fill_data
UNIMPLEMENTED art_quick_lock_object
UNIMPLEMENTED art_quick_unlock_object
UNIMPLEMENTED art_quick_check_cast
UNIMPLEMENTED art_quick_aput_obj_with_null_and_bound_check
UNIMPLEMENTED art_quick_aput_obj_with_bound_check
UNIMPLEMENTED art_quick_aput_obj
UNIMPLEMENTED art_quick_initialize_static_storage
UNIMPLEMENTED art_quick_initialize_type
UNIMPLEMENTED art_quick_initialize_type_and_verify_access
UNIMPLEMENTED art_quick_get_boolean_static
UNIMPLEMENTED art_quick_get_byte_static
UNIMPLEMENTED art_quick_get_char_static
UNIMPLEMENTED art_quick_get_short_static
UNIMPLEMENTED art_quick_get32_static
UNIMPLEMENTED art_quick_get64_static
UNIMPLEMENTED art_quick_get_obj_static
UNIMPLEMENTED art_quick_get_boolean_instance
UNIMPLEMENTED art_quick_get_byte_instance
UNIMPLEMENTED art_quick_get_char_instance
UNIMPLEMENTED art_quick_get_short_instance
UNIMPLEMENTED art_quick_get32_instance
UNIMPLEMENTED art_quick_get64_instance
UNIMPLEMENTED art_quick_get_obj_instance
UNIMPLEMENTED art_quick_set8_static
UNIMPLEMENTED art_quick_set16_static
UNIMPLEMENTED art_quick_set32_static
UNIMPLEMENTED art_quick_set64_static
UNIMPLEMENTED art_quick_set_obj_static
UNIMPLEMENTED art_quick_set8_instance
UNIMPLEMENTED art_quick_set16_instance
UNIMPLEMENTED art_quick_set32_instance
UNIMPLEMENTED art_quick_set64_instance
UNIMPLEMENTED art_quick_set_obj_instance
UNIMPLEMENTED art_quick_resolve_string

// Macro to facilitate adding new allocation entrypoints.
.macro TWO_ARG_DOWNCALL name, entrypoint, return
ENTRY \name
    break
    break
END \name
.endm

.macro THREE_ARG_DOWNCALL name, entrypoint, return
ENTRY \name
    break
    break
END \name
.endm

// Generate the allocation entrypoints for each allocator.
GENERATE_ALL_ALLOC_ENTRYPOINTS

UNIMPLEMENTED art_quick_test_suspend
UNIMPLEMENTED art_quick_proxy_invoke_handler
UNIMPLEMENTED art_quick_imt_conflict_trampoline
UNIMPLEMENTED art_quick_resolution_trampoline

    .extern artQuickGenericJniTrampoline
    .extern artQuickGenericJniEndTrampoline
ENTRY art_quick_generic_jni_trampoline
    SETUP_REFS_AND_ARGS_CALLEE_SAVE_FRAME_INTERNAL
    sd      $a0, 0($sp)            # store native ArtMethod* to bottom of stack
    move    $s8, $sp               # save $sp

    # prepare for call to artQuickGenericJniTrampoline(Thread*, SP)
    move    $a0, rSELF             # pass Thread::Current
    move    $a1, $sp               # pass $sp
    jal     artQuickGenericJniTrampoline   # (Thread*, SP)
    daddiu  $sp, $sp, -5120        # reserve space on the stack

    # The C call will have registered the complete save-frame on success.
    # The result of the call is:
    # v0: ptr to native code, 0 on error.
    # v1: ptr to the bottom of the used area of the alloca, can restore stack till here.
    beq     $v0, $zero, 1f         # check entry error
    move    $t9, $v0               # save the code ptr
    move    $sp, $v1               # release part of the alloca

    # Load parameters from stack into registers
    ld      $a0,   0($sp)
    ld      $a1,   8($sp)
    ld      $a2,  16($sp)
    ld      $a3,  24($sp)
    ld      $a4,  32($sp)
    ld      $a5,  40($sp)
    ld      $a6,  48($sp)
    ld      $a7,  56($sp)
    # Load FPRs the same as GPRs. Look at BuildNativeCallFrameStateMachine.
    l.d     $f12,  0($sp)
    l.d     $f13,  8($sp)
    l.d     $f14, 16($sp)
    l.d     $f15, 24($sp)
    l.d     $f16, 32($sp)
    l.d     $f17, 40($sp)
    l.d     $f18, 48($sp)
    l.d     $f19, 56($sp)
    jalr    $t9                    # native call
    daddiu  $sp, $sp, 64

    # result sign extension is handled in C code
    # prepare for call to artQuickGenericJniEndTrampoline(Thread*, result, result_f)
    move    $a0, rSELF             # pass Thread::Current
    move    $a1, $v0
    jal     artQuickGenericJniEndTrampoline
    dmfc1   $a2, $f0

    ld      $t0, THREAD_EXCEPTION_OFFSET(rSELF) # load Thread::Current()->exception_
    bne     $t0, $zero, 2f         # check for pending exceptions
    move    $sp, $s8               # tear down the alloca

    # tear dpown the callee-save frame
    RESTORE_REFS_AND_ARGS_CALLEE_SAVE_FRAME

    jalr    $zero, $ra
    dmtc1   $v0, $f0               # place return value to FP return value

1:
    move    $sp, $s8               # tear down the alloca
2:
    RESTORE_REFS_AND_ARGS_CALLEE_SAVE_FRAME
    DELIVER_PENDING_EXCEPTION
END art_quick_generic_jni_trampoline

    .extern artQuickToInterpreterBridge
ENTRY art_quick_to_interpreter_bridge
    SETUP_REFS_AND_ARGS_CALLEE_SAVE_FRAME
    move    $a1, rSELF             # pass Thread::Current
    jal     artQuickToInterpreterBridge    # (Method* method, Thread*, SP)
    move    $a2, $sp               # pass $sp
    ld      $t0, THREAD_EXCEPTION_OFFSET(rSELF) # load Thread::Current()->exception_
    daddiu  $sp, $sp, REFS_AND_ARGS_MINUS_REFS_SIZE  # skip a0-a7 and f12-f19
    RESTORE_REFS_ONLY_CALLEE_SAVE_FRAME
    bne     $t0, $zero, 1f
    dmtc1   $v0, $f0               # place return value to FP return value
    jalr    $zero, $ra
    dmtc1   $v1, $f1               # place return value to FP return value
1:
    DELIVER_PENDING_EXCEPTION
END art_quick_to_interpreter_bridge

    /*
     * Routine that intercepts method calls and returns.
     */
    .extern artInstrumentationMethodEntryFromCode
    .extern artInstrumentationMethodExitFromCode
ENTRY art_quick_instrumentation_entry
    SETUP_REFS_AND_ARGS_CALLEE_SAVE_FRAME
    daddiu   $sp, $sp, -16     # space for saving arg0
    .cfi_adjust_cfa_offset 16
    sd       $a0, 0($sp)       # save arg0
    move     $a3, $ra          # pass $ra
    jal      artInstrumentationMethodEntryFromCode  # (Method*, Object*, Thread*, RA)
    move     $a2, rSELF        # pass Thread::Current
    move     $t9, $v0          # $t9 holds reference to code
    ld       $a0, 0($sp)       # restore arg0
    daddiu   $sp, $sp, 16      # remove args
    .cfi_adjust_cfa_offset -16
    RESTORE_REFS_AND_ARGS_CALLEE_SAVE_FRAME
    jalr     $t9               # call method
    nop
END art_quick_instrumentation_entry
    /* intentional fallthrough */
    .global art_quick_instrumentation_exit
art_quick_instrumentation_exit:
    .cfi_startproc
    daddiu   $t9, $ra, 4       # put current address into $t9 to rebuild $gp
    .cpload  $t9
    move     $ra, $zero        # link register is to here, so clobber with 0 for later checks
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME
    move     $t0, $sp          # remember bottom of caller's frame
    daddiu   $sp, $sp, -16     # save return values and set up args
    .cfi_adjust_cfa_offset 16
    sd       $v0, 0($sp)
    .cfi_rel_offset 2, 0
    s.d      $f0, 8($sp)
    mov.d    $f15, $f0         # pass fpr result
    move     $a2, $v0          # pass gpr result
    move     $a1, $t0          # pass $sp
    jal      artInstrumentationMethodExitFromCode  # (Thread*, SP, gpr_res, fpr_res)
    move     $a0, rSELF        # pass Thread::Current
    move     $t0, $v0          # set aside returned link register
    move     $ra, $v1          # set link register for deoptimization
    ld       $v0, 0($sp)       # restore return values
    l.d      $f0, 8($sp)
    jalr     $zero, $t0        # return
    daddiu   $sp, $sp, 16+FRAME_SIZE_REFS_ONLY_CALLEE_SAVE  # 16 bytes of saved values + ref_only callee save frame
    .cfi_adjust_cfa_offset -(16+FRAME_SIZE_REFS_ONLY_CALLEE_SAVE)
END art_quick_instrumentation_exit

UNIMPLEMENTED art_quick_deoptimize
UNIMPLEMENTED art_quick_indexof
UNIMPLEMENTED art_quick_string_compareto
