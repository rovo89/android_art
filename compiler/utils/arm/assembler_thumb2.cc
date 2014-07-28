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

#include "assembler_thumb2.h"

#include "base/logging.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "offsets.h"
#include "thread.h"
#include "utils.h"

namespace art {
namespace arm {

void Thumb2Assembler::and_(Register rd, Register rn, const ShifterOperand& so,
                           Condition cond) {
  EmitDataProcessing(cond, AND, 0, rn, rd, so);
}


void Thumb2Assembler::eor(Register rd, Register rn, const ShifterOperand& so,
                          Condition cond) {
  EmitDataProcessing(cond, EOR, 0, rn, rd, so);
}


void Thumb2Assembler::sub(Register rd, Register rn, const ShifterOperand& so,
                          Condition cond) {
  EmitDataProcessing(cond, SUB, 0, rn, rd, so);
}


void Thumb2Assembler::rsb(Register rd, Register rn, const ShifterOperand& so,
                          Condition cond) {
  EmitDataProcessing(cond, RSB, 0, rn, rd, so);
}


void Thumb2Assembler::rsbs(Register rd, Register rn, const ShifterOperand& so,
                           Condition cond) {
  EmitDataProcessing(cond, RSB, 1, rn, rd, so);
}


void Thumb2Assembler::add(Register rd, Register rn, const ShifterOperand& so,
                          Condition cond) {
  EmitDataProcessing(cond, ADD, 0, rn, rd, so);
}


void Thumb2Assembler::adds(Register rd, Register rn, const ShifterOperand& so,
                           Condition cond) {
  EmitDataProcessing(cond, ADD, 1, rn, rd, so);
}


void Thumb2Assembler::subs(Register rd, Register rn, const ShifterOperand& so,
                           Condition cond) {
  EmitDataProcessing(cond, SUB, 1, rn, rd, so);
}


void Thumb2Assembler::adc(Register rd, Register rn, const ShifterOperand& so,
                          Condition cond) {
  EmitDataProcessing(cond, ADC, 0, rn, rd, so);
}


void Thumb2Assembler::sbc(Register rd, Register rn, const ShifterOperand& so,
                          Condition cond) {
  EmitDataProcessing(cond, SBC, 0, rn, rd, so);
}


void Thumb2Assembler::rsc(Register rd, Register rn, const ShifterOperand& so,
                          Condition cond) {
  EmitDataProcessing(cond, RSC, 0, rn, rd, so);
}


void Thumb2Assembler::tst(Register rn, const ShifterOperand& so, Condition cond) {
  CHECK_NE(rn, PC);  // Reserve tst pc instruction for exception handler marker.
  EmitDataProcessing(cond, TST, 1, rn, R0, so);
}


void Thumb2Assembler::teq(Register rn, const ShifterOperand& so, Condition cond) {
  CHECK_NE(rn, PC);  // Reserve teq pc instruction for exception handler marker.
  EmitDataProcessing(cond, TEQ, 1, rn, R0, so);
}


void Thumb2Assembler::cmp(Register rn, const ShifterOperand& so, Condition cond) {
  EmitDataProcessing(cond, CMP, 1, rn, R0, so);
}


void Thumb2Assembler::cmn(Register rn, const ShifterOperand& so, Condition cond) {
  EmitDataProcessing(cond, CMN, 1, rn, R0, so);
}


void Thumb2Assembler::orr(Register rd, Register rn,
                          const ShifterOperand& so, Condition cond) {
  EmitDataProcessing(cond, ORR, 0, rn, rd, so);
}


void Thumb2Assembler::orrs(Register rd, Register rn,
                           const ShifterOperand& so, Condition cond) {
  EmitDataProcessing(cond, ORR, 1, rn, rd, so);
}


void Thumb2Assembler::mov(Register rd, const ShifterOperand& so, Condition cond) {
  EmitDataProcessing(cond, MOV, 0, R0, rd, so);
}


void Thumb2Assembler::movs(Register rd, const ShifterOperand& so, Condition cond) {
  EmitDataProcessing(cond, MOV, 1, R0, rd, so);
}


void Thumb2Assembler::bic(Register rd, Register rn, const ShifterOperand& so,
                       Condition cond) {
  EmitDataProcessing(cond, BIC, 0, rn, rd, so);
}


void Thumb2Assembler::mvn(Register rd, const ShifterOperand& so, Condition cond) {
  EmitDataProcessing(cond, MVN, 0, R0, rd, so);
}


void Thumb2Assembler::mvns(Register rd, const ShifterOperand& so, Condition cond) {
  EmitDataProcessing(cond, MVN, 1, R0, rd, so);
}


void Thumb2Assembler::mul(Register rd, Register rn, Register rm, Condition cond) {
  if (rd == rm && !IsHighRegister(rd) && !IsHighRegister(rn) && !force_32bit_) {
    // 16 bit.
    int16_t encoding = B14 | B9 | B8 | B6 |
        rn << 3 | rd;
    Emit16(encoding);
  } else {
    // 32 bit.
    uint32_t op1 = 0b000;
    uint32_t op2 = 0b00;
    int32_t encoding = B31 | B30 | B29 | B28 | B27 | B25 | B24 |
        op1 << 20 |
        B15 | B14 | B13 | B12 |
        op2 << 4 |
        static_cast<uint32_t>(rd) << 8 |
        static_cast<uint32_t>(rn) << 16 |
        static_cast<uint32_t>(rm);

    Emit32(encoding);
  }
}


void Thumb2Assembler::mla(Register rd, Register rn, Register rm, Register ra,
                          Condition cond) {
  uint32_t op1 = 0b000;
  uint32_t op2 = 0b00;
  int32_t encoding = B31 | B30 | B29 | B28 | B27 | B25 | B24 |
      op1 << 20 |
      op2 << 4 |
      static_cast<uint32_t>(rd) << 8 |
      static_cast<uint32_t>(ra) << 12 |
      static_cast<uint32_t>(rn) << 16 |
      static_cast<uint32_t>(rm);

  Emit32(encoding);
}


void Thumb2Assembler::mls(Register rd, Register rn, Register rm, Register ra,
                          Condition cond) {
  uint32_t op1 = 0b000;
  uint32_t op2 = 0b01;
  int32_t encoding = B31 | B30 | B29 | B28 | B27 | B25 | B24 |
      op1 << 20 |
      op2 << 4 |
      static_cast<uint32_t>(rd) << 8 |
      static_cast<uint32_t>(ra) << 12 |
      static_cast<uint32_t>(rn) << 16 |
      static_cast<uint32_t>(rm);

  Emit32(encoding);
}


void Thumb2Assembler::umull(Register rd_lo, Register rd_hi, Register rn,
                            Register rm, Condition cond) {
  uint32_t op1 = 0b010;
  uint32_t op2 = 0b0000;
  int32_t encoding = B31 | B30 | B29 | B28 | B27 | B25 | B24 | B23 |
      op1 << 20 |
      op2 << 4 |
      static_cast<uint32_t>(rd_lo) << 12 |
      static_cast<uint32_t>(rd_hi) << 8 |
      static_cast<uint32_t>(rn) << 16 |
      static_cast<uint32_t>(rm);

  Emit32(encoding);
}


void Thumb2Assembler::sdiv(Register rd, Register rn, Register rm, Condition cond) {
  uint32_t op1 = 0b001;
  uint32_t op2 = 0b1111;
  int32_t encoding = B31 | B30 | B29 | B28 | B27 | B25 | B24 | B23 | B20 |
      op1 << 20 |
      op2 << 4 |
      0xf << 12 |
      static_cast<uint32_t>(rd) << 8 |
      static_cast<uint32_t>(rn) << 16 |
      static_cast<uint32_t>(rm);

  Emit32(encoding);
}


void Thumb2Assembler::udiv(Register rd, Register rn, Register rm, Condition cond) {
  uint32_t op1 = 0b001;
  uint32_t op2 = 0b1111;
  int32_t encoding = B31 | B30 | B29 | B28 | B27 | B25 | B24 | B23 | B21 | B20 |
      op1 << 20 |
      op2 << 4 |
      0xf << 12 |
      static_cast<uint32_t>(rd) << 8 |
      static_cast<uint32_t>(rn) << 16 |
      static_cast<uint32_t>(rm);

  Emit32(encoding);
}


void Thumb2Assembler::ldr(Register rd, const Address& ad, Condition cond) {
  EmitLoadStore(cond, true, false, false, false, rd, ad);
}


void Thumb2Assembler::str(Register rd, const Address& ad, Condition cond) {
  EmitLoadStore(cond, false, false, false, false, rd, ad);
}


void Thumb2Assembler::ldrb(Register rd, const Address& ad, Condition cond) {
  EmitLoadStore(cond, true, true, false, false, rd, ad);
}


void Thumb2Assembler::strb(Register rd, const Address& ad, Condition cond) {
  EmitLoadStore(cond, false, true, false, false, rd, ad);
}


void Thumb2Assembler::ldrh(Register rd, const Address& ad, Condition cond) {
  EmitLoadStore(cond, true, false, true, false, rd, ad);
}


void Thumb2Assembler::strh(Register rd, const Address& ad, Condition cond) {
  EmitLoadStore(cond, false, false, true, false, rd, ad);
}


void Thumb2Assembler::ldrsb(Register rd, const Address& ad, Condition cond) {
  EmitLoadStore(cond, true, true, false, true, rd, ad);
}


void Thumb2Assembler::ldrsh(Register rd, const Address& ad, Condition cond) {
  EmitLoadStore(cond, true, false, true, true, rd, ad);
}


void Thumb2Assembler::ldrd(Register rd, const Address& ad, Condition cond) {
  CHECK_EQ(rd % 2, 0);
  // This is different from other loads.  The encoding is like ARM.
  int32_t encoding = B31 | B30 | B29 | B27 | B22 | B20 |
      static_cast<int32_t>(rd) << 12 |
      (static_cast<int32_t>(rd) + 1) << 8 |
      ad.encodingThumbLdrdStrd();
  Emit32(encoding);
}


void Thumb2Assembler::strd(Register rd, const Address& ad, Condition cond) {
  CHECK_EQ(rd % 2, 0);
  // This is different from other loads.  The encoding is like ARM.
  int32_t encoding = B31 | B30 | B29 | B27 | B22 |
      static_cast<int32_t>(rd) << 12 |
      (static_cast<int32_t>(rd) + 1) << 8 |
      ad.encodingThumbLdrdStrd();
  Emit32(encoding);
}


void Thumb2Assembler::ldm(BlockAddressMode am,
                          Register base,
                          RegList regs,
                          Condition cond) {
  if (__builtin_popcount(regs) == 1) {
    // Thumb doesn't support one reg in the list.
    // Find the register number.
    int reg = 0;
    while (reg < 16) {
      if ((regs & (1 << reg)) != 0) {
         break;
      }
      ++reg;
    }
    CHECK_LT(reg, 16);
    CHECK(am == DB_W);      // Only writeback is supported.
    ldr(static_cast<Register>(reg), Address(base, kRegisterSize, Address::PostIndex), cond);
  } else {
    EmitMultiMemOp(cond, am, true, base, regs);
  }
}


void Thumb2Assembler::stm(BlockAddressMode am,
                          Register base,
                          RegList regs,
                          Condition cond) {
  if (__builtin_popcount(regs) == 1) {
    // Thumb doesn't support one reg in the list.
    // Find the register number.
    int reg = 0;
    while (reg < 16) {
      if ((regs & (1 << reg)) != 0) {
         break;
      }
      ++reg;
    }
    CHECK_LT(reg, 16);
    CHECK(am == IA || am == IA_W);
    Address::Mode strmode = am == IA ? Address::PreIndex : Address::Offset;
    str(static_cast<Register>(reg), Address(base, -kRegisterSize, strmode), cond);
  } else {
    EmitMultiMemOp(cond, am, false, base, regs);
  }
}


bool Thumb2Assembler::vmovs(SRegister sd, float s_imm, Condition cond) {
  uint32_t imm32 = bit_cast<uint32_t, float>(s_imm);
  if (((imm32 & ((1 << 19) - 1)) == 0) &&
      ((((imm32 >> 25) & ((1 << 6) - 1)) == (1 << 5)) ||
       (((imm32 >> 25) & ((1 << 6) - 1)) == ((1 << 5) -1)))) {
    uint8_t imm8 = ((imm32 >> 31) << 7) | (((imm32 >> 29) & 1) << 6) |
        ((imm32 >> 19) & ((1 << 6) -1));
    EmitVFPsss(cond, B23 | B21 | B20 | ((imm8 >> 4)*B16) | (imm8 & 0xf),
               sd, S0, S0);
    return true;
  }
  return false;
}


bool Thumb2Assembler::vmovd(DRegister dd, double d_imm, Condition cond) {
  uint64_t imm64 = bit_cast<uint64_t, double>(d_imm);
  if (((imm64 & ((1LL << 48) - 1)) == 0) &&
      ((((imm64 >> 54) & ((1 << 9) - 1)) == (1 << 8)) ||
       (((imm64 >> 54) & ((1 << 9) - 1)) == ((1 << 8) -1)))) {
    uint8_t imm8 = ((imm64 >> 63) << 7) | (((imm64 >> 61) & 1) << 6) |
        ((imm64 >> 48) & ((1 << 6) -1));
    EmitVFPddd(cond, B23 | B21 | B20 | ((imm8 >> 4)*B16) | B8 | (imm8 & 0xf),
               dd, D0, D0);
    return true;
  }
  return false;
}


void Thumb2Assembler::vmovs(SRegister sd, SRegister sm, Condition cond) {
  EmitVFPsss(cond, B23 | B21 | B20 | B6, sd, S0, sm);
}


void Thumb2Assembler::vmovd(DRegister dd, DRegister dm, Condition cond) {
  EmitVFPddd(cond, B23 | B21 | B20 | B6, dd, D0, dm);
}


void Thumb2Assembler::vadds(SRegister sd, SRegister sn, SRegister sm,
                            Condition cond) {
  EmitVFPsss(cond, B21 | B20, sd, sn, sm);
}


void Thumb2Assembler::vaddd(DRegister dd, DRegister dn, DRegister dm,
                            Condition cond) {
  EmitVFPddd(cond, B21 | B20, dd, dn, dm);
}


void Thumb2Assembler::vsubs(SRegister sd, SRegister sn, SRegister sm,
                            Condition cond) {
  EmitVFPsss(cond, B21 | B20 | B6, sd, sn, sm);
}


void Thumb2Assembler::vsubd(DRegister dd, DRegister dn, DRegister dm,
                            Condition cond) {
  EmitVFPddd(cond, B21 | B20 | B6, dd, dn, dm);
}


void Thumb2Assembler::vmuls(SRegister sd, SRegister sn, SRegister sm,
                            Condition cond) {
  EmitVFPsss(cond, B21, sd, sn, sm);
}


void Thumb2Assembler::vmuld(DRegister dd, DRegister dn, DRegister dm,
                            Condition cond) {
  EmitVFPddd(cond, B21, dd, dn, dm);
}


void Thumb2Assembler::vmlas(SRegister sd, SRegister sn, SRegister sm,
                            Condition cond) {
  EmitVFPsss(cond, 0, sd, sn, sm);
}


void Thumb2Assembler::vmlad(DRegister dd, DRegister dn, DRegister dm,
                            Condition cond) {
  EmitVFPddd(cond, 0, dd, dn, dm);
}


void Thumb2Assembler::vmlss(SRegister sd, SRegister sn, SRegister sm,
                            Condition cond) {
  EmitVFPsss(cond, B6, sd, sn, sm);
}


void Thumb2Assembler::vmlsd(DRegister dd, DRegister dn, DRegister dm,
                            Condition cond) {
  EmitVFPddd(cond, B6, dd, dn, dm);
}


void Thumb2Assembler::vdivs(SRegister sd, SRegister sn, SRegister sm,
                            Condition cond) {
  EmitVFPsss(cond, B23, sd, sn, sm);
}


void Thumb2Assembler::vdivd(DRegister dd, DRegister dn, DRegister dm,
                            Condition cond) {
  EmitVFPddd(cond, B23, dd, dn, dm);
}


void Thumb2Assembler::vabss(SRegister sd, SRegister sm, Condition cond) {
  EmitVFPsss(cond, B23 | B21 | B20 | B7 | B6, sd, S0, sm);
}


void Thumb2Assembler::vabsd(DRegister dd, DRegister dm, Condition cond) {
  EmitVFPddd(cond, B23 | B21 | B20 | B7 | B6, dd, D0, dm);
}


void Thumb2Assembler::vnegs(SRegister sd, SRegister sm, Condition cond) {
  EmitVFPsss(cond, B23 | B21 | B20 | B16 | B6, sd, S0, sm);
}


void Thumb2Assembler::vnegd(DRegister dd, DRegister dm, Condition cond) {
  EmitVFPddd(cond, B23 | B21 | B20 | B16 | B6, dd, D0, dm);
}


void Thumb2Assembler::vsqrts(SRegister sd, SRegister sm, Condition cond) {
  EmitVFPsss(cond, B23 | B21 | B20 | B16 | B7 | B6, sd, S0, sm);
}

void Thumb2Assembler::vsqrtd(DRegister dd, DRegister dm, Condition cond) {
  EmitVFPddd(cond, B23 | B21 | B20 | B16 | B7 | B6, dd, D0, dm);
}


void Thumb2Assembler::vcvtsd(SRegister sd, DRegister dm, Condition cond) {
  EmitVFPsd(cond, B23 | B21 | B20 | B18 | B17 | B16 | B8 | B7 | B6, sd, dm);
}


void Thumb2Assembler::vcvtds(DRegister dd, SRegister sm, Condition cond) {
  EmitVFPds(cond, B23 | B21 | B20 | B18 | B17 | B16 | B7 | B6, dd, sm);
}


void Thumb2Assembler::vcvtis(SRegister sd, SRegister sm, Condition cond) {
  EmitVFPsss(cond, B23 | B21 | B20 | B19 | B18 | B16 | B7 | B6, sd, S0, sm);
}


void Thumb2Assembler::vcvtid(SRegister sd, DRegister dm, Condition cond) {
  EmitVFPsd(cond, B23 | B21 | B20 | B19 | B18 | B16 | B8 | B7 | B6, sd, dm);
}


void Thumb2Assembler::vcvtsi(SRegister sd, SRegister sm, Condition cond) {
  EmitVFPsss(cond, B23 | B21 | B20 | B19 | B7 | B6, sd, S0, sm);
}


void Thumb2Assembler::vcvtdi(DRegister dd, SRegister sm, Condition cond) {
  EmitVFPds(cond, B23 | B21 | B20 | B19 | B8 | B7 | B6, dd, sm);
}


void Thumb2Assembler::vcvtus(SRegister sd, SRegister sm, Condition cond) {
  EmitVFPsss(cond, B23 | B21 | B20 | B19 | B18 | B7 | B6, sd, S0, sm);
}


void Thumb2Assembler::vcvtud(SRegister sd, DRegister dm, Condition cond) {
  EmitVFPsd(cond, B23 | B21 | B20 | B19 | B18 | B8 | B7 | B6, sd, dm);
}


void Thumb2Assembler::vcvtsu(SRegister sd, SRegister sm, Condition cond) {
  EmitVFPsss(cond, B23 | B21 | B20 | B19 | B6, sd, S0, sm);
}


void Thumb2Assembler::vcvtdu(DRegister dd, SRegister sm, Condition cond) {
  EmitVFPds(cond, B23 | B21 | B20 | B19 | B8 | B6, dd, sm);
}


void Thumb2Assembler::vcmps(SRegister sd, SRegister sm, Condition cond) {
  EmitVFPsss(cond, B23 | B21 | B20 | B18 | B6, sd, S0, sm);
}


void Thumb2Assembler::vcmpd(DRegister dd, DRegister dm, Condition cond) {
  EmitVFPddd(cond, B23 | B21 | B20 | B18 | B6, dd, D0, dm);
}


void Thumb2Assembler::vcmpsz(SRegister sd, Condition cond) {
  EmitVFPsss(cond, B23 | B21 | B20 | B18 | B16 | B6, sd, S0, S0);
}


void Thumb2Assembler::vcmpdz(DRegister dd, Condition cond) {
  EmitVFPddd(cond, B23 | B21 | B20 | B18 | B16 | B6, dd, D0, D0);
}

void Thumb2Assembler::b(Label* label, Condition cond) {
  EmitBranch(cond, label, false, false);
}


void Thumb2Assembler::bl(Label* label, Condition cond) {
  CheckCondition(cond);
  EmitBranch(cond, label, true, false);
}


void Thumb2Assembler::blx(Label* label) {
  EmitBranch(AL, label, true, true);
}


void Thumb2Assembler::MarkExceptionHandler(Label* label) {
  EmitDataProcessing(AL, TST, 1, PC, R0, ShifterOperand(0));
  Label l;
  b(&l);
  EmitBranch(AL, label, false, false);
  Bind(&l);
}


void Thumb2Assembler::Emit32(int32_t value) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  buffer_.Emit<int16_t>(value >> 16);
  buffer_.Emit<int16_t>(value & 0xffff);
}


void Thumb2Assembler::Emit16(int16_t value) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  buffer_.Emit<int16_t>(value);
}


