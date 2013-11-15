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

#include "driver/compiler_driver.h"
#include "mem_map.h"
#include "oat.h"
#include "mirror/class.h"
#include "safe_map.h"
#include "UniquePtr.h"

namespace art {

class BitVector;
class OutputStream;

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
// padding           if necessary so that the following code will be page aligned
//
// CompiledMethod    one variable sized blob with the contents of each CompiledMethod
// CompiledMethod
// CompiledMethod
// CompiledMethod
// CompiledMethod
// CompiledMethod
// ...
// CompiledMethod
//
class OatWriter {
 public:
  OatWriter(const std::vector<const DexFile*>& dex_files,
            uint32_t image_file_location_oat_checksum,
            uint32_t image_file_location_oat_begin,
            const std::string& image_file_location,
            const CompilerDriver* compiler,
            TimingLogger* timings);

  const OatHeader& GetOatHeader() const {
    return *oat_header_;
  }

  size_t GetSize() const {
    return size_;
  }

  bool Write(OutputStream& out);

  ~OatWriter();

 private:
  size_t InitOatHeader();
  size_t InitOatDexFiles(size_t offset);
  size_t InitDexFiles(size_t offset);
  size_t InitOatClasses(size_t offset);
  size_t InitOatCode(size_t offset)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  size_t InitOatCodeDexFiles(size_t offset)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  size_t InitOatCodeDexFile(size_t offset,
                            size_t* oat_class_index,
                            const DexFile& dex_file)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  size_t InitOatCodeClassDef(size_t offset,
                             size_t oat_class_index, size_t class_def_index,
                             const DexFile& dex_file,
                             const DexFile::ClassDef& class_def)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  size_t InitOatCodeMethod(size_t offset, size_t oat_class_index, size_t class_def_index,
                           size_t class_def_method_index, size_t* method_offsets_index,
                           bool is_native, InvokeType type, uint32_t method_idx, const DexFile&)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  bool WriteTables(OutputStream& out, const size_t file_offset);
  size_t WriteCode(OutputStream& out, const size_t file_offset);
  size_t WriteCodeDexFiles(OutputStream& out, const size_t file_offset, size_t relative_offset);
  size_t WriteCodeDexFile(OutputStream& out, const size_t file_offset, size_t relative_offset,
                          size_t* oat_class_index, const DexFile& dex_file);
  size_t WriteCodeClassDef(OutputStream& out, const size_t file_offset, size_t relative_offset,
                           size_t oat_class_index, const DexFile& dex_file,
                           const DexFile::ClassDef& class_def);
  size_t WriteCodeMethod(OutputStream& out, const size_t file_offset, size_t relative_offset,
                         size_t oat_class_index, size_t class_def_method_index,
                         size_t* method_offsets_index, bool is_static, uint32_t method_idx,
                         const DexFile& dex_file);

  void ReportWriteFailure(const char* what, uint32_t method_idx, const DexFile& dex_file,
                          OutputStream& out) const;

  class OatDexFile {
   public:
    explicit OatDexFile(size_t offset, const DexFile& dex_file);
    size_t SizeOf() const;
    void UpdateChecksum(OatHeader& oat_header) const;
    bool Write(OatWriter* oat_writer, OutputStream& out, const size_t file_offset) const;

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
                      std::vector<CompiledMethod*>* compiled_methods,
                      uint32_t num_non_null_compiled_methods,
                      mirror::Class::Status status);
    ~OatClass();
#if defined(ART_USE_PORTABLE_COMPILER)
    size_t GetOatMethodOffsetsOffsetFromOatHeader(size_t class_def_method_index_) const;
    size_t GetOatMethodOffsetsOffsetFromOatClass(size_t class_def_method_index_) const;
#endif
    size_t SizeOf() const;
    void UpdateChecksum(OatHeader& oat_header) const;
    bool Write(OatWriter* oat_writer, OutputStream& out, const size_t file_offset) const;

    CompiledMethod* GetCompiledMethod(size_t class_def_method_index) const {
      DCHECK(compiled_methods_ != NULL);
      return (*compiled_methods_)[class_def_method_index];
    }

    // Offset of start of OatClass from beginning of OatHeader. It is
    // used to validate file position when writing. For Portable, it
    // is also used to calculate the position of the OatMethodOffsets
    // so that code pointers within the OatMethodOffsets can be
    // patched to point to code in the Portable .o ELF objects.
    size_t offset_;

    // CompiledMethods for each class_def_method_index, or NULL if no method is available.
    std::vector<CompiledMethod*>* compiled_methods_;

