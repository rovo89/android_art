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

namespace art {

bool genArithOpFloat(CompilationUnit *cUnit, Instruction::Code opcode, RegLocation rlDest,
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
      return genArithOpFloatPortable(cUnit, opcode, rlDest, rlSrc1, rlSrc2);
    }
    default:
      return true;
  }
  rlSrc1 = loadValue(cUnit, rlSrc1, kFPReg);
  rlSrc2 = loadValue(cUnit, rlSrc2, kFPReg);
  rlResult = oatEvalLoc(cUnit, rlDest, kFPReg, true);
  newLIR3(cUnit, (MipsOpCode)op, rlResult.lowReg, rlSrc1.lowReg,
          rlSrc2.lowReg);
  storeValue(cUnit, rlDest, rlResult);

  return false;
#else
  return genArithOpFloatPortable(cUnit, opcode, rlDest, rlSrc1, rlSrc2);
#endif
}

static bool genArithOpDouble(CompilationUnit *cUnit, Instruction::Code opcode,
                             RegLocation rlDest, RegLocation rlSrc1,
                             RegLocation rlSrc2)
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
      return genArithOpDoublePortable(cUnit, opcode, rlDest, rlSrc1, rlSrc2);
    }
    default:
      return true;
  }
  rlSrc1 = loadValueWide(cUnit, rlSrc1, kFPReg);
  DCHECK(rlSrc1.wide);
  rlSrc2 = loadValueWide(cUnit, rlSrc2, kFPReg);
  DCHECK(rlSrc2.wide);
  rlResult = oatEvalLoc(cUnit, rlDest, kFPReg, true);
  DCHECK(rlDest.wide);
  DCHECK(rlResult.wide);
  newLIR3(cUnit, (MipsOpCode)op, S2D(rlResult.lowReg, rlResult.highReg),
          S2D(rlSrc1.lowReg, rlSrc1.highReg),
          S2D(rlSrc2.lowReg, rlSrc2.highReg));
  storeValueWide(cUnit, rlDest, rlResult);
  return false;
#else
  return genArithOpDoublePortable(cUnit, opcode, rlDest, rlSrc1, rlSrc2);
#endif
}

static bool genConversion(CompilationUnit *cUnit, Instruction::Code opcode,
                          RegLocation rlDest, RegLocation rlSrc)
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
      return genConversionPortable(cUnit, opcode, rlDest, rlSrc);
    default:
      return true;
  }
  if (rlSrc.wide) {
    rlSrc = loadValueWide(cUnit, rlSrc, kFPReg);
    srcReg = S2D(rlSrc.lowReg, rlSrc.highReg);
  } else {
    rlSrc = loadValue(cUnit, rlSrc, kFPReg);
    srcReg = rlSrc.lowReg;
  }
  if (rlDest.wide) {
    rlResult = oatEvalLoc(cUnit, rlDest, kFPReg, true);
    newLIR2(cUnit, (MipsOpCode)op, S2D(rlResult.lowReg, rlResult.highReg),
            srcReg);
    storeValueWide(cUnit, rlDest, rlResult);
  } else {
    rlResult = oatEvalLoc(cUnit, rlDest, kFPReg, true);
    newLIR2(cUnit, (MipsOpCode)op, rlResult.lowReg, srcReg);
    storeValue(cUnit, rlDest, rlResult);
  }
  return false;
#else
  return genConversionPortable(cUnit, opcode, rlDest, rlSrc);
#endif
}

static bool genCmpFP(CompilationUnit *cUnit, Instruction::Code opcode, RegLocation rlDest,
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
  oatFlushAllRegs(cUnit);
  oatLockCallTemps(cUnit);
  if (wide) {
    loadValueDirectWideFixed(cUnit, rlSrc1, r_FARG0, r_FARG1);
    loadValueDirectWideFixed(cUnit, rlSrc2, r_FARG2, r_FARG3);
  } else {
    loadValueDirectFixed(cUnit, rlSrc1, r_FARG0);
    loadValueDirectFixed(cUnit, rlSrc2, r_FARG2);
  }
  int rTgt = loadHelper(cUnit, offset);
  // NOTE: not a safepoint
  opReg(cUnit, kOpBlx, rTgt);
  RegLocation rlResult = oatGetReturn(cUnit, false);
  storeValue(cUnit, rlDest, rlResult);
  return false;
}

void genFusedFPCmpBranch(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir,
                                bool gtBias, bool isDouble)
{
  UNIMPLEMENTED(FATAL) << "Need codegen for fused fp cmp branch";
}

} //  namespace art
