/*
 * Copyright (C) 2016 The Android Open Source Project
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

#include "elf_debug_writer.h"

#include <vector>

#include "debug/dwarf/dwarf_constants.h"
#include "debug/elf_compilation_unit.h"
#include "debug/elf_debug_frame_writer.h"
#include "debug/elf_debug_info_writer.h"
#include "debug/elf_debug_line_writer.h"
#include "debug/elf_debug_loc_writer.h"
#include "debug/elf_gnu_debugdata_writer.h"
#include "debug/elf_symtab_writer.h"
#include "debug/method_debug_info.h"
#include "elf_builder.h"
#include "linker/vector_output_stream.h"
#include "utils/array_ref.h"

namespace art {
namespace debug {

template <typename ElfTypes>
void WriteDebugInfo(ElfBuilder<ElfTypes>* builder,
                    const ArrayRef<const MethodDebugInfo>& method_infos,
                    dwarf::CFIFormat cfi_format,
                    bool write_oat_patches) {
  // Add methods to .symtab.
  WriteDebugSymbols(builder, method_infos, true /* with_signature */);
  // Generate CFI (stack unwinding information).
  WriteCFISection(builder, method_infos, cfi_format, write_oat_patches);
  // Write DWARF .debug_* sections.
  WriteDebugSections(builder, method_infos, write_oat_patches);
}

template<typename ElfTypes>
static void WriteDebugSections(ElfBuilder<ElfTypes>* builder,
                               const ArrayRef<const MethodDebugInfo>& method_infos,
                               bool write_oat_patches) {
  // Group the methods into compilation units based on source file.
  std::vector<ElfCompilationUnit> compilation_units;
  const char* last_source_file = nullptr;
  for (const MethodDebugInfo& mi : method_infos) {
    auto& dex_class_def = mi.dex_file->GetClassDef(mi.class_def_index);
    const char* source_file = mi.dex_file->GetSourceFile(dex_class_def);
    if (compilation_units.empty() || source_file != last_source_file) {
      compilation_units.push_back(ElfCompilationUnit());
    }
    ElfCompilationUnit& cu = compilation_units.back();
    cu.methods.push_back(&mi);
    cu.low_pc = std::min(cu.low_pc, mi.low_pc);
    cu.high_pc = std::max(cu.high_pc, mi.high_pc);
    last_source_file = source_file;
  }

  // Write .debug_line section.
  if (!compilation_units.empty()) {
    ElfDebugLineWriter<ElfTypes> line_writer(builder);
    line_writer.Start();
    for (auto& compilation_unit : compilation_units) {
      line_writer.WriteCompilationUnit(compilation_unit);
    }
    line_writer.End(write_oat_patches);
  }

  // Write .debug_info section.
  if (!compilation_units.empty()) {
    ElfDebugInfoWriter<ElfTypes> info_writer(builder);
    info_writer.Start();
    for (const auto& compilation_unit : compilation_units) {
      ElfCompilationUnitWriter<ElfTypes> cu_writer(&info_writer);
      cu_writer.Write(compilation_unit);
    }
    info_writer.End(write_oat_patches);
  }
}

std::vector<uint8_t> MakeMiniDebugInfo(
    InstructionSet isa,
    const InstructionSetFeatures* features,
    size_t rodata_size,
    size_t text_size,
    const ArrayRef<const MethodDebugInfo>& method_infos) {
  if (Is64BitInstructionSet(isa)) {
    return MakeMiniDebugInfoInternal<ElfTypes64>(isa,
                                                 features,
                                                 rodata_size,
                                                 text_size,
                                                 method_infos);
  } else {
    return MakeMiniDebugInfoInternal<ElfTypes32>(isa,
                                                 features,
                                                 rodata_size,
                                                 text_size,
                                                 method_infos);
  }
}

