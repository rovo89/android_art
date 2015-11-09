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
// Let's use .debug_frame because it is easier to strip or compress.
constexpr dwarf::CFIFormat kCFIFormat = dwarf::DW_DEBUG_FRAME_FORMAT;

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

template <typename ElfTypes>
bool ElfWriterQuick<ElfTypes>::Write(
    OatWriter* oat_writer,
    const std::vector<const DexFile*>& dex_files_unused ATTRIBUTE_UNUSED,
    const std::string& android_root_unused ATTRIBUTE_UNUSED,
    bool is_host_unused ATTRIBUTE_UNUSED) {
  const InstructionSet isa = compiler_driver_->GetInstructionSet();
  std::unique_ptr<BufferedOutputStream> output_stream(
      new BufferedOutputStream(new FileOutputStream(elf_file_)));
  std::unique_ptr<ElfBuilder<ElfTypes>> builder(
      new ElfBuilder<ElfTypes>(isa, output_stream.get()));

  builder->Start();

  auto* rodata = builder->GetRoData();
  auto* text = builder->GetText();
  auto* bss = builder->GetBss();

  rodata->Start();
  if (!oat_writer->WriteRodata(rodata)) {
    return false;
  }
  rodata->End();

  text->Start();
  if (!oat_writer->WriteCode(text)) {
    return false;
  }
  text->End();

  if (oat_writer->GetBssSize() != 0) {
    bss->Start();
    bss->SetSize(oat_writer->GetBssSize());
    bss->End();
  }

  builder->WriteDynamicSection(elf_file_->GetPath());

  if (compiler_driver_->GetCompilerOptions().GetGenerateDebugInfo()) {
    const auto& method_infos = oat_writer->GetMethodDebugInfo();
    if (!method_infos.empty()) {
      // Add methods to .symtab.
      WriteDebugSymbols(builder.get(), oat_writer);
      // Generate CFI (stack unwinding information).
      dwarf::WriteCFISection(builder.get(), method_infos, kCFIFormat);
      // Write DWARF .debug_* sections.
      dwarf::WriteDebugSections(builder.get(), method_infos);
    }
  }

  // Add relocation section for .text.
  if (compiler_driver_->GetCompilerOptions().GetIncludePatchInformation()) {
    // Note that ElfWriter::Fixup will be called regardless and therefore
    // we need to include oat_patches for debug sections unconditionally.
    builder->WritePatches(".text.oat_patches", &oat_writer->GetAbsolutePatchLocations());
  }

  builder->End();

  return builder->Good() && output_stream->Flush();
}

template <typename ElfTypes>
static void WriteDebugSymbols(ElfBuilder<ElfTypes>* builder, OatWriter* oat_writer) {
  const std::vector<OatWriter::DebugInfo>& method_info = oat_writer->GetMethodDebugInfo();
  bool generated_mapping_symbol = false;
  auto* strtab = builder->GetStrTab();
  auto* symtab = builder->GetSymTab();

  if (method_info.empty()) {
    return;
  }

  // Find all addresses (low_pc) which contain deduped methods.
  // The first instance of method is not marked deduped_, but the rest is.
  std::unordered_set<uint32_t> deduped_addresses;
  for (auto it = method_info.begin(); it != method_info.end(); ++it) {
    if (it->deduped_) {
      deduped_addresses.insert(it->low_pc_);
    }
  }

  strtab->Start();
  strtab->Write("");  // strtab should start with empty string.
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
    symtab->Add(strtab->Write(name), builder->GetText(), low_pc,
                true, it->high_pc_ - it->low_pc_, STB_GLOBAL, STT_FUNC);

    // Conforming to aaelf, add $t mapping symbol to indicate start of a sequence of thumb2
    // instructions, so that disassembler tools can correctly disassemble.
    // Note that even if we generate just a single mapping symbol, ARM's Streamline
    // requires it to match function symbol.  Just address 0 does not work.
    if (it->compiled_method_->GetInstructionSet() == kThumb2) {
      if (!generated_mapping_symbol || !kGenerateSingleArmMappingSymbol) {
        symtab->Add(strtab->Write("$t"), builder->GetText(), it->low_pc_ & ~1,
                    true, 0, STB_LOCAL, STT_NOTYPE);
        generated_mapping_symbol = true;
      }
    }
  }
  strtab->End();

  // Symbols are buffered and written after names (because they are smaller).
  // We could also do two passes in this function to avoid the buffering.
  symtab->Start();
  symtab->Write();
  symtab->End();
}

// Explicit instantiations
template class ElfWriterQuick<ElfTypes32>;
template class ElfWriterQuick<ElfTypes64>;

}  // namespace art
