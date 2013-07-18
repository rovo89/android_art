/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "compiled_method.h"

namespace art {

CompiledCode::CompiledCode(InstructionSet instruction_set, const std::vector<uint8_t>& code)
    : instruction_set_(instruction_set), code_(code) {
  CHECK_NE(code.size(), 0U);
}

CompiledCode::CompiledCode(InstructionSet instruction_set,
                           const std::string& elf_object,
                           const std::string& symbol)
    : instruction_set_(instruction_set), symbol_(symbol) {
  CHECK_NE(elf_object.size(), 0U);
  CHECK_NE(symbol.size(), 0U);
  // TODO: we shouldn't just shove ELF objects in as "code" but
  // change to have different kinds of compiled methods.  This is
  // being deferred until we work on hybrid execution or at least
  // until we work on batch compilation.
  code_.resize(elf_object.size());
  memcpy(&code_[0], &elf_object[0], elf_object.size());
}

uint32_t CompiledCode::AlignCode(uint32_t offset) const {
  return AlignCode(offset, instruction_set_);
}

uint32_t CompiledCode::AlignCode(uint32_t offset, InstructionSet instruction_set) {
  switch (instruction_set) {
    case kArm:
    case kThumb2:
      return RoundUp(offset, kArmAlignment);
    case kMips:
      return RoundUp(offset, kMipsAlignment);
    case kX86:
      return RoundUp(offset, kX86Alignment);
    default:
      LOG(FATAL) << "Unknown InstructionSet: " << instruction_set;
      return 0;
  }
}

size_t CompiledCode::CodeDelta() const {
  switch (instruction_set_) {
    case kArm:
    case kMips:
    case kX86:
      return 0;
    case kThumb2: {
      // +1 to set the low-order bit so a BLX will switch to Thumb mode
      return 1;
    }
    default:
      LOG(FATAL) << "Unknown InstructionSet: " << instruction_set_;
      return 0;
  }
}

const void* CompiledCode::CodePointer(const void* code_pointer,
                                      InstructionSet instruction_set) {
  switch (instruction_set) {
    case kArm:
    case kMips:
    case kX86:
      return code_pointer;
    case kThumb2: {
      uintptr_t address = reinterpret_cast<uintptr_t>(code_pointer);
      // Set the low-order bit so a BLX will switch to Thumb mode
      address |= 0x1;
      return reinterpret_cast<const void*>(address);
    }
    default:
      LOG(FATAL) << "Unknown InstructionSet: " << instruction_set;
      return NULL;
  }
}

#if defined(ART_USE_PORTABLE_COMPILER)
const std::string& CompiledCode::GetSymbol() const {
  CHECK_NE(0U, symbol_.size());
  return symbol_;
}

const std::vector<uint32_t>& CompiledCode::GetOatdataOffsetsToCompliledCodeOffset() const {
  CHECK_NE(0U, oatdata_offsets_to_compiled_code_offset_.size()) << symbol_;
  return oatdata_offsets_to_compiled_code_offset_;
}

void CompiledCode::AddOatdataOffsetToCompliledCodeOffset(uint32_t offset) {
  oatdata_offsets_to_compiled_code_offset_.push_back(offset);
}
#endif

CompiledMethod::CompiledMethod(InstructionSet instruction_set,
                               const std::vector<uint8_t>& code,
                               const size_t frame_size_in_bytes,
                               const uint32_t core_spill_mask,
                               const uint32_t fp_spill_mask,
                               const std::vector<uint32_t>& mapping_table,
                               const std::vector<uint16_t>& vmap_table,
                               const std::vector<uint8_t>& native_gc_map)
    : CompiledCode(instruction_set, code), frame_size_in_bytes_(frame_size_in_bytes),
      core_spill_mask_(core_spill_mask), fp_spill_mask_(fp_spill_mask),
      gc_map_(native_gc_map) {
  DCHECK_EQ(vmap_table.size(),
            static_cast<uint32_t>(__builtin_popcount(core_spill_mask)
                                  + __builtin_popcount(fp_spill_mask)));
  CHECK_LE(vmap_table.size(), (1U << 16) - 1); // length must fit in 2^16-1

  std::vector<uint32_t> length_prefixed_mapping_table;
  length_prefixed_mapping_table.push_back(mapping_table.size());
  length_prefixed_mapping_table.insert(length_prefixed_mapping_table.end(),
                                       mapping_table.begin(),
                                       mapping_table.end());
  DCHECK_EQ(mapping_table.size() + 1, length_prefixed_mapping_table.size());

  std::vector<uint16_t> length_prefixed_vmap_table;
  length_prefixed_vmap_table.push_back(vmap_table.size());
  length_prefixed_vmap_table.insert(length_prefixed_vmap_table.end(),
                                    vmap_table.begin(),
                                    vmap_table.end());
  DCHECK_EQ(vmap_table.size() + 1, length_prefixed_vmap_table.size());
  DCHECK_EQ(vmap_table.size(), length_prefixed_vmap_table[0]);

  mapping_table_ = length_prefixed_mapping_table;
  vmap_table_ = length_prefixed_vmap_table;
  DCHECK_EQ(vmap_table_[0], static_cast<uint32_t>(__builtin_popcount(core_spill_mask) + __builtin_popcount(fp_spill_mask)));
}

CompiledMethod::CompiledMethod(InstructionSet instruction_set,
                               const std::vector<uint8_t>& code,
                               const size_t frame_size_in_bytes,
                               const uint32_t core_spill_mask,
                               const uint32_t fp_spill_mask)
    : CompiledCode(instruction_set, code),
      frame_size_in_bytes_(frame_size_in_bytes),
      core_spill_mask_(core_spill_mask), fp_spill_mask_(fp_spill_mask) {}

}  // namespace art
