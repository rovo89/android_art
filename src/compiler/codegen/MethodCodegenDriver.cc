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

namespace art {

#define DISPLAY_MISSING_TARGETS (cUnit->enableDebug & \
    (1 << kDebugDisplayMissingTargets))

const RegLocation badLoc = {kLocDalvikFrame, 0, 0, 0, 0, 0, 0,
                            INVALID_REG, INVALID_REG, INVALID_SREG};

/* Mark register usage state and return long retloc */
RegLocation getRetLocWide(CompilationUnit* cUnit)
{
    RegLocation res = LOC_C_RETURN_WIDE;
    oatLockTemp(cUnit, res.lowReg);
    oatLockTemp(cUnit, res.highReg);
    oatMarkPair(cUnit, res.lowReg, res.highReg);
    return res;
}

RegLocation getRetLoc(CompilationUnit* cUnit)
{
    RegLocation res = LOC_C_RETURN;
    oatLockTemp(cUnit, res.lowReg);
    return res;
}

void genInvoke(CompilationUnit* cUnit, MIR* mir, InvokeType type, bool isRange)
{
    DecodedInstruction* dInsn = &mir->dalvikInsn;
    int callState = 0;
    LIR* nullCk;
    LIR** pNullCk = NULL;
    NextCallInsn nextCallInsn;
    oatFlushAllRegs(cUnit);    /* Everything to home location */
    // Explicit register usage
    oatLockCallTemps(cUnit);

    OatCompilationUnit mUnit(cUnit->class_loader, cUnit->class_linker,
                             *cUnit->dex_file, *cUnit->dex_cache,
                             cUnit->code_item, cUnit->method_idx,
                             cUnit->access_flags);

    uint32_t dexMethodIdx = dInsn->vB;
    int vtableIdx;
    bool skipThis;
    bool fastPath =
        cUnit->compiler->ComputeInvokeInfo(dexMethodIdx, &mUnit, type,
                                           vtableIdx)
        && !SLOW_INVOKE_PATH;
    if (type == kInterface) {
      nextCallInsn = fastPath ? nextInterfaceCallInsn
                              : nextInterfaceCallInsnWithAccessCheck;
      skipThis = false;
    } else if (type == kDirect) {
      if (fastPath) {
        pNullCk = &nullCk;
      }
      nextCallInsn = fastPath ? nextSDCallInsn : nextDirectCallInsnSP;
      skipThis = false;
    } else if (type == kStatic) {
      nextCallInsn = fastPath ? nextSDCallInsn : nextStaticCallInsnSP;
      skipThis = false;
    } else if (type == kSuper) {
      nextCallInsn = fastPath ? nextSuperCallInsn : nextSuperCallInsnSP;
      skipThis = fastPath;
    } else {
      DCHECK_EQ(type, kVirtual);
      nextCallInsn = fastPath ? nextVCallInsn : nextVCallInsnSP;
      skipThis = fastPath;
    }
    if (!isRange) {
        callState = genDalvikArgsNoRange(cUnit, mir, dInsn, callState, pNullCk,
                                         nextCallInsn, dexMethodIdx,
                                         vtableIdx, skipThis);
    } else {
        callState = genDalvikArgsRange(cUnit, mir, dInsn, callState, pNullCk,
                                       nextCallInsn, dexMethodIdx, vtableIdx,
                                       skipThis);
    }
    // Finish up any of the call sequence not interleaved in arg loading
    while (callState >= 0) {
        callState = nextCallInsn(cUnit, mir, callState, dexMethodIdx,
                                 vtableIdx);
    }
    if (DISPLAY_MISSING_TARGETS) {
        genShowTarget(cUnit);
    }
#if defined(TARGET_MIPS)
    UNIMPLEMENTED(WARNING) << "Need to handle common target register";
#else
    opReg(cUnit, kOpBlx, rLR);
#endif
    oatClobberCalleeSave(cUnit);
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
    Opcode opcode = mir->dalvikInsn.opcode;

    /* Prep Src and Dest locations */
    int nextSreg = 0;
    int nextLoc = 0;
    int attrs = oatDataFlowAttributes[opcode];
    rlSrc[0] = rlSrc[1] = rlSrc[2] = badLoc;
    if (attrs & DF_UA) {
        rlSrc[nextLoc++] = oatGetSrc(cUnit, mir, nextSreg);
        nextSreg++;
    } else if (attrs & DF_UA_WIDE) {
        rlSrc[nextLoc++] = oatGetSrcWide(cUnit, mir, nextSreg,
                                                 nextSreg + 1);
        nextSreg+= 2;
    }
    if (attrs & DF_UB) {
        rlSrc[nextLoc++] = oatGetSrc(cUnit, mir, nextSreg);
        nextSreg++;
    } else if (attrs & DF_UB_WIDE) {
        rlSrc[nextLoc++] = oatGetSrcWide(cUnit, mir, nextSreg,
                                                 nextSreg + 1);
        nextSreg+= 2;
    }
    if (attrs & DF_UC) {
        rlSrc[nextLoc++] = oatGetSrc(cUnit, mir, nextSreg);
    } else if (attrs & DF_UC_WIDE) {
        rlSrc[nextLoc++] = oatGetSrcWide(cUnit, mir, nextSreg,
                                                 nextSreg + 1);
    }
    if (attrs & DF_DA) {
        rlDest = oatGetDest(cUnit, mir, 0);
    } else if (attrs & DF_DA_WIDE) {
        rlDest = oatGetDestWide(cUnit, mir, 0, 1);
    }

    switch(opcode) {
        case OP_NOP:
            break;

        case OP_MOVE_EXCEPTION:
            int exOffset;
            int resetReg;
            exOffset = Thread::ExceptionOffset().Int32Value();
            resetReg = oatAllocTemp(cUnit);
            rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
            loadWordDisp(cUnit, rSELF, exOffset, rlResult.lowReg);
            loadConstant(cUnit, resetReg, 0);
            storeWordDisp(cUnit, rSELF, exOffset, resetReg);
            storeValue(cUnit, rlDest, rlResult);
            break;

        case OP_RETURN_VOID:
            genSuspendTest(cUnit, mir);
            break;

        case OP_RETURN:
        case OP_RETURN_OBJECT:
            genSuspendTest(cUnit, mir);
            storeValue(cUnit, getRetLoc(cUnit), rlSrc[0]);
            break;

        case OP_RETURN_WIDE:
            genSuspendTest(cUnit, mir);
            storeValueWide(cUnit, getRetLocWide(cUnit), rlSrc[0]);
            break;

        case OP_MOVE_RESULT_WIDE:
            if (mir->optimizationFlags & MIR_INLINED)
                break;  // Nop - combined w/ previous invoke
            storeValueWide(cUnit, rlDest, getRetLocWide(cUnit));
            break;

        case OP_MOVE_RESULT:
        case OP_MOVE_RESULT_OBJECT:
            if (mir->optimizationFlags & MIR_INLINED)
                break;  // Nop - combined w/ previous invoke
            storeValue(cUnit, rlDest, getRetLoc(cUnit));
            break;

        case OP_MOVE:
        case OP_MOVE_OBJECT:
        case OP_MOVE_16:
        case OP_MOVE_OBJECT_16:
        case OP_MOVE_FROM16:
        case OP_MOVE_OBJECT_FROM16:
            storeValue(cUnit, rlDest, rlSrc[0]);
            break;

        case OP_MOVE_WIDE:
        case OP_MOVE_WIDE_16:
        case OP_MOVE_WIDE_FROM16:
            storeValueWide(cUnit, rlDest, rlSrc[0]);
            break;

        case OP_CONST:
        case OP_CONST_4:
        case OP_CONST_16:
            rlResult = oatEvalLoc(cUnit, rlDest, kAnyReg, true);
            loadConstantNoClobber(cUnit, rlResult.lowReg, mir->dalvikInsn.vB);
            storeValue(cUnit, rlDest, rlResult);
            break;

        case OP_CONST_HIGH16:
            rlResult = oatEvalLoc(cUnit, rlDest, kAnyReg, true);
            loadConstantNoClobber(cUnit, rlResult.lowReg,
                                  mir->dalvikInsn.vB << 16);
            storeValue(cUnit, rlDest, rlResult);
            break;

        case OP_CONST_WIDE_16:
        case OP_CONST_WIDE_32:
            rlResult = oatEvalLoc(cUnit, rlDest, kAnyReg, true);
            loadConstantValueWide(cUnit, rlResult.lowReg, rlResult.highReg,
                                  mir->dalvikInsn.vB,
                                  (mir->dalvikInsn.vB & 0x80000000) ? -1 : 0);
            storeValueWide(cUnit, rlDest, rlResult);
            break;

        case OP_CONST_WIDE:
            rlResult = oatEvalLoc(cUnit, rlDest, kAnyReg, true);
            loadConstantValueWide(cUnit, rlResult.lowReg, rlResult.highReg,
                          mir->dalvikInsn.vB_wide & 0xffffffff,
                          (mir->dalvikInsn.vB_wide >> 32) & 0xffffffff);
            storeValueWide(cUnit, rlDest, rlResult);
            break;

        case OP_CONST_WIDE_HIGH16:
            rlResult = oatEvalLoc(cUnit, rlDest, kAnyReg, true);
            loadConstantValueWide(cUnit, rlResult.lowReg, rlResult.highReg,
                                  0, mir->dalvikInsn.vB << 16);
            storeValueWide(cUnit, rlDest, rlResult);
            break;

        case OP_MONITOR_ENTER:
            genMonitorEnter(cUnit, mir, rlSrc[0]);
            break;

        case OP_MONITOR_EXIT:
            genMonitorExit(cUnit, mir, rlSrc[0]);
            break;

        case OP_CHECK_CAST:
            genCheckCast(cUnit, mir, rlSrc[0]);
            break;

        case OP_INSTANCE_OF:
            genInstanceof(cUnit, mir, rlDest, rlSrc[0]);
            break;

        case OP_NEW_INSTANCE:
            genNewInstance(cUnit, mir, rlDest);
            break;

        case OP_THROW:
            genThrow(cUnit, mir, rlSrc[0]);
            break;

        case OP_THROW_VERIFICATION_ERROR:
            genThrowVerificationError(cUnit, mir);
            break;

        case OP_ARRAY_LENGTH:
            int lenOffset;
            lenOffset = Array::LengthOffset().Int32Value();
            rlSrc[0] = loadValue(cUnit, rlSrc[0], kCoreReg);
            genNullCheck(cUnit, rlSrc[0].sRegLow, rlSrc[0].lowReg, mir);
            rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
            loadWordDisp(cUnit, rlSrc[0].lowReg, lenOffset,
                         rlResult.lowReg);
            storeValue(cUnit, rlDest, rlResult);
            break;

        case OP_CONST_STRING:
        case OP_CONST_STRING_JUMBO:
            genConstString(cUnit, mir, rlDest, rlSrc[0]);
            break;

        case OP_CONST_CLASS:
            genConstClass(cUnit, mir, rlDest, rlSrc[0]);
            break;

        case OP_FILL_ARRAY_DATA:
            genFillArrayData(cUnit, mir, rlSrc[0]);
            break;

        case OP_FILLED_NEW_ARRAY:
            genFilledNewArray(cUnit, mir, false /* not range */);
            break;

        case OP_FILLED_NEW_ARRAY_RANGE:
            genFilledNewArray(cUnit, mir, true /* range */);
            break;

        case OP_NEW_ARRAY:
            genNewArray(cUnit, mir, rlDest, rlSrc[0]);
            break;

        case OP_GOTO:
        case OP_GOTO_16:
        case OP_GOTO_32:
            if (bb->taken->startOffset <= mir->offset) {
                genSuspendTest(cUnit, mir);
            }
            opUnconditionalBranch(cUnit, &labelList[bb->taken->id]);
            break;

        case OP_PACKED_SWITCH:
            genPackedSwitch(cUnit, mir, rlSrc[0]);
            break;

        case OP_SPARSE_SWITCH:
            genSparseSwitch(cUnit, mir, rlSrc[0]);
            break;

        case OP_CMPL_FLOAT:
        case OP_CMPG_FLOAT:
        case OP_CMPL_DOUBLE:
        case OP_CMPG_DOUBLE:
            res = genCmpFP(cUnit, mir, rlDest, rlSrc[0], rlSrc[1]);
            break;

        case OP_CMP_LONG:
            genCmpLong(cUnit, mir, rlDest, rlSrc[0], rlSrc[1]);
            break;

        case OP_IF_EQ:
        case OP_IF_NE:
        case OP_IF_LT:
        case OP_IF_GE:
        case OP_IF_GT:
        case OP_IF_LE: {
            bool backwardBranch;
            backwardBranch = (bb->taken->startOffset <= mir->offset);
            if (backwardBranch) {
                genSuspendTest(cUnit, mir);
            }
            genCompareAndBranch(cUnit, bb, mir, rlSrc[0], rlSrc[1], labelList);
            break;
            }

        case OP_IF_EQZ:
        case OP_IF_NEZ:
        case OP_IF_LTZ:
        case OP_IF_GEZ:
        case OP_IF_GTZ:
        case OP_IF_LEZ: {
            bool backwardBranch;
            backwardBranch = (bb->taken->startOffset <= mir->offset);
            if (backwardBranch) {
                genSuspendTest(cUnit, mir);
            }
            genCompareZeroAndBranch(cUnit, bb, mir, rlSrc[0], labelList);
            break;
            }

      case OP_AGET_WIDE:
            genArrayGet(cUnit, mir, kLong, rlSrc[0], rlSrc[1], rlDest, 3);
            break;
        case OP_AGET:
        case OP_AGET_OBJECT:
            genArrayGet(cUnit, mir, kWord, rlSrc[0], rlSrc[1], rlDest, 2);
            break;
        case OP_AGET_BOOLEAN:
            genArrayGet(cUnit, mir, kUnsignedByte, rlSrc[0], rlSrc[1],
                        rlDest, 0);
            break;
        case OP_AGET_BYTE:
            genArrayGet(cUnit, mir, kSignedByte, rlSrc[0], rlSrc[1], rlDest, 0);
            break;
        case OP_AGET_CHAR:
            genArrayGet(cUnit, mir, kUnsignedHalf, rlSrc[0], rlSrc[1],
                        rlDest, 1);
            break;
        case OP_AGET_SHORT:
            genArrayGet(cUnit, mir, kSignedHalf, rlSrc[0], rlSrc[1], rlDest, 1);
            break;
        case OP_APUT_WIDE:
            genArrayPut(cUnit, mir, kLong, rlSrc[1], rlSrc[2], rlSrc[0], 3);
            break;
        case OP_APUT:
            genArrayPut(cUnit, mir, kWord, rlSrc[1], rlSrc[2], rlSrc[0], 2);
            break;
        case OP_APUT_OBJECT:
            genArrayObjPut(cUnit, mir, rlSrc[1], rlSrc[2], rlSrc[0], 2);
            break;
        case OP_APUT_SHORT:
        case OP_APUT_CHAR:
            genArrayPut(cUnit, mir, kUnsignedHalf, rlSrc[1], rlSrc[2],
                        rlSrc[0], 1);
            break;
        case OP_APUT_BYTE:
        case OP_APUT_BOOLEAN:
            genArrayPut(cUnit, mir, kUnsignedByte, rlSrc[1], rlSrc[2],
                        rlSrc[0], 0);
            break;

        case OP_IGET_OBJECT:
        case OP_IGET_OBJECT_VOLATILE:
            genIGet(cUnit, mir, kWord, rlDest, rlSrc[0], false, true);
            break;

        case OP_IGET_WIDE:
        case OP_IGET_WIDE_VOLATILE:
            genIGet(cUnit, mir, kLong, rlDest, rlSrc[0], true, false);
            break;

        case OP_IGET:
        case OP_IGET_VOLATILE:
            genIGet(cUnit, mir, kWord, rlDest, rlSrc[0], false, false);
            break;

        case OP_IGET_CHAR:
            genIGet(cUnit, mir, kUnsignedHalf, rlDest, rlSrc[0], false, false);
            break;

        case OP_IGET_SHORT:
            genIGet(cUnit, mir, kSignedHalf, rlDest, rlSrc[0], false, false);
            break;

        case OP_IGET_BOOLEAN:
        case OP_IGET_BYTE:
            genIGet(cUnit, mir, kUnsignedByte, rlDest, rlSrc[0], false, false);
            break;

        case OP_IPUT_WIDE:
        case OP_IPUT_WIDE_VOLATILE:
            genIPut(cUnit, mir, kLong, rlSrc[0], rlSrc[1], true, false);
            break;

        case OP_IPUT_OBJECT:
        case OP_IPUT_OBJECT_VOLATILE:
            genIPut(cUnit, mir, kWord, rlSrc[0], rlSrc[1], false, true);
            break;

        case OP_IPUT:
        case OP_IPUT_VOLATILE:
            genIPut(cUnit, mir, kWord, rlSrc[0], rlSrc[1], false, false);
            break;

        case OP_IPUT_BOOLEAN:
        case OP_IPUT_BYTE:
            genIPut(cUnit, mir, kUnsignedByte, rlSrc[0], rlSrc[1], false, false);
            break;

        case OP_IPUT_CHAR:
            genIPut(cUnit, mir, kUnsignedHalf, rlSrc[0], rlSrc[1], false, false);
            break;

        case OP_IPUT_SHORT:
            genIPut(cUnit, mir, kSignedHalf, rlSrc[0], rlSrc[1], false, false);
            break;

        case OP_SGET_OBJECT:
            genSget(cUnit, mir, rlDest, false, true);
            break;
        case OP_SGET:
        case OP_SGET_BOOLEAN:
        case OP_SGET_BYTE:
        case OP_SGET_CHAR:
        case OP_SGET_SHORT:
            genSget(cUnit, mir, rlDest, false, false);
            break;

        case OP_SGET_WIDE:
            genSget(cUnit, mir, rlDest, true, false);
            break;

        case OP_SPUT_OBJECT:
            genSput(cUnit, mir, rlSrc[0], false, true);
            break;

        case OP_SPUT:
        case OP_SPUT_BOOLEAN:
        case OP_SPUT_BYTE:
        case OP_SPUT_CHAR:
        case OP_SPUT_SHORT:
            genSput(cUnit, mir, rlSrc[0], false, false);
            break;

        case OP_SPUT_WIDE:
            genSput(cUnit, mir, rlSrc[0], true, false);
            break;

        case OP_INVOKE_STATIC_RANGE:
            genInvoke(cUnit, mir, kStatic, true /*range*/);
            break;
        case OP_INVOKE_STATIC:
            genInvoke(cUnit, mir, kStatic, false /*range*/);
            break;

        case OP_INVOKE_DIRECT:
            genInvoke(cUnit, mir, kDirect, false /*range*/);
            break;
        case OP_INVOKE_DIRECT_RANGE:
            genInvoke(cUnit, mir, kDirect, true /*range*/);
            break;

        case OP_INVOKE_VIRTUAL:
            genInvoke(cUnit, mir, kVirtual, false /*range*/);
            break;
        case OP_INVOKE_VIRTUAL_RANGE:
            genInvoke(cUnit, mir, kVirtual, true /*range*/);
            break;

        case OP_INVOKE_SUPER:
            genInvoke(cUnit, mir, kSuper, false /*range*/);
            break;
        case OP_INVOKE_SUPER_RANGE:
            genInvoke(cUnit, mir, kSuper, true /*range*/);
            break;

        case OP_INVOKE_INTERFACE:
            genInvoke(cUnit, mir, kInterface, false /*range*/);
            break;
        case OP_INVOKE_INTERFACE_RANGE:
            genInvoke(cUnit, mir, kInterface, true /*range*/);
            break;

        case OP_NEG_INT:
        case OP_NOT_INT:
            res = genArithOpInt(cUnit, mir, rlDest, rlSrc[0], rlSrc[0]);
            break;

        case OP_NEG_LONG:
        case OP_NOT_LONG:
            res = genArithOpLong(cUnit, mir, rlDest, rlSrc[0], rlSrc[0]);
            break;

        case OP_NEG_FLOAT:
            res = genArithOpFloat(cUnit, mir, rlDest, rlSrc[0], rlSrc[0]);
            break;

        case OP_NEG_DOUBLE:
            res = genArithOpDouble(cUnit, mir, rlDest, rlSrc[0], rlSrc[0]);
            break;

        case OP_INT_TO_LONG:
            genIntToLong(cUnit, mir, rlDest, rlSrc[0]);
            break;

        case OP_LONG_TO_INT:
            rlSrc[0] = oatUpdateLocWide(cUnit, rlSrc[0]);
            rlSrc[0] = oatWideToNarrow(cUnit, rlSrc[0]);
            storeValue(cUnit, rlDest, rlSrc[0]);
            break;

        case OP_INT_TO_BYTE:
        case OP_INT_TO_SHORT:
        case OP_INT_TO_CHAR:
            genIntNarrowing(cUnit, mir, rlDest, rlSrc[0]);
            break;

        case OP_INT_TO_FLOAT:
        case OP_INT_TO_DOUBLE:
        case OP_LONG_TO_FLOAT:
        case OP_LONG_TO_DOUBLE:
        case OP_FLOAT_TO_INT:
        case OP_FLOAT_TO_LONG:
        case OP_FLOAT_TO_DOUBLE:
        case OP_DOUBLE_TO_INT:
        case OP_DOUBLE_TO_LONG:
        case OP_DOUBLE_TO_FLOAT:
            genConversion(cUnit, mir);
            break;

        case OP_ADD_INT:
        case OP_SUB_INT:
        case OP_MUL_INT:
        case OP_DIV_INT:
        case OP_REM_INT:
        case OP_AND_INT:
        case OP_OR_INT:
        case OP_XOR_INT:
        case OP_SHL_INT:
        case OP_SHR_INT:
        case OP_USHR_INT:
        case OP_ADD_INT_2ADDR:
        case OP_SUB_INT_2ADDR:
        case OP_MUL_INT_2ADDR:
        case OP_DIV_INT_2ADDR:
        case OP_REM_INT_2ADDR:
        case OP_AND_INT_2ADDR:
        case OP_OR_INT_2ADDR:
        case OP_XOR_INT_2ADDR:
        case OP_SHL_INT_2ADDR:
        case OP_SHR_INT_2ADDR:
        case OP_USHR_INT_2ADDR:
            genArithOpInt(cUnit, mir, rlDest, rlSrc[0], rlSrc[1]);
            break;

        case OP_ADD_LONG:
        case OP_SUB_LONG:
        case OP_MUL_LONG:
        case OP_DIV_LONG:
        case OP_REM_LONG:
        case OP_AND_LONG:
        case OP_OR_LONG:
        case OP_XOR_LONG:
        case OP_ADD_LONG_2ADDR:
        case OP_SUB_LONG_2ADDR:
        case OP_MUL_LONG_2ADDR:
        case OP_DIV_LONG_2ADDR:
        case OP_REM_LONG_2ADDR:
        case OP_AND_LONG_2ADDR:
        case OP_OR_LONG_2ADDR:
        case OP_XOR_LONG_2ADDR:
            genArithOpLong(cUnit, mir, rlDest, rlSrc[0], rlSrc[1]);
            break;

        case OP_SHL_LONG:
        case OP_SHR_LONG:
        case OP_USHR_LONG:
        case OP_SHL_LONG_2ADDR:
        case OP_SHR_LONG_2ADDR:
        case OP_USHR_LONG_2ADDR:
            genShiftOpLong(cUnit,mir, rlDest, rlSrc[0], rlSrc[1]);
            break;

        case OP_ADD_FLOAT:
        case OP_SUB_FLOAT:
        case OP_MUL_FLOAT:
        case OP_DIV_FLOAT:
        case OP_REM_FLOAT:
        case OP_ADD_FLOAT_2ADDR:
        case OP_SUB_FLOAT_2ADDR:
        case OP_MUL_FLOAT_2ADDR:
        case OP_DIV_FLOAT_2ADDR:
        case OP_REM_FLOAT_2ADDR:
            genArithOpFloat(cUnit, mir, rlDest, rlSrc[0], rlSrc[1]);
            break;

        case OP_ADD_DOUBLE:
        case OP_SUB_DOUBLE:
        case OP_MUL_DOUBLE:
        case OP_DIV_DOUBLE:
        case OP_REM_DOUBLE:
        case OP_ADD_DOUBLE_2ADDR:
        case OP_SUB_DOUBLE_2ADDR:
        case OP_MUL_DOUBLE_2ADDR:
        case OP_DIV_DOUBLE_2ADDR:
        case OP_REM_DOUBLE_2ADDR:
            genArithOpDouble(cUnit, mir, rlDest, rlSrc[0], rlSrc[1]);
            break;

        case OP_RSUB_INT:
        case OP_ADD_INT_LIT16:
        case OP_MUL_INT_LIT16:
        case OP_DIV_INT_LIT16:
        case OP_REM_INT_LIT16:
        case OP_AND_INT_LIT16:
        case OP_OR_INT_LIT16:
        case OP_XOR_INT_LIT16:
        case OP_ADD_INT_LIT8:
        case OP_RSUB_INT_LIT8:
        case OP_MUL_INT_LIT8:
        case OP_DIV_INT_LIT8:
        case OP_REM_INT_LIT8:
        case OP_AND_INT_LIT8:
        case OP_OR_INT_LIT8:
        case OP_XOR_INT_LIT8:
        case OP_SHL_INT_LIT8:
        case OP_SHR_INT_LIT8:
        case OP_USHR_INT_LIT8:
            genArithOpIntLit(cUnit, mir, rlDest, rlSrc[0], mir->dalvikInsn.vC);
            break;

        default:
            res = true;
    }
    return res;
}

