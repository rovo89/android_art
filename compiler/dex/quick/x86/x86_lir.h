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
 *  XMM8 .. 11: caller save available as scratch registers for ART.
 *  XMM12 .. 15: callee save available as promoted registers for ART.
 *  This change (XMM12..15) is for QCG only, for others they are caller save.
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

enum X86ResourceEncodingPos {
  kX86GPReg0   = 0,
  kX86RegSP    = 4,
  kX86FPReg0   = 16,  // xmm0 .. xmm7/xmm15.
  kX86FPRegEnd = 32,
  kX86FPStack  = 33,
  kX86RegEnd   = kX86FPStack,
};

// FIXME: for 64-bit, perhaps add an X86_64NativeRegisterPool enum?
enum X86NativeRegisterPool {
  r0             = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 0,
  r0q            = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 0,
  rAX            = r0,
  r1             = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 1,
  r1q            = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 1,
  rCX            = r1,
  r2             = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 2,
  r2q            = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 2,
  rDX            = r2,
  r3             = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 3,
  r3q            = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 3,
  rBX            = r3,
  r4sp_32        = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 4,
  rX86_SP_32     = r4sp_32,
  r4sp_64        = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 4,
  rX86_SP_64     = r4sp_64,
  r5             = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 5,
  r5q            = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 5,
  rBP            = r5,
  r5sib_no_base  = r5,
  r6             = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 6,
  r6q            = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 6,
  rSI            = r6,
  r7             = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 7,
  r7q            = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 7,
  rDI            = r7,
  r8             = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 8,
  r8q            = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 8,
  r9             = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 9,
  r9q            = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 9,
  r10            = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 10,
  r10q           = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 10,
  r11            = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 11,
  r11q           = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 11,
  r12            = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 12,
  r12q           = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 12,
  r13            = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 13,
  r13q           = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 13,
  r14            = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 14,
  r14q           = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 14,
  r15            = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 15,
  r15q           = RegStorage::k64BitSolo | RegStorage::kCoreRegister | 15,
  // fake return address register for core spill mask.
  rRET           = RegStorage::k32BitSolo | RegStorage::kCoreRegister | 16,

  // xmm registers, single precision view.
  fr0  = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 0,
  fr1  = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 1,
  fr2  = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 2,
  fr3  = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 3,
  fr4  = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 4,
  fr5  = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 5,
  fr6  = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 6,
  fr7  = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 7,
  fr8  = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 8,
  fr9  = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 9,
  fr10 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 10,
  fr11 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 11,
  fr12 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 12,
  fr13 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 13,
  fr14 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 14,
  fr15 = RegStorage::k32BitSolo | RegStorage::kFloatingPoint | 15,

  // xmm registers, double precision aliases.
  dr0  = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 0,
  dr1  = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 1,
  dr2  = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 2,
  dr3  = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 3,
  dr4  = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 4,
  dr5  = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 5,
  dr6  = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 6,
  dr7  = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 7,
  dr8  = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 8,
  dr9  = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 9,
  dr10 = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 10,
  dr11 = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 11,
  dr12 = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 12,
  dr13 = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 13,
  dr14 = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 14,
  dr15 = RegStorage::k64BitSolo | RegStorage::kFloatingPoint | 15,

  // xmm registers, quad precision aliases
  xr0  = RegStorage::k128BitSolo | 0,
  xr1  = RegStorage::k128BitSolo | 1,
  xr2  = RegStorage::k128BitSolo | 2,
  xr3  = RegStorage::k128BitSolo | 3,
  xr4  = RegStorage::k128BitSolo | 4,
  xr5  = RegStorage::k128BitSolo | 5,
  xr6  = RegStorage::k128BitSolo | 6,
  xr7  = RegStorage::k128BitSolo | 7,
  xr8  = RegStorage::k128BitSolo | 8,
  xr9  = RegStorage::k128BitSolo | 9,
  xr10 = RegStorage::k128BitSolo | 10,
  xr11 = RegStorage::k128BitSolo | 11,
  xr12 = RegStorage::k128BitSolo | 12,
  xr13 = RegStorage::k128BitSolo | 13,
  xr14 = RegStorage::k128BitSolo | 14,
  xr15 = RegStorage::k128BitSolo | 15,

