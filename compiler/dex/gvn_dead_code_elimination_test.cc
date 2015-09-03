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

#include "dataflow_iterator-inl.h"
#include "dex/mir_field_info.h"
#include "global_value_numbering.h"
#include "gvn_dead_code_elimination.h"
#include "local_value_numbering.h"
#include "gtest/gtest.h"

namespace art {

class GvnDeadCodeEliminationTest : public testing::Test {
 protected:
  static constexpr uint16_t kNoValue = GlobalValueNumbering::kNoValue;

  struct IFieldDef {
    uint16_t field_idx;
    uintptr_t declaring_dex_file;
    uint16_t declaring_field_idx;
    bool is_volatile;
    DexMemAccessType type;
  };

  struct SFieldDef {
    uint16_t field_idx;
    uintptr_t declaring_dex_file;
    uint16_t declaring_field_idx;
    bool is_volatile;
    DexMemAccessType type;
  };

  struct BBDef {
    static constexpr size_t kMaxSuccessors = 4;
    static constexpr size_t kMaxPredecessors = 4;

    BBType type;
    size_t num_successors;
    BasicBlockId successors[kMaxPredecessors];
    size_t num_predecessors;
    BasicBlockId predecessors[kMaxPredecessors];
  };

  struct MIRDef {
    static constexpr size_t kMaxSsaDefs = 2;
    static constexpr size_t kMaxSsaUses = 4;

    BasicBlockId bbid;
    Instruction::Code opcode;
    int64_t value;
    uint32_t field_info;
    size_t num_uses;
    int32_t uses[kMaxSsaUses];
    size_t num_defs;
    int32_t defs[kMaxSsaDefs];
  };

#define DEF_SUCC0() \
    0u, { }
#define DEF_SUCC1(s1) \
    1u, { s1 }
#define DEF_SUCC2(s1, s2) \
    2u, { s1, s2 }
#define DEF_SUCC3(s1, s2, s3) \
    3u, { s1, s2, s3 }
#define DEF_SUCC4(s1, s2, s3, s4) \
    4u, { s1, s2, s3, s4 }
#define DEF_PRED0() \
    0u, { }
#define DEF_PRED1(p1) \
    1u, { p1 }
#define DEF_PRED2(p1, p2) \
    2u, { p1, p2 }
#define DEF_PRED3(p1, p2, p3) \
    3u, { p1, p2, p3 }
#define DEF_PRED4(p1, p2, p3, p4) \
    4u, { p1, p2, p3, p4 }
#define DEF_BB(type, succ, pred) \
    { type, succ, pred }

#define DEF_CONST(bb, opcode, reg, value) \
    { bb, opcode, value, 0u, 0, { }, 1, { reg } }
#define DEF_CONST_WIDE(bb, opcode, reg, value) \
    { bb, opcode, value, 0u, 0, { }, 2, { reg, reg + 1 } }
#define DEF_CONST_STRING(bb, opcode, reg, index) \
    { bb, opcode, index, 0u, 0, { }, 1, { reg } }
#define DEF_IGET(bb, opcode, reg, obj, field_info) \
    { bb, opcode, 0u, field_info, 1, { obj }, 1, { reg } }
#define DEF_IGET_WIDE(bb, opcode, reg, obj, field_info) \
    { bb, opcode, 0u, field_info, 1, { obj }, 2, { reg, reg + 1 } }
#define DEF_IPUT(bb, opcode, reg, obj, field_info) \
    { bb, opcode, 0u, field_info, 2, { reg, obj }, 0, { } }
#define DEF_IPUT_WIDE(bb, opcode, reg, obj, field_info) \
    { bb, opcode, 0u, field_info, 3, { reg, reg + 1, obj }, 0, { } }
#define DEF_SGET(bb, opcode, reg, field_info) \
    { bb, opcode, 0u, field_info, 0, { }, 1, { reg } }
#define DEF_SGET_WIDE(bb, opcode, reg, field_info) \
    { bb, opcode, 0u, field_info, 0, { }, 2, { reg, reg + 1 } }
#define DEF_SPUT(bb, opcode, reg, field_info) \
    { bb, opcode, 0u, field_info, 1, { reg }, 0, { } }
#define DEF_SPUT_WIDE(bb, opcode, reg, field_info) \
    { bb, opcode, 0u, field_info, 2, { reg, reg + 1 }, 0, { } }
#define DEF_AGET(bb, opcode, reg, obj, idx) \
    { bb, opcode, 0u, 0u, 2, { obj, idx }, 1, { reg } }
#define DEF_AGET_WIDE(bb, opcode, reg, obj, idx) \
    { bb, opcode, 0u, 0u, 2, { obj, idx }, 2, { reg, reg + 1 } }
#define DEF_APUT(bb, opcode, reg, obj, idx) \
    { bb, opcode, 0u, 0u, 3, { reg, obj, idx }, 0, { } }
#define DEF_APUT_WIDE(bb, opcode, reg, obj, idx) \
    { bb, opcode, 0u, 0u, 4, { reg, reg + 1, obj, idx }, 0, { } }
#define DEF_INVOKE1(bb, opcode, reg) \
    { bb, opcode, 0u, 0u, 1, { reg }, 0, { } }
#define DEF_UNIQUE_REF(bb, opcode, reg) \
    { bb, opcode, 0u, 0u, 0, { }, 1, { reg } }  // CONST_CLASS, CONST_STRING, NEW_ARRAY, ...
#define DEF_IFZ(bb, opcode, reg) \
    { bb, opcode, 0u, 0u, 1, { reg }, 0, { } }
#define DEF_MOVE(bb, opcode, reg, src) \
    { bb, opcode, 0u, 0u, 1, { src }, 1, { reg } }
#define DEF_MOVE_WIDE(bb, opcode, reg, src) \
    { bb, opcode, 0u, 0u, 2, { src, src + 1 }, 2, { reg, reg + 1 } }
#define DEF_PHI2(bb, reg, src1, src2) \
    { bb, static_cast<Instruction::Code>(kMirOpPhi), 0, 0u, 2u, { src1, src2 }, 1, { reg } }
#define DEF_UNOP(bb, opcode, result, src1) \
    { bb, opcode, 0u, 0u, 1, { src1 }, 1, { result } }
#define DEF_BINOP(bb, opcode, result, src1, src2) \
    { bb, opcode, 0u, 0u, 2, { src1, src2 }, 1, { result } }
#define DEF_BINOP_WIDE(bb, opcode, result, src1, src2) \
    { bb, opcode, 0u, 0u, 4, { src1, src1 + 1, src2, src2 + 1 }, 2, { result, result + 1 } }

  void DoPrepareIFields(const IFieldDef* defs, size_t count) {
    cu_.mir_graph->ifield_lowering_infos_.clear();
    cu_.mir_graph->ifield_lowering_infos_.reserve(count);
    for (size_t i = 0u; i != count; ++i) {
      const IFieldDef* def = &defs[i];
      MirIFieldLoweringInfo field_info(def->field_idx, def->type, false);
      if (def->declaring_dex_file != 0u) {
        field_info.declaring_dex_file_ = reinterpret_cast<const DexFile*>(def->declaring_dex_file);
        field_info.declaring_field_idx_ = def->declaring_field_idx;
        field_info.flags_ =
            MirIFieldLoweringInfo::kFlagFastGet | MirIFieldLoweringInfo::kFlagFastPut |
            (field_info.flags_ & ~(def->is_volatile ? 0u : MirIFieldLoweringInfo::kFlagIsVolatile));
      }
      cu_.mir_graph->ifield_lowering_infos_.push_back(field_info);
    }
  }

  template <size_t count>
  void PrepareIFields(const IFieldDef (&defs)[count]) {
    DoPrepareIFields(defs, count);
  }

  void DoPrepareSFields(const SFieldDef* defs, size_t count) {
    cu_.mir_graph->sfield_lowering_infos_.clear();
    cu_.mir_graph->sfield_lowering_infos_.reserve(count);
    for (size_t i = 0u; i != count; ++i) {
      const SFieldDef* def = &defs[i];
      MirSFieldLoweringInfo field_info(def->field_idx, def->type);
      // Mark even unresolved fields as initialized.
      field_info.flags_ |= MirSFieldLoweringInfo::kFlagClassIsInitialized;
      // NOTE: MirSFieldLoweringInfo::kFlagClassIsInDexCache isn't used by GVN.
      if (def->declaring_dex_file != 0u) {
        field_info.declaring_dex_file_ = reinterpret_cast<const DexFile*>(def->declaring_dex_file);
        field_info.declaring_field_idx_ = def->declaring_field_idx;
        field_info.flags_ =
            MirSFieldLoweringInfo::kFlagFastGet | MirSFieldLoweringInfo::kFlagFastPut |
            (field_info.flags_ & ~(def->is_volatile ? 0u : MirSFieldLoweringInfo::kFlagIsVolatile));
      }
      cu_.mir_graph->sfield_lowering_infos_.push_back(field_info);
    }
  }

  template <size_t count>
  void PrepareSFields(const SFieldDef (&defs)[count]) {
    DoPrepareSFields(defs, count);
  }

  void DoPrepareBasicBlocks(const BBDef* defs, size_t count) {
    cu_.mir_graph->block_id_map_.clear();
    cu_.mir_graph->block_list_.clear();
    ASSERT_LT(3u, count);  // null, entry, exit and at least one bytecode block.
    ASSERT_EQ(kNullBlock, defs[0].type);
    ASSERT_EQ(kEntryBlock, defs[1].type);
    ASSERT_EQ(kExitBlock, defs[2].type);
    for (size_t i = 0u; i != count; ++i) {
      const BBDef* def = &defs[i];
      BasicBlock* bb = cu_.mir_graph->CreateNewBB(def->type);
      if (def->num_successors <= 2) {
        bb->successor_block_list_type = kNotUsed;
        bb->fall_through = (def->num_successors >= 1) ? def->successors[0] : 0u;
        bb->taken = (def->num_successors >= 2) ? def->successors[1] : 0u;
      } else {
        bb->successor_block_list_type = kPackedSwitch;
        bb->fall_through = 0u;
        bb->taken = 0u;
        bb->successor_blocks.reserve(def->num_successors);
        for (size_t j = 0u; j != def->num_successors; ++j) {
          SuccessorBlockInfo* successor_block_info =
              static_cast<SuccessorBlockInfo*>(cu_.arena.Alloc(sizeof(SuccessorBlockInfo),
                                                               kArenaAllocSuccessor));
          successor_block_info->block = j;
          successor_block_info->key = 0u;  // Not used by class init check elimination.
          bb->successor_blocks.push_back(successor_block_info);
        }
      }
      bb->predecessors.assign(def->predecessors, def->predecessors + def->num_predecessors);
      if (def->type == kDalvikByteCode || def->type == kEntryBlock || def->type == kExitBlock) {
        bb->data_flow_info = static_cast<BasicBlockDataFlow*>(
            cu_.arena.Alloc(sizeof(BasicBlockDataFlow), kArenaAllocDFInfo));
        bb->data_flow_info->live_in_v = live_in_v_;
        bb->data_flow_info->vreg_to_ssa_map_exit = nullptr;
      }
    }
    ASSERT_EQ(count, cu_.mir_graph->block_list_.size());
    cu_.mir_graph->entry_block_ = cu_.mir_graph->block_list_[1];
    ASSERT_EQ(kEntryBlock, cu_.mir_graph->entry_block_->block_type);
    cu_.mir_graph->exit_block_ = cu_.mir_graph->block_list_[2];
    ASSERT_EQ(kExitBlock, cu_.mir_graph->exit_block_->block_type);
  }

  template <size_t count>
  void PrepareBasicBlocks(const BBDef (&defs)[count]) {
    DoPrepareBasicBlocks(defs, count);
  }

  int SRegToVReg(int32_t s_reg, bool wide) {
    int v_reg = cu_.mir_graph->SRegToVReg(s_reg);
    CHECK_LT(static_cast<size_t>(v_reg), num_vregs_);
    if (wide) {
      CHECK_LT(static_cast<size_t>(v_reg + 1), num_vregs_);
    }
    return v_reg;
  }

  int SRegToVReg(int32_t* uses, size_t* use, bool wide) {
    int v_reg = SRegToVReg(uses[*use], wide);
    if (wide) {
      CHECK_EQ(uses[*use] + 1, uses[*use + 1]);
      *use += 2u;
    } else {
      *use += 1u;
    }
    return v_reg;
  }