    // Offset from OatClass::offset_ to the OatMethodOffsets for the
    // class_def_method_index. If 0, it means the corresponding
    // CompiledMethod entry in OatClass::compiled_methods_ should be
    // NULL and that the OatClass::type_ should be kOatClassBitmap.
    std::vector<uint32_t> oat_method_offsets_offsets_from_oat_class_;

    // data to write

    COMPILE_ASSERT(mirror::Class::Status::kStatusMax < (2 ^ 16), class_status_wont_fit_in_16bits);
    int16_t status_;

    COMPILE_ASSERT(OatClassType::kOatClassMax < (2 ^ 16), oat_class_type_wont_fit_in_16bits);
    uint16_t type_;

    uint32_t method_bitmap_size_;

    // bit vector indexed by ClassDef method index. When
    // OatClassType::type_ is kOatClassBitmap, a set bit indicates the
    // method has an OatMethodOffsets in methods_offsets_, otherwise
    // the entry was ommited to save space. If OatClassType::type_ is
    // not is kOatClassBitmap, the bitmap will be NULL.
    BitVector* method_bitmap_;

    // OatMethodOffsets for each CompiledMethod present in the
    // OatClass. Note that some may be missing if
    // OatClass::compiled_methods_ contains NULL values (and
    // oat_method_offsets_offsets_from_oat_class_ should contain 0
    // values in this case).
    std::vector<OatMethodOffsets> method_offsets_;

   private:
    DISALLOW_COPY_AND_ASSIGN(OatClass);
  };

  const CompilerDriver* const compiler_driver_;

  // note OatFile does not take ownership of the DexFiles
  const std::vector<const DexFile*>* dex_files_;

  // Size required for Oat data structures.
  size_t size_;

  // dependencies on the image.
  uint32_t image_file_location_oat_checksum_;
  uint32_t image_file_location_oat_begin_;
  std::string image_file_location_;

  // data to write
  OatHeader* oat_header_;
  std::vector<OatDexFile*> oat_dex_files_;
  std::vector<OatClass*> oat_classes_;
  UniquePtr<const std::vector<uint8_t> > interpreter_to_interpreter_bridge_;
  UniquePtr<const std::vector<uint8_t> > interpreter_to_compiled_code_bridge_;
  UniquePtr<const std::vector<uint8_t> > jni_dlsym_lookup_;
  UniquePtr<const std::vector<uint8_t> > portable_imt_conflict_trampoline_;
  UniquePtr<const std::vector<uint8_t> > portable_resolution_trampoline_;
  UniquePtr<const std::vector<uint8_t> > portable_to_interpreter_bridge_;
  UniquePtr<const std::vector<uint8_t> > quick_imt_conflict_trampoline_;
  UniquePtr<const std::vector<uint8_t> > quick_resolution_trampoline_;
  UniquePtr<const std::vector<uint8_t> > quick_to_interpreter_bridge_;

  // output stats
  uint32_t size_dex_file_alignment_;
  uint32_t size_executable_offset_alignment_;
  uint32_t size_oat_header_;
  uint32_t size_oat_header_image_file_location_;
  uint32_t size_dex_file_;
  uint32_t size_interpreter_to_interpreter_bridge_;
  uint32_t size_interpreter_to_compiled_code_bridge_;
  uint32_t size_jni_dlsym_lookup_;
  uint32_t size_portable_imt_conflict_trampoline_;
  uint32_t size_portable_resolution_trampoline_;
  uint32_t size_portable_to_interpreter_bridge_;
  uint32_t size_quick_imt_conflict_trampoline_;
  uint32_t size_quick_resolution_trampoline_;
  uint32_t size_quick_to_interpreter_bridge_;
  uint32_t size_trampoline_alignment_;
  uint32_t size_code_size_;
  uint32_t size_code_;
  uint32_t size_code_alignment_;
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

  // Code mappings for deduplication. Deduplication is already done on a pointer basis by the
  // compiler driver, so we can simply compare the pointers to find out if things are duplicated.
  SafeMap<const std::vector<uint8_t>*, uint32_t> code_offsets_;
  SafeMap<const std::vector<uint8_t>*, uint32_t> vmap_table_offsets_;
  SafeMap<const std::vector<uint8_t>*, uint32_t> mapping_table_offsets_;
  SafeMap<const std::vector<uint8_t>*, uint32_t> gc_map_offsets_;

  DISALLOW_COPY_AND_ASSIGN(OatWriter);
};

}  // namespace art

#endif  // ART_COMPILER_OAT_WRITER_H_
