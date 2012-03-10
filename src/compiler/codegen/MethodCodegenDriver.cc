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
RegLocation oatGetReturnWide(CompilationUnit* cUnit)
{
    RegLocation res = LOC_C_RETURN_WIDE;
    oatClobber(cUnit, res.lowReg);
    oatClobber(cUnit, res.highReg);
    oatLockTemp(cUnit, res.lowReg);
    oatLockTemp(cUnit, res.highReg);
    oatMarkPair(cUnit, res.lowReg, res.highReg);
    return res;
}

RegLocation oatGetReturn(CompilationUnit* cUnit)
{
    RegLocation res = LOC_C_RETURN;
    oatClobber(cUnit, res.lowReg);
    if (cUnit->instructionSet == kMips) {
        oatMarkInUse(cUnit, res.lowReg);
    } else {
        oatLockTemp(cUnit, res.lowReg);
    }
    return res;
}

void genInvoke(CompilationUnit* cUnit, MIR* mir, InvokeType type, bool isRange)
{
#if defined(TARGET_X86)
    UNIMPLEMENTED(WARNING) << "genInvoke";
#else
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
    opReg(cUnit, kOpBlx, rINVOKE_TGT);
    oatClobberCalleeSave(cUnit);
#endif
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
        case Instruction::NOP:
            break;

        case Instruction::MOVE_EXCEPTION:
#if defined(TARGET_X86)
            UNIMPLEMENTED(WARNING) << "Instruction::MOVE_EXCEPTION";
#else
            int exOffset;
            int resetReg;
            exOffset = Thread::ExceptionOffset().Int32Value();
            resetReg = oatAllocTemp(cUnit);
            rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
            loadWordDisp(cUnit, rSELF, exOffset, rlResult.lowReg);
            loadConstant(cUnit, resetReg, 0);
            storeWordDisp(cUnit, rSELF, exOffset, resetReg);
            storeValue(cUnit, rlDest, rlResult);
#endif
            break;

        case Instruction::RETURN_VOID:
            if (!cUnit->attrs & METHOD_IS_LEAF) {
                genSuspendTest(cUnit, mir);
            }
            break;

        case Instruction::RETURN:
        case Instruction::RETURN_OBJECT:
            if (!cUnit->attrs & METHOD_IS_LEAF) {
                genSuspendTest(cUnit, mir);
            }
            storeValue(cUnit, oatGetReturn(cUnit), rlSrc[0]);
            break;

        case Instruction::RETURN_WIDE:
            if (!cUnit->attrs & METHOD_IS_LEAF) {
                genSuspendTest(cUnit, mir);
            }
            storeValueWide(cUnit, oatGetReturnWide(cUnit), rlSrc[0]);
            break;

        case Instruction::MOVE_RESULT_WIDE:
            if (mir->optimizationFlags & MIR_INLINED)
                break;  // Nop - combined w/ previous invoke
            storeValueWide(cUnit, rlDest, oatGetReturnWide(cUnit));
            break;

        case Instruction::MOVE_RESULT:
        case Instruction::MOVE_RESULT_OBJECT:
            if (mir->optimizationFlags & MIR_INLINED)
                break;  // Nop - combined w/ previous invoke
            storeValue(cUnit, rlDest, oatGetReturn(cUnit));
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
            loadConstantNoClobber(cUnit, rlResult.lowReg, mir->dalvikInsn.vB);
            storeValue(cUnit, rlDest, rlResult);
            break;

        case Instruction::CONST_HIGH16:
            rlResult = oatEvalLoc(cUnit, rlDest, kAnyReg, true);
            loadConstantNoClobber(cUnit, rlResult.lowReg,
                                  mir->dalvikInsn.vB << 16);
            storeValue(cUnit, rlDest, rlResult);
            break;

        case Instruction::CONST_WIDE_16:
        case Instruction::CONST_WIDE_32:
            rlResult = oatEvalLoc(cUnit, rlDest, kAnyReg, true);
            loadConstantValueWide(cUnit, rlResult.lowReg, rlResult.highReg,
                                  mir->dalvikInsn.vB,
                                  (mir->dalvikInsn.vB & 0x80000000) ? -1 : 0);
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
                                  0, mir->dalvikInsn.vB << 16);
            storeValueWide(cUnit, rlDest, rlResult);
            break;

        case Instruction::MONITOR_ENTER:
            genMonitorEnter(cUnit, mir, rlSrc[0]);
            break;

        case Instruction::MONITOR_EXIT:
            genMonitorExit(cUnit, mir, rlSrc[0]);
            break;

        case Instruction::CHECK_CAST:
            genCheckCast(cUnit, mir, rlSrc[0]);
            break;

        case Instruction::INSTANCE_OF:
            genInstanceof(cUnit, mir, rlDest, rlSrc[0]);
            break;

        case Instruction::NEW_INSTANCE:
            genNewInstance(cUnit, mir, rlDest);
            break;

        case Instruction::THROW:
            genThrow(cUnit, mir, rlSrc[0]);
            break;

        case Instruction::THROW_VERIFICATION_ERROR:
            genThrowVerificationError(cUnit, mir);
            break;

        case Instruction::ARRAY_LENGTH:
            int lenOffset;
            lenOffset = Array::LengthOffset().Int32Value();
            rlSrc[0] = loadValue(cUnit, rlSrc[0], kCoreReg);
            genNullCheck(cUnit, rlSrc[0].sRegLow, rlSrc[0].lowReg, mir);
            rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
            loadWordDisp(cUnit, rlSrc[0].lowReg, lenOffset,
                         rlResult.lowReg);
            storeValue(cUnit, rlDest, rlResult);
            break;

        case Instruction::CONST_STRING:
        case Instruction::CONST_STRING_JUMBO:
            genConstString(cUnit, mir, rlDest, rlSrc[0]);
            break;

        case Instruction::CONST_CLASS:
            genConstClass(cUnit, mir, rlDest, rlSrc[0]);
            break;

        case Instruction::FILL_ARRAY_DATA:
            genFillArrayData(cUnit, mir, rlSrc[0]);
            break;

        case Instruction::FILLED_NEW_ARRAY:
            genFilledNewArray(cUnit, mir, false /* not range */);
            break;

        case Instruction::FILLED_NEW_ARRAY_RANGE:
            genFilledNewArray(cUnit, mir, true /* range */);
            break;

        case Instruction::NEW_ARRAY:
            genNewArray(cUnit, mir, rlDest, rlSrc[0]);
            break;

        case Instruction::GOTO:
        case Instruction::GOTO_16:
        case Instruction::GOTO_32:
            if (bb->taken->startOffset <= mir->offset) {
                genSuspendTest(cUnit, mir);
            }
            opUnconditionalBranch(cUnit, &labelList[bb->taken->id]);
            break;

        case Instruction::PACKED_SWITCH:
            genPackedSwitch(cUnit, mir, rlSrc[0]);
            break;

        case Instruction::SPARSE_SWITCH:
            genSparseSwitch(cUnit, mir, rlSrc[0]);
            break;

        case Instruction::CMPL_FLOAT:
        case Instruction::CMPG_FLOAT:
        case Instruction::CMPL_DOUBLE:
        case Instruction::CMPG_DOUBLE:
            res = genCmpFP(cUnit, mir, rlDest, rlSrc[0], rlSrc[1]);
            break;

        case Instruction::CMP_LONG:
            genCmpLong(cUnit, mir, rlDest, rlSrc[0], rlSrc[1]);
            break;

        case Instruction::IF_EQ:
        case Instruction::IF_NE:
        case Instruction::IF_LT:
        case Instruction::IF_GE:
        case Instruction::IF_GT:
        case Instruction::IF_LE: {
            bool backwardBranch;
            backwardBranch = (bb->taken->startOffset <= mir->offset);
            if (backwardBranch) {
                genSuspendTest(cUnit, mir);
            }
            genCompareAndBranch(cUnit, bb, mir, rlSrc[0], rlSrc[1], labelList);
            break;
            }

        case Instruction::IF_EQZ:
        case Instruction::IF_NEZ:
        case Instruction::IF_LTZ:
        case Instruction::IF_GEZ:
        case Instruction::IF_GTZ:
        case Instruction::IF_LEZ: {
            bool backwardBranch;
            backwardBranch = (bb->taken->startOffset <= mir->offset);
            if (backwardBranch) {
                genSuspendTest(cUnit, mir);
            }
            genCompareZeroAndBranch(cUnit, bb, mir, rlSrc[0], labelList);
            break;
            }

      case Instruction::AGET_WIDE:
            genArrayGet(cUnit, mir, kLong, rlSrc[0], rlSrc[1], rlDest, 3);
            break;
        case Instruction::AGET:
        case Instruction::AGET_OBJECT:
            genArrayGet(cUnit, mir, kWord, rlSrc[0], rlSrc[1], rlDest, 2);
            break;
        case Instruction::AGET_BOOLEAN:
            genArrayGet(cUnit, mir, kUnsignedByte, rlSrc[0], rlSrc[1],
                        rlDest, 0);
            break;
        case Instruction::AGET_BYTE:
            genArrayGet(cUnit, mir, kSignedByte, rlSrc[0], rlSrc[1], rlDest, 0);
            break;
        case Instruction::AGET_CHAR:
            genArrayGet(cUnit, mir, kUnsignedHalf, rlSrc[0], rlSrc[1],
                        rlDest, 1);
            break;
        case Instruction::AGET_SHORT:
            genArrayGet(cUnit, mir, kSignedHalf, rlSrc[0], rlSrc[1], rlDest, 1);
            break;
        case Instruction::APUT_WIDE:
            genArrayPut(cUnit, mir, kLong, rlSrc[1], rlSrc[2], rlSrc[0], 3);
            break;
        case Instruction::APUT:
            genArrayPut(cUnit, mir, kWord, rlSrc[1], rlSrc[2], rlSrc[0], 2);
            break;
        case Instruction::APUT_OBJECT:
            genArrayObjPut(cUnit, mir, rlSrc[1], rlSrc[2], rlSrc[0], 2);
            break;
        case Instruction::APUT_SHORT:
        case Instruction::APUT_CHAR:
            genArrayPut(cUnit, mir, kUnsignedHalf, rlSrc[1], rlSrc[2],
                        rlSrc[0], 1);
            break;
        case Instruction::APUT_BYTE:
        case Instruction::APUT_BOOLEAN:
            genArrayPut(cUnit, mir, kUnsignedByte, rlSrc[1], rlSrc[2],
                        rlSrc[0], 0);
            break;

        case Instruction::IGET_OBJECT:
        //case Instruction::IGET_OBJECT_VOLATILE:
            genIGet(cUnit, mir, kWord, rlDest, rlSrc[0], false, true);
            break;

        case Instruction::IGET_WIDE:
        //case Instruction::IGET_WIDE_VOLATILE:
            genIGet(cUnit, mir, kLong, rlDest, rlSrc[0], true, false);
            break;

        case Instruction::IGET:
        //case Instruction::IGET_VOLATILE:
            genIGet(cUnit, mir, kWord, rlDest, rlSrc[0], false, false);
            break;

        case Instruction::IGET_CHAR:
            genIGet(cUnit, mir, kUnsignedHalf, rlDest, rlSrc[0], false, false);
            break;

        case Instruction::IGET_SHORT:
            genIGet(cUnit, mir, kSignedHalf, rlDest, rlSrc[0], false, false);
            break;

        case Instruction::IGET_BOOLEAN:
        case Instruction::IGET_BYTE:
            genIGet(cUnit, mir, kUnsignedByte, rlDest, rlSrc[0], false, false);
            break;

        case Instruction::IPUT_WIDE:
        //case Instruction::IPUT_WIDE_VOLATILE:
            genIPut(cUnit, mir, kLong, rlSrc[0], rlSrc[1], true, false);
            break;

        case Instruction::IPUT_OBJECT:
        //case Instruction::IPUT_OBJECT_VOLATILE:
            genIPut(cUnit, mir, kWord, rlSrc[0], rlSrc[1], false, true);
            break;

        case Instruction::IPUT:
        //case Instruction::IPUT_VOLATILE:
            genIPut(cUnit, mir, kWord, rlSrc[0], rlSrc[1], false, false);
            break;

        case Instruction::IPUT_BOOLEAN:
        case Instruction::IPUT_BYTE:
            genIPut(cUnit, mir, kUnsignedByte, rlSrc[0], rlSrc[1], false, false);
            break;

        case Instruction::IPUT_CHAR:
            genIPut(cUnit, mir, kUnsignedHalf, rlSrc[0], rlSrc[1], false, false);
            break;

        case Instruction::IPUT_SHORT:
            genIPut(cUnit, mir, kSignedHalf, rlSrc[0], rlSrc[1], false, false);
            break;

        case Instruction::SGET_OBJECT:
            genSget(cUnit, mir, rlDest, false, true);
            break;
        case Instruction::SGET:
        case Instruction::SGET_BOOLEAN:
        case Instruction::SGET_BYTE:
        case Instruction::SGET_CHAR:
        case Instruction::SGET_SHORT:
            genSget(cUnit, mir, rlDest, false, false);
            break;

        case Instruction::SGET_WIDE:
            genSget(cUnit, mir, rlDest, true, false);
            break;

        case Instruction::SPUT_OBJECT:
            genSput(cUnit, mir, rlSrc[0], false, true);
            break;

        case Instruction::SPUT:
        case Instruction::SPUT_BOOLEAN:
        case Instruction::SPUT_BYTE:
        case Instruction::SPUT_CHAR:
        case Instruction::SPUT_SHORT:
            genSput(cUnit, mir, rlSrc[0], false, false);
            break;

        case Instruction::SPUT_WIDE:
            genSput(cUnit, mir, rlSrc[0], true, false);
            break;

        case Instruction::INVOKE_STATIC_RANGE:
            genInvoke(cUnit, mir, kStatic, true /*range*/);
            break;
        case Instruction::INVOKE_STATIC:
            genInvoke(cUnit, mir, kStatic, false /*range*/);
            break;

        case Instruction::INVOKE_DIRECT:
            genInvoke(cUnit, mir, kDirect, false /*range*/);
            break;
        case Instruction::INVOKE_DIRECT_RANGE:
            genInvoke(cUnit, mir, kDirect, true /*range*/);
            break;

        case Instruction::INVOKE_VIRTUAL:
            genInvoke(cUnit, mir, kVirtual, false /*range*/);
            break;
        case Instruction::INVOKE_VIRTUAL_RANGE:
            genInvoke(cUnit, mir, kVirtual, true /*range*/);
            break;

        case Instruction::INVOKE_SUPER:
            genInvoke(cUnit, mir, kSuper, false /*range*/);
            break;
        case Instruction::INVOKE_SUPER_RANGE:
            genInvoke(cUnit, mir, kSuper, true /*range*/);
            break;

        case Instruction::INVOKE_INTERFACE:
            genInvoke(cUnit, mir, kInterface, false /*range*/);
            break;
        case Instruction::INVOKE_INTERFACE_RANGE:
            genInvoke(cUnit, mir, kInterface, true /*range*/);
            break;

        case Instruction::NEG_INT:
        case Instruction::NOT_INT:
            res = genArithOpInt(cUnit, mir, rlDest, rlSrc[0], rlSrc[0]);
            break;

        case Instruction::NEG_LONG:
        case Instruction::NOT_LONG:
            res = genArithOpLong(cUnit, mir, rlDest, rlSrc[0], rlSrc[0]);
            break;

        case Instruction::NEG_FLOAT:
            res = genArithOpFloat(cUnit, mir, rlDest, rlSrc[0], rlSrc[0]);
            break;

        case Instruction::NEG_DOUBLE:
            res = genArithOpDouble(cUnit, mir, rlDest, rlSrc[0], rlSrc[0]);
            break;

        case Instruction::INT_TO_LONG:
            genIntToLong(cUnit, mir, rlDest, rlSrc[0]);
            break;

        case Instruction::LONG_TO_INT:
            rlSrc[0] = oatUpdateLocWide(cUnit, rlSrc[0]);
            rlSrc[0] = oatWideToNarrow(cUnit, rlSrc[0]);
            storeValue(cUnit, rlDest, rlSrc[0]);
            break;

        case Instruction::INT_TO_BYTE:
        case Instruction::INT_TO_SHORT:
        case Instruction::INT_TO_CHAR:
            genIntNarrowing(cUnit, mir, rlDest, rlSrc[0]);
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
            genConversion(cUnit, mir);
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
            genArithOpInt(cUnit, mir, rlDest, rlSrc[0], rlSrc[1]);
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
            genArithOpLong(cUnit, mir, rlDest, rlSrc[0], rlSrc[1]);
            break;

        case Instruction::SHL_LONG:
        case Instruction::SHR_LONG:
        case Instruction::USHR_LONG:
        case Instruction::SHL_LONG_2ADDR:
        case Instruction::SHR_LONG_2ADDR:
        case Instruction::USHR_LONG_2ADDR:
            genShiftOpLong(cUnit,mir, rlDest, rlSrc[0], rlSrc[1]);
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
            genArithOpFloat(cUnit, mir, rlDest, rlSrc[0], rlSrc[1]);
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
            genArithOpDouble(cUnit, mir, rlDest, rlSrc[0], rlSrc[1]);
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

        Instruction::Code dalvikOpcode = mir->dalvikInsn.opcode;
        Instruction::Format dalvikFormat = Instruction::FormatOf(dalvikOpcode);

        LIR* boundaryLIR;

        /* Mark the beginning of a Dalvik instruction for line tracking */
        char* instStr = cUnit->printMe ?
           oatGetDalvikDisassembly(cUnit, mir->dalvikInsn, "") : NULL;
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
          LOG(FATAL) << StringPrintf("%#06x: Opcode %#x (%s) / Fmt %d not handled",
                                     mir->offset, dalvikOpcode, Instruction::Name(dalvikOpcode), dalvikFormat);

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

    if (!(cUnit->disableOpt & (1 << kSafeOptimizations))) {
        removeRedundantBranches(cUnit);
    }
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
