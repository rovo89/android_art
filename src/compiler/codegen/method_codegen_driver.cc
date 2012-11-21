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

#include "../compiler_internals.h"
#include "local_optimizations.h"
#include "codegen_util.h"
#include "ralloc_util.h"

namespace art {

// TODO: unify badLoc
const RegLocation badLoc = {kLocDalvikFrame, 0, 0, 0, 0, 0, 0, 0, 0,
                            INVALID_REG, INVALID_REG, INVALID_SREG,
                            INVALID_SREG};

/* Mark register usage state and return long retloc */
RegLocation GetReturnWide(CompilationUnit* cUnit, bool isDouble)
{
  RegLocation gpr_res = LocCReturnWide();
  RegLocation fpr_res = LocCReturnDouble();
  RegLocation res = isDouble ? fpr_res : gpr_res;
  Clobber(cUnit, res.lowReg);
  Clobber(cUnit, res.highReg);
  LockTemp(cUnit, res.lowReg);
  LockTemp(cUnit, res.highReg);
  MarkPair(cUnit, res.lowReg, res.highReg);
  return res;
}

RegLocation GetReturn(CompilationUnit* cUnit, bool isFloat)
{
  RegLocation gpr_res = LocCReturn();
  RegLocation fpr_res = LocCReturnFloat();
  RegLocation res = isFloat ? fpr_res : gpr_res;
  Clobber(cUnit, res.lowReg);
  if (cUnit->instructionSet == kMips) {
    MarkInUse(cUnit, res.lowReg);
  } else {
    LockTemp(cUnit, res.lowReg);
  }
  return res;
}

/*
 * Target-independent code generation.  Use only high-level
 * load/store utilities here, or target-dependent genXX() handlers
 * when necessary.
 */
static bool CompileDalvikInstruction(CompilationUnit* cUnit, MIR* mir, BasicBlock* bb,
                                     LIR* labelList)
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
      rlSrc[nextLoc++] = GetSrcWide(cUnit, mir, nextSreg);
      nextSreg+= 2;
    } else {
      rlSrc[nextLoc++] = GetSrc(cUnit, mir, nextSreg);
      nextSreg++;
    }
  }
  if (attrs & DF_UB) {
    if (attrs & DF_B_WIDE) {
      rlSrc[nextLoc++] = GetSrcWide(cUnit, mir, nextSreg);
      nextSreg+= 2;
    } else {
      rlSrc[nextLoc++] = GetSrc(cUnit, mir, nextSreg);
      nextSreg++;
    }
  }
  if (attrs & DF_UC) {
    if (attrs & DF_C_WIDE) {
      rlSrc[nextLoc++] = GetSrcWide(cUnit, mir, nextSreg);
    } else {
      rlSrc[nextLoc++] = GetSrc(cUnit, mir, nextSreg);
    }
  }
  if (attrs & DF_DA) {
    if (attrs & DF_A_WIDE) {
      rlDest = GetDestWide(cUnit, mir);
    } else {
      rlDest = GetDest(cUnit, mir);
    }
  }
  switch (opcode) {
    case Instruction::NOP:
      break;

    case Instruction::MOVE_EXCEPTION:
      GenMoveException(cUnit, rlDest);
      break;
    case Instruction::RETURN_VOID:
      if (!(cUnit->attrs & METHOD_IS_LEAF)) {
        GenSuspendTest(cUnit, optFlags);
      }
      break;

    case Instruction::RETURN:
    case Instruction::RETURN_OBJECT:
      if (!(cUnit->attrs & METHOD_IS_LEAF)) {
        GenSuspendTest(cUnit, optFlags);
      }
      StoreValue(cUnit, GetReturn(cUnit, cUnit->shorty[0] == 'F'), rlSrc[0]);
      break;

    case Instruction::RETURN_WIDE:
      if (!(cUnit->attrs & METHOD_IS_LEAF)) {
        GenSuspendTest(cUnit, optFlags);
      }
      StoreValueWide(cUnit, GetReturnWide(cUnit,
                       cUnit->shorty[0] == 'D'), rlSrc[0]);
      break;

    case Instruction::MOVE_RESULT_WIDE:
      if (optFlags & MIR_INLINED)
        break;  // Nop - combined w/ previous invoke
      StoreValueWide(cUnit, rlDest, GetReturnWide(cUnit, rlDest.fp));
      break;

    case Instruction::MOVE_RESULT:
    case Instruction::MOVE_RESULT_OBJECT:
      if (optFlags & MIR_INLINED)
        break;  // Nop - combined w/ previous invoke
      StoreValue(cUnit, rlDest, GetReturn(cUnit, rlDest.fp));
      break;

    case Instruction::MOVE:
    case Instruction::MOVE_OBJECT:
    case Instruction::MOVE_16:
    case Instruction::MOVE_OBJECT_16:
    case Instruction::MOVE_FROM16:
    case Instruction::MOVE_OBJECT_FROM16:
      StoreValue(cUnit, rlDest, rlSrc[0]);
      break;

    case Instruction::MOVE_WIDE:
    case Instruction::MOVE_WIDE_16:
    case Instruction::MOVE_WIDE_FROM16:
      StoreValueWide(cUnit, rlDest, rlSrc[0]);
      break;

    case Instruction::CONST:
    case Instruction::CONST_4:
    case Instruction::CONST_16:
      rlResult = EvalLoc(cUnit, rlDest, kAnyReg, true);
      LoadConstantNoClobber(cUnit, rlResult.lowReg, vB);
      StoreValue(cUnit, rlDest, rlResult);
      break;

    case Instruction::CONST_HIGH16:
      rlResult = EvalLoc(cUnit, rlDest, kAnyReg, true);
      LoadConstantNoClobber(cUnit, rlResult.lowReg, vB << 16);
      StoreValue(cUnit, rlDest, rlResult);
      break;

    case Instruction::CONST_WIDE_16:
    case Instruction::CONST_WIDE_32:
      rlResult = EvalLoc(cUnit, rlDest, kAnyReg, true);
      LoadConstantValueWide(cUnit, rlResult.lowReg, rlResult.highReg, vB,
                            (vB & 0x80000000) ? -1 : 0);
      StoreValueWide(cUnit, rlDest, rlResult);
      break;

    case Instruction::CONST_WIDE:
      rlResult = EvalLoc(cUnit, rlDest, kAnyReg, true);
      LoadConstantValueWide(cUnit, rlResult.lowReg, rlResult.highReg,
                            mir->dalvikInsn.vB_wide & 0xffffffff,
                            (mir->dalvikInsn.vB_wide >> 32) & 0xffffffff);
      StoreValueWide(cUnit, rlDest, rlResult);
      break;

    case Instruction::CONST_WIDE_HIGH16:
      rlResult = EvalLoc(cUnit, rlDest, kAnyReg, true);
      LoadConstantValueWide(cUnit, rlResult.lowReg, rlResult.highReg,
                            0, vB << 16);
      StoreValueWide(cUnit, rlDest, rlResult);
      break;

    case Instruction::MONITOR_ENTER:
      GenMonitorEnter(cUnit, optFlags, rlSrc[0]);
      break;

    case Instruction::MONITOR_EXIT:
      GenMonitorExit(cUnit, optFlags, rlSrc[0]);
      break;

    case Instruction::CHECK_CAST:
      GenCheckCast(cUnit, vB, rlSrc[0]);
      break;

    case Instruction::INSTANCE_OF:
      GenInstanceof(cUnit, vC, rlDest, rlSrc[0]);
      break;

    case Instruction::NEW_INSTANCE:
      GenNewInstance(cUnit, vB, rlDest);
      break;

    case Instruction::THROW:
      GenThrow(cUnit, rlSrc[0]);
      break;

    case Instruction::ARRAY_LENGTH:
      int lenOffset;
      lenOffset = Array::LengthOffset().Int32Value();
      rlSrc[0] = LoadValue(cUnit, rlSrc[0], kCoreReg);
      GenNullCheck(cUnit, rlSrc[0].sRegLow, rlSrc[0].lowReg, optFlags);
      rlResult = EvalLoc(cUnit, rlDest, kCoreReg, true);
      LoadWordDisp(cUnit, rlSrc[0].lowReg, lenOffset, rlResult.lowReg);
      StoreValue(cUnit, rlDest, rlResult);
      break;

    case Instruction::CONST_STRING:
    case Instruction::CONST_STRING_JUMBO:
      GenConstString(cUnit, vB, rlDest);
      break;

    case Instruction::CONST_CLASS:
      GenConstClass(cUnit, vB, rlDest);
      break;

    case Instruction::FILL_ARRAY_DATA:
      GenFillArrayData(cUnit, vB, rlSrc[0]);
      break;

    case Instruction::FILLED_NEW_ARRAY:
      GenFilledNewArray(cUnit, NewMemCallInfo(cUnit, bb, mir, kStatic,
                        false /* not range */));
      break;

    case Instruction::FILLED_NEW_ARRAY_RANGE:
      GenFilledNewArray(cUnit, NewMemCallInfo(cUnit, bb, mir, kStatic,
                        true /* range */));
      break;

    case Instruction::NEW_ARRAY:
      GenNewArray(cUnit, vC, rlDest, rlSrc[0]);
      break;

    case Instruction::GOTO:
    case Instruction::GOTO_16:
    case Instruction::GOTO_32:
      if (bb->taken->startOffset <= mir->offset) {
        GenSuspendTestAndBranch(cUnit, optFlags, &labelList[bb->taken->id]);
      } else {
        OpUnconditionalBranch(cUnit, &labelList[bb->taken->id]);
      }
      break;

    case Instruction::PACKED_SWITCH:
      GenPackedSwitch(cUnit, vB, rlSrc[0]);
      break;

    case Instruction::SPARSE_SWITCH:
      GenSparseSwitch(cUnit, vB, rlSrc[0]);
      break;

    case Instruction::CMPL_FLOAT:
    case Instruction::CMPG_FLOAT:
    case Instruction::CMPL_DOUBLE:
    case Instruction::CMPG_DOUBLE:
      res = GenCmpFP(cUnit, opcode, rlDest, rlSrc[0], rlSrc[1]);
      break;

    case Instruction::CMP_LONG:
      GenCmpLong(cUnit, rlDest, rlSrc[0], rlSrc[1]);
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
        GenSuspendTest(cUnit, optFlags);
      }
      GenCompareAndBranch(cUnit, opcode, rlSrc[0], rlSrc[1], taken,
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
        GenSuspendTest(cUnit, optFlags);
      }
      GenCompareZeroAndBranch(cUnit, opcode, rlSrc[0], taken, fallThrough);
      break;
      }

    case Instruction::AGET_WIDE:
      GenArrayGet(cUnit, optFlags, kLong, rlSrc[0], rlSrc[1], rlDest, 3);
      break;
    case Instruction::AGET:
    case Instruction::AGET_OBJECT:
      GenArrayGet(cUnit, optFlags, kWord, rlSrc[0], rlSrc[1], rlDest, 2);
      break;
    case Instruction::AGET_BOOLEAN:
      GenArrayGet(cUnit, optFlags, kUnsignedByte, rlSrc[0], rlSrc[1], rlDest, 0);
      break;
    case Instruction::AGET_BYTE:
      GenArrayGet(cUnit, optFlags, kSignedByte, rlSrc[0], rlSrc[1], rlDest, 0);
      break;
    case Instruction::AGET_CHAR:
      GenArrayGet(cUnit, optFlags, kUnsignedHalf, rlSrc[0], rlSrc[1], rlDest, 1);
      break;
    case Instruction::AGET_SHORT:
      GenArrayGet(cUnit, optFlags, kSignedHalf, rlSrc[0], rlSrc[1], rlDest, 1);
      break;
    case Instruction::APUT_WIDE:
      GenArrayPut(cUnit, optFlags, kLong, rlSrc[1], rlSrc[2], rlSrc[0], 3);
      break;
    case Instruction::APUT:
      GenArrayPut(cUnit, optFlags, kWord, rlSrc[1], rlSrc[2], rlSrc[0], 2);
      break;
    case Instruction::APUT_OBJECT:
      GenArrayObjPut(cUnit, optFlags, rlSrc[1], rlSrc[2], rlSrc[0], 2);
      break;
    case Instruction::APUT_SHORT:
    case Instruction::APUT_CHAR:
      GenArrayPut(cUnit, optFlags, kUnsignedHalf, rlSrc[1], rlSrc[2], rlSrc[0], 1);
      break;
    case Instruction::APUT_BYTE:
    case Instruction::APUT_BOOLEAN:
      GenArrayPut(cUnit, optFlags, kUnsignedByte, rlSrc[1], rlSrc[2],
            rlSrc[0], 0);
      break;

    case Instruction::IGET_OBJECT:
    //case Instruction::IGET_OBJECT_VOLATILE:
      GenIGet(cUnit, vC, optFlags, kWord, rlDest, rlSrc[0], false, true);
      break;

    case Instruction::IGET_WIDE:
    //case Instruction::IGET_WIDE_VOLATILE:
      GenIGet(cUnit, vC, optFlags, kLong, rlDest, rlSrc[0], true, false);
      break;

    case Instruction::IGET:
    //case Instruction::IGET_VOLATILE:
      GenIGet(cUnit, vC, optFlags, kWord, rlDest, rlSrc[0], false, false);
      break;

    case Instruction::IGET_CHAR:
      GenIGet(cUnit, vC, optFlags, kUnsignedHalf, rlDest, rlSrc[0], false, false);
      break;

    case Instruction::IGET_SHORT:
      GenIGet(cUnit, vC, optFlags, kSignedHalf, rlDest, rlSrc[0], false, false);
      break;

    case Instruction::IGET_BOOLEAN:
    case Instruction::IGET_BYTE:
      GenIGet(cUnit, vC, optFlags, kUnsignedByte, rlDest, rlSrc[0], false, false);
      break;

    case Instruction::IPUT_WIDE:
    //case Instruction::IPUT_WIDE_VOLATILE:
      GenIPut(cUnit, vC, optFlags, kLong, rlSrc[0], rlSrc[1], true, false);
      break;

    case Instruction::IPUT_OBJECT:
    //case Instruction::IPUT_OBJECT_VOLATILE:
      GenIPut(cUnit, vC, optFlags, kWord, rlSrc[0], rlSrc[1], false, true);
      break;

    case Instruction::IPUT:
    //case Instruction::IPUT_VOLATILE:
      GenIPut(cUnit, vC, optFlags, kWord, rlSrc[0], rlSrc[1], false, false);
      break;

    case Instruction::IPUT_BOOLEAN:
    case Instruction::IPUT_BYTE:
      GenIPut(cUnit, vC, optFlags, kUnsignedByte, rlSrc[0], rlSrc[1], false, false);
      break;

    case Instruction::IPUT_CHAR:
      GenIPut(cUnit, vC, optFlags, kUnsignedHalf, rlSrc[0], rlSrc[1], false, false);
      break;

    case Instruction::IPUT_SHORT:
      GenIPut(cUnit, vC, optFlags, kSignedHalf, rlSrc[0], rlSrc[1], false, false);
      break;

    case Instruction::SGET_OBJECT:
      GenSget(cUnit, vB, rlDest, false, true);
      break;
    case Instruction::SGET:
    case Instruction::SGET_BOOLEAN:
    case Instruction::SGET_BYTE:
    case Instruction::SGET_CHAR:
    case Instruction::SGET_SHORT:
      GenSget(cUnit, vB, rlDest, false, false);
      break;

    case Instruction::SGET_WIDE:
      GenSget(cUnit, vB, rlDest, true, false);
      break;

    case Instruction::SPUT_OBJECT:
      GenSput(cUnit, vB, rlSrc[0], false, true);
      break;

    case Instruction::SPUT:
    case Instruction::SPUT_BOOLEAN:
    case Instruction::SPUT_BYTE:
    case Instruction::SPUT_CHAR:
    case Instruction::SPUT_SHORT:
      GenSput(cUnit, vB, rlSrc[0], false, false);
      break;

    case Instruction::SPUT_WIDE:
      GenSput(cUnit, vB, rlSrc[0], true, false);
      break;

    case Instruction::INVOKE_STATIC_RANGE:
      GenInvoke(cUnit, NewMemCallInfo(cUnit, bb, mir, kStatic, true));
      break;
    case Instruction::INVOKE_STATIC:
      GenInvoke(cUnit, NewMemCallInfo(cUnit, bb, mir, kStatic, false));
      break;

    case Instruction::INVOKE_DIRECT:
      GenInvoke(cUnit, NewMemCallInfo(cUnit, bb, mir, kDirect, false));
      break;
    case Instruction::INVOKE_DIRECT_RANGE:
      GenInvoke(cUnit, NewMemCallInfo(cUnit, bb, mir, kDirect, true));
      break;

    case Instruction::INVOKE_VIRTUAL:
      GenInvoke(cUnit, NewMemCallInfo(cUnit, bb, mir, kVirtual, false));
      break;
    case Instruction::INVOKE_VIRTUAL_RANGE:
      GenInvoke(cUnit, NewMemCallInfo(cUnit, bb, mir, kVirtual, true));
      break;

    case Instruction::INVOKE_SUPER:
      GenInvoke(cUnit, NewMemCallInfo(cUnit, bb, mir, kSuper, false));
      break;
    case Instruction::INVOKE_SUPER_RANGE:
      GenInvoke(cUnit, NewMemCallInfo(cUnit, bb, mir, kSuper, true));
      break;

    case Instruction::INVOKE_INTERFACE:
      GenInvoke(cUnit, NewMemCallInfo(cUnit, bb, mir, kInterface, false));
      break;
    case Instruction::INVOKE_INTERFACE_RANGE:
      GenInvoke(cUnit, NewMemCallInfo(cUnit, bb, mir, kInterface, true));
      break;

    case Instruction::NEG_INT:
    case Instruction::NOT_INT:
      res = GenArithOpInt(cUnit, opcode, rlDest, rlSrc[0], rlSrc[0]);
      break;

    case Instruction::NEG_LONG:
    case Instruction::NOT_LONG:
      res = GenArithOpLong(cUnit, opcode, rlDest, rlSrc[0], rlSrc[0]);
      break;

    case Instruction::NEG_FLOAT:
      res = GenArithOpFloat(cUnit, opcode, rlDest, rlSrc[0], rlSrc[0]);
      break;

    case Instruction::NEG_DOUBLE:
      res = GenArithOpDouble(cUnit, opcode, rlDest, rlSrc[0], rlSrc[0]);
      break;

    case Instruction::INT_TO_LONG:
      GenIntToLong(cUnit, rlDest, rlSrc[0]);
      break;

    case Instruction::LONG_TO_INT:
      rlSrc[0] = UpdateLocWide(cUnit, rlSrc[0]);
      rlSrc[0] = WideToNarrow(cUnit, rlSrc[0]);
      StoreValue(cUnit, rlDest, rlSrc[0]);
      break;

    case Instruction::INT_TO_BYTE:
    case Instruction::INT_TO_SHORT:
    case Instruction::INT_TO_CHAR:
      GenIntNarrowing(cUnit, opcode, rlDest, rlSrc[0]);
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
      GenConversion(cUnit, opcode, rlDest, rlSrc[0]);
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
      GenArithOpInt(cUnit, opcode, rlDest, rlSrc[0], rlSrc[1]);
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
      GenArithOpLong(cUnit, opcode, rlDest, rlSrc[0], rlSrc[1]);
      break;

    case Instruction::SHL_LONG:
    case Instruction::SHR_LONG:
    case Instruction::USHR_LONG:
    case Instruction::SHL_LONG_2ADDR:
    case Instruction::SHR_LONG_2ADDR:
    case Instruction::USHR_LONG_2ADDR:
      GenShiftOpLong(cUnit, opcode, rlDest, rlSrc[0], rlSrc[1]);
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
      GenArithOpFloat(cUnit, opcode, rlDest, rlSrc[0], rlSrc[1]);
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
      GenArithOpDouble(cUnit, opcode, rlDest, rlSrc[0], rlSrc[1]);
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
      GenArithOpIntLit(cUnit, opcode, rlDest, rlSrc[0], vC);
      break;

    default:
      res = true;
  }
  return res;
}

