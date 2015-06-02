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

#include <vector>

#include "base/macros.h"
#include "constants_mips64.h"
#include "globals.h"
#include "managed_register_mips64.h"
#include "utils/assembler.h"
#include "offsets.h"

namespace art {
namespace mips64 {

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

class Mips64Assembler FINAL : public Assembler {
 public:
  Mips64Assembler() {}
  virtual ~Mips64Assembler() {}

  // Emit Machine Instructions.
  void Add(GpuRegister rd, GpuRegister rs, GpuRegister rt);
  void Addi(GpuRegister rt, GpuRegister rs, uint16_t imm16);
  void Addu(GpuRegister rd, GpuRegister rs, GpuRegister rt);
  void Addiu(GpuRegister rt, GpuRegister rs, uint16_t imm16);
  void Daddu(GpuRegister rd, GpuRegister rs, GpuRegister rt);  // MIPS64
  void Daddiu(GpuRegister rt, GpuRegister rs, uint16_t imm16);  // MIPS64
  void Sub(GpuRegister rd, GpuRegister rs, GpuRegister rt);
  void Subu(GpuRegister rd, GpuRegister rs, GpuRegister rt);
  void Dsubu(GpuRegister rd, GpuRegister rs, GpuRegister rt);  // MIPS64

  void MultR2(GpuRegister rs, GpuRegister rt);  // R2
  void MultuR2(GpuRegister rs, GpuRegister rt);  // R2
  void DivR2(GpuRegister rs, GpuRegister rt);  // R2
  void DivuR2(GpuRegister rs, GpuRegister rt);  // R2
  void MulR2(GpuRegister rd, GpuRegister rs, GpuRegister rt);  // R2
  void DivR2(GpuRegister rd, GpuRegister rs, GpuRegister rt);  // R2
  void ModR2(GpuRegister rd, GpuRegister rs, GpuRegister rt);  // R2
  void DivuR2(GpuRegister rd, GpuRegister rs, GpuRegister rt);  // R2
  void ModuR2(GpuRegister rd, GpuRegister rs, GpuRegister rt);  // R2
  void MulR6(GpuRegister rd, GpuRegister rs, GpuRegister rt);  // R6
  void DivR6(GpuRegister rd, GpuRegister rs, GpuRegister rt);  // R6
  void ModR6(GpuRegister rd, GpuRegister rs, GpuRegister rt);  // R6
  void DivuR6(GpuRegister rd, GpuRegister rs, GpuRegister rt);  // R6
  void ModuR6(GpuRegister rd, GpuRegister rs, GpuRegister rt);  // R6
  void Dmul(GpuRegister rd, GpuRegister rs, GpuRegister rt);  // MIPS64 R6
  void Ddiv(GpuRegister rd, GpuRegister rs, GpuRegister rt);  // MIPS64 R6
  void Dmod(GpuRegister rd, GpuRegister rs, GpuRegister rt);  // MIPS64 R6
  void Ddivu(GpuRegister rd, GpuRegister rs, GpuRegister rt);  // MIPS64 R6
  void Dmodu(GpuRegister rd, GpuRegister rs, GpuRegister rt);  // MIPS64 R6

  void And(GpuRegister rd, GpuRegister rs, GpuRegister rt);
  void Andi(GpuRegister rt, GpuRegister rs, uint16_t imm16);
  void Or(GpuRegister rd, GpuRegister rs, GpuRegister rt);
  void Ori(GpuRegister rt, GpuRegister rs, uint16_t imm16);
  void Xor(GpuRegister rd, GpuRegister rs, GpuRegister rt);
  void Xori(GpuRegister rt, GpuRegister rs, uint16_t imm16);
  void Nor(GpuRegister rd, GpuRegister rs, GpuRegister rt);

  void Seb(GpuRegister rd, GpuRegister rt);  // R2+
  void Seh(GpuRegister rd, GpuRegister rt);  // R2+
  void Dext(GpuRegister rs, GpuRegister rt, int pos, int size_less_one);  // MIPS64

