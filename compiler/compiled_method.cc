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
                           const ArrayRef<const uint8_t>& quick_code, bool owns_code_array)
    : compiler_driver_(compiler_driver), instruction_set_(instruction_set),
      owns_code_array_(owns_code_array), quick_code_(nullptr) {
  SetCode(&quick_code);
}

void CompiledCode::SetCode(const ArrayRef<const uint8_t>* quick_code) {
  if (quick_code != nullptr) {
    CHECK(!quick_code->empty());
    if (owns_code_array_) {
      // If we are supposed to own the code, don't deduplicate it.
      CHECK(quick_code_ == nullptr);
      quick_code_ = new SwapVector<uint8_t>(quick_code->begin(), quick_code->end(),
                                            compiler_driver_->GetSwapSpaceAllocator());
    } else {
      quick_code_ = compiler_driver_->DeduplicateCode(*quick_code);
    }
  }
}

CompiledCode::~CompiledCode() {
  if (owns_code_array_) {
    delete quick_code_;
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
  }
  return (rhs.quick_code_ == nullptr);
}

size_t CompiledCode::AlignCode(size_t offset) const {
  return AlignCode(offset, instruction_set_);
}

size_t CompiledCode::AlignCode(size_t offset, InstructionSet instruction_set) {
  return RoundUp(offset, GetInstructionSetAlignment(instruction_set));
}

size_t CompiledCode::CodeDelta() const {
  return CodeDelta(instruction_set_);
}

size_t CompiledCode::CodeDelta(InstructionSet instruction_set) {
  switch (instruction_set) {
    case kArm:
    case kArm64:
    case kMips:
    case kMips64:
    case kX86:
    case kX86_64:
      return 0;
    case kThumb2: {
      // +1 to set the low-order bit so a BLX will switch to Thumb mode
      return 1;
    }
    default:
      LOG(FATAL) << "Unknown InstructionSet: " << instruction_set;
      return 0;
  }
}

const void* CompiledCode::CodePointer(const void* code_pointer,
                                      InstructionSet instruction_set) {
  switch (instruction_set) {
    case kArm:
    case kArm64:
    case kMips:
    case kMips64:
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
      return nullptr;
  }
}

const std::vector<uint32_t>& CompiledCode::GetOatdataOffsetsToCompliledCodeOffset() const {
  CHECK_NE(0U, oatdata_offsets_to_compiled_code_offset_.size());
  return oatdata_offsets_to_compiled_code_offset_;
}

void CompiledCode::AddOatdataOffsetToCompliledCodeOffset(uint32_t offset) {
  oatdata_offsets_to_compiled_code_offset_.push_back(offset);
}

CompiledMethod::CompiledMethod(CompilerDriver* driver,
                               InstructionSet instruction_set,
                               const ArrayRef<const uint8_t>& quick_code,
                               const size_t frame_size_in_bytes,
                               const uint32_t core_spill_mask,
                               const uint32_t fp_spill_mask,
                               DefaultSrcMap* src_mapping_table,
                               const ArrayRef<const uint8_t>& mapping_table,
                               const ArrayRef<const uint8_t>& vmap_table,
                               const ArrayRef<const uint8_t>& native_gc_map,
                               const ArrayRef<const uint8_t>& cfi_info,
                               const ArrayRef<const LinkerPatch>& patches)
    : CompiledCode(driver, instruction_set, quick_code, !driver->DedupeEnabled()),
      owns_arrays_(!driver->DedupeEnabled()),
      frame_size_in_bytes_(frame_size_in_bytes), core_spill_mask_(core_spill_mask),
      fp_spill_mask_(fp_spill_mask),
      patches_(patches.begin(), patches.end(), driver->GetSwapSpaceAllocator()) {
  if (owns_arrays_) {
    if (src_mapping_table == nullptr) {
      src_mapping_table_ = new SwapSrcMap(driver->GetSwapSpaceAllocator());
    } else {
      src_mapping_table_ = new SwapSrcMap(src_mapping_table->begin(), src_mapping_table->end(),
                                          driver->GetSwapSpaceAllocator());
    }
    mapping_table_ = mapping_table.empty() ?
        nullptr : new SwapVector<uint8_t>(mapping_table.begin(), mapping_table.end(),
                                          driver->GetSwapSpaceAllocator());
    vmap_table_ = new SwapVector<uint8_t>(vmap_table.begin(), vmap_table.end(),
                                          driver->GetSwapSpaceAllocator());
    gc_map_ = native_gc_map.empty() ? nullptr :
        new SwapVector<uint8_t>(native_gc_map.begin(), native_gc_map.end(),
                                driver->GetSwapSpaceAllocator());
    cfi_info_ = cfi_info.empty() ? nullptr :
        new SwapVector<uint8_t>(cfi_info.begin(), cfi_info.end(), driver->GetSwapSpaceAllocator());
  } else {
    src_mapping_table_ = src_mapping_table == nullptr ?
        driver->DeduplicateSrcMappingTable(ArrayRef<SrcMapElem>()) :
        driver->DeduplicateSrcMappingTable(ArrayRef<SrcMapElem>(*src_mapping_table));
    mapping_table_ = mapping_table.empty() ?
        nullptr : driver->DeduplicateMappingTable(mapping_table);
    vmap_table_ = driver->DeduplicateVMapTable(vmap_table);
    gc_map_ = native_gc_map.empty() ? nullptr : driver->DeduplicateGCMap(native_gc_map);
    cfi_info_ = cfi_info.empty() ? nullptr : driver->DeduplicateCFIInfo(cfi_info);
  }
}

CompiledMethod* CompiledMethod::SwapAllocCompiledMethod(
    CompilerDriver* driver,
    InstructionSet instruction_set,
    const ArrayRef<const uint8_t>& quick_code,
    const size_t frame_size_in_bytes,
    const uint32_t core_spill_mask,
    const uint32_t fp_spill_mask,
    DefaultSrcMap* src_mapping_table,
    const ArrayRef<const uint8_t>& mapping_table,
    const ArrayRef<const uint8_t>& vmap_table,
    const ArrayRef<const uint8_t>& native_gc_map,
    const ArrayRef<const uint8_t>& cfi_info,
    const ArrayRef<const LinkerPatch>& patches) {
  SwapAllocator<CompiledMethod> alloc(driver->GetSwapSpaceAllocator());
  CompiledMethod* ret = alloc.allocate(1);
  alloc.construct(ret, driver, instruction_set, quick_code, frame_size_in_bytes, core_spill_mask,
                  fp_spill_mask, src_mapping_table, mapping_table, vmap_table, native_gc_map,
                  cfi_info, patches);
  return ret;
}



void CompiledMethod::ReleaseSwapAllocatedCompiledMethod(CompilerDriver* driver, CompiledMethod* m) {
  SwapAllocator<CompiledMethod> alloc(driver->GetSwapSpaceAllocator());
  alloc.destroy(m);
  alloc.deallocate(m, 1);
}

CompiledMethod::~CompiledMethod() {
  if (owns_arrays_) {
    delete src_mapping_table_;
    delete mapping_table_;
    delete vmap_table_;
    delete gc_map_;
    delete cfi_info_;
  }
}

}  // namespace art
