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

#ifndef ART_COMPILER_DWARF_DEBUG_FRAME_WRITER_H_
#define ART_COMPILER_DWARF_DEBUG_FRAME_WRITER_H_

#include "debug_frame_opcode_writer.h"
#include "dwarf.h"
#include "writer.h"

namespace art {
namespace dwarf {

// Writer for the .eh_frame section (which extends .debug_frame specification).
template<typename Allocator = std::allocator<uint8_t>>
class DebugFrameWriter FINAL : private Writer<Allocator> {
 public:
  void WriteCIE(Reg return_address_register,
                const uint8_t* initial_opcodes,
                int initial_opcodes_size) {
    DCHECK(cie_header_start_ == ~0u);
    cie_header_start_ = this->data()->size();
    this->PushUint32(0);  // Length placeholder.
    this->PushUint32(0);  // CIE id.
    this->PushUint8(1);   // Version.
    this->PushString("zR");
    this->PushUleb128(DebugFrameOpCodeWriter<Allocator>::kCodeAlignmentFactor);
    this->PushSleb128(DebugFrameOpCodeWriter<Allocator>::kDataAlignmentFactor);
    this->PushUleb128(return_address_register.num());  // ubyte in DWARF2.
    this->PushUleb128(1);  // z: Augmentation data size.
    if (use_64bit_address_) {
      this->PushUint8(0x04);  // R: ((DW_EH_PE_absptr << 4) | DW_EH_PE_udata8).
    } else {
      this->PushUint8(0x03);  // R: ((DW_EH_PE_absptr << 4) | DW_EH_PE_udata4).
    }
    this->PushData(initial_opcodes, initial_opcodes_size);
    this->Pad(use_64bit_address_ ? 8 : 4);
    this->UpdateUint32(cie_header_start_, this->data()->size() - cie_header_start_ - 4);
  }

  void WriteCIE(Reg return_address_register,
                const DebugFrameOpCodeWriter<Allocator>& opcodes) {
    WriteCIE(return_address_register, opcodes.data()->data(), opcodes.data()->size());
  }

  void WriteFDE(uint64_t initial_address,
                uint64_t address_range,
                const uint8_t* unwind_opcodes,
                int unwind_opcodes_size) {
    DCHECK(cie_header_start_ != ~0u);
    size_t fde_header_start = this->data()->size();
    this->PushUint32(0);  // Length placeholder.
    this->PushUint32(this->data()->size() - cie_header_start_);  // 'CIE_pointer'
    if (use_64bit_address_) {
      this->PushUint64(initial_address);
      this->PushUint64(address_range);
    } else {
      this->PushUint32(initial_address);
      this->PushUint32(address_range);
    }
    this->PushUleb128(0);  // Augmentation data size.
    this->PushData(unwind_opcodes, unwind_opcodes_size);
    this->Pad(use_64bit_address_ ? 8 : 4);
    this->UpdateUint32(fde_header_start, this->data()->size() - fde_header_start - 4);
  }

  DebugFrameWriter(std::vector<uint8_t, Allocator>* buffer, bool use_64bit_address)
      : Writer<Allocator>(buffer),
        use_64bit_address_(use_64bit_address),
        cie_header_start_(~0u) {
  }

 private:
  bool use_64bit_address_;
  size_t cie_header_start_;

  DISALLOW_COPY_AND_ASSIGN(DebugFrameWriter);
};

}  // namespace dwarf
}  // namespace art

#endif  // ART_COMPILER_DWARF_DEBUG_FRAME_WRITER_H_
