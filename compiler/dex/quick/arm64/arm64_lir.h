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

#ifndef ART_COMPILER_DEX_QUICK_ARM64_ARM64_LIR_H_
#define ART_COMPILER_DEX_QUICK_ARM64_ARM64_LIR_H_

#include "dex/compiler_internals.h"

namespace art {

/*
 * TODO(Arm64): the comments below are outdated.
 *
 * Runtime register usage conventions.
 *
 * r0-r3: Argument registers in both Dalvik and C/C++ conventions.
 *        However, for Dalvik->Dalvik calls we'll pass the target's Method*
 *        pointer in r0 as a hidden arg0. Otherwise used as codegen scratch
 *        registers.
 * r0-r1: As in C/C++ r0 is 32-bit return register and r0/r1 is 64-bit
 * r4   : (rA64_SUSPEND) is reserved (suspend check/debugger assist)
 * r5   : Callee save (promotion target)
 * r6   : Callee save (promotion target)
 * r7   : Callee save (promotion target)
 * r8   : Callee save (promotion target)
 * r9   : (rA64_SELF) is reserved (pointer to thread-local storage)
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
#define A64_FP_CALLEE_SAVE_BASE 8

// Temporary macros, used to mark code which wants to distinguish betweek zr/sp.
#define A64_REG_IS_SP(reg_num) ((reg_num) == rwsp || (reg_num) == rsp)
#define A64_REG_IS_ZR(reg_num) ((reg_num) == rwzr || (reg_num) == rxzr)
#define A64_REGSTORAGE_IS_SP_OR_ZR(rs) (((rs).GetRegNum() & 0x1f) == 0x1f)

enum Arm64ResourceEncodingPos {
  kArm64GPReg0   = 0,
  kArm64RegLR    = 30,
  kArm64RegSP    = 31,
  kArm64FPReg0   = 32,
  kArm64RegEnd   = 64,
};

#define IS_SIGNED_IMM(size, value) \
  ((value) >= -(1 << ((size) - 1)) && (value) < (1 << ((size) - 1)))
#define IS_SIGNED_IMM7(value) IS_SIGNED_IMM(7, value)
#define IS_SIGNED_IMM9(value) IS_SIGNED_IMM(9, value)
#define IS_SIGNED_IMM12(value) IS_SIGNED_IMM(12, value)
#define IS_SIGNED_IMM19(value) IS_SIGNED_IMM(19, value)
#define IS_SIGNED_IMM21(value) IS_SIGNED_IMM(21, value)

// Quick macro used to define the registers.
#define A64_REGISTER_CODE_LIST(R) \
  R(0)  R(1)  R(2)  R(3)  R(4)  R(5)  R(6)  R(7) \
  R(8)  R(9)  R(10) R(11) R(12) R(13) R(14) R(15) \
  R(16) R(17) R(18) R(19) R(20) R(21) R(22) R(23) \
  R(24) R(25) R(26) R(27) R(28) R(29) R(30) R(31)

// Registers (integer) values.
enum A64NativeRegisterPool {
#  define A64_DEFINE_REGISTERS(nr) \
    rw##nr = RegStorage::k32BitSolo | RegStorage::kCoreRegister | nr, \
    rx##nr = RegStorage::k64BitSolo | RegStorage::kCoreRegister | nr, \
    rf##nr = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | nr, \
    rd##nr = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | nr,
  A64_REGISTER_CODE_LIST(A64_DEFINE_REGISTERS)
#undef A64_DEFINE_REGISTERS

  rxzr = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 0x3f,
  rwzr = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 0x3f,
  rsp = rx31,
  rwsp = rw31,

  // Aliases which are not defined in "ARM Architecture Reference, register names".
  rxIP0 = rx16,
  rxIP1 = rx17,
  rxSUSPEND = rx19,
  rxSELF = rx18,
  rxLR = rx30,
  /*
   * FIXME: It's a bit awkward to define both 32 and 64-bit views of these - we'll only ever use
   * the 64-bit view. However, for now we'll define a 32-bit view to keep these from being
   * allocated as 32-bit temp registers.
   */
  rwIP0 = rw16,
  rwIP1 = rw17,
  rwSUSPEND = rw19,
  rwSELF = rw18,
  rwLR = rw30,
};

