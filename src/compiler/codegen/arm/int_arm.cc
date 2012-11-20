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

/* This file contains codegen for the Thumb2 ISA. */

#include "oat_compilation_unit.h"
#include "oat/runtime/oat_support_entrypoints.h"
#include "arm_lir.h"
#include "../codegen_util.h"
#include "../ralloc_util.h"

namespace art {

LIR* opCmpBranch(CompilationUnit* cUnit, ConditionCode cond, int src1,
         int src2, LIR* target)
{
  opRegReg(cUnit, kOpCmp, src1, src2);
  return opCondBranch(cUnit, cond, target);
}

/*
 * Generate a Thumb2 IT instruction, which can nullify up to
 * four subsequent instructions based on a condition and its
 * inverse.  The condition applies to the first instruction, which
 * is executed if the condition is met.  The string "guide" consists
 * of 0 to 3 chars, and applies to the 2nd through 4th instruction.
 * A "T" means the instruction is executed if the condition is
 * met, and an "E" means the instruction is executed if the condition
 * is not met.
 */
LIR* opIT(CompilationUnit* cUnit, ArmConditionCode code, const char* guide)
{
  int mask;
  int condBit = code & 1;
  int altBit = condBit ^ 1;
  int mask3 = 0;
  int mask2 = 0;
  int mask1 = 0;

  //Note: case fallthroughs intentional
  switch (strlen(guide)) {
    case 3:
      mask1 = (guide[2] == 'T') ? condBit : altBit;
    case 2:
      mask2 = (guide[1] == 'T') ? condBit : altBit;
    case 1:
      mask3 = (guide[0] == 'T') ? condBit : altBit;
      break;
    case 0:
      break;
    default:
      LOG(FATAL) << "OAT: bad case in opIT";
  }
  mask = (mask3 << 3) | (mask2 << 2) | (mask1 << 1) |
       (1 << (3 - strlen(guide)));
  return newLIR2(cUnit, kThumb2It, code, mask);
}

/*
 * 64-bit 3way compare function.
 *     mov   rX, #-1
 *     cmp   op1hi, op2hi
 *     blt   done
 *     bgt   flip
 *     sub   rX, op1lo, op2lo (treat as unsigned)
 *     beq   done
 *     ite   hi
 *     mov(hi)   rX, #-1
 *     mov(!hi)  rX, #1
 * flip:
 *     neg   rX
 * done:
 */
void genCmpLong(CompilationUnit* cUnit, RegLocation rlDest,
        RegLocation rlSrc1, RegLocation rlSrc2)
{
  LIR* target1;
  LIR* target2;
  rlSrc1 = loadValueWide(cUnit, rlSrc1, kCoreReg);
  rlSrc2 = loadValueWide(cUnit, rlSrc2, kCoreReg);
  int tReg = oatAllocTemp(cUnit);
  loadConstant(cUnit, tReg, -1);
  opRegReg(cUnit, kOpCmp, rlSrc1.highReg, rlSrc2.highReg);
  LIR* branch1 = opCondBranch(cUnit, kCondLt, NULL);
  LIR* branch2 = opCondBranch(cUnit, kCondGt, NULL);
  opRegRegReg(cUnit, kOpSub, tReg, rlSrc1.lowReg, rlSrc2.lowReg);
  LIR* branch3 = opCondBranch(cUnit, kCondEq, NULL);

  opIT(cUnit, kArmCondHi, "E");
  newLIR2(cUnit, kThumb2MovImmShift, tReg, modifiedImmediate(-1));
  loadConstant(cUnit, tReg, 1);
  genBarrier(cUnit);

  target2 = newLIR0(cUnit, kPseudoTargetLabel);
  opRegReg(cUnit, kOpNeg, tReg, tReg);

  target1 = newLIR0(cUnit, kPseudoTargetLabel);

  RegLocation rlTemp = locCReturn(); // Just using as template, will change
  rlTemp.lowReg = tReg;
  storeValue(cUnit, rlDest, rlTemp);
  oatFreeTemp(cUnit, tReg);

  branch1->target = target1;
  branch2->target = target2;
  branch3->target = branch1->target;
}

void genFusedLongCmpBranch(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir)
{
  LIR* labelList = cUnit->blockLabelList;
  LIR* taken = &labelList[bb->taken->id];
  LIR* notTaken = &labelList[bb->fallThrough->id];
  RegLocation rlSrc1 = oatGetSrcWide(cUnit, mir, 0);
  RegLocation rlSrc2 = oatGetSrcWide(cUnit, mir, 2);
  rlSrc1 = loadValueWide(cUnit, rlSrc1, kCoreReg);
  rlSrc2 = loadValueWide(cUnit, rlSrc2, kCoreReg);
  ConditionCode ccode = static_cast<ConditionCode>(mir->dalvikInsn.arg[0]);
  opRegReg(cUnit, kOpCmp, rlSrc1.highReg, rlSrc2.highReg);
  switch(ccode) {
    case kCondEq:
      opCondBranch(cUnit, kCondNe, notTaken);
      break;
    case kCondNe:
      opCondBranch(cUnit, kCondNe, taken);
      break;
    case kCondLt:
      opCondBranch(cUnit, kCondLt, taken);
      opCondBranch(cUnit, kCondGt, notTaken);
      ccode = kCondCc;
      break;
    case kCondLe:
      opCondBranch(cUnit, kCondLt, taken);
      opCondBranch(cUnit, kCondGt, notTaken);
      ccode = kCondLs;
      break;
    case kCondGt:
      opCondBranch(cUnit, kCondGt, taken);
      opCondBranch(cUnit, kCondLt, notTaken);
      ccode = kCondHi;
      break;
    case kCondGe:
      opCondBranch(cUnit, kCondGt, taken);
      opCondBranch(cUnit, kCondLt, notTaken);
      ccode = kCondCs;
      break;
    default:
      LOG(FATAL) << "Unexpected ccode: " << ccode;
  }
  opRegReg(cUnit, kOpCmp, rlSrc1.lowReg, rlSrc2.lowReg);
  opCondBranch(cUnit, ccode, taken);
}

/*
 * Generate a register comparison to an immediate and branch.  Caller
 * is responsible for setting branch target field.
 */
LIR* opCmpImmBranch(CompilationUnit* cUnit, ConditionCode cond, int reg,
          int checkValue, LIR* target)
{
  LIR* branch;
  int modImm;
  ArmConditionCode armCond = oatArmConditionEncoding(cond);
  if ((ARM_LOWREG(reg)) && (checkValue == 0) &&
     ((armCond == kArmCondEq) || (armCond == kArmCondNe))) {
    branch = newLIR2(cUnit, (armCond == kArmCondEq) ? kThumb2Cbz : kThumb2Cbnz,
                     reg, 0);
  } else {
    modImm = modifiedImmediate(checkValue);
    if (ARM_LOWREG(reg) && ((checkValue & 0xff) == checkValue)) {
      newLIR2(cUnit, kThumbCmpRI8, reg, checkValue);
    } else if (modImm >= 0) {
      newLIR2(cUnit, kThumb2CmpRI8, reg, modImm);
    } else {
      int tReg = oatAllocTemp(cUnit);
      loadConstant(cUnit, tReg, checkValue);
      opRegReg(cUnit, kOpCmp, reg, tReg);
    }
    branch = newLIR2(cUnit, kThumbBCond, 0, armCond);
  }
  branch->target = target;
  return branch;
}
LIR* opRegCopyNoInsert(CompilationUnit* cUnit, int rDest, int rSrc)
{
  LIR* res;
  int opcode;
  if (ARM_FPREG(rDest) || ARM_FPREG(rSrc))
    return fpRegCopy(cUnit, rDest, rSrc);
  if (ARM_LOWREG(rDest) && ARM_LOWREG(rSrc))
    opcode = kThumbMovRR;
  else if (!ARM_LOWREG(rDest) && !ARM_LOWREG(rSrc))
     opcode = kThumbMovRR_H2H;
  else if (ARM_LOWREG(rDest))
     opcode = kThumbMovRR_H2L;
  else
     opcode = kThumbMovRR_L2H;
  res = rawLIR(cUnit, cUnit->currentDalvikOffset, opcode, rDest, rSrc);
  if (!(cUnit->disableOpt & (1 << kSafeOptimizations)) && rDest == rSrc) {
    res->flags.isNop = true;
  }
  return res;
}

LIR* opRegCopy(CompilationUnit* cUnit, int rDest, int rSrc)
{
  LIR* res = opRegCopyNoInsert(cUnit, rDest, rSrc);
  oatAppendLIR(cUnit, res);
  return res;
}

void opRegCopyWide(CompilationUnit* cUnit, int destLo, int destHi,
               int srcLo, int srcHi)
{
  bool destFP = ARM_FPREG(destLo) && ARM_FPREG(destHi);
  bool srcFP = ARM_FPREG(srcLo) && ARM_FPREG(srcHi);
  DCHECK_EQ(ARM_FPREG(srcLo), ARM_FPREG(srcHi));
  DCHECK_EQ(ARM_FPREG(destLo), ARM_FPREG(destHi));
  if (destFP) {
    if (srcFP) {
      opRegCopy(cUnit, s2d(destLo, destHi), s2d(srcLo, srcHi));
    } else {
      newLIR3(cUnit, kThumb2Fmdrr, s2d(destLo, destHi), srcLo, srcHi);
    }
  } else {
    if (srcFP) {
      newLIR3(cUnit, kThumb2Fmrrd, destLo, destHi, s2d(srcLo, srcHi));
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
}

// Table of magic divisors
struct MagicTable {
  uint32_t magic;
  uint32_t shift;
  DividePattern pattern;
};

static const MagicTable magicTable[] = {
  {0, 0, DivideNone},        // 0
  {0, 0, DivideNone},        // 1
  {0, 0, DivideNone},        // 2
  {0x55555556, 0, Divide3},  // 3
  {0, 0, DivideNone},        // 4
  {0x66666667, 1, Divide5},  // 5
  {0x2AAAAAAB, 0, Divide3},  // 6
  {0x92492493, 2, Divide7},  // 7
  {0, 0, DivideNone},        // 8
  {0x38E38E39, 1, Divide5},  // 9
  {0x66666667, 2, Divide5},  // 10
  {0x2E8BA2E9, 1, Divide5},  // 11
  {0x2AAAAAAB, 1, Divide5},  // 12
  {0x4EC4EC4F, 2, Divide5},  // 13
  {0x92492493, 3, Divide7},  // 14
  {0x88888889, 3, Divide7},  // 15
};

// Integer division by constant via reciprocal multiply (Hacker's Delight, 10-4)
bool smallLiteralDivide(CompilationUnit* cUnit, Instruction::Code dalvikOpcode,
                        RegLocation rlSrc, RegLocation rlDest, int lit)
{
  if ((lit < 0) || (lit >= static_cast<int>(sizeof(magicTable)/sizeof(magicTable[0])))) {
    return false;
  }
  DividePattern pattern = magicTable[lit].pattern;
  if (pattern == DivideNone) {
    return false;
  }
  // Tuning: add rem patterns
  if (dalvikOpcode != Instruction::DIV_INT_LIT8) {
    return false;
  }

  int rMagic = oatAllocTemp(cUnit);
  loadConstant(cUnit, rMagic, magicTable[lit].magic);
  rlSrc = loadValue(cUnit, rlSrc, kCoreReg);
  RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
  int rHi = oatAllocTemp(cUnit);
  int rLo = oatAllocTemp(cUnit);
  newLIR4(cUnit, kThumb2Smull, rLo, rHi, rMagic, rlSrc.lowReg);
  switch(pattern) {
    case Divide3:
      opRegRegRegShift(cUnit, kOpSub, rlResult.lowReg, rHi,
               rlSrc.lowReg, encodeShift(kArmAsr, 31));
      break;
    case Divide5:
      opRegRegImm(cUnit, kOpAsr, rLo, rlSrc.lowReg, 31);
      opRegRegRegShift(cUnit, kOpRsub, rlResult.lowReg, rLo, rHi,
               encodeShift(kArmAsr, magicTable[lit].shift));
      break;
    case Divide7:
      opRegReg(cUnit, kOpAdd, rHi, rlSrc.lowReg);
      opRegRegImm(cUnit, kOpAsr, rLo, rlSrc.lowReg, 31);
      opRegRegRegShift(cUnit, kOpRsub, rlResult.lowReg, rLo, rHi,
               encodeShift(kArmAsr, magicTable[lit].shift));
      break;
    default:
      LOG(FATAL) << "Unexpected pattern: " << pattern;
  }
  storeValue(cUnit, rlDest, rlResult);
  return true;
}

LIR* genRegMemCheck(CompilationUnit* cUnit, ConditionCode cCode,
                    int reg1, int base, int offset, ThrowKind kind)
{
  LOG(FATAL) << "Unexpected use of genRegMemCheck for Arm";
  return NULL;
}

RegLocation genDivRemLit(CompilationUnit* cUnit, RegLocation rlDest, int reg1, int lit, bool isDiv)
{
  LOG(FATAL) << "Unexpected use of genDivRemLit for Arm";
  return rlDest;
}

RegLocation genDivRem(CompilationUnit* cUnit, RegLocation rlDest, int reg1, int reg2, bool isDiv)
{
  LOG(FATAL) << "Unexpected use of genDivRem for Arm";
  return rlDest;
}

bool genInlinedMinMaxInt(CompilationUnit *cUnit, CallInfo* info, bool isMin)
{
  DCHECK_EQ(cUnit->instructionSet, kThumb2);
  RegLocation rlSrc1 = info->args[0];
  RegLocation rlSrc2 = info->args[1];
  rlSrc1 = loadValue(cUnit, rlSrc1, kCoreReg);
  rlSrc2 = loadValue(cUnit, rlSrc2, kCoreReg);
  RegLocation rlDest = inlineTarget(cUnit, info);
  RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
  opRegReg(cUnit, kOpCmp, rlSrc1.lowReg, rlSrc2.lowReg);
  opIT(cUnit, (isMin) ? kArmCondGt : kArmCondLt, "E");
  opRegReg(cUnit, kOpMov, rlResult.lowReg, rlSrc2.lowReg);
  opRegReg(cUnit, kOpMov, rlResult.lowReg, rlSrc1.lowReg);
  genBarrier(cUnit);
  storeValue(cUnit, rlDest, rlResult);
  return true;
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
  DCHECK_EQ(cUnit->instructionSet, kThumb2);
  // Unused - RegLocation rlSrcUnsafe = info->args[0];
  RegLocation rlSrcObj= info->args[1];  // Object - known non-null
  RegLocation rlSrcOffset= info->args[2];  // long low
  rlSrcOffset.wide = 0;  // ignore high half in info->args[3]
  RegLocation rlSrcExpected= info->args[4];  // int or Object
  RegLocation rlSrcNewValue= info->args[5];  // int or Object
  RegLocation rlDest = inlineTarget(cUnit, info);  // boolean place for result


  // Release store semantics, get the barrier out of the way.  TODO: revisit
  oatGenMemBarrier(cUnit, kStoreLoad);

  RegLocation rlObject = loadValue(cUnit, rlSrcObj, kCoreReg);
  RegLocation rlNewValue = loadValue(cUnit, rlSrcNewValue, kCoreReg);

  if (need_write_barrier) {
    // Mark card for object assuming new value is stored.
    markGCCard(cUnit, rlNewValue.lowReg, rlObject.lowReg);
  }

  RegLocation rlOffset = loadValue(cUnit, rlSrcOffset, kCoreReg);

  int rPtr = oatAllocTemp(cUnit);
  opRegRegReg(cUnit, kOpAdd, rPtr, rlObject.lowReg, rlOffset.lowReg);

  // Free now unneeded rlObject and rlOffset to give more temps.
  oatClobberSReg(cUnit, rlObject.sRegLow);
  oatFreeTemp(cUnit, rlObject.lowReg);
  oatClobberSReg(cUnit, rlOffset.sRegLow);
  oatFreeTemp(cUnit, rlOffset.lowReg);

  int rOldValue = oatAllocTemp(cUnit);
  newLIR3(cUnit, kThumb2Ldrex, rOldValue, rPtr, 0);  // rOldValue := [rPtr]

  RegLocation rlExpected = loadValue(cUnit, rlSrcExpected, kCoreReg);

  // if (rOldValue == rExpected) {
  //   [rPtr] <- rNewValue && rResult := success ? 0 : 1
  //   rResult ^= 1
  // } else {
  //   rResult := 0
  // }
  opRegReg(cUnit, kOpCmp, rOldValue, rlExpected.lowReg);
  oatFreeTemp(cUnit, rOldValue);  // Now unneeded.
  RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
  opIT(cUnit, kArmCondEq, "TE");
  newLIR4(cUnit, kThumb2Strex, rlResult.lowReg, rlNewValue.lowReg, rPtr, 0);
  oatFreeTemp(cUnit, rPtr);  // Now unneeded.
  opRegImm(cUnit, kOpXor, rlResult.lowReg, 1);
  opRegReg(cUnit, kOpXor, rlResult.lowReg, rlResult.lowReg);

  storeValue(cUnit, rlDest, rlResult);

  return true;
}

LIR* opPcRelLoad(CompilationUnit* cUnit, int reg, LIR* target)
{
  return rawLIR(cUnit, cUnit->currentDalvikOffset, kThumb2LdrPcRel12, reg, 0, 0, 0, 0, target);
}

LIR* opVldm(CompilationUnit* cUnit, int rBase, int count)
{
  return newLIR3(cUnit, kThumb2Vldms, rBase, fr0, count);
}

LIR* opVstm(CompilationUnit* cUnit, int rBase, int count)
{
  return newLIR3(cUnit, kThumb2Vstms, rBase, fr0, count);
}

void genMultiplyByTwoBitMultiplier(CompilationUnit* cUnit, RegLocation rlSrc,
                                   RegLocation rlResult, int lit,
                                   int firstBit, int secondBit)
{
  opRegRegRegShift(cUnit, kOpAdd, rlResult.lowReg, rlSrc.lowReg, rlSrc.lowReg,
                   encodeShift(kArmLsl, secondBit - firstBit));
  if (firstBit != 0) {
    opRegRegImm(cUnit, kOpLsl, rlResult.lowReg, rlResult.lowReg, firstBit);
  }
}

void genDivZeroCheck(CompilationUnit* cUnit, int regLo, int regHi)
{
  int tReg = oatAllocTemp(cUnit);
  newLIR4(cUnit, kThumb2OrrRRRs, tReg, regLo, regHi, 0);
  oatFreeTemp(cUnit, tReg);
  genCheck(cUnit, kCondEq, kThrowDivZero);
}

// Test suspend flag, return target of taken suspend branch
LIR* opTestSuspend(CompilationUnit* cUnit, LIR* target)
{
  newLIR2(cUnit, kThumbSubRI8, rARM_SUSPEND, 1);
  return opCondBranch(cUnit, (target == NULL) ? kCondEq : kCondNe, target);
}

// Decrement register and branch on condition
LIR* opDecAndBranch(CompilationUnit* cUnit, ConditionCode cCode, int reg, LIR* target)
{
  // Combine sub & test using sub setflags encoding here
  newLIR3(cUnit, kThumb2SubsRRI12, reg, reg, 1);
  return opCondBranch(cUnit, cCode, target);
}

void oatGenMemBarrier(CompilationUnit* cUnit, MemBarrierKind barrierKind)
{
#if ANDROID_SMP != 0
  int dmbFlavor;
  // TODO: revisit Arm barrier kinds
  switch (barrierKind) {
    case kLoadStore: dmbFlavor = kSY; break;
    case kLoadLoad: dmbFlavor = kSY; break;
    case kStoreStore: dmbFlavor = kST; break;
    case kStoreLoad: dmbFlavor = kSY; break;
    default:
      LOG(FATAL) << "Unexpected MemBarrierKind: " << barrierKind;
      dmbFlavor = kSY;  // quiet gcc.
      break;
  }
  LIR* dmb = newLIR1(cUnit, kThumb2Dmb, dmbFlavor);
  dmb->defMask = ENCODE_ALL;
#endif
}

bool genNegLong(CompilationUnit* cUnit, RegLocation rlDest,
                RegLocation rlSrc)
{
  rlSrc = loadValueWide(cUnit, rlSrc, kCoreReg);
  RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
  int zReg = oatAllocTemp(cUnit);
  loadConstantNoClobber(cUnit, zReg, 0);
  // Check for destructive overlap
  if (rlResult.lowReg == rlSrc.highReg) {
    int tReg = oatAllocTemp(cUnit);
    opRegRegReg(cUnit, kOpSub, rlResult.lowReg, zReg, rlSrc.lowReg);
    opRegRegReg(cUnit, kOpSbc, rlResult.highReg, zReg, tReg);
    oatFreeTemp(cUnit, tReg);
  } else {
    opRegRegReg(cUnit, kOpSub, rlResult.lowReg, zReg, rlSrc.lowReg);
    opRegRegReg(cUnit, kOpSbc, rlResult.highReg, zReg, rlSrc.highReg);
  }
  oatFreeTemp(cUnit, zReg);
  storeValueWide(cUnit, rlDest, rlResult);
  return false;
}

bool genAddLong(CompilationUnit* cUnit, RegLocation rlDest,
                RegLocation rlSrc1, RegLocation rlSrc2)
{
  LOG(FATAL) << "Unexpected use of genAddLong for Arm";
  return false;
}

bool genSubLong(CompilationUnit* cUnit, RegLocation rlDest,
                RegLocation rlSrc1, RegLocation rlSrc2)
{
  LOG(FATAL) << "Unexpected use of genSubLong for Arm";
  return false;
}

bool genAndLong(CompilationUnit* cUnit, RegLocation rlDest,
                RegLocation rlSrc1, RegLocation rlSrc2)
{
  LOG(FATAL) << "Unexpected use of genAndLong for Arm";
  return false;
}

bool genOrLong(CompilationUnit* cUnit, RegLocation rlDest,
               RegLocation rlSrc1, RegLocation rlSrc2)
{
  LOG(FATAL) << "Unexpected use of genOrLong for Arm";
  return false;
}

bool genXorLong(CompilationUnit* cUnit, RegLocation rlDest,
               RegLocation rlSrc1, RegLocation rlSrc2)
{
  LOG(FATAL) << "Unexpected use of genXoLong for Arm";
  return false;
}

}  // namespace art
