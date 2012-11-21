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
#include "../compiler_ir.h"
#include "ralloc_util.h"
#include "codegen_util.h"

namespace art {

//TODO: remove decl.
void GenInvoke(CompilationUnit* cUnit, CallInfo* info);

/*
 * This source files contains "gen" codegen routines that should
 * be applicable to most targets.  Only mid-level support utilities
 * and "op" calls may be used here.
 */

void MarkSafepointPC(CompilationUnit* cUnit, LIR* inst)
{
  inst->defMask = ENCODE_ALL;
  LIR* safepointPC = NewLIR0(cUnit, kPseudoSafepointPC);
  DCHECK_EQ(safepointPC->defMask, ENCODE_ALL);
}

/*
 * To save scheduling time, helper calls are broken into two parts: generation of
 * the helper target address, and the actuall call to the helper.  Because x86
 * has a memory call operation, part 1 is a NOP for x86.  For other targets,
 * load arguments between the two parts.
 */
int CallHelperSetup(CompilationUnit* cUnit, int helperOffset)
{
  return (cUnit->instructionSet == kX86) ? 0 : LoadHelper(cUnit, helperOffset);
}

/* NOTE: if rTgt is a temp, it will be freed following use */
LIR* CallHelper(CompilationUnit* cUnit, int rTgt, int helperOffset, bool safepointPC)
{
  LIR* callInst;
  if (cUnit->instructionSet == kX86) {
    callInst = OpThreadMem(cUnit, kOpBlx, helperOffset);
  } else {
    callInst = OpReg(cUnit, kOpBlx, rTgt);
    FreeTemp(cUnit, rTgt);
  }
  if (safepointPC) {
    MarkSafepointPC(cUnit, callInst);
  }
  return callInst;
}

void CallRuntimeHelperImm(CompilationUnit* cUnit, int helperOffset, int arg0, bool safepointPC) {
  int rTgt = CallHelperSetup(cUnit, helperOffset);
  LoadConstant(cUnit, TargetReg(kArg0), arg0);
  ClobberCalleeSave(cUnit);
  CallHelper(cUnit, rTgt, helperOffset, safepointPC);
}

void CallRuntimeHelperReg(CompilationUnit* cUnit, int helperOffset, int arg0, bool safepointPC) {
  int rTgt = CallHelperSetup(cUnit, helperOffset);
  OpRegCopy(cUnit, TargetReg(kArg0), arg0);
  ClobberCalleeSave(cUnit);
  CallHelper(cUnit, rTgt, helperOffset, safepointPC);
}

void CallRuntimeHelperRegLocation(CompilationUnit* cUnit, int helperOffset, RegLocation arg0,
                                  bool safepointPC) {
  int rTgt = CallHelperSetup(cUnit, helperOffset);
  if (arg0.wide == 0) {
    LoadValueDirectFixed(cUnit, arg0, TargetReg(kArg0));
  } else {
    LoadValueDirectWideFixed(cUnit, arg0, TargetReg(kArg0), TargetReg(kArg1));
  }
  ClobberCalleeSave(cUnit);
  CallHelper(cUnit, rTgt, helperOffset, safepointPC);
}

void CallRuntimeHelperImmImm(CompilationUnit* cUnit, int helperOffset, int arg0, int arg1,
                             bool safepointPC) {
  int rTgt = CallHelperSetup(cUnit, helperOffset);
  LoadConstant(cUnit, TargetReg(kArg0), arg0);
  LoadConstant(cUnit, TargetReg(kArg1), arg1);
  ClobberCalleeSave(cUnit);
  CallHelper(cUnit, rTgt, helperOffset, safepointPC);
}

void CallRuntimeHelperImmRegLocation(CompilationUnit* cUnit, int helperOffset, int arg0,
                                     RegLocation arg1, bool safepointPC) {
  int rTgt = CallHelperSetup(cUnit, helperOffset);
  if (arg1.wide == 0) {
    LoadValueDirectFixed(cUnit, arg1, TargetReg(kArg1));
  } else {
    LoadValueDirectWideFixed(cUnit, arg1, TargetReg(kArg1), TargetReg(kArg2));
  }
  LoadConstant(cUnit, TargetReg(kArg0), arg0);
  ClobberCalleeSave(cUnit);
  CallHelper(cUnit, rTgt, helperOffset, safepointPC);
}

void CallRuntimeHelperRegLocationImm(CompilationUnit* cUnit, int helperOffset, RegLocation arg0,
                                     int arg1, bool safepointPC) {
  int rTgt = CallHelperSetup(cUnit, helperOffset);
  LoadValueDirectFixed(cUnit, arg0, TargetReg(kArg0));
  LoadConstant(cUnit, TargetReg(kArg1), arg1);
  ClobberCalleeSave(cUnit);
  CallHelper(cUnit, rTgt, helperOffset, safepointPC);
}

void CallRuntimeHelperImmReg(CompilationUnit* cUnit, int helperOffset, int arg0, int arg1,
                             bool safepointPC) {
  int rTgt = CallHelperSetup(cUnit, helperOffset);
  OpRegCopy(cUnit, TargetReg(kArg1), arg1);
  LoadConstant(cUnit, TargetReg(kArg0), arg0);
  ClobberCalleeSave(cUnit);
  CallHelper(cUnit, rTgt, helperOffset, safepointPC);
}

void CallRuntimeHelperRegImm(CompilationUnit* cUnit, int helperOffset, int arg0, int arg1,
                             bool safepointPC) {
  int rTgt = CallHelperSetup(cUnit, helperOffset);
  OpRegCopy(cUnit, TargetReg(kArg0), arg0);
  LoadConstant(cUnit, TargetReg(kArg1), arg1);
  ClobberCalleeSave(cUnit);
  CallHelper(cUnit, rTgt, helperOffset, safepointPC);
}

void CallRuntimeHelperImmMethod(CompilationUnit* cUnit, int helperOffset, int arg0, bool safepointPC) {
  int rTgt = CallHelperSetup(cUnit, helperOffset);
  LoadCurrMethodDirect(cUnit, TargetReg(kArg1));
  LoadConstant(cUnit, TargetReg(kArg0), arg0);
  ClobberCalleeSave(cUnit);
  CallHelper(cUnit, rTgt, helperOffset, safepointPC);
}

void CallRuntimeHelperRegLocationRegLocation(CompilationUnit* cUnit, int helperOffset,
                                             RegLocation arg0, RegLocation arg1, bool safepointPC) {
  int rTgt = CallHelperSetup(cUnit, helperOffset);
  if (arg0.wide == 0) {
    LoadValueDirectFixed(cUnit, arg0, arg0.fp ? TargetReg(kFArg0) : TargetReg(kArg0));
    if (arg1.wide == 0) {
      if (cUnit->instructionSet == kMips) {
        LoadValueDirectFixed(cUnit, arg1, arg1.fp ? TargetReg(kFArg2) : TargetReg(kArg1));
      } else {
        LoadValueDirectFixed(cUnit, arg1, TargetReg(kArg1));
      }
    } else {
      if (cUnit->instructionSet == kMips) {
        LoadValueDirectWideFixed(cUnit, arg1, arg1.fp ? TargetReg(kFArg2) : TargetReg(kArg1), arg1.fp ? TargetReg(kFArg3) : TargetReg(kArg2));
      } else {
        LoadValueDirectWideFixed(cUnit, arg1, TargetReg(kArg1), TargetReg(kArg2));
      }
    }
  } else {
    LoadValueDirectWideFixed(cUnit, arg0, arg0.fp ? TargetReg(kFArg0) : TargetReg(kArg0), arg0.fp ? TargetReg(kFArg1) : TargetReg(kArg1));
    if (arg1.wide == 0) {
      LoadValueDirectFixed(cUnit, arg1, arg1.fp ? TargetReg(kFArg2) : TargetReg(kArg2));
    } else {
      LoadValueDirectWideFixed(cUnit, arg1, arg1.fp ? TargetReg(kFArg2) : TargetReg(kArg2), arg1.fp ? TargetReg(kFArg3) : TargetReg(kArg3));
    }
  }
  ClobberCalleeSave(cUnit);
  CallHelper(cUnit, rTgt, helperOffset, safepointPC);
}

void CallRuntimeHelperRegReg(CompilationUnit* cUnit, int helperOffset, int arg0, int arg1,
                             bool safepointPC) {
  int rTgt = CallHelperSetup(cUnit, helperOffset);
  DCHECK_NE(TargetReg(kArg0), arg1);  // check copy into arg0 won't clobber arg1
  OpRegCopy(cUnit, TargetReg(kArg0), arg0);
  OpRegCopy(cUnit, TargetReg(kArg1), arg1);
  ClobberCalleeSave(cUnit);
  CallHelper(cUnit, rTgt, helperOffset, safepointPC);
}

void CallRuntimeHelperRegRegImm(CompilationUnit* cUnit, int helperOffset, int arg0, int arg1,
                                int arg2, bool safepointPC) {
  int rTgt = CallHelperSetup(cUnit, helperOffset);
  DCHECK_NE(TargetReg(kArg0), arg1);  // check copy into arg0 won't clobber arg1
  OpRegCopy(cUnit, TargetReg(kArg0), arg0);
  OpRegCopy(cUnit, TargetReg(kArg1), arg1);
  LoadConstant(cUnit, TargetReg(kArg2), arg2);
  ClobberCalleeSave(cUnit);
  CallHelper(cUnit, rTgt, helperOffset, safepointPC);
}

void CallRuntimeHelperImmMethodRegLocation(CompilationUnit* cUnit, int helperOffset, int arg0,
                                           RegLocation arg2, bool safepointPC) {
  int rTgt = CallHelperSetup(cUnit, helperOffset);
  LoadValueDirectFixed(cUnit, arg2, TargetReg(kArg2));
  LoadCurrMethodDirect(cUnit, TargetReg(kArg1));
  LoadConstant(cUnit, TargetReg(kArg0), arg0);
  ClobberCalleeSave(cUnit);
  CallHelper(cUnit, rTgt, helperOffset, safepointPC);
}

void CallRuntimeHelperImmMethodImm(CompilationUnit* cUnit, int helperOffset, int arg0, int arg2,
                                   bool safepointPC) {
  int rTgt = CallHelperSetup(cUnit, helperOffset);
  LoadCurrMethodDirect(cUnit, TargetReg(kArg1));
  LoadConstant(cUnit, TargetReg(kArg2), arg2);
  LoadConstant(cUnit, TargetReg(kArg0), arg0);
  ClobberCalleeSave(cUnit);
  CallHelper(cUnit, rTgt, helperOffset, safepointPC);
}

void CallRuntimeHelperImmRegLocationRegLocation(CompilationUnit* cUnit, int helperOffset,
                                                int arg0, RegLocation arg1, RegLocation arg2,
                                                bool safepointPC) {
  int rTgt = CallHelperSetup(cUnit, helperOffset);
  LoadValueDirectFixed(cUnit, arg1, TargetReg(kArg1));
  if (arg2.wide == 0) {
    LoadValueDirectFixed(cUnit, arg2, TargetReg(kArg2));
  } else {
    LoadValueDirectWideFixed(cUnit, arg2, TargetReg(kArg2), TargetReg(kArg3));
  }
  LoadConstant(cUnit, TargetReg(kArg0), arg0);
  ClobberCalleeSave(cUnit);
  CallHelper(cUnit, rTgt, helperOffset, safepointPC);
}

/*
 * Generate an kPseudoBarrier marker to indicate the boundary of special
 * blocks.
 */
void GenBarrier(CompilationUnit* cUnit)
{
  LIR* barrier = NewLIR0(cUnit, kPseudoBarrier);
  /* Mark all resources as being clobbered */
  barrier->defMask = -1;
}


/* Generate unconditional branch instructions */
LIR* OpUnconditionalBranch(CompilationUnit* cUnit, LIR* target)
{
  LIR* branch = OpBranchUnconditional(cUnit, kOpUncondBr);
  branch->target = target;
  return branch;
}

// FIXME: need to do some work to split out targets with
// condition codes and those without
LIR* GenCheck(CompilationUnit* cUnit, ConditionCode cCode,
              ThrowKind kind)
{
  DCHECK_NE(cUnit->instructionSet, kMips);
  LIR* tgt = RawLIR(cUnit, 0, kPseudoThrowTarget, kind,
                    cUnit->currentDalvikOffset);
  LIR* branch = OpCondBranch(cUnit, cCode, tgt);
  // Remember branch target - will process later
  InsertGrowableList(cUnit, &cUnit->throwLaunchpads, reinterpret_cast<uintptr_t>(tgt));
  return branch;
}

LIR* GenImmedCheck(CompilationUnit* cUnit, ConditionCode cCode,
                   int reg, int immVal, ThrowKind kind)
{
  LIR* tgt = RawLIR(cUnit, 0, kPseudoThrowTarget, kind,
                    cUnit->currentDalvikOffset);
  LIR* branch;
  if (cCode == kCondAl) {
    branch = OpUnconditionalBranch(cUnit, tgt);
  } else {
    branch = OpCmpImmBranch(cUnit, cCode, reg, immVal, tgt);
  }
  // Remember branch target - will process later
  InsertGrowableList(cUnit, &cUnit->throwLaunchpads, reinterpret_cast<uintptr_t>(tgt));
  return branch;
}

/* Perform null-check on a register.  */
LIR* GenNullCheck(CompilationUnit* cUnit, int sReg, int mReg, int optFlags)
{
  if (!(cUnit->disableOpt & (1 << kNullCheckElimination)) &&
    optFlags & MIR_IGNORE_NULL_CHECK) {
    return NULL;
  }
  return GenImmedCheck(cUnit, kCondEq, mReg, 0, kThrowNullPointer);
}

/* Perform check on two registers */
LIR* GenRegRegCheck(CompilationUnit* cUnit, ConditionCode cCode,
                    int reg1, int reg2, ThrowKind kind)
{
  LIR* tgt = RawLIR(cUnit, 0, kPseudoThrowTarget, kind,
                    cUnit->currentDalvikOffset, reg1, reg2);
  LIR* branch = OpCmpBranch(cUnit, cCode, reg1, reg2, tgt);
  // Remember branch target - will process later
  InsertGrowableList(cUnit, &cUnit->throwLaunchpads, reinterpret_cast<uintptr_t>(tgt));
  return branch;
}

void GenCompareAndBranch(CompilationUnit* cUnit, Instruction::Code opcode,
                         RegLocation rlSrc1, RegLocation rlSrc2, LIR* taken,
                         LIR* fallThrough)
{
  ConditionCode cond;
  rlSrc1 = LoadValue(cUnit, rlSrc1, kCoreReg);
  rlSrc2 = LoadValue(cUnit, rlSrc2, kCoreReg);
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
      cond = static_cast<ConditionCode>(0);
      LOG(FATAL) << "Unexpected opcode " << opcode;
  }
  OpCmpBranch(cUnit, cond, rlSrc1.lowReg, rlSrc2.lowReg, taken);
  OpUnconditionalBranch(cUnit, fallThrough);
}

