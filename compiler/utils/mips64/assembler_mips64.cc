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

void Mips64Assembler::Emit(uint32_t value) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  buffer_.Emit<uint32_t>(value);
}

void Mips64Assembler::EmitR(int opcode, GpuRegister rs, GpuRegister rt, GpuRegister rd,
                            int shamt, int funct) {
  CHECK_NE(rs, kNoGpuRegister);
  CHECK_NE(rt, kNoGpuRegister);
  CHECK_NE(rd, kNoGpuRegister);
  uint32_t encoding = static_cast<uint32_t>(opcode) << kOpcodeShift |
                      static_cast<uint32_t>(rs) << kRsShift |
                      static_cast<uint32_t>(rt) << kRtShift |
                      static_cast<uint32_t>(rd) << kRdShift |
                      shamt << kShamtShift |
                      funct;
  Emit(encoding);
}

void Mips64Assembler::EmitI(int opcode, GpuRegister rs, GpuRegister rt, uint16_t imm) {
  CHECK_NE(rs, kNoGpuRegister);
  CHECK_NE(rt, kNoGpuRegister);
  uint32_t encoding = static_cast<uint32_t>(opcode) << kOpcodeShift |
                      static_cast<uint32_t>(rs) << kRsShift |
                      static_cast<uint32_t>(rt) << kRtShift |
                      imm;
  Emit(encoding);
}

void Mips64Assembler::EmitI21(int opcode, GpuRegister rs, uint32_t imm21) {
  CHECK_NE(rs, kNoGpuRegister);
  uint32_t encoding = static_cast<uint32_t>(opcode) << kOpcodeShift |
                      static_cast<uint32_t>(rs) << kRsShift |
                      (imm21 & 0x1FFFFF);
  Emit(encoding);
}

void Mips64Assembler::EmitJ(int opcode, uint32_t addr26) {
  uint32_t encoding = static_cast<uint32_t>(opcode) << kOpcodeShift |
                      (addr26 & 0x3FFFFFF);
  Emit(encoding);
}

void Mips64Assembler::EmitFR(int opcode, int fmt, FpuRegister ft, FpuRegister fs, FpuRegister fd,
                             int funct) {
  CHECK_NE(ft, kNoFpuRegister);
  CHECK_NE(fs, kNoFpuRegister);
  CHECK_NE(fd, kNoFpuRegister);
  uint32_t encoding = static_cast<uint32_t>(opcode) << kOpcodeShift |
                      fmt << kFmtShift |
                      static_cast<uint32_t>(ft) << kFtShift |
                      static_cast<uint32_t>(fs) << kFsShift |
                      static_cast<uint32_t>(fd) << kFdShift |
                      funct;
  Emit(encoding);
}

