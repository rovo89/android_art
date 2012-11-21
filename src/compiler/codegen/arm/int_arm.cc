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

LIR* OpCmpBranch(CompilationUnit* cUnit, ConditionCode cond, int src1,
         int src2, LIR* target)
{
  OpRegReg(cUnit, kOpCmp, src1, src2);
  return OpCondBranch(cUnit, cond, target);
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
LIR* OpIT(CompilationUnit* cUnit, ArmConditionCode code, const char* guide)
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
      LOG(FATAL) << "OAT: bad case in OpIT";
  }
  mask = (mask3 << 3) | (mask2 << 2) | (mask1 << 1) |
       (1 << (3 - strlen(guide)));
  return NewLIR2(cUnit, kThumb2It, code, mask);
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
void GenCmpLong(CompilationUnit* cUnit, RegLocation rlDest,
        RegLocation rlSrc1, RegLocation rlSrc2)
{
  LIR* target1;
  LIR* target2;
  rlSrc1 = LoadValueWide(cUnit, rlSrc1, kCoreReg);
  rlSrc2 = LoadValueWide(cUnit, rlSrc2, kCoreReg);
  int tReg = AllocTemp(cUnit);
  LoadConstant(cUnit, tReg, -1);
  OpRegReg(cUnit, kOpCmp, rlSrc1.highReg, rlSrc2.highReg);
  LIR* branch1 = OpCondBranch(cUnit, kCondLt, NULL);
  LIR* branch2 = OpCondBranch(cUnit, kCondGt, NULL);
  OpRegRegReg(cUnit, kOpSub, tReg, rlSrc1.lowReg, rlSrc2.lowReg);
  LIR* branch3 = OpCondBranch(cUnit, kCondEq, NULL);

  OpIT(cUnit, kArmCondHi, "E");
  NewLIR2(cUnit, kThumb2MovImmShift, tReg, ModifiedImmediate(-1));
  LoadConstant(cUnit, tReg, 1);
  GenBarrier(cUnit);

  target2 = NewLIR0(cUnit, kPseudoTargetLabel);
  OpRegReg(cUnit, kOpNeg, tReg, tReg);

  target1 = NewLIR0(cUnit, kPseudoTargetLabel);

  RegLocation rlTemp = LocCReturn(); // Just using as template, will change
  rlTemp.lowReg = tReg;
  StoreValue(cUnit, rlDest, rlTemp);
  FreeTemp(cUnit, tReg);

  branch1->target = target1;
  branch2->target = target2;
  branch3->target = branch1->target;
}

void GenFusedLongCmpBranch(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir)
{
  LIR* labelList = cUnit->blockLabelList;
  LIR* taken = &labelList[bb->taken->id];
  LIR* notTaken = &labelList[bb->fallThrough->id];
  RegLocation rlSrc1 = GetSrcWide(cUnit, mir, 0);
  RegLocation rlSrc2 = GetSrcWide(cUnit, mir, 2);
  rlSrc1 = LoadValueWide(cUnit, rlSrc1, kCoreReg);
  rlSrc2 = LoadValueWide(cUnit, rlSrc2, kCoreReg);
  ConditionCode ccode = static_cast<ConditionCode>(mir->dalvikInsn.arg[0]);
  OpRegReg(cUnit, kOpCmp, rlSrc1.highReg, rlSrc2.highReg);
  switch(ccode) {
    case kCondEq:
      OpCondBranch(cUnit, kCondNe, notTaken);
      break;
    case kCondNe:
      OpCondBranch(cUnit, kCondNe, taken);
      break;
    case kCondLt:
      OpCondBranch(cUnit, kCondLt, taken);
      OpCondBranch(cUnit, kCondGt, notTaken);
      ccode = kCondCc;
      break;
    case kCondLe:
      OpCondBranch(cUnit, kCondLt, taken);
      OpCondBranch(cUnit, kCondGt, notTaken);
      ccode = kCondLs;
      break;
    case kCondGt:
      OpCondBranch(cUnit, kCondGt, taken);
      OpCondBranch(cUnit, kCondLt, notTaken);
      ccode = kCondHi;
      break;
    case kCondGe:
      OpCondBranch(cUnit, kCondGt, taken);
      OpCondBranch(cUnit, kCondLt, notTaken);
      ccode = kCondCs;
      break;
    default:
      LOG(FATAL) << "Unexpected ccode: " << ccode;
  }
  OpRegReg(cUnit, kOpCmp, rlSrc1.lowReg, rlSrc2.lowReg);
  OpCondBranch(cUnit, ccode, taken);
}

