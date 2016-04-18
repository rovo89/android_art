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

#ifndef ART_COMPILER_UTILS_MIPS64_ASSEMBLER_MIPS64_H_
#define ART_COMPILER_UTILS_MIPS64_ASSEMBLER_MIPS64_H_

#include <utility>
#include <vector>

#include "base/macros.h"
#include "constants_mips64.h"
#include "globals.h"
#include "managed_register_mips64.h"
#include "offsets.h"
#include "utils/assembler.h"
#include "utils/label.h"

namespace art {
namespace mips64 {

static constexpr size_t kMips64WordSize = 4;
static constexpr size_t kMips64DoublewordSize = 8;

enum LoadOperandType {
  kLoadSignedByte,
  kLoadUnsignedByte,
  kLoadSignedHalfword,
  kLoadUnsignedHalfword,
  kLoadWord,
  kLoadUnsignedWord,
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

class Mips64Label : public Label {
 public:
  Mips64Label() : prev_branch_id_plus_one_(0) {}

  Mips64Label(Mips64Label&& src)
      : Label(std::move(src)), prev_branch_id_plus_one_(src.prev_branch_id_plus_one_) {}

 private:
  uint32_t prev_branch_id_plus_one_;  // To get distance from preceding branch, if any.

  friend class Mips64Assembler;
  DISALLOW_COPY_AND_ASSIGN(Mips64Label);
};

// Slowpath entered when Thread::Current()->_exception is non-null.
class Mips64ExceptionSlowPath {
 public:
  explicit Mips64ExceptionSlowPath(Mips64ManagedRegister scratch, size_t stack_adjust)
      : scratch_(scratch), stack_adjust_(stack_adjust) {}

  Mips64ExceptionSlowPath(Mips64ExceptionSlowPath&& src)
      : scratch_(src.scratch_),
        stack_adjust_(src.stack_adjust_),
        exception_entry_(std::move(src.exception_entry_)) {}

 private:
  Mips64Label* Entry() { return &exception_entry_; }
  const Mips64ManagedRegister scratch_;
  const size_t stack_adjust_;
  Mips64Label exception_entry_;

  friend class Mips64Assembler;
  DISALLOW_COPY_AND_ASSIGN(Mips64ExceptionSlowPath);
};

class Mips64Assembler FINAL : public Assembler {
 public:
  explicit Mips64Assembler(ArenaAllocator* arena)
      : Assembler(arena),
        overwriting_(false),
        overwrite_location_(0),
        last_position_adjustment_(0),
        last_old_position_(0),
        last_branch_id_(0) {
    cfi().DelayEmittingAdvancePCs();
  }

  virtual ~Mips64Assembler() {
    for (auto& branch : branches_) {
      CHECK(branch.IsResolved());
    }
  }

  // Emit Machine Instructions.
  void Addu(GpuRegister rd, GpuRegister rs, GpuRegister rt);
  void Addiu(GpuRegister rt, GpuRegister rs, uint16_t imm16);
  void Daddu(GpuRegister rd, GpuRegister rs, GpuRegister rt);  // MIPS64
  void Daddiu(GpuRegister rt, GpuRegister rs, uint16_t imm16);  // MIPS64
  void Subu(GpuRegister rd, GpuRegister rs, GpuRegister rt);
  void Dsubu(GpuRegister rd, GpuRegister rs, GpuRegister rt);  // MIPS64