const char* extendedMIROpNames[kMirOpLast - kMirOpFirst] = {
    "kMirOpPhi",
    "kMirOpNullNRangeUpCheck",
    "kMirOpNullNRangeDownCheck",
    "kMirOpLowerBound",
    "kMirOpPunt",
    "kMirOpCheckInlinePrediction",
};

/* Extended MIR instructions like PHI */
void handleExtendedMethodMIR(CompilationUnit* cUnit, MIR* mir)
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
        default:
            break;
    }
}

/* Handle the content in each basic block */
bool methodBlockCodeGen(CompilationUnit* cUnit, BasicBlock* bb)
{
    MIR* mir;
    LIR* labelList = (LIR*) cUnit->blockLabelList;
    int blockId = bb->id;

    cUnit->curBlock = bb;
    labelList[blockId].operands[0] = bb->startOffset;

    /* Insert the block label */
    labelList[blockId].opcode = kPseudoNormalBlockLabel;
    oatAppendLIR(cUnit, (LIR*) &labelList[blockId]);

    /* Reset local optimization data on block boundaries */
    oatResetRegPool(cUnit);
    oatClobberAllRegs(cUnit);
    oatResetDefTracking(cUnit);

    LIR* headLIR = NULL;

    if (bb->blockType == kEntryBlock) {
        genEntrySequence(cUnit, bb);
    } else if (bb->blockType == kExitBlock) {
        genExitSequence(cUnit, bb);
    }

    for (mir = bb->firstMIRInsn; mir; mir = mir->next) {

        oatResetRegPool(cUnit);
        if (cUnit->disableOpt & (1 << kTrackLiveTemps)) {
            oatClobberAllRegs(cUnit);
        }

        if (cUnit->disableOpt & (1 << kSuppressLoads)) {
            oatResetDefTracking(cUnit);
        }

        if ((int)mir->dalvikInsn.opcode >= (int)kMirOpFirst) {
            handleExtendedMethodMIR(cUnit, mir);
            continue;
        }

        cUnit->currentDalvikOffset = mir->offset;

        Opcode dalvikOpcode = mir->dalvikInsn.opcode;
        InstructionFormat dalvikFormat =
            dexGetFormatFromOpcode(dalvikOpcode);

        LIR* boundaryLIR;

        /* Mark the beginning of a Dalvik instruction for line tracking */
        char* instStr = cUnit->printMe ?
           oatGetDalvikDisassembly(cUnit, &mir->dalvikInsn, "") : NULL;
        boundaryLIR = newLIR1(cUnit, kPseudoDalvikByteCodeBoundary,
                              (intptr_t) instStr);
        cUnit->boundaryMap.insert(std::make_pair(mir->offset,
                                 (LIR*)boundaryLIR));
        /* Remember the first LIR for this block */
        if (headLIR == NULL) {
            headLIR = boundaryLIR;
            /* Set the first boundaryLIR as a scheduling barrier */
            headLIR->defMask = ENCODE_ALL;
        }

        /* If we're compiling for the debugger, generate an update callout */
        if (cUnit->genDebugger) {
            genDebuggerUpdate(cUnit, mir->offset);
        }

        /* Don't generate the SSA annotation unless verbose mode is on */
        if (cUnit->printMe && mir->ssaRep) {
            char* ssaString = oatGetSSAString(cUnit, mir->ssaRep);
            newLIR1(cUnit, kPseudoSSARep, (int) ssaString);
        }

        bool notHandled = compileDalvikInstruction(cUnit, mir, bb, labelList);

        if (notHandled) {
            char buf[100];
            snprintf(buf, 100, "%#06x: Opcode %#x (%s) / Fmt %d not handled",
                 mir->offset,
                 dalvikOpcode, dexGetOpcodeName(dalvikOpcode),
                 dalvikFormat);
            LOG(FATAL) << buf;
        }
    }

    if (headLIR) {
        /*
         * Eliminate redundant loads/stores and delay stores into later
         * slots
         */
        oatApplyLocalOptimizations(cUnit, (LIR*) headLIR,
                                           cUnit->lastLIRInsn);

        /*
         * Generate an unconditional branch to the fallthrough block.
         */
        if (bb->fallThrough) {
            opUnconditionalBranch(cUnit,
                                  &labelList[bb->fallThrough->id]);
        }
    }
    return false;
}