  // TODO: as needed, add 256, 512 and 1024-bit xmm views.
};

constexpr RegStorage rs_r0(RegStorage::kValid | r0);
constexpr RegStorage rs_r0q(RegStorage::kValid | r0q);
constexpr RegStorage rs_rAX = rs_r0;
constexpr RegStorage rs_r1(RegStorage::kValid | r1);
constexpr RegStorage rs_r1q(RegStorage::kValid | r1q);
constexpr RegStorage rs_rCX = rs_r1;
constexpr RegStorage rs_r2(RegStorage::kValid | r2);
constexpr RegStorage rs_r2q(RegStorage::kValid | r2q);
constexpr RegStorage rs_rDX = rs_r2;
constexpr RegStorage rs_r3(RegStorage::kValid | r3);
constexpr RegStorage rs_r3q(RegStorage::kValid | r3q);
constexpr RegStorage rs_rBX = rs_r3;
constexpr RegStorage rs_rX86_SP_64(RegStorage::kValid | r4sp_64);
constexpr RegStorage rs_rX86_SP_32(RegStorage::kValid | r4sp_32);
extern RegStorage rs_rX86_SP;
constexpr RegStorage rs_r5(RegStorage::kValid | r5);
constexpr RegStorage rs_r5q(RegStorage::kValid | r5q);
constexpr RegStorage rs_rBP = rs_r5;
constexpr RegStorage rs_r6(RegStorage::kValid | r6);
constexpr RegStorage rs_r6q(RegStorage::kValid | r6q);
constexpr RegStorage rs_rSI = rs_r6;
constexpr RegStorage rs_r7(RegStorage::kValid | r7);
constexpr RegStorage rs_r7q(RegStorage::kValid | r7q);
constexpr RegStorage rs_rDI = rs_r7;
constexpr RegStorage rs_rRET(RegStorage::kValid | rRET);
constexpr RegStorage rs_r8(RegStorage::kValid | r8);
constexpr RegStorage rs_r8q(RegStorage::kValid | r8q);
constexpr RegStorage rs_r9(RegStorage::kValid | r9);
constexpr RegStorage rs_r9q(RegStorage::kValid | r9q);
constexpr RegStorage rs_r10(RegStorage::kValid | r10);
constexpr RegStorage rs_r10q(RegStorage::kValid | r10q);
constexpr RegStorage rs_r11(RegStorage::kValid | r11);
constexpr RegStorage rs_r11q(RegStorage::kValid | r11q);
constexpr RegStorage rs_r12(RegStorage::kValid | r12);
constexpr RegStorage rs_r12q(RegStorage::kValid | r12q);
constexpr RegStorage rs_r13(RegStorage::kValid | r13);
constexpr RegStorage rs_r13q(RegStorage::kValid | r13q);
constexpr RegStorage rs_r14(RegStorage::kValid | r14);
constexpr RegStorage rs_r14q(RegStorage::kValid | r14q);
constexpr RegStorage rs_r15(RegStorage::kValid | r15);
constexpr RegStorage rs_r15q(RegStorage::kValid | r15q);

constexpr RegStorage rs_fr0(RegStorage::kValid | fr0);
constexpr RegStorage rs_fr1(RegStorage::kValid | fr1);
constexpr RegStorage rs_fr2(RegStorage::kValid | fr2);
constexpr RegStorage rs_fr3(RegStorage::kValid | fr3);
constexpr RegStorage rs_fr4(RegStorage::kValid | fr4);
constexpr RegStorage rs_fr5(RegStorage::kValid | fr5);
constexpr RegStorage rs_fr6(RegStorage::kValid | fr6);
constexpr RegStorage rs_fr7(RegStorage::kValid | fr7);
constexpr RegStorage rs_fr8(RegStorage::kValid | fr8);
constexpr RegStorage rs_fr9(RegStorage::kValid | fr9);
constexpr RegStorage rs_fr10(RegStorage::kValid | fr10);
constexpr RegStorage rs_fr11(RegStorage::kValid | fr11);
constexpr RegStorage rs_fr12(RegStorage::kValid | fr12);
constexpr RegStorage rs_fr13(RegStorage::kValid | fr13);
constexpr RegStorage rs_fr14(RegStorage::kValid | fr14);
constexpr RegStorage rs_fr15(RegStorage::kValid | fr15);

