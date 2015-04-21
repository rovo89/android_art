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

#include "codegen_arm.h"

#include "arch/arm/instruction_set_features_arm.h"
#include "arm_lir.h"
#include "base/logging.h"
#include "dex/mir_graph.h"
#include "dex/quick/mir_to_lir-inl.h"
#include "dex/reg_storage_eq.h"
#include "driver/compiler_driver.h"

namespace art {

/* This file contains codegen for the Thumb ISA. */

static int32_t EncodeImmSingle(int32_t value) {
  int32_t res;
  int32_t bit_a =  (value & 0x80000000) >> 31;
  int32_t not_bit_b = (value & 0x40000000) >> 30;
  int32_t bit_b =  (value & 0x20000000) >> 29;
  int32_t b_smear =  (value & 0x3e000000) >> 25;
  int32_t slice =   (value & 0x01f80000) >> 19;
  int32_t zeroes =  (value & 0x0007ffff);
  if (zeroes != 0)
    return -1;
  if (bit_b) {
    if ((not_bit_b != 0) || (b_smear != 0x1f))
      return -1;
  } else {
    if ((not_bit_b != 1) || (b_smear != 0x0))
      return -1;
  }
  res = (bit_a << 7) | (bit_b << 6) | slice;
  return res;
}

/*
 * Determine whether value can be encoded as a Thumb2 floating point
 * immediate.  If not, return -1.  If so return encoded 8-bit value.
 */
static int32_t EncodeImmDouble(int64_t value) {
  int32_t res;
  int32_t bit_a = (value & INT64_C(0x8000000000000000)) >> 63;
  int32_t not_bit_b = (value & INT64_C(0x4000000000000000)) >> 62;
  int32_t bit_b = (value & INT64_C(0x2000000000000000)) >> 61;
  int32_t b_smear = (value & INT64_C(0x3fc0000000000000)) >> 54;
  int32_t slice =  (value & INT64_C(0x003f000000000000)) >> 48;
  uint64_t zeroes = (value & INT64_C(0x0000ffffffffffff));
  if (zeroes != 0ull)
    return -1;
  if (bit_b) {
    if ((not_bit_b != 0) || (b_smear != 0xff))
      return -1;
  } else {
    if ((not_bit_b != 1) || (b_smear != 0x0))
      return -1;
  }
  res = (bit_a << 7) | (bit_b << 6) | slice;
  return res;
}

LIR* ArmMir2Lir::LoadFPConstantValue(int r_dest, int value) {
  DCHECK(RegStorage::IsSingle(r_dest));
  if (value == 0) {
    // TODO: we need better info about the target CPU.  a vector exclusive or
    //       would probably be better here if we could rely on its existance.
    // Load an immediate +2.0 (which encodes to 0)
    NewLIR2(kThumb2Vmovs_IMM8, r_dest, 0);
    // +0.0 = +2.0 - +2.0
    return NewLIR3(kThumb2Vsubs, r_dest, r_dest, r_dest);
  } else {
    int encoded_imm = EncodeImmSingle(value);
    if (encoded_imm >= 0) {
      return NewLIR2(kThumb2Vmovs_IMM8, r_dest, encoded_imm);
    }
  }
  LIR* data_target = ScanLiteralPool(literal_list_, value, 0);
  if (data_target == nullptr) {
    data_target = AddWordData(&literal_list_, value);
  }
  ScopedMemRefType mem_ref_type(this, ResourceMask::kLiteral);
  LIR* load_pc_rel = RawLIR(current_dalvik_offset_, kThumb2Vldrs,
                          r_dest, rs_r15pc.GetReg(), 0, 0, 0, data_target);
  AppendLIR(load_pc_rel);
  return load_pc_rel;
}

/*
 * Determine whether value can be encoded as a Thumb2 modified
 * immediate.  If not, return -1.  If so, return i:imm3:a:bcdefgh form.
 */
int ArmMir2Lir::ModifiedImmediate(uint32_t value) {
  uint32_t b0 = value & 0xff;

  /* Note: case of value==0 must use 0:000:0:0000000 encoding */
  if (value <= 0xFF)
    return b0;  // 0:000:a:bcdefgh
  if (value == ((b0 << 16) | b0))
    return (0x1 << 8) | b0; /* 0:001:a:bcdefgh */
  if (value == ((b0 << 24) | (b0 << 16) | (b0 << 8) | b0))
    return (0x3 << 8) | b0; /* 0:011:a:bcdefgh */
  b0 = (value >> 8) & 0xff;
  if (value == ((b0 << 24) | (b0 << 8)))
    return (0x2 << 8) | b0; /* 0:010:a:bcdefgh */
  /* Can we do it with rotation? */
  int z_leading = CLZ(value);
  int z_trailing = CTZ(value);
  /* A run of eight or fewer active bits? */
  if ((z_leading + z_trailing) < 24)
    return -1;  /* No - bail */
  /* left-justify the constant, discarding msb (known to be 1) */
  value <<= z_leading + 1;
  /* Create bcdefgh */
  value >>= 25;
  /* Put it all together */
  return value | ((0x8 + z_leading) << 7); /* [01000..11111]:bcdefgh */
}

bool ArmMir2Lir::InexpensiveConstantInt(int32_t value) {
  return (ModifiedImmediate(value) >= 0) || (ModifiedImmediate(~value) >= 0);
}

bool ArmMir2Lir::InexpensiveConstantInt(int32_t value, Instruction::Code opcode) {
  switch (opcode) {
    case Instruction::ADD_INT:
    case Instruction::ADD_INT_2ADDR:
    case Instruction::SUB_INT:
    case Instruction::SUB_INT_2ADDR:
      if ((value >> 12) == (value >> 31)) {  // Signed 12-bit, RRI12 versions of ADD/SUB.
        return true;
      }
      FALLTHROUGH_INTENDED;
    case Instruction::IF_EQ:
    case Instruction::IF_NE:
    case Instruction::IF_LT:
    case Instruction::IF_GE:
    case Instruction::IF_GT:
    case Instruction::IF_LE:
      return (ModifiedImmediate(value) >= 0) || (ModifiedImmediate(-value) >= 0);
    case Instruction::SHL_INT:
    case Instruction::SHL_INT_2ADDR:
    case Instruction::SHR_INT:
    case Instruction::SHR_INT_2ADDR:
    case Instruction::USHR_INT:
    case Instruction::USHR_INT_2ADDR:
      return true;
    case Instruction::CONST:
    case Instruction::CONST_4:
    case Instruction::CONST_16:
      if ((value >> 16) == 0) {
        return true;  // movw, 16-bit unsigned.
      }
      FALLTHROUGH_INTENDED;
    case Instruction::AND_INT:
    case Instruction::AND_INT_2ADDR:
    case Instruction::AND_INT_LIT16:
    case Instruction::AND_INT_LIT8:
    case Instruction::OR_INT:
    case Instruction::OR_INT_2ADDR:
    case Instruction::OR_INT_LIT16:
    case Instruction::OR_INT_LIT8:
      return (ModifiedImmediate(value) >= 0) || (ModifiedImmediate(~value) >= 0);
    case Instruction::XOR_INT:
    case Instruction::XOR_INT_2ADDR:
    case Instruction::XOR_INT_LIT16:
    case Instruction::XOR_INT_LIT8:
      return (ModifiedImmediate(value) >= 0);
    case Instruction::MUL_INT:
    case Instruction::MUL_INT_2ADDR:
    case Instruction::MUL_INT_LIT8:
    case Instruction::MUL_INT_LIT16:
    case Instruction::DIV_INT:
    case Instruction::DIV_INT_2ADDR:
    case Instruction::DIV_INT_LIT8:
    case Instruction::DIV_INT_LIT16:
    case Instruction::REM_INT:
    case Instruction::REM_INT_2ADDR:
    case Instruction::REM_INT_LIT8:
    case Instruction::REM_INT_LIT16: {
      EasyMultiplyOp ops[2];
      return GetEasyMultiplyTwoOps(value, ops);
    }
    default:
      return false;
  }
}

bool ArmMir2Lir::InexpensiveConstantFloat(int32_t value) {
  return EncodeImmSingle(value) >= 0;
}

bool ArmMir2Lir::InexpensiveConstantLong(int64_t value) {
  return InexpensiveConstantInt(High32Bits(value)) && InexpensiveConstantInt(Low32Bits(value));
}

bool ArmMir2Lir::InexpensiveConstantDouble(int64_t value) {
  return EncodeImmDouble(value) >= 0;
}

/*
 * Load a immediate using a shortcut if possible; otherwise
 * grab from the per-translation literal pool.
 *
 * No additional register clobbering operation performed. Use this version when
 * 1) r_dest is freshly returned from AllocTemp or
 * 2) The codegen is under fixed register usage
 */
LIR* ArmMir2Lir::LoadConstantNoClobber(RegStorage r_dest, int value) {
  LIR* res;
  int mod_imm;

  if (r_dest.IsFloat()) {
    return LoadFPConstantValue(r_dest.GetReg(), value);
  }

  /* See if the value can be constructed cheaply */
  if (r_dest.Low8() && (value >= 0) && (value <= 255)) {
    return NewLIR2(kThumbMovImm, r_dest.GetReg(), value);
  }
  /* Check Modified immediate special cases */
  mod_imm = ModifiedImmediate(value);
  if (mod_imm >= 0) {
    res = NewLIR2(kThumb2MovI8M, r_dest.GetReg(), mod_imm);
    return res;
  }
  mod_imm = ModifiedImmediate(~value);
  if (mod_imm >= 0) {
    res = NewLIR2(kThumb2MvnI8M, r_dest.GetReg(), mod_imm);
    return res;
  }
  /* 16-bit immediate? */
  if ((value & 0xffff) == value) {
    res = NewLIR2(kThumb2MovImm16, r_dest.GetReg(), value);
    return res;
  }
  /* Do a low/high pair */
  res = NewLIR2(kThumb2MovImm16, r_dest.GetReg(), Low16Bits(value));
  NewLIR2(kThumb2MovImm16H, r_dest.GetReg(), High16Bits(value));
  return res;
}

LIR* ArmMir2Lir::OpUnconditionalBranch(LIR* target) {
  LIR* res = NewLIR1(kThumbBUncond, 0 /* offset to be patched  during assembly */);
  res->target = target;
  return res;
}

LIR* ArmMir2Lir::OpCondBranch(ConditionCode cc, LIR* target) {
  LIR* branch = NewLIR2(kThumbBCond, 0 /* offset to be patched */,
                        ArmConditionEncoding(cc));
  branch->target = target;
  return branch;
}

LIR* ArmMir2Lir::OpReg(OpKind op, RegStorage r_dest_src) {
  ArmOpcode opcode = kThumbBkpt;
  switch (op) {
    case kOpBlx:
      opcode = kThumbBlxR;
      break;
    case kOpBx:
      opcode = kThumbBx;
      break;
    default:
      LOG(FATAL) << "Bad opcode " << op;
  }
  return NewLIR1(opcode, r_dest_src.GetReg());
}

LIR* ArmMir2Lir::OpRegRegShift(OpKind op, RegStorage r_dest_src1, RegStorage r_src2,
                               int shift) {
  bool thumb_form =
      ((shift == 0) && r_dest_src1.Low8() && r_src2.Low8());
  ArmOpcode opcode = kThumbBkpt;
  switch (op) {
    case kOpAdc:
      opcode = (thumb_form) ? kThumbAdcRR : kThumb2AdcRRR;
      break;
    case kOpAnd:
      opcode = (thumb_form) ? kThumbAndRR : kThumb2AndRRR;
      break;
    case kOpBic:
      opcode = (thumb_form) ? kThumbBicRR : kThumb2BicRRR;
      break;
    case kOpCmn:
      DCHECK_EQ(shift, 0);
      opcode = (thumb_form) ? kThumbCmnRR : kThumb2CmnRR;
      break;
    case kOpCmp:
      if (thumb_form)
        opcode = kThumbCmpRR;
      else if ((shift == 0) && !r_dest_src1.Low8() && !r_src2.Low8())
        opcode = kThumbCmpHH;
      else if ((shift == 0) && r_dest_src1.Low8())
        opcode = kThumbCmpLH;
      else if (shift == 0)
        opcode = kThumbCmpHL;
      else
        opcode = kThumb2CmpRR;
      break;
    case kOpXor:
      opcode = (thumb_form) ? kThumbEorRR : kThumb2EorRRR;
      break;
    case kOpMov:
      DCHECK_EQ(shift, 0);
      if (r_dest_src1.Low8() && r_src2.Low8())
        opcode = kThumbMovRR;
      else if (!r_dest_src1.Low8() && !r_src2.Low8())
        opcode = kThumbMovRR_H2H;
      else if (r_dest_src1.Low8())
        opcode = kThumbMovRR_H2L;
      else
        opcode = kThumbMovRR_L2H;
      break;
    case kOpMul:
      DCHECK_EQ(shift, 0);
      opcode = (thumb_form) ? kThumbMul : kThumb2MulRRR;
      break;
    case kOpMvn:
      opcode = (thumb_form) ? kThumbMvn : kThumb2MnvRR;
      break;
    case kOpNeg:
      DCHECK_EQ(shift, 0);
      opcode = (thumb_form) ? kThumbNeg : kThumb2NegRR;
      break;
    case kOpOr:
      opcode = (thumb_form) ? kThumbOrr : kThumb2OrrRRR;
      break;
    case kOpSbc:
      opcode = (thumb_form) ? kThumbSbc : kThumb2SbcRRR;
      break;
    case kOpTst:
      opcode = (thumb_form) ? kThumbTst : kThumb2TstRR;
      break;
    case kOpLsl:
      DCHECK_EQ(shift, 0);
      opcode = (thumb_form) ? kThumbLslRR : kThumb2LslRRR;
      break;
    case kOpLsr:
      DCHECK_EQ(shift, 0);
      opcode = (thumb_form) ? kThumbLsrRR : kThumb2LsrRRR;
      break;
    case kOpAsr:
      DCHECK_EQ(shift, 0);
      opcode = (thumb_form) ? kThumbAsrRR : kThumb2AsrRRR;
      break;
    case kOpRor:
      DCHECK_EQ(shift, 0);
      opcode = (thumb_form) ? kThumbRorRR : kThumb2RorRRR;
      break;
    case kOpAdd:
      opcode = (thumb_form) ? kThumbAddRRR : kThumb2AddRRR;
      break;
    case kOpSub:
      opcode = (thumb_form) ? kThumbSubRRR : kThumb2SubRRR;
      break;
    case kOpRev:
      DCHECK_EQ(shift, 0);
      if (!thumb_form) {
        // Binary, but rm is encoded twice.
        return NewLIR3(kThumb2RevRR, r_dest_src1.GetReg(), r_src2.GetReg(), r_src2.GetReg());
      }
      opcode = kThumbRev;
      break;
    case kOpRevsh:
      DCHECK_EQ(shift, 0);
      if (!thumb_form) {
        // Binary, but rm is encoded twice.
        return NewLIR3(kThumb2RevshRR, r_dest_src1.GetReg(), r_src2.GetReg(), r_src2.GetReg());
      }
      opcode = kThumbRevsh;
      break;
    case kOp2Byte:
      DCHECK_EQ(shift, 0);
      return NewLIR4(kThumb2Sbfx, r_dest_src1.GetReg(), r_src2.GetReg(), 0, 8);
    case kOp2Short:
      DCHECK_EQ(shift, 0);
      return NewLIR4(kThumb2Sbfx, r_dest_src1.GetReg(), r_src2.GetReg(), 0, 16);
    case kOp2Char:
      DCHECK_EQ(shift, 0);
      return NewLIR4(kThumb2Ubfx, r_dest_src1.GetReg(), r_src2.GetReg(), 0, 16);
    default:
      LOG(FATAL) << "Bad opcode: " << op;
      break;
  }
  DCHECK(!IsPseudoLirOp(opcode));
  if (EncodingMap[opcode].flags & IS_BINARY_OP) {
    return NewLIR2(opcode, r_dest_src1.GetReg(), r_src2.GetReg());
  } else if (EncodingMap[opcode].flags & IS_TERTIARY_OP) {
    if (EncodingMap[opcode].field_loc[2].kind == kFmtShift) {
      return NewLIR3(opcode, r_dest_src1.GetReg(), r_src2.GetReg(), shift);
    } else {
      return NewLIR3(opcode, r_dest_src1.GetReg(), r_dest_src1.GetReg(), r_src2.GetReg());
    }
  } else if (EncodingMap[opcode].flags & IS_QUAD_OP) {
    return NewLIR4(opcode, r_dest_src1.GetReg(), r_dest_src1.GetReg(), r_src2.GetReg(), shift);
  } else {
    LOG(FATAL) << "Unexpected encoding operand count";
    return nullptr;
  }
}

LIR* ArmMir2Lir::OpRegReg(OpKind op, RegStorage r_dest_src1, RegStorage r_src2) {
  return OpRegRegShift(op, r_dest_src1, r_src2, 0);
}

LIR* ArmMir2Lir::OpMovRegMem(RegStorage r_dest, RegStorage r_base, int offset, MoveType move_type) {
  UNUSED(r_dest, r_base, offset, move_type);
  UNIMPLEMENTED(FATAL);
  UNREACHABLE();
}

LIR* ArmMir2Lir::OpMovMemReg(RegStorage r_base, int offset, RegStorage r_src, MoveType move_type) {
  UNUSED(r_base, offset, r_src, move_type);
  UNIMPLEMENTED(FATAL);
  UNREACHABLE();
}

LIR* ArmMir2Lir::OpCondRegReg(OpKind op, ConditionCode cc, RegStorage r_dest, RegStorage r_src) {
  UNUSED(op, cc, r_dest, r_src);
  LOG(FATAL) << "Unexpected use of OpCondRegReg for Arm";
  UNREACHABLE();
}

LIR* ArmMir2Lir::OpRegRegRegShift(OpKind op, RegStorage r_dest, RegStorage r_src1,
                                  RegStorage r_src2, int shift) {
  ArmOpcode opcode = kThumbBkpt;
  bool thumb_form = (shift == 0) && r_dest.Low8() && r_src1.Low8() && r_src2.Low8();
  switch (op) {
    case kOpAdd:
      opcode = (thumb_form) ? kThumbAddRRR : kThumb2AddRRR;
      break;
    case kOpSub:
      opcode = (thumb_form) ? kThumbSubRRR : kThumb2SubRRR;
      break;
    case kOpRsub:
      opcode = kThumb2RsubRRR;
      break;
    case kOpAdc:
      opcode = kThumb2AdcRRR;
      break;
    case kOpAnd:
      opcode = kThumb2AndRRR;
      break;
    case kOpBic:
      opcode = kThumb2BicRRR;
      break;
    case kOpXor:
      opcode = kThumb2EorRRR;
      break;
    case kOpMul:
      DCHECK_EQ(shift, 0);
      opcode = kThumb2MulRRR;
      break;
    case kOpDiv:
      DCHECK_EQ(shift, 0);
      opcode = kThumb2SdivRRR;
      break;
    case kOpOr:
      opcode = kThumb2OrrRRR;
      break;
    case kOpSbc:
      opcode = kThumb2SbcRRR;
      break;
    case kOpLsl:
      DCHECK_EQ(shift, 0);
      opcode = kThumb2LslRRR;
      break;
    case kOpLsr:
      DCHECK_EQ(shift, 0);
      opcode = kThumb2LsrRRR;
      break;
    case kOpAsr:
      DCHECK_EQ(shift, 0);
      opcode = kThumb2AsrRRR;
      break;
    case kOpRor:
      DCHECK_EQ(shift, 0);
      opcode = kThumb2RorRRR;
      break;
    default:
      LOG(FATAL) << "Bad opcode: " << op;
      break;
  }
  DCHECK(!IsPseudoLirOp(opcode));
  if (EncodingMap[opcode].flags & IS_QUAD_OP) {
    return NewLIR4(opcode, r_dest.GetReg(), r_src1.GetReg(), r_src2.GetReg(), shift);
  } else {
    DCHECK(EncodingMap[opcode].flags & IS_TERTIARY_OP);
    return NewLIR3(opcode, r_dest.GetReg(), r_src1.GetReg(), r_src2.GetReg());
  }
}

LIR* ArmMir2Lir::OpRegRegReg(OpKind op, RegStorage r_dest, RegStorage r_src1, RegStorage r_src2) {
  return OpRegRegRegShift(op, r_dest, r_src1, r_src2, 0);
}

LIR* ArmMir2Lir::OpRegRegImm(OpKind op, RegStorage r_dest, RegStorage r_src1, int value) {
  bool neg = (value < 0);
  int32_t abs_value = (neg) ? -value : value;
  ArmOpcode opcode = kThumbBkpt;
  ArmOpcode alt_opcode = kThumbBkpt;
  bool all_low_regs = r_dest.Low8() && r_src1.Low8();
  int32_t mod_imm = ModifiedImmediate(value);

  switch (op) {
    case kOpLsl:
      if (all_low_regs)
        return NewLIR3(kThumbLslRRI5, r_dest.GetReg(), r_src1.GetReg(), value);
      else
        return NewLIR3(kThumb2LslRRI5, r_dest.GetReg(), r_src1.GetReg(), value);
    case kOpLsr:
      if (all_low_regs)
        return NewLIR3(kThumbLsrRRI5, r_dest.GetReg(), r_src1.GetReg(), value);
      else
        return NewLIR3(kThumb2LsrRRI5, r_dest.GetReg(), r_src1.GetReg(), value);
    case kOpAsr:
      if (all_low_regs)
        return NewLIR3(kThumbAsrRRI5, r_dest.GetReg(), r_src1.GetReg(), value);
      else
        return NewLIR3(kThumb2AsrRRI5, r_dest.GetReg(), r_src1.GetReg(), value);
    case kOpRor:
      return NewLIR3(kThumb2RorRRI5, r_dest.GetReg(), r_src1.GetReg(), value);
    case kOpAdd:
      if (r_dest.Low8() && (r_src1 == rs_r13sp) && (value <= 1020) && ((value & 0x3) == 0)) {
        return NewLIR3(kThumbAddSpRel, r_dest.GetReg(), r_src1.GetReg(), value >> 2);
      } else if (r_dest.Low8() && (r_src1 == rs_r15pc) &&
          (value <= 1020) && ((value & 0x3) == 0)) {
        return NewLIR3(kThumbAddPcRel, r_dest.GetReg(), r_src1.GetReg(), value >> 2);
      }
      FALLTHROUGH_INTENDED;
    case kOpSub:
      if (all_low_regs && ((abs_value & 0x7) == abs_value)) {
        if (op == kOpAdd)
          opcode = (neg) ? kThumbSubRRI3 : kThumbAddRRI3;
        else
          opcode = (neg) ? kThumbAddRRI3 : kThumbSubRRI3;
        return NewLIR3(opcode, r_dest.GetReg(), r_src1.GetReg(), abs_value);
      }
      if (mod_imm < 0) {
        mod_imm = ModifiedImmediate(-value);
        if (mod_imm >= 0) {
          op = (op == kOpAdd) ? kOpSub : kOpAdd;
        }
      }
      if (mod_imm < 0 && (abs_value >> 12) == 0) {
        // This is deliberately used only if modified immediate encoding is inadequate since
        // we sometimes actually use the flags for small values but not necessarily low regs.
        if (op == kOpAdd)
          opcode = (neg) ? kThumb2SubRRI12 : kThumb2AddRRI12;
        else
          opcode = (neg) ? kThumb2AddRRI12 : kThumb2SubRRI12;
        return NewLIR3(opcode, r_dest.GetReg(), r_src1.GetReg(), abs_value);
      }
      if (op == kOpSub) {
        opcode = kThumb2SubRRI8M;
        alt_opcode = kThumb2SubRRR;
      } else {
        opcode = kThumb2AddRRI8M;
        alt_opcode = kThumb2AddRRR;
      }
      break;
    case kOpRsub:
      opcode = kThumb2RsubRRI8M;
      alt_opcode = kThumb2RsubRRR;
      break;
    case kOpAdc:
      opcode = kThumb2AdcRRI8M;
      alt_opcode = kThumb2AdcRRR;
      break;
    case kOpSbc:
      opcode = kThumb2SbcRRI8M;
      alt_opcode = kThumb2SbcRRR;
      break;
    case kOpOr:
      opcode = kThumb2OrrRRI8M;
      alt_opcode = kThumb2OrrRRR;
      if (mod_imm < 0) {
        mod_imm = ModifiedImmediate(~value);
        if (mod_imm >= 0) {
          opcode = kThumb2OrnRRI8M;
        }
      }
      break;
    case kOpAnd:
      if (mod_imm < 0) {
        mod_imm = ModifiedImmediate(~value);
        if (mod_imm >= 0) {
          return NewLIR3(kThumb2BicRRI8M, r_dest.GetReg(), r_src1.GetReg(), mod_imm);
        }
      }
      opcode = kThumb2AndRRI8M;
      alt_opcode = kThumb2AndRRR;
      break;
    case kOpXor:
      opcode = kThumb2EorRRI8M;
      alt_opcode = kThumb2EorRRR;
      break;
    case kOpMul:
      // TUNING: power of 2, shift & add
      mod_imm = -1;
      alt_opcode = kThumb2MulRRR;
      break;
    case kOpCmp: {
      LIR* res;
      if (mod_imm >= 0) {
        res = NewLIR2(kThumb2CmpRI8M, r_src1.GetReg(), mod_imm);
      } else {
        mod_imm = ModifiedImmediate(-value);
        if (mod_imm >= 0) {
          res = NewLIR2(kThumb2CmnRI8M, r_src1.GetReg(), mod_imm);
        } else {
          RegStorage r_tmp = AllocTemp();
          res = LoadConstant(r_tmp, value);
          OpRegReg(kOpCmp, r_src1, r_tmp);
          FreeTemp(r_tmp);
        }
      }
      return res;
    }
    default:
      LOG(FATAL) << "Bad opcode: " << op;
  }

  if (mod_imm >= 0) {
    return NewLIR3(opcode, r_dest.GetReg(), r_src1.GetReg(), mod_imm);
  } else {
    RegStorage r_scratch = AllocTemp();
    LoadConstant(r_scratch, value);
    LIR* res;
    if (EncodingMap[alt_opcode].flags & IS_QUAD_OP)
      res = NewLIR4(alt_opcode, r_dest.GetReg(), r_src1.GetReg(), r_scratch.GetReg(), 0);
    else
      res = NewLIR3(alt_opcode, r_dest.GetReg(), r_src1.GetReg(), r_scratch.GetReg());
    FreeTemp(r_scratch);
    return res;
  }
}

/* Handle Thumb-only variants here - otherwise punt to OpRegRegImm */
LIR* ArmMir2Lir::OpRegImm(OpKind op, RegStorage r_dest_src1, int value) {
  bool neg = (value < 0);
  int32_t abs_value = (neg) ? -value : value;
  bool short_form = (((abs_value & 0xff) == abs_value) && r_dest_src1.Low8());
  ArmOpcode opcode = kThumbBkpt;
  switch (op) {
    case kOpAdd:
      if (!neg && (r_dest_src1 == rs_r13sp) && (value <= 508)) { /* sp */
        DCHECK_EQ((value & 0x3), 0);
        return NewLIR1(kThumbAddSpI7, value >> 2);
      } else if (short_form) {
        opcode = (neg) ? kThumbSubRI8 : kThumbAddRI8;
      }
      break;
    case kOpSub:
      if (!neg && (r_dest_src1 == rs_r13sp) && (value <= 508)) { /* sp */
        DCHECK_EQ((value & 0x3), 0);
        return NewLIR1(kThumbSubSpI7, value >> 2);
      } else if (short_form) {
        opcode = (neg) ? kThumbAddRI8 : kThumbSubRI8;
      }
      break;
    case kOpCmp:
      if (!neg && short_form) {
        opcode = kThumbCmpRI8;
      } else {
        short_form = false;
      }
      break;
    default:
      /* Punt to OpRegRegImm - if bad case catch it there */
      short_form = false;
      break;
  }
  if (short_form) {
    return NewLIR2(opcode, r_dest_src1.GetReg(), abs_value);
  } else {
    return OpRegRegImm(op, r_dest_src1, r_dest_src1, value);
  }
}

LIR* ArmMir2Lir::LoadConstantWide(RegStorage r_dest, int64_t value) {
  LIR* res = nullptr;
  int32_t val_lo = Low32Bits(value);
  int32_t val_hi = High32Bits(value);
  if (r_dest.IsFloat()) {
    DCHECK(!r_dest.IsPair());
    if ((val_lo == 0) && (val_hi == 0)) {
      // TODO: we need better info about the target CPU.  a vector exclusive or
      //       would probably be better here if we could rely on its existance.
      // Load an immediate +2.0 (which encodes to 0)
      NewLIR2(kThumb2Vmovd_IMM8, r_dest.GetReg(), 0);
      // +0.0 = +2.0 - +2.0
      res = NewLIR3(kThumb2Vsubd, r_dest.GetReg(), r_dest.GetReg(), r_dest.GetReg());
    } else {
      int encoded_imm = EncodeImmDouble(value);
      if (encoded_imm >= 0) {
        res = NewLIR2(kThumb2Vmovd_IMM8, r_dest.GetReg(), encoded_imm);
      }
    }
  } else {
    // NOTE: Arm32 assumption here.
    DCHECK(r_dest.IsPair());
    if ((InexpensiveConstantInt(val_lo) && (InexpensiveConstantInt(val_hi)))) {
      res = LoadConstantNoClobber(r_dest.GetLow(), val_lo);
      LoadConstantNoClobber(r_dest.GetHigh(), val_hi);
    }
  }
  if (res == nullptr) {
    // No short form - load from the literal pool.
    LIR* data_target = ScanLiteralPoolWide(literal_list_, val_lo, val_hi);
    if (data_target == nullptr) {
      data_target = AddWideData(&literal_list_, val_lo, val_hi);
    }
    ScopedMemRefType mem_ref_type(this, ResourceMask::kLiteral);
    if (r_dest.IsFloat()) {
      res = RawLIR(current_dalvik_offset_, kThumb2Vldrd,
                   r_dest.GetReg(), rs_r15pc.GetReg(), 0, 0, 0, data_target);
    } else {
      DCHECK(r_dest.IsPair());
      res = RawLIR(current_dalvik_offset_, kThumb2LdrdPcRel8,
                   r_dest.GetLowReg(), r_dest.GetHighReg(), rs_r15pc.GetReg(), 0, 0, data_target);
    }
    AppendLIR(res);
  }
  return res;
}

int ArmMir2Lir::EncodeShift(int code, int amount) {
  return ((amount & 0x1f) << 2) | code;
}

LIR* ArmMir2Lir::LoadBaseIndexed(RegStorage r_base, RegStorage r_index, RegStorage r_dest,
                                 int scale, OpSize size) {
  bool all_low_regs = r_base.Low8() && r_index.Low8() && r_dest.Low8();
  LIR* load;
  ArmOpcode opcode = kThumbBkpt;
  bool thumb_form = (all_low_regs && (scale == 0));
  RegStorage reg_ptr;

  if (r_dest.IsFloat()) {
    if (r_dest.IsSingle()) {
      DCHECK((size == k32) || (size == kSingle) || (size == kReference));
      opcode = kThumb2Vldrs;
      size = kSingle;
    } else {
      DCHECK(r_dest.IsDouble());
      DCHECK((size == k64) || (size == kDouble));
      opcode = kThumb2Vldrd;
      size = kDouble;
    }
  } else {
    if (size == kSingle)
      size = k32;
  }

  switch (size) {
    case kDouble:  // fall-through
    // Intentional fall-though.
    case kSingle:
      reg_ptr = AllocTemp();
      if (scale) {
        NewLIR4(kThumb2AddRRR, reg_ptr.GetReg(), r_base.GetReg(), r_index.GetReg(),
                EncodeShift(kArmLsl, scale));
      } else {
        OpRegRegReg(kOpAdd, reg_ptr, r_base, r_index);
      }
      load = NewLIR3(opcode, r_dest.GetReg(), reg_ptr.GetReg(), 0);
      FreeTemp(reg_ptr);
      return load;
    case k32:
    // Intentional fall-though.
    case kReference:
      opcode = (thumb_form) ? kThumbLdrRRR : kThumb2LdrRRR;
      break;
    case kUnsignedHalf:
      opcode = (thumb_form) ? kThumbLdrhRRR : kThumb2LdrhRRR;
      break;
    case kSignedHalf:
      opcode = (thumb_form) ? kThumbLdrshRRR : kThumb2LdrshRRR;
      break;
    case kUnsignedByte:
      opcode = (thumb_form) ? kThumbLdrbRRR : kThumb2LdrbRRR;
      break;
    case kSignedByte:
      opcode = (thumb_form) ? kThumbLdrsbRRR : kThumb2LdrsbRRR;
      break;
    default:
      LOG(FATAL) << "Bad size: " << size;
  }
  if (thumb_form)
    load = NewLIR3(opcode, r_dest.GetReg(), r_base.GetReg(), r_index.GetReg());
  else
    load = NewLIR4(opcode, r_dest.GetReg(), r_base.GetReg(), r_index.GetReg(), scale);

  return load;
}

LIR* ArmMir2Lir::StoreBaseIndexed(RegStorage r_base, RegStorage r_index, RegStorage r_src,
                                  int scale, OpSize size) {
  bool all_low_regs = r_base.Low8() && r_index.Low8() && r_src.Low8();
  LIR* store = nullptr;
  ArmOpcode opcode = kThumbBkpt;
  bool thumb_form = (all_low_regs && (scale == 0));
  RegStorage reg_ptr;

  if (r_src.IsFloat()) {
    if (r_src.IsSingle()) {
      DCHECK((size == k32) || (size == kSingle) || (size == kReference));
      opcode = kThumb2Vstrs;
      size = kSingle;
    } else {
      DCHECK(r_src.IsDouble());
      DCHECK((size == k64) || (size == kDouble));
      DCHECK_EQ((r_src.GetReg() & 0x1), 0);
      opcode = kThumb2Vstrd;
      size = kDouble;
    }
  } else {
    if (size == kSingle)
      size = k32;
  }

  switch (size) {
    case kDouble:  // fall-through
    // Intentional fall-though.
    case kSingle:
      reg_ptr = AllocTemp();
      if (scale) {
        NewLIR4(kThumb2AddRRR, reg_ptr.GetReg(), r_base.GetReg(), r_index.GetReg(),
                EncodeShift(kArmLsl, scale));
      } else {
        OpRegRegReg(kOpAdd, reg_ptr, r_base, r_index);
      }
      store = NewLIR3(opcode, r_src.GetReg(), reg_ptr.GetReg(), 0);
      FreeTemp(reg_ptr);
      return store;
    case k32:
    // Intentional fall-though.
    case kReference:
      opcode = (thumb_form) ? kThumbStrRRR : kThumb2StrRRR;
      break;
    case kUnsignedHalf:
    // Intentional fall-though.
    case kSignedHalf:
      opcode = (thumb_form) ? kThumbStrhRRR : kThumb2StrhRRR;
      break;
    case kUnsignedByte:
    // Intentional fall-though.
    case kSignedByte:
      opcode = (thumb_form) ? kThumbStrbRRR : kThumb2StrbRRR;
      break;
    default:
      LOG(FATAL) << "Bad size: " << size;
  }
  if (thumb_form)
    store = NewLIR3(opcode, r_src.GetReg(), r_base.GetReg(), r_index.GetReg());
  else
    store = NewLIR4(opcode, r_src.GetReg(), r_base.GetReg(), r_index.GetReg(), scale);

  return store;
}

// Helper function for LoadBaseDispBody()/StoreBaseDispBody().
LIR* ArmMir2Lir::LoadStoreUsingInsnWithOffsetImm8Shl2(ArmOpcode opcode, RegStorage r_base,
                                                      int displacement, RegStorage r_src_dest,
                                                      RegStorage r_work) {
  DCHECK_EQ(displacement & 3, 0);
  constexpr int kOffsetMask = 0xff << 2;
  int encoded_disp = (displacement & kOffsetMask) >> 2;  // Within range of the instruction.
  RegStorage r_ptr = r_base;
  if ((displacement & ~kOffsetMask) != 0) {
    r_ptr = r_work.Valid() ? r_work : AllocTemp();
    // Add displacement & ~kOffsetMask to base, it's a single instruction for up to +-256KiB.
    OpRegRegImm(kOpAdd, r_ptr, r_base, displacement & ~kOffsetMask);
  }
  LIR* lir = nullptr;
  if (!r_src_dest.IsPair()) {
    lir = NewLIR3(opcode, r_src_dest.GetReg(), r_ptr.GetReg(), encoded_disp);
  } else {
    lir = NewLIR4(opcode, r_src_dest.GetLowReg(), r_src_dest.GetHighReg(), r_ptr.GetReg(),
                  encoded_disp);
  }
  if ((displacement & ~kOffsetMask) != 0 && !r_work.Valid()) {
    FreeTemp(r_ptr);
  }
  return lir;
}

/*
 * Load value from base + displacement.  Optionally perform null check
 * on base (which must have an associated s_reg and MIR).  If not
 * performing null check, incoming MIR can be null.
 */
LIR* ArmMir2Lir::LoadBaseDispBody(RegStorage r_base, int displacement, RegStorage r_dest,
                                  OpSize size) {
  LIR* load = nullptr;
  ArmOpcode opcode16 = kThumbBkpt;  // 16-bit Thumb opcode.
  ArmOpcode opcode32 = kThumbBkpt;  // 32-bit Thumb2 opcode.
  bool short_form = false;
  bool all_low = r_dest.Is32Bit() && r_base.Low8() && r_dest.Low8();
  int scale = 0;  // Used for opcode16 and some indexed loads.
  bool already_generated = false;
  switch (size) {
    case kDouble:
    // Intentional fall-though.
    case k64:
      if (r_dest.IsFloat()) {
        DCHECK(!r_dest.IsPair());
        load = LoadStoreUsingInsnWithOffsetImm8Shl2(kThumb2Vldrd, r_base, displacement, r_dest);
      } else {
        DCHECK(r_dest.IsPair());
        // Use the r_dest.GetLow() for the temporary pointer if needed.
        load = LoadStoreUsingInsnWithOffsetImm8Shl2(kThumb2LdrdI8, r_base, displacement, r_dest,
                                                    r_dest.GetLow());
      }
      already_generated = true;
      break;
    case kSingle:
    // Intentional fall-though.
    case k32:
    // Intentional fall-though.
    case kReference:
      if (r_dest.IsFloat()) {
        DCHECK(r_dest.IsSingle());
        load = LoadStoreUsingInsnWithOffsetImm8Shl2(kThumb2Vldrs, r_base, displacement, r_dest);
        already_generated = true;
        break;
      }
      DCHECK_EQ((displacement & 0x3), 0);
      scale = 2;
      if (r_dest.Low8() && (r_base == rs_rARM_PC) && (displacement <= 1020) &&
          (displacement >= 0)) {
        short_form = true;
        opcode16 = kThumbLdrPcRel;
      } else if (r_dest.Low8() && (r_base == rs_rARM_SP) && (displacement <= 1020) &&
                 (displacement >= 0)) {
        short_form = true;
        opcode16 = kThumbLdrSpRel;
      } else {
        short_form = all_low && (displacement >> (5 + scale)) == 0;
        opcode16 = kThumbLdrRRI5;
        opcode32 = kThumb2LdrRRI12;
      }
      break;
    case kUnsignedHalf:
      DCHECK_EQ((displacement & 0x1), 0);
      scale = 1;
      short_form = all_low && (displacement >> (5 + scale)) == 0;
      opcode16 = kThumbLdrhRRI5;
      opcode32 = kThumb2LdrhRRI12;
      break;
    case kSignedHalf:
      DCHECK_EQ((displacement & 0x1), 0);
      scale = 1;
      DCHECK_EQ(opcode16, kThumbBkpt);  // Not available.
      opcode32 = kThumb2LdrshRRI12;
      break;
    case kUnsignedByte:
      DCHECK_EQ(scale, 0);  // Keep scale = 0.
      short_form = all_low && (displacement >> (5 + scale)) == 0;
      opcode16 = kThumbLdrbRRI5;
      opcode32 = kThumb2LdrbRRI12;
      break;
    case kSignedByte:
      DCHECK_EQ(scale, 0);  // Keep scale = 0.
      DCHECK_EQ(opcode16, kThumbBkpt);  // Not available.
      opcode32 = kThumb2LdrsbRRI12;
      break;
    default:
      LOG(FATAL) << "Bad size: " << size;
  }

  if (!already_generated) {
    if (short_form) {
      load = NewLIR3(opcode16, r_dest.GetReg(), r_base.GetReg(), displacement >> scale);
    } else if ((displacement >> 12) == 0) {  // Thumb2 form.
      load = NewLIR3(opcode32, r_dest.GetReg(), r_base.GetReg(), displacement);
    } else if (!InexpensiveConstantInt(displacement >> scale, Instruction::CONST) &&
        InexpensiveConstantInt(displacement & ~0x00000fff, Instruction::ADD_INT)) {
      // In this case, using LoadIndexed would emit 3 insns (movw+movt+ldr) but we can
      // actually do it in two because we know that the kOpAdd is a single insn. On the
      // other hand, we introduce an extra dependency, so this is not necessarily faster.
      if (opcode16 != kThumbBkpt && r_dest.Low8() &&
          InexpensiveConstantInt(displacement & ~(0x1f << scale), Instruction::ADD_INT)) {
        // We can use the 16-bit Thumb opcode for the load.
        OpRegRegImm(kOpAdd, r_dest, r_base, displacement & ~(0x1f << scale));
        load = NewLIR3(opcode16, r_dest.GetReg(), r_dest.GetReg(), (displacement >> scale) & 0x1f);
      } else {
        DCHECK_NE(opcode32, kThumbBkpt);
        OpRegRegImm(kOpAdd, r_dest, r_base, displacement & ~0x00000fff);
        load = NewLIR3(opcode32, r_dest.GetReg(), r_dest.GetReg(), displacement & 0x00000fff);
      }
    } else {
      if (!InexpensiveConstantInt(displacement >> scale, Instruction::CONST) ||
          (scale != 0 && InexpensiveConstantInt(displacement, Instruction::CONST))) {
        scale = 0;  // Prefer unscaled indexing if the same number of insns.
      }
      RegStorage reg_offset = AllocTemp();
      LoadConstant(reg_offset, displacement >> scale);
      DCHECK(!r_dest.IsFloat());
      load = LoadBaseIndexed(r_base, reg_offset, r_dest, scale, size);
      FreeTemp(reg_offset);
    }
  }

  // TODO: in future may need to differentiate Dalvik accesses w/ spills
  if (mem_ref_type_ == ResourceMask::kDalvikReg) {
    DCHECK_EQ(r_base, rs_rARM_SP);
    AnnotateDalvikRegAccess(load, displacement >> 2, true /* is_load */, r_dest.Is64Bit());
  }
  return load;
}

LIR* ArmMir2Lir::LoadBaseDisp(RegStorage r_base, int displacement, RegStorage r_dest,
                              OpSize size, VolatileKind is_volatile) {
  // TODO: base this on target.
  if (size == kWord) {
    size = k32;
  }
  LIR* load;
  if (is_volatile == kVolatile && (size == k64 || size == kDouble) &&
      !cu_->compiler_driver->GetInstructionSetFeatures()->
          AsArmInstructionSetFeatures()->HasAtomicLdrdAndStrd()) {
    // Only 64-bit load needs special handling.
    // If the cpu supports LPAE, aligned LDRD is atomic - fall through to LoadBaseDisp().
    DCHECK(!r_dest.IsFloat());  // See RegClassForFieldLoadSave().
    // Use LDREXD for the atomic load. (Expect displacement > 0, don't optimize for == 0.)
    RegStorage r_ptr = AllocTemp();
    OpRegRegImm(kOpAdd, r_ptr, r_base, displacement);
    load = NewLIR3(kThumb2Ldrexd, r_dest.GetLowReg(), r_dest.GetHighReg(), r_ptr.GetReg());
    FreeTemp(r_ptr);
  } else {
    load = LoadBaseDispBody(r_base, displacement, r_dest, size);
  }

  if (UNLIKELY(is_volatile == kVolatile)) {
    GenMemBarrier(kLoadAny);
  }

  return load;
}


LIR* ArmMir2Lir::StoreBaseDispBody(RegStorage r_base, int displacement, RegStorage r_src,
                                   OpSize size) {
  LIR* store = nullptr;
  ArmOpcode opcode16 = kThumbBkpt;  // 16-bit Thumb opcode.
  ArmOpcode opcode32 = kThumbBkpt;  // 32-bit Thumb2 opcode.
  bool short_form = false;
  bool all_low = r_src.Is32Bit() && r_base.Low8() && r_src.Low8();
  int scale = 0;  // Used for opcode16 and some indexed loads.
  bool already_generated = false;
  switch (size) {
    case kDouble:
    // Intentional fall-though.
    case k64:
      if (r_src.IsFloat()) {
        // Note: If the register is retrieved by register allocator, it should never be a pair.
        // But some functions in mir2lir assume 64-bit registers are 32-bit register pairs.
        // TODO: Rework Mir2Lir::LoadArg() and Mir2Lir::LoadArgDirect().
        if (r_src.IsPair()) {
          r_src = As64BitFloatReg(r_src);
        }
        DCHECK(!r_src.IsPair());
        store = LoadStoreUsingInsnWithOffsetImm8Shl2(kThumb2Vstrd, r_base, displacement, r_src);
      } else {
        DCHECK(r_src.IsPair());
        store = LoadStoreUsingInsnWithOffsetImm8Shl2(kThumb2StrdI8, r_base, displacement, r_src);
      }
      already_generated = true;
      break;
    case kSingle:
    // Intentional fall-through.
    case k32:
    // Intentional fall-through.
    case kReference:
      if (r_src.IsFloat()) {
        DCHECK(r_src.IsSingle());
        store = LoadStoreUsingInsnWithOffsetImm8Shl2(kThumb2Vstrs, r_base, displacement, r_src);
        already_generated = true;
        break;
      }
      DCHECK_EQ((displacement & 0x3), 0);
      scale = 2;
      if (r_src.Low8() && (r_base == rs_r13sp) && (displacement <= 1020) && (displacement >= 0)) {
        short_form = true;
        opcode16 = kThumbStrSpRel;
      } else {
        short_form = all_low && (displacement >> (5 + scale)) == 0;
        opcode16 = kThumbStrRRI5;
        opcode32 = kThumb2StrRRI12;
      }
      break;
    case kUnsignedHalf:
    case kSignedHalf:
      DCHECK_EQ((displacement & 0x1), 0);
      scale = 1;
      short_form = all_low && (displacement >> (5 + scale)) == 0;
      opcode16 = kThumbStrhRRI5;
      opcode32 = kThumb2StrhRRI12;
      break;
    case kUnsignedByte:
    case kSignedByte:
      DCHECK_EQ(scale, 0);  // Keep scale = 0.
      short_form = all_low && (displacement >> (5 + scale)) == 0;
      opcode16 = kThumbStrbRRI5;
      opcode32 = kThumb2StrbRRI12;
      break;
    default:
      LOG(FATAL) << "Bad size: " << size;
  }
  if (!already_generated) {
    if (short_form) {
      store = NewLIR3(opcode16, r_src.GetReg(), r_base.GetReg(), displacement >> scale);
    } else if ((displacement >> 12) == 0) {
      store = NewLIR3(opcode32, r_src.GetReg(), r_base.GetReg(), displacement);
    } else if (!InexpensiveConstantInt(displacement >> scale, Instruction::CONST) &&
        InexpensiveConstantInt(displacement & ~0x00000fff, Instruction::ADD_INT)) {
      // In this case, using StoreIndexed would emit 3 insns (movw+movt+str) but we can
      // actually do it in two because we know that the kOpAdd is a single insn. On the
      // other hand, we introduce an extra dependency, so this is not necessarily faster.
      RegStorage r_scratch = AllocTemp();
      if (opcode16 != kThumbBkpt && r_src.Low8() && r_scratch.Low8() &&
          InexpensiveConstantInt(displacement & ~(0x1f << scale), Instruction::ADD_INT)) {
        // We can use the 16-bit Thumb opcode for the load.
        OpRegRegImm(kOpAdd, r_scratch, r_base, displacement & ~(0x1f << scale));
        store = NewLIR3(opcode16, r_src.GetReg(), r_scratch.GetReg(),
                        (displacement >> scale) & 0x1f);
      } else {
        DCHECK_NE(opcode32, kThumbBkpt);
        OpRegRegImm(kOpAdd, r_scratch, r_base, displacement & ~0x00000fff);
        store = NewLIR3(opcode32, r_src.GetReg(), r_scratch.GetReg(), displacement & 0x00000fff);
      }
      FreeTemp(r_scratch);
    } else {
      if (!InexpensiveConstantInt(displacement >> scale, Instruction::CONST) ||
          (scale != 0 && InexpensiveConstantInt(displacement, Instruction::CONST))) {
        scale = 0;  // Prefer unscaled indexing if the same number of insns.
      }
      RegStorage r_scratch = AllocTemp();
      LoadConstant(r_scratch, displacement >> scale);
      DCHECK(!r_src.IsFloat());
      store = StoreBaseIndexed(r_base, r_scratch, r_src, scale, size);
      FreeTemp(r_scratch);
    }
  }

  // TODO: In future, may need to differentiate Dalvik & spill accesses
  if (mem_ref_type_ == ResourceMask::kDalvikReg) {
    DCHECK_EQ(r_base, rs_rARM_SP);
    AnnotateDalvikRegAccess(store, displacement >> 2, false /* is_load */, r_src.Is64Bit());
  }
  return store;
}

LIR* ArmMir2Lir::StoreBaseDisp(RegStorage r_base, int displacement, RegStorage r_src,
                               OpSize size, VolatileKind is_volatile) {
  if (UNLIKELY(is_volatile == kVolatile)) {
    // Ensure that prior accesses become visible to other threads first.
    GenMemBarrier(kAnyStore);
  }

  LIR* null_ck_insn;
  if (is_volatile == kVolatile && (size == k64 || size == kDouble) &&
      !cu_->compiler_driver->GetInstructionSetFeatures()->
          AsArmInstructionSetFeatures()->HasAtomicLdrdAndStrd()) {
    // Only 64-bit store needs special handling.
    // If the cpu supports LPAE, aligned STRD is atomic - fall through to StoreBaseDisp().
    // Use STREXD for the atomic store. (Expect displacement > 0, don't optimize for == 0.)
    DCHECK(!r_src.IsFloat());  // See RegClassForFieldLoadSave().
    RegStorage r_ptr = AllocTemp();
    OpRegRegImm(kOpAdd, r_ptr, r_base, displacement);
    LIR* fail_target = NewLIR0(kPseudoTargetLabel);
    // We have only 5 temporary registers available and if r_base, r_src and r_ptr already
    // take 4, we can't directly allocate 2 more for LDREXD temps. In that case clobber r_ptr
    // in LDREXD and recalculate it from r_base.
    RegStorage r_temp = AllocTemp();
    RegStorage r_temp_high = AllocTemp(false);  // We may not have another temp.
    if (r_temp_high.Valid()) {
      null_ck_insn = NewLIR3(kThumb2Ldrexd, r_temp.GetReg(), r_temp_high.GetReg(), r_ptr.GetReg());
      FreeTemp(r_temp_high);
      FreeTemp(r_temp);
    } else {
      // If we don't have another temp, clobber r_ptr in LDREXD and reload it.
      null_ck_insn = NewLIR3(kThumb2Ldrexd, r_temp.GetReg(), r_ptr.GetReg(), r_ptr.GetReg());
      FreeTemp(r_temp);  // May need the temp for kOpAdd.
      OpRegRegImm(kOpAdd, r_ptr, r_base, displacement);
    }
    NewLIR4(kThumb2Strexd, r_temp.GetReg(), r_src.GetLowReg(), r_src.GetHighReg(), r_ptr.GetReg());
    OpCmpImmBranch(kCondNe, r_temp, 0, fail_target);
    FreeTemp(r_ptr);
  } else {
    // TODO: base this on target.
    if (size == kWord) {
      size = k32;
    }

    null_ck_insn = StoreBaseDispBody(r_base, displacement, r_src, size);
  }

  if (UNLIKELY(is_volatile == kVolatile)) {
    // Preserve order with respect to any subsequent volatile loads.
    // We need StoreLoad, but that generally requires the most expensive barrier.
    GenMemBarrier(kAnyAny);
  }

  return null_ck_insn;
}

LIR* ArmMir2Lir::OpFpRegCopy(RegStorage r_dest, RegStorage r_src) {
  int opcode;
  DCHECK_EQ(r_dest.IsDouble(), r_src.IsDouble());
  if (r_dest.IsDouble()) {
    opcode = kThumb2Vmovd;
  } else {
    if (r_dest.IsSingle()) {
      opcode = r_src.IsSingle() ? kThumb2Vmovs : kThumb2Fmsr;
    } else {
      DCHECK(r_src.IsSingle());
      opcode = kThumb2Fmrs;
    }
  }
  LIR* res = RawLIR(current_dalvik_offset_, opcode, r_dest.GetReg(), r_src.GetReg());
  if (!(cu_->disable_opt & (1 << kSafeOptimizations)) && r_dest == r_src) {
    res->flags.is_nop = true;
  }
  return res;
}

LIR* ArmMir2Lir::OpMem(OpKind op, RegStorage r_base, int disp) {
  UNUSED(op, r_base, disp);
  LOG(FATAL) << "Unexpected use of OpMem for Arm";
  UNREACHABLE();
}

LIR* ArmMir2Lir::InvokeTrampoline(OpKind op, RegStorage r_tgt, QuickEntrypointEnum trampoline) {
  UNUSED(trampoline);  // The address of the trampoline is already loaded into r_tgt.
  return OpReg(op, r_tgt);
}

size_t ArmMir2Lir::GetInstructionOffset(LIR* lir) {
  uint64_t check_flags = GetTargetInstFlags(lir->opcode);
  DCHECK((check_flags & IS_LOAD) || (check_flags & IS_STORE));
  size_t offset = (check_flags & IS_TERTIARY_OP) ? lir->operands[2] : 0;

  if (check_flags & SCALED_OFFSET_X2) {
    offset = offset * 2;
  } else if (check_flags & SCALED_OFFSET_X4) {
    offset = offset * 4;
  }
  return offset;
}

void ArmMir2Lir::CountRefs(RefCounts* core_counts, RefCounts* fp_counts, size_t num_regs) {
  // Start with the default counts.
  Mir2Lir::CountRefs(core_counts, fp_counts, num_regs);

  if (pc_rel_temp_ != nullptr) {
    // Now, if the dex cache array base temp is used only once outside any loops (weight = 1),
    // avoid the promotion, otherwise boost the weight by factor 3 because the full PC-relative
    // load sequence is 4 instructions long and by promoting the PC base we save up to 3
    // instructions per use.
    int p_map_idx = SRegToPMap(pc_rel_temp_->s_reg_low);
    if (core_counts[p_map_idx].count == 1) {
      core_counts[p_map_idx].count = 0;
    } else {
      core_counts[p_map_idx].count *= 3;
    }
  }
}

void ArmMir2Lir::DoPromotion() {
  if (CanUseOpPcRelDexCacheArrayLoad()) {
    pc_rel_temp_ = mir_graph_->GetNewCompilerTemp(kCompilerTempBackend, false);
  }

  Mir2Lir::DoPromotion();

  if (pc_rel_temp_ != nullptr) {
    // Now, if the dex cache array base temp is promoted, remember the register but
    // always remove the temp's stack location to avoid unnecessarily bloating the stack.
    dex_cache_arrays_base_reg_ = mir_graph_->reg_location_[pc_rel_temp_->s_reg_low].reg;
    DCHECK(!dex_cache_arrays_base_reg_.Valid() || !dex_cache_arrays_base_reg_.IsFloat());
    mir_graph_->RemoveLastCompilerTemp(kCompilerTempBackend, false, pc_rel_temp_);
    pc_rel_temp_ = nullptr;
  }
}

}  // namespace art
