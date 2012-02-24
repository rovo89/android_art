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

#ifndef ART_SRC_UTILS_LLVM_H_
#define ART_SRC_UTILS_LLVM_H_

#include "object.h"

#include <string>

namespace art {

// Performs LLVM name mangling (similar to MangleForJni with additional '<' and
// '>' being mangled).
std::string MangleForLLVM(const std::string& s);

// Returns the LLVM function name for the non-overloaded method 'm'.
std::string LLVMShortName(const Method* m);

// Returns the LLVM function name for the overloaded method 'm'.
std::string LLVMLongName(const Method* m);

// Returns the LLVM stub function name for the overloaded method 'm'.
std::string LLVMStubName(const Method* m);

void LLVMLinkLoadMethod(const std::string& file_name, Method* method);
}  // namespace art

#endif  // ART_SRC_UTILS_LLVM_H_
