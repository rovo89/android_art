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

/* This file contains codegen for the X86 ISA */

namespace art {

/*
 * Perform register memory operation.
 */
LIR* genRegMemCheck(CompilationUnit* cUnit, ConditionCode cCode,
                    int reg1, int base, int offset, ThrowKind kind)
{
  LIR* tgt = rawLIR(cUnit, 0, kPseudoThrowTarget, kind,
                    cUnit->currentDalvikOffset, reg1, base, offset);
  opRegMem(cUnit, kOpCmp, reg1, base, offset);
  LIR* branch = opCondBranch(cUnit, cCode, tgt);
  // Remember branch target - will process later
  oatInsertGrowableList(cUnit, &cUnit->throwLaunchpads, reinterpret_cast<uintptr_t>(tgt));
  return branch;
}

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
  oatFlushAllRegs(cUnit);
  oatLockCallTemps(cUnit);  // Prepare for explicit register usage
  loadValueDirectWideFixed(cUnit, rlSrc1, r0, r1);
  loadValueDirectWideFixed(cUnit, rlSrc2, r2, r3);
  // Compute (r1:r0) = (r1:r0) - (r3:r2)
  opRegReg(cUnit, kOpSub, r0, r2);  // r0 = r0 - r2
  opRegReg(cUnit, kOpSbc, r1, r3);  // r1 = r1 - r3 - CF
  newLIR2(cUnit, kX86Set8R, r2, kX86CondL);  // r2 = (r1:r0) < (r3:r2) ? 1 : 0
  newLIR2(cUnit, kX86Movzx8RR, r2, r2);
  opReg(cUnit, kOpNeg, r2);         // r2 = -r2
  opRegReg(cUnit, kOpOr, r0, r1);   // r0 = high | low - sets ZF
  newLIR2(cUnit, kX86Set8R, r0, kX86CondNz);  // r0 = (r1:r0) != (r3:r2) ? 1 : 0
  newLIR2(cUnit, kX86Movzx8RR, r0, r0);
  opRegReg(cUnit, kOpOr, r0, r2);   // r0 = r0 | r2
  RegLocation rlResult = locCReturn();
  storeValue(cUnit, rlDest, rlResult);
}

X86ConditionCode oatX86ConditionEncoding(ConditionCode cond) {
  switch (cond) {
    case kCondEq: return kX86CondEq;
    case kCondNe: return kX86CondNe;
    case kCondCs: return kX86CondC;
    case kCondCc: return kX86CondNc;
    case kCondMi: return kX86CondS;
    case kCondPl: return kX86CondNs;
    case kCondVs: return kX86CondO;
    case kCondVc: return kX86CondNo;
    case kCondHi: return kX86CondA;
    case kCondLs: return kX86CondBe;
    case kCondGe: return kX86CondGe;
    case kCondLt: return kX86CondL;
    case kCondGt: return kX86CondG;
    case kCondLe: return kX86CondLe;
    case kCondAl:
    case kCondNv: LOG(FATAL) << "Should not reach here";
  }
  return kX86CondO;
}

LIR* opCmpBranch(CompilationUnit* cUnit, ConditionCode cond, int src1,
                 int src2, LIR* target)
{
  newLIR2(cUnit, kX86Cmp32RR, src1, src2);
  X86ConditionCode cc = oatX86ConditionEncoding(cond);
  LIR* branch = newLIR2(cUnit, kX86Jcc8, 0 /* lir operand for Jcc offset */ ,
                        cc);
  branch->target = target;
  return branch;
}

LIR* opCmpImmBranch(CompilationUnit* cUnit, ConditionCode cond, int reg,
                    int checkValue, LIR* target)
{
  if ((checkValue == 0) && (cond == kCondEq || cond == kCondNe)) {
    // TODO: when checkValue == 0 and reg is rCX, use the jcxz/nz opcode
    newLIR2(cUnit, kX86Test32RR, reg, reg);
  } else {
    newLIR2(cUnit, IS_SIMM8(checkValue) ? kX86Cmp32RI8 : kX86Cmp32RI, reg, checkValue);
  }
  X86ConditionCode cc = oatX86ConditionEncoding(cond);
  LIR* branch = newLIR2(cUnit, kX86Jcc8, 0 /* lir operand for Jcc offset */ , cc);
  branch->target = target;
  return branch;
}