void GenCompareZeroAndBranch(CompilationUnit* cUnit, Instruction::Code opcode,
                             RegLocation rlSrc, LIR* taken, LIR* fallThrough)
{
  ConditionCode cond;
  rlSrc = LoadValue(cUnit, rlSrc, kCoreReg);
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
      cond = static_cast<ConditionCode>(0);
      LOG(FATAL) << "Unexpected opcode " << opcode;
  }
  if (cUnit->instructionSet == kThumb2) {
    OpRegImm(cUnit, kOpCmp, rlSrc.lowReg, 0);
    OpCondBranch(cUnit, cond, taken);
  } else {
    OpCmpImmBranch(cUnit, cond, rlSrc.lowReg, 0, taken);
  }
  OpUnconditionalBranch(cUnit, fallThrough);
}

void GenIntToLong(CompilationUnit* cUnit, RegLocation rlDest,
                  RegLocation rlSrc)
{
  RegLocation rlResult = EvalLoc(cUnit, rlDest, kCoreReg, true);
  if (rlSrc.location == kLocPhysReg) {
    OpRegCopy(cUnit, rlResult.lowReg, rlSrc.lowReg);
  } else {
    LoadValueDirect(cUnit, rlSrc, rlResult.lowReg);
  }
  OpRegRegImm(cUnit, kOpAsr, rlResult.highReg, rlResult.lowReg, 31);
  StoreValueWide(cUnit, rlDest, rlResult);
}

void GenIntNarrowing(CompilationUnit* cUnit, Instruction::Code opcode,
                     RegLocation rlDest, RegLocation rlSrc)
{
   rlSrc = LoadValue(cUnit, rlSrc, kCoreReg);
   RegLocation rlResult = EvalLoc(cUnit, rlDest, kCoreReg, true);
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
   OpRegReg(cUnit, op, rlResult.lowReg, rlSrc.lowReg);
   StoreValue(cUnit, rlDest, rlResult);
}

/*
 * Let helper function take care of everything.  Will call
 * Array::AllocFromCode(type_idx, method, count);
 * Note: AllocFromCode will handle checks for errNegativeArraySize.
 */
void GenNewArray(CompilationUnit* cUnit, uint32_t type_idx, RegLocation rlDest,
                 RegLocation rlSrc)
{
  FlushAllRegs(cUnit);  /* Everything to home location */
  int funcOffset;
  if (cUnit->compiler->CanAccessTypeWithoutChecks(cUnit->method_idx,
                                                  *cUnit->dex_file,
                                                  type_idx)) {
    funcOffset = ENTRYPOINT_OFFSET(pAllocArrayFromCode);
  } else {
    funcOffset= ENTRYPOINT_OFFSET(pAllocArrayFromCodeWithAccessCheck);
  }
  CallRuntimeHelperImmMethodRegLocation(cUnit, funcOffset, type_idx, rlSrc, true);
  RegLocation rlResult = GetReturn(cUnit, false);
  StoreValue(cUnit, rlDest, rlResult);
}

/*
 * Similar to GenNewArray, but with post-allocation initialization.
 * Verifier guarantees we're dealing with an array class.  Current
 * code throws runtime exception "bad Filled array req" for 'D' and 'J'.
 * Current code also throws internal unimp if not 'L', '[' or 'I'.
 */
void GenFilledNewArray(CompilationUnit* cUnit, CallInfo* info)
{
  int elems = info->numArgWords;
  int typeIdx = info->index;
  FlushAllRegs(cUnit);  /* Everything to home location */
  int funcOffset;
  if (cUnit->compiler->CanAccessTypeWithoutChecks(cUnit->method_idx,
                                                  *cUnit->dex_file,
                                                  typeIdx)) {
    funcOffset = ENTRYPOINT_OFFSET(pCheckAndAllocArrayFromCode);
  } else {
    funcOffset = ENTRYPOINT_OFFSET(pCheckAndAllocArrayFromCodeWithAccessCheck);
  }
  CallRuntimeHelperImmMethodImm(cUnit, funcOffset, typeIdx, elems, true);
  FreeTemp(cUnit, TargetReg(kArg2));
  FreeTemp(cUnit, TargetReg(kArg1));
  /*
   * NOTE: the implicit target for Instruction::FILLED_NEW_ARRAY is the
   * return region.  Because AllocFromCode placed the new array
   * in kRet0, we'll just lock it into place.  When debugger support is
   * added, it may be necessary to additionally copy all return
   * values to a home location in thread-local storage
   */
  LockTemp(cUnit, TargetReg(kRet0));

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
      RegLocation loc = UpdateLoc(cUnit, info->args[i]);
      if (loc.location == kLocPhysReg) {
        StoreBaseDisp(cUnit, TargetReg(kSp), SRegOffset(cUnit, loc.sRegLow),
                      loc.lowReg, kWord);
      }
    }
    /*
     * TUNING note: generated code here could be much improved, but
     * this is an uncommon operation and isn't especially performance
     * critical.
     */
    int rSrc = AllocTemp(cUnit);
    int rDst = AllocTemp(cUnit);
    int rIdx = AllocTemp(cUnit);
    int rVal = INVALID_REG;
    switch(cUnit->instructionSet) {
      case kThumb2:
        rVal = TargetReg(kLr);
        break;
      case kX86:
        FreeTemp(cUnit, TargetReg(kRet0));
        rVal = AllocTemp(cUnit);
        break;
      case kMips:
        rVal = AllocTemp(cUnit);
        break;
      default: LOG(FATAL) << "Unexpected instruction set: " << cUnit->instructionSet;
    }
    // Set up source pointer
    RegLocation rlFirst = info->args[0];
    OpRegRegImm(cUnit, kOpAdd, rSrc, TargetReg(kSp),
                SRegOffset(cUnit, rlFirst.sRegLow));
    // Set up the target pointer
    OpRegRegImm(cUnit, kOpAdd, rDst, TargetReg(kRet0),
                Array::DataOffset(component_size).Int32Value());
    // Set up the loop counter (known to be > 0)
    LoadConstant(cUnit, rIdx, elems - 1);
    // Generate the copy loop.  Going backwards for convenience
    LIR* target = NewLIR0(cUnit, kPseudoTargetLabel);
    // Copy next element
    LoadBaseIndexed(cUnit, rSrc, rIdx, rVal, 2, kWord);
    StoreBaseIndexed(cUnit, rDst, rIdx, rVal, 2, kWord);
    FreeTemp(cUnit, rVal);
    OpDecAndBranch(cUnit, kCondGe, rIdx, target);
    if (cUnit->instructionSet == kX86) {
      // Restore the target pointer
      OpRegRegImm(cUnit, kOpAdd, TargetReg(kRet0), rDst, -Array::DataOffset(component_size).Int32Value());
    }
  } else if (!info->isRange) {
    // TUNING: interleave
    for (int i = 0; i < elems; i++) {
      RegLocation rlArg = LoadValue(cUnit, info->args[i], kCoreReg);
      StoreBaseDisp(cUnit, TargetReg(kRet0),
                    Array::DataOffset(component_size).Int32Value() +
                    i * 4, rlArg.lowReg, kWord);
      // If the LoadValue caused a temp to be allocated, free it
      if (IsTemp(cUnit, rlArg.lowReg)) {
        FreeTemp(cUnit, rlArg.lowReg);
      }
    }
  }
  if (info->result.location != kLocInvalid) {
    StoreValue(cUnit, info->result, GetReturn(cUnit, false /* not fp */));
  }
}

void GenSput(CompilationUnit* cUnit, uint32_t fieldIdx, RegLocation rlSrc,
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
      RegLocation rlMethod  = LoadCurrMethod(cUnit);
      rBase = AllocTemp(cUnit);
      LoadWordDisp(cUnit, rlMethod.lowReg,
                   AbstractMethod::DeclaringClassOffset().Int32Value(), rBase);
      if (IsTemp(cUnit, rlMethod.lowReg)) {
        FreeTemp(cUnit, rlMethod.lowReg);
      }
    } else {
      // Medium path, static storage base in a different class which
      // requires checks that the other class is initialized.
      DCHECK_GE(ssbIndex, 0);
      // May do runtime call so everything to home locations.
      FlushAllRegs(cUnit);
      // Using fixed register to sync with possible call to runtime
      // support.
      int rMethod = TargetReg(kArg1);
      LockTemp(cUnit, rMethod);
      LoadCurrMethodDirect(cUnit, rMethod);
      rBase = TargetReg(kArg0);
      LockTemp(cUnit, rBase);
      LoadWordDisp(cUnit, rMethod,
                   AbstractMethod::DexCacheInitializedStaticStorageOffset().Int32Value(),
                   rBase);
      LoadWordDisp(cUnit, rBase,
                   Array::DataOffset(sizeof(Object*)).Int32Value() +
                   sizeof(int32_t*) * ssbIndex, rBase);
      // rBase now points at appropriate static storage base (Class*)
      // or NULL if not initialized. Check for NULL and call helper if NULL.
      // TUNING: fast path should fall through
      LIR* branchOver = OpCmpImmBranch(cUnit, kCondNe, rBase, 0, NULL);
      LoadConstant(cUnit, TargetReg(kArg0), ssbIndex);
      CallRuntimeHelperImm(cUnit, ENTRYPOINT_OFFSET(pInitializeStaticStorage), ssbIndex, true);
      if (cUnit->instructionSet == kMips) {
        // For Arm, kRet0 = kArg0 = rBase, for Mips, we need to copy
        OpRegCopy(cUnit, rBase, TargetReg(kRet0));
      }
      LIR* skipTarget = NewLIR0(cUnit, kPseudoTargetLabel);
      branchOver->target = skipTarget;
      FreeTemp(cUnit, rMethod);
    }
    // rBase now holds static storage base
    if (isLongOrDouble) {
      rlSrc = LoadValueWide(cUnit, rlSrc, kAnyReg);
    } else {
      rlSrc = LoadValue(cUnit, rlSrc, kAnyReg);
    }
    if (isVolatile) {
      GenMemBarrier(cUnit, kStoreStore);
    }
    if (isLongOrDouble) {
      StoreBaseDispWide(cUnit, rBase, fieldOffset, rlSrc.lowReg,
                        rlSrc.highReg);
    } else {
      StoreWordDisp(cUnit, rBase, fieldOffset, rlSrc.lowReg);
    }
    if (isVolatile) {
      GenMemBarrier(cUnit, kStoreLoad);
    }
    if (isObject) {
      MarkGCCard(cUnit, rlSrc.lowReg, rBase);
    }
    FreeTemp(cUnit, rBase);
  } else {
    FlushAllRegs(cUnit);  // Everything to home locations
    int setterOffset = isLongOrDouble ? ENTRYPOINT_OFFSET(pSet64Static) :
        (isObject ? ENTRYPOINT_OFFSET(pSetObjStatic)
        : ENTRYPOINT_OFFSET(pSet32Static));
    CallRuntimeHelperImmRegLocation(cUnit, setterOffset, fieldIdx, rlSrc, true);
  }
}

