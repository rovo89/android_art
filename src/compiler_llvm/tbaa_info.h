/*
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef ART_SRC_COMPILER_LLVM_TBAA_INFO_H_
#define ART_SRC_COMPILER_LLVM_TBAA_INFO_H_

#include "backend_types.h"

#include <cstring>

namespace llvm {
  class LLVMContext;
  class MDNode;
  class StringRef;
}

namespace art {
namespace compiler_llvm {


class TBAAInfo {
 public:
  TBAAInfo(llvm::LLVMContext& context) : context_(context), root_(NULL) {
    std::memset(special_type_, 0, sizeof(special_type_));
    std::memset(memory_jtype_, 0, sizeof(memory_jtype_));
  }

  llvm::MDNode* GetRootType();

  llvm::MDNode* GetSpecialType(TBAASpecialType special_ty);

  llvm::MDNode* GetMemoryJType(TBAASpecialType special_ty, JType j_ty);

  llvm::MDNode* GenTBAANode(llvm::StringRef name,
                            llvm::MDNode* parent = NULL,
                            bool readonly = false);

 private:
  llvm::LLVMContext& context_;
  llvm::MDNode* root_;
  llvm::MDNode* special_type_[MAX_TBAA_SPECIAL_TYPE];
  // There are 3 categories of memory types will not alias: array element, instance field, and
  // static field.
  llvm::MDNode* memory_jtype_[3][MAX_JTYPE];
};


} // namespace compiler_llvm
} // namespace art

#endif // ART_SRC_COMPILER_LLVM_TBAA_INFO_H_
