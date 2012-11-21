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
#include "mips_lir.h"
#include "../codegen_util.h"
#include "../ralloc_util.h"

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
void GenCmpLong(CompilationUnit* cUnit, RegLocation rlDest,
        RegLocation rlSrc1, RegLocation rlSrc2)
{
  rlSrc1 = LoadValueWide(cUnit, rlSrc1, kCoreReg);
  rlSrc2 = LoadValueWide(cUnit, rlSrc2, kCoreReg);
  int t0 = AllocTemp(cUnit);
  int t1 = AllocTemp(cUnit);
  RegLocation rlResult = EvalLoc(cUnit, rlDest, kCoreReg, true);
  NewLIR3(cUnit, kMipsSlt, t0, rlSrc1.highReg, rlSrc2.highReg);
  NewLIR3(cUnit, kMipsSlt, t1, rlSrc2.highReg, rlSrc1.highReg);
  NewLIR3(cUnit, kMipsSubu, rlResult.lowReg, t1, t0);
  LIR* branch = OpCmpImmBranch(cUnit, kCondNe, rlResult.lowReg, 0, NULL);
  NewLIR3(cUnit, kMipsSltu, t0, rlSrc1.lowReg, rlSrc2.lowReg);
  NewLIR3(cUnit, kMipsSltu, t1, rlSrc2.lowReg, rlSrc1.lowReg);
  NewLIR3(cUnit, kMipsSubu, rlResult.lowReg, t1, t0);
  FreeTemp(cUnit, t0);
  FreeTemp(cUnit, t1);
  LIR* target = NewLIR0(cUnit, kPseudoTargetLabel);
  branch->target = target;
  StoreValue(cUnit, rlDest, rlResult);
}

LIR* OpCmpBranch(CompilationUnit* cUnit, ConditionCode cond, int src1,
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
      LOG(FATAL) << "No support for ConditionCode: " << cond;
      return NULL;
  }
  if (cmpZero) {
    branch = NewLIR2(cUnit, brOp, src1, src2);
  } else {
    int tReg = AllocTemp(cUnit);
    if (swapped) {
      NewLIR3(cUnit, sltOp, tReg, src2, src1);
    } else {
      NewLIR3(cUnit, sltOp, tReg, src1, src2);
    }
    branch = NewLIR1(cUnit, brOp, tReg);
    FreeTemp(cUnit, tReg);
  }
  branch->target = target;
  return branch;
}

LIR* OpCmpImmBranch(CompilationUnit* cUnit, ConditionCode cond, int reg,
          int checkValue, LIR* target)
{
  LIR* branch;
  if (checkValue != 0) {
    // TUNING: handle s16 & kCondLt/Mi case using slti
    int tReg = AllocTemp(cUnit);
    LoadConstant(cUnit, tReg, checkValue);
    branch = OpCmpBranch(cUnit, cond, reg, tReg, target);
    FreeTemp(cUnit, tReg);
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
      int tReg = AllocTemp(cUnit);
      LoadConstant(cUnit, tReg, checkValue);
      branch = OpCmpBranch(cUnit, cond, reg, tReg, target);
      FreeTemp(cUnit, tReg);
      return branch;
  }
  branch = NewLIR1(cUnit, opc, reg);
  branch->target = target;
  return branch;
}

LIR* OpRegCopyNoInsert(CompilationUnit *cUnit, int rDest, int rSrc)
{
#ifdef __mips_hard_float
  if (MIPS_FPREG(rDest) || MIPS_FPREG(rSrc))
    return FpRegCopy(cUnit, rDest, rSrc);
#endif
  LIR* res = RawLIR(cUnit, cUnit->currentDalvikOffset, kMipsMove,
            rDest, rSrc);
  if (!(cUnit->disableOpt & (1 << kSafeOptimizations)) && rDest == rSrc) {
    res->flags.isNop = true;
  }
  return res;
}