#define A64_DEFINE_REGSTORAGES(nr) \
  constexpr RegStorage rs_w##nr(RegStorage::kValid | rw##nr); \
  constexpr RegStorage rs_x##nr(RegStorage::kValid | rx##nr); \
  constexpr RegStorage rs_f##nr(RegStorage::kValid | rf##nr); \
  constexpr RegStorage rs_d##nr(RegStorage::kValid | rd##nr);
A64_REGISTER_CODE_LIST(A64_DEFINE_REGSTORAGES)
#undef A64_DEFINE_REGSTORAGES

constexpr RegStorage rs_xzr(RegStorage::kValid | rxzr);
constexpr RegStorage rs_wzr(RegStorage::kValid | rwzr);
constexpr RegStorage rs_xIP0(RegStorage::kValid | rxIP0);
constexpr RegStorage rs_wIP0(RegStorage::kValid | rwIP0);
constexpr RegStorage rs_xIP1(RegStorage::kValid | rxIP1);
constexpr RegStorage rs_wIP1(RegStorage::kValid | rwIP1);
// Reserved registers.
constexpr RegStorage rs_xSUSPEND(RegStorage::kValid | rxSUSPEND);
constexpr RegStorage rs_xSELF(RegStorage::kValid | rxSELF);
constexpr RegStorage rs_sp(RegStorage::kValid | rsp);
constexpr RegStorage rs_xLR(RegStorage::kValid | rxLR);
// TODO: eliminate the need for these.
constexpr RegStorage rs_wSUSPEND(RegStorage::kValid | rwSUSPEND);
constexpr RegStorage rs_wSELF(RegStorage::kValid | rwSELF);
constexpr RegStorage rs_wsp(RegStorage::kValid | rwsp);
constexpr RegStorage rs_wLR(RegStorage::kValid | rwLR);

// RegisterLocation templates return values (following the hard-float calling convention).
const RegLocation arm_loc_c_return =
    {kLocPhysReg, 0, 0, 0, 0, 0, 0, 0, 1, rs_w0, INVALID_SREG, INVALID_SREG};
const RegLocation arm_loc_c_return_ref =
    {kLocPhysReg, 0, 0, 0, 0, 0, 1, 0, 1, rs_x0, INVALID_SREG, INVALID_SREG};
const RegLocation arm_loc_c_return_wide =
    {kLocPhysReg, 1, 0, 0, 0, 0, 0, 0, 1, rs_x0, INVALID_SREG, INVALID_SREG};
const RegLocation arm_loc_c_return_float =
    {kLocPhysReg, 0, 0, 0, 1, 0, 0, 0, 1, rs_f0, INVALID_SREG, INVALID_SREG};
const RegLocation arm_loc_c_return_double =
    {kLocPhysReg, 1, 0, 0, 1, 0, 0, 0, 1, rs_d0, INVALID_SREG, INVALID_SREG};

/**
 * @brief Shift-type to be applied to a register via EncodeShift().
 */
enum A64ShiftEncodings {
  kA64Lsl = 0x0,
  kA64Lsr = 0x1,
  kA64Asr = 0x2,
  kA64Ror = 0x3
};

/**
 * @brief Extend-type to be applied to a register via EncodeExtend().
 */
enum A64RegExtEncodings {
  kA64Uxtb = 0x0,
  kA64Uxth = 0x1,
  kA64Uxtw = 0x2,
  kA64Uxtx = 0x3,
  kA64Sxtb = 0x4,
  kA64Sxth = 0x5,
  kA64Sxtw = 0x6,
  kA64Sxtx = 0x7
};

#define ENCODE_NO_SHIFT (EncodeShift(kA64Lsl, 0))
#define ENCODE_NO_EXTEND (EncodeExtend(kA64Uxtx, 0))
/*
 * The following enum defines the list of supported A64 instructions by the
 * assembler. Their corresponding EncodingMap positions will be defined in
 * assemble_arm64.cc.
 */
