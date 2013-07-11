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

#include "oat_writer.h"

#include <zlib.h>

#include "base/stl_util.h"
#include "base/unix_file/fd_file.h"
#include "class_linker.h"
#include "dex_file-inl.h"
#include "gc/space/space.h"
#include "mirror/abstract_method-inl.h"
#include "mirror/array.h"
#include "mirror/class_loader.h"
#include "mirror/object-inl.h"
#include "os.h"
#include "output_stream.h"
#include "safe_map.h"
#include "scoped_thread_state_change.h"
#include "verifier/method_verifier.h"

namespace art {

bool OatWriter::Create(OutputStream& output_stream,
                       const std::vector<const DexFile*>& dex_files,
                       uint32_t image_file_location_oat_checksum,
                       uint32_t image_file_location_oat_begin,
                       const std::string& image_file_location,
                       const CompilerDriver& driver) {
  OatWriter oat_writer(dex_files,
                       image_file_location_oat_checksum,
                       image_file_location_oat_begin,
                       image_file_location,
                       &driver);
  return oat_writer.Write(output_stream);
}

OatWriter::OatWriter(const std::vector<const DexFile*>& dex_files,
                     uint32_t image_file_location_oat_checksum,
                     uint32_t image_file_location_oat_begin,
                     const std::string& image_file_location,
                     const CompilerDriver* compiler)
  : compiler_driver_(compiler),
    dex_files_(&dex_files),
    image_file_location_oat_checksum_(image_file_location_oat_checksum),
    image_file_location_oat_begin_(image_file_location_oat_begin),
    image_file_location_(image_file_location),
    oat_header_(NULL),
    size_dex_file_alignment_(0),
    size_executable_offset_alignment_(0),
    size_oat_header_(0),
    size_oat_header_image_file_location_(0),
    size_dex_file_(0),
    size_interpreter_to_interpreter_entry_(0),
    size_interpreter_to_quick_entry_(0),
    size_portable_resolution_trampoline_(0),
    size_quick_resolution_trampoline_(0),
    size_stubs_alignment_(0),
    size_code_size_(0),
    size_code_(0),
    size_code_alignment_(0),
    size_mapping_table_(0),
    size_vmap_table_(0),
    size_gc_map_(0),
    size_oat_dex_file_location_size_(0),
    size_oat_dex_file_location_data_(0),
    size_oat_dex_file_location_checksum_(0),
    size_oat_dex_file_offset_(0),
    size_oat_dex_file_methods_offsets_(0),
    size_oat_class_status_(0),
    size_oat_class_method_offsets_(0) {

  size_t offset = InitOatHeader();
  offset = InitOatDexFiles(offset);
  offset = InitDexFiles(offset);
  offset = InitOatClasses(offset);
  offset = InitOatCode(offset);
  offset = InitOatCodeDexFiles(offset);

  CHECK_EQ(dex_files_->size(), oat_dex_files_.size());
  CHECK(image_file_location.empty() == compiler->IsImage());
}

OatWriter::~OatWriter() {
  delete oat_header_;
  STLDeleteElements(&oat_dex_files_);
  STLDeleteElements(&oat_classes_);
}

size_t OatWriter::InitOatHeader() {
  // create the OatHeader
  oat_header_ = new OatHeader(compiler_driver_->GetInstructionSet(),
                              dex_files_,
                              image_file_location_oat_checksum_,
                              image_file_location_oat_begin_,
                              image_file_location_);
  size_t offset = sizeof(*oat_header_);
  offset += image_file_location_.size();
  return offset;
}

size_t OatWriter::InitOatDexFiles(size_t offset) {
  // create the OatDexFiles
  for (size_t i = 0; i != dex_files_->size(); ++i) {
    const DexFile* dex_file = (*dex_files_)[i];
    CHECK(dex_file != NULL);
    OatDexFile* oat_dex_file = new OatDexFile(offset, *dex_file);
    oat_dex_files_.push_back(oat_dex_file);
    offset += oat_dex_file->SizeOf();
  }
  return offset;
}

size_t OatWriter::InitDexFiles(size_t offset) {
  // calculate the offsets within OatDexFiles to the DexFiles
  for (size_t i = 0; i != dex_files_->size(); ++i) {
    // dex files are required to be 4 byte aligned
    size_t original_offset = offset;
    offset = RoundUp(offset, 4);
    size_dex_file_alignment_ += offset - original_offset;

    // set offset in OatDexFile to DexFile
    oat_dex_files_[i]->dex_file_offset_ = offset;

    const DexFile* dex_file = (*dex_files_)[i];
    offset += dex_file->GetHeader().file_size_;
  }
  return offset;
}

size_t OatWriter::InitOatClasses(size_t offset) {
  // create the OatClasses
  // calculate the offsets within OatDexFiles to OatClasses
  for (size_t i = 0; i != dex_files_->size(); ++i) {
    const DexFile* dex_file = (*dex_files_)[i];
    for (size_t class_def_index = 0;
         class_def_index < dex_file->NumClassDefs();
         class_def_index++) {
      oat_dex_files_[i]->methods_offsets_[class_def_index] = offset;
      const DexFile::ClassDef& class_def = dex_file->GetClassDef(class_def_index);
      const byte* class_data = dex_file->GetClassData(class_def);
      uint32_t num_methods = 0;
      if (class_data != NULL) {  // ie not an empty class, such as a marker interface
        ClassDataItemIterator it(*dex_file, class_data);
        size_t num_direct_methods = it.NumDirectMethods();
        size_t num_virtual_methods = it.NumVirtualMethods();
        num_methods = num_direct_methods + num_virtual_methods;
      }

      ClassReference class_ref(dex_file, class_def_index);
      CompiledClass* compiled_class = compiler_driver_->GetCompiledClass(class_ref);
      mirror::Class::Status status;
      if (compiled_class != NULL) {
        status = compiled_class->GetStatus();
      } else if (verifier::MethodVerifier::IsClassRejected(class_ref)) {
        status = mirror::Class::kStatusError;
      } else {
        status = mirror::Class::kStatusNotReady;
      }

      OatClass* oat_class = new OatClass(offset, status, num_methods);
      oat_classes_.push_back(oat_class);
      offset += oat_class->SizeOf();
    }
    oat_dex_files_[i]->UpdateChecksum(*oat_header_);
  }
  return offset;
}

size_t OatWriter::InitOatCode(size_t offset) {
  // calculate the offsets within OatHeader to executable code
  size_t old_offset = offset;
  // required to be on a new page boundary
  offset = RoundUp(offset, kPageSize);
  oat_header_->SetExecutableOffset(offset);
  size_executable_offset_alignment_ = offset - old_offset;
  if (compiler_driver_->IsImage()) {
    InstructionSet instruction_set = compiler_driver_->GetInstructionSet();
    oat_header_->SetInterpreterToInterpreterEntryOffset(offset);
    interpreter_to_interpreter_entry_.reset(compiler_driver_->CreateInterpreterToInterpreterEntry());
    offset += interpreter_to_interpreter_entry_->size();

    offset = CompiledCode::AlignCode(offset, instruction_set);
    oat_header_->SetInterpreterToQuickEntryOffset(offset);
    interpreter_to_quick_entry_.reset(compiler_driver_->CreateInterpreterToQuickEntry());
    offset += interpreter_to_quick_entry_->size();

    offset = CompiledCode::AlignCode(offset, instruction_set);
    oat_header_->SetPortableResolutionTrampolineOffset(offset);
    portable_resolution_trampoline_.reset(compiler_driver_->CreatePortableResolutionTrampoline());
    offset += portable_resolution_trampoline_->size();

    offset = CompiledCode::AlignCode(offset, instruction_set);
    oat_header_->SetQuickResolutionTrampolineOffset(offset);
    quick_resolution_trampoline_.reset(compiler_driver_->CreateQuickResolutionTrampoline());
    offset += quick_resolution_trampoline_->size();
  } else {
    oat_header_->SetInterpreterToInterpreterEntryOffset(0);
    oat_header_->SetInterpreterToQuickEntryOffset(0);
    oat_header_->SetPortableResolutionTrampolineOffset(0);
    oat_header_->SetQuickResolutionTrampolineOffset(0);
  }
  return offset;
}

size_t OatWriter::InitOatCodeDexFiles(size_t offset) {
  size_t oat_class_index = 0;
  for (size_t i = 0; i != dex_files_->size(); ++i) {
    const DexFile* dex_file = (*dex_files_)[i];
    CHECK(dex_file != NULL);
    offset = InitOatCodeDexFile(offset, oat_class_index, *dex_file);
  }
  return offset;
}

size_t OatWriter::InitOatCodeDexFile(size_t offset,
                                     size_t& oat_class_index,
                                     const DexFile& dex_file) {
  for (size_t class_def_index = 0;
       class_def_index < dex_file.NumClassDefs();
       class_def_index++, oat_class_index++) {
    const DexFile::ClassDef& class_def = dex_file.GetClassDef(class_def_index);
    offset = InitOatCodeClassDef(offset, oat_class_index, class_def_index, dex_file, class_def);
    oat_classes_[oat_class_index]->UpdateChecksum(*oat_header_);
  }
  return offset;
}

size_t OatWriter::InitOatCodeClassDef(size_t offset,
                                      size_t oat_class_index, size_t class_def_index,
                                      const DexFile& dex_file,
                                      const DexFile::ClassDef& class_def) {
  const byte* class_data = dex_file.GetClassData(class_def);
  if (class_data == NULL) {
    // empty class, such as a marker interface
    return offset;
  }
  ClassDataItemIterator it(dex_file, class_data);
  CHECK_EQ(oat_classes_[oat_class_index]->method_offsets_.size(),
           it.NumDirectMethods() + it.NumVirtualMethods());
  // Skip fields
  while (it.HasNextStaticField()) {
    it.Next();
  }
  while (it.HasNextInstanceField()) {
    it.Next();
  }
  // Process methods
  size_t class_def_method_index = 0;
  while (it.HasNextDirectMethod()) {
    bool is_native = (it.GetMemberAccessFlags() & kAccNative) != 0;
    offset = InitOatCodeMethod(offset, oat_class_index, class_def_index, class_def_method_index,
                               is_native, it.GetMethodInvokeType(class_def), it.GetMemberIndex(),
                               &dex_file);
    class_def_method_index++;
    it.Next();
  }
  while (it.HasNextVirtualMethod()) {
    bool is_native = (it.GetMemberAccessFlags() & kAccNative) != 0;
    offset = InitOatCodeMethod(offset, oat_class_index, class_def_index, class_def_method_index,
                               is_native, it.GetMethodInvokeType(class_def), it.GetMemberIndex(),
                               &dex_file);
    class_def_method_index++;
    it.Next();
  }
  DCHECK(!it.HasNext());
  return offset;
}

size_t OatWriter::InitOatCodeMethod(size_t offset, size_t oat_class_index,
                                    size_t __attribute__((unused)) class_def_index,
                                    size_t class_def_method_index,
                                    bool __attribute__((unused)) is_native,
                                    InvokeType invoke_type,
                                    uint32_t method_idx, const DexFile* dex_file) {
  // derived from CompiledMethod if available
  uint32_t code_offset = 0;
  uint32_t frame_size_in_bytes = kStackAlignment;
  uint32_t core_spill_mask = 0;
  uint32_t fp_spill_mask = 0;
  uint32_t mapping_table_offset = 0;
  uint32_t vmap_table_offset = 0;
  uint32_t gc_map_offset = 0;

  OatClass* oat_class = oat_classes_[oat_class_index];
#if defined(ART_USE_PORTABLE_COMPILER)
  size_t oat_method_offsets_offset =
      oat_class->GetOatMethodOffsetsOffsetFromOatHeader(class_def_method_index);
#endif

  CompiledMethod* compiled_method =
      compiler_driver_->GetCompiledMethod(MethodReference(dex_file, method_idx));
  if (compiled_method != NULL) {
#if defined(ART_USE_PORTABLE_COMPILER)
    compiled_method->AddOatdataOffsetToCompliledCodeOffset(
        oat_method_offsets_offset + OFFSETOF_MEMBER(OatMethodOffsets, code_offset_));
#else
    const std::vector<uint8_t>& code = compiled_method->GetCode();
    offset = compiled_method->AlignCode(offset);
    DCHECK_ALIGNED(offset, kArmAlignment);
    uint32_t code_size = code.size() * sizeof(code[0]);
    CHECK_NE(code_size, 0U);
    uint32_t thumb_offset = compiled_method->CodeDelta();
    code_offset = offset + sizeof(code_size) + thumb_offset;

    // Deduplicate code arrays
    SafeMap<const std::vector<uint8_t>*, uint32_t>::iterator code_iter = code_offsets_.find(&code);
    if (code_iter != code_offsets_.end()) {
      code_offset = code_iter->second;
    } else {
      code_offsets_.Put(&code, code_offset);
      offset += sizeof(code_size);  // code size is prepended before code
      offset += code_size;
      oat_header_->UpdateChecksum(&code[0], code_size);
    }
#endif
    frame_size_in_bytes = compiled_method->GetFrameSizeInBytes();
    core_spill_mask = compiled_method->GetCoreSpillMask();
    fp_spill_mask = compiled_method->GetFpSpillMask();

    const std::vector<uint32_t>& mapping_table = compiled_method->GetMappingTable();
    size_t mapping_table_size = mapping_table.size() * sizeof(mapping_table[0]);
    mapping_table_offset = (mapping_table_size == 0) ? 0 : offset;

    // Deduplicate mapping tables
    SafeMap<const std::vector<uint32_t>*, uint32_t>::iterator mapping_iter = mapping_table_offsets_.find(&mapping_table);
    if (mapping_iter != mapping_table_offsets_.end()) {
      mapping_table_offset = mapping_iter->second;
    } else {
      mapping_table_offsets_.Put(&mapping_table, mapping_table_offset);
      offset += mapping_table_size;
      oat_header_->UpdateChecksum(&mapping_table[0], mapping_table_size);
    }

    const std::vector<uint16_t>& vmap_table = compiled_method->GetVmapTable();
    size_t vmap_table_size = vmap_table.size() * sizeof(vmap_table[0]);
    vmap_table_offset = (vmap_table_size == 0) ? 0 : offset;

    // Deduplicate vmap tables
    SafeMap<const std::vector<uint16_t>*, uint32_t>::iterator vmap_iter = vmap_table_offsets_.find(&vmap_table);
    if (vmap_iter != vmap_table_offsets_.end()) {
      vmap_table_offset = vmap_iter->second;
    } else {
      vmap_table_offsets_.Put(&vmap_table, vmap_table_offset);
      offset += vmap_table_size;
      oat_header_->UpdateChecksum(&vmap_table[0], vmap_table_size);
    }

    const std::vector<uint8_t>& gc_map = compiled_method->GetGcMap();
    size_t gc_map_size = gc_map.size() * sizeof(gc_map[0]);
    gc_map_offset = (gc_map_size == 0) ? 0 : offset;

#if !defined(NDEBUG)
    // We expect GC maps except when the class hasn't been verified or the method is native
    ClassReference class_ref(dex_file, class_def_index);
    CompiledClass* compiled_class = compiler_driver_->GetCompiledClass(class_ref);
    mirror::Class::Status status;
    if (compiled_class != NULL) {
      status = compiled_class->GetStatus();
    } else if (verifier::MethodVerifier::IsClassRejected(class_ref)) {
      status = mirror::Class::kStatusError;
    } else {
      status = mirror::Class::kStatusNotReady;
    }
    CHECK(gc_map_size != 0 || is_native || status < mirror::Class::kStatusVerified)
        << &gc_map << " " << gc_map_size << " " << (is_native ? "true" : "false") << " "
        << (status < mirror::Class::kStatusVerified) << " " << status << " "
        << PrettyMethod(method_idx, *dex_file);
#endif

    // Deduplicate GC maps
    SafeMap<const std::vector<uint8_t>*, uint32_t>::iterator gc_map_iter = gc_map_offsets_.find(&gc_map);
    if (gc_map_iter != gc_map_offsets_.end()) {
      gc_map_offset = gc_map_iter->second;
    } else {
      gc_map_offsets_.Put(&gc_map, gc_map_offset);
      offset += gc_map_size;
      oat_header_->UpdateChecksum(&gc_map[0], gc_map_size);
    }
  }

  oat_class->method_offsets_[class_def_method_index]
      = OatMethodOffsets(code_offset,
                         frame_size_in_bytes,
                         core_spill_mask,
                         fp_spill_mask,
                         mapping_table_offset,
                         vmap_table_offset,
                         gc_map_offset
                         );

  if (compiler_driver_->IsImage()) {
    ClassLinker* linker = Runtime::Current()->GetClassLinker();
    mirror::DexCache* dex_cache = linker->FindDexCache(*dex_file);
    // Unchecked as we hold mutator_lock_ on entry.
    ScopedObjectAccessUnchecked soa(Thread::Current());
    mirror::AbstractMethod* method = linker->ResolveMethod(*dex_file, method_idx, dex_cache,
                                                           NULL, NULL, invoke_type);
    CHECK(method != NULL);
    method->SetFrameSizeInBytes(frame_size_in_bytes);
    method->SetCoreSpillMask(core_spill_mask);
    method->SetFpSpillMask(fp_spill_mask);
    method->SetOatMappingTableOffset(mapping_table_offset);
    // Don't overwrite static method trampoline
    if (!method->IsStatic() || method->IsConstructor() ||
        method->GetDeclaringClass()->IsInitialized()) {
      method->SetOatCodeOffset(code_offset);
    } else {
      method->SetEntryPointFromCompiledCode(NULL);
    }
    method->SetOatVmapTableOffset(vmap_table_offset);
    method->SetOatNativeGcMapOffset(gc_map_offset);
  }

  return offset;
}

#define DCHECK_OFFSET() \
  DCHECK_EQ(static_cast<off_t>(offset), out.Seek(0, kSeekCurrent))

#define DCHECK_OFFSET_() \
  DCHECK_EQ(static_cast<off_t>(offset_), out.Seek(0, kSeekCurrent))

bool OatWriter::Write(OutputStream& out) {
  if (!out.WriteFully(oat_header_, sizeof(*oat_header_))) {
    PLOG(ERROR) << "Failed to write oat header to " << out.GetLocation();
    return false;
  }
  size_oat_header_ += sizeof(*oat_header_);

  if (!out.WriteFully(image_file_location_.data(), image_file_location_.size())) {
    PLOG(ERROR) << "Failed to write oat header image file location to " << out.GetLocation();
    return false;
  }
  size_oat_header_image_file_location_ += image_file_location_.size();

  if (!WriteTables(out)) {
    LOG(ERROR) << "Failed to write oat tables to " << out.GetLocation();
    return false;
  }

  size_t code_offset = WriteCode(out);
  if (code_offset == 0) {
    LOG(ERROR) << "Failed to write oat code to " << out.GetLocation();
    return false;
  }

  code_offset = WriteCodeDexFiles(out, code_offset);
  if (code_offset == 0) {
    LOG(ERROR) << "Failed to write oat code for dex files to " << out.GetLocation();
    return false;
  }

  if (kIsDebugBuild) {
    uint32_t size_total = 0;
    #define DO_STAT(x) \
      LOG(INFO) << #x "=" << PrettySize(x) << " (" << x << "B)"; \
      size_total += x;

    DO_STAT(size_dex_file_alignment_);
    DO_STAT(size_executable_offset_alignment_);
    DO_STAT(size_oat_header_);
    DO_STAT(size_oat_header_image_file_location_);
    DO_STAT(size_dex_file_);
    DO_STAT(size_interpreter_to_interpreter_entry_);
    DO_STAT(size_interpreter_to_quick_entry_);
    DO_STAT(size_portable_resolution_trampoline_);
    DO_STAT(size_quick_resolution_trampoline_);
    DO_STAT(size_stubs_alignment_);
    DO_STAT(size_code_size_);
    DO_STAT(size_code_);
    DO_STAT(size_code_alignment_);
    DO_STAT(size_mapping_table_);
    DO_STAT(size_vmap_table_);
    DO_STAT(size_gc_map_);
    DO_STAT(size_oat_dex_file_location_size_);
    DO_STAT(size_oat_dex_file_location_data_);
    DO_STAT(size_oat_dex_file_location_checksum_);
    DO_STAT(size_oat_dex_file_offset_);
    DO_STAT(size_oat_dex_file_methods_offsets_);
    DO_STAT(size_oat_class_status_);
    DO_STAT(size_oat_class_method_offsets_);
    #undef DO_STAT

    LOG(INFO) << "size_total=" << PrettySize(size_total) << " (" << size_total << "B)"; \
    CHECK_EQ(size_total, static_cast<uint32_t>(out.Seek(0, kSeekCurrent)));
  }

  return true;
}

bool OatWriter::WriteTables(OutputStream& out) {
  for (size_t i = 0; i != oat_dex_files_.size(); ++i) {
    if (!oat_dex_files_[i]->Write(this, out)) {
      PLOG(ERROR) << "Failed to write oat dex information to " << out.GetLocation();
      return false;
    }
  }
  for (size_t i = 0; i != oat_dex_files_.size(); ++i) {
    uint32_t expected_offset = oat_dex_files_[i]->dex_file_offset_;
    off_t actual_offset = out.Seek(expected_offset, kSeekSet);
    if (static_cast<uint32_t>(actual_offset) != expected_offset) {
      const DexFile* dex_file = (*dex_files_)[i];
      PLOG(ERROR) << "Failed to seek to dex file section. Actual: " << actual_offset
                  << " Expected: " << expected_offset << " File: " << dex_file->GetLocation();
      return false;
    }
    const DexFile* dex_file = (*dex_files_)[i];
    if (!out.WriteFully(&dex_file->GetHeader(), dex_file->GetHeader().file_size_)) {
      PLOG(ERROR) << "Failed to write dex file " << dex_file->GetLocation() << " to " << out.GetLocation();
      return false;
    }
    size_dex_file_ += dex_file->GetHeader().file_size_;
  }
  for (size_t i = 0; i != oat_classes_.size(); ++i) {
    if (!oat_classes_[i]->Write(this, out)) {
      PLOG(ERROR) << "Failed to write oat methods information to " << out.GetLocation();
      return false;
    }
  }
  return true;
}

size_t OatWriter::WriteCode(OutputStream& out) {
  uint32_t offset = oat_header_->GetExecutableOffset();
  off_t new_offset = out.Seek(size_executable_offset_alignment_, kSeekCurrent);
  if (static_cast<uint32_t>(new_offset) != offset) {
    PLOG(ERROR) << "Failed to seek to oat code section. Actual: " << new_offset
                << " Expected: " << offset << " File: " << out.GetLocation();
    return 0;
  }
  DCHECK_OFFSET();
  if (compiler_driver_->IsImage()) {
    InstructionSet instruction_set = compiler_driver_->GetInstructionSet();
    if (!out.WriteFully(&(*interpreter_to_interpreter_entry_)[0], interpreter_to_interpreter_entry_->size())) {
      PLOG(ERROR) << "Failed to write interpreter to interpreter entry to " << out.GetLocation();
      return false;
    }
    size_interpreter_to_interpreter_entry_ += interpreter_to_interpreter_entry_->size();
    offset += interpreter_to_interpreter_entry_->size();
    DCHECK_OFFSET();

    uint32_t aligned_offset = CompiledCode::AlignCode(offset, instruction_set);
    uint32_t alignment_padding = aligned_offset - offset;
    out.Seek(alignment_padding, kSeekCurrent);
    size_stubs_alignment_ += alignment_padding;
    if (!out.WriteFully(&(*interpreter_to_quick_entry_)[0], interpreter_to_quick_entry_->size())) {
      PLOG(ERROR) << "Failed to write interpreter to quick entry to " << out.GetLocation();
      return false;
    }
    size_interpreter_to_quick_entry_ += interpreter_to_quick_entry_->size();
    offset += alignment_padding + interpreter_to_quick_entry_->size();
    DCHECK_OFFSET();

    aligned_offset = CompiledCode::AlignCode(offset, instruction_set);
    alignment_padding = aligned_offset - offset;
    out.Seek(alignment_padding, kSeekCurrent);
    size_stubs_alignment_ += alignment_padding;
    if (!out.WriteFully(&(*portable_resolution_trampoline_)[0], portable_resolution_trampoline_->size())) {
      PLOG(ERROR) << "Failed to write portable resolution trampoline to " << out.GetLocation();
      return false;
    }
    size_portable_resolution_trampoline_ += portable_resolution_trampoline_->size();
    offset += alignment_padding + portable_resolution_trampoline_->size();
    DCHECK_OFFSET();

    aligned_offset = CompiledCode::AlignCode(offset, instruction_set);
    alignment_padding = aligned_offset - offset;
    out.Seek(alignment_padding, kSeekCurrent);
    size_stubs_alignment_ += alignment_padding;
    if (!out.WriteFully(&(*quick_resolution_trampoline_)[0], quick_resolution_trampoline_->size())) {
      PLOG(ERROR) << "Failed to write quick resolution trampoline to " << out.GetLocation();
      return false;
    }
    size_quick_resolution_trampoline_ += quick_resolution_trampoline_->size();
    offset += alignment_padding + quick_resolution_trampoline_->size();
    DCHECK_OFFSET();
  }
  return offset;
}

size_t OatWriter::WriteCodeDexFiles(OutputStream& out, size_t code_offset) {
  size_t oat_class_index = 0;
  for (size_t i = 0; i != oat_dex_files_.size(); ++i) {
    const DexFile* dex_file = (*dex_files_)[i];
    CHECK(dex_file != NULL);
    code_offset = WriteCodeDexFile(out, code_offset, oat_class_index, *dex_file);
    if (code_offset == 0) {
      return 0;
    }
  }
  return code_offset;
}

size_t OatWriter::WriteCodeDexFile(OutputStream& out, size_t code_offset, size_t& oat_class_index,
                                   const DexFile& dex_file) {
  for (size_t class_def_index = 0; class_def_index < dex_file.NumClassDefs();
      class_def_index++, oat_class_index++) {
    const DexFile::ClassDef& class_def = dex_file.GetClassDef(class_def_index);
    code_offset = WriteCodeClassDef(out, code_offset, oat_class_index, dex_file, class_def);
    if (code_offset == 0) {
      return 0;
    }
  }
  return code_offset;
}

void OatWriter::ReportWriteFailure(const char* what, uint32_t method_idx,
                                   const DexFile& dex_file, OutputStream& out) const {
  PLOG(ERROR) << "Failed to write " << what << " for " << PrettyMethod(method_idx, dex_file)
      << " to " << out.GetLocation();
}

size_t OatWriter::WriteCodeClassDef(OutputStream& out,
                                    size_t code_offset, size_t oat_class_index,
                                    const DexFile& dex_file,
                                    const DexFile::ClassDef& class_def) {
  const byte* class_data = dex_file.GetClassData(class_def);
  if (class_data == NULL) {
    // ie. an empty class such as a marker interface
    return code_offset;
  }
  ClassDataItemIterator it(dex_file, class_data);
  // Skip fields
  while (it.HasNextStaticField()) {
    it.Next();
  }
  while (it.HasNextInstanceField()) {
    it.Next();
  }
  // Process methods
  size_t class_def_method_index = 0;
  while (it.HasNextDirectMethod()) {
    bool is_static = (it.GetMemberAccessFlags() & kAccStatic) != 0;
    code_offset = WriteCodeMethod(out, code_offset, oat_class_index, class_def_method_index,
                                  is_static, it.GetMemberIndex(), dex_file);
    if (code_offset == 0) {
      return 0;
    }
    class_def_method_index++;
    it.Next();
  }
  while (it.HasNextVirtualMethod()) {
    code_offset = WriteCodeMethod(out, code_offset, oat_class_index, class_def_method_index,
                                  false, it.GetMemberIndex(), dex_file);
    if (code_offset == 0) {
      return 0;
    }
    class_def_method_index++;
    it.Next();
  }
  return code_offset;
}

size_t OatWriter::WriteCodeMethod(OutputStream& out, size_t offset, size_t oat_class_index,
                                  size_t class_def_method_index, bool is_static,
                                  uint32_t method_idx, const DexFile& dex_file) {
  const CompiledMethod* compiled_method =
      compiler_driver_->GetCompiledMethod(MethodReference(&dex_file, method_idx));

  OatMethodOffsets method_offsets =
      oat_classes_[oat_class_index]->method_offsets_[class_def_method_index];


  if (compiled_method != NULL) {  // ie. not an abstract method
#if !defined(ART_USE_PORTABLE_COMPILER)
    uint32_t aligned_offset = compiled_method->AlignCode(offset);
    uint32_t aligned_code_delta = aligned_offset - offset;
    if (aligned_code_delta != 0) {
      off_t new_offset = out.Seek(aligned_code_delta, kSeekCurrent);
      size_code_alignment_ += aligned_code_delta;
      if (static_cast<uint32_t>(new_offset) != aligned_offset) {
        PLOG(ERROR) << "Failed to seek to align oat code. Actual: " << new_offset
                    << " Expected: " << aligned_offset << " File: " << out.GetLocation();
        return 0;
      }
      offset += aligned_code_delta;
      DCHECK_OFFSET();
    }
    DCHECK_ALIGNED(offset, kArmAlignment);
    const std::vector<uint8_t>& code = compiled_method->GetCode();
    uint32_t code_size = code.size() * sizeof(code[0]);
    CHECK_NE(code_size, 0U);

    // Deduplicate code arrays
    size_t code_offset = offset + sizeof(code_size) + compiled_method->CodeDelta();
    SafeMap<const std::vector<uint8_t>*, uint32_t>::iterator code_iter = code_offsets_.find(&code);
    if (code_iter != code_offsets_.end() && code_offset != method_offsets.code_offset_) {
      DCHECK(code_iter->second == method_offsets.code_offset_)
          << PrettyMethod(method_idx, dex_file);
    } else {
      DCHECK(code_offset == method_offsets.code_offset_) << PrettyMethod(method_idx, dex_file);
      if (!out.WriteFully(&code_size, sizeof(code_size))) {
        ReportWriteFailure("method code size", method_idx, dex_file, out);
        return 0;
      }
      size_code_size_ += sizeof(code_size);
      offset += sizeof(code_size);
      DCHECK_OFFSET();
      if (!out.WriteFully(&code[0], code_size)) {
        ReportWriteFailure("method code", method_idx, dex_file, out);
        return 0;
      }
      size_code_ += code_size;
      offset += code_size;
    }
    DCHECK_OFFSET();
#endif

    const std::vector<uint32_t>& mapping_table = compiled_method->GetMappingTable();
    size_t mapping_table_size = mapping_table.size() * sizeof(mapping_table[0]);

    // Deduplicate mapping tables
    SafeMap<const std::vector<uint32_t>*, uint32_t>::iterator mapping_iter =
        mapping_table_offsets_.find(&mapping_table);
    if (mapping_iter != mapping_table_offsets_.end() &&
        offset != method_offsets.mapping_table_offset_) {
      DCHECK((mapping_table_size == 0 && method_offsets.mapping_table_offset_ == 0)
          || mapping_iter->second == method_offsets.mapping_table_offset_)
          << PrettyMethod(method_idx, dex_file);
    } else {
      DCHECK((mapping_table_size == 0 && method_offsets.mapping_table_offset_ == 0)
          || offset == method_offsets.mapping_table_offset_)
          << PrettyMethod(method_idx, dex_file);
      if (!out.WriteFully(&mapping_table[0], mapping_table_size)) {
        ReportWriteFailure("mapping table", method_idx, dex_file, out);
        return 0;
      }
      size_mapping_table_ += mapping_table_size;
      offset += mapping_table_size;
    }
    DCHECK_OFFSET();

    const std::vector<uint16_t>& vmap_table = compiled_method->GetVmapTable();
    size_t vmap_table_size = vmap_table.size() * sizeof(vmap_table[0]);

    // Deduplicate vmap tables
    SafeMap<const std::vector<uint16_t>*, uint32_t>::iterator vmap_iter =
        vmap_table_offsets_.find(&vmap_table);
    if (vmap_iter != vmap_table_offsets_.end() &&
        offset != method_offsets.vmap_table_offset_) {
      DCHECK((vmap_table_size == 0 && method_offsets.vmap_table_offset_ == 0)
          || vmap_iter->second == method_offsets.vmap_table_offset_)
          << PrettyMethod(method_idx, dex_file);
    } else {
      DCHECK((vmap_table_size == 0 && method_offsets.vmap_table_offset_ == 0)
          || offset == method_offsets.vmap_table_offset_)
          << PrettyMethod(method_idx, dex_file);
      if (!out.WriteFully(&vmap_table[0], vmap_table_size)) {
        ReportWriteFailure("vmap table", method_idx, dex_file, out);
        return 0;
      }
      size_vmap_table_ += vmap_table_size;
      offset += vmap_table_size;
    }
    DCHECK_OFFSET();

    const std::vector<uint8_t>& gc_map = compiled_method->GetGcMap();
    size_t gc_map_size = gc_map.size() * sizeof(gc_map[0]);

    // Deduplicate GC maps
    SafeMap<const std::vector<uint8_t>*, uint32_t>::iterator gc_map_iter =
        gc_map_offsets_.find(&gc_map);
    if (gc_map_iter != gc_map_offsets_.end() &&
        offset != method_offsets.gc_map_offset_) {
      DCHECK((gc_map_size == 0 && method_offsets.gc_map_offset_ == 0)
          || gc_map_iter->second == method_offsets.gc_map_offset_)
          << PrettyMethod(method_idx, dex_file);
    } else {
      DCHECK((gc_map_size == 0 && method_offsets.gc_map_offset_ == 0)
          || offset == method_offsets.gc_map_offset_)
          << PrettyMethod(method_idx, dex_file);
      if (!out.WriteFully(&gc_map[0], gc_map_size)) {
        ReportWriteFailure("GC map", method_idx, dex_file, out);
        return 0;
      }
      size_gc_map_ += gc_map_size;
      offset += gc_map_size;
    }
    DCHECK_OFFSET();
  }

  return offset;
}

OatWriter::OatDexFile::OatDexFile(size_t offset, const DexFile& dex_file) {
  offset_ = offset;
  const std::string& location(dex_file.GetLocation());
  dex_file_location_size_ = location.size();
  dex_file_location_data_ = reinterpret_cast<const uint8_t*>(location.data());
  dex_file_location_checksum_ = dex_file.GetLocationChecksum();
  dex_file_offset_ = 0;
  methods_offsets_.resize(dex_file.NumClassDefs());
}

size_t OatWriter::OatDexFile::SizeOf() const {
  return sizeof(dex_file_location_size_)
          + dex_file_location_size_
          + sizeof(dex_file_location_checksum_)
          + sizeof(dex_file_offset_)
          + (sizeof(methods_offsets_[0]) * methods_offsets_.size());
}

void OatWriter::OatDexFile::UpdateChecksum(OatHeader& oat_header) const {
  oat_header.UpdateChecksum(&dex_file_location_size_, sizeof(dex_file_location_size_));
  oat_header.UpdateChecksum(dex_file_location_data_, dex_file_location_size_);
  oat_header.UpdateChecksum(&dex_file_location_checksum_, sizeof(dex_file_location_checksum_));
  oat_header.UpdateChecksum(&dex_file_offset_, sizeof(dex_file_offset_));
  oat_header.UpdateChecksum(&methods_offsets_[0],
                            sizeof(methods_offsets_[0]) * methods_offsets_.size());
}

bool OatWriter::OatDexFile::Write(OatWriter* oat_writer, OutputStream& out) const {
  DCHECK_OFFSET_();
  if (!out.WriteFully(&dex_file_location_size_, sizeof(dex_file_location_size_))) {
    PLOG(ERROR) << "Failed to write dex file location length to " << out.GetLocation();
    return false;
  }
  oat_writer->size_oat_dex_file_location_size_ += sizeof(dex_file_location_size_);
  if (!out.WriteFully(dex_file_location_data_, dex_file_location_size_)) {
    PLOG(ERROR) << "Failed to write dex file location data to " << out.GetLocation();
    return false;
  }
  oat_writer->size_oat_dex_file_location_data_ += dex_file_location_size_;
  if (!out.WriteFully(&dex_file_location_checksum_, sizeof(dex_file_location_checksum_))) {
    PLOG(ERROR) << "Failed to write dex file location checksum to " << out.GetLocation();
    return false;
  }
  oat_writer->size_oat_dex_file_location_checksum_ += sizeof(dex_file_location_checksum_);
  if (!out.WriteFully(&dex_file_offset_, sizeof(dex_file_offset_))) {
    PLOG(ERROR) << "Failed to write dex file offset to " << out.GetLocation();
    return false;
  }
  oat_writer->size_oat_dex_file_offset_ += sizeof(dex_file_offset_);
  if (!out.WriteFully(&methods_offsets_[0],
                      sizeof(methods_offsets_[0]) * methods_offsets_.size())) {
    PLOG(ERROR) << "Failed to write methods offsets to " << out.GetLocation();
    return false;
  }
  oat_writer->size_oat_dex_file_methods_offsets_ +=
      sizeof(methods_offsets_[0]) * methods_offsets_.size();
  return true;
}

OatWriter::OatClass::OatClass(size_t offset, mirror::Class::Status status, uint32_t methods_count) {
  offset_ = offset;
  status_ = status;
  method_offsets_.resize(methods_count);
}

size_t OatWriter::OatClass::GetOatMethodOffsetsOffsetFromOatHeader(
    size_t class_def_method_index_) const {
  return offset_ + GetOatMethodOffsetsOffsetFromOatClass(class_def_method_index_);
}

size_t OatWriter::OatClass::GetOatMethodOffsetsOffsetFromOatClass(
    size_t class_def_method_index_) const {
  return sizeof(status_)
          + (sizeof(method_offsets_[0]) * class_def_method_index_);
}

size_t OatWriter::OatClass::SizeOf() const {
  return GetOatMethodOffsetsOffsetFromOatClass(method_offsets_.size());
}

void OatWriter::OatClass::UpdateChecksum(OatHeader& oat_header) const {
  oat_header.UpdateChecksum(&status_, sizeof(status_));
  oat_header.UpdateChecksum(&method_offsets_[0],
                            sizeof(method_offsets_[0]) * method_offsets_.size());
}

bool OatWriter::OatClass::Write(OatWriter* oat_writer, OutputStream& out) const {
  DCHECK_OFFSET_();
  if (!out.WriteFully(&status_, sizeof(status_))) {
    PLOG(ERROR) << "Failed to write class status to " << out.GetLocation();
    return false;
  }
  oat_writer->size_oat_class_status_ += sizeof(status_);
  DCHECK_EQ(static_cast<off_t>(GetOatMethodOffsetsOffsetFromOatHeader(0)),
            out.Seek(0, kSeekCurrent));
  if (!out.WriteFully(&method_offsets_[0],
                      sizeof(method_offsets_[0]) * method_offsets_.size())) {
    PLOG(ERROR) << "Failed to write method offsets to " << out.GetLocation();
    return false;
  }
  oat_writer->size_oat_class_method_offsets_ += sizeof(method_offsets_[0]) * method_offsets_.size();
  DCHECK_EQ(static_cast<off_t>(GetOatMethodOffsetsOffsetFromOatHeader(method_offsets_.size())),
            out.Seek(0, kSeekCurrent));
  return true;
}

}  // namespace art
