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
#include <string>
#include <unordered_map>
#include <vector>

#include "base/stringpiece.h"
#include "base/unix_file/fd_file.h"
#include "class_linker.h"
#include "class_linker-inl.h"
#include "dex_file-inl.h"
#include "dex_instruction.h"
#include "disassembler.h"
#include "elf_builder.h"
#include "field_helper.h"
#include "gc_map.h"
#include "gc/space/image_space.h"
#include "gc/space/large_object_space.h"
#include "gc/space/space-inl.h"
#include "image.h"
#include "indenter.h"
#include "mapping_table.h"
#include "mirror/art_field-inl.h"
#include "mirror/art_method-inl.h"
#include "mirror/array-inl.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "noop_compiler_callbacks.h"
#include "oat.h"
#include "oat_file-inl.h"
#include "os.h"
#include "output_stream.h"
#include "runtime.h"
#include "safe_map.h"
#include "scoped_thread_state_change.h"
#include "thread_list.h"
#include "verifier/dex_gc_map.h"
#include "verifier/method_verifier.h"
#include "vmap_table.h"

namespace art {

static void usage() {
  fprintf(stderr,
          "Usage: oatdump [options] ...\n"
          "    Example: oatdump --image=$ANDROID_PRODUCT_OUT/system/framework/boot.art\n"
          "    Example: adb shell oatdump --image=/system/framework/boot.art\n"
          "\n");
  fprintf(stderr,
          "  --oat-file=<file.oat>: specifies an input oat filename.\n"
          "      Example: --oat-file=/system/framework/boot.oat\n"
          "\n");
  fprintf(stderr,
          "  --image=<file.art>: specifies an input image filename.\n"
          "      Example: --image=/system/framework/boot.art\n"
          "\n");
  fprintf(stderr,
          "  --boot-image=<file.art>: provide the image file for the boot class path.\n"
          "      Example: --boot-image=/system/framework/boot.art\n"
          "\n");
  fprintf(stderr,
          "  --instruction-set=(arm|arm64|mips|x86|x86_64): for locating the image file based on the image location\n"
          "      set.\n"
          "      Example: --instruction-set=x86\n"
          "      Default: %s\n"
          "\n",
          GetInstructionSetString(kRuntimeISA));
  fprintf(stderr,
          "  --output=<file> may be used to send the output to a file.\n"
          "      Example: --output=/tmp/oatdump.txt\n"
          "\n");
  fprintf(stderr,
          "  --dump:[raw_mapping_table|raw_gc_map]\n"
          "    Example: --dump:raw_gc_map\n"
          "    Default: neither\n"
          "\n");
  exit(EXIT_FAILURE);
}

const char* image_roots_descriptions_[] = {
  "kResolutionMethod",
  "kImtConflictMethod",
  "kDefaultImt",
  "kCalleeSaveMethod",
  "kRefsOnlySaveMethod",
  "kRefsAndArgsSaveMethod",
  "kDexCaches",
  "kClassRoots",
};

class OatSymbolizer : public CodeOutput {
 public:
  explicit OatSymbolizer(const OatFile* oat_file, std::string& output_name) :
      oat_file_(oat_file), builder_(nullptr), elf_output_(nullptr), output_name_(output_name) {}

  bool Init() {
    Elf32_Word oat_data_size = oat_file_->GetOatHeader().GetExecutableOffset();

    uint32_t diff = static_cast<uint32_t>(oat_file_->End() - oat_file_->Begin());
    uint32_t oat_exec_size = diff - oat_data_size;

    if (output_name_.empty()) {
      output_name_ = "symbolized.oat";
    }
    elf_output_ = OS::CreateEmptyFile(output_name_.c_str());

    builder_.reset(new ElfBuilder<Elf32_Word, Elf32_Sword, Elf32_Addr, Elf32_Dyn,
                                  Elf32_Sym, Elf32_Ehdr, Elf32_Phdr, Elf32_Shdr>(
        this,
        elf_output_,
        oat_file_->GetOatHeader().GetInstructionSet(),
        0,
        oat_data_size,
        oat_data_size,
        oat_exec_size,
        true,
        false));

    if (!builder_->Init()) {
      builder_.reset(nullptr);
      return false;
    }

    return true;
  }

  typedef void (OatSymbolizer::*Callback)(const DexFile::ClassDef&,
                                          uint32_t,
                                          const OatFile::OatMethod&,
                                          const DexFile&,
                                          uint32_t,
                                          const DexFile::CodeItem*,
                                          uint32_t);

  bool Symbolize() {
    if (builder_.get() == nullptr) {
      return false;
    }

    Walk(&art::OatSymbolizer::RegisterForDedup);

    NormalizeState();

    Walk(&art::OatSymbolizer::AddSymbol);

    bool result = builder_->Write();

    elf_output_->Flush();
    elf_output_->Close();

    return result;
  }

  void Walk(Callback callback) {
    std::vector<const OatFile::OatDexFile*> oat_dex_files = oat_file_->GetOatDexFiles();
    for (size_t i = 0; i < oat_dex_files.size(); i++) {
      const OatFile::OatDexFile* oat_dex_file = oat_dex_files[i];
      CHECK(oat_dex_file != NULL);
      WalkOatDexFile(oat_dex_file, callback);
    }
  }

  void WalkOatDexFile(const OatFile::OatDexFile* oat_dex_file, Callback callback) {
    std::string error_msg;
    std::unique_ptr<const DexFile> dex_file(oat_dex_file->OpenDexFile(&error_msg));
    if (dex_file.get() == nullptr) {
      return;
    }
    for (size_t class_def_index = 0;
        class_def_index < dex_file->NumClassDefs();
        class_def_index++) {
      const DexFile::ClassDef& class_def = dex_file->GetClassDef(class_def_index);
      const OatFile::OatClass oat_class = oat_dex_file->GetOatClass(class_def_index);
      OatClassType type = oat_class.GetType();
      switch (type) {
        case kOatClassAllCompiled:
        case kOatClassSomeCompiled:
          WalkOatClass(oat_class, *dex_file.get(), class_def, callback);
          break;

        case kOatClassNoneCompiled:
        case kOatClassMax:
          // Ignore.
          break;
      }
    }
  }

  void WalkOatClass(const OatFile::OatClass& oat_class, const DexFile& dex_file,
                    const DexFile::ClassDef& class_def, Callback callback) {
    const byte* class_data = dex_file.GetClassData(class_def);
    if (class_data == nullptr) {  // empty class such as a marker interface?
      return;
    }
    // Note: even if this is an interface or a native class, we still have to walk it, as there
    //       might be a static initializer.
    ClassDataItemIterator it(dex_file, class_data);
    SkipAllFields(&it);
    uint32_t class_method_idx = 0;
    while (it.HasNextDirectMethod()) {
      const OatFile::OatMethod oat_method = oat_class.GetOatMethod(class_method_idx);
      WalkOatMethod(class_def, class_method_idx, oat_method, dex_file, it.GetMemberIndex(),
                    it.GetMethodCodeItem(), it.GetMethodAccessFlags(), callback);
      class_method_idx++;
      it.Next();
    }
    while (it.HasNextVirtualMethod()) {
      const OatFile::OatMethod oat_method = oat_class.GetOatMethod(class_method_idx);
      WalkOatMethod(class_def, class_method_idx, oat_method, dex_file, it.GetMemberIndex(),
                    it.GetMethodCodeItem(), it.GetMethodAccessFlags(), callback);
      class_method_idx++;
      it.Next();
    }
    DCHECK(!it.HasNext());
  }

  void WalkOatMethod(const DexFile::ClassDef& class_def, uint32_t class_method_index,
                     const OatFile::OatMethod& oat_method, const DexFile& dex_file,
                     uint32_t dex_method_idx, const DexFile::CodeItem* code_item,
                     uint32_t method_access_flags, Callback callback) {
    if ((method_access_flags & kAccAbstract) != 0) {
      // Abstract method, no code.
      return;
    }
    if (oat_method.GetCodeOffset() == 0) {
      // No code.
      return;
    }

    (this->*callback)(class_def, class_method_index, oat_method, dex_file, dex_method_idx, code_item,
                      method_access_flags);
  }

