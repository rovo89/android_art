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

#include "../../Dalvik.h"
#include "../../CompilerInternals.h"
#include "ArmLIR.h"
#include "Codegen.h"
#include <sys/mman.h>           /* for protection change */

//#define TESTMODE
#ifdef TESTMODE
#include <cutils/ashmem.h>      /* for oat testing */
#include <unistd.h>
#endif

#define MAX_ASSEMBLER_RETRIES 50

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
/* NOTE: must be kept in sync with enum ArmOpcode from ArmLIR.h */
ArmEncodingMap EncodingMap[kArmLast] = {
    ENCODING_MAP(kArm16BitData,    0x0000,
                 kFmtBitBlt, 15, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP, "data", "0x!0h(!0d)", 1),
    ENCODING_MAP(kThumbAdcRR,        0x4140,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE01 | SETS_CCODES | USES_CCODES,
                 "adcs", "!0C, !1C", 1),
    ENCODING_MAP(kThumbAddRRI3,      0x1c00,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 8, 6,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_DEF0_USE1 | SETS_CCODES,
                 "adds", "!0C, !1C, #!2d", 1),
    ENCODING_MAP(kThumbAddRI8,       0x3000,
                 kFmtBitBlt, 10, 8, kFmtBitBlt, 7, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE0 | SETS_CCODES,
                 "adds", "!0C, !0C, #!1d", 1),
    ENCODING_MAP(kThumbAddRRR,       0x1800,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 8, 6,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_DEF0_USE12 | SETS_CCODES,
                 "adds", "!0C, !1C, !2C", 1),
    ENCODING_MAP(kThumbAddRRLH,     0x4440,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE01,
                 "add", "!0C, !1C", 1),
    ENCODING_MAP(kThumbAddRRHL,     0x4480,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE01,
                 "add", "!0C, !1C", 1),
    ENCODING_MAP(kThumbAddRRHH,     0x44c0,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE01,
                 "add", "!0C, !1C", 1),
    ENCODING_MAP(kThumbAddPcRel,    0xa000,
                 kFmtBitBlt, 10, 8, kFmtBitBlt, 7, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | IS_BRANCH,
                 "add", "!0C, pc, #!1E", 1),
    ENCODING_MAP(kThumbAddSpRel,    0xa800,
                 kFmtBitBlt, 10, 8, kFmtUnused, -1, -1, kFmtBitBlt, 7, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF_SP | REG_USE_SP,
                 "add", "!0C, sp, #!2E", 1),
    ENCODING_MAP(kThumbAddSpI7,      0xb000,
                 kFmtBitBlt, 6, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP | REG_DEF_SP | REG_USE_SP,
                 "add", "sp, #!0d*4", 1),
    ENCODING_MAP(kThumbAndRR,        0x4000,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE01 | SETS_CCODES,
                 "ands", "!0C, !1C", 1),
    ENCODING_MAP(kThumbAsrRRI5,      0x1000,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 10, 6,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_DEF0_USE1 | SETS_CCODES,
                 "asrs", "!0C, !1C, #!2d", 1),
    ENCODING_MAP(kThumbAsrRR,        0x4100,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE01 | SETS_CCODES,
                 "asrs", "!0C, !1C", 1),
    ENCODING_MAP(kThumbBCond,        0xd000,
                 kFmtBitBlt, 7, 0, kFmtBitBlt, 11, 8, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | IS_BRANCH | USES_CCODES,
                 "b!1c", "!0t", 1),
    ENCODING_MAP(kThumbBUncond,      0xe000,
                 kFmtBitBlt, 10, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, NO_OPERAND | IS_BRANCH,
                 "b", "!0t", 1),
    ENCODING_MAP(kThumbBicRR,        0x4380,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE01 | SETS_CCODES,
                 "bics", "!0C, !1C", 1),
    ENCODING_MAP(kThumbBkpt,          0xbe00,
                 kFmtBitBlt, 7, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP | IS_BRANCH,
                 "bkpt", "!0d", 1),
    ENCODING_MAP(kThumbBlx1,         0xf000,
                 kFmtBitBlt, 10, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | IS_BRANCH | REG_DEF_LR,
                 "blx_1", "!0u", 1),
    ENCODING_MAP(kThumbBlx2,         0xe800,
                 kFmtBitBlt, 10, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | IS_BRANCH | REG_DEF_LR,
                 "blx_2", "!0v", 1),
    ENCODING_MAP(kThumbBl1,          0xf000,
                 kFmtBitBlt, 10, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP | IS_BRANCH | REG_DEF_LR,
                 "bl_1", "!0u", 1),
    ENCODING_MAP(kThumbBl2,          0xf800,
                 kFmtBitBlt, 10, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP | IS_BRANCH | REG_DEF_LR,
                 "bl_2", "!0v", 1),
    ENCODING_MAP(kThumbBlxR,         0x4780,
                 kFmtBitBlt, 6, 3, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_UNARY_OP | REG_USE0 | IS_BRANCH | REG_DEF_LR,
                 "blx", "!0C", 1),
    ENCODING_MAP(kThumbBx,            0x4700,
                 kFmtBitBlt, 6, 3, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP | IS_BRANCH,
                 "bx", "!0C", 1),
    ENCODING_MAP(kThumbCmnRR,        0x42c0,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_USE01 | SETS_CCODES,
                 "cmn", "!0C, !1C", 1),
    ENCODING_MAP(kThumbCmpRI8,       0x2800,
                 kFmtBitBlt, 10, 8, kFmtBitBlt, 7, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_USE0 | SETS_CCODES,
                 "cmp", "!0C, #!1d", 1),
    ENCODING_MAP(kThumbCmpRR,        0x4280,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_USE01 | SETS_CCODES,
                 "cmp", "!0C, !1C", 1),
    ENCODING_MAP(kThumbCmpLH,        0x4540,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_USE01 | SETS_CCODES,
                 "cmp", "!0C, !1C", 1),
    ENCODING_MAP(kThumbCmpHL,        0x4580,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_USE01 | SETS_CCODES,
                 "cmp", "!0C, !1C", 1),
    ENCODING_MAP(kThumbCmpHH,        0x45c0,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_USE01 | SETS_CCODES,
                 "cmp", "!0C, !1C", 1),
    ENCODING_MAP(kThumbEorRR,        0x4040,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE01 | SETS_CCODES,
                 "eors", "!0C, !1C", 1),
    ENCODING_MAP(kThumbLdmia,         0xc800,
                 kFmtBitBlt, 10, 8, kFmtBitBlt, 7, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE0 | REG_DEF_LIST1 | IS_LOAD,
                 "ldmia", "!0C!!, <!1R>", 1),
    ENCODING_MAP(kThumbLdrRRI5,      0x6800,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 10, 6,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1 | IS_LOAD,
                 "ldr", "!0C, [!1C, #!2E]", 1),
    ENCODING_MAP(kThumbLdrRRR,       0x5800,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 8, 6,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12 | IS_LOAD,
                 "ldr", "!0C, [!1C, !2C]", 1),
    ENCODING_MAP(kThumbLdrPcRel,    0x4800,
                 kFmtBitBlt, 10, 8, kFmtBitBlt, 7, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0 | REG_USE_PC
                 | IS_LOAD, "ldr", "!0C, [pc, #!1E]", 1),
    ENCODING_MAP(kThumbLdrSpRel,    0x9800,
                 kFmtBitBlt, 10, 8, kFmtUnused, -1, -1, kFmtBitBlt, 7, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0 | REG_USE_SP
                 | IS_LOAD, "ldr", "!0C, [sp, #!2E]", 1),
    ENCODING_MAP(kThumbLdrbRRI5,     0x7800,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 10, 6,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1 | IS_LOAD,
                 "ldrb", "!0C, [!1C, #2d]", 1),
    ENCODING_MAP(kThumbLdrbRRR,      0x5c00,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 8, 6,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12 | IS_LOAD,
                 "ldrb", "!0C, [!1C, !2C]", 1),
    ENCODING_MAP(kThumbLdrhRRI5,     0x8800,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 10, 6,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1 | IS_LOAD,
                 "ldrh", "!0C, [!1C, #!2F]", 1),
    ENCODING_MAP(kThumbLdrhRRR,      0x5a00,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 8, 6,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12 | IS_LOAD,
                 "ldrh", "!0C, [!1C, !2C]", 1),
    ENCODING_MAP(kThumbLdrsbRRR,     0x5600,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 8, 6,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12 | IS_LOAD,
                 "ldrsb", "!0C, [!1C, !2C]", 1),
    ENCODING_MAP(kThumbLdrshRRR,     0x5e00,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 8, 6,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12 | IS_LOAD,
                 "ldrsh", "!0C, [!1C, !2C]", 1),
    ENCODING_MAP(kThumbLslRRI5,      0x0000,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 10, 6,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_DEF0_USE1 | SETS_CCODES,
                 "lsls", "!0C, !1C, #!2d", 1),
    ENCODING_MAP(kThumbLslRR,        0x4080,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE01 | SETS_CCODES,
                 "lsls", "!0C, !1C", 1),
    ENCODING_MAP(kThumbLsrRRI5,      0x0800,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 10, 6,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_DEF0_USE1 | SETS_CCODES,
                 "lsrs", "!0C, !1C, #!2d", 1),
    ENCODING_MAP(kThumbLsrRR,        0x40c0,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE01 | SETS_CCODES,
                 "lsrs", "!0C, !1C", 1),
    ENCODING_MAP(kThumbMovImm,       0x2000,
                 kFmtBitBlt, 10, 8, kFmtBitBlt, 7, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0 | SETS_CCODES,
                 "movs", "!0C, #!1d", 1),
    ENCODING_MAP(kThumbMovRR,        0x1c00,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE1 | SETS_CCODES,
                 "movs", "!0C, !1C", 1),
    ENCODING_MAP(kThumbMovRR_H2H,    0x46c0,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "mov", "!0C, !1C", 1),
    ENCODING_MAP(kThumbMovRR_H2L,    0x4640,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "mov", "!0C, !1C", 1),
    ENCODING_MAP(kThumbMovRR_L2H,    0x4680,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "mov", "!0C, !1C", 1),
    ENCODING_MAP(kThumbMul,           0x4340,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE01 | SETS_CCODES,
                 "muls", "!0C, !1C", 1),
    ENCODING_MAP(kThumbMvn,           0x43c0,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE1 | SETS_CCODES,
                 "mvns", "!0C, !1C", 1),
    ENCODING_MAP(kThumbNeg,           0x4240,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE1 | SETS_CCODES,
                 "negs", "!0C, !1C", 1),
    ENCODING_MAP(kThumbOrr,           0x4300,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE01 | SETS_CCODES,
                 "orrs", "!0C, !1C", 1),
    ENCODING_MAP(kThumbPop,           0xbc00,
                 kFmtBitBlt, 8, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_UNARY_OP | REG_DEF_SP | REG_USE_SP | REG_DEF_LIST0
                 | IS_LOAD, "pop", "<!0R>", 1),
    ENCODING_MAP(kThumbPush,          0xb400,
                 kFmtBitBlt, 8, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_UNARY_OP | REG_DEF_SP | REG_USE_SP | REG_USE_LIST0
                 | IS_STORE, "push", "<!0R>", 1),
    ENCODING_MAP(kThumbRorRR,        0x41c0,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE01 | SETS_CCODES,
                 "rors", "!0C, !1C", 1),
    ENCODING_MAP(kThumbSbc,           0x4180,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE01 | USES_CCODES | SETS_CCODES,
                 "sbcs", "!0C, !1C", 1),
    ENCODING_MAP(kThumbStmia,         0xc000,
                 kFmtBitBlt, 10, 8, kFmtBitBlt, 7, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0 | REG_USE0 | REG_USE_LIST1 | IS_STORE,
                 "stmia", "!0C!!, <!1R>", 1),
    ENCODING_MAP(kThumbStrRRI5,      0x6000,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 10, 6,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE01 | IS_STORE,
                 "str", "!0C, [!1C, #!2E]", 1),
    ENCODING_MAP(kThumbStrRRR,       0x5000,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 8, 6,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE012 | IS_STORE,
                 "str", "!0C, [!1C, !2C]", 1),
    ENCODING_MAP(kThumbStrSpRel,    0x9000,
                 kFmtBitBlt, 10, 8, kFmtUnused, -1, -1, kFmtBitBlt, 7, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE0 | REG_USE_SP
                 | IS_STORE, "str", "!0C, [sp, #!2E]", 1),
    ENCODING_MAP(kThumbStrbRRI5,     0x7000,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 10, 6,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE01 | IS_STORE,
                 "strb", "!0C, [!1C, #!2d]", 1),
    ENCODING_MAP(kThumbStrbRRR,      0x5400,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 8, 6,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE012 | IS_STORE,
                 "strb", "!0C, [!1C, !2C]", 1),
    ENCODING_MAP(kThumbStrhRRI5,     0x8000,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 10, 6,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE01 | IS_STORE,
                 "strh", "!0C, [!1C, #!2F]", 1),
    ENCODING_MAP(kThumbStrhRRR,      0x5200,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 8, 6,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE012 | IS_STORE,
                 "strh", "!0C, [!1C, !2C]", 1),
    ENCODING_MAP(kThumbSubRRI3,      0x1e00,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 8, 6,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_DEF0_USE1 | SETS_CCODES,
                 "subs", "!0C, !1C, #!2d", 1),
    ENCODING_MAP(kThumbSubRI8,       0x3800,
                 kFmtBitBlt, 10, 8, kFmtBitBlt, 7, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE0 | SETS_CCODES,
                 "subs", "!0C, #!1d", 1),
    ENCODING_MAP(kThumbSubRRR,       0x1a00,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtBitBlt, 8, 6,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_DEF0_USE12 | SETS_CCODES,
                 "subs", "!0C, !1C, !2C", 1),
    ENCODING_MAP(kThumbSubSpI7,      0xb080,
                 kFmtBitBlt, 6, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_UNARY_OP | REG_DEF_SP | REG_USE_SP,
                 "sub", "sp, #!0d*4", 1),
    ENCODING_MAP(kThumbSwi,           0xdf00,
                 kFmtBitBlt, 7, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP | IS_BRANCH,
                 "swi", "!0d", 1),
    ENCODING_MAP(kThumbTst,           0x4200,
                 kFmtBitBlt, 2, 0, kFmtBitBlt, 5, 3, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP | REG_USE01 | SETS_CCODES,
                 "tst", "!0C, !1C", 1),
    ENCODING_MAP(kThumb2Vldrs,       0xed900a00,
                 kFmtSfp, 22, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 7, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1 | IS_LOAD,
                 "vldr", "!0s, [!1C, #!2E]", 2),
    ENCODING_MAP(kThumb2Vldrd,       0xed900b00,
                 kFmtDfp, 22, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 7, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1 | IS_LOAD,
                 "vldr", "!0S, [!1C, #!2E]", 2),
    ENCODING_MAP(kThumb2Vmuls,        0xee200a00,
                 kFmtSfp, 22, 12, kFmtSfp, 7, 16, kFmtSfp, 5, 0,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_DEF0_USE12,
                 "vmuls", "!0s, !1s, !2s", 2),
    ENCODING_MAP(kThumb2Vmuld,        0xee200b00,
                 kFmtDfp, 22, 12, kFmtDfp, 7, 16, kFmtDfp, 5, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "vmuld", "!0S, !1S, !2S", 2),
    ENCODING_MAP(kThumb2Vstrs,       0xed800a00,
                 kFmtSfp, 22, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 7, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE01 | IS_STORE,
                 "vstr", "!0s, [!1C, #!2E]", 2),
    ENCODING_MAP(kThumb2Vstrd,       0xed800b00,
                 kFmtDfp, 22, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 7, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE01 | IS_STORE,
                 "vstr", "!0S, [!1C, #!2E]", 2),
    ENCODING_MAP(kThumb2Vsubs,        0xee300a40,
                 kFmtSfp, 22, 12, kFmtSfp, 7, 16, kFmtSfp, 5, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "vsub", "!0s, !1s, !2s", 2),
    ENCODING_MAP(kThumb2Vsubd,        0xee300b40,
                 kFmtDfp, 22, 12, kFmtDfp, 7, 16, kFmtDfp, 5, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "vsub", "!0S, !1S, !2S", 2),
    ENCODING_MAP(kThumb2Vadds,        0xee300a00,
                 kFmtSfp, 22, 12, kFmtSfp, 7, 16, kFmtSfp, 5, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "vadd", "!0s, !1s, !2s", 2),
    ENCODING_MAP(kThumb2Vaddd,        0xee300b00,
                 kFmtDfp, 22, 12, kFmtDfp, 7, 16, kFmtDfp, 5, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "vadd", "!0S, !1S, !2S", 2),
    ENCODING_MAP(kThumb2Vdivs,        0xee800a00,
                 kFmtSfp, 22, 12, kFmtSfp, 7, 16, kFmtSfp, 5, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "vdivs", "!0s, !1s, !2s", 2),
    ENCODING_MAP(kThumb2Vdivd,        0xee800b00,
                 kFmtDfp, 22, 12, kFmtDfp, 7, 16, kFmtDfp, 5, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "vdivd", "!0S, !1S, !2S", 2),
    ENCODING_MAP(kThumb2VcvtIF,       0xeeb80ac0,
                 kFmtSfp, 22, 12, kFmtSfp, 5, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "vcvt.f32", "!0s, !1s", 2),
    ENCODING_MAP(kThumb2VcvtID,       0xeeb80bc0,
                 kFmtDfp, 22, 12, kFmtSfp, 5, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "vcvt.f64", "!0S, !1s", 2),
    ENCODING_MAP(kThumb2VcvtFI,       0xeebd0ac0,
                 kFmtSfp, 22, 12, kFmtSfp, 5, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "vcvt.s32.f32 ", "!0s, !1s", 2),
    ENCODING_MAP(kThumb2VcvtDI,       0xeebd0bc0,
                 kFmtSfp, 22, 12, kFmtDfp, 5, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "vcvt.s32.f64 ", "!0s, !1S", 2),
    ENCODING_MAP(kThumb2VcvtFd,       0xeeb70ac0,
                 kFmtDfp, 22, 12, kFmtSfp, 5, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "vcvt.f64.f32 ", "!0S, !1s", 2),
    ENCODING_MAP(kThumb2VcvtDF,       0xeeb70bc0,
                 kFmtSfp, 22, 12, kFmtDfp, 5, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "vcvt.f32.f64 ", "!0s, !1S", 2),
    ENCODING_MAP(kThumb2Vsqrts,       0xeeb10ac0,
                 kFmtSfp, 22, 12, kFmtSfp, 5, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "vsqrt.f32 ", "!0s, !1s", 2),
    ENCODING_MAP(kThumb2Vsqrtd,       0xeeb10bc0,
                 kFmtDfp, 22, 12, kFmtDfp, 5, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "vsqrt.f64 ", "!0S, !1S", 2),
    ENCODING_MAP(kThumb2MovImmShift, 0xf04f0000, /* no setflags encoding */
                 kFmtBitBlt, 11, 8, kFmtModImm, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0,
                 "mov", "!0C, #!1m", 2),
    ENCODING_MAP(kThumb2MovImm16,       0xf2400000,
                 kFmtBitBlt, 11, 8, kFmtImm16, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0,
                 "mov", "!0C, #!1M", 2),
    ENCODING_MAP(kThumb2StrRRI12,       0xf8c00000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 11, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE01 | IS_STORE,
                 "str", "!0C, [!1C, #!2d]", 2),
    ENCODING_MAP(kThumb2LdrRRI12,       0xf8d00000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 11, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1 | IS_LOAD,
                 "ldr", "!0C, [!1C, #!2d]", 2),
    ENCODING_MAP(kThumb2StrRRI8Predec,       0xf8400c00,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 8, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE01 | IS_STORE,
                 "str", "!0C, [!1C, #-!2d]", 2),
    ENCODING_MAP(kThumb2LdrRRI8Predec,       0xf8500c00,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 8, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1 | IS_LOAD,
                 "ldr", "!0C, [!1C, #-!2d]", 2),
    ENCODING_MAP(kThumb2Cbnz,       0xb900, /* Note: does not affect flags */
                 kFmtBitBlt, 2, 0, kFmtImm6, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_USE0 | IS_BRANCH,
                 "cbnz", "!0C,!1t", 1),
    ENCODING_MAP(kThumb2Cbz,       0xb100, /* Note: does not affect flags */
                 kFmtBitBlt, 2, 0, kFmtImm6, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_USE0 | IS_BRANCH,
                 "cbz", "!0C,!1t", 1),
    ENCODING_MAP(kThumb2AddRRI12,       0xf2000000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtImm12, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_DEF0_USE1,/* Note: doesn't affect flags */
                 "add", "!0C,!1C,#!2d", 2),
    ENCODING_MAP(kThumb2MovRR,       0xea4f0000, /* no setflags encoding */
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 3, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "mov", "!0C, !1C", 2),
    ENCODING_MAP(kThumb2Vmovs,       0xeeb00a40,
                 kFmtSfp, 22, 12, kFmtSfp, 5, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "vmov.f32 ", " !0s, !1s", 2),
    ENCODING_MAP(kThumb2Vmovd,       0xeeb00b40,
                 kFmtDfp, 22, 12, kFmtDfp, 5, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "vmov.f64 ", " !0S, !1S", 2),
    ENCODING_MAP(kThumb2Ldmia,         0xe8900000,
                 kFmtBitBlt, 19, 16, kFmtBitBlt, 15, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE0 | REG_DEF_LIST1 | IS_LOAD,
                 "ldmia", "!0C!!, <!1R>", 2),
    ENCODING_MAP(kThumb2Stmia,         0xe8800000,
                 kFmtBitBlt, 19, 16, kFmtBitBlt, 15, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE0 | REG_USE_LIST1 | IS_STORE,
                 "stmia", "!0C!!, <!1R>", 2),
    ENCODING_MAP(kThumb2AddRRR,  0xeb100000, /* setflags encoding */
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtShift, -1, -1,
                 IS_QUAD_OP | REG_DEF0_USE12 | SETS_CCODES,
                 "adds", "!0C, !1C, !2C!3H", 2),
    ENCODING_MAP(kThumb2SubRRR,       0xebb00000, /* setflags enconding */
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtShift, -1, -1,
                 IS_QUAD_OP | REG_DEF0_USE12 | SETS_CCODES,
                 "subs", "!0C, !1C, !2C!3H", 2),
    ENCODING_MAP(kThumb2SbcRRR,       0xeb700000, /* setflags encoding */
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtShift, -1, -1,
                 IS_QUAD_OP | REG_DEF0_USE12 | USES_CCODES | SETS_CCODES,
                 "sbcs", "!0C, !1C, !2C!3H", 2),
    ENCODING_MAP(kThumb2CmpRR,       0xebb00f00,
                 kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0, kFmtShift, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_USE01 | SETS_CCODES,
                 "cmp", "!0C, !1C", 2),
    ENCODING_MAP(kThumb2SubRRI12,       0xf2a00000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtImm12, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_DEF0_USE1,/* Note: doesn't affect flags */
                 "sub", "!0C,!1C,#!2d", 2),
    ENCODING_MAP(kThumb2MvnImmShift,  0xf06f0000, /* no setflags encoding */
                 kFmtBitBlt, 11, 8, kFmtModImm, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0,
                 "mvn", "!0C, #!1n", 2),
    ENCODING_MAP(kThumb2Sel,       0xfaa0f080,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_DEF0_USE12 | USES_CCODES,
                 "sel", "!0C, !1C, !2C", 2),
    ENCODING_MAP(kThumb2Ubfx,       0xf3c00000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtLsb, -1, -1,
                 kFmtBWidth, 4, 0, IS_QUAD_OP | REG_DEF0_USE1,
                 "ubfx", "!0C, !1C, #!2d, #!3d", 2),
    ENCODING_MAP(kThumb2Sbfx,       0xf3400000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtLsb, -1, -1,
                 kFmtBWidth, 4, 0, IS_QUAD_OP | REG_DEF0_USE1,
                 "sbfx", "!0C, !1C, #!2d, #!3d", 2),
    ENCODING_MAP(kThumb2LdrRRR,    0xf8500000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtBitBlt, 5, 4, IS_QUAD_OP | REG_DEF0_USE12 | IS_LOAD,
                 "ldr", "!0C, [!1C, !2C, LSL #!3d]", 2),
    ENCODING_MAP(kThumb2LdrhRRR,    0xf8300000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtBitBlt, 5, 4, IS_QUAD_OP | REG_DEF0_USE12 | IS_LOAD,
                 "ldrh", "!0C, [!1C, !2C, LSL #!3d]", 2),
    ENCODING_MAP(kThumb2LdrshRRR,    0xf9300000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtBitBlt, 5, 4, IS_QUAD_OP | REG_DEF0_USE12 | IS_LOAD,
                 "ldrsh", "!0C, [!1C, !2C, LSL #!3d]", 2),
    ENCODING_MAP(kThumb2LdrbRRR,    0xf8100000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtBitBlt, 5, 4, IS_QUAD_OP | REG_DEF0_USE12 | IS_LOAD,
                 "ldrb", "!0C, [!1C, !2C, LSL #!3d]", 2),
    ENCODING_MAP(kThumb2LdrsbRRR,    0xf9100000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtBitBlt, 5, 4, IS_QUAD_OP | REG_DEF0_USE12 | IS_LOAD,
                 "ldrsb", "!0C, [!1C, !2C, LSL #!3d]", 2),
    ENCODING_MAP(kThumb2StrRRR,    0xf8400000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtBitBlt, 5, 4, IS_QUAD_OP | REG_USE012 | IS_STORE,
                 "str", "!0C, [!1C, !2C, LSL #!3d]", 2),
    ENCODING_MAP(kThumb2StrhRRR,    0xf8200000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtBitBlt, 5, 4, IS_QUAD_OP | REG_USE012 | IS_STORE,
                 "strh", "!0C, [!1C, !2C, LSL #!3d]", 2),
    ENCODING_MAP(kThumb2StrbRRR,    0xf8000000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtBitBlt, 5, 4, IS_QUAD_OP | REG_USE012 | IS_STORE,
                 "strb", "!0C, [!1C, !2C, LSL #!3d]", 2),
    ENCODING_MAP(kThumb2LdrhRRI12,       0xf8b00000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 11, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1 | IS_LOAD,
                 "ldrh", "!0C, [!1C, #!2d]", 2),
    ENCODING_MAP(kThumb2LdrshRRI12,       0xf9b00000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 11, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1 | IS_LOAD,
                 "ldrsh", "!0C, [!1C, #!2d]", 2),
    ENCODING_MAP(kThumb2LdrbRRI12,       0xf8900000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 11, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1 | IS_LOAD,
                 "ldrb", "!0C, [!1C, #!2d]", 2),
    ENCODING_MAP(kThumb2LdrsbRRI12,       0xf9900000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 11, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1 | IS_LOAD,
                 "ldrsb", "!0C, [!1C, #!2d]", 2),
    ENCODING_MAP(kThumb2StrhRRI12,       0xf8a00000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 11, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE01 | IS_STORE,
                 "strh", "!0C, [!1C, #!2d]", 2),
    ENCODING_MAP(kThumb2StrbRRI12,       0xf8800000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 11, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE01 | IS_STORE,
                 "strb", "!0C, [!1C, #!2d]", 2),
    ENCODING_MAP(kThumb2Pop,           0xe8bd0000,
                 kFmtBitBlt, 15, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_UNARY_OP | REG_DEF_SP | REG_USE_SP | REG_DEF_LIST0
                 | IS_LOAD, "pop", "<!0R>", 2),
    ENCODING_MAP(kThumb2Push,          0xe92d0000,
                 kFmtBitBlt, 15, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_UNARY_OP | REG_DEF_SP | REG_USE_SP | REG_USE_LIST0
                 | IS_STORE, "push", "<!0R>", 2),
    ENCODING_MAP(kThumb2CmpRI8, 0xf1b00f00,
                 kFmtBitBlt, 19, 16, kFmtModImm, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_USE0 | SETS_CCODES,
                 "cmp", "!0C, #!1m", 2),
    ENCODING_MAP(kThumb2AdcRRR,  0xeb500000, /* setflags encoding */
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtShift, -1, -1,
                 IS_QUAD_OP | REG_DEF0_USE12 | SETS_CCODES,
                 "adcs", "!0C, !1C, !2C!3H", 2),
    ENCODING_MAP(kThumb2AndRRR,  0xea000000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtShift, -1, -1, IS_QUAD_OP | REG_DEF0_USE12,
                 "and", "!0C, !1C, !2C!3H", 2),
    ENCODING_MAP(kThumb2BicRRR,  0xea200000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtShift, -1, -1, IS_QUAD_OP | REG_DEF0_USE12,
                 "bic", "!0C, !1C, !2C!3H", 2),
    ENCODING_MAP(kThumb2CmnRR,  0xeb000000,
                 kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0, kFmtShift, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_DEF0_USE1 | SETS_CCODES,
                 "cmn", "!0C, !1C, shift !2d", 2),
    ENCODING_MAP(kThumb2EorRRR,  0xea800000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtShift, -1, -1, IS_QUAD_OP | REG_DEF0_USE12,
                 "eor", "!0C, !1C, !2C!3H", 2),
    ENCODING_MAP(kThumb2MulRRR,  0xfb00f000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "mul", "!0C, !1C, !2C", 2),
    ENCODING_MAP(kThumb2MnvRR,  0xea6f0000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 3, 0, kFmtShift, -1, -1,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "mvn", "!0C, !1C, shift !2d", 2),
    ENCODING_MAP(kThumb2RsubRRI8,       0xf1d00000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtModImm, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_DEF0_USE1 | SETS_CCODES,
                 "rsb", "!0C,!1C,#!2m", 2),
    ENCODING_MAP(kThumb2NegRR,       0xf1d00000, /* instance of rsub */
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE1 | SETS_CCODES,
                 "neg", "!0C,!1C", 2),
    ENCODING_MAP(kThumb2OrrRRR,  0xea400000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtShift, -1, -1, IS_QUAD_OP | REG_DEF0_USE12,
                 "orr", "!0C, !1C, !2C!3H", 2),
    ENCODING_MAP(kThumb2TstRR,       0xea100f00,
                 kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0, kFmtShift, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_USE01 | SETS_CCODES,
                 "tst", "!0C, !1C, shift !2d", 2),
    ENCODING_MAP(kThumb2LslRRR,  0xfa00f000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "lsl", "!0C, !1C, !2C", 2),
    ENCODING_MAP(kThumb2LsrRRR,  0xfa20f000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "lsr", "!0C, !1C, !2C", 2),
    ENCODING_MAP(kThumb2AsrRRR,  0xfa40f000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "asr", "!0C, !1C, !2C", 2),
    ENCODING_MAP(kThumb2RorRRR,  0xfa60f000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "ror", "!0C, !1C, !2C", 2),
    ENCODING_MAP(kThumb2LslRRI5,  0xea4f0000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 3, 0, kFmtShift5, -1, -1,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "lsl", "!0C, !1C, #!2d", 2),
    ENCODING_MAP(kThumb2LsrRRI5,  0xea4f0010,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 3, 0, kFmtShift5, -1, -1,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "lsr", "!0C, !1C, #!2d", 2),
    ENCODING_MAP(kThumb2AsrRRI5,  0xea4f0020,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 3, 0, kFmtShift5, -1, -1,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "asr", "!0C, !1C, #!2d", 2),
    ENCODING_MAP(kThumb2RorRRI5,  0xea4f0030,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 3, 0, kFmtShift5, -1, -1,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "ror", "!0C, !1C, #!2d", 2),
    ENCODING_MAP(kThumb2BicRRI8,  0xf0200000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtModImm, -1, -1,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "bic", "!0C, !1C, #!2m", 2),
    ENCODING_MAP(kThumb2AndRRI8,  0xf0000000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtModImm, -1, -1,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "and", "!0C, !1C, #!2m", 2),
    ENCODING_MAP(kThumb2OrrRRI8,  0xf0400000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtModImm, -1, -1,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "orr", "!0C, !1C, #!2m", 2),
    ENCODING_MAP(kThumb2EorRRI8,  0xf0800000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtModImm, -1, -1,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "eor", "!0C, !1C, #!2m", 2),
    ENCODING_MAP(kThumb2AddRRI8,  0xf1100000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtModImm, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_DEF0_USE1 | SETS_CCODES,
                 "adds", "!0C, !1C, #!2m", 2),
    ENCODING_MAP(kThumb2AdcRRI8,  0xf1500000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtModImm, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_DEF0_USE1 | SETS_CCODES | USES_CCODES,
                 "adcs", "!0C, !1C, #!2m", 2),
    ENCODING_MAP(kThumb2SubRRI8,  0xf1b00000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtModImm, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_DEF0_USE1 | SETS_CCODES,
                 "subs", "!0C, !1C, #!2m", 2),
    ENCODING_MAP(kThumb2SbcRRI8,  0xf1700000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtModImm, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_DEF0_USE1 | SETS_CCODES | USES_CCODES,
                 "sbcs", "!0C, !1C, #!2m", 2),
    ENCODING_MAP(kThumb2It,  0xbf00,
                 kFmtBitBlt, 7, 4, kFmtBitBlt, 3, 0, kFmtModImm, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | IS_IT | USES_CCODES,
                 "it:!1b", "!0c", 1),
    ENCODING_MAP(kThumb2Fmstat,  0xeef1fa10,
                 kFmtUnused, -1, -1, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, NO_OPERAND | SETS_CCODES,
                 "fmstat", "", 2),
    ENCODING_MAP(kThumb2Vcmpd,        0xeeb40b40,
                 kFmtDfp, 22, 12, kFmtDfp, 5, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_USE01,
                 "vcmp.f64", "!0S, !1S", 2),
    ENCODING_MAP(kThumb2Vcmps,        0xeeb40a40,
                 kFmtSfp, 22, 12, kFmtSfp, 5, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_USE01,
                 "vcmp.f32", "!0s, !1s", 2),
    ENCODING_MAP(kThumb2LdrPcRel12,       0xf8df0000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 11, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_DEF0 | REG_USE_PC | IS_LOAD,
                 "ldr", "!0C, [r15pc, #!1d]", 2),
    ENCODING_MAP(kThumb2BCond,        0xf0008000,
                 kFmtBrOffset, -1, -1, kFmtBitBlt, 25, 22, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | IS_BRANCH | USES_CCODES,
                 "b!1c", "!0t", 2),
    ENCODING_MAP(kThumb2Vmovd_RR,       0xeeb00b40,
                 kFmtDfp, 22, 12, kFmtDfp, 5, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "vmov.f64", "!0S, !1S", 2),
    ENCODING_MAP(kThumb2Vmovs_RR,       0xeeb00a40,
                 kFmtSfp, 22, 12, kFmtSfp, 5, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "vmov.f32", "!0s, !1s", 2),
    ENCODING_MAP(kThumb2Fmrs,       0xee100a10,
                 kFmtBitBlt, 15, 12, kFmtSfp, 7, 16, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "fmrs", "!0C, !1s", 2),
    ENCODING_MAP(kThumb2Fmsr,       0xee000a10,
                 kFmtSfp, 7, 16, kFmtBitBlt, 15, 12, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "fmsr", "!0s, !1C", 2),
    ENCODING_MAP(kThumb2Fmrrd,       0xec500b10,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16, kFmtDfp, 5, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF01_USE2,
                 "fmrrd", "!0C, !1C, !2S", 2),
    ENCODING_MAP(kThumb2Fmdrr,       0xec400b10,
                 kFmtDfp, 5, 0, kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "fmdrr", "!0S, !1C, !2C", 2),
    ENCODING_MAP(kThumb2Vabsd,       0xeeb00bc0,
                 kFmtDfp, 22, 12, kFmtDfp, 5, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "vabs.f64", "!0S, !1S", 2),
    ENCODING_MAP(kThumb2Vabss,       0xeeb00ac0,
                 kFmtSfp, 22, 12, kFmtSfp, 5, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "vabs.f32", "!0s, !1s", 2),
    ENCODING_MAP(kThumb2Vnegd,       0xeeb10b40,
                 kFmtDfp, 22, 12, kFmtDfp, 5, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "vneg.f64", "!0S, !1S", 2),
    ENCODING_MAP(kThumb2Vnegs,       0xeeb10a40,
                 kFmtSfp, 22, 12, kFmtSfp, 5, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "vneg.f32", "!0s, !1s", 2),
    ENCODING_MAP(kThumb2Vmovs_IMM8,       0xeeb00a00,
                 kFmtSfp, 22, 12, kFmtFPImm, 16, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0,
                 "vmov.f32", "!0s, #0x!1h", 2),
    ENCODING_MAP(kThumb2Vmovd_IMM8,       0xeeb00b00,
                 kFmtDfp, 22, 12, kFmtFPImm, 16, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0,
                 "vmov.f64", "!0S, #0x!1h", 2),
    ENCODING_MAP(kThumb2Mla,  0xfb000000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtBitBlt, 3, 0,
                 kFmtBitBlt, 15, 12,
                 IS_QUAD_OP | REG_DEF0 | REG_USE1 | REG_USE2 | REG_USE3,
                 "mla", "!0C, !1C, !2C, !3C", 2),
    ENCODING_MAP(kThumb2Umull,  0xfba00000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16,
                 kFmtBitBlt, 3, 0,
                 IS_QUAD_OP | REG_DEF0 | REG_DEF1 | REG_USE2 | REG_USE3,
                 "umull", "!0C, !1C, !2C, !3C", 2),
    ENCODING_MAP(kThumb2Ldrex,       0xe8500f00,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16, kFmtBitBlt, 7, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1 | IS_LOAD,
                 "ldrex", "!0C, [!1C, #!2E]", 2),
    ENCODING_MAP(kThumb2Strex,       0xe8400000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 15, 12, kFmtBitBlt, 19, 16,
                 kFmtBitBlt, 7, 0, IS_QUAD_OP | REG_DEF0_USE12 | IS_STORE,
                 "strex", "!0C,!1C, [!2C, #!2E]", 2),
    ENCODING_MAP(kThumb2Clrex,       0xf3bf8f2f,
                 kFmtUnused, -1, -1, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, NO_OPERAND,
                 "clrex", "", 2),
    ENCODING_MAP(kThumb2Bfi,         0xf3600000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtShift5, -1, -1,
                 kFmtBitBlt, 4, 0, IS_QUAD_OP | REG_DEF0_USE1,
                 "bfi", "!0C,!1C,#!2d,#!3d", 2),
    ENCODING_MAP(kThumb2Bfc,         0xf36f0000,
                 kFmtBitBlt, 11, 8, kFmtShift5, -1, -1, kFmtBitBlt, 4, 0,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0,
                 "bfc", "!0C,#!1d,#!2d", 2),
    ENCODING_MAP(kThumb2Dmb,         0xf3bf8f50,
                 kFmtBitBlt, 3, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP,
                 "dmb","#!0B",2),
    ENCODING_MAP(kThumb2LdrPcReln12,       0xf85f0000,
                 kFmtBitBlt, 15, 12, kFmtBitBlt, 11, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0 | REG_USE_PC | IS_LOAD,
                 "ldr", "!0C, [r15pc, -#!1d]", 2),
    ENCODING_MAP(kThumb2Stm,          0xe9000000,
                 kFmtBitBlt, 19, 16, kFmtBitBlt, 12, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_USE0 | REG_USE_LIST1 | IS_STORE,
                 "stm", "!0C, <!1R>", 2),
    ENCODING_MAP(kThumbUndefined,       0xde00,
                 kFmtUnused, -1, -1, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, NO_OPERAND,
                 "undefined", "", 1),
    // NOTE: vpop, vpush hard-encoded for s16+ reg list
    ENCODING_MAP(kThumb2VPopCS,       0xecbd8a00,
                 kFmtBitBlt, 7, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_UNARY_OP | REG_DEF_SP | REG_USE_SP | REG_DEF_FPCS_LIST0
                 | IS_LOAD, "vpop", "<!0P>", 2),
    ENCODING_MAP(kThumb2VPushCS,      0xed2d8a00,
                 kFmtBitBlt, 7, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_UNARY_OP | REG_DEF_SP | REG_USE_SP | REG_USE_FPCS_LIST0
                 | IS_STORE, "vpush", "<!0P>", 2),
    ENCODING_MAP(kThumb2Vldms,        0xec900a00,
                 kFmtBitBlt, 19, 16, kFmtSfp, 22, 12, kFmtBitBlt, 7, 0,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_USE0 | REG_DEF_FPCS_LIST2
                 | IS_LOAD, "vldms", "!0C, <!2Q>", 2),
    ENCODING_MAP(kThumb2Vstms,        0xec800a00,
                 kFmtBitBlt, 19, 16, kFmtSfp, 22, 12, kFmtBitBlt, 7, 0,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_USE0 | REG_USE_FPCS_LIST2
                 | IS_STORE, "vstms", "!0C, <!2Q>", 2),
    ENCODING_MAP(kThumb2BUncond,      0xf0009000,
                 kFmtOff24, -1, -1, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, NO_OPERAND | IS_BRANCH,
                 "b", "!0t", 2),
    ENCODING_MAP(kThumb2MovImm16H,       0xf2c00000,
                 kFmtBitBlt, 11, 8, kFmtImm16, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0,
                 "movh", "!0C, #!1M", 2),
    ENCODING_MAP(kThumb2AddPCR,      0x4487,
                 kFmtBitBlt, 6, 3, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_UNARY_OP | REG_USE0 | IS_BRANCH,
                 "add", "rPC, !0C", 1),
    ENCODING_MAP(kThumb2Adr,         0xf20f0000,
                 kFmtBitBlt, 11, 8, kFmtImm12, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_DEF0,/* Note: doesn't affect flags */
                 "adr", "!0C,#!1d", 2),
    ENCODING_MAP(kThumb2MovImm16LST,     0xf2400000,
                 kFmtBitBlt, 11, 8, kFmtImm16, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0,
                 "mov", "!0C, #!1M", 2),
    ENCODING_MAP(kThumb2MovImm16HST,     0xf2c00000,
                 kFmtBitBlt, 11, 8, kFmtImm16, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0,
                 "movh", "!0C, #!1M", 2),
    ENCODING_MAP(kThumb2LdmiaWB,         0xe8b00000,
                 kFmtBitBlt, 19, 16, kFmtBitBlt, 15, 0, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0_USE0 | REG_DEF_LIST1 | IS_LOAD,
                 "ldmia", "!0C!!, <!1R>", 2),
    ENCODING_MAP(kThumb2SubsRRI12,       0xf1b00000,
                 kFmtBitBlt, 11, 8, kFmtBitBlt, 19, 16, kFmtImm12, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_TERTIARY_OP | REG_DEF0_USE1 | SETS_CCODES,
                 "subs", "!0C,!1C,#!2d", 2),

};

