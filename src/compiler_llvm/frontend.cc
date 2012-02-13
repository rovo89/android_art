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

#include "method_compiler.h"

#include "class_linker.h"
#include "class_loader.h"
#include "compiler.h"
#include "constants.h"
#include "dex_file.h"
#include "runtime.h"

#include <UniquePtr.h>
#include <stdint.h>

using namespace art::compiler_llvm;

namespace art {

int oatVRegOffset(const art::DexFile::CodeItem* code_item,
                  uint32_t core_spills, uint32_t fp_spills,
                  size_t frame_size, int reg) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

} // namespace art