  void Sll(GpuRegister rd, GpuRegister rt, int shamt);
  void Srl(GpuRegister rd, GpuRegister rt, int shamt);
  void Sra(GpuRegister rd, GpuRegister rt, int shamt);
  void Sllv(GpuRegister rd, GpuRegister rt, GpuRegister rs);
  void Srlv(GpuRegister rd, GpuRegister rt, GpuRegister rs);
  void Srav(GpuRegister rd, GpuRegister rt, GpuRegister rs);
  void Dsll(GpuRegister rd, GpuRegister rt, int shamt);  // MIPS64
  void Dsrl(GpuRegister rd, GpuRegister rt, int shamt);  // MIPS64
  void Dsra(GpuRegister rd, GpuRegister rt, int shamt);  // MIPS64
  void Dsll32(GpuRegister rd, GpuRegister rt, int shamt);  // MIPS64
  void Dsrl32(GpuRegister rd, GpuRegister rt, int shamt);  // MIPS64
  void Dsra32(GpuRegister rd, GpuRegister rt, int shamt);  // MIPS64
  void Dsllv(GpuRegister rd, GpuRegister rt, GpuRegister rs);  // MIPS64
  void Dsrlv(GpuRegister rd, GpuRegister rt, GpuRegister rs);  // MIPS64
  void Dsrav(GpuRegister rd, GpuRegister rt, GpuRegister rs);  // MIPS64

  void Lb(GpuRegister rt, GpuRegister rs, uint16_t imm16);
  void Lh(GpuRegister rt, GpuRegister rs, uint16_t imm16);
  void Lw(GpuRegister rt, GpuRegister rs, uint16_t imm16);
  void Ld(GpuRegister rt, GpuRegister rs, uint16_t imm16);  // MIPS64
  void Lbu(GpuRegister rt, GpuRegister rs, uint16_t imm16);
  void Lhu(GpuRegister rt, GpuRegister rs, uint16_t imm16);
  void Lwu(GpuRegister rt, GpuRegister rs, uint16_t imm16);  // MIPS64
  void Lui(GpuRegister rt, uint16_t imm16);
  void Dahi(GpuRegister rs, uint16_t imm16);  // MIPS64 R6
  void Dati(GpuRegister rs, uint16_t imm16);  // MIPS64 R6
  void Sync(uint32_t stype);
  void Mfhi(GpuRegister rd);  // R2
  void Mflo(GpuRegister rd);  // R2

  void Sb(GpuRegister rt, GpuRegister rs, uint16_t imm16);
  void Sh(GpuRegister rt, GpuRegister rs, uint16_t imm16);
  void Sw(GpuRegister rt, GpuRegister rs, uint16_t imm16);
  void Sd(GpuRegister rt, GpuRegister rs, uint16_t imm16);  // MIPS64

  void Slt(GpuRegister rd, GpuRegister rs, GpuRegister rt);
  void Sltu(GpuRegister rd, GpuRegister rs, GpuRegister rt);
  void Slti(GpuRegister rt, GpuRegister rs, uint16_t imm16);
  void Sltiu(GpuRegister rt, GpuRegister rs, uint16_t imm16);

  void Beq(GpuRegister rs, GpuRegister rt, uint16_t imm16);
  void Bne(GpuRegister rs, GpuRegister rt, uint16_t imm16);
  void J(uint32_t addr26);
  void Jal(uint32_t addr26);
  void Jalr(GpuRegister rd, GpuRegister rs);
  void Jalr(GpuRegister rs);
  void Jr(GpuRegister rs);
  void Auipc(GpuRegister rs, uint16_t imm16);  // R6
  void Jic(GpuRegister rt, uint16_t imm16);  // R6
  void Jialc(GpuRegister rt, uint16_t imm16);  // R6
  void Bltc(GpuRegister rs, GpuRegister rt, uint16_t imm16);  // R6
  void Bltzc(GpuRegister rt, uint16_t imm16);  // R6
  void Bgtzc(GpuRegister rt, uint16_t imm16);  // R6
  void Bgec(GpuRegister rs, GpuRegister rt, uint16_t imm16);  // R6
  void Bgezc(GpuRegister rt, uint16_t imm16);  // R6
  void Blezc(GpuRegister rt, uint16_t imm16);  // R6
  void Bltuc(GpuRegister rs, GpuRegister rt, uint16_t imm16);  // R6
  void Bgeuc(GpuRegister rs, GpuRegister rt, uint16_t imm16);  // R6
  void Beqc(GpuRegister rs, GpuRegister rt, uint16_t imm16);  // R6
  void Bnec(GpuRegister rs, GpuRegister rt, uint16_t imm16);  // R6
  void Beqzc(GpuRegister rs, uint32_t imm21);  // R6
  void Bnezc(GpuRegister rs, uint32_t imm21);  // R6

