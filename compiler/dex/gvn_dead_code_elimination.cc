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

#include <sstream>

#include "gvn_dead_code_elimination.h"

#include "base/bit_vector-inl.h"
#include "base/macros.h"
#include "base/allocator.h"
#include "compiler_enums.h"
#include "dataflow_iterator-inl.h"
#include "dex_instruction.h"
#include "dex/mir_graph.h"
#include "local_value_numbering.h"
#include "utils/arena_bit_vector.h"

namespace art {

constexpr uint16_t GvnDeadCodeElimination::kNoValue;
constexpr uint16_t GvnDeadCodeElimination::kNPos;

inline uint16_t GvnDeadCodeElimination::MIRData::PrevChange(int v_reg) const {
  DCHECK(has_def);
  DCHECK(v_reg == vreg_def || v_reg == vreg_def + 1);
  return (v_reg == vreg_def) ? prev_value.change : prev_value_high.change;
}

inline void GvnDeadCodeElimination::MIRData::SetPrevChange(int v_reg, uint16_t change) {
  DCHECK(has_def);
  DCHECK(v_reg == vreg_def || v_reg == vreg_def + 1);
  if (v_reg == vreg_def) {
    prev_value.change = change;
  } else {
    prev_value_high.change = change;
  }
}

inline void GvnDeadCodeElimination::MIRData::RemovePrevChange(int v_reg, MIRData* prev_data) {
  DCHECK_NE(PrevChange(v_reg), kNPos);
  DCHECK(v_reg == prev_data->vreg_def || v_reg == prev_data->vreg_def + 1);
  if (vreg_def == v_reg) {
    if (prev_data->vreg_def == v_reg) {
      prev_value = prev_data->prev_value;
      low_def_over_high_word = prev_data->low_def_over_high_word;
    } else {
      prev_value = prev_data->prev_value_high;
      low_def_over_high_word = !prev_data->high_def_over_low_word;
    }
  } else {
    if (prev_data->vreg_def == v_reg) {
      prev_value_high = prev_data->prev_value;
      high_def_over_low_word = !prev_data->low_def_over_high_word;
    } else {
      prev_value_high = prev_data->prev_value_high;
      high_def_over_low_word = prev_data->high_def_over_low_word;
    }
  }
}

GvnDeadCodeElimination::VRegChains::VRegChains(uint32_t num_vregs, ScopedArenaAllocator* alloc)
    : num_vregs_(num_vregs),
      vreg_data_(alloc->AllocArray<VRegValue>(num_vregs, kArenaAllocMisc)),
      vreg_high_words_(false, Allocator::GetNoopAllocator(),
                       BitVector::BitsToWords(num_vregs),
                       alloc->AllocArray<uint32_t>(BitVector::BitsToWords(num_vregs))),
      mir_data_(alloc->Adapter()) {
  mir_data_.reserve(100);
}

inline void GvnDeadCodeElimination::VRegChains::Reset() {
  DCHECK(mir_data_.empty());
  std::fill_n(vreg_data_, num_vregs_, VRegValue());
  vreg_high_words_.ClearAllBits();
}

void GvnDeadCodeElimination::VRegChains::AddMIRWithDef(MIR* mir, int v_reg, bool wide,
                                                       uint16_t new_value) {
  uint16_t pos = mir_data_.size();
  mir_data_.emplace_back(mir);
  MIRData* data = &mir_data_.back();
  data->has_def = true;
  data->wide_def = wide;
  data->vreg_def = v_reg;

  DCHECK_LT(static_cast<size_t>(v_reg), num_vregs_);
  data->prev_value = vreg_data_[v_reg];
  data->low_def_over_high_word =
      (vreg_data_[v_reg].change != kNPos)
      ? GetMIRData(vreg_data_[v_reg].change)->vreg_def + 1 == v_reg
      : vreg_high_words_.IsBitSet(v_reg);
  vreg_data_[v_reg].value = new_value;
  vreg_data_[v_reg].change = pos;
  vreg_high_words_.ClearBit(v_reg);

  if (wide) {
    DCHECK_LT(static_cast<size_t>(v_reg + 1), num_vregs_);
    data->prev_value_high = vreg_data_[v_reg + 1];
    data->high_def_over_low_word =
        (vreg_data_[v_reg + 1].change != kNPos)
        ? GetMIRData(vreg_data_[v_reg + 1].change)->vreg_def == v_reg + 1
        : !vreg_high_words_.IsBitSet(v_reg + 1);
    vreg_data_[v_reg + 1].value = new_value;
    vreg_data_[v_reg + 1].change = pos;
    vreg_high_words_.SetBit(v_reg + 1);
  }
}

inline void GvnDeadCodeElimination::VRegChains::AddMIRWithoutDef(MIR* mir) {
  mir_data_.emplace_back(mir);
}

void GvnDeadCodeElimination::VRegChains::RemoveLastMIRData() {
  MIRData* data = LastMIRData();
  if (data->has_def) {
    DCHECK_EQ(vreg_data_[data->vreg_def].change, NumMIRs() - 1u);
    vreg_data_[data->vreg_def] = data->prev_value;
    DCHECK(!vreg_high_words_.IsBitSet(data->vreg_def));
    if (data->low_def_over_high_word) {
      vreg_high_words_.SetBit(data->vreg_def);
    }
    if (data->wide_def) {
      DCHECK_EQ(vreg_data_[data->vreg_def + 1].change, NumMIRs() - 1u);
      vreg_data_[data->vreg_def + 1] = data->prev_value_high;
      DCHECK(vreg_high_words_.IsBitSet(data->vreg_def + 1));
      if (data->high_def_over_low_word) {
        vreg_high_words_.ClearBit(data->vreg_def + 1);
      }
    }
  }
  mir_data_.pop_back();
}

void GvnDeadCodeElimination::VRegChains::RemoveTrailingNops() {
  // There's at least one NOP to drop. There may be more.
  MIRData* last_data = LastMIRData();
  DCHECK(!last_data->must_keep && !last_data->has_def);
  do {
    DCHECK_EQ(static_cast<int>(last_data->mir->dalvikInsn.opcode), static_cast<int>(kMirOpNop));
    mir_data_.pop_back();
    if (mir_data_.empty()) {
      break;
    }
    last_data = LastMIRData();
  } while (!last_data->must_keep && !last_data->has_def);
}

inline size_t GvnDeadCodeElimination::VRegChains::NumMIRs() const {
  return mir_data_.size();
}

inline GvnDeadCodeElimination::MIRData* GvnDeadCodeElimination::VRegChains::GetMIRData(size_t pos) {
  DCHECK_LT(pos, mir_data_.size());
  return &mir_data_[pos];
}

inline GvnDeadCodeElimination::MIRData* GvnDeadCodeElimination::VRegChains::LastMIRData() {
  DCHECK(!mir_data_.empty());
  return &mir_data_.back();
}

uint32_t GvnDeadCodeElimination::VRegChains::NumVRegs() const {
  return num_vregs_;
}

void GvnDeadCodeElimination::VRegChains::InsertInitialValueHigh(int v_reg, uint16_t value) {
  DCHECK_NE(value, kNoValue);
  DCHECK_LT(static_cast<size_t>(v_reg), num_vregs_);
  uint16_t change = vreg_data_[v_reg].change;
  if (change == kNPos) {
    vreg_data_[v_reg].value = value;
    vreg_high_words_.SetBit(v_reg);
  } else {
    while (true) {
      MIRData* data = &mir_data_[change];
      DCHECK(data->vreg_def == v_reg || data->vreg_def + 1 == v_reg);
      if (data->vreg_def == v_reg) {  // Low word, use prev_value.
        if (data->prev_value.change == kNPos) {
          DCHECK_EQ(data->prev_value.value, kNoValue);
          data->prev_value.value = value;
          data->low_def_over_high_word = true;
          break;
        }
        change = data->prev_value.change;
      } else {  // High word, use prev_value_high.
        if (data->prev_value_high.change == kNPos) {
          DCHECK_EQ(data->prev_value_high.value, kNoValue);
          data->prev_value_high.value = value;
          break;
        }
        change = data->prev_value_high.change;
      }
    }
  }
}

void GvnDeadCodeElimination::VRegChains::UpdateInitialVRegValue(int v_reg, bool wide,
                                                                const LocalValueNumbering* lvn) {
  DCHECK_LT(static_cast<size_t>(v_reg), num_vregs_);
  if (!wide) {
    if (vreg_data_[v_reg].value == kNoValue) {
      uint16_t old_value = lvn->GetStartingVregValueNumber(v_reg);
      if (old_value == kNoValue) {
        // Maybe there was a wide value in v_reg before. Do not check for wide value in v_reg-1,
        // that will be done only if we see a definition of v_reg-1, otherwise it's unnecessary.
        old_value = lvn->GetStartingVregValueNumberWide(v_reg);
        if (old_value != kNoValue) {
          InsertInitialValueHigh(v_reg + 1, old_value);
        }
      }
      vreg_data_[v_reg].value = old_value;
      DCHECK(!vreg_high_words_.IsBitSet(v_reg));  // Keep marked as low word.
    }
  } else {
    DCHECK_LT(static_cast<size_t>(v_reg + 1), num_vregs_);
    bool check_high = true;
    if (vreg_data_[v_reg].value == kNoValue) {
      uint16_t old_value = lvn->GetStartingVregValueNumberWide(v_reg);
      if (old_value != kNoValue) {
        InsertInitialValueHigh(v_reg + 1, old_value);
        check_high = false;  // High word has been processed.
      } else {
        // Maybe there was a narrow value before. Do not check for wide value in v_reg-1,
        // that will be done only if we see a definition of v_reg-1, otherwise it's unnecessary.
        old_value = lvn->GetStartingVregValueNumber(v_reg);
      }
      vreg_data_[v_reg].value = old_value;
      DCHECK(!vreg_high_words_.IsBitSet(v_reg));  // Keep marked as low word.
    }
    if (check_high && vreg_data_[v_reg + 1].value == kNoValue) {
      uint16_t old_value = lvn->GetStartingVregValueNumber(v_reg + 1);
      if (old_value == kNoValue && static_cast<size_t>(v_reg + 2) < num_vregs_) {
        // Maybe there was a wide value before.
        old_value = lvn->GetStartingVregValueNumberWide(v_reg + 1);
        if (old_value != kNoValue) {
          InsertInitialValueHigh(v_reg + 2, old_value);
        }
      }
      vreg_data_[v_reg + 1].value = old_value;
      DCHECK(!vreg_high_words_.IsBitSet(v_reg + 1));  // Keep marked as low word.
    }
  }
}

inline uint16_t GvnDeadCodeElimination::VRegChains::LastChange(int v_reg) {
  DCHECK_LT(static_cast<size_t>(v_reg), num_vregs_);
  return vreg_data_[v_reg].change;
}

inline uint16_t GvnDeadCodeElimination::VRegChains::CurrentValue(int v_reg) {
  DCHECK_LT(static_cast<size_t>(v_reg), num_vregs_);
  return vreg_data_[v_reg].value;
}

uint16_t GvnDeadCodeElimination::VRegChains::FindKillHead(int v_reg, uint16_t cutoff) {
  uint16_t current_value = this->CurrentValue(v_reg);
  DCHECK_NE(current_value, kNoValue);
  uint16_t change = LastChange(v_reg);
  DCHECK_LT(change, mir_data_.size());
  DCHECK_GE(change, cutoff);
  bool match_high_word = (mir_data_[change].vreg_def != v_reg);
  do {
    MIRData* data = &mir_data_[change];
    DCHECK(data->vreg_def == v_reg || data->vreg_def + 1 == v_reg);
    if (data->vreg_def == v_reg) {  // Low word, use prev_value.
      if (data->prev_value.value == current_value &&
          match_high_word == data->low_def_over_high_word) {
        break;
      }
      change = data->prev_value.change;
    } else {  // High word, use prev_value_high.
      if (data->prev_value_high.value == current_value &&
          match_high_word != data->high_def_over_low_word) {
        break;
      }
      change = data->prev_value_high.change;
    }
    if (change < cutoff) {
      change = kNPos;
    }
  } while (change != kNPos);
  return change;
}

uint16_t GvnDeadCodeElimination::VRegChains::FindFirstChangeAfter(int v_reg,
                                                                  uint16_t change) const {
  DCHECK_LT(static_cast<size_t>(v_reg), num_vregs_);
  DCHECK_LT(change, mir_data_.size());
  uint16_t result = kNPos;
  uint16_t search_change = vreg_data_[v_reg].change;
  while (search_change != kNPos && search_change > change) {
    result = search_change;
    search_change = mir_data_[search_change].PrevChange(v_reg);
  }
  return result;
}

void GvnDeadCodeElimination::VRegChains::ReplaceChange(uint16_t old_change, uint16_t new_change) {
  const MIRData* old_data = GetMIRData(old_change);
  DCHECK(old_data->has_def);
  int count = old_data->wide_def ? 2 : 1;
  for (int v_reg = old_data->vreg_def, end = old_data->vreg_def + count; v_reg != end; ++v_reg) {
    uint16_t next_change = FindFirstChangeAfter(v_reg, old_change);
    if (next_change == kNPos) {
      DCHECK_EQ(vreg_data_[v_reg].change, old_change);
      vreg_data_[v_reg].change = new_change;
      DCHECK_EQ(vreg_high_words_.IsBitSet(v_reg), v_reg == old_data->vreg_def + 1);
      // No change in vreg_high_words_.
    } else {
      DCHECK_EQ(mir_data_[next_change].PrevChange(v_reg), old_change);
      mir_data_[next_change].SetPrevChange(v_reg, new_change);
    }
  }
}

void GvnDeadCodeElimination::VRegChains::RemoveChange(uint16_t change) {
  MIRData* data = &mir_data_[change];
  DCHECK(data->has_def);
  int count = data->wide_def ? 2 : 1;
  for (int v_reg = data->vreg_def, end = data->vreg_def + count; v_reg != end; ++v_reg) {
    uint16_t next_change = FindFirstChangeAfter(v_reg, change);
    if (next_change == kNPos) {
      DCHECK_EQ(vreg_data_[v_reg].change, change);
      vreg_data_[v_reg] = (data->vreg_def == v_reg) ? data->prev_value : data->prev_value_high;
      DCHECK_EQ(vreg_high_words_.IsBitSet(v_reg), v_reg == data->vreg_def + 1);
      if (data->vreg_def == v_reg && data->low_def_over_high_word) {
        vreg_high_words_.SetBit(v_reg);
      } else if (data->vreg_def != v_reg && data->high_def_over_low_word) {
        vreg_high_words_.ClearBit(v_reg);
      }
    } else {
      DCHECK_EQ(mir_data_[next_change].PrevChange(v_reg), change);
      mir_data_[next_change].RemovePrevChange(v_reg, data);
    }
  }
}

inline bool GvnDeadCodeElimination::VRegChains::IsTopChange(uint16_t change) const {
  DCHECK_LT(change, mir_data_.size());
  const MIRData* data = &mir_data_[change];
  DCHECK(data->has_def);
  DCHECK_LT(data->wide_def ? data->vreg_def + 1u : data->vreg_def, num_vregs_);
  return vreg_data_[data->vreg_def].change == change &&
      (!data->wide_def || vreg_data_[data->vreg_def + 1u].change == change);
}

bool GvnDeadCodeElimination::VRegChains::IsSRegUsed(uint16_t first_change, uint16_t last_change,
                                                    int s_reg) const {
  DCHECK_LE(first_change, last_change);
  DCHECK_LE(last_change, mir_data_.size());
  for (size_t c = first_change; c != last_change; ++c) {
    SSARepresentation* ssa_rep = mir_data_[c].mir->ssa_rep;
    for (int i = 0; i != ssa_rep->num_uses; ++i) {
      if (ssa_rep->uses[i] == s_reg) {
        return true;
      }
    }
  }
  return false;
}

bool GvnDeadCodeElimination::VRegChains::IsVRegUsed(uint16_t first_change, uint16_t last_change,
                                                    int v_reg, MIRGraph* mir_graph) const {
  DCHECK_LE(first_change, last_change);
  DCHECK_LE(last_change, mir_data_.size());
  for (size_t c = first_change; c != last_change; ++c) {
    SSARepresentation* ssa_rep = mir_data_[c].mir->ssa_rep;
    for (int i = 0; i != ssa_rep->num_uses; ++i) {
      if (mir_graph->SRegToVReg(ssa_rep->uses[i]) == v_reg) {
        return true;
      }
    }
  }
  return false;
}

void GvnDeadCodeElimination::VRegChains::RenameSRegUses(uint16_t first_change, uint16_t last_change,
                                                        int old_s_reg, int new_s_reg, bool wide) {
  for (size_t c = first_change; c != last_change; ++c) {
    SSARepresentation* ssa_rep = mir_data_[c].mir->ssa_rep;
    for (int i = 0; i != ssa_rep->num_uses; ++i) {
      if (ssa_rep->uses[i] == old_s_reg) {
        ssa_rep->uses[i] = new_s_reg;
        if (wide) {
          ++i;
          DCHECK_LT(i, ssa_rep->num_uses);
          ssa_rep->uses[i] = new_s_reg + 1;
        }
      }
    }
  }
}

void GvnDeadCodeElimination::VRegChains::RenameVRegUses(uint16_t first_change, uint16_t last_change,
                                                    int old_s_reg, int old_v_reg,
                                                    int new_s_reg, int new_v_reg) {
  for (size_t c = first_change; c != last_change; ++c) {
    MIR* mir = mir_data_[c].mir;
    if (IsInstructionBinOp2Addr(mir->dalvikInsn.opcode) &&
        mir->ssa_rep->uses[0] == old_s_reg && old_v_reg != new_v_reg) {
      // Rewrite binop_2ADDR with plain binop before doing the register rename.
      ChangeBinOp2AddrToPlainBinOp(mir);
    }
    uint64_t df_attr = MIRGraph::GetDataFlowAttributes(mir);
    size_t use = 0u;
#define REPLACE_VREG(REG) \
    if ((df_attr & DF_U##REG) != 0) {                                         \
      if (mir->ssa_rep->uses[use] == old_s_reg) {                             \
        DCHECK_EQ(mir->dalvikInsn.v##REG, static_cast<uint32_t>(old_v_reg));  \
        mir->dalvikInsn.v##REG = new_v_reg;                                   \
        mir->ssa_rep->uses[use] = new_s_reg;                                  \
        if ((df_attr & DF_##REG##_WIDE) != 0) {                               \
          DCHECK_EQ(mir->ssa_rep->uses[use + 1], old_s_reg + 1);              \
          mir->ssa_rep->uses[use + 1] = new_s_reg + 1;                        \
        }                                                                     \
      }                                                                       \
      use += ((df_attr & DF_##REG##_WIDE) != 0) ? 2 : 1;                      \
    }
    REPLACE_VREG(A)
    REPLACE_VREG(B)
    REPLACE_VREG(C)
#undef REPLACE_VREG
    // We may encounter an out-of-order Phi which we need to ignore, otherwise we should
    // only be asked to rename registers specified by DF_UA, DF_UB and DF_UC.
    DCHECK_EQ(use,
              static_cast<int>(mir->dalvikInsn.opcode) == kMirOpPhi
              ? 0u
              : static_cast<size_t>(mir->ssa_rep->num_uses));
  }
}

GvnDeadCodeElimination::GvnDeadCodeElimination(const GlobalValueNumbering* gvn,
                                         ScopedArenaAllocator* alloc)
    : gvn_(gvn),
      mir_graph_(gvn_->GetMirGraph()),
      vreg_chains_(mir_graph_->GetNumOfCodeAndTempVRs(), alloc),
      bb_(nullptr),
      lvn_(nullptr),
      no_uses_all_since_(0u),
      unused_vregs_(new (alloc) ArenaBitVector(alloc, vreg_chains_.NumVRegs(), false)),
      vregs_to_kill_(new (alloc) ArenaBitVector(alloc, vreg_chains_.NumVRegs(), false)),
      kill_heads_(alloc->AllocArray<uint16_t>(vreg_chains_.NumVRegs(), kArenaAllocMisc)),
      changes_to_kill_(alloc->Adapter()),
      dependent_vregs_(new (alloc) ArenaBitVector(alloc, vreg_chains_.NumVRegs(), false)) {
  changes_to_kill_.reserve(16u);
}

void GvnDeadCodeElimination::Apply(BasicBlock* bb) {
  bb_ = bb;
  lvn_ = gvn_->GetLvn(bb->id);

  RecordPass();
  BackwardPass();

  DCHECK_EQ(no_uses_all_since_, 0u);
  lvn_ = nullptr;
  bb_ = nullptr;
}

void GvnDeadCodeElimination::RecordPass() {
  // Record MIRs with vreg definition data, eliminate single instructions.
  vreg_chains_.Reset();
  DCHECK_EQ(no_uses_all_since_, 0u);
  for (MIR* mir = bb_->first_mir_insn; mir != nullptr; mir = mir->next) {
    if (RecordMIR(mir)) {
      RecordPassTryToKillOverwrittenMoveOrMoveSrc();
      RecordPassTryToKillLastMIR();
    }
  }
}

void GvnDeadCodeElimination::BackwardPass() {
  // Now process MIRs in reverse order, trying to eliminate them.
  unused_vregs_->ClearAllBits();  // Implicitly depend on all vregs at the end of BB.
  while (vreg_chains_.NumMIRs() != 0u) {
    if (BackwardPassTryToKillLastMIR()) {
      continue;
    }
    BackwardPassProcessLastMIR();
  }
}

void GvnDeadCodeElimination::KillMIR(MIRData* data) {
  DCHECK(!data->must_keep);
  DCHECK(!data->uses_all_vregs);
  DCHECK(data->has_def);
  DCHECK(data->mir->ssa_rep->num_defs == 1 || data->mir->ssa_rep->num_defs == 2);

  KillMIR(data->mir);
  data->has_def = false;
  data->is_move = false;
  data->is_move_src = false;
}

void GvnDeadCodeElimination::KillMIR(MIR* mir) {
  mir->dalvikInsn.opcode = static_cast<Instruction::Code>(kMirOpNop);
  mir->ssa_rep->num_uses = 0;
  mir->ssa_rep->num_defs = 0;
}

void GvnDeadCodeElimination::ChangeBinOp2AddrToPlainBinOp(MIR* mir) {
  mir->dalvikInsn.vC = mir->dalvikInsn.vB;
  mir->dalvikInsn.vB = mir->dalvikInsn.vA;
  mir->dalvikInsn.opcode = static_cast<Instruction::Code>(
      mir->dalvikInsn.opcode - Instruction::ADD_INT_2ADDR +  Instruction::ADD_INT);
}

MIR* GvnDeadCodeElimination::CreatePhi(int s_reg) {
  int v_reg = mir_graph_->SRegToVReg(s_reg);
  MIR* phi = mir_graph_->NewMIR();
  phi->dalvikInsn.opcode = static_cast<Instruction::Code>(kMirOpPhi);
  phi->dalvikInsn.vA = v_reg;
  phi->offset = bb_->start_offset;
  phi->m_unit_index = 0;  // Arbitrarily assign all Phi nodes to outermost method.

  phi->ssa_rep = static_cast<struct SSARepresentation *>(mir_graph_->GetArena()->Alloc(
      sizeof(SSARepresentation), kArenaAllocDFInfo));

  mir_graph_->AllocateSSADefData(phi, 1);
  phi->ssa_rep->defs[0] = s_reg;

  size_t num_uses = bb_->predecessors.size();
  mir_graph_->AllocateSSAUseData(phi, num_uses);
  size_t idx = 0u;
  for (BasicBlockId pred_id : bb_->predecessors) {
    BasicBlock* pred_bb = mir_graph_->GetBasicBlock(pred_id);
    DCHECK(pred_bb != nullptr);
    phi->ssa_rep->uses[idx] = pred_bb->data_flow_info->vreg_to_ssa_map_exit[v_reg];
    DCHECK_NE(phi->ssa_rep->uses[idx], INVALID_SREG);
    idx++;
  }

  phi->meta.phi_incoming = static_cast<BasicBlockId*>(mir_graph_->GetArena()->Alloc(
      sizeof(BasicBlockId) * num_uses, kArenaAllocDFInfo));
  std::copy(bb_->predecessors.begin(), bb_->predecessors.end(), phi->meta.phi_incoming);
  bb_->PrependMIR(phi);
  return phi;
}

MIR* GvnDeadCodeElimination::RenameSRegDefOrCreatePhi(uint16_t def_change, uint16_t last_change,
                                                      MIR* mir_to_kill) {
  DCHECK(mir_to_kill->ssa_rep->num_defs == 1 || mir_to_kill->ssa_rep->num_defs == 2);
  bool wide = (mir_to_kill->ssa_rep->num_defs != 1);
  int new_s_reg = mir_to_kill->ssa_rep->defs[0];

  // Just before we kill mir_to_kill, we need to replace the previous SSA reg assigned to the
  // same dalvik reg to keep consistency with subsequent instructions. However, if there's no
  // defining MIR for that dalvik reg, the preserved values must come from its predecessors
  // and we need to create a new Phi (a degenerate Phi if there's only a single predecessor).
  if (def_change == kNPos) {
    if (wide) {
      DCHECK_EQ(new_s_reg + 1, mir_to_kill->ssa_rep->defs[1]);
      DCHECK_EQ(mir_graph_->SRegToVReg(new_s_reg) + 1, mir_graph_->SRegToVReg(new_s_reg + 1));
      CreatePhi(new_s_reg + 1);  // High word Phi.
    }
    MIR* phi = CreatePhi(new_s_reg);
    // If this is a degenerate Phi with all inputs being the same SSA reg, we need to its uses.
    DCHECK_NE(phi->ssa_rep->num_uses, 0u);
    int old_s_reg = phi->ssa_rep->uses[0];
    bool all_same = true;
    for (size_t i = 1u, num = phi->ssa_rep->num_uses; i != num; ++i) {
      if (phi->ssa_rep->uses[i] != old_s_reg) {
        all_same = false;
        break;
      }
    }
    if (all_same) {
      vreg_chains_.RenameSRegUses(0u, last_change, old_s_reg, new_s_reg, wide);
    }
    return phi;
  } else {
    DCHECK_LT(def_change, last_change);
    DCHECK_LE(last_change, vreg_chains_.NumMIRs());
    MIRData* def_data = vreg_chains_.GetMIRData(def_change);
    DCHECK(def_data->has_def);
    int old_s_reg = def_data->mir->ssa_rep->defs[0];
    DCHECK_NE(old_s_reg, new_s_reg);
    DCHECK_EQ(mir_graph_->SRegToVReg(old_s_reg), mir_graph_->SRegToVReg(new_s_reg));
    def_data->mir->ssa_rep->defs[0] = new_s_reg;
    if (wide) {
      if (static_cast<int>(def_data->mir->dalvikInsn.opcode) == kMirOpPhi) {
        // Currently the high word Phi is always located after the low word Phi.
        MIR* phi_high = def_data->mir->next;
        DCHECK(phi_high != nullptr && static_cast<int>(phi_high->dalvikInsn.opcode) == kMirOpPhi);
        DCHECK_EQ(phi_high->ssa_rep->defs[0], old_s_reg + 1);
        phi_high->ssa_rep->defs[0] = new_s_reg + 1;
      } else {
        DCHECK_EQ(def_data->mir->ssa_rep->defs[1], old_s_reg + 1);
        def_data->mir->ssa_rep->defs[1] = new_s_reg + 1;
      }
    }
    vreg_chains_.RenameSRegUses(def_change + 1u, last_change, old_s_reg, new_s_reg, wide);
    return nullptr;
  }
}


void GvnDeadCodeElimination::BackwardPassProcessLastMIR() {
  MIRData* data = vreg_chains_.LastMIRData();
  if (data->uses_all_vregs) {
    DCHECK(data->must_keep);
    unused_vregs_->ClearAllBits();
    DCHECK_EQ(no_uses_all_since_, vreg_chains_.NumMIRs());
    --no_uses_all_since_;
    while (no_uses_all_since_ != 0u &&
        !vreg_chains_.GetMIRData(no_uses_all_since_ - 1u)->uses_all_vregs) {
      --no_uses_all_since_;
    }
  } else {
    if (data->has_def) {
      unused_vregs_->SetBit(data->vreg_def);
      if (data->wide_def) {
        unused_vregs_->SetBit(data->vreg_def + 1);
      }
    }
    for (int i = 0, num_uses = data->mir->ssa_rep->num_uses; i != num_uses; ++i) {
      int v_reg = mir_graph_->SRegToVReg(data->mir->ssa_rep->uses[i]);
      unused_vregs_->ClearBit(v_reg);
    }
  }
  vreg_chains_.RemoveLastMIRData();
}

void GvnDeadCodeElimination::RecordPassKillMoveByRenamingSrcDef(uint16_t src_change,
                                                                uint16_t move_change) {
  DCHECK_LT(src_change, move_change);
  MIRData* src_data = vreg_chains_.GetMIRData(src_change);
  MIRData* move_data = vreg_chains_.GetMIRData(move_change);
  DCHECK(src_data->is_move_src);
  DCHECK_EQ(src_data->wide_def, move_data->wide_def);
  DCHECK(move_data->prev_value.change == kNPos || move_data->prev_value.change <= src_change);
  DCHECK(!move_data->wide_def || move_data->prev_value_high.change == kNPos ||
         move_data->prev_value_high.change <= src_change);

  int old_s_reg = src_data->mir->ssa_rep->defs[0];
  // NOTE: old_s_reg may differ from move_data->mir->ssa_rep->uses[0]; value names must match.
  int new_s_reg = move_data->mir->ssa_rep->defs[0];
  DCHECK_NE(old_s_reg, new_s_reg);

  if (IsInstructionBinOp2Addr(src_data->mir->dalvikInsn.opcode) &&
      src_data->vreg_def != move_data->vreg_def) {
    // Rewrite binop_2ADDR with plain binop before doing the register rename.
    ChangeBinOp2AddrToPlainBinOp(src_data->mir);
  }
  // Remove src_change from the vreg chain(s).
  vreg_chains_.RemoveChange(src_change);
  // Replace the move_change with the src_change, copying all necessary data.
  src_data->is_move_src = move_data->is_move_src;
  src_data->low_def_over_high_word = move_data->low_def_over_high_word;
  src_data->high_def_over_low_word = move_data->high_def_over_low_word;
  src_data->vreg_def = move_data->vreg_def;
  src_data->prev_value = move_data->prev_value;
  src_data->prev_value_high = move_data->prev_value_high;
  src_data->mir->dalvikInsn.vA = move_data->vreg_def;
  src_data->mir->ssa_rep->defs[0] = new_s_reg;
  if (move_data->wide_def) {
    DCHECK_EQ(src_data->mir->ssa_rep->defs[1], old_s_reg + 1);
    src_data->mir->ssa_rep->defs[1] = new_s_reg + 1;
  }
  vreg_chains_.ReplaceChange(move_change, src_change);

  // Rename uses and kill the move.
  vreg_chains_.RenameVRegUses(src_change + 1u, vreg_chains_.NumMIRs(),
                              old_s_reg, mir_graph_->SRegToVReg(old_s_reg),
                              new_s_reg, mir_graph_->SRegToVReg(new_s_reg));
  KillMIR(move_data);
}

void GvnDeadCodeElimination::RecordPassTryToKillOverwrittenMoveOrMoveSrc(uint16_t check_change) {
  MIRData* data = vreg_chains_.GetMIRData(check_change);
  DCHECK(data->is_move || data->is_move_src);
  int32_t dest_s_reg = data->mir->ssa_rep->defs[0];

  if (data->is_move) {
    // Check if source vreg has changed since the MOVE.
    int32_t src_s_reg = data->mir->ssa_rep->uses[0];
    uint32_t src_v_reg = mir_graph_->SRegToVReg(src_s_reg);
    uint16_t src_change = vreg_chains_.FindFirstChangeAfter(src_v_reg, check_change);
    bool wide = data->wide_def;
    if (wide) {
      uint16_t src_change_high = vreg_chains_.FindFirstChangeAfter(src_v_reg + 1, check_change);
      if (src_change_high != kNPos && (src_change == kNPos || src_change_high < src_change)) {
        src_change = src_change_high;
      }
    }
    if (src_change == kNPos ||
        !vreg_chains_.IsSRegUsed(src_change + 1u, vreg_chains_.NumMIRs(), dest_s_reg)) {
      // We can simply change all uses of dest to src.
      size_t rename_end = (src_change != kNPos) ? src_change + 1u : vreg_chains_.NumMIRs();
      vreg_chains_.RenameVRegUses(check_change + 1u, rename_end,
                                  dest_s_reg, mir_graph_->SRegToVReg(dest_s_reg),
                                  src_s_reg,  mir_graph_->SRegToVReg(src_s_reg));

      // Now, remove the MOVE from the vreg chain(s) and kill it.
      vreg_chains_.RemoveChange(check_change);
      KillMIR(data);
      return;
    }
  }

  if (data->is_move_src) {
    // Try to find a MOVE to a vreg that wasn't changed since check_change.
    uint16_t value_name =
        data->wide_def ? lvn_->GetSregValueWide(dest_s_reg) : lvn_->GetSregValue(dest_s_reg);
    uint32_t dest_v_reg = mir_graph_->SRegToVReg(dest_s_reg);
    for (size_t c = check_change + 1u, size = vreg_chains_.NumMIRs(); c != size; ++c) {
      MIRData* d = vreg_chains_.GetMIRData(c);
      if (d->is_move && d->wide_def == data->wide_def &&
          (d->prev_value.change == kNPos || d->prev_value.change <= check_change) &&
          (!d->wide_def ||
           d->prev_value_high.change == kNPos || d->prev_value_high.change <= check_change)) {
        // Compare value names to find move to move.
        int32_t src_s_reg = d->mir->ssa_rep->uses[0];
        uint16_t src_name =
            (d->wide_def ? lvn_->GetSregValueWide(src_s_reg) : lvn_->GetSregValue(src_s_reg));
        if (value_name == src_name) {
          // Check if the move's destination vreg is unused between check_change and the move.
          uint32_t new_dest_v_reg = mir_graph_->SRegToVReg(d->mir->ssa_rep->defs[0]);
          if (!vreg_chains_.IsVRegUsed(check_change + 1u, c, new_dest_v_reg, mir_graph_) &&
              (!d->wide_def ||
               !vreg_chains_.IsVRegUsed(check_change + 1u, c, new_dest_v_reg + 1, mir_graph_))) {
            // If the move's destination vreg changed, check if the vreg we're trying
            // to rename is unused after that change.
            uint16_t dest_change = vreg_chains_.FindFirstChangeAfter(new_dest_v_reg, c);
            if (d->wide_def) {
              uint16_t dest_change_high = vreg_chains_.FindFirstChangeAfter(new_dest_v_reg + 1, c);
              if (dest_change_high != kNPos &&
                  (dest_change == kNPos || dest_change_high < dest_change)) {
                dest_change = dest_change_high;
              }
            }
            if (dest_change == kNPos ||
                !vreg_chains_.IsVRegUsed(dest_change + 1u, size, dest_v_reg, mir_graph_)) {
              RecordPassKillMoveByRenamingSrcDef(check_change, c);
              return;
            }
          }
        }
      }
    }
  }
}

void GvnDeadCodeElimination::RecordPassTryToKillOverwrittenMoveOrMoveSrc() {
  // Check if we're overwriting a the result of a move or the definition of a source of a move.
  // For MOVE_WIDE, we may be overwriting partially; if that's the case, check that the other
  // word wasn't previously overwritten - we would have tried to rename back then.
  MIRData* data = vreg_chains_.LastMIRData();
  if (!data->has_def) {
    return;
  }
  // NOTE: Instructions such as new-array implicitly use all vregs (if they throw) but they can
  // define a move source which can be renamed. Therefore we allow the checked change to be the
  // change before no_uses_all_since_. This has no effect on moves as they never use all vregs.
  if (data->prev_value.change != kNPos && data->prev_value.change + 1u >= no_uses_all_since_) {
    MIRData* check_data = vreg_chains_.GetMIRData(data->prev_value.change);
    bool try_to_kill = false;
    if (!check_data->is_move && !check_data->is_move_src) {
      DCHECK(!try_to_kill);
    } else if (!check_data->wide_def) {
      // Narrow move; always fully overwritten by the last MIR.
      try_to_kill = true;
    } else if (data->low_def_over_high_word) {
      // Overwriting only the high word; is the low word still valid?
      DCHECK_EQ(check_data->vreg_def + 1u, data->vreg_def);
      if (vreg_chains_.LastChange(check_data->vreg_def) == data->prev_value.change) {
        try_to_kill = true;
      }
    } else if (!data->wide_def) {
      // Overwriting only the low word, is the high word still valid?
      if (vreg_chains_.LastChange(data->vreg_def + 1) == data->prev_value.change) {
        try_to_kill = true;
      }
    } else {
      // Overwriting both words; was the high word still from the same move?
      if (data->prev_value_high.change == data->prev_value.change) {
        try_to_kill = true;
      }
    }
    if (try_to_kill) {
      RecordPassTryToKillOverwrittenMoveOrMoveSrc(data->prev_value.change);
    }
  }
  if (data->wide_def && data->high_def_over_low_word &&
      data->prev_value_high.change != kNPos &&
      data->prev_value_high.change + 1u >= no_uses_all_since_) {
    MIRData* check_data = vreg_chains_.GetMIRData(data->prev_value_high.change);
    bool try_to_kill = false;
    if (!check_data->is_move && !check_data->is_move_src) {
      DCHECK(!try_to_kill);
    } else if (!check_data->wide_def) {
      // Narrow move; always fully overwritten by the last MIR.
      try_to_kill = true;
    } else if (vreg_chains_.LastChange(check_data->vreg_def + 1) ==
        data->prev_value_high.change) {
      // High word is still valid.
      try_to_kill = true;
    }
    if (try_to_kill) {
      RecordPassTryToKillOverwrittenMoveOrMoveSrc(data->prev_value_high.change);
    }
  }
}

void GvnDeadCodeElimination::RecordPassTryToKillLastMIR() {
  MIRData* last_data = vreg_chains_.LastMIRData();
  if (last_data->must_keep) {
    return;
  }
  if (UNLIKELY(!last_data->has_def)) {
    // Must be an eliminated MOVE. Drop its data and data of all eliminated MIRs before it.
    vreg_chains_.RemoveTrailingNops();
    return;
  }

  // Try to kill a sequence of consecutive definitions of the same vreg. Allow mixing
  // wide and non-wide defs; consider high word dead if low word has been overwritten.
  uint16_t current_value = vreg_chains_.CurrentValue(last_data->vreg_def);
  uint16_t change = vreg_chains_.NumMIRs() - 1u;
  MIRData* data = last_data;
  while (data->prev_value.value != current_value) {
    --change;
    if (data->prev_value.change == kNPos || data->prev_value.change != change) {
      return;
    }
    data = vreg_chains_.GetMIRData(data->prev_value.change);
    if (data->must_keep || !data->has_def || data->vreg_def != last_data->vreg_def) {
      return;
    }
  }

  bool wide = last_data->wide_def;
  if (wide) {
    // Check that the low word is valid.
    if (data->low_def_over_high_word) {
      return;
    }
    // Check that the high word is valid.
    MIRData* high_data = data;
    if (!high_data->wide_def) {
      uint16_t high_change = vreg_chains_.FindFirstChangeAfter(data->vreg_def + 1, change);
      DCHECK_NE(high_change, kNPos);
      high_data = vreg_chains_.GetMIRData(high_change);
      DCHECK_EQ(high_data->vreg_def, data->vreg_def);
    }
    if (high_data->prev_value_high.value != current_value || high_data->high_def_over_low_word) {
      return;
    }
  }

  MIR* phi = RenameSRegDefOrCreatePhi(data->prev_value.change, change, last_data->mir);
  for (size_t i = 0, count = vreg_chains_.NumMIRs() - change; i != count; ++i) {
    KillMIR(vreg_chains_.LastMIRData()->mir);
    vreg_chains_.RemoveLastMIRData();
  }
  if (phi != nullptr) {
    // Though the Phi has been added to the beginning, we can put the MIRData at the end.
    vreg_chains_.AddMIRWithDef(phi, phi->dalvikInsn.vA, wide, current_value);
    // Reset the previous value to avoid eventually eliminating the Phi itself (unless unused).
    last_data = vreg_chains_.LastMIRData();
    last_data->prev_value.value = kNoValue;
    last_data->prev_value_high.value = kNoValue;
  }
}

uint16_t GvnDeadCodeElimination::FindChangesToKill(uint16_t first_change, uint16_t last_change) {
  // Process dependencies for changes in range [first_change, last_change) and record all
  // changes that we need to kill. Return kNPos if there's a dependent change that must be
  // kept unconditionally; otherwise the end of the range processed before encountering
  // a change that defines a dalvik reg that we need to keep (last_change on full success).
  changes_to_kill_.clear();
  dependent_vregs_->ClearAllBits();
  for (size_t change = first_change; change != last_change; ++change) {
    MIRData* data = vreg_chains_.GetMIRData(change);
    DCHECK(!data->uses_all_vregs);
    bool must_not_depend = data->must_keep;
    bool depends = false;
    // Check if the MIR defines a vreg we're trying to eliminate.
    if (data->has_def && vregs_to_kill_->IsBitSet(data->vreg_def)) {
      if (change < kill_heads_[data->vreg_def]) {
        must_not_depend = true;
      } else {
        depends = true;
      }
    }
    if (data->has_def && data->wide_def && vregs_to_kill_->IsBitSet(data->vreg_def + 1)) {
      if (change < kill_heads_[data->vreg_def + 1]) {
        must_not_depend = true;
      } else {
        depends = true;
      }
    }
    if (!depends) {
      // Check for dependency through SSA reg uses.
      SSARepresentation* ssa_rep = data->mir->ssa_rep;
      for (int i = 0; i != ssa_rep->num_uses; ++i) {
        if (dependent_vregs_->IsBitSet(mir_graph_->SRegToVReg(ssa_rep->uses[i]))) {
          depends = true;
          break;
        }
      }
    }
    // Now check if we can eliminate the insn if we need to.
    if (depends && must_not_depend) {
      return kNPos;
    }
    if (depends && data->has_def &&
        vreg_chains_.IsTopChange(change) && !vregs_to_kill_->IsBitSet(data->vreg_def) &&
        !unused_vregs_->IsBitSet(data->vreg_def) &&
        (!data->wide_def || !unused_vregs_->IsBitSet(data->vreg_def + 1))) {
      // This is a top change but neither unnecessary nor one of the top kill changes.
      return change;
    }
    // Finally, update the data.
    if (depends) {
      changes_to_kill_.push_back(change);
      if (data->has_def) {
        dependent_vregs_->SetBit(data->vreg_def);
        if (data->wide_def) {
          dependent_vregs_->SetBit(data->vreg_def + 1);
        }
      }
    } else {
      if (data->has_def) {
        dependent_vregs_->ClearBit(data->vreg_def);
        if (data->wide_def) {
          dependent_vregs_->ClearBit(data->vreg_def + 1);
        }
      }
    }
  }
  return last_change;
}

void GvnDeadCodeElimination::BackwardPassTryToKillRevertVRegs() {
}

bool GvnDeadCodeElimination::BackwardPassTryToKillLastMIR() {
  MIRData* last_data = vreg_chains_.LastMIRData();
  if (last_data->must_keep) {
    return false;
  }
  DCHECK(!last_data->uses_all_vregs);
  if (!last_data->has_def) {
    // Previously eliminated.
    DCHECK_EQ(static_cast<int>(last_data->mir->dalvikInsn.opcode), static_cast<int>(kMirOpNop));
    vreg_chains_.RemoveTrailingNops();
    return true;
  }
  if (unused_vregs_->IsBitSet(last_data->vreg_def) ||
      (last_data->wide_def && unused_vregs_->IsBitSet(last_data->vreg_def + 1))) {
    if (last_data->wide_def) {
      // For wide defs, one of the vregs may still be considered needed, fix that.
      unused_vregs_->SetBit(last_data->vreg_def);
      unused_vregs_->SetBit(last_data->vreg_def + 1);
    }
    KillMIR(last_data->mir);
    vreg_chains_.RemoveLastMIRData();
    return true;
  }

  vregs_to_kill_->ClearAllBits();
  size_t num_mirs = vreg_chains_.NumMIRs();
  DCHECK_NE(num_mirs, 0u);
  uint16_t kill_change = num_mirs - 1u;
  uint16_t start = num_mirs;
  size_t num_killed_top_changes = 0u;
  while (num_killed_top_changes != kMaxNumTopChangesToKill &&
      kill_change != kNPos && kill_change != num_mirs) {
    ++num_killed_top_changes;

    DCHECK(vreg_chains_.IsTopChange(kill_change));
    MIRData* data = vreg_chains_.GetMIRData(kill_change);
    int count = data->wide_def ? 2 : 1;
    for (int v_reg = data->vreg_def, end = data->vreg_def + count; v_reg != end; ++v_reg) {
      uint16_t kill_head = vreg_chains_.FindKillHead(v_reg, no_uses_all_since_);
      if (kill_head == kNPos) {
        return false;
      }
      kill_heads_[v_reg] = kill_head;
      vregs_to_kill_->SetBit(v_reg);
      start = std::min(start, kill_head);
    }
    DCHECK_LT(start, vreg_chains_.NumMIRs());

    kill_change = FindChangesToKill(start, num_mirs);
  }

  if (kill_change != num_mirs) {
    return false;
  }

  // Kill all MIRs marked as dependent.
  for (uint32_t v_reg : vregs_to_kill_->Indexes()) {
    // Rename s_regs or create Phi only once for each MIR (only for low word).
    MIRData* data = vreg_chains_.GetMIRData(vreg_chains_.LastChange(v_reg));
    DCHECK(data->has_def);
    if (data->vreg_def == v_reg) {
      MIRData* kill_head_data = vreg_chains_.GetMIRData(kill_heads_[v_reg]);
      RenameSRegDefOrCreatePhi(kill_head_data->PrevChange(v_reg), num_mirs, data->mir);
    } else {
      DCHECK_EQ(data->vreg_def + 1u, v_reg);
      DCHECK_EQ(vreg_chains_.GetMIRData(kill_heads_[v_reg - 1u])->PrevChange(v_reg - 1u),
                vreg_chains_.GetMIRData(kill_heads_[v_reg])->PrevChange(v_reg));
    }
  }
  for (auto it = changes_to_kill_.rbegin(), end = changes_to_kill_.rend(); it != end; ++it) {
    MIRData* data = vreg_chains_.GetMIRData(*it);
    DCHECK(!data->must_keep);
    DCHECK(data->has_def);
    vreg_chains_.RemoveChange(*it);
    KillMIR(data);
  }

  // Each dependent register not in vregs_to_kill_ is either already marked unused or
  // it's one word of a wide register where the other word has been overwritten.
  unused_vregs_->UnionIfNotIn(dependent_vregs_, vregs_to_kill_);

  vreg_chains_.RemoveTrailingNops();
  return true;
}

bool GvnDeadCodeElimination::RecordMIR(MIR* mir) {
  bool must_keep = false;
  bool uses_all_vregs = false;
  bool is_move = false;
  uint16_t opcode = mir->dalvikInsn.opcode;
  switch (opcode) {
    case kMirOpPhi: {
      // Determine if this Phi is merging wide regs.
      RegLocation raw_dest = gvn_->GetMirGraph()->GetRawDest(mir);
      if (raw_dest.high_word) {
        // This is the high part of a wide reg. Ignore the Phi.
        return false;
      }
      bool wide = raw_dest.wide;
      // Record the value.
      DCHECK_EQ(mir->ssa_rep->num_defs, 1);
      int s_reg = mir->ssa_rep->defs[0];
      uint16_t new_value = wide ? lvn_->GetSregValueWide(s_reg) : lvn_->GetSregValue(s_reg);

      int v_reg = mir_graph_->SRegToVReg(s_reg);
      DCHECK_EQ(vreg_chains_.CurrentValue(v_reg), kNoValue);  // No previous def for v_reg.
      if (wide) {
        DCHECK_EQ(vreg_chains_.CurrentValue(v_reg + 1), kNoValue);
      }
      vreg_chains_.AddMIRWithDef(mir, v_reg, wide, new_value);
      return true;  // Avoid the common processing.
    }

    case kMirOpNop:
    case Instruction::NOP:
      // Don't record NOPs.
      return false;

    case kMirOpCheck:
      must_keep = true;
      uses_all_vregs = true;
      break;

    case Instruction::RETURN_VOID:
    case Instruction::RETURN:
    case Instruction::RETURN_OBJECT:
    case Instruction::RETURN_WIDE:
    case Instruction::GOTO:
    case Instruction::GOTO_16:
    case Instruction::GOTO_32:
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
      must_keep = true;
      uses_all_vregs = true;  // Keep the implicit dependencies on all vregs.
      break;

    case Instruction::CONST_CLASS:
    case Instruction::CONST_STRING:
    case Instruction::CONST_STRING_JUMBO:
      // NOTE: While we're currently treating CONST_CLASS, CONST_STRING and CONST_STRING_JUMBO
      // as throwing but we could conceivably try and eliminate those exceptions if we're
      // retrieving the class/string repeatedly.
      must_keep = true;
      uses_all_vregs = true;
      break;

    case Instruction::MONITOR_ENTER:
    case Instruction::MONITOR_EXIT:
      // We can actually try and optimize across the acquire operation of MONITOR_ENTER,
      // the value names provided by GVN reflect the possible changes to memory visibility.
      // NOTE: In ART, MONITOR_ENTER and MONITOR_EXIT can throw only NPE.
      must_keep = true;
      uses_all_vregs = (mir->optimization_flags & MIR_IGNORE_NULL_CHECK) == 0;
      break;

    case Instruction::INVOKE_DIRECT:
    case Instruction::INVOKE_DIRECT_RANGE:
    case Instruction::INVOKE_VIRTUAL:
    case Instruction::INVOKE_VIRTUAL_RANGE:
    case Instruction::INVOKE_SUPER:
    case Instruction::INVOKE_SUPER_RANGE:
    case Instruction::INVOKE_INTERFACE:
    case Instruction::INVOKE_INTERFACE_RANGE:
    case Instruction::INVOKE_STATIC:
    case Instruction::INVOKE_STATIC_RANGE:
    case Instruction::THROW:
    case Instruction::FILLED_NEW_ARRAY:
    case Instruction::FILLED_NEW_ARRAY_RANGE:
    case Instruction::FILL_ARRAY_DATA:
      must_keep = true;
      uses_all_vregs = true;
      break;

    case Instruction::NEW_INSTANCE:
    case Instruction::NEW_ARRAY:
      must_keep = true;
      uses_all_vregs = true;
      break;

    case Instruction::CHECK_CAST:
      DCHECK_EQ(mir->ssa_rep->num_uses, 1);
      must_keep = true;  // Keep for type information even if MIR_IGNORE_CHECK_CAST.
      uses_all_vregs = (mir->optimization_flags & MIR_IGNORE_CHECK_CAST) == 0;
      break;

    case kMirOpNullCheck:
      DCHECK_EQ(mir->ssa_rep->num_uses, 1);
      if ((mir->optimization_flags & MIR_IGNORE_NULL_CHECK) != 0) {
        mir->ssa_rep->num_uses = 0;
        mir->dalvikInsn.opcode = static_cast<Instruction::Code>(kMirOpNop);
        return false;
      }
      must_keep = true;
      uses_all_vregs = true;
      break;

    case Instruction::MOVE_RESULT:
    case Instruction::MOVE_RESULT_OBJECT:
    case Instruction::MOVE_RESULT_WIDE:
      break;

    case Instruction::INSTANCE_OF:
      break;

    case Instruction::MOVE_EXCEPTION:
      must_keep = true;
      break;

    case kMirOpCopy:
    case Instruction::MOVE:
    case Instruction::MOVE_FROM16:
    case Instruction::MOVE_16:
    case Instruction::MOVE_WIDE:
    case Instruction::MOVE_WIDE_FROM16:
    case Instruction::MOVE_WIDE_16:
    case Instruction::MOVE_OBJECT:
    case Instruction::MOVE_OBJECT_FROM16:
    case Instruction::MOVE_OBJECT_16: {
      is_move = true;
      // If the MIR defining src vreg is known, allow renaming all uses of src vreg to dest vreg
      // while updating the defining MIR to directly define dest vreg. However, changing Phi's
      // def this way doesn't work without changing MIRs in other BBs.
      int src_v_reg = mir_graph_->SRegToVReg(mir->ssa_rep->uses[0]);
      int src_change = vreg_chains_.LastChange(src_v_reg);
      if (src_change != kNPos) {
        MIRData* src_data = vreg_chains_.GetMIRData(src_change);
        if (static_cast<int>(src_data->mir->dalvikInsn.opcode) != kMirOpPhi) {
          src_data->is_move_src = true;
        }
      }
      break;
    }

    case Instruction::CONST_4:
    case Instruction::CONST_16:
    case Instruction::CONST:
    case Instruction::CONST_HIGH16:
    case Instruction::CONST_WIDE_16:
    case Instruction::CONST_WIDE_32:
    case Instruction::CONST_WIDE:
    case Instruction::CONST_WIDE_HIGH16:
    case Instruction::CMPL_FLOAT:
    case Instruction::CMPG_FLOAT:
    case Instruction::CMPL_DOUBLE:
    case Instruction::CMPG_DOUBLE:
    case Instruction::CMP_LONG:
    case Instruction::NEG_INT:
    case Instruction::NOT_INT:
    case Instruction::NEG_LONG:
    case Instruction::NOT_LONG:
    case Instruction::NEG_FLOAT:
    case Instruction::NEG_DOUBLE:
    case Instruction::INT_TO_LONG:
    case Instruction::INT_TO_FLOAT:
    case Instruction::INT_TO_DOUBLE:
    case Instruction::LONG_TO_INT:
    case Instruction::LONG_TO_FLOAT:
    case Instruction::LONG_TO_DOUBLE:
    case Instruction::FLOAT_TO_INT:
    case Instruction::FLOAT_TO_LONG:
    case Instruction::FLOAT_TO_DOUBLE:
    case Instruction::DOUBLE_TO_INT:
    case Instruction::DOUBLE_TO_LONG:
    case Instruction::DOUBLE_TO_FLOAT:
    case Instruction::INT_TO_BYTE:
    case Instruction::INT_TO_CHAR:
    case Instruction::INT_TO_SHORT:
    case Instruction::ADD_INT:
    case Instruction::SUB_INT:
    case Instruction::MUL_INT:
    case Instruction::AND_INT:
    case Instruction::OR_INT:
    case Instruction::XOR_INT:
    case Instruction::SHL_INT:
    case Instruction::SHR_INT:
    case Instruction::USHR_INT:
    case Instruction::ADD_LONG:
    case Instruction::SUB_LONG:
    case Instruction::MUL_LONG:
    case Instruction::AND_LONG:
    case Instruction::OR_LONG:
    case Instruction::XOR_LONG:
    case Instruction::SHL_LONG:
    case Instruction::SHR_LONG:
    case Instruction::USHR_LONG:
    case Instruction::ADD_FLOAT:
    case Instruction::SUB_FLOAT:
    case Instruction::MUL_FLOAT:
    case Instruction::DIV_FLOAT:
    case Instruction::REM_FLOAT:
    case Instruction::ADD_DOUBLE:
    case Instruction::SUB_DOUBLE:
    case Instruction::MUL_DOUBLE:
    case Instruction::DIV_DOUBLE:
    case Instruction::REM_DOUBLE:
    case Instruction::ADD_INT_2ADDR:
    case Instruction::SUB_INT_2ADDR:
    case Instruction::MUL_INT_2ADDR:
    case Instruction::AND_INT_2ADDR:
    case Instruction::OR_INT_2ADDR:
    case Instruction::XOR_INT_2ADDR:
    case Instruction::SHL_INT_2ADDR:
    case Instruction::SHR_INT_2ADDR:
    case Instruction::USHR_INT_2ADDR:
    case Instruction::ADD_LONG_2ADDR:
    case Instruction::SUB_LONG_2ADDR:
    case Instruction::MUL_LONG_2ADDR:
    case Instruction::AND_LONG_2ADDR:
    case Instruction::OR_LONG_2ADDR:
    case Instruction::XOR_LONG_2ADDR:
    case Instruction::SHL_LONG_2ADDR:
    case Instruction::SHR_LONG_2ADDR:
    case Instruction::USHR_LONG_2ADDR:
    case Instruction::ADD_FLOAT_2ADDR:
    case Instruction::SUB_FLOAT_2ADDR:
    case Instruction::MUL_FLOAT_2ADDR:
    case Instruction::DIV_FLOAT_2ADDR:
    case Instruction::REM_FLOAT_2ADDR:
    case Instruction::ADD_DOUBLE_2ADDR:
    case Instruction::SUB_DOUBLE_2ADDR:
    case Instruction::MUL_DOUBLE_2ADDR:
    case Instruction::DIV_DOUBLE_2ADDR:
    case Instruction::REM_DOUBLE_2ADDR:
    case Instruction::ADD_INT_LIT16:
    case Instruction::RSUB_INT:
    case Instruction::MUL_INT_LIT16:
    case Instruction::AND_INT_LIT16:
    case Instruction::OR_INT_LIT16:
    case Instruction::XOR_INT_LIT16:
    case Instruction::ADD_INT_LIT8:
    case Instruction::RSUB_INT_LIT8:
    case Instruction::MUL_INT_LIT8:
    case Instruction::AND_INT_LIT8:
    case Instruction::OR_INT_LIT8:
    case Instruction::XOR_INT_LIT8:
    case Instruction::SHL_INT_LIT8:
    case Instruction::SHR_INT_LIT8:
    case Instruction::USHR_INT_LIT8:
      break;

    case Instruction::DIV_INT:
    case Instruction::REM_INT:
    case Instruction::DIV_LONG:
    case Instruction::REM_LONG:
    case Instruction::DIV_INT_2ADDR:
    case Instruction::REM_INT_2ADDR:
    case Instruction::DIV_LONG_2ADDR:
    case Instruction::REM_LONG_2ADDR:
      if ((mir->optimization_flags & MIR_IGNORE_DIV_ZERO_CHECK) == 0) {
        must_keep = true;
        uses_all_vregs = true;
      }
      break;

    case Instruction::DIV_INT_LIT16:
    case Instruction::REM_INT_LIT16:
    case Instruction::DIV_INT_LIT8:
    case Instruction::REM_INT_LIT8:
      if (mir->dalvikInsn.vC == 0) {  // Explicit division by 0?
        must_keep = true;
        uses_all_vregs = true;
      }
      break;

    case Instruction::ARRAY_LENGTH:
      if ((mir->optimization_flags & MIR_IGNORE_NULL_CHECK) == 0) {
        must_keep = true;
        uses_all_vregs = true;
      }
      break;

    case Instruction::AGET_OBJECT:
    case Instruction::AGET:
    case Instruction::AGET_WIDE:
    case Instruction::AGET_BOOLEAN:
    case Instruction::AGET_BYTE:
    case Instruction::AGET_CHAR:
    case Instruction::AGET_SHORT:
      if ((mir->optimization_flags & MIR_IGNORE_NULL_CHECK) == 0 ||
          (mir->optimization_flags & MIR_IGNORE_RANGE_CHECK) == 0) {
        must_keep = true;
        uses_all_vregs = true;
      }
      break;

    case Instruction::APUT_OBJECT:
    case Instruction::APUT:
    case Instruction::APUT_WIDE:
    case Instruction::APUT_BYTE:
    case Instruction::APUT_BOOLEAN:
    case Instruction::APUT_SHORT:
    case Instruction::APUT_CHAR:
      must_keep = true;
      if ((mir->optimization_flags & MIR_IGNORE_NULL_CHECK) == 0 ||
          (mir->optimization_flags & MIR_IGNORE_RANGE_CHECK) == 0) {
        uses_all_vregs = true;
      }
      break;

    case Instruction::IGET_OBJECT:
    case Instruction::IGET:
    case Instruction::IGET_WIDE:
    case Instruction::IGET_BOOLEAN:
    case Instruction::IGET_BYTE:
    case Instruction::IGET_CHAR:
    case Instruction::IGET_SHORT: {
      const MirIFieldLoweringInfo& info = mir_graph_->GetIFieldLoweringInfo(mir);
      if ((mir->optimization_flags & MIR_IGNORE_NULL_CHECK) == 0 ||
          !info.IsResolved() || !info.FastGet()) {
        must_keep = true;
        uses_all_vregs = true;
      } else if (info.IsVolatile()) {
        must_keep = true;
      }
      break;
    }

    case Instruction::IPUT_OBJECT:
    case Instruction::IPUT:
    case Instruction::IPUT_WIDE:
    case Instruction::IPUT_BOOLEAN:
    case Instruction::IPUT_BYTE:
    case Instruction::IPUT_CHAR:
    case Instruction::IPUT_SHORT: {
      must_keep = true;
      const MirIFieldLoweringInfo& info = mir_graph_->GetIFieldLoweringInfo(mir);
      if ((mir->optimization_flags & MIR_IGNORE_NULL_CHECK) == 0 ||
          !info.IsResolved() || !info.FastPut()) {
        uses_all_vregs = true;
      }
      break;
    }

    case Instruction::SGET_OBJECT:
    case Instruction::SGET:
    case Instruction::SGET_WIDE:
    case Instruction::SGET_BOOLEAN:
    case Instruction::SGET_BYTE:
    case Instruction::SGET_CHAR:
    case Instruction::SGET_SHORT: {
      const MirSFieldLoweringInfo& info = mir_graph_->GetSFieldLoweringInfo(mir);
      if ((mir->optimization_flags & MIR_CLASS_IS_INITIALIZED) == 0 ||
          !info.IsResolved() || !info.FastGet()) {
        must_keep = true;
        uses_all_vregs = true;
      } else if (info.IsVolatile()) {
        must_keep = true;
      }
      break;
    }

    case Instruction::SPUT_OBJECT:
    case Instruction::SPUT:
    case Instruction::SPUT_WIDE:
    case Instruction::SPUT_BOOLEAN:
    case Instruction::SPUT_BYTE:
    case Instruction::SPUT_CHAR:
    case Instruction::SPUT_SHORT: {
      must_keep = true;
      const MirSFieldLoweringInfo& info = mir_graph_->GetSFieldLoweringInfo(mir);
      if ((mir->optimization_flags & MIR_CLASS_IS_INITIALIZED) == 0 ||
          !info.IsResolved() || !info.FastPut()) {
        uses_all_vregs = true;
      }
      break;
    }

    default:
      LOG(FATAL) << "Unexpected opcode: " << opcode;
      UNREACHABLE();
  }

  if (mir->ssa_rep->num_defs != 0) {
    DCHECK(mir->ssa_rep->num_defs == 1 || mir->ssa_rep->num_defs == 2);
    bool wide = (mir->ssa_rep->num_defs == 2);
    int s_reg = mir->ssa_rep->defs[0];
    int v_reg = mir_graph_->SRegToVReg(s_reg);
    uint16_t new_value = wide ? lvn_->GetSregValueWide(s_reg) : lvn_->GetSregValue(s_reg);
    DCHECK_NE(new_value, kNoValue);

    vreg_chains_.UpdateInitialVRegValue(v_reg, wide, lvn_);
    vreg_chains_.AddMIRWithDef(mir, v_reg, wide, new_value);
    if (is_move) {
      // Allow renaming all uses of dest vreg to src vreg.
      vreg_chains_.LastMIRData()->is_move = true;
    }
  } else {
    vreg_chains_.AddMIRWithoutDef(mir);
    DCHECK(!is_move) << opcode;
  }

  if (must_keep) {
    MIRData* last_data = vreg_chains_.LastMIRData();
    last_data->must_keep = true;
    if (uses_all_vregs) {
      last_data->uses_all_vregs = true;
      no_uses_all_since_ = vreg_chains_.NumMIRs();
    }
  } else {
    DCHECK_NE(mir->ssa_rep->num_defs, 0) << opcode;
    DCHECK(!uses_all_vregs) << opcode;
  }
  return true;
}

}  // namespace art
