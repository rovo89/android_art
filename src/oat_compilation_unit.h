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

class ClassLoader;
class ClassLinker;
class DexFile;
class DexCache;

class OatCompilationUnit {
 public:
  OatCompilationUnit(const ClassLoader* class_loader, ClassLinker* class_linker,
                     const DexFile& dex_file, DexCache& dex_cache,
                     const DexFile::CodeItem* code_item,
                     uint32_t method_idx, uint32_t access_flags)
      : class_loader_(class_loader), class_linker_(class_linker),
        dex_file_(&dex_file), dex_cache_(&dex_cache), code_item_(code_item),
        method_idx_(method_idx), access_flags_(access_flags) {
  }

  OatCompilationUnit* GetCallee(uint32_t callee_method_idx,
                                uint32_t callee_access_flags) {
    return new OatCompilationUnit(class_loader_, class_linker_, *dex_file_,
                                  *dex_cache_, NULL, callee_method_idx,
                                  callee_access_flags);
  }

  const ClassLoader* GetClassLoader() const {
    return class_loader_;
  }

  ClassLinker* GetClassLinker() const {
    return class_linker_;
  }

  const DexFile* GetDexFile() const {
    return dex_file_;
  }

  DexCache* GetDexCache() const {
    return dex_cache_;
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
  const ClassLoader* class_loader_;
  ClassLinker* class_linker_;

  const DexFile* dex_file_;
  DexCache* dex_cache_;

  const DexFile::CodeItem* code_item_;
  uint32_t method_idx_;
  uint32_t access_flags_;
};

} // namespace art

#endif // ART_SRC_OAT_COMPILATION_UNIT_H_
