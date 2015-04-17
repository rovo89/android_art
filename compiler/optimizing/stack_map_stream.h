/*
 * Copyright (C) 2014 The Android Open Source Project
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

#ifndef ART_COMPILER_OPTIMIZING_STACK_MAP_STREAM_H_
#define ART_COMPILER_OPTIMIZING_STACK_MAP_STREAM_H_

#include "base/arena_containers.h"
#include "base/bit_vector-inl.h"
#include "base/value_object.h"
#include "memory_region.h"
#include "nodes.h"
#include "stack_map.h"
#include "utils/growable_array.h"

namespace art {

// Helper to build art::StackMapStream::LocationCatalogEntriesIndices.
class LocationCatalogEntriesIndicesEmptyFn {
 public:
  void MakeEmpty(std::pair<DexRegisterLocation, size_t>& item) const {
    item.first = DexRegisterLocation::None();
  }
  bool IsEmpty(const std::pair<DexRegisterLocation, size_t>& item) const {
    return item.first == DexRegisterLocation::None();
  }
};

// Hash function for art::StackMapStream::LocationCatalogEntriesIndices.
// This hash function does not create collisions.
class DexRegisterLocationHashFn {
 public:
  size_t operator()(DexRegisterLocation key) const {
    // Concatenate `key`s fields to create a 64-bit value to be hashed.
    int64_t kind_and_value =
        (static_cast<int64_t>(key.kind_) << 32) | static_cast<int64_t>(key.value_);
    return inner_hash_fn_(kind_and_value);
  }
 private:
  std::hash<int64_t> inner_hash_fn_;
};


/**
 * Collects and builds stack maps for a method. All the stack maps
 * for a method are placed in a CodeInfo object.
 */
class StackMapStream : public ValueObject {
 public:
  explicit StackMapStream(ArenaAllocator* allocator)
      : allocator_(allocator),
        stack_maps_(allocator, 10),
        location_catalog_entries_(allocator, 4),
        dex_register_locations_(allocator, 10 * 4),
        inline_infos_(allocator, 2),
        stack_mask_max_(-1),
        dex_pc_max_(0),
        native_pc_offset_max_(0),
        register_mask_max_(0),
        number_of_stack_maps_with_inline_info_(0),
        dex_map_hash_to_stack_map_indices_(std::less<uint32_t>(), allocator->Adapter()) {}

  // Compute bytes needed to encode a mask with the given maximum element.
  static uint32_t StackMaskEncodingSize(int max_element) {
    int number_of_bits = max_element + 1;  // Need room for max element too.
    return RoundUp(number_of_bits, kBitsPerByte) / kBitsPerByte;
  }

  // See runtime/stack_map.h to know what these fields contain.
  struct StackMapEntry {
    uint32_t dex_pc;
    uint32_t native_pc_offset;
    uint32_t register_mask;
    BitVector* sp_mask;
    uint32_t num_dex_registers;
    uint8_t inlining_depth;
    size_t dex_register_locations_start_index;
    size_t inline_infos_start_index;
    BitVector* live_dex_registers_mask;
    uint32_t dex_register_map_hash;
  };

  struct InlineInfoEntry {
    uint32_t method_index;
  };

