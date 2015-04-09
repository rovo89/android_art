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

#ifndef ART_COMPILER_DEX_QUICK_LAZY_DEBUG_FRAME_OPCODE_WRITER_H_
#define ART_COMPILER_DEX_QUICK_LAZY_DEBUG_FRAME_OPCODE_WRITER_H_

#include "base/arena_allocator.h"
#include "base/arena_containers.h"
#include "dwarf/debug_frame_opcode_writer.h"

namespace art {
struct LIR;
namespace dwarf {

// When we are generating the CFI code, we do not know the instuction offsets,
// this class stores the LIR references and patches the instruction stream later.
class LazyDebugFrameOpCodeWriter FINAL
    : public DebugFrameOpCodeWriter<ArenaAllocatorAdapter<uint8_t>> {
  typedef DebugFrameOpCodeWriter<ArenaAllocatorAdapter<uint8_t>> Base;
 public:
  // This method is implicitely called the by opcode writers.
  virtual void ImplicitlyAdvancePC() OVERRIDE {
    DCHECK_EQ(patched_, false);
    DCHECK_EQ(this->current_pc_, 0);
    advances_.push_back({this->data()->size(), *last_lir_insn_});
  }

  const ArenaVector<uint8_t>* Patch(size_t code_size);

  explicit LazyDebugFrameOpCodeWriter(LIR** last_lir_insn, bool enable_writes,
                                      ArenaAllocator* allocator)
    : Base(enable_writes, allocator->Adapter()),
      last_lir_insn_(last_lir_insn),
      advances_(allocator->Adapter()),
      patched_(false) {
  }

 private:
  typedef struct {
    size_t pos;
    LIR* last_lir_insn;
  } Advance;

  using Base::data;  // Hidden. Use Patch method instead.

  LIR** last_lir_insn_;
  ArenaVector<Advance> advances_;
  bool patched_;

  DISALLOW_COPY_AND_ASSIGN(LazyDebugFrameOpCodeWriter);
};

}  // namespace dwarf
}  // namespace art

#endif  // ART_COMPILER_DEX_QUICK_LAZY_DEBUG_FRAME_OPCODE_WRITER_H_