constexpr RegStorage rs_dr0(RegStorage::kValid | dr0);
constexpr RegStorage rs_dr1(RegStorage::kValid | dr1);
constexpr RegStorage rs_dr2(RegStorage::kValid | dr2);
constexpr RegStorage rs_dr3(RegStorage::kValid | dr3);
constexpr RegStorage rs_dr4(RegStorage::kValid | dr4);
constexpr RegStorage rs_dr5(RegStorage::kValid | dr5);
constexpr RegStorage rs_dr6(RegStorage::kValid | dr6);
constexpr RegStorage rs_dr7(RegStorage::kValid | dr7);
constexpr RegStorage rs_dr8(RegStorage::kValid | dr8);
constexpr RegStorage rs_dr9(RegStorage::kValid | dr9);
constexpr RegStorage rs_dr10(RegStorage::kValid | dr10);
constexpr RegStorage rs_dr11(RegStorage::kValid | dr11);
constexpr RegStorage rs_dr12(RegStorage::kValid | dr12);
constexpr RegStorage rs_dr13(RegStorage::kValid | dr13);
constexpr RegStorage rs_dr14(RegStorage::kValid | dr14);
constexpr RegStorage rs_dr15(RegStorage::kValid | dr15);

constexpr RegStorage rs_xr0(RegStorage::kValid | xr0);
constexpr RegStorage rs_xr1(RegStorage::kValid | xr1);
constexpr RegStorage rs_xr2(RegStorage::kValid | xr2);
constexpr RegStorage rs_xr3(RegStorage::kValid | xr3);
constexpr RegStorage rs_xr4(RegStorage::kValid | xr4);
constexpr RegStorage rs_xr5(RegStorage::kValid | xr5);
constexpr RegStorage rs_xr6(RegStorage::kValid | xr6);
constexpr RegStorage rs_xr7(RegStorage::kValid | xr7);
constexpr RegStorage rs_xr8(RegStorage::kValid | xr8);
constexpr RegStorage rs_xr9(RegStorage::kValid | xr9);
constexpr RegStorage rs_xr10(RegStorage::kValid | xr10);
constexpr RegStorage rs_xr11(RegStorage::kValid | xr11);
constexpr RegStorage rs_xr12(RegStorage::kValid | xr12);
constexpr RegStorage rs_xr13(RegStorage::kValid | xr13);
constexpr RegStorage rs_xr14(RegStorage::kValid | xr14);
constexpr RegStorage rs_xr15(RegStorage::kValid | xr15);

extern X86NativeRegisterPool rX86_ARG0;
extern X86NativeRegisterPool rX86_ARG1;
extern X86NativeRegisterPool rX86_ARG2;
extern X86NativeRegisterPool rX86_ARG3;
extern X86NativeRegisterPool rX86_ARG4;
extern X86NativeRegisterPool rX86_ARG5;
extern X86NativeRegisterPool rX86_FARG0;
extern X86NativeRegisterPool rX86_FARG1;
extern X86NativeRegisterPool rX86_FARG2;
extern X86NativeRegisterPool rX86_FARG3;
extern X86NativeRegisterPool rX86_FARG4;
extern X86NativeRegisterPool rX86_FARG5;
extern X86NativeRegisterPool rX86_FARG6;
extern X86NativeRegisterPool rX86_FARG7;
extern X86NativeRegisterPool rX86_RET0;
extern X86NativeRegisterPool rX86_RET1;
extern X86NativeRegisterPool rX86_INVOKE_TGT;
extern X86NativeRegisterPool rX86_COUNT;