  void AddStackMapEntry(uint32_t dex_pc,
                        uint32_t native_pc_offset,
                        uint32_t register_mask,
                        BitVector* sp_mask,
                        uint32_t num_dex_registers,
                        uint8_t inlining_depth) {
    StackMapEntry entry;
    entry.dex_pc = dex_pc;
    entry.native_pc_offset = native_pc_offset;
    entry.register_mask = register_mask;
    entry.sp_mask = sp_mask;
    entry.num_dex_registers = num_dex_registers;
    entry.inlining_depth = inlining_depth;
    entry.dex_register_locations_start_index = dex_register_locations_.Size();
    entry.inline_infos_start_index = inline_infos_.Size();
    entry.dex_register_map_hash = 0;
    if (num_dex_registers != 0) {
      entry.live_dex_registers_mask =
          new (allocator_) ArenaBitVector(allocator_, num_dex_registers, true);
    } else {
      entry.live_dex_registers_mask = nullptr;
    }
    stack_maps_.Add(entry);

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

  void AddInlineInfoEntry(uint32_t method_index) {
    InlineInfoEntry entry;
    entry.method_index = method_index;
    inline_infos_.Add(entry);
  }

  size_t ComputeNeededSize() {
    size_t size = CodeInfo::kFixedSize
        + ComputeDexRegisterLocationCatalogSize()
        + ComputeStackMapsSize()
        + ComputeDexRegisterMapsSize()
        + ComputeInlineInfoSize();
    // Note: use RoundUp to word-size here if you want CodeInfo objects to be word aligned.
    return size;
  }

  size_t ComputeStackMaskSize() const {
    return StackMaskEncodingSize(stack_mask_max_);
  }

  size_t ComputeStackMapsSize() {
    return stack_maps_.Size() * StackMap::ComputeStackMapSize(
        ComputeStackMaskSize(),
        ComputeInlineInfoSize(),
        ComputeDexRegisterMapsSize(),
        dex_pc_max_,
        native_pc_offset_max_,
        register_mask_max_);
  }

  // Compute the size of the Dex register location catalog of `entry`.
  size_t ComputeDexRegisterLocationCatalogSize() const {
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

  size_t ComputeDexRegisterMapSize(const StackMapEntry& entry) const {
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

  // Compute the size of all the Dex register maps.
  size_t ComputeDexRegisterMapsSize() {
    size_t size = 0;
    for (size_t i = 0; i < stack_maps_.Size(); ++i) {
      if (FindEntryWithTheSameDexMap(i) == kNoSameDexMapFound) {
        // Entries with the same dex map will have the same offset.
        size += ComputeDexRegisterMapSize(stack_maps_.Get(i));
      }
    }
    return size;
  }

  // Compute the size of all the inline information pieces.
  size_t ComputeInlineInfoSize() const {
    return inline_infos_.Size() * InlineInfo::SingleEntrySize()
      // For encoding the depth.
      + (number_of_stack_maps_with_inline_info_ * InlineInfo::kFixedSize);
  }

  size_t ComputeDexRegisterLocationCatalogStart() const {
    return CodeInfo::kFixedSize;
  }

  size_t ComputeStackMapsStart() const {
    return ComputeDexRegisterLocationCatalogStart() + ComputeDexRegisterLocationCatalogSize();
  }

  size_t ComputeDexRegisterMapsStart() {
    return ComputeStackMapsStart() + ComputeStackMapsSize();
  }

  size_t ComputeInlineInfoStart() {
    return ComputeDexRegisterMapsStart() + ComputeDexRegisterMapsSize();
  }

  void FillIn(MemoryRegion region) {
    CodeInfo code_info(region);
    DCHECK_EQ(region.size(), ComputeNeededSize());
    code_info.SetOverallSize(region.size());

    size_t stack_mask_size = ComputeStackMaskSize();

    size_t dex_register_map_size = ComputeDexRegisterMapsSize();
    size_t inline_info_size = ComputeInlineInfoSize();

    MemoryRegion dex_register_locations_region = region.Subregion(
      ComputeDexRegisterMapsStart(),
      dex_register_map_size);

    MemoryRegion inline_infos_region = region.Subregion(
      ComputeInlineInfoStart(),
      inline_info_size);

    code_info.SetEncoding(inline_info_size,
                          dex_register_map_size,
                          dex_pc_max_,
                          native_pc_offset_max_,
                          register_mask_max_);
    code_info.SetNumberOfStackMaps(stack_maps_.Size());
    code_info.SetStackMaskSize(stack_mask_size);
    DCHECK_EQ(code_info.GetStackMapsSize(), ComputeStackMapsSize());

    // Set the Dex register location catalog.
    code_info.SetNumberOfDexRegisterLocationCatalogEntries(
        location_catalog_entries_.Size());
    MemoryRegion dex_register_location_catalog_region = region.Subregion(
        ComputeDexRegisterLocationCatalogStart(),
        ComputeDexRegisterLocationCatalogSize());
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
        size_t entry_with_same_map = FindEntryWithTheSameDexMap(i);
        if (entry_with_same_map != kNoSameDexMapFound) {
          // If we have a hit reuse the offset.
          stack_map.SetDexRegisterMapOffset(code_info,
              code_info.GetStackMapAt(entry_with_same_map).GetDexRegisterMapOffset(code_info));
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
        if (inline_info_size != 0) {
          stack_map.SetInlineDescriptorOffset(code_info, StackMap::kNoInlineInfo);
        }
      }
    }
  }

  void AddDexRegisterEntry(uint16_t dex_register, DexRegisterLocation::Kind kind, int32_t value) {
    StackMapEntry entry = stack_maps_.Get(stack_maps_.Size() - 1);
    DCHECK_LT(dex_register, entry.num_dex_registers);

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

      entry.live_dex_registers_mask->SetBit(dex_register);
      entry.dex_register_map_hash +=
        (1 << (dex_register % (sizeof(entry.dex_register_map_hash) * kBitsPerByte)));
      entry.dex_register_map_hash += static_cast<uint32_t>(value);
      entry.dex_register_map_hash += static_cast<uint32_t>(kind);
      stack_maps_.Put(stack_maps_.Size() - 1, entry);
    }
  }

 private:
  // Returns the index of an entry with the same dex register map
  // or kNoSameDexMapFound if no such entry exists.
  size_t FindEntryWithTheSameDexMap(size_t entry_index) {
    StackMapEntry entry = stack_maps_.Get(entry_index);
    auto entries_it = dex_map_hash_to_stack_map_indices_.find(entry.dex_register_map_hash);
    if (entries_it == dex_map_hash_to_stack_map_indices_.end()) {
      // We don't have a perfect hash functions so we need a list to collect all stack maps
      // which might have the same dex register map.
      GrowableArray<uint32_t> stack_map_indices(allocator_, 1);
      stack_map_indices.Add(entry_index);
      dex_map_hash_to_stack_map_indices_.Put(entry.dex_register_map_hash, stack_map_indices);
      return kNoSameDexMapFound;
    }

    // TODO: We don't need to add ourselves to the map if we can guarantee that
    // FindEntryWithTheSameDexMap is called just once per stack map entry.
    // A good way to do this is to cache the offset in the stack map entry. This
    // is easier to do if we add markers when the stack map constructions begins
    // and when it ends.

    // We might have collisions, so we need to check whether or not we should
    // add the entry to the map. `needs_to_be_added` keeps track of this.
    bool needs_to_be_added = true;
    size_t result = kNoSameDexMapFound;
    for (size_t i = 0; i < entries_it->second.Size(); i++) {
      size_t test_entry_index = entries_it->second.Get(i);
      if (test_entry_index == entry_index) {
        needs_to_be_added = false;
      } else if (HaveTheSameDexMaps(stack_maps_.Get(test_entry_index), entry)) {
        result = test_entry_index;
        needs_to_be_added = false;
        break;
      }
    }
    if (needs_to_be_added) {
      entries_it->second.Add(entry_index);
    }
    return result;
  }

  bool HaveTheSameDexMaps(const StackMapEntry& a, const StackMapEntry& b) const {
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

  ArenaAllocator* allocator_;
  GrowableArray<StackMapEntry> stack_maps_;

  // A catalog of unique [location_kind, register_value] pairs (per method).
  GrowableArray<DexRegisterLocation> location_catalog_entries_;
  // Map from Dex register location catalog entries to their indices in the
  // location catalog.
  typedef HashMap<DexRegisterLocation, size_t, LocationCatalogEntriesIndicesEmptyFn,
                  DexRegisterLocationHashFn> LocationCatalogEntriesIndices;
  LocationCatalogEntriesIndices location_catalog_entries_indices_;

  // A set of concatenated maps of Dex register locations indices to
  // `location_catalog_entries_`.
  GrowableArray<size_t> dex_register_locations_;
  GrowableArray<InlineInfoEntry> inline_infos_;
  int stack_mask_max_;
  uint32_t dex_pc_max_;
  uint32_t native_pc_offset_max_;
  uint32_t register_mask_max_;
  size_t number_of_stack_maps_with_inline_info_;

  ArenaSafeMap<uint32_t, GrowableArray<uint32_t>> dex_map_hash_to_stack_map_indices_;

  static constexpr uint32_t kNoSameDexMapFound = -1;

  DISALLOW_COPY_AND_ASSIGN(StackMapStream);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_STACK_MAP_STREAM_H_
