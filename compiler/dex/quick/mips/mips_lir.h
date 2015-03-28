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

#include "dex/reg_location.h"
#include "dex/reg_storage.h"

namespace art {

/*
 * Runtime register conventions.
 *
 *          mips32            | mips64
 * $0:      zero is always the value 0
 * $1:      at is scratch (normally used as temp reg by assembler)
 * $2,$3:   v0, v1 are scratch (normally hold subroutine return values)
 * $4-$7:   a0-a3 are scratch (normally hold subroutine arguments)
 * $8-$11:  t0-t3 are scratch | a4-a7 are scratch (normally hold subroutine arguments)
 * $12-$15: t4-t7 are scratch | t0-t3 are scratch
 * $16:     s0 (rSUSPEND) is reserved [holds suspend-check counter]
 * $17:     s1 (rSELF) is reserved [holds current &Thread]
 * $18-$23: s2-s7 are callee save (promotion target)
 * $24:     t8 is scratch
 * $25:     t9 is scratch (normally used for function calls)
 * $26,$27: k0, k1 are reserved for use by interrupt handlers
 * $28:     gp is reserved for global pointer
 * $29:     sp is reserved
 * $30:     s8 is callee save (promotion target)
 * $31:     ra is scratch (normally holds the return addr)
 *
 * Preserved across C calls: s0-s8
 * Trashed across C calls (mips32): at, v0-v1, a0-a3, t0-t9, gp, ra
 * Trashed across C calls (mips64): at, v0-v1, a0-a7, t0-t3, t8, t9, gp, ra
 *
 * Floating pointer registers (mips32)
 * NOTE: there are 32 fp registers (16 df pairs), but currently
 *       only support 16 fp registers (8 df pairs).
 * f0-f15
 * df0-df7, where df0={f0,f1}, df1={f2,f3}, ... , df7={f14,f15}
 *
 * f0-f15 (df0-df7) trashed across C calls
 *
 * Floating pointer registers (mips64)
 * NOTE: there are 32 fp registers.
 * f0-f31
 *
 * For mips32 code use:
 *      a0-a3 to hold operands
 *      v0-v1 to hold results
 *      t0-t9 for temps
 *
 * For mips64 code use:
 *      a0-a7 to hold operands
 *      v0-v1 to hold results
 *      t0-t3, t8-t9 for temps
 *
 * All jump/branch instructions have a delay slot after it.
 *
 * Stack frame diagram (stack grows down, higher addresses at top):
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


#define LOWORD_OFFSET 0
#define HIWORD_OFFSET 4

#define rFARG0 rF12
#define rs_rFARG0 rs_rF12
#define rFARG1 rF13
#define rs_rFARG1 rs_rF13
#define rFARG2 rF14
#define rs_rFARG2 rs_rF14
#define rFARG3 rF15
#define rs_rFARG3 rs_rF15

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
  // Mips64 related:
  kMips64FPRegEnd = 64,
  kMips64RegPC    = kMips64FPRegEnd,
  kMips64RegEnd   = 65,
};

#define ENCODE_MIPS_REG_LIST(N)      (static_cast<uint64_t>(N))
#define ENCODE_MIPS_REG_SP           (1ULL << kMipsRegSP)
#define ENCODE_MIPS_REG_LR           (1ULL << kMipsRegLR)
#define ENCODE_MIPS_REG_PC           (1ULL << kMipsRegPC)
#define ENCODE_MIPS_REG_HI           (1ULL << kMipsRegHI)
#define ENCODE_MIPS_REG_LO           (1ULL << kMipsRegLO)

// Set FR_BIT to 0
// This bit determines how the CPU access FP registers.
#define FR_BIT   0

enum MipsNativeRegisterPool {  // private marker to avoid generate-operator-out.py from processing.
  rZERO  = RegStorage::k32BitSolo | RegStorage::kCoreRegister |  0,
  rZEROd = RegStorage::k64BitSolo | RegStorage::kCoreRegister |  0,
  rAT    = RegStorage::k32BitSolo | RegStorage::kCoreRegister |  1,
  rATd   = RegStorage::k64BitSolo | RegStorage::kCoreRegister |  1,
  rV0    = RegStorage::k32BitSolo | RegStorage::kCoreRegister |  2,
  rV0d   = RegStorage::k64BitSolo | RegStorage::kCoreRegister |  2,
  rV1    = RegStorage::k32BitSolo | RegStorage::kCoreRegister |  3,
  rV1d   = RegStorage::k64BitSolo | RegStorage::kCoreRegister |  3,
  rA0    = RegStorage::k32BitSolo | RegStorage::kCoreRegister |  4,
  rA0d   = RegStorage::k64BitSolo | RegStorage::kCoreRegister |  4,
  rA1    = RegStorage::k32BitSolo | RegStorage::kCoreRegister |  5,
  rA1d   = RegStorage::k64BitSolo | RegStorage::kCoreRegister |  5,
  rA2    = RegStorage::k32BitSolo | RegStorage::kCoreRegister |  6,
  rA2d   = RegStorage::k64BitSolo | RegStorage::kCoreRegister |  6,
  rA3    = RegStorage::k32BitSolo | RegStorage::kCoreRegister |  7,
  rA3d   = RegStorage::k64BitSolo | RegStorage::kCoreRegister |  7,
  rT0_32 = RegStorage::k32BitSolo | RegStorage::kCoreRegister |  8,
  rA4    = rT0_32,
  rA4d   = RegStorage::k64BitSolo | RegStorage::kCoreRegister |  8,
  rT1_32 = RegStorage::k32BitSolo | RegStorage::kCoreRegister |  9,
  rA5    = rT1_32,
  rA5d   = RegStorage::k64BitSolo | RegStorage::kCoreRegister |  9,
  rT2_32 = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 10,
  rA6    = rT2_32,
  rA6d   = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 10,
  rT3_32 = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 11,
  rA7    = rT3_32,
  rA7d   = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 11,
  rT4_32 = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 12,
  rT0    = rT4_32,
  rT0d   = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 12,
  rT5_32 = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 13,
  rT1    = rT5_32,
  rT1d   = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 13,
  rT6_32 = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 14,
  rT2    = rT6_32,
  rT2d   = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 14,
  rT7_32 = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 15,
  rT3    = rT7_32,
  rT3d   = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 15,
  rS0    = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 16,
  rS0d   = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 16,
  rS1    = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 17,
  rS1d   = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 17,
  rS2    = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 18,
  rS2d   = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 18,
  rS3    = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 19,
  rS3d   = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 19,
  rS4    = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 20,
  rS4d   = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 20,
  rS5    = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 21,
  rS5d   = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 21,
  rS6    = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 22,
  rS6d   = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 22,
  rS7    = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 23,
  rS7d   = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 23,
  rT8    = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 24,
  rT8d   = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 24,
  rT9    = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 25,
  rT9d   = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 25,
  rK0    = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 26,
  rK0d   = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 26,
  rK1    = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 27,
  rK1d   = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 27,
  rGP    = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 28,
  rGPd   = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 28,
  rSP    = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 29,
  rSPd   = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 29,
  rFP    = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 30,
  rFPd   = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 30,
  rRA    = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 31,
  rRAd   = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 31,

  rF0  = RegStorage::k32BitSolo | RegStorage::kFloatingPoint |  0,
  rF1  = RegStorage::k32BitSolo | RegStorage::kFloatingPoint |  1,
  rF2  = RegStorage::k32BitSolo | RegStorage::kFloatingPoint |  2,
  rF3  = RegStorage::k32BitSolo | RegStorage::kFloatingPoint |  3,
  rF4  = RegStorage::k32BitSolo | RegStorage::kFloatingPoint |  4,
  rF5  = RegStorage::k32BitSolo | RegStorage::kFloatingPoint |  5,
  rF6  = RegStorage::k32BitSolo | RegStorage::kFloatingPoint |  6,
  rF7  = RegStorage::k32BitSolo | RegStorage::kFloatingPoint |  7,
  rF8  = RegStorage::k32BitSolo | RegStorage::kFloatingPoint |  8,
  rF9  = RegStorage::k32BitSolo | RegStorage::kFloatingPoint |  9,
  rF10 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 10,
  rF11 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 11,
  rF12 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 12,
  rF13 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 13,
  rF14 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 14,
  rF15 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 15,

  rF16 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 16,
  rF17 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 17,
  rF18 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 18,
  rF19 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 19,
  rF20 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 20,
  rF21 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 21,
  rF22 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 22,
  rF23 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 23,
  rF24 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 24,
  rF25 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 25,
  rF26 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 26,
  rF27 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 27,
  rF28 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 28,
  rF29 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 29,
  rF30 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 30,
  rF31 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 31,

#if 0
  /*
   * TODO: The shared resource mask doesn't have enough bit positions to describe all
   * MIPS registers.  Expand it and enable use of fp registers 16 through 31.
   */
  rF16 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 16,
  rF17 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 17,
  rF18 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 18,
  rF19 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 19,
  rF20 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 20,
  rF21 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 21,
  rF22 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 22,
  rF23 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 23,
  rF24 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 24,
  rF25 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 25,
  rF26 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 26,
  rF27 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 27,
  rF28 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 28,
  rF29 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 29,
  rF30 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 30,
  rF31 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 31,
#endif
  // Double precision registers where the FPU is in 32-bit mode.
  rD0_fr0  = RegStorage::k64BitSolo | RegStorage::kFloatingPoint |  0,
  rD1_fr0  = RegStorage::k64BitSolo | RegStorage::kFloatingPoint |  2,
  rD2_fr0  = RegStorage::k64BitSolo | RegStorage::kFloatingPoint |  4,
  rD3_fr0  = RegStorage::k64BitSolo | RegStorage::kFloatingPoint |  6,
  rD4_fr0  = RegStorage::k64BitSolo | RegStorage::kFloatingPoint |  8,
  rD5_fr0  = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 10,
  rD6_fr0  = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 12,
  rD7_fr0  = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 14,
#if 0  // TODO: expand resource mask to enable use of all MIPS fp registers.
  rD8_fr0  = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 16,
  rD9_fr0  = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 18,
  rD10_fr0 = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 20,
  rD11_fr0 = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 22,
  rD12_fr0 = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 24,
  rD13_fr0 = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 26,
  rD14_fr0 = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 28,
  rD15_fr0 = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 30,
#endif
  // Double precision registers where the FPU is in 64-bit mode.
  rD0_fr1  = RegStorage::k64BitSolo | RegStorage::kFloatingPoint |  0,
  rD1_fr1  = RegStorage::k64BitSolo | RegStorage::kFloatingPoint |  2,
  rD2_fr1  = RegStorage::k64BitSolo | RegStorage::kFloatingPoint |  4,
  rD3_fr1  = RegStorage::k64BitSolo | RegStorage::kFloatingPoint |  6,
  rD4_fr1  = RegStorage::k64BitSolo | RegStorage::kFloatingPoint |  8,
  rD5_fr1  = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 10,
  rD6_fr1  = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 12,
  rD7_fr1  = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 14,
#if 0  // TODO: expand resource mask to enable use of all MIPS fp registers.
  rD8_fr1  = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 16,
  rD9_fr1  = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 18,
  rD10_fr1 = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 20,
  rD11_fr1 = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 22,
  rD12_fr1 = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 24,
  rD13_fr1 = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 26,
  rD14_fr1 = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 28,
  rD15_fr1 = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 30,
#endif

