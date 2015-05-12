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

#ifndef ART_COMPILER_DEX_GVN_DEAD_CODE_ELIMINATION_H_
#define ART_COMPILER_DEX_GVN_DEAD_CODE_ELIMINATION_H_

#include "base/arena_object.h"
#include "base/scoped_arena_containers.h"
#include "global_value_numbering.h"

namespace art {

class ArenaBitVector;
class BasicBlock;
class LocalValueNumbering;
class MIR;
class MIRGraph;

/**
 * @class DeadCodeElimination
 * @details Eliminate dead code based on the results of global value numbering.
 * Also get rid of MOVE insns when we can use the source instead of destination
 * without affecting the vreg values at safepoints; this is useful in methods
 * with a large number of vregs that frequently move values to and from low vregs
 * to accommodate insns that can work only with the low 16 or 256 vregs.
 */
class GvnDeadCodeElimination : public DeletableArenaObject<kArenaAllocMisc> {
 public:
  GvnDeadCodeElimination(const GlobalValueNumbering* gvn, ScopedArenaAllocator* alloc);

  // Apply the DCE to a basic block.
  void Apply(BasicBlock* bb);

 private:
  static constexpr uint16_t kNoValue = GlobalValueNumbering::kNoValue;
  static constexpr uint16_t kNPos = 0xffffu;
  static constexpr size_t kMaxNumTopChangesToKill = 2;

  struct VRegValue {
    VRegValue() : value(kNoValue), change(kNPos) { }

    // Value name as reported by GVN, kNoValue if not available.
    uint16_t value;
    // Index of the change in mir_data_ that defined the value, kNPos if initial value for the BB.
    uint16_t change;
  };

  struct MIRData {
    explicit MIRData(MIR* m)
        : mir(m), uses_all_vregs(false), must_keep(false), is_move(false), is_move_src(false),
          has_def(false), wide_def(false),
          low_def_over_high_word(false), high_def_over_low_word(false), vreg_def(0u),
          prev_value(), prev_value_high() {
    }

    uint16_t PrevChange(int v_reg) const;
    void SetPrevChange(int v_reg, uint16_t change);
    void RemovePrevChange(int v_reg, MIRData* prev_data);

    MIR* mir;
    bool uses_all_vregs : 1;  // If mir uses all vregs, uses in mir->ssa_rep are irrelevant.
    bool must_keep : 1;
    bool is_move : 1;
    bool is_move_src : 1;
    bool has_def : 1;
    bool wide_def : 1;
    bool low_def_over_high_word : 1;
    bool high_def_over_low_word : 1;
    uint16_t vreg_def;
    VRegValue prev_value;
    VRegValue prev_value_high;   // For wide defs.
  };

  class VRegChains {
   public:
    VRegChains(uint32_t num_vregs, ScopedArenaAllocator* alloc);

    void Reset();

    void AddMIRWithDef(MIR* mir, int v_reg, bool wide, uint16_t new_value);
    void AddMIRWithoutDef(MIR* mir);
    void RemoveLastMIRData();
    void RemoveTrailingNops();

    size_t NumMIRs() const;
    MIRData* GetMIRData(size_t pos);
    MIRData* LastMIRData();

    uint32_t NumVRegs() const;
    void InsertInitialValueHigh(int v_reg, uint16_t value);
    void UpdateInitialVRegValue(int v_reg, bool wide, const LocalValueNumbering* lvn);
    uint16_t LastChange(int v_reg);
    uint16_t CurrentValue(int v_reg);

    uint16_t FindKillHead(int v_reg, uint16_t cutoff);
    uint16_t FindFirstChangeAfter(int v_reg, uint16_t change) const;
    void ReplaceChange(uint16_t old_change, uint16_t new_change);
    void RemoveChange(uint16_t change);
    bool IsTopChange(uint16_t change) const;
    bool IsSRegUsed(uint16_t first_change, uint16_t last_change, int s_reg) const;
    bool IsVRegUsed(uint16_t first_change, uint16_t last_change, int v_reg,
                    MIRGraph* mir_graph) const;
    void RenameSRegUses(uint16_t first_change, uint16_t last_change,
                        int old_s_reg, int new_s_reg, bool wide);
    void RenameVRegUses(uint16_t first_change, uint16_t last_change,
                        int old_s_reg, int old_v_reg, int new_s_reg, int new_v_reg);

   private:
    const uint32_t num_vregs_;
    VRegValue* const vreg_data_;
    BitVector vreg_high_words_;
    ScopedArenaVector<MIRData> mir_data_;
  };

  void RecordPass();
  void BackwardPass();

  void KillMIR(MIRData* data);
  static void KillMIR(MIR* mir);
  static void ChangeBinOp2AddrToPlainBinOp(MIR* mir);
  MIR* CreatePhi(int s_reg);
  MIR* RenameSRegDefOrCreatePhi(uint16_t def_change, uint16_t last_change, MIR* mir_to_kill);

  // Update state variables going backwards through a MIR.
  void BackwardPassProcessLastMIR();

  uint16_t FindChangesToKill(uint16_t first_change, uint16_t last_change);
  void BackwardPassTryToKillRevertVRegs();
  bool BackwardPassTryToKillLastMIR();

  void RecordPassKillMoveByRenamingSrcDef(uint16_t src_change, uint16_t move_change);
  void RecordPassTryToKillOverwrittenMoveOrMoveSrc(uint16_t check_change);
  void RecordPassTryToKillOverwrittenMoveOrMoveSrc();
  void RecordPassTryToKillLastMIR();

  bool RecordMIR(MIR* mir);

  const GlobalValueNumbering* const gvn_;
  MIRGraph* const mir_graph_;

  VRegChains vreg_chains_;
  BasicBlock* bb_;
  const LocalValueNumbering* lvn_;
  size_t no_uses_all_since_;  // The change index after the last change with uses_all_vregs set.

  // Data used when processing MIRs in reverse order.
  ArenaBitVector* unused_vregs_;              // vregs that are not needed later.
  ArenaBitVector* vregs_to_kill_;             // vregs that revert to a previous value.
  uint16_t* kill_heads_;  // For each vreg in vregs_to_kill_, the first change to kill.
  ScopedArenaVector<uint16_t> changes_to_kill_;
  ArenaBitVector* dependent_vregs_;
};

}  // namespace art

#endif  // ART_COMPILER_DEX_GVN_DEAD_CODE_ELIMINATION_H_