void GenSget(CompilationUnit* cUnit, uint32_t fieldIdx, RegLocation rlDest,
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
      RegLocation rlMethod  = LoadCurrMethod(cUnit);
      rBase = AllocTemp(cUnit);
      LoadWordDisp(cUnit, rlMethod.lowReg,
                   AbstractMethod::DeclaringClassOffset().Int32Value(), rBase);
    } else {
      // Medium path, static storage base in a different class which
      // requires checks that the other class is initialized
      DCHECK_GE(ssbIndex, 0);
      // May do runtime call so everything to home locations.
      FlushAllRegs(cUnit);
      // Using fixed register to sync with possible call to runtime
      // support
      int rMethod = TargetReg(kArg1);
      LockTemp(cUnit, rMethod);
      LoadCurrMethodDirect(cUnit, rMethod);
      rBase = TargetReg(kArg0);
      LockTemp(cUnit, rBase);
      LoadWordDisp(cUnit, rMethod,
                   AbstractMethod::DexCacheInitializedStaticStorageOffset().Int32Value(),
                   rBase);
      LoadWordDisp(cUnit, rBase,
                   Array::DataOffset(sizeof(Object*)).Int32Value() +
                   sizeof(int32_t*) * ssbIndex, rBase);
      // rBase now points at appropriate static storage base (Class*)
      // or NULL if not initialized. Check for NULL and call helper if NULL.
      // TUNING: fast path should fall through
      LIR* branchOver = OpCmpImmBranch(cUnit, kCondNe, rBase, 0, NULL);
      CallRuntimeHelperImm(cUnit, ENTRYPOINT_OFFSET(pInitializeStaticStorage), ssbIndex, true);
      if (cUnit->instructionSet == kMips) {
        // For Arm, kRet0 = kArg0 = rBase, for Mips, we need to copy
        OpRegCopy(cUnit, rBase, TargetReg(kRet0));
      }
      LIR* skipTarget = NewLIR0(cUnit, kPseudoTargetLabel);
      branchOver->target = skipTarget;
      FreeTemp(cUnit, rMethod);
    }
    // rBase now holds static storage base
    RegLocation rlResult = EvalLoc(cUnit, rlDest, kAnyReg, true);
    if (isVolatile) {
      GenMemBarrier(cUnit, kLoadLoad);
    }
    if (isLongOrDouble) {
      LoadBaseDispWide(cUnit, rBase, fieldOffset, rlResult.lowReg,
                       rlResult.highReg, INVALID_SREG);
    } else {
      LoadWordDisp(cUnit, rBase, fieldOffset, rlResult.lowReg);
    }
    FreeTemp(cUnit, rBase);
    if (isLongOrDouble) {
      StoreValueWide(cUnit, rlDest, rlResult);
    } else {
      StoreValue(cUnit, rlDest, rlResult);
    }
  } else {
    FlushAllRegs(cUnit);  // Everything to home locations
    int getterOffset = isLongOrDouble ? ENTRYPOINT_OFFSET(pGet64Static) :
        (isObject ? ENTRYPOINT_OFFSET(pGetObjStatic)
        : ENTRYPOINT_OFFSET(pGet32Static));
    CallRuntimeHelperImm(cUnit, getterOffset, fieldIdx, true);
    if (isLongOrDouble) {
      RegLocation rlResult = GetReturnWide(cUnit, rlDest.fp);
      StoreValueWide(cUnit, rlDest, rlResult);
    } else {
      RegLocation rlResult = GetReturn(cUnit, rlDest.fp);
      StoreValue(cUnit, rlDest, rlResult);
    }
  }
}


// Debugging routine - if null target, branch to DebugMe
void GenShowTarget(CompilationUnit* cUnit)
{
  DCHECK_NE(cUnit->instructionSet, kX86) << "unimplemented GenShowTarget";
  LIR* branchOver = OpCmpImmBranch(cUnit, kCondNe, TargetReg(kInvokeTgt), 0, NULL);
  LoadWordDisp(cUnit, TargetReg(kSelf), ENTRYPOINT_OFFSET(pDebugMe), TargetReg(kInvokeTgt));
  LIR* target = NewLIR0(cUnit, kPseudoTargetLabel);
  branchOver->target = target;
}

void HandleSuspendLaunchPads(CompilationUnit *cUnit)
{
  LIR** suspendLabel = reinterpret_cast<LIR**>(cUnit->suspendLaunchpads.elemList);
  int numElems = cUnit->suspendLaunchpads.numUsed;
  int helperOffset = ENTRYPOINT_OFFSET(pTestSuspendFromCode);
  for (int i = 0; i < numElems; i++) {
    ResetRegPool(cUnit);
    ResetDefTracking(cUnit);
    LIR* lab = suspendLabel[i];
    LIR* resumeLab = reinterpret_cast<LIR*>(lab->operands[0]);
    cUnit->currentDalvikOffset = lab->operands[1];
    AppendLIR(cUnit, lab);
    int rTgt = CallHelperSetup(cUnit, helperOffset);
    CallHelper(cUnit, rTgt, helperOffset, true /* MarkSafepointPC */);
    OpUnconditionalBranch(cUnit, resumeLab);
  }
}

void HandleIntrinsicLaunchPads(CompilationUnit *cUnit)
{
  LIR** intrinsicLabel = reinterpret_cast<LIR**>(cUnit->intrinsicLaunchpads.elemList);
  int numElems = cUnit->intrinsicLaunchpads.numUsed;
  for (int i = 0; i < numElems; i++) {
    ResetRegPool(cUnit);
    ResetDefTracking(cUnit);
    LIR* lab = intrinsicLabel[i];
    CallInfo* info = reinterpret_cast<CallInfo*>(lab->operands[0]);
    cUnit->currentDalvikOffset = info->offset;
    AppendLIR(cUnit, lab);
    // NOTE: GenInvoke handles MarkSafepointPC
    GenInvoke(cUnit, info);
    LIR* resumeLab = reinterpret_cast<LIR*>(lab->operands[2]);
    if (resumeLab != NULL) {
      OpUnconditionalBranch(cUnit, resumeLab);
    }
  }
}

void HandleThrowLaunchPads(CompilationUnit *cUnit)
{
  LIR** throwLabel = reinterpret_cast<LIR**>(cUnit->throwLaunchpads.elemList);
  int numElems = cUnit->throwLaunchpads.numUsed;
  for (int i = 0; i < numElems; i++) {
    ResetRegPool(cUnit);
    ResetDefTracking(cUnit);
    LIR* lab = throwLabel[i];
    cUnit->currentDalvikOffset = lab->operands[1];
    AppendLIR(cUnit, lab);
    int funcOffset = 0;
    int v1 = lab->operands[2];
    int v2 = lab->operands[3];
    bool targetX86 = (cUnit->instructionSet == kX86);
    switch (lab->operands[0]) {
      case kThrowNullPointer:
        funcOffset = ENTRYPOINT_OFFSET(pThrowNullPointerFromCode);
        break;
      case kThrowArrayBounds:
        // Move v1 (array index) to kArg0 and v2 (array length) to kArg1
        if (v2 != TargetReg(kArg0)) {
          OpRegCopy(cUnit, TargetReg(kArg0), v1);
          if (targetX86) {
            // x86 leaves the array pointer in v2, so load the array length that the handler expects
            OpRegMem(cUnit, kOpMov, TargetReg(kArg1), v2, Array::LengthOffset().Int32Value());
          } else {
            OpRegCopy(cUnit, TargetReg(kArg1), v2);
          }
        } else {
          if (v1 == TargetReg(kArg1)) {
            // Swap v1 and v2, using kArg2 as a temp
            OpRegCopy(cUnit, TargetReg(kArg2), v1);
            if (targetX86) {
              // x86 leaves the array pointer in v2; load the array length that the handler expects
              OpRegMem(cUnit, kOpMov, TargetReg(kArg1), v2, Array::LengthOffset().Int32Value());
            } else {
              OpRegCopy(cUnit, TargetReg(kArg1), v2);
            }
            OpRegCopy(cUnit, TargetReg(kArg0), TargetReg(kArg2));
          } else {
            if (targetX86) {
              // x86 leaves the array pointer in v2; load the array length that the handler expects
              OpRegMem(cUnit, kOpMov, TargetReg(kArg1), v2, Array::LengthOffset().Int32Value());
            } else {
              OpRegCopy(cUnit, TargetReg(kArg1), v2);
            }
            OpRegCopy(cUnit, TargetReg(kArg0), v1);
          }
        }
        funcOffset = ENTRYPOINT_OFFSET(pThrowArrayBoundsFromCode);
        break;
      case kThrowDivZero:
        funcOffset = ENTRYPOINT_OFFSET(pThrowDivZeroFromCode);
        break;
      case kThrowNoSuchMethod:
        OpRegCopy(cUnit, TargetReg(kArg0), v1);
        funcOffset =
          ENTRYPOINT_OFFSET(pThrowNoSuchMethodFromCode);
        break;
      case kThrowStackOverflow:
        funcOffset = ENTRYPOINT_OFFSET(pThrowStackOverflowFromCode);
        // Restore stack alignment
        if (targetX86) {
          OpRegImm(cUnit, kOpAdd, TargetReg(kSp), cUnit->frameSize);
        } else {
          OpRegImm(cUnit, kOpAdd, TargetReg(kSp), (cUnit->numCoreSpills + cUnit->numFPSpills) * 4);
        }
        break;
      default:
        LOG(FATAL) << "Unexpected throw kind: " << lab->operands[0];
    }
    ClobberCalleeSave(cUnit);
    int rTgt = CallHelperSetup(cUnit, funcOffset);
    CallHelper(cUnit, rTgt, funcOffset, true /* MarkSafepointPC */);
  }
}

bool FastInstance(CompilationUnit* cUnit,  uint32_t fieldIdx,
                  int& fieldOffset, bool& isVolatile, bool isPut)
{
  OatCompilationUnit mUnit(cUnit->class_loader, cUnit->class_linker,
               *cUnit->dex_file,
               cUnit->code_item, cUnit->method_idx,
               cUnit->access_flags);
  return cUnit->compiler->ComputeInstanceFieldInfo(fieldIdx, &mUnit,
           fieldOffset, isVolatile, isPut);
}

