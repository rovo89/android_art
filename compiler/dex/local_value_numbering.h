/*
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef ART_COMPILER_DEX_LOCAL_VALUE_NUMBERING_H_
#define ART_COMPILER_DEX_LOCAL_VALUE_NUMBERING_H_

#include <memory>

#include "compiler_internals.h"
#include "global_value_numbering.h"
#include "utils/scoped_arena_allocator.h"
#include "utils/scoped_arena_containers.h"

namespace art {

class DexFile;

// Enable/disable tracking values stored in the FILLED_NEW_ARRAY result.
static constexpr bool kLocalValueNumberingEnableFilledNewArrayTracking = true;

class LocalValueNumbering {
 private:
  static constexpr uint16_t kNoValue = GlobalValueNumbering::kNoValue;

 public:
  LocalValueNumbering(GlobalValueNumbering* gvn, BasicBlockId id, ScopedArenaAllocator* allocator);

  BasicBlockId Id() const {
    return id_;
  }

  bool Equals(const LocalValueNumbering& other) const;

  uint16_t GetSRegValueName(uint16_t s_reg) const {
    return GetOperandValue(s_reg);
  }

  void SetValueNameNullChecked(uint16_t value_name) {
    null_checked_.insert(value_name);
  }

  bool IsValueNullChecked(uint16_t value_name) const {
    return null_checked_.find(value_name) != null_checked_.end();
  }

  bool IsSregValue(uint16_t s_reg, uint16_t value_name) const {
    auto it = sreg_value_map_.find(s_reg);
    if (it != sreg_value_map_.end()) {
      return it->second == value_name;
    } else {
      return gvn_->HasValue(kNoValue, s_reg, kNoValue, kNoValue, value_name);
    }
  }

  enum MergeType {
    kNormalMerge,
    kCatchMerge,
    kReturnMerge,  // RETURN or PHI+RETURN. Merge only sreg maps.
  };

  void MergeOne(const LocalValueNumbering& other, MergeType merge_type);
  void Merge(MergeType merge_type);  // Merge gvn_->merge_lvns_.

  uint16_t GetValueNumber(MIR* mir);

  // LocalValueNumbering should be allocated on the ArenaStack (or the native stack).
  static void* operator new(size_t size, ScopedArenaAllocator* allocator) {
    return allocator->Alloc(sizeof(LocalValueNumbering), kArenaAllocMisc);
  }

  // Allow delete-expression to destroy a LocalValueNumbering object without deallocation.
  static void operator delete(void* ptr) { UNUSED(ptr); }

 private:
  // A set of value names.
  typedef GlobalValueNumbering::ValueNameSet ValueNameSet;

  // Field types correspond to the ordering of GET/PUT instructions; this order is the same
  // for IGET, IPUT, SGET, SPUT, AGET and APUT:
  // op         0
  // op_WIDE    1
  // op_OBJECT  2
  // op_BOOLEAN 3
  // op_BYTE    4
  // op_CHAR    5
  // op_SHORT   6
  static constexpr size_t kFieldTypeCount = 7;

  // Key is s_reg, value is value name.
  typedef ScopedArenaSafeMap<uint16_t, uint16_t> SregValueMap;

  void SetOperandValueImpl(uint16_t s_reg, uint16_t value, SregValueMap* map) {
    DCHECK_EQ(map->count(s_reg), 0u) << PrettyMethod(gvn_->cu_->method_idx, *gvn_->cu_->dex_file)
        << " LVN id: " << id_ << ", s_reg: " << s_reg;
    map->Put(s_reg, value);
  }

  uint16_t GetOperandValueImpl(int s_reg, const SregValueMap* map) const {
    uint16_t res = kNoValue;
    auto lb = map->find(s_reg);
    if (lb != map->end()) {
      res = lb->second;
    } else {
      // Using the original value; s_reg refers to an input reg.
      res = gvn_->LookupValue(kNoValue, s_reg, kNoValue, kNoValue);
    }
    return res;
  }

  void SetOperandValue(uint16_t s_reg, uint16_t value) {
    SetOperandValueImpl(s_reg, value, &sreg_value_map_);
  };

  uint16_t GetOperandValue(int s_reg) const {
    return GetOperandValueImpl(s_reg, &sreg_value_map_);
  };

  void SetOperandValueWide(uint16_t s_reg, uint16_t value) {
    SetOperandValueImpl(s_reg, value, &sreg_wide_value_map_);
  };

  uint16_t GetOperandValueWide(int s_reg) const {
    return GetOperandValueImpl(s_reg, &sreg_wide_value_map_);
  };

  struct RangeCheckKey {
    uint16_t array;
    uint16_t index;

    // NOTE: Can't define this at namespace scope for a private struct.
    bool operator==(const RangeCheckKey& other) const {
      return array == other.array && index == other.index;
    }
  };

  struct RangeCheckKeyComparator {
    bool operator()(const RangeCheckKey& lhs, const RangeCheckKey& rhs) const {
      if (lhs.array != rhs.array) {
        return lhs.array < rhs.array;
      }
      return lhs.index < rhs.index;
    }
  };

  typedef ScopedArenaSet<RangeCheckKey, RangeCheckKeyComparator> RangeCheckSet;

  // Maps instance field "location" (derived from base, field_id and type) to value name.
  typedef ScopedArenaSafeMap<uint16_t, uint16_t> IFieldLocToValueMap;

  // Maps static field id to value name
  typedef ScopedArenaSafeMap<uint16_t, uint16_t> SFieldToValueMap;

  struct EscapedIFieldClobberKey {
    uint16_t base;      // Or array.
    uint16_t type;
    uint16_t field_id;  // None (kNoValue) for arrays and unresolved instance field stores.

    // NOTE: Can't define this at namespace scope for a private struct.
    bool operator==(const EscapedIFieldClobberKey& other) const {
      return base == other.base && type == other.type && field_id == other.field_id;
    }
  };

  struct EscapedIFieldClobberKeyComparator {
    bool operator()(const EscapedIFieldClobberKey& lhs, const EscapedIFieldClobberKey& rhs) const {
      // Compare base first. This makes sequential iteration respect the order of base.
      if (lhs.base != rhs.base) {
        return lhs.base < rhs.base;
      }
      // Compare type second. This makes the type-clobber entries (field_id == kNoValue) last
      // for given base and type and makes it easy to prune unnecessary entries when merging
      // escaped_ifield_clobber_set_ from multiple LVNs.
      if (lhs.type != rhs.type) {
        return lhs.type < rhs.type;
      }
      return lhs.field_id < rhs.field_id;
    }
  };

  typedef ScopedArenaSet<EscapedIFieldClobberKey, EscapedIFieldClobberKeyComparator>
      EscapedIFieldClobberSet;

  struct EscapedArrayClobberKey {
    uint16_t base;
    uint16_t type;

    // NOTE: Can't define this at namespace scope for a private struct.
    bool operator==(const EscapedArrayClobberKey& other) const {
      return base == other.base && type == other.type;
    }
  };

  struct EscapedArrayClobberKeyComparator {
    bool operator()(const EscapedArrayClobberKey& lhs, const EscapedArrayClobberKey& rhs) const {
      // Compare base first. This makes sequential iteration respect the order of base.
      if (lhs.base != rhs.base) {
        return lhs.base < rhs.base;
      }
      return lhs.type < rhs.type;
    }
  };

  // Clobber set for previously non-aliasing array refs that escaped.
  typedef ScopedArenaSet<EscapedArrayClobberKey, EscapedArrayClobberKeyComparator>
      EscapedArrayClobberSet;

  // Known location values for an aliasing set. The set can be tied to one of:
  //   1. Instance field. The locations are aliasing references used to access the field.
  //   2. Non-aliasing array reference. The locations are indexes to the array.
  //   3. Aliasing array type. The locations are (reference, index) pair ids assigned by GVN.
  // In each case we keep track of the last stored value, if any, and the set of locations
  // where it was stored. We also keep track of all values known for the current write state
  // (load_value_map), which can be known either because they have been loaded since the last
  // store or because they contained the last_stored_value before the store and thus could not
  // have changed as a result.
  struct AliasingValues {
    explicit AliasingValues(LocalValueNumbering* lvn)
        : memory_version_before_stores(kNoValue),
          last_stored_value(kNoValue),
          store_loc_set(std::less<uint16_t>(), lvn->null_checked_.get_allocator()),
          last_load_memory_version(kNoValue),
          load_value_map(std::less<uint16_t>(), lvn->null_checked_.get_allocator()) {
    }

    uint16_t memory_version_before_stores;  // kNoValue if start version for the field.
    uint16_t last_stored_value;             // Last stored value name, kNoValue if none.
    ValueNameSet store_loc_set;             // Where was last_stored_value stored.

    // Maps refs (other than stored_to) to currently known values for this field other. On write,
    // anything that differs from the written value is removed as it may be overwritten.
    uint16_t last_load_memory_version;    // kNoValue if not known.
    ScopedArenaSafeMap<uint16_t, uint16_t> load_value_map;

    // NOTE: Can't define this at namespace scope for a private struct.
    bool operator==(const AliasingValues& other) const {
      return memory_version_before_stores == other.memory_version_before_stores &&
          last_load_memory_version == other.last_load_memory_version &&
          last_stored_value == other.last_stored_value &&
          store_loc_set == other.store_loc_set &&
          load_value_map == other.load_value_map;
    }
  };

  // Maps instance field id to AliasingValues, locations are object refs.
  typedef ScopedArenaSafeMap<uint16_t, AliasingValues> AliasingIFieldValuesMap;

  // Maps non-aliasing array reference to AliasingValues, locations are array indexes.
  typedef ScopedArenaSafeMap<uint16_t, AliasingValues> NonAliasingArrayValuesMap;

  // Maps aliasing array type to AliasingValues, locations are (array, index) pair ids.
  typedef ScopedArenaSafeMap<uint16_t, AliasingValues> AliasingArrayValuesMap;

  // Helper classes defining versions for updating and merging the AliasingValues maps above.
  class AliasingIFieldVersions;
  class NonAliasingArrayVersions;
  class AliasingArrayVersions;

  template <typename Map>
  AliasingValues* GetAliasingValues(Map* map, const typename Map::key_type& key);

  template <typename Versions, typename KeyType>
  void UpdateAliasingValuesLoadVersion(const KeyType& key, AliasingValues* values);

  template <typename Versions, typename Map>
  static uint16_t AliasingValuesMergeGet(GlobalValueNumbering* gvn,
                                         const LocalValueNumbering* lvn,
                                         Map* map, const typename Map::key_type& key,
                                         uint16_t location);

  template <typename Versions, typename Map>
  uint16_t HandleAliasingValuesGet(Map* map, const typename Map::key_type& key,
                                   uint16_t location);

  template <typename Versions, typename Map>
  bool HandleAliasingValuesPut(Map* map, const typename Map::key_type& key,
                               uint16_t location, uint16_t value);

  template <typename K>
  void CopyAliasingValuesMap(ScopedArenaSafeMap<K, AliasingValues>* dest,
                             const ScopedArenaSafeMap<K, AliasingValues>& src);

  uint16_t MarkNonAliasingNonNull(MIR* mir);
  bool IsNonAliasing(uint16_t reg) const;
  bool IsNonAliasingIField(uint16_t reg, uint16_t field_id, uint16_t type) const;
  bool IsNonAliasingArray(uint16_t reg, uint16_t type) const;
  void HandleNullCheck(MIR* mir, uint16_t reg);
  void HandleRangeCheck(MIR* mir, uint16_t array, uint16_t index);
  void HandlePutObject(MIR* mir);
  void HandleEscapingRef(uint16_t base);
  uint16_t HandlePhi(MIR* mir);
  uint16_t HandleAGet(MIR* mir, uint16_t opcode);
  void HandleAPut(MIR* mir, uint16_t opcode);
  uint16_t HandleIGet(MIR* mir, uint16_t opcode);
  void HandleIPut(MIR* mir, uint16_t opcode);
  uint16_t HandleSGet(MIR* mir, uint16_t opcode);
  void HandleSPut(MIR* mir, uint16_t opcode);
  void RemoveSFieldsForType(uint16_t type);
  void HandleInvokeOrClInit(MIR* mir);

  bool SameMemoryVersion(const LocalValueNumbering& other) const;

  uint16_t NewMemoryVersion(uint16_t* new_version);
  void MergeMemoryVersions(bool clobbered_catch);

  void PruneNonAliasingRefsForCatch();

  template <typename Set, Set LocalValueNumbering::* set_ptr>
  void IntersectSets();

  void CopyLiveSregValues(SregValueMap* dest, const SregValueMap& src);

  // Intersect maps as sets. The value type must be equality-comparable.
  template <SregValueMap LocalValueNumbering::* map_ptr>
  void IntersectSregValueMaps();

  // Intersect maps as sets. The value type must be equality-comparable.
  template <typename Map>
  static void InPlaceIntersectMaps(Map* work_map, const Map& other_map);

  template <typename Set, Set LocalValueNumbering::*set_ptr, void (LocalValueNumbering::*MergeFn)(
      const typename Set::value_type& entry, typename Set::iterator hint)>
  void MergeSets();

  void IntersectAliasingValueLocations(AliasingValues* work_values, const AliasingValues* values);

  void MergeEscapedRefs(const ValueNameSet::value_type& entry, ValueNameSet::iterator hint);
  void MergeEscapedIFieldTypeClobberSets(const EscapedIFieldClobberSet::value_type& entry,
                                         EscapedIFieldClobberSet::iterator hint);
  void MergeEscapedIFieldClobberSets(const EscapedIFieldClobberSet::value_type& entry,
                                     EscapedIFieldClobberSet::iterator hint);
  void MergeEscapedArrayClobberSets(const EscapedArrayClobberSet::value_type& entry,
                                    EscapedArrayClobberSet::iterator hint);
  void MergeNullChecked(const ValueNameSet::value_type& entry, ValueNameSet::iterator hint);
  void MergeSFieldValues(const SFieldToValueMap::value_type& entry,
                         SFieldToValueMap::iterator hint);
  void MergeNonAliasingIFieldValues(const IFieldLocToValueMap::value_type& entry,
                                    IFieldLocToValueMap::iterator hint);

  template <typename Map, Map LocalValueNumbering::*map_ptr, typename Versions>
  void MergeAliasingValues(const typename Map::value_type& entry, typename Map::iterator hint);

  GlobalValueNumbering* gvn_;

  // We're using the block id as a 16-bit operand value for some lookups.
  COMPILE_ASSERT(sizeof(BasicBlockId) == sizeof(uint16_t), BasicBlockId_must_be_16_bit);
  BasicBlockId id_;

  SregValueMap sreg_value_map_;
  SregValueMap sreg_wide_value_map_;

  SFieldToValueMap sfield_value_map_;
  IFieldLocToValueMap non_aliasing_ifield_value_map_;
  AliasingIFieldValuesMap aliasing_ifield_value_map_;
  NonAliasingArrayValuesMap non_aliasing_array_value_map_;
  AliasingArrayValuesMap aliasing_array_value_map_;

  // Data for dealing with memory clobbering and store/load aliasing.
  uint16_t global_memory_version_;
  uint16_t unresolved_sfield_version_[kFieldTypeCount];
  uint16_t unresolved_ifield_version_[kFieldTypeCount];
  // Value names of references to objects that cannot be reached through a different value name.
  ValueNameSet non_aliasing_refs_;
  // Previously non-aliasing refs that escaped but can still be used for non-aliasing AGET/IGET.
  ValueNameSet escaped_refs_;
  // Blacklists for cases where escaped_refs_ can't be used.
  EscapedIFieldClobberSet escaped_ifield_clobber_set_;
  EscapedArrayClobberSet escaped_array_clobber_set_;

  // Range check and null check elimination.
  RangeCheckSet range_checked_;
  ValueNameSet null_checked_;

  // Reuse one vector for all merges to avoid leaking too much memory on the ArenaStack.
  ScopedArenaVector<BasicBlockId> merge_names_;
  // Map to identify when different locations merge the same values.
  ScopedArenaSafeMap<ScopedArenaVector<BasicBlockId>, uint16_t> merge_map_;
  // New memory version for merge, kNoValue if all memory versions matched.
  uint16_t merge_new_memory_version_;

  DISALLOW_COPY_AND_ASSIGN(LocalValueNumbering);
};

}  // namespace art

#endif  // ART_COMPILER_DEX_LOCAL_VALUE_NUMBERING_H_
