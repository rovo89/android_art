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

#include "x86_lir.h"
#include "../codegen_util.h"
#include "../ralloc_util.h"

namespace art {

/*
 * Perform register memory operation.
 */
LIR* GenRegMemCheck(CompilationUnit* cUnit, ConditionCode cCode,
                    int reg1, int base, int offset, ThrowKind kind)
{
  LIR* tgt = RawLIR(cUnit, 0, kPseudoThrowTarget, kind,
                    cUnit->currentDalvikOffset, reg1, base, offset);
  OpRegMem(cUnit, kOpCmp, reg1, base, offset);
  LIR* branch = OpCondBranch(cUnit, cCode, tgt);
  // Remember branch target - will process later
  InsertGrowableList(cUnit, &cUnit->throwLaunchpads, reinterpret_cast<uintptr_t>(tgt));
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
void GenCmpLong(CompilationUnit* cUnit, RegLocation rlDest,
                RegLocation rlSrc1, RegLocation rlSrc2)
{
  FlushAllRegs(cUnit);
  LockCallTemps(cUnit);  // Prepare for explicit register usage
  LoadValueDirectWideFixed(cUnit, rlSrc1, r0, r1);
  LoadValueDirectWideFixed(cUnit, rlSrc2, r2, r3);
  // Compute (r1:r0) = (r1:r0) - (r3:r2)
  OpRegReg(cUnit, kOpSub, r0, r2);  // r0 = r0 - r2
  OpRegReg(cUnit, kOpSbc, r1, r3);  // r1 = r1 - r3 - CF
  NewLIR2(cUnit, kX86Set8R, r2, kX86CondL);  // r2 = (r1:r0) < (r3:r2) ? 1 : 0
  NewLIR2(cUnit, kX86Movzx8RR, r2, r2);
  OpReg(cUnit, kOpNeg, r2);         // r2 = -r2
  OpRegReg(cUnit, kOpOr, r0, r1);   // r0 = high | low - sets ZF
  NewLIR2(cUnit, kX86Set8R, r0, kX86CondNz);  // r0 = (r1:r0) != (r3:r2) ? 1 : 0
  NewLIR2(cUnit, kX86Movzx8RR, r0, r0);
  OpRegReg(cUnit, kOpOr, r0, r2);   // r0 = r0 | r2
  RegLocation rlResult = LocCReturn();
  StoreValue(cUnit, rlDest, rlResult);
}

X86ConditionCode X86ConditionEncoding(ConditionCode cond) {
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

LIR* OpCmpBranch(CompilationUnit* cUnit, ConditionCode cond, int src1,
                 int src2, LIR* target)
{
  NewLIR2(cUnit, kX86Cmp32RR, src1, src2);
  X86ConditionCode cc = X86ConditionEncoding(cond);
  LIR* branch = NewLIR2(cUnit, kX86Jcc8, 0 /* lir operand for Jcc offset */ ,
                        cc);
  branch->target = target;
  return branch;
}

LIR* OpCmpImmBranch(CompilationUnit* cUnit, ConditionCode cond, int reg,
                    int checkValue, LIR* target)
{
  if ((checkValue == 0) && (cond == kCondEq || cond == kCondNe)) {
    // TODO: when checkValue == 0 and reg is rCX, use the jcxz/nz opcode
    NewLIR2(cUnit, kX86Test32RR, reg, reg);
  } else {
    NewLIR2(cUnit, IS_SIMM8(checkValue) ? kX86Cmp32RI8 : kX86Cmp32RI, reg, checkValue);
  }
  X86ConditionCode cc = X86ConditionEncoding(cond);
  LIR* branch = NewLIR2(cUnit, kX86Jcc8, 0 /* lir operand for Jcc offset */ , cc);
  branch->target = target;
  return branch;
}

LIR* OpRegCopyNoInsert(CompilationUnit *cUnit, int rDest, int rSrc)
{
  if (X86_FPREG(rDest) || X86_FPREG(rSrc))
    return FpRegCopy(cUnit, rDest, rSrc);
  LIR* res = RawLIR(cUnit, cUnit->currentDalvikOffset, kX86Mov32RR,
                    rDest, rSrc);
  if (rDest == rSrc) {
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
  bool destFP = X86_FPREG(destLo) && X86_FPREG(destHi);
  bool srcFP = X86_FPREG(srcLo) && X86_FPREG(srcHi);
  assert(X86_FPREG(srcLo) == X86_FPREG(srcHi));
  assert(X86_FPREG(destLo) == X86_FPREG(destHi));
  if (destFP) {
    if (srcFP) {
      OpRegCopy(cUnit, S2d(destLo, destHi), S2d(srcLo, srcHi));
    } else {
      // TODO: Prevent this from happening in the code. The result is often
      // unused or could have been loaded more easily from memory.
      NewLIR2(cUnit, kX86MovdxrRR, destLo, srcLo);
      NewLIR2(cUnit, kX86MovdxrRR, destHi, srcHi);
      NewLIR2(cUnit, kX86PsllqRI, destHi, 32);
      NewLIR2(cUnit, kX86OrpsRR, destLo, destHi);
    }
  } else {
    if (srcFP) {
      NewLIR2(cUnit, kX86MovdrxRR, destLo, srcLo);
      NewLIR2(cUnit, kX86PsrlqRI, srcLo, 32);
      NewLIR2(cUnit, kX86MovdrxRR, destHi, srcLo);
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

void GenFusedLongCmpBranch(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir) {
  LIR* labelList = cUnit->blockLabelList;
  LIR* taken = &labelList[bb->taken->id];
  RegLocation rlSrc1 = GetSrcWide(cUnit, mir, 0);
  RegLocation rlSrc2 = GetSrcWide(cUnit, mir, 2);
  FlushAllRegs(cUnit);
  LockCallTemps(cUnit);  // Prepare for explicit register usage
  LoadValueDirectWideFixed(cUnit, rlSrc1, r0, r1);
  LoadValueDirectWideFixed(cUnit, rlSrc2, r2, r3);
  ConditionCode ccode = static_cast<ConditionCode>(mir->dalvikInsn.arg[0]);
  // Swap operands and condition code to prevent use of zero flag.
  if (ccode == kCondLe || ccode == kCondGt) {
    // Compute (r3:r2) = (r3:r2) - (r1:r0)
    OpRegReg(cUnit, kOpSub, r2, r0);  // r2 = r2 - r0
    OpRegReg(cUnit, kOpSbc, r3, r1);  // r3 = r3 - r1 - CF
  } else {
    // Compute (r1:r0) = (r1:r0) - (r3:r2)
    OpRegReg(cUnit, kOpSub, r0, r2);  // r0 = r0 - r2
    OpRegReg(cUnit, kOpSbc, r1, r3);  // r1 = r1 - r3 - CF
  }
  switch (ccode) {
    case kCondEq:
    case kCondNe:
      OpRegReg(cUnit, kOpOr, r0, r1);  // r0 = r0 | r1
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
  OpCondBranch(cUnit, ccode, taken);
}
RegLocation GenDivRemLit(CompilationUnit* cUnit, RegLocation rlDest, int regLo, int lit, bool isDiv)
{
  LOG(FATAL) << "Unexpected use of GenDivRemLit for x86";
  return rlDest;
}

RegLocation GenDivRem(CompilationUnit* cUnit, RegLocation rlDest, int regLo, int regHi, bool isDiv)
{
  LOG(FATAL) << "Unexpected use of GenDivRem for x86";
  return rlDest;
}

bool GenInlinedMinMaxInt(CompilationUnit *cUnit, CallInfo* info, bool isMin)
{
  DCHECK_EQ(cUnit->instructionSet, kX86);
  RegLocation rlSrc1 = info->args[0];
  RegLocation rlSrc2 = info->args[1];
  rlSrc1 = LoadValue(cUnit, rlSrc1, kCoreReg);
  rlSrc2 = LoadValue(cUnit, rlSrc2, kCoreReg);
  RegLocation rlDest = InlineTarget(cUnit, info);
  RegLocation rlResult = EvalLoc(cUnit, rlDest, kCoreReg, true);
  OpRegReg(cUnit, kOpCmp, rlSrc1.lowReg, rlSrc2.lowReg);
  DCHECK_EQ(cUnit->instructionSet, kX86);
  LIR* branch = NewLIR2(cUnit, kX86Jcc8, 0, isMin ? kX86CondG : kX86CondL);
  OpRegReg(cUnit, kOpMov, rlResult.lowReg, rlSrc1.lowReg);
  LIR* branch2 = NewLIR1(cUnit, kX86Jmp8, 0);
  branch->target = NewLIR0(cUnit, kPseudoTargetLabel);
  OpRegReg(cUnit, kOpMov, rlResult.lowReg, rlSrc2.lowReg);
  branch2->target = NewLIR0(cUnit, kPseudoTargetLabel);
  StoreValue(cUnit, rlDest, rlResult);
  return true;
}

void OpLea(CompilationUnit* cUnit, int rBase, int reg1, int reg2, int scale, int offset)
{
  NewLIR5(cUnit, kX86Lea32RA, rBase, reg1, reg2, scale, offset);
}

void OpTlsCmp(CompilationUnit* cUnit, int offset, int val)
{
  NewLIR2(cUnit, kX86Cmp16TI8, offset, val);
}

bool GenInlinedCas32(CompilationUnit* cUnit, CallInfo* info, bool need_write_barrier) {
  DCHECK_NE(cUnit->instructionSet, kThumb2);
  return false;
}

LIR* OpPcRelLoad(CompilationUnit* cUnit, int reg, LIR* target) {
  LOG(FATAL) << "Unexpected use of OpPcRelLoad for x86";
  return NULL;
}

LIR* OpVldm(CompilationUnit* cUnit, int rBase, int count)
{
  LOG(FATAL) << "Unexpected use of OpVldm for x86";
  return NULL;
}

LIR* OpVstm(CompilationUnit* cUnit, int rBase, int count)
{
  LOG(FATAL) << "Unexpected use of OpVstm for x86";
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
  OpTlsCmp(cUnit, Thread::ThreadFlagsOffset().Int32Value(), 0);
  return OpCondBranch(cUnit, (target == NULL) ? kCondNe : kCondEq, target);
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
  LOG(FATAL) << "Unexpected use of smallLiteralDive in x86";
  return false;
}

LIR* OpIT(CompilationUnit* cUnit, ArmConditionCode cond, const char* guide)
{
  LOG(FATAL) << "Unexpected use of OpIT in x86";
  return NULL;
}
bool GenAddLong(CompilationUnit* cUnit, RegLocation rlDest,
                RegLocation rlSrc1, RegLocation rlSrc2)
{
  FlushAllRegs(cUnit);
  LockCallTemps(cUnit);  // Prepare for explicit register usage
  LoadValueDirectWideFixed(cUnit, rlSrc1, r0, r1);
  LoadValueDirectWideFixed(cUnit, rlSrc2, r2, r3);
  // Compute (r1:r0) = (r1:r0) + (r2:r3)
  OpRegReg(cUnit, kOpAdd, r0, r2);  // r0 = r0 + r2
  OpRegReg(cUnit, kOpAdc, r1, r3);  // r1 = r1 + r3 + CF
  RegLocation rlResult = {kLocPhysReg, 1, 0, 0, 0, 0, 0, 0, 1, r0, r1,
                          INVALID_SREG, INVALID_SREG};
  StoreValueWide(cUnit, rlDest, rlResult);
  return false;
}

bool GenSubLong(CompilationUnit* cUnit, RegLocation rlDest,
                RegLocation rlSrc1, RegLocation rlSrc2)
{
  FlushAllRegs(cUnit);
  LockCallTemps(cUnit);  // Prepare for explicit register usage
  LoadValueDirectWideFixed(cUnit, rlSrc1, r0, r1);
  LoadValueDirectWideFixed(cUnit, rlSrc2, r2, r3);
  // Compute (r1:r0) = (r1:r0) + (r2:r3)
  OpRegReg(cUnit, kOpSub, r0, r2);  // r0 = r0 - r2
  OpRegReg(cUnit, kOpSbc, r1, r3);  // r1 = r1 - r3 - CF
  RegLocation rlResult = {kLocPhysReg, 1, 0, 0, 0, 0, 0, 0, 1, r0, r1,
                          INVALID_SREG, INVALID_SREG};
  StoreValueWide(cUnit, rlDest, rlResult);
  return false;
}

bool GenAndLong(CompilationUnit* cUnit, RegLocation rlDest,
                RegLocation rlSrc1, RegLocation rlSrc2)
{
  FlushAllRegs(cUnit);
  LockCallTemps(cUnit);  // Prepare for explicit register usage
  LoadValueDirectWideFixed(cUnit, rlSrc1, r0, r1);
  LoadValueDirectWideFixed(cUnit, rlSrc2, r2, r3);
  // Compute (r1:r0) = (r1:r0) + (r2:r3)
  OpRegReg(cUnit, kOpAnd, r0, r2);  // r0 = r0 - r2
  OpRegReg(cUnit, kOpAnd, r1, r3);  // r1 = r1 - r3 - CF
  RegLocation rlResult = {kLocPhysReg, 1, 0, 0, 0, 0, 0, 0, 1, r0, r1,
                          INVALID_SREG, INVALID_SREG};
  StoreValueWide(cUnit, rlDest, rlResult);
  return false;
}

bool GenOrLong(CompilationUnit* cUnit, RegLocation rlDest,
               RegLocation rlSrc1, RegLocation rlSrc2)
{
  FlushAllRegs(cUnit);
  LockCallTemps(cUnit);  // Prepare for explicit register usage
  LoadValueDirectWideFixed(cUnit, rlSrc1, r0, r1);
  LoadValueDirectWideFixed(cUnit, rlSrc2, r2, r3);
  // Compute (r1:r0) = (r1:r0) + (r2:r3)
  OpRegReg(cUnit, kOpOr, r0, r2);  // r0 = r0 - r2
  OpRegReg(cUnit, kOpOr, r1, r3);  // r1 = r1 - r3 - CF
  RegLocation rlResult = {kLocPhysReg, 1, 0, 0, 0, 0, 0, 0, 1, r0, r1,
                          INVALID_SREG, INVALID_SREG};
  StoreValueWide(cUnit, rlDest, rlResult);
  return false;
}

bool GenXorLong(CompilationUnit* cUnit, RegLocation rlDest,
                RegLocation rlSrc1, RegLocation rlSrc2)
{
  FlushAllRegs(cUnit);
  LockCallTemps(cUnit);  // Prepare for explicit register usage
  LoadValueDirectWideFixed(cUnit, rlSrc1, r0, r1);
  LoadValueDirectWideFixed(cUnit, rlSrc2, r2, r3);
  // Compute (r1:r0) = (r1:r0) + (r2:r3)
  OpRegReg(cUnit, kOpXor, r0, r2);  // r0 = r0 - r2
  OpRegReg(cUnit, kOpXor, r1, r3);  // r1 = r1 - r3 - CF
  RegLocation rlResult = {kLocPhysReg, 1, 0, 0, 0, 0, 0, 0, 1, r0, r1,
                          INVALID_SREG, INVALID_SREG};
  StoreValueWide(cUnit, rlDest, rlResult);
  return false;
}

bool GenNegLong(CompilationUnit* cUnit, RegLocation rlDest,
                RegLocation rlSrc)
{
  FlushAllRegs(cUnit);
  LockCallTemps(cUnit);  // Prepare for explicit register usage
  LoadValueDirectWideFixed(cUnit, rlSrc, r0, r1);
  // Compute (r1:r0) = -(r1:r0)
  OpRegReg(cUnit, kOpNeg, r0, r0);  // r0 = -r0
  OpRegImm(cUnit, kOpAdc, r1, 0);   // r1 = r1 + CF
  OpRegReg(cUnit, kOpNeg, r1, r1);  // r1 = -r1
  RegLocation rlResult = {kLocPhysReg, 1, 0, 0, 0, 0, 0, 0, 1, r0, r1,
                          INVALID_SREG, INVALID_SREG};
  StoreValueWide(cUnit, rlDest, rlResult);
  return false;
}

void OpRegThreadMem(CompilationUnit* cUnit, OpKind op, int rDest, int threadOffset) {
  X86OpCode opcode = kX86Bkpt;
  switch (op) {
  case kOpCmp: opcode = kX86Cmp32RT;  break;
  default:
    LOG(FATAL) << "Bad opcode: " << op;
    break;
  }
  NewLIR2(cUnit, opcode, rDest, threadOffset);
}

}  // namespace art
