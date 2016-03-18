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

#include "assembler_mips.h"

#include "base/bit_utils.h"
#include "base/casts.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "entrypoints/quick/quick_entrypoints_enum.h"
#include "memory_region.h"
#include "thread.h"

namespace art {
namespace mips {

std::ostream& operator<<(std::ostream& os, const DRegister& rhs) {
  if (rhs >= D0 && rhs < kNumberOfDRegisters) {
    os << "d" << static_cast<int>(rhs);
  } else {
    os << "DRegister[" << static_cast<int>(rhs) << "]";
  }
  return os;
}

void MipsAssembler::FinalizeCode() {
  for (auto& exception_block : exception_blocks_) {
    EmitExceptionPoll(&exception_block);
  }
  PromoteBranches();
}

void MipsAssembler::FinalizeInstructions(const MemoryRegion& region) {
  size_t number_of_delayed_adjust_pcs = cfi().NumberOfDelayedAdvancePCs();
  EmitBranches();
  Assembler::FinalizeInstructions(region);
  PatchCFI(number_of_delayed_adjust_pcs);
}

void MipsAssembler::PatchCFI(size_t number_of_delayed_adjust_pcs) {
  if (cfi().NumberOfDelayedAdvancePCs() == 0u) {
    DCHECK_EQ(number_of_delayed_adjust_pcs, 0u);
    return;
  }

  typedef DebugFrameOpCodeWriterForAssembler::DelayedAdvancePC DelayedAdvancePC;
  const auto data = cfi().ReleaseStreamAndPrepareForDelayedAdvancePC();
  const std::vector<uint8_t>& old_stream = data.first;
  const std::vector<DelayedAdvancePC>& advances = data.second;

  // PCs recorded before EmitBranches() need to be adjusted.
  // PCs recorded during EmitBranches() are already adjusted.
  // Both ranges are separately sorted but they may overlap.
  if (kIsDebugBuild) {
    auto cmp = [](const DelayedAdvancePC& lhs, const DelayedAdvancePC& rhs) {
      return lhs.pc < rhs.pc;
    };
    CHECK(std::is_sorted(advances.begin(), advances.begin() + number_of_delayed_adjust_pcs, cmp));
    CHECK(std::is_sorted(advances.begin() + number_of_delayed_adjust_pcs, advances.end(), cmp));
  }

  // Append initial CFI data if any.
  size_t size = advances.size();
  DCHECK_NE(size, 0u);
  cfi().AppendRawData(old_stream, 0u, advances[0].stream_pos);
  // Emit PC adjustments interleaved with the old CFI stream.
  size_t adjust_pos = 0u;
  size_t late_emit_pos = number_of_delayed_adjust_pcs;
  while (adjust_pos != number_of_delayed_adjust_pcs || late_emit_pos != size) {
    size_t adjusted_pc = (adjust_pos != number_of_delayed_adjust_pcs)
        ? GetAdjustedPosition(advances[adjust_pos].pc)
        : static_cast<size_t>(-1);
    size_t late_emit_pc = (late_emit_pos != size)
        ? advances[late_emit_pos].pc
        : static_cast<size_t>(-1);
    size_t advance_pc = std::min(adjusted_pc, late_emit_pc);
    DCHECK_NE(advance_pc, static_cast<size_t>(-1));
    size_t entry = (adjusted_pc <= late_emit_pc) ? adjust_pos : late_emit_pos;
    if (adjusted_pc <= late_emit_pc) {
      ++adjust_pos;
    } else {
      ++late_emit_pos;
    }
    cfi().AdvancePC(advance_pc);
    size_t end_pos = (entry + 1u == size) ? old_stream.size() : advances[entry + 1u].stream_pos;
    cfi().AppendRawData(old_stream, advances[entry].stream_pos, end_pos);
  }
}

void MipsAssembler::EmitBranches() {
  CHECK(!overwriting_);
  // Switch from appending instructions at the end of the buffer to overwriting
  // existing instructions (branch placeholders) in the buffer.
  overwriting_ = true;
  for (auto& branch : branches_) {
    EmitBranch(&branch);
  }
  overwriting_ = false;
}

void MipsAssembler::Emit(uint32_t value) {
  if (overwriting_) {
    // Branches to labels are emitted into their placeholders here.
    buffer_.Store<uint32_t>(overwrite_location_, value);
    overwrite_location_ += sizeof(uint32_t);
  } else {
    // Other instructions are simply appended at the end here.
    AssemblerBuffer::EnsureCapacity ensured(&buffer_);
    buffer_.Emit<uint32_t>(value);
  }
}

void MipsAssembler::EmitR(int opcode, Register rs, Register rt, Register rd, int shamt, int funct) {
  CHECK_NE(rs, kNoRegister);
  CHECK_NE(rt, kNoRegister);
  CHECK_NE(rd, kNoRegister);
  uint32_t encoding = static_cast<uint32_t>(opcode) << kOpcodeShift |
                      static_cast<uint32_t>(rs) << kRsShift |
                      static_cast<uint32_t>(rt) << kRtShift |
                      static_cast<uint32_t>(rd) << kRdShift |
                      shamt << kShamtShift |
                      funct;
  Emit(encoding);
}

void MipsAssembler::EmitI(int opcode, Register rs, Register rt, uint16_t imm) {
  CHECK_NE(rs, kNoRegister);
  CHECK_NE(rt, kNoRegister);
  uint32_t encoding = static_cast<uint32_t>(opcode) << kOpcodeShift |
                      static_cast<uint32_t>(rs) << kRsShift |
                      static_cast<uint32_t>(rt) << kRtShift |
                      imm;
  Emit(encoding);
}

void MipsAssembler::EmitI21(int opcode, Register rs, uint32_t imm21) {
  CHECK_NE(rs, kNoRegister);
  CHECK(IsUint<21>(imm21)) << imm21;
  uint32_t encoding = static_cast<uint32_t>(opcode) << kOpcodeShift |
                      static_cast<uint32_t>(rs) << kRsShift |
                      imm21;
  Emit(encoding);
}

void MipsAssembler::EmitI26(int opcode, uint32_t imm26) {
  CHECK(IsUint<26>(imm26)) << imm26;
  uint32_t encoding = static_cast<uint32_t>(opcode) << kOpcodeShift | imm26;
  Emit(encoding);
}

void MipsAssembler::EmitFR(int opcode, int fmt, FRegister ft, FRegister fs, FRegister fd,
                           int funct) {
  CHECK_NE(ft, kNoFRegister);
  CHECK_NE(fs, kNoFRegister);
  CHECK_NE(fd, kNoFRegister);
  uint32_t encoding = static_cast<uint32_t>(opcode) << kOpcodeShift |
                      fmt << kFmtShift |
                      static_cast<uint32_t>(ft) << kFtShift |
                      static_cast<uint32_t>(fs) << kFsShift |
                      static_cast<uint32_t>(fd) << kFdShift |
                      funct;
  Emit(encoding);
}

void MipsAssembler::EmitFI(int opcode, int fmt, FRegister ft, uint16_t imm) {
  CHECK_NE(ft, kNoFRegister);
  uint32_t encoding = static_cast<uint32_t>(opcode) << kOpcodeShift |
                      fmt << kFmtShift |
                      static_cast<uint32_t>(ft) << kFtShift |
                      imm;
  Emit(encoding);
}

void MipsAssembler::Addu(Register rd, Register rs, Register rt) {
  EmitR(0, rs, rt, rd, 0, 0x21);
}

void MipsAssembler::Addiu(Register rt, Register rs, uint16_t imm16) {
  EmitI(0x9, rs, rt, imm16);
}

void MipsAssembler::Subu(Register rd, Register rs, Register rt) {
  EmitR(0, rs, rt, rd, 0, 0x23);
}

void MipsAssembler::MultR2(Register rs, Register rt) {
  CHECK(!IsR6());
  EmitR(0, rs, rt, static_cast<Register>(0), 0, 0x18);
}

void MipsAssembler::MultuR2(Register rs, Register rt) {
  CHECK(!IsR6());
  EmitR(0, rs, rt, static_cast<Register>(0), 0, 0x19);
}

void MipsAssembler::DivR2(Register rs, Register rt) {
  CHECK(!IsR6());
  EmitR(0, rs, rt, static_cast<Register>(0), 0, 0x1a);
}

void MipsAssembler::DivuR2(Register rs, Register rt) {
  CHECK(!IsR6());
  EmitR(0, rs, rt, static_cast<Register>(0), 0, 0x1b);
}

void MipsAssembler::MulR2(Register rd, Register rs, Register rt) {
  CHECK(!IsR6());
  EmitR(0x1c, rs, rt, rd, 0, 2);
}

void MipsAssembler::DivR2(Register rd, Register rs, Register rt) {
  CHECK(!IsR6());
  DivR2(rs, rt);
  Mflo(rd);
}

void MipsAssembler::ModR2(Register rd, Register rs, Register rt) {
  CHECK(!IsR6());
  DivR2(rs, rt);
  Mfhi(rd);
}

void MipsAssembler::DivuR2(Register rd, Register rs, Register rt) {
  CHECK(!IsR6());
  DivuR2(rs, rt);
  Mflo(rd);
}

void MipsAssembler::ModuR2(Register rd, Register rs, Register rt) {
  CHECK(!IsR6());
  DivuR2(rs, rt);
  Mfhi(rd);
}

void MipsAssembler::MulR6(Register rd, Register rs, Register rt) {
  CHECK(IsR6());
  EmitR(0, rs, rt, rd, 2, 0x18);
}

void MipsAssembler::MuhR6(Register rd, Register rs, Register rt) {
  CHECK(IsR6());
  EmitR(0, rs, rt, rd, 3, 0x18);
}

void MipsAssembler::MuhuR6(Register rd, Register rs, Register rt) {
  CHECK(IsR6());
  EmitR(0, rs, rt, rd, 3, 0x19);
}

void MipsAssembler::DivR6(Register rd, Register rs, Register rt) {
  CHECK(IsR6());
  EmitR(0, rs, rt, rd, 2, 0x1a);
}

void MipsAssembler::ModR6(Register rd, Register rs, Register rt) {
  CHECK(IsR6());
  EmitR(0, rs, rt, rd, 3, 0x1a);
}

void MipsAssembler::DivuR6(Register rd, Register rs, Register rt) {
  CHECK(IsR6());
  EmitR(0, rs, rt, rd, 2, 0x1b);
}

void MipsAssembler::ModuR6(Register rd, Register rs, Register rt) {
  CHECK(IsR6());
  EmitR(0, rs, rt, rd, 3, 0x1b);
}

void MipsAssembler::And(Register rd, Register rs, Register rt) {
  EmitR(0, rs, rt, rd, 0, 0x24);
}

void MipsAssembler::Andi(Register rt, Register rs, uint16_t imm16) {
  EmitI(0xc, rs, rt, imm16);
}

void MipsAssembler::Or(Register rd, Register rs, Register rt) {
  EmitR(0, rs, rt, rd, 0, 0x25);
}

void MipsAssembler::Ori(Register rt, Register rs, uint16_t imm16) {
  EmitI(0xd, rs, rt, imm16);
}

void MipsAssembler::Xor(Register rd, Register rs, Register rt) {
  EmitR(0, rs, rt, rd, 0, 0x26);
}

void MipsAssembler::Xori(Register rt, Register rs, uint16_t imm16) {
  EmitI(0xe, rs, rt, imm16);
}

void MipsAssembler::Nor(Register rd, Register rs, Register rt) {
  EmitR(0, rs, rt, rd, 0, 0x27);
}

void MipsAssembler::Movz(Register rd, Register rs, Register rt) {
  CHECK(!IsR6());
  EmitR(0, rs, rt, rd, 0, 0x0A);
}

void MipsAssembler::Movn(Register rd, Register rs, Register rt) {
  CHECK(!IsR6());
  EmitR(0, rs, rt, rd, 0, 0x0B);
}

void MipsAssembler::Seleqz(Register rd, Register rs, Register rt) {
  CHECK(IsR6());
  EmitR(0, rs, rt, rd, 0, 0x35);
}

void MipsAssembler::Selnez(Register rd, Register rs, Register rt) {
  CHECK(IsR6());
  EmitR(0, rs, rt, rd, 0, 0x37);
}

void MipsAssembler::ClzR6(Register rd, Register rs) {
  CHECK(IsR6());
  EmitR(0, rs, static_cast<Register>(0), rd, 0x01, 0x10);
}

void MipsAssembler::ClzR2(Register rd, Register rs) {
  CHECK(!IsR6());
  EmitR(0x1C, rs, rd, rd, 0, 0x20);
}

void MipsAssembler::CloR6(Register rd, Register rs) {
  CHECK(IsR6());
  EmitR(0, rs, static_cast<Register>(0), rd, 0x01, 0x11);
}

void MipsAssembler::CloR2(Register rd, Register rs) {
  CHECK(!IsR6());
  EmitR(0x1C, rs, rd, rd, 0, 0x21);
}

void MipsAssembler::Seb(Register rd, Register rt) {
  EmitR(0x1f, static_cast<Register>(0), rt, rd, 0x10, 0x20);
}

void MipsAssembler::Seh(Register rd, Register rt) {
  EmitR(0x1f, static_cast<Register>(0), rt, rd, 0x18, 0x20);
}

void MipsAssembler::Wsbh(Register rd, Register rt) {
  EmitR(0x1f, static_cast<Register>(0), rt, rd, 2, 0x20);
}

void MipsAssembler::Bitswap(Register rd, Register rt) {
  CHECK(IsR6());
  EmitR(0x1f, static_cast<Register>(0), rt, rd, 0x0, 0x20);
}

void MipsAssembler::Sll(Register rd, Register rt, int shamt) {
  CHECK(IsUint<5>(shamt)) << shamt;
  EmitR(0, static_cast<Register>(0), rt, rd, shamt, 0x00);
}

void MipsAssembler::Srl(Register rd, Register rt, int shamt) {
  CHECK(IsUint<5>(shamt)) << shamt;
  EmitR(0, static_cast<Register>(0), rt, rd, shamt, 0x02);
}

void MipsAssembler::Rotr(Register rd, Register rt, int shamt) {
  CHECK(IsUint<5>(shamt)) << shamt;
  EmitR(0, static_cast<Register>(1), rt, rd, shamt, 0x02);
}

void MipsAssembler::Sra(Register rd, Register rt, int shamt) {
  CHECK(IsUint<5>(shamt)) << shamt;
  EmitR(0, static_cast<Register>(0), rt, rd, shamt, 0x03);
}

void MipsAssembler::Sllv(Register rd, Register rt, Register rs) {
  EmitR(0, rs, rt, rd, 0, 0x04);
}

void MipsAssembler::Srlv(Register rd, Register rt, Register rs) {
  EmitR(0, rs, rt, rd, 0, 0x06);
}

void MipsAssembler::Rotrv(Register rd, Register rt, Register rs) {
  EmitR(0, rs, rt, rd, 1, 0x06);
}

void MipsAssembler::Srav(Register rd, Register rt, Register rs) {
  EmitR(0, rs, rt, rd, 0, 0x07);
}

void MipsAssembler::Ext(Register rd, Register rt, int pos, int size) {
  CHECK(IsUint<5>(pos)) << pos;
  CHECK(0 < size && size <= 32) << size;
  CHECK(0 < pos + size && pos + size <= 32) << pos << " + " << size;
  EmitR(0x1f, rt, rd, static_cast<Register>(size - 1), pos, 0x00);
}

void MipsAssembler::Ins(Register rd, Register rt, int pos, int size) {
  CHECK(IsUint<5>(pos)) << pos;
  CHECK(0 < size && size <= 32) << size;
  CHECK(0 < pos + size && pos + size <= 32) << pos << " + " << size;
  EmitR(0x1f, rt, rd, static_cast<Register>(pos + size - 1), pos, 0x04);
}

void MipsAssembler::Lb(Register rt, Register rs, uint16_t imm16) {
  EmitI(0x20, rs, rt, imm16);
}

void MipsAssembler::Lh(Register rt, Register rs, uint16_t imm16) {
  EmitI(0x21, rs, rt, imm16);
}

void MipsAssembler::Lw(Register rt, Register rs, uint16_t imm16) {
  EmitI(0x23, rs, rt, imm16);
}

void MipsAssembler::Lwl(Register rt, Register rs, uint16_t imm16) {
  CHECK(!IsR6());
  EmitI(0x22, rs, rt, imm16);
}

void MipsAssembler::Lwr(Register rt, Register rs, uint16_t imm16) {
  CHECK(!IsR6());
  EmitI(0x26, rs, rt, imm16);
}

void MipsAssembler::Lbu(Register rt, Register rs, uint16_t imm16) {
  EmitI(0x24, rs, rt, imm16);
}

void MipsAssembler::Lhu(Register rt, Register rs, uint16_t imm16) {
  EmitI(0x25, rs, rt, imm16);
}

void MipsAssembler::Lui(Register rt, uint16_t imm16) {
  EmitI(0xf, static_cast<Register>(0), rt, imm16);
}

void MipsAssembler::Sync(uint32_t stype) {
  EmitR(0, static_cast<Register>(0), static_cast<Register>(0), static_cast<Register>(0),
        stype & 0x1f, 0xf);
}

void MipsAssembler::Mfhi(Register rd) {
  CHECK(!IsR6());
  EmitR(0, static_cast<Register>(0), static_cast<Register>(0), rd, 0, 0x10);
}

void MipsAssembler::Mflo(Register rd) {
  CHECK(!IsR6());
  EmitR(0, static_cast<Register>(0), static_cast<Register>(0), rd, 0, 0x12);
}

void MipsAssembler::Sb(Register rt, Register rs, uint16_t imm16) {
  EmitI(0x28, rs, rt, imm16);
}

void MipsAssembler::Sh(Register rt, Register rs, uint16_t imm16) {
  EmitI(0x29, rs, rt, imm16);
}

void MipsAssembler::Sw(Register rt, Register rs, uint16_t imm16) {
  EmitI(0x2b, rs, rt, imm16);
}

void MipsAssembler::Swl(Register rt, Register rs, uint16_t imm16) {
  CHECK(!IsR6());
  EmitI(0x2a, rs, rt, imm16);
}

void MipsAssembler::Swr(Register rt, Register rs, uint16_t imm16) {
  CHECK(!IsR6());
  EmitI(0x2e, rs, rt, imm16);
}

void MipsAssembler::LlR2(Register rt, Register base, int16_t imm16) {
  CHECK(!IsR6());
  EmitI(0x30, base, rt, imm16);
}

void MipsAssembler::ScR2(Register rt, Register base, int16_t imm16) {
  CHECK(!IsR6());
  EmitI(0x38, base, rt, imm16);
}

void MipsAssembler::LlR6(Register rt, Register base, int16_t imm9) {
  CHECK(IsR6());
  CHECK(IsInt<9>(imm9));
  EmitI(0x1f, base, rt, ((imm9 & 0x1ff) << 7) | 0x36);
}

void MipsAssembler::ScR6(Register rt, Register base, int16_t imm9) {
  CHECK(IsR6());
  CHECK(IsInt<9>(imm9));
  EmitI(0x1f, base, rt, ((imm9 & 0x1ff) << 7) | 0x26);
}

void MipsAssembler::Slt(Register rd, Register rs, Register rt) {
  EmitR(0, rs, rt, rd, 0, 0x2a);
}

void MipsAssembler::Sltu(Register rd, Register rs, Register rt) {
  EmitR(0, rs, rt, rd, 0, 0x2b);
}

void MipsAssembler::Slti(Register rt, Register rs, uint16_t imm16) {
  EmitI(0xa, rs, rt, imm16);
}

void MipsAssembler::Sltiu(Register rt, Register rs, uint16_t imm16) {
  EmitI(0xb, rs, rt, imm16);
}

void MipsAssembler::B(uint16_t imm16) {
  EmitI(0x4, static_cast<Register>(0), static_cast<Register>(0), imm16);
}

void MipsAssembler::Beq(Register rs, Register rt, uint16_t imm16) {
  EmitI(0x4, rs, rt, imm16);
}

void MipsAssembler::Bne(Register rs, Register rt, uint16_t imm16) {
  EmitI(0x5, rs, rt, imm16);
}

void MipsAssembler::Beqz(Register rt, uint16_t imm16) {
  Beq(ZERO, rt, imm16);
}

void MipsAssembler::Bnez(Register rt, uint16_t imm16) {
  Bne(ZERO, rt, imm16);
}

void MipsAssembler::Bltz(Register rt, uint16_t imm16) {
  EmitI(0x1, rt, static_cast<Register>(0), imm16);
}

void MipsAssembler::Bgez(Register rt, uint16_t imm16) {
  EmitI(0x1, rt, static_cast<Register>(0x1), imm16);
}

void MipsAssembler::Blez(Register rt, uint16_t imm16) {
  EmitI(0x6, rt, static_cast<Register>(0), imm16);
}

void MipsAssembler::Bgtz(Register rt, uint16_t imm16) {
  EmitI(0x7, rt, static_cast<Register>(0), imm16);
}

void MipsAssembler::Bc1f(uint16_t imm16) {
  Bc1f(0, imm16);
}

void MipsAssembler::Bc1f(int cc, uint16_t imm16) {
  CHECK(!IsR6());
  CHECK(IsUint<3>(cc)) << cc;
  EmitI(0x11, static_cast<Register>(0x8), static_cast<Register>(cc << 2), imm16);
}

void MipsAssembler::Bc1t(uint16_t imm16) {
  Bc1t(0, imm16);
}

void MipsAssembler::Bc1t(int cc, uint16_t imm16) {
  CHECK(!IsR6());
  CHECK(IsUint<3>(cc)) << cc;
  EmitI(0x11, static_cast<Register>(0x8), static_cast<Register>((cc << 2) | 1), imm16);
}

void MipsAssembler::J(uint32_t addr26) {
  EmitI26(0x2, addr26);
}

void MipsAssembler::Jal(uint32_t addr26) {
  EmitI26(0x3, addr26);
}

void MipsAssembler::Jalr(Register rd, Register rs) {
  EmitR(0, rs, static_cast<Register>(0), rd, 0, 0x09);
}

void MipsAssembler::Jalr(Register rs) {
  Jalr(RA, rs);
}

void MipsAssembler::Jr(Register rs) {
  Jalr(ZERO, rs);
}

void MipsAssembler::Nal() {
  EmitI(0x1, static_cast<Register>(0), static_cast<Register>(0x10), 0);
}

void MipsAssembler::Auipc(Register rs, uint16_t imm16) {
  CHECK(IsR6());
  EmitI(0x3B, rs, static_cast<Register>(0x1E), imm16);
}

void MipsAssembler::Addiupc(Register rs, uint32_t imm19) {
  CHECK(IsR6());
  CHECK(IsUint<19>(imm19)) << imm19;
  EmitI21(0x3B, rs, imm19);
}

void MipsAssembler::Bc(uint32_t imm26) {
  CHECK(IsR6());
  EmitI26(0x32, imm26);
}

void MipsAssembler::Jic(Register rt, uint16_t imm16) {
  CHECK(IsR6());
  EmitI(0x36, static_cast<Register>(0), rt, imm16);
}

void MipsAssembler::Jialc(Register rt, uint16_t imm16) {
  CHECK(IsR6());
  EmitI(0x3E, static_cast<Register>(0), rt, imm16);
}

void MipsAssembler::Bltc(Register rs, Register rt, uint16_t imm16) {
  CHECK(IsR6());
  CHECK_NE(rs, ZERO);
  CHECK_NE(rt, ZERO);
  CHECK_NE(rs, rt);
  EmitI(0x17, rs, rt, imm16);
}

void MipsAssembler::Bltzc(Register rt, uint16_t imm16) {
  CHECK(IsR6());
  CHECK_NE(rt, ZERO);
  EmitI(0x17, rt, rt, imm16);
}

void MipsAssembler::Bgtzc(Register rt, uint16_t imm16) {
  CHECK(IsR6());
  CHECK_NE(rt, ZERO);
  EmitI(0x17, static_cast<Register>(0), rt, imm16);
}

void MipsAssembler::Bgec(Register rs, Register rt, uint16_t imm16) {
  CHECK(IsR6());
  CHECK_NE(rs, ZERO);
  CHECK_NE(rt, ZERO);
  CHECK_NE(rs, rt);
  EmitI(0x16, rs, rt, imm16);
}

void MipsAssembler::Bgezc(Register rt, uint16_t imm16) {
  CHECK(IsR6());
  CHECK_NE(rt, ZERO);
  EmitI(0x16, rt, rt, imm16);
}

void MipsAssembler::Blezc(Register rt, uint16_t imm16) {
  CHECK(IsR6());
  CHECK_NE(rt, ZERO);
  EmitI(0x16, static_cast<Register>(0), rt, imm16);
}

void MipsAssembler::Bltuc(Register rs, Register rt, uint16_t imm16) {
  CHECK(IsR6());
  CHECK_NE(rs, ZERO);
  CHECK_NE(rt, ZERO);
  CHECK_NE(rs, rt);
  EmitI(0x7, rs, rt, imm16);
}

void MipsAssembler::Bgeuc(Register rs, Register rt, uint16_t imm16) {
  CHECK(IsR6());
  CHECK_NE(rs, ZERO);
  CHECK_NE(rt, ZERO);
  CHECK_NE(rs, rt);
  EmitI(0x6, rs, rt, imm16);
}

void MipsAssembler::Beqc(Register rs, Register rt, uint16_t imm16) {
  CHECK(IsR6());
  CHECK_NE(rs, ZERO);
  CHECK_NE(rt, ZERO);
  CHECK_NE(rs, rt);
  EmitI(0x8, std::min(rs, rt), std::max(rs, rt), imm16);
}

void MipsAssembler::Bnec(Register rs, Register rt, uint16_t imm16) {
  CHECK(IsR6());
  CHECK_NE(rs, ZERO);
  CHECK_NE(rt, ZERO);
  CHECK_NE(rs, rt);
  EmitI(0x18, std::min(rs, rt), std::max(rs, rt), imm16);
}

void MipsAssembler::Beqzc(Register rs, uint32_t imm21) {
  CHECK(IsR6());
  CHECK_NE(rs, ZERO);
  EmitI21(0x36, rs, imm21);
}

void MipsAssembler::Bnezc(Register rs, uint32_t imm21) {
  CHECK(IsR6());
  CHECK_NE(rs, ZERO);
  EmitI21(0x3E, rs, imm21);
}

void MipsAssembler::Bc1eqz(FRegister ft, uint16_t imm16) {
  CHECK(IsR6());
  EmitFI(0x11, 0x9, ft, imm16);
}

void MipsAssembler::Bc1nez(FRegister ft, uint16_t imm16) {
  CHECK(IsR6());
  EmitFI(0x11, 0xD, ft, imm16);
}

void MipsAssembler::EmitBcondR2(BranchCondition cond, Register rs, Register rt, uint16_t imm16) {
  switch (cond) {
    case kCondLTZ:
      CHECK_EQ(rt, ZERO);
      Bltz(rs, imm16);
      break;
    case kCondGEZ:
      CHECK_EQ(rt, ZERO);
      Bgez(rs, imm16);
      break;
    case kCondLEZ:
      CHECK_EQ(rt, ZERO);
      Blez(rs, imm16);
      break;
    case kCondGTZ:
      CHECK_EQ(rt, ZERO);
      Bgtz(rs, imm16);
      break;
    case kCondEQ:
      Beq(rs, rt, imm16);
      break;
    case kCondNE:
      Bne(rs, rt, imm16);
      break;
    case kCondEQZ:
      CHECK_EQ(rt, ZERO);
      Beqz(rs, imm16);
      break;
    case kCondNEZ:
      CHECK_EQ(rt, ZERO);
      Bnez(rs, imm16);
      break;
    case kCondF:
      CHECK_EQ(rt, ZERO);
      Bc1f(static_cast<int>(rs), imm16);
      break;
    case kCondT:
      CHECK_EQ(rt, ZERO);
      Bc1t(static_cast<int>(rs), imm16);
      break;
    case kCondLT:
    case kCondGE:
    case kCondLE:
    case kCondGT:
    case kCondLTU:
    case kCondGEU:
    case kUncond:
      // We don't support synthetic R2 branches (preceded with slt[u]) at this level
      // (R2 doesn't have branches to compare 2 registers using <, <=, >=, >).
      LOG(FATAL) << "Unexpected branch condition " << cond;
      UNREACHABLE();
  }
}

void MipsAssembler::EmitBcondR6(BranchCondition cond, Register rs, Register rt, uint32_t imm16_21) {
  switch (cond) {
    case kCondLT:
      Bltc(rs, rt, imm16_21);
      break;
    case kCondGE:
      Bgec(rs, rt, imm16_21);
      break;
    case kCondLE:
      Bgec(rt, rs, imm16_21);
      break;
    case kCondGT:
      Bltc(rt, rs, imm16_21);
      break;
    case kCondLTZ:
      CHECK_EQ(rt, ZERO);
      Bltzc(rs, imm16_21);
      break;
    case kCondGEZ:
      CHECK_EQ(rt, ZERO);
      Bgezc(rs, imm16_21);
      break;
    case kCondLEZ:
      CHECK_EQ(rt, ZERO);
      Blezc(rs, imm16_21);
      break;
    case kCondGTZ:
      CHECK_EQ(rt, ZERO);
      Bgtzc(rs, imm16_21);
      break;
    case kCondEQ:
      Beqc(rs, rt, imm16_21);
      break;
    case kCondNE:
      Bnec(rs, rt, imm16_21);
      break;
    case kCondEQZ:
      CHECK_EQ(rt, ZERO);
      Beqzc(rs, imm16_21);
      break;
    case kCondNEZ:
      CHECK_EQ(rt, ZERO);
      Bnezc(rs, imm16_21);
      break;
    case kCondLTU:
      Bltuc(rs, rt, imm16_21);
      break;
    case kCondGEU:
      Bgeuc(rs, rt, imm16_21);
      break;
    case kCondF:
      CHECK_EQ(rt, ZERO);
      Bc1eqz(static_cast<FRegister>(rs), imm16_21);
      break;
    case kCondT:
      CHECK_EQ(rt, ZERO);
      Bc1nez(static_cast<FRegister>(rs), imm16_21);
      break;
    case kUncond:
      LOG(FATAL) << "Unexpected branch condition " << cond;
      UNREACHABLE();
  }
}

void MipsAssembler::AddS(FRegister fd, FRegister fs, FRegister ft) {
  EmitFR(0x11, 0x10, ft, fs, fd, 0x0);
}

void MipsAssembler::SubS(FRegister fd, FRegister fs, FRegister ft) {
  EmitFR(0x11, 0x10, ft, fs, fd, 0x1);
}

void MipsAssembler::MulS(FRegister fd, FRegister fs, FRegister ft) {
  EmitFR(0x11, 0x10, ft, fs, fd, 0x2);
}

void MipsAssembler::DivS(FRegister fd, FRegister fs, FRegister ft) {
  EmitFR(0x11, 0x10, ft, fs, fd, 0x3);
}

void MipsAssembler::AddD(FRegister fd, FRegister fs, FRegister ft) {
  EmitFR(0x11, 0x11, ft, fs, fd, 0x0);
}

void MipsAssembler::SubD(FRegister fd, FRegister fs, FRegister ft) {
  EmitFR(0x11, 0x11, ft, fs, fd, 0x1);
}

void MipsAssembler::MulD(FRegister fd, FRegister fs, FRegister ft) {
  EmitFR(0x11, 0x11, ft, fs, fd, 0x2);
}

void MipsAssembler::DivD(FRegister fd, FRegister fs, FRegister ft) {
  EmitFR(0x11, 0x11, ft, fs, fd, 0x3);
}

void MipsAssembler::SqrtS(FRegister fd, FRegister fs) {
  EmitFR(0x11, 0x10, static_cast<FRegister>(0), fs, fd, 0x4);
}

void MipsAssembler::SqrtD(FRegister fd, FRegister fs) {
  EmitFR(0x11, 0x11, static_cast<FRegister>(0), fs, fd, 0x4);
}

void MipsAssembler::AbsS(FRegister fd, FRegister fs) {
  EmitFR(0x11, 0x10, static_cast<FRegister>(0), fs, fd, 0x5);
}

void MipsAssembler::AbsD(FRegister fd, FRegister fs) {
  EmitFR(0x11, 0x11, static_cast<FRegister>(0), fs, fd, 0x5);
}

void MipsAssembler::MovS(FRegister fd, FRegister fs) {
  EmitFR(0x11, 0x10, static_cast<FRegister>(0), fs, fd, 0x6);
}

void MipsAssembler::MovD(FRegister fd, FRegister fs) {
  EmitFR(0x11, 0x11, static_cast<FRegister>(0), fs, fd, 0x6);
}

void MipsAssembler::NegS(FRegister fd, FRegister fs) {
  EmitFR(0x11, 0x10, static_cast<FRegister>(0), fs, fd, 0x7);
}

void MipsAssembler::NegD(FRegister fd, FRegister fs) {
  EmitFR(0x11, 0x11, static_cast<FRegister>(0), fs, fd, 0x7);
}

void MipsAssembler::CunS(FRegister fs, FRegister ft) {
  CunS(0, fs, ft);
}

void MipsAssembler::CunS(int cc, FRegister fs, FRegister ft) {
  CHECK(!IsR6());
  CHECK(IsUint<3>(cc)) << cc;
  EmitFR(0x11, 0x10, ft, fs, static_cast<FRegister>(cc << 2), 0x31);
}

void MipsAssembler::CeqS(FRegister fs, FRegister ft) {
  CeqS(0, fs, ft);
}

void MipsAssembler::CeqS(int cc, FRegister fs, FRegister ft) {
  CHECK(!IsR6());
  CHECK(IsUint<3>(cc)) << cc;
  EmitFR(0x11, 0x10, ft, fs, static_cast<FRegister>(cc << 2), 0x32);
}

void MipsAssembler::CueqS(FRegister fs, FRegister ft) {
  CueqS(0, fs, ft);
}

void MipsAssembler::CueqS(int cc, FRegister fs, FRegister ft) {
  CHECK(!IsR6());
  CHECK(IsUint<3>(cc)) << cc;
  EmitFR(0x11, 0x10, ft, fs, static_cast<FRegister>(cc << 2), 0x33);
}

void MipsAssembler::ColtS(FRegister fs, FRegister ft) {
  ColtS(0, fs, ft);
}

void MipsAssembler::ColtS(int cc, FRegister fs, FRegister ft) {
  CHECK(!IsR6());
  CHECK(IsUint<3>(cc)) << cc;
  EmitFR(0x11, 0x10, ft, fs, static_cast<FRegister>(cc << 2), 0x34);
}

void MipsAssembler::CultS(FRegister fs, FRegister ft) {
  CultS(0, fs, ft);
}

void MipsAssembler::CultS(int cc, FRegister fs, FRegister ft) {
  CHECK(!IsR6());
  CHECK(IsUint<3>(cc)) << cc;
  EmitFR(0x11, 0x10, ft, fs, static_cast<FRegister>(cc << 2), 0x35);
}

void MipsAssembler::ColeS(FRegister fs, FRegister ft) {
  ColeS(0, fs, ft);
}

void MipsAssembler::ColeS(int cc, FRegister fs, FRegister ft) {
  CHECK(!IsR6());
  CHECK(IsUint<3>(cc)) << cc;
  EmitFR(0x11, 0x10, ft, fs, static_cast<FRegister>(cc << 2), 0x36);
}

void MipsAssembler::CuleS(FRegister fs, FRegister ft) {
  CuleS(0, fs, ft);
}

void MipsAssembler::CuleS(int cc, FRegister fs, FRegister ft) {
  CHECK(!IsR6());
  CHECK(IsUint<3>(cc)) << cc;
  EmitFR(0x11, 0x10, ft, fs, static_cast<FRegister>(cc << 2), 0x37);
}

void MipsAssembler::CunD(FRegister fs, FRegister ft) {
  CunD(0, fs, ft);
}

void MipsAssembler::CunD(int cc, FRegister fs, FRegister ft) {
  CHECK(!IsR6());
  CHECK(IsUint<3>(cc)) << cc;
  EmitFR(0x11, 0x11, ft, fs, static_cast<FRegister>(cc << 2), 0x31);
}

void MipsAssembler::CeqD(FRegister fs, FRegister ft) {
  CeqD(0, fs, ft);
}

void MipsAssembler::CeqD(int cc, FRegister fs, FRegister ft) {
  CHECK(!IsR6());
  CHECK(IsUint<3>(cc)) << cc;
  EmitFR(0x11, 0x11, ft, fs, static_cast<FRegister>(cc << 2), 0x32);
}

void MipsAssembler::CueqD(FRegister fs, FRegister ft) {
  CueqD(0, fs, ft);
}

void MipsAssembler::CueqD(int cc, FRegister fs, FRegister ft) {
  CHECK(!IsR6());
  CHECK(IsUint<3>(cc)) << cc;
  EmitFR(0x11, 0x11, ft, fs, static_cast<FRegister>(cc << 2), 0x33);
}

void MipsAssembler::ColtD(FRegister fs, FRegister ft) {
  ColtD(0, fs, ft);
}

void MipsAssembler::ColtD(int cc, FRegister fs, FRegister ft) {
  CHECK(!IsR6());
  CHECK(IsUint<3>(cc)) << cc;
  EmitFR(0x11, 0x11, ft, fs, static_cast<FRegister>(cc << 2), 0x34);
}

void MipsAssembler::CultD(FRegister fs, FRegister ft) {
  CultD(0, fs, ft);
}

void MipsAssembler::CultD(int cc, FRegister fs, FRegister ft) {
  CHECK(!IsR6());
  CHECK(IsUint<3>(cc)) << cc;
  EmitFR(0x11, 0x11, ft, fs, static_cast<FRegister>(cc << 2), 0x35);
}

void MipsAssembler::ColeD(FRegister fs, FRegister ft) {
  ColeD(0, fs, ft);
}

void MipsAssembler::ColeD(int cc, FRegister fs, FRegister ft) {
  CHECK(!IsR6());
  CHECK(IsUint<3>(cc)) << cc;
  EmitFR(0x11, 0x11, ft, fs, static_cast<FRegister>(cc << 2), 0x36);
}

void MipsAssembler::CuleD(FRegister fs, FRegister ft) {
  CuleD(0, fs, ft);
}

void MipsAssembler::CuleD(int cc, FRegister fs, FRegister ft) {
  CHECK(!IsR6());
  CHECK(IsUint<3>(cc)) << cc;
  EmitFR(0x11, 0x11, ft, fs, static_cast<FRegister>(cc << 2), 0x37);
}

void MipsAssembler::CmpUnS(FRegister fd, FRegister fs, FRegister ft) {
  CHECK(IsR6());
  EmitFR(0x11, 0x14, ft, fs, fd, 0x01);
}

void MipsAssembler::CmpEqS(FRegister fd, FRegister fs, FRegister ft) {
  CHECK(IsR6());
  EmitFR(0x11, 0x14, ft, fs, fd, 0x02);
}

void MipsAssembler::CmpUeqS(FRegister fd, FRegister fs, FRegister ft) {
  CHECK(IsR6());
  EmitFR(0x11, 0x14, ft, fs, fd, 0x03);
}

void MipsAssembler::CmpLtS(FRegister fd, FRegister fs, FRegister ft) {
  CHECK(IsR6());
  EmitFR(0x11, 0x14, ft, fs, fd, 0x04);
}

void MipsAssembler::CmpUltS(FRegister fd, FRegister fs, FRegister ft) {
  CHECK(IsR6());
  EmitFR(0x11, 0x14, ft, fs, fd, 0x05);
}

void MipsAssembler::CmpLeS(FRegister fd, FRegister fs, FRegister ft) {
  CHECK(IsR6());
  EmitFR(0x11, 0x14, ft, fs, fd, 0x06);
}

void MipsAssembler::CmpUleS(FRegister fd, FRegister fs, FRegister ft) {
  CHECK(IsR6());
  EmitFR(0x11, 0x14, ft, fs, fd, 0x07);
}

void MipsAssembler::CmpOrS(FRegister fd, FRegister fs, FRegister ft) {
  CHECK(IsR6());
  EmitFR(0x11, 0x14, ft, fs, fd, 0x11);
}

void MipsAssembler::CmpUneS(FRegister fd, FRegister fs, FRegister ft) {
  CHECK(IsR6());
  EmitFR(0x11, 0x14, ft, fs, fd, 0x12);
}

void MipsAssembler::CmpNeS(FRegister fd, FRegister fs, FRegister ft) {
  CHECK(IsR6());
  EmitFR(0x11, 0x14, ft, fs, fd, 0x13);
}

void MipsAssembler::CmpUnD(FRegister fd, FRegister fs, FRegister ft) {
  CHECK(IsR6());
  EmitFR(0x11, 0x15, ft, fs, fd, 0x01);
}

void MipsAssembler::CmpEqD(FRegister fd, FRegister fs, FRegister ft) {
  CHECK(IsR6());
  EmitFR(0x11, 0x15, ft, fs, fd, 0x02);
}

void MipsAssembler::CmpUeqD(FRegister fd, FRegister fs, FRegister ft) {
  CHECK(IsR6());
  EmitFR(0x11, 0x15, ft, fs, fd, 0x03);
}

void MipsAssembler::CmpLtD(FRegister fd, FRegister fs, FRegister ft) {
  CHECK(IsR6());
  EmitFR(0x11, 0x15, ft, fs, fd, 0x04);
}

void MipsAssembler::CmpUltD(FRegister fd, FRegister fs, FRegister ft) {
  CHECK(IsR6());
  EmitFR(0x11, 0x15, ft, fs, fd, 0x05);
}

void MipsAssembler::CmpLeD(FRegister fd, FRegister fs, FRegister ft) {
  CHECK(IsR6());
  EmitFR(0x11, 0x15, ft, fs, fd, 0x06);
}

void MipsAssembler::CmpUleD(FRegister fd, FRegister fs, FRegister ft) {
  CHECK(IsR6());
  EmitFR(0x11, 0x15, ft, fs, fd, 0x07);
}

void MipsAssembler::CmpOrD(FRegister fd, FRegister fs, FRegister ft) {
  CHECK(IsR6());
  EmitFR(0x11, 0x15, ft, fs, fd, 0x11);
}

void MipsAssembler::CmpUneD(FRegister fd, FRegister fs, FRegister ft) {
  CHECK(IsR6());
  EmitFR(0x11, 0x15, ft, fs, fd, 0x12);
}

void MipsAssembler::CmpNeD(FRegister fd, FRegister fs, FRegister ft) {
  CHECK(IsR6());
  EmitFR(0x11, 0x15, ft, fs, fd, 0x13);
}

void MipsAssembler::Movf(Register rd, Register rs, int cc) {
  CHECK(!IsR6());
  CHECK(IsUint<3>(cc)) << cc;
  EmitR(0, rs, static_cast<Register>(cc << 2), rd, 0, 0x01);
}

void MipsAssembler::Movt(Register rd, Register rs, int cc) {
  CHECK(!IsR6());
  CHECK(IsUint<3>(cc)) << cc;
  EmitR(0, rs, static_cast<Register>((cc << 2) | 1), rd, 0, 0x01);
}

void MipsAssembler::MovfS(FRegister fd, FRegister fs, int cc) {
  CHECK(!IsR6());
  CHECK(IsUint<3>(cc)) << cc;
  EmitFR(0x11, 0x10, static_cast<FRegister>(cc << 2), fs, fd, 0x11);
}

void MipsAssembler::MovfD(FRegister fd, FRegister fs, int cc) {
  CHECK(!IsR6());
  CHECK(IsUint<3>(cc)) << cc;
  EmitFR(0x11, 0x11, static_cast<FRegister>(cc << 2), fs, fd, 0x11);
}

void MipsAssembler::MovtS(FRegister fd, FRegister fs, int cc) {
  CHECK(!IsR6());
  CHECK(IsUint<3>(cc)) << cc;
  EmitFR(0x11, 0x10, static_cast<FRegister>((cc << 2) | 1), fs, fd, 0x11);
}

void MipsAssembler::MovtD(FRegister fd, FRegister fs, int cc) {
  CHECK(!IsR6());
  CHECK(IsUint<3>(cc)) << cc;
  EmitFR(0x11, 0x11, static_cast<FRegister>((cc << 2) | 1), fs, fd, 0x11);
}

void MipsAssembler::SelS(FRegister fd, FRegister fs, FRegister ft) {
  CHECK(IsR6());
  EmitFR(0x11, 0x10, ft, fs, fd, 0x10);
}

void MipsAssembler::SelD(FRegister fd, FRegister fs, FRegister ft) {
  CHECK(IsR6());
  EmitFR(0x11, 0x11, ft, fs, fd, 0x10);
}

void MipsAssembler::ClassS(FRegister fd, FRegister fs) {
  CHECK(IsR6());
  EmitFR(0x11, 0x10, static_cast<FRegister>(0), fs, fd, 0x1b);
}

void MipsAssembler::ClassD(FRegister fd, FRegister fs) {
  CHECK(IsR6());
  EmitFR(0x11, 0x11, static_cast<FRegister>(0), fs, fd, 0x1b);
}

void MipsAssembler::MinS(FRegister fd, FRegister fs, FRegister ft) {
  CHECK(IsR6());
  EmitFR(0x11, 0x10, ft, fs, fd, 0x1c);
}

void MipsAssembler::MinD(FRegister fd, FRegister fs, FRegister ft) {
  CHECK(IsR6());
  EmitFR(0x11, 0x11, ft, fs, fd, 0x1c);
}

void MipsAssembler::MaxS(FRegister fd, FRegister fs, FRegister ft) {
  CHECK(IsR6());
  EmitFR(0x11, 0x10, ft, fs, fd, 0x1e);
}

void MipsAssembler::MaxD(FRegister fd, FRegister fs, FRegister ft) {
  CHECK(IsR6());
  EmitFR(0x11, 0x11, ft, fs, fd, 0x1e);
}

void MipsAssembler::TruncLS(FRegister fd, FRegister fs) {
  EmitFR(0x11, 0x10, static_cast<FRegister>(0), fs, fd, 0x09);
}

void MipsAssembler::TruncLD(FRegister fd, FRegister fs) {
  EmitFR(0x11, 0x11, static_cast<FRegister>(0), fs, fd, 0x09);
}

void MipsAssembler::TruncWS(FRegister fd, FRegister fs) {
  EmitFR(0x11, 0x10, static_cast<FRegister>(0), fs, fd, 0x0D);
}

void MipsAssembler::TruncWD(FRegister fd, FRegister fs) {
  EmitFR(0x11, 0x11, static_cast<FRegister>(0), fs, fd, 0x0D);
}

void MipsAssembler::Cvtsw(FRegister fd, FRegister fs) {
  EmitFR(0x11, 0x14, static_cast<FRegister>(0), fs, fd, 0x20);
}

void MipsAssembler::Cvtdw(FRegister fd, FRegister fs) {
  EmitFR(0x11, 0x14, static_cast<FRegister>(0), fs, fd, 0x21);
}

void MipsAssembler::Cvtsd(FRegister fd, FRegister fs) {
  EmitFR(0x11, 0x11, static_cast<FRegister>(0), fs, fd, 0x20);
}

void MipsAssembler::Cvtds(FRegister fd, FRegister fs) {
  EmitFR(0x11, 0x10, static_cast<FRegister>(0), fs, fd, 0x21);
}

void MipsAssembler::Cvtsl(FRegister fd, FRegister fs) {
  EmitFR(0x11, 0x15, static_cast<FRegister>(0), fs, fd, 0x20);
}

void MipsAssembler::Cvtdl(FRegister fd, FRegister fs) {
  EmitFR(0x11, 0x15, static_cast<FRegister>(0), fs, fd, 0x21);
}

void MipsAssembler::FloorWS(FRegister fd, FRegister fs) {
  EmitFR(0x11, 0x10, static_cast<FRegister>(0), fs, fd, 0xf);
}

void MipsAssembler::FloorWD(FRegister fd, FRegister fs) {
  EmitFR(0x11, 0x11, static_cast<FRegister>(0), fs, fd, 0xf);
}

void MipsAssembler::Mfc1(Register rt, FRegister fs) {
  EmitFR(0x11, 0x00, static_cast<FRegister>(rt), fs, static_cast<FRegister>(0), 0x0);
}

void MipsAssembler::Mtc1(Register rt, FRegister fs) {
  EmitFR(0x11, 0x04, static_cast<FRegister>(rt), fs, static_cast<FRegister>(0), 0x0);
}

void MipsAssembler::Mfhc1(Register rt, FRegister fs) {
  EmitFR(0x11, 0x03, static_cast<FRegister>(rt), fs, static_cast<FRegister>(0), 0x0);
}

void MipsAssembler::Mthc1(Register rt, FRegister fs) {
  EmitFR(0x11, 0x07, static_cast<FRegister>(rt), fs, static_cast<FRegister>(0), 0x0);
}

void MipsAssembler::MoveFromFpuHigh(Register rt, FRegister fs) {
  if (Is32BitFPU()) {
    CHECK_EQ(fs % 2, 0) << fs;
    Mfc1(rt, static_cast<FRegister>(fs + 1));
  } else {
    Mfhc1(rt, fs);
  }
}

void MipsAssembler::MoveToFpuHigh(Register rt, FRegister fs) {
  if (Is32BitFPU()) {
    CHECK_EQ(fs % 2, 0) << fs;
    Mtc1(rt, static_cast<FRegister>(fs + 1));
  } else {
    Mthc1(rt, fs);
  }
}

void MipsAssembler::Lwc1(FRegister ft, Register rs, uint16_t imm16) {
  EmitI(0x31, rs, static_cast<Register>(ft), imm16);
}

void MipsAssembler::Ldc1(FRegister ft, Register rs, uint16_t imm16) {
  EmitI(0x35, rs, static_cast<Register>(ft), imm16);
}

void MipsAssembler::Swc1(FRegister ft, Register rs, uint16_t imm16) {
  EmitI(0x39, rs, static_cast<Register>(ft), imm16);
}

void MipsAssembler::Sdc1(FRegister ft, Register rs, uint16_t imm16) {
  EmitI(0x3d, rs, static_cast<Register>(ft), imm16);
}

void MipsAssembler::Break() {
  EmitR(0, static_cast<Register>(0), static_cast<Register>(0),
        static_cast<Register>(0), 0, 0xD);
}

void MipsAssembler::Nop() {
  EmitR(0x0, static_cast<Register>(0), static_cast<Register>(0), static_cast<Register>(0), 0, 0x0);
}

void MipsAssembler::Move(Register rd, Register rs) {
  Or(rd, rs, ZERO);
}

void MipsAssembler::Clear(Register rd) {
  Move(rd, ZERO);
}

void MipsAssembler::Not(Register rd, Register rs) {
  Nor(rd, rs, ZERO);
}

void MipsAssembler::Push(Register rs) {
  IncreaseFrameSize(kMipsWordSize);
  Sw(rs, SP, 0);
}

void MipsAssembler::Pop(Register rd) {
  Lw(rd, SP, 0);
  DecreaseFrameSize(kMipsWordSize);
}

void MipsAssembler::PopAndReturn(Register rd, Register rt) {
  Lw(rd, SP, 0);
  Jr(rt);
  DecreaseFrameSize(kMipsWordSize);
}

void MipsAssembler::LoadConst32(Register rd, int32_t value) {
  if (IsUint<16>(value)) {
    // Use OR with (unsigned) immediate to encode 16b unsigned int.
    Ori(rd, ZERO, value);
  } else if (IsInt<16>(value)) {
    // Use ADD with (signed) immediate to encode 16b signed int.
    Addiu(rd, ZERO, value);
  } else {
    Lui(rd, High16Bits(value));
    if (value & 0xFFFF)
      Ori(rd, rd, Low16Bits(value));
  }
}

void MipsAssembler::LoadConst64(Register reg_hi, Register reg_lo, int64_t value) {
  uint32_t low = Low32Bits(value);
  uint32_t high = High32Bits(value);
  LoadConst32(reg_lo, low);
  if (high != low) {
    LoadConst32(reg_hi, high);
  } else {
    Move(reg_hi, reg_lo);
  }
}

void MipsAssembler::StoreConst32ToOffset(int32_t value,
                                         Register base,
                                         int32_t offset,
                                         Register temp) {
  if (!IsInt<16>(offset)) {
    CHECK_NE(temp, AT);  //  Must not use AT as temp, as not to overwrite the loaded value.
    LoadConst32(AT, offset);
    Addu(AT, AT, base);
    base = AT;
    offset = 0;
  }
  if (value == 0) {
    temp = ZERO;
  } else {
    LoadConst32(temp, value);
  }
  Sw(temp, base, offset);
}

void MipsAssembler::StoreConst64ToOffset(int64_t value,
                                         Register base,
                                         int32_t offset,
                                         Register temp) {
  // IsInt<16> must be passed a signed value.
  if (!IsInt<16>(offset) || !IsInt<16>(static_cast<int32_t>(offset + kMipsWordSize))) {
    CHECK_NE(temp, AT);  //  Must not use AT as temp, as not to overwrite the loaded value.
    LoadConst32(AT, offset);
    Addu(AT, AT, base);
    base = AT;
    offset = 0;
  }
  uint32_t low = Low32Bits(value);
  uint32_t high = High32Bits(value);
  if (low == 0) {
    Sw(ZERO, base, offset);
  } else {
    LoadConst32(temp, low);
    Sw(temp, base, offset);
  }
  if (high == 0) {
    Sw(ZERO, base, offset + kMipsWordSize);
  } else {
    if (high != low) {
      LoadConst32(temp, high);
    }
    Sw(temp, base, offset + kMipsWordSize);
  }
}

void MipsAssembler::LoadSConst32(FRegister r, int32_t value, Register temp) {
  if (value == 0) {
    temp = ZERO;
  } else {
    LoadConst32(temp, value);
  }
  Mtc1(temp, r);
}

void MipsAssembler::LoadDConst64(FRegister rd, int64_t value, Register temp) {
  uint32_t low = Low32Bits(value);
  uint32_t high = High32Bits(value);
  if (low == 0) {
    Mtc1(ZERO, rd);
  } else {
    LoadConst32(temp, low);
    Mtc1(temp, rd);
  }
  if (high == 0) {
    MoveToFpuHigh(ZERO, rd);
  } else {
    LoadConst32(temp, high);
    MoveToFpuHigh(temp, rd);
  }
}

void MipsAssembler::Addiu32(Register rt, Register rs, int32_t value, Register temp) {
  if (IsInt<16>(value)) {
    Addiu(rt, rs, value);
  } else {
    LoadConst32(temp, value);
    Addu(rt, rs, temp);
  }
}

void MipsAssembler::Branch::InitShortOrLong(MipsAssembler::Branch::OffsetBits offset_size,
                                            MipsAssembler::Branch::Type short_type,
                                            MipsAssembler::Branch::Type long_type) {
  type_ = (offset_size <= branch_info_[short_type].offset_size) ? short_type : long_type;
}

void MipsAssembler::Branch::InitializeType(bool is_call, bool is_r6) {
  OffsetBits offset_size = GetOffsetSizeNeeded(location_, target_);
  if (is_r6) {
    // R6
    if (is_call) {
      InitShortOrLong(offset_size, kR6Call, kR6LongCall);
    } else if (condition_ == kUncond) {
      InitShortOrLong(offset_size, kR6UncondBranch, kR6LongUncondBranch);
    } else {
      if (condition_ == kCondEQZ || condition_ == kCondNEZ) {
        // Special case for beqzc/bnezc with longer offset than in other b<cond>c instructions.
        type_ = (offset_size <= kOffset23) ? kR6CondBranch : kR6LongCondBranch;
      } else {
        InitShortOrLong(offset_size, kR6CondBranch, kR6LongCondBranch);
      }
    }
  } else {
    // R2
    if (is_call) {
      InitShortOrLong(offset_size, kCall, kLongCall);
    } else if (condition_ == kUncond) {
      InitShortOrLong(offset_size, kUncondBranch, kLongUncondBranch);
    } else {
      InitShortOrLong(offset_size, kCondBranch, kLongCondBranch);
    }
  }
  old_type_ = type_;
}

bool MipsAssembler::Branch::IsNop(BranchCondition condition, Register lhs, Register rhs) {
  switch (condition) {
    case kCondLT:
    case kCondGT:
    case kCondNE:
    case kCondLTU:
      return lhs == rhs;
    default:
      return false;
  }
}

bool MipsAssembler::Branch::IsUncond(BranchCondition condition, Register lhs, Register rhs) {
  switch (condition) {
    case kUncond:
      return true;
    case kCondGE:
    case kCondLE:
    case kCondEQ:
    case kCondGEU:
      return lhs == rhs;
    default:
      return false;
  }
}

MipsAssembler::Branch::Branch(bool is_r6, uint32_t location, uint32_t target)
    : old_location_(location),
      location_(location),
      target_(target),
      lhs_reg_(0),
      rhs_reg_(0),
      condition_(kUncond) {
  InitializeType(false, is_r6);
}

MipsAssembler::Branch::Branch(bool is_r6,
                              uint32_t location,
                              uint32_t target,
                              MipsAssembler::BranchCondition condition,
                              Register lhs_reg,
                              Register rhs_reg)
    : old_location_(location),
      location_(location),
      target_(target),
      lhs_reg_(lhs_reg),
      rhs_reg_(rhs_reg),
      condition_(condition) {
  CHECK_NE(condition, kUncond);
  switch (condition) {
    case kCondLT:
    case kCondGE:
    case kCondLE:
    case kCondGT:
    case kCondLTU:
    case kCondGEU:
      // We don't support synthetic R2 branches (preceded with slt[u]) at this level
      // (R2 doesn't have branches to compare 2 registers using <, <=, >=, >).
      // We leave this up to the caller.
      CHECK(is_r6);
      FALLTHROUGH_INTENDED;
    case kCondEQ:
    case kCondNE:
      // Require registers other than 0 not only for R6, but also for R2 to catch errors.
      // To compare with 0, use dedicated kCond*Z conditions.
      CHECK_NE(lhs_reg, ZERO);
      CHECK_NE(rhs_reg, ZERO);
      break;
    case kCondLTZ:
    case kCondGEZ:
    case kCondLEZ:
    case kCondGTZ:
    case kCondEQZ:
    case kCondNEZ:
      // Require registers other than 0 not only for R6, but also for R2 to catch errors.
      CHECK_NE(lhs_reg, ZERO);
      CHECK_EQ(rhs_reg, ZERO);
      break;
    case kCondF:
    case kCondT:
      CHECK_EQ(rhs_reg, ZERO);
      break;
    case kUncond:
      UNREACHABLE();
  }
  CHECK(!IsNop(condition, lhs_reg, rhs_reg));
  if (IsUncond(condition, lhs_reg, rhs_reg)) {
    // Branch condition is always true, make the branch unconditional.
    condition_ = kUncond;
  }
  InitializeType(false, is_r6);
}

MipsAssembler::Branch::Branch(bool is_r6, uint32_t location, uint32_t target, Register indirect_reg)
    : old_location_(location),
      location_(location),
      target_(target),
      lhs_reg_(indirect_reg),
      rhs_reg_(0),
      condition_(kUncond) {
  CHECK_NE(indirect_reg, ZERO);
  CHECK_NE(indirect_reg, AT);
  InitializeType(true, is_r6);
}

MipsAssembler::BranchCondition MipsAssembler::Branch::OppositeCondition(
    MipsAssembler::BranchCondition cond) {
  switch (cond) {
    case kCondLT:
      return kCondGE;
    case kCondGE:
      return kCondLT;
    case kCondLE:
      return kCondGT;
    case kCondGT:
      return kCondLE;
    case kCondLTZ:
      return kCondGEZ;
    case kCondGEZ:
      return kCondLTZ;
    case kCondLEZ:
      return kCondGTZ;
    case kCondGTZ:
      return kCondLEZ;
    case kCondEQ:
      return kCondNE;
    case kCondNE:
      return kCondEQ;
    case kCondEQZ:
      return kCondNEZ;
    case kCondNEZ:
      return kCondEQZ;
    case kCondLTU:
      return kCondGEU;
    case kCondGEU:
      return kCondLTU;
    case kCondF:
      return kCondT;
    case kCondT:
      return kCondF;
    case kUncond:
      LOG(FATAL) << "Unexpected branch condition " << cond;
  }
  UNREACHABLE();
}

MipsAssembler::Branch::Type MipsAssembler::Branch::GetType() const {
  return type_;
}

MipsAssembler::BranchCondition MipsAssembler::Branch::GetCondition() const {
  return condition_;
}

Register MipsAssembler::Branch::GetLeftRegister() const {
  return static_cast<Register>(lhs_reg_);
}

Register MipsAssembler::Branch::GetRightRegister() const {
  return static_cast<Register>(rhs_reg_);
}

uint32_t MipsAssembler::Branch::GetTarget() const {
  return target_;
}

uint32_t MipsAssembler::Branch::GetLocation() const {
  return location_;
}

uint32_t MipsAssembler::Branch::GetOldLocation() const {
  return old_location_;
}

uint32_t MipsAssembler::Branch::GetLength() const {
  return branch_info_[type_].length;
}

uint32_t MipsAssembler::Branch::GetOldLength() const {
  return branch_info_[old_type_].length;
}

uint32_t MipsAssembler::Branch::GetSize() const {
  return GetLength() * sizeof(uint32_t);
}

uint32_t MipsAssembler::Branch::GetOldSize() const {
  return GetOldLength() * sizeof(uint32_t);
}

uint32_t MipsAssembler::Branch::GetEndLocation() const {
  return GetLocation() + GetSize();
}

uint32_t MipsAssembler::Branch::GetOldEndLocation() const {
  return GetOldLocation() + GetOldSize();
}

bool MipsAssembler::Branch::IsLong() const {
  switch (type_) {
    // R2 short branches.
    case kUncondBranch:
    case kCondBranch:
    case kCall:
    // R6 short branches.
    case kR6UncondBranch:
    case kR6CondBranch:
    case kR6Call:
      return false;
    // R2 long branches.
    case kLongUncondBranch:
    case kLongCondBranch:
    case kLongCall:
    // R6 long branches.
    case kR6LongUncondBranch:
    case kR6LongCondBranch:
    case kR6LongCall:
      return true;
  }
  UNREACHABLE();
}

bool MipsAssembler::Branch::IsResolved() const {
  return target_ != kUnresolved;
}

MipsAssembler::Branch::OffsetBits MipsAssembler::Branch::GetOffsetSize() const {
  OffsetBits offset_size =
      (type_ == kR6CondBranch && (condition_ == kCondEQZ || condition_ == kCondNEZ))
          ? kOffset23
          : branch_info_[type_].offset_size;
  return offset_size;
}

MipsAssembler::Branch::OffsetBits MipsAssembler::Branch::GetOffsetSizeNeeded(uint32_t location,
                                                                             uint32_t target) {
  // For unresolved targets assume the shortest encoding
  // (later it will be made longer if needed).
  if (target == kUnresolved)
    return kOffset16;
  int64_t distance = static_cast<int64_t>(target) - location;
  // To simplify calculations in composite branches consisting of multiple instructions
  // bump up the distance by a value larger than the max byte size of a composite branch.
  distance += (distance >= 0) ? kMaxBranchSize : -kMaxBranchSize;
  if (IsInt<kOffset16>(distance))
    return kOffset16;
  else if (IsInt<kOffset18>(distance))
    return kOffset18;
  else if (IsInt<kOffset21>(distance))
    return kOffset21;
  else if (IsInt<kOffset23>(distance))
    return kOffset23;
  else if (IsInt<kOffset28>(distance))
    return kOffset28;
  return kOffset32;
}

void MipsAssembler::Branch::Resolve(uint32_t target) {
  target_ = target;
}

void MipsAssembler::Branch::Relocate(uint32_t expand_location, uint32_t delta) {
  if (location_ > expand_location) {
    location_ += delta;
  }
  if (!IsResolved()) {
    return;  // Don't know the target yet.
  }
  if (target_ > expand_location) {
    target_ += delta;
  }
}

void MipsAssembler::Branch::PromoteToLong() {
  switch (type_) {
    // R2 short branches.
    case kUncondBranch:
      type_ = kLongUncondBranch;
      break;
    case kCondBranch:
      type_ = kLongCondBranch;
      break;
    case kCall:
      type_ = kLongCall;
      break;
    // R6 short branches.
    case kR6UncondBranch:
      type_ = kR6LongUncondBranch;
      break;
    case kR6CondBranch:
      type_ = kR6LongCondBranch;
      break;
    case kR6Call:
      type_ = kR6LongCall;
      break;
    default:
      // Note: 'type_' is already long.
      break;
  }
  CHECK(IsLong());
}

uint32_t MipsAssembler::Branch::PromoteIfNeeded(uint32_t max_short_distance) {
  // If the branch is still unresolved or already long, nothing to do.
  if (IsLong() || !IsResolved()) {
    return 0;
  }
  // Promote the short branch to long if the offset size is too small
  // to hold the distance between location_ and target_.
  if (GetOffsetSizeNeeded(location_, target_) > GetOffsetSize()) {
    PromoteToLong();
    uint32_t old_size = GetOldSize();
    uint32_t new_size = GetSize();
    CHECK_GT(new_size, old_size);
    return new_size - old_size;
  }
  // The following logic is for debugging/testing purposes.
  // Promote some short branches to long when it's not really required.
  if (UNLIKELY(max_short_distance != std::numeric_limits<uint32_t>::max())) {
    int64_t distance = static_cast<int64_t>(target_) - location_;
    distance = (distance >= 0) ? distance : -distance;
    if (distance >= max_short_distance) {
      PromoteToLong();
      uint32_t old_size = GetOldSize();
      uint32_t new_size = GetSize();
      CHECK_GT(new_size, old_size);
      return new_size - old_size;
    }
  }
  return 0;
}

uint32_t MipsAssembler::Branch::GetOffsetLocation() const {
  return location_ + branch_info_[type_].instr_offset * sizeof(uint32_t);
}

uint32_t MipsAssembler::Branch::GetOffset() const {
  CHECK(IsResolved());
  uint32_t ofs_mask = 0xFFFFFFFF >> (32 - GetOffsetSize());
  // Calculate the byte distance between instructions and also account for
  // different PC-relative origins.
  uint32_t offset = target_ - GetOffsetLocation() - branch_info_[type_].pc_org * sizeof(uint32_t);
  // Prepare the offset for encoding into the instruction(s).
  offset = (offset & ofs_mask) >> branch_info_[type_].offset_shift;
  return offset;
}

MipsAssembler::Branch* MipsAssembler::GetBranch(uint32_t branch_id) {
  CHECK_LT(branch_id, branches_.size());
  return &branches_[branch_id];
}

const MipsAssembler::Branch* MipsAssembler::GetBranch(uint32_t branch_id) const {
  CHECK_LT(branch_id, branches_.size());
  return &branches_[branch_id];
}

void MipsAssembler::Bind(MipsLabel* label) {
  CHECK(!label->IsBound());
  uint32_t bound_pc = buffer_.Size();

  // Walk the list of branches referring to and preceding this label.
  // Store the previously unknown target addresses in them.
  while (label->IsLinked()) {
    uint32_t branch_id = label->Position();
    Branch* branch = GetBranch(branch_id);
    branch->Resolve(bound_pc);

    uint32_t branch_location = branch->GetLocation();
    // Extract the location of the previous branch in the list (walking the list backwards;
    // the previous branch ID was stored in the space reserved for this branch).
    uint32_t prev = buffer_.Load<uint32_t>(branch_location);

    // On to the previous branch in the list...
    label->position_ = prev;
  }

  // Now make the label object contain its own location (relative to the end of the preceding
  // branch, if any; it will be used by the branches referring to and following this label).
  label->prev_branch_id_plus_one_ = branches_.size();
  if (label->prev_branch_id_plus_one_) {
    uint32_t branch_id = label->prev_branch_id_plus_one_ - 1;
    const Branch* branch = GetBranch(branch_id);
    bound_pc -= branch->GetEndLocation();
  }
  label->BindTo(bound_pc);
}

uint32_t MipsAssembler::GetLabelLocation(MipsLabel* label) const {
  CHECK(label->IsBound());
  uint32_t target = label->Position();
  if (label->prev_branch_id_plus_one_) {
    // Get label location based on the branch preceding it.
    uint32_t branch_id = label->prev_branch_id_plus_one_ - 1;
    const Branch* branch = GetBranch(branch_id);
    target += branch->GetEndLocation();
  }
  return target;
}

uint32_t MipsAssembler::GetAdjustedPosition(uint32_t old_position) {
  // We can reconstruct the adjustment by going through all the branches from the beginning
  // up to the old_position. Since we expect AdjustedPosition() to be called in a loop
  // with increasing old_position, we can use the data from last AdjustedPosition() to
  // continue where we left off and the whole loop should be O(m+n) where m is the number
  // of positions to adjust and n is the number of branches.
  if (old_position < last_old_position_) {
    last_position_adjustment_ = 0;
    last_old_position_ = 0;
    last_branch_id_ = 0;
  }
  while (last_branch_id_ != branches_.size()) {
    const Branch* branch = GetBranch(last_branch_id_);
    if (branch->GetLocation() >= old_position + last_position_adjustment_) {
      break;
    }
    last_position_adjustment_ += branch->GetSize() - branch->GetOldSize();
    ++last_branch_id_;
  }
  last_old_position_ = old_position;
  return old_position + last_position_adjustment_;
}

void MipsAssembler::FinalizeLabeledBranch(MipsLabel* label) {
  uint32_t length = branches_.back().GetLength();
  if (!label->IsBound()) {
    // Branch forward (to a following label), distance is unknown.
    // The first branch forward will contain 0, serving as the terminator of
    // the list of forward-reaching branches.
    Emit(label->position_);
    length--;
    // Now make the label object point to this branch
    // (this forms a linked list of branches preceding this label).
    uint32_t branch_id = branches_.size() - 1;
    label->LinkTo(branch_id);
  }
  // Reserve space for the branch.
  while (length--) {
    Nop();
  }
}

void MipsAssembler::Buncond(MipsLabel* label) {
  uint32_t target = label->IsBound() ? GetLabelLocation(label) : Branch::kUnresolved;
  branches_.emplace_back(IsR6(), buffer_.Size(), target);
  FinalizeLabeledBranch(label);
}

void MipsAssembler::Bcond(MipsLabel* label, BranchCondition condition, Register lhs, Register rhs) {
  // If lhs = rhs, this can be a NOP.
  if (Branch::IsNop(condition, lhs, rhs)) {
    return;
  }
  uint32_t target = label->IsBound() ? GetLabelLocation(label) : Branch::kUnresolved;
  branches_.emplace_back(IsR6(), buffer_.Size(), target, condition, lhs, rhs);
  FinalizeLabeledBranch(label);
}

void MipsAssembler::Call(MipsLabel* label, Register indirect_reg) {
  uint32_t target = label->IsBound() ? GetLabelLocation(label) : Branch::kUnresolved;
  branches_.emplace_back(IsR6(), buffer_.Size(), target, indirect_reg);
  FinalizeLabeledBranch(label);
}

void MipsAssembler::PromoteBranches() {
  // Promote short branches to long as necessary.
  bool changed;
  do {
    changed = false;
    for (auto& branch : branches_) {
      CHECK(branch.IsResolved());
      uint32_t delta = branch.PromoteIfNeeded();
      // If this branch has been promoted and needs to expand in size,
      // relocate all branches by the expansion size.
      if (delta) {
        changed = true;
        uint32_t expand_location = branch.GetLocation();
        for (auto& branch2 : branches_) {
          branch2.Relocate(expand_location, delta);
        }
      }
    }
  } while (changed);

  // Account for branch expansion by resizing the code buffer
  // and moving the code in it to its final location.
  size_t branch_count = branches_.size();
  if (branch_count > 0) {
    // Resize.
    Branch& last_branch = branches_[branch_count - 1];
    uint32_t size_delta = last_branch.GetEndLocation() - last_branch.GetOldEndLocation();
    uint32_t old_size = buffer_.Size();
    buffer_.Resize(old_size + size_delta);
    // Move the code residing between branch placeholders.
    uint32_t end = old_size;
    for (size_t i = branch_count; i > 0; ) {
      Branch& branch = branches_[--i];
      uint32_t size = end - branch.GetOldEndLocation();
      buffer_.Move(branch.GetEndLocation(), branch.GetOldEndLocation(), size);
      end = branch.GetOldLocation();
    }
  }
}

// Note: make sure branch_info_[] and EmitBranch() are kept synchronized.
const MipsAssembler::Branch::BranchInfo MipsAssembler::Branch::branch_info_[] = {
  // R2 short branches.
  {  2, 0, 1, MipsAssembler::Branch::kOffset18, 2 },  // kUncondBranch
  {  2, 0, 1, MipsAssembler::Branch::kOffset18, 2 },  // kCondBranch
  {  5, 2, 0, MipsAssembler::Branch::kOffset16, 0 },  // kCall
  // R2 long branches.
  {  9, 3, 1, MipsAssembler::Branch::kOffset32, 0 },  // kLongUncondBranch
  { 10, 4, 1, MipsAssembler::Branch::kOffset32, 0 },  // kLongCondBranch
  {  6, 1, 1, MipsAssembler::Branch::kOffset32, 0 },  // kLongCall
  // R6 short branches.
  {  1, 0, 1, MipsAssembler::Branch::kOffset28, 2 },  // kR6UncondBranch
  {  2, 0, 1, MipsAssembler::Branch::kOffset18, 2 },  // kR6CondBranch
                                                      // Exception: kOffset23 for beqzc/bnezc.
  {  2, 0, 0, MipsAssembler::Branch::kOffset21, 2 },  // kR6Call
  // R6 long branches.
  {  2, 0, 0, MipsAssembler::Branch::kOffset32, 0 },  // kR6LongUncondBranch
  {  3, 1, 0, MipsAssembler::Branch::kOffset32, 0 },  // kR6LongCondBranch
  {  3, 0, 0, MipsAssembler::Branch::kOffset32, 0 },  // kR6LongCall
};

// Note: make sure branch_info_[] and mitBranch() are kept synchronized.
void MipsAssembler::EmitBranch(MipsAssembler::Branch* branch) {
  CHECK_EQ(overwriting_, true);
  overwrite_location_ = branch->GetLocation();
  uint32_t offset = branch->GetOffset();
  BranchCondition condition = branch->GetCondition();
  Register lhs = branch->GetLeftRegister();
  Register rhs = branch->GetRightRegister();
  switch (branch->GetType()) {
    // R2 short branches.
    case Branch::kUncondBranch:
      CHECK_EQ(overwrite_location_, branch->GetOffsetLocation());
      B(offset);
      Nop();  // TODO: improve by filling the delay slot.
      break;
    case Branch::kCondBranch:
      CHECK_EQ(overwrite_location_, branch->GetOffsetLocation());
      EmitBcondR2(condition, lhs, rhs, offset);
      Nop();  // TODO: improve by filling the delay slot.
      break;
    case Branch::kCall:
      Nal();
      Nop();  // TODO: is this NOP really needed here?
      CHECK_EQ(overwrite_location_, branch->GetOffsetLocation());
      Addiu(lhs, RA, offset);
      Jalr(lhs);
      Nop();
      break;

    // R2 long branches.
    case Branch::kLongUncondBranch:
      // To get the value of the PC register we need to use the NAL instruction.
      // NAL clobbers the RA register. However, RA must be preserved if the
      // method is compiled without the entry/exit sequences that would take care
      // of preserving RA (typically, leaf methods don't preserve RA explicitly).
      // So, we need to preserve RA in some temporary storage ourselves. The AT
      // register can't be used for this because we need it to load a constant
      // which will be added to the value that NAL stores in RA. And we can't
      // use T9 for this in the context of the JNI compiler, which uses it
      // as a scratch register (see InterproceduralScratchRegister()).
      // If we were to add a 32-bit constant to RA using two ADDIU instructions,
      // we'd also need to use the ROTR instruction, which requires no less than
      // MIPSR2.
      // Perhaps, we could use T8 or one of R2's multiplier/divider registers
      // (LO or HI) or even a floating-point register, but that doesn't seem
      // like a nice solution. We may want this to work on both R6 and pre-R6.
      // For now simply use the stack for RA. This should be OK since for the
      // vast majority of code a short PC-relative branch is sufficient.
      // TODO: can this be improved?
      Push(RA);
      Nal();
      CHECK_EQ(overwrite_location_, branch->GetOffsetLocation());
      Lui(AT, High16Bits(offset));
      Ori(AT, AT, Low16Bits(offset));
      Addu(AT, AT, RA);
      Lw(RA, SP, 0);
      Jr(AT);
      DecreaseFrameSize(kMipsWordSize);
      break;
    case Branch::kLongCondBranch:
      // The comment on case 'Branch::kLongUncondBranch' applies here as well.
      // Note: the opposite condition branch encodes 8 as the distance, which is equal to the
      // number of instructions skipped:
      // (PUSH(IncreaseFrameSize(ADDIU) + SW) + NAL + LUI + ORI + ADDU + LW + JR).
      EmitBcondR2(Branch::OppositeCondition(condition), lhs, rhs, 8);
      Push(RA);
      Nal();
      CHECK_EQ(overwrite_location_, branch->GetOffsetLocation());
      Lui(AT, High16Bits(offset));
      Ori(AT, AT, Low16Bits(offset));
      Addu(AT, AT, RA);
      Lw(RA, SP, 0);
      Jr(AT);
      DecreaseFrameSize(kMipsWordSize);
      break;
    case Branch::kLongCall:
      Nal();
      CHECK_EQ(overwrite_location_, branch->GetOffsetLocation());
      Lui(AT, High16Bits(offset));
      Ori(AT, AT, Low16Bits(offset));
      Addu(lhs, AT, RA);
      Jalr(lhs);
      Nop();
      break;

    // R6 short branches.
    case Branch::kR6UncondBranch:
      CHECK_EQ(overwrite_location_, branch->GetOffsetLocation());
      Bc(offset);
      break;
    case Branch::kR6CondBranch:
      CHECK_EQ(overwrite_location_, branch->GetOffsetLocation());
      EmitBcondR6(condition, lhs, rhs, offset);
      Nop();  // TODO: improve by filling the forbidden/delay slot.
      break;
    case Branch::kR6Call:
      CHECK_EQ(overwrite_location_, branch->GetOffsetLocation());
      Addiupc(lhs, offset);
      Jialc(lhs, 0);
      break;

    // R6 long branches.
    case Branch::kR6LongUncondBranch:
      offset += (offset & 0x8000) << 1;  // Account for sign extension in jic.
      CHECK_EQ(overwrite_location_, branch->GetOffsetLocation());
      Auipc(AT, High16Bits(offset));
      Jic(AT, Low16Bits(offset));
      break;
    case Branch::kR6LongCondBranch:
      EmitBcondR6(Branch::OppositeCondition(condition), lhs, rhs, 2);
      offset += (offset & 0x8000) << 1;  // Account for sign extension in jic.
      CHECK_EQ(overwrite_location_, branch->GetOffsetLocation());
      Auipc(AT, High16Bits(offset));
      Jic(AT, Low16Bits(offset));
      break;
    case Branch::kR6LongCall:
      offset += (offset & 0x8000) << 1;  // Account for sign extension in addiu.
      CHECK_EQ(overwrite_location_, branch->GetOffsetLocation());
      Auipc(lhs, High16Bits(offset));
      Addiu(lhs, lhs, Low16Bits(offset));
      Jialc(lhs, 0);
      break;
  }
  CHECK_EQ(overwrite_location_, branch->GetEndLocation());
  CHECK_LT(branch->GetSize(), static_cast<uint32_t>(Branch::kMaxBranchSize));
}

void MipsAssembler::B(MipsLabel* label) {
  Buncond(label);
}

void MipsAssembler::Jalr(MipsLabel* label, Register indirect_reg) {
  Call(label, indirect_reg);
}

void MipsAssembler::Beq(Register rs, Register rt, MipsLabel* label) {
  Bcond(label, kCondEQ, rs, rt);
}

void MipsAssembler::Bne(Register rs, Register rt, MipsLabel* label) {
  Bcond(label, kCondNE, rs, rt);
}

void MipsAssembler::Beqz(Register rt, MipsLabel* label) {
  Bcond(label, kCondEQZ, rt);
}

void MipsAssembler::Bnez(Register rt, MipsLabel* label) {
  Bcond(label, kCondNEZ, rt);
}

void MipsAssembler::Bltz(Register rt, MipsLabel* label) {
  Bcond(label, kCondLTZ, rt);
}

void MipsAssembler::Bgez(Register rt, MipsLabel* label) {
  Bcond(label, kCondGEZ, rt);
}

void MipsAssembler::Blez(Register rt, MipsLabel* label) {
  Bcond(label, kCondLEZ, rt);
}

void MipsAssembler::Bgtz(Register rt, MipsLabel* label) {
  Bcond(label, kCondGTZ, rt);
}

void MipsAssembler::Blt(Register rs, Register rt, MipsLabel* label) {
  if (IsR6()) {
    Bcond(label, kCondLT, rs, rt);
  } else if (!Branch::IsNop(kCondLT, rs, rt)) {
    // Synthesize the instruction (not available on R2).
    Slt(AT, rs, rt);
    Bnez(AT, label);
  }
}

void MipsAssembler::Bge(Register rs, Register rt, MipsLabel* label) {
  if (IsR6()) {
    Bcond(label, kCondGE, rs, rt);
  } else if (Branch::IsUncond(kCondGE, rs, rt)) {
    B(label);
  } else {
    // Synthesize the instruction (not available on R2).
    Slt(AT, rs, rt);
    Beqz(AT, label);
  }
}

void MipsAssembler::Bltu(Register rs, Register rt, MipsLabel* label) {
  if (IsR6()) {
    Bcond(label, kCondLTU, rs, rt);
  } else if (!Branch::IsNop(kCondLTU, rs, rt)) {
    // Synthesize the instruction (not available on R2).
    Sltu(AT, rs, rt);
    Bnez(AT, label);
  }
}

void MipsAssembler::Bgeu(Register rs, Register rt, MipsLabel* label) {
  if (IsR6()) {
    Bcond(label, kCondGEU, rs, rt);
  } else if (Branch::IsUncond(kCondGEU, rs, rt)) {
    B(label);
  } else {
    // Synthesize the instruction (not available on R2).
    Sltu(AT, rs, rt);
    Beqz(AT, label);
  }
}

void MipsAssembler::Bc1f(MipsLabel* label) {
  Bc1f(0, label);
}

void MipsAssembler::Bc1f(int cc, MipsLabel* label) {
  CHECK(IsUint<3>(cc)) << cc;
  Bcond(label, kCondF, static_cast<Register>(cc), ZERO);
}

void MipsAssembler::Bc1t(MipsLabel* label) {
  Bc1t(0, label);
}

void MipsAssembler::Bc1t(int cc, MipsLabel* label) {
  CHECK(IsUint<3>(cc)) << cc;
  Bcond(label, kCondT, static_cast<Register>(cc), ZERO);
}

void MipsAssembler::Bc1eqz(FRegister ft, MipsLabel* label) {
  Bcond(label, kCondF, static_cast<Register>(ft), ZERO);
}

void MipsAssembler::Bc1nez(FRegister ft, MipsLabel* label) {
  Bcond(label, kCondT, static_cast<Register>(ft), ZERO);
}

void MipsAssembler::LoadFromOffset(LoadOperandType type, Register reg, Register base,
                                   int32_t offset) {
  // IsInt<16> must be passed a signed value.
  if (!IsInt<16>(offset) ||
      (type == kLoadDoubleword && !IsInt<16>(static_cast<int32_t>(offset + kMipsWordSize)))) {
    LoadConst32(AT, offset);
    Addu(AT, AT, base);
    base = AT;
    offset = 0;
  }

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
    case kLoadDoubleword:
      if (reg == base) {
        // This will clobber the base when loading the lower register. Since we have to load the
        // higher register as well, this will fail. Solution: reverse the order.
        Lw(static_cast<Register>(reg + 1), base, offset + kMipsWordSize);
        Lw(reg, base, offset);
      } else {
        Lw(reg, base, offset);
        Lw(static_cast<Register>(reg + 1), base, offset + kMipsWordSize);
      }
      break;
    default:
      LOG(FATAL) << "UNREACHABLE";
  }
}

void MipsAssembler::LoadSFromOffset(FRegister reg, Register base, int32_t offset) {
  if (!IsInt<16>(offset)) {
    LoadConst32(AT, offset);
    Addu(AT, AT, base);
    base = AT;
    offset = 0;
  }

  Lwc1(reg, base, offset);
}

void MipsAssembler::LoadDFromOffset(FRegister reg, Register base, int32_t offset) {
  // IsInt<16> must be passed a signed value.
  if (!IsInt<16>(offset) ||
      (!IsAligned<kMipsDoublewordSize>(offset) &&
       !IsInt<16>(static_cast<int32_t>(offset + kMipsWordSize)))) {
    LoadConst32(AT, offset);
    Addu(AT, AT, base);
    base = AT;
    offset = 0;
  }

  if (offset & 0x7) {
    if (Is32BitFPU()) {
      Lwc1(reg, base, offset);
      Lwc1(static_cast<FRegister>(reg + 1), base, offset + kMipsWordSize);
    } else {
      // 64-bit FPU.
      Lwc1(reg, base, offset);
      Lw(T8, base, offset + kMipsWordSize);
      Mthc1(T8, reg);
    }
  } else {
    Ldc1(reg, base, offset);
  }
}

void MipsAssembler::EmitLoad(ManagedRegister m_dst, Register src_register, int32_t src_offset,
                             size_t size) {
  MipsManagedRegister dst = m_dst.AsMips();
  if (dst.IsNoRegister()) {
    CHECK_EQ(0u, size) << dst;
  } else if (dst.IsCoreRegister()) {
    CHECK_EQ(kMipsWordSize, size) << dst;
    LoadFromOffset(kLoadWord, dst.AsCoreRegister(), src_register, src_offset);
  } else if (dst.IsRegisterPair()) {
    CHECK_EQ(kMipsDoublewordSize, size) << dst;
    LoadFromOffset(kLoadDoubleword, dst.AsRegisterPairLow(), src_register, src_offset);
  } else if (dst.IsFRegister()) {
    if (size == kMipsWordSize) {
      LoadSFromOffset(dst.AsFRegister(), src_register, src_offset);
    } else {
      CHECK_EQ(kMipsDoublewordSize, size) << dst;
      LoadDFromOffset(dst.AsFRegister(), src_register, src_offset);
    }
  }
}

void MipsAssembler::StoreToOffset(StoreOperandType type, Register reg, Register base,
                                  int32_t offset) {
  // IsInt<16> must be passed a signed value.
  if (!IsInt<16>(offset) ||
      (type == kStoreDoubleword && !IsInt<16>(static_cast<int32_t>(offset + kMipsWordSize)))) {
    LoadConst32(AT, offset);
    Addu(AT, AT, base);
    base = AT;
    offset = 0;
  }

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
      CHECK_NE(reg, base);
      CHECK_NE(static_cast<Register>(reg + 1), base);
      Sw(reg, base, offset);
      Sw(static_cast<Register>(reg + 1), base, offset + kMipsWordSize);
      break;
    default:
      LOG(FATAL) << "UNREACHABLE";
  }
}

