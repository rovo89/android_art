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

/* This file contains codegen for the Mips ISA */

#include "codegen_mips.h"

#include "base/logging.h"
#include "dex/mir_graph.h"
#include "dex/quick/mir_to_lir-inl.h"
#include "dex/reg_storage_eq.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "mips_lir.h"
#include "mirror/array-inl.h"

namespace art {

/*
 * Compare two 64-bit values
 *    x = y     return  0
 *    x < y     return -1
 *    x > y     return  1
 *
 * Mips32 implementation
 *    slt   t0,  x.hi, y.hi;        # (x.hi < y.hi) ? 1:0
 *    sgt   t1,  x.hi, y.hi;        # (y.hi > x.hi) ? 1:0
 *    subu  res, t0, t1             # res = -1:1:0 for [ < > = ]
 *    bnez  res, finish
 *    sltu  t0, x.lo, y.lo
 *    sgtu  r1, x.lo, y.lo
 *    subu  res, t0, t1
 * finish:
 *
 * Mips64 implementation
 *    slt   temp, x, y;             # (x < y) ? 1:0
 *    slt   res, y, x;              # (x > y) ? 1:0
 *    subu  res, res, temp;         # res = -1:1:0 for [ < > = ]
 *
 */
void MipsMir2Lir::GenCmpLong(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2) {
  rl_src1 = LoadValueWide(rl_src1, kCoreReg);
  rl_src2 = LoadValueWide(rl_src2, kCoreReg);
  if (cu_->target64) {
    RegStorage temp = AllocTempWide();
    RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
    NewLIR3(kMipsSlt, temp.GetReg(), rl_src1.reg.GetReg(), rl_src2.reg.GetReg());
    NewLIR3(kMipsSlt, rl_result.reg.GetReg(), rl_src2.reg.GetReg(), rl_src1.reg.GetReg());
    NewLIR3(kMipsSubu, rl_result.reg.GetReg(), rl_result.reg.GetReg(), temp.GetReg());
    FreeTemp(temp);
    StoreValue(rl_dest, rl_result);
  } else {
    RegStorage t0 = AllocTemp();
    RegStorage t1 = AllocTemp();
    RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
    NewLIR3(kMipsSlt, t0.GetReg(), rl_src1.reg.GetHighReg(), rl_src2.reg.GetHighReg());
    NewLIR3(kMipsSlt, t1.GetReg(), rl_src2.reg.GetHighReg(), rl_src1.reg.GetHighReg());
    NewLIR3(kMipsSubu, rl_result.reg.GetReg(), t1.GetReg(), t0.GetReg());
    LIR* branch = OpCmpImmBranch(kCondNe, rl_result.reg, 0, nullptr);
    NewLIR3(kMipsSltu, t0.GetReg(), rl_src1.reg.GetLowReg(), rl_src2.reg.GetLowReg());
    NewLIR3(kMipsSltu, t1.GetReg(), rl_src2.reg.GetLowReg(), rl_src1.reg.GetLowReg());
    NewLIR3(kMipsSubu, rl_result.reg.GetReg(), t1.GetReg(), t0.GetReg());
    FreeTemp(t0);
    FreeTemp(t1);
    LIR* target = NewLIR0(kPseudoTargetLabel);
    branch->target = target;
    StoreValue(rl_dest, rl_result);
  }
}

LIR* MipsMir2Lir::OpCmpBranch(ConditionCode cond, RegStorage src1, RegStorage src2, LIR* target) {
  LIR* branch;
  MipsOpCode slt_op;
  MipsOpCode br_op;
  bool cmp_zero = false;
  bool swapped = false;
  switch (cond) {
    case kCondEq:
      br_op = kMipsBeq;
      cmp_zero = true;
      break;
    case kCondNe:
      br_op = kMipsBne;
      cmp_zero = true;
      break;
    case kCondUlt:
      slt_op = kMipsSltu;
      br_op = kMipsBnez;
      break;
    case kCondUge:
      slt_op = kMipsSltu;
      br_op = kMipsBeqz;
      break;
    case kCondGe:
      slt_op = kMipsSlt;
      br_op = kMipsBeqz;
      break;
    case kCondGt:
      slt_op = kMipsSlt;
      br_op = kMipsBnez;
      swapped = true;
      break;
    case kCondLe:
      slt_op = kMipsSlt;
      br_op = kMipsBeqz;
      swapped = true;
      break;
    case kCondLt:
      slt_op = kMipsSlt;
      br_op = kMipsBnez;
      break;
    case kCondHi:  // Gtu
      slt_op = kMipsSltu;
      br_op = kMipsBnez;
      swapped = true;
      break;
    default:
      LOG(FATAL) << "No support for ConditionCode: " << cond;
      return nullptr;
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

LIR* MipsMir2Lir::OpCmpImmBranch(ConditionCode cond, RegStorage reg, int check_value, LIR* target) {
  LIR* branch;
  if (check_value != 0) {
    // TUNING: handle s16 & kCondLt/Mi case using slti.
    RegStorage t_reg = AllocTemp();
    LoadConstant(t_reg, check_value);
    branch = OpCmpBranch(cond, reg, t_reg, target);
    FreeTemp(t_reg);
    return branch;
  }
  MipsOpCode opc;
  switch (cond) {
    case kCondEq: opc = kMipsBeqz; break;
    case kCondGe: opc = kMipsBgez; break;
    case kCondGt: opc = kMipsBgtz; break;
    case kCondLe: opc = kMipsBlez; break;
    // case KCondMi:
    case kCondLt: opc = kMipsBltz; break;
    case kCondNe: opc = kMipsBnez; break;
    default:
      // Tuning: use slti when applicable
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

LIR* MipsMir2Lir::OpRegCopyNoInsert(RegStorage r_dest, RegStorage r_src) {
  LIR* res;
  MipsOpCode opcode;

  if (!cu_->target64) {
    // If src or dest is a pair, we'll be using low reg.
    if (r_dest.IsPair()) {
      r_dest = r_dest.GetLow();
    }
    if (r_src.IsPair()) {
      r_src = r_src.GetLow();
    }
  } else {
    DCHECK(!r_dest.IsPair() && !r_src.IsPair());
  }

  if (r_dest.IsFloat() || r_src.IsFloat())
    return OpFpRegCopy(r_dest, r_src);
  if (cu_->target64) {
    // TODO: Check that r_src and r_dest are both 32 or both 64 bits length on Mips64.
    if (r_dest.Is64Bit() || r_src.Is64Bit()) {
      opcode = kMipsMove;
    } else {
      opcode = kMipsSll;
    }
  } else {
    opcode = kMipsMove;
  }
  res = RawLIR(current_dalvik_offset_, opcode, r_dest.GetReg(), r_src.GetReg());
  if (!(cu_->disable_opt & (1 << kSafeOptimizations)) && r_dest == r_src) {
    res->flags.is_nop = true;
  }
  return res;
}

void MipsMir2Lir::OpRegCopy(RegStorage r_dest, RegStorage r_src) {
  if (r_dest != r_src) {
    LIR *res = OpRegCopyNoInsert(r_dest, r_src);
    AppendLIR(res);
  }
}

void MipsMir2Lir::OpRegCopyWide(RegStorage r_dest, RegStorage r_src) {
  if (cu_->target64) {
    OpRegCopy(r_dest, r_src);
    return;
  }
  if (r_dest != r_src) {
    bool dest_fp = r_dest.IsFloat();
    bool src_fp = r_src.IsFloat();
    if (dest_fp) {
      if (src_fp) {
        // Here if both src and dest are fp registers. OpRegCopy will choose the right copy
        // (solo or pair).
        OpRegCopy(r_dest, r_src);
      } else {
        // note the operands are swapped for the mtc1 and mthc1 instr.
        // Here if dest is fp reg and src is core reg.
        if (fpuIs32Bit_) {
          NewLIR2(kMipsMtc1, r_src.GetLowReg(), r_dest.GetLowReg());
          NewLIR2(kMipsMtc1, r_src.GetHighReg(), r_dest.GetHighReg());
        } else {
          r_dest = Fp64ToSolo32(r_dest);
          NewLIR2(kMipsMtc1, r_src.GetLowReg(), r_dest.GetReg());
          NewLIR2(kMipsMthc1, r_src.GetHighReg(), r_dest.GetReg());
        }
      }
    } else {
      if (src_fp) {
        // Here if dest is core reg and src is fp reg.
        if (fpuIs32Bit_) {
          NewLIR2(kMipsMfc1, r_dest.GetLowReg(), r_src.GetLowReg());
          NewLIR2(kMipsMfc1, r_dest.GetHighReg(), r_src.GetHighReg());
        } else {
          r_src = Fp64ToSolo32(r_src);
          NewLIR2(kMipsMfc1, r_dest.GetLowReg(), r_src.GetReg());
          NewLIR2(kMipsMfhc1, r_dest.GetHighReg(), r_src.GetReg());
        }
      } else {
        // Here if both src and dest are core registers.
        // Handle overlap
        if (r_src.GetHighReg() != r_dest.GetLowReg()) {
          OpRegCopy(r_dest.GetLow(), r_src.GetLow());
          OpRegCopy(r_dest.GetHigh(), r_src.GetHigh());
        } else if (r_src.GetLowReg() != r_dest.GetHighReg()) {
          OpRegCopy(r_dest.GetHigh(), r_src.GetHigh());
          OpRegCopy(r_dest.GetLow(), r_src.GetLow());
        } else {
          RegStorage r_tmp = AllocTemp();
          OpRegCopy(r_tmp, r_src.GetHigh());
          OpRegCopy(r_dest.GetLow(), r_src.GetLow());
          OpRegCopy(r_dest.GetHigh(), r_tmp);
          FreeTemp(r_tmp);
        }
      }
    }
  }
}

void MipsMir2Lir::GenSelectConst32(RegStorage left_op, RegStorage right_op, ConditionCode code,
                                   int32_t true_val, int32_t false_val, RegStorage rs_dest,
                                   RegisterClass dest_reg_class) {
  UNUSED(dest_reg_class);
  // Implement as a branch-over.
  // TODO: Conditional move?
  LoadConstant(rs_dest, true_val);
  LIR* ne_branchover = OpCmpBranch(code, left_op, right_op, nullptr);
  LoadConstant(rs_dest, false_val);
  LIR* target_label = NewLIR0(kPseudoTargetLabel);
  ne_branchover->target = target_label;
}

void MipsMir2Lir::GenSelect(BasicBlock* bb, MIR* mir) {
  UNUSED(bb, mir);
  UNIMPLEMENTED(FATAL) << "Need codegen for select";
}

void MipsMir2Lir::GenFusedLongCmpBranch(BasicBlock* bb, MIR* mir) {
  UNUSED(bb, mir);
  UNIMPLEMENTED(FATAL) << "Need codegen for fused long cmp branch";
}

RegLocation MipsMir2Lir::GenDivRem(RegLocation rl_dest, RegStorage reg1, RegStorage reg2,
                                   bool is_div) {
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);

  if (isaIsR6_) {
    NewLIR3(is_div ? kMipsR6Div : kMipsR6Mod, rl_result.reg.GetReg(), reg1.GetReg(), reg2.GetReg());
  } else {
    NewLIR2(kMipsR2Div, reg1.GetReg(), reg2.GetReg());
    NewLIR1(is_div ? kMipsR2Mflo : kMipsR2Mfhi, rl_result.reg.GetReg());
  }
  return rl_result;
}

RegLocation MipsMir2Lir::GenDivRemLit(RegLocation rl_dest, RegStorage reg1, int lit, bool is_div) {
  RegStorage t_reg = AllocTemp();
  // lit is guarantee to be a 16-bit constant
  if (IsUint<16>(lit)) {
    NewLIR3(kMipsOri, t_reg.GetReg(), rZERO, lit);
  } else {
    // Addiu will sign extend the entire width (32 or 64) of the register.
    NewLIR3(kMipsAddiu, t_reg.GetReg(), rZERO, lit);
  }
  RegLocation rl_result = GenDivRem(rl_dest, reg1, t_reg, is_div);
  FreeTemp(t_reg);
  return rl_result;
}

RegLocation MipsMir2Lir::GenDivRem(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2,
                                   bool is_div, int flags) {
  UNUSED(rl_dest, rl_src1, rl_src2, is_div, flags);
  LOG(FATAL) << "Unexpected use of GenDivRem for Mips";
  UNREACHABLE();
}

RegLocation MipsMir2Lir::GenDivRemLit(RegLocation rl_dest, RegLocation rl_src1, int lit,
                                      bool is_div) {
  UNUSED(rl_dest, rl_src1, lit, is_div);
  LOG(FATAL) << "Unexpected use of GenDivRemLit for Mips";
  UNREACHABLE();
}

bool MipsMir2Lir::GenInlinedCas(CallInfo* info, bool is_long, bool is_object) {
  UNUSED(info, is_long, is_object);
  return false;
}

bool MipsMir2Lir::GenInlinedAbsFloat(CallInfo* info) {
  UNUSED(info);
  // TODO: add Mips implementation.
  return false;
}

bool MipsMir2Lir::GenInlinedAbsDouble(CallInfo* info) {
  UNUSED(info);
  // TODO: add Mips implementation.
  return false;
}

bool MipsMir2Lir::GenInlinedSqrt(CallInfo* info) {
  UNUSED(info);
  return false;
}

bool MipsMir2Lir::GenInlinedPeek(CallInfo* info, OpSize size) {
  if (size != kSignedByte) {
    // MIPS supports only aligned access. Defer unaligned access to JNI implementation.
    return false;
  }
  RegLocation rl_src_address = info->args[0];       // Long address.
  if (!cu_->target64) {
    rl_src_address = NarrowRegLoc(rl_src_address);  // Ignore high half in info->args[1].
  }
  RegLocation rl_dest = InlineTarget(info);
  RegLocation rl_address;
  if (cu_->target64) {
    rl_address = LoadValueWide(rl_src_address, kCoreReg);
  } else {
    rl_address = LoadValue(rl_src_address, kCoreReg);
  }
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  DCHECK(size == kSignedByte);
  LoadBaseDisp(rl_address.reg, 0, rl_result.reg, size, kNotVolatile);
  StoreValue(rl_dest, rl_result);
  return true;
}

bool MipsMir2Lir::GenInlinedPoke(CallInfo* info, OpSize size) {
  if (size != kSignedByte) {
    // MIPS supports only aligned access. Defer unaligned access to JNI implementation.
    return false;
  }
  RegLocation rl_src_address = info->args[0];       // Long address.
  if (!cu_->target64) {
    rl_src_address = NarrowRegLoc(rl_src_address);  // Ignore high half in info->args[1].
  }
  RegLocation rl_src_value = info->args[2];         // [size] value.
  RegLocation rl_address;
  if (cu_->target64) {
    rl_address = LoadValueWide(rl_src_address, kCoreReg);
  } else {
    rl_address = LoadValue(rl_src_address, kCoreReg);
  }
  DCHECK(size == kSignedByte);
  RegLocation rl_value = LoadValue(rl_src_value, kCoreReg);
  StoreBaseDisp(rl_address.reg, 0, rl_value.reg, size, kNotVolatile);
  return true;
}

void MipsMir2Lir::OpPcRelLoad(RegStorage reg, LIR* target) {
  UNUSED(reg, target);
  LOG(FATAL) << "Unexpected use of OpPcRelLoad for Mips";
  UNREACHABLE();
}

LIR* MipsMir2Lir::OpVldm(RegStorage r_base, int count) {
  UNUSED(r_base, count);
  LOG(FATAL) << "Unexpected use of OpVldm for Mips";
  UNREACHABLE();
}

LIR* MipsMir2Lir::OpVstm(RegStorage r_base, int count) {
  UNUSED(r_base, count);
  LOG(FATAL) << "Unexpected use of OpVstm for Mips";
  UNREACHABLE();
}

void MipsMir2Lir::GenMultiplyByTwoBitMultiplier(RegLocation rl_src, RegLocation rl_result, int lit,
                                                int first_bit, int second_bit) {
  UNUSED(lit);
  RegStorage t_reg = AllocTemp();
  OpRegRegImm(kOpLsl, t_reg, rl_src.reg, second_bit - first_bit);
  OpRegRegReg(kOpAdd, rl_result.reg, rl_src.reg, t_reg);
  FreeTemp(t_reg);
  if (first_bit != 0) {
    OpRegRegImm(kOpLsl, rl_result.reg, rl_result.reg, first_bit);
  }
}

void MipsMir2Lir::GenDivZeroCheckWide(RegStorage reg) {
  if (cu_->target64) {
    GenDivZeroCheck(reg);
  } else {
    DCHECK(reg.IsPair());   // TODO: support k64BitSolo.
    RegStorage t_reg = AllocTemp();
    OpRegRegReg(kOpOr, t_reg, reg.GetLow(), reg.GetHigh());
    GenDivZeroCheck(t_reg);
    FreeTemp(t_reg);
  }
}

// Test suspend flag, return target of taken suspend branch.
LIR* MipsMir2Lir::OpTestSuspend(LIR* target) {
  OpRegImm(kOpSub, TargetPtrReg(kSuspend), 1);
  return OpCmpImmBranch((target == nullptr) ? kCondEq : kCondNe, TargetPtrReg(kSuspend), 0, target);
}

// Decrement register and branch on condition.
LIR* MipsMir2Lir::OpDecAndBranch(ConditionCode c_code, RegStorage reg, LIR* target) {
  OpRegImm(kOpSub, reg, 1);
  return OpCmpImmBranch(c_code, reg, 0, target);
}

bool MipsMir2Lir::SmallLiteralDivRem(Instruction::Code dalvik_opcode, bool is_div,
                                     RegLocation rl_src, RegLocation rl_dest, int lit) {
  UNUSED(dalvik_opcode, is_div, rl_src, rl_dest, lit);
  LOG(FATAL) << "Unexpected use of smallLiteralDive in Mips";
  UNREACHABLE();
}

bool MipsMir2Lir::EasyMultiply(RegLocation rl_src, RegLocation rl_dest, int lit) {
  UNUSED(rl_src, rl_dest, lit);
  LOG(FATAL) << "Unexpected use of easyMultiply in Mips";
  UNREACHABLE();
}

LIR* MipsMir2Lir::OpIT(ConditionCode cond, const char* guide) {
  UNUSED(cond, guide);
  LOG(FATAL) << "Unexpected use of OpIT in Mips";
  UNREACHABLE();
}

void MipsMir2Lir::OpEndIT(LIR* it) {
  UNUSED(it);
  LOG(FATAL) << "Unexpected use of OpEndIT in Mips";
}

void MipsMir2Lir::GenAddLong(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2) {
  rl_src1 = LoadValueWide(rl_src1, kCoreReg);
  rl_src2 = LoadValueWide(rl_src2, kCoreReg);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  /*
   *  [v1 v0] =  [a1 a0] + [a3 a2];
   *  addu v0,a2,a0
   *  addu t1,a3,a1
   *  sltu v1,v0,a2
   *  addu v1,v1,t1
   */

  OpRegRegReg(kOpAdd, rl_result.reg.GetLow(), rl_src2.reg.GetLow(), rl_src1.reg.GetLow());
  RegStorage t_reg = AllocTemp();
  OpRegRegReg(kOpAdd, t_reg, rl_src2.reg.GetHigh(), rl_src1.reg.GetHigh());
  NewLIR3(kMipsSltu, rl_result.reg.GetHighReg(), rl_result.reg.GetLowReg(),
          rl_src2.reg.GetLowReg());
  OpRegRegReg(kOpAdd, rl_result.reg.GetHigh(), rl_result.reg.GetHigh(), t_reg);
  FreeTemp(t_reg);
  StoreValueWide(rl_dest, rl_result);
}

void MipsMir2Lir::GenSubLong(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2) {
  rl_src1 = LoadValueWide(rl_src1, kCoreReg);
  rl_src2 = LoadValueWide(rl_src2, kCoreReg);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  /*
   *  [v1 v0] =  [a1 a0] - [a3 a2];
   *  sltu  t1,a0,a2
   *  subu  v0,a0,a2
   *  subu  v1,a1,a3
   *  subu  v1,v1,t1
   */

  RegStorage t_reg = AllocTemp();
  NewLIR3(kMipsSltu, t_reg.GetReg(), rl_src1.reg.GetLowReg(), rl_src2.reg.GetLowReg());
  OpRegRegReg(kOpSub, rl_result.reg.GetLow(), rl_src1.reg.GetLow(), rl_src2.reg.GetLow());
  OpRegRegReg(kOpSub, rl_result.reg.GetHigh(), rl_src1.reg.GetHigh(), rl_src2.reg.GetHigh());
  OpRegRegReg(kOpSub, rl_result.reg.GetHigh(), rl_result.reg.GetHigh(), t_reg);
  FreeTemp(t_reg);
  StoreValueWide(rl_dest, rl_result);
}

void MipsMir2Lir::GenArithOpLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                                 RegLocation rl_src2, int flags) {
  if (cu_->target64) {
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
  } else {
    switch (opcode) {
      case Instruction::ADD_LONG:
      case Instruction::ADD_LONG_2ADDR:
        GenAddLong(rl_dest, rl_src1, rl_src2);
        return;
      case Instruction::SUB_LONG:
      case Instruction::SUB_LONG_2ADDR:
        GenSubLong(rl_dest, rl_src1, rl_src2);
        return;
      case Instruction::NEG_LONG:
        GenNegLong(rl_dest, rl_src2);
        return;
      default:
        break;
    }
    // Fallback for all other ops.
    Mir2Lir::GenArithOpLong(opcode, rl_dest, rl_src1, rl_src2, flags);
  }
}

void MipsMir2Lir::GenLongOp(OpKind op, RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2) {
  rl_src1 = LoadValueWide(rl_src1, kCoreReg);
  rl_src2 = LoadValueWide(rl_src2, kCoreReg);
  RegLocation rl_result = EvalLocWide(rl_dest, kCoreReg, true);
  OpRegRegReg(op, rl_result.reg, rl_src1.reg, rl_src2.reg);
  StoreValueWide(rl_dest, rl_result);
}

void MipsMir2Lir::GenNotLong(RegLocation rl_dest, RegLocation rl_src) {
  rl_src = LoadValueWide(rl_src, kCoreReg);
  RegLocation rl_result = EvalLocWide(rl_dest, kCoreReg, true);
  OpRegReg(kOpMvn, rl_result.reg, rl_src.reg);
  StoreValueWide(rl_dest, rl_result);
}

void MipsMir2Lir::GenMulLong(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2) {
  rl_src1 = LoadValueWide(rl_src1, kCoreReg);
  rl_src2 = LoadValueWide(rl_src2, kCoreReg);
  RegLocation rl_result = EvalLocWide(rl_dest, kCoreReg, true);
  NewLIR3(kMips64Dmul, rl_result.reg.GetReg(), rl_src1.reg.GetReg(), rl_src2.reg.GetReg());
  StoreValueWide(rl_dest, rl_result);
}

void MipsMir2Lir::GenDivRemLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                                RegLocation rl_src2, bool is_div, int flags) {
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

void MipsMir2Lir::GenNegLong(RegLocation rl_dest, RegLocation rl_src) {
  rl_src = LoadValueWide(rl_src, kCoreReg);
  RegLocation rl_result;

  if (cu_->target64) {
    rl_result = EvalLocWide(rl_dest, kCoreReg, true);
    OpRegReg(kOpNeg, rl_result.reg, rl_src.reg);
    StoreValueWide(rl_dest, rl_result);
  } else {
    rl_result = EvalLoc(rl_dest, kCoreReg, true);
    //  [v1 v0] =  -[a1 a0]
    //  negu  v0,a0
    //  negu  v1,a1
    //  sltu  t1,r_zero
    //  subu  v1,v1,t1
    OpRegReg(kOpNeg, rl_result.reg.GetLow(), rl_src.reg.GetLow());
    OpRegReg(kOpNeg, rl_result.reg.GetHigh(), rl_src.reg.GetHigh());
    RegStorage t_reg = AllocTemp();
    NewLIR3(kMipsSltu, t_reg.GetReg(), rZERO, rl_result.reg.GetLowReg());
    OpRegRegReg(kOpSub, rl_result.reg.GetHigh(), rl_result.reg.GetHigh(), t_reg);
    FreeTemp(t_reg);
    StoreValueWide(rl_dest, rl_result);
  }
}

/*
 * Generate array load
 */
void MipsMir2Lir::GenArrayGet(int opt_flags, OpSize size, RegLocation rl_array,
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

  RegStorage reg_ptr = (cu_->target64) ? AllocTempRef() : AllocTemp();
  bool needs_range_check = (!(opt_flags & MIR_IGNORE_RANGE_CHECK));
  RegStorage reg_len;
  if (needs_range_check) {
    reg_len = AllocTemp();
    // Get len.
    Load32Disp(rl_array.reg, len_offset, reg_len);
    MarkPossibleNullPointerException(opt_flags);
  } else {
    ForceImplicitNullCheck(rl_array.reg, opt_flags, false);
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

    if (cu_->target64) {
      if (rl_result.ref) {
        LoadBaseIndexed(reg_ptr, As64BitReg(rl_index.reg), As32BitReg(rl_result.reg), scale,
                        kReference);
      } else {
        LoadBaseIndexed(reg_ptr, As64BitReg(rl_index.reg), rl_result.reg, scale, size);
      }
    } else {
      LoadBaseIndexed(reg_ptr, rl_index.reg, rl_result.reg, scale, size);
    }

    FreeTemp(reg_ptr);
    StoreValue(rl_dest, rl_result);
  }
}

/*
 * Generate array store
 *
 */
void MipsMir2Lir::GenArrayPut(int opt_flags, OpSize size, RegLocation rl_array,
                              RegLocation rl_index, RegLocation rl_src, int scale, bool card_mark) {
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
    MarkPossibleNullPointerException(opt_flags);
  } else {
    ForceImplicitNullCheck(rl_array.reg, opt_flags, false);
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

void MipsMir2Lir::GenShiftOpLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                                 RegLocation rl_shift) {
  if (!cu_->target64) {
    Mir2Lir::GenShiftOpLong(opcode, rl_dest, rl_src1, rl_shift);
    return;
  }
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

void MipsMir2Lir::GenShiftImmOpLong(Instruction::Code opcode, RegLocation rl_dest,
                                    RegLocation rl_src1, RegLocation rl_shift, int flags) {
  UNUSED(flags);
  if (!cu_->target64) {
    // Default implementation is just to ignore the constant case.
    GenShiftOpLong(opcode, rl_dest, rl_src1, rl_shift);
    return;
  }
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

void MipsMir2Lir::GenArithImmOpLong(Instruction::Code opcode, RegLocation rl_dest,
                                    RegLocation rl_src1, RegLocation rl_src2, int flags) {
  // Default - bail to non-const handler.
  GenArithOpLong(opcode, rl_dest, rl_src1, rl_src2, flags);
}

void MipsMir2Lir::GenIntToLong(RegLocation rl_dest, RegLocation rl_src) {
  if (!cu_->target64) {
    Mir2Lir::GenIntToLong(rl_dest, rl_src);
    return;
  }
  rl_src = LoadValue(rl_src, kCoreReg);
  RegLocation rl_result = EvalLocWide(rl_dest, kCoreReg, true);
  NewLIR3(kMipsSll, rl_result.reg.GetReg(), As64BitReg(rl_src.reg).GetReg(), 0);
  StoreValueWide(rl_dest, rl_result);
}

void MipsMir2Lir::GenConversionCall(QuickEntrypointEnum trampoline, RegLocation rl_dest,
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
