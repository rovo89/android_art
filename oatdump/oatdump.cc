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

#include <stdio.h>
#include <stdlib.h>

#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "arch/instruction_set_features.h"
#include "art_field-inl.h"
#include "art_method-inl.h"
#include "base/stl_util.h"
#include "base/unix_file/fd_file.h"
#include "class_linker.h"
#include "class_linker-inl.h"
#include "debug/elf_debug_writer.h"
#include "debug/method_debug_info.h"
#include "dex_file-inl.h"
#include "dex_instruction.h"
#include "disassembler.h"
#include "elf_builder.h"
#include "gc/space/image_space.h"
#include "gc/space/large_object_space.h"
#include "gc/space/space-inl.h"
#include "image-inl.h"
#include "indenter.h"
#include "linker/buffered_output_stream.h"
#include "linker/file_output_stream.h"
#include "mirror/array-inl.h"
#include "mirror/class-inl.h"
#include "mirror/dex_cache-inl.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "oat.h"
#include "oat_file-inl.h"
#include "oat_file_manager.h"
#include "os.h"
#include "safe_map.h"
#include "scoped_thread_state_change.h"
#include "stack_map.h"
#include "ScopedLocalRef.h"
#include "thread_list.h"
#include "type_lookup_table.h"
#include "verifier/method_verifier.h"
#include "well_known_classes.h"

#include <sys/stat.h>
#include "cmdline.h"

namespace art {

const char* image_methods_descriptions_[] = {
  "kResolutionMethod",
  "kImtConflictMethod",
  "kImtUnimplementedMethod",
  "kCalleeSaveMethod",
  "kRefsOnlySaveMethod",
  "kRefsAndArgsSaveMethod",
};

const char* image_roots_descriptions_[] = {
  "kDexCaches",
  "kClassRoots",
};

// Map is so that we don't allocate multiple dex files for the same OatDexFile.
static std::map<const OatFile::OatDexFile*,
                std::unique_ptr<const DexFile>> opened_dex_files;

const DexFile* OpenDexFile(const OatFile::OatDexFile* oat_dex_file, std::string* error_msg) {
  DCHECK(oat_dex_file != nullptr);
  auto it = opened_dex_files.find(oat_dex_file);
  if (it != opened_dex_files.end()) {
    return it->second.get();
  }
  const DexFile* ret = oat_dex_file->OpenDexFile(error_msg).release();
  opened_dex_files.emplace(oat_dex_file, std::unique_ptr<const DexFile>(ret));
  return ret;
}

template <typename ElfTypes>
class OatSymbolizer FINAL {
 public:
  OatSymbolizer(const OatFile* oat_file, const std::string& output_name, bool no_bits) :
      oat_file_(oat_file),
      builder_(nullptr),
      output_name_(output_name.empty() ? "symbolized.oat" : output_name),
      no_bits_(no_bits) {
  }

  bool Symbolize() {
    const InstructionSet isa = oat_file_->GetOatHeader().GetInstructionSet();
    const InstructionSetFeatures* features = InstructionSetFeatures::FromBitmap(
        isa, oat_file_->GetOatHeader().GetInstructionSetFeaturesBitmap());

    File* elf_file = OS::CreateEmptyFile(output_name_.c_str());
    std::unique_ptr<BufferedOutputStream> output_stream(
        MakeUnique<BufferedOutputStream>(MakeUnique<FileOutputStream>(elf_file)));
    builder_.reset(new ElfBuilder<ElfTypes>(isa, features, output_stream.get()));

    builder_->Start();

    auto* rodata = builder_->GetRoData();
    auto* text = builder_->GetText();
    auto* bss = builder_->GetBss();

    const uint8_t* rodata_begin = oat_file_->Begin();
    const size_t rodata_size = oat_file_->GetOatHeader().GetExecutableOffset();
    if (no_bits_) {
      rodata->WriteNoBitsSection(rodata_size);
    } else {
      rodata->Start();
      rodata->WriteFully(rodata_begin, rodata_size);
      rodata->End();
    }

    const uint8_t* text_begin = oat_file_->Begin() + rodata_size;
    const size_t text_size = oat_file_->End() - text_begin;
    if (no_bits_) {
      text->WriteNoBitsSection(text_size);
    } else {
      text->Start();
      text->WriteFully(text_begin, text_size);
      text->End();
    }

    if (oat_file_->BssSize() != 0) {
      bss->WriteNoBitsSection(oat_file_->BssSize());
    }

    if (isa == kMips || isa == kMips64) {
      builder_->WriteMIPSabiflagsSection();
    }
    builder_->PrepareDynamicSection(
        elf_file->GetPath(), rodata_size, text_size, oat_file_->BssSize());
    builder_->WriteDynamicSection();

    Walk();
    for (const auto& trampoline : debug::MakeTrampolineInfos(oat_file_->GetOatHeader())) {
      method_debug_infos_.push_back(trampoline);
    }

    debug::WriteDebugInfo(builder_.get(),
                          ArrayRef<const debug::MethodDebugInfo>(method_debug_infos_),
                          dwarf::DW_DEBUG_FRAME_FORMAT,
                          true /* write_oat_patches */);

    builder_->End();

    return builder_->Good();
  }

  void Walk() {
    std::vector<const OatFile::OatDexFile*> oat_dex_files = oat_file_->GetOatDexFiles();
    for (size_t i = 0; i < oat_dex_files.size(); i++) {
      const OatFile::OatDexFile* oat_dex_file = oat_dex_files[i];
      CHECK(oat_dex_file != nullptr);
      WalkOatDexFile(oat_dex_file);
    }
  }

  void WalkOatDexFile(const OatFile::OatDexFile* oat_dex_file) {
    std::string error_msg;
    const DexFile* const dex_file = OpenDexFile(oat_dex_file, &error_msg);
    if (dex_file == nullptr) {
      return;
    }
    for (size_t class_def_index = 0;
        class_def_index < dex_file->NumClassDefs();
        class_def_index++) {
      const OatFile::OatClass oat_class = oat_dex_file->GetOatClass(class_def_index);
      OatClassType type = oat_class.GetType();
      switch (type) {
        case kOatClassAllCompiled:
        case kOatClassSomeCompiled:
          WalkOatClass(oat_class, *dex_file, class_def_index);
          break;

        case kOatClassNoneCompiled:
        case kOatClassMax:
          // Ignore.
          break;
      }
    }
  }

  void WalkOatClass(const OatFile::OatClass& oat_class,
                    const DexFile& dex_file,
                    uint32_t class_def_index) {
    const DexFile::ClassDef& class_def = dex_file.GetClassDef(class_def_index);
    const uint8_t* class_data = dex_file.GetClassData(class_def);
    if (class_data == nullptr) {  // empty class such as a marker interface?
      return;
    }
    // Note: even if this is an interface or a native class, we still have to walk it, as there
    //       might be a static initializer.
    ClassDataItemIterator it(dex_file, class_data);
    uint32_t class_method_idx = 0;
    for (; it.HasNextStaticField(); it.Next()) { /* skip */ }
    for (; it.HasNextInstanceField(); it.Next()) { /* skip */ }
    for (; it.HasNextDirectMethod() || it.HasNextVirtualMethod(); it.Next()) {
      WalkOatMethod(oat_class.GetOatMethod(class_method_idx++),
                    dex_file,
                    class_def_index,
                    it.GetMemberIndex(),
                    it.GetMethodCodeItem(),
                    it.GetMethodAccessFlags());
    }
    DCHECK(!it.HasNext());
  }

  void WalkOatMethod(const OatFile::OatMethod& oat_method,
                     const DexFile& dex_file,
                     uint32_t class_def_index,
                     uint32_t dex_method_index,
                     const DexFile::CodeItem* code_item,
                     uint32_t method_access_flags) {
    if ((method_access_flags & kAccAbstract) != 0) {
      // Abstract method, no code.
      return;
    }
    const OatHeader& oat_header = oat_file_->GetOatHeader();
    const OatQuickMethodHeader* method_header = oat_method.GetOatQuickMethodHeader();
    if (method_header == nullptr || method_header->GetCodeSize() == 0) {
      // No code.
      return;
    }

    uint32_t entry_point = oat_method.GetCodeOffset() - oat_header.GetExecutableOffset();
    // Clear Thumb2 bit.
    const void* code_address = EntryPointToCodePointer(reinterpret_cast<void*>(entry_point));

    debug::MethodDebugInfo info = debug::MethodDebugInfo();
    info.trampoline_name = nullptr;
    info.dex_file = &dex_file;
    info.class_def_index = class_def_index;
    info.dex_method_index = dex_method_index;
    info.access_flags = method_access_flags;
    info.code_item = code_item;
    info.isa = oat_header.GetInstructionSet();
    info.deduped = !seen_offsets_.insert(oat_method.GetCodeOffset()).second;
    info.is_native_debuggable = oat_header.IsNativeDebuggable();
    info.is_optimized = method_header->IsOptimized();
    info.is_code_address_text_relative = true;
    info.code_address = reinterpret_cast<uintptr_t>(code_address);
    info.code_size = method_header->GetCodeSize();
    info.frame_size_in_bytes = method_header->GetFrameSizeInBytes();
    info.code_info = info.is_optimized ? method_header->GetOptimizedCodeInfoPtr() : nullptr;
    info.cfi = ArrayRef<uint8_t>();
    method_debug_infos_.push_back(info);
  }

 private:
  const OatFile* oat_file_;
  std::unique_ptr<ElfBuilder<ElfTypes> > builder_;
  std::vector<debug::MethodDebugInfo> method_debug_infos_;
  std::unordered_set<uint32_t> seen_offsets_;
  const std::string output_name_;
  bool no_bits_;
};

class OatDumperOptions {
 public:
  OatDumperOptions(bool dump_vmap,
                   bool dump_code_info_stack_maps,
                   bool disassemble_code,
                   bool absolute_addresses,
                   const char* class_filter,
                   const char* method_filter,
                   bool list_classes,
                   bool list_methods,
                   bool dump_header_only,
                   const char* export_dex_location,
                   const char* app_image,
                   const char* app_oat,
                   uint32_t addr2instr)
    : dump_vmap_(dump_vmap),
      dump_code_info_stack_maps_(dump_code_info_stack_maps),
      disassemble_code_(disassemble_code),
      absolute_addresses_(absolute_addresses),
      class_filter_(class_filter),
      method_filter_(method_filter),
      list_classes_(list_classes),
      list_methods_(list_methods),
      dump_header_only_(dump_header_only),
      export_dex_location_(export_dex_location),
      app_image_(app_image),
      app_oat_(app_oat),
      addr2instr_(addr2instr),
      class_loader_(nullptr) {}

  const bool dump_vmap_;
  const bool dump_code_info_stack_maps_;
  const bool disassemble_code_;
  const bool absolute_addresses_;
  const char* const class_filter_;
  const char* const method_filter_;
  const bool list_classes_;
  const bool list_methods_;
  const bool dump_header_only_;
  const char* const export_dex_location_;
  const char* const app_image_;
  const char* const app_oat_;
  uint32_t addr2instr_;
  Handle<mirror::ClassLoader>* class_loader_;
};

class OatDumper {
 public:
  OatDumper(const OatFile& oat_file, const OatDumperOptions& options)
    : oat_file_(oat_file),
      oat_dex_files_(oat_file.GetOatDexFiles()),
      options_(options),
      resolved_addr2instr_(0),
      instruction_set_(oat_file_.GetOatHeader().GetInstructionSet()),
      disassembler_(Disassembler::Create(instruction_set_,
                                         new DisassemblerOptions(options_.absolute_addresses_,
                                                                 oat_file.Begin(),
                                                                 oat_file.End(),
                                                                 true /* can_read_literals_ */))) {
    CHECK(options_.class_loader_ != nullptr);
    CHECK(options_.class_filter_ != nullptr);
    CHECK(options_.method_filter_ != nullptr);
    AddAllOffsets();
  }

  ~OatDumper() {
    delete disassembler_;
  }

  InstructionSet GetInstructionSet() {
    return instruction_set_;
  }

