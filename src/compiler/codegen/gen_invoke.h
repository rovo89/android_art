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

typedef int (*NextCallInsn)(CompilationUnit*, CallInfo*, int, uint32_t dex_idx,
                            uint32_t method_idx, uintptr_t direct_code,
                            uintptr_t direct_method, InvokeType type);

void FlushIns(CompilationUnit* cu, RegLocation* ArgLocs, RegLocation rl_method);
int GenDalvikArgsNoRange(CompilationUnit* cu, CallInfo* info, int call_state, LIR** pcrLabel, NextCallInsn next_call_insn, uint32_t dex_idx, uint32_t method_idx, uintptr_t direct_code, uintptr_t direct_method, InvokeType type, bool skip_this);
int GenDalvikArgsRange(CompilationUnit* cu, CallInfo* info, int call_state, LIR** pcrLabel, NextCallInsn next_call_insn, uint32_t dex_idx, uint32_t method_idx, uintptr_t direct_code, uintptr_t direct_method, InvokeType type, bool skip_this);
RegLocation InlineTarget(CompilationUnit* cu, CallInfo* info);
RegLocation InlineTargetWide(CompilationUnit* cu, CallInfo* info);
bool GenInlinedCharAt(CompilationUnit* cu, CallInfo* info);
bool GenInlinedStringIsEmptyOrLength(CompilationUnit* cu, CallInfo* info, bool is_empty);
bool GenInlinedAbsInt(CompilationUnit *cu, CallInfo* info);
bool GenInlinedAbsLong(CompilationUnit *cu, CallInfo* info);
bool GenInlinedFloatCvt(CompilationUnit *cu, CallInfo* info);
bool GenInlinedDoubleCvt(CompilationUnit *cu, CallInfo* info);
bool GenInlinedIndexOf(CompilationUnit* cu, CallInfo* info, bool zero_based);
bool GenInlinedStringCompareTo(CompilationUnit* cu, CallInfo* info);
bool GenIntrinsic(CompilationUnit* cu, CallInfo* info);
void GenInvoke(CompilationUnit* cu, CallInfo* info);
CallInfo* NewMemCallInfo(CompilationUnit* cu, BasicBlock* bb, MIR* mir, InvokeType type, bool is_range);

#endif // ART_SRC_COMPILER_CODEGEN_GENINVOKE_H_