enum ArmOpcode {
  kA64First = 0,
  kA64Adc3rrr = kA64First,  // adc [00011010000] rm[20-16] [000000] rn[9-5] rd[4-0].
  kA64Add4RRdT,      // add [s001000100] imm_12[21-10] rn[9-5] rd[4-0].
  kA64Add4rrro,      // add [00001011000] rm[20-16] imm_6[15-10] rn[9-5] rd[4-0].
  kA64Add4RRre,      // add [00001011001] rm[20-16] option[15-13] imm_3[12-10] rn[9-5] rd[4-0].
  kA64Adr2xd,        // adr [0] immlo[30-29] [10000] immhi[23-5] rd[4-0].
  kA64And3Rrl,       // and [00010010] N[22] imm_r[21-16] imm_s[15-10] rn[9-5] rd[4-0].
  kA64And4rrro,      // and [00001010] shift[23-22] [N=0] rm[20-16] imm_6[15-10] rn[9-5] rd[4-0].
  kA64Asr3rrd,       // asr [0001001100] immr[21-16] imms[15-10] rn[9-5] rd[4-0].
  kA64Asr3rrr,       // asr alias of "sbfm arg0, arg1, arg2, {#31/#63}".
  kA64B2ct,          // b.cond [01010100] imm_19[23-5] [0] cond[3-0].
  kA64Blr1x,         // blr [1101011000111111000000] rn[9-5] [00000].
  kA64Br1x,          // br  [1101011000011111000000] rn[9-5] [00000].
  kA64Brk1d,         // brk [11010100001] imm_16[20-5] [00000].
  kA64B1t,           // b   [00010100] offset_26[25-0].
  kA64Cbnz2rt,       // cbnz[00110101] imm_19[23-5] rt[4-0].
  kA64Cbz2rt,        // cbz [00110100] imm_19[23-5] rt[4-0].
  kA64Cmn3rro,       // cmn [s0101011] shift[23-22] [0] rm[20-16] imm_6[15-10] rn[9-5] [11111].
  kA64Cmn3Rre,       // cmn [s0101011001] rm[20-16] option[15-13] imm_3[12-10] rn[9-5] [11111].
  kA64Cmn3RdT,       // cmn [00110001] shift[23-22] imm_12[21-10] rn[9-5] [11111].
  kA64Cmp3rro,       // cmp [s1101011] shift[23-22] [0] rm[20-16] imm_6[15-10] rn[9-5] [11111].
  kA64Cmp3Rre,       // cmp [s1101011001] rm[20-16] option[15-13] imm_3[12-10] rn[9-5] [11111].
  kA64Cmp3RdT,       // cmp [01110001] shift[23-22] imm_12[21-10] rn[9-5] [11111].
  kA64Csel4rrrc,     // csel[s0011010100] rm[20-16] cond[15-12] [00] rn[9-5] rd[4-0].
  kA64Csinc4rrrc,    // csinc [s0011010100] rm[20-16] cond[15-12] [01] rn[9-5] rd[4-0].
  kA64Csinv4rrrc,    // csinv [s1011010100] rm[20-16] cond[15-12] [00] rn[9-5] rd[4-0].
  kA64Csneg4rrrc,    // csneg [s1011010100] rm[20-16] cond[15-12] [01] rn[9-5] rd[4-0].
  kA64Dmb1B,         // dmb [11010101000000110011] CRm[11-8] [10111111].
  kA64Eor3Rrl,       // eor [s10100100] N[22] imm_r[21-16] imm_s[15-10] rn[9-5] rd[4-0].
  kA64Eor4rrro,      // eor [s1001010] shift[23-22] [0] rm[20-16] imm_6[15-10] rn[9-5] rd[4-0].
  kA64Extr4rrrd,     // extr[s00100111N0] rm[20-16] imm_s[15-10] rn[9-5] rd[4-0].
  kA64Fabs2ff,       // fabs[000111100s100000110000] rn[9-5] rd[4-0].
  kA64Fadd3fff,      // fadd[000111100s1] rm[20-16] [001010] rn[9-5] rd[4-0].
  kA64Fcmp1f,        // fcmp[000111100s100000001000] rn[9-5] [01000].
  kA64Fcmp2ff,       // fcmp[000111100s1] rm[20-16] [001000] rn[9-5] [00000].
  kA64Fcvtzs2wf,     // fcvtzs [000111100s111000000000] rn[9-5] rd[4-0].
  kA64Fcvtzs2xf,     // fcvtzs [100111100s111000000000] rn[9-5] rd[4-0].
  kA64Fcvt2Ss,       // fcvt   [0001111000100010110000] rn[9-5] rd[4-0].
  kA64Fcvt2sS,       // fcvt   [0001111001100010010000] rn[9-5] rd[4-0].
  kA64Fcvtms2ws,     // fcvtms [0001111000110000000000] rn[9-5] rd[4-0].
  kA64Fcvtms2xS,     // fcvtms [1001111001110000000000] rn[9-5] rd[4-0].
  kA64Fdiv3fff,      // fdiv[000111100s1] rm[20-16] [000110] rn[9-5] rd[4-0].
  kA64Fmax3fff,      // fmax[000111100s1] rm[20-16] [010010] rn[9-5] rd[4-0].
  kA64Fmin3fff,      // fmin[000111100s1] rm[20-16] [010110] rn[9-5] rd[4-0].
  kA64Fmov2ff,       // fmov[000111100s100000010000] rn[9-5] rd[4-0].
  kA64Fmov2fI,       // fmov[000111100s1] imm_8[20-13] [10000000] rd[4-0].
  kA64Fmov2sw,       // fmov[0001111000100111000000] rn[9-5] rd[4-0].
  kA64Fmov2Sx,       // fmov[1001111001100111000000] rn[9-5] rd[4-0].
  kA64Fmov2ws,       // fmov[0001111001101110000000] rn[9-5] rd[4-0].
  kA64Fmov2xS,       // fmov[1001111001101111000000] rn[9-5] rd[4-0].
  kA64Fmul3fff,      // fmul[000111100s1] rm[20-16] [000010] rn[9-5] rd[4-0].
  kA64Fneg2ff,       // fneg[000111100s100001010000] rn[9-5] rd[4-0].
  kA64Frintp2ff,     // frintp [000111100s100100110000] rn[9-5] rd[4-0].
  kA64Frintm2ff,     // frintm [000111100s100101010000] rn[9-5] rd[4-0].
  kA64Frintn2ff,     // frintn [000111100s100100010000] rn[9-5] rd[4-0].
  kA64Frintz2ff,     // frintz [000111100s100101110000] rn[9-5] rd[4-0].
  kA64Fsqrt2ff,      // fsqrt[000111100s100001110000] rn[9-5] rd[4-0].
  kA64Fsub3fff,      // fsub[000111100s1] rm[20-16] [001110] rn[9-5] rd[4-0].
  kA64Ldrb3wXd,      // ldrb[0011100101] imm_12[21-10] rn[9-5] rt[4-0].
  kA64Ldrb3wXx,      // ldrb[00111000011] rm[20-16] [011] S[12] [10] rn[9-5] rt[4-0].
  kA64Ldrsb3rXd,     // ldrsb[001110011s] imm_12[21-10] rn[9-5] rt[4-0].
  kA64Ldrsb3rXx,     // ldrsb[0011 1000 1s1] rm[20-16] [011] S[12] [10] rn[9-5] rt[4-0].
  kA64Ldrh3wXF,      // ldrh[0111100101] imm_12[21-10] rn[9-5] rt[4-0].
  kA64Ldrh4wXxd,     // ldrh[01111000011] rm[20-16] [011] S[12] [10] rn[9-5] rt[4-0].
  kA64Ldrsh3rXF,     // ldrsh[011110011s] imm_12[21-10] rn[9-5] rt[4-0].
  kA64Ldrsh4rXxd,    // ldrsh[011110001s1] rm[20-16] [011] S[12] [10] rn[9-5] rt[4-0]
  kA64Ldr2fp,        // ldr [0s011100] imm_19[23-5] rt[4-0].
  kA64Ldr2rp,        // ldr [0s011000] imm_19[23-5] rt[4-0].
  kA64Ldr3fXD,       // ldr [1s11110100] imm_12[21-10] rn[9-5] rt[4-0].
  kA64Ldr3rXD,       // ldr [1s111000010] imm_9[20-12] [01] rn[9-5] rt[4-0].
  kA64Ldr4fXxG,      // ldr [1s111100011] rm[20-16] [011] S[12] [10] rn[9-5] rt[4-0].
  kA64Ldr4rXxG,      // ldr [1s111000011] rm[20-16] [011] S[12] [10] rn[9-5] rt[4-0].
  kA64LdrPost3rXd,   // ldr [1s111000010] imm_9[20-12] [01] rn[9-5] rt[4-0].
  kA64Ldp4ffXD,      // ldp [0s10110101] imm_7[21-15] rt2[14-10] rn[9-5] rt[4-0].
  kA64Ldp4rrXD,      // ldp [s010100101] imm_7[21-15] rt2[14-10] rn[9-5] rt[4-0].
  kA64LdpPost4rrXD,  // ldp [s010100011] imm_7[21-15] rt2[14-10] rn[9-5] rt[4-0].
  kA64Ldur3fXd,      // ldur[1s111100010] imm_9[20-12] [00] rn[9-5] rt[4-0].
  kA64Ldur3rXd,      // ldur[1s111000010] imm_9[20-12] [00] rn[9-5] rt[4-0].
  kA64Ldxr2rX,       // ldxr[1s00100001011111011111] rn[9-5] rt[4-0].
  kA64Ldaxr2rX,      // ldaxr[1s00100001011111111111] rn[9-5] rt[4-0].
  kA64Lsl3rrr,       // lsl [s0011010110] rm[20-16] [001000] rn[9-5] rd[4-0].
  kA64Lsr3rrd,       // lsr alias of "ubfm arg0, arg1, arg2, #{31/63}".
  kA64Lsr3rrr,       // lsr [s0011010110] rm[20-16] [001001] rn[9-5] rd[4-0].
  kA64Movk3rdM,      // mov [010100101] hw[22-21] imm_16[20-5] rd[4-0].
  kA64Movn3rdM,      // mov [000100101] hw[22-21] imm_16[20-5] rd[4-0].
  kA64Movz3rdM,      // mov [011100101] hw[22-21] imm_16[20-5] rd[4-0].
  kA64Mov2rr,        // mov [00101010000] rm[20-16] [000000] [11111] rd[4-0].
  kA64Mvn2rr,        // mov [00101010001] rm[20-16] [000000] [11111] rd[4-0].
  kA64Mul3rrr,       // mul [00011011000] rm[20-16] [011111] rn[9-5] rd[4-0].
  kA64Madd4rrrr,     // madd[s0011011000] rm[20-16] [0] ra[14-10] rn[9-5] rd[4-0].
  kA64Msub4rrrr,     // msub[s0011011000] rm[20-16] [1] ra[14-10] rn[9-5] rd[4-0].
  kA64Neg3rro,       // neg alias of "sub arg0, rzr, arg1, arg2".
  kA64Orr3Rrl,       // orr [s01100100] N[22] imm_r[21-16] imm_s[15-10] rn[9-5] rd[4-0].
  kA64Orr4rrro,      // orr [s0101010] shift[23-22] [0] rm[20-16] imm_6[15-10] rn[9-5] rd[4-0].
  kA64Ret,           // ret [11010110010111110000001111000000].
  kA64Rbit2rr,       // rbit [s101101011000000000000] rn[9-5] rd[4-0].
  kA64Rev2rr,        // rev [s10110101100000000001x] rn[9-5] rd[4-0].
  kA64Rev162rr,      // rev16[s101101011000000000001] rn[9-5] rd[4-0].
  kA64Ror3rrr,       // ror [s0011010110] rm[20-16] [001011] rn[9-5] rd[4-0].
  kA64Sbc3rrr,       // sbc [s0011010000] rm[20-16] [000000] rn[9-5] rd[4-0].
  kA64Sbfm4rrdd,     // sbfm[0001001100] imm_r[21-16] imm_s[15-10] rn[9-5] rd[4-0].
  kA64Scvtf2fw,      // scvtf  [000111100s100010000000] rn[9-5] rd[4-0].
  kA64Scvtf2fx,      // scvtf  [100111100s100010000000] rn[9-5] rd[4-0].
  kA64Sdiv3rrr,      // sdiv[s0011010110] rm[20-16] [000011] rn[9-5] rd[4-0].
  kA64Smaddl4xwwx,   // smaddl [10011011001] rm[20-16] [0] ra[14-10] rn[9-5] rd[4-0].
  kA64Smulh3xxx,     // smulh [10011011010] rm[20-16] [011111] rn[9-5] rd[4-0].
  kA64Stp4ffXD,      // stp [0s10110100] imm_7[21-15] rt2[14-10] rn[9-5] rt[4-0].
  kA64Stp4rrXD,      // stp [s010100100] imm_7[21-15] rt2[14-10] rn[9-5] rt[4-0].
  kA64StpPost4rrXD,  // stp [s010100010] imm_7[21-15] rt2[14-10] rn[9-5] rt[4-0].
  kA64StpPre4ffXD,   // stp [0s10110110] imm_7[21-15] rt2[14-10] rn[9-5] rt[4-0].
  kA64StpPre4rrXD,   // stp [s010100110] imm_7[21-15] rt2[14-10] rn[9-5] rt[4-0].
  kA64Str3fXD,       // str [1s11110100] imm_12[21-10] rn[9-5] rt[4-0].
  kA64Str4fXxG,      // str [1s111100001] rm[20-16] [011] S[12] [10] rn[9-5] rt[4-0].
  kA64Str3rXD,       // str [1s11100100] imm_12[21-10] rn[9-5] rt[4-0].
  kA64Str4rXxG,      // str [1s111000001] rm[20-16] option[15-13] S[12-12] [10] rn[9-5] rt[4-0].
  kA64Strb3wXd,      // strb[0011100100] imm_12[21-10] rn[9-5] rt[4-0].
  kA64Strb3wXx,      // strb[00111000001] rm[20-16] [011] S[12] [10] rn[9-5] rt[4-0].
  kA64Strh3wXF,      // strh[0111100100] imm_12[21-10] rn[9-5] rt[4-0].
  kA64Strh4wXxd,     // strh[01111000001] rm[20-16] [011] S[12] [10] rn[9-5] rt[4-0].
  kA64StrPost3rXd,   // str [1s111000000] imm_9[20-12] [01] rn[9-5] rt[4-0].
  kA64Stur3fXd,      // stur[1s111100000] imm_9[20-12] [00] rn[9-5] rt[4-0].
  kA64Stur3rXd,      // stur[1s111000000] imm_9[20-12] [00] rn[9-5] rt[4-0].
  kA64Stxr3wrX,      // stxr[11001000000] rs[20-16] [011111] rn[9-5] rt[4-0].
  kA64Stlxr3wrX,     // stlxr[11001000000] rs[20-16] [111111] rn[9-5] rt[4-0].
  kA64Sub4RRdT,      // sub [s101000100] imm_12[21-10] rn[9-5] rd[4-0].
  kA64Sub4rrro,      // sub [s1001011000] rm[20-16] imm_6[15-10] rn[9-5] rd[4-0].
  kA64Sub4RRre,      // sub [s1001011001] rm[20-16] option[15-13] imm_3[12-10] rn[9-5] rd[4-0].
  kA64Subs3rRd,      // subs[s111000100] imm_12[21-10] rn[9-5] rd[4-0].
  kA64Tst3rro,       // tst alias of "ands rzr, arg1, arg2, arg3".
  kA64Ubfm4rrdd,     // ubfm[s10100110] N[22] imm_r[21-16] imm_s[15-10] rn[9-5] rd[4-0].
  kA64Last,
  kA64NotWide = 0,   // Flag used to select the first instruction variant.
  kA64Wide = 0x1000  // Flag used to select the second instruction variant.
};

