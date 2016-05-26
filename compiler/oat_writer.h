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

#include "base/dchecked_vector.h"
#include "linker/relative_patcher.h"  // For linker::RelativePatcherTargetProvider.
#include "mem_map.h"
#include "method_reference.h"
#include "mirror/class.h"
#include "oat.h"
#include "os.h"
#include "safe_map.h"
#include "ScopedFd.h"
#include "utils/array_ref.h"

namespace art {

class BitVector;
class CompiledMethod;
class CompilerDriver;
class ImageWriter;
class OutputStream;
class TimingLogger;
class TypeLookupTable;
class ZipEntry;

namespace debug {
struct MethodDebugInfo;
}  // namespace debug

namespace linker {
class MultiOatRelativePatcher;
}  // namespace linker

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
// TypeLookupTable[0] one descriptor to class def index hash table for each OatDexFile.
// TypeLookupTable[1]
// ...
// TypeLookupTable[D]
//
// ClassOffsets[0]   one table of OatClass offsets for each class def for each OatDexFile.
// ClassOffsets[1]
// ...
// ClassOffsets[D]
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
  enum class CreateTypeLookupTable {
    kCreate,
    kDontCreate,
    kDefault = kCreate
  };

  OatWriter(bool compiling_boot_image, TimingLogger* timings);

  // To produce a valid oat file, the user must first add sources with any combination of
  //   - AddDexFileSource(),
  //   - AddZippedDexFilesSource(),
  //   - AddRawDexFileSource().
  // Then the user must call in order
  //   - WriteAndOpenDexFiles()
  //   - PrepareLayout(),
  //   - WriteRodata(),
  //   - WriteCode(),
  //   - WriteHeader().

  // Add dex file source(s) from a file, either a plain dex file or
  // a zip file with one or more dex files.
  bool AddDexFileSource(
      const char* filename,
      const char* location,
      CreateTypeLookupTable create_type_lookup_table = CreateTypeLookupTable::kDefault);
  // Add dex file source(s) from a zip file specified by a file handle.
  bool AddZippedDexFilesSource(
      ScopedFd&& zip_fd,
      const char* location,
      CreateTypeLookupTable create_type_lookup_table = CreateTypeLookupTable::kDefault);
  // Add dex file source from raw memory.
  bool AddRawDexFileSource(
      const ArrayRef<const uint8_t>& data,
      const char* location,
      uint32_t location_checksum,
      CreateTypeLookupTable create_type_lookup_table = CreateTypeLookupTable::kDefault);
  dchecked_vector<const char*> GetSourceLocations() const;

  // Write raw dex files to the .rodata section and open them from the oat file. The verify
  // setting dictates whether the dex file verifier should check the dex files. This is generally
  // the case, and should only be false for tests.
  bool WriteAndOpenDexFiles(OutputStream* rodata,
                            File* file,
                            InstructionSet instruction_set,
                            const InstructionSetFeatures* instruction_set_features,
                            SafeMap<std::string, std::string>* key_value_store,
                            bool verify,
                            /*out*/ std::unique_ptr<MemMap>* opened_dex_files_map,
                            /*out*/ std::vector<std::unique_ptr<const DexFile>>* opened_dex_files);
  // Prepare layout of remaining data.
  void PrepareLayout(const CompilerDriver* compiler,
                     ImageWriter* image_writer,
                     const std::vector<const DexFile*>& dex_files,
                     linker::MultiOatRelativePatcher* relative_patcher);
  // Write the rest of .rodata section (ClassOffsets[], OatClass[], maps).
  bool WriteRodata(OutputStream* out);
  // Write the code to the .text section.
  bool WriteCode(OutputStream* out);
  // Write the oat header. This finalizes the oat file.
  bool WriteHeader(OutputStream* out,
                   uint32_t image_file_location_oat_checksum,
                   uintptr_t image_file_location_oat_begin,
                   int32_t image_patch_delta);

  // Returns whether the oat file has an associated image.
  bool HasImage() const {
    // Since the image is being created at the same time as the oat file,
    // check if there's an image writer.
    return image_writer_ != nullptr;
  }

  bool HasBootImage() const {
    return compiling_boot_image_;
  }

  const OatHeader& GetOatHeader() const {
    return *oat_header_;
  }

  size_t GetSize() const {
    return size_;
  }

  size_t GetBssSize() const {
    return bss_size_;
  }

  size_t GetOatDataOffset() const {
    return oat_data_offset_;
  }

  ArrayRef<const uintptr_t> GetAbsolutePatchLocations() const {
    return ArrayRef<const uintptr_t>(absolute_patch_locations_);
  }

  ~OatWriter();

  void AddMethodDebugInfos(const std::vector<debug::MethodDebugInfo>& infos) {
    method_info_.insert(method_info_.end(), infos.begin(), infos.end());
  }

  ArrayRef<const debug::MethodDebugInfo> GetMethodDebugInfo() const {
    return ArrayRef<const debug::MethodDebugInfo>(method_info_);
  }

  const CompilerDriver* GetCompilerDriver() {
    return compiler_driver_;
  }

 private:
  class DexFileSource;
  class OatClass;
  class OatDexFile;

  // The function VisitDexMethods() below iterates through all the methods in all
  // the compiled dex files in order of their definitions. The method visitor
  // classes provide individual bits of processing for each of the passes we need to
  // first collect the data we want to write to the oat file and then, in later passes,
  // to actually write it.
  class DexMethodVisitor;
  class OatDexMethodVisitor;
  class InitOatClassesMethodVisitor;
  class InitCodeMethodVisitor;
  class InitMapMethodVisitor;
  class InitImageMethodVisitor;
  class WriteCodeMethodVisitor;
  class WriteMapMethodVisitor;

