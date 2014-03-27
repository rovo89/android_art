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

#ifndef ART_COMPILER_DEX_QUICK_X86_X86_LIR_H_
#define ART_COMPILER_DEX_QUICK_X86_X86_LIR_H_

#include "dex/compiler_internals.h"

namespace art {

/*
 * Runtime register conventions. We consider both x86, x86-64 and x32 (32bit mode x86-64). The ABI
 * has different conventions and we capture those here. Changing something that is callee save and
 * making it caller save places a burden on up-calls to save/restore the callee save register,
 * however, there are few registers that are callee save in the ABI. Changing something that is
 * caller save and making it callee save places a burden on down-calls to save/restore the callee
 * save register. For these reasons we aim to match native conventions for caller and callee save.
 * On x86 only the first 4 registers can be used for byte operations, for this reason they are
 * preferred for temporary scratch registers.
 *
 * General Purpose Register:
 *  Native: x86    | x86-64 / x32 | ART x86                                         | ART x86-64
 *  r0/eax: caller | caller       | caller, Method*, scratch, return value          | caller, scratch, return value
 *  r1/ecx: caller | caller, arg4 | caller, arg1, scratch                           | caller, arg3, scratch
 *  r2/edx: caller | caller, arg3 | caller, arg2, scratch, high half of long return | caller, arg2, scratch
 *  r3/ebx: callEE | callEE       | callER, arg3, scratch                           | callee, promotable
 *  r4/esp: stack pointer
 *  r5/ebp: callee | callee       | callee, promotable                              | callee, promotable
 *  r6/esi: callEE | callER, arg2 | callee, promotable                              | caller, arg1, scratch
 *  r7/edi: callEE | callER, arg1 | callee, promotable                              | caller, Method*, scratch
 *  ---  x86-64/x32 registers
 *  Native: x86-64 / x32      | ART
 *  r8:     caller save, arg5 | caller, arg4, scratch
 *  r9:     caller save, arg6 | caller, arg5, scratch
 *  r10:    caller save       | caller, scratch
 *  r11:    caller save       | caller, scratch
 *  r12:    callee save       | callee, available for register promotion (promotable)
 *  r13:    callee save       | callee, available for register promotion (promotable)
 *  r14:    callee save       | callee, available for register promotion (promotable)
 *  r15:    callee save       | callee, available for register promotion (promotable)
 *
 * There is no rSELF, instead on x86 fs: has a base address of Thread::Current, whereas on
 * x86-64/x32 gs: holds it.
 *
 * For floating point we don't support CPUs without SSE2 support (ie newer than PIII):
 *  Native: x86  | x86-64 / x32 | ART x86                    | ART x86-64
 *  XMM0: caller | caller, arg1 | caller, float return value | caller, arg1, float return value
 *  XMM1: caller | caller, arg2 | caller, scratch            | caller, arg2, scratch
 *  XMM2: caller | caller, arg3 | caller, scratch            | caller, arg3, scratch
 *  XMM3: caller | caller, arg4 | caller, scratch            | caller, arg4, scratch
 *  XMM4: caller | caller, arg5 | caller, scratch            | caller, arg5, scratch
 *  XMM5: caller | caller, arg6 | caller, scratch            | caller, arg6, scratch
 *  XMM6: caller | caller, arg7 | caller, scratch            | caller, arg7, scratch
 *  XMM7: caller | caller, arg8 | caller, scratch            | caller, arg8, scratch
 *  ---  x86-64/x32 registers
 *  XMM8 .. 15: caller save available as scratch registers for ART.
 *
 * X87 is a necessary evil outside of ART code for x86:
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
 * | cur_method*            | <<== sp w/ 16-byte alignment
 * +========================+
 */

// Offset to distingish FP regs.
#define X86_FP_REG_OFFSET 32
// Offset to distinguish DP FP regs.
#define X86_FP_DOUBLE (X86_FP_REG_OFFSET + 16)
// Offset to distingish the extra regs.
#define X86_EXTRA_REG_OFFSET (X86_FP_DOUBLE + 16)
// Reg types.
#define X86_REGTYPE(x) (x & (X86_FP_REG_OFFSET | X86_FP_DOUBLE))
#define X86_FPREG(x) ((x & X86_FP_REG_OFFSET) == X86_FP_REG_OFFSET)
#define X86_EXTRAREG(x) ((x & X86_EXTRA_REG_OFFSET) == X86_EXTRA_REG_OFFSET)
#define X86_DOUBLEREG(x) ((x & X86_FP_DOUBLE) == X86_FP_DOUBLE)
#define X86_SINGLEREG(x) (X86_FPREG(x) && !X86_DOUBLEREG(x))

/*
 * Note: the low register of a floating point pair is sufficient to
 * create the name of a double, but require both names to be passed to
 * allow for asserts to verify that the pair is consecutive if significant
 * rework is done in this area.  Also, it is a good reminder in the calling
 * code that reg locations always describe doubles as a pair of singles.
 */
#define X86_S2D(x, y) ((x) | X86_FP_DOUBLE)
/* Mask to strip off fp flags */
#define X86_FP_REG_MASK 0xF

enum X86ResourceEncodingPos {
  kX86GPReg0   = 0,
  kX86RegSP    = 4,
  kX86FPReg0   = 16,  // xmm0 .. xmm7/xmm15.
  kX86FPRegEnd = 32,
  kX86FPStack  = 33,
  kX86RegEnd   = kX86FPStack,
};

#define ENCODE_X86_REG_LIST(N)      (static_cast<uint64_t>(N))
#define ENCODE_X86_REG_SP           (1ULL << kX86RegSP)
#define ENCODE_X86_FP_STACK         (1ULL << kX86FPStack)

enum X86NativeRegisterPool {
  r0     = 0,
  rAX    = r0,
  r1     = 1,
  rCX    = r1,
  r2     = 2,
  rDX    = r2,
  r3     = 3,
  rBX    = r3,
  r4sp   = 4,
  rX86_SP    = r4sp,
  r4sib_no_index = r4sp,
  r5     = 5,
  rBP    = r5,
  r5sib_no_base = r5,
  r6     = 6,
  rSI    = r6,
  r7     = 7,
  rDI    = r7,
#ifndef TARGET_REX_SUPPORT
  rRET   = 8,  // fake return address register for core spill mask.
#else
  r8     = 8,
  r9     = 9,
  r10    = 10,
  r11    = 11,
  r12    = 12,
  r13    = 13,
  r14    = 14,
  r15    = 15,
  rRET   = 16,  // fake return address register for core spill mask.
#endif
  fr0  =  0 + X86_FP_REG_OFFSET,
  fr1  =  1 + X86_FP_REG_OFFSET,
  fr2  =  2 + X86_FP_REG_OFFSET,
  fr3  =  3 + X86_FP_REG_OFFSET,
  fr4  =  4 + X86_FP_REG_OFFSET,
  fr5  =  5 + X86_FP_REG_OFFSET,
  fr6  =  6 + X86_FP_REG_OFFSET,
  fr7  =  7 + X86_FP_REG_OFFSET,
  fr8  =  8 + X86_FP_REG_OFFSET,
  fr9  =  9 + X86_FP_REG_OFFSET,
  fr10 = 10 + X86_FP_REG_OFFSET,
  fr11 = 11 + X86_FP_REG_OFFSET,
  fr12 = 12 + X86_FP_REG_OFFSET,
  fr13 = 13 + X86_FP_REG_OFFSET,
  fr14 = 14 + X86_FP_REG_OFFSET,
  fr15 = 15 + X86_FP_REG_OFFSET,
};

const RegStorage rs_r0(RegStorage::k32BitSolo, r0);
const RegStorage rs_rAX = rs_r0;
const RegStorage rs_r1(RegStorage::k32BitSolo, r1);
const RegStorage rs_rCX = rs_r1;
const RegStorage rs_r2(RegStorage::k32BitSolo, r2);
const RegStorage rs_rDX = rs_r2;
const RegStorage rs_r3(RegStorage::k32BitSolo, r3);
const RegStorage rs_rBX = rs_r3;
const RegStorage rs_r4sp(RegStorage::k32BitSolo, r4sp);
const RegStorage rs_rX86_SP = rs_r4sp;
const RegStorage rs_r5(RegStorage::k32BitSolo, r5);
const RegStorage rs_rBP = rs_r5;
const RegStorage rs_r6(RegStorage::k32BitSolo, r6);
const RegStorage rs_rSI = rs_r6;
const RegStorage rs_r7(RegStorage::k32BitSolo, r7);
const RegStorage rs_rDI = rs_r7;

// TODO: elminate these #defines?
#define rX86_ARG0 rAX
#define rs_rX86_ARG0 rs_rAX
#define rX86_ARG1 rCX
#define rs_rX86_ARG1 rs_rCX
#define rX86_ARG2 rDX
#define rs_rX86_ARG2 rs_rDX
#define rX86_ARG3 rBX
#define rs_rX86_ARG3 rs_rBX
#define rX86_FARG0 rAX
#define rs_rX86_FARG0 rs_rAX
#define rX86_FARG1 rCX
#define rs_rX86_FARG1 rs_rCX
#define rX86_FARG2 rDX
#define rs_rX86_FARG2 rs_rDX
#define rX86_FARG3 rBX
#define rs_rX86_FARG3 rs_rBX
#define rX86_RET0 rAX
#define rs_rX86_RET0 rs_rAX
#define rX86_RET1 rDX
#define rs_rX86_RET1 rs_rDX
#define rX86_INVOKE_TGT rAX
#define rs_rX86_INVOKE_TGT rs_rAX
#define rX86_LR RegStorage::kInvalidRegVal
#define rX86_SUSPEND RegStorage::kInvalidRegVal
#define rX86_SELF RegStorage::kInvalidRegVal
#define rX86_COUNT rCX
#define rs_rX86_COUNT rs_rCX
#define rX86_PC RegStorage::kInvalidRegVal

// RegisterLocation templates return values (r_V0, or r_V0/r_V1).
const RegLocation x86_loc_c_return
    {kLocPhysReg, 0, 0, 0, 0, 0, 0, 0, 1, kVectorNotUsed,
     RegStorage(RegStorage::k32BitSolo, rAX), INVALID_SREG, INVALID_SREG};
const RegLocation x86_loc_c_return_wide
    {kLocPhysReg, 1, 0, 0, 0, 0, 0, 0, 1, kVectorNotUsed,
     RegStorage(RegStorage::k64BitPair, rAX, rDX), INVALID_SREG, INVALID_SREG};
// TODO: update to use k32BitVector (must encode in 7 bits, including fp flag).
const RegLocation x86_loc_c_return_float
    {kLocPhysReg, 0, 0, 0, 1, 0, 0, 0, 1, kVectorLength4,
     RegStorage(RegStorage::k32BitSolo, fr0), INVALID_SREG, INVALID_SREG};
// TODO: update to use k64BitVector (must encode in 7 bits, including fp flag).
const RegLocation x86_loc_c_return_double
    {kLocPhysReg, 1, 0, 0, 1, 0, 0, 0, 1, kVectorLength8,
     RegStorage(RegStorage::k64BitPair, fr0, fr0), INVALID_SREG, INVALID_SREG};

/*
 * The following enum defines the list of supported X86 instructions by the
 * assembler. Their corresponding EncodingMap positions will be defined in
 * Assemble.cc.
 */
enum X86OpCode {
  kX86First = 0,
  kX8632BitData = kX86First,  // data [31..0].
  kX86Bkpt,
  kX86Nop,
  // Define groups of binary operations
  // MR - Memory Register  - opcode [base + disp], reg
  //             - lir operands - 0: base, 1: disp, 2: reg
  // AR - Array Register   - opcode [base + index * scale + disp], reg
  //             - lir operands - 0: base, 1: index, 2: scale, 3: disp, 4: reg
  // TR - Thread Register  - opcode fs:[disp], reg - where fs: is equal to Thread::Current()
  //             - lir operands - 0: disp, 1: reg
  // RR - Register Register  - opcode reg1, reg2
  //             - lir operands - 0: reg1, 1: reg2
  // RM - Register Memory  - opcode reg, [base + disp]
  //             - lir operands - 0: reg, 1: base, 2: disp
  // RA - Register Array   - opcode reg, [base + index * scale + disp]
  //             - lir operands - 0: reg, 1: base, 2: index, 3: scale, 4: disp
  // RT - Register Thread  - opcode reg, fs:[disp] - where fs: is equal to Thread::Current()
  //             - lir operands - 0: reg, 1: disp
  // RI - Register Immediate - opcode reg, #immediate
  //             - lir operands - 0: reg, 1: immediate
  // MI - Memory Immediate   - opcode [base + disp], #immediate
  //             - lir operands - 0: base, 1: disp, 2: immediate
  // AI - Array Immediate  - opcode [base + index * scale + disp], #immediate
  //             - lir operands - 0: base, 1: index, 2: scale, 3: disp 4: immediate
  // TI - Thread Immediate  - opcode fs:[disp], imm - where fs: is equal to Thread::Current()
  //             - lir operands - 0: disp, 1: imm
#define BinaryOpCode(opcode) \
  opcode ## 8MR, opcode ## 8AR, opcode ## 8TR, \
  opcode ## 8RR, opcode ## 8RM, opcode ## 8RA, opcode ## 8RT, \
  opcode ## 8RI, opcode ## 8MI, opcode ## 8AI, opcode ## 8TI, \
  opcode ## 16MR, opcode ## 16AR, opcode ## 16TR, \
  opcode ## 16RR, opcode ## 16RM, opcode ## 16RA, opcode ## 16RT, \
  opcode ## 16RI, opcode ## 16MI, opcode ## 16AI, opcode ## 16TI, \
  opcode ## 16RI8, opcode ## 16MI8, opcode ## 16AI8, opcode ## 16TI8, \
  opcode ## 32MR, opcode ## 32AR, opcode ## 32TR,  \
  opcode ## 32RR, opcode ## 32RM, opcode ## 32RA, opcode ## 32RT, \
  opcode ## 32RI, opcode ## 32MI, opcode ## 32AI, opcode ## 32TI, \
  opcode ## 32RI8, opcode ## 32MI8, opcode ## 32AI8, opcode ## 32TI8
  BinaryOpCode(kX86Add),
  BinaryOpCode(kX86Or),
  BinaryOpCode(kX86Adc),
  BinaryOpCode(kX86Sbb),
  BinaryOpCode(kX86And),
  BinaryOpCode(kX86Sub),
  BinaryOpCode(kX86Xor),
  BinaryOpCode(kX86Cmp),
#undef BinaryOpCode
  kX86Imul16RRI, kX86Imul16RMI, kX86Imul16RAI,
  kX86Imul32RRI, kX86Imul32RMI, kX86Imul32RAI,
  kX86Imul32RRI8, kX86Imul32RMI8, kX86Imul32RAI8,
  kX86Mov8MR, kX86Mov8AR, kX86Mov8TR,
  kX86Mov8RR, kX86Mov8RM, kX86Mov8RA, kX86Mov8RT,
  kX86Mov8RI, kX86Mov8MI, kX86Mov8AI, kX86Mov8TI,
  kX86Mov16MR, kX86Mov16AR, kX86Mov16TR,
  kX86Mov16RR, kX86Mov16RM, kX86Mov16RA, kX86Mov16RT,
  kX86Mov16RI, kX86Mov16MI, kX86Mov16AI, kX86Mov16TI,
  kX86Mov32MR, kX86Mov32AR, kX86Mov32TR,
  kX86Mov32RR, kX86Mov32RM, kX86Mov32RA, kX86Mov32RT,
  kX86Mov32RI, kX86Mov32MI, kX86Mov32AI, kX86Mov32TI,
  kX86Lea32RM,
  kX86Lea32RA,
  // RRC - Register Register ConditionCode - cond_opcode reg1, reg2
  //             - lir operands - 0: reg1, 1: reg2, 2: CC
  kX86Cmov32RRC,
  // RC - Register CL - opcode reg, CL
  //          - lir operands - 0: reg, 1: CL
  // MC - Memory CL   - opcode [base + disp], CL
  //          - lir operands - 0: base, 1: disp, 2: CL
  // AC - Array CL  - opcode [base + index * scale + disp], CL
  //          - lir operands - 0: base, 1: index, 2: scale, 3: disp, 4: CL
#define BinaryShiftOpCode(opcode) \
  opcode ## 8RI, opcode ## 8MI, opcode ## 8AI, \
  opcode ## 8RC, opcode ## 8MC, opcode ## 8AC, \
  opcode ## 16RI, opcode ## 16MI, opcode ## 16AI, \
  opcode ## 16RC, opcode ## 16MC, opcode ## 16AC, \
  opcode ## 32RI, opcode ## 32MI, opcode ## 32AI, \
  opcode ## 32RC, opcode ## 32MC, opcode ## 32AC
  BinaryShiftOpCode(kX86Rol),
  BinaryShiftOpCode(kX86Ror),
  BinaryShiftOpCode(kX86Rcl),
  BinaryShiftOpCode(kX86Rcr),
  BinaryShiftOpCode(kX86Sal),
  BinaryShiftOpCode(kX86Shr),
  BinaryShiftOpCode(kX86Sar),
#undef BinaryShiftOpcode
  kX86Cmc,
  kX86Shld32RRI,
  kX86Shrd32RRI,
#define UnaryOpcode(opcode, reg, mem, array) \
  opcode ## 8 ## reg, opcode ## 8 ## mem, opcode ## 8 ## array, \
  opcode ## 16 ## reg, opcode ## 16 ## mem, opcode ## 16 ## array, \
  opcode ## 32 ## reg, opcode ## 32 ## mem, opcode ## 32 ## array
  UnaryOpcode(kX86Test, RI, MI, AI),
  kX86Test32RR,
  UnaryOpcode(kX86Not, R, M, A),
  UnaryOpcode(kX86Neg, R, M, A),
  UnaryOpcode(kX86Mul,  DaR, DaM, DaA),
  UnaryOpcode(kX86Imul, DaR, DaM, DaA),
  UnaryOpcode(kX86Divmod,  DaR, DaM, DaA),
  UnaryOpcode(kX86Idivmod, DaR, DaM, DaA),
  kx86Cdq32Da,
  kX86Bswap32R,
  kX86Push32R, kX86Pop32R,
#undef UnaryOpcode
#define Binary0fOpCode(opcode) \
  opcode ## RR, opcode ## RM, opcode ## RA
  Binary0fOpCode(kX86Movsd),
  kX86MovsdMR,
  kX86MovsdAR,
  Binary0fOpCode(kX86Movss),
  kX86MovssMR,
  kX86MovssAR,
  Binary0fOpCode(kX86Cvtsi2sd),  // int to double
  Binary0fOpCode(kX86Cvtsi2ss),  // int to float
  Binary0fOpCode(kX86Cvttsd2si),  // truncating double to int
  Binary0fOpCode(kX86Cvttss2si),  // truncating float to int
  Binary0fOpCode(kX86Cvtsd2si),  // rounding double to int
  Binary0fOpCode(kX86Cvtss2si),  // rounding float to int
  Binary0fOpCode(kX86Ucomisd),  // unordered double compare
  Binary0fOpCode(kX86Ucomiss),  // unordered float compare
  Binary0fOpCode(kX86Comisd),   // double compare
  Binary0fOpCode(kX86Comiss),   // float compare
  Binary0fOpCode(kX86Orps),     // or of floating point registers
  Binary0fOpCode(kX86Xorps),    // xor of floating point registers
  Binary0fOpCode(kX86Addsd),    // double add
  Binary0fOpCode(kX86Addss),    // float add
  Binary0fOpCode(kX86Mulsd),    // double multiply
  Binary0fOpCode(kX86Mulss),    // float multiply
  Binary0fOpCode(kX86Cvtsd2ss),  // double to float
  Binary0fOpCode(kX86Cvtss2sd),  // float to double
  Binary0fOpCode(kX86Subsd),    // double subtract
  Binary0fOpCode(kX86Subss),    // float subtract
  Binary0fOpCode(kX86Divsd),    // double divide
  Binary0fOpCode(kX86Divss),    // float divide
  Binary0fOpCode(kX86Punpckldq),  // Interleave low-order double words
  kX86PsrlqRI,                  // right shift of floating point registers
  kX86PsllqRI,                  // left shift of floating point registers
  kX86SqrtsdRR,                 // sqrt of floating point register
  kX86Fild32M,                  // push 32-bit integer on x87 stack
  kX86Fild64M,                  // push 64-bit integer on x87 stack
  kX86Fstp32M,                  // pop top x87 fp stack and do 32-bit store
  kX86Fstp64M,                  // pop top x87 fp stack and do 64-bit store
  Binary0fOpCode(kX86Movups),   // load unaligned packed single FP values from xmm2/m128 to xmm1
  kX86MovupsMR, kX86MovupsAR,   // store unaligned packed single FP values from xmm1 to m128
  Binary0fOpCode(kX86Movaps),   // load aligned packed single FP values from xmm2/m128 to xmm1
  kX86MovapsMR, kX86MovapsAR,   // store aligned packed single FP values from xmm1 to m128
  kX86MovlpsRM, kX86MovlpsRA,   // load packed single FP values from m64 to low quadword of xmm
  kX86MovlpsMR, kX86MovlpsAR,   // store packed single FP values from low quadword of xmm to m64
  kX86MovhpsRM, kX86MovhpsRA,   // load packed single FP values from m64 to high quadword of xmm
  kX86MovhpsMR, kX86MovhpsAR,   // store packed single FP values from high quadword of xmm to m64
  Binary0fOpCode(kX86Movdxr),   // move into xmm from gpr
  kX86MovdrxRR, kX86MovdrxMR, kX86MovdrxAR,  // move into reg from xmm
  kX86Set8R, kX86Set8M, kX86Set8A,  // set byte depending on condition operand
  kX86Mfence,                   // memory barrier
  Binary0fOpCode(kX86Imul16),   // 16bit multiply
  Binary0fOpCode(kX86Imul32),   // 32bit multiply
  kX86CmpxchgRR, kX86CmpxchgMR, kX86CmpxchgAR,  // compare and exchange
  kX86LockCmpxchgMR, kX86LockCmpxchgAR,  // locked compare and exchange
  kX86LockCmpxchg8bM, kX86LockCmpxchg8bA,  // locked compare and exchange
  kX86XchgMR,  // exchange memory with register (automatically locked)
  Binary0fOpCode(kX86Movzx8),   // zero-extend 8-bit value
  Binary0fOpCode(kX86Movzx16),  // zero-extend 16-bit value
  Binary0fOpCode(kX86Movsx8),   // sign-extend 8-bit value
  Binary0fOpCode(kX86Movsx16),  // sign-extend 16-bit value
#undef Binary0fOpCode
  kX86Jcc8, kX86Jcc32,  // jCC rel8/32; lir operands - 0: rel, 1: CC, target assigned
  kX86Jmp8, kX86Jmp32,  // jmp rel8/32; lir operands - 0: rel, target assigned
  kX86JmpR,             // jmp reg; lir operands - 0: reg
  kX86Jecxz8,           // jcexz rel8; jump relative if ECX is zero.
  kX86JmpT,             // jmp fs:[disp]; fs: is equal to Thread::Current(); lir operands - 0: disp

