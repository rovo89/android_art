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

#ifndef ART_RUNTIME_OAT_H_
#define ART_RUNTIME_OAT_H_

#include <vector>

#include "base/macros.h"
#include "dex_file.h"
#include "instruction_set.h"
#include "quick/quick_method_frame_info.h"

namespace art {

class PACKED(4) OatHeader {
 public:
  static const uint8_t kOatMagic[4];
  static const uint8_t kOatVersion[4];

  OatHeader();
  OatHeader(InstructionSet instruction_set,
            const InstructionSetFeatures& instruction_set_features,
            const std::vector<const DexFile*>* dex_files,
            uint32_t image_file_location_oat_checksum,
            uint32_t image_file_location_oat_data_begin,
            const std::string& image_file_location);

  bool IsValid() const;
  const char* GetMagic() const;
  uint32_t GetChecksum() const;
  void UpdateChecksum(const void* data, size_t length);
  uint32_t GetDexFileCount() const {
    DCHECK(IsValid());
    return dex_file_count_;
  }
  uint32_t GetExecutableOffset() const;
  void SetExecutableOffset(uint32_t executable_offset);

  const void* GetInterpreterToInterpreterBridge() const;
  uint32_t GetInterpreterToInterpreterBridgeOffset() const;
  void SetInterpreterToInterpreterBridgeOffset(uint32_t offset);
  const void* GetInterpreterToCompiledCodeBridge() const;
  uint32_t GetInterpreterToCompiledCodeBridgeOffset() const;
  void SetInterpreterToCompiledCodeBridgeOffset(uint32_t offset);

  const void* GetJniDlsymLookup() const;
  uint32_t GetJniDlsymLookupOffset() const;
  void SetJniDlsymLookupOffset(uint32_t offset);

  const void* GetPortableResolutionTrampoline() const;
  uint32_t GetPortableResolutionTrampolineOffset() const;
  void SetPortableResolutionTrampolineOffset(uint32_t offset);
  const void* GetPortableImtConflictTrampoline() const;
  uint32_t GetPortableImtConflictTrampolineOffset() const;
  void SetPortableImtConflictTrampolineOffset(uint32_t offset);
  const void* GetPortableToInterpreterBridge() const;
  uint32_t GetPortableToInterpreterBridgeOffset() const;
  void SetPortableToInterpreterBridgeOffset(uint32_t offset);

  const void* GetQuickGenericJniTrampoline() const;
  uint32_t GetQuickGenericJniTrampolineOffset() const;
  void SetQuickGenericJniTrampolineOffset(uint32_t offset);
  const void* GetQuickResolutionTrampoline() const;
  uint32_t GetQuickResolutionTrampolineOffset() const;
  void SetQuickResolutionTrampolineOffset(uint32_t offset);
  const void* GetQuickImtConflictTrampoline() const;
  uint32_t GetQuickImtConflictTrampolineOffset() const;
  void SetQuickImtConflictTrampolineOffset(uint32_t offset);
  const void* GetQuickToInterpreterBridge() const;
  uint32_t GetQuickToInterpreterBridgeOffset() const;
  void SetQuickToInterpreterBridgeOffset(uint32_t offset);

  InstructionSet GetInstructionSet() const;
  const InstructionSetFeatures& GetInstructionSetFeatures() const;
  uint32_t GetImageFileLocationOatChecksum() const;
  uint32_t GetImageFileLocationOatDataBegin() const;
  uint32_t GetImageFileLocationSize() const;
  const uint8_t* GetImageFileLocationData() const;
  std::string GetImageFileLocation() const;

 private:
  uint8_t magic_[4];
  uint8_t version_[4];
  uint32_t adler32_checksum_;

  InstructionSet instruction_set_;
  InstructionSetFeatures instruction_set_features_;
  uint32_t dex_file_count_;
  uint32_t executable_offset_;
  uint32_t interpreter_to_interpreter_bridge_offset_;
  uint32_t interpreter_to_compiled_code_bridge_offset_;
  uint32_t jni_dlsym_lookup_offset_;
  uint32_t portable_imt_conflict_trampoline_offset_;
  uint32_t portable_resolution_trampoline_offset_;
  uint32_t portable_to_interpreter_bridge_offset_;
  uint32_t quick_generic_jni_trampoline_offset_;
  uint32_t quick_imt_conflict_trampoline_offset_;
  uint32_t quick_resolution_trampoline_offset_;
  uint32_t quick_to_interpreter_bridge_offset_;

  uint32_t image_file_location_oat_checksum_;
  uint32_t image_file_location_oat_data_begin_;
  uint32_t image_file_location_size_;
  uint8_t image_file_location_data_[0];  // note variable width data at end

  DISALLOW_COPY_AND_ASSIGN(OatHeader);
};

// OatMethodOffsets are currently 5x32-bits=160-bits long, so if we can
// save even one OatMethodOffsets struct, the more complicated encoding
// using a bitmap pays for itself since few classes will have 160
// methods.
enum OatClassType {
  kOatClassAllCompiled = 0,   // OatClass is followed by an OatMethodOffsets for each method.
  kOatClassSomeCompiled = 1,  // A bitmap of which OatMethodOffsets are present follows the OatClass.
  kOatClassNoneCompiled = 2,  // All methods are interpretted so no OatMethodOffsets are necessary.
  kOatClassMax = 3,
};

std::ostream& operator<<(std::ostream& os, const OatClassType& rhs);

class PACKED(4) OatMethodOffsets {
 public:
  OatMethodOffsets();

  OatMethodOffsets(uint32_t code_offset,
                   uint32_t gc_map_offset);

  ~OatMethodOffsets();

  uint32_t code_offset_;
  uint32_t gc_map_offset_;
};

// OatQuickMethodHeader precedes the raw code chunk generated by the Quick compiler.
class PACKED(4) OatQuickMethodHeader {
 public:
  OatQuickMethodHeader();

  explicit OatQuickMethodHeader(uint32_t mapping_table_offset, uint32_t vmap_table_offset,
                                uint32_t frame_size_in_bytes, uint32_t core_spill_mask,
                                uint32_t fp_spill_mask, uint32_t code_size);

  ~OatQuickMethodHeader();

  // The offset in bytes from the start of the mapping table to the end of the header.
  uint32_t mapping_table_offset_;
  // The offset in bytes from the start of the vmap table to the end of the header.
  uint32_t vmap_table_offset_;
  // The stack frame information.
  QuickMethodFrameInfo frame_info_;
  // The code size in bytes.
  uint32_t code_size_;
};

}  // namespace art

#endif  // ART_RUNTIME_OAT_H_
