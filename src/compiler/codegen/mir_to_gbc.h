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

#ifndef ART_SRC_COMPILER_CODEGEN_MIRTOGBC_H_
#define ART_SRC_COMPILER_CODEGEN_MIRTOGBC_H_

namespace art {

void MethodMIR2Bitcode(CompilationUnit* cu);
void MethodBitcode2LIR(CompilationUnit* cu);

}  // namespace art

#endif // ART_SRC_COMPILER_CODEGEN_MIRTOGBC_H_
