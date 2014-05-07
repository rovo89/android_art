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

#include <vector>

#include "base/macros.h"
#include "constants_mips.h"
#include "globals.h"
#include "managed_register_mips.h"
#include "utils/assembler.h"
#include "offsets.h"
#include "utils.h"

namespace art {
namespace mips {

enum LoadOperandType {
  kLoadSignedByte,
  kLoadUnsignedByte,
  kLoadSignedHalfword,
  kLoadUnsignedHalfword,
  kLoadWord,
  kLoadWordPair,
  kLoadSWord,
  kLoadDWord
};

enum StoreOperandType {
  kStoreByte,
  kStoreHalfword,
  kStoreWord,
  kStoreWordPair,
  kStoreSWord,
  kStoreDWord
};

class MipsAssembler FINAL : public Assembler {
 public:
  MipsAssembler() {}
  virtual ~MipsAssembler() {}

  // Emit Machine Instructions.
  void Add(Register rd, Register rs, Register rt);
  void Addu(Register rd, Register rs, Register rt);
  void Addi(Register rt, Register rs, uint16_t imm16);
  void Addiu(Register rt, Register rs, uint16_t imm16);
  void Sub(Register rd, Register rs, Register rt);
  void Subu(Register rd, Register rs, Register rt);
  void Mult(Register rs, Register rt);
  void Multu(Register rs, Register rt);
  void Div(Register rs, Register rt);
  void Divu(Register rs, Register rt);

  void And(Register rd, Register rs, Register rt);
  void Andi(Register rt, Register rs, uint16_t imm16);
  void Or(Register rd, Register rs, Register rt);
  void Ori(Register rt, Register rs, uint16_t imm16);
  void Xor(Register rd, Register rs, Register rt);
  void Xori(Register rt, Register rs, uint16_t imm16);
  void Nor(Register rd, Register rs, Register rt);

  void Sll(Register rd, Register rs, int shamt);
  void Srl(Register rd, Register rs, int shamt);
  void Sra(Register rd, Register rs, int shamt);
  void Sllv(Register rd, Register rs, Register rt);
  void Srlv(Register rd, Register rs, Register rt);
  void Srav(Register rd, Register rs, Register rt);

  void Lb(Register rt, Register rs, uint16_t imm16);
  void Lh(Register rt, Register rs, uint16_t imm16);
  void Lw(Register rt, Register rs, uint16_t imm16);
  void Lbu(Register rt, Register rs, uint16_t imm16);
  void Lhu(Register rt, Register rs, uint16_t imm16);
  void Lui(Register rt, uint16_t imm16);
  void Mfhi(Register rd);
  void Mflo(Register rd);

  void Sb(Register rt, Register rs, uint16_t imm16);
  void Sh(Register rt, Register rs, uint16_t imm16);
  void Sw(Register rt, Register rs, uint16_t imm16);

  void Slt(Register rd, Register rs, Register rt);
  void Sltu(Register rd, Register rs, Register rt);
  void Slti(Register rt, Register rs, uint16_t imm16);
  void Sltiu(Register rt, Register rs, uint16_t imm16);

  void Beq(Register rt, Register rs, uint16_t imm16);
  void Bne(Register rt, Register rs, uint16_t imm16);
  void J(uint32_t address);
  void Jal(uint32_t address);
  void Jr(Register rs);
  void Jalr(Register rs);

  void AddS(FRegister fd, FRegister fs, FRegister ft);
  void SubS(FRegister fd, FRegister fs, FRegister ft);
  void MulS(FRegister fd, FRegister fs, FRegister ft);
  void DivS(FRegister fd, FRegister fs, FRegister ft);
  void AddD(DRegister fd, DRegister fs, DRegister ft);
  void SubD(DRegister fd, DRegister fs, DRegister ft);
  void MulD(DRegister fd, DRegister fs, DRegister ft);
  void DivD(DRegister fd, DRegister fs, DRegister ft);
  void MovS(FRegister fd, FRegister fs);
  void MovD(DRegister fd, DRegister fs);