  void DoPrepareMIRs(const MIRDef* defs, size_t count) {
    mir_count_ = count;
    mirs_ = reinterpret_cast<MIR*>(cu_.arena.Alloc(sizeof(MIR) * count, kArenaAllocMIR));
    ssa_reps_.resize(count);
    for (size_t i = 0u; i != count; ++i) {
      const MIRDef* def = &defs[i];
      MIR* mir = &mirs_[i];
      ASSERT_LT(def->bbid, cu_.mir_graph->block_list_.size());
      BasicBlock* bb = cu_.mir_graph->block_list_[def->bbid];
      bb->AppendMIR(mir);
      mir->dalvikInsn.opcode = def->opcode;
      mir->dalvikInsn.vB = static_cast<int32_t>(def->value);
      mir->dalvikInsn.vB_wide = def->value;
      if (IsInstructionIGetOrIPut(def->opcode)) {
        ASSERT_LT(def->field_info, cu_.mir_graph->ifield_lowering_infos_.size());
        mir->meta.ifield_lowering_info = def->field_info;
        ASSERT_EQ(cu_.mir_graph->ifield_lowering_infos_[def->field_info].MemAccessType(),
                  IGetOrIPutMemAccessType(def->opcode));
      } else if (IsInstructionSGetOrSPut(def->opcode)) {
        ASSERT_LT(def->field_info, cu_.mir_graph->sfield_lowering_infos_.size());
        mir->meta.sfield_lowering_info = def->field_info;
        ASSERT_EQ(cu_.mir_graph->sfield_lowering_infos_[def->field_info].MemAccessType(),
                  SGetOrSPutMemAccessType(def->opcode));
      } else if (def->opcode == static_cast<Instruction::Code>(kMirOpPhi)) {
        mir->meta.phi_incoming =
            allocator_->AllocArray<BasicBlockId>(def->num_uses, kArenaAllocDFInfo);
        ASSERT_EQ(def->num_uses, bb->predecessors.size());
        std::copy(bb->predecessors.begin(), bb->predecessors.end(), mir->meta.phi_incoming);
      }
      mir->ssa_rep = &ssa_reps_[i];
      cu_.mir_graph->AllocateSSAUseData(mir, def->num_uses);
      std::copy_n(def->uses, def->num_uses, mir->ssa_rep->uses);
      // Keep mir->ssa_rep->fp_use[.] zero-initialized (false). Not used by DCE, only copied.
      cu_.mir_graph->AllocateSSADefData(mir, def->num_defs);
      std::copy_n(def->defs, def->num_defs, mir->ssa_rep->defs);
      // Keep mir->ssa_rep->fp_def[.] zero-initialized (false). Not used by DCE, only copied.
      mir->dalvikInsn.opcode = def->opcode;
      mir->offset = i;  // LVN uses offset only for debug output
      mir->optimization_flags = 0u;
      uint64_t df_attrs = MIRGraph::GetDataFlowAttributes(mir);
      if ((df_attrs & DF_DA) != 0) {
        CHECK_NE(def->num_defs, 0u);
        mir->dalvikInsn.vA = SRegToVReg(def->defs[0], (df_attrs & DF_A_WIDE) != 0);
        bb->data_flow_info->vreg_to_ssa_map_exit[mir->dalvikInsn.vA] = def->defs[0];
        if ((df_attrs & DF_A_WIDE) != 0) {
          CHECK_EQ(def->defs[0] + 1, def->defs[1]);
          bb->data_flow_info->vreg_to_ssa_map_exit[mir->dalvikInsn.vA + 1u] = def->defs[0] + 1;
        }
      }
      if ((df_attrs & (DF_UA | DF_UB | DF_UC)) != 0) {
        size_t use = 0;
        if ((df_attrs & DF_UA) != 0) {
          mir->dalvikInsn.vA = SRegToVReg(mir->ssa_rep->uses, &use, (df_attrs & DF_A_WIDE) != 0);
        }
        if ((df_attrs & DF_UB) != 0) {
          mir->dalvikInsn.vB = SRegToVReg(mir->ssa_rep->uses, &use, (df_attrs & DF_B_WIDE) != 0);
        }
        if ((df_attrs & DF_UC) != 0) {
          mir->dalvikInsn.vC = SRegToVReg(mir->ssa_rep->uses, &use, (df_attrs & DF_C_WIDE) != 0);
        }
        DCHECK_EQ(def->num_uses, use);
      }
    }
    DexFile::CodeItem* code_item = static_cast<DexFile::CodeItem*>(
        cu_.arena.Alloc(sizeof(DexFile::CodeItem), kArenaAllocMisc));
    code_item->insns_size_in_code_units_ = 2u * count;
    code_item->registers_size_ = kMaxVRegs;
    cu_.mir_graph->current_code_item_ = code_item;
  }

  template <size_t count>
  void PrepareMIRs(const MIRDef (&defs)[count]) {
    DoPrepareMIRs(defs, count);
  }

  template <size_t count>
  void PrepareSRegToVRegMap(const int (&map)[count]) {
    cu_.mir_graph->ssa_base_vregs_.assign(map, map + count);
    num_vregs_ = *std::max_element(map, map + count) + 1u;
    AllNodesIterator iterator(cu_.mir_graph.get());
    for (BasicBlock* bb = iterator.Next(); bb != nullptr; bb = iterator.Next()) {
      if (bb->data_flow_info != nullptr) {
        bb->data_flow_info->vreg_to_ssa_map_exit = static_cast<int32_t*>(
            cu_.arena.Alloc(sizeof(int32_t) * num_vregs_, kArenaAllocDFInfo));
        std::fill_n(bb->data_flow_info->vreg_to_ssa_map_exit, num_vregs_, INVALID_SREG);
      }
    }
  }

  void PerformGVN() {
    cu_.mir_graph->SSATransformationStart();
    cu_.mir_graph->ComputeDFSOrders();
    cu_.mir_graph->ComputeDominators();
    cu_.mir_graph->ComputeTopologicalSortOrder();
    cu_.mir_graph->SSATransformationEnd();
    cu_.mir_graph->temp_.gvn.ifield_ids =  GlobalValueNumbering::PrepareGvnFieldIds(
        allocator_.get(), cu_.mir_graph->ifield_lowering_infos_);
    cu_.mir_graph->temp_.gvn.sfield_ids =  GlobalValueNumbering::PrepareGvnFieldIds(
        allocator_.get(), cu_.mir_graph->sfield_lowering_infos_);
    ASSERT_TRUE(gvn_ == nullptr);
    gvn_.reset(new (allocator_.get()) GlobalValueNumbering(&cu_, allocator_.get(),
                                                           GlobalValueNumbering::kModeGvn));
    value_names_.resize(mir_count_, 0xffffu);
    LoopRepeatingTopologicalSortIterator iterator(cu_.mir_graph.get());
    bool change = false;
    for (BasicBlock* bb = iterator.Next(change); bb != nullptr; bb = iterator.Next(change)) {
      LocalValueNumbering* lvn = gvn_->PrepareBasicBlock(bb);
      if (lvn != nullptr) {
        for (MIR* mir = bb->first_mir_insn; mir != nullptr; mir = mir->next) {
          value_names_[mir - mirs_] = lvn->GetValueNumber(mir);
        }
      }
      change = (lvn != nullptr) && gvn_->FinishBasicBlock(bb);
      ASSERT_TRUE(gvn_->Good());
    }
  }

  void PerformGVNCodeModifications() {
    ASSERT_TRUE(gvn_ != nullptr);
    ASSERT_TRUE(gvn_->Good());
    gvn_->StartPostProcessing();
    TopologicalSortIterator iterator(cu_.mir_graph.get());
    for (BasicBlock* bb = iterator.Next(); bb != nullptr; bb = iterator.Next()) {
      LocalValueNumbering* lvn = gvn_->PrepareBasicBlock(bb);
      if (lvn != nullptr) {
        for (MIR* mir = bb->first_mir_insn; mir != nullptr; mir = mir->next) {
          uint16_t value_name = lvn->GetValueNumber(mir);
          ASSERT_EQ(value_name, value_names_[mir - mirs_]);
        }
      }
      bool change = (lvn != nullptr) && gvn_->FinishBasicBlock(bb);
      ASSERT_FALSE(change);
      ASSERT_TRUE(gvn_->Good());
    }
  }

  void FillVregToSsaRegExitMaps() {
    // Fill in vreg_to_ssa_map_exit for each BB.
    PreOrderDfsIterator iterator(cu_.mir_graph.get());
    for (BasicBlock* bb = iterator.Next(); bb != nullptr; bb = iterator.Next()) {
      if (bb->block_type == kDalvikByteCode) {
        CHECK(!bb->predecessors.empty());
        BasicBlock* pred_bb = cu_.mir_graph->GetBasicBlock(bb->predecessors[0]);
        for (size_t v_reg = 0; v_reg != num_vregs_; ++v_reg) {
          if (bb->data_flow_info->vreg_to_ssa_map_exit[v_reg] == INVALID_SREG) {
            bb->data_flow_info->vreg_to_ssa_map_exit[v_reg] =
                pred_bb->data_flow_info->vreg_to_ssa_map_exit[v_reg];
          }
        }
      }
    }
  }

  template <size_t count>
  void MarkAsWideSRegs(const int32_t (&sregs)[count]) {
    for (int32_t sreg : sregs) {
      cu_.mir_graph->reg_location_[sreg].wide = true;
      cu_.mir_graph->reg_location_[sreg + 1].wide = true;
      cu_.mir_graph->reg_location_[sreg + 1].high_word = true;
    }
  }

  void PerformDCE() {
    FillVregToSsaRegExitMaps();
    cu_.mir_graph->GetNumOfCodeAndTempVRs();
    dce_.reset(new (allocator_.get()) GvnDeadCodeElimination(gvn_.get(), allocator_.get()));
    PreOrderDfsIterator iterator(cu_.mir_graph.get());
    for (BasicBlock* bb = iterator.Next(); bb != nullptr; bb = iterator.Next()) {
      if (bb->block_type == kDalvikByteCode) {
        dce_->Apply(bb);
      }
    }
  }

  void PerformGVN_DCE() {
    PerformGVN();
    PerformGVNCodeModifications();  // Eliminate null/range checks.
    PerformDCE();
  }

  template <size_t count>
  void ExpectValueNamesNE(const size_t (&indexes)[count]) {
    for (size_t i1 = 0; i1 != count; ++i1) {
      size_t idx1 = indexes[i1];
      for (size_t i2 = i1 + 1; i2 != count; ++i2) {
        size_t idx2 = indexes[i2];
        EXPECT_NE(value_names_[idx1], value_names_[idx2]) << idx1 << " " << idx2;
      }
    }
  }

  template <size_t count>
  void ExpectNoNullCheck(const size_t (&indexes)[count]) {
    for (size_t i = 0; i != count; ++i) {
      size_t idx = indexes[i];
      EXPECT_EQ(MIR_IGNORE_NULL_CHECK, mirs_[idx].optimization_flags & MIR_IGNORE_NULL_CHECK)
          << idx;
    }
    size_t num_no_null_ck = 0u;
    for (size_t i = 0; i != mir_count_; ++i) {
      if ((mirs_[i].optimization_flags & MIR_IGNORE_NULL_CHECK) != 0) {
        ++num_no_null_ck;
      }
    }
    EXPECT_EQ(count, num_no_null_ck);
  }

  GvnDeadCodeEliminationTest()
      : pool_(),
        cu_(&pool_, kRuntimeISA, nullptr, nullptr),
        num_vregs_(0u),
        mir_count_(0u),
        mirs_(nullptr),
        ssa_reps_(),
        allocator_(),
        gvn_(),
        dce_(),
        value_names_(),
        live_in_v_(new (&cu_.arena) ArenaBitVector(&cu_.arena, kMaxSsaRegs, false, kBitMapMisc)) {
    cu_.mir_graph.reset(new MIRGraph(&cu_, &cu_.arena));
    cu_.access_flags = kAccStatic;  // Don't let "this" interfere with this test.
    allocator_.reset(ScopedArenaAllocator::Create(&cu_.arena_stack));
    // By default, the zero-initialized reg_location_[.] with ref == false tells LVN that
    // 0 constants are integral, not references, and the values are all narrow.
    // Nothing else is used by LVN/GVN. Tests can override the default values as needed.
    cu_.mir_graph->reg_location_ = static_cast<RegLocation*>(cu_.arena.Alloc(
        kMaxSsaRegs * sizeof(cu_.mir_graph->reg_location_[0]), kArenaAllocRegAlloc));
    cu_.mir_graph->num_ssa_regs_ = kMaxSsaRegs;
    // Bind all possible sregs to live vregs for test purposes.
    live_in_v_->SetInitialBits(kMaxSsaRegs);
    cu_.mir_graph->ssa_base_vregs_.reserve(kMaxSsaRegs);
    cu_.mir_graph->ssa_subscripts_.reserve(kMaxSsaRegs);
    for (unsigned int i = 0; i < kMaxSsaRegs; i++) {
      cu_.mir_graph->ssa_base_vregs_.push_back(i);
      cu_.mir_graph->ssa_subscripts_.push_back(0);
    }
    // Set shorty for a void-returning method without arguments.
    cu_.shorty = "V";
  }

  static constexpr size_t kMaxSsaRegs = 16384u;
  static constexpr size_t kMaxVRegs = 256u;

  ArenaPool pool_;
  CompilationUnit cu_;
  size_t num_vregs_;
  size_t mir_count_;
  MIR* mirs_;
  std::vector<SSARepresentation> ssa_reps_;
  std::unique_ptr<ScopedArenaAllocator> allocator_;
  std::unique_ptr<GlobalValueNumbering> gvn_;
  std::unique_ptr<GvnDeadCodeElimination> dce_;
  std::vector<uint16_t> value_names_;
  ArenaBitVector* live_in_v_;
};

constexpr uint16_t GvnDeadCodeEliminationTest::kNoValue;

class GvnDeadCodeEliminationTestSimple : public GvnDeadCodeEliminationTest {
 public:
  GvnDeadCodeEliminationTestSimple();

 private:
  static const BBDef kSimpleBbs[];
};

const GvnDeadCodeEliminationTest::BBDef GvnDeadCodeEliminationTestSimple::kSimpleBbs[] = {
    DEF_BB(kNullBlock, DEF_SUCC0(), DEF_PRED0()),
    DEF_BB(kEntryBlock, DEF_SUCC1(3), DEF_PRED0()),
    DEF_BB(kExitBlock, DEF_SUCC0(), DEF_PRED1(3)),
    DEF_BB(kDalvikByteCode, DEF_SUCC1(2), DEF_PRED1(1)),
};

GvnDeadCodeEliminationTestSimple::GvnDeadCodeEliminationTestSimple()
    : GvnDeadCodeEliminationTest() {
  PrepareBasicBlocks(kSimpleBbs);
}

class GvnDeadCodeEliminationTestDiamond : public GvnDeadCodeEliminationTest {
 public:
  GvnDeadCodeEliminationTestDiamond();

 private:
  static const BBDef kDiamondBbs[];
};

const GvnDeadCodeEliminationTest::BBDef GvnDeadCodeEliminationTestDiamond::kDiamondBbs[] = {
    DEF_BB(kNullBlock, DEF_SUCC0(), DEF_PRED0()),
    DEF_BB(kEntryBlock, DEF_SUCC1(3), DEF_PRED0()),
    DEF_BB(kExitBlock, DEF_SUCC0(), DEF_PRED1(6)),
    DEF_BB(kDalvikByteCode, DEF_SUCC2(4, 5), DEF_PRED1(1)),  // Block #3, top of the diamond.
    DEF_BB(kDalvikByteCode, DEF_SUCC1(6), DEF_PRED1(3)),     // Block #4, left side.
    DEF_BB(kDalvikByteCode, DEF_SUCC1(6), DEF_PRED1(3)),     // Block #5, right side.
    DEF_BB(kDalvikByteCode, DEF_SUCC1(2), DEF_PRED2(4, 5)),  // Block #6, bottom.
};

GvnDeadCodeEliminationTestDiamond::GvnDeadCodeEliminationTestDiamond()
    : GvnDeadCodeEliminationTest() {
  PrepareBasicBlocks(kDiamondBbs);
}

class GvnDeadCodeEliminationTestLoop : public GvnDeadCodeEliminationTest {
 public:
  GvnDeadCodeEliminationTestLoop();

