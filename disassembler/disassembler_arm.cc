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

#include "disassembler_arm.h"

#include <inttypes.h>

#include <iostream>

#include "base/logging.h"
#include "base/stringprintf.h"
#include "thread.h"

namespace art {
namespace arm {

size_t DisassemblerArm::Dump(std::ostream& os, const uint8_t* begin) {
  if ((reinterpret_cast<intptr_t>(begin) & 1) == 0) {
    DumpArm(os, begin);
    return 4;
  } else {
    // remove thumb specifier bits
    begin = reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(begin) & ~1);
    return DumpThumb16(os, begin);
  }
}

void DisassemblerArm::Dump(std::ostream& os, const uint8_t* begin, const uint8_t* end) {
  if ((reinterpret_cast<intptr_t>(begin) & 1) == 0) {
    for (const uint8_t* cur = begin; cur < end; cur += 4) {
      DumpArm(os, cur);
    }
  } else {
    // remove thumb specifier bits
    begin = reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(begin) & ~1);
    end = reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(end) & ~1);
    for (const uint8_t* cur = begin; cur < end;) {
      cur += DumpThumb16(os, cur);
    }
  }
}

static const char* kConditionCodeNames[] = {
  "eq",  // 0000 - equal
  "ne",  // 0001 - not-equal
  "cs",  // 0010 - carry-set, greater than, equal or unordered
  "cc",  // 0011 - carry-clear, less than
  "mi",  // 0100 - minus, negative
  "pl",  // 0101 - plus, positive or zero
  "vs",  // 0110 - overflow
  "vc",  // 0111 - no overflow
  "hi",  // 1000 - unsigned higher
  "ls",  // 1001 - unsigned lower or same
  "ge",  // 1010 - signed greater than or equal
  "lt",  // 1011 - signed less than
  "gt",  // 1100 - signed greater than
  "le",  // 1101 - signed less than or equal
  "",    // 1110 - always
  "nv",  // 1111 - never (mostly obsolete, but might be a clue that we're mistranslating)
};

void DisassemblerArm::DumpCond(std::ostream& os, uint32_t cond) {
  if (cond < 15) {
    os << kConditionCodeNames[cond];
  } else {
    os << "Unexpected condition: " << cond;
  }
}

void DisassemblerArm::DumpMemoryDomain(std::ostream& os, uint32_t domain) {
  switch (domain) {
    case 0b1111: os << "sy"; break;
    case 0b1110: os << "st"; break;
    case 0b1011: os << "ish"; break;
    case 0b1010: os << "ishst"; break;
    case 0b0111: os << "nsh"; break;
    case 0b0110: os << "nshst"; break;
    case 0b0011: os << "osh"; break;
    case 0b0010: os << "oshst"; break;
  }
}

void DisassemblerArm::DumpBranchTarget(std::ostream& os, const uint8_t* instr_ptr, int32_t imm32) {
  os << StringPrintf("%+d (", imm32) << FormatInstructionPointer(instr_ptr + imm32) << ")";
}

static uint32_t ReadU16(const uint8_t* ptr) {
  return ptr[0] | (ptr[1] << 8);
}

static uint32_t ReadU32(const uint8_t* ptr) {
  return ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24);
}

static const char* kDataProcessingOperations[] = {
  "and", "eor", "sub", "rsb", "add", "adc", "sbc", "rsc",
  "tst", "teq", "cmp", "cmn", "orr", "mov", "bic", "mvn",
};

static const char* kThumbDataProcessingOperations[] = {
  "and", "eor", "lsl", "lsr", "asr", "adc", "sbc", "ror",
  "tst", "rsb", "cmp", "cmn", "orr", "mul", "bic", "mvn",
};

static const char* const kThumb2ShiftOperations[] = {
    "lsl", "lsr", "asr", "ror"
};

static const char* kThumbReverseOperations[] = {
    "rev", "rev16", "rbit", "revsh"
};

struct ArmRegister {
  explicit ArmRegister(uint32_t r) : r(r) { CHECK_LE(r, 15U); }
  ArmRegister(uint32_t instruction, uint32_t at_bit) : r((instruction >> at_bit) & 0xf) { CHECK_LE(r, 15U); }
  uint32_t r;
};
std::ostream& operator<<(std::ostream& os, const ArmRegister& r) {
  if (r.r == 13) {
    os << "sp";
  } else if (r.r == 14) {
    os << "lr";
  } else if (r.r == 15) {
    os << "pc";
  } else {
    os << "r" << r.r;
  }
  return os;
}

struct ThumbRegister : ArmRegister {
  ThumbRegister(uint16_t instruction, uint16_t at_bit) : ArmRegister((instruction >> at_bit) & 0x7) {}
};

struct Rm {
  explicit Rm(uint32_t instruction) : shift((instruction >> 4) & 0xff), rm(instruction & 0xf) {}
  uint32_t shift;
  ArmRegister rm;
};
std::ostream& operator<<(std::ostream& os, const Rm& r) {
  os << r.rm;
  if (r.shift != 0) {
    os << "-shift-" << r.shift;  // TODO
  }
  return os;
}

struct ShiftedImmediate {
  explicit ShiftedImmediate(uint32_t instruction) {
    uint32_t rotate = ((instruction >> 8) & 0xf);
    uint32_t imm = (instruction & 0xff);
    value = (imm >> (2 * rotate)) | (imm << (32 - (2 * rotate)));
  }
  uint32_t value;
};
std::ostream& operator<<(std::ostream& os, const ShiftedImmediate& rhs) {
  os << "#" << rhs.value;
  return os;
}

struct RegisterList {
  explicit RegisterList(uint32_t instruction) : register_list(instruction & 0xffff) {}
  uint32_t register_list;
};
std::ostream& operator<<(std::ostream& os, const RegisterList& rhs) {
  if (rhs.register_list == 0) {
    os << "<no register list?>";
    return os;
  }
  os << "{";
  bool first = true;
  for (size_t i = 0; i < 16; i++) {
    if ((rhs.register_list & (1 << i)) != 0) {
      if (first) {
        first = false;
      } else {
        os << ", ";
      }
      os << ArmRegister(i);
    }
  }
  os << "}";
  return os;
}

struct FpRegister {
  explicit FpRegister(uint32_t instr, uint16_t at_bit, uint16_t extra_at_bit) {
    size = (instr >> 8) & 1;
    uint32_t Vn = (instr >> at_bit) & 0xF;
    uint32_t N = (instr >> extra_at_bit) & 1;
    r = (size != 0 ? ((N << 4) | Vn) : ((Vn << 1) | N));
  }
  explicit FpRegister(uint32_t instr, uint16_t at_bit, uint16_t extra_at_bit,
                      uint32_t forced_size) {
    size = forced_size;
    uint32_t Vn = (instr >> at_bit) & 0xF;
    uint32_t N = (instr >> extra_at_bit) & 1;
    r = (size != 0 ? ((N << 4) | Vn) : ((Vn << 1) | N));
  }
  FpRegister(const FpRegister& other, uint32_t offset)
      : size(other.size), r(other.r + offset) {}

  uint32_t size;  // 0 = f32, 1 = f64
  uint32_t r;
};
std::ostream& operator<<(std::ostream& os, const FpRegister& rhs) {
  return os << ((rhs.size != 0) ? "d" : "s") << rhs.r;
}

struct FpRegisterRange {
  explicit FpRegisterRange(uint32_t instr)
      : first(instr, 12, 22), imm8(instr & 0xFF) {}
  FpRegister first;
  uint32_t imm8;
};
std::ostream& operator<<(std::ostream& os, const FpRegisterRange& rhs) {
  os << "{" << rhs.first;
  int count = (rhs.first.size != 0 ? ((rhs.imm8 + 1u) >> 1) : rhs.imm8);
  if (count > 1) {
    os << "-" << FpRegister(rhs.first, count - 1);
  }
  if (rhs.imm8 == 0) {
    os << " (EMPTY)";
  } else if (rhs.first.size != 0 && (rhs.imm8 & 1) != 0) {
    os << rhs.first << " (HALF)";
  }
  os << "}";
  return os;
}

void DisassemblerArm::DumpArm(std::ostream& os, const uint8_t* instr_ptr) {
  uint32_t instruction = ReadU32(instr_ptr);
  uint32_t cond = (instruction >> 28) & 0xf;
  uint32_t op1 = (instruction >> 25) & 0x7;
  std::string opcode;
  std::string suffixes;
  std::ostringstream args;
  switch (op1) {
    case 0:
    case 1:  // Data processing instructions.
      {
        if ((instruction & 0x0ff000f0) == 0x01200070) {  // BKPT
          opcode = "bkpt";
          uint32_t imm12 = (instruction >> 8) & 0xfff;
          uint32_t imm4 = (instruction & 0xf);
          args << '#' << ((imm12 << 4) | imm4);
          break;
        }
        if ((instruction & 0x0fffffd0) == 0x012fff10) {  // BX and BLX (register)
          opcode = (((instruction >> 5) & 1) ? "blx" : "bx");
          args << ArmRegister(instruction & 0xf);
          break;
        }
        bool i = (instruction & (1 << 25)) != 0;
        bool s = (instruction & (1 << 20)) != 0;
        uint32_t op = (instruction >> 21) & 0xf;
        opcode = kDataProcessingOperations[op];
        bool implicit_s = ((op & ~3) == 8);  // TST, TEQ, CMP, and CMN.
        bool is_mov = op == 0b1101 || op == 0b1111;
        if (is_mov) {
          // Show only Rd and Rm.
          if (s) {
             suffixes += 's';
           }
           args << ArmRegister(instruction, 12) << ", ";
           if (i) {
              args << ShiftedImmediate(instruction);
            } else {
              // TODO: Shifted register.
              args << ArmRegister(instruction, 16) << ", " << ArmRegister(instruction, 0);
            }
        } else {
          if (implicit_s) {
            // Rd is unused (and not shown), and we don't show the 's' suffix either.
          } else {
            if (s) {
              suffixes += 's';
            }
            args << ArmRegister(instruction, 12) << ", ";
          }
          if (i) {
            args << ArmRegister(instruction, 16) << ", " << ShiftedImmediate(instruction);
          } else {
            // TODO: Shifted register.
            args << ArmRegister(instruction, 16) << ", " << ArmRegister(instruction, 0);
          }
        }
      }
      break;
    case 2:  // Load/store word and unsigned byte.
      {
        bool p = (instruction & (1 << 24)) != 0;
        bool b = (instruction & (1 << 22)) != 0;
        bool w = (instruction & (1 << 21)) != 0;
        bool l = (instruction & (1 << 20)) != 0;
        opcode = StringPrintf("%s%s", (l ? "ldr" : "str"), (b ? "b" : ""));
        args << ArmRegister(instruction, 12) << ", ";
        ArmRegister rn(instruction, 16);
        if (rn.r == 0xf) {
          UNIMPLEMENTED(FATAL) << "literals";
        } else {
          bool wback = !p || w;
          uint32_t offset = (instruction & 0xfff);
          if (p && !wback) {
            args << "[" << rn << ", #" << offset << "]";
          } else if (p && wback) {
            args << "[" << rn << ", #" << offset << "]!";
          } else if (!p && wback) {
            args << "[" << rn << "], #" << offset;
          } else {
            LOG(FATAL) << p << " " << w;
          }
          if (rn.r == 9) {
            args << "  ; ";
            Thread::DumpThreadOffset<4>(args, offset);
          }
        }
      }
      break;
    case 4:  // Load/store multiple.
      {
        bool p = (instruction & (1 << 24)) != 0;
        bool u = (instruction & (1 << 23)) != 0;
        bool w = (instruction & (1 << 21)) != 0;
        bool l = (instruction & (1 << 20)) != 0;
        opcode = StringPrintf("%s%c%c", (l ? "ldm" : "stm"), (u ? 'i' : 'd'), (p ? 'b' : 'a'));
        args << ArmRegister(instruction, 16) << (w ? "!" : "") << ", " << RegisterList(instruction);
      }
      break;
    case 5:  // Branch/branch with link.
      {
        bool bl = (instruction & (1 << 24)) != 0;
        opcode = (bl ? "bl" : "b");
        int32_t imm26 = (instruction & 0xffffff) << 2;
        int32_t imm32 = (imm26 << 6) >> 6;  // Sign extend.
        DumpBranchTarget(args, instr_ptr + 8, imm32);
      }
      break;
    default:
      opcode = "???";
      break;
    }
    opcode += kConditionCodeNames[cond];
    opcode += suffixes;
    // TODO: a more complete ARM disassembler could generate wider opcodes.
    os << FormatInstructionPointer(instr_ptr)
       << StringPrintf(": %08x\t%-7s ", instruction, opcode.c_str())
       << args.str() << '\n';
}

