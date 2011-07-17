// Copyright 2011 Google Inc. All Rights Reserved.

#include "src/assembler.h"
#include "src/logging.h"

namespace art {

// Instruction encoding bits.
enum {
  H   = 1 << 5,   // halfword (or byte)
  L   = 1 << 20,  // load (or store)
  S   = 1 << 20,  // set condition code (or leave unchanged)
  W   = 1 << 21,  // writeback base register (or leave unchanged)
  A   = 1 << 21,  // accumulate in multiply instruction (or not)
  B   = 1 << 22,  // unsigned byte (or word)
  N   = 1 << 22,  // long (or short)
  U   = 1 << 23,  // positive (or negative) offset/index
  P   = 1 << 24,  // offset/pre-indexed addressing (or post-indexed addressing)
  I   = 1 << 25,  // immediate shifter operand (or not)

  B0 = 1,
  B1 = 1 << 1,
  B2 = 1 << 2,
  B3 = 1 << 3,
  B4 = 1 << 4,
  B5 = 1 << 5,
  B6 = 1 << 6,
  B7 = 1 << 7,
  B8 = 1 << 8,
  B9 = 1 << 9,
  B10 = 1 << 10,
  B11 = 1 << 11,
  B12 = 1 << 12,
  B16 = 1 << 16,
  B17 = 1 << 17,
  B18 = 1 << 18,
  B19 = 1 << 19,
  B20 = 1 << 20,
  B21 = 1 << 21,
  B22 = 1 << 22,
  B23 = 1 << 23,
  B24 = 1 << 24,
  B25 = 1 << 25,
  B26 = 1 << 26,
  B27 = 1 << 27,

  // Instruction bit masks.
  RdMask = 15 << 12,  // in str instruction
  CondMask = 15 << 28,
  CoprocessorMask = 15 << 8,
  OpCodeMask = 15 << 21,  // in data-processing instructions
  Imm24Mask = (1 << 24) - 1,
  Off12Mask = (1 << 12) - 1,

  // ldrex/strex register field encodings.
  kLdExRnShift = 16,
  kLdExRtShift = 12,
  kStrExRnShift = 16,
  kStrExRdShift = 12,
  kStrExRtShift = 0,
};


static const char* kRegisterNames[] = {
  "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10",
  "fp", "ip", "sp", "lr", "pc"
};
std::ostream& operator<<(std::ostream& os, const Register& rhs) {
  if (rhs >= R0 && rhs <= PC) {
    os << kRegisterNames[rhs];
  } else {
     os << "Register[" << int(rhs) << "]";
  }
  return os;
}


std::ostream& operator<<(std::ostream& os, const SRegister& rhs) {
  if (rhs >= S0 && rhs < kNumberOfSRegisters) {
    os << "s" << int(rhs);
  } else {
    os << "SRegister[" << int(rhs) << "]";
  }
  return os;
}


std::ostream& operator<<(std::ostream& os, const DRegister& rhs) {
  if (rhs >= D0 && rhs < kNumberOfDRegisters) {
    os << "d" << int(rhs);
  } else {
    os << "DRegister[" << int(rhs) << "]";
  }
  return os;
}


static const char* kConditionNames[] = {
  "EQ", "NE", "CS", "CC", "MI", "PL", "VS", "VC", "HI", "LS", "GE", "LT", "GT", "LE", "AL",
};
std::ostream& operator<<(std::ostream& os, const Condition& rhs) {
  if (rhs >= EQ && rhs <= AL) {
    os << kConditionNames[rhs];
  } else {
    os << "Condition[" << int(rhs) << "]";
  }
  return os;
}


void Assembler::Emit(int32_t value) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  buffer_.Emit<int32_t>(value);
}


void Assembler::EmitType01(Condition cond,
                           int type,
                           Opcode opcode,
                           int set_cc,
                           Register rn,
                           Register rd,
                           ShifterOperand so) {
  CHECK_NE(rd, kNoRegister);
  CHECK_NE(cond, kNoCondition);
  int32_t encoding = static_cast<int32_t>(cond) << kConditionShift |
                     type << kTypeShift |
                     static_cast<int32_t>(opcode) << kOpcodeShift |
                     set_cc << kSShift |
                     static_cast<int32_t>(rn) << kRnShift |
                     static_cast<int32_t>(rd) << kRdShift |
                     so.encoding();
  Emit(encoding);
}


void Assembler::EmitType5(Condition cond, int offset, bool link) {
  CHECK_NE(cond, kNoCondition);
  int32_t encoding = static_cast<int32_t>(cond) << kConditionShift |
                     5 << kTypeShift |
                     (link ? 1 : 0) << kLinkShift;
  Emit(Assembler::EncodeBranchOffset(offset, encoding));
}