void Mips64Assembler::EmitFI(int opcode, int fmt, FpuRegister ft, uint16_t imm) {
  CHECK_NE(ft, kNoFpuRegister);
  uint32_t encoding = static_cast<uint32_t>(opcode) << kOpcodeShift |
                      fmt << kFmtShift |
                      static_cast<uint32_t>(ft) << kFtShift |
                      imm;
  Emit(encoding);
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

void Mips64Assembler::Daddu(GpuRegister rd, GpuRegister rs, GpuRegister rt) {
  EmitR(0, rs, rt, rd, 0, 0x2d);
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

void Mips64Assembler::Dsubu(GpuRegister rd, GpuRegister rs, GpuRegister rt) {
  EmitR(0, rs, rt, rd, 0, 0x2f);
}

void Mips64Assembler::MultR2(GpuRegister rs, GpuRegister rt) {
  EmitR(0, rs, rt, static_cast<GpuRegister>(0), 0, 0x18);
}

void Mips64Assembler::MultuR2(GpuRegister rs, GpuRegister rt) {
  EmitR(0, rs, rt, static_cast<GpuRegister>(0), 0, 0x19);
}

void Mips64Assembler::DivR2(GpuRegister rs, GpuRegister rt) {
  EmitR(0, rs, rt, static_cast<GpuRegister>(0), 0, 0x1a);
}

void Mips64Assembler::DivuR2(GpuRegister rs, GpuRegister rt) {
  EmitR(0, rs, rt, static_cast<GpuRegister>(0), 0, 0x1b);
}

void Mips64Assembler::MulR2(GpuRegister rd, GpuRegister rs, GpuRegister rt) {
  EmitR(0x1c, rs, rt, rd, 0, 2);
}

void Mips64Assembler::DivR2(GpuRegister rd, GpuRegister rs, GpuRegister rt) {
  DivR2(rs, rt);
  Mflo(rd);
}

void Mips64Assembler::ModR2(GpuRegister rd, GpuRegister rs, GpuRegister rt) {
  DivR2(rs, rt);
  Mfhi(rd);
}

void Mips64Assembler::DivuR2(GpuRegister rd, GpuRegister rs, GpuRegister rt) {
  DivuR2(rs, rt);
  Mflo(rd);
}

void Mips64Assembler::ModuR2(GpuRegister rd, GpuRegister rs, GpuRegister rt) {
  DivuR2(rs, rt);
  Mfhi(rd);
}

void Mips64Assembler::MulR6(GpuRegister rd, GpuRegister rs, GpuRegister rt) {
  EmitR(0, rs, rt, rd, 2, 0x18);
}

void Mips64Assembler::DivR6(GpuRegister rd, GpuRegister rs, GpuRegister rt) {
  EmitR(0, rs, rt, rd, 2, 0x1a);
}

void Mips64Assembler::ModR6(GpuRegister rd, GpuRegister rs, GpuRegister rt) {
  EmitR(0, rs, rt, rd, 3, 0x1a);
}

void Mips64Assembler::DivuR6(GpuRegister rd, GpuRegister rs, GpuRegister rt) {
  EmitR(0, rs, rt, rd, 2, 0x1b);
}

void Mips64Assembler::ModuR6(GpuRegister rd, GpuRegister rs, GpuRegister rt) {
  EmitR(0, rs, rt, rd, 3, 0x1b);
}

void Mips64Assembler::Dmul(GpuRegister rd, GpuRegister rs, GpuRegister rt) {
  EmitR(0, rs, rt, rd, 2, 0x1c);
}

void Mips64Assembler::Ddiv(GpuRegister rd, GpuRegister rs, GpuRegister rt) {
  EmitR(0, rs, rt, rd, 2, 0x1e);
}

void Mips64Assembler::Dmod(GpuRegister rd, GpuRegister rs, GpuRegister rt) {
  EmitR(0, rs, rt, rd, 3, 0x1e);
}

void Mips64Assembler::Ddivu(GpuRegister rd, GpuRegister rs, GpuRegister rt) {
  EmitR(0, rs, rt, rd, 2, 0x1f);
}

void Mips64Assembler::Dmodu(GpuRegister rd, GpuRegister rs, GpuRegister rt) {
  EmitR(0, rs, rt, rd, 3, 0x1f);
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

void Mips64Assembler::Seb(GpuRegister rd, GpuRegister rt) {
  EmitR(0x1f, static_cast<GpuRegister>(0), rt, rd, 0x10, 0x20);
}

void Mips64Assembler::Seh(GpuRegister rd, GpuRegister rt) {
  EmitR(0x1f, static_cast<GpuRegister>(0), rt, rd, 0x18, 0x20);
}

void Mips64Assembler::Dext(GpuRegister rt, GpuRegister rs, int pos, int size_less_one) {
  DCHECK(0 <= pos && pos < 32) << pos;
  DCHECK(0 <= size_less_one && size_less_one < 32) << size_less_one;
  EmitR(0x1f, rs, rt, static_cast<GpuRegister>(size_less_one), pos, 3);
}

void Mips64Assembler::Sll(GpuRegister rd, GpuRegister rt, int shamt) {
  EmitR(0, static_cast<GpuRegister>(0), rt, rd, shamt, 0x00);
}

void Mips64Assembler::Srl(GpuRegister rd, GpuRegister rt, int shamt) {
  EmitR(0, static_cast<GpuRegister>(0), rt, rd, shamt, 0x02);
}

void Mips64Assembler::Sra(GpuRegister rd, GpuRegister rt, int shamt) {
  EmitR(0, static_cast<GpuRegister>(0), rt, rd, shamt, 0x03);
}

void Mips64Assembler::Sllv(GpuRegister rd, GpuRegister rt, GpuRegister rs) {
  EmitR(0, rs, rt, rd, 0, 0x04);
}

void Mips64Assembler::Srlv(GpuRegister rd, GpuRegister rt, GpuRegister rs) {
  EmitR(0, rs, rt, rd, 0, 0x06);
}

void Mips64Assembler::Srav(GpuRegister rd, GpuRegister rt, GpuRegister rs) {
  EmitR(0, rs, rt, rd, 0, 0x07);
}

void Mips64Assembler::Dsll(GpuRegister rd, GpuRegister rt, int shamt) {
  EmitR(0, static_cast<GpuRegister>(0), rt, rd, shamt, 0x38);
}

void Mips64Assembler::Dsrl(GpuRegister rd, GpuRegister rt, int shamt) {
  EmitR(0, static_cast<GpuRegister>(0), rt, rd, shamt, 0x3a);
}

void Mips64Assembler::Dsra(GpuRegister rd, GpuRegister rt, int shamt) {
  EmitR(0, static_cast<GpuRegister>(0), rt, rd, shamt, 0x3b);
}

void Mips64Assembler::Dsll32(GpuRegister rd, GpuRegister rt, int shamt) {
  EmitR(0, static_cast<GpuRegister>(0), rt, rd, shamt, 0x3c);
}

void Mips64Assembler::Dsrl32(GpuRegister rd, GpuRegister rt, int shamt) {
  EmitR(0, static_cast<GpuRegister>(0), rt, rd, shamt, 0x3e);
}

void Mips64Assembler::Dsra32(GpuRegister rd, GpuRegister rt, int shamt) {
  EmitR(0, static_cast<GpuRegister>(0), rt, rd, shamt, 0x3f);
}

void Mips64Assembler::Dsllv(GpuRegister rd, GpuRegister rt, GpuRegister rs) {
  EmitR(0, rs, rt, rd, 0, 0x14);
}

void Mips64Assembler::Dsrlv(GpuRegister rd, GpuRegister rt, GpuRegister rs) {
  EmitR(0, rs, rt, rd, 0, 0x16);
}

void Mips64Assembler::Dsrav(GpuRegister rd, GpuRegister rt, GpuRegister rs) {
  EmitR(0, rs, rt, rd, 0, 0x17);
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

void Mips64Assembler::Dahi(GpuRegister rs, uint16_t imm16) {
  EmitI(1, rs, static_cast<GpuRegister>(6), imm16);
}

void Mips64Assembler::Dati(GpuRegister rs, uint16_t imm16) {
  EmitI(1, rs, static_cast<GpuRegister>(0x1e), imm16);
}

void Mips64Assembler::Sync(uint32_t stype) {
  EmitR(0, static_cast<GpuRegister>(0), static_cast<GpuRegister>(0),
           static_cast<GpuRegister>(0), stype & 0x1f, 0xf);
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

void Mips64Assembler::Beq(GpuRegister rs, GpuRegister rt, uint16_t imm16) {
  EmitI(0x4, rs, rt, imm16);
  Nop();
}

void Mips64Assembler::Bne(GpuRegister rs, GpuRegister rt, uint16_t imm16) {
  EmitI(0x5, rs, rt, imm16);
  Nop();
}

void Mips64Assembler::J(uint32_t addr26) {
  EmitJ(0x2, addr26);
  Nop();
}

void Mips64Assembler::Jal(uint32_t addr26) {
  EmitJ(0x3, addr26);
  Nop();
}

void Mips64Assembler::Jalr(GpuRegister rd, GpuRegister rs) {
  EmitR(0, rs, static_cast<GpuRegister>(0), rd, 0, 0x09);
  Nop();
}

void Mips64Assembler::Jalr(GpuRegister rs) {
  Jalr(RA, rs);
}

void Mips64Assembler::Jr(GpuRegister rs) {
  Jalr(ZERO, rs);
}

void Mips64Assembler::Auipc(GpuRegister rs, uint16_t imm16) {
  EmitI(0x3B, rs, static_cast<GpuRegister>(0x1E), imm16);
}

void Mips64Assembler::Jic(GpuRegister rt, uint16_t imm16) {
  EmitI(0x36, static_cast<GpuRegister>(0), rt, imm16);
}

void Mips64Assembler::Jialc(GpuRegister rt, uint16_t imm16) {
  EmitI(0x3E, static_cast<GpuRegister>(0), rt, imm16);
}

void Mips64Assembler::Bltc(GpuRegister rs, GpuRegister rt, uint16_t imm16) {
  CHECK_NE(rs, ZERO);
  CHECK_NE(rt, ZERO);
  CHECK_NE(rs, rt);
  EmitI(0x17, rs, rt, imm16);
}

void Mips64Assembler::Bltzc(GpuRegister rt, uint16_t imm16) {
  CHECK_NE(rt, ZERO);
  EmitI(0x17, rt, rt, imm16);
}

void Mips64Assembler::Bgtzc(GpuRegister rt, uint16_t imm16) {
  CHECK_NE(rt, ZERO);
  EmitI(0x17, static_cast<GpuRegister>(0), rt, imm16);
}

void Mips64Assembler::Bgec(GpuRegister rs, GpuRegister rt, uint16_t imm16) {
  CHECK_NE(rs, ZERO);
  CHECK_NE(rt, ZERO);
  CHECK_NE(rs, rt);
  EmitI(0x16, rs, rt, imm16);
}

void Mips64Assembler::Bgezc(GpuRegister rt, uint16_t imm16) {
  CHECK_NE(rt, ZERO);
  EmitI(0x16, rt, rt, imm16);
}

void Mips64Assembler::Blezc(GpuRegister rt, uint16_t imm16) {
  CHECK_NE(rt, ZERO);
  EmitI(0x16, static_cast<GpuRegister>(0), rt, imm16);
}

void Mips64Assembler::Bltuc(GpuRegister rs, GpuRegister rt, uint16_t imm16) {
  CHECK_NE(rs, ZERO);
  CHECK_NE(rt, ZERO);
  CHECK_NE(rs, rt);
  EmitI(0x7, rs, rt, imm16);
}

void Mips64Assembler::Bgeuc(GpuRegister rs, GpuRegister rt, uint16_t imm16) {
  CHECK_NE(rs, ZERO);
  CHECK_NE(rt, ZERO);
  CHECK_NE(rs, rt);
  EmitI(0x6, rs, rt, imm16);
}

void Mips64Assembler::Beqc(GpuRegister rs, GpuRegister rt, uint16_t imm16) {
  CHECK_NE(rs, ZERO);
  CHECK_NE(rt, ZERO);
  CHECK_NE(rs, rt);
  EmitI(0x8, (rs < rt) ? rs : rt, (rs < rt) ? rt : rs, imm16);
}

void Mips64Assembler::Bnec(GpuRegister rs, GpuRegister rt, uint16_t imm16) {
  CHECK_NE(rs, ZERO);
  CHECK_NE(rt, ZERO);
  CHECK_NE(rs, rt);
  EmitI(0x18, (rs < rt) ? rs : rt, (rs < rt) ? rt : rs, imm16);
}

void Mips64Assembler::Beqzc(GpuRegister rs, uint32_t imm21) {
  CHECK_NE(rs, ZERO);
  EmitI21(0x36, rs, imm21);
}

void Mips64Assembler::Bnezc(GpuRegister rs, uint32_t imm21) {
  CHECK_NE(rs, ZERO);
  EmitI21(0x3E, rs, imm21);
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
  EmitFR(0x11, 0x11, ft, fs, fd, 0x0);
}

void Mips64Assembler::SubD(FpuRegister fd, FpuRegister fs, FpuRegister ft) {
  EmitFR(0x11, 0x11, ft, fs, fd, 0x1);
}

void Mips64Assembler::MulD(FpuRegister fd, FpuRegister fs, FpuRegister ft) {
  EmitFR(0x11, 0x11, ft, fs, fd, 0x2);
}

void Mips64Assembler::DivD(FpuRegister fd, FpuRegister fs, FpuRegister ft) {
  EmitFR(0x11, 0x11, ft, fs, fd, 0x3);
}

void Mips64Assembler::MovS(FpuRegister fd, FpuRegister fs) {
  EmitFR(0x11, 0x10, static_cast<FpuRegister>(0), fs, fd, 0x6);
}

void Mips64Assembler::MovD(FpuRegister fd, FpuRegister fs) {
  EmitFR(0x11, 0x11, static_cast<FpuRegister>(0), fs, fd, 0x6);
}

void Mips64Assembler::NegS(FpuRegister fd, FpuRegister fs) {
  EmitFR(0x11, 0x10, static_cast<FpuRegister>(0), fs, fd, 0x7);
}

void Mips64Assembler::NegD(FpuRegister fd, FpuRegister fs) {
  EmitFR(0x11, 0x11, static_cast<FpuRegister>(0), fs, fd, 0x7);
}

void Mips64Assembler::Cvtsw(FpuRegister fd, FpuRegister fs) {
  EmitFR(0x11, 0x14, static_cast<FpuRegister>(0), fs, fd, 0x20);
}

void Mips64Assembler::Cvtdw(FpuRegister fd, FpuRegister fs) {
  EmitFR(0x11, 0x14, static_cast<FpuRegister>(0), fs, fd, 0x21);
}

void Mips64Assembler::Cvtsd(FpuRegister fd, FpuRegister fs) {
  EmitFR(0x11, 0x11, static_cast<FpuRegister>(0), fs, fd, 0x20);
}

void Mips64Assembler::Cvtds(FpuRegister fd, FpuRegister fs) {
  EmitFR(0x11, 0x10, static_cast<FpuRegister>(0), fs, fd, 0x21);
}

void Mips64Assembler::Mfc1(GpuRegister rt, FpuRegister fs) {
  EmitFR(0x11, 0x00, static_cast<FpuRegister>(rt), fs, static_cast<FpuRegister>(0), 0x0);
}

void Mips64Assembler::Mtc1(GpuRegister rt, FpuRegister fs) {
  EmitFR(0x11, 0x04, static_cast<FpuRegister>(rt), fs, static_cast<FpuRegister>(0), 0x0);
}

void Mips64Assembler::Dmfc1(GpuRegister rt, FpuRegister fs) {
  EmitFR(0x11, 0x01, static_cast<FpuRegister>(rt), fs, static_cast<FpuRegister>(0), 0x0);
}

void Mips64Assembler::Dmtc1(GpuRegister rt, FpuRegister fs) {
  EmitFR(0x11, 0x05, static_cast<FpuRegister>(rt), fs, static_cast<FpuRegister>(0), 0x0);
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

void Mips64Assembler::Move(GpuRegister rd, GpuRegister rs) {
  Or(rd, rs, ZERO);
}

void Mips64Assembler::Clear(GpuRegister rd) {
  Move(rd, ZERO);
}

void Mips64Assembler::Not(GpuRegister rd, GpuRegister rs) {
  Nor(rd, rs, ZERO);
}

void Mips64Assembler::LoadConst32(GpuRegister rd, int32_t value) {
  if (IsUint<16>(value)) {
    // Use OR with (unsigned) immediate to encode 16b unsigned int.
    Ori(rd, ZERO, value);
  } else if (IsInt<16>(value)) {
    // Use ADD with (signed) immediate to encode 16b signed int.
    Addiu(rd, ZERO, value);
  } else {
    Lui(rd, value >> 16);
    if (value & 0xFFFF)
      Ori(rd, rd, value);
  }
}

void Mips64Assembler::LoadConst64(GpuRegister rd, int64_t value) {
  int bit31 = (value & UINT64_C(0x80000000)) != 0;

  // Loads with 1 instruction.
  if (IsUint<16>(value)) {
    Ori(rd, ZERO, value);
  } else if (IsInt<16>(value)) {
    Daddiu(rd, ZERO, value);
  } else if ((value & 0xFFFF) == 0 && IsInt<16>(value >> 16)) {
    Lui(rd, value >> 16);
  } else if (IsInt<32>(value)) {
    // Loads with 2 instructions.
    Lui(rd, value >> 16);
    Ori(rd, rd, value);
  } else if ((value & 0xFFFF0000) == 0 && IsInt<16>(value >> 32)) {
    Ori(rd, ZERO, value);
    Dahi(rd, value >> 32);
  } else if ((value & UINT64_C(0xFFFFFFFF0000)) == 0) {
    Ori(rd, ZERO, value);
    Dati(rd, value >> 48);
  } else if ((value & 0xFFFF) == 0 &&
             (-32768 - bit31) <= (value >> 32) && (value >> 32) <= (32767 - bit31)) {
    Lui(rd, value >> 16);
    Dahi(rd, (value >> 32) + bit31);
  } else if ((value & 0xFFFF) == 0 && ((value >> 31) & 0x1FFFF) == ((0x20000 - bit31) & 0x1FFFF)) {
    Lui(rd, value >> 16);
    Dati(rd, (value >> 48) + bit31);
  } else {
    int shift_cnt = CTZ(value);
    int64_t tmp = value >> shift_cnt;
    if (IsUint<16>(tmp)) {
      Ori(rd, ZERO, tmp);
      if (shift_cnt < 32)
        Dsll(rd, rd, shift_cnt);
      else
        Dsll32(rd, rd, shift_cnt & 31);
    } else if (IsInt<16>(tmp)) {
      Daddiu(rd, ZERO, tmp);
      if (shift_cnt < 32)
        Dsll(rd, rd, shift_cnt);
      else
        Dsll32(rd, rd, shift_cnt & 31);
    } else if (IsInt<32>(tmp)) {
      // Loads with 3 instructions.
      Lui(rd, tmp >> 16);
      Ori(rd, rd, tmp);
      if (shift_cnt < 32)
        Dsll(rd, rd, shift_cnt);
      else
        Dsll32(rd, rd, shift_cnt & 31);
    } else {
      shift_cnt = 16 + CTZ(value >> 16);
      tmp = value >> shift_cnt;
      if (IsUint<16>(tmp)) {
        Ori(rd, ZERO, tmp);
        if (shift_cnt < 32)
          Dsll(rd, rd, shift_cnt);
        else
          Dsll32(rd, rd, shift_cnt & 31);
        Ori(rd, rd, value);
      } else if (IsInt<16>(tmp)) {
        Daddiu(rd, ZERO, tmp);
        if (shift_cnt < 32)
          Dsll(rd, rd, shift_cnt);
        else
          Dsll32(rd, rd, shift_cnt & 31);
        Ori(rd, rd, value);
      } else {
        // Loads with 3-4 instructions.
        uint64_t tmp2 = value;
        bool used_lui = false;
        if (((tmp2 >> 16) & 0xFFFF) != 0 || (tmp2 & 0xFFFFFFFF) == 0) {
          Lui(rd, tmp2 >> 16);
          used_lui = true;
        }
        if ((tmp2 & 0xFFFF) != 0) {
          if (used_lui)
            Ori(rd, rd, tmp2);
          else
            Ori(rd, ZERO, tmp2);
        }
        if (bit31) {
          tmp2 += UINT64_C(0x100000000);
        }
        if (((tmp2 >> 32) & 0xFFFF) != 0) {
          Dahi(rd, tmp2 >> 32);
        }
        if (tmp2 & UINT64_C(0x800000000000)) {
          tmp2 += UINT64_C(0x1000000000000);
        }
        if ((tmp2 >> 48) != 0) {
          Dati(rd, tmp2 >> 48);
        }
      }
    }
  }
}

void Mips64Assembler::Addiu32(GpuRegister rt, GpuRegister rs, int32_t value, GpuRegister rtmp) {
  if (IsInt<16>(value)) {
    Addiu(rt, rs, value);
  } else {
    LoadConst32(rtmp, value);
    Addu(rt, rs, rtmp);
  }
}

void Mips64Assembler::Daddiu64(GpuRegister rt, GpuRegister rs, int64_t value, GpuRegister rtmp) {
  if (IsInt<16>(value)) {
    Daddiu(rt, rs, value);
  } else {
    LoadConst64(rtmp, value);
    Daddu(rt, rs, rtmp);
  }
}

//
// MIPS64R6 branches
//
//
// Unconditional (pc + 32-bit signed offset):
//
//   auipc    at, ofs_high
//   jic      at, ofs_low
//   // no delay/forbidden slot
//
//
// Conditional (pc + 32-bit signed offset):
//
//   b<cond>c   reg, +2      // skip next 2 instructions
//   auipc      at, ofs_high
//   jic        at, ofs_low
//   // no delay/forbidden slot
//
//
// Unconditional (pc + 32-bit signed offset) and link:
//
//   auipc    reg, ofs_high
//   daddiu   reg, ofs_low
//   jialc    reg, 0
//   // no delay/forbidden slot
//
//
// TODO: use shorter instruction sequences whenever possible.
//

void Mips64Assembler::Bind(Label* label) {
  CHECK(!label->IsBound());
  int32_t bound_pc = buffer_.Size();

  // Walk the list of the branches (auipc + jic pairs) referring to and preceding this label.
  // Embed the previously unknown pc-relative addresses in them.
  while (label->IsLinked()) {
    int32_t position = label->Position();
    // Extract the branch (instruction pair)
    uint32_t auipc = buffer_.Load<uint32_t>(position);
    uint32_t jic = buffer_.Load<uint32_t>(position + 4);  // actually, jic or daddiu

    // Extract the location of the previous pair in the list (walking the list backwards;
    // the previous pair location was stored in the immediate operands of the instructions)
    int32_t prev = (auipc << 16) | (jic & 0xFFFF);

    // Get the pc-relative address
    uint32_t offset = bound_pc - position;
    offset += (offset & 0x8000) << 1;  // account for sign extension in jic/daddiu

    // Embed it in the two instructions
    auipc = (auipc & 0xFFFF0000) | (offset >> 16);
    jic = (jic & 0xFFFF0000) | (offset & 0xFFFF);

    // Save the adjusted instructions
    buffer_.Store<uint32_t>(position, auipc);
    buffer_.Store<uint32_t>(position + 4, jic);

    // On to the previous branch in the list...
    label->position_ = prev;
  }

  // Now make the label object contain its own location
  // (it will be used by the branches referring to and following this label)
  label->BindTo(bound_pc);
}

void Mips64Assembler::B(Label* label) {
  if (label->IsBound()) {
    // Branch backwards (to a preceding label), distance is known
    uint32_t offset = label->Position() - buffer_.Size();
    CHECK_LE(static_cast<int32_t>(offset), 0);
    offset += (offset & 0x8000) << 1;  // account for sign extension in jic
    Auipc(AT, offset >> 16);
    Jic(AT, offset);
  } else {
    // Branch forward (to a following label), distance is unknown
    int32_t position = buffer_.Size();
    // The first branch forward will have 0 in its pc-relative address (copied from label's
    // position). It will be the terminator of the list of forward-reaching branches.
    uint32_t prev = label->position_;
    Auipc(AT, prev >> 16);
    Jic(AT, prev);
    // Now make the link object point to the location of this branch
    // (this forms a linked list of branches preceding this label)
    label->LinkTo(position);
  }
}

void Mips64Assembler::Jalr(Label* label, GpuRegister indirect_reg) {
  if (label->IsBound()) {
    // Branch backwards (to a preceding label), distance is known
    uint32_t offset = label->Position() - buffer_.Size();
    CHECK_LE(static_cast<int32_t>(offset), 0);
    offset += (offset & 0x8000) << 1;  // account for sign extension in daddiu
    Auipc(indirect_reg, offset >> 16);
    Daddiu(indirect_reg, indirect_reg, offset);
    Jialc(indirect_reg, 0);
  } else {
    // Branch forward (to a following label), distance is unknown
    int32_t position = buffer_.Size();
    // The first branch forward will have 0 in its pc-relative address (copied from label's
    // position). It will be the terminator of the list of forward-reaching branches.
    uint32_t prev = label->position_;
    Auipc(indirect_reg, prev >> 16);
    Daddiu(indirect_reg, indirect_reg, prev);
    Jialc(indirect_reg, 0);
    // Now make the link object point to the location of this branch
    // (this forms a linked list of branches preceding this label)
    label->LinkTo(position);
  }
}

void Mips64Assembler::Bltc(GpuRegister rs, GpuRegister rt, Label* label) {
  Bgec(rs, rt, 2);
  B(label);
}

void Mips64Assembler::Bltzc(GpuRegister rt, Label* label) {
  Bgezc(rt, 2);
  B(label);
}

void Mips64Assembler::Bgtzc(GpuRegister rt, Label* label) {
  Blezc(rt, 2);
  B(label);
}

void Mips64Assembler::Bgec(GpuRegister rs, GpuRegister rt, Label* label) {
  Bltc(rs, rt, 2);
  B(label);
}

void Mips64Assembler::Bgezc(GpuRegister rt, Label* label) {
  Bltzc(rt, 2);
  B(label);
}

void Mips64Assembler::Blezc(GpuRegister rt, Label* label) {
  Bgtzc(rt, 2);
  B(label);
}

void Mips64Assembler::Bltuc(GpuRegister rs, GpuRegister rt, Label* label) {
  Bgeuc(rs, rt, 2);
  B(label);
}

void Mips64Assembler::Bgeuc(GpuRegister rs, GpuRegister rt, Label* label) {
  Bltuc(rs, rt, 2);
  B(label);
}

void Mips64Assembler::Beqc(GpuRegister rs, GpuRegister rt, Label* label) {
  Bnec(rs, rt, 2);
  B(label);
}

void Mips64Assembler::Bnec(GpuRegister rs, GpuRegister rt, Label* label) {
  Beqc(rs, rt, 2);
  B(label);
}

void Mips64Assembler::Beqzc(GpuRegister rs, Label* label) {
  Bnezc(rs, 2);
  B(label);
}

void Mips64Assembler::Bnezc(GpuRegister rs, Label* label) {
  Beqzc(rs, 2);
  B(label);
}

void Mips64Assembler::LoadFromOffset(LoadOperandType type, GpuRegister reg, GpuRegister base,
                                     int32_t offset) {
  if (!IsInt<16>(offset)) {
    LoadConst32(AT, offset);
    Daddu(AT, AT, base);
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
    case kLoadUnsignedWord:
      Lwu(reg, base, offset);
      break;
    case kLoadDoubleword:
      Ld(reg, base, offset);
      break;
  }
}

void Mips64Assembler::LoadFpuFromOffset(LoadOperandType type, FpuRegister reg, GpuRegister base,
                                        int32_t offset) {
  if (!IsInt<16>(offset)) {
    LoadConst32(AT, offset);
    Daddu(AT, AT, base);
    base = AT;
    offset = 0;
  }

  switch (type) {
    case kLoadWord:
      Lwc1(reg, base, offset);
      break;
    case kLoadDoubleword:
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
  if (!IsInt<16>(offset)) {
    LoadConst32(AT, offset);
    Daddu(AT, AT, base);
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
      Sd(reg, base, offset);
      break;
    default:
      LOG(FATAL) << "UNREACHABLE";
  }
}

void Mips64Assembler::StoreFpuToOffset(StoreOperandType type, FpuRegister reg, GpuRegister base,
                                       int32_t offset) {
  if (!IsInt<16>(offset)) {
    LoadConst32(AT, offset);
    Daddu(AT, AT, base);
    base = AT;
    offset = 0;
  }

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
  StoreToOffset(kStoreDoubleword, method_reg.AsMips64().AsGpuRegister(), SP, 0);

  // Write out entry spills.
  int32_t offset = frame_size + kFramePointerSize;
  for (size_t i = 0; i < entry_spills.size(); ++i) {
    Mips64ManagedRegister reg = entry_spills.at(i).AsMips64();
    ManagedRegisterSpill spill = entry_spills.at(i);
    int32_t size = spill.getSize();
    if (reg.IsNoRegister()) {
      // only increment stack offset.
      offset += size;
    } else if (reg.IsFpuRegister()) {
      StoreFpuToOffset((size == 4) ? kStoreWord : kStoreDoubleword,
          reg.AsFpuRegister(), SP, offset);
      offset += size;
    } else if (reg.IsGpuRegister()) {
      StoreToOffset((size == 4) ? kStoreWord : kStoreDoubleword,
          reg.AsGpuRegister(), SP, offset);
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
  CHECK_ALIGNED(adjust, kFramePointerSize);
  Daddiu64(SP, SP, static_cast<int32_t>(-adjust));
  cfi_.AdjustCFAOffset(adjust);
}

void Mips64Assembler::DecreaseFrameSize(size_t adjust) {
  CHECK_ALIGNED(adjust, kFramePointerSize);
  Daddiu64(SP, SP, static_cast<int32_t>(adjust));
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
  LoadConst32(scratch.AsGpuRegister(), imm);
  StoreToOffset(kStoreWord, scratch.AsGpuRegister(), SP, dest.Int32Value());
}

void Mips64Assembler::StoreImmediateToThread64(ThreadOffset<8> dest, uint32_t imm,
                                               ManagedRegister mscratch) {
  Mips64ManagedRegister scratch = mscratch.AsMips64();
  CHECK(scratch.IsGpuRegister()) << scratch;
  // TODO: it's unclear wether 32 or 64 bits need to be stored (Arm64 and x86/x64 disagree?).
  // Is this function even referenced anywhere else in the code?
  LoadConst32(scratch.AsGpuRegister(), imm);
  StoreToOffset(kStoreDoubleword, scratch.AsGpuRegister(), S1, dest.Int32Value());
}

void Mips64Assembler::StoreStackOffsetToThread64(ThreadOffset<8> thr_offs,
                                                 FrameOffset fr_offs,
                                                 ManagedRegister mscratch) {
  Mips64ManagedRegister scratch = mscratch.AsMips64();
  CHECK(scratch.IsGpuRegister()) << scratch;
  Daddiu64(scratch.AsGpuRegister(), SP, fr_offs.Int32Value());
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

void Mips64Assembler::LoadRef(ManagedRegister mdest, ManagedRegister base, MemberOffset offs,
                              bool poison_reference) {
  Mips64ManagedRegister dest = mdest.AsMips64();
  CHECK(dest.IsGpuRegister() && base.AsMips64().IsGpuRegister());
  LoadFromOffset(kLoadUnsignedWord, dest.AsGpuRegister(),
                 base.AsMips64().AsGpuRegister(), offs.Int32Value());
  if (kPoisonHeapReferences && poison_reference) {
    // TODO: review
    // Negate the 32-bit ref
    Dsubu(dest.AsGpuRegister(), ZERO, dest.AsGpuRegister());
    // And constrain it to 32 bits (zero-extend into bits 32 through 63) as on Arm64 and x86/64
    Dext(dest.AsGpuRegister(), dest.AsGpuRegister(), 0, 31);
  }
}

void Mips64Assembler::LoadRawPtr(ManagedRegister mdest, ManagedRegister base,
                                 Offset offs) {
  Mips64ManagedRegister dest = mdest.AsMips64();
  CHECK(dest.IsGpuRegister() && base.AsMips64().IsGpuRegister());
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
    StoreToOffset(kStoreDoubleword, scratch.AsGpuRegister(), SP, dest.Int32Value());
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
    StoreToOffset(kStoreDoubleword, scratch, SP, dest.Int32Value());
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
    StoreToOffset(kStoreDoubleword, scratch, dest_base.AsMips64().AsGpuRegister(),
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
    StoreToOffset(kStoreDoubleword, scratch, dest.AsMips64().AsGpuRegister(), dest_offset.Int32Value());
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
  // TODO: sync?
  UNIMPLEMENTED(FATAL) << "no mips64 implementation";
}

void Mips64Assembler::CreateHandleScopeEntry(ManagedRegister mout_reg,
                                             FrameOffset handle_scope_offset,
                                             ManagedRegister min_reg,
                                             bool null_allowed) {
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
      LoadConst32(out_reg.AsGpuRegister(), 0);
    }
    Beqzc(in_reg.AsGpuRegister(), &null_arg);
    Daddiu64(out_reg.AsGpuRegister(), SP, handle_scope_offset.Int32Value());
    Bind(&null_arg);
  } else {
    Daddiu64(out_reg.AsGpuRegister(), SP, handle_scope_offset.Int32Value());
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
    Beqzc(scratch.AsGpuRegister(), &null_arg);
    Daddiu64(scratch.AsGpuRegister(), SP, handle_scope_offset.Int32Value());
    Bind(&null_arg);
  } else {
    Daddiu64(scratch.AsGpuRegister(), SP, handle_scope_offset.Int32Value());
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
    LoadConst32(out_reg.AsGpuRegister(), 0);
  }
  Beqzc(in_reg.AsGpuRegister(), &null_arg);
  LoadFromOffset(kLoadDoubleword, out_reg.AsGpuRegister(),
                 in_reg.AsGpuRegister(), 0);
  Bind(&null_arg);
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
  LoadFromOffset(kLoadDoubleword, scratch.AsGpuRegister(),
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
  Bnezc(scratch.AsGpuRegister(), slow->Entry());
}

void Mips64ExceptionSlowPath::Emit(Assembler* sasm) {
  Mips64Assembler* sp_asm = down_cast<Mips64Assembler*>(sasm);
#define __ sp_asm->
  __ Bind(&entry_);
  if (stack_adjust_ != 0) {  // Fix up the frame.
    __ DecreaseFrameSize(stack_adjust_);
  }
  // Pass exception object as argument
  // Don't care about preserving A0 as this call won't return
  __ Move(A0, scratch_.AsGpuRegister());
  // Set up call to Thread::Current()->pDeliverException
  __ LoadFromOffset(kLoadDoubleword, T9, S1,
                    QUICK_ENTRYPOINT_OFFSET(8, pDeliverException).Int32Value());
  // TODO: check T9 usage
  __ Jr(T9);
  // Call never returns
  __ Break();
#undef __
}

}  // namespace mips64
}  // namespace art
