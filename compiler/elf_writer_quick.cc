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
#include "debug/elf_debug_writer.h"
#include "debug/method_debug_info.h"
#include "driver/compiler_options.h"
#include "elf.h"
#include "elf_builder.h"
#include "elf_utils.h"
#include "globals.h"
#include "leb128.h"
#include "linker/buffered_output_stream.h"
#include "linker/file_output_stream.h"
#include "thread-inl.h"
#include "thread_pool.h"
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

class DebugInfoTask : public Task {
 public:
  DebugInfoTask(InstructionSet isa,
                const InstructionSetFeatures* features,
                size_t rodata_section_size,
                size_t text_section_size,
                const ArrayRef<const debug::MethodDebugInfo>& method_infos)
      : isa_(isa),
        instruction_set_features_(features),
        rodata_section_size_(rodata_section_size),
        text_section_size_(text_section_size),
        method_infos_(method_infos) {
  }

  void Run(Thread*) {
    result_ = debug::MakeMiniDebugInfo(isa_,
                                       instruction_set_features_,
                                       rodata_section_size_,
                                       text_section_size_,
                                       method_infos_);
  }

  std::vector<uint8_t>* GetResult() {
    return &result_;
  }

 private:
  InstructionSet isa_;
  const InstructionSetFeatures* instruction_set_features_;
  size_t rodata_section_size_;
  size_t text_section_size_;
  const ArrayRef<const debug::MethodDebugInfo>& method_infos_;
  std::vector<uint8_t> result_;
};

template <typename ElfTypes>
class ElfWriterQuick FINAL : public ElfWriter {
 public:
  ElfWriterQuick(InstructionSet instruction_set,
                 const InstructionSetFeatures* features,
                 const CompilerOptions* compiler_options,
                 File* elf_file);
  ~ElfWriterQuick();

  void Start() OVERRIDE;
  void SetLoadedSectionSizes(size_t rodata_size, size_t text_size, size_t bss_size) OVERRIDE;
  void PrepareDebugInfo(const ArrayRef<const debug::MethodDebugInfo>& method_infos) OVERRIDE;
  OutputStream* StartRoData() OVERRIDE;
  void EndRoData(OutputStream* rodata) OVERRIDE;
  OutputStream* StartText() OVERRIDE;
  void EndText(OutputStream* text) OVERRIDE;
  void WriteDynamicSection() OVERRIDE;
  void WriteDebugInfo(const ArrayRef<const debug::MethodDebugInfo>& method_infos) OVERRIDE;
  void WritePatchLocations(const ArrayRef<const uintptr_t>& patch_locations) OVERRIDE;
  bool End() OVERRIDE;

  virtual OutputStream* GetStream() OVERRIDE;

  size_t GetLoadedSize() OVERRIDE;

  static void EncodeOatPatches(const std::vector<uintptr_t>& locations,
                               std::vector<uint8_t>* buffer);

 private:
  const InstructionSetFeatures* instruction_set_features_;
  const CompilerOptions* const compiler_options_;
  File* const elf_file_;
  size_t rodata_size_;
  size_t text_size_;
  size_t bss_size_;
  std::unique_ptr<BufferedOutputStream> output_stream_;
  std::unique_ptr<ElfBuilder<ElfTypes>> builder_;
  std::unique_ptr<DebugInfoTask> debug_info_task_;
  std::unique_ptr<ThreadPool> debug_info_thread_pool_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(ElfWriterQuick);
};

std::unique_ptr<ElfWriter> CreateElfWriterQuick(InstructionSet instruction_set,
                                                const InstructionSetFeatures* features,
                                                const CompilerOptions* compiler_options,
                                                File* elf_file) {
  if (Is64BitInstructionSet(instruction_set)) {
    return MakeUnique<ElfWriterQuick<ElfTypes64>>(instruction_set,
                                                  features,
                                                  compiler_options,
                                                  elf_file);
  } else {
    return MakeUnique<ElfWriterQuick<ElfTypes32>>(instruction_set,
                                                  features,
                                                  compiler_options,
                                                  elf_file);
  }
}

