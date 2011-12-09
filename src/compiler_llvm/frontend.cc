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

#include "dex_file.h"
#include "logging.h"

#include <stdint.h>

namespace art {

int oatVRegOffset(const DexFile::CodeItem* code_item,
                  uint32_t core_spills, uint32_t fp_spills,
                  size_t frame_size, int reg) {

  // TODO: Remove oatVRegOffset() after we have adapted the OatWriter
  // and OatFile.

  UNIMPLEMENTED(WARNING) << "oatVRegOffset() is not and won't be "
                         << "implemented in LLVM backend";
  return 0;
}

} // namespace art