void MipsAssembler::StoreSToOffset(FRegister reg, Register base, int32_t offset) {
  if (!IsInt<16>(offset)) {
    LoadConst32(AT, offset);
    Addu(AT, AT, base);
    base = AT;
    offset = 0;
  }

  Swc1(reg, base, offset);
}

void MipsAssembler::StoreDToOffset(FRegister reg, Register base, int32_t offset) {
  // IsInt<16> must be passed a signed value.
  if (!IsInt<16>(offset) ||
      (!IsAligned<kMipsDoublewordSize>(offset) &&
       !IsInt<16>(static_cast<int32_t>(offset + kMipsWordSize)))) {
    LoadConst32(AT, offset);
    Addu(AT, AT, base);
    base = AT;
    offset = 0;
  }

  if (offset & 0x7) {
    if (Is32BitFPU()) {
      Swc1(reg, base, offset);
      Swc1(static_cast<FRegister>(reg + 1), base, offset + kMipsWordSize);
    } else {
      // 64-bit FPU.
      Mfhc1(T8, reg);
      Swc1(reg, base, offset);
      Sw(T8, base, offset + kMipsWordSize);
    }
  } else {
    Sdc1(reg, base, offset);
  }
}

static dwarf::Reg DWARFReg(Register reg) {
  return dwarf::Reg::MipsCore(static_cast<int>(reg));
}

