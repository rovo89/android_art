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

#ifndef ART_COMPILER_UTILS_MIPS_ASSEMBLER_MIPS_H_
#define ART_COMPILER_UTILS_MIPS_ASSEMBLER_MIPS_H_

#include <utility>
#include <vector>

#include "arch/mips/instruction_set_features_mips.h"
#include "base/macros.h"
#include "constants_mips.h"
#include "globals.h"
#include "managed_register_mips.h"
#include "offsets.h"
#include "utils/assembler.h"
#include "utils/label.h"

namespace art {
namespace mips {

static constexpr size_t kMipsWordSize = 4;
static constexpr size_t kMipsDoublewordSize = 8;

enum LoadOperandType {
  kLoadSignedByte,
  kLoadUnsignedByte,
  kLoadSignedHalfword,
  kLoadUnsignedHalfword,
  kLoadWord,
  kLoadDoubleword
};

enum StoreOperandType {
  kStoreByte,
  kStoreHalfword,
  kStoreWord,
  kStoreDoubleword
};

// Used to test the values returned by ClassS/ClassD.
enum FPClassMaskType {
  kSignalingNaN      = 0x001,
  kQuietNaN          = 0x002,
  kNegativeInfinity  = 0x004,
  kNegativeNormal    = 0x008,
  kNegativeSubnormal = 0x010,
  kNegativeZero      = 0x020,
  kPositiveInfinity  = 0x040,
  kPositiveNormal    = 0x080,
  kPositiveSubnormal = 0x100,
  kPositiveZero      = 0x200,
};

class MipsLabel : public Label {
 public:
  MipsLabel() : prev_branch_id_plus_one_(0) {}

  MipsLabel(MipsLabel&& src)
      : Label(std::move(src)), prev_branch_id_plus_one_(src.prev_branch_id_plus_one_) {}

 private:
  uint32_t prev_branch_id_plus_one_;  // To get distance from preceding branch, if any.

  friend class MipsAssembler;
  DISALLOW_COPY_AND_ASSIGN(MipsLabel);
};

// Slowpath entered when Thread::Current()->_exception is non-null.
class MipsExceptionSlowPath {
 public:
  explicit MipsExceptionSlowPath(MipsManagedRegister scratch, size_t stack_adjust)
      : scratch_(scratch), stack_adjust_(stack_adjust) {}

  MipsExceptionSlowPath(MipsExceptionSlowPath&& src)
      : scratch_(src.scratch_),
        stack_adjust_(src.stack_adjust_),
        exception_entry_(std::move(src.exception_entry_)) {}

 private:
  MipsLabel* Entry() { return &exception_entry_; }
  const MipsManagedRegister scratch_;
  const size_t stack_adjust_;
  MipsLabel exception_entry_;

  friend class MipsAssembler;
  DISALLOW_COPY_AND_ASSIGN(MipsExceptionSlowPath);
};

class MipsAssembler FINAL : public Assembler {
 public:
  explicit MipsAssembler(ArenaAllocator* arena,
                         const MipsInstructionSetFeatures* instruction_set_features = nullptr)
      : Assembler(arena),
        overwriting_(false),
        overwrite_location_(0),
        last_position_adjustment_(0),
        last_old_position_(0),
        last_branch_id_(0),
        isa_features_(instruction_set_features) {
    cfi().DelayEmittingAdvancePCs();
  }

  virtual ~MipsAssembler() {
    for (auto& branch : branches_) {
      CHECK(branch.IsResolved());
    }
  }

  // Emit Machine Instructions.
  void Addu(Register rd, Register rs, Register rt);
  void Addiu(Register rt, Register rs, uint16_t imm16);
  void Subu(Register rd, Register rs, Register rt);

  void MultR2(Register rs, Register rt);  // R2
  void MultuR2(Register rs, Register rt);  // R2
  void DivR2(Register rs, Register rt);  // R2
  void DivuR2(Register rs, Register rt);  // R2
  void MulR2(Register rd, Register rs, Register rt);  // R2
  void DivR2(Register rd, Register rs, Register rt);  // R2
  void ModR2(Register rd, Register rs, Register rt);  // R2
  void DivuR2(Register rd, Register rs, Register rt);  // R2
  void ModuR2(Register rd, Register rs, Register rt);  // R2
  void MulR6(Register rd, Register rs, Register rt);  // R6
  void MuhR6(Register rd, Register rs, Register rt);  // R6
  void MuhuR6(Register rd, Register rs, Register rt);  // R6
  void DivR6(Register rd, Register rs, Register rt);  // R6
  void ModR6(Register rd, Register rs, Register rt);  // R6
  void DivuR6(Register rd, Register rs, Register rt);  // R6
  void ModuR6(Register rd, Register rs, Register rt);  // R6