LIR* OpRegCopy(CompilationUnit *cUnit, int rDest, int rSrc)
{
  LIR *res = OpRegCopyNoInsert(cUnit, rDest, rSrc);
  AppendLIR(cUnit, res);
  return res;
}

void OpRegCopyWide(CompilationUnit *cUnit, int destLo, int destHi,
          int srcLo, int srcHi)
{
#ifdef __mips_hard_float
  bool destFP = MIPS_FPREG(destLo) && MIPS_FPREG(destHi);
  bool srcFP = MIPS_FPREG(srcLo) && MIPS_FPREG(srcHi);
  assert(MIPS_FPREG(srcLo) == MIPS_FPREG(srcHi));
  assert(MIPS_FPREG(destLo) == MIPS_FPREG(destHi));
  if (destFP) {
    if (srcFP) {
      OpRegCopy(cUnit, S2d(destLo, destHi), S2d(srcLo, srcHi));
    } else {
       /* note the operands are swapped for the mtc1 instr */
      NewLIR2(cUnit, kMipsMtc1, srcLo, destLo);
      NewLIR2(cUnit, kMipsMtc1, srcHi, destHi);
    }
  } else {
    if (srcFP) {
      NewLIR2(cUnit, kMipsMfc1, destLo, srcLo);
      NewLIR2(cUnit, kMipsMfc1, destHi, srcHi);
    } else {
      // Handle overlap
      if (srcHi == destLo) {
        OpRegCopy(cUnit, destHi, srcHi);
        OpRegCopy(cUnit, destLo, srcLo);
      } else {
        OpRegCopy(cUnit, destLo, srcLo);
        OpRegCopy(cUnit, destHi, srcHi);
      }
    }
  }
#else
  // Handle overlap
  if (srcHi == destLo) {
    OpRegCopy(cUnit, destHi, srcHi);
    OpRegCopy(cUnit, destLo, srcLo);
  } else {
    OpRegCopy(cUnit, destLo, srcLo);
    OpRegCopy(cUnit, destHi, srcHi);
  }
#endif
}

void GenFusedLongCmpBranch(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir)
{
  UNIMPLEMENTED(FATAL) << "Need codegen for fused long cmp branch";
}

LIR* GenRegMemCheck(CompilationUnit* cUnit, ConditionCode cCode,
                    int reg1, int base, int offset, ThrowKind kind)
{
  LOG(FATAL) << "Unexpected use of GenRegMemCheck for Arm";
  return NULL;
}

RegLocation GenDivRem(CompilationUnit* cUnit, RegLocation rlDest, int reg1, int reg2, bool isDiv)
{
  NewLIR4(cUnit, kMipsDiv, r_HI, r_LO, reg1, reg2);
  RegLocation rlResult = EvalLoc(cUnit, rlDest, kCoreReg, true);
  if (isDiv) {
    NewLIR2(cUnit, kMipsMflo, rlResult.lowReg, r_LO);
  } else {
    NewLIR2(cUnit, kMipsMfhi, rlResult.lowReg, r_HI);
  }
  return rlResult;
}

RegLocation GenDivRemLit(CompilationUnit* cUnit, RegLocation rlDest, int reg1, int lit, bool isDiv)
{
  int tReg = AllocTemp(cUnit);
  NewLIR3(cUnit, kMipsAddiu, tReg, r_ZERO, lit);
  NewLIR4(cUnit, kMipsDiv, r_HI, r_LO, reg1, tReg);
  RegLocation rlResult = EvalLoc(cUnit, rlDest, kCoreReg, true);
  if (isDiv) {
    NewLIR2(cUnit, kMipsMflo, rlResult.lowReg, r_LO);
  } else {
    NewLIR2(cUnit, kMipsMfhi, rlResult.lowReg, r_HI);
  }
  FreeTemp(cUnit, tReg);
  return rlResult;
}

void OpLea(CompilationUnit* cUnit, int rBase, int reg1, int reg2, int scale, int offset)
{
  LOG(FATAL) << "Unexpected use of OpLea for Arm";
}

