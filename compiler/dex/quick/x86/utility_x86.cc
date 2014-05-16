/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include "codegen_x86.h"
#include "dex/quick/mir_to_lir-inl.h"
#include "dex/dataflow_iterator-inl.h"
#include "x86_lir.h"

namespace art {

/* This file contains codegen for the X86 ISA */

LIR* X86Mir2Lir::OpFpRegCopy(RegStorage r_dest, RegStorage r_src) {
  int opcode;
  /* must be both DOUBLE or both not DOUBLE */
  DCHECK(r_dest.IsFloat() || r_src.IsFloat());
  DCHECK_EQ(r_dest.IsDouble(), r_src.IsDouble());
  if (r_dest.IsDouble()) {
    opcode = kX86MovsdRR;
  } else {
    if (r_dest.IsSingle()) {
      if (r_src.IsSingle()) {
        opcode = kX86MovssRR;
      } else {  // Fpr <- Gpr
        opcode = kX86MovdxrRR;
      }
    } else {  // Gpr <- Fpr
      DCHECK(r_src.IsSingle()) << "Raw: 0x" << std::hex << r_src.GetRawBits();
      opcode = kX86MovdrxRR;
    }
  }
  DCHECK_NE((EncodingMap[opcode].flags & IS_BINARY_OP), 0ULL);
  LIR* res = RawLIR(current_dalvik_offset_, opcode, r_dest.GetReg(), r_src.GetReg());
  if (r_dest == r_src) {
    res->flags.is_nop = true;
  }
  return res;
}

bool X86Mir2Lir::InexpensiveConstantInt(int32_t value) {
  return true;
}

bool X86Mir2Lir::InexpensiveConstantFloat(int32_t value) {
  return false;
}

bool X86Mir2Lir::InexpensiveConstantLong(int64_t value) {
  return true;
}

bool X86Mir2Lir::InexpensiveConstantDouble(int64_t value) {
  return value == 0;
}

/*
 * Load a immediate using a shortcut if possible; otherwise
 * grab from the per-translation literal pool.  If target is
 * a high register, build constant into a low register and copy.
 *
 * No additional register clobbering operation performed. Use this version when
 * 1) r_dest is freshly returned from AllocTemp or
 * 2) The codegen is under fixed register usage
 */
LIR* X86Mir2Lir::LoadConstantNoClobber(RegStorage r_dest, int value) {
  RegStorage r_dest_save = r_dest;
  if (r_dest.IsFloat()) {
    if (value == 0) {
      return NewLIR2(kX86XorpsRR, r_dest.GetReg(), r_dest.GetReg());
    }
    r_dest = AllocTemp();
  }

  LIR *res;
  if (value == 0) {
    res = NewLIR2(kX86Xor32RR, r_dest.GetReg(), r_dest.GetReg());
  } else {
    // Note, there is no byte immediate form of a 32 bit immediate move.
    res = NewLIR2(kX86Mov32RI, r_dest.GetReg(), value);
  }

  if (r_dest_save.IsFloat()) {
    NewLIR2(kX86MovdxrRR, r_dest_save.GetReg(), r_dest.GetReg());
    FreeTemp(r_dest);
  }

  return res;
}

LIR* X86Mir2Lir::OpUnconditionalBranch(LIR* target) {
  LIR* res = NewLIR1(kX86Jmp8, 0 /* offset to be patched during assembly*/);
  res->target = target;
  return res;
}

LIR* X86Mir2Lir::OpCondBranch(ConditionCode cc, LIR* target) {
  LIR* branch = NewLIR2(kX86Jcc8, 0 /* offset to be patched */,
                        X86ConditionEncoding(cc));
  branch->target = target;
  return branch;
}

LIR* X86Mir2Lir::OpReg(OpKind op, RegStorage r_dest_src) {
  X86OpCode opcode = kX86Bkpt;
  switch (op) {
    case kOpNeg: opcode = kX86Neg32R; break;
    case kOpNot: opcode = kX86Not32R; break;
    case kOpRev: opcode = kX86Bswap32R; break;
    case kOpBlx: opcode = kX86CallR; break;
    default:
      LOG(FATAL) << "Bad case in OpReg " << op;
  }
  return NewLIR1(opcode, r_dest_src.GetReg());
}

LIR* X86Mir2Lir::OpRegImm(OpKind op, RegStorage r_dest_src1, int value) {
  X86OpCode opcode = kX86Bkpt;
  bool byte_imm = IS_SIMM8(value);
  DCHECK(!r_dest_src1.IsFloat());
  if (r_dest_src1.Is64Bit()) {
    switch (op) {
      case kOpAdd: opcode = byte_imm ? kX86Add64RI8 : kX86Add64RI; break;
      case kOpSub: opcode = byte_imm ? kX86Sub64RI8 : kX86Sub64RI; break;
      default:
        LOG(FATAL) << "Bad case in OpRegImm (64-bit) " << op;
    }
  } else {
    switch (op) {
      case kOpLsl: opcode = kX86Sal32RI; break;
      case kOpLsr: opcode = kX86Shr32RI; break;
      case kOpAsr: opcode = kX86Sar32RI; break;
      case kOpAdd: opcode = byte_imm ? kX86Add32RI8 : kX86Add32RI; break;
      case kOpOr:  opcode = byte_imm ? kX86Or32RI8  : kX86Or32RI;  break;
      case kOpAdc: opcode = byte_imm ? kX86Adc32RI8 : kX86Adc32RI; break;
      // case kOpSbb: opcode = kX86Sbb32RI; break;
      case kOpAnd: opcode = byte_imm ? kX86And32RI8 : kX86And32RI; break;
      case kOpSub: opcode = byte_imm ? kX86Sub32RI8 : kX86Sub32RI; break;
      case kOpXor: opcode = byte_imm ? kX86Xor32RI8 : kX86Xor32RI; break;
      case kOpCmp: opcode = byte_imm ? kX86Cmp32RI8 : kX86Cmp32RI; break;
      case kOpMov:
        /*
         * Moving the constant zero into register can be specialized as an xor of the register.
         * However, that sets eflags while the move does not. For that reason here, always do
         * the move and if caller is flexible, they should be calling LoadConstantNoClobber instead.
         */
        opcode = kX86Mov32RI;
        break;
      case kOpMul:
        opcode = byte_imm ? kX86Imul32RRI8 : kX86Imul32RRI;
        return NewLIR3(opcode, r_dest_src1.GetReg(), r_dest_src1.GetReg(), value);
      default:
        LOG(FATAL) << "Bad case in OpRegImm " << op;
    }
  }
  CHECK(!r_dest_src1.Is64Bit() || X86Mir2Lir::EncodingMap[opcode].kind == kReg64Imm) << "OpRegImm(" << op << ")";
  return NewLIR2(opcode, r_dest_src1.GetReg(), value);
}

LIR* X86Mir2Lir::OpRegReg(OpKind op, RegStorage r_dest_src1, RegStorage r_src2) {
    X86OpCode opcode = kX86Nop;
    bool src2_must_be_cx = false;
    switch (op) {
        // X86 unary opcodes
      case kOpMvn:
        OpRegCopy(r_dest_src1, r_src2);
        return OpReg(kOpNot, r_dest_src1);
      case kOpNeg:
        OpRegCopy(r_dest_src1, r_src2);
        return OpReg(kOpNeg, r_dest_src1);
      case kOpRev:
        OpRegCopy(r_dest_src1, r_src2);
        return OpReg(kOpRev, r_dest_src1);
      case kOpRevsh:
        OpRegCopy(r_dest_src1, r_src2);
        OpReg(kOpRev, r_dest_src1);
        return OpRegImm(kOpAsr, r_dest_src1, 16);
        // X86 binary opcodes
      case kOpSub: opcode = kX86Sub32RR; break;
      case kOpSbc: opcode = kX86Sbb32RR; break;
      case kOpLsl: opcode = kX86Sal32RC; src2_must_be_cx = true; break;
      case kOpLsr: opcode = kX86Shr32RC; src2_must_be_cx = true; break;
      case kOpAsr: opcode = kX86Sar32RC; src2_must_be_cx = true; break;
      case kOpMov: opcode = kX86Mov32RR; break;
      case kOpCmp: opcode = kX86Cmp32RR; break;
      case kOpAdd: opcode = kX86Add32RR; break;
      case kOpAdc: opcode = kX86Adc32RR; break;
      case kOpAnd: opcode = kX86And32RR; break;
      case kOpOr:  opcode = kX86Or32RR; break;
      case kOpXor: opcode = kX86Xor32RR; break;
      case kOp2Byte:
        // TODO: there are several instances of this check.  A utility function perhaps?
        // TODO: Similar to Arm's reg < 8 check.  Perhaps add attribute checks to RegStorage?
        // Use shifts instead of a byte operand if the source can't be byte accessed.
        if (r_src2.GetRegNum() >= rs_rX86_SP.GetRegNum()) {
          NewLIR2(kX86Mov32RR, r_dest_src1.GetReg(), r_src2.GetReg());
          NewLIR2(kX86Sal32RI, r_dest_src1.GetReg(), 24);
          return NewLIR2(kX86Sar32RI, r_dest_src1.GetReg(), 24);
        } else {
          opcode = kX86Movsx8RR;
        }
        break;
      case kOp2Short: opcode = kX86Movsx16RR; break;
      case kOp2Char: opcode = kX86Movzx16RR; break;
      case kOpMul: opcode = kX86Imul32RR; break;
      default:
        LOG(FATAL) << "Bad case in OpRegReg " << op;
        break;
    }
    CHECK(!src2_must_be_cx || r_src2.GetReg() == rs_rCX.GetReg());
    return NewLIR2(opcode, r_dest_src1.GetReg(), r_src2.GetReg());
}

LIR* X86Mir2Lir::OpMovRegMem(RegStorage r_dest, RegStorage r_base, int offset, MoveType move_type) {
  DCHECK(!r_base.IsFloat());
  X86OpCode opcode = kX86Nop;
  int dest = r_dest.IsPair() ? r_dest.GetLowReg() : r_dest.GetReg();
  switch (move_type) {
    case kMov8GP:
      CHECK(!r_dest.IsFloat());
      opcode = kX86Mov8RM;
      break;
    case kMov16GP:
      CHECK(!r_dest.IsFloat());
      opcode = kX86Mov16RM;
      break;
    case kMov32GP:
      CHECK(!r_dest.IsFloat());
      opcode = kX86Mov32RM;
      break;
    case kMov32FP:
      CHECK(r_dest.IsFloat());
      opcode = kX86MovssRM;
      break;
    case kMov64FP:
      CHECK(r_dest.IsFloat());
      opcode = kX86MovsdRM;
      break;
    case kMovU128FP:
      CHECK(r_dest.IsFloat());
      opcode = kX86MovupsRM;
      break;
    case kMovA128FP:
      CHECK(r_dest.IsFloat());
      opcode = kX86MovapsRM;
      break;
    case kMovLo128FP:
      CHECK(r_dest.IsFloat());
      opcode = kX86MovlpsRM;
      break;
    case kMovHi128FP:
      CHECK(r_dest.IsFloat());
      opcode = kX86MovhpsRM;
      break;
    case kMov64GP:
    case kMovLo64FP:
    case kMovHi64FP:
    default:
      LOG(FATAL) << "Bad case in OpMovRegMem";
      break;
  }

  return NewLIR3(opcode, dest, r_base.GetReg(), offset);
}

LIR* X86Mir2Lir::OpMovMemReg(RegStorage r_base, int offset, RegStorage r_src, MoveType move_type) {
  DCHECK(!r_base.IsFloat());
  int src = r_src.IsPair() ? r_src.GetLowReg() : r_src.GetReg();

  X86OpCode opcode = kX86Nop;
  switch (move_type) {
    case kMov8GP:
      CHECK(!r_src.IsFloat());
      opcode = kX86Mov8MR;
      break;
    case kMov16GP:
      CHECK(!r_src.IsFloat());
      opcode = kX86Mov16MR;
      break;
    case kMov32GP:
      CHECK(!r_src.IsFloat());
      opcode = kX86Mov32MR;
      break;
    case kMov32FP:
      CHECK(r_src.IsFloat());
      opcode = kX86MovssMR;
      break;
    case kMov64FP:
      CHECK(r_src.IsFloat());
      opcode = kX86MovsdMR;
      break;
    case kMovU128FP:
      CHECK(r_src.IsFloat());
      opcode = kX86MovupsMR;
      break;
    case kMovA128FP:
      CHECK(r_src.IsFloat());
      opcode = kX86MovapsMR;
      break;
    case kMovLo128FP:
      CHECK(r_src.IsFloat());
      opcode = kX86MovlpsMR;
      break;
    case kMovHi128FP:
      CHECK(r_src.IsFloat());
      opcode = kX86MovhpsMR;
      break;
    case kMov64GP:
    case kMovLo64FP:
    case kMovHi64FP:
    default:
      LOG(FATAL) << "Bad case in OpMovMemReg";
      break;
  }

  return NewLIR3(opcode, r_base.GetReg(), offset, src);
}

LIR* X86Mir2Lir::OpCondRegReg(OpKind op, ConditionCode cc, RegStorage r_dest, RegStorage r_src) {
  // The only conditional reg to reg operation supported is Cmov
  DCHECK_EQ(op, kOpCmov);
  return NewLIR3(kX86Cmov32RRC, r_dest.GetReg(), r_src.GetReg(), X86ConditionEncoding(cc));
}

LIR* X86Mir2Lir::OpRegMem(OpKind op, RegStorage r_dest, RegStorage r_base, int offset) {
  X86OpCode opcode = kX86Nop;
  switch (op) {
      // X86 binary opcodes
    case kOpSub: opcode = kX86Sub32RM; break;
    case kOpMov: opcode = kX86Mov32RM; break;
    case kOpCmp: opcode = kX86Cmp32RM; break;
    case kOpAdd: opcode = kX86Add32RM; break;
    case kOpAnd: opcode = kX86And32RM; break;
    case kOpOr:  opcode = kX86Or32RM; break;
    case kOpXor: opcode = kX86Xor32RM; break;
    case kOp2Byte: opcode = kX86Movsx8RM; break;
    case kOp2Short: opcode = kX86Movsx16RM; break;
    case kOp2Char: opcode = kX86Movzx16RM; break;
    case kOpMul:
    default:
      LOG(FATAL) << "Bad case in OpRegMem " << op;
      break;
  }
  LIR *l = NewLIR3(opcode, r_dest.GetReg(), r_base.GetReg(), offset);
  if (r_base == rs_rX86_SP) {
    AnnotateDalvikRegAccess(l, offset >> 2, true /* is_load */, false /* is_64bit */);
  }
  return l;
}

LIR* X86Mir2Lir::OpMemReg(OpKind op, RegLocation rl_dest, int r_value) {
  DCHECK_NE(rl_dest.location, kLocPhysReg);
  int displacement = SRegOffset(rl_dest.s_reg_low);
  X86OpCode opcode = kX86Nop;
  switch (op) {
    case kOpSub: opcode = kX86Sub32MR; break;
    case kOpMov: opcode = kX86Mov32MR; break;
    case kOpCmp: opcode = kX86Cmp32MR; break;
    case kOpAdd: opcode = kX86Add32MR; break;
    case kOpAnd: opcode = kX86And32MR; break;
    case kOpOr:  opcode = kX86Or32MR; break;
    case kOpXor: opcode = kX86Xor32MR; break;
    case kOpLsl: opcode = kX86Sal32MC; break;
    case kOpLsr: opcode = kX86Shr32MC; break;
    case kOpAsr: opcode = kX86Sar32MC; break;
    default:
      LOG(FATAL) << "Bad case in OpMemReg " << op;
      break;
  }
  LIR *l = NewLIR3(opcode, rs_rX86_SP.GetReg(), displacement, r_value);
  AnnotateDalvikRegAccess(l, displacement >> 2, true /* is_load */, false /* is_64bit */);
  AnnotateDalvikRegAccess(l, displacement >> 2, false /* is_load */, false /* is_64bit */);
  return l;
}

LIR* X86Mir2Lir::OpRegMem(OpKind op, RegStorage r_dest, RegLocation rl_value) {
  DCHECK_NE(rl_value.location, kLocPhysReg);
  int displacement = SRegOffset(rl_value.s_reg_low);
  X86OpCode opcode = kX86Nop;
  switch (op) {
    case kOpSub: opcode = kX86Sub32RM; break;
    case kOpMov: opcode = kX86Mov32RM; break;
    case kOpCmp: opcode = kX86Cmp32RM; break;
    case kOpAdd: opcode = kX86Add32RM; break;
    case kOpAnd: opcode = kX86And32RM; break;
    case kOpOr:  opcode = kX86Or32RM; break;
    case kOpXor: opcode = kX86Xor32RM; break;
    case kOpMul: opcode = kX86Imul32RM; break;
    default:
      LOG(FATAL) << "Bad case in OpRegMem " << op;
      break;
  }
  LIR *l = NewLIR3(opcode, r_dest.GetReg(), rs_rX86_SP.GetReg(), displacement);
  AnnotateDalvikRegAccess(l, displacement >> 2, true /* is_load */, false /* is_64bit */);
  return l;
}

LIR* X86Mir2Lir::OpRegRegReg(OpKind op, RegStorage r_dest, RegStorage r_src1,
                             RegStorage r_src2) {
  if (r_dest != r_src1 && r_dest != r_src2) {
    if (op == kOpAdd) {  // lea special case, except can't encode rbp as base
      if (r_src1 == r_src2) {
        OpRegCopy(r_dest, r_src1);
        return OpRegImm(kOpLsl, r_dest, 1);
      } else if (r_src1 != rs_rBP) {
        return NewLIR5(kX86Lea32RA, r_dest.GetReg(), r_src1.GetReg() /* base */,
                       r_src2.GetReg() /* index */, 0 /* scale */, 0 /* disp */);
      } else {
        return NewLIR5(kX86Lea32RA, r_dest.GetReg(), r_src2.GetReg() /* base */,
                       r_src1.GetReg() /* index */, 0 /* scale */, 0 /* disp */);
      }
    } else {
      OpRegCopy(r_dest, r_src1);
      return OpRegReg(op, r_dest, r_src2);
    }
  } else if (r_dest == r_src1) {
    return OpRegReg(op, r_dest, r_src2);
  } else {  // r_dest == r_src2
    switch (op) {
      case kOpSub:  // non-commutative
        OpReg(kOpNeg, r_dest);
        op = kOpAdd;
        break;
      case kOpSbc:
      case kOpLsl: case kOpLsr: case kOpAsr: case kOpRor: {
        RegStorage t_reg = AllocTemp();
        OpRegCopy(t_reg, r_src1);
        OpRegReg(op, t_reg, r_src2);
        LIR* res = OpRegCopyNoInsert(r_dest, t_reg);
        AppendLIR(res);
        FreeTemp(t_reg);
        return res;
      }
      case kOpAdd:  // commutative
      case kOpOr:
      case kOpAdc:
      case kOpAnd:
      case kOpXor:
        break;
      default:
        LOG(FATAL) << "Bad case in OpRegRegReg " << op;
    }
    return OpRegReg(op, r_dest, r_src1);
  }
}

LIR* X86Mir2Lir::OpRegRegImm(OpKind op, RegStorage r_dest, RegStorage r_src, int value) {
  if (op == kOpMul) {
    X86OpCode opcode = IS_SIMM8(value) ? kX86Imul32RRI8 : kX86Imul32RRI;
    return NewLIR3(opcode, r_dest.GetReg(), r_src.GetReg(), value);
  } else if (op == kOpAnd) {
    if (value == 0xFF && r_src.Low4()) {
      return NewLIR2(kX86Movzx8RR, r_dest.GetReg(), r_src.GetReg());
    } else if (value == 0xFFFF) {
      return NewLIR2(kX86Movzx16RR, r_dest.GetReg(), r_src.GetReg());
    }
  }
  if (r_dest != r_src) {
    if (false && op == kOpLsl && value >= 0 && value <= 3) {  // lea shift special case
      // TODO: fix bug in LEA encoding when disp == 0
      return NewLIR5(kX86Lea32RA, r_dest.GetReg(),  r5sib_no_base /* base */,
                     r_src.GetReg() /* index */, value /* scale */, 0 /* disp */);
    } else if (op == kOpAdd) {  // lea add special case
      return NewLIR5(kX86Lea32RA, r_dest.GetReg(), r_src.GetReg() /* base */,
                     rs_rX86_SP.GetReg()/*r4sib_no_index*/ /* index */, 0 /* scale */, value /* disp */);
    }
    OpRegCopy(r_dest, r_src);
  }
  return OpRegImm(op, r_dest, value);
}

LIR* X86Mir2Lir::OpThreadMem(OpKind op, ThreadOffset<4> thread_offset) {
  DCHECK_EQ(kX86, cu_->instruction_set);
  X86OpCode opcode = kX86Bkpt;
  switch (op) {
    case kOpBlx: opcode = kX86CallT;  break;
    case kOpBx: opcode = kX86JmpT;  break;
    default:
      LOG(FATAL) << "Bad opcode: " << op;
      break;
  }
  return NewLIR1(opcode, thread_offset.Int32Value());
}

LIR* X86Mir2Lir::OpThreadMem(OpKind op, ThreadOffset<8> thread_offset) {
  DCHECK_EQ(kX86_64, cu_->instruction_set);
  X86OpCode opcode = kX86Bkpt;
  switch (op) {
    case kOpBlx: opcode = kX86CallT;  break;
    case kOpBx: opcode = kX86JmpT;  break;
    default:
      LOG(FATAL) << "Bad opcode: " << op;
      break;
  }
  return NewLIR1(opcode, thread_offset.Int32Value());
}

LIR* X86Mir2Lir::OpMem(OpKind op, RegStorage r_base, int disp) {
  X86OpCode opcode = kX86Bkpt;
  switch (op) {
    case kOpBlx: opcode = kX86CallM;  break;
    default:
      LOG(FATAL) << "Bad opcode: " << op;
      break;
  }
  return NewLIR2(opcode, r_base.GetReg(), disp);
}

LIR* X86Mir2Lir::LoadConstantWide(RegStorage r_dest, int64_t value) {
    int32_t val_lo = Low32Bits(value);
    int32_t val_hi = High32Bits(value);
    int32_t low_reg_val = r_dest.IsPair() ? r_dest.GetLowReg() : r_dest.GetReg();
    LIR *res;
    bool is_fp = RegStorage::IsFloat(low_reg_val);
    // TODO: clean this up once we fully recognize 64-bit storage containers.
    if (is_fp) {
      if (value == 0) {
        return NewLIR2(kX86XorpsRR, low_reg_val, low_reg_val);
      } else if (base_of_code_ != nullptr) {
        // We will load the value from the literal area.
        LIR* data_target = ScanLiteralPoolWide(literal_list_, val_lo, val_hi);
        if (data_target == NULL) {
          data_target = AddWideData(&literal_list_, val_lo, val_hi);
        }

        // Address the start of the method
        RegLocation rl_method = mir_graph_->GetRegLocation(base_of_code_->s_reg_low);
        rl_method = LoadValue(rl_method, kCoreReg);

        // Load the proper value from the literal area.
        // We don't know the proper offset for the value, so pick one that will force
        // 4 byte offset.  We will fix this up in the assembler later to have the right
        // value.
        res = LoadBaseDisp(rl_method.reg, 256 /* bogus */, RegStorage::Solo64(low_reg_val),
                           kDouble);
        res->target = data_target;
        res->flags.fixup = kFixupLoad;
        SetMemRefType(res, true, kLiteral);
        store_method_addr_used_ = true;
      } else {
        if (val_lo == 0) {
          res = NewLIR2(kX86XorpsRR, low_reg_val, low_reg_val);
        } else {
          res = LoadConstantNoClobber(RegStorage::Solo32(low_reg_val), val_lo);
        }
        if (val_hi != 0) {
          RegStorage r_dest_hi = AllocTempDouble();
          LoadConstantNoClobber(r_dest_hi, val_hi);
          NewLIR2(kX86PunpckldqRR, low_reg_val, r_dest_hi.GetReg());
          FreeTemp(r_dest_hi);
        }
      }
    } else {
      res = LoadConstantNoClobber(r_dest.GetLow(), val_lo);
      LoadConstantNoClobber(r_dest.GetHigh(), val_hi);
    }
    return res;
}

LIR* X86Mir2Lir::LoadBaseIndexedDisp(RegStorage r_base, RegStorage r_index, int scale,
                                     int displacement, RegStorage r_dest, OpSize size) {
  LIR *load = NULL;
  LIR *load2 = NULL;
  bool is_array = r_index.Valid();
  bool pair = r_dest.IsPair();
  bool is64bit = ((size == k64) || (size == kDouble));
  X86OpCode opcode = kX86Nop;
  switch (size) {
    case k64:
    case kDouble:
      if (r_dest.IsFloat()) {
        opcode = is_array ? kX86MovsdRA : kX86MovsdRM;
      } else {
        opcode = is_array ? kX86Mov32RA  : kX86Mov32RM;
      }
      // TODO: double store is to unaligned address
      DCHECK_EQ((displacement & 0x3), 0);
      break;
    case kWord:
      if (Gen64Bit()) {
        opcode = is_array ? kX86Mov64RA  : kX86Mov64RM;
        CHECK_EQ(is_array, false);
        CHECK_EQ(r_dest.IsFloat(), false);
        break;
      }  // else fall-through to k32 case
    case k32:
    case kSingle:
    case kReference:  // TODO: update for reference decompression on 64-bit targets.
      opcode = is_array ? kX86Mov32RA : kX86Mov32RM;
      if (r_dest.IsFloat()) {
        opcode = is_array ? kX86MovssRA : kX86MovssRM;
        DCHECK(r_dest.IsFloat());
      }
      DCHECK_EQ((displacement & 0x3), 0);
      break;
    case kUnsignedHalf:
      opcode = is_array ? kX86Movzx16RA : kX86Movzx16RM;
      DCHECK_EQ((displacement & 0x1), 0);
      break;
    case kSignedHalf:
      opcode = is_array ? kX86Movsx16RA : kX86Movsx16RM;
      DCHECK_EQ((displacement & 0x1), 0);
      break;
    case kUnsignedByte:
      opcode = is_array ? kX86Movzx8RA : kX86Movzx8RM;
      break;
    case kSignedByte:
      opcode = is_array ? kX86Movsx8RA : kX86Movsx8RM;
      break;
    default:
      LOG(FATAL) << "Bad case in LoadBaseIndexedDispBody";
  }

  if (!is_array) {
    if (!pair) {
      load = NewLIR3(opcode, r_dest.GetReg(), r_base.GetReg(), displacement + LOWORD_OFFSET);
    } else {
      DCHECK(!r_dest.IsFloat());  // Make sure we're not still using a pair here.
      if (r_base == r_dest.GetLow()) {
        load2 = NewLIR3(opcode, r_dest.GetHighReg(), r_base.GetReg(),
                        displacement + HIWORD_OFFSET);
        load = NewLIR3(opcode, r_dest.GetLowReg(), r_base.GetReg(), displacement + LOWORD_OFFSET);
      } else {
        load = NewLIR3(opcode, r_dest.GetLowReg(), r_base.GetReg(), displacement + LOWORD_OFFSET);
        load2 = NewLIR3(opcode, r_dest.GetHighReg(), r_base.GetReg(),
                        displacement + HIWORD_OFFSET);
      }
    }
    if (r_base == rs_rX86_SP) {
      AnnotateDalvikRegAccess(load, (displacement + (pair ? LOWORD_OFFSET : 0)) >> 2,
                              true /* is_load */, is64bit);
      if (pair) {
        AnnotateDalvikRegAccess(load2, (displacement + HIWORD_OFFSET) >> 2,
                                true /* is_load */, is64bit);
      }
    }
  } else {
    if (!pair) {
      load = NewLIR5(opcode, r_dest.GetReg(), r_base.GetReg(), r_index.GetReg(), scale,
                     displacement + LOWORD_OFFSET);
    } else {
      DCHECK(!r_dest.IsFloat());  // Make sure we're not still using a pair here.
      if (r_base == r_dest.GetLow()) {
        if (r_dest.GetHigh() == r_index) {
          // We can't use either register for the first load.
          RegStorage temp = AllocTemp();
          load2 = NewLIR5(opcode, temp.GetReg(), r_base.GetReg(), r_index.GetReg(), scale,
                          displacement + HIWORD_OFFSET);
          load = NewLIR5(opcode, r_dest.GetLowReg(), r_base.GetReg(), r_index.GetReg(), scale,
                         displacement + LOWORD_OFFSET);
          OpRegCopy(r_dest.GetHigh(), temp);
          FreeTemp(temp);
        } else {
          load2 = NewLIR5(opcode, r_dest.GetHighReg(), r_base.GetReg(), r_index.GetReg(), scale,
                          displacement + HIWORD_OFFSET);
          load = NewLIR5(opcode, r_dest.GetLowReg(), r_base.GetReg(), r_index.GetReg(), scale,
                         displacement + LOWORD_OFFSET);
        }
      } else {
        if (r_dest.GetLow() == r_index) {
          // We can't use either register for the first load.
          RegStorage temp = AllocTemp();
          load = NewLIR5(opcode, temp.GetReg(), r_base.GetReg(), r_index.GetReg(), scale,
                         displacement + LOWORD_OFFSET);
          load2 = NewLIR5(opcode, r_dest.GetHighReg(), r_base.GetReg(), r_index.GetReg(), scale,
                          displacement + HIWORD_OFFSET);
          OpRegCopy(r_dest.GetLow(), temp);
          FreeTemp(temp);
        } else {
          load = NewLIR5(opcode, r_dest.GetLowReg(), r_base.GetReg(), r_index.GetReg(), scale,
                         displacement + LOWORD_OFFSET);
          load2 = NewLIR5(opcode, r_dest.GetHighReg(), r_base.GetReg(), r_index.GetReg(), scale,
                          displacement + HIWORD_OFFSET);
        }
      }
    }
  }

  return load;
}

/* Load value from base + scaled index. */
LIR* X86Mir2Lir::LoadBaseIndexed(RegStorage r_base, RegStorage r_index, RegStorage r_dest,
                                 int scale, OpSize size) {
  return LoadBaseIndexedDisp(r_base, r_index, scale, 0, r_dest, size);
}

LIR* X86Mir2Lir::LoadBaseDispVolatile(RegStorage r_base, int displacement, RegStorage r_dest,
                                      OpSize size) {
  // LoadBaseDisp() will emit correct insn for atomic load on x86
  // assuming r_dest is correctly prepared using RegClassForFieldLoadStore().
  return LoadBaseDisp(r_base, displacement, r_dest, size);
}

LIR* X86Mir2Lir::LoadBaseDisp(RegStorage r_base, int displacement, RegStorage r_dest,
                              OpSize size) {
  return LoadBaseIndexedDisp(r_base, RegStorage::InvalidReg(), 0, displacement, r_dest,
                             size);
}

LIR* X86Mir2Lir::StoreBaseIndexedDisp(RegStorage r_base, RegStorage r_index, int scale,
                                      int displacement, RegStorage r_src, OpSize size) {
  LIR *store = NULL;
  LIR *store2 = NULL;
  bool is_array = r_index.Valid();
  bool pair = r_src.IsPair();
  bool is64bit = (size == k64) || (size == kDouble);
  X86OpCode opcode = kX86Nop;
  switch (size) {
    case k64:
    case kDouble:
      if (r_src.IsFloat()) {
        opcode = is_array ? kX86MovsdAR : kX86MovsdMR;
      } else {
        if (Gen64Bit()) {
          opcode = is_array ? kX86Mov64AR  : kX86Mov64MR;
        } else {
          // TODO(64): pair = true;
          opcode = is_array ? kX86Mov32AR  : kX86Mov32MR;
        }
      }
      // TODO: double store is to unaligned address
      DCHECK_EQ((displacement & 0x3), 0);
      break;
    case kWord:
      if (Gen64Bit()) {
        opcode = is_array ? kX86Mov64AR  : kX86Mov64MR;
        CHECK_EQ(is_array, false);
        CHECK_EQ(r_src.IsFloat(), false);
        break;
      }  // else fall-through to k32 case
    case k32:
    case kSingle:
    case kReference:
      opcode = is_array ? kX86Mov32AR : kX86Mov32MR;
      if (r_src.IsFloat()) {
        opcode = is_array ? kX86MovssAR : kX86MovssMR;
        DCHECK(r_src.IsSingle());
      }
      DCHECK_EQ((displacement & 0x3), 0);
      break;
    case kUnsignedHalf:
    case kSignedHalf:
      opcode = is_array ? kX86Mov16AR : kX86Mov16MR;
      DCHECK_EQ((displacement & 0x1), 0);
      break;
    case kUnsignedByte:
    case kSignedByte:
      opcode = is_array ? kX86Mov8AR : kX86Mov8MR;
      break;
    default:
      LOG(FATAL) << "Bad case in StoreBaseIndexedDispBody";
  }

  if (!is_array) {
    if (!pair) {
      store = NewLIR3(opcode, r_base.GetReg(), displacement + LOWORD_OFFSET, r_src.GetReg());
    } else {
      DCHECK(!r_src.IsFloat());  // Make sure we're not still using a pair here.
      store = NewLIR3(opcode, r_base.GetReg(), displacement + LOWORD_OFFSET, r_src.GetLowReg());
      store2 = NewLIR3(opcode, r_base.GetReg(), displacement + HIWORD_OFFSET, r_src.GetHighReg());
    }
    if (r_base == rs_rX86_SP) {
      AnnotateDalvikRegAccess(store, (displacement + (pair ? LOWORD_OFFSET : 0)) >> 2,
                              false /* is_load */, is64bit);
      if (pair) {
        AnnotateDalvikRegAccess(store2, (displacement + HIWORD_OFFSET) >> 2,
                                false /* is_load */, is64bit);
      }
    }
  } else {
    if (!pair) {
      store = NewLIR5(opcode, r_base.GetReg(), r_index.GetReg(), scale,
                      displacement + LOWORD_OFFSET, r_src.GetReg());
    } else {
      DCHECK(!r_src.IsFloat());  // Make sure we're not still using a pair here.
      store = NewLIR5(opcode, r_base.GetReg(), r_index.GetReg(), scale,
                      displacement + LOWORD_OFFSET, r_src.GetLowReg());
      store2 = NewLIR5(opcode, r_base.GetReg(), r_index.GetReg(), scale,
                       displacement + HIWORD_OFFSET, r_src.GetHighReg());
    }
  }
  return store;
}

/* store value base base + scaled index. */
LIR* X86Mir2Lir::StoreBaseIndexed(RegStorage r_base, RegStorage r_index, RegStorage r_src,
                      int scale, OpSize size) {
  return StoreBaseIndexedDisp(r_base, r_index, scale, 0, r_src, size);
}

LIR* X86Mir2Lir::StoreBaseDispVolatile(RegStorage r_base, int displacement,
                                       RegStorage r_src, OpSize size) {
  // StoreBaseDisp() will emit correct insn for atomic store on x86
  // assuming r_dest is correctly prepared using RegClassForFieldLoadStore().
  return StoreBaseDisp(r_base, displacement, r_src, size);
}

LIR* X86Mir2Lir::StoreBaseDisp(RegStorage r_base, int displacement,
                               RegStorage r_src, OpSize size) {
  return StoreBaseIndexedDisp(r_base, RegStorage::InvalidReg(), 0, displacement, r_src, size);
}

LIR* X86Mir2Lir::OpCmpMemImmBranch(ConditionCode cond, RegStorage temp_reg, RegStorage base_reg,
                                   int offset, int check_value, LIR* target) {
    NewLIR3(IS_SIMM8(check_value) ? kX86Cmp32MI8 : kX86Cmp32MI, base_reg.GetReg(), offset,
            check_value);
    LIR* branch = OpCondBranch(cond, target);
    return branch;
}

void X86Mir2Lir::AnalyzeMIR() {
  // Assume we don't need a pointer to the base of the code.
  cu_->NewTimingSplit("X86 MIR Analysis");
  store_method_addr_ = false;

  // Walk the MIR looking for interesting items.
  PreOrderDfsIterator iter(mir_graph_);
  BasicBlock* curr_bb = iter.Next();
  while (curr_bb != NULL) {
    AnalyzeBB(curr_bb);
    curr_bb = iter.Next();
  }

  // Did we need a pointer to the method code?
  if (store_method_addr_) {
    base_of_code_ = mir_graph_->GetNewCompilerTemp(kCompilerTempVR, false);
  } else {
    base_of_code_ = nullptr;
  }
}

void X86Mir2Lir::AnalyzeBB(BasicBlock * bb) {
  if (bb->block_type == kDead) {
    // Ignore dead blocks
    return;
  }

  for (MIR *mir = bb->first_mir_insn; mir != NULL; mir = mir->next) {
    int opcode = mir->dalvikInsn.opcode;
    if (opcode >= kMirOpFirst) {
      AnalyzeExtendedMIR(opcode, bb, mir);
    } else {
      AnalyzeMIR(opcode, bb, mir);
    }
  }
}


void X86Mir2Lir::AnalyzeExtendedMIR(int opcode, BasicBlock * bb, MIR *mir) {
  switch (opcode) {
    // Instructions referencing doubles.
    case kMirOpFusedCmplDouble:
    case kMirOpFusedCmpgDouble:
      AnalyzeFPInstruction(opcode, bb, mir);
      break;
    case kMirOpConstVector:
      store_method_addr_ = true;
      break;
    default:
      // Ignore the rest.
      break;
  }
}

void X86Mir2Lir::AnalyzeMIR(int opcode, BasicBlock * bb, MIR *mir) {
  // Looking for
  // - Do we need a pointer to the code (used for packed switches and double lits)?

  switch (opcode) {
    // Instructions referencing doubles.
    case Instruction::CMPL_DOUBLE:
    case Instruction::CMPG_DOUBLE:
    case Instruction::NEG_DOUBLE:
    case Instruction::ADD_DOUBLE:
    case Instruction::SUB_DOUBLE:
    case Instruction::MUL_DOUBLE:
    case Instruction::DIV_DOUBLE:
    case Instruction::REM_DOUBLE:
    case Instruction::ADD_DOUBLE_2ADDR:
    case Instruction::SUB_DOUBLE_2ADDR:
    case Instruction::MUL_DOUBLE_2ADDR:
    case Instruction::DIV_DOUBLE_2ADDR:
    case Instruction::REM_DOUBLE_2ADDR:
      AnalyzeFPInstruction(opcode, bb, mir);
      break;

    // Packed switches and array fills need a pointer to the base of the method.
    case Instruction::FILL_ARRAY_DATA:
    case Instruction::PACKED_SWITCH:
      store_method_addr_ = true;
      break;
    default:
      // Other instructions are not interesting yet.
      break;
  }
}

void X86Mir2Lir::AnalyzeFPInstruction(int opcode, BasicBlock * bb, MIR *mir) {
  // Look at all the uses, and see if they are double constants.
  uint64_t attrs = MIRGraph::GetDataFlowAttributes(static_cast<Instruction::Code>(opcode));
  int next_sreg = 0;
  if (attrs & DF_UA) {
    if (attrs & DF_A_WIDE) {
      AnalyzeDoubleUse(mir_graph_->GetSrcWide(mir, next_sreg));
      next_sreg += 2;
    } else {
      next_sreg++;
    }
  }
  if (attrs & DF_UB) {
    if (attrs & DF_B_WIDE) {
      AnalyzeDoubleUse(mir_graph_->GetSrcWide(mir, next_sreg));
      next_sreg += 2;
    } else {
      next_sreg++;
    }
  }
  if (attrs & DF_UC) {
    if (attrs & DF_C_WIDE) {
      AnalyzeDoubleUse(mir_graph_->GetSrcWide(mir, next_sreg));
    }
  }
}

void X86Mir2Lir::AnalyzeDoubleUse(RegLocation use) {
  // If this is a double literal, we will want it in the literal pool.
  if (use.is_const) {
    store_method_addr_ = true;
  }
}

RegLocation X86Mir2Lir::UpdateLocTyped(RegLocation loc, int reg_class) {
  loc = UpdateLoc(loc);
  if ((loc.location == kLocPhysReg) && (loc.fp != loc.reg.IsFloat())) {
    if (GetRegInfo(loc.reg)->IsTemp()) {
      Clobber(loc.reg);
      FreeTemp(loc.reg);
      loc.reg = RegStorage::InvalidReg();
      loc.location = kLocDalvikFrame;
    }
  }
  return loc;
}

RegLocation X86Mir2Lir::UpdateLocWideTyped(RegLocation loc, int reg_class) {
  loc = UpdateLocWide(loc);
  if ((loc.location == kLocPhysReg) && (loc.fp != loc.reg.IsFloat())) {
    if (GetRegInfo(loc.reg)->IsTemp()) {
      Clobber(loc.reg);
      FreeTemp(loc.reg);
      loc.reg = RegStorage::InvalidReg();
      loc.location = kLocDalvikFrame;
    }
  }
  return loc;
}

}  // namespace art
