/*
 * Copyright (C) 2011 The Android Open Source Project
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

#ifndef ART_COMPILER_DEX_QUICK_ARM_ARM_LIR_H_
#define ART_COMPILER_DEX_QUICK_ARM_ARM_LIR_H_

#include "dex/compiler_internals.h"

namespace art {

/*
 * Runtime register usage conventions.
 *
 * r0-r3: Argument registers in both Dalvik and C/C++ conventions.
 *        However, for Dalvik->Dalvik calls we'll pass the target's Method*
 *        pointer in r0 as a hidden arg0. Otherwise used as codegen scratch
 *        registers.
 * r0-r1: As in C/C++ r0 is 32-bit return register and r0/r1 is 64-bit
 * r4   : If ARM_R4_SUSPEND_FLAG is set then reserved as a suspend check/debugger
 *        assist flag, otherwise a callee save promotion target.
 * r5   : Callee save (promotion target)
 * r6   : Callee save (promotion target)
 * r7   : Callee save (promotion target)
 * r8   : Callee save (promotion target)
 * r9   : (rARM_SELF) is reserved (pointer to thread-local storage)
 * r10  : Callee save (promotion target)
 * r11  : Callee save (promotion target)
 * r12  : Scratch, may be trashed by linkage stubs
 * r13  : (sp) is reserved
 * r14  : (lr) is reserved
 * r15  : (pc) is reserved
 *
 * 5 core temps that codegen can use (r0, r1, r2, r3, r12)
 * 7 core registers that can be used for promotion
 *
 * Floating pointer registers
 * s0-s31
 * d0-d15, where d0={s0,s1}, d1={s2,s3}, ... , d15={s30,s31}
 *
 * s16-s31 (d8-d15) preserved across C calls
 * s0-s15 (d0-d7) trashed across C calls
 *
 * s0-s15/d0-d7 used as codegen temp/scratch
 * s16-s31/d8-d31 can be used for promotion.
 *
 * Calling convention
 *     o On a call to a Dalvik method, pass target's Method* in r0
 *     o r1-r3 will be used for up to the first 3 words of arguments
 *     o Arguments past the first 3 words will be placed in appropriate
 *       out slots by the caller.
 *     o If a 64-bit argument would span the register/memory argument
 *       boundary, it will instead be fully passed in the frame.
 *     o Maintain a 16-byte stack alignment
 *
 *  Stack frame diagram (stack grows down, higher addresses at top):
 *
 * +------------------------+
 * | IN[ins-1]              |  {Note: resides in caller's frame}
 * |       .                |
 * | IN[0]                  |
 * | caller's Method*       |
 * +========================+  {Note: start of callee's frame}
 * | spill region           |  {variable sized - will include lr if non-leaf.}
 * +------------------------+
 * | ...filler word...      |  {Note: used as 2nd word of V[locals-1] if long]
 * +------------------------+
 * | V[locals-1]            |
 * | V[locals-2]            |
 * |      .                 |
 * |      .                 |
 * | V[1]                   |
 * | V[0]                   |
 * +------------------------+
 * |  0 to 3 words padding  |
 * +------------------------+
 * | OUT[outs-1]            |
 * | OUT[outs-2]            |
 * |       .                |
 * | OUT[0]                 |
 * | cur_method*            | <<== sp w/ 16-byte alignment
 * +========================+
 */

// First FP callee save.
#define ARM_FP_CALLEE_SAVE_BASE 16
// Flag for using R4 to do suspend check
#define ARM_R4_SUSPEND_FLAG

enum ArmResourceEncodingPos {
  kArmGPReg0   = 0,
  kArmRegSP    = 13,
  kArmRegLR    = 14,
  kArmRegPC    = 15,
  kArmFPReg0   = 16,
  kArmFPReg16  = 32,
  kArmRegEnd   = 48,
};

enum ArmNativeRegisterPool {
  r0           = RegStorage::k32BitSolo | RegStorage::kCoreRegister |  0,
  r1           = RegStorage::k32BitSolo | RegStorage::kCoreRegister |  1,
  r2           = RegStorage::k32BitSolo | RegStorage::kCoreRegister |  2,
  r3           = RegStorage::k32BitSolo | RegStorage::kCoreRegister |  3,
#ifdef ARM_R4_SUSPEND_FLAG
  rARM_SUSPEND = RegStorage::k32BitSolo | RegStorage::kCoreRegister |  4,
#else
  r4           = RegStorage::k32BitSolo | RegStorage::kCoreRegister |  4,
#endif
  r5           = RegStorage::k32BitSolo | RegStorage::kCoreRegister |  5,
  r6           = RegStorage::k32BitSolo | RegStorage::kCoreRegister |  6,
  r7           = RegStorage::k32BitSolo | RegStorage::kCoreRegister |  7,
  r8           = RegStorage::k32BitSolo | RegStorage::kCoreRegister |  8,
  rARM_SELF    = RegStorage::k32BitSolo | RegStorage::kCoreRegister |  9,
  r10          = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 10,
  r11          = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 11,
  r12          = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 12,
  r13sp        = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 13,
  rARM_SP      = r13sp,
  r14lr        = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 14,
  rARM_LR      = r14lr,
  r15pc        = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 15,
  rARM_PC      = r15pc,