  void And(Register rd, Register rs, Register rt);
  void Andi(Register rt, Register rs, uint16_t imm16);
  void Or(Register rd, Register rs, Register rt);
  void Ori(Register rt, Register rs, uint16_t imm16);
  void Xor(Register rd, Register rs, Register rt);
  void Xori(Register rt, Register rs, uint16_t imm16);
  void Nor(Register rd, Register rs, Register rt);

  void Movz(Register rd, Register rs, Register rt);  // R2
  void Movn(Register rd, Register rs, Register rt);  // R2
  void Seleqz(Register rd, Register rs, Register rt);  // R6
  void Selnez(Register rd, Register rs, Register rt);  // R6
  void ClzR6(Register rd, Register rs);
  void ClzR2(Register rd, Register rs);
  void CloR6(Register rd, Register rs);
  void CloR2(Register rd, Register rs);

  void Seb(Register rd, Register rt);  // R2+
  void Seh(Register rd, Register rt);  // R2+
  void Wsbh(Register rd, Register rt);  // R2+
  void Bitswap(Register rd, Register rt);  // R6

  void Sll(Register rd, Register rt, int shamt);
  void Srl(Register rd, Register rt, int shamt);
  void Rotr(Register rd, Register rt, int shamt);  // R2+
  void Sra(Register rd, Register rt, int shamt);
  void Sllv(Register rd, Register rt, Register rs);
  void Srlv(Register rd, Register rt, Register rs);
  void Rotrv(Register rd, Register rt, Register rs);  // R2+
  void Srav(Register rd, Register rt, Register rs);
  void Ext(Register rd, Register rt, int pos, int size);  // R2+
  void Ins(Register rd, Register rt, int pos, int size);  // R2+

  void Lb(Register rt, Register rs, uint16_t imm16);
  void Lh(Register rt, Register rs, uint16_t imm16);
  void Lw(Register rt, Register rs, uint16_t imm16);
  void Lwl(Register rt, Register rs, uint16_t imm16);
  void Lwr(Register rt, Register rs, uint16_t imm16);
  void Lbu(Register rt, Register rs, uint16_t imm16);
  void Lhu(Register rt, Register rs, uint16_t imm16);
  void Lui(Register rt, uint16_t imm16);
  void Sync(uint32_t stype);
  void Mfhi(Register rd);  // R2
  void Mflo(Register rd);  // R2

  void Sb(Register rt, Register rs, uint16_t imm16);
  void Sh(Register rt, Register rs, uint16_t imm16);
  void Sw(Register rt, Register rs, uint16_t imm16);
  void Swl(Register rt, Register rs, uint16_t imm16);
  void Swr(Register rt, Register rs, uint16_t imm16);

  void LlR2(Register rt, Register base, int16_t imm16 = 0);
  void ScR2(Register rt, Register base, int16_t imm16 = 0);
  void LlR6(Register rt, Register base, int16_t imm9 = 0);
  void ScR6(Register rt, Register base, int16_t imm9 = 0);

  void Slt(Register rd, Register rs, Register rt);
  void Sltu(Register rd, Register rs, Register rt);
  void Slti(Register rt, Register rs, uint16_t imm16);
  void Sltiu(Register rt, Register rs, uint16_t imm16);

  void B(uint16_t imm16);
  void Beq(Register rs, Register rt, uint16_t imm16);
  void Bne(Register rs, Register rt, uint16_t imm16);
  void Beqz(Register rt, uint16_t imm16);
  void Bnez(Register rt, uint16_t imm16);
  void Bltz(Register rt, uint16_t imm16);
  void Bgez(Register rt, uint16_t imm16);
  void Blez(Register rt, uint16_t imm16);
  void Bgtz(Register rt, uint16_t imm16);
  void Bc1f(uint16_t imm16);  // R2
  void Bc1f(int cc, uint16_t imm16);  // R2
  void Bc1t(uint16_t imm16);  // R2
  void Bc1t(int cc, uint16_t imm16);  // R2
  void J(uint32_t addr26);
  void Jal(uint32_t addr26);
  void Jalr(Register rd, Register rs);
  void Jalr(Register rs);
  void Jr(Register rs);
  void Nal();
  void Auipc(Register rs, uint16_t imm16);  // R6
  void Addiupc(Register rs, uint32_t imm19);  // R6
  void Bc(uint32_t imm26);  // R6
  void Jic(Register rt, uint16_t imm16);  // R6
  void Jialc(Register rt, uint16_t imm16);  // R6
  void Bltc(Register rs, Register rt, uint16_t imm16);  // R6
  void Bltzc(Register rt, uint16_t imm16);  // R6
  void Bgtzc(Register rt, uint16_t imm16);  // R6
  void Bgec(Register rs, Register rt, uint16_t imm16);  // R6
  void Bgezc(Register rt, uint16_t imm16);  // R6
  void Blezc(Register rt, uint16_t imm16);  // R6
  void Bltuc(Register rs, Register rt, uint16_t imm16);  // R6
  void Bgeuc(Register rs, Register rt, uint16_t imm16);  // R6
  void Beqc(Register rs, Register rt, uint16_t imm16);  // R6
  void Bnec(Register rs, Register rt, uint16_t imm16);  // R6
  void Beqzc(Register rs, uint32_t imm21);  // R6
  void Bnezc(Register rs, uint32_t imm21);  // R6
  void Bc1eqz(FRegister ft, uint16_t imm16);  // R6
  void Bc1nez(FRegister ft, uint16_t imm16);  // R6

