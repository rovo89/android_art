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

#ifndef ART_COMPILER_COMPILER_CODEGEN_MIPS_MIPSLIR_H_
#define ART_COMPILER_COMPILER_CODEGEN_MIPS_MIPSLIR_H_

#include "../../Dalvik.h"
#include "../../CompilerInternals.h"

namespace art {

// Set to 1 to measure cost of suspend check
#define NO_SUSPEND 0

/*
 * Runtime register conventions.
 *
 * zero is always the value 0
 * at is scratch (normally used as temp reg by assembler)
 * v0, v1 are scratch (normally hold subroutine return values)
 * a0-a3 are scratch (normally hold subroutine arguments)
 * t0-t8 are scratch
 * t9 is scratch (normally used for function calls)
 * s0 (rSUSPEND) is reserved [holds suspend-check counter]
 * s1 (rSELF) is reserved [holds current &Thread]
 * s2-s7 are callee save (promotion target)
 * k0, k1 are reserved for use by interrupt handlers
 * gp is reserved for global pointer
 * sp is reserved
 * s8 is callee save (promotion target)
 * ra is scratch (normally holds the return addr)
 *
 * Preserved across C calls: s0-s8
 * Trashed across C calls: at, v0-v1, a0-a3, t0-t9, gp, ra
 *
 * Floating pointer registers
 * NOTE: there are 32 fp registers (16 df pairs), but currently
 *       only support 16 fp registers (8 df pairs).
 * f0-f15
 * df0-df7, where df0={f0,f1}, df1={f2,f3}, ... , df7={f14,f15}
 *
 * f0-f15 (df0-df7) trashed across C calls
 *
 * For mips32 code use:
 *      a0-a3 to hold operands
 *      v0-v1 to hold results
 *      t0-t9 for temps
 *
 * All jump/branch instructions have a delay slot after it.
 *
 *  Stack frame diagram (stack grows down, higher addresses at top):
 *
 * +------------------------+
 * | IN[ins-1]              |  {Note: resides in caller's frame}
 * |       .                |
 * | IN[0]                  |
 * | caller's Method*       |
 * +========================+  {Note: start of callee's frame}
 * | spill region           |  {variable sized - will include lr if non-leaf.}
 * +------------------------+
 * | ...filler word...      |  {Note: used as 2nd word of V[locals-1] if long]
 * +------------------------+
 * | V[locals-1]            |
 * | V[locals-2]            |
 * |      .                 |
 * |      .                 |
 * | V[1]                   |
 * | V[0]                   |
 * +------------------------+
 * |  0 to 3 words padding  |
 * +------------------------+
 * | OUT[outs-1]            |
 * | OUT[outs-2]            |
 * |       .                |
 * | OUT[0]                 |
 * | curMethod*             | <<== sp w/ 16-byte alignment
 * +========================+
 */

/* Offset to distingish FP regs */
#define FP_REG_OFFSET 32
/* Offset to distinguish DP FP regs */
#define FP_DOUBLE 64
/* Offset to distingish the extra regs */
#define EXTRA_REG_OFFSET 128
/* Reg types */
#define REGTYPE(x) (x & (FP_REG_OFFSET | FP_DOUBLE))
#define FPREG(x) ((x & FP_REG_OFFSET) == FP_REG_OFFSET)
#define EXTRAREG(x) ((x & EXTRA_REG_OFFSET) == EXTRA_REG_OFFSET)
#define LOWREG(x) ((x & 0x1f) == x)
#define DOUBLEREG(x) ((x & FP_DOUBLE) == FP_DOUBLE)
#define SINGLEREG(x) (FPREG(x) && !DOUBLEREG(x))
/*
 * Note: the low register of a floating point pair is sufficient to
 * create the name of a double, but require both names to be passed to
 * allow for asserts to verify that the pair is consecutive if significant
 * rework is done in this area.  Also, it is a good reminder in the calling
 * code that reg locations always describe doubles as a pair of singles.
 */
#define S2D(x,y) ((x) | FP_DOUBLE)
/* Mask to strip off fp flags */
#define FP_REG_MASK (FP_REG_OFFSET-1)
/* non-existent Dalvik register */
#define vNone   (-1)
/* non-existant physical register */
#define rNone   (-1)

#ifdef HAVE_LITTLE_ENDIAN
#define LOWORD_OFFSET 0
#define HIWORD_OFFSET 4
#define r_ARG0 r_A0
#define r_ARG1 r_A1
#define r_ARG2 r_A2
#define r_ARG3 r_A3
#define r_RESULT0 r_V0
#define r_RESULT1 r_V1
#else
#define LOWORD_OFFSET 4
#define HIWORD_OFFSET 0
#define r_ARG0 r_A1
#define r_ARG1 r_A0
#define r_ARG2 r_A3
#define r_ARG3 r_A2
#define r_RESULT0 r_V1
#define r_RESULT1 r_V0
#endif

/* These are the same for both big and little endian. */
#define r_FARG0 r_F12
#define r_FARG1 r_F13
#define r_FRESULT0 r_F0
#define r_FRESULT1 r_F1

/* RegisterLocation templates return values (r_V0, or r_V0/r_V1) */
#define LOC_C_RETURN {kLocPhysReg, 0, 0, 0, 0, 0, 0, 0, 1, r_V0, INVALID_REG, \
                      INVALID_SREG, INVALID_SREG}
