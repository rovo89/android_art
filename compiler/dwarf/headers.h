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

#include "debug_frame_opcode_writer.h"
#include "debug_info_entry_writer.h"
#include "debug_line_opcode_writer.h"
#include "register.h"
#include "writer.h"

namespace art {
namespace dwarf {

// Write common information entry (CIE) to .eh_frame section.
template<typename Allocator>
void WriteEhFrameCIE(bool is64bit, Reg return_address_register,
                     const DebugFrameOpCodeWriter<Allocator>& opcodes,
                     std::vector<uint8_t>* eh_frame) {
  Writer<> writer(eh_frame);
  size_t cie_header_start_ = writer.data()->size();
  if (is64bit) {
    // TODO: This is not related to being 64bit.
    writer.PushUint32(0xffffffff);
    writer.PushUint64(0);  // Length placeholder.
    writer.PushUint64(0);  // CIE id.
  } else {
    writer.PushUint32(0);  // Length placeholder.
    writer.PushUint32(0);  // CIE id.
  }
  writer.PushUint8(1);   // Version.
  writer.PushString("zR");
  writer.PushUleb128(DebugFrameOpCodeWriter<Allocator>::kCodeAlignmentFactor);
  writer.PushSleb128(DebugFrameOpCodeWriter<Allocator>::kDataAlignmentFactor);
  writer.PushUleb128(return_address_register.num());  // ubyte in DWARF2.
  writer.PushUleb128(1);  // z: Augmentation data size.
  if (is64bit) {
    writer.PushUint8(0x04);  // R: ((DW_EH_PE_absptr << 4) | DW_EH_PE_udata8).
  } else {
    writer.PushUint8(0x03);  // R: ((DW_EH_PE_absptr << 4) | DW_EH_PE_udata4).
  }
  writer.PushData(opcodes.data());
  writer.Pad(is64bit ? 8 : 4);
  if (is64bit) {
    writer.UpdateUint64(cie_header_start_ + 4, writer.data()->size() - cie_header_start_ - 12);
  } else {
    writer.UpdateUint32(cie_header_start_, writer.data()->size() - cie_header_start_ - 4);
  }
}

// Write frame description entry (FDE) to .eh_frame section.
template<typename Allocator>
void WriteEhFrameFDE(bool is64bit, size_t cie_offset,
                     uint64_t initial_address, uint64_t address_range,
                     const std::vector<uint8_t, Allocator>* opcodes,
                     std::vector<uint8_t>* eh_frame) {
  Writer<> writer(eh_frame);
  size_t fde_header_start = writer.data()->size();
  if (is64bit) {
    // TODO: This is not related to being 64bit.
    writer.PushUint32(0xffffffff);
    writer.PushUint64(0);  // Length placeholder.
    uint64_t cie_pointer = writer.data()->size() - cie_offset;
    writer.PushUint64(cie_pointer);
  } else {
    writer.PushUint32(0);  // Length placeholder.
    uint32_t cie_pointer = writer.data()->size() - cie_offset;
    writer.PushUint32(cie_pointer);
  }
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
  if (is64bit) {
    writer.UpdateUint64(fde_header_start + 4, writer.data()->size() - fde_header_start - 12);
  } else {
    writer.UpdateUint32(fde_header_start, writer.data()->size() - fde_header_start - 4);
  }
}

// Write compilation unit (CU) to .debug_info section.
template<typename Allocator>
void WriteDebugInfoCU(uint32_t debug_abbrev_offset,
                      const DebugInfoEntryWriter<Allocator>& entries,
                      std::vector<uint8_t>* debug_info) {
  Writer<> writer(debug_info);
  size_t start = writer.data()->size();
  writer.PushUint32(0);  // Length placeholder.
  writer.PushUint16(3);  // Version.
  writer.PushUint32(debug_abbrev_offset);
  writer.PushUint8(entries.is64bit() ? 8 : 4);
  writer.PushData(entries.data());
  writer.UpdateUint32(start, writer.data()->size() - start - 4);
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
                         std::vector<uint8_t>* debug_line) {
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
  writer.PushData(opcodes.data()->data(), opcodes.data()->size());
  writer.UpdateUint32(header_start, writer.data()->size() - header_start - 4);
}

}  // namespace dwarf
}  // namespace art

#endif  // ART_COMPILER_DWARF_HEADERS_H_
