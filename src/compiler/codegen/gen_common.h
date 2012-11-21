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

#ifndef ART_SRC_COMPILER_CODEGEN_GENCOMMON_H_
#define ART_SRC_COMPILER_CODEGEN_GENCOMMON_H_

void MarkSafepointPC(CompilationUnit* cUnit, LIR* inst);
void CallRuntimeHelperImm(CompilationUnit* cUnit, int helperOffset, int arg0, bool safepointPC);
void CallRuntimeHelperReg(CompilationUnit* cUnit, int helperOffset, int arg0, bool safepointPC);
void CallRuntimeHelperRegLocation(CompilationUnit* cUnit, int helperOffset, RegLocation arg0, bool safepointPC);
void CallRuntimeHelperImmImm(CompilationUnit* cUnit, int helperOffset, int arg0, int arg1, bool safepointPC);
void CallRuntimeHelperImmRegLocation(CompilationUnit* cUnit, int helperOffset, int arg0, RegLocation arg1, bool safepointPC);
void CallRuntimeHelperRegLocationImm(CompilationUnit* cUnit, int helperOffset, RegLocation arg0, int arg1, bool safepointPC);
void CallRuntimeHelperImmReg(CompilationUnit* cUnit, int helperOffset, int arg0, int arg1, bool safepointPC);
void CallRuntimeHelperRegImm(CompilationUnit* cUnit, int helperOffset, int arg0, int arg1, bool safepointPC);
void CallRuntimeHelperImmMethod(CompilationUnit* cUnit, int helperOffset, int arg0, bool safepointPC);
void CallRuntimeHelperRegLocationRegLocation(CompilationUnit* cUnit, int helperOffset, RegLocation arg0, RegLocation arg1, bool safepointPC);
void CallRuntimeHelperRegReg(CompilationUnit* cUnit, int helperOffset, int arg0, int arg1, bool safepointPC);
void CallRuntimeHelperRegRegImm(CompilationUnit* cUnit, int helperOffset, int arg0, int arg1, int arg2, bool safepointPC);
void CallRuntimeHelperImmMethodRegLocation(CompilationUnit* cUnit, int helperOffset, int arg0, RegLocation arg2, bool safepointPC);
void CallRuntimeHelperImmMethodImm(CompilationUnit* cUnit, int helperOffset, int arg0, int arg2, bool safepointPC);
void CallRuntimeHelperImmRegLocationRegLocation(CompilationUnit* cUnit, int helperOffset, int arg0, RegLocation arg1, RegLocation arg2, bool safepointPC);
void GenBarrier(CompilationUnit* cUnit);
LIR* OpUnconditionalBranch(CompilationUnit* cUnit, LIR* target);
LIR* GenCheck(CompilationUnit* cUnit, ConditionCode cCode, ThrowKind kind);
LIR* GenImmedCheck(CompilationUnit* cUnit, ConditionCode cCode, int reg, int immVal, ThrowKind kind);
LIR* GenNullCheck(CompilationUnit* cUnit, int sReg, int mReg, int optFlags);
LIR* GenRegRegCheck(CompilationUnit* cUnit, ConditionCode cCode, int reg1, int reg2, ThrowKind kind);
void GenCompareAndBranch(CompilationUnit* cUnit, Instruction::Code opcode, RegLocation rlSrc1, RegLocation rlSrc2, LIR* taken, LIR* fallThrough);
void GenCompareZeroAndBranch(CompilationUnit* cUnit, Instruction::Code opcode, RegLocation rlSrc, LIR* taken, LIR* fallThrough);
void GenIntToLong(CompilationUnit* cUnit, RegLocation rlDest, RegLocation rlSrc);
void GenIntNarrowing(CompilationUnit* cUnit, Instruction::Code opcode, RegLocation rlDest, RegLocation rlSrc);
void GenNewArray(CompilationUnit* cUnit, uint32_t type_idx, RegLocation rlDest, RegLocation rlSrc);
void GenFilledNewArray(CompilationUnit* cUnit, CallInfo* info);
void GenSput(CompilationUnit* cUnit, uint32_t fieldIdx, RegLocation rlSrc, bool isLongOrDouble, bool isObject);
void GenSget(CompilationUnit* cUnit, uint32_t fieldIdx, RegLocation rlDest, bool isLongOrDouble, bool isObject);
void GenShowTarget(CompilationUnit* cUnit);
void HandleSuspendLaunchPads(CompilationUnit *cUnit);
void HandleIntrinsicLaunchPads(CompilationUnit *cUnit);
void HandleThrowLaunchPads(CompilationUnit *cUnit);
void SetupResourceMasks(CompilationUnit* cUnit, LIR* lir);
bool FastInstance(CompilationUnit* cUnit,  uint32_t fieldIdx, int& fieldOffset, bool& isVolatile, bool isPut);
void GenIGet(CompilationUnit* cUnit, uint32_t fieldIdx, int optFlags, OpSize size, RegLocation rlDest, RegLocation rlObj, bool isLongOrDouble, bool isObject);
void GenIPut(CompilationUnit* cUnit, uint32_t fieldIdx, int optFlags, OpSize size, RegLocation rlSrc, RegLocation rlObj, bool isLongOrDouble, bool isObject);
void GenConstClass(CompilationUnit* cUnit, uint32_t type_idx, RegLocation rlDest);
void GenConstString(CompilationUnit* cUnit, uint32_t string_idx, RegLocation rlDest);
void GenNewInstance(CompilationUnit* cUnit, uint32_t type_idx, RegLocation rlDest);
void GenMoveException(CompilationUnit* cUnit, RegLocation rlDest);
void GenThrow(CompilationUnit* cUnit, RegLocation rlSrc);
void GenInstanceof(CompilationUnit* cUnit, uint32_t type_idx, RegLocation rlDest, RegLocation rlSrc);
void GenCheckCast(CompilationUnit* cUnit, uint32_t type_idx, RegLocation rlSrc);
void GenArrayObjPut(CompilationUnit* cUnit, int optFlags, RegLocation rlArray, RegLocation rlIndex, RegLocation rlSrc, int scale);
void GenArrayGet(CompilationUnit* cUnit, int optFlags, OpSize size, RegLocation rlArray, RegLocation rlIndex, RegLocation rlDest, int scale);
void GenArrayPut(CompilationUnit* cUnit, int optFlags, OpSize size, RegLocation rlArray, RegLocation rlIndex, RegLocation rlSrc, int scale);
void GenLong3Addr(CompilationUnit* cUnit, OpKind firstOp, OpKind secondOp, RegLocation rlDest, RegLocation rlSrc1, RegLocation rlSrc2);
bool GenShiftOpLong(CompilationUnit* cUnit, Instruction::Code opcode, RegLocation rlDest, RegLocation rlSrc1, RegLocation rlShift);
bool GenArithOpInt(CompilationUnit* cUnit, Instruction::Code opcode, RegLocation rlDest, RegLocation rlSrc1, RegLocation rlSrc2);
bool HandleEasyDivide(CompilationUnit* cUnit, Instruction::Code dalvikOpcode, RegLocation rlSrc, RegLocation rlDest, int lit);
bool HandleEasyMultiply(CompilationUnit* cUnit, RegLocation rlSrc, RegLocation rlDest, int lit);
bool GenArithOpIntLit(CompilationUnit* cUnit, Instruction::Code opcode, RegLocation rlDest, RegLocation rlSrc, int lit);
bool GenArithOpLong(CompilationUnit* cUnit, Instruction::Code opcode, RegLocation rlDest, RegLocation rlSrc1, RegLocation rlSrc2);
bool GenConversionCall(CompilationUnit* cUnit, int funcOffset, RegLocation rlDest, RegLocation rlSrc);
bool GenArithOpFloatPortable(CompilationUnit* cUnit, Instruction::Code opcode, RegLocation rlDest, RegLocation rlSrc1, RegLocation rlSrc2);
bool GenArithOpDoublePortable(CompilationUnit* cUnit, Instruction::Code opcode, RegLocation rlDest, RegLocation rlSrc1, RegLocation rlSrc2);
bool GenConversionPortable(CompilationUnit* cUnit, Instruction::Code opcode, RegLocation rlDest, RegLocation rlSrc);
void GenSuspendTest(CompilationUnit* cUnit, int optFlags);
void GenSuspendTestAndBranch(CompilationUnit* cUnit, int optFlags, LIR* target);

#endif // ART_SRC_COMPILER_CODEGEN_GENCOMMON_H_
