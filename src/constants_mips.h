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

#ifndef ART_SRC_CONSTANTS_MIPS_H_
#define ART_SRC_CONSTANTS_MIPS_H_

#include <iosfwd>
#include "globals.h"
#include "logging.h"
#include "macros.h"

namespace art {
namespace mips {

enum Register {
  ZERO =  0,
  AT   =  1,
  V0   =  2,
  V1   =  3,
  A0   =  4,
  A1   =  5,
  A2   =  6,
  A3   =  7,
  T0   =  8,
  T1   =  9,
  T2   = 10,
  T3   = 11,
  T4   = 12,
  T5   = 13,
  T6   = 14,
  T7   = 15,
  S0   = 16,
  S1   = 17,
  S2   = 18,
  S3   = 19,
  S4   = 20,
  S5   = 21,
  S6   = 22,
  S7   = 23,
  T8   = 24,
  T9   = 25,
  K0   = 26,
  K1   = 27,
  GP   = 28,
  SP   = 29,
  FP   = 30,
  RA   = 31,
  kNumberOfCoreRegisters = 32,
  kNoRegister = -1  // Signals an illegal register.
};
std::ostream& operator<<(std::ostream& os, const Register& rhs);

// Values for single-precision floating point registers.
enum FRegister {
  F0  =  0,
  F1  =  1,
  F2  =  2,
  F3  =  3,
  F4  =  4,
  F5  =  5,
  F6  =  6,
  F7  =  7,
  F8  =  8,
  F9  =  9,
  F10 = 10,
  F11 = 11,
  F12 = 12,
  F13 = 13,
  F14 = 14,
  F15 = 15,
  F16 = 16,
  F17 = 17,
  F18 = 18,
  F19 = 19,
  F20 = 20,
  F21 = 21,
  F22 = 22,
  F23 = 23,
  F24 = 24,
  F25 = 25,
  F26 = 26,
  F27 = 27,
  F28 = 28,
  F29 = 29,
  F30 = 30,
  F31 = 31,
  kNumberOfFRegisters = 32,
  kNoFRegister = -1,
};
std::ostream& operator<<(std::ostream& os, const FRegister& rhs);

// Values for double-precision floating point registers.
enum DRegister {
  D0  =  0,
  D1  =  1,
  D2  =  2,
  D3  =  3,
  D4  =  4,
  D5  =  5,
  D6  =  6,
  D7  =  7,
  D8  =  8,
  D9  =  9,
  D10 = 10,
  D11 = 11,
  D12 = 12,
  D13 = 13,
  D14 = 14,
  D15 = 15,
  kNumberOfDRegisters = 16,
  kNumberOfOverlappingDRegisters = 16,
  kNoDRegister = -1,
};
std::ostream& operator<<(std::ostream& os, const DRegister& rhs);

// Constants used for the decoding or encoding of the individual fields of instructions.
enum InstructionFields {
  kOpcodeShift = 26,
  kOpcodeBits = 6,
  kRsShift = 21,
  kRsBits = 5,
  kRtShift = 16,
  kRtBits = 5,
  kRdShift = 11,
  kRdBits = 5,
  kShamtShift = 6,
  kShamtBits = 5,
  kFunctShift = 0,
  kFunctBits = 6,

  kFmtShift = 21,
  kFmtBits = 5,
  kFtShift = 16,
  kFtBits = 5,
  kFsShift = 11,
  kFsBits = 5,
  kFdShift = 6,
  kFdBits = 5,

  kBranchOffsetMask = 0x0000ffff,
  kJumpOffsetMask = 0x03ffffff,
};

enum ScaleFactor {
  TIMES_1 = 0,
  TIMES_2 = 1,
  TIMES_4 = 2,
  TIMES_8 = 3
};

class Instr {
 public:
  static const uint32_t kBreakPointInstruction = 0x0000000D;

  bool IsBreakPoint() {
    return ((*reinterpret_cast<const uint32_t*>(this)) & 0xFC0000CF) == kBreakPointInstruction;
  }

  // Instructions are read out of a code stream. The only way to get a
  // reference to an instruction is to convert a pointer. There is no way
  // to allocate or create instances of class Instr.
  // Use the At(pc) function to create references to Instr.
  static Instr* At(uintptr_t pc) { return reinterpret_cast<Instr*>(pc); }

 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(Instr);
};

}  // namespace mips
}  // namespace art

#endif  // ART_SRC_CONSTANTS_MIPS_H_
