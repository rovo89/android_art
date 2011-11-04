// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_OAT_H_
#define ART_SRC_OAT_H_

#include <vector>

#include "dex_file.h"
#include "macros.h"

namespace art {

class PACKED OatHeader {
 public:
  OatHeader() {}
  OatHeader(const std::vector<const DexFile*>* dex_files);

  bool IsValid() const;
  const char* GetMagic() const;
  uint32_t GetChecksum() const;
  void UpdateChecksum(const void* data, size_t length);
  uint32_t GetDexFileCount() const;
  uint32_t GetExecutableOffset() const;
  void SetExecutableOffset(uint32_t executable_offset);

 private:
  static const uint8_t kOatMagic[4];
  static const uint8_t kOatVersion[4];

  uint8_t magic_[4];
  uint8_t version_[4];
  uint32_t adler32_checksum_;
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
                   uint32_t invoke_stub_offset);
  ~OatMethodOffsets();

  uint32_t code_offset_;
  uint32_t frame_size_in_bytes_;
  uint32_t core_spill_mask_;
  uint32_t fp_spill_mask_;
  uint32_t mapping_table_offset_;
  uint32_t vmap_table_offset_;
  uint32_t invoke_stub_offset_;
};

}  // namespace art

#endif  // ART_SRC_OAT_H_
