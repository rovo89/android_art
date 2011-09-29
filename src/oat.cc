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
  DCHECK(IsAligned(executable_offset_, kPageSize));
  CHECK_GT(executable_offset_, sizeof(OatHeader));
  return executable_offset_;
}

void OatHeader::SetExecutableOffset(uint32_t executable_offset) {
  DCHECK(IsAligned(executable_offset, kPageSize));
  CHECK_GT(executable_offset, sizeof(OatHeader));
  DCHECK(IsValid());
  DCHECK_EQ(executable_offset_, 0U);
  executable_offset_ = executable_offset;
  UpdateChecksum(&executable_offset_, sizeof(executable_offset));
}

}  // namespace art