  bool Dump(std::ostream& os) {
    bool success = true;
    const OatHeader& oat_header = oat_file_.GetOatHeader();

    os << "MAGIC:\n";
    os << oat_header.GetMagic() << "\n\n";

    os << "LOCATION:\n";
    os << oat_file_.GetLocation() << "\n\n";

    os << "CHECKSUM:\n";
    os << StringPrintf("0x%08x\n\n", oat_header.GetChecksum());

    os << "INSTRUCTION SET:\n";
    os << oat_header.GetInstructionSet() << "\n\n";

    {
      std::unique_ptr<const InstructionSetFeatures> features(
          InstructionSetFeatures::FromBitmap(oat_header.GetInstructionSet(),
                                             oat_header.GetInstructionSetFeaturesBitmap()));
      os << "INSTRUCTION SET FEATURES:\n";
      os << features->GetFeatureString() << "\n\n";
    }

    os << "DEX FILE COUNT:\n";
    os << oat_header.GetDexFileCount() << "\n\n";

#define DUMP_OAT_HEADER_OFFSET(label, offset) \
    os << label " OFFSET:\n"; \
    os << StringPrintf("0x%08x", oat_header.offset()); \
    if (oat_header.offset() != 0 && options_.absolute_addresses_) { \
      os << StringPrintf(" (%p)", oat_file_.Begin() + oat_header.offset()); \
    } \
    os << StringPrintf("\n\n");

    DUMP_OAT_HEADER_OFFSET("EXECUTABLE", GetExecutableOffset);
    DUMP_OAT_HEADER_OFFSET("INTERPRETER TO INTERPRETER BRIDGE",
                           GetInterpreterToInterpreterBridgeOffset);
    DUMP_OAT_HEADER_OFFSET("INTERPRETER TO COMPILED CODE BRIDGE",
                           GetInterpreterToCompiledCodeBridgeOffset);
    DUMP_OAT_HEADER_OFFSET("JNI DLSYM LOOKUP",
                           GetJniDlsymLookupOffset);
    DUMP_OAT_HEADER_OFFSET("QUICK GENERIC JNI TRAMPOLINE",
                           GetQuickGenericJniTrampolineOffset);
    DUMP_OAT_HEADER_OFFSET("QUICK IMT CONFLICT TRAMPOLINE",
                           GetQuickImtConflictTrampolineOffset);
    DUMP_OAT_HEADER_OFFSET("QUICK RESOLUTION TRAMPOLINE",
                           GetQuickResolutionTrampolineOffset);
    DUMP_OAT_HEADER_OFFSET("QUICK TO INTERPRETER BRIDGE",
                           GetQuickToInterpreterBridgeOffset);
#undef DUMP_OAT_HEADER_OFFSET

    os << "IMAGE PATCH DELTA:\n";
    os << StringPrintf("%d (0x%08x)\n\n",
                       oat_header.GetImagePatchDelta(),
                       oat_header.GetImagePatchDelta());

    os << "IMAGE FILE LOCATION OAT CHECKSUM:\n";
    os << StringPrintf("0x%08x\n\n", oat_header.GetImageFileLocationOatChecksum());

    os << "IMAGE FILE LOCATION OAT BEGIN:\n";
    os << StringPrintf("0x%08x\n\n", oat_header.GetImageFileLocationOatDataBegin());

    // Print the key-value store.
    {
      os << "KEY VALUE STORE:\n";
      size_t index = 0;
      const char* key;
      const char* value;
      while (oat_header.GetStoreKeyValuePairByIndex(index, &key, &value)) {
        os << key << " = " << value << "\n";
        index++;
      }
      os << "\n";
    }

    if (options_.absolute_addresses_) {
      os << "BEGIN:\n";
      os << reinterpret_cast<const void*>(oat_file_.Begin()) << "\n\n";

      os << "END:\n";
      os << reinterpret_cast<const void*>(oat_file_.End()) << "\n\n";
    }

    os << "SIZE:\n";
    os << oat_file_.Size() << "\n\n";

    os << std::flush;

    // If set, adjust relative address to be searched
    if (options_.addr2instr_ != 0) {
      resolved_addr2instr_ = options_.addr2instr_ + oat_header.GetExecutableOffset();
      os << "SEARCH ADDRESS (executable offset + input):\n";
      os << StringPrintf("0x%08x\n\n", resolved_addr2instr_);
    }

    if (!options_.dump_header_only_) {
      for (size_t i = 0; i < oat_dex_files_.size(); i++) {
        const OatFile::OatDexFile* oat_dex_file = oat_dex_files_[i];
        CHECK(oat_dex_file != nullptr);

        // If file export selected skip file analysis
        if (options_.export_dex_location_) {
          if (!ExportDexFile(os, *oat_dex_file)) {
            success = false;
          }
        } else {
          if (!DumpOatDexFile(os, *oat_dex_file)) {
            success = false;
          }
        }
      }
    }

    os << std::flush;
    return success;
  }

  size_t ComputeSize(const void* oat_data) {
    if (reinterpret_cast<const uint8_t*>(oat_data) < oat_file_.Begin() ||
        reinterpret_cast<const uint8_t*>(oat_data) > oat_file_.End()) {
      return 0;  // Address not in oat file
    }
    uintptr_t begin_offset = reinterpret_cast<uintptr_t>(oat_data) -
                             reinterpret_cast<uintptr_t>(oat_file_.Begin());
    auto it = offsets_.upper_bound(begin_offset);
    CHECK(it != offsets_.end());
    uintptr_t end_offset = *it;
    return end_offset - begin_offset;
  }

  InstructionSet GetOatInstructionSet() {
    return oat_file_.GetOatHeader().GetInstructionSet();
  }

  const void* GetQuickOatCode(ArtMethod* m) SHARED_REQUIRES(Locks::mutator_lock_) {
    for (size_t i = 0; i < oat_dex_files_.size(); i++) {
      const OatFile::OatDexFile* oat_dex_file = oat_dex_files_[i];
      CHECK(oat_dex_file != nullptr);
      std::string error_msg;
      const DexFile* const dex_file = OpenDexFile(oat_dex_file, &error_msg);
      if (dex_file == nullptr) {
        LOG(WARNING) << "Failed to open dex file '" << oat_dex_file->GetDexFileLocation()
            << "': " << error_msg;
      } else {
        const char* descriptor = m->GetDeclaringClassDescriptor();
        const DexFile::ClassDef* class_def =
            dex_file->FindClassDef(descriptor, ComputeModifiedUtf8Hash(descriptor));
        if (class_def != nullptr) {
          uint16_t class_def_index = dex_file->GetIndexForClassDef(*class_def);
          const OatFile::OatClass oat_class = oat_dex_file->GetOatClass(class_def_index);
          size_t method_index = m->GetMethodIndex();
          return oat_class.GetOatMethod(method_index).GetQuickCode();
        }
      }
    }
    return nullptr;
  }

 private:
  void AddAllOffsets() {
    // We don't know the length of the code for each method, but we need to know where to stop
    // when disassembling. What we do know is that a region of code will be followed by some other
    // region, so if we keep a sorted sequence of the start of each region, we can infer the length
    // of a piece of code by using upper_bound to find the start of the next region.
    for (size_t i = 0; i < oat_dex_files_.size(); i++) {
      const OatFile::OatDexFile* oat_dex_file = oat_dex_files_[i];
      CHECK(oat_dex_file != nullptr);
      std::string error_msg;
      const DexFile* const dex_file = OpenDexFile(oat_dex_file, &error_msg);
      if (dex_file == nullptr) {
        LOG(WARNING) << "Failed to open dex file '" << oat_dex_file->GetDexFileLocation()
            << "': " << error_msg;
        continue;
      }
      offsets_.insert(reinterpret_cast<uintptr_t>(&dex_file->GetHeader()));
      for (size_t class_def_index = 0;
           class_def_index < dex_file->NumClassDefs();
           class_def_index++) {
        const DexFile::ClassDef& class_def = dex_file->GetClassDef(class_def_index);
        const OatFile::OatClass oat_class = oat_dex_file->GetOatClass(class_def_index);
        const uint8_t* class_data = dex_file->GetClassData(class_def);
        if (class_data != nullptr) {
          ClassDataItemIterator it(*dex_file, class_data);
          SkipAllFields(it);
          uint32_t class_method_index = 0;
          while (it.HasNextDirectMethod()) {
            AddOffsets(oat_class.GetOatMethod(class_method_index++));
            it.Next();
          }
          while (it.HasNextVirtualMethod()) {
            AddOffsets(oat_class.GetOatMethod(class_method_index++));
            it.Next();
          }
        }
      }
    }

    // If the last thing in the file is code for a method, there won't be an offset for the "next"
    // thing. Instead of having a special case in the upper_bound code, let's just add an entry
    // for the end of the file.
    offsets_.insert(oat_file_.Size());
  }

  static uint32_t AlignCodeOffset(uint32_t maybe_thumb_offset) {
    return maybe_thumb_offset & ~0x1;  // TODO: Make this Thumb2 specific.
  }

  void AddOffsets(const OatFile::OatMethod& oat_method) {
    uint32_t code_offset = oat_method.GetCodeOffset();
    if (oat_file_.GetOatHeader().GetInstructionSet() == kThumb2) {
      code_offset &= ~0x1;
    }
    offsets_.insert(code_offset);
    offsets_.insert(oat_method.GetVmapTableOffset());
  }

  bool DumpOatDexFile(std::ostream& os, const OatFile::OatDexFile& oat_dex_file) {
    bool success = true;
    bool stop_analysis = false;
    os << "OatDexFile:\n";
    os << StringPrintf("location: %s\n", oat_dex_file.GetDexFileLocation().c_str());
    os << StringPrintf("checksum: 0x%08x\n", oat_dex_file.GetDexFileLocationChecksum());

    // Print embedded dex file data range.
    const uint8_t* const oat_file_begin = oat_dex_file.GetOatFile()->Begin();
    const uint8_t* const dex_file_pointer = oat_dex_file.GetDexFilePointer();
    uint32_t dex_offset = dchecked_integral_cast<uint32_t>(dex_file_pointer - oat_file_begin);
    os << StringPrintf("dex-file: 0x%08x..0x%08x\n",
                       dex_offset,
                       dchecked_integral_cast<uint32_t>(dex_offset + oat_dex_file.FileSize() - 1));

    // Create the dex file early. A lot of print-out things depend on it.
    std::string error_msg;
    const DexFile* const dex_file = OpenDexFile(&oat_dex_file, &error_msg);
    if (dex_file == nullptr) {
      os << "NOT FOUND: " << error_msg << "\n\n";
      os << std::flush;
      return false;
    }

    // Print lookup table, if it exists.
    if (oat_dex_file.GetLookupTableData() != nullptr) {
      uint32_t table_offset = dchecked_integral_cast<uint32_t>(
          oat_dex_file.GetLookupTableData() - oat_file_begin);
      uint32_t table_size = TypeLookupTable::RawDataLength(*dex_file);
      os << StringPrintf("type-table: 0x%08x..0x%08x\n",
                         table_offset,
                         table_offset + table_size - 1);
    }

    VariableIndentationOutputStream vios(&os);
    ScopedIndentation indent1(&vios);
    for (size_t class_def_index = 0;
         class_def_index < dex_file->NumClassDefs();
         class_def_index++) {
      const DexFile::ClassDef& class_def = dex_file->GetClassDef(class_def_index);
      const char* descriptor = dex_file->GetClassDescriptor(class_def);

      // TODO: Support regex
      if (DescriptorToDot(descriptor).find(options_.class_filter_) == std::string::npos) {
        continue;
      }

      uint32_t oat_class_offset = oat_dex_file.GetOatClassOffset(class_def_index);
      const OatFile::OatClass oat_class = oat_dex_file.GetOatClass(class_def_index);
      os << StringPrintf("%zd: %s (offset=0x%08x) (type_idx=%d)",
                         class_def_index, descriptor, oat_class_offset, class_def.class_idx_)
         << " (" << oat_class.GetStatus() << ")"
         << " (" << oat_class.GetType() << ")\n";
      // TODO: include bitmap here if type is kOatClassSomeCompiled?
      if (options_.list_classes_) continue;
      if (!DumpOatClass(&vios, oat_class, *dex_file, class_def, &stop_analysis)) {
        success = false;
      }
      if (stop_analysis) {
        os << std::flush;
        return success;
      }
    }

    os << std::flush;
    return success;
  }

  bool ExportDexFile(std::ostream& os, const OatFile::OatDexFile& oat_dex_file) {
    std::string error_msg;
    std::string dex_file_location = oat_dex_file.GetDexFileLocation();

    const DexFile* const dex_file = OpenDexFile(&oat_dex_file, &error_msg);
    if (dex_file == nullptr) {
      os << "Failed to open dex file '" << dex_file_location << "': " << error_msg;
      return false;
    }
    size_t fsize = oat_dex_file.FileSize();

    // Some quick checks just in case
    if (fsize == 0 || fsize < sizeof(DexFile::Header)) {
      os << "Invalid dex file\n";
      return false;
    }

    // Verify output directory exists
    if (!OS::DirectoryExists(options_.export_dex_location_)) {
      // TODO: Extend OS::DirectoryExists if symlink support is required
      os << options_.export_dex_location_ << " output directory not found or symlink\n";
      return false;
    }

    // Beautify path names
    if (dex_file_location.size() > PATH_MAX || dex_file_location.size() <= 0) {
      return false;
    }

    std::string dex_orig_name;
    size_t dex_orig_pos = dex_file_location.rfind('/');
    if (dex_orig_pos == std::string::npos)
      dex_orig_name = dex_file_location;
    else
      dex_orig_name = dex_file_location.substr(dex_orig_pos + 1);

    // A more elegant approach to efficiently name user installed apps is welcome
    if (dex_orig_name.size() == 8 && !dex_orig_name.compare("base.apk")) {
      dex_file_location.erase(dex_orig_pos, strlen("base.apk") + 1);
      size_t apk_orig_pos = dex_file_location.rfind('/');
      if (apk_orig_pos != std::string::npos) {
        dex_orig_name = dex_file_location.substr(++apk_orig_pos);
      }
    }

    std::string out_dex_path(options_.export_dex_location_);
    if (out_dex_path.back() != '/') {
      out_dex_path.append("/");
    }
    out_dex_path.append(dex_orig_name);
    out_dex_path.append("_export.dex");
    if (out_dex_path.length() > PATH_MAX) {
      return false;
    }

    std::unique_ptr<File> file(OS::CreateEmptyFile(out_dex_path.c_str()));
    if (file.get() == nullptr) {
      os << "Failed to open output dex file " << out_dex_path;
      return false;
    }

    if (!file->WriteFully(dex_file->Begin(), fsize)) {
      os << "Failed to write dex file";
      file->Erase();
      return false;
    }

    if (file->FlushCloseOrErase() != 0) {
      os << "Flush and close failed";
      return false;
    }

    os << StringPrintf("Dex file exported at %s (%zd bytes)\n", out_dex_path.c_str(), fsize);
    os << std::flush;

    return true;
  }