/*
 * The A64 instruction set provides two variants for many instructions. For example, "mov wN, wM"
 * and "mov xN, xM" or - for floating point instructions - "mov sN, sM" and "mov dN, dM".
 * It definitely makes sense to exploit this symmetries of the instruction set. We do this via the
 * WIDE, UNWIDE macros. For opcodes that allow it, the wide variant can be obtained by applying the
 * WIDE macro to the non-wide opcode. E.g. WIDE(kA64Sub4RRdT).
 */

// Return the wide and no-wide variants of the given opcode.
#define WIDE(op) ((ArmOpcode)((op) | kA64Wide))
#define UNWIDE(op) ((ArmOpcode)((op) & ~kA64Wide))

// Whether the given opcode is wide.
#define IS_WIDE(op) (((op) & kA64Wide) != 0)

/*
 * Floating point variants. These are just aliases of the macros above which we use for floating
 * point instructions, just for readibility reasons.
 * TODO(Arm64): should we remove these and use the original macros?
 */
#define FWIDE WIDE
#define FUNWIDE UNWIDE
#define IS_FWIDE IS_WIDE

enum ArmOpDmbOptions {
  kSY = 0xf,
  kST = 0xe,
  kISH = 0xb,
  kISHST = 0xa,
  kISHLD = 0x9,
  kNSH = 0x7,
  kNSHST = 0x6
};

