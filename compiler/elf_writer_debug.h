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

#include <vector>

#include "dwarf/dwarf_constants.h"
#include "oat_writer.h"

namespace art {
namespace dwarf {

void WriteCFISection(const CompilerDriver* compiler,
                     const OatWriter* oat_writer,
                     ExceptionHeaderValueApplication address_type,
                     CFIFormat format,
                     std::vector<uint8_t>* debug_frame,
                     std::vector<uintptr_t>* debug_frame_patches,
                     std::vector<uint8_t>* eh_frame_hdr,
                     std::vector<uintptr_t>* eh_frame_hdr_patches);

void WriteDebugSections(const CompilerDriver* compiler,
                        const OatWriter* oat_writer,
                        std::vector<uint8_t>* debug_info,
                        std::vector<uintptr_t>* debug_info_patches,
                        std::vector<uint8_t>* debug_abbrev,
                        std::vector<uint8_t>* debug_str,
                        std::vector<uint8_t>* debug_line,
                        std::vector<uintptr_t>* debug_line_patches);

}  // namespace dwarf
}  // namespace art

#endif  // ART_COMPILER_ELF_WRITER_DEBUG_H_