  static void SkipAllFields(ClassDataItemIterator& it) {
    while (it.HasNextStaticField()) {
      it.Next();
    }
    while (it.HasNextInstanceField()) {
      it.Next();
    }
  }

  bool DumpOatClass(VariableIndentationOutputStream* vios,
                    const OatFile::OatClass& oat_class, const DexFile& dex_file,
                    const DexFile::ClassDef& class_def, bool* stop_analysis) {
    bool success = true;
    bool addr_found = false;
    const uint8_t* class_data = dex_file.GetClassData(class_def);
    if (class_data == nullptr) {  // empty class such as a marker interface?
      vios->Stream() << std::flush;
      return success;
    }
    ClassDataItemIterator it(dex_file, class_data);
    SkipAllFields(it);
    uint32_t class_method_index = 0;
    while (it.HasNextDirectMethod()) {
      if (!DumpOatMethod(vios, class_def, class_method_index, oat_class, dex_file,
                         it.GetMemberIndex(), it.GetMethodCodeItem(),
                         it.GetRawMemberAccessFlags(), &addr_found)) {
        success = false;
      }
      if (addr_found) {
        *stop_analysis = true;
        return success;
      }
      class_method_index++;
      it.Next();
    }
    while (it.HasNextVirtualMethod()) {
      if (!DumpOatMethod(vios, class_def, class_method_index, oat_class, dex_file,
                         it.GetMemberIndex(), it.GetMethodCodeItem(),
                         it.GetRawMemberAccessFlags(), &addr_found)) {
        success = false;
      }
      if (addr_found) {
        *stop_analysis = true;
        return success;
      }
      class_method_index++;
      it.Next();
    }
    DCHECK(!it.HasNext());
    vios->Stream() << std::flush;
    return success;
  }

  static constexpr uint32_t kPrologueBytes = 16;

  // When this was picked, the largest arm method was 55,256 bytes and arm64 was 50,412 bytes.
  static constexpr uint32_t kMaxCodeSize = 100 * 1000;

  bool DumpOatMethod(VariableIndentationOutputStream* vios,
                     const DexFile::ClassDef& class_def,
                     uint32_t class_method_index,
                     const OatFile::OatClass& oat_class, const DexFile& dex_file,
                     uint32_t dex_method_idx, const DexFile::CodeItem* code_item,
                     uint32_t method_access_flags, bool* addr_found) {
    bool success = true;

    // TODO: Support regex
    std::string method_name = dex_file.GetMethodName(dex_file.GetMethodId(dex_method_idx));
    if (method_name.find(options_.method_filter_) == std::string::npos) {
      return success;
    }

    std::string pretty_method = PrettyMethod(dex_method_idx, dex_file, true);
    vios->Stream() << StringPrintf("%d: %s (dex_method_idx=%d)\n",
                                   class_method_index, pretty_method.c_str(),
                                   dex_method_idx);
    if (options_.list_methods_) return success;

    uint32_t oat_method_offsets_offset = oat_class.GetOatMethodOffsetsOffset(class_method_index);
    const OatMethodOffsets* oat_method_offsets = oat_class.GetOatMethodOffsets(class_method_index);
    const OatFile::OatMethod oat_method = oat_class.GetOatMethod(class_method_index);
    uint32_t code_offset = oat_method.GetCodeOffset();
    uint32_t code_size = oat_method.GetQuickCodeSize();
    if (resolved_addr2instr_ != 0) {
      if (resolved_addr2instr_ > code_offset + code_size) {
        return success;
      } else {
        *addr_found = true;  // stop analyzing file at next iteration
      }
    }

    // Everything below is indented at least once.
    ScopedIndentation indent1(vios);

    {
      vios->Stream() << "DEX CODE:\n";
      ScopedIndentation indent2(vios);
      DumpDexCode(vios->Stream(), dex_file, code_item);
    }

    std::unique_ptr<StackHandleScope<1>> hs;
    std::unique_ptr<verifier::MethodVerifier> verifier;
    if (Runtime::Current() != nullptr) {
      // We need to have the handle scope stay live until after the verifier since the verifier has
      // a handle to the dex cache from hs.
      hs.reset(new StackHandleScope<1>(Thread::Current()));
      vios->Stream() << "VERIFIER TYPE ANALYSIS:\n";
      ScopedIndentation indent2(vios);
      verifier.reset(DumpVerifier(vios, hs.get(),
                                  dex_method_idx, &dex_file, class_def, code_item,
                                  method_access_flags));
    }
    {
      vios->Stream() << "OatMethodOffsets ";
      if (options_.absolute_addresses_) {
        vios->Stream() << StringPrintf("%p ", oat_method_offsets);
      }
      vios->Stream() << StringPrintf("(offset=0x%08x)\n", oat_method_offsets_offset);
      if (oat_method_offsets_offset > oat_file_.Size()) {
        vios->Stream() << StringPrintf(
            "WARNING: oat method offsets offset 0x%08x is past end of file 0x%08zx.\n",
            oat_method_offsets_offset, oat_file_.Size());
        // If we can't read OatMethodOffsets, the rest of the data is dangerous to read.
        vios->Stream() << std::flush;
        return false;
      }

      ScopedIndentation indent2(vios);
      vios->Stream() << StringPrintf("code_offset: 0x%08x ", code_offset);
      uint32_t aligned_code_begin = AlignCodeOffset(oat_method.GetCodeOffset());
      if (aligned_code_begin > oat_file_.Size()) {
        vios->Stream() << StringPrintf("WARNING: "
                                       "code offset 0x%08x is past end of file 0x%08zx.\n",
                                       aligned_code_begin, oat_file_.Size());
        success = false;
      }
      vios->Stream() << "\n";
    }
    {
      vios->Stream() << "OatQuickMethodHeader ";
      uint32_t method_header_offset = oat_method.GetOatQuickMethodHeaderOffset();
      const OatQuickMethodHeader* method_header = oat_method.GetOatQuickMethodHeader();

      if (options_.absolute_addresses_) {
        vios->Stream() << StringPrintf("%p ", method_header);
      }
      vios->Stream() << StringPrintf("(offset=0x%08x)\n", method_header_offset);
      if (method_header_offset > oat_file_.Size()) {
        vios->Stream() << StringPrintf(
            "WARNING: oat quick method header offset 0x%08x is past end of file 0x%08zx.\n",
            method_header_offset, oat_file_.Size());
        // If we can't read the OatQuickMethodHeader, the rest of the data is dangerous to read.
        vios->Stream() << std::flush;
        return false;
      }

      ScopedIndentation indent2(vios);
      vios->Stream() << "vmap_table: ";
      if (options_.absolute_addresses_) {
        vios->Stream() << StringPrintf("%p ", oat_method.GetVmapTable());
      }
      uint32_t vmap_table_offset = oat_method.GetVmapTableOffset();
      vios->Stream() << StringPrintf("(offset=0x%08x)\n", vmap_table_offset);
      if (vmap_table_offset > oat_file_.Size()) {
        vios->Stream() << StringPrintf("WARNING: "
                                       "vmap table offset 0x%08x is past end of file 0x%08zx. "
                                       "vmap table offset was loaded from offset 0x%08x.\n",
                                       vmap_table_offset, oat_file_.Size(),
                                       oat_method.GetVmapTableOffsetOffset());
        success = false;
      } else if (options_.dump_vmap_) {
        DumpVmapData(vios, oat_method, code_item);
      }
    }
    {
      vios->Stream() << "QuickMethodFrameInfo\n";

      ScopedIndentation indent2(vios);
      vios->Stream()
          << StringPrintf("frame_size_in_bytes: %zd\n", oat_method.GetFrameSizeInBytes());
      vios->Stream() << StringPrintf("core_spill_mask: 0x%08x ", oat_method.GetCoreSpillMask());
      DumpSpillMask(vios->Stream(), oat_method.GetCoreSpillMask(), false);
      vios->Stream() << "\n";
      vios->Stream() << StringPrintf("fp_spill_mask: 0x%08x ", oat_method.GetFpSpillMask());
      DumpSpillMask(vios->Stream(), oat_method.GetFpSpillMask(), true);
      vios->Stream() << "\n";
    }
    {
      // Based on spill masks from QuickMethodFrameInfo so placed
      // after it is dumped, but useful for understanding quick
      // code, so dumped here.
      ScopedIndentation indent2(vios);
      DumpVregLocations(vios->Stream(), oat_method, code_item);
    }
    {
      vios->Stream() << "CODE: ";
      uint32_t code_size_offset = oat_method.GetQuickCodeSizeOffset();
      if (code_size_offset > oat_file_.Size()) {
        ScopedIndentation indent2(vios);
        vios->Stream() << StringPrintf("WARNING: "
                                       "code size offset 0x%08x is past end of file 0x%08zx.",
                                       code_size_offset, oat_file_.Size());
        success = false;
      } else {
        const void* code = oat_method.GetQuickCode();
        uint32_t aligned_code_begin = AlignCodeOffset(code_offset);
        uint64_t aligned_code_end = aligned_code_begin + code_size;

        if (options_.absolute_addresses_) {
          vios->Stream() << StringPrintf("%p ", code);
        }
        vios->Stream() << StringPrintf("(code_offset=0x%08x size_offset=0x%08x size=%u)%s\n",
                                       code_offset,
                                       code_size_offset,
                                       code_size,
                                       code != nullptr ? "..." : "");

        ScopedIndentation indent2(vios);
        if (aligned_code_begin > oat_file_.Size()) {
          vios->Stream() << StringPrintf("WARNING: "
                                         "start of code at 0x%08x is past end of file 0x%08zx.",
                                         aligned_code_begin, oat_file_.Size());
          success = false;
        } else if (aligned_code_end > oat_file_.Size()) {
          vios->Stream() << StringPrintf(
              "WARNING: "
              "end of code at 0x%08" PRIx64 " is past end of file 0x%08zx. "
              "code size is 0x%08x loaded from offset 0x%08x.\n",
              aligned_code_end, oat_file_.Size(),
              code_size, code_size_offset);
          success = false;
          if (options_.disassemble_code_) {
            if (code_size_offset + kPrologueBytes <= oat_file_.Size()) {
              DumpCode(vios, oat_method, code_item, true, kPrologueBytes);
            }
          }
        } else if (code_size > kMaxCodeSize) {
          vios->Stream() << StringPrintf(
              "WARNING: "
              "code size %d is bigger than max expected threshold of %d. "
              "code size is 0x%08x loaded from offset 0x%08x.\n",
              code_size, kMaxCodeSize,
              code_size, code_size_offset);
          success = false;
          if (options_.disassemble_code_) {
            if (code_size_offset + kPrologueBytes <= oat_file_.Size()) {
              DumpCode(vios, oat_method, code_item, true, kPrologueBytes);
            }
          }
        } else if (options_.disassemble_code_) {
          DumpCode(vios, oat_method, code_item, !success, 0);
        }
      }
    }
    vios->Stream() << std::flush;
    return success;
  }

  void DumpSpillMask(std::ostream& os, uint32_t spill_mask, bool is_float) {
    if (spill_mask == 0) {
      return;
    }
    os << "(";
    for (size_t i = 0; i < 32; i++) {
      if ((spill_mask & (1 << i)) != 0) {
        if (is_float) {
          os << "fr" << i;
        } else {
          os << "r" << i;
        }
        spill_mask ^= 1 << i;  // clear bit
        if (spill_mask != 0) {
          os << ", ";
        } else {
          break;
        }
      }
    }
    os << ")";
  }

  // Display data stored at the the vmap offset of an oat method.
  void DumpVmapData(VariableIndentationOutputStream* vios,
                    const OatFile::OatMethod& oat_method,
                    const DexFile::CodeItem* code_item) {
    if (IsMethodGeneratedByOptimizingCompiler(oat_method, code_item)) {
      // The optimizing compiler outputs its CodeInfo data in the vmap table.
      const void* raw_code_info = oat_method.GetVmapTable();
      if (raw_code_info != nullptr) {
        CodeInfo code_info(raw_code_info);
        DCHECK(code_item != nullptr);
        ScopedIndentation indent1(vios);
        DumpCodeInfo(vios, code_info, oat_method, *code_item);
      }
    } else if (IsMethodGeneratedByDexToDexCompiler(oat_method, code_item)) {
      // We don't encode the size in the table, so just emit that we have quickened
      // information.
      ScopedIndentation indent(vios);
      vios->Stream() << "quickened data\n";
    } else {
      // Otherwise, there is nothing to display.
    }
  }