/*
 * The fake NOP of moving r0 to r0 actually will incur data stalls if r0 is
 * not ready. Since r5FP is not updated often, it is less likely to
 * generate unnecessary stall cycles.
 * TUNING: No longer true - find new NOP pattern.
 */
#define PADDING_MOV_R5_R5               0x1C2D

static void pushWord(std::vector<short>&buf, int data) {
    buf.push_back( data & 0xffff);
    buf.push_back( (data >> 16) & 0xffff);
}

void alignBuffer(std::vector<short>&buf, size_t offset) {
    while (buf.size() < (offset/2))
        buf.push_back(0);
}

/* Write the numbers in the constant to the output stream */
static void installLiteralPools(CompilationUnit* cUnit)
{
    alignBuffer(cUnit->codeBuffer, cUnit->dataOffset);
    ArmLIR* dataLIR = (ArmLIR*) cUnit->literalList;
    while (dataLIR) {
        pushWord(cUnit->codeBuffer, dataLIR->operands[0]);
        dataLIR = NEXT_LIR(dataLIR);
    }
}

/* Write the switch tables to the output stream */
static void installSwitchTables(CompilationUnit* cUnit)
{
    GrowableListIterator iterator;
    oatGrowableListIteratorInit(&cUnit->switchTables, &iterator);
    while (true) {
        SwitchTable* tabRec = (SwitchTable *) oatGrowableListIteratorNext(
             &iterator);
        if (tabRec == NULL) break;
        alignBuffer(cUnit->codeBuffer, tabRec->offset);
        int bxOffset = tabRec->bxInst->generic.offset + 4;
        if (cUnit->printMe) {
            LOG(INFO) << "Switch table for offset 0x" << std::hex << bxOffset;
        }
        if (tabRec->table[0] == kSparseSwitchSignature) {
            int* keys = (int*)&(tabRec->table[2]);
            for (int elems = 0; elems < tabRec->table[1]; elems++) {
                int disp = tabRec->targets[elems]->generic.offset - bxOffset;
                if (cUnit->printMe) {
                    LOG(INFO) << "    Case[" << elems << "] key: 0x" <<
                        std::hex << keys[elems] << ", disp: 0x" <<
                        std::hex << disp;
                }
                pushWord(cUnit->codeBuffer, keys[elems]);
                pushWord(cUnit->codeBuffer,
                    tabRec->targets[elems]->generic.offset - bxOffset);
            }
        } else {
            assert(tabRec->table[0] == kPackedSwitchSignature);
            for (int elems = 0; elems < tabRec->table[1]; elems++) {
                int disp = tabRec->targets[elems]->generic.offset - bxOffset;
                if (cUnit->printMe) {
                    LOG(INFO) << "    Case[" << elems << "] disp: 0x" <<
                        std::hex << disp;
                }
                pushWord(cUnit->codeBuffer,
                         tabRec->targets[elems]->generic.offset - bxOffset);
            }
        }
    }
}

