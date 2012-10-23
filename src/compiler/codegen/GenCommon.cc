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

/*
 * This source files contains "gen" codegen routines that should
 * be applicable to most targets.  Only mid-level support utilities
 * and "op" calls may be used here.
 */
void genInvoke(CompilationUnit* cUnit, CallInfo* info);
#if defined(TARGET_ARM)
LIR* opIT(CompilationUnit* cUnit, ArmConditionCode cond, const char* guide);
bool smallLiteralDivide(CompilationUnit* cUnit, Instruction::Code dalvikOpcode,
                        RegLocation rlSrc, RegLocation rlDest, int lit);
#endif

void markSafepointPC(CompilationUnit* cUnit, LIR* inst)
{
  inst->defMask = ENCODE_ALL;
  LIR* safepointPC = newLIR0(cUnit, kPseudoSafepointPC);
  DCHECK_EQ(safepointPC->defMask, ENCODE_ALL);
}

void callRuntimeHelperImm(CompilationUnit* cUnit, int helperOffset, int arg0, bool safepointPC) {
#if !defined(TARGET_X86)
  int rTgt = loadHelper(cUnit, helperOffset);
#endif
  loadConstant(cUnit, rARG0, arg0);
  oatClobberCalleeSave(cUnit);
#if !defined(TARGET_X86)
  LIR* callInst = opReg(cUnit, kOpBlx, rTgt);
  oatFreeTemp(cUnit, rTgt);
#else
  LIR* callInst = opThreadMem(cUnit, kOpBlx, helperOffset);
#endif
  if (safepointPC) {
    markSafepointPC(cUnit, callInst);
  }
}

void callRuntimeHelperReg(CompilationUnit* cUnit, int helperOffset, int arg0, bool safepointPC) {
#if !defined(TARGET_X86)
  int rTgt = loadHelper(cUnit, helperOffset);
#endif
  opRegCopy(cUnit, rARG0, arg0);
  oatClobberCalleeSave(cUnit);
#if !defined(TARGET_X86)
  LIR* callInst = opReg(cUnit, kOpBlx, rTgt);
  oatFreeTemp(cUnit, rTgt);
#else
  LIR* callInst = opThreadMem(cUnit, kOpBlx, helperOffset);
#endif
  if (safepointPC) {
    markSafepointPC(cUnit, callInst);
  }
}

void callRuntimeHelperRegLocation(CompilationUnit* cUnit, int helperOffset, RegLocation arg0,
                                  bool safepointPC) {
#if !defined(TARGET_X86)
  int rTgt = loadHelper(cUnit, helperOffset);
#endif
  if (arg0.wide == 0) {
    loadValueDirectFixed(cUnit, arg0, rARG0);
  } else {
    loadValueDirectWideFixed(cUnit, arg0, rARG0, rARG1);
  }
  oatClobberCalleeSave(cUnit);
#if !defined(TARGET_X86)
  LIR* callInst = opReg(cUnit, kOpBlx, rTgt);
  oatFreeTemp(cUnit, rTgt);
#else
  LIR* callInst = opThreadMem(cUnit, kOpBlx, helperOffset);
#endif
  if (safepointPC) {
    markSafepointPC(cUnit, callInst);
  }
}

void callRuntimeHelperImmImm(CompilationUnit* cUnit, int helperOffset, int arg0, int arg1,
                             bool safepointPC) {
#if !defined(TARGET_X86)
  int rTgt = loadHelper(cUnit, helperOffset);
#endif
  loadConstant(cUnit, rARG0, arg0);
  loadConstant(cUnit, rARG1, arg1);
  oatClobberCalleeSave(cUnit);
#if !defined(TARGET_X86)
  LIR* callInst = opReg(cUnit, kOpBlx, rTgt);
  oatFreeTemp(cUnit, rTgt);
#else
  LIR* callInst = opThreadMem(cUnit, kOpBlx, helperOffset);
#endif
  if (safepointPC) {
    markSafepointPC(cUnit, callInst);
  }
}

void callRuntimeHelperImmRegLocation(CompilationUnit* cUnit, int helperOffset, int arg0,
                                     RegLocation arg1, bool safepointPC) {
#if !defined(TARGET_X86)
  int rTgt = loadHelper(cUnit, helperOffset);
#endif
  if (arg1.wide == 0) {
    loadValueDirectFixed(cUnit, arg1, rARG1);
  } else {
    loadValueDirectWideFixed(cUnit, arg1, rARG1, rARG2);
  }
  loadConstant(cUnit, rARG0, arg0);
  oatClobberCalleeSave(cUnit);
#if !defined(TARGET_X86)
  LIR* callInst = opReg(cUnit, kOpBlx, rTgt);
  oatFreeTemp(cUnit, rTgt);
#else
  LIR* callInst = opThreadMem(cUnit, kOpBlx, helperOffset);
#endif
  if (safepointPC) {
    markSafepointPC(cUnit, callInst);
  }
}

void callRuntimeHelperRegLocationImm(CompilationUnit* cUnit, int helperOffset, RegLocation arg0,
                                     int arg1, bool safepointPC) {
#if !defined(TARGET_X86)
  int rTgt = loadHelper(cUnit, helperOffset);
#endif
  loadValueDirectFixed(cUnit, arg0, rARG0);
  loadConstant(cUnit, rARG1, arg1);
  oatClobberCalleeSave(cUnit);
#if !defined(TARGET_X86)
  LIR* callInst = opReg(cUnit, kOpBlx, rTgt);
  oatFreeTemp(cUnit, rTgt);
#else
  LIR* callInst = opThreadMem(cUnit, kOpBlx, helperOffset);
#endif
  if (safepointPC) {
    markSafepointPC(cUnit, callInst);
  }
}

void callRuntimeHelperImmReg(CompilationUnit* cUnit, int helperOffset, int arg0, int arg1,
                             bool safepointPC) {
#if !defined(TARGET_X86)
  int rTgt = loadHelper(cUnit, helperOffset);
#endif
  opRegCopy(cUnit, rARG1, arg1);
  loadConstant(cUnit, rARG0, arg0);
  oatClobberCalleeSave(cUnit);
#if !defined(TARGET_X86)
  LIR* callInst = opReg(cUnit, kOpBlx, rTgt);
  oatFreeTemp(cUnit, rTgt);
#else
  LIR* callInst = opThreadMem(cUnit, kOpBlx, helperOffset);
#endif
  if (safepointPC) {
    markSafepointPC(cUnit, callInst);
  }
}

void callRuntimeHelperRegImm(CompilationUnit* cUnit, int helperOffset, int arg0, int arg1,
                             bool safepointPC) {
#if !defined(TARGET_X86)
  int rTgt = loadHelper(cUnit, helperOffset);
#endif
  opRegCopy(cUnit, rARG0, arg0);
  loadConstant(cUnit, rARG1, arg1);
  oatClobberCalleeSave(cUnit);
#if !defined(TARGET_X86)
  LIR* callInst = opReg(cUnit, kOpBlx, rTgt);
  oatFreeTemp(cUnit, rTgt);
#else
  LIR* callInst = opThreadMem(cUnit, kOpBlx, helperOffset);
#endif
  if (safepointPC) {
    markSafepointPC(cUnit, callInst);
  }
}

void callRuntimeHelperImmMethod(CompilationUnit* cUnit, int helperOffset, int arg0, bool safepointPC) {
#if !defined(TARGET_X86)
  int rTgt = loadHelper(cUnit, helperOffset);
#endif
  loadCurrMethodDirect(cUnit, rARG1);
  loadConstant(cUnit, rARG0, arg0);
  oatClobberCalleeSave(cUnit);
#if !defined(TARGET_X86)
  LIR* callInst = opReg(cUnit, kOpBlx, rTgt);
  oatFreeTemp(cUnit, rTgt);
#else
  LIR* callInst = opThreadMem(cUnit, kOpBlx, helperOffset);
#endif
  if (safepointPC) {
    markSafepointPC(cUnit, callInst);
  }
}

void callRuntimeHelperRegLocationRegLocation(CompilationUnit* cUnit, int helperOffset,
                                             RegLocation arg0, RegLocation arg1, bool safepointPC) {
#if !defined(TARGET_X86)
  int rTgt = loadHelper(cUnit, helperOffset);
#endif
  if (arg0.wide == 0) {
    loadValueDirectFixed(cUnit, arg0, arg0.fp ? rFARG0 : rARG0);
    if (arg1.wide == 0) {
#if defined(TARGET_MIPS)
      loadValueDirectFixed(cUnit, arg1, arg1.fp ? rFARG2 : rARG1);
#else
      loadValueDirectFixed(cUnit, arg1, rARG1);
#endif
    } else {
#if defined(TARGET_MIPS)
      loadValueDirectWideFixed(cUnit, arg1, arg1.fp ? rFARG2 : rARG1, arg1.fp ? rFARG3 : rARG2);
#else
      loadValueDirectWideFixed(cUnit, arg1, rARG1, rARG2);
#endif
    }
  } else {
    loadValueDirectWideFixed(cUnit, arg0, arg0.fp ? rFARG0 : rARG0, arg0.fp ? rFARG1 : rARG1);
    if (arg1.wide == 0) {
      loadValueDirectFixed(cUnit, arg1, arg1.fp ? rFARG2 : rARG2);
    } else {
      loadValueDirectWideFixed(cUnit, arg1, arg1.fp ? rFARG2 : rARG2, arg1.fp ? rFARG3 : rARG3);
    }
  }
  oatClobberCalleeSave(cUnit);
#if !defined(TARGET_X86)
  LIR* callInst = opReg(cUnit, kOpBlx, rTgt);
  oatFreeTemp(cUnit, rTgt);
#else
  LIR* callInst = opThreadMem(cUnit, kOpBlx, helperOffset);
#endif
  if (safepointPC) {
    markSafepointPC(cUnit, callInst);
  }
}

void callRuntimeHelperRegReg(CompilationUnit* cUnit, int helperOffset, int arg0, int arg1,
                             bool safepointPC) {
#if !defined(TARGET_X86)
  int rTgt = loadHelper(cUnit, helperOffset);
#endif
  DCHECK_NE((int)rARG0, arg1);  // check copy into arg0 won't clobber arg1
  opRegCopy(cUnit, rARG0, arg0);
  opRegCopy(cUnit, rARG1, arg1);
  oatClobberCalleeSave(cUnit);
#if !defined(TARGET_X86)
  LIR* callInst = opReg(cUnit, kOpBlx, rTgt);
  oatFreeTemp(cUnit, rTgt);
#else
  LIR* callInst = opThreadMem(cUnit, kOpBlx, helperOffset);
#endif
  if (safepointPC) {
    markSafepointPC(cUnit, callInst);
  }
}

void callRuntimeHelperRegRegImm(CompilationUnit* cUnit, int helperOffset, int arg0, int arg1,
                                int arg2, bool safepointPC) {
#if !defined(TARGET_X86)
  int rTgt = loadHelper(cUnit, helperOffset);
#endif
  DCHECK_NE((int)rARG0, arg1);  // check copy into arg0 won't clobber arg1
  opRegCopy(cUnit, rARG0, arg0);
  opRegCopy(cUnit, rARG1, arg1);
  loadConstant(cUnit, rARG2, arg2);
  oatClobberCalleeSave(cUnit);
#if !defined(TARGET_X86)
  LIR* callInst = opReg(cUnit, kOpBlx, rTgt);
  oatFreeTemp(cUnit, rTgt);
#else
  LIR* callInst = opThreadMem(cUnit, kOpBlx, helperOffset);
#endif
  if (safepointPC) {
    markSafepointPC(cUnit, callInst);
  }
}

void callRuntimeHelperImmMethodRegLocation(CompilationUnit* cUnit, int helperOffset, int arg0,
                                           RegLocation arg2, bool safepointPC) {
#if !defined(TARGET_X86)
  int rTgt = loadHelper(cUnit, helperOffset);
#endif
  loadValueDirectFixed(cUnit, arg2, rARG2);
  loadCurrMethodDirect(cUnit, rARG1);
  loadConstant(cUnit, rARG0, arg0);
  oatClobberCalleeSave(cUnit);
#if !defined(TARGET_X86)
  LIR* callInst = opReg(cUnit, kOpBlx, rTgt);
  oatFreeTemp(cUnit, rTgt);
#else
  LIR* callInst = opThreadMem(cUnit, kOpBlx, helperOffset);
#endif
  if (safepointPC) {
    markSafepointPC(cUnit, callInst);
  }
}

void callRuntimeHelperImmMethodImm(CompilationUnit* cUnit, int helperOffset, int arg0, int arg2,
                                   bool safepointPC) {
#if !defined(TARGET_X86)
  int rTgt = loadHelper(cUnit, helperOffset);
#endif
  loadCurrMethodDirect(cUnit, rARG1);
  loadConstant(cUnit, rARG2, arg2);
  loadConstant(cUnit, rARG0, arg0);
  oatClobberCalleeSave(cUnit);
#if !defined(TARGET_X86)
  LIR* callInst = opReg(cUnit, kOpBlx, rTgt);
  oatFreeTemp(cUnit, rTgt);
#else
  LIR* callInst = opThreadMem(cUnit, kOpBlx, helperOffset);
#endif
  if (safepointPC) {
    markSafepointPC(cUnit, callInst);
  }
}

void callRuntimeHelperImmRegLocationRegLocation(CompilationUnit* cUnit, int helperOffset,
                                                int arg0, RegLocation arg1, RegLocation arg2,
                                                bool safepointPC) {
#if !defined(TARGET_X86)
  int rTgt = loadHelper(cUnit, helperOffset);
#endif
  loadValueDirectFixed(cUnit, arg1, rARG1);
  if (arg2.wide == 0) {
    loadValueDirectFixed(cUnit, arg2, rARG2);
  } else {
    loadValueDirectWideFixed(cUnit, arg2, rARG2, rARG3);
  }
  loadConstant(cUnit, rARG0, arg0);
  oatClobberCalleeSave(cUnit);
#if !defined(TARGET_X86)
  LIR* callInst = opReg(cUnit, kOpBlx, rTgt);
  oatFreeTemp(cUnit, rTgt);
#else
  LIR* callInst = opThreadMem(cUnit, kOpBlx, helperOffset);
#endif
  if (safepointPC) {
    markSafepointPC(cUnit, callInst);
  }
}

/*
 * Generate an kPseudoBarrier marker to indicate the boundary of special
 * blocks.
 */