  rD0  = RegStorage::k64BitSolo | RegStorage::kFloatingPoint |  0,
  rD1  = RegStorage::k64BitSolo | RegStorage::kFloatingPoint |  1,
  rD2  = RegStorage::k64BitSolo | RegStorage::kFloatingPoint |  2,
  rD3  = RegStorage::k64BitSolo | RegStorage::kFloatingPoint |  3,
  rD4  = RegStorage::k64BitSolo | RegStorage::kFloatingPoint |  4,
  rD5  = RegStorage::k64BitSolo | RegStorage::kFloatingPoint |  5,
  rD6  = RegStorage::k64BitSolo | RegStorage::kFloatingPoint |  6,
  rD7  = RegStorage::k64BitSolo | RegStorage::kFloatingPoint |  7,
  rD8  = RegStorage::k64BitSolo | RegStorage::kFloatingPoint |  8,
  rD9  = RegStorage::k64BitSolo | RegStorage::kFloatingPoint |  9,
  rD10 = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 10,
  rD11 = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 11,
  rD12 = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 12,
  rD13 = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 13,
  rD14 = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 14,
  rD15 = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 15,
  rD16 = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 16,
  rD17 = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 17,
  rD18 = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 18,
  rD19 = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 19,
  rD20 = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 20,
  rD21 = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 21,
  rD22 = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 22,
  rD23 = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 23,
  rD24 = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 24,
  rD25 = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 25,
  rD26 = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 26,
  rD27 = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 27,
  rD28 = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 28,
  rD29 = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 29,
  rD30 = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 30,
  rD31 = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 31,
};

