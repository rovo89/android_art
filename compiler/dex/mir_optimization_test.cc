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

#include <vector>

#include "compiler_internals.h"
#include "dataflow_iterator.h"
#include "dataflow_iterator-inl.h"
#include "gtest/gtest.h"

namespace art {

class ClassInitCheckEliminationTest : public testing::Test {
 protected:
  struct SFieldDef {
    uint16_t field_idx;
    uintptr_t declaring_dex_file;
    uint16_t declaring_class_idx;
    uint16_t declaring_field_idx;
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
    Instruction::Code opcode;
    BasicBlockId bbid;
    uint32_t field_or_method_info;
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

#define DEF_MIR(opcode, bb, field_info) \
    { opcode, bb, field_info }

  void DoPrepareSFields(const SFieldDef* defs, size_t count) {
    cu_.mir_graph->sfield_lowering_infos_.Reset();
    cu_.mir_graph->sfield_lowering_infos_.Resize(count);
    for (size_t i = 0u; i != count; ++i) {
      const SFieldDef* def = &defs[i];
      MirSFieldLoweringInfo field_info(def->field_idx);
      if (def->declaring_dex_file != 0u) {
        field_info.declaring_dex_file_ = reinterpret_cast<const DexFile*>(def->declaring_dex_file);
        field_info.declaring_class_idx_ = def->declaring_class_idx;
        field_info.declaring_field_idx_ = def->declaring_field_idx;
        field_info.flags_ = MirSFieldLoweringInfo::kFlagIsStatic;
      }
      ASSERT_EQ(def->declaring_dex_file != 0u, field_info.IsResolved());
      ASSERT_FALSE(field_info.IsInitialized());
      cu_.mir_graph->sfield_lowering_infos_.Insert(field_info);
    }
  }

  template <size_t count>
  void PrepareSFields(const SFieldDef (&defs)[count]) {
    DoPrepareSFields(defs, count);
  }

  void DoPrepareBasicBlocks(const BBDef* defs, size_t count) {
    cu_.mir_graph->block_id_map_.clear();
    cu_.mir_graph->block_list_.Reset();
    ASSERT_LT(3u, count);  // null, entry, exit and at least one bytecode block.
    ASSERT_EQ(kNullBlock, defs[0].type);
    ASSERT_EQ(kEntryBlock, defs[1].type);
    ASSERT_EQ(kExitBlock, defs[2].type);
    for (size_t i = 0u; i != count; ++i) {
      const BBDef* def = &defs[i];
      BasicBlock* bb = cu_.mir_graph->NewMemBB(def->type, i);
      cu_.mir_graph->block_list_.Insert(bb);
      if (def->num_successors <= 2) {
        bb->successor_block_list_type = kNotUsed;
        bb->successor_blocks = nullptr;
        bb->fall_through = (def->num_successors >= 1) ? def->successors[0] : 0u;
        bb->taken = (def->num_successors >= 2) ? def->successors[1] : 0u;
      } else {
        bb->successor_block_list_type = kPackedSwitch;
        bb->fall_through = 0u;
        bb->taken = 0u;
        bb->successor_blocks = new (&cu_.arena) GrowableArray<SuccessorBlockInfo*>(
            &cu_.arena, def->num_successors, kGrowableArraySuccessorBlocks);
        for (size_t j = 0u; j != def->num_successors; ++j) {
          SuccessorBlockInfo* successor_block_info =
              static_cast<SuccessorBlockInfo*>(cu_.arena.Alloc(sizeof(SuccessorBlockInfo),
                                                               kArenaAllocSuccessor));
          successor_block_info->block = j;
          successor_block_info->key = 0u;  // Not used by class init check elimination.
          bb->successor_blocks->Insert(successor_block_info);
        }
      }
      bb->predecessors = new (&cu_.arena) GrowableArray<BasicBlockId>(
          &cu_.arena, def->num_predecessors, kGrowableArrayPredecessors);
      for (size_t j = 0u; j != def->num_predecessors; ++j) {
        ASSERT_NE(0u, def->predecessors[j]);
        bb->predecessors->Insert(def->predecessors[j]);
      }
      if (def->type == kDalvikByteCode || def->type == kEntryBlock || def->type == kExitBlock) {
        bb->data_flow_info = static_cast<BasicBlockDataFlow*>(
            cu_.arena.Alloc(sizeof(BasicBlockDataFlow), kArenaAllocDFInfo));
      }
    }
    cu_.mir_graph->num_blocks_ = count;
    ASSERT_EQ(count, cu_.mir_graph->block_list_.Size());
    cu_.mir_graph->entry_block_ = cu_.mir_graph->block_list_.Get(1);
    ASSERT_EQ(kEntryBlock, cu_.mir_graph->entry_block_->block_type);
    cu_.mir_graph->exit_block_ = cu_.mir_graph->block_list_.Get(2);
    ASSERT_EQ(kExitBlock, cu_.mir_graph->exit_block_->block_type);
  }

