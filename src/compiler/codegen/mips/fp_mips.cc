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

#include "oat/runtime/oat_support_entrypoints.h"
#include "mips_lir.h"
#include "../codegen_util.h"
#include "../ralloc_util.h"

namespace art {

bool GenArithOpFloat(CompilationUnit *cu, Instruction::Code opcode, RegLocation rl_dest,
                     RegLocation rl_src1, RegLocation rl_src2)
{
#ifdef __mips_hard_float
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
    case Instruction::NEG_FLOAT: {
      return GenArithOpFloatPortable(cu, opcode, rl_dest, rl_src1, rl_src2);
    }
    default:
      return true;
  }
  rl_src1 = LoadValue(cu, rl_src1, kFPReg);
  rl_src2 = LoadValue(cu, rl_src2, kFPReg);
  rl_result = EvalLoc(cu, rl_dest, kFPReg, true);
  NewLIR3(cu, op, rl_result.low_reg, rl_src1.low_reg, rl_src2.low_reg);
  StoreValue(cu, rl_dest, rl_result);

  return false;
#else
  return GenArithOpFloatPortable(cu, opcode, rl_dest, rl_src1, rl_src2);
#endif
}

bool GenArithOpDouble(CompilationUnit *cu, Instruction::Code opcode,
                      RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2)
{
#ifdef __mips_hard_float
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
    case Instruction::NEG_DOUBLE: {
      return GenArithOpDoublePortable(cu, opcode, rl_dest, rl_src1, rl_src2);
    }
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
  NewLIR3(cu, op, S2d(rl_result.low_reg, rl_result.high_reg), S2d(rl_src1.low_reg, rl_src1.high_reg),
          S2d(rl_src2.low_reg, rl_src2.high_reg));
  StoreValueWide(cu, rl_dest, rl_result);
  return false;
#else
  return GenArithOpDoublePortable(cu, opcode, rl_dest, rl_src1, rl_src2);
#endif
}

bool GenConversion(CompilationUnit *cu, Instruction::Code opcode, RegLocation rl_dest,
                   RegLocation rl_src)
{
#ifdef __mips_hard_float
  int op = kMipsNop;
  int src_reg;
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
    case Instruction::DOUBLE_TO_INT:
    case Instruction::LONG_TO_DOUBLE:
    case Instruction::FLOAT_TO_LONG:
    case Instruction::LONG_TO_FLOAT:
    case Instruction::DOUBLE_TO_LONG:
      return GenConversionPortable(cu, opcode, rl_dest, rl_src);
    default:
      return true;
  }
  if (rl_src.wide) {
    rl_src = LoadValueWide(cu, rl_src, kFPReg);
    src_reg = S2d(rl_src.low_reg, rl_src.high_reg);
  } else {
    rl_src = LoadValue(cu, rl_src, kFPReg);
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
#else
  return GenConversionPortable(cu, opcode, rl_dest, rl_src);
#endif
}

bool GenCmpFP(CompilationUnit *cu, Instruction::Code opcode, RegLocation rl_dest,
              RegLocation rl_src1, RegLocation rl_src2)
{
  bool wide = true;
  int offset;

  switch (opcode) {
    case Instruction::CMPL_FLOAT:
      offset = ENTRYPOINT_OFFSET(pCmplFloat);
      wide = false;
      break;
    case Instruction::CMPG_FLOAT:
      offset = ENTRYPOINT_OFFSET(pCmpgFloat);
      wide = false;
      break;
    case Instruction::CMPL_DOUBLE:
      offset = ENTRYPOINT_OFFSET(pCmplDouble);
      break;
    case Instruction::CMPG_DOUBLE:
      offset = ENTRYPOINT_OFFSET(pCmpgDouble);
      break;
    default:
      return true;
  }
  FlushAllRegs(cu);
  LockCallTemps(cu);
  if (wide) {
    LoadValueDirectWideFixed(cu, rl_src1, rMIPS_FARG0, rMIPS_FARG1);
    LoadValueDirectWideFixed(cu, rl_src2, rMIPS_FARG2, rMIPS_FARG3);
  } else {
    LoadValueDirectFixed(cu, rl_src1, rMIPS_FARG0);
    LoadValueDirectFixed(cu, rl_src2, rMIPS_FARG2);
  }
  int r_tgt = LoadHelper(cu, offset);
  // NOTE: not a safepoint
  OpReg(cu, kOpBlx, r_tgt);
  RegLocation rl_result = GetReturn(cu, false);
  StoreValue(cu, rl_dest, rl_result);
  return false;
}

void GenFusedFPCmpBranch(CompilationUnit* cu, BasicBlock* bb, MIR* mir,
                                bool gt_bias, bool is_double)
{
  UNIMPLEMENTED(FATAL) << "Need codegen for fused fp cmp branch";
}

void GenNegFloat(CompilationUnit *cu, RegLocation rl_dest, RegLocation rl_src)
{
  RegLocation rl_result;
  rl_src = LoadValue(cu, rl_src, kCoreReg);
  rl_result = EvalLoc(cu, rl_dest, kCoreReg, true);
  OpRegRegImm(cu, kOpAdd, rl_result.low_reg, rl_src.low_reg, 0x80000000);
  StoreValue(cu, rl_dest, rl_result);
}

void GenNegDouble(CompilationUnit *cu, RegLocation rl_dest, RegLocation rl_src)
{
  RegLocation rl_result;
  rl_src = LoadValueWide(cu, rl_src, kCoreReg);
  rl_result = EvalLoc(cu, rl_dest, kCoreReg, true);
  OpRegRegImm(cu, kOpAdd, rl_result.high_reg, rl_src.high_reg, 0x80000000);
  OpRegCopy(cu, rl_result.low_reg, rl_src.low_reg);
  StoreValueWide(cu, rl_dest, rl_result);
}

bool GenInlinedMinMaxInt(CompilationUnit *cu, CallInfo* info, bool is_min)
{
  // TODO: need Mips implementation
  return false;
}

} //  namespace art