void GenIGet(CompilationUnit* cUnit, uint32_t fieldIdx, int optFlags, OpSize size,
             RegLocation rlDest, RegLocation rlObj,
             bool isLongOrDouble, bool isObject)
{
  int fieldOffset;
  bool isVolatile;

  bool fastPath = FastInstance(cUnit, fieldIdx, fieldOffset, isVolatile, false);

  if (fastPath && !SLOW_FIELD_PATH) {
    RegLocation rlResult;
    RegisterClass regClass = oatRegClassBySize(size);
    DCHECK_GE(fieldOffset, 0);
    rlObj = LoadValue(cUnit, rlObj, kCoreReg);
    if (isLongOrDouble) {
      DCHECK(rlDest.wide);
      GenNullCheck(cUnit, rlObj.sRegLow, rlObj.lowReg, optFlags);
      if (cUnit->instructionSet == kX86) {
        rlResult = EvalLoc(cUnit, rlDest, regClass, true);
        GenNullCheck(cUnit, rlObj.sRegLow, rlObj.lowReg, optFlags);
        LoadBaseDispWide(cUnit, rlObj.lowReg, fieldOffset, rlResult.lowReg,
                         rlResult.highReg, rlObj.sRegLow);
        if (isVolatile) {
          GenMemBarrier(cUnit, kLoadLoad);
        }
      } else {
        int regPtr = AllocTemp(cUnit);
        OpRegRegImm(cUnit, kOpAdd, regPtr, rlObj.lowReg, fieldOffset);
        rlResult = EvalLoc(cUnit, rlDest, regClass, true);
        LoadPair(cUnit, regPtr, rlResult.lowReg, rlResult.highReg);
        if (isVolatile) {
          GenMemBarrier(cUnit, kLoadLoad);
        }
        FreeTemp(cUnit, regPtr);
      }
      StoreValueWide(cUnit, rlDest, rlResult);
    } else {
      rlResult = EvalLoc(cUnit, rlDest, regClass, true);
      GenNullCheck(cUnit, rlObj.sRegLow, rlObj.lowReg, optFlags);
      LoadBaseDisp(cUnit, rlObj.lowReg, fieldOffset, rlResult.lowReg,
                   kWord, rlObj.sRegLow);
      if (isVolatile) {
        GenMemBarrier(cUnit, kLoadLoad);
      }
      StoreValue(cUnit, rlDest, rlResult);
    }
  } else {
    int getterOffset = isLongOrDouble ? ENTRYPOINT_OFFSET(pGet64Instance) :
        (isObject ? ENTRYPOINT_OFFSET(pGetObjInstance)
        : ENTRYPOINT_OFFSET(pGet32Instance));
    CallRuntimeHelperImmRegLocation(cUnit, getterOffset, fieldIdx, rlObj, true);
    if (isLongOrDouble) {
      RegLocation rlResult = GetReturnWide(cUnit, rlDest.fp);
      StoreValueWide(cUnit, rlDest, rlResult);
    } else {
      RegLocation rlResult = GetReturn(cUnit, rlDest.fp);
      StoreValue(cUnit, rlDest, rlResult);
    }
  }
}

void GenIPut(CompilationUnit* cUnit, uint32_t fieldIdx, int optFlags, OpSize size,
             RegLocation rlSrc, RegLocation rlObj, bool isLongOrDouble, bool isObject)
{
  int fieldOffset;
  bool isVolatile;

  bool fastPath = FastInstance(cUnit, fieldIdx, fieldOffset, isVolatile,
                 true);
  if (fastPath && !SLOW_FIELD_PATH) {
    RegisterClass regClass = oatRegClassBySize(size);
    DCHECK_GE(fieldOffset, 0);
    rlObj = LoadValue(cUnit, rlObj, kCoreReg);
    if (isLongOrDouble) {
      int regPtr;
      rlSrc = LoadValueWide(cUnit, rlSrc, kAnyReg);
      GenNullCheck(cUnit, rlObj.sRegLow, rlObj.lowReg, optFlags);
      regPtr = AllocTemp(cUnit);
      OpRegRegImm(cUnit, kOpAdd, regPtr, rlObj.lowReg, fieldOffset);
      if (isVolatile) {
        GenMemBarrier(cUnit, kStoreStore);
      }
      StoreBaseDispWide(cUnit, regPtr, 0, rlSrc.lowReg, rlSrc.highReg);
      if (isVolatile) {
        GenMemBarrier(cUnit, kLoadLoad);
      }
      FreeTemp(cUnit, regPtr);
    } else {
      rlSrc = LoadValue(cUnit, rlSrc, regClass);
      GenNullCheck(cUnit, rlObj.sRegLow, rlObj.lowReg, optFlags);
      if (isVolatile) {
        GenMemBarrier(cUnit, kStoreStore);
      }
      StoreBaseDisp(cUnit, rlObj.lowReg, fieldOffset, rlSrc.lowReg, kWord);
      if (isVolatile) {
        GenMemBarrier(cUnit, kLoadLoad);
      }
      if (isObject) {
        MarkGCCard(cUnit, rlSrc.lowReg, rlObj.lowReg);
      }
    }
  } else {
    int setterOffset = isLongOrDouble ? ENTRYPOINT_OFFSET(pSet64Instance) :
        (isObject ? ENTRYPOINT_OFFSET(pSetObjInstance)
        : ENTRYPOINT_OFFSET(pSet32Instance));
    CallRuntimeHelperImmRegLocationRegLocation(cUnit, setterOffset, fieldIdx, rlObj, rlSrc, true);
  }
}

void GenConstClass(CompilationUnit* cUnit, uint32_t type_idx,
                   RegLocation rlDest)
{
  RegLocation rlMethod = LoadCurrMethod(cUnit);
  int resReg = AllocTemp(cUnit);
  RegLocation rlResult = EvalLoc(cUnit, rlDest, kCoreReg, true);
  if (!cUnit->compiler->CanAccessTypeWithoutChecks(cUnit->method_idx,
                                                   *cUnit->dex_file,
                                                   type_idx)) {
    // Call out to helper which resolves type and verifies access.
    // Resolved type returned in kRet0.
    CallRuntimeHelperImmReg(cUnit, ENTRYPOINT_OFFSET(pInitializeTypeAndVerifyAccessFromCode),
                            type_idx, rlMethod.lowReg, true);
    RegLocation rlResult = GetReturn(cUnit, false);
    StoreValue(cUnit, rlDest, rlResult);
  } else {
    // We're don't need access checks, load type from dex cache
    int32_t dex_cache_offset =
        AbstractMethod::DexCacheResolvedTypesOffset().Int32Value();
    LoadWordDisp(cUnit, rlMethod.lowReg, dex_cache_offset, resReg);
    int32_t offset_of_type =
        Array::DataOffset(sizeof(Class*)).Int32Value() + (sizeof(Class*)
                          * type_idx);
    LoadWordDisp(cUnit, resReg, offset_of_type, rlResult.lowReg);
    if (!cUnit->compiler->CanAssumeTypeIsPresentInDexCache(*cUnit->dex_file,
        type_idx) || SLOW_TYPE_PATH) {
      // Slow path, at runtime test if type is null and if so initialize
      FlushAllRegs(cUnit);
      LIR* branch1 = OpCmpImmBranch(cUnit, kCondEq, rlResult.lowReg, 0, NULL);
      // Resolved, store and hop over following code
      StoreValue(cUnit, rlDest, rlResult);
      /*
       * Because we have stores of the target value on two paths,
       * clobber temp tracking for the destination using the ssa name
       */
      ClobberSReg(cUnit, rlDest.sRegLow);
      LIR* branch2 = OpUnconditionalBranch(cUnit,0);
      // TUNING: move slow path to end & remove unconditional branch
      LIR* target1 = NewLIR0(cUnit, kPseudoTargetLabel);
      // Call out to helper, which will return resolved type in kArg0
      CallRuntimeHelperImmReg(cUnit, ENTRYPOINT_OFFSET(pInitializeTypeFromCode), type_idx,
                              rlMethod.lowReg, true);
      RegLocation rlResult = GetReturn(cUnit, false);
      StoreValue(cUnit, rlDest, rlResult);
      /*
       * Because we have stores of the target value on two paths,
       * clobber temp tracking for the destination using the ssa name
       */
      ClobberSReg(cUnit, rlDest.sRegLow);
      // Rejoin code paths
      LIR* target2 = NewLIR0(cUnit, kPseudoTargetLabel);
      branch1->target = target1;
      branch2->target = target2;
    } else {
      // Fast path, we're done - just store result
      StoreValue(cUnit, rlDest, rlResult);
    }
  }
}

void GenConstString(CompilationUnit* cUnit, uint32_t string_idx,
                    RegLocation rlDest)
{
  /* NOTE: Most strings should be available at compile time */
  int32_t offset_of_string = Array::DataOffset(sizeof(String*)).Int32Value() +
                 (sizeof(String*) * string_idx);
  if (!cUnit->compiler->CanAssumeStringIsPresentInDexCache(
      *cUnit->dex_file, string_idx) || SLOW_STRING_PATH) {
    // slow path, resolve string if not in dex cache
    FlushAllRegs(cUnit);
    LockCallTemps(cUnit); // Using explicit registers
    LoadCurrMethodDirect(cUnit, TargetReg(kArg2));
    LoadWordDisp(cUnit, TargetReg(kArg2),
                 AbstractMethod::DexCacheStringsOffset().Int32Value(), TargetReg(kArg0));
    // Might call out to helper, which will return resolved string in kRet0
    int rTgt = CallHelperSetup(cUnit, ENTRYPOINT_OFFSET(pResolveStringFromCode));
    LoadWordDisp(cUnit, TargetReg(kArg0), offset_of_string, TargetReg(kRet0));
    LoadConstant(cUnit, TargetReg(kArg1), string_idx);
    if (cUnit->instructionSet == kThumb2) {
      OpRegImm(cUnit, kOpCmp, TargetReg(kRet0), 0);  // Is resolved?
      GenBarrier(cUnit);
      // For testing, always force through helper
      if (!EXERCISE_SLOWEST_STRING_PATH) {
        OpIT(cUnit, kArmCondEq, "T");
      }
      OpRegCopy(cUnit, TargetReg(kArg0), TargetReg(kArg2));   // .eq
      LIR* callInst = OpReg(cUnit, kOpBlx, rTgt);    // .eq, helper(Method*, string_idx)
      MarkSafepointPC(cUnit, callInst);
      FreeTemp(cUnit, rTgt);
    } else if (cUnit->instructionSet == kMips) {
      LIR* branch = OpCmpImmBranch(cUnit, kCondNe, TargetReg(kRet0), 0, NULL);
      OpRegCopy(cUnit, TargetReg(kArg0), TargetReg(kArg2));   // .eq
      LIR* callInst = OpReg(cUnit, kOpBlx, rTgt);
      MarkSafepointPC(cUnit, callInst);
      FreeTemp(cUnit, rTgt);
      LIR* target = NewLIR0(cUnit, kPseudoTargetLabel);
      branch->target = target;
    } else {
      DCHECK_EQ(cUnit->instructionSet, kX86);
      CallRuntimeHelperRegReg(cUnit, ENTRYPOINT_OFFSET(pResolveStringFromCode), TargetReg(kArg2), TargetReg(kArg1), true);
    }
    GenBarrier(cUnit);
    StoreValue(cUnit, rlDest, GetReturn(cUnit, false));
  } else {
    RegLocation rlMethod = LoadCurrMethod(cUnit);
    int resReg = AllocTemp(cUnit);
    RegLocation rlResult = EvalLoc(cUnit, rlDest, kCoreReg, true);
    LoadWordDisp(cUnit, rlMethod.lowReg,
                 AbstractMethod::DexCacheStringsOffset().Int32Value(), resReg);
    LoadWordDisp(cUnit, resReg, offset_of_string, rlResult.lowReg);
    StoreValue(cUnit, rlDest, rlResult);
  }
}

/*
 * Let helper function take care of everything.  Will
 * call Class::NewInstanceFromCode(type_idx, method);
 */
void GenNewInstance(CompilationUnit* cUnit, uint32_t type_idx, RegLocation rlDest)
{
  FlushAllRegs(cUnit);  /* Everything to home location */
  // alloc will always check for resolution, do we also need to verify
  // access because the verifier was unable to?
  int funcOffset;
  if (cUnit->compiler->CanAccessInstantiableTypeWithoutChecks(
      cUnit->method_idx, *cUnit->dex_file, type_idx)) {
    funcOffset = ENTRYPOINT_OFFSET(pAllocObjectFromCode);
  } else {
    funcOffset = ENTRYPOINT_OFFSET(pAllocObjectFromCodeWithAccessCheck);
  }
  CallRuntimeHelperImmMethod(cUnit, funcOffset, type_idx, true);
  RegLocation rlResult = GetReturn(cUnit, false);
  StoreValue(cUnit, rlDest, rlResult);
}

