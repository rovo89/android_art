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

#include "x86_lir.h"
#include "codegen_x86.h"
#include "../codegen_util.h"
#include "../ralloc_util.h"

namespace art {

bool X86Codegen::GenArithOpFloat(CompilationUnit *cu, Instruction::Code opcode,
                                 RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2) {
  X86OpCode op = kX86Nop;
  RegLocation rl_result;

  /*
   * Don't attempt to optimize register usage since these opcodes call out to
   * the handlers.
   */
  switch (opcode) {
    case Instruction::ADD_FLOAT_2ADDR:
    case Instruction::ADD_FLOAT:
      op = kX86AddssRR;
      break;
    case Instruction::SUB_FLOAT_2ADDR:
    case Instruction::SUB_FLOAT:
      op = kX86SubssRR;
      break;
    case Instruction::DIV_FLOAT_2ADDR:
    case Instruction::DIV_FLOAT:
      op = kX86DivssRR;
      break;
    case Instruction::MUL_FLOAT_2ADDR:
    case Instruction::MUL_FLOAT:
      op = kX86MulssRR;
      break;
    case Instruction::REM_FLOAT_2ADDR:
    case Instruction::REM_FLOAT:
      FlushAllRegs(cu);   // Send everything to home location
      CallRuntimeHelperRegLocationRegLocation(cu, ENTRYPOINT_OFFSET(pFmodf), rl_src1, rl_src2, false);
      rl_result = GetReturn(cu, true);
      StoreValue(cu, rl_dest, rl_result);
      return false;
    case Instruction::NEG_FLOAT:
      GenNegFloat(cu, rl_dest, rl_src1);
      return false;
    default:
      return true;
  }
  rl_src1 = LoadValue(cu, rl_src1, kFPReg);
  rl_src2 = LoadValue(cu, rl_src2, kFPReg);
  rl_result = EvalLoc(cu, rl_dest, kFPReg, true);
  int r_dest = rl_result.low_reg;
  int r_src1 = rl_src1.low_reg;
  int r_src2 = rl_src2.low_reg;
  if (r_dest == r_src2) {
    r_src2 = AllocTempFloat(cu);
    OpRegCopy(cu, r_src2, r_dest);
  }
  OpRegCopy(cu, r_dest, r_src1);
  NewLIR2(cu, op, r_dest, r_src2);
  StoreValue(cu, rl_dest, rl_result);

  return false;
}

bool X86Codegen::GenArithOpDouble(CompilationUnit *cu, Instruction::Code opcode,
                                  RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2) {
  X86OpCode op = kX86Nop;
  RegLocation rl_result;

  switch (opcode) {
    case Instruction::ADD_DOUBLE_2ADDR:
    case Instruction::ADD_DOUBLE:
      op = kX86AddsdRR;
      break;
    case Instruction::SUB_DOUBLE_2ADDR:
    case Instruction::SUB_DOUBLE:
      op = kX86SubsdRR;
      break;
    case Instruction::DIV_DOUBLE_2ADDR:
    case Instruction::DIV_DOUBLE:
      op = kX86DivsdRR;
      break;
    case Instruction::MUL_DOUBLE_2ADDR:
    case Instruction::MUL_DOUBLE:
      op = kX86MulsdRR;
      break;
    case Instruction::REM_DOUBLE_2ADDR:
    case Instruction::REM_DOUBLE:
      FlushAllRegs(cu);   // Send everything to home location
      CallRuntimeHelperRegLocationRegLocation(cu, ENTRYPOINT_OFFSET(pFmod), rl_src1, rl_src2, false);
      rl_result = GetReturnWide(cu, true);
      StoreValueWide(cu, rl_dest, rl_result);
      return false;
    case Instruction::NEG_DOUBLE:
      GenNegDouble(cu, rl_dest, rl_src1);
      return false;
    default:
      return true;
  }
  rl_src1 = LoadValueWide(cu, rl_src1, kFPReg);
  DCHECK(rl_src1.wide);
  rl_src2 = LoadValueWide(cu, rl_src2, kFPReg);
  DCHECK(rl_src2.wide);
  rl_result = EvalLoc(cu, rl_dest, kFPReg, true);
  DCHECK(rl_dest.wide);
  DCHECK(rl_result.wide);
  int r_dest = S2d(rl_result.low_reg, rl_result.high_reg);
  int r_src1 = S2d(rl_src1.low_reg, rl_src1.high_reg);
  int r_src2 = S2d(rl_src2.low_reg, rl_src2.high_reg);
  if (r_dest == r_src2) {
    r_src2 = AllocTempDouble(cu) | X86_FP_DOUBLE;
    OpRegCopy(cu, r_src2, r_dest);
  }
  OpRegCopy(cu, r_dest, r_src1);
  NewLIR2(cu, op, r_dest, r_src2);
  StoreValueWide(cu, rl_dest, rl_result);
  return false;
}

bool X86Codegen::GenConversion(CompilationUnit *cu, Instruction::Code opcode, RegLocation rl_dest,
                               RegLocation rl_src) {
  RegisterClass rcSrc = kFPReg;
  X86OpCode op = kX86Nop;
  int src_reg;
  RegLocation rl_result;
  switch (opcode) {
    case Instruction::INT_TO_FLOAT:
      rcSrc = kCoreReg;
      op = kX86Cvtsi2ssRR;
      break;
    case Instruction::DOUBLE_TO_FLOAT:
      rcSrc = kFPReg;
      op = kX86Cvtsd2ssRR;
      break;
    case Instruction::FLOAT_TO_DOUBLE:
      rcSrc = kFPReg;
      op = kX86Cvtss2sdRR;
      break;
    case Instruction::INT_TO_DOUBLE:
      rcSrc = kCoreReg;
      op = kX86Cvtsi2sdRR;
      break;
    case Instruction::FLOAT_TO_INT: {
      rl_src = LoadValue(cu, rl_src, kFPReg);
      src_reg = rl_src.low_reg;
      // In case result vreg is also src vreg, break association to avoid useless copy by EvalLoc()
      ClobberSReg(cu, rl_dest.s_reg_low);
      rl_result = EvalLoc(cu, rl_dest, kCoreReg, true);
      int temp_reg = AllocTempFloat(cu);

      LoadConstant(cu, rl_result.low_reg, 0x7fffffff);
      NewLIR2(cu, kX86Cvtsi2ssRR, temp_reg, rl_result.low_reg);
      NewLIR2(cu, kX86ComissRR, src_reg, temp_reg);
      LIR* branch_pos_overflow = NewLIR2(cu, kX86Jcc8, 0, kX86CondA);
      LIR* branch_na_n = NewLIR2(cu, kX86Jcc8, 0, kX86CondP);
      NewLIR2(cu, kX86Cvttss2siRR, rl_result.low_reg, src_reg);
      LIR* branch_normal = NewLIR1(cu, kX86Jmp8, 0);
      branch_na_n->target = NewLIR0(cu, kPseudoTargetLabel);
      NewLIR2(cu, kX86Xor32RR, rl_result.low_reg, rl_result.low_reg);
      branch_pos_overflow->target = NewLIR0(cu, kPseudoTargetLabel);
      branch_normal->target = NewLIR0(cu, kPseudoTargetLabel);
      StoreValue(cu, rl_dest, rl_result);
      return false;
    }
    case Instruction::DOUBLE_TO_INT: {
      rl_src = LoadValueWide(cu, rl_src, kFPReg);
      src_reg = rl_src.low_reg;
      // In case result vreg is also src vreg, break association to avoid useless copy by EvalLoc()
      ClobberSReg(cu, rl_dest.s_reg_low);
      rl_result = EvalLoc(cu, rl_dest, kCoreReg, true);
      int temp_reg = AllocTempDouble(cu) | X86_FP_DOUBLE;

      LoadConstant(cu, rl_result.low_reg, 0x7fffffff);
      NewLIR2(cu, kX86Cvtsi2sdRR, temp_reg, rl_result.low_reg);
      NewLIR2(cu, kX86ComisdRR, src_reg, temp_reg);
      LIR* branch_pos_overflow = NewLIR2(cu, kX86Jcc8, 0, kX86CondA);
      LIR* branch_na_n = NewLIR2(cu, kX86Jcc8, 0, kX86CondP);
      NewLIR2(cu, kX86Cvttsd2siRR, rl_result.low_reg, src_reg);
      LIR* branch_normal = NewLIR1(cu, kX86Jmp8, 0);
      branch_na_n->target = NewLIR0(cu, kPseudoTargetLabel);
      NewLIR2(cu, kX86Xor32RR, rl_result.low_reg, rl_result.low_reg);
      branch_pos_overflow->target = NewLIR0(cu, kPseudoTargetLabel);
      branch_normal->target = NewLIR0(cu, kPseudoTargetLabel);
      StoreValue(cu, rl_dest, rl_result);
      return false;
    }
    case Instruction::LONG_TO_DOUBLE:
      return GenConversionCall(cu, ENTRYPOINT_OFFSET(pL2d), rl_dest, rl_src);
    case Instruction::LONG_TO_FLOAT:
      // TODO: inline by using memory as a 64-bit source. Be careful about promoted registers.
      return GenConversionCall(cu, ENTRYPOINT_OFFSET(pL2f), rl_dest, rl_src);
    case Instruction::FLOAT_TO_LONG:
      return GenConversionCall(cu, ENTRYPOINT_OFFSET(pF2l), rl_dest, rl_src);
    case Instruction::DOUBLE_TO_LONG:
      return GenConversionCall(cu, ENTRYPOINT_OFFSET(pD2l), rl_dest, rl_src);
    default:
      return true;
  }
  if (rl_src.wide) {
    rl_src = LoadValueWide(cu, rl_src, rcSrc);
    src_reg = S2d(rl_src.low_reg, rl_src.high_reg);
  } else {
    rl_src = LoadValue(cu, rl_src, rcSrc);
    src_reg = rl_src.low_reg;
  }
  if (rl_dest.wide) {
    rl_result = EvalLoc(cu, rl_dest, kFPReg, true);
    NewLIR2(cu, op, S2d(rl_result.low_reg, rl_result.high_reg), src_reg);
    StoreValueWide(cu, rl_dest, rl_result);
  } else {
    rl_result = EvalLoc(cu, rl_dest, kFPReg, true);
    NewLIR2(cu, op, rl_result.low_reg, src_reg);
    StoreValue(cu, rl_dest, rl_result);
  }
  return false;
}

bool X86Codegen::GenCmpFP(CompilationUnit *cu, Instruction::Code code, RegLocation rl_dest,
                          RegLocation rl_src1, RegLocation rl_src2) {
  bool single = (code == Instruction::CMPL_FLOAT) || (code == Instruction::CMPG_FLOAT);
  bool unordered_gt = (code == Instruction::CMPG_DOUBLE) || (code == Instruction::CMPG_FLOAT);
  int src_reg1;
  int src_reg2;
  if (single) {
    rl_src1 = LoadValue(cu, rl_src1, kFPReg);
    src_reg1 = rl_src1.low_reg;
    rl_src2 = LoadValue(cu, rl_src2, kFPReg);
    src_reg2 = rl_src2.low_reg;
  } else {
    rl_src1 = LoadValueWide(cu, rl_src1, kFPReg);
    src_reg1 = S2d(rl_src1.low_reg, rl_src1.high_reg);
    rl_src2 = LoadValueWide(cu, rl_src2, kFPReg);
    src_reg2 = S2d(rl_src2.low_reg, rl_src2.high_reg);
  }
  // In case result vreg is also src vreg, break association to avoid useless copy by EvalLoc()
  ClobberSReg(cu, rl_dest.s_reg_low);
  RegLocation rl_result = EvalLoc(cu, rl_dest, kCoreReg, true);
  LoadConstantNoClobber(cu, rl_result.low_reg, unordered_gt ? 1 : 0);
  if (single) {
    NewLIR2(cu, kX86UcomissRR, src_reg1, src_reg2);
  } else {
    NewLIR2(cu, kX86UcomisdRR, src_reg1, src_reg2);
  }
  LIR* branch = NULL;
  if (unordered_gt) {
    branch = NewLIR2(cu, kX86Jcc8, 0, kX86CondPE);
  }
  // If the result reg can't be byte accessed, use a jump and move instead of a set.
  if (rl_result.low_reg >= 4) {
    LIR* branch2 = NULL;
    if (unordered_gt) {
      branch2 = NewLIR2(cu, kX86Jcc8, 0, kX86CondA);
      NewLIR2(cu, kX86Mov32RI, rl_result.low_reg, 0x0);
    } else {
      branch2 = NewLIR2(cu, kX86Jcc8, 0, kX86CondBe);
      NewLIR2(cu, kX86Mov32RI, rl_result.low_reg, 0x1);
    }
    branch2->target = NewLIR0(cu, kPseudoTargetLabel);
  } else {
    NewLIR2(cu, kX86Set8R, rl_result.low_reg, kX86CondA /* above - unsigned > */);
  }
  NewLIR2(cu, kX86Sbb32RI, rl_result.low_reg, 0);
  if (unordered_gt) {
    branch->target = NewLIR0(cu, kPseudoTargetLabel);
  }
  StoreValue(cu, rl_dest, rl_result);
  return false;
}

void X86Codegen::GenFusedFPCmpBranch(CompilationUnit* cu, BasicBlock* bb, MIR* mir, bool gt_bias,
                                     bool is_double) {
  LIR* label_list = cu->block_label_list;
  LIR* taken = &label_list[bb->taken->id];
  LIR* not_taken = &label_list[bb->fall_through->id];
  LIR* branch = NULL;
  RegLocation rl_src1;
  RegLocation rl_src2;
  if (is_double) {
    rl_src1 = GetSrcWide(cu, mir, 0);
    rl_src2 = GetSrcWide(cu, mir, 2);
    rl_src1 = LoadValueWide(cu, rl_src1, kFPReg);
    rl_src2 = LoadValueWide(cu, rl_src2, kFPReg);
    NewLIR2(cu, kX86UcomisdRR, S2d(rl_src1.low_reg, rl_src1.high_reg),
            S2d(rl_src2.low_reg, rl_src2.high_reg));
  } else {
    rl_src1 = GetSrc(cu, mir, 0);
    rl_src2 = GetSrc(cu, mir, 1);
    rl_src1 = LoadValue(cu, rl_src1, kFPReg);
    rl_src2 = LoadValue(cu, rl_src2, kFPReg);
    NewLIR2(cu, kX86UcomissRR, rl_src1.low_reg, rl_src2.low_reg);
  }
  ConditionCode ccode = static_cast<ConditionCode>(mir->dalvikInsn.arg[0]);
  switch (ccode) {
    case kCondEq:
      if (!gt_bias) {
        branch = NewLIR2(cu, kX86Jcc8, 0, kX86CondPE);
        branch->target = not_taken;
      }
      break;
    case kCondNe:
      if (!gt_bias) {
        branch = NewLIR2(cu, kX86Jcc8, 0, kX86CondPE);
        branch->target = taken;
      }
      break;
    case kCondLt:
      if (gt_bias) {
        branch = NewLIR2(cu, kX86Jcc8, 0, kX86CondPE);
        branch->target = not_taken;
      }
      ccode = kCondCs;
      break;
    case kCondLe:
      if (gt_bias) {
        branch = NewLIR2(cu, kX86Jcc8, 0, kX86CondPE);
        branch->target = not_taken;
      }
      ccode = kCondLs;
      break;
    case kCondGt:
      if (gt_bias) {
        branch = NewLIR2(cu, kX86Jcc8, 0, kX86CondPE);
        branch->target = taken;
      }
      ccode = kCondHi;
      break;
    case kCondGe:
      if (gt_bias) {
        branch = NewLIR2(cu, kX86Jcc8, 0, kX86CondPE);
        branch->target = taken;
      }
      ccode = kCondCc;
      break;
    default:
      LOG(FATAL) << "Unexpected ccode: " << ccode;
  }
  OpCondBranch(cu, ccode, taken);
}

void X86Codegen::GenNegFloat(CompilationUnit *cu, RegLocation rl_dest, RegLocation rl_src)
{
  RegLocation rl_result;
  rl_src = LoadValue(cu, rl_src, kCoreReg);
  rl_result = EvalLoc(cu, rl_dest, kCoreReg, true);
  OpRegRegImm(cu, kOpAdd, rl_result.low_reg, rl_src.low_reg, 0x80000000);
  StoreValue(cu, rl_dest, rl_result);
}

void X86Codegen::GenNegDouble(CompilationUnit *cu, RegLocation rl_dest, RegLocation rl_src)
{
  RegLocation rl_result;
  rl_src = LoadValueWide(cu, rl_src, kCoreReg);
  rl_result = EvalLoc(cu, rl_dest, kCoreReg, true);
  OpRegRegImm(cu, kOpAdd, rl_result.high_reg, rl_src.high_reg, 0x80000000);
  OpRegCopy(cu, rl_result.low_reg, rl_src.low_reg);
  StoreValueWide(cu, rl_dest, rl_result);
}

bool X86Codegen::GenInlinedSqrt(CompilationUnit* cu, CallInfo* info) {
  DCHECK_NE(cu->instruction_set, kThumb2);
  return false;
}



} //  namespace art