  void AddS(FRegister fd, FRegister fs, FRegister ft);
  void SubS(FRegister fd, FRegister fs, FRegister ft);
  void MulS(FRegister fd, FRegister fs, FRegister ft);
  void DivS(FRegister fd, FRegister fs, FRegister ft);
  void AddD(FRegister fd, FRegister fs, FRegister ft);
  void SubD(FRegister fd, FRegister fs, FRegister ft);
  void MulD(FRegister fd, FRegister fs, FRegister ft);
  void DivD(FRegister fd, FRegister fs, FRegister ft);
  void SqrtS(FRegister fd, FRegister fs);
  void SqrtD(FRegister fd, FRegister fs);
  void AbsS(FRegister fd, FRegister fs);
  void AbsD(FRegister fd, FRegister fs);
  void MovS(FRegister fd, FRegister fs);
  void MovD(FRegister fd, FRegister fs);
  void NegS(FRegister fd, FRegister fs);
  void NegD(FRegister fd, FRegister fs);

  void CunS(FRegister fs, FRegister ft);  // R2
  void CunS(int cc, FRegister fs, FRegister ft);  // R2
  void CeqS(FRegister fs, FRegister ft);  // R2
  void CeqS(int cc, FRegister fs, FRegister ft);  // R2
  void CueqS(FRegister fs, FRegister ft);  // R2
  void CueqS(int cc, FRegister fs, FRegister ft);  // R2
  void ColtS(FRegister fs, FRegister ft);  // R2
  void ColtS(int cc, FRegister fs, FRegister ft);  // R2
  void CultS(FRegister fs, FRegister ft);  // R2
  void CultS(int cc, FRegister fs, FRegister ft);  // R2
  void ColeS(FRegister fs, FRegister ft);  // R2
  void ColeS(int cc, FRegister fs, FRegister ft);  // R2
  void CuleS(FRegister fs, FRegister ft);  // R2
  void CuleS(int cc, FRegister fs, FRegister ft);  // R2
  void CunD(FRegister fs, FRegister ft);  // R2
  void CunD(int cc, FRegister fs, FRegister ft);  // R2
  void CeqD(FRegister fs, FRegister ft);  // R2
  void CeqD(int cc, FRegister fs, FRegister ft);  // R2
  void CueqD(FRegister fs, FRegister ft);  // R2
  void CueqD(int cc, FRegister fs, FRegister ft);  // R2
  void ColtD(FRegister fs, FRegister ft);  // R2
  void ColtD(int cc, FRegister fs, FRegister ft);  // R2
  void CultD(FRegister fs, FRegister ft);  // R2
  void CultD(int cc, FRegister fs, FRegister ft);  // R2
  void ColeD(FRegister fs, FRegister ft);  // R2
  void ColeD(int cc, FRegister fs, FRegister ft);  // R2
  void CuleD(FRegister fs, FRegister ft);  // R2
  void CuleD(int cc, FRegister fs, FRegister ft);  // R2
  void CmpUnS(FRegister fd, FRegister fs, FRegister ft);  // R6
  void CmpEqS(FRegister fd, FRegister fs, FRegister ft);  // R6
  void CmpUeqS(FRegister fd, FRegister fs, FRegister ft);  // R6
  void CmpLtS(FRegister fd, FRegister fs, FRegister ft);  // R6
  void CmpUltS(FRegister fd, FRegister fs, FRegister ft);  // R6
  void CmpLeS(FRegister fd, FRegister fs, FRegister ft);  // R6
  void CmpUleS(FRegister fd, FRegister fs, FRegister ft);  // R6
  void CmpOrS(FRegister fd, FRegister fs, FRegister ft);  // R6
  void CmpUneS(FRegister fd, FRegister fs, FRegister ft);  // R6
  void CmpNeS(FRegister fd, FRegister fs, FRegister ft);  // R6
  void CmpUnD(FRegister fd, FRegister fs, FRegister ft);  // R6
  void CmpEqD(FRegister fd, FRegister fs, FRegister ft);  // R6
  void CmpUeqD(FRegister fd, FRegister fs, FRegister ft);  // R6
  void CmpLtD(FRegister fd, FRegister fs, FRegister ft);  // R6
  void CmpUltD(FRegister fd, FRegister fs, FRegister ft);  // R6
  void CmpLeD(FRegister fd, FRegister fs, FRegister ft);  // R6
  void CmpUleD(FRegister fd, FRegister fs, FRegister ft);  // R6
  void CmpOrD(FRegister fd, FRegister fs, FRegister ft);  // R6
  void CmpUneD(FRegister fd, FRegister fs, FRegister ft);  // R6
  void CmpNeD(FRegister fd, FRegister fs, FRegister ft);  // R6
  void Movf(Register rd, Register rs, int cc = 0);  // R2
  void Movt(Register rd, Register rs, int cc = 0);  // R2
  void MovfS(FRegister fd, FRegister fs, int cc = 0);  // R2
  void MovfD(FRegister fd, FRegister fs, int cc = 0);  // R2
  void MovtS(FRegister fd, FRegister fs, int cc = 0);  // R2
  void MovtD(FRegister fd, FRegister fs, int cc = 0);  // R2
  void SelS(FRegister fd, FRegister fs, FRegister ft);  // R6
  void SelD(FRegister fd, FRegister fs, FRegister ft);  // R6
  void ClassS(FRegister fd, FRegister fs);  // R6
  void ClassD(FRegister fd, FRegister fs);  // R6
  void MinS(FRegister fd, FRegister fs, FRegister ft);  // R6
  void MinD(FRegister fd, FRegister fs, FRegister ft);  // R6
  void MaxS(FRegister fd, FRegister fs, FRegister ft);  // R6
  void MaxD(FRegister fd, FRegister fs, FRegister ft);  // R6

