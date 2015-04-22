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

#include "base/logging.h"
#include "dex/mir_graph.h"
#include "dex/quick/mir_to_lir-inl.h"
#include "dex/dataflow_iterator-inl.h"
#include "dex/quick/dex_file_method_inliner.h"
#include "dex/quick/dex_file_to_method_inliner_map.h"
#include "dex/reg_storage_eq.h"
#include "driver/compiler_driver.h"
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
  UNUSED(value);
  return true;
}

bool X86Mir2Lir::InexpensiveConstantFloat(int32_t value) {
  return value == 0;
}

bool X86Mir2Lir::InexpensiveConstantLong(int64_t value) {
  UNUSED(value);
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
    // 64-bit immediate is not supported by LIR structure
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
    case kOpNeg: opcode = r_dest_src.Is64Bit() ? kX86Neg64R : kX86Neg32R; break;
    case kOpNot: opcode = r_dest_src.Is64Bit() ? kX86Not64R : kX86Not32R; break;
    case kOpRev: opcode = r_dest_src.Is64Bit() ? kX86Bswap64R : kX86Bswap32R; break;
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
      case kOpLsl: opcode = kX86Sal64RI; break;
      case kOpLsr: opcode = kX86Shr64RI; break;
      case kOpAsr: opcode = kX86Sar64RI; break;
      case kOpCmp: opcode = byte_imm ? kX86Cmp64RI8 : kX86Cmp64RI; break;
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
      case kOp2Byte:
        opcode = kX86Mov32RI;
        value = static_cast<int8_t>(value);
        break;
      case kOp2Short:
        opcode = kX86Mov32RI;
        value = static_cast<int16_t>(value);
        break;
      case kOp2Char:
        opcode = kX86Mov32RI;
        value = static_cast<uint16_t>(value);
        break;
      case kOpNeg:
        opcode = kX86Mov32RI;
        value = -value;
        break;
      default:
        LOG(FATAL) << "Bad case in OpRegImm " << op;
    }
  }
  return NewLIR2(opcode, r_dest_src1.GetReg(), value);
}

LIR* X86Mir2Lir::OpRegReg(OpKind op, RegStorage r_dest_src1, RegStorage r_src2) {
    bool is64Bit = r_dest_src1.Is64Bit();
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
      case kOpSub: opcode = is64Bit ? kX86Sub64RR : kX86Sub32RR; break;
      case kOpSbc: opcode = is64Bit ? kX86Sbb64RR : kX86Sbb32RR; break;
      case kOpLsl: opcode = is64Bit ? kX86Sal64RC : kX86Sal32RC; src2_must_be_cx = true; break;
      case kOpLsr: opcode = is64Bit ? kX86Shr64RC : kX86Shr32RC; src2_must_be_cx = true; break;
      case kOpAsr: opcode = is64Bit ? kX86Sar64RC : kX86Sar32RC; src2_must_be_cx = true; break;
      case kOpMov: opcode = is64Bit ? kX86Mov64RR : kX86Mov32RR; break;
      case kOpCmp: opcode = is64Bit ? kX86Cmp64RR : kX86Cmp32RR; break;
      case kOpAdd: opcode = is64Bit ? kX86Add64RR : kX86Add32RR; break;
      case kOpAdc: opcode = is64Bit ? kX86Adc64RR : kX86Adc32RR; break;
      case kOpAnd: opcode = is64Bit ? kX86And64RR : kX86And32RR; break;
      case kOpOr:  opcode = is64Bit ? kX86Or64RR : kX86Or32RR; break;
      case kOpXor: opcode = is64Bit ? kX86Xor64RR : kX86Xor32RR; break;
      case kOp2Byte:
        // TODO: there are several instances of this check.  A utility function perhaps?
        // TODO: Similar to Arm's reg < 8 check.  Perhaps add attribute checks to RegStorage?
        // Use shifts instead of a byte operand if the source can't be byte accessed.
        if (r_src2.GetRegNum() >= rs_rX86_SP_32.GetRegNum()) {
          NewLIR2(is64Bit ? kX86Mov64RR : kX86Mov32RR, r_dest_src1.GetReg(), r_src2.GetReg());
          NewLIR2(is64Bit ? kX86Sal64RI : kX86Sal32RI, r_dest_src1.GetReg(), is64Bit ? 56 : 24);
          return NewLIR2(is64Bit ? kX86Sar64RI : kX86Sar32RI, r_dest_src1.GetReg(),
                         is64Bit ? 56 : 24);
        } else {
          opcode = is64Bit ? kX86Bkpt : kX86Movsx8RR;
        }
        break;
      case kOp2Short: opcode = is64Bit ? kX86Bkpt : kX86Movsx16RR; break;
      case kOp2Char: opcode = is64Bit ? kX86Bkpt : kX86Movzx16RR; break;
      case kOpMul: opcode = is64Bit ? kX86Bkpt : kX86Imul32RR; break;
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
  DCHECK_EQ(r_dest.Is64Bit(), r_src.Is64Bit());
  return NewLIR3(r_dest.Is64Bit() ? kX86Cmov64RRC : kX86Cmov32RRC, r_dest.GetReg(),
                 r_src.GetReg(), X86ConditionEncoding(cc));
}

