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

#ifndef ART_COMPILER_OAT_WRITER_H_
#define ART_COMPILER_OAT_WRITER_H_

#include <stdint.h>
#include <cstddef>
#include <memory>

#include "linker/relative_patcher.h"  // For linker::RelativePatcherTargetProvider.
#include "mem_map.h"
#include "method_reference.h"
#include "oat.h"
#include "mirror/class.h"
#include "safe_map.h"

namespace art {

class BitVector;
class CompiledMethod;
class CompilerDriver;
class ImageWriter;
class OutputStream;
class TimingLogger;

// OatHeader         variable length with count of D OatDexFiles
//
// OatDexFile[0]     one variable sized OatDexFile with offsets to Dex and OatClasses
// OatDexFile[1]
// ...
// OatDexFile[D]
//
// Dex[0]            one variable sized DexFile for each OatDexFile.
// Dex[1]            these are literal copies of the input .dex files.
// ...
// Dex[D]
//
// OatClass[0]       one variable sized OatClass for each of C DexFile::ClassDefs
// OatClass[1]       contains OatClass entries with class status, offsets to code, etc.
// ...
// OatClass[C]
//
// GcMap             one variable sized blob with GC map.
// GcMap             GC maps are deduplicated.
// ...
// GcMap
//
// VmapTable         one variable sized VmapTable blob (quick compiler only).
// VmapTable         VmapTables are deduplicated.
// ...
// VmapTable
//
// MappingTable      one variable sized blob with MappingTable (quick compiler only).
// MappingTable      MappingTables are deduplicated.
// ...
// MappingTable
//
// padding           if necessary so that the following code will be page aligned
//
// OatMethodHeader   fixed size header for a CompiledMethod including the size of the MethodCode.
// MethodCode        one variable sized blob with the code of a CompiledMethod.
// OatMethodHeader   (OatMethodHeader, MethodCode) pairs are deduplicated.
// MethodCode
// ...
// OatMethodHeader
// MethodCode
//
class OatWriter {
 public:
  OatWriter(const std::vector<const DexFile*>& dex_files,
            uint32_t image_file_location_oat_checksum,
            uintptr_t image_file_location_oat_begin,
            int32_t image_patch_delta,
            const CompilerDriver* compiler,
            ImageWriter* image_writer,
            TimingLogger* timings,
            SafeMap<std::string, std::string>* key_value_store);

  const OatHeader& GetOatHeader() const {
    return *oat_header_;
  }

  size_t GetSize() const {
    return size_;
  }

  size_t GetBssSize() const {
    return bss_size_;
  }

  const std::vector<uintptr_t>& GetAbsolutePatchLocations() const {
    return absolute_patch_locations_;
  }

  bool WriteRodata(OutputStream* out);
  bool WriteCode(OutputStream* out);

  ~OatWriter();

  struct DebugInfo {
    const DexFile* dex_file_;
    size_t class_def_index_;
    uint32_t dex_method_index_;
    uint32_t access_flags_;
    const DexFile::CodeItem *code_item_;
    bool deduped_;
    uint32_t low_pc_;
    uint32_t high_pc_;
    CompiledMethod* compiled_method_;
  };

  const std::vector<DebugInfo>& GetMethodDebugInfo() const {
    return method_info_;
  }

  const CompilerDriver* GetCompilerDriver() {
    return compiler_driver_;
  }

 private:
  // The DataAccess classes are helper classes that provide access to members related to
  // a given map, i.e. GC map, mapping table or vmap table. By abstracting these away
  // we can share a lot of code for processing the maps with template classes below.
  struct GcMapDataAccess;
  struct MappingTableDataAccess;
  struct VmapTableDataAccess;

  // The function VisitDexMethods() below iterates through all the methods in all
  // the compiled dex files in order of their definitions. The method visitor
  // classes provide individual bits of processing for each of the passes we need to
  // first collect the data we want to write to the oat file and then, in later passes,
  // to actually write it.
  class DexMethodVisitor;
  class OatDexMethodVisitor;
  class InitOatClassesMethodVisitor;
  class InitCodeMethodVisitor;
  template <typename DataAccess>
  class InitMapMethodVisitor;
  class InitImageMethodVisitor;
  class WriteCodeMethodVisitor;
  template <typename DataAccess>
  class WriteMapMethodVisitor;