bool Thumb2Assembler::Is32BitDataProcessing(Condition cond,
                                            Opcode opcode,
                                            int set_cc,
                                            Register rn,
                                            Register rd,
                                            const ShifterOperand& so) {
  if (force_32bit_) {
    return true;
  }

  bool can_contain_high_register = (opcode == MOV)
      || ((opcode == ADD || opcode == SUB) && (rn == rd));

  if (IsHighRegister(rd) || IsHighRegister(rn)) {
    if (can_contain_high_register) {
      // There are high register instructions available for this opcode.
      // However, there is no RRX available.
      if (so.IsShift() && so.GetShift() == RRX) {
        return true;
      }

      // Check special case for SP relative ADD and SUB immediate.
      if ((opcode == ADD || opcode == SUB) && so.IsImmediate()) {
        // If rn is SP and rd is a high register we need to use a 32 bit encoding.
         if (rn == SP && rd != SP && IsHighRegister(rd)) {
           return true;
         }

         uint32_t imm = so.GetImmediate();
         // If the immediates are out of range use 32 bit.
         if (rd == SP && rn == SP) {
           if (imm > (1 << 9)) {    // 9 bit immediate.
             return true;
           }
         } else if (opcode == ADD && rd != SP && rn == SP) {   // 10 bit immediate.
           if (imm > (1 << 10)) {
             return true;
           }
         } else if (opcode == SUB && rd != SP && rn == SP) {
           // SUB rd, SP, #imm is always 32 bit.
           return true;
         }
      }
    }

    // The ADD,SUB and MOV instructions that work with high registers don't have
    // immediate variants.
    if (so.IsImmediate()) {
      return true;
    }

    if (!can_contain_high_register) {
      return true;
    }
  }

  if (so.IsRegister() && IsHighRegister(so.GetRegister()) && !can_contain_high_register) {
    return true;
  }

  // Check for MOV with an ROR.
  if (opcode == MOV && so.IsRegister() && so.IsShift() && so.GetShift() == ROR) {
    if (so.GetImmediate() != 0) {
      return true;
    }
  }

  bool rn_is_valid = true;

  // Check for single operand instructions and ADD/SUB.
  switch (opcode) {
    case CMP:
    case MOV:
    case TST:
    case MVN:
      rn_is_valid = false;      // There is no Rn for these instructions.
      break;
    case TEQ:
      return true;
      break;
    case ADD:
    case SUB:
      break;
    default:
      if (so.IsRegister() && rd != rn) {
        return true;
      }
  }

  if (so.IsImmediate()) {
    if (rn_is_valid && rn != rd) {
      // The only thumb1 instruction with a register and an immediate are ADD and SUB.  The
      // immediate must be 3 bits.
      if (opcode != ADD && opcode != SUB) {
        return true;
      } else {
        // Check that the immediate is 3 bits for ADD and SUB.
        if (so.GetImmediate() >= 8) {
          return true;
        }
      }
    } else {
      // ADD, SUB, CMP and MOV may be thumb1 only if the immediate is 8 bits.
      if (!(opcode == ADD || opcode == SUB || opcode == MOV || opcode == CMP)) {
        return true;
      } else {
        if (so.GetImmediate() > 255) {
          return true;
        }
      }
    }
  }

  // The instruction can be encoded in 16 bits.
  return false;
}


