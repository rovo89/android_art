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
  DCHECK_NE(dex_pc, static_cast<uint32_t>(-1)) << "invalid dex_pc";
  current_entry_.dex_pc = dex_pc;
  current_entry_.native_pc_offset = native_pc_offset;
  current_entry_.register_mask = register_mask;
  current_entry_.sp_mask = sp_mask;
  current_entry_.num_dex_registers = num_dex_registers;
  current_entry_.inlining_depth = inlining_depth;
  current_entry_.dex_register_locations_start_index = dex_register_locations_.size();
  current_entry_.inline_infos_start_index = inline_infos_.size();
  current_entry_.dex_register_map_hash = 0;
  current_entry_.same_dex_register_map_as_ = kNoSameDexMapFound;
  if (num_dex_registers != 0) {
    current_entry_.live_dex_registers_mask =
        ArenaBitVector::Create(allocator_, num_dex_registers, true, kArenaAllocStackMapStream);
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
  register_mask_max_ = std::max(register_mask_max_, register_mask);
  current_dex_register_ = 0;
}

void StackMapStream::EndStackMapEntry() {
  current_entry_.same_dex_register_map_as_ = FindEntryWithTheSameDexMap();
  stack_maps_.push_back(current_entry_);
  current_entry_ = StackMapEntry();
}

void StackMapStream::AddDexRegisterEntry(DexRegisterLocation::Kind kind, int32_t value) {
  if (kind != DexRegisterLocation::Kind::kNone) {
    // Ensure we only use non-compressed location kind at this stage.
    DCHECK(DexRegisterLocation::IsShortLocationKind(kind)) << kind;
    DexRegisterLocation location(kind, value);

    // Look for Dex register `location` in the location catalog (using the
    // companion hash map of locations to indices).  Use its index if it
    // is already in the location catalog.  If not, insert it (in the
    // location catalog and the hash map) and use the newly created index.
    auto it = location_catalog_entries_indices_.Find(location);
    if (it != location_catalog_entries_indices_.end()) {
      // Retrieve the index from the hash map.
      dex_register_locations_.push_back(it->second);
    } else {
      // Create a new entry in the location catalog and the hash map.
      size_t index = location_catalog_entries_.size();
      location_catalog_entries_.push_back(location);
      dex_register_locations_.push_back(index);
      location_catalog_entries_indices_.Insert(std::make_pair(location, index));
    }

    if (in_inline_frame_) {
      // TODO: Support sharing DexRegisterMap across InlineInfo.
      DCHECK_LT(current_dex_register_, current_inline_info_.num_dex_registers);
      current_inline_info_.live_dex_registers_mask->SetBit(current_dex_register_);
    } else {
      DCHECK_LT(current_dex_register_, current_entry_.num_dex_registers);
      current_entry_.live_dex_registers_mask->SetBit(current_dex_register_);
      current_entry_.dex_register_map_hash += (1 <<
          (current_dex_register_ % (sizeof(current_entry_.dex_register_map_hash) * kBitsPerByte)));
      current_entry_.dex_register_map_hash += static_cast<uint32_t>(value);
      current_entry_.dex_register_map_hash += static_cast<uint32_t>(kind);
    }
  }
  current_dex_register_++;
}

void StackMapStream::BeginInlineInfoEntry(uint32_t method_index,
                                          uint32_t dex_pc,
                                          InvokeType invoke_type,
                                          uint32_t num_dex_registers) {
  DCHECK(!in_inline_frame_);
  in_inline_frame_ = true;
  current_inline_info_.method_index = method_index;
  current_inline_info_.dex_pc = dex_pc;
  current_inline_info_.invoke_type = invoke_type;
  current_inline_info_.num_dex_registers = num_dex_registers;
  current_inline_info_.dex_register_locations_start_index = dex_register_locations_.size();
  if (num_dex_registers != 0) {
    current_inline_info_.live_dex_registers_mask =
        ArenaBitVector::Create(allocator_, num_dex_registers, true, kArenaAllocStackMapStream);
  } else {
    current_inline_info_.live_dex_registers_mask = nullptr;
  }
  current_dex_register_ = 0;
}

