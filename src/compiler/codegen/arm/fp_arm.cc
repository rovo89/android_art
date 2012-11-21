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
#include "../codegen_util.h"
#include "../ralloc_util.h"

namespace art {

bool GenArithOpFloat(CompilationUnit* cUnit, Instruction::Code opcode, RegLocation rlDest,
                     RegLocation rlSrc1, RegLocation rlSrc2)
{
  int op = kThumbBkpt;
  RegLocation rlResult;

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
    case Instruction::NEG_FLOAT: {
      return GenArithOpFloatPortable(cUnit, opcode, rlDest, rlSrc1, rlSrc2);
    }
    default:
      return true;
  }
  rlSrc1 = LoadValue(cUnit, rlSrc1, kFPReg);
  rlSrc2 = LoadValue(cUnit, rlSrc2, kFPReg);
  rlResult = EvalLoc(cUnit, rlDest, kFPReg, true);
  NewLIR3(cUnit, op, rlResult.lowReg, rlSrc1.lowReg, rlSrc2.lowReg);
  StoreValue(cUnit, rlDest, rlResult);
  return false;
}

bool GenArithOpDouble(CompilationUnit* cUnit, Instruction::Code opcode,
                      RegLocation rlDest, RegLocation rlSrc1, RegLocation rlSrc2)
{
  int op = kThumbBkpt;
  RegLocation rlResult;

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
    case Instruction::NEG_DOUBLE: {
      return GenArithOpDoublePortable(cUnit, opcode, rlDest, rlSrc1, rlSrc2);
    }
    default:
      return true;
  }

  rlSrc1 = LoadValueWide(cUnit, rlSrc1, kFPReg);
  DCHECK(rlSrc1.wide);
  rlSrc2 = LoadValueWide(cUnit, rlSrc2, kFPReg);
  DCHECK(rlSrc2.wide);
  rlResult = EvalLoc(cUnit, rlDest, kFPReg, true);
  DCHECK(rlDest.wide);
  DCHECK(rlResult.wide);
  NewLIR3(cUnit, op, S2d(rlResult.lowReg, rlResult.highReg), S2d(rlSrc1.lowReg, rlSrc1.highReg),
          S2d(rlSrc2.lowReg, rlSrc2.highReg));
  StoreValueWide(cUnit, rlDest, rlResult);
  return false;
}

bool GenConversion(CompilationUnit* cUnit, Instruction::Code opcode,
                   RegLocation rlDest, RegLocation rlSrc)
{
  int op = kThumbBkpt;
  int srcReg;
  RegLocation rlResult;

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
      op = kThumb2VcvtID;
      break;
    case Instruction::DOUBLE_TO_INT:
      op = kThumb2VcvtDI;
      break;
    case Instruction::LONG_TO_DOUBLE:
    case Instruction::FLOAT_TO_LONG:
    case Instruction::LONG_TO_FLOAT:
    case Instruction::DOUBLE_TO_LONG:
      return GenConversionPortable(cUnit, opcode, rlDest, rlSrc);
    default:
      return true;
  }
  if (rlSrc.wide) {
    rlSrc = LoadValueWide(cUnit, rlSrc, kFPReg);
    srcReg = S2d(rlSrc.lowReg, rlSrc.highReg);
  } else {
    rlSrc = LoadValue(cUnit, rlSrc, kFPReg);
    srcReg = rlSrc.lowReg;
  }
  if (rlDest.wide) {
    rlResult = EvalLoc(cUnit, rlDest, kFPReg, true);
    NewLIR2(cUnit, op, S2d(rlResult.lowReg, rlResult.highReg), srcReg);
    StoreValueWide(cUnit, rlDest, rlResult);
  } else {
    rlResult = EvalLoc(cUnit, rlDest, kFPReg, true);
    NewLIR2(cUnit, op, rlResult.lowReg, srcReg);
    StoreValue(cUnit, rlDest, rlResult);
  }
  return false;
}

void GenFusedFPCmpBranch(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir,
                         bool gtBias, bool isDouble)
{
  LIR* labelList = cUnit->blockLabelList;
  LIR* target = &labelList[bb->taken->id];
  RegLocation rlSrc1;
  RegLocation rlSrc2;
  if (isDouble) {
    rlSrc1 = GetSrcWide(cUnit, mir, 0);
    rlSrc2 = GetSrcWide(cUnit, mir, 2);
    rlSrc1 = LoadValueWide(cUnit, rlSrc1, kFPReg);
    rlSrc2 = LoadValueWide(cUnit, rlSrc2, kFPReg);
    NewLIR2(cUnit, kThumb2Vcmpd, S2d(rlSrc1.lowReg, rlSrc2.highReg),
            S2d(rlSrc2.lowReg, rlSrc2.highReg));
  } else {
    rlSrc1 = GetSrc(cUnit, mir, 0);
    rlSrc2 = GetSrc(cUnit, mir, 1);
    rlSrc1 = LoadValue(cUnit, rlSrc1, kFPReg);
    rlSrc2 = LoadValue(cUnit, rlSrc2, kFPReg);
    NewLIR2(cUnit, kThumb2Vcmps, rlSrc1.lowReg, rlSrc2.lowReg);
  }
  NewLIR0(cUnit, kThumb2Fmstat);
  ConditionCode ccode = static_cast<ConditionCode>(mir->dalvikInsn.arg[0]);
  switch(ccode) {
    case kCondEq:
    case kCondNe:
      break;
    case kCondLt:
      if (gtBias) {
        ccode = kCondMi;
      }
      break;
    case kCondLe:
      if (gtBias) {
        ccode = kCondLs;
      }
      break;
    case kCondGt:
      if (gtBias) {
        ccode = kCondHi;
      }
      break;
    case kCondGe:
      if (gtBias) {
        ccode = kCondCs;
      }
      break;
    default:
      LOG(FATAL) << "Unexpected ccode: " << ccode;
  }
  OpCondBranch(cUnit, ccode, target);
}