constexpr size_t kFramePointerSize = 4;

void MipsAssembler::BuildFrame(size_t frame_size, ManagedRegister method_reg,
                               const std::vector<ManagedRegister>& callee_save_regs,
                               const ManagedRegisterEntrySpills& entry_spills) {
  CHECK_ALIGNED(frame_size, kStackAlignment);
  DCHECK(!overwriting_);

  // Increase frame to required size.
  IncreaseFrameSize(frame_size);

  // Push callee saves and return address.
  int stack_offset = frame_size - kFramePointerSize;
  StoreToOffset(kStoreWord, RA, SP, stack_offset);
  cfi_.RelOffset(DWARFReg(RA), stack_offset);
  for (int i = callee_save_regs.size() - 1; i >= 0; --i) {
    stack_offset -= kFramePointerSize;
    Register reg = callee_save_regs.at(i).AsMips().AsCoreRegister();
    StoreToOffset(kStoreWord, reg, SP, stack_offset);
    cfi_.RelOffset(DWARFReg(reg), stack_offset);
  }

  // Write out Method*.
  StoreToOffset(kStoreWord, method_reg.AsMips().AsCoreRegister(), SP, 0);

  // Write out entry spills.
  int32_t offset = frame_size + kFramePointerSize;
  for (size_t i = 0; i < entry_spills.size(); ++i) {
    MipsManagedRegister reg = entry_spills.at(i).AsMips();
    if (reg.IsNoRegister()) {
      ManagedRegisterSpill spill = entry_spills.at(i);
      offset += spill.getSize();
    } else if (reg.IsCoreRegister()) {
      StoreToOffset(kStoreWord, reg.AsCoreRegister(), SP, offset);
      offset += kMipsWordSize;
    } else if (reg.IsFRegister()) {
      StoreSToOffset(reg.AsFRegister(), SP, offset);
      offset += kMipsWordSize;
    } else if (reg.IsDRegister()) {
      StoreDToOffset(reg.AsOverlappingDRegisterLow(), SP, offset);
      offset += kMipsDoublewordSize;
    }
  }
}