void Thumb2Assembler::Emit32BitDataProcessing(Condition cond,
                                              Opcode opcode,
                                              int set_cc,
                                              Register rn,
                                              Register rd,
                                              const ShifterOperand& so) {
  uint8_t thumb_opcode = 0b11111111;
  switch (opcode) {
    case AND: thumb_opcode = 0b0000; break;
    case EOR: thumb_opcode = 0b0100; break;
    case SUB: thumb_opcode = 0b1101; break;
    case RSB: thumb_opcode = 0b1110; break;
    case ADD: thumb_opcode = 0b1000; break;
    case ADC: thumb_opcode = 0b1010; break;
    case SBC: thumb_opcode = 0b1011; break;
    case RSC: break;
    case TST: thumb_opcode = 0b0000; set_cc = true; rd = PC; break;
    case TEQ: thumb_opcode = 0b0100; set_cc = true; rd = PC; break;
    case CMP: thumb_opcode = 0b1101; set_cc = true; rd = PC; break;
    case CMN: thumb_opcode = 0b1000; set_cc = true; rd = PC; break;
    case ORR: thumb_opcode = 0b0010; break;
    case MOV: thumb_opcode = 0b0010; rn = PC; break;
    case BIC: thumb_opcode = 0b0001; break;
    case MVN: thumb_opcode = 0b0011; rn = PC; break;
    default:
      break;
  }

  if (thumb_opcode == 0b11111111) {
    LOG(FATAL) << "Invalid thumb2 opcode " << opcode;
  }

  int32_t encoding = 0;
  if (so.IsImmediate()) {
    // Check special cases.
    if ((opcode == SUB || opcode == ADD) && (so.GetImmediate() < (1u << 12))) {
      if (opcode == SUB) {
        thumb_opcode = 0b0101;
      } else {
        thumb_opcode = 0;
      }
      uint32_t imm = so.GetImmediate();

      uint32_t i = (imm >> 11) & 1;
      uint32_t imm3 = (imm >> 8) & 0b111;
      uint32_t imm8 = imm & 0xff;

      encoding = B31 | B30 | B29 | B28 | B25 |
           thumb_opcode << 21 |
           rn << 16 |
           rd << 8 |
           i << 26 |
           imm3 << 12 |
           imm8;
    } else {
      // Modified immediate.
      uint32_t imm = ModifiedImmediate(so.encodingThumb());
      if (imm == kInvalidModifiedImmediate) {
        LOG(FATAL) << "Immediate value cannot fit in thumb2 modified immediate";
      }
      encoding = B31 | B30 | B29 | B28 |
          thumb_opcode << 21 |
          set_cc << 20 |
          rn << 16 |
          rd << 8 |
          imm;
    }
  } else if (so.IsRegister()) {
     // Register (possibly shifted)
     encoding = B31 | B30 | B29 | B27 | B25 |
         thumb_opcode << 21 |
         set_cc << 20 |
         rn << 16 |
         rd << 8 |
         so.encodingThumb();
  }
  Emit32(encoding);
}


void Thumb2Assembler::Emit16BitDataProcessing(Condition cond,
                                              Opcode opcode,
                                              int set_cc,
                                              Register rn,
                                              Register rd,
                                              const ShifterOperand& so) {
  if (opcode == ADD || opcode == SUB) {
    Emit16BitAddSub(cond, opcode, set_cc, rn, rd, so);
    return;
  }
  uint8_t thumb_opcode = 0b11111111;
  // Thumb1.
  uint8_t dp_opcode = 0b01;
  uint8_t opcode_shift = 6;
  uint8_t rd_shift = 0;
  uint8_t rn_shift = 3;
  uint8_t immediate_shift = 0;
  bool use_immediate = false;
  uint8_t immediate = 0;

  if (opcode == MOV && so.IsRegister() && so.IsShift()) {
    // Convert shifted mov operand2 into 16 bit opcodes.
    dp_opcode = 0;
    opcode_shift = 11;

    use_immediate = true;
    immediate = so.GetImmediate();
    immediate_shift = 6;

    rn = so.GetRegister();

    switch (so.GetShift()) {
    case LSL: thumb_opcode = 0b00; break;
    case LSR: thumb_opcode = 0b01; break;
    case ASR: thumb_opcode = 0b10; break;
    case ROR:
      // ROR doesn't allow immediates.
      thumb_opcode = 0b111;
      dp_opcode = 0b01;
      opcode_shift = 6;
      use_immediate = false;
      break;
    case RRX: break;
    default:
     break;
    }
  } else {
    if (so.IsImmediate()) {
      use_immediate = true;
      immediate = so.GetImmediate();
    }

    switch (opcode) {
      case AND: thumb_opcode = 0b0000; break;
      case EOR: thumb_opcode = 0b0001; break;
      case SUB: break;
      case RSB: thumb_opcode = 0b1001; break;
      case ADD: break;
      case ADC: thumb_opcode = 0b0101; break;
      case SBC: thumb_opcode = 0b0110; break;
      case RSC: break;
      case TST: thumb_opcode = 0b1000; rn = so.GetRegister(); break;
      case TEQ: break;
      case CMP:
        if (use_immediate) {
          // T2 encoding.
           dp_opcode = 0;
           opcode_shift = 11;
           thumb_opcode = 0b101;
           rd_shift = 8;
           rn_shift = 8;
        } else {
          thumb_opcode = 0b1010;
          rd = rn;
          rn = so.GetRegister();
        }

        break;
      case CMN: {
        thumb_opcode = 0b1011;
        rd = rn;
        rn = so.GetRegister();
        break;
      }
      case ORR: thumb_opcode = 0b1100; break;
      case MOV:
        dp_opcode = 0;
        if (use_immediate) {
          // T2 encoding.
          opcode_shift = 11;
          thumb_opcode = 0b100;
          rd_shift = 8;
          rn_shift = 8;
        } else {
          rn = so.GetRegister();
          if (IsHighRegister(rn) || IsHighRegister(rd)) {
            // Special mov for high registers.
            dp_opcode = 0b01;
            opcode_shift = 7;
            // Put the top bit of rd into the bottom bit of the opcode.
            thumb_opcode = 0b0001100 | static_cast<uint32_t>(rd) >> 3;
            rd = static_cast<Register>(static_cast<uint32_t>(rd) & 0b111);
          } else {
            thumb_opcode = 0;
          }
        }
        break;
      case BIC: thumb_opcode = 0b1110; break;
      case MVN: thumb_opcode = 0b1111; rn = so.GetRegister(); break;
      default:
        break;
    }
  }

  if (thumb_opcode == 0b11111111) {
    LOG(FATAL) << "Invalid thumb1 opcode " << opcode;
  }

  int16_t encoding = dp_opcode << 14 |
      (thumb_opcode << opcode_shift) |
      rd << rd_shift |
      rn << rn_shift |
      (use_immediate ? (immediate << immediate_shift) : 0);

  Emit16(encoding);
}