constexpr RegStorage rs_rZERO(RegStorage::kValid | rZERO);
constexpr RegStorage rs_rAT(RegStorage::kValid | rAT);
constexpr RegStorage rs_rV0(RegStorage::kValid | rV0);
constexpr RegStorage rs_rV1(RegStorage::kValid | rV1);
constexpr RegStorage rs_rA0(RegStorage::kValid | rA0);
constexpr RegStorage rs_rA1(RegStorage::kValid | rA1);
constexpr RegStorage rs_rA2(RegStorage::kValid | rA2);
constexpr RegStorage rs_rA3(RegStorage::kValid | rA3);
constexpr RegStorage rs_rT0_32(RegStorage::kValid | rT0_32);
constexpr RegStorage rs_rA4 = rs_rT0_32;
constexpr RegStorage rs_rT1_32(RegStorage::kValid | rT1_32);
constexpr RegStorage rs_rA5 = rs_rT1_32;
constexpr RegStorage rs_rT2_32(RegStorage::kValid | rT2_32);
constexpr RegStorage rs_rA6 = rs_rT2_32;
constexpr RegStorage rs_rT3_32(RegStorage::kValid | rT3_32);
constexpr RegStorage rs_rA7 = rs_rT3_32;
constexpr RegStorage rs_rT4_32(RegStorage::kValid | rT4_32);
constexpr RegStorage rs_rT0 = rs_rT4_32;
constexpr RegStorage rs_rT5_32(RegStorage::kValid | rT5_32);
constexpr RegStorage rs_rT1 = rs_rT5_32;
constexpr RegStorage rs_rT6_32(RegStorage::kValid | rT6_32);
constexpr RegStorage rs_rT2 = rs_rT6_32;
constexpr RegStorage rs_rT7_32(RegStorage::kValid | rT7_32);
constexpr RegStorage rs_rT3 = rs_rT7_32;
constexpr RegStorage rs_rS0(RegStorage::kValid | rS0);
constexpr RegStorage rs_rS1(RegStorage::kValid | rS1);
constexpr RegStorage rs_rS2(RegStorage::kValid | rS2);
constexpr RegStorage rs_rS3(RegStorage::kValid | rS3);
constexpr RegStorage rs_rS4(RegStorage::kValid | rS4);
constexpr RegStorage rs_rS5(RegStorage::kValid | rS5);
constexpr RegStorage rs_rS6(RegStorage::kValid | rS6);
constexpr RegStorage rs_rS7(RegStorage::kValid | rS7);
constexpr RegStorage rs_rT8(RegStorage::kValid | rT8);
constexpr RegStorage rs_rT9(RegStorage::kValid | rT9);
constexpr RegStorage rs_rK0(RegStorage::kValid | rK0);
constexpr RegStorage rs_rK1(RegStorage::kValid | rK1);
constexpr RegStorage rs_rGP(RegStorage::kValid | rGP);
constexpr RegStorage rs_rSP(RegStorage::kValid | rSP);
constexpr RegStorage rs_rFP(RegStorage::kValid | rFP);
constexpr RegStorage rs_rRA(RegStorage::kValid | rRA);

