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
#include "MipsLIR.h"
#include "Codegen.h"
#include <sys/mman.h>           /* for protection change */

namespace art {

#define MAX_ASSEMBLER_RETRIES 50

/*
 * opcode: MipsOpCode enum
 * skeleton: pre-designated bit-pattern for this opcode
 * k0: key to applying ds/de
 * ds: dest start bit position
 * de: dest end bit position
 * k1: key to applying s1s/s1e
 * s1s: src1 start bit position
 * s1e: src1 end bit position
 * k2: key to applying s2s/s2e
 * s2s: src2 start bit position
 * s2e: src2 end bit position
 * operands: number of operands (for sanity check purposes)
 * name: mnemonic name
 * fmt: for pretty-printing
 */
#define ENCODING_MAP(opcode, skeleton, k0, ds, de, k1, s1s, s1e, k2, s2s, s2e, \
                     k3, k3s, k3e, flags, name, fmt, size) \
        {skeleton, {{k0, ds, de}, {k1, s1s, s1e}, {k2, s2s, s2e}, \
                    {k3, k3s, k3e}}, opcode, flags, name, fmt, size}

/* Instruction dump string format keys: !pf, where "!" is the start
 * of the key, "p" is which numeric operand to use and "f" is the
 * print format.
 *
 * [p]ositions:
 *     0 -> operands[0] (dest)
 *     1 -> operands[1] (src1)
 *     2 -> operands[2] (src2)
 *     3 -> operands[3] (extra)
 *
 * [f]ormats:
 *     h -> 4-digit hex
 *     d -> decimal
 *     E -> decimal*4
 *     F -> decimal*2
 *     c -> branch condition (beq, bne, etc.)
 *     t -> pc-relative target
 *     T -> pc-region target
 *     u -> 1st half of bl[x] target
 *     v -> 2nd half ob bl[x] target
 *     R -> register list
 *     s -> single precision floating point register
 *     S -> double precision floating point register
 *     m -> Thumb2 modified immediate
 *     n -> complimented Thumb2 modified immediate
 *     M -> Thumb2 16-bit zero-extended immediate
 *     b -> 4-digit binary
 *
 *  [!] escape.  To insert "!", use "!!"
 */
