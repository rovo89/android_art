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

void FlushIns(CompilationUnit* cUnit, RegLocation* ArgLocs, RegLocation rlMethod);
int GenDalvikArgsNoRange(CompilationUnit* cUnit, CallInfo* info, int callState, LIR** pcrLabel, NextCallInsn nextCallInsn, uint32_t dexIdx, uint32_t methodIdx, uintptr_t directCode, uintptr_t directMethod, InvokeType type, bool skipThis);
int GenDalvikArgsRange(CompilationUnit* cUnit, CallInfo* info, int callState, LIR** pcrLabel, NextCallInsn nextCallInsn, uint32_t dexIdx, uint32_t methodIdx, uintptr_t directCode, uintptr_t directMethod, InvokeType type, bool skipThis);
RegLocation InlineTarget(CompilationUnit* cUnit, CallInfo* info);
RegLocation InlineTargetWide(CompilationUnit* cUnit, CallInfo* info);
bool GenInlinedCharAt(CompilationUnit* cUnit, CallInfo* info);
bool GenInlinedStringIsEmptyOrLength(CompilationUnit* cUnit, CallInfo* info, bool isEmpty);
bool GenInlinedAbsInt(CompilationUnit *cUnit, CallInfo* info);
bool GenInlinedAbsLong(CompilationUnit *cUnit, CallInfo* info);
bool GenInlinedFloatCvt(CompilationUnit *cUnit, CallInfo* info);
bool GenInlinedDoubleCvt(CompilationUnit *cUnit, CallInfo* info);
bool GenInlinedIndexOf(CompilationUnit* cUnit, CallInfo* info, bool zeroBased);
bool GenInlinedStringCompareTo(CompilationUnit* cUnit, CallInfo* info);
bool GenIntrinsic(CompilationUnit* cUnit, CallInfo* info);
void GenInvoke(CompilationUnit* cUnit, CallInfo* info);
CallInfo* NewMemCallInfo(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir, InvokeType type, bool isRange);

#endif // ART_SRC_COMPILER_CODEGEN_GENINVOKE_H_
