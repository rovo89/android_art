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

bool GenArithOpFloat(CompilationUnit *cUnit, Instruction::Code opcode, RegLocation rlDest,
                     RegLocation rlSrc1, RegLocation rlSrc2)
{
#ifdef __mips_hard_float
  int op = kMipsNop;
  RegLocation rlResult;

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
#else
  return GenArithOpFloatPortable(cUnit, opcode, rlDest, rlSrc1, rlSrc2);
#endif
}

bool GenArithOpDouble(CompilationUnit *cUnit, Instruction::Code opcode,
                      RegLocation rlDest, RegLocation rlSrc1, RegLocation rlSrc2)
{
#ifdef __mips_hard_float
  int op = kMipsNop;
  RegLocation rlResult;

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
#else
  return GenArithOpDoublePortable(cUnit, opcode, rlDest, rlSrc1, rlSrc2);
#endif
}

bool GenConversion(CompilationUnit *cUnit, Instruction::Code opcode, RegLocation rlDest,
                   RegLocation rlSrc)
{
#ifdef __mips_hard_float
  int op = kMipsNop;
  int srcReg;
  RegLocation rlResult;
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
#else
  return GenConversionPortable(cUnit, opcode, rlDest, rlSrc);
#endif
}

bool GenCmpFP(CompilationUnit *cUnit, Instruction::Code opcode, RegLocation rlDest,
              RegLocation rlSrc1, RegLocation rlSrc2)
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
  FlushAllRegs(cUnit);
  LockCallTemps(cUnit);
  if (wide) {
    LoadValueDirectWideFixed(cUnit, rlSrc1, rMIPS_FARG0, rMIPS_FARG1);
    LoadValueDirectWideFixed(cUnit, rlSrc2, rMIPS_FARG2, rMIPS_FARG3);
  } else {
    LoadValueDirectFixed(cUnit, rlSrc1, rMIPS_FARG0);
    LoadValueDirectFixed(cUnit, rlSrc2, rMIPS_FARG2);
  }
  int rTgt = LoadHelper(cUnit, offset);
  // NOTE: not a safepoint
  OpReg(cUnit, kOpBlx, rTgt);
  RegLocation rlResult = GetReturn(cUnit, false);
  StoreValue(cUnit, rlDest, rlResult);
  return false;
}

void GenFusedFPCmpBranch(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir,
                                bool gtBias, bool isDouble)
{
  UNIMPLEMENTED(FATAL) << "Need codegen for fused fp cmp branch";
}

void GenNegFloat(CompilationUnit *cUnit, RegLocation rlDest, RegLocation rlSrc)
{
  RegLocation rlResult;
  rlSrc = LoadValue(cUnit, rlSrc, kCoreReg);
  rlResult = EvalLoc(cUnit, rlDest, kCoreReg, true);
  OpRegRegImm(cUnit, kOpAdd, rlResult.lowReg, rlSrc.lowReg, 0x80000000);
  StoreValue(cUnit, rlDest, rlResult);
}

void GenNegDouble(CompilationUnit *cUnit, RegLocation rlDest, RegLocation rlSrc)
{
  RegLocation rlResult;
  rlSrc = LoadValueWide(cUnit, rlSrc, kCoreReg);
  rlResult = EvalLoc(cUnit, rlDest, kCoreReg, true);
  OpRegRegImm(cUnit, kOpAdd, rlResult.highReg, rlSrc.highReg, 0x80000000);
  OpRegCopy(cUnit, rlResult.lowReg, rlSrc.lowReg);
  StoreValueWide(cUnit, rlDest, rlResult);
}

bool GenInlinedMinMaxInt(CompilationUnit *cUnit, CallInfo* info, bool isMin)
{
  // TODO: need Mips implementation
  return false;
}

} //  namespace art
