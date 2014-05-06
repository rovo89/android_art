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
#include "driver/compiler_driver.h"

namespace art {

CompiledCode::CompiledCode(CompilerDriver* compiler_driver, InstructionSet instruction_set,
                           const std::vector<uint8_t>& quick_code)
    : compiler_driver_(compiler_driver), instruction_set_(instruction_set),
      portable_code_(nullptr), quick_code_(nullptr) {
  SetCode(&quick_code, nullptr);
}

CompiledCode::CompiledCode(CompilerDriver* compiler_driver, InstructionSet instruction_set,
                           const std::string& elf_object, const std::string& symbol)
    : compiler_driver_(compiler_driver), instruction_set_(instruction_set),
      portable_code_(nullptr), quick_code_(nullptr), symbol_(symbol) {
  CHECK_NE(elf_object.size(), 0U);
  CHECK_NE(symbol.size(), 0U);
  std::vector<uint8_t> temp_code(elf_object.size());
  for (size_t i = 0; i < elf_object.size(); ++i) {
    temp_code[i] = elf_object[i];
  }
  // TODO: we shouldn't just shove ELF objects in as "code" but
  // change to have different kinds of compiled methods.  This is
  // being deferred until we work on hybrid execution or at least
  // until we work on batch compilation.
  SetCode(nullptr, &temp_code);
}

void CompiledCode::SetCode(const std::vector<uint8_t>* quick_code,
                           const std::vector<uint8_t>* portable_code) {
  if (portable_code != nullptr) {
    CHECK(!portable_code->empty());
    portable_code_ = compiler_driver_->DeduplicateCode(*portable_code);
  }
  if (quick_code != nullptr) {
    CHECK(!quick_code->empty());
    quick_code_ = compiler_driver_->DeduplicateCode(*quick_code);
  }
}

bool CompiledCode::operator==(const CompiledCode& rhs) const {
  if (quick_code_ != nullptr) {
    if (rhs.quick_code_ == nullptr) {
      return false;
    } else if (quick_code_->size() != rhs.quick_code_->size()) {
      return false;
    } else {
      return std::equal(quick_code_->begin(), quick_code_->end(), rhs.quick_code_->begin());
    }
  } else if (portable_code_ != nullptr) {
    if (rhs.portable_code_ == nullptr) {
      return false;
    } else if (portable_code_->size() != rhs.portable_code_->size()) {
      return false;
    } else {
      return std::equal(portable_code_->begin(), portable_code_->end(),
                        rhs.portable_code_->begin());
    }
  }
  return (rhs.quick_code_ == nullptr) && (rhs.portable_code_ == nullptr);
}

uint32_t CompiledCode::AlignCode(uint32_t offset) const {
  return AlignCode(offset, instruction_set_);
}

uint32_t CompiledCode::AlignCode(uint32_t offset, InstructionSet instruction_set) {
  return RoundUp(offset, GetInstructionSetAlignment(instruction_set));
}

size_t CompiledCode::CodeDelta() const {
  switch (instruction_set_) {
    case kArm:
    case kArm64:
    case kMips:
    case kX86:
    case kX86_64:
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
    case kArm64:
    case kMips:
    case kX86:
    case kX86_64:
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

CompiledMethod::CompiledMethod(CompilerDriver* driver,
                               InstructionSet instruction_set,
                               const std::vector<uint8_t>& quick_code,
                               const size_t frame_size_in_bytes,
                               const uint32_t core_spill_mask,
                               const uint32_t fp_spill_mask,
                               const std::vector<uint8_t>& mapping_table,
                               const std::vector<uint8_t>& vmap_table,
                               const std::vector<uint8_t>& native_gc_map,
                               const std::vector<uint8_t>* cfi_info)
    : CompiledCode(driver, instruction_set, quick_code), frame_size_in_bytes_(frame_size_in_bytes),
      core_spill_mask_(core_spill_mask), fp_spill_mask_(fp_spill_mask),
  mapping_table_(driver->DeduplicateMappingTable(mapping_table)),
  vmap_table_(driver->DeduplicateVMapTable(vmap_table)),
  gc_map_(driver->DeduplicateGCMap(native_gc_map)),
  cfi_info_(driver->DeduplicateCFIInfo(cfi_info)) {
}

CompiledMethod::CompiledMethod(CompilerDriver* driver,
                               InstructionSet instruction_set,
                               const std::vector<uint8_t>& code,
                               const size_t frame_size_in_bytes,
                               const uint32_t core_spill_mask,
                               const uint32_t fp_spill_mask)
    : CompiledCode(driver, instruction_set, code),
      frame_size_in_bytes_(frame_size_in_bytes),
      core_spill_mask_(core_spill_mask), fp_spill_mask_(fp_spill_mask),
      mapping_table_(driver->DeduplicateMappingTable(std::vector<uint8_t>())),
      vmap_table_(driver->DeduplicateVMapTable(std::vector<uint8_t>())),
      gc_map_(driver->DeduplicateGCMap(std::vector<uint8_t>())),
      cfi_info_(nullptr) {
}

// Constructs a CompiledMethod for the Portable compiler.
CompiledMethod::CompiledMethod(CompilerDriver* driver, InstructionSet instruction_set,
                               const std::string& code, const std::vector<uint8_t>& gc_map,
                               const std::string& symbol)
    : CompiledCode(driver, instruction_set, code, symbol),
      frame_size_in_bytes_(kStackAlignment), core_spill_mask_(0),
      fp_spill_mask_(0), gc_map_(driver->DeduplicateGCMap(gc_map)) {
  mapping_table_ = driver->DeduplicateMappingTable(std::vector<uint8_t>());
  vmap_table_ = driver->DeduplicateVMapTable(std::vector<uint8_t>());
}

CompiledMethod::CompiledMethod(CompilerDriver* driver, InstructionSet instruction_set,
                               const std::string& code, const std::string& symbol)
    : CompiledCode(driver, instruction_set, code, symbol),
      frame_size_in_bytes_(kStackAlignment), core_spill_mask_(0),
      fp_spill_mask_(0) {
  mapping_table_ = driver->DeduplicateMappingTable(std::vector<uint8_t>());
  vmap_table_ = driver->DeduplicateVMapTable(std::vector<uint8_t>());
  gc_map_ = driver->DeduplicateGCMap(std::vector<uint8_t>());
}

}  // namespace art