  // Display a CodeInfo object emitted by the optimizing compiler.
  void DumpCodeInfo(VariableIndentationOutputStream* vios,
                    const CodeInfo& code_info,
                    const OatFile::OatMethod& oat_method,
                    const DexFile::CodeItem& code_item) {
    code_info.Dump(vios,
                   oat_method.GetCodeOffset(),
                   code_item.registers_size_,
                   options_.dump_code_info_stack_maps_);
  }

  void DumpVregLocations(std::ostream& os, const OatFile::OatMethod& oat_method,
                         const DexFile::CodeItem* code_item) {
    if (code_item != nullptr) {
      size_t num_locals_ins = code_item->registers_size_;
      size_t num_ins = code_item->ins_size_;
      size_t num_locals = num_locals_ins - num_ins;
      size_t num_outs = code_item->outs_size_;

      os << "vr_stack_locations:";
      for (size_t reg = 0; reg <= num_locals_ins; reg++) {
        // For readability, delimit the different kinds of VRs.
        if (reg == num_locals_ins) {
          os << "\n\tmethod*:";
        } else if (reg == num_locals && num_ins > 0) {
          os << "\n\tins:";
        } else if (reg == 0 && num_locals > 0) {
          os << "\n\tlocals:";
        }

        uint32_t offset = StackVisitor::GetVRegOffsetFromQuickCode(
            code_item,
            oat_method.GetCoreSpillMask(),
            oat_method.GetFpSpillMask(),
            oat_method.GetFrameSizeInBytes(),
            reg,
            GetInstructionSet());
        os << " v" << reg << "[sp + #" << offset << "]";
      }

      for (size_t out_reg = 0; out_reg < num_outs; out_reg++) {
        if (out_reg == 0) {
          os << "\n\touts:";
        }

        uint32_t offset = StackVisitor::GetOutVROffset(out_reg, GetInstructionSet());
        os << " v" << out_reg << "[sp + #" << offset << "]";
      }

      os << "\n";
    }
  }

  void DumpDexCode(std::ostream& os, const DexFile& dex_file, const DexFile::CodeItem* code_item) {
    if (code_item != nullptr) {
      size_t i = 0;
      while (i < code_item->insns_size_in_code_units_) {
        const Instruction* instruction = Instruction::At(&code_item->insns_[i]);
        os << StringPrintf("0x%04zx: ", i) << instruction->DumpHexLE(5)
           << StringPrintf("\t| %s\n", instruction->DumpString(&dex_file).c_str());
        i += instruction->SizeInCodeUnits();
      }
    }
  }

  // Has `oat_method` -- corresponding to the Dex `code_item` -- been compiled by
  // the optimizing compiler?
  static bool IsMethodGeneratedByOptimizingCompiler(const OatFile::OatMethod& oat_method,
                                                    const DexFile::CodeItem* code_item) {
    // If the native GC map is null and the Dex `code_item` is not
    // null, then this method has been compiled with the optimizing
    // compiler.
    return oat_method.GetQuickCode() != nullptr &&
           oat_method.GetVmapTable() != nullptr &&
           code_item != nullptr;
  }

  // Has `oat_method` -- corresponding to the Dex `code_item` -- been compiled by
  // the dextodex compiler?
  static bool IsMethodGeneratedByDexToDexCompiler(const OatFile::OatMethod& oat_method,
                                                  const DexFile::CodeItem* code_item) {
    // If the quick code is null, the Dex `code_item` is not
    // null, and the vmap table is not null, then this method has been compiled
    // with the dextodex compiler.
    return oat_method.GetQuickCode() == nullptr &&
           oat_method.GetVmapTable() != nullptr &&
           code_item != nullptr;
  }

  verifier::MethodVerifier* DumpVerifier(VariableIndentationOutputStream* vios,
                                         StackHandleScope<1>* hs,
                                         uint32_t dex_method_idx,
                                         const DexFile* dex_file,
                                         const DexFile::ClassDef& class_def,
                                         const DexFile::CodeItem* code_item,
                                         uint32_t method_access_flags) {
    if ((method_access_flags & kAccNative) == 0) {
      ScopedObjectAccess soa(Thread::Current());
      Runtime* const runtime = Runtime::Current();
      Handle<mirror::DexCache> dex_cache(
          hs->NewHandle(runtime->GetClassLinker()->RegisterDexFile(*dex_file, nullptr)));
      DCHECK(options_.class_loader_ != nullptr);
      return verifier::MethodVerifier::VerifyMethodAndDump(
          soa.Self(), vios, dex_method_idx, dex_file, dex_cache, *options_.class_loader_,
          &class_def, code_item, nullptr, method_access_flags);
    }

    return nullptr;
  }

  // The StackMapsHelper provides the stack maps in the native PC order.
  // For identical native PCs, the order from the CodeInfo is preserved.
  class StackMapsHelper {
   public:
    explicit StackMapsHelper(const uint8_t* raw_code_info)
        : code_info_(raw_code_info),
          encoding_(code_info_.ExtractEncoding()),
          number_of_stack_maps_(code_info_.GetNumberOfStackMaps(encoding_)),
          indexes_(),
          offset_(static_cast<size_t>(-1)),
          stack_map_index_(0u) {
      if (number_of_stack_maps_ != 0u) {
        // Check if native PCs are ordered.
        bool ordered = true;
        StackMap last = code_info_.GetStackMapAt(0u, encoding_);
        for (size_t i = 1; i != number_of_stack_maps_; ++i) {
          StackMap current = code_info_.GetStackMapAt(i, encoding_);
          if (last.GetNativePcOffset(encoding_.stack_map_encoding) >
              current.GetNativePcOffset(encoding_.stack_map_encoding)) {
            ordered = false;
            break;
          }
          last = current;
        }
        if (!ordered) {
          // Create indirection indexes for access in native PC order. We do not optimize
          // for the fact that there can currently be only two separately ordered ranges,
          // namely normal stack maps and catch-point stack maps.
          indexes_.resize(number_of_stack_maps_);
          std::iota(indexes_.begin(), indexes_.end(), 0u);
          std::sort(indexes_.begin(),
                    indexes_.end(),
                    [this](size_t lhs, size_t rhs) {
                      StackMap left = code_info_.GetStackMapAt(lhs, encoding_);
                      uint32_t left_pc = left.GetNativePcOffset(encoding_.stack_map_encoding);
                      StackMap right = code_info_.GetStackMapAt(rhs, encoding_);
                      uint32_t right_pc = right.GetNativePcOffset(encoding_.stack_map_encoding);
                      // If the PCs are the same, compare indexes to preserve the original order.
                      return (left_pc < right_pc) || (left_pc == right_pc && lhs < rhs);
                    });
        }
        offset_ = GetStackMapAt(0).GetNativePcOffset(encoding_.stack_map_encoding);
      }
    }

    const CodeInfo& GetCodeInfo() const {
      return code_info_;
    }

    const CodeInfoEncoding& GetEncoding() const {
      return encoding_;
    }

    size_t GetOffset() const {
      return offset_;
    }

    StackMap GetStackMap() const {
      return GetStackMapAt(stack_map_index_);
    }

    void Next() {
      ++stack_map_index_;
      offset_ = (stack_map_index_ == number_of_stack_maps_)
          ? static_cast<size_t>(-1)
          : GetStackMapAt(stack_map_index_).GetNativePcOffset(encoding_.stack_map_encoding);
    }

   private:
    StackMap GetStackMapAt(size_t i) const {
      if (!indexes_.empty()) {
        i = indexes_[i];
      }
      DCHECK_LT(i, number_of_stack_maps_);
      return code_info_.GetStackMapAt(i, encoding_);
    }

    const CodeInfo code_info_;
    const CodeInfoEncoding encoding_;
    const size_t number_of_stack_maps_;
    dchecked_vector<size_t> indexes_;  // Used if stack map native PCs are not ordered.
    size_t offset_;
    size_t stack_map_index_;
  };

  void DumpCode(VariableIndentationOutputStream* vios,
                const OatFile::OatMethod& oat_method, const DexFile::CodeItem* code_item,
                bool bad_input, size_t code_size) {
    const void* quick_code = oat_method.GetQuickCode();

    if (code_size == 0) {
      code_size = oat_method.GetQuickCodeSize();
    }
    if (code_size == 0 || quick_code == nullptr) {
      vios->Stream() << "NO CODE!\n";
      return;
    } else if (!bad_input && IsMethodGeneratedByOptimizingCompiler(oat_method, code_item)) {
      // The optimizing compiler outputs its CodeInfo data in the vmap table.
      StackMapsHelper helper(oat_method.GetVmapTable());
      const uint8_t* quick_native_pc = reinterpret_cast<const uint8_t*>(quick_code);
      size_t offset = 0;
      while (offset < code_size) {
        offset += disassembler_->Dump(vios->Stream(), quick_native_pc + offset);
        if (offset == helper.GetOffset()) {
          ScopedIndentation indent1(vios);
          StackMap stack_map = helper.GetStackMap();
          DCHECK(stack_map.IsValid());
          stack_map.Dump(vios,
                         helper.GetCodeInfo(),
                         helper.GetEncoding(),
                         oat_method.GetCodeOffset(),
                         code_item->registers_size_);
          do {
            helper.Next();
            // There may be multiple stack maps at a given PC. We display only the first one.
          } while (offset == helper.GetOffset());
        }
        DCHECK_LT(offset, helper.GetOffset());
      }
    } else {
      const uint8_t* quick_native_pc = reinterpret_cast<const uint8_t*>(quick_code);
      size_t offset = 0;
      while (offset < code_size) {
        offset += disassembler_->Dump(vios->Stream(), quick_native_pc + offset);
      }
    }
  }

  const OatFile& oat_file_;
  const std::vector<const OatFile::OatDexFile*> oat_dex_files_;
  const OatDumperOptions& options_;
  uint32_t resolved_addr2instr_;
  InstructionSet instruction_set_;
  std::set<uintptr_t> offsets_;
  Disassembler* disassembler_;
};

class ImageDumper {
 public:
  ImageDumper(std::ostream* os,
              gc::space::ImageSpace& image_space,
              const ImageHeader& image_header,
              OatDumperOptions* oat_dumper_options)
      : os_(os),
        vios_(os),
        indent1_(&vios_),
        image_space_(image_space),
        image_header_(image_header),
        oat_dumper_options_(oat_dumper_options) {}

