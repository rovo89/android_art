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

#include "type_inference.h"

#include "base/bit_vector-inl.h"
#include "compiler_ir.h"
#include "dataflow_iterator-inl.h"
#include "dex_flags.h"
#include "dex_file-inl.h"
#include "driver/dex_compilation_unit.h"
#include "mir_field_info.h"
#include "mir_graph.h"
#include "mir_method_info.h"
#include "utils.h"

namespace art {

inline TypeInference::Type TypeInference::Type::ArrayType(uint32_t array_depth, Type nested_type) {
  DCHECK_NE(array_depth, 0u);
  return Type(kFlagNarrow | kFlagRef | kFlagLowWord | (array_depth << kBitArrayDepthStart) |
              ((nested_type.raw_bits_ & kMaskWideAndType) << kArrayTypeShift));
}

inline TypeInference::Type TypeInference::Type::ArrayTypeFromComponent(Type component_type) {
  if (component_type.ArrayDepth() == 0u) {
    return ArrayType(1u, component_type);
  }
  if (UNLIKELY(component_type.ArrayDepth() == kMaxArrayDepth)) {
    return component_type;
  }
  return Type(component_type.raw_bits_ + (1u << kBitArrayDepthStart));  // array_depth + 1u;
}

TypeInference::Type TypeInference::Type::ShortyType(char shorty) {
  switch (shorty) {
    case 'L':
      return Type(kFlagLowWord | kFlagNarrow | kFlagRef);
    case 'D':
      return Type(kFlagLowWord | kFlagWide | kFlagFp);
    case 'J':
      return Type(kFlagLowWord | kFlagWide | kFlagCore);
    case 'F':
      return Type(kFlagLowWord | kFlagNarrow | kFlagFp);
    default:
      DCHECK(shorty == 'I' || shorty == 'S' || shorty == 'C' || shorty == 'B' || shorty == 'Z');
      return Type(kFlagLowWord | kFlagNarrow | kFlagCore);
  }
}

TypeInference::Type TypeInference::Type::DexType(const DexFile* dex_file, uint32_t type_idx) {
  const char* desc = dex_file->GetTypeDescriptor(dex_file->GetTypeId(type_idx));
  if (UNLIKELY(desc[0] == 'V')) {
    return Unknown();
  } else if (UNLIKELY(desc[0] == '[')) {
    size_t array_depth = 0u;
    while (*desc == '[') {
      ++array_depth;
      ++desc;
    }
    if (UNLIKELY(array_depth > kMaxArrayDepth)) {
      LOG(WARNING) << "Array depth exceeds " << kMaxArrayDepth << ": " << array_depth
          << " in dex file " << dex_file->GetLocation() << " type index " << type_idx;
      array_depth = kMaxArrayDepth;
    }
    Type shorty_result = Type::ShortyType(desc[0]);
    return ArrayType(array_depth, shorty_result);
  } else {
    return ShortyType(desc[0]);
  }
}

bool TypeInference::Type::MergeArrayConflict(Type src_type) {
  DCHECK(Ref());
  DCHECK_NE(ArrayDepth(), src_type.ArrayDepth());
  DCHECK_GE(std::min(ArrayDepth(), src_type.ArrayDepth()), 1u);
  bool size_conflict =
      (ArrayDepth() == 1u && (raw_bits_ & kFlagArrayWide) != 0u) ||
      (src_type.ArrayDepth() == 1u && (src_type.raw_bits_ & kFlagArrayWide) != 0u);
  // Mark all three array type bits so that merging any other type bits will not change this type.
  return Copy(Type((raw_bits_ & kMaskNonArray) |
                   (1u << kBitArrayDepthStart) | kFlagArrayCore | kFlagArrayRef | kFlagArrayFp |
                   kFlagArrayNarrow | (size_conflict ? kFlagArrayWide : 0u)));
}

bool TypeInference::Type::MergeStrong(Type src_type) {
  bool changed = MergeNonArrayFlags(src_type);
  if (src_type.ArrayDepth() != 0u) {
    if (ArrayDepth() == 0u) {
      DCHECK_EQ(raw_bits_ & ~kMaskNonArray, 0u);
      DCHECK_NE(src_type.raw_bits_ & kFlagRef, 0u);
      raw_bits_ |= src_type.raw_bits_ & (~kMaskNonArray | kFlagRef);
      changed = true;
    } else if (ArrayDepth() == src_type.ArrayDepth()) {
      changed |= MergeBits(src_type, kMaskArrayWideAndType);
    } else if (src_type.ArrayDepth() == 1u &&
        (((src_type.raw_bits_ ^ UnknownArrayType().raw_bits_) & kMaskArrayWideAndType) == 0u ||
         ((src_type.raw_bits_ ^ ObjectArrayType().raw_bits_) & kMaskArrayWideAndType) == 0u)) {
      // Source type is [L or [? but current type is at least [[, preserve it.
    } else if (ArrayDepth() == 1u &&
        (((raw_bits_ ^ UnknownArrayType().raw_bits_) & kMaskArrayWideAndType) == 0u ||
         ((raw_bits_ ^ ObjectArrayType().raw_bits_) & kMaskArrayWideAndType) == 0u)) {
      // Overwrite [? or [L with the source array type which is at least [[.
      raw_bits_ = (raw_bits_ & kMaskNonArray) | (src_type.raw_bits_ & ~kMaskNonArray);
      changed = true;
    } else {
      // Mark the array value type with conflict - both ref and fp.
      changed |= MergeArrayConflict(src_type);
    }
  }
  return changed;
}

bool TypeInference::Type::MergeWeak(Type src_type) {
  bool changed = MergeNonArrayFlags(src_type);
  if (src_type.ArrayDepth() != 0u && src_type.NonNull()) {
    DCHECK_NE(src_type.ArrayDepth(), 0u);
    if (ArrayDepth() == 0u) {
      DCHECK_EQ(raw_bits_ & ~kMaskNonArray, 0u);
      // Preserve current type.
    } else if (ArrayDepth() == src_type.ArrayDepth()) {
      changed |= MergeBits(src_type, kMaskArrayWideAndType);
    } else if (src_type.ArrayDepth() == 1u &&
        (((src_type.raw_bits_ ^ UnknownArrayType().raw_bits_) & kMaskArrayWideAndType) == 0u ||
         ((src_type.raw_bits_ ^ ObjectArrayType().raw_bits_) & kMaskArrayWideAndType) == 0u)) {
      // Source type is [L or [? but current type is at least [[, preserve it.
    } else if (ArrayDepth() == 1u &&
        (((raw_bits_ ^ UnknownArrayType().raw_bits_) & kMaskArrayWideAndType) == 0u ||
         ((raw_bits_ ^ ObjectArrayType().raw_bits_) & kMaskArrayWideAndType) == 0u)) {
      // We have [? or [L. If it's [?, upgrade to [L as the source array type is at least [[.
      changed |= MergeBits(ObjectArrayType(), kMaskArrayWideAndType);
    } else {
      // Mark the array value type with conflict - both ref and fp.
      changed |= MergeArrayConflict(src_type);
    }
  }
  return changed;
}

TypeInference::CheckCastData::CheckCastData(MIRGraph* mir_graph, ScopedArenaAllocator* alloc)
    : mir_graph_(mir_graph),
      alloc_(alloc),
      num_blocks_(mir_graph->GetNumBlocks()),
      num_sregs_(mir_graph->GetNumSSARegs()),
      check_cast_map_(std::less<MIR*>(), alloc->Adapter()),
      split_sreg_data_(std::less<int32_t>(), alloc->Adapter()) {
}

void TypeInference::CheckCastData::AddCheckCast(MIR* check_cast, Type type) {
  DCHECK_EQ(check_cast->dalvikInsn.opcode, Instruction::CHECK_CAST);
  type.CheckPureRef();
  int32_t extra_s_reg = static_cast<int32_t>(num_sregs_);
  num_sregs_ += 1;
  check_cast_map_.Put(check_cast, CheckCastMapValue{extra_s_reg, type});  // NOLINT
  int32_t s_reg = check_cast->ssa_rep->uses[0];
  auto lb = split_sreg_data_.lower_bound(s_reg);
  if (lb == split_sreg_data_.end() || split_sreg_data_.key_comp()(s_reg, lb->first)) {
    SplitSRegData split_s_reg_data = {
        0,
        alloc_->AllocArray<int32_t>(num_blocks_, kArenaAllocMisc),
        alloc_->AllocArray<int32_t>(num_blocks_, kArenaAllocMisc),
        new (alloc_) ArenaBitVector(alloc_, num_blocks_, false)
    };
    std::fill_n(split_s_reg_data.starting_mod_s_reg, num_blocks_, INVALID_SREG);
    std::fill_n(split_s_reg_data.ending_mod_s_reg, num_blocks_, INVALID_SREG);
    split_s_reg_data.def_phi_blocks_->ClearAllBits();
    BasicBlock* def_bb = FindDefBlock(check_cast);
    split_s_reg_data.ending_mod_s_reg[def_bb->id] = s_reg;
    split_s_reg_data.def_phi_blocks_->SetBit(def_bb->id);
    lb = split_sreg_data_.PutBefore(lb, s_reg, split_s_reg_data);
  }
  lb->second.ending_mod_s_reg[check_cast->bb] = extra_s_reg;
  lb->second.def_phi_blocks_->SetBit(check_cast->bb);
}

void TypeInference::CheckCastData::AddPseudoPhis() {
  // Look for pseudo-phis where a split SSA reg merges with a differently typed version
  // and initialize all starting_mod_s_reg.
  DCHECK(!split_sreg_data_.empty());
  ArenaBitVector* phi_blocks = new (alloc_) ArenaBitVector(alloc_, num_blocks_, false);

  for (auto& entry : split_sreg_data_) {
    SplitSRegData& data = entry.second;

    // Find pseudo-phi nodes.
    phi_blocks->ClearAllBits();
    ArenaBitVector* input_blocks = data.def_phi_blocks_;
    do {
      for (uint32_t idx : input_blocks->Indexes()) {
        BasicBlock* def_bb = mir_graph_->GetBasicBlock(idx);
        if (def_bb->dom_frontier != nullptr) {
          phi_blocks->Union(def_bb->dom_frontier);
        }
      }
    } while (input_blocks->Union(phi_blocks));

    // Find live pseudo-phis. Make sure they're merging the same SSA reg.
    data.def_phi_blocks_->ClearAllBits();
    int32_t s_reg = entry.first;
    int v_reg = mir_graph_->SRegToVReg(s_reg);
    for (uint32_t phi_bb_id : phi_blocks->Indexes()) {
      BasicBlock* phi_bb = mir_graph_->GetBasicBlock(phi_bb_id);
      DCHECK(phi_bb != nullptr);
      DCHECK(phi_bb->data_flow_info != nullptr);
      DCHECK(phi_bb->data_flow_info->live_in_v != nullptr);
      if (IsSRegLiveAtStart(phi_bb, v_reg, s_reg)) {
        int32_t extra_s_reg = static_cast<int32_t>(num_sregs_);
        num_sregs_ += 1;
        data.starting_mod_s_reg[phi_bb_id] = extra_s_reg;
        data.def_phi_blocks_->SetBit(phi_bb_id);
      }
    }

    // SSA rename for s_reg.
    TopologicalSortIterator iter(mir_graph_);
    for (BasicBlock* bb = iter.Next(); bb != nullptr; bb = iter.Next()) {
      if (bb->data_flow_info == nullptr || bb->block_type == kEntryBlock) {
        continue;
      }
      BasicBlockId bb_id = bb->id;
      if (data.def_phi_blocks_->IsBitSet(bb_id)) {
        DCHECK_NE(data.starting_mod_s_reg[bb_id], INVALID_SREG);
      } else {
        DCHECK_EQ(data.starting_mod_s_reg[bb_id], INVALID_SREG);
        if (IsSRegLiveAtStart(bb, v_reg, s_reg)) {
          // The earliest predecessor must have been processed already.
          BasicBlock* pred_bb = FindTopologicallyEarliestPredecessor(bb);
          int32_t mod_s_reg = data.ending_mod_s_reg[pred_bb->id];
          data.starting_mod_s_reg[bb_id] = (mod_s_reg != INVALID_SREG) ? mod_s_reg : s_reg;
        } else if (data.ending_mod_s_reg[bb_id] != INVALID_SREG) {
          // Start the original defining block with s_reg.
          data.starting_mod_s_reg[bb_id] = s_reg;
        }
      }
      if (data.ending_mod_s_reg[bb_id] == INVALID_SREG) {
        // If the block doesn't define the modified SSA reg, it propagates the starting type.
        data.ending_mod_s_reg[bb_id] = data.starting_mod_s_reg[bb_id];
      }
    }
  }
}

void TypeInference::CheckCastData::InitializeCheckCastSRegs(Type* sregs) const {
  for (const auto& entry : check_cast_map_) {
    DCHECK_LT(static_cast<size_t>(entry.second.modified_s_reg), num_sregs_);
    sregs[entry.second.modified_s_reg] = entry.second.type.AsNonNull();
  }
}

void TypeInference::CheckCastData::MergeCheckCastConflicts(Type* sregs) const {
  for (const auto& entry : check_cast_map_) {
    DCHECK_LT(static_cast<size_t>(entry.second.modified_s_reg), num_sregs_);
    sregs[entry.first->ssa_rep->uses[0]].MergeNonArrayFlags(
        sregs[entry.second.modified_s_reg].AsNull());
  }
}

void TypeInference::CheckCastData::MarkPseudoPhiBlocks(uint64_t* bb_df_attrs) const {
  for (auto& entry : split_sreg_data_) {
    for (uint32_t bb_id : entry.second.def_phi_blocks_->Indexes()) {
      bb_df_attrs[bb_id] |= DF_NULL_TRANSFER_N;
    }
  }
}

void TypeInference::CheckCastData::Start(BasicBlock* bb) {
  for (auto& entry : split_sreg_data_) {
    entry.second.current_mod_s_reg = entry.second.starting_mod_s_reg[bb->id];
  }
}

bool TypeInference::CheckCastData::ProcessPseudoPhis(BasicBlock* bb, Type* sregs) {
  bool changed = false;
  for (auto& entry : split_sreg_data_) {
    DCHECK_EQ(entry.second.current_mod_s_reg, entry.second.starting_mod_s_reg[bb->id]);
    if (entry.second.def_phi_blocks_->IsBitSet(bb->id)) {
      int32_t* ending_mod_s_reg = entry.second.ending_mod_s_reg;
      Type merged_type = sregs[entry.second.current_mod_s_reg];
      for (BasicBlockId pred_id : bb->predecessors) {
        DCHECK_LT(static_cast<size_t>(ending_mod_s_reg[pred_id]), num_sregs_);
        merged_type.MergeWeak(sregs[ending_mod_s_reg[pred_id]]);
      }
      if (UNLIKELY(!merged_type.IsDefined())) {
        // This can happen during an initial merge of a loop head if the original def is
        // actually an untyped null. (All other definitions are typed using the check-cast.)
      } else if (merged_type.Wide()) {
        // Ignore the pseudo-phi, just remember that there's a size mismatch.
        sregs[entry.second.current_mod_s_reg].MarkSizeConflict();
      } else {
        DCHECK(merged_type.Narrow() && merged_type.LowWord() && !merged_type.HighWord());
        // Propagate both down (fully) and up (without the "non-null" flag).
        changed |= sregs[entry.second.current_mod_s_reg].Copy(merged_type);
        merged_type = merged_type.AsNull();
        for (BasicBlockId pred_id : bb->predecessors) {
          DCHECK_LT(static_cast<size_t>(ending_mod_s_reg[pred_id]), num_sregs_);
          sregs[ending_mod_s_reg[pred_id]].MergeStrong(merged_type);
        }
      }
    }
  }
  return changed;
}

void TypeInference::CheckCastData::ProcessCheckCast(MIR* mir) {
  auto mir_it = check_cast_map_.find(mir);
  DCHECK(mir_it != check_cast_map_.end());
  auto sreg_it = split_sreg_data_.find(mir->ssa_rep->uses[0]);
  DCHECK(sreg_it != split_sreg_data_.end());
  sreg_it->second.current_mod_s_reg = mir_it->second.modified_s_reg;
}

TypeInference::SplitSRegData* TypeInference::CheckCastData::GetSplitSRegData(int32_t s_reg) {
  auto it = split_sreg_data_.find(s_reg);
  return (it == split_sreg_data_.end()) ? nullptr : &it->second;
}

BasicBlock* TypeInference::CheckCastData::FindDefBlock(MIR* check_cast) {
  // Find the initial definition of the SSA reg used by the check-cast.
  DCHECK_EQ(check_cast->dalvikInsn.opcode, Instruction::CHECK_CAST);
  int32_t s_reg = check_cast->ssa_rep->uses[0];
  if (mir_graph_->IsInVReg(s_reg)) {
    return mir_graph_->GetEntryBlock();
  }
  int v_reg = mir_graph_->SRegToVReg(s_reg);
  BasicBlock* bb = mir_graph_->GetBasicBlock(check_cast->bb);
  DCHECK(bb != nullptr);
  while (true) {
    // Find the earliest predecessor in the topological sort order to ensure we don't
    // go in a loop.
    BasicBlock* pred_bb = FindTopologicallyEarliestPredecessor(bb);
    DCHECK(pred_bb != nullptr);
    DCHECK(pred_bb->data_flow_info != nullptr);
    DCHECK(pred_bb->data_flow_info->vreg_to_ssa_map_exit != nullptr);
    if (pred_bb->data_flow_info->vreg_to_ssa_map_exit[v_reg] != s_reg) {
      // The s_reg was not valid at the end of pred_bb, so it must have been defined in bb.
      return bb;
    }
    bb = pred_bb;
  }
}

BasicBlock* TypeInference::CheckCastData::FindTopologicallyEarliestPredecessor(BasicBlock* bb) {
  DCHECK(!bb->predecessors.empty());
  const auto& indexes = mir_graph_->GetTopologicalSortOrderIndexes();
  DCHECK_LT(bb->id, indexes.size());
  size_t best_idx = indexes[bb->id];
  BasicBlockId best_id = NullBasicBlockId;
  for (BasicBlockId pred_id : bb->predecessors) {
    DCHECK_LT(pred_id, indexes.size());
    if (best_idx > indexes[pred_id]) {
      best_idx = indexes[pred_id];
      best_id = pred_id;
    }
  }
  // There must be at least one predecessor earlier than the bb.
  DCHECK_LT(best_idx, indexes[bb->id]);
  return mir_graph_->GetBasicBlock(best_id);
}

bool TypeInference::CheckCastData::IsSRegLiveAtStart(BasicBlock* bb, int v_reg, int32_t s_reg) {
  DCHECK_EQ(v_reg, mir_graph_->SRegToVReg(s_reg));
  DCHECK(bb != nullptr);
  DCHECK(bb->data_flow_info != nullptr);
  DCHECK(bb->data_flow_info->live_in_v != nullptr);
  if (!bb->data_flow_info->live_in_v->IsBitSet(v_reg)) {
    return false;
  }
  for (BasicBlockId pred_id : bb->predecessors) {
    BasicBlock* pred_bb = mir_graph_->GetBasicBlock(pred_id);
    DCHECK(pred_bb != nullptr);
    DCHECK(pred_bb->data_flow_info != nullptr);
    DCHECK(pred_bb->data_flow_info->vreg_to_ssa_map_exit != nullptr);
    if (pred_bb->data_flow_info->vreg_to_ssa_map_exit[v_reg] != s_reg) {
      return false;
    }
  }
  return true;
}

TypeInference::TypeInference(MIRGraph* mir_graph, ScopedArenaAllocator* alloc)
    : mir_graph_(mir_graph),
      cu_(mir_graph->GetCurrentDexCompilationUnit()->GetCompilationUnit()),
      check_cast_data_(!mir_graph->HasCheckCast() ? nullptr :
          InitializeCheckCastData(mir_graph, alloc)),
      num_sregs_(
          check_cast_data_ != nullptr ? check_cast_data_->NumSRegs() : mir_graph->GetNumSSARegs()),
      ifields_(mir_graph->GetIFieldLoweringInfoCount() == 0u ? nullptr :
          PrepareIFieldTypes(cu_->dex_file, mir_graph, alloc)),
      sfields_(mir_graph->GetSFieldLoweringInfoCount() == 0u ? nullptr :
          PrepareSFieldTypes(cu_->dex_file, mir_graph, alloc)),
      signatures_(mir_graph->GetMethodLoweringInfoCount() == 0u ? nullptr :
          PrepareSignatures(cu_->dex_file, mir_graph, alloc)),
      current_method_signature_(
          Signature(cu_->dex_file, cu_->method_idx, (cu_->access_flags & kAccStatic) != 0, alloc)),
      sregs_(alloc->AllocArray<Type>(num_sregs_, kArenaAllocMisc)),
      bb_df_attrs_(alloc->AllocArray<uint64_t>(mir_graph->GetNumBlocks(), kArenaAllocDFInfo)) {
  InitializeSRegs();
}

bool TypeInference::Apply(BasicBlock* bb) {
  bool changed = false;
  uint64_t bb_df_attrs = bb_df_attrs_[bb->id];
  if (bb_df_attrs != 0u) {
    if (UNLIKELY(check_cast_data_ != nullptr)) {
      check_cast_data_->Start(bb);
      if (bb_df_attrs & DF_NULL_TRANSFER_N) {
        changed |= check_cast_data_->ProcessPseudoPhis(bb, sregs_);
      }
    }
    MIR* mir = bb->first_mir_insn;
    MIR* main_mirs_end = ((bb_df_attrs & DF_SAME_TYPE_AB) != 0u) ? bb->last_mir_insn : nullptr;
    for (; mir != main_mirs_end && static_cast<int>(mir->dalvikInsn.opcode) == kMirOpPhi;
        mir = mir->next) {
      // Special-case handling for Phi comes first because we have 2 Phis instead of a wide one.
      // At least one input must have been previously processed. Look for the first
      // occurrence of a high_word or low_word flag to determine the type.
      size_t num_uses = mir->ssa_rep->num_uses;
      const int32_t* uses = mir->ssa_rep->uses;
      const int32_t* defs = mir->ssa_rep->defs;
      DCHECK_EQ(bb->predecessors.size(), num_uses);
      Type merged_type = sregs_[defs[0]];
      for (size_t pred_idx = 0; pred_idx != num_uses; ++pred_idx) {
        int32_t input_mod_s_reg = PhiInputModifiedSReg(uses[pred_idx], bb, pred_idx);
        merged_type.MergeWeak(sregs_[input_mod_s_reg]);
      }
      if (UNLIKELY(!merged_type.IsDefined())) {
        // No change
      } else if (merged_type.HighWord()) {
        // Ignore the high word phi, just remember if there's a size mismatch.
        if (UNLIKELY(merged_type.LowWord())) {
          sregs_[defs[0]].MarkSizeConflict();
        }
      } else {
        // Propagate both down (fully) and up (without the "non-null" flag).
        changed |= sregs_[defs[0]].Copy(merged_type);
        merged_type = merged_type.AsNull();
        for (size_t pred_idx = 0; pred_idx != num_uses; ++pred_idx) {
          int32_t input_mod_s_reg = PhiInputModifiedSReg(uses[pred_idx], bb, pred_idx);
          changed |= UpdateSRegFromLowWordType(input_mod_s_reg, merged_type);
        }
      }
    }

    // Propagate types with MOVEs and AGETs, process CHECK_CASTs for modified SSA reg tracking.
    for (; mir != main_mirs_end; mir = mir->next) {
      uint64_t attrs = MIRGraph::GetDataFlowAttributes(mir);
      size_t num_uses = mir->ssa_rep->num_uses;
      const int32_t* uses = mir->ssa_rep->uses;
      const int32_t* defs = mir->ssa_rep->defs;

      // Special handling for moves. Propagate type both ways.
      if ((attrs & DF_IS_MOVE) != 0) {
        int32_t used_mod_s_reg = ModifiedSReg(uses[0]);
        int32_t defd_mod_s_reg = defs[0];

        // The "non-null" flag is propagated only downwards from actual definitions and it's
        // not initially marked for moves, so used sreg must be marked before defined sreg.
        // The only exception is an inlined move where we know the type from the original invoke.
        DCHECK(sregs_[used_mod_s_reg].NonNull() || !sregs_[defd_mod_s_reg].NonNull() ||
               (mir->optimization_flags & MIR_CALLEE) != 0);
        changed |= UpdateSRegFromLowWordType(used_mod_s_reg, sregs_[defd_mod_s_reg].AsNull());

        // The value is the same, so either both registers are null or no register is.
        // In any case we can safely propagate the array type down.
        changed |= UpdateSRegFromLowWordType(defd_mod_s_reg, sregs_[used_mod_s_reg]);
        if (UNLIKELY((attrs & DF_REF_A) == 0 && sregs_[used_mod_s_reg].Ref())) {
          // Mark type conflict: move instead of move-object.
          sregs_[used_mod_s_reg].MarkTypeConflict();
        }
        continue;
      }

      // Handle AGET/APUT.
      if ((attrs & DF_HAS_RANGE_CHKS) != 0) {
        int32_t base_mod_s_reg = ModifiedSReg(uses[num_uses - 2u]);
        int32_t mod_s_reg = (attrs & DF_DA) != 0 ? defs[0] : ModifiedSReg(uses[0]);
        DCHECK_NE(sregs_[base_mod_s_reg].ArrayDepth(), 0u);
        if (!sregs_[base_mod_s_reg].NonNull()) {
          // If the base is null, don't propagate anything. All that we could determine
          // has already been merged in the previous stage.
        } else {
          changed |= UpdateSRegFromLowWordType(mod_s_reg, sregs_[base_mod_s_reg].ComponentType());
          Type array_type = Type::ArrayTypeFromComponent(sregs_[mod_s_reg]);
          if ((attrs & DF_DA) != 0) {
            changed |= sregs_[base_mod_s_reg].MergeStrong(array_type);
          } else {
            changed |= sregs_[base_mod_s_reg].MergeWeak(array_type);
          }
        }
        if (UNLIKELY((attrs & DF_REF_A) == 0 && sregs_[mod_s_reg].Ref())) {
          // Mark type conflict: aget/aput instead of aget/aput-object.
          sregs_[mod_s_reg].MarkTypeConflict();
        }
        continue;
      }

      // Special-case handling for check-cast to advance modified SSA reg.
      if (UNLIKELY((attrs & DF_CHK_CAST) != 0)) {
        DCHECK(check_cast_data_ != nullptr);
        check_cast_data_->ProcessCheckCast(mir);
      }
    }

    // Propagate types for IF_cc if present.
    if (mir != nullptr) {
      DCHECK(mir == bb->last_mir_insn);
      DCHECK(mir->next == nullptr);
      DCHECK_NE(MIRGraph::GetDataFlowAttributes(mir) & DF_SAME_TYPE_AB, 0u);
      DCHECK_EQ(mir->ssa_rep->num_uses, 2u);
      const int32_t* uses = mir->ssa_rep->uses;
      int32_t mod_s_reg0 = ModifiedSReg(uses[0]);
      int32_t mod_s_reg1 = ModifiedSReg(uses[1]);
      changed |= sregs_[mod_s_reg0].MergeWeak(sregs_[mod_s_reg1].AsNull());
      changed |= sregs_[mod_s_reg1].MergeWeak(sregs_[mod_s_reg0].AsNull());
    }
  }
  return changed;
}

void TypeInference::Finish() {
  if (UNLIKELY(check_cast_data_ != nullptr)) {
    check_cast_data_->MergeCheckCastConflicts(sregs_);
  }

  size_t num_sregs = mir_graph_->GetNumSSARegs();  // Without the extra SSA regs.
  for (size_t s_reg = 0; s_reg != num_sregs; ++s_reg) {
    if (sregs_[s_reg].SizeConflict()) {
      /*
       * The dex bytecode definition does not explicitly outlaw the definition of the same
       * virtual register to be used in both a 32-bit and 64-bit pair context.  However, dx
       * does not generate this pattern (at least recently).  Further, in the next revision of
       * dex, we will forbid this.  To support the few cases in the wild, detect this pattern
       * and punt to the interpreter.
       */
      LOG(WARNING) << PrettyMethod(cu_->method_idx, *cu_->dex_file)
                   << " has size conflict block for sreg " << s_reg
                   << ", punting to interpreter.";
      mir_graph_->SetPuntToInterpreter(true);
      return;
    }
  }

  size_t conflict_s_reg = 0;
  bool type_conflict = false;
  for (size_t s_reg = 0; s_reg != num_sregs; ++s_reg) {
    Type type = sregs_[s_reg];
    RegLocation* loc = &mir_graph_->reg_location_[s_reg];
    loc->wide = type.Wide();
    loc->defined = type.IsDefined();
    loc->fp = type.Fp();
    loc->core = type.Core();
    loc->ref = type.Ref();
    loc->high_word = type.HighWord();
    if (UNLIKELY(type.TypeConflict())) {
      type_conflict = true;
      conflict_s_reg = s_reg;
    }
  }

  if (type_conflict) {
    /*
     * Each dalvik register definition should be used either as a reference, or an
     * integer or a floating point value. We don't normally expect to see a Dalvik
     * register definition used in two or three of these roles though technically it
     * could happen with constants (0 for all three roles, non-zero for integer and
     * FP). Detect this situation and disable optimizations that rely on correct
     * typing, i.e. register promotion, GVN/LVN and GVN-based DCE.
     */
    LOG(WARNING) << PrettyMethod(cu_->method_idx, *cu_->dex_file)
                 << " has type conflict block for sreg " << conflict_s_reg
                 << ", disabling register promotion.";
    cu_->disable_opt |=
        (1u << kPromoteRegs) |
        (1u << kGlobalValueNumbering) |
        (1u << kGvnDeadCodeElimination) |
        (1u << kLocalValueNumbering);
  }
}

TypeInference::Type TypeInference::FieldType(const DexFile* dex_file, uint32_t field_idx) {
  uint32_t type_idx = dex_file->GetFieldId(field_idx).type_idx_;
  Type result = Type::DexType(dex_file, type_idx);
  return result;
}

TypeInference::Type* TypeInference::PrepareIFieldTypes(const DexFile* dex_file,
                                                       MIRGraph* mir_graph,
                                                       ScopedArenaAllocator* alloc) {
  size_t count = mir_graph->GetIFieldLoweringInfoCount();
  Type* ifields = alloc->AllocArray<Type>(count, kArenaAllocDFInfo);
  for (uint32_t i = 0u; i != count; ++i) {
    // NOTE: Quickened field accesses have invalid FieldIndex() but they are always resolved.
    const MirFieldInfo& info = mir_graph->GetIFieldLoweringInfo(i);
    const DexFile* current_dex_file = info.IsResolved() ? info.DeclaringDexFile() : dex_file;
    uint32_t field_idx = info.IsResolved() ? info.DeclaringFieldIndex() : info.FieldIndex();
    ifields[i] = FieldType(current_dex_file, field_idx);
    DCHECK_EQ(info.MemAccessType() == kDexMemAccessWide, ifields[i].Wide());
    DCHECK_EQ(info.MemAccessType() == kDexMemAccessObject, ifields[i].Ref());
  }
  return ifields;
}

TypeInference::Type* TypeInference::PrepareSFieldTypes(const DexFile* dex_file,
                                                       MIRGraph* mir_graph,
                                                       ScopedArenaAllocator* alloc) {
  size_t count = mir_graph->GetSFieldLoweringInfoCount();
  Type* sfields = alloc->AllocArray<Type>(count, kArenaAllocDFInfo);
  for (uint32_t i = 0u; i != count; ++i) {
    // FieldIndex() is always valid for static fields (no quickened instructions).
    sfields[i] = FieldType(dex_file, mir_graph->GetSFieldLoweringInfo(i).FieldIndex());
  }
  return sfields;
}

TypeInference::MethodSignature TypeInference::Signature(const DexFile* dex_file,
                                                        uint32_t method_idx,
                                                        bool is_static,
                                                        ScopedArenaAllocator* alloc) {
  const DexFile::MethodId& method_id = dex_file->GetMethodId(method_idx);
  const DexFile::ProtoId& proto_id = dex_file->GetMethodPrototype(method_id);
  Type return_type = Type::DexType(dex_file, proto_id.return_type_idx_);
  const DexFile::TypeList* type_list = dex_file->GetProtoParameters(proto_id);
  size_t this_size = (is_static ? 0u : 1u);
  size_t param_size = ((type_list != nullptr) ? type_list->Size() : 0u);
  size_t size = this_size + param_size;
  Type* param_types = (size != 0u) ? alloc->AllocArray<Type>(size, kArenaAllocDFInfo) : nullptr;
  if (!is_static) {
    param_types[0] = Type::DexType(dex_file, method_id.class_idx_);
  }
  for (size_t i = 0; i != param_size; ++i)  {
    uint32_t type_idx = type_list->GetTypeItem(i).type_idx_;
    param_types[this_size + i] = Type::DexType(dex_file, type_idx);
  }
  return MethodSignature{ return_type, size, param_types };  // NOLINT
}

TypeInference::MethodSignature* TypeInference::PrepareSignatures(const DexFile* dex_file,
                                                                 MIRGraph* mir_graph,
                                                                 ScopedArenaAllocator* alloc) {
  size_t count = mir_graph->GetMethodLoweringInfoCount();
  MethodSignature* signatures = alloc->AllocArray<MethodSignature>(count, kArenaAllocDFInfo);
  for (uint32_t i = 0u; i != count; ++i) {
    // NOTE: Quickened invokes have invalid MethodIndex() but they are always resolved.
    const MirMethodInfo& info = mir_graph->GetMethodLoweringInfo(i);
    uint32_t method_idx = info.IsResolved() ? info.DeclaringMethodIndex() : info.MethodIndex();
    const DexFile* current_dex_file = info.IsResolved() ? info.DeclaringDexFile() : dex_file;
    signatures[i] = Signature(current_dex_file, method_idx, info.IsStatic(), alloc);
  }
  return signatures;
}

TypeInference::CheckCastData* TypeInference::InitializeCheckCastData(MIRGraph* mir_graph,
                                                                     ScopedArenaAllocator* alloc) {
  if (!mir_graph->HasCheckCast()) {
    return nullptr;
  }

  CheckCastData* data = nullptr;
  const DexFile* dex_file = nullptr;
  PreOrderDfsIterator iter(mir_graph);
  for (BasicBlock* bb = iter.Next(); bb != nullptr; bb = iter.Next()) {
    for (MIR* mir = bb->first_mir_insn; mir != nullptr; mir = mir->next) {
      if (mir->dalvikInsn.opcode == Instruction::CHECK_CAST) {
        if (data == nullptr) {
          data = new (alloc) CheckCastData(mir_graph, alloc);
          dex_file = mir_graph->GetCurrentDexCompilationUnit()->GetCompilationUnit()->dex_file;
        }
        Type type = Type::DexType(dex_file, mir->dalvikInsn.vB);
        data->AddCheckCast(mir, type);
      }
    }
  }
  if (data != nullptr) {
    data->AddPseudoPhis();
  }
  return data;
}

void TypeInference::InitializeSRegs() {
  std::fill_n(sregs_, num_sregs_, Type::Unknown());

  /* Treat ArtMethod* specially since they are pointer sized */
  sregs_[mir_graph_->GetMethodSReg()] = Type::ArtMethodType(cu_->target64);

  // Initialize parameter SSA regs at method entry.
  int32_t entry_param_s_reg = mir_graph_->GetFirstInVR();
  for (size_t i = 0, size = current_method_signature_.num_params; i != size; ++i)  {
    Type param_type = current_method_signature_.param_types[i].AsNonNull();
    sregs_[entry_param_s_reg] = param_type;
    entry_param_s_reg += param_type.Wide() ? 2 : 1;
  }
  DCHECK_EQ(static_cast<uint32_t>(entry_param_s_reg),
            mir_graph_->GetFirstInVR() + mir_graph_->GetNumOfInVRs());

  // Initialize check-cast types.
  if (UNLIKELY(check_cast_data_ != nullptr)) {
    check_cast_data_->InitializeCheckCastSRegs(sregs_);
  }

  // Initialize well-known SSA register definition types. Merge inferred types
  // upwards where a single merge is enough (INVOKE arguments and return type,
  // RETURN type, IPUT/SPUT source type).
  // NOTE: Using topological sort order to make sure the definition comes before
  // any upward merging. This allows simple assignment of the defined types
  // instead of MergeStrong().
  TopologicalSortIterator iter(mir_graph_);
  for (BasicBlock* bb = iter.Next(); bb != nullptr; bb = iter.Next()) {
    uint64_t bb_df_attrs = 0u;
    if (UNLIKELY(check_cast_data_ != nullptr)) {
      check_cast_data_->Start(bb);
    }
    // Ignore pseudo-phis, we're not setting types for SSA regs that depend on them in this pass.
    for (MIR* mir = bb->first_mir_insn; mir != nullptr; mir = mir->next) {
      uint64_t attrs = MIRGraph::GetDataFlowAttributes(mir);
      bb_df_attrs |= attrs;

      const uint32_t num_uses = mir->ssa_rep->num_uses;
      const int32_t* uses = mir->ssa_rep->uses;
      const int32_t* defs = mir->ssa_rep->defs;

      uint16_t opcode = mir->dalvikInsn.opcode;
      switch (opcode) {
        case Instruction::CONST_4:
        case Instruction::CONST_16:
        case Instruction::CONST:
        case Instruction::CONST_HIGH16:
        case Instruction::CONST_WIDE_16:
        case Instruction::CONST_WIDE_32:
        case Instruction::CONST_WIDE:
        case Instruction::CONST_WIDE_HIGH16:
        case Instruction::MOVE:
        case Instruction::MOVE_FROM16:
        case Instruction::MOVE_16:
        case Instruction::MOVE_WIDE:
        case Instruction::MOVE_WIDE_FROM16:
        case Instruction::MOVE_WIDE_16:
        case Instruction::MOVE_OBJECT:
        case Instruction::MOVE_OBJECT_FROM16:
        case Instruction::MOVE_OBJECT_16:
          if ((mir->optimization_flags & MIR_CALLEE) != 0) {
            // Inlined const/move keeps method_lowering_info for type inference.
            DCHECK_LT(mir->meta.method_lowering_info, mir_graph_->GetMethodLoweringInfoCount());
            Type return_type = signatures_[mir->meta.method_lowering_info].return_type;
            DCHECK(return_type.IsDefined());  // Method return type can't be void.
            sregs_[defs[0]] = return_type.AsNonNull();
            if (return_type.Wide()) {
              DCHECK_EQ(defs[0] + 1, defs[1]);
              sregs_[defs[1]] = return_type.ToHighWord();
            }
            break;
          }
          FALLTHROUGH_INTENDED;
        case kMirOpPhi:
          // These cannot be determined in this simple pass and will be processed later.
          break;

        case Instruction::MOVE_RESULT:
        case Instruction::MOVE_RESULT_WIDE:
        case Instruction::MOVE_RESULT_OBJECT:
          // Nothing to do, handled with invoke-* or filled-new-array/-range.
          break;
        case Instruction::MOVE_EXCEPTION:
          // NOTE: We can never catch an array.
          sregs_[defs[0]] = Type::NonArrayRefType().AsNonNull();
          break;
        case Instruction::CONST_STRING:
        case Instruction::CONST_STRING_JUMBO:
          sregs_[defs[0]] = Type::NonArrayRefType().AsNonNull();
          break;
        case Instruction::CONST_CLASS:
          sregs_[defs[0]] = Type::NonArrayRefType().AsNonNull();
          break;
        case Instruction::CHECK_CAST:
          DCHECK(check_cast_data_ != nullptr);
          check_cast_data_->ProcessCheckCast(mir);
          break;
        case Instruction::ARRAY_LENGTH:
          sregs_[ModifiedSReg(uses[0])].MergeStrong(Type::UnknownArrayType());
          break;
        case Instruction::NEW_INSTANCE:
          sregs_[defs[0]] = Type::DexType(cu_->dex_file, mir->dalvikInsn.vB).AsNonNull();
          DCHECK(sregs_[defs[0]].Ref());
          DCHECK_EQ(sregs_[defs[0]].ArrayDepth(), 0u);
          break;
        case Instruction::NEW_ARRAY:
          sregs_[defs[0]] = Type::DexType(cu_->dex_file, mir->dalvikInsn.vC).AsNonNull();
          DCHECK(sregs_[defs[0]].Ref());
          DCHECK_NE(sregs_[defs[0]].ArrayDepth(), 0u);
          break;
        case Instruction::FILLED_NEW_ARRAY:
        case Instruction::FILLED_NEW_ARRAY_RANGE: {
          Type array_type = Type::DexType(cu_->dex_file, mir->dalvikInsn.vB);
          array_type.CheckPureRef();  // Previously checked by the method verifier.
          DCHECK_NE(array_type.ArrayDepth(), 0u);
          Type component_type = array_type.ComponentType();
          DCHECK(!component_type.Wide());
          MIR* move_result_mir = mir_graph_->FindMoveResult(bb, mir);
          if (move_result_mir != nullptr) {
            DCHECK_EQ(move_result_mir->dalvikInsn.opcode, Instruction::MOVE_RESULT_OBJECT);
            sregs_[move_result_mir->ssa_rep->defs[0]] = array_type.AsNonNull();
          }
          DCHECK_EQ(num_uses, mir->dalvikInsn.vA);
          for (size_t next = 0u; next != num_uses; ++next) {
            int32_t input_mod_s_reg = ModifiedSReg(uses[next]);
            sregs_[input_mod_s_reg].MergeStrong(component_type);
          }
          break;
        }
        case Instruction::INVOKE_VIRTUAL:
        case Instruction::INVOKE_SUPER:
        case Instruction::INVOKE_DIRECT:
        case Instruction::INVOKE_STATIC:
        case Instruction::INVOKE_INTERFACE:
        case Instruction::INVOKE_VIRTUAL_RANGE:
        case Instruction::INVOKE_SUPER_RANGE:
        case Instruction::INVOKE_DIRECT_RANGE:
        case Instruction::INVOKE_STATIC_RANGE:
        case Instruction::INVOKE_INTERFACE_RANGE:
        case Instruction::INVOKE_VIRTUAL_QUICK:
        case Instruction::INVOKE_VIRTUAL_RANGE_QUICK: {
          const MethodSignature* signature = &signatures_[mir->meta.method_lowering_info];
          MIR* move_result_mir = mir_graph_->FindMoveResult(bb, mir);
          if (move_result_mir != nullptr) {
            Type return_type = signature->return_type;
            sregs_[move_result_mir->ssa_rep->defs[0]] = return_type.AsNonNull();
            if (return_type.Wide()) {
              DCHECK_EQ(move_result_mir->ssa_rep->defs[0] + 1, move_result_mir->ssa_rep->defs[1]);
              sregs_[move_result_mir->ssa_rep->defs[1]] = return_type.ToHighWord();
            }
          }
          size_t next = 0u;
          for (size_t i = 0, size = signature->num_params; i != size; ++i)  {
            Type param_type = signature->param_types[i];
            int32_t param_s_reg = ModifiedSReg(uses[next]);
            DCHECK(!param_type.Wide() || uses[next] + 1 == uses[next + 1]);
            UpdateSRegFromLowWordType(param_s_reg, param_type);
            next += param_type.Wide() ? 2 : 1;
          }
          DCHECK_EQ(next, num_uses);
          DCHECK_EQ(next, mir->dalvikInsn.vA);
          break;
        }

        case Instruction::RETURN_WIDE:
          DCHECK(current_method_signature_.return_type.Wide());
          DCHECK_EQ(uses[0] + 1, uses[1]);
          DCHECK_EQ(ModifiedSReg(uses[0]), uses[0]);
          FALLTHROUGH_INTENDED;
        case Instruction::RETURN:
        case Instruction::RETURN_OBJECT: {
          int32_t mod_s_reg = ModifiedSReg(uses[0]);
          UpdateSRegFromLowWordType(mod_s_reg, current_method_signature_.return_type);
          break;
        }

        // NOTE: For AGET/APUT we set only the array type. The operand type is set
        // below based on the data flow attributes.
        case Instruction::AGET:
        case Instruction::APUT:
          sregs_[ModifiedSReg(uses[num_uses - 2u])].MergeStrong(Type::NarrowArrayType());
          break;
        case Instruction::AGET_WIDE:
        case Instruction::APUT_WIDE:
          sregs_[ModifiedSReg(uses[num_uses - 2u])].MergeStrong(Type::WideArrayType());
          break;
        case Instruction::AGET_OBJECT:
          sregs_[defs[0]] = sregs_[defs[0]].AsNonNull();
          FALLTHROUGH_INTENDED;
        case Instruction::APUT_OBJECT:
          sregs_[ModifiedSReg(uses[num_uses - 2u])].MergeStrong(Type::ObjectArrayType());
          break;
        case Instruction::AGET_BOOLEAN:
        case Instruction::APUT_BOOLEAN:
        case Instruction::AGET_BYTE:
        case Instruction::APUT_BYTE:
        case Instruction::AGET_CHAR:
        case Instruction::APUT_CHAR:
        case Instruction::AGET_SHORT:
        case Instruction::APUT_SHORT:
          sregs_[ModifiedSReg(uses[num_uses - 2u])].MergeStrong(Type::NarrowCoreArrayType());
          break;

        case Instruction::IGET_WIDE:
        case Instruction::IGET_WIDE_QUICK:
          DCHECK_EQ(defs[0] + 1, defs[1]);
          DCHECK_LT(mir->meta.ifield_lowering_info, mir_graph_->GetIFieldLoweringInfoCount());
          sregs_[defs[1]] = ifields_[mir->meta.ifield_lowering_info].ToHighWord();
          FALLTHROUGH_INTENDED;
        case Instruction::IGET:
        case Instruction::IGET_OBJECT:
        case Instruction::IGET_BOOLEAN:
        case Instruction::IGET_BYTE:
        case Instruction::IGET_CHAR:
        case Instruction::IGET_SHORT:
        case Instruction::IGET_QUICK:
        case Instruction::IGET_OBJECT_QUICK:
        case Instruction::IGET_BOOLEAN_QUICK:
        case Instruction::IGET_BYTE_QUICK:
        case Instruction::IGET_CHAR_QUICK:
        case Instruction::IGET_SHORT_QUICK:
          DCHECK_LT(mir->meta.ifield_lowering_info, mir_graph_->GetIFieldLoweringInfoCount());
          sregs_[defs[0]] = ifields_[mir->meta.ifield_lowering_info].AsNonNull();
          break;
        case Instruction::IPUT_WIDE:
        case Instruction::IPUT_WIDE_QUICK:
          DCHECK_EQ(uses[0] + 1, uses[1]);
          FALLTHROUGH_INTENDED;
        case Instruction::IPUT:
        case Instruction::IPUT_OBJECT:
        case Instruction::IPUT_BOOLEAN:
        case Instruction::IPUT_BYTE:
        case Instruction::IPUT_CHAR:
        case Instruction::IPUT_SHORT:
        case Instruction::IPUT_QUICK:
        case Instruction::IPUT_OBJECT_QUICK:
        case Instruction::IPUT_BOOLEAN_QUICK:
        case Instruction::IPUT_BYTE_QUICK:
        case Instruction::IPUT_CHAR_QUICK:
        case Instruction::IPUT_SHORT_QUICK:
          DCHECK_LT(mir->meta.ifield_lowering_info, mir_graph_->GetIFieldLoweringInfoCount());
          UpdateSRegFromLowWordType(ModifiedSReg(uses[0]),
                                    ifields_[mir->meta.ifield_lowering_info]);
          break;
        case Instruction::SGET_WIDE:
          DCHECK_EQ(defs[0] + 1, defs[1]);
          DCHECK_LT(mir->meta.sfield_lowering_info, mir_graph_->GetSFieldLoweringInfoCount());
          sregs_[defs[1]] = sfields_[mir->meta.sfield_lowering_info].ToHighWord();
          FALLTHROUGH_INTENDED;
        case Instruction::SGET:
        case Instruction::SGET_OBJECT:
        case Instruction::SGET_BOOLEAN:
        case Instruction::SGET_BYTE:
        case Instruction::SGET_CHAR:
        case Instruction::SGET_SHORT:
          DCHECK_LT(mir->meta.sfield_lowering_info, mir_graph_->GetSFieldLoweringInfoCount());
          sregs_[defs[0]] = sfields_[mir->meta.sfield_lowering_info].AsNonNull();
          break;
        case Instruction::SPUT_WIDE:
          DCHECK_EQ(uses[0] + 1, uses[1]);
          FALLTHROUGH_INTENDED;
        case Instruction::SPUT:
        case Instruction::SPUT_OBJECT:
        case Instruction::SPUT_BOOLEAN:
        case Instruction::SPUT_BYTE:
        case Instruction::SPUT_CHAR:
        case Instruction::SPUT_SHORT:
          DCHECK_LT(mir->meta.sfield_lowering_info, mir_graph_->GetSFieldLoweringInfoCount());
          UpdateSRegFromLowWordType(ModifiedSReg(uses[0]),
                                          sfields_[mir->meta.sfield_lowering_info]);
          break;

        default:
          // No invokes or reference definitions here.
          DCHECK_EQ(attrs & (DF_FORMAT_35C | DF_FORMAT_3RC), 0u);
          DCHECK_NE(attrs & (DF_DA | DF_REF_A), (DF_DA | DF_REF_A));
          break;
      }

      if ((attrs & DF_NULL_TRANSFER_N) != 0) {
        // Don't process Phis at this stage.
        continue;
      }

      // Handle defs
      if (attrs & DF_DA) {
        int32_t s_reg = defs[0];
        sregs_[s_reg].SetLowWord();
        if (attrs & DF_FP_A) {
          sregs_[s_reg].SetFp();
        }
        if (attrs & DF_CORE_A) {
          sregs_[s_reg].SetCore();
        }
        if (attrs & DF_REF_A) {
          sregs_[s_reg].SetRef();
        }
        if (attrs & DF_A_WIDE) {
          sregs_[s_reg].SetWide();
          DCHECK_EQ(s_reg + 1, ModifiedSReg(defs[1]));
          sregs_[s_reg + 1].MergeHighWord(sregs_[s_reg]);
        } else {
          sregs_[s_reg].SetNarrow();
        }
      }

      // Handles uses
      size_t next = 0;
  #define PROCESS(REG)                                                        \
      if (attrs & DF_U##REG) {                                                \
        int32_t mod_s_reg = ModifiedSReg(uses[next]);                         \
        sregs_[mod_s_reg].SetLowWord();                                       \
        if (attrs & DF_FP_##REG) {                                            \
          sregs_[mod_s_reg].SetFp();                                          \
        }                                                                     \
        if (attrs & DF_CORE_##REG) {                                          \
          sregs_[mod_s_reg].SetCore();                                        \
        }                                                                     \
        if (attrs & DF_REF_##REG) {                                           \
          sregs_[mod_s_reg].SetRef();                                         \
        }                                                                     \
        if (attrs & DF_##REG##_WIDE) {                                        \
          sregs_[mod_s_reg].SetWide();                                        \
          DCHECK_EQ(mod_s_reg + 1, ModifiedSReg(uses[next + 1]));             \
          sregs_[mod_s_reg + 1].SetWide();                                    \
          sregs_[mod_s_reg + 1].MergeHighWord(sregs_[mod_s_reg]);             \
          next += 2;                                                          \
        } else {                                                              \
          sregs_[mod_s_reg].SetNarrow();                                      \
          next++;                                                             \
        }                                                                     \
      }
      PROCESS(A)
      PROCESS(B)
      PROCESS(C)
  #undef PROCESS
      DCHECK(next == mir->ssa_rep->num_uses || (attrs & (DF_FORMAT_35C | DF_FORMAT_3RC)) != 0);
    }
    // Record relevant attributes.
    bb_df_attrs_[bb->id] = bb_df_attrs &
        (DF_NULL_TRANSFER_N | DF_CHK_CAST | DF_IS_MOVE | DF_HAS_RANGE_CHKS | DF_SAME_TYPE_AB);
  }

  if (UNLIKELY(check_cast_data_ != nullptr)) {
    check_cast_data_->MarkPseudoPhiBlocks(bb_df_attrs_);
  }
}

int32_t TypeInference::ModifiedSReg(int32_t s_reg) {
  if (UNLIKELY(check_cast_data_ != nullptr)) {
    SplitSRegData* split_data = check_cast_data_->GetSplitSRegData(s_reg);
    if (UNLIKELY(split_data != nullptr)) {
      DCHECK_NE(split_data->current_mod_s_reg, INVALID_SREG);
      return split_data->current_mod_s_reg;
    }
  }
  return s_reg;
}

int32_t TypeInference::PhiInputModifiedSReg(int32_t s_reg, BasicBlock* bb, size_t pred_idx) {
  DCHECK_LT(pred_idx, bb->predecessors.size());
  if (UNLIKELY(check_cast_data_ != nullptr)) {
    SplitSRegData* split_data = check_cast_data_->GetSplitSRegData(s_reg);
    if (UNLIKELY(split_data != nullptr)) {
      return split_data->ending_mod_s_reg[bb->predecessors[pred_idx]];
    }
  }
  return s_reg;
}

bool TypeInference::UpdateSRegFromLowWordType(int32_t mod_s_reg, Type low_word_type) {
  DCHECK(low_word_type.LowWord());
  bool changed = sregs_[mod_s_reg].MergeStrong(low_word_type);
  if (!sregs_[mod_s_reg].Narrow()) {  // Wide without conflict with narrow.
    DCHECK(!low_word_type.Narrow());
    DCHECK_LT(mod_s_reg, mir_graph_->GetNumSSARegs());  // Original SSA reg.
    changed |= sregs_[mod_s_reg + 1].MergeHighWord(sregs_[mod_s_reg]);
  }
  return changed;
}

}  // namespace art