  fr0          = RegStorage::k32BitSolo | RegStorage::kFloatingPoint |  0,
  fr1          = RegStorage::k32BitSolo | RegStorage::kFloatingPoint |  1,
  fr2          = RegStorage::k32BitSolo | RegStorage::kFloatingPoint |  2,
  fr3          = RegStorage::k32BitSolo | RegStorage::kFloatingPoint |  3,
  fr4          = RegStorage::k32BitSolo | RegStorage::kFloatingPoint |  4,
  fr5          = RegStorage::k32BitSolo | RegStorage::kFloatingPoint |  5,
  fr6          = RegStorage::k32BitSolo | RegStorage::kFloatingPoint |  6,
  fr7          = RegStorage::k32BitSolo | RegStorage::kFloatingPoint |  7,
  fr8          = RegStorage::k32BitSolo | RegStorage::kFloatingPoint |  8,
  fr9          = RegStorage::k32BitSolo | RegStorage::kFloatingPoint |  9,
  fr10         = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 10,
  fr11         = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 11,
  fr12         = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 12,
  fr13         = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 13,
  fr14         = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 14,
  fr15         = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 15,
  fr16         = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 16,
  fr17         = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 17,
  fr18         = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 18,
  fr19         = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 19,
  fr20         = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 20,
  fr21         = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 21,
  fr22         = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 22,
  fr23         = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 23,
  fr24         = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 24,
  fr25         = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 25,
  fr26         = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 26,
  fr27         = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 27,
  fr28         = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 28,
  fr29         = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 29,
  fr30         = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 30,
  fr31         = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 31,

  dr0          = RegStorage::k64BitSolo | RegStorage::kFloatingPoint |  0,
  dr1          = RegStorage::k64BitSolo | RegStorage::kFloatingPoint |  1,
  dr2          = RegStorage::k64BitSolo | RegStorage::kFloatingPoint |  2,
  dr3          = RegStorage::k64BitSolo | RegStorage::kFloatingPoint |  3,
  dr4          = RegStorage::k64BitSolo | RegStorage::kFloatingPoint |  4,
  dr5          = RegStorage::k64BitSolo | RegStorage::kFloatingPoint |  5,
  dr6          = RegStorage::k64BitSolo | RegStorage::kFloatingPoint |  6,
  dr7          = RegStorage::k64BitSolo | RegStorage::kFloatingPoint |  7,
  dr8          = RegStorage::k64BitSolo | RegStorage::kFloatingPoint |  8,
  dr9          = RegStorage::k64BitSolo | RegStorage::kFloatingPoint |  9,
  dr10         = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 10,
  dr11         = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 11,
  dr12         = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 12,
  dr13         = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 13,
  dr14         = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 14,
  dr15         = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 15,
#if 0
  // Enable when def/use and runtime able to handle these.
  dr16         = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 16,
  dr17         = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 17,
  dr18         = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 18,
  dr19         = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 19,
  dr20         = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 20,
  dr21         = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 21,
  dr22         = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 22,
  dr23         = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 23,
  dr24         = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 24,
  dr25         = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 25,
  dr26         = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 26,
  dr27         = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 27,
  dr28         = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 28,
  dr29         = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 29,
  dr30         = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 30,
  dr31         = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 31,
#endif
};

constexpr RegStorage rs_r0(RegStorage::kValid | r0);
constexpr RegStorage rs_r1(RegStorage::kValid | r1);
constexpr RegStorage rs_r2(RegStorage::kValid | r2);
constexpr RegStorage rs_r3(RegStorage::kValid | r3);
#ifdef ARM_R4_SUSPEND_FLAG
constexpr RegStorage rs_rARM_SUSPEND(RegStorage::kValid | rARM_SUSPEND);
#else
constexpr RegStorage rs_r4(RegStorage::kValid | r4);
#endif
constexpr RegStorage rs_r5(RegStorage::kValid | r5);
constexpr RegStorage rs_r6(RegStorage::kValid | r6);
constexpr RegStorage rs_r7(RegStorage::kValid | r7);
constexpr RegStorage rs_r8(RegStorage::kValid | r8);
constexpr RegStorage rs_rARM_SELF(RegStorage::kValid | rARM_SELF);
constexpr RegStorage rs_r10(RegStorage::kValid | r10);
constexpr RegStorage rs_r11(RegStorage::kValid | r11);
constexpr RegStorage rs_r12(RegStorage::kValid | r12);
constexpr RegStorage rs_r13sp(RegStorage::kValid | r13sp);
constexpr RegStorage rs_rARM_SP(RegStorage::kValid | rARM_SP);
constexpr RegStorage rs_r14lr(RegStorage::kValid | r14lr);
constexpr RegStorage rs_rARM_LR(RegStorage::kValid | rARM_LR);
constexpr RegStorage rs_r15pc(RegStorage::kValid | r15pc);
constexpr RegStorage rs_rARM_PC(RegStorage::kValid | rARM_PC);
constexpr RegStorage rs_invalid(RegStorage::kInvalid);