void StackMapStream::EndInlineInfoEntry() {
  DCHECK(in_inline_frame_);
  DCHECK_EQ(current_dex_register_, current_inline_info_.num_dex_registers)
      << "Inline information contains less registers than expected";
  in_inline_frame_ = false;
  inline_infos_.push_back(current_inline_info_);
  current_inline_info_ = InlineInfoEntry();
}

uint32_t StackMapStream::ComputeMaxNativePcOffset() const {
  uint32_t max_native_pc_offset = 0u;
  for (const StackMapEntry& entry : stack_maps_) {
    max_native_pc_offset = std::max(max_native_pc_offset, entry.native_pc_offset);
  }
  return max_native_pc_offset;
}

size_t StackMapStream::PrepareForFillIn() {
  int stack_mask_number_of_bits = stack_mask_max_ + 1;  // Need room for max element too.
  dex_register_maps_size_ = ComputeDexRegisterMapsSize();
  ComputeInlineInfoEncoding();  // needs dex_register_maps_size_.
  inline_info_size_ = inline_infos_.size() * inline_info_encoding_.GetEntrySize();
  uint32_t max_native_pc_offset = ComputeMaxNativePcOffset();
  size_t stack_map_size = stack_map_encoding_.SetFromSizes(max_native_pc_offset,
                                                           dex_pc_max_,
                                                           dex_register_maps_size_,
                                                           inline_info_size_,
                                                           register_mask_max_,
                                                           stack_mask_number_of_bits);
  stack_maps_size_ = stack_maps_.size() * stack_map_size;
  dex_register_location_catalog_size_ = ComputeDexRegisterLocationCatalogSize();

  size_t non_header_size =
      stack_maps_size_ +
      dex_register_location_catalog_size_ +
      dex_register_maps_size_ +
      inline_info_size_;

  // Prepare the CodeInfo variable-sized encoding.
  CodeInfoEncoding code_info_encoding;
  code_info_encoding.non_header_size = non_header_size;
  code_info_encoding.number_of_stack_maps = stack_maps_.size();
  code_info_encoding.stack_map_size_in_bytes = stack_map_size;
  code_info_encoding.stack_map_encoding = stack_map_encoding_;
  code_info_encoding.inline_info_encoding = inline_info_encoding_;
  code_info_encoding.number_of_location_catalog_entries = location_catalog_entries_.size();
  code_info_encoding.Compress(&code_info_encoding_);

  // TODO: Move the catalog at the end. It is currently too expensive at runtime
  // to compute its size (note that we do not encode that size in the CodeInfo).
  dex_register_location_catalog_start_ = code_info_encoding_.size() + stack_maps_size_;
  dex_register_maps_start_ =
      dex_register_location_catalog_start_ + dex_register_location_catalog_size_;
  inline_infos_start_ = dex_register_maps_start_ + dex_register_maps_size_;

  needed_size_ = code_info_encoding_.size() + non_header_size;
  return needed_size_;
}

size_t StackMapStream::ComputeDexRegisterLocationCatalogSize() const {
  size_t size = DexRegisterLocationCatalog::kFixedSize;
  for (const DexRegisterLocation& dex_register_location : location_catalog_entries_) {
    size += DexRegisterLocationCatalog::EntrySize(dex_register_location);
  }
  return size;
}

size_t StackMapStream::ComputeDexRegisterMapSize(uint32_t num_dex_registers,
                                                 const BitVector* live_dex_registers_mask) const {
  // For num_dex_registers == 0u live_dex_registers_mask may be null.
  if (num_dex_registers == 0u) {
    return 0u;  // No register map will be emitted.
  }
  DCHECK(live_dex_registers_mask != nullptr);

  // Size of the map in bytes.
  size_t size = DexRegisterMap::kFixedSize;
  // Add the live bit mask for the Dex register liveness.
  size += DexRegisterMap::GetLiveBitMaskSize(num_dex_registers);
  // Compute the size of the set of live Dex register entries.
  size_t number_of_live_dex_registers = live_dex_registers_mask->NumSetBits();
  size_t map_entries_size_in_bits =
      DexRegisterMap::SingleEntrySizeInBits(location_catalog_entries_.size())
      * number_of_live_dex_registers;
  size_t map_entries_size_in_bytes =
      RoundUp(map_entries_size_in_bits, kBitsPerByte) / kBitsPerByte;
  size += map_entries_size_in_bytes;
  return size;
}