  void RegisterForDedup(const DexFile::ClassDef& class_def, uint32_t class_method_index,
                        const OatFile::OatMethod& oat_method, const DexFile& dex_file,
                        uint32_t dex_method_idx, const DexFile::CodeItem* code_item,
                        uint32_t method_access_flags) {
    state_[oat_method.GetCodeOffset()]++;
  }

  void NormalizeState() {
    for (auto& x : state_) {
      if (x.second == 1) {
        state_[x.first] = 0;
      }
    }
  }

  enum class DedupState {  // private
    kNotDeduplicated,
    kDeduplicatedFirst,
    kDeduplicatedOther
  };
  DedupState IsDuplicated(uint32_t offset) {
    if (state_[offset] == 0) {
      return DedupState::kNotDeduplicated;
    }
    if (state_[offset] == 1) {
      return DedupState::kDeduplicatedOther;
    }
    state_[offset] = 1;
    return DedupState::kDeduplicatedFirst;
  }

  void AddSymbol(const DexFile::ClassDef& class_def, uint32_t class_method_index,
                 const OatFile::OatMethod& oat_method, const DexFile& dex_file,
                 uint32_t dex_method_idx, const DexFile::CodeItem* code_item,
                 uint32_t method_access_flags) {
    DedupState dedup = IsDuplicated(oat_method.GetCodeOffset());
    if (dedup != DedupState::kDeduplicatedOther) {
      std::string pretty_name = PrettyMethod(dex_method_idx, dex_file, true);

      if (dedup == DedupState::kDeduplicatedFirst) {
        pretty_name = "[Dedup]" + pretty_name;
      }

      ElfSymtabBuilder<Elf32_Word, Elf32_Sword, Elf32_Addr,
      Elf32_Sym, Elf32_Shdr>* symtab = &builder_->symtab_builder_;

      symtab->AddSymbol(pretty_name, &builder_->text_builder_, oat_method.GetCodeOffset() -
                        oat_file_->GetOatHeader().GetExecutableOffset(), true,
                        oat_method.GetQuickCodeSize(), STB_GLOBAL, STT_FUNC);
    }
  }

  // Write oat code. Required by ElfBuilder/CodeOutput.
  bool Write(OutputStream* out) {
    return out->WriteFully(oat_file_->Begin(), oat_file_->End() - oat_file_->Begin());
  }

 private:
  static void SkipAllFields(ClassDataItemIterator* it) {
    while (it->HasNextStaticField()) {
      it->Next();
    }
    while (it->HasNextInstanceField()) {
      it->Next();
    }
  }

  const OatFile* oat_file_;
  std::unique_ptr<ElfBuilder<Elf32_Word, Elf32_Sword, Elf32_Addr, Elf32_Dyn,
                              Elf32_Sym, Elf32_Ehdr, Elf32_Phdr, Elf32_Shdr> > builder_;
  File* elf_output_;
  std::unordered_map<uint32_t, uint32_t> state_;
  std::string output_name_;
};

class OatDumper {
 public:
  explicit OatDumper(const OatFile& oat_file, bool dump_raw_mapping_table, bool dump_raw_gc_map)
    : oat_file_(oat_file),
      oat_dex_files_(oat_file.GetOatDexFiles()),
      dump_raw_mapping_table_(dump_raw_mapping_table),
      dump_raw_gc_map_(dump_raw_gc_map),
      disassembler_(Disassembler::Create(oat_file_.GetOatHeader().GetInstructionSet())) {
    AddAllOffsets();
  }

