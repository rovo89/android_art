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
#include "dex/reg_storage_eq.h"
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
      GenRemFP(rl_dest, rl_src1, rl_src2, false /* is_double */);
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
  RegStorage r_dest = rl_result.reg;
  RegStorage r_src1 = rl_src1.reg;
  RegStorage r_src2 = rl_src2.reg;
  if (r_dest == r_src2) {
    r_src2 = AllocTempSingle();
    OpRegCopy(r_src2, r_dest);
  }
  OpRegCopy(r_dest, r_src1);
  NewLIR2(op, r_dest.GetReg(), r_src2.GetReg());
  StoreValue(rl_dest, rl_result);
}

void X86Mir2Lir::GenArithOpDouble(Instruction::Code opcode,
                                  RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2) {
  DCHECK(rl_dest.wide);
  DCHECK(rl_dest.fp);
  DCHECK(rl_src1.wide);
  DCHECK(rl_src1.fp);
  DCHECK(rl_src2.wide);
  DCHECK(rl_src2.fp);
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
      GenRemFP(rl_dest, rl_src1, rl_src2, true /* is_double */);
      return;
    case Instruction::NEG_DOUBLE:
      GenNegDouble(rl_dest, rl_src1);
      return;
    default:
      LOG(FATAL) << "Unexpected opcode: " << opcode;
  }
  rl_src1 = LoadValueWide(rl_src1, kFPReg);
  rl_src2 = LoadValueWide(rl_src2, kFPReg);
  rl_result = EvalLoc(rl_dest, kFPReg, true);
  if (rl_result.reg == rl_src2.reg) {
    rl_src2.reg = AllocTempDouble();
    OpRegCopy(rl_src2.reg, rl_result.reg);
  }
  OpRegCopy(rl_result.reg, rl_src1.reg);
  NewLIR2(op, rl_result.reg.GetReg(), rl_src2.reg.GetReg());
  StoreValueWide(rl_dest, rl_result);
}

void X86Mir2Lir::GenLongToFP(RegLocation rl_dest, RegLocation rl_src, bool is_double) {
  // Compute offsets to the source and destination VRs on stack
  int src_v_reg_offset = SRegOffset(rl_src.s_reg_low);
  int dest_v_reg_offset = SRegOffset(rl_dest.s_reg_low);

  // Update the in-register state of source.
  rl_src = UpdateLocWide(rl_src);

  // All memory accesses below reference dalvik regs.
  ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);

  // If the source is in physical register, then put it in its location on stack.
  if (rl_src.location == kLocPhysReg) {
    RegisterInfo* reg_info = GetRegInfo(rl_src.reg);

    if (reg_info != nullptr && reg_info->IsTemp()) {
      // Calling FlushSpecificReg because it will only write back VR if it is dirty.
      FlushSpecificReg(reg_info);
      // ResetDef to prevent NullifyRange from removing stores.
      ResetDef(rl_src.reg);
    } else {
      // It must have been register promoted if it is not a temp but is still in physical
      // register. Since we need it to be in memory to convert, we place it there now.
      StoreBaseDisp(rs_rX86_SP, src_v_reg_offset, rl_src.reg, k64, kNotVolatile);
    }
  }

  // Push the source virtual register onto the x87 stack.
  LIR *fild64 = NewLIR2NoDest(kX86Fild64M, rs_rX86_SP.GetReg(),
                              src_v_reg_offset + LOWORD_OFFSET);
  AnnotateDalvikRegAccess(fild64, (src_v_reg_offset + LOWORD_OFFSET) >> 2,
                          true /* is_load */, true /* is64bit */);

  // Now pop off x87 stack and store it in the destination VR's stack location.
  int opcode = is_double ? kX86Fstp64M : kX86Fstp32M;
  int displacement = is_double ? dest_v_reg_offset + LOWORD_OFFSET : dest_v_reg_offset;
  LIR *fstp = NewLIR2NoDest(opcode, rs_rX86_SP.GetReg(), displacement);
  AnnotateDalvikRegAccess(fstp, displacement >> 2, false /* is_load */, is_double);

  /*
   * The result is in a physical register if it was in a temp or was register
   * promoted. For that reason it is enough to check if it is in physical
   * register. If it is, then we must do all of the bookkeeping necessary to
   * invalidate temp (if needed) and load in promoted register (if needed).
   * If the result's location is in memory, then we do not need to do anything
   * more since the fstp has already placed the correct value in memory.
   */
  RegLocation rl_result = is_double ? UpdateLocWideTyped(rl_dest, kFPReg) :
      UpdateLocTyped(rl_dest, kFPReg);
  if (rl_result.location == kLocPhysReg) {
    /*
     * We already know that the result is in a physical register but do not know if it is the
     * right class. So we call EvalLoc(Wide) first which will ensure that it will get moved to the
     * correct register class.
     */
    rl_result = EvalLoc(rl_dest, kFPReg, true);
    if (is_double) {
      LoadBaseDisp(rs_rX86_SP, dest_v_reg_offset, rl_result.reg, k64, kNotVolatile);

      StoreFinalValueWide(rl_dest, rl_result);
    } else {
      Load32Disp(rs_rX86_SP, dest_v_reg_offset, rl_result.reg);

      StoreFinalValue(rl_dest, rl_result);
    }
  }
}

