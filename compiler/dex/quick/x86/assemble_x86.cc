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

#include "codegen_x86.h"
#include "dex/quick/mir_to_lir-inl.h"
#include "x86_lir.h"

namespace art {

#define MAX_ASSEMBLER_RETRIES 50

const X86EncodingMap X86Mir2Lir::EncodingMap[kX86Last] = {
  { kX8632BitData, kData,    IS_UNARY_OP,            { 0, 0, 0x00, 0, 0, 0, 0, 4 }, "data",  "0x!0d" },
  { kX86Bkpt,      kNullary, NO_OPERAND | IS_BRANCH, { 0, 0, 0xCC, 0, 0, 0, 0, 0 }, "int 3", "" },
  { kX86Nop,       kNop,     NO_OPERAND,             { 0, 0, 0x90, 0, 0, 0, 0, 0 }, "nop",   "" },

#define ENCODING_MAP(opname, mem_use, reg_def, uses_ccodes, \
                     rm8_r8, rm32_r32, \
                     r8_rm8, r32_rm32, \
                     ax8_i8, ax32_i32, \
                     rm8_i8, rm8_i8_modrm, \
                     rm32_i32, rm32_i32_modrm, \
                     rm32_i8, rm32_i8_modrm) \
{ kX86 ## opname ## 8MR, kMemReg,    mem_use | IS_TERTIARY_OP |           REG_USE02  | SETS_CCODES | uses_ccodes, { 0,             0, rm8_r8, 0, 0, 0,            0,      0 }, #opname "8MR", "[!0r+!1d],!2r" }, \
{ kX86 ## opname ## 8AR, kArrayReg,  mem_use | IS_QUIN_OP     |           REG_USE014 | SETS_CCODES | uses_ccodes, { 0,             0, rm8_r8, 0, 0, 0,            0,      0 }, #opname "8AR", "[!0r+!1r<<!2d+!3d],!4r" }, \
{ kX86 ## opname ## 8TR, kThreadReg, mem_use | IS_BINARY_OP   |           REG_USE1   | SETS_CCODES | uses_ccodes, { THREAD_PREFIX, 0, rm8_r8, 0, 0, 0,            0,      0 }, #opname "8TR", "fs:[!0d],!1r" }, \
{ kX86 ## opname ## 8RR, kRegReg,              IS_BINARY_OP   | reg_def | REG_USE01  | SETS_CCODES | uses_ccodes, { 0,             0, r8_rm8, 0, 0, 0,            0,      0 }, #opname "8RR", "!0r,!1r" }, \
{ kX86 ## opname ## 8RM, kRegMem,    IS_LOAD | IS_TERTIARY_OP | reg_def | REG_USE01  | SETS_CCODES | uses_ccodes, { 0,             0, r8_rm8, 0, 0, 0,            0,      0 }, #opname "8RM", "!0r,[!1r+!2d]" }, \
{ kX86 ## opname ## 8RA, kRegArray,  IS_LOAD | IS_QUIN_OP     | reg_def | REG_USE012 | SETS_CCODES | uses_ccodes, { 0,             0, r8_rm8, 0, 0, 0,            0,      0 }, #opname "8RA", "!0r,[!1r+!2r<<!3d+!4d]" }, \
{ kX86 ## opname ## 8RT, kRegThread, IS_LOAD | IS_BINARY_OP   | reg_def | REG_USE0   | SETS_CCODES | uses_ccodes, { THREAD_PREFIX, 0, r8_rm8, 0, 0, 0,            0,      0 }, #opname "8RT", "!0r,fs:[!1d]" }, \
{ kX86 ## opname ## 8RI, kRegImm,              IS_BINARY_OP   | reg_def | REG_USE0   | SETS_CCODES | uses_ccodes, { 0,             0, rm8_i8, 0, 0, rm8_i8_modrm, ax8_i8, 1 }, #opname "8RI", "!0r,!1d" }, \
{ kX86 ## opname ## 8MI, kMemImm,    mem_use | IS_TERTIARY_OP |           REG_USE0   | SETS_CCODES | uses_ccodes, { 0,             0, rm8_i8, 0, 0, rm8_i8_modrm, 0,      1 }, #opname "8MI", "[!0r+!1d],!2d" }, \
{ kX86 ## opname ## 8AI, kArrayImm,  mem_use | IS_QUIN_OP     |           REG_USE01  | SETS_CCODES | uses_ccodes, { 0,             0, rm8_i8, 0, 0, rm8_i8_modrm, 0,      1 }, #opname "8AI", "[!0r+!1r<<!2d+!3d],!4d" }, \
{ kX86 ## opname ## 8TI, kThreadImm, mem_use | IS_BINARY_OP   |                        SETS_CCODES | uses_ccodes, { THREAD_PREFIX, 0, rm8_i8, 0, 0, rm8_i8_modrm, 0,      1 }, #opname "8TI", "fs:[!0d],!1d" }, \
  \
{ kX86 ## opname ## 16MR,  kMemReg,    mem_use | IS_TERTIARY_OP |           REG_USE02  | SETS_CCODES | uses_ccodes, { 0x66,          0,    rm32_r32, 0, 0, 0,              0,        0 }, #opname "16MR", "[!0r+!1d],!2r" }, \
{ kX86 ## opname ## 16AR,  kArrayReg,  mem_use | IS_QUIN_OP     |           REG_USE014 | SETS_CCODES | uses_ccodes, { 0x66,          0,    rm32_r32, 0, 0, 0,              0,        0 }, #opname "16AR", "[!0r+!1r<<!2d+!3d],!4r" }, \
{ kX86 ## opname ## 16TR,  kThreadReg, mem_use | IS_BINARY_OP   |           REG_USE1   | SETS_CCODES | uses_ccodes, { THREAD_PREFIX, 0x66, rm32_r32, 0, 0, 0,              0,        0 }, #opname "16TR", "fs:[!0d],!1r" }, \
{ kX86 ## opname ## 16RR,  kRegReg,              IS_BINARY_OP   | reg_def | REG_USE01  | SETS_CCODES | uses_ccodes, { 0x66,          0,    r32_rm32, 0, 0, 0,              0,        0 }, #opname "16RR", "!0r,!1r" }, \
{ kX86 ## opname ## 16RM,  kRegMem,    IS_LOAD | IS_TERTIARY_OP | reg_def | REG_USE01  | SETS_CCODES | uses_ccodes, { 0x66,          0,    r32_rm32, 0, 0, 0,              0,        0 }, #opname "16RM", "!0r,[!1r+!2d]" }, \
{ kX86 ## opname ## 16RA,  kRegArray,  IS_LOAD | IS_QUIN_OP     | reg_def | REG_USE012 | SETS_CCODES | uses_ccodes, { 0x66,          0,    r32_rm32, 0, 0, 0,              0,        0 }, #opname "16RA", "!0r,[!1r+!2r<<!3d+!4d]" }, \
{ kX86 ## opname ## 16RT,  kRegThread, IS_LOAD | IS_BINARY_OP   | reg_def | REG_USE0   | SETS_CCODES | uses_ccodes, { THREAD_PREFIX, 0x66, r32_rm32, 0, 0, 0,              0,        0 }, #opname "16RT", "!0r,fs:[!1d]" }, \
{ kX86 ## opname ## 16RI,  kRegImm,              IS_BINARY_OP   | reg_def | REG_USE0   | SETS_CCODES | uses_ccodes, { 0x66,          0,    rm32_i32, 0, 0, rm32_i32_modrm, ax32_i32, 2 }, #opname "16RI", "!0r,!1d" }, \
{ kX86 ## opname ## 16MI,  kMemImm,    mem_use | IS_TERTIARY_OP |           REG_USE0   | SETS_CCODES | uses_ccodes, { 0x66,          0,    rm32_i32, 0, 0, rm32_i32_modrm, 0,        2 }, #opname "16MI", "[!0r+!1d],!2d" }, \
{ kX86 ## opname ## 16AI,  kArrayImm,  mem_use | IS_QUIN_OP     |           REG_USE01  | SETS_CCODES | uses_ccodes, { 0x66,          0,    rm32_i32, 0, 0, rm32_i32_modrm, 0,        2 }, #opname "16AI", "[!0r+!1r<<!2d+!3d],!4d" }, \
{ kX86 ## opname ## 16TI,  kThreadImm, mem_use | IS_BINARY_OP   |                        SETS_CCODES | uses_ccodes, { THREAD_PREFIX, 0x66, rm32_i32, 0, 0, rm32_i32_modrm, 0,        2 }, #opname "16TI", "fs:[!0d],!1d" }, \
{ kX86 ## opname ## 16RI8, kRegImm,              IS_BINARY_OP   | reg_def | REG_USE0   | SETS_CCODES | uses_ccodes, { 0x66,          0,    rm32_i8,  0, 0, rm32_i8_modrm,  0,        1 }, #opname "16RI8", "!0r,!1d" }, \
{ kX86 ## opname ## 16MI8, kMemImm,    mem_use | IS_TERTIARY_OP |           REG_USE0   | SETS_CCODES | uses_ccodes, { 0x66,          0,    rm32_i8,  0, 0, rm32_i8_modrm,  0,        1 }, #opname "16MI8", "[!0r+!1d],!2d" }, \
{ kX86 ## opname ## 16AI8, kArrayImm,  mem_use | IS_QUIN_OP     |           REG_USE01  | SETS_CCODES | uses_ccodes, { 0x66,          0,    rm32_i8,  0, 0, rm32_i8_modrm,  0,        1 }, #opname "16AI8", "[!0r+!1r<<!2d+!3d],!4d" }, \
{ kX86 ## opname ## 16TI8, kThreadImm, mem_use | IS_BINARY_OP   |                        SETS_CCODES | uses_ccodes, { THREAD_PREFIX, 0x66, rm32_i8,  0, 0, rm32_i8_modrm,  0,        1 }, #opname "16TI8", "fs:[!0d],!1d" }, \
  \
{ kX86 ## opname ## 32MR,  kMemReg,    mem_use | IS_TERTIARY_OP |           REG_USE02  | SETS_CCODES | uses_ccodes, { 0,             0, rm32_r32, 0, 0, 0,              0,        0 }, #opname "32MR", "[!0r+!1d],!2r" }, \
{ kX86 ## opname ## 32AR,  kArrayReg,  mem_use | IS_QUIN_OP     |           REG_USE014 | SETS_CCODES | uses_ccodes, { 0,             0, rm32_r32, 0, 0, 0,              0,        0 }, #opname "32AR", "[!0r+!1r<<!2d+!3d],!4r" }, \
{ kX86 ## opname ## 32TR,  kThreadReg, mem_use | IS_BINARY_OP   |           REG_USE1   | SETS_CCODES | uses_ccodes, { THREAD_PREFIX, 0, rm32_r32, 0, 0, 0,              0,        0 }, #opname "32TR", "fs:[!0d],!1r" }, \
{ kX86 ## opname ## 32RR,  kRegReg,              IS_BINARY_OP   | reg_def | REG_USE01  | SETS_CCODES | uses_ccodes, { 0,             0, r32_rm32, 0, 0, 0,              0,        0 }, #opname "32RR", "!0r,!1r" }, \
{ kX86 ## opname ## 32RM,  kRegMem,    IS_LOAD | IS_TERTIARY_OP | reg_def | REG_USE01  | SETS_CCODES | uses_ccodes, { 0,             0, r32_rm32, 0, 0, 0,              0,        0 }, #opname "32RM", "!0r,[!1r+!2d]" }, \
{ kX86 ## opname ## 32RA,  kRegArray,  IS_LOAD | IS_QUIN_OP     | reg_def | REG_USE012 | SETS_CCODES | uses_ccodes, { 0,             0, r32_rm32, 0, 0, 0,              0,        0 }, #opname "32RA", "!0r,[!1r+!2r<<!3d+!4d]" }, \
{ kX86 ## opname ## 32RT,  kRegThread, IS_LOAD | IS_BINARY_OP   | reg_def | REG_USE0   | SETS_CCODES | uses_ccodes, { THREAD_PREFIX, 0, r32_rm32, 0, 0, 0,              0,        0 }, #opname "32RT", "!0r,fs:[!1d]" }, \
{ kX86 ## opname ## 32RI,  kRegImm,              IS_BINARY_OP   | reg_def | REG_USE0   | SETS_CCODES | uses_ccodes, { 0,             0, rm32_i32, 0, 0, rm32_i32_modrm, ax32_i32, 4 }, #opname "32RI", "!0r,!1d" }, \
{ kX86 ## opname ## 32MI,  kMemImm,    mem_use | IS_TERTIARY_OP |           REG_USE0   | SETS_CCODES | uses_ccodes, { 0,             0, rm32_i32, 0, 0, rm32_i32_modrm, 0,        4 }, #opname "32MI", "[!0r+!1d],!2d" }, \
{ kX86 ## opname ## 32AI,  kArrayImm,  mem_use | IS_QUIN_OP     |           REG_USE01  | SETS_CCODES | uses_ccodes, { 0,             0, rm32_i32, 0, 0, rm32_i32_modrm, 0,        4 }, #opname "32AI", "[!0r+!1r<<!2d+!3d],!4d" }, \
{ kX86 ## opname ## 32TI,  kThreadImm, mem_use | IS_BINARY_OP   |                        SETS_CCODES | uses_ccodes, { THREAD_PREFIX, 0, rm32_i32, 0, 0, rm32_i32_modrm, 0,        4 }, #opname "32TI", "fs:[!0d],!1d" }, \
{ kX86 ## opname ## 32RI8, kRegImm,              IS_BINARY_OP   | reg_def | REG_USE0   | SETS_CCODES | uses_ccodes, { 0,             0, rm32_i8,  0, 0, rm32_i8_modrm,  0,        1 }, #opname "32RI8", "!0r,!1d" }, \
{ kX86 ## opname ## 32MI8, kMemImm,    mem_use | IS_TERTIARY_OP |           REG_USE0   | SETS_CCODES | uses_ccodes, { 0,             0, rm32_i8,  0, 0, rm32_i8_modrm,  0,        1 }, #opname "32MI8", "[!0r+!1d],!2d" }, \
{ kX86 ## opname ## 32AI8, kArrayImm,  mem_use | IS_QUIN_OP     |           REG_USE01  | SETS_CCODES | uses_ccodes, { 0,             0, rm32_i8,  0, 0, rm32_i8_modrm,  0,        1 }, #opname "32AI8", "[!0r+!1r<<!2d+!3d],!4d" }, \
{ kX86 ## opname ## 32TI8, kThreadImm, mem_use | IS_BINARY_OP   |                        SETS_CCODES | uses_ccodes, { THREAD_PREFIX, 0, rm32_i8,  0, 0, rm32_i8_modrm,  0,        1 }, #opname "32TI8", "fs:[!0d],!1d" }

ENCODING_MAP(Add, IS_LOAD | IS_STORE, REG_DEF0, 0,
  0x00 /* RegMem8/Reg8 */,     0x01 /* RegMem32/Reg32 */,
  0x02 /* Reg8/RegMem8 */,     0x03 /* Reg32/RegMem32 */,
  0x04 /* Rax8/imm8 opcode */, 0x05 /* Rax32/imm32 */,
  0x80, 0x0 /* RegMem8/imm8 */,
  0x81, 0x0 /* RegMem32/imm32 */, 0x83, 0x0 /* RegMem32/imm8 */),
ENCODING_MAP(Or, IS_LOAD | IS_STORE, REG_DEF0, 0,
  0x08 /* RegMem8/Reg8 */,     0x09 /* RegMem32/Reg32 */,
  0x0A /* Reg8/RegMem8 */,     0x0B /* Reg32/RegMem32 */,
  0x0C /* Rax8/imm8 opcode */, 0x0D /* Rax32/imm32 */,
  0x80, 0x1 /* RegMem8/imm8 */,
  0x81, 0x1 /* RegMem32/imm32 */, 0x83, 0x1 /* RegMem32/imm8 */),
ENCODING_MAP(Adc, IS_LOAD | IS_STORE, REG_DEF0, USES_CCODES,
  0x10 /* RegMem8/Reg8 */,     0x11 /* RegMem32/Reg32 */,
  0x12 /* Reg8/RegMem8 */,     0x13 /* Reg32/RegMem32 */,
  0x14 /* Rax8/imm8 opcode */, 0x15 /* Rax32/imm32 */,
  0x80, 0x2 /* RegMem8/imm8 */,
  0x81, 0x2 /* RegMem32/imm32 */, 0x83, 0x2 /* RegMem32/imm8 */),
ENCODING_MAP(Sbb, IS_LOAD | IS_STORE, REG_DEF0, USES_CCODES,
  0x18 /* RegMem8/Reg8 */,     0x19 /* RegMem32/Reg32 */,
  0x1A /* Reg8/RegMem8 */,     0x1B /* Reg32/RegMem32 */,
  0x1C /* Rax8/imm8 opcode */, 0x1D /* Rax32/imm32 */,
  0x80, 0x3 /* RegMem8/imm8 */,
  0x81, 0x3 /* RegMem32/imm32 */, 0x83, 0x3 /* RegMem32/imm8 */),
ENCODING_MAP(And, IS_LOAD | IS_STORE, REG_DEF0, 0,
  0x20 /* RegMem8/Reg8 */,     0x21 /* RegMem32/Reg32 */,
  0x22 /* Reg8/RegMem8 */,     0x23 /* Reg32/RegMem32 */,
  0x24 /* Rax8/imm8 opcode */, 0x25 /* Rax32/imm32 */,
  0x80, 0x4 /* RegMem8/imm8 */,
  0x81, 0x4 /* RegMem32/imm32 */, 0x83, 0x4 /* RegMem32/imm8 */),
ENCODING_MAP(Sub, IS_LOAD | IS_STORE, REG_DEF0, 0,
  0x28 /* RegMem8/Reg8 */,     0x29 /* RegMem32/Reg32 */,
  0x2A /* Reg8/RegMem8 */,     0x2B /* Reg32/RegMem32 */,
  0x2C /* Rax8/imm8 opcode */, 0x2D /* Rax32/imm32 */,
  0x80, 0x5 /* RegMem8/imm8 */,
  0x81, 0x5 /* RegMem32/imm32 */, 0x83, 0x5 /* RegMem32/imm8 */),
ENCODING_MAP(Xor, IS_LOAD | IS_STORE, REG_DEF0, 0,
  0x30 /* RegMem8/Reg8 */,     0x31 /* RegMem32/Reg32 */,
  0x32 /* Reg8/RegMem8 */,     0x33 /* Reg32/RegMem32 */,
  0x34 /* Rax8/imm8 opcode */, 0x35 /* Rax32/imm32 */,
  0x80, 0x6 /* RegMem8/imm8 */,
  0x81, 0x6 /* RegMem32/imm32 */, 0x83, 0x6 /* RegMem32/imm8 */),
ENCODING_MAP(Cmp, IS_LOAD, 0, 0,
  0x38 /* RegMem8/Reg8 */,     0x39 /* RegMem32/Reg32 */,
  0x3A /* Reg8/RegMem8 */,     0x3B /* Reg32/RegMem32 */,
  0x3C /* Rax8/imm8 opcode */, 0x3D /* Rax32/imm32 */,
  0x80, 0x7 /* RegMem8/imm8 */,
  0x81, 0x7 /* RegMem32/imm32 */, 0x83, 0x7 /* RegMem32/imm8 */),
#undef ENCODING_MAP

  { kX86Imul16RRI,   kRegRegImm,             IS_TERTIARY_OP | REG_DEF0_USE1  | SETS_CCODES, { 0x66, 0, 0x69, 0, 0, 0, 0, 2 }, "Imul16RRI", "!0r,!1r,!2d" },
  { kX86Imul16RMI,   kRegMemImm,   IS_LOAD | IS_QUAD_OP     | REG_DEF0_USE1  | SETS_CCODES, { 0x66, 0, 0x69, 0, 0, 0, 0, 2 }, "Imul16RMI", "!0r,[!1r+!2d],!3d" },
  { kX86Imul16RAI,   kRegArrayImm, IS_LOAD | IS_SEXTUPLE_OP | REG_DEF0_USE12 | SETS_CCODES, { 0x66, 0, 0x69, 0, 0, 0, 0, 2 }, "Imul16RAI", "!0r,[!1r+!2r<<!3d+!4d],!5d" },

  { kX86Imul32RRI,   kRegRegImm,             IS_TERTIARY_OP | REG_DEF0_USE1  | SETS_CCODES, { 0, 0, 0x69, 0, 0, 0, 0, 4 }, "Imul32RRI", "!0r,!1r,!2d" },
  { kX86Imul32RMI,   kRegMemImm,   IS_LOAD | IS_QUAD_OP     | REG_DEF0_USE1  | SETS_CCODES, { 0, 0, 0x69, 0, 0, 0, 0, 4 }, "Imul32RMI", "!0r,[!1r+!2d],!3d" },
  { kX86Imul32RAI,   kRegArrayImm, IS_LOAD | IS_SEXTUPLE_OP | REG_DEF0_USE12 | SETS_CCODES, { 0, 0, 0x69, 0, 0, 0, 0, 4 }, "Imul32RAI", "!0r,[!1r+!2r<<!3d+!4d],!5d" },
  { kX86Imul32RRI8,  kRegRegImm,             IS_TERTIARY_OP | REG_DEF0_USE1  | SETS_CCODES, { 0, 0, 0x6B, 0, 0, 0, 0, 1 }, "Imul32RRI8", "!0r,!1r,!2d" },
  { kX86Imul32RMI8,  kRegMemImm,   IS_LOAD | IS_QUAD_OP     | REG_DEF0_USE1  | SETS_CCODES, { 0, 0, 0x6B, 0, 0, 0, 0, 1 }, "Imul32RMI8", "!0r,[!1r+!2d],!3d" },
  { kX86Imul32RAI8,  kRegArrayImm, IS_LOAD | IS_SEXTUPLE_OP | REG_DEF0_USE12 | SETS_CCODES, { 0, 0, 0x6B, 0, 0, 0, 0, 1 }, "Imul32RAI8", "!0r,[!1r+!2r<<!3d+!4d],!5d" },

  { kX86Mov8MR, kMemReg,    IS_STORE | IS_TERTIARY_OP | REG_USE02,      { 0,             0, 0x88, 0, 0, 0, 0, 0 }, "Mov8MR", "[!0r+!1d],!2r" },
  { kX86Mov8AR, kArrayReg,  IS_STORE | IS_QUIN_OP     | REG_USE014,     { 0,             0, 0x88, 0, 0, 0, 0, 0 }, "Mov8AR", "[!0r+!1r<<!2d+!3d],!4r" },
  { kX86Mov8TR, kThreadReg, IS_STORE | IS_BINARY_OP   | REG_USE1,       { THREAD_PREFIX, 0, 0x88, 0, 0, 0, 0, 0 }, "Mov8TR", "fs:[!0d],!1r" },
  { kX86Mov8RR, kRegReg,               IS_BINARY_OP   | REG_DEF0_USE1,  { 0,             0, 0x8A, 0, 0, 0, 0, 0 }, "Mov8RR", "!0r,!1r" },
  { kX86Mov8RM, kRegMem,    IS_LOAD  | IS_TERTIARY_OP | REG_DEF0_USE1,  { 0,             0, 0x8A, 0, 0, 0, 0, 0 }, "Mov8RM", "!0r,[!1r+!2d]" },
  { kX86Mov8RA, kRegArray,  IS_LOAD  | IS_QUIN_OP     | REG_DEF0_USE12, { 0,             0, 0x8A, 0, 0, 0, 0, 0 }, "Mov8RA", "!0r,[!1r+!2r<<!3d+!4d]" },
  { kX86Mov8RT, kRegThread, IS_LOAD  | IS_BINARY_OP   | REG_DEF0,       { THREAD_PREFIX, 0, 0x8A, 0, 0, 0, 0, 0 }, "Mov8RT", "!0r,fs:[!1d]" },
  { kX86Mov8RI, kMovRegImm,            IS_BINARY_OP   | REG_DEF0,       { 0,             0, 0xB0, 0, 0, 0, 0, 1 }, "Mov8RI", "!0r,!1d" },
  { kX86Mov8MI, kMemImm,    IS_STORE | IS_TERTIARY_OP | REG_USE0,       { 0,             0, 0xC6, 0, 0, 0, 0, 1 }, "Mov8MI", "[!0r+!1d],!2d" },
  { kX86Mov8AI, kArrayImm,  IS_STORE | IS_QUIN_OP     | REG_USE01,      { 0,             0, 0xC6, 0, 0, 0, 0, 1 }, "Mov8AI", "[!0r+!1r<<!2d+!3d],!4d" },
  { kX86Mov8TI, kThreadImm, IS_STORE | IS_BINARY_OP,                    { THREAD_PREFIX, 0, 0xC6, 0, 0, 0, 0, 1 }, "Mov8TI", "fs:[!0d],!1d" },

  { kX86Mov16MR, kMemReg,    IS_STORE | IS_TERTIARY_OP | REG_USE02,      { 0x66,          0,    0x89, 0, 0, 0, 0, 0 }, "Mov16MR", "[!0r+!1d],!2r" },
  { kX86Mov16AR, kArrayReg,  IS_STORE | IS_QUIN_OP     | REG_USE014,     { 0x66,          0,    0x89, 0, 0, 0, 0, 0 }, "Mov16AR", "[!0r+!1r<<!2d+!3d],!4r" },
  { kX86Mov16TR, kThreadReg, IS_STORE | IS_BINARY_OP   | REG_USE1,       { THREAD_PREFIX, 0x66, 0x89, 0, 0, 0, 0, 0 }, "Mov16TR", "fs:[!0d],!1r" },
  { kX86Mov16RR, kRegReg,               IS_BINARY_OP   | REG_DEF0_USE1,  { 0x66,          0,    0x8B, 0, 0, 0, 0, 0 }, "Mov16RR", "!0r,!1r" },
  { kX86Mov16RM, kRegMem,    IS_LOAD  | IS_TERTIARY_OP | REG_DEF0_USE1,  { 0x66,          0,    0x8B, 0, 0, 0, 0, 0 }, "Mov16RM", "!0r,[!1r+!2d]" },
  { kX86Mov16RA, kRegArray,  IS_LOAD  | IS_QUIN_OP     | REG_DEF0_USE12, { 0x66,          0,    0x8B, 0, 0, 0, 0, 0 }, "Mov16RA", "!0r,[!1r+!2r<<!3d+!4d]" },
  { kX86Mov16RT, kRegThread, IS_LOAD  | IS_BINARY_OP   | REG_DEF0,       { THREAD_PREFIX, 0x66, 0x8B, 0, 0, 0, 0, 0 }, "Mov16RT", "!0r,fs:[!1d]" },
  { kX86Mov16RI, kMovRegImm,            IS_BINARY_OP   | REG_DEF0,       { 0x66,          0,    0xB8, 0, 0, 0, 0, 2 }, "Mov16RI", "!0r,!1d" },
  { kX86Mov16MI, kMemImm,    IS_STORE | IS_TERTIARY_OP | REG_USE0,       { 0x66,          0,    0xC7, 0, 0, 0, 0, 2 }, "Mov16MI", "[!0r+!1d],!2d" },
  { kX86Mov16AI, kArrayImm,  IS_STORE | IS_QUIN_OP     | REG_USE01,      { 0x66,          0,    0xC7, 0, 0, 0, 0, 2 }, "Mov16AI", "[!0r+!1r<<!2d+!3d],!4d" },
  { kX86Mov16TI, kThreadImm, IS_STORE | IS_BINARY_OP,                    { THREAD_PREFIX, 0x66, 0xC7, 0, 0, 0, 0, 2 }, "Mov16TI", "fs:[!0d],!1d" },

  { kX86Mov32MR, kMemReg,    IS_STORE | IS_TERTIARY_OP | REG_USE02,      { 0,             0, 0x89, 0, 0, 0, 0, 0 }, "Mov32MR", "[!0r+!1d],!2r" },
  { kX86Mov32AR, kArrayReg,  IS_STORE | IS_QUIN_OP     | REG_USE014,     { 0,             0, 0x89, 0, 0, 0, 0, 0 }, "Mov32AR", "[!0r+!1r<<!2d+!3d],!4r" },
  { kX86Mov32TR, kThreadReg, IS_STORE | IS_BINARY_OP   | REG_USE1,       { THREAD_PREFIX, 0, 0x89, 0, 0, 0, 0, 0 }, "Mov32TR", "fs:[!0d],!1r" },
  { kX86Mov32RR, kRegReg,               IS_BINARY_OP   | REG_DEF0_USE1,  { 0,             0, 0x8B, 0, 0, 0, 0, 0 }, "Mov32RR", "!0r,!1r" },
  { kX86Mov32RM, kRegMem,    IS_LOAD  | IS_TERTIARY_OP | REG_DEF0_USE1,  { 0,             0, 0x8B, 0, 0, 0, 0, 0 }, "Mov32RM", "!0r,[!1r+!2d]" },
  { kX86Mov32RA, kRegArray,  IS_LOAD  | IS_QUIN_OP     | REG_DEF0_USE12, { 0,             0, 0x8B, 0, 0, 0, 0, 0 }, "Mov32RA", "!0r,[!1r+!2r<<!3d+!4d]" },
  { kX86Mov32RT, kRegThread, IS_LOAD  | IS_BINARY_OP   | REG_DEF0,       { THREAD_PREFIX, 0, 0x8B, 0, 0, 0, 0, 0 }, "Mov32RT", "!0r,fs:[!1d]" },
  { kX86Mov32RI, kMovRegImm,            IS_BINARY_OP   | REG_DEF0,       { 0,             0, 0xB8, 0, 0, 0, 0, 4 }, "Mov32RI", "!0r,!1d" },
  { kX86Mov32MI, kMemImm,    IS_STORE | IS_TERTIARY_OP | REG_USE0,       { 0,             0, 0xC7, 0, 0, 0, 0, 4 }, "Mov32MI", "[!0r+!1d],!2d" },
  { kX86Mov32AI, kArrayImm,  IS_STORE | IS_QUIN_OP     | REG_USE01,      { 0,             0, 0xC7, 0, 0, 0, 0, 4 }, "Mov32AI", "[!0r+!1r<<!2d+!3d],!4d" },
  { kX86Mov32TI, kThreadImm, IS_STORE | IS_BINARY_OP,                    { THREAD_PREFIX, 0, 0xC7, 0, 0, 0, 0, 4 }, "Mov32TI", "fs:[!0d],!1d" },

  { kX86Lea32RM, kRegMem, IS_TERTIARY_OP | IS_LOAD | REG_DEF0_USE1,      { 0, 0, 0x8D, 0, 0, 0, 0, 0 }, "Lea32RM", "!0r,[!1r+!2d]" },

  { kX86Lea32RA, kRegArray, IS_QUIN_OP | REG_DEF0_USE12, { 0, 0, 0x8D, 0, 0, 0, 0, 0 }, "Lea32RA", "!0r,[!1r+!2r<<!3d+!4d]" },

  { kX86Cmov32RRC, kRegRegCond, IS_TERTIARY_OP | REG_DEF0_USE01 | USES_CCODES, {0, 0, 0x0F, 0x40, 0, 0, 0, 0}, "Cmovcc32RR", "!2c !0r,!1r" },

  { kX86Cmov32RMC, kRegMemCond, IS_QUAD_OP | IS_LOAD | REG_DEF0_USE01 | USES_CCODES, {0, 0, 0x0F, 0x40, 0, 0, 0, 0}, "Cmovcc32RM", "!3c !0r,[!1r+!2d]" },

#define SHIFT_ENCODING_MAP(opname, modrm_opcode) \
{ kX86 ## opname ## 8RI, kShiftRegImm,                        IS_BINARY_OP   | REG_DEF0_USE0 |            SETS_CCODES, { 0,    0, 0xC0, 0, 0, modrm_opcode, 0xD1, 1 }, #opname "8RI", "!0r,!1d" }, \
{ kX86 ## opname ## 8MI, kShiftMemImm,   IS_LOAD | IS_STORE | IS_TERTIARY_OP | REG_USE0      |            SETS_CCODES, { 0,    0, 0xC0, 0, 0, modrm_opcode, 0xD1, 1 }, #opname "8MI", "[!0r+!1d],!2d" }, \
{ kX86 ## opname ## 8AI, kShiftArrayImm, IS_LOAD | IS_STORE | IS_QUIN_OP     | REG_USE01     |            SETS_CCODES, { 0,    0, 0xC0, 0, 0, modrm_opcode, 0xD1, 1 }, #opname "8AI", "[!0r+!1r<<!2d+!3d],!4d" }, \
{ kX86 ## opname ## 8RC, kShiftRegCl,                         IS_BINARY_OP   | REG_DEF0_USE0 | REG_USEC | SETS_CCODES, { 0,    0, 0xD2, 0, 0, modrm_opcode, 0,    1 }, #opname "8RC", "!0r,cl" }, \
{ kX86 ## opname ## 8MC, kShiftMemCl,    IS_LOAD | IS_STORE | IS_TERTIARY_OP | REG_USE0      | REG_USEC | SETS_CCODES, { 0,    0, 0xD2, 0, 0, modrm_opcode, 0,    1 }, #opname "8MC", "[!0r+!1d],cl" }, \
{ kX86 ## opname ## 8AC, kShiftArrayCl,  IS_LOAD | IS_STORE | IS_QUIN_OP     | REG_USE01     | REG_USEC | SETS_CCODES, { 0,    0, 0xD2, 0, 0, modrm_opcode, 0,    1 }, #opname "8AC", "[!0r+!1r<<!2d+!3d],cl" }, \
  \
{ kX86 ## opname ## 16RI, kShiftRegImm,                        IS_BINARY_OP   | REG_DEF0_USE0 |            SETS_CCODES, { 0x66, 0, 0xC1, 0, 0, modrm_opcode, 0xD1, 1 }, #opname "16RI", "!0r,!1d" }, \
{ kX86 ## opname ## 16MI, kShiftMemImm,   IS_LOAD | IS_STORE | IS_TERTIARY_OP | REG_USE0      |            SETS_CCODES, { 0x66, 0, 0xC1, 0, 0, modrm_opcode, 0xD1, 1 }, #opname "16MI", "[!0r+!1d],!2d" }, \
{ kX86 ## opname ## 16AI, kShiftArrayImm, IS_LOAD | IS_STORE | IS_QUIN_OP     | REG_USE01     |            SETS_CCODES, { 0x66, 0, 0xC1, 0, 0, modrm_opcode, 0xD1, 1 }, #opname "16AI", "[!0r+!1r<<!2d+!3d],!4d" }, \
{ kX86 ## opname ## 16RC, kShiftRegCl,                         IS_BINARY_OP   | REG_DEF0_USE0 | REG_USEC | SETS_CCODES, { 0x66, 0, 0xD3, 0, 0, modrm_opcode, 0,    1 }, #opname "16RC", "!0r,cl" }, \
{ kX86 ## opname ## 16MC, kShiftMemCl,    IS_LOAD | IS_STORE | IS_TERTIARY_OP | REG_USE0      | REG_USEC | SETS_CCODES, { 0x66, 0, 0xD3, 0, 0, modrm_opcode, 0,    1 }, #opname "16MC", "[!0r+!1d],cl" }, \
{ kX86 ## opname ## 16AC, kShiftArrayCl,  IS_LOAD | IS_STORE | IS_QUIN_OP     | REG_USE01     | REG_USEC | SETS_CCODES, { 0x66, 0, 0xD3, 0, 0, modrm_opcode, 0,    1 }, #opname "16AC", "[!0r+!1r<<!2d+!3d],cl" }, \
  \
{ kX86 ## opname ## 32RI, kShiftRegImm,                        IS_BINARY_OP   | REG_DEF0_USE0 |            SETS_CCODES, { 0,    0, 0xC1, 0, 0, modrm_opcode, 0xD1, 1 }, #opname "32RI", "!0r,!1d" }, \
{ kX86 ## opname ## 32MI, kShiftMemImm,   IS_LOAD | IS_STORE | IS_TERTIARY_OP | REG_USE0      |            SETS_CCODES, { 0,    0, 0xC1, 0, 0, modrm_opcode, 0xD1, 1 }, #opname "32MI", "[!0r+!1d],!2d" }, \
{ kX86 ## opname ## 32AI, kShiftArrayImm, IS_LOAD | IS_STORE | IS_QUIN_OP     | REG_USE01     |            SETS_CCODES, { 0,    0, 0xC1, 0, 0, modrm_opcode, 0xD1, 1 }, #opname "32AI", "[!0r+!1r<<!2d+!3d],!4d" }, \
{ kX86 ## opname ## 32RC, kShiftRegCl,                         IS_BINARY_OP   | REG_DEF0_USE0 | REG_USEC | SETS_CCODES, { 0,    0, 0xD3, 0, 0, modrm_opcode, 0,    0 }, #opname "32RC", "!0r,cl" }, \
{ kX86 ## opname ## 32MC, kShiftMemCl,    IS_LOAD | IS_STORE | IS_TERTIARY_OP | REG_USE0      | REG_USEC | SETS_CCODES, { 0,    0, 0xD3, 0, 0, modrm_opcode, 0,    0 }, #opname "32MC", "[!0r+!1d],cl" }, \
{ kX86 ## opname ## 32AC, kShiftArrayCl,  IS_LOAD | IS_STORE | IS_QUIN_OP     | REG_USE01     | REG_USEC | SETS_CCODES, { 0,    0, 0xD3, 0, 0, modrm_opcode, 0,    0 }, #opname "32AC", "[!0r+!1r<<!2d+!3d],cl" }

  SHIFT_ENCODING_MAP(Rol, 0x0),
  SHIFT_ENCODING_MAP(Ror, 0x1),
  SHIFT_ENCODING_MAP(Rcl, 0x2),
  SHIFT_ENCODING_MAP(Rcr, 0x3),
  SHIFT_ENCODING_MAP(Sal, 0x4),
  SHIFT_ENCODING_MAP(Shr, 0x5),
  SHIFT_ENCODING_MAP(Sar, 0x7),
#undef SHIFT_ENCODING_MAP

  { kX86Cmc, kNullary, NO_OPERAND, { 0, 0, 0xF5, 0, 0, 0, 0, 0}, "Cmc", "" },
  { kX86Shld32RRI,  kRegRegImmRev, IS_TERTIARY_OP | REG_DEF0_USE01  | SETS_CCODES, { 0,    0, 0x0F, 0xA4, 0, 0, 0, 1}, "Shld32RRI", "!0r,!1r,!2d" },
  { kX86Shld32MRI,  kMemRegImm,    IS_QUAD_OP | REG_USE02 | IS_LOAD | IS_STORE | SETS_CCODES, { 0,    0, 0x0F, 0xA4, 0, 0, 0, 1}, "Shld32MRI", "[!0r+!1d],!2r,!3d" },
  { kX86Shrd32RRI,  kRegRegImmRev, IS_TERTIARY_OP | REG_DEF0_USE01  | SETS_CCODES, { 0,    0, 0x0F, 0xAC, 0, 0, 0, 1}, "Shrd32RRI", "!0r,!1r,!2d" },
  { kX86Shrd32MRI,  kMemRegImm,    IS_QUAD_OP | REG_USE02 | IS_LOAD | IS_STORE | SETS_CCODES, { 0,    0, 0x0F, 0xAC, 0, 0, 0, 1}, "Shrd32MRI", "[!0r+!1d],!2r,!3d" },

  { kX86Test8RI,  kRegImm,             IS_BINARY_OP   | REG_USE0  | SETS_CCODES, { 0,    0, 0xF6, 0, 0, 0, 0, 1}, "Test8RI", "!0r,!1d" },
  { kX86Test8MI,  kMemImm,   IS_LOAD | IS_TERTIARY_OP | REG_USE0  | SETS_CCODES, { 0,    0, 0xF6, 0, 0, 0, 0, 1}, "Test8MI", "[!0r+!1d],!2d" },
  { kX86Test8AI,  kArrayImm, IS_LOAD | IS_QUIN_OP     | REG_USE01 | SETS_CCODES, { 0,    0, 0xF6, 0, 0, 0, 0, 1}, "Test8AI", "[!0r+!1r<<!2d+!3d],!4d" },
  { kX86Test16RI, kRegImm,             IS_BINARY_OP   | REG_USE0  | SETS_CCODES, { 0x66, 0, 0xF7, 0, 0, 0, 0, 2}, "Test16RI", "!0r,!1d" },
  { kX86Test16MI, kMemImm,   IS_LOAD | IS_TERTIARY_OP | REG_USE0  | SETS_CCODES, { 0x66, 0, 0xF7, 0, 0, 0, 0, 2}, "Test16MI", "[!0r+!1d],!2d" },
  { kX86Test16AI, kArrayImm, IS_LOAD | IS_QUIN_OP     | REG_USE01 | SETS_CCODES, { 0x66, 0, 0xF7, 0, 0, 0, 0, 2}, "Test16AI", "[!0r+!1r<<!2d+!3d],!4d" },
  { kX86Test32RI, kRegImm,             IS_BINARY_OP   | REG_USE0  | SETS_CCODES, { 0,    0, 0xF7, 0, 0, 0, 0, 4}, "Test32RI", "!0r,!1d" },
  { kX86Test32MI, kMemImm,   IS_LOAD | IS_TERTIARY_OP | REG_USE0  | SETS_CCODES, { 0,    0, 0xF7, 0, 0, 0, 0, 4}, "Test32MI", "[!0r+!1d],!2d" },
  { kX86Test32AI, kArrayImm, IS_LOAD | IS_QUIN_OP     | REG_USE01 | SETS_CCODES, { 0,    0, 0xF7, 0, 0, 0, 0, 4}, "Test32AI", "[!0r+!1r<<!2d+!3d],!4d" },
  { kX86Test32RR, kRegReg,             IS_BINARY_OP   | REG_USE01 | SETS_CCODES, { 0,    0, 0x85, 0, 0, 0, 0, 0}, "Test32RR", "!0r,!1r" },

#define UNARY_ENCODING_MAP(opname, modrm, is_store, sets_ccodes, \
                           reg, reg_kind, reg_flags, \
                           mem, mem_kind, mem_flags, \
                           arr, arr_kind, arr_flags, imm, \
                           b_flags, hw_flags, w_flags, \
                           b_format, hw_format, w_format) \
{ kX86 ## opname ## 8 ## reg,  reg_kind,                      reg_flags | b_flags  | sets_ccodes, { 0,    0, 0xF6, 0, 0, modrm, 0, imm << 0}, #opname "8" #reg, b_format "!0r" }, \
{ kX86 ## opname ## 8 ## mem,  mem_kind, IS_LOAD | is_store | mem_flags | b_flags  | sets_ccodes, { 0,    0, 0xF6, 0, 0, modrm, 0, imm << 0}, #opname "8" #mem, b_format "[!0r+!1d]" }, \
{ kX86 ## opname ## 8 ## arr,  arr_kind, IS_LOAD | is_store | arr_flags | b_flags  | sets_ccodes, { 0,    0, 0xF6, 0, 0, modrm, 0, imm << 0}, #opname "8" #arr, b_format "[!0r+!1r<<!2d+!3d]" }, \
{ kX86 ## opname ## 16 ## reg, reg_kind,                      reg_flags | hw_flags | sets_ccodes, { 0x66, 0, 0xF7, 0, 0, modrm, 0, imm << 1}, #opname "16" #reg, hw_format "!0r" }, \
{ kX86 ## opname ## 16 ## mem, mem_kind, IS_LOAD | is_store | mem_flags | hw_flags | sets_ccodes, { 0x66, 0, 0xF7, 0, 0, modrm, 0, imm << 1}, #opname "16" #mem, hw_format "[!0r+!1d]" }, \
{ kX86 ## opname ## 16 ## arr, arr_kind, IS_LOAD | is_store | arr_flags | hw_flags | sets_ccodes, { 0x66, 0, 0xF7, 0, 0, modrm, 0, imm << 1}, #opname "16" #arr, hw_format "[!0r+!1r<<!2d+!3d]" }, \
{ kX86 ## opname ## 32 ## reg, reg_kind,                      reg_flags | w_flags  | sets_ccodes, { 0,    0, 0xF7, 0, 0, modrm, 0, imm << 2}, #opname "32" #reg, w_format "!0r" }, \
{ kX86 ## opname ## 32 ## mem, mem_kind, IS_LOAD | is_store | mem_flags | w_flags  | sets_ccodes, { 0,    0, 0xF7, 0, 0, modrm, 0, imm << 2}, #opname "32" #mem, w_format "[!0r+!1d]" }, \
{ kX86 ## opname ## 32 ## arr, arr_kind, IS_LOAD | is_store | arr_flags | w_flags  | sets_ccodes, { 0,    0, 0xF7, 0, 0, modrm, 0, imm << 2}, #opname "32" #arr, w_format "[!0r+!1r<<!2d+!3d]" }

  UNARY_ENCODING_MAP(Not, 0x2, IS_STORE, 0,           R, kReg, IS_UNARY_OP | REG_DEF0_USE0, M, kMem, IS_BINARY_OP | REG_USE0, A, kArray, IS_QUAD_OP | REG_USE01, 0, 0, 0, 0, "", "", ""),
  UNARY_ENCODING_MAP(Neg, 0x3, IS_STORE, SETS_CCODES, R, kReg, IS_UNARY_OP | REG_DEF0_USE0, M, kMem, IS_BINARY_OP | REG_USE0, A, kArray, IS_QUAD_OP | REG_USE01, 0, 0, 0, 0, "", "", ""),

  UNARY_ENCODING_MAP(Mul,     0x4, 0, SETS_CCODES, DaR, kReg, IS_UNARY_OP | REG_USE0, DaM, kMem, IS_BINARY_OP | REG_USE0, DaA, kArray, IS_QUAD_OP | REG_USE01, 0, REG_DEFA_USEA, REG_DEFAD_USEA,  REG_DEFAD_USEA,  "ax,al,", "dx:ax,ax,", "edx:eax,eax,"),
  UNARY_ENCODING_MAP(Imul,    0x5, 0, SETS_CCODES, DaR, kReg, IS_UNARY_OP | REG_USE0, DaM, kMem, IS_BINARY_OP | REG_USE0, DaA, kArray, IS_QUAD_OP | REG_USE01, 0, REG_DEFA_USEA, REG_DEFAD_USEA,  REG_DEFAD_USEA,  "ax,al,", "dx:ax,ax,", "edx:eax,eax,"),
  UNARY_ENCODING_MAP(Divmod,  0x6, 0, SETS_CCODES, DaR, kReg, IS_UNARY_OP | REG_USE0, DaM, kMem, IS_BINARY_OP | REG_USE0, DaA, kArray, IS_QUAD_OP | REG_USE01, 0, REG_DEFA_USEA, REG_DEFAD_USEAD, REG_DEFAD_USEAD, "ah:al,ax,", "dx:ax,dx:ax,", "edx:eax,edx:eax,"),
  UNARY_ENCODING_MAP(Idivmod, 0x7, 0, SETS_CCODES, DaR, kReg, IS_UNARY_OP | REG_USE0, DaM, kMem, IS_BINARY_OP | REG_USE0, DaA, kArray, IS_QUAD_OP | REG_USE01, 0, REG_DEFA_USEA, REG_DEFAD_USEAD, REG_DEFAD_USEAD, "ah:al,ax,", "dx:ax,dx:ax,", "edx:eax,edx:eax,"),
#undef UNARY_ENCODING_MAP

  { kx86Cdq32Da, kRegOpcode, NO_OPERAND | REG_DEFAD_USEA,                                  { 0, 0, 0x99, 0, 0, 0, 0, 0 }, "Cdq", "" },
  { kX86Bswap32R, kRegOpcode, IS_UNARY_OP | REG_DEF0_USE0,                                 { 0, 0, 0x0F, 0xC8, 0, 0, 0, 0 }, "Bswap32R", "!0r" },
  { kX86Push32R,  kRegOpcode, IS_UNARY_OP | REG_USE0 | REG_USE_SP | REG_DEF_SP | IS_STORE, { 0, 0, 0x50, 0,    0, 0, 0, 0 }, "Push32R",  "!0r" },
  { kX86Pop32R,   kRegOpcode, IS_UNARY_OP | REG_DEF0 | REG_USE_SP | REG_DEF_SP | IS_LOAD,  { 0, 0, 0x58, 0,    0, 0, 0, 0 }, "Pop32R",   "!0r" },

#define EXT_0F_ENCODING_MAP(opname, prefix, opcode, reg_def) \
{ kX86 ## opname ## RR, kRegReg,             IS_BINARY_OP   | reg_def | REG_USE1,  { prefix, 0, 0x0F, opcode, 0, 0, 0, 0 }, #opname "RR", "!0r,!1r" }, \
{ kX86 ## opname ## RM, kRegMem,   IS_LOAD | IS_TERTIARY_OP | reg_def | REG_USE1,  { prefix, 0, 0x0F, opcode, 0, 0, 0, 0 }, #opname "RM", "!0r,[!1r+!2d]" }, \
{ kX86 ## opname ## RA, kRegArray, IS_LOAD | IS_QUIN_OP     | reg_def | REG_USE12, { prefix, 0, 0x0F, opcode, 0, 0, 0, 0 }, #opname "RA", "!0r,[!1r+!2r<<!3d+!4d]" }

  EXT_0F_ENCODING_MAP(Movsd, 0xF2, 0x10, REG_DEF0),
  { kX86MovsdMR, kMemReg,   IS_STORE | IS_TERTIARY_OP | REG_USE02,  { 0xF2, 0, 0x0F, 0x11, 0, 0, 0, 0 }, "MovsdMR", "[!0r+!1d],!2r" },
  { kX86MovsdAR, kArrayReg, IS_STORE | IS_QUIN_OP     | REG_USE014, { 0xF2, 0, 0x0F, 0x11, 0, 0, 0, 0 }, "MovsdAR", "[!0r+!1r<<!2d+!3d],!4r" },

  EXT_0F_ENCODING_MAP(Movss, 0xF3, 0x10, REG_DEF0),
  { kX86MovssMR, kMemReg,   IS_STORE | IS_TERTIARY_OP | REG_USE02,  { 0xF3, 0, 0x0F, 0x11, 0, 0, 0, 0 }, "MovssMR", "[!0r+!1d],!2r" },
  { kX86MovssAR, kArrayReg, IS_STORE | IS_QUIN_OP     | REG_USE014, { 0xF3, 0, 0x0F, 0x11, 0, 0, 0, 0 }, "MovssAR", "[!0r+!1r<<!2d+!3d],!4r" },

  EXT_0F_ENCODING_MAP(Cvtsi2sd,  0xF2, 0x2A, REG_DEF0),
  EXT_0F_ENCODING_MAP(Cvtsi2ss,  0xF3, 0x2A, REG_DEF0),
  EXT_0F_ENCODING_MAP(Cvttsd2si, 0xF2, 0x2C, REG_DEF0),
  EXT_0F_ENCODING_MAP(Cvttss2si, 0xF3, 0x2C, REG_DEF0),
  EXT_0F_ENCODING_MAP(Cvtsd2si,  0xF2, 0x2D, REG_DEF0),
  EXT_0F_ENCODING_MAP(Cvtss2si,  0xF3, 0x2D, REG_DEF0),
  EXT_0F_ENCODING_MAP(Ucomisd,   0x66, 0x2E, SETS_CCODES|REG_USE0),
  EXT_0F_ENCODING_MAP(Ucomiss,   0x00, 0x2E, SETS_CCODES|REG_USE0),
  EXT_0F_ENCODING_MAP(Comisd,    0x66, 0x2F, SETS_CCODES|REG_USE0),
  EXT_0F_ENCODING_MAP(Comiss,    0x00, 0x2F, SETS_CCODES|REG_USE0),
  EXT_0F_ENCODING_MAP(Orps,      0x00, 0x56, REG_DEF0_USE0),
  EXT_0F_ENCODING_MAP(Xorps,     0x00, 0x57, REG_DEF0_USE0),
  EXT_0F_ENCODING_MAP(Addsd,     0xF2, 0x58, REG_DEF0_USE0),
  EXT_0F_ENCODING_MAP(Addss,     0xF3, 0x58, REG_DEF0_USE0),
  EXT_0F_ENCODING_MAP(Mulsd,     0xF2, 0x59, REG_DEF0_USE0),
  EXT_0F_ENCODING_MAP(Mulss,     0xF3, 0x59, REG_DEF0_USE0),
  EXT_0F_ENCODING_MAP(Cvtsd2ss,  0xF2, 0x5A, REG_DEF0),
  EXT_0F_ENCODING_MAP(Cvtss2sd,  0xF3, 0x5A, REG_DEF0),
  EXT_0F_ENCODING_MAP(Subsd,     0xF2, 0x5C, REG_DEF0_USE0),
  EXT_0F_ENCODING_MAP(Subss,     0xF3, 0x5C, REG_DEF0_USE0),
  EXT_0F_ENCODING_MAP(Divsd,     0xF2, 0x5E, REG_DEF0_USE0),
  EXT_0F_ENCODING_MAP(Divss,     0xF3, 0x5E, REG_DEF0_USE0),
  EXT_0F_ENCODING_MAP(Punpckldq, 0x66, 0x62, REG_DEF0_USE0),

  { kX86PsrlqRI, kRegImm, IS_BINARY_OP | REG_DEF0_USE0, { 0x66, 0, 0x0F, 0x73, 0, 2, 0, 1 }, "PsrlqRI", "!0r,!1d" },
  { kX86PsllqRI, kRegImm, IS_BINARY_OP | REG_DEF0_USE0, { 0x66, 0, 0x0F, 0x73, 0, 6, 0, 1 }, "PsllqRI", "!0r,!1d" },
  { kX86SqrtsdRR, kRegReg, IS_BINARY_OP | REG_DEF0_USE1, { 0xF2, 0, 0x0F, 0x51, 0, 0, 0, 0 }, "SqrtsdRR", "!0r,!1r" },

  { kX86Fild32M, kMem, IS_LOAD | IS_UNARY_OP | REG_USE0 | USE_FP_STACK, { 0x0, 0, 0xDB, 0x00, 0, 0, 0, 0 }, "Fild32M", "[!0r,!1d]" },
  { kX86Fild64M, kMem, IS_LOAD | IS_UNARY_OP | REG_USE0 | USE_FP_STACK, { 0x0, 0, 0xDF, 0x00, 0, 5, 0, 0 }, "Fild64M", "[!0r,!1d]" },
  { kX86Fstp32M, kMem, IS_STORE | IS_UNARY_OP | REG_USE0 | USE_FP_STACK, { 0x0, 0, 0xD9, 0x00, 0, 3, 0, 0 }, "FstpsM", "[!0r,!1d]" },
  { kX86Fstp64M, kMem, IS_STORE | IS_UNARY_OP | REG_USE0 | USE_FP_STACK, { 0x0, 0, 0xDD, 0x00, 0, 3, 0, 0 }, "FstpdM", "[!0r,!1d]" },

  EXT_0F_ENCODING_MAP(Movups,    0x0, 0x10, REG_DEF0),
  { kX86MovupsMR, kMemReg,      IS_STORE | IS_TERTIARY_OP | REG_USE02,  { 0x0, 0, 0x0F, 0x11, 0, 0, 0, 0 }, "MovupsMR", "[!0r+!1d],!2r" },
  { kX86MovupsAR, kArrayReg,    IS_STORE | IS_QUIN_OP     | REG_USE014, { 0x0, 0, 0x0F, 0x11, 0, 0, 0, 0 }, "MovupsAR", "[!0r+!1r<<!2d+!3d],!4r" },

  EXT_0F_ENCODING_MAP(Movaps,    0x0, 0x28, REG_DEF0),
  { kX86MovapsMR, kMemReg,      IS_STORE | IS_TERTIARY_OP | REG_USE02,  { 0x0, 0, 0x0F, 0x29, 0, 0, 0, 0 }, "MovapsMR", "[!0r+!1d],!2r" },
  { kX86MovapsAR, kArrayReg,    IS_STORE | IS_QUIN_OP     | REG_USE014, { 0x0, 0, 0x0F, 0x29, 0, 0, 0, 0 }, "MovapsAR", "[!0r+!1r<<!2d+!3d],!4r" },

  { kX86MovlpsRM, kRegMem,      IS_LOAD | IS_TERTIARY_OP | REG_DEF0 | REG_USE01,  { 0x0, 0, 0x0F, 0x12, 0, 0, 0, 0 }, "MovlpsRM", "!0r,[!1r+!2d]" },
  { kX86MovlpsRA, kRegArray,    IS_LOAD | IS_QUIN_OP     | REG_DEF0 | REG_USE012, { 0x0, 0, 0x0F, 0x12, 0, 0, 0, 0 }, "MovlpsRA", "!0r,[!1r+!2r<<!3d+!4d]" },
  { kX86MovlpsMR, kMemReg,      IS_STORE | IS_TERTIARY_OP | REG_USE02,  { 0x0, 0, 0x0F, 0x13, 0, 0, 0, 0 }, "MovlpsMR", "[!0r+!1d],!2r" },
  { kX86MovlpsAR, kArrayReg,    IS_STORE | IS_QUIN_OP     | REG_USE014, { 0x0, 0, 0x0F, 0x13, 0, 0, 0, 0 }, "MovlpsAR", "[!0r+!1r<<!2d+!3d],!4r" },

  { kX86MovhpsRM, kRegMem,      IS_LOAD | IS_TERTIARY_OP | REG_DEF0 | REG_USE01,  { 0x0, 0, 0x0F, 0x16, 0, 0, 0, 0 }, "MovhpsRM", "!0r,[!1r+!2d]" },
  { kX86MovhpsRA, kRegArray,    IS_LOAD | IS_QUIN_OP     | REG_DEF0 | REG_USE012, { 0x0, 0, 0x0F, 0x16, 0, 0, 0, 0 }, "MovhpsRA", "!0r,[!1r+!2r<<!3d+!4d]" },
  { kX86MovhpsMR, kMemReg,      IS_STORE | IS_TERTIARY_OP | REG_USE02,  { 0x0, 0, 0x0F, 0x17, 0, 0, 0, 0 }, "MovhpsMR", "[!0r+!1d],!2r" },
  { kX86MovhpsAR, kArrayReg,    IS_STORE | IS_QUIN_OP     | REG_USE014, { 0x0, 0, 0x0F, 0x17, 0, 0, 0, 0 }, "MovhpsAR", "[!0r+!1r<<!2d+!3d],!4r" },

  EXT_0F_ENCODING_MAP(Movdxr,    0x66, 0x6E, REG_DEF0),
  { kX86MovdrxRR, kRegRegStore, IS_BINARY_OP | REG_DEF0   | REG_USE1,   { 0x66, 0, 0x0F, 0x7E, 0, 0, 0, 0 }, "MovdrxRR", "!0r,!1r" },
  { kX86MovdrxMR, kMemReg,      IS_STORE | IS_TERTIARY_OP | REG_USE02,  { 0x66, 0, 0x0F, 0x7E, 0, 0, 0, 0 }, "MovdrxMR", "[!0r+!1d],!2r" },
  { kX86MovdrxAR, kArrayReg,    IS_STORE | IS_QUIN_OP     | REG_USE014, { 0x66, 0, 0x0F, 0x7E, 0, 0, 0, 0 }, "MovdrxAR", "[!0r+!1r<<!2d+!3d],!4r" },

  { kX86Set8R, kRegCond,              IS_BINARY_OP   | REG_DEF0  | USES_CCODES, { 0, 0, 0x0F, 0x90, 0, 0, 0, 0 }, "Set8R", "!1c !0r" },
  { kX86Set8M, kMemCond,   IS_STORE | IS_TERTIARY_OP | REG_USE0  | USES_CCODES, { 0, 0, 0x0F, 0x90, 0, 0, 0, 0 }, "Set8M", "!2c [!0r+!1d]" },
  { kX86Set8A, kArrayCond, IS_STORE | IS_QUIN_OP     | REG_USE01 | USES_CCODES, { 0, 0, 0x0F, 0x90, 0, 0, 0, 0 }, "Set8A", "!4c [!0r+!1r<<!2d+!3d]" },

  // TODO: load/store?
  // Encode the modrm opcode as an extra opcode byte to avoid computation during assembly.
  { kX86Mfence, kReg,                 NO_OPERAND,     { 0, 0, 0x0F, 0xAE, 0, 6, 0, 0 }, "Mfence", "" },

  EXT_0F_ENCODING_MAP(Imul16,  0x66, 0xAF, REG_USE0 | REG_DEF0 | SETS_CCODES),
  EXT_0F_ENCODING_MAP(Imul32,  0x00, 0xAF, REG_USE0 | REG_DEF0 | SETS_CCODES),

  { kX86CmpxchgRR, kRegRegStore, IS_BINARY_OP | REG_DEF0 | REG_USE01 | REG_DEFA_USEA | SETS_CCODES, { 0, 0, 0x0F, 0xB1, 0, 0, 0, 0 }, "Cmpxchg", "!0r,!1r" },
  { kX86CmpxchgMR, kMemReg,   IS_STORE | IS_TERTIARY_OP | REG_USE02 | REG_DEFA_USEA | SETS_CCODES, { 0, 0, 0x0F, 0xB1, 0, 0, 0, 0 }, "Cmpxchg", "[!0r+!1d],!2r" },
  { kX86CmpxchgAR, kArrayReg, IS_STORE | IS_QUIN_OP | REG_USE014 | REG_DEFA_USEA | SETS_CCODES, { 0, 0, 0x0F, 0xB1, 0, 0, 0, 0 }, "Cmpxchg", "[!0r+!1r<<!2d+!3d],!4r" },
  { kX86LockCmpxchgMR, kMemReg,   IS_STORE | IS_TERTIARY_OP | REG_USE02 | REG_DEFA_USEA | SETS_CCODES, { 0xF0, 0, 0x0F, 0xB1, 0, 0, 0, 0 }, "Lock Cmpxchg", "[!0r+!1d],!2r" },
  { kX86LockCmpxchgAR, kArrayReg, IS_STORE | IS_QUIN_OP | REG_USE014 | REG_DEFA_USEA | SETS_CCODES, { 0xF0, 0, 0x0F, 0xB1, 0, 0, 0, 0 }, "Lock Cmpxchg", "[!0r+!1r<<!2d+!3d],!4r" },
  { kX86LockCmpxchg8bM, kMem,   IS_STORE | IS_BINARY_OP | REG_USE0 | REG_DEFAD_USEAD | REG_USEC | REG_USEB | SETS_CCODES, { 0xF0, 0, 0x0F, 0xC7, 0, 1, 0, 0 }, "Lock Cmpxchg8b", "[!0r+!1d]" },
  { kX86LockCmpxchg8bA, kArray, IS_STORE | IS_QUAD_OP | REG_USE01 | REG_DEFAD_USEAD | REG_USEC | REG_USEB | SETS_CCODES, { 0xF0, 0, 0x0F, 0xC7, 0, 1, 0, 0 }, "Lock Cmpxchg8b", "[!0r+!1r<<!2d+!3d]" },
  { kX86XchgMR, kMemReg, IS_STORE | IS_LOAD | IS_TERTIARY_OP | REG_DEF2 | REG_USE02, { 0, 0, 0x87, 0, 0, 0, 0, 0 }, "Xchg", "[!0r+!1d],!2r" },

  EXT_0F_ENCODING_MAP(Movzx8,  0x00, 0xB6, REG_DEF0),
  EXT_0F_ENCODING_MAP(Movzx16, 0x00, 0xB7, REG_DEF0),
  EXT_0F_ENCODING_MAP(Movsx8,  0x00, 0xBE, REG_DEF0),
  EXT_0F_ENCODING_MAP(Movsx16, 0x00, 0xBF, REG_DEF0),
#undef EXT_0F_ENCODING_MAP

  { kX86Jcc8,  kJcc,  IS_BINARY_OP | IS_BRANCH | NEEDS_FIXUP | USES_CCODES, { 0,             0, 0x70, 0,    0, 0, 0, 0 }, "Jcc8",  "!1c !0t" },
  { kX86Jcc32, kJcc,  IS_BINARY_OP | IS_BRANCH | NEEDS_FIXUP | USES_CCODES, { 0,             0, 0x0F, 0x80, 0, 0, 0, 0 }, "Jcc32", "!1c !0t" },
  { kX86Jmp8,  kJmp,  IS_UNARY_OP  | IS_BRANCH | NEEDS_FIXUP,               { 0,             0, 0xEB, 0,    0, 0, 0, 0 }, "Jmp8",  "!0t" },
  { kX86Jmp32, kJmp,  IS_UNARY_OP  | IS_BRANCH | NEEDS_FIXUP,               { 0,             0, 0xE9, 0,    0, 0, 0, 0 }, "Jmp32", "!0t" },
  { kX86JmpR,  kJmp,  IS_UNARY_OP  | IS_BRANCH | REG_USE0,                  { 0,             0, 0xFF, 0,    0, 4, 0, 0 }, "JmpR",  "!0r" },
  { kX86Jecxz8, kJmp, NO_OPERAND   | IS_BRANCH | NEEDS_FIXUP | REG_USEC,    { 0,             0, 0xE3, 0,    0, 0, 0, 0 }, "Jecxz", "!0t" },
  { kX86JmpT,  kJmp,  IS_UNARY_OP  | IS_BRANCH | IS_LOAD,                   { THREAD_PREFIX, 0, 0xFF, 0,    0, 4, 0, 0 }, "JmpT",  "fs:[!0d]" },
  { kX86CallR, kCall, IS_UNARY_OP  | IS_BRANCH | REG_USE0,                  { 0,             0, 0xE8, 0,    0, 0, 0, 0 }, "CallR", "!0r" },
  { kX86CallM, kCall, IS_BINARY_OP | IS_BRANCH | IS_LOAD | REG_USE0,        { 0,             0, 0xFF, 0,    0, 2, 0, 0 }, "CallM", "[!0r+!1d]" },
  { kX86CallA, kCall, IS_QUAD_OP   | IS_BRANCH | IS_LOAD | REG_USE01,       { 0,             0, 0xFF, 0,    0, 2, 0, 0 }, "CallA", "[!0r+!1r<<!2d+!3d]" },
  { kX86CallT, kCall, IS_UNARY_OP  | IS_BRANCH | IS_LOAD,                   { THREAD_PREFIX, 0, 0xFF, 0,    0, 2, 0, 0 }, "CallT", "fs:[!0d]" },
  { kX86CallI, kCall, IS_UNARY_OP  | IS_BRANCH,                             { 0,             0, 0xE8, 0,    0, 0, 0, 4 }, "CallI", "!0d" },
  { kX86Ret,   kNullary, NO_OPERAND | IS_BRANCH,                            { 0,             0, 0xC3, 0,    0, 0, 0, 0 }, "Ret", "" },

  { kX86StartOfMethod, kMacro,  IS_UNARY_OP | SETS_CCODES,             { 0, 0, 0,    0, 0, 0, 0, 0 }, "StartOfMethod", "!0r" },
  { kX86PcRelLoadRA,   kPcRel,  IS_LOAD | IS_QUIN_OP | REG_DEF0_USE12, { 0, 0, 0x8B, 0, 0, 0, 0, 0 }, "PcRelLoadRA",   "!0r,[!1r+!2r<<!3d+!4p]" },
  { kX86PcRelAdr,      kPcRel,  IS_LOAD | IS_BINARY_OP | REG_DEF0,     { 0, 0, 0xB8, 0, 0, 0, 0, 4 }, "PcRelAdr",      "!0r,!1d" },
  { kX86RepneScasw, kPrefix2Nullary, NO_OPERAND | REG_USEA | REG_USEC | SETS_CCODES, { 0x66, 0xF2, 0xAF, 0, 0, 0, 0, 0 }, "RepNE ScasW", "" },
};

static size_t ComputeSize(const X86EncodingMap* entry, int base, int displacement, bool has_sib) {
  size_t size = 0;
  if (entry->skeleton.prefix1 > 0) {
    ++size;
    if (entry->skeleton.prefix2 > 0) {
      ++size;
    }
  }
  ++size;  // opcode
  if (entry->skeleton.opcode == 0x0F) {
    ++size;
    if (entry->skeleton.extra_opcode1 == 0x38 || entry->skeleton.extra_opcode1 == 0x3A) {
      ++size;
    }
  }
  ++size;  // modrm
  if (has_sib || RegStorage::RegNum(base) == rs_rX86_SP.GetRegNum()) {
    // SP requires a SIB byte.
    ++size;
  }
  if (displacement != 0 || RegStorage::RegNum(base) == rs_rBP.GetRegNum()) {
    // BP requires an explicit displacement, even when it's 0.
    if (entry->opcode != kX86Lea32RA) {
      DCHECK_NE(entry->flags & (IS_LOAD | IS_STORE), 0ULL) << entry->name;
    }
    size += IS_SIMM8(displacement) ? 1 : 4;
  }
  size += entry->skeleton.immediate_bytes;
  return size;
}

int X86Mir2Lir::GetInsnSize(LIR* lir) {
  DCHECK(!IsPseudoLirOp(lir->opcode));
  const X86EncodingMap* entry = &X86Mir2Lir::EncodingMap[lir->opcode];
  switch (entry->kind) {
    case kData:
      return 4;  // 4 bytes of data
    case kNop:
      return lir->operands[0];  // length of nop is sole operand
    case kNullary:
      return 1;  // 1 byte of opcode
    case kPrefix2Nullary:
      return 3;  // 1 byte of opcode + 2 prefixes
    case kRegOpcode:  // lir operands - 0: reg
      return ComputeSize(entry, 0, 0, false) - 1;  // substract 1 for modrm
    case kReg:  // lir operands - 0: reg
      return ComputeSize(entry, 0, 0, false);
    case kMem:  // lir operands - 0: base, 1: disp
      return ComputeSize(entry, lir->operands[0], lir->operands[1], false);
    case kArray:  // lir operands - 0: base, 1: index, 2: scale, 3: disp
      return ComputeSize(entry, lir->operands[0], lir->operands[3], true);
    case kMemReg:  // lir operands - 0: base, 1: disp, 2: reg
      return ComputeSize(entry, lir->operands[0], lir->operands[1], false);
    case kMemRegImm:  // lir operands - 0: base, 1: disp, 2: reg 3: immediate
      return ComputeSize(entry, lir->operands[0], lir->operands[1], false);
    case kArrayReg:  // lir operands - 0: base, 1: index, 2: scale, 3: disp, 4: reg
      return ComputeSize(entry, lir->operands[0], lir->operands[3], true);
    case kThreadReg:  // lir operands - 0: disp, 1: reg
      return ComputeSize(entry, 0, lir->operands[0], false);
    case kRegReg:
      return ComputeSize(entry, 0, 0, false);
    case kRegRegStore:
      return ComputeSize(entry, 0, 0, false);
    case kRegMem:  // lir operands - 0: reg, 1: base, 2: disp
      return ComputeSize(entry, lir->operands[1], lir->operands[2], false);
    case kRegArray:   // lir operands - 0: reg, 1: base, 2: index, 3: scale, 4: disp
      return ComputeSize(entry, lir->operands[1], lir->operands[4], true);
    case kRegThread:  // lir operands - 0: reg, 1: disp
      return ComputeSize(entry, 0, 0x12345678, false);  // displacement size is always 32bit
    case kRegImm: {  // lir operands - 0: reg, 1: immediate
      size_t size = ComputeSize(entry, 0, 0, false);
      if (entry->skeleton.ax_opcode == 0) {
        return size;
      } else {
        // AX opcodes don't require the modrm byte.
        int reg = lir->operands[0];
        return size - (RegStorage::RegNum(reg) == rs_rAX.GetRegNum() ? 1 : 0);
      }
    }
    case kMemImm:  // lir operands - 0: base, 1: disp, 2: immediate
      return ComputeSize(entry, lir->operands[0], lir->operands[1], false);
    case kArrayImm:  // lir operands - 0: base, 1: index, 2: scale, 3: disp 4: immediate
      return ComputeSize(entry, lir->operands[0], lir->operands[3], true);
    case kThreadImm:  // lir operands - 0: disp, 1: imm
      return ComputeSize(entry, 0, 0x12345678, false);  // displacement size is always 32bit
    case kRegRegImm:  // lir operands - 0: reg, 1: reg, 2: imm
    case kRegRegImmRev:
      return ComputeSize(entry, 0, 0, false);
    case kRegMemImm:  // lir operands - 0: reg, 1: base, 2: disp, 3: imm
      return ComputeSize(entry, lir->operands[1], lir->operands[2], false);
    case kRegArrayImm:  // lir operands - 0: reg, 1: base, 2: index, 3: scale, 4: disp, 5: imm
      return ComputeSize(entry, lir->operands[1], lir->operands[4], true);
    case kMovRegImm:  // lir operands - 0: reg, 1: immediate
      return 1 + entry->skeleton.immediate_bytes;
    case kShiftRegImm:  // lir operands - 0: reg, 1: immediate
      // Shift by immediate one has a shorter opcode.
      return ComputeSize(entry, 0, 0, false) - (lir->operands[1] == 1 ? 1 : 0);
    case kShiftMemImm:  // lir operands - 0: base, 1: disp, 2: immediate
      // Shift by immediate one has a shorter opcode.
      return ComputeSize(entry, lir->operands[0], lir->operands[1], false) -
             (lir->operands[2] == 1 ? 1 : 0);
    case kShiftArrayImm:  // lir operands - 0: base, 1: index, 2: scale, 3: disp 4: immediate
      // Shift by immediate one has a shorter opcode.
      return ComputeSize(entry, lir->operands[0], lir->operands[3], true) -
             (lir->operands[4] == 1 ? 1 : 0);
    case kShiftRegCl:
      return ComputeSize(entry, 0, 0, false);
    case kShiftMemCl:  // lir operands - 0: base, 1: disp, 2: cl
      return ComputeSize(entry, lir->operands[0], lir->operands[1], false);
    case kShiftArrayCl:  // lir operands - 0: base, 1: index, 2: scale, 3: disp, 4: reg
      return ComputeSize(entry, lir->operands[0], lir->operands[3], true);
    case kRegCond:  // lir operands - 0: reg, 1: cond
      return ComputeSize(entry, 0, 0, false);
    case kMemCond:  // lir operands - 0: base, 1: disp, 2: cond
      return ComputeSize(entry, lir->operands[0], lir->operands[1], false);
    case kArrayCond:  // lir operands - 0: base, 1: index, 2: scale, 3: disp, 4: cond
      return ComputeSize(entry, lir->operands[0], lir->operands[3], true);
    case kRegRegCond:  // lir operands - 0: reg, 1: reg, 2: cond
      return ComputeSize(entry, 0, 0, false);
    case kRegMemCond:  // lir operands - 0: reg, 1: reg, 2: disp, 3:cond
      return ComputeSize(entry, lir->operands[1], lir->operands[2], false);
    case kJcc:
      if (lir->opcode == kX86Jcc8) {
        return 2;  // opcode + rel8
      } else {
        DCHECK(lir->opcode == kX86Jcc32);
        return 6;  // 2 byte opcode + rel32
      }
    case kJmp:
      if (lir->opcode == kX86Jmp8 || lir->opcode == kX86Jecxz8) {
        return 2;  // opcode + rel8
      } else if (lir->opcode == kX86Jmp32) {
        return 5;  // opcode + rel32
      } else if (lir->opcode == kX86JmpT) {
        return ComputeSize(entry, 0, 0x12345678, false);  // displacement size is always 32bit
      } else {
        DCHECK(lir->opcode == kX86JmpR);
        return 2;  // opcode + modrm
      }
    case kCall:
      switch (lir->opcode) {
        case kX86CallI: return 5;  // opcode 0:disp
        case kX86CallR: return 2;  // opcode modrm
        case kX86CallM:  // lir operands - 0: base, 1: disp
          return ComputeSize(entry, lir->operands[0], lir->operands[1], false);
        case kX86CallA:  // lir operands - 0: base, 1: index, 2: scale, 3: disp
          return ComputeSize(entry, lir->operands[0], lir->operands[3], true);
        case kX86CallT:  // lir operands - 0: disp
          return ComputeSize(entry, 0, 0x12345678, false);  // displacement size is always 32bit
        default:
          break;
      }
      break;
    case kPcRel:
      if (entry->opcode == kX86PcRelLoadRA) {
        // lir operands - 0: reg, 1: base, 2: index, 3: scale, 4: table
        return ComputeSize(entry, lir->operands[1], 0x12345678, true);
      } else {
        DCHECK(entry->opcode == kX86PcRelAdr);
        return 5;  // opcode with reg + 4 byte immediate
      }
    case kMacro:
      DCHECK_EQ(lir->opcode, static_cast<int>(kX86StartOfMethod));
      return 5 /* call opcode + 4 byte displacement */ + 1 /* pop reg */ +
          ComputeSize(&X86Mir2Lir::EncodingMap[kX86Sub32RI], 0, 0, false) -
          (RegStorage::RegNum(lir->operands[0]) == rs_rAX.GetRegNum()  ? 1 : 0);  // shorter ax encoding
    default:
      break;
  }
  UNIMPLEMENTED(FATAL) << "Unimplemented size encoding for: " << entry->name;
  return 0;
}

void X86Mir2Lir::EmitPrefix(const X86EncodingMap* entry) {
  if (entry->skeleton.prefix1 != 0) {
    code_buffer_.push_back(entry->skeleton.prefix1);
    if (entry->skeleton.prefix2 != 0) {
      code_buffer_.push_back(entry->skeleton.prefix2);
    }
  } else {
    DCHECK_EQ(0, entry->skeleton.prefix2);
  }
}

void X86Mir2Lir::EmitOpcode(const X86EncodingMap* entry) {
  code_buffer_.push_back(entry->skeleton.opcode);
  if (entry->skeleton.opcode == 0x0F) {
    code_buffer_.push_back(entry->skeleton.extra_opcode1);
    if (entry->skeleton.extra_opcode1 == 0x38 || entry->skeleton.extra_opcode1 == 0x3A) {
      code_buffer_.push_back(entry->skeleton.extra_opcode2);
    } else {
      DCHECK_EQ(0, entry->skeleton.extra_opcode2);
    }
  } else {
    DCHECK_EQ(0, entry->skeleton.extra_opcode1);
    DCHECK_EQ(0, entry->skeleton.extra_opcode2);
  }
}

void X86Mir2Lir::EmitPrefixAndOpcode(const X86EncodingMap* entry) {
  EmitPrefix(entry);
  EmitOpcode(entry);
}

static uint8_t ModrmForDisp(int base, int disp) {
  // BP requires an explicit disp, so do not omit it in the 0 case
  if (disp == 0 && RegStorage::RegNum(base) != rs_rBP.GetRegNum()) {
    return 0;
  } else if (IS_SIMM8(disp)) {
    return 1;
  } else {
    return 2;
  }
}

void X86Mir2Lir::EmitDisp(uint8_t base, int disp) {
  // BP requires an explicit disp, so do not omit it in the 0 case
  if (disp == 0 && RegStorage::RegNum(base) != rs_rBP.GetRegNum()) {
    return;
  } else if (IS_SIMM8(disp)) {
    code_buffer_.push_back(disp & 0xFF);
  } else {
    code_buffer_.push_back(disp & 0xFF);
    code_buffer_.push_back((disp >> 8) & 0xFF);
    code_buffer_.push_back((disp >> 16) & 0xFF);
    code_buffer_.push_back((disp >> 24) & 0xFF);
  }
}

void X86Mir2Lir::EmitModrmDisp(uint8_t reg_or_opcode, uint8_t base, int disp) {
  DCHECK_LT(RegStorage::RegNum(reg_or_opcode), 8);
  DCHECK_LT(RegStorage::RegNum(base), 8);
  uint8_t modrm = (ModrmForDisp(base, disp) << 6) | (RegStorage::RegNum(reg_or_opcode) << 3) |
     RegStorage::RegNum(base);
  code_buffer_.push_back(modrm);
  if (RegStorage::RegNum(base) == rs_rX86_SP.GetRegNum()) {
    // Special SIB for SP base
    code_buffer_.push_back(0 << 6 | rs_rX86_SP.GetRegNum() << 3 | rs_rX86_SP.GetRegNum());
  }
  EmitDisp(base, disp);
}

void X86Mir2Lir::EmitModrmSibDisp(uint8_t reg_or_opcode, uint8_t base, uint8_t index,
                                  int scale, int disp) {
  DCHECK_LT(RegStorage::RegNum(reg_or_opcode), 8);
  uint8_t modrm = (ModrmForDisp(base, disp) << 6) | RegStorage::RegNum(reg_or_opcode) << 3 |
      rs_rX86_SP.GetRegNum();
  code_buffer_.push_back(modrm);
  DCHECK_LT(scale, 4);
  DCHECK_LT(RegStorage::RegNum(index), 8);
  DCHECK_LT(RegStorage::RegNum(base), 8);
  uint8_t sib = (scale << 6) | (RegStorage::RegNum(index) << 3) | RegStorage::RegNum(base);
  code_buffer_.push_back(sib);
  EmitDisp(base, disp);
}

void X86Mir2Lir::EmitImm(const X86EncodingMap* entry, int imm) {
  switch (entry->skeleton.immediate_bytes) {
    case 1:
      DCHECK(IS_SIMM8(imm));
      code_buffer_.push_back(imm & 0xFF);
      break;
    case 2:
      DCHECK(IS_SIMM16(imm));
      code_buffer_.push_back(imm & 0xFF);
      code_buffer_.push_back((imm >> 8) & 0xFF);
      break;
    case 4:
      code_buffer_.push_back(imm & 0xFF);
      code_buffer_.push_back((imm >> 8) & 0xFF);
      code_buffer_.push_back((imm >> 16) & 0xFF);
      code_buffer_.push_back((imm >> 24) & 0xFF);
      break;
    default:
      LOG(FATAL) << "Unexpected immediate bytes (" << entry->skeleton.immediate_bytes
                 << ") for instruction: " << entry->name;
      break;
  }
}

void X86Mir2Lir::EmitOpRegOpcode(const X86EncodingMap* entry, uint8_t reg) {
  EmitPrefixAndOpcode(entry);
  // There's no 3-byte instruction with +rd
  DCHECK(entry->skeleton.opcode != 0x0F ||
         (entry->skeleton.extra_opcode1 != 0x38 && entry->skeleton.extra_opcode1 != 0x3A));
  DCHECK(!RegStorage::IsFloat(reg));
  DCHECK_LT(RegStorage::RegNum(reg), 8);
  code_buffer_.back() += RegStorage::RegNum(reg);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  DCHECK_EQ(0, entry->skeleton.immediate_bytes);
}

void X86Mir2Lir::EmitOpReg(const X86EncodingMap* entry, uint8_t reg) {
  EmitPrefixAndOpcode(entry);
  if (RegStorage::RegNum(reg) >= 4) {
    DCHECK(strchr(entry->name, '8') == NULL) << entry->name << " "
        << static_cast<int>(RegStorage::RegNum(reg))
        << " in " << PrettyMethod(cu_->method_idx, *cu_->dex_file);
  }
  DCHECK_LT(RegStorage::RegNum(reg), 8);
  uint8_t modrm = (3 << 6) | (entry->skeleton.modrm_opcode << 3) | RegStorage::RegNum(reg);
  code_buffer_.push_back(modrm);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  DCHECK_EQ(0, entry->skeleton.immediate_bytes);
}

void X86Mir2Lir::EmitOpMem(const X86EncodingMap* entry, uint8_t base, int disp) {
  EmitPrefix(entry);
  code_buffer_.push_back(entry->skeleton.opcode);
  DCHECK_NE(0x0F, entry->skeleton.opcode);
  DCHECK_EQ(0, entry->skeleton.extra_opcode1);
  DCHECK_EQ(0, entry->skeleton.extra_opcode2);
  EmitModrmDisp(entry->skeleton.modrm_opcode, base, disp);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  DCHECK_EQ(0, entry->skeleton.immediate_bytes);
}

void X86Mir2Lir::EmitOpArray(const X86EncodingMap* entry, uint8_t base, uint8_t index,
                             int scale, int disp) {
  EmitPrefixAndOpcode(entry);
  EmitModrmSibDisp(entry->skeleton.modrm_opcode, base, index, scale, disp);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  DCHECK_EQ(0, entry->skeleton.immediate_bytes);
}

void X86Mir2Lir::EmitMemReg(const X86EncodingMap* entry,
                       uint8_t base, int disp, uint8_t reg) {
  EmitPrefixAndOpcode(entry);
  if (RegStorage::RegNum(reg) >= 4) {
    DCHECK(strchr(entry->name, '8') == NULL ||
           entry->opcode == kX86Movzx8RM || entry->opcode == kX86Movsx8RM)
        << entry->name << " " << static_cast<int>(RegStorage::RegNum(reg))
        << " in " << PrettyMethod(cu_->method_idx, *cu_->dex_file);
  }
  EmitModrmDisp(reg, base, disp);
  DCHECK_EQ(0, entry->skeleton.modrm_opcode);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  DCHECK_EQ(0, entry->skeleton.immediate_bytes);
}

void X86Mir2Lir::EmitRegMem(const X86EncodingMap* entry,
                       uint8_t reg, uint8_t base, int disp) {
  // Opcode will flip operands.
  EmitMemReg(entry, base, disp, reg);
}

void X86Mir2Lir::EmitRegArray(const X86EncodingMap* entry, uint8_t reg, uint8_t base, uint8_t index,
                              int scale, int disp) {
  EmitPrefixAndOpcode(entry);
  EmitModrmSibDisp(reg, base, index, scale, disp);
  DCHECK_EQ(0, entry->skeleton.modrm_opcode);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  DCHECK_EQ(0, entry->skeleton.immediate_bytes);
}

void X86Mir2Lir::EmitArrayReg(const X86EncodingMap* entry, uint8_t base, uint8_t index, int scale,
                              int disp, uint8_t reg) {
  // Opcode will flip operands.
  EmitRegArray(entry, reg, base, index, scale, disp);
}

void X86Mir2Lir::EmitArrayImm(const X86EncodingMap* entry, uint8_t base, uint8_t index, int scale,
                              int disp, int32_t imm) {
  EmitPrefixAndOpcode(entry);
  EmitModrmSibDisp(entry->skeleton.modrm_opcode, base, index, scale, disp);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  EmitImm(entry, imm);
}

void X86Mir2Lir::EmitRegThread(const X86EncodingMap* entry, uint8_t reg, int disp) {
  DCHECK_NE(entry->skeleton.prefix1, 0);
  EmitPrefixAndOpcode(entry);
  if (RegStorage::RegNum(reg) >= 4) {
    DCHECK(strchr(entry->name, '8') == NULL) << entry->name << " "
        << static_cast<int>(RegStorage::RegNum(reg))
        << " in " << PrettyMethod(cu_->method_idx, *cu_->dex_file);
  }
  DCHECK_LT(RegStorage::RegNum(reg), 8);
  uint8_t modrm = (0 << 6) | (RegStorage::RegNum(reg) << 3) | rs_rBP.GetRegNum();
  code_buffer_.push_back(modrm);
  code_buffer_.push_back(disp & 0xFF);
  code_buffer_.push_back((disp >> 8) & 0xFF);
  code_buffer_.push_back((disp >> 16) & 0xFF);
  code_buffer_.push_back((disp >> 24) & 0xFF);
  DCHECK_EQ(0, entry->skeleton.modrm_opcode);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  DCHECK_EQ(0, entry->skeleton.immediate_bytes);
}

void X86Mir2Lir::EmitRegReg(const X86EncodingMap* entry, uint8_t reg1, uint8_t reg2) {
  EmitPrefixAndOpcode(entry);
  DCHECK_LT(RegStorage::RegNum(reg1), 8);
  DCHECK_LT(RegStorage::RegNum(reg2), 8);
  uint8_t modrm = (3 << 6) | (RegStorage::RegNum(reg1) << 3) | RegStorage::RegNum(reg2);
  code_buffer_.push_back(modrm);
  DCHECK_EQ(0, entry->skeleton.modrm_opcode);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  DCHECK_EQ(0, entry->skeleton.immediate_bytes);
}

void X86Mir2Lir::EmitRegRegImm(const X86EncodingMap* entry,
                          uint8_t reg1, uint8_t reg2, int32_t imm) {
  EmitPrefixAndOpcode(entry);
  DCHECK_LT(RegStorage::RegNum(reg1), 8);
  DCHECK_LT(RegStorage::RegNum(reg2), 8);
  uint8_t modrm = (3 << 6) | (RegStorage::RegNum(reg1) << 3) | RegStorage::RegNum(reg2);
  code_buffer_.push_back(modrm);
  DCHECK_EQ(0, entry->skeleton.modrm_opcode);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  EmitImm(entry, imm);
}

void X86Mir2Lir::EmitRegRegImmRev(const X86EncodingMap* entry,
                                  uint8_t reg1, uint8_t reg2, int32_t imm) {
  EmitRegRegImm(entry, reg2, reg1, imm);
}

void X86Mir2Lir::EmitRegMemImm(const X86EncodingMap* entry,
                               uint8_t reg, uint8_t base, int disp, int32_t imm) {
  EmitPrefixAndOpcode(entry);
  DCHECK(!RegStorage::IsFloat(reg));
  DCHECK_LT(RegStorage::RegNum(reg), 8);
  EmitModrmDisp(reg, base, disp);
  DCHECK_EQ(0, entry->skeleton.modrm_opcode);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  EmitImm(entry, imm);
}

void X86Mir2Lir::EmitMemRegImm(const X86EncodingMap* entry,
                               uint8_t base, int disp, uint8_t reg, int32_t imm) {
  EmitRegMemImm(entry, reg, base, disp, imm);
}

void X86Mir2Lir::EmitRegImm(const X86EncodingMap* entry, uint8_t reg, int imm) {
  if (entry->skeleton.prefix1 != 0) {
    code_buffer_.push_back(entry->skeleton.prefix1);
    if (entry->skeleton.prefix2 != 0) {
      code_buffer_.push_back(entry->skeleton.prefix2);
    }
  } else {
    DCHECK_EQ(0, entry->skeleton.prefix2);
  }
  if (RegStorage::RegNum(reg) == rs_rAX.GetRegNum() && entry->skeleton.ax_opcode != 0) {
    code_buffer_.push_back(entry->skeleton.ax_opcode);
  } else {
    EmitOpcode(entry);
    uint8_t modrm = (3 << 6) | (entry->skeleton.modrm_opcode << 3) | RegStorage::RegNum(reg);
    code_buffer_.push_back(modrm);
  }
  EmitImm(entry, imm);
}

void X86Mir2Lir::EmitMemImm(const X86EncodingMap* entry, uint8_t base, int disp, int32_t imm) {
  EmitPrefixAndOpcode(entry);
  EmitModrmDisp(entry->skeleton.modrm_opcode, base, disp);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  EmitImm(entry, imm);
}

void X86Mir2Lir::EmitThreadImm(const X86EncodingMap* entry, int disp, int imm) {
  EmitPrefixAndOpcode(entry);
  uint8_t modrm = (0 << 6) | (entry->skeleton.modrm_opcode << 3) | rs_rBP.GetRegNum();
  code_buffer_.push_back(modrm);
  code_buffer_.push_back(disp & 0xFF);
  code_buffer_.push_back((disp >> 8) & 0xFF);
  code_buffer_.push_back((disp >> 16) & 0xFF);
  code_buffer_.push_back((disp >> 24) & 0xFF);
  EmitImm(entry, imm);
  DCHECK_EQ(entry->skeleton.ax_opcode, 0);
}

void X86Mir2Lir::EmitMovRegImm(const X86EncodingMap* entry, uint8_t reg, int imm) {
  DCHECK_LT(RegStorage::RegNum(reg), 8);
  code_buffer_.push_back(0xB8 + RegStorage::RegNum(reg));
  code_buffer_.push_back(imm & 0xFF);
  code_buffer_.push_back((imm >> 8) & 0xFF);
  code_buffer_.push_back((imm >> 16) & 0xFF);
  code_buffer_.push_back((imm >> 24) & 0xFF);
}

void X86Mir2Lir::EmitShiftRegImm(const X86EncodingMap* entry, uint8_t reg, int imm) {
  EmitPrefix(entry);
  if (imm != 1) {
    code_buffer_.push_back(entry->skeleton.opcode);
  } else {
    // Shorter encoding for 1 bit shift
    code_buffer_.push_back(entry->skeleton.ax_opcode);
  }
  DCHECK_NE(0x0F, entry->skeleton.opcode);
  DCHECK_EQ(0, entry->skeleton.extra_opcode1);
  DCHECK_EQ(0, entry->skeleton.extra_opcode2);
  if (RegStorage::RegNum(reg) >= 4) {
    DCHECK(strchr(entry->name, '8') == NULL) << entry->name << " "
        << static_cast<int>(RegStorage::RegNum(reg))
        << " in " << PrettyMethod(cu_->method_idx, *cu_->dex_file);
  }
  DCHECK_LT(RegStorage::RegNum(reg), 8);
  uint8_t modrm = (3 << 6) | (entry->skeleton.modrm_opcode << 3) | RegStorage::RegNum(reg);
  code_buffer_.push_back(modrm);
  if (imm != 1) {
    DCHECK_EQ(entry->skeleton.immediate_bytes, 1);
    DCHECK(IS_SIMM8(imm));
    code_buffer_.push_back(imm & 0xFF);
  }
}

void X86Mir2Lir::EmitShiftRegCl(const X86EncodingMap* entry, uint8_t reg, uint8_t cl) {
  DCHECK_EQ(cl, static_cast<uint8_t>(rs_rCX.GetReg()));
  EmitPrefix(entry);
  code_buffer_.push_back(entry->skeleton.opcode);
  DCHECK_NE(0x0F, entry->skeleton.opcode);
  DCHECK_EQ(0, entry->skeleton.extra_opcode1);
  DCHECK_EQ(0, entry->skeleton.extra_opcode2);
  DCHECK_LT(RegStorage::RegNum(reg), 8);
  uint8_t modrm = (3 << 6) | (entry->skeleton.modrm_opcode << 3) | RegStorage::RegNum(reg);
  code_buffer_.push_back(modrm);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  DCHECK_EQ(0, entry->skeleton.immediate_bytes);
}

void X86Mir2Lir::EmitShiftMemCl(const X86EncodingMap* entry, uint8_t base,
                                int displacement, uint8_t cl) {
  DCHECK_EQ(cl, static_cast<uint8_t>(rs_rCX.GetReg()));
  EmitPrefix(entry);
  code_buffer_.push_back(entry->skeleton.opcode);
  DCHECK_NE(0x0F, entry->skeleton.opcode);
  DCHECK_EQ(0, entry->skeleton.extra_opcode1);
  DCHECK_EQ(0, entry->skeleton.extra_opcode2);
  DCHECK_LT(RegStorage::RegNum(base), 8);
  EmitModrmDisp(entry->skeleton.modrm_opcode, base, displacement);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  DCHECK_EQ(0, entry->skeleton.immediate_bytes);
}

void X86Mir2Lir::EmitShiftMemImm(const X86EncodingMap* entry, uint8_t base,
                                int displacement, int imm) {
  EmitPrefix(entry);
  if (imm != 1) {
    code_buffer_.push_back(entry->skeleton.opcode);
  } else {
    // Shorter encoding for 1 bit shift
    code_buffer_.push_back(entry->skeleton.ax_opcode);
  }
  DCHECK_NE(0x0F, entry->skeleton.opcode);
  DCHECK_EQ(0, entry->skeleton.extra_opcode1);
  DCHECK_EQ(0, entry->skeleton.extra_opcode2);
  EmitModrmDisp(entry->skeleton.modrm_opcode, base, displacement);
  if (imm != 1) {
    DCHECK_EQ(entry->skeleton.immediate_bytes, 1);
    DCHECK(IS_SIMM8(imm));
    code_buffer_.push_back(imm & 0xFF);
  }
}

void X86Mir2Lir::EmitRegCond(const X86EncodingMap* entry, uint8_t reg, uint8_t condition) {
  if (entry->skeleton.prefix1 != 0) {
    code_buffer_.push_back(entry->skeleton.prefix1);
    if (entry->skeleton.prefix2 != 0) {
      code_buffer_.push_back(entry->skeleton.prefix2);
    }
  } else {
    DCHECK_EQ(0, entry->skeleton.prefix2);
  }
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  DCHECK_EQ(0x0F, entry->skeleton.opcode);
  code_buffer_.push_back(0x0F);
  DCHECK_EQ(0x90, entry->skeleton.extra_opcode1);
  code_buffer_.push_back(0x90 | condition);
  DCHECK_EQ(0, entry->skeleton.extra_opcode2);
  DCHECK_LT(RegStorage::RegNum(reg), 8);
  uint8_t modrm = (3 << 6) | (entry->skeleton.modrm_opcode << 3) | RegStorage::RegNum(reg);
  code_buffer_.push_back(modrm);
  DCHECK_EQ(entry->skeleton.immediate_bytes, 0);
}

void X86Mir2Lir::EmitMemCond(const X86EncodingMap* entry, uint8_t base, int displacement, uint8_t condition) {
  if (entry->skeleton.prefix1 != 0) {
    code_buffer_.push_back(entry->skeleton.prefix1);
    if (entry->skeleton.prefix2 != 0) {
      code_buffer_.push_back(entry->skeleton.prefix2);
    }
  } else {
    DCHECK_EQ(0, entry->skeleton.prefix2);
  }
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  DCHECK_EQ(0x0F, entry->skeleton.opcode);
  code_buffer_.push_back(0x0F);
  DCHECK_EQ(0x90, entry->skeleton.extra_opcode1);
  code_buffer_.push_back(0x90 | condition);
  DCHECK_EQ(0, entry->skeleton.extra_opcode2);
  EmitModrmDisp(entry->skeleton.modrm_opcode, base, displacement);
  DCHECK_EQ(entry->skeleton.immediate_bytes, 0);
}

void X86Mir2Lir::EmitRegRegCond(const X86EncodingMap* entry, uint8_t reg1, uint8_t reg2,
                                uint8_t condition) {
  // Generate prefix and opcode without the condition
  EmitPrefixAndOpcode(entry);

  // Now add the condition. The last byte of opcode is the one that receives it.
  DCHECK_LE(condition, 0xF);
  code_buffer_.back() += condition;

  // Not expecting to have to encode immediate or do anything special for ModR/M since there are two registers.
  DCHECK_EQ(0, entry->skeleton.immediate_bytes);
  DCHECK_EQ(0, entry->skeleton.modrm_opcode);

  // Check that registers requested for encoding are sane.
  DCHECK_LT(RegStorage::RegNum(reg1), 8);
  DCHECK_LT(RegStorage::RegNum(reg2), 8);

  // For register to register encoding, the mod is 3.
  const uint8_t mod = (3 << 6);

  // Encode the ModR/M byte now.
  const uint8_t modrm = mod | (RegStorage::RegNum(reg1) << 3) | RegStorage::RegNum(reg2);
  code_buffer_.push_back(modrm);
}

void X86Mir2Lir::EmitRegMemCond(const X86EncodingMap* entry, uint8_t reg1, uint8_t base, int displacement, uint8_t condition) {
  // Generate prefix and opcode without the condition
  EmitPrefixAndOpcode(entry);

  // Now add the condition. The last byte of opcode is the one that receives it.
  DCHECK_LE(condition, 0xF);
  code_buffer_.back() += condition;

  DCHECK_EQ(0, entry->skeleton.immediate_bytes);
  DCHECK_EQ(0, entry->skeleton.modrm_opcode);

  // Check that registers requested for encoding are sane.
  DCHECK_LT(reg1, 8);
  DCHECK_LT(base, 8);

  EmitModrmDisp(reg1, base, displacement);
}

void X86Mir2Lir::EmitJmp(const X86EncodingMap* entry, int rel) {
  if (entry->opcode == kX86Jmp8) {
    DCHECK(IS_SIMM8(rel));
    code_buffer_.push_back(0xEB);
    code_buffer_.push_back(rel & 0xFF);
  } else if (entry->opcode == kX86Jmp32) {
    code_buffer_.push_back(0xE9);
    code_buffer_.push_back(rel & 0xFF);
    code_buffer_.push_back((rel >> 8) & 0xFF);
    code_buffer_.push_back((rel >> 16) & 0xFF);
    code_buffer_.push_back((rel >> 24) & 0xFF);
  } else if (entry->opcode == kX86Jecxz8) {
    DCHECK(IS_SIMM8(rel));
    code_buffer_.push_back(0xE3);
    code_buffer_.push_back(rel & 0xFF);
  } else {
    DCHECK(entry->opcode == kX86JmpR);
    code_buffer_.push_back(entry->skeleton.opcode);
    uint8_t reg = static_cast<uint8_t>(rel);
    DCHECK_LT(RegStorage::RegNum(reg), 8);
    uint8_t modrm = (3 << 6) | (entry->skeleton.modrm_opcode << 3) | RegStorage::RegNum(reg);
    code_buffer_.push_back(modrm);
  }
}

void X86Mir2Lir::EmitJcc(const X86EncodingMap* entry, int rel, uint8_t cc) {
  DCHECK_LT(cc, 16);
  if (entry->opcode == kX86Jcc8) {
    DCHECK(IS_SIMM8(rel));
    code_buffer_.push_back(0x70 | cc);
    code_buffer_.push_back(rel & 0xFF);
  } else {
    DCHECK(entry->opcode == kX86Jcc32);
    code_buffer_.push_back(0x0F);
    code_buffer_.push_back(0x80 | cc);
    code_buffer_.push_back(rel & 0xFF);
    code_buffer_.push_back((rel >> 8) & 0xFF);
    code_buffer_.push_back((rel >> 16) & 0xFF);
    code_buffer_.push_back((rel >> 24) & 0xFF);
  }
}

void X86Mir2Lir::EmitCallMem(const X86EncodingMap* entry, uint8_t base, int disp) {
  EmitPrefixAndOpcode(entry);
  EmitModrmDisp(entry->skeleton.modrm_opcode, base, disp);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  DCHECK_EQ(0, entry->skeleton.immediate_bytes);
}

void X86Mir2Lir::EmitCallImmediate(const X86EncodingMap* entry, int disp) {
  EmitPrefixAndOpcode(entry);
  DCHECK_EQ(4, entry->skeleton.immediate_bytes);
  code_buffer_.push_back(disp & 0xFF);
  code_buffer_.push_back((disp >> 8) & 0xFF);
  code_buffer_.push_back((disp >> 16) & 0xFF);
  code_buffer_.push_back((disp >> 24) & 0xFF);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
}

void X86Mir2Lir::EmitCallThread(const X86EncodingMap* entry, int disp) {
  DCHECK_NE(entry->skeleton.prefix1, 0);
  EmitPrefixAndOpcode(entry);
  uint8_t modrm = (0 << 6) | (entry->skeleton.modrm_opcode << 3) | rs_rBP.GetRegNum();
  code_buffer_.push_back(modrm);
  code_buffer_.push_back(disp & 0xFF);
  code_buffer_.push_back((disp >> 8) & 0xFF);
  code_buffer_.push_back((disp >> 16) & 0xFF);
  code_buffer_.push_back((disp >> 24) & 0xFF);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  DCHECK_EQ(0, entry->skeleton.immediate_bytes);
}

void X86Mir2Lir::EmitPcRel(const X86EncodingMap* entry, uint8_t reg,
                      int base_or_table, uint8_t index, int scale, int table_or_disp) {
  int disp;
  if (entry->opcode == kX86PcRelLoadRA) {
    Mir2Lir::EmbeddedData *tab_rec =
        reinterpret_cast<Mir2Lir::EmbeddedData*>(UnwrapPointer(table_or_disp));
    disp = tab_rec->offset;
  } else {
    DCHECK(entry->opcode == kX86PcRelAdr);
    Mir2Lir::EmbeddedData *tab_rec =
        reinterpret_cast<Mir2Lir::EmbeddedData*>(UnwrapPointer(base_or_table));
    disp = tab_rec->offset;
  }
  EmitPrefix(entry);
  DCHECK_LT(RegStorage::RegNum(reg), 8);
  if (entry->opcode == kX86PcRelLoadRA) {
    code_buffer_.push_back(entry->skeleton.opcode);
    DCHECK_NE(0x0F, entry->skeleton.opcode);
    DCHECK_EQ(0, entry->skeleton.extra_opcode1);
    DCHECK_EQ(0, entry->skeleton.extra_opcode2);
    uint8_t modrm = (2 << 6) | (RegStorage::RegNum(reg) << 3) | rs_rX86_SP.GetRegNum();
    code_buffer_.push_back(modrm);
    DCHECK_LT(scale, 4);
    DCHECK_LT(RegStorage::RegNum(index), 8);
    DCHECK_LT(RegStorage::RegNum(base_or_table), 8);
    uint8_t base = static_cast<uint8_t>(base_or_table);
    uint8_t sib = (scale << 6) | (RegStorage::RegNum(index) << 3) | RegStorage::RegNum(base);
    code_buffer_.push_back(sib);
    DCHECK_EQ(0, entry->skeleton.immediate_bytes);
  } else {
    code_buffer_.push_back(entry->skeleton.opcode + RegStorage::RegNum(reg));
  }
  code_buffer_.push_back(disp & 0xFF);
  code_buffer_.push_back((disp >> 8) & 0xFF);
  code_buffer_.push_back((disp >> 16) & 0xFF);
  code_buffer_.push_back((disp >> 24) & 0xFF);
  DCHECK_EQ(0, entry->skeleton.modrm_opcode);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
}

void X86Mir2Lir::EmitMacro(const X86EncodingMap* entry, uint8_t reg, int offset) {
  DCHECK(entry->opcode == kX86StartOfMethod) << entry->name;
  code_buffer_.push_back(0xE8);  // call +0
  code_buffer_.push_back(0);
  code_buffer_.push_back(0);
  code_buffer_.push_back(0);
  code_buffer_.push_back(0);

  DCHECK_LT(RegStorage::RegNum(reg), 8);
  code_buffer_.push_back(0x58 + RegStorage::RegNum(reg));  // pop reg

  EmitRegImm(&X86Mir2Lir::EncodingMap[kX86Sub32RI], RegStorage::RegNum(reg),
             offset + 5 /* size of call +0 */);
}

void X86Mir2Lir::EmitUnimplemented(const X86EncodingMap* entry, LIR* lir) {
  UNIMPLEMENTED(WARNING) << "encoding kind for " << entry->name << " "
                         << BuildInsnString(entry->fmt, lir, 0);
  for (int i = 0; i < GetInsnSize(lir); ++i) {
    code_buffer_.push_back(0xCC);  // push breakpoint instruction - int 3
  }
}

/*
 * Assemble the LIR into binary instruction format.  Note that we may
 * discover that pc-relative displacements may not fit the selected
 * instruction.  In those cases we will try to substitute a new code
 * sequence or request that the trace be shortened and retried.
 */
AssemblerStatus X86Mir2Lir::AssembleInstructions(CodeOffset start_addr) {
  LIR *lir;
  AssemblerStatus res = kSuccess;  // Assume success

  const bool kVerbosePcFixup = false;
  for (lir = first_lir_insn_; lir != NULL; lir = NEXT_LIR(lir)) {
    if (IsPseudoLirOp(lir->opcode)) {
      continue;
    }

    if (lir->flags.is_nop) {
      continue;
    }

    if (lir->flags.fixup != kFixupNone) {
      switch (lir->opcode) {
        case kX86Jcc8: {
          LIR *target_lir = lir->target;
          DCHECK(target_lir != NULL);
          int delta = 0;
          CodeOffset pc;
          if (IS_SIMM8(lir->operands[0])) {
            pc = lir->offset + 2 /* opcode + rel8 */;
          } else {
            pc = lir->offset + 6 /* 2 byte opcode + rel32 */;
          }
          CodeOffset target = target_lir->offset;
          delta = target - pc;
          if (IS_SIMM8(delta) != IS_SIMM8(lir->operands[0])) {
            if (kVerbosePcFixup) {
              LOG(INFO) << "Retry for JCC growth at " << lir->offset
                  << " delta: " << delta << " old delta: " << lir->operands[0];
            }
            lir->opcode = kX86Jcc32;
            SetupResourceMasks(lir);
            res = kRetryAll;
          }
          if (kVerbosePcFixup) {
            LOG(INFO) << "Source:";
            DumpLIRInsn(lir, 0);
            LOG(INFO) << "Target:";
            DumpLIRInsn(target_lir, 0);
            LOG(INFO) << "Delta " << delta;
          }
          lir->operands[0] = delta;
          break;
        }
        case kX86Jcc32: {
          LIR *target_lir = lir->target;
          DCHECK(target_lir != NULL);
          CodeOffset pc = lir->offset + 6 /* 2 byte opcode + rel32 */;
          CodeOffset target = target_lir->offset;
          int delta = target - pc;
          if (kVerbosePcFixup) {
            LOG(INFO) << "Source:";
            DumpLIRInsn(lir, 0);
            LOG(INFO) << "Target:";
            DumpLIRInsn(target_lir, 0);
            LOG(INFO) << "Delta " << delta;
          }
          lir->operands[0] = delta;
          break;
        }
        case kX86Jecxz8: {
          LIR *target_lir = lir->target;
          DCHECK(target_lir != NULL);
          CodeOffset pc;
          pc = lir->offset + 2;  // opcode + rel8
          CodeOffset target = target_lir->offset;
          int delta = target - pc;
          lir->operands[0] = delta;
          DCHECK(IS_SIMM8(delta));
          break;
        }
        case kX86Jmp8: {
          LIR *target_lir = lir->target;
          DCHECK(target_lir != NULL);
          int delta = 0;
          CodeOffset pc;
          if (IS_SIMM8(lir->operands[0])) {
            pc = lir->offset + 2 /* opcode + rel8 */;
          } else {
            pc = lir->offset + 5 /* opcode + rel32 */;
          }
          CodeOffset target = target_lir->offset;
          delta = target - pc;
          if (!(cu_->disable_opt & (1 << kSafeOptimizations)) && delta == 0) {
            // Useless branch
            NopLIR(lir);
            if (kVerbosePcFixup) {
              LOG(INFO) << "Retry for useless branch at " << lir->offset;
            }
            res = kRetryAll;
          } else if (IS_SIMM8(delta) != IS_SIMM8(lir->operands[0])) {
            if (kVerbosePcFixup) {
              LOG(INFO) << "Retry for JMP growth at " << lir->offset;
            }
            lir->opcode = kX86Jmp32;
            SetupResourceMasks(lir);
            res = kRetryAll;
          }
          lir->operands[0] = delta;
          break;
        }
        case kX86Jmp32: {
          LIR *target_lir = lir->target;
          DCHECK(target_lir != NULL);
          CodeOffset pc = lir->offset + 5 /* opcode + rel32 */;
          CodeOffset target = target_lir->offset;
          int delta = target - pc;
          lir->operands[0] = delta;
          break;
        }
        default:
          if (lir->flags.fixup == kFixupLoad) {
            LIR *target_lir = lir->target;
            DCHECK(target_lir != NULL);
            CodeOffset target = target_lir->offset;
            lir->operands[2] = target;
            int newSize = GetInsnSize(lir);
            if (newSize != lir->flags.size) {
              lir->flags.size = newSize;
              res = kRetryAll;
            }
          }
          break;
      }
    }

    /*
     * If one of the pc-relative instructions expanded we'll have
     * to make another pass.  Don't bother to fully assemble the
     * instruction.
     */
    if (res != kSuccess) {
      continue;
    }
    CHECK_EQ(static_cast<size_t>(lir->offset), code_buffer_.size());
    const X86EncodingMap *entry = &X86Mir2Lir::EncodingMap[lir->opcode];
    size_t starting_cbuf_size = code_buffer_.size();
    switch (entry->kind) {
      case kData:  // 4 bytes of data
        code_buffer_.push_back(lir->operands[0]);
        break;
      case kNullary:  // 1 byte of opcode
        DCHECK_EQ(0, entry->skeleton.prefix1);
        DCHECK_EQ(0, entry->skeleton.prefix2);
        EmitOpcode(entry);
        DCHECK_EQ(0, entry->skeleton.modrm_opcode);
        DCHECK_EQ(0, entry->skeleton.ax_opcode);
        DCHECK_EQ(0, entry->skeleton.immediate_bytes);
        break;
      case kPrefix2Nullary:  // 1 byte of opcode + 2 prefixes.
        DCHECK_NE(0, entry->skeleton.prefix1);
        DCHECK_NE(0, entry->skeleton.prefix2);
        EmitPrefixAndOpcode(entry);
        DCHECK_EQ(0, entry->skeleton.modrm_opcode);
        DCHECK_EQ(0, entry->skeleton.ax_opcode);
        DCHECK_EQ(0, entry->skeleton.immediate_bytes);
        break;
      case kRegOpcode:  // lir operands - 0: reg
        EmitOpRegOpcode(entry, lir->operands[0]);
        break;
      case kReg:  // lir operands - 0: reg
        EmitOpReg(entry, lir->operands[0]);
        break;
      case kMem:  // lir operands - 0: base, 1: disp
        EmitOpMem(entry, lir->operands[0], lir->operands[1]);
        break;
      case kArray:  // lir operands - 0: base, 1: index, 2: scale, 3: disp
        EmitOpArray(entry, lir->operands[0], lir->operands[1], lir->operands[2], lir->operands[3]);
        break;
      case kMemReg:  // lir operands - 0: base, 1: disp, 2: reg
        EmitMemReg(entry, lir->operands[0], lir->operands[1], lir->operands[2]);
        break;
      case kMemImm:  // lir operands - 0: base, 1: disp, 2: immediate
        EmitMemImm(entry, lir->operands[0], lir->operands[1], lir->operands[2]);
        break;
      case kArrayImm:  // lir operands - 0: base, 1: index, 2: disp, 3:scale, 4:immediate
        EmitArrayImm(entry, lir->operands[0], lir->operands[1], lir->operands[2],
                     lir->operands[3], lir->operands[4]);
        break;
      case kArrayReg:  // lir operands - 0: base, 1: index, 2: scale, 3: disp, 4: reg
        EmitArrayReg(entry, lir->operands[0], lir->operands[1], lir->operands[2],
                     lir->operands[3], lir->operands[4]);
        break;
      case kRegMem:  // lir operands - 0: reg, 1: base, 2: disp
        EmitRegMem(entry, lir->operands[0], lir->operands[1], lir->operands[2]);
        break;
      case kRegArray:  // lir operands - 0: reg, 1: base, 2: index, 3: scale, 4: disp
        EmitRegArray(entry, lir->operands[0], lir->operands[1], lir->operands[2],
                     lir->operands[3], lir->operands[4]);
        break;
      case kRegThread:  // lir operands - 0: reg, 1: disp
        EmitRegThread(entry, lir->operands[0], lir->operands[1]);
        break;
      case kRegReg:  // lir operands - 0: reg1, 1: reg2
        EmitRegReg(entry, lir->operands[0], lir->operands[1]);
        break;
      case kRegRegStore:  // lir operands - 0: reg2, 1: reg1
        EmitRegReg(entry, lir->operands[1], lir->operands[0]);
        break;
      case kRegRegImmRev:
        EmitRegRegImmRev(entry, lir->operands[0], lir->operands[1], lir->operands[2]);
        break;
      case kMemRegImm:
        EmitMemRegImm(entry, lir->operands[0], lir->operands[1], lir->operands[2],
                      lir->operands[3]);
        break;
      case kRegRegImm:
        EmitRegRegImm(entry, lir->operands[0], lir->operands[1], lir->operands[2]);
        break;
      case kRegMemImm:
        EmitRegMemImm(entry, lir->operands[0], lir->operands[1], lir->operands[2],
                      lir->operands[3]);
        break;
      case kRegImm:  // lir operands - 0: reg, 1: immediate
        EmitRegImm(entry, lir->operands[0], lir->operands[1]);
        break;
      case kThreadImm:  // lir operands - 0: disp, 1: immediate
        EmitThreadImm(entry, lir->operands[0], lir->operands[1]);
        break;
      case kMovRegImm:  // lir operands - 0: reg, 1: immediate
        EmitMovRegImm(entry, lir->operands[0], lir->operands[1]);
        break;
      case kShiftRegImm:  // lir operands - 0: reg, 1: immediate
        EmitShiftRegImm(entry, lir->operands[0], lir->operands[1]);
        break;
      case kShiftMemImm:  // lir operands - 0: base, 1: disp, 2:immediate
        EmitShiftMemImm(entry, lir->operands[0], lir->operands[1], lir->operands[2]);
        break;
      case kShiftRegCl:  // lir operands - 0: reg, 1: cl
        EmitShiftRegCl(entry, lir->operands[0], lir->operands[1]);
        break;
      case kShiftMemCl:  // lir operands - 0: base, 1:displacement, 2: cl
        EmitShiftMemCl(entry, lir->operands[0], lir->operands[1], lir->operands[2]);
        break;
      case kRegCond:  // lir operands - 0: reg, 1: condition
        EmitRegCond(entry, lir->operands[0], lir->operands[1]);
        break;
      case kMemCond:  // lir operands - 0: base, 1: displacement, 2: condition
        EmitMemCond(entry, lir->operands[0], lir->operands[1], lir->operands[2]);
        break;
      case kRegRegCond:  // lir operands - 0: reg, 1: reg, 2: condition
        EmitRegRegCond(entry, lir->operands[0], lir->operands[1], lir->operands[2]);
        break;
      case kRegMemCond:  // lir operands - 0: reg, 1: reg, displacement, 3: condition
        EmitRegMemCond(entry, lir->operands[0], lir->operands[1], lir->operands[2], lir->operands[3]);
        break;
      case kJmp:  // lir operands - 0: rel
        if (entry->opcode == kX86JmpT) {
          // This works since the instruction format for jmp and call is basically the same and
          // EmitCallThread loads opcode info.
          EmitCallThread(entry, lir->operands[0]);
        } else {
          EmitJmp(entry, lir->operands[0]);
        }
        break;
      case kJcc:  // lir operands - 0: rel, 1: CC, target assigned
        EmitJcc(entry, lir->operands[0], lir->operands[1]);
        break;
      case kCall:
        switch (entry->opcode) {
          case kX86CallI:  // lir operands - 0: disp
            EmitCallImmediate(entry, lir->operands[0]);
            break;
          case kX86CallM:  // lir operands - 0: base, 1: disp
            EmitCallMem(entry, lir->operands[0], lir->operands[1]);
            break;
          case kX86CallT:  // lir operands - 0: disp
            EmitCallThread(entry, lir->operands[0]);
            break;
          default:
            EmitUnimplemented(entry, lir);
            break;
        }
        break;
      case kPcRel:  // lir operands - 0: reg, 1: base, 2: index, 3: scale, 4: table
        EmitPcRel(entry, lir->operands[0], lir->operands[1], lir->operands[2],
                  lir->operands[3], lir->operands[4]);
        break;
      case kMacro:
        EmitMacro(entry, lir->operands[0], lir->offset);
        break;
      default:
        EmitUnimplemented(entry, lir);
        break;
    }
    CHECK_EQ(static_cast<size_t>(GetInsnSize(lir)),
             code_buffer_.size() - starting_cbuf_size)
        << "Instruction size mismatch for entry: " << X86Mir2Lir::EncodingMap[lir->opcode].name;
  }
  return res;
}

// LIR offset assignment.
// TODO: consolidate w/ Arm assembly mechanism.
int X86Mir2Lir::AssignInsnOffsets() {
  LIR* lir;
  int offset = 0;

  for (lir = first_lir_insn_; lir != NULL; lir = NEXT_LIR(lir)) {
    lir->offset = offset;
    if (LIKELY(!IsPseudoLirOp(lir->opcode))) {
      if (!lir->flags.is_nop) {
        offset += lir->flags.size;
      }
    } else if (UNLIKELY(lir->opcode == kPseudoPseudoAlign4)) {
      if (offset & 0x2) {
        offset += 2;
        lir->operands[0] = 1;
      } else {
        lir->operands[0] = 0;
      }
    }
    /* Pseudo opcodes don't consume space */
  }
  return offset;
}

/*
 * Walk the compilation unit and assign offsets to instructions
 * and literals and compute the total size of the compiled unit.
 * TODO: consolidate w/ Arm assembly mechanism.
 */
void X86Mir2Lir::AssignOffsets() {
  int offset = AssignInsnOffsets();

  /* Const values have to be word aligned */
  offset = RoundUp(offset, 4);

  /* Set up offsets for literals */
  data_offset_ = offset;

  offset = AssignLiteralOffset(offset);

  offset = AssignSwitchTablesOffset(offset);

  offset = AssignFillArrayDataOffset(offset);

  total_size_ = offset;
}

/*
 * Go over each instruction in the list and calculate the offset from the top
 * before sending them off to the assembler. If out-of-range branch distance is
 * seen rearrange the instructions a bit to correct it.
 * TODO: consolidate w/ Arm assembly mechanism.
 */
void X86Mir2Lir::AssembleLIR() {
  cu_->NewTimingSplit("Assemble");

  // We will remove the method address if we never ended up using it
  if (store_method_addr_ && !store_method_addr_used_) {
    setup_method_address_[0]->flags.is_nop = true;
    setup_method_address_[1]->flags.is_nop = true;
  }

  AssignOffsets();
  int assembler_retries = 0;
  /*
   * Assemble here.  Note that we generate code with optimistic assumptions
   * and if found now to work, we'll have to redo the sequence and retry.
   */

  while (true) {
    AssemblerStatus res = AssembleInstructions(0);
    if (res == kSuccess) {
      break;
    } else {
      assembler_retries++;
      if (assembler_retries > MAX_ASSEMBLER_RETRIES) {
        CodegenDump();
        LOG(FATAL) << "Assembler error - too many retries";
      }
      // Redo offsets and try again
      AssignOffsets();
      code_buffer_.clear();
    }
  }

  // Install literals
  InstallLiteralPools();

  // Install switch tables
  InstallSwitchTables();

  // Install fill array data
  InstallFillArrayData();

  // Create the mapping table and native offset to reference map.
  cu_->NewTimingSplit("PcMappingTable");
  CreateMappingTables();

  cu_->NewTimingSplit("GcMap");
  CreateNativeGcMap();
}

}  // namespace art