  // Visit all the methods in all the compiled dex files in their definition order
  // with a given DexMethodVisitor.
  bool VisitDexMethods(DexMethodVisitor* visitor);

  size_t InitOatHeader();
  size_t InitOatDexFiles(size_t offset);
  size_t InitDexFiles(size_t offset);
  size_t InitOatClasses(size_t offset);
  size_t InitOatMaps(size_t offset);
  size_t InitOatCode(size_t offset)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  size_t InitOatCodeDexFiles(size_t offset)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  bool WriteTables(OutputStream* out, const size_t file_offset);
  size_t WriteMaps(OutputStream* out, const size_t file_offset, size_t relative_offset);
  size_t WriteCode(OutputStream* out, const size_t file_offset, size_t relative_offset);
  size_t WriteCodeDexFiles(OutputStream* out, const size_t file_offset, size_t relative_offset);

  bool WriteCodeAlignment(OutputStream* out, uint32_t aligned_code_delta);

  class OatDexFile {
   public:
    explicit OatDexFile(size_t offset, const DexFile& dex_file);
    size_t SizeOf() const;
    void UpdateChecksum(OatHeader* oat_header) const;
    bool Write(OatWriter* oat_writer, OutputStream* out, const size_t file_offset) const;

    // Offset of start of OatDexFile from beginning of OatHeader. It is
    // used to validate file position when writing.
    size_t offset_;

    // data to write
    uint32_t dex_file_location_size_;
    const uint8_t* dex_file_location_data_;
    uint32_t dex_file_location_checksum_;
    uint32_t dex_file_offset_;
    std::vector<uint32_t> methods_offsets_;

   private:
    DISALLOW_COPY_AND_ASSIGN(OatDexFile);
  };

  class OatClass {
   public:
    explicit OatClass(size_t offset,
                      const std::vector<CompiledMethod*>& compiled_methods,
                      uint32_t num_non_null_compiled_methods,
                      mirror::Class::Status status);
    ~OatClass();
    size_t GetOatMethodOffsetsOffsetFromOatHeader(size_t class_def_method_index_) const;
    size_t GetOatMethodOffsetsOffsetFromOatClass(size_t class_def_method_index_) const;
    size_t SizeOf() const;
    void UpdateChecksum(OatHeader* oat_header) const;
    bool Write(OatWriter* oat_writer, OutputStream* out, const size_t file_offset) const;

    CompiledMethod* GetCompiledMethod(size_t class_def_method_index) const {
      DCHECK_LT(class_def_method_index, compiled_methods_.size());
      return compiled_methods_[class_def_method_index];
    }

    // Offset of start of OatClass from beginning of OatHeader. It is
    // used to validate file position when writing.
    size_t offset_;

    // CompiledMethods for each class_def_method_index, or null if no method is available.
    std::vector<CompiledMethod*> compiled_methods_;

    // Offset from OatClass::offset_ to the OatMethodOffsets for the
    // class_def_method_index. If 0, it means the corresponding
    // CompiledMethod entry in OatClass::compiled_methods_ should be
    // null and that the OatClass::type_ should be kOatClassBitmap.
    std::vector<uint32_t> oat_method_offsets_offsets_from_oat_class_;

    // data to write

    static_assert(mirror::Class::Status::kStatusMax < (2 ^ 16), "class status won't fit in 16bits");
    int16_t status_;

    static_assert(OatClassType::kOatClassMax < (2 ^ 16), "oat_class type won't fit in 16bits");
    uint16_t type_;

    uint32_t method_bitmap_size_;

    // bit vector indexed by ClassDef method index. When
    // OatClassType::type_ is kOatClassBitmap, a set bit indicates the
    // method has an OatMethodOffsets in methods_offsets_, otherwise
    // the entry was ommited to save space. If OatClassType::type_ is
    // not is kOatClassBitmap, the bitmap will be null.
    BitVector* method_bitmap_;