int32_t ThumbExpand(int32_t imm12) {
  if ((imm12 & 0xC00) == 0) {
    switch ((imm12 >> 8) & 3) {
      case 0:
        return imm12 & 0xFF;
      case 1:
        return ((imm12 & 0xFF) << 16) | (imm12 & 0xFF);
      case 2:
        return ((imm12 & 0xFF) << 24) | ((imm12 & 0xFF) << 8);
      default:  // 3
        return ((imm12 & 0xFF) << 24) | ((imm12 & 0xFF) << 16) | ((imm12 & 0xFF) << 8) |
            (imm12 & 0xFF);
    }
  } else {
    uint32_t val = 0x80 | (imm12 & 0x7F);
    int32_t rotate = (imm12 >> 7) & 0x1F;
    return (val >> rotate) | (val << (32 - rotate));
  }
}

uint32_t VFPExpand32(uint32_t imm8) {
  CHECK_EQ(imm8 & 0xffu, imm8);
  uint32_t bit_a = (imm8 >> 7) & 1;
  uint32_t bit_b = (imm8 >> 6) & 1;
  uint32_t slice = imm8 & 0x3f;
  return (bit_a << 31) | ((1 << 30) - (bit_b << 25)) | (slice << 19);
}

uint64_t VFPExpand64(uint32_t imm8) {
  CHECK_EQ(imm8 & 0xffu, imm8);
  uint64_t bit_a = (imm8 >> 7) & 1;
  uint64_t bit_b = (imm8 >> 6) & 1;
  uint64_t slice = imm8 & 0x3f;
  return (bit_a << 31) | ((UINT64_C(1) << 62) - (bit_b << 54)) | (slice << 48);
}

uint64_t AdvSIMDExpand(uint32_t op, uint32_t cmode, uint32_t imm8) {
  CHECK_EQ(op & 1, op);
  CHECK_EQ(cmode & 0xf, cmode);
  CHECK_EQ(imm8 & 0xff, imm8);
  int32_t cmode321 = cmode >> 1;
  if (imm8 == 0 && cmode321 != 0 && cmode321 != 4 && cmode321 != 7) {
    return INT64_C(0x00000000deadbeef);  // UNPREDICTABLE
  }
  uint64_t imm = imm8;
  switch (cmode321) {
    case 3: imm <<= 8;  // Fall through.
    case 2: imm <<= 8;  // Fall through.
    case 1: imm <<= 8;  // Fall through.
    case 0: return static_cast<int64_t>((imm << 32) | imm);
    case 5: imm <<= 8;  // Fall through.
    case 4: return static_cast<int64_t>((imm << 48) | (imm << 32) | (imm << 16) | imm);
    case 6:
      imm = ((imm + 1u) << ((cmode & 1) != 0 ? 16 : 8)) - 1u;  // Add 8 or 16 ones.
      return static_cast<int64_t>((imm << 32) | imm);
    default:
      CHECK_EQ(cmode321, 7);
      if ((cmode & 1) == 0 && op == 0) {
        imm = (imm << 8) | imm;
        return static_cast<int64_t>((imm << 48) | (imm << 32) | (imm << 16) | imm);
      } else if ((cmode & 1) == 0 && op != 0) {
        for (int i = 1; i != 8; ++i) {
          imm |= ((imm >> i) & UINT64_C(1)) << (i * 8);
        }
        imm = imm & ~UINT64_C(0xfe);
        return static_cast<int64_t>((imm << 8) - imm);
      } else if ((cmode & 1) != 0 && op == 0) {
        imm = static_cast<uint32_t>(VFPExpand32(imm8));
        return static_cast<int64_t>((imm << 32) | imm);
      } else {
        return INT64_C(0xdeadbeef00000000);  // UNDEFINED
      }
  }
}