  template <size_t count>
  void PrepareBasicBlocks(const BBDef (&defs)[count]) {
    DoPrepareBasicBlocks(defs, count);
  }

  void DoPrepareMIRs(const MIRDef* defs, size_t count) {
    mir_count_ = count;
    mirs_ = reinterpret_cast<MIR*>(cu_.arena.Alloc(sizeof(MIR) * count, kArenaAllocMIR));
    uint64_t merged_df_flags = 0u;
    for (size_t i = 0u; i != count; ++i) {
      const MIRDef* def = &defs[i];
      MIR* mir = &mirs_[i];
      mir->dalvikInsn.opcode = def->opcode;
      ASSERT_LT(def->bbid, cu_.mir_graph->block_list_.Size());
      BasicBlock* bb = cu_.mir_graph->block_list_.Get(def->bbid);
      bb->AppendMIR(mir);
      if (def->opcode >= Instruction::SGET && def->opcode <= Instruction::SPUT_SHORT) {
        ASSERT_LT(def->field_or_method_info, cu_.mir_graph->sfield_lowering_infos_.Size());
        mir->meta.sfield_lowering_info = def->field_or_method_info;
      }
      mir->ssa_rep = nullptr;
      mir->offset = 2 * i;  // All insns need to be at least 2 code units long.
      mir->optimization_flags = 0u;
      merged_df_flags |= MIRGraph::GetDataFlowAttributes(def->opcode);
    }
    cu_.mir_graph->merged_df_flags_ = merged_df_flags;

    code_item_ = static_cast<DexFile::CodeItem*>(
        cu_.arena.Alloc(sizeof(DexFile::CodeItem), kArenaAllocMisc));
    memset(code_item_, 0, sizeof(DexFile::CodeItem));
    code_item_->insns_size_in_code_units_ = 2u * count;
    cu_.mir_graph->current_code_item_ = cu_.code_item = code_item_;
  }

  template <size_t count>
  void PrepareMIRs(const MIRDef (&defs)[count]) {
    DoPrepareMIRs(defs, count);
  }

  void PerformClassInitCheckElimination() {
    cu_.mir_graph->SSATransformationStart();
    cu_.mir_graph->ComputeDFSOrders();
    cu_.mir_graph->ComputeDominators();
    cu_.mir_graph->ComputeTopologicalSortOrder();
    cu_.mir_graph->SSATransformationEnd();
    bool gate_result = cu_.mir_graph->EliminateClassInitChecksGate();
    ASSERT_TRUE(gate_result);
    LoopRepeatingTopologicalSortIterator iterator(cu_.mir_graph.get());
    bool change = false;
    for (BasicBlock* bb = iterator.Next(change); bb != nullptr; bb = iterator.Next(change)) {
      change = cu_.mir_graph->EliminateClassInitChecks(bb);
    }
    cu_.mir_graph->EliminateClassInitChecksEnd();
  }

  ClassInitCheckEliminationTest()
      : pool_(),
        cu_(&pool_),
        mir_count_(0u),
        mirs_(nullptr),
        code_item_(nullptr) {
    cu_.mir_graph.reset(new MIRGraph(&cu_, &cu_.arena));
  }