  void AddS(FpuRegister fd, FpuRegister fs, FpuRegister ft);
  void SubS(FpuRegister fd, FpuRegister fs, FpuRegister ft);
  void MulS(FpuRegister fd, FpuRegister fs, FpuRegister ft);
  void DivS(FpuRegister fd, FpuRegister fs, FpuRegister ft);
  void AddD(FpuRegister fd, FpuRegister fs, FpuRegister ft);
  void SubD(FpuRegister fd, FpuRegister fs, FpuRegister ft);
  void MulD(FpuRegister fd, FpuRegister fs, FpuRegister ft);
  void DivD(FpuRegister fd, FpuRegister fs, FpuRegister ft);
  void MovS(FpuRegister fd, FpuRegister fs);
  void MovD(FpuRegister fd, FpuRegister fs);
  void NegS(FpuRegister fd, FpuRegister fs);
  void NegD(FpuRegister fd, FpuRegister fs);

  void Cvtsw(FpuRegister fd, FpuRegister fs);
  void Cvtdw(FpuRegister fd, FpuRegister fs);
  void Cvtsd(FpuRegister fd, FpuRegister fs);
  void Cvtds(FpuRegister fd, FpuRegister fs);

  void Mfc1(GpuRegister rt, FpuRegister fs);
  void Mtc1(GpuRegister rt, FpuRegister fs);
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

  // Higher level composite instructions
  void LoadConst32(GpuRegister rd, int32_t value);
  void LoadConst64(GpuRegister rd, int64_t value);  // MIPS64

  void Addiu32(GpuRegister rt, GpuRegister rs, int32_t value, GpuRegister rtmp = AT);
  void Daddiu64(GpuRegister rt, GpuRegister rs, int64_t value, GpuRegister rtmp = AT);  // MIPS64

  void Bind(Label* label);  // R6
  void B(Label* label);  // R6
  void Jalr(Label* label, GpuRegister indirect_reg = RA);  // R6
  // TODO: implement common for R6 and non-R6 interface for conditional branches?
  void Bltc(GpuRegister rs, GpuRegister rt, Label* label);  // R6
  void Bltzc(GpuRegister rt, Label* label);  // R6
  void Bgtzc(GpuRegister rt, Label* label);  // R6
  void Bgec(GpuRegister rs, GpuRegister rt, Label* label);  // R6
  void Bgezc(GpuRegister rt, Label* label);  // R6
  void Blezc(GpuRegister rt, Label* label);  // R6
  void Bltuc(GpuRegister rs, GpuRegister rt, Label* label);  // R6
  void Bgeuc(GpuRegister rs, GpuRegister rt, Label* label);  // R6
  void Beqc(GpuRegister rs, GpuRegister rt, Label* label);  // R6
  void Bnec(GpuRegister rs, GpuRegister rt, Label* label);  // R6
  void Beqzc(GpuRegister rs, Label* label);  // R6
  void Bnezc(GpuRegister rs, Label* label);  // R6

  void EmitLoad(ManagedRegister m_dst, GpuRegister src_register, int32_t src_offset, size_t size);
  void LoadFromOffset(LoadOperandType type, GpuRegister reg, GpuRegister base, int32_t offset);
  void LoadFpuFromOffset(LoadOperandType type, FpuRegister reg, GpuRegister base, int32_t offset);
  void StoreToOffset(StoreOperandType type, GpuRegister reg, GpuRegister base, int32_t offset);
  void StoreFpuToOffset(StoreOperandType type, FpuRegister reg, GpuRegister base, int32_t offset);

  // Emit data (e.g. encoded instruction or immediate) to the instruction stream.
  void Emit(uint32_t value);

  //
  // Overridden common assembler high-level functionality
  //

  // Emit code that will create an activation on the stack
  void BuildFrame(size_t frame_size, ManagedRegister method_reg,
                  const std::vector<ManagedRegister>& callee_save_regs,
                  const ManagedRegisterEntrySpills& entry_spills) OVERRIDE;

  // Emit code that will remove an activation from the stack
  void RemoveFrame(size_t frame_size,
                   const std::vector<ManagedRegister>& callee_save_regs) OVERRIDE;

  void IncreaseFrameSize(size_t adjust) OVERRIDE;
  void DecreaseFrameSize(size_t adjust) OVERRIDE;