void MipsAssembler::RemoveFrame(size_t frame_size,
                                const std::vector<ManagedRegister>& callee_save_regs) {
  CHECK_ALIGNED(frame_size, kStackAlignment);
  DCHECK(!overwriting_);
  cfi_.RememberState();

  // Pop callee saves and return address.
  int stack_offset = frame_size - (callee_save_regs.size() * kFramePointerSize) - kFramePointerSize;
  for (size_t i = 0; i < callee_save_regs.size(); ++i) {
    Register reg = callee_save_regs.at(i).AsMips().AsCoreRegister();
    LoadFromOffset(kLoadWord, reg, SP, stack_offset);
    cfi_.Restore(DWARFReg(reg));
    stack_offset += kFramePointerSize;
  }
  LoadFromOffset(kLoadWord, RA, SP, stack_offset);
  cfi_.Restore(DWARFReg(RA));

  // Decrease frame to required size.
  DecreaseFrameSize(frame_size);

  // Then jump to the return address.
  Jr(RA);
  Nop();

  // The CFI should be restored for any code that follows the exit block.
  cfi_.RestoreState();
  cfi_.DefCFAOffset(frame_size);
}

void MipsAssembler::IncreaseFrameSize(size_t adjust) {
  CHECK_ALIGNED(adjust, kFramePointerSize);
  Addiu32(SP, SP, -adjust);
  cfi_.AdjustCFAOffset(adjust);
  if (overwriting_) {
    cfi_.OverrideDelayedPC(overwrite_location_);
  }
}