#define LOC_C_RETURN_FLOAT  LOC_C_RETURN
#define LOC_C_RETURN_ALT {kLocPhysReg, 0, 0, 0, 0, 0, 0, 0, 1, r_V1, \
                          INVALID_REG, INVALID_SREG, INVALID_SREG}
#define LOC_C_RETURN_WIDE {kLocPhysReg, 1, 0, 0, 0, 0, 0, 0, 1, r_RESULT0, \
                           r_RESULT1, INVALID_SREG, INVALID_SREG}
#define LOC_C_RETURN_WIDE_DOUBLE  LOC_C_RETURN_WIDE
#define LOC_C_RETURN_WIDE_ALT {kLocPhysReg, 1, 0, 0, 0, 0, 0, 0, 1, r_FRESULT0,\
                               r_FRESULT1, INVALID_SREG, INVALID_SREG}

enum ResourceEncodingPos {
  kGPReg0   = 0,
  kRegSP    = 29,
  kRegLR    = 31,
  kFPReg0   = 32, /* only 16 fp regs supported currently */
  kFPRegEnd   = 48,
  kRegHI    = kFPRegEnd,
  kRegLO,
  kRegPC,
  kRegEnd   = 51,
  kCCode    = kRegEnd,
  kFPStatus,      // FP status word
  // The following four bits are for memory disambiguation
  kDalvikReg,     // 1 Dalvik Frame (can be fully disambiguated)
  kLiteral,       // 2 Literal pool (can be fully disambiguated)
  kHeapRef,       // 3 Somewhere on the heap (alias with any other heap)
  kMustNotAlias,    // 4 Guaranteed to be non-alias (eg *(r6+x))
};

#define ENCODE_REG_LIST(N)      ((u8) N)
#define ENCODE_REG_SP           (1ULL << kRegSP)
#define ENCODE_REG_LR           (1ULL << kRegLR)
#define ENCODE_REG_PC           (1ULL << kRegPC)
#define ENCODE_CCODE            (1ULL << kCCode)
#define ENCODE_FP_STATUS        (1ULL << kFPStatus)

/* Abstract memory locations */
#define ENCODE_DALVIK_REG       (1ULL << kDalvikReg)
#define ENCODE_LITERAL          (1ULL << kLiteral)
#define ENCODE_HEAP_REF         (1ULL << kHeapRef)
#define ENCODE_MUST_NOT_ALIAS   (1ULL << kMustNotAlias)

#define ENCODE_ALL              (~0ULL)
#define ENCODE_MEM              (ENCODE_DALVIK_REG | ENCODE_LITERAL | \
                 ENCODE_HEAP_REF | ENCODE_MUST_NOT_ALIAS)

