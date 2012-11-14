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

static bool genArithOpFloat(CompilationUnit *cUnit, Instruction::Code opcode,
                            RegLocation rlDest, RegLocation rlSrc1,
                            RegLocation rlSrc2) {
  X86OpCode op = kX86Nop;
  RegLocation rlResult;

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
    case Instruction::NEG_FLOAT:
    case Instruction::REM_FLOAT_2ADDR:
    case Instruction::REM_FLOAT:
      return genArithOpFloatPortable(cUnit, opcode, rlDest, rlSrc1, rlSrc2);
    default:
      return true;
  }
  rlSrc1 = loadValue(cUnit, rlSrc1, kFPReg);
  rlSrc2 = loadValue(cUnit, rlSrc2, kFPReg);
  rlResult = oatEvalLoc(cUnit, rlDest, kFPReg, true);
  int rDest = rlResult.lowReg;
  int rSrc1 = rlSrc1.lowReg;
  int rSrc2 = rlSrc2.lowReg;
  if (rDest == rSrc2) {
    rSrc2 = oatAllocTempFloat(cUnit);
    opRegCopy(cUnit, rSrc2, rDest);
  }
  opRegCopy(cUnit, rDest, rSrc1);
  newLIR2(cUnit, op, rDest, rSrc2);
  storeValue(cUnit, rlDest, rlResult);

  return false;
}

static bool genArithOpDouble(CompilationUnit *cUnit, Instruction::Code opcode,
                             RegLocation rlDest, RegLocation rlSrc1,
                             RegLocation rlSrc2) {
  X86OpCode op = kX86Nop;
  RegLocation rlResult;

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
    case Instruction::NEG_DOUBLE:
    case Instruction::REM_DOUBLE_2ADDR:
    case Instruction::REM_DOUBLE:
      return genArithOpDoublePortable(cUnit, opcode, rlDest, rlSrc1, rlSrc2);
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
  int rDest = s2d(rlResult.lowReg, rlResult.highReg);
  int rSrc1 = s2d(rlSrc1.lowReg, rlSrc1.highReg);
  int rSrc2 = s2d(rlSrc2.lowReg, rlSrc2.highReg);
  if (rDest == rSrc2) {
    rSrc2 = oatAllocTempDouble(cUnit) | X86_FP_DOUBLE;
    opRegCopy(cUnit, rSrc2, rDest);
  }
  opRegCopy(cUnit, rDest, rSrc1);
  newLIR2(cUnit, op, rDest, rSrc2);
  storeValueWide(cUnit, rlDest, rlResult);
  return false;
}