 private:
  static const BBDef kLoopBbs[];
};

const GvnDeadCodeEliminationTest::BBDef GvnDeadCodeEliminationTestLoop::kLoopBbs[] = {
    DEF_BB(kNullBlock, DEF_SUCC0(), DEF_PRED0()),
    DEF_BB(kEntryBlock, DEF_SUCC1(3), DEF_PRED0()),
    DEF_BB(kExitBlock, DEF_SUCC0(), DEF_PRED1(5)),
    DEF_BB(kDalvikByteCode, DEF_SUCC1(4), DEF_PRED1(1)),
    DEF_BB(kDalvikByteCode, DEF_SUCC2(5, 4), DEF_PRED2(3, 4)),  // "taken" loops to self.
    DEF_BB(kDalvikByteCode, DEF_SUCC1(2), DEF_PRED1(4)),
};

GvnDeadCodeEliminationTestLoop::GvnDeadCodeEliminationTestLoop()
    : GvnDeadCodeEliminationTest() {
  PrepareBasicBlocks(kLoopBbs);
}

TEST_F(GvnDeadCodeEliminationTestSimple, Rename1) {
  static const IFieldDef ifields[] = {
      { 0u, 1u, 0u, false, kDexMemAccessWord },
      { 1u, 1u, 1u, false, kDexMemAccessWord },
  };
  static const MIRDef mirs[] = {
      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 0u),
      DEF_IGET(3, Instruction::IGET, 1u, 0u, 0u),
      DEF_MOVE(3, Instruction::MOVE_OBJECT, 2u, 0u),
      DEF_IGET(3, Instruction::IGET, 3u, 2u, 1u),
  };

  static const int32_t sreg_to_vreg_map[] = { 0, 1, 2, 2 };
  PrepareSRegToVRegMap(sreg_to_vreg_map);

  PrepareIFields(ifields);
  PrepareMIRs(mirs);
  PerformGVN_DCE();

  ASSERT_EQ(arraysize(mirs), value_names_.size());
  static const size_t diff_indexes[] = { 0, 1, 3 };
  ExpectValueNamesNE(diff_indexes);
  EXPECT_EQ(value_names_[0], value_names_[2]);

  const size_t no_null_ck_indexes[] = { 1, 3 };
  ExpectNoNullCheck(no_null_ck_indexes);

  static const bool eliminated[] = {
      false, false, true, false
  };
  static_assert(arraysize(eliminated) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(eliminated); ++i) {
    bool actually_eliminated = (static_cast<int>(mirs_[i].dalvikInsn.opcode) == kMirOpNop);
    EXPECT_EQ(eliminated[i], actually_eliminated) << i;
  }
  // Check that the IGET uses the s_reg 0, v_reg 0, defined by mirs_[0].
  ASSERT_EQ(1, mirs_[3].ssa_rep->num_uses);
  EXPECT_EQ(0, mirs_[3].ssa_rep->uses[0]);
  EXPECT_EQ(0u, mirs_[3].dalvikInsn.vB);
}

TEST_F(GvnDeadCodeEliminationTestSimple, Rename2) {
  static const IFieldDef ifields[] = {
      { 0u, 1u, 0u, false, kDexMemAccessWord },
      { 1u, 1u, 1u, false, kDexMemAccessWord },
  };
  static const MIRDef mirs[] = {
      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 0u),
      DEF_IGET(3, Instruction::IGET, 1u, 0u, 0u),
      DEF_MOVE(3, Instruction::MOVE_OBJECT, 2u, 0u),
      DEF_IGET(3, Instruction::IGET, 3u, 2u, 1u),
      DEF_CONST(3, Instruction::CONST, 4u, 1000),
  };

  static const int32_t sreg_to_vreg_map[] = { 0, 1, 2, 3, 2 };
  PrepareSRegToVRegMap(sreg_to_vreg_map);

  PrepareIFields(ifields);
  PrepareMIRs(mirs);
  PerformGVN_DCE();

  ASSERT_EQ(arraysize(mirs), value_names_.size());
  static const size_t diff_indexes[] = { 0, 1, 3, 4 };
  ExpectValueNamesNE(diff_indexes);
  EXPECT_EQ(value_names_[0], value_names_[2]);

  const size_t no_null_ck_indexes[] = { 1, 3 };
  ExpectNoNullCheck(no_null_ck_indexes);

  static const bool eliminated[] = {
      false, false, true, false, false
  };
  static_assert(arraysize(eliminated) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(eliminated); ++i) {
    bool actually_eliminated = (static_cast<int>(mirs_[i].dalvikInsn.opcode) == kMirOpNop);
    EXPECT_EQ(eliminated[i], actually_eliminated) << i;
  }
  // Check that the IGET uses the s_reg 0, v_reg 0, defined by mirs_[0].
  ASSERT_EQ(1, mirs_[3].ssa_rep->num_uses);
  EXPECT_EQ(0, mirs_[3].ssa_rep->uses[0]);
  EXPECT_EQ(0u, mirs_[3].dalvikInsn.vB);
}

TEST_F(GvnDeadCodeEliminationTestSimple, Rename3) {
  static const IFieldDef ifields[] = {
      { 0u, 1u, 0u, false, kDexMemAccessWord },
      { 1u, 1u, 1u, false, kDexMemAccessWord },
  };
  static const MIRDef mirs[] = {
      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 0u),
      DEF_IGET(3, Instruction::IGET, 1u, 0u, 0u),
      DEF_MOVE(3, Instruction::MOVE_OBJECT, 2u, 0u),
      DEF_IGET(3, Instruction::IGET, 3u, 2u, 1u),
  };

  static const int32_t sreg_to_vreg_map[] = { 0, 1, 2, 0 };
  PrepareSRegToVRegMap(sreg_to_vreg_map);

  PrepareIFields(ifields);
  PrepareMIRs(mirs);
  PerformGVN_DCE();

  ASSERT_EQ(arraysize(mirs), value_names_.size());
  static const size_t diff_indexes[] = { 0, 1, 3 };
  ExpectValueNamesNE(diff_indexes);
  EXPECT_EQ(value_names_[0], value_names_[2]);

  const size_t no_null_ck_indexes[] = { 1, 3 };
  ExpectNoNullCheck(no_null_ck_indexes);

  static const bool eliminated[] = {
      false, false, true, false
  };
  static_assert(arraysize(eliminated) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(eliminated); ++i) {
    bool actually_eliminated = (static_cast<int>(mirs_[i].dalvikInsn.opcode) == kMirOpNop);
    EXPECT_EQ(eliminated[i], actually_eliminated) << i;
  }
  // Check that the NEW_INSTANCE defines the s_reg 2, v_reg 2, originally defined by the move.
  ASSERT_EQ(1, mirs_[0].ssa_rep->num_defs);
  EXPECT_EQ(2, mirs_[0].ssa_rep->defs[0]);
  EXPECT_EQ(2u, mirs_[0].dalvikInsn.vA);
  // Check that the first IGET is using the s_reg 2, v_reg 2.
  ASSERT_EQ(1, mirs_[1].ssa_rep->num_uses);
  EXPECT_EQ(2, mirs_[1].ssa_rep->uses[0]);
  EXPECT_EQ(2u, mirs_[1].dalvikInsn.vB);
}

TEST_F(GvnDeadCodeEliminationTestSimple, Rename4) {
  static const MIRDef mirs[] = {
      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 0u),
      DEF_MOVE(3, Instruction::MOVE_OBJECT, 1u, 0u),
      DEF_MOVE(3, Instruction::MOVE_OBJECT, 2u, 1u),
      DEF_CONST_WIDE(3, Instruction::CONST_WIDE, 3u, 1000u),
  };

  static const int32_t sreg_to_vreg_map[] = { 0, 1, 2, 0, 1 /* high word */ };
  PrepareSRegToVRegMap(sreg_to_vreg_map);

  PrepareMIRs(mirs);
  static const int32_t wide_sregs[] = { 3 };
  MarkAsWideSRegs(wide_sregs);
  PerformGVN_DCE();

  ASSERT_EQ(arraysize(mirs), value_names_.size());
  static const size_t diff_indexes[] = { 0, 3 };
  ExpectValueNamesNE(diff_indexes);
  EXPECT_EQ(value_names_[0], value_names_[1]);
  EXPECT_EQ(value_names_[0], value_names_[2]);

  static const bool eliminated[] = {
      false, true, true, false
  };
  static_assert(arraysize(eliminated) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(eliminated); ++i) {
    bool actually_eliminated = (static_cast<int>(mirs_[i].dalvikInsn.opcode) == kMirOpNop);
    EXPECT_EQ(eliminated[i], actually_eliminated) << i;
  }
  // Check that the NEW_INSTANCE defines the s_reg 2, v_reg 2, originally defined by the move 2u.
  ASSERT_EQ(1, mirs_[0].ssa_rep->num_defs);
  EXPECT_EQ(2, mirs_[0].ssa_rep->defs[0]);
  EXPECT_EQ(2u, mirs_[0].dalvikInsn.vA);
}

TEST_F(GvnDeadCodeEliminationTestSimple, Rename5) {
  static const IFieldDef ifields[] = {
      { 0u, 1u, 0u, false, kDexMemAccessWord },
  };
  static const MIRDef mirs[] = {
      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 0u),
      DEF_IGET(3, Instruction::IGET, 1u, 0u, 0u),
      DEF_UNOP(3, Instruction::INT_TO_FLOAT, 2u, 1u),
      DEF_MOVE(3, Instruction::MOVE_OBJECT, 3u, 0u),
      DEF_MOVE(3, Instruction::MOVE_OBJECT, 4u, 3u),
      DEF_CONST_WIDE(3, Instruction::CONST_WIDE, 5u, 1000u),
  };

  static const int32_t sreg_to_vreg_map[] = { 0, 1, 2, 1, 3, 0, 1 /* high word */ };
  PrepareSRegToVRegMap(sreg_to_vreg_map);

  PrepareIFields(ifields);
  PrepareMIRs(mirs);
  static const int32_t wide_sregs[] = { 5 };
  MarkAsWideSRegs(wide_sregs);
  PerformGVN_DCE();

  ASSERT_EQ(arraysize(mirs), value_names_.size());
  static const size_t diff_indexes[] = { 0, 1, 2, 5 };
  ExpectValueNamesNE(diff_indexes);
  EXPECT_EQ(value_names_[0], value_names_[3]);
  EXPECT_EQ(value_names_[0], value_names_[4]);

  static const bool eliminated[] = {
      false, false, false, true, true, false
  };
  static_assert(arraysize(eliminated) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(eliminated); ++i) {
    bool actually_eliminated = (static_cast<int>(mirs_[i].dalvikInsn.opcode) == kMirOpNop);
    EXPECT_EQ(eliminated[i], actually_eliminated) << i;
  }
  // Check that the NEW_INSTANCE defines the s_reg 4, v_reg 3, originally defined by the move 4u.
  ASSERT_EQ(1, mirs_[0].ssa_rep->num_defs);
  EXPECT_EQ(4, mirs_[0].ssa_rep->defs[0]);
  EXPECT_EQ(3u, mirs_[0].dalvikInsn.vA);
}

TEST_F(GvnDeadCodeEliminationTestSimple, Rename6) {
  static const MIRDef mirs[] = {
      DEF_CONST_WIDE(3, Instruction::CONST_WIDE, 0u, 1000u),
      DEF_MOVE_WIDE(3, Instruction::MOVE_WIDE, 2u, 0u),
  };

  static const int32_t sreg_to_vreg_map[] = { 0, 1 /* high word */, 1, 2 /* high word */ };
  PrepareSRegToVRegMap(sreg_to_vreg_map);

  PrepareMIRs(mirs);
  static const int32_t wide_sregs[] = { 0, 2 };
  MarkAsWideSRegs(wide_sregs);
  PerformGVN_DCE();

  ASSERT_EQ(arraysize(mirs), value_names_.size());
  EXPECT_EQ(value_names_[0], value_names_[1]);

  static const bool eliminated[] = {
      false, true
  };
  static_assert(arraysize(eliminated) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(eliminated); ++i) {
    bool actually_eliminated = (static_cast<int>(mirs_[i].dalvikInsn.opcode) == kMirOpNop);
    EXPECT_EQ(eliminated[i], actually_eliminated) << i;
  }
  // Check that the CONST_WIDE defines the s_reg 2, v_reg 1, originally defined by the move 2u.
  ASSERT_EQ(2, mirs_[0].ssa_rep->num_defs);
  EXPECT_EQ(2, mirs_[0].ssa_rep->defs[0]);
  EXPECT_EQ(3, mirs_[0].ssa_rep->defs[1]);
  EXPECT_EQ(1u, mirs_[0].dalvikInsn.vA);
}

TEST_F(GvnDeadCodeEliminationTestSimple, Rename7) {
  static const MIRDef mirs[] = {
      DEF_CONST(3, Instruction::CONST, 0u, 1000u),
      DEF_MOVE(3, Instruction::MOVE, 1u, 0u),
      DEF_BINOP(3, Instruction::ADD_INT, 2u, 0u, 1u),
  };

  static const int32_t sreg_to_vreg_map[] = { 0, 1, 0 };
  PrepareSRegToVRegMap(sreg_to_vreg_map);

  PrepareMIRs(mirs);
  PerformGVN_DCE();

  ASSERT_EQ(arraysize(mirs), value_names_.size());
  EXPECT_NE(value_names_[0], value_names_[2]);
  EXPECT_EQ(value_names_[0], value_names_[1]);

  static const bool eliminated[] = {
      false, true, false
  };
  static_assert(arraysize(eliminated) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(eliminated); ++i) {
    bool actually_eliminated = (static_cast<int>(mirs_[i].dalvikInsn.opcode) == kMirOpNop);
    EXPECT_EQ(eliminated[i], actually_eliminated) << i;
  }
  // Check that the CONST defines the s_reg 1, v_reg 1, originally defined by the move 1u.
  ASSERT_EQ(1, mirs_[0].ssa_rep->num_defs);
  EXPECT_EQ(1, mirs_[0].ssa_rep->defs[0]);
  EXPECT_EQ(1u, mirs_[0].dalvikInsn.vA);
  // Check that the ADD_INT inputs are both s_reg1, vreg 1.
  ASSERT_EQ(2, mirs_[2].ssa_rep->num_uses);
  EXPECT_EQ(1, mirs_[2].ssa_rep->uses[0]);
  EXPECT_EQ(1, mirs_[2].ssa_rep->uses[1]);
  EXPECT_EQ(1u, mirs_[2].dalvikInsn.vB);
  EXPECT_EQ(1u, mirs_[2].dalvikInsn.vC);
}