void OpTlsCmp(CompilationUnit* cUnit, int offset, int val)
{
  LOG(FATAL) << "Unexpected use of OpTlsCmp for Arm";
}

bool GenInlinedCas32(CompilationUnit* cUnit, CallInfo* info, bool need_write_barrier) {
  DCHECK_NE(cUnit->instructionSet, kThumb2);
  return false;
}

bool GenInlinedSqrt(CompilationUnit* cUnit, CallInfo* info) {
  DCHECK_NE(cUnit->instructionSet, kThumb2);
  return false;
}

LIR* OpPcRelLoad(CompilationUnit* cUnit, int reg, LIR* target) {
  LOG(FATAL) << "Unexpected use of OpPcRelLoad for Mips";
  return NULL;
}

LIR* OpVldm(CompilationUnit* cUnit, int rBase, int count)
{
  LOG(FATAL) << "Unexpected use of OpVldm for Mips";
  return NULL;
}

LIR* OpVstm(CompilationUnit* cUnit, int rBase, int count)
{
  LOG(FATAL) << "Unexpected use of OpVstm for Mips";
  return NULL;
}

void GenMultiplyByTwoBitMultiplier(CompilationUnit* cUnit, RegLocation rlSrc,
                                   RegLocation rlResult, int lit,
                                   int firstBit, int secondBit)
{
  int tReg = AllocTemp(cUnit);
  OpRegRegImm(cUnit, kOpLsl, tReg, rlSrc.lowReg, secondBit - firstBit);
  OpRegRegReg(cUnit, kOpAdd, rlResult.lowReg, rlSrc.lowReg, tReg);
  FreeTemp(cUnit, tReg);
  if (firstBit != 0) {
    OpRegRegImm(cUnit, kOpLsl, rlResult.lowReg, rlResult.lowReg, firstBit);
  }
}

void GenDivZeroCheck(CompilationUnit* cUnit, int regLo, int regHi)
{
  int tReg = AllocTemp(cUnit);
  OpRegRegReg(cUnit, kOpOr, tReg, regLo, regHi);
  GenImmedCheck(cUnit, kCondEq, tReg, 0, kThrowDivZero);
  FreeTemp(cUnit, tReg);
}

// Test suspend flag, return target of taken suspend branch
LIR* OpTestSuspend(CompilationUnit* cUnit, LIR* target)
{
  OpRegImm(cUnit, kOpSub, rMIPS_SUSPEND, 1);
  return OpCmpImmBranch(cUnit, (target == NULL) ? kCondEq : kCondNe, rMIPS_SUSPEND, 0, target);
}

// Decrement register and branch on condition
LIR* OpDecAndBranch(CompilationUnit* cUnit, ConditionCode cCode, int reg, LIR* target)
{
  OpRegImm(cUnit, kOpSub, reg, 1);
  return OpCmpImmBranch(cUnit, cCode, reg, 0, target);
}

bool SmallLiteralDivide(CompilationUnit* cUnit, Instruction::Code dalvikOpcode,
                        RegLocation rlSrc, RegLocation rlDest, int lit)
{
  LOG(FATAL) << "Unexpected use of smallLiteralDive in Mips";
  return false;
}

LIR* OpIT(CompilationUnit* cUnit, ArmConditionCode cond, const char* guide)
{
  LOG(FATAL) << "Unexpected use of OpIT in Mips";
  return NULL;
}

