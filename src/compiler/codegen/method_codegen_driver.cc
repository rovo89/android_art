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

#include "object_utils.h"

#include "local_optimizations.h"

namespace art {

const RegLocation badLoc = {kLocDalvikFrame, 0, 0, 0, 0, 0, 0, 0, 0,
                            INVALID_REG, INVALID_REG, INVALID_SREG,
                            INVALID_SREG};

/* Mark register usage state and return long retloc */
RegLocation oatGetReturnWide(CompilationUnit* cUnit, bool isDouble)
{
  RegLocation gpr_res = locCReturnWide();
  RegLocation fpr_res = locCReturnDouble();
  RegLocation res = isDouble ? fpr_res : gpr_res;
  oatClobber(cUnit, res.lowReg);
  oatClobber(cUnit, res.highReg);
  oatLockTemp(cUnit, res.lowReg);
  oatLockTemp(cUnit, res.highReg);
  oatMarkPair(cUnit, res.lowReg, res.highReg);
  return res;
}

RegLocation oatGetReturn(CompilationUnit* cUnit, bool isFloat)
{
  RegLocation gpr_res = locCReturn();
  RegLocation fpr_res = locCReturnFloat();
  RegLocation res = isFloat ? fpr_res : gpr_res;
  oatClobber(cUnit, res.lowReg);
  if (cUnit->instructionSet == kMips) {
    oatMarkInUse(cUnit, res.lowReg);
  } else {
    oatLockTemp(cUnit, res.lowReg);
  }
  return res;
}

// TODO: move to gen_invoke.cc
void genInvoke(CompilationUnit* cUnit, CallInfo* info)
{
  if (genIntrinsic(cUnit, info)) {
    return;
  }
  InvokeType originalType = info->type;  // avoiding mutation by ComputeInvokeInfo
  int callState = 0;
  LIR* nullCk;
  LIR** pNullCk = NULL;
  NextCallInsn nextCallInsn;
  oatFlushAllRegs(cUnit);  /* Everything to home location */
  // Explicit register usage
  oatLockCallTemps(cUnit);

  OatCompilationUnit mUnit(cUnit->class_loader, cUnit->class_linker,
                           *cUnit->dex_file,
                           cUnit->code_item, cUnit->method_idx,
                           cUnit->access_flags);

  uint32_t dexMethodIdx = info->index;
  int vtableIdx;
  uintptr_t directCode;
  uintptr_t directMethod;
  bool skipThis;
  bool fastPath =
    cUnit->compiler->ComputeInvokeInfo(dexMethodIdx, &mUnit, info->type,
                                       vtableIdx, directCode,
                                       directMethod)
    && !SLOW_INVOKE_PATH;
  if (info->type == kInterface) {
    if (fastPath) {
      pNullCk = &nullCk;
    }
    nextCallInsn = fastPath ? nextInterfaceCallInsn
                            : nextInterfaceCallInsnWithAccessCheck;
    skipThis = false;
  } else if (info->type == kDirect) {
    if (fastPath) {
      pNullCk = &nullCk;
    }
    nextCallInsn = fastPath ? nextSDCallInsn : nextDirectCallInsnSP;
    skipThis = false;
  } else if (info->type == kStatic) {
    nextCallInsn = fastPath ? nextSDCallInsn : nextStaticCallInsnSP;
    skipThis = false;
  } else if (info->type == kSuper) {
    DCHECK(!fastPath);  // Fast path is a direct call.
    nextCallInsn = nextSuperCallInsnSP;
    skipThis = false;
  } else {
    DCHECK_EQ(info->type, kVirtual);
    nextCallInsn = fastPath ? nextVCallInsn : nextVCallInsnSP;
    skipThis = fastPath;
  }
  if (!info->isRange) {
    callState = genDalvikArgsNoRange(cUnit, info, callState, pNullCk,
                                     nextCallInsn, dexMethodIdx,
                                     vtableIdx, directCode, directMethod,
                                     originalType, skipThis);
  } else {
    callState = genDalvikArgsRange(cUnit, info, callState, pNullCk,
                                   nextCallInsn, dexMethodIdx, vtableIdx,
                                   directCode, directMethod, originalType,
                                   skipThis);
  }
  // Finish up any of the call sequence not interleaved in arg loading
  while (callState >= 0) {
    callState = nextCallInsn(cUnit, info, callState, dexMethodIdx,
                             vtableIdx, directCode, directMethod,
                             originalType);
  }
  if (cUnit->enableDebug & (1 << kDebugDisplayMissingTargets)) {
    genShowTarget(cUnit);
  }
  LIR* callInst;
  if (cUnit->instructionSet != kX86) {
    callInst = opReg(cUnit, kOpBlx, targetReg(kInvokeTgt));
  } else {
    if (fastPath && info->type != kInterface) {
      callInst = opMem(cUnit, kOpBlx, targetReg(kArg0),
                       AbstractMethod::GetCodeOffset().Int32Value());
    } else {
      int trampoline = 0;
      switch (info->type) {
      case kInterface:
        trampoline = fastPath ? ENTRYPOINT_OFFSET(pInvokeInterfaceTrampoline)
            : ENTRYPOINT_OFFSET(pInvokeInterfaceTrampolineWithAccessCheck);
        break;
      case kDirect:
        trampoline = ENTRYPOINT_OFFSET(pInvokeDirectTrampolineWithAccessCheck);
        break;
      case kStatic:
        trampoline = ENTRYPOINT_OFFSET(pInvokeStaticTrampolineWithAccessCheck);
        break;
      case kSuper:
        trampoline = ENTRYPOINT_OFFSET(pInvokeSuperTrampolineWithAccessCheck);
        break;
      case kVirtual:
        trampoline = ENTRYPOINT_OFFSET(pInvokeVirtualTrampolineWithAccessCheck);
        break;
      default:
        LOG(FATAL) << "Unexpected invoke type";
      }
      callInst = opThreadMem(cUnit, kOpBlx, trampoline);
    }
  }
  markSafepointPC(cUnit, callInst);

  oatClobberCalleeSave(cUnit);
  if (info->result.location != kLocInvalid) {
    // We have a following MOVE_RESULT - do it now.
    if (info->result.wide) {
      RegLocation retLoc = oatGetReturnWide(cUnit, info->result.fp);
      storeValueWide(cUnit, info->result, retLoc);
    } else {
      RegLocation retLoc = oatGetReturn(cUnit, info->result.fp);
      storeValue(cUnit, info->result, retLoc);
    }
  }
}

/*
 * Build an array of location records for the incoming arguments.
 * Note: one location record per word of arguments, with dummy
 * high-word loc for wide arguments.  Also pull up any following
 * MOVE_RESULT and incorporate it into the invoke.
 */
//TODO: move to gen_invoke.cc or utils
CallInfo* oatNewCallInfo(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir,
                         InvokeType type, bool isRange)
{
  CallInfo* info = (CallInfo*)oatNew(cUnit, sizeof(CallInfo), true,
                                         kAllocMisc);
  MIR* moveResultMIR = oatFindMoveResult(cUnit, bb, mir);
  if (moveResultMIR == NULL) {
    info->result.location = kLocInvalid;
  } else {
    info->result = oatGetRawDest(cUnit, moveResultMIR);
    moveResultMIR->dalvikInsn.opcode = Instruction::NOP;
  }
  info->numArgWords = mir->ssaRep->numUses;
  info->args = (info->numArgWords == 0) ? NULL : (RegLocation*)
      oatNew(cUnit, sizeof(RegLocation) * info->numArgWords, false, kAllocMisc);
  for (int i = 0; i < info->numArgWords; i++) {
    info->args[i] = oatGetRawSrc(cUnit, mir, i);
  }
  info->optFlags = mir->optimizationFlags;
  info->type = type;
  info->isRange = isRange;
  info->index = mir->dalvikInsn.vB;
  info->offset = mir->offset;
  return info;
}

/*
 * Target-independent code generation.  Use only high-level
 * load/store utilities here, or target-dependent genXX() handlers
 * when necessary.
 */
bool compileDalvikInstruction(CompilationUnit* cUnit, MIR* mir,
                              BasicBlock* bb, LIR* labelList)
{
  bool res = false;   // Assume success
  RegLocation rlSrc[3];
  RegLocation rlDest = badLoc;
  RegLocation rlResult = badLoc;
  Instruction::Code opcode = mir->dalvikInsn.opcode;
  int optFlags = mir->optimizationFlags;
  uint32_t vB = mir->dalvikInsn.vB;
  uint32_t vC = mir->dalvikInsn.vC;

  /* Prep Src and Dest locations */
  int nextSreg = 0;
  int nextLoc = 0;
  int attrs = oatDataFlowAttributes[opcode];
  rlSrc[0] = rlSrc[1] = rlSrc[2] = badLoc;
  if (attrs & DF_UA) {
    if (attrs & DF_A_WIDE) {
      rlSrc[nextLoc++] = oatGetSrcWide(cUnit, mir, nextSreg);
      nextSreg+= 2;
    } else {
      rlSrc[nextLoc++] = oatGetSrc(cUnit, mir, nextSreg);
      nextSreg++;
    }
  }
  if (attrs & DF_UB) {
    if (attrs & DF_B_WIDE) {
      rlSrc[nextLoc++] = oatGetSrcWide(cUnit, mir, nextSreg);
      nextSreg+= 2;
    } else {
      rlSrc[nextLoc++] = oatGetSrc(cUnit, mir, nextSreg);
      nextSreg++;
    }
  }
  if (attrs & DF_UC) {
    if (attrs & DF_C_WIDE) {
      rlSrc[nextLoc++] = oatGetSrcWide(cUnit, mir, nextSreg);
    } else {
      rlSrc[nextLoc++] = oatGetSrc(cUnit, mir, nextSreg);
    }
  }
  if (attrs & DF_DA) {
    if (attrs & DF_A_WIDE) {
      rlDest = oatGetDestWide(cUnit, mir);
    } else {
      rlDest = oatGetDest(cUnit, mir);
    }
  }
  switch (opcode) {
    case Instruction::NOP:
      break;

    case Instruction::MOVE_EXCEPTION:
      genMoveException(cUnit, rlDest);
      break;
    case Instruction::RETURN_VOID:
      if (!(cUnit->attrs & METHOD_IS_LEAF)) {
        genSuspendTest(cUnit, optFlags);
      }
      break;

    case Instruction::RETURN:
    case Instruction::RETURN_OBJECT:
      if (!(cUnit->attrs & METHOD_IS_LEAF)) {
        genSuspendTest(cUnit, optFlags);
      }
      storeValue(cUnit, oatGetReturn(cUnit, cUnit->shorty[0] == 'F'), rlSrc[0]);
      break;

    case Instruction::RETURN_WIDE:
      if (!(cUnit->attrs & METHOD_IS_LEAF)) {
        genSuspendTest(cUnit, optFlags);
      }
      storeValueWide(cUnit, oatGetReturnWide(cUnit,
                       cUnit->shorty[0] == 'D'), rlSrc[0]);
      break;

    case Instruction::MOVE_RESULT_WIDE:
      if (optFlags & MIR_INLINED)
        break;  // Nop - combined w/ previous invoke
      storeValueWide(cUnit, rlDest, oatGetReturnWide(cUnit, rlDest.fp));
      break;

    case Instruction::MOVE_RESULT:
    case Instruction::MOVE_RESULT_OBJECT:
      if (optFlags & MIR_INLINED)
        break;  // Nop - combined w/ previous invoke
      storeValue(cUnit, rlDest, oatGetReturn(cUnit, rlDest.fp));
      break;

    case Instruction::MOVE:
    case Instruction::MOVE_OBJECT:
    case Instruction::MOVE_16:
    case Instruction::MOVE_OBJECT_16:
    case Instruction::MOVE_FROM16:
    case Instruction::MOVE_OBJECT_FROM16:
      storeValue(cUnit, rlDest, rlSrc[0]);
      break;

    case Instruction::MOVE_WIDE:
    case Instruction::MOVE_WIDE_16:
    case Instruction::MOVE_WIDE_FROM16:
      storeValueWide(cUnit, rlDest, rlSrc[0]);
      break;

    case Instruction::CONST:
    case Instruction::CONST_4:
    case Instruction::CONST_16:
      rlResult = oatEvalLoc(cUnit, rlDest, kAnyReg, true);
      loadConstantNoClobber(cUnit, rlResult.lowReg, vB);
      storeValue(cUnit, rlDest, rlResult);
      break;

    case Instruction::CONST_HIGH16:
      rlResult = oatEvalLoc(cUnit, rlDest, kAnyReg, true);
      loadConstantNoClobber(cUnit, rlResult.lowReg, vB << 16);
      storeValue(cUnit, rlDest, rlResult);
      break;

    case Instruction::CONST_WIDE_16:
    case Instruction::CONST_WIDE_32:
      rlResult = oatEvalLoc(cUnit, rlDest, kAnyReg, true);
      loadConstantValueWide(cUnit, rlResult.lowReg, rlResult.highReg, vB,
                            (vB & 0x80000000) ? -1 : 0);
      storeValueWide(cUnit, rlDest, rlResult);
      break;

    case Instruction::CONST_WIDE:
      rlResult = oatEvalLoc(cUnit, rlDest, kAnyReg, true);
      loadConstantValueWide(cUnit, rlResult.lowReg, rlResult.highReg,
                            mir->dalvikInsn.vB_wide & 0xffffffff,
                            (mir->dalvikInsn.vB_wide >> 32) & 0xffffffff);
      storeValueWide(cUnit, rlDest, rlResult);
      break;

    case Instruction::CONST_WIDE_HIGH16:
      rlResult = oatEvalLoc(cUnit, rlDest, kAnyReg, true);
      loadConstantValueWide(cUnit, rlResult.lowReg, rlResult.highReg,
                            0, vB << 16);
      storeValueWide(cUnit, rlDest, rlResult);
      break;

    case Instruction::MONITOR_ENTER:
      genMonitorEnter(cUnit, optFlags, rlSrc[0]);
      break;

    case Instruction::MONITOR_EXIT:
      genMonitorExit(cUnit, optFlags, rlSrc[0]);
      break;

    case Instruction::CHECK_CAST:
      genCheckCast(cUnit, vB, rlSrc[0]);
      break;

    case Instruction::INSTANCE_OF:
      genInstanceof(cUnit, vC, rlDest, rlSrc[0]);
      break;

    case Instruction::NEW_INSTANCE:
      genNewInstance(cUnit, vB, rlDest);
      break;

    case Instruction::THROW:
      genThrow(cUnit, rlSrc[0]);
      break;

    case Instruction::ARRAY_LENGTH:
      int lenOffset;
      lenOffset = Array::LengthOffset().Int32Value();
      rlSrc[0] = loadValue(cUnit, rlSrc[0], kCoreReg);
      genNullCheck(cUnit, rlSrc[0].sRegLow, rlSrc[0].lowReg, optFlags);
      rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
      loadWordDisp(cUnit, rlSrc[0].lowReg, lenOffset, rlResult.lowReg);
      storeValue(cUnit, rlDest, rlResult);
      break;

    case Instruction::CONST_STRING:
    case Instruction::CONST_STRING_JUMBO:
      genConstString(cUnit, vB, rlDest);
      break;

    case Instruction::CONST_CLASS:
      genConstClass(cUnit, vB, rlDest);
      break;

    case Instruction::FILL_ARRAY_DATA:
      genFillArrayData(cUnit, vB, rlSrc[0]);
      break;

    case Instruction::FILLED_NEW_ARRAY:
      genFilledNewArray(cUnit, oatNewCallInfo(cUnit, bb, mir, kStatic,
                        false /* not range */));
      break;

    case Instruction::FILLED_NEW_ARRAY_RANGE:
      genFilledNewArray(cUnit, oatNewCallInfo(cUnit, bb, mir, kStatic,
                        true /* range */));
      break;

    case Instruction::NEW_ARRAY:
      genNewArray(cUnit, vC, rlDest, rlSrc[0]);
      break;

    case Instruction::GOTO:
    case Instruction::GOTO_16:
    case Instruction::GOTO_32:
      if (bb->taken->startOffset <= mir->offset) {
        genSuspendTestAndBranch(cUnit, optFlags, &labelList[bb->taken->id]);
      } else {
        opUnconditionalBranch(cUnit, &labelList[bb->taken->id]);
      }
      break;

    case Instruction::PACKED_SWITCH:
      genPackedSwitch(cUnit, vB, rlSrc[0]);
      break;

    case Instruction::SPARSE_SWITCH:
      genSparseSwitch(cUnit, vB, rlSrc[0]);
      break;

    case Instruction::CMPL_FLOAT:
    case Instruction::CMPG_FLOAT:
    case Instruction::CMPL_DOUBLE:
    case Instruction::CMPG_DOUBLE:
      res = genCmpFP(cUnit, opcode, rlDest, rlSrc[0], rlSrc[1]);
      break;

    case Instruction::CMP_LONG:
      genCmpLong(cUnit, rlDest, rlSrc[0], rlSrc[1]);
      break;

    case Instruction::IF_EQ:
    case Instruction::IF_NE:
    case Instruction::IF_LT:
    case Instruction::IF_GE:
    case Instruction::IF_GT:
    case Instruction::IF_LE: {
      LIR* taken = &labelList[bb->taken->id];
      LIR* fallThrough = &labelList[bb->fallThrough->id];
      bool backwardBranch;
      backwardBranch = (bb->taken->startOffset <= mir->offset);
      if (backwardBranch) {
        genSuspendTest(cUnit, optFlags);
      }
      genCompareAndBranch(cUnit, opcode, rlSrc[0], rlSrc[1], taken,
                          fallThrough);
      break;
      }

    case Instruction::IF_EQZ:
    case Instruction::IF_NEZ:
    case Instruction::IF_LTZ:
    case Instruction::IF_GEZ:
    case Instruction::IF_GTZ:
    case Instruction::IF_LEZ: {
      LIR* taken = &labelList[bb->taken->id];
      LIR* fallThrough = &labelList[bb->fallThrough->id];
      bool backwardBranch;
      backwardBranch = (bb->taken->startOffset <= mir->offset);
      if (backwardBranch) {
        genSuspendTest(cUnit, optFlags);
      }
      genCompareZeroAndBranch(cUnit, opcode, rlSrc[0], taken, fallThrough);
      break;
      }

    case Instruction::AGET_WIDE:
      genArrayGet(cUnit, optFlags, kLong, rlSrc[0], rlSrc[1], rlDest, 3);
      break;
    case Instruction::AGET:
    case Instruction::AGET_OBJECT:
      genArrayGet(cUnit, optFlags, kWord, rlSrc[0], rlSrc[1], rlDest, 2);
      break;
    case Instruction::AGET_BOOLEAN:
      genArrayGet(cUnit, optFlags, kUnsignedByte, rlSrc[0], rlSrc[1], rlDest, 0);
      break;
    case Instruction::AGET_BYTE:
      genArrayGet(cUnit, optFlags, kSignedByte, rlSrc[0], rlSrc[1], rlDest, 0);
      break;
    case Instruction::AGET_CHAR:
      genArrayGet(cUnit, optFlags, kUnsignedHalf, rlSrc[0], rlSrc[1], rlDest, 1);
      break;
    case Instruction::AGET_SHORT:
      genArrayGet(cUnit, optFlags, kSignedHalf, rlSrc[0], rlSrc[1], rlDest, 1);
      break;
    case Instruction::APUT_WIDE:
      genArrayPut(cUnit, optFlags, kLong, rlSrc[1], rlSrc[2], rlSrc[0], 3);
      break;
    case Instruction::APUT:
      genArrayPut(cUnit, optFlags, kWord, rlSrc[1], rlSrc[2], rlSrc[0], 2);
      break;
    case Instruction::APUT_OBJECT:
      genArrayObjPut(cUnit, optFlags, rlSrc[1], rlSrc[2], rlSrc[0], 2);
      break;
    case Instruction::APUT_SHORT:
    case Instruction::APUT_CHAR:
      genArrayPut(cUnit, optFlags, kUnsignedHalf, rlSrc[1], rlSrc[2], rlSrc[0], 1);
      break;
    case Instruction::APUT_BYTE:
    case Instruction::APUT_BOOLEAN:
      genArrayPut(cUnit, optFlags, kUnsignedByte, rlSrc[1], rlSrc[2],
            rlSrc[0], 0);
      break;

    case Instruction::IGET_OBJECT:
    //case Instruction::IGET_OBJECT_VOLATILE:
      genIGet(cUnit, vC, optFlags, kWord, rlDest, rlSrc[0], false, true);
      break;

    case Instruction::IGET_WIDE:
    //case Instruction::IGET_WIDE_VOLATILE:
      genIGet(cUnit, vC, optFlags, kLong, rlDest, rlSrc[0], true, false);
      break;

    case Instruction::IGET:
    //case Instruction::IGET_VOLATILE:
      genIGet(cUnit, vC, optFlags, kWord, rlDest, rlSrc[0], false, false);
      break;

    case Instruction::IGET_CHAR:
      genIGet(cUnit, vC, optFlags, kUnsignedHalf, rlDest, rlSrc[0], false, false);
      break;

    case Instruction::IGET_SHORT:
      genIGet(cUnit, vC, optFlags, kSignedHalf, rlDest, rlSrc[0], false, false);
      break;

    case Instruction::IGET_BOOLEAN:
    case Instruction::IGET_BYTE:
      genIGet(cUnit, vC, optFlags, kUnsignedByte, rlDest, rlSrc[0], false, false);
      break;

    case Instruction::IPUT_WIDE:
    //case Instruction::IPUT_WIDE_VOLATILE:
      genIPut(cUnit, vC, optFlags, kLong, rlSrc[0], rlSrc[1], true, false);
      break;

    case Instruction::IPUT_OBJECT:
    //case Instruction::IPUT_OBJECT_VOLATILE:
      genIPut(cUnit, vC, optFlags, kWord, rlSrc[0], rlSrc[1], false, true);
      break;

    case Instruction::IPUT:
    //case Instruction::IPUT_VOLATILE:
      genIPut(cUnit, vC, optFlags, kWord, rlSrc[0], rlSrc[1], false, false);
      break;

    case Instruction::IPUT_BOOLEAN:
    case Instruction::IPUT_BYTE:
      genIPut(cUnit, vC, optFlags, kUnsignedByte, rlSrc[0], rlSrc[1], false, false);
      break;

    case Instruction::IPUT_CHAR:
      genIPut(cUnit, vC, optFlags, kUnsignedHalf, rlSrc[0], rlSrc[1], false, false);
      break;

    case Instruction::IPUT_SHORT:
      genIPut(cUnit, vC, optFlags, kSignedHalf, rlSrc[0], rlSrc[1], false, false);
      break;

    case Instruction::SGET_OBJECT:
      genSget(cUnit, vB, rlDest, false, true);
      break;
    case Instruction::SGET:
    case Instruction::SGET_BOOLEAN:
    case Instruction::SGET_BYTE:
    case Instruction::SGET_CHAR:
    case Instruction::SGET_SHORT:
      genSget(cUnit, vB, rlDest, false, false);
      break;

    case Instruction::SGET_WIDE:
      genSget(cUnit, vB, rlDest, true, false);
      break;

    case Instruction::SPUT_OBJECT:
      genSput(cUnit, vB, rlSrc[0], false, true);
      break;

    case Instruction::SPUT:
    case Instruction::SPUT_BOOLEAN:
    case Instruction::SPUT_BYTE:
    case Instruction::SPUT_CHAR:
    case Instruction::SPUT_SHORT:
      genSput(cUnit, vB, rlSrc[0], false, false);
      break;

    case Instruction::SPUT_WIDE:
      genSput(cUnit, vB, rlSrc[0], true, false);
      break;

    case Instruction::INVOKE_STATIC_RANGE:
      genInvoke(cUnit, oatNewCallInfo(cUnit, bb, mir, kStatic, true));
      break;
    case Instruction::INVOKE_STATIC:
      genInvoke(cUnit, oatNewCallInfo(cUnit, bb, mir, kStatic, false));
      break;

    case Instruction::INVOKE_DIRECT:
      genInvoke(cUnit, oatNewCallInfo(cUnit, bb, mir, kDirect, false));
      break;
    case Instruction::INVOKE_DIRECT_RANGE:
      genInvoke(cUnit, oatNewCallInfo(cUnit, bb, mir, kDirect, true));
      break;

    case Instruction::INVOKE_VIRTUAL:
      genInvoke(cUnit, oatNewCallInfo(cUnit, bb, mir, kVirtual, false));
      break;
    case Instruction::INVOKE_VIRTUAL_RANGE:
      genInvoke(cUnit, oatNewCallInfo(cUnit, bb, mir, kVirtual, true));
      break;

    case Instruction::INVOKE_SUPER:
      genInvoke(cUnit, oatNewCallInfo(cUnit, bb, mir, kSuper, false));
      break;
    case Instruction::INVOKE_SUPER_RANGE:
      genInvoke(cUnit, oatNewCallInfo(cUnit, bb, mir, kSuper, true));
      break;

    case Instruction::INVOKE_INTERFACE:
      genInvoke(cUnit, oatNewCallInfo(cUnit, bb, mir, kInterface, false));
      break;
    case Instruction::INVOKE_INTERFACE_RANGE:
      genInvoke(cUnit, oatNewCallInfo(cUnit, bb, mir, kInterface, true));
      break;

    case Instruction::NEG_INT:
    case Instruction::NOT_INT:
      res = genArithOpInt(cUnit, opcode, rlDest, rlSrc[0], rlSrc[0]);
      break;

    case Instruction::NEG_LONG:
    case Instruction::NOT_LONG:
      res = genArithOpLong(cUnit, opcode, rlDest, rlSrc[0], rlSrc[0]);
      break;

    case Instruction::NEG_FLOAT:
      res = genArithOpFloat(cUnit, opcode, rlDest, rlSrc[0], rlSrc[0]);
      break;

    case Instruction::NEG_DOUBLE:
      res = genArithOpDouble(cUnit, opcode, rlDest, rlSrc[0], rlSrc[0]);
      break;

    case Instruction::INT_TO_LONG:
      genIntToLong(cUnit, rlDest, rlSrc[0]);
      break;

    case Instruction::LONG_TO_INT:
      rlSrc[0] = oatUpdateLocWide(cUnit, rlSrc[0]);
      rlSrc[0] = oatWideToNarrow(cUnit, rlSrc[0]);
      storeValue(cUnit, rlDest, rlSrc[0]);
      break;

    case Instruction::INT_TO_BYTE:
    case Instruction::INT_TO_SHORT:
    case Instruction::INT_TO_CHAR:
      genIntNarrowing(cUnit, opcode, rlDest, rlSrc[0]);
      break;

    case Instruction::INT_TO_FLOAT:
    case Instruction::INT_TO_DOUBLE:
    case Instruction::LONG_TO_FLOAT:
    case Instruction::LONG_TO_DOUBLE:
    case Instruction::FLOAT_TO_INT:
    case Instruction::FLOAT_TO_LONG:
    case Instruction::FLOAT_TO_DOUBLE:
    case Instruction::DOUBLE_TO_INT:
    case Instruction::DOUBLE_TO_LONG:
    case Instruction::DOUBLE_TO_FLOAT:
      genConversion(cUnit, opcode, rlDest, rlSrc[0]);
      break;

    case Instruction::ADD_INT:
    case Instruction::SUB_INT:
    case Instruction::MUL_INT:
    case Instruction::DIV_INT:
    case Instruction::REM_INT:
    case Instruction::AND_INT:
    case Instruction::OR_INT:
    case Instruction::XOR_INT:
    case Instruction::SHL_INT:
    case Instruction::SHR_INT:
    case Instruction::USHR_INT:
    case Instruction::ADD_INT_2ADDR:
    case Instruction::SUB_INT_2ADDR:
    case Instruction::MUL_INT_2ADDR:
    case Instruction::DIV_INT_2ADDR:
    case Instruction::REM_INT_2ADDR:
    case Instruction::AND_INT_2ADDR:
    case Instruction::OR_INT_2ADDR:
    case Instruction::XOR_INT_2ADDR:
    case Instruction::SHL_INT_2ADDR:
    case Instruction::SHR_INT_2ADDR:
    case Instruction::USHR_INT_2ADDR:
      genArithOpInt(cUnit, opcode, rlDest, rlSrc[0], rlSrc[1]);
      break;

    case Instruction::ADD_LONG:
    case Instruction::SUB_LONG:
    case Instruction::MUL_LONG:
    case Instruction::DIV_LONG:
    case Instruction::REM_LONG:
    case Instruction::AND_LONG:
    case Instruction::OR_LONG:
    case Instruction::XOR_LONG:
    case Instruction::ADD_LONG_2ADDR:
    case Instruction::SUB_LONG_2ADDR:
    case Instruction::MUL_LONG_2ADDR:
    case Instruction::DIV_LONG_2ADDR:
    case Instruction::REM_LONG_2ADDR:
    case Instruction::AND_LONG_2ADDR:
    case Instruction::OR_LONG_2ADDR:
    case Instruction::XOR_LONG_2ADDR:
      genArithOpLong(cUnit, opcode, rlDest, rlSrc[0], rlSrc[1]);
      break;

    case Instruction::SHL_LONG:
    case Instruction::SHR_LONG:
    case Instruction::USHR_LONG:
    case Instruction::SHL_LONG_2ADDR:
    case Instruction::SHR_LONG_2ADDR:
    case Instruction::USHR_LONG_2ADDR:
      genShiftOpLong(cUnit, opcode, rlDest, rlSrc[0], rlSrc[1]);
      break;

    case Instruction::ADD_FLOAT:
    case Instruction::SUB_FLOAT:
    case Instruction::MUL_FLOAT:
    case Instruction::DIV_FLOAT:
    case Instruction::REM_FLOAT:
    case Instruction::ADD_FLOAT_2ADDR:
    case Instruction::SUB_FLOAT_2ADDR:
    case Instruction::MUL_FLOAT_2ADDR:
    case Instruction::DIV_FLOAT_2ADDR:
    case Instruction::REM_FLOAT_2ADDR:
      genArithOpFloat(cUnit, opcode, rlDest, rlSrc[0], rlSrc[1]);
      break;

    case Instruction::ADD_DOUBLE:
    case Instruction::SUB_DOUBLE:
    case Instruction::MUL_DOUBLE:
    case Instruction::DIV_DOUBLE:
    case Instruction::REM_DOUBLE:
    case Instruction::ADD_DOUBLE_2ADDR:
    case Instruction::SUB_DOUBLE_2ADDR:
    case Instruction::MUL_DOUBLE_2ADDR:
    case Instruction::DIV_DOUBLE_2ADDR:
    case Instruction::REM_DOUBLE_2ADDR:
      genArithOpDouble(cUnit, opcode, rlDest, rlSrc[0], rlSrc[1]);
      break;

    case Instruction::RSUB_INT:
    case Instruction::ADD_INT_LIT16:
    case Instruction::MUL_INT_LIT16:
    case Instruction::DIV_INT_LIT16:
    case Instruction::REM_INT_LIT16:
    case Instruction::AND_INT_LIT16:
    case Instruction::OR_INT_LIT16:
    case Instruction::XOR_INT_LIT16:
    case Instruction::ADD_INT_LIT8:
    case Instruction::RSUB_INT_LIT8:
    case Instruction::MUL_INT_LIT8:
    case Instruction::DIV_INT_LIT8:
    case Instruction::REM_INT_LIT8:
    case Instruction::AND_INT_LIT8:
    case Instruction::OR_INT_LIT8:
    case Instruction::XOR_INT_LIT8:
    case Instruction::SHL_INT_LIT8:
    case Instruction::SHR_INT_LIT8:
    case Instruction::USHR_INT_LIT8:
      genArithOpIntLit(cUnit, opcode, rlDest, rlSrc[0], vC);
      break;

    default:
      res = true;
  }
  return res;
}

const char* extendedMIROpNames[kMirOpLast - kMirOpFirst] = {
  "kMirOpPhi",
  "kMirOpCopy",
  "kMirFusedCmplFloat",
  "kMirFusedCmpgFloat",
  "kMirFusedCmplDouble",
  "kMirFusedCmpgDouble",
  "kMirFusedCmpLong",
  "kMirNop",
  "kMirOpNullCheck",
  "kMirOpRangeCheck",
  "kMirOpDivZeroCheck",
  "kMirOpCheck",
};

/* Extended MIR instructions like PHI */
void handleExtendedMethodMIR(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir)
{
  int opOffset = mir->dalvikInsn.opcode - kMirOpFirst;
  char* msg = NULL;
  if (cUnit->printMe) {
    msg = (char*)oatNew(cUnit, strlen(extendedMIROpNames[opOffset]) + 1,
                        false, kAllocDebugInfo);
    strcpy(msg, extendedMIROpNames[opOffset]);
  }
  LIR* op = newLIR1(cUnit, kPseudoExtended, (int) msg);

  switch ((ExtendedMIROpcode)mir->dalvikInsn.opcode) {
    case kMirOpPhi: {
      char* ssaString = NULL;
      if (cUnit->printMe) {
        ssaString = oatGetSSAString(cUnit, mir->ssaRep);
      }
      op->flags.isNop = true;
      newLIR1(cUnit, kPseudoSSARep, (int) ssaString);
      break;
    }
    case kMirOpCopy: {
      RegLocation rlSrc = oatGetSrc(cUnit, mir, 0);
      RegLocation rlDest = oatGetDest(cUnit, mir);
      storeValue(cUnit, rlDest, rlSrc);
      break;
    }
    case kMirOpFusedCmplFloat:
      genFusedFPCmpBranch(cUnit, bb, mir, false /*gt bias*/, false /*double*/);
      break;
    case kMirOpFusedCmpgFloat:
      genFusedFPCmpBranch(cUnit, bb, mir, true /*gt bias*/, false /*double*/);
      break;
    case kMirOpFusedCmplDouble:
      genFusedFPCmpBranch(cUnit, bb, mir, false /*gt bias*/, true /*double*/);
      break;
    case kMirOpFusedCmpgDouble:
      genFusedFPCmpBranch(cUnit, bb, mir, true /*gt bias*/, true /*double*/);
      break;
    case kMirOpFusedCmpLong:
      genFusedLongCmpBranch(cUnit, bb, mir);
      break;
    default:
      break;
  }
}

/* Handle the content in each basic block */
bool methodBlockCodeGen(CompilationUnit* cUnit, BasicBlock* bb)
{
  if (bb->blockType == kDead) return false;
  cUnit->currentDalvikOffset = bb->startOffset;
  MIR* mir;
  LIR* labelList = cUnit->blockLabelList;
  int blockId = bb->id;

  cUnit->curBlock = bb;
  labelList[blockId].operands[0] = bb->startOffset;

  /* Insert the block label */
  labelList[blockId].opcode = kPseudoNormalBlockLabel;
  oatAppendLIR(cUnit, (LIR*) &labelList[blockId]);

  LIR* headLIR = NULL;

  /* If this is a catch block, export the start address */
  if (bb->catchEntry) {
    headLIR = newLIR0(cUnit, kPseudoExportedPC);
  }

  /* Free temp registers and reset redundant store tracking */
  oatResetRegPool(cUnit);
  oatResetDefTracking(cUnit);

  oatClobberAllRegs(cUnit);


  if (bb->blockType == kEntryBlock) {
    int startVReg = cUnit->numDalvikRegisters - cUnit->numIns;
    genEntrySequence(cUnit, &cUnit->regLocation[startVReg],
                     cUnit->regLocation[cUnit->methodSReg]);
  } else if (bb->blockType == kExitBlock) {
    genExitSequence(cUnit);
  }

  for (mir = bb->firstMIRInsn; mir; mir = mir->next) {
    oatResetRegPool(cUnit);
    if (cUnit->disableOpt & (1 << kTrackLiveTemps)) {
      oatClobberAllRegs(cUnit);
    }

    if (cUnit->disableOpt & (1 << kSuppressLoads)) {
      oatResetDefTracking(cUnit);
    }

#ifndef NDEBUG
    /* Reset temp tracking sanity check */
    cUnit->liveSReg = INVALID_SREG;
#endif

    cUnit->currentDalvikOffset = mir->offset;
    int opcode = mir->dalvikInsn.opcode;
    LIR* boundaryLIR;

    /* Mark the beginning of a Dalvik instruction for line tracking */
    char* instStr = cUnit->printMe ?
       oatGetDalvikDisassembly(cUnit, mir->dalvikInsn, "") : NULL;
    boundaryLIR = markBoundary(cUnit, mir->offset, instStr);
    /* Remember the first LIR for this block */
    if (headLIR == NULL) {
      headLIR = boundaryLIR;
      /* Set the first boundaryLIR as a scheduling barrier */
      headLIR->defMask = ENCODE_ALL;
    }

    /* Don't generate the SSA annotation unless verbose mode is on */
    if (cUnit->printMe && mir->ssaRep) {
      char* ssaString = oatGetSSAString(cUnit, mir->ssaRep);
      newLIR1(cUnit, kPseudoSSARep, (int) ssaString);
    }

    if (opcode == kMirOpCheck) {
      // Combine check and work halves of throwing instruction.
      MIR* workHalf = mir->meta.throwInsn;
      mir->dalvikInsn.opcode = workHalf->dalvikInsn.opcode;
      opcode = workHalf->dalvikInsn.opcode;
      SSARepresentation* ssaRep = workHalf->ssaRep;
      workHalf->ssaRep = mir->ssaRep;
      mir->ssaRep = ssaRep;
      workHalf->dalvikInsn.opcode = static_cast<Instruction::Code>(kMirOpNop);
    }

    if (opcode >= kMirOpFirst) {
      handleExtendedMethodMIR(cUnit, bb, mir);
      continue;
    }

    bool notHandled = compileDalvikInstruction(cUnit, mir, bb, labelList);
    if (notHandled) {
      LOG(FATAL) << StringPrintf("%#06x: Opcode %#x (%s)",
                                 mir->offset, opcode,
                                 Instruction::Name(mir->dalvikInsn.opcode));
    }
  }

  if (headLIR) {
    /*
     * Eliminate redundant loads/stores and delay stores into later
     * slots
     */
    oatApplyLocalOptimizations(cUnit, (LIR*) headLIR, cUnit->lastLIRInsn);

    /*
     * Generate an unconditional branch to the fallthrough block.
     */
    if (bb->fallThrough) {
      opUnconditionalBranch(cUnit, &labelList[bb->fallThrough->id]);
    }
  }
  return false;
}

/* Set basic block labels */
bool labelBlocks(CompilationUnit* cUnit, BasicBlock* bb)
{
  LIR* labelList = cUnit->blockLabelList;
  int blockId = bb->id;

  cUnit->curBlock = bb;
  labelList[blockId].operands[0] = bb->startOffset;

  /* Insert the block label */
  labelList[blockId].opcode = kPseudoNormalBlockLabel;
  return false;
}

void oatSpecialMIR2LIR(CompilationUnit* cUnit, SpecialCaseHandler specialCase)
{
  /* Find the first DalvikByteCode block */
  int numReachableBlocks = cUnit->numReachableBlocks;
  const GrowableList *blockList = &cUnit->blockList;
  BasicBlock*bb = NULL;
  for (int idx = 0; idx < numReachableBlocks; idx++) {
    int dfsIndex = cUnit->dfsOrder.elemList[idx];
    bb = (BasicBlock*)oatGrowableListGetElement(blockList, dfsIndex);
    if (bb->blockType == kDalvikByteCode) {
      break;
    }
  }
  if (bb == NULL) {
    return;
  }
  DCHECK_EQ(bb->startOffset, 0);
  DCHECK(bb->firstMIRInsn != NULL);

  /* Get the first instruction */
  MIR* mir = bb->firstMIRInsn;

  /* Free temp registers and reset redundant store tracking */
  oatResetRegPool(cUnit);
  oatResetDefTracking(cUnit);
  oatClobberAllRegs(cUnit);

  genSpecialCase(cUnit, bb, mir, specialCase);
}

void oatMethodMIR2LIR(CompilationUnit* cUnit)
{
  /* Used to hold the labels of each block */
  cUnit->blockLabelList =
      (LIR*) oatNew(cUnit, sizeof(LIR) * cUnit->numBlocks, true, kAllocLIR);

  oatDataFlowAnalysisDispatcher(cUnit, methodBlockCodeGen,
                                kPreOrderDFSTraversal, false /* Iterative */);

  handleSuspendLaunchpads(cUnit);

  handleThrowLaunchpads(cUnit);

  handleIntrinsicLaunchpads(cUnit);

  if (!(cUnit->disableOpt & (1 << kSafeOptimizations))) {
    removeRedundantBranches(cUnit);
  }
}

}  // namespace art
