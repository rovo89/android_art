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

#ifndef ART_SRC_COMPILER_CODEGEN_GENINVOKE_H_
#define ART_SRC_COMPILER_CODEGEN_GENINVOKE_H_

typedef int (*NextCallInsn)(CompilationUnit*, CallInfo*, int, uint32_t dexIdx,
                            uint32_t methodIdx, uintptr_t directCode,
                            uintptr_t directMethod, InvokeType type);

void flushIns(CompilationUnit* cUnit, RegLocation* argLocs, RegLocation rlMethod);
int genDalvikArgsNoRange(CompilationUnit* cUnit, CallInfo* info, int callState, LIR** pcrLabel, NextCallInsn nextCallInsn, uint32_t dexIdx, uint32_t methodIdx, uintptr_t directCode, uintptr_t directMethod, InvokeType type, bool skipThis);
int genDalvikArgsRange(CompilationUnit* cUnit, CallInfo* info, int callState, LIR** pcrLabel, NextCallInsn nextCallInsn, uint32_t dexIdx, uint32_t methodIdx, uintptr_t directCode, uintptr_t directMethod, InvokeType type, bool skipThis);
RegLocation inlineTarget(CompilationUnit* cUnit, CallInfo* info);
RegLocation inlineTargetWide(CompilationUnit* cUnit, CallInfo* info);
bool genInlinedCharAt(CompilationUnit* cUnit, CallInfo* info);
bool genInlinedStringIsEmptyOrLength(CompilationUnit* cUnit, CallInfo* info, bool isEmpty);
bool genInlinedAbsInt(CompilationUnit *cUnit, CallInfo* info);
bool genInlinedAbsLong(CompilationUnit *cUnit, CallInfo* info);
bool genInlinedFloatCvt(CompilationUnit *cUnit, CallInfo* info);
bool genInlinedDoubleCvt(CompilationUnit *cUnit, CallInfo* info);
bool genInlinedIndexOf(CompilationUnit* cUnit, CallInfo* info, bool zeroBased);
bool genInlinedStringCompareTo(CompilationUnit* cUnit, CallInfo* info);
bool genIntrinsic(CompilationUnit* cUnit, CallInfo* info);
void genInvoke(CompilationUnit* cUnit, CallInfo* info);
CallInfo* oatNewCallInfo(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir, InvokeType type, bool isRange);

#endif // ART_SRC_COMPILER_CODEGEN_GENINVOKE_H_