  void Mfc1(Register rt, FRegister fs);
  void Mtc1(FRegister ft, Register rs);
  void Lwc1(FRegister ft, Register rs, uint16_t imm16);
  void Ldc1(DRegister ft, Register rs, uint16_t imm16);
  void Swc1(FRegister ft, Register rs, uint16_t imm16);
  void Sdc1(DRegister ft, Register rs, uint16_t imm16);

  void Break();
  void Nop();
  void Move(Register rt, Register rs);
  void Clear(Register rt);
  void Not(Register rt, Register rs);
  void Mul(Register rd, Register rs, Register rt);
  void Div(Register rd, Register rs, Register rt);
  void Rem(Register rd, Register rs, Register rt);

  void AddConstant(Register rt, Register rs, int32_t value);
  void LoadImmediate(Register rt, int32_t value);

  void EmitLoad(ManagedRegister m_dst, Register src_register, int32_t src_offset, size_t size);
  void LoadFromOffset(LoadOperandType type, Register reg, Register base, int32_t offset);
  void LoadSFromOffset(FRegister reg, Register base, int32_t offset);
  void LoadDFromOffset(DRegister reg, Register base, int32_t offset);
  void StoreToOffset(StoreOperandType type, Register reg, Register base, int32_t offset);
  void StoreFToOffset(FRegister reg, Register base, int32_t offset);
  void StoreDToOffset(DRegister reg, Register base, int32_t offset);

  // Emit data (e.g. encoded instruction or immediate) to the instruction stream.
  void Emit(int32_t value);
  void EmitBranch(Register rt, Register rs, Label* label, bool equal);
  void EmitJump(Label* label, bool link);
  void Bind(Label* label, bool is_jump);

  //
  // Overridden common assembler high-level functionality
  //

  // Emit code that will create an activation on the stack
  void BuildFrame(size_t frame_size, ManagedRegister method_reg,
                  const std::vector<ManagedRegister>& callee_save_regs,
                  const ManagedRegisterEntrySpills& entry_spills) OVERRIDE;

  // Emit code that will remove an activation from the stack
  void RemoveFrame(size_t frame_size, const std::vector<ManagedRegister>& callee_save_regs)
      OVERRIDE;

  void IncreaseFrameSize(size_t adjust) OVERRIDE;
  void DecreaseFrameSize(size_t adjust) OVERRIDE;

  // Store routines
  void Store(FrameOffset offs, ManagedRegister msrc, size_t size) OVERRIDE;
  void StoreRef(FrameOffset dest, ManagedRegister msrc) OVERRIDE;
  void StoreRawPtr(FrameOffset dest, ManagedRegister msrc) OVERRIDE;

  void StoreImmediateToFrame(FrameOffset dest, uint32_t imm, ManagedRegister mscratch) OVERRIDE;

  void StoreImmediateToThread32(ThreadOffset<4> dest, uint32_t imm, ManagedRegister mscratch)
      OVERRIDE;

  void StoreStackOffsetToThread32(ThreadOffset<4> thr_offs, FrameOffset fr_offs,
                                  ManagedRegister mscratch) OVERRIDE;

  void StoreStackPointerToThread32(ThreadOffset<4> thr_offs) OVERRIDE;

  void StoreSpanning(FrameOffset dest, ManagedRegister msrc, FrameOffset in_off,
                     ManagedRegister mscratch) OVERRIDE;

  // Load routines
  void Load(ManagedRegister mdest, FrameOffset src, size_t size) OVERRIDE;

  void LoadFromThread32(ManagedRegister mdest, ThreadOffset<4> src, size_t size) OVERRIDE;

  void LoadRef(ManagedRegister dest, FrameOffset  src) OVERRIDE;

  void LoadRef(ManagedRegister mdest, ManagedRegister base, MemberOffset offs) OVERRIDE;

  void LoadRawPtr(ManagedRegister mdest, ManagedRegister base, Offset offs) OVERRIDE;

