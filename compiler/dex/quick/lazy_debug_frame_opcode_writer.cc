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

#include "lazy_debug_frame_opcode_writer.h"
#include "mir_to_lir.h"

namespace art {
namespace dwarf {

const ArenaVector<uint8_t>* LazyDebugFrameOpCodeWriter::Patch(size_t code_size) {
  if (!this->enabled_) {
    DCHECK(this->data()->empty());
    return this->data();
  }
  if (!patched_) {
    patched_ = true;
    // Move our data buffer to temporary variable.
    ArenaVector<uint8_t> old_opcodes(this->opcodes_.get_allocator());
    old_opcodes.swap(this->opcodes_);
    // Refill our data buffer with patched opcodes.
    this->opcodes_.reserve(old_opcodes.size() + advances_.size() + 4);
    size_t pos = 0;
    for (auto advance : advances_) {
      DCHECK_GE(advance.pos, pos);
      // Copy old data up to the point when advance was issued.
      this->opcodes_.insert(this->opcodes_.end(),
                            old_opcodes.begin() + pos,
                            old_opcodes.begin() + advance.pos);
      pos = advance.pos;
      // This may be null if there is no slow-path code after return.
      LIR* next_lir = NEXT_LIR(advance.last_lir_insn);
      // Insert the advance command with its final offset.
      Base::AdvancePC(next_lir != nullptr ? next_lir->offset : code_size);
    }
    // Copy the final segment.
    this->opcodes_.insert(this->opcodes_.end(),
                          old_opcodes.begin() + pos,
                          old_opcodes.end());
    Base::AdvancePC(code_size);
  }
  return this->data();
}

}  // namespace dwarf
}  // namespace art