constexpr RegStorage rs_rZEROd(RegStorage::kValid | rZEROd);
constexpr RegStorage rs_rATd(RegStorage::kValid | rATd);
constexpr RegStorage rs_rV0d(RegStorage::kValid | rV0d);
constexpr RegStorage rs_rV1d(RegStorage::kValid | rV1d);
constexpr RegStorage rs_rA0d(RegStorage::kValid | rA0d);
constexpr RegStorage rs_rA1d(RegStorage::kValid | rA1d);
constexpr RegStorage rs_rA2d(RegStorage::kValid | rA2d);
constexpr RegStorage rs_rA3d(RegStorage::kValid | rA3d);
constexpr RegStorage rs_rA4d(RegStorage::kValid | rA4d);
constexpr RegStorage rs_rA5d(RegStorage::kValid | rA5d);
constexpr RegStorage rs_rA6d(RegStorage::kValid | rA6d);
constexpr RegStorage rs_rA7d(RegStorage::kValid | rA7d);
constexpr RegStorage rs_rT0d(RegStorage::kValid | rT0d);
constexpr RegStorage rs_rT1d(RegStorage::kValid | rT1d);
constexpr RegStorage rs_rT2d(RegStorage::kValid | rT2d);
constexpr RegStorage rs_rT3d(RegStorage::kValid | rT3d);
constexpr RegStorage rs_rS0d(RegStorage::kValid | rS0d);
constexpr RegStorage rs_rS1d(RegStorage::kValid | rS1d);
constexpr RegStorage rs_rS2d(RegStorage::kValid | rS2d);
constexpr RegStorage rs_rS3d(RegStorage::kValid | rS3d);
constexpr RegStorage rs_rS4d(RegStorage::kValid | rS4d);
constexpr RegStorage rs_rS5d(RegStorage::kValid | rS5d);
constexpr RegStorage rs_rS6d(RegStorage::kValid | rS6d);
constexpr RegStorage rs_rS7d(RegStorage::kValid | rS7d);
constexpr RegStorage rs_rT8d(RegStorage::kValid | rT8d);
constexpr RegStorage rs_rT9d(RegStorage::kValid | rT9d);
constexpr RegStorage rs_rK0d(RegStorage::kValid | rK0d);
constexpr RegStorage rs_rK1d(RegStorage::kValid | rK1d);
constexpr RegStorage rs_rGPd(RegStorage::kValid | rGPd);
constexpr RegStorage rs_rSPd(RegStorage::kValid | rSPd);
constexpr RegStorage rs_rFPd(RegStorage::kValid | rFPd);
constexpr RegStorage rs_rRAd(RegStorage::kValid | rRAd);

constexpr RegStorage rs_rF0(RegStorage::kValid | rF0);
constexpr RegStorage rs_rF1(RegStorage::kValid | rF1);
constexpr RegStorage rs_rF2(RegStorage::kValid | rF2);
constexpr RegStorage rs_rF3(RegStorage::kValid | rF3);
constexpr RegStorage rs_rF4(RegStorage::kValid | rF4);
constexpr RegStorage rs_rF5(RegStorage::kValid | rF5);
constexpr RegStorage rs_rF6(RegStorage::kValid | rF6);
constexpr RegStorage rs_rF7(RegStorage::kValid | rF7);
constexpr RegStorage rs_rF8(RegStorage::kValid | rF8);
constexpr RegStorage rs_rF9(RegStorage::kValid | rF9);
constexpr RegStorage rs_rF10(RegStorage::kValid | rF10);
constexpr RegStorage rs_rF11(RegStorage::kValid | rF11);
constexpr RegStorage rs_rF12(RegStorage::kValid | rF12);
constexpr RegStorage rs_rF13(RegStorage::kValid | rF13);
constexpr RegStorage rs_rF14(RegStorage::kValid | rF14);
constexpr RegStorage rs_rF15(RegStorage::kValid | rF15);