void oatMethodMIR2LIR(CompilationUnit* cUnit)
{
    /* Used to hold the labels of each block */
    cUnit->blockLabelList =
        (void *) oatNew(cUnit, sizeof(LIR) * cUnit->numBlocks, true,
                        kAllocLIR);

    oatDataFlowAnalysisDispatcher(cUnit, methodBlockCodeGen,
                                  kPreOrderDFSTraversal, false /* Iterative */);
    handleSuspendLaunchpads(cUnit);

    handleThrowLaunchpads(cUnit);

    removeRedundantBranches(cUnit);
}

/* Needed by the ld/st optmizatons */
LIR* oatRegCopyNoInsert(CompilationUnit* cUnit, int rDest, int rSrc)
{
    return opRegCopyNoInsert(cUnit, rDest, rSrc);
}

/* Needed by the register allocator */
void oatRegCopy(CompilationUnit* cUnit, int rDest, int rSrc)
{
    opRegCopy(cUnit, rDest, rSrc);
}

/* Needed by the register allocator */
void oatRegCopyWide(CompilationUnit* cUnit, int destLo, int destHi,
                            int srcLo, int srcHi)
{
    opRegCopyWide(cUnit, destLo, destHi, srcLo, srcHi);
}

void oatFlushRegImpl(CompilationUnit* cUnit, int rBase,
                             int displacement, int rSrc, OpSize size)
{
    storeBaseDisp(cUnit, rBase, displacement, rSrc, size);
}

void oatFlushRegWideImpl(CompilationUnit* cUnit, int rBase,
                                 int displacement, int rSrcLo, int rSrcHi)
{
    storeBaseDispWide(cUnit, rBase, displacement, rSrcLo, rSrcHi);
}

}  // namespace art