/* Write the fill array dta to the output stream */
static void installFillArrayData(CompilationUnit* cUnit)
{
    GrowableListIterator iterator;
    oatGrowableListIteratorInit(&cUnit->fillArrayData, &iterator);
    while (true) {
        FillArrayData *tabRec = (FillArrayData *) oatGrowableListIteratorNext(
             &iterator);
        if (tabRec == NULL) break;
        alignBuffer(cUnit->codeBuffer, tabRec->offset);
        for (int i = 0; i < (tabRec->size / 2) ; i++) {
            cUnit->codeBuffer.push_back( tabRec->table[i]);
        }
    }
}

/*
 * Assemble the LIR into binary instruction format.  Note that we may
 * discover that pc-relative displacements may not fit the selected
 * instruction.
 */
static AssemblerStatus assembleInstructions(CompilationUnit* cUnit,
                                            intptr_t startAddr)
{
    ArmLIR* lir;
    AssemblerStatus res = kSuccess;  // Assume success

    for (lir = (ArmLIR *) cUnit->firstLIRInsn; lir; lir = NEXT_LIR(lir)) {
        if (lir->opcode < 0) {
            if ((lir->opcode == kArmPseudoPseudoAlign4) &&
                /* 1 means padding is needed */
                (lir->operands[0] == 1)) {
                cUnit->codeBuffer.push_back(PADDING_MOV_R5_R5);
            }
            continue;
        }

        if (lir->flags.isNop) {
            continue;
        }

        /*
         * For PC-relative displacements we won't know if the
         * selected instruction will work until late (i.e. - now).
         * If something doesn't fit, we must replace the short-form
         * operation with a longer-form one.  Note, though, that this
         * can change code we've already processed, so we'll need to
         * re-calculate offsets and restart.  To limit the number of
         * restarts, the entire list will be scanned and patched.
         * Of course, the patching itself may cause new overflows so this
         * is an iterative process.
         */

        if (lir->opcode == kThumbLdrPcRel ||
            lir->opcode == kThumb2LdrPcRel12 ||
            lir->opcode == kThumbAddPcRel ||
            ((lir->opcode == kThumb2Vldrd) && (lir->operands[1] == r15pc)) ||
            ((lir->opcode == kThumb2Vldrs) && (lir->operands[1] == r15pc))) {
            /*
             * PC-relative loads are mostly used to load immediates
             * that are too large to materialize directly in one shot.
             * However, if the load displacement exceeds the limit,
             * we revert to a 2-instruction materialization sequence.
             */
            ArmLIR *lirTarget = (ArmLIR *) lir->generic.target;
            intptr_t pc = (lir->generic.offset + 4) & ~3;
            intptr_t target = lirTarget->generic.offset;
            int delta = target - pc;
            if (delta & 0x3) {
                LOG(FATAL) << "PC-rel offset not multiple of 4: " << delta;
            }
            // First, a sanity check for cases we shouldn't see now
            if (((lir->opcode == kThumbAddPcRel) && (delta > 1020)) ||
                ((lir->opcode == kThumbLdrPcRel) && (delta > 1020))) {
                // Shouldn't happen in current codegen.
                LOG(FATAL) << "Unexpected pc-rel offset " << delta;
            }
            // Now, check for the two difficult cases
            if (((lir->opcode == kThumb2LdrPcRel12) && (delta > 4091)) ||
                ((lir->opcode == kThumb2Vldrs) && (delta > 1020)) ||
                ((lir->opcode == kThumb2Vldrd) && (delta > 1020))) {
                int baseReg = (lir->opcode == kThumb2LdrPcRel12) ?
                    lir->operands[0] : rLR;

                // Add new Adr to generate the address
                ArmLIR *newAdr =
                    (ArmLIR *)oatNew(sizeof(ArmLIR), true);
                newAdr->generic.dalvikOffset = lir->generic.dalvikOffset;
                newAdr->generic.target = lir->generic.target;
                newAdr->opcode = kThumb2Adr;
                newAdr->operands[0] = baseReg;
                oatSetupResourceMasks(newAdr);
                oatInsertLIRBefore((LIR*)lir, (LIR*)newAdr);

                // Convert to normal load
                if (lir->opcode == kThumb2LdrPcRel12) {
                    lir->opcode = kThumb2LdrRRI12;
                }
                // Change the load to be relative to the new Adr base
                lir->operands[1] = baseReg;
                lir->operands[2] = 0;
                oatSetupResourceMasks(lir);
                res = kRetryAll;
            } else {
                if ((lir->opcode == kThumb2Vldrs) ||
                    (lir->opcode == kThumb2Vldrd)) {
                    lir->operands[2] = delta >> 2;
                } else {
                    lir->operands[1] = (lir->opcode == kThumb2LdrPcRel12) ?
                                        delta : delta >> 2;
                }
            }
        } else if (lir->opcode == kThumb2Cbnz || lir->opcode == kThumb2Cbz) {
            ArmLIR *targetLIR = (ArmLIR *) lir->generic.target;
            intptr_t pc = lir->generic.offset + 4;
            intptr_t target = targetLIR->generic.offset;
            int delta = target - pc;
            if (delta > 126 || delta < 0) {
                /* Convert to cmp rx,#0 / b[eq/ne] tgt pair */
                ArmLIR *newInst =
                    (ArmLIR *)oatNew(sizeof(ArmLIR), true);
                /* Make new branch instruction and insert after */
                newInst->generic.dalvikOffset = lir->generic.dalvikOffset;
                newInst->opcode = kThumbBCond;
                newInst->operands[0] = 0;
                newInst->operands[1] = (lir->opcode == kThumb2Cbz) ?
                                        kArmCondEq : kArmCondNe;
                newInst->generic.target = lir->generic.target;
                oatSetupResourceMasks(newInst);
                oatInsertLIRAfter((LIR *)lir, (LIR *)newInst);
                /* Convert the cb[n]z to a cmp rx, #0 ] */
                lir->opcode = kThumbCmpRI8;
                /* operand[0] is src1 in both cb[n]z & CmpRI8 */
                lir->operands[1] = 0;
                lir->generic.target = 0;
                oatSetupResourceMasks(lir);
                res = kRetryAll;
            } else {
                lir->operands[1] = delta >> 1;
            }
        } else if (lir->opcode == kThumbBCond ||
                   lir->opcode == kThumb2BCond) {
            ArmLIR *targetLIR = (ArmLIR *) lir->generic.target;
            int delta = 0;
            assert(targetLIR);
            intptr_t pc = lir->generic.offset + 4;
            intptr_t target = targetLIR->generic.offset;
            delta = target - pc;
            if ((lir->opcode == kThumbBCond) && (delta > 254 || delta < -256)) {
                lir->opcode = kThumb2BCond;
                oatSetupResourceMasks(lir);
                res = kRetryAll;
            }
            lir->operands[0] = delta >> 1;
        } else if (lir->opcode == kThumb2BUncond) {
            ArmLIR *targetLIR = (ArmLIR *) lir->generic.target;
            intptr_t pc = lir->generic.offset + 4;
            intptr_t target = targetLIR->generic.offset;
            int delta = target - pc;
            lir->operands[0] = delta >> 1;
            if (lir->operands[0] == 0) {  // Useless branch?
                lir->flags.isNop = true;
                res = kRetryAll;
            }
        } else if (lir->opcode == kThumbBUncond) {
            ArmLIR *targetLIR = (ArmLIR *) lir->generic.target;
            intptr_t pc = lir->generic.offset + 4;
            intptr_t target = targetLIR->generic.offset;
            int delta = target - pc;
            if (delta > 2046 || delta < -2048) {
                // Convert to Thumb2BCond w/ kArmCondAl
                lir->opcode = kThumb2BUncond;
                lir->operands[0] = 0;
                oatSetupResourceMasks(lir);
                res = kRetryAll;
            }
            lir->operands[0] = delta >> 1;
            if ((lir->operands[0] == 0) ||
                (lir->operands[0] == -1)) {  // Useless branch?
                lir->flags.isNop = true;
                res = kRetryAll;
            }
        } else if (lir->opcode == kThumbBlx1) {
            assert(NEXT_LIR(lir)->opcode == kThumbBlx2);
            /* curPC is Thumb */
            intptr_t curPC = (startAddr + lir->generic.offset + 4) & ~3;
            intptr_t target = lir->operands[1];

            /* Match bit[1] in target with base */
            if (curPC & 0x2) {
                target |= 0x2;
            }
            int delta = target - curPC;
            assert((delta >= -(1<<22)) && (delta <= ((1<<22)-2)));

            lir->operands[0] = (delta >> 12) & 0x7ff;
            NEXT_LIR(lir)->operands[0] = (delta>> 1) & 0x7ff;
        } else if (lir->opcode == kThumbBl1) {
            assert(NEXT_LIR(lir)->opcode == kThumbBl2);
            /* Both curPC and target are Thumb */
            intptr_t curPC = startAddr + lir->generic.offset + 4;
            intptr_t target = lir->operands[1];

            int delta = target - curPC;
            assert((delta >= -(1<<22)) && (delta <= ((1<<22)-2)));

            lir->operands[0] = (delta >> 12) & 0x7ff;
            NEXT_LIR(lir)->operands[0] = (delta>> 1) & 0x7ff;
        } else if (lir->opcode == kThumb2Adr) {
            SwitchTable *tabRec = (SwitchTable*)lir->operands[2];
            ArmLIR* target = (ArmLIR*)lir->generic.target;
            int targetDisp = tabRec ? tabRec->offset : target->generic.offset;
            int disp = targetDisp - ((lir->generic.offset + 4) & ~3);
            if (disp < 4096) {
                lir->operands[1] = disp;
            } else {
                // convert to ldimm16l, ldimm16h, add tgt, pc, operands[0]
                ArmLIR *newMov16L =
                    (ArmLIR *)oatNew(sizeof(ArmLIR), true);
                newMov16L->generic.dalvikOffset = lir->generic.dalvikOffset;
                newMov16L->generic.target = lir->generic.target;
                newMov16L->opcode = kThumb2MovImm16LST;
                newMov16L->operands[0] = lir->operands[0];
                newMov16L->operands[2] = (intptr_t)lir;
                newMov16L->operands[3] = (intptr_t)tabRec;
                oatSetupResourceMasks(newMov16L);
                oatInsertLIRBefore((LIR*)lir, (LIR*)newMov16L);
                ArmLIR *newMov16H =
                    (ArmLIR *)oatNew(sizeof(ArmLIR), true);
                newMov16H->generic.dalvikOffset = lir->generic.dalvikOffset;
                newMov16H->generic.target = lir->generic.target;
                newMov16H->opcode = kThumb2MovImm16HST;
                newMov16H->operands[0] = lir->operands[0];
                newMov16H->operands[2] = (intptr_t)lir;
                newMov16H->operands[3] = (intptr_t)tabRec;
                oatSetupResourceMasks(newMov16H);
                oatInsertLIRBefore((LIR*)lir, (LIR*)newMov16H);
                lir->opcode = kThumb2AddRRR;
                lir->operands[1] = rPC;
                lir->operands[2] = lir->operands[0];
                oatSetupResourceMasks(lir);
                res = kRetryAll;
            }
        } else if (lir->opcode == kThumb2MovImm16LST) {
            // operands[1] should hold disp, [2] has add, [3] has tabRec
            ArmLIR *addPCInst = (ArmLIR*)lir->operands[2];
            SwitchTable *tabRec = (SwitchTable*)lir->operands[3];
            // If tabRec is null, this is a literal load - use generic.target
            ArmLIR* target = (ArmLIR*)lir->generic.target;
            int targetDisp = tabRec ? tabRec->offset : target->generic.offset;
            lir->operands[1] = (targetDisp -
                (addPCInst->generic.offset + 4)) & 0xffff;
        } else if (lir->opcode == kThumb2MovImm16HST) {
            // operands[1] should hold disp, [2] has add, [3] has tabRec
            ArmLIR *addPCInst = (ArmLIR*)lir->operands[2];
            SwitchTable *tabRec = (SwitchTable*)lir->operands[3];
            // If tabRec is null, this is a literal load - use generic.target
            ArmLIR* target = (ArmLIR*)lir->generic.target;
            int targetDisp = tabRec ? tabRec->offset : target->generic.offset;
            lir->operands[1] = ((targetDisp -
                (addPCInst->generic.offset + 4)) >> 16) & 0xffff;
        }
        ArmEncodingMap *encoder = &EncodingMap[lir->opcode];
        u4 bits = encoder->skeleton;
        int i;
        for (i = 0; i < 4; i++) {
            u4 operand;
            u4 value;
            operand = lir->operands[i];
            switch(encoder->fieldLoc[i].kind) {
                case kFmtUnused:
                    break;
                case kFmtFPImm:
                    value = ((operand & 0xF0) >> 4) << encoder->fieldLoc[i].end;
                    value |= (operand & 0x0F) << encoder->fieldLoc[i].start;
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
                case kFmtBitBlt:
                    value = (operand << encoder->fieldLoc[i].start) &
                            ((1 << (encoder->fieldLoc[i].end + 1)) - 1);
                    bits |= value;
                    break;
                case kFmtDfp: {
                    assert(DOUBLEREG(operand));
                    assert((operand & 0x1) == 0);
                    int regName = (operand & FP_REG_MASK) >> 1;
                    /* Snag the 1-bit slice and position it */
                    value = ((regName & 0x10) >> 4) <<
                            encoder->fieldLoc[i].end;
                    /* Extract and position the 4-bit slice */
                    value |= (regName & 0x0f) <<
                            encoder->fieldLoc[i].start;
                    bits |= value;
                    break;
                }
                case kFmtSfp:
                    assert(SINGLEREG(operand));
                    /* Snag the 1-bit slice and position it */
                    value = (operand & 0x1) <<
                            encoder->fieldLoc[i].end;
                    /* Extract and position the 4-bit slice */
                    value |= ((operand & 0x1e) >> 1) <<
                            encoder->fieldLoc[i].start;
                    bits |= value;
                    break;
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
                    u4 signbit = (operand >> 31) & 0x1;
                    u4 i1 = (operand >> 22) & 0x1;
                    u4 i2 = (operand >> 21) & 0x1;
                    u4 imm10 = (operand >> 11) & 0x03ff;
                    u4 imm11 = operand & 0x07ff;
                    u4 j1 = (i1 ^ signbit) ? 0 : 1;
                    u4 j2 = (i2 ^ signbit) ? 0 : 1;
                    value = (signbit << 26) | (j1 << 13) | (j2 << 11) |
                            (imm10 << 16) | imm11;
                    bits |= value;
                    }
                    break;
                default:
                    assert(0);
            }
        }
        if (encoder->size == 2) {
                cUnit->codeBuffer.push_back((bits >> 16) & 0xffff);
        }
        cUnit->codeBuffer.push_back(bits & 0xffff);
    }
    return res;
}

