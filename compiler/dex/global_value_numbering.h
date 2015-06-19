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

#ifndef ART_COMPILER_DEX_GLOBAL_VALUE_NUMBERING_H_
#define ART_COMPILER_DEX_GLOBAL_VALUE_NUMBERING_H_

#include "base/arena_object.h"
#include "base/logging.h"
#include "base/macros.h"
#include "mir_graph.h"
#include "compiler_ir.h"
#include "dex_flags.h"

namespace art {

class LocalValueNumbering;
class MirFieldInfo;

class GlobalValueNumbering : public DeletableArenaObject<kArenaAllocMisc> {
 public:
  static constexpr uint16_t kNoValue = 0xffffu;
  static constexpr uint16_t kNullValue = 1u;

  enum Mode {
    kModeGvn,
    kModeGvnPostProcessing,
    kModeLvn
  };

  static bool Skip(CompilationUnit* cu) {
    return (cu->disable_opt & (1u << kGlobalValueNumbering)) != 0u ||
        cu->mir_graph->GetMaxNestedLoops() > kMaxAllowedNestedLoops;
  }

  // Instance and static field id map is held by MIRGraph to avoid multiple recalculations
  // when doing LVN.
  template <typename Container>  // Container of MirIFieldLoweringInfo or MirSFieldLoweringInfo.
  static uint16_t* PrepareGvnFieldIds(ScopedArenaAllocator* allocator,
                                      const Container& field_infos);

  GlobalValueNumbering(CompilationUnit* cu, ScopedArenaAllocator* allocator, Mode mode);
  ~GlobalValueNumbering();

  CompilationUnit* GetCompilationUnit() const {
    return cu_;
  }

  MIRGraph* GetMirGraph() const {
    return mir_graph_;
  }

  // Prepare LVN for the basic block.
  LocalValueNumbering* PrepareBasicBlock(BasicBlock* bb,
                                         ScopedArenaAllocator* allocator = nullptr);

  // Finish processing the basic block.
  bool FinishBasicBlock(BasicBlock* bb);

  // Checks that the value names didn't overflow.
  bool Good() const {
    return last_value_ < kNoValue;
  }

  // Allow modifications.
  void StartPostProcessing();

  bool CanModify() const {
    return modifications_allowed_ && Good();
  }

  // Retrieve the LVN with GVN results for a given BasicBlock.
  const LocalValueNumbering* GetLvn(BasicBlockId bb_id) const;

 private:
  // Allocate a new value name.
  uint16_t NewValueName();

  // Key is concatenation of opcode, operand1, operand2 and modifier, value is value name.
  typedef ScopedArenaSafeMap<uint64_t, uint16_t> ValueMap;

  static uint64_t BuildKey(uint16_t op, uint16_t operand1, uint16_t operand2, uint16_t modifier) {
    return (static_cast<uint64_t>(op) << 48 | static_cast<uint64_t>(operand1) << 32 |
            static_cast<uint64_t>(operand2) << 16 | static_cast<uint64_t>(modifier));
  }

  // Look up a value in the global value map, adding a new entry if there was none before.
  uint16_t LookupValue(uint16_t op, uint16_t operand1, uint16_t operand2, uint16_t modifier) {
    uint16_t res;
    uint64_t key = BuildKey(op, operand1, operand2, modifier);
    auto lb = global_value_map_.lower_bound(key);
    if (lb != global_value_map_.end() && lb->first == key) {
      res = lb->second;
    } else {
      res = NewValueName();
      global_value_map_.PutBefore(lb, key, res);
    }
    return res;
  }

  // Look up a value in the global value map, don't add a new entry if there was none before.
  uint16_t FindValue(uint16_t op, uint16_t operand1, uint16_t operand2, uint16_t modifier) const {
    uint16_t res;
    uint64_t key = BuildKey(op, operand1, operand2, modifier);
    auto lb = global_value_map_.lower_bound(key);
    if (lb != global_value_map_.end() && lb->first == key) {
      res = lb->second;
    } else {
      res = kNoValue;
    }
    return res;
  }

  // Get an instance field id.
  uint16_t GetIFieldId(MIR* mir) {
    return GetMirGraph()->GetGvnIFieldId(mir);
  }

  // Get a static field id.
  uint16_t GetSFieldId(MIR* mir) {
    return GetMirGraph()->GetGvnSFieldId(mir);
  }

  // Get an instance field type based on field id.
  uint16_t GetIFieldType(uint16_t field_id) {
    return static_cast<uint16_t>(GetMirGraph()->GetIFieldLoweringInfo(field_id).MemAccessType());
  }

  // Get a static field type based on field id.
  uint16_t GetSFieldType(uint16_t field_id) {
    return static_cast<uint16_t>(GetMirGraph()->GetSFieldLoweringInfo(field_id).MemAccessType());
  }

  struct ArrayLocation {
    uint16_t base;
    uint16_t index;
  };

  struct ArrayLocationComparator {
    bool operator()(const ArrayLocation& lhs, const ArrayLocation& rhs) const {
      if (lhs.base != rhs.base) {
        return lhs.base < rhs.base;
      }
      return lhs.index < rhs.index;
    }
  };

  typedef ScopedArenaSafeMap<ArrayLocation, uint16_t, ArrayLocationComparator> ArrayLocationMap;

  // Get an array location.
  uint16_t GetArrayLocation(uint16_t base, uint16_t index);

  // Get the array base from an array location.
  uint16_t GetArrayLocationBase(uint16_t location) const {
    return array_location_reverse_map_[location]->first.base;
  }