void Assembler::EmitMemOp(Condition cond,
                          bool load,
                          bool byte,
                          Register rd,
                          Address ad) {
  CHECK_NE(rd, kNoRegister);
  CHECK_NE(cond, kNoCondition);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B26 |
                     (load ? L : 0) |
                     (byte ? B : 0) |
                     (static_cast<int32_t>(rd) << kRdShift) |
                     ad.encoding();
  Emit(encoding);
}


void Assembler::EmitMemOpAddressMode3(Condition cond,
                                      int32_t mode,
                                      Register rd,
                                      Address ad) {
  CHECK_NE(rd, kNoRegister);
  CHECK_NE(cond, kNoCondition);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B22  |
                     mode |
                     (static_cast<int32_t>(rd) << kRdShift) |
                     ad.encoding3();
  Emit(encoding);
}


void Assembler::EmitMultiMemOp(Condition cond,
                               BlockAddressMode am,
                               bool load,
                               Register base,
                               RegList regs) {
  CHECK_NE(base, kNoRegister);
  CHECK_NE(cond, kNoCondition);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 |
                     am |
                     (load ? L : 0) |
                     (static_cast<int32_t>(base) << kRnShift) |
                     regs;
  Emit(encoding);
}


void Assembler::EmitShiftImmediate(Condition cond,
                                   Shift opcode,
                                   Register rd,
                                   Register rm,
                                   ShifterOperand so) {
  CHECK_NE(cond, kNoCondition);
  CHECK_EQ(so.type(), 1U);
  int32_t encoding = static_cast<int32_t>(cond) << kConditionShift |
                     static_cast<int32_t>(MOV) << kOpcodeShift |
                     static_cast<int32_t>(rd) << kRdShift |
                     so.encoding() << kShiftImmShift |
                     static_cast<int32_t>(opcode) << kShiftShift |
                     static_cast<int32_t>(rm);
  Emit(encoding);
}


void Assembler::EmitShiftRegister(Condition cond,
                                  Shift opcode,
                                  Register rd,
                                  Register rm,
                                  ShifterOperand so) {
  CHECK_NE(cond, kNoCondition);
  CHECK_EQ(so.type(), 0U);
  int32_t encoding = static_cast<int32_t>(cond) << kConditionShift |
                     static_cast<int32_t>(MOV) << kOpcodeShift |
                     static_cast<int32_t>(rd) << kRdShift |
                     so.encoding() << kShiftRegisterShift |
                     static_cast<int32_t>(opcode) << kShiftShift |
                     B4 |
                     static_cast<int32_t>(rm);
  Emit(encoding);
}


void Assembler::EmitBranch(Condition cond, Label* label, bool link) {
  if (label->IsBound()) {
    EmitType5(cond, label->Position() - buffer_.Size(), link);
  } else {
    int position = buffer_.Size();
    // Use the offset field of the branch instruction for linking the sites.
    EmitType5(cond, label->position_, link);
    label->LinkTo(position);
  }
}


void Assembler::and_(Register rd, Register rn, ShifterOperand so,
                     Condition cond) {
  EmitType01(cond, so.type(), AND, 0, rn, rd, so);
}


void Assembler::eor(Register rd, Register rn, ShifterOperand so,
                    Condition cond) {
  EmitType01(cond, so.type(), EOR, 0, rn, rd, so);
}


void Assembler::sub(Register rd, Register rn, ShifterOperand so,
                    Condition cond) {
  EmitType01(cond, so.type(), SUB, 0, rn, rd, so);
}

void Assembler::rsb(Register rd, Register rn, ShifterOperand so,
                    Condition cond) {
  EmitType01(cond, so.type(), RSB, 0, rn, rd, so);
}

void Assembler::rsbs(Register rd, Register rn, ShifterOperand so,
                     Condition cond) {
  EmitType01(cond, so.type(), RSB, 1, rn, rd, so);
}


void Assembler::add(Register rd, Register rn, ShifterOperand so,
                    Condition cond) {
  EmitType01(cond, so.type(), ADD, 0, rn, rd, so);
}


void Assembler::adds(Register rd, Register rn, ShifterOperand so,
                     Condition cond) {
  EmitType01(cond, so.type(), ADD, 1, rn, rd, so);
}


void Assembler::subs(Register rd, Register rn, ShifterOperand so,
                     Condition cond) {
  EmitType01(cond, so.type(), SUB, 1, rn, rd, so);
}


void Assembler::adc(Register rd, Register rn, ShifterOperand so,
                    Condition cond) {
  EmitType01(cond, so.type(), ADC, 0, rn, rd, so);
}


void Assembler::sbc(Register rd, Register rn, ShifterOperand so,
                    Condition cond) {
  EmitType01(cond, so.type(), SBC, 0, rn, rd, so);
}