/* Extended MIR instructions like PHI */
static void HandleExtendedMethodMIR(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir)
{
  int opOffset = mir->dalvikInsn.opcode - kMirOpFirst;
  char* msg = NULL;
  if (cUnit->printMe) {
    msg = static_cast<char*>(NewMem(cUnit, strlen(extendedMIROpNames[opOffset]) + 1,
                                    false, kAllocDebugInfo));
    strcpy(msg, extendedMIROpNames[opOffset]);
  }
  LIR* op = NewLIR1(cUnit, kPseudoExtended, reinterpret_cast<uintptr_t>(msg));

  switch (static_cast<ExtendedMIROpcode>(mir->dalvikInsn.opcode)) {
    case kMirOpPhi: {
      char* ssaString = NULL;
      if (cUnit->printMe) {
        ssaString = GetSSAString(cUnit, mir->ssaRep);
      }
      op->flags.isNop = true;
      NewLIR1(cUnit, kPseudoSSARep, reinterpret_cast<uintptr_t>(ssaString));
      break;
    }
    case kMirOpCopy: {
      RegLocation rlSrc = GetSrc(cUnit, mir, 0);
      RegLocation rlDest = GetDest(cUnit, mir);
      StoreValue(cUnit, rlDest, rlSrc);
      break;
    }
    case kMirOpFusedCmplFloat:
      GenFusedFPCmpBranch(cUnit, bb, mir, false /*gt bias*/, false /*double*/);
      break;
    case kMirOpFusedCmpgFloat:
      GenFusedFPCmpBranch(cUnit, bb, mir, true /*gt bias*/, false /*double*/);
      break;
    case kMirOpFusedCmplDouble:
      GenFusedFPCmpBranch(cUnit, bb, mir, false /*gt bias*/, true /*double*/);
      break;
    case kMirOpFusedCmpgDouble:
      GenFusedFPCmpBranch(cUnit, bb, mir, true /*gt bias*/, true /*double*/);
      break;
    case kMirOpFusedCmpLong:
      GenFusedLongCmpBranch(cUnit, bb, mir);
      break;
    default:
      break;
  }
}

