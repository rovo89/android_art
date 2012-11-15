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

/* This file contains codegen for the Mips ISA */

#include "oat/runtime/oat_support_entrypoints.h"

namespace art {

/*
 * Compare two 64-bit values
 *    x = y     return  0
 *    x < y     return -1
 *    x > y     return  1
 *
 *    slt   t0,  x.hi, y.hi;        # (x.hi < y.hi) ? 1:0
 *    sgt   t1,  x.hi, y.hi;        # (y.hi > x.hi) ? 1:0
 *    subu  res, t0, t1             # res = -1:1:0 for [ < > = ]
 *    bnez  res, finish
 *    sltu  t0, x.lo, y.lo
 *    sgtu  r1, x.lo, y.lo
 *    subu  res, t0, t1
 * finish:
 *
 */
void genCmpLong(CompilationUnit* cUnit, RegLocation rlDest,
        RegLocation rlSrc1, RegLocation rlSrc2)
{
  rlSrc1 = loadValueWide(cUnit, rlSrc1, kCoreReg);
  rlSrc2 = loadValueWide(cUnit, rlSrc2, kCoreReg);
  int t0 = oatAllocTemp(cUnit);
  int t1 = oatAllocTemp(cUnit);
  RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
  newLIR3(cUnit, kMipsSlt, t0, rlSrc1.highReg, rlSrc2.highReg);
  newLIR3(cUnit, kMipsSlt, t1, rlSrc2.highReg, rlSrc1.highReg);
  newLIR3(cUnit, kMipsSubu, rlResult.lowReg, t1, t0);
  LIR* branch = opCmpImmBranch(cUnit, kCondNe, rlResult.lowReg, 0, NULL);
  newLIR3(cUnit, kMipsSltu, t0, rlSrc1.lowReg, rlSrc2.lowReg);
  newLIR3(cUnit, kMipsSltu, t1, rlSrc2.lowReg, rlSrc1.lowReg);
  newLIR3(cUnit, kMipsSubu, rlResult.lowReg, t1, t0);
  oatFreeTemp(cUnit, t0);
  oatFreeTemp(cUnit, t1);
  LIR* target = newLIR0(cUnit, kPseudoTargetLabel);
  branch->target = (LIR*)target;
  storeValue(cUnit, rlDest, rlResult);
}

LIR* opCmpBranch(CompilationUnit* cUnit, ConditionCode cond, int src1,
         int src2, LIR* target)
{
  LIR* branch;
  MipsOpCode sltOp;
  MipsOpCode brOp;
  bool cmpZero = false;
  bool swapped = false;
  switch (cond) {
    case kCondEq:
      brOp = kMipsBeq;
      cmpZero = true;
      break;
    case kCondNe:
      brOp = kMipsBne;
      cmpZero = true;
      break;
    case kCondCc:
      sltOp = kMipsSltu;
      brOp = kMipsBnez;
      break;
    case kCondCs:
      sltOp = kMipsSltu;
      brOp = kMipsBeqz;
      break;
    case kCondGe:
      sltOp = kMipsSlt;
      brOp = kMipsBeqz;
      break;
    case kCondGt:
      sltOp = kMipsSlt;
      brOp = kMipsBnez;
      swapped = true;
      break;
    case kCondLe:
      sltOp = kMipsSlt;
      brOp = kMipsBeqz;
      swapped = true;
      break;
    case kCondLt:
      sltOp = kMipsSlt;
      brOp = kMipsBnez;
      break;
    case kCondHi:  // Gtu
      sltOp = kMipsSltu;
      brOp = kMipsBnez;
      swapped = true;
      break;
    default:
      LOG(FATAL) << "No support for ConditionCode: " << (int) cond;
      return NULL;
  }
  if (cmpZero) {
    branch = newLIR2(cUnit, brOp, src1, src2);
  } else {
    int tReg = oatAllocTemp(cUnit);
    if (swapped) {
      newLIR3(cUnit, sltOp, tReg, src2, src1);
    } else {
      newLIR3(cUnit, sltOp, tReg, src1, src2);
    }
    branch = newLIR1(cUnit, brOp, tReg);
    oatFreeTemp(cUnit, tReg);
  }
  branch->target = target;
  return branch;
}

LIR* opCmpImmBranch(CompilationUnit* cUnit, ConditionCode cond, int reg,
          int checkValue, LIR* target)
{
  LIR* branch;
  if (checkValue != 0) {
    // TUNING: handle s16 & kCondLt/Mi case using slti
    int tReg = oatAllocTemp(cUnit);
    loadConstant(cUnit, tReg, checkValue);
    branch = opCmpBranch(cUnit, cond, reg, tReg, target);
    oatFreeTemp(cUnit, tReg);
    return branch;
  }
  MipsOpCode opc;
  switch (cond) {
    case kCondEq: opc = kMipsBeqz; break;
    case kCondGe: opc = kMipsBgez; break;
    case kCondGt: opc = kMipsBgtz; break;
    case kCondLe: opc = kMipsBlez; break;
    //case KCondMi:
    case kCondLt: opc = kMipsBltz; break;
    case kCondNe: opc = kMipsBnez; break;
    default:
      // Tuning: use slti when applicable
      int tReg = oatAllocTemp(cUnit);
      loadConstant(cUnit, tReg, checkValue);
      branch = opCmpBranch(cUnit, cond, reg, tReg, target);
      oatFreeTemp(cUnit, tReg);
      return branch;
  }
  branch = newLIR1(cUnit, opc, reg);
  branch->target = target;
  return branch;
}

LIR* opRegCopyNoInsert(CompilationUnit *cUnit, int rDest, int rSrc)
{
#ifdef __mips_hard_float
  if (MIPS_FPREG(rDest) || MIPS_FPREG(rSrc))
    return fpRegCopy(cUnit, rDest, rSrc);
#endif
  LIR* res = rawLIR(cUnit, cUnit->currentDalvikOffset, kMipsMove,
            rDest, rSrc);
  if (!(cUnit->disableOpt & (1 << kSafeOptimizations)) && rDest == rSrc) {
    res->flags.isNop = true;
  }
  return res;
}

LIR* opRegCopy(CompilationUnit *cUnit, int rDest, int rSrc)
{
  LIR *res = opRegCopyNoInsert(cUnit, rDest, rSrc);
  oatAppendLIR(cUnit, (LIR*)res);
  return res;
}

void opRegCopyWide(CompilationUnit *cUnit, int destLo, int destHi,
          int srcLo, int srcHi)
{
#ifdef __mips_hard_float
  bool destFP = MIPS_FPREG(destLo) && MIPS_FPREG(destHi);
  bool srcFP = MIPS_FPREG(srcLo) && MIPS_FPREG(srcHi);
  assert(MIPS_FPREG(srcLo) == MIPS_FPREG(srcHi));
  assert(MIPS_FPREG(destLo) == MIPS_FPREG(destHi));
  if (destFP) {
    if (srcFP) {
      opRegCopy(cUnit, s2d(destLo, destHi), s2d(srcLo, srcHi));
    } else {
       /* note the operands are swapped for the mtc1 instr */
      newLIR2(cUnit, kMipsMtc1, srcLo, destLo);
      newLIR2(cUnit, kMipsMtc1, srcHi, destHi);
    }
  } else {
    if (srcFP) {
      newLIR2(cUnit, kMipsMfc1, destLo, srcLo);
      newLIR2(cUnit, kMipsMfc1, destHi, srcHi);
    } else {
      // Handle overlap
      if (srcHi == destLo) {
        opRegCopy(cUnit, destHi, srcHi);
        opRegCopy(cUnit, destLo, srcLo);
      } else {
        opRegCopy(cUnit, destLo, srcLo);
        opRegCopy(cUnit, destHi, srcHi);
      }
    }
  }
#else
  // Handle overlap
  if (srcHi == destLo) {
    opRegCopy(cUnit, destHi, srcHi);
    opRegCopy(cUnit, destLo, srcLo);
  } else {
    opRegCopy(cUnit, destLo, srcLo);
    opRegCopy(cUnit, destHi, srcHi);
  }
#endif
}

void genFusedLongCmpBranch(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir)
{
  UNIMPLEMENTED(FATAL) << "Need codegen for fused long cmp branch";
}

LIR* genRegMemCheck(CompilationUnit* cUnit, ConditionCode cCode,
                    int reg1, int base, int offset, ThrowKind kind)
{
  LOG(FATAL) << "Unexpected use of genRegMemCheck for Arm";
  return NULL;
}

RegLocation genDivRem(CompilationUnit* cUnit, RegLocation rlDest, int reg1, int reg2, bool isDiv)
{
  newLIR4(cUnit, kMipsDiv, r_HI, r_LO, reg1, reg2);
  RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
  if (isDiv) {
    newLIR2(cUnit, kMipsMflo, rlResult.lowReg, r_LO);
  } else {
    newLIR2(cUnit, kMipsMfhi, rlResult.lowReg, r_HI);
  }
  return rlResult;
}

RegLocation genDivRemLit(CompilationUnit* cUnit, RegLocation rlDest, int reg1, int lit, bool isDiv)
{
  int tReg = oatAllocTemp(cUnit);
  newLIR3(cUnit, kMipsAddiu, tReg, r_ZERO, lit);
  newLIR4(cUnit, kMipsDiv, r_HI, r_LO, reg1, tReg);
  RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
  if (isDiv) {
    newLIR2(cUnit, kMipsMflo, rlResult.lowReg, r_LO);
  } else {
    newLIR2(cUnit, kMipsMfhi, rlResult.lowReg, r_HI);
  }
  oatFreeTemp(cUnit, tReg);
  return rlResult;
}

void opLea(CompilationUnit* cUnit, int rBase, int reg1, int reg2, int scale, int offset)
{
  LOG(FATAL) << "Unexpected use of opLea for Arm";
}

void opTlsCmp(CompilationUnit* cUnit, int offset, int val)
{
  LOG(FATAL) << "Unexpected use of opTlsCmp for Arm";
}

bool genInlinedCas32(CompilationUnit* cUnit, CallInfo* info, bool need_write_barrier) {
  DCHECK_NE(cUnit->instructionSet, kThumb2);
  return false;
}

bool genInlinedSqrt(CompilationUnit* cUnit, CallInfo* info) {
  DCHECK_NE(cUnit->instructionSet, kThumb2);
  return false;
}

LIR* opPcRelLoad(CompilationUnit* cUnit, int reg, LIR* target) {
  LOG(FATAL) << "Unexpected use of opPcRelLoad for Mips";
  return NULL;
}

LIR* opVldm(CompilationUnit* cUnit, int rBase, int count)
{
  LOG(FATAL) << "Unexpected use of opVldm for Mips";
  return NULL;
}

LIR* opVstm(CompilationUnit* cUnit, int rBase, int count)
{
  LOG(FATAL) << "Unexpected use of opVstm for Mips";
  return NULL;
}

void genMultiplyByTwoBitMultiplier(CompilationUnit* cUnit, RegLocation rlSrc,
                                   RegLocation rlResult, int lit,
                                   int firstBit, int secondBit)
{
  int tReg = oatAllocTemp(cUnit);
  opRegRegImm(cUnit, kOpLsl, tReg, rlSrc.lowReg, secondBit - firstBit);
  opRegRegReg(cUnit, kOpAdd, rlResult.lowReg, rlSrc.lowReg, tReg);
  oatFreeTemp(cUnit, tReg);
  if (firstBit != 0) {
    opRegRegImm(cUnit, kOpLsl, rlResult.lowReg, rlResult.lowReg, firstBit);
  }
}

void genDivZeroCheck(CompilationUnit* cUnit, int regLo, int regHi)
{
  int tReg = oatAllocTemp(cUnit);
  opRegRegReg(cUnit, kOpOr, tReg, regLo, regHi);
  genImmedCheck(cUnit, kCondEq, tReg, 0, kThrowDivZero);
  oatFreeTemp(cUnit, tReg);
}

// Test suspend flag, return target of taken suspend branch
LIR* opTestSuspend(CompilationUnit* cUnit, LIR* target)
{
  opRegImm(cUnit, kOpSub, rMIPS_SUSPEND, 1);
  return opCmpImmBranch(cUnit, (target == NULL) ? kCondEq : kCondNe, rMIPS_SUSPEND, 0, target);
}

// Decrement register and branch on condition
LIR* opDecAndBranch(CompilationUnit* cUnit, ConditionCode cCode, int reg, LIR* target)
{
  opRegImm(cUnit, kOpSub, reg, 1);
  return opCmpImmBranch(cUnit, cCode, reg, 0, target);
}

bool smallLiteralDivide(CompilationUnit* cUnit, Instruction::Code dalvikOpcode,
                        RegLocation rlSrc, RegLocation rlDest, int lit)
{
  LOG(FATAL) << "Unexpected use of smallLiteralDive in Mips";
  return false;
}

LIR* opIT(CompilationUnit* cUnit, ArmConditionCode cond, const char* guide)
{
  LOG(FATAL) << "Unexpected use of opIT in Mips";
  return NULL;
}

bool genAddLong(CompilationUnit* cUnit, RegLocation rlDest,
                RegLocation rlSrc1, RegLocation rlSrc2)
{
  rlSrc1 = loadValueWide(cUnit, rlSrc1, kCoreReg);
  rlSrc2 = loadValueWide(cUnit, rlSrc2, kCoreReg);
  RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
  /*
   *  [v1 v0] =  [a1 a0] + [a3 a2];
   *  addu v0,a2,a0
   *  addu t1,a3,a1
   *  sltu v1,v0,a2
   *  addu v1,v1,t1
   */

  opRegRegReg(cUnit, kOpAdd, rlResult.lowReg, rlSrc2.lowReg, rlSrc1.lowReg);
  int tReg = oatAllocTemp(cUnit);
  opRegRegReg(cUnit, kOpAdd, tReg, rlSrc2.highReg, rlSrc1.highReg);
  newLIR3(cUnit, kMipsSltu, rlResult.highReg, rlResult.lowReg, rlSrc2.lowReg);
  opRegRegReg(cUnit, kOpAdd, rlResult.highReg, rlResult.highReg, tReg);
  oatFreeTemp(cUnit, tReg);
  storeValueWide(cUnit, rlDest, rlResult);
  return false;
}

bool genSubLong(CompilationUnit* cUnit, RegLocation rlDest,
        RegLocation rlSrc1, RegLocation rlSrc2)
{
  rlSrc1 = loadValueWide(cUnit, rlSrc1, kCoreReg);
  rlSrc2 = loadValueWide(cUnit, rlSrc2, kCoreReg);
  RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
  /*
   *  [v1 v0] =  [a1 a0] - [a3 a2];
   *  sltu  t1,a0,a2
   *  subu  v0,a0,a2
   *  subu  v1,a1,a3
   *  subu  v1,v1,t1
   */

  int tReg = oatAllocTemp(cUnit);
  newLIR3(cUnit, kMipsSltu, tReg, rlSrc1.lowReg, rlSrc2.lowReg);
  opRegRegReg(cUnit, kOpSub, rlResult.lowReg, rlSrc1.lowReg, rlSrc2.lowReg);
  opRegRegReg(cUnit, kOpSub, rlResult.highReg, rlSrc1.highReg, rlSrc2.highReg);
  opRegRegReg(cUnit, kOpSub, rlResult.highReg, rlResult.highReg, tReg);
  oatFreeTemp(cUnit, tReg);
  storeValueWide(cUnit, rlDest, rlResult);
  return false;
}

bool genNegLong(CompilationUnit* cUnit, RegLocation rlDest,
                RegLocation rlSrc)
{
  rlSrc = loadValueWide(cUnit, rlSrc, kCoreReg);
  RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
  /*
   *  [v1 v0] =  -[a1 a0]
   *  negu  v0,a0
   *  negu  v1,a1
   *  sltu  t1,r_zero
   *  subu  v1,v1,t1
   */

  opRegReg(cUnit, kOpNeg, rlResult.lowReg, rlSrc.lowReg);
  opRegReg(cUnit, kOpNeg, rlResult.highReg, rlSrc.highReg);
  int tReg = oatAllocTemp(cUnit);
  newLIR3(cUnit, kMipsSltu, tReg, r_ZERO, rlResult.lowReg);
  opRegRegReg(cUnit, kOpSub, rlResult.highReg, rlResult.highReg, tReg);
  oatFreeTemp(cUnit, tReg);
  storeValueWide(cUnit, rlDest, rlResult);
  return false;
}

bool genAndLong(CompilationUnit* cUnit, RegLocation rlDest,
                RegLocation rlSrc1, RegLocation rlSrc2)
{
  LOG(FATAL) << "Unexpected use of genAndLong for Mips";
  return false;
}

bool genOrLong(CompilationUnit* cUnit, RegLocation rlDest,
               RegLocation rlSrc1, RegLocation rlSrc2)
{
  LOG(FATAL) << "Unexpected use of genOrLong for Mips";
  return false;
}

bool genXorLong(CompilationUnit* cUnit, RegLocation rlDest,
               RegLocation rlSrc1, RegLocation rlSrc2)
{
  LOG(FATAL) << "Unexpected use of genXorLong for Mips";
  return false;
}

}  // namespace art