TEST_F(GvnDeadCodeEliminationTestSimple, Rename8) {
  static const MIRDef mirs[] = {
      DEF_CONST(3, Instruction::CONST, 0u, 1000u),
      DEF_MOVE(3, Instruction::MOVE, 1u, 0u),
      DEF_BINOP(3, Instruction::ADD_INT_2ADDR, 2u, 0u, 1u),
  };

  static const int32_t sreg_to_vreg_map[] = { 0, 1, 0 };
  PrepareSRegToVRegMap(sreg_to_vreg_map);

  PrepareMIRs(mirs);
  PerformGVN_DCE();

  ASSERT_EQ(arraysize(mirs), value_names_.size());
  EXPECT_NE(value_names_[0], value_names_[2]);
  EXPECT_EQ(value_names_[0], value_names_[1]);

  static const bool eliminated[] = {
      false, true, false
  };
  static_assert(arraysize(eliminated) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(eliminated); ++i) {
    bool actually_eliminated = (static_cast<int>(mirs_[i].dalvikInsn.opcode) == kMirOpNop);
    EXPECT_EQ(eliminated[i], actually_eliminated) << i;
  }
  // Check that the CONST defines the s_reg 1, v_reg 1, originally defined by the move 1u.
  ASSERT_EQ(1, mirs_[0].ssa_rep->num_defs);
  EXPECT_EQ(1, mirs_[0].ssa_rep->defs[0]);
  EXPECT_EQ(1u, mirs_[0].dalvikInsn.vA);
  // Check that the ADD_INT_2ADDR was replaced by ADD_INT and inputs are both s_reg 1, vreg 1.
  EXPECT_EQ(Instruction::ADD_INT, mirs_[2].dalvikInsn.opcode);
  ASSERT_EQ(2, mirs_[2].ssa_rep->num_uses);
  EXPECT_EQ(1, mirs_[2].ssa_rep->uses[0]);
  EXPECT_EQ(1, mirs_[2].ssa_rep->uses[1]);
  EXPECT_EQ(1u, mirs_[2].dalvikInsn.vB);
  EXPECT_EQ(1u, mirs_[2].dalvikInsn.vC);
}

TEST_F(GvnDeadCodeEliminationTestSimple, Rename9) {
  static const MIRDef mirs[] = {
      DEF_CONST(3, Instruction::CONST, 0u, 1000u),
      DEF_BINOP(3, Instruction::ADD_INT_2ADDR, 1u, 0u, 0u),
      DEF_MOVE(3, Instruction::MOVE, 2u, 1u),
      DEF_CONST(3, Instruction::CONST, 3u, 3000u),
  };

  static const int32_t sreg_to_vreg_map[] = { 0, 0, 1, 0 };
  PrepareSRegToVRegMap(sreg_to_vreg_map);

  PrepareMIRs(mirs);
  PerformGVN_DCE();

  ASSERT_EQ(arraysize(mirs), value_names_.size());
  static const size_t diff_indexes[] = { 0, 1, 3 };
  ExpectValueNamesNE(diff_indexes);
  EXPECT_EQ(value_names_[1], value_names_[2]);

  static const bool eliminated[] = {
      false, false, true, false
  };
  static_assert(arraysize(eliminated) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(eliminated); ++i) {
    bool actually_eliminated = (static_cast<int>(mirs_[i].dalvikInsn.opcode) == kMirOpNop);
    EXPECT_EQ(eliminated[i], actually_eliminated) << i;
  }
  // Check that the ADD_INT_2ADDR was replaced by ADD_INT and output is in s_reg 2, vreg 1.
  EXPECT_EQ(Instruction::ADD_INT, mirs_[1].dalvikInsn.opcode);
  ASSERT_EQ(2, mirs_[1].ssa_rep->num_uses);
  EXPECT_EQ(0, mirs_[1].ssa_rep->uses[0]);
  EXPECT_EQ(0, mirs_[1].ssa_rep->uses[1]);
  EXPECT_EQ(0u, mirs_[1].dalvikInsn.vB);
  EXPECT_EQ(0u, mirs_[1].dalvikInsn.vC);
  ASSERT_EQ(1, mirs_[1].ssa_rep->num_defs);
  EXPECT_EQ(2, mirs_[1].ssa_rep->defs[0]);
  EXPECT_EQ(1u, mirs_[1].dalvikInsn.vA);
}

TEST_F(GvnDeadCodeEliminationTestSimple, NoRename1) {
  static const IFieldDef ifields[] = {
      { 0u, 1u, 0u, false, kDexMemAccessWord },
      { 1u, 1u, 1u, false, kDexMemAccessWord },
  };
  static const MIRDef mirs[] = {
      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 0u),
      DEF_IGET(3, Instruction::IGET, 1u, 0u, 0u),
      DEF_UNOP(3, Instruction::INT_TO_FLOAT, 2u, 1u),
      DEF_MOVE(3, Instruction::MOVE_OBJECT, 3u, 0u),
      DEF_CONST(3, Instruction::CONST, 4u, 1000),
      DEF_IGET(3, Instruction::IGET, 5u, 3u, 1u),
  };

  static const int32_t sreg_to_vreg_map[] = { 0, 1, 2, 1, 0, 1 };
  PrepareSRegToVRegMap(sreg_to_vreg_map);

  PrepareIFields(ifields);
  PrepareMIRs(mirs);
  PerformGVN_DCE();

  ASSERT_EQ(arraysize(mirs), value_names_.size());
  static const size_t diff_indexes[] = { 0, 1, 2, 4, 5 };
  ExpectValueNamesNE(diff_indexes);
  EXPECT_EQ(value_names_[0], value_names_[3]);

  const size_t no_null_ck_indexes[] = { 1, 5 };
  ExpectNoNullCheck(no_null_ck_indexes);

  static const bool eliminated[] = {
      false, false, false, false, false, false
  };
  static_assert(arraysize(eliminated) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(eliminated); ++i) {
    bool actually_eliminated = (static_cast<int>(mirs_[i].dalvikInsn.opcode) == kMirOpNop);
    EXPECT_EQ(eliminated[i], actually_eliminated) << i;
  }
}

TEST_F(GvnDeadCodeEliminationTestSimple, NoRename2) {
  static const IFieldDef ifields[] = {
      { 0u, 1u, 0u, false, kDexMemAccessWord },
      { 1u, 1u, 1u, false, kDexMemAccessWord },
  };
  static const MIRDef mirs[] = {
      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 0u),
      DEF_IGET(3, Instruction::IGET, 1u, 0u, 0u),
      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 2u),
      DEF_MOVE(3, Instruction::MOVE_OBJECT, 3u, 0u),
      DEF_CONST(3, Instruction::CONST, 4u, 1000),
      DEF_IGET(3, Instruction::IGET, 5u, 3u, 1u),
      DEF_CONST(3, Instruction::CONST, 6u, 2000),
  };

  static const int32_t sreg_to_vreg_map[] = { 0, 1, 2, 2, 0, 3, 2 };
  PrepareSRegToVRegMap(sreg_to_vreg_map);

  PrepareIFields(ifields);
  PrepareMIRs(mirs);
  PerformGVN_DCE();

  ASSERT_EQ(arraysize(mirs), value_names_.size());
  static const size_t diff_indexes[] = { 0, 1, 2, 4, 5, 6 };
  ExpectValueNamesNE(diff_indexes);
  EXPECT_EQ(value_names_[0], value_names_[3]);

  const size_t no_null_ck_indexes[] = { 1, 5 };
  ExpectNoNullCheck(no_null_ck_indexes);

  static const bool eliminated[] = {
      false, false, false, false, false, false, false
  };
  static_assert(arraysize(eliminated) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(eliminated); ++i) {
    bool actually_eliminated = (static_cast<int>(mirs_[i].dalvikInsn.opcode) == kMirOpNop);
    EXPECT_EQ(eliminated[i], actually_eliminated) << i;
  }
}

TEST_F(GvnDeadCodeEliminationTestSimple, NoRename3) {
  static const IFieldDef ifields[] = {
      { 0u, 1u, 0u, false, kDexMemAccessWord },
      { 1u, 1u, 1u, false, kDexMemAccessWord },
      { 2u, 1u, 2u, false, kDexMemAccessWord },
  };
  static const MIRDef mirs[] = {
      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 0u),
      DEF_IGET(3, Instruction::IGET, 1u, 0u, 0u),
      DEF_IGET(3, Instruction::IGET, 2u, 0u, 2u),
      DEF_BINOP(3, Instruction::ADD_INT, 3u, 1u, 2u),
      DEF_MOVE(3, Instruction::MOVE_OBJECT, 4u, 0u),
      DEF_IGET(3, Instruction::IGET, 5u, 4u, 1u),
  };

  static const int32_t sreg_to_vreg_map[] = { 0, 1, 2, 3, 2, 0 };
  PrepareSRegToVRegMap(sreg_to_vreg_map);

  PrepareIFields(ifields);
  PrepareMIRs(mirs);
  PerformGVN_DCE();

  ASSERT_EQ(arraysize(mirs), value_names_.size());
  static const size_t diff_indexes[] = { 0, 1, 2, 3, 5 };
  ExpectValueNamesNE(diff_indexes);
  EXPECT_EQ(value_names_[0], value_names_[4]);

  const size_t no_null_ck_indexes[] = { 1, 2, 5 };
  ExpectNoNullCheck(no_null_ck_indexes);

  static const bool eliminated[] = {
      false, false, false, false, false, false
  };
  static_assert(arraysize(eliminated) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(eliminated); ++i) {
    bool actually_eliminated = (static_cast<int>(mirs_[i].dalvikInsn.opcode) == kMirOpNop);
    EXPECT_EQ(eliminated[i], actually_eliminated) << i;
  }
}

TEST_F(GvnDeadCodeEliminationTestSimple, NoRename4) {
  static const MIRDef mirs[] = {
      DEF_CONST(3, Instruction::CONST, 0u, 1000u),
      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 1u),
      DEF_CONST(3, Instruction::CONST, 2u, 100u),
      DEF_CONST(3, Instruction::CONST, 3u, 200u),
      DEF_BINOP(3, Instruction::OR_INT_2ADDR, 4u, 2u, 3u),   // 3. Find definition of the move src.
      DEF_MOVE(3, Instruction::MOVE, 5u, 0u),                // 4. Uses move dest vreg.
      DEF_MOVE(3, Instruction::MOVE, 6u, 4u),                // 2. Find overwritten move src.
      DEF_CONST(3, Instruction::CONST, 7u, 2000u),           // 1. Overwrites 4u, look for moves.
  };

  static const int32_t sreg_to_vreg_map[] = { 0, 1, 2, 3, 2, 4, 0, 2 };
  PrepareSRegToVRegMap(sreg_to_vreg_map);

  PrepareMIRs(mirs);
  PerformGVN_DCE();

  ASSERT_EQ(arraysize(mirs), value_names_.size());
  static const size_t diff_indexes[] = { 0, 1, 2, 3, 4, 7 };
  ExpectValueNamesNE(diff_indexes);
  EXPECT_EQ(value_names_[0], value_names_[5]);
  EXPECT_EQ(value_names_[4], value_names_[6]);

  static const bool eliminated[] = {
      false, false, false, false, false, false, false, false
  };
  static_assert(arraysize(eliminated) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(eliminated); ++i) {
    bool actually_eliminated = (static_cast<int>(mirs_[i].dalvikInsn.opcode) == kMirOpNop);
    EXPECT_EQ(eliminated[i], actually_eliminated) << i;
  }
}

TEST_F(GvnDeadCodeEliminationTestSimple, Simple1) {
  static const IFieldDef ifields[] = {
      { 0u, 1u, 0u, false, kDexMemAccessObject },
      { 1u, 1u, 1u, false, kDexMemAccessObject },
      { 2u, 1u, 2u, false, kDexMemAccessWord },
  };
  static const MIRDef mirs[] = {
      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 0u),
      DEF_IGET(3, Instruction::IGET_OBJECT, 1u, 0u, 0u),
      DEF_IGET(3, Instruction::IGET_OBJECT, 2u, 1u, 1u),
      DEF_IGET(3, Instruction::IGET, 3u, 2u, 2u),
      DEF_IGET(3, Instruction::IGET_OBJECT, 4u, 0u, 0u),
      DEF_IGET(3, Instruction::IGET_OBJECT, 5u, 4u, 1u),
  };

  static const int32_t sreg_to_vreg_map[] = { 0, 1, 2, 3, 1, 2 };
  PrepareSRegToVRegMap(sreg_to_vreg_map);

  PrepareIFields(ifields);
  PrepareMIRs(mirs);
  PerformGVN_DCE();

  ASSERT_EQ(arraysize(mirs), value_names_.size());
  EXPECT_NE(value_names_[0], value_names_[1]);
  EXPECT_NE(value_names_[0], value_names_[2]);
  EXPECT_NE(value_names_[0], value_names_[3]);
  EXPECT_NE(value_names_[1], value_names_[2]);
  EXPECT_NE(value_names_[1], value_names_[3]);
  EXPECT_NE(value_names_[2], value_names_[3]);
  EXPECT_EQ(value_names_[1], value_names_[4]);
  EXPECT_EQ(value_names_[2], value_names_[5]);

  EXPECT_EQ(MIR_IGNORE_NULL_CHECK, mirs_[4].optimization_flags & MIR_IGNORE_NULL_CHECK);
  EXPECT_EQ(MIR_IGNORE_NULL_CHECK, mirs_[5].optimization_flags & MIR_IGNORE_NULL_CHECK);

  static const bool eliminated[] = {
      false, false, false, false, true, true
  };
  static_assert(arraysize(eliminated) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(eliminated); ++i) {
    bool actually_eliminated = (static_cast<int>(mirs_[i].dalvikInsn.opcode) == kMirOpNop);
    EXPECT_EQ(eliminated[i], actually_eliminated) << i;
  }
  // Check that the sregs have been renamed correctly.
  ASSERT_EQ(1, mirs_[1].ssa_rep->num_defs);
  EXPECT_EQ(4, mirs_[1].ssa_rep->defs[0]);
  ASSERT_EQ(1, mirs_[1].ssa_rep->num_uses);
  EXPECT_EQ(0, mirs_[1].ssa_rep->uses[0]);
  ASSERT_EQ(1, mirs_[2].ssa_rep->num_defs);
  EXPECT_EQ(5, mirs_[2].ssa_rep->defs[0]);
  ASSERT_EQ(1, mirs_[2].ssa_rep->num_uses);
  EXPECT_EQ(4, mirs_[2].ssa_rep->uses[0]);
  ASSERT_EQ(1, mirs_[3].ssa_rep->num_defs);
  EXPECT_EQ(3, mirs_[3].ssa_rep->defs[0]);
  ASSERT_EQ(1, mirs_[3].ssa_rep->num_uses);
  EXPECT_EQ(5, mirs_[3].ssa_rep->uses[0]);
}