/*
 * Generate a register comparison to an immediate and branch.  Caller
 * is responsible for setting branch target field.
 */
LIR* OpCmpImmBranch(CompilationUnit* cUnit, ConditionCode cond, int reg,
          int checkValue, LIR* target)
{
  LIR* branch;
  int modImm;
  ArmConditionCode armCond = ArmConditionEncoding(cond);
  if ((ARM_LOWREG(reg)) && (checkValue == 0) &&
     ((armCond == kArmCondEq) || (armCond == kArmCondNe))) {
    branch = NewLIR2(cUnit, (armCond == kArmCondEq) ? kThumb2Cbz : kThumb2Cbnz,
                     reg, 0);
  } else {
    modImm = ModifiedImmediate(checkValue);
    if (ARM_LOWREG(reg) && ((checkValue & 0xff) == checkValue)) {
      NewLIR2(cUnit, kThumbCmpRI8, reg, checkValue);
    } else if (modImm >= 0) {
      NewLIR2(cUnit, kThumb2CmpRI8, reg, modImm);
    } else {
      int tReg = AllocTemp(cUnit);
      LoadConstant(cUnit, tReg, checkValue);
      OpRegReg(cUnit, kOpCmp, reg, tReg);
    }
    branch = NewLIR2(cUnit, kThumbBCond, 0, armCond);
  }
  branch->target = target;
  return branch;
}
LIR* OpRegCopyNoInsert(CompilationUnit* cUnit, int rDest, int rSrc)
{
  LIR* res;
  int opcode;
  if (ARM_FPREG(rDest) || ARM_FPREG(rSrc))
    return FpRegCopy(cUnit, rDest, rSrc);
  if (ARM_LOWREG(rDest) && ARM_LOWREG(rSrc))
    opcode = kThumbMovRR;
  else if (!ARM_LOWREG(rDest) && !ARM_LOWREG(rSrc))
     opcode = kThumbMovRR_H2H;
  else if (ARM_LOWREG(rDest))
     opcode = kThumbMovRR_H2L;
  else
     opcode = kThumbMovRR_L2H;
  res = RawLIR(cUnit, cUnit->currentDalvikOffset, opcode, rDest, rSrc);
  if (!(cUnit->disableOpt & (1 << kSafeOptimizations)) && rDest == rSrc) {
    res->flags.isNop = true;
  }
  return res;
}

LIR* OpRegCopy(CompilationUnit* cUnit, int rDest, int rSrc)
{
  LIR* res = OpRegCopyNoInsert(cUnit, rDest, rSrc);
  AppendLIR(cUnit, res);
  return res;
}