size_t StackMapStream::ComputeDexRegisterMapsSize() const {
  size_t size = 0;
  size_t inline_info_index = 0;
  for (const StackMapEntry& entry : stack_maps_) {
    if (entry.same_dex_register_map_as_ == kNoSameDexMapFound) {
      size += ComputeDexRegisterMapSize(entry.num_dex_registers, entry.live_dex_registers_mask);
    } else {
      // Entries with the same dex map will have the same offset.
    }
    for (size_t j = 0; j < entry.inlining_depth; ++j) {
      InlineInfoEntry inline_entry = inline_infos_[inline_info_index++];
      size += ComputeDexRegisterMapSize(inline_entry.num_dex_registers,
                                        inline_entry.live_dex_registers_mask);
    }
  }
  return size;
}

void StackMapStream::ComputeInlineInfoEncoding() {
  uint32_t method_index_max = 0;
  uint32_t dex_pc_max = 0;
  uint32_t invoke_type_max = 0;

  uint32_t inline_info_index = 0;
  for (const StackMapEntry& entry : stack_maps_) {
    for (size_t j = 0; j < entry.inlining_depth; ++j) {
      InlineInfoEntry inline_entry = inline_infos_[inline_info_index++];
      method_index_max = std::max(method_index_max, inline_entry.method_index);
      dex_pc_max = std::max(dex_pc_max, inline_entry.dex_pc);
      invoke_type_max = std::max(invoke_type_max, static_cast<uint32_t>(inline_entry.invoke_type));
    }
  }
  DCHECK_EQ(inline_info_index, inline_infos_.size());

  inline_info_encoding_.SetFromSizes(method_index_max,
                                     dex_pc_max,
                                     invoke_type_max,
                                     dex_register_maps_size_);
}