  // Visit all the methods in all the compiled dex files in their definition order
  // with a given DexMethodVisitor.
  bool VisitDexMethods(DexMethodVisitor* visitor);

  size_t InitOatHeader(InstructionSet instruction_set,
                       const InstructionSetFeatures* instruction_set_features,
                       uint32_t num_dex_files,
                       SafeMap<std::string, std::string>* key_value_store);
  size_t InitOatDexFiles(size_t offset);
  size_t InitOatClasses(size_t offset);
  size_t InitOatMaps(size_t offset);
  size_t InitOatCode(size_t offset);
  size_t InitOatCodeDexFiles(size_t offset);

  bool WriteClassOffsets(OutputStream* out);
  bool WriteClasses(OutputStream* out);
  size_t WriteMaps(OutputStream* out, const size_t file_offset, size_t relative_offset);
  size_t WriteCode(OutputStream* out, const size_t file_offset, size_t relative_offset);
  size_t WriteCodeDexFiles(OutputStream* out, const size_t file_offset, size_t relative_offset);

  bool RecordOatDataOffset(OutputStream* out);
  bool ReadDexFileHeader(File* file, OatDexFile* oat_dex_file);
  bool ValidateDexFileHeader(const uint8_t* raw_header, const char* location);
  bool WriteDexFiles(OutputStream* rodata, File* file);
  bool WriteDexFile(OutputStream* rodata, File* file, OatDexFile* oat_dex_file);
  bool SeekToDexFile(OutputStream* rodata, File* file, OatDexFile* oat_dex_file);
  bool WriteDexFile(OutputStream* rodata, File* file, OatDexFile* oat_dex_file, ZipEntry* dex_file);
  bool WriteDexFile(OutputStream* rodata, File* file, OatDexFile* oat_dex_file, File* dex_file);
  bool WriteDexFile(OutputStream* rodata, OatDexFile* oat_dex_file, const uint8_t* dex_file);
  bool WriteOatDexFiles(OutputStream* rodata);
  bool ExtendForTypeLookupTables(OutputStream* rodata, File* file, size_t offset);
  bool OpenDexFiles(File* file,
                    bool verify,
                    /*out*/ std::unique_ptr<MemMap>* opened_dex_files_map,
                    /*out*/ std::vector<std::unique_ptr<const DexFile>>* opened_dex_files);
  bool WriteTypeLookupTables(MemMap* opened_dex_files_map,
                             const std::vector<std::unique_ptr<const DexFile>>& opened_dex_files);
  bool WriteCodeAlignment(OutputStream* out, uint32_t aligned_code_delta);
  void SetMultiOatRelativePatcherAdjustment();

  enum class WriteState {
    kAddingDexFileSources,
    kPrepareLayout,
    kWriteRoData,
    kWriteText,
    kWriteHeader,
    kDone
  };

  WriteState write_state_;
  TimingLogger* timings_;

  std::vector<std::unique_ptr<File>> raw_dex_files_;
  std::vector<std::unique_ptr<ZipArchive>> zip_archives_;
  std::vector<std::unique_ptr<ZipEntry>> zipped_dex_files_;

  // Using std::list<> which doesn't move elements around on push/emplace_back().
  // We need this because we keep plain pointers to the strings' c_str().
  std::list<std::string> zipped_dex_file_locations_;

  dchecked_vector<debug::MethodDebugInfo> method_info_;

  const CompilerDriver* compiler_driver_;
  ImageWriter* image_writer_;
  const bool compiling_boot_image_;

  // note OatFile does not take ownership of the DexFiles
  const std::vector<const DexFile*>* dex_files_;

  // Size required for Oat data structures.
  size_t size_;

  // The size of the required .bss section holding the DexCache data.
  size_t bss_size_;

  // Offsets of the dex cache arrays for each app dex file. For the
  // boot image, this information is provided by the ImageWriter.
  SafeMap<const DexFile*, size_t> dex_cache_arrays_offsets_;  // DexFiles not owned.

  // Offset of the oat data from the start of the mmapped region of the elf file.
  size_t oat_data_offset_;

  // data to write
  std::unique_ptr<OatHeader> oat_header_;
  dchecked_vector<OatDexFile> oat_dex_files_;
  dchecked_vector<OatClass> oat_classes_;
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
  uint32_t size_vmap_table_;
  uint32_t size_oat_dex_file_location_size_;
  uint32_t size_oat_dex_file_location_data_;
  uint32_t size_oat_dex_file_location_checksum_;
  uint32_t size_oat_dex_file_offset_;
  uint32_t size_oat_dex_file_class_offsets_offset_;
  uint32_t size_oat_dex_file_lookup_table_offset_;
  uint32_t size_oat_lookup_table_alignment_;
  uint32_t size_oat_lookup_table_;
  uint32_t size_oat_class_offsets_alignment_;
  uint32_t size_oat_class_offsets_;
  uint32_t size_oat_class_type_;
  uint32_t size_oat_class_status_;
  uint32_t size_oat_class_method_bitmaps_;
  uint32_t size_oat_class_method_offsets_;

  // The helper for processing relative patches is external so that we can patch across oat files.
  linker::MultiOatRelativePatcher* relative_patcher_;

  // The locations of absolute patches relative to the start of the executable section.
  dchecked_vector<uintptr_t> absolute_patch_locations_;

  DISALLOW_COPY_AND_ASSIGN(OatWriter);
};

}  // namespace art

#endif  // ART_COMPILER_OAT_WRITER_H_
