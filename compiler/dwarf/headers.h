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

#ifndef ART_COMPILER_DWARF_HEADERS_H_
#define ART_COMPILER_DWARF_HEADERS_H_

#include <cstdint>

#include "dwarf/debug_frame_opcode_writer.h"
#include "dwarf/debug_info_entry_writer.h"
#include "dwarf/debug_line_opcode_writer.h"
#include "dwarf/dwarf_constants.h"
#include "dwarf/register.h"
#include "dwarf/writer.h"

namespace art {
namespace dwarf {

// Note that all headers start with 32-bit length.
// DWARF also supports 64-bit lengths, but we never use that.
// It is intended to support very large debug sections (>4GB),
// and compilers are expected *not* to use it by default.
// In particular, it is not related to machine architecture.

// Write common information entry (CIE) to .debug_frame or .eh_frame section.
template<typename Allocator>
void WriteDebugFrameCIE(bool is64bit,
                        ExceptionHeaderValueApplication address_type,
                        Reg return_address_register,
                        const DebugFrameOpCodeWriter<Allocator>& opcodes,
                        CFIFormat format,
                        std::vector<uint8_t>* debug_frame) {
  Writer<> writer(debug_frame);
  size_t cie_header_start_ = writer.data()->size();
  writer.PushUint32(0);  // Length placeholder.
  writer.PushUint32((format == DW_EH_FRAME_FORMAT) ? 0 : 0xFFFFFFFF);  // CIE id.
  writer.PushUint8(1);   // Version.
  writer.PushString("zR");
  writer.PushUleb128(DebugFrameOpCodeWriter<Allocator>::kCodeAlignmentFactor);
  writer.PushSleb128(DebugFrameOpCodeWriter<Allocator>::kDataAlignmentFactor);
  writer.PushUleb128(return_address_register.num());  // ubyte in DWARF2.
  writer.PushUleb128(1);  // z: Augmentation data size.
  if (is64bit) {
    if (address_type == DW_EH_PE_pcrel) {
      writer.PushUint8(DW_EH_PE_pcrel | DW_EH_PE_sdata8);   // R: Pointer encoding.
    } else {
      DCHECK(address_type == DW_EH_PE_absptr);
      writer.PushUint8(DW_EH_PE_absptr | DW_EH_PE_udata8);  // R: Pointer encoding.
    }
  } else {
    if (address_type == DW_EH_PE_pcrel) {
      writer.PushUint8(DW_EH_PE_pcrel | DW_EH_PE_sdata4);   // R: Pointer encoding.
    } else {
      DCHECK(address_type == DW_EH_PE_absptr);
      writer.PushUint8(DW_EH_PE_absptr | DW_EH_PE_udata4);  // R: Pointer encoding.
    }
  }
  writer.PushData(opcodes.data());
  writer.Pad(is64bit ? 8 : 4);
  writer.UpdateUint32(cie_header_start_, writer.data()->size() - cie_header_start_ - 4);
}

// Write frame description entry (FDE) to .debug_frame or .eh_frame section.
template<typename Allocator>
void WriteDebugFrameFDE(bool is64bit, size_t cie_offset,
                        uint64_t initial_address, uint64_t address_range,
                        const std::vector<uint8_t, Allocator>* opcodes,
                        CFIFormat format,
                        std::vector<uint8_t>* debug_frame,
                        std::vector<uintptr_t>* debug_frame_patches) {
  Writer<> writer(debug_frame);
  size_t fde_header_start = writer.data()->size();
  writer.PushUint32(0);  // Length placeholder.
  if (format == DW_EH_FRAME_FORMAT) {
    uint32_t cie_pointer = writer.data()->size() - cie_offset;
    writer.PushUint32(cie_pointer);
  } else {
    uint32_t cie_pointer = cie_offset;
    writer.PushUint32(cie_pointer);
  }
  // Relocate initial_address, but not address_range (it is size).
  debug_frame_patches->push_back(writer.data()->size());
  if (is64bit) {
    writer.PushUint64(initial_address);
    writer.PushUint64(address_range);
  } else {
    writer.PushUint32(initial_address);
    writer.PushUint32(address_range);
  }
  writer.PushUleb128(0);  // Augmentation data size.
  writer.PushData(opcodes);
  writer.Pad(is64bit ? 8 : 4);
  writer.UpdateUint32(fde_header_start, writer.data()->size() - fde_header_start - 4);
}

// Write compilation unit (CU) to .debug_info section.
template<typename Allocator>
void WriteDebugInfoCU(uint32_t debug_abbrev_offset,
                      const DebugInfoEntryWriter<Allocator>& entries,
                      std::vector<uint8_t>* debug_info,
                      std::vector<uintptr_t>* debug_info_patches) {
  Writer<> writer(debug_info);
  size_t start = writer.data()->size();
  writer.PushUint32(0);  // Length placeholder.
  writer.PushUint16(3);  // Version.
  writer.PushUint32(debug_abbrev_offset);
  writer.PushUint8(entries.Is64bit() ? 8 : 4);
  size_t entries_offset = writer.data()->size();
  writer.PushData(entries.data());
  writer.UpdateUint32(start, writer.data()->size() - start - 4);
  // Copy patch locations and make them relative to .debug_info section.
  for (uintptr_t patch_location : entries.GetPatchLocations()) {
    debug_info_patches->push_back(entries_offset + patch_location);
  }
}

struct FileEntry {
  std::string file_name;
  int directory_index;
  int modification_time;
  int file_size;
};

// Write line table to .debug_line section.
template<typename Allocator>
void WriteDebugLineTable(const std::vector<std::string>& include_directories,
                         const std::vector<FileEntry>& files,
                         const DebugLineOpCodeWriter<Allocator>& opcodes,
                         std::vector<uint8_t>* debug_line,
                         std::vector<uintptr_t>* debug_line_patches) {
  Writer<> writer(debug_line);
  size_t header_start = writer.data()->size();
  writer.PushUint32(0);  // Section-length placeholder.
  // Claim DWARF-2 version even though we use some DWARF-3 features.
  // DWARF-2 consumers will ignore the unknown opcodes.
  // This is what clang currently does.
  writer.PushUint16(2);  // .debug_line version.
  size_t header_length_pos = writer.data()->size();
  writer.PushUint32(0);  // Header-length placeholder.
  writer.PushUint8(1 << opcodes.GetCodeFactorBits());
  writer.PushUint8(DebugLineOpCodeWriter<Allocator>::kDefaultIsStmt ? 1 : 0);
  writer.PushInt8(DebugLineOpCodeWriter<Allocator>::kLineBase);
  writer.PushUint8(DebugLineOpCodeWriter<Allocator>::kLineRange);
  writer.PushUint8(DebugLineOpCodeWriter<Allocator>::kOpcodeBase);
  static const int opcode_lengths[DebugLineOpCodeWriter<Allocator>::kOpcodeBase] = {
      0, 0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1 };
  for (int i = 1; i < DebugLineOpCodeWriter<Allocator>::kOpcodeBase; i++) {
    writer.PushUint8(opcode_lengths[i]);
  }
  for (const std::string& directory : include_directories) {
    writer.PushData(directory.data(), directory.size() + 1);
  }
  writer.PushUint8(0);  // Terminate include_directories list.
  for (const FileEntry& file : files) {
    writer.PushData(file.file_name.data(), file.file_name.size() + 1);
    writer.PushUleb128(file.directory_index);
    writer.PushUleb128(file.modification_time);
    writer.PushUleb128(file.file_size);
  }
  writer.PushUint8(0);  // Terminate file list.
  writer.UpdateUint32(header_length_pos, writer.data()->size() - header_length_pos - 4);
  size_t opcodes_offset = writer.data()->size();
  writer.PushData(opcodes.data());
  writer.UpdateUint32(header_start, writer.data()->size() - header_start - 4);
  // Copy patch locations and make them relative to .debug_line section.
  for (uintptr_t patch_location : opcodes.GetPatchLocations()) {
    debug_line_patches->push_back(opcodes_offset + patch_location);
  }
}

}  // namespace dwarf
}  // namespace art

#endif  // ART_COMPILER_DWARF_HEADERS_H_
