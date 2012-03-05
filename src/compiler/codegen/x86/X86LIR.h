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

#ifndef ART_COMPILER_COMPILER_CODEGEN_X86_X86LIR_H_
#define ART_COMPILER_COMPILER_CODEGEN_X86_X86LIR_H_

#include "../../Dalvik.h"
#include "../../CompilerInternals.h"

namespace art {

// Set to 1 to measure cost of suspend check
#define NO_SUSPEND 0

/*
 * Runtime register conventions. We consider both x86, x86-64 and x32 (32bit mode x86-64), although
 * we currently only target x86. The ABI has different conventions and we hope to have a single
 * convention to simplify code generation. Changing something that is callee save and making it
 * caller save places a burden on up-calls to save/restore the callee save register, however, there
 * are few registers that are callee save in the ABI. Changing something that is caller save and
 * making it callee save places a burden on down-calls to save/restore the callee save register.
 * For these reasons we aim to match native conventions for caller and callee save
 *
 * General Purpose Register:
 *  Native: x86         | x86-64 / x32      | ART
 *  r0/eax: caller save | caller save       | caller, Method*, scratch, return value
 *  r1/ecx: caller save | caller save, arg4 | caller, arg2, scratch
 *  r2/edx: caller save | caller save, arg3 | caller, arg1, scratch, high half of long return
 *  r3/ebx: callee save | callee save       | callee, available for dalvik register promotion
 *  r4/esp: stack pointer
 *  r5/ebp: callee save | callee save       | callee, available for dalvik register promotion
 *  r6/esi: callEE save | callER save, arg2 | callee, available for dalvik register promotion
 *  r7/edi: callEE save | callER save, arg1 | callee, available for dalvik register promotion
 *  ---  x86-64/x32 registers
 *  Native: x86-64 / x32      | ART
 *  r8:     caller save, arg5 | caller, scratch
 *  r9:     caller save, arg6 | caller, scratch
 *  r10:    caller save       | caller, scratch
 *  r11:    caller save       | caller, scratch
 *  r12:    callee save       | callee, available for dalvik register promotion
 *  r13:    callee save       | callee, available for dalvik register promotion
 *  r14:    callee save       | callee, available for dalvik register promotion
 *  r15:    callee save       | callee, available for dalvik register promotion
 *
 * There is no rSELF, instead on x86 fs: has a base address of Thread::Current, whereas on
 * x86-64/x32 gs: holds it.
 *
 * For floating point we don't support CPUs without SSE2 support (ie newer than PIII):
 *  Native: x86       | x86-64 / x32     | ART
 *  XMM0: caller save |caller save, arg1 | caller, float/double return value (except for native x86 code)
 *  XMM1: caller save |caller save, arg2 | caller, scratch
 *  XMM2: caller save |caller save, arg3 | caller, scratch
 *  XMM3: caller save |caller save, arg4 | caller, scratch
 *  XMM4: caller save |caller save, arg5 | caller, scratch
 *  XMM5: caller save |caller save, arg6 | caller, scratch
 *  XMM6: caller save |caller save, arg7 | caller, scratch
 *  XMM7: caller save |caller save, arg8 | caller, scratch
 *  ---  x86-64/x32 registers
 *  XMM8 .. 15: caller save
 *
 * X87 is a necessary evil outside of ART code:
 *  ST0:  x86 float/double native return value, caller save
 *  ST1 .. ST7: caller save
 *
 *  Stack frame diagram (stack grows down, higher addresses at top):
 *
 * +------------------------+
 * | IN[ins-1]              |  {Note: resides in caller's frame}
 * |       .                |
 * | IN[0]                  |
 * | caller's Method*       |
 * +========================+  {Note: start of callee's frame}
 * | return address         |  {pushed by call}
 * | spill region           |  {variable sized}
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
#define FP_REG_OFFSET 16
/* Offset to distinguish DP FP regs */
#define FP_DOUBLE 32
/* Offset to distingish the extra regs */
#define EXTRA_REG_OFFSET 64
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


typedef enum ResourceEncodingPos {
    kGPReg0     = 0,
    kRegSP      = 4,
    kRegLR      = -1,
    kFPReg0     = 16,  // xmm0 .. xmm7/xmm15
    kFPRegEnd   = 32,
    kRegEnd     = kFPRegEnd,
    kCCode      = kRegEnd,
    // The following four bits are for memory disambiguation
    kDalvikReg,         // 1 Dalvik Frame (can be fully disambiguated)
    kLiteral,           // 2 Literal pool (can be fully disambiguated)
    kHeapRef,           // 3 Somewhere on the heap (alias with any other heap)
    kMustNotAlias,      // 4 Guaranteed to be non-alias (eg *(r6+x))
} ResourceEncodingPos;

#define ENCODE_REG_LIST(N)      ((u8) N)
#define ENCODE_REG_SP           (1ULL << kRegSP)
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

typedef enum NativeRegisterPool {
  r0     = 0,
  rAX   = r0,
  r1     = 1,
  rCX   = r1,
  r2     = 2,
  rDX    = r2,
  r3     = 3,
  rBX    = r3,
  r4sp   = 4,
  rSP    =r4sp,
  r5     = 5,
  rBP    = r5,
  r6     = 6,
  rSI    = r6,
  r7     = 7,
  rDI    = r7,
  r8     = 8,
  r9     = 9,
  r10    = 10,
  r11    = 11,
  r12    = 12,
  r13    = 13,
  r14    = 14,
  r15    = 15,
  fr0  =  0 + FP_REG_OFFSET,
  fr1  =  1 + FP_REG_OFFSET,
  fr2  =  2 + FP_REG_OFFSET,
  fr3  =  3 + FP_REG_OFFSET,
  fr4  =  4 + FP_REG_OFFSET,
  fr5  =  5 + FP_REG_OFFSET,
  fr6  =  6 + FP_REG_OFFSET,
  fr7  =  7 + FP_REG_OFFSET,
  fr8  =  8 + FP_REG_OFFSET,
  fr9  =  9 + FP_REG_OFFSET,
  fr10 = 10 + FP_REG_OFFSET,
  fr11 = 11 + FP_REG_OFFSET,
  fr12 = 12 + FP_REG_OFFSET,
  fr13 = 13 + FP_REG_OFFSET,
  fr14 = 14 + FP_REG_OFFSET,
  fr15 = 15 + FP_REG_OFFSET,
} NativeRegisterPool;

/*
 * Target-independent aliases
 */

#define rARG0 rAX
#define rARG1 rDX
#define rARG2 rCX
#define rRET0 rAX
#define rRET1 rDX

#define isPseudoOpcode(opCode) ((int)(opCode) < 0)

/*
 * The following enum defines the list of supported Thumb instructions by the
 * assembler. Their corresponding snippet positions will be defined in
 * Assemble.c.
 */
typedef enum X86OpCode {
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
    kOpAddRR,  // add reg, reg
    kOpAddRM,  // add reg, [reg + displacement]
    kOpAddMR,  // add [reg + displacement], reg
    kOpAddRI,  // add reg, #immediate
    kOpAddMI,  // add [reg + displacement], #immediate
    kOpAddRA,  // add reg, [base reg + index reg * scale + displacment]
    kOpAddAR,  // add [base reg + index reg * scale + displacment], reg
    kOpAddAI,  // add [base reg + index reg * scale + displacment], #immediate
    kX86First,
    kX86Last
} X86OpCode;

/* Bit flags describing the behavior of each native opcode */
typedef enum X86OpFeatureFlags {
    kIsBranch = 0,
    kRegDef0,
    kRegDef1,
    kRegDefSP,
    kRegDefList0,
    kRegDefList1,
    kRegUse0,
    kRegUse1,
    kRegUse2,
    kRegUse3,
    kRegUseSP,
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
// FIXME: add NEEDS_FIXUP to instruction attributes
} X86OpFeatureFlags;

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
#define IS_IT           (1 << kIsIT)
#define SETS_CCODES     (1 << kSetsCCodes)
#define USES_CCODES     (1 << kUsesCCodes)
#define NEEDS_FIXUP      (1 << kPCRelFixup)

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
typedef enum X86EncodingKind {
    kFmtUnused,
    kFmtBitBlt,        /* Bit string using end/start */
    kFmtDfp,           /* Double FP reg */
    kFmtSfp,           /* Single FP reg */
} X86EncodingKind;

/* Struct used to define the snippet positions for each X86 opcode */
typedef struct X86EncodingMap {
    X86OpCode opcode;
    int flags;
    const char *name;
    const char* fmt;
    int size;     /* Size in bytes */
} X86EncodingMap;

/* Keys for target-specific scheduling and other optimization hints */
typedef enum X86TargetOptHints {
    kMaxHoistDistance,
} X86TargetOptHints;

extern X86EncodingMap EncodingMap[kX86Last];

#define IS_UIMM16(v) ((0 <= (v)) && ((v) <= 65535))
#define IS_SIMM16(v) ((-32768 <= (v)) && ((v) <= 32766))
#define IS_SIMM16_2WORD(v) ((-32764 <= (v)) && ((v) <= 32763)) /* 2 offsets must fit */

}  // namespace art

#endif  // ART_COMPILER_COMPILER_CODEGEN_X86_X86LIR_H_
