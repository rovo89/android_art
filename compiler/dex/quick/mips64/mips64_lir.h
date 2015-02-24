/*
 * Copyright (C) 2015 The Android Open Source Project
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

#ifndef ART_COMPILER_DEX_QUICK_MIPS64_MIPS64_LIR_H_
#define ART_COMPILER_DEX_QUICK_MIPS64_MIPS64_LIR_H_

#include "dex/reg_location.h"
#include "dex/reg_storage.h"

namespace art {

/*
 * Runtime register conventions.
 *
 * zero is always the value 0
 * at is scratch (normally used as temp reg by assembler)
 * v0, v1 are scratch (normally hold subroutine return values)
 * a0-a7 are scratch (normally hold subroutine arguments)
 * t0-t3, t8 are scratch
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
 * Trashed across C calls: at, v0-v1, a0-a7, t0-t3, t8-t9, gp, ra
 *
 * Floating pointer registers
 * NOTE: there are 32 fp registers.
 * f0-f31
 *
 * f0-f31 trashed across C calls
 *
 * For mips64 code use:
 *      a0-a7 to hold operands
 *      v0-v1 to hold results
 *      t0-t3, t8-t9 for temps
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


#define rARG0 rA0d
#define rs_rARG0 rs_rA0d
#define rARG1 rA1d
#define rs_rARG1 rs_rA1d
#define rARG2 rA2d
#define rs_rARG2 rs_rA2d
#define rARG3 rA3d
#define rs_rARG3 rs_rA3d
#define rARG4 rA4d
#define rs_rARG4 rs_rA4d
#define rARG5 rA5d
#define rs_rARG5 rs_rA5d
#define rARG6 rA6d
#define rs_rARG6 rs_rA6d
#define rARG7 rA7d
#define rs_rARG7 rs_rA7d
#define rRESULT0 rV0d
#define rs_rRESULT0 rs_rV0d
#define rRESULT1 rV1d
#define rs_rRESULT1 rs_rV1d

#define rFARG0 rF12
#define rs_rFARG0 rs_rF12
#define rFARG1 rF13
#define rs_rFARG1 rs_rF13
#define rFARG2 rF14
#define rs_rFARG2 rs_rF14
#define rFARG3 rF15
#define rs_rFARG3 rs_rF15
#define rFARG4 rF16
#define rs_rFARG4 rs_rF16
#define rFARG5 rF17
#define rs_rFARG5 rs_rF17
#define rFARG6 rF18
#define rs_rFARG6 rs_rF18
#define rFARG7 rF19
#define rs_rFARG7 rs_rF19
#define rFRESULT0 rF0
#define rs_rFRESULT0 rs_rF0
#define rFRESULT1 rF1
#define rs_rFRESULT1 rs_rF1

// Regs not used for Mips64.
#define rMIPS64_LR RegStorage::kInvalidRegVal
#define rMIPS64_PC RegStorage::kInvalidRegVal

enum Mips64ResourceEncodingPos {
  kMips64GPReg0   = 0,
  kMips64RegSP    = 29,
  kMips64RegLR    = 31,
  kMips64FPReg0   = 32,
  kMips64FPRegEnd = 64,
  kMips64RegPC    = kMips64FPRegEnd,
  kMips64RegEnd   = 65,
};

enum Mips64NativeRegisterPool {  // private marker to avoid generate-operator-out.py from processing.
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
  rA4    = RegStorage::k32BitSolo | RegStorage::kCoreRegister |  8,
  rA4d   = RegStorage::k64BitSolo | RegStorage::kCoreRegister |  8,
  rA5    = RegStorage::k32BitSolo | RegStorage::kCoreRegister |  9,
  rA5d   = RegStorage::k64BitSolo | RegStorage::kCoreRegister |  9,
  rA6    = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 10,
  rA6d   = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 10,
  rA7    = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 11,
  rA7d   = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 11,
  rT0    = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 12,
  rT0d   = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 12,
  rT1    = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 13,
  rT1d   = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 13,
  rT2    = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 14,
  rT2d   = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 14,
  rT3    = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 15,
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
constexpr RegStorage rs_rZEROd(RegStorage::kValid | rZEROd);
constexpr RegStorage rs_rAT(RegStorage::kValid | rAT);
constexpr RegStorage rs_rATd(RegStorage::kValid | rATd);
constexpr RegStorage rs_rV0(RegStorage::kValid | rV0);
constexpr RegStorage rs_rV0d(RegStorage::kValid | rV0d);
constexpr RegStorage rs_rV1(RegStorage::kValid | rV1);
constexpr RegStorage rs_rV1d(RegStorage::kValid | rV1d);
constexpr RegStorage rs_rA0(RegStorage::kValid | rA0);
constexpr RegStorage rs_rA0d(RegStorage::kValid | rA0d);
constexpr RegStorage rs_rA1(RegStorage::kValid | rA1);
constexpr RegStorage rs_rA1d(RegStorage::kValid | rA1d);
constexpr RegStorage rs_rA2(RegStorage::kValid | rA2);
constexpr RegStorage rs_rA2d(RegStorage::kValid | rA2d);
constexpr RegStorage rs_rA3(RegStorage::kValid | rA3);
constexpr RegStorage rs_rA3d(RegStorage::kValid | rA3d);
constexpr RegStorage rs_rA4(RegStorage::kValid | rA4);
constexpr RegStorage rs_rA4d(RegStorage::kValid | rA4d);
constexpr RegStorage rs_rA5(RegStorage::kValid | rA5);
constexpr RegStorage rs_rA5d(RegStorage::kValid | rA5d);
constexpr RegStorage rs_rA6(RegStorage::kValid | rA6);
constexpr RegStorage rs_rA6d(RegStorage::kValid | rA6d);
constexpr RegStorage rs_rA7(RegStorage::kValid | rA7);
constexpr RegStorage rs_rA7d(RegStorage::kValid | rA7d);
constexpr RegStorage rs_rT0(RegStorage::kValid | rT0);
constexpr RegStorage rs_rT0d(RegStorage::kValid | rT0d);
constexpr RegStorage rs_rT1(RegStorage::kValid | rT1);
constexpr RegStorage rs_rT1d(RegStorage::kValid | rT1d);
constexpr RegStorage rs_rT2(RegStorage::kValid | rT2);
constexpr RegStorage rs_rT2d(RegStorage::kValid | rT2d);
constexpr RegStorage rs_rT3(RegStorage::kValid | rT3);
constexpr RegStorage rs_rT3d(RegStorage::kValid | rT3d);
constexpr RegStorage rs_rS0(RegStorage::kValid | rS0);
constexpr RegStorage rs_rS0d(RegStorage::kValid | rS0d);
constexpr RegStorage rs_rS1(RegStorage::kValid | rS1);
constexpr RegStorage rs_rS1d(RegStorage::kValid | rS1d);
constexpr RegStorage rs_rS2(RegStorage::kValid | rS2);
constexpr RegStorage rs_rS2d(RegStorage::kValid | rS2d);
constexpr RegStorage rs_rS3(RegStorage::kValid | rS3);
constexpr RegStorage rs_rS3d(RegStorage::kValid | rS3d);
constexpr RegStorage rs_rS4(RegStorage::kValid | rS4);
constexpr RegStorage rs_rS4d(RegStorage::kValid | rS4d);
constexpr RegStorage rs_rS5(RegStorage::kValid | rS5);
constexpr RegStorage rs_rS5d(RegStorage::kValid | rS5d);
constexpr RegStorage rs_rS6(RegStorage::kValid | rS6);
constexpr RegStorage rs_rS6d(RegStorage::kValid | rS6d);
constexpr RegStorage rs_rS7(RegStorage::kValid | rS7);
constexpr RegStorage rs_rS7d(RegStorage::kValid | rS7d);
constexpr RegStorage rs_rT8(RegStorage::kValid | rT8);
constexpr RegStorage rs_rT8d(RegStorage::kValid | rT8d);
constexpr RegStorage rs_rT9(RegStorage::kValid | rT9);
constexpr RegStorage rs_rT9d(RegStorage::kValid | rT9d);
constexpr RegStorage rs_rK0(RegStorage::kValid | rK0);
constexpr RegStorage rs_rK0d(RegStorage::kValid | rK0d);
constexpr RegStorage rs_rK1(RegStorage::kValid | rK1);
constexpr RegStorage rs_rK1d(RegStorage::kValid | rK1d);
constexpr RegStorage rs_rGP(RegStorage::kValid | rGP);
constexpr RegStorage rs_rGPd(RegStorage::kValid | rGPd);
constexpr RegStorage rs_rSP(RegStorage::kValid | rSP);
constexpr RegStorage rs_rSPd(RegStorage::kValid | rSPd);
constexpr RegStorage rs_rFP(RegStorage::kValid | rFP);
constexpr RegStorage rs_rFPd(RegStorage::kValid | rFPd);
constexpr RegStorage rs_rRA(RegStorage::kValid | rRA);
constexpr RegStorage rs_rRAd(RegStorage::kValid | rRAd);

constexpr RegStorage rs_rMIPS64_LR(RegStorage::kInvalid);     // Not used for MIPS64.
constexpr RegStorage rs_rMIPS64_PC(RegStorage::kInvalid);     // Not used for MIPS64.
constexpr RegStorage rs_rMIPS64_COUNT(RegStorage::kInvalid);  // Not used for MIPS64.

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

// TODO: reduce/eliminate use of these.
#define rMIPS64_SUSPEND rS0d
#define rs_rMIPS64_SUSPEND rs_rS0d
#define rMIPS64_SELF rS1d
#define rs_rMIPS64_SELF rs_rS1d
#define rMIPS64_SP rSPd
#define rs_rMIPS64_SP rs_rSPd
#define rMIPS64_ARG0 rARG0
#define rs_rMIPS64_ARG0 rs_rARG0
#define rMIPS64_ARG1 rARG1
#define rs_rMIPS64_ARG1 rs_rARG1
#define rMIPS64_ARG2 rARG2
#define rs_rMIPS64_ARG2 rs_rARG2
#define rMIPS64_ARG3 rARG3
#define rs_rMIPS64_ARG3 rs_rARG3
#define rMIPS64_ARG4 rARG4
#define rs_rMIPS64_ARG4 rs_rARG4
#define rMIPS64_ARG5 rARG5
#define rs_rMIPS64_ARG5 rs_rARG5
#define rMIPS64_ARG6 rARG6
#define rs_rMIPS64_ARG6 rs_rARG6
#define rMIPS64_ARG7 rARG7
#define rs_rMIPS64_ARG7 rs_rARG7
#define rMIPS64_FARG0 rFARG0
#define rs_rMIPS64_FARG0 rs_rFARG0
#define rMIPS64_FARG1 rFARG1
#define rs_rMIPS64_FARG1 rs_rFARG1
#define rMIPS64_FARG2 rFARG2
#define rs_rMIPS64_FARG2 rs_rFARG2
#define rMIPS64_FARG3 rFARG3
#define rs_rMIPS64_FARG3 rs_rFARG3
#define rMIPS64_FARG4 rFARG4
#define rs_rMIPS64_FARG4 rs_rFARG4
#define rMIPS64_FARG5 rFARG5
#define rs_rMIPS64_FARG5 rs_rFARG5
#define rMIPS64_FARG6 rFARG6
#define rs_rMIPS64_FARG6 rs_rFARG6
#define rMIPS64_FARG7 rFARG7
#define rs_rMIPS64_FARG7 rs_rFARG7
#define rMIPS64_RET0 rRESULT0
#define rs_rMIPS64_RET0 rs_rRESULT0
#define rMIPS64_RET1 rRESULT1
#define rs_rMIPS64_RET1 rs_rRESULT1
#define rMIPS64_INVOKE_TGT rT9d
#define rs_rMIPS64_INVOKE_TGT rs_rT9d
#define rMIPS64_COUNT RegStorage::kInvalidRegVal

// RegisterLocation templates return values (r_V0).
const RegLocation mips64_loc_c_return
    {kLocPhysReg, 0, 0, 0, 0, 0, 0, 0, 1,
     RegStorage(RegStorage::k32BitSolo, rV0), INVALID_SREG, INVALID_SREG};
const RegLocation mips64_loc_c_return_ref
    {kLocPhysReg, 0, 0, 0, 0, 0, 1, 0, 1,
     RegStorage(RegStorage::k64BitSolo, rV0d), INVALID_SREG, INVALID_SREG};
const RegLocation mips64_loc_c_return_wide
    {kLocPhysReg, 1, 0, 0, 0, 0, 0, 0, 1,
     RegStorage(RegStorage::k64BitSolo, rV0d), INVALID_SREG, INVALID_SREG};
const RegLocation mips64_loc_c_return_float
    {kLocPhysReg, 0, 0, 0, 1, 0, 0, 0, 1,
     RegStorage(RegStorage::k32BitSolo, rF0), INVALID_SREG, INVALID_SREG};
const RegLocation mips64_loc_c_return_double
    {kLocPhysReg, 1, 0, 0, 1, 0, 0, 0, 1,
     RegStorage(RegStorage::k64BitSolo, rD0), INVALID_SREG, INVALID_SREG};

enum Mips64ShiftEncodings {
  kMips64Lsl = 0x0,
  kMips64Lsr = 0x1,
  kMips64Asr = 0x2,
  kMips64Ror = 0x3
};

// MIPS64 sync kinds (Note: support for kinds other than kSYNC0 may not exist).
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
 * The following enum defines the list of supported Mips64 instructions by the
 * assembler. Their corresponding EncodingMap positions will be defined in
 * assemble_mips64.cc.
 */
