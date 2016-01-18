/*
 * Copyright (C) 2015 The Android Open Source Project
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

#ifndef ART_COMPILER_ELF_WRITER_DEBUG_H_
#define ART_COMPILER_ELF_WRITER_DEBUG_H_

#include "base/macros.h"
#include "base/mutex.h"
#include "dwarf/dwarf_constants.h"
#include "elf_builder.h"
#include "utils/array_ref.h"

namespace art {
namespace mirror {
class Class;
}
namespace dwarf {
struct MethodDebugInfo;

template <typename ElfTypes>
void WriteDebugInfo(ElfBuilder<ElfTypes>* builder,
                    const ArrayRef<const MethodDebugInfo>& method_infos,
                    CFIFormat cfi_format);

ArrayRef<const uint8_t> WriteDebugElfFileForMethod(const dwarf::MethodDebugInfo& method_info);

ArrayRef<const uint8_t> WriteDebugElfFileForClasses(const InstructionSet isa,
                                                    const ArrayRef<mirror::Class*>& types)
    SHARED_REQUIRES(Locks::mutator_lock_);

}  // namespace dwarf
}  // namespace art

#endif  // ART_COMPILER_ELF_WRITER_DEBUG_H_