  void MulR6(GpuRegister rd, GpuRegister rs, GpuRegister rt);
  void MuhR6(GpuRegister rd, GpuRegister rs, GpuRegister rt);
  void DivR6(GpuRegister rd, GpuRegister rs, GpuRegister rt);
  void ModR6(GpuRegister rd, GpuRegister rs, GpuRegister rt);
  void DivuR6(GpuRegister rd, GpuRegister rs, GpuRegister rt);
  void ModuR6(GpuRegister rd, GpuRegister rs, GpuRegister rt);
  void Dmul(GpuRegister rd, GpuRegister rs, GpuRegister rt);  // MIPS64
  void Dmuh(GpuRegister rd, GpuRegister rs, GpuRegister rt);  // MIPS64
  void Ddiv(GpuRegister rd, GpuRegister rs, GpuRegister rt);  // MIPS64
  void Dmod(GpuRegister rd, GpuRegister rs, GpuRegister rt);  // MIPS64
  void Ddivu(GpuRegister rd, GpuRegister rs, GpuRegister rt);  // MIPS64
  void Dmodu(GpuRegister rd, GpuRegister rs, GpuRegister rt);  // MIPS64

  void And(GpuRegister rd, GpuRegister rs, GpuRegister rt);
  void Andi(GpuRegister rt, GpuRegister rs, uint16_t imm16);
  void Or(GpuRegister rd, GpuRegister rs, GpuRegister rt);
  void Ori(GpuRegister rt, GpuRegister rs, uint16_t imm16);
  void Xor(GpuRegister rd, GpuRegister rs, GpuRegister rt);
  void Xori(GpuRegister rt, GpuRegister rs, uint16_t imm16);
  void Nor(GpuRegister rd, GpuRegister rs, GpuRegister rt);

  void Bitswap(GpuRegister rd, GpuRegister rt);
  void Dbitswap(GpuRegister rd, GpuRegister rt);
  void Seb(GpuRegister rd, GpuRegister rt);
  void Seh(GpuRegister rd, GpuRegister rt);
  void Dsbh(GpuRegister rd, GpuRegister rt);
  void Dshd(GpuRegister rd, GpuRegister rt);
  void Dext(GpuRegister rs, GpuRegister rt, int pos, int size);  // MIPS64
  void Dinsu(GpuRegister rt, GpuRegister rs, int pos, int size);  // MIPS64
  void Wsbh(GpuRegister rd, GpuRegister rt);
  void Sc(GpuRegister rt, GpuRegister base, int16_t imm9 = 0);
  void Scd(GpuRegister rt, GpuRegister base, int16_t imm9 = 0);
  void Ll(GpuRegister rt, GpuRegister base, int16_t imm9 = 0);
  void Lld(GpuRegister rt, GpuRegister base, int16_t imm9 = 0);

  void Sll(GpuRegister rd, GpuRegister rt, int shamt);
  void Srl(GpuRegister rd, GpuRegister rt, int shamt);
  void Rotr(GpuRegister rd, GpuRegister rt, int shamt);
  void Sra(GpuRegister rd, GpuRegister rt, int shamt);
  void Sllv(GpuRegister rd, GpuRegister rt, GpuRegister rs);
  void Srlv(GpuRegister rd, GpuRegister rt, GpuRegister rs);
  void Rotrv(GpuRegister rd, GpuRegister rt, GpuRegister rs);
  void Srav(GpuRegister rd, GpuRegister rt, GpuRegister rs);
  void Dsll(GpuRegister rd, GpuRegister rt, int shamt);  // MIPS64
  void Dsrl(GpuRegister rd, GpuRegister rt, int shamt);  // MIPS64
  void Drotr(GpuRegister rd, GpuRegister rt, int shamt);
  void Dsra(GpuRegister rd, GpuRegister rt, int shamt);  // MIPS64
  void Dsll32(GpuRegister rd, GpuRegister rt, int shamt);  // MIPS64
  void Dsrl32(GpuRegister rd, GpuRegister rt, int shamt);  // MIPS64
  void Drotr32(GpuRegister rd, GpuRegister rt, int shamt);  // MIPS64
  void Dsra32(GpuRegister rd, GpuRegister rt, int shamt);  // MIPS64
  void Dsllv(GpuRegister rd, GpuRegister rt, GpuRegister rs);  // MIPS64
  void Dsrlv(GpuRegister rd, GpuRegister rt, GpuRegister rs);  // MIPS64
  void Drotrv(GpuRegister rd, GpuRegister rt, GpuRegister rs);  // MIPS64
  void Dsrav(GpuRegister rd, GpuRegister rt, GpuRegister rs);  // MIPS64