constexpr RegStorage rs_fr0(RegStorage::kValid | fr0);
constexpr RegStorage rs_fr1(RegStorage::kValid | fr1);
constexpr RegStorage rs_fr2(RegStorage::kValid | fr2);
constexpr RegStorage rs_fr3(RegStorage::kValid | fr3);
constexpr RegStorage rs_fr4(RegStorage::kValid | fr4);
constexpr RegStorage rs_fr5(RegStorage::kValid | fr5);
constexpr RegStorage rs_fr6(RegStorage::kValid | fr6);
constexpr RegStorage rs_fr7(RegStorage::kValid | fr7);
constexpr RegStorage rs_fr8(RegStorage::kValid | fr8);
constexpr RegStorage rs_fr9(RegStorage::kValid | fr9);
constexpr RegStorage rs_fr10(RegStorage::kValid | fr10);
constexpr RegStorage rs_fr11(RegStorage::kValid | fr11);
constexpr RegStorage rs_fr12(RegStorage::kValid | fr12);
constexpr RegStorage rs_fr13(RegStorage::kValid | fr13);
constexpr RegStorage rs_fr14(RegStorage::kValid | fr14);
constexpr RegStorage rs_fr15(RegStorage::kValid | fr15);
constexpr RegStorage rs_fr16(RegStorage::kValid | fr16);
constexpr RegStorage rs_fr17(RegStorage::kValid | fr17);
constexpr RegStorage rs_fr18(RegStorage::kValid | fr18);
constexpr RegStorage rs_fr19(RegStorage::kValid | fr19);
constexpr RegStorage rs_fr20(RegStorage::kValid | fr20);
constexpr RegStorage rs_fr21(RegStorage::kValid | fr21);
constexpr RegStorage rs_fr22(RegStorage::kValid | fr22);
constexpr RegStorage rs_fr23(RegStorage::kValid | fr23);
constexpr RegStorage rs_fr24(RegStorage::kValid | fr24);
constexpr RegStorage rs_fr25(RegStorage::kValid | fr25);
constexpr RegStorage rs_fr26(RegStorage::kValid | fr26);
constexpr RegStorage rs_fr27(RegStorage::kValid | fr27);
constexpr RegStorage rs_fr28(RegStorage::kValid | fr28);
constexpr RegStorage rs_fr29(RegStorage::kValid | fr29);
constexpr RegStorage rs_fr30(RegStorage::kValid | fr30);
constexpr RegStorage rs_fr31(RegStorage::kValid | fr31);

constexpr RegStorage rs_dr0(RegStorage::kValid | dr0);
constexpr RegStorage rs_dr1(RegStorage::kValid | dr1);
constexpr RegStorage rs_dr2(RegStorage::kValid | dr2);
constexpr RegStorage rs_dr3(RegStorage::kValid | dr3);
constexpr RegStorage rs_dr4(RegStorage::kValid | dr4);
constexpr RegStorage rs_dr5(RegStorage::kValid | dr5);
constexpr RegStorage rs_dr6(RegStorage::kValid | dr6);
constexpr RegStorage rs_dr7(RegStorage::kValid | dr7);
constexpr RegStorage rs_dr8(RegStorage::kValid | dr8);
constexpr RegStorage rs_dr9(RegStorage::kValid | dr9);
constexpr RegStorage rs_dr10(RegStorage::kValid | dr10);
constexpr RegStorage rs_dr11(RegStorage::kValid | dr11);
constexpr RegStorage rs_dr12(RegStorage::kValid | dr12);
constexpr RegStorage rs_dr13(RegStorage::kValid | dr13);
constexpr RegStorage rs_dr14(RegStorage::kValid | dr14);
constexpr RegStorage rs_dr15(RegStorage::kValid | dr15);
#if 0
constexpr RegStorage rs_dr16(RegStorage::kValid | dr16);
constexpr RegStorage rs_dr17(RegStorage::kValid | dr17);
constexpr RegStorage rs_dr18(RegStorage::kValid | dr18);
constexpr RegStorage rs_dr19(RegStorage::kValid | dr19);
constexpr RegStorage rs_dr20(RegStorage::kValid | dr20);
constexpr RegStorage rs_dr21(RegStorage::kValid | dr21);
constexpr RegStorage rs_dr22(RegStorage::kValid | dr22);
constexpr RegStorage rs_dr23(RegStorage::kValid | dr23);
constexpr RegStorage rs_dr24(RegStorage::kValid | dr24);
constexpr RegStorage rs_dr25(RegStorage::kValid | dr25);
constexpr RegStorage rs_dr26(RegStorage::kValid | dr26);
constexpr RegStorage rs_dr27(RegStorage::kValid | dr27);
constexpr RegStorage rs_dr28(RegStorage::kValid | dr28);
constexpr RegStorage rs_dr29(RegStorage::kValid | dr29);
constexpr RegStorage rs_dr30(RegStorage::kValid | dr30);
constexpr RegStorage rs_dr31(RegStorage::kValid | dr31);
#endif

// RegisterLocation templates return values (r0, or r0/r1).
const RegLocation arm_loc_c_return
    {kLocPhysReg, 0, 0, 0, 0, 0, 0, 0, 1,
     RegStorage(RegStorage::k32BitSolo, r0), INVALID_SREG, INVALID_SREG};
const RegLocation arm_loc_c_return_wide
    {kLocPhysReg, 1, 0, 0, 0, 0, 0, 0, 1,
     RegStorage(RegStorage::k64BitPair, r0, r1), INVALID_SREG, INVALID_SREG};
const RegLocation arm_loc_c_return_float
    {kLocPhysReg, 0, 0, 0, 0, 0, 0, 0, 1,
     RegStorage(RegStorage::k32BitSolo, r0), INVALID_SREG, INVALID_SREG};
const RegLocation arm_loc_c_return_double
    {kLocPhysReg, 1, 0, 0, 0, 0, 0, 0, 1,
     RegStorage(RegStorage::k64BitPair, r0, r1), INVALID_SREG, INVALID_SREG};

enum ArmShiftEncodings {
  kArmLsl = 0x0,
  kArmLsr = 0x1,
  kArmAsr = 0x2,
  kArmRor = 0x3
};

/*
 * The following enum defines the list of supported Thumb instructions by the
 * assembler. Their corresponding EncodingMap positions will be defined in
 * Assemble.cc.
 */
