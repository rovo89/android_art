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

#include "arm64_lir.h"
#include "codegen_arm64.h"
#include "dex/quick/mir_to_lir-inl.h"

namespace art {

// The macros below are exclusively used in the encoding map.

// Most generic way of providing two variants for one instructions.
#define CUSTOM_VARIANTS(variant1, variant2) variant1, variant2

// Used for instructions which do not have a wide variant.
#define NO_VARIANTS(variant) \
  CUSTOM_VARIANTS(variant, 0)

// Used for instructions which have a wide variant with the sf bit set to 1.
#define SF_VARIANTS(sf0_skeleton) \
  CUSTOM_VARIANTS(sf0_skeleton, (sf0_skeleton | 0x80000000))

// Used for instructions which have a wide variant with the size bits set to either x0 or x1.
#define SIZE_VARIANTS(sizex0_skeleton) \
  CUSTOM_VARIANTS(sizex0_skeleton, (sizex0_skeleton | 0x40000000))

// Used for instructions which have a wide variant with the sf and n bits set to 1.
#define SF_N_VARIANTS(sf0_n0_skeleton) \
  CUSTOM_VARIANTS(sf0_n0_skeleton, (sf0_n0_skeleton | 0x80400000))

// Used for FP instructions which have a single and double precision variants, with he type bits set
// to either 00 or 01.
#define FLOAT_VARIANTS(type00_skeleton) \
  CUSTOM_VARIANTS(type00_skeleton, (type00_skeleton | 0x00400000))

/*
 * opcode: ArmOpcode enum
 * variants: instruction skeletons supplied via CUSTOM_VARIANTS or derived macros.
 * a{n}k: key to applying argument {n}    \
 * a{n}s: argument {n} start bit position | n = 0, 1, 2, 3
 * a{n}e: argument {n} end bit position   /
 * flags: instruction attributes (used in optimization)
 * name: mnemonic name
 * fmt: for pretty-printing
 * fixup: used for second-pass fixes (e.g. adresses fixups in branch instructions).
 */
#define ENCODING_MAP(opcode, variants, a0k, a0s, a0e, a1k, a1s, a1e, a2k, a2s, a2e, \
                     a3k, a3s, a3e, flags, name, fmt, fixup) \
        {variants, {{a0k, a0s, a0e}, {a1k, a1s, a1e}, {a2k, a2s, a2e}, \
                    {a3k, a3s, a3e}}, opcode, flags, name, fmt, 4, fixup}

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
 *     d -> decimal
 *     D -> decimal*4 or decimal*8 depending on the instruction width
 *     E -> decimal*4
 *     F -> decimal*2
 *     G -> ", lsl #2" or ", lsl #3" depending on the instruction width
 *     c -> branch condition (eq, ne, etc.)
 *     t -> pc-relative target
 *     p -> pc-relative address
 *     s -> single precision floating point register
 *     S -> double precision floating point register
 *     f -> single or double precision register (depending on instruction width)
 *     I -> 8-bit immediate floating point number
 *     l -> logical immediate
 *     M -> 16-bit shift expression ("" or ", lsl #16" or ", lsl #32"...)
 *     B -> dmb option string (sy, st, ish, ishst, nsh, hshst)
 *     H -> operand shift
 *     T -> register shift (either ", lsl #0" or ", lsl #12")
 *     e -> register extend (e.g. uxtb #1)
 *     o -> register shift (e.g. lsl #1) for Word registers
 *     w -> word (32-bit) register wn, or wzr
 *     W -> word (32-bit) register wn, or wsp
 *     x -> extended (64-bit) register xn, or xzr
 *     X -> extended (64-bit) register xn, or sp
 *     r -> register with same width as instruction, r31 -> wzr, xzr
 *     R -> register with same width as instruction, r31 -> wsp, sp
 *
 *  [!] escape.  To insert "!", use "!!"
 */
/* NOTE: must be kept in sync with enum ArmOpcode from arm64_lir.h */
const ArmEncodingMap Arm64Mir2Lir::EncodingMap[kA64Last] = {
    ENCODING_MAP(WIDE(kA64Adc3rrr), SF_VARIANTS(0x1a000000),
                 kFmtRegR, 4, 0, kFmtRegR, 9, 5, kFmtRegR, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "adc", "!0r, !1r, !2r", kFixupNone),
    ENCODING_MAP(WIDE(kA64Add4RRdT), SF_VARIANTS(0x11000000),
                 kFmtRegROrSp, 4, 0, kFmtRegROrSp, 9, 5, kFmtBitBlt, 21, 10,
                 kFmtBitBlt, 23, 22, IS_QUAD_OP | REG_DEF0_USE1,
                 "add", "!0R, !1R, #!2d!3T", kFixupNone),
    ENCODING_MAP(WIDE(kA64Add4rrro), SF_VARIANTS(0x0b000000),
                 kFmtRegR, 4, 0, kFmtRegR, 9, 5, kFmtRegR, 20, 16,
                 kFmtShift, -1, -1, IS_QUAD_OP | REG_DEF0_USE12,
                 "add", "!0r, !1r, !2r!3o", kFixupNone),
    ENCODING_MAP(WIDE(kA64Add4RRre), SF_VARIANTS(0x0b200000),
                 kFmtRegROrSp, 4, 0, kFmtRegROrSp, 9, 5, kFmtRegR, 20, 16,
                 kFmtExtend, -1, -1, IS_QUAD_OP | REG_DEF0_USE12,
                 "add", "!0r, !1r, !2r!3e", kFixupNone),
    // Note: adr is binary, but declared as tertiary. The third argument is used while doing the
    //   fixups and contains information to identify the adr label.
    ENCODING_MAP(kA64Adr2xd, NO_VARIANTS(0x10000000),
                 kFmtRegX, 4, 0, kFmtImm21, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0 | NEEDS_FIXUP,
                 "adr", "!0x, #!1d", kFixupAdr),
    ENCODING_MAP(WIDE(kA64And3Rrl), SF_VARIANTS(0x12000000),
                 kFmtRegROrSp, 4, 0, kFmtRegR, 9, 5, kFmtBitBlt, 22, 10,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "and", "!0R, !1r, #!2l", kFixupNone),
    ENCODING_MAP(WIDE(kA64And4rrro), SF_VARIANTS(0x0a000000),
                 kFmtRegR, 4, 0, kFmtRegR, 9, 5, kFmtRegR, 20, 16,
                 kFmtShift, -1, -1, IS_QUAD_OP | REG_DEF0_USE12,
                 "and", "!0r, !1r, !2r!3o", kFixupNone),
    ENCODING_MAP(WIDE(kA64Asr3rrd), CUSTOM_VARIANTS(0x13007c00, 0x9340fc00),
                 kFmtRegR, 4, 0, kFmtRegR, 9, 5, kFmtBitBlt, 21, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "asr", "!0r, !1r, #!2d", kFixupNone),
    ENCODING_MAP(WIDE(kA64Asr3rrr), SF_VARIANTS(0x1ac02800),
                 kFmtRegR, 4, 0, kFmtRegR, 9, 5, kFmtRegR, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "asr", "!0r, !1r, !2r", kFixupNone),
    ENCODING_MAP(kA64B2ct, NO_VARIANTS(0x54000000),
                 kFmtBitBlt, 3, 0, kFmtBitBlt, 23, 5, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | IS_BRANCH | USES_CCODES |
                 NEEDS_FIXUP, "b.!0c", "!1t", kFixupCondBranch),
    ENCODING_MAP(kA64Blr1x, NO_VARIANTS(0xd63f0000),
                 kFmtRegX, 9, 5, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_UNARY_OP | REG_USE0 | IS_BRANCH | REG_DEF_LR,
                 "blr", "!0x", kFixupNone),
    ENCODING_MAP(kA64Br1x, NO_VARIANTS(0xd61f0000),
                 kFmtRegX, 9, 5, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP | REG_USE0 | IS_BRANCH,
                 "br", "!0x", kFixupNone),
    ENCODING_MAP(kA64Brk1d, NO_VARIANTS(0xd4200000),
                 kFmtBitBlt, 20, 5, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP | IS_BRANCH,
                 "brk", "!0d", kFixupNone),
    ENCODING_MAP(kA64B1t, NO_VARIANTS(0x14000000),
                 kFmtBitBlt, 25, 0, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP | IS_BRANCH | NEEDS_FIXUP,
                 "b", "!0t", kFixupT1Branch),
    ENCODING_MAP(WIDE(kA64Cbnz2rt), SF_VARIANTS(0x35000000),
                 kFmtRegR, 4, 0, kFmtBitBlt, 23, 5, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_USE0 | IS_BRANCH | NEEDS_FIXUP,
                 "cbnz", "!0r, !1t", kFixupCBxZ),
    ENCODING_MAP(WIDE(kA64Cbz2rt), SF_VARIANTS(0x34000000),
                 kFmtRegR, 4, 0, kFmtBitBlt, 23, 5, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_USE0 | IS_BRANCH | NEEDS_FIXUP,
                 "cbz", "!0r, !1t", kFixupCBxZ),
    ENCODING_MAP(WIDE(kA64Cmn3rro), SF_VARIANTS(0x2b00001f),
                 kFmtRegR, 9, 5, kFmtRegR, 20, 16, kFmtShift, -1, -1,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE01 | SETS_CCODES,
                 "cmn", "!0r, !1r!2o", kFixupNone),
    ENCODING_MAP(WIDE(kA64Cmn3Rre), SF_VARIANTS(0x2b20001f),
                 kFmtRegROrSp, 9, 5, kFmtRegR, 20, 16, kFmtExtend, -1, -1,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE01 | SETS_CCODES,
                 "cmn", "!0R, !1r!2e", kFixupNone),
    ENCODING_MAP(WIDE(kA64Cmn3RdT), SF_VARIANTS(0x3100001f),
                 kFmtRegROrSp, 9, 5, kFmtBitBlt, 21, 10, kFmtBitBlt, 23, 22,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE0 | SETS_CCODES,
                 "cmn", "!0R, #!1d!2T", kFixupNone),
    ENCODING_MAP(WIDE(kA64Cmp3rro), SF_VARIANTS(0x6b00001f),
                 kFmtRegR, 9, 5, kFmtRegR, 20, 16, kFmtShift, -1, -1,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE01 | SETS_CCODES,
                 "cmp", "!0r, !1r!2o", kFixupNone),
    ENCODING_MAP(WIDE(kA64Cmp3Rre), SF_VARIANTS(0x6b20001f),
                 kFmtRegROrSp, 9, 5, kFmtRegR, 20, 16, kFmtExtend, -1, -1,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE01 | SETS_CCODES,
                 "cmp", "!0R, !1r!2e", kFixupNone),
    ENCODING_MAP(WIDE(kA64Cmp3RdT), SF_VARIANTS(0x7100001f),
                 kFmtRegROrSp, 9, 5, kFmtBitBlt, 21, 10, kFmtBitBlt, 23, 22,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE0 | SETS_CCODES,
                 "cmp", "!0R, #!1d!2T", kFixupNone),
    ENCODING_MAP(WIDE(kA64Csel4rrrc), SF_VARIANTS(0x1a800000),
                 kFmtRegR, 4, 0, kFmtRegR, 9, 5, kFmtRegR, 20, 16,
                 kFmtBitBlt, 15, 12, IS_QUAD_OP | REG_DEF0_USE12 | USES_CCODES,
                 "csel", "!0r, !1r, !2r, !3c", kFixupNone),
    ENCODING_MAP(WIDE(kA64Csinc4rrrc), SF_VARIANTS(0x1a800400),
                 kFmtRegR, 4, 0, kFmtRegR, 9, 5, kFmtRegR, 20, 16,
                 kFmtBitBlt, 15, 12, IS_QUAD_OP | REG_DEF0_USE12 | USES_CCODES,
                 "csinc", "!0r, !1r, !2r, !3c", kFixupNone),
    ENCODING_MAP(WIDE(kA64Csinv4rrrc), SF_VARIANTS(0x5a800000),
                 kFmtRegR, 4, 0, kFmtRegR, 9, 5, kFmtRegR, 20, 16,
                 kFmtBitBlt, 15, 12, IS_QUAD_OP | REG_DEF0_USE12 | USES_CCODES,
                 "csinv", "!0r, !1r, !2r, !3c", kFixupNone),
    ENCODING_MAP(WIDE(kA64Csneg4rrrc), SF_VARIANTS(0x5a800400),
                 kFmtRegR, 4, 0, kFmtRegR, 9, 5, kFmtRegR, 20, 16,
                 kFmtBitBlt, 15, 12, IS_QUAD_OP | REG_DEF0_USE12 | USES_CCODES,
                 "csneg", "!0r, !1r, !2r, !3c", kFixupNone),
    ENCODING_MAP(kA64Dmb1B, NO_VARIANTS(0xd50330bf),
                 kFmtBitBlt, 11, 8, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP | IS_VOLATILE,
                 "dmb", "#!0B", kFixupNone),
    ENCODING_MAP(WIDE(kA64Eor3Rrl), SF_VARIANTS(0x52000000),
                 kFmtRegROrSp, 4, 0, kFmtRegR, 9, 5, kFmtBitBlt, 22, 10,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "eor", "!0R, !1r, #!2l", kFixupNone),
    ENCODING_MAP(WIDE(kA64Eor4rrro), SF_VARIANTS(0x4a000000),
                 kFmtRegR, 4, 0, kFmtRegR, 9, 5, kFmtRegR, 20, 16,
                 kFmtShift, -1, -1, IS_QUAD_OP | REG_DEF0_USE12,
                 "eor", "!0r, !1r, !2r!3o", kFixupNone),
    ENCODING_MAP(WIDE(kA64Extr4rrrd), SF_N_VARIANTS(0x13800000),
                 kFmtRegR, 4, 0, kFmtRegR, 9, 5, kFmtRegR, 20, 16,
                 kFmtBitBlt, 15, 10, IS_QUAD_OP | REG_DEF0_USE12,
                 "extr", "!0r, !1r, !2r, #!3d", kFixupNone),
    ENCODING_MAP(FWIDE(kA64Fabs2ff), FLOAT_VARIANTS(0x1e20c000),
                 kFmtRegF, 4, 0, kFmtRegF, 9, 5, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP| REG_DEF0_USE1,
                 "fabs", "!0f, !1f", kFixupNone),
    ENCODING_MAP(FWIDE(kA64Fadd3fff), FLOAT_VARIANTS(0x1e202800),
                 kFmtRegF, 4, 0, kFmtRegF, 9, 5, kFmtRegF, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "fadd", "!0f, !1f, !2f", kFixupNone),
    ENCODING_MAP(FWIDE(kA64Fcmp1f), FLOAT_VARIANTS(0x1e202008),
                 kFmtRegF, 9, 5, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_UNARY_OP | REG_USE0 | SETS_CCODES,
                 "fcmp", "!0f, #0", kFixupNone),
    ENCODING_MAP(FWIDE(kA64Fcmp2ff), FLOAT_VARIANTS(0x1e202000),
                 kFmtRegF, 9, 5, kFmtRegF, 20, 16, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_USE01 | SETS_CCODES,
                 "fcmp", "!0f, !1f", kFixupNone),
    ENCODING_MAP(FWIDE(kA64Fcvtzs2wf), FLOAT_VARIANTS(0x1e380000),
                 kFmtRegW, 4, 0, kFmtRegF, 9, 5, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "fcvtzs", "!0w, !1f", kFixupNone),
    ENCODING_MAP(FWIDE(kA64Fcvtzs2xf), FLOAT_VARIANTS(0x9e380000),
                 kFmtRegX, 4, 0, kFmtRegF, 9, 5, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "fcvtzs", "!0x, !1f", kFixupNone),
    ENCODING_MAP(kA64Fcvt2Ss, NO_VARIANTS(0x1e22C000),
                 kFmtRegD, 4, 0, kFmtRegS, 9, 5, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "fcvt", "!0S, !1s", kFixupNone),
    ENCODING_MAP(kA64Fcvt2sS, NO_VARIANTS(0x1e624000),
                 kFmtRegS, 4, 0, kFmtRegD, 9, 5, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "fcvt", "!0s, !1S", kFixupNone),
    ENCODING_MAP(kA64Fcvtms2ws, NO_VARIANTS(0x1e300000),
                 kFmtRegW, 4, 0, kFmtRegS, 9, 5, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "fcvtms", "!0w, !1s", kFixupNone),
    ENCODING_MAP(kA64Fcvtms2xS, NO_VARIANTS(0x9e700000),
                 kFmtRegX, 4, 0, kFmtRegD, 9, 5, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "fcvtms", "!0x, !1S", kFixupNone),
    ENCODING_MAP(FWIDE(kA64Fdiv3fff), FLOAT_VARIANTS(0x1e201800),
                 kFmtRegF, 4, 0, kFmtRegF, 9, 5, kFmtRegF, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "fdiv", "!0f, !1f, !2f", kFixupNone),
    ENCODING_MAP(FWIDE(kA64Fmax3fff), FLOAT_VARIANTS(0x1e204800),
                 kFmtRegF, 4, 0, kFmtRegF, 9, 5, kFmtRegF, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "fmax", "!0f, !1f, !2f", kFixupNone),
    ENCODING_MAP(FWIDE(kA64Fmin3fff), FLOAT_VARIANTS(0x1e205800),
                 kFmtRegF, 4, 0, kFmtRegF, 9, 5, kFmtRegF, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "fmin", "!0f, !1f, !2f", kFixupNone),
    ENCODING_MAP(FWIDE(kA64Fmov2ff), FLOAT_VARIANTS(0x1e204000),
                 kFmtRegF, 4, 0, kFmtRegF, 9, 5, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1 | IS_MOVE,
                 "fmov", "!0f, !1f", kFixupNone),
    ENCODING_MAP(FWIDE(kA64Fmov2fI), FLOAT_VARIANTS(0x1e201000),
                 kFmtRegF, 4, 0, kFmtBitBlt, 20, 13, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0,
                 "fmov", "!0f, #!1I", kFixupNone),
    ENCODING_MAP(kA64Fmov2sw, NO_VARIANTS(0x1e270000),
                 kFmtRegS, 4, 0, kFmtRegW, 9, 5, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "fmov", "!0s, !1w", kFixupNone),
    ENCODING_MAP(kA64Fmov2Sx, NO_VARIANTS(0x9e670000),
                 kFmtRegD, 4, 0, kFmtRegX, 9, 5, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "fmov", "!0S, !1x", kFixupNone),
    ENCODING_MAP(kA64Fmov2ws, NO_VARIANTS(0x1e260000),
                 kFmtRegW, 4, 0, kFmtRegS, 9, 5, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "fmov", "!0w, !1s", kFixupNone),
    ENCODING_MAP(kA64Fmov2xS, NO_VARIANTS(0x9e660000),
                 kFmtRegX, 4, 0, kFmtRegD, 9, 5, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "fmov", "!0x, !1S", kFixupNone),
    ENCODING_MAP(FWIDE(kA64Fmul3fff), FLOAT_VARIANTS(0x1e200800),
                 kFmtRegF, 4, 0, kFmtRegF, 9, 5, kFmtRegF, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "fmul", "!0f, !1f, !2f", kFixupNone),
    ENCODING_MAP(FWIDE(kA64Fneg2ff), FLOAT_VARIANTS(0x1e214000),
                 kFmtRegF, 4, 0, kFmtRegF, 9, 5, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "fneg", "!0f, !1f", kFixupNone),
    ENCODING_MAP(FWIDE(kA64Frintp2ff), FLOAT_VARIANTS(0x1e24c000),
                 kFmtRegF, 4, 0, kFmtRegF, 9, 5, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "frintp", "!0f, !1f", kFixupNone),
    ENCODING_MAP(FWIDE(kA64Frintm2ff), FLOAT_VARIANTS(0x1e254000),
                 kFmtRegF, 4, 0, kFmtRegF, 9, 5, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "frintm", "!0f, !1f", kFixupNone),
    ENCODING_MAP(FWIDE(kA64Frintn2ff), FLOAT_VARIANTS(0x1e244000),
                 kFmtRegF, 4, 0, kFmtRegF, 9, 5, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "frintn", "!0f, !1f", kFixupNone),
    ENCODING_MAP(FWIDE(kA64Frintz2ff), FLOAT_VARIANTS(0x1e25c000),
                 kFmtRegF, 4, 0, kFmtRegF, 9, 5, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "frintz", "!0f, !1f", kFixupNone),
    ENCODING_MAP(FWIDE(kA64Fsqrt2ff), FLOAT_VARIANTS(0x1e61c000),
                 kFmtRegF, 4, 0, kFmtRegF, 9, 5, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "fsqrt", "!0f, !1f", kFixupNone),
    ENCODING_MAP(FWIDE(kA64Fsub3fff), FLOAT_VARIANTS(0x1e203800),
                 kFmtRegF, 4, 0, kFmtRegF, 9, 5, kFmtRegF, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "fsub", "!0f, !1f, !2f", kFixupNone),
    ENCODING_MAP(kA64Ldrb3wXd, NO_VARIANTS(0x39400000),
                 kFmtRegW, 4, 0, kFmtRegXOrSp, 9, 5, kFmtBitBlt, 21, 10,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1 | IS_LOAD_OFF,
                 "ldrb", "!0w, [!1X, #!2d]", kFixupNone),
    ENCODING_MAP(kA64Ldrb3wXx, NO_VARIANTS(0x38606800),
                 kFmtRegW, 4, 0, kFmtRegXOrSp, 9, 5, kFmtRegX, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12 | IS_LOAD,
                 "ldrb", "!0w, [!1X, !2x]", kFixupNone),
    ENCODING_MAP(WIDE(kA64Ldrsb3rXd), CUSTOM_VARIANTS(0x39c00000, 0x39800000),
                 kFmtRegR, 4, 0, kFmtRegXOrSp, 9, 5, kFmtBitBlt, 21, 10,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1 | IS_LOAD_OFF,
                 "ldrsb", "!0r, [!1X, #!2d]", kFixupNone),
    ENCODING_MAP(WIDE(kA64Ldrsb3rXx), CUSTOM_VARIANTS(0x38e06800, 0x38a06800),
                 kFmtRegR, 4, 0, kFmtRegXOrSp, 9, 5, kFmtRegX, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12 | IS_LOAD,
                 "ldrsb", "!0r, [!1X, !2x]", kFixupNone),
    ENCODING_MAP(kA64Ldrh3wXF, NO_VARIANTS(0x79400000),
                 kFmtRegW, 4, 0, kFmtRegXOrSp, 9, 5, kFmtBitBlt, 21, 10,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1 | IS_LOAD_OFF,
                 "ldrh", "!0w, [!1X, #!2F]", kFixupNone),
    ENCODING_MAP(kA64Ldrh4wXxd, NO_VARIANTS(0x78606800),
                 kFmtRegW, 4, 0, kFmtRegXOrSp, 9, 5, kFmtRegX, 20, 16,
                 kFmtBitBlt, 12, 12, IS_QUAD_OP | REG_DEF0_USE12 | IS_LOAD_OFF,
                 "ldrh", "!0w, [!1X, !2x, lsl #!3d]", kFixupNone),
    ENCODING_MAP(WIDE(kA64Ldrsh3rXF), CUSTOM_VARIANTS(0x79c00000, 0x79800000),
                 kFmtRegR, 4, 0, kFmtRegXOrSp, 9, 5, kFmtBitBlt, 21, 10,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1 | IS_LOAD_OFF,
                 "ldrsh", "!0r, [!1X, #!2F]", kFixupNone),
    ENCODING_MAP(WIDE(kA64Ldrsh4rXxd), CUSTOM_VARIANTS(0x78e06800, 0x78906800),
                 kFmtRegR, 4, 0, kFmtRegXOrSp, 9, 5, kFmtRegX, 20, 16,
                 kFmtBitBlt, 12, 12, IS_QUAD_OP | REG_DEF0_USE12 | IS_LOAD_OFF,
                 "ldrsh", "!0r, [!1X, !2x, lsl #!3d]", kFixupNone),
    ENCODING_MAP(FWIDE(kA64Ldr2fp), SIZE_VARIANTS(0x1c000000),
                 kFmtRegF, 4, 0, kFmtBitBlt, 23, 5, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0 | REG_USE_PC | IS_LOAD | NEEDS_FIXUP,
                 "ldr", "!0f, !1p", kFixupLoad),
    ENCODING_MAP(WIDE(kA64Ldr2rp), SIZE_VARIANTS(0x18000000),
                 kFmtRegR, 4, 0, kFmtBitBlt, 23, 5, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1,
                 IS_BINARY_OP | REG_DEF0 | REG_USE_PC | IS_LOAD | NEEDS_FIXUP,
                 "ldr", "!0r, !1p", kFixupLoad),
    ENCODING_MAP(FWIDE(kA64Ldr3fXD), SIZE_VARIANTS(0xbd400000),
                 kFmtRegF, 4, 0, kFmtRegXOrSp, 9, 5, kFmtBitBlt, 21, 10,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1 | IS_LOAD_OFF,
                 "ldr", "!0f, [!1X, #!2D]", kFixupNone),
    ENCODING_MAP(WIDE(kA64Ldr3rXD), SIZE_VARIANTS(0xb9400000),
                 kFmtRegR, 4, 0, kFmtRegXOrSp, 9, 5, kFmtBitBlt, 21, 10,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1 | IS_LOAD_OFF,
                 "ldr", "!0r, [!1X, #!2D]", kFixupNone),
    ENCODING_MAP(FWIDE(kA64Ldr4fXxG), SIZE_VARIANTS(0xbc606800),
                 kFmtRegF, 4, 0, kFmtRegXOrSp, 9, 5, kFmtRegX, 20, 16,
                 kFmtBitBlt, 12, 12, IS_QUAD_OP | REG_DEF0_USE12 | IS_LOAD,
                 "ldr", "!0f, [!1X, !2x!3G]", kFixupNone),
    ENCODING_MAP(WIDE(kA64Ldr4rXxG), SIZE_VARIANTS(0xb8606800),
                 kFmtRegR, 4, 0, kFmtRegXOrSp, 9, 5, kFmtRegX, 20, 16,
                 kFmtBitBlt, 12, 12, IS_QUAD_OP | REG_DEF0_USE12 | IS_LOAD,
                 "ldr", "!0r, [!1X, !2x!3G]", kFixupNone),
    ENCODING_MAP(WIDE(kA64LdrPost3rXd), SIZE_VARIANTS(0xb8400400),
                 kFmtRegR, 4, 0, kFmtRegXOrSp, 9, 5, kFmtBitBlt, 20, 12,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF01 | REG_USE1 | IS_LOAD,
                 "ldr", "!0r, [!1X], #!2d", kFixupNone),
    ENCODING_MAP(WIDE(kA64Ldp4ffXD), CUSTOM_VARIANTS(0x2d400000, 0x6d400000),
                 kFmtRegF, 4, 0, kFmtRegF, 14, 10, kFmtRegXOrSp, 9, 5,
                 kFmtBitBlt, 21, 15, IS_QUAD_OP | REG_USE2 | REG_DEF01 | IS_LOAD_OFF,
                 "ldp", "!0f, !1f, [!2X, #!3D]", kFixupNone),
    ENCODING_MAP(WIDE(kA64Ldp4rrXD), SF_VARIANTS(0x29400000),
                 kFmtRegR, 4, 0, kFmtRegR, 14, 10, kFmtRegXOrSp, 9, 5,
                 kFmtBitBlt, 21, 15, IS_QUAD_OP | REG_USE2 | REG_DEF01 | IS_LOAD_OFF,
                 "ldp", "!0r, !1r, [!2X, #!3D]", kFixupNone),
    ENCODING_MAP(WIDE(kA64LdpPost4rrXD), CUSTOM_VARIANTS(0x28c00000, 0xa8c00000),
                 kFmtRegR, 4, 0, kFmtRegR, 14, 10, kFmtRegXOrSp, 9, 5,
                 kFmtBitBlt, 21, 15, IS_QUAD_OP | REG_USE2 | REG_DEF012 | IS_LOAD,
                 "ldp", "!0r, !1r, [!2X], #!3D", kFixupNone),
    ENCODING_MAP(FWIDE(kA64Ldur3fXd), CUSTOM_VARIANTS(0xbc400000, 0xfc400000),
                 kFmtRegF, 4, 0, kFmtRegXOrSp, 9, 5, kFmtBitBlt, 20, 12,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1 | IS_LOAD,
                 "ldur", "!0f, [!1X, #!2d]", kFixupNone),
    ENCODING_MAP(WIDE(kA64Ldur3rXd), SIZE_VARIANTS(0xb8400000),
                 kFmtRegR, 4, 0, kFmtRegXOrSp, 9, 5, kFmtBitBlt, 20, 12,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1 | IS_LOAD,
                 "ldur", "!0r, [!1X, #!2d]", kFixupNone),
    ENCODING_MAP(WIDE(kA64Ldxr2rX), SIZE_VARIANTS(0x885f7c00),
                 kFmtRegR, 4, 0, kFmtRegXOrSp, 9, 5, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1 | IS_LOADX,
                 "ldxr", "!0r, [!1X]", kFixupNone),
    ENCODING_MAP(WIDE(kA64Ldaxr2rX), SIZE_VARIANTS(0x885ffc00),
                 kFmtRegR, 4, 0, kFmtRegXOrSp, 9, 5, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1 | IS_LOADX,
                 "ldaxr", "!0r, [!1X]", kFixupNone),
    ENCODING_MAP(WIDE(kA64Lsl3rrr), SF_VARIANTS(0x1ac02000),
                 kFmtRegR, 4, 0, kFmtRegR, 9, 5, kFmtRegR, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "lsl", "!0r, !1r, !2r", kFixupNone),
    ENCODING_MAP(WIDE(kA64Lsr3rrd), CUSTOM_VARIANTS(0x53007c00, 0xd340fc00),
                 kFmtRegR, 4, 0, kFmtRegR, 9, 5, kFmtBitBlt, 21, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "lsr", "!0r, !1r, #!2d", kFixupNone),
    ENCODING_MAP(WIDE(kA64Lsr3rrr), SF_VARIANTS(0x1ac02400),
                 kFmtRegR, 4, 0, kFmtRegR, 9, 5, kFmtRegR, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "lsr", "!0r, !1r, !2r", kFixupNone),
    ENCODING_MAP(WIDE(kA64Movk3rdM), SF_VARIANTS(0x72800000),
                 kFmtRegR, 4, 0, kFmtBitBlt, 20, 5, kFmtBitBlt, 22, 21,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE0,
                 "movk", "!0r, #!1d!2M", kFixupNone),
    ENCODING_MAP(WIDE(kA64Movn3rdM), SF_VARIANTS(0x12800000),
                 kFmtRegR, 4, 0, kFmtBitBlt, 20, 5, kFmtBitBlt, 22, 21,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0,
                 "movn", "!0r, #!1d!2M", kFixupNone),
    ENCODING_MAP(WIDE(kA64Movz3rdM), SF_VARIANTS(0x52800000),
                 kFmtRegR, 4, 0, kFmtBitBlt, 20, 5, kFmtBitBlt, 22, 21,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0,
                 "movz", "!0r, #!1d!2M", kFixupNone),
    ENCODING_MAP(WIDE(kA64Mov2rr), SF_VARIANTS(0x2a0003e0),
                 kFmtRegR, 4, 0, kFmtRegR, 20, 16, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1 | IS_MOVE,
                 "mov", "!0r, !1r", kFixupNone),
    ENCODING_MAP(WIDE(kA64Mvn2rr), SF_VARIANTS(0x2a2003e0),
                 kFmtRegR, 4, 0, kFmtRegR, 20, 16, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "mvn", "!0r, !1r", kFixupNone),
    ENCODING_MAP(WIDE(kA64Mul3rrr), SF_VARIANTS(0x1b007c00),
                 kFmtRegR, 4, 0, kFmtRegR, 9, 5, kFmtRegR, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "mul", "!0r, !1r, !2r", kFixupNone),
    ENCODING_MAP(WIDE(kA64Msub4rrrr), SF_VARIANTS(0x1b008000),
                 kFmtRegR, 4, 0, kFmtRegR, 9, 5, kFmtRegR, 14, 10,
                 kFmtRegR, 20, 16, IS_QUAD_OP | REG_DEF0_USE123,
                 "msub", "!0r, !1r, !3r, !2r", kFixupNone),
    ENCODING_MAP(WIDE(kA64Neg3rro), SF_VARIANTS(0x4b0003e0),
                 kFmtRegR, 4, 0, kFmtRegR, 20, 16, kFmtShift, -1, -1,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "neg", "!0r, !1r!2o", kFixupNone),
    ENCODING_MAP(WIDE(kA64Orr3Rrl), SF_VARIANTS(0x32000000),
                 kFmtRegROrSp, 4, 0, kFmtRegR, 9, 5, kFmtBitBlt, 22, 10,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1,
                 "orr", "!0R, !1r, #!2l", kFixupNone),
    ENCODING_MAP(WIDE(kA64Orr4rrro), SF_VARIANTS(0x2a000000),
                 kFmtRegR, 4, 0, kFmtRegR, 9, 5, kFmtRegR, 20, 16,
                 kFmtShift, -1, -1, IS_QUAD_OP | REG_DEF0_USE12,
                 "orr", "!0r, !1r, !2r!3o", kFixupNone),
    ENCODING_MAP(kA64Ret, NO_VARIANTS(0xd65f03c0),
                 kFmtUnused, -1, -1, kFmtUnused, -1, -1, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, NO_OPERAND | IS_BRANCH,
                 "ret", "", kFixupNone),
    ENCODING_MAP(WIDE(kA64Rbit2rr), SF_VARIANTS(0x5ac00000),
                 kFmtRegR, 4, 0, kFmtRegR, 9, 5, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "rbit", "!0r, !1r", kFixupNone),
    ENCODING_MAP(WIDE(kA64Rev2rr), CUSTOM_VARIANTS(0x5ac00800, 0xdac00c00),
                 kFmtRegR, 4, 0, kFmtRegR, 9, 5, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "rev", "!0r, !1r", kFixupNone),
    ENCODING_MAP(WIDE(kA64Rev162rr), SF_VARIANTS(0x5ac00400),
                 kFmtRegR, 4, 0, kFmtRegR, 9, 5, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "rev16", "!0r, !1r", kFixupNone),
    ENCODING_MAP(WIDE(kA64Ror3rrr), SF_VARIANTS(0x1ac02c00),
                 kFmtRegR, 4, 0, kFmtRegR, 9, 5, kFmtRegR, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "ror", "!0r, !1r, !2r", kFixupNone),
    ENCODING_MAP(WIDE(kA64Sbc3rrr), SF_VARIANTS(0x5a000000),
                 kFmtRegR, 4, 0, kFmtRegR, 9, 5, kFmtRegR, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "sbc", "!0r, !1r, !2r", kFixupNone),
    ENCODING_MAP(WIDE(kA64Sbfm4rrdd), SF_N_VARIANTS(0x13000000),
                 kFmtRegR, 4, 0, kFmtRegR, 9, 5, kFmtBitBlt, 21, 16,
                 kFmtBitBlt, 15, 10, IS_QUAD_OP | REG_DEF0_USE1,
                 "sbfm", "!0r, !1r, #!2d, #!3d", kFixupNone),
    ENCODING_MAP(FWIDE(kA64Scvtf2fw), FLOAT_VARIANTS(0x1e220000),
                 kFmtRegF, 4, 0, kFmtRegW, 9, 5, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "scvtf", "!0f, !1w", kFixupNone),
    ENCODING_MAP(FWIDE(kA64Scvtf2fx), FLOAT_VARIANTS(0x9e220000),
                 kFmtRegF, 4, 0, kFmtRegX, 9, 5, kFmtUnused, -1, -1,
                 kFmtUnused, -1, -1, IS_BINARY_OP | REG_DEF0_USE1,
                 "scvtf", "!0f, !1x", kFixupNone),
    ENCODING_MAP(WIDE(kA64Sdiv3rrr), SF_VARIANTS(0x1ac00c00),
                 kFmtRegR, 4, 0, kFmtRegR, 9, 5, kFmtRegR, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "sdiv", "!0r, !1r, !2r", kFixupNone),
    ENCODING_MAP(WIDE(kA64Smaddl4xwwx), NO_VARIANTS(0x9b200000),
                 kFmtRegX, 4, 0, kFmtRegW, 9, 5, kFmtRegW, 20, 16,
                 kFmtRegX, 14, 10, IS_QUAD_OP | REG_DEF0_USE123,
                 "smaddl", "!0x, !1w, !2w, !3x", kFixupNone),
    ENCODING_MAP(kA64Smulh3xxx, NO_VARIANTS(0x9b407c00),
                 kFmtRegX, 4, 0, kFmtRegX, 9, 5, kFmtRegX, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12,
                 "smulh", "!0x, !1x, !2x", kFixupNone),
    ENCODING_MAP(WIDE(kA64Stp4ffXD), CUSTOM_VARIANTS(0x2d000000, 0x6d000000),
                 kFmtRegF, 4, 0, kFmtRegF, 14, 10, kFmtRegXOrSp, 9, 5,
                 kFmtBitBlt, 21, 15, IS_QUAD_OP | REG_USE012 | IS_STORE_OFF,
                 "stp", "!0f, !1f, [!2X, #!3D]", kFixupNone),
    ENCODING_MAP(WIDE(kA64Stp4rrXD), SF_VARIANTS(0x29000000),
                 kFmtRegR, 4, 0, kFmtRegR, 14, 10, kFmtRegXOrSp, 9, 5,
                 kFmtBitBlt, 21, 15, IS_QUAD_OP | REG_USE012 | IS_STORE_OFF,
                 "stp", "!0r, !1r, [!2X, #!3D]", kFixupNone),
    ENCODING_MAP(WIDE(kA64StpPost4rrXD), CUSTOM_VARIANTS(0x28800000, 0xa8800000),
                 kFmtRegR, 4, 0, kFmtRegR, 14, 10, kFmtRegXOrSp, 9, 5,
                 kFmtBitBlt, 21, 15, IS_QUAD_OP | REG_DEF2 | REG_USE012 | IS_STORE,
                 "stp", "!0r, !1r, [!2X], #!3D", kFixupNone),
    ENCODING_MAP(WIDE(kA64StpPre4ffXD), CUSTOM_VARIANTS(0x2d800000, 0x6d800000),
                 kFmtRegF, 4, 0, kFmtRegF, 14, 10, kFmtRegXOrSp, 9, 5,
                 kFmtBitBlt, 21, 15, IS_QUAD_OP | REG_DEF2 | REG_USE012 | IS_STORE,
                 "stp", "!0f, !1f, [!2X, #!3D]!!", kFixupNone),
    ENCODING_MAP(WIDE(kA64StpPre4rrXD), CUSTOM_VARIANTS(0x29800000, 0xa9800000),
                 kFmtRegR, 4, 0, kFmtRegR, 14, 10, kFmtRegXOrSp, 9, 5,
                 kFmtBitBlt, 21, 15, IS_QUAD_OP | REG_DEF2 | REG_USE012 | IS_STORE,
                 "stp", "!0r, !1r, [!2X, #!3D]!!", kFixupNone),
    ENCODING_MAP(FWIDE(kA64Str3fXD), CUSTOM_VARIANTS(0xbd000000, 0xfd000000),
                 kFmtRegF, 4, 0, kFmtRegXOrSp, 9, 5, kFmtBitBlt, 21, 10,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE01 | IS_STORE_OFF,
                 "str", "!0f, [!1X, #!2D]", kFixupNone),
    ENCODING_MAP(FWIDE(kA64Str4fXxG), CUSTOM_VARIANTS(0xbc206800, 0xfc206800),
                 kFmtRegF, 4, 0, kFmtRegXOrSp, 9, 5, kFmtRegX, 20, 16,
                 kFmtBitBlt, 12, 12, IS_QUAD_OP | REG_USE012 | IS_STORE,
                 "str", "!0f, [!1X, !2x!3G]", kFixupNone),
    ENCODING_MAP(WIDE(kA64Str3rXD), SIZE_VARIANTS(0xb9000000),
                 kFmtRegR, 4, 0, kFmtRegXOrSp, 9, 5, kFmtBitBlt, 21, 10,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE01 | IS_STORE_OFF,
                 "str", "!0r, [!1X, #!2D]", kFixupNone),
    ENCODING_MAP(WIDE(kA64Str4rXxG), SIZE_VARIANTS(0xb8206800),
                 kFmtRegR, 4, 0, kFmtRegXOrSp, 9, 5, kFmtRegX, 20, 16,
                 kFmtBitBlt, 12, 12, IS_QUAD_OP | REG_USE012 | IS_STORE,
                 "str", "!0r, [!1X, !2x!3G]", kFixupNone),
    ENCODING_MAP(kA64Strb3wXd, NO_VARIANTS(0x39000000),
                 kFmtRegW, 4, 0, kFmtRegXOrSp, 9, 5, kFmtBitBlt, 21, 10,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE01 | IS_STORE_OFF,
                 "strb", "!0w, [!1X, #!2d]", kFixupNone),
    ENCODING_MAP(kA64Strb3wXx, NO_VARIANTS(0x38206800),
                 kFmtRegW, 4, 0, kFmtRegXOrSp, 9, 5, kFmtRegX, 20, 16,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE012 | IS_STORE,
                 "strb", "!0w, [!1X, !2x]", kFixupNone),
    ENCODING_MAP(kA64Strh3wXF, NO_VARIANTS(0x79000000),
                 kFmtRegW, 4, 0, kFmtRegXOrSp, 9, 5, kFmtBitBlt, 21, 10,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE01 | IS_STORE_OFF,
                 "strh", "!0w, [!1X, #!2F]", kFixupNone),
    ENCODING_MAP(kA64Strh4wXxd, NO_VARIANTS(0x78206800),
                 kFmtRegW, 4, 0, kFmtRegXOrSp, 9, 5, kFmtRegX, 20, 16,
                 kFmtBitBlt, 12, 12, IS_QUAD_OP | REG_USE012 | IS_STORE,
                 "strh", "!0w, [!1X, !2x, lsl #!3d]", kFixupNone),
    ENCODING_MAP(WIDE(kA64StrPost3rXd), SIZE_VARIANTS(0xb8000400),
                 kFmtRegR, 4, 0, kFmtRegXOrSp, 9, 5, kFmtBitBlt, 20, 12,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE01 | REG_DEF1 | IS_STORE,
                 "str", "!0r, [!1X], #!2d", kFixupNone),
    ENCODING_MAP(FWIDE(kA64Stur3fXd), CUSTOM_VARIANTS(0xbc000000, 0xfc000000),
                 kFmtRegF, 4, 0, kFmtRegXOrSp, 9, 5, kFmtBitBlt, 20, 12,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE01 | IS_STORE,
                 "stur", "!0f, [!1X, #!2d]", kFixupNone),
    ENCODING_MAP(WIDE(kA64Stur3rXd), SIZE_VARIANTS(0xb8000000),
                 kFmtRegR, 4, 0, kFmtRegXOrSp, 9, 5, kFmtBitBlt, 20, 12,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_USE01 | IS_STORE,
                 "stur", "!0r, [!1X, #!2d]", kFixupNone),
    ENCODING_MAP(WIDE(kA64Stxr3wrX), SIZE_VARIANTS(0x88007c00),
                 kFmtRegW, 20, 16, kFmtRegR, 4, 0, kFmtRegXOrSp, 9, 5,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12 | IS_STOREX,
                 "stxr", "!0w, !1r, [!2X]", kFixupNone),
    ENCODING_MAP(WIDE(kA64Stlxr3wrX), SIZE_VARIANTS(0x8800fc00),
                 kFmtRegW, 20, 16, kFmtRegR, 4, 0, kFmtRegXOrSp, 9, 5,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE12 | IS_STOREX,
                 "stlxr", "!0w, !1r, [!2X]", kFixupNone),
    ENCODING_MAP(WIDE(kA64Sub4RRdT), SF_VARIANTS(0x51000000),
                 kFmtRegROrSp, 4, 0, kFmtRegROrSp, 9, 5, kFmtBitBlt, 21, 10,
                 kFmtBitBlt, 23, 22, IS_QUAD_OP | REG_DEF0_USE1,
                 "sub", "!0R, !1R, #!2d!3T", kFixupNone),
    ENCODING_MAP(WIDE(kA64Sub4rrro), SF_VARIANTS(0x4b000000),
                 kFmtRegR, 4, 0, kFmtRegR, 9, 5, kFmtRegR, 20, 16,
                 kFmtShift, -1, -1, IS_QUAD_OP | REG_DEF0_USE12,
                 "sub", "!0r, !1r, !2r!3o", kFixupNone),
    ENCODING_MAP(WIDE(kA64Sub4RRre), SF_VARIANTS(0x4b200000),
                 kFmtRegROrSp, 4, 0, kFmtRegROrSp, 9, 5, kFmtRegR, 20, 16,
                 kFmtExtend, -1, -1, IS_QUAD_OP | REG_DEF0_USE12,
                 "sub", "!0r, !1r, !2r!3e", kFixupNone),
    ENCODING_MAP(WIDE(kA64Subs3rRd), SF_VARIANTS(0x71000000),
                 kFmtRegR, 4, 0, kFmtRegROrSp, 9, 5, kFmtBitBlt, 21, 10,
                 kFmtUnused, -1, -1, IS_TERTIARY_OP | REG_DEF0_USE1 | SETS_CCODES,
                 "subs", "!0r, !1R, #!2d", kFixupNone),
    ENCODING_MAP(WIDE(kA64Tst3rro), SF_VARIANTS(0x6a000000),
                 kFmtRegR, 9, 5, kFmtRegR, 20, 16, kFmtShift, -1, -1,
                 kFmtUnused, -1, -1, IS_QUAD_OP | REG_USE01 | SETS_CCODES,
                 "tst", "!0r, !1r!2o", kFixupNone),
    ENCODING_MAP(WIDE(kA64Ubfm4rrdd), SF_N_VARIANTS(0x53000000),
                 kFmtRegR, 4, 0, kFmtRegR, 9, 5, kFmtBitBlt, 21, 16,
                 kFmtBitBlt, 15, 10, IS_QUAD_OP | REG_DEF0_USE1,
                 "ubfm", "!0r, !1r, !2d, !3d", kFixupNone),
};

// new_lir replaces orig_lir in the pcrel_fixup list.
void Arm64Mir2Lir::ReplaceFixup(LIR* prev_lir, LIR* orig_lir, LIR* new_lir) {
  new_lir->u.a.pcrel_next = orig_lir->u.a.pcrel_next;
  if (UNLIKELY(prev_lir == NULL)) {
    first_fixup_ = new_lir;
  } else {
    prev_lir->u.a.pcrel_next = new_lir;
  }
  orig_lir->flags.fixup = kFixupNone;
}

// new_lir is inserted before orig_lir in the pcrel_fixup list.
void Arm64Mir2Lir::InsertFixupBefore(LIR* prev_lir, LIR* orig_lir, LIR* new_lir) {
  new_lir->u.a.pcrel_next = orig_lir;
  if (UNLIKELY(prev_lir == NULL)) {
    first_fixup_ = new_lir;
  } else {
    DCHECK(prev_lir->u.a.pcrel_next == orig_lir);
    prev_lir->u.a.pcrel_next = new_lir;
  }
}

/* Nop, used for aligning code. Nop is an alias for hint #0. */
#define PADDING_NOP (UINT32_C(0xd503201f))

uint8_t* Arm64Mir2Lir::EncodeLIRs(uint8_t* write_pos, LIR* lir) {
  for (; lir != nullptr; lir = NEXT_LIR(lir)) {
    bool opcode_is_wide = IS_WIDE(lir->opcode);
    ArmOpcode opcode = UNWIDE(lir->opcode);

    if (UNLIKELY(IsPseudoLirOp(opcode))) {
      continue;
    }

    if (LIKELY(!lir->flags.is_nop)) {
      const ArmEncodingMap *encoder = &EncodingMap[opcode];

      // Select the right variant of the skeleton.
      uint32_t bits = opcode_is_wide ? encoder->xskeleton : encoder->wskeleton;
      DCHECK(!opcode_is_wide || IS_WIDE(encoder->opcode));

      for (int i = 0; i < 4; i++) {
        ArmEncodingKind kind = encoder->field_loc[i].kind;
        uint32_t operand = lir->operands[i];
        uint32_t value;

        if (LIKELY(static_cast<unsigned>(kind) <= kFmtBitBlt)) {
          // Note: this will handle kFmtReg* and kFmtBitBlt.

          if (static_cast<unsigned>(kind) < kFmtBitBlt) {
            bool is_zero = A64_REG_IS_ZR(operand);

            if (kIsDebugBuild && (kFailOnSizeError || kReportSizeError)) {
              // Register usage checks: First establish register usage requirements based on the
              // format in `kind'.
              bool want_float = false;     // Want a float (rather than core) register.
              bool want_64_bit = false;    // Want a 64-bit (rather than 32-bit) register.
              bool want_var_size = true;   // Want register with variable size (kFmtReg{R,F}).
              bool want_zero = false;      // Want the zero (rather than sp) register.
              switch (kind) {
                case kFmtRegX:
                  want_64_bit = true;
                  // Intentional fall-through.
                case kFmtRegW:
                  want_var_size = false;
                  // Intentional fall-through.
                case kFmtRegR:
                  want_zero = true;
                  break;
                case kFmtRegXOrSp:
                  want_64_bit = true;
                  // Intentional fall-through.
                case kFmtRegWOrSp:
                  want_var_size = false;
                  break;
                case kFmtRegROrSp:
                  break;
                case kFmtRegD:
                  want_64_bit = true;
                  // Intentional fall-through.
                case kFmtRegS:
                  want_var_size = false;
                  // Intentional fall-through.
                case kFmtRegF:
                  want_float = true;
                  break;
                default:
                  LOG(FATAL) << "Bad fmt for arg n. " << i << " of " << encoder->name
                             << " (" << kind << ")";
                  break;
              }

              // want_var_size == true means kind == kFmtReg{R,F}. In these two cases, we want
              // the register size to be coherent with the instruction width.
              if (want_var_size) {
                want_64_bit = opcode_is_wide;
              }

              // Now check that the requirements are satisfied.
              RegStorage reg(operand | RegStorage::kValid);
              const char *expected = nullptr;
              if (want_float) {
                if (!reg.IsFloat()) {
                  expected = "float register";
                } else if (reg.IsDouble() != want_64_bit) {
                  expected = (want_64_bit) ? "double register" : "single register";
                }
              } else {
                if (reg.IsFloat()) {
                  expected = "core register";
                } else if (reg.Is64Bit() != want_64_bit) {
                  expected = (want_64_bit) ? "x-register" : "w-register";
                } else if (A64_REGSTORAGE_IS_SP_OR_ZR(reg) && is_zero != want_zero) {
                  expected = (want_zero) ? "zero-register" : "sp-register";
                }
              }

              // Fail, if `expected' contains an unsatisfied requirement.
              if (expected != nullptr) {
                LOG(WARNING) << "Method: " << PrettyMethod(cu_->method_idx, *cu_->dex_file)
                             << " @ 0x" << std::hex << lir->dalvik_offset;
                if (kFailOnSizeError) {
                  LOG(FATAL) << "Bad argument n. " << i << " of " << encoder->name
                             << "(" << UNWIDE(encoder->opcode) << ", " << encoder->fmt << ")"
                             << ". Expected " << expected << ", got 0x" << std::hex << operand;
                } else {
                  LOG(WARNING) << "Bad argument n. " << i << " of " << encoder->name
                               << ". Expected " << expected << ", got 0x" << std::hex << operand;
                }
              }
            }

            // In the lines below, we rely on (operand & 0x1f) == 31 to be true for register sp
            // and zr. This means that these two registers do not need any special treatment, as
            // their bottom 5 bits are correctly set to 31 == 0b11111, which is the right
            // value for encoding both sp and zr.
            COMPILE_ASSERT((rxzr & 0x1f) == 0x1f, rzr_register_number_must_be_31);
            COMPILE_ASSERT((rsp & 0x1f) == 0x1f, rsp_register_number_must_be_31);
          }

          value = (operand << encoder->field_loc[i].start) &
              ((1 << (encoder->field_loc[i].end + 1)) - 1);
          bits |= value;
        } else {
          switch (kind) {
            case kFmtSkip:
              break;  // Nothing to do, but continue to next.
            case kFmtUnused:
              i = 4;  // Done, break out of the enclosing loop.
              break;
            case kFmtShift:
              // Intentional fallthrough.
            case kFmtExtend:
              DCHECK_EQ((operand & (1 << 6)) == 0, kind == kFmtShift);
              value = (operand & 0x3f) << 10;
              value |= ((operand & 0x1c0) >> 6) << 21;
              bits |= value;
              break;
            case kFmtImm21:
              value = (operand & 0x3) << 29;
              value |= ((operand & 0x1ffffc) >> 2) << 5;
              bits |= value;
              break;
            default:
              LOG(FATAL) << "Bad fmt for arg. " << i << " in " << encoder->name
                         << " (" << kind << ")";
          }
        }
      }

      DCHECK_EQ(encoder->size, 4);
      write_pos[0] = (bits & 0xff);
      write_pos[1] = ((bits >> 8) & 0xff);
      write_pos[2] = ((bits >> 16) & 0xff);
      write_pos[3] = ((bits >> 24) & 0xff);
      write_pos += 4;
    }
  }

  return write_pos;
}

// Align data offset on 8 byte boundary: it will only contain double-word items, as word immediates
// are better set directly from the code (they will require no more than 2 instructions).
#define ALIGNED_DATA_OFFSET(offset) (((offset) + 0x7) & ~0x7)

// Assemble the LIR into binary instruction format.
void Arm64Mir2Lir::AssembleLIR() {
  LIR* lir;
  LIR* prev_lir;
  cu_->NewTimingSplit("Assemble");
  int assembler_retries = 0;
  CodeOffset starting_offset = LinkFixupInsns(first_lir_insn_, last_lir_insn_, 0);
  data_offset_ = ALIGNED_DATA_OFFSET(starting_offset);
  int32_t offset_adjustment;
  AssignDataOffsets();

  /*
   * Note: generation must be 1 on first pass (to distinguish from initialized state of 0
   * for non-visited nodes). Start at zero here, and bit will be flipped to 1 on entry to the loop.
   */
  int generation = 0;
  while (true) {
    // TODO(Arm64): check whether passes and offset adjustments are really necessary.
    //   Currently they aren't, as - in the fixups below - LIR are never inserted.
    //   Things can be different if jump ranges above 1 MB need to be supported.
    //   If they are not, then we can get rid of the assembler retry logic.

    offset_adjustment = 0;
    AssemblerStatus res = kSuccess;  // Assume success
    generation ^= 1;
    // Note: nodes requiring possible fixup linked in ascending order.
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
        case kFixupVLoad:
          break;
        case kFixupT1Branch: {
          LIR *target_lir = lir->target;
          DCHECK(target_lir);
          CodeOffset pc = lir->offset;
          CodeOffset target = target_lir->offset +
              ((target_lir->flags.generation == lir->flags.generation) ? 0 : offset_adjustment);
          int32_t delta = target - pc;
          if (!((delta & 0x3) == 0 && IS_SIGNED_IMM19(delta >> 2))) {
            LOG(FATAL) << "Invalid jump range in kFixupT1Branch";
          }
          lir->operands[0] = delta >> 2;
          break;
        }
        case kFixupLoad:
        case kFixupCBxZ:
        case kFixupCondBranch: {
          LIR *target_lir = lir->target;
          DCHECK(target_lir);
          CodeOffset pc = lir->offset;
          CodeOffset target = target_lir->offset +
            ((target_lir->flags.generation == lir->flags.generation) ? 0 : offset_adjustment);
          int32_t delta = target - pc;
          if (!((delta & 0x3) == 0 && IS_SIGNED_IMM19(delta >> 2))) {
            LOG(FATAL) << "Invalid jump range in kFixupLoad";
          }
          lir->operands[1] = delta >> 2;
          break;
        }
        case kFixupAdr: {
          LIR* target_lir = lir->target;
          int32_t delta;
          if (target_lir) {
            CodeOffset target_offs = ((target_lir->flags.generation == lir->flags.generation) ?
                                      0 : offset_adjustment) + target_lir->offset;
            delta = target_offs - lir->offset;
          } else if (lir->operands[2] >= 0) {
            EmbeddedData* tab = reinterpret_cast<EmbeddedData*>(UnwrapPointer(lir->operands[2]));
            delta = tab->offset + offset_adjustment - lir->offset;
          } else {
            // No fixup: this usage allows to retrieve the current PC.
            delta = lir->operands[1];
          }
          if (!IS_SIGNED_IMM21(delta)) {
            LOG(FATAL) << "Jump range above 1MB in kFixupAdr";
          }
          lir->operands[1] = delta;
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
      data_offset_ = ALIGNED_DATA_OFFSET(starting_offset);
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

  DCHECK_EQ(data_offset_, ALIGNED_DATA_OFFSET(code_buffer_.size()));

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

size_t Arm64Mir2Lir::GetInsnSize(LIR* lir) {
  ArmOpcode opcode = UNWIDE(lir->opcode);
  DCHECK(!IsPseudoLirOp(opcode));
  return EncodingMap[opcode].size;
}

// Encode instruction bit pattern and assign offsets.
uint32_t Arm64Mir2Lir::LinkFixupInsns(LIR* head_lir, LIR* tail_lir, uint32_t offset) {
  LIR* end_lir = tail_lir->next;

  LIR* last_fixup = NULL;
  for (LIR* lir = head_lir; lir != end_lir; lir = NEXT_LIR(lir)) {
    ArmOpcode opcode = UNWIDE(lir->opcode);
    if (!lir->flags.is_nop) {
      if (lir->flags.fixup != kFixupNone) {
        if (!IsPseudoLirOp(opcode)) {
          lir->flags.size = EncodingMap[opcode].size;
          lir->flags.fixup = EncodingMap[opcode].fixup;
        } else {
          DCHECK_NE(static_cast<int>(opcode), kPseudoPseudoAlign4);
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

void Arm64Mir2Lir::AssignDataOffsets() {
  /* Set up offsets for literals */
  CodeOffset offset = data_offset_;

  offset = AssignLiteralOffset(offset);

  offset = AssignSwitchTablesOffset(offset);

  total_size_ = AssignFillArrayDataOffset(offset);
}

}  // namespace art