#define DECODE_ALIAS_INFO_REG(X)        (X & 0xffff)
#define DECODE_ALIAS_INFO_WIDE(X)       ((X & 0x80000000) ? 1 : 0)

/*
 * Annotate special-purpose core registers:
 */

enum NativeRegisterPool {
  r_ZERO = 0,
  r_AT = 1,
  r_V0 = 2,
  r_V1 = 3,
  r_A0 = 4,
  r_A1 = 5,
  r_A2 = 6,
  r_A3 = 7,
  r_T0 = 8,
  r_T1 = 9,
  r_T2 = 10,
  r_T3 = 11,
  r_T4 = 12,
  r_T5 = 13,
  r_T6 = 14,
  r_T7 = 15,
  r_S0 = 16,
  r_S1 = 17,
  r_S2 = 18,
  r_S3 = 19,
  r_S4 = 20,
  r_S5 = 21,
  r_S6 = 22,
  r_S7 = 23,
  r_T8 = 24,
  r_T9 = 25,
  r_K0 = 26,
  r_K1 = 27,
  r_GP = 28,
  r_SP = 29,
  r_FP = 30,
  r_RA = 31,

  r_F0 = 0 + FP_REG_OFFSET,
  r_F1,
  r_F2,
  r_F3,
  r_F4,
  r_F5,
  r_F6,
  r_F7,
  r_F8,
  r_F9,
  r_F10,
  r_F11,
  r_F12,
  r_F13,
  r_F14,
  r_F15,
#if 0 /* only 16 fp regs supported currently */
  r_F16,
  r_F17,
  r_F18,
  r_F19,
  r_F20,
  r_F21,
  r_F22,
  r_F23,
  r_F24,
  r_F25,
  r_F26,
  r_F27,
  r_F28,
  r_F29,
  r_F30,
  r_F31,
#endif
  r_DF0 = r_F0 + FP_DOUBLE,
  r_DF1 = r_F2 + FP_DOUBLE,
  r_DF2 = r_F4 + FP_DOUBLE,
  r_DF3 = r_F6 + FP_DOUBLE,
  r_DF4 = r_F8 + FP_DOUBLE,
  r_DF5 = r_F10 + FP_DOUBLE,
  r_DF6 = r_F12 + FP_DOUBLE,
  r_DF7 = r_F14 + FP_DOUBLE,
#if 0 /* only 16 fp regs supported currently */
  r_DF8 = r_F16 + FP_DOUBLE,
  r_DF9 = r_F18 + FP_DOUBLE,
  r_DF10 = r_F20 + FP_DOUBLE,
  r_DF11 = r_F22 + FP_DOUBLE,
  r_DF12 = r_F24 + FP_DOUBLE,
  r_DF13 = r_F26 + FP_DOUBLE,
  r_DF14 = r_F28 + FP_DOUBLE,
  r_DF15 = r_F30 + FP_DOUBLE,
#endif
  r_HI = EXTRA_REG_OFFSET,
  r_LO,
  r_PC,
};

/*
 * Target-independent aliases
 */

#define rSUSPEND r_S0
#define rSELF r_S1
#define rSP r_SP
#define rARG0 r_ARG0
#define rARG1 r_ARG1
#define rARG2 r_ARG2
#define rARG3 r_ARG3
#define rRET0 r_RESULT0
#define rRET1 r_RESULT1
#define rINVOKE_TGT r_V0

/* Shift encodings */
enum MipsShiftEncodings {
  kMipsLsl = 0x0,
  kMipsLsr = 0x1,
  kMipsAsr = 0x2,
  kMipsRor = 0x3
};

// MIPS sync kinds (Note: support for kinds other than kSYNC0 may not exist)
#define kSYNC0        0x00
#define kSYNC_WMB     0x04
#define kSYNC_MB      0x01
#define kSYNC_ACQUIRE 0x11
#define kSYNC_RELEASE 0x12
#define kSYNC_RMB     0x13