enum ArmOpcode {
  kArmFirst = 0,
  kArm16BitData = kArmFirst,  // DATA   [0] rd[15..0].
  kThumbAdcRR,       // adc   [0100000101] rm[5..3] rd[2..0].
  kThumbAddRRI3,     // add(1)  [0001110] imm_3[8..6] rn[5..3] rd[2..0].
  kThumbAddRI8,      // add(2)  [00110] rd[10..8] imm_8[7..0].
  kThumbAddRRR,      // add(3)  [0001100] rm[8..6] rn[5..3] rd[2..0].
  kThumbAddRRLH,     // add(4)  [01000100] H12[01] rm[5..3] rd[2..0].
  kThumbAddRRHL,     // add(4)  [01001000] H12[10] rm[5..3] rd[2..0].
  kThumbAddRRHH,     // add(4)  [01001100] H12[11] rm[5..3] rd[2..0].
  kThumbAddPcRel,    // add(5)  [10100] rd[10..8] imm_8[7..0].
  kThumbAddSpRel,    // add(6)  [10101] rd[10..8] imm_8[7..0].
  kThumbAddSpI7,     // add(7)  [101100000] imm_7[6..0].
  kThumbAndRR,       // and   [0100000000] rm[5..3] rd[2..0].
  kThumbAsrRRI5,     // asr(1)  [00010] imm_5[10..6] rm[5..3] rd[2..0].
  kThumbAsrRR,       // asr(2)  [0100000100] rs[5..3] rd[2..0].
  kThumbBCond,       // b(1)  [1101] cond[11..8] offset_8[7..0].
  kThumbBUncond,     // b(2)  [11100] offset_11[10..0].
  kThumbBicRR,       // bic   [0100001110] rm[5..3] rd[2..0].
  kThumbBkpt,        // bkpt  [10111110] imm_8[7..0].
  kThumbBlx1,        // blx(1)  [111] H[10] offset_11[10..0].
  kThumbBlx2,        // blx(1)  [111] H[01] offset_11[10..0].
  kThumbBl1,         // blx(1)  [111] H[10] offset_11[10..0].
  kThumbBl2,         // blx(1)  [111] H[11] offset_11[10..0].
  kThumbBlxR,        // blx(2)  [010001111] rm[6..3] [000].
  kThumbBx,          // bx    [010001110] H2[6..6] rm[5..3] SBZ[000].
  kThumbCmnRR,       // cmn   [0100001011] rm[5..3] rd[2..0].
  kThumbCmpRI8,      // cmp(1)  [00101] rn[10..8] imm_8[7..0].
  kThumbCmpRR,       // cmp(2)  [0100001010] rm[5..3] rd[2..0].
  kThumbCmpLH,       // cmp(3)  [01000101] H12[01] rm[5..3] rd[2..0].
  kThumbCmpHL,       // cmp(3)  [01000110] H12[10] rm[5..3] rd[2..0].
  kThumbCmpHH,       // cmp(3)  [01000111] H12[11] rm[5..3] rd[2..0].
  kThumbEorRR,       // eor   [0100000001] rm[5..3] rd[2..0].
  kThumbLdmia,       // ldmia   [11001] rn[10..8] reglist [7..0].
  kThumbLdrRRI5,     // ldr(1)  [01101] imm_5[10..6] rn[5..3] rd[2..0].
  kThumbLdrRRR,      // ldr(2)  [0101100] rm[8..6] rn[5..3] rd[2..0].
  kThumbLdrPcRel,    // ldr(3)  [01001] rd[10..8] imm_8[7..0].
  kThumbLdrSpRel,    // ldr(4)  [10011] rd[10..8] imm_8[7..0].
  kThumbLdrbRRI5,    // ldrb(1) [01111] imm_5[10..6] rn[5..3] rd[2..0].
  kThumbLdrbRRR,     // ldrb(2) [0101110] rm[8..6] rn[5..3] rd[2..0].
  kThumbLdrhRRI5,    // ldrh(1) [10001] imm_5[10..6] rn[5..3] rd[2..0].
  kThumbLdrhRRR,     // ldrh(2) [0101101] rm[8..6] rn[5..3] rd[2..0].
  kThumbLdrsbRRR,    // ldrsb   [0101011] rm[8..6] rn[5..3] rd[2..0].
  kThumbLdrshRRR,    // ldrsh   [0101111] rm[8..6] rn[5..3] rd[2..0].
  kThumbLslRRI5,     // lsl(1)  [00000] imm_5[10..6] rm[5..3] rd[2..0].
  kThumbLslRR,       // lsl(2)  [0100000010] rs[5..3] rd[2..0].
  kThumbLsrRRI5,     // lsr(1)  [00001] imm_5[10..6] rm[5..3] rd[2..0].
  kThumbLsrRR,       // lsr(2)  [0100000011] rs[5..3] rd[2..0].
  kThumbMovImm,      // mov(1)  [00100] rd[10..8] imm_8[7..0].
  kThumbMovRR,       // mov(2)  [0001110000] rn[5..3] rd[2..0].
  kThumbMovRR_H2H,   // mov(3)  [01000111] H12[11] rm[5..3] rd[2..0].
  kThumbMovRR_H2L,   // mov(3)  [01000110] H12[01] rm[5..3] rd[2..0].
  kThumbMovRR_L2H,   // mov(3)  [01000101] H12[10] rm[5..3] rd[2..0].
  kThumbMul,         // mul   [0100001101] rm[5..3] rd[2..0].
  kThumbMvn,         // mvn   [0100001111] rm[5..3] rd[2..0].
  kThumbNeg,         // neg   [0100001001] rm[5..3] rd[2..0].
  kThumbOrr,         // orr   [0100001100] rm[5..3] rd[2..0].
  kThumbPop,         // pop   [1011110] r[8..8] rl[7..0].
  kThumbPush,        // push  [1011010] r[8..8] rl[7..0].
  kThumbRev,         // rev   [1011101000] rm[5..3] rd[2..0]
  kThumbRevsh,       // revsh   [1011101011] rm[5..3] rd[2..0]
  kThumbRorRR,       // ror   [0100000111] rs[5..3] rd[2..0].
  kThumbSbc,         // sbc   [0100000110] rm[5..3] rd[2..0].
  kThumbStmia,       // stmia   [11000] rn[10..8] reglist [7.. 0].
  kThumbStrRRI5,     // str(1)  [01100] imm_5[10..6] rn[5..3] rd[2..0].
  kThumbStrRRR,      // str(2)  [0101000] rm[8..6] rn[5..3] rd[2..0].
  kThumbStrSpRel,    // str(3)  [10010] rd[10..8] imm_8[7..0].
  kThumbStrbRRI5,    // strb(1) [01110] imm_5[10..6] rn[5..3] rd[2..0].
  kThumbStrbRRR,     // strb(2) [0101010] rm[8..6] rn[5..3] rd[2..0].
  kThumbStrhRRI5,    // strh(1) [10000] imm_5[10..6] rn[5..3] rd[2..0].
  kThumbStrhRRR,     // strh(2) [0101001] rm[8..6] rn[5..3] rd[2..0].
  kThumbSubRRI3,     // sub(1)  [0001111] imm_3[8..6] rn[5..3] rd[2..0]*/
  kThumbSubRI8,      // sub(2)  [00111] rd[10..8] imm_8[7..0].
  kThumbSubRRR,      // sub(3)  [0001101] rm[8..6] rn[5..3] rd[2..0].
  kThumbSubSpI7,     // sub(4)  [101100001] imm_7[6..0].
  kThumbSwi,         // swi   [11011111] imm_8[7..0].
  kThumbTst,         // tst   [0100001000] rm[5..3] rn[2..0].
  kThumb2Vldrs,      // vldr low  sx [111011011001] rn[19..16] rd[15-12] [1010] imm_8[7..0].
  kThumb2Vldrd,      // vldr low  dx [111011011001] rn[19..16] rd[15-12] [1011] imm_8[7..0].
  kThumb2Vmuls,      // vmul vd, vn, vm [111011100010] rn[19..16] rd[15-12] [10100000] rm[3..0].
  kThumb2Vmuld,      // vmul vd, vn, vm [111011100010] rn[19..16] rd[15-12] [10110000] rm[3..0].
  kThumb2Vstrs,      // vstr low  sx [111011011000] rn[19..16] rd[15-12] [1010] imm_8[7..0].
  kThumb2Vstrd,      // vstr low  dx [111011011000] rn[19..16] rd[15-12] [1011] imm_8[7..0].
  kThumb2Vsubs,      // vsub vd, vn, vm [111011100011] rn[19..16] rd[15-12] [10100040] rm[3..0].
  kThumb2Vsubd,      // vsub vd, vn, vm [111011100011] rn[19..16] rd[15-12] [10110040] rm[3..0].
  kThumb2Vadds,      // vadd vd, vn, vm [111011100011] rn[19..16] rd[15-12] [10100000] rm[3..0].
  kThumb2Vaddd,      // vadd vd, vn, vm [111011100011] rn[19..16] rd[15-12] [10110000] rm[3..0].
  kThumb2Vdivs,      // vdiv vd, vn, vm [111011101000] rn[19..16] rd[15-12] [10100000] rm[3..0].
  kThumb2Vdivd,      // vdiv vd, vn, vm [111011101000] rn[19..16] rd[15-12] [10110000] rm[3..0].
  kThumb2VmlaF64,    // vmla.F64 vd, vn, vm [111011100000] vn[19..16] vd[15..12] [10110000] vm[3..0].
  kThumb2VcvtIF,     // vcvt.F32.S32 vd, vm [1110111010111000] vd[15..12] [10101100] vm[3..0].
  kThumb2VcvtFI,     // vcvt.S32.F32 vd, vm [1110111010111101] vd[15..12] [10101100] vm[3..0].
  kThumb2VcvtDI,     // vcvt.S32.F32 vd, vm [1110111010111101] vd[15..12] [10111100] vm[3..0].
  kThumb2VcvtFd,     // vcvt.F64.F32 vd, vm [1110111010110111] vd[15..12] [10101100] vm[3..0].
  kThumb2VcvtDF,     // vcvt.F32.F64 vd, vm [1110111010110111] vd[15..12] [10111100] vm[3..0].
  kThumb2VcvtF64S32,  // vcvt.F64.S32 vd, vm [1110111010111000] vd[15..12] [10111100] vm[3..0].
  kThumb2VcvtF64U32,  // vcvt.F64.U32 vd, vm [1110111010111000] vd[15..12] [10110100] vm[3..0].
  kThumb2Vsqrts,     // vsqrt.f32 vd, vm [1110111010110001] vd[15..12] [10101100] vm[3..0].
  kThumb2Vsqrtd,     // vsqrt.f64 vd, vm [1110111010110001] vd[15..12] [10111100] vm[3..0].
  kThumb2MovI8M,     // mov(T2) rd, #<const> [11110] i [00001001111] imm3 rd[11..8] imm8.
  kThumb2MovImm16,   // mov(T3) rd, #<const> [11110] i [0010100] imm4 [0] imm3 rd[11..8] imm8.
  kThumb2StrRRI12,   // str(Imm,T3) rd,[rn,#imm12] [111110001100] rn[19..16] rt[15..12] imm12[11..0].
  kThumb2LdrRRI12,   // str(Imm,T3) rd,[rn,#imm12] [111110001100] rn[19..16] rt[15..12] imm12[11..0].
  kThumb2StrRRI8Predec,  // str(Imm,T4) rd,[rn,#-imm8] [111110000100] rn[19..16] rt[15..12] [1100] imm[7..0].
  kThumb2LdrRRI8Predec,  // ldr(Imm,T4) rd,[rn,#-imm8] [111110000101] rn[19..16] rt[15..12] [1100] imm[7..0].
  kThumb2Cbnz,       // cbnz rd,<label> [101110] i [1] imm5[7..3] rn[2..0].
  kThumb2Cbz,        // cbn rd,<label> [101100] i [1] imm5[7..3] rn[2..0].
  kThumb2AddRRI12,   // add rd, rn, #imm12 [11110] i [100000] rn[19..16] [0] imm3[14..12] rd[11..8] imm8[7..0].
  kThumb2MovRR,      // mov rd, rm [11101010010011110000] rd[11..8] [0000] rm[3..0].
  kThumb2Vmovs,      // vmov.f32 vd, vm [111011101] D [110000] vd[15..12] 101001] M [0] vm[3..0].
  kThumb2Vmovd,      // vmov.f64 vd, vm [111011101] D [110000] vd[15..12] 101101] M [0] vm[3..0].
  kThumb2Ldmia,      // ldmia  [111010001001] rn[19..16] mask[15..0].
  kThumb2Stmia,      // stmia  [111010001000] rn[19..16] mask[15..0].
  kThumb2AddRRR,     // add [111010110000] rn[19..16] [0000] rd[11..8] [0000] rm[3..0].
  kThumb2SubRRR,     // sub [111010111010] rn[19..16] [0000] rd[11..8] [0000] rm[3..0].
  kThumb2SbcRRR,     // sbc [111010110110] rn[19..16] [0000] rd[11..8] [0000] rm[3..0].
  kThumb2CmpRR,      // cmp [111010111011] rn[19..16] [0000] [1111] [0000] rm[3..0].
  kThumb2SubRRI12,   // sub rd, rn, #imm12 [11110] i [101010] rn[19..16] [0] imm3[14..12] rd[11..8] imm8[7..0].
  kThumb2MvnI8M,     // mov(T2) rd, #<const> [11110] i [00011011110] imm3 rd[11..8] imm8.
  kThumb2Sel,        // sel rd, rn, rm [111110101010] rn[19-16] rd[11-8] rm[3-0].
  kThumb2Ubfx,       // ubfx rd,rn,#lsb,#width [111100111100] rn[19..16] [0] imm3[14-12] rd[11-8] w[4-0].
  kThumb2Sbfx,       // ubfx rd,rn,#lsb,#width [111100110100] rn[19..16] [0] imm3[14-12] rd[11-8] w[4-0].
  kThumb2LdrRRR,     // ldr rt,[rn,rm,LSL #imm] [111110000101] rn[19-16] rt[15-12] [000000] imm[5-4] rm[3-0].
  kThumb2LdrhRRR,    // ldrh rt,[rn,rm,LSL #imm] [111110000101] rn[19-16] rt[15-12] [000000] imm[5-4] rm[3-0].
  kThumb2LdrshRRR,   // ldrsh rt,[rn,rm,LSL #imm] [111110000101] rn[19-16] rt[15-12] [000000] imm[5-4] rm[3-0].
  kThumb2LdrbRRR,    // ldrb rt,[rn,rm,LSL #imm] [111110000101] rn[19-16] rt[15-12] [000000] imm[5-4] rm[3-0].
  kThumb2LdrsbRRR,   // ldrsb rt,[rn,rm,LSL #imm] [111110000101] rn[19-16] rt[15-12] [000000] imm[5-4] rm[3-0].
  kThumb2StrRRR,     // str rt,[rn,rm,LSL #imm] [111110000100] rn[19-16] rt[15-12] [000000] imm[5-4] rm[3-0].
  kThumb2StrhRRR,    // str rt,[rn,rm,LSL #imm] [111110000010] rn[19-16] rt[15-12] [000000] imm[5-4] rm[3-0].
  kThumb2StrbRRR,    // str rt,[rn,rm,LSL #imm] [111110000000] rn[19-16] rt[15-12] [000000] imm[5-4] rm[3-0].
  kThumb2LdrhRRI12,  // ldrh rt,[rn,#imm12] [111110001011] rt[15..12] rn[19..16] imm12[11..0].
  kThumb2LdrshRRI12,  // ldrsh rt,[rn,#imm12] [111110011011] rt[15..12] rn[19..16] imm12[11..0].
  kThumb2LdrbRRI12,  // ldrb rt,[rn,#imm12] [111110001001] rt[15..12] rn[19..16] imm12[11..0].
  kThumb2LdrsbRRI12,  // ldrsb rt,[rn,#imm12] [111110011001] rt[15..12] rn[19..16] imm12[11..0].
  kThumb2StrhRRI12,  // strh rt,[rn,#imm12] [111110001010] rt[15..12] rn[19..16] imm12[11..0].
  kThumb2StrbRRI12,  // strb rt,[rn,#imm12] [111110001000] rt[15..12] rn[19..16] imm12[11..0].
  kThumb2Pop,        // pop   [1110100010111101] list[15-0]*/
  kThumb2Push,       // push  [1110100100101101] list[15-0]*/
  kThumb2CmpRI8M,    // cmp rn, #<const> [11110] i [011011] rn[19-16] [0] imm3 [1111] imm8[7..0].
  kThumb2CmnRI8M,    // cmn rn, #<const> [11110] i [010001] rn[19-16] [0] imm3 [1111] imm8[7..0].
  kThumb2AdcRRR,     // adc [111010110101] rn[19..16] [0000] rd[11..8] [0000] rm[3..0].
  kThumb2AndRRR,     // and [111010100000] rn[19..16] [0000] rd[11..8] [0000] rm[3..0].
  kThumb2BicRRR,     // bic [111010100010] rn[19..16] [0000] rd[11..8] [0000] rm[3..0].
  kThumb2CmnRR,      // cmn [111010110001] rn[19..16] [0000] [1111] [0000] rm[3..0].
  kThumb2EorRRR,     // eor [111010101000] rn[19..16] [0000] rd[11..8] [0000] rm[3..0].
  kThumb2MulRRR,     // mul [111110110000] rn[19..16] [1111] rd[11..8] [0000] rm[3..0].
  kThumb2SdivRRR,    // sdiv [111110111001] rn[19..16] [1111] rd[11..8] [1111] rm[3..0].
  kThumb2UdivRRR,    // udiv [111110111011] rn[19..16] [1111] rd[11..8] [1111] rm[3..0].
  kThumb2MnvRR,      // mvn [11101010011011110] rd[11-8] [0000] rm[3..0].
  kThumb2RsubRRI8M,  // rsb rd, rn, #<const> [11110] i [011101] rn[19..16] [0] imm3[14..12] rd[11..8] imm8[7..0].
  kThumb2NegRR,      // actually rsub rd, rn, #0.
  kThumb2OrrRRR,     // orr [111010100100] rn[19..16] [0000] rd[11..8] [0000] rm[3..0].
  kThumb2TstRR,      // tst [111010100001] rn[19..16] [0000] [1111] [0000] rm[3..0].
  kThumb2LslRRR,     // lsl [111110100000] rn[19..16] [1111] rd[11..8] [0000] rm[3..0].
  kThumb2LsrRRR,     // lsr [111110100010] rn[19..16] [1111] rd[11..8] [0000] rm[3..0].
  kThumb2AsrRRR,     // asr [111110100100] rn[19..16] [1111] rd[11..8] [0000] rm[3..0].
  kThumb2RorRRR,     // ror [111110100110] rn[19..16] [1111] rd[11..8] [0000] rm[3..0].
  kThumb2LslRRI5,    // lsl [11101010010011110] imm[14.12] rd[11..8] [00] rm[3..0].
  kThumb2LsrRRI5,    // lsr [11101010010011110] imm[14.12] rd[11..8] [01] rm[3..0].
  kThumb2AsrRRI5,    // asr [11101010010011110] imm[14.12] rd[11..8] [10] rm[3..0].
  kThumb2RorRRI5,    // ror [11101010010011110] imm[14.12] rd[11..8] [11] rm[3..0].
  kThumb2BicRRI8M,   // bic rd, rn, #<const> [11110] i [000010] rn[19..16] [0] imm3[14..12] rd[11..8] imm8[7..0].
  kThumb2AndRRI8M,   // and rd, rn, #<const> [11110] i [000000] rn[19..16] [0] imm3[14..12] rd[11..8] imm8[7..0].
  kThumb2OrrRRI8M,   // orr rd, rn, #<const> [11110] i [000100] rn[19..16] [0] imm3[14..12] rd[11..8] imm8[7..0].
  kThumb2EorRRI8M,   // eor rd, rn, #<const> [11110] i [001000] rn[19..16] [0] imm3[14..12] rd[11..8] imm8[7..0].
  kThumb2AddRRI8M,   // add rd, rn, #<const> [11110] i [010001] rn[19..16] [0] imm3[14..12] rd[11..8] imm8[7..0].
  kThumb2AdcRRI8M,   // adc rd, rn, #<const> [11110] i [010101] rn[19..16] [0] imm3[14..12] rd[11..8] imm8[7..0].
  kThumb2SubRRI8M,   // sub rd, rn, #<const> [11110] i [011011] rn[19..16] [0] imm3[14..12] rd[11..8] imm8[7..0].
  kThumb2SbcRRI8M,   // sub rd, rn, #<const> [11110] i [010111] rn[19..16] [0] imm3[14..12] rd[11..8] imm8[7..0].
  kThumb2RevRR,      // rev [111110101001] rm[19..16] [1111] rd[11..8] 1000 rm[3..0]
  kThumb2RevshRR,    // rev [111110101001] rm[19..16] [1111] rd[11..8] 1011 rm[3..0]
  kThumb2It,         // it [10111111] firstcond[7-4] mask[3-0].
  kThumb2Fmstat,     // fmstat [11101110111100011111101000010000].
  kThumb2Vcmpd,      // vcmp [111011101] D [11011] rd[15-12] [1011] E [1] M [0] rm[3-0].
  kThumb2Vcmps,      // vcmp [111011101] D [11010] rd[15-12] [1011] E [1] M [0] rm[3-0].
  kThumb2LdrPcRel12,  // ldr rd,[pc,#imm12] [1111100011011111] rt[15-12] imm12[11-0].
  kThumb2BCond,      // b<c> [1110] S cond[25-22] imm6[21-16] [10] J1 [0] J2 imm11[10..0].
  kThumb2Fmrs,       // vmov [111011100000] vn[19-16] rt[15-12] [1010] N [0010000].
  kThumb2Fmsr,       // vmov [111011100001] vn[19-16] rt[15-12] [1010] N [0010000].
  kThumb2Fmrrd,      // vmov [111011000100] rt2[19-16] rt[15-12] [101100] M [1] vm[3-0].
  kThumb2Fmdrr,      // vmov [111011000101] rt2[19-16] rt[15-12] [101100] M [1] vm[3-0].
  kThumb2Vabsd,      // vabs.f64 [111011101] D [110000] rd[15-12] [1011110] M [0] vm[3-0].
  kThumb2Vabss,      // vabs.f32 [111011101] D [110000] rd[15-12] [1010110] M [0] vm[3-0].
  kThumb2Vnegd,      // vneg.f64 [111011101] D [110000] rd[15-12] [1011110] M [0] vm[3-0].
  kThumb2Vnegs,      // vneg.f32 [111011101] D [110000] rd[15-12] [1010110] M [0] vm[3-0].
  kThumb2Vmovs_IMM8,  // vmov.f32 [111011101] D [11] imm4h[19-16] vd[15-12] [10100000] imm4l[3-0].
  kThumb2Vmovd_IMM8,  // vmov.f64 [111011101] D [11] imm4h[19-16] vd[15-12] [10110000] imm4l[3-0].
  kThumb2Mla,        // mla [111110110000] rn[19-16] ra[15-12] rd[7-4] [0000] rm[3-0].
  kThumb2Umull,      // umull [111110111010] rn[19-16], rdlo[15-12] rdhi[11-8] [0000] rm[3-0].
  kThumb2Ldrex,      // ldrex [111010000101] rn[19-16] rt[15-12] [1111] imm8[7-0].
  kThumb2Ldrexd,     // ldrexd [111010001101] rn[19-16] rt[15-12] rt2[11-8] [11111111].
  kThumb2Strex,      // strex [111010000100] rn[19-16] rt[15-12] rd[11-8] imm8[7-0].
  kThumb2Strexd,     // strexd [111010001100] rn[19-16] rt[15-12] rt2[11-8] [0111] Rd[3-0].
  kThumb2Clrex,      // clrex [11110011101111111000111100101111].
  kThumb2Bfi,        // bfi [111100110110] rn[19-16] [0] imm3[14-12] rd[11-8] imm2[7-6] [0] msb[4-0].
  kThumb2Bfc,        // bfc [11110011011011110] [0] imm3[14-12] rd[11-8] imm2[7-6] [0] msb[4-0].
  kThumb2Dmb,        // dmb [1111001110111111100011110101] option[3-0].
  kThumb2LdrPcReln12,  // ldr rd,[pc,-#imm12] [1111100011011111] rt[15-12] imm12[11-0].
  kThumb2Stm,        // stm <list> [111010010000] rn[19-16] 000 rl[12-0].
  kThumbUndefined,   // undefined [11011110xxxxxxxx].
  kThumb2VPopCS,     // vpop <list of callee save fp singles (s16+).
  kThumb2VPushCS,    // vpush <list callee save fp singles (s16+).
  kThumb2Vldms,      // vldms rd, <list>.
  kThumb2Vstms,      // vstms rd, <list>.
  kThumb2BUncond,    // b <label>.
  kThumb2MovImm16H,  // similar to kThumb2MovImm16, but target high hw.
  kThumb2AddPCR,     // Thumb2 2-operand add with hard-coded PC target.
  kThumb2Adr,        // Special purpose encoding of ADR for switch tables.
  kThumb2MovImm16LST,  // Special purpose version for switch table use.
  kThumb2MovImm16HST,  // Special purpose version for switch table use.
  kThumb2LdmiaWB,    // ldmia  [111010011001[ rn[19..16] mask[15..0].
  kThumb2OrrRRRs,    // orrs [111010100101] rn[19..16] [0000] rd[11..8] [0000] rm[3..0].
  kThumb2Push1,      // t3 encoding of push.
  kThumb2Pop1,       // t3 encoding of pop.
  kThumb2RsubRRR,    // rsb [111010111101] rn[19..16] [0000] rd[11..8] [0000] rm[3..0].
  kThumb2Smull,      // smull [111110111000] rn[19-16], rdlo[15-12] rdhi[11-8] [0000] rm[3-0].
  kThumb2LdrdPcRel8,  // ldrd rt, rt2, pc +-/1024.
  kThumb2LdrdI8,     // ldrd rt, rt2, [rn +-/1024].
  kThumb2StrdI8,     // strd rt, rt2, [rn +-/1024].
  kArmLast,
};