constexpr RegStorage rs_rF16(RegStorage::kValid | rF16);
constexpr RegStorage rs_rF17(RegStorage::kValid | rF17);
constexpr RegStorage rs_rF18(RegStorage::kValid | rF18);
constexpr RegStorage rs_rF19(RegStorage::kValid | rF19);
constexpr RegStorage rs_rF20(RegStorage::kValid | rF20);
constexpr RegStorage rs_rF21(RegStorage::kValid | rF21);
constexpr RegStorage rs_rF22(RegStorage::kValid | rF22);
constexpr RegStorage rs_rF23(RegStorage::kValid | rF23);
constexpr RegStorage rs_rF24(RegStorage::kValid | rF24);
constexpr RegStorage rs_rF25(RegStorage::kValid | rF25);
constexpr RegStorage rs_rF26(RegStorage::kValid | rF26);
constexpr RegStorage rs_rF27(RegStorage::kValid | rF27);
constexpr RegStorage rs_rF28(RegStorage::kValid | rF28);
constexpr RegStorage rs_rF29(RegStorage::kValid | rF29);
constexpr RegStorage rs_rF30(RegStorage::kValid | rF30);
constexpr RegStorage rs_rF31(RegStorage::kValid | rF31);

constexpr RegStorage rs_rD0_fr0(RegStorage::kValid | rD0_fr0);
constexpr RegStorage rs_rD1_fr0(RegStorage::kValid | rD1_fr0);
constexpr RegStorage rs_rD2_fr0(RegStorage::kValid | rD2_fr0);
constexpr RegStorage rs_rD3_fr0(RegStorage::kValid | rD3_fr0);
constexpr RegStorage rs_rD4_fr0(RegStorage::kValid | rD4_fr0);
constexpr RegStorage rs_rD5_fr0(RegStorage::kValid | rD5_fr0);
constexpr RegStorage rs_rD6_fr0(RegStorage::kValid | rD6_fr0);
constexpr RegStorage rs_rD7_fr0(RegStorage::kValid | rD7_fr0);

constexpr RegStorage rs_rD0_fr1(RegStorage::kValid | rD0_fr1);
constexpr RegStorage rs_rD1_fr1(RegStorage::kValid | rD1_fr1);
constexpr RegStorage rs_rD2_fr1(RegStorage::kValid | rD2_fr1);
constexpr RegStorage rs_rD3_fr1(RegStorage::kValid | rD3_fr1);
constexpr RegStorage rs_rD4_fr1(RegStorage::kValid | rD4_fr1);
constexpr RegStorage rs_rD5_fr1(RegStorage::kValid | rD5_fr1);
constexpr RegStorage rs_rD6_fr1(RegStorage::kValid | rD6_fr1);
constexpr RegStorage rs_rD7_fr1(RegStorage::kValid | rD7_fr1);

constexpr RegStorage rs_rD0(RegStorage::kValid | rD0);
constexpr RegStorage rs_rD1(RegStorage::kValid | rD1);
constexpr RegStorage rs_rD2(RegStorage::kValid | rD2);
constexpr RegStorage rs_rD3(RegStorage::kValid | rD3);
constexpr RegStorage rs_rD4(RegStorage::kValid | rD4);
constexpr RegStorage rs_rD5(RegStorage::kValid | rD5);
constexpr RegStorage rs_rD6(RegStorage::kValid | rD6);
constexpr RegStorage rs_rD7(RegStorage::kValid | rD7);
constexpr RegStorage rs_rD8(RegStorage::kValid | rD8);
constexpr RegStorage rs_rD9(RegStorage::kValid | rD9);
constexpr RegStorage rs_rD10(RegStorage::kValid | rD10);
constexpr RegStorage rs_rD11(RegStorage::kValid | rD11);
constexpr RegStorage rs_rD12(RegStorage::kValid | rD12);
constexpr RegStorage rs_rD13(RegStorage::kValid | rD13);
constexpr RegStorage rs_rD14(RegStorage::kValid | rD14);
constexpr RegStorage rs_rD15(RegStorage::kValid | rD15);
constexpr RegStorage rs_rD16(RegStorage::kValid | rD16);
constexpr RegStorage rs_rD17(RegStorage::kValid | rD17);
constexpr RegStorage rs_rD18(RegStorage::kValid | rD18);
constexpr RegStorage rs_rD19(RegStorage::kValid | rD19);
constexpr RegStorage rs_rD20(RegStorage::kValid | rD20);
constexpr RegStorage rs_rD21(RegStorage::kValid | rD21);
constexpr RegStorage rs_rD22(RegStorage::kValid | rD22);
constexpr RegStorage rs_rD23(RegStorage::kValid | rD23);
constexpr RegStorage rs_rD24(RegStorage::kValid | rD24);
constexpr RegStorage rs_rD25(RegStorage::kValid | rD25);
constexpr RegStorage rs_rD26(RegStorage::kValid | rD26);
constexpr RegStorage rs_rD27(RegStorage::kValid | rD27);
constexpr RegStorage rs_rD28(RegStorage::kValid | rD28);
constexpr RegStorage rs_rD29(RegStorage::kValid | rD29);
constexpr RegStorage rs_rD30(RegStorage::kValid | rD30);
constexpr RegStorage rs_rD31(RegStorage::kValid | rD31);

// RegisterLocation templates return values (r_V0, or r_V0/r_V1).
const RegLocation mips_loc_c_return
    {kLocPhysReg, 0, 0, 0, 0, 0, 0, 0, 1,
     RegStorage(RegStorage::k32BitSolo, rV0), INVALID_SREG, INVALID_SREG};
const RegLocation mips64_loc_c_return_ref
    {kLocPhysReg, 0, 0, 0, 0, 0, 1, 0, 1,
     RegStorage(RegStorage::k64BitSolo, rV0d), INVALID_SREG, INVALID_SREG};
