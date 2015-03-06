/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "codegen_mips64.h"

#include "arch/mips64/instruction_set_features_mips64.h"
#include "base/logging.h"
#include "dex/quick/mir_to_lir-inl.h"
#include "dex/reg_storage_eq.h"
#include "driver/compiler_driver.h"
#include "mips64_lir.h"

namespace art {

/* This file contains codegen for the MIPS64 ISA. */

LIR* Mips64Mir2Lir::OpFpRegCopy(RegStorage r_dest, RegStorage r_src) {
  int opcode;
  // Must be both DOUBLE or both not DOUBLE.
  DCHECK_EQ(r_dest.Is64Bit(), r_src.Is64Bit());
  if (r_dest.Is64Bit()) {
    if (r_dest.IsDouble()) {
      if (r_src.IsDouble()) {
        opcode = kMips64Fmovd;
      } else {
        // Note the operands are swapped for the dmtc1 instr.
        RegStorage t_opnd = r_src;
        r_src = r_dest;
        r_dest = t_opnd;
        opcode = kMips64Dmtc1;
      }
    } else {
      DCHECK(r_src.IsDouble());
      opcode = kMips64Dmfc1;
    }
  } else {
    if (r_dest.IsSingle()) {
      if (r_src.IsSingle()) {
        opcode = kMips64Fmovs;
      } else {
        // Note the operands are swapped for the mtc1 instr.
        RegStorage t_opnd = r_src;
        r_src = r_dest;
        r_dest = t_opnd;
        opcode = kMips64Mtc1;
      }
    } else {
      DCHECK(r_src.IsSingle());
      opcode = kMips64Mfc1;
    }
  }
  LIR* res = RawLIR(current_dalvik_offset_, opcode, r_dest.GetReg(), r_src.GetReg());
  if (!(cu_->disable_opt & (1 << kSafeOptimizations)) && r_dest == r_src) {
    res->flags.is_nop = true;
  }
  return res;
}

bool Mips64Mir2Lir::InexpensiveConstantInt(int32_t value) {
  // For encodings, see LoadConstantNoClobber below.
  return ((value == 0) || IsUint<16>(value) || IsInt<16>(value));
}

bool Mips64Mir2Lir::InexpensiveConstantFloat(int32_t value) {
  UNUSED(value);
  return false;  // TUNING
}

bool Mips64Mir2Lir::InexpensiveConstantLong(int64_t value) {
  UNUSED(value);
  return false;  // TUNING
}

bool Mips64Mir2Lir::InexpensiveConstantDouble(int64_t value) {
  UNUSED(value);
  return false;  // TUNING
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
LIR* Mips64Mir2Lir::LoadConstantNoClobber(RegStorage r_dest, int value) {
  LIR *res;

  RegStorage r_dest_save = r_dest;
  int is_fp_reg = r_dest.IsFloat();
  if (is_fp_reg) {
    DCHECK(r_dest.IsSingle());
    r_dest = AllocTemp();
  }

  // See if the value can be constructed cheaply.
  if (value == 0) {
    res = NewLIR2(kMips64Move, r_dest.GetReg(), rZERO);
  } else if (IsUint<16>(value)) {
    // Use OR with (unsigned) immediate to encode 16b unsigned int.
    res = NewLIR3(kMips64Ori, r_dest.GetReg(), rZERO, value);
  } else if (IsInt<16>(value)) {
    // Use ADD with (signed) immediate to encode 16b signed int.
    res = NewLIR3(kMips64Addiu, r_dest.GetReg(), rZERO, value);
  } else {
    res = NewLIR2(kMips64Lui, r_dest.GetReg(), value >> 16);
    if (value & 0xffff)
      NewLIR3(kMips64Ori, r_dest.GetReg(), r_dest.GetReg(), value);
  }

  if (is_fp_reg) {
    NewLIR2(kMips64Mtc1, r_dest.GetReg(), r_dest_save.GetReg());
    FreeTemp(r_dest);
  }

  return res;
}

LIR* Mips64Mir2Lir::OpUnconditionalBranch(LIR* target) {
  LIR* res = NewLIR1(kMips64B, 0 /* offset to be patched during assembly*/);
  res->target = target;
  return res;
}

LIR* Mips64Mir2Lir::OpReg(OpKind op, RegStorage r_dest_src) {
  Mips64OpCode opcode = kMips64Nop;
  switch (op) {
    case kOpBlx:
      opcode = kMips64Jalr;
      break;
    case kOpBx:
      return NewLIR2(kMips64Jalr, rZERO, r_dest_src.GetReg());
      break;
    default:
      LOG(FATAL) << "Bad case in OpReg";
  }
  return NewLIR2(opcode, rRAd, r_dest_src.GetReg());
}

LIR* Mips64Mir2Lir::OpRegImm(OpKind op, RegStorage r_dest_src1, int value) {
  LIR *res;
  bool neg = (value < 0);
  int abs_value = (neg) ? -value : value;
  bool short_form = (abs_value & 0xff) == abs_value;
  bool is64bit = r_dest_src1.Is64Bit();
  RegStorage r_scratch;
  Mips64OpCode opcode = kMips64Nop;
  switch (op) {
    case kOpAdd:
      return OpRegRegImm(op, r_dest_src1, r_dest_src1, value);
    case kOpSub:
      return OpRegRegImm(op, r_dest_src1, r_dest_src1, value);
    default:
      LOG(FATAL) << "Bad case in OpRegImm";
  }
  if (short_form) {
    res = NewLIR2(opcode, r_dest_src1.GetReg(), abs_value);
  } else {
    if (is64bit) {
      r_scratch = AllocTempWide();
      res = LoadConstantWide(r_scratch, value);
    } else {
      r_scratch = AllocTemp();
      res = LoadConstant(r_scratch, value);
    }
    if (op == kOpCmp) {
      NewLIR2(opcode, r_dest_src1.GetReg(), r_scratch.GetReg());
    } else {
      NewLIR3(opcode, r_dest_src1.GetReg(), r_dest_src1.GetReg(), r_scratch.GetReg());
    }
  }
  return res;
}

LIR* Mips64Mir2Lir::OpRegRegReg(OpKind op, RegStorage r_dest,
                                RegStorage r_src1, RegStorage r_src2) {
  Mips64OpCode opcode = kMips64Nop;
  bool is64bit = r_dest.Is64Bit() || r_src1.Is64Bit() || r_src2.Is64Bit();

  switch (op) {
    case kOpAdd:
      if (is64bit) {
        opcode = kMips64Daddu;
      } else {
        opcode = kMips64Addu;
      }
      break;
    case kOpSub:
      if (is64bit) {
        opcode = kMips64Dsubu;
      } else {
        opcode = kMips64Subu;
      }
      break;
    case kOpAnd:
      opcode = kMips64And;
      break;
    case kOpMul:
      opcode = kMips64Mul;
      break;
    case kOpOr:
      opcode = kMips64Or;
      break;
    case kOpXor:
      opcode = kMips64Xor;
      break;
    case kOpLsl:
      if (is64bit) {
        opcode = kMips64Dsllv;
      } else {
        opcode = kMips64Sllv;
      }
      break;
    case kOpLsr:
      if (is64bit) {
        opcode = kMips64Dsrlv;
      } else {
        opcode = kMips64Srlv;
      }
      break;
    case kOpAsr:
      if (is64bit) {
        opcode = kMips64Dsrav;
      } else {
        opcode = kMips64Srav;
      }
      break;
    case kOpAdc:
    case kOpSbc:
      LOG(FATAL) << "No carry bit on MIPS64";
      break;
    default:
      LOG(FATAL) << "Bad case in OpRegRegReg";
      break;
  }
  return NewLIR3(opcode, r_dest.GetReg(), r_src1.GetReg(), r_src2.GetReg());
}

LIR* Mips64Mir2Lir::OpRegRegImm(OpKind op, RegStorage r_dest, RegStorage r_src1, int value) {
  LIR *res;
  Mips64OpCode opcode = kMips64Nop;
  bool short_form = true;
  bool is64bit = r_dest.Is64Bit() || r_src1.Is64Bit();

  switch (op) {
    case kOpAdd:
      if (is64bit) {
        if (IS_SIMM16(value)) {
          opcode = kMips64Daddiu;
        } else {
          short_form = false;
          opcode = kMips64Daddu;
        }
      } else {
        if (IS_SIMM16(value)) {
          opcode = kMips64Addiu;
        } else {
          short_form = false;
          opcode = kMips64Addu;
        }
      }
      break;
    case kOpSub:
      if (is64bit) {
        if (IS_SIMM16((-value))) {
          value = -value;
          opcode = kMips64Daddiu;
        } else {
          short_form = false;
          opcode = kMips64Dsubu;
        }
      } else {
        if (IS_SIMM16((-value))) {
          value = -value;
          opcode = kMips64Addiu;
        } else {
          short_form = false;
          opcode = kMips64Subu;
        }
      }
      break;
    case kOpLsl:
      if (is64bit) {
        DCHECK(value >= 0 && value <= 63);
        if (value >= 0 && value <= 31) {
          opcode = kMips64Dsll;
        } else {
          opcode = kMips64Dsll32;
          value = value - 32;
        }
      } else {
        DCHECK(value >= 0 && value <= 31);
        opcode = kMips64Sll;
      }
      break;
    case kOpLsr:
      if (is64bit) {
        DCHECK(value >= 0 && value <= 63);
        if (value >= 0 && value <= 31) {
          opcode = kMips64Dsrl;
        } else {
          opcode = kMips64Dsrl32;
          value = value - 32;
        }
      } else {
        DCHECK(value >= 0 && value <= 31);
        opcode = kMips64Srl;
      }
      break;
    case kOpAsr:
      if (is64bit) {
        DCHECK(value >= 0 && value <= 63);
        if (value >= 0 && value <= 31) {
          opcode = kMips64Dsra;
        } else {
          opcode = kMips64Dsra32;
          value = value - 32;
        }
      } else {
        DCHECK(value >= 0 && value <= 31);
        opcode = kMips64Sra;
      }
      break;
    case kOpAnd:
      if (IS_UIMM16((value))) {
        opcode = kMips64Andi;
      } else {
        short_form = false;
        opcode = kMips64And;
      }
      break;
    case kOpOr:
      if (IS_UIMM16((value))) {
        opcode = kMips64Ori;
      } else {
        short_form = false;
        opcode = kMips64Or;
      }
      break;
    case kOpXor:
      if (IS_UIMM16((value))) {
        opcode = kMips64Xori;
      } else {
        short_form = false;
        opcode = kMips64Xor;
      }
      break;
    case kOpMul:
      short_form = false;
      opcode = kMips64Mul;
      break;
    default:
      LOG(FATAL) << "Bad case in OpRegRegImm";
      break;
  }

  if (short_form) {
    res = NewLIR3(opcode, r_dest.GetReg(), r_src1.GetReg(), value);
  } else {
    if (r_dest != r_src1) {
      res = LoadConstant(r_dest, value);
      NewLIR3(opcode, r_dest.GetReg(), r_src1.GetReg(), r_dest.GetReg());
    } else {
      if (is64bit) {
        RegStorage r_scratch = AllocTempWide();
        res = LoadConstantWide(r_scratch, value);
        NewLIR3(opcode, r_dest.GetReg(), r_src1.GetReg(), r_scratch.GetReg());
      } else {
        RegStorage r_scratch = AllocTemp();
        res = LoadConstant(r_scratch, value);
        NewLIR3(opcode, r_dest.GetReg(), r_src1.GetReg(), r_scratch.GetReg());
      }
    }
  }
  return res;
}

LIR* Mips64Mir2Lir::OpRegReg(OpKind op, RegStorage r_dest_src1, RegStorage r_src2) {
  Mips64OpCode opcode = kMips64Nop;
  LIR *res;
  switch (op) {
    case kOpMov:
      opcode = kMips64Move;
      break;
    case kOpMvn:
      return NewLIR3(kMips64Nor, r_dest_src1.GetReg(), r_src2.GetReg(), rZEROd);
    case kOpNeg:
      if (r_dest_src1.Is64Bit())
        return NewLIR3(kMips64Dsubu, r_dest_src1.GetReg(), rZEROd, r_src2.GetReg());
      else
        return NewLIR3(kMips64Subu, r_dest_src1.GetReg(), rZERO, r_src2.GetReg());
    case kOpAdd:
    case kOpAnd:
    case kOpMul:
    case kOpOr:
    case kOpSub:
    case kOpXor:
      return OpRegRegReg(op, r_dest_src1, r_dest_src1, r_src2);
    case kOp2Byte:
      res = NewLIR2(kMips64Seb, r_dest_src1.GetReg(), r_src2.GetReg());
      return res;
    case kOp2Short:
      res = NewLIR2(kMips64Seh, r_dest_src1.GetReg(), r_src2.GetReg());
      return res;
    case kOp2Char:
       return NewLIR3(kMips64Andi, r_dest_src1.GetReg(), r_src2.GetReg(), 0xFFFF);
    default:
      LOG(FATAL) << "Bad case in OpRegReg";
      UNREACHABLE();
  }
  return NewLIR2(opcode, r_dest_src1.GetReg(), r_src2.GetReg());
}

LIR* Mips64Mir2Lir::OpMovRegMem(RegStorage r_dest, RegStorage r_base, int offset,
                              MoveType move_type) {
  UNUSED(r_dest, r_base, offset, move_type);
  UNIMPLEMENTED(FATAL);
  UNREACHABLE();
}

LIR* Mips64Mir2Lir::OpMovMemReg(RegStorage r_base, int offset,
                                RegStorage r_src, MoveType move_type) {
  UNUSED(r_base, offset, r_src, move_type);
  UNIMPLEMENTED(FATAL);
  UNREACHABLE();
}

LIR* Mips64Mir2Lir::OpCondRegReg(OpKind op, ConditionCode cc,
                                 RegStorage r_dest, RegStorage r_src) {
  UNUSED(op, cc, r_dest, r_src);
  LOG(FATAL) << "Unexpected use of OpCondRegReg for MIPS64";
  UNREACHABLE();
}

LIR* Mips64Mir2Lir::LoadConstantWide(RegStorage r_dest, int64_t value) {
  LIR *res = nullptr;
  DCHECK(r_dest.Is64Bit());
  RegStorage r_dest_save = r_dest;
  int is_fp_reg = r_dest.IsFloat();
  if (is_fp_reg) {
    DCHECK(r_dest.IsDouble());
    r_dest = AllocTemp();
  }

  int bit31 = (value & UINT64_C(0x80000000)) != 0;

  // Loads with 1 instruction.
  if (IsUint<16>(value)) {
    res = NewLIR3(kMips64Ori, r_dest.GetReg(), rZEROd, value);
  } else if (IsInt<16>(value)) {
    res = NewLIR3(kMips64Daddiu, r_dest.GetReg(), rZEROd, value);
  } else if ((value & 0xFFFF) == 0 && IsInt<16>(value >> 16)) {
    res = NewLIR2(kMips64Lui, r_dest.GetReg(), value >> 16);
  } else if (IsInt<32>(value)) {
    // Loads with 2 instructions.
    res = NewLIR2(kMips64Lui, r_dest.GetReg(), value >> 16);
    NewLIR3(kMips64Ori, r_dest.GetReg(), r_dest.GetReg(), value);
  } else if ((value & 0xFFFF0000) == 0 && IsInt<16>(value >> 32)) {
    res = NewLIR3(kMips64Ori, r_dest.GetReg(), rZEROd, value);
    NewLIR2(kMips64Dahi, r_dest.GetReg(), value >> 32);
  } else if ((value & UINT64_C(0xFFFFFFFF0000)) == 0) {
    res = NewLIR3(kMips64Ori, r_dest.GetReg(), rZEROd, value);
    NewLIR2(kMips64Dati, r_dest.GetReg(), value >> 48);
  } else if ((value & 0xFFFF) == 0 && (value >> 32) >= (-32768 - bit31) &&
             (value >> 32) <= (32767 - bit31)) {
    res = NewLIR2(kMips64Lui, r_dest.GetReg(), value >> 16);
    NewLIR2(kMips64Dahi, r_dest.GetReg(), (value >> 32) + bit31);
  } else if ((value & 0xFFFF) == 0 && ((value >> 31) & 0x1FFFF) == ((0x20000 - bit31) & 0x1FFFF)) {
    res = NewLIR2(kMips64Lui, r_dest.GetReg(), value >> 16);
    NewLIR2(kMips64Dati, r_dest.GetReg(), (value >> 48) + bit31);
  } else {
    int64_t tmp = value;
    int shift_cnt = 0;
    while ((tmp & 1) == 0) {
      tmp >>= 1;
      shift_cnt++;
    }

    if (IsUint<16>(tmp)) {
      res = NewLIR3(kMips64Ori, r_dest.GetReg(), rZEROd, tmp);
      NewLIR3((shift_cnt < 32) ? kMips64Dsll : kMips64Dsll32, r_dest.GetReg(), r_dest.GetReg(),
              shift_cnt & 0x1F);
    } else if (IsInt<16>(tmp)) {
      res = NewLIR3(kMips64Daddiu, r_dest.GetReg(), rZEROd, tmp);
      NewLIR3((shift_cnt < 32) ? kMips64Dsll : kMips64Dsll32, r_dest.GetReg(), r_dest.GetReg(),
              shift_cnt & 0x1F);
    } else if (IsInt<32>(tmp)) {
      // Loads with 3 instructions.
      res = NewLIR2(kMips64Lui, r_dest.GetReg(), tmp >> 16);
      NewLIR3(kMips64Ori, r_dest.GetReg(), r_dest.GetReg(), tmp);
      NewLIR3((shift_cnt < 32) ? kMips64Dsll : kMips64Dsll32, r_dest.GetReg(), r_dest.GetReg(),
              shift_cnt & 0x1F);
    } else {
      tmp = value >> 16;
      shift_cnt = 16;
      while ((tmp & 1) == 0) {
        tmp >>= 1;
        shift_cnt++;
      }

      if (IsUint<16>(tmp)) {
        res = NewLIR3(kMips64Ori, r_dest.GetReg(), rZEROd, tmp);
        NewLIR3((shift_cnt < 32) ? kMips64Dsll : kMips64Dsll32, r_dest.GetReg(), r_dest.GetReg(),
                shift_cnt & 0x1F);
        NewLIR3(kMips64Ori, r_dest.GetReg(), r_dest.GetReg(), value);
      } else if (IsInt<16>(tmp)) {
        res = NewLIR3(kMips64Daddiu, r_dest.GetReg(), rZEROd, tmp);
        NewLIR3((shift_cnt < 32) ? kMips64Dsll : kMips64Dsll32, r_dest.GetReg(), r_dest.GetReg(),
                shift_cnt & 0x1F);
        NewLIR3(kMips64Ori, r_dest.GetReg(), r_dest.GetReg(), value);
      } else {
        // Loads with 3-4 instructions.
        uint64_t tmp2 = value;
        if (((tmp2 >> 16) & 0xFFFF) != 0 || (tmp2 & 0xFFFFFFFF) == 0) {
          res = NewLIR2(kMips64Lui, r_dest.GetReg(), tmp2 >> 16);
        }
        if ((tmp2 & 0xFFFF) != 0) {
          if (res)
            NewLIR3(kMips64Ori, r_dest.GetReg(), r_dest.GetReg(), tmp2);
          else
            res = NewLIR3(kMips64Ori, r_dest.GetReg(), rZEROd, tmp2);
        }
        if (bit31) {
          tmp2 += UINT64_C(0x100000000);
        }
        if (((tmp2 >> 32) & 0xFFFF) != 0) {
          NewLIR2(kMips64Dahi, r_dest.GetReg(), tmp2 >> 32);
        }
        if (tmp2 & UINT64_C(0x800000000000)) {
          tmp2 += UINT64_C(0x1000000000000);
        }
        if ((tmp2 >> 48) != 0) {
          NewLIR2(kMips64Dati, r_dest.GetReg(), tmp2 >> 48);
        }
      }
    }
  }

  if (is_fp_reg) {
    NewLIR2(kMips64Dmtc1, r_dest.GetReg(), r_dest_save.GetReg());
    FreeTemp(r_dest);
  }

  return res;
}

/* Load value from base + scaled index. */
LIR* Mips64Mir2Lir::LoadBaseIndexed(RegStorage r_base, RegStorage r_index, RegStorage r_dest,
                                    int scale, OpSize size) {
  LIR *first = NULL;
  LIR *res;
  RegStorage t_reg;
  Mips64OpCode opcode = kMips64Nop;
  bool is64bit = r_dest.Is64Bit();
  if (is64bit) {
    t_reg = AllocTempWide();
  } else {
    t_reg = AllocTemp();
  }

  if (r_dest.IsFloat()) {
    DCHECK(r_dest.IsSingle());
    DCHECK((size == k32) || (size == kSingle) || (size == kReference));
    size = kSingle;
  } else if (is64bit) {
    size = k64;
  } else {
    if (size == kSingle)
      size = k32;
  }

  if (!scale) {
    if (is64bit) {
      first = NewLIR3(kMips64Daddu, t_reg.GetReg() , r_base.GetReg(), r_index.GetReg());
    } else {
      first = NewLIR3(kMips64Addu, t_reg.GetReg() , r_base.GetReg(), r_index.GetReg());
    }
  } else {
    first = OpRegRegImm(kOpLsl, t_reg, r_index, scale);
    NewLIR3(kMips64Daddu, t_reg.GetReg() , r_base.GetReg(), t_reg.GetReg());
  }

  switch (size) {
    case k64:
      opcode = kMips64Ld;
      break;
    case kSingle:
      opcode = kMips64Flwc1;
      break;
    case k32:
    case kReference:
      opcode = kMips64Lw;
      break;
    case kUnsignedHalf:
      opcode = kMips64Lhu;
      break;
    case kSignedHalf:
      opcode = kMips64Lh;
      break;
    case kUnsignedByte:
      opcode = kMips64Lbu;
      break;
    case kSignedByte:
      opcode = kMips64Lb;
      break;
    default:
      LOG(FATAL) << "Bad case in LoadBaseIndexed";
  }

  res = NewLIR3(opcode, r_dest.GetReg(), 0, t_reg.GetReg());
  FreeTemp(t_reg);
  return (first) ? first : res;
}

/* Store value base base + scaled index. */
LIR* Mips64Mir2Lir::StoreBaseIndexed(RegStorage r_base, RegStorage r_index, RegStorage r_src,
                                     int scale, OpSize size) {
  LIR *first = NULL;
  Mips64OpCode opcode = kMips64Nop;
  RegStorage t_reg = AllocTemp();

  if (r_src.IsFloat()) {
    DCHECK(r_src.IsSingle());
    DCHECK((size == k32) || (size == kSingle) || (size == kReference));
    size = kSingle;
  } else {
    if (size == kSingle)
      size = k32;
  }

  if (!scale) {
    first = NewLIR3(kMips64Daddu, t_reg.GetReg() , r_base.GetReg(), r_index.GetReg());
  } else {
    first = OpRegRegImm(kOpLsl, t_reg, r_index, scale);
    NewLIR3(kMips64Daddu, t_reg.GetReg() , r_base.GetReg(), t_reg.GetReg());
  }

  switch (size) {
    case kSingle:
      opcode = kMips64Fswc1;
      break;
    case k32:
    case kReference:
      opcode = kMips64Sw;
      break;
    case kUnsignedHalf:
    case kSignedHalf:
      opcode = kMips64Sh;
      break;
    case kUnsignedByte:
    case kSignedByte:
      opcode = kMips64Sb;
      break;
    default:
      LOG(FATAL) << "Bad case in StoreBaseIndexed";
  }
  NewLIR3(opcode, r_src.GetReg(), 0, t_reg.GetReg());
  return first;
}

// FIXME: don't split r_dest into 2 containers.
LIR* Mips64Mir2Lir::LoadBaseDispBody(RegStorage r_base, int displacement, RegStorage r_dest,
                                     OpSize size) {
/*
 * Load value from base + displacement.  Optionally perform null check
 * on base (which must have an associated s_reg and MIR).  If not
 * performing null check, incoming MIR can be null. IMPORTANT: this
 * code must not allocate any new temps.  If a new register is needed
 * and base and dest are the same, spill some other register to
 * rlp and then restore.
 */
  LIR *res;
  LIR *load = NULL;
  Mips64OpCode opcode = kMips64Nop;
  bool short_form = IS_SIMM16(displacement);

  switch (size) {
    case k64:
    case kDouble:
      r_dest = Check64BitReg(r_dest);
      if (!r_dest.IsFloat())
        opcode = kMips64Ld;
      else
        opcode = kMips64Fldc1;
      DCHECK_EQ((displacement & 0x3), 0);
      break;
    case k32:
    case kSingle:
    case kReference:
      opcode = kMips64Lw;
      if (r_dest.IsFloat()) {
        opcode = kMips64Flwc1;
        DCHECK(r_dest.IsSingle());
      }
      DCHECK_EQ((displacement & 0x3), 0);
      break;
    case kUnsignedHalf:
      opcode = kMips64Lhu;
      DCHECK_EQ((displacement & 0x1), 0);
      break;
    case kSignedHalf:
      opcode = kMips64Lh;
      DCHECK_EQ((displacement & 0x1), 0);
      break;
    case kUnsignedByte:
      opcode = kMips64Lbu;
      break;
    case kSignedByte:
      opcode = kMips64Lb;
      break;
    default:
      LOG(FATAL) << "Bad case in LoadBaseIndexedBody";
  }

  if (short_form) {
    load = res = NewLIR3(opcode, r_dest.GetReg(), displacement, r_base.GetReg());
  } else {
    RegStorage r_tmp = (r_base == r_dest) ? AllocTemp() : r_dest;
    res = OpRegRegImm(kOpAdd, r_tmp, r_base, displacement);
    load = NewLIR3(opcode, r_dest.GetReg(), 0, r_tmp.GetReg());
    if (r_tmp != r_dest)
      FreeTemp(r_tmp);
  }

  if (mem_ref_type_ == ResourceMask::kDalvikReg) {
    DCHECK_EQ(r_base, rs_rMIPS64_SP);
    AnnotateDalvikRegAccess(load, displacement >> 2, true /* is_load */, r_dest.Is64Bit());
  }
  return res;
}

LIR* Mips64Mir2Lir::LoadBaseDisp(RegStorage r_base, int displacement, RegStorage r_dest,
                                 OpSize size, VolatileKind is_volatile) {
  if (UNLIKELY(is_volatile == kVolatile && (size == k64 || size == kDouble) &&
      displacement & 0x7)) {
    // TODO: use lld/scd instructions for Mips64.
    // Do atomic 64-bit load.
    return GenAtomic64Load(r_base, displacement, r_dest);
  }

  // TODO: base this on target.
  if (size == kWord) {
    size = k64;
  }
  LIR* load;
  load = LoadBaseDispBody(r_base, displacement, r_dest, size);

  if (UNLIKELY(is_volatile == kVolatile)) {
    GenMemBarrier(kLoadAny);
  }

  return load;
}

// FIXME: don't split r_dest into 2 containers.
LIR* Mips64Mir2Lir::StoreBaseDispBody(RegStorage r_base, int displacement, RegStorage r_src,
                                      OpSize size) {
  LIR *res;
  LIR *store = NULL;
  Mips64OpCode opcode = kMips64Nop;
  bool short_form = IS_SIMM16(displacement);

  switch (size) {
    case k64:
    case kDouble:
      r_src = Check64BitReg(r_src);
      if (!r_src.IsFloat())
        opcode = kMips64Sd;
      else
        opcode = kMips64Fsdc1;
      DCHECK_EQ((displacement & 0x3), 0);
      break;
    case k32:
    case kSingle:
    case kReference:
      opcode = kMips64Sw;
      if (r_src.IsFloat()) {
        opcode = kMips64Fswc1;
        DCHECK(r_src.IsSingle());
      }
      DCHECK_EQ((displacement & 0x3), 0);
      break;
    case kUnsignedHalf:
    case kSignedHalf:
      opcode = kMips64Sh;
      DCHECK_EQ((displacement & 0x1), 0);
      break;
    case kUnsignedByte:
    case kSignedByte:
      opcode = kMips64Sb;
      break;
    default:
      LOG(FATAL) << "Bad case in StoreBaseDispBody";
  }

  if (short_form) {
    store = res = NewLIR3(opcode, r_src.GetReg(), displacement, r_base.GetReg());
  } else {
    RegStorage r_scratch = AllocTemp();
    res = OpRegRegImm(kOpAdd, r_scratch, r_base, displacement);
    store = NewLIR3(opcode, r_src.GetReg(), 0, r_scratch.GetReg());
    FreeTemp(r_scratch);
  }

  if (mem_ref_type_ == ResourceMask::kDalvikReg) {
    DCHECK_EQ(r_base, rs_rMIPS64_SP);
    AnnotateDalvikRegAccess(store, displacement >> 2, false /* is_load */, r_src.Is64Bit());
  }

  return res;
}

LIR* Mips64Mir2Lir::StoreBaseDisp(RegStorage r_base, int displacement, RegStorage r_src,
                                  OpSize size, VolatileKind is_volatile) {
  if (is_volatile == kVolatile) {
    // Ensure that prior accesses become visible to other threads first.
    GenMemBarrier(kAnyStore);
  }

  LIR* store;
  if (UNLIKELY(is_volatile == kVolatile && (size == k64 || size == kDouble) &&
      displacement & 0x7)) {
    // TODO - use lld/scd instructions for Mips64
    // Do atomic 64-bit load.
    store = GenAtomic64Store(r_base, displacement, r_src);
  } else {
    // TODO: base this on target.
    if (size == kWord) {
      size = k64;
    }
    store = StoreBaseDispBody(r_base, displacement, r_src, size);
  }

  if (UNLIKELY(is_volatile == kVolatile)) {
    // Preserve order with respect to any subsequent volatile loads.
    // We need StoreLoad, but that generally requires the most expensive barrier.
    GenMemBarrier(kAnyAny);
  }

  return store;
}

LIR* Mips64Mir2Lir::OpMem(OpKind op, RegStorage r_base, int disp) {
  UNUSED(op, r_base, disp);
  LOG(FATAL) << "Unexpected use of OpMem for MIPS64";
  UNREACHABLE();
}

LIR* Mips64Mir2Lir::OpCondBranch(ConditionCode cc, LIR* target) {
  UNUSED(cc, target);
  LOG(FATAL) << "Unexpected use of OpCondBranch for MIPS64";
  UNREACHABLE();
}

LIR* Mips64Mir2Lir::InvokeTrampoline(OpKind op, RegStorage r_tgt, QuickEntrypointEnum trampoline) {
  UNUSED(trampoline);  // The address of the trampoline is already loaded into r_tgt.
  return OpReg(op, r_tgt);
}

}  // namespace art