  bool Dump() SHARED_REQUIRES(Locks::mutator_lock_) {
    std::ostream& os = *os_;
    std::ostream& indent_os = vios_.Stream();

    os << "MAGIC: " << image_header_.GetMagic() << "\n\n";

    os << "IMAGE LOCATION: " << image_space_.GetImageLocation() << "\n\n";

    os << "IMAGE BEGIN: " << reinterpret_cast<void*>(image_header_.GetImageBegin()) << "\n\n";

    os << "IMAGE SIZE: " << image_header_.GetImageSize() << "\n\n";

    for (size_t i = 0; i < ImageHeader::kSectionCount; ++i) {
      auto section = static_cast<ImageHeader::ImageSections>(i);
      os << "IMAGE SECTION " << section << ": " << image_header_.GetImageSection(section) << "\n\n";
    }

    os << "OAT CHECKSUM: " << StringPrintf("0x%08x\n\n", image_header_.GetOatChecksum());

    os << "OAT FILE BEGIN:" << reinterpret_cast<void*>(image_header_.GetOatFileBegin()) << "\n\n";

    os << "OAT DATA BEGIN:" << reinterpret_cast<void*>(image_header_.GetOatDataBegin()) << "\n\n";

    os << "OAT DATA END:" << reinterpret_cast<void*>(image_header_.GetOatDataEnd()) << "\n\n";

    os << "OAT FILE END:" << reinterpret_cast<void*>(image_header_.GetOatFileEnd()) << "\n\n";

    os << "PATCH DELTA:" << image_header_.GetPatchDelta() << "\n\n";

    os << "COMPILE PIC: " << (image_header_.CompilePic() ? "yes" : "no") << "\n\n";

    {
      os << "ROOTS: " << reinterpret_cast<void*>(image_header_.GetImageRoots()) << "\n";
      static_assert(arraysize(image_roots_descriptions_) ==
          static_cast<size_t>(ImageHeader::kImageRootsMax), "sizes must match");
      for (int i = 0; i < ImageHeader::kImageRootsMax; i++) {
        ImageHeader::ImageRoot image_root = static_cast<ImageHeader::ImageRoot>(i);
        const char* image_root_description = image_roots_descriptions_[i];
        mirror::Object* image_root_object = image_header_.GetImageRoot(image_root);
        indent_os << StringPrintf("%s: %p\n", image_root_description, image_root_object);
        if (image_root_object->IsObjectArray()) {
          mirror::ObjectArray<mirror::Object>* image_root_object_array
              = image_root_object->AsObjectArray<mirror::Object>();
          ScopedIndentation indent2(&vios_);
          for (int j = 0; j < image_root_object_array->GetLength(); j++) {
            mirror::Object* value = image_root_object_array->Get(j);
            size_t run = 0;
            for (int32_t k = j + 1; k < image_root_object_array->GetLength(); k++) {
              if (value == image_root_object_array->Get(k)) {
                run++;
              } else {
                break;
              }
            }
            if (run == 0) {
              indent_os << StringPrintf("%d: ", j);
            } else {
              indent_os << StringPrintf("%d to %zd: ", j, j + run);
              j = j + run;
            }
            if (value != nullptr) {
              PrettyObjectValue(indent_os, value->GetClass(), value);
            } else {
              indent_os << j << ": null\n";
            }
          }
        }
      }
    }

    {
      os << "METHOD ROOTS\n";
      static_assert(arraysize(image_methods_descriptions_) ==
          static_cast<size_t>(ImageHeader::kImageMethodsCount), "sizes must match");
      for (int i = 0; i < ImageHeader::kImageMethodsCount; i++) {
        auto image_root = static_cast<ImageHeader::ImageMethod>(i);
        const char* description = image_methods_descriptions_[i];
        auto* image_method = image_header_.GetImageMethod(image_root);
        indent_os << StringPrintf("%s: %p\n", description, image_method);
      }
    }
    os << "\n";

    Runtime* const runtime = Runtime::Current();
    ClassLinker* class_linker = runtime->GetClassLinker();
    std::string image_filename = image_space_.GetImageFilename();
    std::string oat_location = ImageHeader::GetOatLocationFromImageLocation(image_filename);
    os << "OAT LOCATION: " << oat_location;
    os << "\n";
    std::string error_msg;
    const OatFile* oat_file = image_space_.GetOatFile();
    if (oat_file == nullptr) {
      oat_file = runtime->GetOatFileManager().FindOpenedOatFileFromOatLocation(oat_location);
    }
    if (oat_file == nullptr) {
      oat_file = OatFile::Open(oat_location,
                               oat_location,
                               nullptr,
                               nullptr,
                               false,
                               /*low_4gb*/false,
                               nullptr,
                               &error_msg);
    }
    if (oat_file == nullptr) {
      os << "OAT FILE NOT FOUND: " << error_msg << "\n";
      return EXIT_FAILURE;
    }
    os << "\n";

    stats_.oat_file_bytes = oat_file->Size();

    oat_dumper_.reset(new OatDumper(*oat_file, *oat_dumper_options_));

    for (const OatFile::OatDexFile* oat_dex_file : oat_file->GetOatDexFiles()) {
      CHECK(oat_dex_file != nullptr);
      stats_.oat_dex_file_sizes.push_back(std::make_pair(oat_dex_file->GetDexFileLocation(),
                                                         oat_dex_file->FileSize()));
    }

    os << "OBJECTS:\n" << std::flush;

    // Loop through the image space and dump its objects.
    gc::Heap* heap = runtime->GetHeap();
    Thread* self = Thread::Current();
    {
      {
        WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
        heap->FlushAllocStack();
      }
      // Since FlushAllocStack() above resets the (active) allocation
      // stack. Need to revoke the thread-local allocation stacks that
      // point into it.
      ScopedThreadSuspension sts(self, kNative);
      ScopedSuspendAll ssa(__FUNCTION__);
      heap->RevokeAllThreadLocalAllocationStacks(self);
    }
    {
      // Mark dex caches.
      dex_caches_.clear();
      {
        ReaderMutexLock mu(self, *class_linker->DexLock());
        for (const ClassLinker::DexCacheData& data : class_linker->GetDexCachesData()) {
          mirror::DexCache* dex_cache =
              down_cast<mirror::DexCache*>(self->DecodeJObject(data.weak_root));
          if (dex_cache != nullptr) {
            dex_caches_.insert(dex_cache);
          }
        }
      }
      ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
      // Dump the normal objects before ArtMethods.
      image_space_.GetLiveBitmap()->Walk(ImageDumper::Callback, this);
      indent_os << "\n";
      // TODO: Dump fields.
      // Dump methods after.
      DumpArtMethodVisitor visitor(this);
      image_header_.VisitPackedArtMethods(&visitor,
                                          image_space_.Begin(),
                                          image_header_.GetPointerSize());
      // Dump the large objects separately.
      heap->GetLargeObjectsSpace()->GetLiveBitmap()->Walk(ImageDumper::Callback, this);
      indent_os << "\n";
    }
    os << "STATS:\n" << std::flush;
    std::unique_ptr<File> file(OS::OpenFileForReading(image_filename.c_str()));
    size_t data_size = image_header_.GetDataSize();  // stored size in file.
    if (file == nullptr) {
      LOG(WARNING) << "Failed to find image in " << image_filename;
    } else {
      stats_.file_bytes = file->GetLength();
      // If the image is compressed, adjust to decompressed size.
      size_t uncompressed_size = image_header_.GetImageSize() - sizeof(ImageHeader);
      if (image_header_.GetStorageMode() == ImageHeader::kStorageModeUncompressed) {
        DCHECK_EQ(uncompressed_size, data_size) << "Sizes should match for uncompressed image";
      }
      stats_.file_bytes += uncompressed_size - data_size;
    }
    size_t header_bytes = sizeof(ImageHeader);
    const auto& object_section = image_header_.GetImageSection(ImageHeader::kSectionObjects);
    const auto& field_section = image_header_.GetImageSection(ImageHeader::kSectionArtFields);
    const auto& method_section = image_header_.GetMethodsSection();
    const auto& dex_cache_arrays_section = image_header_.GetImageSection(
        ImageHeader::kSectionDexCacheArrays);
    const auto& intern_section = image_header_.GetImageSection(
        ImageHeader::kSectionInternedStrings);
    const auto& class_table_section = image_header_.GetImageSection(
        ImageHeader::kSectionClassTable);
    const auto& bitmap_section = image_header_.GetImageSection(ImageHeader::kSectionImageBitmap);

    stats_.header_bytes = header_bytes;

    // Objects are kObjectAlignment-aligned.
    // CHECK_EQ(RoundUp(header_bytes, kObjectAlignment), object_section.Offset());
    if (object_section.Offset() > header_bytes) {
      stats_.alignment_bytes += object_section.Offset() - header_bytes;
    }

    // Field section is 4-byte aligned.
    constexpr size_t kFieldSectionAlignment = 4U;
    uint32_t end_objects = object_section.Offset() + object_section.Size();
    CHECK_EQ(RoundUp(end_objects, kFieldSectionAlignment), field_section.Offset());
    stats_.alignment_bytes += field_section.Offset() - end_objects;

    // Method section is 4/8 byte aligned depending on target. Just check for 4-byte alignment.
    uint32_t end_fields = field_section.Offset() + field_section.Size();
    CHECK_ALIGNED(method_section.Offset(), 4);
    stats_.alignment_bytes += method_section.Offset() - end_fields;

    // Dex cache arrays section is aligned depending on the target. Just check for 4-byte alignment.
    uint32_t end_methods = method_section.Offset() + method_section.Size();
    CHECK_ALIGNED(dex_cache_arrays_section.Offset(), 4);
    stats_.alignment_bytes += dex_cache_arrays_section.Offset() - end_methods;

    // Intern table is 8-byte aligned.
    uint32_t end_caches = dex_cache_arrays_section.Offset() + dex_cache_arrays_section.Size();
    CHECK_EQ(RoundUp(end_caches, 8U), intern_section.Offset());
    stats_.alignment_bytes += intern_section.Offset() - end_caches;

    // Add space between intern table and class table.
    uint32_t end_intern = intern_section.Offset() + intern_section.Size();
    stats_.alignment_bytes += class_table_section.Offset() - end_intern;

    // Add space between end of image data and bitmap. Expect the bitmap to be page-aligned.
    const size_t bitmap_offset = sizeof(ImageHeader) + data_size;
    CHECK_ALIGNED(bitmap_section.Offset(), kPageSize);
    stats_.alignment_bytes += RoundUp(bitmap_offset, kPageSize) - bitmap_offset;

    stats_.bitmap_bytes += bitmap_section.Size();
    stats_.art_field_bytes += field_section.Size();
    stats_.art_method_bytes += method_section.Size();
    stats_.dex_cache_arrays_bytes += dex_cache_arrays_section.Size();
    stats_.interned_strings_bytes += intern_section.Size();
    stats_.class_table_bytes += class_table_section.Size();
    stats_.Dump(os, indent_os);
    os << "\n";

    os << std::flush;

    return oat_dumper_->Dump(os);
  }

 private:
  class DumpArtMethodVisitor : public ArtMethodVisitor {
   public:
    explicit DumpArtMethodVisitor(ImageDumper* image_dumper) : image_dumper_(image_dumper) {}

    virtual void Visit(ArtMethod* method) OVERRIDE SHARED_REQUIRES(Locks::mutator_lock_) {
      std::ostream& indent_os = image_dumper_->vios_.Stream();
      indent_os << method << " " << " ArtMethod: " << PrettyMethod(method) << "\n";
      image_dumper_->DumpMethod(method, indent_os);
      indent_os << "\n";
    }

   private:
    ImageDumper* const image_dumper_;
  };

  static void PrettyObjectValue(std::ostream& os, mirror::Class* type, mirror::Object* value)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    CHECK(type != nullptr);
    if (value == nullptr) {
      os << StringPrintf("null   %s\n", PrettyDescriptor(type).c_str());
    } else if (type->IsStringClass()) {
      mirror::String* string = value->AsString();
      os << StringPrintf("%p   String: %s\n", string,
                         PrintableString(string->ToModifiedUtf8().c_str()).c_str());
    } else if (type->IsClassClass()) {
      mirror::Class* klass = value->AsClass();
      os << StringPrintf("%p   Class: %s\n", klass, PrettyDescriptor(klass).c_str());
    } else {
      os << StringPrintf("%p   %s\n", value, PrettyDescriptor(type).c_str());
    }
  }