  void TruncLS(FRegister fd, FRegister fs);  // R2+, FR=1
  void TruncLD(FRegister fd, FRegister fs);  // R2+, FR=1
  void TruncWS(FRegister fd, FRegister fs);
  void TruncWD(FRegister fd, FRegister fs);
  void Cvtsw(FRegister fd, FRegister fs);
  void Cvtdw(FRegister fd, FRegister fs);
  void Cvtsd(FRegister fd, FRegister fs);
  void Cvtds(FRegister fd, FRegister fs);
  void Cvtsl(FRegister fd, FRegister fs);  // R2+, FR=1
  void Cvtdl(FRegister fd, FRegister fs);  // R2+, FR=1
  void FloorWS(FRegister fd, FRegister fs);
  void FloorWD(FRegister fd, FRegister fs);

  void Mfc1(Register rt, FRegister fs);
  void Mtc1(Register rt, FRegister fs);
  void Mfhc1(Register rt, FRegister fs);
  void Mthc1(Register rt, FRegister fs);
  void MoveFromFpuHigh(Register rt, FRegister fs);
  void MoveToFpuHigh(Register rt, FRegister fs);
  void Lwc1(FRegister ft, Register rs, uint16_t imm16);
  void Ldc1(FRegister ft, Register rs, uint16_t imm16);
  void Swc1(FRegister ft, Register rs, uint16_t imm16);
  void Sdc1(FRegister ft, Register rs, uint16_t imm16);

  void Break();
  void Nop();
  void Move(Register rd, Register rs);
  void Clear(Register rd);
  void Not(Register rd, Register rs);

  // Higher level composite instructions.
  void LoadConst32(Register rd, int32_t value);
  void LoadConst64(Register reg_hi, Register reg_lo, int64_t value);
  void LoadDConst64(FRegister rd, int64_t value, Register temp);
  void LoadSConst32(FRegister r, int32_t value, Register temp);
  void StoreConst32ToOffset(int32_t value, Register base, int32_t offset, Register temp);
  void StoreConst64ToOffset(int64_t value, Register base, int32_t offset, Register temp);
  void Addiu32(Register rt, Register rs, int32_t value, Register rtmp = AT);

  // These will generate R2 branches or R6 branches as appropriate.
  void Bind(MipsLabel* label);
  void B(MipsLabel* label);
  void Jalr(MipsLabel* label, Register indirect_reg);
  void Beq(Register rs, Register rt, MipsLabel* label);
  void Bne(Register rs, Register rt, MipsLabel* label);
  void Beqz(Register rt, MipsLabel* label);
  void Bnez(Register rt, MipsLabel* label);
  void Bltz(Register rt, MipsLabel* label);
  void Bgez(Register rt, MipsLabel* label);
  void Blez(Register rt, MipsLabel* label);
  void Bgtz(Register rt, MipsLabel* label);
  void Blt(Register rs, Register rt, MipsLabel* label);
  void Bge(Register rs, Register rt, MipsLabel* label);
  void Bltu(Register rs, Register rt, MipsLabel* label);
  void Bgeu(Register rs, Register rt, MipsLabel* label);
  void Bc1f(MipsLabel* label);  // R2
  void Bc1f(int cc, MipsLabel* label);  // R2
  void Bc1t(MipsLabel* label);  // R2
  void Bc1t(int cc, MipsLabel* label);  // R2
  void Bc1eqz(FRegister ft, MipsLabel* label);  // R6
  void Bc1nez(FRegister ft, MipsLabel* label);  // R6