TEST_F(GvnDeadCodeEliminationTestSimple, Simple2) {
  static const IFieldDef ifields[] = {
      { 0u, 1u, 0u, false, kDexMemAccessWord },
  };
  static const MIRDef mirs[] = {
      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 0u),
      DEF_CONST(3, Instruction::CONST, 1u, 1000),
      DEF_IGET(3, Instruction::IGET, 2u, 0u, 0u),
      DEF_BINOP(3, Instruction::ADD_INT_2ADDR, 3u, 2u, 1u),
      DEF_UNOP(3, Instruction::INT_TO_FLOAT, 4u, 3u),
      DEF_IGET(3, Instruction::IGET, 5u, 0u, 0u),
      DEF_BINOP(3, Instruction::ADD_INT_2ADDR, 6u, 5u, 1u),
  };

  static const int32_t sreg_to_vreg_map[] = { 0, 1, 2, 2, 3, 2, 2 };
  PrepareSRegToVRegMap(sreg_to_vreg_map);

  PrepareIFields(ifields);
  PrepareMIRs(mirs);
  PerformGVN_DCE();

  ASSERT_EQ(arraysize(mirs), value_names_.size());
  static const size_t diff_indexes[] = { 0, 1, 2, 3 };
  ExpectValueNamesNE(diff_indexes);
  EXPECT_EQ(value_names_[2], value_names_[5]);
  EXPECT_EQ(value_names_[3], value_names_[6]);

  const size_t no_null_ck_indexes[] = { 2, 5 };
  ExpectNoNullCheck(no_null_ck_indexes);

  static const bool eliminated[] = {
      false, false, false, false, false, true, true
  };
  static_assert(arraysize(eliminated) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(eliminated); ++i) {
    bool actually_eliminated = (static_cast<int>(mirs_[i].dalvikInsn.opcode) == kMirOpNop);
    EXPECT_EQ(eliminated[i], actually_eliminated) << i;
  }
  // Check that the sregs have been renamed correctly.
  ASSERT_EQ(1, mirs_[3].ssa_rep->num_defs);
  EXPECT_EQ(6, mirs_[3].ssa_rep->defs[0]);
  ASSERT_EQ(2, mirs_[3].ssa_rep->num_uses);
  EXPECT_EQ(2, mirs_[3].ssa_rep->uses[0]);
  EXPECT_EQ(1, mirs_[3].ssa_rep->uses[1]);
  ASSERT_EQ(1, mirs_[4].ssa_rep->num_defs);
  EXPECT_EQ(4, mirs_[4].ssa_rep->defs[0]);
  ASSERT_EQ(1, mirs_[4].ssa_rep->num_uses);
  EXPECT_EQ(6, mirs_[4].ssa_rep->uses[0]);
}

TEST_F(GvnDeadCodeEliminationTestSimple, Simple3) {
  static const IFieldDef ifields[] = {
      { 0u, 1u, 0u, false, kDexMemAccessWord },
  };
  static const MIRDef mirs[] = {
      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 0u),
      DEF_CONST(3, Instruction::CONST, 1u, 1000),
      DEF_CONST(3, Instruction::CONST, 2u, 2000),
      DEF_CONST(3, Instruction::CONST, 3u, 3000),
      DEF_IGET(3, Instruction::IGET, 4u, 0u, 0u),
      DEF_BINOP(3, Instruction::ADD_INT, 5u, 4u, 1u),
      DEF_BINOP(3, Instruction::MUL_INT, 6u, 5u, 2u),
      DEF_BINOP(3, Instruction::SUB_INT, 7u, 6u, 3u),
      DEF_UNOP(3, Instruction::INT_TO_FLOAT, 8u, 7u),
      DEF_IGET(3, Instruction::IGET, 9u, 0u, 0u),
      DEF_BINOP(3, Instruction::ADD_INT, 10u, 9u, 1u),
      DEF_BINOP(3, Instruction::MUL_INT, 11u, 10u, 2u),  // Simple elimination of ADD+MUL
      DEF_BINOP(3, Instruction::SUB_INT, 12u, 11u, 3u),  // allows simple elimination of IGET+SUB.
  };

  static const int32_t sreg_to_vreg_map[] = { 0, 1, 2, 3, 4, 5, 5, 4, 6, 4, 5, 5, 4 };
  PrepareSRegToVRegMap(sreg_to_vreg_map);

  PrepareIFields(ifields);
  PrepareMIRs(mirs);
  PerformGVN_DCE();

  ASSERT_EQ(arraysize(mirs), value_names_.size());
  static const size_t diff_indexes[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8 };
  ExpectValueNamesNE(diff_indexes);
  EXPECT_EQ(value_names_[4], value_names_[9]);
  EXPECT_EQ(value_names_[5], value_names_[10]);
  EXPECT_EQ(value_names_[6], value_names_[11]);
  EXPECT_EQ(value_names_[7], value_names_[12]);

  const size_t no_null_ck_indexes[] = { 4, 9 };
  ExpectNoNullCheck(no_null_ck_indexes);

  static const bool eliminated[] = {
      false, false, false, false, false, false, false, false, false, true, true, true, true
  };
  static_assert(arraysize(eliminated) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(eliminated); ++i) {
    bool actually_eliminated = (static_cast<int>(mirs_[i].dalvikInsn.opcode) == kMirOpNop);
    EXPECT_EQ(eliminated[i], actually_eliminated) << i;
  }
  // Check that the sregs have been renamed correctly.
  ASSERT_EQ(1, mirs_[6].ssa_rep->num_defs);
  EXPECT_EQ(11, mirs_[6].ssa_rep->defs[0]);  // 6 -> 11
  ASSERT_EQ(2, mirs_[6].ssa_rep->num_uses);
  EXPECT_EQ(5, mirs_[6].ssa_rep->uses[0]);
  EXPECT_EQ(2, mirs_[6].ssa_rep->uses[1]);
  ASSERT_EQ(1, mirs_[7].ssa_rep->num_defs);
  EXPECT_EQ(12, mirs_[7].ssa_rep->defs[0]);  // 7 -> 12
  ASSERT_EQ(2, mirs_[7].ssa_rep->num_uses);
  EXPECT_EQ(11, mirs_[7].ssa_rep->uses[0]);  // 6 -> 11
  EXPECT_EQ(3, mirs_[7].ssa_rep->uses[1]);
  ASSERT_EQ(1, mirs_[8].ssa_rep->num_defs);
  EXPECT_EQ(8, mirs_[8].ssa_rep->defs[0]);
  ASSERT_EQ(1, mirs_[8].ssa_rep->num_uses);
  EXPECT_EQ(12, mirs_[8].ssa_rep->uses[0]);  // 7 -> 12
}

TEST_F(GvnDeadCodeEliminationTestSimple, Simple4) {
  static const IFieldDef ifields[] = {
      { 0u, 1u, 0u, false, kDexMemAccessWord },
  };
  static const MIRDef mirs[] = {
      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 0u),
      DEF_CONST_WIDE(3, Instruction::CONST_WIDE, 1u, INT64_C(1)),
      DEF_BINOP(3, Instruction::LONG_TO_FLOAT, 3u, 1u, 2u),
      DEF_IGET(3, Instruction::IGET, 4u, 0u, 0u),
      DEF_UNOP(3, Instruction::INT_TO_FLOAT, 5u, 4u),
      DEF_CONST_WIDE(3, Instruction::CONST_WIDE, 6u, INT64_C(1)),
      DEF_BINOP(3, Instruction::LONG_TO_FLOAT, 8u, 6u, 7u),
      DEF_IGET(3, Instruction::IGET, 9u, 0u, 0u),
  };

  static const int32_t sreg_to_vreg_map[] = { 0, 1, 2, 1, 2, 3, 1, 2, 1, 2 };
  PrepareSRegToVRegMap(sreg_to_vreg_map);

  PrepareIFields(ifields);
  PrepareMIRs(mirs);
  static const int32_t wide_sregs[] = { 1, 6 };
  MarkAsWideSRegs(wide_sregs);
  PerformGVN_DCE();

  ASSERT_EQ(arraysize(mirs), value_names_.size());
  static const size_t diff_indexes[] = { 0, 1, 2, 3, 4 };
  ExpectValueNamesNE(diff_indexes);
  EXPECT_EQ(value_names_[1], value_names_[5]);
  EXPECT_EQ(value_names_[2], value_names_[6]);
  EXPECT_EQ(value_names_[3], value_names_[7]);

  const size_t no_null_ck_indexes[] = { 3, 7 };
  ExpectNoNullCheck(no_null_ck_indexes);

  static const bool eliminated[] = {
      // Simple elimination of CONST_WIDE+LONG_TO_FLOAT allows simple eliminatiion of IGET.
      false, false, false, false, false, true, true, true
  };
  static_assert(arraysize(eliminated) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(eliminated); ++i) {
    bool actually_eliminated = (static_cast<int>(mirs_[i].dalvikInsn.opcode) == kMirOpNop);
    EXPECT_EQ(eliminated[i], actually_eliminated) << i;
  }
  // Check that the sregs have been renamed correctly.
  ASSERT_EQ(1, mirs_[2].ssa_rep->num_defs);
  EXPECT_EQ(8, mirs_[2].ssa_rep->defs[0]);   // 3 -> 8
  ASSERT_EQ(2, mirs_[2].ssa_rep->num_uses);
  EXPECT_EQ(1, mirs_[2].ssa_rep->uses[0]);
  EXPECT_EQ(2, mirs_[2].ssa_rep->uses[1]);
  ASSERT_EQ(1, mirs_[3].ssa_rep->num_defs);
  EXPECT_EQ(9, mirs_[3].ssa_rep->defs[0]);   // 4 -> 9
  ASSERT_EQ(1, mirs_[3].ssa_rep->num_uses);
  EXPECT_EQ(0, mirs_[3].ssa_rep->uses[0]);
  ASSERT_EQ(1, mirs_[4].ssa_rep->num_defs);
  EXPECT_EQ(5, mirs_[4].ssa_rep->defs[0]);
  ASSERT_EQ(1, mirs_[4].ssa_rep->num_uses);
  EXPECT_EQ(9, mirs_[4].ssa_rep->uses[0]);   // 4 -> 9
}

TEST_F(GvnDeadCodeEliminationTestSimple, KillChain1) {
  static const IFieldDef ifields[] = {
      { 0u, 1u, 0u, false, kDexMemAccessWord },
  };
  static const MIRDef mirs[] = {
      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 0u),
      DEF_CONST(3, Instruction::CONST, 1u, 1000),
      DEF_CONST(3, Instruction::CONST, 2u, 2000),
      DEF_CONST(3, Instruction::CONST, 3u, 3000),
      DEF_IGET(3, Instruction::IGET, 4u, 0u, 0u),
      DEF_BINOP(3, Instruction::ADD_INT, 5u, 4u, 1u),
      DEF_BINOP(3, Instruction::MUL_INT, 6u, 5u, 2u),
      DEF_BINOP(3, Instruction::SUB_INT, 7u, 6u, 3u),
      DEF_UNOP(3, Instruction::INT_TO_FLOAT, 8u, 7u),
      DEF_IGET(3, Instruction::IGET, 9u, 0u, 0u),
      DEF_BINOP(3, Instruction::ADD_INT, 10u, 9u, 1u),
      DEF_BINOP(3, Instruction::MUL_INT, 11u, 10u, 2u),
      DEF_BINOP(3, Instruction::SUB_INT, 12u, 11u, 3u),
  };

  static const int32_t sreg_to_vreg_map[] = { 0, 1, 2, 3, 4, 5, 4, 5, 6, 4, 5, 4, 5 };
  PrepareSRegToVRegMap(sreg_to_vreg_map);

  PrepareIFields(ifields);
  PrepareMIRs(mirs);
  PerformGVN_DCE();

  ASSERT_EQ(arraysize(mirs), value_names_.size());
  static const size_t diff_indexes[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8 };
  ExpectValueNamesNE(diff_indexes);
  EXPECT_EQ(value_names_[4], value_names_[9]);
  EXPECT_EQ(value_names_[5], value_names_[10]);
  EXPECT_EQ(value_names_[6], value_names_[11]);
  EXPECT_EQ(value_names_[7], value_names_[12]);

  const size_t no_null_ck_indexes[] = { 4, 9 };
  ExpectNoNullCheck(no_null_ck_indexes);

  static const bool eliminated[] = {
      false, false, false, false, false, false, false, false, false, true, true, true, true
  };
  static_assert(arraysize(eliminated) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(eliminated); ++i) {
    bool actually_eliminated = (static_cast<int>(mirs_[i].dalvikInsn.opcode) == kMirOpNop);
    EXPECT_EQ(eliminated[i], actually_eliminated) << i;
  }
  // Check that the sregs have been renamed correctly.
  ASSERT_EQ(1, mirs_[6].ssa_rep->num_defs);
  EXPECT_EQ(11, mirs_[6].ssa_rep->defs[0]);  // 6 -> 11
  ASSERT_EQ(2, mirs_[6].ssa_rep->num_uses);
  EXPECT_EQ(5, mirs_[6].ssa_rep->uses[0]);
  EXPECT_EQ(2, mirs_[6].ssa_rep->uses[1]);
  ASSERT_EQ(1, mirs_[7].ssa_rep->num_defs);
  EXPECT_EQ(12, mirs_[7].ssa_rep->defs[0]);  // 7 -> 12
  ASSERT_EQ(2, mirs_[7].ssa_rep->num_uses);
  EXPECT_EQ(11, mirs_[7].ssa_rep->uses[0]);  // 6 -> 11
  EXPECT_EQ(3, mirs_[7].ssa_rep->uses[1]);
  ASSERT_EQ(1, mirs_[8].ssa_rep->num_defs);
  EXPECT_EQ(8, mirs_[8].ssa_rep->defs[0]);
  ASSERT_EQ(1, mirs_[8].ssa_rep->num_uses);
  EXPECT_EQ(12, mirs_[8].ssa_rep->uses[0]);   // 7 -> 12
}