void MipsAssembler::DecreaseFrameSize(size_t adjust) {
  CHECK_ALIGNED(adjust, kFramePointerSize);
  Addiu32(SP, SP, adjust);
  cfi_.AdjustCFAOffset(-adjust);
  if (overwriting_) {
    cfi_.OverrideDelayedPC(overwrite_location_);
  }
}

void MipsAssembler::Store(FrameOffset dest, ManagedRegister msrc, size_t size) {
  MipsManagedRegister src = msrc.AsMips();
  if (src.IsNoRegister()) {
    CHECK_EQ(0u, size);
  } else if (src.IsCoreRegister()) {
    CHECK_EQ(kMipsWordSize, size);
    StoreToOffset(kStoreWord, src.AsCoreRegister(), SP, dest.Int32Value());
  } else if (src.IsRegisterPair()) {
    CHECK_EQ(kMipsDoublewordSize, size);
    StoreToOffset(kStoreWord, src.AsRegisterPairLow(), SP, dest.Int32Value());
    StoreToOffset(kStoreWord, src.AsRegisterPairHigh(),
                  SP, dest.Int32Value() + kMipsWordSize);
  } else if (src.IsFRegister()) {
    if (size == kMipsWordSize) {
      StoreSToOffset(src.AsFRegister(), SP, dest.Int32Value());
    } else {
      CHECK_EQ(kMipsDoublewordSize, size);
      StoreDToOffset(src.AsFRegister(), SP, dest.Int32Value());
    }
  }
}