// Instruction assembly field_loc kind.
enum ArmEncodingKind {
  // All the formats below are encoded in the same way (as a kFmtBitBlt).
  // These are grouped together, for fast handling (e.g. "if (LIKELY(fmt <= kFmtBitBlt)) ...").
  kFmtRegW = 0,  // Word register (w) or wzr.
  kFmtRegX,      // Extended word register (x) or xzr.
  kFmtRegR,      // Register with same width as the instruction or zr.
  kFmtRegWOrSp,  // Word register (w) or wsp.
  kFmtRegXOrSp,  // Extended word register (x) or sp.
  kFmtRegROrSp,  // Register with same width as the instruction or sp.
  kFmtRegS,      // Single FP reg.
  kFmtRegD,      // Double FP reg.
  kFmtRegF,      // Single/double FP reg depending on the instruction width.
  kFmtBitBlt,    // Bit string using end/start.

  // Less likely formats.
  kFmtUnused,    // Unused field and marks end of formats.
  kFmtImm21,     // Sign-extended immediate using [23..5,30..29].
  kFmtShift,     // Register shift, 9-bit at [23..21, 15..10]..
  kFmtExtend,    // Register extend, 9-bit at [23..21, 15..10].
  kFmtSkip,      // Unused field, but continue to next.
};