TEST_F(GvnDeadCodeEliminationTestSimple, KillChain2) {
  static const IFieldDef ifields[] = {
      { 0u, 1u, 0u, false, kDexMemAccessWord },
  };
  static const MIRDef mirs[] = {
      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 0u),
      DEF_CONST(3, Instruction::CONST, 1u, 1000),
      DEF_CONST(3, Instruction::CONST, 2u, 2000),
      DEF_CONST(3, Instruction::CONST, 3u, 3000),
      DEF_IGET(3, Instruction::IGET, 4u, 0u, 0u),
      DEF_BINOP(3, Instruction::ADD_INT, 5u, 4u, 1u),
      DEF_BINOP(3, Instruction::MUL_INT, 6u, 5u, 2u),
      DEF_BINOP(3, Instruction::SUB_INT, 7u, 6u, 3u),
      DEF_UNOP(3, Instruction::INT_TO_FLOAT, 8u, 7u),
      DEF_IGET(3, Instruction::IGET, 9u, 0u, 0u),
      DEF_BINOP(3, Instruction::ADD_INT, 10u, 9u, 1u),
      DEF_BINOP(3, Instruction::MUL_INT, 11u, 10u, 2u),
      DEF_BINOP(3, Instruction::SUB_INT, 12u, 11u, 3u),
      DEF_CONST(3, Instruction::CONST, 13u, 4000),
  };

  static const int32_t sreg_to_vreg_map[] = { 0, 1, 2, 3, 4, 5, 5, 4, 6, 4, 7, 7, 4, 7 };
  PrepareSRegToVRegMap(sreg_to_vreg_map);

  PrepareIFields(ifields);
  PrepareMIRs(mirs);
  PerformGVN_DCE();

  ASSERT_EQ(arraysize(mirs), value_names_.size());
  static const size_t diff_indexes[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 13 };
  ExpectValueNamesNE(diff_indexes);
  EXPECT_EQ(value_names_[4], value_names_[9]);
  EXPECT_EQ(value_names_[5], value_names_[10]);
  EXPECT_EQ(value_names_[6], value_names_[11]);
  EXPECT_EQ(value_names_[7], value_names_[12]);

  const size_t no_null_ck_indexes[] = { 4, 9 };
  ExpectNoNullCheck(no_null_ck_indexes);

  static const bool eliminated[] = {
      false, false, false, false, false, false, false, false, false, true, true, true, true, false
  };
  static_assert(arraysize(eliminated) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(eliminated); ++i) {
    bool actually_eliminated = (static_cast<int>(mirs_[i].dalvikInsn.opcode) == kMirOpNop);
    EXPECT_EQ(eliminated[i], actually_eliminated) << i;
  }
  // Check that the sregs have been renamed correctly.
  ASSERT_EQ(1, mirs_[7].ssa_rep->num_defs);
  EXPECT_EQ(12, mirs_[7].ssa_rep->defs[0]);  // 7 -> 12
  ASSERT_EQ(2, mirs_[7].ssa_rep->num_uses);
  EXPECT_EQ(6, mirs_[7].ssa_rep->uses[0]);
  EXPECT_EQ(3, mirs_[7].ssa_rep->uses[1]);
  ASSERT_EQ(1, mirs_[8].ssa_rep->num_defs);
  EXPECT_EQ(8, mirs_[8].ssa_rep->defs[0]);
  ASSERT_EQ(1, mirs_[8].ssa_rep->num_uses);
  EXPECT_EQ(12, mirs_[8].ssa_rep->uses[0]);   // 7 -> 12
}

TEST_F(GvnDeadCodeEliminationTestSimple, KillChain3) {
  static const IFieldDef ifields[] = {
      { 0u, 1u, 0u, false, kDexMemAccessWord },
  };
  static const MIRDef mirs[] = {
      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 0u),
      DEF_CONST(3, Instruction::CONST, 1u, 1000),
      DEF_CONST(3, Instruction::CONST, 2u, 2000),
      DEF_CONST(3, Instruction::CONST, 3u, 3000),
      DEF_IGET(3, Instruction::IGET, 4u, 0u, 0u),
      DEF_BINOP(3, Instruction::ADD_INT, 5u, 4u, 1u),
      DEF_BINOP(3, Instruction::MUL_INT, 6u, 5u, 2u),
      DEF_BINOP(3, Instruction::SUB_INT, 7u, 6u, 3u),
      DEF_UNOP(3, Instruction::INT_TO_FLOAT, 8u, 7u),
      DEF_IGET(3, Instruction::IGET, 9u, 0u, 0u),
      DEF_BINOP(3, Instruction::ADD_INT, 10u, 9u, 1u),
      DEF_BINOP(3, Instruction::MUL_INT, 11u, 10u, 2u),
      DEF_CONST(3, Instruction::CONST, 12u, 4000),
      DEF_BINOP(3, Instruction::SUB_INT, 13u, 11u, 3u),
  };

  static const int32_t sreg_to_vreg_map[] = { 0, 1, 2, 3, 4, 5, 5, 4, 6, 4, 7, 4, 7, 4 };
  PrepareSRegToVRegMap(sreg_to_vreg_map);

  PrepareIFields(ifields);
  PrepareMIRs(mirs);
  PerformGVN_DCE();

  ASSERT_EQ(arraysize(mirs), value_names_.size());
  static const size_t diff_indexes[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 12 };
  ExpectValueNamesNE(diff_indexes);
  EXPECT_EQ(value_names_[4], value_names_[9]);
  EXPECT_EQ(value_names_[5], value_names_[10]);
  EXPECT_EQ(value_names_[6], value_names_[11]);
  EXPECT_EQ(value_names_[7], value_names_[13]);

  const size_t no_null_ck_indexes[] = { 4, 9 };
  ExpectNoNullCheck(no_null_ck_indexes);

  static const bool eliminated[] = {
      false, false, false, false, false, false, false, false, false, true, true, true, false, true
  };
  static_assert(arraysize(eliminated) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(eliminated); ++i) {
    bool actually_eliminated = (static_cast<int>(mirs_[i].dalvikInsn.opcode) == kMirOpNop);
    EXPECT_EQ(eliminated[i], actually_eliminated) << i;
  }
  // Check that the sregs have been renamed correctly.
  ASSERT_EQ(1, mirs_[7].ssa_rep->num_defs);
  EXPECT_EQ(13, mirs_[7].ssa_rep->defs[0]);  // 7 -> 13
  ASSERT_EQ(2, mirs_[7].ssa_rep->num_uses);
  EXPECT_EQ(6, mirs_[7].ssa_rep->uses[0]);
  EXPECT_EQ(3, mirs_[7].ssa_rep->uses[1]);
  ASSERT_EQ(1, mirs_[8].ssa_rep->num_defs);
  EXPECT_EQ(8, mirs_[8].ssa_rep->defs[0]);
  ASSERT_EQ(1, mirs_[8].ssa_rep->num_uses);
  EXPECT_EQ(13, mirs_[8].ssa_rep->uses[0]);   // 7 -> 13
}

TEST_F(GvnDeadCodeEliminationTestSimple, KeepChain1) {
  // KillChain2 without the final CONST.
  static const IFieldDef ifields[] = {
      { 0u, 1u, 0u, false, kDexMemAccessWord },
  };
  static const MIRDef mirs[] = {
      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 0u),
      DEF_CONST(3, Instruction::CONST, 1u, 1000),
      DEF_CONST(3, Instruction::CONST, 2u, 2000),
      DEF_CONST(3, Instruction::CONST, 3u, 3000),
      DEF_IGET(3, Instruction::IGET, 4u, 0u, 0u),
      DEF_BINOP(3, Instruction::ADD_INT, 5u, 4u, 1u),
      DEF_BINOP(3, Instruction::MUL_INT, 6u, 5u, 2u),
      DEF_BINOP(3, Instruction::SUB_INT, 7u, 6u, 3u),
      DEF_UNOP(3, Instruction::INT_TO_FLOAT, 8u, 7u),
      DEF_IGET(3, Instruction::IGET, 9u, 0u, 0u),
      DEF_BINOP(3, Instruction::ADD_INT, 10u, 9u, 1u),
      DEF_BINOP(3, Instruction::MUL_INT, 11u, 10u, 2u),
      DEF_BINOP(3, Instruction::SUB_INT, 12u, 11u, 3u),
  };

  static const int32_t sreg_to_vreg_map[] = { 0, 1, 2, 3, 4, 5, 5, 4, 6, 4, 7, 7, 4 };
  PrepareSRegToVRegMap(sreg_to_vreg_map);

  PrepareIFields(ifields);
  PrepareMIRs(mirs);
  PerformGVN_DCE();

  ASSERT_EQ(arraysize(mirs), value_names_.size());
  static const size_t diff_indexes[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8 };
  ExpectValueNamesNE(diff_indexes);
  EXPECT_EQ(value_names_[4], value_names_[9]);
  EXPECT_EQ(value_names_[5], value_names_[10]);
  EXPECT_EQ(value_names_[6], value_names_[11]);
  EXPECT_EQ(value_names_[7], value_names_[12]);

  const size_t no_null_ck_indexes[] = { 4, 9 };
  ExpectNoNullCheck(no_null_ck_indexes);

  static const bool eliminated[] = {
      false, false, false, false, false, false, false, false, false, false, false, false, false
  };
  static_assert(arraysize(eliminated) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(eliminated); ++i) {
    bool actually_eliminated = (static_cast<int>(mirs_[i].dalvikInsn.opcode) == kMirOpNop);
    EXPECT_EQ(eliminated[i], actually_eliminated) << i;
  }
}

TEST_F(GvnDeadCodeEliminationTestSimple, KeepChain2) {
  // KillChain1 with MIRs in the middle of the chain.
  static const IFieldDef ifields[] = {
      { 0u, 1u, 0u, false, kDexMemAccessWord },
  };
  static const MIRDef mirs[] = {
      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 0u),
      DEF_CONST(3, Instruction::CONST, 1u, 1000),
      DEF_CONST(3, Instruction::CONST, 2u, 2000),
      DEF_CONST(3, Instruction::CONST, 3u, 3000),
      DEF_IGET(3, Instruction::IGET, 4u, 0u, 0u),
      DEF_BINOP(3, Instruction::ADD_INT, 5u, 4u, 1u),
      DEF_BINOP(3, Instruction::MUL_INT, 6u, 5u, 2u),
      DEF_BINOP(3, Instruction::SUB_INT, 7u, 6u, 3u),
      DEF_UNOP(3, Instruction::INT_TO_FLOAT, 8u, 7u),
      DEF_IGET(3, Instruction::IGET, 9u, 0u, 0u),
      DEF_BINOP(3, Instruction::ADD_INT, 10u, 9u, 1u),
      DEF_CONST(3, Instruction::CONST, 11u, 4000),
      DEF_UNOP(3, Instruction::INT_TO_FLOAT, 12u, 11u),
      DEF_BINOP(3, Instruction::MUL_INT, 13u, 10u, 2u),
      DEF_BINOP(3, Instruction::SUB_INT, 14u, 13u, 3u),
  };

  static const int32_t sreg_to_vreg_map[] = { 0, 1, 2, 3, 4, 5, 4, 5, 6, 4, 5, 4, 7, 4, 5 };
  PrepareSRegToVRegMap(sreg_to_vreg_map);

  PrepareIFields(ifields);
  PrepareMIRs(mirs);
  PerformGVN_DCE();

  ASSERT_EQ(arraysize(mirs), value_names_.size());
  static const size_t diff_indexes[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8 };
  ExpectValueNamesNE(diff_indexes);
  EXPECT_EQ(value_names_[4], value_names_[9]);
  EXPECT_EQ(value_names_[5], value_names_[10]);
  EXPECT_EQ(value_names_[6], value_names_[13]);
  EXPECT_EQ(value_names_[7], value_names_[14]);

  const size_t no_null_ck_indexes[] = { 4, 9 };
  ExpectNoNullCheck(no_null_ck_indexes);

  static const bool eliminated[] = {
      false, false, false, false, false, false, false, false, false,
      false, false, false, false, false, false
  };
  static_assert(arraysize(eliminated) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(eliminated); ++i) {
    bool actually_eliminated = (static_cast<int>(mirs_[i].dalvikInsn.opcode) == kMirOpNop);
    EXPECT_EQ(eliminated[i], actually_eliminated) << i;
  }
}

TEST_F(GvnDeadCodeEliminationTestDiamond, CreatePhi1) {
  static const MIRDef mirs[] = {
      DEF_CONST(3, Instruction::CONST, 0u, 1000),
      DEF_CONST(4, Instruction::CONST, 1u, 1000),
  };

  static const int32_t sreg_to_vreg_map[] = { 0, 0 };
  PrepareSRegToVRegMap(sreg_to_vreg_map);

  PrepareMIRs(mirs);
  PerformGVN_DCE();

  ASSERT_EQ(arraysize(mirs), value_names_.size());
  EXPECT_EQ(value_names_[0], value_names_[1]);

  static const bool eliminated[] = {
      false, true,
  };
  static_assert(arraysize(eliminated) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(eliminated); ++i) {
    bool actually_eliminated = (static_cast<int>(mirs_[i].dalvikInsn.opcode) == kMirOpNop);
    EXPECT_EQ(eliminated[i], actually_eliminated) << i;
  }
  // Check that we've created a single-input Phi to replace the CONST 3u.
  BasicBlock* bb4 = cu_.mir_graph->GetBasicBlock(4);
  MIR* phi = bb4->first_mir_insn;
  ASSERT_TRUE(phi != nullptr);
  ASSERT_EQ(kMirOpPhi, static_cast<int>(phi->dalvikInsn.opcode));
  ASSERT_EQ(1, phi->ssa_rep->num_uses);
  EXPECT_EQ(0, phi->ssa_rep->uses[0]);
  ASSERT_EQ(1, phi->ssa_rep->num_defs);
  EXPECT_EQ(1, phi->ssa_rep->defs[0]);
  EXPECT_EQ(0u, phi->dalvikInsn.vA);
}

TEST_F(GvnDeadCodeEliminationTestDiamond, CreatePhi2) {
  static const MIRDef mirs[] = {
      DEF_CONST(3, Instruction::CONST, 0u, 1000),
      DEF_MOVE(4, Instruction::MOVE, 1u, 0u),
      DEF_CONST(4, Instruction::CONST, 2u, 1000),
  };

  static const int32_t sreg_to_vreg_map[] = { 0, 1, 0 };
  PrepareSRegToVRegMap(sreg_to_vreg_map);

  PrepareMIRs(mirs);
  PerformGVN_DCE();

  ASSERT_EQ(arraysize(mirs), value_names_.size());
  EXPECT_EQ(value_names_[0], value_names_[1]);
  EXPECT_EQ(value_names_[0], value_names_[2]);

  static const bool eliminated[] = {
      false, false, true,
  };
  static_assert(arraysize(eliminated) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(eliminated); ++i) {
    bool actually_eliminated = (static_cast<int>(mirs_[i].dalvikInsn.opcode) == kMirOpNop);
    EXPECT_EQ(eliminated[i], actually_eliminated) << i;
  }
  // Check that we've created a single-input Phi to replace the CONST 3u.
  BasicBlock* bb4 = cu_.mir_graph->GetBasicBlock(4);
  MIR* phi = bb4->first_mir_insn;
  ASSERT_TRUE(phi != nullptr);
  ASSERT_EQ(kMirOpPhi, static_cast<int>(phi->dalvikInsn.opcode));
  ASSERT_EQ(1, phi->ssa_rep->num_uses);
  EXPECT_EQ(0, phi->ssa_rep->uses[0]);
  ASSERT_EQ(1, phi->ssa_rep->num_defs);
  EXPECT_EQ(2, phi->ssa_rep->defs[0]);
  EXPECT_EQ(0u, phi->dalvikInsn.vA);
  MIR* move = phi->next;
  ASSERT_TRUE(move != nullptr);
  ASSERT_EQ(Instruction::MOVE, move->dalvikInsn.opcode);
  ASSERT_EQ(1, move->ssa_rep->num_uses);
  EXPECT_EQ(2, move->ssa_rep->uses[0]);
  ASSERT_EQ(1, move->ssa_rep->num_defs);
  EXPECT_EQ(1, move->ssa_rep->defs[0]);
  EXPECT_EQ(1u, move->dalvikInsn.vA);
  EXPECT_EQ(0u, move->dalvikInsn.vB);
}