  void Lb(GpuRegister rt, GpuRegister rs, uint16_t imm16);
  void Lh(GpuRegister rt, GpuRegister rs, uint16_t imm16);
  void Lw(GpuRegister rt, GpuRegister rs, uint16_t imm16);
  void Ld(GpuRegister rt, GpuRegister rs, uint16_t imm16);  // MIPS64
  void Lbu(GpuRegister rt, GpuRegister rs, uint16_t imm16);
  void Lhu(GpuRegister rt, GpuRegister rs, uint16_t imm16);
  void Lwu(GpuRegister rt, GpuRegister rs, uint16_t imm16);  // MIPS64
  void Lui(GpuRegister rt, uint16_t imm16);
  void Dahi(GpuRegister rs, uint16_t imm16);  // MIPS64
  void Dati(GpuRegister rs, uint16_t imm16);  // MIPS64
  void Sync(uint32_t stype);

  void Sb(GpuRegister rt, GpuRegister rs, uint16_t imm16);
  void Sh(GpuRegister rt, GpuRegister rs, uint16_t imm16);
  void Sw(GpuRegister rt, GpuRegister rs, uint16_t imm16);
  void Sd(GpuRegister rt, GpuRegister rs, uint16_t imm16);  // MIPS64

  void Slt(GpuRegister rd, GpuRegister rs, GpuRegister rt);
  void Sltu(GpuRegister rd, GpuRegister rs, GpuRegister rt);
  void Slti(GpuRegister rt, GpuRegister rs, uint16_t imm16);
  void Sltiu(GpuRegister rt, GpuRegister rs, uint16_t imm16);
  void Seleqz(GpuRegister rd, GpuRegister rs, GpuRegister rt);
  void Selnez(GpuRegister rd, GpuRegister rs, GpuRegister rt);
  void Clz(GpuRegister rd, GpuRegister rs);
  void Clo(GpuRegister rd, GpuRegister rs);
  void Dclz(GpuRegister rd, GpuRegister rs);
  void Dclo(GpuRegister rd, GpuRegister rs);

  void Jalr(GpuRegister rd, GpuRegister rs);
  void Jalr(GpuRegister rs);
  void Jr(GpuRegister rs);
  void Auipc(GpuRegister rs, uint16_t imm16);
  void Addiupc(GpuRegister rs, uint32_t imm19);
  void Bc(uint32_t imm26);
  void Jic(GpuRegister rt, uint16_t imm16);
  void Jialc(GpuRegister rt, uint16_t imm16);
  void Bltc(GpuRegister rs, GpuRegister rt, uint16_t imm16);
  void Bltzc(GpuRegister rt, uint16_t imm16);
  void Bgtzc(GpuRegister rt, uint16_t imm16);
  void Bgec(GpuRegister rs, GpuRegister rt, uint16_t imm16);
  void Bgezc(GpuRegister rt, uint16_t imm16);
  void Blezc(GpuRegister rt, uint16_t imm16);
  void Bltuc(GpuRegister rs, GpuRegister rt, uint16_t imm16);
  void Bgeuc(GpuRegister rs, GpuRegister rt, uint16_t imm16);
  void Beqc(GpuRegister rs, GpuRegister rt, uint16_t imm16);
  void Bnec(GpuRegister rs, GpuRegister rt, uint16_t imm16);
  void Beqzc(GpuRegister rs, uint32_t imm21);
  void Bnezc(GpuRegister rs, uint32_t imm21);
  void Bc1eqz(FpuRegister ft, uint16_t imm16);
  void Bc1nez(FpuRegister ft, uint16_t imm16);