enum Mips64OpCode {
  kMips64First = 0,
  kMips6432BitData = kMips64First,  // data [31..0].
  kMips64Addiu,      // addiu t,s,imm16 [001001] s[25..21] t[20..16] imm16[15..0].
  kMips64Addu,       // add d,s,t [000000] s[25..21] t[20..16] d[15..11] [00000100001].
  kMips64And,        // and d,s,t [000000] s[25..21] t[20..16] d[15..11] [00000100100].
  kMips64Andi,       // andi t,s,imm16 [001100] s[25..21] t[20..16] imm16[15..0].
  kMips64B,          // b o   [0001000000000000] o[15..0].
  kMips64Bal,        // bal o [0000010000010001] o[15..0].
  // NOTE: the code tests the range kMips64Beq thru kMips64Bne, so adding an instruction in this
  //       range may require updates.
  kMips64Beq,        // beq s,t,o [000100] s[25..21] t[20..16] o[15..0].
  kMips64Beqz,       // beqz s,o [000100] s[25..21] [00000] o[15..0].
  kMips64Bgez,       // bgez s,o [000001] s[25..21] [00001] o[15..0].
  kMips64Bgtz,       // bgtz s,o [000111] s[25..21] [00000] o[15..0].
  kMips64Blez,       // blez s,o [000110] s[25..21] [00000] o[15..0].
  kMips64Bltz,       // bltz s,o [000001] s[25..21] [00000] o[15..0].
  kMips64Bnez,       // bnez s,o [000101] s[25..21] [00000] o[15..0].
  kMips64Bne,        // bne s,t,o [000101] s[25..21] t[20..16] o[15..0].
  kMips64Break,      // break code [000000] code[25..6] [001101].
  kMips64Daddiu,     // daddiu t,s,imm16 [011001] s[25..21] t[20..16] imm16[15..11].
  kMips64Daddu,      // daddu d,s,t [000000] s[25..21] t[20..16] d[15..11] [00000101101].
  kMips64Dahi,       // dahi s,imm16 [000001] s[25..21] [00110] imm16[15..11].
  kMips64Dati,       // dati s,imm16 [000001] s[25..21] [11110] imm16[15..11].
  kMips64Daui,       // daui t,s,imm16 [011101] s[25..21] t[20..16] imm16[15..11].
  kMips64Ddiv,       // ddiv  d,s,t [000000] s[25..21] t[20..16] d[15..11] [00010011110].
  kMips64Div,        // div   d,s,t [000000] s[25..21] t[20..16] d[15..11] [00010011010].
  kMips64Dmod,       // dmod  d,s,t [000000] s[25..21] t[20..16] d[15..11] [00011011110].
  kMips64Dmul,       // dmul  d,s,t [000000] s[25..21] t[20..16] d[15..11] [00010011100].
  kMips64Dmfc1,      // dmfc1 t,s [01000100001] t[20..16] s[15..11] [00000000000].
  kMips64Dmtc1,      // dmtc1 t,s [01000100101] t[20..16] s[15..11] [00000000000].
  kMips64Drotr32,    // drotr32 d,t,a [00000000001] t[20..16] d[15..11] a[10..6] [111110].
  kMips64Dsll,       // dsll    d,t,a [00000000000] t[20..16] d[15..11] a[10..6] [111000].
  kMips64Dsll32,     // dsll32  d,t,a [00000000000] t[20..16] d[15..11] a[10..6] [111100].
  kMips64Dsrl,       // dsrl    d,t,a [00000000000] t[20..16] d[15..11] a[10..6] [111010].
  kMips64Dsrl32,     // dsrl32  d,t,a [00000000000] t[20..16] d[15..11] a[10..6] [111110].
  kMips64Dsra,       // dsra    d,t,a [00000000000] t[20..16] d[15..11] a[10..6] [111011].
  kMips64Dsra32,     // dsra32  d,t,a [00000000000] t[20..16] d[15..11] a[10..6] [111111].
  kMips64Dsllv,      // dsllv d,t,s [000000] s[25..21] t[20..16] d[15..11] [00000010100].
  kMips64Dsrlv,      // dsrlv d,t,s [000000] s[25..21] t[20..16] d[15..11] [00000010110].
  kMips64Dsrav,      // dsrav d,t,s [000000] s[25..21] t[20..16] d[15..11] [00000010111].
  kMips64Dsubu,      // dsubu d,s,t [000000] s[25..21] t[20..16] d[15..11] [00000101111].
  kMips64Ext,        // ext t,s,p,z [011111] s[25..21] t[20..16] z[15..11] p[10..6] [000000].
  kMips64Faddd,      // add.d d,s,t [01000110001] t[20..16] s[15..11] d[10..6] [000000].
  kMips64Fadds,      // add.s d,s,t [01000110000] t[20..16] s[15..11] d[10..6] [000000].
  kMips64Fdivd,      // div.d d,s,t [01000110001] t[20..16] s[15..11] d[10..6] [000011].
  kMips64Fdivs,      // div.s d,s,t [01000110000] t[20..16] s[15..11] d[10..6] [000011].
  kMips64Fmuld,      // mul.d d,s,t [01000110001] t[20..16] s[15..11] d[10..6] [000010].
  kMips64Fmuls,      // mul.s d,s,t [01000110000] t[20..16] s[15..11] d[10..6] [000010].
  kMips64Fsubd,      // sub.d d,s,t [01000110001] t[20..16] s[15..11] d[10..6] [000001].
  kMips64Fsubs,      // sub.s d,s,t [01000110000] t[20..16] s[15..11] d[10..6] [000001].
  kMips64Fcvtsd,     // cvt.s.d d,s [01000110001] [00000] s[15..11] d[10..6] [100000].
  kMips64Fcvtsw,     // cvt.s.w d,s [01000110100] [00000] s[15..11] d[10..6] [100000].
  kMips64Fcvtds,     // cvt.d.s d,s [01000110000] [00000] s[15..11] d[10..6] [100001].
  kMips64Fcvtdw,     // cvt.d.w d,s [01000110100] [00000] s[15..11] d[10..6] [100001].
  kMips64Fcvtws,     // cvt.w.d d,s [01000110000] [00000] s[15..11] d[10..6] [100100].
  kMips64Fcvtwd,     // cvt.w.d d,s [01000110001] [00000] s[15..11] d[10..6] [100100].
  kMips64Fmovd,      // mov.d d,s [01000110001] [00000] s[15..11] d[10..6] [000110].
  kMips64Fmovs,      // mov.s d,s [01000110000] [00000] s[15..11] d[10..6] [000110].
  kMips64Fnegd,      // neg.d d,s [01000110001] [00000] s[15..11] d[10..6] [000111].
  kMips64Fnegs,      // neg.s d,s [01000110000] [00000] s[15..11] d[10..6] [000111].
  kMips64Fldc1,      // ldc1 t,o(b) [110101] b[25..21] t[20..16] o[15..0].
  kMips64Flwc1,      // lwc1 t,o(b) [110001] b[25..21] t[20..16] o[15..0].
  kMips64Fsdc1,      // sdc1 t,o(b) [111101] b[25..21] t[20..16] o[15..0].
  kMips64Fswc1,      // swc1 t,o(b) [111001] b[25..21] t[20..16] o[15..0].
  kMips64Jal,        // jal t [000011] t[25..0].
  kMips64Jalr,       // jalr d,s [000000] s[25..21] [00000] d[15..11] hint[10..6] [001001].
  kMips64Lahi,       // lui t,imm16 [00111100000] t[20..16] imm16[15..0] load addr hi.
  kMips64Lalo,       // ori t,s,imm16 [001001] s[25..21] t[20..16] imm16[15..0] load addr lo.
  kMips64Lb,         // lb  t,o(b) [100000] b[25..21] t[20..16] o[15..0].
  kMips64Lbu,        // lbu t,o(b) [100100] b[25..21] t[20..16] o[15..0].
  kMips64Ld,         // ld  t,o(b) [110111] b[25..21] t[20..16] o[15..0].
  kMips64Lh,         // lh  t,o(b) [100001] b[25..21] t[20..16] o[15..0].
  kMips64Lhu,        // lhu t,o(b) [100101] b[25..21] t[20..16] o[15..0].
  kMips64Lui,        // lui t,imm16 [00111100000] t[20..16] imm16[15..0].
  kMips64Lw,         // lw  t,o(b) [100011] b[25..21] t[20..16] o[15..0].
  kMips64Lwu,        // lwu t,o(b) [100111] b[25..21] t[20..16] o[15..0].
  kMips64Mfc1,       // mfc1 t,s [01000100000] t[20..16] s[15..11] [00000000000].
  kMips64Mtc1,       // mtc1 t,s [01000100100] t[20..16] s[15..11] [00000000000].
  kMips64Move,       // move d,s [000000] s[25..21] [00000] d[15..11] [00000101101].
  kMips64Mod,        // mod d,s,t [000000] s[25..21] t[20..16] d[15..11] [00011011010].
  kMips64Mul,        // mul d,s,t [000000] s[25..21] t[20..16] d[15..11] [00010011000].
  kMips64Nop,        // nop [00000000000000000000000000000000].
  kMips64Nor,        // nor d,s,t [000000] s[25..21] t[20..16] d[15..11] [00000100111].
  kMips64Or,         // or  d,s,t [000000] s[25..21] t[20..16] d[15..11] [00000100101].
  kMips64Ori,        // ori t,s,imm16 [001001] s[25..21] t[20..16] imm16[15..0].
  kMips64Sb,         // sb t,o(b) [101000] b[25..21] t[20..16] o[15..0].
  kMips64Sd,         // sd t,o(b) [111111] b[25..21] t[20..16] o[15..0].
  kMips64Seb,        // seb d,t [01111100000] t[20..16] d[15..11] [10000100000].
  kMips64Seh,        // seh d,t [01111100000] t[20..16] d[15..11] [11000100000].
  kMips64Sh,         // sh t,o(b) [101001] b[25..21] t[20..16] o[15..0].
  kMips64Sll,        // sll d,t,a [00000000000] t[20..16] d[15..11] a[10..6] [000000].
  kMips64Sllv,       // sllv d,t,s [000000] s[25..21] t[20..16] d[15..11] [00000000100].
  kMips64Slt,        // slt d,s,t [000000] s[25..21] t[20..16] d[15..11] [00000101010].
  kMips64Slti,       // slti t,s,imm16 [001010] s[25..21] t[20..16] imm16[15..0].
  kMips64Sltu,       // sltu d,s,t [000000] s[25..21] t[20..16] d[15..11] [00000101011].
  kMips64Sra,        // sra d,s,imm5 [00000000000] t[20..16] d[15..11] imm5[10..6] [000011].
  kMips64Srav,       // srav d,t,s [000000] s[25..21] t[20..16] d[15..11] [00000000111].
  kMips64Srl,        // srl d,t,a [00000000000] t[20..16] d[20..16] a[10..6] [000010].
  kMips64Srlv,       // srlv d,t,s [000000] s[25..21] t[20..16] d[15..11] [00000000110].
  kMips64Subu,       // subu d,s,t [000000] s[25..21] t[20..16] d[15..11] [00000100011].
  kMips64Sw,         // sw t,o(b) [101011] b[25..21] t[20..16] o[15..0].
  kMips64Sync,       // sync kind [000000] [0000000000000000] s[10..6] [001111].
  kMips64Xor,        // xor d,s,t [000000] s[25..21] t[20..16] d[15..11] [00000100110].
  kMips64Xori,       // xori t,s,imm16 [001110] s[25..21] t[20..16] imm16[15..0].
  kMips64CurrPC,     // jal to .+8 to materialize pc.
  kMips64Delta,      // Psuedo for ori t, s, <label>-<label>.
  kMips64DeltaHi,    // Pseudo for lui t, high16(<label>-<label>).
  kMips64DeltaLo,    // Pseudo for ori t, s, low16(<label>-<label>).
  kMips64Undefined,  // undefined [011001xxxxxxxxxxxxxxxx].
  kMips64Last
};
std::ostream& operator<<(std::ostream& os, const Mips64OpCode& rhs);