TEST_F(GvnDeadCodeEliminationTestDiamond, CreatePhi3) {
  static const IFieldDef ifields[] = {
      { 0u, 1u, 0u, false, kDexMemAccessWord },
  };
  static const MIRDef mirs[] = {
      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 0u),
      DEF_CONST(4, Instruction::CONST, 1u, 1000),
      DEF_IPUT(4, Instruction::IPUT, 1u, 0u, 0u),
      DEF_CONST(5, Instruction::CONST, 3u, 2000),
      DEF_IPUT(5, Instruction::IPUT, 3u, 0u, 0u),
      DEF_IGET(6, Instruction::IGET, 5u, 0u, 0u),
  };

  static const int32_t sreg_to_vreg_map[] = { 0, 1, 2 /* dummy */, 1, 2 /* dummy */, 1 };
  PrepareSRegToVRegMap(sreg_to_vreg_map);

  PrepareIFields(ifields);
  PrepareMIRs(mirs);
  PerformGVN_DCE();

  ASSERT_EQ(arraysize(mirs), value_names_.size());
  static const size_t diff_indexes[] = { 0, 1, 3, 5 };
  ExpectValueNamesNE(diff_indexes);

  const size_t no_null_ck_indexes[] = { 2, 4, 5 };
  ExpectNoNullCheck(no_null_ck_indexes);

  static const bool eliminated[] = {
      false, false, false, false, false, true,
  };
  static_assert(arraysize(eliminated) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(eliminated); ++i) {
    bool actually_eliminated = (static_cast<int>(mirs_[i].dalvikInsn.opcode) == kMirOpNop);
    EXPECT_EQ(eliminated[i], actually_eliminated) << i;
  }
  // Check that we've created a two-input Phi to replace the IGET 5u.
  BasicBlock* bb6 = cu_.mir_graph->GetBasicBlock(6);
  MIR* phi = bb6->first_mir_insn;
  ASSERT_TRUE(phi != nullptr);
  ASSERT_EQ(kMirOpPhi, static_cast<int>(phi->dalvikInsn.opcode));
  ASSERT_EQ(2, phi->ssa_rep->num_uses);
  EXPECT_EQ(1, phi->ssa_rep->uses[0]);
  EXPECT_EQ(3, phi->ssa_rep->uses[1]);
  ASSERT_EQ(1, phi->ssa_rep->num_defs);
  EXPECT_EQ(5, phi->ssa_rep->defs[0]);
  EXPECT_EQ(1u, phi->dalvikInsn.vA);
}

TEST_F(GvnDeadCodeEliminationTestDiamond, KillChainInAnotherBlock1) {
  static const IFieldDef ifields[] = {
      { 0u, 1u, 0u, false, kDexMemAccessObject },  // linked list
  };
  static const MIRDef mirs[] = {
      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 0u),
      DEF_IGET(3, Instruction::IGET_OBJECT, 1u, 0u, 0u),
      DEF_IGET(3, Instruction::IGET_OBJECT, 2u, 1u, 0u),
      DEF_IGET(3, Instruction::IGET_OBJECT, 3u, 2u, 0u),
      DEF_IGET(3, Instruction::IGET_OBJECT, 4u, 3u, 0u),
      DEF_IFZ(3, Instruction::IF_NEZ, 4u),
      DEF_IGET(4, Instruction::IGET_OBJECT, 6u, 0u, 0u),
      DEF_IGET(4, Instruction::IGET_OBJECT, 7u, 6u, 0u),
      DEF_IGET(4, Instruction::IGET_OBJECT, 8u, 7u, 0u),
      DEF_IGET(4, Instruction::IGET_OBJECT, 9u, 8u, 0u),
  };

  static const int32_t sreg_to_vreg_map[] = { 0, 1, 2, 1, 2, 3 /* dummy */, 1, 2, 1, 2 };
  PrepareSRegToVRegMap(sreg_to_vreg_map);

  PrepareIFields(ifields);
  PrepareMIRs(mirs);
  PerformGVN_DCE();

  ASSERT_EQ(arraysize(mirs), value_names_.size());
  static const size_t diff_indexes[] = { 0, 1, 2, 3, 4 };
  ExpectValueNamesNE(diff_indexes);
  EXPECT_EQ(value_names_[1], value_names_[6]);
  EXPECT_EQ(value_names_[2], value_names_[7]);
  EXPECT_EQ(value_names_[3], value_names_[8]);
  EXPECT_EQ(value_names_[4], value_names_[9]);

  const size_t no_null_ck_indexes[] = { 1, 6, 7, 8, 9 };
  ExpectNoNullCheck(no_null_ck_indexes);

  static const bool eliminated[] = {
      false, false, false, false, false, false, true, true, true, true,
  };
  static_assert(arraysize(eliminated) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(eliminated); ++i) {
    bool actually_eliminated = (static_cast<int>(mirs_[i].dalvikInsn.opcode) == kMirOpNop);
    EXPECT_EQ(eliminated[i], actually_eliminated) << i;
  }
  // Check that we've created two single-input Phis to replace the IGET 8u and IGET 9u;
  // the IGET 6u and IGET 7u were killed without a replacement.
  BasicBlock* bb4 = cu_.mir_graph->GetBasicBlock(4);
  MIR* phi1 = bb4->first_mir_insn;
  ASSERT_TRUE(phi1 != nullptr);
  ASSERT_EQ(kMirOpPhi, static_cast<int>(phi1->dalvikInsn.opcode));
  MIR* phi2 = phi1->next;
  ASSERT_TRUE(phi2 != nullptr);
  ASSERT_EQ(kMirOpPhi, static_cast<int>(phi2->dalvikInsn.opcode));
  ASSERT_TRUE(phi2->next == &mirs_[6]);
  if (phi1->dalvikInsn.vA == 2u) {
    std::swap(phi1, phi2);
  }
  ASSERT_EQ(1, phi1->ssa_rep->num_uses);
  EXPECT_EQ(3, phi1->ssa_rep->uses[0]);
  ASSERT_EQ(1, phi1->ssa_rep->num_defs);
  EXPECT_EQ(8, phi1->ssa_rep->defs[0]);
  EXPECT_EQ(1u, phi1->dalvikInsn.vA);
  ASSERT_EQ(1, phi2->ssa_rep->num_uses);
  EXPECT_EQ(4, phi2->ssa_rep->uses[0]);
  ASSERT_EQ(1, phi2->ssa_rep->num_defs);
  EXPECT_EQ(9, phi2->ssa_rep->defs[0]);
  EXPECT_EQ(2u, phi2->dalvikInsn.vA);
}

TEST_F(GvnDeadCodeEliminationTestDiamond, KillChainInAnotherBlock2) {
  static const IFieldDef ifields[] = {
      { 0u, 1u, 0u, false, kDexMemAccessObject },  // linked list
  };
  static const MIRDef mirs[] = {
      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 0u),
      DEF_IGET(3, Instruction::IGET_OBJECT, 1u, 0u, 0u),
      DEF_IGET(3, Instruction::IGET_OBJECT, 2u, 1u, 0u),
      DEF_IGET(3, Instruction::IGET_OBJECT, 3u, 2u, 0u),
      DEF_IGET(3, Instruction::IGET_OBJECT, 4u, 3u, 0u),
      DEF_IFZ(3, Instruction::IF_NEZ, 4u),
      DEF_IGET(4, Instruction::IGET_OBJECT, 6u, 0u, 0u),
      DEF_IGET(4, Instruction::IGET_OBJECT, 7u, 6u, 0u),
      DEF_IGET(4, Instruction::IGET_OBJECT, 8u, 7u, 0u),
      DEF_CONST(4, Instruction::CONST, 9u, 1000),
  };

  static const int32_t sreg_to_vreg_map[] = { 0, 1, 2, 1, 2, 3 /* dummy */, 1, 2, 1, 2 };
  PrepareSRegToVRegMap(sreg_to_vreg_map);

  PrepareIFields(ifields);
  PrepareMIRs(mirs);
  PerformGVN_DCE();

  ASSERT_EQ(arraysize(mirs), value_names_.size());
  static const size_t diff_indexes[] = { 0, 1, 2, 3, 4, 9 };
  ExpectValueNamesNE(diff_indexes);
  EXPECT_EQ(value_names_[1], value_names_[6]);
  EXPECT_EQ(value_names_[2], value_names_[7]);
  EXPECT_EQ(value_names_[3], value_names_[8]);

  const size_t no_null_ck_indexes[] = { 1, 6, 7, 8 };
  ExpectNoNullCheck(no_null_ck_indexes);

  static const bool eliminated[] = {
      false, false, false, false, false, false, true, true, true, false,
  };
  static_assert(arraysize(eliminated) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(eliminated); ++i) {
    bool actually_eliminated = (static_cast<int>(mirs_[i].dalvikInsn.opcode) == kMirOpNop);
    EXPECT_EQ(eliminated[i], actually_eliminated) << i;
  }
  // Check that we've created a single-input Phi to replace the IGET 8u;
  // the IGET 6u and IGET 7u were killed without a replacement.
  BasicBlock* bb4 = cu_.mir_graph->GetBasicBlock(4);
  MIR* phi = bb4->first_mir_insn;
  ASSERT_TRUE(phi != nullptr);
  ASSERT_EQ(kMirOpPhi, static_cast<int>(phi->dalvikInsn.opcode));
  ASSERT_TRUE(phi->next == &mirs_[6]);
  ASSERT_EQ(1, phi->ssa_rep->num_uses);
  EXPECT_EQ(3, phi->ssa_rep->uses[0]);
  ASSERT_EQ(1, phi->ssa_rep->num_defs);
  EXPECT_EQ(8, phi->ssa_rep->defs[0]);
  EXPECT_EQ(1u, phi->dalvikInsn.vA);
}

TEST_F(GvnDeadCodeEliminationTestLoop, IFieldLoopVariable) {
  static const IFieldDef ifields[] = {
      { 0u, 1u, 0u, false, kDexMemAccessWord },
  };
  static const MIRDef mirs[] = {
      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 0u),
      DEF_CONST(3, Instruction::CONST, 1u, 1),
      DEF_CONST(3, Instruction::CONST, 2u, 0),
      DEF_IPUT(3, Instruction::IPUT, 2u, 0u, 0u),
      DEF_IGET(4, Instruction::IGET, 4u, 0u, 0u),
      DEF_BINOP(4, Instruction::ADD_INT, 5u, 4u, 1u),
      DEF_IPUT(4, Instruction::IPUT, 5u, 0u, 0u),
  };

  static const int32_t sreg_to_vreg_map[] = { 0, 1, 2, 3 /* dummy */, 2, 2 };
  PrepareSRegToVRegMap(sreg_to_vreg_map);

  PrepareIFields(ifields);
  PrepareMIRs(mirs);
  PerformGVN_DCE();

  ASSERT_EQ(arraysize(mirs), value_names_.size());
  static const size_t diff_indexes[] = { 0, 1, 2, 4, 5 };
  ExpectValueNamesNE(diff_indexes);

  const size_t no_null_ck_indexes[] = { 3, 4, 6 };
  ExpectNoNullCheck(no_null_ck_indexes);

  static const bool eliminated[] = {
      false, false, false, false, true, false, false,
  };
  static_assert(arraysize(eliminated) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(eliminated); ++i) {
    bool actually_eliminated = (static_cast<int>(mirs_[i].dalvikInsn.opcode) == kMirOpNop);
    EXPECT_EQ(eliminated[i], actually_eliminated) << i;
  }
  // Check that we've created a two-input Phi to replace the IGET 3u.
  BasicBlock* bb4 = cu_.mir_graph->GetBasicBlock(4);
  MIR* phi = bb4->first_mir_insn;
  ASSERT_TRUE(phi != nullptr);
  ASSERT_EQ(kMirOpPhi, static_cast<int>(phi->dalvikInsn.opcode));
  ASSERT_TRUE(phi->next == &mirs_[4]);
  ASSERT_EQ(2, phi->ssa_rep->num_uses);
  EXPECT_EQ(2, phi->ssa_rep->uses[0]);
  EXPECT_EQ(5, phi->ssa_rep->uses[1]);
  ASSERT_EQ(1, phi->ssa_rep->num_defs);
  EXPECT_EQ(4, phi->ssa_rep->defs[0]);
  EXPECT_EQ(2u, phi->dalvikInsn.vA);
}

TEST_F(GvnDeadCodeEliminationTestDiamond, LongOverlaps1) {
  static const MIRDef mirs[] = {
      DEF_CONST_WIDE(3, Instruction::CONST_WIDE, 0u, 1000u),
      DEF_CONST_WIDE(3, Instruction::CONST_WIDE, 2u, 1000u),
      DEF_MOVE_WIDE(4, Instruction::MOVE_WIDE, 4u, 0u),
      DEF_MOVE_WIDE(4, Instruction::MOVE_WIDE, 6u, 2u),
      DEF_MOVE_WIDE(4, Instruction::MOVE_WIDE, 8u, 4u),
      DEF_MOVE_WIDE(4, Instruction::MOVE_WIDE, 10u, 6u),
  };

  // The last insn should overlap the first and second.
  static const int32_t sreg_to_vreg_map[] = { 1, 2, 3, 4, 5, 6, 7, 8, 0, 1, 2, 3 };
  PrepareSRegToVRegMap(sreg_to_vreg_map);

  PrepareMIRs(mirs);
  static const int32_t wide_sregs[] = { 0, 2, 4, 6, 8, 10 };
  MarkAsWideSRegs(wide_sregs);
  PerformGVN_DCE();

  ASSERT_EQ(arraysize(mirs), value_names_.size());
  EXPECT_EQ(value_names_[0], value_names_[1]);
  EXPECT_EQ(value_names_[0], value_names_[2]);
  EXPECT_EQ(value_names_[0], value_names_[3]);
  EXPECT_EQ(value_names_[0], value_names_[4]);

  static const bool eliminated[] = {
      false, false, false, false, false, false,
  };
  static_assert(arraysize(eliminated) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(eliminated); ++i) {
    bool actually_eliminated = (static_cast<int>(mirs_[i].dalvikInsn.opcode) == kMirOpNop);
    EXPECT_EQ(eliminated[i], actually_eliminated) << i;
  }
}