  void AddS(FpuRegister fd, FpuRegister fs, FpuRegister ft);
  void SubS(FpuRegister fd, FpuRegister fs, FpuRegister ft);
  void MulS(FpuRegister fd, FpuRegister fs, FpuRegister ft);
  void DivS(FpuRegister fd, FpuRegister fs, FpuRegister ft);
  void AddD(FpuRegister fd, FpuRegister fs, FpuRegister ft);
  void SubD(FpuRegister fd, FpuRegister fs, FpuRegister ft);
  void MulD(FpuRegister fd, FpuRegister fs, FpuRegister ft);
  void DivD(FpuRegister fd, FpuRegister fs, FpuRegister ft);
  void SqrtS(FpuRegister fd, FpuRegister fs);
  void SqrtD(FpuRegister fd, FpuRegister fs);
  void AbsS(FpuRegister fd, FpuRegister fs);
  void AbsD(FpuRegister fd, FpuRegister fs);
  void MovS(FpuRegister fd, FpuRegister fs);
  void MovD(FpuRegister fd, FpuRegister fs);
  void NegS(FpuRegister fd, FpuRegister fs);
  void NegD(FpuRegister fd, FpuRegister fs);
  void RoundLS(FpuRegister fd, FpuRegister fs);
  void RoundLD(FpuRegister fd, FpuRegister fs);
  void RoundWS(FpuRegister fd, FpuRegister fs);
  void RoundWD(FpuRegister fd, FpuRegister fs);
  void TruncLS(FpuRegister fd, FpuRegister fs);
  void TruncLD(FpuRegister fd, FpuRegister fs);
  void TruncWS(FpuRegister fd, FpuRegister fs);
  void TruncWD(FpuRegister fd, FpuRegister fs);
  void CeilLS(FpuRegister fd, FpuRegister fs);
  void CeilLD(FpuRegister fd, FpuRegister fs);
  void CeilWS(FpuRegister fd, FpuRegister fs);
  void CeilWD(FpuRegister fd, FpuRegister fs);
  void FloorLS(FpuRegister fd, FpuRegister fs);
  void FloorLD(FpuRegister fd, FpuRegister fs);
  void FloorWS(FpuRegister fd, FpuRegister fs);
  void FloorWD(FpuRegister fd, FpuRegister fs);
  void SelS(FpuRegister fd, FpuRegister fs, FpuRegister ft);
  void SelD(FpuRegister fd, FpuRegister fs, FpuRegister ft);
  void RintS(FpuRegister fd, FpuRegister fs);
  void RintD(FpuRegister fd, FpuRegister fs);
  void ClassS(FpuRegister fd, FpuRegister fs);
  void ClassD(FpuRegister fd, FpuRegister fs);
  void MinS(FpuRegister fd, FpuRegister fs, FpuRegister ft);
  void MinD(FpuRegister fd, FpuRegister fs, FpuRegister ft);
  void MaxS(FpuRegister fd, FpuRegister fs, FpuRegister ft);
  void MaxD(FpuRegister fd, FpuRegister fs, FpuRegister ft);
  void CmpUnS(FpuRegister fd, FpuRegister fs, FpuRegister ft);
  void CmpEqS(FpuRegister fd, FpuRegister fs, FpuRegister ft);
  void CmpUeqS(FpuRegister fd, FpuRegister fs, FpuRegister ft);
  void CmpLtS(FpuRegister fd, FpuRegister fs, FpuRegister ft);
  void CmpUltS(FpuRegister fd, FpuRegister fs, FpuRegister ft);
  void CmpLeS(FpuRegister fd, FpuRegister fs, FpuRegister ft);
  void CmpUleS(FpuRegister fd, FpuRegister fs, FpuRegister ft);
  void CmpOrS(FpuRegister fd, FpuRegister fs, FpuRegister ft);
  void CmpUneS(FpuRegister fd, FpuRegister fs, FpuRegister ft);
  void CmpNeS(FpuRegister fd, FpuRegister fs, FpuRegister ft);
  void CmpUnD(FpuRegister fd, FpuRegister fs, FpuRegister ft);
  void CmpEqD(FpuRegister fd, FpuRegister fs, FpuRegister ft);
  void CmpUeqD(FpuRegister fd, FpuRegister fs, FpuRegister ft);
  void CmpLtD(FpuRegister fd, FpuRegister fs, FpuRegister ft);
  void CmpUltD(FpuRegister fd, FpuRegister fs, FpuRegister ft);
  void CmpLeD(FpuRegister fd, FpuRegister fs, FpuRegister ft);
  void CmpUleD(FpuRegister fd, FpuRegister fs, FpuRegister ft);
  void CmpOrD(FpuRegister fd, FpuRegister fs, FpuRegister ft);
  void CmpUneD(FpuRegister fd, FpuRegister fs, FpuRegister ft);
  void CmpNeD(FpuRegister fd, FpuRegister fs, FpuRegister ft);