void Assembler::rsc(Register rd, Register rn, ShifterOperand so,
                    Condition cond) {
  EmitType01(cond, so.type(), RSC, 0, rn, rd, so);
}


void Assembler::tst(Register rn, ShifterOperand so, Condition cond) {
  CHECK_NE(rn, PC);  // Reserve tst pc instruction for exception handler marker.
  EmitType01(cond, so.type(), TST, 1, rn, R0, so);
}


void Assembler::teq(Register rn, ShifterOperand so, Condition cond) {
  CHECK_NE(rn, PC);  // Reserve teq pc instruction for exception handler marker.
  EmitType01(cond, so.type(), TEQ, 1, rn, R0, so);
}


void Assembler::cmp(Register rn, ShifterOperand so, Condition cond) {
  EmitType01(cond, so.type(), CMP, 1, rn, R0, so);
}


void Assembler::cmn(Register rn, ShifterOperand so, Condition cond) {
  EmitType01(cond, so.type(), CMN, 1, rn, R0, so);
}


void Assembler::orr(Register rd, Register rn,
                    ShifterOperand so, Condition cond) {
  EmitType01(cond, so.type(), ORR, 0, rn, rd, so);
}


void Assembler::orrs(Register rd, Register rn,
                     ShifterOperand so, Condition cond) {
  EmitType01(cond, so.type(), ORR, 1, rn, rd, so);
}


void Assembler::mov(Register rd, ShifterOperand so, Condition cond) {
  EmitType01(cond, so.type(), MOV, 0, R0, rd, so);
}


void Assembler::movs(Register rd, ShifterOperand so, Condition cond) {
  EmitType01(cond, so.type(), MOV, 1, R0, rd, so);
}


void Assembler::bic(Register rd, Register rn, ShifterOperand so,
                    Condition cond) {
  EmitType01(cond, so.type(), BIC, 0, rn, rd, so);
}


void Assembler::mvn(Register rd, ShifterOperand so, Condition cond) {
  EmitType01(cond, so.type(), MVN, 0, R0, rd, so);
}


void Assembler::mvns(Register rd, ShifterOperand so, Condition cond) {
  EmitType01(cond, so.type(), MVN, 1, R0, rd, so);
}


void Assembler::clz(Register rd, Register rm, Condition cond) {
  CHECK_NE(rd, kNoRegister);
  CHECK_NE(rm, kNoRegister);
  CHECK_NE(cond, kNoCondition);
  CHECK_NE(rd, PC);
  CHECK_NE(rm, PC);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B24 | B22 | B21 | (0xf << 16) |
                     (static_cast<int32_t>(rd) << kRdShift) |
                     (0xf << 8) | B4 | static_cast<int32_t>(rm);
  Emit(encoding);
}


void Assembler::movw(Register rd, uint16_t imm16, Condition cond) {
  CHECK_NE(cond, kNoCondition);
  int32_t encoding = static_cast<int32_t>(cond) << kConditionShift |
                     B25 | B24 | ((imm16 >> 12) << 16) |
                     static_cast<int32_t>(rd) << kRdShift | (imm16 & 0xfff);
  Emit(encoding);
}


void Assembler::movt(Register rd, uint16_t imm16, Condition cond) {
  CHECK_NE(cond, kNoCondition);
  int32_t encoding = static_cast<int32_t>(cond) << kConditionShift |
                     B25 | B24 | B22 | ((imm16 >> 12) << 16) |
                     static_cast<int32_t>(rd) << kRdShift | (imm16 & 0xfff);
  Emit(encoding);
}


void Assembler::EmitMulOp(Condition cond, int32_t opcode,
                          Register rd, Register rn,
                          Register rm, Register rs) {
  CHECK_NE(rd, kNoRegister);
  CHECK_NE(rn, kNoRegister);
  CHECK_NE(rm, kNoRegister);
  CHECK_NE(rs, kNoRegister);
  CHECK_NE(cond, kNoCondition);
  int32_t encoding = opcode |
      (static_cast<int32_t>(cond) << kConditionShift) |
      (static_cast<int32_t>(rn) << kRnShift) |
      (static_cast<int32_t>(rd) << kRdShift) |
      (static_cast<int32_t>(rs) << kRsShift) |
      B7 | B4 |
      (static_cast<int32_t>(rm) << kRmShift);
  Emit(encoding);
}


void Assembler::mul(Register rd, Register rn,
                    Register rm, Condition cond) {
  // Assembler registers rd, rn, rm are encoded as rn, rm, rs.
  EmitMulOp(cond, 0, R0, rd, rn, rm);
}