// Instruction assembly field_loc kind.
enum Mips64EncodingKind {
  kFmtUnused,
  kFmtBitBlt,    // Bit string using end/start.
  kFmtDfp,       // Double FP reg.
  kFmtSfp,       // Single FP reg.
  kFmtBlt5_2,    // Same 5-bit field to 2 locations.
};
std::ostream& operator<<(std::ostream& os, const Mips64EncodingKind& rhs);

// Struct used to define the snippet positions for each MIPS64 opcode.
struct Mips64EncodingMap {
  uint32_t skeleton;
  struct {
    Mips64EncodingKind kind;
    int end;   // end for kFmtBitBlt, 1-bit slice end for FP regs.
    int start;  // start for kFmtBitBlt, 4-bit slice end for FP regs.
  } field_loc[4];
  Mips64OpCode opcode;
  uint64_t flags;
  const char *name;
  const char* fmt;
  int size;   // Note: size is in bytes.
};

extern Mips64EncodingMap EncodingMap[kMips64Last];

#define IS_UIMM16(v) ((0 <= (v)) && ((v) <= 65535))
#define IS_SIMM16(v) ((-32768 <= (v)) && ((v) <= 32766))
#define IS_SIMM16_2WORD(v) ((-32764 <= (v)) && ((v) <= 32763))  // 2 offsets must fit.

}  // namespace art

#endif  // ART_COMPILER_DEX_QUICK_MIPS64_MIPS64_LIR_H_