static bool genConversion(CompilationUnit *cUnit, Instruction::Code opcode,
                          RegLocation rlDest, RegLocation rlSrc) {
  RegisterClass rcSrc = kFPReg;
  X86OpCode op = kX86Nop;
  int srcReg;
  RegLocation rlResult;
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
      rlSrc = loadValue(cUnit, rlSrc, kFPReg);
      srcReg = rlSrc.lowReg;
      oatClobberSReg(cUnit, rlDest.sRegLow);
      rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
      int tempReg = oatAllocTempFloat(cUnit);

      loadConstant(cUnit, rlResult.lowReg, 0x7fffffff);
      newLIR2(cUnit, kX86Cvtsi2ssRR, tempReg, rlResult.lowReg);
      newLIR2(cUnit, kX86ComissRR, srcReg, tempReg);
      LIR* branchPosOverflow = newLIR2(cUnit, kX86Jcc8, 0, kX86CondA);
      LIR* branchNaN = newLIR2(cUnit, kX86Jcc8, 0, kX86CondP);
      newLIR2(cUnit, kX86Cvttss2siRR, rlResult.lowReg, srcReg);
      LIR* branchNormal = newLIR1(cUnit, kX86Jmp8, 0);
      branchNaN->target = newLIR0(cUnit, kPseudoTargetLabel);
      newLIR2(cUnit, kX86Xor32RR, rlResult.lowReg, rlResult.lowReg);
      branchPosOverflow->target = newLIR0(cUnit, kPseudoTargetLabel);
      branchNormal->target = newLIR0(cUnit, kPseudoTargetLabel);
      storeValue(cUnit, rlDest, rlResult);
      return false;
    }
    case Instruction::DOUBLE_TO_INT: {
      rlSrc = loadValueWide(cUnit, rlSrc, kFPReg);
      srcReg = rlSrc.lowReg;
      oatClobberSReg(cUnit, rlDest.sRegLow);
      rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
      int tempReg = oatAllocTempDouble(cUnit) | X86_FP_DOUBLE;

      loadConstant(cUnit, rlResult.lowReg, 0x7fffffff);
      newLIR2(cUnit, kX86Cvtsi2sdRR, tempReg, rlResult.lowReg);
      newLIR2(cUnit, kX86ComisdRR, srcReg, tempReg);
      LIR* branchPosOverflow = newLIR2(cUnit, kX86Jcc8, 0, kX86CondA);
      LIR* branchNaN = newLIR2(cUnit, kX86Jcc8, 0, kX86CondP);
      newLIR2(cUnit, kX86Cvttsd2siRR, rlResult.lowReg, srcReg);
      LIR* branchNormal = newLIR1(cUnit, kX86Jmp8, 0);
      branchNaN->target = newLIR0(cUnit, kPseudoTargetLabel);
      newLIR2(cUnit, kX86Xor32RR, rlResult.lowReg, rlResult.lowReg);
      branchPosOverflow->target = newLIR0(cUnit, kPseudoTargetLabel);
      branchNormal->target = newLIR0(cUnit, kPseudoTargetLabel);
      storeValue(cUnit, rlDest, rlResult);
      return false;
    }
    case Instruction::LONG_TO_DOUBLE:
    case Instruction::LONG_TO_FLOAT:
      // TODO: inline by using memory as a 64-bit source. Be careful about promoted registers.
    case Instruction::FLOAT_TO_LONG:
    case Instruction::DOUBLE_TO_LONG:
      return genConversionPortable(cUnit, opcode, rlDest, rlSrc);
    default:
      return true;
  }
  if (rlSrc.wide) {
    rlSrc = loadValueWide(cUnit, rlSrc, rcSrc);
    srcReg = s2d(rlSrc.lowReg, rlSrc.highReg);
  } else {
    rlSrc = loadValue(cUnit, rlSrc, rcSrc);
    srcReg = rlSrc.lowReg;
  }
  if (rlDest.wide) {
    rlResult = oatEvalLoc(cUnit, rlDest, kFPReg, true);
    newLIR2(cUnit, op, s2d(rlResult.lowReg, rlResult.highReg), srcReg);
    storeValueWide(cUnit, rlDest, rlResult);
  } else {
    rlResult = oatEvalLoc(cUnit, rlDest, kFPReg, true);
    newLIR2(cUnit, op, rlResult.lowReg, srcReg);
    storeValue(cUnit, rlDest, rlResult);
  }
  return false;
}

static bool genCmpFP(CompilationUnit *cUnit, Instruction::Code code, RegLocation rlDest,
                     RegLocation rlSrc1, RegLocation rlSrc2) {
  bool single = (code == Instruction::CMPL_FLOAT) || (code == Instruction::CMPG_FLOAT);
  bool unorderedGt = (code == Instruction::CMPG_DOUBLE) || (code == Instruction::CMPG_FLOAT);
  int srcReg1;
  int srcReg2;
  if (single) {
    rlSrc1 = loadValue(cUnit, rlSrc1, kFPReg);
    srcReg1 = rlSrc1.lowReg;
    rlSrc2 = loadValue(cUnit, rlSrc2, kFPReg);
    srcReg2 = rlSrc2.lowReg;
  } else {
    rlSrc1 = loadValueWide(cUnit, rlSrc1, kFPReg);
    srcReg1 = s2d(rlSrc1.lowReg, rlSrc1.highReg);
    rlSrc2 = loadValueWide(cUnit, rlSrc2, kFPReg);
    srcReg2 = s2d(rlSrc2.lowReg, rlSrc2.highReg);
  }
  oatClobberSReg(cUnit, rlDest.sRegLow);
  RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
  loadConstantNoClobber(cUnit, rlResult.lowReg, unorderedGt ? 1 : 0);
  if (single) {
    newLIR2(cUnit, kX86UcomissRR, srcReg1, srcReg2);
  } else {
    newLIR2(cUnit, kX86UcomisdRR, srcReg1, srcReg2);
  }
  LIR* branch = NULL;
  if (unorderedGt) {
    branch = newLIR2(cUnit, kX86Jcc8, 0, kX86CondPE);
  }
  // If the result reg can't be byte accessed, use a jump and move instead of a set.
  if (rlResult.lowReg >= 4) {
    LIR* branch2 = NULL;
    if (unorderedGt) {
      branch2 = newLIR2(cUnit, kX86Jcc8, 0, kX86CondA);
      newLIR2(cUnit, kX86Mov32RI, rlResult.lowReg, 0x0);
    } else {
      branch2 = newLIR2(cUnit, kX86Jcc8, 0, kX86CondBe);
      newLIR2(cUnit, kX86Mov32RI, rlResult.lowReg, 0x1);
    }
    branch2->target = newLIR0(cUnit, kPseudoTargetLabel);
  } else {
    newLIR2(cUnit, kX86Set8R, rlResult.lowReg, kX86CondA /* above - unsigned > */);
  }
  newLIR2(cUnit, kX86Sbb32RI, rlResult.lowReg, 0);
  if (unorderedGt) {
    branch->target = newLIR0(cUnit, kPseudoTargetLabel);
  }
  storeValue(cUnit, rlDest, rlResult);
  return false;
}