void MipsAssembler::StoreRef(FrameOffset dest, ManagedRegister msrc) {
  MipsManagedRegister src = msrc.AsMips();
  CHECK(src.IsCoreRegister());
  StoreToOffset(kStoreWord, src.AsCoreRegister(), SP, dest.Int32Value());
}

void MipsAssembler::StoreRawPtr(FrameOffset dest, ManagedRegister msrc) {
  MipsManagedRegister src = msrc.AsMips();
  CHECK(src.IsCoreRegister());
  StoreToOffset(kStoreWord, src.AsCoreRegister(), SP, dest.Int32Value());
}

void MipsAssembler::StoreImmediateToFrame(FrameOffset dest, uint32_t imm,
                                          ManagedRegister mscratch) {
  MipsManagedRegister scratch = mscratch.AsMips();
  CHECK(scratch.IsCoreRegister()) << scratch;
  LoadConst32(scratch.AsCoreRegister(), imm);
  StoreToOffset(kStoreWord, scratch.AsCoreRegister(), SP, dest.Int32Value());
}

void MipsAssembler::StoreImmediateToThread32(ThreadOffset<kMipsWordSize> dest, uint32_t imm,
                                             ManagedRegister mscratch) {
  MipsManagedRegister scratch = mscratch.AsMips();
  CHECK(scratch.IsCoreRegister()) << scratch;
  // Is this function even referenced anywhere else in the code?
  LoadConst32(scratch.AsCoreRegister(), imm);
  StoreToOffset(kStoreWord, scratch.AsCoreRegister(), S1, dest.Int32Value());
}

