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

#include "oat_writer.h"

namespace art {
namespace dwarf {

void WriteEhFrame(const CompilerDriver* compiler,
                  OatWriter* oat_writer,
                  uint32_t text_section_offset,
                  std::vector<uint8_t>* eh_frame);

void WriteDebugSections(const CompilerDriver* compiler,
                        OatWriter* oat_writer,
                        uint32_t text_section_offset,
                        std::vector<uint8_t>* debug_info,
                        std::vector<uint8_t>* debug_abbrev,
                        std::vector<uint8_t>* debug_str,
                        std::vector<uint8_t>* debug_line);

}  // namespace dwarf
}  // namespace art

#endif  // ART_COMPILER_ELF_WRITER_DEBUG_H_