LIR* X86Mir2Lir::OpRegMem(OpKind op, RegStorage r_dest, RegStorage r_base, int offset) {
  bool is64Bit = r_dest.Is64Bit();
  X86OpCode opcode = kX86Nop;
  switch (op) {
      // X86 binary opcodes
    case kOpSub: opcode = is64Bit ? kX86Sub64RM : kX86Sub32RM; break;
    case kOpMov: opcode = is64Bit ? kX86Mov64RM : kX86Mov32RM; break;
    case kOpCmp: opcode = is64Bit ? kX86Cmp64RM : kX86Cmp32RM; break;
    case kOpAdd: opcode = is64Bit ? kX86Add64RM : kX86Add32RM; break;
    case kOpAnd: opcode = is64Bit ? kX86And64RM : kX86And32RM; break;
    case kOpOr:  opcode = is64Bit ? kX86Or64RM : kX86Or32RM; break;
    case kOpXor: opcode = is64Bit ? kX86Xor64RM : kX86Xor32RM; break;
    case kOp2Byte: opcode = kX86Movsx8RM; break;
    case kOp2Short: opcode = kX86Movsx16RM; break;
    case kOp2Char: opcode = kX86Movzx16RM; break;
    case kOpMul:
    default:
      LOG(FATAL) << "Bad case in OpRegMem " << op;
      break;
  }
  LIR *l = NewLIR3(opcode, r_dest.GetReg(), r_base.GetReg(), offset);
  if (mem_ref_type_ == ResourceMask::kDalvikReg) {
    DCHECK_EQ(r_base, cu_->target64 ? rs_rX86_SP_64 : rs_rX86_SP_32);
    AnnotateDalvikRegAccess(l, offset >> 2, true /* is_load */, false /* is_64bit */);
  }
  return l;
}

LIR* X86Mir2Lir::OpMemReg(OpKind op, RegLocation rl_dest, int r_value) {
  DCHECK_NE(rl_dest.location, kLocPhysReg);
  int displacement = SRegOffset(rl_dest.s_reg_low);
  bool is64Bit = rl_dest.wide != 0;
  X86OpCode opcode = kX86Nop;
  switch (op) {
    case kOpSub: opcode = is64Bit ? kX86Sub64MR : kX86Sub32MR; break;
    case kOpMov: opcode = is64Bit ? kX86Mov64MR : kX86Mov32MR; break;
    case kOpCmp: opcode = is64Bit ? kX86Cmp64MR : kX86Cmp32MR; break;
    case kOpAdd: opcode = is64Bit ? kX86Add64MR : kX86Add32MR; break;
    case kOpAnd: opcode = is64Bit ? kX86And64MR : kX86And32MR; break;
    case kOpOr:  opcode = is64Bit ? kX86Or64MR : kX86Or32MR; break;
    case kOpXor: opcode = is64Bit ? kX86Xor64MR : kX86Xor32MR; break;
    case kOpLsl: opcode = is64Bit ? kX86Sal64MC : kX86Sal32MC; break;
    case kOpLsr: opcode = is64Bit ? kX86Shr64MC : kX86Shr32MC; break;
    case kOpAsr: opcode = is64Bit ? kX86Sar64MC : kX86Sar32MC; break;
    default:
      LOG(FATAL) << "Bad case in OpMemReg " << op;
      break;
  }
  LIR *l = NewLIR3(opcode, rs_rX86_SP_32.GetReg(), displacement, r_value);
  if (mem_ref_type_ == ResourceMask::kDalvikReg) {
    AnnotateDalvikRegAccess(l, displacement >> 2, true /* is_load */, is64Bit /* is_64bit */);
    AnnotateDalvikRegAccess(l, displacement >> 2, false /* is_load */, is64Bit /* is_64bit */);
  }
  return l;
}