  ArenaPool pool_;
  CompilationUnit cu_;
  size_t mir_count_;
  MIR* mirs_;
  DexFile::CodeItem* code_item_;
};

TEST_F(ClassInitCheckEliminationTest, SingleBlock) {
  static const SFieldDef sfields[] = {
      { 0u, 1u, 0u, 0u },
      { 1u, 1u, 1u, 1u },
      { 2u, 1u, 2u, 2u },
      { 3u, 1u, 3u, 3u },  // Same declaring class as sfield[4].
      { 4u, 1u, 3u, 4u },  // Same declaring class as sfield[3].
      { 5u, 0u, 0u, 0u },  // Unresolved.
  };
  static const BBDef bbs[] = {
      DEF_BB(kNullBlock, DEF_SUCC0(), DEF_PRED0()),
      DEF_BB(kEntryBlock, DEF_SUCC1(3), DEF_PRED0()),
      DEF_BB(kExitBlock, DEF_SUCC0(), DEF_PRED1(3)),
      DEF_BB(kDalvikByteCode, DEF_SUCC1(2), DEF_PRED1(1)),
  };
  static const MIRDef mirs[] = {
      DEF_MIR(Instruction::SPUT, 3u, 5u),  // Unresolved.
      DEF_MIR(Instruction::SPUT, 3u, 0u),
      DEF_MIR(Instruction::SGET, 3u, 1u),
      DEF_MIR(Instruction::SGET, 3u, 2u),
      DEF_MIR(Instruction::SGET, 3u, 5u),  // Unresolved.
      DEF_MIR(Instruction::SGET, 3u, 0u),
      DEF_MIR(Instruction::SGET, 3u, 1u),
      DEF_MIR(Instruction::SGET, 3u, 2u),
      DEF_MIR(Instruction::SGET, 3u, 5u),  // Unresolved.
      DEF_MIR(Instruction::SGET, 3u, 3u),
      DEF_MIR(Instruction::SGET, 3u, 4u),
  };
  static const bool expected_ignore_clinit_check[] = {
      false, false, false, false, true, true, true, true, true, false, true
  };

  PrepareSFields(sfields);
  PrepareBasicBlocks(bbs);
  PrepareMIRs(mirs);
  PerformClassInitCheckElimination();
  ASSERT_EQ(arraysize(expected_ignore_clinit_check), mir_count_);
  for (size_t i = 0u; i != arraysize(mirs); ++i) {
    EXPECT_EQ(expected_ignore_clinit_check[i],
              (mirs_[i].optimization_flags & MIR_IGNORE_CLINIT_CHECK) != 0) << i;
  }
}

TEST_F(ClassInitCheckEliminationTest, Diamond) {
  static const SFieldDef sfields[] = {
      { 0u, 1u, 0u, 0u },
      { 1u, 1u, 1u, 1u },
      { 2u, 1u, 2u, 2u },
      { 3u, 1u, 3u, 3u },
      { 4u, 1u, 4u, 4u },
      { 5u, 1u, 5u, 5u },
      { 6u, 1u, 6u, 6u },
      { 7u, 1u, 7u, 7u },
      { 8u, 1u, 8u, 8u },  // Same declaring class as sfield[9].
      { 9u, 1u, 8u, 9u },  // Same declaring class as sfield[8].
      { 10u, 0u, 0u, 0u },  // Unresolved.
  };
  static const BBDef bbs[] = {
      DEF_BB(kNullBlock, DEF_SUCC0(), DEF_PRED0()),
      DEF_BB(kEntryBlock, DEF_SUCC1(3), DEF_PRED0()),
      DEF_BB(kExitBlock, DEF_SUCC0(), DEF_PRED1(6)),
      DEF_BB(kDalvikByteCode, DEF_SUCC2(4, 5), DEF_PRED1(1)),
      DEF_BB(kDalvikByteCode, DEF_SUCC1(6), DEF_PRED1(3)),
      DEF_BB(kDalvikByteCode, DEF_SUCC1(6), DEF_PRED1(3)),
      DEF_BB(kDalvikByteCode, DEF_SUCC1(2), DEF_PRED2(4, 5)),
  };
  static const MIRDef mirs[] = {
      // NOTE: MIRs here are ordered by unique tests. They will be put into appropriate blocks.
      DEF_MIR(Instruction::SGET, 3u, 10u),  // Unresolved.
      DEF_MIR(Instruction::SPUT, 3u, 10u),  // Unresolved.
      DEF_MIR(Instruction::SPUT, 3u, 0u),
      DEF_MIR(Instruction::SGET, 6u, 0u),  // Eliminated (block #3 dominates #6).
      DEF_MIR(Instruction::SPUT, 4u, 1u),
      DEF_MIR(Instruction::SGET, 6u, 1u),  // Not eliminated (block #4 doesn't dominate #6).
      DEF_MIR(Instruction::SGET, 3u, 2u),
      DEF_MIR(Instruction::SGET, 4u, 2u),  // Eliminated (block #3 dominates #4).
      DEF_MIR(Instruction::SGET, 3u, 3u),
      DEF_MIR(Instruction::SGET, 5u, 3u),  // Eliminated (block #3 dominates #5).
      DEF_MIR(Instruction::SGET, 3u, 4u),
      DEF_MIR(Instruction::SGET, 6u, 4u),  // Eliminated (block #3 dominates #6).
      DEF_MIR(Instruction::SGET, 4u, 5u),
      DEF_MIR(Instruction::SGET, 6u, 5u),  // Not eliminated (block #4 doesn't dominate #6).
      DEF_MIR(Instruction::SGET, 5u, 6u),
      DEF_MIR(Instruction::SGET, 6u, 6u),  // Not eliminated (block #5 doesn't dominate #6).
      DEF_MIR(Instruction::SGET, 4u, 7u),
      DEF_MIR(Instruction::SGET, 5u, 7u),
      DEF_MIR(Instruction::SGET, 6u, 7u),  // Eliminated (initialized in both blocks #3 and #4).
      DEF_MIR(Instruction::SGET, 4u, 8u),
      DEF_MIR(Instruction::SGET, 5u, 9u),
      DEF_MIR(Instruction::SGET, 6u, 8u),  // Eliminated (with sfield[9] in block #5).
      DEF_MIR(Instruction::SPUT, 6u, 9u),  // Eliminated (with sfield[8] in block #4).
  };
  static const bool expected_ignore_clinit_check[] = {
      false, true,          // Unresolved: sfield[10], method[2]
      false, true,          // sfield[0]
      false, false,         // sfield[1]
      false, true,          // sfield[2]
      false, true,          // sfield[3]
      false, true,          // sfield[4]
      false, false,         // sfield[5]
      false, false,         // sfield[6]
      false, false, true,   // sfield[7]
      false, false, true, true,  // sfield[8], sfield[9]
  };

  PrepareSFields(sfields);
  PrepareBasicBlocks(bbs);
  PrepareMIRs(mirs);
  PerformClassInitCheckElimination();
  ASSERT_EQ(arraysize(expected_ignore_clinit_check), mir_count_);
  for (size_t i = 0u; i != arraysize(mirs); ++i) {
    EXPECT_EQ(expected_ignore_clinit_check[i],
              (mirs_[i].optimization_flags & MIR_IGNORE_CLINIT_CHECK) != 0) << i;
  }
}

TEST_F(ClassInitCheckEliminationTest, Loop) {
  static const SFieldDef sfields[] = {
      { 0u, 1u, 0u, 0u },
      { 1u, 1u, 1u, 1u },
  };
  static const BBDef bbs[] = {
      DEF_BB(kNullBlock, DEF_SUCC0(), DEF_PRED0()),
      DEF_BB(kEntryBlock, DEF_SUCC1(3), DEF_PRED0()),
      DEF_BB(kExitBlock, DEF_SUCC0(), DEF_PRED1(5)),
      DEF_BB(kDalvikByteCode, DEF_SUCC1(4), DEF_PRED1(1)),
      DEF_BB(kDalvikByteCode, DEF_SUCC2(5, 4), DEF_PRED2(3, 4)),  // "taken" loops to self.
      DEF_BB(kDalvikByteCode, DEF_SUCC1(2), DEF_PRED1(4)),
  };
  static const MIRDef mirs[] = {
      DEF_MIR(Instruction::SGET, 3u, 0u),
      DEF_MIR(Instruction::SGET, 4u, 1u),
      DEF_MIR(Instruction::SGET, 5u, 0u),  // Eliminated.
      DEF_MIR(Instruction::SGET, 5u, 1u),  // Eliminated.
  };
  static const bool expected_ignore_clinit_check[] = {
      false, false, true, true
  };

  PrepareSFields(sfields);
  PrepareBasicBlocks(bbs);
  PrepareMIRs(mirs);
  PerformClassInitCheckElimination();
  ASSERT_EQ(arraysize(expected_ignore_clinit_check), mir_count_);
  for (size_t i = 0u; i != arraysize(mirs); ++i) {
    EXPECT_EQ(expected_ignore_clinit_check[i],
              (mirs_[i].optimization_flags & MIR_IGNORE_CLINIT_CHECK) != 0) << i;
  }
}

TEST_F(ClassInitCheckEliminationTest, Catch) {
  static const SFieldDef sfields[] = {
      { 0u, 1u, 0u, 0u },
      { 1u, 1u, 1u, 1u },
      { 2u, 1u, 2u, 2u },
      { 3u, 1u, 3u, 3u },
  };
  static const BBDef bbs[] = {
      DEF_BB(kNullBlock, DEF_SUCC0(), DEF_PRED0()),
      DEF_BB(kEntryBlock, DEF_SUCC1(3), DEF_PRED0()),
      DEF_BB(kExitBlock, DEF_SUCC0(), DEF_PRED1(6)),
      DEF_BB(kDalvikByteCode, DEF_SUCC1(4), DEF_PRED1(1)),     // The top.
      DEF_BB(kDalvikByteCode, DEF_SUCC1(6), DEF_PRED1(3)),     // The throwing insn.
      DEF_BB(kDalvikByteCode, DEF_SUCC1(6), DEF_PRED1(3)),     // Catch handler.
      DEF_BB(kDalvikByteCode, DEF_SUCC1(2), DEF_PRED2(4, 5)),  // The merged block.
  };
  static const MIRDef mirs[] = {
      DEF_MIR(Instruction::SGET, 3u, 0u),  // Before the exception edge.
      DEF_MIR(Instruction::SGET, 3u, 1u),  // Before the exception edge.
      DEF_MIR(Instruction::SGET, 4u, 2u),  // After the exception edge.
      DEF_MIR(Instruction::SGET, 4u, 3u),  // After the exception edge.
      DEF_MIR(Instruction::SGET, 5u, 0u),  // In catch handler; class init check eliminated.
      DEF_MIR(Instruction::SGET, 5u, 2u),  // In catch handler; class init check not eliminated.
      DEF_MIR(Instruction::SGET, 6u, 0u),  // Class init check eliminated.
      DEF_MIR(Instruction::SGET, 6u, 1u),  // Class init check eliminated.
      DEF_MIR(Instruction::SGET, 6u, 2u),  // Class init check eliminated.
      DEF_MIR(Instruction::SGET, 6u, 3u),  // Class init check not eliminated.
  };
  static const bool expected_ignore_clinit_check[] = {
      false, false, false, false, true, false, true, true, true, false
  };

  PrepareSFields(sfields);
  PrepareBasicBlocks(bbs);
  BasicBlock* catch_handler = cu_.mir_graph->GetBasicBlock(5u);
  catch_handler->catch_entry = true;
  // Add successor block info to the check block.
  BasicBlock* check_bb = cu_.mir_graph->GetBasicBlock(3u);
  check_bb->successor_block_list_type = kCatch;
  check_bb->successor_blocks = new (&cu_.arena) GrowableArray<SuccessorBlockInfo*>(
      &cu_.arena, 2, kGrowableArraySuccessorBlocks);
  SuccessorBlockInfo* successor_block_info = reinterpret_cast<SuccessorBlockInfo*>
      (cu_.arena.Alloc(sizeof(SuccessorBlockInfo), kArenaAllocSuccessor));
  successor_block_info->block = catch_handler->id;
  check_bb->successor_blocks->Insert(successor_block_info);
  PrepareMIRs(mirs);
  PerformClassInitCheckElimination();
  ASSERT_EQ(arraysize(expected_ignore_clinit_check), mir_count_);
  for (size_t i = 0u; i != arraysize(mirs); ++i) {
    EXPECT_EQ(expected_ignore_clinit_check[i],
              (mirs_[i].optimization_flags & MIR_IGNORE_CLINIT_CHECK) != 0) << i;
  }
}

}  // namespace art