    // OatMethodOffsets and OatMethodHeaders for each CompiledMethod
    // present in the OatClass. Note that some may be missing if
    // OatClass::compiled_methods_ contains null values (and
    // oat_method_offsets_offsets_from_oat_class_ should contain 0
    // values in this case).
    std::vector<OatMethodOffsets> method_offsets_;
    std::vector<OatQuickMethodHeader> method_headers_;

   private:
    DISALLOW_COPY_AND_ASSIGN(OatClass);
  };

  std::vector<DebugInfo> method_info_;

  const CompilerDriver* const compiler_driver_;
  ImageWriter* const image_writer_;

  // note OatFile does not take ownership of the DexFiles
  const std::vector<const DexFile*>* dex_files_;

  // Size required for Oat data structures.
  size_t size_;

  // The size of the required .bss section holding the DexCache data.
  size_t bss_size_;

  // Offset of the oat data from the start of the mmapped region of the elf file.
  size_t oat_data_offset_;

  // dependencies on the image.
  uint32_t image_file_location_oat_checksum_;
  uintptr_t image_file_location_oat_begin_;
  int32_t image_patch_delta_;

  // data to write
  SafeMap<std::string, std::string>* key_value_store_;
  OatHeader* oat_header_;
  std::vector<OatDexFile*> oat_dex_files_;
  std::vector<OatClass*> oat_classes_;
  std::unique_ptr<const std::vector<uint8_t>> interpreter_to_interpreter_bridge_;
  std::unique_ptr<const std::vector<uint8_t>> interpreter_to_compiled_code_bridge_;
  std::unique_ptr<const std::vector<uint8_t>> jni_dlsym_lookup_;
  std::unique_ptr<const std::vector<uint8_t>> quick_generic_jni_trampoline_;
  std::unique_ptr<const std::vector<uint8_t>> quick_imt_conflict_trampoline_;
  std::unique_ptr<const std::vector<uint8_t>> quick_resolution_trampoline_;
  std::unique_ptr<const std::vector<uint8_t>> quick_to_interpreter_bridge_;

  // output stats
  uint32_t size_dex_file_alignment_;
  uint32_t size_executable_offset_alignment_;
  uint32_t size_oat_header_;
  uint32_t size_oat_header_key_value_store_;
  uint32_t size_dex_file_;
  uint32_t size_interpreter_to_interpreter_bridge_;
  uint32_t size_interpreter_to_compiled_code_bridge_;
  uint32_t size_jni_dlsym_lookup_;
  uint32_t size_quick_generic_jni_trampoline_;
  uint32_t size_quick_imt_conflict_trampoline_;
  uint32_t size_quick_resolution_trampoline_;
  uint32_t size_quick_to_interpreter_bridge_;
  uint32_t size_trampoline_alignment_;
  uint32_t size_method_header_;
  uint32_t size_code_;
  uint32_t size_code_alignment_;
  uint32_t size_relative_call_thunks_;
  uint32_t size_misc_thunks_;
  uint32_t size_mapping_table_;
  uint32_t size_vmap_table_;
  uint32_t size_gc_map_;
  uint32_t size_oat_dex_file_location_size_;
  uint32_t size_oat_dex_file_location_data_;
  uint32_t size_oat_dex_file_location_checksum_;
  uint32_t size_oat_dex_file_offset_;
  uint32_t size_oat_dex_file_methods_offsets_;
  uint32_t size_oat_class_type_;
  uint32_t size_oat_class_status_;
  uint32_t size_oat_class_method_bitmaps_;
  uint32_t size_oat_class_method_offsets_;

  std::unique_ptr<linker::RelativePatcher> relative_patcher_;

  // The locations of absolute patches relative to the start of the executable section.
  std::vector<uintptr_t> absolute_patch_locations_;

  // Map method reference to assigned offset.
  // Wrap the map in a class implementing linker::RelativePatcherTargetProvider.
  class MethodOffsetMap FINAL : public linker::RelativePatcherTargetProvider {
   public:
    std::pair<bool, uint32_t> FindMethodOffset(MethodReference ref) OVERRIDE;
    SafeMap<MethodReference, uint32_t, MethodReferenceComparator> map;
  };
  MethodOffsetMap method_offset_map_;

  DISALLOW_COPY_AND_ASSIGN(OatWriter);
};

}  // namespace art

#endif  // ART_COMPILER_OAT_WRITER_H_
