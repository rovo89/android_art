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

#include "disassembler_mips.h"

#include <ostream>
#include <sstream>

#include "base/logging.h"
#include "base/stringprintf.h"
#include "thread.h"

namespace art {
namespace mips {

struct MipsInstruction {
  uint32_t mask;
  uint32_t value;
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
static const uint32_t kSpecial2Mask = (0x3f << kOpcodeShift);
static const uint32_t kFpMask = kRTypeMask;

static const MipsInstruction gMipsInstructions[] = {
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
  { kRTypeMask, 8, "jr", "S", },
  { kRTypeMask | (0x1f << 11), 9 | (31 << 11), "jalr", "S", },  // rd = 31 is implicit.
  { kRTypeMask | (0x1f << 11), 9, "jr", "S", },  // rd = 0 is implicit.
  { kRTypeMask, 9, "jalr", "DS", },  // General case.
  { kRTypeMask | (0x1f << 6), 10, "movz", "DST", },
  { kRTypeMask | (0x1f << 6), 11, "movn", "DST", },
  { kRTypeMask, 12, "syscall", "", },  // TODO: code
  { kRTypeMask, 13, "break", "", },  // TODO: code
  { kRTypeMask, 15, "sync", "", },  // TODO: type
  { kRTypeMask, 16, "mfhi", "D", },
  { kRTypeMask, 17, "mthi", "S", },
  { kRTypeMask, 18, "mflo", "D", },
  { kRTypeMask, 19, "mtlo", "S", },
  { kRTypeMask, 20, "dsllv", "DTS", },
  { kRTypeMask, 22, "dsrlv", "DTS", },
  { kRTypeMask, 23, "dsrav", "DTS", },
  { kRTypeMask | (0x1f << 6), 24, "mult", "ST", },
  { kRTypeMask | (0x1f << 6), 25, "multu", "ST", },
  { kRTypeMask | (0x1f << 6), 26, "div", "ST", },
  { kRTypeMask | (0x1f << 6), 27, "divu", "ST", },
  { kRTypeMask | (0x1f << 6), 24 + (2 << 6), "mul", "DST", },
  { kRTypeMask | (0x1f << 6), 24 + (3 << 6), "muh", "DST", },
  { kRTypeMask | (0x1f << 6), 26 + (2 << 6), "div", "DST", },
  { kRTypeMask | (0x1f << 6), 26 + (3 << 6), "mod", "DST", },
  { kRTypeMask, 32, "add", "DST", },
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
  // TODO: tge[u], tlt[u], teg, tne
  // TODO: seleqz, selnez
  { kRTypeMask, 56, "dsll", "DTA", },
  { kRTypeMask, 58, "dsrl", "DTA", },
  { kRTypeMask, 59, "dsra", "DTA", },
  { kRTypeMask, 60, "dsll32", "DTA", },
  { kRTypeMask | (0x1f << 21), 62 | (1 << 21), "drotr32", "DTA", },
  { kRTypeMask, 62, "dsrl32", "DTA", },
  { kRTypeMask, 63, "dsra32", "DTA", },
  { kRTypeMask, (31u << kOpcodeShift) | 3, "dext", "TSAZ", },
  { kRTypeMask | (0x1f << 21) | (0x1f << 6), (31u << 26) | (16 << 6) | 32, "seb", "DT", },
  { kRTypeMask | (0x1f << 21) | (0x1f << 6), (31u << 26) | (24 << 6) | 32, "seh", "DT", },

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

  // SPECIAL2
  { kSpecial2Mask | 0x7ff, (28 << kOpcodeShift) | 2, "mul", "DST" },
  { kSpecial2Mask | 0x7ff, (28 << kOpcodeShift) | 32, "clz", "DS" },
  { kSpecial2Mask | 0xffff, (28 << kOpcodeShift) | 0, "madd", "ST" },
  { kSpecial2Mask | 0xffff, (28 << kOpcodeShift) | 1, "maddu", "ST" },
  { kSpecial2Mask | 0xffff, (28 << kOpcodeShift) | 2, "mul", "DST" },
  { kSpecial2Mask | 0xffff, (28 << kOpcodeShift) | 4, "msub", "ST" },
  { kSpecial2Mask | 0xffff, (28 << kOpcodeShift) | 5, "msubu", "ST" },
  { kSpecial2Mask | 0x3f, (28 << kOpcodeShift) | 0x3f, "sdbbp", "" },  // TODO: code

  // J-type instructions.
  { kJTypeMask, 2 << kOpcodeShift, "j", "L" },
  { kJTypeMask, 3 << kOpcodeShift, "jal", "L" },

  // I-type instructions.
  { kITypeMask, 4 << kOpcodeShift, "beq", "STB" },
  { kITypeMask, 5 << kOpcodeShift, "bne", "STB" },
  { kITypeMask | (0x1f << 16), 1 << kOpcodeShift | (1 << 16), "bgez", "SB" },
  { kITypeMask | (0x1f << 16), 1 << kOpcodeShift | (0 << 16), "bltz", "SB" },
  { kITypeMask | (0x1f << 16), 1 << kOpcodeShift | (2 << 16), "bltzl", "SB" },
  { kITypeMask | (0x1f << 16), 1 << kOpcodeShift | (16 << 16), "bltzal", "SB" },
  { kITypeMask | (0x1f << 16), 1 << kOpcodeShift | (18 << 16), "bltzall", "SB" },
  { kITypeMask | (0x1f << 16), 6 << kOpcodeShift | (0 << 16), "blez", "SB" },
  { kITypeMask, 6 << kOpcodeShift, "bgeuc", "STB" },
  { kITypeMask | (0x1f << 16), 7 << kOpcodeShift | (0 << 16), "bgtz", "SB" },
  { kITypeMask, 7 << kOpcodeShift, "bltuc", "STB" },
  { kITypeMask | (0x1f << 16), 1 << kOpcodeShift | (6 << 16), "dahi", "Si", },
  { kITypeMask | (0x1f << 16), 1 << kOpcodeShift | (30 << 16), "dati", "Si", },

  { 0xffff0000, (4 << kOpcodeShift), "b", "B" },
  { 0xffff0000, (1 << kOpcodeShift) | (17 << 16), "bal", "B" },

  { kITypeMask, 8 << kOpcodeShift, "beqc", "STB" },

  { kITypeMask, 8 << kOpcodeShift, "addi", "TSi", },
  { kITypeMask, 9 << kOpcodeShift, "addiu", "TSi", },
  { kITypeMask, 10 << kOpcodeShift, "slti", "TSi", },
  { kITypeMask, 11 << kOpcodeShift, "sltiu", "TSi", },
  { kITypeMask, 12 << kOpcodeShift, "andi", "TSi", },
  { kITypeMask, 13 << kOpcodeShift, "ori", "TSi", },
  { kITypeMask, 14 << kOpcodeShift, "xori", "TSi", },
  { kITypeMask | (0x1f << 21), 15 << kOpcodeShift, "lui", "TI", },
  { kITypeMask, 15 << kOpcodeShift, "aui", "TSI", },

  { kITypeMask | (0x1f << 21), 22 << kOpcodeShift, "blezc", "TB" },

  // TODO: de-dup
  { kITypeMask | (0x3ff << 16), (22 << kOpcodeShift) | (1  << 21) | (1  << 16), "bgezc", "TB" },
  { kITypeMask | (0x3ff << 16), (22 << kOpcodeShift) | (2  << 21) | (2  << 16), "bgezc", "TB" },
  { kITypeMask | (0x3ff << 16), (22 << kOpcodeShift) | (3  << 21) | (3  << 16), "bgezc", "TB" },
  { kITypeMask | (0x3ff << 16), (22 << kOpcodeShift) | (4  << 21) | (4  << 16), "bgezc", "TB" },
  { kITypeMask | (0x3ff << 16), (22 << kOpcodeShift) | (5  << 21) | (5  << 16), "bgezc", "TB" },
  { kITypeMask | (0x3ff << 16), (22 << kOpcodeShift) | (6  << 21) | (6  << 16), "bgezc", "TB" },
  { kITypeMask | (0x3ff << 16), (22 << kOpcodeShift) | (7  << 21) | (7  << 16), "bgezc", "TB" },
  { kITypeMask | (0x3ff << 16), (22 << kOpcodeShift) | (8  << 21) | (8  << 16), "bgezc", "TB" },
  { kITypeMask | (0x3ff << 16), (22 << kOpcodeShift) | (9  << 21) | (9  << 16), "bgezc", "TB" },
  { kITypeMask | (0x3ff << 16), (22 << kOpcodeShift) | (10 << 21) | (10 << 16), "bgezc", "TB" },
  { kITypeMask | (0x3ff << 16), (22 << kOpcodeShift) | (11 << 21) | (11 << 16), "bgezc", "TB" },
  { kITypeMask | (0x3ff << 16), (22 << kOpcodeShift) | (12 << 21) | (12 << 16), "bgezc", "TB" },
  { kITypeMask | (0x3ff << 16), (22 << kOpcodeShift) | (13 << 21) | (13 << 16), "bgezc", "TB" },
  { kITypeMask | (0x3ff << 16), (22 << kOpcodeShift) | (14 << 21) | (14 << 16), "bgezc", "TB" },
  { kITypeMask | (0x3ff << 16), (22 << kOpcodeShift) | (15 << 21) | (15 << 16), "bgezc", "TB" },
  { kITypeMask | (0x3ff << 16), (22 << kOpcodeShift) | (16 << 21) | (16 << 16), "bgezc", "TB" },
  { kITypeMask | (0x3ff << 16), (22 << kOpcodeShift) | (17 << 21) | (17 << 16), "bgezc", "TB" },
  { kITypeMask | (0x3ff << 16), (22 << kOpcodeShift) | (18 << 21) | (18 << 16), "bgezc", "TB" },
  { kITypeMask | (0x3ff << 16), (22 << kOpcodeShift) | (19 << 21) | (19 << 16), "bgezc", "TB" },
  { kITypeMask | (0x3ff << 16), (22 << kOpcodeShift) | (20 << 21) | (20 << 16), "bgezc", "TB" },
  { kITypeMask | (0x3ff << 16), (22 << kOpcodeShift) | (21 << 21) | (21 << 16), "bgezc", "TB" },
  { kITypeMask | (0x3ff << 16), (22 << kOpcodeShift) | (22 << 21) | (22 << 16), "bgezc", "TB" },
  { kITypeMask | (0x3ff << 16), (22 << kOpcodeShift) | (23 << 21) | (23 << 16), "bgezc", "TB" },
  { kITypeMask | (0x3ff << 16), (22 << kOpcodeShift) | (24 << 21) | (24 << 16), "bgezc", "TB" },
  { kITypeMask | (0x3ff << 16), (22 << kOpcodeShift) | (25 << 21) | (25 << 16), "bgezc", "TB" },
  { kITypeMask | (0x3ff << 16), (22 << kOpcodeShift) | (26 << 21) | (26 << 16), "bgezc", "TB" },
  { kITypeMask | (0x3ff << 16), (22 << kOpcodeShift) | (27 << 21) | (27 << 16), "bgezc", "TB" },
  { kITypeMask | (0x3ff << 16), (22 << kOpcodeShift) | (28 << 21) | (28 << 16), "bgezc", "TB" },
  { kITypeMask | (0x3ff << 16), (22 << kOpcodeShift) | (29 << 21) | (29 << 16), "bgezc", "TB" },
  { kITypeMask | (0x3ff << 16), (22 << kOpcodeShift) | (30 << 21) | (30 << 16), "bgezc", "TB" },
  { kITypeMask | (0x3ff << 16), (22 << kOpcodeShift) | (31 << 21) | (31 << 16), "bgezc", "TB" },

  { kITypeMask, 22 << kOpcodeShift, "bgec", "STB" },

  { kITypeMask | (0x1f << 21), 23 << kOpcodeShift, "bgtzc", "TB" },

  // TODO: de-dup
  { kITypeMask | (0x3ff << 16), (23 << kOpcodeShift) | (1  << 21) | (1  << 16), "bltzc", "TB" },
  { kITypeMask | (0x3ff << 16), (23 << kOpcodeShift) | (2  << 21) | (2  << 16), "bltzc", "TB" },
  { kITypeMask | (0x3ff << 16), (23 << kOpcodeShift) | (3  << 21) | (3  << 16), "bltzc", "TB" },
  { kITypeMask | (0x3ff << 16), (23 << kOpcodeShift) | (4  << 21) | (4  << 16), "bltzc", "TB" },
  { kITypeMask | (0x3ff << 16), (23 << kOpcodeShift) | (5  << 21) | (5  << 16), "bltzc", "TB" },
  { kITypeMask | (0x3ff << 16), (23 << kOpcodeShift) | (6  << 21) | (6  << 16), "bltzc", "TB" },
  { kITypeMask | (0x3ff << 16), (23 << kOpcodeShift) | (7  << 21) | (7  << 16), "bltzc", "TB" },
  { kITypeMask | (0x3ff << 16), (23 << kOpcodeShift) | (8  << 21) | (8  << 16), "bltzc", "TB" },
  { kITypeMask | (0x3ff << 16), (23 << kOpcodeShift) | (9  << 21) | (9  << 16), "bltzc", "TB" },
  { kITypeMask | (0x3ff << 16), (23 << kOpcodeShift) | (10 << 21) | (10 << 16), "bltzc", "TB" },
  { kITypeMask | (0x3ff << 16), (23 << kOpcodeShift) | (11 << 21) | (11 << 16), "bltzc", "TB" },
  { kITypeMask | (0x3ff << 16), (23 << kOpcodeShift) | (12 << 21) | (12 << 16), "bltzc", "TB" },
  { kITypeMask | (0x3ff << 16), (23 << kOpcodeShift) | (13 << 21) | (13 << 16), "bltzc", "TB" },
  { kITypeMask | (0x3ff << 16), (23 << kOpcodeShift) | (14 << 21) | (14 << 16), "bltzc", "TB" },
  { kITypeMask | (0x3ff << 16), (23 << kOpcodeShift) | (15 << 21) | (15 << 16), "bltzc", "TB" },
  { kITypeMask | (0x3ff << 16), (23 << kOpcodeShift) | (16 << 21) | (16 << 16), "bltzc", "TB" },
  { kITypeMask | (0x3ff << 16), (23 << kOpcodeShift) | (17 << 21) | (17 << 16), "bltzc", "TB" },
  { kITypeMask | (0x3ff << 16), (23 << kOpcodeShift) | (18 << 21) | (18 << 16), "bltzc", "TB" },
  { kITypeMask | (0x3ff << 16), (23 << kOpcodeShift) | (19 << 21) | (19 << 16), "bltzc", "TB" },
  { kITypeMask | (0x3ff << 16), (23 << kOpcodeShift) | (20 << 21) | (20 << 16), "bltzc", "TB" },
  { kITypeMask | (0x3ff << 16), (23 << kOpcodeShift) | (21 << 21) | (21 << 16), "bltzc", "TB" },
  { kITypeMask | (0x3ff << 16), (23 << kOpcodeShift) | (22 << 21) | (22 << 16), "bltzc", "TB" },
  { kITypeMask | (0x3ff << 16), (23 << kOpcodeShift) | (23 << 21) | (23 << 16), "bltzc", "TB" },
  { kITypeMask | (0x3ff << 16), (23 << kOpcodeShift) | (24 << 21) | (24 << 16), "bltzc", "TB" },
  { kITypeMask | (0x3ff << 16), (23 << kOpcodeShift) | (25 << 21) | (25 << 16), "bltzc", "TB" },
  { kITypeMask | (0x3ff << 16), (23 << kOpcodeShift) | (26 << 21) | (26 << 16), "bltzc", "TB" },
  { kITypeMask | (0x3ff << 16), (23 << kOpcodeShift) | (27 << 21) | (27 << 16), "bltzc", "TB" },
  { kITypeMask | (0x3ff << 16), (23 << kOpcodeShift) | (28 << 21) | (28 << 16), "bltzc", "TB" },
  { kITypeMask | (0x3ff << 16), (23 << kOpcodeShift) | (29 << 21) | (29 << 16), "bltzc", "TB" },
  { kITypeMask | (0x3ff << 16), (23 << kOpcodeShift) | (30 << 21) | (30 << 16), "bltzc", "TB" },
  { kITypeMask | (0x3ff << 16), (23 << kOpcodeShift) | (31 << 21) | (31 << 16), "bltzc", "TB" },

  { kITypeMask, 23 << kOpcodeShift, "bltc", "STB" },

  { kITypeMask, 24 << kOpcodeShift, "bnec", "STB" },

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
  { kITypeMask | (0x1f << 21), 54u << kOpcodeShift, "jic", "Ti" },
  { kITypeMask | (1 << 21), (54u << kOpcodeShift) | (1 << 21), "beqzc", "Sb" },  // TODO: de-dup?
  { kITypeMask | (1 << 22), (54u << kOpcodeShift) | (1 << 22), "beqzc", "Sb" },
  { kITypeMask | (1 << 23), (54u << kOpcodeShift) | (1 << 23), "beqzc", "Sb" },
  { kITypeMask | (1 << 24), (54u << kOpcodeShift) | (1 << 24), "beqzc", "Sb" },
  { kITypeMask | (1 << 25), (54u << kOpcodeShift) | (1 << 25), "beqzc", "Sb" },
  { kITypeMask, 55u << kOpcodeShift, "ld", "TO", },
  { kITypeMask, 57u << kOpcodeShift, "swc1", "tO", },
  { kITypeMask | (0x1f << 16), (59u << kOpcodeShift) | (30 << 16), "auipc", "Si" },
  { kITypeMask, 61u << kOpcodeShift, "sdc1", "tO", },
  { kITypeMask | (0x1f << 21), 62u << kOpcodeShift, "jialc", "Ti" },
  { kITypeMask | (1 << 21), (62u << kOpcodeShift) | (1 << 21), "bnezc", "Sb" },  // TODO: de-dup?
  { kITypeMask | (1 << 22), (62u << kOpcodeShift) | (1 << 22), "bnezc", "Sb" },
  { kITypeMask | (1 << 23), (62u << kOpcodeShift) | (1 << 23), "bnezc", "Sb" },
  { kITypeMask | (1 << 24), (62u << kOpcodeShift) | (1 << 24), "bnezc", "Sb" },
  { kITypeMask | (1 << 25), (62u << kOpcodeShift) | (1 << 25), "bnezc", "Sb" },
  { kITypeMask, 63u << kOpcodeShift, "sd", "TO", },

  // Floating point.
  { kFpMask | (0x1f << 21), kCop1 | (0x00 << 21), "mfc1", "Td" },
  { kFpMask | (0x1f << 21), kCop1 | (0x01 << 21), "dmfc1", "Td" },
  { kFpMask | (0x1f << 21), kCop1 | (0x03 << 21), "mfhc1", "Td" },
  { kFpMask | (0x1f << 21), kCop1 | (0x04 << 21), "mtc1", "Td" },
  { kFpMask | (0x1f << 21), kCop1 | (0x05 << 21), "dmtc1", "Td" },
  { kFpMask | (0x1f << 21), kCop1 | (0x07 << 21), "mthc1", "Td" },
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
  // We only support little-endian MIPS.
  return ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24);
}

size_t DisassemblerMips::Dump(std::ostream& os, const uint8_t* instr_ptr) {
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

  for (size_t i = 0; i < arraysize(gMipsInstructions); ++i) {
    if (gMipsInstructions[i].Matches(instruction)) {
      opcode = gMipsInstructions[i].name;
      for (const char* args_fmt = gMipsInstructions[i].args_fmt; *args_fmt; ++args_fmt) {
        switch (*args_fmt) {
          case 'A':  // sa (shift amount or [d]ext position).
            args << sa;
            break;
          case 'B':  // Branch offset.
            {
              int32_t offset = static_cast<int16_t>(instruction & 0xffff);
              offset <<= 2;
              offset += 4;  // Delay slot.
              args << FormatInstructionPointer(instr_ptr + offset)
                   << StringPrintf("  ; %+d", offset);
            }
            break;
          case 'b':  // 21-bit branch offset.
            {
              int32_t offset = (instruction & 0x1fffff) - ((instruction & 0x100000) << 1);
              offset <<= 2;
              offset += 4;  // Delay slot.
              args << FormatInstructionPointer(instr_ptr + offset)
                   << StringPrintf("  ; %+d", offset);
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
              target |= (reinterpret_cast<uintptr_t>(instr_ptr + 4) & 0xf0000000);
              args << reinterpret_cast<void*>(target);
            }
            break;
          case 'O':  // +x(rs)
            {
              int32_t offset = static_cast<int16_t>(instruction & 0xffff);
              args << StringPrintf("%+d(r%d)", offset, rs);
              if (rs == 17) {
                args << "  ; ";
                if (is64bit_) {
                  Thread::DumpThreadOffset<8>(args, offset);
                } else {
                  Thread::DumpThreadOffset<4>(args, offset);
                }
              }
            }
            break;
          case 'S': args << 'r' << rs; break;
          case 's': args << 'f' << rs; break;
          case 'T': args << 'r' << rt; break;
          case 't': args << 'f' << rt; break;
          case 'Z': args << rd; break;   // sz ([d]ext size).
        }
        if (*(args_fmt + 1)) {
          args << ", ";
        }
      }
      break;
    }
  }

  // Special cases for sequences of:
  //   pc-relative +/- 2GB branch:
  //     auipc  reg, imm
  //     jic    reg, imm
  //   pc-relative +/- 2GB branch and link:
  //     auipc  reg, imm
  //     daddiu reg, reg, imm
  //     jialc  reg, 0
  if (((op == 0x36 && rs == 0 && rt != 0) ||  // jic
       (op == 0x19 && rs == rt && rt != 0)) &&  // daddiu
      last_ptr_ && (intptr_t)instr_ptr - (intptr_t)last_ptr_ == 4 &&
      (last_instr_ & 0xFC1F0000) == 0xEC1E0000 &&  // auipc
      ((last_instr_ >> 21) & 0x1F) == rt) {
    uint32_t offset = (last_instr_ << 16) | (instruction & 0xFFFF);
    offset -= (offset & 0x8000) << 1;
    offset -= 4;
    if (op == 0x36) {
      args << "  ; b ";
    } else {
      args << "  ; move r" << rt << ", ";
    }
    args << FormatInstructionPointer(instr_ptr + (int32_t)offset);
    args << StringPrintf("  ; %+d", (int32_t)offset);
  }

  os << FormatInstructionPointer(instr_ptr)
     << StringPrintf(": %08x\t%-7s ", instruction, opcode.c_str())
     << args.str() << '\n';
  last_ptr_ = instr_ptr;
  last_instr_ = instruction;
  return 4;
}

void DisassemblerMips::Dump(std::ostream& os, const uint8_t* begin, const uint8_t* end) {
  for (const uint8_t* cur = begin; cur < end; cur += 4) {
    Dump(os, cur);
  }
}

}  // namespace mips
}  // namespace art