extern RegStorage rs_rX86_ARG0;
extern RegStorage rs_rX86_ARG1;
extern RegStorage rs_rX86_ARG2;
extern RegStorage rs_rX86_ARG3;
extern RegStorage rs_rX86_ARG4;
extern RegStorage rs_rX86_ARG5;
extern RegStorage rs_rX86_FARG0;
extern RegStorage rs_rX86_FARG1;
extern RegStorage rs_rX86_FARG2;
extern RegStorage rs_rX86_FARG3;
extern RegStorage rs_rX86_FARG4;
extern RegStorage rs_rX86_FARG5;
extern RegStorage rs_rX86_FARG6;
extern RegStorage rs_rX86_FARG7;
extern RegStorage rs_rX86_RET0;
extern RegStorage rs_rX86_RET1;
extern RegStorage rs_rX86_INVOKE_TGT;
extern RegStorage rs_rX86_COUNT;

// RegisterLocation templates return values (r_V0, or r_V0/r_V1).
const RegLocation x86_loc_c_return
    {kLocPhysReg, 0, 0, 0, 0, 0, 0, 0, 1,
     RegStorage(RegStorage::k32BitSolo, rAX), INVALID_SREG, INVALID_SREG};
const RegLocation x86_loc_c_return_wide
    {kLocPhysReg, 1, 0, 0, 0, 0, 0, 0, 1,
     RegStorage(RegStorage::k64BitPair, rAX, rDX), INVALID_SREG, INVALID_SREG};
const RegLocation x86_loc_c_return_ref
    {kLocPhysReg, 0, 0, 0, 0, 0, 1, 0, 1,
     RegStorage(RegStorage::k32BitSolo, rAX), INVALID_SREG, INVALID_SREG};
const RegLocation x86_64_loc_c_return_ref
    {kLocPhysReg, 0, 0, 0, 0, 0, 1, 0, 1,
     RegStorage(RegStorage::k64BitSolo, rAX), INVALID_SREG, INVALID_SREG};
const RegLocation x86_64_loc_c_return_wide
    {kLocPhysReg, 1, 0, 0, 0, 0, 0, 0, 1,
     RegStorage(RegStorage::k64BitSolo, rAX), INVALID_SREG, INVALID_SREG};
const RegLocation x86_loc_c_return_float
    {kLocPhysReg, 0, 0, 0, 1, 0, 0, 0, 1,
     RegStorage(RegStorage::k32BitSolo, fr0), INVALID_SREG, INVALID_SREG};
const RegLocation x86_loc_c_return_double
    {kLocPhysReg, 1, 0, 0, 1, 0, 0, 0, 1,
     RegStorage(RegStorage::k64BitSolo, dr0), INVALID_SREG, INVALID_SREG};

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
  opcode ## 32RI8, opcode ## 32MI8, opcode ## 32AI8, opcode ## 32TI8, \
  opcode ## 64MR, opcode ## 64AR, opcode ## 64TR,  \
  opcode ## 64RR, opcode ## 64RM, opcode ## 64RA, opcode ## 64RT, \
  opcode ## 64RI, opcode ## 64MI, opcode ## 64AI, opcode ## 64TI, \
  opcode ## 64RI8, opcode ## 64MI8, opcode ## 64AI8, opcode ## 64TI8
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
  kX86Imul64RRI, kX86Imul64RMI, kX86Imul64RAI,
  kX86Imul64RRI8, kX86Imul64RMI8, kX86Imul64RAI8,
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
  kX86Mov64MR, kX86Mov64AR, kX86Mov64TR,
  kX86Mov64RR, kX86Mov64RM, kX86Mov64RA, kX86Mov64RT,
  kX86Mov64RI32, kX86Mov64RI64, kX86Mov64MI, kX86Mov64AI, kX86Mov64TI,
  kX86Lea64RM,
  kX86Lea64RA,
  // RRC - Register Register ConditionCode - cond_opcode reg1, reg2
  //             - lir operands - 0: reg1, 1: reg2, 2: CC
  kX86Cmov32RRC,
  kX86Cmov64RRC,
  // RMC - Register Memory ConditionCode - cond_opcode reg1, [base + disp]
  //             - lir operands - 0: reg1, 1: base, 2: disp 3: CC
  kX86Cmov32RMC,
  kX86Cmov64RMC,

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
  opcode ## 32RC, opcode ## 32MC, opcode ## 32AC, \
  opcode ## 64RI, opcode ## 64MI, opcode ## 64AI, \
  opcode ## 64RC, opcode ## 64MC, opcode ## 64AC
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
  kX86Shld32MRI,
  kX86Shrd32RRI,
  kX86Shrd32MRI,
  kX86Shld64RRI,
  kX86Shld64MRI,
  kX86Shrd64RRI,
  kX86Shrd64MRI,
