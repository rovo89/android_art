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

#include "asm_support_arm64.S"

#include "arch/quick_alloc_entrypoints.S"


    /*
     * Macro that sets up the callee save frame to conform with
     * Runtime::CreateCalleeSaveMethod(kSaveAll)
     */
.macro SETUP_SAVE_ALL_CALLEE_SAVE_FRAME
    adrp x9, :got:_ZN3art7Runtime9instance_E
    ldr x9, [x9, #:got_lo12:_ZN3art7Runtime9instance_E]

    // Our registers aren't intermixed - just spill in order.
    ldr x9,[x9]  // x9 = & (art::Runtime * art::Runtime.instance_) .

    // x9 = (ArtMethod*) Runtime.instance_.callee_save_methods[kRefAndArgs]  .
    ldr x9, [x9, RUNTIME_SAVE_ALL_CALLEE_SAVE_FRAME_OFFSET ]

    sub sp, sp, #368
    .cfi_adjust_cfa_offset 368

    // FP args
    stp d1, d2,   [sp, #8]
    stp d2, d3, [sp, #24]
    stp d4, d5, [sp, #40]
    stp d6, d7, [sp, #56]

    // FP callee-saves
    stp d8, d9,   [sp, #72]
    stp d10, d11, [sp, #88]
    stp d12, d13, [sp, #104]
    stp d14, d15, [sp, #120]

    stp d16, d17,   [sp, #136]
    stp d18, d19,   [sp, #152]
    stp d20, d21,   [sp, #168]
    stp d22, d23,   [sp, #184]
    stp d24, d25,   [sp, #200]
    stp d26, d27,   [sp, #216]
    stp d28, d29,   [sp, #232]
    stp d30, d31,   [sp, #248]


    // Callee saved.
    stp xSELF, x19, [sp, #264]
    stp x20, x21, [sp, #280]
    stp x22, x23, [sp, #296]
    stp x24, x25, [sp, #312]
    stp x26, x27, [sp, #328]
    stp x28, xFP, [sp, #344]    // Save FP.
    str xLR, [sp, #360]

    .cfi_offset x18,72
    .cfi_offset x19,80
    .cfi_offset x20,88
    .cfi_offset x21,96
    .cfi_offset x22,104
    .cfi_offset x23,112
    .cfi_offset x24,120
    .cfi_offset x25,128
    .cfi_offset x26,136
    .cfi_offset x27,144
    .cfi_offset x28,152
    .cfi_offset x29,160
    .cfi_offset x30,168

    // Loads appropriate callee-save-method
    str x9, [sp]    // Store ArtMethod* Runtime::callee_save_methods_[kRefsAndArgs]

.endm

    /*
     * Macro that sets up the callee save frame to conform with
     * Runtime::CreateCalleeSaveMethod(kRefsOnly).
     */
.macro SETUP_REF_ONLY_CALLEE_SAVE_FRAME
    brk 0
.endm

.macro RESTORE_REF_ONLY_CALLEE_SAVE_FRAME
    brk 0
.endm

.macro RESTORE_REF_ONLY_CALLEE_SAVE_FRAME_AND_RETURN
    brk 0
.endm


.macro SETUP_REF_AND_ARGS_CALLEE_SAVE_FRAME_INTERNAL
    sub sp, sp, #304
    .cfi_adjust_cfa_offset 304

    stp d0, d1,   [sp, #16]
    stp d2, d3,   [sp, #32]
    stp d4, d5,   [sp, #48]
    stp d6, d7,   [sp, #64]
    stp d8, d9,   [sp, #80]
    stp d10, d11, [sp, #96]
    stp d12, d13, [sp, #112]
    stp d14, d15, [sp, #128]

    stp x1,  x2, [sp, #144]
    stp x3,  x4, [sp, #160]
    stp x5,  x6, [sp, #176]
    stp x7,  xSELF, [sp, #192]
    stp x19, x20, [sp, #208]
    stp x21, x22, [sp, #224]
    stp x23, x24, [sp, #240]
    stp x25, x26, [sp, #256]
    stp x27, x28, [sp, #272]
    stp xFP, xLR, [sp, #288]

    .cfi_offset x1,144
    .cfi_offset x2,152
    .cfi_offset x3,160
    .cfi_offset x4,168
    .cfi_offset x5,176
    .cfi_offset x6,184
    .cfi_offset x7,192
    .cfi_offset x18,200
    .cfi_offset x19,208
    .cfi_offset x20,216
    .cfi_offset x21,224
    .cfi_offset x22,232
    .cfi_offset x23,240
    .cfi_offset x24,248
    .cfi_offset x25,256
    .cfi_offset x26,264
    .cfi_offset x27,272
    .cfi_offset x28,280
    .cfi_offset x29,288
    .cfi_offset x30,296
.endm

    /*
     * Macro that sets up the callee save frame to conform with
     * Runtime::CreateCalleeSaveMethod(kRefsAndArgs).
     *
     * TODO This is probably too conservative - saving FP & LR.
     */
.macro SETUP_REF_AND_ARGS_CALLEE_SAVE_FRAME
    adrp x9, :got:_ZN3art7Runtime9instance_E
    ldr x9, [x9, #:got_lo12:_ZN3art7Runtime9instance_E]

    // Our registers aren't intermixed - just spill in order.
    ldr x9,[x9]  // x9 = & (art::Runtime * art::Runtime.instance_) .

    // x9 = (ArtMethod*) Runtime.instance_.callee_save_methods[kRefAndArgs]  .
    ldr x9, [x9, RUNTIME_REF_AND_ARGS_CALLEE_SAVE_FRAME_OFFSET ]

    SETUP_REF_AND_ARGS_CALLEE_SAVE_FRAME_INTERNAL

    str x9, [sp]    // Store ArtMethod* Runtime::callee_save_methods_[kRefsAndArgs]
.endm

.macro RESTORE_REF_AND_ARGS_CALLEE_SAVE_FRAME

    ldp d0, d1,   [sp, #16]
    ldp d2, d3,   [sp, #32]
    ldp d4, d5,   [sp, #48]
    ldp d6, d7,   [sp, #64]
    ldp d8, d9,   [sp, #80]
    ldp d10, d11, [sp, #96]
    ldp d12, d13, [sp, #112]
    ldp d14, d15, [sp, #128]

    // args.
    ldp x1,  x2, [sp, #144]
    ldp x3,  x4, [sp, #160]
    ldp x5,  x6, [sp, #176]
    ldp x7,  xSELF, [sp, #192]
    ldp x19, x20, [sp, #208]
    ldp x21, x22, [sp, #224]
    ldp x23, x24, [sp, #240]
    ldp x25, x26, [sp, #256]
    ldp x27, x28, [sp, #272]
    ldp xFP, xLR, [sp, #288]

    add sp, sp, #304
    .cfi_adjust_cfa_offset -304
.endm

.macro RETURN_IF_RESULT_IS_ZERO
    brk 0
.endm

.macro RETURN_IF_RESULT_IS_NON_ZERO
    brk 0
.endm

    /*
     * Macro that set calls through to artDeliverPendingExceptionFromCode, where the pending
     * exception is Thread::Current()->exception_
     */
.macro DELIVER_PENDING_EXCEPTION
    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME
    mov x0, xSELF
    mov x1, sp

    // Point of no return.
    b artDeliverPendingExceptionFromCode  // artDeliverPendingExceptionFromCode(Thread*, SP)
    brk 0  // Unreached
.endm

.macro RETURN_OR_DELIVER_PENDING_EXCEPTION
    ldr x9, [xSELF, # THREAD_EXCEPTION_OFFSET]   // Get exception field.
    cbnz x9, 1f
    ret
1:
    DELIVER_PENDING_EXCEPTION
.endm

.macro NO_ARG_RUNTIME_EXCEPTION c_name, cxx_name
    .extern \cxx_name
ENTRY \c_name
    brk 0
END \c_name
.endm

.macro ONE_ARG_RUNTIME_EXCEPTION c_name, cxx_name
    .extern \cxx_name
ENTRY \c_name
    brk 0
END \c_name
.endm

.macro TWO_ARG_RUNTIME_EXCEPTION c_name, cxx_name
    .extern \cxx_name
ENTRY \c_name
    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME  // save all registers as basis for long jump context
    brk 0
END \c_name
.endm

    /*
     * Called by managed code, saves callee saves and then calls artThrowException
     * that will place a mock Method* at the bottom of the stack. Arg1 holds the exception.
     */
ONE_ARG_RUNTIME_EXCEPTION art_quick_deliver_exception, artDeliverExceptionFromCode

    /*
     * Called by managed code to create and deliver a NullPointerException.
     */
NO_ARG_RUNTIME_EXCEPTION art_quick_throw_null_pointer_exception, artThrowNullPointerExceptionFromCode

    /*
     * Called by managed code to create and deliver an ArithmeticException.
     */
NO_ARG_RUNTIME_EXCEPTION art_quick_throw_div_zero, artThrowDivZeroFromCode

    /*
     * Called by managed code to create and deliver an ArrayIndexOutOfBoundsException. Arg1 holds
     * index, arg2 holds limit.
     */
TWO_ARG_RUNTIME_EXCEPTION art_quick_throw_array_bounds, artThrowArrayBoundsFromCode

    /*
     * Called by managed code to create and deliver a StackOverflowError.
     */
NO_ARG_RUNTIME_EXCEPTION art_quick_throw_stack_overflow, artThrowStackOverflowFromCode

    /*
     * Called by managed code to create and deliver a NoSuchMethodError.
     */
ONE_ARG_RUNTIME_EXCEPTION art_quick_throw_no_such_method, artThrowNoSuchMethodFromCode

    /*
     * TODO arm64 specifics need to be fleshed out.
     * All generated callsites for interface invokes and invocation slow paths will load arguments
     * as usual - except instead of loading x0 with the target Method*, x0 will contain
     * the method_idx.  This wrapper will save x1-x3, load the caller's Method*, align the
     * stack and call the appropriate C helper.
     * NOTE: "this" is first visible argument of the target, and so can be found in x1.
     *
     * The helper will attempt to locate the target and return a result in x0 consisting
     * of the target Method* in x0 and method->code_ in x1.
     *
     * If unsuccessful, the helper will return NULL/NULL. There will be a pending exception in the
     * thread and we branch to another stub to deliver it.
     *
     * On success this wrapper will restore arguments and *jump* to the target, leaving the lr
     * pointing back to the original caller.
     */
.macro INVOKE_TRAMPOLINE c_name, cxx_name
    .extern \cxx_name
ENTRY \c_name
    brk 0
END \c_name
.endm

INVOKE_TRAMPOLINE art_quick_invoke_interface_trampoline, artInvokeInterfaceTrampoline
INVOKE_TRAMPOLINE art_quick_invoke_interface_trampoline_with_access_check, artInvokeInterfaceTrampolineWithAccessCheck

INVOKE_TRAMPOLINE art_quick_invoke_static_trampoline_with_access_check, artInvokeStaticTrampolineWithAccessCheck
INVOKE_TRAMPOLINE art_quick_invoke_direct_trampoline_with_access_check, artInvokeDirectTrampolineWithAccessCheck
INVOKE_TRAMPOLINE art_quick_invoke_super_trampoline_with_access_check, artInvokeSuperTrampolineWithAccessCheck
INVOKE_TRAMPOLINE art_quick_invoke_virtual_trampoline_with_access_check, artInvokeVirtualTrampolineWithAccessCheck

/*
 *  extern"C" void art_quick_invoke_stub(ArtMethod *method,   x0
 *                                       uint32_t  *args,     x1
 *                                       uint32_t argsize,    w2
 *                                       Thread *self,        x3
 *                                       JValue *result,      x4
 *                                       char   *shorty);     x5
 *  +----------------------+
 *  |                      |
 *  |  C/C++ frame         |
 *  |       LR''           |
 *  |       FP''           | <- SP'
 *  +----------------------+
 *  +----------------------+
 *  |        SP'           |
 *  |        X5            |
 *  |        X4            |        Saved registers
 *  |        LR'           |
 *  |        FP'           | <- FP
 *  +----------------------+
 *  | uint32_t out[n-1]    |
 *  |    :      :          |        Outs
 *  | uint32_t out[0]      |
 *  | ArtMethod* NULL      | <- SP
 *  +----------------------+
 *
 * Outgoing registers:
 *  x0    - Method*
 *  x1-x7 - integer parameters.
 *  d0-d7 - Floating point parameters.
 *  xSELF = self
 *  SP = & of ArtMethod*
 *  x1 = "this" pointer.
 *
 */
ENTRY art_quick_invoke_stub
    // Spill registers as per AACPS64 calling convention.

SAVE_SIZE=5*8   // x4, x5, LR & FP saved.
SAVE_SIZE_AND_METHOD=SAVE_SIZE+8

    mov x9, sp     // Save stack pointer.

    mov x10, xFP   // Save frame pointer
    .cfi_register x29,x10
    add x11, x2, # SAVE_SIZE_AND_METHOD // calculate size of frame.

    sub x11, sp, x11 // Calculate SP position - saves + ArtMethod* +  args

    and x11, x11, # ~0xf  // Enforce 16 byte stack alignment.

    sub xFP, x9, #SAVE_SIZE   // Calculate new FP. Don't store here until SP moved.
    .cfi_def_cfa_register x29

    mov sp, x11        // set new SP.

    str x9, [xFP, #32]     // Save old stack pointer.

    .cfi_offset x9, 32

    stp x4, x5, [xFP, #16]  // Save result and shorty addresses.

    .cfi_offset x4, 16
    .cfi_offset x5, 24

    stp x10, xLR, [xFP]   // Store lr & old fp @ fp

    .cfi_offset x30, 0
    .cfi_offset x10, 8

    mov xSELF, x3       // Move thread pointer into SELF register.

    // Copy arguments into stack frame.
    // Use simple copy routine for now.
    // 4 bytes per slot.
    // X1 - source address
    // W2 - args length
    // X10 - destination address.
    add x9, sp, #8     // Destination address is bottom of stack + NULL.

    // w2 = argsize parameter.
.LcopyParams:
    cmp w2, #0
    beq .LendCopyParams
    sub w2, w2, #4      // Need 65536 bytes of range.
    ldr w10, [x1, x2]
    str w10, [x9, x2]

    b .LcopyParams

.LendCopyParams:

    // Store NULL into Method* at bottom of frame.
    str xzr, [sp]

    // Fill registers x/w1 to x/w7 and s/d0 to s/d7 with parameters.
    // Parse the passed shorty to determine which register to load.
    // Load addresses for routines that load WXSD registers.
    adr  x11, .LstoreW2
    adr  x12, .LstoreX2
    adr  x13, .LstoreS0
    adr  x14, .LstoreD0

    // Initialize routine offsets to 0 for integers and floats.
    // x8 for integers, x15 for floating point.
    mov x8, #0
    mov x15, #0

    add x10, x5, #1         // Load shorty address, plus one to skip return value.
    ldr w1, [x9],#4         // Load "this" parameter, and increment arg pointer.

    // Loop to fill registers.
.LfillRegisters:
    ldrb w17, [x10], #1       // Load next character in signature, and increment.
    cbz w17, .LcallFunction   // Exit at end of signature. Shorty 0 terminated.

    cmp  w17, #'F' // is this a float?
    bne .LisDouble

    cmp x15, # 8*12         // Skip this load if all registers full.
    beq .LfillRegisters

    add x17, x13, x15       // Calculate subroutine to jump to.
    br  x17

.LisDouble:
    cmp w17, #'D'           // is this a double?
    bne .LisLong

    cmp x15, # 8*12         // Skip this load if all registers full.
    beq .LfillRegisters


    add x17, x14, x15       // Calculate subroutine to jump to.
    br x17

.LisLong:
    cmp w17, #'J'           // is this a long?
    bne .LisOther

    cmp x8, # 7*12          // Skip this load if all registers full.
    beq .LfillRegisters

    add x17, x12, x8        // Calculate subroutine to jump to.
    br x17


.LisOther:                  // Everything else takes one vReg.
    cmp x8, # 7*12          // Skip this load if all registers full.
    beq .LfillRegisters
    add x17, x11, x8        // Calculate subroutine to jump to.
    br x17

// Macro for loading a parameter into a register.
//  counter - the register with offset into these tables
//  size - the size of the register - 4 or 8 bytes.
//  register - the name of the register to be loaded.
.macro LOADREG counter size register return
    ldr \register , [x9], #\size
    add \counter, \counter, 12
    b \return
.endm

// Store ints.
.LstoreW2:
    LOADREG x8 4 w2 .LfillRegisters
    LOADREG x8 4 w3 .LfillRegisters
    LOADREG x8 4 w4 .LfillRegisters
    LOADREG x8 4 w5 .LfillRegisters
    LOADREG x8 4 w6 .LfillRegisters
    LOADREG x8 4 w7 .LfillRegisters

// Store longs.
.LstoreX2:
    LOADREG x8 8 x2 .LfillRegisters
    LOADREG x8 8 x3 .LfillRegisters
    LOADREG x8 8 x4 .LfillRegisters
    LOADREG x8 8 x5 .LfillRegisters
    LOADREG x8 8 x6 .LfillRegisters
    LOADREG x8 8 x7 .LfillRegisters

// Store singles.
.LstoreS0:
    LOADREG x15 4 s0 .LfillRegisters
    LOADREG x15 4 s1 .LfillRegisters
    LOADREG x15 4 s2 .LfillRegisters
    LOADREG x15 4 s3 .LfillRegisters
    LOADREG x15 4 s4 .LfillRegisters
    LOADREG x15 4 s5 .LfillRegisters
    LOADREG x15 4 s6 .LfillRegisters
    LOADREG x15 4 s7 .LfillRegisters

// Store doubles.
.LstoreD0:
    LOADREG x15 8 d0 .LfillRegisters
    LOADREG x15 8 d1 .LfillRegisters
    LOADREG x15 8 d2 .LfillRegisters
    LOADREG x15 8 d3 .LfillRegisters
    LOADREG x15 8 d4 .LfillRegisters
    LOADREG x15 8 d5 .LfillRegisters
    LOADREG x15 8 d6 .LfillRegisters
    LOADREG x15 8 d7 .LfillRegisters


.LcallFunction:

    // load method-> METHOD_QUICK_CODE_OFFSET
    ldr x9, [x0 , #METHOD_QUICK_CODE_OFFSET]
    // Branch to method.
    blr x9

    // Restore return value address and shorty address.
    ldp x4,x5, [xFP, #16]
    .cfi_restore x4
    .cfi_restore x5

    // Store result (w0/x0/s0/d0) appropriately, depending on resultType.
    ldrb w10, [x5]

    // Don't set anything for a void type.
    cmp w10, #'V'
    beq .Lexit_art_quick_invoke_stub

    cmp w10, #'D'
    bne .Lreturn_is_float
    str d0, [x4]
    b .Lexit_art_quick_invoke_stub

.Lreturn_is_float:
    cmp w10, #'F'
    bne .Lreturn_is_int
    str s0, [x4]
    b .Lexit_art_quick_invoke_stub

    // Just store x0. Doesn't matter if it is 64 or 32 bits.
.Lreturn_is_int:
    str x0, [x4]

.Lexit_art_quick_invoke_stub:
    ldr x2, [x29, #32]   // Restore stack pointer.
    mov sp, x2
    .cfi_restore sp

    ldp x29, x30, [x29]    // Restore old frame pointer and link register.
    .cfi_restore x29
    .cfi_restore x30

    ret
END art_quick_invoke_stub

/*  extern"C"
 *     void art_quick_invoke_static_stub(ArtMethod *method,   x0
 *                                       uint32_t  *args,     x1
 *                                       uint32_t argsize,    w2
 *                                       Thread *self,        x3
 *                                       JValue *result,      x4
 *                                       char   *shorty);     x5
 */
ENTRY art_quick_invoke_static_stub
    // Spill registers as per AACPS64 calling convention.

SAVE_SIZE=5*8   // x4, x5, SP, LR & FP saved
SAVE_SIZE_AND_METHOD=SAVE_SIZE+8

    mov x9, sp     // Save stack pointer.

    mov x10, xFP   // Save frame pointer
    .cfi_register x29,x10
    add x11, x2, # SAVE_SIZE_AND_METHOD // calculate size of frame.

    sub x11, sp, x11 // Calculate SP position - saves + ArtMethod* +  args

    and x11, x11, # ~0xf  // Enforce 16 byte stack alignment.

    sub xFP, x9, #SAVE_SIZE   // Calculate new FP. Don't store here until SP moved.

    mov sp, x11        // set new SP.

    .cfi_def_cfa_register   29

    str x9, [xFP, #32]     // Save old stack pointer.

    .cfi_offset x9, 32

    stp x4, x5, [xFP, #16]  // Save result and shorty addresses.

    .cfi_offset x4, 16
    .cfi_offset x5, 24

    stp x10, xLR, [x29]   // Store lr & old fp @ fp

    .cfi_offset x30, 0
    .cfi_offset x10, 8

    mov xSELF, x3       // Move thread pointer into SELF register.

    // Copy arguments into stack frame.
    // Use simple copy routine for now.
    // 4 bytes per slot.
    // X1 - source address
    // W2 - args length
    // X10 - destination address.
    add x9, sp, #8     // Destination address is bottom of stack + NULL.

    // w2 = argsize parameter.
.LcopyParams2:
    cmp w2, #0
    beq .LendCopyParams2
    sub w2, w2, #4      // Need 65536 bytes of range.
    ldr w10, [x1, x2]
    str w10, [x9, x2]

    b .LcopyParams2

.LendCopyParams2:

    // Store NULL into Method* at bottom of frame.
    str xzr, [sp]

    // Fill registers x/w1 to x/w7 and s/d0 to s/d7 with parameters.
    // Parse the passed shorty to determine which register to load.
    // Load addresses for routines that load WXSD registers.
    adr  x11, .LstoreW1_2
    adr  x12, .LstoreX1_2
    adr  x13, .LstoreS0_2
    adr  x14, .LstoreD0_2

    // Initialize routine offsets to 0 for integers and floats.
    // x8 for integers, x15 for floating point.
    mov x8, #0
    mov x15, #0

    add x10, x5, #1     // Load shorty address, plus one to skip return value.

    // Loop to fill registers.
.LfillRegisters2:
    ldrb w17, [x10], #1         // Load next character in signature, and increment.
    cbz w17, .LcallFunction2    // Exit at end of signature. Shorty 0 terminated.

    cmp  w17, #'F'          // is this a float?
    bne .LisDouble2

    cmp x15, # 8*12         // Skip this load if all registers full.
    beq .LfillRegisters2

    add x17, x13, x15       // Calculate subroutine to jump to.
    br  x17

.LisDouble2:
    cmp w17, #'D'           // is this a double?
    bne .LisLong2

    cmp x15, # 8*12         // Skip this load if all registers full.
    beq .LfillRegisters2


    add x17, x14, x15       // Calculate subroutine to jump to.
    br x17

.LisLong2:
    cmp w17, #'J'           // is this a long?
    bne .LisOther2

    cmp x8, # 7*12          // Skip this load if all registers full.
    beq .LfillRegisters2

    add x17, x12, x8        // Calculate subroutine to jump to.
    br x17


.LisOther2:                 // Everything else takes one vReg.
    cmp x8, # 7*12          // Skip this load if all registers full.
    beq .LfillRegisters2
    add x17, x11, x8        // Calculate subroutine to jump to.
    br x17

// Store ints.
.LstoreW1_2:
    LOADREG x8 4 w1 .LfillRegisters2
    LOADREG x8 4 w2 .LfillRegisters2
    LOADREG x8 4 w3 .LfillRegisters2
    LOADREG x8 4 w4 .LfillRegisters2
    LOADREG x8 4 w5 .LfillRegisters2
    LOADREG x8 4 w6 .LfillRegisters2
    LOADREG x8 4 w7 .LfillRegisters2

// Store longs.
.LstoreX1_2:
    LOADREG x8 8 x1 .LfillRegisters2
    LOADREG x8 8 x2 .LfillRegisters2
    LOADREG x8 8 x3 .LfillRegisters2
    LOADREG x8 8 x4 .LfillRegisters2
    LOADREG x8 8 x5 .LfillRegisters2
    LOADREG x8 8 x6 .LfillRegisters2
    LOADREG x8 8 x7 .LfillRegisters2

// Store singles.
.LstoreS0_2:
    LOADREG x15 4 s0 .LfillRegisters2
    LOADREG x15 4 s1 .LfillRegisters2
    LOADREG x15 4 s2 .LfillRegisters2
    LOADREG x15 4 s3 .LfillRegisters2
    LOADREG x15 4 s4 .LfillRegisters2
    LOADREG x15 4 s5 .LfillRegisters2
    LOADREG x15 4 s6 .LfillRegisters2
    LOADREG x15 4 s7 .LfillRegisters2

// Store doubles.
.LstoreD0_2:
    LOADREG x15 8 d0 .LfillRegisters2
    LOADREG x15 8 d1 .LfillRegisters2
    LOADREG x15 8 d2 .LfillRegisters2
    LOADREG x15 8 d3 .LfillRegisters2
    LOADREG x15 8 d4 .LfillRegisters2
    LOADREG x15 8 d5 .LfillRegisters2
    LOADREG x15 8 d6 .LfillRegisters2
    LOADREG x15 8 d7 .LfillRegisters2


.LcallFunction2:

    // load method-> METHOD_QUICK_CODE_OFFSET.
    ldr x9, [x0 , #METHOD_QUICK_CODE_OFFSET]
    // Branch to method.
    blr x9

    // Restore return value address and shorty address.
    ldp x4, x5, [xFP, #16]
    .cfi_restore x4
    .cfi_restore x5

    // Store result (w0/x0/s0/d0) appropriately, depending on resultType.
    ldrb w10, [x5]

    // Don't set anything for a void type.
    cmp w10, #'V'
    beq .Lexit_art_quick_invoke_stub2

    cmp w10, #'D'
    bne .Lreturn_is_float2
    str d0, [x4]
    b .Lexit_art_quick_invoke_stub2

.Lreturn_is_float2:
    cmp w10, #'F'
    bne .Lreturn_is_int2
    str s0, [x4]
    b .Lexit_art_quick_invoke_stub2

    // Just store x0. Doesn't matter if it is 64 or 32 bits.
.Lreturn_is_int2:
    str x0, [x4]

.Lexit_art_quick_invoke_stub2:

    ldr x2, [xFP, #32]   // Restore stack pointer.
    mov sp, x2
    .cfi_restore sp

    ldp xFP, xLR, [xFP]    // Restore old frame pointer and link register.
    .cfi_restore x29
    .cfi_restore x30

    ret
END art_quick_invoke_static_stub

// UNIMPLEMENTED art_quick_do_long_jump

    /*
     * On entry x0 is uintptr_t* gprs_ and x1 is uint64_t* fprs_
     */

ENTRY art_quick_do_long_jump
    // Load FPRs
    ldp d0, d1, [x1], #16
    ldp d2, d3, [x1], #16
    ldp d4, d5, [x1], #16
    ldp d6, d7, [x1], #16
    ldp d8, d9, [x1], #16
    ldp d10, d11, [x1], #16
    ldp d12, d13, [x1], #16
    ldp d14, d15, [x1], #16
    ldp d16, d17, [x1], #16
    ldp d18, d19, [x1], #16
    ldp d20, d21, [x1], #16
    ldp d22, d23, [x1], #16
    ldp d24, d25, [x1], #16
    ldp d26, d27, [x1], #16
    ldp d28, d29, [x1], #16
    ldp d30, d31, [x1]

    // Load GPRs
    // TODO: lots of those are smashed, could optimize.
    add x0, x0, #30*8
    ldp x30, x1, [x0], #-16
    ldp x28, x29, [x0], #-16
    ldp x26, x27, [x0], #-16
    ldp x24, x25, [x0], #-16
    ldp x22, x23, [x0], #-16
    ldp x20, x21, [x0], #-16
    ldp x18, x19, [x0], #-16
    ldp x16, x17, [x0], #-16
    ldp x14, x15, [x0], #-16
    ldp x12, x13, [x0], #-16
    ldp x10, x11, [x0], #-16
    ldp x8, x9, [x0], #-16
    ldp x6, x7, [x0], #-16
    ldp x4, x5, [x0], #-16
    ldp x2, x3, [x0], #-16
    mov sp, x1

    // TODO: Is it really OK to use LR for the target PC?
    mov x0, #0
    mov x1, #0
    br  xLR
END art_quick_do_long_jump

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
UNIMPLEMENTED art_quick_get32_static
UNIMPLEMENTED art_quick_get64_static
UNIMPLEMENTED art_quick_get_obj_static
UNIMPLEMENTED art_quick_get32_instance
UNIMPLEMENTED art_quick_get64_instance
UNIMPLEMENTED art_quick_get_obj_instance
UNIMPLEMENTED art_quick_set32_static
UNIMPLEMENTED art_quick_set64_static
UNIMPLEMENTED art_quick_set_obj_static
UNIMPLEMENTED art_quick_set32_instance
UNIMPLEMENTED art_quick_set64_instance
UNIMPLEMENTED art_quick_set_obj_instance
UNIMPLEMENTED art_quick_resolve_string

// Macro to facilitate adding new allocation entrypoints.
.macro TWO_ARG_DOWNCALL name, entrypoint, return
    .extern \entrypoint
ENTRY \name
    brk 0
END \name
.endm

// Macro to facilitate adding new array allocation entrypoints.
.macro THREE_ARG_DOWNCALL name, entrypoint, return
    .extern \entrypoint
ENTRY \name
    brk 0
END \name
.endm

// Generate the allocation entrypoints for each allocator.
GENERATE_ALL_ALLOC_ENTRYPOINTS

UNIMPLEMENTED art_quick_test_suspend

/**
 * Returned by ClassLinker::GetOatCodeFor
 *
 */
UNIMPLEMENTED art_quick_proxy_invoke_handler

UNIMPLEMENTED art_quick_imt_conflict_trampoline


ENTRY art_quick_resolution_trampoline
    SETUP_REF_AND_ARGS_CALLEE_SAVE_FRAME
    mov x2, xSELF
    mov x3, sp
    bl artQuickResolutionTrampoline  // (called, receiver, Thread*, SP)
    mov x9, x0           // Remember returned code pointer in x9.
    RESTORE_REF_AND_ARGS_CALLEE_SAVE_FRAME
    cbz x9, 1f
    br x0
1:
    RESTORE_REF_AND_ARGS_CALLEE_SAVE_FRAME
    DELIVER_PENDING_EXCEPTION
END art_quick_resolution_trampoline

/*
 * Generic JNI frame layout:
 *
 * #-------------------#
 * |                   |
 * | caller method...  |
 * #-------------------#    <--- SP on entry
 * | Return X30/LR     |
 * | X29/FP            |    callee save
 * | X28               |    callee save
 * | X27               |    callee save
 * | X26               |    callee save
 * | X25               |    callee save
 * | X24               |    callee save
 * | X23               |    callee save
 * | X22               |    callee save
 * | X21               |    callee save
 * | X20               |    callee save
 * | X19               |    callee save
 * | X7                |    arg7
 * | X6                |    arg6
 * | X5                |    arg5
 * | X4                |    arg4
 * | X3                |    arg3
 * | X2                |    arg2
 * | X1                |    arg1
 * | D15               |    float arg 8
 * | D14               |    float arg 8
 * | D13               |    float arg 8
 * | D12               |    callee save
 * | D11               |    callee save
 * | D10               |    callee save
 * | D9                |    callee save
 * | D8                |    callee save
 * | D7                |    float arg 8
 * | D6                |    float arg 7
 * | D5                |    float arg 6
 * | D4                |    float arg 5
 * | D3                |    float arg 4
 * | D2                |    float arg 3
 * | D1                |    float arg 2
 * | D0                |    float arg 1
 * | RDI/Method*       |  <- X0
 * #-------------------#
 * | local ref cookie  | // 4B
 * |   SIRT size       | // 4B
 * #-------------------#
 * | JNI Call Stack    |
 * #-------------------#    <--- SP on native call
 * |                   |
 * | Stack for Regs    |    The trampoline assembly will pop these values
 * |                   |    into registers for native call
 * #-------------------#
 * | Native code ptr   |
 * #-------------------#
 * | Free scratch      |
 * #-------------------#
 * | Ptr to (1)        |    <--- SP
 * #-------------------#
 */
    /*
     * Called to do a generic JNI down-call
     */
ENTRY art_quick_generic_jni_trampoline
    SETUP_REF_AND_ARGS_CALLEE_SAVE_FRAME_INTERNAL
    str x0, [sp, #0]  // Store native ArtMethod* to bottom of stack.

    // Save SP , so we can have static CFI info.
    mov x28, sp
    .cfi_def_cfa_register x28

    // This looks the same, but is different: this will be updated to point to the bottom
    // of the frame when the SIRT is inserted.
    mov xFP, sp

    mov x8, #5120
    sub sp, sp, x8

    // prepare for artQuickGenericJniTrampoline call
    // (Thread*,  SP)
    //    x0      x1   <= C calling convention
    //   xSELF    xFP  <= where they are

    mov x0, xSELF   // Thread*
    mov x1, xFP
    bl artQuickGenericJniTrampoline  // (Thread*, sp)

    // Get the updated pointer. This is the bottom of the frame _with_ SIRT.
    ldr xFP, [sp]
    add x9, sp, #8

    cmp x0, #0
    b.mi .Lentry_error      // Check for error, negative value.

    // release part of the alloca.
    add x9, x9, x0

    // Get the code pointer
    ldr xIP0, [x9, #0]

    // Load parameters from frame into registers.
    // TODO Check with artQuickGenericJniTrampoline.
    //      Also, check again APPCS64 - the stack arguments are interleaved.
    ldp x0, x1, [x9, #8]
    ldp x2, x3, [x9, #24]
    ldp x4, x5, [x9, #40]
    ldp x6, x7, [x9, #56]

    ldp d0, d1, [x9, #72]
    ldp d2, d3, [x9, #88]
    ldp d4, d5, [x9, #104]
    ldp d6, d7, [x9, #120]

    add sp, x9, #136

    blr xIP0           // native call.

    // Restore self pointer.
    ldr xSELF, [x28, #200]

    // result sign extension is handled in C code
    // prepare for artQuickGenericJniEndTrampoline call
    // (Thread*,  SP, result, result_f)
    //   x0       x1   x2       x3       <= C calling convention
    mov x5, x0      // Save return value
    mov x0, xSELF   // Thread register
    mov x1, xFP     // Stack pointer
    mov x2, x5      // Result (from saved)
    fmov x3, d0     // d0 will contain floating point result, but needs to go into x3

    bl artQuickGenericJniEndTrampoline

    // Tear down the alloca.
    mov sp, x28
    .cfi_def_cfa_register sp

    // Restore self pointer.
    ldr xSELF, [x28, #200]

    // Pending exceptions possible.
    ldr x1, [xSELF, THREAD_EXCEPTION_OFFSET]
    cbnz x1, .Lexception_in_native

    // Tear down the callee-save frame.
    RESTORE_REF_AND_ARGS_CALLEE_SAVE_FRAME

    // store into fpr, for when it's a fpr return...
    fmov d0, x0
    ret

.Lentry_error:
    mov sp, x28
    .cfi_def_cfa_register sp
    ldr xSELF, [x28, #200]
.Lexception_in_native:
    RESTORE_REF_AND_ARGS_CALLEE_SAVE_FRAME
    DELIVER_PENDING_EXCEPTION

END art_quick_generic_jni_trampoline

/*
 * Called to bridge from the quick to interpreter ABI. On entry the arguments match those
 * of a quick call:
 * x0 = method being called/to bridge to.
 * x1..x7, d0..d7 = arguments to that method.
 */
ENTRY art_quick_to_interpreter_bridge
    SETUP_REF_AND_ARGS_CALLEE_SAVE_FRAME   // Set up frame and save arguments.

    //  x0 will contain mirror::ArtMethod* method.
    mov x1, xSELF                          // How to get Thread::Current() ???
    mov x2, sp

    // uint64_t artQuickToInterpreterBridge(mirror::ArtMethod* method, Thread* self,
    //                                      mirror::ArtMethod** sp)
    bl   artQuickToInterpreterBridge

    RESTORE_REF_AND_ARGS_CALLEE_SAVE_FRAME  // TODO: no need to restore arguments in this case.

    fmov d0, x0

    RETURN_OR_DELIVER_PENDING_EXCEPTION
END art_quick_to_interpreter_bridge

UNIMPLEMENTED art_quick_instrumentation_entry
UNIMPLEMENTED art_quick_instrumentation_exit
UNIMPLEMENTED art_quick_deoptimize
UNIMPLEMENTED art_quick_mul_long
UNIMPLEMENTED art_quick_shl_long
UNIMPLEMENTED art_quick_shr_long
UNIMPLEMENTED art_quick_ushr_long
UNIMPLEMENTED art_quick_indexof
UNIMPLEMENTED art_quick_string_compareto
