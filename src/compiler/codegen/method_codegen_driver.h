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

#ifndef ART_SRC_COMPILER_CODEGEN_METHODCODEGENDRIVER_H_
#define ART_SRC_COMPILER_CODEGEN_METHODCODEGENDRIVER_H_

namespace art {
// TODO: move GenInvoke to gen_invoke.cc
void GenInvoke(CompilationUnit* cUnit, CallInfo* info);
// TODO: move GenInvoke to gen_invoke.cc or utils
CallInfo* NewMemCallInfo(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir, InvokeType type, bool isRange);
void SpecialMIR2LIR(CompilationUnit* cUnit, SpecialCaseHandler specialCase);
void MethodMIR2LIR(CompilationUnit* cUnit);


}  // namespace art

#endif // ART_SRC_COMPILER_CODEGEN_METHODCODEGENDRIVER_H_