  kX86CallR,            // call reg; lir operands - 0: reg
  kX86CallM,            // call [base + disp]; lir operands - 0: base, 1: disp
  kX86CallA,            // call [base + index * scale + disp]
                        // lir operands - 0: base, 1: index, 2: scale, 3: disp
  kX86CallT,            // call fs:[disp]; fs: is equal to Thread::Current(); lir operands - 0: disp
  kX86CallI,            // call <relative> - 0: disp; Used for core.oat linking only
  kX86Ret,              // ret; no lir operands
  kX86StartOfMethod,    // call 0; pop reg; sub reg, # - generate start of method into reg
                        // lir operands - 0: reg
  kX86PcRelLoadRA,      // mov reg, [base + index * scale + PC relative displacement]
                        // lir operands - 0: reg, 1: base, 2: index, 3: scale, 4: table
  kX86PcRelAdr,         // mov reg, PC relative displacement; lir operands - 0: reg, 1: table
  kX86RepneScasw,       // repne scasw
  kX86Last
};

/* Instruction assembly field_loc kind */
enum X86EncodingKind {
  kData,                                   // Special case for raw data.
  kNop,                                    // Special case for variable length nop.
  kNullary,                                // Opcode that takes no arguments.
  kPrefix2Nullary,                         // Opcode that takes no arguments, but 2 prefixes.
  kRegOpcode,                              // Shorter form of R instruction kind (opcode+rd)
  kReg, kMem, kArray,                      // R, M and A instruction kinds.
  kMemReg, kArrayReg, kThreadReg,          // MR, AR and TR instruction kinds.
  kRegReg, kRegMem, kRegArray, kRegThread,  // RR, RM, RA and RT instruction kinds.
  kRegRegStore,                            // RR following the store modrm reg-reg encoding rather than the load.
  kRegImm, kMemImm, kArrayImm, kThreadImm,  // RI, MI, AI and TI instruction kinds.
  kRegRegImm, kRegMemImm, kRegArrayImm,    // RRI, RMI and RAI instruction kinds.
  kMovRegImm,                              // Shorter form move RI.
  kRegRegImmRev,                           // RRI with first reg in r/m
  kShiftRegImm, kShiftMemImm, kShiftArrayImm,  // Shift opcode with immediate.
  kShiftRegCl, kShiftMemCl, kShiftArrayCl,     // Shift opcode with register CL.
  kRegRegReg, kRegRegMem, kRegRegArray,    // RRR, RRM, RRA instruction kinds.
  kRegCond, kMemCond, kArrayCond,          // R, M, A instruction kinds following by a condition.
  kRegRegCond,                             // RR instruction kind followed by a condition.
  kJmp, kJcc, kCall,                       // Branch instruction kinds.
  kPcRel,                                  // Operation with displacement that is PC relative
  kMacro,                                  // An instruction composing multiple others
  kUnimplemented                           // Encoding used when an instruction isn't yet implemented.
};

/* Struct used to define the EncodingMap positions for each X86 opcode */
struct X86EncodingMap {
  X86OpCode opcode;      // e.g. kOpAddRI
  X86EncodingKind kind;  // Used to discriminate in the union below
  uint64_t flags;
  struct {
  uint8_t prefix1;       // non-zero => a prefix byte
  uint8_t prefix2;       // non-zero => a second prefix byte
  uint8_t opcode;        // 1 byte opcode
  uint8_t extra_opcode1;  // possible extra opcode byte
  uint8_t extra_opcode2;  // possible second extra opcode byte
  // 3bit opcode that gets encoded in the register bits of the modrm byte, use determined by the
  // encoding kind
  uint8_t modrm_opcode;
  uint8_t ax_opcode;  // non-zero => shorter encoding for AX as a destination
  uint8_t immediate_bytes;  // number of bytes of immediate
  } skeleton;
  const char *name;
  const char* fmt;
};


// FIXME: mem barrier type - what do we do for x86?
#define kSY 0
#define kST 0

// Offsets of high and low halves of a 64bit value.
#define LOWORD_OFFSET 0
#define HIWORD_OFFSET 4

// Segment override instruction prefix used for quick TLS access to Thread::Current().
#define THREAD_PREFIX 0x64

#define IS_SIMM8(v) ((-128 <= (v)) && ((v) <= 127))
#define IS_SIMM16(v) ((-32768 <= (v)) && ((v) <= 32767))

extern X86EncodingMap EncodingMap[kX86Last];
extern X86ConditionCode X86ConditionEncoding(ConditionCode cond);

}  // namespace art

#endif  // ART_COMPILER_DEX_QUICK_X86_X86_LIR_H_