void StackMapStream::FillIn(MemoryRegion region) {
  DCHECK_EQ(0u, current_entry_.dex_pc) << "EndStackMapEntry not called after BeginStackMapEntry";
  DCHECK_NE(0u, needed_size_) << "PrepareForFillIn not called before FillIn";

  DCHECK_EQ(region.size(), needed_size_);

  // Note that the memory region does not have to be zeroed when we JIT code
  // because we do not use the arena allocator there.

  // Write the CodeInfo header.
  region.CopyFrom(0, MemoryRegion(code_info_encoding_.data(), code_info_encoding_.size()));

  MemoryRegion dex_register_locations_region = region.Subregion(
      dex_register_maps_start_, dex_register_maps_size_);

  MemoryRegion inline_infos_region = region.Subregion(
      inline_infos_start_, inline_info_size_);

  CodeInfo code_info(region);
  CodeInfoEncoding encoding = code_info.ExtractEncoding();
  DCHECK_EQ(code_info.GetStackMapsSize(encoding), stack_maps_size_);

  // Set the Dex register location catalog.
  MemoryRegion dex_register_location_catalog_region = region.Subregion(
      dex_register_location_catalog_start_, dex_register_location_catalog_size_);
  DexRegisterLocationCatalog dex_register_location_catalog(dex_register_location_catalog_region);
  // Offset in `dex_register_location_catalog` where to store the next
  // register location.
  size_t location_catalog_offset = DexRegisterLocationCatalog::kFixedSize;
  for (DexRegisterLocation dex_register_location : location_catalog_entries_) {
    dex_register_location_catalog.SetRegisterInfo(location_catalog_offset, dex_register_location);
    location_catalog_offset += DexRegisterLocationCatalog::EntrySize(dex_register_location);
  }
  // Ensure we reached the end of the Dex registers location_catalog.
  DCHECK_EQ(location_catalog_offset, dex_register_location_catalog_region.size());

  ArenaBitVector empty_bitmask(allocator_, 0, /* expandable */ false, kArenaAllocStackMapStream);
  uintptr_t next_dex_register_map_offset = 0;
  uintptr_t next_inline_info_offset = 0;
  for (size_t i = 0, e = stack_maps_.size(); i < e; ++i) {
    StackMap stack_map = code_info.GetStackMapAt(i, encoding);
    StackMapEntry entry = stack_maps_[i];

    stack_map.SetDexPc(stack_map_encoding_, entry.dex_pc);
    stack_map.SetNativePcOffset(stack_map_encoding_, entry.native_pc_offset);
    stack_map.SetRegisterMask(stack_map_encoding_, entry.register_mask);
    size_t number_of_stack_mask_bits = stack_map.GetNumberOfStackMaskBits(stack_map_encoding_);
    if (entry.sp_mask != nullptr) {
      for (size_t bit = 0; bit < number_of_stack_mask_bits; bit++) {
        stack_map.SetStackMaskBit(stack_map_encoding_, bit, entry.sp_mask->IsBitSet(bit));
      }
    } else {
      // The MemoryRegion does not have to be zeroed, so make sure we clear the bits.
      for (size_t bit = 0; bit < number_of_stack_mask_bits; bit++) {
        stack_map.SetStackMaskBit(stack_map_encoding_, bit, false);
      }
    }

    if (entry.num_dex_registers == 0 || (entry.live_dex_registers_mask->NumSetBits() == 0)) {
      // No dex map available.
      stack_map.SetDexRegisterMapOffset(stack_map_encoding_, StackMap::kNoDexRegisterMap);
    } else {
      // Search for an entry with the same dex map.
      if (entry.same_dex_register_map_as_ != kNoSameDexMapFound) {
        // If we have a hit reuse the offset.
        stack_map.SetDexRegisterMapOffset(
            stack_map_encoding_,
            code_info.GetStackMapAt(entry.same_dex_register_map_as_, encoding)
                .GetDexRegisterMapOffset(stack_map_encoding_));
      } else {
        // New dex registers maps should be added to the stack map.
        MemoryRegion register_region = dex_register_locations_region.Subregion(
            next_dex_register_map_offset,
            ComputeDexRegisterMapSize(entry.num_dex_registers, entry.live_dex_registers_mask));
        next_dex_register_map_offset += register_region.size();
        DexRegisterMap dex_register_map(register_region);
        stack_map.SetDexRegisterMapOffset(
            stack_map_encoding_, register_region.start() - dex_register_locations_region.start());

        // Set the dex register location.
        FillInDexRegisterMap(dex_register_map,
                             entry.num_dex_registers,
                             *entry.live_dex_registers_mask,
                             entry.dex_register_locations_start_index);
      }
    }

    // Set the inlining info.
    if (entry.inlining_depth != 0) {
      MemoryRegion inline_region = inline_infos_region.Subregion(
          next_inline_info_offset,
          entry.inlining_depth * inline_info_encoding_.GetEntrySize());
      next_inline_info_offset += inline_region.size();
      InlineInfo inline_info(inline_region);

      // Currently relative to the dex register map.
      stack_map.SetInlineDescriptorOffset(
          stack_map_encoding_, inline_region.start() - dex_register_locations_region.start());

      inline_info.SetDepth(inline_info_encoding_, entry.inlining_depth);
      DCHECK_LE(entry.inline_infos_start_index + entry.inlining_depth, inline_infos_.size());
      for (size_t depth = 0; depth < entry.inlining_depth; ++depth) {
        InlineInfoEntry inline_entry = inline_infos_[depth + entry.inline_infos_start_index];
        inline_info.SetMethodIndexAtDepth(inline_info_encoding_, depth, inline_entry.method_index);
        inline_info.SetDexPcAtDepth(inline_info_encoding_, depth, inline_entry.dex_pc);
        inline_info.SetInvokeTypeAtDepth(inline_info_encoding_, depth, inline_entry.invoke_type);
        if (inline_entry.num_dex_registers == 0) {
          // No dex map available.
          inline_info.SetDexRegisterMapOffsetAtDepth(inline_info_encoding_,
                                                     depth,
                                                     StackMap::kNoDexRegisterMap);
          DCHECK(inline_entry.live_dex_registers_mask == nullptr);
        } else {
          MemoryRegion register_region = dex_register_locations_region.Subregion(
              next_dex_register_map_offset,
              ComputeDexRegisterMapSize(inline_entry.num_dex_registers,
                                        inline_entry.live_dex_registers_mask));
          next_dex_register_map_offset += register_region.size();
          DexRegisterMap dex_register_map(register_region);
          inline_info.SetDexRegisterMapOffsetAtDepth(
              inline_info_encoding_,
              depth, register_region.start() - dex_register_locations_region.start());

          FillInDexRegisterMap(dex_register_map,
                               inline_entry.num_dex_registers,
                               *inline_entry.live_dex_registers_mask,
                               inline_entry.dex_register_locations_start_index);
        }
      }
    } else {
      if (inline_info_size_ != 0) {
        stack_map.SetInlineDescriptorOffset(stack_map_encoding_, StackMap::kNoInlineInfo);
      }
    }
  }

  // Verify all written data in debug build.
  if (kIsDebugBuild) {
    CheckCodeInfo(region);
  }
}

