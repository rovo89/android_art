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

#include "../../Dalvik.h"
#include "../../CompilerInternals.h"
#include "X86LIR.h"
#include "Codegen.h"
#include <sys/mman.h>           /* for protection change */

namespace art {

#define MAX_ASSEMBLER_RETRIES 50

X86EncodingMap EncodingMap[kX86Last] = {
  { kX8632BitData, kData,    IS_UNARY_OP,            { 0, 0, 0x00, 0, 0, 0, 0, 4 }, "data",  "0x!0d" },
  { kX86Bkpt,      kNullary, NO_OPERAND | IS_BRANCH, { 0, 0, 0xCC, 0, 0, 0, 0, 4 }, "int 3", "" },
  { kX86Nop,       kNop,     IS_UNARY_OP,            { 0, 0, 0x90, 0, 0, 0, 0, 0 }, "nop",   "" },

#define ENCODING_MAP(opname, is_store, \
                     rm8_r8, rm32_r32, \
                     r8_rm8, r32_rm32, \
                     ax8_i8, ax32_i32, \
                     rm8_i8, rm8_i8_modrm, \
                     rm32_i32, rm32_i32_modrm, \
                     rm32_i8, rm32_i8_modrm) \
{ kX86 ## opname ## 8MR, kMemReg,   is_store | IS_TERTIARY_OP | SETS_CCODES, { 0, 0,             rm8_r8, 0, 0, 0,            0,      0 }, #opname "8MR", "[!0r+!1d],!2r" }, \
{ kX86 ## opname ## 8AR, kArrayReg, is_store | IS_QUIN_OP     | SETS_CCODES, { 0, 0,             rm8_r8, 0, 0, 0,            0,      0 }, #opname "8AR", "[!0r+!1r<<!2d+!3d],!4r" }, \
{ kX86 ## opname ## 8TR, kThreadReg,is_store | IS_BINARY_OP   | SETS_CCODES, { THREAD_PREFIX, 0, rm8_r8, 0, 0, 0,            0,      0 }, #opname "8TR", "fs:[!0d],!1r" }, \
{ kX86 ## opname ## 8RR, kRegReg,              IS_BINARY_OP   | SETS_CCODES, { 0, 0,             r8_rm8, 0, 0, 0,            0,      0 }, #opname "8RR", "!0r,!1r" }, \
{ kX86 ## opname ## 8RM, kRegMem,    IS_LOAD | IS_TERTIARY_OP | SETS_CCODES, { 0, 0,             r8_rm8, 0, 0, 0,            0,      0 }, #opname "8RM", "!0r,[!1r+!2d]" }, \
{ kX86 ## opname ## 8RA, kRegArray,  IS_LOAD | IS_QUIN_OP     | SETS_CCODES, { 0, 0,             r8_rm8, 0, 0, 0,            0,      0 }, #opname "8RA", "!0r,[!1r+!2r<<!3d+!4d]" }, \
{ kX86 ## opname ## 8RT, kRegThread, IS_LOAD | IS_BINARY_OP   | SETS_CCODES, { THREAD_PREFIX, 0, r8_rm8, 0, 0, 0,            0,      0 }, #opname "8RT", "!0r,fs:[!1d]" }, \
{ kX86 ## opname ## 8RI, kRegImm,              IS_BINARY_OP   | SETS_CCODES, { 0, 0,             rm8_i8, 0, 0, rm8_i8_modrm, ax8_i8, 1 }, #opname "8RI", "!0r,!1d" }, \
{ kX86 ## opname ## 8MI, kMemImm,   is_store | IS_TERTIARY_OP | SETS_CCODES, { 0, 0,             rm8_i8, 0, 0, rm8_i8_modrm, 0,      1 }, #opname "8MI", "[!0r+!1d],!2r" }, \
{ kX86 ## opname ## 8AI, kArrayImm, is_store | IS_QUIN_OP     | SETS_CCODES, { 0, 0,             rm8_i8, 0, 0, rm8_i8_modrm, 0,      1 }, #opname "8AI", "[!0r+!1r<<!2d+!3d],!4r" }, \
{ kX86 ## opname ## 8TI, kThreadImm,is_store | IS_BINARY_OP   | SETS_CCODES, { THREAD_PREFIX, 0, rm8_i8, 0, 0, rm8_i8_modrm, 0,      1 }, #opname "8TI", "fs:[!0d],!1r" }, \
  \
{ kX86 ## opname ## 16MR,  kMemReg,   is_store | IS_TERTIARY_OP | SETS_CCODES, { 0x66, 0,             rm32_r32, 0, 0, 0,              0,        0 }, #opname "16MR", "[!0r+!1d],!2r" }, \
{ kX86 ## opname ## 16AR,  kArrayReg, is_store | IS_QUIN_OP     | SETS_CCODES, { 0x66, 0,             rm32_r32, 0, 0, 0,              0,        0 }, #opname "16AR", "[!0r+!1r<<!2d+!3d],!4r" }, \
{ kX86 ## opname ## 16TR,  kThreadReg,is_store | IS_BINARY_OP   | SETS_CCODES, { THREAD_PREFIX, 0x66, rm32_r32, 0, 0, 0,              0,        0 }, #opname "16TR", "fs:[!0d],!1r" }, \
{ kX86 ## opname ## 16RR,  kRegReg,              IS_BINARY_OP   | SETS_CCODES, { 0x66, 0,             r32_rm32, 0, 0, 0,              0,        0 }, #opname "16RR", "!0r,!1r" }, \
{ kX86 ## opname ## 16RM,  kRegMem,    IS_LOAD | IS_TERTIARY_OP | SETS_CCODES, { 0x66, 0,             r32_rm32, 0, 0, 0,              0,        0 }, #opname "16RM", "!0r,[!1r+!2d]" }, \
{ kX86 ## opname ## 16RA,  kRegArray,  IS_LOAD | IS_QUIN_OP     | SETS_CCODES, { 0x66, 0,             r32_rm32, 0, 0, 0,              0,        0 }, #opname "16RA", "!0r,[!1r+!2r<<!3d+!4d]" }, \
{ kX86 ## opname ## 16RT,  kRegThread, IS_LOAD | IS_BINARY_OP   | SETS_CCODES, { THREAD_PREFIX, 0x66, r32_rm32, 0, 0, 0,              0,        0 }, #opname "16RT", "!0r,fs:[!1d]" }, \
{ kX86 ## opname ## 16RI,  kRegImm,              IS_BINARY_OP   | SETS_CCODES, { 0x66, 0,             rm32_i32, 0, 0, rm32_i32_modrm, ax32_i32, 2 }, #opname "16RI", "!0r,!1d" }, \
{ kX86 ## opname ## 16MI,  kMemImm,   is_store | IS_TERTIARY_OP | SETS_CCODES, { 0x66, 0,             rm32_i32, 0, 0, rm32_i32_modrm, 0,        2 }, #opname "16MI", "[!0r+!1d],!2d" }, \
{ kX86 ## opname ## 16AI,  kArrayImm, is_store | IS_QUIN_OP     | SETS_CCODES, { 0x66, 0,             rm32_i32, 0, 0, rm32_i32_modrm, 0,        2 }, #opname "16AI", "[!0r+!1r<<!2d+!3d],!4d" }, \
{ kX86 ## opname ## 16TI,  kThreadImm,is_store | IS_BINARY_OP   | SETS_CCODES, { THREAD_PREFIX, 0x66, rm32_i32, 0, 0, rm32_i32_modrm, 0,        2 }, #opname "16TI", "fs:[!0d],!1d" }, \
{ kX86 ## opname ## 16RI8, kRegImm,              IS_BINARY_OP   | SETS_CCODES, { 0x66, 0,             rm32_i8,  0, 0, rm32_i8_modrm,  0,        1 }, #opname "16RI8", "!0r,!1d" }, \
{ kX86 ## opname ## 16MI8, kMemImm,   is_store | IS_TERTIARY_OP | SETS_CCODES, { 0x66, 0,             rm32_i8,  0, 0, rm32_i8_modrm,  0,        1 }, #opname "16MI8", "[!0r+!1d],!2d" }, \
{ kX86 ## opname ## 16AI8, kArrayImm, is_store | IS_QUIN_OP     | SETS_CCODES, { 0x66, 0,             rm32_i8,  0, 0, rm32_i8_modrm,  0,        1 }, #opname "16AI8", "[!0r+!1r<<!2d+!3d],!4d" }, \
{ kX86 ## opname ## 16TI8, kThreadImm,is_store | IS_BINARY_OP   | SETS_CCODES, { THREAD_PREFIX, 0x66, rm32_i8,  0, 0, rm32_i8_modrm,  0,        1 }, #opname "16TI8", "fs:[!0d],!1d" }, \
  \
{ kX86 ## opname ## 32MR,  kMemReg,   is_store | IS_TERTIARY_OP | SETS_CCODES, { 0, 0,             rm32_r32, 0, 0, 0,              0,        0 }, #opname "32MR", "[!0r+!1d],!2r" }, \
{ kX86 ## opname ## 32AR,  kArrayReg, is_store | IS_QUIN_OP     | SETS_CCODES, { 0, 0,             rm32_r32, 0, 0, 0,              0,        0 }, #opname "32AR", "[!0r+!1r<<!2d+!3d],!4r" }, \
{ kX86 ## opname ## 32TR,  kThreadReg,is_store | IS_BINARY_OP   | SETS_CCODES, { THREAD_PREFIX, 0, rm32_r32, 0, 0, 0,              0,        0 }, #opname "32TR", "fs:[!0d],!1r" }, \
{ kX86 ## opname ## 32RR,  kRegReg,              IS_BINARY_OP   | SETS_CCODES, { 0, 0,             r32_rm32, 0, 0, 0,              0,        0 }, #opname "32RR", "!0r,!1r" }, \
{ kX86 ## opname ## 32RM,  kRegMem,    IS_LOAD | IS_TERTIARY_OP | SETS_CCODES, { 0, 0,             r32_rm32, 0, 0, 0,              0,        0 }, #opname "32RM", "!0r,[!1r+!2d]" }, \
{ kX86 ## opname ## 32RA,  kRegArray,  IS_LOAD | IS_QUIN_OP     | SETS_CCODES, { 0, 0,             r32_rm32, 0, 0, 0,              0,        0 }, #opname "32RA", "!0r,[!1r+!2r<<!3d+!4d]" }, \
{ kX86 ## opname ## 32RT,  kRegThread, IS_LOAD | IS_BINARY_OP   | SETS_CCODES, { THREAD_PREFIX, 0, r32_rm32, 0, 0, 0,              0,        0 }, #opname "32RT", "!0r,fs:[!1d]" }, \
{ kX86 ## opname ## 32RI,  kRegImm,              IS_BINARY_OP   | SETS_CCODES, { 0, 0,             rm32_i32, 0, 0, rm32_i32_modrm, ax32_i32, 4 }, #opname "32RI", "!0r,!1d" }, \
{ kX86 ## opname ## 32MI,  kMemImm,   is_store | IS_TERTIARY_OP | SETS_CCODES, { 0, 0,             rm32_i32, 0, 0, rm32_i32_modrm, 0,        4 }, #opname "32MI", "[!0r+!1d],!2r" }, \
{ kX86 ## opname ## 32AI,  kArrayImm, is_store | IS_QUIN_OP     | SETS_CCODES, { 0, 0,             rm32_i32, 0, 0, rm32_i32_modrm, 0,        4 }, #opname "32AI", "[!0r+!1r<<!2d+!3d],!4d" }, \
{ kX86 ## opname ## 32TI,  kThreadImm,is_store | IS_BINARY_OP   | SETS_CCODES, { THREAD_PREFIX, 0, rm32_i32, 0, 0, rm32_i32_modrm, 0,        4 }, #opname "32TI", "fs:[!0d],!1d" }, \
{ kX86 ## opname ## 32RI8, kRegImm,              IS_BINARY_OP   | SETS_CCODES, { 0, 0,             rm32_i8,  0, 0, rm32_i8_modrm,  0,        1 }, #opname "32RI8", "!0r,!1d" }, \
{ kX86 ## opname ## 32MI8, kMemImm,   is_store | IS_TERTIARY_OP | SETS_CCODES, { 0, 0,             rm32_i8,  0, 0, rm32_i8_modrm,  0,        1 }, #opname "32MI8", "[!0r+!1d],!2d" }, \
{ kX86 ## opname ## 32AI8, kArrayImm, is_store | IS_QUIN_OP     | SETS_CCODES, { 0, 0,             rm32_i8,  0, 0, rm32_i8_modrm,  0,        1 }, #opname "32AI8", "[!0r+!1r<<!2d+!3d],!4d" }, \
{ kX86 ## opname ## 32TI8, kThreadImm,is_store | IS_BINARY_OP   | SETS_CCODES, { THREAD_PREFIX, 0, rm32_i8,  0, 0, rm32_i8_modrm,  0,        1 }, #opname "32TI8", "fs:[!0d],!1d" }

ENCODING_MAP(Add, IS_STORE,
  0x00 /* RegMem8/Reg8 */,     0x01 /* RegMem32/Reg32 */,
  0x02 /* Reg8/RegMem8 */,     0x03 /* Reg32/RegMem32 */,
  0x04 /* Rax8/imm8 opcode */, 0x05 /* Rax32/imm32 */,
  0x80, 0x0 /* RegMem8/imm8 */,
  0x81, 0x0 /* RegMem32/imm32 */, 0x83, 0x0 /* RegMem32/imm8 */),
ENCODING_MAP(Or, IS_STORE,
  0x08 /* RegMem8/Reg8 */,     0x09 /* RegMem32/Reg32 */,
  0x0A /* Reg8/RegMem8 */,     0x0B /* Reg32/RegMem32 */,
  0x0C /* Rax8/imm8 opcode */, 0x0D /* Rax32/imm32 */,
  0x80, 0x1 /* RegMem8/imm8 */,
  0x81, 0x1 /* RegMem32/imm32 */, 0x83, 0x1 /* RegMem32/imm8 */),
ENCODING_MAP(Adc, IS_STORE,
  0x10 /* RegMem8/Reg8 */,     0x11 /* RegMem32/Reg32 */,
  0x12 /* Reg8/RegMem8 */,     0x13 /* Reg32/RegMem32 */,
  0x14 /* Rax8/imm8 opcode */, 0x15 /* Rax32/imm32 */,
  0x80, 0x2 /* RegMem8/imm8 */,
  0x81, 0x2 /* RegMem32/imm32 */, 0x83, 0x2 /* RegMem32/imm8 */),
ENCODING_MAP(Sbb, IS_STORE,
  0x18 /* RegMem8/Reg8 */,     0x19 /* RegMem32/Reg32 */,
  0x1A /* Reg8/RegMem8 */,     0x1B /* Reg32/RegMem32 */,
  0x1C /* Rax8/imm8 opcode */, 0x1D /* Rax32/imm32 */,
  0x80, 0x3 /* RegMem8/imm8 */,
  0x81, 0x3 /* RegMem32/imm32 */, 0x83, 0x3 /* RegMem32/imm8 */),
ENCODING_MAP(And, IS_STORE,
  0x20 /* RegMem8/Reg8 */,     0x21 /* RegMem32/Reg32 */,
  0x22 /* Reg8/RegMem8 */,     0x23 /* Reg32/RegMem32 */,
  0x24 /* Rax8/imm8 opcode */, 0x25 /* Rax32/imm32 */,
  0x80, 0x4 /* RegMem8/imm8 */,
  0x81, 0x4 /* RegMem32/imm32 */, 0x83, 0x4 /* RegMem32/imm8 */),
ENCODING_MAP(Sub, IS_STORE,
  0x28 /* RegMem8/Reg8 */,     0x29 /* RegMem32/Reg32 */,
  0x2A /* Reg8/RegMem8 */,     0x2B /* Reg32/RegMem32 */,
  0x2C /* Rax8/imm8 opcode */, 0x2D /* Rax32/imm32 */,
  0x80, 0x5 /* RegMem8/imm8 */,
  0x81, 0x5 /* RegMem32/imm32 */, 0x83, 0x5 /* RegMem32/imm8 */),
ENCODING_MAP(Xor, IS_STORE,
  0x30 /* RegMem8/Reg8 */,     0x31 /* RegMem32/Reg32 */,
  0x32 /* Reg8/RegMem8 */,     0x33 /* Reg32/RegMem32 */,
  0x34 /* Rax8/imm8 opcode */, 0x35 /* Rax32/imm32 */,
  0x80, 0x6 /* RegMem8/imm8 */,
  0x81, 0x6 /* RegMem32/imm32 */, 0x83, 0x6 /* RegMem32/imm8 */),
ENCODING_MAP(Cmp, IS_LOAD,
  0x38 /* RegMem8/Reg8 */,     0x39 /* RegMem32/Reg32 */,
  0x3A /* Reg8/RegMem8 */,     0x3B /* Reg32/RegMem32 */,
  0x3C /* Rax8/imm8 opcode */, 0x3D /* Rax32/imm32 */,
  0x80, 0x7 /* RegMem8/imm8 */,
  0x81, 0x7 /* RegMem32/imm32 */, 0x83, 0x7 /* RegMem32/imm8 */),
#undef ENCODING_MAP

  { kX86Imul16RRI,   kRegRegImm,             IS_TERTIARY_OP | SETS_CCODES, { 0x66, 0, 0x69, 0, 0, 0, 0, 2 }, "Imul16RRI", "" },
  { kX86Imul16RMI,   kRegMemImm,   IS_LOAD | IS_QUAD_OP     | SETS_CCODES, { 0x66, 0, 0x69, 0, 0, 0, 0, 2 }, "Imul16RMI", "" },
  { kX86Imul16RAI,   kRegArrayImm, IS_LOAD | IS_SEXTUPLE_OP | SETS_CCODES, { 0x66, 0, 0x69, 0, 0, 0, 0, 2 }, "Imul16RAI", "" },

  { kX86Imul32RRI,   kRegRegImm,             IS_TERTIARY_OP | SETS_CCODES, { 0, 0, 0x69, 0, 0, 0, 0, 2 }, "Imul32RRI", "" },
  { kX86Imul32RMI,   kRegMemImm,   IS_LOAD | IS_QUAD_OP     | SETS_CCODES, { 0, 0, 0x69, 0, 0, 0, 0, 2 }, "Imul32RMI", "" },
  { kX86Imul32RAI,   kRegArrayImm, IS_LOAD | IS_SEXTUPLE_OP | SETS_CCODES, { 0, 0, 0x69, 0, 0, 0, 0, 2 }, "Imul32RAI", "" },
  { kX86Imul32RRI8,  kRegRegImm,             IS_TERTIARY_OP | SETS_CCODES, { 0, 0, 0x6B, 0, 0, 0, 0, 1 }, "Imul32RRI8", "" },
  { kX86Imul32RMI8,  kRegMemImm,   IS_LOAD | IS_QUAD_OP     | SETS_CCODES, { 0, 0, 0x6B, 0, 0, 0, 0, 1 }, "Imul32RMI8", "" },
  { kX86Imul32RAI8,  kRegArrayImm, IS_LOAD | IS_SEXTUPLE_OP | SETS_CCODES, { 0, 0, 0x6B, 0, 0, 0, 0, 1 }, "Imul32RAI8", "" },

  { kX86Mov8MR, kMemReg,    IS_STORE | IS_TERTIARY_OP, { 0, 0,             0x88, 0, 0, 0, 0, 0 }, "Mov8MR", "[!0r+!1d],!2r" },
  { kX86Mov8AR, kArrayReg,  IS_STORE | IS_QUIN_OP,     { 0, 0,             0x88, 0, 0, 0, 0, 0 }, "Mov8AR", "[!0r+!1r<<!2d+!3d],!4r" },
  { kX86Mov8TR, kThreadReg, IS_STORE | IS_BINARY_OP,   { THREAD_PREFIX, 0, 0x88, 0, 0, 0, 0, 0 }, "Mov8TR", "fs:[!0d],!1r" },
  { kX86Mov8RR, kRegReg,               IS_BINARY_OP,   { 0, 0,             0x8A, 0, 0, 0, 0, 0 }, "Mov8RR", "!0r,!1r" },
  { kX86Mov8RM, kRegMem,    IS_LOAD  | IS_TERTIARY_OP, { 0, 0,             0x8A, 0, 0, 0, 0, 0 }, "Mov8RM", "!0r,[!1r+!2d]" },
  { kX86Mov8RA, kRegArray,  IS_LOAD  | IS_QUIN_OP,     { 0, 0,             0x8A, 0, 0, 0, 0, 0 }, "Mov8RA", "!0r,[!1r+!2r<<!3d+!4d]" },
  { kX86Mov8RT, kRegThread, IS_LOAD  | IS_BINARY_OP,   { THREAD_PREFIX, 0, 0x8A, 0, 0, 0, 0, 0 }, "Mov8RT", "!0r,fs:[!1d]" },
  { kX86Mov8RI, kMovRegImm,            IS_BINARY_OP,   { 0, 0,             0xB0, 0, 0, 0, 0, 1 }, "Mov8RI", "!0r,!1d" },
  { kX86Mov8MI, kMemImm,    IS_STORE | IS_TERTIARY_OP, { 0, 0,             0xC6, 0, 0, 0, 0, 1 }, "Mov8MI", "[!0r+!1d],!2r" },
  { kX86Mov8AI, kArrayImm,  IS_STORE | IS_QUIN_OP,     { 0, 0,             0xC6, 0, 0, 0, 0, 1 }, "Mov8AI", "[!0r+!1r<<!2d+!3d],!4d" },
  { kX86Mov8TI, kThreadImm, IS_STORE | IS_BINARY_OP,   { THREAD_PREFIX, 0, 0xC6, 0, 0, 0, 0, 1 }, "Mov8TI", "fs:[!0d],!1d" },

  { kX86Mov16MR, kMemReg,    IS_STORE | IS_TERTIARY_OP, { 0x66, 0,             0x89, 0, 0, 0, 0, 0 }, "Mov16MR", "[!0r+!1d],!2r" },
  { kX86Mov16AR, kArrayReg,  IS_STORE | IS_QUIN_OP,     { 0x66, 0,             0x89, 0, 0, 0, 0, 0 }, "Mov16AR", "[!0r+!1r<<!2d+!3d],!4r" },
  { kX86Mov16TR, kThreadReg, IS_STORE | IS_BINARY_OP,   { THREAD_PREFIX, 0x66, 0x89, 0, 0, 0, 0, 0 }, "Mov16TR", "fs:[!0d],!1r" },
  { kX86Mov16RR, kRegReg,               IS_BINARY_OP,   { 0x66, 0,             0x8B, 0, 0, 0, 0, 0 }, "Mov16RR", "!0r,!1r" },
  { kX86Mov16RM, kRegMem,    IS_LOAD  | IS_TERTIARY_OP, { 0x66, 0,             0x8B, 0, 0, 0, 0, 0 }, "Mov16RM", "!0r,[!1r+!2d]" },
  { kX86Mov16RA, kRegArray,  IS_LOAD  | IS_QUIN_OP,     { 0x66, 0,             0x8B, 0, 0, 0, 0, 0 }, "Mov16RA", "!0r,[!1r+!2r<<!3d+!4d]" },
  { kX86Mov16RT, kRegThread, IS_LOAD  | IS_BINARY_OP,   { THREAD_PREFIX, 0x66, 0x8B, 0, 0, 0, 0, 0 }, "Mov16RT", "!0r,fs:[!1d]" },
  { kX86Mov16RI, kMovRegImm,            IS_BINARY_OP,   { 0x66, 0,             0xB8, 0, 0, 0, 0, 2 }, "Mov16RI", "!0r,!1d" },
  { kX86Mov16MI, kMemImm,    IS_STORE | IS_TERTIARY_OP, { 0x66, 0,             0xC7, 0, 0, 0, 0, 2 }, "Mov16MI", "[!0r+!1d],!2r" },
  { kX86Mov16AI, kArrayImm,  IS_STORE | IS_QUIN_OP,     { 0x66, 0,             0xC7, 0, 0, 0, 0, 2 }, "Mov16AI", "[!0r+!1r<<!2d+!3d],!4d" },
  { kX86Mov16TI, kThreadImm, IS_STORE | IS_BINARY_OP,   { THREAD_PREFIX, 0x66, 0xC7, 0, 0, 0, 0, 2 }, "Mov16TI", "fs:[!0d],!1d" },

  { kX86Mov32MR, kMemReg,    IS_STORE | IS_TERTIARY_OP, { 0, 0,             0x89, 0, 0, 0, 0, 0 }, "Mov32MR", "[!0r+!1d],!2r" },
  { kX86Mov32AR, kArrayReg,  IS_STORE | IS_QUIN_OP,     { 0, 0,             0x89, 0, 0, 0, 0, 0 }, "Mov32AR", "[!0r+!1r<<!2d+!3d],!4r" },
  { kX86Mov32TR, kThreadReg, IS_STORE | IS_BINARY_OP,   { THREAD_PREFIX, 0, 0x89, 0, 0, 0, 0, 0 }, "Mov32TR", "fs:[!0d],!1r" },
  { kX86Mov32RR, kRegReg,               IS_BINARY_OP,   { 0, 0,             0x8B, 0, 0, 0, 0, 0 }, "Mov32RR", "!0r,!1r" },
  { kX86Mov32RM, kRegMem,    IS_LOAD  | IS_TERTIARY_OP, { 0, 0,             0x8B, 0, 0, 0, 0, 0 }, "Mov32RM", "!0r,[!1r+!2d]" },
  { kX86Mov32RA, kRegArray,  IS_LOAD  | IS_QUIN_OP,     { 0, 0,             0x8B, 0, 0, 0, 0, 0 }, "Mov32RA", "!0r,[!1r+!2r<<!3d+!4d]" },
  { kX86Mov32RT, kRegThread, IS_LOAD  | IS_BINARY_OP,   { THREAD_PREFIX, 0, 0x8B, 0, 0, 0, 0, 0 }, "Mov32RT", "!0r,fs:[!1d]" },
  { kX86Mov32RI, kMovRegImm,            IS_BINARY_OP,   { 0, 0,             0xB8, 0, 0, 0, 0, 4 }, "Mov32RI", "!0r,!1d" },
  { kX86Mov32MI, kMemImm,    IS_STORE | IS_TERTIARY_OP, { 0, 0,             0xC7, 0, 0, 0, 0, 4 }, "Mov32MI", "[!0r+!1d],!2r" },
  { kX86Mov32AI, kArrayImm,  IS_STORE | IS_QUIN_OP,     { 0, 0,             0xC7, 0, 0, 0, 0, 4 }, "Mov32AI", "[!0r+!1r<<!2d+!3d],!4d" },
  { kX86Mov32TI, kThreadImm, IS_STORE | IS_BINARY_OP,   { THREAD_PREFIX, 0, 0xC7, 0, 0, 0, 0, 4 }, "Mov32TI", "fs:[!0d],!1d" },

  { kX86Lea32RA, kRegArray, IS_QUIN_OP, { 0, 0, 0x8D, 0, 0, 0, 0, 0 }, "Lea32RA", "!0r,[!1r+!2r<<!3d+!4d]" },

#define SHIFT_ENCODING_MAP(opname, modrm_opcode) \
{ kX86 ## opname ## 8RI, kShiftRegImm,   IS_BINARY_OP   | SETS_CCODES, { 0, 0,    0xC0, 0, 0, modrm_opcode, 0xD1, 1 }, #opname "8RI", "!0r,!1d" }, \
{ kX86 ## opname ## 8MI, kShiftMemImm,   IS_TERTIARY_OP | SETS_CCODES, { 0, 0,    0xC0, 0, 0, modrm_opcode, 0xD1, 1 }, #opname "8MI", "[!0r+!1d],!2r" }, \
{ kX86 ## opname ## 8AI, kShiftArrayImm, IS_QUIN_OP     | SETS_CCODES, { 0, 0,    0xC0, 0, 0, modrm_opcode, 0xD1, 1 }, #opname "8AI", "[!0r+!1r<<!2d+!3d],!4d" }, \
{ kX86 ## opname ## 8RC, kShiftRegCl,    IS_BINARY_OP   | SETS_CCODES, { 0, 0,    0xD2, 0, 0, modrm_opcode, 0,    1 }, #opname "8RC", "" }, \
{ kX86 ## opname ## 8MC, kShiftMemCl,    IS_TERTIARY_OP | SETS_CCODES, { 0, 0,    0xD2, 0, 0, modrm_opcode, 0,    1 }, #opname "8MC", "" }, \
{ kX86 ## opname ## 8AC, kShiftArrayCl,  IS_QUIN_OP     | SETS_CCODES, { 0, 0,    0xD2, 0, 0, modrm_opcode, 0,    1 }, #opname "8AC", "" }, \
  \
{ kX86 ## opname ## 16RI, kShiftRegImm,   IS_BINARY_OP   | SETS_CCODES, { 0x66, 0, 0xC1, 0, 0, modrm_opcode, 0xD1, 1 }, #opname "16RI", "!0r,!1d" }, \
{ kX86 ## opname ## 16MI, kShiftMemImm,   IS_TERTIARY_OP | SETS_CCODES, { 0x66, 0, 0xC1, 0, 0, modrm_opcode, 0xD1, 1 }, #opname "16MI", "[!0r+!1d],!2r" }, \
{ kX86 ## opname ## 16AI, kShiftArrayImm, IS_QUIN_OP     | SETS_CCODES, { 0x66, 0, 0xC1, 0, 0, modrm_opcode, 0xD1, 1 }, #opname "16AI", "[!0r+!1r<<!2d+!3d],!4d" }, \
{ kX86 ## opname ## 16RC, kShiftRegCl,    IS_BINARY_OP   | SETS_CCODES, { 0x66, 0, 0xD3, 0, 0, modrm_opcode, 0,    1 }, #opname "16RC", "" }, \
{ kX86 ## opname ## 16MC, kShiftMemCl,    IS_TERTIARY_OP | SETS_CCODES, { 0x66, 0, 0xD3, 0, 0, modrm_opcode, 0,    1 }, #opname "16MC", "" }, \
{ kX86 ## opname ## 16AC, kShiftArrayCl,  IS_QUIN_OP     | SETS_CCODES, { 0x66, 0, 0xD3, 0, 0, modrm_opcode, 0,    1 }, #opname "16AC", "" }, \
  \
{ kX86 ## opname ## 32RI, kShiftRegImm,   IS_BINARY_OP   | SETS_CCODES, { 0, 0,    0xC1, 0, 0, modrm_opcode, 0xD1, 1 }, #opname "32RI", "!0r,!1d" }, \
{ kX86 ## opname ## 32MI, kShiftMemImm,   IS_TERTIARY_OP | SETS_CCODES, { 0, 0,    0xC1, 0, 0, modrm_opcode, 0xD1, 1 }, #opname "32MI", "[!0r+!1d],!2r" }, \
{ kX86 ## opname ## 32AI, kShiftArrayImm, IS_QUIN_OP     | SETS_CCODES, { 0, 0,    0xC1, 0, 0, modrm_opcode, 0xD1, 1 }, #opname "32AI", "[!0r+!1r<<!2d+!3d],!4d" }, \
{ kX86 ## opname ## 32RC, kShiftRegCl,    IS_BINARY_OP   | SETS_CCODES, { 0, 0,    0xD3, 0, 0, modrm_opcode, 0,    1 }, #opname "32RC", "" }, \
{ kX86 ## opname ## 32MC, kShiftMemCl,    IS_TERTIARY_OP | SETS_CCODES, { 0, 0,    0xD3, 0, 0, modrm_opcode, 0,    1 }, #opname "32MC", "" }, \
{ kX86 ## opname ## 32AC, kShiftArrayCl,  IS_QUIN_OP     | SETS_CCODES, { 0, 0,    0xD3, 0, 0, modrm_opcode, 0,    1 }, #opname "32AC", "" }

  SHIFT_ENCODING_MAP(Rol, 0x0),
  SHIFT_ENCODING_MAP(Ror, 0x1),
  SHIFT_ENCODING_MAP(Rcl, 0x2),
  SHIFT_ENCODING_MAP(Rcr, 0x3),
  SHIFT_ENCODING_MAP(Sal, 0x4),
  SHIFT_ENCODING_MAP(Shl, 0x5),
  SHIFT_ENCODING_MAP(Shr, 0x6),
  SHIFT_ENCODING_MAP(Sar, 0x7),
#undef SHIFT_ENCODING_MAP

#define UNARY_ENCODING_MAP(opname, modrm, \
                           reg, reg_kind, reg_flags, \
                           mem, mem_kind, mem_flags, \
                           arr, arr_kind, arr_flags, imm) \
{ kX86 ## opname ## 8 ## reg,  reg_kind,           reg_flags, { 0,    0, 0xF6, 0, 0, modrm, 0, imm << 0}, #opname "8" #reg, "" }, \
{ kX86 ## opname ## 8 ## mem,  mem_kind, IS_LOAD | mem_flags, { 0,    0, 0xF6, 0, 0, modrm, 0, imm << 0}, #opname "8" #mem, "" }, \
{ kX86 ## opname ## 8 ## arr,  arr_kind, IS_LOAD | arr_flags, { 0,    0, 0xF6, 0, 0, modrm, 0, imm << 0}, #opname "8" #arr, "" }, \
{ kX86 ## opname ## 16 ## reg, reg_kind,           reg_flags, { 0x66, 0, 0xF7, 0, 0, modrm, 0, imm << 1}, #opname "16" #reg, "" }, \
{ kX86 ## opname ## 16 ## mem, mem_kind, IS_LOAD | mem_flags, { 0x66, 0, 0xF7, 0, 0, modrm, 0, imm << 1}, #opname "16" #mem, "" }, \
{ kX86 ## opname ## 16 ## arr, arr_kind, IS_LOAD | arr_flags, { 0x66, 0, 0xF7, 0, 0, modrm, 0, imm << 1}, #opname "16" #arr, "" }, \
{ kX86 ## opname ## 32 ## reg, reg_kind,           reg_flags, { 0,    0, 0xF7, 0, 0, modrm, 0, imm << 2}, #opname "32" #reg, "" }, \
{ kX86 ## opname ## 32 ## mem, mem_kind, IS_LOAD | mem_flags, { 0,    0, 0xF7, 0, 0, modrm, 0, imm << 2}, #opname "32" #mem, "" }, \
{ kX86 ## opname ## 32 ## arr, arr_kind, IS_LOAD | arr_flags, { 0,    0, 0xF7, 0, 0, modrm, 0, imm << 2}, #opname "32" #arr, "" }

  UNARY_ENCODING_MAP(Test,    0x0, RI, kRegImm, IS_BINARY_OP, MI, kMemImm, IS_TERTIARY_OP, AI, kArrayImm, IS_QUIN_OP, 1),
  UNARY_ENCODING_MAP(Not,     0x2, R, kReg, IS_UNARY_OP, M, kMem, IS_BINARY_OP, A, kArray, IS_QUAD_OP, 0),
  UNARY_ENCODING_MAP(Neg,     0x3, R, kReg, IS_UNARY_OP, M, kMem, IS_BINARY_OP, A, kArray, IS_QUAD_OP, 0),
  UNARY_ENCODING_MAP(Mul,     0x4, DaR, kRegRegReg, IS_TERTIARY_OP, DaM, kRegRegMem, IS_QUAD_OP, DaA, kRegRegArray, IS_SEXTUPLE_OP, 0),
  UNARY_ENCODING_MAP(Imul,    0x5, DaR, kRegRegReg, IS_TERTIARY_OP, DaM, kRegRegMem, IS_QUAD_OP, DaA, kRegRegArray, IS_SEXTUPLE_OP, 0),
  UNARY_ENCODING_MAP(Divmod,  0x6, DaR, kRegRegReg, IS_TERTIARY_OP, DaM, kRegRegMem, IS_QUAD_OP, DaA, kRegRegArray, IS_SEXTUPLE_OP, 0),
  UNARY_ENCODING_MAP(Idivmod, 0x7, DaR, kRegRegReg, IS_TERTIARY_OP, DaM, kRegRegMem, IS_QUAD_OP, DaA, kRegRegArray, IS_SEXTUPLE_OP, 0),
#undef UNARY_ENCODING_MAP

#define EXT_0F_ENCODING_MAP(opname, prefix, opcode) \
{ kX86 ## opname ## RR, kRegReg,             IS_BINARY_OP,   { prefix, 0, 0x0F, opcode, 0, 0, 0, 0 }, #opname "RR", "!0r,!1r" }, \
{ kX86 ## opname ## RM, kRegMem,   IS_LOAD | IS_TERTIARY_OP, { prefix, 0, 0x0F, opcode, 0, 0, 0, 0 }, #opname "RM", "!0r,[!1r+!2d]" }, \
{ kX86 ## opname ## RA, kRegArray, IS_LOAD | IS_QUIN_OP,     { prefix, 0, 0x0F, opcode, 0, 0, 0, 0 }, #opname "RA", "!0r,[!1r+!2r<<!3d+!4d]" }

  EXT_0F_ENCODING_MAP(Movsd, 0xF2, 0x10),
  { kX86MovsdMR, kMemReg,   IS_STORE | IS_TERTIARY_OP, { 0xF2, 0, 0x0F, 0x11, 0, 0, 0, 0 }, "MovsdMR", "[!0r+!1d],!2r" },
  { kX86MovsdAR, kArrayReg, IS_STORE | IS_QUIN_OP,     { 0xF2, 0, 0x0F, 0x11, 0, 0, 0, 0 }, "MovsdAR", "[!0r+!1r<<!2d+!3d],!4r" },

  EXT_0F_ENCODING_MAP(Movss, 0xF3, 0x10),
  { kX86MovssMR, kMemReg,   IS_STORE | IS_TERTIARY_OP, { 0xF3, 0, 0x0F, 0x11, 0, 0, 0, 0 }, "MovssMR", "[!0r+!1d],!2r" },
  { kX86MovssAR, kArrayReg, IS_STORE | IS_QUIN_OP,     { 0xF3, 0, 0x0F, 0x11, 0, 0, 0, 0 }, "MovssAR", "[!0r+!1r<<!2d+!3d],!4r" },

  EXT_0F_ENCODING_MAP(Cvtsi2sd,  0xF2, 0x2A),
  EXT_0F_ENCODING_MAP(Cvtsi2ss,  0xF3, 0x2A),
  EXT_0F_ENCODING_MAP(Cvttsd2si, 0xF2, 0x2C),
  EXT_0F_ENCODING_MAP(Cvttss2si, 0xF3, 0x2C),
  EXT_0F_ENCODING_MAP(Cvtsd2si,  0xF2, 0x2D),
  EXT_0F_ENCODING_MAP(Cvtss2si,  0xF3, 0x2D),
  EXT_0F_ENCODING_MAP(Ucomisd,   0x66, 0x2E),
  EXT_0F_ENCODING_MAP(Ucomiss,   0x00, 0x2E),
  EXT_0F_ENCODING_MAP(Comisd,    0x66, 0x2F),
  EXT_0F_ENCODING_MAP(Comiss,    0x00, 0x2F),
  EXT_0F_ENCODING_MAP(Orps,      0x00, 0x56),
  EXT_0F_ENCODING_MAP(Xorps,     0x00, 0x57),
  EXT_0F_ENCODING_MAP(Addsd,     0xF2, 0x58),
  EXT_0F_ENCODING_MAP(Addss,     0xF3, 0x58),
  EXT_0F_ENCODING_MAP(Mulsd,     0xF2, 0x59),
  EXT_0F_ENCODING_MAP(Mulss,     0xF3, 0x59),
  EXT_0F_ENCODING_MAP(Cvtss2sd,  0xF2, 0x5A),
  EXT_0F_ENCODING_MAP(Cvtsd2ss,  0xF3, 0x5A),
  EXT_0F_ENCODING_MAP(Subsd,     0xF2, 0x5C),
  EXT_0F_ENCODING_MAP(Subss,     0xF3, 0x5C),
  EXT_0F_ENCODING_MAP(Divsd,     0xF2, 0x5E),
  EXT_0F_ENCODING_MAP(Divss,     0xF3, 0x5E),

  { kX86PsllqRI, kRegImm, IS_BINARY_OP, { 0, 0, 0x0F, 0x73, 0, 7, 0, 1 }, "PsllqRI", "!0r, !1d" },

  EXT_0F_ENCODING_MAP(Movdxr,    0x66, 0x6E),
  EXT_0F_ENCODING_MAP(Movdrx,    0x66, 0x7E),

  { kX86Set8R, kRegCond,              IS_BINARY_OP,   { 0, 0, 0x0F, 0x90, 0, 0, 0, 0 }, "Set8R", "!1c !0r" },
  { kX86Set8M, kMemCond,   IS_STORE | IS_TERTIARY_OP, { 0, 0, 0x0F, 0x90, 0, 0, 0, 0 }, "Set8M", "!2c [!0r+!1d]" },
  { kX86Set8A, kArrayCond, IS_STORE | IS_QUIN_OP,     { 0, 0, 0x0F, 0x90, 0, 0, 0, 0 }, "Set8A", "!4c [!0r+!1r<<!2d+!3d]" },

  EXT_0F_ENCODING_MAP(Imul16,  0x66, 0xAF),
  EXT_0F_ENCODING_MAP(Imul32,  0x00, 0xAF),
  EXT_0F_ENCODING_MAP(Movzx8,  0x00, 0xB6),
  EXT_0F_ENCODING_MAP(Movzx16, 0x00, 0xB7),
  EXT_0F_ENCODING_MAP(Movsx8,  0x00, 0xBE),
  EXT_0F_ENCODING_MAP(Movsx16, 0x00, 0xBF),
#undef EXT_0F_ENCODING_MAP

  { kX86Jcc8,  kJcc,  IS_BINARY_OP | IS_BRANCH | NEEDS_FIXUP, { 0,             0, 0x70, 0,    0, 0, 0, 0 }, "Jcc8",  "!1c !0t" },
  { kX86Jcc32, kJcc,  IS_BINARY_OP | IS_BRANCH | NEEDS_FIXUP, { 0,             0, 0x0F, 0x80, 0, 0, 0, 0 }, "Jcc32", "!1c !0t" },
  { kX86Jmp8,  kJmp,  IS_UNARY_OP  | IS_BRANCH | NEEDS_FIXUP, { 0,             0, 0xEB, 0,    0, 0, 0, 0 }, "Jmp8",  "!0t" },
  { kX86Jmp32, kJmp,  IS_UNARY_OP  | IS_BRANCH | NEEDS_FIXUP, { 0,             0, 0xE9, 0,    0, 0, 0, 0 }, "Jmp32", "!0t" },
  { kX86CallR, kCall, IS_UNARY_OP  | IS_BRANCH,               { 0,             0, 0xE8, 0, 0, 0, 0, 0 }, "CallR", "!0r" },
  { kX86CallM, kCall, IS_BINARY_OP | IS_BRANCH | IS_LOAD,     { 0,             0, 0xFF, 0, 0, 2, 0, 0 }, "CallM", "[!0r+!1d]" },
  { kX86CallA, kCall, IS_QUAD_OP   | IS_BRANCH | IS_LOAD,     { 0,             0, 0xFF, 0, 0, 2, 0, 0 }, "CallA", "[!0r+!1r<<!2d+!3d]" },
  { kX86CallT, kCall, IS_UNARY_OP  | IS_BRANCH | IS_LOAD,     { THREAD_PREFIX, 0, 0xFF, 0, 0, 2, 0, 0 }, "CallT", "fs:[!0d]" },
  { kX86Ret,   kNullary,NO_OPERAND | IS_BRANCH,               { 0,             0, 0xC3, 0, 0, 0, 0, 0 }, "Ret", "" },
};

static size_t computeSize(X86EncodingMap* entry, int displacement, bool has_sib) {
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
  if (has_sib) {
    ++size;
  }
  if (displacement != 0) {
    if (entry->opcode != kX86Lea32RA) {
      DCHECK_NE(entry->flags & (IS_LOAD | IS_STORE), 0);
    }
    size += IS_SIMM8(displacement) ? 1 : 4;
  }
  size += entry->skeleton.immediate_bytes;
  return size;
}

int oatGetInsnSize(LIR* lir) {
  X86EncodingMap* entry = &EncodingMap[lir->opcode];
  switch (entry->kind) {
    case kData:
      return 4;  // 4 bytes of data
    case kNop:
      return lir->operands[0];  // length of nop is sole operand
    case kNullary:
      return 1;  // 1 byte of opcode
    case kReg:  // lir operands - 0: reg
      return computeSize(entry, 0, false);
    case kMem: { // lir operands - 0: base, 1: disp
      int base = lir->operands[0];
      // SP requires a special extra SIB byte
      return computeSize(entry, lir->operands[1], false) + (base == rSP ? 1 : 0);
    }
    case kArray:  // lir operands - 0: base, 1: index, 2: scale, 3: disp
      return computeSize(entry, lir->operands[3], true);
    case kMemReg: { // lir operands - 0: base, 1: disp, 2: reg
      int base = lir->operands[0];
      // SP requires a special extra SIB byte
      return computeSize(entry, lir->operands[1], false) + (base == rSP ? 1 : 0);
    }
    case kArrayReg:  // lir operands - 0: base, 1: index, 2: scale, 3: disp, 4: reg
      return computeSize(entry, lir->operands[3], true);
    case kThreadReg:  // lir operands - 0: disp, 1: reg
      return computeSize(entry, lir->operands[0], false);
    case kRegReg:
      return computeSize(entry, 0, false);
    case kRegMem: { // lir operands - 0: reg, 1: base, 2: disp
      int base = lir->operands[1];
      return computeSize(entry, lir->operands[2], false) + (base == rSP ? 1 : 0);
    }
    case kRegArray:  // lir operands - 0: reg, 1: base, 2: index, 3: scale, 4: disp
      return computeSize(entry, lir->operands[4], true);
    case kRegThread:  // lir operands - 0: reg, 1: disp
      return computeSize(entry, 0x12345678, false);  // displacement size is always 32bit
    case kRegImm: {  // lir operands - 0: reg, 1: immediate
      size_t size = computeSize(entry, 0, false);
      if (entry->skeleton.ax_opcode == 0) {
        return size;
      } else {
        // AX opcodes don't require the modrm byte.
        int reg = lir->operands[0];
        return size - (reg == rAX ? 1 : 0);
      }
    }
    case kMemImm:  // lir operands - 0: base, 1: disp, 2: immediate
      CHECK_NE(lir->operands[0], static_cast<int>(rSP));  // TODO: add extra SIB byte
      return computeSize(entry, lir->operands[1], false);
    case kArrayImm:  // lir operands - 0: base, 1: index, 2: scale, 3: disp 4: immediate
      return computeSize(entry, lir->operands[3], true);
    case kThreadImm:  // lir operands - 0: disp, 1: imm
      return computeSize(entry, 0x12345678, false);  // displacement size is always 32bit
    case kRegRegImm:  // lir operands - 0: reg, 1: reg, 2: imm
      return computeSize(entry, 0, false);
    case kRegMemImm:  // lir operands - 0: reg, 1: base, 2: disp, 3: imm
      CHECK_NE(lir->operands[1], static_cast<int>(rSP));  // TODO: add extra SIB byte
      return computeSize(entry, lir->operands[2], false);
    case kRegArrayImm:  // lir operands - 0: reg, 1: base, 2: index, 3: scale, 4: disp, 5: imm
      return computeSize(entry, lir->operands[4], true);
    case kMovRegImm:  // lir operands - 0: reg, 1: immediate
      return 1 + entry->skeleton.immediate_bytes;
    case kShiftRegImm:  // lir operands - 0: reg, 1: immediate
      // Shift by immediate one has a shorter opcode.
      return computeSize(entry, 0, false) - (lir->operands[1] == 1 ? 1 : 0);
    case kShiftMemImm:  // lir operands - 0: base, 1: disp, 2: immediate
      CHECK_NE(lir->operands[0], static_cast<int>(rSP));  // TODO: add extra SIB byte
      // Shift by immediate one has a shorter opcode.
      return computeSize(entry, lir->operands[1], false) - (lir->operands[2] == 1 ? 1 : 0);
    case kShiftArrayImm:  // lir operands - 0: base, 1: index, 2: scale, 3: disp 4: immediate
      // Shift by immediate one has a shorter opcode.
      return computeSize(entry, lir->operands[3], true) - (lir->operands[4] == 1 ? 1 : 0);
    case kShiftRegCl:
      return computeSize(entry, 0, false);
    case kShiftMemCl:  // lir operands - 0: base, 1: disp, 2: cl
      CHECK_NE(lir->operands[0], static_cast<int>(rSP));  // TODO: add extra SIB byte
      return computeSize(entry, lir->operands[1], false);
    case kShiftArrayCl:  // lir operands - 0: base, 1: index, 2: scale, 3: disp, 4: reg
      return computeSize(entry, lir->operands[3], true);
    case kRegCond:  // lir operands - 0: reg, 1: cond
      return computeSize(entry, 0, false);
    case kMemCond:  // lir operands - 0: base, 1: disp, 2: cond
      CHECK_NE(lir->operands[0], static_cast<int>(rSP));  // TODO: add extra SIB byte
      return computeSize(entry, lir->operands[1], false);
    case kArrayCond:  // lir operands - 0: base, 1: index, 2: scale, 3: disp, 4: cond
      return computeSize(entry, lir->operands[3], true);
    case kJcc:
      if (lir->opcode == kX86Jcc8) {
        return 2;  // opcode + rel8
      } else {
        DCHECK(lir->opcode == kX86Jcc32);
        return 6;  // 2 byte opcode + rel32
      }
    case kJmp:
      if (lir->opcode == kX86Jmp8) {
        return 2;  // opcode + rel8
      } else {
        DCHECK(lir->opcode == kX86Jmp32);
        return 5;  // opcode + rel32
      }
    case kCall:
      switch(lir->opcode) {
        case kX86CallR: return 2;  // opcode modrm
        case kX86CallM:  // lir operands - 0: base, 1: disp
          return computeSize(entry, lir->operands[1], false);
        case kX86CallA:  // lir operands - 0: base, 1: index, 2: scale, 3: disp
          return computeSize(entry, lir->operands[3], true);
        case kX86CallT:  // lir operands - 0: disp
          return computeSize(entry, 0x12345678, false);  // displacement size is always 32bit
        default:
          break;
      }
      break;
    default:
      break;
  }
  UNIMPLEMENTED(FATAL) << "Unimplemented size encoding for: " << entry->name;
  return 0;
}

static uint8_t modrmForDisp(int disp) {
  if (disp == 0) {
    return 0;
  } else if (IS_SIMM8(disp)) {
    return 1;
  } else {
    return 2;
  }
}

static void emitDisp(CompilationUnit* cUnit, int disp) {
  if (disp == 0) {
    return;
  } else if (IS_SIMM8(disp)) {
    cUnit->codeBuffer.push_back(disp & 0xFF);
  } else {
    cUnit->codeBuffer.push_back(disp & 0xFF);
    cUnit->codeBuffer.push_back((disp >> 8) & 0xFF);
    cUnit->codeBuffer.push_back((disp >> 16) & 0xFF);
    cUnit->codeBuffer.push_back((disp >> 24) & 0xFF);
  }
}

static void emitOpReg(CompilationUnit* cUnit, const X86EncodingMap* entry, uint8_t reg) {
  if (entry->skeleton.prefix1 != 0) {
    cUnit->codeBuffer.push_back(entry->skeleton.prefix1);
    if (entry->skeleton.prefix2 != 0) {
      cUnit->codeBuffer.push_back(entry->skeleton.prefix2);
    }
  } else {
    DCHECK_EQ(0, entry->skeleton.prefix2);
  }
  cUnit->codeBuffer.push_back(entry->skeleton.opcode);
  if (entry->skeleton.opcode == 0x0F) {
    cUnit->codeBuffer.push_back(entry->skeleton.extra_opcode1);
    if (entry->skeleton.extra_opcode1 == 0x38 || entry->skeleton.extra_opcode2 == 0x3A) {
      cUnit->codeBuffer.push_back(entry->skeleton.extra_opcode2);
    } else {
      DCHECK_EQ(0, entry->skeleton.extra_opcode2);
    }
  } else {
    DCHECK_EQ(0, entry->skeleton.extra_opcode1);
    DCHECK_EQ(0, entry->skeleton.extra_opcode2);
  }
  if (FPREG(reg)) {
    reg = reg & FP_REG_MASK;
  }
  DCHECK_LT(reg, 8);
  uint8_t modrm = (3 << 6) | (entry->skeleton.modrm_opcode << 3) | reg;
  cUnit->codeBuffer.push_back(modrm);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  DCHECK_EQ(0, entry->skeleton.immediate_bytes);
}

static void emitOpMem(CompilationUnit* cUnit, const X86EncodingMap* entry, uint8_t base, int disp) {
  if (entry->skeleton.prefix1 != 0) {
    cUnit->codeBuffer.push_back(entry->skeleton.prefix1);
    if (entry->skeleton.prefix2 != 0) {
      cUnit->codeBuffer.push_back(entry->skeleton.prefix2);
    }
  } else {
    DCHECK_EQ(0, entry->skeleton.prefix2);
  }
  cUnit->codeBuffer.push_back(entry->skeleton.opcode);
  DCHECK_EQ(0, entry->skeleton.extra_opcode1);
  DCHECK_EQ(0, entry->skeleton.extra_opcode2);
  DCHECK_LT(entry->skeleton.modrm_opcode, 8);
  DCHECK_LT(base, 8);
  uint8_t modrm = (modrmForDisp(disp) << 6) | (entry->skeleton.modrm_opcode << 3) | base;
  cUnit->codeBuffer.push_back(modrm);
  emitDisp(cUnit, disp);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  DCHECK_EQ(0, entry->skeleton.immediate_bytes);
}

static void emitMemReg(CompilationUnit* cUnit, const X86EncodingMap* entry,
                       uint8_t base, int disp, uint8_t reg) {
  if (entry->skeleton.prefix1 != 0) {
    cUnit->codeBuffer.push_back(entry->skeleton.prefix1);
    if (entry->skeleton.prefix2 != 0) {
      cUnit->codeBuffer.push_back(entry->skeleton.prefix2);
    }
  } else {
    DCHECK_EQ(0, entry->skeleton.prefix2);
  }
  cUnit->codeBuffer.push_back(entry->skeleton.opcode);
  if (entry->skeleton.opcode == 0x0F) {
    cUnit->codeBuffer.push_back(entry->skeleton.extra_opcode1);
    if (entry->skeleton.extra_opcode1 == 0x38 || entry->skeleton.extra_opcode2 == 0x3A) {
      cUnit->codeBuffer.push_back(entry->skeleton.extra_opcode2);
    } else {
      DCHECK_EQ(0, entry->skeleton.extra_opcode2);
    }
  } else {
    DCHECK_EQ(0, entry->skeleton.extra_opcode1);
    DCHECK_EQ(0, entry->skeleton.extra_opcode2);
  }
  if (FPREG(reg)) {
    reg = reg & FP_REG_MASK;
  }
  DCHECK_LT(reg, 8);
  DCHECK_LT(base, 8);
  uint8_t modrm = (modrmForDisp(disp) << 6) | (reg << 3) | base;
  cUnit->codeBuffer.push_back(modrm);
  if (base == rSP) {
    // Special SIB for SP base
    cUnit->codeBuffer.push_back(0 << 6 | (rSP << 3) | rSP);
  }
  emitDisp(cUnit, disp);
  DCHECK_EQ(0, entry->skeleton.modrm_opcode);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  DCHECK_EQ(0, entry->skeleton.immediate_bytes);
}

static void emitRegMem(CompilationUnit* cUnit, const X86EncodingMap* entry,
                       uint8_t reg, uint8_t base, int disp) {
  // Opcode will flip operands.
  emitMemReg(cUnit, entry, base, disp, reg);
}

static void emitRegArray(CompilationUnit* cUnit, const X86EncodingMap* entry, uint8_t reg,
                         uint8_t base, uint8_t index, int scale, int disp) {
  if (entry->skeleton.prefix1 != 0) {
    cUnit->codeBuffer.push_back(entry->skeleton.prefix1);
    if (entry->skeleton.prefix2 != 0) {
      cUnit->codeBuffer.push_back(entry->skeleton.prefix2);
    }
  } else {
    DCHECK_EQ(0, entry->skeleton.prefix2);
  }
  cUnit->codeBuffer.push_back(entry->skeleton.opcode);
  if (entry->skeleton.opcode == 0x0F) {
    cUnit->codeBuffer.push_back(entry->skeleton.extra_opcode1);
    if (entry->skeleton.extra_opcode1 == 0x38 || entry->skeleton.extra_opcode2 == 0x3A) {
      cUnit->codeBuffer.push_back(entry->skeleton.extra_opcode2);
    } else {
      DCHECK_EQ(0, entry->skeleton.extra_opcode2);
    }
  } else {
    DCHECK_EQ(0, entry->skeleton.extra_opcode1);
    DCHECK_EQ(0, entry->skeleton.extra_opcode2);
  }
  if (FPREG(reg)) {
    reg = reg & FP_REG_MASK;
  }
  DCHECK_LT(reg, 8);
  uint8_t modrm = (modrmForDisp(disp) << 6) | (reg << 3) | rSP;
  cUnit->codeBuffer.push_back(modrm);
  DCHECK_LT(scale, 4);
  DCHECK_LT(index, 8);
  DCHECK_LT(base, 8);
  uint8_t sib = (scale << 6) | (index << 3) | base;
  cUnit->codeBuffer.push_back(sib);
  emitDisp(cUnit, disp);
  DCHECK_EQ(0, entry->skeleton.modrm_opcode);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  DCHECK_EQ(0, entry->skeleton.immediate_bytes);
}

static void emitArrayReg(CompilationUnit* cUnit, const X86EncodingMap* entry,
                         uint8_t base, uint8_t index, int scale, int disp, uint8_t reg) {
  // Opcode will flip operands.
  emitRegArray(cUnit, entry, reg, base, index, scale, disp);
}

static void emitRegThread(CompilationUnit* cUnit, const X86EncodingMap* entry,
                          uint8_t reg, int disp) {
  DCHECK_NE(entry->skeleton.prefix1, 0);
  cUnit->codeBuffer.push_back(entry->skeleton.prefix1);
  if (entry->skeleton.prefix2 != 0) {
    cUnit->codeBuffer.push_back(entry->skeleton.prefix2);
  }
  cUnit->codeBuffer.push_back(entry->skeleton.opcode);
  if (entry->skeleton.opcode == 0x0F) {
    cUnit->codeBuffer.push_back(entry->skeleton.extra_opcode1);
    if (entry->skeleton.extra_opcode1 == 0x38 || entry->skeleton.extra_opcode2 == 0x3A) {
      cUnit->codeBuffer.push_back(entry->skeleton.extra_opcode2);
    } else {
      DCHECK_EQ(0, entry->skeleton.extra_opcode2);
    }
  } else {
    DCHECK_EQ(0, entry->skeleton.extra_opcode1);
    DCHECK_EQ(0, entry->skeleton.extra_opcode2);
  }
  if (FPREG(reg)) {
    reg = reg & FP_REG_MASK;
  }
  DCHECK_LT(reg, 8);
  uint8_t modrm = (0 << 6) | (reg << 3) | rBP;
  cUnit->codeBuffer.push_back(modrm);
  cUnit->codeBuffer.push_back(disp & 0xFF);
  cUnit->codeBuffer.push_back((disp >> 8) & 0xFF);
  cUnit->codeBuffer.push_back((disp >> 16) & 0xFF);
  cUnit->codeBuffer.push_back((disp >> 24) & 0xFF);
  DCHECK_EQ(0, entry->skeleton.modrm_opcode);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  DCHECK_EQ(0, entry->skeleton.immediate_bytes);
}

static void emitRegReg(CompilationUnit* cUnit, const X86EncodingMap* entry,
                       uint8_t reg1, uint8_t reg2) {
  if (entry->skeleton.prefix1 != 0) {
    cUnit->codeBuffer.push_back(entry->skeleton.prefix1);
    if (entry->skeleton.prefix2 != 0) {
      cUnit->codeBuffer.push_back(entry->skeleton.prefix2);
    }
  } else {
    DCHECK_EQ(0, entry->skeleton.prefix2);
  }
  cUnit->codeBuffer.push_back(entry->skeleton.opcode);
  if (entry->skeleton.opcode == 0x0F) {
    cUnit->codeBuffer.push_back(entry->skeleton.extra_opcode1);
    if (entry->skeleton.extra_opcode1 == 0x38 || entry->skeleton.extra_opcode2 == 0x3A) {
      cUnit->codeBuffer.push_back(entry->skeleton.extra_opcode2);
    } else {
      DCHECK_EQ(0, entry->skeleton.extra_opcode2);
    }
  } else {
    DCHECK_EQ(0, entry->skeleton.extra_opcode1);
    DCHECK_EQ(0, entry->skeleton.extra_opcode2);
  }
  if (FPREG(reg1)) {
    reg1 = reg1 & FP_REG_MASK;
  }
  if (FPREG(reg2)) {
    reg2 = reg2 & FP_REG_MASK;
  }
  DCHECK_LT(reg1, 8);
  DCHECK_LT(reg2, 8);
  uint8_t modrm = (3 << 6) | (reg1 << 3) | reg2;
  cUnit->codeBuffer.push_back(modrm);
  DCHECK_EQ(0, entry->skeleton.modrm_opcode);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  DCHECK_EQ(0, entry->skeleton.immediate_bytes);
}

static void emitRegImm(CompilationUnit* cUnit, const X86EncodingMap* entry,
                       uint8_t reg, int imm) {
  if (entry->skeleton.prefix1 != 0) {
    cUnit->codeBuffer.push_back(entry->skeleton.prefix1);
    if (entry->skeleton.prefix2 != 0) {
      cUnit->codeBuffer.push_back(entry->skeleton.prefix2);
    }
  } else {
    DCHECK_EQ(0, entry->skeleton.prefix2);
  }
  if (reg == rAX && entry->skeleton.ax_opcode != 0) {
    cUnit->codeBuffer.push_back(entry->skeleton.ax_opcode);
  } else {
    cUnit->codeBuffer.push_back(entry->skeleton.opcode);
    if (entry->skeleton.opcode == 0x0F) {
      cUnit->codeBuffer.push_back(entry->skeleton.extra_opcode1);
      if (entry->skeleton.extra_opcode1 == 0x38 || entry->skeleton.extra_opcode2 == 0x3A) {
        cUnit->codeBuffer.push_back(entry->skeleton.extra_opcode2);
      } else {
        DCHECK_EQ(0, entry->skeleton.extra_opcode2);
      }
    } else {
      DCHECK_EQ(0, entry->skeleton.extra_opcode1);
      DCHECK_EQ(0, entry->skeleton.extra_opcode2);
    }
    uint8_t modrm = (3 << 6) | (entry->skeleton.modrm_opcode << 3) | reg;
    cUnit->codeBuffer.push_back(modrm);
  }
  switch (entry->skeleton.immediate_bytes) {
    case 1:
      DCHECK(IS_SIMM8(imm));
      cUnit->codeBuffer.push_back(imm & 0xFF);
      break;
    case 2:
      DCHECK(IS_SIMM16(imm));
      cUnit->codeBuffer.push_back(imm & 0xFF);
      cUnit->codeBuffer.push_back((imm >> 8) & 0xFF);
      break;
    case 4:
      cUnit->codeBuffer.push_back(imm & 0xFF);
      cUnit->codeBuffer.push_back((imm >> 8) & 0xFF);
      cUnit->codeBuffer.push_back((imm >> 16) & 0xFF);
      cUnit->codeBuffer.push_back((imm >> 24) & 0xFF);
      break;
    default:
      LOG(FATAL) << "Unexpected immediate bytes (" << entry->skeleton.immediate_bytes
          << ") for instruction: " << entry->name;
      break;
  }
}

static void emitThreadImm(CompilationUnit* cUnit, const X86EncodingMap* entry,
                          int disp, int imm) {
  if (entry->skeleton.prefix1 != 0) {
    cUnit->codeBuffer.push_back(entry->skeleton.prefix1);
    if (entry->skeleton.prefix2 != 0) {
      cUnit->codeBuffer.push_back(entry->skeleton.prefix2);
    }
  } else {
    DCHECK_EQ(0, entry->skeleton.prefix2);
  }
  cUnit->codeBuffer.push_back(entry->skeleton.opcode);
  if (entry->skeleton.opcode == 0x0F) {
    cUnit->codeBuffer.push_back(entry->skeleton.extra_opcode1);
    if (entry->skeleton.extra_opcode1 == 0x38 || entry->skeleton.extra_opcode2 == 0x3A) {
      cUnit->codeBuffer.push_back(entry->skeleton.extra_opcode2);
    } else {
      DCHECK_EQ(0, entry->skeleton.extra_opcode2);
    }
  } else {
    DCHECK_EQ(0, entry->skeleton.extra_opcode1);
    DCHECK_EQ(0, entry->skeleton.extra_opcode2);
  }
  uint8_t modrm = (0 << 6) | (entry->skeleton.modrm_opcode << 3) | rBP;
  cUnit->codeBuffer.push_back(modrm);
  cUnit->codeBuffer.push_back(disp & 0xFF);
  cUnit->codeBuffer.push_back((disp >> 8) & 0xFF);
  cUnit->codeBuffer.push_back((disp >> 16) & 0xFF);
  cUnit->codeBuffer.push_back((disp >> 24) & 0xFF);
  switch (entry->skeleton.immediate_bytes) {
    case 1:
      DCHECK(IS_SIMM8(imm));
      cUnit->codeBuffer.push_back(imm & 0xFF);
      break;
    case 2:
      DCHECK(IS_SIMM16(imm));
      cUnit->codeBuffer.push_back(imm & 0xFF);
      cUnit->codeBuffer.push_back((imm >> 8) & 0xFF);
      break;
    case 4:
      cUnit->codeBuffer.push_back(imm & 0xFF);
      cUnit->codeBuffer.push_back((imm >> 8) & 0xFF);
      cUnit->codeBuffer.push_back((imm >> 16) & 0xFF);
      cUnit->codeBuffer.push_back((imm >> 24) & 0xFF);
      break;
    default:
      LOG(FATAL) << "Unexpected immediate bytes (" << entry->skeleton.immediate_bytes
          << ") for instruction: " << entry->name;
      break;
  }
  DCHECK_EQ(entry->skeleton.ax_opcode, 0);
}

static void emitMovRegImm(CompilationUnit* cUnit, const X86EncodingMap* entry,
                       uint8_t reg, int imm) {
  DCHECK_LT(reg, 8);
  cUnit->codeBuffer.push_back(0xB8 + reg);
  cUnit->codeBuffer.push_back(imm & 0xFF);
  cUnit->codeBuffer.push_back((imm >> 8) & 0xFF);
  cUnit->codeBuffer.push_back((imm >> 16) & 0xFF);
  cUnit->codeBuffer.push_back((imm >> 24) & 0xFF);
}

static void emitShiftRegImm(CompilationUnit* cUnit, const X86EncodingMap* entry,
                       uint8_t reg, int imm) {
  if (entry->skeleton.prefix1 != 0) {
    cUnit->codeBuffer.push_back(entry->skeleton.prefix1);
    if (entry->skeleton.prefix2 != 0) {
      cUnit->codeBuffer.push_back(entry->skeleton.prefix2);
    }
  } else {
    DCHECK_EQ(0, entry->skeleton.prefix2);
  }
  if (imm != 1) {
    cUnit->codeBuffer.push_back(entry->skeleton.opcode);
  } else {
    // Shorter encoding for 1 bit shift
    cUnit->codeBuffer.push_back(entry->skeleton.ax_opcode);
  }
  if (entry->skeleton.opcode == 0x0F) {
    cUnit->codeBuffer.push_back(entry->skeleton.extra_opcode1);
    if (entry->skeleton.extra_opcode1 == 0x38 || entry->skeleton.extra_opcode2 == 0x3A) {
      cUnit->codeBuffer.push_back(entry->skeleton.extra_opcode2);
    } else {
      DCHECK_EQ(0, entry->skeleton.extra_opcode2);
    }
  } else {
    DCHECK_EQ(0, entry->skeleton.extra_opcode1);
    DCHECK_EQ(0, entry->skeleton.extra_opcode2);
  }
  DCHECK_LT(reg, 8);
  uint8_t modrm = (0 << 6) | (entry->skeleton.modrm_opcode << 3) | reg;
  cUnit->codeBuffer.push_back(modrm);
  if (imm != 1) {
    DCHECK_EQ(entry->skeleton.immediate_bytes, 1);
    DCHECK(IS_SIMM8(imm));
    cUnit->codeBuffer.push_back(imm & 0xFF);
  }
}

static void emitJmp(CompilationUnit* cUnit, const X86EncodingMap* entry, int rel) {
  if (entry->opcode == kX86Jmp8) {
    DCHECK(IS_SIMM8(rel));
    cUnit->codeBuffer.push_back(0xEB);
    cUnit->codeBuffer.push_back(rel & 0xFF);
  } else {
    DCHECK(entry->opcode == kX86Jmp32);
    cUnit->codeBuffer.push_back(0xE9);
    cUnit->codeBuffer.push_back(rel & 0xFF);
    cUnit->codeBuffer.push_back((rel >> 8) & 0xFF);
    cUnit->codeBuffer.push_back((rel >> 16) & 0xFF);
    cUnit->codeBuffer.push_back((rel >> 24) & 0xFF);
  }
}

static void emitJcc(CompilationUnit* cUnit, const X86EncodingMap* entry,
                    int rel, uint8_t cc) {
  DCHECK_LT(cc, 16);
  if (entry->opcode == kX86Jcc8) {
    DCHECK(IS_SIMM8(rel));
    cUnit->codeBuffer.push_back(0x70 | cc);
    cUnit->codeBuffer.push_back(rel & 0xFF);
  } else {
    DCHECK(entry->opcode == kX86Jcc32);
    cUnit->codeBuffer.push_back(0x0F);
    cUnit->codeBuffer.push_back(0x80 | cc);
    cUnit->codeBuffer.push_back(rel & 0xFF);
    cUnit->codeBuffer.push_back((rel >> 8) & 0xFF);
    cUnit->codeBuffer.push_back((rel >> 16) & 0xFF);
    cUnit->codeBuffer.push_back((rel >> 24) & 0xFF);
  }
}

static void emitCallMem(CompilationUnit* cUnit, const X86EncodingMap* entry,
                        uint8_t base, int disp) {
  if (entry->skeleton.prefix1 != 0) {
    cUnit->codeBuffer.push_back(entry->skeleton.prefix1);
    if (entry->skeleton.prefix2 != 0) {
      cUnit->codeBuffer.push_back(entry->skeleton.prefix2);
    }
  } else {
    DCHECK_EQ(0, entry->skeleton.prefix2);
  }
  cUnit->codeBuffer.push_back(entry->skeleton.opcode);
  if (entry->skeleton.opcode == 0x0F) {
    cUnit->codeBuffer.push_back(entry->skeleton.extra_opcode1);
    if (entry->skeleton.extra_opcode1 == 0x38 || entry->skeleton.extra_opcode2 == 0x3A) {
      cUnit->codeBuffer.push_back(entry->skeleton.extra_opcode2);
    } else {
      DCHECK_EQ(0, entry->skeleton.extra_opcode2);
    }
  } else {
    DCHECK_EQ(0, entry->skeleton.extra_opcode1);
    DCHECK_EQ(0, entry->skeleton.extra_opcode2);
  }
  uint8_t modrm = (modrmForDisp(disp) << 6) | (entry->skeleton.modrm_opcode << 3) | base;
  cUnit->codeBuffer.push_back(modrm);
  if (base == rSP) {
    // Special SIB for SP base
    cUnit->codeBuffer.push_back(0 << 6 | (rSP << 3) | rSP);
  }
  emitDisp(cUnit, disp);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  DCHECK_EQ(0, entry->skeleton.immediate_bytes);
}

static void emitCallThread(CompilationUnit* cUnit, const X86EncodingMap* entry, int disp) {
  DCHECK_NE(entry->skeleton.prefix1, 0);
  cUnit->codeBuffer.push_back(entry->skeleton.prefix1);
  if (entry->skeleton.prefix2 != 0) {
    cUnit->codeBuffer.push_back(entry->skeleton.prefix2);
  }
  cUnit->codeBuffer.push_back(entry->skeleton.opcode);
  if (entry->skeleton.opcode == 0x0F) {
    cUnit->codeBuffer.push_back(entry->skeleton.extra_opcode1);
    if (entry->skeleton.extra_opcode1 == 0x38 || entry->skeleton.extra_opcode2 == 0x3A) {
      cUnit->codeBuffer.push_back(entry->skeleton.extra_opcode2);
    } else {
      DCHECK_EQ(0, entry->skeleton.extra_opcode2);
    }
  } else {
    DCHECK_EQ(0, entry->skeleton.extra_opcode1);
    DCHECK_EQ(0, entry->skeleton.extra_opcode2);
  }
  uint8_t modrm = (0 << 6) | (entry->skeleton.modrm_opcode << 3) | rBP;
  cUnit->codeBuffer.push_back(modrm);
  cUnit->codeBuffer.push_back(disp & 0xFF);
  cUnit->codeBuffer.push_back((disp >> 8) & 0xFF);
  cUnit->codeBuffer.push_back((disp >> 16) & 0xFF);
  cUnit->codeBuffer.push_back((disp >> 24) & 0xFF);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  DCHECK_EQ(0, entry->skeleton.immediate_bytes);
}

void emitUnimplemented(CompilationUnit* cUnit, const X86EncodingMap* entry, LIR* lir) {
  UNIMPLEMENTED(WARNING) << "encoding for: " << entry->name;
  for (int i = 0; i < oatGetInsnSize(lir); ++i) {
    cUnit->codeBuffer.push_back(0xCC);  // push breakpoint instruction - int 3
  }
}

/*
 * Assemble the LIR into binary instruction format.  Note that we may
 * discover that pc-relative displacements may not fit the selected
 * instruction.  In those cases we will try to substitute a new code
 * sequence or request that the trace be shortened and retried.
 */
AssemblerStatus oatAssembleInstructions(CompilationUnit *cUnit, intptr_t startAddr) {
  LIR *lir;
  AssemblerStatus res = kSuccess;  // Assume success

  for (lir = (LIR *) cUnit->firstLIRInsn; lir; lir = NEXT_LIR(lir)) {
    if (lir->opcode < 0) {
      continue;
    }


    if (lir->flags.isNop) {
      continue;
    }

    if (lir->flags.pcRelFixup) {
      switch (lir->opcode) {
        case kX86Jcc8: {
          LIR *targetLIR = lir->target;
          DCHECK(targetLIR != NULL);
          int delta = 0;
          intptr_t pc;
          if (IS_SIMM8(lir->operands[0])) {
            pc = lir->offset + 2 /* opcode + rel8 */;
          } else {
            pc = lir->offset + 6 /* 2 byte opcode + rel32 */;
          }
          intptr_t target = targetLIR->offset;
          delta = target - pc;
          if (IS_SIMM8(delta) != IS_SIMM8(lir->operands[0])) {
            LOG(INFO) << "Retry for JCC growth at " << lir->offset
                << " delta: " << delta << " old delta: " << lir->operands[0];
            lir->opcode = kX86Jcc32;
            oatSetupResourceMasks(lir);
            res = kRetryAll;
          }
          lir->operands[0] = delta;
          break;
        }
        case kX86Jmp8: {
          LIR *targetLIR = lir->target;
          DCHECK(targetLIR != NULL);
          int delta = 0;
          intptr_t pc;
          if (IS_SIMM8(lir->operands[0])) {
            pc = lir->offset + 2 /* opcode + rel8 */;
          } else {
            pc = lir->offset + 5 /* opcode + rel32 */;
          }
          intptr_t target = targetLIR->offset;
          delta = target - pc;
          if (!(cUnit->disableOpt & (1 << kSafeOptimizations)) && lir->operands[0] == 0) {
            // Useless branch
            lir->flags.isNop = true;
            LOG(INFO) << "Retry for useless branch at " << lir->offset;
            res = kRetryAll;
          } else if (IS_SIMM8(delta) != IS_SIMM8(lir->operands[0])) {
            LOG(INFO) << "Retry for JMP growth at " << lir->offset;
            lir->opcode = kX86Jmp32;
            oatSetupResourceMasks(lir);
            res = kRetryAll;
          }
          lir->operands[0] = delta;
          break;
        }
        default:
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
    const X86EncodingMap *entry = &EncodingMap[lir->opcode];
    size_t starting_cbuf_size = cUnit->codeBuffer.size();
    switch(entry->kind) {
      case kData:  // 4 bytes of data
        cUnit->codeBuffer.push_back(lir->operands[0]);
        break;
      case kNullary:  // 1 byte of opcode
        DCHECK_EQ(0, entry->skeleton.prefix1);
        DCHECK_EQ(0, entry->skeleton.prefix2);
        cUnit->codeBuffer.push_back(entry->skeleton.opcode);
        DCHECK_EQ(0, entry->skeleton.extra_opcode1);
        DCHECK_EQ(0, entry->skeleton.extra_opcode2);
        DCHECK_EQ(0, entry->skeleton.modrm_opcode);
        DCHECK_EQ(0, entry->skeleton.ax_opcode);
        DCHECK_EQ(0, entry->skeleton.immediate_bytes);
        break;
      case kReg:  // lir operands - 0: reg
        emitOpReg(cUnit, entry, lir->operands[0]);
        break;
      case kMem:  // lir operands - 0: base, 1: disp
        emitOpMem(cUnit, entry, lir->operands[0], lir->operands[1]);
        break;
      case kMemReg:  // lir operands - 0: base, 1: disp, 2: reg
        emitMemReg(cUnit, entry, lir->operands[0], lir->operands[1], lir->operands[2]);
        break;
      case kArrayReg:  // lir operands - 0: base, 1: index, 2: scale, 3: disp, 4: reg
        emitArrayReg(cUnit, entry, lir->operands[0], lir->operands[1], lir->operands[2],
                     lir->operands[3], lir->operands[4]);
        break;
      case kRegMem:  // lir operands - 0: reg, 1: base, 2: disp
        emitRegMem(cUnit, entry, lir->operands[0], lir->operands[1], lir->operands[2]);
        break;
      case kRegArray:  // lir operands - 0: reg, 1: base, 2: index, 3: scale, 4: disp
        emitRegArray(cUnit, entry, lir->operands[0], lir->operands[1], lir->operands[2],
                     lir->operands[3], lir->operands[4]);
        break;
      case kRegThread:  // lir operands - 0: reg, 1: disp
        emitRegThread(cUnit, entry, lir->operands[0], lir->operands[1]);
        break;
      case kRegReg:  // lir operands - 0: reg1, 1: reg2
        emitRegReg(cUnit, entry, lir->operands[0], lir->operands[1]);
        break;
      case kRegImm:  // lir operands - 0: reg, 1: immediate
        emitRegImm(cUnit, entry, lir->operands[0], lir->operands[1]);
        break;
      case kThreadImm:  // lir operands - 0: disp, 1: immediate
        emitThreadImm(cUnit, entry, lir->operands[0], lir->operands[1]);
        break;
      case kMovRegImm:  // lir operands - 0: reg, 1: immediate
        emitMovRegImm(cUnit, entry, lir->operands[0], lir->operands[1]);
        break;
      case kShiftRegImm:  // lir operands - 0: reg, 1: immediate
        emitShiftRegImm(cUnit, entry, lir->operands[0], lir->operands[1]);
        break;
      case kJmp:  // lir operands - 0: rel
        emitJmp(cUnit, entry, lir->operands[0]);
        break;
      case kJcc:  // lir operands - 0: rel, 1: CC, target assigned
        emitJcc(cUnit, entry, lir->operands[0], lir->operands[1]);
        break;
      case kCall:
        switch(entry->opcode) {
          case kX86CallM:  // lir operands - 0: base, 1: disp
            emitCallMem(cUnit, entry, lir->operands[0], lir->operands[1]);
            break;
          case kX86CallT:  // lir operands - 0: disp
            emitCallThread(cUnit, entry, lir->operands[0]);
            break;
          default:
            emitUnimplemented(cUnit, entry, lir);
            break;
        }
        break;
      default:
        emitUnimplemented(cUnit, entry, lir);
        break;
    }
    if (entry->kind != kJcc && entry->kind != kJmp) {
      CHECK_EQ(static_cast<size_t>(oatGetInsnSize(lir)),
               cUnit->codeBuffer.size() - starting_cbuf_size)
          << "Instruction size mismatch for entry: " << EncodingMap[lir->opcode].name;
    }
  }
  return res;
}

/*
 * Target-dependent offset assignment.
 * independent.
 */
int oatAssignInsnOffsets(CompilationUnit* cUnit)
{
    LIR* x86LIR;
    int offset = 0;

    for (x86LIR = (LIR *) cUnit->firstLIRInsn;
        x86LIR;
        x86LIR = NEXT_LIR(x86LIR)) {
        x86LIR->offset = offset;
        if (x86LIR->opcode >= 0) {
            if (!x86LIR->flags.isNop) {
                offset += x86LIR->flags.size;
            }
        } else if (x86LIR->opcode == kPseudoPseudoAlign4) {
            if (offset & 0x2) {
                offset += 2;
                x86LIR->operands[0] = 1;
            } else {
                x86LIR->operands[0] = 0;
            }
        }
        /* Pseudo opcodes don't consume space */
    }

    return offset;
}

}  // namespace art