// ADD and SUB are complex enough to warrant their own emitter.
void Thumb2Assembler::Emit16BitAddSub(Condition cond,
                                      Opcode opcode,
                                      int set_cc,
                                      Register rn,
                                      Register rd,
                                      const ShifterOperand& so) {
  uint8_t dp_opcode = 0;
  uint8_t opcode_shift = 6;
  uint8_t rd_shift = 0;
  uint8_t rn_shift = 3;
  uint8_t immediate_shift = 0;
  bool use_immediate = false;
  uint8_t immediate = 0;
  uint8_t thumb_opcode;;

  if (so.IsImmediate()) {
    use_immediate = true;
    immediate = so.GetImmediate();
  }

  switch (opcode) {
    case ADD:
      if (so.IsRegister()) {
        Register rm = so.GetRegister();
        if (rn == rd) {
          // Can use T2 encoding (allows 4 bit registers)
          dp_opcode = 0b01;
          opcode_shift = 10;
          thumb_opcode = 0b0001;
          // Make Rn also contain the top bit of rd.
          rn = static_cast<Register>(static_cast<uint32_t>(rm) |
                                     (static_cast<uint32_t>(rd) & 0b1000) << 1);
          rd = static_cast<Register>(static_cast<uint32_t>(rd) & 0b111);
        } else {
          // T1.
          opcode_shift = 9;
          thumb_opcode = 0b01100;
          immediate = static_cast<uint32_t>(so.GetRegister());
          use_immediate = true;
          immediate_shift = 6;
        }
      } else {
        // Immediate.
        if (rd == SP && rn == SP) {
          // ADD sp, sp, #imm
          dp_opcode = 0b10;
          thumb_opcode = 0b11;
          opcode_shift = 12;
          CHECK_LT(immediate, (1 << 9));
          CHECK_EQ((immediate & 0b11), 0);

          // Remove rd and rn from instruction by orring it with immed and clearing bits.
          rn = R0;
          rd = R0;
          rd_shift = 0;
          rn_shift = 0;
          immediate >>= 2;
        } else if (rd != SP && rn == SP) {
          // ADD rd, SP, #imm
          dp_opcode = 0b10;
          thumb_opcode = 0b101;
          opcode_shift = 11;
          CHECK_LT(immediate, (1 << 10));
          CHECK_EQ((immediate & 0b11), 0);

          // Remove rn from instruction.
          rn = R0;
          rn_shift = 0;
          rd_shift = 8;
          immediate >>= 2;
        } else if (rn != rd) {
          // Must use T1.
          opcode_shift = 9;
          thumb_opcode = 0b01110;
          immediate_shift = 6;
        } else {
          // T2 encoding.
          opcode_shift = 11;
          thumb_opcode = 0b110;
          rd_shift = 8;
          rn_shift = 8;
        }
      }
      break;

    case SUB:
      if (so.IsRegister()) {
         // T1.
         opcode_shift = 9;
         thumb_opcode = 0b01101;
         immediate = static_cast<uint32_t>(so.GetRegister());
         use_immediate = true;
         immediate_shift = 6;
       } else {
         if (rd == SP && rn == SP) {
           // SUB sp, sp, #imm
           dp_opcode = 0b10;
           thumb_opcode = 0b1100001;
           opcode_shift = 7;
           CHECK_LT(immediate, (1 << 9));
           CHECK_EQ((immediate & 0b11), 0);

           // Remove rd and rn from instruction by orring it with immed and clearing bits.
           rn = R0;
           rd = R0;
           rd_shift = 0;
           rn_shift = 0;
           immediate >>= 2;
         } else if (rn != rd) {
           // Must use T1.
           opcode_shift = 9;
           thumb_opcode = 0b01111;
           immediate_shift = 6;
         } else {
           // T2 encoding.
           opcode_shift = 11;
           thumb_opcode = 0b111;
           rd_shift = 8;
           rn_shift = 8;
         }
       }
      break;
    default:
      LOG(FATAL) << "This opcode is not an ADD or SUB: " << opcode;
      return;
  }

  int16_t encoding = dp_opcode << 14 |
      (thumb_opcode << opcode_shift) |
      rd << rd_shift |
      rn << rn_shift |
      (use_immediate ? (immediate << immediate_shift) : 0);

  Emit16(encoding);
}


void Thumb2Assembler::EmitDataProcessing(Condition cond,
                                         Opcode opcode,
                                         int set_cc,
                                         Register rn,
                                         Register rd,
                                         const ShifterOperand& so) {
  CHECK_NE(rd, kNoRegister);
  CheckCondition(cond);

  if (Is32BitDataProcessing(cond, opcode, set_cc, rn, rd, so)) {
    Emit32BitDataProcessing(cond, opcode, set_cc, rn, rd, so);
  } else {
    Emit16BitDataProcessing(cond, opcode, set_cc, rn, rd, so);
  }
}

void Thumb2Assembler::EmitShift(Register rd, Register rm, Shift shift, uint8_t amount, bool setcc) {
  CHECK_LT(amount, (1 << 5));
  if (IsHighRegister(rd) || IsHighRegister(rm) || shift == ROR || shift == RRX) {
    uint16_t opcode = 0;
    switch (shift) {
      case LSL: opcode = 0b00; break;
      case LSR: opcode = 0b01; break;
      case ASR: opcode = 0b10; break;
      case ROR: opcode = 0b11; break;
      case RRX: opcode = 0b11; amount = 0; break;
      default:
        LOG(FATAL) << "Unsupported thumb2 shift opcode";
    }
    // 32 bit.
    int32_t encoding = B31 | B30 | B29 | B27 | B25 | B22 |
        0xf << 16 | (setcc ? B20 : 0);
    uint32_t imm3 = amount >> 2;
    uint32_t imm2 = amount & 0b11;
    encoding |= imm3 << 12 | imm2 << 6 | static_cast<int16_t>(rm) |
        static_cast<int16_t>(rd) << 8 | opcode << 4;
    Emit32(encoding);
  } else {
    // 16 bit shift
    uint16_t opcode = 0;
    switch (shift) {
      case LSL: opcode = 0b00; break;
      case LSR: opcode = 0b01; break;
      case ASR: opcode = 0b10; break;
      default:
         LOG(FATAL) << "Unsupported thumb2 shift opcode";
    }
    int16_t encoding = opcode << 11 | amount << 6 | static_cast<int16_t>(rm) << 3 |
        static_cast<int16_t>(rd);
    Emit16(encoding);
  }
}

void Thumb2Assembler::EmitShift(Register rd, Register rn, Shift shift, Register rm, bool setcc) {
  CHECK_NE(shift, RRX);
  bool must_be_32bit = false;
  if (IsHighRegister(rd) || IsHighRegister(rm) || IsHighRegister(rn) || rd != rn) {
    must_be_32bit = true;
  }

  if (must_be_32bit) {
    uint16_t opcode = 0;
     switch (shift) {
       case LSL: opcode = 0b00; break;
       case LSR: opcode = 0b01; break;
       case ASR: opcode = 0b10; break;
       case ROR: opcode = 0b11; break;
       default:
         LOG(FATAL) << "Unsupported thumb2 shift opcode";
     }
     // 32 bit.
     int32_t encoding = B31 | B30 | B29 | B28 | B27 | B25 |
         0xf << 12 | (setcc ? B20 : 0);
     encoding |= static_cast<int16_t>(rn) << 16 | static_cast<int16_t>(rm) |
         static_cast<int16_t>(rd) << 8 | opcode << 21;
     Emit32(encoding);
  } else {
    uint16_t opcode = 0;
    switch (shift) {
      case LSL: opcode = 0b0010; break;
      case LSR: opcode = 0b0011; break;
      case ASR: opcode = 0b0100; break;
      default:
         LOG(FATAL) << "Unsupported thumb2 shift opcode";
    }
    int16_t encoding = B14 | opcode << 6 | static_cast<int16_t>(rm) << 3 |
        static_cast<int16_t>(rd);
    Emit16(encoding);
  }
}



void Thumb2Assembler::Branch::Emit(AssemblerBuffer* buffer) const {
  bool link = type_ == kUnconditionalLinkX || type_ == kUnconditionalLink;
  bool x = type_ == kUnconditionalX || type_ == kUnconditionalLinkX;
  int32_t offset = target_ - location_;

  if (size_ == k32Bit) {
    int32_t encoding = B31 | B30 | B29 | B28 | B15;
    if (link) {
      // BL or BLX immediate.
      encoding |= B14;
      if (!x) {
        encoding |= B12;
      } else {
        // Bottom bit of offset must be 0.
        CHECK_EQ((offset & 1), 0);
      }
    } else {
      if (x) {
        LOG(FATAL) << "Invalid use of BX";
      } else {
        if (cond_ == AL) {
          // Can use the T4 encoding allowing a 24 bit offset.
          if (!x) {
            encoding |= B12;
          }
        } else {
          // Must be T3 encoding with a 20 bit offset.
          encoding |= cond_ << 22;
        }
      }
    }
    encoding = Thumb2Assembler::EncodeBranchOffset(offset, encoding);
    buffer->Store<int16_t>(location_, static_cast<int16_t>(encoding >> 16));
    buffer->Store<int16_t>(location_+2, static_cast<int16_t>(encoding & 0xffff));
  } else {
    if (IsCompareAndBranch()) {
      offset -= 4;
      uint16_t i = (offset >> 6) & 1;
      uint16_t imm5 = (offset >> 1) & 0b11111;
      int16_t encoding = B15 | B13 | B12 |
            (type_ ==  kCompareAndBranchNonZero ? B11 : 0) |
            static_cast<uint32_t>(rn_) |
            B8 |
            i << 9 |
            imm5 << 3;
      buffer->Store<int16_t>(location_, encoding);
    } else {
      offset -= 4;    // Account for PC offset.
      int16_t encoding;
      // 16 bit.
      if (cond_ == AL) {
        encoding = B15 | B14 | B13 |
            ((offset >> 1) & 0x7ff);
      } else {
        encoding = B15 | B14 | B12 |
            cond_ << 8 | ((offset >> 1) & 0xff);
      }
      buffer->Store<int16_t>(location_, encoding);
    }
  }
}


uint16_t Thumb2Assembler::EmitCompareAndBranch(Register rn, uint16_t prev, bool n) {
  uint32_t location = buffer_.Size();

  // This is always unresolved as it must be a forward branch.
  Emit16(prev);      // Previous link.
  return AddBranch(n ? Branch::kCompareAndBranchNonZero : Branch::kCompareAndBranchZero,
      location, rn);
}