  void EmitLoad(ManagedRegister m_dst, Register src_register, int32_t src_offset, size_t size);
  void LoadFromOffset(LoadOperandType type, Register reg, Register base, int32_t offset);
  void LoadSFromOffset(FRegister reg, Register base, int32_t offset);
  void LoadDFromOffset(FRegister reg, Register base, int32_t offset);
  void StoreToOffset(StoreOperandType type, Register reg, Register base, int32_t offset);
  void StoreSToOffset(FRegister reg, Register base, int32_t offset);
  void StoreDToOffset(FRegister reg, Register base, int32_t offset);

  // Emit data (e.g. encoded instruction or immediate) to the instruction stream.
  void Emit(uint32_t value);

  // Push/pop composite routines.
  void Push(Register rs);
  void Pop(Register rd);
  void PopAndReturn(Register rd, Register rt);

  void Bind(Label* label) OVERRIDE {
    Bind(down_cast<MipsLabel*>(label));
  }
  void Jump(Label* label ATTRIBUTE_UNUSED) OVERRIDE {
    UNIMPLEMENTED(FATAL) << "Do not use Jump for MIPS";
  }

  //
  // Overridden common assembler high-level functionality.
  //

  // Emit code that will create an activation on the stack.
  void BuildFrame(size_t frame_size,
                  ManagedRegister method_reg,
                  const std::vector<ManagedRegister>& callee_save_regs,
                  const ManagedRegisterEntrySpills& entry_spills) OVERRIDE;

  // Emit code that will remove an activation from the stack.
  void RemoveFrame(size_t frame_size, const std::vector<ManagedRegister>& callee_save_regs)
      OVERRIDE;

  void IncreaseFrameSize(size_t adjust) OVERRIDE;
  void DecreaseFrameSize(size_t adjust) OVERRIDE;

  // Store routines.
  void Store(FrameOffset offs, ManagedRegister msrc, size_t size) OVERRIDE;
  void StoreRef(FrameOffset dest, ManagedRegister msrc) OVERRIDE;
  void StoreRawPtr(FrameOffset dest, ManagedRegister msrc) OVERRIDE;

  void StoreImmediateToFrame(FrameOffset dest, uint32_t imm, ManagedRegister mscratch) OVERRIDE;

  void StoreImmediateToThread32(ThreadOffset<kMipsWordSize> dest,
                                uint32_t imm,
                                ManagedRegister mscratch) OVERRIDE;

  void StoreStackOffsetToThread32(ThreadOffset<kMipsWordSize> thr_offs,
                                  FrameOffset fr_offs,
                                  ManagedRegister mscratch) OVERRIDE;

  void StoreStackPointerToThread32(ThreadOffset<kMipsWordSize> thr_offs) OVERRIDE;

  void StoreSpanning(FrameOffset dest,
                     ManagedRegister msrc,
                     FrameOffset in_off,
                     ManagedRegister mscratch) OVERRIDE;

  // Load routines.
  void Load(ManagedRegister mdest, FrameOffset src, size_t size) OVERRIDE;

  void LoadFromThread32(ManagedRegister mdest,
                        ThreadOffset<kMipsWordSize> src,
                        size_t size) OVERRIDE;

  void LoadRef(ManagedRegister dest, FrameOffset src) OVERRIDE;

  void LoadRef(ManagedRegister mdest,
               ManagedRegister base,
               MemberOffset offs,
               bool unpoison_reference) OVERRIDE;

  void LoadRawPtr(ManagedRegister mdest, ManagedRegister base, Offset offs) OVERRIDE;

  void LoadRawPtrFromThread32(ManagedRegister mdest, ThreadOffset<kMipsWordSize> offs) OVERRIDE;

  // Copying routines.
  void Move(ManagedRegister mdest, ManagedRegister msrc, size_t size) OVERRIDE;

  void CopyRawPtrFromThread32(FrameOffset fr_offs,
                              ThreadOffset<kMipsWordSize> thr_offs,
                              ManagedRegister mscratch) OVERRIDE;

  void CopyRawPtrToThread32(ThreadOffset<kMipsWordSize> thr_offs,
                            FrameOffset fr_offs,
                            ManagedRegister mscratch) OVERRIDE;

  void CopyRef(FrameOffset dest, FrameOffset src, ManagedRegister mscratch) OVERRIDE;