/* Handle the content in each basic block */
static bool MethodBlockCodeGen(CompilationUnit* cUnit, BasicBlock* bb)
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
  AppendLIR(cUnit, &labelList[blockId]);

  LIR* headLIR = NULL;

  /* If this is a catch block, export the start address */
  if (bb->catchEntry) {
    headLIR = NewLIR0(cUnit, kPseudoExportedPC);
  }

  /* Free temp registers and reset redundant store tracking */
  ResetRegPool(cUnit);
  ResetDefTracking(cUnit);

  ClobberAllRegs(cUnit);


  if (bb->blockType == kEntryBlock) {
    int startVReg = cUnit->numDalvikRegisters - cUnit->numIns;
    GenEntrySequence(cUnit, &cUnit->regLocation[startVReg],
                     cUnit->regLocation[cUnit->methodSReg]);
  } else if (bb->blockType == kExitBlock) {
    GenExitSequence(cUnit);
  }

  for (mir = bb->firstMIRInsn; mir; mir = mir->next) {
    ResetRegPool(cUnit);
    if (cUnit->disableOpt & (1 << kTrackLiveTemps)) {
      ClobberAllRegs(cUnit);
    }

    if (cUnit->disableOpt & (1 << kSuppressLoads)) {
      ResetDefTracking(cUnit);
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
       GetDalvikDisassembly(cUnit, mir->dalvikInsn, "") : NULL;
    boundaryLIR = MarkBoundary(cUnit, mir->offset, instStr);
    /* Remember the first LIR for this block */
    if (headLIR == NULL) {
      headLIR = boundaryLIR;
      /* Set the first boundaryLIR as a scheduling barrier */
      headLIR->defMask = ENCODE_ALL;
    }

    /* Don't generate the SSA annotation unless verbose mode is on */
    if (cUnit->printMe && mir->ssaRep) {
      char* ssaString = GetSSAString(cUnit, mir->ssaRep);
      NewLIR1(cUnit, kPseudoSSARep, reinterpret_cast<uintptr_t>(ssaString));
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
      HandleExtendedMethodMIR(cUnit, bb, mir);
      continue;
    }

    bool notHandled = CompileDalvikInstruction(cUnit, mir, bb, labelList);
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
    ApplyLocalOptimizations(cUnit, headLIR, cUnit->lastLIRInsn);

    /*
     * Generate an unconditional branch to the fallthrough block.
     */
    if (bb->fallThrough) {
      OpUnconditionalBranch(cUnit, &labelList[bb->fallThrough->id]);
    }
  }
  return false;
}