void genBarrier(CompilationUnit* cUnit)
{
  LIR* barrier = newLIR0(cUnit, kPseudoBarrier);
  /* Mark all resources as being clobbered */
  barrier->defMask = -1;
}


/* Generate unconditional branch instructions */
LIR* opUnconditionalBranch(CompilationUnit* cUnit, LIR* target)
{
  LIR* branch = opBranchUnconditional(cUnit, kOpUncondBr);
  branch->target = (LIR*) target;
  return branch;
}

// FIXME: need to do some work to split out targets with
// condition codes and those without
#if defined(TARGET_ARM) || defined(TARGET_X86)
LIR* genCheck(CompilationUnit* cUnit, ConditionCode cCode,
              ThrowKind kind)
{
  LIR* tgt = rawLIR(cUnit, 0, kPseudoThrowTarget, kind,
                    cUnit->currentDalvikOffset);
  LIR* branch = opCondBranch(cUnit, cCode, tgt);
  // Remember branch target - will process later
  oatInsertGrowableList(cUnit, &cUnit->throwLaunchpads, (intptr_t)tgt);
  return branch;
}
#endif

LIR* genImmedCheck(CompilationUnit* cUnit, ConditionCode cCode,
                   int reg, int immVal, ThrowKind kind)
{
  LIR* tgt = rawLIR(cUnit, 0, kPseudoThrowTarget, kind,
                    cUnit->currentDalvikOffset);
  LIR* branch;
  if (cCode == kCondAl) {
    branch = opUnconditionalBranch(cUnit, tgt);
  } else {
    branch = opCmpImmBranch(cUnit, cCode, reg, immVal, tgt);
  }
  // Remember branch target - will process later
  oatInsertGrowableList(cUnit, &cUnit->throwLaunchpads, (intptr_t)tgt);
  return branch;
}

/* Perform null-check on a register.  */
LIR* genNullCheck(CompilationUnit* cUnit, int sReg, int mReg, int optFlags)
{
  if (!(cUnit->disableOpt & (1 << kNullCheckElimination)) &&
    optFlags & MIR_IGNORE_NULL_CHECK) {
    return NULL;
  }
  return genImmedCheck(cUnit, kCondEq, mReg, 0, kThrowNullPointer);
}

/* Perform check on two registers */
LIR* genRegRegCheck(CompilationUnit* cUnit, ConditionCode cCode,
                    int reg1, int reg2, ThrowKind kind)
{
  LIR* tgt = rawLIR(cUnit, 0, kPseudoThrowTarget, kind,
                    cUnit->currentDalvikOffset, reg1, reg2);
#if defined(TARGET_MIPS)
  LIR* branch = opCmpBranch(cUnit, cCode, reg1, reg2, tgt);
#else
  opRegReg(cUnit, kOpCmp, reg1, reg2);
  LIR* branch = opCondBranch(cUnit, cCode, tgt);
#endif
  // Remember branch target - will process later
  oatInsertGrowableList(cUnit, &cUnit->throwLaunchpads, (intptr_t)tgt);
  return branch;
}

void genCompareAndBranch(CompilationUnit* cUnit, Instruction::Code opcode,
                         RegLocation rlSrc1, RegLocation rlSrc2, LIR* taken,
                         LIR* fallThrough)
{
  ConditionCode cond;
  rlSrc1 = loadValue(cUnit, rlSrc1, kCoreReg);
  rlSrc2 = loadValue(cUnit, rlSrc2, kCoreReg);
  switch (opcode) {
    case Instruction::IF_EQ:
      cond = kCondEq;
      break;
    case Instruction::IF_NE:
      cond = kCondNe;
      break;
    case Instruction::IF_LT:
      cond = kCondLt;
      break;
    case Instruction::IF_GE:
      cond = kCondGe;
      break;
    case Instruction::IF_GT:
      cond = kCondGt;
      break;
    case Instruction::IF_LE:
      cond = kCondLe;
      break;
    default:
      cond = (ConditionCode)0;
      LOG(FATAL) << "Unexpected opcode " << (int)opcode;
  }
#if defined(TARGET_MIPS)
  opCmpBranch(cUnit, cond, rlSrc1.lowReg, rlSrc2.lowReg, taken);
#else
  opRegReg(cUnit, kOpCmp, rlSrc1.lowReg, rlSrc2.lowReg);
  opCondBranch(cUnit, cond, taken);
#endif
  opUnconditionalBranch(cUnit, fallThrough);
}

void genCompareZeroAndBranch(CompilationUnit* cUnit, Instruction::Code opcode,
                             RegLocation rlSrc, LIR* taken, LIR* fallThrough)
{
  ConditionCode cond;
  rlSrc = loadValue(cUnit, rlSrc, kCoreReg);
  switch (opcode) {
    case Instruction::IF_EQZ:
      cond = kCondEq;
      break;
    case Instruction::IF_NEZ:
      cond = kCondNe;
      break;
    case Instruction::IF_LTZ:
      cond = kCondLt;
      break;
    case Instruction::IF_GEZ:
      cond = kCondGe;
      break;
    case Instruction::IF_GTZ:
      cond = kCondGt;
      break;
    case Instruction::IF_LEZ:
      cond = kCondLe;
      break;
    default:
      cond = (ConditionCode)0;
      LOG(FATAL) << "Unexpected opcode " << (int)opcode;
  }
#if defined(TARGET_MIPS) || defined(TARGET_X86)
  opCmpImmBranch(cUnit, cond, rlSrc.lowReg, 0, taken);
#else
  opRegImm(cUnit, kOpCmp, rlSrc.lowReg, 0);
  opCondBranch(cUnit, cond, taken);
#endif
  opUnconditionalBranch(cUnit, fallThrough);
}

void genIntToLong(CompilationUnit* cUnit, RegLocation rlDest,
                  RegLocation rlSrc)
{
  RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
  if (rlSrc.location == kLocPhysReg) {
    opRegCopy(cUnit, rlResult.lowReg, rlSrc.lowReg);
  } else {
    loadValueDirect(cUnit, rlSrc, rlResult.lowReg);
  }
  opRegRegImm(cUnit, kOpAsr, rlResult.highReg, rlResult.lowReg, 31);
  storeValueWide(cUnit, rlDest, rlResult);
}

void genIntNarrowing(CompilationUnit* cUnit, Instruction::Code opcode,
                     RegLocation rlDest, RegLocation rlSrc)
{
   rlSrc = loadValue(cUnit, rlSrc, kCoreReg);
   RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
   OpKind op = kOpInvalid;
   switch (opcode) {
     case Instruction::INT_TO_BYTE:
       op = kOp2Byte;
       break;
     case Instruction::INT_TO_SHORT:
        op = kOp2Short;
        break;
     case Instruction::INT_TO_CHAR:
        op = kOp2Char;
        break;
     default:
       LOG(ERROR) << "Bad int conversion type";
   }
   opRegReg(cUnit, op, rlResult.lowReg, rlSrc.lowReg);
   storeValue(cUnit, rlDest, rlResult);
}

/*
 * Let helper function take care of everything.  Will call
 * Array::AllocFromCode(type_idx, method, count);
 * Note: AllocFromCode will handle checks for errNegativeArraySize.
 */
void genNewArray(CompilationUnit* cUnit, uint32_t type_idx, RegLocation rlDest,
                 RegLocation rlSrc)
{
  oatFlushAllRegs(cUnit);  /* Everything to home location */
  int funcOffset;
  if (cUnit->compiler->CanAccessTypeWithoutChecks(cUnit->method_idx,
                                                  *cUnit->dex_file,
                                                  type_idx)) {
    funcOffset = ENTRYPOINT_OFFSET(pAllocArrayFromCode);
  } else {
    funcOffset= ENTRYPOINT_OFFSET(pAllocArrayFromCodeWithAccessCheck);
  }
  callRuntimeHelperImmMethodRegLocation(cUnit, funcOffset, type_idx, rlSrc, true);
  RegLocation rlResult = oatGetReturn(cUnit, false);
  storeValue(cUnit, rlDest, rlResult);
}

/*
 * Similar to genNewArray, but with post-allocation initialization.
 * Verifier guarantees we're dealing with an array class.  Current
 * code throws runtime exception "bad Filled array req" for 'D' and 'J'.
 * Current code also throws internal unimp if not 'L', '[' or 'I'.
 */
void genFilledNewArray(CompilationUnit* cUnit, CallInfo* info)
{
  int elems = info->numArgWords;
  int typeIdx = info->index;
  oatFlushAllRegs(cUnit);  /* Everything to home location */
  int funcOffset;
  if (cUnit->compiler->CanAccessTypeWithoutChecks(cUnit->method_idx,
                                                  *cUnit->dex_file,
                                                  typeIdx)) {
    funcOffset = ENTRYPOINT_OFFSET(pCheckAndAllocArrayFromCode);
  } else {
    funcOffset = ENTRYPOINT_OFFSET(pCheckAndAllocArrayFromCodeWithAccessCheck);
  }
  callRuntimeHelperImmMethodImm(cUnit, funcOffset, typeIdx, elems, true);
  oatFreeTemp(cUnit, rARG2);
  oatFreeTemp(cUnit, rARG1);
  /*
   * NOTE: the implicit target for Instruction::FILLED_NEW_ARRAY is the
   * return region.  Because AllocFromCode placed the new array
   * in rRET0, we'll just lock it into place.  When debugger support is
   * added, it may be necessary to additionally copy all return
   * values to a home location in thread-local storage
   */
  oatLockTemp(cUnit, rRET0);

  // TODO: use the correct component size, currently all supported types
  // share array alignment with ints (see comment at head of function)
  size_t component_size = sizeof(int32_t);

  // Having a range of 0 is legal
  if (info->isRange && (elems > 0)) {
    /*
     * Bit of ugliness here.  We're going generate a mem copy loop
     * on the register range, but it is possible that some regs
     * in the range have been promoted.  This is unlikely, but
     * before generating the copy, we'll just force a flush
     * of any regs in the source range that have been promoted to
     * home location.
     */
    for (int i = 0; i < elems; i++) {
      RegLocation loc = oatUpdateLoc(cUnit, info->args[i]);
      if (loc.location == kLocPhysReg) {
        storeBaseDisp(cUnit, rSP, oatSRegOffset(cUnit, loc.sRegLow),
                      loc.lowReg, kWord);
      }
    }
    /*
     * TUNING note: generated code here could be much improved, but
     * this is an uncommon operation and isn't especially performance
     * critical.
     */
    int rSrc = oatAllocTemp(cUnit);
    int rDst = oatAllocTemp(cUnit);
    int rIdx = oatAllocTemp(cUnit);
#if defined(TARGET_ARM)
    int rVal = rLR;  // Using a lot of temps, rLR is known free here
#elif defined(TARGET_X86)
    oatFreeTemp(cUnit, rRET0);
    int rVal = oatAllocTemp(cUnit);
#else
    int rVal = oatAllocTemp(cUnit);
#endif
    // Set up source pointer
    RegLocation rlFirst = info->args[0];
    opRegRegImm(cUnit, kOpAdd, rSrc, rSP,
                oatSRegOffset(cUnit, rlFirst.sRegLow));
    // Set up the target pointer
    opRegRegImm(cUnit, kOpAdd, rDst, rRET0,
                Array::DataOffset(component_size).Int32Value());
    // Set up the loop counter (known to be > 0)
    loadConstant(cUnit, rIdx, elems - 1);
    // Generate the copy loop.  Going backwards for convenience
    LIR* target = newLIR0(cUnit, kPseudoTargetLabel);
    // Copy next element
    loadBaseIndexed(cUnit, rSrc, rIdx, rVal, 2, kWord);
    storeBaseIndexed(cUnit, rDst, rIdx, rVal, 2, kWord);
#if defined(TARGET_ARM)
    // Combine sub & test using sub setflags encoding here
    newLIR3(cUnit, kThumb2SubsRRI12, rIdx, rIdx, 1);
    opCondBranch(cUnit, kCondGe, target);
#else
    oatFreeTemp(cUnit, rVal);
    opRegImm(cUnit, kOpSub, rIdx, 1);
    opCmpImmBranch(cUnit, kCondGe, rIdx, 0, target);
#endif
#if defined(TARGET_X86)
    // Restore the target pointer
    opRegRegImm(cUnit, kOpAdd, rRET0, rDst,
                -Array::DataOffset(component_size).Int32Value());
#endif
  } else if (!info->isRange) {
    // TUNING: interleave
    for (int i = 0; i < elems; i++) {
      RegLocation rlArg = loadValue(cUnit, info->args[i], kCoreReg);
      storeBaseDisp(cUnit, rRET0,
                    Array::DataOffset(component_size).Int32Value() +
                    i * 4, rlArg.lowReg, kWord);
      // If the loadValue caused a temp to be allocated, free it
      if (oatIsTemp(cUnit, rlArg.lowReg)) {
        oatFreeTemp(cUnit, rlArg.lowReg);
      }
    }
  }
  if (info->result.location != kLocInvalid) {
    storeValue(cUnit, info->result, oatGetReturn(cUnit, false /* not fp */));
  }
}