// NOTE: this only support immediate offsets, not [rx,ry].
// TODO: support [rx,ry] instructions.
void Thumb2Assembler::EmitLoadStore(Condition cond,
                                    bool load,
                                    bool byte,
                                    bool half,
                                    bool is_signed,
                                    Register rd,
                                    const Address& ad) {
  CHECK_NE(rd, kNoRegister);
  CheckCondition(cond);
  bool must_be_32bit = force_32bit_;
  if (IsHighRegister(rd)) {
    must_be_32bit = true;
  }

  Register rn = ad.GetRegister();
  if (IsHighRegister(rn) && rn != SP && rn != PC) {
    must_be_32bit = true;
  }

  if (is_signed || ad.GetOffset() < 0 || ad.GetMode() != Address::Offset) {
    must_be_32bit = true;
  }

  if (ad.IsImmediate()) {
    // Immediate offset
    int32_t offset = ad.GetOffset();

    // The 16 bit SP relative instruction can only have a 10 bit offset.
    if (rn == SP && offset >= (1 << 10)) {
      must_be_32bit = true;
    }

    if (byte) {
      // 5 bit offset, no shift.
      if (offset >= (1 << 5)) {
        must_be_32bit = true;
      }
    } else if (half) {
      // 6 bit offset, shifted by 1.
      if (offset >= (1 << 6)) {
        must_be_32bit = true;
      }
    } else {
      // 7 bit offset, shifted by 2.
      if (offset >= (1 << 7)) {
        must_be_32bit = true;
      }
    }

    if (must_be_32bit) {
      int32_t encoding = B31 | B30 | B29 | B28 | B27 |
          (load ? B20 : 0) |
          (is_signed ? B24 : 0) |
          static_cast<uint32_t>(rd) << 12 |
          ad.encodingThumb(true) |
          (byte ? 0 : half ? B21 : B22);
      Emit32(encoding);
    } else {
      // 16 bit thumb1.
      uint8_t opA = 0;
      bool sp_relative = false;

      if (byte) {
        opA = 0b0111;
      } else if (half) {
        opA = 0b1000;
      } else {
        if (rn == SP) {
          opA = 0b1001;
          sp_relative = true;
        } else {
          opA = 0b0110;
        }
      }
      int16_t encoding = opA << 12 |
          (load ? B11 : 0);

      CHECK_GE(offset, 0);
      if (sp_relative) {
        // SP relative, 10 bit offset.
        CHECK_LT(offset, (1 << 10));
        CHECK_EQ((offset & 0b11), 0);
        encoding |= rd << 8 | offset >> 2;
      } else {
        // No SP relative.  The offset is shifted right depending on
        // the size of the load/store.
        encoding |= static_cast<uint32_t>(rd);

        if (byte) {
          // 5 bit offset, no shift.
          CHECK_LT(offset, (1 << 5));
        } else if (half) {
          // 6 bit offset, shifted by 1.
          CHECK_LT(offset, (1 << 6));
          CHECK_EQ((offset & 0b1), 0);
          offset >>= 1;
        } else {
          // 7 bit offset, shifted by 2.
          CHECK_LT(offset, (1 << 7));
          CHECK_EQ((offset & 0b11), 0);
          offset >>= 2;
        }
        encoding |= rn << 3 | offset  << 6;
      }

      Emit16(encoding);
    }
  } else {
    // Register shift.
    if (ad.GetRegister() == PC) {
       // PC relative literal encoding.
      int32_t offset = ad.GetOffset();
      if (must_be_32bit || offset < 0 || offset >= (1 << 10) || !load) {
        int32_t up = B23;
        if (offset < 0) {
          offset = -offset;
          up = 0;
        }
        CHECK_LT(offset, (1 << 12));
        int32_t encoding = 0x1f << 27 | 0xf << 16 | B22 | (load ? B20 : 0) |
            offset | up |
            static_cast<uint32_t>(rd) << 12;
        Emit32(encoding);
      } else {
        // 16 bit literal load.
        CHECK_GE(offset, 0);
        CHECK_LT(offset, (1 << 10));
        int32_t encoding = B14 | (load ? B11 : 0) | static_cast<uint32_t>(rd) << 8 | offset >> 2;
        Emit16(encoding);
      }
    } else {
      if (ad.GetShiftCount() != 0) {
        // If there is a shift count this must be 32 bit.
        must_be_32bit = true;
      } else if (IsHighRegister(ad.GetRegisterOffset())) {
        must_be_32bit = true;
      }

      if (must_be_32bit) {
        int32_t encoding = 0x1f << 27 | (load ? B20 : 0) | static_cast<uint32_t>(rd) << 12 |
            ad.encodingThumb(true);
        if (half) {
          encoding |= B21;
        } else if (!byte) {
          encoding |= B22;
        }
        Emit32(encoding);
      } else {
        // 16 bit register offset.
        int32_t encoding = B14 | B12 | (load ? B11 : 0) | static_cast<uint32_t>(rd) |
            ad.encodingThumb(false);
        if (byte) {
          encoding |= B10;
        } else if (half) {
          encoding |= B9;
        }
        Emit16(encoding);
      }
    }
  }
}


void Thumb2Assembler::EmitMultiMemOp(Condition cond,
                                     BlockAddressMode am,
                                     bool load,
                                     Register base,
                                     RegList regs) {
  CHECK_NE(base, kNoRegister);
  CheckCondition(cond);
  bool must_be_32bit = force_32bit_;

  if ((regs & 0xff00) != 0) {
    must_be_32bit = true;
  }

  uint32_t w_bit = am == IA_W || am == DB_W || am == DA_W || am == IB_W;
  // 16 bit always uses writeback.
  if (!w_bit) {
    must_be_32bit = true;
  }

  if (must_be_32bit) {
    uint32_t op = 0;
    switch (am) {
      case IA:
      case IA_W:
        op = 0b01;
        break;
      case DB:
      case DB_W:
        op = 0b10;
        break;
      case DA:
      case IB:
      case DA_W:
      case IB_W:
        LOG(FATAL) << "LDM/STM mode not supported on thumb: " << am;
    }
    if (load) {
      // Cannot have SP in the list.
      CHECK_EQ((regs & (1 << SP)), 0);
    } else {
      // Cannot have PC or SP in the list.
      CHECK_EQ((regs & (1 << PC | 1 << SP)), 0);
    }
    int32_t encoding = B31 | B30 | B29 | B27 |
                    (op << 23) |
                    (load ? B20 : 0) |
                    base << 16 |
                    regs |
                    (w_bit << 21);
    Emit32(encoding);
  } else {
    int16_t encoding = B15 | B14 |
                    (load ? B11 : 0) |
                    base << 8 |
                    regs;
    Emit16(encoding);
  }
}


void Thumb2Assembler::EmitBranch(Condition cond, Label* label, bool link, bool x) {
  uint32_t pc = buffer_.Size();
  Branch::Type branch_type;
  if (cond == AL) {
    if (link) {
      if (x) {
        branch_type = Branch::kUnconditionalLinkX;      // BLX.
      } else {
        branch_type = Branch::kUnconditionalLink;       // BX.
      }
    } else {
      branch_type = Branch::kUnconditional;             // B.
    }
  } else {
    branch_type = Branch::kConditional;                 // B<cond>.
  }

  if (label->IsBound()) {
    Branch::Size size = AddBranch(branch_type, pc, label->Position(), cond);  // Resolved branch.

    // The branch is to a bound label which means that it's a backwards branch.  We know the
    // current size of it so we can emit the appropriate space.  Note that if it's a 16 bit
    // branch the size may change if it so happens that other branches change size that change
    // the distance to the target and that distance puts this branch over the limit for 16 bits.
    if (size == Branch::k16Bit) {
      DCHECK(!force_32bit_branches_);
      Emit16(0);          // Space for a 16 bit branch.
    } else {
      Emit32(0);            // Space for a 32 bit branch.
    }
  } else {
    // Branch is to an unbound label.  Emit space for it.
    uint16_t branch_id = AddBranch(branch_type, pc, cond);    // Unresolved branch.
    if (force_32bit_branches_ || force_32bit_) {
      Emit16(static_cast<uint16_t>(label->position_));    // Emit current label link.
      Emit16(0);                   // another 16 bits.
    } else {
      Emit16(static_cast<uint16_t>(label->position_));    // Emit current label link.
    }
    label->LinkTo(branch_id);           // Link to the branch ID.
  }
}


void Thumb2Assembler::clz(Register rd, Register rm, Condition cond) {
  CHECK_NE(rd, kNoRegister);
  CHECK_NE(rm, kNoRegister);
  CheckCondition(cond);
  CHECK_NE(rd, PC);
  CHECK_NE(rm, PC);
  int32_t encoding = B31 | B30 | B29 | B28 | B27 |
      B25 | B23 | B21 | B20 |
      static_cast<uint32_t>(rm) << 16 |
      0xf << 12 |
      static_cast<uint32_t>(rd) << 8 |
      B7 |
      static_cast<uint32_t>(rm);
  Emit32(encoding);
}


void Thumb2Assembler::movw(Register rd, uint16_t imm16, Condition cond) {
  CheckCondition(cond);
  bool must_be_32bit = force_32bit_;
  if (IsHighRegister(rd)|| imm16 >= 256u) {
    must_be_32bit = true;
  }

  if (must_be_32bit) {
    // Use encoding T3.
    uint32_t imm4 = (imm16 >> 12) & 0b1111;
    uint32_t i = (imm16 >> 11) & 0b1;
    uint32_t imm3 = (imm16 >> 8) & 0b111;
    uint32_t imm8 = imm16 & 0xff;
    int32_t encoding = B31 | B30 | B29 | B28 |
                    B25 | B22 |
                    static_cast<uint32_t>(rd) << 8 |
                    i << 26 |
                    imm4 << 16 |
                    imm3 << 12 |
                    imm8;
    Emit32(encoding);
  } else {
    int16_t encoding = B13 | static_cast<uint16_t>(rd) << 8 |
                imm16;
    Emit16(encoding);
  }
}


void Thumb2Assembler::movt(Register rd, uint16_t imm16, Condition cond) {
  CheckCondition(cond);
  // Always 32 bits.
  uint32_t imm4 = (imm16 >> 12) & 0b1111;
  uint32_t i = (imm16 >> 11) & 0b1;
  uint32_t imm3 = (imm16 >> 8) & 0b111;
  uint32_t imm8 = imm16 & 0xff;
  int32_t encoding = B31 | B30 | B29 | B28 |
                  B25 | B23 | B22 |
                  static_cast<uint32_t>(rd) << 8 |
                  i << 26 |
                  imm4 << 16 |
                  imm3 << 12 |
                  imm8;
  Emit32(encoding);
}


void Thumb2Assembler::ldrex(Register rt, Register rn, uint16_t imm, Condition cond) {
  CHECK_NE(rn, kNoRegister);
  CHECK_NE(rt, kNoRegister);
  CheckCondition(cond);
  CHECK_NE(rn, kNoRegister);
  CHECK_NE(rt, kNoRegister);
  CheckCondition(cond);
  CHECK_LT(imm, (1u << 10));

  int32_t encoding = B31 | B30 | B29 | B27 | B22 | B20 |
      static_cast<uint32_t>(rn) << 16 |
      static_cast<uint32_t>(rt) << 12 |
      0xf << 8 |
      imm >> 2;
  Emit32(encoding);
}


void Thumb2Assembler::ldrex(Register rt, Register rn, Condition cond) {
  ldrex(rt, rn, 0, cond);
}


void Thumb2Assembler::strex(Register rd,
                            Register rt,
                            Register rn,
                            uint16_t imm,
                            Condition cond) {
  CHECK_NE(rn, kNoRegister);
  CHECK_NE(rd, kNoRegister);
  CHECK_NE(rt, kNoRegister);
  CheckCondition(cond);
  CHECK_LT(imm, (1u << 10));

  int32_t encoding = B31 | B30 | B29 | B27 | B22 |
      static_cast<uint32_t>(rn) << 16 |
      static_cast<uint32_t>(rt) << 12 |
      static_cast<uint32_t>(rd) << 8 |
      imm >> 2;
  Emit32(encoding);
}


void Thumb2Assembler::strex(Register rd,
                            Register rt,
                            Register rn,
                            Condition cond) {
  strex(rd, rt, rn, 0, cond);
}


void Thumb2Assembler::clrex(Condition cond) {
  CheckCondition(cond);
  int32_t encoding = B31 | B30 | B29 | B27 | B28 | B25 | B24 | B23 |
      B21 | B20 |
      0xf << 16 |
      B15 |
      0xf << 8 |
      B5 |
      0xf;
  Emit32(encoding);
}


void Thumb2Assembler::nop(Condition cond) {
  CheckCondition(cond);
  int16_t encoding = B15 | B13 | B12 |
      B11 | B10 | B9 | B8;
  Emit16(encoding);
}


void Thumb2Assembler::vmovsr(SRegister sn, Register rt, Condition cond) {
  CHECK_NE(sn, kNoSRegister);
  CHECK_NE(rt, kNoRegister);
  CHECK_NE(rt, SP);
  CHECK_NE(rt, PC);
  CheckCondition(cond);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B25 |
                     ((static_cast<int32_t>(sn) >> 1)*B16) |
                     (static_cast<int32_t>(rt)*B12) | B11 | B9 |
                     ((static_cast<int32_t>(sn) & 1)*B7) | B4;
  Emit32(encoding);
}


