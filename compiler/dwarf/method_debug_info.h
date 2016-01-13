/*
 * Copyright (C) 2015 The Android Open Source Project
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

#ifndef ART_COMPILER_DWARF_METHOD_DEBUG_INFO_H_
#define ART_COMPILER_DWARF_METHOD_DEBUG_INFO_H_

#include "dex_file.h"

namespace art {
class CompiledMethod;
namespace dwarf {

struct MethodDebugInfo {
  const DexFile* dex_file_;
  size_t class_def_index_;
  uint32_t dex_method_index_;
  uint32_t access_flags_;
  const DexFile::CodeItem* code_item_;
  bool deduped_;
  uintptr_t low_pc_;
  uintptr_t high_pc_;
  CompiledMethod* compiled_method_;
};

}  // namespace dwarf
}  // namespace art

#endif  // ART_COMPILER_DWARF_METHOD_DEBUG_INFO_H_