template <typename ElfTypes>
ElfWriterQuick<ElfTypes>::ElfWriterQuick(InstructionSet instruction_set,
                                         const InstructionSetFeatures* features,
                                         const CompilerOptions* compiler_options,
                                         File* elf_file)
    : ElfWriter(),
      instruction_set_features_(features),
      compiler_options_(compiler_options),
      elf_file_(elf_file),
      rodata_size_(0u),
      text_size_(0u),
      bss_size_(0u),
      output_stream_(MakeUnique<BufferedOutputStream>(MakeUnique<FileOutputStream>(elf_file))),
      builder_(new ElfBuilder<ElfTypes>(instruction_set, features, output_stream_.get())) {}

template <typename ElfTypes>
ElfWriterQuick<ElfTypes>::~ElfWriterQuick() {}

template <typename ElfTypes>
void ElfWriterQuick<ElfTypes>::Start() {
  builder_->Start();
}

template <typename ElfTypes>
void ElfWriterQuick<ElfTypes>::SetLoadedSectionSizes(size_t rodata_size,
                                                     size_t text_size,
                                                     size_t bss_size) {
  DCHECK_EQ(rodata_size_, 0u);
  rodata_size_ = rodata_size;
  DCHECK_EQ(text_size_, 0u);
  text_size_ = text_size;
  DCHECK_EQ(bss_size_, 0u);
  bss_size_ = bss_size;
  builder_->PrepareDynamicSection(elf_file_->GetPath(), rodata_size_, text_size_, bss_size_);
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
void ElfWriterQuick<ElfTypes>::WriteDynamicSection() {
  if (bss_size_ != 0u) {
    builder_->GetBss()->WriteNoBitsSection(bss_size_);
  }
  if (builder_->GetIsa() == kMips || builder_->GetIsa() == kMips64) {
    builder_->WriteMIPSabiflagsSection();
  }
  builder_->WriteDynamicSection();
}

template <typename ElfTypes>
void ElfWriterQuick<ElfTypes>::PrepareDebugInfo(
    const ArrayRef<const debug::MethodDebugInfo>& method_infos) {
  if (!method_infos.empty() && compiler_options_->GetGenerateMiniDebugInfo()) {
    // Prepare the mini-debug-info in background while we do other I/O.
    Thread* self = Thread::Current();
    debug_info_task_ = std::unique_ptr<DebugInfoTask>(
        new DebugInfoTask(builder_->GetIsa(),
                          instruction_set_features_,
                          rodata_size_,
                          text_size_,
                          method_infos));
    debug_info_thread_pool_ = std::unique_ptr<ThreadPool>(
        new ThreadPool("Mini-debug-info writer", 1));
    debug_info_thread_pool_->AddTask(self, debug_info_task_.get());
    debug_info_thread_pool_->StartWorkers(self);
  }
}

template <typename ElfTypes>
void ElfWriterQuick<ElfTypes>::WriteDebugInfo(
    const ArrayRef<const debug::MethodDebugInfo>& method_infos) {
  if (!method_infos.empty()) {
    if (compiler_options_->GetGenerateDebugInfo()) {
      // Generate all the debug information we can.
      debug::WriteDebugInfo(builder_.get(), method_infos, kCFIFormat, true /* write_oat_patches */);
    }
    if (compiler_options_->GetGenerateMiniDebugInfo()) {
      // Wait for the mini-debug-info generation to finish and write it to disk.
      Thread* self = Thread::Current();
      DCHECK(debug_info_thread_pool_ != nullptr);
      debug_info_thread_pool_->Wait(self, true, false);
      builder_->WriteSection(".gnu_debugdata", debug_info_task_->GetResult());
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
OutputStream* ElfWriterQuick<ElfTypes>::GetStream() {
  return builder_->GetStream();
}

template <typename ElfTypes>
size_t ElfWriterQuick<ElfTypes>::GetLoadedSize() {
  return builder_->GetLoadedSize();
}

// Explicit instantiations
template class ElfWriterQuick<ElfTypes32>;
template class ElfWriterQuick<ElfTypes64>;

}  // namespace art