static int assignLiteralOffsetCommon(LIR* lir, int offset)
{
    for (;lir != NULL; lir = lir->next) {
        lir->offset = offset;
        offset += 4;
    }
    return offset;
}

static void createMappingTable(CompilationUnit* cUnit)
{
    ArmLIR* armLIR;
    int currentDalvikOffset = -1;

    for (armLIR = (ArmLIR *) cUnit->firstLIRInsn;
         armLIR;
         armLIR = NEXT_LIR(armLIR)) {
        if ((armLIR->opcode >= 0) && !armLIR->flags.isNop &&
            (currentDalvikOffset != armLIR->generic.dalvikOffset)) {
            // Changed - need to emit a record
            cUnit->mappingTable.push_back(armLIR->generic.offset);
            cUnit->mappingTable.push_back(armLIR->generic.dalvikOffset);
            currentDalvikOffset = armLIR->generic.dalvikOffset;
        }
    }
}

/* Determine the offset of each literal field */
static int assignLiteralOffset(CompilationUnit* cUnit, int offset)
{
    offset = assignLiteralOffsetCommon(cUnit->literalList, offset);
    return offset;
}

static int assignSwitchTablesOffset(CompilationUnit* cUnit, int offset)
{
    GrowableListIterator iterator;
    oatGrowableListIteratorInit(&cUnit->switchTables, &iterator);
    while (true) {
        SwitchTable *tabRec = (SwitchTable *) oatGrowableListIteratorNext(
             &iterator);
        if (tabRec == NULL) break;
        tabRec->offset = offset;
        if (tabRec->table[0] == kSparseSwitchSignature) {
            offset += tabRec->table[1] * (sizeof(int) * 2);
        } else {
            assert(tabRec->table[0] == kPackedSwitchSignature);
            offset += tabRec->table[1] * sizeof(int);
        }
    }
    return offset;
}