  void LoadRawPtrFromThread32(ManagedRegister mdest, ThreadOffset<4> offs) OVERRIDE;

  // Copying routines
  void Move(ManagedRegister mdest, ManagedRegister msrc, size_t size) OVERRIDE;

  void CopyRawPtrFromThread32(FrameOffset fr_offs, ThreadOffset<4> thr_offs,
                              ManagedRegister mscratch) OVERRIDE;

  void CopyRawPtrToThread32(ThreadOffset<4> thr_offs, FrameOffset fr_offs,
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

  // Sign extension
  void SignExtend(ManagedRegister mreg, size_t size) OVERRIDE;

  // Zero extension
  void ZeroExtend(ManagedRegister mreg, size_t size) OVERRIDE;

  // Exploit fast access in managed code to Thread::Current()
  void GetCurrentThread(ManagedRegister tr) OVERRIDE;
  void GetCurrentThread(FrameOffset dest_offset, ManagedRegister mscratch) OVERRIDE;

  // Set up out_reg to hold a Object** into the handle scope, or to be NULL if the
  // value is null and null_allowed. in_reg holds a possibly stale reference
  // that can be used to avoid loading the handle scope entry to see if the value is
  // NULL.
  void CreateHandleScopeEntry(ManagedRegister out_reg, FrameOffset handlescope_offset, ManagedRegister in_reg,
                       bool null_allowed) OVERRIDE;

  // Set up out_off to hold a Object** into the handle scope, or to be NULL if the
  // value is null and null_allowed.
  void CreateHandleScopeEntry(FrameOffset out_off, FrameOffset handlescope_offset, ManagedRegister mscratch,
                       bool null_allowed) OVERRIDE;

  // src holds a handle scope entry (Object**) load this into dst
  void LoadReferenceFromHandleScope(ManagedRegister dst, ManagedRegister src) OVERRIDE;

  // Heap::VerifyObject on src. In some cases (such as a reference to this) we
  // know that src may not be null.
  void VerifyObject(ManagedRegister src, bool could_be_null) OVERRIDE;
  void VerifyObject(FrameOffset src, bool could_be_null) OVERRIDE;

  // Call to address held at [base+offset]
  void Call(ManagedRegister base, Offset offset, ManagedRegister mscratch) OVERRIDE;
  void Call(FrameOffset base, Offset offset, ManagedRegister mscratch) OVERRIDE;
  void CallFromThread32(ThreadOffset<4> offset, ManagedRegister mscratch) OVERRIDE;

  // Generate code to check if Thread::Current()->exception_ is non-null
  // and branch to a ExceptionSlowPath if it is.
  void ExceptionPoll(ManagedRegister mscratch, size_t stack_adjust) OVERRIDE;

 private:
  void EmitR(int opcode, Register rs, Register rt, Register rd, int shamt, int funct);
  void EmitI(int opcode, Register rs, Register rt, uint16_t imm);
  void EmitJ(int opcode, int address);
  void EmitFR(int opcode, int fmt, FRegister ft, FRegister fs, FRegister fd, int funct);
  void EmitFI(int opcode, int fmt, FRegister rt, uint16_t imm);

  int32_t EncodeBranchOffset(int offset, int32_t inst, bool is_jump);
  int DecodeBranchOffset(int32_t inst, bool is_jump);

  DISALLOW_COPY_AND_ASSIGN(MipsAssembler);
};

// Slowpath entered when Thread::Current()->_exception is non-null
class MipsExceptionSlowPath FINAL : public SlowPath {
 public:
  explicit MipsExceptionSlowPath(MipsManagedRegister scratch, size_t stack_adjust)
      : scratch_(scratch), stack_adjust_(stack_adjust) {}
  virtual void Emit(Assembler *sp_asm) OVERRIDE;
 private:
  const MipsManagedRegister scratch_;
  const size_t stack_adjust_;
};

}  // namespace mips
}  // namespace art

#endif  // ART_COMPILER_UTILS_MIPS_ASSEMBLER_MIPS_H_