template <typename ElfTypes>
static ArrayRef<const uint8_t> WriteDebugElfFileForMethodInternal(
    const InstructionSet isa,
    const InstructionSetFeatures* features,
    const MethodDebugInfo& method_info) {
  std::vector<uint8_t> buffer;
  buffer.reserve(KB);
  VectorOutputStream out("Debug ELF file", &buffer);
  std::unique_ptr<ElfBuilder<ElfTypes>> builder(new ElfBuilder<ElfTypes>(isa, features, &out));
  // No program headers since the ELF file is not linked and has no allocated sections.
  builder->Start(false /* write_program_headers */);
  WriteDebugInfo(builder.get(),
                 ArrayRef<const MethodDebugInfo>(&method_info, 1),
                 dwarf::DW_DEBUG_FRAME_FORMAT,
                 false /* write_oat_patches */);
  builder->End();
  CHECK(builder->Good());
  // Make a copy of the buffer.  We want to shrink it anyway.
  uint8_t* result = new uint8_t[buffer.size()];
  CHECK(result != nullptr);
  memcpy(result, buffer.data(), buffer.size());
  return ArrayRef<const uint8_t>(result, buffer.size());
}

ArrayRef<const uint8_t> WriteDebugElfFileForMethod(const InstructionSet isa,
                                                   const InstructionSetFeatures* features,
                                                   const MethodDebugInfo& method_info) {
  if (Is64BitInstructionSet(isa)) {
    return WriteDebugElfFileForMethodInternal<ElfTypes64>(isa, features, method_info);
  } else {
    return WriteDebugElfFileForMethodInternal<ElfTypes32>(isa, features, method_info);
  }
}

template <typename ElfTypes>
static ArrayRef<const uint8_t> WriteDebugElfFileForClassesInternal(
    const InstructionSet isa,
    const InstructionSetFeatures* features,
    const ArrayRef<mirror::Class*>& types)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  std::vector<uint8_t> buffer;
  buffer.reserve(KB);
  VectorOutputStream out("Debug ELF file", &buffer);
  std::unique_ptr<ElfBuilder<ElfTypes>> builder(new ElfBuilder<ElfTypes>(isa, features, &out));
  // No program headers since the ELF file is not linked and has no allocated sections.
  builder->Start(false /* write_program_headers */);
  ElfDebugInfoWriter<ElfTypes> info_writer(builder.get());
  info_writer.Start();
  ElfCompilationUnitWriter<ElfTypes> cu_writer(&info_writer);
  cu_writer.Write(types);
  info_writer.End(false /* write_oat_patches */);

  builder->End();
  CHECK(builder->Good());
  // Make a copy of the buffer.  We want to shrink it anyway.
  uint8_t* result = new uint8_t[buffer.size()];
  CHECK(result != nullptr);
  memcpy(result, buffer.data(), buffer.size());
  return ArrayRef<const uint8_t>(result, buffer.size());
}

ArrayRef<const uint8_t> WriteDebugElfFileForClasses(const InstructionSet isa,
                                                    const InstructionSetFeatures* features,
                                                    const ArrayRef<mirror::Class*>& types) {
  if (Is64BitInstructionSet(isa)) {
    return WriteDebugElfFileForClassesInternal<ElfTypes64>(isa, features, types);
  } else {
    return WriteDebugElfFileForClassesInternal<ElfTypes32>(isa, features, types);
  }
}

// Explicit instantiations
template void WriteDebugInfo<ElfTypes32>(
    ElfBuilder<ElfTypes32>* builder,
    const ArrayRef<const MethodDebugInfo>& method_infos,
    dwarf::CFIFormat cfi_format,
    bool write_oat_patches);
template void WriteDebugInfo<ElfTypes64>(
    ElfBuilder<ElfTypes64>* builder,
    const ArrayRef<const MethodDebugInfo>& method_infos,
    dwarf::CFIFormat cfi_format,
    bool write_oat_patches);

}  // namespace debug
}  // namespace art