const RegLocation mips_loc_c_return_wide
    {kLocPhysReg, 1, 0, 0, 0, 0, 0, 0, 1,
     RegStorage(RegStorage::k64BitPair, rV0, rV1), INVALID_SREG, INVALID_SREG};
const RegLocation mips64_loc_c_return_wide
    {kLocPhysReg, 1, 0, 0, 0, 0, 0, 0, 1,
     RegStorage(RegStorage::k64BitSolo, rV0d), INVALID_SREG, INVALID_SREG};
const RegLocation mips_loc_c_return_float
    {kLocPhysReg, 0, 0, 0, 1, 0, 0, 0, 1,
     RegStorage(RegStorage::k32BitSolo, rF0), INVALID_SREG, INVALID_SREG};
// FIXME: move MIPS to k64Bitsolo for doubles
const RegLocation mips_loc_c_return_double_fr0
    {kLocPhysReg, 1, 0, 0, 1, 0, 0, 0, 1,
     RegStorage(RegStorage::k64BitPair, rF0, rF1), INVALID_SREG, INVALID_SREG};
const RegLocation mips_loc_c_return_double_fr1
    {kLocPhysReg, 1, 0, 0, 1, 0, 0, 0, 1,
     RegStorage(RegStorage::k64BitSolo, rF0), INVALID_SREG, INVALID_SREG};
const RegLocation mips64_loc_c_return_double
    {kLocPhysReg, 1, 0, 0, 1, 0, 0, 0, 1,
     RegStorage(RegStorage::k64BitSolo, rD0), INVALID_SREG, INVALID_SREG};

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
 * The following enum defines the list of supported mips instructions by the
 * assembler. Their corresponding EncodingMap positions will be defined in
 * assemble_mips.cc.
 */