  void Copy(FrameOffset dest, FrameOffset src, ManagedRegister mscratch, size_t size) OVERRIDE;

  void Copy(FrameOffset dest,
            ManagedRegister src_base,
            Offset src_offset,
            ManagedRegister mscratch,
            size_t size) OVERRIDE;

  void Copy(ManagedRegister dest_base,
            Offset dest_offset,
            FrameOffset src,
            ManagedRegister mscratch,
            size_t size) OVERRIDE;

  void Copy(FrameOffset dest,
            FrameOffset src_base,
            Offset src_offset,
            ManagedRegister mscratch,
            size_t size) OVERRIDE;

  void Copy(ManagedRegister dest,
            Offset dest_offset,
            ManagedRegister src,
            Offset src_offset,
            ManagedRegister mscratch,
            size_t size) OVERRIDE;

  void Copy(FrameOffset dest,
            Offset dest_offset,
            FrameOffset src,
            Offset src_offset,
            ManagedRegister mscratch,
            size_t size) OVERRIDE;

  void MemoryBarrier(ManagedRegister) OVERRIDE;

  // Sign extension.
  void SignExtend(ManagedRegister mreg, size_t size) OVERRIDE;

  // Zero extension.
  void ZeroExtend(ManagedRegister mreg, size_t size) OVERRIDE;

  // Exploit fast access in managed code to Thread::Current().
  void GetCurrentThread(ManagedRegister tr) OVERRIDE;
  void GetCurrentThread(FrameOffset dest_offset, ManagedRegister mscratch) OVERRIDE;

  // Set up out_reg to hold a Object** into the handle scope, or to be null if the
  // value is null and null_allowed. in_reg holds a possibly stale reference
  // that can be used to avoid loading the handle scope entry to see if the value is
  // null.
  void CreateHandleScopeEntry(ManagedRegister out_reg,
                              FrameOffset handlescope_offset,
                              ManagedRegister in_reg,
                              bool null_allowed) OVERRIDE;

  // Set up out_off to hold a Object** into the handle scope, or to be null if the
  // value is null and null_allowed.
  void CreateHandleScopeEntry(FrameOffset out_off,
                              FrameOffset handlescope_offset,
                              ManagedRegister mscratch,
                              bool null_allowed) OVERRIDE;

  // src holds a handle scope entry (Object**) load this into dst.
  void LoadReferenceFromHandleScope(ManagedRegister dst, ManagedRegister src) OVERRIDE;

  // Heap::VerifyObject on src. In some cases (such as a reference to this) we
  // know that src may not be null.
  void VerifyObject(ManagedRegister src, bool could_be_null) OVERRIDE;
  void VerifyObject(FrameOffset src, bool could_be_null) OVERRIDE;

  // Call to address held at [base+offset].
  void Call(ManagedRegister base, Offset offset, ManagedRegister mscratch) OVERRIDE;
  void Call(FrameOffset base, Offset offset, ManagedRegister mscratch) OVERRIDE;
  void CallFromThread32(ThreadOffset<kMipsWordSize> offset, ManagedRegister mscratch) OVERRIDE;

  // Generate code to check if Thread::Current()->exception_ is non-null
  // and branch to a ExceptionSlowPath if it is.
  void ExceptionPoll(ManagedRegister mscratch, size_t stack_adjust) OVERRIDE;

  // Emit slow paths queued during assembly and promote short branches to long if needed.
  void FinalizeCode() OVERRIDE;

  // Emit branches and finalize all instructions.
  void FinalizeInstructions(const MemoryRegion& region);

  // Returns the (always-)current location of a label (can be used in class CodeGeneratorMIPS,
  // must be used instead of MipsLabel::GetPosition()).
  uint32_t GetLabelLocation(MipsLabel* label) const;

  // Get the final position of a label after local fixup based on the old position
  // recorded before FinalizeCode().
  uint32_t GetAdjustedPosition(uint32_t old_position);

  enum BranchCondition {
    kCondLT,
    kCondGE,
    kCondLE,
    kCondGT,
    kCondLTZ,
    kCondGEZ,
    kCondLEZ,
    kCondGTZ,
    kCondEQ,
    kCondNE,
    kCondEQZ,
    kCondNEZ,
    kCondLTU,
    kCondGEU,
    kCondF,    // Floating-point predicate false.
    kCondT,    // Floating-point predicate true.
    kUncond,
  };
  friend std::ostream& operator<<(std::ostream& os, const BranchCondition& rhs);