#define UnaryOpcode(opcode, reg, mem, array) \
  opcode ## 8 ## reg, opcode ## 8 ## mem, opcode ## 8 ## array, \
  opcode ## 16 ## reg, opcode ## 16 ## mem, opcode ## 16 ## array, \
  opcode ## 32 ## reg, opcode ## 32 ## mem, opcode ## 32 ## array, \
  opcode ## 64 ## reg, opcode ## 64 ## mem, opcode ## 64 ## array
  UnaryOpcode(kX86Test, RI, MI, AI),
  kX86Test32RR,
  kX86Test64RR,
  kX86Test32RM,
  UnaryOpcode(kX86Not, R, M, A),
  UnaryOpcode(kX86Neg, R, M, A),
  UnaryOpcode(kX86Mul,  DaR, DaM, DaA),
  UnaryOpcode(kX86Imul, DaR, DaM, DaA),
  UnaryOpcode(kX86Divmod,  DaR, DaM, DaA),
  UnaryOpcode(kX86Idivmod, DaR, DaM, DaA),
  kx86Cdq32Da,
  kx86Cqo64Da,
  kX86Bswap32R,
  kX86Bswap64R,
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
  Binary0fOpCode(kX86Cvtsqi2sd),  // long to double
  Binary0fOpCode(kX86Cvtsqi2ss),  // long to float
  Binary0fOpCode(kX86Cvttsd2si),  // truncating double to int
  Binary0fOpCode(kX86Cvttss2si),  // truncating float to int
  Binary0fOpCode(kX86Cvttsd2sqi),  // truncating double to long
  Binary0fOpCode(kX86Cvttss2sqi),  // truncating float to long
  Binary0fOpCode(kX86Cvtsd2si),  // rounding double to int
  Binary0fOpCode(kX86Cvtss2si),  // rounding float to int
  Binary0fOpCode(kX86Ucomisd),  // unordered double compare
  Binary0fOpCode(kX86Ucomiss),  // unordered float compare
  Binary0fOpCode(kX86Comisd),   // double compare
  Binary0fOpCode(kX86Comiss),   // float compare
  Binary0fOpCode(kX86Orpd),     // double logical OR
  Binary0fOpCode(kX86Orps),     // float logical OR
  Binary0fOpCode(kX86Andpd),    // double logical AND
  Binary0fOpCode(kX86Andps),    // float logical AND
  Binary0fOpCode(kX86Xorpd),    // double logical XOR
  Binary0fOpCode(kX86Xorps),    // float logical XOR
  Binary0fOpCode(kX86Addsd),    // double ADD
  Binary0fOpCode(kX86Addss),    // float ADD
  Binary0fOpCode(kX86Mulsd),    // double multiply
  Binary0fOpCode(kX86Mulss),    // float multiply
  Binary0fOpCode(kX86Cvtsd2ss),  // double to float
  Binary0fOpCode(kX86Cvtss2sd),  // float to double
  Binary0fOpCode(kX86Subsd),    // double subtract
  Binary0fOpCode(kX86Subss),    // float subtract
  Binary0fOpCode(kX86Divsd),    // double divide
  Binary0fOpCode(kX86Divss),    // float divide
  Binary0fOpCode(kX86Punpckldq),  // Interleave low-order double words
  Binary0fOpCode(kX86Sqrtsd),   // square root
  Binary0fOpCode(kX86Pmulld),   // parallel integer multiply 32 bits x 4
  Binary0fOpCode(kX86Pmullw),   // parallel integer multiply 16 bits x 8
  Binary0fOpCode(kX86Mulps),    // parallel FP multiply 32 bits x 4
  Binary0fOpCode(kX86Mulpd),    // parallel FP multiply 64 bits x 2
  Binary0fOpCode(kX86Paddb),    // parallel integer addition 8 bits x 16
  Binary0fOpCode(kX86Paddw),    // parallel integer addition 16 bits x 8
  Binary0fOpCode(kX86Paddd),    // parallel integer addition 32 bits x 4
  Binary0fOpCode(kX86Addps),    // parallel FP addition 32 bits x 4
  Binary0fOpCode(kX86Addpd),    // parallel FP addition 64 bits x 2
  Binary0fOpCode(kX86Psubb),    // parallel integer subtraction 8 bits x 16
  Binary0fOpCode(kX86Psubw),    // parallel integer subtraction 16 bits x 8
  Binary0fOpCode(kX86Psubd),    // parallel integer subtraction 32 bits x 4
  Binary0fOpCode(kX86Subps),    // parallel FP subtraction 32 bits x 4
  Binary0fOpCode(kX86Subpd),    // parallel FP subtraction 64 bits x 2
  Binary0fOpCode(kX86Pand),     // parallel AND 128 bits x 1
  Binary0fOpCode(kX86Por),      // parallel OR 128 bits x 1
  Binary0fOpCode(kX86Pxor),     // parallel XOR 128 bits x 1
  Binary0fOpCode(kX86Phaddw),   // parallel horizontal addition 16 bits x 8
  Binary0fOpCode(kX86Phaddd),   // parallel horizontal addition 32 bits x 4
  Binary0fOpCode(kX86Haddpd),   // parallel FP horizontal addition 64 bits x 2
  Binary0fOpCode(kX86Haddps),   // parallel FP horizontal addition 32 bits x 4
  kX86PextrbRRI,                // Extract 8 bits from XMM into GPR
  kX86PextrwRRI,                // Extract 16 bits from XMM into GPR
  kX86PextrdRRI,                // Extract 32 bits from XMM into GPR
  kX86PextrbMRI,                // Extract 8 bits from XMM into memory
  kX86PextrwMRI,                // Extract 16 bits from XMM into memory
  kX86PextrdMRI,                // Extract 32 bits from XMM into memory
  kX86PshuflwRRI,               // Shuffle 16 bits in lower 64 bits of XMM.
  kX86PshufdRRI,                // Shuffle 32 bits in XMM.
  kX86ShufpsRRI,                // FP Shuffle 32 bits in XMM.
  kX86ShufpdRRI,                // FP Shuffle 64 bits in XMM.
  kX86PsrawRI,                  // signed right shift of floating point registers 16 bits x 8
  kX86PsradRI,                  // signed right shift of floating point registers 32 bits x 4
  kX86PsrlwRI,                  // logical right shift of floating point registers 16 bits x 8
  kX86PsrldRI,                  // logical right shift of floating point registers 32 bits x 4
  kX86PsrlqRI,                  // logical right shift of floating point registers 64 bits x 2
  kX86PsllwRI,                  // left shift of floating point registers 16 bits x 8
  kX86PslldRI,                  // left shift of floating point registers 32 bits x 4
  kX86PsllqRI,                  // left shift of floating point registers 64 bits x 2
  kX86Fild32M,                  // push 32-bit integer on x87 stack
  kX86Fild64M,                  // push 64-bit integer on x87 stack
  kX86Fld32M,                   // push float on x87 stack
  kX86Fld64M,                   // push double on x87 stack
  kX86Fstp32M,                  // pop top x87 fp stack and do 32-bit store
  kX86Fstp64M,                  // pop top x87 fp stack and do 64-bit store
  kX86Fst32M,                   // do 32-bit store
  kX86Fst64M,                   // do 64-bit store
  kX86Fprem,                    // remainder from dividing of two floating point values
  kX86Fucompp,                  // compare floating point values and pop x87 fp stack twice
  kX86Fstsw16R,                 // store FPU status word
  Binary0fOpCode(kX86Mova128),  // move 128 bits aligned
  kX86Mova128MR, kX86Mova128AR,  // store 128 bit aligned from xmm1 to m128
  Binary0fOpCode(kX86Movups),   // load unaligned packed single FP values from xmm2/m128 to xmm1
  kX86MovupsMR, kX86MovupsAR,   // store unaligned packed single FP values from xmm1 to m128
  Binary0fOpCode(kX86Movaps),   // load aligned packed single FP values from xmm2/m128 to xmm1
  kX86MovapsMR, kX86MovapsAR,   // store aligned packed single FP values from xmm1 to m128
  kX86MovlpsRM, kX86MovlpsRA,   // load packed single FP values from m64 to low quadword of xmm
  kX86MovlpsMR, kX86MovlpsAR,   // store packed single FP values from low quadword of xmm to m64
  kX86MovhpsRM, kX86MovhpsRA,   // load packed single FP values from m64 to high quadword of xmm
  kX86MovhpsMR, kX86MovhpsAR,   // store packed single FP values from high quadword of xmm to m64
  Binary0fOpCode(kX86Movdxr),   // move into xmm from gpr
  Binary0fOpCode(kX86Movqxr),   // move into xmm from 64 bit gpr
  kX86MovqrxRR, kX86MovqrxMR, kX86MovqrxAR,  // move into 64 bit reg from xmm
  kX86MovdrxRR, kX86MovdrxMR, kX86MovdrxAR,  // move into reg from xmm
  kX86MovsxdRR, kX86MovsxdRM, kX86MovsxdRA,  // move 32 bit to 64 bit with sign extension
  kX86Set8R, kX86Set8M, kX86Set8A,  // set byte depending on condition operand
  kX86Mfence,                   // memory barrier
  Binary0fOpCode(kX86Imul16),   // 16bit multiply
  Binary0fOpCode(kX86Imul32),   // 32bit multiply
  Binary0fOpCode(kX86Imul64),   // 64bit multiply
  kX86CmpxchgRR, kX86CmpxchgMR, kX86CmpxchgAR,  // compare and exchange
  kX86LockCmpxchgMR, kX86LockCmpxchgAR, kX86LockCmpxchg64AR,  // locked compare and exchange
  kX86LockCmpxchg64M, kX86LockCmpxchg64A,  // locked compare and exchange
  kX86XchgMR,  // exchange memory with register (automatically locked)
  Binary0fOpCode(kX86Movzx8),   // zero-extend 8-bit value
  Binary0fOpCode(kX86Movzx16),  // zero-extend 16-bit value
  Binary0fOpCode(kX86Movsx8),   // sign-extend 8-bit value
  Binary0fOpCode(kX86Movsx16),  // sign-extend 16-bit value
  Binary0fOpCode(kX86Movzx8q),   // zero-extend 8-bit value to quad word
  Binary0fOpCode(kX86Movzx16q),  // zero-extend 16-bit value to quad word
  Binary0fOpCode(kX86Movsx8q),   // sign-extend 8-bit value to quad word
  Binary0fOpCode(kX86Movsx16q),  // sign-extend 16-bit value to quad word
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
  kData,                                    // Special case for raw data.
  kNop,                                     // Special case for variable length nop.
  kNullary,                                 // Opcode that takes no arguments.
  kRegOpcode,                               // Shorter form of R instruction kind (opcode+rd)
  kReg, kMem, kArray,                       // R, M and A instruction kinds.
  kMemReg, kArrayReg, kThreadReg,           // MR, AR and TR instruction kinds.
  kRegReg, kRegMem, kRegArray, kRegThread,  // RR, RM, RA and RT instruction kinds.
  kRegRegStore,                             // RR following the store modrm reg-reg encoding rather than the load.
  kRegImm, kMemImm, kArrayImm, kThreadImm,  // RI, MI, AI and TI instruction kinds.
  kRegRegImm, kRegMemImm, kRegArrayImm,     // RRI, RMI and RAI instruction kinds.
  kMovRegImm,                               // Shorter form move RI.
  kMovRegQuadImm,                           // 64 bit move RI
  kRegRegImmStore,                          // RRI following the store modrm reg-reg encoding rather than the load.
  kMemRegImm,                               // MRI instruction kinds.
  kShiftRegImm, kShiftMemImm, kShiftArrayImm,  // Shift opcode with immediate.
  kShiftRegCl, kShiftMemCl, kShiftArrayCl,     // Shift opcode with register CL.
  // kRegRegReg, kRegRegMem, kRegRegArray,    // RRR, RRM, RRA instruction kinds.
  kRegCond, kMemCond, kArrayCond,          // R, M, A instruction kinds following by a condition.
  kRegRegCond,                             // RR instruction kind followed by a condition.
  kRegMemCond,                             // RM instruction kind followed by a condition.
  kJmp, kJcc, kCall,                       // Branch instruction kinds.
  kPcRel,                                  // Operation with displacement that is PC relative
  kMacro,                                  // An instruction composing multiple others
  kUnimplemented                           // Encoding used when an instruction isn't yet implemented.
};