  static void PrintField(std::ostream& os, ArtField* field, mirror::Object* obj)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    os << StringPrintf("%s: ", field->GetName());
    switch (field->GetTypeAsPrimitiveType()) {
      case Primitive::kPrimLong:
        os << StringPrintf("%" PRId64 " (0x%" PRIx64 ")\n", field->Get64(obj), field->Get64(obj));
        break;
      case Primitive::kPrimDouble:
        os << StringPrintf("%f (%a)\n", field->GetDouble(obj), field->GetDouble(obj));
        break;
      case Primitive::kPrimFloat:
        os << StringPrintf("%f (%a)\n", field->GetFloat(obj), field->GetFloat(obj));
        break;
      case Primitive::kPrimInt:
        os << StringPrintf("%d (0x%x)\n", field->Get32(obj), field->Get32(obj));
        break;
      case Primitive::kPrimChar:
        os << StringPrintf("%u (0x%x)\n", field->GetChar(obj), field->GetChar(obj));
        break;
      case Primitive::kPrimShort:
        os << StringPrintf("%d (0x%x)\n", field->GetShort(obj), field->GetShort(obj));
        break;
      case Primitive::kPrimBoolean:
        os << StringPrintf("%s (0x%x)\n", field->GetBoolean(obj)? "true" : "false",
            field->GetBoolean(obj));
        break;
      case Primitive::kPrimByte:
        os << StringPrintf("%d (0x%x)\n", field->GetByte(obj), field->GetByte(obj));
        break;
      case Primitive::kPrimNot: {
        // Get the value, don't compute the type unless it is non-null as we don't want
        // to cause class loading.
        mirror::Object* value = field->GetObj(obj);
        if (value == nullptr) {
          os << StringPrintf("null   %s\n", PrettyDescriptor(field->GetTypeDescriptor()).c_str());
        } else {
          // Grab the field type without causing resolution.
          mirror::Class* field_type = field->GetType<false>();
          if (field_type != nullptr) {
            PrettyObjectValue(os, field_type, value);
          } else {
            os << StringPrintf("%p   %s\n", value,
                               PrettyDescriptor(field->GetTypeDescriptor()).c_str());
          }
        }
        break;
      }
      default:
        os << "unexpected field type: " << field->GetTypeDescriptor() << "\n";
        break;
    }
  }

  static void DumpFields(std::ostream& os, mirror::Object* obj, mirror::Class* klass)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    mirror::Class* super = klass->GetSuperClass();
    if (super != nullptr) {
      DumpFields(os, obj, super);
    }
    for (ArtField& field : klass->GetIFields()) {
      PrintField(os, &field, obj);
    }
  }

  bool InDumpSpace(const mirror::Object* object) {
    return image_space_.Contains(object);
  }

  const void* GetQuickOatCodeBegin(ArtMethod* m) SHARED_REQUIRES(Locks::mutator_lock_) {
    const void* quick_code = m->GetEntryPointFromQuickCompiledCodePtrSize(
        image_header_.GetPointerSize());
    if (Runtime::Current()->GetClassLinker()->IsQuickResolutionStub(quick_code)) {
      quick_code = oat_dumper_->GetQuickOatCode(m);
    }
    if (oat_dumper_->GetInstructionSet() == kThumb2) {
      quick_code = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(quick_code) & ~0x1);
    }
    return quick_code;
  }

  uint32_t GetQuickOatCodeSize(ArtMethod* m)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    const uint32_t* oat_code_begin = reinterpret_cast<const uint32_t*>(GetQuickOatCodeBegin(m));
    if (oat_code_begin == nullptr) {
      return 0;
    }
    return oat_code_begin[-1];
  }

  const void* GetQuickOatCodeEnd(ArtMethod* m)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    const uint8_t* oat_code_begin = reinterpret_cast<const uint8_t*>(GetQuickOatCodeBegin(m));
    if (oat_code_begin == nullptr) {
      return nullptr;
    }
    return oat_code_begin + GetQuickOatCodeSize(m);
  }

  static void Callback(mirror::Object* obj, void* arg) SHARED_REQUIRES(Locks::mutator_lock_) {
    DCHECK(obj != nullptr);
    DCHECK(arg != nullptr);
    ImageDumper* state = reinterpret_cast<ImageDumper*>(arg);
    if (!state->InDumpSpace(obj)) {
      return;
    }

    size_t object_bytes = obj->SizeOf();
    size_t alignment_bytes = RoundUp(object_bytes, kObjectAlignment) - object_bytes;
    state->stats_.object_bytes += object_bytes;
    state->stats_.alignment_bytes += alignment_bytes;

    std::ostream& os = state->vios_.Stream();

    mirror::Class* obj_class = obj->GetClass();
    if (obj_class->IsArrayClass()) {
      os << StringPrintf("%p: %s length:%d\n", obj, PrettyDescriptor(obj_class).c_str(),
                         obj->AsArray()->GetLength());
    } else if (obj->IsClass()) {
      mirror::Class* klass = obj->AsClass();
      os << StringPrintf("%p: java.lang.Class \"%s\" (", obj, PrettyDescriptor(klass).c_str())
         << klass->GetStatus() << ")\n";
    } else if (obj_class->IsStringClass()) {
      os << StringPrintf("%p: java.lang.String %s\n", obj,
                         PrintableString(obj->AsString()->ToModifiedUtf8().c_str()).c_str());
    } else {
      os << StringPrintf("%p: %s\n", obj, PrettyDescriptor(obj_class).c_str());
    }
    ScopedIndentation indent1(&state->vios_);
    DumpFields(os, obj, obj_class);
    const size_t image_pointer_size = state->image_header_.GetPointerSize();
    if (obj->IsObjectArray()) {
      auto* obj_array = obj->AsObjectArray<mirror::Object>();
      for (int32_t i = 0, length = obj_array->GetLength(); i < length; i++) {
        mirror::Object* value = obj_array->Get(i);
        size_t run = 0;
        for (int32_t j = i + 1; j < length; j++) {
          if (value == obj_array->Get(j)) {
            run++;
          } else {
            break;
          }
        }
        if (run == 0) {
          os << StringPrintf("%d: ", i);
        } else {
          os << StringPrintf("%d to %zd: ", i, i + run);
          i = i + run;
        }
        mirror::Class* value_class =
            (value == nullptr) ? obj_class->GetComponentType() : value->GetClass();
        PrettyObjectValue(os, value_class, value);
      }
    } else if (obj->IsClass()) {
      mirror::Class* klass = obj->AsClass();
      if (klass->NumStaticFields() != 0) {
        os << "STATICS:\n";
        ScopedIndentation indent2(&state->vios_);
        for (ArtField& field : klass->GetSFields()) {
          PrintField(os, &field, field.GetDeclaringClass());
        }
      }
    } else {
      auto it = state->dex_caches_.find(obj);
      if (it != state->dex_caches_.end()) {
        auto* dex_cache = down_cast<mirror::DexCache*>(obj);
        const auto& field_section = state->image_header_.GetImageSection(
            ImageHeader::kSectionArtFields);
        const auto& method_section = state->image_header_.GetMethodsSection();
        size_t num_methods = dex_cache->NumResolvedMethods();
        if (num_methods != 0u) {
          os << "Methods (size=" << num_methods << "):";
          ScopedIndentation indent2(&state->vios_);
          auto* resolved_methods = dex_cache->GetResolvedMethods();
          for (size_t i = 0, length = dex_cache->NumResolvedMethods(); i < length; ++i) {
            auto* elem = mirror::DexCache::GetElementPtrSize(resolved_methods,
                                                             i,
                                                             image_pointer_size);
            size_t run = 0;
            for (size_t j = i + 1;
                j != length && elem == mirror::DexCache::GetElementPtrSize(resolved_methods,
                                                                           j,
                                                                           image_pointer_size);
                ++j, ++run) {}
            if (run == 0) {
              os << StringPrintf("%zd: ", i);
            } else {
              os << StringPrintf("%zd to %zd: ", i, i + run);
              i = i + run;
            }
            std::string msg;
            if (elem == nullptr) {
              msg = "null";
            } else if (method_section.Contains(
                reinterpret_cast<uint8_t*>(elem) - state->image_space_.Begin())) {
              msg = PrettyMethod(reinterpret_cast<ArtMethod*>(elem));
            } else {
              msg = "<not in method section>";
            }
            os << StringPrintf("%p   %s\n", elem, msg.c_str());
          }
        }
        size_t num_fields = dex_cache->NumResolvedFields();
        if (num_fields != 0u) {
          os << "Fields (size=" << num_fields << "):";
          ScopedIndentation indent2(&state->vios_);
          auto* resolved_fields = dex_cache->GetResolvedFields();
          for (size_t i = 0, length = dex_cache->NumResolvedFields(); i < length; ++i) {
            auto* elem = mirror::DexCache::GetElementPtrSize(resolved_fields, i, image_pointer_size);
            size_t run = 0;
            for (size_t j = i + 1;
                j != length && elem == mirror::DexCache::GetElementPtrSize(resolved_fields,
                                                                           j,
                                                                           image_pointer_size);
                ++j, ++run) {}
            if (run == 0) {
              os << StringPrintf("%zd: ", i);
            } else {
              os << StringPrintf("%zd to %zd: ", i, i + run);
              i = i + run;
            }
            std::string msg;
            if (elem == nullptr) {
              msg = "null";
            } else if (field_section.Contains(
                reinterpret_cast<uint8_t*>(elem) - state->image_space_.Begin())) {
              msg = PrettyField(reinterpret_cast<ArtField*>(elem));
            } else {
              msg = "<not in field section>";
            }
            os << StringPrintf("%p   %s\n", elem, msg.c_str());
          }
        }
      }
    }
    std::string temp;
    state->stats_.Update(obj_class->GetDescriptor(&temp), object_bytes);
  }

  void DumpMethod(ArtMethod* method, std::ostream& indent_os)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    DCHECK(method != nullptr);
    const void* quick_oat_code_begin = GetQuickOatCodeBegin(method);
    const void* quick_oat_code_end = GetQuickOatCodeEnd(method);
    const size_t pointer_size = image_header_.GetPointerSize();
    OatQuickMethodHeader* method_header = reinterpret_cast<OatQuickMethodHeader*>(
        reinterpret_cast<uintptr_t>(quick_oat_code_begin) - sizeof(OatQuickMethodHeader));
    if (method->IsNative()) {
      bool first_occurrence;
      uint32_t quick_oat_code_size = GetQuickOatCodeSize(method);
      ComputeOatSize(quick_oat_code_begin, &first_occurrence);
      if (first_occurrence) {
        stats_.native_to_managed_code_bytes += quick_oat_code_size;
      }
      if (quick_oat_code_begin != method->GetEntryPointFromQuickCompiledCodePtrSize(
          image_header_.GetPointerSize())) {
        indent_os << StringPrintf("OAT CODE: %p\n", quick_oat_code_begin);
      }
    } else if (method->IsAbstract() || method->IsClassInitializer()) {
      // Don't print information for these.
    } else if (method->IsRuntimeMethod()) {
      ImtConflictTable* table = method->GetImtConflictTable(image_header_.GetPointerSize());
      if (table != nullptr) {
        indent_os << "IMT conflict table " << table << " method: ";
        for (size_t i = 0, count = table->NumEntries(pointer_size); i < count; ++i) {
          indent_os << PrettyMethod(table->GetImplementationMethod(i, pointer_size)) << " ";
        }
      }
    } else {
      const DexFile::CodeItem* code_item = method->GetCodeItem();
      size_t dex_instruction_bytes = code_item->insns_size_in_code_units_ * 2;
      stats_.dex_instruction_bytes += dex_instruction_bytes;

      bool first_occurrence;
      size_t vmap_table_bytes = 0u;
      if (!method_header->IsOptimized()) {
        // Method compiled with the optimizing compiler have no vmap table.
        vmap_table_bytes = ComputeOatSize(method_header->GetVmapTable(), &first_occurrence);
        if (first_occurrence) {
          stats_.vmap_table_bytes += vmap_table_bytes;
        }
      }

      uint32_t quick_oat_code_size = GetQuickOatCodeSize(method);
      ComputeOatSize(quick_oat_code_begin, &first_occurrence);
      if (first_occurrence) {
        stats_.managed_code_bytes += quick_oat_code_size;
        if (method->IsConstructor()) {
          if (method->IsStatic()) {
            stats_.class_initializer_code_bytes += quick_oat_code_size;
          } else if (dex_instruction_bytes > kLargeConstructorDexBytes) {
            stats_.large_initializer_code_bytes += quick_oat_code_size;
          }
        } else if (dex_instruction_bytes > kLargeMethodDexBytes) {
          stats_.large_method_code_bytes += quick_oat_code_size;
        }
      }
      stats_.managed_code_bytes_ignoring_deduplication += quick_oat_code_size;

      uint32_t method_access_flags = method->GetAccessFlags();

      indent_os << StringPrintf("OAT CODE: %p-%p\n", quick_oat_code_begin, quick_oat_code_end);
      indent_os << StringPrintf("SIZE: Dex Instructions=%zd StackMaps=%zd AccessFlags=0x%x\n",
                                dex_instruction_bytes,
                                vmap_table_bytes,
                                method_access_flags);

      size_t total_size = dex_instruction_bytes +
          vmap_table_bytes + quick_oat_code_size + ArtMethod::Size(image_header_.GetPointerSize());

      double expansion =
      static_cast<double>(quick_oat_code_size) / static_cast<double>(dex_instruction_bytes);
      stats_.ComputeOutliers(total_size, expansion, method);
    }
  }

  std::set<const void*> already_seen_;
  // Compute the size of the given data within the oat file and whether this is the first time
  // this data has been requested
  size_t ComputeOatSize(const void* oat_data, bool* first_occurrence) {
    if (already_seen_.count(oat_data) == 0) {
      *first_occurrence = true;
      already_seen_.insert(oat_data);
    } else {
      *first_occurrence = false;
    }
    return oat_dumper_->ComputeSize(oat_data);
  }

 public:
  struct Stats {
    size_t oat_file_bytes;
    size_t file_bytes;

    size_t header_bytes;
    size_t object_bytes;
    size_t art_field_bytes;
    size_t art_method_bytes;
    size_t dex_cache_arrays_bytes;
    size_t interned_strings_bytes;
    size_t class_table_bytes;
    size_t bitmap_bytes;
    size_t alignment_bytes;

    size_t managed_code_bytes;
    size_t managed_code_bytes_ignoring_deduplication;
    size_t managed_to_native_code_bytes;
    size_t native_to_managed_code_bytes;
    size_t class_initializer_code_bytes;
    size_t large_initializer_code_bytes;
    size_t large_method_code_bytes;

    size_t vmap_table_bytes;

    size_t dex_instruction_bytes;

    std::vector<ArtMethod*> method_outlier;
    std::vector<size_t> method_outlier_size;
    std::vector<double> method_outlier_expansion;
    std::vector<std::pair<std::string, size_t>> oat_dex_file_sizes;

    Stats()
        : oat_file_bytes(0),
          file_bytes(0),
          header_bytes(0),
          object_bytes(0),
          art_field_bytes(0),
          art_method_bytes(0),
          dex_cache_arrays_bytes(0),
          interned_strings_bytes(0),
          class_table_bytes(0),
          bitmap_bytes(0),
          alignment_bytes(0),
          managed_code_bytes(0),
          managed_code_bytes_ignoring_deduplication(0),
          managed_to_native_code_bytes(0),
          native_to_managed_code_bytes(0),
          class_initializer_code_bytes(0),
          large_initializer_code_bytes(0),
          large_method_code_bytes(0),
          vmap_table_bytes(0),
          dex_instruction_bytes(0) {}

    struct SizeAndCount {
      SizeAndCount(size_t bytes_in, size_t count_in) : bytes(bytes_in), count(count_in) {}
      size_t bytes;
      size_t count;
    };
    typedef SafeMap<std::string, SizeAndCount> SizeAndCountTable;
    SizeAndCountTable sizes_and_counts;

    void Update(const char* descriptor, size_t object_bytes_in) {
      SizeAndCountTable::iterator it = sizes_and_counts.find(descriptor);
      if (it != sizes_and_counts.end()) {
        it->second.bytes += object_bytes_in;
        it->second.count += 1;
      } else {
        sizes_and_counts.Put(descriptor, SizeAndCount(object_bytes_in, 1));
      }
    }

    double PercentOfOatBytes(size_t size) {
      return (static_cast<double>(size) / static_cast<double>(oat_file_bytes)) * 100;
    }

    double PercentOfFileBytes(size_t size) {
      return (static_cast<double>(size) / static_cast<double>(file_bytes)) * 100;
    }

    double PercentOfObjectBytes(size_t size) {
      return (static_cast<double>(size) / static_cast<double>(object_bytes)) * 100;
    }

    void ComputeOutliers(size_t total_size, double expansion, ArtMethod* method) {
      method_outlier_size.push_back(total_size);
      method_outlier_expansion.push_back(expansion);
      method_outlier.push_back(method);
    }

    void DumpOutliers(std::ostream& os)
        SHARED_REQUIRES(Locks::mutator_lock_) {
      size_t sum_of_sizes = 0;
      size_t sum_of_sizes_squared = 0;
      size_t sum_of_expansion = 0;
      size_t sum_of_expansion_squared = 0;
      size_t n = method_outlier_size.size();
      if (n == 0) {
        return;
      }
      for (size_t i = 0; i < n; i++) {
        size_t cur_size = method_outlier_size[i];
        sum_of_sizes += cur_size;
        sum_of_sizes_squared += cur_size * cur_size;
        double cur_expansion = method_outlier_expansion[i];
        sum_of_expansion += cur_expansion;
        sum_of_expansion_squared += cur_expansion * cur_expansion;
      }
      size_t size_mean = sum_of_sizes / n;
      size_t size_variance = (sum_of_sizes_squared - sum_of_sizes * size_mean) / (n - 1);
      double expansion_mean = sum_of_expansion / n;
      double expansion_variance =
          (sum_of_expansion_squared - sum_of_expansion * expansion_mean) / (n - 1);

      // Dump methods whose size is a certain number of standard deviations from the mean
      size_t dumped_values = 0;
      size_t skipped_values = 0;
      for (size_t i = 100; i > 0; i--) {  // i is the current number of standard deviations
        size_t cur_size_variance = i * i * size_variance;
        bool first = true;
        for (size_t j = 0; j < n; j++) {
          size_t cur_size = method_outlier_size[j];
          if (cur_size > size_mean) {
            size_t cur_var = cur_size - size_mean;
            cur_var = cur_var * cur_var;
            if (cur_var > cur_size_variance) {
              if (dumped_values > 20) {
                if (i == 1) {
                  skipped_values++;
                } else {
                  i = 2;  // jump to counting for 1 standard deviation
                  break;
                }
              } else {
                if (first) {
                  os << "\nBig methods (size > " << i << " standard deviations the norm):\n";
                  first = false;
                }
                os << PrettyMethod(method_outlier[j]) << " requires storage of "
                    << PrettySize(cur_size) << "\n";
                method_outlier_size[j] = 0;  // don't consider this method again
                dumped_values++;
              }
            }
          }
        }
      }
      if (skipped_values > 0) {
        os << "... skipped " << skipped_values
           << " methods with size > 1 standard deviation from the norm\n";
      }
      os << std::flush;

      // Dump methods whose expansion is a certain number of standard deviations from the mean
      dumped_values = 0;
      skipped_values = 0;
      for (size_t i = 10; i > 0; i--) {  // i is the current number of standard deviations
        double cur_expansion_variance = i * i * expansion_variance;
        bool first = true;
        for (size_t j = 0; j < n; j++) {
          double cur_expansion = method_outlier_expansion[j];
          if (cur_expansion > expansion_mean) {
            size_t cur_var = cur_expansion - expansion_mean;
            cur_var = cur_var * cur_var;
            if (cur_var > cur_expansion_variance) {
              if (dumped_values > 20) {
                if (i == 1) {
                  skipped_values++;
                } else {
                  i = 2;  // jump to counting for 1 standard deviation
                  break;
                }
              } else {
                if (first) {
                  os << "\nLarge expansion methods (size > " << i
                      << " standard deviations the norm):\n";
                  first = false;
                }
                os << PrettyMethod(method_outlier[j]) << " expanded code by "
                   << cur_expansion << "\n";
                method_outlier_expansion[j] = 0.0;  // don't consider this method again
                dumped_values++;
              }
            }
          }
        }
      }
      if (skipped_values > 0) {
        os << "... skipped " << skipped_values
           << " methods with expansion > 1 standard deviation from the norm\n";
      }
      os << "\n" << std::flush;
    }

    void Dump(std::ostream& os, std::ostream& indent_os)
        SHARED_REQUIRES(Locks::mutator_lock_) {
      {
        os << "art_file_bytes = " << PrettySize(file_bytes) << "\n\n"
           << "art_file_bytes = header_bytes + object_bytes + alignment_bytes\n";
        indent_os << StringPrintf("header_bytes           =  %8zd (%2.0f%% of art file bytes)\n"
                                  "object_bytes           =  %8zd (%2.0f%% of art file bytes)\n"
                                  "art_field_bytes        =  %8zd (%2.0f%% of art file bytes)\n"
                                  "art_method_bytes       =  %8zd (%2.0f%% of art file bytes)\n"
                                  "dex_cache_arrays_bytes =  %8zd (%2.0f%% of art file bytes)\n"
                                  "interned_string_bytes  =  %8zd (%2.0f%% of art file bytes)\n"
                                  "class_table_bytes      =  %8zd (%2.0f%% of art file bytes)\n"
                                  "bitmap_bytes           =  %8zd (%2.0f%% of art file bytes)\n"
                                  "alignment_bytes        =  %8zd (%2.0f%% of art file bytes)\n\n",
                                  header_bytes, PercentOfFileBytes(header_bytes),
                                  object_bytes, PercentOfFileBytes(object_bytes),
                                  art_field_bytes, PercentOfFileBytes(art_field_bytes),
                                  art_method_bytes, PercentOfFileBytes(art_method_bytes),
                                  dex_cache_arrays_bytes,
                                  PercentOfFileBytes(dex_cache_arrays_bytes),
                                  interned_strings_bytes,
                                  PercentOfFileBytes(interned_strings_bytes),
                                  class_table_bytes, PercentOfFileBytes(class_table_bytes),
                                  bitmap_bytes, PercentOfFileBytes(bitmap_bytes),
                                  alignment_bytes, PercentOfFileBytes(alignment_bytes))
            << std::flush;
        CHECK_EQ(file_bytes,
                 header_bytes + object_bytes + art_field_bytes + art_method_bytes +
                 dex_cache_arrays_bytes + interned_strings_bytes + class_table_bytes +
                 bitmap_bytes + alignment_bytes);
      }

      os << "object_bytes breakdown:\n";
      size_t object_bytes_total = 0;
      for (const auto& sizes_and_count : sizes_and_counts) {
        const std::string& descriptor(sizes_and_count.first);
        double average = static_cast<double>(sizes_and_count.second.bytes) /
            static_cast<double>(sizes_and_count.second.count);
        double percent = PercentOfObjectBytes(sizes_and_count.second.bytes);
        os << StringPrintf("%32s %8zd bytes %6zd instances "
                           "(%4.0f bytes/instance) %2.0f%% of object_bytes\n",
                           descriptor.c_str(), sizes_and_count.second.bytes,
                           sizes_and_count.second.count, average, percent);
        object_bytes_total += sizes_and_count.second.bytes;
      }
      os << "\n" << std::flush;
      CHECK_EQ(object_bytes, object_bytes_total);

      os << StringPrintf("oat_file_bytes               = %8zd\n"
                         "managed_code_bytes           = %8zd (%2.0f%% of oat file bytes)\n"
                         "managed_to_native_code_bytes = %8zd (%2.0f%% of oat file bytes)\n"
                         "native_to_managed_code_bytes = %8zd (%2.0f%% of oat file bytes)\n\n"
                         "class_initializer_code_bytes = %8zd (%2.0f%% of oat file bytes)\n"
                         "large_initializer_code_bytes = %8zd (%2.0f%% of oat file bytes)\n"
                         "large_method_code_bytes      = %8zd (%2.0f%% of oat file bytes)\n\n",
                         oat_file_bytes,
                         managed_code_bytes,
                         PercentOfOatBytes(managed_code_bytes),
                         managed_to_native_code_bytes,
                         PercentOfOatBytes(managed_to_native_code_bytes),
                         native_to_managed_code_bytes,
                         PercentOfOatBytes(native_to_managed_code_bytes),
                         class_initializer_code_bytes,
                         PercentOfOatBytes(class_initializer_code_bytes),
                         large_initializer_code_bytes,
                         PercentOfOatBytes(large_initializer_code_bytes),
                         large_method_code_bytes,
                         PercentOfOatBytes(large_method_code_bytes))
            << "DexFile sizes:\n";
      for (const std::pair<std::string, size_t>& oat_dex_file_size : oat_dex_file_sizes) {
        os << StringPrintf("%s = %zd (%2.0f%% of oat file bytes)\n",
                           oat_dex_file_size.first.c_str(), oat_dex_file_size.second,
                           PercentOfOatBytes(oat_dex_file_size.second));
      }

      os << "\n" << StringPrintf("vmap_table_bytes       = %7zd (%2.0f%% of oat file bytes)\n\n",
                                 vmap_table_bytes, PercentOfOatBytes(vmap_table_bytes))
         << std::flush;

      os << StringPrintf("dex_instruction_bytes = %zd\n", dex_instruction_bytes)
         << StringPrintf("managed_code_bytes expansion = %.2f (ignoring deduplication %.2f)\n\n",
                         static_cast<double>(managed_code_bytes) /
                             static_cast<double>(dex_instruction_bytes),
                         static_cast<double>(managed_code_bytes_ignoring_deduplication) /
                             static_cast<double>(dex_instruction_bytes))
         << std::flush;

      DumpOutliers(os);
    }
  } stats_;

 private:
  enum {
    // Number of bytes for a constructor to be considered large. Based on the 1000 basic block
    // threshold, we assume 2 bytes per instruction and 2 instructions per block.
    kLargeConstructorDexBytes = 4000,
    // Number of bytes for a method to be considered large. Based on the 4000 basic block
    // threshold, we assume 2 bytes per instruction and 2 instructions per block.
    kLargeMethodDexBytes = 16000
  };

  // For performance, use the *os_ directly for anything that doesn't need indentation
  // and prepare an indentation stream with default indentation 1.
  std::ostream* os_;
  VariableIndentationOutputStream vios_;
  ScopedIndentation indent1_;

  gc::space::ImageSpace& image_space_;
  const ImageHeader& image_header_;
  std::unique_ptr<OatDumper> oat_dumper_;
  OatDumperOptions* oat_dumper_options_;
  std::set<mirror::Object*> dex_caches_;

  DISALLOW_COPY_AND_ASSIGN(ImageDumper);
};