  void Cvtsw(FpuRegister fd, FpuRegister fs);
  void Cvtdw(FpuRegister fd, FpuRegister fs);
  void Cvtsd(FpuRegister fd, FpuRegister fs);
  void Cvtds(FpuRegister fd, FpuRegister fs);
  void Cvtsl(FpuRegister fd, FpuRegister fs);
  void Cvtdl(FpuRegister fd, FpuRegister fs);

  void Mfc1(GpuRegister rt, FpuRegister fs);
  void Mfhc1(GpuRegister rt, FpuRegister fs);
  void Mtc1(GpuRegister rt, FpuRegister fs);
  void Mthc1(GpuRegister rt, FpuRegister fs);
  void Dmfc1(GpuRegister rt, FpuRegister fs);  // MIPS64
  void Dmtc1(GpuRegister rt, FpuRegister fs);  // MIPS64
  void Lwc1(FpuRegister ft, GpuRegister rs, uint16_t imm16);
  void Ldc1(FpuRegister ft, GpuRegister rs, uint16_t imm16);
  void Swc1(FpuRegister ft, GpuRegister rs, uint16_t imm16);
  void Sdc1(FpuRegister ft, GpuRegister rs, uint16_t imm16);

  void Break();
  void Nop();
  void Move(GpuRegister rd, GpuRegister rs);
  void Clear(GpuRegister rd);
  void Not(GpuRegister rd, GpuRegister rs);

  // Higher level composite instructions.
  void LoadConst32(GpuRegister rd, int32_t value);
  void LoadConst64(GpuRegister rd, int64_t value);  // MIPS64

  void Daddiu64(GpuRegister rt, GpuRegister rs, int64_t value, GpuRegister rtmp = AT);  // MIPS64

  void Bind(Label* label) OVERRIDE {
    Bind(down_cast<Mips64Label*>(label));
  }
  void Jump(Label* label ATTRIBUTE_UNUSED) OVERRIDE {
    UNIMPLEMENTED(FATAL) << "Do not use Jump for MIPS64";
  }

  void Bind(Mips64Label* label);
  void Bc(Mips64Label* label);
  void Jialc(Mips64Label* label, GpuRegister indirect_reg);
  void Bltc(GpuRegister rs, GpuRegister rt, Mips64Label* label);
  void Bltzc(GpuRegister rt, Mips64Label* label);
  void Bgtzc(GpuRegister rt, Mips64Label* label);
  void Bgec(GpuRegister rs, GpuRegister rt, Mips64Label* label);
  void Bgezc(GpuRegister rt, Mips64Label* label);
  void Blezc(GpuRegister rt, Mips64Label* label);
  void Bltuc(GpuRegister rs, GpuRegister rt, Mips64Label* label);
  void Bgeuc(GpuRegister rs, GpuRegister rt, Mips64Label* label);
  void Beqc(GpuRegister rs, GpuRegister rt, Mips64Label* label);
  void Bnec(GpuRegister rs, GpuRegister rt, Mips64Label* label);
  void Beqzc(GpuRegister rs, Mips64Label* label);
  void Bnezc(GpuRegister rs, Mips64Label* label);
  void Bc1eqz(FpuRegister ft, Mips64Label* label);
  void Bc1nez(FpuRegister ft, Mips64Label* label);

