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
#include "base/hash_map.h"
#include "base/value_object.h"
#include "memory_region.h"
#include "nodes.h"
#include "stack_map.h"

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
        stack_maps_(allocator->Adapter(kArenaAllocStackMapStream)),
        location_catalog_entries_(allocator->Adapter(kArenaAllocStackMapStream)),
        location_catalog_entries_indices_(allocator->Adapter(kArenaAllocStackMapStream)),
        dex_register_locations_(allocator->Adapter(kArenaAllocStackMapStream)),
        inline_infos_(allocator->Adapter(kArenaAllocStackMapStream)),
        stack_mask_max_(-1),
        dex_pc_max_(0),
        register_mask_max_(0),
        number_of_stack_maps_with_inline_info_(0),
        dex_map_hash_to_stack_map_indices_(std::less<uint32_t>(),
                                           allocator->Adapter(kArenaAllocStackMapStream)),
        current_entry_(),
        current_inline_info_(),
        code_info_encoding_(allocator->Adapter(kArenaAllocStackMapStream)),
        inline_info_size_(0),
        dex_register_maps_size_(0),
        stack_maps_size_(0),
        dex_register_location_catalog_size_(0),
        dex_register_location_catalog_start_(0),
        dex_register_maps_start_(0),
        inline_infos_start_(0),
        needed_size_(0),
        current_dex_register_(0),
        in_inline_frame_(false) {
    stack_maps_.reserve(10);
    location_catalog_entries_.reserve(4);
    dex_register_locations_.reserve(10 * 4);
    inline_infos_.reserve(2);
    code_info_encoding_.reserve(16);
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
    size_t same_dex_register_map_as_;
  };

  struct InlineInfoEntry {
    uint32_t dex_pc;
    uint32_t method_index;
    InvokeType invoke_type;
    uint32_t num_dex_registers;
    BitVector* live_dex_registers_mask;
    size_t dex_register_locations_start_index;
  };

  void BeginStackMapEntry(uint32_t dex_pc,
                          uint32_t native_pc_offset,
                          uint32_t register_mask,
                          BitVector* sp_mask,
                          uint32_t num_dex_registers,
                          uint8_t inlining_depth);
  void EndStackMapEntry();

  void AddDexRegisterEntry(DexRegisterLocation::Kind kind, int32_t value);

  void BeginInlineInfoEntry(uint32_t method_index,
                            uint32_t dex_pc,
                            InvokeType invoke_type,
                            uint32_t num_dex_registers);
  void EndInlineInfoEntry();

  size_t GetNumberOfStackMaps() const {
    return stack_maps_.size();
  }

  const StackMapEntry& GetStackMap(size_t i) const {
    return stack_maps_[i];
  }

  void SetStackMapNativePcOffset(size_t i, uint32_t native_pc_offset) {
    stack_maps_[i].native_pc_offset = native_pc_offset;
  }

  uint32_t ComputeMaxNativePcOffset() const;

  // Prepares the stream to fill in a memory region. Must be called before FillIn.
  // Returns the size (in bytes) needed to store this stream.
  size_t PrepareForFillIn();
  void FillIn(MemoryRegion region);

 private:
  size_t ComputeDexRegisterLocationCatalogSize() const;
  size_t ComputeDexRegisterMapSize(uint32_t num_dex_registers,
                                   const BitVector* live_dex_registers_mask) const;
  size_t ComputeDexRegisterMapsSize() const;
  void ComputeInlineInfoEncoding();

  // Returns the index of an entry with the same dex register map as the current_entry,
  // or kNoSameDexMapFound if no such entry exists.
  size_t FindEntryWithTheSameDexMap();
  bool HaveTheSameDexMaps(const StackMapEntry& a, const StackMapEntry& b) const;
  void FillInDexRegisterMap(DexRegisterMap dex_register_map,
                            uint32_t num_dex_registers,
                            const BitVector& live_dex_registers_mask,
                            uint32_t start_index_in_dex_register_locations) const;

  void CheckDexRegisterMap(const CodeInfo& code_info,
                           const DexRegisterMap& dex_register_map,
                           size_t num_dex_registers,
                           BitVector* live_dex_registers_mask,
                           size_t dex_register_locations_index) const;
  void CheckCodeInfo(MemoryRegion region) const;

  ArenaAllocator* allocator_;
  ArenaVector<StackMapEntry> stack_maps_;

  // A catalog of unique [location_kind, register_value] pairs (per method).
  ArenaVector<DexRegisterLocation> location_catalog_entries_;
  // Map from Dex register location catalog entries to their indices in the
  // location catalog.
  using LocationCatalogEntriesIndices = ArenaHashMap<DexRegisterLocation,
                                                     size_t,
                                                     LocationCatalogEntriesIndicesEmptyFn,
                                                     DexRegisterLocationHashFn>;
  LocationCatalogEntriesIndices location_catalog_entries_indices_;

  // A set of concatenated maps of Dex register locations indices to `location_catalog_entries_`.
  ArenaVector<size_t> dex_register_locations_;
  ArenaVector<InlineInfoEntry> inline_infos_;
  int stack_mask_max_;
  uint32_t dex_pc_max_;
  uint32_t register_mask_max_;
  size_t number_of_stack_maps_with_inline_info_;

  ArenaSafeMap<uint32_t, ArenaVector<uint32_t>> dex_map_hash_to_stack_map_indices_;

  StackMapEntry current_entry_;
  InlineInfoEntry current_inline_info_;
  StackMapEncoding stack_map_encoding_;
  InlineInfoEncoding inline_info_encoding_;
  ArenaVector<uint8_t> code_info_encoding_;
  size_t inline_info_size_;
  size_t dex_register_maps_size_;
  size_t stack_maps_size_;
  size_t dex_register_location_catalog_size_;
  size_t dex_register_location_catalog_start_;
  size_t dex_register_maps_start_;
  size_t inline_infos_start_;
  size_t needed_size_;
  uint32_t current_dex_register_;
  bool in_inline_frame_;

  static constexpr uint32_t kNoSameDexMapFound = -1;

  DISALLOW_COPY_AND_ASSIGN(StackMapStream);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_STACK_MAP_STREAM_H_