static int DumpImage(gc::space::ImageSpace* image_space,
                     OatDumperOptions* options,
                     std::ostream* os) SHARED_REQUIRES(Locks::mutator_lock_) {
  const ImageHeader& image_header = image_space->GetImageHeader();
  if (!image_header.IsValid()) {
    fprintf(stderr, "Invalid image header %s\n", image_space->GetImageLocation().c_str());
    return EXIT_FAILURE;
  }
  ImageDumper image_dumper(os, *image_space, image_header, options);
  if (!image_dumper.Dump()) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}

static int DumpImages(Runtime* runtime, OatDumperOptions* options, std::ostream* os) {
  // Dumping the image, no explicit class loader.
  ScopedNullHandle<mirror::ClassLoader> null_class_loader;
  options->class_loader_ = &null_class_loader;

  ScopedObjectAccess soa(Thread::Current());
  if (options->app_image_ != nullptr) {
    if (options->app_oat_ == nullptr) {
      LOG(ERROR) << "Can not dump app image without app oat file";
      return EXIT_FAILURE;
    }
    // We can't know if the app image is 32 bits yet, but it contains pointers into the oat file.
    // We need to map the oat file in the low 4gb or else the fixup wont be able to fit oat file
    // pointers into 32 bit pointer sized ArtMethods.
    std::string error_msg;
    std::unique_ptr<OatFile> oat_file(OatFile::Open(options->app_oat_,
                                                    options->app_oat_,
                                                    nullptr,
                                                    nullptr,
                                                    false,
                                                    /*low_4gb*/true,
                                                    nullptr,
                                                    &error_msg));
    if (oat_file == nullptr) {
      LOG(ERROR) << "Failed to open oat file " << options->app_oat_ << " with error " << error_msg;
      return EXIT_FAILURE;
    }
    std::unique_ptr<gc::space::ImageSpace> space(
        gc::space::ImageSpace::CreateFromAppImage(options->app_image_, oat_file.get(), &error_msg));
    if (space == nullptr) {
      LOG(ERROR) << "Failed to open app image " << options->app_image_ << " with error "
                 << error_msg;
    }
    // Open dex files for the image.
    std::vector<std::unique_ptr<const DexFile>> dex_files;
    if (!runtime->GetClassLinker()->OpenImageDexFiles(space.get(), &dex_files, &error_msg)) {
      LOG(ERROR) << "Failed to open app image dex files " << options->app_image_ << " with error "
                 << error_msg;
    }
    // Dump the actual image.
    int result = DumpImage(space.get(), options, os);
    if (result != EXIT_SUCCESS) {
      return result;
    }
    // Fall through to dump the boot images.
  }

  gc::Heap* heap = runtime->GetHeap();
  CHECK(heap->HasBootImageSpace()) << "No image spaces";
  for (gc::space::ImageSpace* image_space : heap->GetBootImageSpaces()) {
    int result = DumpImage(image_space, options, os);
    if (result != EXIT_SUCCESS) {
      return result;
    }
  }
  return EXIT_SUCCESS;
}

