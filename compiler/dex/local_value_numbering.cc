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

#include "local_value_numbering.h"

#include "global_value_numbering.h"
#include "mir_field_info.h"
#include "mir_graph.h"

namespace art {

namespace {  // anonymous namespace

// Operations used for value map keys instead of actual opcode.
static constexpr uint16_t kInvokeMemoryVersionBumpOp = Instruction::INVOKE_VIRTUAL;
static constexpr uint16_t kUnresolvedSFieldOp = Instruction::SGET;
static constexpr uint16_t kResolvedSFieldOp = Instruction::SGET_WIDE;
static constexpr uint16_t kUnresolvedIFieldOp = Instruction::IGET;
static constexpr uint16_t kNonAliasingIFieldLocOp = Instruction::IGET_WIDE;
static constexpr uint16_t kNonAliasingIFieldInitialOp = Instruction::IGET_OBJECT;
static constexpr uint16_t kAliasingIFieldOp = Instruction::IGET_BOOLEAN;
static constexpr uint16_t kAliasingIFieldStartVersionOp = Instruction::IGET_BYTE;
static constexpr uint16_t kAliasingIFieldBumpVersionOp = Instruction::IGET_CHAR;
static constexpr uint16_t kNonAliasingArrayOp = Instruction::AGET;
static constexpr uint16_t kNonAliasingArrayStartVersionOp = Instruction::AGET_WIDE;
static constexpr uint16_t kNonAliasingArrayBumpVersionOp = Instruction::AGET_OBJECT;
static constexpr uint16_t kAliasingArrayOp = Instruction::AGET_BOOLEAN;
static constexpr uint16_t kAliasingArrayStartVersionOp = Instruction::AGET_BYTE;
static constexpr uint16_t kAliasingArrayBumpVersionOp = Instruction::AGET_CHAR;
static constexpr uint16_t kMergeBlockMemoryVersionBumpOp = Instruction::INVOKE_VIRTUAL_RANGE;
static constexpr uint16_t kMergeBlockAliasingIFieldVersionBumpOp = Instruction::IPUT;
static constexpr uint16_t kMergeBlockAliasingIFieldMergeLocationOp = Instruction::IPUT_WIDE;
static constexpr uint16_t kMergeBlockNonAliasingArrayVersionBumpOp = Instruction::APUT;
static constexpr uint16_t kMergeBlockNonAliasingArrayMergeLocationOp = Instruction::APUT_WIDE;
static constexpr uint16_t kMergeBlockAliasingArrayVersionBumpOp = Instruction::APUT_OBJECT;
static constexpr uint16_t kMergeBlockAliasingArrayMergeLocationOp = Instruction::APUT_BOOLEAN;
static constexpr uint16_t kMergeBlockNonAliasingIFieldVersionBumpOp = Instruction::APUT_BYTE;
static constexpr uint16_t kMergeBlockSFieldVersionBumpOp = Instruction::APUT_CHAR;

}  // anonymous namespace

class LocalValueNumbering::AliasingIFieldVersions {
 public:
  static uint16_t StartMemoryVersion(GlobalValueNumbering* gvn, const LocalValueNumbering* lvn,
                                     uint16_t field_id) {
    uint16_t type = gvn->GetFieldType(field_id);
    return gvn->LookupValue(kAliasingIFieldStartVersionOp, field_id,
                            lvn->global_memory_version_, lvn->unresolved_ifield_version_[type]);
  }

  static uint16_t BumpMemoryVersion(GlobalValueNumbering* gvn, uint16_t old_version,
                                    uint16_t store_ref_set_id, uint16_t stored_value) {
    return gvn->LookupValue(kAliasingIFieldBumpVersionOp, old_version,
                            store_ref_set_id, stored_value);
  }

  static uint16_t LookupGlobalValue(GlobalValueNumbering* gvn,
                                    uint16_t field_id, uint16_t base, uint16_t memory_version) {
    return gvn->LookupValue(kAliasingIFieldOp, field_id, base, memory_version);
  }

  static uint16_t LookupMergeValue(GlobalValueNumbering* gvn, const LocalValueNumbering* lvn,
                                   uint16_t field_id, uint16_t base) {
    // If the base/field_id is non-aliasing in lvn, use the non-aliasing value.
    uint16_t type = gvn->GetFieldType(field_id);
    if (lvn->IsNonAliasingIField(base, field_id, type)) {
      uint16_t loc = gvn->LookupValue(kNonAliasingIFieldLocOp, base, field_id, type);
      auto lb = lvn->non_aliasing_ifield_value_map_.find(loc);
      return (lb != lvn->non_aliasing_ifield_value_map_.end())
          ? lb->second
          : gvn->LookupValue(kNonAliasingIFieldInitialOp, loc, kNoValue, kNoValue);
    }
    return AliasingValuesMergeGet<AliasingIFieldVersions>(
        gvn, lvn, &lvn->aliasing_ifield_value_map_, field_id, base);
  }

  static bool HasNewBaseVersion(GlobalValueNumbering* gvn, const LocalValueNumbering* lvn,
                                uint16_t field_id) {
    uint16_t type = gvn->GetFieldType(field_id);
    return lvn->unresolved_ifield_version_[type] == lvn->merge_new_memory_version_ ||
        lvn->global_memory_version_ == lvn->merge_new_memory_version_;
  }

  static uint16_t LookupMergeBlockValue(GlobalValueNumbering* gvn, uint16_t lvn_id,
                                        uint16_t field_id) {
    return gvn->LookupValue(kMergeBlockAliasingIFieldVersionBumpOp, field_id, kNoValue, lvn_id);
  }

  static uint16_t LookupMergeLocationValue(GlobalValueNumbering* gvn, uint16_t lvn_id,
                                           uint16_t field_id, uint16_t base) {
    return gvn->LookupValue(kMergeBlockAliasingIFieldMergeLocationOp, field_id, base, lvn_id);
  }
};

class LocalValueNumbering::NonAliasingArrayVersions {
 public:
  static uint16_t StartMemoryVersion(GlobalValueNumbering* gvn, const LocalValueNumbering* lvn,
                                     uint16_t array) {
    return gvn->LookupValue(kNonAliasingArrayStartVersionOp, array, kNoValue, kNoValue);
  }

  static uint16_t BumpMemoryVersion(GlobalValueNumbering* gvn, uint16_t old_version,
                                    uint16_t store_ref_set_id, uint16_t stored_value) {
    return gvn->LookupValue(kNonAliasingArrayBumpVersionOp, old_version,
                            store_ref_set_id, stored_value);
  }

  static uint16_t LookupGlobalValue(GlobalValueNumbering* gvn,
                                    uint16_t array, uint16_t index, uint16_t memory_version) {
    return gvn->LookupValue(kNonAliasingArrayOp, array, index, memory_version);
  }

  static uint16_t LookupMergeValue(GlobalValueNumbering* gvn, const LocalValueNumbering* lvn,
                                   uint16_t array, uint16_t index) {
    return AliasingValuesMergeGet<NonAliasingArrayVersions>(
        gvn, lvn, &lvn->non_aliasing_array_value_map_, array, index);
  }

  static bool HasNewBaseVersion(GlobalValueNumbering* gvn, const LocalValueNumbering* lvn,
                                uint16_t array) {
    return false;  // Not affected by global_memory_version_.
  }

  static uint16_t LookupMergeBlockValue(GlobalValueNumbering* gvn, uint16_t lvn_id,
                                        uint16_t array) {
    return gvn->LookupValue(kMergeBlockNonAliasingArrayVersionBumpOp, array, kNoValue, lvn_id);
  }

  static uint16_t LookupMergeLocationValue(GlobalValueNumbering* gvn, uint16_t lvn_id,
                                           uint16_t array, uint16_t index) {
    return gvn->LookupValue(kMergeBlockNonAliasingArrayMergeLocationOp, array, index, lvn_id);
  }
};

class LocalValueNumbering::AliasingArrayVersions {
 public:
  static uint16_t StartMemoryVersion(GlobalValueNumbering* gvn, const LocalValueNumbering* lvn,
                                     uint16_t type) {
    return gvn->LookupValue(kAliasingArrayStartVersionOp, type, lvn->global_memory_version_,
                            kNoValue);
  }

  static uint16_t BumpMemoryVersion(GlobalValueNumbering* gvn, uint16_t old_version,
                                    uint16_t store_ref_set_id, uint16_t stored_value) {
    return gvn->LookupValue(kAliasingArrayBumpVersionOp, old_version,
                            store_ref_set_id, stored_value);
  }

  static uint16_t LookupGlobalValue(GlobalValueNumbering* gvn,
                                    uint16_t type, uint16_t location, uint16_t memory_version) {
    return gvn->LookupValue(kAliasingArrayOp, type, location, memory_version);
  }

  static uint16_t LookupMergeValue(GlobalValueNumbering* gvn, const LocalValueNumbering* lvn,
                                   uint16_t type, uint16_t location) {
    // If the location is non-aliasing in lvn, use the non-aliasing value.
    uint16_t array = gvn->GetArrayLocationBase(location);
    if (lvn->IsNonAliasingArray(array, type)) {
      uint16_t index = gvn->GetArrayLocationIndex(location);
      return NonAliasingArrayVersions::LookupMergeValue(gvn, lvn, array, index);
    }
    return AliasingValuesMergeGet<AliasingArrayVersions>(
        gvn, lvn, &lvn->aliasing_array_value_map_, type, location);
  }

  static bool HasNewBaseVersion(GlobalValueNumbering* gvn, const LocalValueNumbering* lvn,
                                uint16_t type) {
    return lvn->global_memory_version_ == lvn->merge_new_memory_version_;
  }

  static uint16_t LookupMergeBlockValue(GlobalValueNumbering* gvn, uint16_t lvn_id,
                                        uint16_t type) {
    return gvn->LookupValue(kMergeBlockAliasingArrayVersionBumpOp, type, kNoValue, lvn_id);
  }