  void Dump(std::ostream& os) {
    const OatHeader& oat_header = oat_file_.GetOatHeader();

    os << "MAGIC:\n";
    os << oat_header.GetMagic() << "\n\n";

    os << "CHECKSUM:\n";
    os << StringPrintf("0x%08x\n\n", oat_header.GetChecksum());

    os << "INSTRUCTION SET:\n";
    os << oat_header.GetInstructionSet() << "\n\n";

    os << "INSTRUCTION SET FEATURES:\n";
    os << oat_header.GetInstructionSetFeatures().GetFeatureString() << "\n\n";

    os << "DEX FILE COUNT:\n";
    os << oat_header.GetDexFileCount() << "\n\n";

#define DUMP_OAT_HEADER_OFFSET(label, offset) \
    os << label " OFFSET:\n"; \
    os << StringPrintf("0x%08x", oat_header.offset()); \
    if (oat_header.offset() != 0) { \
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
    DUMP_OAT_HEADER_OFFSET("PORTABLE IMT CONFLICT TRAMPOLINE",
                           GetPortableImtConflictTrampolineOffset);
    DUMP_OAT_HEADER_OFFSET("PORTABLE RESOLUTION TRAMPOLINE",
                           GetPortableResolutionTrampolineOffset);
    DUMP_OAT_HEADER_OFFSET("PORTABLE TO INTERPRETER BRIDGE",
                           GetPortableToInterpreterBridgeOffset);
    DUMP_OAT_HEADER_OFFSET("QUICK GENERIC JNI TRAMPOLINE",
                           GetQuickGenericJniTrampolineOffset);
    DUMP_OAT_HEADER_OFFSET("QUICK IMT CONFLICT TRAMPOLINE",
                           GetQuickImtConflictTrampolineOffset);
    DUMP_OAT_HEADER_OFFSET("QUICK RESOLUTION TRAMPOLINE",
                           GetQuickResolutionTrampolineOffset);
    DUMP_OAT_HEADER_OFFSET("QUICK TO INTERPRETER BRIDGE",
                           GetQuickToInterpreterBridgeOffset);
#undef DUMP_OAT_HEADER_OFFSET

    os << "IMAGE PATCH DELTA:\n" << oat_header.GetImagePatchDelta() << "\n\n";

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

    os << "BEGIN:\n";
    os << reinterpret_cast<const void*>(oat_file_.Begin()) << "\n\n";

    os << "END:\n";
    os << reinterpret_cast<const void*>(oat_file_.End()) << "\n\n";

    os << std::flush;

    for (size_t i = 0; i < oat_dex_files_.size(); i++) {
      const OatFile::OatDexFile* oat_dex_file = oat_dex_files_[i];
      CHECK(oat_dex_file != nullptr);
      DumpOatDexFile(os, *oat_dex_file);
    }
  }

  size_t ComputeSize(const void* oat_data) {
    if (reinterpret_cast<const byte*>(oat_data) < oat_file_.Begin() ||
        reinterpret_cast<const byte*>(oat_data) > oat_file_.End()) {
      return 0;  // Address not in oat file
    }
    uintptr_t begin_offset = reinterpret_cast<uintptr_t>(oat_data) -
                             reinterpret_cast<uintptr_t>(oat_file_.Begin());
    auto it = offsets_.upper_bound(begin_offset);
    CHECK(it != offsets_.end());
    uintptr_t end_offset = *it;
    return end_offset - begin_offset;
  }

  InstructionSet GetInstructionSet() {
    return oat_file_.GetOatHeader().GetInstructionSet();
  }

  const void* GetQuickOatCode(mirror::ArtMethod* m) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    for (size_t i = 0; i < oat_dex_files_.size(); i++) {
      const OatFile::OatDexFile* oat_dex_file = oat_dex_files_[i];
      CHECK(oat_dex_file != nullptr);
      std::string error_msg;
      std::unique_ptr<const DexFile> dex_file(oat_dex_file->OpenDexFile(&error_msg));
      if (dex_file.get() == nullptr) {
        LOG(WARNING) << "Failed to open dex file '" << oat_dex_file->GetDexFileLocation()
            << "': " << error_msg;
      } else {
        const DexFile::ClassDef* class_def =
            dex_file->FindClassDef(m->GetDeclaringClassDescriptor());
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
      std::unique_ptr<const DexFile> dex_file(oat_dex_file->OpenDexFile(&error_msg));
      if (dex_file.get() == nullptr) {
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
        const byte* class_data = dex_file->GetClassData(class_def);
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

  void AddOffsets(const OatFile::OatMethod& oat_method) {
    uint32_t code_offset = oat_method.GetCodeOffset();
    if (oat_file_.GetOatHeader().GetInstructionSet() == kThumb2) {
      code_offset &= ~0x1;
    }
    offsets_.insert(code_offset);
    offsets_.insert(oat_method.GetMappingTableOffset());
    offsets_.insert(oat_method.GetVmapTableOffset());
    offsets_.insert(oat_method.GetNativeGcMapOffset());
  }

  void DumpOatDexFile(std::ostream& os, const OatFile::OatDexFile& oat_dex_file) {
    os << "OAT DEX FILE:\n";
    os << StringPrintf("location: %s\n", oat_dex_file.GetDexFileLocation().c_str());
    os << StringPrintf("checksum: 0x%08x\n", oat_dex_file.GetDexFileLocationChecksum());

    // Create the verifier early.

    std::string error_msg;
    std::unique_ptr<const DexFile> dex_file(oat_dex_file.OpenDexFile(&error_msg));
    if (dex_file.get() == nullptr) {
      os << "NOT FOUND: " << error_msg << "\n\n";
      return;
    }
    for (size_t class_def_index = 0;
         class_def_index < dex_file->NumClassDefs();
         class_def_index++) {
      const DexFile::ClassDef& class_def = dex_file->GetClassDef(class_def_index);
      const char* descriptor = dex_file->GetClassDescriptor(class_def);
      const OatFile::OatClass oat_class = oat_dex_file.GetOatClass(class_def_index);
      os << StringPrintf("%zd: %s (type_idx=%d)", class_def_index, descriptor, class_def.class_idx_)
         << " (" << oat_class.GetStatus() << ")"
         << " (" << oat_class.GetType() << ")\n";
      // TODO: include bitmap here if type is kOatClassSomeCompiled?
      Indenter indent_filter(os.rdbuf(), kIndentChar, kIndentBy1Count);
      std::ostream indented_os(&indent_filter);
      DumpOatClass(indented_os, oat_class, *(dex_file.get()), class_def);
    }

    os << std::flush;
  }

  static void SkipAllFields(ClassDataItemIterator& it) {
    while (it.HasNextStaticField()) {
      it.Next();
    }
    while (it.HasNextInstanceField()) {
      it.Next();
    }
  }

  void DumpOatClass(std::ostream& os, const OatFile::OatClass& oat_class, const DexFile& dex_file,
                    const DexFile::ClassDef& class_def) {
    const byte* class_data = dex_file.GetClassData(class_def);
    if (class_data == nullptr) {  // empty class such as a marker interface?
      return;
    }
    ClassDataItemIterator it(dex_file, class_data);
    SkipAllFields(it);
    uint32_t class_method_idx = 0;
    while (it.HasNextDirectMethod()) {
      const OatFile::OatMethod oat_method = oat_class.GetOatMethod(class_method_idx);
      DumpOatMethod(os, class_def, class_method_idx, oat_method, dex_file,
                    it.GetMemberIndex(), it.GetMethodCodeItem(), it.GetRawMemberAccessFlags());
      class_method_idx++;
      it.Next();
    }
    while (it.HasNextVirtualMethod()) {
      const OatFile::OatMethod oat_method = oat_class.GetOatMethod(class_method_idx);
      DumpOatMethod(os, class_def, class_method_idx, oat_method, dex_file,
                    it.GetMemberIndex(), it.GetMethodCodeItem(), it.GetRawMemberAccessFlags());
      class_method_idx++;
      it.Next();
    }
    DCHECK(!it.HasNext());
    os << std::flush;
  }

  void DumpOatMethod(std::ostream& os, const DexFile::ClassDef& class_def,
                     uint32_t class_method_index,
                     const OatFile::OatMethod& oat_method, const DexFile& dex_file,
                     uint32_t dex_method_idx, const DexFile::CodeItem* code_item,
                     uint32_t method_access_flags) {
    os << StringPrintf("%d: %s (dex_method_idx=%d)\n",
                       class_method_index, PrettyMethod(dex_method_idx, dex_file, true).c_str(),
                       dex_method_idx);
    Indenter indent1_filter(os.rdbuf(), kIndentChar, kIndentBy1Count);
    std::unique_ptr<std::ostream> indent1_os(new std::ostream(&indent1_filter));
    Indenter indent2_filter(indent1_os->rdbuf(), kIndentChar, kIndentBy1Count);
    std::unique_ptr<std::ostream> indent2_os(new std::ostream(&indent2_filter));
    {
      *indent1_os << "DEX CODE:\n";
      DumpDexCode(*indent2_os, dex_file, code_item);
    }

    std::unique_ptr<verifier::MethodVerifier> verifier;
    if (Runtime::Current() != nullptr) {
      *indent1_os << "VERIFIER TYPE ANALYSIS:\n";
      verifier.reset(DumpVerifier(*indent2_os, dex_method_idx, &dex_file, class_def, code_item,
                                  method_access_flags));
    }
    {
      *indent1_os << "OAT DATA:\n";

      *indent2_os << StringPrintf("frame_size_in_bytes: %zd\n", oat_method.GetFrameSizeInBytes());
      *indent2_os << StringPrintf("core_spill_mask: 0x%08x ", oat_method.GetCoreSpillMask());
      DumpSpillMask(*indent2_os, oat_method.GetCoreSpillMask(), false);
      *indent2_os << StringPrintf("\nfp_spill_mask: 0x%08x ", oat_method.GetFpSpillMask());
      DumpSpillMask(*indent2_os, oat_method.GetFpSpillMask(), true);
      *indent2_os << StringPrintf("\nvmap_table: %p (offset=0x%08x)\n",
                                  oat_method.GetVmapTable(), oat_method.GetVmapTableOffset());

      if (oat_method.GetNativeGcMap() != nullptr) {
        // The native GC map is null for methods compiled with the optimizing compiler.
        DumpVmap(*indent2_os, oat_method);
      }
      DumpVregLocations(*indent2_os, oat_method, code_item);
      *indent2_os << StringPrintf("mapping_table: %p (offset=0x%08x)\n",
                                  oat_method.GetMappingTable(), oat_method.GetMappingTableOffset());
      if (dump_raw_mapping_table_) {
        Indenter indent3_filter(indent2_os->rdbuf(), kIndentChar, kIndentBy1Count);
        std::ostream indent3_os(&indent3_filter);
        DumpMappingTable(indent3_os, oat_method);
      }
      *indent2_os << StringPrintf("gc_map: %p (offset=0x%08x)\n",
                                  oat_method.GetNativeGcMap(), oat_method.GetNativeGcMapOffset());
      if (dump_raw_gc_map_) {
        Indenter indent3_filter(indent2_os->rdbuf(), kIndentChar, kIndentBy1Count);
        std::ostream indent3_os(&indent3_filter);
        DumpGcMap(indent3_os, oat_method, code_item);
      }
    }
    {
      const void* code = oat_method.GetQuickCode();
      uint32_t code_size = oat_method.GetQuickCodeSize();
      if (code == nullptr) {
        code = oat_method.GetPortableCode();
        code_size = oat_method.GetPortableCodeSize();
      }
      *indent1_os << StringPrintf("CODE: %p (offset=0x%08x size=%d)%s\n",
                                 code,
                                 oat_method.GetCodeOffset(),
                                 code_size,
                                 code != nullptr ? "..." : "");

      DumpCode(*indent2_os, verifier.get(), oat_method, code_item);
    }
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

  void DumpVmap(std::ostream& os, const OatFile::OatMethod& oat_method) {
    const uint8_t* raw_table = oat_method.GetVmapTable();
    if (raw_table != nullptr) {
      const VmapTable vmap_table(raw_table);
      bool first = true;
      bool processing_fp = false;
      uint32_t spill_mask = oat_method.GetCoreSpillMask();
      for (size_t i = 0; i < vmap_table.Size(); i++) {
        uint16_t dex_reg = vmap_table[i];
        uint32_t cpu_reg = vmap_table.ComputeRegister(spill_mask, i,
                                                      processing_fp ? kFloatVReg : kIntVReg);
        os << (first ? "v" : ", v")  << dex_reg;
        if (!processing_fp) {
          os << "/r" << cpu_reg;
        } else {
          os << "/fr" << cpu_reg;
        }
        first = false;
        if (!processing_fp && dex_reg == 0xFFFF) {
          processing_fp = true;
          spill_mask = oat_method.GetFpSpillMask();
        }
      }
      os << "\n";
    }
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

        uint32_t offset = StackVisitor::GetVRegOffset(code_item, oat_method.GetCoreSpillMask(),
                                                      oat_method.GetFpSpillMask(),
                                                      oat_method.GetFrameSizeInBytes(), reg,
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

  void DescribeVReg(std::ostream& os, const OatFile::OatMethod& oat_method,
                    const DexFile::CodeItem* code_item, size_t reg, VRegKind kind) {
    const uint8_t* raw_table = oat_method.GetVmapTable();
    if (raw_table != nullptr) {
      const VmapTable vmap_table(raw_table);
      uint32_t vmap_offset;
      if (vmap_table.IsInContext(reg, kind, &vmap_offset)) {
        bool is_float = (kind == kFloatVReg) || (kind == kDoubleLoVReg) || (kind == kDoubleHiVReg);
        uint32_t spill_mask = is_float ? oat_method.GetFpSpillMask()
                                       : oat_method.GetCoreSpillMask();
        os << (is_float ? "fr" : "r") << vmap_table.ComputeRegister(spill_mask, vmap_offset, kind);
      } else {
        uint32_t offset = StackVisitor::GetVRegOffset(code_item, oat_method.GetCoreSpillMask(),
                                                      oat_method.GetFpSpillMask(),
                                                      oat_method.GetFrameSizeInBytes(), reg,
                                                      GetInstructionSet());
        os << "[sp + #" << offset << "]";
      }
    }
  }

  void DumpGcMapRegisters(std::ostream& os, const OatFile::OatMethod& oat_method,
                          const DexFile::CodeItem* code_item,
                          size_t num_regs, const uint8_t* reg_bitmap) {
    bool first = true;
    for (size_t reg = 0; reg < num_regs; reg++) {
      if (((reg_bitmap[reg / 8] >> (reg % 8)) & 0x01) != 0) {
        if (first) {
          os << "  v" << reg << " (";
          DescribeVReg(os, oat_method, code_item, reg, kReferenceVReg);
          os << ")";
          first = false;
        } else {
          os << ", v" << reg << " (";
          DescribeVReg(os, oat_method, code_item, reg, kReferenceVReg);
          os << ")";
        }
      }
    }
    if (first) {
      os << "No registers in GC map\n";
    } else {
      os << "\n";
    }
  }
  void DumpGcMap(std::ostream& os, const OatFile::OatMethod& oat_method,
                 const DexFile::CodeItem* code_item) {
    const uint8_t* gc_map_raw = oat_method.GetNativeGcMap();
    if (gc_map_raw == nullptr) {
      return;  // No GC map.
    }
    const void* quick_code = oat_method.GetQuickCode();
    if (quick_code != nullptr) {
      NativePcOffsetToReferenceMap map(gc_map_raw);
      for (size_t entry = 0; entry < map.NumEntries(); entry++) {
        const uint8_t* native_pc = reinterpret_cast<const uint8_t*>(quick_code) +
            map.GetNativePcOffset(entry);
        os << StringPrintf("%p", native_pc);
        DumpGcMapRegisters(os, oat_method, code_item, map.RegWidth() * 8, map.GetBitMap(entry));
      }
    } else {
      const void* portable_code = oat_method.GetPortableCode();
      CHECK(portable_code != nullptr);
      verifier::DexPcToReferenceMap map(gc_map_raw);
      for (size_t entry = 0; entry < map.NumEntries(); entry++) {
        uint32_t dex_pc = map.GetDexPc(entry);
        os << StringPrintf("0x%08x", dex_pc);
        DumpGcMapRegisters(os, oat_method, code_item, map.RegWidth() * 8, map.GetBitMap(entry));
      }
    }
  }

  void DumpMappingTable(std::ostream& os, const OatFile::OatMethod& oat_method) {
    const void* quick_code = oat_method.GetQuickCode();
    if (quick_code == nullptr) {
      return;
    }
    MappingTable table(oat_method.GetMappingTable());
    if (table.TotalSize() != 0) {
      Indenter indent_filter(os.rdbuf(), kIndentChar, kIndentBy1Count);
      std::ostream indent_os(&indent_filter);
      if (table.PcToDexSize() != 0) {
        typedef MappingTable::PcToDexIterator It;
        os << "suspend point mappings {\n";
        for (It cur = table.PcToDexBegin(), end = table.PcToDexEnd(); cur != end; ++cur) {
          indent_os << StringPrintf("0x%04x -> 0x%04x\n", cur.NativePcOffset(), cur.DexPc());
        }
        os << "}\n";
      }
      if (table.DexToPcSize() != 0) {
        typedef MappingTable::DexToPcIterator It;
        os << "catch entry mappings {\n";
        for (It cur = table.DexToPcBegin(), end = table.DexToPcEnd(); cur != end; ++cur) {
          indent_os << StringPrintf("0x%04x -> 0x%04x\n", cur.NativePcOffset(), cur.DexPc());
        }
        os << "}\n";
      }
    }
  }

  uint32_t DumpMappingAtOffset(std::ostream& os, const OatFile::OatMethod& oat_method,
                               size_t offset, bool suspend_point_mapping) {
    MappingTable table(oat_method.GetMappingTable());
    if (suspend_point_mapping && table.PcToDexSize() > 0) {
      typedef MappingTable::PcToDexIterator It;
      for (It cur = table.PcToDexBegin(), end = table.PcToDexEnd(); cur != end; ++cur) {
        if (offset == cur.NativePcOffset()) {
          os << StringPrintf("suspend point dex PC: 0x%04x\n", cur.DexPc());
          return cur.DexPc();
        }
      }
    } else if (!suspend_point_mapping && table.DexToPcSize() > 0) {
      typedef MappingTable::DexToPcIterator It;
      for (It cur = table.DexToPcBegin(), end = table.DexToPcEnd(); cur != end; ++cur) {
        if (offset == cur.NativePcOffset()) {
          os << StringPrintf("catch entry dex PC: 0x%04x\n", cur.DexPc());
          return cur.DexPc();
        }
      }
    }
    return DexFile::kDexNoIndex;
  }

  void DumpGcMapAtNativePcOffset(std::ostream& os, const OatFile::OatMethod& oat_method,
                                 const DexFile::CodeItem* code_item, size_t native_pc_offset) {
    const uint8_t* gc_map_raw = oat_method.GetNativeGcMap();
    if (gc_map_raw != nullptr) {
      NativePcOffsetToReferenceMap map(gc_map_raw);
      if (map.HasEntry(native_pc_offset)) {
        size_t num_regs = map.RegWidth() * 8;
        const uint8_t* reg_bitmap = map.FindBitMap(native_pc_offset);
        bool first = true;
        for (size_t reg = 0; reg < num_regs; reg++) {
          if (((reg_bitmap[reg / 8] >> (reg % 8)) & 0x01) != 0) {
            if (first) {
              os << "GC map objects:  v" << reg << " (";
              DescribeVReg(os, oat_method, code_item, reg, kReferenceVReg);
              os << ")";
              first = false;
            } else {
              os << ", v" << reg << " (";
              DescribeVReg(os, oat_method, code_item, reg, kReferenceVReg);
              os << ")";
            }
          }
        }
        if (!first) {
          os << "\n";
        }
      }
    }
  }

  void DumpVRegsAtDexPc(std::ostream& os, verifier::MethodVerifier* verifier,
                        const OatFile::OatMethod& oat_method,
                        const DexFile::CodeItem* code_item, uint32_t dex_pc) {
    DCHECK(verifier != nullptr);
    std::vector<int32_t> kinds = verifier->DescribeVRegs(dex_pc);
    bool first = true;
    for (size_t reg = 0; reg < code_item->registers_size_; reg++) {
      VRegKind kind = static_cast<VRegKind>(kinds.at(reg * 2));
      if (kind != kUndefined) {
        if (first) {
          os << "VRegs:  v";
          first = false;
        } else {
          os << ", v";
        }
        os << reg << " (";
        switch (kind) {
          case kImpreciseConstant:
            os << "Imprecise Constant: " << kinds.at((reg * 2) + 1) << ", ";
            DescribeVReg(os, oat_method, code_item, reg, kind);
            break;
          case kConstant:
            os << "Constant: " << kinds.at((reg * 2) + 1);
            break;
          default:
            DescribeVReg(os, oat_method, code_item, reg, kind);
            break;
        }
        os << ")";
      }
    }
    if (!first) {
      os << "\n";
    }
  }


  void DumpDexCode(std::ostream& os, const DexFile& dex_file, const DexFile::CodeItem* code_item) {
    if (code_item != nullptr) {
      size_t i = 0;
      while (i < code_item->insns_size_in_code_units_) {
        const Instruction* instruction = Instruction::At(&code_item->insns_[i]);
        os << StringPrintf("0x%04zx: %s\n", i, instruction->DumpString(&dex_file).c_str());
        i += instruction->SizeInCodeUnits();
      }
    }
  }

  verifier::MethodVerifier* DumpVerifier(std::ostream& os, uint32_t dex_method_idx,
                                         const DexFile* dex_file,
                                         const DexFile::ClassDef& class_def,
                                         const DexFile::CodeItem* code_item,
                                         uint32_t method_access_flags) {
    if ((method_access_flags & kAccNative) == 0) {
      ScopedObjectAccess soa(Thread::Current());
      StackHandleScope<1> hs(soa.Self());
      Handle<mirror::DexCache> dex_cache(
          hs.NewHandle(Runtime::Current()->GetClassLinker()->FindDexCache(*dex_file)));
      return verifier::MethodVerifier::VerifyMethodAndDump(soa.Self(), os, dex_method_idx, dex_file,
                                                           dex_cache,
                                                           NullHandle<mirror::ClassLoader>(),
                                                           &class_def, code_item,
                                                           NullHandle<mirror::ArtMethod>(),
                                                           method_access_flags);
    }

    return nullptr;
  }

  void DumpCode(std::ostream& os, verifier::MethodVerifier* verifier,
                const OatFile::OatMethod& oat_method, const DexFile::CodeItem* code_item) {
    const void* portable_code = oat_method.GetPortableCode();
    const void* quick_code = oat_method.GetQuickCode();

    size_t code_size = oat_method.GetQuickCodeSize();
    if ((code_size == 0) || ((portable_code == nullptr) && (quick_code == nullptr))) {
      os << "NO CODE!\n";
      return;
    } else if (quick_code != nullptr) {
      const uint8_t* quick_native_pc = reinterpret_cast<const uint8_t*>(quick_code);
      size_t offset = 0;
      while (offset < code_size) {
        DumpMappingAtOffset(os, oat_method, offset, false);
        offset += disassembler_->Dump(os, quick_native_pc + offset);
        uint32_t dex_pc = DumpMappingAtOffset(os, oat_method, offset, true);
        if (dex_pc != DexFile::kDexNoIndex) {
          DumpGcMapAtNativePcOffset(os, oat_method, code_item, offset);
          if (verifier != nullptr) {
            DumpVRegsAtDexPc(os, verifier, oat_method, code_item, dex_pc);
          }
        }
      }
    } else {
      CHECK(portable_code != nullptr);
      CHECK_EQ(code_size, 0U);  // TODO: disassembly of portable is currently not supported.
    }
  }

  const OatFile& oat_file_;
  std::vector<const OatFile::OatDexFile*> oat_dex_files_;
  bool dump_raw_mapping_table_;
  bool dump_raw_gc_map_;
  std::set<uintptr_t> offsets_;
  std::unique_ptr<Disassembler> disassembler_;
};

class ImageDumper {
 public:
  explicit ImageDumper(std::ostream* os, gc::space::ImageSpace& image_space,
                       const ImageHeader& image_header, bool dump_raw_mapping_table,
                       bool dump_raw_gc_map)
      : os_(os), image_space_(image_space), image_header_(image_header),
        dump_raw_mapping_table_(dump_raw_mapping_table),
        dump_raw_gc_map_(dump_raw_gc_map) {}

  void Dump() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    std::ostream& os = *os_;
    os << "MAGIC: " << image_header_.GetMagic() << "\n\n";

    os << "IMAGE BEGIN: " << reinterpret_cast<void*>(image_header_.GetImageBegin()) << "\n\n";

    os << "IMAGE BITMAP OFFSET: " << reinterpret_cast<void*>(image_header_.GetImageBitmapOffset())
       << " SIZE: " << reinterpret_cast<void*>(image_header_.GetImageBitmapSize()) << "\n\n";

    os << "OAT CHECKSUM: " << StringPrintf("0x%08x\n\n", image_header_.GetOatChecksum());

    os << "OAT FILE BEGIN:" << reinterpret_cast<void*>(image_header_.GetOatFileBegin()) << "\n\n";

    os << "OAT DATA BEGIN:" << reinterpret_cast<void*>(image_header_.GetOatDataBegin()) << "\n\n";

    os << "OAT DATA END:" << reinterpret_cast<void*>(image_header_.GetOatDataEnd()) << "\n\n";

    os << "OAT FILE END:" << reinterpret_cast<void*>(image_header_.GetOatFileEnd()) << "\n\n";

    os << "PATCH DELTA:" << image_header_.GetPatchDelta() << "\n\n";

    {
      os << "ROOTS: " << reinterpret_cast<void*>(image_header_.GetImageRoots()) << "\n";
      Indenter indent1_filter(os.rdbuf(), kIndentChar, kIndentBy1Count);
      std::ostream indent1_os(&indent1_filter);
      CHECK_EQ(arraysize(image_roots_descriptions_), size_t(ImageHeader::kImageRootsMax));
      for (int i = 0; i < ImageHeader::kImageRootsMax; i++) {
        ImageHeader::ImageRoot image_root = static_cast<ImageHeader::ImageRoot>(i);
        const char* image_root_description = image_roots_descriptions_[i];
        mirror::Object* image_root_object = image_header_.GetImageRoot(image_root);
        indent1_os << StringPrintf("%s: %p\n", image_root_description, image_root_object);
        if (image_root_object->IsObjectArray()) {
          Indenter indent2_filter(indent1_os.rdbuf(), kIndentChar, kIndentBy1Count);
          std::ostream indent2_os(&indent2_filter);
          mirror::ObjectArray<mirror::Object>* image_root_object_array
              = image_root_object->AsObjectArray<mirror::Object>();
          for (int i = 0; i < image_root_object_array->GetLength(); i++) {
            mirror::Object* value = image_root_object_array->Get(i);
            size_t run = 0;
            for (int32_t j = i + 1; j < image_root_object_array->GetLength(); j++) {
              if (value == image_root_object_array->Get(j)) {
                run++;
              } else {
                break;
              }
            }
            if (run == 0) {
              indent2_os << StringPrintf("%d: ", i);
            } else {
              indent2_os << StringPrintf("%d to %zd: ", i, i + run);
              i = i + run;
            }
            if (value != nullptr) {
              PrettyObjectValue(indent2_os, value->GetClass(), value);
            } else {
              indent2_os << i << ": null\n";
            }
          }
        }
      }
    }
    os << "\n";

    ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
    std::string image_filename = image_space_.GetImageFilename();
    std::string oat_location = ImageHeader::GetOatLocationFromImageLocation(image_filename);
    os << "OAT LOCATION: " << oat_location;
    os << "\n";
    std::string error_msg;
    const OatFile* oat_file = class_linker->FindOpenedOatFileFromOatLocation(oat_location);
    if (oat_file == nullptr) {
      oat_file = OatFile::Open(oat_location, oat_location, nullptr, false, &error_msg);
      if (oat_file == nullptr) {
        os << "NOT FOUND: " << error_msg << "\n";
        return;
      }
    }
    os << "\n";

    stats_.oat_file_bytes = oat_file->Size();

    oat_dumper_.reset(new OatDumper(*oat_file, dump_raw_mapping_table_,
        dump_raw_gc_map_));

    for (const OatFile::OatDexFile* oat_dex_file : oat_file->GetOatDexFiles()) {
      CHECK(oat_dex_file != nullptr);
      stats_.oat_dex_file_sizes.push_back(std::make_pair(oat_dex_file->GetDexFileLocation(),
                                                         oat_dex_file->FileSize()));
    }

    os << "OBJECTS:\n" << std::flush;

    // Loop through all the image spaces and dump their objects.
    gc::Heap* heap = Runtime::Current()->GetHeap();
    const std::vector<gc::space::ContinuousSpace*>& spaces = heap->GetContinuousSpaces();
    Thread* self = Thread::Current();
    {
      {
        WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
        heap->FlushAllocStack();
      }
      // Since FlushAllocStack() above resets the (active) allocation
      // stack. Need to revoke the thread-local allocation stacks that
      // point into it.
      {
        self->TransitionFromRunnableToSuspended(kNative);
        ThreadList* thread_list = Runtime::Current()->GetThreadList();
        thread_list->SuspendAll();
        heap->RevokeAllThreadLocalAllocationStacks(self);
        thread_list->ResumeAll();
        self->TransitionFromSuspendedToRunnable();
      }
    }
    {
      std::ostream* saved_os = os_;
      Indenter indent_filter(os.rdbuf(), kIndentChar, kIndentBy1Count);
      std::ostream indent_os(&indent_filter);
      os_ = &indent_os;
      ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
      for (const auto& space : spaces) {
        if (space->IsImageSpace()) {
          gc::space::ImageSpace* image_space = space->AsImageSpace();
          image_space->GetLiveBitmap()->Walk(ImageDumper::Callback, this);
          indent_os << "\n";
        }
      }
      // Dump the large objects separately.
      heap->GetLargeObjectsSpace()->GetLiveBitmap()->Walk(ImageDumper::Callback, this);
      indent_os << "\n";
      os_ = saved_os;
    }
    os << "STATS:\n" << std::flush;
    std::unique_ptr<File> file(OS::OpenFileForReading(image_filename.c_str()));
    if (file.get() == nullptr) {
      LOG(WARNING) << "Failed to find image in " << image_filename;
    }
    if (file.get() != nullptr) {
      stats_.file_bytes = file->GetLength();
    }
    size_t header_bytes = sizeof(ImageHeader);
    stats_.header_bytes = header_bytes;
    size_t alignment_bytes = RoundUp(header_bytes, kObjectAlignment) - header_bytes;
    stats_.alignment_bytes += alignment_bytes;
    stats_.alignment_bytes += image_header_.GetImageBitmapOffset() - image_header_.GetImageSize();
    stats_.bitmap_bytes += image_header_.GetImageBitmapSize();
    stats_.Dump(os);
    os << "\n";

    os << std::flush;

    oat_dumper_->Dump(os);
  }

 private:
  static void PrettyObjectValue(std::ostream& os, mirror::Class* type, mirror::Object* value)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
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
    } else if (type->IsArtFieldClass()) {
      mirror::ArtField* field = value->AsArtField();
      os << StringPrintf("%p   Field: %s\n", field, PrettyField(field).c_str());
    } else if (type->IsArtMethodClass()) {
      mirror::ArtMethod* method = value->AsArtMethod();
      os << StringPrintf("%p   Method: %s\n", method, PrettyMethod(method).c_str());
    } else {
      os << StringPrintf("%p   %s\n", value, PrettyDescriptor(type).c_str());
    }
  }

  static void PrintField(std::ostream& os, mirror::ArtField* field, mirror::Object* obj)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    const char* descriptor = field->GetTypeDescriptor();
    os << StringPrintf("%s: ", field->GetName());
    if (descriptor[0] != 'L' && descriptor[0] != '[') {
      StackHandleScope<1> hs(Thread::Current());
      FieldHelper fh(hs.NewHandle(field));
      mirror::Class* type = fh.GetType();
      DCHECK(type->IsPrimitive());
      if (type->IsPrimitiveLong()) {
        os << StringPrintf("%" PRId64 " (0x%" PRIx64 ")\n", field->Get64(obj), field->Get64(obj));
      } else if (type->IsPrimitiveDouble()) {
        os << StringPrintf("%f (%a)\n", field->GetDouble(obj), field->GetDouble(obj));
      } else if (type->IsPrimitiveFloat()) {
        os << StringPrintf("%f (%a)\n", field->GetFloat(obj), field->GetFloat(obj));
      } else if (type->IsPrimitiveInt()) {
        os << StringPrintf("%d (0x%x)\n", field->Get32(obj), field->Get32(obj));
      } else if (type->IsPrimitiveChar()) {
        os << StringPrintf("%u (0x%x)\n", field->GetChar(obj), field->GetChar(obj));
      } else if (type->IsPrimitiveShort()) {
        os << StringPrintf("%d (0x%x)\n", field->GetShort(obj), field->GetShort(obj));
      } else if (type->IsPrimitiveBoolean()) {
        os << StringPrintf("%s (0x%x)\n", field->GetBoolean(obj)? "true" : "false",
            field->GetBoolean(obj));
      } else if (type->IsPrimitiveByte()) {
        os << StringPrintf("%d (0x%x)\n", field->GetByte(obj), field->GetByte(obj));
      } else {
        LOG(FATAL) << "Unknown type: " << PrettyClass(type);
      }
    } else {
      // Get the value, don't compute the type unless it is non-null as we don't want
      // to cause class loading.
      mirror::Object* value = field->GetObj(obj);
      if (value == nullptr) {
        os << StringPrintf("null   %s\n", PrettyDescriptor(descriptor).c_str());
      } else {
        // Grab the field type without causing resolution.
        StackHandleScope<1> hs(Thread::Current());
        FieldHelper fh(hs.NewHandle(field));
        mirror::Class* field_type = fh.GetType(false);
        if (field_type != nullptr) {
          PrettyObjectValue(os, field_type, value);
        } else {
          os << StringPrintf("%p   %s\n", value, PrettyDescriptor(descriptor).c_str());
        }
      }
    }
  }

  static void DumpFields(std::ostream& os, mirror::Object* obj, mirror::Class* klass)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    mirror::Class* super = klass->GetSuperClass();
    if (super != nullptr) {
      DumpFields(os, obj, super);
    }
    mirror::ObjectArray<mirror::ArtField>* fields = klass->GetIFields();
    if (fields != nullptr) {
      for (int32_t i = 0; i < fields->GetLength(); i++) {
        mirror::ArtField* field = fields->Get(i);
        PrintField(os, field, obj);
      }
    }
  }

  bool InDumpSpace(const mirror::Object* object) {
    return image_space_.Contains(object);
  }

  const void* GetQuickOatCodeBegin(mirror::ArtMethod* m)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    const void* quick_code = m->GetEntryPointFromQuickCompiledCode();
    if (quick_code == Runtime::Current()->GetClassLinker()->GetQuickResolutionTrampoline()) {
      quick_code = oat_dumper_->GetQuickOatCode(m);
    }
    if (oat_dumper_->GetInstructionSet() == kThumb2) {
      quick_code = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(quick_code) & ~0x1);
    }
    return quick_code;
  }

