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

#ifndef ART_COMPILER_DEX_QUICK_MIPS_MIPS_LIR_H_
#define ART_COMPILER_DEX_QUICK_MIPS_MIPS_LIR_H_

#include "dex/compiler_internals.h"

namespace art {

/*
 * Runtime register conventions.
 *
 * zero is always the value 0
 * at is scratch (normally used as temp reg by assembler)
 * v0, v1 are scratch (normally hold subroutine return values)
 * a0-a3 are scratch (normally hold subroutine arguments)
 * t0-t8 are scratch
 * t9 is scratch (normally used for function calls)
 * s0 (rMIPS_SUSPEND) is reserved [holds suspend-check counter]
 * s1 (rMIPS_SELF) is reserved [holds current &Thread]
 * s2-s7 are callee save (promotion target)
 * k0, k1 are reserved for use by interrupt handlers
 * gp is reserved for global pointer
 * sp is reserved
 * s8 is callee save (promotion target)
 * ra is scratch (normally holds the return addr)
 *
 * Preserved across C calls: s0-s8
 * Trashed across C calls: at, v0-v1, a0-a3, t0-t9, gp, ra
 *
 * Floating pointer registers
 * NOTE: there are 32 fp registers (16 df pairs), but currently
 *       only support 16 fp registers (8 df pairs).
 * f0-f15
 * df0-df7, where df0={f0,f1}, df1={f2,f3}, ... , df7={f14,f15}
 *
 * f0-f15 (df0-df7) trashed across C calls
 *
 * For mips32 code use:
 *      a0-a3 to hold operands
 *      v0-v1 to hold results
 *      t0-t9 for temps
 *
 * All jump/branch instructions have a delay slot after it.
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

// Offset to distingish FP regs.
#define MIPS_FP_REG_OFFSET 32
// Offset to distinguish DP FP regs.
#define MIPS_FP_DOUBLE 64
// Reg types.
#define MIPS_REGTYPE(x) (x & (MIPS_FP_REG_OFFSET | MIPS_FP_DOUBLE))
#define MIPS_FPREG(x) ((x & MIPS_FP_REG_OFFSET) == MIPS_FP_REG_OFFSET)
#define MIPS_DOUBLEREG(x) ((x & MIPS_FP_DOUBLE) == MIPS_FP_DOUBLE)
#define MIPS_SINGLEREG(x) (MIPS_FPREG(x) && !MIPS_DOUBLEREG(x))
// FIXME: out of date comment.
/*
 * Note: the low register of a floating point pair is sufficient to
 * create the name of a double, but require both names to be passed to
 * allow for asserts to verify that the pair is consecutive if significant
 * rework is done in this area.  Also, it is a good reminder in the calling
 * code that reg locations always describe doubles as a pair of singles.
 */
#define MIPS_S2D(x, y) ((x) | MIPS_FP_DOUBLE)
// Mask to strip off fp flags.
#define MIPS_FP_REG_MASK (MIPS_FP_REG_OFFSET-1)

#define LOWORD_OFFSET 0
#define HIWORD_OFFSET 4
#define rARG0 rA0
#define rs_rARG0 rs_rA0
#define rARG1 rA1
#define rs_rARG1 rs_rA1
#define rARG2 rA2
#define rs_rARG2 rs_rA2
#define rARG3 rA3
#define rs_rARG3 rs_rA3
#define rRESULT0 rV0
#define rs_rRESULT0 rs_rV0
#define rRESULT1 rV1
#define rs_rRESULT1 rs_rV1

#define rFARG0 rF12
#define rs_rFARG0 rs_rF12
#define rFARG1 rF13
#define rs_rFARG1 rs_rF13
#define rFARG2 rF14
#define rs_rFARG2 rs_rF14
#define rFARG3 rF15
#define rs_rFARG3 rs_rF15
#define rFRESULT0 rF0
#define rs_rFRESULT0 rs_rF0
#define rFRESULT1 rF1
#define rs_rFRESULT1 rs_rF1

// Regs not used for Mips.
#define rMIPS_LR RegStorage::kInvalidRegVal
#define rMIPS_PC RegStorage::kInvalidRegVal

enum MipsResourceEncodingPos {
  kMipsGPReg0   = 0,
  kMipsRegSP    = 29,
  kMipsRegLR    = 31,
  kMipsFPReg0   = 32,  // only 16 fp regs supported currently.
  kMipsFPRegEnd   = 48,
  kMipsRegHI    = kMipsFPRegEnd,
  kMipsRegLO,
  kMipsRegPC,
  kMipsRegEnd   = 51,
};

#define ENCODE_MIPS_REG_LIST(N)      (static_cast<uint64_t>(N))
#define ENCODE_MIPS_REG_SP           (1ULL << kMipsRegSP)
#define ENCODE_MIPS_REG_LR           (1ULL << kMipsRegLR)
#define ENCODE_MIPS_REG_PC           (1ULL << kMipsRegPC)
#define ENCODE_MIPS_REG_HI           (1ULL << kMipsRegHI)
#define ENCODE_MIPS_REG_LO           (1ULL << kMipsRegLO)

enum MipsNativeRegisterPool {
  rZERO = 0,
  rAT = 1,
  rV0 = 2,
  rV1 = 3,
  rA0 = 4,
  rA1 = 5,
  rA2 = 6,
  rA3 = 7,
  rT0 = 8,
  rT1 = 9,
  rT2 = 10,
  rT3 = 11,
  rT4 = 12,
  rT5 = 13,
  rT6 = 14,
  rT7 = 15,
  rS0 = 16,
  rS1 = 17,
  rS2 = 18,
  rS3 = 19,
  rS4 = 20,
  rS5 = 21,
  rS6 = 22,
  rS7 = 23,
  rT8 = 24,
  rT9 = 25,
  rK0 = 26,
  rK1 = 27,
  rGP = 28,
  rSP = 29,
  rFP = 30,
  rRA = 31,