void Assembler::mla(Register rd, Register rn,
                    Register rm, Register ra, Condition cond) {
  // Assembler registers rd, rn, rm, ra are encoded as rn, rm, rs, rd.
  EmitMulOp(cond, B21, ra, rd, rn, rm);
}


void Assembler::mls(Register rd, Register rn,
                    Register rm, Register ra, Condition cond) {
  // Assembler registers rd, rn, rm, ra are encoded as rn, rm, rs, rd.
  EmitMulOp(cond, B22 | B21, ra, rd, rn, rm);
}


void Assembler::umull(Register rd_lo, Register rd_hi,
                      Register rn, Register rm, Condition cond) {
  // Assembler registers rd_lo, rd_hi, rn, rm are encoded as rd, rn, rm, rs.
  EmitMulOp(cond, B23, rd_lo, rd_hi, rn, rm);
}


void Assembler::ldr(Register rd, Address ad, Condition cond) {
  EmitMemOp(cond, true, false, rd, ad);
}


void Assembler::str(Register rd, Address ad, Condition cond) {
  EmitMemOp(cond, false, false, rd, ad);
}


void Assembler::ldrb(Register rd, Address ad, Condition cond) {
  EmitMemOp(cond, true, true, rd, ad);
}


void Assembler::strb(Register rd, Address ad, Condition cond) {
  EmitMemOp(cond, false, true, rd, ad);
}


void Assembler::ldrh(Register rd, Address ad, Condition cond) {
  EmitMemOpAddressMode3(cond, L | B7 | H | B4, rd, ad);
}


void Assembler::strh(Register rd, Address ad, Condition cond) {
  EmitMemOpAddressMode3(cond, B7 | H | B4, rd, ad);
}


void Assembler::ldrsb(Register rd, Address ad, Condition cond) {
  EmitMemOpAddressMode3(cond, L | B7 | B6 | B4, rd, ad);
}


void Assembler::ldrsh(Register rd, Address ad, Condition cond) {
  EmitMemOpAddressMode3(cond, L | B7 | B6 | H | B4, rd, ad);
}


void Assembler::ldrd(Register rd, Address ad, Condition cond) {
  CHECK_EQ(rd % 2, 0);
  EmitMemOpAddressMode3(cond, B7 | B6 | B4, rd, ad);
}


void Assembler::strd(Register rd, Address ad, Condition cond) {
  CHECK_EQ(rd % 2, 0);
  EmitMemOpAddressMode3(cond, B7 | B6 | B5 | B4, rd, ad);
}


void Assembler::ldm(BlockAddressMode am,
                    Register base,
                    RegList regs,
                    Condition cond) {
  EmitMultiMemOp(cond, am, true, base, regs);
}


void Assembler::stm(BlockAddressMode am,
                    Register base,
                    RegList regs,
                    Condition cond) {
  EmitMultiMemOp(cond, am, false, base, regs);
}


void Assembler::ldrex(Register rt, Register rn, Condition cond) {
  CHECK_NE(rn, kNoRegister);
  CHECK_NE(rt, kNoRegister);
  CHECK_NE(cond, kNoCondition);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B24 |
                     B23 |
                     L   |
                     (static_cast<int32_t>(rn) << kLdExRnShift) |
                     (static_cast<int32_t>(rt) << kLdExRtShift) |
                     B11 | B10 | B9 | B8 | B7 | B4 | B3 | B2 | B1 | B0;
  Emit(encoding);
}


void Assembler::strex(Register rd,
                      Register rt,
                      Register rn,
                      Condition cond) {
  CHECK_NE(rn, kNoRegister);
  CHECK_NE(rd, kNoRegister);
  CHECK_NE(rt, kNoRegister);
  CHECK_NE(cond, kNoCondition);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B24 |
                     B23 |
                     (static_cast<int32_t>(rn) << kStrExRnShift) |
                     (static_cast<int32_t>(rd) << kStrExRdShift) |
                     B11 | B10 | B9 | B8 | B7 | B4 |
                     (static_cast<int32_t>(rt) << kStrExRtShift);
  Emit(encoding);
}


void Assembler::clrex() {
  int32_t encoding = (kSpecialCondition << kConditionShift) |
                     B26 | B24 | B22 | B21 | B20 | (0xff << 12) | B4 | 0xf;
  Emit(encoding);
}


void Assembler::nop(Condition cond) {
  CHECK_NE(cond, kNoCondition);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B25 | B24 | B21 | (0xf << 12);
  Emit(encoding);
}


