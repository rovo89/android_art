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

#include "global_value_numbering.h"

#include "local_value_numbering.h"

namespace art {

GlobalValueNumbering::GlobalValueNumbering(CompilationUnit* cu, ScopedArenaAllocator* allocator)
    : cu_(cu),
      mir_graph_(cu->mir_graph.get()),
      allocator_(allocator),
      bbs_processed_(0u),
      max_bbs_to_process_(kMaxBbsToProcessMultiplyFactor * mir_graph_->GetNumReachableBlocks()),
      last_value_(0u),
      modifications_allowed_(false),
      global_value_map_(std::less<uint64_t>(), allocator->Adapter()),
      field_index_map_(FieldReferenceComparator(), allocator->Adapter()),
      field_index_reverse_map_(allocator->Adapter()),
      array_location_map_(ArrayLocationComparator(), allocator->Adapter()),
      array_location_reverse_map_(allocator->Adapter()),
      ref_set_map_(std::less<ValueNameSet>(), allocator->Adapter()),
      lvns_(mir_graph_->GetNumBlocks(), nullptr, allocator->Adapter()),
      work_lvn_(nullptr),
      merge_lvns_(allocator->Adapter()) {
}

GlobalValueNumbering::~GlobalValueNumbering() {
  STLDeleteElements(&lvns_);
}

LocalValueNumbering* GlobalValueNumbering::PrepareBasicBlock(BasicBlock* bb,
                                                             ScopedArenaAllocator* allocator) {
  if (UNLIKELY(!Good())) {
    return nullptr;
  }
  if (UNLIKELY(bb->data_flow_info == nullptr)) {
    return nullptr;
  }
  if (UNLIKELY(bb->block_type == kExitBlock)) {
    DCHECK(bb->first_mir_insn == nullptr);
    return nullptr;
  }
  if (UNLIKELY(bbs_processed_ == max_bbs_to_process_)) {
    last_value_ = kNoValue;  // Make bad.
    return nullptr;
  }
  if (allocator == nullptr) {
    allocator = allocator_;
  }
  DCHECK(work_lvn_.get() == nullptr);
  work_lvn_.reset(new (allocator) LocalValueNumbering(this, bb->id, allocator));
  if (bb->block_type == kEntryBlock) {
    if ((cu_->access_flags & kAccStatic) == 0) {
      // If non-static method, mark "this" as non-null
      int this_reg = cu_->num_dalvik_registers - cu_->num_ins;
      uint16_t value_name = work_lvn_->GetSRegValueName(this_reg);
      work_lvn_->SetValueNameNullChecked(value_name);
    }
  } else {
    // To avoid repeated allocation on the ArenaStack, reuse a single vector kept as a member.
    DCHECK(merge_lvns_.empty());
    // If we're running the full GVN, the RepeatingTopologicalSortIterator keeps the loop
    // head stack in the MIRGraph up to date and for a loop head we need to check whether
    // we're making the initial computation and need to merge only preceding blocks in the
    // topological order, or we're recalculating a loop head and need to merge all incoming
    // LVNs. When we're not at a loop head (including having an empty loop head stack) all
    // predecessors should be preceding blocks and we shall merge all of them anyway.
    //
    // If we're running the modification phase of the full GVN, the loop head stack will be
    // empty and we need to merge all incoming LVNs. If we're running just a simple LVN,
    // the loop head stack will also be empty and there will be nothing to merge anyway.
    bool use_all_predecessors = true;
    uint16_t loop_head_idx = 0u;  // Used only if !use_all_predecessors.
    if (mir_graph_->GetTopologicalSortOrderLoopHeadStack()->Size() != 0) {
      // Full GVN inside a loop, see if we're at the loop head for the first time.
      auto top = mir_graph_->GetTopologicalSortOrderLoopHeadStack()->Peek();
      loop_head_idx = top.first;
      bool recalculating = top.second;
      use_all_predecessors = recalculating ||
          loop_head_idx != mir_graph_->GetTopologicalSortOrderIndexes()->Get(bb->id);
    }
    GrowableArray<BasicBlockId>::Iterator iter(bb->predecessors);
    for (BasicBlock* pred_bb = mir_graph_->GetBasicBlock(iter.Next());
         pred_bb != nullptr; pred_bb = mir_graph_->GetBasicBlock(iter.Next())) {
      if (lvns_[pred_bb->id] != nullptr &&
          (use_all_predecessors ||
              mir_graph_->GetTopologicalSortOrderIndexes()->Get(pred_bb->id) < loop_head_idx)) {
        merge_lvns_.push_back(lvns_[pred_bb->id]);
      }
    }
    // Determine merge type.
    LocalValueNumbering::MergeType merge_type = LocalValueNumbering::kNormalMerge;
    if (bb->catch_entry) {
      merge_type = LocalValueNumbering::kCatchMerge;
    } else if (bb->last_mir_insn != nullptr &&
        (bb->last_mir_insn->dalvikInsn.opcode == Instruction::RETURN_VOID ||
         bb->last_mir_insn->dalvikInsn.opcode == Instruction::RETURN ||
         bb->last_mir_insn->dalvikInsn.opcode == Instruction::RETURN_OBJECT ||
         bb->last_mir_insn->dalvikInsn.opcode == Instruction::RETURN_WIDE) &&
        (bb->first_mir_insn == bb->last_mir_insn ||
         (static_cast<int>(bb->first_mir_insn->dalvikInsn.opcode) == kMirOpPhi &&
          (bb->first_mir_insn->next == bb->last_mir_insn ||
           (static_cast<int>(bb->first_mir_insn->next->dalvikInsn.opcode) == kMirOpPhi &&
            bb->first_mir_insn->next->next == bb->last_mir_insn))))) {
      merge_type = LocalValueNumbering::kReturnMerge;
    }
    // At least one predecessor must have been processed before this bb.
    CHECK(!merge_lvns_.empty());
    if (merge_lvns_.size() == 1u) {
      work_lvn_->MergeOne(*merge_lvns_[0], merge_type);
      BasicBlock* pred_bb = mir_graph_->GetBasicBlock(merge_lvns_[0]->Id());
      if (HasNullCheckLastInsn(pred_bb, bb->id)) {
        int s_reg = pred_bb->last_mir_insn->ssa_rep->uses[0];
        uint16_t value_name = merge_lvns_[0]->GetSRegValueName(s_reg);
        work_lvn_->SetValueNameNullChecked(value_name);
      }
    } else {
      work_lvn_->Merge(merge_type);
    }
  }
  return work_lvn_.get();
}

bool GlobalValueNumbering::FinishBasicBlock(BasicBlock* bb) {
  DCHECK(work_lvn_ != nullptr);
  DCHECK_EQ(bb->id, work_lvn_->Id());
  ++bbs_processed_;
  merge_lvns_.clear();

  bool change = (lvns_[bb->id] == nullptr) || !lvns_[bb->id]->Equals(*work_lvn_);
  if (change) {
    std::unique_ptr<const LocalValueNumbering> old_lvn(lvns_[bb->id]);
    lvns_[bb->id] = work_lvn_.release();
  } else {
    work_lvn_.reset();
  }
  return change;
}

uint16_t GlobalValueNumbering::GetFieldId(const MirFieldInfo& field_info, uint16_t type) {
  FieldReference key = { field_info.DeclaringDexFile(), field_info.DeclaringFieldIndex(), type };
  auto lb = field_index_map_.lower_bound(key);
  if (lb != field_index_map_.end() && !field_index_map_.key_comp()(key, lb->first)) {
    return lb->second;
  }
  DCHECK_LT(field_index_map_.size(), kNoValue);
  uint16_t id = field_index_map_.size();
  auto it = field_index_map_.PutBefore(lb, key, id);
  field_index_reverse_map_.push_back(&*it);
  return id;
}

uint16_t GlobalValueNumbering::GetArrayLocation(uint16_t base, uint16_t index) {
  auto cmp = array_location_map_.key_comp();
  ArrayLocation key = { base, index };
  auto lb = array_location_map_.lower_bound(key);
  if (lb != array_location_map_.end() && !cmp(key, lb->first)) {
    return lb->second;
  }
  uint16_t location = static_cast<uint16_t>(array_location_reverse_map_.size());
  DCHECK_EQ(location, array_location_reverse_map_.size());  // No overflow.
  auto it = array_location_map_.PutBefore(lb, key, location);
  array_location_reverse_map_.push_back(&*it);
  return location;
}

bool GlobalValueNumbering::HasNullCheckLastInsn(const BasicBlock* pred_bb,
                                                BasicBlockId succ_id) {
  if (pred_bb->block_type != kDalvikByteCode || pred_bb->last_mir_insn == nullptr) {
    return false;
  }
  Instruction::Code last_opcode = pred_bb->last_mir_insn->dalvikInsn.opcode;
  return ((last_opcode == Instruction::IF_EQZ && pred_bb->fall_through == succ_id) ||
      (last_opcode == Instruction::IF_NEZ && pred_bb->taken == succ_id));
}

bool GlobalValueNumbering::NullCheckedInAllPredecessors(
    const ScopedArenaVector<uint16_t>& merge_names) const {
  // Implicit parameters:
  //   - *work_lvn: the LVN for which we're checking predecessors.
  //   - merge_lvns_: the predecessor LVNs.
  DCHECK_EQ(merge_lvns_.size(), merge_names.size());
  for (size_t i = 0, size = merge_lvns_.size(); i != size; ++i) {
    const LocalValueNumbering* pred_lvn = merge_lvns_[i];
    uint16_t value_name = merge_names[i];
    if (!pred_lvn->IsValueNullChecked(value_name)) {
      // Check if the predecessor has an IF_EQZ/IF_NEZ as the last insn.
      const BasicBlock* pred_bb = mir_graph_->GetBasicBlock(pred_lvn->Id());
      if (!HasNullCheckLastInsn(pred_bb, work_lvn_->Id())) {
        return false;
      }
      // IF_EQZ/IF_NEZ checks some sreg, see if that sreg contains the value_name.
      int s_reg = pred_bb->last_mir_insn->ssa_rep->uses[0];
      if (!pred_lvn->IsSregValue(s_reg, value_name)) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace art