LIR* X86Mir2Lir::OpRegMem(OpKind op, RegStorage r_dest, RegLocation rl_value) {
  DCHECK_NE(rl_value.location, kLocPhysReg);
  bool is64Bit = r_dest.Is64Bit();
  int displacement = SRegOffset(rl_value.s_reg_low);
  X86OpCode opcode = kX86Nop;
  switch (op) {
    case kOpSub: opcode = is64Bit ? kX86Sub64RM : kX86Sub32RM; break;
    case kOpMov: opcode = is64Bit ? kX86Mov64RM : kX86Mov32RM; break;
    case kOpCmp: opcode = is64Bit ? kX86Cmp64RM : kX86Cmp32RM; break;
    case kOpAdd: opcode = is64Bit ? kX86Add64RM : kX86Add32RM; break;
    case kOpAnd: opcode = is64Bit ? kX86And64RM : kX86And32RM; break;
    case kOpOr:  opcode = is64Bit ? kX86Or64RM : kX86Or32RM; break;
    case kOpXor: opcode = is64Bit ? kX86Xor64RM : kX86Xor32RM; break;
    case kOpMul: opcode = is64Bit ? kX86Bkpt : kX86Imul32RM; break;
    default:
      LOG(FATAL) << "Bad case in OpRegMem " << op;
      break;
  }
  LIR *l = NewLIR3(opcode, r_dest.GetReg(), rs_rX86_SP_32.GetReg(), displacement);
  if (mem_ref_type_ == ResourceMask::kDalvikReg) {
    AnnotateDalvikRegAccess(l, displacement >> 2, true /* is_load */, is64Bit /* is_64bit */);
  }
  return l;
}

