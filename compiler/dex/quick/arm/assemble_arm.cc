/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "arm_lir.h"
#include "codegen_arm.h"
#include "dex/quick/mir_to_lir-inl.h"

namespace art {

/*
 * opcode: ArmOpcode enum
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
                     k3, k3s, k3e, flags, name, fmt, size, fixup) \
        {skeleton, {{k0, ds, de}, {k1, s1s, s1e}, {k2, s2s, s2e}, \
                    {k3, k3s, k3e}}, opcode, flags, name, fmt, size, fixup}

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
 *     u -> 1st half of bl[x] target
 *     v -> 2nd half ob bl[x] target
 *     R -> register list
 *     s -> single precision floating point register
 *     S -> double precision floating point register
 *     m -> Thumb2 modified immediate
 *     n -> complimented Thumb2 modified immediate
 *     M -> Thumb2 16-bit zero-extended immediate
 *     b -> 4-digit binary
 *     B -> dmb option string (sy, st, ish, ishst, nsh, hshst)
 *     H -> operand shift
 *     C -> core register name
 *     P -> fp cs register list (base of s16)
 *     Q -> fp cs register list (base of s0)
 *
 *  [!] escape.  To insert "!", use "!!"
 */
/* NOTE: must be kept in sync with enum ArmOpcode from LIR.h */
const ArmEncodingMap ArmMir2Lir::EncodingMap[kArmLast] = {
    ENCODING_MAP(kArm16BitData,    0x0000,
                 kFmtBitBlt, 15, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP, "data", "0x!0h(!0d)", 2, kFixupNone),
    ENCODING_MAP(kThumbAdcRR,        0x4140,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE01 | SETS_CCODES | USES_CCODES,
                 "adcs", "!0C, !1C", 2, kFixupNone),
    ENCODING_MAP(kThumbAddRRI3,      0x1c00,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 8, 6,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_DEF0_USE1 | SETS_CCODES,
                 "adds", "!0C, !1C, #!2d", 2, kFixupNone),
    ENCODING_MAP(kThumbAddRI8,       0x3000,
                 kFmtBitBlt, 10, 8, kFmtBitBlt, 7, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE0 | SETS_CCODES,
                 "adds", "!0C, !0C, #!1d", 2, kFixupNone),
    ENCODING_MAP(kThumbAddRRR,       0x1800,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 8, 6,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_DEF0_USE12 | SETS_CCODES,
                 "adds", "!0C, !1C, !2C", 2, kFixupNone),
    ENCODING_MAP(kThumbAddRRLH,     0x4440,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE01,
                 "add", "!0C, !1C", 2, kFixupNone),
    ENCODING_MAP(kThumbAddRRHL,     0x4480,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE01,
                 "add", "!0C, !1C", 2, kFixupNone),
    ENCODING_MAP(kThumbAddRRHH,     0x44c0,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE01,
                 "add", "!0C, !1C", 2, kFixupNone),
    ENCODING_MAP(kThumbAddPcRel,    0xa000,
                 kFmtBitBlt, 10, 8, kFmtBitBlt, 7, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | IS_BRANCH | NEEDS_FIXUP,
                 "add", "!0C, pc, #!1E", 2, kFixupLoad),
    ENCODING_MAP(kThumbAddSpRel,    0xa800,
                 kFmtBitBlt, 10, 8, kFmtSkip, -1, -1, kFmtBitBlt, 7, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF_SP | REG_USE_SP,
                 "add", "!0C, sp, #!2E", 2, kFixupNone),
    ENCODING_MAP(kThumbAddSpI7,      0xb000,
                 kFmtBitBlt, 6, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP | REG_DEF_SP | REG_USE_SP,
                 "add", "sp, #!0d*4", 2, kFixupNone),
    ENCODING_MAP(kThumbAndRR,        0x4000,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE01 | SETS_CCODES,
                 "ands", "!0C, !1C", 2, kFixupNone),
    ENCODING_MAP(kThumbAsrRRI5,      0x1000,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 10, 6,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_DEF0_USE1 | SETS_CCODES,
                 "asrs", "!0C, !1C, #!2d", 2, kFixupNone),
    ENCODING_MAP(kThumbAsrRR,        0x4100,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE01 | SETS_CCODES,
                 "asrs", "!0C, !1C", 2, kFixupNone),
    ENCODING_MAP(kThumbBCond,        0xd000,
                 kFmtBitBlt, 7, 0, kFmtBitBlt, 11, 8, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | IS_BRANCH | USES_CCODES |
                 NEEDS_FIXUP, "b!1c", "!0t", 2, kFixupCondBranch),
    ENCODING_MAP(kThumbBUncond,      0xe000,
                 kFmtBitBlt, 10, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP | IS_BRANCH | NEEDS_FIXUP,
                 "b", "!0t", 2, kFixupT1Branch),
    ENCODING_MAP(kThumbBicRR,        0x4380,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE01 | SETS_CCODES,
                 "bics", "!0C, !1C", 2, kFixupNone),
    ENCODING_MAP(kThumbBkpt,          0xbe00,
                 kFmtBitBlt, 7, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP | IS_BRANCH,
                 "bkpt", "!0d", 2, kFixupNone),
    ENCODING_MAP(kThumbBlx1,         0xf000,
                 kFmtBitBlt, 10, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | IS_BRANCH | REG_DEF_LR |
                 NEEDS_FIXUP, "blx_1", "!0u", 2, kFixupBlx1),
    ENCODING_MAP(kThumbBlx2,         0xe800,
                 kFmtBitBlt, 10, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | IS_BRANCH | REG_DEF_LR |
                 NEEDS_FIXUP, "blx_2", "!0v", 2, kFixupLabel),
    ENCODING_MAP(kThumbBl1,          0xf000,
                 kFmtBitBlt, 10, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP | IS_BRANCH | REG_DEF_LR | NEEDS_FIXUP,
                 "bl_1", "!0u", 2, kFixupBl1),
    ENCODING_MAP(kThumbBl2,          0xf800,
                 kFmtBitBlt, 10, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP | IS_BRANCH | REG_DEF_LR | NEEDS_FIXUP,
                 "bl_2", "!0v", 2, kFixupLabel),
    ENCODING_MAP(kThumbBlxR,         0x4780,
                 kFmtBitBlt, 6, 3, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_UNARY_OP | REG_USE0 | IS_BRANCH | REG_DEF_LR,
                 "blx", "!0C", 2, kFixupNone),
    ENCODING_MAP(kThumbBx,            0x4700,
                 kFmtBitBlt, 6, 3, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP | IS_BRANCH,
                 "bx", "!0C", 2, kFixupNone),
    ENCODING_MAP(kThumbCmnRR,        0x42c0,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_USE01 | SETS_CCODES,
                 "cmn", "!0C, !1C", 2, kFixupNone),
    ENCODING_MAP(kThumbCmpRI8,       0x2800,
                 kFmtBitBlt, 10, 8, kFmtBitBlt, 7, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_USE0 | SETS_CCODES,
                 "cmp", "!0C, #!1d", 2, kFixupNone),
    ENCODING_MAP(kThumbCmpRR,        0x4280,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_USE01 | SETS_CCODES,
                 "cmp", "!0C, !1C", 2, kFixupNone),
    ENCODING_MAP(kThumbCmpLH,        0x4540,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_USE01 | SETS_CCODES,
                 "cmp", "!0C, !1C", 2, kFixupNone),
    ENCODING_MAP(kThumbCmpHL,        0x4580,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_USE01 | SETS_CCODES,
                 "cmp", "!0C, !1C", 2, kFixupNone),
    ENCODING_MAP(kThumbCmpHH,        0x45c0,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_USE01 | SETS_CCODES,
                 "cmp", "!0C, !1C", 2, kFixupNone),
    ENCODING_MAP(kThumbEorRR,        0x4040,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE01 | SETS_CCODES,
                 "eors", "!0C, !1C", 2, kFixupNone),
    ENCODING_MAP(kThumbLdmia,         0xc800,
                 kFmtBitBlt, 10, 8, kFmtBitBlt, 7, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE0 | REG_DEF_LIST1 | IS_LOAD,
                 "ldmia", "!0C!!, <!1R>", 2, kFixupNone),
    ENCODING_MAP(kThumbLdrRRI5,      0x6800,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 10, 6,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1 | IS_LOAD,
                 "ldr", "!0C, [!1C, #!2E]", 2, kFixupNone),
    ENCODING_MAP(kThumbLdrRRR,       0x5800,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 8, 6,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12 | IS_LOAD,
                 "ldr", "!0C, [!1C, !2C]", 2, kFixupNone),
    ENCODING_MAP(kThumbLdrPcRel,    0x4800,
                 kFmtBitBlt, 10, 8, kFmtBitBlt, 7, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0 | REG_USE_PC
                 | IS_LOAD | NEEDS_FIXUP, "ldr", "!0C, [pc, #!1E]", 2, kFixupLoad),
    ENCODING_MAP(kThumbLdrSpRel,    0x9800,
                 kFmtBitBlt, 10, 8, kFmtSkip, -1, -1, kFmtBitBlt, 7, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0 | REG_USE_SP
                 | IS_LOAD, "ldr", "!0C, [sp, #!2E]", 2, kFixupNone),
    ENCODING_MAP(kThumbLdrbRRI5,     0x7800,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 10, 6,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1 | IS_LOAD,
                 "ldrb", "!0C, [!1C, #2d]", 2, kFixupNone),
    ENCODING_MAP(kThumbLdrbRRR,      0x5c00,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 8, 6,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12 | IS_LOAD,
                 "ldrb", "!0C, [!1C, !2C]", 2, kFixupNone),
    ENCODING_MAP(kThumbLdrhRRI5,     0x8800,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 10, 6,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1 | IS_LOAD,
                 "ldrh", "!0C, [!1C, #!2F]", 2, kFixupNone),
    ENCODING_MAP(kThumbLdrhRRR,      0x5a00,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 8, 6,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12 | IS_LOAD,
                 "ldrh", "!0C, [!1C, !2C]", 2, kFixupNone),
    ENCODING_MAP(kThumbLdrsbRRR,     0x5600,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 8, 6,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12 | IS_LOAD,
                 "ldrsb", "!0C, [!1C, !2C]", 2, kFixupNone),
    ENCODING_MAP(kThumbLdrshRRR,     0x5e00,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 8, 6,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12 | IS_LOAD,
                 "ldrsh", "!0C, [!1C, !2C]", 2, kFixupNone),
    ENCODING_MAP(kThumbLslRRI5,      0x0000,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 10, 6,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_DEF0_USE1 | SETS_CCODES,
                 "lsls", "!0C, !1C, #!2d", 2, kFixupNone),
    ENCODING_MAP(kThumbLslRR,        0x4080,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE01 | SETS_CCODES,
                 "lsls", "!0C, !1C", 2, kFixupNone),
    ENCODING_MAP(kThumbLsrRRI5,      0x0800,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 10, 6,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_DEF0_USE1 | SETS_CCODES,
                 "lsrs", "!0C, !1C, #!2d", 2, kFixupNone),
    ENCODING_MAP(kThumbLsrRR,        0x40c0,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE01 | SETS_CCODES,
                 "lsrs", "!0C, !1C", 2, kFixupNone),
    ENCODING_MAP(kThumbMovImm,       0x2000,
                 kFmtBitBlt, 10, 8, kFmtBitBlt, 7, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0 | SETS_CCODES,
                 "movs", "!0C, #!1d", 2, kFixupNone),
    ENCODING_MAP(kThumbMovRR,        0x1c00,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE1 | SETS_CCODES,
                 "movs", "!0C, !1C", 2, kFixupNone),
    ENCODING_MAP(kThumbMovRR_H2H,    0x46c0,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "mov", "!0C, !1C", 2, kFixupNone),
    ENCODING_MAP(kThumbMovRR_H2L,    0x4640,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "mov", "!0C, !1C", 2, kFixupNone),
    ENCODING_MAP(kThumbMovRR_L2H,    0x4680,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "mov", "!0C, !1C", 2, kFixupNone),
    ENCODING_MAP(kThumbMul,           0x4340,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE01 | SETS_CCODES,
                 "muls", "!0C, !1C", 2, kFixupNone),
    ENCODING_MAP(kThumbMvn,           0x43c0,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE1 | SETS_CCODES,
                 "mvns", "!0C, !1C", 2, kFixupNone),
    ENCODING_MAP(kThumbNeg,           0x4240,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE1 | SETS_CCODES,
                 "negs", "!0C, !1C", 2, kFixupNone),
    ENCODING_MAP(kThumbOrr,           0x4300,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE01 | SETS_CCODES,
                 "orrs", "!0C, !1C", 2, kFixupNone),
    ENCODING_MAP(kThumbPop,           0xbc00,
                 kFmtBitBlt, 8, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_UNARY_OP | REG_DEF_SP | REG_USE_SP | REG_DEF_LIST0
                 | IS_LOAD, "pop", "<!0R>", 2, kFixupNone),
    ENCODING_MAP(kThumbPush,          0xb400,
                 kFmtBitBlt, 8, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_UNARY_OP | REG_DEF_SP | REG_USE_SP | REG_USE_LIST0
                 | IS_STORE, "push", "<!0R>", 2, kFixupNone),
    ENCODING_MAP(kThumbRev,           0xba00,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE1,
                 "rev", "!0C, !1C", 2, kFixupNone),
    ENCODING_MAP(kThumbRevsh,         0xbac0,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE1,
                 "rev", "!0C, !1C", 2, kFixupNone),
    ENCODING_MAP(kThumbRorRR,        0x41c0,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE01 | SETS_CCODES,
                 "rors", "!0C, !1C", 2, kFixupNone),
    ENCODING_MAP(kThumbSbc,           0x4180,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE01 | USES_CCODES | SETS_CCODES,
                 "sbcs", "!0C, !1C", 2, kFixupNone),
    ENCODING_MAP(kThumbStmia,         0xc000,
                 kFmtBitBlt, 10, 8, kFmtBitBlt, 7, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0 | REG_USE0 | REG_USE_LIST1 | IS_STORE,
                 "stmia", "!0C!!, <!1R>", 2, kFixupNone),
    ENCODING_MAP(kThumbStrRRI5,      0x6000,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 10, 6,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE01 | IS_STORE,
                 "str", "!0C, [!1C, #!2E]", 2, kFixupNone),
    ENCODING_MAP(kThumbStrRRR,       0x5000,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 8, 6,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE012 | IS_STORE,
                 "str", "!0C, [!1C, !2C]", 2, kFixupNone),
    ENCODING_MAP(kThumbStrSpRel,    0x9000,
                 kFmtBitBlt, 10, 8, kFmtSkip, -1, -1, kFmtBitBlt, 7, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE0 | REG_USE_SP
                 | IS_STORE, "str", "!0C, [sp, #!2E]", 2, kFixupNone),
    ENCODING_MAP(kThumbStrbRRI5,     0x7000,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 10, 6,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE01 | IS_STORE,
                 "strb", "!0C, [!1C, #!2d]", 2, kFixupNone),
    ENCODING_MAP(kThumbStrbRRR,      0x5400,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 8, 6,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE012 | IS_STORE,
                 "strb", "!0C, [!1C, !2C]", 2, kFixupNone),
    ENCODING_MAP(kThumbStrhRRI5,     0x8000,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 10, 6,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE01 | IS_STORE,
                 "strh", "!0C, [!1C, #!2F]", 2, kFixupNone),
    ENCODING_MAP(kThumbStrhRRR,      0x5200,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 8, 6,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE012 | IS_STORE,
                 "strh", "!0C, [!1C, !2C]", 2, kFixupNone),
    ENCODING_MAP(kThumbSubRRI3,      0x1e00,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 8, 6,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_DEF0_USE1 | SETS_CCODES,
                 "subs", "!0C, !1C, #!2d", 2, kFixupNone),
    ENCODING_MAP(kThumbSubRI8,       0x3800,
                 kFmtBitBlt, 10, 8, kFmtBitBlt, 7, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE0 | SETS_CCODES,
                 "subs", "!0C, #!1d", 2, kFixupNone),
    ENCODING_MAP(kThumbSubRRR,       0x1a00,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 8, 6,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_DEF0_USE12 | SETS_CCODES,
                 "subs", "!0C, !1C, !2C", 2, kFixupNone),
    ENCODING_MAP(kThumbSubSpI7,      0xb080,
                 kFmtBitBlt, 6, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_UNARY_OP | REG_DEF_SP | REG_USE_SP,
                 "sub", "sp, #!0d*4", 2, kFixupNone),
    ENCODING_MAP(kThumbSwi,           0xdf00,
                 kFmtBitBlt, 7, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP | IS_BRANCH,
                 "swi", "!0d", 2, kFixupNone),
    ENCODING_MAP(kThumbTst,           0x4200,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP | REG_USE01 | SETS_CCODES,
                 "tst", "!0C, !1C", 2, kFixupNone),
    /*
     * Note: The encoding map entries for vldrd and vldrs include REG_DEF_LR, even though
     * these instructions don't define lr.  The reason is that these instructions
     * are used for loading values from the literal pool, and the displacement may be found
     * to be insuffient at assembly time.  In that case, we need to materialize a new base
     * register - and will use lr as the temp register.  This works because lr is used as
     * a temp register in very limited situations, and never in conjunction with a floating
     * point constant load.  However, it is possible that during instruction scheduling,
     * another use of lr could be moved across a vldrd/vldrs.  By setting REG_DEF_LR, we
     * prevent that from happening.  Note that we set REG_DEF_LR on all vldrd/vldrs - even those
     * not used in a pc-relative case.  It is really only needed on the pc-relative loads, but
     * the case we're handling is rare enough that it seemed not worth the trouble to distinguish.
     */
    ENCODING_MAP(kThumb2Vldrs,       0xed900a00,
                 kFmtSfp, 22, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 7, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1 | IS_LOAD |
                 REG_DEF_LR | NEEDS_FIXUP, "vldr", "!0s, [!1C, #!2E]", 4, kFixupVLoad),
    ENCODING_MAP(kThumb2Vldrd,       0xed900b00,
                 kFmtDfp, 22, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 7, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1 | IS_LOAD |
                 REG_DEF_LR | NEEDS_FIXUP, "vldr", "!0S, [!1C, #!2E]", 4, kFixupVLoad),
    ENCODING_MAP(kThumb2Vmuls,        0xee200a00,
                 kFmtSfp, 22, 12, kFmtSfp, 7, 16, kFmtSfp, 5, 0,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_DEF0_USE12,
                 "vmuls", "!0s, !1s, !2s", 4, kFixupNone),
    ENCODING_MAP(kThumb2Vmuld,        0xee200b00,
                 kFmtDfp, 22, 12, kFmtDfp, 7, 16, kFmtDfp, 5, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "vmuld", "!0S, !1S, !2S", 4, kFixupNone),
    ENCODING_MAP(kThumb2Vstrs,       0xed800a00,
                 kFmtSfp, 22, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 7, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE01 | IS_STORE,
                 "vstr", "!0s, [!1C, #!2E]", 4, kFixupNone),
    ENCODING_MAP(kThumb2Vstrd,       0xed800b00,
                 kFmtDfp, 22, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 7, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE01 | IS_STORE,
                 "vstr", "!0S, [!1C, #!2E]", 4, kFixupNone),
    ENCODING_MAP(kThumb2Vsubs,        0xee300a40,
                 kFmtSfp, 22, 12, kFmtSfp, 7, 16, kFmtSfp, 5, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "vsub", "!0s, !1s, !2s", 4, kFixupNone),
    ENCODING_MAP(kThumb2Vsubd,        0xee300b40,
                 kFmtDfp, 22, 12, kFmtDfp, 7, 16, kFmtDfp, 5, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "vsub", "!0S, !1S, !2S", 4, kFixupNone),
    ENCODING_MAP(kThumb2Vadds,        0xee300a00,
                 kFmtSfp, 22, 12, kFmtSfp, 7, 16, kFmtSfp, 5, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "vadd", "!0s, !1s, !2s", 4, kFixupNone),
    ENCODING_MAP(kThumb2Vaddd,        0xee300b00,
                 kFmtDfp, 22, 12, kFmtDfp, 7, 16, kFmtDfp, 5, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "vadd", "!0S, !1S, !2S", 4, kFixupNone),
    ENCODING_MAP(kThumb2Vdivs,        0xee800a00,
                 kFmtSfp, 22, 12, kFmtSfp, 7, 16, kFmtSfp, 5, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "vdivs", "!0s, !1s, !2s", 4, kFixupNone),
    ENCODING_MAP(kThumb2Vdivd,        0xee800b00,
                 kFmtDfp, 22, 12, kFmtDfp, 7, 16, kFmtDfp, 5, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "vdivd", "!0S, !1S, !2S", 4, kFixupNone),
    ENCODING_MAP(kThumb2VmlaF64,     0xee000b00,
                 kFmtDfp, 22, 12, kFmtDfp, 7, 16, kFmtDfp, 5, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0 | REG_USE012,
                 "vmla", "!0S, !1S, !2S", 4, kFixupNone),
    ENCODING_MAP(kThumb2VcvtIF,       0xeeb80ac0,
                 kFmtSfp, 22, 12, kFmtSfp, 5, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "vcvt.f32.s32", "!0s, !1s", 4, kFixupNone),
    ENCODING_MAP(kThumb2VcvtFI,       0xeebd0ac0,
                 kFmtSfp, 22, 12, kFmtSfp, 5, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "vcvt.s32.f32 ", "!0s, !1s", 4, kFixupNone),
    ENCODING_MAP(kThumb2VcvtDI,       0xeebd0bc0,
                 kFmtSfp, 22, 12, kFmtDfp, 5, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "vcvt.s32.f64 ", "!0s, !1S", 4, kFixupNone),
    ENCODING_MAP(kThumb2VcvtFd,       0xeeb70ac0,
                 kFmtDfp, 22, 12, kFmtSfp, 5, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "vcvt.f64.f32 ", "!0S, !1s", 4, kFixupNone),
    ENCODING_MAP(kThumb2VcvtDF,       0xeeb70bc0,
                 kFmtSfp, 22, 12, kFmtDfp, 5, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "vcvt.f32.f64 ", "!0s, !1S", 4, kFixupNone),
    ENCODING_MAP(kThumb2VcvtF64S32,   0xeeb80bc0,
                 kFmtDfp, 22, 12, kFmtSfp, 5, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "vcvt.f64.s32 ", "!0S, !1s", 4, kFixupNone),
    ENCODING_MAP(kThumb2VcvtF64U32,   0xeeb80b40,
                 kFmtDfp, 22, 12, kFmtSfp, 5, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "vcvt.f64.u32 ", "!0S, !1s", 4, kFixupNone),
    ENCODING_MAP(kThumb2Vsqrts,       0xeeb10ac0,
                 kFmtSfp, 22, 12, kFmtSfp, 5, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "vsqrt.f32 ", "!0s, !1s", 4, kFixupNone),
    ENCODING_MAP(kThumb2Vsqrtd,       0xeeb10bc0,
                 kFmtDfp, 22, 12, kFmtDfp, 5, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "vsqrt.f64 ", "!0S, !1S", 4, kFixupNone),
    ENCODING_MAP(kThumb2MovI8M, 0xf04f0000, /* no setflags encoding */
                 kFmtBitBlt, 11, 8, kFmtModImm, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0,
                 "mov", "!0C, #!1m", 4, kFixupNone),
    ENCODING_MAP(kThumb2MovImm16,       0xf2400000,
                 kFmtBitBlt, 11, 8, kFmtImm16, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0,
                 "mov", "!0C, #!1M", 4, kFixupNone),
    ENCODING_MAP(kThumb2StrRRI12,       0xf8c00000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 11, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE01 | IS_STORE,
                 "str", "!0C, [!1C, #!2d]", 4, kFixupNone),
    ENCODING_MAP(kThumb2LdrRRI12,       0xf8d00000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 11, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1 | IS_LOAD,
                 "ldr", "!0C, [!1C, #!2d]", 4, kFixupNone),
    ENCODING_MAP(kThumb2StrRRI8Predec,       0xf8400c00,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 8, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE01 | IS_STORE,
                 "str", "!0C, [!1C, #-!2d]", 4, kFixupNone),
    ENCODING_MAP(kThumb2LdrRRI8Predec,       0xf8500c00,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 8, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1 | IS_LOAD,
                 "ldr", "!0C, [!1C, #-!2d]", 4, kFixupNone),
    ENCODING_MAP(kThumb2Cbnz,       0xb900, /* Note: does not affect flags */
                 kFmtBitBlt, 2, 0, kFmtImm6, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_USE0 | IS_BRANCH |
                 NEEDS_FIXUP, "cbnz", "!0C,!1t", 2, kFixupCBxZ),
    ENCODING_MAP(kThumb2Cbz,       0xb100, /* Note: does not affect flags */
                 kFmtBitBlt, 2, 0, kFmtImm6, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_USE0 | IS_BRANCH |
                 NEEDS_FIXUP, "cbz", "!0C,!1t", 2, kFixupCBxZ),
    ENCODING_MAP(kThumb2AddRRI12,       0xf2000000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtImm12, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_DEF0_USE1,/* Note: doesn't affect flags */
                 "add", "!0C,!1C,#!2d", 4, kFixupNone),
    ENCODING_MAP(kThumb2MovRR,       0xea4f0000, /* no setflags encoding */
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 3, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "mov", "!0C, !1C", 4, kFixupNone),
    ENCODING_MAP(kThumb2Vmovs,       0xeeb00a40,
                 kFmtSfp, 22, 12, kFmtSfp, 5, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "vmov.f32 ", " !0s, !1s", 4, kFixupNone),
    ENCODING_MAP(kThumb2Vmovd,       0xeeb00b40,
                 kFmtDfp, 22, 12, kFmtDfp, 5, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "vmov.f64 ", " !0S, !1S", 4, kFixupNone),
    ENCODING_MAP(kThumb2Ldmia,         0xe8900000,
                 kFmtBitBlt, 19, 16, kFmtBitBlt, 15, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE0 | REG_DEF_LIST1 | IS_LOAD,
                 "ldmia", "!0C!!, <!1R>", 4, kFixupNone),
    ENCODING_MAP(kThumb2Stmia,         0xe8800000,
                 kFmtBitBlt, 19, 16, kFmtBitBlt, 15, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE0 | REG_USE_LIST1 | IS_STORE,
                 "stmia", "!0C!!, <!1R>", 4, kFixupNone),
    ENCODING_MAP(kThumb2AddRRR,  0xeb100000, /* setflags encoding */
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtShift, -1, -1,
                 IS_QUAD_OP | REG_DEF0_USE12 | SETS_CCODES,
                 "adds", "!0C, !1C, !2C!3H", 4, kFixupNone),
    ENCODING_MAP(kThumb2SubRRR,       0xebb00000, /* setflags enconding */
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtShift, -1, -1,
                 IS_QUAD_OP | REG_DEF0_USE12 | SETS_CCODES,
                 "subs", "!0C, !1C, !2C!3H", 4, kFixupNone),
    ENCODING_MAP(kThumb2SbcRRR,       0xeb700000, /* setflags encoding */
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtShift, -1, -1,
                 IS_QUAD_OP | REG_DEF0_USE12 | USES_CCODES | SETS_CCODES,
                 "sbcs", "!0C, !1C, !2C!3H", 4, kFixupNone),
    ENCODING_MAP(kThumb2CmpRR,       0xebb00f00,
                 kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0, kFmtShift, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_USE01 | SETS_CCODES,
                 "cmp", "!0C, !1C", 4, kFixupNone),
    ENCODING_MAP(kThumb2SubRRI12,       0xf2a00000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtImm12, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_DEF0_USE1,/* Note: doesn't affect flags */
                 "sub", "!0C,!1C,#!2d", 4, kFixupNone),
    ENCODING_MAP(kThumb2MvnI8M,  0xf06f0000, /* no setflags encoding */
                 kFmtBitBlt, 11, 8, kFmtModImm, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0,
                 "mvn", "!0C, #!1n", 4, kFixupNone),
    ENCODING_MAP(kThumb2Sel,       0xfaa0f080,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_DEF0_USE12 | USES_CCODES,
                 "sel", "!0C, !1C, !2C", 4, kFixupNone),
    ENCODING_MAP(kThumb2Ubfx,       0xf3c00000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtLsb, -1, -1,
                 kFmtBWidth, 4, 0, IS_QUAD_OP | REG_DEF0_USE1,
                 "ubfx", "!0C, !1C, #!2d, #!3d", 4, kFixupNone),
    ENCODING_MAP(kThumb2Sbfx,       0xf3400000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtLsb, -1, -1,
                 kFmtBWidth, 4, 0, IS_QUAD_OP | REG_DEF0_USE1,
                 "sbfx", "!0C, !1C, #!2d, #!3d", 4, kFixupNone),
    ENCODING_MAP(kThumb2LdrRRR,    0xf8500000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtBitBlt, 5, 4, IS_QUAD_OP | REG_DEF0_USE12 | IS_LOAD,
                 "ldr", "!0C, [!1C, !2C, LSL #!3d]", 4, kFixupNone),
    ENCODING_MAP(kThumb2LdrhRRR,    0xf8300000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtBitBlt, 5, 4, IS_QUAD_OP | REG_DEF0_USE12 | IS_LOAD,
                 "ldrh", "!0C, [!1C, !2C, LSL #!3d]", 4, kFixupNone),
    ENCODING_MAP(kThumb2LdrshRRR,    0xf9300000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtBitBlt, 5, 4, IS_QUAD_OP | REG_DEF0_USE12 | IS_LOAD,
                 "ldrsh", "!0C, [!1C, !2C, LSL #!3d]", 4, kFixupNone),
    ENCODING_MAP(kThumb2LdrbRRR,    0xf8100000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtBitBlt, 5, 4, IS_QUAD_OP | REG_DEF0_USE12 | IS_LOAD,
                 "ldrb", "!0C, [!1C, !2C, LSL #!3d]", 4, kFixupNone),
    ENCODING_MAP(kThumb2LdrsbRRR,    0xf9100000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtBitBlt, 5, 4, IS_QUAD_OP | REG_DEF0_USE12 | IS_LOAD,
                 "ldrsb", "!0C, [!1C, !2C, LSL #!3d]", 4, kFixupNone),
    ENCODING_MAP(kThumb2StrRRR,    0xf8400000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtBitBlt, 5, 4, IS_QUAD_OP | REG_USE012 | IS_STORE,
                 "str", "!0C, [!1C, !2C, LSL #!3d]", 4, kFixupNone),
    ENCODING_MAP(kThumb2StrhRRR,    0xf8200000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtBitBlt, 5, 4, IS_QUAD_OP | REG_USE012 | IS_STORE,
                 "strh", "!0C, [!1C, !2C, LSL #!3d]", 4, kFixupNone),
    ENCODING_MAP(kThumb2StrbRRR,    0xf8000000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtBitBlt, 5, 4, IS_QUAD_OP | REG_USE012 | IS_STORE,
                 "strb", "!0C, [!1C, !2C, LSL #!3d]", 4, kFixupNone),
    ENCODING_MAP(kThumb2LdrhRRI12,       0xf8b00000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 11, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1 | IS_LOAD,
                 "ldrh", "!0C, [!1C, #!2d]", 4, kFixupNone),
    ENCODING_MAP(kThumb2LdrshRRI12,       0xf9b00000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 11, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1 | IS_LOAD,
                 "ldrsh", "!0C, [!1C, #!2d]", 4, kFixupNone),
    ENCODING_MAP(kThumb2LdrbRRI12,       0xf8900000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 11, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1 | IS_LOAD,
                 "ldrb", "!0C, [!1C, #!2d]", 4, kFixupNone),
    ENCODING_MAP(kThumb2LdrsbRRI12,       0xf9900000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 11, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1 | IS_LOAD,
                 "ldrsb", "!0C, [!1C, #!2d]", 4, kFixupNone),
    ENCODING_MAP(kThumb2StrhRRI12,       0xf8a00000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 11, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE01 | IS_STORE,
                 "strh", "!0C, [!1C, #!2d]", 4, kFixupNone),
    ENCODING_MAP(kThumb2StrbRRI12,       0xf8800000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 11, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE01 | IS_STORE,
                 "strb", "!0C, [!1C, #!2d]", 4, kFixupNone),
    ENCODING_MAP(kThumb2Pop,           0xe8bd0000,
                 kFmtBitBlt, 15, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_UNARY_OP | REG_DEF_SP | REG_USE_SP | REG_DEF_LIST0
                 | IS_LOAD | NEEDS_FIXUP, "pop", "<!0R>", 4, kFixupPushPop),
    ENCODING_MAP(kThumb2Push,          0xe92d0000,
                 kFmtBitBlt, 15, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_UNARY_OP | REG_DEF_SP | REG_USE_SP | REG_USE_LIST0
                 | IS_STORE | NEEDS_FIXUP, "push", "<!0R>", 4, kFixupPushPop),
    ENCODING_MAP(kThumb2CmpRI8M, 0xf1b00f00,
                 kFmtBitBlt, 19, 16, kFmtModImm, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_USE0 | SETS_CCODES,
                 "cmp", "!0C, #!1m", 4, kFixupNone),
    ENCODING_MAP(kThumb2CmnRI8M, 0xf1100f00,
                 kFmtBitBlt, 19, 16, kFmtModImm, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_USE0 | SETS_CCODES,
                 "cmn", "!0C, #!1m", 4, kFixupNone),
    ENCODING_MAP(kThumb2AdcRRR,  0xeb500000, /* setflags encoding */
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtShift, -1, -1,
                 IS_QUAD_OP | REG_DEF0_USE12 | SETS_CCODES,
                 "adcs", "!0C, !1C, !2C!3H", 4, kFixupNone),
    ENCODING_MAP(kThumb2AndRRR,  0xea000000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtShift, -1, -1, IS_QUAD_OP | REG_DEF0_USE12,
                 "and", "!0C, !1C, !2C!3H", 4, kFixupNone),
    ENCODING_MAP(kThumb2BicRRR,  0xea200000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtShift, -1, -1, IS_QUAD_OP | REG_DEF0_USE12,
                 "bic", "!0C, !1C, !2C!3H", 4, kFixupNone),
    ENCODING_MAP(kThumb2CmnRR,  0xeb000000,
                 kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0, kFmtShift, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_DEF0_USE1 | SETS_CCODES,
                 "cmn", "!0C, !1C, shift !2d", 4, kFixupNone),
    ENCODING_MAP(kThumb2EorRRR,  0xea800000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtShift, -1, -1, IS_QUAD_OP | REG_DEF0_USE12,
                 "eor", "!0C, !1C, !2C!3H", 4, kFixupNone),
    ENCODING_MAP(kThumb2MulRRR,  0xfb00f000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "mul", "!0C, !1C, !2C", 4, kFixupNone),
    ENCODING_MAP(kThumb2SdivRRR,  0xfb90f0f0,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "sdiv", "!0C, !1C, !2C", 4, kFixupNone),
    ENCODING_MAP(kThumb2UdivRRR,  0xfbb0f0f0,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "udiv", "!0C, !1C, !2C", 4, kFixupNone),
    ENCODING_MAP(kThumb2MnvRR,  0xea6f0000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 3, 0, kFmtShift, -1, -1,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "mvn", "!0C, !1C, shift !2d", 4, kFixupNone),
    ENCODING_MAP(kThumb2RsubRRI8M,       0xf1d00000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtModImm, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_DEF0_USE1 | SETS_CCODES,
                 "rsbs", "!0C,!1C,#!2m", 4, kFixupNone),
    ENCODING_MAP(kThumb2NegRR,       0xf1d00000, /* instance of rsub */
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE1 | SETS_CCODES,
                 "neg", "!0C,!1C", 4, kFixupNone),
    ENCODING_MAP(kThumb2OrrRRR,  0xea400000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtShift, -1, -1, IS_QUAD_OP | REG_DEF0_USE12,
                 "orr", "!0C, !1C, !2C!3H", 4, kFixupNone),
    ENCODING_MAP(kThumb2TstRR,       0xea100f00,
                 kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0, kFmtShift, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_USE01 | SETS_CCODES,
                 "tst", "!0C, !1C, shift !2d", 4, kFixupNone),
    ENCODING_MAP(kThumb2LslRRR,  0xfa00f000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "lsl", "!0C, !1C, !2C", 4, kFixupNone),
    ENCODING_MAP(kThumb2LsrRRR,  0xfa20f000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "lsr", "!0C, !1C, !2C", 4, kFixupNone),
    ENCODING_MAP(kThumb2AsrRRR,  0xfa40f000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "asr", "!0C, !1C, !2C", 4, kFixupNone),
    ENCODING_MAP(kThumb2RorRRR,  0xfa60f000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "ror", "!0C, !1C, !2C", 4, kFixupNone),
    ENCODING_MAP(kThumb2LslRRI5,  0xea4f0000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 3, 0, kFmtShift5, -1, -1,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "lsl", "!0C, !1C, #!2d", 4, kFixupNone),
    ENCODING_MAP(kThumb2LsrRRI5,  0xea4f0010,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 3, 0, kFmtShift5, -1, -1,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "lsr", "!0C, !1C, #!2d", 4, kFixupNone),
    ENCODING_MAP(kThumb2AsrRRI5,  0xea4f0020,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 3, 0, kFmtShift5, -1, -1,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "asr", "!0C, !1C, #!2d", 4, kFixupNone),
    ENCODING_MAP(kThumb2RorRRI5,  0xea4f0030,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 3, 0, kFmtShift5, -1, -1,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "ror", "!0C, !1C, #!2d", 4, kFixupNone),
    ENCODING_MAP(kThumb2BicRRI8M,  0xf0200000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtModImm, -1, -1,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "bic", "!0C, !1C, #!2m", 4, kFixupNone),
    ENCODING_MAP(kThumb2AndRRI8M,  0xf0000000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtModImm, -1, -1,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "and", "!0C, !1C, #!2m", 4, kFixupNone),
    ENCODING_MAP(kThumb2OrrRRI8M,  0xf0400000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtModImm, -1, -1,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "orr", "!0C, !1C, #!2m", 4, kFixupNone),
    ENCODING_MAP(kThumb2EorRRI8M,  0xf0800000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtModImm, -1, -1,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "eor", "!0C, !1C, #!2m", 4, kFixupNone),
    ENCODING_MAP(kThumb2AddRRI8M,  0xf1100000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtModImm, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_DEF0_USE1 | SETS_CCODES,
                 "adds", "!0C, !1C, #!2m", 4, kFixupNone),
    ENCODING_MAP(kThumb2AdcRRI8M,  0xf1500000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtModImm, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_DEF0_USE1 | SETS_CCODES | USES_CCODES,
                 "adcs", "!0C, !1C, #!2m", 4, kFixupNone),
    ENCODING_MAP(kThumb2SubRRI8M,  0xf1b00000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtModImm, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_DEF0_USE1 | SETS_CCODES,
                 "subs", "!0C, !1C, #!2m", 4, kFixupNone),
    ENCODING_MAP(kThumb2SbcRRI8M,  0xf1700000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtModImm, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_DEF0_USE1 | SETS_CCODES | USES_CCODES,
                 "sbcs", "!0C, !1C, #!2m", 4, kFixupNone),
    ENCODING_MAP(kThumb2RevRR, 0xfa90f080,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_DEF0_USE12,  // Binary, but rm is stored twice.
                 "rev", "!0C, !1C", 4, kFixupNone),
    ENCODING_MAP(kThumb2RevshRR, 0xfa90f0b0,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_DEF0_USE12,  // Binary, but rm is stored twice.
                 "revsh", "!0C, !1C", 4, kFixupNone),
    ENCODING_MAP(kThumb2It,  0xbf00,
                 kFmtBitBlt, 7, 4, kFmtBitBlt, 3, 0, kFmtModImm, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | IS_IT | USES_CCODES,
                 "it:!1b", "!0c", 2, kFixupNone),
    ENCODING_MAP(kThumb2Fmstat,  0xeef1fa10,
                 kFmtUnused, -1, -1, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, NO_OPERAND | SETS_CCODES,
                 "fmstat", "", 4, kFixupNone),
    ENCODING_MAP(kThumb2Vcmpd,        0xeeb40b40,
                 kFmtDfp, 22, 12, kFmtDfp, 5, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_USE01,
                 "vcmp.f64", "!0S, !1S", 4, kFixupNone),
    ENCODING_MAP(kThumb2Vcmps,        0xeeb40a40,
                 kFmtSfp, 22, 12, kFmtSfp, 5, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_USE01,
                 "vcmp.f32", "!0s, !1s", 4, kFixupNone),
    ENCODING_MAP(kThumb2LdrPcRel12,       0xf8df0000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 11, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_DEF0 | REG_USE_PC | IS_LOAD | NEEDS_FIXUP,
                 "ldr", "!0C, [r15pc, #!1d]", 4, kFixupLoad),
    ENCODING_MAP(kThumb2BCond,        0xf0008000,
                 kFmtBrOffset, -1, -1, kFmtBitBlt, 25, 22, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | IS_BRANCH | USES_CCODES | NEEDS_FIXUP,
                 "b!1c", "!0t", 4, kFixupCondBranch),
    ENCODING_MAP(kThumb2Fmrs,       0xee100a10,
                 kFmtBitBlt, 15, 12, kFmtSfp, 7, 16, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "fmrs", "!0C, !1s", 4, kFixupNone),
    ENCODING_MAP(kThumb2Fmsr,       0xee000a10,
                 kFmtSfp, 7, 16, kFmtBitBlt, 15, 12, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "fmsr", "!0s, !1C", 4, kFixupNone),
    ENCODING_MAP(kThumb2Fmrrd,       0xec500b10,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16, kFmtDfp, 5, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF01_USE2,
                 "fmrrd", "!0C, !1C, !2S", 4, kFixupNone),
    ENCODING_MAP(kThumb2Fmdrr,       0xec400b10,
                 kFmtDfp, 5, 0, kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "fmdrr", "!0S, !1C, !2C", 4, kFixupNone),
    ENCODING_MAP(kThumb2Vabsd,       0xeeb00bc0,
                 kFmtDfp, 22, 12, kFmtDfp, 5, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "vabs.f64", "!0S, !1S", 4, kFixupNone),
    ENCODING_MAP(kThumb2Vabss,       0xeeb00ac0,
                 kFmtSfp, 22, 12, kFmtSfp, 5, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "vabs.f32", "!0s, !1s", 4, kFixupNone),
    ENCODING_MAP(kThumb2Vnegd,       0xeeb10b40,
                 kFmtDfp, 22, 12, kFmtDfp, 5, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "vneg.f64", "!0S, !1S", 4, kFixupNone),
    ENCODING_MAP(kThumb2Vnegs,       0xeeb10a40,
                 kFmtSfp, 22, 12, kFmtSfp, 5, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "vneg.f32", "!0s, !1s", 4, kFixupNone),
    ENCODING_MAP(kThumb2Vmovs_IMM8,       0xeeb00a00,
                 kFmtSfp, 22, 12, kFmtFPImm, 16, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0,
                 "vmov.f32", "!0s, #0x!1h", 4, kFixupNone),
    ENCODING_MAP(kThumb2Vmovd_IMM8,       0xeeb00b00,
                 kFmtDfp, 22, 12, kFmtFPImm, 16, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0,
                 "vmov.f64", "!0S, #0x!1h", 4, kFixupNone),
    ENCODING_MAP(kThumb2Mla,  0xfb000000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtBitBlt, 15, 12, IS_QUAD_OP | REG_DEF0_USE123,
                 "mla", "!0C, !1C, !2C, !3C", 4, kFixupNone),
    ENCODING_MAP(kThumb2Umull,  0xfba00000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16,
                 kFmtBitBlt, 3, 0,
                 IS_QUAD_OP | REG_DEF0 | REG_DEF1 | REG_USE2 | REG_USE3,
                 "umull", "!0C, !1C, !2C, !3C", 4, kFixupNone),
    ENCODING_MAP(kThumb2Ldrex,       0xe8500f00,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 7, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1 | IS_LOAD,
                 "ldrex", "!0C, [!1C, #!2E]", 4, kFixupNone),
    ENCODING_MAP(kThumb2Ldrexd,      0xe8d0007f,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF01_USE2 | IS_LOAD,
                 "ldrexd", "!0C, !1C, [!2C]", 4, kFixupNone),
    ENCODING_MAP(kThumb2Strex,       0xe8400000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16,
                 kFmtBitBlt, 7, 0, IS_QUAD_OP | REG_DEF0_USE12 | IS_STORE,
                 "strex", "!0C, !1C, [!2C, #!2E]", 4, kFixupNone),
    ENCODING_MAP(kThumb2Strexd,      0xe8c00070,
                 kFmtBitBlt, 3, 0, kFmtBitBlt, 15, 12, kFmtBitBlt, 11, 8,
                 kFmtBitBlt, 19, 16, IS_QUAD_OP | REG_DEF0_USE123 | IS_STORE,
                 "strexd", "!0C, !1C, !2C, [!3C]", 4, kFixupNone),
    ENCODING_MAP(kThumb2Clrex,       0xf3bf8f2f,
                 kFmtUnused, -1, -1, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, NO_OPERAND,
                 "clrex", "", 4, kFixupNone),
    ENCODING_MAP(kThumb2Bfi,         0xf3600000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtShift5, -1, -1,
                 kFmtBitBlt, 4, 0, IS_QUAD_OP | REG_DEF0_USE1,
                 "bfi", "!0C,!1C,#!2d,#!3d", 4, kFixupNone),
    ENCODING_MAP(kThumb2Bfc,         0xf36f0000,
                 kFmtBitBlt, 11, 8, kFmtShift5, -1, -1, kFmtBitBlt, 4, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0,
                 "bfc", "!0C,#!1d,#!2d", 4, kFixupNone),
    ENCODING_MAP(kThumb2Dmb,         0xf3bf8f50,
                 kFmtBitBlt, 3, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP,
                 "dmb", "#!0B", 4, kFixupNone),
    ENCODING_MAP(kThumb2LdrPcReln12,       0xf85f0000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 11, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0 | REG_USE_PC | IS_LOAD,
                 "ldr", "!0C, [r15pc, -#!1d]", 4, kFixupNone),
    ENCODING_MAP(kThumb2Stm,          0xe9000000,
                 kFmtBitBlt, 19, 16, kFmtBitBlt, 12, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_USE0 | REG_USE_LIST1 | IS_STORE,
                 "stm", "!0C, <!1R>", 4, kFixupNone),
    ENCODING_MAP(kThumbUndefined,       0xde00,
                 kFmtUnused, -1, -1, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, NO_OPERAND,
                 "undefined", "", 2, kFixupNone),
    // NOTE: vpop, vpush hard-encoded for s16+ reg list
    ENCODING_MAP(kThumb2VPopCS,       0xecbd8a00,
                 kFmtBitBlt, 7, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_UNARY_OP | REG_DEF_SP | REG_USE_SP | REG_DEF_FPCS_LIST0
                 | IS_LOAD, "vpop", "<!0P>", 4, kFixupNone),
    ENCODING_MAP(kThumb2VPushCS,      0xed2d8a00,
                 kFmtBitBlt, 7, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_UNARY_OP | REG_DEF_SP | REG_USE_SP | REG_USE_FPCS_LIST0
                 | IS_STORE, "vpush", "<!0P>", 4, kFixupNone),
    ENCODING_MAP(kThumb2Vldms,        0xec900a00,
                 kFmtBitBlt, 19, 16, kFmtSfp, 22, 12, kFmtBitBlt, 7, 0,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_USE0 | REG_DEF_FPCS_LIST2
                 | IS_LOAD, "vldms", "!0C, <!2Q>", 4, kFixupNone),
    ENCODING_MAP(kThumb2Vstms,        0xec800a00,
                 kFmtBitBlt, 19, 16, kFmtSfp, 22, 12, kFmtBitBlt, 7, 0,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_USE0 | REG_USE_FPCS_LIST2
                 | IS_STORE, "vstms", "!0C, <!2Q>", 4, kFixupNone),
    ENCODING_MAP(kThumb2BUncond,      0xf0009000,
                 kFmtOff24, -1, -1, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, NO_OPERAND | IS_BRANCH,
                 "b", "!0t", 4, kFixupT2Branch),
    ENCODING_MAP(kThumb2MovImm16H,       0xf2c00000,
                 kFmtBitBlt, 11, 8, kFmtImm16, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0 | REG_USE0,
                 "movt", "!0C, #!1M", 4, kFixupNone),
    ENCODING_MAP(kThumb2AddPCR,      0x4487,
                 kFmtBitBlt, 6, 3, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_UNARY_OP | REG_USE0 | IS_BRANCH | NEEDS_FIXUP,
                 "add", "rPC, !0C", 2, kFixupLabel),
    ENCODING_MAP(kThumb2Adr,         0xf20f0000,
                 kFmtBitBlt, 11, 8, kFmtImm12, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 /* Note: doesn't affect flags */
                 IS_TERTIARY_OP | REG_DEF0 | NEEDS_FIXUP,
                 "adr", "!0C,#!1d", 4, kFixupAdr),
    ENCODING_MAP(kThumb2MovImm16LST,     0xf2400000,
                 kFmtBitBlt, 11, 8, kFmtImm16, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0 | NEEDS_FIXUP,
                 "mov", "!0C, #!1M", 4, kFixupMovImmLST),
    ENCODING_MAP(kThumb2MovImm16HST,     0xf2c00000,
                 kFmtBitBlt, 11, 8, kFmtImm16, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0 | REG_USE0 | NEEDS_FIXUP,
                 "movt", "!0C, #!1M", 4, kFixupMovImmHST),
    ENCODING_MAP(kThumb2LdmiaWB,         0xe8b00000,
                 kFmtBitBlt, 19, 16, kFmtBitBlt, 15, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE0 | REG_DEF_LIST1 | IS_LOAD,
                 "ldmia", "!0C!!, <!1R>", 4, kFixupNone),
    ENCODING_MAP(kThumb2OrrRRRs,  0xea500000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtShift, -1, -1, IS_QUAD_OP | REG_DEF0_USE12 | SETS_CCODES,
                 "orrs", "!0C, !1C, !2C!3H", 4, kFixupNone),
    ENCODING_MAP(kThumb2Push1,    0xf84d0d04,
                 kFmtBitBlt, 15, 12, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_UNARY_OP | REG_DEF_SP | REG_USE_SP | REG_USE0
                 | IS_STORE, "push1", "!0C", 4, kFixupNone),
    ENCODING_MAP(kThumb2Pop1,    0xf85d0b04,
                 kFmtBitBlt, 15, 12, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_UNARY_OP | REG_DEF_SP | REG_USE_SP | REG_DEF0
                 | IS_LOAD, "pop1", "!0C", 4, kFixupNone),
    ENCODING_MAP(kThumb2RsubRRR,  0xebd00000, /* setflags encoding */
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtShift, -1, -1,
                 IS_QUAD_OP | REG_DEF0_USE12 | SETS_CCODES,
                 "rsbs", "!0C, !1C, !2C!3H", 4, kFixupNone),
    ENCODING_MAP(kThumb2Smull,  0xfb800000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16,
                 kFmtBitBlt, 3, 0,
                 IS_QUAD_OP | REG_DEF0 | REG_DEF1 | REG_USE2 | REG_USE3,
                 "smull", "!0C, !1C, !2C, !3C", 4, kFixupNone),
    ENCODING_MAP(kThumb2LdrdPcRel8,  0xe9df0000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 11, 8, kFmtBitBlt, 7, 0,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_DEF0 | REG_DEF1 | REG_USE_PC | IS_LOAD | NEEDS_FIXUP,
                 "ldrd", "!0C, !1C, [pc, #!2E]", 4, kFixupLoad),
    ENCODING_MAP(kThumb2LdrdI8, 0xe9d00000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16,
                 kFmtBitBlt, 7, 0,
                 IS_QUAD_OP | REG_DEF0 | REG_DEF1 | REG_USE2 | IS_LOAD,
                 "ldrd", "!0C, !1C, [!2C, #!3E]", 4, kFixupNone),
    ENCODING_MAP(kThumb2StrdI8, 0xe9c00000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16,
                 kFmtBitBlt, 7, 0,
                 IS_QUAD_OP | REG_USE0 | REG_USE1 | REG_USE2 | IS_STORE,
                 "strd", "!0C, !1C, [!2C, #!3E]", 4, kFixupNone),
};

// new_lir replaces orig_lir in the pcrel_fixup list.
void ArmMir2Lir::ReplaceFixup(LIR* prev_lir, LIR* orig_lir, LIR* new_lir) {
  new_lir->u.a.pcrel_next = orig_lir->u.a.pcrel_next;
  if (UNLIKELY(prev_lir == NULL)) {
    first_fixup_ = new_lir;
  } else {
    prev_lir->u.a.pcrel_next = new_lir;
  }
  orig_lir->flags.fixup = kFixupNone;
}

// new_lir is inserted before orig_lir in the pcrel_fixup list.
void ArmMir2Lir::InsertFixupBefore(LIR* prev_lir, LIR* orig_lir, LIR* new_lir) {
  new_lir->u.a.pcrel_next = orig_lir;
  if (UNLIKELY(prev_lir == NULL)) {
    first_fixup_ = new_lir;
  } else {
    DCHECK(prev_lir->u.a.pcrel_next == orig_lir);
    prev_lir->u.a.pcrel_next = new_lir;
  }
}

/*
 * The fake NOP of moving r0 to r0 actually will incur data stalls if r0 is
 * not ready. Since r5FP is not updated often, it is less likely to
 * generate unnecessary stall cycles.
 * TUNING: No longer true - find new NOP pattern.
 */
#define PADDING_MOV_R5_R5               0x1C2D

uint8_t* ArmMir2Lir::EncodeLIRs(uint8_t* write_pos, LIR* lir) {
  for (; lir != NULL; lir = NEXT_LIR(lir)) {
    if (!lir->flags.is_nop) {
      int opcode = lir->opcode;
      if (IsPseudoLirOp(opcode)) {
        if (UNLIKELY(opcode == kPseudoPseudoAlign4)) {
          // Note: size for this opcode will be either 0 or 2 depending on final alignment.
          if (lir->offset & 0x2) {
            write_pos[0] = (PADDING_MOV_R5_R5 & 0xff);
            write_pos[1] = ((PADDING_MOV_R5_R5 >> 8) & 0xff);
            write_pos += 2;
          }
        }
      } else if (LIKELY(!lir->flags.is_nop)) {
        const ArmEncodingMap *encoder = &EncodingMap[lir->opcode];
        uint32_t bits = encoder->skeleton;
        for (int i = 0; i < 4; i++) {
          uint32_t operand;
          uint32_t value;
          operand = lir->operands[i];
          ArmEncodingKind kind = encoder->field_loc[i].kind;
          if (LIKELY(kind == kFmtBitBlt)) {
            value = (operand << encoder->field_loc[i].start) &
                ((1 << (encoder->field_loc[i].end + 1)) - 1);
            bits |= value;
          } else {
            switch (encoder->field_loc[i].kind) {
              case kFmtSkip:
                break;  // Nothing to do, but continue to next.
              case kFmtUnused:
                i = 4;  // Done, break out of the enclosing loop.
                break;
              case kFmtFPImm:
                value = ((operand & 0xF0) >> 4) << encoder->field_loc[i].end;
                value |= (operand & 0x0F) << encoder->field_loc[i].start;
                bits |= value;
                break;
              case kFmtBrOffset:
                value = ((operand  & 0x80000) >> 19) << 26;
                value |= ((operand & 0x40000) >> 18) << 11;
                value |= ((operand & 0x20000) >> 17) << 13;
                value |= ((operand & 0x1f800) >> 11) << 16;
                value |= (operand  & 0x007ff);
                bits |= value;
                break;
              case kFmtShift5:
                value = ((operand & 0x1c) >> 2) << 12;
                value |= (operand & 0x03) << 6;
                bits |= value;
                break;
              case kFmtShift:
                value = ((operand & 0x70) >> 4) << 12;
                value |= (operand & 0x0f) << 4;
                bits |= value;
                break;
              case kFmtBWidth:
                value = operand - 1;
                bits |= value;
                break;
              case kFmtLsb:
                value = ((operand & 0x1c) >> 2) << 12;
                value |= (operand & 0x03) << 6;
                bits |= value;
                break;
              case kFmtImm6:
                value = ((operand & 0x20) >> 5) << 9;
                value |= (operand & 0x1f) << 3;
                bits |= value;
                break;
              case kFmtDfp: {
                DCHECK(RegStorage::IsDouble(operand)) << ", Operand = 0x" << std::hex << operand;
                uint32_t reg_num = RegStorage::RegNum(operand);
                /* Snag the 1-bit slice and position it */
                value = ((reg_num & 0x10) >> 4) << encoder->field_loc[i].end;
                /* Extract and position the 4-bit slice */
                value |= (reg_num & 0x0f) << encoder->field_loc[i].start;
                bits |= value;
                break;
              }
              case kFmtSfp: {
                DCHECK(RegStorage::IsSingle(operand)) << ", Operand = 0x" << std::hex << operand;
                uint32_t reg_num = RegStorage::RegNum(operand);
                /* Snag the 1-bit slice and position it */
                value = (reg_num & 0x1) << encoder->field_loc[i].end;
                /* Extract and position the 4-bit slice */
                value |= ((reg_num & 0x1e) >> 1) << encoder->field_loc[i].start;
                bits |= value;
                break;
              }
              case kFmtImm12:
              case kFmtModImm:
                value = ((operand & 0x800) >> 11) << 26;
                value |= ((operand & 0x700) >> 8) << 12;
                value |= operand & 0x0ff;
                bits |= value;
                break;
              case kFmtImm16:
                value = ((operand & 0x0800) >> 11) << 26;
                value |= ((operand & 0xf000) >> 12) << 16;
                value |= ((operand & 0x0700) >> 8) << 12;
                value |= operand & 0x0ff;
                bits |= value;
                break;
              case kFmtOff24: {
                uint32_t signbit = (operand >> 31) & 0x1;
                uint32_t i1 = (operand >> 22) & 0x1;
                uint32_t i2 = (operand >> 21) & 0x1;
                uint32_t imm10 = (operand >> 11) & 0x03ff;
                uint32_t imm11 = operand & 0x07ff;
                uint32_t j1 = (i1 ^ signbit) ? 0 : 1;
                uint32_t j2 = (i2 ^ signbit) ? 0 : 1;
                value = (signbit << 26) | (j1 << 13) | (j2 << 11) | (imm10 << 16) |
                    imm11;
                bits |= value;
                }
                break;
              default:
                LOG(FATAL) << "Bad fmt:" << encoder->field_loc[i].kind;
            }
          }
        }
        if (encoder->size == 4) {
          write_pos[0] = ((bits >> 16) & 0xff);
          write_pos[1] = ((bits >> 24) & 0xff);
          write_pos[2] = (bits & 0xff);
          write_pos[3] = ((bits >> 8) & 0xff);
          write_pos += 4;
        } else {
          DCHECK_EQ(encoder->size, 2);
          write_pos[0] = (bits & 0xff);
          write_pos[1] = ((bits >> 8) & 0xff);
          write_pos += 2;
        }
      }
    }
  }
  return write_pos;
}

// Assemble the LIR into binary instruction format.
void ArmMir2Lir::AssembleLIR() {
  LIR* lir;
  LIR* prev_lir;
  cu_->NewTimingSplit("Assemble");
  int assembler_retries = 0;
  CodeOffset starting_offset = LinkFixupInsns(first_lir_insn_, last_lir_insn_, 0);
  data_offset_ = RoundUp(starting_offset, 4);
  int32_t offset_adjustment;
  AssignDataOffsets();

  /*
   * Note: generation must be 1 on first pass (to distinguish from initialized state of 0 for
   * non-visited nodes).  Start at zero here, and bit will be flipped to 1 on entry to the loop.
   */
  int generation = 0;
  while (true) {
    offset_adjustment = 0;
    AssemblerStatus res = kSuccess;  // Assume success
    generation ^= 1;
    // Note: nodes requring possible fixup linked in ascending order.
    lir = first_fixup_;
    prev_lir = NULL;
    while (lir != NULL) {
      /*
       * NOTE: the lir being considered here will be encoded following the switch (so long as
       * we're not in a retry situation).  However, any new non-pc_rel instructions inserted
       * due to retry must be explicitly encoded at the time of insertion.  Note that
       * inserted instructions don't need use/def flags, but do need size and pc-rel status
       * properly updated.
       */
      lir->offset += offset_adjustment;
      // During pass, allows us to tell whether a node has been updated with offset_adjustment yet.
      lir->flags.generation = generation;
      switch (static_cast<FixupKind>(lir->flags.fixup)) {
        case kFixupLabel:
        case kFixupNone:
          break;
        case kFixupVLoad:
          if (lir->operands[1] != rs_r15pc.GetReg()) {
            break;
          }
          // NOTE: intentional fallthrough.
        case kFixupLoad: {
          /*
           * PC-relative loads are mostly used to load immediates
           * that are too large to materialize directly in one shot.
           * However, if the load displacement exceeds the limit,
           * we revert to a multiple-instruction materialization sequence.
           */
          LIR *lir_target = lir->target;
          CodeOffset pc = (lir->offset + 4) & ~3;
          CodeOffset target = lir_target->offset +
              ((lir_target->flags.generation == lir->flags.generation) ? 0 : offset_adjustment);
          int32_t delta = target - pc;
          if (res != kSuccess) {
            /*
             * In this case, we're just estimating and will do it again for real.  Ensure offset
             * is legal.
             */
            delta &= ~0x3;
          }
          DCHECK_EQ((delta & 0x3), 0);
          // First, a sanity check for cases we shouldn't see now
          if (kIsDebugBuild && (((lir->opcode == kThumbAddPcRel) && (delta > 1020)) ||
              ((lir->opcode == kThumbLdrPcRel) && (delta > 1020)))) {
            // Shouldn't happen in current codegen.
            LOG(FATAL) << "Unexpected pc-rel offset " << delta;
          }
          // Now, check for the difficult cases
          if (((lir->opcode == kThumb2LdrPcRel12) && (delta > 4091)) ||
              ((lir->opcode == kThumb2LdrdPcRel8) && (delta > 1020)) ||
              ((lir->opcode == kThumb2Vldrs) && (delta > 1020)) ||
              ((lir->opcode == kThumb2Vldrd) && (delta > 1020))) {
            /*
             * Note: The reason vldrs/vldrd include rARM_LR in their use/def masks is that we
             * sometimes have to use it to fix up out-of-range accesses.  This is where that
             * happens.
             */
            int base_reg = ((lir->opcode == kThumb2LdrdPcRel8) ||
                            (lir->opcode == kThumb2LdrPcRel12)) ?  lir->operands[0] :
                            rs_rARM_LR.GetReg();

            // Add new Adr to generate the address.
            LIR* new_adr = RawLIR(lir->dalvik_offset, kThumb2Adr,
                       base_reg, 0, 0, 0, 0, lir->target);
            new_adr->offset = lir->offset;
            new_adr->flags.fixup = kFixupAdr;
            new_adr->flags.size = EncodingMap[kThumb2Adr].size;
            InsertLIRBefore(lir, new_adr);
            lir->offset += new_adr->flags.size;
            offset_adjustment += new_adr->flags.size;

            // lir no longer pcrel, unlink and link in new_adr.
            ReplaceFixup(prev_lir, lir, new_adr);

            // Convert to normal load.
            offset_adjustment -= lir->flags.size;
            if (lir->opcode == kThumb2LdrPcRel12) {
              lir->opcode = kThumb2LdrRRI12;
            } else if (lir->opcode == kThumb2LdrdPcRel8) {
              lir->opcode = kThumb2LdrdI8;
            }
            lir->flags.size = EncodingMap[lir->opcode].size;
            offset_adjustment += lir->flags.size;
            // Change the load to be relative to the new Adr base.
            if (lir->opcode == kThumb2LdrdI8) {
              lir->operands[3] = 0;
              lir->operands[2] = base_reg;
            } else {
              lir->operands[2] = 0;
              lir->operands[1] = base_reg;
            }
            prev_lir = new_adr;  // Continue scan with new_adr;
            lir = new_adr->u.a.pcrel_next;
            res = kRetryAll;
            continue;
          } else {
            if ((lir->opcode == kThumb2Vldrs) ||
                (lir->opcode == kThumb2Vldrd) ||
                (lir->opcode == kThumb2LdrdPcRel8)) {
              lir->operands[2] = delta >> 2;
            } else {
              lir->operands[1] = (lir->opcode == kThumb2LdrPcRel12) ?  delta :
                  delta >> 2;
            }
          }
          break;
        }
        case kFixupCBxZ: {
          LIR *target_lir = lir->target;
          CodeOffset pc = lir->offset + 4;
          CodeOffset target = target_lir->offset +
              ((target_lir->flags.generation == lir->flags.generation) ? 0 : offset_adjustment);
          int32_t delta = target - pc;
          if (delta > 126 || delta < 0) {
            /*
             * Convert to cmp rx,#0 / b[eq/ne] tgt pair
             * Make new branch instruction and insert after
             */
            LIR* new_inst =
              RawLIR(lir->dalvik_offset, kThumbBCond, 0,
                     (lir->opcode == kThumb2Cbz) ? kArmCondEq : kArmCondNe,
                     0, 0, 0, lir->target);
            InsertLIRAfter(lir, new_inst);

            /* Convert the cb[n]z to a cmp rx, #0 ] */
            // Subtract the old size.
            offset_adjustment -= lir->flags.size;
            lir->opcode = kThumbCmpRI8;
            /* operand[0] is src1 in both cb[n]z & CmpRI8 */
            lir->operands[1] = 0;
            lir->target = 0;
            lir->flags.size = EncodingMap[lir->opcode].size;
            // Add back the new size.
            offset_adjustment += lir->flags.size;
            // Set up the new following inst.
            new_inst->offset = lir->offset + lir->flags.size;
            new_inst->flags.fixup = kFixupCondBranch;
            new_inst->flags.size = EncodingMap[new_inst->opcode].size;
            offset_adjustment += new_inst->flags.size;

            // lir no longer pcrel, unlink and link in new_inst.
            ReplaceFixup(prev_lir, lir, new_inst);
            prev_lir = new_inst;  // Continue with the new instruction.
            lir = new_inst->u.a.pcrel_next;
            res = kRetryAll;
            continue;
          } else {
            lir->operands[1] = delta >> 1;
          }
          break;
        }
        case kFixupPushPop: {
          if (__builtin_popcount(lir->operands[0]) == 1) {
            /*
             * The standard push/pop multiple instruction
             * requires at least two registers in the list.
             * If we've got just one, switch to the single-reg
             * encoding.
             */
            lir->opcode = (lir->opcode == kThumb2Push) ? kThumb2Push1 :
                kThumb2Pop1;
            int reg = 0;
            while (lir->operands[0]) {
              if (lir->operands[0] & 0x1) {
                break;
              } else {
                reg++;
                lir->operands[0] >>= 1;
              }
            }
            lir->operands[0] = reg;
            // This won't change again, don't bother unlinking, just reset fixup kind
            lir->flags.fixup = kFixupNone;
          }
          break;
        }
        case kFixupCondBranch: {
          LIR *target_lir = lir->target;
          int32_t delta = 0;
          DCHECK(target_lir);
          CodeOffset pc = lir->offset + 4;
          CodeOffset target = target_lir->offset +
              ((target_lir->flags.generation == lir->flags.generation) ? 0 : offset_adjustment);
          delta = target - pc;
          if ((lir->opcode == kThumbBCond) && (delta > 254 || delta < -256)) {
            offset_adjustment -= lir->flags.size;
            lir->opcode = kThumb2BCond;
            lir->flags.size = EncodingMap[lir->opcode].size;
            // Fixup kind remains the same.
            offset_adjustment += lir->flags.size;
            res = kRetryAll;
          }
          lir->operands[0] = delta >> 1;
          break;
        }
        case kFixupT2Branch: {
          LIR *target_lir = lir->target;
          CodeOffset pc = lir->offset + 4;
          CodeOffset target = target_lir->offset +
              ((target_lir->flags.generation == lir->flags.generation) ? 0 : offset_adjustment);
          int32_t delta = target - pc;
          lir->operands[0] = delta >> 1;
          if (!(cu_->disable_opt & (1 << kSafeOptimizations)) && lir->operands[0] == 0) {
            // Useless branch
            offset_adjustment -= lir->flags.size;
            lir->flags.is_nop = true;
            // Don't unlink - just set to do-nothing.
            lir->flags.fixup = kFixupNone;
            res = kRetryAll;
          }
          break;
        }
        case kFixupT1Branch: {
          LIR *target_lir = lir->target;
          CodeOffset pc = lir->offset + 4;
          CodeOffset target = target_lir->offset +
              ((target_lir->flags.generation == lir->flags.generation) ? 0 : offset_adjustment);
          int32_t delta = target - pc;
          if (delta > 2046 || delta < -2048) {
            // Convert to Thumb2BCond w/ kArmCondAl
            offset_adjustment -= lir->flags.size;
            lir->opcode = kThumb2BUncond;
            lir->operands[0] = 0;
            lir->flags.size = EncodingMap[lir->opcode].size;
            lir->flags.fixup = kFixupT2Branch;
            offset_adjustment += lir->flags.size;
            res = kRetryAll;
          } else {
            lir->operands[0] = delta >> 1;
            if (!(cu_->disable_opt & (1 << kSafeOptimizations)) && lir->operands[0] == -1) {
              // Useless branch
              offset_adjustment -= lir->flags.size;
              lir->flags.is_nop = true;
              // Don't unlink - just set to do-nothing.
              lir->flags.fixup = kFixupNone;
              res = kRetryAll;
            }
          }
          break;
        }
        case kFixupBlx1: {
          DCHECK(NEXT_LIR(lir)->opcode == kThumbBlx2);
          /* cur_pc is Thumb */
          CodeOffset cur_pc = (lir->offset + 4) & ~3;
          CodeOffset target = lir->operands[1];

          /* Match bit[1] in target with base */
          if (cur_pc & 0x2) {
            target |= 0x2;
          }
          int32_t delta = target - cur_pc;
          DCHECK((delta >= -(1<<22)) && (delta <= ((1<<22)-2)));

          lir->operands[0] = (delta >> 12) & 0x7ff;
          NEXT_LIR(lir)->operands[0] = (delta>> 1) & 0x7ff;
          break;
        }
        case kFixupBl1: {
          DCHECK(NEXT_LIR(lir)->opcode == kThumbBl2);
          /* Both cur_pc and target are Thumb */
          CodeOffset cur_pc = lir->offset + 4;
          CodeOffset target = lir->operands[1];

          int32_t delta = target - cur_pc;
          DCHECK((delta >= -(1<<22)) && (delta <= ((1<<22)-2)));

          lir->operands[0] = (delta >> 12) & 0x7ff;
          NEXT_LIR(lir)->operands[0] = (delta>> 1) & 0x7ff;
          break;
        }
        case kFixupAdr: {
          EmbeddedData *tab_rec = reinterpret_cast<EmbeddedData*>(UnwrapPointer(lir->operands[2]));
          LIR* target = lir->target;
          int32_t target_disp = (tab_rec != NULL) ?  tab_rec->offset + offset_adjustment
              : target->offset + ((target->flags.generation == lir->flags.generation) ? 0 :
              offset_adjustment);
          int32_t disp = target_disp - ((lir->offset + 4) & ~3);
          if (disp < 4096) {
            lir->operands[1] = disp;
          } else {
            // convert to ldimm16l, ldimm16h, add tgt, pc, operands[0]
            // TUNING: if this case fires often, it can be improved.  Not expected to be common.
            LIR *new_mov16L =
                RawLIR(lir->dalvik_offset, kThumb2MovImm16LST, lir->operands[0], 0,
                       WrapPointer(lir), WrapPointer(tab_rec), 0, lir->target);
            new_mov16L->flags.size = EncodingMap[new_mov16L->opcode].size;
            new_mov16L->flags.fixup = kFixupMovImmLST;
            new_mov16L->offset = lir->offset;
            // Link the new instruction, retaining lir.
            InsertLIRBefore(lir, new_mov16L);
            lir->offset += new_mov16L->flags.size;
            offset_adjustment += new_mov16L->flags.size;
            InsertFixupBefore(prev_lir, lir, new_mov16L);
            prev_lir = new_mov16L;   // Now we've got a new prev.
            LIR *new_mov16H =
                RawLIR(lir->dalvik_offset, kThumb2MovImm16HST, lir->operands[0], 0,
                       WrapPointer(lir), WrapPointer(tab_rec), 0, lir->target);
            new_mov16H->flags.size = EncodingMap[new_mov16H->opcode].size;
            new_mov16H->flags.fixup = kFixupMovImmHST;
            new_mov16H->offset = lir->offset;
            // Link the new instruction, retaining lir.
            InsertLIRBefore(lir, new_mov16H);
            lir->offset += new_mov16H->flags.size;
            offset_adjustment += new_mov16H->flags.size;
            InsertFixupBefore(prev_lir, lir, new_mov16H);
            prev_lir = new_mov16H;  // Now we've got a new prev.

            offset_adjustment -= lir->flags.size;
            if (RegStorage::RegNum(lir->operands[0]) < 8) {
              lir->opcode = kThumbAddRRLH;
            } else {
              lir->opcode = kThumbAddRRHH;
            }
            lir->operands[1] = rs_rARM_PC.GetReg();
            lir->flags.size = EncodingMap[lir->opcode].size;
            offset_adjustment += lir->flags.size;
            // Must stay in fixup list and have offset updated; will be used by LST/HSP pair.
            lir->flags.fixup = kFixupNone;
            res = kRetryAll;
          }
          break;
        }
        case kFixupMovImmLST: {
          // operands[1] should hold disp, [2] has add, [3] has tab_rec
          LIR *addPCInst = reinterpret_cast<LIR*>(UnwrapPointer(lir->operands[2]));
          EmbeddedData *tab_rec = reinterpret_cast<EmbeddedData*>(UnwrapPointer(lir->operands[3]));
          // If tab_rec is null, this is a literal load. Use target
          LIR* target = lir->target;
          int32_t target_disp = tab_rec ? tab_rec->offset : target->offset;
          lir->operands[1] = (target_disp - (addPCInst->offset + 4)) & 0xffff;
          break;
        }
        case kFixupMovImmHST: {
          // operands[1] should hold disp, [2] has add, [3] has tab_rec
          LIR *addPCInst = reinterpret_cast<LIR*>(UnwrapPointer(lir->operands[2]));
          EmbeddedData *tab_rec = reinterpret_cast<EmbeddedData*>(UnwrapPointer(lir->operands[3]));
          // If tab_rec is null, this is a literal load. Use target
          LIR* target = lir->target;
          int32_t target_disp = tab_rec ? tab_rec->offset : target->offset;
          lir->operands[1] =
              ((target_disp - (addPCInst->offset + 4)) >> 16) & 0xffff;
          break;
        }
        case kFixupAlign4: {
          int32_t required_size = lir->offset & 0x2;
          if (lir->flags.size != required_size) {
            offset_adjustment += required_size - lir->flags.size;
            lir->flags.size = required_size;
            res = kRetryAll;
          }
          break;
        }
        default:
          LOG(FATAL) << "Unexpected case " << lir->flags.fixup;
      }
      prev_lir = lir;
      lir = lir->u.a.pcrel_next;
    }

    if (res == kSuccess) {
      break;
    } else {
      assembler_retries++;
      if (assembler_retries > MAX_ASSEMBLER_RETRIES) {
        CodegenDump();
        LOG(FATAL) << "Assembler error - too many retries";
      }
      starting_offset += offset_adjustment;
      data_offset_ = RoundUp(starting_offset, 4);
      AssignDataOffsets();
    }
  }

  // Build the CodeBuffer.
  DCHECK_LE(data_offset_, total_size_);
  code_buffer_.reserve(total_size_);
  code_buffer_.resize(starting_offset);
  uint8_t* write_pos = &code_buffer_[0];
  write_pos = EncodeLIRs(write_pos, first_lir_insn_);
  DCHECK_EQ(static_cast<CodeOffset>(write_pos - &code_buffer_[0]), starting_offset);

  DCHECK_EQ(data_offset_, RoundUp(code_buffer_.size(), 4));

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

int ArmMir2Lir::GetInsnSize(LIR* lir) {
  DCHECK(!IsPseudoLirOp(lir->opcode));
  return EncodingMap[lir->opcode].size;
}

// Encode instruction bit pattern and assign offsets.
uint32_t ArmMir2Lir::LinkFixupInsns(LIR* head_lir, LIR* tail_lir, uint32_t offset) {
  LIR* end_lir = tail_lir->next;

  LIR* last_fixup = NULL;
  for (LIR* lir = head_lir; lir != end_lir; lir = NEXT_LIR(lir)) {
    if (!lir->flags.is_nop) {
      if (lir->flags.fixup != kFixupNone) {
        if (!IsPseudoLirOp(lir->opcode)) {
          lir->flags.size = EncodingMap[lir->opcode].size;
          lir->flags.fixup = EncodingMap[lir->opcode].fixup;
        } else if (UNLIKELY(lir->opcode == kPseudoPseudoAlign4)) {
          lir->flags.size = (offset & 0x2);
          lir->flags.fixup = kFixupAlign4;
        } else {
          lir->flags.size = 0;
          lir->flags.fixup = kFixupLabel;
        }
        // Link into the fixup chain.
        lir->flags.use_def_invalid = true;
        lir->u.a.pcrel_next = NULL;
        if (first_fixup_ == NULL) {
          first_fixup_ = lir;
        } else {
          last_fixup->u.a.pcrel_next = lir;
        }
        last_fixup = lir;
        lir->offset = offset;
      }
      offset += lir->flags.size;
    }
  }
  return offset;
}

void ArmMir2Lir::AssignDataOffsets() {
  /* Set up offsets for literals */
  CodeOffset offset = data_offset_;

  offset = AssignLiteralOffset(offset);

  offset = AssignSwitchTablesOffset(offset);

  total_size_ = AssignFillArrayDataOffset(offset);
}

}  // namespace art