enum ArmOpDmbOptions {
  kSY = 0xf,
  kST = 0xe,
  kISH = 0xb,
  kISHST = 0xa,
  kNSH = 0x7,
  kNSHST = 0x6
};

// Instruction assembly field_loc kind.
enum ArmEncodingKind {
  kFmtUnused,    // Unused field and marks end of formats.
  kFmtBitBlt,    // Bit string using end/start.
  kFmtDfp,       // Double FP reg.
  kFmtSfp,       // Single FP reg.
  kFmtModImm,    // Shifted 8-bit immed using [26,14..12,7..0].
  kFmtImm16,     // Zero-extended immed using [26,19..16,14..12,7..0].
  kFmtImm6,      // Encoded branch target using [9,7..3]0.
  kFmtImm12,     // Zero-extended immediate using [26,14..12,7..0].
  kFmtShift,     // Shift descriptor, [14..12,7..4].
  kFmtLsb,       // least significant bit using [14..12][7..6].
  kFmtBWidth,    // bit-field width, encoded as width-1.
  kFmtShift5,    // Shift count, [14..12,7..6].
  kFmtBrOffset,  // Signed extended [26,11,13,21-16,10-0]:0.
  kFmtFPImm,     // Encoded floating point immediate.
  kFmtOff24,     // 24-bit Thumb2 unconditional branch encoding.
  kFmtSkip,      // Unused field, but continue to next.
};

// Struct used to define the snippet positions for each Thumb opcode.
struct ArmEncodingMap {
  uint32_t skeleton;
  struct {
    ArmEncodingKind kind;
    int end;   // end for kFmtBitBlt, 1-bit slice end for FP regs.
    int start;  // start for kFmtBitBlt, 4-bit slice end for FP regs.
  } field_loc[4];
  ArmOpcode opcode;
  uint64_t flags;
  const char* name;
  const char* fmt;
  int size;   // Note: size is in bytes.
  FixupKind fixup;
};

}  // namespace art

#endif  // ART_COMPILER_DEX_QUICK_ARM_ARM_LIR_H_
