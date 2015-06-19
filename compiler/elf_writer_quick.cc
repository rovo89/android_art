/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include "elf_writer_quick.h"

#include <unordered_map>
#include <unordered_set>

#include "base/casts.h"
#include "base/logging.h"
#include "base/unix_file/fd_file.h"
#include "compiled_method.h"
#include "dex_file-inl.h"
#include "driver/compiler_driver.h"
#include "driver/compiler_options.h"
#include "elf_builder.h"
#include "elf_file.h"
#include "elf_utils.h"
#include "elf_writer_debug.h"
#include "globals.h"
#include "leb128.h"
#include "oat.h"
#include "oat_writer.h"
#include "utils.h"

namespace art {

// .eh_frame and .debug_frame are almost identical.
// Except for some minor formatting differences, the main difference
// is that .eh_frame is allocated within the running program because
// it is used by C++ exception handling (which we do not use so we
// can choose either).  C++ compilers generally tend to use .eh_frame
// because if they need it sometimes, they might as well always use it.
constexpr dwarf::CFIFormat kCFIFormat = dwarf::DW_EH_FRAME_FORMAT;

// The ARM specification defines three special mapping symbols
// $a, $t and $d which mark ARM, Thumb and data ranges respectively.
// These symbols can be used by tools, for example, to pretty
// print instructions correctly.  Objdump will use them if they
// exist, but it will still work well without them.
// However, these extra symbols take space, so let's just generate
// one symbol which marks the whole .text section as code.
constexpr bool kGenerateSingleArmMappingSymbol = true;

template <typename ElfTypes>
bool ElfWriterQuick<ElfTypes>::Create(File* elf_file,
                                      OatWriter* oat_writer,
                                      const std::vector<const DexFile*>& dex_files,
                                      const std::string& android_root,
                                      bool is_host,
                                      const CompilerDriver& driver) {
  ElfWriterQuick elf_writer(driver, elf_file);
  return elf_writer.Write(oat_writer, dex_files, android_root, is_host);
}

template <typename ElfTypes>
static void WriteDebugSymbols(ElfBuilder<ElfTypes>* builder, OatWriter* oat_writer);

// Encode patch locations as LEB128 list of deltas between consecutive addresses.
template <typename ElfTypes>
void ElfWriterQuick<ElfTypes>::EncodeOatPatches(const std::vector<uintptr_t>& locations,
                                                std::vector<uint8_t>* buffer) {
  buffer->reserve(buffer->size() + locations.size() * 2);  // guess 2 bytes per ULEB128.
  uintptr_t address = 0;  // relative to start of section.
  for (uintptr_t location : locations) {
    DCHECK_GE(location, address) << "Patch locations are not in sorted order";
    EncodeUnsignedLeb128(buffer, dchecked_integral_cast<uint32_t>(location - address));
    address = location;
  }
}

class RodataWriter FINAL : public CodeOutput {
 public:
  explicit RodataWriter(OatWriter* oat_writer) : oat_writer_(oat_writer) {}

  bool Write(OutputStream* out) OVERRIDE {
    return oat_writer_->WriteRodata(out);
  }

 private:
  OatWriter* oat_writer_;
};

class TextWriter FINAL : public CodeOutput {
 public:
  explicit TextWriter(OatWriter* oat_writer) : oat_writer_(oat_writer) {}

  bool Write(OutputStream* out) OVERRIDE {
    return oat_writer_->WriteCode(out);
  }