void Assembler::vmovsr(SRegister sn, Register rt, Condition cond) {
  CHECK_NE(sn, kNoSRegister);
  CHECK_NE(rt, kNoRegister);
  CHECK_NE(rt, SP);
  CHECK_NE(rt, PC);
  CHECK_NE(cond, kNoCondition);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B25 |
                     ((static_cast<int32_t>(sn) >> 1)*B16) |
                     (static_cast<int32_t>(rt)*B12) | B11 | B9 |
                     ((static_cast<int32_t>(sn) & 1)*B7) | B4;
  Emit(encoding);
}


void Assembler::vmovrs(Register rt, SRegister sn, Condition cond) {
  CHECK_NE(sn, kNoSRegister);
  CHECK_NE(rt, kNoRegister);
  CHECK_NE(rt, SP);
  CHECK_NE(rt, PC);
  CHECK_NE(cond, kNoCondition);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B25 | B20 |
                     ((static_cast<int32_t>(sn) >> 1)*B16) |
                     (static_cast<int32_t>(rt)*B12) | B11 | B9 |
                     ((static_cast<int32_t>(sn) & 1)*B7) | B4;
  Emit(encoding);
}


void Assembler::vmovsrr(SRegister sm, Register rt, Register rt2,
                        Condition cond) {
  CHECK_NE(sm, kNoSRegister);
  CHECK_NE(sm, S31);
  CHECK_NE(rt, kNoRegister);
  CHECK_NE(rt, SP);
  CHECK_NE(rt, PC);
  CHECK_NE(rt2, kNoRegister);
  CHECK_NE(rt2, SP);
  CHECK_NE(rt2, PC);
  CHECK_NE(cond, kNoCondition);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B22 |
                     (static_cast<int32_t>(rt2)*B16) |
                     (static_cast<int32_t>(rt)*B12) | B11 | B9 |
                     ((static_cast<int32_t>(sm) & 1)*B5) | B4 |
                     (static_cast<int32_t>(sm) >> 1);
  Emit(encoding);
}


void Assembler::vmovrrs(Register rt, Register rt2, SRegister sm,
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
  CHECK_NE(cond, kNoCondition);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B22 | B20 |
                     (static_cast<int32_t>(rt2)*B16) |
                     (static_cast<int32_t>(rt)*B12) | B11 | B9 |
                     ((static_cast<int32_t>(sm) & 1)*B5) | B4 |
                     (static_cast<int32_t>(sm) >> 1);
  Emit(encoding);
}


void Assembler::vmovdrr(DRegister dm, Register rt, Register rt2,
                        Condition cond) {
  CHECK_NE(dm, kNoDRegister);
  CHECK_NE(rt, kNoRegister);
  CHECK_NE(rt, SP);
  CHECK_NE(rt, PC);
  CHECK_NE(rt2, kNoRegister);
  CHECK_NE(rt2, SP);
  CHECK_NE(rt2, PC);
  CHECK_NE(cond, kNoCondition);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B22 |
                     (static_cast<int32_t>(rt2)*B16) |
                     (static_cast<int32_t>(rt)*B12) | B11 | B9 | B8 |
                     ((static_cast<int32_t>(dm) >> 4)*B5) | B4 |
                     (static_cast<int32_t>(dm) & 0xf);
  Emit(encoding);
}


void Assembler::vmovrrd(Register rt, Register rt2, DRegister dm,
                              Condition cond) {
  CHECK_NE(dm, kNoDRegister);
  CHECK_NE(rt, kNoRegister);
  CHECK_NE(rt, SP);
  CHECK_NE(rt, PC);
  CHECK_NE(rt2, kNoRegister);
  CHECK_NE(rt2, SP);
  CHECK_NE(rt2, PC);
  CHECK_NE(rt, rt2);
  CHECK_NE(cond, kNoCondition);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B22 | B20 |
                     (static_cast<int32_t>(rt2)*B16) |
                     (static_cast<int32_t>(rt)*B12) | B11 | B9 | B8 |
                     ((static_cast<int32_t>(dm) >> 4)*B5) | B4 |
                     (static_cast<int32_t>(dm) & 0xf);
  Emit(encoding);
}


void Assembler::vldrs(SRegister sd, Address ad, Condition cond) {
  CHECK_NE(sd, kNoSRegister);
  CHECK_NE(cond, kNoCondition);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B24 | B20 |
                     ((static_cast<int32_t>(sd) & 1)*B22) |
                     ((static_cast<int32_t>(sd) >> 1)*B12) |
                     B11 | B9 | ad.vencoding();
  Emit(encoding);
}


void Assembler::vstrs(SRegister sd, Address ad, Condition cond) {
  CHECK_NE(static_cast<Register>(ad.encoding_ & (0xf << kRnShift)), PC);
  CHECK_NE(sd, kNoSRegister);
  CHECK_NE(cond, kNoCondition);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B24 |
                     ((static_cast<int32_t>(sd) & 1)*B22) |
                     ((static_cast<int32_t>(sd) >> 1)*B12) |
                     B11 | B9 | ad.vencoding();
  Emit(encoding);
}