bool GenCmpFP(CompilationUnit* cUnit, Instruction::Code opcode, RegLocation rlDest,
        RegLocation rlSrc1, RegLocation rlSrc2)
{
  bool isDouble;
  int defaultResult;
  RegLocation rlResult;

  switch (opcode) {
    case Instruction::CMPL_FLOAT:
      isDouble = false;
      defaultResult = -1;
      break;
    case Instruction::CMPG_FLOAT:
      isDouble = false;
      defaultResult = 1;
      break;
    case Instruction::CMPL_DOUBLE:
      isDouble = true;
      defaultResult = -1;
      break;
    case Instruction::CMPG_DOUBLE:
      isDouble = true;
      defaultResult = 1;
      break;
    default:
      return true;
  }
  if (isDouble) {
    rlSrc1 = LoadValueWide(cUnit, rlSrc1, kFPReg);
    rlSrc2 = LoadValueWide(cUnit, rlSrc2, kFPReg);
    ClobberSReg(cUnit, rlDest.sRegLow);
    rlResult = EvalLoc(cUnit, rlDest, kCoreReg, true);
    LoadConstant(cUnit, rlResult.lowReg, defaultResult);
    NewLIR2(cUnit, kThumb2Vcmpd, S2d(rlSrc1.lowReg, rlSrc2.highReg),
            S2d(rlSrc2.lowReg, rlSrc2.highReg));
  } else {
    rlSrc1 = LoadValue(cUnit, rlSrc1, kFPReg);
    rlSrc2 = LoadValue(cUnit, rlSrc2, kFPReg);
    ClobberSReg(cUnit, rlDest.sRegLow);
    rlResult = EvalLoc(cUnit, rlDest, kCoreReg, true);
    LoadConstant(cUnit, rlResult.lowReg, defaultResult);
    NewLIR2(cUnit, kThumb2Vcmps, rlSrc1.lowReg, rlSrc2.lowReg);
  }
  DCHECK(!ARM_FPREG(rlResult.lowReg));
  NewLIR0(cUnit, kThumb2Fmstat);

  OpIT(cUnit, (defaultResult == -1) ? kArmCondGt : kArmCondMi, "");
  NewLIR2(cUnit, kThumb2MovImmShift, rlResult.lowReg,
          ModifiedImmediate(-defaultResult)); // Must not alter ccodes
  GenBarrier(cUnit);

  OpIT(cUnit, kArmCondEq, "");
  LoadConstant(cUnit, rlResult.lowReg, 0);
  GenBarrier(cUnit);

  StoreValue(cUnit, rlDest, rlResult);
  return false;
}

void GenNegFloat(CompilationUnit* cUnit, RegLocation rlDest, RegLocation rlSrc)
{
  RegLocation rlResult;
  rlSrc = LoadValue(cUnit, rlSrc, kFPReg);
  rlResult = EvalLoc(cUnit, rlDest, kFPReg, true);
  NewLIR2(cUnit, kThumb2Vnegs, rlResult.lowReg, rlSrc.lowReg);
  StoreValue(cUnit, rlDest, rlResult);
}

void GenNegDouble(CompilationUnit* cUnit, RegLocation rlDest, RegLocation rlSrc)
{
  RegLocation rlResult;
  rlSrc = LoadValueWide(cUnit, rlSrc, kFPReg);
  rlResult = EvalLoc(cUnit, rlDest, kFPReg, true);
  NewLIR2(cUnit, kThumb2Vnegd, S2d(rlResult.lowReg, rlResult.highReg),
          S2d(rlSrc.lowReg, rlSrc.highReg));
  StoreValueWide(cUnit, rlDest, rlResult);
}

bool GenInlinedSqrt(CompilationUnit* cUnit, CallInfo* info) {
  DCHECK_EQ(cUnit->instructionSet, kThumb2);
  LIR *branch;
  RegLocation rlSrc = info->args[0];
  RegLocation rlDest = InlineTargetWide(cUnit, info);  // double place for result
  rlSrc = LoadValueWide(cUnit, rlSrc, kFPReg);
  RegLocation rlResult = EvalLoc(cUnit, rlDest, kFPReg, true);
  NewLIR2(cUnit, kThumb2Vsqrtd, S2d(rlResult.lowReg, rlResult.highReg),
          S2d(rlSrc.lowReg, rlSrc.highReg));
  NewLIR2(cUnit, kThumb2Vcmpd, S2d(rlResult.lowReg, rlResult.highReg),
          S2d(rlResult.lowReg, rlResult.highReg));
  NewLIR0(cUnit, kThumb2Fmstat);
  branch = NewLIR2(cUnit, kThumbBCond, 0, kArmCondEq);
  ClobberCalleeSave(cUnit);
  LockCallTemps(cUnit);  // Using fixed registers
  int rTgt = LoadHelper(cUnit, ENTRYPOINT_OFFSET(pSqrt));
  NewLIR3(cUnit, kThumb2Fmrrd, r0, r1, S2d(rlSrc.lowReg, rlSrc.highReg));
  NewLIR1(cUnit, kThumbBlxR, rTgt);
  NewLIR3(cUnit, kThumb2Fmdrr, S2d(rlResult.lowReg, rlResult.highReg), r0, r1);
  branch->target = NewLIR0(cUnit, kPseudoTargetLabel);
  StoreValueWide(cUnit, rlDest, rlResult);
  return true;
}


}  // namespace art
