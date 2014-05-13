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
const uint8_t OatHeader::kOatVersion[] = { '0', '2', '8', '\0' };

OatHeader::OatHeader() {
  memset(this, 0, sizeof(*this));
}

OatHeader::OatHeader(InstructionSet instruction_set,
                     const InstructionSetFeatures& instruction_set_features,
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

  instruction_set_features_ = instruction_set_features;
  UpdateChecksum(&instruction_set_features_, sizeof(instruction_set_features_));

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
  interpreter_to_interpreter_bridge_offset_ = 0;
  interpreter_to_compiled_code_bridge_offset_ = 0;
  jni_dlsym_lookup_offset_ = 0;
  portable_imt_conflict_trampoline_offset_ = 0;
  portable_resolution_trampoline_offset_ = 0;
  portable_to_interpreter_bridge_offset_ = 0;
  quick_generic_jni_trampoline_offset_ = 0;
  quick_imt_conflict_trampoline_offset_ = 0;
  quick_resolution_trampoline_offset_ = 0;
  quick_to_interpreter_bridge_offset_ = 0;
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

const InstructionSetFeatures& OatHeader::GetInstructionSetFeatures() const {
  CHECK(IsValid());
  return instruction_set_features_;
}

uint32_t OatHeader::GetExecutableOffset() const {
  DCHECK(IsValid());
  DCHECK_ALIGNED(executable_offset_, kPageSize);
  CHECK_GT(executable_offset_, sizeof(OatHeader));
  return executable_offset_;
}

void OatHeader::SetExecutableOffset(uint32_t executable_offset) {
  DCHECK_ALIGNED(executable_offset, kPageSize);
  CHECK_GT(executable_offset, sizeof(OatHeader));
  DCHECK(IsValid());
  DCHECK_EQ(executable_offset_, 0U);

  executable_offset_ = executable_offset;
  UpdateChecksum(&executable_offset_, sizeof(executable_offset));
}

const void* OatHeader::GetInterpreterToInterpreterBridge() const {
  return reinterpret_cast<const uint8_t*>(this) + GetInterpreterToInterpreterBridgeOffset();
}

uint32_t OatHeader::GetInterpreterToInterpreterBridgeOffset() const {
  DCHECK(IsValid());
  CHECK(interpreter_to_interpreter_bridge_offset_ == 0 ||
        interpreter_to_interpreter_bridge_offset_ >= executable_offset_);
  return interpreter_to_interpreter_bridge_offset_;
}

void OatHeader::SetInterpreterToInterpreterBridgeOffset(uint32_t offset) {
  CHECK(offset == 0 || offset >= executable_offset_);
  DCHECK(IsValid());
  DCHECK_EQ(interpreter_to_interpreter_bridge_offset_, 0U) << offset;

  interpreter_to_interpreter_bridge_offset_ = offset;
  UpdateChecksum(&interpreter_to_interpreter_bridge_offset_, sizeof(offset));
}

const void* OatHeader::GetInterpreterToCompiledCodeBridge() const {
  return reinterpret_cast<const uint8_t*>(this) + GetInterpreterToCompiledCodeBridgeOffset();
}

uint32_t OatHeader::GetInterpreterToCompiledCodeBridgeOffset() const {
  DCHECK(IsValid());
  CHECK_GE(interpreter_to_compiled_code_bridge_offset_, interpreter_to_interpreter_bridge_offset_);
  return interpreter_to_compiled_code_bridge_offset_;
}

void OatHeader::SetInterpreterToCompiledCodeBridgeOffset(uint32_t offset) {
  CHECK(offset == 0 || offset >= interpreter_to_interpreter_bridge_offset_);
  DCHECK(IsValid());
  DCHECK_EQ(interpreter_to_compiled_code_bridge_offset_, 0U) << offset;

  interpreter_to_compiled_code_bridge_offset_ = offset;
  UpdateChecksum(&interpreter_to_compiled_code_bridge_offset_, sizeof(offset));
}

const void* OatHeader::GetJniDlsymLookup() const {
  return reinterpret_cast<const uint8_t*>(this) + GetJniDlsymLookupOffset();
}

uint32_t OatHeader::GetJniDlsymLookupOffset() const {
  DCHECK(IsValid());
  CHECK_GE(jni_dlsym_lookup_offset_, interpreter_to_compiled_code_bridge_offset_);
  return jni_dlsym_lookup_offset_;
}

void OatHeader::SetJniDlsymLookupOffset(uint32_t offset) {
  CHECK(offset == 0 || offset >= interpreter_to_compiled_code_bridge_offset_);
  DCHECK(IsValid());
  DCHECK_EQ(jni_dlsym_lookup_offset_, 0U) << offset;

  jni_dlsym_lookup_offset_ = offset;
  UpdateChecksum(&jni_dlsym_lookup_offset_, sizeof(offset));
}

const void* OatHeader::GetPortableImtConflictTrampoline() const {
  return reinterpret_cast<const uint8_t*>(this) + GetPortableImtConflictTrampolineOffset();
}

uint32_t OatHeader::GetPortableImtConflictTrampolineOffset() const {
  DCHECK(IsValid());
  CHECK_GE(portable_imt_conflict_trampoline_offset_, jni_dlsym_lookup_offset_);
  return portable_imt_conflict_trampoline_offset_;
}

void OatHeader::SetPortableImtConflictTrampolineOffset(uint32_t offset) {
  CHECK(offset == 0 || offset >= jni_dlsym_lookup_offset_);
  DCHECK(IsValid());
  DCHECK_EQ(portable_imt_conflict_trampoline_offset_, 0U) << offset;

  portable_imt_conflict_trampoline_offset_ = offset;
  UpdateChecksum(&portable_imt_conflict_trampoline_offset_, sizeof(offset));
}

const void* OatHeader::GetPortableResolutionTrampoline() const {
  return reinterpret_cast<const uint8_t*>(this) + GetPortableResolutionTrampolineOffset();
}

uint32_t OatHeader::GetPortableResolutionTrampolineOffset() const {
  DCHECK(IsValid());
  CHECK_GE(portable_resolution_trampoline_offset_, portable_imt_conflict_trampoline_offset_);
  return portable_resolution_trampoline_offset_;
}

void OatHeader::SetPortableResolutionTrampolineOffset(uint32_t offset) {
  CHECK(offset == 0 || offset >= portable_imt_conflict_trampoline_offset_);
  DCHECK(IsValid());
  DCHECK_EQ(portable_resolution_trampoline_offset_, 0U) << offset;

  portable_resolution_trampoline_offset_ = offset;
  UpdateChecksum(&portable_resolution_trampoline_offset_, sizeof(offset));
}

const void* OatHeader::GetPortableToInterpreterBridge() const {
  return reinterpret_cast<const uint8_t*>(this) + GetPortableToInterpreterBridgeOffset();
}

uint32_t OatHeader::GetPortableToInterpreterBridgeOffset() const {
  DCHECK(IsValid());
  CHECK_GE(portable_to_interpreter_bridge_offset_, portable_resolution_trampoline_offset_);
  return portable_to_interpreter_bridge_offset_;
}

void OatHeader::SetPortableToInterpreterBridgeOffset(uint32_t offset) {
  CHECK(offset == 0 || offset >= portable_resolution_trampoline_offset_);
  DCHECK(IsValid());
  DCHECK_EQ(portable_to_interpreter_bridge_offset_, 0U) << offset;

  portable_to_interpreter_bridge_offset_ = offset;
  UpdateChecksum(&portable_to_interpreter_bridge_offset_, sizeof(offset));
}

const void* OatHeader::GetQuickGenericJniTrampoline() const {
  return reinterpret_cast<const uint8_t*>(this) + GetQuickGenericJniTrampolineOffset();
}

uint32_t OatHeader::GetQuickGenericJniTrampolineOffset() const {
  DCHECK(IsValid());
  CHECK_GE(quick_generic_jni_trampoline_offset_, portable_to_interpreter_bridge_offset_);
  return quick_generic_jni_trampoline_offset_;
}

void OatHeader::SetQuickGenericJniTrampolineOffset(uint32_t offset) {
  CHECK(offset == 0 || offset >= portable_to_interpreter_bridge_offset_);
  DCHECK(IsValid());
  DCHECK_EQ(quick_generic_jni_trampoline_offset_, 0U) << offset;

  quick_generic_jni_trampoline_offset_ = offset;
  UpdateChecksum(&quick_generic_jni_trampoline_offset_, sizeof(offset));
}

const void* OatHeader::GetQuickImtConflictTrampoline() const {
  return reinterpret_cast<const uint8_t*>(this) + GetQuickImtConflictTrampolineOffset();
}

uint32_t OatHeader::GetQuickImtConflictTrampolineOffset() const {
  DCHECK(IsValid());
  CHECK_GE(quick_imt_conflict_trampoline_offset_, quick_generic_jni_trampoline_offset_);
  return quick_imt_conflict_trampoline_offset_;
}

void OatHeader::SetQuickImtConflictTrampolineOffset(uint32_t offset) {
  CHECK(offset == 0 || offset >= quick_generic_jni_trampoline_offset_);
  DCHECK(IsValid());
  DCHECK_EQ(quick_imt_conflict_trampoline_offset_, 0U) << offset;

  quick_imt_conflict_trampoline_offset_ = offset;
  UpdateChecksum(&quick_imt_conflict_trampoline_offset_, sizeof(offset));
}

const void* OatHeader::GetQuickResolutionTrampoline() const {
  return reinterpret_cast<const uint8_t*>(this) + GetQuickResolutionTrampolineOffset();
}

uint32_t OatHeader::GetQuickResolutionTrampolineOffset() const {
  DCHECK(IsValid());
  CHECK_GE(quick_resolution_trampoline_offset_, quick_imt_conflict_trampoline_offset_);
  return quick_resolution_trampoline_offset_;
}

void OatHeader::SetQuickResolutionTrampolineOffset(uint32_t offset) {
  CHECK(offset == 0 || offset >= quick_imt_conflict_trampoline_offset_);
  DCHECK(IsValid());
  DCHECK_EQ(quick_resolution_trampoline_offset_, 0U) << offset;

  quick_resolution_trampoline_offset_ = offset;
  UpdateChecksum(&quick_resolution_trampoline_offset_, sizeof(offset));
}

const void* OatHeader::GetQuickToInterpreterBridge() const {
  return reinterpret_cast<const uint8_t*>(this) + GetQuickToInterpreterBridgeOffset();
}

uint32_t OatHeader::GetQuickToInterpreterBridgeOffset() const {
  DCHECK(IsValid());
  CHECK_GE(quick_to_interpreter_bridge_offset_, quick_resolution_trampoline_offset_);
  return quick_to_interpreter_bridge_offset_;
}

void OatHeader::SetQuickToInterpreterBridgeOffset(uint32_t offset) {
  CHECK(offset == 0 || offset >= quick_resolution_trampoline_offset_);
  DCHECK(IsValid());
  DCHECK_EQ(quick_to_interpreter_bridge_offset_, 0U) << offset;

  quick_to_interpreter_bridge_offset_ = offset;
  UpdateChecksum(&quick_to_interpreter_bridge_offset_, sizeof(offset));
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

OatMethodOffsets::OatMethodOffsets()
  : code_offset_(0),
    gc_map_offset_(0)
{}

OatMethodOffsets::OatMethodOffsets(uint32_t code_offset,
                                   uint32_t gc_map_offset
                                   )
  : code_offset_(code_offset),
    gc_map_offset_(gc_map_offset)
{}

OatMethodOffsets::~OatMethodOffsets() {}

OatQuickMethodHeader::OatQuickMethodHeader()
  : mapping_table_offset_(0),
    vmap_table_offset_(0),
    frame_info_(0, 0, 0),
    code_size_(0)
{}

OatQuickMethodHeader::OatQuickMethodHeader(
    uint32_t mapping_table_offset, uint32_t vmap_table_offset, uint32_t frame_size_in_bytes,
    uint32_t core_spill_mask, uint32_t fp_spill_mask, uint32_t code_size)
  : mapping_table_offset_(mapping_table_offset),
    vmap_table_offset_(vmap_table_offset),
    frame_info_(frame_size_in_bytes, core_spill_mask, fp_spill_mask),
    code_size_(code_size)
{}

OatQuickMethodHeader::~OatQuickMethodHeader() {}

}  // namespace art
