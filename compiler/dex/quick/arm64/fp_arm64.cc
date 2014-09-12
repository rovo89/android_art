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

#include "arm64_lir.h"
#include "codegen_arm64.h"
#include "dex/quick/mir_to_lir-inl.h"
#include "utils.h"

namespace art {

void Arm64Mir2Lir::GenArithOpFloat(Instruction::Code opcode, RegLocation rl_dest,
                                   RegLocation rl_src1, RegLocation rl_src2) {
  int op = kA64Brk1d;
  RegLocation rl_result;

  switch (opcode) {
    case Instruction::ADD_FLOAT_2ADDR:
    case Instruction::ADD_FLOAT:
      op = kA64Fadd3fff;
      break;
    case Instruction::SUB_FLOAT_2ADDR:
    case Instruction::SUB_FLOAT:
      op = kA64Fsub3fff;
      break;
    case Instruction::DIV_FLOAT_2ADDR:
    case Instruction::DIV_FLOAT:
      op = kA64Fdiv3fff;
      break;
    case Instruction::MUL_FLOAT_2ADDR:
    case Instruction::MUL_FLOAT:
      op = kA64Fmul3fff;
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

void Arm64Mir2Lir::GenArithOpDouble(Instruction::Code opcode,
                                    RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2) {
  int op = kA64Brk1d;
  RegLocation rl_result;

  switch (opcode) {
    case Instruction::ADD_DOUBLE_2ADDR:
    case Instruction::ADD_DOUBLE:
      op = kA64Fadd3fff;
      break;
    case Instruction::SUB_DOUBLE_2ADDR:
    case Instruction::SUB_DOUBLE:
      op = kA64Fsub3fff;
      break;
    case Instruction::DIV_DOUBLE_2ADDR:
    case Instruction::DIV_DOUBLE:
      op = kA64Fdiv3fff;
      break;
    case Instruction::MUL_DOUBLE_2ADDR:
    case Instruction::MUL_DOUBLE:
      op = kA64Fmul3fff;
      break;
    case Instruction::REM_DOUBLE_2ADDR:
    case Instruction::REM_DOUBLE:
      FlushAllRegs();   // Send everything to home location
      {
        RegStorage r_tgt = CallHelperSetup(kQuickFmod);
        LoadValueDirectWideFixed(rl_src1, rs_d0);
        LoadValueDirectWideFixed(rl_src2, rs_d1);
        ClobberCallerSave();
        CallHelper(r_tgt, kQuickFmod, false);
      }
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
  NewLIR3(FWIDE(op), rl_result.reg.GetReg(), rl_src1.reg.GetReg(), rl_src2.reg.GetReg());
  StoreValueWide(rl_dest, rl_result);
}

void Arm64Mir2Lir::GenConversion(Instruction::Code opcode,
                                 RegLocation rl_dest, RegLocation rl_src) {
  int op = kA64Brk1d;
  RegLocation rl_result;
  RegisterClass src_reg_class = kInvalidRegClass;
  RegisterClass dst_reg_class = kInvalidRegClass;

  switch (opcode) {
    case Instruction::INT_TO_FLOAT:
      op = kA64Scvtf2fw;
      src_reg_class = kCoreReg;
      dst_reg_class = kFPReg;
      break;
    case Instruction::FLOAT_TO_INT:
      op = kA64Fcvtzs2wf;
      src_reg_class = kFPReg;
      dst_reg_class = kCoreReg;
      break;
    case Instruction::DOUBLE_TO_FLOAT:
      op = kA64Fcvt2sS;
      src_reg_class = kFPReg;
      dst_reg_class = kFPReg;
      break;
    case Instruction::FLOAT_TO_DOUBLE:
      op = kA64Fcvt2Ss;
      src_reg_class = kFPReg;
      dst_reg_class = kFPReg;
      break;
    case Instruction::INT_TO_DOUBLE:
      op = FWIDE(kA64Scvtf2fw);
      src_reg_class = kCoreReg;
      dst_reg_class = kFPReg;
      break;
    case Instruction::DOUBLE_TO_INT:
      op = FWIDE(kA64Fcvtzs2wf);
      src_reg_class = kFPReg;
      dst_reg_class = kCoreReg;
      break;
    case Instruction::LONG_TO_DOUBLE:
      op = FWIDE(kA64Scvtf2fx);
      src_reg_class = kCoreReg;
      dst_reg_class = kFPReg;
      break;
    case Instruction::FLOAT_TO_LONG:
      op = kA64Fcvtzs2xf;
      src_reg_class = kFPReg;
      dst_reg_class = kCoreReg;
      break;
    case Instruction::LONG_TO_FLOAT:
      op = kA64Scvtf2fx;
      src_reg_class = kCoreReg;
      dst_reg_class = kFPReg;
      break;
    case Instruction::DOUBLE_TO_LONG:
      op = FWIDE(kA64Fcvtzs2xf);
      src_reg_class = kFPReg;
      dst_reg_class = kCoreReg;
      break;
    default:
      LOG(FATAL) << "Unexpected opcode: " << opcode;
  }

  DCHECK_NE(src_reg_class, kInvalidRegClass);
  DCHECK_NE(dst_reg_class, kInvalidRegClass);
  DCHECK_NE(op, kA64Brk1d);

  if (rl_src.wide) {
    rl_src = LoadValueWide(rl_src, src_reg_class);
  } else {
    rl_src = LoadValue(rl_src, src_reg_class);
  }

  rl_result = EvalLoc(rl_dest, dst_reg_class, true);
  NewLIR2(op, rl_result.reg.GetReg(), rl_src.reg.GetReg());

  if (rl_dest.wide) {
    StoreValueWide(rl_dest, rl_result);
  } else {
    StoreValue(rl_dest, rl_result);
  }
}

void Arm64Mir2Lir::GenFusedFPCmpBranch(BasicBlock* bb, MIR* mir, bool gt_bias,
                                     bool is_double) {
  LIR* target = &block_label_list_[bb->taken];
  RegLocation rl_src1;
  RegLocation rl_src2;
  if (is_double) {
    rl_src1 = mir_graph_->GetSrcWide(mir, 0);
    rl_src2 = mir_graph_->GetSrcWide(mir, 2);
    rl_src1 = LoadValueWide(rl_src1, kFPReg);
    rl_src2 = LoadValueWide(rl_src2, kFPReg);
    NewLIR2(FWIDE(kA64Fcmp2ff), rl_src1.reg.GetReg(), rl_src2.reg.GetReg());
  } else {
    rl_src1 = mir_graph_->GetSrc(mir, 0);
    rl_src2 = mir_graph_->GetSrc(mir, 1);
    rl_src1 = LoadValue(rl_src1, kFPReg);
    rl_src2 = LoadValue(rl_src2, kFPReg);
    NewLIR2(kA64Fcmp2ff, rl_src1.reg.GetReg(), rl_src2.reg.GetReg());
  }
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


void Arm64Mir2Lir::GenCmpFP(Instruction::Code opcode, RegLocation rl_dest,
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
    NewLIR2(FWIDE(kA64Fcmp2ff), rl_src1.reg.GetReg(), rl_src2.reg.GetReg());
  } else {
    rl_src1 = LoadValue(rl_src1, kFPReg);
    rl_src2 = LoadValue(rl_src2, kFPReg);
    // In case result vreg is also a srcvreg, break association to avoid useless copy by EvalLoc()
    ClobberSReg(rl_dest.s_reg_low);
    rl_result = EvalLoc(rl_dest, kCoreReg, true);
    LoadConstant(rl_result.reg, default_result);
    NewLIR2(kA64Fcmp2ff, rl_src1.reg.GetReg(), rl_src2.reg.GetReg());
  }
  DCHECK(!rl_result.reg.IsFloat());

  // TODO(Arm64): should we rather do this?
  // csinc wD, wzr, wzr, eq
  // csneg wD, wD, wD, le
  // (which requires 2 instructions rather than 3)

  // Rd = if cond then Rd else -Rd.
  NewLIR4(kA64Csneg4rrrc, rl_result.reg.GetReg(), rl_result.reg.GetReg(),
          rl_result.reg.GetReg(), (default_result == 1) ? kArmCondPl : kArmCondLe);
  NewLIR4(kA64Csel4rrrc, rl_result.reg.GetReg(), rwzr, rl_result.reg.GetReg(),
          kArmCondEq);
  StoreValue(rl_dest, rl_result);
}

void Arm64Mir2Lir::GenNegFloat(RegLocation rl_dest, RegLocation rl_src) {
  RegLocation rl_result;
  rl_src = LoadValue(rl_src, kFPReg);
  rl_result = EvalLoc(rl_dest, kFPReg, true);
  NewLIR2(kA64Fneg2ff, rl_result.reg.GetReg(), rl_src.reg.GetReg());
  StoreValue(rl_dest, rl_result);
}

void Arm64Mir2Lir::GenNegDouble(RegLocation rl_dest, RegLocation rl_src) {
  RegLocation rl_result;
  rl_src = LoadValueWide(rl_src, kFPReg);
  rl_result = EvalLoc(rl_dest, kFPReg, true);
  NewLIR2(FWIDE(kA64Fneg2ff), rl_result.reg.GetReg(), rl_src.reg.GetReg());
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
  // is faster using a core reg or fp reg depends on the particular CPU. For example, on A53
  // it's faster using core reg while on A57 it's faster with fp reg, the difference being
  // bigger on the A53. Without further investigation and testing we prefer core register.
  // (If the result is subsequently used in another fp operation, the dalvik reg will probably
  // get promoted and that should be handled by the cases above.)
  return kCoreReg;
}

bool Arm64Mir2Lir::GenInlinedAbsFloat(CallInfo* info) {
  if (info->result.location == kLocInvalid) {
    return true;  // Result is unused: inlining successful, no code generated.
  }
  RegLocation rl_dest = info->result;
  RegLocation rl_src = UpdateLoc(info->args[0]);
  RegisterClass reg_class = RegClassForAbsFP(rl_src, rl_dest);
  rl_src = LoadValue(rl_src, reg_class);
  RegLocation rl_result = EvalLoc(rl_dest, reg_class, true);
  if (reg_class == kFPReg) {
    NewLIR2(kA64Fabs2ff, rl_result.reg.GetReg(), rl_src.reg.GetReg());
  } else {
    NewLIR4(kA64Ubfm4rrdd, rl_result.reg.GetReg(), rl_src.reg.GetReg(), 0, 30);
  }
  StoreValue(rl_dest, rl_result);
  return true;
}

bool Arm64Mir2Lir::GenInlinedAbsDouble(CallInfo* info) {
  if (info->result.location == kLocInvalid) {
    return true;  // Result is unused: inlining successful, no code generated.
  }
  RegLocation rl_dest = info->result;
  RegLocation rl_src = UpdateLocWide(info->args[0]);
  RegisterClass reg_class = RegClassForAbsFP(rl_src, rl_dest);
  rl_src = LoadValueWide(rl_src, reg_class);
  RegLocation rl_result = EvalLoc(rl_dest, reg_class, true);
  if (reg_class == kFPReg) {
    NewLIR2(FWIDE(kA64Fabs2ff), rl_result.reg.GetReg(), rl_src.reg.GetReg());
  } else {
    NewLIR4(WIDE(kA64Ubfm4rrdd), rl_result.reg.GetReg(), rl_src.reg.GetReg(), 0, 62);
  }
  StoreValueWide(rl_dest, rl_result);
  return true;
}

bool Arm64Mir2Lir::GenInlinedSqrt(CallInfo* info) {
  RegLocation rl_src = info->args[0];
  RegLocation rl_dest = InlineTargetWide(info);  // double place for result
  rl_src = LoadValueWide(rl_src, kFPReg);
  RegLocation rl_result = EvalLoc(rl_dest, kFPReg, true);
  NewLIR2(FWIDE(kA64Fsqrt2ff), rl_result.reg.GetReg(), rl_src.reg.GetReg());
  StoreValueWide(rl_dest, rl_result);
  return true;
}

bool Arm64Mir2Lir::GenInlinedCeil(CallInfo* info) {
  RegLocation rl_src = info->args[0];
  RegLocation rl_dest = InlineTargetWide(info);
  rl_src = LoadValueWide(rl_src, kFPReg);
  RegLocation rl_result = EvalLoc(rl_dest, kFPReg, true);
  NewLIR2(FWIDE(kA64Frintp2ff), rl_result.reg.GetReg(), rl_src.reg.GetReg());
  StoreValueWide(rl_dest, rl_result);
  return true;
}

bool Arm64Mir2Lir::GenInlinedFloor(CallInfo* info) {
  RegLocation rl_src = info->args[0];
  RegLocation rl_dest = InlineTargetWide(info);
  rl_src = LoadValueWide(rl_src, kFPReg);
  RegLocation rl_result = EvalLoc(rl_dest, kFPReg, true);
  NewLIR2(FWIDE(kA64Frintm2ff), rl_result.reg.GetReg(), rl_src.reg.GetReg());
  StoreValueWide(rl_dest, rl_result);
  return true;
}

bool Arm64Mir2Lir::GenInlinedRint(CallInfo* info) {
  RegLocation rl_src = info->args[0];
  RegLocation rl_dest = InlineTargetWide(info);
  rl_src = LoadValueWide(rl_src, kFPReg);
  RegLocation rl_result = EvalLoc(rl_dest, kFPReg, true);
  NewLIR2(FWIDE(kA64Frintn2ff), rl_result.reg.GetReg(), rl_src.reg.GetReg());
  StoreValueWide(rl_dest, rl_result);
  return true;
}

bool Arm64Mir2Lir::GenInlinedRound(CallInfo* info, bool is_double) {
  int32_t encoded_imm = EncodeImmSingle(bit_cast<float, uint32_t>(0.5f));
  ArmOpcode wide = (is_double) ? FWIDE(0) : FUNWIDE(0);
  RegLocation rl_src = info->args[0];
  RegLocation rl_dest = (is_double) ? InlineTargetWide(info) : InlineTarget(info);
  rl_src = (is_double) ? LoadValueWide(rl_src, kFPReg) : LoadValue(rl_src, kFPReg);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  RegStorage r_imm_point5 = (is_double) ? AllocTempDouble() : AllocTempSingle();
  RegStorage r_tmp = (is_double) ? AllocTempDouble() : AllocTempSingle();
  // 0.5f and 0.5d are encoded in the same way.
  NewLIR2(kA64Fmov2fI | wide, r_imm_point5.GetReg(), encoded_imm);
  NewLIR3(kA64Fadd3fff | wide, r_tmp.GetReg(), rl_src.reg.GetReg(), r_imm_point5.GetReg());
  NewLIR2((is_double) ? kA64Fcvtms2xS : kA64Fcvtms2ws, rl_result.reg.GetReg(), r_tmp.GetReg());
  (is_double) ? StoreValueWide(rl_dest, rl_result) : StoreValue(rl_dest, rl_result);
  return true;
}

bool Arm64Mir2Lir::GenInlinedMinMaxFP(CallInfo* info, bool is_min, bool is_double) {
  DCHECK_EQ(cu_->instruction_set, kArm64);
  int op = (is_min) ? kA64Fmin3fff : kA64Fmax3fff;
  ArmOpcode wide = (is_double) ? FWIDE(0) : FUNWIDE(0);
  RegLocation rl_src1 = info->args[0];
  RegLocation rl_src2 = (is_double) ? info->args[2] : info->args[1];
  rl_src1 = (is_double) ? LoadValueWide(rl_src1, kFPReg) : LoadValue(rl_src1, kFPReg);
  rl_src2 = (is_double) ? LoadValueWide(rl_src2, kFPReg) : LoadValue(rl_src2, kFPReg);
  RegLocation rl_dest = (is_double) ? InlineTargetWide(info) : InlineTarget(info);
  RegLocation rl_result = EvalLoc(rl_dest, kFPReg, true);
  NewLIR3(op | wide, rl_result.reg.GetReg(), rl_src1.reg.GetReg(), rl_src2.reg.GetReg());
  (is_double) ?  StoreValueWide(rl_dest, rl_result) : StoreValue(rl_dest, rl_result);
  return true;
}

}  // namespace art
