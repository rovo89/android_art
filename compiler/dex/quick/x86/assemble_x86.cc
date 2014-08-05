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
  { kX8632BitData, kData,    IS_UNARY_OP,            { 0, 0, 0x00, 0, 0, 0, 0, 4, false }, "data",  "0x!0d" },
  { kX86Bkpt,      kNullary, NO_OPERAND | IS_BRANCH, { 0, 0, 0xCC, 0, 0, 0, 0, 0, false }, "int 3", "" },
  { kX86Nop,       kNop,     NO_OPERAND,             { 0, 0, 0x90, 0, 0, 0, 0, 0, false }, "nop",   "" },

#define ENCODING_MAP(opname, mem_use, reg_def, uses_ccodes, \
                     rm8_r8, rm32_r32, \
                     r8_rm8, r32_rm32, \
                     ax8_i8, ax32_i32, \
                     rm8_i8, rm8_i8_modrm, \
                     rm32_i32, rm32_i32_modrm, \
                     rm32_i8, rm32_i8_modrm) \
{ kX86 ## opname ## 8MR, kMemReg,    mem_use | IS_TERTIARY_OP |           REG_USE02  | SETS_CCODES | uses_ccodes, { 0,             0, rm8_r8, 0, 0, 0,            0,      0, true }, #opname "8MR", "[!0r+!1d],!2r" }, \
{ kX86 ## opname ## 8AR, kArrayReg,  mem_use | IS_QUIN_OP     |           REG_USE014 | SETS_CCODES | uses_ccodes, { 0,             0, rm8_r8, 0, 0, 0,            0,      0, true }, #opname "8AR", "[!0r+!1r<<!2d+!3d],!4r" }, \
{ kX86 ## opname ## 8TR, kThreadReg, mem_use | IS_BINARY_OP   |           REG_USE1   | SETS_CCODES | uses_ccodes, { THREAD_PREFIX, 0, rm8_r8, 0, 0, 0,            0,      0, true }, #opname "8TR", "fs:[!0d],!1r" }, \
{ kX86 ## opname ## 8RR, kRegReg,              IS_BINARY_OP   | reg_def | REG_USE01  | SETS_CCODES | uses_ccodes, { 0,             0, r8_rm8, 0, 0, 0,            0,      0, true }, #opname "8RR", "!0r,!1r" }, \
{ kX86 ## opname ## 8RM, kRegMem,    IS_LOAD | IS_TERTIARY_OP | reg_def | REG_USE01  | SETS_CCODES | uses_ccodes, { 0,             0, r8_rm8, 0, 0, 0,            0,      0, true }, #opname "8RM", "!0r,[!1r+!2d]" }, \
{ kX86 ## opname ## 8RA, kRegArray,  IS_LOAD | IS_QUIN_OP     | reg_def | REG_USE012 | SETS_CCODES | uses_ccodes, { 0,             0, r8_rm8, 0, 0, 0,            0,      0, true }, #opname "8RA", "!0r,[!1r+!2r<<!3d+!4d]" }, \
{ kX86 ## opname ## 8RT, kRegThread, IS_LOAD | IS_BINARY_OP   | reg_def | REG_USE0   | SETS_CCODES | uses_ccodes, { THREAD_PREFIX, 0, r8_rm8, 0, 0, 0,            0,      0, true }, #opname "8RT", "!0r,fs:[!1d]" }, \
{ kX86 ## opname ## 8RI, kRegImm,              IS_BINARY_OP   | reg_def | REG_USE0   | SETS_CCODES | uses_ccodes, { 0,             0, rm8_i8, 0, 0, rm8_i8_modrm, ax8_i8, 1, true }, #opname "8RI", "!0r,!1d" }, \
{ kX86 ## opname ## 8MI, kMemImm,    mem_use | IS_TERTIARY_OP |           REG_USE0   | SETS_CCODES | uses_ccodes, { 0,             0, rm8_i8, 0, 0, rm8_i8_modrm, 0,      1, false}, #opname "8MI", "[!0r+!1d],!2d" }, \
{ kX86 ## opname ## 8AI, kArrayImm,  mem_use | IS_QUIN_OP     |           REG_USE01  | SETS_CCODES | uses_ccodes, { 0,             0, rm8_i8, 0, 0, rm8_i8_modrm, 0,      1, false}, #opname "8AI", "[!0r+!1r<<!2d+!3d],!4d" }, \
{ kX86 ## opname ## 8TI, kThreadImm, mem_use | IS_BINARY_OP   |                        SETS_CCODES | uses_ccodes, { THREAD_PREFIX, 0, rm8_i8, 0, 0, rm8_i8_modrm, 0,      1, false}, #opname "8TI", "fs:[!0d],!1d" }, \
  \
{ kX86 ## opname ## 16MR,  kMemReg,    mem_use | IS_TERTIARY_OP |           REG_USE02  | SETS_CCODES | uses_ccodes, { 0x66,          0,    rm32_r32, 0, 0, 0,              0,        0, false }, #opname "16MR", "[!0r+!1d],!2r" }, \
{ kX86 ## opname ## 16AR,  kArrayReg,  mem_use | IS_QUIN_OP     |           REG_USE014 | SETS_CCODES | uses_ccodes, { 0x66,          0,    rm32_r32, 0, 0, 0,              0,        0, false }, #opname "16AR", "[!0r+!1r<<!2d+!3d],!4r" }, \
{ kX86 ## opname ## 16TR,  kThreadReg, mem_use | IS_BINARY_OP   |           REG_USE1   | SETS_CCODES | uses_ccodes, { THREAD_PREFIX, 0x66, rm32_r32, 0, 0, 0,              0,        0, false }, #opname "16TR", "fs:[!0d],!1r" }, \
{ kX86 ## opname ## 16RR,  kRegReg,              IS_BINARY_OP   | reg_def | REG_USE01  | SETS_CCODES | uses_ccodes, { 0x66,          0,    r32_rm32, 0, 0, 0,              0,        0, false }, #opname "16RR", "!0r,!1r" }, \
{ kX86 ## opname ## 16RM,  kRegMem,    IS_LOAD | IS_TERTIARY_OP | reg_def | REG_USE01  | SETS_CCODES | uses_ccodes, { 0x66,          0,    r32_rm32, 0, 0, 0,              0,        0, false }, #opname "16RM", "!0r,[!1r+!2d]" }, \
{ kX86 ## opname ## 16RA,  kRegArray,  IS_LOAD | IS_QUIN_OP     | reg_def | REG_USE012 | SETS_CCODES | uses_ccodes, { 0x66,          0,    r32_rm32, 0, 0, 0,              0,        0, false }, #opname "16RA", "!0r,[!1r+!2r<<!3d+!4d]" }, \
{ kX86 ## opname ## 16RT,  kRegThread, IS_LOAD | IS_BINARY_OP   | reg_def | REG_USE0   | SETS_CCODES | uses_ccodes, { THREAD_PREFIX, 0x66, r32_rm32, 0, 0, 0,              0,        0, false }, #opname "16RT", "!0r,fs:[!1d]" }, \
{ kX86 ## opname ## 16RI,  kRegImm,              IS_BINARY_OP   | reg_def | REG_USE0   | SETS_CCODES | uses_ccodes, { 0x66,          0,    rm32_i32, 0, 0, rm32_i32_modrm, ax32_i32, 2, false }, #opname "16RI", "!0r,!1d" }, \
{ kX86 ## opname ## 16MI,  kMemImm,    mem_use | IS_TERTIARY_OP |           REG_USE0   | SETS_CCODES | uses_ccodes, { 0x66,          0,    rm32_i32, 0, 0, rm32_i32_modrm, 0,        2, false }, #opname "16MI", "[!0r+!1d],!2d" }, \
{ kX86 ## opname ## 16AI,  kArrayImm,  mem_use | IS_QUIN_OP     |           REG_USE01  | SETS_CCODES | uses_ccodes, { 0x66,          0,    rm32_i32, 0, 0, rm32_i32_modrm, 0,        2, false }, #opname "16AI", "[!0r+!1r<<!2d+!3d],!4d" }, \
{ kX86 ## opname ## 16TI,  kThreadImm, mem_use | IS_BINARY_OP   |                        SETS_CCODES | uses_ccodes, { THREAD_PREFIX, 0x66, rm32_i32, 0, 0, rm32_i32_modrm, 0,        2, false }, #opname "16TI", "fs:[!0d],!1d" }, \
{ kX86 ## opname ## 16RI8, kRegImm,              IS_BINARY_OP   | reg_def | REG_USE0   | SETS_CCODES | uses_ccodes, { 0x66,          0,    rm32_i8,  0, 0, rm32_i8_modrm,  0,        1, false }, #opname "16RI8", "!0r,!1d" }, \
{ kX86 ## opname ## 16MI8, kMemImm,    mem_use | IS_TERTIARY_OP |           REG_USE0   | SETS_CCODES | uses_ccodes, { 0x66,          0,    rm32_i8,  0, 0, rm32_i8_modrm,  0,        1, false }, #opname "16MI8", "[!0r+!1d],!2d" }, \
{ kX86 ## opname ## 16AI8, kArrayImm,  mem_use | IS_QUIN_OP     |           REG_USE01  | SETS_CCODES | uses_ccodes, { 0x66,          0,    rm32_i8,  0, 0, rm32_i8_modrm,  0,        1, false }, #opname "16AI8", "[!0r+!1r<<!2d+!3d],!4d" }, \
{ kX86 ## opname ## 16TI8, kThreadImm, mem_use | IS_BINARY_OP   |                        SETS_CCODES | uses_ccodes, { THREAD_PREFIX, 0x66, rm32_i8,  0, 0, rm32_i8_modrm,  0,        1, false }, #opname "16TI8", "fs:[!0d],!1d" }, \
  \
{ kX86 ## opname ## 32MR,  kMemReg,    mem_use | IS_TERTIARY_OP |           REG_USE02  | SETS_CCODES | uses_ccodes, { 0,             0, rm32_r32, 0, 0, 0,              0,        0, false }, #opname "32MR", "[!0r+!1d],!2r" }, \
{ kX86 ## opname ## 32AR,  kArrayReg,  mem_use | IS_QUIN_OP     |           REG_USE014 | SETS_CCODES | uses_ccodes, { 0,             0, rm32_r32, 0, 0, 0,              0,        0, false }, #opname "32AR", "[!0r+!1r<<!2d+!3d],!4r" }, \
{ kX86 ## opname ## 32TR,  kThreadReg, mem_use | IS_BINARY_OP   |           REG_USE1   | SETS_CCODES | uses_ccodes, { THREAD_PREFIX, 0, rm32_r32, 0, 0, 0,              0,        0, false }, #opname "32TR", "fs:[!0d],!1r" }, \
{ kX86 ## opname ## 32RR,  kRegReg,              IS_BINARY_OP   | reg_def | REG_USE01  | SETS_CCODES | uses_ccodes, { 0,             0, r32_rm32, 0, 0, 0,              0,        0, false }, #opname "32RR", "!0r,!1r" }, \
{ kX86 ## opname ## 32RM,  kRegMem,    IS_LOAD | IS_TERTIARY_OP | reg_def | REG_USE01  | SETS_CCODES | uses_ccodes, { 0,             0, r32_rm32, 0, 0, 0,              0,        0, false }, #opname "32RM", "!0r,[!1r+!2d]" }, \
{ kX86 ## opname ## 32RA,  kRegArray,  IS_LOAD | IS_QUIN_OP     | reg_def | REG_USE012 | SETS_CCODES | uses_ccodes, { 0,             0, r32_rm32, 0, 0, 0,              0,        0, false }, #opname "32RA", "!0r,[!1r+!2r<<!3d+!4d]" }, \
{ kX86 ## opname ## 32RT,  kRegThread, IS_LOAD | IS_BINARY_OP   | reg_def | REG_USE0   | SETS_CCODES | uses_ccodes, { THREAD_PREFIX, 0, r32_rm32, 0, 0, 0,              0,        0, false }, #opname "32RT", "!0r,fs:[!1d]" }, \
{ kX86 ## opname ## 32RI,  kRegImm,              IS_BINARY_OP   | reg_def | REG_USE0   | SETS_CCODES | uses_ccodes, { 0,             0, rm32_i32, 0, 0, rm32_i32_modrm, ax32_i32, 4, false }, #opname "32RI", "!0r,!1d" }, \
{ kX86 ## opname ## 32MI,  kMemImm,    mem_use | IS_TERTIARY_OP |           REG_USE0   | SETS_CCODES | uses_ccodes, { 0,             0, rm32_i32, 0, 0, rm32_i32_modrm, 0,        4, false }, #opname "32MI", "[!0r+!1d],!2d" }, \
{ kX86 ## opname ## 32AI,  kArrayImm,  mem_use | IS_QUIN_OP     |           REG_USE01  | SETS_CCODES | uses_ccodes, { 0,             0, rm32_i32, 0, 0, rm32_i32_modrm, 0,        4, false }, #opname "32AI", "[!0r+!1r<<!2d+!3d],!4d" }, \
{ kX86 ## opname ## 32TI,  kThreadImm, mem_use | IS_BINARY_OP   |                        SETS_CCODES | uses_ccodes, { THREAD_PREFIX, 0, rm32_i32, 0, 0, rm32_i32_modrm, 0,        4, false }, #opname "32TI", "fs:[!0d],!1d" }, \
{ kX86 ## opname ## 32RI8, kRegImm,              IS_BINARY_OP   | reg_def | REG_USE0   | SETS_CCODES | uses_ccodes, { 0,             0, rm32_i8,  0, 0, rm32_i8_modrm,  0,        1, false }, #opname "32RI8", "!0r,!1d" }, \
{ kX86 ## opname ## 32MI8, kMemImm,    mem_use | IS_TERTIARY_OP |           REG_USE0   | SETS_CCODES | uses_ccodes, { 0,             0, rm32_i8,  0, 0, rm32_i8_modrm,  0,        1, false }, #opname "32MI8", "[!0r+!1d],!2d" }, \
{ kX86 ## opname ## 32AI8, kArrayImm,  mem_use | IS_QUIN_OP     |           REG_USE01  | SETS_CCODES | uses_ccodes, { 0,             0, rm32_i8,  0, 0, rm32_i8_modrm,  0,        1, false }, #opname "32AI8", "[!0r+!1r<<!2d+!3d],!4d" }, \
{ kX86 ## opname ## 32TI8, kThreadImm, mem_use | IS_BINARY_OP   |                        SETS_CCODES | uses_ccodes, { THREAD_PREFIX, 0, rm32_i8,  0, 0, rm32_i8_modrm,  0,        1, false }, #opname "32TI8", "fs:[!0d],!1d" }, \
  \
{ kX86 ## opname ## 64MR,  kMemReg,    mem_use | IS_TERTIARY_OP |           REG_USE02  | SETS_CCODES | uses_ccodes, { REX_W,             0, rm32_r32, 0, 0, 0,              0,        0, false }, #opname "64MR", "[!0r+!1d],!2r" }, \
{ kX86 ## opname ## 64AR,  kArrayReg,  mem_use | IS_QUIN_OP     |           REG_USE014 | SETS_CCODES | uses_ccodes, { REX_W,             0, rm32_r32, 0, 0, 0,              0,        0, false }, #opname "64AR", "[!0r+!1r<<!2d+!3d],!4r" }, \
{ kX86 ## opname ## 64TR,  kThreadReg, mem_use | IS_BINARY_OP   |           REG_USE1   | SETS_CCODES | uses_ccodes, { THREAD_PREFIX, REX_W, rm32_r32, 0, 0, 0,              0,        0, false }, #opname "64TR", "fs:[!0d],!1r" }, \
{ kX86 ## opname ## 64RR,  kRegReg,              IS_BINARY_OP   | reg_def | REG_USE01  | SETS_CCODES | uses_ccodes, { REX_W,             0, r32_rm32, 0, 0, 0,              0,        0, false }, #opname "64RR", "!0r,!1r" }, \
{ kX86 ## opname ## 64RM,  kRegMem,    IS_LOAD | IS_TERTIARY_OP | reg_def | REG_USE01  | SETS_CCODES | uses_ccodes, { REX_W,             0, r32_rm32, 0, 0, 0,              0,        0, false }, #opname "64RM", "!0r,[!1r+!2d]" }, \
{ kX86 ## opname ## 64RA,  kRegArray,  IS_LOAD | IS_QUIN_OP     | reg_def | REG_USE012 | SETS_CCODES | uses_ccodes, { REX_W,             0, r32_rm32, 0, 0, 0,              0,        0, false }, #opname "64RA", "!0r,[!1r+!2r<<!3d+!4d]" }, \
{ kX86 ## opname ## 64RT,  kRegThread, IS_LOAD | IS_BINARY_OP   | reg_def | REG_USE0   | SETS_CCODES | uses_ccodes, { THREAD_PREFIX, REX_W, r32_rm32, 0, 0, 0,              0,        0, false }, #opname "64RT", "!0r,fs:[!1d]" }, \
{ kX86 ## opname ## 64RI,  kRegImm,              IS_BINARY_OP   | reg_def | REG_USE0   | SETS_CCODES | uses_ccodes, { REX_W,             0, rm32_i32, 0, 0, rm32_i32_modrm, ax32_i32, 4, false }, #opname "64RI", "!0r,!1d" }, \
{ kX86 ## opname ## 64MI,  kMemImm,    mem_use | IS_TERTIARY_OP |           REG_USE0   | SETS_CCODES | uses_ccodes, { REX_W,             0, rm32_i32, 0, 0, rm32_i32_modrm, 0,        4, false }, #opname "64MI", "[!0r+!1d],!2d" }, \
{ kX86 ## opname ## 64AI,  kArrayImm,  mem_use | IS_QUIN_OP     |           REG_USE01  | SETS_CCODES | uses_ccodes, { REX_W,             0, rm32_i32, 0, 0, rm32_i32_modrm, 0,        4, false }, #opname "64AI", "[!0r+!1r<<!2d+!3d],!4d" }, \
{ kX86 ## opname ## 64TI,  kThreadImm, mem_use | IS_BINARY_OP   |                        SETS_CCODES | uses_ccodes, { THREAD_PREFIX, REX_W, rm32_i32, 0, 0, rm32_i32_modrm, 0,        4, false }, #opname "64TI", "fs:[!0d],!1d" }, \
{ kX86 ## opname ## 64RI8, kRegImm,              IS_BINARY_OP   | reg_def | REG_USE0   | SETS_CCODES | uses_ccodes, { REX_W,             0, rm32_i8,  0, 0, rm32_i8_modrm,  0,        1, false }, #opname "64RI8", "!0r,!1d" }, \
{ kX86 ## opname ## 64MI8, kMemImm,    mem_use | IS_TERTIARY_OP |           REG_USE0   | SETS_CCODES | uses_ccodes, { REX_W,             0, rm32_i8,  0, 0, rm32_i8_modrm,  0,        1, false }, #opname "64MI8", "[!0r+!1d],!2d" }, \
{ kX86 ## opname ## 64AI8, kArrayImm,  mem_use | IS_QUIN_OP     |           REG_USE01  | SETS_CCODES | uses_ccodes, { REX_W,             0, rm32_i8,  0, 0, rm32_i8_modrm,  0,        1, false }, #opname "64AI8", "[!0r+!1r<<!2d+!3d],!4d" }, \
{ kX86 ## opname ## 64TI8, kThreadImm, mem_use | IS_BINARY_OP   |                        SETS_CCODES | uses_ccodes, { THREAD_PREFIX, REX_W, rm32_i8,  0, 0, rm32_i8_modrm,  0,        1, false }, #opname "64TI8", "fs:[!0d],!1d" }

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

  { kX86Imul16RRI,   kRegRegImm,             IS_TERTIARY_OP | REG_DEF0_USE1  | SETS_CCODES, { 0x66, 0, 0x69, 0, 0, 0, 0, 2, false }, "Imul16RRI", "!0r,!1r,!2d" },
  { kX86Imul16RMI,   kRegMemImm,   IS_LOAD | IS_QUAD_OP     | REG_DEF0_USE1  | SETS_CCODES, { 0x66, 0, 0x69, 0, 0, 0, 0, 2, false }, "Imul16RMI", "!0r,[!1r+!2d],!3d" },
  { kX86Imul16RAI,   kRegArrayImm, IS_LOAD | IS_SEXTUPLE_OP | REG_DEF0_USE12 | SETS_CCODES, { 0x66, 0, 0x69, 0, 0, 0, 0, 2, false }, "Imul16RAI", "!0r,[!1r+!2r<<!3d+!4d],!5d" },

  { kX86Imul32RRI,   kRegRegImm,             IS_TERTIARY_OP | REG_DEF0_USE1  | SETS_CCODES, { 0, 0, 0x69, 0, 0, 0, 0, 4, false }, "Imul32RRI", "!0r,!1r,!2d" },
  { kX86Imul32RMI,   kRegMemImm,   IS_LOAD | IS_QUAD_OP     | REG_DEF0_USE1  | SETS_CCODES, { 0, 0, 0x69, 0, 0, 0, 0, 4, false }, "Imul32RMI", "!0r,[!1r+!2d],!3d" },
  { kX86Imul32RAI,   kRegArrayImm, IS_LOAD | IS_SEXTUPLE_OP | REG_DEF0_USE12 | SETS_CCODES, { 0, 0, 0x69, 0, 0, 0, 0, 4, false }, "Imul32RAI", "!0r,[!1r+!2r<<!3d+!4d],!5d" },
  { kX86Imul32RRI8,  kRegRegImm,             IS_TERTIARY_OP | REG_DEF0_USE1  | SETS_CCODES, { 0, 0, 0x6B, 0, 0, 0, 0, 1, false }, "Imul32RRI8", "!0r,!1r,!2d" },
  { kX86Imul32RMI8,  kRegMemImm,   IS_LOAD | IS_QUAD_OP     | REG_DEF0_USE1  | SETS_CCODES, { 0, 0, 0x6B, 0, 0, 0, 0, 1, false }, "Imul32RMI8", "!0r,[!1r+!2d],!3d" },
  { kX86Imul32RAI8,  kRegArrayImm, IS_LOAD | IS_SEXTUPLE_OP | REG_DEF0_USE12 | SETS_CCODES, { 0, 0, 0x6B, 0, 0, 0, 0, 1, false }, "Imul32RAI8", "!0r,[!1r+!2r<<!3d+!4d],!5d" },

  { kX86Imul64RRI,   kRegRegImm,             IS_TERTIARY_OP | REG_DEF0_USE1  | SETS_CCODES, { REX_W, 0, 0x69, 0, 0, 0, 0, 4, false }, "Imul64RRI", "!0r,!1r,!2d" },
  { kX86Imul64RMI,   kRegMemImm,   IS_LOAD | IS_QUAD_OP     | REG_DEF0_USE1  | SETS_CCODES, { REX_W, 0, 0x69, 0, 0, 0, 0, 4, false }, "Imul64RMI", "!0r,[!1r+!2d],!3d" },
  { kX86Imul64RAI,   kRegArrayImm, IS_LOAD | IS_SEXTUPLE_OP | REG_DEF0_USE12 | SETS_CCODES, { REX_W, 0, 0x69, 0, 0, 0, 0, 4, false }, "Imul64RAI", "!0r,[!1r+!2r<<!3d+!4d],!5d" },
  { kX86Imul64RRI8,  kRegRegImm,             IS_TERTIARY_OP | REG_DEF0_USE1  | SETS_CCODES, { REX_W, 0, 0x6B, 0, 0, 0, 0, 1, false }, "Imul64RRI8", "!0r,!1r,!2d" },
  { kX86Imul64RMI8,  kRegMemImm,   IS_LOAD | IS_QUAD_OP     | REG_DEF0_USE1  | SETS_CCODES, { REX_W, 0, 0x6B, 0, 0, 0, 0, 1, false }, "Imul64RMI8", "!0r,[!1r+!2d],!3d" },
  { kX86Imul64RAI8,  kRegArrayImm, IS_LOAD | IS_SEXTUPLE_OP | REG_DEF0_USE12 | SETS_CCODES, { REX_W, 0, 0x6B, 0, 0, 0, 0, 1, false }, "Imul64RAI8", "!0r,[!1r+!2r<<!3d+!4d],!5d" },

  { kX86Mov8MR, kMemReg,    IS_STORE | IS_TERTIARY_OP | REG_USE02,      { 0,             0, 0x88, 0, 0, 0, 0, 0, true }, "Mov8MR", "[!0r+!1d],!2r" },
  { kX86Mov8AR, kArrayReg,  IS_STORE | IS_QUIN_OP     | REG_USE014,     { 0,             0, 0x88, 0, 0, 0, 0, 0, true }, "Mov8AR", "[!0r+!1r<<!2d+!3d],!4r" },
  { kX86Mov8TR, kThreadReg, IS_STORE | IS_BINARY_OP   | REG_USE1,       { THREAD_PREFIX, 0, 0x88, 0, 0, 0, 0, 0, true }, "Mov8TR", "fs:[!0d],!1r" },
  { kX86Mov8RR, kRegReg,               IS_BINARY_OP   | REG_DEF0_USE1,  { 0,             0, 0x8A, 0, 0, 0, 0, 0, true }, "Mov8RR", "!0r,!1r" },
  { kX86Mov8RM, kRegMem,    IS_LOAD  | IS_TERTIARY_OP | REG_DEF0_USE1,  { 0,             0, 0x8A, 0, 0, 0, 0, 0, true }, "Mov8RM", "!0r,[!1r+!2d]" },
  { kX86Mov8RA, kRegArray,  IS_LOAD  | IS_QUIN_OP     | REG_DEF0_USE12, { 0,             0, 0x8A, 0, 0, 0, 0, 0, true }, "Mov8RA", "!0r,[!1r+!2r<<!3d+!4d]" },
  { kX86Mov8RT, kRegThread, IS_LOAD  | IS_BINARY_OP   | REG_DEF0,       { THREAD_PREFIX, 0, 0x8A, 0, 0, 0, 0, 0, true }, "Mov8RT", "!0r,fs:[!1d]" },
  { kX86Mov8RI, kMovRegImm,            IS_BINARY_OP   | REG_DEF0,       { 0,             0, 0xB0, 0, 0, 0, 0, 1, true }, "Mov8RI", "!0r,!1d" },
  { kX86Mov8MI, kMemImm,    IS_STORE | IS_TERTIARY_OP | REG_USE0,       { 0,             0, 0xC6, 0, 0, 0, 0, 1, false}, "Mov8MI", "[!0r+!1d],!2d" },
  { kX86Mov8AI, kArrayImm,  IS_STORE | IS_QUIN_OP     | REG_USE01,      { 0,             0, 0xC6, 0, 0, 0, 0, 1, false}, "Mov8AI", "[!0r+!1r<<!2d+!3d],!4d" },
  { kX86Mov8TI, kThreadImm, IS_STORE | IS_BINARY_OP,                    { THREAD_PREFIX, 0, 0xC6, 0, 0, 0, 0, 1, false}, "Mov8TI", "fs:[!0d],!1d" },

  { kX86Mov16MR, kMemReg,    IS_STORE | IS_TERTIARY_OP | REG_USE02,      { 0x66,          0,    0x89, 0, 0, 0, 0, 0, false }, "Mov16MR", "[!0r+!1d],!2r" },
  { kX86Mov16AR, kArrayReg,  IS_STORE | IS_QUIN_OP     | REG_USE014,     { 0x66,          0,    0x89, 0, 0, 0, 0, 0, false }, "Mov16AR", "[!0r+!1r<<!2d+!3d],!4r" },
  { kX86Mov16TR, kThreadReg, IS_STORE | IS_BINARY_OP   | REG_USE1,       { THREAD_PREFIX, 0x66, 0x89, 0, 0, 0, 0, 0, false }, "Mov16TR", "fs:[!0d],!1r" },
  { kX86Mov16RR, kRegReg,               IS_BINARY_OP   | REG_DEF0_USE1,  { 0x66,          0,    0x8B, 0, 0, 0, 0, 0, false }, "Mov16RR", "!0r,!1r" },
  { kX86Mov16RM, kRegMem,    IS_LOAD  | IS_TERTIARY_OP | REG_DEF0_USE1,  { 0x66,          0,    0x8B, 0, 0, 0, 0, 0, false }, "Mov16RM", "!0r,[!1r+!2d]" },
  { kX86Mov16RA, kRegArray,  IS_LOAD  | IS_QUIN_OP     | REG_DEF0_USE12, { 0x66,          0,    0x8B, 0, 0, 0, 0, 0, false }, "Mov16RA", "!0r,[!1r+!2r<<!3d+!4d]" },
  { kX86Mov16RT, kRegThread, IS_LOAD  | IS_BINARY_OP   | REG_DEF0,       { THREAD_PREFIX, 0x66, 0x8B, 0, 0, 0, 0, 0, false }, "Mov16RT", "!0r,fs:[!1d]" },
  { kX86Mov16RI, kMovRegImm,            IS_BINARY_OP   | REG_DEF0,       { 0x66,          0,    0xB8, 0, 0, 0, 0, 2, false }, "Mov16RI", "!0r,!1d" },
  { kX86Mov16MI, kMemImm,    IS_STORE | IS_TERTIARY_OP | REG_USE0,       { 0x66,          0,    0xC7, 0, 0, 0, 0, 2, false }, "Mov16MI", "[!0r+!1d],!2d" },
  { kX86Mov16AI, kArrayImm,  IS_STORE | IS_QUIN_OP     | REG_USE01,      { 0x66,          0,    0xC7, 0, 0, 0, 0, 2, false }, "Mov16AI", "[!0r+!1r<<!2d+!3d],!4d" },
  { kX86Mov16TI, kThreadImm, IS_STORE | IS_BINARY_OP,                    { THREAD_PREFIX, 0x66, 0xC7, 0, 0, 0, 0, 2, false }, "Mov16TI", "fs:[!0d],!1d" },

  { kX86Mov32MR, kMemReg,    IS_STORE | IS_TERTIARY_OP | REG_USE02,      { 0,             0, 0x89, 0, 0, 0, 0, 0, false }, "Mov32MR", "[!0r+!1d],!2r" },
  { kX86Mov32AR, kArrayReg,  IS_STORE | IS_QUIN_OP     | REG_USE014,     { 0,             0, 0x89, 0, 0, 0, 0, 0, false }, "Mov32AR", "[!0r+!1r<<!2d+!3d],!4r" },
  { kX86Mov32TR, kThreadReg, IS_STORE | IS_BINARY_OP   | REG_USE1,       { THREAD_PREFIX, 0, 0x89, 0, 0, 0, 0, 0, false }, "Mov32TR", "fs:[!0d],!1r" },
  { kX86Mov32RR, kRegReg,               IS_BINARY_OP   | REG_DEF0_USE1,  { 0,             0, 0x8B, 0, 0, 0, 0, 0, false }, "Mov32RR", "!0r,!1r" },
  { kX86Mov32RM, kRegMem,    IS_LOAD  | IS_TERTIARY_OP | REG_DEF0_USE1,  { 0,             0, 0x8B, 0, 0, 0, 0, 0, false }, "Mov32RM", "!0r,[!1r+!2d]" },
  { kX86Mov32RA, kRegArray,  IS_LOAD  | IS_QUIN_OP     | REG_DEF0_USE12, { 0,             0, 0x8B, 0, 0, 0, 0, 0, false }, "Mov32RA", "!0r,[!1r+!2r<<!3d+!4d]" },
  { kX86Mov32RT, kRegThread, IS_LOAD  | IS_BINARY_OP   | REG_DEF0,       { THREAD_PREFIX, 0, 0x8B, 0, 0, 0, 0, 0, false }, "Mov32RT", "!0r,fs:[!1d]" },
  { kX86Mov32RI, kMovRegImm,            IS_BINARY_OP   | REG_DEF0,       { 0,             0, 0xB8, 0, 0, 0, 0, 4, false }, "Mov32RI", "!0r,!1d" },
  { kX86Mov32MI, kMemImm,    IS_STORE | IS_TERTIARY_OP | REG_USE0,       { 0,             0, 0xC7, 0, 0, 0, 0, 4, false }, "Mov32MI", "[!0r+!1d],!2d" },
  { kX86Mov32AI, kArrayImm,  IS_STORE | IS_QUIN_OP     | REG_USE01,      { 0,             0, 0xC7, 0, 0, 0, 0, 4, false }, "Mov32AI", "[!0r+!1r<<!2d+!3d],!4d" },
  { kX86Mov32TI, kThreadImm, IS_STORE | IS_BINARY_OP,                    { THREAD_PREFIX, 0, 0xC7, 0, 0, 0, 0, 4, false }, "Mov32TI", "fs:[!0d],!1d" },

  { kX86Lea32RM, kRegMem, IS_TERTIARY_OP | IS_LOAD | REG_DEF0_USE1,      { 0,             0, 0x8D, 0, 0, 0, 0, 0, false }, "Lea32RM", "!0r,[!1r+!2d]" },
  { kX86Lea32RA, kRegArray, IS_QUIN_OP | REG_DEF0_USE12,                 { 0,             0, 0x8D, 0, 0, 0, 0, 0, false }, "Lea32RA", "!0r,[!1r+!2r<<!3d+!4d]" },

  { kX86Mov64MR, kMemReg,    IS_STORE | IS_TERTIARY_OP | REG_USE02,      { REX_W,             0, 0x89, 0, 0, 0, 0, 0, false }, "Mov64MR", "[!0r+!1d],!2r" },
  { kX86Mov64AR, kArrayReg,  IS_STORE | IS_QUIN_OP     | REG_USE014,     { REX_W,             0, 0x89, 0, 0, 0, 0, 0, false }, "Mov64AR", "[!0r+!1r<<!2d+!3d],!4r" },
  { kX86Mov64TR, kThreadReg, IS_STORE | IS_BINARY_OP   | REG_USE1,       { THREAD_PREFIX, REX_W, 0x89, 0, 0, 0, 0, 0, false }, "Mov64TR", "fs:[!0d],!1r" },
  { kX86Mov64RR, kRegReg,               IS_BINARY_OP   | REG_DEF0_USE1,  { REX_W,             0, 0x8B, 0, 0, 0, 0, 0, false }, "Mov64RR", "!0r,!1r" },
  { kX86Mov64RM, kRegMem,    IS_LOAD  | IS_TERTIARY_OP | REG_DEF0_USE1,  { REX_W,             0, 0x8B, 0, 0, 0, 0, 0, false }, "Mov64RM", "!0r,[!1r+!2d]" },
  { kX86Mov64RA, kRegArray,  IS_LOAD  | IS_QUIN_OP     | REG_DEF0_USE12, { REX_W,             0, 0x8B, 0, 0, 0, 0, 0, false }, "Mov64RA", "!0r,[!1r+!2r<<!3d+!4d]" },
  { kX86Mov64RT, kRegThread, IS_LOAD  | IS_BINARY_OP   | REG_DEF0,       { THREAD_PREFIX, REX_W, 0x8B, 0, 0, 0, 0, 0, false }, "Mov64RT", "!0r,fs:[!1d]" },
  { kX86Mov64RI32, kRegImm,             IS_BINARY_OP   | REG_DEF0,       { REX_W,             0, 0xC7, 0, 0, 0, 0, 4, false }, "Mov64RI32", "!0r,!1d" },
  { kX86Mov64RI64, kMovRegQuadImm,      IS_TERTIARY_OP | REG_DEF0,       { REX_W,             0, 0xB8, 0, 0, 0, 0, 8, false }, "Mov64RI64", "!0r,!1q" },
  { kX86Mov64MI, kMemImm,    IS_STORE | IS_TERTIARY_OP | REG_USE0,       { REX_W,             0, 0xC7, 0, 0, 0, 0, 4, false }, "Mov64MI", "[!0r+!1d],!2d" },
  { kX86Mov64AI, kArrayImm,  IS_STORE | IS_QUIN_OP     | REG_USE01,      { REX_W,             0, 0xC7, 0, 0, 0, 0, 4, false }, "Mov64AI", "[!0r+!1r<<!2d+!3d],!4d" },
  { kX86Mov64TI, kThreadImm, IS_STORE | IS_BINARY_OP,                    { THREAD_PREFIX, REX_W, 0xC7, 0, 0, 0, 0, 4, false }, "Mov64TI", "fs:[!0d],!1d" },

  { kX86Lea64RM, kRegMem, IS_TERTIARY_OP | IS_LOAD | REG_DEF0_USE1,      { REX_W,             0, 0x8D, 0, 0, 0, 0, 0, false }, "Lea64RM", "!0r,[!1r+!2d]" },
  { kX86Lea64RA, kRegArray, IS_QUIN_OP | REG_DEF0_USE12,                 { REX_W,             0, 0x8D, 0, 0, 0, 0, 0, false }, "Lea64RA", "!0r,[!1r+!2r<<!3d+!4d]" },

  { kX86Cmov32RRC, kRegRegCond, IS_TERTIARY_OP | REG_DEF0_USE01 | USES_CCODES, { 0,     0, 0x0F, 0x40, 0, 0, 0, 0, false }, "Cmovcc32RR", "!2c !0r,!1r" },
  { kX86Cmov64RRC, kRegRegCond, IS_TERTIARY_OP | REG_DEF0_USE01 | USES_CCODES, { REX_W, 0, 0x0F, 0x40, 0, 0, 0, 0, false }, "Cmovcc64RR", "!2c !0r,!1r" },

  { kX86Cmov32RMC, kRegMemCond, IS_QUAD_OP | IS_LOAD | REG_DEF0_USE01 | USES_CCODES, { 0,     0, 0x0F, 0x40, 0, 0, 0, 0, false }, "Cmovcc32RM", "!3c !0r,[!1r+!2d]" },
  { kX86Cmov64RMC, kRegMemCond, IS_QUAD_OP | IS_LOAD | REG_DEF0_USE01 | USES_CCODES, { REX_W, 0, 0x0F, 0x40, 0, 0, 0, 0, false }, "Cmovcc64RM", "!3c !0r,[!1r+!2d]" },

#define SHIFT_ENCODING_MAP(opname, modrm_opcode) \
{ kX86 ## opname ## 8RI, kShiftRegImm,                        IS_BINARY_OP   | REG_DEF0_USE0 |            SETS_CCODES, { 0,    0, 0xC0, 0, 0, modrm_opcode, 0xD1, 1, true }, #opname "8RI", "!0r,!1d" }, \
{ kX86 ## opname ## 8MI, kShiftMemImm,   IS_LOAD | IS_STORE | IS_TERTIARY_OP | REG_USE0      |            SETS_CCODES, { 0,    0, 0xC0, 0, 0, modrm_opcode, 0xD1, 1, true }, #opname "8MI", "[!0r+!1d],!2d" }, \
{ kX86 ## opname ## 8AI, kShiftArrayImm, IS_LOAD | IS_STORE | IS_QUIN_OP     | REG_USE01     |            SETS_CCODES, { 0,    0, 0xC0, 0, 0, modrm_opcode, 0xD1, 1, true }, #opname "8AI", "[!0r+!1r<<!2d+!3d],!4d" }, \
{ kX86 ## opname ## 8RC, kShiftRegCl,                         IS_BINARY_OP   | REG_DEF0_USE0 | REG_USEC | SETS_CCODES, { 0,    0, 0xD2, 0, 0, modrm_opcode, 0,    1, true }, #opname "8RC", "!0r,cl" }, \
{ kX86 ## opname ## 8MC, kShiftMemCl,    IS_LOAD | IS_STORE | IS_TERTIARY_OP | REG_USE0      | REG_USEC | SETS_CCODES, { 0,    0, 0xD2, 0, 0, modrm_opcode, 0,    1, true }, #opname "8MC", "[!0r+!1d],cl" }, \
{ kX86 ## opname ## 8AC, kShiftArrayCl,  IS_LOAD | IS_STORE | IS_QUIN_OP     | REG_USE01     | REG_USEC | SETS_CCODES, { 0,    0, 0xD2, 0, 0, modrm_opcode, 0,    1, true }, #opname "8AC", "[!0r+!1r<<!2d+!3d],cl" }, \
  \
{ kX86 ## opname ## 16RI, kShiftRegImm,                        IS_BINARY_OP   | REG_DEF0_USE0 |            SETS_CCODES, { 0x66, 0, 0xC1, 0, 0, modrm_opcode, 0xD1, 1, false }, #opname "16RI", "!0r,!1d" }, \
{ kX86 ## opname ## 16MI, kShiftMemImm,   IS_LOAD | IS_STORE | IS_TERTIARY_OP | REG_USE0      |            SETS_CCODES, { 0x66, 0, 0xC1, 0, 0, modrm_opcode, 0xD1, 1, false }, #opname "16MI", "[!0r+!1d],!2d" }, \
{ kX86 ## opname ## 16AI, kShiftArrayImm, IS_LOAD | IS_STORE | IS_QUIN_OP     | REG_USE01     |            SETS_CCODES, { 0x66, 0, 0xC1, 0, 0, modrm_opcode, 0xD1, 1, false }, #opname "16AI", "[!0r+!1r<<!2d+!3d],!4d" }, \
{ kX86 ## opname ## 16RC, kShiftRegCl,                         IS_BINARY_OP   | REG_DEF0_USE0 | REG_USEC | SETS_CCODES, { 0x66, 0, 0xD3, 0, 0, modrm_opcode, 0,    1, false }, #opname "16RC", "!0r,cl" }, \
{ kX86 ## opname ## 16MC, kShiftMemCl,    IS_LOAD | IS_STORE | IS_TERTIARY_OP | REG_USE0      | REG_USEC | SETS_CCODES, { 0x66, 0, 0xD3, 0, 0, modrm_opcode, 0,    1, false }, #opname "16MC", "[!0r+!1d],cl" }, \
{ kX86 ## opname ## 16AC, kShiftArrayCl,  IS_LOAD | IS_STORE | IS_QUIN_OP     | REG_USE01     | REG_USEC | SETS_CCODES, { 0x66, 0, 0xD3, 0, 0, modrm_opcode, 0,    1, false }, #opname "16AC", "[!0r+!1r<<!2d+!3d],cl" }, \
  \
{ kX86 ## opname ## 32RI, kShiftRegImm,                        IS_BINARY_OP   | REG_DEF0_USE0 |            SETS_CCODES, { 0,    0, 0xC1, 0, 0, modrm_opcode, 0xD1, 1, false }, #opname "32RI", "!0r,!1d" }, \
{ kX86 ## opname ## 32MI, kShiftMemImm,   IS_LOAD | IS_STORE | IS_TERTIARY_OP | REG_USE0      |            SETS_CCODES, { 0,    0, 0xC1, 0, 0, modrm_opcode, 0xD1, 1, false }, #opname "32MI", "[!0r+!1d],!2d" }, \
{ kX86 ## opname ## 32AI, kShiftArrayImm, IS_LOAD | IS_STORE | IS_QUIN_OP     | REG_USE01     |            SETS_CCODES, { 0,    0, 0xC1, 0, 0, modrm_opcode, 0xD1, 1, false }, #opname "32AI", "[!0r+!1r<<!2d+!3d],!4d" }, \
{ kX86 ## opname ## 32RC, kShiftRegCl,                         IS_BINARY_OP   | REG_DEF0_USE0 | REG_USEC | SETS_CCODES, { 0,    0, 0xD3, 0, 0, modrm_opcode, 0,    0, false }, #opname "32RC", "!0r,cl" }, \
{ kX86 ## opname ## 32MC, kShiftMemCl,    IS_LOAD | IS_STORE | IS_TERTIARY_OP | REG_USE0      | REG_USEC | SETS_CCODES, { 0,    0, 0xD3, 0, 0, modrm_opcode, 0,    0, false }, #opname "32MC", "[!0r+!1d],cl" }, \
{ kX86 ## opname ## 32AC, kShiftArrayCl,  IS_LOAD | IS_STORE | IS_QUIN_OP     | REG_USE01     | REG_USEC | SETS_CCODES, { 0,    0, 0xD3, 0, 0, modrm_opcode, 0,    0, false }, #opname "32AC", "[!0r+!1r<<!2d+!3d],cl" }, \
  \
{ kX86 ## opname ## 64RI, kShiftRegImm,                        IS_BINARY_OP   | REG_DEF0_USE0 |            SETS_CCODES, { REX_W,    0, 0xC1, 0, 0, modrm_opcode, 0xD1, 1, false }, #opname "64RI", "!0r,!1d" }, \
{ kX86 ## opname ## 64MI, kShiftMemImm,   IS_LOAD | IS_STORE | IS_TERTIARY_OP | REG_USE0      |            SETS_CCODES, { REX_W,    0, 0xC1, 0, 0, modrm_opcode, 0xD1, 1, false }, #opname "64MI", "[!0r+!1d],!2d" }, \
{ kX86 ## opname ## 64AI, kShiftArrayImm, IS_LOAD | IS_STORE | IS_QUIN_OP     | REG_USE01     |            SETS_CCODES, { REX_W,    0, 0xC1, 0, 0, modrm_opcode, 0xD1, 1, false }, #opname "64AI", "[!0r+!1r<<!2d+!3d],!4d" }, \
{ kX86 ## opname ## 64RC, kShiftRegCl,                         IS_BINARY_OP   | REG_DEF0_USE0 | REG_USEC | SETS_CCODES, { REX_W,    0, 0xD3, 0, 0, modrm_opcode, 0,    0, false }, #opname "64RC", "!0r,cl" }, \
{ kX86 ## opname ## 64MC, kShiftMemCl,    IS_LOAD | IS_STORE | IS_TERTIARY_OP | REG_USE0      | REG_USEC | SETS_CCODES, { REX_W,    0, 0xD3, 0, 0, modrm_opcode, 0,    0, false }, #opname "64MC", "[!0r+!1d],cl" }, \
{ kX86 ## opname ## 64AC, kShiftArrayCl,  IS_LOAD | IS_STORE | IS_QUIN_OP     | REG_USE01     | REG_USEC | SETS_CCODES, { REX_W,    0, 0xD3, 0, 0, modrm_opcode, 0,    0, false }, #opname "64AC", "[!0r+!1r<<!2d+!3d],cl" }

  SHIFT_ENCODING_MAP(Rol, 0x0),
  SHIFT_ENCODING_MAP(Ror, 0x1),
  SHIFT_ENCODING_MAP(Rcl, 0x2),
  SHIFT_ENCODING_MAP(Rcr, 0x3),
  SHIFT_ENCODING_MAP(Sal, 0x4),
  SHIFT_ENCODING_MAP(Shr, 0x5),
  SHIFT_ENCODING_MAP(Sar, 0x7),
#undef SHIFT_ENCODING_MAP

  { kX86Cmc, kNullary, NO_OPERAND, { 0, 0, 0xF5, 0, 0, 0, 0, 0, false }, "Cmc", "" },
  { kX86Shld32RRI,  kRegRegImmStore, IS_TERTIARY_OP | REG_DEF0_USE01  | SETS_CCODES,            { 0,    0, 0x0F, 0xA4, 0, 0, 0, 1, false }, "Shld32RRI", "!0r,!1r,!2d" },
  { kX86Shld32MRI,  kMemRegImm,      IS_QUAD_OP | REG_USE02 | IS_LOAD | IS_STORE | SETS_CCODES, { 0,    0, 0x0F, 0xA4, 0, 0, 0, 1, false }, "Shld32MRI", "[!0r+!1d],!2r,!3d" },
  { kX86Shrd32RRI,  kRegRegImmStore, IS_TERTIARY_OP | REG_DEF0_USE01  | SETS_CCODES,            { 0,    0, 0x0F, 0xAC, 0, 0, 0, 1, false }, "Shrd32RRI", "!0r,!1r,!2d" },
  { kX86Shrd32MRI,  kMemRegImm,      IS_QUAD_OP | REG_USE02 | IS_LOAD | IS_STORE | SETS_CCODES, { 0,    0, 0x0F, 0xAC, 0, 0, 0, 1, false }, "Shrd32MRI", "[!0r+!1d],!2r,!3d" },
  { kX86Shld64RRI,  kRegRegImmStore, IS_TERTIARY_OP | REG_DEF0_USE01  | SETS_CCODES,            { REX_W,    0, 0x0F, 0xA4, 0, 0, 0, 1, false }, "Shld64RRI", "!0r,!1r,!2d" },
  { kX86Shld64MRI,  kMemRegImm,      IS_QUAD_OP | REG_USE02 | IS_LOAD | IS_STORE | SETS_CCODES, { REX_W,    0, 0x0F, 0xA4, 0, 0, 0, 1, false }, "Shld64MRI", "[!0r+!1d],!2r,!3d" },
  { kX86Shrd64RRI,  kRegRegImmStore, IS_TERTIARY_OP | REG_DEF0_USE01  | SETS_CCODES,            { REX_W,    0, 0x0F, 0xAC, 0, 0, 0, 1, false }, "Shrd64RRI", "!0r,!1r,!2d" },
  { kX86Shrd64MRI,  kMemRegImm,      IS_QUAD_OP | REG_USE02 | IS_LOAD | IS_STORE | SETS_CCODES, { REX_W,    0, 0x0F, 0xAC, 0, 0, 0, 1, false }, "Shrd64MRI", "[!0r+!1d],!2r,!3d" },

  { kX86Test8RI,  kRegImm,             IS_BINARY_OP   | REG_USE0  | SETS_CCODES, { 0,     0, 0xF6, 0, 0, 0, 0, 1, true }, "Test8RI", "!0r,!1d" },
  { kX86Test8MI,  kMemImm,   IS_LOAD | IS_TERTIARY_OP | REG_USE0  | SETS_CCODES, { 0,     0, 0xF6, 0, 0, 0, 0, 1, true }, "Test8MI", "[!0r+!1d],!2d" },
  { kX86Test8AI,  kArrayImm, IS_LOAD | IS_QUIN_OP     | REG_USE01 | SETS_CCODES, { 0,     0, 0xF6, 0, 0, 0, 0, 1, true }, "Test8AI", "[!0r+!1r<<!2d+!3d],!4d" },
  { kX86Test16RI, kRegImm,             IS_BINARY_OP   | REG_USE0  | SETS_CCODES, { 0x66,  0, 0xF7, 0, 0, 0, 0, 2, false }, "Test16RI", "!0r,!1d" },
  { kX86Test16MI, kMemImm,   IS_LOAD | IS_TERTIARY_OP | REG_USE0  | SETS_CCODES, { 0x66,  0, 0xF7, 0, 0, 0, 0, 2, false }, "Test16MI", "[!0r+!1d],!2d" },
  { kX86Test16AI, kArrayImm, IS_LOAD | IS_QUIN_OP     | REG_USE01 | SETS_CCODES, { 0x66,  0, 0xF7, 0, 0, 0, 0, 2, false }, "Test16AI", "[!0r+!1r<<!2d+!3d],!4d" },
  { kX86Test32RI, kRegImm,             IS_BINARY_OP   | REG_USE0  | SETS_CCODES, { 0,     0, 0xF7, 0, 0, 0, 0, 4, false }, "Test32RI", "!0r,!1d" },
  { kX86Test32MI, kMemImm,   IS_LOAD | IS_TERTIARY_OP | REG_USE0  | SETS_CCODES, { 0,     0, 0xF7, 0, 0, 0, 0, 4, false }, "Test32MI", "[!0r+!1d],!2d" },
  { kX86Test32AI, kArrayImm, IS_LOAD | IS_QUIN_OP     | REG_USE01 | SETS_CCODES, { 0,     0, 0xF7, 0, 0, 0, 0, 4, false }, "Test32AI", "[!0r+!1r<<!2d+!3d],!4d" },
  { kX86Test64RI, kRegImm,             IS_BINARY_OP   | REG_USE0  | SETS_CCODES, { REX_W, 0, 0xF7, 0, 0, 0, 0, 4, false }, "Test64RI", "!0r,!1d" },
  { kX86Test64MI, kMemImm,   IS_LOAD | IS_TERTIARY_OP | REG_USE0  | SETS_CCODES, { REX_W, 0, 0xF7, 0, 0, 0, 0, 4, false }, "Test64MI", "[!0r+!1d],!2d" },
  { kX86Test64AI, kArrayImm, IS_LOAD | IS_QUIN_OP     | REG_USE01 | SETS_CCODES, { REX_W, 0, 0xF7, 0, 0, 0, 0, 4, false }, "Test64AI", "[!0r+!1r<<!2d+!3d],!4d" },

  { kX86Test32RR, kRegReg,             IS_BINARY_OP   | REG_USE01 | SETS_CCODES, { 0,     0, 0x85, 0, 0, 0, 0, 0, false }, "Test32RR", "!0r,!1r" },
  { kX86Test64RR, kRegReg,             IS_BINARY_OP   | REG_USE01 | SETS_CCODES, { REX_W, 0, 0x85, 0, 0, 0, 0, 0, false }, "Test64RR", "!0r,!1r" },
  { kX86Test32RM, kRegMem,   IS_LOAD | IS_TERTIARY_OP | REG_USE01 | SETS_CCODES, { 0,     0, 0x85, 0, 0, 0, 0, 0, false }, "Test32RM", "!0r,[!1r+!2d]" },

#define UNARY_ENCODING_MAP(opname, modrm, is_store, sets_ccodes, \
                           reg, reg_kind, reg_flags, \
                           mem, mem_kind, mem_flags, \
                           arr, arr_kind, arr_flags, imm, \
                           b_flags, hw_flags, w_flags, \
                           b_format, hw_format, w_format) \
{ kX86 ## opname ## 8 ## reg,  reg_kind,                      reg_flags | b_flags  | sets_ccodes, { 0,    0, 0xF6, 0, 0, modrm, 0, imm << 0, true }, #opname "8" #reg, b_format "!0r" }, \
{ kX86 ## opname ## 8 ## mem,  mem_kind, IS_LOAD | is_store | mem_flags | b_flags  | sets_ccodes, { 0,    0, 0xF6, 0, 0, modrm, 0, imm << 0, true }, #opname "8" #mem, b_format "[!0r+!1d]" }, \
{ kX86 ## opname ## 8 ## arr,  arr_kind, IS_LOAD | is_store | arr_flags | b_flags  | sets_ccodes, { 0,    0, 0xF6, 0, 0, modrm, 0, imm << 0, true }, #opname "8" #arr, b_format "[!0r+!1r<<!2d+!3d]" }, \
{ kX86 ## opname ## 16 ## reg, reg_kind,                      reg_flags | hw_flags | sets_ccodes, { 0x66, 0, 0xF7, 0, 0, modrm, 0, imm << 1, false }, #opname "16" #reg, hw_format "!0r" }, \
{ kX86 ## opname ## 16 ## mem, mem_kind, IS_LOAD | is_store | mem_flags | hw_flags | sets_ccodes, { 0x66, 0, 0xF7, 0, 0, modrm, 0, imm << 1, false }, #opname "16" #mem, hw_format "[!0r+!1d]" }, \
{ kX86 ## opname ## 16 ## arr, arr_kind, IS_LOAD | is_store | arr_flags | hw_flags | sets_ccodes, { 0x66, 0, 0xF7, 0, 0, modrm, 0, imm << 1, false }, #opname "16" #arr, hw_format "[!0r+!1r<<!2d+!3d]" }, \
{ kX86 ## opname ## 32 ## reg, reg_kind,                      reg_flags | w_flags  | sets_ccodes, { 0,    0, 0xF7, 0, 0, modrm, 0, imm << 2, false }, #opname "32" #reg, w_format "!0r" }, \
{ kX86 ## opname ## 32 ## mem, mem_kind, IS_LOAD | is_store | mem_flags | w_flags  | sets_ccodes, { 0,    0, 0xF7, 0, 0, modrm, 0, imm << 2, false }, #opname "32" #mem, w_format "[!0r+!1d]" }, \
{ kX86 ## opname ## 32 ## arr, arr_kind, IS_LOAD | is_store | arr_flags | w_flags  | sets_ccodes, { 0,    0, 0xF7, 0, 0, modrm, 0, imm << 2, false }, #opname "32" #arr, w_format "[!0r+!1r<<!2d+!3d]" }, \
{ kX86 ## opname ## 64 ## reg, reg_kind,                      reg_flags | w_flags  | sets_ccodes, { REX_W, 0, 0xF7, 0, 0, modrm, 0, imm << 2, false }, #opname "64" #reg, w_format "!0r" }, \
{ kX86 ## opname ## 64 ## mem, mem_kind, IS_LOAD | is_store | mem_flags | w_flags  | sets_ccodes, { REX_W, 0, 0xF7, 0, 0, modrm, 0, imm << 2, false }, #opname "64" #mem, w_format "[!0r+!1d]" }, \
{ kX86 ## opname ## 64 ## arr, arr_kind, IS_LOAD | is_store | arr_flags | w_flags  | sets_ccodes, { REX_W, 0, 0xF7, 0, 0, modrm, 0, imm << 2, false }, #opname "64" #arr, w_format "[!0r+!1r<<!2d+!3d]" }

  UNARY_ENCODING_MAP(Not, 0x2, IS_STORE, 0,           R, kReg, IS_UNARY_OP | REG_DEF0_USE0, M, kMem, IS_BINARY_OP | REG_USE0, A, kArray, IS_QUAD_OP | REG_USE01, 0, 0, 0, 0, "", "", ""),
  UNARY_ENCODING_MAP(Neg, 0x3, IS_STORE, SETS_CCODES, R, kReg, IS_UNARY_OP | REG_DEF0_USE0, M, kMem, IS_BINARY_OP | REG_USE0, A, kArray, IS_QUAD_OP | REG_USE01, 0, 0, 0, 0, "", "", ""),

  UNARY_ENCODING_MAP(Mul,     0x4, 0, SETS_CCODES, DaR, kReg, IS_UNARY_OP | REG_USE0, DaM, kMem, IS_BINARY_OP | REG_USE0, DaA, kArray, IS_QUAD_OP | REG_USE01, 0, REG_DEFA_USEA, REG_DEFAD_USEA,  REG_DEFAD_USEA,  "ax,al,", "dx:ax,ax,", "edx:eax,eax,"),
  UNARY_ENCODING_MAP(Imul,    0x5, 0, SETS_CCODES, DaR, kReg, IS_UNARY_OP | REG_USE0, DaM, kMem, IS_BINARY_OP | REG_USE0, DaA, kArray, IS_QUAD_OP | REG_USE01, 0, REG_DEFA_USEA, REG_DEFAD_USEA,  REG_DEFAD_USEA,  "ax,al,", "dx:ax,ax,", "edx:eax,eax,"),
  UNARY_ENCODING_MAP(Divmod,  0x6, 0, SETS_CCODES, DaR, kReg, IS_UNARY_OP | REG_USE0, DaM, kMem, IS_BINARY_OP | REG_USE0, DaA, kArray, IS_QUAD_OP | REG_USE01, 0, REG_DEFA_USEA, REG_DEFAD_USEAD, REG_DEFAD_USEAD, "ah:al,ax,", "dx:ax,dx:ax,", "edx:eax,edx:eax,"),
  UNARY_ENCODING_MAP(Idivmod, 0x7, 0, SETS_CCODES, DaR, kReg, IS_UNARY_OP | REG_USE0, DaM, kMem, IS_BINARY_OP | REG_USE0, DaA, kArray, IS_QUAD_OP | REG_USE01, 0, REG_DEFA_USEA, REG_DEFAD_USEAD, REG_DEFAD_USEAD, "ah:al,ax,", "dx:ax,dx:ax,", "edx:eax,edx:eax,"),
#undef UNARY_ENCODING_MAP

  { kx86Cdq32Da, kRegOpcode, NO_OPERAND | REG_DEFAD_USEA,                                  { 0,     0, 0x99, 0,    0, 0, 0, 0, false }, "Cdq", "" },
  { kx86Cqo64Da, kRegOpcode, NO_OPERAND | REG_DEFAD_USEA,                                  { REX_W, 0, 0x99, 0,    0, 0, 0, 0, false }, "Cqo", "" },
  { kX86Bswap32R, kRegOpcode, IS_UNARY_OP | REG_DEF0_USE0,                                 { 0,     0, 0x0F, 0xC8, 0, 0, 0, 0, false }, "Bswap32R", "!0r" },
  { kX86Bswap64R, kRegOpcode, IS_UNARY_OP | REG_DEF0_USE0,                                 { REX_W, 0, 0x0F, 0xC8, 0, 0, 0, 0, false }, "Bswap64R", "!0r" },
  { kX86Push32R,  kRegOpcode, IS_UNARY_OP | REG_USE0 | REG_USE_SP | REG_DEF_SP | IS_STORE, { 0,     0, 0x50, 0,    0, 0, 0, 0, false }, "Push32R",  "!0r" },
  { kX86Pop32R,   kRegOpcode, IS_UNARY_OP | REG_DEF0 | REG_USE_SP | REG_DEF_SP | IS_LOAD,  { 0,     0, 0x58, 0,    0, 0, 0, 0, false }, "Pop32R",   "!0r" },

#define EXT_0F_ENCODING_MAP(opname, prefix, opcode, reg_def) \
{ kX86 ## opname ## RR, kRegReg,             IS_BINARY_OP   | reg_def | REG_USE1,  { prefix, 0, 0x0F, opcode, 0, 0, 0, 0, false }, #opname "RR", "!0r,!1r" }, \
{ kX86 ## opname ## RM, kRegMem,   IS_LOAD | IS_TERTIARY_OP | reg_def | REG_USE1,  { prefix, 0, 0x0F, opcode, 0, 0, 0, 0, false }, #opname "RM", "!0r,[!1r+!2d]" }, \
{ kX86 ## opname ## RA, kRegArray, IS_LOAD | IS_QUIN_OP     | reg_def | REG_USE12, { prefix, 0, 0x0F, opcode, 0, 0, 0, 0, false }, #opname "RA", "!0r,[!1r+!2r<<!3d+!4d]" }

// This is a special encoding with r8_form on the second register only
// for Movzx8 and Movsx8.
#define EXT_0F_R8_FORM_ENCODING_MAP(opname, prefix, opcode, reg_def) \
{ kX86 ## opname ## RR, kRegReg,             IS_BINARY_OP   | reg_def | REG_USE1,  { prefix, 0, 0x0F, opcode, 0, 0, 0, 0, true }, #opname "RR", "!0r,!1r" }, \
{ kX86 ## opname ## RM, kRegMem,   IS_LOAD | IS_TERTIARY_OP | reg_def | REG_USE1,  { prefix, 0, 0x0F, opcode, 0, 0, 0, 0, false }, #opname "RM", "!0r,[!1r+!2d]" }, \
{ kX86 ## opname ## RA, kRegArray, IS_LOAD | IS_QUIN_OP     | reg_def | REG_USE12, { prefix, 0, 0x0F, opcode, 0, 0, 0, 0, false }, #opname "RA", "!0r,[!1r+!2r<<!3d+!4d]" }

#define EXT_0F_REX_W_ENCODING_MAP(opname, prefix, opcode, reg_def) \
{ kX86 ## opname ## RR, kRegReg,             IS_BINARY_OP   | reg_def | REG_USE1,  { prefix, REX_W, 0x0F, opcode, 0, 0, 0, 0, false }, #opname "RR", "!0r,!1r" }, \
{ kX86 ## opname ## RM, kRegMem,   IS_LOAD | IS_TERTIARY_OP | reg_def | REG_USE1,  { prefix, REX_W, 0x0F, opcode, 0, 0, 0, 0, false }, #opname "RM", "!0r,[!1r+!2d]" }, \
{ kX86 ## opname ## RA, kRegArray, IS_LOAD | IS_QUIN_OP     | reg_def | REG_USE12, { prefix, REX_W, 0x0F, opcode, 0, 0, 0, 0, false }, #opname "RA", "!0r,[!1r+!2r<<!3d+!4d]" }

#define EXT_0F_ENCODING2_MAP(opname, prefix, opcode, opcode2, reg_def) \
{ kX86 ## opname ## RR, kRegReg,             IS_BINARY_OP   | reg_def | REG_USE1,  { prefix, 0, 0x0F, opcode, opcode2, 0, 0, 0, false }, #opname "RR", "!0r,!1r" }, \
{ kX86 ## opname ## RM, kRegMem,   IS_LOAD | IS_TERTIARY_OP | reg_def | REG_USE1,  { prefix, 0, 0x0F, opcode, opcode2, 0, 0, 0, false }, #opname "RM", "!0r,[!1r+!2d]" }, \
{ kX86 ## opname ## RA, kRegArray, IS_LOAD | IS_QUIN_OP     | reg_def | REG_USE12, { prefix, 0, 0x0F, opcode, opcode2, 0, 0, 0, false }, #opname "RA", "!0r,[!1r+!2r<<!3d+!4d]" }

  EXT_0F_ENCODING_MAP(Movsd, 0xF2, 0x10, REG_DEF0),
  { kX86MovsdMR, kMemReg,   IS_STORE | IS_TERTIARY_OP | REG_USE02,  { 0xF2, 0, 0x0F, 0x11, 0, 0, 0, 0, false }, "MovsdMR", "[!0r+!1d],!2r" },
  { kX86MovsdAR, kArrayReg, IS_STORE | IS_QUIN_OP     | REG_USE014, { 0xF2, 0, 0x0F, 0x11, 0, 0, 0, 0, false }, "MovsdAR", "[!0r+!1r<<!2d+!3d],!4r" },

  EXT_0F_ENCODING_MAP(Movss, 0xF3, 0x10, REG_DEF0),
  { kX86MovssMR, kMemReg,   IS_STORE | IS_TERTIARY_OP | REG_USE02,  { 0xF3, 0, 0x0F, 0x11, 0, 0, 0, 0, false }, "MovssMR", "[!0r+!1d],!2r" },
  { kX86MovssAR, kArrayReg, IS_STORE | IS_QUIN_OP     | REG_USE014, { 0xF3, 0, 0x0F, 0x11, 0, 0, 0, 0, false }, "MovssAR", "[!0r+!1r<<!2d+!3d],!4r" },

  EXT_0F_ENCODING_MAP(Cvtsi2sd,  0xF2, 0x2A, REG_DEF0),
  EXT_0F_ENCODING_MAP(Cvtsi2ss,  0xF3, 0x2A, REG_DEF0),
  EXT_0F_REX_W_ENCODING_MAP(Cvtsqi2sd,  0xF2, 0x2A, REG_DEF0),
  EXT_0F_REX_W_ENCODING_MAP(Cvtsqi2ss,  0xF3, 0x2A, REG_DEF0),
  EXT_0F_ENCODING_MAP(Cvttsd2si, 0xF2, 0x2C, REG_DEF0),
  EXT_0F_ENCODING_MAP(Cvttss2si, 0xF3, 0x2C, REG_DEF0),
  EXT_0F_REX_W_ENCODING_MAP(Cvttsd2sqi, 0xF2, 0x2C, REG_DEF0),
  EXT_0F_REX_W_ENCODING_MAP(Cvttss2sqi, 0xF3, 0x2C, REG_DEF0),
  EXT_0F_ENCODING_MAP(Cvtsd2si,  0xF2, 0x2D, REG_DEF0),
  EXT_0F_ENCODING_MAP(Cvtss2si,  0xF3, 0x2D, REG_DEF0),
  EXT_0F_ENCODING_MAP(Ucomisd,   0x66, 0x2E, SETS_CCODES|REG_USE0),
  EXT_0F_ENCODING_MAP(Ucomiss,   0x00, 0x2E, SETS_CCODES|REG_USE0),
  EXT_0F_ENCODING_MAP(Comisd,    0x66, 0x2F, SETS_CCODES|REG_USE0),
  EXT_0F_ENCODING_MAP(Comiss,    0x00, 0x2F, SETS_CCODES|REG_USE0),
  EXT_0F_ENCODING_MAP(Orpd,      0x66, 0x56, REG_DEF0_USE0),
  EXT_0F_ENCODING_MAP(Orps,      0x00, 0x56, REG_DEF0_USE0),
  EXT_0F_ENCODING_MAP(Andpd,     0x66, 0x54, REG_DEF0_USE0),
  EXT_0F_ENCODING_MAP(Andps,     0x00, 0x54, REG_DEF0_USE0),
  EXT_0F_ENCODING_MAP(Xorpd,     0x66, 0x57, REG_DEF0_USE0),
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
  EXT_0F_ENCODING_MAP(Sqrtsd,    0xF2, 0x51, REG_DEF0_USE0),
  EXT_0F_ENCODING2_MAP(Pmulld,   0x66, 0x38, 0x40, REG_DEF0_USE0),
  EXT_0F_ENCODING_MAP(Pmullw,    0x66, 0xD5, REG_DEF0_USE0),
  EXT_0F_ENCODING_MAP(Mulps,     0x00, 0x59, REG_DEF0_USE0),
  EXT_0F_ENCODING_MAP(Mulpd,     0x66, 0x59, REG_DEF0_USE0),
  EXT_0F_ENCODING_MAP(Paddb,     0x66, 0xFC, REG_DEF0_USE0),
  EXT_0F_ENCODING_MAP(Paddw,     0x66, 0xFD, REG_DEF0_USE0),
  EXT_0F_ENCODING_MAP(Paddd,     0x66, 0xFE, REG_DEF0_USE0),
  EXT_0F_ENCODING_MAP(Addps,     0x00, 0x58, REG_DEF0_USE0),
  EXT_0F_ENCODING_MAP(Addpd,     0xF2, 0x58, REG_DEF0_USE0),
  EXT_0F_ENCODING_MAP(Psubb,     0x66, 0xF8, REG_DEF0_USE0),
  EXT_0F_ENCODING_MAP(Psubw,     0x66, 0xF9, REG_DEF0_USE0),
  EXT_0F_ENCODING_MAP(Psubd,     0x66, 0xFA, REG_DEF0_USE0),
  EXT_0F_ENCODING_MAP(Subps,     0x00, 0x5C, REG_DEF0_USE0),
  EXT_0F_ENCODING_MAP(Subpd,     0x66, 0x5C, REG_DEF0_USE0),
  EXT_0F_ENCODING_MAP(Pand,      0x66, 0xDB, REG_DEF0_USE0),
  EXT_0F_ENCODING_MAP(Por,       0x66, 0xEB, REG_DEF0_USE0),
  EXT_0F_ENCODING_MAP(Pxor,      0x66, 0xEF, REG_DEF0_USE0),
  EXT_0F_ENCODING2_MAP(Phaddw,   0x66, 0x38, 0x01, REG_DEF0_USE0),
  EXT_0F_ENCODING2_MAP(Phaddd,   0x66, 0x38, 0x02, REG_DEF0_USE0),
  EXT_0F_ENCODING_MAP(Haddpd,    0x66, 0x7C, REG_DEF0_USE0),
  EXT_0F_ENCODING_MAP(Haddps,    0xF2, 0x7C, REG_DEF0_USE0),

  { kX86PextrbRRI, kRegRegImmStore, IS_TERTIARY_OP | REG_DEF0  | REG_USE1, { 0x66, 0, 0x0F, 0x3A, 0x14, 0, 0, 1, false }, "PextbRRI", "!0r,!1r,!2d" },
  { kX86PextrwRRI, kRegRegImm, IS_TERTIARY_OP | REG_DEF0  | REG_USE1, { 0x66, 0, 0x0F, 0xC5, 0x00, 0, 0, 1, false }, "PextwRRI", "!0r,!1r,!2d" },
  { kX86PextrdRRI, kRegRegImmStore, IS_TERTIARY_OP | REG_DEF0  | REG_USE1, { 0x66, 0, 0x0F, 0x3A, 0x16, 0, 0, 1, false }, "PextdRRI", "!0r,!1r,!2d" },
  { kX86PextrbMRI, kMemRegImm, IS_QUAD_OP     | REG_USE02 | IS_STORE, { 0x66, 0, 0x0F, 0x3A, 0x16, 0, 0, 1, false }, "kX86PextrbMRI", "[!0r+!1d],!2r,!3d" },
  { kX86PextrwMRI, kMemRegImm, IS_QUAD_OP     | REG_USE02 | IS_STORE, { 0x66, 0, 0x0F, 0x3A, 0x16, 0, 0, 1, false }, "kX86PextrwMRI", "[!0r+!1d],!2r,!3d" },
  { kX86PextrdMRI, kMemRegImm, IS_QUAD_OP     | REG_USE02 | IS_STORE, { 0x66, 0, 0x0F, 0x3A, 0x16, 0, 0, 1, false }, "kX86PextrdMRI", "[!0r+!1d],!2r,!3d" },

  { kX86PshuflwRRI, kRegRegImm, IS_TERTIARY_OP | REG_DEF0 | REG_USE1, { 0xF2, 0, 0x0F, 0x70, 0, 0, 0, 1, false }, "PshuflwRRI", "!0r,!1r,!2d" },
  { kX86PshufdRRI,  kRegRegImm, IS_TERTIARY_OP | REG_DEF0 | REG_USE1, { 0x66, 0, 0x0F, 0x70, 0, 0, 0, 1, false }, "PshuffRRI", "!0r,!1r,!2d" },

  { kX86ShufpsRRI, kRegRegImm, IS_TERTIARY_OP | REG_DEF0 | REG_USE1, { 0x00, 0, 0x0F, 0xC6, 0, 0, 0, 1, false }, "kX86ShufpsRRI", "!0r,!1r,!2d" },
  { kX86ShufpdRRI, kRegRegImm, IS_TERTIARY_OP | REG_DEF0 | REG_USE1, { 0x66, 0, 0x0F, 0xC6, 0, 0, 0, 1, false }, "kX86ShufpdRRI", "!0r,!1r,!2d" },

  { kX86PsrawRI, kRegImm, IS_BINARY_OP | REG_DEF0_USE0, { 0x66, 0, 0x0F, 0x71, 0, 4, 0, 1, false }, "PsrawRI", "!0r,!1d" },
  { kX86PsradRI, kRegImm, IS_BINARY_OP | REG_DEF0_USE0, { 0x66, 0, 0x0F, 0x72, 0, 4, 0, 1, false }, "PsradRI", "!0r,!1d" },
  { kX86PsrlwRI, kRegImm, IS_BINARY_OP | REG_DEF0_USE0, { 0x66, 0, 0x0F, 0x71, 0, 2, 0, 1, false }, "PsrlwRI", "!0r,!1d" },
  { kX86PsrldRI, kRegImm, IS_BINARY_OP | REG_DEF0_USE0, { 0x66, 0, 0x0F, 0x72, 0, 2, 0, 1, false }, "PsrldRI", "!0r,!1d" },
  { kX86PsrlqRI, kRegImm, IS_BINARY_OP | REG_DEF0_USE0, { 0x66, 0, 0x0F, 0x73, 0, 2, 0, 1, false }, "PsrlqRI", "!0r,!1d" },
  { kX86PsllwRI, kRegImm, IS_BINARY_OP | REG_DEF0_USE0, { 0x66, 0, 0x0F, 0x71, 0, 6, 0, 1, false }, "PsllwRI", "!0r,!1d" },
  { kX86PslldRI, kRegImm, IS_BINARY_OP | REG_DEF0_USE0, { 0x66, 0, 0x0F, 0x72, 0, 6, 0, 1, false }, "PslldRI", "!0r,!1d" },
  { kX86PsllqRI, kRegImm, IS_BINARY_OP | REG_DEF0_USE0, { 0x66, 0, 0x0F, 0x73, 0, 6, 0, 1, false }, "PsllqRI", "!0r,!1d" },

  { kX86Fild32M,  kMem,     IS_LOAD    | IS_UNARY_OP | REG_USE0 | USE_FP_STACK, { 0x0,  0,    0xDB, 0x00, 0, 0, 0, 0, false }, "Fild32M",  "[!0r,!1d]" },
  { kX86Fild64M,  kMem,     IS_LOAD    | IS_UNARY_OP | REG_USE0 | USE_FP_STACK, { 0x0,  0,    0xDF, 0x00, 0, 5, 0, 0, false }, "Fild64M",  "[!0r,!1d]" },
  { kX86Fld32M,   kMem,     IS_LOAD    | IS_UNARY_OP | REG_USE0 | USE_FP_STACK, { 0x0,  0,    0xD9, 0x00, 0, 0, 0, 0, false }, "Fld32M",   "[!0r,!1d]" },
  { kX86Fld64M,   kMem,     IS_LOAD    | IS_UNARY_OP | REG_USE0 | USE_FP_STACK, { 0x0,  0,    0xDD, 0x00, 0, 0, 0, 0, false }, "Fld64M",   "[!0r,!1d]" },
  { kX86Fstp32M,  kMem,     IS_STORE   | IS_UNARY_OP | REG_USE0 | USE_FP_STACK, { 0x0,  0,    0xD9, 0x00, 0, 3, 0, 0, false }, "Fstps32M", "[!0r,!1d]" },
  { kX86Fstp64M,  kMem,     IS_STORE   | IS_UNARY_OP | REG_USE0 | USE_FP_STACK, { 0x0,  0,    0xDD, 0x00, 0, 3, 0, 0, false }, "Fstpd64M", "[!0r,!1d]" },
  { kX86Fst32M,   kMem,     IS_STORE   | IS_UNARY_OP | REG_USE0 | USE_FP_STACK, { 0x0,  0,    0xD9, 0x00, 0, 2, 0, 0, false }, "Fsts32M",  "[!0r,!1d]" },
  { kX86Fst64M,   kMem,     IS_STORE   | IS_UNARY_OP | REG_USE0 | USE_FP_STACK, { 0x0,  0,    0xDD, 0x00, 0, 2, 0, 0, false }, "Fstd64M",  "[!0r,!1d]" },
  { kX86Fprem,    kNullary, NO_OPERAND | USE_FP_STACK,                          { 0xD9, 0,    0xF8, 0,    0, 0, 0, 0, false }, "Fprem64",  "" },
  { kX86Fucompp,  kNullary, NO_OPERAND | USE_FP_STACK,                          { 0xDA, 0,    0xE9, 0,    0, 0, 0, 0, false }, "Fucompp",  "" },
  { kX86Fstsw16R, kNullary, NO_OPERAND | REG_DEFA | USE_FP_STACK,               { 0x9B, 0xDF, 0xE0, 0,    0, 0, 0, 0, false }, "Fstsw16R", "ax" },

  EXT_0F_ENCODING_MAP(Mova128,    0x66, 0x6F, REG_DEF0),
  { kX86Mova128MR, kMemReg,   IS_STORE | IS_TERTIARY_OP | REG_USE02,  { 0x66, 0, 0x0F, 0x6F, 0, 0, 0, 0, false }, "Mova128MR", "[!0r+!1d],!2r" },
  { kX86Mova128AR, kArrayReg, IS_STORE | IS_QUIN_OP     | REG_USE014, { 0x66, 0, 0x0F, 0x6F, 0, 0, 0, 0, false }, "Mova128AR", "[!0r+!1r<<!2d+!3d],!4r" },


  EXT_0F_ENCODING_MAP(Movups,    0x0, 0x10, REG_DEF0),
  { kX86MovupsMR, kMemReg,      IS_STORE | IS_TERTIARY_OP | REG_USE02,  { 0x0, 0, 0x0F, 0x11, 0, 0, 0, 0, false }, "MovupsMR", "[!0r+!1d],!2r" },
  { kX86MovupsAR, kArrayReg,    IS_STORE | IS_QUIN_OP     | REG_USE014, { 0x0, 0, 0x0F, 0x11, 0, 0, 0, 0, false }, "MovupsAR", "[!0r+!1r<<!2d+!3d],!4r" },

  EXT_0F_ENCODING_MAP(Movaps,    0x0, 0x28, REG_DEF0),
  { kX86MovapsMR, kMemReg,      IS_STORE | IS_TERTIARY_OP | REG_USE02,  { 0x0, 0, 0x0F, 0x29, 0, 0, 0, 0, false }, "MovapsMR", "[!0r+!1d],!2r" },
  { kX86MovapsAR, kArrayReg,    IS_STORE | IS_QUIN_OP     | REG_USE014, { 0x0, 0, 0x0F, 0x29, 0, 0, 0, 0, false }, "MovapsAR", "[!0r+!1r<<!2d+!3d],!4r" },

  { kX86MovlpsRM, kRegMem,      IS_LOAD | IS_TERTIARY_OP | REG_DEF0 | REG_USE01,  { 0x0, 0, 0x0F, 0x12, 0, 0, 0, 0, false }, "MovlpsRM", "!0r,[!1r+!2d]" },
  { kX86MovlpsRA, kRegArray,    IS_LOAD | IS_QUIN_OP     | REG_DEF0 | REG_USE012, { 0x0, 0, 0x0F, 0x12, 0, 0, 0, 0, false }, "MovlpsRA", "!0r,[!1r+!2r<<!3d+!4d]" },
  { kX86MovlpsMR, kMemReg,      IS_STORE | IS_TERTIARY_OP | REG_USE02,            { 0x0, 0, 0x0F, 0x13, 0, 0, 0, 0, false }, "MovlpsMR", "[!0r+!1d],!2r" },
  { kX86MovlpsAR, kArrayReg,    IS_STORE | IS_QUIN_OP     | REG_USE014,           { 0x0, 0, 0x0F, 0x13, 0, 0, 0, 0, false }, "MovlpsAR", "[!0r+!1r<<!2d+!3d],!4r" },

  { kX86MovhpsRM, kRegMem,      IS_LOAD | IS_TERTIARY_OP | REG_DEF0 | REG_USE01,  { 0x0, 0, 0x0F, 0x16, 0, 0, 0, 0, false }, "MovhpsRM", "!0r,[!1r+!2d]" },
  { kX86MovhpsRA, kRegArray,    IS_LOAD | IS_QUIN_OP     | REG_DEF0 | REG_USE012, { 0x0, 0, 0x0F, 0x16, 0, 0, 0, 0, false }, "MovhpsRA", "!0r,[!1r+!2r<<!3d+!4d]" },
  { kX86MovhpsMR, kMemReg,      IS_STORE | IS_TERTIARY_OP | REG_USE02,            { 0x0, 0, 0x0F, 0x17, 0, 0, 0, 0, false }, "MovhpsMR", "[!0r+!1d],!2r" },
  { kX86MovhpsAR, kArrayReg,    IS_STORE | IS_QUIN_OP     | REG_USE014,           { 0x0, 0, 0x0F, 0x17, 0, 0, 0, 0, false }, "MovhpsAR", "[!0r+!1r<<!2d+!3d],!4r" },

  EXT_0F_ENCODING_MAP(Movdxr,    0x66, 0x6E, REG_DEF0),
  EXT_0F_REX_W_ENCODING_MAP(Movqxr, 0x66, 0x6E, REG_DEF0),
  { kX86MovqrxRR, kRegRegStore, IS_BINARY_OP | REG_DEF0   | REG_USE1,   { 0x66, REX_W, 0x0F, 0x7E, 0, 0, 0, 0, false }, "MovqrxRR", "!0r,!1r" },
  { kX86MovqrxMR, kMemReg,      IS_STORE | IS_TERTIARY_OP | REG_USE02,  { 0x66, REX_W, 0x0F, 0x7E, 0, 0, 0, 0, false }, "MovqrxMR", "[!0r+!1d],!2r" },
  { kX86MovqrxAR, kArrayReg,    IS_STORE | IS_QUIN_OP     | REG_USE014, { 0x66, REX_W, 0x0F, 0x7E, 0, 0, 0, 0, false }, "MovqrxAR", "[!0r+!1r<<!2d+!3d],!4r" },

  { kX86MovdrxRR, kRegRegStore, IS_BINARY_OP | REG_DEF0   | REG_USE1,   { 0x66, 0, 0x0F, 0x7E, 0, 0, 0, 0, false }, "MovdrxRR", "!0r,!1r" },
  { kX86MovdrxMR, kMemReg,      IS_STORE | IS_TERTIARY_OP | REG_USE02,  { 0x66, 0, 0x0F, 0x7E, 0, 0, 0, 0, false }, "MovdrxMR", "[!0r+!1d],!2r" },
  { kX86MovdrxAR, kArrayReg,    IS_STORE | IS_QUIN_OP     | REG_USE014, { 0x66, 0, 0x0F, 0x7E, 0, 0, 0, 0, false }, "MovdrxAR", "[!0r+!1r<<!2d+!3d],!4r" },

  { kX86MovsxdRR, kRegReg,      IS_BINARY_OP | REG_DEF0 | REG_USE1,              { REX_W, 0, 0x63, 0, 0, 0, 0, 0, false }, "MovsxdRR", "!0r,!1r" },
  { kX86MovsxdRM, kRegMem,      IS_LOAD | IS_TERTIARY_OP | REG_DEF0 | REG_USE1,  { REX_W, 0, 0x63, 0, 0, 0, 0, 0, false }, "MovsxdRM", "!0r,[!1r+!2d]" },
  { kX86MovsxdRA, kRegArray,    IS_LOAD | IS_QUIN_OP     | REG_DEF0 | REG_USE12, { REX_W, 0, 0x63, 0, 0, 0, 0, 0, false }, "MovsxdRA", "!0r,[!1r+!2r<<!3d+!4d]" },

  { kX86Set8R, kRegCond,   IS_BINARY_OP | REG_DEF0   | REG_USE0  | USES_CCODES, { 0, 0, 0x0F, 0x90, 0, 0, 0, 0, true  }, "Set8R", "!1c !0r" },
  { kX86Set8M, kMemCond,   IS_STORE | IS_TERTIARY_OP | REG_USE0  | USES_CCODES, { 0, 0, 0x0F, 0x90, 0, 0, 0, 0, false }, "Set8M", "!2c [!0r+!1d]" },
  { kX86Set8A, kArrayCond, IS_STORE | IS_QUIN_OP     | REG_USE01 | USES_CCODES, { 0, 0, 0x0F, 0x90, 0, 0, 0, 0, false }, "Set8A", "!4c [!0r+!1r<<!2d+!3d]" },

  // TODO: load/store?
  // Encode the modrm opcode as an extra opcode byte to avoid computation during assembly.
  { kX86Mfence, kReg,                 NO_OPERAND,     { 0, 0, 0x0F, 0xAE, 0, 6, 0, 0, false }, "Mfence", "" },

  EXT_0F_ENCODING_MAP(Imul16,  0x66, 0xAF, REG_USE0 | REG_DEF0 | SETS_CCODES),
  EXT_0F_ENCODING_MAP(Imul32,  0x00, 0xAF, REG_USE0 | REG_DEF0 | SETS_CCODES),
  EXT_0F_ENCODING_MAP(Imul64,  REX_W, 0xAF, REG_USE0 | REG_DEF0 | SETS_CCODES),

  { kX86CmpxchgRR, kRegRegStore,  IS_BINARY_OP | REG_DEF0 | REG_USE01 | REG_DEFA_USEA | SETS_CCODES,   { 0,    0, 0x0F, 0xB1, 0, 0, 0, 0, false }, "Cmpxchg", "!0r,!1r" },
  { kX86CmpxchgMR, kMemReg,       IS_STORE | IS_TERTIARY_OP | REG_USE02 | REG_DEFA_USEA | SETS_CCODES, { 0,    0, 0x0F, 0xB1, 0, 0, 0, 0, false }, "Cmpxchg", "[!0r+!1d],!2r" },
  { kX86CmpxchgAR, kArrayReg,     IS_STORE | IS_QUIN_OP | REG_USE014 | REG_DEFA_USEA | SETS_CCODES,    { 0,    0, 0x0F, 0xB1, 0, 0, 0, 0, false }, "Cmpxchg", "[!0r+!1r<<!2d+!3d],!4r" },
  { kX86LockCmpxchgMR, kMemReg,   IS_STORE | IS_TERTIARY_OP | REG_USE02 | REG_DEFA_USEA | SETS_CCODES, { 0xF0, 0, 0x0F, 0xB1, 0, 0, 0, 0, false }, "Lock Cmpxchg", "[!0r+!1d],!2r" },
  { kX86LockCmpxchgAR, kArrayReg, IS_STORE | IS_QUIN_OP | REG_USE014 | REG_DEFA_USEA | SETS_CCODES,    { 0xF0, 0, 0x0F, 0xB1, 0, 0, 0, 0, false }, "Lock Cmpxchg", "[!0r+!1r<<!2d+!3d],!4r" },
  { kX86LockCmpxchg64AR, kArrayReg, IS_STORE | IS_QUIN_OP | REG_USE014 | REG_DEFA_USEA | SETS_CCODES,    { 0xF0, REX_W, 0x0F, 0xB1, 0, 0, 0, 0, false }, "Lock Cmpxchg", "[!0r+!1r<<!2d+!3d],!4r" },
  { kX86LockCmpxchg64M, kMem,     IS_STORE | IS_BINARY_OP | REG_USE0 | REG_DEFAD_USEAD | REG_USEC | REG_USEB | SETS_CCODES, { 0xF0, 0, 0x0F, 0xC7, 0, 1, 0, 0, false }, "Lock Cmpxchg8b", "[!0r+!1d]" },
  { kX86LockCmpxchg64A, kArray,   IS_STORE | IS_QUAD_OP | REG_USE01 | REG_DEFAD_USEAD | REG_USEC | REG_USEB | SETS_CCODES,  { 0xF0, 0, 0x0F, 0xC7, 0, 1, 0, 0, false }, "Lock Cmpxchg8b", "[!0r+!1r<<!2d+!3d]" },
  { kX86XchgMR, kMemReg,          IS_STORE | IS_LOAD | IS_TERTIARY_OP | REG_DEF2 | REG_USE02,          { 0, 0, 0x87, 0, 0, 0, 0, 0, false }, "Xchg", "[!0r+!1d],!2r" },

  EXT_0F_R8_FORM_ENCODING_MAP(Movzx8,  0x00, 0xB6, REG_DEF0),
  EXT_0F_ENCODING_MAP(Movzx16, 0x00, 0xB7, REG_DEF0),
  EXT_0F_R8_FORM_ENCODING_MAP(Movsx8,  0x00, 0xBE, REG_DEF0),
  EXT_0F_ENCODING_MAP(Movsx16, 0x00, 0xBF, REG_DEF0),
  EXT_0F_ENCODING_MAP(Movzx8q,  REX_W, 0xB6, REG_DEF0),
  EXT_0F_ENCODING_MAP(Movzx16q, REX_W, 0xB7, REG_DEF0),
  EXT_0F_ENCODING_MAP(Movsx8q,  REX, 0xBE, REG_DEF0),
  EXT_0F_ENCODING_MAP(Movsx16q, REX_W, 0xBF, REG_DEF0),
#undef EXT_0F_ENCODING_MAP

  { kX86Jcc8,  kJcc,  IS_BINARY_OP | IS_BRANCH | NEEDS_FIXUP | USES_CCODES, { 0,             0, 0x70, 0,    0, 0, 0, 0, false }, "Jcc8",  "!1c !0t" },
  { kX86Jcc32, kJcc,  IS_BINARY_OP | IS_BRANCH | NEEDS_FIXUP | USES_CCODES, { 0,             0, 0x0F, 0x80, 0, 0, 0, 0, false }, "Jcc32", "!1c !0t" },
  { kX86Jmp8,  kJmp,  IS_UNARY_OP  | IS_BRANCH | NEEDS_FIXUP,               { 0,             0, 0xEB, 0,    0, 0, 0, 0, false }, "Jmp8",  "!0t" },
  { kX86Jmp32, kJmp,  IS_UNARY_OP  | IS_BRANCH | NEEDS_FIXUP,               { 0,             0, 0xE9, 0,    0, 0, 0, 0, false }, "Jmp32", "!0t" },
  { kX86JmpR,  kJmp,  IS_UNARY_OP  | IS_BRANCH | REG_USE0,                  { 0,             0, 0xFF, 0,    0, 4, 0, 0, false }, "JmpR",  "!0r" },
  { kX86Jecxz8, kJmp, NO_OPERAND   | IS_BRANCH | NEEDS_FIXUP | REG_USEC,    { 0,             0, 0xE3, 0,    0, 0, 0, 0, false }, "Jecxz", "!0t" },
  { kX86JmpT,  kJmp,  IS_UNARY_OP  | IS_BRANCH | IS_LOAD,                   { THREAD_PREFIX, 0, 0xFF, 0,    0, 4, 0, 0, false }, "JmpT",  "fs:[!0d]" },
  { kX86CallR, kCall, IS_UNARY_OP  | IS_BRANCH | REG_USE0,                  { 0,             0, 0xE8, 0,    0, 0, 0, 0, false }, "CallR", "!0r" },
  { kX86CallM, kCall, IS_BINARY_OP | IS_BRANCH | IS_LOAD | REG_USE0,        { 0,             0, 0xFF, 0,    0, 2, 0, 0, false }, "CallM", "[!0r+!1d]" },
  { kX86CallA, kCall, IS_QUAD_OP   | IS_BRANCH | IS_LOAD | REG_USE01,       { 0,             0, 0xFF, 0,    0, 2, 0, 0, false }, "CallA", "[!0r+!1r<<!2d+!3d]" },
  { kX86CallT, kCall, IS_UNARY_OP  | IS_BRANCH | IS_LOAD,                   { THREAD_PREFIX, 0, 0xFF, 0,    0, 2, 0, 0, false }, "CallT", "fs:[!0d]" },
  { kX86CallI, kCall, IS_UNARY_OP  | IS_BRANCH,                             { 0,             0, 0xE8, 0,    0, 0, 0, 4, false }, "CallI", "!0d" },
  { kX86Ret,   kNullary, NO_OPERAND | IS_BRANCH,                            { 0,             0, 0xC3, 0,    0, 0, 0, 0, false }, "Ret", "" },

  { kX86StartOfMethod, kMacro,  IS_UNARY_OP | SETS_CCODES,             { 0, 0, 0,    0, 0, 0, 0, 0, false }, "StartOfMethod", "!0r" },
  { kX86PcRelLoadRA,   kPcRel,  IS_LOAD | IS_QUIN_OP | REG_DEF0_USE12, { 0, 0, 0x8B, 0, 0, 0, 0, 0, false }, "PcRelLoadRA",   "!0r,[!1r+!2r<<!3d+!4p]" },
  { kX86PcRelAdr,      kPcRel,  IS_LOAD | IS_BINARY_OP | REG_DEF0,     { 0, 0, 0xB8, 0, 0, 0, 0, 4, false }, "PcRelAdr",      "!0r,!1d" },
  { kX86RepneScasw,    kNullary, NO_OPERAND | REG_USEA | REG_USEC | SETS_CCODES, { 0x66, 0xF2, 0xAF, 0, 0, 0, 0, 0, false }, "RepNE ScasW", "" },
};

static bool NeedsRex(int32_t raw_reg) {
  return RegStorage::RegNum(raw_reg) > 7;
}

static uint8_t LowRegisterBits(int32_t raw_reg) {
  uint8_t low_reg = RegStorage::RegNum(raw_reg) & kRegNumMask32;  // 3 bits
  DCHECK_LT(low_reg, 8);
  return low_reg;
}

static bool HasModrm(const X86EncodingMap* entry) {
  switch (entry->kind) {
    case kNullary: return false;
    case kRegOpcode: return false;
    default: return true;
  }
}

static bool HasSib(const X86EncodingMap* entry) {
  switch (entry->kind) {
    case kArray: return true;
    case kArrayReg: return true;
    case kRegArray: return true;
    case kArrayImm: return true;
    case kRegArrayImm: return true;
    case kShiftArrayImm: return true;
    case kShiftArrayCl: return true;
    case kArrayCond: return true;
    case kCall:
      switch (entry->opcode) {
        case kX86CallA: return true;
        default: return false;
      }
    case kPcRel: return true;
       switch (entry->opcode) {
         case kX86PcRelLoadRA: return true;
         default: return false;
        }
    default: return false;
  }
}

static bool ModrmIsRegReg(const X86EncodingMap* entry) {
  switch (entry->kind) {
    // There is no modrm for this kind of instruction, therefore the reg doesn't form part of the
    // modrm:
    case kNullary: return true;
    case kRegOpcode: return true;
    case kMovRegImm: return true;
    // Regular modrm value of 3 cases, when there is one register the other register holds an
    // opcode so the base register is special.
    case kReg: return true;
    case kRegReg: return true;
    case kRegRegStore: return true;
    case kRegImm: return true;
    case kRegRegImm: return true;
    case kRegRegImmStore: return true;
    case kShiftRegImm: return true;
    case kShiftRegCl: return true;
    case kRegCond: return true;
    case kRegRegCond: return true;
    case kJmp:
      switch (entry->opcode) {
        case kX86JmpR: return true;
        default: return false;
      }
    case kCall:
      switch (entry->opcode) {
        case kX86CallR: return true;
        default: return false;
      }
    default: return false;
  }
}

static bool IsByteSecondOperand(const X86EncodingMap* entry) {
  return StartsWith(entry->name, "Movzx8") || StartsWith(entry->name, "Movsx8");
}

size_t X86Mir2Lir::ComputeSize(const X86EncodingMap* entry, int32_t raw_reg, int32_t raw_index,
                               int32_t raw_base, int32_t displacement) {
  bool has_modrm = HasModrm(entry);
  bool has_sib = HasSib(entry);
  bool r8_form = entry->skeleton.r8_form;
  bool modrm_is_reg_reg = ModrmIsRegReg(entry);
  if (has_sib) {
    DCHECK(!modrm_is_reg_reg);
  }
  size_t size = 0;
  if (entry->skeleton.prefix1 > 0) {
    ++size;
    if (entry->skeleton.prefix2 > 0) {
      ++size;
    }
  }
  if (cu_->target64 || kIsDebugBuild) {
    bool registers_need_rex_prefix = NeedsRex(raw_reg) || NeedsRex(raw_index) || NeedsRex(raw_base);
    if (r8_form) {
      // Do we need an empty REX prefix to normalize byte registers?
      registers_need_rex_prefix = registers_need_rex_prefix ||
          (RegStorage::RegNum(raw_reg) >= 4 && !IsByteSecondOperand(entry));
      registers_need_rex_prefix = registers_need_rex_prefix ||
          (modrm_is_reg_reg && (RegStorage::RegNum(raw_base) >= 4));
    }
    if (registers_need_rex_prefix) {
      DCHECK(cu_->target64) << "Attempt to use a 64-bit only addressable register "
          << RegStorage::RegNum(raw_reg) << " with instruction " << entry->name;
      if (entry->skeleton.prefix1 != REX_W && entry->skeleton.prefix2 != REX_W
         && entry->skeleton.prefix1 != REX && entry->skeleton.prefix2 != REX) {
        ++size;  // rex
      }
    }
  }
  ++size;  // opcode
  if (entry->skeleton.opcode == 0x0F) {
    ++size;
    if (entry->skeleton.extra_opcode1 == 0x38 || entry->skeleton.extra_opcode1 == 0x3A) {
      ++size;
    }
  }
  if (has_modrm) {
    ++size;  // modrm
  }
  if (!modrm_is_reg_reg) {
    if (has_sib || LowRegisterBits(raw_base) == rs_rX86_SP.GetRegNum()
        || (cu_->target64 && entry->skeleton.prefix1 == THREAD_PREFIX)) {
      // SP requires a SIB byte.
      // GS access also needs a SIB byte for absolute adressing in 64-bit mode.
      ++size;
    }
    if (displacement != 0 || LowRegisterBits(raw_base) == rs_rBP.GetRegNum()) {
      // BP requires an explicit displacement, even when it's 0.
      if (entry->opcode != kX86Lea32RA && entry->opcode != kX86Lea64RA) {
        DCHECK_NE(entry->flags & (IS_LOAD | IS_STORE), UINT64_C(0)) << entry->name;
      }
      size += IS_SIMM8(displacement) ? 1 : 4;
    }
  }
  size += entry->skeleton.immediate_bytes;
  return size;
}

size_t X86Mir2Lir::GetInsnSize(LIR* lir) {
  DCHECK(!IsPseudoLirOp(lir->opcode));
  const X86EncodingMap* entry = &X86Mir2Lir::EncodingMap[lir->opcode];
  DCHECK_EQ(entry->opcode, lir->opcode) << entry->name;

  switch (entry->kind) {
    case kData:
      return 4;  // 4 bytes of data.
    case kNop:
      return lir->operands[0];  // Length of nop is sole operand.
    case kNullary:
      return ComputeSize(entry, NO_REG, NO_REG, NO_REG, 0);
    case kRegOpcode:  // lir operands - 0: reg
      return ComputeSize(entry, NO_REG, NO_REG, lir->operands[0], 0);
    case kReg:  // lir operands - 0: reg
      return ComputeSize(entry, NO_REG, NO_REG, lir->operands[0], 0);
    case kMem:  // lir operands - 0: base, 1: disp
      return ComputeSize(entry, NO_REG, NO_REG, lir->operands[0], lir->operands[1]);
    case kArray:  // lir operands - 0: base, 1: index, 2: scale, 3: disp
      return ComputeSize(entry, NO_REG, lir->operands[1], lir->operands[0], lir->operands[3]);
    case kMemReg:  // lir operands - 0: base, 1: disp, 2: reg
      return ComputeSize(entry, lir->operands[2], NO_REG, lir->operands[0], lir->operands[1]);
    case kMemRegImm:  // lir operands - 0: base, 1: disp, 2: reg 3: immediate
      return ComputeSize(entry, lir->operands[2], NO_REG, lir->operands[0], lir->operands[1]);
    case kArrayReg:  // lir operands - 0: base, 1: index, 2: scale, 3: disp, 4: reg
      return ComputeSize(entry, lir->operands[4], lir->operands[1], lir->operands[0],
                         lir->operands[3]);
    case kThreadReg:  // lir operands - 0: disp, 1: reg
      // Thread displacement size is always 32bit.
      return ComputeSize(entry, lir->operands[1], NO_REG, NO_REG, 0x12345678);
    case kRegReg:  // lir operands - 0: reg1, 1: reg2
      return ComputeSize(entry, lir->operands[0], NO_REG, lir->operands[1], 0);
    case kRegRegStore:  // lir operands - 0: reg2, 1: reg1
      return ComputeSize(entry, lir->operands[1], NO_REG, lir->operands[0], 0);
    case kRegMem:  // lir operands - 0: reg, 1: base, 2: disp
      return ComputeSize(entry, lir->operands[0], NO_REG, lir->operands[1], lir->operands[2]);
    case kRegArray:   // lir operands - 0: reg, 1: base, 2: index, 3: scale, 4: disp
      return ComputeSize(entry, lir->operands[0], lir->operands[2], lir->operands[1],
                         lir->operands[4]);
    case kRegThread:  // lir operands - 0: reg, 1: disp
      // Thread displacement size is always 32bit.
      return ComputeSize(entry, lir->operands[0], NO_REG, NO_REG, 0x12345678);
    case kRegImm: {  // lir operands - 0: reg, 1: immediate
      size_t size = ComputeSize(entry, lir->operands[0], NO_REG, NO_REG, 0);
      // AX opcodes don't require the modrm byte.
      if (entry->skeleton.ax_opcode == 0) {
        return size;
      } else {
        return size - (RegStorage::RegNum(lir->operands[0]) == rs_rAX.GetRegNum() ? 1 : 0);
      }
    }
    case kMemImm:  // lir operands - 0: base, 1: disp, 2: immediate
      return ComputeSize(entry, NO_REG, NO_REG, lir->operands[0], lir->operands[1]);
    case kArrayImm:  // lir operands - 0: base, 1: index, 2: scale, 3: disp 4: immediate
      return ComputeSize(entry, NO_REG, lir->operands[1], lir->operands[0], lir->operands[3]);
    case kThreadImm:  // lir operands - 0: disp, 1: imm
      // Thread displacement size is always 32bit.
      return ComputeSize(entry, NO_REG, NO_REG, NO_REG, 0x12345678);
    case kRegRegImm:  // lir operands - 0: reg1, 1: reg2, 2: imm
      // Note: RegRegImm form passes reg2 as index but encodes it using base.
      return ComputeSize(entry, lir->operands[0], lir->operands[1], NO_REG, 0);
    case kRegRegImmStore:  // lir operands - 0: reg2, 1: reg1, 2: imm
      // Note: RegRegImmStore form passes reg1 as index but encodes it using base.
      return ComputeSize(entry, lir->operands[1], lir->operands[0], NO_REG, 0);
    case kRegMemImm:  // lir operands - 0: reg, 1: base, 2: disp, 3: imm
      return ComputeSize(entry, lir->operands[0], NO_REG, lir->operands[1], lir->operands[2]);
    case kRegArrayImm:  // lir operands - 0: reg, 1: base, 2: index, 3: scale, 4: disp, 5: imm
      return ComputeSize(entry, lir->operands[0], lir->operands[2], lir->operands[1],
                         lir->operands[4]);
    case kMovRegImm:  // lir operands - 0: reg, 1: immediate
    case kMovRegQuadImm:
      return ((entry->skeleton.prefix1 != 0 || NeedsRex(lir->operands[0])) ? 1 : 0) + 1 +
          entry->skeleton.immediate_bytes;
    case kShiftRegImm:  // lir operands - 0: reg, 1: immediate
      // Shift by immediate one has a shorter opcode.
      return ComputeSize(entry, lir->operands[0], NO_REG, NO_REG, 0) -
          (lir->operands[1] == 1 ? 1 : 0);
    case kShiftMemImm:  // lir operands - 0: base, 1: disp, 2: immediate
      // Shift by immediate one has a shorter opcode.
      return ComputeSize(entry, NO_REG, NO_REG, lir->operands[0], lir->operands[1]) -
          (lir->operands[2] == 1 ? 1 : 0);
    case kShiftArrayImm:  // lir operands - 0: base, 1: index, 2: scale, 3: disp 4: immediate
      // Shift by immediate one has a shorter opcode.
      return ComputeSize(entry, NO_REG, lir->operands[1], lir->operands[0], lir->operands[3]) -
          (lir->operands[4] == 1 ? 1 : 0);
    case kShiftRegCl:  // lir operands - 0: reg, 1: cl
      DCHECK_EQ(rs_rCX.GetRegNum(), RegStorage::RegNum(lir->operands[1]));
      // Note: ShiftRegCl form passes reg as reg but encodes it using base.
      return ComputeSize(entry, lir->operands[0], NO_REG, NO_REG, 0);
    case kShiftMemCl:  // lir operands - 0: base, 1: disp, 2: cl
      DCHECK_EQ(rs_rCX.GetRegNum(), RegStorage::RegNum(lir->operands[2]));
      return ComputeSize(entry, NO_REG, NO_REG, lir->operands[0], lir->operands[1]);
    case kShiftArrayCl:  // lir operands - 0: base, 1: index, 2: scale, 3: disp, 4: cl
      DCHECK_EQ(rs_rCX.GetRegNum(), RegStorage::RegNum(lir->operands[4]));
      return ComputeSize(entry, lir->operands[4], lir->operands[1], lir->operands[0],
                         lir->operands[3]);
    case kRegCond:  // lir operands - 0: reg, 1: cond
      return ComputeSize(entry, NO_REG, NO_REG, lir->operands[0], 0);
    case kMemCond:  // lir operands - 0: base, 1: disp, 2: cond
      return ComputeSize(entry, NO_REG, NO_REG, lir->operands[0], lir->operands[1]);
    case kArrayCond:  // lir operands - 0: base, 1: index, 2: scale, 3: disp, 4: cond
      DCHECK_EQ(false, entry->skeleton.r8_form);
      return ComputeSize(entry, NO_REG, lir->operands[1], lir->operands[0], lir->operands[3]);
    case kRegRegCond:  // lir operands - 0: reg1, 1: reg2, 2: cond
      DCHECK_EQ(false, entry->skeleton.r8_form);
      return ComputeSize(entry, lir->operands[0], NO_REG, lir->operands[1], 0);
    case kRegMemCond:  // lir operands - 0: reg, 1: base, 2: disp, 3:cond
      DCHECK_EQ(false, entry->skeleton.r8_form);
      return ComputeSize(entry, lir->operands[0], NO_REG, lir->operands[1], lir->operands[2]);
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
        // Thread displacement size is always 32bit.
        return ComputeSize(entry, NO_REG, NO_REG, NO_REG, 0x12345678);
      } else {
        DCHECK(lir->opcode == kX86JmpR);
        if (NeedsRex(lir->operands[0])) {
          return 3;  // REX.B + opcode + modrm
        } else {
          return 2;  // opcode + modrm
        }
      }
    case kCall:
      switch (lir->opcode) {
        case kX86CallI: return 5;  // opcode 0:disp
        case kX86CallR: return 2;  // opcode modrm
        case kX86CallM:  // lir operands - 0: base, 1: disp
          return ComputeSize(entry, NO_REG, NO_REG, lir->operands[0], lir->operands[1]);
        case kX86CallA:  // lir operands - 0: base, 1: index, 2: scale, 3: disp
          return ComputeSize(entry, NO_REG, lir->operands[1], lir->operands[0], lir->operands[3]);
        case kX86CallT:  // lir operands - 0: disp
          // Thread displacement size is always 32bit.
          return ComputeSize(entry, NO_REG, NO_REG, NO_REG, 0x12345678);
        default:
          break;
      }
      break;
    case kPcRel:
      if (entry->opcode == kX86PcRelLoadRA) {
        // lir operands - 0: reg, 1: base, 2: index, 3: scale, 4: table
        // Force the displacement size to 32bit, it will hold a computed offset later.
        return ComputeSize(entry, lir->operands[0], lir->operands[2], lir->operands[1],
                           0x12345678);
      } else {
        DCHECK_EQ(entry->opcode, kX86PcRelAdr);
        return 5;  // opcode with reg + 4 byte immediate
      }
    case kMacro:  // lir operands - 0: reg
      DCHECK_EQ(lir->opcode, static_cast<int>(kX86StartOfMethod));
      return 5 /* call opcode + 4 byte displacement */ + 1 /* pop reg */ +
          ComputeSize(&X86Mir2Lir::EncodingMap[cu_->target64 ? kX86Sub64RI : kX86Sub32RI],
                      lir->operands[0], NO_REG, NO_REG, 0) -
              // Shorter ax encoding.
              (RegStorage::RegNum(lir->operands[0]) == rs_rAX.GetRegNum()  ? 1 : 0);
    case kUnimplemented:
      break;
  }
  UNIMPLEMENTED(FATAL) << "Unimplemented size encoding for: " << entry->name;
  return 0;
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

void X86Mir2Lir::CheckValidByteRegister(const X86EncodingMap* entry, int32_t raw_reg) {
  if (kIsDebugBuild) {
    // Sanity check r8_form is correctly specified.
    if (entry->skeleton.r8_form) {
      CHECK(strchr(entry->name, '8') != nullptr) << entry->name;
    } else {
      if (entry->skeleton.immediate_bytes != 1) {  // Ignore ...I8 instructions.
        if (!StartsWith(entry->name, "Movzx8") && !StartsWith(entry->name, "Movsx8")
           && !StartsWith(entry->name, "Movzx8q") && !StartsWith(entry->name, "Movsx8q")) {
          CHECK(strchr(entry->name, '8') == nullptr) << entry->name;
        }
      }
    }
    if (RegStorage::RegNum(raw_reg) >= 4) {
      // ah, bh, ch and dh are not valid registers in 32-bit.
      CHECK(cu_->target64 || !entry->skeleton.r8_form)
               << "Invalid register " << static_cast<int>(RegStorage::RegNum(raw_reg))
               << " for instruction " << entry->name << " in "
               << PrettyMethod(cu_->method_idx, *cu_->dex_file);
    }
  }
}

void X86Mir2Lir::EmitPrefix(const X86EncodingMap* entry,
                            int32_t raw_reg_r, int32_t raw_reg_x, int32_t raw_reg_b) {
  // REX.WRXB
  // W - 64-bit operand
  // R - MODRM.reg
  // X - SIB.index
  // B - MODRM.rm/SIB.base
  bool w = (entry->skeleton.prefix1 == REX_W) || (entry->skeleton.prefix2 == REX_W);
  bool r = NeedsRex(raw_reg_r);
  bool x = NeedsRex(raw_reg_x);
  bool b = NeedsRex(raw_reg_b);
  bool r8_form = entry->skeleton.r8_form;
  bool modrm_is_reg_reg = ModrmIsRegReg(entry);

  uint8_t rex = 0;
  if (r8_form) {
    // Do we need an empty REX prefix to normalize byte register addressing?
    if (RegStorage::RegNum(raw_reg_r) >= 4 && !IsByteSecondOperand(entry)) {
      rex |= 0x40;  // REX.0000
    } else if (modrm_is_reg_reg && RegStorage::RegNum(raw_reg_b) >= 4) {
      rex |= 0x40;  // REX.0000
    }
  }
  if (w) {
    rex |= 0x48;  // REX.W000
  }
  if (r) {
    rex |= 0x44;  // REX.0R00
  }
  if (x) {
    rex |= 0x42;  // REX.00X0
  }
  if (b) {
    rex |= 0x41;  // REX.000B
  }
  if (entry->skeleton.prefix1 != 0) {
    if (cu_->target64 && entry->skeleton.prefix1 == THREAD_PREFIX) {
      // 64 bit addresses by GS, not FS.
      code_buffer_.push_back(THREAD_PREFIX_GS);
    } else {
      if (entry->skeleton.prefix1 == REX_W || entry->skeleton.prefix1 == REX) {
        DCHECK(cu_->target64);
        rex |= entry->skeleton.prefix1;
        code_buffer_.push_back(rex);
        rex = 0;
      } else {
        code_buffer_.push_back(entry->skeleton.prefix1);
      }
    }
    if (entry->skeleton.prefix2 != 0) {
      if (entry->skeleton.prefix2 == REX_W || entry->skeleton.prefix1 == REX) {
        DCHECK(cu_->target64);
        rex |= entry->skeleton.prefix2;
        code_buffer_.push_back(rex);
        rex = 0;
      } else {
        code_buffer_.push_back(entry->skeleton.prefix2);
      }
    }
  } else {
    DCHECK_EQ(0, entry->skeleton.prefix2);
  }
  if (rex != 0) {
    DCHECK(cu_->target64);
    code_buffer_.push_back(rex);
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

void X86Mir2Lir::EmitPrefixAndOpcode(const X86EncodingMap* entry,
                                     int32_t raw_reg_r, int32_t raw_reg_x, int32_t raw_reg_b) {
  EmitPrefix(entry, raw_reg_r, raw_reg_x, raw_reg_b);
  EmitOpcode(entry);
}

void X86Mir2Lir::EmitDisp(uint8_t base, int32_t disp) {
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

void X86Mir2Lir::EmitModrmThread(uint8_t reg_or_opcode) {
  if (cu_->target64) {
    // Absolute adressing for GS access.
    uint8_t modrm = (0 << 6) | (reg_or_opcode << 3) | rs_rX86_SP.GetRegNum();
    code_buffer_.push_back(modrm);
    uint8_t sib = (0/*TIMES_1*/ << 6) | (rs_rX86_SP.GetRegNum() << 3) | rs_rBP.GetRegNum();
    code_buffer_.push_back(sib);
  } else {
    uint8_t modrm = (0 << 6) | (reg_or_opcode << 3) | rs_rBP.GetRegNum();
    code_buffer_.push_back(modrm);
  }
}

void X86Mir2Lir::EmitModrmDisp(uint8_t reg_or_opcode, uint8_t base, int32_t disp) {
  DCHECK_LT(reg_or_opcode, 8);
  DCHECK_LT(base, 8);
  uint8_t modrm = (ModrmForDisp(base, disp) << 6) | (reg_or_opcode << 3) | base;
  code_buffer_.push_back(modrm);
  if (base == rs_rX86_SP.GetRegNum()) {
    // Special SIB for SP base
    code_buffer_.push_back(0 << 6 | rs_rX86_SP.GetRegNum() << 3 | rs_rX86_SP.GetRegNum());
  }
  EmitDisp(base, disp);
}

void X86Mir2Lir::EmitModrmSibDisp(uint8_t reg_or_opcode, uint8_t base, uint8_t index,
                                  int scale, int32_t disp) {
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

void X86Mir2Lir::EmitImm(const X86EncodingMap* entry, int64_t imm) {
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
      DCHECK(IS_SIMM32(imm));
      code_buffer_.push_back(imm & 0xFF);
      code_buffer_.push_back((imm >> 8) & 0xFF);
      code_buffer_.push_back((imm >> 16) & 0xFF);
      code_buffer_.push_back((imm >> 24) & 0xFF);
      break;
    case 8:
      code_buffer_.push_back(imm & 0xFF);
      code_buffer_.push_back((imm >> 8) & 0xFF);
      code_buffer_.push_back((imm >> 16) & 0xFF);
      code_buffer_.push_back((imm >> 24) & 0xFF);
      code_buffer_.push_back((imm >> 32) & 0xFF);
      code_buffer_.push_back((imm >> 40) & 0xFF);
      code_buffer_.push_back((imm >> 48) & 0xFF);
      code_buffer_.push_back((imm >> 56) & 0xFF);
      break;
    default:
      LOG(FATAL) << "Unexpected immediate bytes (" << entry->skeleton.immediate_bytes
                 << ") for instruction: " << entry->name;
      break;
  }
}

void X86Mir2Lir::EmitNullary(const X86EncodingMap* entry) {
  DCHECK_EQ(false, entry->skeleton.r8_form);
  EmitPrefixAndOpcode(entry, NO_REG, NO_REG, NO_REG);
  DCHECK_EQ(0, entry->skeleton.modrm_opcode);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  DCHECK_EQ(0, entry->skeleton.immediate_bytes);
}

void X86Mir2Lir::EmitOpRegOpcode(const X86EncodingMap* entry, int32_t raw_reg) {
  DCHECK_EQ(false, entry->skeleton.r8_form);
  EmitPrefixAndOpcode(entry, NO_REG, NO_REG, raw_reg);
  // There's no 3-byte instruction with +rd
  DCHECK(entry->skeleton.opcode != 0x0F ||
         (entry->skeleton.extra_opcode1 != 0x38 && entry->skeleton.extra_opcode1 != 0x3A));
  DCHECK(!RegStorage::IsFloat(raw_reg));
  uint8_t low_reg = LowRegisterBits(raw_reg);
  code_buffer_.back() += low_reg;
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  DCHECK_EQ(0, entry->skeleton.immediate_bytes);
}

void X86Mir2Lir::EmitOpReg(const X86EncodingMap* entry, int32_t raw_reg) {
  CheckValidByteRegister(entry, raw_reg);
  EmitPrefixAndOpcode(entry, NO_REG, NO_REG, raw_reg);
  uint8_t low_reg = LowRegisterBits(raw_reg);
  uint8_t modrm = (3 << 6) | (entry->skeleton.modrm_opcode << 3) | low_reg;
  code_buffer_.push_back(modrm);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  DCHECK_EQ(0, entry->skeleton.immediate_bytes);
}

void X86Mir2Lir::EmitOpMem(const X86EncodingMap* entry, int32_t raw_base, int32_t disp) {
  DCHECK_EQ(false, entry->skeleton.r8_form);
  EmitPrefix(entry, NO_REG, NO_REG, raw_base);
  code_buffer_.push_back(entry->skeleton.opcode);
  DCHECK_NE(0x0F, entry->skeleton.opcode);
  DCHECK_EQ(0, entry->skeleton.extra_opcode1);
  DCHECK_EQ(0, entry->skeleton.extra_opcode2);
  uint8_t low_base = LowRegisterBits(raw_base);
  EmitModrmDisp(entry->skeleton.modrm_opcode, low_base, disp);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  DCHECK_EQ(0, entry->skeleton.immediate_bytes);
}

void X86Mir2Lir::EmitOpArray(const X86EncodingMap* entry, int32_t raw_base, int32_t raw_index,
                             int scale, int32_t disp) {
  DCHECK_EQ(false, entry->skeleton.r8_form);
  EmitPrefixAndOpcode(entry, NO_REG, raw_index, raw_base);
  uint8_t low_index = LowRegisterBits(raw_index);
  uint8_t low_base = LowRegisterBits(raw_base);
  EmitModrmSibDisp(entry->skeleton.modrm_opcode, low_base, low_index, scale, disp);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  DCHECK_EQ(0, entry->skeleton.immediate_bytes);
}

void X86Mir2Lir::EmitMemReg(const X86EncodingMap* entry, int32_t raw_base, int32_t disp,
                            int32_t raw_reg) {
  CheckValidByteRegister(entry, raw_reg);
  EmitPrefixAndOpcode(entry, raw_reg, NO_REG, raw_base);
  uint8_t low_reg = LowRegisterBits(raw_reg);
  uint8_t low_base = LowRegisterBits(raw_base);
  EmitModrmDisp(low_reg, low_base, disp);
  DCHECK_EQ(0, entry->skeleton.modrm_opcode);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  DCHECK_EQ(0, entry->skeleton.immediate_bytes);
}

void X86Mir2Lir::EmitRegMem(const X86EncodingMap* entry, int32_t raw_reg, int32_t raw_base,
                            int32_t disp) {
  // Opcode will flip operands.
  EmitMemReg(entry, raw_base, disp, raw_reg);
}

void X86Mir2Lir::EmitRegArray(const X86EncodingMap* entry, int32_t raw_reg, int32_t raw_base,
                              int32_t raw_index, int scale, int32_t disp) {
  CheckValidByteRegister(entry, raw_reg);
  EmitPrefixAndOpcode(entry, raw_reg, raw_index, raw_base);
  uint8_t low_reg = LowRegisterBits(raw_reg);
  uint8_t low_index = LowRegisterBits(raw_index);
  uint8_t low_base = LowRegisterBits(raw_base);
  EmitModrmSibDisp(low_reg, low_base, low_index, scale, disp);
  DCHECK_EQ(0, entry->skeleton.modrm_opcode);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  DCHECK_EQ(0, entry->skeleton.immediate_bytes);
}

void X86Mir2Lir::EmitArrayReg(const X86EncodingMap* entry, int32_t raw_base, int32_t raw_index,
                              int scale, int32_t disp, int32_t raw_reg) {
  // Opcode will flip operands.
  EmitRegArray(entry, raw_reg, raw_base, raw_index, scale, disp);
}

void X86Mir2Lir::EmitMemImm(const X86EncodingMap* entry, int32_t raw_base, int32_t disp,
                            int32_t imm) {
  DCHECK_EQ(false, entry->skeleton.r8_form);
  EmitPrefixAndOpcode(entry, NO_REG, NO_REG, raw_base);
  uint8_t low_base = LowRegisterBits(raw_base);
  EmitModrmDisp(entry->skeleton.modrm_opcode, low_base, disp);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  EmitImm(entry, imm);
}

void X86Mir2Lir::EmitArrayImm(const X86EncodingMap* entry,
                              int32_t raw_base, int32_t raw_index, int scale, int32_t disp,
                              int32_t imm) {
  DCHECK_EQ(false, entry->skeleton.r8_form);
  EmitPrefixAndOpcode(entry, NO_REG, raw_index, raw_base);
  uint8_t low_index = LowRegisterBits(raw_index);
  uint8_t low_base = LowRegisterBits(raw_base);
  EmitModrmSibDisp(entry->skeleton.modrm_opcode, low_base, low_index, scale, disp);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  EmitImm(entry, imm);
}

void X86Mir2Lir::EmitRegThread(const X86EncodingMap* entry, int32_t raw_reg, int32_t disp) {
  DCHECK_EQ(false, entry->skeleton.r8_form);
  DCHECK_NE(entry->skeleton.prefix1, 0);
  EmitPrefixAndOpcode(entry, raw_reg, NO_REG, NO_REG);
  uint8_t low_reg = LowRegisterBits(raw_reg);
  EmitModrmThread(low_reg);
  code_buffer_.push_back(disp & 0xFF);
  code_buffer_.push_back((disp >> 8) & 0xFF);
  code_buffer_.push_back((disp >> 16) & 0xFF);
  code_buffer_.push_back((disp >> 24) & 0xFF);
  DCHECK_EQ(0, entry->skeleton.modrm_opcode);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  DCHECK_EQ(0, entry->skeleton.immediate_bytes);
}

void X86Mir2Lir::EmitRegReg(const X86EncodingMap* entry, int32_t raw_reg1, int32_t raw_reg2) {
  if (!IsByteSecondOperand(entry)) {
    CheckValidByteRegister(entry, raw_reg1);
  }
  CheckValidByteRegister(entry, raw_reg2);
  EmitPrefixAndOpcode(entry, raw_reg1, NO_REG, raw_reg2);
  uint8_t low_reg1 = LowRegisterBits(raw_reg1);
  uint8_t low_reg2 = LowRegisterBits(raw_reg2);
  uint8_t modrm = (3 << 6) | (low_reg1 << 3) | low_reg2;
  code_buffer_.push_back(modrm);
  DCHECK_EQ(0, entry->skeleton.modrm_opcode);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  DCHECK_EQ(0, entry->skeleton.immediate_bytes);
}

void X86Mir2Lir::EmitRegRegImm(const X86EncodingMap* entry, int32_t raw_reg1, int32_t raw_reg2,
                               int32_t imm) {
  DCHECK_EQ(false, entry->skeleton.r8_form);
  EmitPrefixAndOpcode(entry, raw_reg1, NO_REG, raw_reg2);
  uint8_t low_reg1 = LowRegisterBits(raw_reg1);
  uint8_t low_reg2 = LowRegisterBits(raw_reg2);
  uint8_t modrm = (3 << 6) | (low_reg1 << 3) | low_reg2;
  code_buffer_.push_back(modrm);
  DCHECK_EQ(0, entry->skeleton.modrm_opcode);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  EmitImm(entry, imm);
}

void X86Mir2Lir::EmitRegMemImm(const X86EncodingMap* entry,
                               int32_t raw_reg, int32_t raw_base, int disp, int32_t imm) {
  DCHECK(!RegStorage::IsFloat(raw_reg));
  CheckValidByteRegister(entry, raw_reg);
  EmitPrefixAndOpcode(entry, raw_reg, NO_REG, raw_base);
  uint8_t low_reg = LowRegisterBits(raw_reg);
  uint8_t low_base = LowRegisterBits(raw_base);
  EmitModrmDisp(low_reg, low_base, disp);
  DCHECK_EQ(0, entry->skeleton.modrm_opcode);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  EmitImm(entry, imm);
}

void X86Mir2Lir::EmitMemRegImm(const X86EncodingMap* entry,
                               int32_t raw_base, int32_t disp, int32_t raw_reg, int32_t imm) {
  // Opcode will flip operands.
  EmitRegMemImm(entry, raw_reg, raw_base, disp, imm);
}

void X86Mir2Lir::EmitRegImm(const X86EncodingMap* entry, int32_t raw_reg, int32_t imm) {
  CheckValidByteRegister(entry, raw_reg);
  EmitPrefix(entry, NO_REG, NO_REG, raw_reg);
  if (RegStorage::RegNum(raw_reg) == rs_rAX.GetRegNum() && entry->skeleton.ax_opcode != 0) {
    code_buffer_.push_back(entry->skeleton.ax_opcode);
  } else {
    uint8_t low_reg = LowRegisterBits(raw_reg);
    EmitOpcode(entry);
    uint8_t modrm = (3 << 6) | (entry->skeleton.modrm_opcode << 3) | low_reg;
    code_buffer_.push_back(modrm);
  }
  EmitImm(entry, imm);
}

void X86Mir2Lir::EmitThreadImm(const X86EncodingMap* entry, int32_t disp, int32_t imm) {
  DCHECK_EQ(false, entry->skeleton.r8_form);
  EmitPrefixAndOpcode(entry, NO_REG, NO_REG, NO_REG);
  EmitModrmThread(entry->skeleton.modrm_opcode);
  code_buffer_.push_back(disp & 0xFF);
  code_buffer_.push_back((disp >> 8) & 0xFF);
  code_buffer_.push_back((disp >> 16) & 0xFF);
  code_buffer_.push_back((disp >> 24) & 0xFF);
  EmitImm(entry, imm);
  DCHECK_EQ(entry->skeleton.ax_opcode, 0);
}

void X86Mir2Lir::EmitMovRegImm(const X86EncodingMap* entry, int32_t raw_reg, int64_t imm) {
  DCHECK_EQ(false, entry->skeleton.r8_form);
  EmitPrefix(entry, NO_REG, NO_REG, raw_reg);
  uint8_t low_reg = LowRegisterBits(raw_reg);
  code_buffer_.push_back(0xB8 + low_reg);
  switch (entry->skeleton.immediate_bytes) {
    case 4:
      code_buffer_.push_back(imm & 0xFF);
      code_buffer_.push_back((imm >> 8) & 0xFF);
      code_buffer_.push_back((imm >> 16) & 0xFF);
      code_buffer_.push_back((imm >> 24) & 0xFF);
      break;
    case 8:
      code_buffer_.push_back(imm & 0xFF);
      code_buffer_.push_back((imm >> 8) & 0xFF);
      code_buffer_.push_back((imm >> 16) & 0xFF);
      code_buffer_.push_back((imm >> 24) & 0xFF);
      code_buffer_.push_back((imm >> 32) & 0xFF);
      code_buffer_.push_back((imm >> 40) & 0xFF);
      code_buffer_.push_back((imm >> 48) & 0xFF);
      code_buffer_.push_back((imm >> 56) & 0xFF);
      break;
    default:
      LOG(FATAL) << "Unsupported immediate size for EmitMovRegImm: "
                 << static_cast<uint32_t>(entry->skeleton.immediate_bytes);
  }
}

void X86Mir2Lir::EmitShiftRegImm(const X86EncodingMap* entry, int32_t raw_reg, int32_t imm) {
  CheckValidByteRegister(entry, raw_reg);
  EmitPrefix(entry, NO_REG, NO_REG, raw_reg);
  if (imm != 1) {
    code_buffer_.push_back(entry->skeleton.opcode);
  } else {
    // Shorter encoding for 1 bit shift
    code_buffer_.push_back(entry->skeleton.ax_opcode);
  }
  DCHECK_NE(0x0F, entry->skeleton.opcode);
  DCHECK_EQ(0, entry->skeleton.extra_opcode1);
  DCHECK_EQ(0, entry->skeleton.extra_opcode2);
  uint8_t low_reg = LowRegisterBits(raw_reg);
  uint8_t modrm = (3 << 6) | (entry->skeleton.modrm_opcode << 3) | low_reg;
  code_buffer_.push_back(modrm);
  if (imm != 1) {
    DCHECK_EQ(entry->skeleton.immediate_bytes, 1);
    DCHECK(IS_SIMM8(imm));
    code_buffer_.push_back(imm & 0xFF);
  }
}

void X86Mir2Lir::EmitShiftRegCl(const X86EncodingMap* entry, int32_t raw_reg, int32_t raw_cl) {
  CheckValidByteRegister(entry, raw_reg);
  DCHECK_EQ(rs_rCX.GetRegNum(), RegStorage::RegNum(raw_cl));
  EmitPrefix(entry, NO_REG, NO_REG, raw_reg);
  code_buffer_.push_back(entry->skeleton.opcode);
  DCHECK_NE(0x0F, entry->skeleton.opcode);
  DCHECK_EQ(0, entry->skeleton.extra_opcode1);
  DCHECK_EQ(0, entry->skeleton.extra_opcode2);
  uint8_t low_reg = LowRegisterBits(raw_reg);
  uint8_t modrm = (3 << 6) | (entry->skeleton.modrm_opcode << 3) | low_reg;
  code_buffer_.push_back(modrm);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  DCHECK_EQ(0, entry->skeleton.immediate_bytes);
}

void X86Mir2Lir::EmitShiftMemCl(const X86EncodingMap* entry, int32_t raw_base,
                                int32_t displacement, int32_t raw_cl) {
  DCHECK_EQ(false, entry->skeleton.r8_form);
  DCHECK_EQ(rs_rCX.GetRegNum(), RegStorage::RegNum(raw_cl));
  EmitPrefix(entry, NO_REG, NO_REG, raw_base);
  code_buffer_.push_back(entry->skeleton.opcode);
  DCHECK_NE(0x0F, entry->skeleton.opcode);
  DCHECK_EQ(0, entry->skeleton.extra_opcode1);
  DCHECK_EQ(0, entry->skeleton.extra_opcode2);
  uint8_t low_base = LowRegisterBits(raw_base);
  EmitModrmDisp(entry->skeleton.modrm_opcode, low_base, displacement);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  DCHECK_EQ(0, entry->skeleton.immediate_bytes);
}

void X86Mir2Lir::EmitShiftMemImm(const X86EncodingMap* entry, int32_t raw_base, int32_t disp,
                                 int32_t imm) {
  DCHECK_EQ(false, entry->skeleton.r8_form);
  EmitPrefix(entry, NO_REG, NO_REG, raw_base);
  if (imm != 1) {
    code_buffer_.push_back(entry->skeleton.opcode);
  } else {
    // Shorter encoding for 1 bit shift
    code_buffer_.push_back(entry->skeleton.ax_opcode);
  }
  DCHECK_NE(0x0F, entry->skeleton.opcode);
  DCHECK_EQ(0, entry->skeleton.extra_opcode1);
  DCHECK_EQ(0, entry->skeleton.extra_opcode2);
  uint8_t low_base = LowRegisterBits(raw_base);
  EmitModrmDisp(entry->skeleton.modrm_opcode, low_base, disp);
  if (imm != 1) {
    DCHECK_EQ(entry->skeleton.immediate_bytes, 1);
    DCHECK(IS_SIMM8(imm));
    code_buffer_.push_back(imm & 0xFF);
  }
}

void X86Mir2Lir::EmitRegCond(const X86EncodingMap* entry, int32_t raw_reg, int32_t cc) {
  CheckValidByteRegister(entry, raw_reg);
  EmitPrefix(entry, NO_REG, NO_REG, raw_reg);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  DCHECK_EQ(0x0F, entry->skeleton.opcode);
  code_buffer_.push_back(0x0F);
  DCHECK_EQ(0x90, entry->skeleton.extra_opcode1);
  DCHECK_GE(cc, 0);
  DCHECK_LT(cc, 16);
  code_buffer_.push_back(0x90 | cc);
  DCHECK_EQ(0, entry->skeleton.extra_opcode2);
  uint8_t low_reg = LowRegisterBits(raw_reg);
  uint8_t modrm = (3 << 6) | (entry->skeleton.modrm_opcode << 3) | low_reg;
  code_buffer_.push_back(modrm);
  DCHECK_EQ(entry->skeleton.immediate_bytes, 0);
}

void X86Mir2Lir::EmitMemCond(const X86EncodingMap* entry, int32_t raw_base, int32_t disp,
                             int32_t cc) {
  DCHECK_EQ(false, entry->skeleton.r8_form);
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
  DCHECK_GE(cc, 0);
  DCHECK_LT(cc, 16);
  code_buffer_.push_back(0x90 | cc);
  DCHECK_EQ(0, entry->skeleton.extra_opcode2);
  uint8_t low_base = LowRegisterBits(raw_base);
  EmitModrmDisp(entry->skeleton.modrm_opcode, low_base, disp);
  DCHECK_EQ(entry->skeleton.immediate_bytes, 0);
}

void X86Mir2Lir::EmitRegRegCond(const X86EncodingMap* entry, int32_t raw_reg1, int32_t raw_reg2,
                                int32_t cc) {
  // Generate prefix and opcode without the condition.
  DCHECK_EQ(false, entry->skeleton.r8_form);
  EmitPrefixAndOpcode(entry, raw_reg1, NO_REG, raw_reg2);

  // Now add the condition. The last byte of opcode is the one that receives it.
  DCHECK_GE(cc, 0);
  DCHECK_LT(cc, 16);
  code_buffer_.back() += cc;

  // Not expecting to have to encode immediate or do anything special for ModR/M since there are
  // two registers.
  DCHECK_EQ(0, entry->skeleton.immediate_bytes);
  DCHECK_EQ(0, entry->skeleton.modrm_opcode);

  // For register to register encoding, the mod is 3.
  const uint8_t mod = (3 << 6);

  // Encode the ModR/M byte now.
  uint8_t low_reg1 = LowRegisterBits(raw_reg1);
  uint8_t low_reg2 = LowRegisterBits(raw_reg2);
  const uint8_t modrm = mod | (low_reg1 << 3) | low_reg2;
  code_buffer_.push_back(modrm);
}

void X86Mir2Lir::EmitRegMemCond(const X86EncodingMap* entry, int32_t raw_reg1, int32_t raw_base,
                                int32_t disp, int32_t cc) {
  // Generate prefix and opcode without the condition.
  DCHECK_EQ(false, entry->skeleton.r8_form);
  EmitPrefixAndOpcode(entry, raw_reg1, NO_REG, raw_base);

  // Now add the condition. The last byte of opcode is the one that receives it.
  DCHECK_GE(cc, 0);
  DCHECK_LT(cc, 16);
  code_buffer_.back() += cc;

  // Not expecting to have to encode immediate or do anything special for ModR/M since there are
  // two registers.
  DCHECK_EQ(0, entry->skeleton.immediate_bytes);
  DCHECK_EQ(0, entry->skeleton.modrm_opcode);

  uint8_t low_reg1 = LowRegisterBits(raw_reg1);
  uint8_t low_base = LowRegisterBits(raw_base);
  EmitModrmDisp(low_reg1, low_base, disp);
}

void X86Mir2Lir::EmitJmp(const X86EncodingMap* entry, int32_t rel) {
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
    DCHECK_EQ(false, entry->skeleton.r8_form);
    EmitPrefix(entry, NO_REG, NO_REG, rel);
    code_buffer_.push_back(entry->skeleton.opcode);
    uint8_t low_reg = LowRegisterBits(rel);
    uint8_t modrm = (3 << 6) | (entry->skeleton.modrm_opcode << 3) | low_reg;
    code_buffer_.push_back(modrm);
  }
}

void X86Mir2Lir::EmitJcc(const X86EncodingMap* entry, int32_t rel, int32_t cc) {
  DCHECK_GE(cc, 0);
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

void X86Mir2Lir::EmitCallMem(const X86EncodingMap* entry, int32_t raw_base, int32_t disp) {
  DCHECK_EQ(false, entry->skeleton.r8_form);
  EmitPrefixAndOpcode(entry, NO_REG, NO_REG, raw_base);
  uint8_t low_base = LowRegisterBits(raw_base);
  EmitModrmDisp(entry->skeleton.modrm_opcode, low_base, disp);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  DCHECK_EQ(0, entry->skeleton.immediate_bytes);
}

void X86Mir2Lir::EmitCallImmediate(const X86EncodingMap* entry, int32_t disp) {
  DCHECK_EQ(false, entry->skeleton.r8_form);
  EmitPrefixAndOpcode(entry, NO_REG, NO_REG, NO_REG);
  DCHECK_EQ(4, entry->skeleton.immediate_bytes);
  code_buffer_.push_back(disp & 0xFF);
  code_buffer_.push_back((disp >> 8) & 0xFF);
  code_buffer_.push_back((disp >> 16) & 0xFF);
  code_buffer_.push_back((disp >> 24) & 0xFF);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
}

void X86Mir2Lir::EmitCallThread(const X86EncodingMap* entry, int32_t disp) {
  DCHECK_EQ(false, entry->skeleton.r8_form);
  DCHECK_NE(entry->skeleton.prefix1, 0);
  EmitPrefixAndOpcode(entry, NO_REG, NO_REG, NO_REG);
  EmitModrmThread(entry->skeleton.modrm_opcode);
  code_buffer_.push_back(disp & 0xFF);
  code_buffer_.push_back((disp >> 8) & 0xFF);
  code_buffer_.push_back((disp >> 16) & 0xFF);
  code_buffer_.push_back((disp >> 24) & 0xFF);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
  DCHECK_EQ(0, entry->skeleton.immediate_bytes);
}

void X86Mir2Lir::EmitPcRel(const X86EncodingMap* entry, int32_t raw_reg, int32_t raw_base_or_table,
                           int32_t raw_index, int scale, int32_t table_or_disp) {
  int disp;
  if (entry->opcode == kX86PcRelLoadRA) {
    Mir2Lir::EmbeddedData *tab_rec =
        reinterpret_cast<Mir2Lir::EmbeddedData*>(UnwrapPointer(table_or_disp));
    disp = tab_rec->offset;
  } else {
    DCHECK(entry->opcode == kX86PcRelAdr);
    Mir2Lir::EmbeddedData *tab_rec =
        reinterpret_cast<Mir2Lir::EmbeddedData*>(UnwrapPointer(raw_base_or_table));
    disp = tab_rec->offset;
  }
  if (entry->opcode == kX86PcRelLoadRA) {
    DCHECK_EQ(false, entry->skeleton.r8_form);
    EmitPrefix(entry, raw_reg, raw_index, raw_base_or_table);
    code_buffer_.push_back(entry->skeleton.opcode);
    DCHECK_NE(0x0F, entry->skeleton.opcode);
    DCHECK_EQ(0, entry->skeleton.extra_opcode1);
    DCHECK_EQ(0, entry->skeleton.extra_opcode2);
    uint8_t low_reg = LowRegisterBits(raw_reg);
    uint8_t modrm = (2 << 6) | (low_reg << 3) | rs_rX86_SP.GetRegNum();
    code_buffer_.push_back(modrm);
    DCHECK_LT(scale, 4);
    uint8_t low_base_or_table = LowRegisterBits(raw_base_or_table);
    uint8_t low_index = LowRegisterBits(raw_index);
    uint8_t sib = (scale << 6) | (low_index << 3) | low_base_or_table;
    code_buffer_.push_back(sib);
    DCHECK_EQ(0, entry->skeleton.immediate_bytes);
  } else {
    uint8_t low_reg = LowRegisterBits(raw_reg);
    code_buffer_.push_back(entry->skeleton.opcode + low_reg);
  }
  code_buffer_.push_back(disp & 0xFF);
  code_buffer_.push_back((disp >> 8) & 0xFF);
  code_buffer_.push_back((disp >> 16) & 0xFF);
  code_buffer_.push_back((disp >> 24) & 0xFF);
  DCHECK_EQ(0, entry->skeleton.modrm_opcode);
  DCHECK_EQ(0, entry->skeleton.ax_opcode);
}

void X86Mir2Lir::EmitMacro(const X86EncodingMap* entry, int32_t raw_reg, int32_t offset) {
  DCHECK_EQ(entry->opcode, kX86StartOfMethod) << entry->name;
  DCHECK_EQ(false, entry->skeleton.r8_form);
  EmitPrefix(entry, raw_reg, NO_REG, NO_REG);
  code_buffer_.push_back(0xE8);  // call +0
  code_buffer_.push_back(0);
  code_buffer_.push_back(0);
  code_buffer_.push_back(0);
  code_buffer_.push_back(0);

  uint8_t low_reg = LowRegisterBits(raw_reg);
  code_buffer_.push_back(0x58 + low_reg);  // pop reg

  EmitRegImm(&X86Mir2Lir::EncodingMap[cu_->target64 ? kX86Sub64RI : kX86Sub32RI],
             raw_reg, offset + 5 /* size of call +0 */);
}

void X86Mir2Lir::EmitUnimplemented(const X86EncodingMap* entry, LIR* lir) {
  UNIMPLEMENTED(WARNING) << "encoding kind for " << entry->name << " "
                         << BuildInsnString(entry->fmt, lir, 0);
  for (size_t i = 0; i < GetInsnSize(lir); ++i) {
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
            lir->flags.size = GetInsnSize(lir);
            DCHECK(lir->u.m.def_mask->Equals(kEncodeAll));
            DCHECK(lir->u.m.use_mask->Equals(kEncodeAll));
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
            lir->flags.size = GetInsnSize(lir);
            DCHECK(lir->u.m.def_mask->Equals(kEncodeAll));
            DCHECK(lir->u.m.use_mask->Equals(kEncodeAll));
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
      case kNullary:  // 1 byte of opcode and possible prefixes.
        EmitNullary(entry);
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
      case kMemRegImm:  // lir operands - 0: base, 1: disp, 2: reg 3: immediate
        EmitMemRegImm(entry, lir->operands[0], lir->operands[1], lir->operands[2],
                      lir->operands[3]);
        break;
      case kRegRegImm:  // lir operands - 0: reg1, 1: reg2, 2: imm
        EmitRegRegImm(entry, lir->operands[0], lir->operands[1], lir->operands[2]);
        break;
      case kRegRegImmStore:   // lir operands - 0: reg2, 1: reg1, 2: imm
        EmitRegRegImm(entry, lir->operands[1], lir->operands[0], lir->operands[2]);
        break;
      case kRegMemImm:  // lir operands - 0: reg, 1: base, 2: disp, 3: imm
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
      case kMovRegQuadImm: {
          int64_t value = static_cast<int64_t>(static_cast<int64_t>(lir->operands[1]) << 32 |
                          static_cast<uint32_t>(lir->operands[2]));
          EmitMovRegImm(entry, lir->operands[0], value);
        }
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
        EmitRegMemCond(entry, lir->operands[0], lir->operands[1], lir->operands[2],
                       lir->operands[3]);
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
      case kMacro:  // lir operands - 0: reg
        EmitMacro(entry, lir->operands[0], lir->offset);
        break;
      case kNop:  // TODO: these instruction kinds are missing implementations.
      case kThreadReg:
      case kRegArrayImm:
      case kShiftArrayImm:
      case kShiftArrayCl:
      case kArrayCond:
      case kUnimplemented:
        EmitUnimplemented(entry, lir);
        break;
    }
    DCHECK_EQ(lir->flags.size, GetInsnSize(lir));
    CHECK_EQ(lir->flags.size, code_buffer_.size() - starting_cbuf_size)
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

  if (const_vectors_ != nullptr) {
    /* assign offsets to vector literals */

    // First, get offset to 12 mod 16 to align to 16 byte boundary.
    // This will ensure that the vector is 16 byte aligned, as the procedure is
    // always aligned at at 4 mod 16.
    int align_size = (16-4) - (offset & 0xF);
    if (align_size < 0) {
      align_size += 16;
    }

    offset += align_size;

    // Now assign each literal the right offset.
    for (LIR *p = const_vectors_; p != nullptr; p = p->next) {
      p->offset = offset;
      offset += 16;
    }
  }

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