  void EmitLoad(ManagedRegister m_dst, GpuRegister src_register, int32_t src_offset, size_t size);
  void LoadFromOffset(LoadOperandType type, GpuRegister reg, GpuRegister base, int32_t offset);
  void LoadFpuFromOffset(LoadOperandType type, FpuRegister reg, GpuRegister base, int32_t offset);
  void StoreToOffset(StoreOperandType type, GpuRegister reg, GpuRegister base, int32_t offset);
  void StoreFpuToOffset(StoreOperandType type, FpuRegister reg, GpuRegister base, int32_t offset);

  // Emit data (e.g. encoded instruction or immediate) to the instruction stream.
  void Emit(uint32_t value);

  //
  // Overridden common assembler high-level functionality.
  //

  // Emit code that will create an activation on the stack.
  void BuildFrame(size_t frame_size, ManagedRegister method_reg,
                  const std::vector<ManagedRegister>& callee_save_regs,
                  const ManagedRegisterEntrySpills& entry_spills) OVERRIDE;

  // Emit code that will remove an activation from the stack.
  void RemoveFrame(size_t frame_size,
                   const std::vector<ManagedRegister>& callee_save_regs) OVERRIDE;

  void IncreaseFrameSize(size_t adjust) OVERRIDE;
  void DecreaseFrameSize(size_t adjust) OVERRIDE;

  // Store routines.
  void Store(FrameOffset offs, ManagedRegister msrc, size_t size) OVERRIDE;
  void StoreRef(FrameOffset dest, ManagedRegister msrc) OVERRIDE;
  void StoreRawPtr(FrameOffset dest, ManagedRegister msrc) OVERRIDE;

  void StoreImmediateToFrame(FrameOffset dest, uint32_t imm, ManagedRegister mscratch) OVERRIDE;

  void StoreStackOffsetToThread64(ThreadOffset<kMips64DoublewordSize> thr_offs, FrameOffset fr_offs,
                                  ManagedRegister mscratch) OVERRIDE;

  void StoreStackPointerToThread64(ThreadOffset<kMips64DoublewordSize> thr_offs) OVERRIDE;

  void StoreSpanning(FrameOffset dest, ManagedRegister msrc, FrameOffset in_off,
                     ManagedRegister mscratch) OVERRIDE;

  // Load routines.
  void Load(ManagedRegister mdest, FrameOffset src, size_t size) OVERRIDE;

  void LoadFromThread64(ManagedRegister mdest,
                        ThreadOffset<kMips64DoublewordSize> src,
                        size_t size) OVERRIDE;

  void LoadRef(ManagedRegister dest, FrameOffset src) OVERRIDE;

  void LoadRef(ManagedRegister mdest, ManagedRegister base, MemberOffset offs,
               bool unpoison_reference) OVERRIDE;

  void LoadRawPtr(ManagedRegister mdest, ManagedRegister base, Offset offs) OVERRIDE;

  void LoadRawPtrFromThread64(ManagedRegister mdest,
                              ThreadOffset<kMips64DoublewordSize> offs) OVERRIDE;

  // Copying routines.
  void Move(ManagedRegister mdest, ManagedRegister msrc, size_t size) OVERRIDE;

  void CopyRawPtrFromThread64(FrameOffset fr_offs, ThreadOffset<kMips64DoublewordSize> thr_offs,
                              ManagedRegister mscratch) OVERRIDE;

  void CopyRawPtrToThread64(ThreadOffset<kMips64DoublewordSize> thr_offs, FrameOffset fr_offs,
                            ManagedRegister mscratch) OVERRIDE;

  void CopyRef(FrameOffset dest, FrameOffset src, ManagedRegister mscratch) OVERRIDE;

  void Copy(FrameOffset dest, FrameOffset src, ManagedRegister mscratch, size_t size) OVERRIDE;