bool GenAddLong(CompilationUnit* cUnit, RegLocation rlDest,
                RegLocation rlSrc1, RegLocation rlSrc2)
{
  rlSrc1 = LoadValueWide(cUnit, rlSrc1, kCoreReg);
  rlSrc2 = LoadValueWide(cUnit, rlSrc2, kCoreReg);
  RegLocation rlResult = EvalLoc(cUnit, rlDest, kCoreReg, true);
  /*
   *  [v1 v0] =  [a1 a0] + [a3 a2];
   *  addu v0,a2,a0
   *  addu t1,a3,a1
   *  sltu v1,v0,a2
   *  addu v1,v1,t1
   */

  OpRegRegReg(cUnit, kOpAdd, rlResult.lowReg, rlSrc2.lowReg, rlSrc1.lowReg);
  int tReg = AllocTemp(cUnit);
  OpRegRegReg(cUnit, kOpAdd, tReg, rlSrc2.highReg, rlSrc1.highReg);
  NewLIR3(cUnit, kMipsSltu, rlResult.highReg, rlResult.lowReg, rlSrc2.lowReg);
  OpRegRegReg(cUnit, kOpAdd, rlResult.highReg, rlResult.highReg, tReg);
  FreeTemp(cUnit, tReg);
  StoreValueWide(cUnit, rlDest, rlResult);
  return false;
}

bool GenSubLong(CompilationUnit* cUnit, RegLocation rlDest,
        RegLocation rlSrc1, RegLocation rlSrc2)
{
  rlSrc1 = LoadValueWide(cUnit, rlSrc1, kCoreReg);
  rlSrc2 = LoadValueWide(cUnit, rlSrc2, kCoreReg);
  RegLocation rlResult = EvalLoc(cUnit, rlDest, kCoreReg, true);
  /*
   *  [v1 v0] =  [a1 a0] - [a3 a2];
   *  sltu  t1,a0,a2
   *  subu  v0,a0,a2
   *  subu  v1,a1,a3
   *  subu  v1,v1,t1
   */

  int tReg = AllocTemp(cUnit);
  NewLIR3(cUnit, kMipsSltu, tReg, rlSrc1.lowReg, rlSrc2.lowReg);
  OpRegRegReg(cUnit, kOpSub, rlResult.lowReg, rlSrc1.lowReg, rlSrc2.lowReg);
  OpRegRegReg(cUnit, kOpSub, rlResult.highReg, rlSrc1.highReg, rlSrc2.highReg);
  OpRegRegReg(cUnit, kOpSub, rlResult.highReg, rlResult.highReg, tReg);
  FreeTemp(cUnit, tReg);
  StoreValueWide(cUnit, rlDest, rlResult);
  return false;
}

bool GenNegLong(CompilationUnit* cUnit, RegLocation rlDest,
                RegLocation rlSrc)
{
  rlSrc = LoadValueWide(cUnit, rlSrc, kCoreReg);
  RegLocation rlResult = EvalLoc(cUnit, rlDest, kCoreReg, true);
  /*
   *  [v1 v0] =  -[a1 a0]
   *  negu  v0,a0
   *  negu  v1,a1
   *  sltu  t1,r_zero
   *  subu  v1,v1,t1
   */

  OpRegReg(cUnit, kOpNeg, rlResult.lowReg, rlSrc.lowReg);
  OpRegReg(cUnit, kOpNeg, rlResult.highReg, rlSrc.highReg);
  int tReg = AllocTemp(cUnit);
  NewLIR3(cUnit, kMipsSltu, tReg, r_ZERO, rlResult.lowReg);
  OpRegRegReg(cUnit, kOpSub, rlResult.highReg, rlResult.highReg, tReg);
  FreeTemp(cUnit, tReg);
  StoreValueWide(cUnit, rlDest, rlResult);
  return false;
}

bool GenAndLong(CompilationUnit* cUnit, RegLocation rlDest,
                RegLocation rlSrc1, RegLocation rlSrc2)
{
  LOG(FATAL) << "Unexpected use of GenAndLong for Mips";
  return false;
}

bool GenOrLong(CompilationUnit* cUnit, RegLocation rlDest,
               RegLocation rlSrc1, RegLocation rlSrc2)
{
  LOG(FATAL) << "Unexpected use of GenOrLong for Mips";
  return false;
}

bool GenXorLong(CompilationUnit* cUnit, RegLocation rlDest,
               RegLocation rlSrc1, RegLocation rlSrc2)
{
  LOG(FATAL) << "Unexpected use of GenXorLong for Mips";
  return false;
}

}  // namespace art