void genSput(CompilationUnit* cUnit, uint32_t fieldIdx, RegLocation rlSrc,
       bool isLongOrDouble, bool isObject)
{
  int fieldOffset;
  int ssbIndex;
  bool isVolatile;
  bool isReferrersClass;

  OatCompilationUnit mUnit(cUnit->class_loader, cUnit->class_linker, *cUnit->dex_file,
                           cUnit->code_item, cUnit->method_idx, cUnit->access_flags);

  bool fastPath =
      cUnit->compiler->ComputeStaticFieldInfo(fieldIdx, &mUnit,
                                              fieldOffset, ssbIndex,
                                              isReferrersClass, isVolatile,
                                              true);
  if (fastPath && !SLOW_FIELD_PATH) {
    DCHECK_GE(fieldOffset, 0);
    int rBase;
    if (isReferrersClass) {
      // Fast path, static storage base is this method's class
      RegLocation rlMethod  = loadCurrMethod(cUnit);
      rBase = oatAllocTemp(cUnit);
      loadWordDisp(cUnit, rlMethod.lowReg,
                   AbstractMethod::DeclaringClassOffset().Int32Value(), rBase);
      if (oatIsTemp(cUnit, rlMethod.lowReg)) {
        oatFreeTemp(cUnit, rlMethod.lowReg);
      }
    } else {
      // Medium path, static storage base in a different class which
      // requires checks that the other class is initialized.
      DCHECK_GE(ssbIndex, 0);
      // May do runtime call so everything to home locations.
      oatFlushAllRegs(cUnit);
      // Using fixed register to sync with possible call to runtime
      // support.
      int rMethod = rARG1;
      oatLockTemp(cUnit, rMethod);
      loadCurrMethodDirect(cUnit, rMethod);
      rBase = rARG0;
      oatLockTemp(cUnit, rBase);
      loadWordDisp(cUnit, rMethod,
                   AbstractMethod::DexCacheInitializedStaticStorageOffset().Int32Value(),
                   rBase);
      loadWordDisp(cUnit, rBase,
                   Array::DataOffset(sizeof(Object*)).Int32Value() +
                   sizeof(int32_t*) * ssbIndex, rBase);
      // rBase now points at appropriate static storage base (Class*)
      // or NULL if not initialized. Check for NULL and call helper if NULL.
      // TUNING: fast path should fall through
      LIR* branchOver = opCmpImmBranch(cUnit, kCondNe, rBase, 0, NULL);
      loadConstant(cUnit, rARG0, ssbIndex);
      callRuntimeHelperImm(cUnit, ENTRYPOINT_OFFSET(pInitializeStaticStorage), ssbIndex, true);
#if defined(TARGET_MIPS)
      // For Arm, rRET0 = rARG0 = rBASE, for Mips, we need to copy
      opRegCopy(cUnit, rBase, rRET0);
#endif
      LIR* skipTarget = newLIR0(cUnit, kPseudoTargetLabel);
      branchOver->target = (LIR*)skipTarget;
      oatFreeTemp(cUnit, rMethod);
    }
    // rBase now holds static storage base
    if (isLongOrDouble) {
      rlSrc = loadValueWide(cUnit, rlSrc, kAnyReg);
    } else {
      rlSrc = loadValue(cUnit, rlSrc, kAnyReg);
    }
//FIXME: need to generalize the barrier call
    if (isVolatile) {
      oatGenMemBarrier(cUnit, kST);
    }
    if (isLongOrDouble) {
      storeBaseDispWide(cUnit, rBase, fieldOffset, rlSrc.lowReg,
                        rlSrc.highReg);
    } else {
      storeWordDisp(cUnit, rBase, fieldOffset, rlSrc.lowReg);
    }
    if (isVolatile) {
      oatGenMemBarrier(cUnit, kSY);
    }
    if (isObject) {
      markGCCard(cUnit, rlSrc.lowReg, rBase);
    }
    oatFreeTemp(cUnit, rBase);
  } else {
    oatFlushAllRegs(cUnit);  // Everything to home locations
    int setterOffset = isLongOrDouble ? ENTRYPOINT_OFFSET(pSet64Static) :
        (isObject ? ENTRYPOINT_OFFSET(pSetObjStatic)
        : ENTRYPOINT_OFFSET(pSet32Static));
    callRuntimeHelperImmRegLocation(cUnit, setterOffset, fieldIdx, rlSrc, true);
  }
}

void genSget(CompilationUnit* cUnit, uint32_t fieldIdx, RegLocation rlDest,
       bool isLongOrDouble, bool isObject)
{
  int fieldOffset;
  int ssbIndex;
  bool isVolatile;
  bool isReferrersClass;

  OatCompilationUnit mUnit(cUnit->class_loader, cUnit->class_linker,
                           *cUnit->dex_file,
                           cUnit->code_item, cUnit->method_idx,
                           cUnit->access_flags);

  bool fastPath =
    cUnit->compiler->ComputeStaticFieldInfo(fieldIdx, &mUnit,
                                            fieldOffset, ssbIndex,
                                            isReferrersClass, isVolatile,
                                            false);
  if (fastPath && !SLOW_FIELD_PATH) {
    DCHECK_GE(fieldOffset, 0);
    int rBase;
    if (isReferrersClass) {
      // Fast path, static storage base is this method's class
      RegLocation rlMethod  = loadCurrMethod(cUnit);
      rBase = oatAllocTemp(cUnit);
      loadWordDisp(cUnit, rlMethod.lowReg,
                   AbstractMethod::DeclaringClassOffset().Int32Value(), rBase);
    } else {
      // Medium path, static storage base in a different class which
      // requires checks that the other class is initialized
      DCHECK_GE(ssbIndex, 0);
      // May do runtime call so everything to home locations.
      oatFlushAllRegs(cUnit);
      // Using fixed register to sync with possible call to runtime
      // support
      int rMethod = rARG1;
      oatLockTemp(cUnit, rMethod);
      loadCurrMethodDirect(cUnit, rMethod);
      rBase = rARG0;
      oatLockTemp(cUnit, rBase);
      loadWordDisp(cUnit, rMethod,
                   AbstractMethod::DexCacheInitializedStaticStorageOffset().Int32Value(),
                   rBase);
      loadWordDisp(cUnit, rBase,
                   Array::DataOffset(sizeof(Object*)).Int32Value() +
                   sizeof(int32_t*) * ssbIndex, rBase);
      // rBase now points at appropriate static storage base (Class*)
      // or NULL if not initialized. Check for NULL and call helper if NULL.
      // TUNING: fast path should fall through
      LIR* branchOver = opCmpImmBranch(cUnit, kCondNe, rBase, 0, NULL);
      callRuntimeHelperImm(cUnit, ENTRYPOINT_OFFSET(pInitializeStaticStorage), ssbIndex, true);
#if defined(TARGET_MIPS)
      // For Arm, rRET0 = rARG0 = rBASE, for Mips, we need to copy
      opRegCopy(cUnit, rBase, rRET0);
#endif
      LIR* skipTarget = newLIR0(cUnit, kPseudoTargetLabel);
      branchOver->target = (LIR*)skipTarget;
      oatFreeTemp(cUnit, rMethod);
    }
    // rBase now holds static storage base
    RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kAnyReg, true);
    if (isVolatile) {
      oatGenMemBarrier(cUnit, kSY);
    }
    if (isLongOrDouble) {
      loadBaseDispWide(cUnit, rBase, fieldOffset, rlResult.lowReg,
                       rlResult.highReg, INVALID_SREG);
    } else {
      loadWordDisp(cUnit, rBase, fieldOffset, rlResult.lowReg);
    }
    oatFreeTemp(cUnit, rBase);
    if (isLongOrDouble) {
      storeValueWide(cUnit, rlDest, rlResult);
    } else {
      storeValue(cUnit, rlDest, rlResult);
    }
  } else {
    oatFlushAllRegs(cUnit);  // Everything to home locations
    int getterOffset = isLongOrDouble ? ENTRYPOINT_OFFSET(pGet64Static) :
        (isObject ? ENTRYPOINT_OFFSET(pGetObjStatic)
        : ENTRYPOINT_OFFSET(pGet32Static));
    callRuntimeHelperImm(cUnit, getterOffset, fieldIdx, true);
    if (isLongOrDouble) {
      RegLocation rlResult = oatGetReturnWide(cUnit, rlDest.fp);
      storeValueWide(cUnit, rlDest, rlResult);
    } else {
      RegLocation rlResult = oatGetReturn(cUnit, rlDest.fp);
      storeValue(cUnit, rlDest, rlResult);
    }
  }
}


// Debugging routine - if null target, branch to DebugMe
void genShowTarget(CompilationUnit* cUnit)
{
#if defined(TARGET_X86)
  UNIMPLEMENTED(WARNING) << "genShowTarget";
#else
  LIR* branchOver = opCmpImmBranch(cUnit, kCondNe, rINVOKE_TGT, 0, NULL);
  loadWordDisp(cUnit, rSELF, ENTRYPOINT_OFFSET(pDebugMe), rINVOKE_TGT);
  LIR* target = newLIR0(cUnit, kPseudoTargetLabel);
  branchOver->target = (LIR*)target;
#endif
}

void handleSuspendLaunchpads(CompilationUnit *cUnit)
{
  LIR** suspendLabel = (LIR **)cUnit->suspendLaunchpads.elemList;
  int numElems = cUnit->suspendLaunchpads.numUsed;
  for (int i = 0; i < numElems; i++) {
    oatResetRegPool(cUnit);
    oatResetDefTracking(cUnit);
    LIR* lab = suspendLabel[i];
    LIR* resumeLab = (LIR*)lab->operands[0];
    cUnit->currentDalvikOffset = lab->operands[1];
    oatAppendLIR(cUnit, lab);
#if defined(TARGET_X86)
    LIR* callInst = opThreadMem(cUnit, kOpBlx, ENTRYPOINT_OFFSET(pTestSuspendFromCode));
#else
    int rTgt = loadHelper(cUnit, ENTRYPOINT_OFFSET(pTestSuspendFromCode));
    LIR* callInst = opReg(cUnit, kOpBlx, rTgt);
#endif
    markSafepointPC(cUnit, callInst);
    opUnconditionalBranch(cUnit, resumeLab);
  }
}

void handleIntrinsicLaunchpads(CompilationUnit *cUnit)
{
  LIR** intrinsicLabel = (LIR **)cUnit->intrinsicLaunchpads.elemList;
  int numElems = cUnit->intrinsicLaunchpads.numUsed;
  for (int i = 0; i < numElems; i++) {
    oatResetRegPool(cUnit);
    oatResetDefTracking(cUnit);
    LIR* lab = intrinsicLabel[i];
    CallInfo* info = (CallInfo*)lab->operands[0];
    cUnit->currentDalvikOffset = info->offset;
    oatAppendLIR(cUnit, lab);
    // NOTE: genInvoke handles markSafepointPC
    genInvoke(cUnit, info);
    LIR* resumeLab = (LIR*)lab->operands[2];
    if (resumeLab != NULL) {
      opUnconditionalBranch(cUnit, resumeLab);
    }
  }
}

void handleThrowLaunchpads(CompilationUnit *cUnit)
{
  LIR** throwLabel = (LIR **)cUnit->throwLaunchpads.elemList;
  int numElems = cUnit->throwLaunchpads.numUsed;
  for (int i = 0; i < numElems; i++) {
    oatResetRegPool(cUnit);
    oatResetDefTracking(cUnit);
    LIR* lab = throwLabel[i];
    cUnit->currentDalvikOffset = lab->operands[1];
    oatAppendLIR(cUnit, lab);
    int funcOffset = 0;
    int v1 = lab->operands[2];
    int v2 = lab->operands[3];
    switch (lab->operands[0]) {
      case kThrowNullPointer:
        funcOffset = ENTRYPOINT_OFFSET(pThrowNullPointerFromCode);
        break;
      case kThrowArrayBounds:
        // Move v1 (array index) to rARG0 and v2 (array length) to rARG1
        if (v2 != rARG0) {
          opRegCopy(cUnit, rARG0, v1);
#if defined (TARGET_X86)
          // x86 leaves the array pointer in v2, so load the array length that the handler expects
          opRegMem(cUnit, kOpMov, rARG1, v2, Array::LengthOffset().Int32Value());
#else
          opRegCopy(cUnit, rARG1, v2);
#endif
        } else {
          if (v1 == rARG1) {
            // Swap v1 and v2, using rARG2 as a temp
            opRegCopy(cUnit, rARG2, v1);
#if defined (TARGET_X86)
            // x86 leaves the array pointer in v2, so load the array length that the handler expects
            opRegMem(cUnit, kOpMov, rARG1, v2, Array::LengthOffset().Int32Value());
#else
            opRegCopy(cUnit, rARG1, v2);
#endif
            opRegCopy(cUnit, rARG0, rARG2);
          } else {
#if defined (TARGET_X86)
            // x86 leaves the array pointer in v2, so load the array length that the handler expects
            opRegMem(cUnit, kOpMov, rARG1, v2, Array::LengthOffset().Int32Value());
#else
            opRegCopy(cUnit, rARG1, v2);
#endif
            opRegCopy(cUnit, rARG0, v1);
          }
        }
        funcOffset = ENTRYPOINT_OFFSET(pThrowArrayBoundsFromCode);
        break;
      case kThrowDivZero:
        funcOffset = ENTRYPOINT_OFFSET(pThrowDivZeroFromCode);
        break;
      case kThrowNoSuchMethod:
        opRegCopy(cUnit, rARG0, v1);
        funcOffset =
          ENTRYPOINT_OFFSET(pThrowNoSuchMethodFromCode);
        break;
      case kThrowStackOverflow:
        funcOffset = ENTRYPOINT_OFFSET(pThrowStackOverflowFromCode);
        // Restore stack alignment
#if !defined(TARGET_X86)
        opRegImm(cUnit, kOpAdd, rSP,
                 (cUnit->numCoreSpills + cUnit->numFPSpills) * 4);
#else
        opRegImm(cUnit, kOpAdd, rSP, cUnit->frameSize);
#endif
        break;
      default:
        LOG(FATAL) << "Unexpected throw kind: " << lab->operands[0];
    }
    oatClobberCalleeSave(cUnit);
#if !defined(TARGET_X86)
    int rTgt = loadHelper(cUnit, funcOffset);
    LIR* callInst = opReg(cUnit, kOpBlx, rTgt);
    oatFreeTemp(cUnit, rTgt);
#else
    LIR* callInst = opThreadMem(cUnit, kOpBlx, funcOffset);
#endif
    markSafepointPC(cUnit, callInst);
  }
}

/* Needed by the Assembler */
void oatSetupResourceMasks(LIR* lir)
{
  setupResourceMasks(lir);
}

bool fastInstance(CompilationUnit* cUnit,  uint32_t fieldIdx,
                  int& fieldOffset, bool& isVolatile, bool isPut)
{
  OatCompilationUnit mUnit(cUnit->class_loader, cUnit->class_linker,
               *cUnit->dex_file,
               cUnit->code_item, cUnit->method_idx,
               cUnit->access_flags);
  return cUnit->compiler->ComputeInstanceFieldInfo(fieldIdx, &mUnit,
           fieldOffset, isVolatile, isPut);
}