void SpecialMIR2LIR(CompilationUnit* cUnit, SpecialCaseHandler specialCase)
{
  /* Find the first DalvikByteCode block */
  int numReachableBlocks = cUnit->numReachableBlocks;
  const GrowableList *blockList = &cUnit->blockList;
  BasicBlock*bb = NULL;
  for (int idx = 0; idx < numReachableBlocks; idx++) {
    int dfsIndex = cUnit->dfsOrder.elemList[idx];
    bb = reinterpret_cast<BasicBlock*>(GrowableListGetElement(blockList, dfsIndex));
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
  ResetRegPool(cUnit);
  ResetDefTracking(cUnit);
  ClobberAllRegs(cUnit);

  GenSpecialCase(cUnit, bb, mir, specialCase);
}

void MethodMIR2LIR(CompilationUnit* cUnit)
{
  /* Used to hold the labels of each block */
  cUnit->blockLabelList =
      static_cast<LIR*>(NewMem(cUnit, sizeof(LIR) * cUnit->numBlocks, true, kAllocLIR));

  DataFlowAnalysisDispatcher(cUnit, MethodBlockCodeGen,
                                kPreOrderDFSTraversal, false /* Iterative */);

  HandleSuspendLaunchPads(cUnit);

  HandleThrowLaunchPads(cUnit);

  HandleIntrinsicLaunchPads(cUnit);

  if (!(cUnit->disableOpt & (1 << kSafeOptimizations))) {
    RemoveRedundantBranches(cUnit);
  }
}

}  // namespace art