enum MipsOpCode {
  kMipsFirst = 0,
  // The following are common mips32r2, mips32r6 and mips64r6 instructions.
  kMips32BitData = kMipsFirst,  // data [31..0].
  kMipsAddiu,      // addiu t,s,imm16 [001001] s[25..21] t[20..16] imm16[15..0].
  kMipsAddu,       // add d,s,t [000000] s[25..21] t[20..16] d[15..11] [00000100001].
  kMipsAnd,        // and d,s,t [000000] s[25..21] t[20..16] d[15..11] [00000100100].
  kMipsAndi,       // andi t,s,imm16 [001100] s[25..21] t[20..16] imm16[15..0].
  kMipsB,          // b o   [0001000000000000] o[15..0].
  kMipsBal,        // bal o [0000010000010001] o[15..0].
  // NOTE : the code tests the range kMipsBeq thru kMipsBne, so adding an instruction in this
  // range may require updates.
  kMipsBeq,        // beq s,t,o [000100] s[25..21] t[20..16] o[15..0].
  kMipsBeqz,       // beqz s,o [000100] s[25..21] [00000] o[15..0].
  kMipsBgez,       // bgez s,o [000001] s[25..21] [00001] o[15..0].
  kMipsBgtz,       // bgtz s,o [000111] s[25..21] [00000] o[15..0].
  kMipsBlez,       // blez s,o [000110] s[25..21] [00000] o[15..0].
  kMipsBltz,       // bltz s,o [000001] s[25..21] [00000] o[15..0].
  kMipsBnez,       // bnez s,o [000101] s[25..21] [00000] o[15..0].
  kMipsBne,        // bne s,t,o [000101] s[25..21] t[20..16] o[15..0].
  kMipsExt,        // ext t,s,p,z [011111] s[25..21] t[20..16] z[15..11] p[10..6] [000000].
  kMipsFaddd,      // add.d d,s,t [01000110001] t[20..16] s[15..11] d[10..6] [000000].
  kMipsFadds,      // add.s d,s,t [01000110000] t[20..16] s[15..11] d[10..6] [000000].
  kMipsFsubd,      // sub.d d,s,t [01000110001] t[20..16] s[15..11] d[10..6] [000001].
  kMipsFsubs,      // sub.s d,s,t [01000110000] t[20..16] s[15..11] d[10..6] [000001].
  kMipsFdivd,      // div.d d,s,t [01000110001] t[20..16] s[15..11] d[10..6] [000011].
  kMipsFdivs,      // div.s d,s,t [01000110000] t[20..16] s[15..11] d[10..6] [000011].
  kMipsFmuld,      // mul.d d,s,t [01000110000] t[20..16] s[15..11] d[10..6] [000010].
  kMipsFmuls,      // mul.s d,s,t [01000110001] t[20..16] s[15..11] d[10..6] [000010].
  kMipsFcvtsd,     // cvt.s.d d,s [01000110001] [00000] s[15..11] d[10..6] [100000].
  kMipsFcvtsw,     // cvt.s.w d,s [01000110100] [00000] s[15..11] d[10..6] [100000].
  kMipsFcvtds,     // cvt.d.s d,s [01000110000] [00000] s[15..11] d[10..6] [100001].
  kMipsFcvtdw,     // cvt.d.w d,s [01000110100] [00000] s[15..11] d[10..6] [100001].
  kMipsFcvtwd,     // cvt.w.d d,s [01000110001] [00000] s[15..11] d[10..6] [100100].
  kMipsFcvtws,     // cvt.w.s d,s [01000110000] [00000] s[15..11] d[10..6] [100100].
  kMipsFmovd,      // mov.d d,s [01000110001] [00000] s[15..11] d[10..6] [000110].
  kMipsFmovs,      // mov.s d,s [01000110000] [00000] s[15..11] d[10..6] [000110].
  kMipsFnegd,      // neg.d d,s [01000110001] [00000] s[15..11] d[10..6] [000111].
  kMipsFnegs,      // neg.s d,s [01000110000] [00000] s[15..11] d[10..6] [000111].
  kMipsFldc1,      // ldc1 t,o(b) [110101] b[25..21] t[20..16] o[15..0].
  kMipsFlwc1,      // lwc1 t,o(b) [110001] b[25..21] t[20..16] o[15..0].
  kMipsFsdc1,      // sdc1 t,o(b) [111101] b[25..21] t[20..16] o[15..0].
  kMipsFswc1,      // swc1 t,o(b) [111001] b[25..21] t[20..16] o[15..0].
  kMipsJal,        // jal t [000011] t[25..0].
  kMipsJalr,       // jalr d,s [000000] s[25..21] [00000] d[15..11] hint[10..6] [001001].
  kMipsJr,         // jr s [000000] s[25..21] [0000000000] hint[10..6] [001000].
  kMipsLahi,       // lui t,imm16 [00111100000] t[20..16] imm16[15..0] load addr hi.
  kMipsLalo,       // ori t,s,imm16 [001001] s[25..21] t[20..16] imm16[15..0] load addr lo.
  kMipsLui,        // lui t,imm16 [00111100000] t[20..16] imm16[15..0].
  kMipsLb,         // lb t,o(b) [100000] b[25..21] t[20..16] o[15..0].
  kMipsLbu,        // lbu t,o(b) [100100] b[25..21] t[20..16] o[15..0].
  kMipsLh,         // lh t,o(b) [100001] b[25..21] t[20..16] o[15..0].
  kMipsLhu,        // lhu t,o(b) [100101] b[25..21] t[20..16] o[15..0].
  kMipsLw,         // lw t,o(b) [100011] b[25..21] t[20..16] o[15..0].
  kMipsMove,       // move d,s [000000] s[25..21] [00000] d[15..11] [00000100101].
  kMipsMfc1,       // mfc1 t,s [01000100000] t[20..16] s[15..11] [00000000000].
  kMipsMtc1,       // mtc1 t,s [01000100100] t[20..16] s[15..11] [00000000000].
  kMipsMfhc1,      // mfhc1 t,s [01000100011] t[20..16] s[15..11] [00000000000].
  kMipsMthc1,      // mthc1 t,s [01000100111] t[20..16] s[15..11] [00000000000].
  kMipsNop,        // nop [00000000000000000000000000000000].
  kMipsNor,        // nor d,s,t [000000] s[25..21] t[20..16] d[15..11] [00000100111].
  kMipsOr,         // or d,s,t [000000] s[25..21] t[20..16] d[15..11] [00000100101].
  kMipsOri,        // ori t,s,imm16 [001001] s[25..21] t[20..16] imm16[15..0].
  kMipsPref,       // pref h,o(b) [101011] b[25..21] h[20..16] o[15..0].
  kMipsSb,         // sb t,o(b) [101000] b[25..21] t[20..16] o[15..0].
  kMipsSeb,        // seb d,t [01111100000] t[20..16] d[15..11] [10000100000].
  kMipsSeh,        // seh d,t [01111100000] t[20..16] d[15..11] [11000100000].
  kMipsSh,         // sh t,o(b) [101001] b[25..21] t[20..16] o[15..0].
  kMipsSll,        // sll d,t,a [00000000000] t[20..16] d[15..11] a[10..6] [000000].
  kMipsSllv,       // sllv d,t,s [000000] s[25..21] t[20..16] d[15..11] [00000000100].
  kMipsSlt,        // slt d,s,t [000000] s[25..21] t[20..16] d[15..11] [00000101010].
  kMipsSlti,       // slti t,s,imm16 [001010] s[25..21] t[20..16] imm16[15..0].
  kMipsSltu,       // sltu d,s,t [000000] s[25..21] t[20..16] d[15..11] [00000101011].
  kMipsSra,        // sra d,s,imm5 [00000000000] t[20..16] d[15..11] imm5[10..6] [000011].
  kMipsSrav,       // srav d,t,s [000000] s[25..21] t[20..16] d[15..11] [00000000111].
  kMipsSrl,        // srl d,t,a [00000000000] t[20..16] d[20..16] a[10..6] [000010].
  kMipsSrlv,       // srlv d,t,s [000000] s[25..21] t[20..16] d[15..11] [00000000110].
  kMipsSubu,       // subu d,s,t [000000] s[25..21] t[20..16] d[15..11] [00000100011].
  kMipsSw,         // sw t,o(b) [101011] b[25..21] t[20..16] o[15..0].
  kMipsSync,       // sync kind [000000] [0000000000000000] s[10..6] [001111].
  kMipsXor,        // xor d,s,t [000000] s[25..21] t[20..16] d[15..11] [00000100110].
  kMipsXori,       // xori t,s,imm16 [001110] s[25..21] t[20..16] imm16[15..0].