 private:
  class Branch {
   public:
    enum Type {
      // R2 short branches.
      kUncondBranch,
      kCondBranch,
      kCall,
      // R2 long branches.
      kLongUncondBranch,
      kLongCondBranch,
      kLongCall,
      // R6 short branches.
      kR6UncondBranch,
      kR6CondBranch,
      kR6Call,
      // R6 long branches.
      kR6LongUncondBranch,
      kR6LongCondBranch,
      kR6LongCall,
    };
    // Bit sizes of offsets defined as enums to minimize chance of typos.
    enum OffsetBits {
      kOffset16 = 16,
      kOffset18 = 18,
      kOffset21 = 21,
      kOffset23 = 23,
      kOffset28 = 28,
      kOffset32 = 32,
    };

    static constexpr uint32_t kUnresolved = 0xffffffff;  // Unresolved target_
    static constexpr int32_t kMaxBranchLength = 32;
    static constexpr int32_t kMaxBranchSize = kMaxBranchLength * sizeof(uint32_t);

    struct BranchInfo {
      // Branch length as a number of 4-byte-long instructions.
      uint32_t length;
      // Ordinal number (0-based) of the first (or the only) instruction that contains the branch's
      // PC-relative offset (or its most significant 16-bit half, which goes first).
      uint32_t instr_offset;
      // Different MIPS instructions with PC-relative offsets apply said offsets to slightly
      // different origins, e.g. to PC or PC+4. Encode the origin distance (as a number of 4-byte
      // instructions) from the instruction containing the offset.
      uint32_t pc_org;
      // How large (in bits) a PC-relative offset can be for a given type of branch (kR6CondBranch
      // is an exception: use kOffset23 for beqzc/bnezc).
      OffsetBits offset_size;
      // Some MIPS instructions with PC-relative offsets shift the offset by 2. Encode the shift
      // count.
      int offset_shift;
    };
    static const BranchInfo branch_info_[/* Type */];

    // Unconditional branch.
    Branch(bool is_r6, uint32_t location, uint32_t target);
    // Conditional branch.
    Branch(bool is_r6,
           uint32_t location,
           uint32_t target,
           BranchCondition condition,
           Register lhs_reg,
           Register rhs_reg = ZERO);
    // Call (branch and link) that stores the target address in a given register (i.e. T9).
    Branch(bool is_r6, uint32_t location, uint32_t target, Register indirect_reg);

    // Some conditional branches with lhs = rhs are effectively NOPs, while some
    // others are effectively unconditional. MIPSR6 conditional branches require lhs != rhs.
    // So, we need a way to identify such branches in order to emit no instructions for them
    // or change them to unconditional.
    static bool IsNop(BranchCondition condition, Register lhs, Register rhs);
    static bool IsUncond(BranchCondition condition, Register lhs, Register rhs);

    static BranchCondition OppositeCondition(BranchCondition cond);

    Type GetType() const;
    BranchCondition GetCondition() const;
    Register GetLeftRegister() const;
    Register GetRightRegister() const;
    uint32_t GetTarget() const;
    uint32_t GetLocation() const;
    uint32_t GetOldLocation() const;
    uint32_t GetLength() const;
    uint32_t GetOldLength() const;
    uint32_t GetSize() const;
    uint32_t GetOldSize() const;
    uint32_t GetEndLocation() const;
    uint32_t GetOldEndLocation() const;
    bool IsLong() const;
    bool IsResolved() const;

    // Returns the bit size of the signed offset that the branch instruction can handle.
    OffsetBits GetOffsetSize() const;

    // Calculates the distance between two byte locations in the assembler buffer and
    // returns the number of bits needed to represent the distance as a signed integer.
    //
    // Branch instructions have signed offsets of 16, 19 (addiupc), 21 (beqzc/bnezc),
    // and 26 (bc) bits, which are additionally shifted left 2 positions at run time.
    //
    // Composite branches (made of several instructions) with longer reach have 32-bit
    // offsets encoded as 2 16-bit "halves" in two instructions (high half goes first).
    // The composite branches cover the range of PC + +/-2GB on MIPS32 CPUs. However,
    // the range is not end-to-end on MIPS64 (unless addresses are forced to zero- or
    // sign-extend from 32 to 64 bits by the appropriate CPU configuration).
    // Consider the following implementation of a long unconditional branch, for
    // example:
    //
    //   auipc at, offset_31_16  // at = pc + sign_extend(offset_31_16) << 16
    //   jic   at, offset_15_0   // pc = at + sign_extend(offset_15_0)
    //
    // Both of the above instructions take 16-bit signed offsets as immediate operands.
    // When bit 15 of offset_15_0 is 1, it effectively causes subtraction of 0x10000
    // due to sign extension. This must be compensated for by incrementing offset_31_16
    // by 1. offset_31_16 can only be incremented by 1 if it's not 0x7FFF. If it is
    // 0x7FFF, adding 1 will overflow the positive offset into the negative range.
    // Therefore, the long branch range is something like from PC - 0x80000000 to
    // PC + 0x7FFF7FFF, IOW, shorter by 32KB on one side.
    //
    // The returned values are therefore: 18, 21, 23, 28 and 32. There's also a special
    // case with the addiu instruction and a 16 bit offset.
    static OffsetBits GetOffsetSizeNeeded(uint32_t location, uint32_t target);