 private:
  OatWriter* oat_writer_;
};

enum PatchResult {
  kAbsoluteAddress,  // Absolute memory location.
  kPointerRelativeAddress,  // Offset relative to the location of the pointer.
  kSectionRelativeAddress,  // Offset relative to start of containing section.
};

// Patch memory addresses within a buffer.
// It assumes that the unpatched addresses are offsets relative to base_address.
// (which generally means method's low_pc relative to the start of .text)
template <typename Elf_Addr, typename Address, PatchResult kPatchResult>
static void Patch(const std::vector<uintptr_t>& patch_locations,
                  Elf_Addr buffer_address, Elf_Addr base_address,
                  std::vector<uint8_t>* buffer) {
  for (uintptr_t location : patch_locations) {
    typedef __attribute__((__aligned__(1))) Address UnalignedAddress;
    auto* to_patch = reinterpret_cast<UnalignedAddress*>(buffer->data() + location);
    switch (kPatchResult) {
      case kAbsoluteAddress:
        *to_patch = (base_address + *to_patch);
        break;
      case kPointerRelativeAddress:
        *to_patch = (base_address + *to_patch) - (buffer_address + location);
        break;
      case kSectionRelativeAddress:
        *to_patch = (base_address + *to_patch) - buffer_address;
        break;
    }
  }
}

template <typename ElfTypes>
bool ElfWriterQuick<ElfTypes>::Write(
    OatWriter* oat_writer,
    const std::vector<const DexFile*>& dex_files_unused ATTRIBUTE_UNUSED,
    const std::string& android_root_unused ATTRIBUTE_UNUSED,
    bool is_host_unused ATTRIBUTE_UNUSED) {
  using Elf_Addr = typename ElfTypes::Addr;
  const InstructionSet isa = compiler_driver_->GetInstructionSet();

  // Setup the builder with the main OAT sections (.rodata .text .bss).
  const size_t rodata_size = oat_writer->GetOatHeader().GetExecutableOffset();
  const size_t text_size = oat_writer->GetSize() - rodata_size;
  const size_t bss_size = oat_writer->GetBssSize();
  RodataWriter rodata_writer(oat_writer);
  TextWriter text_writer(oat_writer);
  std::unique_ptr<ElfBuilder<ElfTypes>> builder(new ElfBuilder<ElfTypes>(
      isa, rodata_size, &rodata_writer, text_size, &text_writer, bss_size));

  // Add debug sections.
  // They are allocated here (in the same scope as the builder),
  // but they are registered with the builder only if they are used.
  using RawSection = typename ElfBuilder<ElfTypes>::RawSection;
  const auto* text = builder->GetText();
  const bool is64bit = Is64BitInstructionSet(isa);
  const int pointer_size = GetInstructionSetPointerSize(isa);
  std::unique_ptr<RawSection> eh_frame(new RawSection(
      ".eh_frame", SHT_PROGBITS, SHF_ALLOC, nullptr, 0, kPageSize, 0,
      is64bit ? Patch<Elf_Addr, uint64_t, kPointerRelativeAddress> :
                Patch<Elf_Addr, uint32_t, kPointerRelativeAddress>,
      text));
  std::unique_ptr<RawSection> eh_frame_hdr(new RawSection(
      ".eh_frame_hdr", SHT_PROGBITS, SHF_ALLOC, nullptr, 0, 4, 0,
      Patch<Elf_Addr, uint32_t, kSectionRelativeAddress>, text));
  std::unique_ptr<RawSection> debug_frame(new RawSection(
      ".debug_frame", SHT_PROGBITS, 0, nullptr, 0, pointer_size, 0,
      is64bit ? Patch<Elf_Addr, uint64_t, kAbsoluteAddress> :
                Patch<Elf_Addr, uint32_t, kAbsoluteAddress>,
      text));
  std::unique_ptr<RawSection> debug_frame_oat_patches(new RawSection(
      ".debug_frame.oat_patches", SHT_OAT_PATCH));
  std::unique_ptr<RawSection> debug_info(new RawSection(
      ".debug_info", SHT_PROGBITS, 0, nullptr, 0, 1, 0,
      Patch<Elf_Addr, uint32_t, kAbsoluteAddress>, text));
  std::unique_ptr<RawSection> debug_info_oat_patches(new RawSection(
      ".debug_info.oat_patches", SHT_OAT_PATCH));
  std::unique_ptr<RawSection> debug_abbrev(new RawSection(
      ".debug_abbrev", SHT_PROGBITS));
  std::unique_ptr<RawSection> debug_str(new RawSection(
      ".debug_str", SHT_PROGBITS));
  std::unique_ptr<RawSection> debug_line(new RawSection(
      ".debug_line", SHT_PROGBITS, 0, nullptr, 0, 1, 0,
      Patch<Elf_Addr, uint32_t, kAbsoluteAddress>, text));
  std::unique_ptr<RawSection> debug_line_oat_patches(new RawSection(
      ".debug_line.oat_patches", SHT_OAT_PATCH));
  if (!oat_writer->GetMethodDebugInfo().empty()) {
    if (compiler_driver_->GetCompilerOptions().GetGenerateDebugInfo()) {
      // Generate CFI (stack unwinding information).
      if (kCFIFormat == dwarf::DW_EH_FRAME_FORMAT) {
        dwarf::WriteCFISection(
            compiler_driver_, oat_writer,
            dwarf::DW_EH_PE_pcrel, kCFIFormat,
            eh_frame->GetBuffer(), eh_frame->GetPatchLocations(),
            eh_frame_hdr->GetBuffer(), eh_frame_hdr->GetPatchLocations());
        builder->RegisterSection(eh_frame.get());
        builder->RegisterSection(eh_frame_hdr.get());
      } else {
        DCHECK(kCFIFormat == dwarf::DW_DEBUG_FRAME_FORMAT);
        dwarf::WriteCFISection(
            compiler_driver_, oat_writer,
            dwarf::DW_EH_PE_absptr, kCFIFormat,
            debug_frame->GetBuffer(), debug_frame->GetPatchLocations(),
            nullptr, nullptr);
        builder->RegisterSection(debug_frame.get());
        EncodeOatPatches(*debug_frame->GetPatchLocations(),
                         debug_frame_oat_patches->GetBuffer());
        builder->RegisterSection(debug_frame_oat_patches.get());
      }
      // Add methods to .symtab.
      WriteDebugSymbols(builder.get(), oat_writer);
      // Generate DWARF .debug_* sections.
      dwarf::WriteDebugSections(
          compiler_driver_, oat_writer,
          debug_info->GetBuffer(), debug_info->GetPatchLocations(),
          debug_abbrev->GetBuffer(),
          debug_str->GetBuffer(),
          debug_line->GetBuffer(), debug_line->GetPatchLocations());
      builder->RegisterSection(debug_info.get());
      EncodeOatPatches(*debug_info->GetPatchLocations(),
                       debug_info_oat_patches->GetBuffer());
      builder->RegisterSection(debug_info_oat_patches.get());
      builder->RegisterSection(debug_abbrev.get());
      builder->RegisterSection(debug_str.get());
      builder->RegisterSection(debug_line.get());
      EncodeOatPatches(*debug_line->GetPatchLocations(),
                       debug_line_oat_patches->GetBuffer());
      builder->RegisterSection(debug_line_oat_patches.get());
    }
  }

  // Add relocation section for .text.
  std::unique_ptr<RawSection> text_oat_patches(new RawSection(
      ".text.oat_patches", SHT_OAT_PATCH));
  if (compiler_driver_->GetCompilerOptions().GetIncludePatchInformation()) {
    // Note that ElfWriter::Fixup will be called regardless and therefore
    // we need to include oat_patches for debug sections unconditionally.
    EncodeOatPatches(oat_writer->GetAbsolutePatchLocations(),
                     text_oat_patches->GetBuffer());
    builder->RegisterSection(text_oat_patches.get());
  }

  return builder->Write(elf_file_);
}

template <typename ElfTypes>
static void WriteDebugSymbols(ElfBuilder<ElfTypes>* builder, OatWriter* oat_writer) {
  const std::vector<OatWriter::DebugInfo>& method_info = oat_writer->GetMethodDebugInfo();
  bool generated_mapping_symbol = false;

  // Find all addresses (low_pc) which contain deduped methods.
  // The first instance of method is not marked deduped_, but the rest is.
  std::unordered_set<uint32_t> deduped_addresses;
  for (auto it = method_info.begin(); it != method_info.end(); ++it) {
    if (it->deduped_) {
      deduped_addresses.insert(it->low_pc_);
    }
  }

  auto* symtab = builder->GetSymtab();
  for (auto it = method_info.begin(); it != method_info.end(); ++it) {
    if (it->deduped_) {
      continue;  // Add symbol only for the first instance.
    }
    std::string name = PrettyMethod(it->dex_method_index_, *it->dex_file_, true);
    if (deduped_addresses.find(it->low_pc_) != deduped_addresses.end()) {
      name += " [DEDUPED]";
    }

    uint32_t low_pc = it->low_pc_;
    // Add in code delta, e.g., thumb bit 0 for Thumb2 code.
    low_pc += it->compiled_method_->CodeDelta();
    symtab->AddSymbol(name, builder->GetText(), low_pc,
                      true, it->high_pc_ - it->low_pc_, STB_GLOBAL, STT_FUNC);

    // Conforming to aaelf, add $t mapping symbol to indicate start of a sequence of thumb2
    // instructions, so that disassembler tools can correctly disassemble.
    // Note that even if we generate just a single mapping symbol, ARM's Streamline
    // requires it to match function symbol.  Just address 0 does not work.
    if (it->compiled_method_->GetInstructionSet() == kThumb2) {
      if (!generated_mapping_symbol || !kGenerateSingleArmMappingSymbol) {
        symtab->AddSymbol("$t", builder->GetText(), it->low_pc_ & ~1, true,
                          0, STB_LOCAL, STT_NOTYPE);
        generated_mapping_symbol = true;
      }
    }
  }
}

// Explicit instantiations
template class ElfWriterQuick<ElfTypes32>;
template class ElfWriterQuick<ElfTypes64>;

}  // namespace art