void genIGet(CompilationUnit* cUnit, uint32_t fieldIdx, int optFlags, OpSize size,
             RegLocation rlDest, RegLocation rlObj,
             bool isLongOrDouble, bool isObject)
{
  int fieldOffset;
  bool isVolatile;

  bool fastPath = fastInstance(cUnit, fieldIdx, fieldOffset, isVolatile, false);

  if (fastPath && !SLOW_FIELD_PATH) {
    RegLocation rlResult;
    RegisterClass regClass = oatRegClassBySize(size);
    DCHECK_GE(fieldOffset, 0);
    rlObj = loadValue(cUnit, rlObj, kCoreReg);
    if (isLongOrDouble) {
      DCHECK(rlDest.wide);
      genNullCheck(cUnit, rlObj.sRegLow, rlObj.lowReg, optFlags);
#if defined(TARGET_X86)
      rlResult = oatEvalLoc(cUnit, rlDest, regClass, true);
      genNullCheck(cUnit, rlObj.sRegLow, rlObj.lowReg, optFlags);
      loadBaseDispWide(cUnit, rlObj.lowReg, fieldOffset, rlResult.lowReg,
                       rlResult.highReg, rlObj.sRegLow);
      if (isVolatile) {
        oatGenMemBarrier(cUnit, kSY);
      }
#else
      int regPtr = oatAllocTemp(cUnit);
      opRegRegImm(cUnit, kOpAdd, regPtr, rlObj.lowReg, fieldOffset);
      rlResult = oatEvalLoc(cUnit, rlDest, regClass, true);
      loadPair(cUnit, regPtr, rlResult.lowReg, rlResult.highReg);
      if (isVolatile) {
        oatGenMemBarrier(cUnit, kSY);
      }
      oatFreeTemp(cUnit, regPtr);
#endif
      storeValueWide(cUnit, rlDest, rlResult);
    } else {
      rlResult = oatEvalLoc(cUnit, rlDest, regClass, true);
      genNullCheck(cUnit, rlObj.sRegLow, rlObj.lowReg, optFlags);
      loadBaseDisp(cUnit, rlObj.lowReg, fieldOffset, rlResult.lowReg,
                   kWord, rlObj.sRegLow);
      if (isVolatile) {
        oatGenMemBarrier(cUnit, kSY);
      }
      storeValue(cUnit, rlDest, rlResult);
    }
  } else {
    int getterOffset = isLongOrDouble ? ENTRYPOINT_OFFSET(pGet64Instance) :
        (isObject ? ENTRYPOINT_OFFSET(pGetObjInstance)
        : ENTRYPOINT_OFFSET(pGet32Instance));
    callRuntimeHelperImmRegLocation(cUnit, getterOffset, fieldIdx, rlObj, true);
    if (isLongOrDouble) {
      RegLocation rlResult = oatGetReturnWide(cUnit, rlDest.fp);
      storeValueWide(cUnit, rlDest, rlResult);
    } else {
      RegLocation rlResult = oatGetReturn(cUnit, rlDest.fp);
      storeValue(cUnit, rlDest, rlResult);
    }
  }
}

void genIPut(CompilationUnit* cUnit, uint32_t fieldIdx, int optFlags, OpSize size,
             RegLocation rlSrc, RegLocation rlObj, bool isLongOrDouble, bool isObject)
{
  int fieldOffset;
  bool isVolatile;

  bool fastPath = fastInstance(cUnit, fieldIdx, fieldOffset, isVolatile,
                 true);
  if (fastPath && !SLOW_FIELD_PATH) {
    RegisterClass regClass = oatRegClassBySize(size);
    DCHECK_GE(fieldOffset, 0);
    rlObj = loadValue(cUnit, rlObj, kCoreReg);
    if (isLongOrDouble) {
      int regPtr;
      rlSrc = loadValueWide(cUnit, rlSrc, kAnyReg);
      genNullCheck(cUnit, rlObj.sRegLow, rlObj.lowReg, optFlags);
      regPtr = oatAllocTemp(cUnit);
      opRegRegImm(cUnit, kOpAdd, regPtr, rlObj.lowReg, fieldOffset);
      if (isVolatile) {
        oatGenMemBarrier(cUnit, kST);
      }
      storeBaseDispWide(cUnit, regPtr, 0, rlSrc.lowReg, rlSrc.highReg);
      if (isVolatile) {
        oatGenMemBarrier(cUnit, kSY);
      }
      oatFreeTemp(cUnit, regPtr);
    } else {
      rlSrc = loadValue(cUnit, rlSrc, regClass);
      genNullCheck(cUnit, rlObj.sRegLow, rlObj.lowReg, optFlags);
      if (isVolatile) {
        oatGenMemBarrier(cUnit, kST);
      }
      storeBaseDisp(cUnit, rlObj.lowReg, fieldOffset, rlSrc.lowReg, kWord);
      if (isVolatile) {
        oatGenMemBarrier(cUnit, kSY);
      }
      if (isObject) {
        markGCCard(cUnit, rlSrc.lowReg, rlObj.lowReg);
      }
    }
  } else {
    int setterOffset = isLongOrDouble ? ENTRYPOINT_OFFSET(pSet64Instance) :
        (isObject ? ENTRYPOINT_OFFSET(pSetObjInstance)
        : ENTRYPOINT_OFFSET(pSet32Instance));
    callRuntimeHelperImmRegLocationRegLocation(cUnit, setterOffset, fieldIdx, rlObj, rlSrc, true);
  }
}

void genConstClass(CompilationUnit* cUnit, uint32_t type_idx,
                   RegLocation rlDest)
{
  RegLocation rlMethod = loadCurrMethod(cUnit);
  int resReg = oatAllocTemp(cUnit);
  RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
  if (!cUnit->compiler->CanAccessTypeWithoutChecks(cUnit->method_idx,
                                                   *cUnit->dex_file,
                                                   type_idx)) {
    // Call out to helper which resolves type and verifies access.
    // Resolved type returned in rRET0.
    callRuntimeHelperImmReg(cUnit, ENTRYPOINT_OFFSET(pInitializeTypeAndVerifyAccessFromCode),
                            type_idx, rlMethod.lowReg, true);
    RegLocation rlResult = oatGetReturn(cUnit, false);
    storeValue(cUnit, rlDest, rlResult);
  } else {
    // We're don't need access checks, load type from dex cache
    int32_t dex_cache_offset =
        AbstractMethod::DexCacheResolvedTypesOffset().Int32Value();
    loadWordDisp(cUnit, rlMethod.lowReg, dex_cache_offset, resReg);
    int32_t offset_of_type =
        Array::DataOffset(sizeof(Class*)).Int32Value() + (sizeof(Class*)
                          * type_idx);
    loadWordDisp(cUnit, resReg, offset_of_type, rlResult.lowReg);
    if (!cUnit->compiler->CanAssumeTypeIsPresentInDexCache(*cUnit->dex_file,
        type_idx) || SLOW_TYPE_PATH) {
      // Slow path, at runtime test if type is null and if so initialize
      oatFlushAllRegs(cUnit);
      LIR* branch1 = opCmpImmBranch(cUnit, kCondEq, rlResult.lowReg, 0, NULL);
      // Resolved, store and hop over following code
      storeValue(cUnit, rlDest, rlResult);
      /*
       * Because we have stores of the target value on two paths,
       * clobber temp tracking for the destination using the ssa name
       */
      oatClobberSReg(cUnit, rlDest.sRegLow);
      LIR* branch2 = opUnconditionalBranch(cUnit,0);
      // TUNING: move slow path to end & remove unconditional branch
      LIR* target1 = newLIR0(cUnit, kPseudoTargetLabel);
      // Call out to helper, which will return resolved type in rARG0
      callRuntimeHelperImmReg(cUnit, ENTRYPOINT_OFFSET(pInitializeTypeFromCode), type_idx,
                              rlMethod.lowReg, true);
      RegLocation rlResult = oatGetReturn(cUnit, false);
      storeValue(cUnit, rlDest, rlResult);
      /*
       * Because we have stores of the target value on two paths,
       * clobber temp tracking for the destination using the ssa name
       */
      oatClobberSReg(cUnit, rlDest.sRegLow);
      // Rejoin code paths
      LIR* target2 = newLIR0(cUnit, kPseudoTargetLabel);
      branch1->target = (LIR*)target1;
      branch2->target = (LIR*)target2;
    } else {
      // Fast path, we're done - just store result
      storeValue(cUnit, rlDest, rlResult);
    }
  }
}

void genConstString(CompilationUnit* cUnit, uint32_t string_idx,
                    RegLocation rlDest)
{
  /* NOTE: Most strings should be available at compile time */
  int32_t offset_of_string = Array::DataOffset(sizeof(String*)).Int32Value() +
                 (sizeof(String*) * string_idx);
  if (!cUnit->compiler->CanAssumeStringIsPresentInDexCache(
      *cUnit->dex_file, string_idx) || SLOW_STRING_PATH) {
    // slow path, resolve string if not in dex cache
    oatFlushAllRegs(cUnit);
    oatLockCallTemps(cUnit); // Using explicit registers
    loadCurrMethodDirect(cUnit, rARG2);
    loadWordDisp(cUnit, rARG2,
                 AbstractMethod::DexCacheStringsOffset().Int32Value(), rARG0);
    // Might call out to helper, which will return resolved string in rRET0
#if !defined(TARGET_X86)
    int rTgt = loadHelper(cUnit, ENTRYPOINT_OFFSET(pResolveStringFromCode));
#endif
    loadWordDisp(cUnit, rARG0, offset_of_string, rRET0);
    loadConstant(cUnit, rARG1, string_idx);
#if defined(TARGET_ARM)
    opRegImm(cUnit, kOpCmp, rRET0, 0);  // Is resolved?
    genBarrier(cUnit);
    // For testing, always force through helper
    if (!EXERCISE_SLOWEST_STRING_PATH) {
      opIT(cUnit, kArmCondEq, "T");
    }
    opRegCopy(cUnit, rARG0, rARG2);   // .eq
    LIR* callInst = opReg(cUnit, kOpBlx, rTgt);    // .eq, helper(Method*, string_idx)
    markSafepointPC(cUnit, callInst);
    oatFreeTemp(cUnit, rTgt);
#elif defined(TARGET_MIPS)
    LIR* branch = opCmpImmBranch(cUnit, kCondNe, rRET0, 0, NULL);
    opRegCopy(cUnit, rARG0, rARG2);   // .eq
    LIR* callInst = opReg(cUnit, kOpBlx, rTgt);
    markSafepointPC(cUnit, callInst);
    oatFreeTemp(cUnit, rTgt);
    LIR* target = newLIR0(cUnit, kPseudoTargetLabel);
    branch->target = target;
#else
    callRuntimeHelperRegReg(cUnit, ENTRYPOINT_OFFSET(pResolveStringFromCode), rARG2, rARG1, true);
#endif
    genBarrier(cUnit);
    storeValue(cUnit, rlDest, oatGetReturn(cUnit, false));
  } else {
    RegLocation rlMethod = loadCurrMethod(cUnit);
    int resReg = oatAllocTemp(cUnit);
    RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
    loadWordDisp(cUnit, rlMethod.lowReg,
                 AbstractMethod::DexCacheStringsOffset().Int32Value(), resReg);
    loadWordDisp(cUnit, resReg, offset_of_string, rlResult.lowReg);
    storeValue(cUnit, rlDest, rlResult);
  }
}

/*
 * Let helper function take care of everything.  Will
 * call Class::NewInstanceFromCode(type_idx, method);
 */
void genNewInstance(CompilationUnit* cUnit, uint32_t type_idx, RegLocation rlDest)
{
  oatFlushAllRegs(cUnit);  /* Everything to home location */
  // alloc will always check for resolution, do we also need to verify
  // access because the verifier was unable to?
  int funcOffset;
  if (cUnit->compiler->CanAccessInstantiableTypeWithoutChecks(
      cUnit->method_idx, *cUnit->dex_file, type_idx)) {
    funcOffset = ENTRYPOINT_OFFSET(pAllocObjectFromCode);
  } else {
    funcOffset = ENTRYPOINT_OFFSET(pAllocObjectFromCodeWithAccessCheck);
  }
  callRuntimeHelperImmMethod(cUnit, funcOffset, type_idx, true);
  RegLocation rlResult = oatGetReturn(cUnit, false);
  storeValue(cUnit, rlDest, rlResult);
}

void genMoveException(CompilationUnit* cUnit, RegLocation rlDest)
{
  oatFlushAllRegs(cUnit);  /* Everything to home location */
  int funcOffset = ENTRYPOINT_OFFSET(pGetAndClearException);
#if defined(TARGET_X86)
  // Runtime helper will load argument for x86.
  callRuntimeHelperReg(cUnit, funcOffset, rARG0, false);
#else
  callRuntimeHelperReg(cUnit, funcOffset, rSELF, false);
#endif
  RegLocation rlResult = oatGetReturn(cUnit, false);
  storeValue(cUnit, rlDest, rlResult);
}

void genThrow(CompilationUnit* cUnit, RegLocation rlSrc)
{
  oatFlushAllRegs(cUnit);
  callRuntimeHelperRegLocation(cUnit, ENTRYPOINT_OFFSET(pDeliverException), rlSrc, true);
}