static int DumpOatWithRuntime(Runtime* runtime, OatFile* oat_file, OatDumperOptions* options,
                              std::ostream* os) {
  CHECK(runtime != nullptr && oat_file != nullptr && options != nullptr);

  Thread* self = Thread::Current();
  CHECK(self != nullptr);
  // Need well-known-classes.
  WellKnownClasses::Init(self->GetJniEnv());

  // Need to register dex files to get a working dex cache.
  ScopedObjectAccess soa(self);
  ClassLinker* class_linker = runtime->GetClassLinker();
  runtime->GetOatFileManager().RegisterOatFile(std::unique_ptr<const OatFile>(oat_file));
  std::vector<const DexFile*> class_path;
  for (const OatFile::OatDexFile* odf : oat_file->GetOatDexFiles()) {
    std::string error_msg;
    const DexFile* const dex_file = OpenDexFile(odf, &error_msg);
    CHECK(dex_file != nullptr) << error_msg;
    class_linker->RegisterDexFile(*dex_file, nullptr);
    class_path.push_back(dex_file);
  }

  // Need a class loader.
  // Fake that we're a compiler.
  jobject class_loader = class_linker->CreatePathClassLoader(self, class_path);

  // Use the class loader while dumping.
  StackHandleScope<1> scope(self);
  Handle<mirror::ClassLoader> loader_handle = scope.NewHandle(
      soa.Decode<mirror::ClassLoader*>(class_loader));
  options->class_loader_ = &loader_handle;

  OatDumper oat_dumper(*oat_file, *options);
  bool success = oat_dumper.Dump(*os);
  return (success) ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int DumpOatWithoutRuntime(OatFile* oat_file, OatDumperOptions* options, std::ostream* os) {
  CHECK(oat_file != nullptr && options != nullptr);
  // No image = no class loader.
  ScopedNullHandle<mirror::ClassLoader> null_class_loader;
  options->class_loader_ = &null_class_loader;

  OatDumper oat_dumper(*oat_file, *options);
  bool success = oat_dumper.Dump(*os);
  return (success) ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int DumpOat(Runtime* runtime, const char* oat_filename, OatDumperOptions* options,
                   std::ostream* os) {
  std::string error_msg;
  OatFile* oat_file = OatFile::Open(oat_filename,
                                    oat_filename,
                                    nullptr,
                                    nullptr,
                                    false,
                                    /*low_4gb*/false,
                                    nullptr,
                                    &error_msg);
  if (oat_file == nullptr) {
    fprintf(stderr, "Failed to open oat file from '%s': %s\n", oat_filename, error_msg.c_str());
    return EXIT_FAILURE;
  }

  if (runtime != nullptr) {
    return DumpOatWithRuntime(runtime, oat_file, options, os);
  } else {
    return DumpOatWithoutRuntime(oat_file, options, os);
  }
}

static int SymbolizeOat(const char* oat_filename, std::string& output_name, bool no_bits) {
  std::string error_msg;
  OatFile* oat_file = OatFile::Open(oat_filename,
                                    oat_filename,
                                    nullptr,
                                    nullptr,
                                    false,
                                    /*low_4gb*/false,
                                    nullptr,
                                    &error_msg);
  if (oat_file == nullptr) {
    fprintf(stderr, "Failed to open oat file from '%s': %s\n", oat_filename, error_msg.c_str());
    return EXIT_FAILURE;
  }

  bool result;
  // Try to produce an ELF file of the same type. This is finicky, as we have used 32-bit ELF
  // files for 64-bit code in the past.
  if (Is64BitInstructionSet(oat_file->GetOatHeader().GetInstructionSet())) {
    OatSymbolizer<ElfTypes64> oat_symbolizer(oat_file, output_name, no_bits);
    result = oat_symbolizer.Symbolize();
  } else {
    OatSymbolizer<ElfTypes32> oat_symbolizer(oat_file, output_name, no_bits);
    result = oat_symbolizer.Symbolize();
  }
  if (!result) {
    fprintf(stderr, "Failed to symbolize\n");
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}

struct OatdumpArgs : public CmdlineArgs {
 protected:
  using Base = CmdlineArgs;

  virtual ParseStatus ParseCustom(const StringPiece& option,
                                  std::string* error_msg) OVERRIDE {
    {
      ParseStatus base_parse = Base::ParseCustom(option, error_msg);
      if (base_parse != kParseUnknownArgument) {
        return base_parse;
      }
    }

    if (option.starts_with("--oat-file=")) {
      oat_filename_ = option.substr(strlen("--oat-file=")).data();
    } else if (option.starts_with("--image=")) {
      image_location_ = option.substr(strlen("--image=")).data();
    } else if (option == "--no-dump:vmap") {
      dump_vmap_ = false;
    } else if (option =="--dump:code_info_stack_maps") {
      dump_code_info_stack_maps_ = true;
    } else if (option == "--no-disassemble") {
      disassemble_code_ = false;
    } else if (option =="--header-only") {
      dump_header_only_ = true;
    } else if (option.starts_with("--symbolize=")) {
      oat_filename_ = option.substr(strlen("--symbolize=")).data();
      symbolize_ = true;
    } else if (option.starts_with("--only-keep-debug")) {
      only_keep_debug_ = true;
    } else if (option.starts_with("--class-filter=")) {
      class_filter_ = option.substr(strlen("--class-filter=")).data();
    } else if (option.starts_with("--method-filter=")) {
      method_filter_ = option.substr(strlen("--method-filter=")).data();
    } else if (option.starts_with("--list-classes")) {
      list_classes_ = true;
    } else if (option.starts_with("--list-methods")) {
      list_methods_ = true;
    } else if (option.starts_with("--export-dex-to=")) {
      export_dex_location_ = option.substr(strlen("--export-dex-to=")).data();
    } else if (option.starts_with("--addr2instr=")) {
      if (!ParseUint(option.substr(strlen("--addr2instr=")).data(), &addr2instr_)) {
        *error_msg = "Address conversion failed";
        return kParseError;
      }
    } else if (option.starts_with("--app-image=")) {
      app_image_ = option.substr(strlen("--app-image=")).data();
    } else if (option.starts_with("--app-oat=")) {
      app_oat_ = option.substr(strlen("--app-oat=")).data();
    } else {
      return kParseUnknownArgument;
    }

    return kParseOk;
  }

  virtual ParseStatus ParseChecks(std::string* error_msg) OVERRIDE {
    // Infer boot image location from the image location if possible.
    if (boot_image_location_ == nullptr) {
      boot_image_location_ = image_location_;
    }

    // Perform the parent checks.
    ParseStatus parent_checks = Base::ParseChecks(error_msg);
    if (parent_checks != kParseOk) {
      return parent_checks;
    }

    // Perform our own checks.
    if (image_location_ == nullptr && oat_filename_ == nullptr) {
      *error_msg = "Either --image or --oat-file must be specified";
      return kParseError;
    } else if (image_location_ != nullptr && oat_filename_ != nullptr) {
      *error_msg = "Either --image or --oat-file must be specified but not both";
      return kParseError;
    }

    return kParseOk;
  }

  virtual std::string GetUsage() const {
    std::string usage;

    usage +=
        "Usage: oatdump [options] ...\n"
        "    Example: oatdump --image=$ANDROID_PRODUCT_OUT/system/framework/boot.art\n"
        "    Example: adb shell oatdump --image=/system/framework/boot.art\n"
        "\n"
        // Either oat-file or image is required.
        "  --oat-file=<file.oat>: specifies an input oat filename.\n"
        "      Example: --oat-file=/system/framework/boot.oat\n"
        "\n"
        "  --image=<file.art>: specifies an input image location.\n"
        "      Example: --image=/system/framework/boot.art\n"
        "\n"
        "  --app-image=<file.art>: specifies an input app image. Must also have a specified\n"
        " boot image and app oat file.\n"
        "      Example: --app-image=app.art\n"
        "\n"
        "  --app-oat=<file.odex>: specifies an input app oat.\n"
        "      Example: --app-oat=app.odex\n"
        "\n";

    usage += Base::GetUsage();

    usage +=  // Optional.
        "  --no-dump:vmap may be used to disable vmap dumping.\n"
        "      Example: --no-dump:vmap\n"
        "\n"
        "  --dump:code_info_stack_maps enables dumping of stack maps in CodeInfo sections.\n"
        "      Example: --dump:code_info_stack_maps\n"
        "\n"
        "  --no-disassemble may be used to disable disassembly.\n"
        "      Example: --no-disassemble\n"
        "\n"
        "  --header-only may be used to print only the oat header.\n"
        "      Example: --header-only\n"
        "\n"
        "  --list-classes may be used to list target file classes (can be used with filters).\n"
        "      Example: --list-classes\n"
        "      Example: --list-classes --class-filter=com.example.foo\n"
        "\n"
        "  --list-methods may be used to list target file methods (can be used with filters).\n"
        "      Example: --list-methods\n"
        "      Example: --list-methods --class-filter=com.example --method-filter=foo\n"
        "\n"
        "  --symbolize=<file.oat>: output a copy of file.oat with elf symbols included.\n"
        "      Example: --symbolize=/system/framework/boot.oat\n"
        "\n"
        "  --only-keep-debug<file.oat>: Modifies the behaviour of --symbolize so that\n"
        "      .rodata and .text sections are omitted in the output file to save space.\n"
        "      Example: --symbolize=/system/framework/boot.oat --only-keep-debug\n"
        "\n"
        "  --class-filter=<class name>: only dumps classes that contain the filter.\n"
        "      Example: --class-filter=com.example.foo\n"
        "\n"
        "  --method-filter=<method name>: only dumps methods that contain the filter.\n"
        "      Example: --method-filter=foo\n"
        "\n"
        "  --export-dex-to=<directory>: may be used to export oat embedded dex files.\n"
        "      Example: --export-dex-to=/data/local/tmp\n"
        "\n"
        "  --addr2instr=<address>: output matching method disassembled code from relative\n"
        "                          address (e.g. PC from crash dump)\n"
        "      Example: --addr2instr=0x00001a3b\n"
        "\n";

    return usage;
  }

 public:
  const char* oat_filename_ = nullptr;
  const char* class_filter_ = "";
  const char* method_filter_ = "";
  const char* image_location_ = nullptr;
  std::string elf_filename_prefix_;
  bool dump_vmap_ = true;
  bool dump_code_info_stack_maps_ = false;
  bool disassemble_code_ = true;
  bool symbolize_ = false;
  bool only_keep_debug_ = false;
  bool list_classes_ = false;
  bool list_methods_ = false;
  bool dump_header_only_ = false;
  uint32_t addr2instr_ = 0;
  const char* export_dex_location_ = nullptr;
  const char* app_image_ = nullptr;
  const char* app_oat_ = nullptr;
};

struct OatdumpMain : public CmdlineMain<OatdumpArgs> {
  virtual bool NeedsRuntime() OVERRIDE {
    CHECK(args_ != nullptr);

    // If we are only doing the oat file, disable absolute_addresses. Keep them for image dumping.
    bool absolute_addresses = (args_->oat_filename_ == nullptr);

    oat_dumper_options_.reset(new OatDumperOptions(
        args_->dump_vmap_,
        args_->dump_code_info_stack_maps_,
        args_->disassemble_code_,
        absolute_addresses,
        args_->class_filter_,
        args_->method_filter_,
        args_->list_classes_,
        args_->list_methods_,
        args_->dump_header_only_,
        args_->export_dex_location_,
        args_->app_image_,
        args_->app_oat_,
        args_->addr2instr_));

    return (args_->boot_image_location_ != nullptr || args_->image_location_ != nullptr) &&
          !args_->symbolize_;
  }

  virtual bool ExecuteWithoutRuntime() OVERRIDE {
    CHECK(args_ != nullptr);
    CHECK(args_->oat_filename_ != nullptr);

    MemMap::Init();

    if (args_->symbolize_) {
      // ELF has special kind of section called SHT_NOBITS which allows us to create
      // sections which exist but their data is omitted from the ELF file to save space.
      // This is what "strip --only-keep-debug" does when it creates separate ELF file
      // with only debug data. We use it in similar way to exclude .rodata and .text.
      bool no_bits = args_->only_keep_debug_;
      return SymbolizeOat(args_->oat_filename_, args_->output_name_, no_bits) == EXIT_SUCCESS;
    } else {
      return DumpOat(nullptr,
                     args_->oat_filename_,
                     oat_dumper_options_.get(),
                     args_->os_) == EXIT_SUCCESS;
    }
  }

  virtual bool ExecuteWithRuntime(Runtime* runtime) {
    CHECK(args_ != nullptr);

    if (args_->oat_filename_ != nullptr) {
      return DumpOat(runtime,
                     args_->oat_filename_,
                     oat_dumper_options_.get(),
                     args_->os_) == EXIT_SUCCESS;
    }

    return DumpImages(runtime, oat_dumper_options_.get(), args_->os_) == EXIT_SUCCESS;
  }

  std::unique_ptr<OatDumperOptions> oat_dumper_options_;
};

}  // namespace art

int main(int argc, char** argv) {
  art::OatdumpMain main;
  return main.Main(argc, argv);
}