void Thumb2Assembler::vmovrs(Register rt, SRegister sn, Condition cond) {
  CHECK_NE(sn, kNoSRegister);
  CHECK_NE(rt, kNoRegister);
  CHECK_NE(rt, SP);
  CHECK_NE(rt, PC);
  CheckCondition(cond);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B25 | B20 |
                     ((static_cast<int32_t>(sn) >> 1)*B16) |
                     (static_cast<int32_t>(rt)*B12) | B11 | B9 |
                     ((static_cast<int32_t>(sn) & 1)*B7) | B4;
  Emit32(encoding);
}


void Thumb2Assembler::vmovsrr(SRegister sm, Register rt, Register rt2,
                              Condition cond) {
  CHECK_NE(sm, kNoSRegister);
  CHECK_NE(sm, S31);
  CHECK_NE(rt, kNoRegister);
  CHECK_NE(rt, SP);
  CHECK_NE(rt, PC);
  CHECK_NE(rt2, kNoRegister);
  CHECK_NE(rt2, SP);
  CHECK_NE(rt2, PC);
  CheckCondition(cond);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B22 |
                     (static_cast<int32_t>(rt2)*B16) |
                     (static_cast<int32_t>(rt)*B12) | B11 | B9 |
                     ((static_cast<int32_t>(sm) & 1)*B5) | B4 |
                     (static_cast<int32_t>(sm) >> 1);
  Emit32(encoding);
}


void Thumb2Assembler::vmovrrs(Register rt, Register rt2, SRegister sm,
                              Condition cond) {
  CHECK_NE(sm, kNoSRegister);
  CHECK_NE(sm, S31);
  CHECK_NE(rt, kNoRegister);
  CHECK_NE(rt, SP);
  CHECK_NE(rt, PC);
  CHECK_NE(rt2, kNoRegister);
  CHECK_NE(rt2, SP);
  CHECK_NE(rt2, PC);
  CHECK_NE(rt, rt2);
  CheckCondition(cond);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B22 | B20 |
                     (static_cast<int32_t>(rt2)*B16) |
                     (static_cast<int32_t>(rt)*B12) | B11 | B9 |
                     ((static_cast<int32_t>(sm) & 1)*B5) | B4 |
                     (static_cast<int32_t>(sm) >> 1);
  Emit32(encoding);
}


void Thumb2Assembler::vmovdrr(DRegister dm, Register rt, Register rt2,
                              Condition cond) {
  CHECK_NE(dm, kNoDRegister);
  CHECK_NE(rt, kNoRegister);
  CHECK_NE(rt, SP);
  CHECK_NE(rt, PC);
  CHECK_NE(rt2, kNoRegister);
  CHECK_NE(rt2, SP);
  CHECK_NE(rt2, PC);
  CheckCondition(cond);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B22 |
                     (static_cast<int32_t>(rt2)*B16) |
                     (static_cast<int32_t>(rt)*B12) | B11 | B9 | B8 |
                     ((static_cast<int32_t>(dm) >> 4)*B5) | B4 |
                     (static_cast<int32_t>(dm) & 0xf);
  Emit32(encoding);
}


void Thumb2Assembler::vmovrrd(Register rt, Register rt2, DRegister dm,
                              Condition cond) {
  CHECK_NE(dm, kNoDRegister);
  CHECK_NE(rt, kNoRegister);
  CHECK_NE(rt, SP);
  CHECK_NE(rt, PC);
  CHECK_NE(rt2, kNoRegister);
  CHECK_NE(rt2, SP);
  CHECK_NE(rt2, PC);
  CHECK_NE(rt, rt2);
  CheckCondition(cond);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B22 | B20 |
                     (static_cast<int32_t>(rt2)*B16) |
                     (static_cast<int32_t>(rt)*B12) | B11 | B9 | B8 |
                     ((static_cast<int32_t>(dm) >> 4)*B5) | B4 |
                     (static_cast<int32_t>(dm) & 0xf);
  Emit32(encoding);
}


void Thumb2Assembler::vldrs(SRegister sd, const Address& ad, Condition cond) {
  const Address& addr = static_cast<const Address&>(ad);
  CHECK_NE(sd, kNoSRegister);
  CheckCondition(cond);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B24 | B20 |
                     ((static_cast<int32_t>(sd) & 1)*B22) |
                     ((static_cast<int32_t>(sd) >> 1)*B12) |
                     B11 | B9 | addr.vencoding();
  Emit32(encoding);
}


void Thumb2Assembler::vstrs(SRegister sd, const Address& ad, Condition cond) {
  const Address& addr = static_cast<const Address&>(ad);
  CHECK_NE(static_cast<Register>(addr.encodingArm() & (0xf << kRnShift)), PC);
  CHECK_NE(sd, kNoSRegister);
  CheckCondition(cond);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B24 |
                     ((static_cast<int32_t>(sd) & 1)*B22) |
                     ((static_cast<int32_t>(sd) >> 1)*B12) |
                     B11 | B9 | addr.vencoding();
  Emit32(encoding);
}


void Thumb2Assembler::vldrd(DRegister dd, const Address& ad, Condition cond) {
  const Address& addr = static_cast<const Address&>(ad);
  CHECK_NE(dd, kNoDRegister);
  CheckCondition(cond);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B24 | B20 |
                     ((static_cast<int32_t>(dd) >> 4)*B22) |
                     ((static_cast<int32_t>(dd) & 0xf)*B12) |
                     B11 | B9 | B8 | addr.vencoding();
  Emit32(encoding);
}


void Thumb2Assembler::vstrd(DRegister dd, const Address& ad, Condition cond) {
  const Address& addr = static_cast<const Address&>(ad);
  CHECK_NE(static_cast<Register>(addr.encodingArm() & (0xf << kRnShift)), PC);
  CHECK_NE(dd, kNoDRegister);
  CheckCondition(cond);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B24 |
                     ((static_cast<int32_t>(dd) >> 4)*B22) |
                     ((static_cast<int32_t>(dd) & 0xf)*B12) |
                     B11 | B9 | B8 | addr.vencoding();
  Emit32(encoding);
}


void Thumb2Assembler::vpushs(SRegister reg, int nregs, Condition cond) {
  EmitVPushPop(static_cast<uint32_t>(reg), nregs, true, false, cond);
}


void Thumb2Assembler::vpushd(DRegister reg, int nregs, Condition cond) {
  EmitVPushPop(static_cast<uint32_t>(reg), nregs, true, true, cond);
}


void Thumb2Assembler::vpops(SRegister reg, int nregs, Condition cond) {
  EmitVPushPop(static_cast<uint32_t>(reg), nregs, false, false, cond);
}


void Thumb2Assembler::vpopd(DRegister reg, int nregs, Condition cond) {
  EmitVPushPop(static_cast<uint32_t>(reg), nregs, false, true, cond);
}


void Thumb2Assembler::EmitVPushPop(uint32_t reg, int nregs, bool push, bool dbl, Condition cond) {
  CheckCondition(cond);

  uint32_t D;
  uint32_t Vd;
  if (dbl) {
    // Encoded as D:Vd.
    D = (reg >> 4) & 1;
    Vd = reg & 0b1111;
  } else {
    // Encoded as Vd:D.
    D = reg & 1;
    Vd = (reg >> 1) & 0b1111;
  }
  int32_t encoding = B27 | B26 | B21 | B19 | B18 | B16 |
                    B11 | B9 |
        (dbl ? B8 : 0) |
        (push ? B24 : (B23 | B20)) |
        0b1110 << 28 |
        nregs << (dbl ? 1 : 0) |
        D << 22 |
        Vd << 12;
  Emit32(encoding);
}


void Thumb2Assembler::EmitVFPsss(Condition cond, int32_t opcode,
                                 SRegister sd, SRegister sn, SRegister sm) {
  CHECK_NE(sd, kNoSRegister);
  CHECK_NE(sn, kNoSRegister);
  CHECK_NE(sm, kNoSRegister);
  CheckCondition(cond);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B25 | B11 | B9 | opcode |
                     ((static_cast<int32_t>(sd) & 1)*B22) |
                     ((static_cast<int32_t>(sn) >> 1)*B16) |
                     ((static_cast<int32_t>(sd) >> 1)*B12) |
                     ((static_cast<int32_t>(sn) & 1)*B7) |
                     ((static_cast<int32_t>(sm) & 1)*B5) |
                     (static_cast<int32_t>(sm) >> 1);
  Emit32(encoding);
}


void Thumb2Assembler::EmitVFPddd(Condition cond, int32_t opcode,
                                 DRegister dd, DRegister dn, DRegister dm) {
  CHECK_NE(dd, kNoDRegister);
  CHECK_NE(dn, kNoDRegister);
  CHECK_NE(dm, kNoDRegister);
  CheckCondition(cond);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B25 | B11 | B9 | B8 | opcode |
                     ((static_cast<int32_t>(dd) >> 4)*B22) |
                     ((static_cast<int32_t>(dn) & 0xf)*B16) |
                     ((static_cast<int32_t>(dd) & 0xf)*B12) |
                     ((static_cast<int32_t>(dn) >> 4)*B7) |
                     ((static_cast<int32_t>(dm) >> 4)*B5) |
                     (static_cast<int32_t>(dm) & 0xf);
  Emit32(encoding);
}


void Thumb2Assembler::EmitVFPsd(Condition cond, int32_t opcode,
                                SRegister sd, DRegister dm) {
  CHECK_NE(sd, kNoSRegister);
  CHECK_NE(dm, kNoDRegister);
  CheckCondition(cond);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B25 | B11 | B9 | opcode |
                     ((static_cast<int32_t>(sd) & 1)*B22) |
                     ((static_cast<int32_t>(sd) >> 1)*B12) |
                     ((static_cast<int32_t>(dm) >> 4)*B5) |
                     (static_cast<int32_t>(dm) & 0xf);
  Emit32(encoding);
}


void Thumb2Assembler::EmitVFPds(Condition cond, int32_t opcode,
                                DRegister dd, SRegister sm) {
  CHECK_NE(dd, kNoDRegister);
  CHECK_NE(sm, kNoSRegister);
  CheckCondition(cond);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B25 | B11 | B9 | opcode |
                     ((static_cast<int32_t>(dd) >> 4)*B22) |
                     ((static_cast<int32_t>(dd) & 0xf)*B12) |
                     ((static_cast<int32_t>(sm) & 1)*B5) |
                     (static_cast<int32_t>(sm) >> 1);
  Emit32(encoding);
}


void Thumb2Assembler::vmstat(Condition cond) {  // VMRS APSR_nzcv, FPSCR.
  CheckCondition(cond);
  UNIMPLEMENTED(FATAL) << "Unimplemented thumb instruction";
}


void Thumb2Assembler::svc(uint32_t imm8) {
  CHECK(IsUint(8, imm8)) << imm8;
  int16_t encoding = B15 | B14 | B12 |
       B11 | B10 | B9 | B8 |
       imm8;
  Emit16(encoding);
}


void Thumb2Assembler::bkpt(uint16_t imm8) {
  CHECK(IsUint(8, imm8)) << imm8;
  int16_t encoding = B15 | B13 | B12 |
      B11 | B10 | B9 |
      imm8;
  Emit16(encoding);
}

