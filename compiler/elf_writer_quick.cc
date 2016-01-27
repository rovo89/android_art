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
#include "linker/buffered_output_stream.h"
#include "linker/file_output_stream.h"
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

  virtual OutputStream* GetStream() OVERRIDE;

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
    bss->WriteNoBitsSection(bss_size);
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
    // Generate all the debug information we can.
    dwarf::WriteDebugInfo(builder_.get(), method_infos, kCFIFormat);
  }
  if (compiler_options_->GetGenerateMiniDebugInfo()) {
    // Generate only some information and compress it.
    dwarf::WriteMiniDebugInfo(builder_.get(), method_infos);
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
OutputStream* ElfWriterQuick<ElfTypes>::GetStream() {
  return builder_->GetStream();
}

// Explicit instantiations
template class ElfWriterQuick<ElfTypes32>;
template class ElfWriterQuick<ElfTypes64>;

}  // namespace art