  // Store routines
  void Store(FrameOffset offs, ManagedRegister msrc, size_t size) OVERRIDE;
  void StoreRef(FrameOffset dest, ManagedRegister msrc) OVERRIDE;
  void StoreRawPtr(FrameOffset dest, ManagedRegister msrc) OVERRIDE;

  void StoreImmediateToFrame(FrameOffset dest, uint32_t imm, ManagedRegister mscratch) OVERRIDE;

  void StoreImmediateToThread64(ThreadOffset<8> dest, uint32_t imm,
                                ManagedRegister mscratch) OVERRIDE;

  void StoreStackOffsetToThread64(ThreadOffset<8> thr_offs, FrameOffset fr_offs,
                                  ManagedRegister mscratch) OVERRIDE;

  void StoreStackPointerToThread64(ThreadOffset<8> thr_offs) OVERRIDE;

  void StoreSpanning(FrameOffset dest, ManagedRegister msrc, FrameOffset in_off,
                     ManagedRegister mscratch) OVERRIDE;

  // Load routines
  void Load(ManagedRegister mdest, FrameOffset src, size_t size) OVERRIDE;

  void LoadFromThread64(ManagedRegister mdest, ThreadOffset<8> src, size_t size) OVERRIDE;

  void LoadRef(ManagedRegister dest, FrameOffset src) OVERRIDE;

  void LoadRef(ManagedRegister mdest, ManagedRegister base, MemberOffset offs,
               bool poison_reference) OVERRIDE;

  void LoadRawPtr(ManagedRegister mdest, ManagedRegister base, Offset offs) OVERRIDE;

  void LoadRawPtrFromThread64(ManagedRegister mdest, ThreadOffset<8> offs) OVERRIDE;

  // Copying routines
  void Move(ManagedRegister mdest, ManagedRegister msrc, size_t size) OVERRIDE;

  void CopyRawPtrFromThread64(FrameOffset fr_offs, ThreadOffset<8> thr_offs,
                              ManagedRegister mscratch) OVERRIDE;

  void CopyRawPtrToThread64(ThreadOffset<8> thr_offs, FrameOffset fr_offs,
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

  // src holds a handle scope entry (Object**) load this into dst
  void LoadReferenceFromHandleScope(ManagedRegister dst, ManagedRegister src) OVERRIDE;

  // Heap::VerifyObject on src. In some cases (such as a reference to this) we
  // know that src may not be null.
  void VerifyObject(ManagedRegister src, bool could_be_null) OVERRIDE;
  void VerifyObject(FrameOffset src, bool could_be_null) OVERRIDE;

  // Call to address held at [base+offset]
  void Call(ManagedRegister base, Offset offset, ManagedRegister mscratch) OVERRIDE;
  void Call(FrameOffset base, Offset offset, ManagedRegister mscratch) OVERRIDE;
  void CallFromThread64(ThreadOffset<8> offset, ManagedRegister mscratch) OVERRIDE;

  // Generate code to check if Thread::Current()->exception_ is non-null
  // and branch to a ExceptionSlowPath if it is.
  void ExceptionPoll(ManagedRegister mscratch, size_t stack_adjust) OVERRIDE;

 private:
  void EmitR(int opcode, GpuRegister rs, GpuRegister rt, GpuRegister rd, int shamt, int funct);
  void EmitI(int opcode, GpuRegister rs, GpuRegister rt, uint16_t imm);
  void EmitI21(int opcode, GpuRegister rs, uint32_t imm21);
  void EmitJ(int opcode, uint32_t addr26);
  void EmitFR(int opcode, int fmt, FpuRegister ft, FpuRegister fs, FpuRegister fd, int funct);
  void EmitFI(int opcode, int fmt, FpuRegister rt, uint16_t imm);

  DISALLOW_COPY_AND_ASSIGN(Mips64Assembler);
};

// Slowpath entered when Thread::Current()->_exception is non-null
class Mips64ExceptionSlowPath FINAL : public SlowPath {
 public:
  explicit Mips64ExceptionSlowPath(Mips64ManagedRegister scratch, size_t stack_adjust)
      : scratch_(scratch), stack_adjust_(stack_adjust) {}
  virtual void Emit(Assembler *sp_asm) OVERRIDE;
 private:
  const Mips64ManagedRegister scratch_;
  const size_t stack_adjust_;
};

}  // namespace mips64
}  // namespace art

#endif  // ART_COMPILER_UTILS_MIPS64_ASSEMBLER_MIPS64_H_
