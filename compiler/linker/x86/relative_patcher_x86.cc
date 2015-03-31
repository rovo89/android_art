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

#include "linker/x86/relative_patcher_x86.h"

namespace art {
namespace linker {

void X86RelativePatcher::PatchDexCacheReference(std::vector<uint8_t>* code ATTRIBUTE_UNUSED,
                                                const LinkerPatch& patch ATTRIBUTE_UNUSED,
                                                uint32_t patch_offset ATTRIBUTE_UNUSED,
                                                uint32_t target_offset ATTRIBUTE_UNUSED) {
  LOG(FATAL) << "Unexpected relative dex cache array patch.";
}

}  // namespace linker
}  // namespace art
