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

#include "stack_map_stream.h"

namespace art {

void StackMapStream::BeginStackMapEntry(uint32_t dex_pc,
                                        uint32_t native_pc_offset,
                                        uint32_t register_mask,
                                        BitVector* sp_mask,
                                        uint32_t num_dex_registers,
                                        uint8_t inlining_depth) {
  DCHECK_EQ(0u, current_entry_.dex_pc) << "EndStackMapEntry not called after BeginStackMapEntry";
  current_entry_.dex_pc = dex_pc;
  current_entry_.native_pc_offset = native_pc_offset;
  current_entry_.register_mask = register_mask;
  current_entry_.sp_mask = sp_mask;
  current_entry_.num_dex_registers = num_dex_registers;
  current_entry_.inlining_depth = inlining_depth;
  current_entry_.dex_register_locations_start_index = dex_register_locations_.Size();
  current_entry_.inline_infos_start_index = inline_infos_.Size();
  current_entry_.dex_register_map_hash = 0;
  current_entry_.same_dex_register_map_as_ = kNoSameDexMapFound;
  if (num_dex_registers != 0) {
    current_entry_.live_dex_registers_mask =
        new (allocator_) ArenaBitVector(allocator_, num_dex_registers, true);
  } else {
    current_entry_.live_dex_registers_mask = nullptr;
  }

  if (sp_mask != nullptr) {
    stack_mask_max_ = std::max(stack_mask_max_, sp_mask->GetHighestBitSet());
  }
  if (inlining_depth > 0) {
    number_of_stack_maps_with_inline_info_++;
  }

  dex_pc_max_ = std::max(dex_pc_max_, dex_pc);
  native_pc_offset_max_ = std::max(native_pc_offset_max_, native_pc_offset);
  register_mask_max_ = std::max(register_mask_max_, register_mask);
}

void StackMapStream::EndStackMapEntry() {
  current_entry_.same_dex_register_map_as_ = FindEntryWithTheSameDexMap();
  stack_maps_.Add(current_entry_);
  current_entry_ = StackMapEntry();
}

void StackMapStream::AddDexRegisterEntry(uint16_t dex_register,
                                         DexRegisterLocation::Kind kind,
                                         int32_t value) {
  DCHECK_LT(dex_register, current_entry_.num_dex_registers);

  if (kind != DexRegisterLocation::Kind::kNone) {
    // Ensure we only use non-compressed location kind at this stage.
    DCHECK(DexRegisterLocation::IsShortLocationKind(kind))
        << DexRegisterLocation::PrettyDescriptor(kind);
    DexRegisterLocation location(kind, value);

    // Look for Dex register `location` in the location catalog (using the
    // companion hash map of locations to indices).  Use its index if it
    // is already in the location catalog.  If not, insert it (in the
    // location catalog and the hash map) and use the newly created index.
    auto it = location_catalog_entries_indices_.Find(location);
    if (it != location_catalog_entries_indices_.end()) {
      // Retrieve the index from the hash map.
      dex_register_locations_.Add(it->second);
    } else {
      // Create a new entry in the location catalog and the hash map.
      size_t index = location_catalog_entries_.Size();
      location_catalog_entries_.Add(location);
      dex_register_locations_.Add(index);
      location_catalog_entries_indices_.Insert(std::make_pair(location, index));
    }

    current_entry_.live_dex_registers_mask->SetBit(dex_register);
    current_entry_.dex_register_map_hash +=
      (1 << (dex_register % (sizeof(current_entry_.dex_register_map_hash) * kBitsPerByte)));
    current_entry_.dex_register_map_hash += static_cast<uint32_t>(value);
    current_entry_.dex_register_map_hash += static_cast<uint32_t>(kind);
  }
}

void StackMapStream::AddInlineInfoEntry(uint32_t method_index) {
  InlineInfoEntry entry;
  entry.method_index = method_index;
  inline_infos_.Add(entry);
}

size_t StackMapStream::PrepareForFillIn() {
  int stack_mask_number_of_bits = stack_mask_max_ + 1;  // Need room for max element too.
  stack_mask_size_ = RoundUp(stack_mask_number_of_bits, kBitsPerByte) / kBitsPerByte;
  inline_info_size_ = ComputeInlineInfoSize();
  dex_register_maps_size_ = ComputeDexRegisterMapsSize();
  stack_maps_size_ = stack_maps_.Size()
      * StackMap::ComputeStackMapSize(stack_mask_size_,
                                      inline_info_size_,
                                      dex_register_maps_size_,
                                      dex_pc_max_,
                                      native_pc_offset_max_,
                                      register_mask_max_);
  dex_register_location_catalog_size_ = ComputeDexRegisterLocationCatalogSize();

  // Note: use RoundUp to word-size here if you want CodeInfo objects to be word aligned.
  needed_size_ = CodeInfo::kFixedSize
      + dex_register_location_catalog_size_
      + stack_maps_size_
      + dex_register_maps_size_
      + inline_info_size_;

  dex_register_location_catalog_start_ = CodeInfo::kFixedSize;
  stack_maps_start_ = dex_register_location_catalog_start_ + dex_register_location_catalog_size_;
  dex_register_maps_start_ = stack_maps_start_ + stack_maps_size_;
  inline_infos_start_ = dex_register_maps_start_ + dex_register_maps_size_;

  return needed_size_;
}

size_t StackMapStream::ComputeDexRegisterLocationCatalogSize() const {
  size_t size = DexRegisterLocationCatalog::kFixedSize;
  for (size_t location_catalog_entry_index = 0;
       location_catalog_entry_index < location_catalog_entries_.Size();
       ++location_catalog_entry_index) {
    DexRegisterLocation dex_register_location =
        location_catalog_entries_.Get(location_catalog_entry_index);
    size += DexRegisterLocationCatalog::EntrySize(dex_register_location);
  }
  return size;
}

size_t StackMapStream::ComputeDexRegisterMapSize(const StackMapEntry& entry) const {
  // Size of the map in bytes.
  size_t size = DexRegisterMap::kFixedSize;
  // Add the live bit mask for the Dex register liveness.
  size += DexRegisterMap::GetLiveBitMaskSize(entry.num_dex_registers);
  // Compute the size of the set of live Dex register entries.
  size_t number_of_live_dex_registers = 0;
  for (size_t dex_register_number = 0;
       dex_register_number < entry.num_dex_registers;
       ++dex_register_number) {
    if (entry.live_dex_registers_mask->IsBitSet(dex_register_number)) {
      ++number_of_live_dex_registers;
    }
  }
  size_t map_entries_size_in_bits =
      DexRegisterMap::SingleEntrySizeInBits(location_catalog_entries_.Size())
      * number_of_live_dex_registers;
  size_t map_entries_size_in_bytes =
      RoundUp(map_entries_size_in_bits, kBitsPerByte) / kBitsPerByte;
  size += map_entries_size_in_bytes;
  return size;
}

size_t StackMapStream::ComputeDexRegisterMapsSize() const {
  size_t size = 0;
  for (size_t i = 0; i < stack_maps_.Size(); ++i) {
    StackMapEntry entry = stack_maps_.Get(i);
    if (entry.same_dex_register_map_as_ == kNoSameDexMapFound) {
      // Entries with the same dex map will have the same offset.
      size += ComputeDexRegisterMapSize(entry);
    }
  }
  return size;
}

size_t StackMapStream::ComputeInlineInfoSize() const {
  return inline_infos_.Size() * InlineInfo::SingleEntrySize()
    // For encoding the depth.
    + (number_of_stack_maps_with_inline_info_ * InlineInfo::kFixedSize);
}

void StackMapStream::FillIn(MemoryRegion region) {
  DCHECK_EQ(0u, current_entry_.dex_pc) << "EndStackMapEntry not called after BeginStackMapEntry";
  DCHECK_NE(0u, needed_size_) << "PrepareForFillIn not called before FillIn";

  CodeInfo code_info(region);
  DCHECK_EQ(region.size(), needed_size_);
  code_info.SetOverallSize(region.size());

  MemoryRegion dex_register_locations_region = region.Subregion(
      dex_register_maps_start_, dex_register_maps_size_);

  MemoryRegion inline_infos_region = region.Subregion(
      inline_infos_start_, inline_info_size_);

  code_info.SetEncoding(inline_info_size_,
                        dex_register_maps_size_,
                        dex_pc_max_,
                        native_pc_offset_max_,
                        register_mask_max_);
  code_info.SetNumberOfStackMaps(stack_maps_.Size());
  code_info.SetStackMaskSize(stack_mask_size_);
  DCHECK_EQ(code_info.GetStackMapsSize(), stack_maps_size_);

  // Set the Dex register location catalog.
  code_info.SetNumberOfDexRegisterLocationCatalogEntries(location_catalog_entries_.Size());
  MemoryRegion dex_register_location_catalog_region = region.Subregion(
      dex_register_location_catalog_start_, dex_register_location_catalog_size_);
  DexRegisterLocationCatalog dex_register_location_catalog(dex_register_location_catalog_region);
  // Offset in `dex_register_location_catalog` where to store the next
  // register location.
  size_t location_catalog_offset = DexRegisterLocationCatalog::kFixedSize;
  for (size_t i = 0, e = location_catalog_entries_.Size(); i < e; ++i) {
    DexRegisterLocation dex_register_location = location_catalog_entries_.Get(i);
    dex_register_location_catalog.SetRegisterInfo(location_catalog_offset, dex_register_location);
    location_catalog_offset += DexRegisterLocationCatalog::EntrySize(dex_register_location);
  }
  // Ensure we reached the end of the Dex registers location_catalog.
  DCHECK_EQ(location_catalog_offset, dex_register_location_catalog_region.size());

  uintptr_t next_dex_register_map_offset = 0;
  uintptr_t next_inline_info_offset = 0;
  for (size_t i = 0, e = stack_maps_.Size(); i < e; ++i) {
    StackMap stack_map = code_info.GetStackMapAt(i);
    StackMapEntry entry = stack_maps_.Get(i);

    stack_map.SetDexPc(code_info, entry.dex_pc);
    stack_map.SetNativePcOffset(code_info, entry.native_pc_offset);
    stack_map.SetRegisterMask(code_info, entry.register_mask);
    if (entry.sp_mask != nullptr) {
      stack_map.SetStackMask(code_info, *entry.sp_mask);
    }

    if (entry.num_dex_registers == 0) {
      // No dex map available.
      stack_map.SetDexRegisterMapOffset(code_info, StackMap::kNoDexRegisterMap);
    } else {
      // Search for an entry with the same dex map.
      if (entry.same_dex_register_map_as_ != kNoSameDexMapFound) {
        // If we have a hit reuse the offset.
        stack_map.SetDexRegisterMapOffset(code_info,
            code_info.GetStackMapAt(entry.same_dex_register_map_as_)
                     .GetDexRegisterMapOffset(code_info));
      } else {
        // New dex registers maps should be added to the stack map.
        MemoryRegion register_region =
            dex_register_locations_region.Subregion(
                next_dex_register_map_offset,
                ComputeDexRegisterMapSize(entry));
        next_dex_register_map_offset += register_region.size();
        DexRegisterMap dex_register_map(register_region);
        stack_map.SetDexRegisterMapOffset(
          code_info, register_region.start() - dex_register_locations_region.start());

        // Set the live bit mask.
        dex_register_map.SetLiveBitMask(entry.num_dex_registers, *entry.live_dex_registers_mask);

        // Set the dex register location mapping data.
        for (size_t dex_register_number = 0, index_in_dex_register_locations = 0;
             dex_register_number < entry.num_dex_registers;
             ++dex_register_number) {
          if (entry.live_dex_registers_mask->IsBitSet(dex_register_number)) {
            size_t location_catalog_entry_index =
                dex_register_locations_.Get(entry.dex_register_locations_start_index
                                            + index_in_dex_register_locations);
            dex_register_map.SetLocationCatalogEntryIndex(
                index_in_dex_register_locations,
                location_catalog_entry_index,
                entry.num_dex_registers,
                location_catalog_entries_.Size());
            ++index_in_dex_register_locations;
          }
        }
      }
    }

    // Set the inlining info.
    if (entry.inlining_depth != 0) {
      MemoryRegion inline_region = inline_infos_region.Subregion(
          next_inline_info_offset,
          InlineInfo::kFixedSize + entry.inlining_depth * InlineInfo::SingleEntrySize());
      next_inline_info_offset += inline_region.size();
      InlineInfo inline_info(inline_region);

      // Currently relative to the dex register map.
      stack_map.SetInlineDescriptorOffset(
          code_info, inline_region.start() - dex_register_locations_region.start());

      inline_info.SetDepth(entry.inlining_depth);
      for (size_t j = 0; j < entry.inlining_depth; ++j) {
        InlineInfoEntry inline_entry = inline_infos_.Get(j + entry.inline_infos_start_index);
        inline_info.SetMethodReferenceIndexAtDepth(j, inline_entry.method_index);
      }
    } else {
      if (inline_info_size_ != 0) {
        stack_map.SetInlineDescriptorOffset(code_info, StackMap::kNoInlineInfo);
      }
    }
  }
}

size_t StackMapStream::FindEntryWithTheSameDexMap() {
  size_t current_entry_index = stack_maps_.Size();
  auto entries_it = dex_map_hash_to_stack_map_indices_.find(current_entry_.dex_register_map_hash);
  if (entries_it == dex_map_hash_to_stack_map_indices_.end()) {
    // We don't have a perfect hash functions so we need a list to collect all stack maps
    // which might have the same dex register map.
    GrowableArray<uint32_t> stack_map_indices(allocator_, 1);
    stack_map_indices.Add(current_entry_index);
    dex_map_hash_to_stack_map_indices_.Put(current_entry_.dex_register_map_hash, stack_map_indices);
    return kNoSameDexMapFound;
  }

  // We might have collisions, so we need to check whether or not we really have a match.
  for (size_t i = 0; i < entries_it->second.Size(); i++) {
    size_t test_entry_index = entries_it->second.Get(i);
    if (HaveTheSameDexMaps(stack_maps_.Get(test_entry_index), current_entry_)) {
      return test_entry_index;
    }
  }
  entries_it->second.Add(current_entry_index);
  return kNoSameDexMapFound;
}

bool StackMapStream::HaveTheSameDexMaps(const StackMapEntry& a, const StackMapEntry& b) const {
  if (a.live_dex_registers_mask == nullptr && b.live_dex_registers_mask == nullptr) {
    return true;
  }
  if (a.live_dex_registers_mask == nullptr || b.live_dex_registers_mask == nullptr) {
    return false;
  }
  if (a.num_dex_registers != b.num_dex_registers) {
    return false;
  }

  int index_in_dex_register_locations = 0;
  for (uint32_t i = 0; i < a.num_dex_registers; i++) {
    if (a.live_dex_registers_mask->IsBitSet(i) != b.live_dex_registers_mask->IsBitSet(i)) {
      return false;
    }
    if (a.live_dex_registers_mask->IsBitSet(i)) {
      size_t a_loc = dex_register_locations_.Get(
          a.dex_register_locations_start_index + index_in_dex_register_locations);
      size_t b_loc = dex_register_locations_.Get(
          b.dex_register_locations_start_index + index_in_dex_register_locations);
      if (a_loc != b_loc) {
        return false;
      }
      ++index_in_dex_register_locations;
    }
  }
  return true;
}

}  // namespace art
