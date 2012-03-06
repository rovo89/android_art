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

#ifndef ART_SRC_OAT_H_
#define ART_SRC_OAT_H_

#include <vector>

#include "constants.h"
#include "dex_file.h"
#include "macros.h"

namespace art {

class PACKED OatHeader {
 public:
  OatHeader();
  OatHeader(InstructionSet instruction_set, const std::vector<const DexFile*>* dex_files);

  bool IsValid() const;
  const char* GetMagic() const;
  uint32_t GetChecksum() const;
  void UpdateChecksum(const void* data, size_t length);
  uint32_t GetDexFileCount() const;
  uint32_t GetExecutableOffset() const;
  InstructionSet GetInstructionSet() const;
  void SetExecutableOffset(uint32_t executable_offset);

 private:
  static const uint8_t kOatMagic[4];
  static const uint8_t kOatVersion[4];

  uint8_t magic_[4];
  uint8_t version_[4];
  uint32_t adler32_checksum_;

  InstructionSet instruction_set_;
  uint32_t dex_file_count_;
  uint32_t executable_offset_;

  DISALLOW_COPY_AND_ASSIGN(OatHeader);
};

class PACKED OatMethodOffsets {
 public:
  OatMethodOffsets();
  OatMethodOffsets(uint32_t code_offset,
                   uint32_t frame_size_in_bytes,
                   uint32_t core_spill_mask,
                   uint32_t fp_spill_mask,
                   uint32_t mapping_table_offset,
                   uint32_t vmap_table_offset,
                   uint32_t gc_map_offset,
                   uint32_t invoke_stub_offset);
  ~OatMethodOffsets();

  uint32_t code_offset_;
  uint32_t frame_size_in_bytes_;
  uint32_t core_spill_mask_;
  uint32_t fp_spill_mask_;
  uint32_t mapping_table_offset_;
  uint32_t vmap_table_offset_;
  uint32_t gc_map_offset_;
  uint32_t invoke_stub_offset_;
};

}  // namespace art

#endif  // ART_SRC_OAT_H_