size_t DisassemblerArm::DumpThumb32(std::ostream& os, const uint8_t* instr_ptr) {
  uint32_t instr = (ReadU16(instr_ptr) << 16) | ReadU16(instr_ptr + 2);
  // |111|1 1|1000000|0000|1111110000000000|
  // |5 3|2 1|0987654|3  0|5    0    5    0|
  // |---|---|-------|----|----------------|
  // |332|2 2|2222222|1111|1111110000000000|
  // |1 9|8 7|6543210|9  6|5    0    5    0|
  // |---|---|-------|----|----------------|
  // |111|op1| op2   |    |                |
  uint32_t op1 = (instr >> 27) & 3;
  if (op1 == 0) {
    return DumpThumb16(os, instr_ptr);
  }

  uint32_t op2 = (instr >> 20) & 0x7F;
  std::ostringstream opcode;
  std::ostringstream args;
  switch (op1) {
    case 0:
      break;
    case 1:
      if ((op2 & 0x64) == 0) {  // 00x x0xx
        // |111|11|10|00|0|00|0000|1111110000000000|
        // |5 3|21|09|87|6|54|3  0|5    0    5    0|
        // |---|--|--|--|-|--|----|----------------|
        // |332|22|22|22|2|22|1111|1111110000000000|
        // |1 9|87|65|43|2|10|9  6|5    0    5    0|
        // |---|--|--|--|-|--|----|----------------|
        // |111|01|00|op|0|WL| Rn |                |
        // |111|01| op2      |    |                |
        // STM - 111 01 00-01-0-W0 nnnn rrrrrrrrrrrrrrrr
        // LDM - 111 01 00-01-0-W1 nnnn rrrrrrrrrrrrrrrr
        // PUSH- 111 01 00-01-0-10 1101 0M0rrrrrrrrrrrrr
        // POP - 111 01 00-01-0-11 1101 PM0rrrrrrrrrrrrr
        uint32_t op = (instr >> 23) & 3;
        uint32_t W = (instr >> 21) & 1;
        uint32_t L = (instr >> 20) & 1;
        ArmRegister Rn(instr, 16);
        if (op == 1 || op == 2) {
          if (op == 1) {
            if (L == 0) {
              opcode << "stm";
              args << Rn << (W == 0 ? "" : "!") << ", ";
            } else {
              if (Rn.r != 13) {
                opcode << "ldm";
                args << Rn << (W == 0 ? "" : "!") << ", ";
              } else {
                opcode << "pop";
              }
            }
          } else {
            if (L == 0) {
              if (Rn.r != 13) {
                opcode << "stmdb";
                args << Rn << (W == 0 ? "" : "!") << ", ";
              } else {
                opcode << "push";
              }
            } else {
              opcode << "ldmdb";
              args << Rn << (W == 0 ? "" : "!") << ", ";
            }
          }
          args << RegisterList(instr);
        }
      } else if ((op2 & 0x64) == 4) {  // 00x x1xx
        uint32_t op3 = (instr >> 23) & 3;
        uint32_t op4 = (instr >> 20) & 3;
        // uint32_t op5 = (instr >> 4) & 0xF;
        ArmRegister Rn(instr, 16);
        ArmRegister Rt(instr, 12);
        ArmRegister Rd(instr, 8);
        uint32_t imm8 = instr & 0xFF;
        if ((op3 & 2) == 2) {     // 1x
          int W = (instr >> 21) & 1;
          int U = (instr >> 23) & 1;
          int P = (instr >> 24) & 1;

          if ((op4 & 1) == 1) {
            opcode << "ldrd";
          } else {
            opcode << "strd";
          }
          args << Rt << "," << Rd << ", [" << Rn;
          const char *sign = U ? "+" : "-";
          if (P == 0 && W == 1) {
            args << "], #" << sign << (imm8 << 2);
          } else {
            args << ", #" << sign << (imm8 << 2) << "]";
            if (W == 1) {
              args << "!";
            }
          }
        } else {                  // 0x
          switch (op4) {
            case 0:
              if (op3 == 0) {   // op3 is 00, op4 is 00
                opcode << "strex";
                args << Rd << ", " << Rt << ", [" << Rn << ", #" << (imm8 << 2) << "]";
                if (Rd.r == 13 || Rd.r == 15 || Rt.r == 13 || Rt.r == 15 || Rn.r == 15 ||
                    Rd.r == Rn.r || Rd.r == Rt.r) {
                  args << " (UNPREDICTABLE)";
                }
              } else {          // op3 is 01, op4 is 00
                // this is one of strexb, strexh or strexd
                int op5 = (instr >> 4) & 0xf;
                switch (op5) {
                  case 4:
                  case 5:
                    opcode << ((op5 == 4) ? "strexb" : "strexh");
                    Rd = ArmRegister(instr, 0);
                    args << Rd << ", " << Rt << ", [" << Rn << "]";
                    if (Rd.r == 13 || Rd.r == 15 || Rt.r == 13 || Rt.r == 15 || Rn.r == 15 ||
                        Rd.r == Rn.r || Rd.r == Rt.r || (instr & 0xf00) != 0xf00) {
                      args << " (UNPREDICTABLE)";
                    }
                    break;
                  case 7:
                    opcode << "strexd";
                    ArmRegister Rt2 = Rd;
                    Rd = ArmRegister(instr, 0);
                    args << Rd << ", " << Rt << ", " << Rt2 << ", [" << Rn << "]";
                    if (Rd.r == 13 || Rd.r == 15 || Rt.r == 13 || Rt.r == 15 ||
                        Rt2.r == 13 || Rt2.r == 15 || Rn.r == 15 ||
                        Rd.r == Rn.r || Rd.r == Rt.r || Rd.r == Rt2.r) {
                      args << " (UNPREDICTABLE)";
                    }
                    break;
                }
              }
              break;
            case 1:
              if (op3 == 0) {   // op3 is 00, op4 is 01
                opcode << "ldrex";
                args << Rt << ", [" << Rn << ", #" << (imm8 << 2) << "]";
                if (Rt.r == 13 || Rt.r == 15 || Rn.r == 15 || (instr & 0xf00) != 0xf00) {
                  args << " (UNPREDICTABLE)";
                }
              } else {          // op3 is 01, op4 is 01
                // this is one of strexb, strexh or strexd
                int op5 = (instr >> 4) & 0xf;
                switch (op5) {
                  case 0:
                    opcode << "tbb";
                    break;
                  case 1:
                    opcode << "tbh";
                    break;
                  case 4:
                  case 5:
                    opcode << ((op5 == 4) ? "ldrexb" : "ldrexh");
                    args << Rt << ", [" << Rn << "]";
                    if (Rt.r == 13 || Rt.r == 15 || Rn.r == 15 || (instr & 0xf0f) != 0xf0f) {
                      args << " (UNPREDICTABLE)";
                    }
                    break;
                  case 7:
                    opcode << "ldrexd";
                    args << Rt << ", " << Rd /* Rt2 */ << ", [" << Rn << "]";
                    if (Rt.r == 13 || Rt.r == 15 || Rd.r == 13 /* Rt2 */ || Rd.r == 15 /* Rt2 */ ||
                        Rn.r == 15 || (instr & 0x00f) != 0x00f) {
                      args << " (UNPREDICTABLE)";
                    }
                    break;
                }
              }
              break;
            case 2:     // op3 is 0x, op4 is 10
            case 3:   // op3 is 0x, op4 is 11
              if (op4 == 2) {
                opcode << "strd";
              } else {
                opcode << "ldrd";
              }
              int W = (instr >> 21) & 1;
              int U = (instr >> 23) & 1;
              int P = (instr >> 24) & 1;

              args << Rt << "," << Rd << ", [" << Rn;
              const char *sign = U ? "+" : "-";
              if (P == 0 && W == 1) {
                args << "], #" << sign << imm8;
              } else {
                args << ", #" << sign << imm8 << "]";
                if (W == 1) {
                  args << "!";
                }
              }
              break;
          }
        }

      } else if ((op2 & 0x60) == 0x20) {  // 01x xxxx
        // Data-processing (shifted register)
        // |111|1110|0000|0|0000|1111|1100|00|00|0000|
        // |5 3|2109|8765|4|3  0|5   |10 8|7 |5 |3  0|
        // |---|----|----|-|----|----|----|--|--|----|
        // |332|2222|2222|2|1111|1111|1100|00|00|0000|
        // |1 9|8765|4321|0|9  6|5   |10 8|7 |5 |3  0|
        // |---|----|----|-|----|----|----|--|--|----|
        // |111|0101| op3|S| Rn |imm3| Rd |i2|ty| Rm |
        uint32_t op3 = (instr >> 21) & 0xF;
        uint32_t S = (instr >> 20) & 1;
        uint32_t imm3 = ((instr >> 12) & 0x7);
        uint32_t imm2 = ((instr >> 6) & 0x3);
        uint32_t imm5 = ((imm3 << 2) | imm2);
        uint32_t shift_type = ((instr >> 4) & 0x3);
        ArmRegister Rd(instr, 8);
        ArmRegister Rn(instr, 16);
        ArmRegister Rm(instr, 0);
        switch (op3) {
          case 0x0:
            if (Rd.r != 0xF) {
              opcode << "and";
            } else {
              if (S != 1U) {
                opcode << "UNKNOWN TST-" << S;
                break;
              }
              opcode << "tst";
              S = 0;  // don't print 's'
            }
            break;
          case 0x1: opcode << "bic"; break;
          case 0x2:
            if (Rn.r != 0xF) {
              opcode << "orr";
            } else {
              // TODO: use canonical form if there is a shift (lsl, ...).
              opcode << "mov";
            }
            break;
          case 0x3:
            if (Rn.r != 0xF) {
              opcode << "orn";
            } else {
              opcode << "mvn";
            }
            break;
          case 0x4:
            if (Rd.r != 0xF) {
              opcode << "eor";
            } else {
              if (S != 1U) {
                opcode << "UNKNOWN TEQ-" << S;
                break;
              }
              opcode << "teq";
              S = 0;  // don't print 's'
            }
            break;
          case 0x6: opcode << "pkh"; break;
          case 0x8:
            if (Rd.r != 0xF) {
              opcode << "add";
            } else {
              if (S != 1U) {
                opcode << "UNKNOWN CMN-" << S;
                break;
              }
              opcode << "cmn";
              S = 0;  // don't print 's'
            }
            break;
          case 0xA: opcode << "adc"; break;
          case 0xB: opcode << "sbc"; break;
          case 0xD:
            if (Rd.r != 0xF) {
              opcode << "sub";
            } else {
              if (S != 1U) {
                opcode << "UNKNOWN CMP-" << S;
                break;
              }
              opcode << "cmp";
              S = 0;  // don't print 's'
            }
            break;
          case 0xE: opcode << "rsb"; break;
          default: opcode << "UNKNOWN DPSR-" << op3; break;
        }

        if (S == 1) {
          opcode << "s";
        }
        opcode << ".w";

        if (Rd.r != 0xF) {
          args << Rd << ", ";
        }
        if (Rn.r != 0xF) {
          args << Rn << ", ";
        }
        args << Rm;

        // Shift operand.
        bool noShift = (imm5 == 0 && shift_type != 0x3);
        if (!noShift) {
          args << ", ";
          switch (shift_type) {
            case 0x0: args << "lsl"; break;
            case 0x1: args << "lsr"; break;
            case 0x2: args << "asr"; break;
            case 0x3:
              if (imm5 == 0) {
                args << "rrx";
              } else {
                args << "ror";
              }
              break;
          }
          if (shift_type != 0x3 /* rrx */) {
            args << StringPrintf(" #%d", (0 != imm5 || 0 == shift_type) ? imm5 : 32);
          }
        }

      } else if ((op2 & 0x40) == 0x40) {  // 1xx xxxx
        // Co-processor instructions
        // |111|1|11|000000|0000|1111|1100|000|0  |0000|
        // |5 3|2|10|987654|3  0|54 2|10 8|7 5|4  |   0|
        // |---|-|--|------|----|----|----|---|---|----|
        // |332|2|22|222222|1111|1111|1100|000|0  |0000|
        // |1 9|8|76|543210|9  6|54 2|10 8|7 5|4  |   0|
        // |---|-|--|------|----|----|----|---|---|----|
        // |111| |11| op3  | Rn |    |copr|   |op4|    |
        uint32_t op3 = (instr >> 20) & 0x3F;
        uint32_t coproc = (instr >> 8) & 0xF;
        uint32_t op4 = (instr >> 4) & 0x1;

        if (coproc == 0xA || coproc == 0xB) {   // 101x
          if (op3 < 0x20 && (op3 & ~5) != 0) {     // 0xxxxx and not 000x0x
            // Extension register load/store instructions
            // |1111|110|00000|0000|1111|110|0|00000000|
            // |5  2|1 9|87654|3  0|5  2|1 9|8|7      0|
            // |----|---|-----|----|----|---|-|--------|
            // |3322|222|22222|1111|1111|110|0|00000000|
            // |1  8|7 5|4   0|9  6|5  2|1 9|8|7      0|
            // |----|---|-----|----|----|---|-|--------|
            // |1110|110|PUDWL| Rn | Vd |101|S|  imm8  |
            uint32_t P = (instr >> 24) & 1;
            uint32_t U = (instr >> 23) & 1;
            uint32_t W = (instr >> 21) & 1;
            if (P == U && W == 1) {
              opcode << "UNDEFINED";
            } else {
              uint32_t L = (instr >> 20) & 1;
              uint32_t S = (instr >> 8) & 1;
              ArmRegister Rn(instr, 16);
              if (P == 1 && W == 0) {  // VLDR
                FpRegister d(instr, 12, 22);
                uint32_t imm8 = instr & 0xFF;
                opcode << (L == 1 ? "vldr" : "vstr");
                args << d << ", [" << Rn << ", #" << ((U == 1) ? "" : "-")
                     << (imm8 << 2) << "]";
                if (Rn.r == 15 && U == 1) {
                  intptr_t lit_adr = reinterpret_cast<intptr_t>(instr_ptr);
                  lit_adr = RoundDown(lit_adr, 4) + 4 + (imm8 << 2);
                  typedef const int64_t unaligned_int64_t __attribute__ ((aligned (2)));
                  args << StringPrintf("  ; 0x%" PRIx64, *reinterpret_cast<unaligned_int64_t*>(lit_adr));
                }
              } else if (Rn.r == 13 && W == 1 && U == L) {  // VPUSH/VPOP
                opcode << (L == 1 ? "vpop" : "vpush");
                args << FpRegisterRange(instr);
              } else {  // VLDM
                opcode << (L == 1 ? "vldm" : "vstm");
                args << Rn << ((W == 1) ? "!" : "") << ", "
                     << FpRegisterRange(instr);
              }
              opcode << (S == 1 ? ".f64" : ".f32");
            }
          } else if ((op3 >> 1) == 2) {      // 00010x
            if ((instr & 0xD0) == 0x10) {
              // 64bit transfers between ARM core and extension registers.
              uint32_t L = (instr >> 20) & 1;
              uint32_t S = (instr >> 8) & 1;
              ArmRegister Rt2(instr, 16);
              ArmRegister Rt(instr, 12);
              FpRegister m(instr, 0, 5);
              opcode << "vmov" << (S ? ".f64" : ".f32");
              if (L == 1) {
                args << Rt << ", " << Rt2 << ", ";
              }
              if (S) {
                args << m;
              } else {
                args << m << ", " << FpRegister(m, 1);
              }
              if (L == 0) {
                args << ", " << Rt << ", " << Rt2;
              }
              if (Rt.r == 15 || Rt.r == 13 || Rt2.r == 15 || Rt2.r == 13 ||
                  (S == 0 && m.r == 31) || (L == 1 && Rt.r == Rt2.r)) {
                args << " (UNPREDICTABLE)";
              }
            }
          } else if ((op3 >> 4) == 2 && op4 == 0) {     // 10xxxx, op = 0
            // fp data processing
            // VMLA, VMLS, VMUL, VNMUL, VADD, VSUB, VDIV, VMOV, ...
            // |1111|1100|0|0|00|0000|1111|110|0|0|0|0|0|0000|
            // |5  2|1  8|7|6|54|3  0|5  2|1 9|8|7|6|5|4|3  0|
            // |----|----|-|-|--|----|----|---|-|-|-|-|-|----|
            // |3322|2222|2|2|22|1111|1111|110|0|0|0|0|0|0000|
            // |1  8|7  4|3|2|10|9  6|5  2|1 9|8|7|6|5|4|3  0|
            // |----|----|-|-|--|----|----|---|-|-|-|-|-|----|
            // |1110|1110|  op3 | Vn | Vd |101|S|N|Q|M|0| Vm |
            // |1110|1110|0|D|00| Vn | Vd |101|S|N|0|M|0| Vm | VMLA
            // |1110|1110|0|D|00| Vn | Vd |101|S|N|1|M|0| Vm | VMLS
            // |1110|1110|0|D|10| Vn | Vd |101|S|N|0|M|0| Vm | VMUL
            // |1110|1110|0|D|10| Vn | Vd |101|S|N|1|M|0| Vm | VNMUL
            // |1110|1110|0|D|11| Vn | Vd |101|S|N|0|M|0| Vm | VADD
            // |1110|1110|0|D|11| Vn | Vd |101|S|N|1|M|0| Vm | VSUB
            // |1110|1110|1|D|00| Vn | Vd |101|S|N|0|M|0| Vm | VDIV
            // |1110|1110|1|D|11| iH | Vd |101|S|0|0|0|0| iL | VMOV (imm)
            // |1110|1110|1|D|11|op5 | Vd |101|S|.|1|M|0| Vm | ... (see below)
            uint32_t S = (instr >> 8) & 1;
            uint32_t Q = (instr >> 6) & 1;
            FpRegister d(instr, 12, 22);
            FpRegister n(instr, 16, 7);
            FpRegister m(instr, 0, 5);
            if ((op3 & 0xB) == 0) {  // 100x00
              opcode << (Q == 0 ? "vmla" : "vmls") << (S != 0 ? ".f64" : ".f32");
              args << d << ", " << n << ", " << m;
            } else if ((op3 & 0xB) == 0x2) {  // 100x10
              opcode << (Q == 0 ? "vmul" : "vnmul") << (S != 0 ? ".f64" : ".f32");
              args << d << ", " << n << ", " << m;
            } else if ((op3 & 0xB) == 0x3) {  // 100x11
              opcode << (Q == 0 ? "vadd" : "vsub") << (S != 0 ? ".f64" : ".f32");
              args << d << ", " << n << ", " << m;
            } else if ((op3 & 0xB) == 0x8 && Q == 0) {  // 101x00, Q == 0
              opcode << "vdiv" << (S != 0 ? ".f64" : ".f32");
              args << d << ", " << n << ", " << m;
            } else if ((op3 & 0xB) == 0xB && Q == 0) {  // 101x11, Q == 0
              uint32_t imm8 = ((instr & 0xf0000u) >> 12) | (instr & 0xfu);
              opcode << "vmov" << (S != 0 ? ".f64" : ".f32");
              args << d << ", " << (S != 0 ? StringPrintf("0x%016" PRIx64, VFPExpand64(imm8))
                                           : StringPrintf("0x%08x", VFPExpand32(imm8)));
              if ((instr & 0xa0) != 0) {
                args << " (UNPREDICTABLE)";
              }
            } else if ((op3 & 0xB) == 0xB && Q == 1) {  // 101x11, Q == 1
              // VNEG, VSQRT, VCMP, VCMPE, VCVT (floating-point conversion)
              // |1111|1100|0|0|00|0000|1111|110|0|0 |0|0|0|0000|
              // |5  2|1  8|7|6|54|3  0|5  2|1 9|8|7 |6|5|4|3  0|
              // |----|----|-|-|--|----|----|---|-|- |-|-|-|----|
              // |3322|2222|2|2|22|1111|1111|110|0|0 |0|0|0|0000|
              // |1  8|7  4|3|2|10|9  6|5  2|1 9|8|7 |6|5|4|3  0|
              // |----|----|-|-|--|----|----|---|-|- |-|-|-|----|
              // |1110|1110|1|D|11|0000| Vd |101|S|0 |1|M|0| Vm | VMOV (reg)
              // |1110|1110|1|D|11|0000| Vd |101|S|1 |1|M|0| Vm | VABS
              // |1110|1110|1|D|11|0001| Vd |101|S|0 |1|M|0| Vm | VNEG
              // |1110|1110|1|D|11|0001| Vd |101|S|1 |1|M|0| Vm | VSQRT
              // |1110|1110|1|D|11|0100| Vd |101|S|op|1|M|0| Vm | VCMP
              // |1110|1110|1|D|11|0101| Vd |101|S|op|1|0|0|0000| VCMPE
              // |1110|1110|1|D|11|op5 | Vd |101|S|op|1|M|0| Vm | VCVT
              uint32_t op5 = (instr >> 16) & 0xF;
              uint32_t op = (instr >> 7) & 1;
              // Register types in VCVT instructions rely on the combination of op5 and S.
              FpRegister Dd(instr, 12, 22, 1);
              FpRegister Sd(instr, 12, 22, 0);
              FpRegister Dm(instr, 0, 5, 1);
              FpRegister Sm(instr, 0, 5, 0);
              if (op5 == 0) {
                opcode << (op == 0 ? "vmov" : "vabs") << (S != 0 ? ".f64" : ".f32");
                args << d << ", " << m;
              } else if (op5 == 1) {
                opcode << (op != 0 ? "vsqrt" : "vneg") << (S != 0 ? ".f64" : ".f32");
                args << d << ", " << m;
              } else if (op5 == 4) {
                opcode << "vcmp" << (S != 0 ? ".f64" : ".f32");
                args << d << ", " << m;
                if (op != 0) {
                  args << " (quiet nan)";
                }
              } else if (op5 == 5) {
                opcode << "vcmpe" << (S != 0 ? ".f64" : ".f32");
                args << d << ", #0.0";
                if (op != 0) {
                  args << " (quiet nan)";
                }
                if ((instr & 0x2f) != 0) {
                  args << " (UNPREDICTABLE)";
                }
              } else if (op5 == 0xD) {
                if (S == 1) {
                  // vcvt{r}.s32.f64
                  opcode << "vcvt" << (op == 0 ? "r" : "") << ".s32.f64";
                  args << Sd << ", " << Dm;
                } else {
                  // vcvt{r}.s32.f32
                  opcode << "vcvt" << (op == 0 ? "r" : "") << ".s32.f32";
                  args << Sd << ", " << Sm;
                }
              } else if (op5 == 0xC) {
                if (S == 1) {
                  // vcvt{r}.u32.f64
                  opcode << "vcvt" << (op == 0 ? "r" : "") << ".u32.f64";
                  args << Sd << ", " << Dm;
                } else {
                  // vcvt{r}.u32.f32
                  opcode << "vcvt" << (op == 0 ? "r" : "") << ".u32.f32";
                  args << Sd << ", " << Sm;
                }
              } else if (op5 == 0x8) {
                if (S == 1) {
                  // vcvt.f64.<Tm>
                  opcode << "vcvt.f64." << (op == 0 ? "u" : "s") << "32";
                  args << Dd << ", " << Sm;
                } else {
                  // vcvt.f32.<Tm>
                  opcode << "vcvt.f32." << (op == 0 ? "u" : "s") << "32";
                  args << Sd << ", " << Sm;
                }
              } else if (op5 == 0x7) {
                if (op == 1) {
                  if (S == 1) {
                    // vcvt.f64.f32
                    opcode << "vcvt.f64.f32";
                    args << Dd << ", " << Sm;
                  } else {
                    // vcvt.f32.f64
                    opcode << "vcvt.f32.f64";
                    args << Sd << ", " << Dm;
                  }
                }
              } else if ((op5 & 0xa) == 0xa) {
                opcode << "vcvt";
                args << "[undecoded: floating <-> fixed]";
              }
            }
          } else if ((op3 >> 4) == 2 && op4 == 1) {     // 10xxxx, op = 1
            if (coproc == 10 && (op3 & 0xE) == 0) {
              // VMOV (between ARM core register and single-precision register)
              // |1111|1100|000|0 |0000|1111|1100|0|00|0|0000|
              // |5   |1  8|7 5|4 |3  0|5  2|1  8|7|65|4|3  0|
              // |----|----|---|- |----|----|----|-|--|-|----|
              // |3322|2222|222|2 |1111|1111|1100|0|00|0|0000|
              // |1  8|7  4|3 1|0 |9  6|5  2|1  8|7|65|4|3  0|
              // |----|----|---|- |----|----|----|-|--|-|----|
              // |1110|1110|000|op| Vn | Rt |1010|N|00|1|0000|
              uint32_t op = op3 & 1;
              ArmRegister Rt(instr, 12);
              FpRegister n(instr, 16, 7);
              opcode << "vmov.f32";
              if (op) {
                args << Rt << ", " << n;
              } else {
                args << n << ", " << Rt;
              }
              if (Rt.r == 13 || Rt.r == 15 || (instr & 0x6F) != 0) {
                args << " (UNPREDICTABLE)";
              }
            } else if (coproc == 10 && op3 == 0x2F) {
              // VMRS
              // |1111|11000000|0000|1111|1100|000|0|0000|
              // |5   |1      4|3  0|5  2|1  8|7 5|4|3  0|
              // |----|--------|----|----|----|---|-|----|
              // |3322|22222222|1111|1111|1100|000|0|0000|
              // |1  8|7      0|9  6|5  2|1  8|7 5|4|3  0|
              // |----|--------|----|----|----|---|-|----|
              // |1110|11101111|reg | Rt |1010|000|1|0000| - last 7 0s are (0)
              uint32_t spec_reg = (instr >> 16) & 0xF;
              ArmRegister Rt(instr, 12);
              opcode << "vmrs";
              if (spec_reg == 1) {
                if (Rt.r == 15) {
                  args << "APSR_nzcv, FPSCR";
                } else if (Rt.r == 13) {
                  args << Rt << ", FPSCR (UNPREDICTABLE)";
                } else {
                  args << Rt << ", FPSCR";
                }
              } else {
                args << "(PRIVILEGED)";
              }
            } else if (coproc == 11 && (op3 & 0x9) != 8) {
              // VMOV (ARM core register to scalar or vice versa; 8/16/32-bit)
            }
          }
        }
      }
      break;
    case 2:
      if ((instr & 0x8000) == 0 && (op2 & 0x20) == 0) {
        // Data-processing (modified immediate)
        // |111|11|10|0000|0|0000|1|111|1100|00000000|
        // |5 3|21|09|8765|4|3  0|5|4 2|10 8|7 5    0|
        // |---|--|--|----|-|----|-|---|----|--------|
        // |332|22|22|2222|2|1111|1|111|1100|00000000|
        // |1 9|87|65|4321|0|9  6|5|4 2|10 8|7 5    0|
        // |---|--|--|----|-|----|-|---|----|--------|
        // |111|10|i0| op3|S| Rn |0|iii| Rd |iiiiiiii|
        //  111 10 x0 xxxx x xxxx opxxx xxxx xxxxxxxx
        uint32_t i = (instr >> 26) & 1;
        uint32_t op3 = (instr >> 21) & 0xF;
        uint32_t S = (instr >> 20) & 1;
        ArmRegister Rn(instr, 16);
        uint32_t imm3 = (instr >> 12) & 7;
        ArmRegister Rd(instr, 8);
        uint32_t imm8 = instr & 0xFF;
        int32_t imm32 = (i << 11) | (imm3 << 8) | imm8;
        if (Rn.r == 0xF && (op3 == 0x2 || op3 == 0x3)) {
          if (op3 == 0x2) {
            opcode << "mov";
            if (S == 1) {
              opcode << "s";
            }
            opcode << ".w";
          } else {
            opcode << "mvn";
            if (S == 1) {
              opcode << "s";
            }
          }
          args << Rd << ", #" << ThumbExpand(imm32);
        } else if (Rd.r == 0xF && S == 1 &&
                   (op3 == 0x0 || op3 == 0x4 || op3 == 0x8 || op3 == 0xD)) {
          if (op3 == 0x0) {
            opcode << "tst";
          } else if (op3 == 0x4) {
            opcode << "teq";
          } else if (op3 == 0x8) {
            opcode << "cmn.w";
          } else {
            opcode << "cmp.w";
          }
          args << Rn << ", #" << ThumbExpand(imm32);
        } else {
          switch (op3) {
            case 0x0: opcode << "and"; break;
            case 0x1: opcode << "bic"; break;
            case 0x2: opcode << "orr"; break;
            case 0x3: opcode << "orn"; break;
            case 0x4: opcode << "eor"; break;
            case 0x8: opcode << "add"; break;
            case 0xA: opcode << "adc"; break;
            case 0xB: opcode << "sbc"; break;
            case 0xD: opcode << "sub"; break;
            case 0xE: opcode << "rsb"; break;
            default: opcode << "UNKNOWN DPMI-" << op3; break;
          }
          if (S == 1) {
            opcode << "s";
          }
          args << Rd << ", " << Rn << ", #" << ThumbExpand(imm32);
        }
      } else if ((instr & 0x8000) == 0 && (op2 & 0x20) != 0) {
        // Data-processing (plain binary immediate)
        // |111|11|10|00000|0000|1|111110000000000|
        // |5 3|21|09|87654|3  0|5|4   0    5    0|
        // |---|--|--|-----|----|-|---------------|
        // |332|22|22|22222|1111|1|111110000000000|
        // |1 9|87|65|43210|9  6|5|4   0    5    0|
        // |---|--|--|-----|----|-|---------------|
        // |111|10|x1| op3 | Rn |0|xxxxxxxxxxxxxxx|
        uint32_t op3 = (instr >> 20) & 0x1F;
        switch (op3) {
          case 0x00: case 0x0A: {
            // ADD/SUB.W Rd, Rn #imm12 - 111 10 i1 0101 0 nnnn 0 iii dddd iiiiiiii
            ArmRegister Rd(instr, 8);
            ArmRegister Rn(instr, 16);
            uint32_t i = (instr >> 26) & 1;
            uint32_t imm3 = (instr >> 12) & 0x7;
            uint32_t imm8 = instr & 0xFF;
            uint32_t imm12 = (i << 11) | (imm3 << 8) | imm8;
            if (Rn.r != 0xF) {
              opcode << (op3 == 0 ? "addw" : "subw");
              args << Rd << ", " << Rn << ", #" << imm12;
            } else {
              opcode << "adr";
              args << Rd << ", ";
              DumpBranchTarget(args, instr_ptr + 4, (op3 == 0) ? imm12 : -imm12);
            }
            break;
          }
          case 0x04: case 0x0C: {
            // MOVW/T Rd, #imm16     - 111 10 i0 0010 0 iiii 0 iii dddd iiiiiiii
            ArmRegister Rd(instr, 8);
            uint32_t i = (instr >> 26) & 1;
            uint32_t imm3 = (instr >> 12) & 0x7;
            uint32_t imm8 = instr & 0xFF;
            uint32_t Rn = (instr >> 16) & 0xF;
            uint32_t imm16 = (Rn << 12) | (i << 11) | (imm3 << 8) | imm8;
            opcode << (op3 == 0x04 ? "movw" : "movt");
            args << Rd << ", #" << imm16;
            break;
          }
          case 0x16: {
            // BFI Rd, Rn, #lsb, #width - 111 10 0 11 011 0 nnnn 0 iii dddd ii 0 iiiii
            ArmRegister Rd(instr, 8);
            ArmRegister Rn(instr, 16);
            uint32_t msb = instr & 0x1F;
            uint32_t imm2 = (instr >> 6) & 0x3;
            uint32_t imm3 = (instr >> 12) & 0x7;
            uint32_t lsb = (imm3 << 2) | imm2;
            uint32_t width = msb - lsb + 1;
            if (Rn.r != 0xF) {
              opcode << "bfi";
              args << Rd << ", " << Rn << ", #" << lsb << ", #" << width;
            } else {
              opcode << "bfc";
              args << Rd << ", #" << lsb << ", #" << width;
            }
            break;
          }
          default:
            break;
        }
      } else {
        // Branches and miscellaneous control
        // |111|11|1000000|0000|1|111|1100|00000000|
        // |5 3|21|0987654|3  0|5|4 2|10 8|7 5    0|
        // |---|--|-------|----|-|---|----|--------|
        // |332|22|2222222|1111|1|111|1100|00000000|
        // |1 9|87|6543210|9  6|5|4 2|10 8|7 5    0|
        // |---|--|-------|----|-|---|----|--------|
        // |111|10| op2   |    |1|op3|op4 |        |

        uint32_t op3 = (instr >> 12) & 7;
        // uint32_t op4 = (instr >> 8) & 0xF;
        switch (op3) {
          case 0:
            if ((op2 & 0x38) != 0x38) {
              // Conditional branch
              // |111|11|1|0000|000000|1|1|1 |1|1 |10000000000|
              // |5 3|21|0|9876|543  0|5|4|3 |2|1 |0    5    0|
              // |---|--|-|----|------|-|-|--|-|--|-----------|
              // |332|22|2|2222|221111|1|1|1 |1|1 |10000000000|
              // |1 9|87|6|5432|109  6|5|4|3 |2|1 |0    5    0|
              // |---|--|-|----|------|-|-|--|-|--|-----------|
              // |111|10|S|cond| imm6 |1|0|J1|0|J2| imm11     |
              uint32_t S = (instr >> 26) & 1;
              uint32_t J2 = (instr >> 11) & 1;
              uint32_t J1 = (instr >> 13) & 1;
              uint32_t imm6 = (instr >> 16) & 0x3F;
              uint32_t imm11 = instr & 0x7FF;
              uint32_t cond = (instr >> 22) & 0xF;
              int32_t imm32 = (S << 20) |  (J2 << 19) | (J1 << 18) | (imm6 << 12) | (imm11 << 1);
              imm32 = (imm32 << 11) >> 11;  // sign extend 21bit immediate
              opcode << "b";
              DumpCond(opcode, cond);
              opcode << ".w";
              DumpBranchTarget(args, instr_ptr + 4, imm32);
            } else if (op2 == 0x3B) {
              // Miscellaneous control instructions
              uint32_t op5 = (instr >> 4) & 0xF;
              switch (op5) {
                case 4: opcode << "dsb"; DumpMemoryDomain(args, instr & 0xF); break;
                case 5: opcode << "dmb"; DumpMemoryDomain(args, instr & 0xF); break;
                case 6: opcode << "isb"; DumpMemoryDomain(args, instr & 0xF); break;
              }
            }
            break;
          case 2:
            if ((op2 & 0x38) == 0x38) {
              if (op2 == 0x7F) {
                opcode << "udf";
              }
              break;
            }
            // Else deliberate fall-through to B.
          case 1: case 3: {
            // B
            // |111|11|1|0000|000000|11|1 |1|1 |10000000000|
            // |5 3|21|0|9876|543  0|54|3 |2|1 |0    5    0|
            // |---|--|-|----|------|--|--|-|--|-----------|
            // |332|22|2|2222|221111|11|1 |1|1 |10000000000|
            // |1 9|87|6|5  2|10   6|54|3 |2|1 |0    5    0|
            // |---|--|-|----|------|--|--|-|--|-----------|
            // |111|10|S|cond| imm6 |10|J1|0|J2| imm11     |
            // |111|10|S| imm10     |10|J1|1|J2| imm11     |
            uint32_t S = (instr >> 26) & 1;
            uint32_t cond = (instr >> 22) & 0xF;
            uint32_t J2 = (instr >> 11) & 1;
            uint32_t form = (instr >> 12) & 1;
            uint32_t J1 = (instr >> 13) & 1;
            uint32_t imm10 = (instr >> 16) & 0x3FF;
            uint32_t imm6  = (instr >> 16) & 0x3F;
            uint32_t imm11 = instr & 0x7FF;
            opcode << "b";
            int32_t imm32;
            if (form == 0) {
              DumpCond(opcode, cond);
              imm32 = (S << 20) | (J2 << 19) | (J1 << 18) | (imm6 << 12) | (imm11 << 1);
              imm32 = (imm32 << 11) >> 11;  // sign extend 21 bit immediate.
            } else {
              uint32_t I1 = ~(J1 ^ S);
              uint32_t I2 = ~(J2 ^ S);
              imm32 = (S << 24) | (I1 << 23) | (I2 << 22) | (imm10 << 12) | (imm11 << 1);
              imm32 = (imm32 << 8) >> 8;  // sign extend 24 bit immediate.
            }
            opcode << ".w";
            DumpBranchTarget(args, instr_ptr + 4, imm32);
            break;
          }
          case 4: case 6: case 5: case 7: {
            // BL, BLX (immediate)
            // |111|11|1|0000000000|11|1 |1|1 |10000000000|
            // |5 3|21|0|9876543  0|54|3 |2|1 |0    5    0|
            // |---|--|-|----------|--|--|-|--|-----------|
            // |332|22|2|2222221111|11|1 |1|1 |10000000000|
            // |1 9|87|6|5    0   6|54|3 |2|1 |0    5    0|
            // |---|--|-|----------|--|--|-|--|-----------|
            // |111|10|S| imm10    |11|J1|L|J2| imm11     |
            uint32_t S = (instr >> 26) & 1;
            uint32_t J2 = (instr >> 11) & 1;
            uint32_t L = (instr >> 12) & 1;
            uint32_t J1 = (instr >> 13) & 1;
            uint32_t imm10 = (instr >> 16) & 0x3FF;
            uint32_t imm11 = instr & 0x7FF;
            if (L == 0) {
              opcode << "bx";
            } else {
              opcode << "blx";
            }
            uint32_t I1 = ~(J1 ^ S);
            uint32_t I2 = ~(J2 ^ S);
            int32_t imm32 = (S << 24) | (I1 << 23) | (I2 << 22) | (imm10 << 12) | (imm11 << 1);
            imm32 = (imm32 << 8) >> 8;  // sign extend 24 bit immediate.
            DumpBranchTarget(args, instr_ptr + 4, imm32);
            break;
          }
        }
      }
      break;
    case 3:
      switch (op2) {
        case 0x00: case 0x02: case 0x04: case 0x06:  // 000xxx0
        case 0x08: case 0x09: case 0x0A: case 0x0C: case 0x0E: {
          // Store single data item
          // |111|11|100|000|0|0000|1111|110000|000000|
          // |5 3|21|098|765|4|3  0|5  2|10   6|5    0|
          // |---|--|---|---|-|----|----|------|------|
          // |332|22|222|222|2|1111|1111|110000|000000|
          // |1 9|87|654|321|0|9  6|5  2|10   6|5    0|
          // |---|--|---|---|-|----|----|------|------|
          // |111|11|000|op3|0|    |    |  op4 |      |
          uint32_t op3 = (instr >> 21) & 7;
          // uint32_t op4 = (instr >> 6) & 0x3F;
          switch (op3) {
            case 0x0: case 0x4: {
              // {ST,LD}RB Rt,[Rn,#+/-imm12]    - 111 11 00 0 1 00 0 nnnn tttt 1 PUWii ii iiii
              // {ST,LD}RB Rt,[Rn,#+/-imm8]     - 111 11 00 0 0 00 0 nnnn tttt 1 PUWii ii iiii
              // {ST,LD}RB Rt,[Rn,Rm,lsl #imm2] - 111 11 00 0 0 00 0 nnnn tttt 0 00000 ii mmmm
              ArmRegister Rn(instr, 16);
              ArmRegister Rt(instr, 12);
              opcode << (HasBitSet(instr, 20) ? "ldrb" : "strb");
              if (HasBitSet(instr, 23)) {
                uint32_t imm12 = instr & 0xFFF;
                args << Rt << ", [" << Rn << ",#" << imm12 << "]";
              } else if ((instr & 0x800) != 0) {
                uint32_t imm8 = instr & 0xFF;
                args << Rt << ", [" << Rn << ",#" << imm8 << "]";
              } else {
                uint32_t imm2 = (instr >> 4) & 3;
                ArmRegister Rm(instr, 0);
                args << Rt << ", [" << Rn << ", " << Rm;
                if (imm2 != 0) {
                  args << ", " << "lsl #" << imm2;
                }
                args << "]";
              }
              break;
            }
            case 0x1: case 0x5: {
              // STRH Rt,[Rn,#+/-imm12]    - 111 11 00 0 1 01 0 nnnn tttt 1 PUWii ii iiii
              // STRH Rt,[Rn,#+/-imm8]     - 111 11 00 0 0 01 0 nnnn tttt 1 PUWii ii iiii
              // STRH Rt,[Rn,Rm,lsl #imm2] - 111 11 00 0 0 01 0 nnnn tttt 0 00000 ii mmmm
              ArmRegister Rn(instr, 16);
              ArmRegister Rt(instr, 12);
              opcode << "strh";
              if (HasBitSet(instr, 23)) {
                uint32_t imm12 = instr & 0xFFF;
                args << Rt << ", [" << Rn << ",#" << imm12 << "]";
              } else if ((instr & 0x800) != 0) {
                uint32_t imm8 = instr & 0xFF;
                args << Rt << ", [" << Rn << ",#" << imm8 << "]";
              } else {
                uint32_t imm2 = (instr >> 4) & 3;
                ArmRegister Rm(instr, 0);
                args << Rt << ", [" << Rn << ", " << Rm;
                if (imm2 != 0) {
                  args << ", " << "lsl #" << imm2;
                }
                args << "]";
              }
              break;
            }
            case 0x2: case 0x6: {
              ArmRegister Rn(instr, 16);
              ArmRegister Rt(instr, 12);
              if (op3 == 2) {
                if ((instr & 0x800) != 0) {
                  // STR Rt, [Rn, #imm8] - 111 11 000 010 0 nnnn tttt 1PUWiiiiiiii
                  uint32_t P = (instr >> 10) & 1;
                  uint32_t U = (instr >> 9) & 1;
                  uint32_t W = (instr >> 8) & 1;
                  uint32_t imm8 = instr & 0xFF;
                  int32_t imm32 = (imm8 << 24) >> 24;  // sign-extend imm8
                  if (Rn.r == 13 && P == 1 && U == 0 && W == 1 && imm32 == 4) {
                    opcode << "push";
                    args << "{" << Rt << "}";
                  } else if (Rn.r == 15 || (P == 0 && W == 0)) {
                    opcode << "UNDEFINED";
                  } else {
                    if (P == 1 && U == 1 && W == 0) {
                      opcode << "strt";
                    } else {
                      opcode << "str";
                    }
                    args << Rt << ", [" << Rn;
                    if (P == 0 && W == 1) {
                      args << "], #" << imm32;
                    } else {
                      args << ", #" << imm32 << "]";
                      if (W == 1) {
                        args << "!";
                      }
                    }
                  }
                } else {
                  // STR Rt, [Rn, Rm, LSL #imm2] - 111 11 000 010 0 nnnn tttt 000000iimmmm
                  ArmRegister Rn(instr, 16);
                  ArmRegister Rt(instr, 12);
                  ArmRegister Rm(instr, 0);
                  uint32_t imm2 = (instr >> 4) & 3;
                  opcode << "str.w";
                  args << Rt << ", [" << Rn << ", " << Rm;
                  if (imm2 != 0) {
                    args << ", lsl #" << imm2;
                  }
                  args << "]";
                }
              } else if (op3 == 6) {
                // STR.W Rt, [Rn, #imm12] - 111 11 000 110 0 nnnn tttt iiiiiiiiiiii
                uint32_t imm12 = instr & 0xFFF;
                opcode << "str.w";
                args << Rt << ", [" << Rn << ", #" << imm12 << "]";
              }
              break;
            }
          }

          break;
        }
        case 0x03: case 0x0B: case 0x11: case 0x13: case 0x19: case 0x1B: {  // 00xx011
          // Load byte/halfword
          // |111|11|10|0 0|00|0|0000|1111|110000|000000|
          // |5 3|21|09|8 7|65|4|3  0|5  2|10   6|5    0|
          // |---|--|--|---|--|-|----|----|------|------|
          // |332|22|22|2 2|22|2|1111|1111|110000|000000|
          // |1 9|87|65|4 3|21|0|9  6|5  2|10   6|5    0|
          // |---|--|--|---|--|-|----|----|------|------|
          // |111|11|00|op3|01|1| Rn | Rt | op4  |      |
          // |111|11| op2       |    |    | imm12       |
          uint32_t op3 = (instr >> 23) & 3;
          ArmRegister Rn(instr, 16);
          ArmRegister Rt(instr, 12);
          if (Rt.r != 15) {
            if (op3 == 1) {
              // LDRH.W Rt, [Rn, #imm12]       - 111 11 00 01 011 nnnn tttt iiiiiiiiiiii
              uint32_t imm12 = instr & 0xFFF;
              opcode << "ldrh.w";
              args << Rt << ", [" << Rn << ", #" << imm12 << "]";
              if (Rn.r == 9) {
                args << "  ; ";
                Thread::DumpThreadOffset<4>(args, imm12);
              } else if (Rn.r == 15) {
                intptr_t lit_adr = reinterpret_cast<intptr_t>(instr_ptr);
                lit_adr = RoundDown(lit_adr, 4) + 4 + imm12;
                args << StringPrintf("  ; 0x%08x", *reinterpret_cast<int32_t*>(lit_adr));
              }
            } else if (op3 == 3) {
              // LDRSH.W Rt, [Rn, #imm12]      - 111 11 00 11 011 nnnn tttt iiiiiiiiiiii
              // LDRSB.W Rt, [Rn, #imm12]      - 111 11 00 11 001 nnnn tttt iiiiiiiiiiii
              uint32_t imm12 = instr & 0xFFF;
              opcode << (HasBitSet(instr, 20) ? "ldrsb.w" : "ldrsh.w");
              args << Rt << ", [" << Rn << ", #" << imm12 << "]";
              if (Rn.r == 9) {
                args << "  ; ";
                Thread::DumpThreadOffset<4>(args, imm12);
              } else if (Rn.r == 15) {
                intptr_t lit_adr = reinterpret_cast<intptr_t>(instr_ptr);
                lit_adr = RoundDown(lit_adr, 4) + 4 + imm12;
                args << StringPrintf("  ; 0x%08x", *reinterpret_cast<int32_t*>(lit_adr));
              }
            }
          }
          break;
        }
        case 0x29: {  // 0101001
          // |111|11|1000000|0000|1111|1100|00|0 0|0000|
          // |5 3|21|0     4|3  0|5  2|1  8|76|5 4|3  0|
          // |---|--|-------|----|----|----|--|---|----|
          // |332|22|2222222|1111|1111|1100|00|0 0|0000|
          // |1 9|87|6     0|9  6|5  2|1  8|76|5 4|3  0|
          // |---|--|-------|----|----|----|--|---|----|
          // |111|11|0101001| Rm |1111| Rd |11|op3| Rm |
          // REV   - 111 11 0101001 mmmm 1111 dddd 1000 mmmm
          // REV16 - 111 11 0101001 mmmm 1111 dddd 1001 mmmm
          // RBIT  - 111 11 0101001 mmmm 1111 dddd 1010 mmmm
          // REVSH - 111 11 0101001 mmmm 1111 dddd 1011 mmmm
          if ((instr & 0xf0c0) == 0xf080) {
            uint32_t op3 = (instr >> 4) & 3;
            opcode << kThumbReverseOperations[op3];
            ArmRegister Rm(instr, 0);
            ArmRegister Rd(instr, 8);
            args << Rd << ", " << Rm;
            ArmRegister Rm2(instr, 16);
            if (Rm.r != Rm2.r || Rm.r == 13 || Rm.r == 15 || Rd.r == 13 || Rd.r == 15) {
              args << " (UNPREDICTABLE)";
            }
          }  // else unknown instruction
          break;
        }
        case 0x05: case 0x0D: case 0x15: case 0x1D: {  // 00xx101
          // Load word
          // |111|11|10|0 0|00|0|0000|1111|110000|000000|
          // |5 3|21|09|8 7|65|4|3  0|5  2|10   6|5    0|
          // |---|--|--|---|--|-|----|----|------|------|
          // |332|22|22|2 2|22|2|1111|1111|110000|000000|
          // |1 9|87|65|4 3|21|0|9  6|5  2|10   6|5    0|
          // |---|--|--|---|--|-|----|----|------|------|
          // |111|11|00|op3|10|1| Rn | Rt | op4  |      |
          // |111|11| op2       |    |    | imm12       |
          uint32_t op3 = (instr >> 23) & 3;
          uint32_t op4 = (instr >> 6) & 0x3F;
          ArmRegister Rn(instr, 16);
          ArmRegister Rt(instr, 12);
          if (op3 == 1 || Rn.r == 15) {
            // LDR.W Rt, [Rn, #imm12]          - 111 11 00 00 101 nnnn tttt iiiiiiiiiiii
            // LDR.W Rt, [PC, #imm12]          - 111 11 00 0x 101 1111 tttt iiiiiiiiiiii
            uint32_t imm12 = instr & 0xFFF;
            opcode << "ldr.w";
            args << Rt << ", [" << Rn << ", #" << imm12 << "]";
            if (Rn.r == 9) {
              args << "  ; ";
              Thread::DumpThreadOffset<4>(args, imm12);
            } else if (Rn.r == 15) {
              intptr_t lit_adr = reinterpret_cast<intptr_t>(instr_ptr);
              lit_adr = RoundDown(lit_adr, 4) + 4 + imm12;
              args << StringPrintf("  ; 0x%08x", *reinterpret_cast<int32_t*>(lit_adr));
            }
          } else if (op4 == 0) {
            // LDR.W Rt, [Rn, Rm{, LSL #imm2}] - 111 11 00 00 101 nnnn tttt 000000iimmmm
            uint32_t imm2 = (instr >> 4) & 0xF;
            ArmRegister rm(instr, 0);
            opcode << "ldr.w";
            args << Rt << ", [" << Rn << ", " << rm;
            if (imm2 != 0) {
              args << ", lsl #" << imm2;
            }
            args << "]";
          } else {
            bool p = (instr & (1 << 10)) != 0;
            bool w = (instr & (1 << 8)) != 0;
            bool u = (instr & (1 << 9)) != 0;
            if (p && u && !w) {
              // LDRT Rt, [Rn, #imm8]            - 111 11 00 00 101 nnnn tttt 1110iiiiiiii
              uint32_t imm8 = instr & 0xFF;
              opcode << "ldrt";
              args << Rt << ", [" << Rn << ", #" << imm8 << "]";
            } else if (Rn.r == 13 && !p && u && w && (instr & 0xff) == 4) {
              // POP
              opcode << "pop";
              args << "{" << Rt << "}";
           } else {
              bool wback = !p || w;
              uint32_t offset = (instr & 0xff);
              opcode << "ldr.w";
              args << Rt << ",";
              if (p && !wback) {
                args << "[" << Rn << ", #" << offset << "]";
              } else if (p && wback) {
                args << "[" << Rn << ", #" << offset << "]!";
              } else if (!p && wback) {
                args << "[" << Rn << "], #" << offset;
              } else {
                LOG(FATAL) << p << " " << w;
              }
            }
          }
          break;
        }
      default:      // more formats
        if ((op2 >> 4) == 2) {      // 010xxxx
          // data processing (register)
          if ((instr & 0x0080f0f0) == 0x0000f000) {
            // LSL, LSR, ASR, ROR
            uint32_t shift_op = (instr >> 21) & 3;
            uint32_t S = (instr >> 20) & 1;
            ArmRegister Rd(instr, 8);
            ArmRegister Rn(instr, 16);
            ArmRegister Rm(instr, 0);
            opcode << kThumb2ShiftOperations[shift_op] << (S != 0 ? "s" : "");
            args << Rd << ", " << Rn << ", " << Rm;
          }
        } else if ((op2 >> 3) == 6) {       // 0110xxx
          // Multiply, multiply accumulate, and absolute difference
          op1 = (instr >> 20) & 0x7;
          op2 = (instr >> 4) & 0x1;
          ArmRegister Ra(instr, 12);
          ArmRegister Rn(instr, 16);
          ArmRegister Rm(instr, 0);
          ArmRegister Rd(instr, 8);
          switch (op1) {
          case 0:
            if (op2 == 0) {
              if (Ra.r == 0xf) {
                opcode << "mul";
                args << Rd << ", " << Rn << ", " << Rm;
              } else {
                opcode << "mla";
                args << Rd << ", " << Rn << ", " << Rm << ", " << Ra;
              }
            } else {
              opcode << "mls";
              args << Rd << ", " << Rn << ", " << Rm << ", " << Ra;
            }
            break;
          case 1:
          case 2:
          case 3:
          case 4:
          case 5:
          case 6:
              break;        // do these sometime
          }
        } else if ((op2 >> 3) == 7) {       // 0111xxx
          // Long multiply, long multiply accumulate, and divide
          op1 = (instr >> 20) & 0x7;
          op2 = (instr >> 4) & 0xf;
          ArmRegister Rn(instr, 16);
          ArmRegister Rm(instr, 0);
          ArmRegister Rd(instr, 8);
          ArmRegister RdHi(instr, 8);
          ArmRegister RdLo(instr, 12);
          switch (op1) {
          case 0:
            opcode << "smull";
            args << RdLo << ", " << RdHi << ", " << Rn << ", " << Rm;
            break;
          case 1:
            opcode << "sdiv";
            args << Rd << ", " << Rn << ", " << Rm;
            break;
          case 2:
            opcode << "umull";
            args << RdLo << ", " << RdHi << ", " << Rn << ", " << Rm;
            break;
          case 3:
            opcode << "udiv";
            args << Rd << ", " << Rn << ", " << Rm;
            break;
          case 4:
          case 5:
          case 6:
            break;      // TODO: when we generate these...
          }
        }
      }
    default:
      break;
  }

  // Apply any IT-block conditions to the opcode if necessary.
  if (!it_conditions_.empty()) {
    opcode << it_conditions_.back();
    it_conditions_.pop_back();
  }
  if (opcode.str().size() == 0) {
    opcode << "UNKNOWN " << op2;
  }

  os << FormatInstructionPointer(instr_ptr)
     << StringPrintf(": %08x\t%-7s ", instr, opcode.str().c_str())
     << args.str() << '\n';
  return 4;
}  // NOLINT(readability/fn_size)