void Assembler::vldrd(DRegister dd, Address ad, Condition cond) {
  CHECK_NE(dd, kNoDRegister);
  CHECK_NE(cond, kNoCondition);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B24 | B20 |
                     ((static_cast<int32_t>(dd) >> 4)*B22) |
                     ((static_cast<int32_t>(dd) & 0xf)*B12) |
                     B11 | B9 | B8 | ad.vencoding();
  Emit(encoding);
}


void Assembler::vstrd(DRegister dd, Address ad, Condition cond) {
  CHECK_NE(static_cast<Register>(ad.encoding_ & (0xf << kRnShift)), PC);
  CHECK_NE(dd, kNoDRegister);
  CHECK_NE(cond, kNoCondition);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B24 |
                     ((static_cast<int32_t>(dd) >> 4)*B22) |
                     ((static_cast<int32_t>(dd) & 0xf)*B12) |
                     B11 | B9 | B8 | ad.vencoding();
  Emit(encoding);
}


void Assembler::EmitVFPsss(Condition cond, int32_t opcode,
                           SRegister sd, SRegister sn, SRegister sm) {
  CHECK_NE(sd, kNoSRegister);
  CHECK_NE(sn, kNoSRegister);
  CHECK_NE(sm, kNoSRegister);
  CHECK_NE(cond, kNoCondition);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B25 | B11 | B9 | opcode |
                     ((static_cast<int32_t>(sd) & 1)*B22) |
                     ((static_cast<int32_t>(sn) >> 1)*B16) |
                     ((static_cast<int32_t>(sd) >> 1)*B12) |
                     ((static_cast<int32_t>(sn) & 1)*B7) |
                     ((static_cast<int32_t>(sm) & 1)*B5) |
                     (static_cast<int32_t>(sm) >> 1);
  Emit(encoding);
}


void Assembler::EmitVFPddd(Condition cond, int32_t opcode,
                           DRegister dd, DRegister dn, DRegister dm) {
  CHECK_NE(dd, kNoDRegister);
  CHECK_NE(dn, kNoDRegister);
  CHECK_NE(dm, kNoDRegister);
  CHECK_NE(cond, kNoCondition);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B25 | B11 | B9 | B8 | opcode |
                     ((static_cast<int32_t>(dd) >> 4)*B22) |
                     ((static_cast<int32_t>(dn) & 0xf)*B16) |
                     ((static_cast<int32_t>(dd) & 0xf)*B12) |
                     ((static_cast<int32_t>(dn) >> 4)*B7) |
                     ((static_cast<int32_t>(dm) >> 4)*B5) |
                     (static_cast<int32_t>(dm) & 0xf);
  Emit(encoding);
}


void Assembler::vmovs(SRegister sd, SRegister sm, Condition cond) {
  EmitVFPsss(cond, B23 | B21 | B20 | B6, sd, S0, sm);
}


void Assembler::vmovd(DRegister dd, DRegister dm, Condition cond) {
  EmitVFPddd(cond, B23 | B21 | B20 | B6, dd, D0, dm);
}