// TODO: Use smaller hammer when appropriate for target CPU
#define kST kSYNC0
#define kSY kSYNC0

#define isPseudoOpcode(opCode) ((int)(opCode) < 0)

/*
 * The following enum defines the list of supported Thumb instructions by the
 * assembler. Their corresponding EncodingMap positions will be defined in
 * Assemble.cc.
 */
enum MipsOpCode {
  kPseudoExportedPC = -18,
  kPseudoSafepointPC = -17,
  kPseudoIntrinsicRetry = -16,
  kPseudoSuspendTarget = -15,
  kPseudoThrowTarget = -14,
  kPseudoCaseLabel = -13,
  kPseudoMethodEntry = -12,
  kPseudoMethodExit = -11,
  kPseudoBarrier = -10,
  kPseudoExtended = -9,
  kPseudoSSARep = -8,
  kPseudoEntryBlock = -7,
  kPseudoExitBlock = -6,
  kPseudoTargetLabel = -5,
  kPseudoDalvikByteCodeBoundary = -4,
  kPseudoPseudoAlign4 = -3,
  kPseudoEHBlockLabel = -2,
  kPseudoNormalBlockLabel = -1,

  kMipsFirst,
  kMips32BitData = kMipsFirst, /* data [31..0] */
  kMipsAddiu, /* addiu t,s,imm16 [001001] s[25..21] t[20..16] imm16[15..0] */
  kMipsAddu,  /* add d,s,t [000000] s[25..21] t[20..16] d[15..11] [00000100001] */
  kMipsAnd,   /* and d,s,t [000000] s[25..21] t[20..16] d[15..11] [00000100100] */
  kMipsAndi,  /* andi t,s,imm16 [001100] s[25..21] t[20..16] imm16[15..0] */
  kMipsB,     /* b o   [0001000000000000] o[15..0] */
  kMipsBal,   /* bal o [0000010000010001] o[15..0] */
  /* NOTE: the code tests the range kMipsBeq thru kMipsBne, so
       adding an instruction in this range may require updates */
  kMipsBeq,   /* beq s,t,o [000100] s[25..21] t[20..16] o[15..0] */
  kMipsBeqz,  /* beqz s,o [000100] s[25..21] [00000] o[15..0] */
  kMipsBgez,  /* bgez s,o [000001] s[25..21] [00001] o[15..0] */
  kMipsBgtz,  /* bgtz s,o [000111] s[25..21] [00000] o[15..0] */
  kMipsBlez,  /* blez s,o [000110] s[25..21] [00000] o[15..0] */
  kMipsBltz,  /* bltz s,o [000001] s[25..21] [00000] o[15..0] */
  kMipsBnez,  /* bnez s,o [000101] s[25..21] [00000] o[15..0] */
  kMipsBne,   /* bne s,t,o [000101] s[25..21] t[20..16] o[15..0] */
  kMipsDiv,   /* div s,t [000000] s[25..21] t[20..16] [0000000000011010] */
#if __mips_isa_rev>=2
  kMipsExt,   /* ext t,s,p,z [011111] s[25..21] t[20..16] z[15..11] p[10..6] [000000] */
#endif
  kMipsJal,   /* jal t [000011] t[25..0] */
  kMipsJalr,  /* jalr d,s [000000] s[25..21] [00000] d[15..11]
                  hint[10..6] [001001] */
  kMipsJr,    /* jr s [000000] s[25..21] [0000000000] hint[10..6] [001000] */
  kMipsLahi,  /* lui t,imm16 [00111100000] t[20..16] imm16[15..0] load addr hi */
  kMipsLalo,  /* ori t,s,imm16 [001001] s[25..21] t[20..16] imm16[15..0] load addr lo */
  kMipsLui,   /* lui t,imm16 [00111100000] t[20..16] imm16[15..0] */
  kMipsLb,    /* lb t,o(b) [100000] b[25..21] t[20..16] o[15..0] */
  kMipsLbu,   /* lbu t,o(b) [100100] b[25..21] t[20..16] o[15..0] */
  kMipsLh,    /* lh t,o(b) [100001] b[25..21] t[20..16] o[15..0] */
  kMipsLhu,   /* lhu t,o(b) [100101] b[25..21] t[20..16] o[15..0] */
  kMipsLw,    /* lw t,o(b) [100011] b[25..21] t[20..16] o[15..0] */
  kMipsMfhi,  /* mfhi d [0000000000000000] d[15..11] [00000010000] */
  kMipsMflo,  /* mflo d [0000000000000000] d[15..11] [00000010010] */
  kMipsMove,  /* move d,s [000000] s[25..21] [00000] d[15..11] [00000100101] */
  kMipsMovz,  /* movz d,s,t [000000] s[25..21] t[20..16] d[15..11] [00000001010] */
  kMipsMul,   /* mul d,s,t [011100] s[25..21] t[20..16] d[15..11] [00000000010] */
  kMipsNop,   /* nop [00000000000000000000000000000000] */
  kMipsNor,   /* nor d,s,t [000000] s[25..21] t[20..16] d[15..11] [00000100111] */
  kMipsOr,    /* or d,s,t [000000] s[25..21] t[20..16] d[15..11] [00000100101] */
  kMipsOri,   /* ori t,s,imm16 [001001] s[25..21] t[20..16] imm16[15..0] */
  kMipsPref,  /* pref h,o(b) [101011] b[25..21] h[20..16] o[15..0] */
  kMipsSb,    /* sb t,o(b) [101000] b[25..21] t[20..16] o[15..0] */
#if __mips_isa_rev>=2
  kMipsSeb,   /* seb d,t [01111100000] t[20..16] d[15..11] [10000100000] */
  kMipsSeh,   /* seh d,t [01111100000] t[20..16] d[15..11] [11000100000] */
#endif
  kMipsSh,    /* sh t,o(b) [101001] b[25..21] t[20..16] o[15..0] */
  kMipsSll,   /* sll d,t,a [00000000000] t[20..16] d[15..11] a[10..6] [000000] */
  kMipsSllv,  /* sllv d,t,s [000000] s[25..21] t[20..16] d[15..11] [00000000100] */
  kMipsSlt,   /* slt d,s,t [000000] s[25..21] t[20..16] d[15..11] [00000101010] */
  kMipsSlti,  /* slti t,s,imm16 [001010] s[25..21] t[20..16] imm16[15..0] */
  kMipsSltu,  /* sltu d,s,t [000000] s[25..21] t[20..16] d[15..11] [00000101011] */
  kMipsSra,   /* sra d,s,imm5 [00000000000] t[20..16] d[15..11] imm5[10..6] [000011] */
  kMipsSrav,  /* srav d,t,s [000000] s[25..21] t[20..16] d[15..11] [00000000111] */
  kMipsSrl,   /* srl d,t,a [00000000000] t[20..16] d[20..16] a[10..6] [000010] */
  kMipsSrlv,  /* srlv d,t,s [000000] s[25..21] t[20..16] d[15..11] [00000000110] */
  kMipsSubu,  /* subu d,s,t [000000] s[25..21] t[20..16] d[15..11] [00000100011] */
  kMipsSw,    /* sw t,o(b) [101011] b[25..21] t[20..16] o[15..0] */
  kMipsXor,   /* xor d,s,t [000000] s[25..21] t[20..16] d[15..11] [00000100110] */
  kMipsXori,  /* xori t,s,imm16 [001110] s[25..21] t[20..16] imm16[15..0] */
#ifdef __mips_hard_float
  kMipsFadds, /* add.s d,s,t [01000110000] t[20..16] s[15..11] d[10..6] [000000] */
  kMipsFsubs, /* sub.s d,s,t [01000110000] t[20..16] s[15..11] d[10..6] [000001] */
  kMipsFmuls, /* mul.s d,s,t [01000110000] t[20..16] s[15..11] d[10..6] [000010] */
  kMipsFdivs, /* div.s d,s,t [01000110000] t[20..16] s[15..11] d[10..6] [000011] */
  kMipsFaddd, /* add.d d,s,t [01000110001] t[20..16] s[15..11] d[10..6] [000000] */
  kMipsFsubd, /* sub.d d,s,t [01000110001] t[20..16] s[15..11] d[10..6] [000001] */
  kMipsFmuld, /* mul.d d,s,t [01000110001] t[20..16] s[15..11] d[10..6] [000010] */
  kMipsFdivd, /* div.d d,s,t [01000110001] t[20..16] s[15..11] d[10..6] [000011] */
  kMipsFcvtsd,/* cvt.s.d d,s [01000110001] [00000] s[15..11] d[10..6] [100000] */
  kMipsFcvtsw,/* cvt.s.w d,s [01000110100] [00000] s[15..11] d[10..6] [100000] */
  kMipsFcvtds,/* cvt.d.s d,s [01000110000] [00000] s[15..11] d[10..6] [100001] */
  kMipsFcvtdw,/* cvt.d.w d,s [01000110100] [00000] s[15..11] d[10..6] [100001] */
  kMipsFcvtws,/* cvt.w.d d,s [01000110000] [00000] s[15..11] d[10..6] [100100] */
  kMipsFcvtwd,/* cvt.w.d d,s [01000110001] [00000] s[15..11] d[10..6] [100100] */
  kMipsFmovs, /* mov.s d,s [01000110000] [00000] s[15..11] d[10..6] [000110] */
  kMipsFmovd, /* mov.d d,s [01000110001] [00000] s[15..11] d[10..6] [000110] */
  kMipsFlwc1, /* lwc1 t,o(b) [110001] b[25..21] t[20..16] o[15..0] */
  kMipsFldc1, /* ldc1 t,o(b) [110101] b[25..21] t[20..16] o[15..0] */
  kMipsFswc1, /* swc1 t,o(b) [111001] b[25..21] t[20..16] o[15..0] */
  kMipsFsdc1, /* sdc1 t,o(b) [111101] b[25..21] t[20..16] o[15..0] */
  kMipsMfc1,  /* mfc1 t,s [01000100000] t[20..16] s[15..11] [00000000000] */
  kMipsMtc1,  /* mtc1 t,s [01000100100] t[20..16] s[15..11] [00000000000] */
#endif
  kMipsDelta, /* Psuedo for ori t, s, <label>-<label> */
  kMipsDeltaHi, /* Pseudo for lui t, high16(<label>-<label>) */
  kMipsDeltaLo, /* Pseudo for ori t, s, low16(<label>-<label>) */
  kMipsCurrPC,  /* jal to .+8 to materialize pc */
  kMipsSync,  /* sync kind [000000] [0000000000000000] s[10..6] [001111] */
  kMipsUndefined,  /* undefined [011001xxxxxxxxxxxxxxxx] */
  kMipsLast
};