void genInstanceof(CompilationUnit* cUnit, uint32_t type_idx, RegLocation rlDest,
                   RegLocation rlSrc)
{
  oatFlushAllRegs(cUnit);
  // May generate a call - use explicit registers
  oatLockCallTemps(cUnit);
  loadCurrMethodDirect(cUnit, rARG1);  // rARG1 <= current Method*
  int classReg = rARG2;  // rARG2 will hold the Class*
  if (!cUnit->compiler->CanAccessTypeWithoutChecks(cUnit->method_idx,
                                                   *cUnit->dex_file,
                                                   type_idx)) {
    // Check we have access to type_idx and if not throw IllegalAccessError,
    // returns Class* in rARG0
    callRuntimeHelperImm(cUnit, ENTRYPOINT_OFFSET(pInitializeTypeAndVerifyAccessFromCode),
                         type_idx, true);
    opRegCopy(cUnit, classReg, rRET0);  // Align usage with fast path
    loadValueDirectFixed(cUnit, rlSrc, rARG0);  // rARG0 <= ref
  } else {
    // Load dex cache entry into classReg (rARG2)
    loadValueDirectFixed(cUnit, rlSrc, rARG0);  // rARG0 <= ref
    loadWordDisp(cUnit, rARG1,
                 AbstractMethod::DexCacheResolvedTypesOffset().Int32Value(), classReg);
    int32_t offset_of_type =
        Array::DataOffset(sizeof(Class*)).Int32Value() + (sizeof(Class*)
        * type_idx);
    loadWordDisp(cUnit, classReg, offset_of_type, classReg);
    if (!cUnit->compiler->CanAssumeTypeIsPresentInDexCache(
        *cUnit->dex_file, type_idx)) {
      // Need to test presence of type in dex cache at runtime
      LIR* hopBranch = opCmpImmBranch(cUnit, kCondNe, classReg, 0, NULL);
      // Not resolved
      // Call out to helper, which will return resolved type in rRET0
      callRuntimeHelperImm(cUnit, ENTRYPOINT_OFFSET(pInitializeTypeFromCode), type_idx, true);
      opRegCopy(cUnit, rARG2, rRET0); // Align usage with fast path
      loadValueDirectFixed(cUnit, rlSrc, rARG0);  /* reload Ref */
      // Rejoin code paths
      LIR* hopTarget = newLIR0(cUnit, kPseudoTargetLabel);
      hopBranch->target = (LIR*)hopTarget;
    }
  }
  /* rARG0 is ref, rARG2 is class. If ref==null, use directly as bool result */
  RegLocation rlResult = oatGetReturn(cUnit, false);
#if defined(TARGET_MIPS)
  opRegCopy(cUnit, rlResult.lowReg, r_ZERO);    // store false result for if branch is taken
#endif
  LIR* branch1 = opCmpImmBranch(cUnit, kCondEq, rARG0, 0, NULL);
  /* load object->klass_ */
  DCHECK_EQ(Object::ClassOffset().Int32Value(), 0);
  loadWordDisp(cUnit, rARG0,  Object::ClassOffset().Int32Value(), rARG1);
  /* rARG0 is ref, rARG1 is ref->klass_, rARG2 is class */
  LIR* callInst;
#if defined(TARGET_ARM)
  /* Uses conditional nullification */
  int rTgt = loadHelper(cUnit,
                        ENTRYPOINT_OFFSET(pInstanceofNonTrivialFromCode));
  opRegReg(cUnit, kOpCmp, rARG1, rARG2);  // Same?
  opIT(cUnit, kArmCondEq, "EE");   // if-convert the test
  loadConstant(cUnit, rARG0, 1);     // .eq case - load true
  opRegCopy(cUnit, rARG0, rARG2);    // .ne case - arg0 <= class
  callInst = opReg(cUnit, kOpBlx, rTgt);    // .ne case: helper(class, ref->class)
  oatFreeTemp(cUnit, rTgt);
#else
  /* Uses branchovers */
  loadConstant(cUnit, rlResult.lowReg, 1);     // assume true
  LIR* branchover = opCmpBranch(cUnit, kCondEq, rARG1, rARG2, NULL);
#if !defined(TARGET_X86)
  int rTgt = loadHelper(cUnit,
                        ENTRYPOINT_OFFSET(pInstanceofNonTrivialFromCode));
  opRegCopy(cUnit, rARG0, rARG2);    // .ne case - arg0 <= class
  callInst = opReg(cUnit, kOpBlx, rTgt);    // .ne case: helper(class, ref->class)
  oatFreeTemp(cUnit, rTgt);
#else
  opRegCopy(cUnit, rARG0, rARG2);
  callInst = opThreadMem(cUnit, kOpBlx, ENTRYPOINT_OFFSET(pInstanceofNonTrivialFromCode));
#endif
#endif
  markSafepointPC(cUnit, callInst);
  oatClobberCalleeSave(cUnit);
  /* branch targets here */
  LIR* target = newLIR0(cUnit, kPseudoTargetLabel);
  storeValue(cUnit, rlDest, rlResult);
  branch1->target = target;
#if !defined(TARGET_ARM)
  branchover->target = target;
#endif
}

void genCheckCast(CompilationUnit* cUnit, uint32_t type_idx, RegLocation rlSrc)
{
  oatFlushAllRegs(cUnit);
  // May generate a call - use explicit registers
  oatLockCallTemps(cUnit);
  loadCurrMethodDirect(cUnit, rARG1);  // rARG1 <= current Method*
  int classReg = rARG2;  // rARG2 will hold the Class*
  if (!cUnit->compiler->CanAccessTypeWithoutChecks(cUnit->method_idx,
                                                   *cUnit->dex_file,
                                                   type_idx)) {
    // Check we have access to type_idx and if not throw IllegalAccessError,
    // returns Class* in rRET0
    // InitializeTypeAndVerifyAccess(idx, method)
    callRuntimeHelperImmReg(cUnit, ENTRYPOINT_OFFSET(pInitializeTypeAndVerifyAccessFromCode),
                            type_idx, rARG1, true);
    opRegCopy(cUnit, classReg, rRET0);  // Align usage with fast path
  } else {
    // Load dex cache entry into classReg (rARG2)
    loadWordDisp(cUnit, rARG1,
                 AbstractMethod::DexCacheResolvedTypesOffset().Int32Value(), classReg);
    int32_t offset_of_type =
        Array::DataOffset(sizeof(Class*)).Int32Value() +
        (sizeof(Class*) * type_idx);
    loadWordDisp(cUnit, classReg, offset_of_type, classReg);
    if (!cUnit->compiler->CanAssumeTypeIsPresentInDexCache(
        *cUnit->dex_file, type_idx)) {
      // Need to test presence of type in dex cache at runtime
      LIR* hopBranch = opCmpImmBranch(cUnit, kCondNe, classReg, 0, NULL);
      // Not resolved
      // Call out to helper, which will return resolved type in rARG0
      // InitializeTypeFromCode(idx, method)
      callRuntimeHelperImmReg(cUnit, ENTRYPOINT_OFFSET(pInitializeTypeFromCode), type_idx, rARG1,
                              true);
      opRegCopy(cUnit, classReg, rRET0); // Align usage with fast path
      // Rejoin code paths
      LIR* hopTarget = newLIR0(cUnit, kPseudoTargetLabel);
      hopBranch->target = (LIR*)hopTarget;
    }
  }
  // At this point, classReg (rARG2) has class
  loadValueDirectFixed(cUnit, rlSrc, rARG0);  // rARG0 <= ref
  /* Null is OK - continue */
  LIR* branch1 = opCmpImmBranch(cUnit, kCondEq, rARG0, 0, NULL);
  /* load object->klass_ */
  DCHECK_EQ(Object::ClassOffset().Int32Value(), 0);
  loadWordDisp(cUnit, rARG0,  Object::ClassOffset().Int32Value(), rARG1);
  /* rARG1 now contains object->klass_ */
#if defined(TARGET_MIPS) || defined(TARGET_X86)
  LIR* branch2 = opCmpBranch(cUnit, kCondEq, rARG1, classReg, NULL);
  callRuntimeHelperRegReg(cUnit, ENTRYPOINT_OFFSET(pCheckCastFromCode), rARG1, rARG2, true);
#else  // defined(TARGET_ARM)
  int rTgt = loadHelper(cUnit, ENTRYPOINT_OFFSET(pCheckCastFromCode));
  opRegReg(cUnit, kOpCmp, rARG1, classReg);
  LIR* branch2 = opCondBranch(cUnit, kCondEq, NULL); /* If eq, trivial yes */
  opRegCopy(cUnit, rARG0, rARG1);
  opRegCopy(cUnit, rARG1, rARG2);
  oatClobberCalleeSave(cUnit);
  LIR* callInst = opReg(cUnit, kOpBlx, rTgt);
  markSafepointPC(cUnit, callInst);
  oatFreeTemp(cUnit, rTgt);
#endif
  /* branch target here */
  LIR* target = newLIR0(cUnit, kPseudoTargetLabel);
  branch1->target = target;
  branch2->target = target;
}

/*
 * Generate array store
 *
 */
void genArrayObjPut(CompilationUnit* cUnit, int optFlags, RegLocation rlArray,
          RegLocation rlIndex, RegLocation rlSrc, int scale)
{
  int lenOffset = Array::LengthOffset().Int32Value();
  int dataOffset = Array::DataOffset(sizeof(Object*)).Int32Value();

  oatFlushAllRegs(cUnit);  // Use explicit registers
  oatLockCallTemps(cUnit);

  int rValue = rARG0;  // Register holding value
  int rArrayClass = rARG1;  // Register holding array's Class
  int rArray = rARG2;  // Register holding array
  int rIndex = rARG3;  // Register holding index into array

  loadValueDirectFixed(cUnit, rlArray, rArray);  // Grab array
  loadValueDirectFixed(cUnit, rlSrc, rValue);  // Grab value
  loadValueDirectFixed(cUnit, rlIndex, rIndex);  // Grab index

  genNullCheck(cUnit, rlArray.sRegLow, rArray, optFlags);  // NPE?

  // Store of null?
  LIR* null_value_check = opCmpImmBranch(cUnit, kCondEq, rValue, 0, NULL);

  // Get the array's class.
  loadWordDisp(cUnit, rArray, Object::ClassOffset().Int32Value(), rArrayClass);
  callRuntimeHelperRegReg(cUnit, ENTRYPOINT_OFFSET(pCanPutArrayElementFromCode), rValue,
                          rArrayClass, true);
  // Redo loadValues in case they didn't survive the call.
  loadValueDirectFixed(cUnit, rlArray, rArray);  // Reload array
  loadValueDirectFixed(cUnit, rlIndex, rIndex);  // Reload index
  loadValueDirectFixed(cUnit, rlSrc, rValue);  // Reload value
  rArrayClass = INVALID_REG;

  // Branch here if value to be stored == null
  LIR* target = newLIR0(cUnit, kPseudoTargetLabel);
  null_value_check->target = target;

#if defined(TARGET_X86)
  // make an extra temp available for card mark below
  oatFreeTemp(cUnit, rARG1);
  if (!(optFlags & MIR_IGNORE_RANGE_CHECK)) {
    /* if (rlIndex >= [rlArray + lenOffset]) goto kThrowArrayBounds */
    genRegMemCheck(cUnit, kCondUge, rIndex, rArray, lenOffset, kThrowArrayBounds);
  }
  storeBaseIndexedDisp(cUnit, rArray, rIndex, scale,
                       dataOffset, rValue, INVALID_REG, kWord, INVALID_SREG);
#else
  bool needsRangeCheck = (!(optFlags & MIR_IGNORE_RANGE_CHECK));
  int regLen = INVALID_REG;
  if (needsRangeCheck) {
    regLen = rARG1;
    loadWordDisp(cUnit, rArray, lenOffset, regLen);  // Get len
  }
  /* rPtr -> array data */
  int rPtr = oatAllocTemp(cUnit);
  opRegRegImm(cUnit, kOpAdd, rPtr, rArray, dataOffset);
  if (needsRangeCheck) {
    genRegRegCheck(cUnit, kCondCs, rIndex, regLen, kThrowArrayBounds);
  }
  storeBaseIndexed(cUnit, rPtr, rIndex, rValue, scale, kWord);
  oatFreeTemp(cUnit, rPtr);
#endif
  oatFreeTemp(cUnit, rIndex);
  markGCCard(cUnit, rValue, rArray);
}

/*
 * Generate array load
 */
void genArrayGet(CompilationUnit* cUnit, int optFlags, OpSize size,
                 RegLocation rlArray, RegLocation rlIndex,
                 RegLocation rlDest, int scale)
{
  RegisterClass regClass = oatRegClassBySize(size);
  int lenOffset = Array::LengthOffset().Int32Value();
  int dataOffset;
  RegLocation rlResult;
  rlArray = loadValue(cUnit, rlArray, kCoreReg);
  rlIndex = loadValue(cUnit, rlIndex, kCoreReg);

  if (size == kLong || size == kDouble) {
    dataOffset = Array::DataOffset(sizeof(int64_t)).Int32Value();
  } else {
    dataOffset = Array::DataOffset(sizeof(int32_t)).Int32Value();
  }

  /* null object? */
  genNullCheck(cUnit, rlArray.sRegLow, rlArray.lowReg, optFlags);

#if defined(TARGET_X86)
  if (!(optFlags & MIR_IGNORE_RANGE_CHECK)) {
    /* if (rlIndex >= [rlArray + lenOffset]) goto kThrowArrayBounds */
    genRegMemCheck(cUnit, kCondUge, rlIndex.lowReg, rlArray.lowReg,
                   lenOffset, kThrowArrayBounds);
  }
  if ((size == kLong) || (size == kDouble)) {
    int regAddr = oatAllocTemp(cUnit);
    newLIR5(cUnit, kX86Lea32RA, regAddr, rlArray.lowReg, rlIndex.lowReg, scale, dataOffset);
    oatFreeTemp(cUnit, rlArray.lowReg);
    oatFreeTemp(cUnit, rlIndex.lowReg);
    rlResult = oatEvalLoc(cUnit, rlDest, regClass, true);
    loadBaseIndexedDisp(cUnit, regAddr, INVALID_REG, 0, 0, rlResult.lowReg,
                        rlResult.highReg, size, INVALID_SREG);
    storeValueWide(cUnit, rlDest, rlResult);
  } else {
    rlResult = oatEvalLoc(cUnit, rlDest, regClass, true);

    loadBaseIndexedDisp(cUnit, rlArray.lowReg, rlIndex.lowReg, scale,
                        dataOffset, rlResult.lowReg, INVALID_REG, size,
                        INVALID_SREG);

    storeValue(cUnit, rlDest, rlResult);
  }
#else
  int regPtr = oatAllocTemp(cUnit);
  bool needsRangeCheck = (!(optFlags & MIR_IGNORE_RANGE_CHECK));
  int regLen = INVALID_REG;
  if (needsRangeCheck) {
    regLen = oatAllocTemp(cUnit);
    /* Get len */
    loadWordDisp(cUnit, rlArray.lowReg, lenOffset, regLen);
  }
  /* regPtr -> array data */
  opRegRegImm(cUnit, kOpAdd, regPtr, rlArray.lowReg, dataOffset);
  oatFreeTemp(cUnit, rlArray.lowReg);
  if ((size == kLong) || (size == kDouble)) {
    if (scale) {
      int rNewIndex = oatAllocTemp(cUnit);
      opRegRegImm(cUnit, kOpLsl, rNewIndex, rlIndex.lowReg, scale);
      opRegReg(cUnit, kOpAdd, regPtr, rNewIndex);
      oatFreeTemp(cUnit, rNewIndex);
    } else {
      opRegReg(cUnit, kOpAdd, regPtr, rlIndex.lowReg);
    }
    oatFreeTemp(cUnit, rlIndex.lowReg);
    rlResult = oatEvalLoc(cUnit, rlDest, regClass, true);

    if (needsRangeCheck) {
      // TODO: change kCondCS to a more meaningful name, is the sense of
      // carry-set/clear flipped?
      genRegRegCheck(cUnit, kCondCs, rlIndex.lowReg, regLen, kThrowArrayBounds);
      oatFreeTemp(cUnit, regLen);
    }
    loadPair(cUnit, regPtr, rlResult.lowReg, rlResult.highReg);

    oatFreeTemp(cUnit, regPtr);
    storeValueWide(cUnit, rlDest, rlResult);
  } else {
    rlResult = oatEvalLoc(cUnit, rlDest, regClass, true);

    if (needsRangeCheck) {
      // TODO: change kCondCS to a more meaningful name, is the sense of
      // carry-set/clear flipped?
      genRegRegCheck(cUnit, kCondCs, rlIndex.lowReg, regLen, kThrowArrayBounds);
      oatFreeTemp(cUnit, regLen);
    }
    loadBaseIndexed(cUnit, regPtr, rlIndex.lowReg, rlResult.lowReg,
                    scale, size);

    oatFreeTemp(cUnit, regPtr);
    storeValue(cUnit, rlDest, rlResult);
  }
#endif
}