  rF0 = 0 + MIPS_FP_REG_OFFSET,
  rF1,
  rF2,
  rF3,
  rF4,
  rF5,
  rF6,
  rF7,
  rF8,
  rF9,
  rF10,
  rF11,
  rF12,
  rF13,
  rF14,
  rF15,
#if 0
  /*
   * TODO: The shared resource mask doesn't have enough bit positions to describe all
   * MIPS registers.  Expand it and enable use of fp registers 16 through 31.
   */
  rF16,
  rF17,
  rF18,
  rF19,
  rF20,
  rF21,
  rF22,
  rF23,
  rF24,
  rF25,
  rF26,
  rF27,
  rF28,
  rF29,
  rF30,
  rF31,
#endif
  rDF0 = rF0 + MIPS_FP_DOUBLE,
  rDF1 = rF2 + MIPS_FP_DOUBLE,
  rDF2 = rF4 + MIPS_FP_DOUBLE,
  rDF3 = rF6 + MIPS_FP_DOUBLE,
  rDF4 = rF8 + MIPS_FP_DOUBLE,
  rDF5 = rF10 + MIPS_FP_DOUBLE,
  rDF6 = rF12 + MIPS_FP_DOUBLE,
  rDF7 = rF14 + MIPS_FP_DOUBLE,
#if 0  // TODO: expand resource mask to enable use of all MIPS fp registers.
  rDF8 = rF16 + MIPS_FP_DOUBLE,
  rDF9 = rF18 + MIPS_FP_DOUBLE,
  rDF10 = rF20 + MIPS_FP_DOUBLE,
  rDF11 = rF22 + MIPS_FP_DOUBLE,
  rDF12 = rF24 + MIPS_FP_DOUBLE,
  rDF13 = rF26 + MIPS_FP_DOUBLE,
  rDF14 = rF28 + MIPS_FP_DOUBLE,
  rDF15 = rF30 + MIPS_FP_DOUBLE,
#endif
};

const RegStorage rs_rZERO(RegStorage::k32BitSolo, rZERO);
const RegStorage rs_rAT(RegStorage::k32BitSolo, rAT);
const RegStorage rs_rV0(RegStorage::k32BitSolo, rV0);
const RegStorage rs_rV1(RegStorage::k32BitSolo, rV1);
const RegStorage rs_rA0(RegStorage::k32BitSolo, rA0);
const RegStorage rs_rA1(RegStorage::k32BitSolo, rA1);
const RegStorage rs_rA2(RegStorage::k32BitSolo, rA2);
const RegStorage rs_rA3(RegStorage::k32BitSolo, rA3);
const RegStorage rs_rT0(RegStorage::k32BitSolo, rT0);
const RegStorage rs_rT1(RegStorage::k32BitSolo, rT1);
const RegStorage rs_rT2(RegStorage::k32BitSolo, rT2);
const RegStorage rs_rT3(RegStorage::k32BitSolo, rT3);
const RegStorage rs_rT4(RegStorage::k32BitSolo, rT4);
const RegStorage rs_rT5(RegStorage::k32BitSolo, rT5);
const RegStorage rs_rT6(RegStorage::k32BitSolo, rT6);
const RegStorage rs_rT7(RegStorage::k32BitSolo, rT7);
const RegStorage rs_rS0(RegStorage::k32BitSolo, rS0);
const RegStorage rs_rS1(RegStorage::k32BitSolo, rS1);
const RegStorage rs_rS2(RegStorage::k32BitSolo, rS2);
const RegStorage rs_rS3(RegStorage::k32BitSolo, rS3);
const RegStorage rs_rS4(RegStorage::k32BitSolo, rS4);
const RegStorage rs_rS5(RegStorage::k32BitSolo, rS5);
const RegStorage rs_rS6(RegStorage::k32BitSolo, rS6);
const RegStorage rs_rS7(RegStorage::k32BitSolo, rS7);
const RegStorage rs_rT8(RegStorage::k32BitSolo, rT8);
const RegStorage rs_rT9(RegStorage::k32BitSolo, rT9);
const RegStorage rs_rK0(RegStorage::k32BitSolo, rK0);
const RegStorage rs_rK1(RegStorage::k32BitSolo, rK1);
const RegStorage rs_rGP(RegStorage::k32BitSolo, rGP);
const RegStorage rs_rSP(RegStorage::k32BitSolo, rSP);
const RegStorage rs_rFP(RegStorage::k32BitSolo, rFP);
const RegStorage rs_rRA(RegStorage::k32BitSolo, rRA);
const RegStorage rs_rF12(RegStorage::k32BitSolo, rF12);
const RegStorage rs_rF13(RegStorage::k32BitSolo, rF13);
const RegStorage rs_rF14(RegStorage::k32BitSolo, rF14);
const RegStorage rs_rF15(RegStorage::k32BitSolo, rF15);
const RegStorage rs_rF0(RegStorage::k32BitSolo, rF0);
const RegStorage rs_rF1(RegStorage::k32BitSolo, rF1);

// TODO: reduce/eliminate use of these.
#define rMIPS_SUSPEND rS0
#define rs_rMIPS_SUSPEND rs_rS0
#define rMIPS_SELF rS1
#define rs_rMIPS_SELF rs_rS1
#define rMIPS_SP rSP
#define rs_rMIPS_SP rs_rSP
#define rMIPS_ARG0 rARG0
#define rs_rMIPS_ARG0 rs_rARG0
#define rMIPS_ARG1 rARG1
#define rs_rMIPS_ARG1 rs_rARG1
#define rMIPS_ARG2 rARG2
#define rs_rMIPS_ARG2 rs_rARG2
#define rMIPS_ARG3 rARG3
#define rs_rMIPS_ARG3 rs_rARG3
#define rMIPS_FARG0 rFARG0
#define rs_rMIPS_FARG0 rs_rFARG0
#define rMIPS_FARG1 rFARG1
#define rs_rMIPS_FARG1 rs_rFARG1
#define rMIPS_FARG2 rFARG2
#define rs_rMIPS_FARG2 rs_rFARG2
#define rMIPS_FARG3 rFARG3
#define rs_MIPS_FARG3 rs_rFARG3
#define rMIPS_RET0 rRESULT0
#define rs_MIPS_RET0 rs_rRESULT0
#define rMIPS_RET1 rRESULT1
#define rs_rMIPS_RET1 rs_rRESULT1
#define rMIPS_INVOKE_TGT rT9
#define rs_rMIPS_INVOKE_TGT rs_rT9
#define rMIPS_COUNT RegStorage::kInvalidRegVal

// RegisterLocation templates return values (r_V0, or r_V0/r_V1).
const RegLocation mips_loc_c_return
    {kLocPhysReg, 0, 0, 0, 0, 0, 0, 0, 1, kVectorNotUsed,
     RegStorage(RegStorage::k32BitSolo, rV0), INVALID_SREG, INVALID_SREG};
const RegLocation mips_loc_c_return_wide
    {kLocPhysReg, 1, 0, 0, 0, 0, 0, 0, 1, kVectorNotUsed,
     RegStorage(RegStorage::k64BitPair, rV0, rV1), INVALID_SREG, INVALID_SREG};
const RegLocation mips_loc_c_return_float
    {kLocPhysReg, 0, 0, 0, 1, 0, 0, 0, 1, kVectorNotUsed,
     RegStorage(RegStorage::k32BitSolo, rF0), INVALID_SREG, INVALID_SREG};
const RegLocation mips_loc_c_return_double
    {kLocPhysReg, 1, 0, 0, 1, 0, 0, 0, 1, kVectorNotUsed,
     RegStorage(RegStorage::k64BitPair, rF0, rF1), INVALID_SREG, INVALID_SREG};

enum MipsShiftEncodings {
  kMipsLsl = 0x0,
  kMipsLsr = 0x1,
  kMipsAsr = 0x2,
  kMipsRor = 0x3
};

// MIPS sync kinds (Note: support for kinds other than kSYNC0 may not exist).
#define kSYNC0        0x00
#define kSYNC_WMB     0x04
#define kSYNC_MB      0x01
#define kSYNC_ACQUIRE 0x11
#define kSYNC_RELEASE 0x12
#define kSYNC_RMB     0x13

// TODO: Use smaller hammer when appropriate for target CPU.
#define kST kSYNC0
#define kSY kSYNC0

/*
 * The following enum defines the list of supported Thumb instructions by the
 * assembler. Their corresponding EncodingMap positions will be defined in
 * Assemble.cc.
 */
enum MipsOpCode {
  kMipsFirst = 0,
  kMips32BitData = kMipsFirst,  // data [31..0].
  kMipsAddiu,  // addiu t,s,imm16 [001001] s[25..21] t[20..16] imm16[15..0].
  kMipsAddu,  // add d,s,t [000000] s[25..21] t[20..16] d[15..11] [00000100001].
  kMipsAnd,   // and d,s,t [000000] s[25..21] t[20..16] d[15..11] [00000100100].
  kMipsAndi,  // andi t,s,imm16 [001100] s[25..21] t[20..16] imm16[15..0].
  kMipsB,     // b o   [0001000000000000] o[15..0].
  kMipsBal,   // bal o [0000010000010001] o[15..0].
  // NOTE: the code tests the range kMipsBeq thru kMipsBne, so adding an instruction in this
  //       range may require updates.
  kMipsBeq,   // beq s,t,o [000100] s[25..21] t[20..16] o[15..0].
  kMipsBeqz,  // beqz s,o [000100] s[25..21] [00000] o[15..0].
  kMipsBgez,  // bgez s,o [000001] s[25..21] [00001] o[15..0].
  kMipsBgtz,  // bgtz s,o [000111] s[25..21] [00000] o[15..0].
  kMipsBlez,  // blez s,o [000110] s[25..21] [00000] o[15..0].
  kMipsBltz,  // bltz s,o [000001] s[25..21] [00000] o[15..0].
  kMipsBnez,  // bnez s,o [000101] s[25..21] [00000] o[15..0].
  kMipsBne,   // bne s,t,o [000101] s[25..21] t[20..16] o[15..0].
  kMipsDiv,   // div s,t [000000] s[25..21] t[20..16] [0000000000011010].
#if __mips_isa_rev >= 2
  kMipsExt,   // ext t,s,p,z [011111] s[25..21] t[20..16] z[15..11] p[10..6] [000000].
#endif
  kMipsJal,   // jal t [000011] t[25..0].
  kMipsJalr,  // jalr d,s [000000] s[25..21] [00000] d[15..11] hint[10..6] [001001].
  kMipsJr,    // jr s [000000] s[25..21] [0000000000] hint[10..6] [001000].
  kMipsLahi,  // lui t,imm16 [00111100000] t[20..16] imm16[15..0] load addr hi.
  kMipsLalo,  // ori t,s,imm16 [001001] s[25..21] t[20..16] imm16[15..0] load addr lo.
  kMipsLui,   // lui t,imm16 [00111100000] t[20..16] imm16[15..0].
  kMipsLb,    // lb t,o(b) [100000] b[25..21] t[20..16] o[15..0].
  kMipsLbu,   // lbu t,o(b) [100100] b[25..21] t[20..16] o[15..0].
  kMipsLh,    // lh t,o(b) [100001] b[25..21] t[20..16] o[15..0].
  kMipsLhu,   // lhu t,o(b) [100101] b[25..21] t[20..16] o[15..0].
  kMipsLw,    // lw t,o(b) [100011] b[25..21] t[20..16] o[15..0].
  kMipsMfhi,  // mfhi d [0000000000000000] d[15..11] [00000010000].
  kMipsMflo,  // mflo d [0000000000000000] d[15..11] [00000010010].
  kMipsMove,  // move d,s [000000] s[25..21] [00000] d[15..11] [00000100101].
  kMipsMovz,  // movz d,s,t [000000] s[25..21] t[20..16] d[15..11] [00000001010].
  kMipsMul,   // mul d,s,t [011100] s[25..21] t[20..16] d[15..11] [00000000010].
  kMipsNop,   // nop [00000000000000000000000000000000].
  kMipsNor,   // nor d,s,t [000000] s[25..21] t[20..16] d[15..11] [00000100111].
  kMipsOr,    // or d,s,t [000000] s[25..21] t[20..16] d[15..11] [00000100101].
  kMipsOri,   // ori t,s,imm16 [001001] s[25..21] t[20..16] imm16[15..0].
  kMipsPref,  // pref h,o(b) [101011] b[25..21] h[20..16] o[15..0].
  kMipsSb,    // sb t,o(b) [101000] b[25..21] t[20..16] o[15..0].
#if __mips_isa_rev >= 2
  kMipsSeb,   // seb d,t [01111100000] t[20..16] d[15..11] [10000100000].
  kMipsSeh,   // seh d,t [01111100000] t[20..16] d[15..11] [11000100000].
#endif
  kMipsSh,    // sh t,o(b) [101001] b[25..21] t[20..16] o[15..0].
  kMipsSll,   // sll d,t,a [00000000000] t[20..16] d[15..11] a[10..6] [000000].
  kMipsSllv,  // sllv d,t,s [000000] s[25..21] t[20..16] d[15..11] [00000000100].
  kMipsSlt,   // slt d,s,t [000000] s[25..21] t[20..16] d[15..11] [00000101010].
  kMipsSlti,  // slti t,s,imm16 [001010] s[25..21] t[20..16] imm16[15..0].
  kMipsSltu,  // sltu d,s,t [000000] s[25..21] t[20..16] d[15..11] [00000101011].
  kMipsSra,   // sra d,s,imm5 [00000000000] t[20..16] d[15..11] imm5[10..6] [000011].
  kMipsSrav,  // srav d,t,s [000000] s[25..21] t[20..16] d[15..11] [00000000111].
  kMipsSrl,   // srl d,t,a [00000000000] t[20..16] d[20..16] a[10..6] [000010].
  kMipsSrlv,  // srlv d,t,s [000000] s[25..21] t[20..16] d[15..11] [00000000110].
  kMipsSubu,  // subu d,s,t [000000] s[25..21] t[20..16] d[15..11] [00000100011].
  kMipsSw,    // sw t,o(b) [101011] b[25..21] t[20..16] o[15..0].
  kMipsXor,   // xor d,s,t [000000] s[25..21] t[20..16] d[15..11] [00000100110].
  kMipsXori,  // xori t,s,imm16 [001110] s[25..21] t[20..16] imm16[15..0].
  kMipsFadds,  // add.s d,s,t [01000110000] t[20..16] s[15..11] d[10..6] [000000].
  kMipsFsubs,  // sub.s d,s,t [01000110000] t[20..16] s[15..11] d[10..6] [000001].
  kMipsFmuls,  // mul.s d,s,t [01000110000] t[20..16] s[15..11] d[10..6] [000010].
  kMipsFdivs,  // div.s d,s,t [01000110000] t[20..16] s[15..11] d[10..6] [000011].
  kMipsFaddd,  // add.d d,s,t [01000110001] t[20..16] s[15..11] d[10..6] [000000].
  kMipsFsubd,  // sub.d d,s,t [01000110001] t[20..16] s[15..11] d[10..6] [000001].
  kMipsFmuld,  // mul.d d,s,t [01000110001] t[20..16] s[15..11] d[10..6] [000010].
  kMipsFdivd,  // div.d d,s,t [01000110001] t[20..16] s[15..11] d[10..6] [000011].
  kMipsFcvtsd,  // cvt.s.d d,s [01000110001] [00000] s[15..11] d[10..6] [100000].
  kMipsFcvtsw,  // cvt.s.w d,s [01000110100] [00000] s[15..11] d[10..6] [100000].
  kMipsFcvtds,  // cvt.d.s d,s [01000110000] [00000] s[15..11] d[10..6] [100001].
  kMipsFcvtdw,  // cvt.d.w d,s [01000110100] [00000] s[15..11] d[10..6] [100001].
  kMipsFcvtws,  // cvt.w.d d,s [01000110000] [00000] s[15..11] d[10..6] [100100].
  kMipsFcvtwd,  // cvt.w.d d,s [01000110001] [00000] s[15..11] d[10..6] [100100].
  kMipsFmovs,  // mov.s d,s [01000110000] [00000] s[15..11] d[10..6] [000110].
  kMipsFmovd,  // mov.d d,s [01000110001] [00000] s[15..11] d[10..6] [000110].
  kMipsFlwc1,  // lwc1 t,o(b) [110001] b[25..21] t[20..16] o[15..0].
  kMipsFldc1,  // ldc1 t,o(b) [110101] b[25..21] t[20..16] o[15..0].
  kMipsFswc1,  // swc1 t,o(b) [111001] b[25..21] t[20..16] o[15..0].
  kMipsFsdc1,  // sdc1 t,o(b) [111101] b[25..21] t[20..16] o[15..0].
  kMipsMfc1,  // mfc1 t,s [01000100000] t[20..16] s[15..11] [00000000000].
  kMipsMtc1,  // mtc1 t,s [01000100100] t[20..16] s[15..11] [00000000000].
  kMipsDelta,  // Psuedo for ori t, s, <label>-<label>.
  kMipsDeltaHi,  // Pseudo for lui t, high16(<label>-<label>).
  kMipsDeltaLo,  // Pseudo for ori t, s, low16(<label>-<label>).
  kMipsCurrPC,  // jal to .+8 to materialize pc.
  kMipsSync,    // sync kind [000000] [0000000000000000] s[10..6] [001111].
  kMipsUndefined,  // undefined [011001xxxxxxxxxxxxxxxx].
  kMipsLast
};

// Instruction assembly field_loc kind.
enum MipsEncodingKind {
  kFmtUnused,
  kFmtBitBlt,    /* Bit string using end/start */
  kFmtDfp,       /* Double FP reg */
  kFmtSfp,       /* Single FP reg */
  kFmtBlt5_2,    /* Same 5-bit field to 2 locations */
};

// Struct used to define the snippet positions for each MIPS opcode.
struct MipsEncodingMap {
  uint32_t skeleton;
  struct {
    MipsEncodingKind kind;
    int end;   // end for kFmtBitBlt, 1-bit slice end for FP regs.
    int start;  // start for kFmtBitBlt, 4-bit slice end for FP regs.
  } field_loc[4];
  MipsOpCode opcode;
  uint64_t flags;
  const char *name;
  const char* fmt;
  int size;   // Note: size is in bytes.
};

extern MipsEncodingMap EncodingMap[kMipsLast];

#define IS_UIMM16(v) ((0 <= (v)) && ((v) <= 65535))
#define IS_SIMM16(v) ((-32768 <= (v)) && ((v) <= 32766))
#define IS_SIMM16_2WORD(v) ((-32764 <= (v)) && ((v) <= 32763))  // 2 offsets must fit.

}  // namespace art

#endif  // ART_COMPILER_DEX_QUICK_MIPS_MIPS_LIR_H_