void OpRegCopyWide(CompilationUnit* cUnit, int destLo, int destHi,
               int srcLo, int srcHi)
{
  bool destFP = ARM_FPREG(destLo) && ARM_FPREG(destHi);
  bool srcFP = ARM_FPREG(srcLo) && ARM_FPREG(srcHi);
  DCHECK_EQ(ARM_FPREG(srcLo), ARM_FPREG(srcHi));
  DCHECK_EQ(ARM_FPREG(destLo), ARM_FPREG(destHi));
  if (destFP) {
    if (srcFP) {
      OpRegCopy(cUnit, S2d(destLo, destHi), S2d(srcLo, srcHi));
    } else {
      NewLIR3(cUnit, kThumb2Fmdrr, S2d(destLo, destHi), srcLo, srcHi);
    }
  } else {
    if (srcFP) {
      NewLIR3(cUnit, kThumb2Fmrrd, destLo, destHi, S2d(srcLo, srcHi));
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
bool SmallLiteralDivide(CompilationUnit* cUnit, Instruction::Code dalvikOpcode,
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

  int rMagic = AllocTemp(cUnit);
  LoadConstant(cUnit, rMagic, magicTable[lit].magic);
  rlSrc = LoadValue(cUnit, rlSrc, kCoreReg);
  RegLocation rlResult = EvalLoc(cUnit, rlDest, kCoreReg, true);
  int rHi = AllocTemp(cUnit);
  int rLo = AllocTemp(cUnit);
  NewLIR4(cUnit, kThumb2Smull, rLo, rHi, rMagic, rlSrc.lowReg);
  switch(pattern) {
    case Divide3:
      OpRegRegRegShift(cUnit, kOpSub, rlResult.lowReg, rHi,
               rlSrc.lowReg, EncodeShift(kArmAsr, 31));
      break;
    case Divide5:
      OpRegRegImm(cUnit, kOpAsr, rLo, rlSrc.lowReg, 31);
      OpRegRegRegShift(cUnit, kOpRsub, rlResult.lowReg, rLo, rHi,
               EncodeShift(kArmAsr, magicTable[lit].shift));
      break;
    case Divide7:
      OpRegReg(cUnit, kOpAdd, rHi, rlSrc.lowReg);
      OpRegRegImm(cUnit, kOpAsr, rLo, rlSrc.lowReg, 31);
      OpRegRegRegShift(cUnit, kOpRsub, rlResult.lowReg, rLo, rHi,
               EncodeShift(kArmAsr, magicTable[lit].shift));
      break;
    default:
      LOG(FATAL) << "Unexpected pattern: " << pattern;
  }
  StoreValue(cUnit, rlDest, rlResult);
  return true;
}

LIR* GenRegMemCheck(CompilationUnit* cUnit, ConditionCode cCode,
                    int reg1, int base, int offset, ThrowKind kind)
{
  LOG(FATAL) << "Unexpected use of GenRegMemCheck for Arm";
  return NULL;
}

RegLocation GenDivRemLit(CompilationUnit* cUnit, RegLocation rlDest, int reg1, int lit, bool isDiv)
{
  LOG(FATAL) << "Unexpected use of GenDivRemLit for Arm";
  return rlDest;
}

RegLocation GenDivRem(CompilationUnit* cUnit, RegLocation rlDest, int reg1, int reg2, bool isDiv)
{
  LOG(FATAL) << "Unexpected use of GenDivRem for Arm";
  return rlDest;
}

bool GenInlinedMinMaxInt(CompilationUnit *cUnit, CallInfo* info, bool isMin)
{
  DCHECK_EQ(cUnit->instructionSet, kThumb2);
  RegLocation rlSrc1 = info->args[0];
  RegLocation rlSrc2 = info->args[1];
  rlSrc1 = LoadValue(cUnit, rlSrc1, kCoreReg);
  rlSrc2 = LoadValue(cUnit, rlSrc2, kCoreReg);
  RegLocation rlDest = InlineTarget(cUnit, info);
  RegLocation rlResult = EvalLoc(cUnit, rlDest, kCoreReg, true);
  OpRegReg(cUnit, kOpCmp, rlSrc1.lowReg, rlSrc2.lowReg);
  OpIT(cUnit, (isMin) ? kArmCondGt : kArmCondLt, "E");
  OpRegReg(cUnit, kOpMov, rlResult.lowReg, rlSrc2.lowReg);
  OpRegReg(cUnit, kOpMov, rlResult.lowReg, rlSrc1.lowReg);
  GenBarrier(cUnit);
  StoreValue(cUnit, rlDest, rlResult);
  return true;
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
  DCHECK_EQ(cUnit->instructionSet, kThumb2);
  // Unused - RegLocation rlSrcUnsafe = info->args[0];
  RegLocation rlSrcObj= info->args[1];  // Object - known non-null
  RegLocation rlSrcOffset= info->args[2];  // long low
  rlSrcOffset.wide = 0;  // ignore high half in info->args[3]
  RegLocation rlSrcExpected= info->args[4];  // int or Object
  RegLocation rlSrcNewValue= info->args[5];  // int or Object
  RegLocation rlDest = InlineTarget(cUnit, info);  // boolean place for result


  // Release store semantics, get the barrier out of the way.  TODO: revisit
  GenMemBarrier(cUnit, kStoreLoad);

  RegLocation rlObject = LoadValue(cUnit, rlSrcObj, kCoreReg);
  RegLocation rlNewValue = LoadValue(cUnit, rlSrcNewValue, kCoreReg);

  if (need_write_barrier) {
    // Mark card for object assuming new value is stored.
    MarkGCCard(cUnit, rlNewValue.lowReg, rlObject.lowReg);
  }

  RegLocation rlOffset = LoadValue(cUnit, rlSrcOffset, kCoreReg);

  int rPtr = AllocTemp(cUnit);
  OpRegRegReg(cUnit, kOpAdd, rPtr, rlObject.lowReg, rlOffset.lowReg);

  // Free now unneeded rlObject and rlOffset to give more temps.
  ClobberSReg(cUnit, rlObject.sRegLow);
  FreeTemp(cUnit, rlObject.lowReg);
  ClobberSReg(cUnit, rlOffset.sRegLow);
  FreeTemp(cUnit, rlOffset.lowReg);

  int rOldValue = AllocTemp(cUnit);
  NewLIR3(cUnit, kThumb2Ldrex, rOldValue, rPtr, 0);  // rOldValue := [rPtr]

  RegLocation rlExpected = LoadValue(cUnit, rlSrcExpected, kCoreReg);

  // if (rOldValue == rExpected) {
  //   [rPtr] <- rNewValue && rResult := success ? 0 : 1
  //   rResult ^= 1
  // } else {
  //   rResult := 0
  // }
  OpRegReg(cUnit, kOpCmp, rOldValue, rlExpected.lowReg);
  FreeTemp(cUnit, rOldValue);  // Now unneeded.
  RegLocation rlResult = EvalLoc(cUnit, rlDest, kCoreReg, true);
  OpIT(cUnit, kArmCondEq, "TE");
  NewLIR4(cUnit, kThumb2Strex, rlResult.lowReg, rlNewValue.lowReg, rPtr, 0);
  FreeTemp(cUnit, rPtr);  // Now unneeded.
  OpRegImm(cUnit, kOpXor, rlResult.lowReg, 1);
  OpRegReg(cUnit, kOpXor, rlResult.lowReg, rlResult.lowReg);

  StoreValue(cUnit, rlDest, rlResult);

  return true;
}

LIR* OpPcRelLoad(CompilationUnit* cUnit, int reg, LIR* target)
{
  return RawLIR(cUnit, cUnit->currentDalvikOffset, kThumb2LdrPcRel12, reg, 0, 0, 0, 0, target);
}

LIR* OpVldm(CompilationUnit* cUnit, int rBase, int count)
{
  return NewLIR3(cUnit, kThumb2Vldms, rBase, fr0, count);
}

LIR* OpVstm(CompilationUnit* cUnit, int rBase, int count)
{
  return NewLIR3(cUnit, kThumb2Vstms, rBase, fr0, count);
}

void GenMultiplyByTwoBitMultiplier(CompilationUnit* cUnit, RegLocation rlSrc,
                                   RegLocation rlResult, int lit,
                                   int firstBit, int secondBit)
{
  OpRegRegRegShift(cUnit, kOpAdd, rlResult.lowReg, rlSrc.lowReg, rlSrc.lowReg,
                   EncodeShift(kArmLsl, secondBit - firstBit));
  if (firstBit != 0) {
    OpRegRegImm(cUnit, kOpLsl, rlResult.lowReg, rlResult.lowReg, firstBit);
  }
}

void GenDivZeroCheck(CompilationUnit* cUnit, int regLo, int regHi)
{
  int tReg = AllocTemp(cUnit);
  NewLIR4(cUnit, kThumb2OrrRRRs, tReg, regLo, regHi, 0);
  FreeTemp(cUnit, tReg);
  GenCheck(cUnit, kCondEq, kThrowDivZero);
}

// Test suspend flag, return target of taken suspend branch
LIR* OpTestSuspend(CompilationUnit* cUnit, LIR* target)
{
  NewLIR2(cUnit, kThumbSubRI8, rARM_SUSPEND, 1);
  return OpCondBranch(cUnit, (target == NULL) ? kCondEq : kCondNe, target);
}

// Decrement register and branch on condition
LIR* OpDecAndBranch(CompilationUnit* cUnit, ConditionCode cCode, int reg, LIR* target)
{
  // Combine sub & test using sub setflags encoding here
  NewLIR3(cUnit, kThumb2SubsRRI12, reg, reg, 1);
  return OpCondBranch(cUnit, cCode, target);
}

void GenMemBarrier(CompilationUnit* cUnit, MemBarrierKind barrierKind)
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
  LIR* dmb = NewLIR1(cUnit, kThumb2Dmb, dmbFlavor);
  dmb->defMask = ENCODE_ALL;
#endif
}

bool GenNegLong(CompilationUnit* cUnit, RegLocation rlDest,
                RegLocation rlSrc)
{
  rlSrc = LoadValueWide(cUnit, rlSrc, kCoreReg);
  RegLocation rlResult = EvalLoc(cUnit, rlDest, kCoreReg, true);
  int zReg = AllocTemp(cUnit);
  LoadConstantNoClobber(cUnit, zReg, 0);
  // Check for destructive overlap
  if (rlResult.lowReg == rlSrc.highReg) {
    int tReg = AllocTemp(cUnit);
    OpRegRegReg(cUnit, kOpSub, rlResult.lowReg, zReg, rlSrc.lowReg);
    OpRegRegReg(cUnit, kOpSbc, rlResult.highReg, zReg, tReg);
    FreeTemp(cUnit, tReg);
  } else {
    OpRegRegReg(cUnit, kOpSub, rlResult.lowReg, zReg, rlSrc.lowReg);
    OpRegRegReg(cUnit, kOpSbc, rlResult.highReg, zReg, rlSrc.highReg);
  }
  FreeTemp(cUnit, zReg);
  StoreValueWide(cUnit, rlDest, rlResult);
  return false;
}

bool GenAddLong(CompilationUnit* cUnit, RegLocation rlDest,
                RegLocation rlSrc1, RegLocation rlSrc2)
{
  LOG(FATAL) << "Unexpected use of GenAddLong for Arm";
  return false;
}

bool GenSubLong(CompilationUnit* cUnit, RegLocation rlDest,
                RegLocation rlSrc1, RegLocation rlSrc2)
{
  LOG(FATAL) << "Unexpected use of GenSubLong for Arm";
  return false;
}

bool GenAndLong(CompilationUnit* cUnit, RegLocation rlDest,
                RegLocation rlSrc1, RegLocation rlSrc2)
{
  LOG(FATAL) << "Unexpected use of GenAndLong for Arm";
  return false;
}

bool GenOrLong(CompilationUnit* cUnit, RegLocation rlDest,
               RegLocation rlSrc1, RegLocation rlSrc2)
{
  LOG(FATAL) << "Unexpected use of GenOrLong for Arm";
  return false;
}

bool GenXorLong(CompilationUnit* cUnit, RegLocation rlDest,
               RegLocation rlSrc1, RegLocation rlSrc2)
{
  LOG(FATAL) << "Unexpected use of genXoLong for Arm";
  return false;
}

}  // namespace art
