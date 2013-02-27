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

#include "oat.h"
#include "utils.h"

#include <zlib.h>

namespace art {

const uint8_t OatHeader::kOatMagic[] = { 'o', 'a', 't', '\n' };
const uint8_t OatHeader::kOatVersion[] = { '0', '0', '3', '\0' };

OatHeader::OatHeader() {
  memset(this, 0, sizeof(*this));
}

OatHeader::OatHeader(InstructionSet instruction_set,
                     const std::vector<const DexFile*>* dex_files,
                     uint32_t image_file_location_oat_checksum,
                     uint32_t image_file_location_oat_data_begin,
                     const std::string& image_file_location) {
  memcpy(magic_, kOatMagic, sizeof(kOatMagic));
  memcpy(version_, kOatVersion, sizeof(kOatVersion));

  adler32_checksum_ = adler32(0L, Z_NULL, 0);

  CHECK_NE(instruction_set, kNone);
  instruction_set_ = instruction_set;
  UpdateChecksum(&instruction_set_, sizeof(instruction_set_));

  dex_file_count_ = dex_files->size();
  UpdateChecksum(&dex_file_count_, sizeof(dex_file_count_));

  image_file_location_oat_checksum_ = image_file_location_oat_checksum;
  UpdateChecksum(&image_file_location_oat_checksum_, sizeof(image_file_location_oat_checksum_));

  CHECK(IsAligned<kPageSize>(image_file_location_oat_data_begin));
  image_file_location_oat_data_begin_ = image_file_location_oat_data_begin;
  UpdateChecksum(&image_file_location_oat_data_begin_, sizeof(image_file_location_oat_data_begin_));

  image_file_location_size_ = image_file_location.size();
  UpdateChecksum(&image_file_location_size_, sizeof(image_file_location_size_));
  UpdateChecksum(image_file_location.data(), image_file_location_size_);

  executable_offset_ = 0;
}

bool OatHeader::IsValid() const {
  if (memcmp(magic_, kOatMagic, sizeof(kOatMagic)) != 0) {
    return false;
  }
  if (memcmp(version_, kOatVersion, sizeof(kOatVersion)) != 0) {
    return false;
  }
  return true;
}

const char* OatHeader::GetMagic() const {
  CHECK(IsValid());
  return reinterpret_cast<const char*>(magic_);
}

uint32_t OatHeader::GetDexFileCount() const {
  DCHECK(IsValid());
  return dex_file_count_;
}

uint32_t OatHeader::GetChecksum() const {
  CHECK(IsValid());
  return adler32_checksum_;
}

void OatHeader::UpdateChecksum(const void* data, size_t length) {
  DCHECK(IsValid());
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
  adler32_checksum_ = adler32(adler32_checksum_, bytes, length);
}

InstructionSet OatHeader::GetInstructionSet() const {
  CHECK(IsValid());
  return instruction_set_;
}

uint32_t OatHeader::GetExecutableOffset() const {
  DCHECK(IsValid());
  DCHECK_ALIGNED(executable_offset_, kPageSize);
  CHECK_GT(executable_offset_, sizeof(OatHeader));
  return executable_offset_;
}

uint32_t OatHeader::GetImageFileLocationOatChecksum() const {
  CHECK(IsValid());
  return image_file_location_oat_checksum_;
}

uint32_t OatHeader::GetImageFileLocationOatDataBegin() const {
  CHECK(IsValid());
  return image_file_location_oat_data_begin_;
}

uint32_t OatHeader::GetImageFileLocationSize() const {
  CHECK(IsValid());
  return image_file_location_size_;
}

const uint8_t* OatHeader::GetImageFileLocationData() const {
  CHECK(IsValid());
  return image_file_location_data_;
}

std::string OatHeader::GetImageFileLocation() const {
  CHECK(IsValid());
  return std::string(reinterpret_cast<const char*>(GetImageFileLocationData()),
                     GetImageFileLocationSize());
}

void OatHeader::SetExecutableOffset(uint32_t executable_offset) {
  DCHECK_ALIGNED(executable_offset, kPageSize);
  CHECK_GT(executable_offset, sizeof(OatHeader));
  DCHECK(IsValid());
  DCHECK_EQ(executable_offset_, 0U);

  executable_offset_ = executable_offset;
  UpdateChecksum(&executable_offset_, sizeof(executable_offset));
}

OatMethodOffsets::OatMethodOffsets()
  : code_offset_(0),
    frame_size_in_bytes_(0),
    core_spill_mask_(0),
    fp_spill_mask_(0),
    mapping_table_offset_(0),
    vmap_table_offset_(0),
    gc_map_offset_(0),
    invoke_stub_offset_(0)
#if defined(ART_USE_PORTABLE_COMPILER)
  , proxy_stub_offset_(0)
#endif
{}

OatMethodOffsets::OatMethodOffsets(uint32_t code_offset,
                                   uint32_t frame_size_in_bytes,
                                   uint32_t core_spill_mask,
                                   uint32_t fp_spill_mask,
                                   uint32_t mapping_table_offset,
                                   uint32_t vmap_table_offset,
                                   uint32_t gc_map_offset,
                                   uint32_t invoke_stub_offset
#if defined(ART_USE_PORTABLE_COMPILER)
                                 , uint32_t proxy_stub_offset
#endif
                                   )
  : code_offset_(code_offset),
    frame_size_in_bytes_(frame_size_in_bytes),
    core_spill_mask_(core_spill_mask),
    fp_spill_mask_(fp_spill_mask),
    mapping_table_offset_(mapping_table_offset),
    vmap_table_offset_(vmap_table_offset),
    gc_map_offset_(gc_map_offset),
    invoke_stub_offset_(invoke_stub_offset)
#if defined(ART_USE_PORTABLE_COMPILER)
  , proxy_stub_offset_(proxy_stub_offset)
#endif
{}

OatMethodOffsets::~OatMethodOffsets() {}

}  // namespace art