    // Resolve a branch when the target is known.
    void Resolve(uint32_t target);

    // Relocate a branch by a given delta if needed due to expansion of this or another
    // branch at a given location by this delta (just changes location_ and target_).
    void Relocate(uint32_t expand_location, uint32_t delta);

    // If the branch is short, changes its type to long.
    void PromoteToLong();

    // If necessary, updates the type by promoting a short branch to a long branch
    // based on the branch location and target. Returns the amount (in bytes) by
    // which the branch size has increased.
    // max_short_distance caps the maximum distance between location_ and target_
    // that is allowed for short branches. This is for debugging/testing purposes.
    // max_short_distance = 0 forces all short branches to become long.
    // Use the implicit default argument when not debugging/testing.
    uint32_t PromoteIfNeeded(uint32_t max_short_distance = std::numeric_limits<uint32_t>::max());

    // Returns the location of the instruction(s) containing the offset.
    uint32_t GetOffsetLocation() const;

    // Calculates and returns the offset ready for encoding in the branch instruction(s).
    uint32_t GetOffset() const;

   private:
    // Completes branch construction by determining and recording its type.
    void InitializeType(bool is_call, bool is_r6);
    // Helper for the above.
    void InitShortOrLong(OffsetBits ofs_size, Type short_type, Type long_type);

    uint32_t old_location_;      // Offset into assembler buffer in bytes.
    uint32_t location_;          // Offset into assembler buffer in bytes.
    uint32_t target_;            // Offset into assembler buffer in bytes.

    uint32_t lhs_reg_;           // Left-hand side register in conditional branches or
                                 // indirect call register.
    uint32_t rhs_reg_;           // Right-hand side register in conditional branches.
    BranchCondition condition_;  // Condition for conditional branches.

    Type type_;                  // Current type of the branch.
    Type old_type_;              // Initial type of the branch.
  };
  friend std::ostream& operator<<(std::ostream& os, const Branch::Type& rhs);
  friend std::ostream& operator<<(std::ostream& os, const Branch::OffsetBits& rhs);

  void EmitR(int opcode, Register rs, Register rt, Register rd, int shamt, int funct);
  void EmitI(int opcode, Register rs, Register rt, uint16_t imm);
  void EmitI21(int opcode, Register rs, uint32_t imm21);
  void EmitI26(int opcode, uint32_t imm26);
  void EmitFR(int opcode, int fmt, FRegister ft, FRegister fs, FRegister fd, int funct);
  void EmitFI(int opcode, int fmt, FRegister rt, uint16_t imm);
  void EmitBcondR2(BranchCondition cond, Register rs, Register rt, uint16_t imm16);
  void EmitBcondR6(BranchCondition cond, Register rs, Register rt, uint32_t imm16_21);

  void Buncond(MipsLabel* label);
  void Bcond(MipsLabel* label, BranchCondition condition, Register lhs, Register rhs = ZERO);
  void Call(MipsLabel* label, Register indirect_reg);
  void FinalizeLabeledBranch(MipsLabel* label);

  Branch* GetBranch(uint32_t branch_id);
  const Branch* GetBranch(uint32_t branch_id) const;

  void PromoteBranches();
  void EmitBranch(Branch* branch);
  void EmitBranches();
  void PatchCFI(size_t number_of_delayed_adjust_pcs);

  // Emits exception block.
  void EmitExceptionPoll(MipsExceptionSlowPath* exception);

  bool IsR6() const {
    if (isa_features_ != nullptr) {
      return isa_features_->IsR6();
    } else {
      return false;
    }
  }

  bool Is32BitFPU() const {
    if (isa_features_ != nullptr) {
      return isa_features_->Is32BitFloatingPoint();
    } else {
      return true;
    }
  }

  // List of exception blocks to generate at the end of the code cache.
  std::vector<MipsExceptionSlowPath> exception_blocks_;

  std::vector<Branch> branches_;

  // Whether appending instructions at the end of the buffer or overwriting the existing ones.
  bool overwriting_;
  // The current overwrite location.
  uint32_t overwrite_location_;

  // Data for AdjustedPosition(), see the description there.
  uint32_t last_position_adjustment_;
  uint32_t last_old_position_;
  uint32_t last_branch_id_;

  const MipsInstructionSetFeatures* isa_features_;

  DISALLOW_COPY_AND_ASSIGN(MipsAssembler);
};

}  // namespace mips
}  // namespace art

#endif  // ART_COMPILER_UTILS_MIPS_ASSEMBLER_MIPS_H_