bool Assembler::vmovs(SRegister sd, float s_imm, Condition cond) {
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


bool Assembler::vmovd(DRegister dd, double d_imm, Condition cond) {
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


void Assembler::vadds(SRegister sd, SRegister sn, SRegister sm,
                      Condition cond) {
  EmitVFPsss(cond, B21 | B20, sd, sn, sm);
}


void Assembler::vaddd(DRegister dd, DRegister dn, DRegister dm,
                      Condition cond) {
  EmitVFPddd(cond, B21 | B20, dd, dn, dm);
}


void Assembler::vsubs(SRegister sd, SRegister sn, SRegister sm,
                      Condition cond) {
  EmitVFPsss(cond, B21 | B20 | B6, sd, sn, sm);
}


void Assembler::vsubd(DRegister dd, DRegister dn, DRegister dm,
                      Condition cond) {
  EmitVFPddd(cond, B21 | B20 | B6, dd, dn, dm);
}


void Assembler::vmuls(SRegister sd, SRegister sn, SRegister sm,
                      Condition cond) {
  EmitVFPsss(cond, B21, sd, sn, sm);
}


void Assembler::vmuld(DRegister dd, DRegister dn, DRegister dm,
                      Condition cond) {
  EmitVFPddd(cond, B21, dd, dn, dm);
}


void Assembler::vmlas(SRegister sd, SRegister sn, SRegister sm,
                      Condition cond) {
  EmitVFPsss(cond, 0, sd, sn, sm);
}


void Assembler::vmlad(DRegister dd, DRegister dn, DRegister dm,
                      Condition cond) {
  EmitVFPddd(cond, 0, dd, dn, dm);
}


void Assembler::vmlss(SRegister sd, SRegister sn, SRegister sm,
                      Condition cond) {
  EmitVFPsss(cond, B6, sd, sn, sm);
}


void Assembler::vmlsd(DRegister dd, DRegister dn, DRegister dm,
                      Condition cond) {
  EmitVFPddd(cond, B6, dd, dn, dm);
}


void Assembler::vdivs(SRegister sd, SRegister sn, SRegister sm,
                      Condition cond) {
  EmitVFPsss(cond, B23, sd, sn, sm);
}


void Assembler::vdivd(DRegister dd, DRegister dn, DRegister dm,
                      Condition cond) {
  EmitVFPddd(cond, B23, dd, dn, dm);
}


void Assembler::vabss(SRegister sd, SRegister sm, Condition cond) {
  EmitVFPsss(cond, B23 | B21 | B20 | B7 | B6, sd, S0, sm);
}


void Assembler::vabsd(DRegister dd, DRegister dm, Condition cond) {
  EmitVFPddd(cond, B23 | B21 | B20 | B7 | B6, dd, D0, dm);
}


void Assembler::vnegs(SRegister sd, SRegister sm, Condition cond) {
  EmitVFPsss(cond, B23 | B21 | B20 | B16 | B6, sd, S0, sm);
}


void Assembler::vnegd(DRegister dd, DRegister dm, Condition cond) {
  EmitVFPddd(cond, B23 | B21 | B20 | B16 | B6, dd, D0, dm);
}


void Assembler::vsqrts(SRegister sd, SRegister sm, Condition cond) {
  EmitVFPsss(cond, B23 | B21 | B20 | B16 | B7 | B6, sd, S0, sm);
}

void Assembler::vsqrtd(DRegister dd, DRegister dm, Condition cond) {
  EmitVFPddd(cond, B23 | B21 | B20 | B16 | B7 | B6, dd, D0, dm);
}


void Assembler::EmitVFPsd(Condition cond, int32_t opcode,
                          SRegister sd, DRegister dm) {
  CHECK_NE(sd, kNoSRegister);
  CHECK_NE(dm, kNoDRegister);
  CHECK_NE(cond, kNoCondition);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B25 | B11 | B9 | opcode |
                     ((static_cast<int32_t>(sd) & 1)*B22) |
                     ((static_cast<int32_t>(sd) >> 1)*B12) |
                     ((static_cast<int32_t>(dm) >> 4)*B5) |
                     (static_cast<int32_t>(dm) & 0xf);
  Emit(encoding);
}


void Assembler::EmitVFPds(Condition cond, int32_t opcode,
                          DRegister dd, SRegister sm) {
  CHECK_NE(dd, kNoDRegister);
  CHECK_NE(sm, kNoSRegister);
  CHECK_NE(cond, kNoCondition);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B25 | B11 | B9 | opcode |
                     ((static_cast<int32_t>(dd) >> 4)*B22) |
                     ((static_cast<int32_t>(dd) & 0xf)*B12) |
                     ((static_cast<int32_t>(sm) & 1)*B5) |
                     (static_cast<int32_t>(sm) >> 1);
  Emit(encoding);
}


void Assembler::vcvtsd(SRegister sd, DRegister dm, Condition cond) {
  EmitVFPsd(cond, B23 | B21 | B20 | B18 | B17 | B16 | B8 | B7 | B6, sd, dm);
}


void Assembler::vcvtds(DRegister dd, SRegister sm, Condition cond) {
  EmitVFPds(cond, B23 | B21 | B20 | B18 | B17 | B16 | B7 | B6, dd, sm);
}


void Assembler::vcvtis(SRegister sd, SRegister sm, Condition cond) {
  EmitVFPsss(cond, B23 | B21 | B20 | B19 | B18 | B16 | B7 | B6, sd, S0, sm);
}


void Assembler::vcvtid(SRegister sd, DRegister dm, Condition cond) {
  EmitVFPsd(cond, B23 | B21 | B20 | B19 | B18 | B16 | B8 | B7 | B6, sd, dm);
}


void Assembler::vcvtsi(SRegister sd, SRegister sm, Condition cond) {
  EmitVFPsss(cond, B23 | B21 | B20 | B19 | B7 | B6, sd, S0, sm);
}


void Assembler::vcvtdi(DRegister dd, SRegister sm, Condition cond) {
  EmitVFPds(cond, B23 | B21 | B20 | B19 | B8 | B7 | B6, dd, sm);
}


void Assembler::vcvtus(SRegister sd, SRegister sm, Condition cond) {
  EmitVFPsss(cond, B23 | B21 | B20 | B19 | B18 | B7 | B6, sd, S0, sm);
}


void Assembler::vcvtud(SRegister sd, DRegister dm, Condition cond) {
  EmitVFPsd(cond, B23 | B21 | B20 | B19 | B18 | B8 | B7 | B6, sd, dm);
}


void Assembler::vcvtsu(SRegister sd, SRegister sm, Condition cond) {
  EmitVFPsss(cond, B23 | B21 | B20 | B19 | B6, sd, S0, sm);
}


void Assembler::vcvtdu(DRegister dd, SRegister sm, Condition cond) {
  EmitVFPds(cond, B23 | B21 | B20 | B19 | B8 | B6, dd, sm);
}


void Assembler::vcmps(SRegister sd, SRegister sm, Condition cond) {
  EmitVFPsss(cond, B23 | B21 | B20 | B18 | B6, sd, S0, sm);
}


void Assembler::vcmpd(DRegister dd, DRegister dm, Condition cond) {
  EmitVFPddd(cond, B23 | B21 | B20 | B18 | B6, dd, D0, dm);
}


void Assembler::vcmpsz(SRegister sd, Condition cond) {
  EmitVFPsss(cond, B23 | B21 | B20 | B18 | B16 | B6, sd, S0, S0);
}


void Assembler::vcmpdz(DRegister dd, Condition cond) {
  EmitVFPddd(cond, B23 | B21 | B20 | B18 | B16 | B6, dd, D0, D0);
}


void Assembler::vmstat(Condition cond) {  // VMRS APSR_nzcv, FPSCR
  CHECK_NE(cond, kNoCondition);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B27 | B26 | B25 | B23 | B22 | B21 | B20 | B16 |
                     (static_cast<int32_t>(PC)*B12) |
                     B11 | B9 | B4;
  Emit(encoding);
}


void Assembler::svc(uint32_t imm24) {
  CHECK(IsUint(24, imm24));
  int32_t encoding = (AL << kConditionShift) | B27 | B26 | B25 | B24 | imm24;
  Emit(encoding);
}


void Assembler::bkpt(uint16_t imm16) {
  int32_t encoding = (AL << kConditionShift) | B24 | B21 |
                     ((imm16 >> 4) << 8) | B6 | B5 | B4 | (imm16 & 0xf);
  Emit(encoding);
}


void Assembler::b(Label* label, Condition cond) {
  EmitBranch(cond, label, false);
}


void Assembler::bl(Label* label, Condition cond) {
  EmitBranch(cond, label, true);
}


void Assembler::blx(Register rm, Condition cond) {
  CHECK_NE(rm, kNoRegister);
  CHECK_NE(cond, kNoCondition);
  int32_t encoding = (static_cast<int32_t>(cond) << kConditionShift) |
                     B24 | B21 | (0xfff << 8) | B5 | B4 |
                     (static_cast<int32_t>(rm) << kRmShift);
  Emit(encoding);
}


void Assembler::MarkExceptionHandler(Label* label) {
  EmitType01(AL, 1, TST, 1, PC, R0, ShifterOperand(0));
  Label l;
  b(&l);
  EmitBranch(AL, label, false);
  Bind(&l);
}


void Assembler::Bind(Label* label) {
  CHECK(!label->IsBound());
  int bound_pc = buffer_.Size();
  while (label->IsLinked()) {
    int32_t position = label->Position();
    int32_t next = buffer_.Load<int32_t>(position);
    int32_t encoded = Assembler::EncodeBranchOffset(bound_pc - position, next);
    buffer_.Store<int32_t>(position, encoded);
    label->position_ = Assembler::DecodeBranchOffset(next);
  }
  label->BindTo(bound_pc);
}


void Assembler::EncodeUint32InTstInstructions(uint32_t data) {
  // TODO: Consider using movw ip, <16 bits>.
  while (!IsUint(8, data)) {
    tst(R0, ShifterOperand(data & 0xFF), VS);
    data >>= 8;
  }
  tst(R0, ShifterOperand(data), MI);
}

int32_t Assembler::EncodeBranchOffset(int offset, int32_t inst) {
  // The offset is off by 8 due to the way the ARM CPUs read PC.
  offset -= 8;
  CHECK(IsAligned(offset, 4));
  CHECK(IsInt(CountOneBits(kBranchOffsetMask), offset));

  // Properly preserve only the bits supported in the instruction.
  offset >>= 2;
  offset &= kBranchOffsetMask;
  return (inst & ~kBranchOffsetMask) | offset;
}


int Assembler::DecodeBranchOffset(int32_t inst) {
  // Sign-extend, left-shift by 2, then add 8.
  return ((((inst & kBranchOffsetMask) << 8) >> 6) + 8);
}

}  // namespace art