void X86Mir2Lir::GenConversion(Instruction::Code opcode, RegLocation rl_dest,
                               RegLocation rl_src) {
  RegisterClass rcSrc = kFPReg;
  X86OpCode op = kX86Nop;
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
      // In case result vreg is also src vreg, break association to avoid useless copy by EvalLoc()
      ClobberSReg(rl_dest.s_reg_low);
      rl_result = EvalLoc(rl_dest, kCoreReg, true);
      RegStorage temp_reg = AllocTempSingle();

      LoadConstant(rl_result.reg, 0x7fffffff);
      NewLIR2(kX86Cvtsi2ssRR, temp_reg.GetReg(), rl_result.reg.GetReg());
      NewLIR2(kX86ComissRR, rl_src.reg.GetReg(), temp_reg.GetReg());
      LIR* branch_pos_overflow = NewLIR2(kX86Jcc8, 0, kX86CondAe);
      LIR* branch_na_n = NewLIR2(kX86Jcc8, 0, kX86CondP);
      NewLIR2(kX86Cvttss2siRR, rl_result.reg.GetReg(), rl_src.reg.GetReg());
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
      // In case result vreg is also src vreg, break association to avoid useless copy by EvalLoc()
      ClobberSReg(rl_dest.s_reg_low);
      rl_result = EvalLoc(rl_dest, kCoreReg, true);
      RegStorage temp_reg = AllocTempDouble();

      LoadConstant(rl_result.reg, 0x7fffffff);
      NewLIR2(kX86Cvtsi2sdRR, temp_reg.GetReg(), rl_result.reg.GetReg());
      NewLIR2(kX86ComisdRR, rl_src.reg.GetReg(), temp_reg.GetReg());
      LIR* branch_pos_overflow = NewLIR2(kX86Jcc8, 0, kX86CondAe);
      LIR* branch_na_n = NewLIR2(kX86Jcc8, 0, kX86CondP);
      NewLIR2(kX86Cvttsd2siRR, rl_result.reg.GetReg(), rl_src.reg.GetReg());
      LIR* branch_normal = NewLIR1(kX86Jmp8, 0);
      branch_na_n->target = NewLIR0(kPseudoTargetLabel);
      NewLIR2(kX86Xor32RR, rl_result.reg.GetReg(), rl_result.reg.GetReg());
      branch_pos_overflow->target = NewLIR0(kPseudoTargetLabel);
      branch_normal->target = NewLIR0(kPseudoTargetLabel);
      StoreValue(rl_dest, rl_result);
      return;
    }
    case Instruction::LONG_TO_DOUBLE:
      if (cu_->target64) {
        rcSrc = kCoreReg;
        op = kX86Cvtsqi2sdRR;
        break;
      }
      GenLongToFP(rl_dest, rl_src, true /* is_double */);
      return;
    case Instruction::LONG_TO_FLOAT:
      if (cu_->target64) {
        rcSrc = kCoreReg;
        op = kX86Cvtsqi2ssRR;
       break;
      }
      GenLongToFP(rl_dest, rl_src, false /* is_double */);
      return;
    case Instruction::FLOAT_TO_LONG:
      if (cu_->target64) {
        rl_src = LoadValue(rl_src, kFPReg);
        // If result vreg is also src vreg, break association to avoid useless copy by EvalLoc()
        ClobberSReg(rl_dest.s_reg_low);
        rl_result = EvalLoc(rl_dest, kCoreReg, true);
        RegStorage temp_reg = AllocTempSingle();

        // Set 0x7fffffffffffffff to rl_result
        LoadConstantWide(rl_result.reg, 0x7fffffffffffffff);
        NewLIR2(kX86Cvtsqi2ssRR, temp_reg.GetReg(), rl_result.reg.GetReg());
        NewLIR2(kX86ComissRR, rl_src.reg.GetReg(), temp_reg.GetReg());
        LIR* branch_pos_overflow = NewLIR2(kX86Jcc8, 0, kX86CondAe);
        LIR* branch_na_n = NewLIR2(kX86Jcc8, 0, kX86CondP);
        NewLIR2(kX86Cvttss2sqiRR, rl_result.reg.GetReg(), rl_src.reg.GetReg());
        LIR* branch_normal = NewLIR1(kX86Jmp8, 0);
        branch_na_n->target = NewLIR0(kPseudoTargetLabel);
        NewLIR2(kX86Xor64RR, rl_result.reg.GetReg(), rl_result.reg.GetReg());
        branch_pos_overflow->target = NewLIR0(kPseudoTargetLabel);
        branch_normal->target = NewLIR0(kPseudoTargetLabel);
        StoreValueWide(rl_dest, rl_result);
      } else {
        GenConversionCall(kQuickF2l, rl_dest, rl_src);
      }
      return;
    case Instruction::DOUBLE_TO_LONG:
      if (cu_->target64) {
        rl_src = LoadValueWide(rl_src, kFPReg);
        // If result vreg is also src vreg, break association to avoid useless copy by EvalLoc()
        ClobberSReg(rl_dest.s_reg_low);
        rl_result = EvalLoc(rl_dest, kCoreReg, true);
        RegStorage temp_reg = AllocTempDouble();

        // Set 0x7fffffffffffffff to rl_result
        LoadConstantWide(rl_result.reg, 0x7fffffffffffffff);
        NewLIR2(kX86Cvtsqi2sdRR, temp_reg.GetReg(), rl_result.reg.GetReg());
        NewLIR2(kX86ComisdRR, rl_src.reg.GetReg(), temp_reg.GetReg());
        LIR* branch_pos_overflow = NewLIR2(kX86Jcc8, 0, kX86CondAe);
        LIR* branch_na_n = NewLIR2(kX86Jcc8, 0, kX86CondP);
        NewLIR2(kX86Cvttsd2sqiRR, rl_result.reg.GetReg(), rl_src.reg.GetReg());
        LIR* branch_normal = NewLIR1(kX86Jmp8, 0);
        branch_na_n->target = NewLIR0(kPseudoTargetLabel);
        NewLIR2(kX86Xor64RR, rl_result.reg.GetReg(), rl_result.reg.GetReg());
        branch_pos_overflow->target = NewLIR0(kPseudoTargetLabel);
        branch_normal->target = NewLIR0(kPseudoTargetLabel);
        StoreValueWide(rl_dest, rl_result);
      } else {
        GenConversionCall(kQuickD2l, rl_dest, rl_src);
      }
      return;
    default:
      LOG(INFO) << "Unexpected opcode: " << opcode;
  }
  // At this point, target will be either float or double.
  DCHECK(rl_dest.fp);
  if (rl_src.wide) {
    rl_src = LoadValueWide(rl_src, rcSrc);
  } else {
    rl_src = LoadValue(rl_src, rcSrc);
  }
  rl_result = EvalLoc(rl_dest, kFPReg, true);
  NewLIR2(op, rl_result.reg.GetReg(), rl_src.reg.GetReg());
  if (rl_dest.wide) {
    StoreValueWide(rl_dest, rl_result);
  } else {
    StoreValue(rl_dest, rl_result);
  }
}

