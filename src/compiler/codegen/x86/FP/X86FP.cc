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

namespace art {

bool genArithOpFloat(CompilationUnit *cUnit, MIR *mir, RegLocation rlDest,
                     RegLocation rlSrc1, RegLocation rlSrc2) {
  X86OpCode op = kX86Nop;
  RegLocation rlResult;

  /*
   * Don't attempt to optimize register usage since these opcodes call out to
   * the handlers.
   */
  switch (mir->dalvikInsn.opcode) {
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
    case Instruction::NEG_FLOAT:
      UNIMPLEMENTED(WARNING) << "inline fneg"; // pxor xmm, [0x80000000]
                                                             // fall-through
    case Instruction::REM_FLOAT_2ADDR:
    case Instruction::REM_FLOAT: {
      return genArithOpFloatPortable(cUnit, mir, rlDest, rlSrc1, rlSrc2);
    }
    default:
      return true;
  }
  rlSrc1 = loadValue(cUnit, rlSrc1, kFPReg);
  rlSrc2 = loadValue(cUnit, rlSrc2, kFPReg);
  rlResult = oatEvalLoc(cUnit, rlDest, kFPReg, true);
  int rDest = rlResult.lowReg;
  int rSrc1 = rlSrc1.lowReg;
  int rSrc2 = rlSrc2.lowReg;
  // TODO: at least CHECK_NE(rDest, rSrc2);
  opRegCopy(cUnit, rDest, rSrc1);
  newLIR2(cUnit, op, rDest, rSrc2);
  storeValue(cUnit, rlDest, rlResult);

  return false;
}

static bool genArithOpDouble(CompilationUnit *cUnit, MIR *mir,
                             RegLocation rlDest, RegLocation rlSrc1,
                             RegLocation rlSrc2) {
  X86OpCode op = kX86Nop;
  RegLocation rlResult;

  switch (mir->dalvikInsn.opcode) {
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
    case Instruction::NEG_DOUBLE:
    case Instruction::REM_DOUBLE_2ADDR:
    case Instruction::REM_DOUBLE: {
      return genArithOpDoublePortable(cUnit, mir, rlDest, rlSrc1, rlSrc2);
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
  int rDest = S2D(rlResult.lowReg, rlResult.highReg);
  int rSrc1 = S2D(rlSrc1.lowReg, rlSrc1.highReg);
  int rSrc2 = S2D(rlSrc2.lowReg, rlSrc2.highReg);
  // TODO: at least CHECK_NE(rDest, rSrc2);
  opRegCopy(cUnit, rDest, rSrc1);
  newLIR2(cUnit, op, rDest, rSrc2);
  storeValueWide(cUnit, rlDest, rlResult);
  return false;
}

static bool genConversion(CompilationUnit *cUnit, MIR *mir) {
  Instruction::Code opcode = mir->dalvikInsn.opcode;
  bool longSrc = false;
  bool longDest = false;
  RegLocation rlSrc;
  RegLocation rlDest;
  X86OpCode op = kX86Nop;
  int srcReg;
  RegLocation rlResult;
  switch (opcode) {
    case Instruction::INT_TO_FLOAT:
      longSrc = false;
      longDest = false;
      op = kX86Cvtsi2ssRR;
      break;
    case Instruction::DOUBLE_TO_FLOAT:
      longSrc = true;
      longDest = false;
      op = kX86Cvtsd2ssRR;
      break;
    case Instruction::FLOAT_TO_DOUBLE:
      longSrc = false;
      longDest = true;
      op = kX86Cvtss2sdRR;
      break;
    case Instruction::INT_TO_DOUBLE:
      longSrc = false;
      longDest = true;
      op = kX86Cvtsi2sdRR;
      break;
    case Instruction::FLOAT_TO_INT:
    case Instruction::DOUBLE_TO_INT:
      // These are easy to implement inline except when the src is > MAX_INT/LONG the result
      // needs to be changed from 0x80000000 to 0x7FFFFFF (requires an in memory float/double
      // literal constant to compare against).
      UNIMPLEMENTED(WARNING) << "inline [df]2i";
    case Instruction::LONG_TO_DOUBLE:
    case Instruction::FLOAT_TO_LONG:
    case Instruction::LONG_TO_FLOAT:
    case Instruction::DOUBLE_TO_LONG:
      return genConversionPortable(cUnit, mir);
    default:
      return true;
  }
  if (longSrc) {
    rlSrc = oatGetSrcWide(cUnit, mir, 0, 1);
    rlSrc = loadValueWide(cUnit, rlSrc, kFPReg);
    srcReg = S2D(rlSrc.lowReg, rlSrc.highReg);
  } else {
    rlSrc = oatGetSrc(cUnit, mir, 0);
    rlSrc = loadValue(cUnit, rlSrc, kFPReg);
    srcReg = rlSrc.lowReg;
  }
  if (longDest) {
    rlDest = oatGetDestWide(cUnit, mir, 0, 1);
    rlResult = oatEvalLoc(cUnit, rlDest, kFPReg, true);
    newLIR2(cUnit, op, S2D(rlResult.lowReg, rlResult.highReg), srcReg);
    storeValueWide(cUnit, rlDest, rlResult);
  } else {
    rlDest = oatGetDest(cUnit, mir, 0);
    rlResult = oatEvalLoc(cUnit, rlDest, kFPReg, true);
    newLIR2(cUnit, op, rlResult.lowReg, srcReg);
    storeValue(cUnit, rlDest, rlResult);
  }
  return false;
}

static bool genCmpFP(CompilationUnit *cUnit, MIR *mir, RegLocation rlDest,
                     RegLocation rlSrc1, RegLocation rlSrc2) {
  Instruction::Code code = mir->dalvikInsn.opcode;
  bool single = (code == Instruction::CMPL_FLOAT) || (code == Instruction::CMPG_FLOAT);
  bool unorderedGt = (code == Instruction::CMPG_DOUBLE) || (code == Instruction::CMPG_FLOAT);
  int srcReg1;
  int srcReg2;
  if (single) {
    rlSrc1 = oatGetSrc(cUnit, mir, 0);
    rlSrc1 = loadValue(cUnit, rlSrc1, kFPReg);
    srcReg1 = rlSrc1.lowReg;
    rlSrc2 = oatGetSrc(cUnit, mir, 0);
    rlSrc2 = loadValue(cUnit, rlSrc1, kFPReg);
    srcReg2 = rlSrc1.lowReg;
  } else {
    rlSrc1 = oatGetSrcWide(cUnit, mir, 0, 1);
    rlSrc1 = loadValueWide(cUnit, rlSrc1, kFPReg);
    srcReg1 = S2D(rlSrc1.lowReg, rlSrc1.highReg);
    rlSrc2 = oatGetSrcWide(cUnit, mir, 0, 1);
    rlSrc2 = loadValueWide(cUnit, rlSrc2, kFPReg);
    srcReg2 = S2D(rlSrc2.lowReg, rlSrc2.highReg);
  }
  rlDest = oatGetDest(cUnit, mir, 0);
  RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kFPReg, true);
  opRegImm(cUnit, kOpMov, rlResult.lowReg, unorderedGt ? 1 : 0);
  if (single) {
    newLIR2(cUnit, kX86UcomissRR, srcReg1, srcReg2);
  } else {
    newLIR2(cUnit, kX86UcomisdRR, srcReg1, srcReg2);
  }
  LIR* branch = NULL;
  if (unorderedGt) {
    branch = newLIR2(cUnit, kX86Jcc8, 0, kX86CondPE);
  }
  newLIR2(cUnit, kX86Set8R, rlResult.lowReg, kX86CondA /* above - unsigned > */);
  newLIR2(cUnit, kX86Sbb32RI, rlResult.lowReg, 0);
  if (unorderedGt) {
    branch->target = newLIR0(cUnit, kPseudoTargetLabel);
  }
  return false;
}

} //  namespace art