/* Struct used to define the EncodingMap positions for each X86 opcode */
struct X86EncodingMap {
  X86OpCode opcode;      // e.g. kOpAddRI
  // The broad category the instruction conforms to, such as kRegReg. Identifies which LIR operands
  // hold meaning for the opcode.
  X86EncodingKind kind;
  uint64_t flags;
  struct {
  uint8_t prefix1;       // Non-zero => a prefix byte.
  uint8_t prefix2;       // Non-zero => a second prefix byte.
  uint8_t opcode;        // 1 byte opcode.
  uint8_t extra_opcode1;  // Possible extra opcode byte.
  uint8_t extra_opcode2;  // Possible second extra opcode byte.
  // 3-bit opcode that gets encoded in the register bits of the modrm byte, use determined by the
  // encoding kind.
  uint8_t modrm_opcode;
  uint8_t ax_opcode;  // Non-zero => shorter encoding for AX as a destination.
  uint8_t immediate_bytes;  // Number of bytes of immediate.
  // Does the instruction address a byte register? In 32-bit mode the registers ah, bh, ch and dh
  // are not used. In 64-bit mode the REX prefix is used to normalize and allow any byte register
  // to be addressed.
  bool r8_form;
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
#define THREAD_PREFIX_GS 0x65

// 64 Bit Operand Size
#define REX_W 0x48
// Extension of the ModR/M reg field
#define REX_R 0x44
// Extension of the SIB index field
#define REX_X 0x42
// Extension of the ModR/M r/m field, SIB base field, or Opcode reg field
#define REX_B 0x41
// An empty REX prefix used to normalize the byte operations so that they apply to R4 through R15
#define REX 0x40
// Mask extracting the least 3 bits of r0..r15
#define kRegNumMask32 0x07
// Value indicating that base or reg is not used
#define NO_REG 0

#define IS_SIMM8(v) ((-128 <= (v)) && ((v) <= 127))
#define IS_SIMM16(v) ((-32768 <= (v)) && ((v) <= 32767))
#define IS_SIMM32(v) ((INT64_C(-2147483648) <= (v)) && ((v) <= INT64_C(2147483647)))

extern X86EncodingMap EncodingMap[kX86Last];
extern X86ConditionCode X86ConditionEncoding(ConditionCode cond);

}  // namespace art

#endif  // ART_COMPILER_DEX_QUICK_X86_X86_LIR_H_
