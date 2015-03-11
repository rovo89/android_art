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

/* This file contains codegen for the Mips64 ISA */

#include "codegen_mips64.h"

#include "base/logging.h"
#include "dex/mir_graph.h"
#include "dex/quick/mir_to_lir-inl.h"
#include "dex/reg_storage_eq.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "mips64_lir.h"
#include "mirror/array-inl.h"

namespace art {

/*
 * Compare two 64-bit values
 *    x = y     return  0
 *    x < y     return -1
 *    x > y     return  1
 *
 *    slt   temp, x, y;          # (x < y) ? 1:0
 *    slt   res, y, x;           # (x > y) ? 1:0
 *    subu  res, res, temp;      # res = -1:1:0 for [ < > = ]
 *
 */
void Mips64Mir2Lir::GenCmpLong(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2) {
  rl_src1 = LoadValueWide(rl_src1, kCoreReg);
  rl_src2 = LoadValueWide(rl_src2, kCoreReg);
  RegStorage temp = AllocTempWide();
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  NewLIR3(kMips64Slt, temp.GetReg(), rl_src1.reg.GetReg(), rl_src2.reg.GetReg());
  NewLIR3(kMips64Slt, rl_result.reg.GetReg(), rl_src2.reg.GetReg(), rl_src1.reg.GetReg());
  NewLIR3(kMips64Subu, rl_result.reg.GetReg(), rl_result.reg.GetReg(), temp.GetReg());
  FreeTemp(temp);
  StoreValue(rl_dest, rl_result);
}

LIR* Mips64Mir2Lir::OpCmpBranch(ConditionCode cond, RegStorage src1, RegStorage src2, LIR* target) {
  LIR* branch;
  Mips64OpCode slt_op;
  Mips64OpCode br_op;
  bool cmp_zero = false;
  bool swapped = false;
  switch (cond) {
    case kCondEq:
      br_op = kMips64Beq;
      cmp_zero = true;
      break;
    case kCondNe:
      br_op = kMips64Bne;
      cmp_zero = true;
      break;
    case kCondUlt:
      slt_op = kMips64Sltu;
      br_op = kMips64Bnez;
      break;
    case kCondUge:
      slt_op = kMips64Sltu;
      br_op = kMips64Beqz;
      break;
    case kCondGe:
      slt_op = kMips64Slt;
      br_op = kMips64Beqz;
      break;
    case kCondGt:
      slt_op = kMips64Slt;
      br_op = kMips64Bnez;
      swapped = true;
      break;
    case kCondLe:
      slt_op = kMips64Slt;
      br_op = kMips64Beqz;
      swapped = true;
      break;
    case kCondLt:
      slt_op = kMips64Slt;
      br_op = kMips64Bnez;
      break;
    case kCondHi:  // Gtu
      slt_op = kMips64Sltu;
      br_op = kMips64Bnez;
      swapped = true;
      break;
    default:
      LOG(FATAL) << "No support for ConditionCode: " << cond;
      return NULL;
  }
  if (cmp_zero) {
    branch = NewLIR2(br_op, src1.GetReg(), src2.GetReg());
  } else {
    RegStorage t_reg = AllocTemp();
    if (swapped) {
      NewLIR3(slt_op, t_reg.GetReg(), src2.GetReg(), src1.GetReg());
    } else {
      NewLIR3(slt_op, t_reg.GetReg(), src1.GetReg(), src2.GetReg());
    }
    branch = NewLIR1(br_op, t_reg.GetReg());
    FreeTemp(t_reg);
  }
  branch->target = target;
  return branch;
}

LIR* Mips64Mir2Lir::OpCmpImmBranch(ConditionCode cond, RegStorage reg,
                                   int check_value, LIR* target) {
  LIR* branch;
  if (check_value != 0) {
    // TUNING: handle s16 & kCondLt/Mi case using slti.
    RegStorage t_reg = AllocTemp();
    LoadConstant(t_reg, check_value);
    branch = OpCmpBranch(cond, reg, t_reg, target);
    FreeTemp(t_reg);
    return branch;
  }
  Mips64OpCode opc;
  switch (cond) {
    case kCondEq: opc = kMips64Beqz; break;
    case kCondGe: opc = kMips64Bgez; break;
    case kCondGt: opc = kMips64Bgtz; break;
    case kCondLe: opc = kMips64Blez; break;
    // case KCondMi:
    case kCondLt: opc = kMips64Bltz; break;
    case kCondNe: opc = kMips64Bnez; break;
    default:
      // Tuning: use slti when applicable.
      RegStorage t_reg = AllocTemp();
      LoadConstant(t_reg, check_value);
      branch = OpCmpBranch(cond, reg, t_reg, target);
      FreeTemp(t_reg);
      return branch;
  }
  branch = NewLIR1(opc, reg.GetReg());
  branch->target = target;
  return branch;
}

LIR* Mips64Mir2Lir::OpRegCopyNoInsert(RegStorage r_dest, RegStorage r_src) {
  DCHECK(!r_dest.IsPair() && !r_src.IsPair());
  if (r_dest.IsFloat() || r_src.IsFloat())
    return OpFpRegCopy(r_dest, r_src);
  // TODO: Check that r_src and r_dest are both 32 or both 64 bits length.
  LIR* res;
  if (r_dest.Is64Bit() || r_src.Is64Bit()) {
    res = RawLIR(current_dalvik_offset_, kMips64Move, r_dest.GetReg(), r_src.GetReg());
  } else {
    res = RawLIR(current_dalvik_offset_, kMips64Sll, r_dest.GetReg(), r_src.GetReg(), 0);
  }
  if (!(cu_->disable_opt & (1 << kSafeOptimizations)) && r_dest == r_src) {
    res->flags.is_nop = true;
  }
  return res;
}

void Mips64Mir2Lir::OpRegCopy(RegStorage r_dest, RegStorage r_src) {
  if (r_dest != r_src) {
    LIR *res = OpRegCopyNoInsert(r_dest, r_src);
    AppendLIR(res);
  }
}

void Mips64Mir2Lir::OpRegCopyWide(RegStorage r_dest, RegStorage r_src) {
  OpRegCopy(r_dest, r_src);
}

void Mips64Mir2Lir::GenSelectConst32(RegStorage left_op, RegStorage right_op, ConditionCode code,
                                     int32_t true_val, int32_t false_val, RegStorage rs_dest,
                                     RegisterClass dest_reg_class) {
  UNUSED(dest_reg_class);
  // Implement as a branch-over.
  // TODO: Conditional move?
  LoadConstant(rs_dest, true_val);
  LIR* ne_branchover = OpCmpBranch(code, left_op, right_op, NULL);
  LoadConstant(rs_dest, false_val);
  LIR* target_label = NewLIR0(kPseudoTargetLabel);
  ne_branchover->target = target_label;
}

void Mips64Mir2Lir::GenSelect(BasicBlock* bb, MIR* mir) {
  UNUSED(bb, mir);
  UNIMPLEMENTED(FATAL) << "Need codegen for select";
}

void Mips64Mir2Lir::GenFusedLongCmpBranch(BasicBlock* bb, MIR* mir) {
  UNUSED(bb, mir);
  UNIMPLEMENTED(FATAL) << "Need codegen for fused long cmp branch";
}

RegLocation Mips64Mir2Lir::GenDivRem(RegLocation rl_dest, RegStorage reg1, RegStorage reg2,
                                     bool is_div) {
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  NewLIR3(is_div ? kMips64Div : kMips64Mod, rl_result.reg.GetReg(), reg1.GetReg(), reg2.GetReg());
  return rl_result;
}

RegLocation Mips64Mir2Lir::GenDivRemLit(RegLocation rl_dest, RegStorage reg1, int lit,
                                        bool is_div) {
  RegStorage t_reg = AllocTemp();
  NewLIR3(kMips64Addiu, t_reg.GetReg(), rZERO, lit);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  NewLIR3(is_div ? kMips64Div : kMips64Mod, rl_result.reg.GetReg(), reg1.GetReg(), t_reg.GetReg());
  FreeTemp(t_reg);
  return rl_result;
}

RegLocation Mips64Mir2Lir::GenDivRem(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2,
                                     bool is_div, int flags) {
  UNUSED(rl_dest, rl_src1, rl_src2, is_div, flags);
  LOG(FATAL) << "Unexpected use of GenDivRem for Mips64";
  UNREACHABLE();
}

RegLocation Mips64Mir2Lir::GenDivRemLit(RegLocation rl_dest, RegLocation rl_src1, int lit,
                                        bool is_div) {
  UNUSED(rl_dest, rl_src1, lit, is_div);
  LOG(FATAL) << "Unexpected use of GenDivRemLit for Mips64";
  UNREACHABLE();
}

bool Mips64Mir2Lir::GenInlinedCas(CallInfo* info, bool is_long, bool is_object) {
  UNUSED(info, is_long, is_object);
  return false;
}

bool Mips64Mir2Lir::GenInlinedAbsFloat(CallInfo* info) {
  UNUSED(info);
  // TODO: add Mips64 implementation.
  return false;
}

bool Mips64Mir2Lir::GenInlinedAbsDouble(CallInfo* info) {
  UNUSED(info);
  // TODO: add Mips64 implementation.
  return false;
}

bool Mips64Mir2Lir::GenInlinedSqrt(CallInfo* info) {
  UNUSED(info);
  return false;
}

bool Mips64Mir2Lir::GenInlinedPeek(CallInfo* info, OpSize size) {
  if (size != kSignedByte) {
    // MIPS64 supports only aligned access. Defer unaligned access to JNI implementation.
    return false;
  }
  RegLocation rl_src_address = info->args[0];     // Long address.
  RegLocation rl_dest = InlineTarget(info);
  RegLocation rl_address = LoadValueWide(rl_src_address, kCoreReg);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  DCHECK(size == kSignedByte);
  LoadBaseDisp(rl_address.reg, 0, rl_result.reg, size, kNotVolatile);
  StoreValue(rl_dest, rl_result);
  return true;
}

bool Mips64Mir2Lir::GenInlinedPoke(CallInfo* info, OpSize size) {
  if (size != kSignedByte) {
    // MIPS64 supports only aligned access. Defer unaligned access to JNI implementation.
    return false;
  }
  RegLocation rl_src_address = info->args[0];     // Long address.
  RegLocation rl_src_value = info->args[2];       // [size] value.
  RegLocation rl_address = LoadValueWide(rl_src_address, kCoreReg);
  DCHECK(size == kSignedByte);
  RegLocation rl_value = LoadValue(rl_src_value, kCoreReg);
  StoreBaseDisp(rl_address.reg, 0, rl_value.reg, size, kNotVolatile);
  return true;
}

LIR* Mips64Mir2Lir::OpPcRelLoad(RegStorage reg, LIR* target) {
  UNUSED(reg, target);
  LOG(FATAL) << "Unexpected use of OpPcRelLoad for Mips64";
  UNREACHABLE();
}

LIR* Mips64Mir2Lir::OpVldm(RegStorage r_base, int count) {
  UNUSED(r_base, count);
  LOG(FATAL) << "Unexpected use of OpVldm for Mips64";
  UNREACHABLE();
}

LIR* Mips64Mir2Lir::OpVstm(RegStorage r_base, int count) {
  UNUSED(r_base, count);
  LOG(FATAL) << "Unexpected use of OpVstm for Mips64";
  UNREACHABLE();
}

void Mips64Mir2Lir::GenMultiplyByTwoBitMultiplier(RegLocation rl_src, RegLocation rl_result,
                                                  int lit, int first_bit, int second_bit) {
  UNUSED(lit);
  RegStorage t_reg = AllocTemp();
  OpRegRegImm(kOpLsl, t_reg, rl_src.reg, second_bit - first_bit);
  OpRegRegReg(kOpAdd, rl_result.reg, rl_src.reg, t_reg);
  FreeTemp(t_reg);
  if (first_bit != 0) {
    OpRegRegImm(kOpLsl, rl_result.reg, rl_result.reg, first_bit);
  }
}

void Mips64Mir2Lir::GenDivZeroCheckWide(RegStorage reg) {
  GenDivZeroCheck(reg);
}

// Test suspend flag, return target of taken suspend branch.
LIR* Mips64Mir2Lir::OpTestSuspend(LIR* target) {
  OpRegImm(kOpSub, rs_rMIPS64_SUSPEND, 1);
  return OpCmpImmBranch((target == NULL) ? kCondEq : kCondNe, rs_rMIPS64_SUSPEND, 0, target);
}

// Decrement register and branch on condition.
LIR* Mips64Mir2Lir::OpDecAndBranch(ConditionCode c_code, RegStorage reg, LIR* target) {
  OpRegImm(kOpSub, reg, 1);
  return OpCmpImmBranch(c_code, reg, 0, target);
}

bool Mips64Mir2Lir::SmallLiteralDivRem(Instruction::Code dalvik_opcode, bool is_div,
                                       RegLocation rl_src, RegLocation rl_dest, int lit) {
  UNUSED(dalvik_opcode, is_div, rl_src, rl_dest, lit);
  LOG(FATAL) << "Unexpected use of smallLiteralDive in Mips64";
  UNREACHABLE();
}

bool Mips64Mir2Lir::EasyMultiply(RegLocation rl_src, RegLocation rl_dest, int lit) {
  UNUSED(rl_src, rl_dest, lit);
  LOG(FATAL) << "Unexpected use of easyMultiply in Mips64";
  UNREACHABLE();
}

LIR* Mips64Mir2Lir::OpIT(ConditionCode cond, const char* guide) {
  UNUSED(cond, guide);
  LOG(FATAL) << "Unexpected use of OpIT in Mips64";
  UNREACHABLE();
}

void Mips64Mir2Lir::OpEndIT(LIR* it) {
  UNUSED(it);
  LOG(FATAL) << "Unexpected use of OpEndIT in Mips64";
}

void Mips64Mir2Lir::GenArithOpLong(Instruction::Code opcode, RegLocation rl_dest,
                                   RegLocation rl_src1, RegLocation rl_src2, int flags) {
  switch (opcode) {
    case Instruction::NOT_LONG:
      GenNotLong(rl_dest, rl_src2);
      return;
    case Instruction::ADD_LONG:
    case Instruction::ADD_LONG_2ADDR:
      GenLongOp(kOpAdd, rl_dest, rl_src1, rl_src2);
      return;
    case Instruction::SUB_LONG:
    case Instruction::SUB_LONG_2ADDR:
      GenLongOp(kOpSub, rl_dest, rl_src1, rl_src2);
      return;
    case Instruction::MUL_LONG:
    case Instruction::MUL_LONG_2ADDR:
      GenMulLong(rl_dest, rl_src1, rl_src2);
      return;
    case Instruction::DIV_LONG:
    case Instruction::DIV_LONG_2ADDR:
      GenDivRemLong(opcode, rl_dest, rl_src1, rl_src2, /*is_div*/ true, flags);
      return;
    case Instruction::REM_LONG:
    case Instruction::REM_LONG_2ADDR:
      GenDivRemLong(opcode, rl_dest, rl_src1, rl_src2, /*is_div*/ false, flags);
      return;
    case Instruction::AND_LONG:
    case Instruction::AND_LONG_2ADDR:
      GenLongOp(kOpAnd, rl_dest, rl_src1, rl_src2);
      return;
    case Instruction::OR_LONG:
    case Instruction::OR_LONG_2ADDR:
      GenLongOp(kOpOr, rl_dest, rl_src1, rl_src2);
      return;
    case Instruction::XOR_LONG:
    case Instruction::XOR_LONG_2ADDR:
      GenLongOp(kOpXor, rl_dest, rl_src1, rl_src2);
      return;
    case Instruction::NEG_LONG:
      GenNegLong(rl_dest, rl_src2);
      return;

    default:
      LOG(FATAL) << "Invalid long arith op";
      return;
  }
}

void Mips64Mir2Lir::GenLongOp(OpKind op, RegLocation rl_dest, RegLocation rl_src1,
                              RegLocation rl_src2) {
  rl_src1 = LoadValueWide(rl_src1, kCoreReg);
  rl_src2 = LoadValueWide(rl_src2, kCoreReg);
  RegLocation rl_result = EvalLocWide(rl_dest, kCoreReg, true);
  OpRegRegReg(op, rl_result.reg, rl_src1.reg, rl_src2.reg);
  StoreValueWide(rl_dest, rl_result);
}

void Mips64Mir2Lir::GenNotLong(RegLocation rl_dest, RegLocation rl_src) {
  rl_src = LoadValueWide(rl_src, kCoreReg);
  RegLocation rl_result = EvalLocWide(rl_dest, kCoreReg, true);
  OpRegReg(kOpMvn, rl_result.reg, rl_src.reg);
  StoreValueWide(rl_dest, rl_result);
}

void Mips64Mir2Lir::GenNegLong(RegLocation rl_dest, RegLocation rl_src) {
  rl_src = LoadValueWide(rl_src, kCoreReg);
  RegLocation rl_result = EvalLocWide(rl_dest, kCoreReg, true);
  OpRegReg(kOpNeg, rl_result.reg, rl_src.reg);
  StoreValueWide(rl_dest, rl_result);
}

void Mips64Mir2Lir::GenMulLong(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2) {
  rl_src1 = LoadValueWide(rl_src1, kCoreReg);
  rl_src2 = LoadValueWide(rl_src2, kCoreReg);
  RegLocation rl_result = EvalLocWide(rl_dest, kCoreReg, true);
  NewLIR3(kMips64Dmul, rl_result.reg.GetReg(), rl_src1.reg.GetReg(), rl_src2.reg.GetReg());
  StoreValueWide(rl_dest, rl_result);
}

void Mips64Mir2Lir::GenDivRemLong(Instruction::Code opcode, RegLocation rl_dest,
                                  RegLocation rl_src1, RegLocation rl_src2, bool is_div,
                                  int flags) {
  UNUSED(opcode);
  // TODO: Implement easy div/rem?
  rl_src1 = LoadValueWide(rl_src1, kCoreReg);
  rl_src2 = LoadValueWide(rl_src2, kCoreReg);
  if ((flags & MIR_IGNORE_DIV_ZERO_CHECK) == 0) {
    GenDivZeroCheckWide(rl_src2.reg);
  }
  RegLocation rl_result = EvalLocWide(rl_dest, kCoreReg, true);
  NewLIR3(is_div ? kMips64Ddiv : kMips64Dmod, rl_result.reg.GetReg(), rl_src1.reg.GetReg(),
          rl_src2.reg.GetReg());
  StoreValueWide(rl_dest, rl_result);
}

/*
 * Generate array load
 */
void Mips64Mir2Lir::GenArrayGet(int opt_flags, OpSize size, RegLocation rl_array,
                                RegLocation rl_index, RegLocation rl_dest, int scale) {
  RegisterClass reg_class = RegClassBySize(size);
  int len_offset = mirror::Array::LengthOffset().Int32Value();
  int data_offset;
  RegLocation rl_result;
  rl_array = LoadValue(rl_array, kRefReg);
  rl_index = LoadValue(rl_index, kCoreReg);

  // FIXME: need to add support for rl_index.is_const.

  if (size == k64 || size == kDouble) {
    data_offset = mirror::Array::DataOffset(sizeof(int64_t)).Int32Value();
  } else {
    data_offset = mirror::Array::DataOffset(sizeof(int32_t)).Int32Value();
  }

  // Null object?
  GenNullCheck(rl_array.reg, opt_flags);

  RegStorage reg_ptr = AllocTempRef();
  bool needs_range_check = (!(opt_flags & MIR_IGNORE_RANGE_CHECK));
  RegStorage reg_len;
  if (needs_range_check) {
    reg_len = AllocTemp();
    // Get len.
    Load32Disp(rl_array.reg, len_offset, reg_len);
  }
  // reg_ptr -> array data.
  OpRegRegImm(kOpAdd, reg_ptr, rl_array.reg, data_offset);
  FreeTemp(rl_array.reg);
  if ((size == k64) || (size == kDouble)) {
    if (scale) {
      RegStorage r_new_index = AllocTemp();
      OpRegRegImm(kOpLsl, r_new_index, rl_index.reg, scale);
      OpRegReg(kOpAdd, reg_ptr, r_new_index);
      FreeTemp(r_new_index);
    } else {
      OpRegReg(kOpAdd, reg_ptr, rl_index.reg);
    }
    FreeTemp(rl_index.reg);
    rl_result = EvalLoc(rl_dest, reg_class, true);

    if (needs_range_check) {
      GenArrayBoundsCheck(rl_index.reg, reg_len);
      FreeTemp(reg_len);
    }
    LoadBaseDisp(reg_ptr, 0, rl_result.reg, size, kNotVolatile);

    FreeTemp(reg_ptr);
    StoreValueWide(rl_dest, rl_result);
  } else {
    rl_result = EvalLoc(rl_dest, reg_class, true);

    if (needs_range_check) {
      GenArrayBoundsCheck(rl_index.reg, reg_len);
      FreeTemp(reg_len);
    }
    if (rl_result.ref) {
      LoadBaseIndexed(reg_ptr, As64BitReg(rl_index.reg), As32BitReg(rl_result.reg), scale,
                      kReference);
    } else {
      LoadBaseIndexed(reg_ptr, As64BitReg(rl_index.reg), rl_result.reg, scale, size);
    }

    FreeTemp(reg_ptr);
    StoreValue(rl_dest, rl_result);
  }
}

/*
 * Generate array store
 *
 */
void Mips64Mir2Lir::GenArrayPut(int opt_flags, OpSize size, RegLocation rl_array,
                                RegLocation rl_index, RegLocation rl_src, int scale,
                                bool card_mark) {
  RegisterClass reg_class = RegClassBySize(size);
  int len_offset = mirror::Array::LengthOffset().Int32Value();
  int data_offset;

  if (size == k64 || size == kDouble) {
    data_offset = mirror::Array::DataOffset(sizeof(int64_t)).Int32Value();
  } else {
    data_offset = mirror::Array::DataOffset(sizeof(int32_t)).Int32Value();
  }

  rl_array = LoadValue(rl_array, kRefReg);
  rl_index = LoadValue(rl_index, kCoreReg);

  // FIXME: need to add support for rl_index.is_const.

  RegStorage reg_ptr;
  bool allocated_reg_ptr_temp = false;
  if (IsTemp(rl_array.reg) && !card_mark) {
    Clobber(rl_array.reg);
    reg_ptr = rl_array.reg;
  } else {
    reg_ptr = AllocTemp();
    OpRegCopy(reg_ptr, rl_array.reg);
    allocated_reg_ptr_temp = true;
  }

  // Null object?
  GenNullCheck(rl_array.reg, opt_flags);

  bool needs_range_check = (!(opt_flags & MIR_IGNORE_RANGE_CHECK));
  RegStorage reg_len;
  if (needs_range_check) {
    reg_len = AllocTemp();
    // NOTE: max live temps(4) here.
    // Get len.
    Load32Disp(rl_array.reg, len_offset, reg_len);
  }
  // reg_ptr -> array data.
  OpRegImm(kOpAdd, reg_ptr, data_offset);
  // At this point, reg_ptr points to array, 2 live temps.
  if ((size == k64) || (size == kDouble)) {
    // TUNING: specific wide routine that can handle fp regs.
    if (scale) {
      RegStorage r_new_index = AllocTemp();
      OpRegRegImm(kOpLsl, r_new_index, rl_index.reg, scale);
      OpRegReg(kOpAdd, reg_ptr, r_new_index);
      FreeTemp(r_new_index);
    } else {
      OpRegReg(kOpAdd, reg_ptr, rl_index.reg);
    }
    rl_src = LoadValueWide(rl_src, reg_class);

    if (needs_range_check) {
      GenArrayBoundsCheck(rl_index.reg, reg_len);
      FreeTemp(reg_len);
    }

    StoreBaseDisp(reg_ptr, 0, rl_src.reg, size, kNotVolatile);
  } else {
    rl_src = LoadValue(rl_src, reg_class);
    if (needs_range_check) {
      GenArrayBoundsCheck(rl_index.reg, reg_len);
      FreeTemp(reg_len);
    }
    StoreBaseIndexed(reg_ptr, rl_index.reg, rl_src.reg, scale, size);
  }
  if (allocated_reg_ptr_temp) {
    FreeTemp(reg_ptr);
  }
  if (card_mark) {
    MarkGCCard(opt_flags, rl_src.reg, rl_array.reg);
  }
}

void Mips64Mir2Lir::GenShiftOpLong(Instruction::Code opcode, RegLocation rl_dest,
                                   RegLocation rl_src1, RegLocation rl_shift) {
  OpKind op = kOpBkpt;
  switch (opcode) {
  case Instruction::SHL_LONG:
  case Instruction::SHL_LONG_2ADDR:
    op = kOpLsl;
    break;
  case Instruction::SHR_LONG:
  case Instruction::SHR_LONG_2ADDR:
    op = kOpAsr;
    break;
  case Instruction::USHR_LONG:
  case Instruction::USHR_LONG_2ADDR:
    op = kOpLsr;
    break;
  default:
    LOG(FATAL) << "Unexpected case: " << opcode;
  }
  rl_shift = LoadValue(rl_shift, kCoreReg);
  rl_src1 = LoadValueWide(rl_src1, kCoreReg);
  RegLocation rl_result = EvalLocWide(rl_dest, kCoreReg, true);
  OpRegRegReg(op, rl_result.reg, rl_src1.reg, As64BitReg(rl_shift.reg));
  StoreValueWide(rl_dest, rl_result);
}

void Mips64Mir2Lir::GenShiftImmOpLong(Instruction::Code opcode, RegLocation rl_dest,
                                      RegLocation rl_src1, RegLocation rl_shift, int flags) {
  UNUSED(flags);
  OpKind op = kOpBkpt;
  // Per spec, we only care about low 6 bits of shift amount.
  int shift_amount = mir_graph_->ConstantValue(rl_shift) & 0x3f;
  rl_src1 = LoadValueWide(rl_src1, kCoreReg);
  if (shift_amount == 0) {
    StoreValueWide(rl_dest, rl_src1);
    return;
  }

  RegLocation rl_result = EvalLocWide(rl_dest, kCoreReg, true);
  switch (opcode) {
    case Instruction::SHL_LONG:
    case Instruction::SHL_LONG_2ADDR:
      op = kOpLsl;
      break;
    case Instruction::SHR_LONG:
    case Instruction::SHR_LONG_2ADDR:
      op = kOpAsr;
      break;
    case Instruction::USHR_LONG:
    case Instruction::USHR_LONG_2ADDR:
      op = kOpLsr;
      break;
    default:
      LOG(FATAL) << "Unexpected case";
  }
  OpRegRegImm(op, rl_result.reg, rl_src1.reg, shift_amount);
  StoreValueWide(rl_dest, rl_result);
}

void Mips64Mir2Lir::GenArithImmOpLong(Instruction::Code opcode, RegLocation rl_dest,
                                      RegLocation rl_src1, RegLocation rl_src2, int flags) {
  // Default - bail to non-const handler.
  GenArithOpLong(opcode, rl_dest, rl_src1, rl_src2, flags);
}

void Mips64Mir2Lir::GenIntToLong(RegLocation rl_dest, RegLocation rl_src) {
  rl_src = LoadValue(rl_src, kCoreReg);
  RegLocation rl_result = EvalLocWide(rl_dest, kCoreReg, true);
  NewLIR3(kMips64Sll, rl_result.reg.GetReg(), As64BitReg(rl_src.reg).GetReg(), 0);
  StoreValueWide(rl_dest, rl_result);
}

void Mips64Mir2Lir::GenConversionCall(QuickEntrypointEnum trampoline, RegLocation rl_dest,
                                      RegLocation rl_src, RegisterClass reg_class) {
  FlushAllRegs();   // Send everything to home location.
  CallRuntimeHelperRegLocation(trampoline, rl_src, false);
  if (rl_dest.wide) {
    RegLocation rl_result;
    rl_result = GetReturnWide(reg_class);
    StoreValueWide(rl_dest, rl_result);
  } else {
    RegLocation rl_result;
    rl_result = GetReturn(reg_class);
    StoreValue(rl_dest, rl_result);
  }
}

}  // namespace art