void X86Mir2Lir::GenRemFP(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2, bool is_double) {
  // Compute offsets to the source and destination VRs on stack.
  int src1_v_reg_offset = SRegOffset(rl_src1.s_reg_low);
  int src2_v_reg_offset = SRegOffset(rl_src2.s_reg_low);
  int dest_v_reg_offset = SRegOffset(rl_dest.s_reg_low);

  // Update the in-register state of sources.
  rl_src1 = is_double ? UpdateLocWide(rl_src1) : UpdateLoc(rl_src1);
  rl_src2 = is_double ? UpdateLocWide(rl_src2) : UpdateLoc(rl_src2);

  // All memory accesses below reference dalvik regs.
  ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);

  // If the source is in physical register, then put it in its location on stack.
  if (rl_src1.location == kLocPhysReg) {
    RegisterInfo* reg_info = GetRegInfo(rl_src1.reg);

    if (reg_info != nullptr && reg_info->IsTemp()) {
      // Calling FlushSpecificReg because it will only write back VR if it is dirty.
      FlushSpecificReg(reg_info);
      // ResetDef to prevent NullifyRange from removing stores.
      ResetDef(rl_src1.reg);
    } else {
      // It must have been register promoted if it is not a temp but is still in physical
      // register. Since we need it to be in memory to convert, we place it there now.
      StoreBaseDisp(rs_rX86_SP, src1_v_reg_offset, rl_src1.reg, is_double ? k64 : k32,
                    kNotVolatile);
    }
  }

  if (rl_src2.location == kLocPhysReg) {
    RegisterInfo* reg_info = GetRegInfo(rl_src2.reg);
    if (reg_info != nullptr && reg_info->IsTemp()) {
      FlushSpecificReg(reg_info);
      ResetDef(rl_src2.reg);
    } else {
      StoreBaseDisp(rs_rX86_SP, src2_v_reg_offset, rl_src2.reg, is_double ? k64 : k32,
                    kNotVolatile);
    }
  }

  int fld_opcode = is_double ? kX86Fld64M : kX86Fld32M;

  // Push the source virtual registers onto the x87 stack.
  LIR *fld_2 = NewLIR2NoDest(fld_opcode, rs_rX86_SP.GetReg(),
                             src2_v_reg_offset + LOWORD_OFFSET);
  AnnotateDalvikRegAccess(fld_2, (src2_v_reg_offset + LOWORD_OFFSET) >> 2,
                          true /* is_load */, is_double /* is64bit */);

  LIR *fld_1 = NewLIR2NoDest(fld_opcode, rs_rX86_SP.GetReg(),
                             src1_v_reg_offset + LOWORD_OFFSET);
  AnnotateDalvikRegAccess(fld_1, (src1_v_reg_offset + LOWORD_OFFSET) >> 2,
                          true /* is_load */, is_double /* is64bit */);

  FlushReg(rs_rAX);
  Clobber(rs_rAX);
  LockTemp(rs_rAX);

  LIR* retry = NewLIR0(kPseudoTargetLabel);

  // Divide ST(0) by ST(1) and place result to ST(0).
  NewLIR0(kX86Fprem);

  // Move FPU status word to AX.
  NewLIR0(kX86Fstsw16R);

  // Check if reduction is complete.
  OpRegImm(kOpAnd, rs_rAX, 0x400);

  // If no then continue to compute remainder.
  LIR* branch = NewLIR2(kX86Jcc8, 0, kX86CondNe);
  branch->target = retry;

  FreeTemp(rs_rAX);

  // Now store result in the destination VR's stack location.
  int displacement = dest_v_reg_offset + LOWORD_OFFSET;
  int opcode = is_double ? kX86Fst64M : kX86Fst32M;
  LIR *fst = NewLIR2NoDest(opcode, rs_rX86_SP.GetReg(), displacement);
  AnnotateDalvikRegAccess(fst, displacement >> 2, false /* is_load */, is_double /* is64bit */);

  // Pop ST(1) and ST(0).
  NewLIR0(kX86Fucompp);

  /*
   * The result is in a physical register if it was in a temp or was register
   * promoted. For that reason it is enough to check if it is in physical
   * register. If it is, then we must do all of the bookkeeping necessary to
   * invalidate temp (if needed) and load in promoted register (if needed).
   * If the result's location is in memory, then we do not need to do anything
   * more since the fstp has already placed the correct value in memory.
   */
  RegLocation rl_result = is_double ? UpdateLocWideTyped(rl_dest, kFPReg) :
      UpdateLocTyped(rl_dest, kFPReg);
  if (rl_result.location == kLocPhysReg) {
    rl_result = EvalLoc(rl_dest, kFPReg, true);
    if (is_double) {
      LoadBaseDisp(rs_rX86_SP, dest_v_reg_offset, rl_result.reg, k64, kNotVolatile);
      StoreFinalValueWide(rl_dest, rl_result);
    } else {
      Load32Disp(rs_rX86_SP, dest_v_reg_offset, rl_result.reg);
      StoreFinalValue(rl_dest, rl_result);
    }
  }
}