  // Get the array index from an array location.
  uint16_t GetArrayLocationIndex(uint16_t location) const {
    return array_location_reverse_map_[location]->first.index;
  }

  // A set of value names.
  typedef ScopedArenaSet<uint16_t> ValueNameSet;

  // A map from a set of references to the set id.
  typedef ScopedArenaSafeMap<ValueNameSet, uint16_t> RefSetIdMap;

  uint16_t GetRefSetId(const ValueNameSet& ref_set) {
    uint16_t res = kNoValue;
    auto lb = ref_set_map_.lower_bound(ref_set);
    if (lb != ref_set_map_.end() && !ref_set_map_.key_comp()(ref_set, lb->first)) {
      res = lb->second;
    } else {
      res = NewValueName();
      ref_set_map_.PutBefore(lb, ref_set, res);
    }
    return res;
  }

  const BasicBlock* GetBasicBlock(uint16_t bb_id) const {
    return mir_graph_->GetBasicBlock(bb_id);
  }

  static bool HasNullCheckLastInsn(const BasicBlock* pred_bb, BasicBlockId succ_id) {
    return pred_bb->BranchesToSuccessorOnlyIfNotZero(succ_id);
  }

  bool NullCheckedInAllPredecessors(const ScopedArenaVector<uint16_t>& merge_names) const;

  bool DivZeroCheckedInAllPredecessors(const ScopedArenaVector<uint16_t>& merge_names) const;

  bool IsBlockEnteredOnTrue(uint16_t cond, BasicBlockId bb_id);
  bool IsTrueInBlock(uint16_t cond, BasicBlockId bb_id);

  ScopedArenaAllocator* Allocator() const {
    return allocator_;
  }

  CompilationUnit* const cu_;
  MIRGraph* const mir_graph_;
  ScopedArenaAllocator* const allocator_;

  // The maximum number of nested loops that we accept for GVN.
  static constexpr size_t kMaxAllowedNestedLoops = 6u;

  // The number of BBs that we need to process grows exponentially with the number
  // of nested loops. Don't allow excessive processing for too many nested loops or
  // otherwise expensive methods.
  static constexpr uint32_t kMaxBbsToProcessMultiplyFactor = 20u;

  uint32_t bbs_processed_;
  uint32_t max_bbs_to_process_;  // Doesn't apply after the main GVN has converged.

  // We have 32-bit last_value_ so that we can detect when we run out of value names, see Good().
  // We usually don't check Good() until the end of LVN unless we're about to modify code.
  uint32_t last_value_;

  // Marks whether code modifications are allowed. The initial GVN is done without code
  // modifications to settle the value names. Afterwards, we allow modifications and rerun
  // LVN once for each BasicBlock.
  bool modifications_allowed_;

  // Specifies the mode of operation.
  Mode mode_;

  ValueMap global_value_map_;
  ArrayLocationMap array_location_map_;
  ScopedArenaVector<const ArrayLocationMap::value_type*> array_location_reverse_map_;
  RefSetIdMap ref_set_map_;

  ScopedArenaVector<const LocalValueNumbering*> lvns_;        // Owning.
  std::unique_ptr<LocalValueNumbering> work_lvn_;
  ScopedArenaVector<const LocalValueNumbering*> merge_lvns_;  // Not owning.

  friend class LocalValueNumbering;
  friend class GlobalValueNumberingTest;

  DISALLOW_COPY_AND_ASSIGN(GlobalValueNumbering);
};
std::ostream& operator<<(std::ostream& os, const GlobalValueNumbering::Mode& rhs);

inline const LocalValueNumbering* GlobalValueNumbering::GetLvn(BasicBlockId bb_id) const {
  DCHECK_EQ(mode_, kModeGvnPostProcessing);
  DCHECK_LT(bb_id, lvns_.size());
  DCHECK(lvns_[bb_id] != nullptr);
  return lvns_[bb_id];
}

inline void GlobalValueNumbering::StartPostProcessing() {
  DCHECK(Good());
  DCHECK_EQ(mode_, kModeGvn);
  mode_ = kModeGvnPostProcessing;
}

inline uint16_t GlobalValueNumbering::NewValueName() {
  DCHECK_NE(mode_, kModeGvnPostProcessing);
  ++last_value_;
  return last_value_;
}

template <typename Container>  // Container of MirIFieldLoweringInfo or MirSFieldLoweringInfo.
uint16_t* GlobalValueNumbering::PrepareGvnFieldIds(ScopedArenaAllocator* allocator,
                                                   const Container& field_infos) {
  size_t size = field_infos.size();
  uint16_t* field_ids = allocator->AllocArray<uint16_t>(size, kArenaAllocMisc);
  for (size_t i = 0u; i != size; ++i) {
    size_t idx = i;
    const MirFieldInfo& cur_info = field_infos[i];
    if (cur_info.IsResolved()) {
      for (size_t j = 0; j != i; ++j) {
        const MirFieldInfo& prev_info = field_infos[j];
        if (prev_info.IsResolved() &&
            prev_info.DeclaringDexFile() == cur_info.DeclaringDexFile() &&
            prev_info.DeclaringFieldIndex() == cur_info.DeclaringFieldIndex()) {
          DCHECK_EQ(cur_info.MemAccessType(), prev_info.MemAccessType());
          idx = j;
          break;
        }
      }
    }
    field_ids[i] = idx;
  }
  return field_ids;
}

}  // namespace art

#endif  // ART_COMPILER_DEX_GLOBAL_VALUE_NUMBERING_H_
