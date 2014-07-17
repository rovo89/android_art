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

#ifndef ART_COMPILER_UTILS_ARM_ASSEMBLER_THUMB2_H_
#define ART_COMPILER_UTILS_ARM_ASSEMBLER_THUMB2_H_

#include <vector>

#include "base/logging.h"
#include "constants_arm.h"
#include "utils/arm/managed_register_arm.h"
#include "utils/arm/assembler_arm.h"
#include "offsets.h"
#include "utils.h"

namespace art {
namespace arm {

class Thumb2Assembler FINAL : public ArmAssembler {
 public:
  explicit Thumb2Assembler(bool force_32bit_branches = false)
      : force_32bit_branches_(force_32bit_branches),
        force_32bit_(false),
        it_cond_index_(kNoItCondition),
        next_condition_(AL) {
  }

  virtual ~Thumb2Assembler() {
    for (auto& branch : branches_) {
      delete branch;
    }
  }

  bool IsThumb() const OVERRIDE {
    return true;
  }

  bool IsForced32Bit() const {
    return force_32bit_;
  }

  bool IsForced32BitBranches() const {
    return force_32bit_branches_;
  }

  void FinalizeInstructions(const MemoryRegion& region) OVERRIDE {
    EmitBranches();
    Assembler::FinalizeInstructions(region);
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

  void ldrex(Register rd, Register rn, uint16_t imm, Condition cond = AL);
  void strex(Register rd, Register rt, Register rn, uint16_t imm, Condition cond = AL);


  // Miscellaneous instructions.
  void clrex(Condition cond = AL) OVERRIDE;
  void nop(Condition cond = AL) OVERRIDE;

  void bkpt(uint16_t imm16) OVERRIDE;
  void svc(uint32_t imm24) OVERRIDE;

  // If-then
  void it(Condition firstcond, ItState i1 = kItOmitted,
        ItState i2 = kItOmitted, ItState i3 = kItOmitted) OVERRIDE;

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
  void blx(Label* label);
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

  // Emit data (e.g. encoded instruction or immediate) to the.
  // instruction stream.
  void Emit32(int32_t value);     // Emit a 32 bit instruction in thumb format.
  void Emit16(int16_t value);     // Emit a 16 bit instruction in little endian format.
  void Bind(Label* label) OVERRIDE;

  void MemoryBarrier(ManagedRegister scratch) OVERRIDE;

  // Force the assembler to generate 32 bit instructions.
  void Force32Bit() {
    force_32bit_ = true;
  }

 private:
  // Emit a single 32 or 16 bit data processing instruction.
  void EmitDataProcessing(Condition cond,
                  Opcode opcode,
                  int set_cc,
                  Register rn,
                  Register rd,
                  const ShifterOperand& so);

  // Must the instruction be 32 bits or can it possibly be encoded
  // in 16 bits?
  bool Is32BitDataProcessing(Condition cond,
                  Opcode opcode,
                  int set_cc,
                  Register rn,
                  Register rd,
                  const ShifterOperand& so);

  // Emit a 32 bit data processing instruction.
  void Emit32BitDataProcessing(Condition cond,
                  Opcode opcode,
                  int set_cc,
                  Register rn,
                  Register rd,
                  const ShifterOperand& so);

  // Emit a 16 bit data processing instruction.
  void Emit16BitDataProcessing(Condition cond,
                  Opcode opcode,
                  int set_cc,
                  Register rn,
                  Register rd,
                  const ShifterOperand& so);

  void Emit16BitAddSub(Condition cond,
                       Opcode opcode,
                       int set_cc,
                       Register rn,
                       Register rd,
                       const ShifterOperand& so);

  uint16_t EmitCompareAndBranch(Register rn, uint16_t prev, bool n);

  void EmitLoadStore(Condition cond,
                 bool load,
                 bool byte,
                 bool half,
                 bool is_signed,
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

  void EmitBranch(Condition cond, Label* label, bool link, bool x);
  static int32_t EncodeBranchOffset(int32_t offset, int32_t inst);
  static int DecodeBranchOffset(int32_t inst);
  int32_t EncodeTstOffset(int offset, int32_t inst);
  int DecodeTstOffset(int32_t inst);
  void EmitShift(Register rd, Register rm, Shift shift, uint8_t amount, bool setcc = false);
  void EmitShift(Register rd, Register rn, Shift shift, Register rm, bool setcc = false);

  bool force_32bit_branches_;  // Force the assembler to use 32 bit branch instructions.
  bool force_32bit_;           // Force the assembler to use 32 bit thumb2 instructions.

  // IfThen conditions.  Used to check that conditional instructions match the preceding IT.
  Condition it_conditions_[4];
  uint8_t it_cond_index_;
  Condition next_condition_;

  void SetItCondition(ItState s, Condition cond, uint8_t index);

  void CheckCondition(Condition cond) {
    CHECK_EQ(cond, next_condition_);

    // Move to the next condition if there is one.
    if (it_cond_index_ < 3) {
      ++it_cond_index_;
      next_condition_ = it_conditions_[it_cond_index_];
    } else {
      next_condition_ = AL;
    }
  }

  void CheckConditionLastIt(Condition cond) {
    if (it_cond_index_ < 3) {
      // Check that the next condition is AL.  This means that the
      // current condition is the last in the IT block.
      CHECK_EQ(it_conditions_[it_cond_index_ + 1], AL);
    }
    CheckCondition(cond);
  }

  // Branches.
  //
  // The thumb2 architecture allows branches to be either 16 or 32 bit instructions.  This
  // depends on both the type of branch and the offset to which it is branching.  When
  // generating code for branches we don't know the size before hand (if the branch is
  // going forward, because we haven't seen the target address yet), so we need to assume
  // that it is going to be one of 16 or 32 bits.  When we know the target (the label is 'bound')
  // we can determine the actual size of the branch.  However, if we had guessed wrong before
  // we knew the target there will be no room in the instruction sequence for the new
  // instruction (assume that we never decrease the size of a branch).
  //
  // To handle this, we keep a record of every branch in the program.  The actual instruction
  // encoding for these is delayed until we know the final size of every branch.  When we
  // bind a label to a branch (we then know the target address) we determine if the branch
  // has changed size.  If it has we need to move all the instructions in the buffer after
  // the branch point forward by the change in size of the branch.  This will create a gap
  // in the code big enough for the new branch encoding.  However, since we have moved
  // a chunk of code we need to relocate the branches in that code to their new address.
  //
  // Creating a hole in the code for the new branch encoding might cause another branch that was
  // 16 bits to become 32 bits, so we need to find this in another pass.
  //
  // We also need to deal with a cbz/cbnz instruction that becomes too big for its offset
  // range.  We do this by converting it to two instructions:
  //     cmp Rn, #0
  //     b<cond> target
  // But we also need to handle the case where the conditional branch is out of range and
  // becomes a 32 bit conditional branch.
  //
  // All branches have a 'branch id' which is a 16 bit unsigned number used to identify
  // the branch.  Unresolved labels use the branch id to link to the next unresolved branch.

  class Branch {
   public:
    // Branch type.
    enum Type {
      kUnconditional,             // B.
      kConditional,               // B<cond>.
      kCompareAndBranchZero,      // cbz.
      kCompareAndBranchNonZero,   // cbnz.
      kUnconditionalLink,         // BL.
      kUnconditionalLinkX,        // BLX.
      kUnconditionalX             // BX.
    };

    // Calculated size of branch instruction based on type and offset.
    enum Size {
      k16Bit,
      k32Bit
    };

    // Unresolved branch possibly with a condition.
    Branch(const Thumb2Assembler* assembler, Type type, uint32_t location, Condition cond = AL) :
        assembler_(assembler), type_(type), location_(location),
        target_(kUnresolved),
        cond_(cond), rn_(R0) {
      CHECK(!IsCompareAndBranch());
      size_ = CalculateSize();
    }

    // Unresolved compare-and-branch instruction with a register.
    Branch(const Thumb2Assembler* assembler, Type type, uint32_t location, Register rn) :
        assembler_(assembler), type_(type), location_(location),
        target_(kUnresolved), cond_(AL), rn_(rn) {
      CHECK(IsCompareAndBranch());
      size_ = CalculateSize();
    }

    // Resolved branch (can't be compare-and-branch) with a target and possibly a condition.
    Branch(const Thumb2Assembler* assembler, Type type, uint32_t location, uint32_t target,
           Condition cond = AL) :
           assembler_(assembler), type_(type), location_(location),
           target_(target), cond_(cond), rn_(R0) {
      CHECK(!IsCompareAndBranch());
      // Resolved branch.
      size_ = CalculateSize();
    }

    bool IsCompareAndBranch() const {
      return type_ == kCompareAndBranchNonZero || type_ == kCompareAndBranchZero;
    }

    // Resolve a branch when the target is known.  If this causes the
    // size of the branch to change return true.  Otherwise return false.
    bool Resolve(uint32_t target) {
      target_ = target;
      Size newsize = CalculateSize();
      if (size_ != newsize) {
        size_ = newsize;
        return true;
      }
      return false;
    }

    // Move a cbz/cbnz branch.  This is always forward.
    void Move(int32_t delta) {
      CHECK(IsCompareAndBranch());
      CHECK_GT(delta, 0);
      location_ += delta;
      target_ += delta;
    }

    // Relocate a branch by a given delta.  This changed the location and
    // target if they need to be changed.  It also recalculates the
    // size of the branch instruction.  It returns true if the branch
    // has changed size.
    bool Relocate(uint32_t oldlocation, int32_t delta) {
      if (location_ > oldlocation) {
        location_ += delta;
      }
      if (target_ != kUnresolved) {
        if (target_ > oldlocation) {
          target_ += delta;
        }
      } else {
        return false;       // Don't know the size yet.
      }

      // Calculate the new size.
      Size newsize = CalculateSize();
      if (size_ != newsize) {
        size_ = newsize;
        return true;
      }
      return false;
    }

    Size GetSize() const {
      return size_;
    }

    Type GetType() const {
      return type_;
    }

    uint32_t GetLocation() const {
      return location_;
    }

    // Emit the branch instruction into the assembler buffer.  This does the
    // encoding into the thumb instruction.
    void Emit(AssemblerBuffer* buffer) const;

    // Reset the type and condition to those given.  This used for
    // cbz/cbnz instructions when they are converted to cmp/b<cond>
    void ResetTypeAndCondition(Type type, Condition cond) {
      CHECK(IsCompareAndBranch());
      CHECK(cond == EQ || cond == NE);
      type_ = type;
      cond_ = cond;
    }

    Register GetRegister() const {
      return rn_;
    }

    void ResetSize(Size size) {
      size_ = size;
    }

   private:
    // Calculate the size of the branch instruction based on its type and offset.
    Size CalculateSize() const {
      if (assembler_->IsForced32BitBranches()) {
        return k32Bit;
      }
      if (target_ == kUnresolved) {
        if (assembler_->IsForced32Bit() && (type_ == kUnconditional || type_ == kConditional)) {
          return k32Bit;
        }
        return k16Bit;
      }
      int32_t delta = target_ - location_ - 4;
      if (delta < 0) {
        delta = -delta;
      }
      switch (type_) {
        case kUnconditional:
          if (assembler_->IsForced32Bit() || delta >= (1 << 11)) {
            return k32Bit;
          } else {
            return k16Bit;
          }
        case kConditional:
          if (assembler_->IsForced32Bit() || delta >= (1 << 8)) {
            return k32Bit;
          } else {
            return k16Bit;
          }
        case kCompareAndBranchZero:
        case kCompareAndBranchNonZero:
          if (delta >= (1 << 7)) {
            return k32Bit;      // Will cause this branch to become invalid.
          }
          return k16Bit;

        case kUnconditionalX:
        case kUnconditionalLinkX:
          return k16Bit;
        case kUnconditionalLink:
          return k32Bit;
      }
      LOG(FATAL) << "Cannot reach";
      return k16Bit;
    }

    static constexpr uint32_t kUnresolved = 0xffffffff;     // Value for target_ for unresolved.
    const Thumb2Assembler* assembler_;
    Type type_;
    uint32_t location_;     // Offset into assembler buffer in bytes.
    uint32_t target_;       // Offset into assembler buffer in bytes.
    Size size_;
    Condition cond_;
    const Register rn_;
  };

  std::vector<Branch*> branches_;

  // Add a resolved branch and return its size.
  Branch::Size AddBranch(Branch::Type type, uint32_t location, uint32_t target,
                         Condition cond = AL) {
    branches_.push_back(new Branch(this, type, location, target, cond));
    return branches_[branches_.size()-1]->GetSize();
  }

  // Add a compare and branch (with a register) and return its id.
  uint16_t AddBranch(Branch::Type type, uint32_t location, Register rn) {
    branches_.push_back(new Branch(this, type, location, rn));
    return branches_.size() - 1;
  }

  // Add an unresolved branch and return its id.
  uint16_t AddBranch(Branch::Type type, uint32_t location, Condition cond = AL) {
    branches_.push_back(new Branch(this, type, location, cond));
    return branches_.size() - 1;
  }

  Branch* GetBranch(uint16_t branchid) {
    if (branchid >= branches_.size()) {
      return nullptr;
    }
    return branches_[branchid];
  }

  void EmitBranches();
  void MakeHoleForBranch(uint32_t location, uint32_t size);
};

}  // namespace arm
}  // namespace art

#endif  // ART_COMPILER_UTILS_ARM_ASSEMBLER_THUMB2_H_