void GenMoveException(CompilationUnit* cUnit, RegLocation rlDest)
{
  FlushAllRegs(cUnit);  /* Everything to home location */
  int funcOffset = ENTRYPOINT_OFFSET(pGetAndClearException);
  if (cUnit->instructionSet == kX86) {
    // Runtime helper will load argument for x86.
    CallRuntimeHelperReg(cUnit, funcOffset, TargetReg(kArg0), false);
  } else {
    CallRuntimeHelperReg(cUnit, funcOffset, TargetReg(kSelf), false);
  }
  RegLocation rlResult = GetReturn(cUnit, false);
  StoreValue(cUnit, rlDest, rlResult);
}

void GenThrow(CompilationUnit* cUnit, RegLocation rlSrc)
{
  FlushAllRegs(cUnit);
  CallRuntimeHelperRegLocation(cUnit, ENTRYPOINT_OFFSET(pDeliverException), rlSrc, true);
}

void GenInstanceof(CompilationUnit* cUnit, uint32_t type_idx, RegLocation rlDest,
                   RegLocation rlSrc)
{
  FlushAllRegs(cUnit);
  // May generate a call - use explicit registers
  LockCallTemps(cUnit);
  LoadCurrMethodDirect(cUnit, TargetReg(kArg1));  // kArg1 <= current Method*
  int classReg = TargetReg(kArg2);  // kArg2 will hold the Class*
  if (!cUnit->compiler->CanAccessTypeWithoutChecks(cUnit->method_idx,
                                                   *cUnit->dex_file,
                                                   type_idx)) {
    // Check we have access to type_idx and if not throw IllegalAccessError,
    // returns Class* in kArg0
    CallRuntimeHelperImm(cUnit, ENTRYPOINT_OFFSET(pInitializeTypeAndVerifyAccessFromCode),
                         type_idx, true);
    OpRegCopy(cUnit, classReg, TargetReg(kRet0));  // Align usage with fast path
    LoadValueDirectFixed(cUnit, rlSrc, TargetReg(kArg0));  // kArg0 <= ref
  } else {
    // Load dex cache entry into classReg (kArg2)
    LoadValueDirectFixed(cUnit, rlSrc, TargetReg(kArg0));  // kArg0 <= ref
    LoadWordDisp(cUnit, TargetReg(kArg1),
                 AbstractMethod::DexCacheResolvedTypesOffset().Int32Value(), classReg);
    int32_t offset_of_type =
        Array::DataOffset(sizeof(Class*)).Int32Value() + (sizeof(Class*)
        * type_idx);
    LoadWordDisp(cUnit, classReg, offset_of_type, classReg);
    if (!cUnit->compiler->CanAssumeTypeIsPresentInDexCache(
        *cUnit->dex_file, type_idx)) {
      // Need to test presence of type in dex cache at runtime
      LIR* hopBranch = OpCmpImmBranch(cUnit, kCondNe, classReg, 0, NULL);
      // Not resolved
      // Call out to helper, which will return resolved type in kRet0
      CallRuntimeHelperImm(cUnit, ENTRYPOINT_OFFSET(pInitializeTypeFromCode), type_idx, true);
      OpRegCopy(cUnit, TargetReg(kArg2), TargetReg(kRet0)); // Align usage with fast path
      LoadValueDirectFixed(cUnit, rlSrc, TargetReg(kArg0));  /* reload Ref */
      // Rejoin code paths
      LIR* hopTarget = NewLIR0(cUnit, kPseudoTargetLabel);
      hopBranch->target = hopTarget;
    }
  }
  /* kArg0 is ref, kArg2 is class. If ref==null, use directly as bool result */
  RegLocation rlResult = GetReturn(cUnit, false);
  if (cUnit->instructionSet == kMips) {
    LoadConstant(cUnit, rlResult.lowReg, 0);  // store false result for if branch is taken
  }
  LIR* branch1 = OpCmpImmBranch(cUnit, kCondEq, TargetReg(kArg0), 0, NULL);
  /* load object->klass_ */
  DCHECK_EQ(Object::ClassOffset().Int32Value(), 0);
  LoadWordDisp(cUnit, TargetReg(kArg0),  Object::ClassOffset().Int32Value(), TargetReg(kArg1));
  /* kArg0 is ref, kArg1 is ref->klass_, kArg2 is class */
  LIR* callInst;
  LIR* branchover = NULL;
  if (cUnit->instructionSet == kThumb2) {
    /* Uses conditional nullification */
    int rTgt = LoadHelper(cUnit, ENTRYPOINT_OFFSET(pInstanceofNonTrivialFromCode));
    OpRegReg(cUnit, kOpCmp, TargetReg(kArg1), TargetReg(kArg2));  // Same?
    OpIT(cUnit, kArmCondEq, "EE");   // if-convert the test
    LoadConstant(cUnit, TargetReg(kArg0), 1);     // .eq case - load true
    OpRegCopy(cUnit, TargetReg(kArg0), TargetReg(kArg2));    // .ne case - arg0 <= class
    callInst = OpReg(cUnit, kOpBlx, rTgt);    // .ne case: helper(class, ref->class)
    FreeTemp(cUnit, rTgt);
  } else {
    /* Uses branchovers */
    LoadConstant(cUnit, rlResult.lowReg, 1);     // assume true
    branchover = OpCmpBranch(cUnit, kCondEq, TargetReg(kArg1), TargetReg(kArg2), NULL);
    if (cUnit->instructionSet != kX86) {
      int rTgt = LoadHelper(cUnit, ENTRYPOINT_OFFSET(pInstanceofNonTrivialFromCode));
      OpRegCopy(cUnit, TargetReg(kArg0), TargetReg(kArg2));    // .ne case - arg0 <= class
      callInst = OpReg(cUnit, kOpBlx, rTgt);    // .ne case: helper(class, ref->class)
      FreeTemp(cUnit, rTgt);
    } else {
      OpRegCopy(cUnit, TargetReg(kArg0), TargetReg(kArg2));
      callInst = OpThreadMem(cUnit, kOpBlx, ENTRYPOINT_OFFSET(pInstanceofNonTrivialFromCode));
    }
  }
  MarkSafepointPC(cUnit, callInst);
  ClobberCalleeSave(cUnit);
  /* branch targets here */
  LIR* target = NewLIR0(cUnit, kPseudoTargetLabel);
  StoreValue(cUnit, rlDest, rlResult);
  branch1->target = target;
  if (cUnit->instructionSet != kThumb2) {
    branchover->target = target;
  }
}

void GenCheckCast(CompilationUnit* cUnit, uint32_t type_idx, RegLocation rlSrc)
{
  FlushAllRegs(cUnit);
  // May generate a call - use explicit registers
  LockCallTemps(cUnit);
  LoadCurrMethodDirect(cUnit, TargetReg(kArg1));  // kArg1 <= current Method*
  int classReg = TargetReg(kArg2);  // kArg2 will hold the Class*
  if (!cUnit->compiler->CanAccessTypeWithoutChecks(cUnit->method_idx,
                                                   *cUnit->dex_file,
                                                   type_idx)) {
    // Check we have access to type_idx and if not throw IllegalAccessError,
    // returns Class* in kRet0
    // InitializeTypeAndVerifyAccess(idx, method)
    CallRuntimeHelperImmReg(cUnit, ENTRYPOINT_OFFSET(pInitializeTypeAndVerifyAccessFromCode),
                            type_idx, TargetReg(kArg1), true);
    OpRegCopy(cUnit, classReg, TargetReg(kRet0));  // Align usage with fast path
  } else {
    // Load dex cache entry into classReg (kArg2)
    LoadWordDisp(cUnit, TargetReg(kArg1),
                 AbstractMethod::DexCacheResolvedTypesOffset().Int32Value(), classReg);
    int32_t offset_of_type =
        Array::DataOffset(sizeof(Class*)).Int32Value() +
        (sizeof(Class*) * type_idx);
    LoadWordDisp(cUnit, classReg, offset_of_type, classReg);
    if (!cUnit->compiler->CanAssumeTypeIsPresentInDexCache(
        *cUnit->dex_file, type_idx)) {
      // Need to test presence of type in dex cache at runtime
      LIR* hopBranch = OpCmpImmBranch(cUnit, kCondNe, classReg, 0, NULL);
      // Not resolved
      // Call out to helper, which will return resolved type in kArg0
      // InitializeTypeFromCode(idx, method)
      CallRuntimeHelperImmReg(cUnit, ENTRYPOINT_OFFSET(pInitializeTypeFromCode), type_idx, TargetReg(kArg1),
                              true);
      OpRegCopy(cUnit, classReg, TargetReg(kRet0)); // Align usage with fast path
      // Rejoin code paths
      LIR* hopTarget = NewLIR0(cUnit, kPseudoTargetLabel);
      hopBranch->target = hopTarget;
    }
  }
  // At this point, classReg (kArg2) has class
  LoadValueDirectFixed(cUnit, rlSrc, TargetReg(kArg0));  // kArg0 <= ref
  /* Null is OK - continue */
  LIR* branch1 = OpCmpImmBranch(cUnit, kCondEq, TargetReg(kArg0), 0, NULL);
  /* load object->klass_ */
  DCHECK_EQ(Object::ClassOffset().Int32Value(), 0);
  LoadWordDisp(cUnit, TargetReg(kArg0),  Object::ClassOffset().Int32Value(), TargetReg(kArg1));
  /* kArg1 now contains object->klass_ */
  LIR* branch2;
  if (cUnit->instructionSet == kThumb2) {
    int rTgt = LoadHelper(cUnit, ENTRYPOINT_OFFSET(pCheckCastFromCode));
    OpRegReg(cUnit, kOpCmp, TargetReg(kArg1), classReg);
    branch2 = OpCondBranch(cUnit, kCondEq, NULL); /* If eq, trivial yes */
    OpRegCopy(cUnit, TargetReg(kArg0), TargetReg(kArg1));
    OpRegCopy(cUnit, TargetReg(kArg1), TargetReg(kArg2));
    ClobberCalleeSave(cUnit);
    LIR* callInst = OpReg(cUnit, kOpBlx, rTgt);
    MarkSafepointPC(cUnit, callInst);
    FreeTemp(cUnit, rTgt);
  } else {
    branch2 = OpCmpBranch(cUnit, kCondEq, TargetReg(kArg1), classReg, NULL);
    CallRuntimeHelperRegReg(cUnit, ENTRYPOINT_OFFSET(pCheckCastFromCode), TargetReg(kArg1), TargetReg(kArg2), true);
  }
  /* branch target here */
  LIR* target = NewLIR0(cUnit, kPseudoTargetLabel);
  branch1->target = target;
  branch2->target = target;
}

/*
 * Generate array store
 *
 */
void GenArrayObjPut(CompilationUnit* cUnit, int optFlags, RegLocation rlArray,
          RegLocation rlIndex, RegLocation rlSrc, int scale)
{
  int lenOffset = Array::LengthOffset().Int32Value();
  int dataOffset = Array::DataOffset(sizeof(Object*)).Int32Value();

  FlushAllRegs(cUnit);  // Use explicit registers
  LockCallTemps(cUnit);

  int rValue = TargetReg(kArg0);  // Register holding value
  int rArrayClass = TargetReg(kArg1);  // Register holding array's Class
  int rArray = TargetReg(kArg2);  // Register holding array
  int rIndex = TargetReg(kArg3);  // Register holding index into array

  LoadValueDirectFixed(cUnit, rlArray, rArray);  // Grab array
  LoadValueDirectFixed(cUnit, rlSrc, rValue);  // Grab value
  LoadValueDirectFixed(cUnit, rlIndex, rIndex);  // Grab index

  GenNullCheck(cUnit, rlArray.sRegLow, rArray, optFlags);  // NPE?

  // Store of null?
  LIR* null_value_check = OpCmpImmBranch(cUnit, kCondEq, rValue, 0, NULL);

  // Get the array's class.
  LoadWordDisp(cUnit, rArray, Object::ClassOffset().Int32Value(), rArrayClass);
  CallRuntimeHelperRegReg(cUnit, ENTRYPOINT_OFFSET(pCanPutArrayElementFromCode), rValue,
                          rArrayClass, true);
  // Redo LoadValues in case they didn't survive the call.
  LoadValueDirectFixed(cUnit, rlArray, rArray);  // Reload array
  LoadValueDirectFixed(cUnit, rlIndex, rIndex);  // Reload index
  LoadValueDirectFixed(cUnit, rlSrc, rValue);  // Reload value
  rArrayClass = INVALID_REG;

  // Branch here if value to be stored == null
  LIR* target = NewLIR0(cUnit, kPseudoTargetLabel);
  null_value_check->target = target;

  if (cUnit->instructionSet == kX86) {
    // make an extra temp available for card mark below
    FreeTemp(cUnit, TargetReg(kArg1));
    if (!(optFlags & MIR_IGNORE_RANGE_CHECK)) {
      /* if (rlIndex >= [rlArray + lenOffset]) goto kThrowArrayBounds */
      GenRegMemCheck(cUnit, kCondUge, rIndex, rArray, lenOffset, kThrowArrayBounds);
    }
    StoreBaseIndexedDisp(cUnit, rArray, rIndex, scale,
                         dataOffset, rValue, INVALID_REG, kWord, INVALID_SREG);
  } else {
    bool needsRangeCheck = (!(optFlags & MIR_IGNORE_RANGE_CHECK));
    int regLen = INVALID_REG;
    if (needsRangeCheck) {
      regLen = TargetReg(kArg1);
      LoadWordDisp(cUnit, rArray, lenOffset, regLen);  // Get len
    }
    /* rPtr -> array data */
    int rPtr = AllocTemp(cUnit);
    OpRegRegImm(cUnit, kOpAdd, rPtr, rArray, dataOffset);
    if (needsRangeCheck) {
      GenRegRegCheck(cUnit, kCondCs, rIndex, regLen, kThrowArrayBounds);
    }
    StoreBaseIndexed(cUnit, rPtr, rIndex, rValue, scale, kWord);
    FreeTemp(cUnit, rPtr);
  }
  FreeTemp(cUnit, rIndex);
  MarkGCCard(cUnit, rValue, rArray);
}