  static uint16_t LookupMergeLocationValue(GlobalValueNumbering* gvn, uint16_t lvn_id,
                                           uint16_t type, uint16_t location) {
    return gvn->LookupValue(kMergeBlockAliasingArrayMergeLocationOp, type, location, lvn_id);
  }
};

template <typename Map>
LocalValueNumbering::AliasingValues* LocalValueNumbering::GetAliasingValues(
    Map* map, const typename Map::key_type& key) {
  auto lb = map->lower_bound(key);
  if (lb == map->end() || map->key_comp()(key, lb->first)) {
    lb = map->PutBefore(lb, key, AliasingValues(this));
  }
  return &lb->second;
}

template <typename Versions, typename KeyType>
void LocalValueNumbering::UpdateAliasingValuesLoadVersion(const KeyType& key,
                                                          AliasingValues* values) {
  if (values->last_load_memory_version == kNoValue) {
    // Get the start version that accounts for aliasing with unresolved fields of the same
    // type and make it unique for the field by including the field_id.
    uint16_t memory_version = values->memory_version_before_stores;
    if (memory_version == kNoValue) {
      memory_version = Versions::StartMemoryVersion(gvn_, this, key);
    }
    if (!values->store_loc_set.empty()) {
      uint16_t ref_set_id = gvn_->GetRefSetId(values->store_loc_set);
      memory_version = Versions::BumpMemoryVersion(gvn_, memory_version, ref_set_id,
                                                   values->last_stored_value);
    }
    values->last_load_memory_version = memory_version;
  }
}

template <typename Versions, typename Map>
uint16_t LocalValueNumbering::AliasingValuesMergeGet(GlobalValueNumbering* gvn,
                                                     const LocalValueNumbering* lvn,
                                                     Map* map, const typename Map::key_type& key,
                                                     uint16_t location) {
  // Retrieve the value name that we would get from
  //   const_cast<LocalValueNumbering*>(lvn)->HandleAliasingValueGet(map. key, location)
  // but don't modify the map.
  uint16_t value_name;
  auto it = map->find(key);
  if (it == map->end()) {
    uint16_t start_version = Versions::StartMemoryVersion(gvn, lvn, key);
    value_name = Versions::LookupGlobalValue(gvn, key, location, start_version);
  } else if (it->second.store_loc_set.count(location) != 0u) {
    value_name = it->second.last_stored_value;
  } else {
    auto load_it = it->second.load_value_map.find(location);
    if (load_it != it->second.load_value_map.end()) {
      value_name = load_it->second;
    } else {
      value_name = Versions::LookupGlobalValue(gvn, key, location, it->second.last_load_memory_version);
    }
  }
  return value_name;
}

template <typename Versions, typename Map>
uint16_t LocalValueNumbering::HandleAliasingValuesGet(Map* map, const typename Map::key_type& key,
                                                      uint16_t location) {
  // Retrieve the value name for IGET/SGET/AGET, update the map with new value if any.
  uint16_t res;
  AliasingValues* values = GetAliasingValues(map, key);
  if (values->store_loc_set.count(location) != 0u) {
    res = values->last_stored_value;
  } else {
    UpdateAliasingValuesLoadVersion<Versions>(key, values);
    auto lb = values->load_value_map.lower_bound(location);
    if (lb != values->load_value_map.end() && lb->first == location) {
      res = lb->second;
    } else {
      res = Versions::LookupGlobalValue(gvn_, key, location, values->last_load_memory_version);
      values->load_value_map.PutBefore(lb, location, res);
    }
  }
  return res;
}

template <typename Versions, typename Map>
bool LocalValueNumbering::HandleAliasingValuesPut(Map* map, const typename Map::key_type& key,
                                                  uint16_t location, uint16_t value) {
  AliasingValues* values = GetAliasingValues(map, key);
  auto load_values_it = values->load_value_map.find(location);
  if (load_values_it != values->load_value_map.end() && load_values_it->second == value) {
    // This insn can be eliminated, it stores the same value that's already in the field.
    return false;
  }
  if (value == values->last_stored_value) {
    auto store_loc_lb = values->store_loc_set.lower_bound(location);
    if (store_loc_lb != values->store_loc_set.end() && *store_loc_lb == location) {
      // This insn can be eliminated, it stores the same value that's already in the field.
      return false;
    }
    values->store_loc_set.emplace_hint(store_loc_lb, location);
  } else {
    UpdateAliasingValuesLoadVersion<Versions>(key, values);
    values->memory_version_before_stores = values->last_load_memory_version;
    values->last_stored_value = value;
    values->store_loc_set.clear();
    values->store_loc_set.insert(location);
  }
  // Clear the last load memory version and remove all potentially overwritten values.
  values->last_load_memory_version = kNoValue;
  auto it = values->load_value_map.begin(), end = values->load_value_map.end();
  while (it != end) {
    if (it->second == value) {
      ++it;
    } else {
      it = values->load_value_map.erase(it);
    }
  }
  return true;
}

template <typename K>
void LocalValueNumbering::CopyAliasingValuesMap(ScopedArenaSafeMap<K, AliasingValues>* dest,
                                                const ScopedArenaSafeMap<K, AliasingValues>& src) {
  // We need each new AliasingValues (or rather its map members) to be constructed
  // with our allocator, rather than the allocator of the source.
  for (const auto& entry : src) {
    auto it = dest->PutBefore(dest->end(), entry.first, AliasingValues(this));
    it->second = entry.second;  // Map assignments preserve current allocator.
  }
}

LocalValueNumbering::LocalValueNumbering(GlobalValueNumbering* gvn, uint16_t id,
                                         ScopedArenaAllocator* allocator)
    : gvn_(gvn),
      id_(id),
      sreg_value_map_(std::less<uint16_t>(), allocator->Adapter()),
      sreg_wide_value_map_(std::less<uint16_t>(), allocator->Adapter()),
      sfield_value_map_(std::less<uint16_t>(), allocator->Adapter()),
      non_aliasing_ifield_value_map_(std::less<uint16_t>(), allocator->Adapter()),
      aliasing_ifield_value_map_(std::less<uint16_t>(), allocator->Adapter()),
      non_aliasing_array_value_map_(std::less<uint16_t>(), allocator->Adapter()),
      aliasing_array_value_map_(std::less<uint16_t>(), allocator->Adapter()),
      global_memory_version_(0u),
      non_aliasing_refs_(std::less<uint16_t>(), allocator->Adapter()),
      escaped_refs_(std::less<uint16_t>(), allocator->Adapter()),
      escaped_ifield_clobber_set_(EscapedIFieldClobberKeyComparator(), allocator->Adapter()),
      escaped_array_clobber_set_(EscapedArrayClobberKeyComparator(), allocator->Adapter()),
      range_checked_(RangeCheckKeyComparator() , allocator->Adapter()),
      null_checked_(std::less<uint16_t>(), allocator->Adapter()),
      merge_names_(allocator->Adapter()),
      merge_map_(std::less<ScopedArenaVector<BasicBlockId>>(), allocator->Adapter()),
      merge_new_memory_version_(kNoValue) {
  std::fill_n(unresolved_sfield_version_, kFieldTypeCount, 0u);
  std::fill_n(unresolved_ifield_version_, kFieldTypeCount, 0u);
}

bool LocalValueNumbering::Equals(const LocalValueNumbering& other) const {
  DCHECK(gvn_ == other.gvn_);
  // Compare the maps/sets and memory versions.
  return sreg_value_map_ == other.sreg_value_map_ &&
      sreg_wide_value_map_ == other.sreg_wide_value_map_ &&
      sfield_value_map_ == other.sfield_value_map_ &&
      non_aliasing_ifield_value_map_ == other.non_aliasing_ifield_value_map_ &&
      aliasing_ifield_value_map_ == other.aliasing_ifield_value_map_ &&
      non_aliasing_array_value_map_ == other.non_aliasing_array_value_map_ &&
      aliasing_array_value_map_ == other.aliasing_array_value_map_ &&
      SameMemoryVersion(other) &&
      non_aliasing_refs_ == other.non_aliasing_refs_ &&
      escaped_refs_ == other.escaped_refs_ &&
      escaped_ifield_clobber_set_ == other.escaped_ifield_clobber_set_ &&
      escaped_array_clobber_set_ == other.escaped_array_clobber_set_ &&
      range_checked_ == other.range_checked_ &&
      null_checked_ == other.null_checked_;
}

void LocalValueNumbering::MergeOne(const LocalValueNumbering& other, MergeType merge_type) {
  CopyLiveSregValues(&sreg_value_map_, other.sreg_value_map_);
  CopyLiveSregValues(&sreg_wide_value_map_, other.sreg_wide_value_map_);

  if (merge_type == kReturnMerge) {
    // RETURN or PHI+RETURN. We need only sreg value maps.
    return;
  }

  non_aliasing_ifield_value_map_ = other.non_aliasing_ifield_value_map_;
  CopyAliasingValuesMap(&non_aliasing_array_value_map_, other.non_aliasing_array_value_map_);
  non_aliasing_refs_ = other.non_aliasing_refs_;
  range_checked_ = other.range_checked_;
  null_checked_ = other.null_checked_;

  if (merge_type == kCatchMerge) {
    // Memory is clobbered. Use new memory version and don't merge aliasing locations.
    global_memory_version_ = NewMemoryVersion(&merge_new_memory_version_);
    std::fill_n(unresolved_sfield_version_, kFieldTypeCount, global_memory_version_);
    std::fill_n(unresolved_ifield_version_, kFieldTypeCount, global_memory_version_);
    PruneNonAliasingRefsForCatch();
    return;
  }

  DCHECK(merge_type == kNormalMerge);
  global_memory_version_ = other.global_memory_version_;
  std::copy_n(other.unresolved_ifield_version_, kFieldTypeCount, unresolved_ifield_version_);
  std::copy_n(other.unresolved_sfield_version_, kFieldTypeCount, unresolved_sfield_version_);
  sfield_value_map_ = other.sfield_value_map_;
  CopyAliasingValuesMap(&aliasing_ifield_value_map_, other.aliasing_ifield_value_map_);
  CopyAliasingValuesMap(&aliasing_array_value_map_, other.aliasing_array_value_map_);
  escaped_refs_ = other.escaped_refs_;
  escaped_ifield_clobber_set_ = other.escaped_ifield_clobber_set_;
  escaped_array_clobber_set_ = other.escaped_array_clobber_set_;
}

bool LocalValueNumbering::SameMemoryVersion(const LocalValueNumbering& other) const {
  return
      global_memory_version_ == other.global_memory_version_ &&
      std::equal(unresolved_ifield_version_, unresolved_ifield_version_ + kFieldTypeCount,
                 other.unresolved_ifield_version_) &&
      std::equal(unresolved_sfield_version_, unresolved_sfield_version_ + kFieldTypeCount,
                 other.unresolved_sfield_version_);
}

uint16_t LocalValueNumbering::NewMemoryVersion(uint16_t* new_version) {
  if (*new_version == kNoValue) {
    *new_version = gvn_->LookupValue(kMergeBlockMemoryVersionBumpOp, 0u, 0u, id_);
  }
  return *new_version;
}

void LocalValueNumbering::MergeMemoryVersions(bool clobbered_catch) {
  DCHECK_GE(gvn_->merge_lvns_.size(), 2u);
  const LocalValueNumbering* cmp = gvn_->merge_lvns_[0];
  // Check if the global version has changed.
  bool new_global_version = clobbered_catch;
  if (!new_global_version) {
    for (const LocalValueNumbering* lvn : gvn_->merge_lvns_) {
      if (lvn->global_memory_version_ != cmp->global_memory_version_) {
        // Use a new version for everything.
        new_global_version = true;
        break;
      }
    }
  }
  if (new_global_version) {
    global_memory_version_ = NewMemoryVersion(&merge_new_memory_version_);
    std::fill_n(unresolved_sfield_version_, kFieldTypeCount, merge_new_memory_version_);
    std::fill_n(unresolved_ifield_version_, kFieldTypeCount, merge_new_memory_version_);
  } else {
    // Initialize with a copy of memory versions from the comparison LVN.
    global_memory_version_ = cmp->global_memory_version_;
    std::copy_n(cmp->unresolved_ifield_version_, kFieldTypeCount, unresolved_ifield_version_);
    std::copy_n(cmp->unresolved_sfield_version_, kFieldTypeCount, unresolved_sfield_version_);
    for (const LocalValueNumbering* lvn : gvn_->merge_lvns_) {
      if (lvn == cmp) {
        continue;
      }
      for (size_t i = 0; i != kFieldTypeCount; ++i) {
        if (lvn->unresolved_ifield_version_[i] != cmp->unresolved_ifield_version_[i]) {
          unresolved_ifield_version_[i] = NewMemoryVersion(&merge_new_memory_version_);
        }
        if (lvn->unresolved_sfield_version_[i] != cmp->unresolved_sfield_version_[i]) {
          unresolved_sfield_version_[i] = NewMemoryVersion(&merge_new_memory_version_);
        }
      }
    }
  }
}

void LocalValueNumbering::PruneNonAliasingRefsForCatch() {
  for (const LocalValueNumbering* lvn : gvn_->merge_lvns_) {
    const BasicBlock* bb = gvn_->GetBasicBlock(lvn->Id());
    if (UNLIKELY(bb->taken == id_) || UNLIKELY(bb->fall_through == id_)) {
      // Non-exceptional path to a catch handler means that the catch block was actually
      // empty and all exceptional paths lead to the shared path after that empty block.
      continue;
    }
    DCHECK_EQ(bb->taken, kNullBlock);
    DCHECK_NE(bb->fall_through, kNullBlock);
    const BasicBlock* fall_through_bb = gvn_->GetBasicBlock(bb->fall_through);
    const MIR* mir = fall_through_bb->first_mir_insn;
    DCHECK(mir != nullptr);
    // Only INVOKEs can leak and clobber non-aliasing references if they throw.
    if ((Instruction::FlagsOf(mir->dalvikInsn.opcode) & Instruction::kInvoke) != 0) {
      for (uint16_t i = 0u; i != mir->ssa_rep->num_uses; ++i) {
        uint16_t value_name = lvn->GetOperandValue(mir->ssa_rep->uses[i]);
        non_aliasing_refs_.erase(value_name);
      }
    }
  }
}


template <typename Set, Set LocalValueNumbering::* set_ptr>
void LocalValueNumbering::IntersectSets() {
  DCHECK_GE(gvn_->merge_lvns_.size(), 2u);

  // Find the LVN with the least entries in the set.
  const LocalValueNumbering* least_entries_lvn = gvn_->merge_lvns_[0];
  for (const LocalValueNumbering* lvn : gvn_->merge_lvns_) {
    if ((lvn->*set_ptr).size() < (least_entries_lvn->*set_ptr).size()) {
      least_entries_lvn = lvn;
    }
  }

  // For each key check if it's in all the LVNs.
  for (const auto& key : least_entries_lvn->*set_ptr) {
    bool checked = true;
    for (const LocalValueNumbering* lvn : gvn_->merge_lvns_) {
      if (lvn != least_entries_lvn && (lvn->*set_ptr).count(key) == 0u) {
        checked = false;
        break;
      }
    }
    if (checked) {
      (this->*set_ptr).emplace_hint((this->*set_ptr).end(), key);
    }
  }
}

void LocalValueNumbering::CopyLiveSregValues(SregValueMap* dest, const SregValueMap& src) {
  auto dest_end = dest->end();
  ArenaBitVector* live_in_v = gvn_->GetMirGraph()->GetBasicBlock(id_)->data_flow_info->live_in_v;
  DCHECK(live_in_v != nullptr);
  for (const auto& entry : src) {
    bool live = live_in_v->IsBitSet(gvn_->GetMirGraph()->SRegToVReg(entry.first));
    if (live) {
      dest->PutBefore(dest_end, entry.first, entry.second);
    }
  }
}

template <LocalValueNumbering::SregValueMap LocalValueNumbering::* map_ptr>
void LocalValueNumbering::IntersectSregValueMaps() {
  DCHECK_GE(gvn_->merge_lvns_.size(), 2u);

  // Find the LVN with the least entries in the set.
  const LocalValueNumbering* least_entries_lvn = gvn_->merge_lvns_[0];
  for (const LocalValueNumbering* lvn : gvn_->merge_lvns_) {
    if ((lvn->*map_ptr).size() < (least_entries_lvn->*map_ptr).size()) {
      least_entries_lvn = lvn;
    }
  }

  // For each key check if it's in all the LVNs.
  ArenaBitVector* live_in_v = gvn_->GetMirGraph()->GetBasicBlock(id_)->data_flow_info->live_in_v;
  DCHECK(live_in_v != nullptr);
  for (const auto& entry : least_entries_lvn->*map_ptr) {
    bool live_and_same = live_in_v->IsBitSet(gvn_->GetMirGraph()->SRegToVReg(entry.first));
    if (live_and_same) {
      for (const LocalValueNumbering* lvn : gvn_->merge_lvns_) {
        if (lvn != least_entries_lvn) {
          auto it = (lvn->*map_ptr).find(entry.first);
          if (it == (lvn->*map_ptr).end() || !(it->second == entry.second)) {
            live_and_same = false;
            break;
          }
        }
      }
    }
    if (live_and_same) {
      (this->*map_ptr).PutBefore((this->*map_ptr).end(), entry.first, entry.second);
    }
  }
}

// Intersect maps as sets. The value type must be equality-comparable.
template <typename Map>
void LocalValueNumbering::InPlaceIntersectMaps(Map* work_map, const Map& other_map) {
  auto work_it = work_map->begin(), work_end = work_map->end();
  auto cmp = work_map->value_comp();
  for (const auto& entry : other_map) {
    while (work_it != work_end &&
        (cmp(*work_it, entry) ||
         (!cmp(entry, *work_it) && !(work_it->second == entry.second)))) {
      work_it = work_map->erase(work_it);
    }
    if (work_it == work_end) {
      return;
    }
    ++work_it;
  }
}

template <typename Set, Set LocalValueNumbering::*set_ptr, void (LocalValueNumbering::*MergeFn)(
    const typename Set::value_type& entry, typename Set::iterator hint)>
void LocalValueNumbering::MergeSets() {
  auto cmp = (this->*set_ptr).value_comp();
  for (const LocalValueNumbering* lvn : gvn_->merge_lvns_) {
    auto my_it = (this->*set_ptr).begin(), my_end = (this->*set_ptr).end();
    for (const auto& entry : lvn->*set_ptr) {
      while (my_it != my_end && cmp(*my_it, entry)) {
        ++my_it;
      }
      if (my_it != my_end && !cmp(entry, *my_it)) {
        // Already handled.
        ++my_it;
      } else {
        // Merge values for this field_id.
        (this->*MergeFn)(entry, my_it);  // my_it remains valid across inserts to std::set/SafeMap.
      }
    }
  }
}

void LocalValueNumbering::IntersectAliasingValueLocations(AliasingValues* work_values,
                                                          const AliasingValues* values) {
  auto cmp = work_values->load_value_map.key_comp();
  auto work_it = work_values->load_value_map.begin(), work_end = work_values->load_value_map.end();
  auto store_it = values->store_loc_set.begin(), store_end = values->store_loc_set.end();
  auto load_it = values->load_value_map.begin(), load_end = values->load_value_map.end();
  while (store_it != store_end || load_it != load_end) {
    uint16_t loc;
    if (store_it != store_end && (load_it == load_end || *store_it < load_it->first)) {
      loc = *store_it;
      ++store_it;
    } else {
      loc = load_it->first;
      ++load_it;
      DCHECK(store_it == store_end || cmp(loc, *store_it));
    }
    while (work_it != work_end && cmp(work_it->first, loc)) {
      work_it = work_values->load_value_map.erase(work_it);
    }
    if (work_it != work_end && !cmp(loc, work_it->first)) {
      // The location matches, keep it.
      ++work_it;
    }
  }
  while (work_it != work_end) {
    work_it = work_values->load_value_map.erase(work_it);
  }
}

void LocalValueNumbering::MergeEscapedRefs(const ValueNameSet::value_type& entry,
                                           ValueNameSet::iterator hint) {
  // See if the ref is either escaped or non-aliasing in each predecessor.
  bool is_escaped = true;
  for (const LocalValueNumbering* lvn : gvn_->merge_lvns_) {
    if (lvn->non_aliasing_refs_.count(entry) == 0u &&
        lvn->escaped_refs_.count(entry) == 0u) {
      is_escaped = false;
      break;
    }
  }
  if (is_escaped) {
    escaped_refs_.emplace_hint(hint, entry);
  }
}

void LocalValueNumbering::MergeEscapedIFieldTypeClobberSets(
    const EscapedIFieldClobberSet::value_type& entry, EscapedIFieldClobberSet::iterator hint) {
  // Insert only type-clobber entries (field_id == kNoValue) of escaped refs.
  if (entry.field_id == kNoValue && escaped_refs_.count(entry.base) != 0u) {
    escaped_ifield_clobber_set_.emplace_hint(hint, entry);
  }
}

void LocalValueNumbering::MergeEscapedIFieldClobberSets(
    const EscapedIFieldClobberSet::value_type& entry, EscapedIFieldClobberSet::iterator hint) {
  // Insert only those entries of escaped refs that are not overridden by a type clobber.
  if (!(hint == escaped_ifield_clobber_set_.end() &&
        hint->base == entry.base && hint->type == entry.type) &&
      escaped_refs_.count(entry.base) != 0u) {
    escaped_ifield_clobber_set_.emplace_hint(hint, entry);
  }
}

void LocalValueNumbering::MergeEscapedArrayClobberSets(
    const EscapedArrayClobberSet::value_type& entry, EscapedArrayClobberSet::iterator hint) {
  if (escaped_refs_.count(entry.base) != 0u) {
    escaped_array_clobber_set_.emplace_hint(hint, entry);
  }
}

void LocalValueNumbering::MergeNullChecked(const ValueNameSet::value_type& entry,
                                           ValueNameSet::iterator hint) {
  // Merge null_checked_ for this ref.
  merge_names_.clear();
  merge_names_.resize(gvn_->merge_lvns_.size(), entry);
  if (gvn_->NullCheckedInAllPredecessors(merge_names_)) {
    null_checked_.insert(hint, entry);
  }
}

void LocalValueNumbering::MergeSFieldValues(const SFieldToValueMap::value_type& entry,
                                            SFieldToValueMap::iterator hint) {
  uint16_t field_id = entry.first;
  merge_names_.clear();
  uint16_t value_name = kNoValue;
  bool same_values = true;
  for (const LocalValueNumbering* lvn : gvn_->merge_lvns_) {
    // Get the value name as in HandleSGet() but don't modify *lvn.
    auto it = lvn->sfield_value_map_.find(field_id);
    if (it != lvn->sfield_value_map_.end()) {
      value_name = it->second;
    } else {
      uint16_t type = gvn_->GetFieldType(field_id);
      value_name = gvn_->LookupValue(kResolvedSFieldOp, field_id,
                                     lvn->unresolved_sfield_version_[type],
                                     lvn->global_memory_version_);
    }

    same_values = same_values && (merge_names_.empty() || value_name == merge_names_.back());
    merge_names_.push_back(value_name);
  }
  if (same_values) {
    // value_name already contains the result.
  } else {
    auto lb = merge_map_.lower_bound(merge_names_);
    if (lb != merge_map_.end() && !merge_map_.key_comp()(merge_names_, lb->first)) {
      value_name = lb->second;
    } else {
      value_name = gvn_->LookupValue(kMergeBlockSFieldVersionBumpOp, field_id, id_, kNoValue);
      merge_map_.PutBefore(lb, merge_names_, value_name);
      if (gvn_->NullCheckedInAllPredecessors(merge_names_)) {
        null_checked_.insert(value_name);
      }
    }
  }
  sfield_value_map_.PutBefore(hint, field_id, value_name);
}

void LocalValueNumbering::MergeNonAliasingIFieldValues(const IFieldLocToValueMap::value_type& entry,
                                                       IFieldLocToValueMap::iterator hint) {
  uint16_t field_loc = entry.first;
  merge_names_.clear();
  uint16_t value_name = kNoValue;
  bool same_values = true;
  for (const LocalValueNumbering* lvn : gvn_->merge_lvns_) {
    // Get the value name as in HandleIGet() but don't modify *lvn.
    auto it = lvn->non_aliasing_ifield_value_map_.find(field_loc);
    if (it != lvn->non_aliasing_ifield_value_map_.end()) {
      value_name = it->second;
    } else {
      value_name = gvn_->LookupValue(kNonAliasingIFieldInitialOp, field_loc, kNoValue, kNoValue);
    }

    same_values = same_values && (merge_names_.empty() || value_name == merge_names_.back());
    merge_names_.push_back(value_name);
  }
  if (same_values) {
    // value_name already contains the result.
  } else {
    auto lb = merge_map_.lower_bound(merge_names_);
    if (lb != merge_map_.end() && !merge_map_.key_comp()(merge_names_, lb->first)) {
      value_name = lb->second;
    } else {
      value_name = gvn_->LookupValue(kMergeBlockNonAliasingIFieldVersionBumpOp, field_loc,
                                     id_, kNoValue);
      merge_map_.PutBefore(lb, merge_names_, value_name);
      if (gvn_->NullCheckedInAllPredecessors(merge_names_)) {
        null_checked_.insert(value_name);
      }
    }
  }
  non_aliasing_ifield_value_map_.PutBefore(hint, field_loc, value_name);
}

template <typename Map, Map LocalValueNumbering::*map_ptr, typename Versions>
void LocalValueNumbering::MergeAliasingValues(const typename Map::value_type& entry,
                                              typename Map::iterator hint) {
  const typename Map::key_type& key = entry.first;

  auto it = (this->*map_ptr).PutBefore(hint, key, AliasingValues(this));
  AliasingValues* my_values = &it->second;

  const AliasingValues* cmp_values = nullptr;
  bool same_version = !Versions::HasNewBaseVersion(gvn_, this, key);
  uint16_t load_memory_version_for_same_version = kNoValue;
  if (same_version) {
    // Find the first non-null values.
    for (const LocalValueNumbering* lvn : gvn_->merge_lvns_) {
      auto it = (lvn->*map_ptr).find(key);
      if (it != (lvn->*map_ptr).end()) {
        cmp_values = &it->second;
        break;
      }
    }
    DCHECK(cmp_values != nullptr);  // There must be at least one non-null values.

    // Check if we have identical memory versions, i.e. the global memory version, unresolved
    // field version and the values' memory_version_before_stores, last_stored_value
    // and store_loc_set are identical.
    for (const LocalValueNumbering* lvn : gvn_->merge_lvns_) {
      auto it = (lvn->*map_ptr).find(key);
      if (it == (lvn->*map_ptr).end()) {
        if (cmp_values->memory_version_before_stores != kNoValue) {
          same_version = false;
          break;
        }
      } else if (cmp_values->last_stored_value != it->second.last_stored_value ||
          cmp_values->memory_version_before_stores != it->second.memory_version_before_stores ||
          cmp_values->store_loc_set != it->second.store_loc_set) {
        same_version = false;
        break;
      } else if (it->second.last_load_memory_version != kNoValue) {
        DCHECK(load_memory_version_for_same_version == kNoValue ||
               load_memory_version_for_same_version == it->second.last_load_memory_version);
        load_memory_version_for_same_version = it->second.last_load_memory_version;
      }
    }
  }

  if (same_version) {
    // Copy the identical values.
    my_values->memory_version_before_stores = cmp_values->memory_version_before_stores;
    my_values->last_stored_value = cmp_values->last_stored_value;
    my_values->store_loc_set = cmp_values->store_loc_set;
    my_values->last_load_memory_version = load_memory_version_for_same_version;
    // Merge load values seen in all incoming arcs (i.e. an intersection).
    if (!cmp_values->load_value_map.empty()) {
      my_values->load_value_map = cmp_values->load_value_map;
      for (const LocalValueNumbering* lvn : gvn_->merge_lvns_) {
        auto it = (lvn->*map_ptr).find(key);
        if (it == (lvn->*map_ptr).end() || it->second.load_value_map.empty()) {
          my_values->load_value_map.clear();
          break;
        }
        InPlaceIntersectMaps(&my_values->load_value_map, it->second.load_value_map);
        if (my_values->load_value_map.empty()) {
          break;
        }
      }
    }
  } else {
    // Bump version number for the merge.
    my_values->memory_version_before_stores = my_values->last_load_memory_version =
        Versions::LookupMergeBlockValue(gvn_, id_, key);

    // Calculate the locations that have been either read from or written to in each incoming LVN.
    bool first_lvn = true;
    for (const LocalValueNumbering* lvn : gvn_->merge_lvns_) {
      auto it = (lvn->*map_ptr).find(key);
      if (it == (lvn->*map_ptr).end()) {
        my_values->load_value_map.clear();
        break;
      }
      if (first_lvn) {
        first_lvn = false;
        // Copy the first LVN's locations. Values will be overwritten later.
        my_values->load_value_map = it->second.load_value_map;
        for (uint16_t location : it->second.store_loc_set) {
          my_values->load_value_map.Put(location, 0u);
        }
      } else {
        IntersectAliasingValueLocations(my_values, &it->second);
      }
    }
    // Calculate merged values for the intersection.
    for (auto& load_value_entry : my_values->load_value_map) {
      uint16_t location = load_value_entry.first;
      bool same_values = true;
      uint16_t value_name = kNoValue;
      merge_names_.clear();
      for (const LocalValueNumbering* lvn : gvn_->merge_lvns_) {
        value_name = Versions::LookupMergeValue(gvn_, lvn, key, location);
        same_values = same_values && (merge_names_.empty() || value_name == merge_names_.back());
        merge_names_.push_back(value_name);
      }
      if (same_values) {
        // value_name already contains the result.
      } else {
        auto lb = merge_map_.lower_bound(merge_names_);
        if (lb != merge_map_.end() && !merge_map_.key_comp()(merge_names_, lb->first)) {
          value_name = lb->second;
        } else {
          // NOTE: In addition to the key and id_ which don't change on an LVN recalculation
          // during GVN, we also add location which can actually change on recalculation, so the
          // value_name below may change. This could lead to an infinite loop if the location
          // value name always changed when the refereced value name changes. However, given that
          // we assign unique value names for other merges, such as Phis, such a dependency is
          // not possible in a well-formed SSA graph.
          value_name = Versions::LookupMergeLocationValue(gvn_, id_, key, location);
          merge_map_.PutBefore(lb, merge_names_, value_name);
          if (gvn_->NullCheckedInAllPredecessors(merge_names_)) {
            null_checked_.insert(value_name);
          }
        }
      }
      load_value_entry.second = value_name;
    }
  }
}

void LocalValueNumbering::Merge(MergeType merge_type) {
  DCHECK_GE(gvn_->merge_lvns_.size(), 2u);

  IntersectSregValueMaps<&LocalValueNumbering::sreg_value_map_>();
  IntersectSregValueMaps<&LocalValueNumbering::sreg_wide_value_map_>();
  if (merge_type == kReturnMerge) {
    // RETURN or PHI+RETURN. We need only sreg value maps.
    return;
  }

  MergeMemoryVersions(merge_type == kCatchMerge);

  // Merge non-aliasing maps/sets.
  IntersectSets<ValueNameSet, &LocalValueNumbering::non_aliasing_refs_>();
  if (!non_aliasing_refs_.empty() && merge_type == kCatchMerge) {
    PruneNonAliasingRefsForCatch();
  }
  if (!non_aliasing_refs_.empty()) {
    MergeSets<IFieldLocToValueMap, &LocalValueNumbering::non_aliasing_ifield_value_map_,
              &LocalValueNumbering::MergeNonAliasingIFieldValues>();
    MergeSets<NonAliasingArrayValuesMap, &LocalValueNumbering::non_aliasing_array_value_map_,
              &LocalValueNumbering::MergeAliasingValues<
                  NonAliasingArrayValuesMap, &LocalValueNumbering::non_aliasing_array_value_map_,
                  NonAliasingArrayVersions>>();
  }

  // We won't do anything complicated for range checks, just calculate the intersection.
  IntersectSets<RangeCheckSet, &LocalValueNumbering::range_checked_>();

  // Merge null_checked_. We may later insert more, such as merged object field values.
  MergeSets<ValueNameSet, &LocalValueNumbering::null_checked_,
            &LocalValueNumbering::MergeNullChecked>();

  if (merge_type == kCatchMerge) {
    // Memory is clobbered. New memory version already created, don't merge aliasing locations.
    return;
  }

  DCHECK(merge_type == kNormalMerge);

  // Merge escaped refs and clobber sets.
  MergeSets<ValueNameSet, &LocalValueNumbering::escaped_refs_,
            &LocalValueNumbering::MergeEscapedRefs>();
  if (!escaped_refs_.empty()) {
    MergeSets<EscapedIFieldClobberSet, &LocalValueNumbering::escaped_ifield_clobber_set_,
              &LocalValueNumbering::MergeEscapedIFieldTypeClobberSets>();
    MergeSets<EscapedIFieldClobberSet, &LocalValueNumbering::escaped_ifield_clobber_set_,
              &LocalValueNumbering::MergeEscapedIFieldClobberSets>();
    MergeSets<EscapedArrayClobberSet, &LocalValueNumbering::escaped_array_clobber_set_,
              &LocalValueNumbering::MergeEscapedArrayClobberSets>();
  }

  MergeSets<SFieldToValueMap, &LocalValueNumbering::sfield_value_map_,
            &LocalValueNumbering::MergeSFieldValues>();
  MergeSets<AliasingIFieldValuesMap, &LocalValueNumbering::aliasing_ifield_value_map_,
            &LocalValueNumbering::MergeAliasingValues<
                AliasingIFieldValuesMap, &LocalValueNumbering::aliasing_ifield_value_map_,
                AliasingIFieldVersions>>();
  MergeSets<AliasingArrayValuesMap, &LocalValueNumbering::aliasing_array_value_map_,
            &LocalValueNumbering::MergeAliasingValues<
                AliasingArrayValuesMap, &LocalValueNumbering::aliasing_array_value_map_,
                AliasingArrayVersions>>();
}

uint16_t LocalValueNumbering::MarkNonAliasingNonNull(MIR* mir) {
  uint16_t res = GetOperandValue(mir->ssa_rep->defs[0]);
  DCHECK(null_checked_.find(res) == null_checked_.end());
  null_checked_.insert(res);
  non_aliasing_refs_.insert(res);
  return res;
}

bool LocalValueNumbering::IsNonAliasing(uint16_t reg) const {
  return non_aliasing_refs_.find(reg) != non_aliasing_refs_.end();
}

bool LocalValueNumbering::IsNonAliasingIField(uint16_t reg, uint16_t field_id,
                                              uint16_t type) const {
  if (IsNonAliasing(reg)) {
    return true;
  }
  if (escaped_refs_.find(reg) == escaped_refs_.end()) {
    return false;
  }
  // Check for IPUTs to unresolved fields.
  EscapedIFieldClobberKey key1 = { reg, type, kNoValue };
  if (escaped_ifield_clobber_set_.find(key1) != escaped_ifield_clobber_set_.end()) {
    return false;
  }
  // Check for aliased IPUTs to the same field.
  EscapedIFieldClobberKey key2 = { reg, type, field_id };
  return escaped_ifield_clobber_set_.find(key2) == escaped_ifield_clobber_set_.end();
}

bool LocalValueNumbering::IsNonAliasingArray(uint16_t reg, uint16_t type) const {
  if (IsNonAliasing(reg)) {
    return true;
  }
  if (escaped_refs_.count(reg) == 0u) {
    return false;
  }
  // Check for aliased APUTs.
  EscapedArrayClobberKey key = { reg, type };
  return escaped_array_clobber_set_.find(key) == escaped_array_clobber_set_.end();
}

void LocalValueNumbering::HandleNullCheck(MIR* mir, uint16_t reg) {
  auto lb = null_checked_.lower_bound(reg);
  if (lb != null_checked_.end() && *lb == reg) {
    if (LIKELY(gvn_->CanModify())) {
      if (gvn_->GetCompilationUnit()->verbose) {
        LOG(INFO) << "Removing null check for 0x" << std::hex << mir->offset;
      }
      mir->optimization_flags |= MIR_IGNORE_NULL_CHECK;
    }
  } else {
    null_checked_.insert(lb, reg);
  }
}

void LocalValueNumbering::HandleRangeCheck(MIR* mir, uint16_t array, uint16_t index) {
  RangeCheckKey key = { array, index };
  auto lb = range_checked_.lower_bound(key);
  if (lb != range_checked_.end() && !RangeCheckKeyComparator()(key, *lb)) {
    if (LIKELY(gvn_->CanModify())) {
      if (gvn_->GetCompilationUnit()->verbose) {
        LOG(INFO) << "Removing range check for 0x" << std::hex << mir->offset;
      }
      mir->optimization_flags |= MIR_IGNORE_RANGE_CHECK;
    }
  } else {
    // Mark range check completed.
    range_checked_.insert(lb, key);
  }
}

void LocalValueNumbering::HandlePutObject(MIR* mir) {
  // If we're storing a non-aliasing reference, stop tracking it as non-aliasing now.
  uint16_t base = GetOperandValue(mir->ssa_rep->uses[0]);
  HandleEscapingRef(base);
}

void LocalValueNumbering::HandleEscapingRef(uint16_t base) {
  auto it = non_aliasing_refs_.find(base);
  if (it != non_aliasing_refs_.end()) {
    non_aliasing_refs_.erase(it);
    escaped_refs_.insert(base);
  }
}

uint16_t LocalValueNumbering::HandlePhi(MIR* mir) {
  if (gvn_->merge_lvns_.empty()) {
    // Running LVN without a full GVN?
    return kNoValue;
  }
  int16_t num_uses = mir->ssa_rep->num_uses;
  int32_t* uses = mir->ssa_rep->uses;
  // Try to find out if this is merging wide regs.
  if (mir->ssa_rep->defs[0] != 0 &&
      sreg_wide_value_map_.count(mir->ssa_rep->defs[0] - 1) != 0u) {
    // This is the high part of a wide reg. Ignore the Phi.
    return kNoValue;
  }
  bool wide = false;
  for (int16_t i = 0; i != num_uses; ++i) {
    if (sreg_wide_value_map_.count(uses[i]) != 0u) {
      wide = true;
      break;
    }
  }
  // Iterate over *merge_lvns_ and skip incoming sregs for BBs without associated LVN.
  uint16_t value_name = kNoValue;
  merge_names_.clear();
  BasicBlockId* incoming = mir->meta.phi_incoming;
  int16_t pos = 0;
  bool same_values = true;
  for (const LocalValueNumbering* lvn : gvn_->merge_lvns_) {
    DCHECK_LT(pos, mir->ssa_rep->num_uses);
    while (incoming[pos] != lvn->Id()) {
      ++pos;
      DCHECK_LT(pos, mir->ssa_rep->num_uses);
    }
    int s_reg = uses[pos];
    ++pos;
    value_name = wide ? lvn->GetOperandValueWide(s_reg) : lvn->GetOperandValue(s_reg);

    same_values = same_values && (merge_names_.empty() || value_name == merge_names_.back());
    merge_names_.push_back(value_name);
  }
  if (same_values) {
    // value_name already contains the result.
  } else {
    auto lb = merge_map_.lower_bound(merge_names_);
    if (lb != merge_map_.end() && !merge_map_.key_comp()(merge_names_, lb->first)) {
      value_name = lb->second;
    } else {
      value_name = gvn_->LookupValue(kNoValue, mir->ssa_rep->defs[0], kNoValue, kNoValue);
      merge_map_.PutBefore(lb, merge_names_, value_name);
      if (!wide && gvn_->NullCheckedInAllPredecessors(merge_names_)) {
        null_checked_.insert(value_name);
      }
    }
  }
  if (wide) {
    SetOperandValueWide(mir->ssa_rep->defs[0], value_name);
  } else {
    SetOperandValue(mir->ssa_rep->defs[0], value_name);
  }
  return value_name;
}

uint16_t LocalValueNumbering::HandleAGet(MIR* mir, uint16_t opcode) {
  // uint16_t type = opcode - Instruction::AGET;
  uint16_t array = GetOperandValue(mir->ssa_rep->uses[0]);
  HandleNullCheck(mir, array);
  uint16_t index = GetOperandValue(mir->ssa_rep->uses[1]);
  HandleRangeCheck(mir, array, index);
  uint16_t type = opcode - Instruction::AGET;
  // Establish value number for loaded register.
  uint16_t res;
  if (IsNonAliasingArray(array, type)) {
    res = HandleAliasingValuesGet<NonAliasingArrayVersions>(&non_aliasing_array_value_map_,
                                                            array, index);
  } else {
    uint16_t location = gvn_->GetArrayLocation(array, index);
    res = HandleAliasingValuesGet<AliasingArrayVersions>(&aliasing_array_value_map_,
                                                         type, location);
  }
  if (opcode == Instruction::AGET_WIDE) {
    SetOperandValueWide(mir->ssa_rep->defs[0], res);
  } else {
    SetOperandValue(mir->ssa_rep->defs[0], res);
  }
  return res;
}

void LocalValueNumbering::HandleAPut(MIR* mir, uint16_t opcode) {
  int array_idx = (opcode == Instruction::APUT_WIDE) ? 2 : 1;
  int index_idx = array_idx + 1;
  uint16_t array = GetOperandValue(mir->ssa_rep->uses[array_idx]);
  HandleNullCheck(mir, array);
  uint16_t index = GetOperandValue(mir->ssa_rep->uses[index_idx]);
  HandleRangeCheck(mir, array, index);

  uint16_t type = opcode - Instruction::APUT;
  uint16_t value = (opcode == Instruction::APUT_WIDE)
                   ? GetOperandValueWide(mir->ssa_rep->uses[0])
                   : GetOperandValue(mir->ssa_rep->uses[0]);
  if (IsNonAliasing(array)) {
    bool put_is_live = HandleAliasingValuesPut<NonAliasingArrayVersions>(
        &non_aliasing_array_value_map_, array, index, value);
    if (!put_is_live) {
      // This APUT can be eliminated, it stores the same value that's already in the field.
      // TODO: Eliminate the APUT.
      return;
    }
  } else {
    uint16_t location = gvn_->GetArrayLocation(array, index);
    bool put_is_live = HandleAliasingValuesPut<AliasingArrayVersions>(
        &aliasing_array_value_map_, type, location, value);
    if (!put_is_live) {
      // This APUT can be eliminated, it stores the same value that's already in the field.
      // TODO: Eliminate the APUT.
      return;
    }

    // Clobber all escaped array refs for this type.
    for (uint16_t escaped_array : escaped_refs_) {
      EscapedArrayClobberKey clobber_key = { escaped_array, type };
      escaped_array_clobber_set_.insert(clobber_key);
    }
  }
}

uint16_t LocalValueNumbering::HandleIGet(MIR* mir, uint16_t opcode) {
  uint16_t base = GetOperandValue(mir->ssa_rep->uses[0]);
  HandleNullCheck(mir, base);
  const MirFieldInfo& field_info = gvn_->GetMirGraph()->GetIFieldLoweringInfo(mir);
  uint16_t res;
  if (!field_info.IsResolved() || field_info.IsVolatile()) {
    // Volatile fields always get a new memory version; field id is irrelevant.
    // Unresolved fields may be volatile, so handle them as such to be safe.
    // Use result s_reg - will be unique.
    res = gvn_->LookupValue(kNoValue, mir->ssa_rep->defs[0], kNoValue, kNoValue);
  } else {
    uint16_t type = opcode - Instruction::IGET;
    uint16_t field_id = gvn_->GetFieldId(field_info, type);
    if (IsNonAliasingIField(base, field_id, type)) {
      uint16_t loc = gvn_->LookupValue(kNonAliasingIFieldLocOp, base, field_id, type);
      auto lb = non_aliasing_ifield_value_map_.lower_bound(loc);
      if (lb != non_aliasing_ifield_value_map_.end() && lb->first == loc) {
        res = lb->second;
      } else {
        res = gvn_->LookupValue(kNonAliasingIFieldInitialOp, loc, kNoValue, kNoValue);
        non_aliasing_ifield_value_map_.PutBefore(lb, loc, res);
      }
    } else {
      res = HandleAliasingValuesGet<AliasingIFieldVersions>(&aliasing_ifield_value_map_,
                                                            field_id, base);
    }
  }
  if (opcode == Instruction::IGET_WIDE) {
    SetOperandValueWide(mir->ssa_rep->defs[0], res);
  } else {
    SetOperandValue(mir->ssa_rep->defs[0], res);
  }
  return res;
}

void LocalValueNumbering::HandleIPut(MIR* mir, uint16_t opcode) {
  uint16_t type = opcode - Instruction::IPUT;
  int base_reg = (opcode == Instruction::IPUT_WIDE) ? 2 : 1;
  uint16_t base = GetOperandValue(mir->ssa_rep->uses[base_reg]);
  HandleNullCheck(mir, base);
  const MirFieldInfo& field_info = gvn_->GetMirGraph()->GetIFieldLoweringInfo(mir);
  if (!field_info.IsResolved()) {
    // Unresolved fields always alias with everything of the same type.
    // Use mir->offset as modifier; without elaborate inlining, it will be unique.
    unresolved_ifield_version_[type] =
        gvn_->LookupValue(kUnresolvedIFieldOp, kNoValue, kNoValue, mir->offset);

    // For simplicity, treat base as escaped now.
    HandleEscapingRef(base);

    // Clobber all fields of escaped references of the same type.
    for (uint16_t escaped_ref : escaped_refs_) {
      EscapedIFieldClobberKey clobber_key = { escaped_ref, type, kNoValue };
      escaped_ifield_clobber_set_.insert(clobber_key);
    }

    // Aliasing fields of the same type may have been overwritten.
    auto it = aliasing_ifield_value_map_.begin(), end = aliasing_ifield_value_map_.end();
    while (it != end) {
      if (gvn_->GetFieldType(it->first) != type) {
        ++it;
      } else {
        it = aliasing_ifield_value_map_.erase(it);
      }
    }
  } else if (field_info.IsVolatile()) {
    // Nothing to do, resolved volatile fields always get a new memory version anyway and
    // can't alias with resolved non-volatile fields.
  } else {
    uint16_t field_id = gvn_->GetFieldId(field_info, type);
    uint16_t value = (opcode == Instruction::IPUT_WIDE)
                     ? GetOperandValueWide(mir->ssa_rep->uses[0])
                     : GetOperandValue(mir->ssa_rep->uses[0]);
    if (IsNonAliasing(base)) {
      uint16_t loc = gvn_->LookupValue(kNonAliasingIFieldLocOp, base, field_id, type);
      auto lb = non_aliasing_ifield_value_map_.lower_bound(loc);
      if (lb != non_aliasing_ifield_value_map_.end() && lb->first == loc) {
        if (lb->second == value) {
          // This IPUT can be eliminated, it stores the same value that's already in the field.
          // TODO: Eliminate the IPUT.
          return;
        }
        lb->second = value;  // Overwrite.
      } else {
        non_aliasing_ifield_value_map_.PutBefore(lb, loc, value);
      }
    } else {
      bool put_is_live = HandleAliasingValuesPut<AliasingIFieldVersions>(
          &aliasing_ifield_value_map_, field_id, base, value);
      if (!put_is_live) {
        // This IPUT can be eliminated, it stores the same value that's already in the field.
        // TODO: Eliminate the IPUT.
        return;
      }

      // Clobber all fields of escaped references for this field.
      for (uint16_t escaped_ref : escaped_refs_) {
        EscapedIFieldClobberKey clobber_key = { escaped_ref, type, field_id };
        escaped_ifield_clobber_set_.insert(clobber_key);
      }
    }
  }
}

uint16_t LocalValueNumbering::HandleSGet(MIR* mir, uint16_t opcode) {
  const MirSFieldLoweringInfo& field_info = gvn_->GetMirGraph()->GetSFieldLoweringInfo(mir);
  if (!field_info.IsInitialized() && (mir->optimization_flags & MIR_IGNORE_CLINIT_CHECK) == 0) {
    // Class initialization can call arbitrary functions, we need to wipe aliasing values.
    HandleInvokeOrClInit(mir);
  }
  uint16_t res;
  if (!field_info.IsResolved() || field_info.IsVolatile()) {
    // Volatile fields always get a new memory version; field id is irrelevant.
    // Unresolved fields may be volatile, so handle them as such to be safe.
    // Use result s_reg - will be unique.
    res = gvn_->LookupValue(kNoValue, mir->ssa_rep->defs[0], kNoValue, kNoValue);
  } else {
    uint16_t type = opcode - Instruction::SGET;
    uint16_t field_id = gvn_->GetFieldId(field_info, type);
    auto lb = sfield_value_map_.lower_bound(field_id);
    if (lb != sfield_value_map_.end() && lb->first == field_id) {
      res = lb->second;
    } else {
      // Resolved non-volatile static fields can alias with non-resolved fields of the same type,
      // so we need to use unresolved_sfield_version_[type] in addition to global_memory_version_
      // to determine the version of the field.
      res = gvn_->LookupValue(kResolvedSFieldOp, field_id,
                              unresolved_sfield_version_[type], global_memory_version_);
      sfield_value_map_.PutBefore(lb, field_id, res);
    }
  }
  if (opcode == Instruction::SGET_WIDE) {
    SetOperandValueWide(mir->ssa_rep->defs[0], res);
  } else {
    SetOperandValue(mir->ssa_rep->defs[0], res);
  }
  return res;
}

void LocalValueNumbering::HandleSPut(MIR* mir, uint16_t opcode) {
  const MirSFieldLoweringInfo& field_info = gvn_->GetMirGraph()->GetSFieldLoweringInfo(mir);
  if (!field_info.IsInitialized() && (mir->optimization_flags & MIR_IGNORE_CLINIT_CHECK) == 0) {
    // Class initialization can call arbitrary functions, we need to wipe aliasing values.
    HandleInvokeOrClInit(mir);
  }
  uint16_t type = opcode - Instruction::SPUT;
  if (!field_info.IsResolved()) {
    // Unresolved fields always alias with everything of the same type.
    // Use mir->offset as modifier; without elaborate inlining, it will be unique.
    unresolved_sfield_version_[type] =
        gvn_->LookupValue(kUnresolvedSFieldOp, kNoValue, kNoValue, mir->offset);
    RemoveSFieldsForType(type);
  } else if (field_info.IsVolatile()) {
    // Nothing to do, resolved volatile fields always get a new memory version anyway and
    // can't alias with resolved non-volatile fields.
  } else {
    uint16_t field_id = gvn_->GetFieldId(field_info, type);
    uint16_t value = (opcode == Instruction::SPUT_WIDE)
                     ? GetOperandValueWide(mir->ssa_rep->uses[0])
                     : GetOperandValue(mir->ssa_rep->uses[0]);
    // Resolved non-volatile static fields can alias with non-resolved fields of the same type,
    // so we need to use unresolved_sfield_version_[type] in addition to global_memory_version_
    // to determine the version of the field.
    auto lb = sfield_value_map_.lower_bound(field_id);
    if (lb != sfield_value_map_.end() && lb->first == field_id) {
      if (lb->second == value) {
        // This SPUT can be eliminated, it stores the same value that's already in the field.
        // TODO: Eliminate the SPUT.
        return;
      }
      lb->second = value;  // Overwrite.
    } else {
      sfield_value_map_.PutBefore(lb, field_id, value);
    }
  }
}

void LocalValueNumbering::RemoveSFieldsForType(uint16_t type) {
  // Erase all static fields of this type from the sfield_value_map_.
  for (auto it = sfield_value_map_.begin(), end = sfield_value_map_.end(); it != end; ) {
    if (gvn_->GetFieldType(it->first) == type) {
      it = sfield_value_map_.erase(it);
    } else {
      ++it;
    }
  }
}

void LocalValueNumbering::HandleInvokeOrClInit(MIR* mir) {
  // Use mir->offset as modifier; without elaborate inlining, it will be unique.
  global_memory_version_ =
      gvn_->LookupValue(kInvokeMemoryVersionBumpOp, 0u, 0u, mir->offset);
  // All static fields and instance fields and array elements of aliasing references,
  // including escaped references, may have been modified.
  sfield_value_map_.clear();
  aliasing_ifield_value_map_.clear();
  aliasing_array_value_map_.clear();
  escaped_refs_.clear();
  escaped_ifield_clobber_set_.clear();
  escaped_array_clobber_set_.clear();
}

uint16_t LocalValueNumbering::GetValueNumber(MIR* mir) {
  uint16_t res = kNoValue;
  uint16_t opcode = mir->dalvikInsn.opcode;
  switch (opcode) {
    case Instruction::NOP:
    case Instruction::RETURN_VOID:
    case Instruction::RETURN:
    case Instruction::RETURN_OBJECT:
    case Instruction::RETURN_WIDE:
    case Instruction::GOTO:
    case Instruction::GOTO_16:
    case Instruction::GOTO_32:
    case Instruction::CHECK_CAST:
    case Instruction::THROW:
    case Instruction::FILL_ARRAY_DATA:
    case Instruction::PACKED_SWITCH:
    case Instruction::SPARSE_SWITCH:
    case Instruction::IF_EQ:
    case Instruction::IF_NE:
    case Instruction::IF_LT:
    case Instruction::IF_GE:
    case Instruction::IF_GT:
    case Instruction::IF_LE:
    case Instruction::IF_EQZ:
    case Instruction::IF_NEZ:
    case Instruction::IF_LTZ:
    case Instruction::IF_GEZ:
    case Instruction::IF_GTZ:
    case Instruction::IF_LEZ:
    case kMirOpFusedCmplFloat:
    case kMirOpFusedCmpgFloat:
    case kMirOpFusedCmplDouble:
    case kMirOpFusedCmpgDouble:
    case kMirOpFusedCmpLong:
      // Nothing defined - take no action.
      break;

    case Instruction::MONITOR_ENTER:
      HandleNullCheck(mir, GetOperandValue(mir->ssa_rep->uses[0]));
      // NOTE: Keeping all aliasing values intact. Programs that rely on loads/stores of the
      // same non-volatile locations outside and inside a synchronized block being different
      // contain races that we cannot fix.
      break;

    case Instruction::MONITOR_EXIT:
      HandleNullCheck(mir, GetOperandValue(mir->ssa_rep->uses[0]));
      // If we're running GVN and CanModify(), uneliminated null check indicates bytecode error.
      if ((gvn_->GetCompilationUnit()->disable_opt & (1u << kGlobalValueNumbering)) == 0u &&
          gvn_->CanModify() && (mir->optimization_flags & MIR_IGNORE_NULL_CHECK) == 0) {
        LOG(WARNING) << "Bytecode error: MONITOR_EXIT is still null checked at 0x" << std::hex
            << mir->offset << " in " << PrettyMethod(gvn_->cu_->method_idx, *gvn_->cu_->dex_file);
      }
      break;

    case Instruction::FILLED_NEW_ARRAY:
    case Instruction::FILLED_NEW_ARRAY_RANGE:
      // Nothing defined but the result will be unique and non-null.
      if (mir->next != nullptr && mir->next->dalvikInsn.opcode == Instruction::MOVE_RESULT_OBJECT) {
        uint16_t array = MarkNonAliasingNonNull(mir->next);
        // Do not SetOperandValue(), we'll do that when we process the MOVE_RESULT_OBJECT.
        if (kLocalValueNumberingEnableFilledNewArrayTracking && mir->ssa_rep->num_uses != 0u) {
          AliasingValues* values = GetAliasingValues(&non_aliasing_array_value_map_, array);
          // Clear the value if we got a merged version in a loop.
          *values = AliasingValues(this);
          for (size_t i = 0u, count = mir->ssa_rep->num_uses; i != count; ++i) {
            DCHECK_EQ(High16Bits(i), 0u);
            uint16_t index = gvn_->LookupValue(Instruction::CONST, i, 0u, 0);
            uint16_t value = GetOperandValue(mir->ssa_rep->uses[i]);
            values->load_value_map.Put(index, value);
            RangeCheckKey key = { array, index };
            range_checked_.insert(key);
          }
        }
        // The MOVE_RESULT_OBJECT will be processed next and we'll return the value name then.
      }
      // All args escaped (if references).
      for (size_t i = 0u, count = mir->ssa_rep->num_uses; i != count; ++i) {
        uint16_t reg = GetOperandValue(mir->ssa_rep->uses[i]);
        HandleEscapingRef(reg);
      }
      break;

    case Instruction::INVOKE_DIRECT:
    case Instruction::INVOKE_DIRECT_RANGE:
    case Instruction::INVOKE_VIRTUAL:
    case Instruction::INVOKE_VIRTUAL_RANGE:
    case Instruction::INVOKE_SUPER:
    case Instruction::INVOKE_SUPER_RANGE:
    case Instruction::INVOKE_INTERFACE:
    case Instruction::INVOKE_INTERFACE_RANGE: {
        // Nothing defined but handle the null check.
        uint16_t reg = GetOperandValue(mir->ssa_rep->uses[0]);
        HandleNullCheck(mir, reg);
      }
      // Intentional fall-through.
    case Instruction::INVOKE_STATIC:
    case Instruction::INVOKE_STATIC_RANGE:
      if ((mir->optimization_flags & MIR_INLINED) == 0) {
        // Make ref args aliasing.
        for (size_t i = 0u, count = mir->ssa_rep->num_uses; i != count; ++i) {
          uint16_t reg = GetOperandValue(mir->ssa_rep->uses[i]);
          non_aliasing_refs_.erase(reg);
        }
        HandleInvokeOrClInit(mir);
      }
      break;

    case Instruction::MOVE_RESULT:
    case Instruction::MOVE_RESULT_OBJECT:
    case Instruction::INSTANCE_OF:
      // 1 result, treat as unique each time, use result s_reg - will be unique.
      res = GetOperandValue(mir->ssa_rep->defs[0]);
      SetOperandValue(mir->ssa_rep->defs[0], res);
      break;
    case Instruction::MOVE_EXCEPTION:
    case Instruction::NEW_INSTANCE:
    case Instruction::CONST_CLASS:
    case Instruction::NEW_ARRAY:
      // 1 result, treat as unique each time, use result s_reg - will be unique.
      res = MarkNonAliasingNonNull(mir);
      SetOperandValue(mir->ssa_rep->defs[0], res);
      break;
    case Instruction::CONST_STRING:
    case Instruction::CONST_STRING_JUMBO:
      // These strings are internalized, so assign value based on the string pool index.
      res = gvn_->LookupValue(Instruction::CONST_STRING, Low16Bits(mir->dalvikInsn.vB),
                              High16Bits(mir->dalvikInsn.vB), 0);
      SetOperandValue(mir->ssa_rep->defs[0], res);
      null_checked_.insert(res);  // May already be there.
      // NOTE: Hacking the contents of an internalized string via reflection is possible
      // but the behavior is undefined. Therefore, we consider the string constant and
      // the reference non-aliasing.
      // TUNING: We could keep this property even if the reference "escapes".
      non_aliasing_refs_.insert(res);  // May already be there.
      break;
    case Instruction::MOVE_RESULT_WIDE:
      // 1 wide result, treat as unique each time, use result s_reg - will be unique.
      res = GetOperandValueWide(mir->ssa_rep->defs[0]);
      SetOperandValueWide(mir->ssa_rep->defs[0], res);
      break;

    case kMirOpPhi:
      res = HandlePhi(mir);
      break;

    case Instruction::MOVE:
    case Instruction::MOVE_OBJECT:
    case Instruction::MOVE_16:
    case Instruction::MOVE_OBJECT_16:
    case Instruction::MOVE_FROM16:
    case Instruction::MOVE_OBJECT_FROM16:
    case kMirOpCopy:
      // Just copy value number of source to value number of result.
      res = GetOperandValue(mir->ssa_rep->uses[0]);
      SetOperandValue(mir->ssa_rep->defs[0], res);
      break;

    case Instruction::MOVE_WIDE:
    case Instruction::MOVE_WIDE_16:
    case Instruction::MOVE_WIDE_FROM16:
      // Just copy value number of source to value number of result.
      res = GetOperandValueWide(mir->ssa_rep->uses[0]);
      SetOperandValueWide(mir->ssa_rep->defs[0], res);
      break;

    case Instruction::CONST:
    case Instruction::CONST_4:
    case Instruction::CONST_16:
      res = gvn_->LookupValue(Instruction::CONST, Low16Bits(mir->dalvikInsn.vB),
                              High16Bits(mir->dalvikInsn.vB), 0);
      SetOperandValue(mir->ssa_rep->defs[0], res);
      break;

    case Instruction::CONST_HIGH16:
      res = gvn_->LookupValue(Instruction::CONST, 0, mir->dalvikInsn.vB, 0);
      SetOperandValue(mir->ssa_rep->defs[0], res);
      break;

    case Instruction::CONST_WIDE_16:
    case Instruction::CONST_WIDE_32: {
        uint16_t low_res = gvn_->LookupValue(Instruction::CONST, Low16Bits(mir->dalvikInsn.vB),
                                             High16Bits(mir->dalvikInsn.vB >> 16), 1);
        uint16_t high_res;
        if (mir->dalvikInsn.vB & 0x80000000) {
          high_res = gvn_->LookupValue(Instruction::CONST, 0xffff, 0xffff, 2);
        } else {
          high_res = gvn_->LookupValue(Instruction::CONST, 0, 0, 2);
        }
        res = gvn_->LookupValue(Instruction::CONST, low_res, high_res, 3);
        SetOperandValueWide(mir->ssa_rep->defs[0], res);
      }
      break;

    case Instruction::CONST_WIDE: {
        uint32_t low_word = Low32Bits(mir->dalvikInsn.vB_wide);
        uint32_t high_word = High32Bits(mir->dalvikInsn.vB_wide);
        uint16_t low_res = gvn_->LookupValue(Instruction::CONST, Low16Bits(low_word),
                                             High16Bits(low_word), 1);
        uint16_t high_res = gvn_->LookupValue(Instruction::CONST, Low16Bits(high_word),
                                              High16Bits(high_word), 2);
        res = gvn_->LookupValue(Instruction::CONST, low_res, high_res, 3);
        SetOperandValueWide(mir->ssa_rep->defs[0], res);
      }
      break;

    case Instruction::CONST_WIDE_HIGH16: {
        uint16_t low_res = gvn_->LookupValue(Instruction::CONST, 0, 0, 1);
        uint16_t high_res = gvn_->LookupValue(Instruction::CONST, 0,
                                              Low16Bits(mir->dalvikInsn.vB), 2);
        res = gvn_->LookupValue(Instruction::CONST, low_res, high_res, 3);
        SetOperandValueWide(mir->ssa_rep->defs[0], res);
      }
      break;

    case Instruction::ARRAY_LENGTH: {
        // Handle the null check.
        uint16_t reg = GetOperandValue(mir->ssa_rep->uses[0]);
        HandleNullCheck(mir, reg);
      }
      // Intentional fall-through.
    case Instruction::NEG_INT:
    case Instruction::NOT_INT:
    case Instruction::NEG_FLOAT:
    case Instruction::INT_TO_BYTE:
    case Instruction::INT_TO_SHORT:
    case Instruction::INT_TO_CHAR:
    case Instruction::INT_TO_FLOAT:
    case Instruction::FLOAT_TO_INT: {
        // res = op + 1 operand
        uint16_t operand1 = GetOperandValue(mir->ssa_rep->uses[0]);
        res = gvn_->LookupValue(opcode, operand1, kNoValue, kNoValue);
        SetOperandValue(mir->ssa_rep->defs[0], res);
      }
      break;

    case Instruction::LONG_TO_FLOAT:
    case Instruction::LONG_TO_INT:
    case Instruction::DOUBLE_TO_FLOAT:
    case Instruction::DOUBLE_TO_INT: {
        // res = op + 1 wide operand
        uint16_t operand1 = GetOperandValueWide(mir->ssa_rep->uses[0]);
        res = gvn_->LookupValue(opcode, operand1, kNoValue, kNoValue);
        SetOperandValue(mir->ssa_rep->defs[0], res);
      }
      break;


    case Instruction::DOUBLE_TO_LONG:
    case Instruction::LONG_TO_DOUBLE:
    case Instruction::NEG_LONG:
    case Instruction::NOT_LONG:
    case Instruction::NEG_DOUBLE: {
        // wide res = op + 1 wide operand
        uint16_t operand1 = GetOperandValueWide(mir->ssa_rep->uses[0]);
        res = gvn_->LookupValue(opcode, operand1, kNoValue, kNoValue);
        SetOperandValueWide(mir->ssa_rep->defs[0], res);
      }
      break;

    case Instruction::FLOAT_TO_DOUBLE:
    case Instruction::FLOAT_TO_LONG:
    case Instruction::INT_TO_DOUBLE:
    case Instruction::INT_TO_LONG: {
        // wide res = op + 1 operand
        uint16_t operand1 = GetOperandValue(mir->ssa_rep->uses[0]);
        res = gvn_->LookupValue(opcode, operand1, kNoValue, kNoValue);
        SetOperandValueWide(mir->ssa_rep->defs[0], res);
      }
      break;

    case Instruction::CMPL_DOUBLE:
    case Instruction::CMPG_DOUBLE:
    case Instruction::CMP_LONG: {
        // res = op + 2 wide operands
        uint16_t operand1 = GetOperandValueWide(mir->ssa_rep->uses[0]);
        uint16_t operand2 = GetOperandValueWide(mir->ssa_rep->uses[2]);
        res = gvn_->LookupValue(opcode, operand1, operand2, kNoValue);
        SetOperandValue(mir->ssa_rep->defs[0], res);
      }
      break;

    case Instruction::CMPG_FLOAT:
    case Instruction::CMPL_FLOAT:
    case Instruction::ADD_INT:
    case Instruction::ADD_INT_2ADDR:
    case Instruction::MUL_INT:
    case Instruction::MUL_INT_2ADDR:
    case Instruction::AND_INT:
    case Instruction::AND_INT_2ADDR:
    case Instruction::OR_INT:
    case Instruction::OR_INT_2ADDR:
    case Instruction::XOR_INT:
    case Instruction::XOR_INT_2ADDR:
    case Instruction::SUB_INT:
    case Instruction::SUB_INT_2ADDR:
    case Instruction::DIV_INT:
    case Instruction::DIV_INT_2ADDR:
    case Instruction::REM_INT:
    case Instruction::REM_INT_2ADDR:
    case Instruction::SHL_INT:
    case Instruction::SHL_INT_2ADDR:
    case Instruction::SHR_INT:
    case Instruction::SHR_INT_2ADDR:
    case Instruction::USHR_INT:
    case Instruction::USHR_INT_2ADDR: {
        // res = op + 2 operands
        uint16_t operand1 = GetOperandValue(mir->ssa_rep->uses[0]);
        uint16_t operand2 = GetOperandValue(mir->ssa_rep->uses[1]);
        res = gvn_->LookupValue(opcode, operand1, operand2, kNoValue);
        SetOperandValue(mir->ssa_rep->defs[0], res);
      }
      break;

    case Instruction::ADD_LONG:
    case Instruction::SUB_LONG:
    case Instruction::MUL_LONG:
    case Instruction::DIV_LONG:
    case Instruction::REM_LONG:
    case Instruction::AND_LONG:
    case Instruction::OR_LONG:
    case Instruction::XOR_LONG:
    case Instruction::ADD_LONG_2ADDR:
    case Instruction::SUB_LONG_2ADDR:
    case Instruction::MUL_LONG_2ADDR:
    case Instruction::DIV_LONG_2ADDR:
    case Instruction::REM_LONG_2ADDR:
    case Instruction::AND_LONG_2ADDR:
    case Instruction::OR_LONG_2ADDR:
    case Instruction::XOR_LONG_2ADDR:
    case Instruction::ADD_DOUBLE:
    case Instruction::SUB_DOUBLE:
    case Instruction::MUL_DOUBLE:
    case Instruction::DIV_DOUBLE:
    case Instruction::REM_DOUBLE:
    case Instruction::ADD_DOUBLE_2ADDR:
    case Instruction::SUB_DOUBLE_2ADDR:
    case Instruction::MUL_DOUBLE_2ADDR:
    case Instruction::DIV_DOUBLE_2ADDR:
    case Instruction::REM_DOUBLE_2ADDR: {
        // wide res = op + 2 wide operands
        uint16_t operand1 = GetOperandValueWide(mir->ssa_rep->uses[0]);
        uint16_t operand2 = GetOperandValueWide(mir->ssa_rep->uses[2]);
        res = gvn_->LookupValue(opcode, operand1, operand2, kNoValue);
        SetOperandValueWide(mir->ssa_rep->defs[0], res);
      }
      break;

    case Instruction::SHL_LONG:
    case Instruction::SHR_LONG:
    case Instruction::USHR_LONG:
    case Instruction::SHL_LONG_2ADDR:
    case Instruction::SHR_LONG_2ADDR:
    case Instruction::USHR_LONG_2ADDR: {
        // wide res = op + 1 wide operand + 1 operand
        uint16_t operand1 = GetOperandValueWide(mir->ssa_rep->uses[0]);
        uint16_t operand2 = GetOperandValue(mir->ssa_rep->uses[2]);
        res = gvn_->LookupValue(opcode, operand1, operand2, kNoValue);
        SetOperandValueWide(mir->ssa_rep->defs[0], res);
      }
      break;

    case Instruction::ADD_FLOAT:
    case Instruction::SUB_FLOAT:
    case Instruction::MUL_FLOAT:
    case Instruction::DIV_FLOAT:
    case Instruction::REM_FLOAT:
    case Instruction::ADD_FLOAT_2ADDR:
    case Instruction::SUB_FLOAT_2ADDR:
    case Instruction::MUL_FLOAT_2ADDR:
    case Instruction::DIV_FLOAT_2ADDR:
    case Instruction::REM_FLOAT_2ADDR: {
        // res = op + 2 operands
        uint16_t operand1 = GetOperandValue(mir->ssa_rep->uses[0]);
        uint16_t operand2 = GetOperandValue(mir->ssa_rep->uses[1]);
        res = gvn_->LookupValue(opcode, operand1, operand2, kNoValue);
        SetOperandValue(mir->ssa_rep->defs[0], res);
      }
      break;

    case Instruction::RSUB_INT:
    case Instruction::ADD_INT_LIT16:
    case Instruction::MUL_INT_LIT16:
    case Instruction::DIV_INT_LIT16:
    case Instruction::REM_INT_LIT16:
    case Instruction::AND_INT_LIT16:
    case Instruction::OR_INT_LIT16:
    case Instruction::XOR_INT_LIT16:
    case Instruction::ADD_INT_LIT8:
    case Instruction::RSUB_INT_LIT8:
    case Instruction::MUL_INT_LIT8:
    case Instruction::DIV_INT_LIT8:
    case Instruction::REM_INT_LIT8:
    case Instruction::AND_INT_LIT8:
    case Instruction::OR_INT_LIT8:
    case Instruction::XOR_INT_LIT8:
    case Instruction::SHL_INT_LIT8:
    case Instruction::SHR_INT_LIT8:
    case Instruction::USHR_INT_LIT8: {
        // Same as res = op + 2 operands, except use vC as operand 2
        uint16_t operand1 = GetOperandValue(mir->ssa_rep->uses[0]);
        uint16_t operand2 = gvn_->LookupValue(Instruction::CONST, mir->dalvikInsn.vC, 0, 0);
        res = gvn_->LookupValue(opcode, operand1, operand2, kNoValue);
        SetOperandValue(mir->ssa_rep->defs[0], res);
      }
      break;

    case Instruction::AGET_OBJECT:
    case Instruction::AGET:
    case Instruction::AGET_WIDE:
    case Instruction::AGET_BOOLEAN:
    case Instruction::AGET_BYTE:
    case Instruction::AGET_CHAR:
    case Instruction::AGET_SHORT:
      res = HandleAGet(mir, opcode);
      break;

    case Instruction::APUT_OBJECT:
      HandlePutObject(mir);
      // Intentional fall-through.
    case Instruction::APUT:
    case Instruction::APUT_WIDE:
    case Instruction::APUT_BYTE:
    case Instruction::APUT_BOOLEAN:
    case Instruction::APUT_SHORT:
    case Instruction::APUT_CHAR:
      HandleAPut(mir, opcode);
      break;

    case Instruction::IGET_OBJECT:
    case Instruction::IGET:
    case Instruction::IGET_WIDE:
    case Instruction::IGET_BOOLEAN:
    case Instruction::IGET_BYTE:
    case Instruction::IGET_CHAR:
    case Instruction::IGET_SHORT:
      res = HandleIGet(mir, opcode);
      break;

    case Instruction::IPUT_OBJECT:
      HandlePutObject(mir);
      // Intentional fall-through.
    case Instruction::IPUT:
    case Instruction::IPUT_WIDE:
    case Instruction::IPUT_BOOLEAN:
    case Instruction::IPUT_BYTE:
    case Instruction::IPUT_CHAR:
    case Instruction::IPUT_SHORT:
      HandleIPut(mir, opcode);
      break;

    case Instruction::SGET_OBJECT:
    case Instruction::SGET:
    case Instruction::SGET_WIDE:
    case Instruction::SGET_BOOLEAN:
    case Instruction::SGET_BYTE:
    case Instruction::SGET_CHAR:
    case Instruction::SGET_SHORT:
      res = HandleSGet(mir, opcode);
      break;

    case Instruction::SPUT_OBJECT:
      HandlePutObject(mir);
      // Intentional fall-through.
    case Instruction::SPUT:
    case Instruction::SPUT_WIDE:
    case Instruction::SPUT_BOOLEAN:
    case Instruction::SPUT_BYTE:
    case Instruction::SPUT_CHAR:
    case Instruction::SPUT_SHORT:
      HandleSPut(mir, opcode);
      break;
  }
  return res;
}

}    // namespace art
