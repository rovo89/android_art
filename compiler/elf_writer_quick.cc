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
#include "base/stl_util.h"
#include "compiled_method.h"
#include "driver/compiler_options.h"
#include "dwarf/method_debug_info.h"
#include "elf.h"
#include "elf_builder.h"
#include "elf_utils.h"
#include "elf_writer_debug.h"
#include "globals.h"
#include "leb128.h"
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
class ElfWriterQuick FINAL : public ElfWriter {
 public:
  ElfWriterQuick(InstructionSet instruction_set,
                 const CompilerOptions* compiler_options,
                 File* elf_file);
  ~ElfWriterQuick();

  void Start() OVERRIDE;
  OutputStream* StartRoData() OVERRIDE;
  void EndRoData(OutputStream* rodata) OVERRIDE;
  OutputStream* StartText() OVERRIDE;
  void EndText(OutputStream* text) OVERRIDE;
  void SetBssSize(size_t bss_size) OVERRIDE;
  void WriteDynamicSection() OVERRIDE;
  void WriteDebugInfo(const ArrayRef<const dwarf::MethodDebugInfo>& method_infos) OVERRIDE;
  void WritePatchLocations(const ArrayRef<const uintptr_t>& patch_locations) OVERRIDE;
  bool End() OVERRIDE;

  static void EncodeOatPatches(const std::vector<uintptr_t>& locations,
                               std::vector<uint8_t>* buffer);

 private:
  const CompilerOptions* const compiler_options_;
  File* const elf_file_;
  std::unique_ptr<BufferedOutputStream> output_stream_;
  std::unique_ptr<ElfBuilder<ElfTypes>> builder_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(ElfWriterQuick);
};

std::unique_ptr<ElfWriter> CreateElfWriterQuick(InstructionSet instruction_set,
                                                const CompilerOptions* compiler_options,
                                                File* elf_file) {
  if (Is64BitInstructionSet(instruction_set)) {
    return MakeUnique<ElfWriterQuick<ElfTypes64>>(instruction_set, compiler_options, elf_file);
  } else {
    return MakeUnique<ElfWriterQuick<ElfTypes32>>(instruction_set, compiler_options, elf_file);
  }
}

template <typename ElfTypes>
static void WriteDebugSymbols(ElfBuilder<ElfTypes>* builder,
                              const ArrayRef<const dwarf::MethodDebugInfo>& method_infos);

template <typename ElfTypes>
ElfWriterQuick<ElfTypes>::ElfWriterQuick(InstructionSet instruction_set,
                                         const CompilerOptions* compiler_options,
                                         File* elf_file)
    : ElfWriter(),
      compiler_options_(compiler_options),
      elf_file_(elf_file),
      output_stream_(MakeUnique<BufferedOutputStream>(MakeUnique<FileOutputStream>(elf_file))),
      builder_(new ElfBuilder<ElfTypes>(instruction_set, output_stream_.get())) {}

template <typename ElfTypes>
ElfWriterQuick<ElfTypes>::~ElfWriterQuick() {}

template <typename ElfTypes>
void ElfWriterQuick<ElfTypes>::Start() {
  builder_->Start();
}

template <typename ElfTypes>
OutputStream* ElfWriterQuick<ElfTypes>::StartRoData() {
  auto* rodata = builder_->GetRoData();
  rodata->Start();
  return rodata;
}

template <typename ElfTypes>
void ElfWriterQuick<ElfTypes>::EndRoData(OutputStream* rodata) {
  CHECK_EQ(builder_->GetRoData(), rodata);
  builder_->GetRoData()->End();
}

template <typename ElfTypes>
OutputStream* ElfWriterQuick<ElfTypes>::StartText() {
  auto* text = builder_->GetText();
  text->Start();
  return text;
}

template <typename ElfTypes>
void ElfWriterQuick<ElfTypes>::EndText(OutputStream* text) {
  CHECK_EQ(builder_->GetText(), text);
  builder_->GetText()->End();
}