/*
 * Generate array load
 */
void GenArrayGet(CompilationUnit* cUnit, int optFlags, OpSize size,
                 RegLocation rlArray, RegLocation rlIndex,
                 RegLocation rlDest, int scale)
{
  RegisterClass regClass = oatRegClassBySize(size);
  int lenOffset = Array::LengthOffset().Int32Value();
  int dataOffset;
  RegLocation rlResult;
  rlArray = LoadValue(cUnit, rlArray, kCoreReg);
  rlIndex = LoadValue(cUnit, rlIndex, kCoreReg);

  if (size == kLong || size == kDouble) {
    dataOffset = Array::DataOffset(sizeof(int64_t)).Int32Value();
  } else {
    dataOffset = Array::DataOffset(sizeof(int32_t)).Int32Value();
  }

  /* null object? */
  GenNullCheck(cUnit, rlArray.sRegLow, rlArray.lowReg, optFlags);

  if (cUnit->instructionSet == kX86) {
    if (!(optFlags & MIR_IGNORE_RANGE_CHECK)) {
      /* if (rlIndex >= [rlArray + lenOffset]) goto kThrowArrayBounds */
      GenRegMemCheck(cUnit, kCondUge, rlIndex.lowReg, rlArray.lowReg,
                     lenOffset, kThrowArrayBounds);
    }
    if ((size == kLong) || (size == kDouble)) {
      int regAddr = AllocTemp(cUnit);
      OpLea(cUnit, regAddr, rlArray.lowReg, rlIndex.lowReg, scale, dataOffset);
      FreeTemp(cUnit, rlArray.lowReg);
      FreeTemp(cUnit, rlIndex.lowReg);
      rlResult = EvalLoc(cUnit, rlDest, regClass, true);
      LoadBaseIndexedDisp(cUnit, regAddr, INVALID_REG, 0, 0, rlResult.lowReg,
                          rlResult.highReg, size, INVALID_SREG);
      StoreValueWide(cUnit, rlDest, rlResult);
    } else {
      rlResult = EvalLoc(cUnit, rlDest, regClass, true);

      LoadBaseIndexedDisp(cUnit, rlArray.lowReg, rlIndex.lowReg, scale,
                          dataOffset, rlResult.lowReg, INVALID_REG, size,
                          INVALID_SREG);

      StoreValue(cUnit, rlDest, rlResult);
    }
  } else {
    int regPtr = AllocTemp(cUnit);
    bool needsRangeCheck = (!(optFlags & MIR_IGNORE_RANGE_CHECK));
    int regLen = INVALID_REG;
    if (needsRangeCheck) {
      regLen = AllocTemp(cUnit);
      /* Get len */
      LoadWordDisp(cUnit, rlArray.lowReg, lenOffset, regLen);
    }
    /* regPtr -> array data */
    OpRegRegImm(cUnit, kOpAdd, regPtr, rlArray.lowReg, dataOffset);
    FreeTemp(cUnit, rlArray.lowReg);
    if ((size == kLong) || (size == kDouble)) {
      if (scale) {
        int rNewIndex = AllocTemp(cUnit);
        OpRegRegImm(cUnit, kOpLsl, rNewIndex, rlIndex.lowReg, scale);
        OpRegReg(cUnit, kOpAdd, regPtr, rNewIndex);
        FreeTemp(cUnit, rNewIndex);
      } else {
        OpRegReg(cUnit, kOpAdd, regPtr, rlIndex.lowReg);
      }
      FreeTemp(cUnit, rlIndex.lowReg);
      rlResult = EvalLoc(cUnit, rlDest, regClass, true);

      if (needsRangeCheck) {
        // TODO: change kCondCS to a more meaningful name, is the sense of
        // carry-set/clear flipped?
        GenRegRegCheck(cUnit, kCondCs, rlIndex.lowReg, regLen, kThrowArrayBounds);
        FreeTemp(cUnit, regLen);
      }
      LoadPair(cUnit, regPtr, rlResult.lowReg, rlResult.highReg);

      FreeTemp(cUnit, regPtr);
      StoreValueWide(cUnit, rlDest, rlResult);
    } else {
      rlResult = EvalLoc(cUnit, rlDest, regClass, true);

      if (needsRangeCheck) {
        // TODO: change kCondCS to a more meaningful name, is the sense of
        // carry-set/clear flipped?
        GenRegRegCheck(cUnit, kCondCs, rlIndex.lowReg, regLen, kThrowArrayBounds);
        FreeTemp(cUnit, regLen);
      }
      LoadBaseIndexed(cUnit, regPtr, rlIndex.lowReg, rlResult.lowReg, scale, size);

      FreeTemp(cUnit, regPtr);
      StoreValue(cUnit, rlDest, rlResult);
    }
  }
}

/*
 * Generate array store
 *
 */
void GenArrayPut(CompilationUnit* cUnit, int optFlags, OpSize size,
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

  rlArray = LoadValue(cUnit, rlArray, kCoreReg);
  rlIndex = LoadValue(cUnit, rlIndex, kCoreReg);
  int regPtr = INVALID_REG;
  if (cUnit->instructionSet != kX86) {
    if (IsTemp(cUnit, rlArray.lowReg)) {
      Clobber(cUnit, rlArray.lowReg);
      regPtr = rlArray.lowReg;
    } else {
      regPtr = AllocTemp(cUnit);
      OpRegCopy(cUnit, regPtr, rlArray.lowReg);
    }
  }

  /* null object? */
  GenNullCheck(cUnit, rlArray.sRegLow, rlArray.lowReg, optFlags);

  if (cUnit->instructionSet == kX86) {
    if (!(optFlags & MIR_IGNORE_RANGE_CHECK)) {
      /* if (rlIndex >= [rlArray + lenOffset]) goto kThrowArrayBounds */
      GenRegMemCheck(cUnit, kCondUge, rlIndex.lowReg, rlArray.lowReg, lenOffset, kThrowArrayBounds);
    }
    if ((size == kLong) || (size == kDouble)) {
      rlSrc = LoadValueWide(cUnit, rlSrc, regClass);
    } else {
      rlSrc = LoadValue(cUnit, rlSrc, regClass);
    }
    // If the src reg can't be byte accessed, move it to a temp first.
    if ((size == kSignedByte || size == kUnsignedByte) && rlSrc.lowReg >= 4) {
      int temp = AllocTemp(cUnit);
      OpRegCopy(cUnit, temp, rlSrc.lowReg);
      StoreBaseIndexedDisp(cUnit, rlArray.lowReg, rlIndex.lowReg, scale, dataOffset, temp,
                           INVALID_REG, size, INVALID_SREG);
    } else {
      StoreBaseIndexedDisp(cUnit, rlArray.lowReg, rlIndex.lowReg, scale, dataOffset, rlSrc.lowReg,
                           rlSrc.highReg, size, INVALID_SREG);
    }
  } else {
    bool needsRangeCheck = (!(optFlags & MIR_IGNORE_RANGE_CHECK));
    int regLen = INVALID_REG;
    if (needsRangeCheck) {
      regLen = AllocTemp(cUnit);
      //NOTE: max live temps(4) here.
      /* Get len */
      LoadWordDisp(cUnit, rlArray.lowReg, lenOffset, regLen);
    }
    /* regPtr -> array data */
    OpRegImm(cUnit, kOpAdd, regPtr, dataOffset);
    /* at this point, regPtr points to array, 2 live temps */
    if ((size == kLong) || (size == kDouble)) {
      //TUNING: specific wide routine that can handle fp regs
      if (scale) {
        int rNewIndex = AllocTemp(cUnit);
        OpRegRegImm(cUnit, kOpLsl, rNewIndex, rlIndex.lowReg, scale);
        OpRegReg(cUnit, kOpAdd, regPtr, rNewIndex);
        FreeTemp(cUnit, rNewIndex);
      } else {
        OpRegReg(cUnit, kOpAdd, regPtr, rlIndex.lowReg);
      }
      rlSrc = LoadValueWide(cUnit, rlSrc, regClass);

      if (needsRangeCheck) {
        GenRegRegCheck(cUnit, kCondCs, rlIndex.lowReg, regLen, kThrowArrayBounds);
        FreeTemp(cUnit, regLen);
      }

      StoreBaseDispWide(cUnit, regPtr, 0, rlSrc.lowReg, rlSrc.highReg);

      FreeTemp(cUnit, regPtr);
    } else {
      rlSrc = LoadValue(cUnit, rlSrc, regClass);
      if (needsRangeCheck) {
        GenRegRegCheck(cUnit, kCondCs, rlIndex.lowReg, regLen, kThrowArrayBounds);
        FreeTemp(cUnit, regLen);
      }
      StoreBaseIndexed(cUnit, regPtr, rlIndex.lowReg, rlSrc.lowReg,
                       scale, size);
    }
  }
}

void GenLong3Addr(CompilationUnit* cUnit, OpKind firstOp,
                  OpKind secondOp, RegLocation rlDest,
                  RegLocation rlSrc1, RegLocation rlSrc2)
{
  RegLocation rlResult;
  if (cUnit->instructionSet == kThumb2) {
    /*
     * NOTE:  This is the one place in the code in which we might have
     * as many as six live temporary registers.  There are 5 in the normal
     * set for Arm.  Until we have spill capabilities, temporarily add
     * lr to the temp set.  It is safe to do this locally, but note that
     * lr is used explicitly elsewhere in the code generator and cannot
     * normally be used as a general temp register.
     */
    MarkTemp(cUnit, TargetReg(kLr));   // Add lr to the temp pool
    FreeTemp(cUnit, TargetReg(kLr));   // and make it available
  }
  rlSrc1 = LoadValueWide(cUnit, rlSrc1, kCoreReg);
  rlSrc2 = LoadValueWide(cUnit, rlSrc2, kCoreReg);
  rlResult = EvalLoc(cUnit, rlDest, kCoreReg, true);
  // The longs may overlap - use intermediate temp if so
  if ((rlResult.lowReg == rlSrc1.highReg) || (rlResult.lowReg == rlSrc2.highReg)){
    int tReg = AllocTemp(cUnit);
    OpRegRegReg(cUnit, firstOp, tReg, rlSrc1.lowReg, rlSrc2.lowReg);
    OpRegRegReg(cUnit, secondOp, rlResult.highReg, rlSrc1.highReg, rlSrc2.highReg);
    OpRegCopy(cUnit, rlResult.lowReg, tReg);
    FreeTemp(cUnit, tReg);
  } else {
    OpRegRegReg(cUnit, firstOp, rlResult.lowReg, rlSrc1.lowReg, rlSrc2.lowReg);
    OpRegRegReg(cUnit, secondOp, rlResult.highReg, rlSrc1.highReg,
                rlSrc2.highReg);
  }
  /*
   * NOTE: If rlDest refers to a frame variable in a large frame, the
   * following StoreValueWide might need to allocate a temp register.
   * To further work around the lack of a spill capability, explicitly
   * free any temps from rlSrc1 & rlSrc2 that aren't still live in rlResult.
   * Remove when spill is functional.
   */
  FreeRegLocTemps(cUnit, rlResult, rlSrc1);
  FreeRegLocTemps(cUnit, rlResult, rlSrc2);
  StoreValueWide(cUnit, rlDest, rlResult);
  if (cUnit->instructionSet == kThumb2) {
    Clobber(cUnit, TargetReg(kLr));
    UnmarkTemp(cUnit, TargetReg(kLr));  // Remove lr from the temp pool
  }
}