  uint32_t GetQuickOatCodeSize(mirror::ArtMethod* m)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    const uint32_t* oat_code_begin = reinterpret_cast<const uint32_t*>(GetQuickOatCodeBegin(m));
    if (oat_code_begin == nullptr) {
      return 0;
    }
    return oat_code_begin[-1];
  }

  const void* GetQuickOatCodeEnd(mirror::ArtMethod* m)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    const uint8_t* oat_code_begin = reinterpret_cast<const uint8_t*>(GetQuickOatCodeBegin(m));
    if (oat_code_begin == nullptr) {
      return nullptr;
    }
    return oat_code_begin + GetQuickOatCodeSize(m);
  }

  static void Callback(mirror::Object* obj, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
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

    std::ostream& os = *state->os_;
    mirror::Class* obj_class = obj->GetClass();
    if (obj_class->IsArrayClass()) {
      os << StringPrintf("%p: %s length:%d\n", obj, PrettyDescriptor(obj_class).c_str(),
                         obj->AsArray()->GetLength());
    } else if (obj->IsClass()) {
      mirror::Class* klass = obj->AsClass();
      os << StringPrintf("%p: java.lang.Class \"%s\" (", obj, PrettyDescriptor(klass).c_str())
         << klass->GetStatus() << ")\n";
    } else if (obj->IsArtField()) {
      os << StringPrintf("%p: java.lang.reflect.ArtField %s\n", obj,
                         PrettyField(obj->AsArtField()).c_str());
    } else if (obj->IsArtMethod()) {
      os << StringPrintf("%p: java.lang.reflect.ArtMethod %s\n", obj,
                         PrettyMethod(obj->AsArtMethod()).c_str());
    } else if (obj_class->IsStringClass()) {
      os << StringPrintf("%p: java.lang.String %s\n", obj,
                         PrintableString(obj->AsString()->ToModifiedUtf8().c_str()).c_str());
    } else {
      os << StringPrintf("%p: %s\n", obj, PrettyDescriptor(obj_class).c_str());
    }
    Indenter indent_filter(os.rdbuf(), kIndentChar, kIndentBy1Count);
    std::ostream indent_os(&indent_filter);
    DumpFields(indent_os, obj, obj_class);
    if (obj->IsObjectArray()) {
      mirror::ObjectArray<mirror::Object>* obj_array = obj->AsObjectArray<mirror::Object>();
      int32_t length = obj_array->GetLength();
      for (int32_t i = 0; i < length; i++) {
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
          indent_os << StringPrintf("%d: ", i);
        } else {
          indent_os << StringPrintf("%d to %zd: ", i, i + run);
          i = i + run;
        }
        mirror::Class* value_class =
            (value == nullptr) ? obj_class->GetComponentType() : value->GetClass();
        PrettyObjectValue(indent_os, value_class, value);
      }
    } else if (obj->IsClass()) {
      mirror::ObjectArray<mirror::ArtField>* sfields = obj->AsClass()->GetSFields();
      if (sfields != nullptr) {
        indent_os << "STATICS:\n";
        Indenter indent2_filter(indent_os.rdbuf(), kIndentChar, kIndentBy1Count);
        std::ostream indent2_os(&indent2_filter);
        for (int32_t i = 0; i < sfields->GetLength(); i++) {
          mirror::ArtField* field = sfields->Get(i);
          PrintField(indent2_os, field, field->GetDeclaringClass());
        }
      }
    } else if (obj->IsArtMethod()) {
      mirror::ArtMethod* method = obj->AsArtMethod();
      if (method->IsNative()) {
        // TODO: portable dumping.
        DCHECK(method->GetNativeGcMap() == nullptr) << PrettyMethod(method);
        DCHECK(method->GetMappingTable() == nullptr) << PrettyMethod(method);
        bool first_occurrence;
        const void* quick_oat_code = state->GetQuickOatCodeBegin(method);
        uint32_t quick_oat_code_size = state->GetQuickOatCodeSize(method);
        state->ComputeOatSize(quick_oat_code, &first_occurrence);
        if (first_occurrence) {
          state->stats_.native_to_managed_code_bytes += quick_oat_code_size;
        }
        if (quick_oat_code != method->GetEntryPointFromQuickCompiledCode()) {
          indent_os << StringPrintf("OAT CODE: %p\n", quick_oat_code);
        }
      } else if (method->IsAbstract() || method->IsCalleeSaveMethod() ||
          method->IsResolutionMethod() || method->IsImtConflictMethod() ||
          method->IsClassInitializer()) {
        DCHECK(method->GetNativeGcMap() == nullptr) << PrettyMethod(method);
        DCHECK(method->GetMappingTable() == nullptr) << PrettyMethod(method);
      } else {
        const DexFile::CodeItem* code_item = method->GetCodeItem();
        size_t dex_instruction_bytes = code_item->insns_size_in_code_units_ * 2;
        state->stats_.dex_instruction_bytes += dex_instruction_bytes;

        bool first_occurrence;
        size_t gc_map_bytes = state->ComputeOatSize(method->GetNativeGcMap(), &first_occurrence);
        if (first_occurrence) {
          state->stats_.gc_map_bytes += gc_map_bytes;
        }

        size_t pc_mapping_table_bytes =
            state->ComputeOatSize(method->GetMappingTable(), &first_occurrence);
        if (first_occurrence) {
          state->stats_.pc_mapping_table_bytes += pc_mapping_table_bytes;
        }

        size_t vmap_table_bytes =
            state->ComputeOatSize(method->GetVmapTable(), &first_occurrence);
        if (first_occurrence) {
          state->stats_.vmap_table_bytes += vmap_table_bytes;
        }

        // TODO: portable dumping.
        const void* quick_oat_code_begin = state->GetQuickOatCodeBegin(method);
        const void* quick_oat_code_end = state->GetQuickOatCodeEnd(method);
        uint32_t quick_oat_code_size = state->GetQuickOatCodeSize(method);
        state->ComputeOatSize(quick_oat_code_begin, &first_occurrence);
        if (first_occurrence) {
          state->stats_.managed_code_bytes += quick_oat_code_size;
          if (method->IsConstructor()) {
            if (method->IsStatic()) {
              state->stats_.class_initializer_code_bytes += quick_oat_code_size;
            } else if (dex_instruction_bytes > kLargeConstructorDexBytes) {
              state->stats_.large_initializer_code_bytes += quick_oat_code_size;
            }
          } else if (dex_instruction_bytes > kLargeMethodDexBytes) {
            state->stats_.large_method_code_bytes += quick_oat_code_size;
          }
        }
        state->stats_.managed_code_bytes_ignoring_deduplication += quick_oat_code_size;

        indent_os << StringPrintf("OAT CODE: %p-%p\n", quick_oat_code_begin, quick_oat_code_end);
        indent_os << StringPrintf("SIZE: Dex Instructions=%zd GC=%zd Mapping=%zd\n",
                                  dex_instruction_bytes, gc_map_bytes, pc_mapping_table_bytes);

        size_t total_size = dex_instruction_bytes + gc_map_bytes + pc_mapping_table_bytes +
            vmap_table_bytes + quick_oat_code_size + object_bytes;

        double expansion =
            static_cast<double>(quick_oat_code_size) / static_cast<double>(dex_instruction_bytes);
        state->stats_.ComputeOutliers(total_size, expansion, method);
      }
    }
    std::string temp;
    state->stats_.Update(obj_class->GetDescriptor(&temp), object_bytes);
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
    size_t bitmap_bytes;
    size_t alignment_bytes;

    size_t managed_code_bytes;
    size_t managed_code_bytes_ignoring_deduplication;
    size_t managed_to_native_code_bytes;
    size_t native_to_managed_code_bytes;
    size_t class_initializer_code_bytes;
    size_t large_initializer_code_bytes;
    size_t large_method_code_bytes;

    size_t gc_map_bytes;
    size_t pc_mapping_table_bytes;
    size_t vmap_table_bytes;

    size_t dex_instruction_bytes;

    std::vector<mirror::ArtMethod*> method_outlier;
    std::vector<size_t> method_outlier_size;
    std::vector<double> method_outlier_expansion;
    std::vector<std::pair<std::string, size_t>> oat_dex_file_sizes;

    explicit Stats()
        : oat_file_bytes(0),
          file_bytes(0),
          header_bytes(0),
          object_bytes(0),
          bitmap_bytes(0),
          alignment_bytes(0),
          managed_code_bytes(0),
          managed_code_bytes_ignoring_deduplication(0),
          managed_to_native_code_bytes(0),
          native_to_managed_code_bytes(0),
          class_initializer_code_bytes(0),
          large_initializer_code_bytes(0),
          large_method_code_bytes(0),
          gc_map_bytes(0),
          pc_mapping_table_bytes(0),
          vmap_table_bytes(0),
          dex_instruction_bytes(0) {}

    struct SizeAndCount {
      SizeAndCount(size_t bytes, size_t count) : bytes(bytes), count(count) {}
      size_t bytes;
      size_t count;
    };
    typedef SafeMap<std::string, SizeAndCount> SizeAndCountTable;
    SizeAndCountTable sizes_and_counts;

    void Update(const char* descriptor, size_t object_bytes) {
      SizeAndCountTable::iterator it = sizes_and_counts.find(descriptor);
      if (it != sizes_and_counts.end()) {
        it->second.bytes += object_bytes;
        it->second.count += 1;
      } else {
        sizes_and_counts.Put(descriptor, SizeAndCount(object_bytes, 1));
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

    void ComputeOutliers(size_t total_size, double expansion, mirror::ArtMethod* method) {
      method_outlier_size.push_back(total_size);
      method_outlier_expansion.push_back(expansion);
      method_outlier.push_back(method);
    }

    void DumpOutliers(std::ostream& os)
        SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
      size_t sum_of_sizes = 0;
      size_t sum_of_sizes_squared = 0;
      size_t sum_of_expansion = 0;
      size_t sum_of_expansion_squared = 0;
      size_t n = method_outlier_size.size();
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

    void Dump(std::ostream& os) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
      {
        os << "art_file_bytes = " << PrettySize(file_bytes) << "\n\n"
           << "art_file_bytes = header_bytes + object_bytes + alignment_bytes\n";
        Indenter indent_filter(os.rdbuf(), kIndentChar, kIndentBy1Count);
        std::ostream indent_os(&indent_filter);
        indent_os << StringPrintf("header_bytes    =  %8zd (%2.0f%% of art file bytes)\n"
                                  "object_bytes    =  %8zd (%2.0f%% of art file bytes)\n"
                                  "bitmap_bytes    =  %8zd (%2.0f%% of art file bytes)\n"
                                  "alignment_bytes =  %8zd (%2.0f%% of art file bytes)\n\n",
                                  header_bytes, PercentOfFileBytes(header_bytes),
                                  object_bytes, PercentOfFileBytes(object_bytes),
                                  bitmap_bytes, PercentOfFileBytes(bitmap_bytes),
                                  alignment_bytes, PercentOfFileBytes(alignment_bytes))
            << std::flush;
        CHECK_EQ(file_bytes, bitmap_bytes + header_bytes + object_bytes + alignment_bytes);
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

      os << "\n" << StringPrintf("gc_map_bytes           = %7zd (%2.0f%% of oat file bytes)\n"
                                 "pc_mapping_table_bytes = %7zd (%2.0f%% of oat file bytes)\n"
                                 "vmap_table_bytes       = %7zd (%2.0f%% of oat file bytes)\n\n",
                                 gc_map_bytes, PercentOfOatBytes(gc_map_bytes),
                                 pc_mapping_table_bytes, PercentOfOatBytes(pc_mapping_table_bytes),
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
  std::unique_ptr<OatDumper> oat_dumper_;
  std::ostream* os_;
  gc::space::ImageSpace& image_space_;
  const ImageHeader& image_header_;
  bool dump_raw_mapping_table_;
  bool dump_raw_gc_map_;

  DISALLOW_COPY_AND_ASSIGN(ImageDumper);
};

static int oatdump(int argc, char** argv) {
  InitLogging(argv);

  // Skip over argv[0].
  argv++;
  argc--;

  if (argc == 0) {
    fprintf(stderr, "No arguments specified\n");
    usage();
  }

  const char* oat_filename = nullptr;
  const char* image_location = nullptr;
  const char* boot_image_location = nullptr;
  InstructionSet instruction_set = kRuntimeISA;
  std::string elf_filename_prefix;
  std::ostream* os = &std::cout;
  std::unique_ptr<std::ofstream> out;
  std::string output_name;
  bool dump_raw_mapping_table = false;
  bool dump_raw_gc_map = false;
  bool symbolize = false;

  for (int i = 0; i < argc; i++) {
    const StringPiece option(argv[i]);
    if (option.starts_with("--oat-file=")) {
      oat_filename = option.substr(strlen("--oat-file=")).data();
    } else if (option.starts_with("--image=")) {
      image_location = option.substr(strlen("--image=")).data();
    } else if (option.starts_with("--boot-image=")) {
      boot_image_location = option.substr(strlen("--boot-image=")).data();
    } else if (option.starts_with("--instruction-set=")) {
      StringPiece instruction_set_str = option.substr(strlen("--instruction-set=")).data();
      if (instruction_set_str == "arm") {
        instruction_set = kThumb2;
      } else if (instruction_set_str == "arm64") {
        instruction_set = kArm64;
      } else if (instruction_set_str == "mips") {
        instruction_set = kMips;
      } else if (instruction_set_str == "x86") {
        instruction_set = kX86;
      } else if (instruction_set_str == "x86_64") {
        instruction_set = kX86_64;
      }
    } else if (option.starts_with("--dump:")) {
        if (option == "--dump:raw_mapping_table") {
          dump_raw_mapping_table = true;
        } else if (option == "--dump:raw_gc_map") {
          dump_raw_gc_map = true;
        } else {
          fprintf(stderr, "Unknown argument %s\n", option.data());
          usage();
        }
    } else if (option.starts_with("--output=")) {
      output_name = option.substr(strlen("--output=")).ToString();
      const char* filename = output_name.c_str();
      out.reset(new std::ofstream(filename));
      if (!out->good()) {
        fprintf(stderr, "Failed to open output filename %s\n", filename);
        usage();
      }
      os = out.get();
    } else if (option.starts_with("--symbolize=")) {
      oat_filename = option.substr(strlen("--symbolize=")).data();
      symbolize = true;
    } else {
      fprintf(stderr, "Unknown argument %s\n", option.data());
      usage();
    }
  }

  if (image_location == nullptr && oat_filename == nullptr) {
    fprintf(stderr, "Either --image or --oat must be specified\n");
    return EXIT_FAILURE;
  }

  if (image_location != nullptr && oat_filename != nullptr) {
    fprintf(stderr, "Either --image or --oat must be specified but not both\n");
    return EXIT_FAILURE;
  }

  if (oat_filename != nullptr) {
    std::string error_msg;
    OatFile* oat_file =
        OatFile::Open(oat_filename, oat_filename, nullptr, false, &error_msg);
    if (oat_file == nullptr) {
      fprintf(stderr, "Failed to open oat file from '%s': %s\n", oat_filename, error_msg.c_str());
      return EXIT_FAILURE;
    }
    if (symbolize) {
      OatSymbolizer oat_symbolizer(oat_file, output_name);
      if (!oat_symbolizer.Init()) {
        fprintf(stderr, "Failed to initialize symbolizer\n");
        return EXIT_FAILURE;
      }
      if (!oat_symbolizer.Symbolize()) {
        fprintf(stderr, "Failed to symbolize\n");
        return EXIT_FAILURE;
      }
    } else {
      OatDumper oat_dumper(*oat_file, dump_raw_mapping_table, dump_raw_gc_map);
      oat_dumper.Dump(*os);
    }
    return EXIT_SUCCESS;
  }

  RuntimeOptions options;
  std::string image_option;
  std::string oat_option;
  std::string boot_image_option;
  std::string boot_oat_option;

  // We are more like a compiler than a run-time. We don't want to execute code.
  NoopCompilerCallbacks callbacks;
  options.push_back(std::make_pair("compilercallbacks", &callbacks));

  if (boot_image_location != nullptr) {
    boot_image_option += "-Ximage:";
    boot_image_option += boot_image_location;
    options.push_back(std::make_pair(boot_image_option.c_str(), nullptr));
  }
  if (image_location != nullptr) {
    image_option += "-Ximage:";
    image_option += image_location;
    options.push_back(std::make_pair(image_option.c_str(), nullptr));
  }
  options.push_back(
      std::make_pair("imageinstructionset",
                     reinterpret_cast<const void*>(GetInstructionSetString(instruction_set))));

  if (!Runtime::Create(options, false)) {
    fprintf(stderr, "Failed to create runtime\n");
    return EXIT_FAILURE;
  }
  std::unique_ptr<Runtime> runtime(Runtime::Current());
  // Runtime::Create acquired the mutator_lock_ that is normally given away when we Runtime::Start,
  // give it away now and then switch to a more manageable ScopedObjectAccess.
  Thread::Current()->TransitionFromRunnableToSuspended(kNative);
  ScopedObjectAccess soa(Thread::Current());
  gc::Heap* heap = Runtime::Current()->GetHeap();
  gc::space::ImageSpace* image_space = heap->GetImageSpace();
  CHECK(image_space != nullptr);
  const ImageHeader& image_header = image_space->GetImageHeader();
  if (!image_header.IsValid()) {
    fprintf(stderr, "Invalid image header %s\n", image_location);
    return EXIT_FAILURE;
  }
  ImageDumper image_dumper(os, *image_space, image_header,
                           dump_raw_mapping_table, dump_raw_gc_map);
  image_dumper.Dump();
  return EXIT_SUCCESS;
}

}  // namespace art

int main(int argc, char** argv) {
  return art::oatdump(argc, argv);
}
