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

#include "disassembler_mips64.h"

#include <ostream>
#include <sstream>

#include "base/logging.h"
#include "base/stringprintf.h"
#include "thread.h"

namespace art {
namespace mips64 {


struct Mips64Instruction {
  uint32_t    mask;
  uint32_t    value;
  const char* name;
  const char* args_fmt;

  bool Matches(uint32_t instruction) const {
    return (instruction & mask) == value;
  }
};

static const uint32_t kOpcodeShift = 26;
static const uint32_t kCop1 = (17 << kOpcodeShift);
static const uint32_t kITypeMask = (0x3f << kOpcodeShift);
static const uint32_t kJTypeMask = (0x3f << kOpcodeShift);
static const uint32_t kRTypeMask = ((0x3f << kOpcodeShift) | (0x3f));
static const uint32_t kSpecial0Mask = (0x3f << kOpcodeShift);
static const uint32_t kFpMask = kRTypeMask;

static const Mips64Instruction gMips64Instructions[] = {
  // "sll r0, r0, 0" is the canonical "nop", used in delay slots.
  { 0xffffffff, 0, "nop", "" },

  // R-type instructions.
  { kRTypeMask, 0, "sll", "DTA", },
  // 0, 1, movci
  { kRTypeMask, 2, "srl", "DTA", },
  { kRTypeMask, 3, "sra", "DTA", },
  { kRTypeMask, 4, "sllv", "DTS", },
  { kRTypeMask, 6, "srlv", "DTS", },
  { kRTypeMask, 7, "srav", "DTS", },
  { kRTypeMask | (0x1f << 11), 9 | (31 << 11), "jalr", "S", },  // rd = 31 is implicit.
  { kRTypeMask | (0x1f << 11), 9, "jr", "S", },  // rd = 0 is implicit.
  { kRTypeMask, 9, "jalr", "DS", },  // General case.
  { kRTypeMask, 12, "syscall", "", },  // TODO: code
  { kRTypeMask, 13, "break", "", },  // TODO: code
  { kRTypeMask, 15, "sync", "", },  // TODO: type
  { kRTypeMask, 20, "dsllv", "DTS", },
  { kRTypeMask, 22, "dsrlv", "DTS", },
  { kRTypeMask, 23, "dsrav", "DTS", },
  { kRTypeMask, 33, "addu", "DST", },
  { kRTypeMask, 34, "sub", "DST", },
  { kRTypeMask, 35, "subu", "DST", },
  { kRTypeMask, 36, "and", "DST", },
  { kRTypeMask, 37, "or", "DST", },
  { kRTypeMask, 38, "xor", "DST", },
  { kRTypeMask, 39, "nor", "DST", },
  { kRTypeMask, 42, "slt", "DST", },
  { kRTypeMask, 43, "sltu", "DST", },
  { kRTypeMask, 45, "daddu", "DST", },
  { kRTypeMask, 46, "dsub", "DST", },
  { kRTypeMask, 47, "dsubu", "DST", },
  // TODO: seleqz, selnez
  { kRTypeMask, 56, "dsll", "DTA", },
  { kRTypeMask, 58, "dsrl", "DTA", },
  { kRTypeMask, 59, "dsra", "DTA", },
  { kRTypeMask, 60, "dsll32", "DTA", },
  { kRTypeMask | (0x1f << 21), 62 | (1 << 21), "drotr32", "DTA", },
  { kRTypeMask, 62, "dsrl32", "DTA", },
  { kRTypeMask, 63, "dsra32", "DTA", },

  // SPECIAL0
  { kSpecial0Mask | 0x7ff, (2 << 6) | 24, "mul", "DST" },
  { kSpecial0Mask | 0x7ff, (3 << 6) | 24, "muh", "DST" },
  { kSpecial0Mask | 0x7ff, (2 << 6) | 25, "mulu", "DST" },
  { kSpecial0Mask | 0x7ff, (3 << 6) | 25, "muhu", "DST" },
  { kSpecial0Mask | 0x7ff, (2 << 6) | 26, "div", "DST" },
  { kSpecial0Mask | 0x7ff, (3 << 6) | 26, "mod", "DST" },
  { kSpecial0Mask | 0x7ff, (2 << 6) | 27, "divu", "DST" },
  { kSpecial0Mask | 0x7ff, (3 << 6) | 27, "modu", "DST" },
  { kSpecial0Mask | 0x7ff, (2 << 6) | 28, "dmul", "DST" },
  { kSpecial0Mask | 0x7ff, (3 << 6) | 28, "dmuh", "DST" },
  { kSpecial0Mask | 0x7ff, (2 << 6) | 29, "dmulu", "DST" },
  { kSpecial0Mask | 0x7ff, (3 << 6) | 29, "dmuhu", "DST" },
  { kSpecial0Mask | 0x7ff, (2 << 6) | 30, "ddiv", "DST" },
  { kSpecial0Mask | 0x7ff, (3 << 6) | 30, "dmod", "DST" },
  { kSpecial0Mask | 0x7ff, (2 << 6) | 31, "ddivu", "DST" },
  { kSpecial0Mask | 0x7ff, (3 << 6) | 31, "dmodu", "DST" },
  // TODO: [d]clz, [d]clo
  // TODO: sdbbp

  // J-type instructions.
  { kJTypeMask, 2 << kOpcodeShift, "j", "L" },
  { kJTypeMask, 3 << kOpcodeShift, "jal", "L" },

  // I-type instructions.
  { kITypeMask, 4 << kOpcodeShift, "beq", "STB" },
  { kITypeMask, 5 << kOpcodeShift, "bne", "STB" },
  { kITypeMask | (0x1f << 16), 1 << kOpcodeShift | (1 << 16), "bgez", "SB" },
  { kITypeMask | (0x1f << 16), 1 << kOpcodeShift | (0 << 16), "bltz", "SB" },
  { kITypeMask | (0x1f << 16), 6 << kOpcodeShift | (0 << 16), "blez", "SB" },
  { kITypeMask | (0x1f << 16), 7 << kOpcodeShift | (0 << 16), "bgtz", "SB" },
  { kITypeMask | (0x1f << 16), 1 << kOpcodeShift | (6 << 16), "dahi", "Si", },
  { kITypeMask | (0x1f << 16), 1 << kOpcodeShift | (30 << 16), "dati", "Si", },

  { 0xffff0000, (4 << kOpcodeShift), "b", "B" },
  { 0xffff0000, (1 << kOpcodeShift) | (17 << 16), "bal", "B" },

  { kITypeMask, 9 << kOpcodeShift, "addiu", "TSi", },
  { kITypeMask, 10 << kOpcodeShift, "slti", "TSi", },
  { kITypeMask, 11 << kOpcodeShift, "sltiu", "TSi", },
  { kITypeMask, 12 << kOpcodeShift, "andi", "TSi", },
  { kITypeMask, 13 << kOpcodeShift, "ori", "TSi", },
  { kITypeMask, 14 << kOpcodeShift, "xori", "TSi", },
  { kITypeMask | (0x1f << 21), 15 << kOpcodeShift, "lui", "TI", },
  { kITypeMask, 15 << kOpcodeShift, "aui", "TSI", },
  { kITypeMask, 25 << kOpcodeShift, "daddiu", "TSi", },
  { kITypeMask, 29 << kOpcodeShift, "daui", "TSi", },

  { kITypeMask, 32u << kOpcodeShift, "lb", "TO", },
  { kITypeMask, 33u << kOpcodeShift, "lh", "TO", },
  { kITypeMask, 35u << kOpcodeShift, "lw", "TO", },
  { kITypeMask, 36u << kOpcodeShift, "lbu", "TO", },
  { kITypeMask, 37u << kOpcodeShift, "lhu", "TO", },
  { kITypeMask, 39u << kOpcodeShift, "lwu", "TO", },
  { kITypeMask, 40u << kOpcodeShift, "sb", "TO", },
  { kITypeMask, 41u << kOpcodeShift, "sh", "TO", },
  { kITypeMask, 43u << kOpcodeShift, "sw", "TO", },
  { kITypeMask, 49u << kOpcodeShift, "lwc1", "tO", },
  { kITypeMask, 53u << kOpcodeShift, "ldc1", "tO", },
  { kITypeMask, 55u << kOpcodeShift, "ld", "TO", },
  { kITypeMask, 57u << kOpcodeShift, "swc1", "tO", },
  { kITypeMask, 61u << kOpcodeShift, "sdc1", "tO", },
  { kITypeMask, 63u << kOpcodeShift, "sd", "TO", },

  // Floating point.
  { kFpMask | (0x1f << 21), kCop1 | (0x00 << 21), "mfc1", "Td" },
  { kFpMask | (0x1f << 21), kCop1 | (0x01 << 21), "dmfc1", "Td" },
  { kFpMask | (0x1f << 21), kCop1 | (0x04 << 21), "mtc1", "Td" },
  { kFpMask | (0x1f << 21), kCop1 | (0x05 << 21), "dmtc1", "Td" },
  { kFpMask | (0x10 << 21), kCop1 | (0x10 << 21) | 0, "add", "fadt" },
  { kFpMask | (0x10 << 21), kCop1 | (0x10 << 21) | 1, "sub", "fadt" },
  { kFpMask | (0x10 << 21), kCop1 | (0x10 << 21) | 2, "mul", "fadt" },
  { kFpMask | (0x10 << 21), kCop1 | (0x10 << 21) | 3, "div", "fadt" },
  { kFpMask | (0x10 << 21), kCop1 | (0x10 << 21) | 4, "sqrt", "fad" },
  { kFpMask | (0x21f << 16), kCop1 | (0x200 << 16) | 5, "abs", "fad" },
  { kFpMask | (0x21f << 16), kCop1 | (0x200 << 16) | 6, "mov", "fad" },
  { kFpMask | (0x21f << 16), kCop1 | (0x200 << 16) | 7, "neg", "fad" },
  { kFpMask | (0x21f << 16), kCop1 | (0x200 << 16) | 8, "round.l", "fad" },
  { kFpMask | (0x21f << 16), kCop1 | (0x200 << 16) | 9, "trunc.l", "fad" },
  { kFpMask | (0x21f << 16), kCop1 | (0x200 << 16) | 10, "ceil.l", "fad" },
  { kFpMask | (0x21f << 16), kCop1 | (0x200 << 16) | 11, "floor.l", "fad" },
  { kFpMask | (0x21f << 16), kCop1 | (0x200 << 16) | 12, "round.w", "fad" },
  { kFpMask | (0x21f << 16), kCop1 | (0x200 << 16) | 13, "trunc.w", "fad" },
  { kFpMask | (0x21f << 16), kCop1 | (0x200 << 16) | 14, "ceil.w", "fad" },
  { kFpMask | (0x21f << 16), kCop1 | (0x200 << 16) | 15, "floor.w", "fad" },
  { kFpMask | (0x21f << 16), kCop1 | (0x200 << 16) | 32, "cvt.s", "fad" },
  { kFpMask | (0x21f << 16), kCop1 | (0x200 << 16) | 33, "cvt.d", "fad" },
  { kFpMask | (0x21f << 16), kCop1 | (0x200 << 16) | 36, "cvt.w", "fad" },
  { kFpMask | (0x21f << 16), kCop1 | (0x200 << 16) | 37, "cvt.l", "fad" },
  { kFpMask | (0x21f << 16), kCop1 | (0x200 << 16) | 38, "cvt.ps", "fad" },
};

static uint32_t ReadU32(const uint8_t* ptr) {
  // We only support little-endian MIPS64.
  return ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24);
}

size_t DisassemblerMips64::Dump(std::ostream& os, const uint8_t* instr_ptr) {
  uint32_t instruction = ReadU32(instr_ptr);

  uint32_t rs = (instruction >> 21) & 0x1f;  // I-type, R-type.
  uint32_t rt = (instruction >> 16) & 0x1f;  // I-type, R-type.
  uint32_t rd = (instruction >> 11) & 0x1f;  // R-type.
  uint32_t sa = (instruction >>  6) & 0x1f;  // R-type.

  std::string opcode;
  std::ostringstream args;

  // TODO: remove this!
  uint32_t op = (instruction >> 26) & 0x3f;
  uint32_t function = (instruction & 0x3f);  // R-type.
  opcode = StringPrintf("op=%d fn=%d", op, function);

  for (size_t i = 0; i < arraysize(gMips64Instructions); ++i) {
    if (gMips64Instructions[i].Matches(instruction)) {
      opcode = gMips64Instructions[i].name;
      for (const char* args_fmt = gMips64Instructions[i].args_fmt; *args_fmt; ++args_fmt) {
        switch (*args_fmt) {
          case 'A':  // sa (shift amount).
            args << sa;
            break;
          case 'B':  // Branch offset.
            {
              int32_t offset = static_cast<int16_t>(instruction & 0xffff);
              offset <<= 2;
              offset += 4;  // Delay slot.
              args << StringPrintf("%p  ; %+d", instr_ptr + offset, offset);
            }
            break;
          case 'D': args << 'r' << rd; break;
          case 'd': args << 'f' << rd; break;
          case 'a': args << 'f' << sa; break;
          case 'f':  // Floating point "fmt".
            {
              size_t fmt = (instruction >> 21) & 0x7;  // TODO: other fmts?
              switch (fmt) {
                case 0: opcode += ".s"; break;
                case 1: opcode += ".d"; break;
                case 4: opcode += ".w"; break;
                case 5: opcode += ".l"; break;
                case 6: opcode += ".ps"; break;
                default: opcode += ".?"; break;
              }
              continue;  // No ", ".
            }
          case 'I':  // Upper 16-bit immediate.
            args << reinterpret_cast<void*>((instruction & 0xffff) << 16);
            break;
          case 'i':  // Sign-extended lower 16-bit immediate.
            args << static_cast<int16_t>(instruction & 0xffff);
            break;
          case 'L':  // Jump label.
            {
              // TODO: is this right?
              uint32_t instr_index = (instruction & 0x1ffffff);
              uint32_t target = (instr_index << 2);
              target |= (reinterpret_cast<uintptr_t>(instr_ptr + 4)
                        & 0xf0000000);
              args << reinterpret_cast<void*>(target);
            }
            break;
          case 'O':  // +x(rs)
            {
              int32_t offset = static_cast<int16_t>(instruction & 0xffff);
              args << StringPrintf("%+d(r%d)", offset, rs);
              if (rs == 17) {
                args << "  ; ";
                Thread::DumpThreadOffset<8>(args, offset);
              }
            }
            break;
          case 'S': args << 'r' << rs; break;
          case 's': args << 'f' << rs; break;
          case 'T': args << 'r' << rt; break;
          case 't': args << 'f' << rt; break;
        }
        if (*(args_fmt + 1)) {
          args << ", ";
        }
      }
      break;
    }
  }

  os << FormatInstructionPointer(instr_ptr)
     << StringPrintf(": %08x\t%-7s ", instruction, opcode.c_str())
     << args.str() << '\n';
  return 4;
}

void DisassemblerMips64::Dump(std::ostream& os, const uint8_t* begin,
                            const uint8_t* end) {
  for (const uint8_t* cur = begin; cur < end; cur += 4) {
    Dump(os, cur);
  }
}

}  // namespace mips64
}  // namespace art