void MipsAssembler::StoreStackOffsetToThread32(ThreadOffset<kMipsWordSize> thr_offs,
                                               FrameOffset fr_offs,
                                               ManagedRegister mscratch) {
  MipsManagedRegister scratch = mscratch.AsMips();
  CHECK(scratch.IsCoreRegister()) << scratch;
  Addiu32(scratch.AsCoreRegister(), SP, fr_offs.Int32Value());
  StoreToOffset(kStoreWord, scratch.AsCoreRegister(),
                S1, thr_offs.Int32Value());
}

void MipsAssembler::StoreStackPointerToThread32(ThreadOffset<kMipsWordSize> thr_offs) {
  StoreToOffset(kStoreWord, SP, S1, thr_offs.Int32Value());
}

void MipsAssembler::StoreSpanning(FrameOffset dest, ManagedRegister msrc,
                                  FrameOffset in_off, ManagedRegister mscratch) {
  MipsManagedRegister src = msrc.AsMips();
  MipsManagedRegister scratch = mscratch.AsMips();
  StoreToOffset(kStoreWord, src.AsCoreRegister(), SP, dest.Int32Value());
  LoadFromOffset(kLoadWord, scratch.AsCoreRegister(), SP, in_off.Int32Value());
  StoreToOffset(kStoreWord, scratch.AsCoreRegister(), SP, dest.Int32Value() + kMipsWordSize);
}

void MipsAssembler::Load(ManagedRegister mdest, FrameOffset src, size_t size) {
  return EmitLoad(mdest, SP, src.Int32Value(), size);
}

void MipsAssembler::LoadFromThread32(ManagedRegister mdest,
                                     ThreadOffset<kMipsWordSize> src, size_t size) {
  return EmitLoad(mdest, S1, src.Int32Value(), size);
}

void MipsAssembler::LoadRef(ManagedRegister mdest, FrameOffset src) {
  MipsManagedRegister dest = mdest.AsMips();
  CHECK(dest.IsCoreRegister());
  LoadFromOffset(kLoadWord, dest.AsCoreRegister(), SP, src.Int32Value());
}

void MipsAssembler::LoadRef(ManagedRegister mdest, ManagedRegister base, MemberOffset offs,
                            bool unpoison_reference) {
  MipsManagedRegister dest = mdest.AsMips();
  CHECK(dest.IsCoreRegister() && base.AsMips().IsCoreRegister());
  LoadFromOffset(kLoadWord, dest.AsCoreRegister(),
                 base.AsMips().AsCoreRegister(), offs.Int32Value());
  if (kPoisonHeapReferences && unpoison_reference) {
    Subu(dest.AsCoreRegister(), ZERO, dest.AsCoreRegister());
  }
}

void MipsAssembler::LoadRawPtr(ManagedRegister mdest, ManagedRegister base, Offset offs) {
  MipsManagedRegister dest = mdest.AsMips();
  CHECK(dest.IsCoreRegister() && base.AsMips().IsCoreRegister());
  LoadFromOffset(kLoadWord, dest.AsCoreRegister(),
                 base.AsMips().AsCoreRegister(), offs.Int32Value());
}

void MipsAssembler::LoadRawPtrFromThread32(ManagedRegister mdest,
                                           ThreadOffset<kMipsWordSize> offs) {
  MipsManagedRegister dest = mdest.AsMips();
  CHECK(dest.IsCoreRegister());
  LoadFromOffset(kLoadWord, dest.AsCoreRegister(), S1, offs.Int32Value());
}

void MipsAssembler::SignExtend(ManagedRegister /*mreg*/, size_t /*size*/) {
  UNIMPLEMENTED(FATAL) << "no sign extension necessary for mips";
}

void MipsAssembler::ZeroExtend(ManagedRegister /*mreg*/, size_t /*size*/) {
  UNIMPLEMENTED(FATAL) << "no zero extension necessary for mips";
}