// Struct used to define the snippet positions for each A64 opcode.
struct ArmEncodingMap {
  uint32_t wskeleton;
  uint32_t xskeleton;
  struct {
    ArmEncodingKind kind;
    int end;         // end for kFmtBitBlt, 1-bit slice end for FP regs.
    int start;       // start for kFmtBitBlt, 4-bit slice end for FP regs.
  } field_loc[4];
  ArmOpcode opcode;  // can be WIDE()-ned to indicate it has a wide variant.
  uint64_t flags;
  const char* name;
  const char* fmt;
  int size;          // Note: size is in bytes.
  FixupKind fixup;
};

#if 0
// TODO(Arm64): try the following alternative, which fits exactly in one cache line (64 bytes).
struct ArmEncodingMap {
  uint32_t wskeleton;
  uint32_t xskeleton;
  uint64_t flags;
  const char* name;
  const char* fmt;
  struct {
    uint8_t kind;
    int8_t end;         // end for kFmtBitBlt, 1-bit slice end for FP regs.
    int8_t start;       // start for kFmtBitBlt, 4-bit slice end for FP regs.
  } field_loc[4];
  uint32_t fixup;
  uint32_t opcode;         // can be WIDE()-ned to indicate it has a wide variant.
  uint32_t padding[3];
};
#endif

}  // namespace art

#endif  // ART_COMPILER_DEX_QUICK_ARM64_ARM64_LIR_H_
