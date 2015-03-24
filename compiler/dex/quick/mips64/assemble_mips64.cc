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

#include "codegen_mips64.h"

#include "base/logging.h"
#include "dex/compiler_ir.h"
#include "dex/quick/mir_to_lir-inl.h"
#include "mips64_lir.h"

namespace art {

#define MAX_ASSEMBLER_RETRIES 50

/*
 * opcode: Mips64OpCode enum
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
 *     N -> append a NOP
 *
 *  [!] escape.  To insert "!", use "!!"
 */
/* NOTE: must be kept in sync with enum Mips64Opcode from mips64_lir.h */
/*
 * TUNING: We're currently punting on the branch delay slots.  All branch
 * instructions in this map are given a size of 8, which during assembly
 * is expanded to include a nop.  This scheme should be replaced with
 * an assembler pass to fill those slots when possible.
 */
const Mips64EncodingMap Mips64Mir2Lir::EncodingMap[kMips64Last] = {
    ENCODING_MAP(kMips6432BitData, 0x00000000,
                 kFmtBitBlt, 31, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP,
                 "data", "0x!0h(!0d)", 4),
    ENCODING_MAP(kMips64Addiu, 0x24000000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 25, 21, kFmtBitBlt, 15, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "addiu", "!0r,!1r,0x!2h(!2d)", 4),
    ENCODING_MAP(kMips64Addu, 0x00000021,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 25, 21, kFmtBitBlt, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "addu", "!0r,!1r,!2r", 4),
    ENCODING_MAP(kMips64And, 0x00000024,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 25, 21, kFmtBitBlt, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "and", "!0r,!1r,!2r", 4),
    ENCODING_MAP(kMips64Andi, 0x30000000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 25, 21, kFmtBitBlt, 15, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "andi", "!0r,!1r,0x!2h(!2d)", 4),
    ENCODING_MAP(kMips64B, 0x10000000,
                 kFmtBitBlt, 15, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP | IS_BRANCH | NEEDS_FIXUP,
                 "b", "!0t!0N", 8),
    ENCODING_MAP(kMips64Bal, 0x04110000,
                 kFmtBitBlt, 15, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP | IS_BRANCH | REG_DEF_LR |
                 NEEDS_FIXUP, "bal", "!0t!0N", 8),
    ENCODING_MAP(kMips64Beq, 0x10000000,
                 kFmtBitBlt, 25, 21, kFmtBitBlt, 20, 16, kFmtBitBlt, 15, 0,
                 kFmtUnused, -1, -1, IS_BINARY_OP | IS_BRANCH | REG_USE01 |
                 NEEDS_FIXUP, "beq", "!0r,!1r,!2t!0N", 8),
    ENCODING_MAP(kMips64Beqz, 0x10000000,  // Same as beq above with t = $zero.
                 kFmtBitBlt, 25, 21, kFmtBitBlt, 15, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP | IS_BRANCH | REG_USE0 |
                 NEEDS_FIXUP, "beqz", "!0r,!1t!0N", 8),
    ENCODING_MAP(kMips64Bgez, 0x04010000,
                 kFmtBitBlt, 25, 21, kFmtBitBlt, 15, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP | IS_BRANCH | REG_USE0 |
                 NEEDS_FIXUP, "bgez", "!0r,!1t!0N", 8),
    ENCODING_MAP(kMips64Bgtz, 0x1c000000,
                 kFmtBitBlt, 25, 21, kFmtBitBlt, 15, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP | IS_BRANCH | REG_USE0 |
                 NEEDS_FIXUP, "bgtz", "!0r,!1t!0N", 8),
    ENCODING_MAP(kMips64Blez, 0x18000000,
                 kFmtBitBlt, 25, 21, kFmtBitBlt, 15, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP | IS_BRANCH | REG_USE0 |
                 NEEDS_FIXUP, "blez", "!0r,!1t!0N", 8),
    ENCODING_MAP(kMips64Bltz, 0x04000000,
                 kFmtBitBlt, 25, 21, kFmtBitBlt, 15, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP | IS_BRANCH | REG_USE0 |
                 NEEDS_FIXUP, "bltz", "!0r,!1t!0N", 8),
    ENCODING_MAP(kMips64Bnez, 0x14000000,  // Same as bne below with t = $zero.
                 kFmtBitBlt, 25, 21, kFmtBitBlt, 15, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP | IS_BRANCH | REG_USE0 |
                 NEEDS_FIXUP, "bnez", "!0r,!1t!0N", 8),
    ENCODING_MAP(kMips64Bne, 0x14000000,
                 kFmtBitBlt, 25, 21, kFmtBitBlt, 20, 16, kFmtBitBlt, 15, 0,
                 kFmtUnused, -1, -1, IS_BINARY_OP | IS_BRANCH | REG_USE01 |
                 NEEDS_FIXUP, "bne", "!0r,!1r,!2t!0N", 8),
    ENCODING_MAP(kMips64Break, 0x0000000d,
                 kFmtBitBlt, 25, 6, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP, "break", "!0d", 4),
    ENCODING_MAP(kMips64Daddiu, 0x64000000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 25, 21, kFmtBitBlt, 15, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "daddiu", "!0r,!1r,0x!2h(!2d)", 4),
    ENCODING_MAP(kMips64Daddu, 0x0000002d,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 25, 21, kFmtBitBlt, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "daddu", "!0r,!1r,!2r", 4),
    ENCODING_MAP(kMips64Dahi, 0x04060000,
                 kFmtBitBlt, 25, 21, kFmtBitBlt, 15, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE0,
                 "dahi", "!0r,0x!1h(!1d)", 4),
    ENCODING_MAP(kMips64Dati, 0x041E0000,
                 kFmtBitBlt, 25, 21, kFmtBitBlt, 15, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE0,
                 "dati", "!0r,0x!1h(!1d)", 4),
    ENCODING_MAP(kMips64Daui, 0x74000000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 25, 21, kFmtBitBlt, 15, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "daui", "!0r,!1r,0x!2h(!2d)", 4),
    ENCODING_MAP(kMips64Ddiv, 0x0000009e,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 25, 21, kFmtBitBlt, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "ddiv", "!0r,!1r,!2r", 4),
    ENCODING_MAP(kMips64Div, 0x0000009a,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 25, 21, kFmtBitBlt, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "div", "!0r,!1r,!2r", 4),
    ENCODING_MAP(kMips64Dmod, 0x000000de,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 25, 21, kFmtBitBlt, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "dmod", "!0r,!1r,!2r", 4),
    ENCODING_MAP(kMips64Dmul, 0x0000009c,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 25, 21, kFmtBitBlt, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "dmul", "!0r,!1r,!2r", 4),
    ENCODING_MAP(kMips64Dmfc1, 0x44200000,
                 kFmtBitBlt, 20, 16, kFmtDfp, 15, 11, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "dmfc1", "!0r,!1s", 4),
    ENCODING_MAP(kMips64Dmtc1, 0x44a00000,
                 kFmtBitBlt, 20, 16, kFmtDfp, 15, 11, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_USE0 | REG_DEF1,
                 "dmtc1", "!0r,!1s", 4),
    ENCODING_MAP(kMips64Drotr32, 0x0000003e | (1 << 21),
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 20, 16, kFmtBitBlt, 10, 6,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "drotr32", "!0r,!1r,0x!2h(!2d)", 4),
    ENCODING_MAP(kMips64Dsll, 0x00000038,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 20, 16, kFmtBitBlt, 10, 6,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "dsll", "!0r,!1r,0x!2h(!2d)", 4),
    ENCODING_MAP(kMips64Dsll32, 0x0000003c,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 20, 16, kFmtBitBlt, 10, 6,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "dsll32", "!0r,!1r,0x!2h(!2d)", 4),
    ENCODING_MAP(kMips64Dsrl, 0x0000003a,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 20, 16, kFmtBitBlt, 10, 6,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "dsrl", "!0r,!1r,0x!2h(!2d)", 4),
    ENCODING_MAP(kMips64Dsrl32, 0x0000003e,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 20, 16, kFmtBitBlt, 10, 6,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "dsrl32", "!0r,!1r,0x!2h(!2d)", 4),
    ENCODING_MAP(kMips64Dsra, 0x0000003b,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 20, 16, kFmtBitBlt, 10, 6,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "dsra", "!0r,!1r,0x!2h(!2d)", 4),
    ENCODING_MAP(kMips64Dsra32, 0x0000003f,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 20, 16, kFmtBitBlt, 10, 6,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "dsra32", "!0r,!1r,0x!2h(!2d)", 4),
    ENCODING_MAP(kMips64Dsllv, 0x00000014,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 20, 16, kFmtBitBlt, 25, 21,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "dsllv", "!0r,!1r,!2r", 4),
    ENCODING_MAP(kMips64Dsrlv, 0x00000016,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 20, 16, kFmtBitBlt, 25, 21,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "dsrlv", "!0r,!1r,!2r", 4),
    ENCODING_MAP(kMips64Dsrav, 0x00000017,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 20, 16, kFmtBitBlt, 25, 21,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "dsrav", "!0r,!1r,!2r", 4),
    ENCODING_MAP(kMips64Dsubu, 0x0000002f,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 25, 21, kFmtBitBlt, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "dsubu", "!0r,!1r,!2r", 4),
    ENCODING_MAP(kMips64Ext, 0x7c000000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 25, 21, kFmtBitBlt, 10, 6,
                 kFmtBitBlt, 15, 11, IS_QUAD_OP | REG_DEF0 | REG_USE1,
                 "ext", "!0r,!1r,!2d,!3D", 4),
    ENCODING_MAP(kMips64Faddd, 0x46200000,
                 kFmtDfp, 10, 6, kFmtDfp, 15, 11, kFmtDfp, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "add.d", "!0S,!1S,!2S", 4),
    ENCODING_MAP(kMips64Fadds, 0x46000000,
                 kFmtSfp, 10, 6, kFmtSfp, 15, 11, kFmtSfp, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "add.s", "!0s,!1s,!2s", 4),
    ENCODING_MAP(kMips64Fdivd, 0x46200003,
                 kFmtDfp, 10, 6, kFmtDfp, 15, 11, kFmtDfp, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "div.d", "!0S,!1S,!2S", 4),
    ENCODING_MAP(kMips64Fdivs, 0x46000003,
                 kFmtSfp, 10, 6, kFmtSfp, 15, 11, kFmtSfp, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "div.s", "!0s,!1s,!2s", 4),
    ENCODING_MAP(kMips64Fmuld, 0x46200002,
                 kFmtDfp, 10, 6, kFmtDfp, 15, 11, kFmtDfp, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "mul.d", "!0S,!1S,!2S", 4),
    ENCODING_MAP(kMips64Fmuls, 0x46000002,
                 kFmtSfp, 10, 6, kFmtSfp, 15, 11, kFmtSfp, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "mul.s", "!0s,!1s,!2s", 4),
    ENCODING_MAP(kMips64Fsubd, 0x46200001,
                 kFmtDfp, 10, 6, kFmtDfp, 15, 11, kFmtDfp, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "sub.d", "!0S,!1S,!2S", 4),
    ENCODING_MAP(kMips64Fsubs, 0x46000001,
                 kFmtSfp, 10, 6, kFmtSfp, 15, 11, kFmtSfp, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "sub.s", "!0s,!1s,!2s", 4),
    ENCODING_MAP(kMips64Fcvtsd, 0x46200020,
                 kFmtSfp, 10, 6, kFmtDfp, 15, 11, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "cvt.s.d", "!0s,!1S", 4),
    ENCODING_MAP(kMips64Fcvtsw, 0x46800020,
                 kFmtSfp, 10, 6, kFmtSfp, 15, 11, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "cvt.s.w", "!0s,!1s", 4),
    ENCODING_MAP(kMips64Fcvtds, 0x46000021,
                 kFmtDfp, 10, 6, kFmtSfp, 15, 11, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "cvt.d.s", "!0S,!1s", 4),
    ENCODING_MAP(kMips64Fcvtdw, 0x46800021,
                 kFmtDfp, 10, 6, kFmtSfp, 15, 11, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "cvt.d.w", "!0S,!1s", 4),
    ENCODING_MAP(kMips64Fcvtws, 0x46000024,
                 kFmtSfp, 10, 6, kFmtSfp, 15, 11, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "cvt.w.s", "!0s,!1s", 4),
    ENCODING_MAP(kMips64Fcvtwd, 0x46200024,
                 kFmtSfp, 10, 6, kFmtDfp, 15, 11, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "cvt.w.d", "!0s,!1S", 4),
    ENCODING_MAP(kMips64Fmovd, 0x46200006,
                 kFmtDfp, 10, 6, kFmtDfp, 15, 11, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "mov.d", "!0S,!1S", 4),
    ENCODING_MAP(kMips64Fmovs, 0x46000006,
                 kFmtSfp, 10, 6, kFmtSfp, 15, 11, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "mov.s", "!0s,!1s", 4),
    ENCODING_MAP(kMips64Fnegd, 0x46200007,
                 kFmtDfp, 10, 6, kFmtDfp, 15, 11, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "neg.d", "!0S,!1S", 4),
    ENCODING_MAP(kMips64Fnegs, 0x46000007,
                 kFmtSfp, 10, 6, kFmtSfp, 15, 11, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "neg.s", "!0s,!1s", 4),
    ENCODING_MAP(kMips64Fldc1, 0xd4000000,
                 kFmtDfp, 20, 16, kFmtBitBlt, 15, 0, kFmtBitBlt, 25, 21,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE2 | IS_LOAD,
                 "ldc1", "!0S,!1d(!2r)", 4),
    ENCODING_MAP(kMips64Flwc1, 0xc4000000,
                 kFmtSfp, 20, 16, kFmtBitBlt, 15, 0, kFmtBitBlt, 25, 21,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE2 | IS_LOAD,
                 "lwc1", "!0s,!1d(!2r)", 4),
    ENCODING_MAP(kMips64Fsdc1, 0xf4000000,
                 kFmtDfp, 20, 16, kFmtBitBlt, 15, 0, kFmtBitBlt, 25, 21,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE02 | IS_STORE,
                 "sdc1", "!0S,!1d(!2r)", 4),
    ENCODING_MAP(kMips64Fswc1, 0xe4000000,
                 kFmtSfp, 20, 16, kFmtBitBlt, 15, 0, kFmtBitBlt, 25, 21,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE02 | IS_STORE,
                 "swc1", "!0s,!1d(!2r)", 4),
    ENCODING_MAP(kMips64Jal, 0x0c000000,
                 kFmtBitBlt, 25, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP | IS_BRANCH | REG_DEF_LR,
                 "jal", "!0T(!0E)!0N", 8),
    ENCODING_MAP(kMips64Jalr, 0x00000009,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 25, 21, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | IS_BRANCH | REG_DEF0_USE1,
                 "jalr", "!0r,!1r!0N", 8),
    ENCODING_MAP(kMips64Lahi, 0x3c000000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 15, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0,
                 "lahi/lui", "!0r,0x!1h(!1d)", 4),
    ENCODING_MAP(kMips64Lalo, 0x34000000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 25, 21, kFmtBitBlt, 15, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "lalo/ori", "!0r,!1r,0x!2h(!2d)", 4),
    ENCODING_MAP(kMips64Lb, 0x80000000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 15, 0, kFmtBitBlt, 25, 21,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE2 | IS_LOAD,
                 "lb", "!0r,!1d(!2r)", 4),
    ENCODING_MAP(kMips64Lbu, 0x90000000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 15, 0, kFmtBitBlt, 25, 21,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE2 | IS_LOAD,
                 "lbu", "!0r,!1d(!2r)", 4),
    ENCODING_MAP(kMips64Ld, 0xdc000000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 15, 0, kFmtBitBlt, 25, 21,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE2 | IS_LOAD,
                 "ld", "!0r,!1d(!2r)", 4),
    ENCODING_MAP(kMips64Lh, 0x84000000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 15, 0, kFmtBitBlt, 25, 21,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE2 | IS_LOAD,
                 "lh", "!0r,!1d(!2r)", 4),
    ENCODING_MAP(kMips64Lhu, 0x94000000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 15, 0, kFmtBitBlt, 25, 21,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE2 | IS_LOAD,
                 "lhu", "!0r,!1d(!2r)", 4),
    ENCODING_MAP(kMips64Lui, 0x3c000000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 15, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0,
                 "lui", "!0r,0x!1h(!1d)", 4),
    ENCODING_MAP(kMips64Lw, 0x8c000000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 15, 0, kFmtBitBlt, 25, 21,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE2 | IS_LOAD,
                 "lw", "!0r,!1d(!2r)", 4),
    ENCODING_MAP(kMips64Lwu, 0x9c000000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 15, 0, kFmtBitBlt, 25, 21,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE2 | IS_LOAD,
                 "lwu", "!0r,!1d(!2r)", 4),
    ENCODING_MAP(kMips64Mfc1, 0x44000000,
                 kFmtBitBlt, 20, 16, kFmtSfp, 15, 11, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "mfc1", "!0r,!1s", 4),
    ENCODING_MAP(kMips64Mtc1, 0x44800000,
                 kFmtBitBlt, 20, 16, kFmtSfp, 15, 11, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_USE0 | REG_DEF1,
                 "mtc1", "!0r,!1s", 4),
    ENCODING_MAP(kMips64Move, 0x0000002d,  // Or using zero reg.
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 25, 21, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "move", "!0r,!1r", 4),
    ENCODING_MAP(kMips64Mod, 0x000000da,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 25, 21, kFmtBitBlt, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "mod", "!0r,!1r,!2r", 4),
    ENCODING_MAP(kMips64Mul, 0x00000098,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 25, 21, kFmtBitBlt, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "mul", "!0r,!1r,!2r", 4),
    ENCODING_MAP(kMips64Nop, 0x00000000,
                 kFmtUnused, -1, -1, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, NO_OPERAND,
                 "nop", ";", 4),
    ENCODING_MAP(kMips64Nor, 0x00000027,  // Used for "not" too.
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 25, 21, kFmtBitBlt, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "nor", "!0r,!1r,!2r", 4),
    ENCODING_MAP(kMips64Or, 0x00000025,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 25, 21, kFmtBitBlt, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "or", "!0r,!1r,!2r", 4),
    ENCODING_MAP(kMips64Ori, 0x34000000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 25, 21, kFmtBitBlt, 15, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "ori", "!0r,!1r,0x!2h(!2d)", 4),
    ENCODING_MAP(kMips64Sb, 0xa0000000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 15, 0, kFmtBitBlt, 25, 21,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE02 | IS_STORE,
                 "sb", "!0r,!1d(!2r)", 4),
    ENCODING_MAP(kMips64Sd, 0xfc000000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 15, 0, kFmtBitBlt, 25, 21,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE02 | IS_STORE,
                 "sd", "!0r,!1d(!2r)", 4),
    ENCODING_MAP(kMips64Seb, 0x7c000420,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 20, 16, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "seb", "!0r,!1r", 4),
    ENCODING_MAP(kMips64Seh, 0x7c000620,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 20, 16, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "seh", "!0r,!1r", 4),
    ENCODING_MAP(kMips64Sh, 0xa4000000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 15, 0, kFmtBitBlt, 25, 21,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE02 | IS_STORE,
                 "sh", "!0r,!1d(!2r)", 4),
    ENCODING_MAP(kMips64Sll, 0x00000000,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 20, 16, kFmtBitBlt, 10, 6,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "sll", "!0r,!1r,0x!2h(!2d)", 4),
    ENCODING_MAP(kMips64Sllv, 0x00000004,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 20, 16, kFmtBitBlt, 25, 21,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "sllv", "!0r,!1r,!2r", 4),
    ENCODING_MAP(kMips64Slt, 0x0000002a,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 25, 21, kFmtBitBlt, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "slt", "!0r,!1r,!2r", 4),
    ENCODING_MAP(kMips64Slti, 0x28000000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 25, 21, kFmtBitBlt, 15, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "slti", "!0r,!1r,0x!2h(!2d)", 4),
    ENCODING_MAP(kMips64Sltu, 0x0000002b,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 25, 21, kFmtBitBlt, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "sltu", "!0r,!1r,!2r", 4),
    ENCODING_MAP(kMips64Sra, 0x00000003,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 20, 16, kFmtBitBlt, 10, 6,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "sra", "!0r,!1r,0x!2h(!2d)", 4),
    ENCODING_MAP(kMips64Srav, 0x00000007,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 20, 16, kFmtBitBlt, 25, 21,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "srav", "!0r,!1r,!2r", 4),
    ENCODING_MAP(kMips64Srl, 0x00000002,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 20, 16, kFmtBitBlt, 10, 6,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "srl", "!0r,!1r,0x!2h(!2d)", 4),
    ENCODING_MAP(kMips64Srlv, 0x00000006,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 20, 16, kFmtBitBlt, 25, 21,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "srlv", "!0r,!1r,!2r", 4),
    ENCODING_MAP(kMips64Subu, 0x00000023,  // Used for "neg" too.
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 25, 21, kFmtBitBlt, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "subu", "!0r,!1r,!2r", 4),
    ENCODING_MAP(kMips64Sw, 0xac000000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 15, 0, kFmtBitBlt, 25, 21,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE02 | IS_STORE,
                 "sw", "!0r,!1d(!2r)", 4),
    ENCODING_MAP(kMips64Sync, 0x0000000f,
                 kFmtBitBlt, 10, 6, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP,
                 "sync", ";", 4),
    ENCODING_MAP(kMips64Xor, 0x00000026,
                 kFmtBitBlt, 15, 11, kFmtBitBlt, 25, 21, kFmtBitBlt, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "xor", "!0r,!1r,!2r", 4),
    ENCODING_MAP(kMips64Xori, 0x38000000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 25, 21, kFmtBitBlt, 15, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "xori", "!0r,!1r,0x!2h(!2d)", 4),
    ENCODING_MAP(kMips64CurrPC, 0x04110001,
                 kFmtUnused, -1, -1, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, NO_OPERAND | IS_BRANCH | REG_DEF_LR,
                 "addiu", "ra,pc,8", 4),
    ENCODING_MAP(kMips64Delta, 0x67e00000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 15, 0, kFmtUnused, 15, 0,
                 kFmtUnused, -1, -1, IS_QUAD_OP | REG_DEF0 | REG_USE_LR |
                 NEEDS_FIXUP, "daddiu", "!0r,ra,0x!1h(!1d)", 4),
    ENCODING_MAP(kMips64DeltaHi, 0x3c000000,
                 kFmtBitBlt, 20, 16, kFmtBitBlt, 15, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_QUAD_OP | REG_DEF0 | NEEDS_FIXUP,
                 "lui", "!0r,0x!1h(!1d)", 4),
    ENCODING_MAP(kMips64DeltaLo, 0x34000000,
                 kFmtBlt5_2, 16, 21, kFmtBitBlt, 15, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_QUAD_OP | REG_DEF0_USE0 | NEEDS_FIXUP,
                 "ori", "!0r,!0r,0x!1h(!1d)", 4),
    ENCODING_MAP(kMips64Undefined, 0x64000000,
                 kFmtUnused, -1, -1, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, NO_OPERAND,
                 "undefined", "", 4),
};


/*
 * Convert a short-form branch to long form.  Hopefully, this won't happen
 * very often because the PIC sequence is especially unfortunate.
 *
 * Orig conditional branch
 * -----------------------
 *      beq  rs,rt,target
 *
 * Long conditional branch
 * -----------------------
 *      bne  rs,rt,hop
 *      bal  .+8   ; rRA <- anchor
 *      lui  rAT, ((target-anchor) >> 16)
 * anchor:
 *      ori  rAT, rAT, ((target-anchor) & 0xffff)
 *      addu rAT, rAT, rRA
 *      jalr rZERO, rAT
 * hop:
 *
 * Orig unconditional branch
 * -------------------------
 *      b target
 *
 * Long unconditional branch
 * -----------------------
 *      bal  .+8   ; rRA <- anchor
 *      lui  rAT, ((target-anchor) >> 16)
 * anchor:
 *      ori  rAT, rAT, ((target-anchor) & 0xffff)
 *      addu rAT, rAT, rRA
 *      jalr rZERO, rAT
 *
 *
 * NOTE: An out-of-range bal isn't supported because it should
 * never happen with the current PIC model.
 */
void Mips64Mir2Lir::ConvertShortToLongBranch(LIR* lir) {
  // For conditional branches we'll need to reverse the sense
  bool unconditional = false;
  int opcode = lir->opcode;
  int dalvik_offset = lir->dalvik_offset;
  switch (opcode) {
    case kMips64Bal:
      LOG(FATAL) << "long branch and link unsupported";
      UNREACHABLE();
    case kMips64B:
      unconditional = true;
      break;
    case kMips64Beq:  opcode = kMips64Bne; break;
    case kMips64Bne:  opcode = kMips64Beq; break;
    case kMips64Beqz: opcode = kMips64Bnez; break;
    case kMips64Bgez: opcode = kMips64Bltz; break;
    case kMips64Bgtz: opcode = kMips64Blez; break;
    case kMips64Blez: opcode = kMips64Bgtz; break;
    case kMips64Bltz: opcode = kMips64Bgez; break;
    case kMips64Bnez: opcode = kMips64Beqz; break;
    default:
      LOG(FATAL) << "Unexpected branch kind " << opcode;
      UNREACHABLE();
  }
  LIR* hop_target = NULL;
  if (!unconditional) {
    hop_target = RawLIR(dalvik_offset, kPseudoTargetLabel);
    LIR* hop_branch = RawLIR(dalvik_offset, opcode, lir->operands[0],
                             lir->operands[1], 0, 0, 0, hop_target);
    InsertLIRBefore(lir, hop_branch);
  }
  LIR* curr_pc = RawLIR(dalvik_offset, kMips64CurrPC);
  InsertLIRBefore(lir, curr_pc);
  LIR* anchor = RawLIR(dalvik_offset, kPseudoTargetLabel);
  LIR* delta_hi = RawLIR(dalvik_offset, kMips64DeltaHi, rAT, 0, WrapPointer(anchor), 0, 0,
                         lir->target);
  InsertLIRBefore(lir, delta_hi);
  InsertLIRBefore(lir, anchor);
  LIR* delta_lo = RawLIR(dalvik_offset, kMips64DeltaLo, rAT, 0, WrapPointer(anchor), 0, 0,
                         lir->target);
  InsertLIRBefore(lir, delta_lo);
  LIR* addu = RawLIR(dalvik_offset, kMips64Addu, rAT, rAT, rRA);
  InsertLIRBefore(lir, addu);
  LIR* jalr = RawLIR(dalvik_offset, kMips64Jalr, rZERO, rAT);
  InsertLIRBefore(lir, jalr);
  if (!unconditional) {
    InsertLIRBefore(lir, hop_target);
  }
  NopLIR(lir);
}

/*
 * Assemble the LIR into binary instruction format.  Note that we may
 * discover that pc-relative displacements may not fit the selected
 * instruction.  In those cases we will try to substitute a new code
 * sequence or request that the trace be shortened and retried.
 */
AssemblerStatus Mips64Mir2Lir::AssembleInstructions(CodeOffset start_addr) {
  LIR *lir;
  AssemblerStatus res = kSuccess;  // Assume success.

  for (lir = first_lir_insn_; lir != NULL; lir = NEXT_LIR(lir)) {
    if (lir->opcode < 0) {
      continue;
    }

    if (lir->flags.is_nop) {
      continue;
    }

    if (lir->flags.fixup != kFixupNone) {
      if (lir->opcode == kMips64Delta) {
        /*
         * The "Delta" pseudo-ops load the difference between
         * two pc-relative locations into a the target register
         * found in operands[0].  The delta is determined by
         * (label2 - label1), where label1 is a standard
         * kPseudoTargetLabel and is stored in operands[2].
         * If operands[3] is null, then label2 is a kPseudoTargetLabel
         * and is found in lir->target.  If operands[3] is non-NULL,
         * then it is a Switch/Data table.
         */
        int offset1 = UnwrapPointer<LIR>(lir->operands[2])->offset;
        const EmbeddedData* tab_rec = UnwrapPointer<EmbeddedData>(lir->operands[3]);
        int offset2 = tab_rec ? tab_rec->offset : lir->target->offset;
        int delta = offset2 - offset1;
        if ((delta & 0xffff) == delta && ((delta & 0x8000) == 0)) {
          // Fits.
          lir->operands[1] = delta;
        } else {
          // Doesn't fit - must expand to kMips64Delta[Hi|Lo] pair.
          LIR *new_delta_hi = RawLIR(lir->dalvik_offset, kMips64DeltaHi, lir->operands[0], 0,
                                     lir->operands[2], lir->operands[3], 0, lir->target);
          InsertLIRBefore(lir, new_delta_hi);
          LIR *new_delta_lo = RawLIR(lir->dalvik_offset, kMips64DeltaLo, lir->operands[0], 0,
                                     lir->operands[2], lir->operands[3], 0, lir->target);
          InsertLIRBefore(lir, new_delta_lo);
          LIR *new_addu = RawLIR(lir->dalvik_offset, kMips64Daddu, lir->operands[0],
                                 lir->operands[0], rRAd);
          InsertLIRBefore(lir, new_addu);
          NopLIR(lir);
          res = kRetryAll;
        }
      } else if (lir->opcode == kMips64DeltaLo) {
        int offset1 = UnwrapPointer<LIR>(lir->operands[2])->offset;
        const EmbeddedData* tab_rec = UnwrapPointer<EmbeddedData>(lir->operands[3]);
        int offset2 = tab_rec ? tab_rec->offset : lir->target->offset;
        int delta = offset2 - offset1;
        lir->operands[1] = delta & 0xffff;
      } else if (lir->opcode == kMips64DeltaHi) {
        int offset1 = UnwrapPointer<LIR>(lir->operands[2])->offset;
        const EmbeddedData* tab_rec = UnwrapPointer<EmbeddedData>(lir->operands[3]);
        int offset2 = tab_rec ? tab_rec->offset : lir->target->offset;
        int delta = offset2 - offset1;
        lir->operands[1] = (delta >> 16) & 0xffff;
      } else if (lir->opcode == kMips64B || lir->opcode == kMips64Bal) {
        LIR *target_lir = lir->target;
        CodeOffset pc = lir->offset + 4;
        CodeOffset target = target_lir->offset;
        int delta = target - pc;
        if (delta & 0x3) {
          LOG(FATAL) << "PC-rel offset not multiple of 4: " << delta;
        }
        if (delta > 131068 || delta < -131069) {
          res = kRetryAll;
          ConvertShortToLongBranch(lir);
        } else {
          lir->operands[0] = delta >> 2;
        }
      } else if (lir->opcode >= kMips64Beqz && lir->opcode <= kMips64Bnez) {
        LIR *target_lir = lir->target;
        CodeOffset pc = lir->offset + 4;
        CodeOffset target = target_lir->offset;
        int delta = target - pc;
        if (delta & 0x3) {
          LOG(FATAL) << "PC-rel offset not multiple of 4: " << delta;
        }
        if (delta > 131068 || delta < -131069) {
          res = kRetryAll;
          ConvertShortToLongBranch(lir);
        } else {
          lir->operands[1] = delta >> 2;
        }
      } else if (lir->opcode == kMips64Beq || lir->opcode == kMips64Bne) {
        LIR *target_lir = lir->target;
        CodeOffset pc = lir->offset + 4;
        CodeOffset target = target_lir->offset;
        int delta = target - pc;
        if (delta & 0x3) {
          LOG(FATAL) << "PC-rel offset not multiple of 4: " << delta;
        }
        if (delta > 131068 || delta < -131069) {
          res = kRetryAll;
          ConvertShortToLongBranch(lir);
        } else {
          lir->operands[2] = delta >> 2;
        }
      } else if (lir->opcode == kMips64Jal) {
        CodeOffset cur_pc = (start_addr + lir->offset + 4) & ~3;
        CodeOffset target = lir->operands[0];
        /* ensure PC-region branch can be used */
        DCHECK_EQ((cur_pc & 0xF0000000), (target & 0xF0000000));
        if (target & 0x3) {
          LOG(FATAL) << "Jump target not multiple of 4: " << target;
        }
        lir->operands[0] =  target >> 2;
      } else if (lir->opcode == kMips64Lahi) { /* ld address hi (via lui) */
        LIR *target_lir = lir->target;
        CodeOffset target = start_addr + target_lir->offset;
        lir->operands[1] = target >> 16;
      } else if (lir->opcode == kMips64Lalo) { /* ld address lo (via ori) */
        LIR *target_lir = lir->target;
        CodeOffset target = start_addr + target_lir->offset;
        lir->operands[2] = lir->operands[2] + target;
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
    DCHECK(!IsPseudoLirOp(lir->opcode));
    const Mips64EncodingMap *encoder = &EncodingMap[lir->opcode];
    uint32_t bits = encoder->skeleton;
    int i;
    for (i = 0; i < 4; i++) {
      uint32_t operand;
      uint32_t value;
      operand = lir->operands[i];
      switch (encoder->field_loc[i].kind) {
        case kFmtUnused:
          break;
        case kFmtBitBlt:
          if (encoder->field_loc[i].start == 0 && encoder->field_loc[i].end == 31) {
            value = operand;
          } else {
            value = (operand << encoder->field_loc[i].start) &
                ((1 << (encoder->field_loc[i].end + 1)) - 1);
          }
          bits |= value;
          break;
        case kFmtBlt5_2:
          value = (operand & 0x1f);
          bits |= (value << encoder->field_loc[i].start);
          bits |= (value << encoder->field_loc[i].end);
          break;
        case kFmtDfp: {
          // TODO: do we need to adjust now that we're using 64BitSolo?
          DCHECK(RegStorage::IsDouble(operand)) << ", Operand = 0x" << std::hex << operand;
          value = (RegStorage::RegNum(operand) << encoder->field_loc[i].start) &
              ((1 << (encoder->field_loc[i].end + 1)) - 1);
          bits |= value;
          break;
        }
        case kFmtSfp:
          DCHECK(RegStorage::IsSingle(operand)) << ", Operand = 0x" << std::hex << operand;
          value = (RegStorage::RegNum(operand) << encoder->field_loc[i].start) &
              ((1 << (encoder->field_loc[i].end + 1)) - 1);
          bits |= value;
          break;
        default:
          LOG(FATAL) << "Bad encoder format: " << encoder->field_loc[i].kind;
      }
    }
    // We only support little-endian MIPS64.
    code_buffer_.push_back(bits & 0xff);
    code_buffer_.push_back((bits >> 8) & 0xff);
    code_buffer_.push_back((bits >> 16) & 0xff);
    code_buffer_.push_back((bits >> 24) & 0xff);
    // TUNING: replace with proper delay slot handling.
    if (encoder->size == 8) {
      DCHECK(!IsPseudoLirOp(lir->opcode));
      const Mips64EncodingMap *encoder2 = &EncodingMap[kMips64Nop];
      uint32_t bits2 = encoder2->skeleton;
      code_buffer_.push_back(bits2 & 0xff);
      code_buffer_.push_back((bits2 >> 8) & 0xff);
      code_buffer_.push_back((bits2 >> 16) & 0xff);
      code_buffer_.push_back((bits2 >> 24) & 0xff);
    }
  }
  return res;
}

size_t Mips64Mir2Lir::GetInsnSize(LIR* lir) {
  DCHECK(!IsPseudoLirOp(lir->opcode));
  return EncodingMap[lir->opcode].size;
}

// LIR offset assignment.
// TODO: consolidate w/ Arm assembly mechanism.
int Mips64Mir2Lir::AssignInsnOffsets() {
  LIR* lir;
  int offset = 0;

  for (lir = first_lir_insn_; lir != NULL; lir = NEXT_LIR(lir)) {
    lir->offset = offset;
    if (LIKELY(lir->opcode >= 0)) {
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
    // Pseudo opcodes don't consume space.
  }
  return offset;
}

/*
 * Walk the compilation unit and assign offsets to instructions
 * and literals and compute the total size of the compiled unit.
 * TODO: consolidate w/ Arm assembly mechanism.
 */
void Mips64Mir2Lir::AssignOffsets() {
  int offset = AssignInsnOffsets();

  // Const values have to be word aligned.
  offset = RoundUp(offset, 4);

  // Set up offsets for literals.
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
void Mips64Mir2Lir::AssembleLIR() {
  cu_->NewTimingSplit("Assemble");
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
      // Redo offsets and try again.
      AssignOffsets();
      code_buffer_.clear();
    }
  }

  // Install literals.
  InstallLiteralPools();

  // Install switch tables.
  InstallSwitchTables();

  // Install fill array data.
  InstallFillArrayData();

  // Create the mapping table and native offset to reference map.
  cu_->NewTimingSplit("PcMappingTable");
  CreateMappingTables();

  cu_->NewTimingSplit("GcMap");
  CreateNativeGcMap();
}

}  // namespace art
