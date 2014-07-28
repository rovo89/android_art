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

#include "codegen_mips.h"
#include "dex/quick/mir_to_lir-inl.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "mips_lir.h"

namespace art {

void MipsMir2Lir::GenArithOpFloat(Instruction::Code opcode,
                                  RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2) {
  int op = kMipsNop;
  RegLocation rl_result;

  /*
   * Don't attempt to optimize register usage since these opcodes call out to
   * the handlers.
   */
  switch (opcode) {
    case Instruction::ADD_FLOAT_2ADDR:
    case Instruction::ADD_FLOAT:
      op = kMipsFadds;
      break;
    case Instruction::SUB_FLOAT_2ADDR:
    case Instruction::SUB_FLOAT:
      op = kMipsFsubs;
      break;
    case Instruction::DIV_FLOAT_2ADDR:
    case Instruction::DIV_FLOAT:
      op = kMipsFdivs;
      break;
    case Instruction::MUL_FLOAT_2ADDR:
    case Instruction::MUL_FLOAT:
      op = kMipsFmuls;
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

void MipsMir2Lir::GenArithOpDouble(Instruction::Code opcode,
                                   RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2) {
  int op = kMipsNop;
  RegLocation rl_result;

  switch (opcode) {
    case Instruction::ADD_DOUBLE_2ADDR:
    case Instruction::ADD_DOUBLE:
      op = kMipsFaddd;
      break;
    case Instruction::SUB_DOUBLE_2ADDR:
    case Instruction::SUB_DOUBLE:
      op = kMipsFsubd;
      break;
    case Instruction::DIV_DOUBLE_2ADDR:
    case Instruction::DIV_DOUBLE:
      op = kMipsFdivd;
      break;
    case Instruction::MUL_DOUBLE_2ADDR:
    case Instruction::MUL_DOUBLE:
      op = kMipsFmuld;
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
      LOG(FATAL) << "Unpexpected opcode: " << opcode;
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

void MipsMir2Lir::GenConversion(Instruction::Code opcode, RegLocation rl_dest,
                                RegLocation rl_src) {
  int op = kMipsNop;
  RegLocation rl_result;
  switch (opcode) {
    case Instruction::INT_TO_FLOAT:
      op = kMipsFcvtsw;
      break;
    case Instruction::DOUBLE_TO_FLOAT:
      op = kMipsFcvtsd;
      break;
    case Instruction::FLOAT_TO_DOUBLE:
      op = kMipsFcvtds;
      break;
    case Instruction::INT_TO_DOUBLE:
      op = kMipsFcvtdw;
      break;
    case Instruction::FLOAT_TO_INT:
      GenConversionCall(kQuickF2iz, rl_dest, rl_src);
      return;
    case Instruction::DOUBLE_TO_INT:
      GenConversionCall(kQuickD2iz, rl_dest, rl_src);
      return;
    case Instruction::LONG_TO_DOUBLE:
      GenConversionCall(kQuickL2d, rl_dest, rl_src);
      return;
    case Instruction::FLOAT_TO_LONG:
      GenConversionCall(kQuickF2l, rl_dest, rl_src);
      return;
    case Instruction::LONG_TO_FLOAT:
      GenConversionCall(kQuickL2f, rl_dest, rl_src);
      return;
    case Instruction::DOUBLE_TO_LONG:
      GenConversionCall(kQuickD2l, rl_dest, rl_src);
      return;
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

void MipsMir2Lir::GenCmpFP(Instruction::Code opcode, RegLocation rl_dest,
                           RegLocation rl_src1, RegLocation rl_src2) {
  bool wide = true;
  QuickEntrypointEnum target;

  switch (opcode) {
    case Instruction::CMPL_FLOAT:
      target = kQuickCmplFloat;
      wide = false;
      break;
    case Instruction::CMPG_FLOAT:
      target = kQuickCmpgFloat;
      wide = false;
      break;
    case Instruction::CMPL_DOUBLE:
      target = kQuickCmplDouble;
      break;
    case Instruction::CMPG_DOUBLE:
      target = kQuickCmpgDouble;
      break;
    default:
      LOG(FATAL) << "Unexpected opcode: " << opcode;
      target = kQuickCmplFloat;
  }
  FlushAllRegs();
  LockCallTemps();
  if (wide) {
    RegStorage r_tmp1(RegStorage::k64BitPair, rMIPS_FARG0, rMIPS_FARG1);
    RegStorage r_tmp2(RegStorage::k64BitPair, rMIPS_FARG2, rMIPS_FARG3);
    LoadValueDirectWideFixed(rl_src1, r_tmp1);
    LoadValueDirectWideFixed(rl_src2, r_tmp2);
  } else {
    LoadValueDirectFixed(rl_src1, rs_rMIPS_FARG0);
    LoadValueDirectFixed(rl_src2, rs_rMIPS_FARG2);
  }
  RegStorage r_tgt = LoadHelper(target);
  // NOTE: not a safepoint
  OpReg(kOpBlx, r_tgt);
  RegLocation rl_result = GetReturn(kCoreReg);
  StoreValue(rl_dest, rl_result);
}

void MipsMir2Lir::GenFusedFPCmpBranch(BasicBlock* bb, MIR* mir,
                                bool gt_bias, bool is_double) {
  UNIMPLEMENTED(FATAL) << "Need codegen for fused fp cmp branch";
}

void MipsMir2Lir::GenNegFloat(RegLocation rl_dest, RegLocation rl_src) {
  RegLocation rl_result;
  rl_src = LoadValue(rl_src, kCoreReg);
  rl_result = EvalLoc(rl_dest, kCoreReg, true);
  OpRegRegImm(kOpAdd, rl_result.reg, rl_src.reg, 0x80000000);
  StoreValue(rl_dest, rl_result);
}

void MipsMir2Lir::GenNegDouble(RegLocation rl_dest, RegLocation rl_src) {
  RegLocation rl_result;
  rl_src = LoadValueWide(rl_src, kCoreReg);
  rl_result = EvalLoc(rl_dest, kCoreReg, true);
  OpRegRegImm(kOpAdd, rl_result.reg.GetHigh(), rl_src.reg.GetHigh(), 0x80000000);
  OpRegCopy(rl_result.reg, rl_src.reg);
  StoreValueWide(rl_dest, rl_result);
}

bool MipsMir2Lir::GenInlinedMinMax(CallInfo* info, bool is_min, bool is_long) {
  // TODO: need Mips implementation
  return false;
}

}  // namespace art