TEST_F(GvnDeadCodeEliminationTestSimple, LongOverlaps2) {
  static const MIRDef mirs[] = {
      DEF_CONST_WIDE(3, Instruction::CONST_WIDE, 0u, 1000u),
      DEF_MOVE_WIDE(3, Instruction::MOVE_WIDE, 2u, 0u),
      DEF_MOVE_WIDE(3, Instruction::MOVE_WIDE, 4u, 2u),
  };

  // The last insn should overlap the first and second.
  static const int32_t sreg_to_vreg_map[] = { 0, 1, 2, 3, 1, 2 };
  PrepareSRegToVRegMap(sreg_to_vreg_map);

  PrepareMIRs(mirs);
  static const int32_t wide_sregs[] = { 0, 2, 4 };
  MarkAsWideSRegs(wide_sregs);
  PerformGVN_DCE();

  ASSERT_EQ(arraysize(mirs), value_names_.size());
  EXPECT_EQ(value_names_[0], value_names_[1]);
  EXPECT_EQ(value_names_[0], value_names_[2]);

  static const bool eliminated[] = {
      false, true, true,
  };
  static_assert(arraysize(eliminated) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(eliminated); ++i) {
    bool actually_eliminated = (static_cast<int>(mirs_[i].dalvikInsn.opcode) == kMirOpNop);
    EXPECT_EQ(eliminated[i], actually_eliminated) << i;
  }
  // Check that the CONST_WIDE registers have been correctly renamed.
  MIR* const_wide = &mirs_[0];
  ASSERT_EQ(2u, const_wide->ssa_rep->num_defs);
  EXPECT_EQ(4, const_wide->ssa_rep->defs[0]);
  EXPECT_EQ(5, const_wide->ssa_rep->defs[1]);
  EXPECT_EQ(1u, const_wide->dalvikInsn.vA);
}

TEST_F(GvnDeadCodeEliminationTestSimple, LongOverlaps3) {
  static const MIRDef mirs[] = {
      DEF_CONST_WIDE(3, Instruction::CONST_WIDE, 0u, 1000u),
      DEF_MOVE_WIDE(3, Instruction::MOVE_WIDE, 2u, 0u),
      DEF_MOVE_WIDE(3, Instruction::MOVE_WIDE, 4u, 2u),
  };

  // The last insn should overlap the first and second.
  static const int32_t sreg_to_vreg_map[] = { 2, 3, 0, 1, 1, 2 };
  PrepareSRegToVRegMap(sreg_to_vreg_map);

  PrepareMIRs(mirs);
  static const int32_t wide_sregs[] = { 0, 2, 4 };
  MarkAsWideSRegs(wide_sregs);
  PerformGVN_DCE();

  ASSERT_EQ(arraysize(mirs), value_names_.size());
  EXPECT_EQ(value_names_[0], value_names_[1]);
  EXPECT_EQ(value_names_[0], value_names_[2]);

  static const bool eliminated[] = {
      false, true, true,
  };
  static_assert(arraysize(eliminated) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(eliminated); ++i) {
    bool actually_eliminated = (static_cast<int>(mirs_[i].dalvikInsn.opcode) == kMirOpNop);
    EXPECT_EQ(eliminated[i], actually_eliminated) << i;
  }
  // Check that the CONST_WIDE registers have been correctly renamed.
  MIR* const_wide = &mirs_[0];
  ASSERT_EQ(2u, const_wide->ssa_rep->num_defs);
  EXPECT_EQ(4, const_wide->ssa_rep->defs[0]);
  EXPECT_EQ(5, const_wide->ssa_rep->defs[1]);
  EXPECT_EQ(1u, const_wide->dalvikInsn.vA);
}

TEST_F(GvnDeadCodeEliminationTestSimple, MixedOverlaps1) {
  static const MIRDef mirs[] = {
      DEF_CONST(3, Instruction::CONST, 0u, 1000u),
      DEF_MOVE(3, Instruction::MOVE, 1u, 0u),
      DEF_CONST(3, Instruction::CONST, 2u, 2000u),
      { 3, Instruction::INT_TO_LONG, 0, 0u, 1, { 2u }, 2, { 3u, 4u } },
      DEF_MOVE_WIDE(3, Instruction::MOVE_WIDE, 5u, 3u),
      DEF_CONST(3, Instruction::CONST, 7u, 3000u),
      DEF_CONST(3, Instruction::CONST, 8u, 4000u),
  };

  static const int32_t sreg_to_vreg_map[] = { 1, 2, 0, 0, 1, 3, 4, 0, 1 };
  PrepareSRegToVRegMap(sreg_to_vreg_map);

  PrepareMIRs(mirs);
  static const int32_t wide_sregs[] = { 3, 5 };
  MarkAsWideSRegs(wide_sregs);
  PerformGVN_DCE();

  ASSERT_EQ(arraysize(mirs), value_names_.size());
  static const size_t diff_indexes[] = { 0, 2, 3, 5, 6 };
  ExpectValueNamesNE(diff_indexes);
  EXPECT_EQ(value_names_[0], value_names_[1]);
  EXPECT_EQ(value_names_[3], value_names_[4]);

  static const bool eliminated[] = {
      false, true, false, false, true, false, false,
  };
  static_assert(arraysize(eliminated) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(eliminated); ++i) {
    bool actually_eliminated = (static_cast<int>(mirs_[i].dalvikInsn.opcode) == kMirOpNop);
    EXPECT_EQ(eliminated[i], actually_eliminated) << i;
  }
  // Check renamed registers in CONST.
  MIR* cst = &mirs_[0];
  ASSERT_EQ(Instruction::CONST, cst->dalvikInsn.opcode);
  ASSERT_EQ(0, cst->ssa_rep->num_uses);
  ASSERT_EQ(1, cst->ssa_rep->num_defs);
  EXPECT_EQ(1, cst->ssa_rep->defs[0]);
  EXPECT_EQ(2u, cst->dalvikInsn.vA);
  // Check renamed registers in INT_TO_LONG.
  MIR* int_to_long = &mirs_[3];
  ASSERT_EQ(Instruction::INT_TO_LONG, int_to_long->dalvikInsn.opcode);
  ASSERT_EQ(1, int_to_long->ssa_rep->num_uses);
  EXPECT_EQ(2, int_to_long->ssa_rep->uses[0]);
  ASSERT_EQ(2, int_to_long->ssa_rep->num_defs);
  EXPECT_EQ(5, int_to_long->ssa_rep->defs[0]);
  EXPECT_EQ(6, int_to_long->ssa_rep->defs[1]);
  EXPECT_EQ(3u, int_to_long->dalvikInsn.vA);
  EXPECT_EQ(0u, int_to_long->dalvikInsn.vB);
}

TEST_F(GvnDeadCodeEliminationTestSimple, UnusedRegs1) {
  static const MIRDef mirs[] = {
      DEF_CONST(3, Instruction::CONST, 0u, 1000u),
      DEF_CONST(3, Instruction::CONST, 1u, 2000u),
      DEF_BINOP(3, Instruction::ADD_INT, 2u, 1u, 0u),
      DEF_CONST(3, Instruction::CONST, 3u, 1000u),            // NOT killed (b/21702651).
      DEF_BINOP(3, Instruction::ADD_INT, 4u, 1u, 3u),         // Killed (RecordPass)
      DEF_CONST(3, Instruction::CONST, 5u, 2000u),            // Killed with 9u (BackwardPass)
      DEF_BINOP(3, Instruction::ADD_INT, 6u, 5u, 0u),         // Killed (RecordPass)
      DEF_CONST(3, Instruction::CONST, 7u, 4000u),
      DEF_MOVE(3, Instruction::MOVE, 8u, 0u),                 // Killed with 6u (BackwardPass)
  };

  static const int32_t sreg_to_vreg_map[] = { 1, 2, 3, 0, 3, 0, 3, 4, 0 };
  PrepareSRegToVRegMap(sreg_to_vreg_map);

  PrepareMIRs(mirs);
  PerformGVN_DCE();

  ASSERT_EQ(arraysize(mirs), value_names_.size());
  static const size_t diff_indexes[] = { 0, 1, 2, 7 };
  ExpectValueNamesNE(diff_indexes);
  EXPECT_EQ(value_names_[0], value_names_[3]);
  EXPECT_EQ(value_names_[2], value_names_[4]);
  EXPECT_EQ(value_names_[1], value_names_[5]);
  EXPECT_EQ(value_names_[2], value_names_[6]);
  EXPECT_EQ(value_names_[0], value_names_[8]);

  static const bool eliminated[] = {
      false, false, false, false, true, true, true, false, true,
  };
  static_assert(arraysize(eliminated) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(eliminated); ++i) {
    bool actually_eliminated = (static_cast<int>(mirs_[i].dalvikInsn.opcode) == kMirOpNop);
    EXPECT_EQ(eliminated[i], actually_eliminated) << i;
  }
}

TEST_F(GvnDeadCodeEliminationTestSimple, UnusedRegs2) {
  static const MIRDef mirs[] = {
      DEF_CONST(3, Instruction::CONST, 0u, 1000u),
      DEF_CONST(3, Instruction::CONST, 1u, 2000u),
      DEF_BINOP(3, Instruction::ADD_INT, 2u, 1u, 0u),
      DEF_CONST(3, Instruction::CONST, 3u, 1000u),            // Killed (BackwardPass; b/21702651)
      DEF_BINOP(3, Instruction::ADD_INT, 4u, 1u, 3u),         // Killed (RecordPass)
      DEF_CONST_WIDE(3, Instruction::CONST_WIDE, 5u, 4000u),
      { 3, Instruction::LONG_TO_INT, 0, 0u, 2, { 5u, 6u }, 1, { 7u } },
      DEF_BINOP(3, Instruction::ADD_INT, 8u, 7u, 0u),
      DEF_CONST_WIDE(3, Instruction::CONST_WIDE, 9u, 4000u),  // Killed with 12u (BackwardPass)
      DEF_CONST(3, Instruction::CONST, 11u, 6000u),
      { 3, Instruction::LONG_TO_INT, 0, 0u, 2, { 9u, 10u }, 1, { 12u } },  // Killed with 9u (BP)
  };

  static const int32_t sreg_to_vreg_map[] = {
      2, 3, 4, 1, 4, 5, 6 /* high word */, 0, 7, 0, 1 /* high word */, 8, 0
  };
  PrepareSRegToVRegMap(sreg_to_vreg_map);

  PrepareMIRs(mirs);
  static const int32_t wide_sregs[] = { 5, 9 };
  MarkAsWideSRegs(wide_sregs);
  PerformGVN_DCE();

  ASSERT_EQ(arraysize(mirs), value_names_.size());
  static const size_t diff_indexes[] = { 0, 1, 2, 5, 6, 7, 9 };
  ExpectValueNamesNE(diff_indexes);
  EXPECT_EQ(value_names_[0], value_names_[3]);
  EXPECT_EQ(value_names_[2], value_names_[4]);
  EXPECT_EQ(value_names_[5], value_names_[8]);
  EXPECT_EQ(value_names_[6], value_names_[10]);

  static const bool eliminated[] = {
      false, false, false, true, true, false, false, false, true, false, true,
  };
  static_assert(arraysize(eliminated) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(eliminated); ++i) {
    bool actually_eliminated = (static_cast<int>(mirs_[i].dalvikInsn.opcode) == kMirOpNop);
    EXPECT_EQ(eliminated[i], actually_eliminated) << i;
  }
}

TEST_F(GvnDeadCodeEliminationTestSimple, ArrayLengthThrows) {
  static const MIRDef mirs[] = {
      DEF_CONST(3, Instruction::CONST, 0u, 0),              // null
      DEF_UNOP(3, Instruction::ARRAY_LENGTH, 1u, 0u),       // null.length
      DEF_CONST(3, Instruction::CONST, 2u, 1000u),          // Overwrite the array-length dest.
  };

  static const int32_t sreg_to_vreg_map[] = { 0, 1, 1 };
  PrepareSRegToVRegMap(sreg_to_vreg_map);

  PrepareMIRs(mirs);
  PerformGVN_DCE();

  ASSERT_EQ(arraysize(mirs), value_names_.size());
  static const size_t diff_indexes[] = { 0, 1, 2 };
  ExpectValueNamesNE(diff_indexes);

  static const bool eliminated[] = {
      false, false, false,
  };
  static_assert(arraysize(eliminated) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(eliminated); ++i) {
    bool actually_eliminated = (static_cast<int>(mirs_[i].dalvikInsn.opcode) == kMirOpNop);
    EXPECT_EQ(eliminated[i], actually_eliminated) << i;
  }
}

TEST_F(GvnDeadCodeEliminationTestSimple, Dependancy) {
  static const MIRDef mirs[] = {
      DEF_MOVE(3, Instruction::MOVE, 5u, 1u),                 // move v5,v1
      DEF_MOVE(3, Instruction::MOVE, 6u, 1u),                 // move v12,v1
      DEF_MOVE(3, Instruction::MOVE, 7u, 0u),                 // move v13,v0
      DEF_MOVE_WIDE(3, Instruction::MOVE_WIDE, 8u, 2u),       // move v0_1,v2_3
      DEF_MOVE(3, Instruction::MOVE, 10u, 6u),                // move v3,v12
      DEF_MOVE(3, Instruction::MOVE, 11u, 4u),                // move v2,v4
      DEF_MOVE(3, Instruction::MOVE, 12u, 7u),                // move v4,v13
      DEF_MOVE(3, Instruction::MOVE, 13, 11u),                // move v12,v2
      DEF_MOVE(3, Instruction::MOVE, 14u, 10u),               // move v2,v3
      DEF_MOVE(3, Instruction::MOVE, 15u, 5u),                // move v3,v5
      DEF_MOVE(3, Instruction::MOVE, 16u, 12u),               // move v5,v4
  };

  static const int32_t sreg_to_vreg_map[] = { 0, 1, 2, 3, 4, 5, 12, 13, 0, 1, 3, 2, 4, 12, 2, 3, 5 };
  PrepareSRegToVRegMap(sreg_to_vreg_map);

  PrepareMIRs(mirs);
  static const int32_t wide_sregs[] = { 2, 8 };
  MarkAsWideSRegs(wide_sregs);
  PerformGVN_DCE();

  static const bool eliminated[] = {
      false, false, false, false, false, false, false, true, true, false, false,
  };
  static_assert(arraysize(eliminated) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(eliminated); ++i) {
    bool actually_eliminated = (static_cast<int>(mirs_[i].dalvikInsn.opcode) == kMirOpNop);
    EXPECT_EQ(eliminated[i], actually_eliminated) << i;
  }
}

}  // namespace art