void MipsAssembler::Move(ManagedRegister mdest, ManagedRegister msrc, size_t size) {
  MipsManagedRegister dest = mdest.AsMips();
  MipsManagedRegister src = msrc.AsMips();
  if (!dest.Equals(src)) {
    if (dest.IsCoreRegister()) {
      CHECK(src.IsCoreRegister()) << src;
      Move(dest.AsCoreRegister(), src.AsCoreRegister());
    } else if (dest.IsFRegister()) {
      CHECK(src.IsFRegister()) << src;
      if (size == kMipsWordSize) {
        MovS(dest.AsFRegister(), src.AsFRegister());
      } else {
        CHECK_EQ(kMipsDoublewordSize, size);
        MovD(dest.AsFRegister(), src.AsFRegister());
      }
    } else if (dest.IsDRegister()) {
      CHECK(src.IsDRegister()) << src;
      MovD(dest.AsOverlappingDRegisterLow(), src.AsOverlappingDRegisterLow());
    } else {
      CHECK(dest.IsRegisterPair()) << dest;
      CHECK(src.IsRegisterPair()) << src;
      // Ensure that the first move doesn't clobber the input of the second.
      if (src.AsRegisterPairHigh() != dest.AsRegisterPairLow()) {
        Move(dest.AsRegisterPairLow(), src.AsRegisterPairLow());
        Move(dest.AsRegisterPairHigh(), src.AsRegisterPairHigh());
      } else {
        Move(dest.AsRegisterPairHigh(), src.AsRegisterPairHigh());
        Move(dest.AsRegisterPairLow(), src.AsRegisterPairLow());
      }
    }
  }
}

void MipsAssembler::CopyRef(FrameOffset dest, FrameOffset src, ManagedRegister mscratch) {
  MipsManagedRegister scratch = mscratch.AsMips();
  CHECK(scratch.IsCoreRegister()) << scratch;
  LoadFromOffset(kLoadWord, scratch.AsCoreRegister(), SP, src.Int32Value());
  StoreToOffset(kStoreWord, scratch.AsCoreRegister(), SP, dest.Int32Value());
}

void MipsAssembler::CopyRawPtrFromThread32(FrameOffset fr_offs,
                                           ThreadOffset<kMipsWordSize> thr_offs,
                                           ManagedRegister mscratch) {
  MipsManagedRegister scratch = mscratch.AsMips();
  CHECK(scratch.IsCoreRegister()) << scratch;
  LoadFromOffset(kLoadWord, scratch.AsCoreRegister(),
                 S1, thr_offs.Int32Value());
  StoreToOffset(kStoreWord, scratch.AsCoreRegister(),
                SP, fr_offs.Int32Value());
}

void MipsAssembler::CopyRawPtrToThread32(ThreadOffset<kMipsWordSize> thr_offs,
                                         FrameOffset fr_offs,
                                         ManagedRegister mscratch) {
  MipsManagedRegister scratch = mscratch.AsMips();
  CHECK(scratch.IsCoreRegister()) << scratch;
  LoadFromOffset(kLoadWord, scratch.AsCoreRegister(),
                 SP, fr_offs.Int32Value());
  StoreToOffset(kStoreWord, scratch.AsCoreRegister(),
                S1, thr_offs.Int32Value());
}

void MipsAssembler::Copy(FrameOffset dest, FrameOffset src, ManagedRegister mscratch, size_t size) {
  MipsManagedRegister scratch = mscratch.AsMips();
  CHECK(scratch.IsCoreRegister()) << scratch;
  CHECK(size == kMipsWordSize || size == kMipsDoublewordSize) << size;
  if (size == kMipsWordSize) {
    LoadFromOffset(kLoadWord, scratch.AsCoreRegister(), SP, src.Int32Value());
    StoreToOffset(kStoreWord, scratch.AsCoreRegister(), SP, dest.Int32Value());
  } else if (size == kMipsDoublewordSize) {
    LoadFromOffset(kLoadWord, scratch.AsCoreRegister(), SP, src.Int32Value());
    StoreToOffset(kStoreWord, scratch.AsCoreRegister(), SP, dest.Int32Value());
    LoadFromOffset(kLoadWord, scratch.AsCoreRegister(), SP, src.Int32Value() + kMipsWordSize);
    StoreToOffset(kStoreWord, scratch.AsCoreRegister(), SP, dest.Int32Value() + kMipsWordSize);
  }
}

void MipsAssembler::Copy(FrameOffset dest, ManagedRegister src_base, Offset src_offset,
                         ManagedRegister mscratch, size_t size) {
  Register scratch = mscratch.AsMips().AsCoreRegister();
  CHECK_EQ(size, kMipsWordSize);
  LoadFromOffset(kLoadWord, scratch, src_base.AsMips().AsCoreRegister(), src_offset.Int32Value());
  StoreToOffset(kStoreWord, scratch, SP, dest.Int32Value());
}

void MipsAssembler::Copy(ManagedRegister dest_base, Offset dest_offset, FrameOffset src,
                         ManagedRegister mscratch, size_t size) {
  Register scratch = mscratch.AsMips().AsCoreRegister();
  CHECK_EQ(size, kMipsWordSize);
  LoadFromOffset(kLoadWord, scratch, SP, src.Int32Value());
  StoreToOffset(kStoreWord, scratch, dest_base.AsMips().AsCoreRegister(), dest_offset.Int32Value());
}

void MipsAssembler::Copy(FrameOffset dest ATTRIBUTE_UNUSED,
                         FrameOffset src_base ATTRIBUTE_UNUSED,
                         Offset src_offset ATTRIBUTE_UNUSED,
                         ManagedRegister mscratch ATTRIBUTE_UNUSED,
                         size_t size ATTRIBUTE_UNUSED) {
  UNIMPLEMENTED(FATAL) << "no MIPS implementation";
}

void MipsAssembler::Copy(ManagedRegister dest, Offset dest_offset,
                         ManagedRegister src, Offset src_offset,
                         ManagedRegister mscratch, size_t size) {
  CHECK_EQ(size, kMipsWordSize);
  Register scratch = mscratch.AsMips().AsCoreRegister();
  LoadFromOffset(kLoadWord, scratch, src.AsMips().AsCoreRegister(), src_offset.Int32Value());
  StoreToOffset(kStoreWord, scratch, dest.AsMips().AsCoreRegister(), dest_offset.Int32Value());
}

void MipsAssembler::Copy(FrameOffset dest ATTRIBUTE_UNUSED,
                         Offset dest_offset ATTRIBUTE_UNUSED,
                         FrameOffset src ATTRIBUTE_UNUSED,
                         Offset src_offset ATTRIBUTE_UNUSED,
                         ManagedRegister mscratch ATTRIBUTE_UNUSED,
                         size_t size ATTRIBUTE_UNUSED) {
  UNIMPLEMENTED(FATAL) << "no MIPS implementation";
}

void MipsAssembler::MemoryBarrier(ManagedRegister) {
  // TODO: sync?
  UNIMPLEMENTED(FATAL) << "no MIPS implementation";
}

void MipsAssembler::CreateHandleScopeEntry(ManagedRegister mout_reg,
                                           FrameOffset handle_scope_offset,
                                           ManagedRegister min_reg,
                                           bool null_allowed) {
  MipsManagedRegister out_reg = mout_reg.AsMips();
  MipsManagedRegister in_reg = min_reg.AsMips();
  CHECK(in_reg.IsNoRegister() || in_reg.IsCoreRegister()) << in_reg;
  CHECK(out_reg.IsCoreRegister()) << out_reg;
  if (null_allowed) {
    MipsLabel null_arg;
    // Null values get a handle scope entry value of 0.  Otherwise, the handle scope entry is
    // the address in the handle scope holding the reference.
    // E.g. out_reg = (handle == 0) ? 0 : (SP+handle_offset).
    if (in_reg.IsNoRegister()) {
      LoadFromOffset(kLoadWord, out_reg.AsCoreRegister(),
                     SP, handle_scope_offset.Int32Value());
      in_reg = out_reg;
    }
    if (!out_reg.Equals(in_reg)) {
      LoadConst32(out_reg.AsCoreRegister(), 0);
    }
    Beqz(in_reg.AsCoreRegister(), &null_arg);
    Addiu32(out_reg.AsCoreRegister(), SP, handle_scope_offset.Int32Value());
    Bind(&null_arg);
  } else {
    Addiu32(out_reg.AsCoreRegister(), SP, handle_scope_offset.Int32Value());
  }
}

void MipsAssembler::CreateHandleScopeEntry(FrameOffset out_off,
                                           FrameOffset handle_scope_offset,
                                           ManagedRegister mscratch,
                                           bool null_allowed) {
  MipsManagedRegister scratch = mscratch.AsMips();
  CHECK(scratch.IsCoreRegister()) << scratch;
  if (null_allowed) {
    MipsLabel null_arg;
    LoadFromOffset(kLoadWord, scratch.AsCoreRegister(), SP, handle_scope_offset.Int32Value());
    // Null values get a handle scope entry value of 0.  Otherwise, the handle scope entry is
    // the address in the handle scope holding the reference.
    // E.g. scratch = (scratch == 0) ? 0 : (SP+handle_scope_offset).
    Beqz(scratch.AsCoreRegister(), &null_arg);
    Addiu32(scratch.AsCoreRegister(), SP, handle_scope_offset.Int32Value());
    Bind(&null_arg);
  } else {
    Addiu32(scratch.AsCoreRegister(), SP, handle_scope_offset.Int32Value());
  }
  StoreToOffset(kStoreWord, scratch.AsCoreRegister(), SP, out_off.Int32Value());
}

// Given a handle scope entry, load the associated reference.
void MipsAssembler::LoadReferenceFromHandleScope(ManagedRegister mout_reg,
                                                 ManagedRegister min_reg) {
  MipsManagedRegister out_reg = mout_reg.AsMips();
  MipsManagedRegister in_reg = min_reg.AsMips();
  CHECK(out_reg.IsCoreRegister()) << out_reg;
  CHECK(in_reg.IsCoreRegister()) << in_reg;
  MipsLabel null_arg;
  if (!out_reg.Equals(in_reg)) {
    LoadConst32(out_reg.AsCoreRegister(), 0);
  }
  Beqz(in_reg.AsCoreRegister(), &null_arg);
  LoadFromOffset(kLoadWord, out_reg.AsCoreRegister(),
                 in_reg.AsCoreRegister(), 0);
  Bind(&null_arg);
}

void MipsAssembler::VerifyObject(ManagedRegister src ATTRIBUTE_UNUSED,
                                 bool could_be_null ATTRIBUTE_UNUSED) {
  // TODO: not validating references.
}

void MipsAssembler::VerifyObject(FrameOffset src ATTRIBUTE_UNUSED,
                                 bool could_be_null ATTRIBUTE_UNUSED) {
  // TODO: not validating references.
}

void MipsAssembler::Call(ManagedRegister mbase, Offset offset, ManagedRegister mscratch) {
  MipsManagedRegister base = mbase.AsMips();
  MipsManagedRegister scratch = mscratch.AsMips();
  CHECK(base.IsCoreRegister()) << base;
  CHECK(scratch.IsCoreRegister()) << scratch;
  LoadFromOffset(kLoadWord, scratch.AsCoreRegister(),
                 base.AsCoreRegister(), offset.Int32Value());
  Jalr(scratch.AsCoreRegister());
  Nop();
  // TODO: place reference map on call.
}

void MipsAssembler::Call(FrameOffset base, Offset offset, ManagedRegister mscratch) {
  MipsManagedRegister scratch = mscratch.AsMips();
  CHECK(scratch.IsCoreRegister()) << scratch;
  // Call *(*(SP + base) + offset)
  LoadFromOffset(kLoadWord, scratch.AsCoreRegister(), SP, base.Int32Value());
  LoadFromOffset(kLoadWord, scratch.AsCoreRegister(),
                 scratch.AsCoreRegister(), offset.Int32Value());
  Jalr(scratch.AsCoreRegister());
  Nop();
  // TODO: place reference map on call.
}

void MipsAssembler::CallFromThread32(ThreadOffset<kMipsWordSize> offset ATTRIBUTE_UNUSED,
                                     ManagedRegister mscratch ATTRIBUTE_UNUSED) {
  UNIMPLEMENTED(FATAL) << "no mips implementation";
}

void MipsAssembler::GetCurrentThread(ManagedRegister tr) {
  Move(tr.AsMips().AsCoreRegister(), S1);
}

void MipsAssembler::GetCurrentThread(FrameOffset offset,
                                     ManagedRegister mscratch ATTRIBUTE_UNUSED) {
  StoreToOffset(kStoreWord, S1, SP, offset.Int32Value());
}

void MipsAssembler::ExceptionPoll(ManagedRegister mscratch, size_t stack_adjust) {
  MipsManagedRegister scratch = mscratch.AsMips();
  exception_blocks_.emplace_back(scratch, stack_adjust);
  LoadFromOffset(kLoadWord, scratch.AsCoreRegister(),
                 S1, Thread::ExceptionOffset<kMipsWordSize>().Int32Value());
  // TODO: on MIPS32R6 prefer Bnezc(scratch.AsCoreRegister(), slow.Entry());
  // as the NAL instruction (occurring in long R2 branches) may become deprecated.
  // For now use common for R2 and R6 instructions as this code must execute on both.
  Bnez(scratch.AsCoreRegister(), exception_blocks_.back().Entry());
}

void MipsAssembler::EmitExceptionPoll(MipsExceptionSlowPath* exception) {
  Bind(exception->Entry());
  if (exception->stack_adjust_ != 0) {  // Fix up the frame.
    DecreaseFrameSize(exception->stack_adjust_);
  }
  // Pass exception object as argument.
  // Don't care about preserving A0 as this call won't return.
  CheckEntrypointTypes<kQuickDeliverException, void, mirror::Object*>();
  Move(A0, exception->scratch_.AsCoreRegister());
  // Set up call to Thread::Current()->pDeliverException.
  LoadFromOffset(kLoadWord, T9, S1,
    QUICK_ENTRYPOINT_OFFSET(kMipsWordSize, pDeliverException).Int32Value());
  Jr(T9);
  Nop();

  // Call never returns.
  Break();
}

}  // namespace mips
}  // namespace art
