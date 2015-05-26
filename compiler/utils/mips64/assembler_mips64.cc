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

#include "assembler_mips64.h"

#include "base/bit_utils.h"
#include "base/casts.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "memory_region.h"
#include "thread.h"

namespace art {
namespace mips64 {

void Mips64Assembler::Emit(int32_t value) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  buffer_.Emit<int32_t>(value);
}

void Mips64Assembler::EmitR(int opcode, GpuRegister rs, GpuRegister rt, GpuRegister rd,
                            int shamt, int funct) {
  CHECK_NE(rs, kNoGpuRegister);
  CHECK_NE(rt, kNoGpuRegister);
  CHECK_NE(rd, kNoGpuRegister);
  int32_t encoding = opcode << kOpcodeShift |
                     static_cast<int32_t>(rs) << kRsShift |
                     static_cast<int32_t>(rt) << kRtShift |
                     static_cast<int32_t>(rd) << kRdShift |
                     shamt << kShamtShift |
                     funct;
  Emit(encoding);
}

void Mips64Assembler::EmitI(int opcode, GpuRegister rs, GpuRegister rt, uint16_t imm) {
  CHECK_NE(rs, kNoGpuRegister);
  CHECK_NE(rt, kNoGpuRegister);
  int32_t encoding = opcode << kOpcodeShift |
                     static_cast<int32_t>(rs) << kRsShift |
                     static_cast<int32_t>(rt) << kRtShift |
                     imm;
  Emit(encoding);
}

void Mips64Assembler::EmitJ(int opcode, int address) {
  int32_t encoding = opcode << kOpcodeShift |
                     address;
  Emit(encoding);
}

void Mips64Assembler::EmitFR(int opcode, int fmt, FpuRegister ft, FpuRegister fs, FpuRegister fd,
int funct) {
  CHECK_NE(ft, kNoFpuRegister);
  CHECK_NE(fs, kNoFpuRegister);
  CHECK_NE(fd, kNoFpuRegister);
  int32_t encoding = opcode << kOpcodeShift |
                     fmt << kFmtShift |
                     static_cast<int32_t>(ft) << kFtShift |
                     static_cast<int32_t>(fs) << kFsShift |
                     static_cast<int32_t>(fd) << kFdShift |
                     funct;
  Emit(encoding);
}

void Mips64Assembler::EmitFI(int opcode, int fmt, FpuRegister rt, uint16_t imm) {
  CHECK_NE(rt, kNoFpuRegister);
  int32_t encoding = opcode << kOpcodeShift |
                     fmt << kFmtShift |
                     static_cast<int32_t>(rt) << kRtShift |
                     imm;
  Emit(encoding);
}

void Mips64Assembler::EmitBranch(GpuRegister rt, GpuRegister rs, Label* label, bool equal) {
  int offset;
  if (label->IsBound()) {
    offset = label->Position() - buffer_.Size();
  } else {
    // Use the offset field of the branch instruction for linking the sites.
    offset = label->position_;
    label->LinkTo(buffer_.Size());
  }
  if (equal) {
    Beq(rt, rs, (offset >> 2) & kBranchOffsetMask);
  } else {
    Bne(rt, rs, (offset >> 2) & kBranchOffsetMask);
  }
}

void Mips64Assembler::EmitJump(Label* label, bool link) {
  int offset;
  if (label->IsBound()) {
    offset = label->Position() - buffer_.Size();
  } else {
    // Use the offset field of the jump instruction for linking the sites.
    offset = label->position_;
    label->LinkTo(buffer_.Size());
  }
  if (link) {
    Jal((offset >> 2) & kJumpOffsetMask);
  } else {
    J((offset >> 2) & kJumpOffsetMask);
  }
}

int32_t Mips64Assembler::EncodeBranchOffset(int offset, int32_t inst, bool is_jump) {
  CHECK_ALIGNED(offset, 4);
  CHECK(IsInt<POPCOUNT(kBranchOffsetMask)>(offset)) << offset;

  // Properly preserve only the bits supported in the instruction.
  offset >>= 2;
  if (is_jump) {
    offset &= kJumpOffsetMask;
    return (inst & ~kJumpOffsetMask) | offset;
  } else {
    offset &= kBranchOffsetMask;
    return (inst & ~kBranchOffsetMask) | offset;
  }
}

int Mips64Assembler::DecodeBranchOffset(int32_t inst, bool is_jump) {
  // Sign-extend, then left-shift by 2.
  if (is_jump) {
    return (((inst & kJumpOffsetMask) << 6) >> 4);
  } else {
    return (((inst & kBranchOffsetMask) << 16) >> 14);
  }
}

void Mips64Assembler::Bind(Label* label, bool is_jump) {
  CHECK(!label->IsBound());
  int bound_pc = buffer_.Size();
  while (label->IsLinked()) {
    int32_t position = label->Position();
    int32_t next = buffer_.Load<int32_t>(position);
    int32_t offset = is_jump ? bound_pc - position : bound_pc - position - 4;
    int32_t encoded = Mips64Assembler::EncodeBranchOffset(offset, next, is_jump);
    buffer_.Store<int32_t>(position, encoded);
    label->position_ = Mips64Assembler::DecodeBranchOffset(next, is_jump);
  }
  label->BindTo(bound_pc);
}

void Mips64Assembler::Add(GpuRegister rd, GpuRegister rs, GpuRegister rt) {
  EmitR(0, rs, rt, rd, 0, 0x20);
}

void Mips64Assembler::Addi(GpuRegister rt, GpuRegister rs, uint16_t imm16) {
  EmitI(0x8, rs, rt, imm16);
}

void Mips64Assembler::Addu(GpuRegister rd, GpuRegister rs, GpuRegister rt) {
  EmitR(0, rs, rt, rd, 0, 0x21);
}

void Mips64Assembler::Addiu(GpuRegister rt, GpuRegister rs, uint16_t imm16) {
  EmitI(0x9, rs, rt, imm16);
}

void Mips64Assembler::Daddiu(GpuRegister rt, GpuRegister rs, uint16_t imm16) {
  EmitI(0x19, rs, rt, imm16);
}

void Mips64Assembler::Sub(GpuRegister rd, GpuRegister rs, GpuRegister rt) {
  EmitR(0, rs, rt, rd, 0, 0x22);
}

void Mips64Assembler::Subu(GpuRegister rd, GpuRegister rs, GpuRegister rt) {
  EmitR(0, rs, rt, rd, 0, 0x23);
}

void Mips64Assembler::Mult(GpuRegister rs, GpuRegister rt) {
  EmitR(0, rs, rt, static_cast<GpuRegister>(0), 0, 0x18);
}

void Mips64Assembler::Multu(GpuRegister rs, GpuRegister rt) {
  EmitR(0, rs, rt, static_cast<GpuRegister>(0), 0, 0x19);
}

void Mips64Assembler::Div(GpuRegister rs, GpuRegister rt) {
  EmitR(0, rs, rt, static_cast<GpuRegister>(0), 0, 0x1a);
}

void Mips64Assembler::Divu(GpuRegister rs, GpuRegister rt) {
  EmitR(0, rs, rt, static_cast<GpuRegister>(0), 0, 0x1b);
}

void Mips64Assembler::And(GpuRegister rd, GpuRegister rs, GpuRegister rt) {
  EmitR(0, rs, rt, rd, 0, 0x24);
}

void Mips64Assembler::Andi(GpuRegister rt, GpuRegister rs, uint16_t imm16) {
  EmitI(0xc, rs, rt, imm16);
}

void Mips64Assembler::Or(GpuRegister rd, GpuRegister rs, GpuRegister rt) {
  EmitR(0, rs, rt, rd, 0, 0x25);
}

void Mips64Assembler::Ori(GpuRegister rt, GpuRegister rs, uint16_t imm16) {
  EmitI(0xd, rs, rt, imm16);
}

void Mips64Assembler::Xor(GpuRegister rd, GpuRegister rs, GpuRegister rt) {
  EmitR(0, rs, rt, rd, 0, 0x26);
}

void Mips64Assembler::Xori(GpuRegister rt, GpuRegister rs, uint16_t imm16) {
  EmitI(0xe, rs, rt, imm16);
}

void Mips64Assembler::Nor(GpuRegister rd, GpuRegister rs, GpuRegister rt) {
  EmitR(0, rs, rt, rd, 0, 0x27);
}

void Mips64Assembler::Sll(GpuRegister rd, GpuRegister rs, int shamt) {
  EmitR(0, rs, static_cast<GpuRegister>(0), rd, shamt, 0x00);
}

void Mips64Assembler::Srl(GpuRegister rd, GpuRegister rs, int shamt) {
  EmitR(0, rs, static_cast<GpuRegister>(0), rd, shamt, 0x02);
}

void Mips64Assembler::Sra(GpuRegister rd, GpuRegister rs, int shamt) {
  EmitR(0, rs, static_cast<GpuRegister>(0), rd, shamt, 0x03);
}

void Mips64Assembler::Sllv(GpuRegister rd, GpuRegister rs, GpuRegister rt) {
  EmitR(0, rs, rt, rd, 0, 0x04);
}

void Mips64Assembler::Srlv(GpuRegister rd, GpuRegister rs, GpuRegister rt) {
  EmitR(0, rs, rt, rd, 0, 0x06);
}

void Mips64Assembler::Srav(GpuRegister rd, GpuRegister rs, GpuRegister rt) {
  EmitR(0, rs, rt, rd, 0, 0x07);
}

void Mips64Assembler::Lb(GpuRegister rt, GpuRegister rs, uint16_t imm16) {
  EmitI(0x20, rs, rt, imm16);
}

void Mips64Assembler::Lh(GpuRegister rt, GpuRegister rs, uint16_t imm16) {
  EmitI(0x21, rs, rt, imm16);
}

void Mips64Assembler::Lw(GpuRegister rt, GpuRegister rs, uint16_t imm16) {
  EmitI(0x23, rs, rt, imm16);
}

void Mips64Assembler::Ld(GpuRegister rt, GpuRegister rs, uint16_t imm16) {
  EmitI(0x37, rs, rt, imm16);
}

void Mips64Assembler::Lbu(GpuRegister rt, GpuRegister rs, uint16_t imm16) {
  EmitI(0x24, rs, rt, imm16);
}

void Mips64Assembler::Lhu(GpuRegister rt, GpuRegister rs, uint16_t imm16) {
  EmitI(0x25, rs, rt, imm16);
}

void Mips64Assembler::Lwu(GpuRegister rt, GpuRegister rs, uint16_t imm16) {
  EmitI(0x27, rs, rt, imm16);
}

void Mips64Assembler::Lui(GpuRegister rt, uint16_t imm16) {
  EmitI(0xf, static_cast<GpuRegister>(0), rt, imm16);
}

void Mips64Assembler::Mfhi(GpuRegister rd) {
  EmitR(0, static_cast<GpuRegister>(0), static_cast<GpuRegister>(0), rd, 0, 0x10);
}

void Mips64Assembler::Mflo(GpuRegister rd) {
  EmitR(0, static_cast<GpuRegister>(0), static_cast<GpuRegister>(0), rd, 0, 0x12);
}

void Mips64Assembler::Sb(GpuRegister rt, GpuRegister rs, uint16_t imm16) {
  EmitI(0x28, rs, rt, imm16);
}

void Mips64Assembler::Sh(GpuRegister rt, GpuRegister rs, uint16_t imm16) {
  EmitI(0x29, rs, rt, imm16);
}

void Mips64Assembler::Sw(GpuRegister rt, GpuRegister rs, uint16_t imm16) {
  EmitI(0x2b, rs, rt, imm16);
}

void Mips64Assembler::Sd(GpuRegister rt, GpuRegister rs, uint16_t imm16) {
  EmitI(0x3f, rs, rt, imm16);
}

void Mips64Assembler::Slt(GpuRegister rd, GpuRegister rs, GpuRegister rt) {
  EmitR(0, rs, rt, rd, 0, 0x2a);
}

void Mips64Assembler::Sltu(GpuRegister rd, GpuRegister rs, GpuRegister rt) {
  EmitR(0, rs, rt, rd, 0, 0x2b);
}

void Mips64Assembler::Slti(GpuRegister rt, GpuRegister rs, uint16_t imm16) {
  EmitI(0xa, rs, rt, imm16);
}

void Mips64Assembler::Sltiu(GpuRegister rt, GpuRegister rs, uint16_t imm16) {
  EmitI(0xb, rs, rt, imm16);
}

void Mips64Assembler::Beq(GpuRegister rt, GpuRegister rs, uint16_t imm16) {
  EmitI(0x4, rs, rt, imm16);
  Nop();
}

void Mips64Assembler::Bne(GpuRegister rt, GpuRegister rs, uint16_t imm16) {
  EmitI(0x5, rs, rt, imm16);
  Nop();
}

void Mips64Assembler::J(uint32_t address) {
  EmitJ(0x2, address);
  Nop();
}

void Mips64Assembler::Jal(uint32_t address) {
  EmitJ(0x2, address);
  Nop();
}

void Mips64Assembler::Jr(GpuRegister rs) {
  EmitR(0, rs, static_cast<GpuRegister>(0), static_cast<GpuRegister>(0), 0, 0x09);  // Jalr zero, rs
  Nop();
}

void Mips64Assembler::Jalr(GpuRegister rs) {
  EmitR(0, rs, static_cast<GpuRegister>(0), RA, 0, 0x09);
  Nop();
}

void Mips64Assembler::AddS(FpuRegister fd, FpuRegister fs, FpuRegister ft) {
  EmitFR(0x11, 0x10, ft, fs, fd, 0x0);
}

void Mips64Assembler::SubS(FpuRegister fd, FpuRegister fs, FpuRegister ft) {
  EmitFR(0x11, 0x10, ft, fs, fd, 0x1);
}

void Mips64Assembler::MulS(FpuRegister fd, FpuRegister fs, FpuRegister ft) {
  EmitFR(0x11, 0x10, ft, fs, fd, 0x2);
}

void Mips64Assembler::DivS(FpuRegister fd, FpuRegister fs, FpuRegister ft) {
  EmitFR(0x11, 0x10, ft, fs, fd, 0x3);
}

void Mips64Assembler::AddD(FpuRegister fd, FpuRegister fs, FpuRegister ft) {
  EmitFR(0x11, 0x11, static_cast<FpuRegister>(ft), static_cast<FpuRegister>(fs),
         static_cast<FpuRegister>(fd), 0x0);
}

void Mips64Assembler::SubD(FpuRegister fd, FpuRegister fs, FpuRegister ft) {
  EmitFR(0x11, 0x11, static_cast<FpuRegister>(ft), static_cast<FpuRegister>(fs),
         static_cast<FpuRegister>(fd), 0x1);
}

void Mips64Assembler::MulD(FpuRegister fd, FpuRegister fs, FpuRegister ft) {
  EmitFR(0x11, 0x11, static_cast<FpuRegister>(ft), static_cast<FpuRegister>(fs),
         static_cast<FpuRegister>(fd), 0x2);
}

void Mips64Assembler::DivD(FpuRegister fd, FpuRegister fs, FpuRegister ft) {
  EmitFR(0x11, 0x11, static_cast<FpuRegister>(ft), static_cast<FpuRegister>(fs),
         static_cast<FpuRegister>(fd), 0x3);
}

void Mips64Assembler::MovS(FpuRegister fd, FpuRegister fs) {
  EmitFR(0x11, 0x10, static_cast<FpuRegister>(0), fs, fd, 0x6);
}

void Mips64Assembler::MovD(FpuRegister fd, FpuRegister fs) {
  EmitFR(0x11, 0x11, static_cast<FpuRegister>(0), static_cast<FpuRegister>(fs),
         static_cast<FpuRegister>(fd), 0x6);
}

void Mips64Assembler::Mfc1(GpuRegister rt, FpuRegister fs) {
  EmitFR(0x11, 0x00, static_cast<FpuRegister>(rt), fs, static_cast<FpuRegister>(0), 0x0);
}

void Mips64Assembler::Mtc1(FpuRegister ft, GpuRegister rs) {
  EmitFR(0x11, 0x04, ft, static_cast<FpuRegister>(rs), static_cast<FpuRegister>(0), 0x0);
}

void Mips64Assembler::Lwc1(FpuRegister ft, GpuRegister rs, uint16_t imm16) {
  EmitI(0x31, rs, static_cast<GpuRegister>(ft), imm16);
}

void Mips64Assembler::Ldc1(FpuRegister ft, GpuRegister rs, uint16_t imm16) {
  EmitI(0x35, rs, static_cast<GpuRegister>(ft), imm16);
}

void Mips64Assembler::Swc1(FpuRegister ft, GpuRegister rs, uint16_t imm16) {
  EmitI(0x39, rs, static_cast<GpuRegister>(ft), imm16);
}

void Mips64Assembler::Sdc1(FpuRegister ft, GpuRegister rs, uint16_t imm16) {
  EmitI(0x3d, rs, static_cast<GpuRegister>(ft), imm16);
}

void Mips64Assembler::Break() {
  EmitR(0, static_cast<GpuRegister>(0), static_cast<GpuRegister>(0),
        static_cast<GpuRegister>(0), 0, 0xD);
}

void Mips64Assembler::Nop() {
  EmitR(0x0, static_cast<GpuRegister>(0), static_cast<GpuRegister>(0),
        static_cast<GpuRegister>(0), 0, 0x0);
}

void Mips64Assembler::Move(GpuRegister rt, GpuRegister rs) {
  EmitI(0x19, rs, rt, 0);   // Daddiu
}

void Mips64Assembler::Clear(GpuRegister rt) {
  EmitR(0, static_cast<GpuRegister>(0), static_cast<GpuRegister>(0), rt, 0, 0x20);
}

void Mips64Assembler::Not(GpuRegister rt, GpuRegister rs) {
  EmitR(0, static_cast<GpuRegister>(0), rs, rt, 0, 0x27);
}

void Mips64Assembler::Mul(GpuRegister rd, GpuRegister rs, GpuRegister rt) {
  Mult(rs, rt);
  Mflo(rd);
}

void Mips64Assembler::Div(GpuRegister rd, GpuRegister rs, GpuRegister rt) {
  Div(rs, rt);
  Mflo(rd);
}

void Mips64Assembler::Rem(GpuRegister rd, GpuRegister rs, GpuRegister rt) {
  Div(rs, rt);
  Mfhi(rd);
}

void Mips64Assembler::AddConstant64(GpuRegister rt, GpuRegister rs, int32_t value) {
  CHECK((value >= -32768) && (value <= 32766));
  Daddiu(rt, rs, value);
}

void Mips64Assembler::LoadImmediate64(GpuRegister rt, int32_t value) {
  CHECK((value >= -32768) && (value <= 32766));
  Daddiu(rt, ZERO, value);
}

void Mips64Assembler::LoadFromOffset(LoadOperandType type, GpuRegister reg, GpuRegister base,
                                     int32_t offset) {
  switch (type) {
    case kLoadSignedByte:
      Lb(reg, base, offset);
      break;
    case kLoadUnsignedByte:
      Lbu(reg, base, offset);
      break;
    case kLoadSignedHalfword:
      Lh(reg, base, offset);
      break;
    case kLoadUnsignedHalfword:
      Lhu(reg, base, offset);
      break;
    case kLoadWord:
      Lw(reg, base, offset);
      break;
    case kLoadUnsignedWord:
      Lwu(reg, base, offset);
      break;
    case kLoadDoubleword:
      // TODO: alignment issues ???
      Ld(reg, base, offset);
      break;
    default:
      LOG(FATAL) << "UNREACHABLE";
  }
}

void Mips64Assembler::LoadFpuFromOffset(LoadOperandType type, FpuRegister reg, GpuRegister base,
                                        int32_t offset) {
  CHECK((offset >= -32768) && (offset <= 32766));
  switch (type) {
    case kLoadWord:
      Lwc1(reg, base, offset);
      break;
    case kLoadDoubleword:
      // TODO: alignment issues ???
      Ldc1(reg, base, offset);
      break;
    default:
      LOG(FATAL) << "UNREACHABLE";
  }
}

void Mips64Assembler::EmitLoad(ManagedRegister m_dst, GpuRegister src_register, int32_t src_offset,
                               size_t size) {
  Mips64ManagedRegister dst = m_dst.AsMips64();
  if (dst.IsNoRegister()) {
    CHECK_EQ(0u, size) << dst;
  } else if (dst.IsGpuRegister()) {
    if (size == 4) {
      LoadFromOffset(kLoadWord, dst.AsGpuRegister(), src_register, src_offset);
    } else if (size == 8) {
      CHECK_EQ(8u, size) << dst;
      LoadFromOffset(kLoadDoubleword, dst.AsGpuRegister(), src_register, src_offset);
    } else {
      UNIMPLEMENTED(FATAL) << "We only support Load() of size 4 and 8";
    }
  } else if (dst.IsFpuRegister()) {
    if (size == 4) {
      CHECK_EQ(4u, size) << dst;
      LoadFpuFromOffset(kLoadWord, dst.AsFpuRegister(), src_register, src_offset);
    } else if (size == 8) {
      CHECK_EQ(8u, size) << dst;
      LoadFpuFromOffset(kLoadDoubleword, dst.AsFpuRegister(), src_register, src_offset);
    } else {
      UNIMPLEMENTED(FATAL) << "We only support Load() of size 4 and 8";
    }
  }
}

void Mips64Assembler::StoreToOffset(StoreOperandType type, GpuRegister reg, GpuRegister base,
                                    int32_t offset) {
  switch (type) {
    case kStoreByte:
      Sb(reg, base, offset);
      break;
    case kStoreHalfword:
      Sh(reg, base, offset);
      break;
    case kStoreWord:
      Sw(reg, base, offset);
      break;
    case kStoreDoubleword:
      // TODO: alignment issues ???
      Sd(reg, base, offset);
      break;
    default:
      LOG(FATAL) << "UNREACHABLE";
  }
}

void Mips64Assembler::StoreFpuToOffset(StoreOperandType type, FpuRegister reg, GpuRegister base,
                                       int32_t offset) {
  switch (type) {
    case kStoreWord:
      Swc1(reg, base, offset);
      break;
    case kStoreDoubleword:
      Sdc1(reg, base, offset);
      break;
    default:
      LOG(FATAL) << "UNREACHABLE";
  }
}

static dwarf::Reg DWARFReg(GpuRegister reg) {
  return dwarf::Reg::Mips64Core(static_cast<int>(reg));
}

constexpr size_t kFramePointerSize = 8;

void Mips64Assembler::BuildFrame(size_t frame_size, ManagedRegister method_reg,
                                 const std::vector<ManagedRegister>& callee_save_regs,
                                 const ManagedRegisterEntrySpills& entry_spills) {
  CHECK_ALIGNED(frame_size, kStackAlignment);

  // Increase frame to required size.
  IncreaseFrameSize(frame_size);

  // Push callee saves and return address
  int stack_offset = frame_size - kFramePointerSize;
  StoreToOffset(kStoreDoubleword, RA, SP, stack_offset);
  cfi_.RelOffset(DWARFReg(RA), stack_offset);
  for (int i = callee_save_regs.size() - 1; i >= 0; --i) {
    stack_offset -= kFramePointerSize;
    GpuRegister reg = callee_save_regs.at(i).AsMips64().AsGpuRegister();
    StoreToOffset(kStoreDoubleword, reg, SP, stack_offset);
    cfi_.RelOffset(DWARFReg(reg), stack_offset);
  }

  // Write out Method*.
  StoreToOffset(kStoreWord, method_reg.AsMips64().AsGpuRegister(), SP, 0);

  // Write out entry spills.
  int32_t offset = frame_size + sizeof(StackReference<mirror::ArtMethod>);
  for (size_t i = 0; i < entry_spills.size(); ++i) {
    Mips64ManagedRegister reg = entry_spills.at(i).AsMips64();
    ManagedRegisterSpill spill = entry_spills.at(i);
    int32_t size = spill.getSize();
    if (reg.IsNoRegister()) {
      // only increment stack offset.
      offset += size;
    } else if (reg.IsFpuRegister()) {
      StoreFpuToOffset((size == 4) ? kStoreWord : kStoreDoubleword, reg.AsFpuRegister(), SP, offset);
      offset += size;
    } else if (reg.IsGpuRegister()) {
      StoreToOffset((size == 4) ? kStoreWord : kStoreDoubleword, reg.AsGpuRegister(), SP, offset);
      offset += size;
    }
  }
}

void Mips64Assembler::RemoveFrame(size_t frame_size,
                                  const std::vector<ManagedRegister>& callee_save_regs) {
  CHECK_ALIGNED(frame_size, kStackAlignment);
  cfi_.RememberState();

  // Pop callee saves and return address
  int stack_offset = frame_size - (callee_save_regs.size() * kFramePointerSize) - kFramePointerSize;
  for (size_t i = 0; i < callee_save_regs.size(); ++i) {
    GpuRegister reg = callee_save_regs.at(i).AsMips64().AsGpuRegister();
    LoadFromOffset(kLoadDoubleword, reg, SP, stack_offset);
    cfi_.Restore(DWARFReg(reg));
    stack_offset += kFramePointerSize;
  }
  LoadFromOffset(kLoadDoubleword, RA, SP, stack_offset);
  cfi_.Restore(DWARFReg(RA));

  // Decrease frame to required size.
  DecreaseFrameSize(frame_size);

  // Then jump to the return address.
  Jr(RA);

  // The CFI should be restored for any code that follows the exit block.
  cfi_.RestoreState();
  cfi_.DefCFAOffset(frame_size);
}

void Mips64Assembler::IncreaseFrameSize(size_t adjust) {
  CHECK_ALIGNED(adjust, kStackAlignment);
  AddConstant64(SP, SP, -adjust);
  cfi_.AdjustCFAOffset(adjust);
}

void Mips64Assembler::DecreaseFrameSize(size_t adjust) {
  CHECK_ALIGNED(adjust, kStackAlignment);
  AddConstant64(SP, SP, adjust);
  cfi_.AdjustCFAOffset(-adjust);
}

void Mips64Assembler::Store(FrameOffset dest, ManagedRegister msrc, size_t size) {
  Mips64ManagedRegister src = msrc.AsMips64();
  if (src.IsNoRegister()) {
    CHECK_EQ(0u, size);
  } else if (src.IsGpuRegister()) {
    CHECK(size == 4 || size == 8) << size;
    if (size == 8) {
      StoreToOffset(kStoreDoubleword, src.AsGpuRegister(), SP, dest.Int32Value());
    } else if (size == 4) {
      StoreToOffset(kStoreWord, src.AsGpuRegister(), SP, dest.Int32Value());
    } else {
      UNIMPLEMENTED(FATAL) << "We only support Store() of size 4 and 8";
    }
  } else if (src.IsFpuRegister()) {
    CHECK(size == 4 || size == 8) << size;
    if (size == 8) {
      StoreFpuToOffset(kStoreDoubleword, src.AsFpuRegister(), SP, dest.Int32Value());
    } else if (size == 4) {
      StoreFpuToOffset(kStoreWord, src.AsFpuRegister(), SP, dest.Int32Value());
    } else {
      UNIMPLEMENTED(FATAL) << "We only support Store() of size 4 and 8";
    }
  }
}

void Mips64Assembler::StoreRef(FrameOffset dest, ManagedRegister msrc) {
  Mips64ManagedRegister src = msrc.AsMips64();
  CHECK(src.IsGpuRegister());
  StoreToOffset(kStoreWord, src.AsGpuRegister(), SP, dest.Int32Value());
}

void Mips64Assembler::StoreRawPtr(FrameOffset dest, ManagedRegister msrc) {
  Mips64ManagedRegister src = msrc.AsMips64();
  CHECK(src.IsGpuRegister());
  StoreToOffset(kStoreDoubleword, src.AsGpuRegister(), SP, dest.Int32Value());
}

void Mips64Assembler::StoreImmediateToFrame(FrameOffset dest, uint32_t imm,
                                            ManagedRegister mscratch) {
  Mips64ManagedRegister scratch = mscratch.AsMips64();
  CHECK(scratch.IsGpuRegister()) << scratch;
  LoadImmediate64(scratch.AsGpuRegister(), imm);
  StoreToOffset(kStoreWord, scratch.AsGpuRegister(), SP, dest.Int32Value());
}

void Mips64Assembler::StoreImmediateToThread64(ThreadOffset<8> dest, uint32_t imm,
                                               ManagedRegister mscratch) {
  Mips64ManagedRegister scratch = mscratch.AsMips64();
  CHECK(scratch.IsGpuRegister()) << scratch;
  LoadImmediate64(scratch.AsGpuRegister(), imm);
  StoreToOffset(kStoreDoubleword, scratch.AsGpuRegister(), S1, dest.Int32Value());
}

void Mips64Assembler::StoreStackOffsetToThread64(ThreadOffset<8> thr_offs,
                                                 FrameOffset fr_offs,
                                                 ManagedRegister mscratch) {
  Mips64ManagedRegister scratch = mscratch.AsMips64();
  CHECK(scratch.IsGpuRegister()) << scratch;
  AddConstant64(scratch.AsGpuRegister(), SP, fr_offs.Int32Value());
  StoreToOffset(kStoreDoubleword, scratch.AsGpuRegister(), S1, thr_offs.Int32Value());
}

void Mips64Assembler::StoreStackPointerToThread64(ThreadOffset<8> thr_offs) {
  StoreToOffset(kStoreDoubleword, SP, S1, thr_offs.Int32Value());
}

void Mips64Assembler::StoreSpanning(FrameOffset dest, ManagedRegister msrc,
                                    FrameOffset in_off, ManagedRegister mscratch) {
  Mips64ManagedRegister src = msrc.AsMips64();
  Mips64ManagedRegister scratch = mscratch.AsMips64();
  StoreToOffset(kStoreDoubleword, src.AsGpuRegister(), SP, dest.Int32Value());
  LoadFromOffset(kLoadDoubleword, scratch.AsGpuRegister(), SP, in_off.Int32Value());
  StoreToOffset(kStoreDoubleword, scratch.AsGpuRegister(), SP, dest.Int32Value() + 8);
}

void Mips64Assembler::Load(ManagedRegister mdest, FrameOffset src, size_t size) {
  return EmitLoad(mdest, SP, src.Int32Value(), size);
}

void Mips64Assembler::LoadFromThread64(ManagedRegister mdest, ThreadOffset<8> src, size_t size) {
  return EmitLoad(mdest, S1, src.Int32Value(), size);
}

void Mips64Assembler::LoadRef(ManagedRegister mdest, FrameOffset src) {
  Mips64ManagedRegister dest = mdest.AsMips64();
  CHECK(dest.IsGpuRegister());
  LoadFromOffset(kLoadUnsignedWord, dest.AsGpuRegister(), SP, src.Int32Value());
}

void Mips64Assembler::LoadRef(ManagedRegister mdest, ManagedRegister base, MemberOffset offs) {
  Mips64ManagedRegister dest = mdest.AsMips64();
  CHECK(dest.IsGpuRegister() && base.AsMips64().IsGpuRegister());
  LoadFromOffset(kLoadUnsignedWord, dest.AsGpuRegister(),
                 base.AsMips64().AsGpuRegister(), offs.Int32Value());
  if (kPoisonHeapReferences) {
    Subu(dest.AsGpuRegister(), ZERO, dest.AsGpuRegister());
  }
}

void Mips64Assembler::LoadRawPtr(ManagedRegister mdest, ManagedRegister base,
                               Offset offs) {
  Mips64ManagedRegister dest = mdest.AsMips64();
  CHECK(dest.IsGpuRegister() && dest.IsGpuRegister()) << dest;
  LoadFromOffset(kLoadDoubleword, dest.AsGpuRegister(),
                 base.AsMips64().AsGpuRegister(), offs.Int32Value());
}

void Mips64Assembler::LoadRawPtrFromThread64(ManagedRegister mdest,
                                         ThreadOffset<8> offs) {
  Mips64ManagedRegister dest = mdest.AsMips64();
  CHECK(dest.IsGpuRegister());
  LoadFromOffset(kLoadDoubleword, dest.AsGpuRegister(), S1, offs.Int32Value());
}

void Mips64Assembler::SignExtend(ManagedRegister /*mreg*/, size_t /*size*/) {
  UNIMPLEMENTED(FATAL) << "no sign extension necessary for mips";
}

void Mips64Assembler::ZeroExtend(ManagedRegister /*mreg*/, size_t /*size*/) {
  UNIMPLEMENTED(FATAL) << "no zero extension necessary for mips";
}

void Mips64Assembler::Move(ManagedRegister mdest, ManagedRegister msrc, size_t size) {
  Mips64ManagedRegister dest = mdest.AsMips64();
  Mips64ManagedRegister src = msrc.AsMips64();
  if (!dest.Equals(src)) {
    if (dest.IsGpuRegister()) {
      CHECK(src.IsGpuRegister()) << src;
      Move(dest.AsGpuRegister(), src.AsGpuRegister());
    } else if (dest.IsFpuRegister()) {
      CHECK(src.IsFpuRegister()) << src;
      if (size == 4) {
        MovS(dest.AsFpuRegister(), src.AsFpuRegister());
      } else if (size == 8) {
        MovD(dest.AsFpuRegister(), src.AsFpuRegister());
      } else {
        UNIMPLEMENTED(FATAL) << "We only support Copy() of size 4 and 8";
      }
    }
  }
}

void Mips64Assembler::CopyRef(FrameOffset dest, FrameOffset src,
                              ManagedRegister mscratch) {
  Mips64ManagedRegister scratch = mscratch.AsMips64();
  CHECK(scratch.IsGpuRegister()) << scratch;
  LoadFromOffset(kLoadWord, scratch.AsGpuRegister(), SP, src.Int32Value());
  StoreToOffset(kStoreWord, scratch.AsGpuRegister(), SP, dest.Int32Value());
}

void Mips64Assembler::CopyRawPtrFromThread64(FrameOffset fr_offs,
                                             ThreadOffset<8> thr_offs,
                                             ManagedRegister mscratch) {
  Mips64ManagedRegister scratch = mscratch.AsMips64();
  CHECK(scratch.IsGpuRegister()) << scratch;
  LoadFromOffset(kLoadDoubleword, scratch.AsGpuRegister(), S1, thr_offs.Int32Value());
  StoreToOffset(kStoreDoubleword, scratch.AsGpuRegister(), SP, fr_offs.Int32Value());
}

void Mips64Assembler::CopyRawPtrToThread64(ThreadOffset<8> thr_offs,
                                           FrameOffset fr_offs,
                                           ManagedRegister mscratch) {
  Mips64ManagedRegister scratch = mscratch.AsMips64();
  CHECK(scratch.IsGpuRegister()) << scratch;
  LoadFromOffset(kLoadDoubleword, scratch.AsGpuRegister(),
                 SP, fr_offs.Int32Value());
  StoreToOffset(kStoreDoubleword, scratch.AsGpuRegister(),
                S1, thr_offs.Int32Value());
}

void Mips64Assembler::Copy(FrameOffset dest, FrameOffset src,
                           ManagedRegister mscratch, size_t size) {
  Mips64ManagedRegister scratch = mscratch.AsMips64();
  CHECK(scratch.IsGpuRegister()) << scratch;
  CHECK(size == 4 || size == 8) << size;
  if (size == 4) {
    LoadFromOffset(kLoadWord, scratch.AsGpuRegister(), SP, src.Int32Value());
    StoreToOffset(kStoreWord, scratch.AsGpuRegister(), SP, dest.Int32Value());
  } else if (size == 8) {
    LoadFromOffset(kLoadDoubleword, scratch.AsGpuRegister(), SP, src.Int32Value());
    StoreToOffset(kStoreDoubleword, scratch.AsGpuRegister(), SP, dest.Int32Value());
  } else {
    UNIMPLEMENTED(FATAL) << "We only support Copy() of size 4 and 8";
  }
}

void Mips64Assembler::Copy(FrameOffset dest, ManagedRegister src_base, Offset src_offset,
                         ManagedRegister mscratch, size_t size) {
  GpuRegister scratch = mscratch.AsMips64().AsGpuRegister();
  CHECK(size == 4 || size == 8) << size;
  if (size == 4) {
    LoadFromOffset(kLoadWord, scratch, src_base.AsMips64().AsGpuRegister(),
                   src_offset.Int32Value());
    StoreToOffset(kStoreWord, scratch, SP, dest.Int32Value());
  } else if (size == 8) {
    LoadFromOffset(kLoadDoubleword, scratch, src_base.AsMips64().AsGpuRegister(),
                   src_offset.Int32Value());
    StoreToOffset(kStoreDoubleword, scratch, SP, dest.Int32Value());
  } else {
    UNIMPLEMENTED(FATAL) << "We only support Copy() of size 4 and 8";
  }
}

void Mips64Assembler::Copy(ManagedRegister dest_base, Offset dest_offset, FrameOffset src,
                         ManagedRegister mscratch, size_t size) {
  GpuRegister scratch = mscratch.AsMips64().AsGpuRegister();
  CHECK(size == 4 || size == 8) << size;
  if (size == 4) {
    LoadFromOffset(kLoadWord, scratch, SP, src.Int32Value());
    StoreToOffset(kStoreWord, scratch, dest_base.AsMips64().AsGpuRegister(),
                  dest_offset.Int32Value());
  } else if (size == 8) {
    LoadFromOffset(kLoadDoubleword, scratch, SP, src.Int32Value());
    StoreToOffset(kStoreDoubleword, scratch, dest_base.AsMips64().AsGpuRegister(),
                  dest_offset.Int32Value());
  } else {
    UNIMPLEMENTED(FATAL) << "We only support Copy() of size 4 and 8";
  }
}

void Mips64Assembler::Copy(FrameOffset /*dest*/, FrameOffset /*src_base*/, Offset /*src_offset*/,
                         ManagedRegister /*mscratch*/, size_t /*size*/) {
  UNIMPLEMENTED(FATAL) << "no mips64 implementation";
}

void Mips64Assembler::Copy(ManagedRegister dest, Offset dest_offset,
                         ManagedRegister src, Offset src_offset,
                         ManagedRegister mscratch, size_t size) {
  GpuRegister scratch = mscratch.AsMips64().AsGpuRegister();
  CHECK(size == 4 || size == 8) << size;
  if (size == 4) {
    LoadFromOffset(kLoadWord, scratch, src.AsMips64().AsGpuRegister(), src_offset.Int32Value());
    StoreToOffset(kStoreWord, scratch, dest.AsMips64().AsGpuRegister(), dest_offset.Int32Value());
  } else if (size == 8) {
    LoadFromOffset(kLoadDoubleword, scratch, src.AsMips64().AsGpuRegister(),
                   src_offset.Int32Value());
    StoreToOffset(kStoreDoubleword, scratch, dest.AsMips64().AsGpuRegister(),
                  dest_offset.Int32Value());
  } else {
    UNIMPLEMENTED(FATAL) << "We only support Copy() of size 4 and 8";
  }
}

void Mips64Assembler::Copy(FrameOffset /*dest*/, Offset /*dest_offset*/, FrameOffset /*src*/, Offset
/*src_offset*/,
                         ManagedRegister /*mscratch*/, size_t /*size*/) {
  UNIMPLEMENTED(FATAL) << "no mips64 implementation";
}

void Mips64Assembler::MemoryBarrier(ManagedRegister) {
  UNIMPLEMENTED(FATAL) << "no mips64 implementation";
}

void Mips64Assembler::CreateHandleScopeEntry(ManagedRegister mout_reg,
                                    FrameOffset handle_scope_offset,
                                    ManagedRegister min_reg, bool null_allowed) {
  Mips64ManagedRegister out_reg = mout_reg.AsMips64();
  Mips64ManagedRegister in_reg = min_reg.AsMips64();
  CHECK(in_reg.IsNoRegister() || in_reg.IsGpuRegister()) << in_reg;
  CHECK(out_reg.IsGpuRegister()) << out_reg;
  if (null_allowed) {
    Label null_arg;
    // Null values get a handle scope entry value of 0.  Otherwise, the handle scope entry is
    // the address in the handle scope holding the reference.
    // e.g. out_reg = (handle == 0) ? 0 : (SP+handle_offset)
    if (in_reg.IsNoRegister()) {
      LoadFromOffset(kLoadUnsignedWord, out_reg.AsGpuRegister(),
                     SP, handle_scope_offset.Int32Value());
      in_reg = out_reg;
    }
    if (!out_reg.Equals(in_reg)) {
      LoadImmediate64(out_reg.AsGpuRegister(), 0);
    }
    EmitBranch(in_reg.AsGpuRegister(), ZERO, &null_arg, true);
    AddConstant64(out_reg.AsGpuRegister(), SP, handle_scope_offset.Int32Value());
    Bind(&null_arg, false);
  } else {
    AddConstant64(out_reg.AsGpuRegister(), SP, handle_scope_offset.Int32Value());
  }
}

void Mips64Assembler::CreateHandleScopeEntry(FrameOffset out_off,
                                    FrameOffset handle_scope_offset,
                                    ManagedRegister mscratch,
                                    bool null_allowed) {
  Mips64ManagedRegister scratch = mscratch.AsMips64();
  CHECK(scratch.IsGpuRegister()) << scratch;
  if (null_allowed) {
    Label null_arg;
    LoadFromOffset(kLoadUnsignedWord, scratch.AsGpuRegister(), SP,
                   handle_scope_offset.Int32Value());
    // Null values get a handle scope entry value of 0.  Otherwise, the handle scope entry is
    // the address in the handle scope holding the reference.
    // e.g. scratch = (scratch == 0) ? 0 : (SP+handle_scope_offset)
    EmitBranch(scratch.AsGpuRegister(), ZERO, &null_arg, true);
    AddConstant64(scratch.AsGpuRegister(), SP, handle_scope_offset.Int32Value());
    Bind(&null_arg, false);
  } else {
    AddConstant64(scratch.AsGpuRegister(), SP, handle_scope_offset.Int32Value());
  }
  StoreToOffset(kStoreDoubleword, scratch.AsGpuRegister(), SP, out_off.Int32Value());
}

// Given a handle scope entry, load the associated reference.
void Mips64Assembler::LoadReferenceFromHandleScope(ManagedRegister mout_reg,
                                          ManagedRegister min_reg) {
  Mips64ManagedRegister out_reg = mout_reg.AsMips64();
  Mips64ManagedRegister in_reg = min_reg.AsMips64();
  CHECK(out_reg.IsGpuRegister()) << out_reg;
  CHECK(in_reg.IsGpuRegister()) << in_reg;
  Label null_arg;
  if (!out_reg.Equals(in_reg)) {
    LoadImmediate64(out_reg.AsGpuRegister(), 0);
  }
  EmitBranch(in_reg.AsGpuRegister(), ZERO, &null_arg, true);
  LoadFromOffset(kLoadDoubleword, out_reg.AsGpuRegister(),
                 in_reg.AsGpuRegister(), 0);
  Bind(&null_arg, false);
}

void Mips64Assembler::VerifyObject(ManagedRegister /*src*/, bool /*could_be_null*/) {
  // TODO: not validating references
}

void Mips64Assembler::VerifyObject(FrameOffset /*src*/, bool /*could_be_null*/) {
  // TODO: not validating references
}

void Mips64Assembler::Call(ManagedRegister mbase, Offset offset, ManagedRegister mscratch) {
  Mips64ManagedRegister base = mbase.AsMips64();
  Mips64ManagedRegister scratch = mscratch.AsMips64();
  CHECK(base.IsGpuRegister()) << base;
  CHECK(scratch.IsGpuRegister()) << scratch;
  LoadFromOffset(kLoadDoubleword, scratch.AsGpuRegister(),
                 base.AsGpuRegister(), offset.Int32Value());
  Jalr(scratch.AsGpuRegister());
  // TODO: place reference map on call
}

void Mips64Assembler::Call(FrameOffset base, Offset offset, ManagedRegister mscratch) {
  Mips64ManagedRegister scratch = mscratch.AsMips64();
  CHECK(scratch.IsGpuRegister()) << scratch;
  // Call *(*(SP + base) + offset)
  LoadFromOffset(kLoadUnsignedWord, scratch.AsGpuRegister(),
                 SP, base.Int32Value());
  LoadFromOffset(kLoadDoubleword, scratch.AsGpuRegister(),
                 scratch.AsGpuRegister(), offset.Int32Value());
  Jalr(scratch.AsGpuRegister());
  // TODO: place reference map on call
}

void Mips64Assembler::CallFromThread64(ThreadOffset<8> /*offset*/, ManagedRegister /*mscratch*/) {
  UNIMPLEMENTED(FATAL) << "no mips64 implementation";
}

void Mips64Assembler::GetCurrentThread(ManagedRegister tr) {
  Move(tr.AsMips64().AsGpuRegister(), S1);
}

void Mips64Assembler::GetCurrentThread(FrameOffset offset,
                                     ManagedRegister /*mscratch*/) {
  StoreToOffset(kStoreDoubleword, S1, SP, offset.Int32Value());
}

void Mips64Assembler::ExceptionPoll(ManagedRegister mscratch, size_t stack_adjust) {
  Mips64ManagedRegister scratch = mscratch.AsMips64();
  Mips64ExceptionSlowPath* slow = new Mips64ExceptionSlowPath(scratch, stack_adjust);
  buffer_.EnqueueSlowPath(slow);
  LoadFromOffset(kLoadDoubleword, scratch.AsGpuRegister(),
                 S1, Thread::ExceptionOffset<8>().Int32Value());
  EmitBranch(scratch.AsGpuRegister(), ZERO, slow->Entry(), false);
}

void Mips64ExceptionSlowPath::Emit(Assembler* sasm) {
  Mips64Assembler* sp_asm = down_cast<Mips64Assembler*>(sasm);
#define __ sp_asm->
  __ Bind(&entry_, false);
  if (stack_adjust_ != 0) {  // Fix up the frame.
    __ DecreaseFrameSize(stack_adjust_);
  }
  // Pass exception object as argument
  // Don't care about preserving A0 as this call won't return
  __ Move(A0, scratch_.AsGpuRegister());
  // Set up call to Thread::Current()->pDeliverException
  __ LoadFromOffset(kLoadDoubleword, T9, S1,
                    QUICK_ENTRYPOINT_OFFSET(8, pDeliverException).Int32Value());
  __ Jr(T9);
  // Call never returns
  __ Break();
#undef __
}

}  // namespace mips64
}  // namespace art