  void Copy(FrameOffset dest, ManagedRegister src_base, Offset src_offset, ManagedRegister mscratch,
            size_t size) OVERRIDE;

  void Copy(ManagedRegister dest_base, Offset dest_offset, FrameOffset src,
            ManagedRegister mscratch, size_t size) OVERRIDE;

  void Copy(FrameOffset dest, FrameOffset src_base, Offset src_offset, ManagedRegister mscratch,
            size_t size) OVERRIDE;

  void Copy(ManagedRegister dest, Offset dest_offset, ManagedRegister src, Offset src_offset,
            ManagedRegister mscratch, size_t size) OVERRIDE;

  void Copy(FrameOffset dest, Offset dest_offset, FrameOffset src, Offset src_offset,
            ManagedRegister mscratch, size_t size) OVERRIDE;

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
  void CreateHandleScopeEntry(ManagedRegister out_reg, FrameOffset handlescope_offset,
                              ManagedRegister in_reg, bool null_allowed) OVERRIDE;

  // Set up out_off to hold a Object** into the handle scope, or to be null if the
  // value is null and null_allowed.
  void CreateHandleScopeEntry(FrameOffset out_off, FrameOffset handlescope_offset, ManagedRegister
                              mscratch, bool null_allowed) OVERRIDE;

  // src holds a handle scope entry (Object**) load this into dst.
  void LoadReferenceFromHandleScope(ManagedRegister dst, ManagedRegister src) OVERRIDE;

  // Heap::VerifyObject on src. In some cases (such as a reference to this) we
  // know that src may not be null.
  void VerifyObject(ManagedRegister src, bool could_be_null) OVERRIDE;
  void VerifyObject(FrameOffset src, bool could_be_null) OVERRIDE;

  // Call to address held at [base+offset].
  void Call(ManagedRegister base, Offset offset, ManagedRegister mscratch) OVERRIDE;
  void Call(FrameOffset base, Offset offset, ManagedRegister mscratch) OVERRIDE;
  void CallFromThread64(ThreadOffset<kMips64DoublewordSize> offset,
                        ManagedRegister mscratch) OVERRIDE;

  // Generate code to check if Thread::Current()->exception_ is non-null
  // and branch to a ExceptionSlowPath if it is.
  void ExceptionPoll(ManagedRegister mscratch, size_t stack_adjust) OVERRIDE;

  // Emit slow paths queued during assembly and promote short branches to long if needed.
  void FinalizeCode() OVERRIDE;

  // Emit branches and finalize all instructions.
  void FinalizeInstructions(const MemoryRegion& region);

  // Returns the (always-)current location of a label (can be used in class CodeGeneratorMIPS64,
  // must be used instead of Mips64Label::GetPosition()).
  uint32_t GetLabelLocation(Mips64Label* label) const;

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
      // Short branches.
      kUncondBranch,
      kCondBranch,
      kCall,
      // Long branches.
      kLongUncondBranch,
      kLongCondBranch,
      kLongCall,
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
      // How large (in bits) a PC-relative offset can be for a given type of branch (kCondBranch is
      // an exception: use kOffset23 for beqzc/bnezc).
      OffsetBits offset_size;
      // Some MIPS instructions with PC-relative offsets shift the offset by 2. Encode the shift
      // count.
      int offset_shift;
    };
    static const BranchInfo branch_info_[/* Type */];

    // Unconditional branch.
    Branch(uint32_t location, uint32_t target);
    // Conditional branch.
    Branch(uint32_t location,
           uint32_t target,
           BranchCondition condition,
           GpuRegister lhs_reg,
           GpuRegister rhs_reg = ZERO);
    // Call (branch and link) that stores the target address in a given register (i.e. T9).
    Branch(uint32_t location, uint32_t target, GpuRegister indirect_reg);