bool GenShiftOpLong(CompilationUnit* cUnit, Instruction::Code opcode, RegLocation rlDest,
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
  FlushAllRegs(cUnit);   /* Send everything to home location */
  CallRuntimeHelperRegLocationRegLocation(cUnit, funcOffset, rlSrc1, rlShift, false);
  RegLocation rlResult = GetReturnWide(cUnit, false);
  StoreValueWide(cUnit, rlDest, rlResult);
  return false;
}


bool GenArithOpInt(CompilationUnit* cUnit, Instruction::Code opcode, RegLocation rlDest,
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
    /* NOTE: returns in kArg1 */
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
      LOG(FATAL) << "Invalid word arith op: " << opcode;
  }
  if (!isDivRem) {
    if (unary) {
      rlSrc1 = LoadValue(cUnit, rlSrc1, kCoreReg);
      rlResult = EvalLoc(cUnit, rlDest, kCoreReg, true);
      OpRegReg(cUnit, op, rlResult.lowReg, rlSrc1.lowReg);
    } else {
      if (shiftOp) {
        int tReg = INVALID_REG;
        if (cUnit->instructionSet == kX86) {
          // X86 doesn't require masking and must use ECX
          tReg = TargetReg(kCount);  // rCX
          LoadValueDirectFixed(cUnit, rlSrc2, tReg);
        } else {
          rlSrc2 = LoadValue(cUnit, rlSrc2, kCoreReg);
          tReg = AllocTemp(cUnit);
          OpRegRegImm(cUnit, kOpAnd, tReg, rlSrc2.lowReg, 31);
        }
        rlSrc1 = LoadValue(cUnit, rlSrc1, kCoreReg);
        rlResult = EvalLoc(cUnit, rlDest, kCoreReg, true);
        OpRegRegReg(cUnit, op, rlResult.lowReg, rlSrc1.lowReg, tReg);
        FreeTemp(cUnit, tReg);
      } else {
        rlSrc1 = LoadValue(cUnit, rlSrc1, kCoreReg);
        rlSrc2 = LoadValue(cUnit, rlSrc2, kCoreReg);
        rlResult = EvalLoc(cUnit, rlDest, kCoreReg, true);
        OpRegRegReg(cUnit, op, rlResult.lowReg, rlSrc1.lowReg, rlSrc2.lowReg);
      }
    }
    StoreValue(cUnit, rlDest, rlResult);
  } else {
    if (cUnit->instructionSet == kMips) {
      rlSrc1 = LoadValue(cUnit, rlSrc1, kCoreReg);
      rlSrc2 = LoadValue(cUnit, rlSrc2, kCoreReg);
      if (checkZero) {
          GenImmedCheck(cUnit, kCondEq, rlSrc2.lowReg, 0, kThrowDivZero);
      }
      rlResult = GenDivRem(cUnit, rlDest, rlSrc1.lowReg, rlSrc2.lowReg, op == kOpDiv);
    } else {
      int funcOffset = ENTRYPOINT_OFFSET(pIdivmod);
      FlushAllRegs(cUnit);   /* Send everything to home location */
      LoadValueDirectFixed(cUnit, rlSrc2, TargetReg(kArg1));
      int rTgt = CallHelperSetup(cUnit, funcOffset);
      LoadValueDirectFixed(cUnit, rlSrc1, TargetReg(kArg0));
      if (checkZero) {
        GenImmedCheck(cUnit, kCondEq, TargetReg(kArg1), 0, kThrowDivZero);
      }
      // NOTE: callout here is not a safepoint
      CallHelper(cUnit, rTgt, funcOffset, false /* not a safepoint */ );
      if (op == kOpDiv)
        rlResult = GetReturn(cUnit, false);
      else
        rlResult = GetReturnAlt(cUnit);
    }
    StoreValue(cUnit, rlDest, rlResult);
  }
  return false;
}

/*
 * The following are the first-level codegen routines that analyze the format
 * of each bytecode then either dispatch special purpose codegen routines
 * or produce corresponding Thumb instructions directly.
 */

bool IsPowerOfTwo(int x)
{
  return (x & (x - 1)) == 0;
}

// Returns true if no more than two bits are set in 'x'.
bool IsPopCountLE2(unsigned int x)
{
  x &= x - 1;
  return (x & (x - 1)) == 0;
}