/* Bit flags describing the behavior of each native opcode */
enum MipsOpFeatureFlags {
  kIsBranch = 0,
  kRegDef0,
  kRegDef1,
  kRegDefSP,
  kRegDefLR,
  kRegDefList0,
  kRegDefList1,
  kRegUse0,
  kRegUse1,
  kRegUse2,
  kRegUse3,
  kRegUseSP,
  kRegUsePC,
  kRegUseList0,
  kRegUseList1,
  kNoOperand,
  kIsUnaryOp,
  kIsBinaryOp,
  kIsTertiaryOp,
  kIsQuadOp,
  kIsIT,
  kSetsCCodes,
  kUsesCCodes,
  kMemLoad,
  kMemStore,
  kPCRelFixup,
  kRegUseLR,
};

#define IS_LOAD         (1 << kMemLoad)
#define IS_STORE        (1 << kMemStore)
#define IS_BRANCH       (1 << kIsBranch)
#define REG_DEF0        (1 << kRegDef0)
#define REG_DEF1        (1 << kRegDef1)
#define REG_DEF_SP      (1 << kRegDefSP)
#define REG_DEF_LR      (1 << kRegDefLR)
#define REG_DEF_LIST0   (1 << kRegDefList0)
#define REG_DEF_LIST1   (1 << kRegDefList1)
#define REG_USE0        (1 << kRegUse0)
#define REG_USE1        (1 << kRegUse1)
#define REG_USE2        (1 << kRegUse2)
#define REG_USE3        (1 << kRegUse3)
#define REG_USE_SP      (1 << kRegUseSP)
#define REG_USE_PC      (1 << kRegUsePC)
#define REG_USE_LIST0   (1 << kRegUseList0)
#define REG_USE_LIST1   (1 << kRegUseList1)
#define NO_OPERAND      (1 << kNoOperand)
#define IS_UNARY_OP     (1 << kIsUnaryOp)
#define IS_BINARY_OP    (1 << kIsBinaryOp)
#define IS_TERTIARY_OP  (1 << kIsTertiaryOp)
#define IS_QUAD_OP      (1 << kIsQuadOp)
#define IS_QUIN_OP      0
#define IS_IT           (1 << kIsIT)
#define SETS_CCODES     (1 << kSetsCCodes)
#define USES_CCODES     (1 << kUsesCCodes)
#define NEEDS_FIXUP     (1 << kPCRelFixup)
#define REG_USE_LR      (1 << kRegUseLR)

