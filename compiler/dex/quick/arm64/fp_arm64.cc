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

namespace art {

void Arm64Mir2Lir::GenArithOpFloat(Instruction::Code opcode, RegLocation rl_dest,
                                   RegLocation rl_src1, RegLocation rl_src2) {
  int op = kA64Brk1d;
  RegLocation rl_result;

  /*
   * Don't attempt to optimize register usage since these opcodes call out to
   * the handlers.
   */
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
      CallRuntimeHelperRegLocationRegLocation(QUICK_ENTRYPOINT_OFFSET(8, pFmodf), rl_src1, rl_src2,
                                              false);
      rl_result = GetReturn(true);
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
      CallRuntimeHelperRegLocationRegLocation(QUICK_ENTRYPOINT_OFFSET(8, pFmod), rl_src1, rl_src2,
                                              false);
      rl_result = GetReturnWide(true);
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

  switch (opcode) {
    case Instruction::INT_TO_FLOAT:
      op = kA64Scvtf2fw;
      break;
    case Instruction::FLOAT_TO_INT:
      op = kA64Fcvtzs2wf;
      break;
    case Instruction::DOUBLE_TO_FLOAT:
      op = kA64Fcvt2sS;
      break;
    case Instruction::FLOAT_TO_DOUBLE:
      op = kA64Fcvt2Ss;
      break;
    case Instruction::INT_TO_DOUBLE:
      op = FWIDE(kA64Scvtf2fw);
      break;
    case Instruction::DOUBLE_TO_INT:
      op = FWIDE(kA64Fcvtzs2wf);
      break;
    case Instruction::LONG_TO_DOUBLE:
      op = FWIDE(kA64Scvtf2fx);
      break;
    case Instruction::FLOAT_TO_LONG:
      op = kA64Fcvtzs2xf;
      break;
    case Instruction::LONG_TO_FLOAT:
      op = kA64Scvtf2fx;
      break;
    case Instruction::DOUBLE_TO_LONG:
      op = FWIDE(kA64Fcvtzs2xf);
      break;
    default:
      LOG(FATAL) << "Unexpected opcode: " << opcode;
  }

  if (rl_src.wide) {
    rl_src = LoadValueWide(rl_src, kFPReg);
  } else {
    rl_src = LoadValue(rl_src, kFPReg);
  }

  rl_result = EvalLoc(rl_dest, kFPReg, true);
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

bool Arm64Mir2Lir::GenInlinedSqrt(CallInfo* info) {
  // TODO(Arm64): implement this.
  UNIMPLEMENTED(FATAL) << "GenInlinedSqrt not implemented for Arm64";

  DCHECK_EQ(cu_->instruction_set, kArm64);
  LIR *branch;
  RegLocation rl_src = info->args[0];
  RegLocation rl_dest = InlineTargetWide(info);  // double place for result
  rl_src = LoadValueWide(rl_src, kFPReg);
  RegLocation rl_result = EvalLoc(rl_dest, kFPReg, true);
  NewLIR2(FWIDE(kA64Fsqrt2ff), rl_result.reg.GetReg(), rl_src.reg.GetReg());
  NewLIR2(FWIDE(kA64Fcmp2ff), rl_result.reg.GetReg(), rl_result.reg.GetReg());
  branch = NewLIR2(kA64B2ct, kArmCondEq, 0);
  ClobberCallerSave();
  LockCallTemps();  // Using fixed registers
  RegStorage r_tgt = LoadHelper(QUICK_ENTRYPOINT_OFFSET(8, pSqrt));
  // NewLIR3(kThumb2Fmrrd, r0, r1, rl_src.reg.GetReg());
  NewLIR1(kA64Blr1x, r_tgt.GetReg());
  // NewLIR3(kThumb2Fmdrr, rl_result.reg.GetReg(), r0, r1);
  branch->target = NewLIR0(kPseudoTargetLabel);
  StoreValueWide(rl_dest, rl_result);
  return true;
}

}  // namespace art