// Convert the given IT state to a mask bit given bit 0 of the first
// condition and a shift position.
static uint8_t ToItMask(ItState s, uint8_t firstcond0, uint8_t shift) {
  switch (s) {
  case kItOmitted: return 1 << shift;
  case kItThen: return firstcond0 << shift;
  case kItElse: return !firstcond0 << shift;
  }
  return 0;
}


// Set the IT condition in the given position for the given state.  This is used
// to check that conditional instructions match the preceding IT statement.
void Thumb2Assembler::SetItCondition(ItState s, Condition cond, uint8_t index) {
  switch (s) {
  case kItOmitted: it_conditions_[index] = AL; break;
  case kItThen: it_conditions_[index] = cond; break;
  case kItElse:
    it_conditions_[index] = static_cast<Condition>(static_cast<uint8_t>(cond) ^ 1);
    break;
  }
}


void Thumb2Assembler::it(Condition firstcond, ItState i1, ItState i2, ItState i3) {
  CheckCondition(AL);       // Not allowed in IT block.
  uint8_t firstcond0 = static_cast<uint8_t>(firstcond) & 1;

  // All conditions to AL.
  for (uint8_t i = 0; i < 4; ++i) {
    it_conditions_[i] = AL;
  }

  SetItCondition(kItThen, firstcond, 0);
  uint8_t mask = ToItMask(i1, firstcond0, 3);
  SetItCondition(i1, firstcond, 1);

  if (i1 != kItOmitted) {
    mask |= ToItMask(i2, firstcond0, 2);
    SetItCondition(i2, firstcond, 2);
    if (i2 != kItOmitted) {
      mask |= ToItMask(i3, firstcond0, 1);
      SetItCondition(i3, firstcond, 3);
      if (i3 != kItOmitted) {
        mask |= 0b0001;
      }
    }
  }

  // Start at first condition.
  it_cond_index_ = 0;
  next_condition_ = it_conditions_[0];
  uint16_t encoding = B15 | B13 | B12 |
        B11 | B10 | B9 | B8 |
        firstcond << 4 |
        mask;
  Emit16(encoding);
}


void Thumb2Assembler::cbz(Register rn, Label* label) {
  CheckCondition(AL);
  if (label->IsBound()) {
    LOG(FATAL) << "cbz can only be used to branch forwards";
  } else {
    uint16_t branchid = EmitCompareAndBranch(rn, static_cast<uint16_t>(label->position_), false);
    label->LinkTo(branchid);
  }
}


void Thumb2Assembler::cbnz(Register rn, Label* label) {
  CheckCondition(AL);
  if (label->IsBound()) {
    LOG(FATAL) << "cbnz can only be used to branch forwards";
  } else {
    uint16_t branchid = EmitCompareAndBranch(rn, static_cast<uint16_t>(label->position_), true);
    label->LinkTo(branchid);
  }
}


void Thumb2Assembler::blx(Register rm, Condition cond) {
  CHECK_NE(rm, kNoRegister);
  CheckCondition(cond);
  int16_t encoding = B14 | B10 | B9 | B8 | B7 | static_cast<int16_t>(rm) << 3;
  Emit16(encoding);
}


void Thumb2Assembler::bx(Register rm, Condition cond) {
  CHECK_NE(rm, kNoRegister);
  CheckCondition(cond);
  int16_t encoding = B14 | B10 | B9 | B8 | static_cast<int16_t>(rm) << 3;
  Emit16(encoding);
}


void Thumb2Assembler::Push(Register rd, Condition cond) {
  str(rd, Address(SP, -kRegisterSize, Address::PreIndex), cond);
}


void Thumb2Assembler::Pop(Register rd, Condition cond) {
  ldr(rd, Address(SP, kRegisterSize, Address::PostIndex), cond);
}


void Thumb2Assembler::PushList(RegList regs, Condition cond) {
  stm(DB_W, SP, regs, cond);
}


void Thumb2Assembler::PopList(RegList regs, Condition cond) {
  ldm(IA_W, SP, regs, cond);
}


void Thumb2Assembler::Mov(Register rd, Register rm, Condition cond) {
  if (cond != AL || rd != rm) {
    mov(rd, ShifterOperand(rm), cond);
  }
}


// A branch has changed size.  Make a hole for it.
void Thumb2Assembler::MakeHoleForBranch(uint32_t location, uint32_t delta) {
  // Move the contents of the buffer using: Move(newposition, oldposition)
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  buffer_.Move(location + delta, location);
}


void Thumb2Assembler::Bind(Label* label) {
  CHECK(!label->IsBound());
  uint32_t bound_pc = buffer_.Size();
  std::vector<Branch*> changed_branches;

  while (label->IsLinked()) {
    uint16_t position = label->Position();                  // Branch id for linked branch.
    Branch* branch = GetBranch(position);                   // Get the branch at this id.
    bool changed = branch->Resolve(bound_pc);               // Branch can be resolved now.
    uint32_t branch_location = branch->GetLocation();
    uint16_t next = buffer_.Load<uint16_t>(branch_location);       // Get next in chain.
    if (changed) {
      DCHECK(!force_32bit_branches_);
      MakeHoleForBranch(branch->GetLocation(), 2);
      if (branch->IsCompareAndBranch()) {
        // A cbz/cbnz instruction has changed size.  There is no valid encoding for
        // a 32 bit cbz/cbnz so we need to change this to an instruction pair:
        // cmp rn, #0
        // b<eq|ne> target
        bool n = branch->GetType() == Branch::kCompareAndBranchNonZero;
        Condition cond = n ? NE : EQ;
        branch->Move(2);      // Move the branch forward by 2 bytes.
        branch->ResetTypeAndCondition(Branch::kConditional, cond);
        branch->ResetSize(Branch::k16Bit);

        // Now add a compare instruction in the place the branch was.
        int16_t cmp = B13 | B11 | static_cast<int16_t>(branch->GetRegister()) << 8;
        buffer_.Store<int16_t>(branch_location, cmp);

        // Since have moved made a hole in the code we need to reload the
        // current pc.
        bound_pc = buffer_.Size();

        // Now resolve the newly added branch.
        changed = branch->Resolve(bound_pc);
        if (changed) {
          MakeHoleForBranch(branch->GetLocation(), 2);
          changed_branches.push_back(branch);
        }
      } else {
        changed_branches.push_back(branch);
      }
    }
    label->position_ = next;                                // Move to next.
  }
  label->BindTo(bound_pc);

  // Now relocate any changed branches.  Do this until there are no more changes.
  std::vector<Branch*> branches_to_process = changed_branches;
  while (branches_to_process.size() != 0) {
    changed_branches.clear();
    for (auto& changed_branch : branches_to_process) {
      for (auto& branch : branches_) {
        bool changed = branch->Relocate(changed_branch->GetLocation(), 2);
        if (changed) {
          changed_branches.push_back(branch);
        }
      }
      branches_to_process = changed_branches;
    }
  }
}


void Thumb2Assembler::EmitBranches() {
  for (auto& branch : branches_) {
    branch->Emit(&buffer_);
  }
}


void Thumb2Assembler::Lsl(Register rd, Register rm, uint32_t shift_imm,
                          bool setcc, Condition cond) {
  CHECK_NE(shift_imm, 0u);  // Do not use Lsl if no shift is wanted.
  CheckCondition(cond);
  EmitShift(rd, rm, LSL, shift_imm, setcc);
}


void Thumb2Assembler::Lsr(Register rd, Register rm, uint32_t shift_imm,
                          bool setcc, Condition cond) {
  CHECK_NE(shift_imm, 0u);  // Do not use Lsr if no shift is wanted.
  if (shift_imm == 32) shift_imm = 0;  // Comply to UAL syntax.
  CheckCondition(cond);
  EmitShift(rd, rm, LSR, shift_imm, setcc);
}


void Thumb2Assembler::Asr(Register rd, Register rm, uint32_t shift_imm,
                          bool setcc, Condition cond) {
  CHECK_NE(shift_imm, 0u);  // Do not use Asr if no shift is wanted.
  if (shift_imm == 32) shift_imm = 0;  // Comply to UAL syntax.
  CheckCondition(cond);
  EmitShift(rd, rm, ASR, shift_imm, setcc);
}


void Thumb2Assembler::Ror(Register rd, Register rm, uint32_t shift_imm,
                          bool setcc, Condition cond) {
  CHECK_NE(shift_imm, 0u);  // Use Rrx instruction.
  CheckCondition(cond);
  EmitShift(rd, rm, ROR, shift_imm, setcc);
}


void Thumb2Assembler::Rrx(Register rd, Register rm, bool setcc, Condition cond) {
  CheckCondition(cond);
  EmitShift(rd, rm, RRX, rm, setcc);
}


void Thumb2Assembler::Lsl(Register rd, Register rm, Register rn,
                          bool setcc, Condition cond) {
  CheckCondition(cond);
  EmitShift(rd, rm, LSL, rn, setcc);
}


void Thumb2Assembler::Lsr(Register rd, Register rm, Register rn,
                          bool setcc, Condition cond) {
  CheckCondition(cond);
  EmitShift(rd, rm, LSR, rn, setcc);
}


void Thumb2Assembler::Asr(Register rd, Register rm, Register rn,
                          bool setcc, Condition cond) {
  CheckCondition(cond);
  EmitShift(rd, rm, ASR, rn, setcc);
}


void Thumb2Assembler::Ror(Register rd, Register rm, Register rn,
                          bool setcc, Condition cond) {
  CheckCondition(cond);
  EmitShift(rd, rm, ROR, rn, setcc);
}


int32_t Thumb2Assembler::EncodeBranchOffset(int32_t offset, int32_t inst) {
  // The offset is off by 4 due to the way the ARM CPUs read PC.
  offset -= 4;
  offset >>= 1;

  uint32_t value = 0;
  // There are two different encodings depending on the value of bit 12.  In one case
  // intermediate values are calculated using the sign bit.
  if ((inst & B12) == B12) {
    // 25 bits of offset.
    uint32_t signbit = (offset >> 31) & 0x1;
    uint32_t i1 = (offset >> 22) & 0x1;
    uint32_t i2 = (offset >> 21) & 0x1;
    uint32_t imm10 = (offset >> 11) & 0x03ff;
    uint32_t imm11 = offset & 0x07ff;
    uint32_t j1 = (i1 ^ signbit) ? 0 : 1;
    uint32_t j2 = (i2 ^ signbit) ? 0 : 1;
    value = (signbit << 26) | (j1 << 13) | (j2 << 11) | (imm10 << 16) |
                      imm11;
    // Remove the offset from the current encoding.
    inst &= ~(0x3ff << 16 | 0x7ff);
  } else {
    uint32_t signbit = (offset >> 31) & 0x1;
    uint32_t imm6 = (offset >> 11) & 0x03f;
    uint32_t imm11 = offset & 0x07ff;
    uint32_t j1 = (offset >> 19) & 1;
    uint32_t j2 = (offset >> 17) & 1;
    value = (signbit << 26) | (j1 << 13) | (j2 << 11) | (imm6 << 16) |
        imm11;
    // Remove the offset from the current encoding.
    inst &= ~(0x3f << 16 | 0x7ff);
  }
  // Mask out offset bits in current instruction.
  inst &= ~(B26 | B13 | B11);
  inst |= value;
  return inst;
}


