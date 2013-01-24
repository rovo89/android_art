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

#ifndef ART_SRC_OAT_COMPILATION_UNIT_H_
#define ART_SRC_OAT_COMPILATION_UNIT_H_

#include "dex_file.h"

#include <stdint.h>

namespace art {
namespace mirror {
class ClassLoader;
class DexCache;
}  // namespace mirror
class ClassLinker;
class DexFile;

class OatCompilationUnit {
 public:
  OatCompilationUnit(jobject class_loader, ClassLinker* class_linker, const DexFile& dex_file,
                     const DexFile::CodeItem* code_item, uint32_t class_def_idx,
                     uint32_t method_idx, uint32_t access_flags)
      : class_loader_(class_loader), class_linker_(class_linker), dex_file_(&dex_file),
        code_item_(code_item), class_def_idx_(class_def_idx), method_idx_(method_idx),
        access_flags_(access_flags) {
  }

  OatCompilationUnit* GetCallee(uint32_t callee_method_idx,
                                uint32_t callee_access_flags) {
    return new OatCompilationUnit(class_loader_, class_linker_, *dex_file_, NULL,
                                  0, callee_method_idx, callee_access_flags);
  }

  jobject GetClassLoader() const {
    return class_loader_;
  }

  ClassLinker* GetClassLinker() const {
    return class_linker_;
  }

  const DexFile* GetDexFile() const {
    return dex_file_;
  }

  uint32_t GetClassDefIndex() const {
    return class_def_idx_;
  }

  uint32_t GetDexMethodIndex() const {
    return method_idx_;
  }

  const DexFile::CodeItem* GetCodeItem() const {
    return code_item_;
  }

  const char* GetShorty() const {
    const DexFile::MethodId& method_id = dex_file_->GetMethodId(method_idx_);
    return dex_file_->GetMethodShorty(method_id);
  }

  const char* GetShorty(uint32_t* shorty_len) const {
    const DexFile::MethodId& method_id = dex_file_->GetMethodId(method_idx_);
    return dex_file_->GetMethodShorty(method_id, shorty_len);
  }

  bool IsStatic() const {
    return ((access_flags_ & kAccStatic) != 0);
  }

 public:
  jobject class_loader_;
  ClassLinker* const class_linker_;

  const DexFile* const dex_file_;

  const DexFile::CodeItem* const code_item_;
  const uint32_t class_def_idx_;
  const uint32_t method_idx_;
  const uint32_t access_flags_;
};

} // namespace art

#endif // ART_SRC_OAT_COMPILATION_UNIT_H_