size_t DisassemblerArm::DumpThumb16(std::ostream& os, const uint8_t* instr_ptr) {
  uint16_t instr = ReadU16(instr_ptr);
  bool is_32bit = ((instr & 0xF000) == 0xF000) || ((instr & 0xF800) == 0xE800);
  if (is_32bit) {
    return DumpThumb32(os, instr_ptr);
  } else {
    std::ostringstream opcode;
    std::ostringstream args;
    uint16_t opcode1 = instr >> 10;
    if (opcode1 < 0x10) {
      // shift (immediate), add, subtract, move, and compare
      uint16_t opcode2 = instr >> 9;
      switch (opcode2) {
        case 0x0: case 0x1: case 0x2: case 0x3: case 0x4: case 0x5: case 0x6: case 0x7:
        case 0x8: case 0x9: case 0xA: case 0xB: {
          // Logical shift left     - 00 000xx iii mmm ddd
          // Logical shift right    - 00 001xx iii mmm ddd
          // Arithmetic shift right - 00 010xx iii mmm ddd
          uint16_t imm5 = (instr >> 6) & 0x1F;
          ThumbRegister rm(instr, 3);
          ThumbRegister Rd(instr, 0);
          if (opcode2 <= 3) {
            opcode << "lsls";
          } else if (opcode2 <= 7) {
            opcode << "lsrs";
          } else {
            opcode << "asrs";
          }
          args << Rd << ", " << rm << ", #" << imm5;
          break;
        }
        case 0xC: case 0xD: case 0xE: case 0xF: {
          // Add register        - 00 01100 mmm nnn ddd
          // Sub register        - 00 01101 mmm nnn ddd
          // Add 3-bit immediate - 00 01110 iii nnn ddd
          // Sub 3-bit immediate - 00 01111 iii nnn ddd
          uint16_t imm3_or_Rm = (instr >> 6) & 7;
          ThumbRegister Rn(instr, 3);
          ThumbRegister Rd(instr, 0);
          if ((opcode2 & 2) != 0 && imm3_or_Rm == 0) {
            opcode << "mov";
          } else {
            if ((opcode2 & 1) == 0) {
              opcode << "adds";
            } else {
              opcode << "subs";
            }
          }
          args << Rd << ", " << Rn;
          if ((opcode2 & 2) == 0) {
            ArmRegister Rm(imm3_or_Rm);
            args << ", " << Rm;
          } else if (imm3_or_Rm != 0) {
            args << ", #" << imm3_or_Rm;
          }
          break;
        }
        case 0x10: case 0x11: case 0x12: case 0x13:
        case 0x14: case 0x15: case 0x16: case 0x17:
        case 0x18: case 0x19: case 0x1A: case 0x1B:
        case 0x1C: case 0x1D: case 0x1E: case 0x1F: {
          // MOVS Rd, #imm8 - 00100 ddd iiiiiiii
          // CMP  Rn, #imm8 - 00101 nnn iiiiiiii
          // ADDS Rn, #imm8 - 00110 nnn iiiiiiii
          // SUBS Rn, #imm8 - 00111 nnn iiiiiiii
          ThumbRegister Rn(instr, 8);
          uint16_t imm8 = instr & 0xFF;
          switch (opcode2 >> 2) {
            case 4: opcode << "movs"; break;
            case 5: opcode << "cmp"; break;
            case 6: opcode << "adds"; break;
            case 7: opcode << "subs"; break;
          }
          args << Rn << ", #" << imm8;
          break;
        }
        default:
          break;
      }
    } else if (opcode1 == 0x10) {
      // Data-processing
      uint16_t opcode2 = (instr >> 6) & 0xF;
      ThumbRegister rm(instr, 3);
      ThumbRegister rdn(instr, 0);
      opcode << kThumbDataProcessingOperations[opcode2];
      args << rdn << ", " << rm;
    } else if (opcode1 == 0x11) {
      // Special data instructions and branch and exchange
      uint16_t opcode2 = (instr >> 6) & 0x0F;
      switch (opcode2) {
        case 0x0: case 0x1: case 0x2: case 0x3: {
          // Add low registers  - 010001 0000 xxxxxx
          // Add high registers - 010001 0001/001x xxxxxx
          uint16_t DN = (instr >> 7) & 1;
          ArmRegister rm(instr, 3);
          uint16_t Rdn = instr & 7;
          ArmRegister DN_Rdn((DN << 3) | Rdn);
          opcode << "add";
          args << DN_Rdn << ", " << rm;
          break;
        }
        case 0x8: case 0x9: case 0xA: case 0xB: {
          // Move low registers  - 010001 1000 xxxxxx
          // Move high registers - 010001 1001/101x xxxxxx
          uint16_t DN = (instr >> 7) & 1;
          ArmRegister rm(instr, 3);
          uint16_t Rdn = instr & 7;
          ArmRegister DN_Rdn((DN << 3) | Rdn);
          opcode << "mov";
          args << DN_Rdn << ", " << rm;
          break;
        }
        case 0x5: case 0x6: case 0x7: {
          // Compare high registers - 010001 0101/011x xxxxxx
          uint16_t N = (instr >> 7) & 1;
          ArmRegister rm(instr, 3);
          uint16_t Rn = instr & 7;
          ArmRegister N_Rn((N << 3) | Rn);
          opcode << "cmp";
          args << N_Rn << ", " << rm;
          break;
        }
        case 0xC: case 0xD: case 0xE: case 0xF: {
          // Branch and exchange           - 010001 110x xxxxxx
          // Branch with link and exchange - 010001 111x xxxxxx
          ArmRegister rm(instr, 3);
          opcode << ((opcode2 & 0x2) == 0 ? "bx" : "blx");
          args << rm;
          break;
        }
        default:
          break;
      }
    } else if (opcode1 == 0x12 || opcode1 == 0x13) {  // 01001x
      ThumbRegister Rt(instr, 8);
      uint16_t imm8 = instr & 0xFF;
      opcode << "ldr";
      args << Rt << ", [pc, #" << (imm8 << 2) << "]";
    } else if ((opcode1 >= 0x14 && opcode1 <= 0x17) ||  // 0101xx
               (opcode1 >= 0x18 && opcode1 <= 0x1f) ||  // 011xxx
               (opcode1 >= 0x20 && opcode1 <= 0x27)) {  // 100xxx
      // Load/store single data item
      uint16_t opA = (instr >> 12) & 0xF;
      if (opA == 0x5) {
        uint16_t opB = (instr >> 9) & 0x7;
        ThumbRegister Rm(instr, 6);
        ThumbRegister Rn(instr, 3);
        ThumbRegister Rt(instr, 0);
        switch (opB) {
          case 0: opcode << "str"; break;
          case 1: opcode << "strh"; break;
          case 2: opcode << "strb"; break;
          case 3: opcode << "ldrsb"; break;
          case 4: opcode << "ldr"; break;
          case 5: opcode << "ldrh"; break;
          case 6: opcode << "ldrb"; break;
          case 7: opcode << "ldrsh"; break;
        }
        args << Rt << ", [" << Rn << ", " << Rm << "]";
      } else if (opA == 9) {
        uint16_t opB = (instr >> 11) & 1;
        ThumbRegister Rt(instr, 8);
        uint16_t imm8 = instr & 0xFF;
        opcode << (opB == 0 ? "str" : "ldr");
        args << Rt << ", [sp, #" << (imm8 << 2) << "]";
      } else {
        uint16_t imm5 = (instr >> 6) & 0x1F;
        uint16_t opB = (instr >> 11) & 1;
        ThumbRegister Rn(instr, 3);
        ThumbRegister Rt(instr, 0);
        switch (opA) {
          case 6:
            imm5 <<= 2;
            opcode << (opB == 0 ? "str" : "ldr");
            break;
          case 7:
            imm5 <<= 0;
            opcode << (opB == 0 ? "strb" : "ldrb");
            break;
          case 8:
            imm5 <<= 1;
            opcode << (opB == 0 ? "strh" : "ldrh");
            break;
        }
        args << Rt << ", [" << Rn << ", #" << imm5 << "]";
      }
    } else if (opcode1 >= 0x34 && opcode1 <= 0x37) {  // 1101xx
      int8_t imm8 = instr & 0xFF;
      uint32_t cond = (instr >> 8) & 0xF;
      opcode << "b";
      DumpCond(opcode, cond);
      DumpBranchTarget(args, instr_ptr + 4, (imm8 << 1));
    } else if ((instr & 0xF800) == 0xA800) {
      // Generate SP-relative address
      ThumbRegister rd(instr, 8);
      int imm8 = instr & 0xFF;
      opcode << "add";
      args << rd << ", sp, #" << (imm8 << 2);
    } else if ((instr & 0xF000) == 0xB000) {
      // Miscellaneous 16-bit instructions
      uint16_t opcode2 = (instr >> 5) & 0x7F;
      switch (opcode2) {
        case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: {
          // Add immediate to SP        - 1011 00000 ii iiiii
          // Subtract immediate from SP - 1011 00001 ii iiiii
          int imm7 = instr & 0x7F;
          opcode << ((opcode2 & 4) == 0 ? "add" : "sub");
          args << "sp, sp, #" << (imm7 << 2);
          break;
        }
        case 0x08: case 0x09: case 0x0A: case 0x0B:  // 0001xxx
        case 0x0C: case 0x0D: case 0x0E: case 0x0F:
        case 0x18: case 0x19: case 0x1A: case 0x1B:  // 0011xxx
        case 0x1C: case 0x1D: case 0x1E: case 0x1F:
        case 0x48: case 0x49: case 0x4A: case 0x4B:  // 1001xxx
        case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        case 0x58: case 0x59: case 0x5A: case 0x5B:  // 1011xxx
        case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
          // CBNZ, CBZ
          uint16_t op = (instr >> 11) & 1;
          uint16_t i = (instr >> 9) & 1;
          uint16_t imm5 = (instr >> 3) & 0x1F;
          ThumbRegister Rn(instr, 0);
          opcode << (op != 0 ? "cbnz" : "cbz");
          uint32_t imm32 = (i << 6) | (imm5 << 1);
          args << Rn << ", ";
          DumpBranchTarget(args, instr_ptr + 4, imm32);
          break;
        }
        case 0x50: case 0x51:    // 101000x
        case 0x52: case 0x53:    // 101001x
        case 0x56: case 0x57: {  // 101011x
          uint16_t op = (instr >> 6) & 3;
          opcode << kThumbReverseOperations[op];
          ThumbRegister Rm(instr, 3);
          ThumbRegister Rd(instr, 0);
          args << Rd << ", " << Rm;
          break;
        }
        case 0x78: case 0x79: case 0x7A: case 0x7B:  // 1111xxx
        case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
          // If-Then, and hints
          uint16_t opA = (instr >> 4) & 0xF;
          uint16_t opB = instr & 0xF;
          if (opB == 0) {
            switch (opA) {
              case 0: opcode << "nop"; break;
              case 1: opcode << "yield"; break;
              case 2: opcode << "wfe";  break;
              case 3: opcode << "sev"; break;
              default: break;
            }
          } else {
            uint32_t first_cond = opA;
            uint32_t mask = opB;
            opcode << "it";

            // Flesh out the base "it" opcode with the specific collection of 't's and 'e's,
            // and store up the actual condition codes we'll want to add to the next few opcodes.
            size_t count = 3 - CTZ(mask);
            it_conditions_.resize(count + 2);  // Plus the implicit 't', plus the "" for the IT itself.
            for (size_t i = 0; i < count; ++i) {
              bool positive_cond = ((first_cond & 1) != 0);
              bool positive_mask = ((mask & (1 << (3 - i))) != 0);
              if (positive_mask == positive_cond) {
                opcode << 't';
                it_conditions_[i] = kConditionCodeNames[first_cond];
              } else {
                opcode << 'e';
                it_conditions_[i] = kConditionCodeNames[first_cond ^ 1];
              }
            }
            it_conditions_[count] = kConditionCodeNames[first_cond];  // The implicit 't'.

            it_conditions_[count + 1] = "";  // No condition code for the IT itself...
            DumpCond(args, first_cond);  // ...because it's considered an argument.
          }
          break;
        }
        default:
          break;
      }
    } else if (((instr & 0xF000) == 0x5000) || ((instr & 0xE000) == 0x6000) ||
        ((instr & 0xE000) == 0x8000)) {
      // Load/store single data item
      uint16_t opA = instr >> 12;
      // uint16_t opB = (instr >> 9) & 7;
      switch (opA) {
        case 0x6: {
          // STR Rt, [Rn, #imm] - 01100 iiiii nnn ttt
          // LDR Rt, [Rn, #imm] - 01101 iiiii nnn ttt
          uint16_t imm5 = (instr >> 6) & 0x1F;
          ThumbRegister Rn(instr, 3);
          ThumbRegister Rt(instr, 0);
          opcode << ((instr & 0x800) == 0 ? "str" : "ldr");
          args << Rt << ", [" << Rn << ", #" << (imm5 << 2) << "]";
          break;
        }
        case 0x9: {
          // STR Rt, [SP, #imm] - 01100 ttt iiiiiiii
          // LDR Rt, [SP, #imm] - 01101 ttt iiiiiiii
          uint16_t imm8 = instr & 0xFF;
          ThumbRegister Rt(instr, 8);
          opcode << ((instr & 0x800) == 0 ? "str" : "ldr");
          args << Rt << ", [sp, #" << (imm8 << 2) << "]";
          break;
        }
        default:
          break;
      }
    } else if (opcode1 == 0x38 || opcode1 == 0x39) {
      uint16_t imm11 = instr & 0x7FFF;
      int32_t imm32 = imm11 << 1;
      imm32 = (imm32 << 20) >> 20;  // sign extend 12 bit immediate
      opcode << "b";
      DumpBranchTarget(args, instr_ptr + 4, imm32);
    }

    // Apply any IT-block conditions to the opcode if necessary.
    if (!it_conditions_.empty()) {
      opcode << it_conditions_.back();
      it_conditions_.pop_back();
    }

    os << FormatInstructionPointer(instr_ptr)
       << StringPrintf(": %04x    \t%-7s ", instr, opcode.str().c_str())
       << args.str() << '\n';
  }
  return 2;
}

}  // namespace arm
}  // namespace art
