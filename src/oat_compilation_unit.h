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

#ifndef ART_SRC_METHOD_UNIT_H_
#define ART_SRC_METHOD_UNIT_H_

#include <stdint.h>

namespace art {

class ClassLoader;
class ClassLinker;
class DexFile;
class DexCache;

class OatCompilationUnit {
 public:
  OatCompilationUnit(ClassLoader const* class_loader, ClassLinker* class_linker,
                     DexFile const& dex_file, DexCache& dex_cache,
                     DexFile::CodeItem const* code_item,
                     uint32_t method_idx, uint32_t access_flags)
      : class_loader_(class_loader), class_linker_(class_linker),
        dex_file_(&dex_file), dex_cache_(&dex_cache), code_item_(code_item),
        method_idx_(method_idx), access_flags_(access_flags) {
  }

 public:
  ClassLoader const* class_loader_;
  ClassLinker* class_linker_;

  DexFile const* dex_file_;
  DexCache* dex_cache_;

  DexFile::CodeItem const* code_item_;
  uint32_t method_idx_;
  uint32_t access_flags_;
};

} // namespace art

#endif // ART_SRC_METHOD_UNIT_H_
