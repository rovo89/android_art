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

#include "base/logging.h"
#include "base/unix_file/fd_file.h"
#include "buffered_output_stream.h"
#include "compiled_method.h"
#include "dex_file-inl.h"
#include "driver/compiler_driver.h"
#include "driver/compiler_options.h"
#include "elf_builder.h"
#include "elf_file.h"
#include "elf_utils.h"
#include "elf_writer_debug.h"
#include "file_output_stream.h"
#include "globals.h"
#include "leb128.h"
#include "oat.h"
#include "oat_writer.h"
#include "utils.h"

namespace art {

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

class OatWriterWrapper FINAL : public CodeOutput {
 public:
  explicit OatWriterWrapper(OatWriter* oat_writer) : oat_writer_(oat_writer) {}

  void SetCodeOffset(size_t offset) {
    oat_writer_->SetOatDataOffset(offset);
  }
  bool Write(OutputStream* out) OVERRIDE {
    return oat_writer_->Write(out);
  }
 private:
  OatWriter* const oat_writer_;
};

template <typename ElfTypes>
static void WriteDebugSymbols(ElfBuilder<ElfTypes>* builder, OatWriter* oat_writer);

// Encode patch locations in .oat_patches format.
template <typename ElfTypes>
void ElfWriterQuick<ElfTypes>::EncodeOatPatches(
    const OatWriter::PatchLocationsMap& sections,
    std::vector<uint8_t>* buffer) {
  for (const auto& section : sections) {
    const std::string& name = section.first;
    std::vector<uintptr_t>* locations = section.second.get();
    DCHECK(!name.empty());
    std::sort(locations->begin(), locations->end());
    // Reserve buffer space - guess 2 bytes per ULEB128.
    buffer->reserve(buffer->size() + name.size() + locations->size() * 2);
    // Write null-terminated section name.
    const uint8_t* name_data = reinterpret_cast<const uint8_t*>(name.c_str());
    buffer->insert(buffer->end(), name_data, name_data + name.size() + 1);
    // Write placeholder for data length.
    size_t length_pos = buffer->size();
    EncodeUnsignedLeb128(buffer, UINT32_MAX);
    // Write LEB128 encoded list of advances (deltas between consequtive addresses).
    size_t data_pos = buffer->size();
    uintptr_t address = 0;  // relative to start of section.
    for (uintptr_t location : *locations) {
      DCHECK_LT(location - address, UINT32_MAX) << "Large gap between patch locations";
      EncodeUnsignedLeb128(buffer, location - address);
      address = location;
    }
    // Update length.
    UpdateUnsignedLeb128(buffer->data() + length_pos, buffer->size() - data_pos);
  }
  buffer->push_back(0);  // End of sections.
}

template<typename AddressType, bool SubtractPatchLocation = false>
static void PatchAddresses(const std::vector<uintptr_t>* patch_locations,
                           AddressType delta, std::vector<uint8_t>* buffer) {
  // Addresses in .debug_* sections are unaligned.
  typedef __attribute__((__aligned__(1))) AddressType UnalignedAddressType;
  if (patch_locations != nullptr) {
    for (uintptr_t patch_location : *patch_locations) {
      *reinterpret_cast<UnalignedAddressType*>(buffer->data() + patch_location) +=
          delta - (SubtractPatchLocation ? patch_location : 0);
    }
  }
}

template <typename ElfTypes>
bool ElfWriterQuick<ElfTypes>::Write(
    OatWriter* oat_writer,
    const std::vector<const DexFile*>& dex_files_unused ATTRIBUTE_UNUSED,
    const std::string& android_root_unused ATTRIBUTE_UNUSED,
    bool is_host_unused ATTRIBUTE_UNUSED) {
  constexpr bool debug = false;
  const OatHeader& oat_header = oat_writer->GetOatHeader();
  typename ElfTypes::Word oat_data_size = oat_header.GetExecutableOffset();
  uint32_t oat_exec_size = oat_writer->GetSize() - oat_data_size;
  uint32_t oat_bss_size = oat_writer->GetBssSize();

  OatWriterWrapper wrapper(oat_writer);

  std::unique_ptr<ElfBuilder<ElfTypes>> builder(new ElfBuilder<ElfTypes>(
      &wrapper,
      elf_file_,
      compiler_driver_->GetInstructionSet(),
      0,
      oat_data_size,
      oat_data_size,
      oat_exec_size,
      RoundUp(oat_data_size + oat_exec_size, kPageSize),
      oat_bss_size,
      compiler_driver_->GetCompilerOptions().GetIncludeDebugSymbols(),
      debug));

  InstructionSet isa = compiler_driver_->GetInstructionSet();
  int alignment = GetInstructionSetPointerSize(isa);
  typedef ElfRawSectionBuilder<ElfTypes> RawSection;
  RawSection eh_frame(".eh_frame", SHT_PROGBITS, SHF_ALLOC, nullptr, 0, alignment, 0);
  RawSection eh_frame_hdr(".eh_frame_hdr", SHT_PROGBITS, SHF_ALLOC, nullptr, 0, 4, 0);
  RawSection debug_info(".debug_info", SHT_PROGBITS, 0, nullptr, 0, 1, 0);
  RawSection debug_abbrev(".debug_abbrev", SHT_PROGBITS, 0, nullptr, 0, 1, 0);
  RawSection debug_str(".debug_str", SHT_PROGBITS, 0, nullptr, 0, 1, 0);
  RawSection debug_line(".debug_line", SHT_PROGBITS, 0, nullptr, 0, 1, 0);
  RawSection oat_patches(".oat_patches", SHT_OAT_PATCH, 0, NULL, 0, 1, 0);

  // Do not add to .oat_patches since we will make the addresses relative.
  std::vector<uintptr_t> eh_frame_patches;
  if (compiler_driver_->GetCompilerOptions().GetIncludeCFI() &&
      !oat_writer->GetMethodDebugInfo().empty()) {
    dwarf::WriteEhFrame(compiler_driver_, oat_writer,
                        dwarf::DW_EH_PE_pcrel,
                        eh_frame.GetBuffer(), &eh_frame_patches,
                        eh_frame_hdr.GetBuffer());
    builder->RegisterRawSection(&eh_frame);
    builder->RegisterRawSection(&eh_frame_hdr);
  }

  // Must be done after .eh_frame is created since it is used in the Elf layout.
  if (!builder->Init()) {
    return false;
  }

  std::vector<uintptr_t>* debug_info_patches = nullptr;
  std::vector<uintptr_t>* debug_line_patches = nullptr;
  if (compiler_driver_->GetCompilerOptions().GetIncludeDebugSymbols() &&
      !oat_writer->GetMethodDebugInfo().empty()) {
    // Add methods to .symtab.
    WriteDebugSymbols(builder.get(), oat_writer);
    // Generate DWARF .debug_* sections.
    debug_info_patches = oat_writer->GetAbsolutePatchLocationsFor(".debug_info");
    debug_line_patches = oat_writer->GetAbsolutePatchLocationsFor(".debug_line");
    dwarf::WriteDebugSections(compiler_driver_, oat_writer,
                              debug_info.GetBuffer(), debug_info_patches,
                              debug_abbrev.GetBuffer(),
                              debug_str.GetBuffer(),
                              debug_line.GetBuffer(), debug_line_patches);
    builder->RegisterRawSection(&debug_info);
    builder->RegisterRawSection(&debug_abbrev);
    builder->RegisterRawSection(&debug_str);
    builder->RegisterRawSection(&debug_line);
  }

  if (compiler_driver_->GetCompilerOptions().GetIncludePatchInformation() ||
      // ElfWriter::Fixup will be called regardless and it needs to be able
      // to patch debug sections so we have to include patches for them.
      compiler_driver_->GetCompilerOptions().GetIncludeDebugSymbols()) {
    EncodeOatPatches(oat_writer->GetAbsolutePatchLocations(), oat_patches.GetBuffer());
    builder->RegisterRawSection(&oat_patches);
  }

  // We know where .text and .eh_frame will be located, so patch the addresses.
  typename ElfTypes::Addr text_addr = builder->GetTextBuilder().GetSection()->sh_addr;
  // TODO: Simplify once we use Elf64 - we can use ElfTypes::Addr instead of branching.
  if (Is64BitInstructionSet(compiler_driver_->GetInstructionSet())) {
    // relative_address = (text_addr + address) - (eh_frame_addr + patch_location);
    PatchAddresses<uint64_t, true>(&eh_frame_patches,
        text_addr - eh_frame.GetSection()->sh_addr, eh_frame.GetBuffer());
    PatchAddresses<uint64_t>(debug_info_patches, text_addr, debug_info.GetBuffer());
    PatchAddresses<uint64_t>(debug_line_patches, text_addr, debug_line.GetBuffer());
  } else {
    // relative_address = (text_addr + address) - (eh_frame_addr + patch_location);
    PatchAddresses<uint32_t, true>(&eh_frame_patches,
        text_addr - eh_frame.GetSection()->sh_addr, eh_frame.GetBuffer());
    PatchAddresses<uint32_t>(debug_info_patches, text_addr, debug_info.GetBuffer());
    PatchAddresses<uint32_t>(debug_line_patches, text_addr, debug_line.GetBuffer());
  }

  return builder->Write();
}

template <typename ElfTypes>
// Do not inline to avoid Clang stack frame problems. b/18738594
NO_INLINE
static void WriteDebugSymbols(ElfBuilder<ElfTypes>* builder, OatWriter* oat_writer) {
  const std::vector<OatWriter::DebugInfo>& method_info = oat_writer->GetMethodDebugInfo();

  // Find all addresses (low_pc) which contain deduped methods.
  // The first instance of method is not marked deduped_, but the rest is.
  std::unordered_set<uint32_t> deduped_addresses;
  for (auto it = method_info.begin(); it != method_info.end(); ++it) {
    if (it->deduped_) {
      deduped_addresses.insert(it->low_pc_);
    }
  }

  ElfSymtabBuilder<ElfTypes>* symtab = builder->GetSymtabBuilder();
  for (auto it = method_info.begin(); it != method_info.end(); ++it) {
    std::string name = PrettyMethod(it->dex_method_index_, *it->dex_file_, true);
    if (deduped_addresses.find(it->low_pc_) != deduped_addresses.end()) {
      name += " [DEDUPED]";
    }

    uint32_t low_pc = it->low_pc_;
    // Add in code delta, e.g., thumb bit 0 for Thumb2 code.
    low_pc += it->compiled_method_->CodeDelta();
    symtab->AddSymbol(name, &builder->GetTextBuilder(), low_pc,
                      true, it->high_pc_ - it->low_pc_, STB_GLOBAL, STT_FUNC);

    // Conforming to aaelf, add $t mapping symbol to indicate start of a sequence of thumb2
    // instructions, so that disassembler tools can correctly disassemble.
    if (it->compiled_method_->GetInstructionSet() == kThumb2) {
      symtab->AddSymbol("$t", &builder->GetTextBuilder(), it->low_pc_ & ~1, true,
                        0, STB_LOCAL, STT_NOTYPE);
    }
  }
}

// Explicit instantiations
template class ElfWriterQuick<ElfTypes32>;
template class ElfWriterQuick<ElfTypes64>;

}  // namespace art