void StackMapStream::FillInDexRegisterMap(DexRegisterMap dex_register_map,
                                          uint32_t num_dex_registers,
                                          const BitVector& live_dex_registers_mask,
                                          uint32_t start_index_in_dex_register_locations) const {
  dex_register_map.SetLiveBitMask(num_dex_registers, live_dex_registers_mask);
  // Set the dex register location mapping data.
  size_t number_of_live_dex_registers = live_dex_registers_mask.NumSetBits();
  DCHECK_LE(number_of_live_dex_registers, dex_register_locations_.size());
  DCHECK_LE(start_index_in_dex_register_locations,
            dex_register_locations_.size() - number_of_live_dex_registers);
  for (size_t index_in_dex_register_locations = 0;
      index_in_dex_register_locations != number_of_live_dex_registers;
       ++index_in_dex_register_locations) {
    size_t location_catalog_entry_index = dex_register_locations_[
        start_index_in_dex_register_locations + index_in_dex_register_locations];
    dex_register_map.SetLocationCatalogEntryIndex(
        index_in_dex_register_locations,
        location_catalog_entry_index,
        num_dex_registers,
        location_catalog_entries_.size());
  }
}

size_t StackMapStream::FindEntryWithTheSameDexMap() {
  size_t current_entry_index = stack_maps_.size();
  auto entries_it = dex_map_hash_to_stack_map_indices_.find(current_entry_.dex_register_map_hash);
  if (entries_it == dex_map_hash_to_stack_map_indices_.end()) {
    // We don't have a perfect hash functions so we need a list to collect all stack maps
    // which might have the same dex register map.
    ArenaVector<uint32_t> stack_map_indices(allocator_->Adapter(kArenaAllocStackMapStream));
    stack_map_indices.push_back(current_entry_index);
    dex_map_hash_to_stack_map_indices_.Put(current_entry_.dex_register_map_hash,
                                           std::move(stack_map_indices));
    return kNoSameDexMapFound;
  }

  // We might have collisions, so we need to check whether or not we really have a match.
  for (uint32_t test_entry_index : entries_it->second) {
    if (HaveTheSameDexMaps(GetStackMap(test_entry_index), current_entry_)) {
      return test_entry_index;
    }
  }
  entries_it->second.push_back(current_entry_index);
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
  if (a.num_dex_registers != 0u) {
    DCHECK(a.live_dex_registers_mask != nullptr);
    DCHECK(b.live_dex_registers_mask != nullptr);
    if (!a.live_dex_registers_mask->Equal(b.live_dex_registers_mask)) {
      return false;
    }
    size_t number_of_live_dex_registers = a.live_dex_registers_mask->NumSetBits();
    DCHECK_LE(number_of_live_dex_registers, dex_register_locations_.size());
    DCHECK_LE(a.dex_register_locations_start_index,
              dex_register_locations_.size() - number_of_live_dex_registers);
    DCHECK_LE(b.dex_register_locations_start_index,
              dex_register_locations_.size() - number_of_live_dex_registers);
    auto a_begin = dex_register_locations_.begin() + a.dex_register_locations_start_index;
    auto b_begin = dex_register_locations_.begin() + b.dex_register_locations_start_index;
    if (!std::equal(a_begin, a_begin + number_of_live_dex_registers, b_begin)) {
      return false;
    }
  }
  return true;
}