void genFusedFPCmpBranch(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir,
                                bool gtBias, bool isDouble) {
  LIR* labelList = cUnit->blockLabelList;
  LIR* taken = &labelList[bb->taken->id];
  LIR* notTaken = &labelList[bb->fallThrough->id];
  LIR* branch = NULL;
  RegLocation rlSrc1;
  RegLocation rlSrc2;
  if (isDouble) {
    rlSrc1 = oatGetSrcWide(cUnit, mir, 0);
    rlSrc2 = oatGetSrcWide(cUnit, mir, 2);
    rlSrc1 = loadValueWide(cUnit, rlSrc1, kFPReg);
    rlSrc2 = loadValueWide(cUnit, rlSrc2, kFPReg);
    newLIR2(cUnit, kX86UcomisdRR, s2d(rlSrc1.lowReg, rlSrc1.highReg),
            s2d(rlSrc2.lowReg, rlSrc2.highReg));
  } else {
    rlSrc1 = oatGetSrc(cUnit, mir, 0);
    rlSrc2 = oatGetSrc(cUnit, mir, 1);
    rlSrc1 = loadValue(cUnit, rlSrc1, kFPReg);
    rlSrc2 = loadValue(cUnit, rlSrc2, kFPReg);
    newLIR2(cUnit, kX86UcomissRR, rlSrc1.lowReg, rlSrc2.lowReg);
  }
  ConditionCode ccode = static_cast<ConditionCode>(mir->dalvikInsn.arg[0]);
  switch (ccode) {
    case kCondEq:
      if (!gtBias) {
        branch = newLIR2(cUnit, kX86Jcc8, 0, kX86CondPE);
        branch->target = notTaken;
      }
      break;
    case kCondNe:
      if (!gtBias) {
        branch = newLIR2(cUnit, kX86Jcc8, 0, kX86CondPE);
        branch->target = taken;
      }
      break;
    case kCondLt:
      if (gtBias) {
        branch = newLIR2(cUnit, kX86Jcc8, 0, kX86CondPE);
        branch->target = notTaken;
      }
      ccode = kCondCs;
      break;
    case kCondLe:
      if (gtBias) {
        branch = newLIR2(cUnit, kX86Jcc8, 0, kX86CondPE);
        branch->target = notTaken;
      }
      ccode = kCondLs;
      break;
    case kCondGt:
      if (gtBias) {
        branch = newLIR2(cUnit, kX86Jcc8, 0, kX86CondPE);
        branch->target = taken;
      }
      ccode = kCondHi;
      break;
    case kCondGe:
      if (gtBias) {
        branch = newLIR2(cUnit, kX86Jcc8, 0, kX86CondPE);
        branch->target = taken;
      }
      ccode = kCondCc;
      break;
    default:
      LOG(FATAL) << "Unexpected ccode: " << (int)ccode;
  }
  opCondBranch(cUnit, ccode, taken);
}

} //  namespace art
