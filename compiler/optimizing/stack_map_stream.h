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
        dex_map_hash_to_stack_map_indices_(std::less<uint32_t>(), allocator->Adapter()),
        current_entry_(),
        stack_mask_size_(0),
        inline_info_size_(0),
        dex_register_maps_size_(0),
        stack_maps_size_(0),
        dex_register_location_catalog_size_(0),
        dex_register_location_catalog_start_(0),
        stack_maps_start_(0),
        dex_register_maps_start_(0),
        inline_infos_start_(0),
        needed_size_(0) {}

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
    size_t same_dex_register_map_as_;
  };

  struct InlineInfoEntry {
    uint32_t method_index;
  };

  void BeginStackMapEntry(uint32_t dex_pc,
                          uint32_t native_pc_offset,
                          uint32_t register_mask,
                          BitVector* sp_mask,
                          uint32_t num_dex_registers,
                          uint8_t inlining_depth);
  void EndStackMapEntry();

  void AddDexRegisterEntry(uint16_t dex_register,
                           DexRegisterLocation::Kind kind,
                           int32_t value);

  void AddInlineInfoEntry(uint32_t method_index);

  // Prepares the stream to fill in a memory region. Must be called before FillIn.
  // Returns the size (in bytes) needed to store this stream.
  size_t PrepareForFillIn();
  void FillIn(MemoryRegion region);

 private:
  size_t ComputeDexRegisterLocationCatalogSize() const;
  size_t ComputeDexRegisterMapSize(const StackMapEntry& entry) const;
  size_t ComputeDexRegisterMapsSize() const;
  size_t ComputeInlineInfoSize() const;

  // Returns the index of an entry with the same dex register map as the current_entry,
  // or kNoSameDexMapFound if no such entry exists.
  size_t FindEntryWithTheSameDexMap();
  bool HaveTheSameDexMaps(const StackMapEntry& a, const StackMapEntry& b) const;

  ArenaAllocator* allocator_;
  GrowableArray<StackMapEntry> stack_maps_;

  // A catalog of unique [location_kind, register_value] pairs (per method).
  GrowableArray<DexRegisterLocation> location_catalog_entries_;
  // Map from Dex register location catalog entries to their indices in the
  // location catalog.
  typedef HashMap<DexRegisterLocation, size_t, LocationCatalogEntriesIndicesEmptyFn,
                  DexRegisterLocationHashFn> LocationCatalogEntriesIndices;
  LocationCatalogEntriesIndices location_catalog_entries_indices_;

  // A set of concatenated maps of Dex register locations indices to `location_catalog_entries_`.
  GrowableArray<size_t> dex_register_locations_;
  GrowableArray<InlineInfoEntry> inline_infos_;
  int stack_mask_max_;
  uint32_t dex_pc_max_;
  uint32_t native_pc_offset_max_;
  uint32_t register_mask_max_;
  size_t number_of_stack_maps_with_inline_info_;

  ArenaSafeMap<uint32_t, GrowableArray<uint32_t>> dex_map_hash_to_stack_map_indices_;

  StackMapEntry current_entry_;
  size_t stack_mask_size_;
  size_t inline_info_size_;
  size_t dex_register_maps_size_;
  size_t stack_maps_size_;
  size_t dex_register_location_catalog_size_;
  size_t dex_register_location_catalog_start_;
  size_t stack_maps_start_;
  size_t dex_register_maps_start_;
  size_t inline_infos_start_;
  size_t needed_size_;

  static constexpr uint32_t kNoSameDexMapFound = -1;

  DISALLOW_COPY_AND_ASSIGN(StackMapStream);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_STACK_MAP_STREAM_H_