static int assignFillArrayDataOffset(CompilationUnit* cUnit, int offset)
{
    GrowableListIterator iterator;
    oatGrowableListIteratorInit(&cUnit->fillArrayData, &iterator);
    while (true) {
        FillArrayData *tabRec = (FillArrayData *) oatGrowableListIteratorNext(
             &iterator);
        if (tabRec == NULL) break;
        tabRec->offset = offset;
        offset += tabRec->size;
        // word align
        offset = (offset + 3) & ~3;
        }
    return offset;
}

/*
 * Walk the compilation unit and assign offsets to instructions
 * and literals and compute the total size of the compiled unit.
 */
void assignOffsets(CompilationUnit* cUnit)
{
    ArmLIR* armLIR;
    int offset = 0;

    for (armLIR = (ArmLIR *) cUnit->firstLIRInsn;
         armLIR;
         armLIR = NEXT_LIR(armLIR)) {
        armLIR->generic.offset = offset;
        if (armLIR->opcode >= 0 && !armLIR->flags.isNop) {
            armLIR->flags.size = EncodingMap[armLIR->opcode].size * 2;
            offset += armLIR->flags.size;
        } else if (armLIR->opcode == kArmPseudoPseudoAlign4) {
            if (offset & 0x2) {
                offset += 2;
                armLIR->operands[0] = 1;
            } else {
                armLIR->operands[0] = 0;
            }
        }
        /* Pseudo opcodes don't consume space */
    }

    /* Const values have to be word aligned */
    offset = (offset + 3) & ~3;

    /* Set up offsets for literals */
    cUnit->dataOffset = offset;

    offset = assignLiteralOffset(cUnit, offset);

    offset = assignSwitchTablesOffset(cUnit, offset);

    offset = assignFillArrayDataOffset(cUnit, offset);

    cUnit->totalSize = offset;
}
/*
 * Go over each instruction in the list and calculate the offset from the top
 * before sending them off to the assembler. If out-of-range branch distance is
 * seen rearrange the instructions a bit to correct it.
 */
void oatAssembleLIR(CompilationUnit* cUnit)
{
    assignOffsets(cUnit);
    /*
     * Assemble here.  Note that we generate code with optimistic assumptions
     * and if found now to work, we'll have to redo the sequence and retry.
     */

    while (true) {
        AssemblerStatus res = assembleInstructions(cUnit, NULL);
        if (res == kSuccess) {
            break;
        } else {
            cUnit->assemblerRetries++;
            if (cUnit->assemblerRetries > MAX_ASSEMBLER_RETRIES) {
                LOG(FATAL) << "Assembler error - too many retries";
            }
            // Redo offsets and try again
            assignOffsets(cUnit);
            cUnit->codeBuffer.clear();
        }
    }

    // Install literals
    installLiteralPools(cUnit);

    // Install switch tables
    installSwitchTables(cUnit);

    // Install fill array data
    installFillArrayData(cUnit);

    /*
     * Create the mapping table
     */
    createMappingTable(cUnit);
}
