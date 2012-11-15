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

void markSafepointPC(CompilationUnit* cUnit, LIR* inst);
void callRuntimeHelperImm(CompilationUnit* cUnit, int helperOffset, int arg0, bool safepointPC);
void callRuntimeHelperReg(CompilationUnit* cUnit, int helperOffset, int arg0, bool safepointPC);
void callRuntimeHelperRegLocation(CompilationUnit* cUnit, int helperOffset, RegLocation arg0, bool safepointPC);
void callRuntimeHelperImmImm(CompilationUnit* cUnit, int helperOffset, int arg0, int arg1, bool safepointPC);
void callRuntimeHelperImmRegLocation(CompilationUnit* cUnit, int helperOffset, int arg0, RegLocation arg1, bool safepointPC);
void callRuntimeHelperRegLocationImm(CompilationUnit* cUnit, int helperOffset, RegLocation arg0, int arg1, bool safepointPC);
void callRuntimeHelperImmReg(CompilationUnit* cUnit, int helperOffset, int arg0, int arg1, bool safepointPC);
void callRuntimeHelperRegImm(CompilationUnit* cUnit, int helperOffset, int arg0, int arg1, bool safepointPC);
void callRuntimeHelperImmMethod(CompilationUnit* cUnit, int helperOffset, int arg0, bool safepointPC);
void callRuntimeHelperRegLocationRegLocation(CompilationUnit* cUnit, int helperOffset, RegLocation arg0, RegLocation arg1, bool safepointPC);
void callRuntimeHelperRegReg(CompilationUnit* cUnit, int helperOffset, int arg0, int arg1, bool safepointPC);
void callRuntimeHelperRegRegImm(CompilationUnit* cUnit, int helperOffset, int arg0, int arg1, int arg2, bool safepointPC);
void callRuntimeHelperImmMethodRegLocation(CompilationUnit* cUnit, int helperOffset, int arg0, RegLocation arg2, bool safepointPC);
void callRuntimeHelperImmMethodImm(CompilationUnit* cUnit, int helperOffset, int arg0, int arg2, bool safepointPC);
void callRuntimeHelperImmRegLocationRegLocation(CompilationUnit* cUnit, int helperOffset, int arg0, RegLocation arg1, RegLocation arg2, bool safepointPC);
void genBarrier(CompilationUnit* cUnit);
LIR* opUnconditionalBranch(CompilationUnit* cUnit, LIR* target);
LIR* genCheck(CompilationUnit* cUnit, ConditionCode cCode, ThrowKind kind);
LIR* genImmedCheck(CompilationUnit* cUnit, ConditionCode cCode, int reg, int immVal, ThrowKind kind);
LIR* genNullCheck(CompilationUnit* cUnit, int sReg, int mReg, int optFlags);
LIR* genRegRegCheck(CompilationUnit* cUnit, ConditionCode cCode, int reg1, int reg2, ThrowKind kind);
void genCompareAndBranch(CompilationUnit* cUnit, Instruction::Code opcode, RegLocation rlSrc1, RegLocation rlSrc2, LIR* taken, LIR* fallThrough);
void genCompareZeroAndBranch(CompilationUnit* cUnit, Instruction::Code opcode, RegLocation rlSrc, LIR* taken, LIR* fallThrough);
void genIntToLong(CompilationUnit* cUnit, RegLocation rlDest, RegLocation rlSrc);
void genIntNarrowing(CompilationUnit* cUnit, Instruction::Code opcode, RegLocation rlDest, RegLocation rlSrc);
void genNewArray(CompilationUnit* cUnit, uint32_t type_idx, RegLocation rlDest, RegLocation rlSrc);
void genFilledNewArray(CompilationUnit* cUnit, CallInfo* info);
void genSput(CompilationUnit* cUnit, uint32_t fieldIdx, RegLocation rlSrc, bool isLongOrDouble, bool isObject);
void genSget(CompilationUnit* cUnit, uint32_t fieldIdx, RegLocation rlDest, bool isLongOrDouble, bool isObject);
void genShowTarget(CompilationUnit* cUnit);
void handleSuspendLaunchpads(CompilationUnit *cUnit);
void handleIntrinsicLaunchpads(CompilationUnit *cUnit);
void handleThrowLaunchpads(CompilationUnit *cUnit);
void oatSetupResourceMasks(CompilationUnit* cUnit, LIR* lir);
bool fastInstance(CompilationUnit* cUnit,  uint32_t fieldIdx, int& fieldOffset, bool& isVolatile, bool isPut);
void genIGet(CompilationUnit* cUnit, uint32_t fieldIdx, int optFlags, OpSize size, RegLocation rlDest, RegLocation rlObj, bool isLongOrDouble, bool isObject);
void genIPut(CompilationUnit* cUnit, uint32_t fieldIdx, int optFlags, OpSize size, RegLocation rlSrc, RegLocation rlObj, bool isLongOrDouble, bool isObject);
void genConstClass(CompilationUnit* cUnit, uint32_t type_idx, RegLocation rlDest);
void genConstString(CompilationUnit* cUnit, uint32_t string_idx, RegLocation rlDest);
void genNewInstance(CompilationUnit* cUnit, uint32_t type_idx, RegLocation rlDest);
void genMoveException(CompilationUnit* cUnit, RegLocation rlDest);
void genThrow(CompilationUnit* cUnit, RegLocation rlSrc);
void genInstanceof(CompilationUnit* cUnit, uint32_t type_idx, RegLocation rlDest, RegLocation rlSrc);
void genCheckCast(CompilationUnit* cUnit, uint32_t type_idx, RegLocation rlSrc);
void genArrayObjPut(CompilationUnit* cUnit, int optFlags, RegLocation rlArray, RegLocation rlIndex, RegLocation rlSrc, int scale);
void genArrayGet(CompilationUnit* cUnit, int optFlags, OpSize size, RegLocation rlArray, RegLocation rlIndex, RegLocation rlDest, int scale);
void genArrayPut(CompilationUnit* cUnit, int optFlags, OpSize size, RegLocation rlArray, RegLocation rlIndex, RegLocation rlSrc, int scale);
void genLong3Addr(CompilationUnit* cUnit, OpKind firstOp, OpKind secondOp, RegLocation rlDest, RegLocation rlSrc1, RegLocation rlSrc2);
bool genShiftOpLong(CompilationUnit* cUnit, Instruction::Code opcode, RegLocation rlDest, RegLocation rlSrc1, RegLocation rlShift);
bool genArithOpInt(CompilationUnit* cUnit, Instruction::Code opcode, RegLocation rlDest, RegLocation rlSrc1, RegLocation rlSrc2);
bool handleEasyDivide(CompilationUnit* cUnit, Instruction::Code dalvikOpcode, RegLocation rlSrc, RegLocation rlDest, int lit);
bool handleEasyMultiply(CompilationUnit* cUnit, RegLocation rlSrc, RegLocation rlDest, int lit);
bool genArithOpIntLit(CompilationUnit* cUnit, Instruction::Code opcode, RegLocation rlDest, RegLocation rlSrc, int lit);
bool genArithOpLong(CompilationUnit* cUnit, Instruction::Code opcode, RegLocation rlDest, RegLocation rlSrc1, RegLocation rlSrc2);
bool genConversionCall(CompilationUnit* cUnit, int funcOffset, RegLocation rlDest, RegLocation rlSrc);
bool genArithOpFloatPortable(CompilationUnit* cUnit, Instruction::Code opcode, RegLocation rlDest, RegLocation rlSrc1, RegLocation rlSrc2);
bool genArithOpDoublePortable(CompilationUnit* cUnit, Instruction::Code opcode, RegLocation rlDest, RegLocation rlSrc1, RegLocation rlSrc2);
bool genConversionPortable(CompilationUnit* cUnit, Instruction::Code opcode, RegLocation rlDest, RegLocation rlSrc);
void genSuspendTest(CompilationUnit* cUnit, int optFlags);
void genSuspendTestAndBranch(CompilationUnit* cUnit, int optFlags, LIR* target);

#endif // ART_SRC_COMPILER_CODEGEN_GENCOMMON_H_