  // The following are mips32r2 instructions.
  kMipsR2Div,      // div s,t [000000] s[25..21] t[20..16] [0000000000011010].
  kMipsR2Mul,      // mul d,s,t [011100] s[25..21] t[20..16] d[15..11] [00000000010].
  kMipsR2Mfhi,     // mfhi d [0000000000000000] d[15..11] [00000010000].
  kMipsR2Mflo,     // mflo d [0000000000000000] d[15..11] [00000010010].
  kMipsR2Movz,     // movz d,s,t [000000] s[25..21] t[20..16] d[15..11] [00000001010].

  // The following are mips32r6 and mips64r6 instructions.
  kMipsR6Div,      // div d,s,t [000000] s[25..21] t[20..16] d[15..11] [00010011010].
  kMipsR6Mod,      // mod d,s,t [000000] s[25..21] t[20..16] d[15..11] [00011011010].
  kMipsR6Mul,      // mul d,s,t [000000] s[25..21] t[20..16] d[15..11] [00010011000].

  // The following are mips64r6 instructions.
  kMips64Daddiu,   // daddiu t,s,imm16 [011001] s[25..21] t[20..16] imm16[15..11].
  kMips64Daddu,    // daddu d,s,t [000000] s[25..21] t[20..16] d[15..11] [00000101101].
  kMips64Dahi,     // dahi s,imm16 [000001] s[25..21] [00110] imm16[15..11].
  kMips64Dati,     // dati s,imm16 [000001] s[25..21] [11110] imm16[15..11].
  kMips64Daui,     // daui t,s,imm16 [011101] s[25..21] t[20..16] imm16[15..11].
  kMips64Ddiv,     // ddiv  d,s,t [000000] s[25..21] t[20..16] d[15..11] [00010011110].
  kMips64Dmod,     // dmod  d,s,t [000000] s[25..21] t[20..16] d[15..11] [00011011110].
  kMips64Dmul,     // dmul  d,s,t [000000] s[25..21] t[20..16] d[15..11] [00010011100].
  kMips64Dmfc1,    // dmfc1 t,s [01000100001] t[20..16] s[15..11] [00000000000].
  kMips64Dmtc1,    // dmtc1 t,s [01000100101] t[20..16] s[15..11] [00000000000].
  kMips64Drotr32,  // drotr32 d,t,a [00000000001] t[20..16] d[15..11] a[10..6] [111110].
  kMips64Dsll,     // dsll    d,t,a [00000000000] t[20..16] d[15..11] a[10..6] [111000].
  kMips64Dsll32,   // dsll32  d,t,a [00000000000] t[20..16] d[15..11] a[10..6] [111100].
  kMips64Dsrl,     // dsrl    d,t,a [00000000000] t[20..16] d[15..11] a[10..6] [111010].
  kMips64Dsrl32,   // dsrl32  d,t,a [00000000000] t[20..16] d[15..11] a[10..6] [111110].
  kMips64Dsra,     // dsra    d,t,a [00000000000] t[20..16] d[15..11] a[10..6] [111011].
  kMips64Dsra32,   // dsra32  d,t,a [00000000000] t[20..16] d[15..11] a[10..6] [111111].
  kMips64Dsllv,    // dsllv d,t,s [000000] s[25..21] t[20..16] d[15..11] [00000010100].
  kMips64Dsrlv,    // dsrlv d,t,s [000000] s[25..21] t[20..16] d[15..11] [00000010110].
  kMips64Dsrav,    // dsrav d,t,s [000000] s[25..21] t[20..16] d[15..11] [00000010111].
  kMips64Dsubu,    // dsubu d,s,t [000000] s[25..21] t[20..16] d[15..11] [00000101111].
  kMips64Ld,       // ld  t,o(b) [110111] b[25..21] t[20..16] o[15..0].
  kMips64Lwu,      // lwu t,o(b) [100111] b[25..21] t[20..16] o[15..0].
  kMips64Sd,       // sd t,o(b) [111111] b[25..21] t[20..16] o[15..0].

  // The following are pseudoinstructions.
  kMipsDelta,      // Psuedo for ori t, s, <label>-<label>.
  kMipsDeltaHi,    // Pseudo for lui t, high16(<label>-<label>).
  kMipsDeltaLo,    // Pseudo for ori t, s, low16(<label>-<label>).
  kMipsCurrPC,     // jal to .+8 to materialize pc.
  kMipsUndefined,  // undefined [011001xxxxxxxxxxxxxxxx].
  kMipsLast
};
std::ostream& operator<<(std::ostream& os, const MipsOpCode& rhs);

// Instruction assembly field_loc kind.
enum MipsEncodingKind {
  kFmtUnused,
  kFmtBitBlt,    // Bit string using end/start.
  kFmtDfp,       // Double FP reg.
  kFmtSfp,       // Single FP reg.
  kFmtBlt5_2,    // Same 5-bit field to 2 locations.
};
std::ostream& operator<<(std::ostream& os, const MipsEncodingKind& rhs);

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
