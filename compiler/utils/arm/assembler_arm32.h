/*
 * Copyright (C) 2014 The Android Open Source Project
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

#ifndef ART_COMPILER_UTILS_ARM_ASSEMBLER_ARM32_H_
#define ART_COMPILER_UTILS_ARM_ASSEMBLER_ARM32_H_

#include <vector>

#include "base/logging.h"
#include "constants_arm.h"
#include "utils/arm/managed_register_arm.h"
#include "utils/arm/assembler_arm.h"
#include "offsets.h"
#include "utils.h"

namespace art {
namespace arm {

class Arm32Assembler FINAL : public ArmAssembler {
 public:
  Arm32Assembler() {
  }
  virtual ~Arm32Assembler() {}

  bool IsThumb() const OVERRIDE {
    return false;
  }

  // Data-processing instructions.
  void and_(Register rd, Register rn, const ShifterOperand& so, Condition cond = AL) OVERRIDE;

  void eor(Register rd, Register rn, const ShifterOperand& so, Condition cond = AL) OVERRIDE;

  void sub(Register rd, Register rn, const ShifterOperand& so, Condition cond = AL) OVERRIDE;
  void subs(Register rd, Register rn, const ShifterOperand& so, Condition cond = AL) OVERRIDE;

  void rsb(Register rd, Register rn, const ShifterOperand& so, Condition cond = AL) OVERRIDE;
  void rsbs(Register rd, Register rn, const ShifterOperand& so, Condition cond = AL) OVERRIDE;

  void add(Register rd, Register rn, const ShifterOperand& so, Condition cond = AL) OVERRIDE;

  void adds(Register rd, Register rn, const ShifterOperand& so, Condition cond = AL) OVERRIDE;

  void adc(Register rd, Register rn, const ShifterOperand& so, Condition cond = AL) OVERRIDE;

  void sbc(Register rd, Register rn, const ShifterOperand& so, Condition cond = AL) OVERRIDE;

  void rsc(Register rd, Register rn, const ShifterOperand& so, Condition cond = AL) OVERRIDE;

  void tst(Register rn, const ShifterOperand& so, Condition cond = AL) OVERRIDE;

  void teq(Register rn, const ShifterOperand& so, Condition cond = AL) OVERRIDE;

  void cmp(Register rn, const ShifterOperand& so, Condition cond = AL) OVERRIDE;

  void cmn(Register rn, const ShifterOperand& so, Condition cond = AL) OVERRIDE;

  void orr(Register rd, Register rn, const ShifterOperand& so, Condition cond = AL) OVERRIDE;
  void orrs(Register rd, Register rn, const ShifterOperand& so, Condition cond = AL) OVERRIDE;

  void mov(Register rd, const ShifterOperand& so, Condition cond = AL) OVERRIDE;
  void movs(Register rd, const ShifterOperand& so, Condition cond = AL) OVERRIDE;

  void bic(Register rd, Register rn, const ShifterOperand& so, Condition cond = AL) OVERRIDE;

  void mvn(Register rd, const ShifterOperand& so, Condition cond = AL) OVERRIDE;
  void mvns(Register rd, const ShifterOperand& so, Condition cond = AL) OVERRIDE;

  // Miscellaneous data-processing instructions.
  void clz(Register rd, Register rm, Condition cond = AL) OVERRIDE;
  void movw(Register rd, uint16_t imm16, Condition cond = AL) OVERRIDE;
  void movt(Register rd, uint16_t imm16, Condition cond = AL) OVERRIDE;

  // Multiply instructions.
  void mul(Register rd, Register rn, Register rm, Condition cond = AL) OVERRIDE;
  void mla(Register rd, Register rn, Register rm, Register ra,
           Condition cond = AL) OVERRIDE;
  void mls(Register rd, Register rn, Register rm, Register ra,
           Condition cond = AL) OVERRIDE;
  void umull(Register rd_lo, Register rd_hi, Register rn, Register rm,
             Condition cond = AL) OVERRIDE;

  void sdiv(Register rd, Register rn, Register rm, Condition cond = AL) OVERRIDE;
  void udiv(Register rd, Register rn, Register rm, Condition cond = AL) OVERRIDE;

  // Load/store instructions.
  void ldr(Register rd, const Address& ad, Condition cond = AL) OVERRIDE;
  void str(Register rd, const Address& ad, Condition cond = AL) OVERRIDE;

  void ldrb(Register rd, const Address& ad, Condition cond = AL) OVERRIDE;
  void strb(Register rd, const Address& ad, Condition cond = AL) OVERRIDE;

  void ldrh(Register rd, const Address& ad, Condition cond = AL) OVERRIDE;
  void strh(Register rd, const Address& ad, Condition cond = AL) OVERRIDE;

  void ldrsb(Register rd, const Address& ad, Condition cond = AL) OVERRIDE;
  void ldrsh(Register rd, const Address& ad, Condition cond = AL) OVERRIDE;

  void ldrd(Register rd, const Address& ad, Condition cond = AL) OVERRIDE;
  void strd(Register rd, const Address& ad, Condition cond = AL) OVERRIDE;

  void ldm(BlockAddressMode am, Register base,
           RegList regs, Condition cond = AL) OVERRIDE;
  void stm(BlockAddressMode am, Register base,
           RegList regs, Condition cond = AL) OVERRIDE;

  void ldrex(Register rd, Register rn, Condition cond = AL) OVERRIDE;
  void strex(Register rd, Register rt, Register rn, Condition cond = AL) OVERRIDE;

  // Miscellaneous instructions.
  void clrex(Condition cond = AL) OVERRIDE;
  void nop(Condition cond = AL) OVERRIDE;

  // Note that gdb sets breakpoints using the undefined instruction 0xe7f001f0.
  void bkpt(uint16_t imm16) OVERRIDE;
  void svc(uint32_t imm24) OVERRIDE;

  void cbz(Register rn, Label* target) OVERRIDE;
  void cbnz(Register rn, Label* target) OVERRIDE;

  // Floating point instructions (VFPv3-D16 and VFPv3-D32 profiles).
  void vmovsr(SRegister sn, Register rt, Condition cond = AL) OVERRIDE;
  void vmovrs(Register rt, SRegister sn, Condition cond = AL) OVERRIDE;
  void vmovsrr(SRegister sm, Register rt, Register rt2, Condition cond = AL) OVERRIDE;
  void vmovrrs(Register rt, Register rt2, SRegister sm, Condition cond = AL) OVERRIDE;
  void vmovdrr(DRegister dm, Register rt, Register rt2, Condition cond = AL) OVERRIDE;
  void vmovrrd(Register rt, Register rt2, DRegister dm, Condition cond = AL) OVERRIDE;
  void vmovs(SRegister sd, SRegister sm, Condition cond = AL) OVERRIDE;
  void vmovd(DRegister dd, DRegister dm, Condition cond = AL) OVERRIDE;

  // Returns false if the immediate cannot be encoded.
  bool vmovs(SRegister sd, float s_imm, Condition cond = AL) OVERRIDE;
  bool vmovd(DRegister dd, double d_imm, Condition cond = AL) OVERRIDE;

  void vldrs(SRegister sd, const Address& ad, Condition cond = AL) OVERRIDE;
  void vstrs(SRegister sd, const Address& ad, Condition cond = AL) OVERRIDE;
  void vldrd(DRegister dd, const Address& ad, Condition cond = AL) OVERRIDE;
  void vstrd(DRegister dd, const Address& ad, Condition cond = AL) OVERRIDE;

  void vadds(SRegister sd, SRegister sn, SRegister sm, Condition cond = AL) OVERRIDE;
  void vaddd(DRegister dd, DRegister dn, DRegister dm, Condition cond = AL) OVERRIDE;
  void vsubs(SRegister sd, SRegister sn, SRegister sm, Condition cond = AL) OVERRIDE;
  void vsubd(DRegister dd, DRegister dn, DRegister dm, Condition cond = AL) OVERRIDE;
  void vmuls(SRegister sd, SRegister sn, SRegister sm, Condition cond = AL) OVERRIDE;
  void vmuld(DRegister dd, DRegister dn, DRegister dm, Condition cond = AL) OVERRIDE;
  void vmlas(SRegister sd, SRegister sn, SRegister sm, Condition cond = AL) OVERRIDE;
  void vmlad(DRegister dd, DRegister dn, DRegister dm, Condition cond = AL) OVERRIDE;
  void vmlss(SRegister sd, SRegister sn, SRegister sm, Condition cond = AL) OVERRIDE;
  void vmlsd(DRegister dd, DRegister dn, DRegister dm, Condition cond = AL) OVERRIDE;
  void vdivs(SRegister sd, SRegister sn, SRegister sm, Condition cond = AL) OVERRIDE;
  void vdivd(DRegister dd, DRegister dn, DRegister dm, Condition cond = AL) OVERRIDE;

  void vabss(SRegister sd, SRegister sm, Condition cond = AL) OVERRIDE;
  void vabsd(DRegister dd, DRegister dm, Condition cond = AL) OVERRIDE;
  void vnegs(SRegister sd, SRegister sm, Condition cond = AL) OVERRIDE;
  void vnegd(DRegister dd, DRegister dm, Condition cond = AL) OVERRIDE;
  void vsqrts(SRegister sd, SRegister sm, Condition cond = AL) OVERRIDE;
  void vsqrtd(DRegister dd, DRegister dm, Condition cond = AL) OVERRIDE;

  void vcvtsd(SRegister sd, DRegister dm, Condition cond = AL) OVERRIDE;
  void vcvtds(DRegister dd, SRegister sm, Condition cond = AL) OVERRIDE;
  void vcvtis(SRegister sd, SRegister sm, Condition cond = AL) OVERRIDE;
  void vcvtid(SRegister sd, DRegister dm, Condition cond = AL) OVERRIDE;
  void vcvtsi(SRegister sd, SRegister sm, Condition cond = AL) OVERRIDE;
  void vcvtdi(DRegister dd, SRegister sm, Condition cond = AL) OVERRIDE;
  void vcvtus(SRegister sd, SRegister sm, Condition cond = AL) OVERRIDE;
  void vcvtud(SRegister sd, DRegister dm, Condition cond = AL) OVERRIDE;
  void vcvtsu(SRegister sd, SRegister sm, Condition cond = AL) OVERRIDE;
  void vcvtdu(DRegister dd, SRegister sm, Condition cond = AL) OVERRIDE;

  void vcmps(SRegister sd, SRegister sm, Condition cond = AL) OVERRIDE;
  void vcmpd(DRegister dd, DRegister dm, Condition cond = AL) OVERRIDE;
  void vcmpsz(SRegister sd, Condition cond = AL) OVERRIDE;
  void vcmpdz(DRegister dd, Condition cond = AL) OVERRIDE;
  void vmstat(Condition cond = AL) OVERRIDE;  // VMRS APSR_nzcv, FPSCR

  void vpushs(SRegister reg, int nregs, Condition cond = AL) OVERRIDE;
  void vpushd(DRegister reg, int nregs, Condition cond = AL) OVERRIDE;
  void vpops(SRegister reg, int nregs, Condition cond = AL) OVERRIDE;
  void vpopd(DRegister reg, int nregs, Condition cond = AL) OVERRIDE;

  // Branch instructions.
  void b(Label* label, Condition cond = AL);
  void bl(Label* label, Condition cond = AL);
  void blx(Register rm, Condition cond = AL) OVERRIDE;
  void bx(Register rm, Condition cond = AL) OVERRIDE;
  void Lsl(Register rd, Register rm, uint32_t shift_imm, bool setcc = false,
           Condition cond = AL) OVERRIDE;
  void Lsr(Register rd, Register rm, uint32_t shift_imm, bool setcc = false,
           Condition cond = AL) OVERRIDE;
  void Asr(Register rd, Register rm, uint32_t shift_imm, bool setcc = false,
           Condition cond = AL) OVERRIDE;
  void Ror(Register rd, Register rm, uint32_t shift_imm, bool setcc = false,
           Condition cond = AL) OVERRIDE;
  void Rrx(Register rd, Register rm, bool setcc = false,
           Condition cond = AL) OVERRIDE;

  void Lsl(Register rd, Register rm, Register rn, bool setcc = false,
           Condition cond = AL) OVERRIDE;
  void Lsr(Register rd, Register rm, Register rn, bool setcc = false,
           Condition cond = AL) OVERRIDE;
  void Asr(Register rd, Register rm, Register rn, bool setcc = false,
           Condition cond = AL) OVERRIDE;
  void Ror(Register rd, Register rm, Register rn, bool setcc = false,
           Condition cond = AL) OVERRIDE;

  void Push(Register rd, Condition cond = AL) OVERRIDE;
  void Pop(Register rd, Condition cond = AL) OVERRIDE;

  void PushList(RegList regs, Condition cond = AL) OVERRIDE;
  void PopList(RegList regs, Condition cond = AL) OVERRIDE;

  void Mov(Register rd, Register rm, Condition cond = AL) OVERRIDE;

  void CompareAndBranchIfZero(Register r, Label* label) OVERRIDE;
  void CompareAndBranchIfNonZero(Register r, Label* label) OVERRIDE;


  // Macros.
  // Add signed constant value to rd. May clobber IP.
  void AddConstant(Register rd, int32_t value, Condition cond = AL) OVERRIDE;
  void AddConstant(Register rd, Register rn, int32_t value,
                   Condition cond = AL) OVERRIDE;
  void AddConstantSetFlags(Register rd, Register rn, int32_t value,
                           Condition cond = AL) OVERRIDE;
  void AddConstantWithCarry(Register rd, Register rn, int32_t value,
                            Condition cond = AL) {}

  // Load and Store. May clobber IP.
  void LoadImmediate(Register rd, int32_t value, Condition cond = AL) OVERRIDE;
  void LoadSImmediate(SRegister sd, float value, Condition cond = AL) {}
  void LoadDImmediate(DRegister dd, double value,
                      Register scratch, Condition cond = AL) {}
  void MarkExceptionHandler(Label* label) OVERRIDE;
  void LoadFromOffset(LoadOperandType type,
                      Register reg,
                      Register base,
                      int32_t offset,
                      Condition cond = AL) OVERRIDE;
  void StoreToOffset(StoreOperandType type,
                     Register reg,
                     Register base,
                     int32_t offset,
                     Condition cond = AL) OVERRIDE;
  void LoadSFromOffset(SRegister reg,
                       Register base,
                       int32_t offset,
                       Condition cond = AL) OVERRIDE;
  void StoreSToOffset(SRegister reg,
                      Register base,
                      int32_t offset,
                      Condition cond = AL) OVERRIDE;
  void LoadDFromOffset(DRegister reg,
                       Register base,
                       int32_t offset,
                       Condition cond = AL) OVERRIDE;
  void StoreDToOffset(DRegister reg,
                      Register base,
                      int32_t offset,
                      Condition cond = AL) OVERRIDE;


  static bool IsInstructionForExceptionHandling(uword pc);

  // Emit data (e.g. encoded instruction or immediate) to the
  // instruction stream.
  void Emit(int32_t value);
  void Bind(Label* label) OVERRIDE;

  void MemoryBarrier(ManagedRegister scratch) OVERRIDE;

 private:
  void EmitType01(Condition cond,
                  int type,
                  Opcode opcode,
                  int set_cc,
                  Register rn,
                  Register rd,
                  const ShifterOperand& so);

  void EmitType5(Condition cond, int offset, bool link);

  void EmitMemOp(Condition cond,
                 bool load,
                 bool byte,
                 Register rd,
                 const Address& ad);

  void EmitMemOpAddressMode3(Condition cond,
                             int32_t mode,
                             Register rd,
                             const Address& ad);

  void EmitMultiMemOp(Condition cond,
                      BlockAddressMode am,
                      bool load,
                      Register base,
                      RegList regs);

  void EmitShiftImmediate(Condition cond,
                          Shift opcode,
                          Register rd,
                          Register rm,
                          const ShifterOperand& so);

  void EmitShiftRegister(Condition cond,
                         Shift opcode,
                         Register rd,
                         Register rm,
                         const ShifterOperand& so);

  void EmitMulOp(Condition cond,
                 int32_t opcode,
                 Register rd,
                 Register rn,
                 Register rm,
                 Register rs);

  void EmitVFPsss(Condition cond,
                  int32_t opcode,
                  SRegister sd,
                  SRegister sn,
                  SRegister sm);

  void EmitVFPddd(Condition cond,
                  int32_t opcode,
                  DRegister dd,
                  DRegister dn,
                  DRegister dm);

  void EmitVFPsd(Condition cond,
                 int32_t opcode,
                 SRegister sd,
                 DRegister dm);

  void EmitVFPds(Condition cond,
                 int32_t opcode,
                 DRegister dd,
                 SRegister sm);

  void EmitVPushPop(uint32_t reg, int nregs, bool push, bool dbl, Condition cond);

  void EmitBranch(Condition cond, Label* label, bool link);
  static int32_t EncodeBranchOffset(int offset, int32_t inst);
  static int DecodeBranchOffset(int32_t inst);
  int32_t EncodeTstOffset(int offset, int32_t inst);
  int DecodeTstOffset(int32_t inst);
};

}  // namespace arm
}  // namespace art

#endif  // ART_COMPILER_UTILS_ARM_ASSEMBLER_ARM32_H_