LIR* X86Mir2Lir::OpRegRegReg(OpKind op, RegStorage r_dest, RegStorage r_src1,
                             RegStorage r_src2) {
  bool is64Bit = r_dest.Is64Bit();
  if (r_dest != r_src1 && r_dest != r_src2) {
    if (op == kOpAdd) {  // lea special case, except can't encode rbp as base
      if (r_src1 == r_src2) {
        OpRegCopy(r_dest, r_src1);
        return OpRegImm(kOpLsl, r_dest, 1);
      } else if (r_src1 != rs_rBP) {
        return NewLIR5(is64Bit ? kX86Lea64RA : kX86Lea32RA, r_dest.GetReg(),
                       r_src1.GetReg() /* base */, r_src2.GetReg() /* index */,
                       0 /* scale */, 0 /* disp */);
      } else {
        return NewLIR5(is64Bit ? kX86Lea64RA : kX86Lea32RA, r_dest.GetReg(),
                       r_src2.GetReg() /* base */, r_src1.GetReg() /* index */,
                       0 /* scale */, 0 /* disp */);
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
      case kOpMul:
        break;
      default:
        LOG(FATAL) << "Bad case in OpRegRegReg " << op;
    }
    return OpRegReg(op, r_dest, r_src1);
  }
}

LIR* X86Mir2Lir::OpRegRegImm(OpKind op, RegStorage r_dest, RegStorage r_src, int value) {
  if (op == kOpMul && !cu_->target64) {
    X86OpCode opcode = IS_SIMM8(value) ? kX86Imul32RRI8 : kX86Imul32RRI;
    return NewLIR3(opcode, r_dest.GetReg(), r_src.GetReg(), value);
  } else if (op == kOpAnd && !cu_->target64) {
    if (value == 0xFF && r_src.Low4()) {
      return NewLIR2(kX86Movzx8RR, r_dest.GetReg(), r_src.GetReg());
    } else if (value == 0xFFFF) {
      return NewLIR2(kX86Movzx16RR, r_dest.GetReg(), r_src.GetReg());
    }
  }
  if (r_dest != r_src) {
    if ((false) && op == kOpLsl && value >= 0 && value <= 3) {  // lea shift special case
      // TODO: fix bug in LEA encoding when disp == 0
      return NewLIR5(kX86Lea32RA, r_dest.GetReg(),  r5sib_no_base /* base */,
                     r_src.GetReg() /* index */, value /* scale */, 0 /* disp */);
    } else if (op == kOpAdd) {  // lea add special case
      return NewLIR5(r_dest.Is64Bit() ? kX86Lea64RA : kX86Lea32RA, r_dest.GetReg(),
                     r_src.GetReg() /* base */, rs_rX86_SP_32.GetReg()/*r4sib_no_index*/ /* index */,
                     0 /* scale */, value /* disp */);
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
    bool is_fp = r_dest.IsFloat();
    // TODO: clean this up once we fully recognize 64-bit storage containers.
    if (is_fp) {
      DCHECK(r_dest.IsDouble());
      if (value == 0) {
        return NewLIR2(kX86XorpdRR, low_reg_val, low_reg_val);
      } else if (pc_rel_base_reg_.Valid() || cu_->target64) {
        // We will load the value from the literal area.
        LIR* data_target = ScanLiteralPoolWide(literal_list_, val_lo, val_hi);
        if (data_target == nullptr) {
          data_target = AddWideData(&literal_list_, val_lo, val_hi);
        }

        // Load the proper value from the literal area.
        // We don't know the proper offset for the value, so pick one that
        // will force 4 byte offset.  We will fix this up in the assembler
        // later to have the right value.
        ScopedMemRefType mem_ref_type(this, ResourceMask::kLiteral);
        if (cu_->target64) {
          res = NewLIR3(kX86MovsdRM, low_reg_val, kRIPReg, 256 /* bogus */);
        } else {
          // Get the PC to a register and get the anchor.
          LIR* anchor;
          RegStorage r_pc = GetPcAndAnchor(&anchor);

          res = LoadBaseDisp(r_pc, kDummy32BitOffset, RegStorage::FloatSolo64(low_reg_val),
                             kDouble, kNotVolatile);
          res->operands[4] = WrapPointer(anchor);
          if (IsTemp(r_pc)) {
            FreeTemp(r_pc);
          }
        }
        res->target = data_target;
        res->flags.fixup = kFixupLoad;
      } else {
        if (r_dest.IsPair()) {
          if (val_lo == 0) {
            res = NewLIR2(kX86XorpsRR, low_reg_val, low_reg_val);
          } else {
            res = LoadConstantNoClobber(RegStorage::FloatSolo32(low_reg_val), val_lo);
          }
          if (val_hi != 0) {
            RegStorage r_dest_hi = AllocTempDouble();
            LoadConstantNoClobber(r_dest_hi, val_hi);
            NewLIR2(kX86PunpckldqRR, low_reg_val, r_dest_hi.GetReg());
            FreeTemp(r_dest_hi);
          }
        } else {
          RegStorage r_temp = AllocTypedTempWide(false, kCoreReg);
          res = LoadConstantWide(r_temp, value);
          OpRegCopyWide(r_dest, r_temp);
          FreeTemp(r_temp);
        }
      }
    } else {
      if (r_dest.IsPair()) {
        res = LoadConstantNoClobber(r_dest.GetLow(), val_lo);
        LoadConstantNoClobber(r_dest.GetHigh(), val_hi);
      } else {
        if (value == 0) {
          res = NewLIR2(kX86Xor64RR, r_dest.GetReg(), r_dest.GetReg());
        } else if (value >= INT_MIN && value <= INT_MAX) {
          res = NewLIR2(kX86Mov64RI32, r_dest.GetReg(), val_lo);
        } else {
          res = NewLIR3(kX86Mov64RI64, r_dest.GetReg(), val_hi, val_lo);
        }
      }
    }
    return res;
}

LIR* X86Mir2Lir::LoadBaseIndexedDisp(RegStorage r_base, RegStorage r_index, int scale,
                                     int displacement, RegStorage r_dest, OpSize size) {
  LIR *load = nullptr;
  LIR *load2 = nullptr;
  bool is_array = r_index.Valid();
  bool pair = r_dest.IsPair();
  bool is64bit = ((size == k64) || (size == kDouble));
  X86OpCode opcode = kX86Nop;
  switch (size) {
    case k64:
    case kDouble:
      if (r_dest.IsFloat()) {
        opcode = is_array ? kX86MovsdRA : kX86MovsdRM;
      } else if (!pair) {
        opcode = is_array ? kX86Mov64RA  : kX86Mov64RM;
      } else {
        opcode = is_array ? kX86Mov32RA  : kX86Mov32RM;
      }
      // TODO: double store is to unaligned address
      DCHECK_EQ((displacement & 0x3), 0);
      break;
    case kWord:
      if (cu_->target64) {
        opcode = is_array ? kX86Mov64RA  : kX86Mov64RM;
        CHECK_EQ(is_array, false);
        CHECK_EQ(r_dest.IsFloat(), false);
        break;
      }
      FALLTHROUGH_INTENDED;  // else fall-through to k32 case
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
        load = NewLIR3(opcode, r_dest.GetHighReg(), r_base.GetReg(),
                        displacement + HIWORD_OFFSET);
        load2 = NewLIR3(opcode, r_dest.GetLowReg(), r_base.GetReg(), displacement + LOWORD_OFFSET);
      } else {
        load = NewLIR3(opcode, r_dest.GetLowReg(), r_base.GetReg(), displacement + LOWORD_OFFSET);
        load2 = NewLIR3(opcode, r_dest.GetHighReg(), r_base.GetReg(),
                        displacement + HIWORD_OFFSET);
      }
    }
    if (mem_ref_type_ == ResourceMask::kDalvikReg) {
      DCHECK_EQ(r_base, cu_->target64 ? rs_rX86_SP_64 : rs_rX86_SP_32);
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
          load = NewLIR5(opcode, temp.GetReg(), r_base.GetReg(), r_index.GetReg(), scale,
                          displacement + HIWORD_OFFSET);
          load2 = NewLIR5(opcode, r_dest.GetLowReg(), r_base.GetReg(), r_index.GetReg(), scale,
                         displacement + LOWORD_OFFSET);
          OpRegCopy(r_dest.GetHigh(), temp);
          FreeTemp(temp);
        } else {
          load = NewLIR5(opcode, r_dest.GetHighReg(), r_base.GetReg(), r_index.GetReg(), scale,
                          displacement + HIWORD_OFFSET);
          load2 = NewLIR5(opcode, r_dest.GetLowReg(), r_base.GetReg(), r_index.GetReg(), scale,
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

  // Always return first load generated as this might cause a fault if base is null.
  return load;
}

/* Load value from base + scaled index. */
LIR* X86Mir2Lir::LoadBaseIndexed(RegStorage r_base, RegStorage r_index, RegStorage r_dest,
                                 int scale, OpSize size) {
  return LoadBaseIndexedDisp(r_base, r_index, scale, 0, r_dest, size);
}

LIR* X86Mir2Lir::LoadBaseDisp(RegStorage r_base, int displacement, RegStorage r_dest,
                              OpSize size, VolatileKind is_volatile) {
  // LoadBaseDisp() will emit correct insn for atomic load on x86
  // assuming r_dest is correctly prepared using RegClassForFieldLoadStore().

  LIR* load = LoadBaseIndexedDisp(r_base, RegStorage::InvalidReg(), 0, displacement, r_dest,
                                  size);

  if (UNLIKELY(is_volatile == kVolatile)) {
    GenMemBarrier(kLoadAny);  // Only a scheduling barrier.
  }

  return load;
}

LIR* X86Mir2Lir::StoreBaseIndexedDisp(RegStorage r_base, RegStorage r_index, int scale,
                                      int displacement, RegStorage r_src, OpSize size,
                                      int opt_flags) {
  LIR *store = nullptr;
  LIR *store2 = nullptr;
  bool is_array = r_index.Valid();
  bool pair = r_src.IsPair();
  bool is64bit = (size == k64) || (size == kDouble);
  bool consider_non_temporal = false;

  X86OpCode opcode = kX86Nop;
  switch (size) {
    case k64:
      consider_non_temporal = true;
      FALLTHROUGH_INTENDED;
    case kDouble:
      if (r_src.IsFloat()) {
        opcode = is_array ? kX86MovsdAR : kX86MovsdMR;
      } else if (!pair) {
        opcode = is_array ? kX86Mov64AR  : kX86Mov64MR;
      } else {
        opcode = is_array ? kX86Mov32AR  : kX86Mov32MR;
      }
      // TODO: double store is to unaligned address
      DCHECK_EQ((displacement & 0x3), 0);
      break;
    case kWord:
      if (cu_->target64) {
        opcode = is_array ? kX86Mov64AR  : kX86Mov64MR;
        CHECK_EQ(is_array, false);
        CHECK_EQ(r_src.IsFloat(), false);
        consider_non_temporal = true;
        break;
      }
      FALLTHROUGH_INTENDED;  // else fall-through to k32 case
    case k32:
    case kSingle:
    case kReference:
      opcode = is_array ? kX86Mov32AR : kX86Mov32MR;
      if (r_src.IsFloat()) {
        opcode = is_array ? kX86MovssAR : kX86MovssMR;
        DCHECK(r_src.IsSingle());
      }
      DCHECK_EQ((displacement & 0x3), 0);
      consider_non_temporal = true;
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

  // Handle non temporal hint here.
  if (consider_non_temporal && ((opt_flags & MIR_STORE_NON_TEMPORAL) != 0)) {
    switch (opcode) {
      // We currently only handle 32/64 bit moves here.
      case kX86Mov64AR:
        opcode = kX86Movnti64AR;
        break;
      case kX86Mov64MR:
        opcode = kX86Movnti64MR;
        break;
      case kX86Mov32AR:
        opcode = kX86Movnti32AR;
        break;
      case kX86Mov32MR:
        opcode = kX86Movnti32MR;
        break;
      default:
        // Do nothing here.
        break;
    }
  }

  if (!is_array) {
    if (!pair) {
      store = NewLIR3(opcode, r_base.GetReg(), displacement + LOWORD_OFFSET, r_src.GetReg());
    } else {
      DCHECK(!r_src.IsFloat());  // Make sure we're not still using a pair here.
      store = NewLIR3(opcode, r_base.GetReg(), displacement + LOWORD_OFFSET, r_src.GetLowReg());
      store2 = NewLIR3(opcode, r_base.GetReg(), displacement + HIWORD_OFFSET, r_src.GetHighReg());
    }
    if (mem_ref_type_ == ResourceMask::kDalvikReg) {
      DCHECK_EQ(r_base, cu_->target64 ? rs_rX86_SP_64 : rs_rX86_SP_32);
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

LIR* X86Mir2Lir::StoreBaseDisp(RegStorage r_base, int displacement, RegStorage r_src, OpSize size,
                               VolatileKind is_volatile) {
  if (UNLIKELY(is_volatile == kVolatile)) {
    GenMemBarrier(kAnyStore);  // Only a scheduling barrier.
  }

  // StoreBaseDisp() will emit correct insn for atomic store on x86
  // assuming r_dest is correctly prepared using RegClassForFieldLoadStore().
  // x86 only allows registers EAX-EDX to be used as byte registers, if the input src is not
  // valid, allocate a temp.
  bool allocated_temp = false;
  if (size == kUnsignedByte || size == kSignedByte) {
    if (!cu_->target64 && !r_src.Low4()) {
      RegStorage r_input = r_src;
      r_src = AllocateByteRegister();
      OpRegCopy(r_src, r_input);
      allocated_temp = true;
    }
  }

  LIR* store = StoreBaseIndexedDisp(r_base, RegStorage::InvalidReg(), 0, displacement, r_src, size);

  if (UNLIKELY(is_volatile == kVolatile)) {
    // A volatile load might follow the volatile store so insert a StoreLoad barrier.
    // This does require a fence, even on x86.
    GenMemBarrier(kAnyAny);
  }

  if (allocated_temp) {
    FreeTemp(r_src);
  }

  return store;
}

LIR* X86Mir2Lir::OpCmpMemImmBranch(ConditionCode cond, RegStorage temp_reg, RegStorage base_reg,
                                   int offset, int check_value, LIR* target, LIR** compare) {
  UNUSED(temp_reg);  // Comparison performed directly with memory.
  LIR* inst = NewLIR3(IS_SIMM8(check_value) ? kX86Cmp32MI8 : kX86Cmp32MI, base_reg.GetReg(),
      offset, check_value);
  if (compare != nullptr) {
    *compare = inst;
  }
  LIR* branch = OpCondBranch(cond, target);
  return branch;
}

void X86Mir2Lir::AnalyzeMIR(RefCounts* core_counts, MIR* mir, uint32_t weight) {
  if (cu_->target64) {
    Mir2Lir::AnalyzeMIR(core_counts, mir, weight);
    return;
  }

  int opcode = mir->dalvikInsn.opcode;
  bool uses_pc_rel_load = false;
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
    case kMirOpFusedCmplDouble:
    case kMirOpFusedCmpgDouble:
      uses_pc_rel_load = AnalyzeFPInstruction(opcode, mir);
      break;

    // Packed switch needs the PC-relative pointer if it's large.
    case Instruction::PACKED_SWITCH:
      if (mir_graph_->GetTable(mir, mir->dalvikInsn.vB)[1] > kSmallSwitchThreshold) {
        uses_pc_rel_load = true;
      }
      break;

    case kMirOpConstVector:
      uses_pc_rel_load = true;
      break;
    case kMirOpPackedMultiply:
    case kMirOpPackedShiftLeft:
    case kMirOpPackedSignedShiftRight:
    case kMirOpPackedUnsignedShiftRight:
      {
        // Byte emulation requires constants from the literal pool.
        OpSize opsize = static_cast<OpSize>(mir->dalvikInsn.vC >> 16);
        if (opsize == kSignedByte || opsize == kUnsignedByte) {
          uses_pc_rel_load = true;
        }
      }
      break;

    case Instruction::INVOKE_STATIC:
    case Instruction::INVOKE_STATIC_RANGE:
      if (mir_graph_->GetMethodLoweringInfo(mir).IsIntrinsic()) {
        uses_pc_rel_load = AnalyzeInvokeStaticIntrinsic(mir);
        break;
      }
      FALLTHROUGH_INTENDED;
    default:
      Mir2Lir::AnalyzeMIR(core_counts, mir, weight);
      break;
  }

  if (uses_pc_rel_load) {
    DCHECK(pc_rel_temp_ != nullptr);
    core_counts[SRegToPMap(pc_rel_temp_->s_reg_low)].count += weight;
  }
}

bool X86Mir2Lir::AnalyzeFPInstruction(int opcode, MIR* mir) {
  DCHECK(!cu_->target64);
  // Look at all the uses, and see if they are double constants.
  uint64_t attrs = MIRGraph::GetDataFlowAttributes(static_cast<Instruction::Code>(opcode));
  int next_sreg = 0;
  if (attrs & DF_UA) {
    if (attrs & DF_A_WIDE) {
      if (AnalyzeDoubleUse(mir_graph_->GetSrcWide(mir, next_sreg))) {
        return true;
      }
      next_sreg += 2;
    } else {
      next_sreg++;
    }
  }
  if (attrs & DF_UB) {
    if (attrs & DF_B_WIDE) {
      if (AnalyzeDoubleUse(mir_graph_->GetSrcWide(mir, next_sreg))) {
        return true;
      }
      next_sreg += 2;
    } else {
      next_sreg++;
    }
  }
  if (attrs & DF_UC) {
    if (attrs & DF_C_WIDE) {
      if (AnalyzeDoubleUse(mir_graph_->GetSrcWide(mir, next_sreg))) {
        return true;
      }
    }
  }
  return false;
}

inline bool X86Mir2Lir::AnalyzeDoubleUse(RegLocation use) {
  // If this is a double literal, we will want it in the literal pool on 32b platforms.
  DCHECK(!cu_->target64);
  return use.is_const;
}

bool X86Mir2Lir::AnalyzeInvokeStaticIntrinsic(MIR* mir) {
  // 64 bit RIP addressing doesn't need this analysis.
  DCHECK(!cu_->target64);

  // Retrieve the type of the intrinsic.
  MethodReference method_ref = mir_graph_->GetMethodLoweringInfo(mir).GetTargetMethod();
  DCHECK(cu_->compiler_driver->GetMethodInlinerMap() != nullptr);
  DexFileMethodInliner* method_inliner =
    cu_->compiler_driver->GetMethodInlinerMap()->GetMethodInliner(method_ref.dex_file);
  InlineMethod method;
  bool is_intrinsic = method_inliner->IsIntrinsic(method_ref.dex_method_index, &method);
  DCHECK(is_intrinsic);

  switch (method.opcode) {
    case kIntrinsicAbsDouble:
    case kIntrinsicMinMaxDouble:
      return true;
    default:
      return false;
  }
}

RegLocation X86Mir2Lir::UpdateLocTyped(RegLocation loc) {
  loc = UpdateLoc(loc);
  if ((loc.location == kLocPhysReg) && (loc.fp != loc.reg.IsFloat())) {
    if (GetRegInfo(loc.reg)->IsTemp()) {
      Clobber(loc.reg);
      FreeTemp(loc.reg);
      loc.reg = RegStorage::InvalidReg();
      loc.location = kLocDalvikFrame;
    }
  }
  DCHECK(CheckCorePoolSanity());
  return loc;
}

RegLocation X86Mir2Lir::UpdateLocWideTyped(RegLocation loc) {
  loc = UpdateLocWide(loc);
  if ((loc.location == kLocPhysReg) && (loc.fp != loc.reg.IsFloat())) {
    if (GetRegInfo(loc.reg)->IsTemp()) {
      Clobber(loc.reg);
      FreeTemp(loc.reg);
      loc.reg = RegStorage::InvalidReg();
      loc.location = kLocDalvikFrame;
    }
  }
  DCHECK(CheckCorePoolSanity());
  return loc;
}

LIR* X86Mir2Lir::InvokeTrampoline(OpKind op, RegStorage r_tgt, QuickEntrypointEnum trampoline) {
  UNUSED(r_tgt);  // Call to absolute memory location doesn't need a temporary target register.
  if (cu_->target64) {
    return OpThreadMem(op, GetThreadOffset<8>(trampoline));
  } else {
    return OpThreadMem(op, GetThreadOffset<4>(trampoline));
  }
}

void X86Mir2Lir::CountRefs(RefCounts* core_counts, RefCounts* fp_counts, size_t num_regs) {
  // Start with the default counts.
  Mir2Lir::CountRefs(core_counts, fp_counts, num_regs);

  if (pc_rel_temp_ != nullptr) {
    // Now, if the dex cache array base temp is used only once outside any loops (weight = 1),
    // avoid the promotion, otherwise boost the weight by factor 2 because the full PC-relative
    // load sequence is 3 instructions long and by promoting the PC base we save 2 instructions
    // per use.
    int p_map_idx = SRegToPMap(pc_rel_temp_->s_reg_low);
    if (core_counts[p_map_idx].count == 1) {
      core_counts[p_map_idx].count = 0;
    } else {
      core_counts[p_map_idx].count *= 2;
    }
  }
}

void X86Mir2Lir::DoPromotion() {
  if (!cu_->target64) {
    pc_rel_temp_ = mir_graph_->GetNewCompilerTemp(kCompilerTempBackend, false);
  }

  Mir2Lir::DoPromotion();

  if (pc_rel_temp_ != nullptr) {
    // Now, if the dex cache array base temp is promoted, remember the register but
    // always remove the temp's stack location to avoid unnecessarily bloating the stack.
    pc_rel_base_reg_ = mir_graph_->reg_location_[pc_rel_temp_->s_reg_low].reg;
    DCHECK(!pc_rel_base_reg_.Valid() || !pc_rel_base_reg_.IsFloat());
    mir_graph_->RemoveLastCompilerTemp(kCompilerTempBackend, false, pc_rel_temp_);
    pc_rel_temp_ = nullptr;
  }
}

}  // namespace art