/*
 * Generate array store
 *
 */
void genArrayPut(CompilationUnit* cUnit, int optFlags, OpSize size,
                 RegLocation rlArray, RegLocation rlIndex,
                 RegLocation rlSrc, int scale)
{
  RegisterClass regClass = oatRegClassBySize(size);
  int lenOffset = Array::LengthOffset().Int32Value();
  int dataOffset;

  if (size == kLong || size == kDouble) {
    dataOffset = Array::DataOffset(sizeof(int64_t)).Int32Value();
  } else {
    dataOffset = Array::DataOffset(sizeof(int32_t)).Int32Value();
  }

  rlArray = loadValue(cUnit, rlArray, kCoreReg);
  rlIndex = loadValue(cUnit, rlIndex, kCoreReg);
#if !defined(TARGET_X86)
  int regPtr;
  if (oatIsTemp(cUnit, rlArray.lowReg)) {
    oatClobber(cUnit, rlArray.lowReg);
    regPtr = rlArray.lowReg;
  } else {
    regPtr = oatAllocTemp(cUnit);
    opRegCopy(cUnit, regPtr, rlArray.lowReg);
  }
#endif

  /* null object? */
  genNullCheck(cUnit, rlArray.sRegLow, rlArray.lowReg, optFlags);

#if defined(TARGET_X86)
  if (!(optFlags & MIR_IGNORE_RANGE_CHECK)) {
    /* if (rlIndex >= [rlArray + lenOffset]) goto kThrowArrayBounds */
    genRegMemCheck(cUnit, kCondUge, rlIndex.lowReg, rlArray.lowReg,
                   lenOffset, kThrowArrayBounds);
  }
  if ((size == kLong) || (size == kDouble)) {
    rlSrc = loadValueWide(cUnit, rlSrc, regClass);
  } else {
    rlSrc = loadValue(cUnit, rlSrc, regClass);
  }
  // If the src reg can't be byte accessed, move it to a temp first.
  if ((size == kSignedByte || size == kUnsignedByte) && rlSrc.lowReg >= 4) {
    int temp = oatAllocTemp(cUnit);
    opRegCopy(cUnit, temp, rlSrc.lowReg);
    storeBaseIndexedDisp(cUnit, rlArray.lowReg, rlIndex.lowReg, scale,
                         dataOffset, temp, INVALID_REG, size,
                         INVALID_SREG);
  } else {
    storeBaseIndexedDisp(cUnit, rlArray.lowReg, rlIndex.lowReg, scale,
                         dataOffset, rlSrc.lowReg, rlSrc.highReg, size,
                         INVALID_SREG);
  }
#else
  bool needsRangeCheck = (!(optFlags & MIR_IGNORE_RANGE_CHECK));
  int regLen = INVALID_REG;
  if (needsRangeCheck) {
    regLen = oatAllocTemp(cUnit);
    //NOTE: max live temps(4) here.
    /* Get len */
    loadWordDisp(cUnit, rlArray.lowReg, lenOffset, regLen);
  }
  /* regPtr -> array data */
  opRegImm(cUnit, kOpAdd, regPtr, dataOffset);
  /* at this point, regPtr points to array, 2 live temps */
  if ((size == kLong) || (size == kDouble)) {
    //TUNING: specific wide routine that can handle fp regs
    if (scale) {
      int rNewIndex = oatAllocTemp(cUnit);
      opRegRegImm(cUnit, kOpLsl, rNewIndex, rlIndex.lowReg, scale);
      opRegReg(cUnit, kOpAdd, regPtr, rNewIndex);
      oatFreeTemp(cUnit, rNewIndex);
    } else {
      opRegReg(cUnit, kOpAdd, regPtr, rlIndex.lowReg);
    }
    rlSrc = loadValueWide(cUnit, rlSrc, regClass);

    if (needsRangeCheck) {
      genRegRegCheck(cUnit, kCondCs, rlIndex.lowReg, regLen, kThrowArrayBounds);
      oatFreeTemp(cUnit, regLen);
    }

    storeBaseDispWide(cUnit, regPtr, 0, rlSrc.lowReg, rlSrc.highReg);

    oatFreeTemp(cUnit, regPtr);
  } else {
    rlSrc = loadValue(cUnit, rlSrc, regClass);
    if (needsRangeCheck) {
      genRegRegCheck(cUnit, kCondCs, rlIndex.lowReg, regLen, kThrowArrayBounds);
      oatFreeTemp(cUnit, regLen);
    }
    storeBaseIndexed(cUnit, regPtr, rlIndex.lowReg, rlSrc.lowReg,
                     scale, size);
  }
#endif
}

void genLong3Addr(CompilationUnit* cUnit, OpKind firstOp,
                  OpKind secondOp, RegLocation rlDest,
                  RegLocation rlSrc1, RegLocation rlSrc2)
{
  RegLocation rlResult;
#if defined(TARGET_ARM)
  /*
   * NOTE:  This is the one place in the code in which we might have
   * as many as six live temporary registers.  There are 5 in the normal
   * set for Arm.  Until we have spill capabilities, temporarily add
   * lr to the temp set.  It is safe to do this locally, but note that
   * lr is used explicitly elsewhere in the code generator and cannot
   * normally be used as a general temp register.
   */
  oatMarkTemp(cUnit, rLR);   // Add lr to the temp pool
  oatFreeTemp(cUnit, rLR);   // and make it available
#endif
  rlSrc1 = loadValueWide(cUnit, rlSrc1, kCoreReg);
  rlSrc2 = loadValueWide(cUnit, rlSrc2, kCoreReg);
  rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
  // The longs may overlap - use intermediate temp if so
  if (rlResult.lowReg == rlSrc1.highReg) {
    int tReg = oatAllocTemp(cUnit);
    opRegCopy(cUnit, tReg, rlSrc1.highReg);
    opRegRegReg(cUnit, firstOp, rlResult.lowReg, rlSrc1.lowReg, rlSrc2.lowReg);
    opRegRegReg(cUnit, secondOp, rlResult.highReg, tReg, rlSrc2.highReg);
    oatFreeTemp(cUnit, tReg);
  } else {
    opRegRegReg(cUnit, firstOp, rlResult.lowReg, rlSrc1.lowReg, rlSrc2.lowReg);
    opRegRegReg(cUnit, secondOp, rlResult.highReg, rlSrc1.highReg,
                rlSrc2.highReg);
  }
  /*
   * NOTE: If rlDest refers to a frame variable in a large frame, the
   * following storeValueWide might need to allocate a temp register.
   * To further work around the lack of a spill capability, explicitly
   * free any temps from rlSrc1 & rlSrc2 that aren't still live in rlResult.
   * Remove when spill is functional.
   */
  freeRegLocTemps(cUnit, rlResult, rlSrc1);
  freeRegLocTemps(cUnit, rlResult, rlSrc2);
  storeValueWide(cUnit, rlDest, rlResult);
#if defined(TARGET_ARM)
  oatClobber(cUnit, rLR);
  oatUnmarkTemp(cUnit, rLR);  // Remove lr from the temp pool
#endif
}


bool genShiftOpLong(CompilationUnit* cUnit, Instruction::Code opcode, RegLocation rlDest,
                    RegLocation rlSrc1, RegLocation rlShift)
{
  int funcOffset;

  switch (opcode) {
    case Instruction::SHL_LONG:
    case Instruction::SHL_LONG_2ADDR:
      funcOffset = ENTRYPOINT_OFFSET(pShlLong);
      break;
    case Instruction::SHR_LONG:
    case Instruction::SHR_LONG_2ADDR:
      funcOffset = ENTRYPOINT_OFFSET(pShrLong);
      break;
    case Instruction::USHR_LONG:
    case Instruction::USHR_LONG_2ADDR:
      funcOffset = ENTRYPOINT_OFFSET(pUshrLong);
      break;
    default:
      LOG(FATAL) << "Unexpected case";
      return true;
  }
  oatFlushAllRegs(cUnit);   /* Send everything to home location */
  callRuntimeHelperRegLocationRegLocation(cUnit, funcOffset, rlSrc1, rlShift, false);
  RegLocation rlResult = oatGetReturnWide(cUnit, false);
  storeValueWide(cUnit, rlDest, rlResult);
  return false;
}


bool genArithOpInt(CompilationUnit* cUnit, Instruction::Code opcode, RegLocation rlDest,
           RegLocation rlSrc1, RegLocation rlSrc2)
{
  OpKind op = kOpBkpt;
  bool isDivRem = false;
  bool checkZero = false;
  bool unary = false;
  RegLocation rlResult;
  bool shiftOp = false;
  switch (opcode) {
    case Instruction::NEG_INT:
      op = kOpNeg;
      unary = true;
      break;
    case Instruction::NOT_INT:
      op = kOpMvn;
      unary = true;
      break;
    case Instruction::ADD_INT:
    case Instruction::ADD_INT_2ADDR:
      op = kOpAdd;
      break;
    case Instruction::SUB_INT:
    case Instruction::SUB_INT_2ADDR:
      op = kOpSub;
      break;
    case Instruction::MUL_INT:
    case Instruction::MUL_INT_2ADDR:
      op = kOpMul;
      break;
    case Instruction::DIV_INT:
    case Instruction::DIV_INT_2ADDR:
      checkZero = true;
      op = kOpDiv;
      isDivRem = true;
      break;
    /* NOTE: returns in rARG1 */
    case Instruction::REM_INT:
    case Instruction::REM_INT_2ADDR:
      checkZero = true;
      op = kOpRem;
      isDivRem = true;
      break;
    case Instruction::AND_INT:
    case Instruction::AND_INT_2ADDR:
      op = kOpAnd;
      break;
    case Instruction::OR_INT:
    case Instruction::OR_INT_2ADDR:
      op = kOpOr;
      break;
    case Instruction::XOR_INT:
    case Instruction::XOR_INT_2ADDR:
      op = kOpXor;
      break;
    case Instruction::SHL_INT:
    case Instruction::SHL_INT_2ADDR:
      shiftOp = true;
      op = kOpLsl;
      break;
    case Instruction::SHR_INT:
    case Instruction::SHR_INT_2ADDR:
      shiftOp = true;
      op = kOpAsr;
      break;
    case Instruction::USHR_INT:
    case Instruction::USHR_INT_2ADDR:
      shiftOp = true;
      op = kOpLsr;
      break;
    default:
      LOG(FATAL) << "Invalid word arith op: " <<
        (int)opcode;
  }
  if (!isDivRem) {
    if (unary) {
      rlSrc1 = loadValue(cUnit, rlSrc1, kCoreReg);
      rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
      opRegReg(cUnit, op, rlResult.lowReg, rlSrc1.lowReg);
    } else {
      if (shiftOp) {
#if !defined(TARGET_X86)
        rlSrc2 = loadValue(cUnit, rlSrc2, kCoreReg);
        int tReg = oatAllocTemp(cUnit);
        opRegRegImm(cUnit, kOpAnd, tReg, rlSrc2.lowReg, 31);
#else
        // X86 doesn't require masking and must use ECX
        loadValueDirectFixed(cUnit, rlSrc2, rCX);
        int tReg = rCX;
#endif
        rlSrc1 = loadValue(cUnit, rlSrc1, kCoreReg);
        rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
        opRegRegReg(cUnit, op, rlResult.lowReg, rlSrc1.lowReg, tReg);
        oatFreeTemp(cUnit, tReg);
      } else {
        rlSrc1 = loadValue(cUnit, rlSrc1, kCoreReg);
        rlSrc2 = loadValue(cUnit, rlSrc2, kCoreReg);
        rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
        opRegRegReg(cUnit, op, rlResult.lowReg, rlSrc1.lowReg, rlSrc2.lowReg);
      }
    }
    storeValue(cUnit, rlDest, rlResult);
  } else {
#if defined(TARGET_MIPS)
    rlSrc1 = loadValue(cUnit, rlSrc1, kCoreReg);
    rlSrc2 = loadValue(cUnit, rlSrc2, kCoreReg);
    if (checkZero) {
        genImmedCheck(cUnit, kCondEq, rlSrc2.lowReg, 0, kThrowDivZero);
    }
    newLIR4(cUnit, kMipsDiv, r_HI, r_LO, rlSrc1.lowReg, rlSrc2.lowReg);
    rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
    if (op == kOpDiv) {
      newLIR2(cUnit, kMipsMflo, rlResult.lowReg, r_LO);
    } else {
      newLIR2(cUnit, kMipsMfhi, rlResult.lowReg, r_HI);
    }
#else
    int funcOffset = ENTRYPOINT_OFFSET(pIdivmod);
    RegLocation rlResult;
    oatFlushAllRegs(cUnit);   /* Send everything to home location */
    loadValueDirectFixed(cUnit, rlSrc2, rARG1);
#if defined(TARGET_ARM)
    int rTgt = loadHelper(cUnit, funcOffset);
#endif
    loadValueDirectFixed(cUnit, rlSrc1, rARG0);
    if (checkZero) {
      genImmedCheck(cUnit, kCondEq, rARG1, 0, kThrowDivZero);
    }
    // NOTE: callout here is not a safepoint
#if defined(TARGET_ARM)
    opReg(cUnit, kOpBlx, rTgt);
    oatFreeTemp(cUnit, rTgt);
#else
    opThreadMem(cUnit, kOpBlx, funcOffset);
#endif
    if (op == kOpDiv)
      rlResult = oatGetReturn(cUnit, false);
    else
      rlResult = oatGetReturnAlt(cUnit);
#endif
    storeValue(cUnit, rlDest, rlResult);
  }
  return false;
}