// Returns the index of the lowest set bit in 'x'.
int LowestSetBit(unsigned int x) {
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
bool HandleEasyDivide(CompilationUnit* cUnit, Instruction::Code dalvikOpcode,
            RegLocation rlSrc, RegLocation rlDest, int lit)
{
  if ((lit < 2) || ((cUnit->instructionSet != kThumb2) && !IsPowerOfTwo(lit))) {
    return false;
  }
  // No divide instruction for Arm, so check for more special cases
  if ((cUnit->instructionSet == kThumb2) && !IsPowerOfTwo(lit)) {
    return SmallLiteralDivide(cUnit, dalvikOpcode, rlSrc, rlDest, lit);
  }
  int k = LowestSetBit(lit);
  if (k >= 30) {
    // Avoid special cases.
    return false;
  }
  bool div = (dalvikOpcode == Instruction::DIV_INT_LIT8 ||
      dalvikOpcode == Instruction::DIV_INT_LIT16);
  rlSrc = LoadValue(cUnit, rlSrc, kCoreReg);
  RegLocation rlResult = EvalLoc(cUnit, rlDest, kCoreReg, true);
  if (div) {
    int tReg = AllocTemp(cUnit);
    if (lit == 2) {
      // Division by 2 is by far the most common division by constant.
      OpRegRegImm(cUnit, kOpLsr, tReg, rlSrc.lowReg, 32 - k);
      OpRegRegReg(cUnit, kOpAdd, tReg, tReg, rlSrc.lowReg);
      OpRegRegImm(cUnit, kOpAsr, rlResult.lowReg, tReg, k);
    } else {
      OpRegRegImm(cUnit, kOpAsr, tReg, rlSrc.lowReg, 31);
      OpRegRegImm(cUnit, kOpLsr, tReg, tReg, 32 - k);
      OpRegRegReg(cUnit, kOpAdd, tReg, tReg, rlSrc.lowReg);
      OpRegRegImm(cUnit, kOpAsr, rlResult.lowReg, tReg, k);
    }
  } else {
    int tReg1 = AllocTemp(cUnit);
    int tReg2 = AllocTemp(cUnit);
    if (lit == 2) {
      OpRegRegImm(cUnit, kOpLsr, tReg1, rlSrc.lowReg, 32 - k);
      OpRegRegReg(cUnit, kOpAdd, tReg2, tReg1, rlSrc.lowReg);
      OpRegRegImm(cUnit, kOpAnd, tReg2, tReg2, lit -1);
      OpRegRegReg(cUnit, kOpSub, rlResult.lowReg, tReg2, tReg1);
    } else {
      OpRegRegImm(cUnit, kOpAsr, tReg1, rlSrc.lowReg, 31);
      OpRegRegImm(cUnit, kOpLsr, tReg1, tReg1, 32 - k);
      OpRegRegReg(cUnit, kOpAdd, tReg2, tReg1, rlSrc.lowReg);
      OpRegRegImm(cUnit, kOpAnd, tReg2, tReg2, lit - 1);
      OpRegRegReg(cUnit, kOpSub, rlResult.lowReg, tReg2, tReg1);
    }
  }
  StoreValue(cUnit, rlDest, rlResult);
  return true;
}

// Returns true if it added instructions to 'cUnit' to multiply 'rlSrc' by 'lit'
// and store the result in 'rlDest'.
bool HandleEasyMultiply(CompilationUnit* cUnit, RegLocation rlSrc,
                        RegLocation rlDest, int lit)
{
  // Can we simplify this multiplication?
  bool powerOfTwo = false;
  bool popCountLE2 = false;
  bool powerOfTwoMinusOne = false;
  if (lit < 2) {
    // Avoid special cases.
    return false;
  } else if (IsPowerOfTwo(lit)) {
    powerOfTwo = true;
  } else if (IsPopCountLE2(lit)) {
    popCountLE2 = true;
  } else if (IsPowerOfTwo(lit + 1)) {
    powerOfTwoMinusOne = true;
  } else {
    return false;
  }
  rlSrc = LoadValue(cUnit, rlSrc, kCoreReg);
  RegLocation rlResult = EvalLoc(cUnit, rlDest, kCoreReg, true);
  if (powerOfTwo) {
    // Shift.
    OpRegRegImm(cUnit, kOpLsl, rlResult.lowReg, rlSrc.lowReg,
                LowestSetBit(lit));
  } else if (popCountLE2) {
    // Shift and add and shift.
    int firstBit = LowestSetBit(lit);
    int secondBit = LowestSetBit(lit ^ (1 << firstBit));
    GenMultiplyByTwoBitMultiplier(cUnit, rlSrc, rlResult, lit,
                                  firstBit, secondBit);
  } else {
    // Reverse subtract: (src << (shift + 1)) - src.
    DCHECK(powerOfTwoMinusOne);
    // TUNING: rsb dst, src, src lsl#LowestSetBit(lit + 1)
    int tReg = AllocTemp(cUnit);
    OpRegRegImm(cUnit, kOpLsl, tReg, rlSrc.lowReg, LowestSetBit(lit + 1));
    OpRegRegReg(cUnit, kOpSub, rlResult.lowReg, tReg, rlSrc.lowReg);
  }
  StoreValue(cUnit, rlDest, rlResult);
  return true;
}

bool GenArithOpIntLit(CompilationUnit* cUnit, Instruction::Code opcode,
                      RegLocation rlDest, RegLocation rlSrc, int lit)
{
  RegLocation rlResult;
  OpKind op = static_cast<OpKind>(0);    /* Make gcc happy */
  int shiftOp = false;
  bool isDiv = false;

  switch (opcode) {
    case Instruction::RSUB_INT_LIT8:
    case Instruction::RSUB_INT: {
      int tReg;
      //TUNING: add support for use of Arm rsub op
      rlSrc = LoadValue(cUnit, rlSrc, kCoreReg);
      tReg = AllocTemp(cUnit);
      LoadConstant(cUnit, tReg, lit);
      rlResult = EvalLoc(cUnit, rlDest, kCoreReg, true);
      OpRegRegReg(cUnit, kOpSub, rlResult.lowReg, tReg, rlSrc.lowReg);
      StoreValue(cUnit, rlDest, rlResult);
      return false;
      break;
    }

    case Instruction::ADD_INT_LIT8:
    case Instruction::ADD_INT_LIT16:
      op = kOpAdd;
      break;
    case Instruction::MUL_INT_LIT8:
    case Instruction::MUL_INT_LIT16: {
      if (HandleEasyMultiply(cUnit, rlSrc, rlDest, lit)) {
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
        GenImmedCheck(cUnit, kCondAl, 0, 0, kThrowDivZero);
        return false;
      }
      if (HandleEasyDivide(cUnit, opcode, rlSrc, rlDest, lit)) {
        return false;
      }
      if ((opcode == Instruction::DIV_INT_LIT8) ||
          (opcode == Instruction::DIV_INT_LIT16)) {
        isDiv = true;
      } else {
        isDiv = false;
      }
      if (cUnit->instructionSet == kMips) {
        rlSrc = LoadValue(cUnit, rlSrc, kCoreReg);
        rlResult = GenDivRemLit(cUnit, rlDest, rlSrc.lowReg, lit, isDiv);
      } else {
        FlushAllRegs(cUnit);   /* Everything to home location */
        LoadValueDirectFixed(cUnit, rlSrc, TargetReg(kArg0));
        Clobber(cUnit, TargetReg(kArg0));
        int funcOffset = ENTRYPOINT_OFFSET(pIdivmod);
        CallRuntimeHelperRegImm(cUnit, funcOffset, TargetReg(kArg0), lit, false);
        if (isDiv)
          rlResult = GetReturn(cUnit, false);
        else
          rlResult = GetReturnAlt(cUnit);
      }
      StoreValue(cUnit, rlDest, rlResult);
      return false;
      break;
    }
    default:
      return true;
  }
  rlSrc = LoadValue(cUnit, rlSrc, kCoreReg);
  rlResult = EvalLoc(cUnit, rlDest, kCoreReg, true);
  // Avoid shifts by literal 0 - no support in Thumb.  Change to copy
  if (shiftOp && (lit == 0)) {
    OpRegCopy(cUnit, rlResult.lowReg, rlSrc.lowReg);
  } else {
    OpRegRegImm(cUnit, op, rlResult.lowReg, rlSrc.lowReg, lit);
  }
  StoreValue(cUnit, rlDest, rlResult);
  return false;
}

bool GenArithOpLong(CompilationUnit* cUnit, Instruction::Code opcode, RegLocation rlDest,
          RegLocation rlSrc1, RegLocation rlSrc2)
{
  RegLocation rlResult;
  OpKind firstOp = kOpBkpt;
  OpKind secondOp = kOpBkpt;
  bool callOut = false;
  bool checkZero = false;
  int funcOffset;
  int retReg = TargetReg(kRet0);

  switch (opcode) {
    case Instruction::NOT_LONG:
      rlSrc2 = LoadValueWide(cUnit, rlSrc2, kCoreReg);
      rlResult = EvalLoc(cUnit, rlDest, kCoreReg, true);
      // Check for destructive overlap
      if (rlResult.lowReg == rlSrc2.highReg) {
        int tReg = AllocTemp(cUnit);
        OpRegCopy(cUnit, tReg, rlSrc2.highReg);
        OpRegReg(cUnit, kOpMvn, rlResult.lowReg, rlSrc2.lowReg);
        OpRegReg(cUnit, kOpMvn, rlResult.highReg, tReg);
        FreeTemp(cUnit, tReg);
      } else {
        OpRegReg(cUnit, kOpMvn, rlResult.lowReg, rlSrc2.lowReg);
        OpRegReg(cUnit, kOpMvn, rlResult.highReg, rlSrc2.highReg);
      }
      StoreValueWide(cUnit, rlDest, rlResult);
      return false;
      break;
    case Instruction::ADD_LONG:
    case Instruction::ADD_LONG_2ADDR:
      if (cUnit->instructionSet != kThumb2) {
        return GenAddLong(cUnit, rlDest, rlSrc1, rlSrc2);
      }
      firstOp = kOpAdd;
      secondOp = kOpAdc;
      break;
    case Instruction::SUB_LONG:
    case Instruction::SUB_LONG_2ADDR:
      if (cUnit->instructionSet != kThumb2) {
        return GenSubLong(cUnit, rlDest, rlSrc1, rlSrc2);
      }
      firstOp = kOpSub;
      secondOp = kOpSbc;
      break;
    case Instruction::MUL_LONG:
    case Instruction::MUL_LONG_2ADDR:
      callOut = true;
      retReg = TargetReg(kRet0);
      funcOffset = ENTRYPOINT_OFFSET(pLmul);
      break;
    case Instruction::DIV_LONG:
    case Instruction::DIV_LONG_2ADDR:
      callOut = true;
      checkZero = true;
      retReg = TargetReg(kRet0);
      funcOffset = ENTRYPOINT_OFFSET(pLdiv);
      break;
    case Instruction::REM_LONG:
    case Instruction::REM_LONG_2ADDR:
      callOut = true;
      checkZero = true;
      funcOffset = ENTRYPOINT_OFFSET(pLdivmod);
      /* NOTE - for Arm, result is in kArg2/kArg3 instead of kRet0/kRet1 */
      retReg = (cUnit->instructionSet == kThumb2) ? TargetReg(kArg2) : TargetReg(kRet0);
      break;
    case Instruction::AND_LONG_2ADDR:
    case Instruction::AND_LONG:
      if (cUnit->instructionSet == kX86) {
        return GenAndLong(cUnit, rlDest, rlSrc1, rlSrc2);
      }
      firstOp = kOpAnd;
      secondOp = kOpAnd;
      break;
    case Instruction::OR_LONG:
    case Instruction::OR_LONG_2ADDR:
      if (cUnit->instructionSet == kX86) {
        return GenOrLong(cUnit, rlDest, rlSrc1, rlSrc2);
      }
      firstOp = kOpOr;
      secondOp = kOpOr;
      break;
    case Instruction::XOR_LONG:
    case Instruction::XOR_LONG_2ADDR:
      if (cUnit->instructionSet == kX86) {
        return GenXorLong(cUnit, rlDest, rlSrc1, rlSrc2);
      }
      firstOp = kOpXor;
      secondOp = kOpXor;
      break;
    case Instruction::NEG_LONG: {
      return GenNegLong(cUnit, rlDest, rlSrc2);
    }
    default:
      LOG(FATAL) << "Invalid long arith op";
  }
  if (!callOut) {
    GenLong3Addr(cUnit, firstOp, secondOp, rlDest, rlSrc1, rlSrc2);
  } else {
    FlushAllRegs(cUnit);   /* Send everything to home location */
    if (checkZero) {
      LoadValueDirectWideFixed(cUnit, rlSrc2, TargetReg(kArg2), TargetReg(kArg3));
      int rTgt = CallHelperSetup(cUnit, funcOffset);
      GenDivZeroCheck(cUnit, TargetReg(kArg2), TargetReg(kArg3));
      LoadValueDirectWideFixed(cUnit, rlSrc1, TargetReg(kArg0), TargetReg(kArg1));
      // NOTE: callout here is not a safepoint
      CallHelper(cUnit, rTgt, funcOffset, false /* not safepoint */);
    } else {
      CallRuntimeHelperRegLocationRegLocation(cUnit, funcOffset,
                          rlSrc1, rlSrc2, false);
    }
    // Adjust return regs in to handle case of rem returning kArg2/kArg3
    if (retReg == TargetReg(kRet0))
      rlResult = GetReturnWide(cUnit, false);
    else
      rlResult = GetReturnWideAlt(cUnit);
    StoreValueWide(cUnit, rlDest, rlResult);
  }
  return false;
}

bool GenConversionCall(CompilationUnit* cUnit, int funcOffset,
                       RegLocation rlDest, RegLocation rlSrc)
{
  /*
   * Don't optimize the register usage since it calls out to support
   * functions
   */
  FlushAllRegs(cUnit);   /* Send everything to home location */
  if (rlSrc.wide) {
    LoadValueDirectWideFixed(cUnit, rlSrc, rlSrc.fp ? TargetReg(kFArg0) : TargetReg(kArg0),
                             rlSrc.fp ? TargetReg(kFArg1) : TargetReg(kArg1));
  } else {
    LoadValueDirectFixed(cUnit, rlSrc, rlSrc.fp ? TargetReg(kFArg0) : TargetReg(kArg0));
  }
  CallRuntimeHelperRegLocation(cUnit, funcOffset, rlSrc, false);
  if (rlDest.wide) {
    RegLocation rlResult;
    rlResult = GetReturnWide(cUnit, rlDest.fp);
    StoreValueWide(cUnit, rlDest, rlResult);
  } else {
    RegLocation rlResult;
    rlResult = GetReturn(cUnit, rlDest.fp);
    StoreValue(cUnit, rlDest, rlResult);
  }
  return false;
}

bool GenArithOpFloatPortable(CompilationUnit* cUnit, Instruction::Code opcode,
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
      GenNegFloat(cUnit, rlDest, rlSrc1);
      return false;
    }
    default:
      return true;
  }
  FlushAllRegs(cUnit);   /* Send everything to home location */
  CallRuntimeHelperRegLocationRegLocation(cUnit, funcOffset, rlSrc1, rlSrc2, false);
  rlResult = GetReturn(cUnit, true);
  StoreValue(cUnit, rlDest, rlResult);
  return false;
}

bool GenArithOpDoublePortable(CompilationUnit* cUnit, Instruction::Code opcode,
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
      GenNegDouble(cUnit, rlDest, rlSrc1);
      return false;
    }
    default:
      return true;
  }
  FlushAllRegs(cUnit);   /* Send everything to home location */
  CallRuntimeHelperRegLocationRegLocation(cUnit, funcOffset, rlSrc1, rlSrc2, false);
  rlResult = GetReturnWide(cUnit, true);
  StoreValueWide(cUnit, rlDest, rlResult);
  return false;
}

bool GenConversionPortable(CompilationUnit* cUnit, Instruction::Code opcode,
                           RegLocation rlDest, RegLocation rlSrc)
{

  switch (opcode) {
    case Instruction::INT_TO_FLOAT:
      return GenConversionCall(cUnit, ENTRYPOINT_OFFSET(pI2f),
                   rlDest, rlSrc);
    case Instruction::FLOAT_TO_INT:
      return GenConversionCall(cUnit, ENTRYPOINT_OFFSET(pF2iz),
                   rlDest, rlSrc);
    case Instruction::DOUBLE_TO_FLOAT:
      return GenConversionCall(cUnit, ENTRYPOINT_OFFSET(pD2f),
                   rlDest, rlSrc);
    case Instruction::FLOAT_TO_DOUBLE:
      return GenConversionCall(cUnit, ENTRYPOINT_OFFSET(pF2d),
                   rlDest, rlSrc);
    case Instruction::INT_TO_DOUBLE:
      return GenConversionCall(cUnit, ENTRYPOINT_OFFSET(pI2d),
                   rlDest, rlSrc);
    case Instruction::DOUBLE_TO_INT:
      return GenConversionCall(cUnit, ENTRYPOINT_OFFSET(pD2iz),
                   rlDest, rlSrc);
    case Instruction::FLOAT_TO_LONG:
      return GenConversionCall(cUnit, ENTRYPOINT_OFFSET(pF2l),
                   rlDest, rlSrc);
    case Instruction::LONG_TO_FLOAT:
      return GenConversionCall(cUnit, ENTRYPOINT_OFFSET(pL2f),
                   rlDest, rlSrc);
    case Instruction::DOUBLE_TO_LONG:
      return GenConversionCall(cUnit, ENTRYPOINT_OFFSET(pD2l),
                   rlDest, rlSrc);
    case Instruction::LONG_TO_DOUBLE:
      return GenConversionCall(cUnit, ENTRYPOINT_OFFSET(pL2d),
                   rlDest, rlSrc);
    default:
      return true;
  }
  return false;
}

/* Check if we need to check for pending suspend request */
void GenSuspendTest(CompilationUnit* cUnit, int optFlags)
{
  if (NO_SUSPEND || (optFlags & MIR_IGNORE_SUSPEND_CHECK)) {
    return;
  }
  FlushAllRegs(cUnit);
  LIR* branch = OpTestSuspend(cUnit, NULL);
  LIR* retLab = NewLIR0(cUnit, kPseudoTargetLabel);
  LIR* target = RawLIR(cUnit, cUnit->currentDalvikOffset, kPseudoSuspendTarget,
                       reinterpret_cast<uintptr_t>(retLab), cUnit->currentDalvikOffset);
  branch->target = target;
  InsertGrowableList(cUnit, &cUnit->suspendLaunchpads, reinterpret_cast<uintptr_t>(target));
}

/* Check if we need to check for pending suspend request */
void GenSuspendTestAndBranch(CompilationUnit* cUnit, int optFlags, LIR* target)
{
  if (NO_SUSPEND || (optFlags & MIR_IGNORE_SUSPEND_CHECK)) {
    OpUnconditionalBranch(cUnit, target);
    return;
  }
  OpTestSuspend(cUnit, target);
  LIR* launchPad =
      RawLIR(cUnit, cUnit->currentDalvikOffset, kPseudoSuspendTarget,
             reinterpret_cast<uintptr_t>(target), cUnit->currentDalvikOffset);
  FlushAllRegs(cUnit);
  OpUnconditionalBranch(cUnit, launchPad);
  InsertGrowableList(cUnit, &cUnit->suspendLaunchpads, reinterpret_cast<uintptr_t>(launchPad));
}

}  // namespace art