int Thumb2Assembler::DecodeBranchOffset(int32_t instr) {
  int32_t imm32;
  if ((instr & B12) == B12) {
    uint32_t S = (instr >> 26) & 1;
    uint32_t J2 = (instr >> 11) & 1;
    uint32_t J1 = (instr >> 13) & 1;
    uint32_t imm10 = (instr >> 16) & 0x3FF;
    uint32_t imm11 = instr & 0x7FF;

    uint32_t I1 = ~(J1 ^ S) & 1;
    uint32_t I2 = ~(J2 ^ S) & 1;
    imm32 = (S << 24) | (I1 << 23) | (I2 << 22) | (imm10 << 12) | (imm11 << 1);
    imm32 = (imm32 << 8) >> 8;  // sign extend 24 bit immediate.
  } else {
    uint32_t S = (instr >> 26) & 1;
    uint32_t J2 = (instr >> 11) & 1;
    uint32_t J1 = (instr >> 13) & 1;
    uint32_t imm6 = (instr >> 16) & 0x3F;
    uint32_t imm11 = instr & 0x7FF;

    imm32 = (S << 20) | (J2 << 19) | (J1 << 18) | (imm6 << 12) | (imm11 << 1);
    imm32 = (imm32 << 11) >> 11;  // sign extend 21 bit immediate.
  }
  imm32 += 4;
  return imm32;
}


void Thumb2Assembler::AddConstant(Register rd, int32_t value, Condition cond) {
  AddConstant(rd, rd, value, cond);
}


void Thumb2Assembler::AddConstant(Register rd, Register rn, int32_t value,
                                  Condition cond) {
  if (value == 0) {
    if (rd != rn) {
      mov(rd, ShifterOperand(rn), cond);
    }
    return;
  }
  // We prefer to select the shorter code sequence rather than selecting add for
  // positive values and sub for negatives ones, which would slightly improve
  // the readability of generated code for some constants.
  ShifterOperand shifter_op;
  if (ShifterOperand::CanHoldThumb(rd, rn, ADD, value, &shifter_op)) {
    add(rd, rn, shifter_op, cond);
  } else if (ShifterOperand::CanHoldThumb(rd, rn, SUB, -value, &shifter_op)) {
    sub(rd, rn, shifter_op, cond);
  } else {
    CHECK(rn != IP);
    if (ShifterOperand::CanHoldThumb(rd, rn, MVN, ~value, &shifter_op)) {
      mvn(IP, shifter_op, cond);
      add(rd, rn, ShifterOperand(IP), cond);
    } else if (ShifterOperand::CanHoldThumb(rd, rn, MVN, ~(-value), &shifter_op)) {
      mvn(IP, shifter_op, cond);
      sub(rd, rn, ShifterOperand(IP), cond);
    } else {
      movw(IP, Low16Bits(value), cond);
      uint16_t value_high = High16Bits(value);
      if (value_high != 0) {
        movt(IP, value_high, cond);
      }
      add(rd, rn, ShifterOperand(IP), cond);
    }
  }
}


void Thumb2Assembler::AddConstantSetFlags(Register rd, Register rn, int32_t value,
                                          Condition cond) {
  ShifterOperand shifter_op;
  if (ShifterOperand::CanHoldThumb(rd, rn, ADD, value, &shifter_op)) {
    adds(rd, rn, shifter_op, cond);
  } else if (ShifterOperand::CanHoldThumb(rd, rn, ADD, -value, &shifter_op)) {
    subs(rd, rn, shifter_op, cond);
  } else {
    CHECK(rn != IP);
    if (ShifterOperand::CanHoldThumb(rd, rn, MVN, ~value, &shifter_op)) {
      mvn(IP, shifter_op, cond);
      adds(rd, rn, ShifterOperand(IP), cond);
    } else if (ShifterOperand::CanHoldThumb(rd, rn, MVN, ~(-value), &shifter_op)) {
      mvn(IP, shifter_op, cond);
      subs(rd, rn, ShifterOperand(IP), cond);
    } else {
      movw(IP, Low16Bits(value), cond);
      uint16_t value_high = High16Bits(value);
      if (value_high != 0) {
        movt(IP, value_high, cond);
      }
      adds(rd, rn, ShifterOperand(IP), cond);
    }
  }
}


void Thumb2Assembler::LoadImmediate(Register rd, int32_t value, Condition cond) {
  ShifterOperand shifter_op;
  if (ShifterOperand::CanHoldThumb(rd, R0, MOV, value, &shifter_op)) {
    mov(rd, shifter_op, cond);
  } else if (ShifterOperand::CanHoldThumb(rd, R0, MVN, ~value, &shifter_op)) {
    mvn(rd, shifter_op, cond);
  } else {
    movw(rd, Low16Bits(value), cond);
    uint16_t value_high = High16Bits(value);
    if (value_high != 0) {
      movt(rd, value_high, cond);
    }
  }
}

// Implementation note: this method must emit at most one instruction when
// Address::CanHoldLoadOffsetThumb.
void Thumb2Assembler::LoadFromOffset(LoadOperandType type,
                                     Register reg,
                                     Register base,
                                     int32_t offset,
                                     Condition cond) {
  if (!Address::CanHoldLoadOffsetThumb(type, offset)) {
    CHECK(base != IP);
    LoadImmediate(IP, offset, cond);
    add(IP, IP, ShifterOperand(base), cond);
    base = IP;
    offset = 0;
  }
  CHECK(Address::CanHoldLoadOffsetThumb(type, offset));
  switch (type) {
    case kLoadSignedByte:
      ldrsb(reg, Address(base, offset), cond);
      break;
    case kLoadUnsignedByte:
      ldrb(reg, Address(base, offset), cond);
      break;
    case kLoadSignedHalfword:
      ldrsh(reg, Address(base, offset), cond);
      break;
    case kLoadUnsignedHalfword:
      ldrh(reg, Address(base, offset), cond);
      break;
    case kLoadWord:
      ldr(reg, Address(base, offset), cond);
      break;
    case kLoadWordPair:
      ldrd(reg, Address(base, offset), cond);
      break;
    default:
      LOG(FATAL) << "UNREACHABLE";
  }
}


// Implementation note: this method must emit at most one instruction when
// Address::CanHoldLoadOffsetThumb, as expected by JIT::GuardedLoadFromOffset.
void Thumb2Assembler::LoadSFromOffset(SRegister reg,
                                      Register base,
                                      int32_t offset,
                                      Condition cond) {
  if (!Address::CanHoldLoadOffsetThumb(kLoadSWord, offset)) {
    CHECK_NE(base, IP);
    LoadImmediate(IP, offset, cond);
    add(IP, IP, ShifterOperand(base), cond);
    base = IP;
    offset = 0;
  }
  CHECK(Address::CanHoldLoadOffsetThumb(kLoadSWord, offset));
  vldrs(reg, Address(base, offset), cond);
}


// Implementation note: this method must emit at most one instruction when
// Address::CanHoldLoadOffsetThumb, as expected by JIT::GuardedLoadFromOffset.
void Thumb2Assembler::LoadDFromOffset(DRegister reg,
                                      Register base,
                                      int32_t offset,
                                      Condition cond) {
  if (!Address::CanHoldLoadOffsetThumb(kLoadDWord, offset)) {
    CHECK_NE(base, IP);
    LoadImmediate(IP, offset, cond);
    add(IP, IP, ShifterOperand(base), cond);
    base = IP;
    offset = 0;
  }
  CHECK(Address::CanHoldLoadOffsetThumb(kLoadDWord, offset));
  vldrd(reg, Address(base, offset), cond);
}


// Implementation note: this method must emit at most one instruction when
// Address::CanHoldStoreOffsetThumb.
void Thumb2Assembler::StoreToOffset(StoreOperandType type,
                                    Register reg,
                                    Register base,
                                    int32_t offset,
                                    Condition cond) {
  if (!Address::CanHoldStoreOffsetThumb(type, offset)) {
    CHECK(reg != IP);
    CHECK(base != IP);
    LoadImmediate(IP, offset, cond);
    add(IP, IP, ShifterOperand(base), cond);
    base = IP;
    offset = 0;
  }
  CHECK(Address::CanHoldStoreOffsetThumb(type, offset));
  switch (type) {
    case kStoreByte:
      strb(reg, Address(base, offset), cond);
      break;
    case kStoreHalfword:
      strh(reg, Address(base, offset), cond);
      break;
    case kStoreWord:
      str(reg, Address(base, offset), cond);
      break;
    case kStoreWordPair:
      strd(reg, Address(base, offset), cond);
      break;
    default:
      LOG(FATAL) << "UNREACHABLE";
  }
}


// Implementation note: this method must emit at most one instruction when
// Address::CanHoldStoreOffsetThumb, as expected by JIT::GuardedStoreToOffset.
void Thumb2Assembler::StoreSToOffset(SRegister reg,
                                     Register base,
                                     int32_t offset,
                                     Condition cond) {
  if (!Address::CanHoldStoreOffsetThumb(kStoreSWord, offset)) {
    CHECK_NE(base, IP);
    LoadImmediate(IP, offset, cond);
    add(IP, IP, ShifterOperand(base), cond);
    base = IP;
    offset = 0;
  }
  CHECK(Address::CanHoldStoreOffsetThumb(kStoreSWord, offset));
  vstrs(reg, Address(base, offset), cond);
}


// Implementation note: this method must emit at most one instruction when
// Address::CanHoldStoreOffsetThumb, as expected by JIT::GuardedStoreSToOffset.
void Thumb2Assembler::StoreDToOffset(DRegister reg,
                                     Register base,
                                     int32_t offset,
                                     Condition cond) {
  if (!Address::CanHoldStoreOffsetThumb(kStoreDWord, offset)) {
    CHECK_NE(base, IP);
    LoadImmediate(IP, offset, cond);
    add(IP, IP, ShifterOperand(base), cond);
    base = IP;
    offset = 0;
  }
  CHECK(Address::CanHoldStoreOffsetThumb(kStoreDWord, offset));
  vstrd(reg, Address(base, offset), cond);
}


void Thumb2Assembler::MemoryBarrier(ManagedRegister mscratch) {
  CHECK_EQ(mscratch.AsArm().AsCoreRegister(), R12);
#if ANDROID_SMP != 0
  int32_t encoding = 0xf3bf8f5f;  // dmb in T1 encoding.
  Emit32(encoding);
#endif
}


void Thumb2Assembler::CompareAndBranchIfZero(Register r, Label* label) {
  if (force_32bit_branches_) {
    cmp(r, ShifterOperand(0));
    b(label, EQ);
  } else {
    cbz(r, label);
  }
}


void Thumb2Assembler::CompareAndBranchIfNonZero(Register r, Label* label) {
  if (force_32bit_branches_) {
    cmp(r, ShifterOperand(0));
    b(label, NE);
  } else {
    cbnz(r, label);
  }
}
}  // namespace arm
}  // namespace art
