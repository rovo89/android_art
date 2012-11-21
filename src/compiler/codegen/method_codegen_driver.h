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
void GenInvoke(CompilationUnit* cu, CallInfo* info);
// TODO: move GenInvoke to gen_invoke.cc or utils
CallInfo* NewMemCallInfo(CompilationUnit* cu, BasicBlock* bb, MIR* mir, InvokeType type, bool is_range);
void SpecialMIR2LIR(CompilationUnit* cu, SpecialCaseHandler special_case);
void MethodMIR2LIR(CompilationUnit* cu);


}  // namespace art

#endif // ART_SRC_COMPILER_CODEGEN_METHODCODEGENDRIVER_H_