    // Some conditional branches with lhs = rhs are effectively NOPs, while some
    // others are effectively unconditional. MIPSR6 conditional branches require lhs != rhs.
    // So, we need a way to identify such branches in order to emit no instructions for them
    // or change them to unconditional.
    static bool IsNop(BranchCondition condition, GpuRegister lhs, GpuRegister rhs);
    static bool IsUncond(BranchCondition condition, GpuRegister lhs, GpuRegister rhs);

    static BranchCondition OppositeCondition(BranchCondition cond);

    Type GetType() const;
    BranchCondition GetCondition() const;
    GpuRegister GetLeftRegister() const;
    GpuRegister GetRightRegister() const;
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
    // The composite branches cover the range of PC + ~+/-2GB. The range is not end-to-end,
    // however. Consider the following implementation of a long unconditional branch, for
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
    void InitializeType(bool is_call);
    // Helper for the above.
    void InitShortOrLong(OffsetBits ofs_size, Type short_type, Type long_type);

    uint32_t old_location_;      // Offset into assembler buffer in bytes.
    uint32_t location_;          // Offset into assembler buffer in bytes.
    uint32_t target_;            // Offset into assembler buffer in bytes.

    GpuRegister lhs_reg_;        // Left-hand side register in conditional branches or
                                 // indirect call register.
    GpuRegister rhs_reg_;        // Right-hand side register in conditional branches.
    BranchCondition condition_;  // Condition for conditional branches.

    Type type_;                  // Current type of the branch.
    Type old_type_;              // Initial type of the branch.
  };
  friend std::ostream& operator<<(std::ostream& os, const Branch::Type& rhs);
  friend std::ostream& operator<<(std::ostream& os, const Branch::OffsetBits& rhs);

  void EmitR(int opcode, GpuRegister rs, GpuRegister rt, GpuRegister rd, int shamt, int funct);
  void EmitRsd(int opcode, GpuRegister rs, GpuRegister rd, int shamt, int funct);
  void EmitRtd(int opcode, GpuRegister rt, GpuRegister rd, int shamt, int funct);
  void EmitI(int opcode, GpuRegister rs, GpuRegister rt, uint16_t imm);
  void EmitI21(int opcode, GpuRegister rs, uint32_t imm21);
  void EmitI26(int opcode, uint32_t imm26);
  void EmitFR(int opcode, int fmt, FpuRegister ft, FpuRegister fs, FpuRegister fd, int funct);
  void EmitFI(int opcode, int fmt, FpuRegister rt, uint16_t imm);
  void EmitBcondc(BranchCondition cond, GpuRegister rs, GpuRegister rt, uint32_t imm16_21);

  void Buncond(Mips64Label* label);
  void Bcond(Mips64Label* label,
             BranchCondition condition,
             GpuRegister lhs,
             GpuRegister rhs = ZERO);
  void Call(Mips64Label* label, GpuRegister indirect_reg);
  void FinalizeLabeledBranch(Mips64Label* label);

  Branch* GetBranch(uint32_t branch_id);
  const Branch* GetBranch(uint32_t branch_id) const;

  void PromoteBranches();
  void EmitBranch(Branch* branch);
  void EmitBranches();
  void PatchCFI();

  // Emits exception block.
  void EmitExceptionPoll(Mips64ExceptionSlowPath* exception);

  // List of exception blocks to generate at the end of the code cache.
  std::vector<Mips64ExceptionSlowPath> exception_blocks_;

  std::vector<Branch> branches_;

  // Whether appending instructions at the end of the buffer or overwriting the existing ones.
  bool overwriting_;
  // The current overwrite location.
  uint32_t overwrite_location_;

  // Data for AdjustedPosition(), see the description there.
  uint32_t last_position_adjustment_;
  uint32_t last_old_position_;
  uint32_t last_branch_id_;

  DISALLOW_COPY_AND_ASSIGN(Mips64Assembler);
};

}  // namespace mips64
}  // namespace art

#endif  // ART_COMPILER_UTILS_MIPS64_ASSEMBLER_MIPS64_H_
