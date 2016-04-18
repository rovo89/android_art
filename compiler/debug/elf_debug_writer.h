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

#ifndef ART_COMPILER_DEBUG_ELF_DEBUG_WRITER_H_
#define ART_COMPILER_DEBUG_ELF_DEBUG_WRITER_H_

#include <vector>

#include "base/macros.h"
#include "base/mutex.h"
#include "debug/dwarf/dwarf_constants.h"
#include "elf_builder.h"
#include "utils/array_ref.h"

namespace art {
class OatHeader;
namespace mirror {
class Class;
}
namespace debug {
struct MethodDebugInfo;

template <typename ElfTypes>
void WriteDebugInfo(
    ElfBuilder<ElfTypes>* builder,
    const ArrayRef<const MethodDebugInfo>& method_infos,
    dwarf::CFIFormat cfi_format,
    bool write_oat_patches);

std::vector<uint8_t> MakeMiniDebugInfo(
    InstructionSet isa,
    const InstructionSetFeatures* features,
    size_t rodata_section_size,
    size_t text_section_size,
    const ArrayRef<const MethodDebugInfo>& method_infos);

std::vector<uint8_t> WriteDebugElfFileForMethods(
    InstructionSet isa,
    const InstructionSetFeatures* features,
    const ArrayRef<const MethodDebugInfo>& method_infos);

std::vector<uint8_t> WriteDebugElfFileForClasses(
    InstructionSet isa,
    const InstructionSetFeatures* features,
    const ArrayRef<mirror::Class*>& types)
    SHARED_REQUIRES(Locks::mutator_lock_);

std::vector<MethodDebugInfo> MakeTrampolineInfos(const OatHeader& oat_header);

}  // namespace debug
}  // namespace art

#endif  // ART_COMPILER_DEBUG_ELF_DEBUG_WRITER_H_