// Helper for CheckCodeInfo - check that register map has the expected content.
void StackMapStream::CheckDexRegisterMap(const CodeInfo& code_info,
                                         const DexRegisterMap& dex_register_map,
                                         size_t num_dex_registers,
                                         BitVector* live_dex_registers_mask,
                                         size_t dex_register_locations_index) const {
  CodeInfoEncoding encoding = code_info.ExtractEncoding();
  for (size_t reg = 0; reg < num_dex_registers; reg++) {
    // Find the location we tried to encode.
    DexRegisterLocation expected = DexRegisterLocation::None();
    if (live_dex_registers_mask->IsBitSet(reg)) {
      size_t catalog_index = dex_register_locations_[dex_register_locations_index++];
      expected = location_catalog_entries_[catalog_index];
    }
    // Compare to the seen location.
    if (expected.GetKind() == DexRegisterLocation::Kind::kNone) {
      DCHECK(!dex_register_map.IsValid() || !dex_register_map.IsDexRegisterLive(reg));
    } else {
      DCHECK(dex_register_map.IsDexRegisterLive(reg));
      DexRegisterLocation seen = dex_register_map.GetDexRegisterLocation(
          reg, num_dex_registers, code_info, encoding);
      DCHECK_EQ(expected.GetKind(), seen.GetKind());
      DCHECK_EQ(expected.GetValue(), seen.GetValue());
    }
  }
  if (num_dex_registers == 0) {
    DCHECK(!dex_register_map.IsValid());
  }
}

// Check that all StackMapStream inputs are correctly encoded by trying to read them back.
void StackMapStream::CheckCodeInfo(MemoryRegion region) const {
  CodeInfo code_info(region);
  CodeInfoEncoding encoding = code_info.ExtractEncoding();
  DCHECK_EQ(code_info.GetNumberOfStackMaps(encoding), stack_maps_.size());
  for (size_t s = 0; s < stack_maps_.size(); ++s) {
    const StackMap stack_map = code_info.GetStackMapAt(s, encoding);
    const StackMapEncoding& stack_map_encoding = encoding.stack_map_encoding;
    StackMapEntry entry = stack_maps_[s];

    // Check main stack map fields.
    DCHECK_EQ(stack_map.GetNativePcOffset(stack_map_encoding), entry.native_pc_offset);
    DCHECK_EQ(stack_map.GetDexPc(stack_map_encoding), entry.dex_pc);
    DCHECK_EQ(stack_map.GetRegisterMask(stack_map_encoding), entry.register_mask);
    size_t num_stack_mask_bits = stack_map.GetNumberOfStackMaskBits(stack_map_encoding);
    if (entry.sp_mask != nullptr) {
      DCHECK_GE(num_stack_mask_bits, entry.sp_mask->GetNumberOfBits());
      for (size_t b = 0; b < num_stack_mask_bits; b++) {
        DCHECK_EQ(stack_map.GetStackMaskBit(stack_map_encoding, b), entry.sp_mask->IsBitSet(b));
      }
    } else {
      for (size_t b = 0; b < num_stack_mask_bits; b++) {
        DCHECK_EQ(stack_map.GetStackMaskBit(stack_map_encoding, b), 0u);
      }
    }

    CheckDexRegisterMap(code_info,
                        code_info.GetDexRegisterMapOf(
                            stack_map, encoding, entry.num_dex_registers),
                        entry.num_dex_registers,
                        entry.live_dex_registers_mask,
                        entry.dex_register_locations_start_index);

    // Check inline info.
    DCHECK_EQ(stack_map.HasInlineInfo(stack_map_encoding), (entry.inlining_depth != 0));
    if (entry.inlining_depth != 0) {
      InlineInfo inline_info = code_info.GetInlineInfoOf(stack_map, encoding);
      DCHECK_EQ(inline_info.GetDepth(encoding.inline_info_encoding), entry.inlining_depth);
      for (size_t d = 0; d < entry.inlining_depth; ++d) {
        size_t inline_info_index = entry.inline_infos_start_index + d;
        DCHECK_LT(inline_info_index, inline_infos_.size());
        InlineInfoEntry inline_entry = inline_infos_[inline_info_index];
        DCHECK_EQ(inline_info.GetDexPcAtDepth(encoding.inline_info_encoding, d),
                  inline_entry.dex_pc);
        DCHECK_EQ(inline_info.GetMethodIndexAtDepth(encoding.inline_info_encoding, d),
                  inline_entry.method_index);
        DCHECK_EQ(inline_info.GetInvokeTypeAtDepth(encoding.inline_info_encoding, d),
                  inline_entry.invoke_type);

        CheckDexRegisterMap(code_info,
                            code_info.GetDexRegisterMapAtDepth(
                                d, inline_info, encoding, inline_entry.num_dex_registers),
                            inline_entry.num_dex_registers,
                            inline_entry.live_dex_registers_mask,
                            inline_entry.dex_register_locations_start_index);
      }
    }
  }
}

}  // namespace art