/*
 * The following are the first-level codegen routines that analyze the format
 * of each bytecode then either dispatch special purpose codegen routines
 * or produce corresponding Thumb instructions directly.
 */

bool isPowerOfTwo(int x)
{
  return (x & (x - 1)) == 0;
}

// Returns true if no more than two bits are set in 'x'.
bool isPopCountLE2(unsigned int x)
{
  x &= x - 1;
  return (x & (x - 1)) == 0;
}

// Returns the index of the lowest set bit in 'x'.
int lowestSetBit(unsigned int x) {
  int bit_posn = 0;
  while ((x & 0xf) == 0) {
    bit_posn += 4;
    x >>= 4;
  }
  while ((x & 1) == 0) {
    bit_posn++;
    x >>= 1;
  }
  return bit_posn;
}

// Returns true if it added instructions to 'cUnit' to divide 'rlSrc' by 'lit'
// and store the result in 'rlDest'.
bool handleEasyDivide(CompilationUnit* cUnit, Instruction::Code dalvikOpcode,
            RegLocation rlSrc, RegLocation rlDest, int lit)
{
#if defined(TARGET_ARM)
  // No divide instruction for Arm, so check for more special cases
  if (lit < 2) {
    return false;
  }
  if (!isPowerOfTwo(lit)) {
    return smallLiteralDivide(cUnit, dalvikOpcode, rlSrc, rlDest, lit);
  }
#else
  if (lit < 2 || !isPowerOfTwo(lit)) {
    return false;
  }
#endif
  int k = lowestSetBit(lit);
  if (k >= 30) {
    // Avoid special cases.
    return false;
  }
  bool div = (dalvikOpcode == Instruction::DIV_INT_LIT8 ||
      dalvikOpcode == Instruction::DIV_INT_LIT16);
  rlSrc = loadValue(cUnit, rlSrc, kCoreReg);
  RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
  if (div) {
    int tReg = oatAllocTemp(cUnit);
    if (lit == 2) {
      // Division by 2 is by far the most common division by constant.
      opRegRegImm(cUnit, kOpLsr, tReg, rlSrc.lowReg, 32 - k);
      opRegRegReg(cUnit, kOpAdd, tReg, tReg, rlSrc.lowReg);
      opRegRegImm(cUnit, kOpAsr, rlResult.lowReg, tReg, k);
    } else {
      opRegRegImm(cUnit, kOpAsr, tReg, rlSrc.lowReg, 31);
      opRegRegImm(cUnit, kOpLsr, tReg, tReg, 32 - k);
      opRegRegReg(cUnit, kOpAdd, tReg, tReg, rlSrc.lowReg);
      opRegRegImm(cUnit, kOpAsr, rlResult.lowReg, tReg, k);
    }
  } else {
    int tReg1 = oatAllocTemp(cUnit);
    int tReg2 = oatAllocTemp(cUnit);
    if (lit == 2) {
      opRegRegImm(cUnit, kOpLsr, tReg1, rlSrc.lowReg, 32 - k);
      opRegRegReg(cUnit, kOpAdd, tReg2, tReg1, rlSrc.lowReg);
      opRegRegImm(cUnit, kOpAnd, tReg2, tReg2, lit -1);
      opRegRegReg(cUnit, kOpSub, rlResult.lowReg, tReg2, tReg1);
    } else {
      opRegRegImm(cUnit, kOpAsr, tReg1, rlSrc.lowReg, 31);
      opRegRegImm(cUnit, kOpLsr, tReg1, tReg1, 32 - k);
      opRegRegReg(cUnit, kOpAdd, tReg2, tReg1, rlSrc.lowReg);
      opRegRegImm(cUnit, kOpAnd, tReg2, tReg2, lit - 1);
      opRegRegReg(cUnit, kOpSub, rlResult.lowReg, tReg2, tReg1);
    }
  }
  storeValue(cUnit, rlDest, rlResult);
  return true;
}

void genMultiplyByTwoBitMultiplier(CompilationUnit* cUnit, RegLocation rlSrc,
                                   RegLocation rlResult, int lit,
                                   int firstBit, int secondBit)
{
#if defined(TARGET_ARM)
  opRegRegRegShift(cUnit, kOpAdd, rlResult.lowReg, rlSrc.lowReg, rlSrc.lowReg,
                   encodeShift(kArmLsl, secondBit - firstBit));
#else
  int tReg = oatAllocTemp(cUnit);
  opRegRegImm(cUnit, kOpLsl, tReg, rlSrc.lowReg, secondBit - firstBit);
  opRegRegReg(cUnit, kOpAdd, rlResult.lowReg, rlSrc.lowReg, tReg);
  oatFreeTemp(cUnit, tReg);
#endif
  if (firstBit != 0) {
    opRegRegImm(cUnit, kOpLsl, rlResult.lowReg, rlResult.lowReg, firstBit);
  }
}

// Returns true if it added instructions to 'cUnit' to multiply 'rlSrc' by 'lit'
// and store the result in 'rlDest'.
bool handleEasyMultiply(CompilationUnit* cUnit, RegLocation rlSrc,
                        RegLocation rlDest, int lit)
{
  // Can we simplify this multiplication?
  bool powerOfTwo = false;
  bool popCountLE2 = false;
  bool powerOfTwoMinusOne = false;
  if (lit < 2) {
    // Avoid special cases.
    return false;
  } else if (isPowerOfTwo(lit)) {
    powerOfTwo = true;
  } else if (isPopCountLE2(lit)) {
    popCountLE2 = true;
  } else if (isPowerOfTwo(lit + 1)) {
    powerOfTwoMinusOne = true;
  } else {
    return false;
  }
  rlSrc = loadValue(cUnit, rlSrc, kCoreReg);
  RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
  if (powerOfTwo) {
    // Shift.
    opRegRegImm(cUnit, kOpLsl, rlResult.lowReg, rlSrc.lowReg,
                lowestSetBit(lit));
  } else if (popCountLE2) {
    // Shift and add and shift.
    int firstBit = lowestSetBit(lit);
    int secondBit = lowestSetBit(lit ^ (1 << firstBit));
    genMultiplyByTwoBitMultiplier(cUnit, rlSrc, rlResult, lit,
                                  firstBit, secondBit);
  } else {
    // Reverse subtract: (src << (shift + 1)) - src.
    DCHECK(powerOfTwoMinusOne);
    // TUNING: rsb dst, src, src lsl#lowestSetBit(lit + 1)
    int tReg = oatAllocTemp(cUnit);
    opRegRegImm(cUnit, kOpLsl, tReg, rlSrc.lowReg, lowestSetBit(lit + 1));
    opRegRegReg(cUnit, kOpSub, rlResult.lowReg, tReg, rlSrc.lowReg);
  }
  storeValue(cUnit, rlDest, rlResult);
  return true;
}

bool genArithOpIntLit(CompilationUnit* cUnit, Instruction::Code opcode,
                      RegLocation rlDest, RegLocation rlSrc, int lit)
{
  RegLocation rlResult;
  OpKind op = (OpKind)0;    /* Make gcc happy */
  int shiftOp = false;
  bool isDiv = false;

  switch (opcode) {
    case Instruction::RSUB_INT_LIT8:
    case Instruction::RSUB_INT: {
      int tReg;
      //TUNING: add support for use of Arm rsub op
      rlSrc = loadValue(cUnit, rlSrc, kCoreReg);
      tReg = oatAllocTemp(cUnit);
      loadConstant(cUnit, tReg, lit);
      rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
      opRegRegReg(cUnit, kOpSub, rlResult.lowReg, tReg, rlSrc.lowReg);
      storeValue(cUnit, rlDest, rlResult);
      return false;
      break;
    }

    case Instruction::ADD_INT_LIT8:
    case Instruction::ADD_INT_LIT16:
      op = kOpAdd;
      break;
    case Instruction::MUL_INT_LIT8:
    case Instruction::MUL_INT_LIT16: {
      if (handleEasyMultiply(cUnit, rlSrc, rlDest, lit)) {
        return false;
      }
      op = kOpMul;
      break;
    }
    case Instruction::AND_INT_LIT8:
    case Instruction::AND_INT_LIT16:
      op = kOpAnd;
      break;
    case Instruction::OR_INT_LIT8:
    case Instruction::OR_INT_LIT16:
      op = kOpOr;
      break;
    case Instruction::XOR_INT_LIT8:
    case Instruction::XOR_INT_LIT16:
      op = kOpXor;
      break;
    case Instruction::SHL_INT_LIT8:
    case Instruction::SHL_INT:
      lit &= 31;
      shiftOp = true;
      op = kOpLsl;
      break;
    case Instruction::SHR_INT_LIT8:
    case Instruction::SHR_INT:
      lit &= 31;
      shiftOp = true;
      op = kOpAsr;
      break;
    case Instruction::USHR_INT_LIT8:
    case Instruction::USHR_INT:
      lit &= 31;
      shiftOp = true;
      op = kOpLsr;
      break;

    case Instruction::DIV_INT_LIT8:
    case Instruction::DIV_INT_LIT16:
    case Instruction::REM_INT_LIT8:
    case Instruction::REM_INT_LIT16: {
      if (lit == 0) {
        genImmedCheck(cUnit, kCondAl, 0, 0, kThrowDivZero);
        return false;
      }
      if (handleEasyDivide(cUnit, opcode, rlSrc, rlDest, lit)) {
        return false;
      }
      if ((opcode == Instruction::DIV_INT_LIT8) ||
          (opcode == Instruction::DIV_INT_LIT16)) {
        isDiv = true;
      } else {
        isDiv = false;
      }
#if defined(TARGET_MIPS)
      rlSrc = loadValue(cUnit, rlSrc, kCoreReg);
      int tReg = oatAllocTemp(cUnit);
      newLIR3(cUnit, kMipsAddiu, tReg, r_ZERO, lit);
      newLIR4(cUnit, kMipsDiv, r_HI, r_LO, rlSrc.lowReg, tReg);
      rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
      if (isDiv) {
        newLIR2(cUnit, kMipsMflo, rlResult.lowReg, r_LO);
      } else {
        newLIR2(cUnit, kMipsMfhi, rlResult.lowReg, r_HI);
      }
      oatFreeTemp(cUnit, tReg);
#else
      oatFlushAllRegs(cUnit);   /* Everything to home location */
      loadValueDirectFixed(cUnit, rlSrc, rARG0);
      oatClobber(cUnit, rARG0);
      int funcOffset = ENTRYPOINT_OFFSET(pIdivmod);
      callRuntimeHelperRegImm(cUnit, funcOffset, rARG0, lit, false);
      if (isDiv)
        rlResult = oatGetReturn(cUnit, false);
      else
        rlResult = oatGetReturnAlt(cUnit);
#endif
      storeValue(cUnit, rlDest, rlResult);
      return false;
      break;
    }
    default:
      return true;
  }
  rlSrc = loadValue(cUnit, rlSrc, kCoreReg);
  rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
  // Avoid shifts by literal 0 - no support in Thumb.  Change to copy
  if (shiftOp && (lit == 0)) {
    opRegCopy(cUnit, rlResult.lowReg, rlSrc.lowReg);
  } else {
    opRegRegImm(cUnit, op, rlResult.lowReg, rlSrc.lowReg, lit);
  }
  storeValue(cUnit, rlDest, rlResult);
  return false;
}

bool genArithOpLong(CompilationUnit* cUnit, Instruction::Code opcode, RegLocation rlDest,
          RegLocation rlSrc1, RegLocation rlSrc2)
{
  RegLocation rlResult;
  OpKind firstOp = kOpBkpt;
  OpKind secondOp = kOpBkpt;
  bool callOut = false;
  bool checkZero = false;
  int funcOffset;
  int retReg = rRET0;

  switch (opcode) {
    case Instruction::NOT_LONG:
      rlSrc2 = loadValueWide(cUnit, rlSrc2, kCoreReg);
      rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
      // Check for destructive overlap
      if (rlResult.lowReg == rlSrc2.highReg) {
        int tReg = oatAllocTemp(cUnit);
        opRegCopy(cUnit, tReg, rlSrc2.highReg);
        opRegReg(cUnit, kOpMvn, rlResult.lowReg, rlSrc2.lowReg);
        opRegReg(cUnit, kOpMvn, rlResult.highReg, tReg);
        oatFreeTemp(cUnit, tReg);
      } else {
        opRegReg(cUnit, kOpMvn, rlResult.lowReg, rlSrc2.lowReg);
        opRegReg(cUnit, kOpMvn, rlResult.highReg, rlSrc2.highReg);
      }
      storeValueWide(cUnit, rlDest, rlResult);
      return false;
      break;
    case Instruction::ADD_LONG:
    case Instruction::ADD_LONG_2ADDR:
#if defined(TARGET_MIPS) || defined(TARGET_X86)
      return genAddLong(cUnit, rlDest, rlSrc1, rlSrc2);
#else
      firstOp = kOpAdd;
      secondOp = kOpAdc;
      break;
#endif
    case Instruction::SUB_LONG:
    case Instruction::SUB_LONG_2ADDR:
#if defined(TARGET_MIPS) || defined(TARGET_X86)
      return genSubLong(cUnit, rlDest, rlSrc1, rlSrc2);
#else
      firstOp = kOpSub;
      secondOp = kOpSbc;
      break;
#endif
    case Instruction::MUL_LONG:
    case Instruction::MUL_LONG_2ADDR:
      callOut = true;
      retReg = rRET0;
      funcOffset = ENTRYPOINT_OFFSET(pLmul);
      break;
    case Instruction::DIV_LONG:
    case Instruction::DIV_LONG_2ADDR:
      callOut = true;
      checkZero = true;
      retReg = rRET0;
      funcOffset = ENTRYPOINT_OFFSET(pLdiv);
      break;
    case Instruction::REM_LONG:
    case Instruction::REM_LONG_2ADDR:
      callOut = true;
      checkZero = true;
      funcOffset = ENTRYPOINT_OFFSET(pLdivmod);
#if defined(TARGET_ARM)
      /* NOTE - result is in rARG2/rARG3 instead of rRET0/rRET1 */
      retReg = rARG2;
#else
      retReg = rRET0;
#endif
      break;
    case Instruction::AND_LONG_2ADDR:
    case Instruction::AND_LONG:
#if defined(TARGET_X86)
      return genAndLong(cUnit, rlDest, rlSrc1, rlSrc2);
#else
      firstOp = kOpAnd;
      secondOp = kOpAnd;
      break;
#endif
    case Instruction::OR_LONG:
    case Instruction::OR_LONG_2ADDR:
#if defined(TARGET_X86)
      return genOrLong(cUnit, rlDest, rlSrc1, rlSrc2);
#else
      firstOp = kOpOr;
      secondOp = kOpOr;
      break;
#endif
    case Instruction::XOR_LONG:
    case Instruction::XOR_LONG_2ADDR:
#if defined(TARGET_X86)
      return genXorLong(cUnit, rlDest, rlSrc1, rlSrc2);
#else
      firstOp = kOpXor;
      secondOp = kOpXor;
      break;
#endif
    case Instruction::NEG_LONG: {
      return genNegLong(cUnit, rlDest, rlSrc2);
    }
    default:
      LOG(FATAL) << "Invalid long arith op";
  }
  if (!callOut) {
    genLong3Addr(cUnit, firstOp, secondOp, rlDest, rlSrc1, rlSrc2);
  } else {
    oatFlushAllRegs(cUnit);   /* Send everything to home location */
    if (checkZero) {
      loadValueDirectWideFixed(cUnit, rlSrc2, rARG2, rARG3);
#if !defined(TARGET_X86)
      int rTgt = loadHelper(cUnit, funcOffset);
#endif
      int tReg = oatAllocTemp(cUnit);
#if defined(TARGET_ARM)
      newLIR4(cUnit, kThumb2OrrRRRs, tReg, rARG2, rARG3, 0);
      oatFreeTemp(cUnit, tReg);
      genCheck(cUnit, kCondEq, kThrowDivZero);
#else
      opRegRegReg(cUnit, kOpOr, tReg, rARG2, rARG3);
#endif
      genImmedCheck(cUnit, kCondEq, tReg, 0, kThrowDivZero);
      oatFreeTemp(cUnit, tReg);
      loadValueDirectWideFixed(cUnit, rlSrc1, rARG0, rARG1);
      // NOTE: callout here is not a safepoint
#if !defined(TARGET_X86)
      opReg(cUnit, kOpBlx, rTgt);
      oatFreeTemp(cUnit, rTgt);
#else
      opThreadMem(cUnit, kOpBlx, funcOffset);
#endif
    } else {
      callRuntimeHelperRegLocationRegLocation(cUnit, funcOffset,
                          rlSrc1, rlSrc2, false);
    }
    // Adjust return regs in to handle case of rem returning rARG2/rARG3
    if (retReg == rRET0)
      rlResult = oatGetReturnWide(cUnit, false);
    else
      rlResult = oatGetReturnWideAlt(cUnit);
    storeValueWide(cUnit, rlDest, rlResult);
  }
  return false;
}

