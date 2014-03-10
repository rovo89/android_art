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
#include "x86_lir.h"

namespace art {

void X86Mir2Lir::GenArithOpFloat(Instruction::Code opcode,
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
      FlushAllRegs();   // Send everything to home location
      CallRuntimeHelperRegLocationRegLocation(QUICK_ENTRYPOINT_OFFSET(pFmodf), rl_src1, rl_src2,
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
  int r_dest = rl_result.reg.GetReg();
  int r_src1 = rl_src1.reg.GetReg();
  int r_src2 = rl_src2.reg.GetReg();
  if (r_dest == r_src2) {
    r_src2 = AllocTempFloat();
    OpRegCopy(r_src2, r_dest);
  }
  OpRegCopy(r_dest, r_src1);
  NewLIR2(op, r_dest, r_src2);
  StoreValue(rl_dest, rl_result);
}

void X86Mir2Lir::GenArithOpDouble(Instruction::Code opcode,
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
      FlushAllRegs();   // Send everything to home location
      CallRuntimeHelperRegLocationRegLocation(QUICK_ENTRYPOINT_OFFSET(pFmod), rl_src1, rl_src2,
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
  int r_dest = S2d(rl_result.reg.GetReg(), rl_result.reg.GetHighReg());
  int r_src1 = S2d(rl_src1.reg.GetReg(), rl_src1.reg.GetHighReg());
  int r_src2 = S2d(rl_src2.reg.GetReg(), rl_src2.reg.GetHighReg());
  if (r_dest == r_src2) {
    r_src2 = AllocTempDouble() | X86_FP_DOUBLE;
    OpRegCopy(r_src2, r_dest);
  }
  OpRegCopy(r_dest, r_src1);
  NewLIR2(op, r_dest, r_src2);
  StoreValueWide(rl_dest, rl_result);
}

void X86Mir2Lir::GenLongToFP(RegLocation rl_dest, RegLocation rl_src, bool is_double) {
  // Compute offsets to the source and destination VRs on stack
  int src_v_reg_offset = SRegOffset(rl_src.s_reg_low);
  int dest_v_reg_offset = SRegOffset(rl_dest.s_reg_low);

  // Update the in-register state of source.
  rl_src = UpdateLocWide(rl_src);

  // If the source is in physical register, then put it in its location on stack.
  if (rl_src.location == kLocPhysReg) {
    RegisterInfo* lo_info = GetRegInfo(rl_src.reg.GetReg());

    if (lo_info != nullptr && lo_info->is_temp) {
      // Calling FlushSpecificReg because it will only write back VR if it is dirty.
      FlushSpecificReg(lo_info);
    } else {
      // It must have been register promoted if it is not a temp but is still in physical
      // register. Since we need it to be in memory to convert, we place it there now.
      StoreBaseDispWide(TargetReg(kSp), src_v_reg_offset, rl_src.reg.GetReg(), rl_src.reg.GetHighReg());
    }
  }

  // Push the source virtual register onto the x87 stack.
  LIR *fild64 = NewLIR2NoDest(kX86Fild64M, TargetReg(kSp), src_v_reg_offset + LOWORD_OFFSET);
  AnnotateDalvikRegAccess(fild64, (src_v_reg_offset + LOWORD_OFFSET) >> 2,
      true /* is_load */, true /* is64bit */);

  // Now pop off x87 stack and store it in the destination VR's stack location.
  int opcode = is_double ? kX86Fstp64M : kX86Fstp32M;
  int displacement = is_double ? dest_v_reg_offset + LOWORD_OFFSET : dest_v_reg_offset;
  LIR *fstp = NewLIR2NoDest(opcode, TargetReg(kSp), displacement);
  AnnotateDalvikRegAccess(fstp, displacement >> 2, false /* is_load */, is_double);

  /*
   * The result is in a physical register if it was in a temp or was register
   * promoted. For that reason it is enough to check if it is in physical
   * register. If it is, then we must do all of the bookkeeping necessary to
   * invalidate temp (if needed) and load in promoted register (if needed).
   * If the result's location is in memory, then we do not need to do anything
   * more since the fstp has already placed the correct value in memory.
   */
  RegLocation rl_result = is_double ? UpdateLocWide(rl_dest) : UpdateLoc(rl_dest);
  if (rl_result.location == kLocPhysReg) {
    /*
     * We already know that the result is in a physical register but do not know if it is the
     * right class. So we call EvalLoc(Wide) first which will ensure that it will get moved to the
     * correct register class.
     */
    if (is_double) {
      rl_result = EvalLocWide(rl_dest, kFPReg, true);

      LoadBaseDispWide(TargetReg(kSp), dest_v_reg_offset, rl_result.reg.GetReg(), rl_result.reg.GetHighReg(), INVALID_SREG);

      StoreFinalValueWide(rl_dest, rl_result);
    } else {
      rl_result = EvalLoc(rl_dest, kFPReg, true);

      LoadWordDisp(TargetReg(kSp), dest_v_reg_offset, rl_result.reg.GetReg());

      StoreFinalValue(rl_dest, rl_result);
    }
  }
}

void X86Mir2Lir::GenConversion(Instruction::Code opcode, RegLocation rl_dest,
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
      rl_src = LoadValue(rl_src, kFPReg);
      src_reg = rl_src.reg.GetReg();
      // In case result vreg is also src vreg, break association to avoid useless copy by EvalLoc()
      ClobberSReg(rl_dest.s_reg_low);
      rl_result = EvalLoc(rl_dest, kCoreReg, true);
      int temp_reg = AllocTempFloat();

      LoadConstant(rl_result.reg.GetReg(), 0x7fffffff);
      NewLIR2(kX86Cvtsi2ssRR, temp_reg, rl_result.reg.GetReg());
      NewLIR2(kX86ComissRR, src_reg, temp_reg);
      LIR* branch_pos_overflow = NewLIR2(kX86Jcc8, 0, kX86CondA);
      LIR* branch_na_n = NewLIR2(kX86Jcc8, 0, kX86CondP);
      NewLIR2(kX86Cvttss2siRR, rl_result.reg.GetReg(), src_reg);
      LIR* branch_normal = NewLIR1(kX86Jmp8, 0);
      branch_na_n->target = NewLIR0(kPseudoTargetLabel);
      NewLIR2(kX86Xor32RR, rl_result.reg.GetReg(), rl_result.reg.GetReg());
      branch_pos_overflow->target = NewLIR0(kPseudoTargetLabel);
      branch_normal->target = NewLIR0(kPseudoTargetLabel);
      StoreValue(rl_dest, rl_result);
      return;
    }
    case Instruction::DOUBLE_TO_INT: {
      rl_src = LoadValueWide(rl_src, kFPReg);
      src_reg = rl_src.reg.GetReg();
      // In case result vreg is also src vreg, break association to avoid useless copy by EvalLoc()
      ClobberSReg(rl_dest.s_reg_low);
      rl_result = EvalLoc(rl_dest, kCoreReg, true);
      int temp_reg = AllocTempDouble() | X86_FP_DOUBLE;

      LoadConstant(rl_result.reg.GetReg(), 0x7fffffff);
      NewLIR2(kX86Cvtsi2sdRR, temp_reg, rl_result.reg.GetReg());
      NewLIR2(kX86ComisdRR, src_reg, temp_reg);
      LIR* branch_pos_overflow = NewLIR2(kX86Jcc8, 0, kX86CondA);
      LIR* branch_na_n = NewLIR2(kX86Jcc8, 0, kX86CondP);
      NewLIR2(kX86Cvttsd2siRR, rl_result.reg.GetReg(), src_reg);
      LIR* branch_normal = NewLIR1(kX86Jmp8, 0);
      branch_na_n->target = NewLIR0(kPseudoTargetLabel);
      NewLIR2(kX86Xor32RR, rl_result.reg.GetReg(), rl_result.reg.GetReg());
      branch_pos_overflow->target = NewLIR0(kPseudoTargetLabel);
      branch_normal->target = NewLIR0(kPseudoTargetLabel);
      StoreValue(rl_dest, rl_result);
      return;
    }
    case Instruction::LONG_TO_DOUBLE:
      GenLongToFP(rl_dest, rl_src, true /* is_double */);
      return;
    case Instruction::LONG_TO_FLOAT:
      GenLongToFP(rl_dest, rl_src, false /* is_double */);
      return;
    case Instruction::FLOAT_TO_LONG:
      GenConversionCall(QUICK_ENTRYPOINT_OFFSET(pF2l), rl_dest, rl_src);
      return;
    case Instruction::DOUBLE_TO_LONG:
      GenConversionCall(QUICK_ENTRYPOINT_OFFSET(pD2l), rl_dest, rl_src);
      return;
    default:
      LOG(INFO) << "Unexpected opcode: " << opcode;
  }
  if (rl_src.wide) {
    rl_src = LoadValueWide(rl_src, rcSrc);
    src_reg = S2d(rl_src.reg.GetReg(), rl_src.reg.GetHighReg());
  } else {
    rl_src = LoadValue(rl_src, rcSrc);
    src_reg = rl_src.reg.GetReg();
  }
  if (rl_dest.wide) {
    rl_result = EvalLoc(rl_dest, kFPReg, true);
    NewLIR2(op, S2d(rl_result.reg.GetReg(), rl_result.reg.GetHighReg()), src_reg);
    StoreValueWide(rl_dest, rl_result);
  } else {
    rl_result = EvalLoc(rl_dest, kFPReg, true);
    NewLIR2(op, rl_result.reg.GetReg(), src_reg);
    StoreValue(rl_dest, rl_result);
  }
}

void X86Mir2Lir::GenCmpFP(Instruction::Code code, RegLocation rl_dest,
                          RegLocation rl_src1, RegLocation rl_src2) {
  bool single = (code == Instruction::CMPL_FLOAT) || (code == Instruction::CMPG_FLOAT);
  bool unordered_gt = (code == Instruction::CMPG_DOUBLE) || (code == Instruction::CMPG_FLOAT);
  int src_reg1;
  int src_reg2;
  if (single) {
    rl_src1 = LoadValue(rl_src1, kFPReg);
    src_reg1 = rl_src1.reg.GetReg();
    rl_src2 = LoadValue(rl_src2, kFPReg);
    src_reg2 = rl_src2.reg.GetReg();
  } else {
    rl_src1 = LoadValueWide(rl_src1, kFPReg);
    src_reg1 = S2d(rl_src1.reg.GetReg(), rl_src1.reg.GetHighReg());
    rl_src2 = LoadValueWide(rl_src2, kFPReg);
    src_reg2 = S2d(rl_src2.reg.GetReg(), rl_src2.reg.GetHighReg());
  }
  // In case result vreg is also src vreg, break association to avoid useless copy by EvalLoc()
  ClobberSReg(rl_dest.s_reg_low);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  LoadConstantNoClobber(rl_result.reg.GetReg(), unordered_gt ? 1 : 0);
  if (single) {
    NewLIR2(kX86UcomissRR, src_reg1, src_reg2);
  } else {
    NewLIR2(kX86UcomisdRR, src_reg1, src_reg2);
  }
  LIR* branch = NULL;
  if (unordered_gt) {
    branch = NewLIR2(kX86Jcc8, 0, kX86CondPE);
  }
  // If the result reg can't be byte accessed, use a jump and move instead of a set.
  if (rl_result.reg.GetReg() >= 4) {
    LIR* branch2 = NULL;
    if (unordered_gt) {
      branch2 = NewLIR2(kX86Jcc8, 0, kX86CondA);
      NewLIR2(kX86Mov32RI, rl_result.reg.GetReg(), 0x0);
    } else {
      branch2 = NewLIR2(kX86Jcc8, 0, kX86CondBe);
      NewLIR2(kX86Mov32RI, rl_result.reg.GetReg(), 0x1);
    }
    branch2->target = NewLIR0(kPseudoTargetLabel);
  } else {
    NewLIR2(kX86Set8R, rl_result.reg.GetReg(), kX86CondA /* above - unsigned > */);
  }
  NewLIR2(kX86Sbb32RI, rl_result.reg.GetReg(), 0);
  if (unordered_gt) {
    branch->target = NewLIR0(kPseudoTargetLabel);
  }
  StoreValue(rl_dest, rl_result);
}

void X86Mir2Lir::GenFusedFPCmpBranch(BasicBlock* bb, MIR* mir, bool gt_bias,
                                     bool is_double) {
  LIR* taken = &block_label_list_[bb->taken];
  LIR* not_taken = &block_label_list_[bb->fall_through];
  LIR* branch = NULL;
  RegLocation rl_src1;
  RegLocation rl_src2;
  if (is_double) {
    rl_src1 = mir_graph_->GetSrcWide(mir, 0);
    rl_src2 = mir_graph_->GetSrcWide(mir, 2);
    rl_src1 = LoadValueWide(rl_src1, kFPReg);
    rl_src2 = LoadValueWide(rl_src2, kFPReg);
    NewLIR2(kX86UcomisdRR, S2d(rl_src1.reg.GetReg(), rl_src1.reg.GetHighReg()),
            S2d(rl_src2.reg.GetReg(), rl_src2.reg.GetHighReg()));
  } else {
    rl_src1 = mir_graph_->GetSrc(mir, 0);
    rl_src2 = mir_graph_->GetSrc(mir, 1);
    rl_src1 = LoadValue(rl_src1, kFPReg);
    rl_src2 = LoadValue(rl_src2, kFPReg);
    NewLIR2(kX86UcomissRR, rl_src1.reg.GetReg(), rl_src2.reg.GetReg());
  }
  ConditionCode ccode = mir->meta.ccode;
  switch (ccode) {
    case kCondEq:
      if (!gt_bias) {
        branch = NewLIR2(kX86Jcc8, 0, kX86CondPE);
        branch->target = not_taken;
      }
      break;
    case kCondNe:
      if (!gt_bias) {
        branch = NewLIR2(kX86Jcc8, 0, kX86CondPE);
        branch->target = taken;
      }
      break;
    case kCondLt:
      if (gt_bias) {
        branch = NewLIR2(kX86Jcc8, 0, kX86CondPE);
        branch->target = not_taken;
      }
      ccode = kCondUlt;
      break;
    case kCondLe:
      if (gt_bias) {
        branch = NewLIR2(kX86Jcc8, 0, kX86CondPE);
        branch->target = not_taken;
      }
      ccode = kCondLs;
      break;
    case kCondGt:
      if (gt_bias) {
        branch = NewLIR2(kX86Jcc8, 0, kX86CondPE);
        branch->target = taken;
      }
      ccode = kCondHi;
      break;
    case kCondGe:
      if (gt_bias) {
        branch = NewLIR2(kX86Jcc8, 0, kX86CondPE);
        branch->target = taken;
      }
      ccode = kCondUge;
      break;
    default:
      LOG(FATAL) << "Unexpected ccode: " << ccode;
  }
  OpCondBranch(ccode, taken);
}

void X86Mir2Lir::GenNegFloat(RegLocation rl_dest, RegLocation rl_src) {
  RegLocation rl_result;
  rl_src = LoadValue(rl_src, kCoreReg);
  rl_result = EvalLoc(rl_dest, kCoreReg, true);
  OpRegRegImm(kOpAdd, rl_result.reg.GetReg(), rl_src.reg.GetReg(), 0x80000000);
  StoreValue(rl_dest, rl_result);
}

void X86Mir2Lir::GenNegDouble(RegLocation rl_dest, RegLocation rl_src) {
  RegLocation rl_result;
  rl_src = LoadValueWide(rl_src, kCoreReg);
  rl_result = EvalLoc(rl_dest, kCoreReg, true);
  OpRegRegImm(kOpAdd, rl_result.reg.GetHighReg(), rl_src.reg.GetHighReg(), 0x80000000);
  OpRegCopy(rl_result.reg.GetReg(), rl_src.reg.GetReg());
  StoreValueWide(rl_dest, rl_result);
}

bool X86Mir2Lir::GenInlinedSqrt(CallInfo* info) {
  RegLocation rl_src = info->args[0];
  RegLocation rl_dest = InlineTargetWide(info);  // double place for result
  rl_src = LoadValueWide(rl_src, kFPReg);
  RegLocation rl_result = EvalLoc(rl_dest, kFPReg, true);
  NewLIR2(kX86SqrtsdRR, S2d(rl_result.reg.GetReg(), rl_result.reg.GetHighReg()),
          S2d(rl_src.reg.GetReg(), rl_src.reg.GetHighReg()));
  StoreValueWide(rl_dest, rl_result);
  return true;
}



}  // namespace art