LIR* opRegCopyNoInsert(CompilationUnit *cUnit, int rDest, int rSrc)
{
  if (X86_FPREG(rDest) || X86_FPREG(rSrc))
    return fpRegCopy(cUnit, rDest, rSrc);
  LIR* res = rawLIR(cUnit, cUnit->currentDalvikOffset, kX86Mov32RR,
                    rDest, rSrc);
  if (rDest == rSrc) {
    res->flags.isNop = true;
  }
  return res;
}

LIR* opRegCopy(CompilationUnit *cUnit, int rDest, int rSrc)
{
  LIR *res = opRegCopyNoInsert(cUnit, rDest, rSrc);
  oatAppendLIR(cUnit, res);
  return res;
}

void opRegCopyWide(CompilationUnit *cUnit, int destLo, int destHi,
                   int srcLo, int srcHi)
{
  bool destFP = X86_FPREG(destLo) && X86_FPREG(destHi);
  bool srcFP = X86_FPREG(srcLo) && X86_FPREG(srcHi);
  assert(X86_FPREG(srcLo) == X86_FPREG(srcHi));
  assert(X86_FPREG(destLo) == X86_FPREG(destHi));
  if (destFP) {
    if (srcFP) {
      opRegCopy(cUnit, s2d(destLo, destHi), s2d(srcLo, srcHi));
    } else {
      // TODO: Prevent this from happening in the code. The result is often
      // unused or could have been loaded more easily from memory.
      newLIR2(cUnit, kX86MovdxrRR, destLo, srcLo);
      newLIR2(cUnit, kX86MovdxrRR, destHi, srcHi);
      newLIR2(cUnit, kX86PsllqRI, destHi, 32);
      newLIR2(cUnit, kX86OrpsRR, destLo, destHi);
    }
  } else {
    if (srcFP) {
      newLIR2(cUnit, kX86MovdrxRR, destLo, srcLo);
      newLIR2(cUnit, kX86PsrlqRI, srcLo, 32);
      newLIR2(cUnit, kX86MovdrxRR, destHi, srcLo);
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

void genFusedLongCmpBranch(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir) {
  LIR* labelList = cUnit->blockLabelList;
  LIR* taken = &labelList[bb->taken->id];
  RegLocation rlSrc1 = oatGetSrcWide(cUnit, mir, 0);
  RegLocation rlSrc2 = oatGetSrcWide(cUnit, mir, 2);
  oatFlushAllRegs(cUnit);
  oatLockCallTemps(cUnit);  // Prepare for explicit register usage
  loadValueDirectWideFixed(cUnit, rlSrc1, r0, r1);
  loadValueDirectWideFixed(cUnit, rlSrc2, r2, r3);
  ConditionCode ccode = static_cast<ConditionCode>(mir->dalvikInsn.arg[0]);
  // Swap operands and condition code to prevent use of zero flag.
  if (ccode == kCondLe || ccode == kCondGt) {
    // Compute (r3:r2) = (r3:r2) - (r1:r0)
    opRegReg(cUnit, kOpSub, r2, r0);  // r2 = r2 - r0
    opRegReg(cUnit, kOpSbc, r3, r1);  // r3 = r3 - r1 - CF
  } else {
    // Compute (r1:r0) = (r1:r0) - (r3:r2)
    opRegReg(cUnit, kOpSub, r0, r2);  // r0 = r0 - r2
    opRegReg(cUnit, kOpSbc, r1, r3);  // r1 = r1 - r3 - CF
  }
  switch (ccode) {
    case kCondEq:
    case kCondNe:
      opRegReg(cUnit, kOpOr, r0, r1);  // r0 = r0 | r1
      break;
    case kCondLe:
      ccode = kCondGe;
      break;
    case kCondGt:
      ccode = kCondLt;
      break;
    case kCondLt:
    case kCondGe:
      break;
    default:
      LOG(FATAL) << "Unexpected ccode: " << ccode;
  }
  opCondBranch(cUnit, ccode, taken);
}
RegLocation genDivRemLit(CompilationUnit* cUnit, RegLocation rlDest, int regLo, int lit, bool isDiv)
{
  LOG(FATAL) << "Unexpected use of genDivRemLit for x86";
  return rlDest;
}

RegLocation genDivRem(CompilationUnit* cUnit, RegLocation rlDest, int regLo, int regHi, bool isDiv)
{
  LOG(FATAL) << "Unexpected use of genDivRem for x86";
  return rlDest;
}

bool genInlinedMinMaxInt(CompilationUnit *cUnit, CallInfo* info, bool isMin)
{
  DCHECK_EQ(cUnit->instructionSet, kX86);
  RegLocation rlSrc1 = info->args[0];
  RegLocation rlSrc2 = info->args[1];
  rlSrc1 = loadValue(cUnit, rlSrc1, kCoreReg);
  rlSrc2 = loadValue(cUnit, rlSrc2, kCoreReg);
  RegLocation rlDest = inlineTarget(cUnit, info);
  RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
  opRegReg(cUnit, kOpCmp, rlSrc1.lowReg, rlSrc2.lowReg);
  DCHECK_EQ(cUnit->instructionSet, kX86);
  LIR* branch = newLIR2(cUnit, kX86Jcc8, 0, isMin ? kX86CondG : kX86CondL);
  opRegReg(cUnit, kOpMov, rlResult.lowReg, rlSrc1.lowReg);
  LIR* branch2 = newLIR1(cUnit, kX86Jmp8, 0);
  branch->target = newLIR0(cUnit, kPseudoTargetLabel);
  opRegReg(cUnit, kOpMov, rlResult.lowReg, rlSrc2.lowReg);
  branch2->target = newLIR0(cUnit, kPseudoTargetLabel);
  storeValue(cUnit, rlDest, rlResult);
  return true;
}

void opLea(CompilationUnit* cUnit, int rBase, int reg1, int reg2, int scale, int offset)
{
  newLIR5(cUnit, kX86Lea32RA, rBase, reg1, reg2, scale, offset);
}

void opTlsCmp(CompilationUnit* cUnit, int offset, int val)
{
  newLIR2(cUnit, kX86Cmp16TI8, offset, val);
}

bool genInlinedCas32(CompilationUnit* cUnit, CallInfo* info, bool need_write_barrier) {
  DCHECK_NE(cUnit->instructionSet, kThumb2);
  return false;
}

LIR* opPcRelLoad(CompilationUnit* cUnit, int reg, LIR* target) {
  LOG(FATAL) << "Unexpected use of opPcRelLoad for x86";
  return NULL;
}

LIR* opVldm(CompilationUnit* cUnit, int rBase, int count)
{
  LOG(FATAL) << "Unexpected use of opVldm for x86";
  return NULL;
}

LIR* opVstm(CompilationUnit* cUnit, int rBase, int count)
{
  LOG(FATAL) << "Unexpected use of opVstm for x86";
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
  opTlsCmp(cUnit, Thread::ThreadFlagsOffset().Int32Value(), 0);
  return opCondBranch(cUnit, (target == NULL) ? kCondNe : kCondEq, target);
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
  LOG(FATAL) << "Unexpected use of smallLiteralDive in x86";
  return false;
}

LIR* opIT(CompilationUnit* cUnit, ArmConditionCode cond, const char* guide)
{
  LOG(FATAL) << "Unexpected use of opIT in x86";
  return NULL;
}
bool genAddLong(CompilationUnit* cUnit, RegLocation rlDest,
                RegLocation rlSrc1, RegLocation rlSrc2)
{
  oatFlushAllRegs(cUnit);
  oatLockCallTemps(cUnit);  // Prepare for explicit register usage
  loadValueDirectWideFixed(cUnit, rlSrc1, r0, r1);
  loadValueDirectWideFixed(cUnit, rlSrc2, r2, r3);
  // Compute (r1:r0) = (r1:r0) + (r2:r3)
  opRegReg(cUnit, kOpAdd, r0, r2);  // r0 = r0 + r2
  opRegReg(cUnit, kOpAdc, r1, r3);  // r1 = r1 + r3 + CF
  RegLocation rlResult = {kLocPhysReg, 1, 0, 0, 0, 0, 0, 0, 1, r0, r1,
                          INVALID_SREG, INVALID_SREG};
  storeValueWide(cUnit, rlDest, rlResult);
  return false;
}

bool genSubLong(CompilationUnit* cUnit, RegLocation rlDest,
                RegLocation rlSrc1, RegLocation rlSrc2)
{
  oatFlushAllRegs(cUnit);
  oatLockCallTemps(cUnit);  // Prepare for explicit register usage
  loadValueDirectWideFixed(cUnit, rlSrc1, r0, r1);
  loadValueDirectWideFixed(cUnit, rlSrc2, r2, r3);
  // Compute (r1:r0) = (r1:r0) + (r2:r3)
  opRegReg(cUnit, kOpSub, r0, r2);  // r0 = r0 - r2
  opRegReg(cUnit, kOpSbc, r1, r3);  // r1 = r1 - r3 - CF
  RegLocation rlResult = {kLocPhysReg, 1, 0, 0, 0, 0, 0, 0, 1, r0, r1,
                          INVALID_SREG, INVALID_SREG};
  storeValueWide(cUnit, rlDest, rlResult);
  return false;
}

bool genAndLong(CompilationUnit* cUnit, RegLocation rlDest,
                RegLocation rlSrc1, RegLocation rlSrc2)
{
  oatFlushAllRegs(cUnit);
  oatLockCallTemps(cUnit);  // Prepare for explicit register usage
  loadValueDirectWideFixed(cUnit, rlSrc1, r0, r1);
  loadValueDirectWideFixed(cUnit, rlSrc2, r2, r3);
  // Compute (r1:r0) = (r1:r0) + (r2:r3)
  opRegReg(cUnit, kOpAnd, r0, r2);  // r0 = r0 - r2
  opRegReg(cUnit, kOpAnd, r1, r3);  // r1 = r1 - r3 - CF
  RegLocation rlResult = {kLocPhysReg, 1, 0, 0, 0, 0, 0, 0, 1, r0, r1,
                          INVALID_SREG, INVALID_SREG};
  storeValueWide(cUnit, rlDest, rlResult);
  return false;
}

bool genOrLong(CompilationUnit* cUnit, RegLocation rlDest,
               RegLocation rlSrc1, RegLocation rlSrc2)
{
  oatFlushAllRegs(cUnit);
  oatLockCallTemps(cUnit);  // Prepare for explicit register usage
  loadValueDirectWideFixed(cUnit, rlSrc1, r0, r1);
  loadValueDirectWideFixed(cUnit, rlSrc2, r2, r3);
  // Compute (r1:r0) = (r1:r0) + (r2:r3)
  opRegReg(cUnit, kOpOr, r0, r2);  // r0 = r0 - r2
  opRegReg(cUnit, kOpOr, r1, r3);  // r1 = r1 - r3 - CF
  RegLocation rlResult = {kLocPhysReg, 1, 0, 0, 0, 0, 0, 0, 1, r0, r1,
                          INVALID_SREG, INVALID_SREG};
  storeValueWide(cUnit, rlDest, rlResult);
  return false;
}

bool genXorLong(CompilationUnit* cUnit, RegLocation rlDest,
                RegLocation rlSrc1, RegLocation rlSrc2)
{
  oatFlushAllRegs(cUnit);
  oatLockCallTemps(cUnit);  // Prepare for explicit register usage
  loadValueDirectWideFixed(cUnit, rlSrc1, r0, r1);
  loadValueDirectWideFixed(cUnit, rlSrc2, r2, r3);
  // Compute (r1:r0) = (r1:r0) + (r2:r3)
  opRegReg(cUnit, kOpXor, r0, r2);  // r0 = r0 - r2
  opRegReg(cUnit, kOpXor, r1, r3);  // r1 = r1 - r3 - CF
  RegLocation rlResult = {kLocPhysReg, 1, 0, 0, 0, 0, 0, 0, 1, r0, r1,
                          INVALID_SREG, INVALID_SREG};
  storeValueWide(cUnit, rlDest, rlResult);
  return false;
}

bool genNegLong(CompilationUnit* cUnit, RegLocation rlDest,
                RegLocation rlSrc)
{
  oatFlushAllRegs(cUnit);
  oatLockCallTemps(cUnit);  // Prepare for explicit register usage
  loadValueDirectWideFixed(cUnit, rlSrc, r0, r1);
  // Compute (r1:r0) = -(r1:r0)
  opRegReg(cUnit, kOpNeg, r0, r0);  // r0 = -r0
  opRegImm(cUnit, kOpAdc, r1, 0);   // r1 = r1 + CF
  opRegReg(cUnit, kOpNeg, r1, r1);  // r1 = -r1
  RegLocation rlResult = {kLocPhysReg, 1, 0, 0, 0, 0, 0, 0, 1, r0, r1,
                          INVALID_SREG, INVALID_SREG};
  storeValueWide(cUnit, rlDest, rlResult);
  return false;
}

void opRegThreadMem(CompilationUnit* cUnit, OpKind op, int rDest, int threadOffset) {
  X86OpCode opcode = kX86Bkpt;
  switch (op) {
  case kOpCmp: opcode = kX86Cmp32RT;  break;
  default:
    LOG(FATAL) << "Bad opcode: " << op;
    break;
  }
  newLIR2(cUnit, opcode, rDest, threadOffset);
}

}  // namespace art