bool genConversionCall(CompilationUnit* cUnit, int funcOffset,
                       RegLocation rlDest, RegLocation rlSrc)
{
  /*
   * Don't optimize the register usage since it calls out to support
   * functions
   */
  oatFlushAllRegs(cUnit);   /* Send everything to home location */
  if (rlSrc.wide) {
    loadValueDirectWideFixed(cUnit, rlSrc, rlSrc.fp ? rFARG0 : rARG0, rlSrc.fp ? rFARG1 : rARG1);
  } else {
    loadValueDirectFixed(cUnit, rlSrc, rlSrc.fp ? rFARG0 : rARG0);
  }
  callRuntimeHelperRegLocation(cUnit, funcOffset, rlSrc, false);
  if (rlDest.wide) {
    RegLocation rlResult;
    rlResult = oatGetReturnWide(cUnit, rlDest.fp);
    storeValueWide(cUnit, rlDest, rlResult);
  } else {
    RegLocation rlResult;
    rlResult = oatGetReturn(cUnit, rlDest.fp);
    storeValue(cUnit, rlDest, rlResult);
  }
  return false;
}

void genNegFloat(CompilationUnit* cUnit, RegLocation rlDest, RegLocation rlSrc);
bool genArithOpFloatPortable(CompilationUnit* cUnit, Instruction::Code opcode,
                             RegLocation rlDest, RegLocation rlSrc1,
                             RegLocation rlSrc2)
{
  RegLocation rlResult;
  int funcOffset;

  switch (opcode) {
    case Instruction::ADD_FLOAT_2ADDR:
    case Instruction::ADD_FLOAT:
      funcOffset = ENTRYPOINT_OFFSET(pFadd);
      break;
    case Instruction::SUB_FLOAT_2ADDR:
    case Instruction::SUB_FLOAT:
      funcOffset = ENTRYPOINT_OFFSET(pFsub);
      break;
    case Instruction::DIV_FLOAT_2ADDR:
    case Instruction::DIV_FLOAT:
      funcOffset = ENTRYPOINT_OFFSET(pFdiv);
      break;
    case Instruction::MUL_FLOAT_2ADDR:
    case Instruction::MUL_FLOAT:
      funcOffset = ENTRYPOINT_OFFSET(pFmul);
      break;
    case Instruction::REM_FLOAT_2ADDR:
    case Instruction::REM_FLOAT:
      funcOffset = ENTRYPOINT_OFFSET(pFmodf);
      break;
    case Instruction::NEG_FLOAT: {
      genNegFloat(cUnit, rlDest, rlSrc1);
      return false;
    }
    default:
      return true;
  }
  oatFlushAllRegs(cUnit);   /* Send everything to home location */
  callRuntimeHelperRegLocationRegLocation(cUnit, funcOffset, rlSrc1, rlSrc2, false);
  rlResult = oatGetReturn(cUnit, true);
  storeValue(cUnit, rlDest, rlResult);
  return false;
}

void genNegDouble(CompilationUnit* cUnit, RegLocation rlDst, RegLocation rlSrc);
bool genArithOpDoublePortable(CompilationUnit* cUnit, Instruction::Code opcode,
                              RegLocation rlDest, RegLocation rlSrc1,
                              RegLocation rlSrc2)
{
  RegLocation rlResult;
  int funcOffset;

  switch (opcode) {
    case Instruction::ADD_DOUBLE_2ADDR:
    case Instruction::ADD_DOUBLE:
      funcOffset = ENTRYPOINT_OFFSET(pDadd);
      break;
    case Instruction::SUB_DOUBLE_2ADDR:
    case Instruction::SUB_DOUBLE:
      funcOffset = ENTRYPOINT_OFFSET(pDsub);
      break;
    case Instruction::DIV_DOUBLE_2ADDR:
    case Instruction::DIV_DOUBLE:
      funcOffset = ENTRYPOINT_OFFSET(pDdiv);
      break;
    case Instruction::MUL_DOUBLE_2ADDR:
    case Instruction::MUL_DOUBLE:
      funcOffset = ENTRYPOINT_OFFSET(pDmul);
      break;
    case Instruction::REM_DOUBLE_2ADDR:
    case Instruction::REM_DOUBLE:
      funcOffset = ENTRYPOINT_OFFSET(pFmod);
      break;
    case Instruction::NEG_DOUBLE: {
      genNegDouble(cUnit, rlDest, rlSrc1);
      return false;
    }
    default:
      return true;
  }
  oatFlushAllRegs(cUnit);   /* Send everything to home location */
  callRuntimeHelperRegLocationRegLocation(cUnit, funcOffset, rlSrc1, rlSrc2, false);
  rlResult = oatGetReturnWide(cUnit, true);
  storeValueWide(cUnit, rlDest, rlResult);
  return false;
}

bool genConversionPortable(CompilationUnit* cUnit, Instruction::Code opcode,
                           RegLocation rlDest, RegLocation rlSrc)
{

  switch (opcode) {
    case Instruction::INT_TO_FLOAT:
      return genConversionCall(cUnit, ENTRYPOINT_OFFSET(pI2f),
                   rlDest, rlSrc);
    case Instruction::FLOAT_TO_INT:
      return genConversionCall(cUnit, ENTRYPOINT_OFFSET(pF2iz),
                   rlDest, rlSrc);
    case Instruction::DOUBLE_TO_FLOAT:
      return genConversionCall(cUnit, ENTRYPOINT_OFFSET(pD2f),
                   rlDest, rlSrc);
    case Instruction::FLOAT_TO_DOUBLE:
      return genConversionCall(cUnit, ENTRYPOINT_OFFSET(pF2d),
                   rlDest, rlSrc);
    case Instruction::INT_TO_DOUBLE:
      return genConversionCall(cUnit, ENTRYPOINT_OFFSET(pI2d),
                   rlDest, rlSrc);
    case Instruction::DOUBLE_TO_INT:
      return genConversionCall(cUnit, ENTRYPOINT_OFFSET(pD2iz),
                   rlDest, rlSrc);
    case Instruction::FLOAT_TO_LONG:
      return genConversionCall(cUnit, ENTRYPOINT_OFFSET(pF2l),
                   rlDest, rlSrc);
    case Instruction::LONG_TO_FLOAT:
      return genConversionCall(cUnit, ENTRYPOINT_OFFSET(pL2f),
                   rlDest, rlSrc);
    case Instruction::DOUBLE_TO_LONG:
      return genConversionCall(cUnit, ENTRYPOINT_OFFSET(pD2l),
                   rlDest, rlSrc);
    case Instruction::LONG_TO_DOUBLE:
      return genConversionCall(cUnit, ENTRYPOINT_OFFSET(pL2d),
                   rlDest, rlSrc);
    default:
      return true;
  }
  return false;
}

/*
 * Generate callout to updateDebugger. Note that we're overloading
 * the use of rSUSPEND here.  When the debugger is active, this
 * register holds the address of the update function.  So, if it's
 * non-null, we call out to it.
 *
 * Note also that rRET0 and rRET1 must be preserved across this
 * code.  This must be handled by the stub.
 */
void genDebuggerUpdate(CompilationUnit* cUnit, int32_t offset)
{
  // Following DCHECK verifies that dPC is in range of single load immediate
  DCHECK((offset == DEBUGGER_METHOD_ENTRY) ||
         (offset == DEBUGGER_METHOD_EXIT) || ((offset & 0xffff) == offset));
  oatClobberCalleeSave(cUnit);
#if defined(TARGET_ARM)
  opRegImm(cUnit, kOpCmp, rSUSPEND, 0);
  opIT(cUnit, kArmCondNe, "T");
  loadConstant(cUnit, rARG2, offset);   // arg2 <- Entry code
  LIR* callInst = opReg(cUnit, kOpBlx, rSUSPEND);
  markSafepointPC(cUnit, callInst);
#elif defined(TARGET_X86)
  UNIMPLEMENTED(FATAL);
#else
  LIR* branch = opCmpImmBranch(cUnit, kCondEq, rSUSPEND, 0, NULL);
  loadConstant(cUnit, rARG2, offset);
  LIR* callInst = opReg(cUnit, kOpBlx, rSUSPEND);
  markSafepointPC(cUnit, callInst);
  LIR* target = newLIR0(cUnit, kPseudoTargetLabel);
  branch->target = (LIR*)target;
#endif
  oatFreeTemp(cUnit, rARG2);
}

/* Check if we need to check for pending suspend request */
void genSuspendTest(CompilationUnit* cUnit, int optFlags)
{
  if (NO_SUSPEND || (optFlags & MIR_IGNORE_SUSPEND_CHECK)) {
    return;
  }
  oatFlushAllRegs(cUnit);
  if (cUnit->genDebugger) {
    // If generating code for the debugger, always check for suspension
#if defined(TARGET_X86)
    UNIMPLEMENTED(FATAL);
#else
    int rTgt = loadHelper(cUnit, ENTRYPOINT_OFFSET(pTestSuspendFromCode));
    LIR* callInst = opReg(cUnit, kOpBlx, rTgt);
    markSafepointPC(cUnit, callInst);
    // Refresh rSUSPEND
    loadWordDisp(cUnit, rSELF,
           ENTRYPOINT_OFFSET(pUpdateDebuggerFromCode),
           rSUSPEND);
#endif
  } else {
    LIR* branch = NULL;
#if defined(TARGET_ARM)
    // In non-debug case, only check periodically
    newLIR2(cUnit, kThumbSubRI8, rSUSPEND, 1);
    branch = opCondBranch(cUnit, kCondEq, NULL);
#elif defined(TARGET_X86)
    newLIR2(cUnit, kX86Cmp16TI8, Thread::ThreadFlagsOffset().Int32Value(), 0);
    branch = opCondBranch(cUnit, kCondNe, NULL);
#else
    opRegImm(cUnit, kOpSub, rSUSPEND, 1);
    branch = opCmpImmBranch(cUnit, kCondEq, rSUSPEND, 0, NULL);
#endif
    LIR* retLab = newLIR0(cUnit, kPseudoTargetLabel);
    LIR* target = rawLIR(cUnit, cUnit->currentDalvikOffset,
               kPseudoSuspendTarget, (intptr_t)retLab, cUnit->currentDalvikOffset);
    branch->target = (LIR*)target;
    oatInsertGrowableList(cUnit, &cUnit->suspendLaunchpads, (intptr_t)target);
  }
}

/* Check if we need to check for pending suspend request */
void genSuspendTestAndBranch(CompilationUnit* cUnit, int optFlags, LIR* target)
{
  if (NO_SUSPEND || (optFlags & MIR_IGNORE_SUSPEND_CHECK)) {
    opUnconditionalBranch(cUnit, target);
    return;
  }
  if (cUnit->genDebugger) {
    genSuspendTest(cUnit, optFlags);
    opUnconditionalBranch(cUnit, target);
  } else {
#if defined(TARGET_ARM)
    // In non-debug case, only check periodically
    newLIR2(cUnit, kThumbSubRI8, rSUSPEND, 1);
    opCondBranch(cUnit, kCondNe, target);
#elif defined(TARGET_X86)
    newLIR2(cUnit, kX86Cmp16TI8, Thread::ThreadFlagsOffset().Int32Value(), 0);
    opCondBranch(cUnit, kCondEq, target);
#else
    opRegImm(cUnit, kOpSub, rSUSPEND, 1);
    opCmpImmBranch(cUnit, kCondNe, rSUSPEND, 0, target);
#endif
    LIR* launchPad = rawLIR(cUnit, cUnit->currentDalvikOffset,
                kPseudoSuspendTarget, (intptr_t)target, cUnit->currentDalvikOffset);
    oatFlushAllRegs(cUnit);
    opUnconditionalBranch(cUnit, launchPad);
    oatInsertGrowableList(cUnit, &cUnit->suspendLaunchpads,
                          (intptr_t)launchPad);
  }
}

}  // namespace art