/*  attributes, included for compatibility */
#define REG_DEF_FPCS_LIST0   (0)
#define REG_DEF_FPCS_LIST2   (0)


/* Common combo register usage patterns */
#define REG_USE01       (REG_USE0 | REG_USE1)
#define REG_USE02       (REG_USE0 | REG_USE2)
#define REG_USE012      (REG_USE01 | REG_USE2)
#define REG_USE12       (REG_USE1 | REG_USE2)
#define REG_USE23       (REG_USE2 | REG_USE3)
#define REG_DEF01       (REG_DEF0 | REG_DEF1)
#define REG_DEF0_USE0   (REG_DEF0 | REG_USE0)
#define REG_DEF0_USE1   (REG_DEF0 | REG_USE1)
#define REG_DEF0_USE2   (REG_DEF0 | REG_USE2)
#define REG_DEF0_USE01  (REG_DEF0 | REG_USE01)
#define REG_DEF0_USE12  (REG_DEF0 | REG_USE12)
#define REG_DEF01_USE2  (REG_DEF0 | REG_DEF1 | REG_USE2)

/* Instruction assembly fieldLoc kind */
enum MipsEncodingKind {
  kFmtUnused,
  kFmtBitBlt,    /* Bit string using end/start */
  kFmtDfp,       /* Double FP reg */
  kFmtSfp,       /* Single FP reg */
  kFmtBlt5_2,    /* Same 5-bit field to 2 locations */
};

/* Struct used to define the snippet positions for each MIPS opcode */
struct MipsEncodingMap {
  u4 skeleton;
  struct {
    MipsEncodingKind kind;
    int end;   /* end for kFmtBitBlt, 1-bit slice end for FP regs */
    int start; /* start for kFmtBitBlt, 4-bit slice end for FP regs */
  } fieldLoc[4];
  MipsOpCode opcode;
  int flags;
  const char *name;
  const char* fmt;
  int size;   /* Size in bytes */
};

/* Keys for target-specific scheduling and other optimization hints */
enum MipsTargetOptHints {
  kMaxHoistDistance,
};

extern MipsEncodingMap EncodingMap[kMipsLast];

#define IS_UIMM16(v) ((0 <= (v)) && ((v) <= 65535))
#define IS_SIMM16(v) ((-32768 <= (v)) && ((v) <= 32766))
#define IS_SIMM16_2WORD(v) ((-32764 <= (v)) && ((v) <= 32763)) /* 2 offsets must fit */

}  // namespace art

#endif  // ART_COMPILER_COMPILER_CODEGEN_MIPS_MIPSLIR_H_