/* NOTE: must be kept in sync with enum MipsOpcode from LIR.h */
MipsEncodingMap EncodingMap[kMipsLast] = {
    ENCODING_MAP(kMips32BitData, 0x00000000,
                 kFmtBitBlt, 31, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP,
                 "data", "0x!0h(!0d)", 2),
    ENCODING_MAP(kMipsAddiu, 0x24000000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 25, 21, kFmtBitBlt, 15, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "addiu", "!0r,!1r,0x!2h(!2d)", 2),
    ENCODING_MAP(kMipsAddu, 0x00000021,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 25, 21, kFmtBitBlt, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "addu", "!0r,!1r,!2r", 2),
    ENCODING_MAP(kMipsAnd, 0x00000024,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 25, 21, kFmtBitBlt, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "and", "!0r,!1r,!2r", 2),
    ENCODING_MAP(kMipsAndi, 0x30000000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 25, 21, kFmtBitBlt, 15, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "andi", "!0r,!1r,0x!2h(!2d)", 2),
    ENCODING_MAP(kMipsB, 0x10000000,
                 kFmtBitBlt, 15, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, NO_OPERAND | IS_BRANCH,
                 "b", "!0t", 2),
    ENCODING_MAP(kMipsBal, 0x04110000,
                 kFmtBitBlt, 15, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, NO_OPERAND | IS_BRANCH | REG_DEF_LR,
                 "bal", "!0t", 2),
    ENCODING_MAP(kMipsBeq, 0x10000000,
                 kFmtBitBlt, 25, 21, kFmtBitBlt, 20, 16, kFmtBitBlt, 15, 0,
                 kFmtUnused, -1, -1, IS_BINARY_OP | IS_BRANCH | REG_USE01,
                 "beq", "!0r,!1r,!2t", 2),
    ENCODING_MAP(kMipsBeqz, 0x10000000, /* same as beq above with t = $zero */
                 kFmtBitBlt, 25, 21, kFmtBitBlt, 15, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP | IS_BRANCH | REG_USE0,
                 "beqz", "!0r,!1t", 2),
    ENCODING_MAP(kMipsBgez, 0x04010000,
                 kFmtBitBlt, 25, 21, kFmtBitBlt, 15, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP | IS_BRANCH | REG_USE0,
                 "bgez", "!0r,!1t", 2),
    ENCODING_MAP(kMipsBgtz, 0x1C000000,
                 kFmtBitBlt, 25, 21, kFmtBitBlt, 15, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP | IS_BRANCH | REG_USE0,
                 "bgtz", "!0r,!1t", 2),
    ENCODING_MAP(kMipsBlez, 0x18000000,
                 kFmtBitBlt, 25, 21, kFmtBitBlt, 15, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP | IS_BRANCH | REG_USE0,
                 "blez", "!0r,!1t", 2),
    ENCODING_MAP(kMipsBltz, 0x04000000,
                 kFmtBitBlt, 25, 21, kFmtBitBlt, 15, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP | IS_BRANCH | REG_USE0,
                 "bltz", "!0r,!1t", 2),
    ENCODING_MAP(kMipsBnez, 0x14000000, /* same as bne below with t = $zero */
                 kFmtBitBlt, 25, 21, kFmtBitBlt, 15, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP | IS_BRANCH | REG_USE0,
                 "bnez", "!0r,!1t", 2),
    ENCODING_MAP(kMipsBne, 0x14000000,
                 kFmtBitBlt, 25, 21, kFmtBitBlt, 20, 16, kFmtBitBlt, 15, 0,
                 kFmtUnused, -1, -1, IS_BINARY_OP | IS_BRANCH | REG_USE01,
                 "bne", "!0r,!1r,!2t", 2),
    ENCODING_MAP(kMipsDiv, 0x0000001a,
                 kFmtUnused, -1, -1, kFmtUnused, -1, -1, kFmtBitBlt, 25, 21,
                 kFmtBitBlt, 20, 16, IS_QUAD_OP | REG_DEF01 | REG_USE23,
                 "div", "!2r,!3r", 2),
#if __mips_isa_rev>=2
    ENCODING_MAP(kMipsExt, 0x7c000000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 25, 21, kFmtBitBlt, 10, 6,
                 kFmtBitBlt, 15, 11, IS_QUAD_OP | REG_DEF0 | REG_USE1,
                 "ext", "!0r,!1r,!2d,!3D", 2),
#endif
    ENCODING_MAP(kMipsJal, 0x0c000000,
                 kFmtBitBlt, 25, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP | IS_BRANCH | REG_DEF_LR,
                 "jal", "!0T(!0E)", 2),
    ENCODING_MAP(kMipsJalr, 0x00000009,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 25, 21, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | IS_BRANCH | REG_DEF0_USE1,
                 "jalr", "!0r,!1r", 2),
    ENCODING_MAP(kMipsJr, 0x00000008,
                 kFmtBitBlt, 25, 21, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP | IS_BRANCH | REG_USE0,
                 "jr", "!0r", 2),
    ENCODING_MAP(kMipsLahi, 0x3C000000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 15, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0,
                 "lahi/lui", "!0r,0x!1h(!1d)", 2),
    ENCODING_MAP(kMipsLalo, 0x34000000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 25, 21, kFmtBitBlt, 15, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "lalo/ori", "!0r,!1r,0x!2h(!2d)", 2),
    ENCODING_MAP(kMipsLui, 0x3C000000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 15, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0,
                 "lui", "!0r,0x!1h(!1d)", 2),
    ENCODING_MAP(kMipsLb, 0x80000000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 15, 0, kFmtBitBlt, 25, 21,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE2 | IS_LOAD,
                 "lb", "!0r,!1d(!2r)", 2),
    ENCODING_MAP(kMipsLbu, 0x90000000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 15, 0, kFmtBitBlt, 25, 21,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE2 | IS_LOAD,
                 "lbu", "!0r,!1d(!2r)", 2),
    ENCODING_MAP(kMipsLh, 0x84000000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 15, 0, kFmtBitBlt, 25, 21,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE2 | IS_LOAD,
                 "lh", "!0r,!1d(!2r)", 2),
    ENCODING_MAP(kMipsLhu, 0x94000000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 15, 0, kFmtBitBlt, 25, 21,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE2 | IS_LOAD,
                 "lhu", "!0r,!1d(!2r)", 2),
    ENCODING_MAP(kMipsLw, 0x8C000000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 15, 0, kFmtBitBlt, 25, 21,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE2 | IS_LOAD,
                 "lw", "!0r,!1d(!2r)", 2),
    ENCODING_MAP(kMipsMfhi, 0x00000010,
                 kFmtBitBlt, 15, 11, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "mfhi", "!0r", 2),
    ENCODING_MAP(kMipsMflo, 0x00000012,
                 kFmtBitBlt, 15, 11, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "mflo", "!0r", 2),
    ENCODING_MAP(kMipsMove, 0x00000025, /* or using zero reg */
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 25, 21, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "move", "!0r,!1r", 2),
    ENCODING_MAP(kMipsMovz, 0x0000000a,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 25, 21, kFmtBitBlt, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "movz", "!0r,!1r,!2r", 2),
    ENCODING_MAP(kMipsMul, 0x70000002,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 25, 21, kFmtBitBlt, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "mul", "!0r,!1r,!2r", 2),
    ENCODING_MAP(kMipsNop, 0x00000000,
                 kFmtUnused, -1, -1, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, NO_OPERAND,
                 "nop", "", 2),
    ENCODING_MAP(kMipsNor, 0x00000027, /* used for "not" too */
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 25, 21, kFmtBitBlt, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "nor", "!0r,!1r,!2r", 2),
    ENCODING_MAP(kMipsOr, 0x00000025,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 25, 21, kFmtBitBlt, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "or", "!0r,!1r,!2r", 2),
    ENCODING_MAP(kMipsOri, 0x34000000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 25, 21, kFmtBitBlt, 15, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "ori", "!0r,!1r,0x!2h(!2d)", 2),
    ENCODING_MAP(kMipsPref, 0xCC000000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 15, 0, kFmtBitBlt, 25, 21,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE2,
                 "pref", "!0d,!1d(!2r)", 2),
    ENCODING_MAP(kMipsSb, 0xA0000000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 15, 0, kFmtBitBlt, 25, 21,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE02 | IS_STORE,
                 "sb", "!0r,!1d(!2r)", 2),
#if __mips_isa_rev>=2
    ENCODING_MAP(kMipsSeb, 0x7c000420,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 20, 16, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "seb", "!0r,!1r", 2),
    ENCODING_MAP(kMipsSeh, 0x7c000620,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 20, 16, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "seh", "!0r,!1r", 2),
#endif
    ENCODING_MAP(kMipsSh, 0xA4000000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 15, 0, kFmtBitBlt, 25, 21,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE02 | IS_STORE,
                 "sh", "!0r,!1d(!2r)", 2),
    ENCODING_MAP(kMipsSll, 0x00000000,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 20, 16, kFmtBitBlt, 10, 6,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "sll", "!0r,!1r,0x!2h(!2d)", 2),
    ENCODING_MAP(kMipsSllv, 0x00000004,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 20, 16, kFmtBitBlt, 25, 21,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "sllv", "!0r,!1r,!2r", 2),
    ENCODING_MAP(kMipsSlt, 0x0000002a,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 25, 21, kFmtBitBlt, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "slt", "!0r,!1r,!2r", 2),
    ENCODING_MAP(kMipsSlti, 0x28000000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 25, 21, kFmtBitBlt, 15, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "slti", "!0r,!1r,0x!2h(!2d)", 2),
    ENCODING_MAP(kMipsSltu, 0x0000002b,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 25, 21, kFmtBitBlt, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "sltu", "!0r,!1r,!2r", 2),
    ENCODING_MAP(kMipsSra, 0x00000003,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 20, 16, kFmtBitBlt, 10, 6,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "sra", "!0r,!1r,0x!2h(!2d)", 2),
    ENCODING_MAP(kMipsSrav, 0x00000007,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 20, 16, kFmtBitBlt, 25, 21,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "srav", "!0r,!1r,!2r", 2),
    ENCODING_MAP(kMipsSrl, 0x00000002,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 20, 16, kFmtBitBlt, 10, 6,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "srl", "!0r,!1r,0x!2h(!2d)", 2),
    ENCODING_MAP(kMipsSrlv, 0x00000006,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 20, 16, kFmtBitBlt, 25, 21,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "srlv", "!0r,!1r,!2r", 2),
    ENCODING_MAP(kMipsSubu, 0x00000023, /* used for "neg" too */
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 25, 21, kFmtBitBlt, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "subu", "!0r,!1r,!2r", 2),
    ENCODING_MAP(kMipsSw, 0xAC000000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 15, 0, kFmtBitBlt, 25, 21,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE02 | IS_STORE,
                 "sw", "!0r,!1d(!2r)", 2),
    ENCODING_MAP(kMipsXor, 0x00000026,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 25, 21, kFmtBitBlt, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "xor", "!0r,!1r,!2r", 2),
    ENCODING_MAP(kMipsXori, 0x38000000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 25, 21, kFmtBitBlt, 15, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "xori", "!0r,!1r,0x!2h(!2d)", 2),
#ifdef __mips_hard_float
    ENCODING_MAP(kMipsFadds, 0x46000000,
                 kFmtSfp, 10, 6, kFmtSfp, 15, 11, kFmtSfp, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "add.s", "!0s,!1s,!2s", 2),
    ENCODING_MAP(kMipsFsubs, 0x46000001,
                 kFmtSfp, 10, 6, kFmtSfp, 15, 11, kFmtSfp, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "sub.s", "!0s,!1s,!2s", 2),
    ENCODING_MAP(kMipsFmuls, 0x46000002,
                 kFmtSfp, 10, 6, kFmtSfp, 15, 11, kFmtSfp, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "mul.s", "!0s,!1s,!2s", 2),
    ENCODING_MAP(kMipsFdivs, 0x46000003,
                 kFmtSfp, 10, 6, kFmtSfp, 15, 11, kFmtSfp, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "div.s", "!0s,!1s,!2s", 2),
    ENCODING_MAP(kMipsFaddd, 0x46200000,
                 kFmtDfp, 10, 6, kFmtDfp, 15, 11, kFmtDfp, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "add.d", "!0S,!1S,!2S", 2),
    ENCODING_MAP(kMipsFsubd, 0x46200001,
                 kFmtDfp, 10, 6, kFmtDfp, 15, 11, kFmtDfp, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "sub.d", "!0S,!1S,!2S", 2),
    ENCODING_MAP(kMipsFmuld, 0x46200002,
                 kFmtDfp, 10, 6, kFmtDfp, 15, 11, kFmtDfp, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "mul.d", "!0S,!1S,!2S", 2),
    ENCODING_MAP(kMipsFdivd, 0x46200003,
                 kFmtDfp, 10, 6, kFmtDfp, 15, 11, kFmtDfp, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "div.d", "!0S,!1S,!2S", 2),
    ENCODING_MAP(kMipsFcvtsd, 0x46200020,
                 kFmtSfp, 10, 6, kFmtDfp, 15, 11, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "cvt.s.d", "!0s,!1S", 2),
    ENCODING_MAP(kMipsFcvtsw, 0x46800020,
                 kFmtSfp, 10, 6, kFmtSfp, 15, 11, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "cvt.s.w", "!0s,!1s", 2),
    ENCODING_MAP(kMipsFcvtds, 0x46000021,
                 kFmtDfp, 10, 6, kFmtSfp, 15, 11, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "cvt.d.s", "!0S,!1s", 2),
    ENCODING_MAP(kMipsFcvtdw, 0x46800021,
                 kFmtDfp, 10, 6, kFmtSfp, 15, 11, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "cvt.d.w", "!0S,!1s", 2),
    ENCODING_MAP(kMipsFcvtws, 0x46000024,
                 kFmtSfp, 10, 6, kFmtSfp, 15, 11, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "cvt.w.s", "!0s,!1s", 2),
    ENCODING_MAP(kMipsFcvtwd, 0x46200024,
                 kFmtSfp, 10, 6, kFmtDfp, 15, 11, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "cvt.w.d", "!0s,!1S", 2),
    ENCODING_MAP(kMipsFmovs, 0x46000006,
                 kFmtSfp, 10, 6, kFmtSfp, 15, 11, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "mov.s", "!0s,!1s", 2),
    ENCODING_MAP(kMipsFmovd, 0x46200006,
                 kFmtDfp, 10, 6, kFmtDfp, 15, 11, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "mov.d", "!0S,!1S", 2),
    ENCODING_MAP(kMipsFlwc1, 0xC4000000,
                 kFmtSfp, 20, 16, kFmtBitBlt, 15, 0, kFmtBitBlt, 25, 21,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE2 | IS_LOAD,
                 "lwc1", "!0s,!1d(!2r)", 2),
    ENCODING_MAP(kMipsFldc1, 0xD4000000,
                 kFmtDfp, 20, 16, kFmtBitBlt, 15, 0, kFmtBitBlt, 25, 21,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE2 | IS_LOAD,
                 "ldc1", "!0S,!1d(!2r)", 2),
    ENCODING_MAP(kMipsFswc1, 0xE4000000,
                 kFmtSfp, 20, 16, kFmtBitBlt, 15, 0, kFmtBitBlt, 25, 21,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE02 | IS_STORE,
                 "swc1", "!0s,!1d(!2r)", 2),
    ENCODING_MAP(kMipsFsdc1, 0xF4000000,
                 kFmtDfp, 20, 16, kFmtBitBlt, 15, 0, kFmtBitBlt, 25, 21,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE02 | IS_STORE,
                 "sdc1", "!0S,!1d(!2r)", 2),
    ENCODING_MAP(kMipsMfc1, 0x44000000,
                 kFmtBitBlt, 20, 16, kFmtSfp, 15, 11, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "mfc1", "!0r,!1s", 2),
    ENCODING_MAP(kMipsMtc1, 0x44800000,
                 kFmtBitBlt, 20, 16, kFmtSfp, 15, 11, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_USE0 | REG_DEF1,
                 "mtc1", "!0r,!1s", 2),
#endif
    ENCODING_MAP(kMipsUndefined, 0x64000000,
                 kFmtUnused, -1, -1, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, NO_OPERAND,
                 "undefined", "", 2),
};

