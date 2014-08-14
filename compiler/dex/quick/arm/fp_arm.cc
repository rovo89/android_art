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

#include "arm_lir.h"
#include "codegen_arm.h"
#include "dex/quick/mir_to_lir-inl.h"

namespace art {

void ArmMir2Lir::GenArithOpFloat(Instruction::Code opcode, RegLocation rl_dest,
                                 RegLocation rl_src1, RegLocation rl_src2) {
  int op = kThumbBkpt;
  RegLocation rl_result;

  /*
   * Don't attempt to optimize register usage since these opcodes call out to
   * the handlers.
   */
  switch (opcode) {
    case Instruction::ADD_FLOAT_2ADDR:
    case Instruction::ADD_FLOAT:
      op = kThumb2Vadds;
      break;
    case Instruction::SUB_FLOAT_2ADDR:
    case Instruction::SUB_FLOAT:
      op = kThumb2Vsubs;
      break;
    case Instruction::DIV_FLOAT_2ADDR:
    case Instruction::DIV_FLOAT:
      op = kThumb2Vdivs;
      break;
    case Instruction::MUL_FLOAT_2ADDR:
    case Instruction::MUL_FLOAT:
      op = kThumb2Vmuls;
      break;
    case Instruction::REM_FLOAT_2ADDR:
    case Instruction::REM_FLOAT:
      FlushAllRegs();   // Send everything to home location
      CallRuntimeHelperRegLocationRegLocation(kQuickFmodf, rl_src1, rl_src2, false);
      rl_result = GetReturn(kFPReg);
      StoreValue(rl_dest, rl_result);
      return;
    case Instruction::NEG_FLOAT:
      GenNegFloat(rl_dest, rl_src1);
      return;
    default:
      LOG(FATAL) << "Unexpected opcode: " << opcode;
  }
  rl_src1 = LoadValue(rl_src1, kFPReg);
  rl_src2 = LoadValue(rl_src2, kFPReg);
  rl_result = EvalLoc(rl_dest, kFPReg, true);
  NewLIR3(op, rl_result.reg.GetReg(), rl_src1.reg.GetReg(), rl_src2.reg.GetReg());
  StoreValue(rl_dest, rl_result);
}

void ArmMir2Lir::GenArithOpDouble(Instruction::Code opcode,
                                  RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2) {
  int op = kThumbBkpt;
  RegLocation rl_result;

  switch (opcode) {
    case Instruction::ADD_DOUBLE_2ADDR:
    case Instruction::ADD_DOUBLE:
      op = kThumb2Vaddd;
      break;
    case Instruction::SUB_DOUBLE_2ADDR:
    case Instruction::SUB_DOUBLE:
      op = kThumb2Vsubd;
      break;
    case Instruction::DIV_DOUBLE_2ADDR:
    case Instruction::DIV_DOUBLE:
      op = kThumb2Vdivd;
      break;
    case Instruction::MUL_DOUBLE_2ADDR:
    case Instruction::MUL_DOUBLE:
      op = kThumb2Vmuld;
      break;
    case Instruction::REM_DOUBLE_2ADDR:
    case Instruction::REM_DOUBLE:
      FlushAllRegs();   // Send everything to home location
      CallRuntimeHelperRegLocationRegLocation(kQuickFmod, rl_src1, rl_src2, false);
      rl_result = GetReturnWide(kFPReg);
      StoreValueWide(rl_dest, rl_result);
      return;
    case Instruction::NEG_DOUBLE:
      GenNegDouble(rl_dest, rl_src1);
      return;
    default:
      LOG(FATAL) << "Unexpected opcode: " << opcode;
  }

  rl_src1 = LoadValueWide(rl_src1, kFPReg);
  DCHECK(rl_src1.wide);
  rl_src2 = LoadValueWide(rl_src2, kFPReg);
  DCHECK(rl_src2.wide);
  rl_result = EvalLoc(rl_dest, kFPReg, true);
  DCHECK(rl_dest.wide);
  DCHECK(rl_result.wide);
  NewLIR3(op, rl_result.reg.GetReg(), rl_src1.reg.GetReg(), rl_src2.reg.GetReg());
  StoreValueWide(rl_dest, rl_result);
}

void ArmMir2Lir::GenConversion(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src) {
  int op = kThumbBkpt;
  int src_reg;
  RegLocation rl_result;

  switch (opcode) {
    case Instruction::INT_TO_FLOAT:
      op = kThumb2VcvtIF;
      break;
    case Instruction::FLOAT_TO_INT:
      op = kThumb2VcvtFI;
      break;
    case Instruction::DOUBLE_TO_FLOAT:
      op = kThumb2VcvtDF;
      break;
    case Instruction::FLOAT_TO_DOUBLE:
      op = kThumb2VcvtFd;
      break;
    case Instruction::INT_TO_DOUBLE:
      op = kThumb2VcvtF64S32;
      break;
    case Instruction::DOUBLE_TO_INT:
      op = kThumb2VcvtDI;
      break;
    case Instruction::LONG_TO_DOUBLE: {
      rl_src = LoadValueWide(rl_src, kFPReg);
      RegisterInfo* info = GetRegInfo(rl_src.reg);
      RegStorage src_low = info->FindMatchingView(RegisterInfo::kLowSingleStorageMask)->GetReg();
      DCHECK(src_low.Valid());
      RegStorage src_high = info->FindMatchingView(RegisterInfo::kHighSingleStorageMask)->GetReg();
      DCHECK(src_high.Valid());
      rl_result = EvalLoc(rl_dest, kFPReg, true);
      RegStorage tmp1 = AllocTempDouble();
      RegStorage tmp2 = AllocTempDouble();

      NewLIR2(kThumb2VcvtF64S32, tmp1.GetReg(), src_high.GetReg());
      NewLIR2(kThumb2VcvtF64U32, rl_result.reg.GetReg(), src_low.GetReg());
      LoadConstantWide(tmp2, 0x41f0000000000000LL);
      NewLIR3(kThumb2VmlaF64, rl_result.reg.GetReg(), tmp1.GetReg(), tmp2.GetReg());
      FreeTemp(tmp1);
      FreeTemp(tmp2);
      StoreValueWide(rl_dest, rl_result);
      return;
    }
    case Instruction::FLOAT_TO_LONG:
      GenConversionCall(kQuickF2l, rl_dest, rl_src);
      return;
    case Instruction::LONG_TO_FLOAT: {
      rl_src = LoadValueWide(rl_src, kFPReg);
      RegisterInfo* info = GetRegInfo(rl_src.reg);
      RegStorage src_low = info->FindMatchingView(RegisterInfo::kLowSingleStorageMask)->GetReg();
      DCHECK(src_low.Valid());
      RegStorage src_high = info->FindMatchingView(RegisterInfo::kHighSingleStorageMask)->GetReg();
      DCHECK(src_high.Valid());
      rl_result = EvalLoc(rl_dest, kFPReg, true);
      // Allocate temp registers.
      RegStorage high_val = AllocTempDouble();
      RegStorage low_val = AllocTempDouble();
      RegStorage const_val = AllocTempDouble();
      // Long to double.
      NewLIR2(kThumb2VcvtF64S32, high_val.GetReg(), src_high.GetReg());
      NewLIR2(kThumb2VcvtF64U32, low_val.GetReg(), src_low.GetReg());
      LoadConstantWide(const_val, INT64_C(0x41f0000000000000));
      NewLIR3(kThumb2VmlaF64, low_val.GetReg(), high_val.GetReg(), const_val.GetReg());
      // Double to float.
      NewLIR2(kThumb2VcvtDF, rl_result.reg.GetReg(), low_val.GetReg());
      // Free temp registers.
      FreeTemp(high_val);
      FreeTemp(low_val);
      FreeTemp(const_val);
      // Store result.
      StoreValue(rl_dest, rl_result);
      return;
    }
    case Instruction::DOUBLE_TO_LONG:
      GenConversionCall(kQuickD2l, rl_dest, rl_src);
      return;
    default:
      LOG(FATAL) << "Unexpected opcode: " << opcode;
  }
  if (rl_src.wide) {
    rl_src = LoadValueWide(rl_src, kFPReg);
    src_reg = rl_src.reg.GetReg();
  } else {
    rl_src = LoadValue(rl_src, kFPReg);
    src_reg = rl_src.reg.GetReg();
  }
  if (rl_dest.wide) {
    rl_result = EvalLoc(rl_dest, kFPReg, true);
    NewLIR2(op, rl_result.reg.GetReg(), src_reg);
    StoreValueWide(rl_dest, rl_result);
  } else {
    rl_result = EvalLoc(rl_dest, kFPReg, true);
    NewLIR2(op, rl_result.reg.GetReg(), src_reg);
    StoreValue(rl_dest, rl_result);
  }
}

void ArmMir2Lir::GenFusedFPCmpBranch(BasicBlock* bb, MIR* mir, bool gt_bias,
                                     bool is_double) {
  LIR* target = &block_label_list_[bb->taken];
  RegLocation rl_src1;
  RegLocation rl_src2;
  if (is_double) {
    rl_src1 = mir_graph_->GetSrcWide(mir, 0);
    rl_src2 = mir_graph_->GetSrcWide(mir, 2);
    rl_src1 = LoadValueWide(rl_src1, kFPReg);
    rl_src2 = LoadValueWide(rl_src2, kFPReg);
    NewLIR2(kThumb2Vcmpd, rl_src1.reg.GetReg(), rl_src2.reg.GetReg());
  } else {
    rl_src1 = mir_graph_->GetSrc(mir, 0);
    rl_src2 = mir_graph_->GetSrc(mir, 1);
    rl_src1 = LoadValue(rl_src1, kFPReg);
    rl_src2 = LoadValue(rl_src2, kFPReg);
    NewLIR2(kThumb2Vcmps, rl_src1.reg.GetReg(), rl_src2.reg.GetReg());
  }
  NewLIR0(kThumb2Fmstat);
  ConditionCode ccode = mir->meta.ccode;
  switch (ccode) {
    case kCondEq:
    case kCondNe:
      break;
    case kCondLt:
      if (gt_bias) {
        ccode = kCondMi;
      }
      break;
    case kCondLe:
      if (gt_bias) {
        ccode = kCondLs;
      }
      break;
    case kCondGt:
      if (gt_bias) {
        ccode = kCondHi;
      }
      break;
    case kCondGe:
      if (gt_bias) {
        ccode = kCondUge;
      }
      break;
    default:
      LOG(FATAL) << "Unexpected ccode: " << ccode;
  }
  OpCondBranch(ccode, target);
}


void ArmMir2Lir::GenCmpFP(Instruction::Code opcode, RegLocation rl_dest,
                          RegLocation rl_src1, RegLocation rl_src2) {
  bool is_double = false;
  int default_result = -1;
  RegLocation rl_result;

  switch (opcode) {
    case Instruction::CMPL_FLOAT:
      is_double = false;
      default_result = -1;
      break;
    case Instruction::CMPG_FLOAT:
      is_double = false;
      default_result = 1;
      break;
    case Instruction::CMPL_DOUBLE:
      is_double = true;
      default_result = -1;
      break;
    case Instruction::CMPG_DOUBLE:
      is_double = true;
      default_result = 1;
      break;
    default:
      LOG(FATAL) << "Unexpected opcode: " << opcode;
  }
  if (is_double) {
    rl_src1 = LoadValueWide(rl_src1, kFPReg);
    rl_src2 = LoadValueWide(rl_src2, kFPReg);
    // In case result vreg is also a src vreg, break association to avoid useless copy by EvalLoc()
    ClobberSReg(rl_dest.s_reg_low);
    rl_result = EvalLoc(rl_dest, kCoreReg, true);
    LoadConstant(rl_result.reg, default_result);
    NewLIR2(kThumb2Vcmpd, rl_src1.reg.GetReg(), rl_src2.reg.GetReg());
  } else {
    rl_src1 = LoadValue(rl_src1, kFPReg);
    rl_src2 = LoadValue(rl_src2, kFPReg);
    // In case result vreg is also a srcvreg, break association to avoid useless copy by EvalLoc()
    ClobberSReg(rl_dest.s_reg_low);
    rl_result = EvalLoc(rl_dest, kCoreReg, true);
    LoadConstant(rl_result.reg, default_result);
    NewLIR2(kThumb2Vcmps, rl_src1.reg.GetReg(), rl_src2.reg.GetReg());
  }
  DCHECK(!rl_result.reg.IsFloat());
  NewLIR0(kThumb2Fmstat);

  LIR* it = OpIT((default_result == -1) ? kCondGt : kCondMi, "");
  NewLIR2(kThumb2MovI8M, rl_result.reg.GetReg(),
          ModifiedImmediate(-default_result));  // Must not alter ccodes
  OpEndIT(it);

  it = OpIT(kCondEq, "");
  LoadConstant(rl_result.reg, 0);
  OpEndIT(it);

  StoreValue(rl_dest, rl_result);
}

void ArmMir2Lir::GenNegFloat(RegLocation rl_dest, RegLocation rl_src) {
  RegLocation rl_result;
  rl_src = LoadValue(rl_src, kFPReg);
  rl_result = EvalLoc(rl_dest, kFPReg, true);
  NewLIR2(kThumb2Vnegs, rl_result.reg.GetReg(), rl_src.reg.GetReg());
  StoreValue(rl_dest, rl_result);
}

void ArmMir2Lir::GenNegDouble(RegLocation rl_dest, RegLocation rl_src) {
  RegLocation rl_result;
  rl_src = LoadValueWide(rl_src, kFPReg);
  rl_result = EvalLoc(rl_dest, kFPReg, true);
  NewLIR2(kThumb2Vnegd, rl_result.reg.GetReg(), rl_src.reg.GetReg());
  StoreValueWide(rl_dest, rl_result);
}

static RegisterClass RegClassForAbsFP(RegLocation rl_src, RegLocation rl_dest) {
  // If src is in a core reg or, unlikely, dest has been promoted to a core reg, use core reg.
  if ((rl_src.location == kLocPhysReg && !rl_src.reg.IsFloat()) ||
      (rl_dest.location == kLocPhysReg && !rl_dest.reg.IsFloat())) {
    return kCoreReg;
  }
  // If src is in an fp reg or dest has been promoted to an fp reg, use fp reg.
  if (rl_src.location == kLocPhysReg || rl_dest.location == kLocPhysReg) {
    return kFPReg;
  }
  // With both src and dest in the stack frame we have to perform load+abs+store. Whether this
  // is faster using a core reg or fp reg depends on the particular CPU. Without further
  // investigation and testing we prefer core register. (If the result is subsequently used in
  // another fp operation, the dalvik reg will probably get promoted and that should be handled
  // by the cases above.)
  return kCoreReg;
}

bool ArmMir2Lir::GenInlinedAbsFloat(CallInfo* info) {
  if (info->result.location == kLocInvalid) {
    return true;  // Result is unused: inlining successful, no code generated.
  }
  RegLocation rl_dest = info->result;
  RegLocation rl_src = UpdateLoc(info->args[0]);
  RegisterClass reg_class = RegClassForAbsFP(rl_src, rl_dest);
  rl_src = LoadValue(rl_src, reg_class);
  RegLocation rl_result = EvalLoc(rl_dest, reg_class, true);
  if (reg_class == kFPReg) {
    NewLIR2(kThumb2Vabss, rl_result.reg.GetReg(), rl_src.reg.GetReg());
  } else {
    OpRegRegImm(kOpAnd, rl_result.reg, rl_src.reg, 0x7fffffff);
  }
  StoreValue(rl_dest, rl_result);
  return true;
}

bool ArmMir2Lir::GenInlinedAbsDouble(CallInfo* info) {
  if (info->result.location == kLocInvalid) {
    return true;  // Result is unused: inlining successful, no code generated.
  }
  RegLocation rl_dest = info->result;
  RegLocation rl_src = UpdateLocWide(info->args[0]);
  RegisterClass reg_class = RegClassForAbsFP(rl_src, rl_dest);
  rl_src = LoadValueWide(rl_src, reg_class);
  RegLocation rl_result = EvalLoc(rl_dest, reg_class, true);
  if (reg_class == kFPReg) {
    NewLIR2(kThumb2Vabsd, rl_result.reg.GetReg(), rl_src.reg.GetReg());
  } else if (rl_result.reg.GetLow().GetReg() != rl_src.reg.GetHigh().GetReg()) {
    // No inconvenient overlap.
    OpRegCopy(rl_result.reg.GetLow(), rl_src.reg.GetLow());
    OpRegRegImm(kOpAnd, rl_result.reg.GetHigh(), rl_src.reg.GetHigh(), 0x7fffffff);
  } else {
    // Inconvenient overlap, use a temp register to preserve the high word of the source.
    RegStorage rs_tmp = AllocTemp();
    OpRegCopy(rs_tmp, rl_src.reg.GetHigh());
    OpRegCopy(rl_result.reg.GetLow(), rl_src.reg.GetLow());
    OpRegRegImm(kOpAnd, rl_result.reg.GetHigh(), rs_tmp, 0x7fffffff);
    FreeTemp(rs_tmp);
  }
  StoreValueWide(rl_dest, rl_result);
  return true;
}

bool ArmMir2Lir::GenInlinedSqrt(CallInfo* info) {
  DCHECK_EQ(cu_->instruction_set, kThumb2);
  RegLocation rl_src = info->args[0];
  RegLocation rl_dest = InlineTargetWide(info);  // double place for result
  rl_src = LoadValueWide(rl_src, kFPReg);
  RegLocation rl_result = EvalLoc(rl_dest, kFPReg, true);
  NewLIR2(kThumb2Vsqrtd, rl_result.reg.GetReg(), rl_src.reg.GetReg());
  StoreValueWide(rl_dest, rl_result);
  return true;
}


}  // namespace art
