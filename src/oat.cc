// Copyright 2011 Google Inc. All Rights Reserved.

#include "oat.h"

#include <zlib.h>

namespace art {

const uint8_t OatHeader::kOatMagic[] = { 'o', 'a', 't', '\n' };
const uint8_t OatHeader::kOatVersion[] = { '0', '0', '1', '\0' };

OatHeader::OatHeader(const std::vector<const DexFile*>* dex_files) {
  memcpy(magic_, kOatMagic, sizeof(kOatMagic));
  memcpy(version_, kOatVersion, sizeof(kOatVersion));
  adler32_checksum_ = adler32(0L, Z_NULL, 0);
  dex_file_count_ = dex_files->size();
  UpdateChecksum(&dex_file_count_, sizeof(dex_file_count_));
  executable_offset_ = 0;
}

bool OatHeader::IsValid() const {
  if (memcmp(magic_, kOatMagic, sizeof(kOatMagic) != 0)) {
    return false;
  }
  if (memcmp(version_, kOatVersion, sizeof(kOatVersion) != 0)) {
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

OatMethodOffsets::OatMethodOffsets()
  : code_offset_(0),
    frame_size_in_bytes_(0),
    return_pc_offset_in_bytes_(0),
    core_spill_mask_(0),
    fp_spill_mask_(0),
    mapping_table_offset_(0),
    vmap_table_offset_(0),
    invoke_stub_offset_(0) {}

OatMethodOffsets::OatMethodOffsets(uint32_t code_offset,
                                   uint32_t frame_size_in_bytes,
                                   uint32_t return_pc_offset_in_bytes,
                                   uint32_t core_spill_mask,
                                   uint32_t fp_spill_mask,
                                   uint32_t mapping_table_offset,
                                   uint32_t vmap_table_offset,
                                   uint32_t invoke_stub_offset)
  : code_offset_(code_offset),
    frame_size_in_bytes_(frame_size_in_bytes),
    return_pc_offset_in_bytes_(return_pc_offset_in_bytes),
    core_spill_mask_(core_spill_mask),
    fp_spill_mask_(fp_spill_mask),
    mapping_table_offset_(mapping_table_offset),
    vmap_table_offset_(vmap_table_offset),
    invoke_stub_offset_(invoke_stub_offset) {}

OatMethodOffsets::~OatMethodOffsets() {}

}  // namespace art
