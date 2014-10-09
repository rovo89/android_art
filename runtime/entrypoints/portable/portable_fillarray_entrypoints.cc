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

#include "dex_instruction.h"
#include "entrypoints/entrypoint_utils.h"
#include "mirror/art_method-inl.h"

namespace art {

extern "C" void art_portable_fill_array_data_from_code(mirror::ArtMethod* method,
                                                       uint32_t dex_pc,
                                                       mirror::Array* array,
                                                       uint32_t payload_offset)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  const DexFile::CodeItem* code_item = method->GetCodeItem();
  const Instruction::ArrayDataPayload* payload =
      reinterpret_cast<const Instruction::ArrayDataPayload*>(code_item->insns_ + payload_offset);
  FillArrayData(array, payload);
}

}  // namespace art