void X86Mir2Lir::GenCmpFP(Instruction::Code code, RegLocation rl_dest,
                          RegLocation rl_src1, RegLocation rl_src2) {
  bool single = (code == Instruction::CMPL_FLOAT) || (code == Instruction::CMPG_FLOAT);
  bool unordered_gt = (code == Instruction::CMPG_DOUBLE) || (code == Instruction::CMPG_FLOAT);
  if (single) {
    rl_src1 = LoadValue(rl_src1, kFPReg);
    rl_src2 = LoadValue(rl_src2, kFPReg);
  } else {
    rl_src1 = LoadValueWide(rl_src1, kFPReg);
    rl_src2 = LoadValueWide(rl_src2, kFPReg);
  }
  // In case result vreg is also src vreg, break association to avoid useless copy by EvalLoc()
  ClobberSReg(rl_dest.s_reg_low);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  LoadConstantNoClobber(rl_result.reg, unordered_gt ? 1 : 0);
  if (single) {
    NewLIR2(kX86UcomissRR, rl_src1.reg.GetReg(), rl_src2.reg.GetReg());
  } else {
    NewLIR2(kX86UcomisdRR, rl_src1.reg.GetReg(), rl_src2.reg.GetReg());
  }
  LIR* branch = NULL;
  if (unordered_gt) {
    branch = NewLIR2(kX86Jcc8, 0, kX86CondPE);
  }
  // If the result reg can't be byte accessed, use a jump and move instead of a set.
  if (!IsByteRegister(rl_result.reg)) {
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
    NewLIR2(kX86UcomisdRR, rl_src1.reg.GetReg(), rl_src2.reg.GetReg());
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
  OpRegRegImm(kOpAdd, rl_result.reg, rl_src.reg, 0x80000000);
  StoreValue(rl_dest, rl_result);
}

void X86Mir2Lir::GenNegDouble(RegLocation rl_dest, RegLocation rl_src) {
  RegLocation rl_result;
  rl_src = LoadValueWide(rl_src, kCoreReg);
  rl_result = EvalLocWide(rl_dest, kCoreReg, true);
  if (cu_->target64) {
    OpRegCopy(rl_result.reg, rl_src.reg);
    // Flip sign bit.
    NewLIR2(kX86Rol64RI, rl_result.reg.GetReg(), 1);
    NewLIR2(kX86Xor64RI, rl_result.reg.GetReg(), 1);
    NewLIR2(kX86Ror64RI, rl_result.reg.GetReg(), 1);
  } else {
    OpRegRegImm(kOpAdd, rl_result.reg.GetHigh(), rl_src.reg.GetHigh(), 0x80000000);
    OpRegCopy(rl_result.reg, rl_src.reg);
  }
  StoreValueWide(rl_dest, rl_result);
}

bool X86Mir2Lir::GenInlinedSqrt(CallInfo* info) {
  RegLocation rl_src = info->args[0];
  RegLocation rl_dest = InlineTargetWide(info);  // double place for result
  rl_src = LoadValueWide(rl_src, kFPReg);
  RegLocation rl_result = EvalLoc(rl_dest, kFPReg, true);
  NewLIR2(kX86SqrtsdRR, rl_result.reg.GetReg(), rl_src.reg.GetReg());
  StoreValueWide(rl_dest, rl_result);
  return true;
}

bool X86Mir2Lir::GenInlinedAbsFloat(CallInfo* info) {
  // Get the argument
  RegLocation rl_src = info->args[0];

  // Get the inlined intrinsic target virtual register
  RegLocation rl_dest = InlineTarget(info);

  // Get the virtual register number
  DCHECK_NE(rl_src.s_reg_low, INVALID_SREG);
  if (rl_dest.s_reg_low == INVALID_SREG) {
    // Result is unused, the code is dead. Inlining successful, no code generated.
    return true;
  }
  int v_src_reg = mir_graph_->SRegToVReg(rl_src.s_reg_low);
  int v_dst_reg = mir_graph_->SRegToVReg(rl_dest.s_reg_low);

  // if argument is the same as inlined intrinsic target
  if (v_src_reg == v_dst_reg) {
    rl_src = UpdateLoc(rl_src);

    // if argument is in the physical register
    if (rl_src.location == kLocPhysReg) {
      rl_src = LoadValue(rl_src, kCoreReg);
      OpRegImm(kOpAnd, rl_src.reg, 0x7fffffff);
      StoreValue(rl_dest, rl_src);
      return true;
    }
    // the argument is in memory
    DCHECK((rl_src.location == kLocDalvikFrame) ||
         (rl_src.location == kLocCompilerTemp));

    // Operate directly into memory.
    int displacement = SRegOffset(rl_dest.s_reg_low);
    ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
    LIR *lir = NewLIR3(kX86And32MI, rs_rX86_SP.GetReg(), displacement, 0x7fffffff);
    AnnotateDalvikRegAccess(lir, displacement >> 2, false /*is_load */, false /* is_64bit */);
    AnnotateDalvikRegAccess(lir, displacement >> 2, true /* is_load */, false /* is_64bit*/);
    return true;
  } else {
    rl_src = LoadValue(rl_src, kCoreReg);
    RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
    OpRegRegImm(kOpAnd, rl_result.reg, rl_src.reg, 0x7fffffff);
    StoreValue(rl_dest, rl_result);
    return true;
  }
}

bool X86Mir2Lir::GenInlinedAbsDouble(CallInfo* info) {
  RegLocation rl_src = info->args[0];
  RegLocation rl_dest = InlineTargetWide(info);
  DCHECK_NE(rl_src.s_reg_low, INVALID_SREG);
  if (rl_dest.s_reg_low == INVALID_SREG) {
    // Result is unused, the code is dead. Inlining successful, no code generated.
    return true;
  }
  if (cu_->target64) {
    rl_src = LoadValueWide(rl_src, kCoreReg);
    RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
    OpRegCopyWide(rl_result.reg, rl_src.reg);
    OpRegImm(kOpLsl, rl_result.reg, 1);
    OpRegImm(kOpLsr, rl_result.reg, 1);
    StoreValueWide(rl_dest, rl_result);
    return true;
  }
  int v_src_reg = mir_graph_->SRegToVReg(rl_src.s_reg_low);
  int v_dst_reg = mir_graph_->SRegToVReg(rl_dest.s_reg_low);
  rl_src = UpdateLocWide(rl_src);

  // if argument is in the physical XMM register
  if (rl_src.location == kLocPhysReg && rl_src.reg.IsFloat()) {
    RegLocation rl_result = EvalLoc(rl_dest, kFPReg, true);
    if (rl_result.reg != rl_src.reg) {
      LoadConstantWide(rl_result.reg, 0x7fffffffffffffff);
      NewLIR2(kX86PandRR, rl_result.reg.GetReg(), rl_src.reg.GetReg());
    } else {
      RegStorage sign_mask = AllocTempDouble();
      LoadConstantWide(sign_mask, 0x7fffffffffffffff);
      NewLIR2(kX86PandRR, rl_result.reg.GetReg(), sign_mask.GetReg());
      FreeTemp(sign_mask);
    }
    StoreValueWide(rl_dest, rl_result);
    return true;
  } else if (v_src_reg == v_dst_reg) {
    // if argument is the same as inlined intrinsic target
    // if argument is in the physical register
    if (rl_src.location == kLocPhysReg) {
      rl_src = LoadValueWide(rl_src, kCoreReg);
      OpRegImm(kOpAnd, rl_src.reg.GetHigh(), 0x7fffffff);
      StoreValueWide(rl_dest, rl_src);
      return true;
    }
    // the argument is in memory
    DCHECK((rl_src.location == kLocDalvikFrame) ||
           (rl_src.location == kLocCompilerTemp));

    // Operate directly into memory.
    int displacement = SRegOffset(rl_dest.s_reg_low);
    ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
    LIR *lir = NewLIR3(kX86And32MI, rs_rX86_SP.GetReg(), displacement  + HIWORD_OFFSET, 0x7fffffff);
    AnnotateDalvikRegAccess(lir, (displacement + HIWORD_OFFSET) >> 2, true /* is_load */, true /* is_64bit*/);
    AnnotateDalvikRegAccess(lir, (displacement + HIWORD_OFFSET) >> 2, false /*is_load */, true /* is_64bit */);
    return true;
  } else {
    rl_src = LoadValueWide(rl_src, kCoreReg);
    RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
    OpRegCopyWide(rl_result.reg, rl_src.reg);
    OpRegImm(kOpAnd, rl_result.reg.GetHigh(), 0x7fffffff);
    StoreValueWide(rl_dest, rl_result);
    return true;
  }
}

bool X86Mir2Lir::GenInlinedMinMaxFP(CallInfo* info, bool is_min, bool is_double) {
  if (is_double) {
    RegLocation rl_src1 = LoadValueWide(info->args[0], kFPReg);
    RegLocation rl_src2 = LoadValueWide(info->args[2], kFPReg);
    RegLocation rl_dest = InlineTargetWide(info);
    RegLocation rl_result = EvalLocWide(rl_dest, kFPReg, true);

    // Avoid src2 corruption by OpRegCopyWide.
    if (rl_result.reg == rl_src2.reg) {
        std::swap(rl_src2.reg, rl_src1.reg);
    }

    OpRegCopyWide(rl_result.reg, rl_src1.reg);
    NewLIR2(kX86UcomisdRR, rl_result.reg.GetReg(), rl_src2.reg.GetReg());
    // If either arg is NaN, return NaN.
    LIR* branch_nan = NewLIR2(kX86Jcc8, 0, kX86CondP);
    // Min/Max branches.
    LIR* branch_cond1 = NewLIR2(kX86Jcc8, 0, (is_min) ? kX86CondA : kX86CondB);
    LIR* branch_cond2 = NewLIR2(kX86Jcc8, 0, (is_min) ? kX86CondB : kX86CondA);
    // If equal, we need to resolve situations like min/max(0.0, -0.0) == -0.0/0.0.
    NewLIR2((is_min) ? kX86OrpdRR : kX86AndpdRR, rl_result.reg.GetReg(), rl_src2.reg.GetReg());
    LIR* branch_exit_equal = NewLIR1(kX86Jmp8, 0);
    // Handle NaN.
    branch_nan->target = NewLIR0(kPseudoTargetLabel);
    LoadConstantWide(rl_result.reg, INT64_C(0x7ff8000000000000));
    LIR* branch_exit_nan = NewLIR1(kX86Jmp8, 0);
    // Handle Min/Max. Copy greater/lesser value from src2.
    branch_cond1->target = NewLIR0(kPseudoTargetLabel);
    OpRegCopyWide(rl_result.reg, rl_src2.reg);
    // Right operand is already in result reg.
    branch_cond2->target = NewLIR0(kPseudoTargetLabel);
    // Exit.
    branch_exit_nan->target = NewLIR0(kPseudoTargetLabel);
    branch_exit_equal->target = NewLIR0(kPseudoTargetLabel);
    StoreValueWide(rl_dest, rl_result);
  } else {
    RegLocation rl_src1 = LoadValue(info->args[0], kFPReg);
    RegLocation rl_src2 = LoadValue(info->args[1], kFPReg);
    RegLocation rl_dest = InlineTarget(info);
    RegLocation rl_result = EvalLoc(rl_dest, kFPReg, true);

    // Avoid src2 corruption by OpRegCopyWide.
    if (rl_result.reg == rl_src2.reg) {
        std::swap(rl_src2.reg, rl_src1.reg);
    }

    OpRegCopy(rl_result.reg, rl_src1.reg);
    NewLIR2(kX86UcomissRR, rl_result.reg.GetReg(), rl_src2.reg.GetReg());
    // If either arg is NaN, return NaN.
    LIR* branch_nan = NewLIR2(kX86Jcc8, 0, kX86CondP);
    // Min/Max branches.
    LIR* branch_cond1 = NewLIR2(kX86Jcc8, 0, (is_min) ? kX86CondA : kX86CondB);
    LIR* branch_cond2 = NewLIR2(kX86Jcc8, 0, (is_min) ? kX86CondB : kX86CondA);
    // If equal, we need to resolve situations like min/max(0.0, -0.0) == -0.0/0.0.
    NewLIR2((is_min) ? kX86OrpsRR : kX86AndpsRR, rl_result.reg.GetReg(), rl_src2.reg.GetReg());
    LIR* branch_exit_equal = NewLIR1(kX86Jmp8, 0);
    // Handle NaN.
    branch_nan->target = NewLIR0(kPseudoTargetLabel);
    LoadConstantNoClobber(rl_result.reg, 0x7fc00000);
    LIR* branch_exit_nan = NewLIR1(kX86Jmp8, 0);
    // Handle Min/Max. Copy greater/lesser value from src2.
    branch_cond1->target = NewLIR0(kPseudoTargetLabel);
    OpRegCopy(rl_result.reg, rl_src2.reg);
    // Right operand is already in result reg.
    branch_cond2->target = NewLIR0(kPseudoTargetLabel);
    // Exit.
    branch_exit_nan->target = NewLIR0(kPseudoTargetLabel);
    branch_exit_equal->target = NewLIR0(kPseudoTargetLabel);
    StoreValue(rl_dest, rl_result);
  }
  return true;
}

}  // namespace art