/*
 * Assemble the LIR into binary instruction format.  Note that we may
 * discover that pc-relative displacements may not fit the selected
 * instruction.  In those cases we will try to substitute a new code
 * sequence or request that the trace be shortened and retried.
 */
AssemblerStatus oatAssembleInstructions(CompilationUnit *cUnit,
                                        intptr_t startAddr)
{
    LIR *lir;
    AssemblerStatus res = kSuccess;  // Assume success

    for (lir = (LIR *) cUnit->firstLIRInsn; lir; lir = NEXT_LIR(lir)) {
        if (lir->opcode < 0) {
            continue;
        }


        if (lir->flags.isNop) {
            continue;
        }

        if (lir->opcode == kMipsB || lir->opcode == kMipsBal) {
            LIR *targetLIR = (LIR *) lir->target;
            intptr_t pc = lir->offset + 4;
            intptr_t target = targetLIR->offset;
            int delta = target - pc;
            if (delta & 0x3) {
                LOG(FATAL) << "PC-rel offset not multiple of 4: " << delta;
            }
            if (delta > 131068 || delta < -131069) {
                UNIMPLEMENTED(FATAL) << "B out of range, need long sequence: " << delta;
            }
            lir->operands[0] = delta >> 2;
        } else if (lir->opcode >= kMipsBeqz && lir->opcode <= kMipsBnez) {
            LIR *targetLIR = (LIR *) lir->target;
            intptr_t pc = lir->offset + 4;
            intptr_t target = targetLIR->offset;
            int delta = target - pc;
            if (delta & 0x3) {
                LOG(FATAL) << "PC-rel offset not multiple of 4: " << delta;
            }
            if (delta > 131068 || delta < -131069) {
                UNIMPLEMENTED(FATAL) << "B[eq|ne]z needs long sequence: " << delta;
            }
            lir->operands[1] = delta >> 2;
        } else if (lir->opcode == kMipsBeq || lir->opcode == kMipsBne) {
            LIR *targetLIR = (LIR *) lir->target;
            intptr_t pc = lir->offset + 4;
            intptr_t target = targetLIR->offset;
            int delta = target - pc;
            if (delta & 0x3) {
                LOG(FATAL) << "PC-rel offset not multiple of 4: " << delta;
            }
            if (delta > 131068 || delta < -131069) {
                UNIMPLEMENTED(FATAL) << "B[eq|ne] needs long sequence: " << delta;
            }
            lir->operands[2] = delta >> 2;
        } else if (lir->opcode == kMipsJal) {
            intptr_t curPC = (startAddr + lir->offset + 4) & ~3;
            intptr_t target = lir->operands[0];
            /* ensure PC-region branch can be used */
            DCHECK_EQ((curPC & 0xF0000000), (target & 0xF0000000));
            if (target & 0x3) {
                LOG(FATAL) << "Jump target not multiple of 4: " << target;
            }
            lir->operands[0] =  target >> 2;
        } else if (lir->opcode == kMipsLahi) { /* load address hi (via lui) */
            LIR *targetLIR = (LIR *) lir->target;
            intptr_t target = startAddr + targetLIR->offset;
            lir->operands[1] = target >> 16;
        } else if (lir->opcode == kMipsLalo) { /* load address lo (via ori) */
            LIR *targetLIR = (LIR *) lir->target;
            intptr_t target = startAddr + targetLIR->offset;
            lir->operands[2] = lir->operands[2] + target;
        }

        /*
         * If one of the pc-relative instructions expanded we'll have
         * to make another pass.  Don't bother to fully assemble the
         * instruction.
         */
        if (res != kSuccess) {
            continue;
        }
        const MipsEncodingMap *encoder = &EncodingMap[lir->opcode];
        u4 bits = encoder->skeleton;
        int i;
        for (i = 0; i < 4; i++) {
            u4 operand;
            u4 value;
            operand = lir->operands[i];
            switch(encoder->fieldLoc[i].kind) {
                case kFmtUnused:
                    break;
                case kFmtBitBlt:
                    if (encoder->fieldLoc[i].start == 0 && encoder->fieldLoc[i].end == 31) {
                        value = operand;
                    } else {
                        value = (operand << encoder->fieldLoc[i].start) &
                                ((1 << (encoder->fieldLoc[i].end + 1)) - 1);
                    }
                    bits |= value;
                    break;
                case kFmtDfp: {
                    DCHECK(DOUBLEREG(operand));
                    DCHECK((operand & 0x1) == 0);
                    value = ((operand & FP_REG_MASK) << encoder->fieldLoc[i].start) &
                            ((1 << (encoder->fieldLoc[i].end + 1)) - 1);
                    bits |= value;
                    break;
                }
                case kFmtSfp:
                    DCHECK(SINGLEREG(operand));
                    value = ((operand & FP_REG_MASK) << encoder->fieldLoc[i].start) &
                            ((1 << (encoder->fieldLoc[i].end + 1)) - 1);
                    bits |= value;
                    break;
                default:
                    LOG(FATAL) << "Bad encoder format: "
                               << (int)encoder->fieldLoc[i].kind;
            }
        }
        DCHECK_EQ(encoder->size, 2);
        // FIXME: need multi-endian handling here
        cUnit->codeBuffer.push_back((bits >> 16) & 0xffff);
        cUnit->codeBuffer.push_back(bits & 0xffff);
    }
    return res;
}

/*
 * Target-dependent offset assignment.
 * TODO: normalize usage of flags.size and make this target
 * independent.
 */
int oatAssignInsnOffsets(CompilationUnit* cUnit)
{
    LIR* mipsLIR;
    int offset = 0;

    for (mipsLIR = (LIR *) cUnit->firstLIRInsn;
        mipsLIR;
        mipsLIR = NEXT_LIR(mipsLIR)) {
        mipsLIR->offset = offset;
        if (mipsLIR->opcode >= 0) {
            if (!mipsLIR->flags.isNop) {
                mipsLIR->flags.size = EncodingMap[mipsLIR->opcode].size * 2;
                offset += mipsLIR->flags.size;
            }
        } else if (mipsLIR->opcode == kPseudoPseudoAlign4) {
            if (offset & 0x2) {
                offset += 2;
                mipsLIR->operands[0] = 1;
            } else {
                mipsLIR->operands[0] = 0;
            }
        }
        /* Pseudo opcodes don't consume space */
    }

    return offset;
}

}  // namespace art