template <typename ElfTypes>
void ElfWriterQuick<ElfTypes>::SetBssSize(size_t bss_size) {
  auto* bss = builder_->GetBss();
  if (bss_size != 0u) {
    bss->Start();
    bss->SetSize(bss_size);
    bss->End();
  }
}

template <typename ElfTypes>
void ElfWriterQuick<ElfTypes>::WriteDynamicSection() {
  builder_->WriteDynamicSection(elf_file_->GetPath());
}

template <typename ElfTypes>
void ElfWriterQuick<ElfTypes>::WriteDebugInfo(
    const ArrayRef<const dwarf::MethodDebugInfo>& method_infos) {
  if (compiler_options_->GetGenerateDebugInfo()) {
    if (!method_infos.empty()) {
      // Add methods to .symtab.
      WriteDebugSymbols(builder_.get(), method_infos);
      // Generate CFI (stack unwinding information).
      dwarf::WriteCFISection(builder_.get(), method_infos, kCFIFormat);
      // Write DWARF .debug_* sections.
      dwarf::WriteDebugSections(builder_.get(), method_infos);
    }
  }
}

template <typename ElfTypes>
void ElfWriterQuick<ElfTypes>::WritePatchLocations(
    const ArrayRef<const uintptr_t>& patch_locations) {
  // Add relocation section for .text.
  if (compiler_options_->GetIncludePatchInformation()) {
    // Note that ElfWriter::Fixup will be called regardless and therefore
    // we need to include oat_patches for debug sections unconditionally.
    builder_->WritePatches(".text.oat_patches", patch_locations);
  }
}

template <typename ElfTypes>
bool ElfWriterQuick<ElfTypes>::End() {
  builder_->End();

  return builder_->Good();
}

template <typename ElfTypes>
static void WriteDebugSymbols(ElfBuilder<ElfTypes>* builder,
                              const ArrayRef<const dwarf::MethodDebugInfo>& method_infos) {
  bool generated_mapping_symbol = false;
  auto* strtab = builder->GetStrTab();
  auto* symtab = builder->GetSymTab();

  if (method_infos.empty()) {
    return;
  }

  // Find all addresses (low_pc) which contain deduped methods.
  // The first instance of method is not marked deduped_, but the rest is.
  std::unordered_set<uint32_t> deduped_addresses;
  for (const dwarf::MethodDebugInfo& info : method_infos) {
    if (info.deduped_) {
      deduped_addresses.insert(info.low_pc_);
    }
  }

  strtab->Start();
  strtab->Write("");  // strtab should start with empty string.
  for (const dwarf::MethodDebugInfo& info : method_infos) {
    if (info.deduped_) {
      continue;  // Add symbol only for the first instance.
    }
    std::string name = PrettyMethod(info.dex_method_index_, *info.dex_file_, true);
    if (deduped_addresses.find(info.low_pc_) != deduped_addresses.end()) {
      name += " [DEDUPED]";
    }

    uint32_t low_pc = info.low_pc_;
    // Add in code delta, e.g., thumb bit 0 for Thumb2 code.
    low_pc += info.compiled_method_->CodeDelta();
    symtab->Add(strtab->Write(name), builder->GetText(), low_pc,
                true, info.high_pc_ - info.low_pc_, STB_GLOBAL, STT_FUNC);

    // Conforming to aaelf, add $t mapping symbol to indicate start of a sequence of thumb2
    // instructions, so that disassembler tools can correctly disassemble.
    // Note that even if we generate just a single mapping symbol, ARM's Streamline
    // requires it to match function symbol.  Just address 0 does not work.
    if (info.compiled_method_->GetInstructionSet() == kThumb2) {
      if (!generated_mapping_symbol || !kGenerateSingleArmMappingSymbol) {
        symtab->Add(strtab->Write("$t"), builder->GetText(), info.low_pc_ & ~1,
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
