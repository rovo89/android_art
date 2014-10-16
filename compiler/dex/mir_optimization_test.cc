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

class MirOptimizationTest : public testing::Test {
 protected:
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
    BasicBlockId bbid;
    Instruction::Code opcode;
    uint32_t field_info;
    uint32_t vA;
    uint32_t vB;
    uint32_t vC;
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
      }
    }
    cu_.mir_graph->num_blocks_ = count;
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

  void PrepareSingleBlock() {
    static const BBDef bbs[] = {
        DEF_BB(kNullBlock, DEF_SUCC0(), DEF_PRED0()),
        DEF_BB(kEntryBlock, DEF_SUCC1(3), DEF_PRED0()),
        DEF_BB(kExitBlock, DEF_SUCC0(), DEF_PRED1(3)),
        DEF_BB(kDalvikByteCode, DEF_SUCC1(2), DEF_PRED1(1)),
    };
    PrepareBasicBlocks(bbs);
  }

  void PrepareDiamond() {
    static const BBDef bbs[] = {
        DEF_BB(kNullBlock, DEF_SUCC0(), DEF_PRED0()),
        DEF_BB(kEntryBlock, DEF_SUCC1(3), DEF_PRED0()),
        DEF_BB(kExitBlock, DEF_SUCC0(), DEF_PRED1(6)),
        DEF_BB(kDalvikByteCode, DEF_SUCC2(4, 5), DEF_PRED1(1)),
        DEF_BB(kDalvikByteCode, DEF_SUCC1(6), DEF_PRED1(3)),
        DEF_BB(kDalvikByteCode, DEF_SUCC1(6), DEF_PRED1(3)),
        DEF_BB(kDalvikByteCode, DEF_SUCC1(2), DEF_PRED2(4, 5)),
    };
    PrepareBasicBlocks(bbs);
  }

  void PrepareLoop() {
    static const BBDef bbs[] = {
        DEF_BB(kNullBlock, DEF_SUCC0(), DEF_PRED0()),
        DEF_BB(kEntryBlock, DEF_SUCC1(3), DEF_PRED0()),
        DEF_BB(kExitBlock, DEF_SUCC0(), DEF_PRED1(5)),
        DEF_BB(kDalvikByteCode, DEF_SUCC1(4), DEF_PRED1(1)),
        DEF_BB(kDalvikByteCode, DEF_SUCC2(5, 4), DEF_PRED2(3, 4)),  // "taken" loops to self.
        DEF_BB(kDalvikByteCode, DEF_SUCC1(2), DEF_PRED1(4)),
    };
    PrepareBasicBlocks(bbs);
  }

  void PrepareCatch() {
    static const BBDef bbs[] = {
        DEF_BB(kNullBlock, DEF_SUCC0(), DEF_PRED0()),
        DEF_BB(kEntryBlock, DEF_SUCC1(3), DEF_PRED0()),
        DEF_BB(kExitBlock, DEF_SUCC0(), DEF_PRED1(6)),
        DEF_BB(kDalvikByteCode, DEF_SUCC1(4), DEF_PRED1(1)),     // The top.
        DEF_BB(kDalvikByteCode, DEF_SUCC1(6), DEF_PRED1(3)),     // The throwing insn.
        DEF_BB(kDalvikByteCode, DEF_SUCC1(6), DEF_PRED1(3)),     // Catch handler.
        DEF_BB(kDalvikByteCode, DEF_SUCC1(2), DEF_PRED2(4, 5)),  // The merged block.
    };
    PrepareBasicBlocks(bbs);
    BasicBlock* catch_handler = cu_.mir_graph->GetBasicBlock(5u);
    catch_handler->catch_entry = true;
    // Add successor block info to the check block.
    BasicBlock* check_bb = cu_.mir_graph->GetBasicBlock(3u);
    check_bb->successor_block_list_type = kCatch;
    SuccessorBlockInfo* successor_block_info = reinterpret_cast<SuccessorBlockInfo*>
        (cu_.arena.Alloc(sizeof(SuccessorBlockInfo), kArenaAllocSuccessor));
    successor_block_info->block = catch_handler->id;
    check_bb->successor_blocks.push_back(successor_block_info);
  }

  void DoPrepareMIRs(const MIRDef* defs, size_t count) {
    mir_count_ = count;
    mirs_ = reinterpret_cast<MIR*>(cu_.arena.Alloc(sizeof(MIR) * count, kArenaAllocMIR));
    uint64_t merged_df_flags = 0u;
    for (size_t i = 0u; i != count; ++i) {
      const MIRDef* def = &defs[i];
      MIR* mir = &mirs_[i];
      mir->dalvikInsn.opcode = def->opcode;
      ASSERT_LT(def->bbid, cu_.mir_graph->block_list_.size());
      BasicBlock* bb = cu_.mir_graph->block_list_[def->bbid];
      bb->AppendMIR(mir);
      if (def->opcode >= Instruction::SGET && def->opcode <= Instruction::SPUT_SHORT) {
        ASSERT_LT(def->field_info, cu_.mir_graph->sfield_lowering_infos_.size());
        mir->meta.sfield_lowering_info = def->field_info;
      } else if (def->opcode >= Instruction::IGET && def->opcode <= Instruction::IPUT_SHORT) {
        ASSERT_LT(def->field_info, cu_.mir_graph->ifield_lowering_infos_.size());
        mir->meta.ifield_lowering_info = def->field_info;
      }
      mir->dalvikInsn.vA = def->vA;
      mir->dalvikInsn.vB = def->vB;
      mir->dalvikInsn.vC = def->vC;
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
    cu_.mir_graph->current_code_item_ = code_item_;
  }

  template <size_t count>
  void PrepareMIRs(const MIRDef (&defs)[count]) {
    DoPrepareMIRs(defs, count);
  }

  MirOptimizationTest()
      : pool_(),
        cu_(&pool_),
        mir_count_(0u),
        mirs_(nullptr),
        code_item_(nullptr) {
    cu_.mir_graph.reset(new MIRGraph(&cu_, &cu_.arena));
    cu_.access_flags = kAccStatic;  // Don't let "this" interfere with this test.
  }

  ArenaPool pool_;
  CompilationUnit cu_;
  size_t mir_count_;
  MIR* mirs_;
  DexFile::CodeItem* code_item_;
};

class ClassInitCheckEliminationTest : public MirOptimizationTest {
 protected:
  struct SFieldDef {
    uint16_t field_idx;
    uintptr_t declaring_dex_file;
    uint16_t declaring_class_idx;
    uint16_t declaring_field_idx;
  };

  void DoPrepareSFields(const SFieldDef* defs, size_t count) {
    cu_.mir_graph->sfield_lowering_infos_.clear();
    cu_.mir_graph->sfield_lowering_infos_.reserve(count);
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
      cu_.mir_graph->sfield_lowering_infos_.push_back(field_info);
    }
  }

  template <size_t count>
  void PrepareSFields(const SFieldDef (&defs)[count]) {
    DoPrepareSFields(defs, count);
  }

  void PerformClassInitCheckElimination() {
    cu_.mir_graph->ComputeDFSOrders();
    bool gate_result = cu_.mir_graph->EliminateClassInitChecksGate();
    ASSERT_TRUE(gate_result);
    RepeatingPreOrderDfsIterator iterator(cu_.mir_graph.get());
    bool change = false;
    for (BasicBlock* bb = iterator.Next(change); bb != nullptr; bb = iterator.Next(change)) {
      change = cu_.mir_graph->EliminateClassInitChecks(bb);
    }
    cu_.mir_graph->EliminateClassInitChecksEnd();
  }

  ClassInitCheckEliminationTest()
      : MirOptimizationTest() {
  }
};

class NullCheckEliminationTest : public MirOptimizationTest {
 protected:
  struct IFieldDef {
    uint16_t field_idx;
    uintptr_t declaring_dex_file;
    uint16_t declaring_class_idx;
    uint16_t declaring_field_idx;
  };

  void DoPrepareIFields(const IFieldDef* defs, size_t count) {
    cu_.mir_graph->ifield_lowering_infos_.clear();
    cu_.mir_graph->ifield_lowering_infos_.reserve(count);
    for (size_t i = 0u; i != count; ++i) {
      const IFieldDef* def = &defs[i];
      MirIFieldLoweringInfo field_info(def->field_idx);
      if (def->declaring_dex_file != 0u) {
        field_info.declaring_dex_file_ = reinterpret_cast<const DexFile*>(def->declaring_dex_file);
        field_info.declaring_class_idx_ = def->declaring_class_idx;
        field_info.declaring_field_idx_ = def->declaring_field_idx;
      }
      ASSERT_EQ(def->declaring_dex_file != 0u, field_info.IsResolved());
      cu_.mir_graph->ifield_lowering_infos_.push_back(field_info);
    }
  }

  template <size_t count>
  void PrepareIFields(const IFieldDef (&defs)[count]) {
    DoPrepareIFields(defs, count);
  }

  void PerformNullCheckElimination() {
    // Make vregs in range [100, 1000) input registers, i.e. requiring a null check.
    code_item_->registers_size_ = 1000;
    code_item_->ins_size_ = 900;

    cu_.mir_graph->ComputeDFSOrders();
    bool gate_result = cu_.mir_graph->EliminateNullChecksGate();
    ASSERT_TRUE(gate_result);
    RepeatingPreOrderDfsIterator iterator(cu_.mir_graph.get());
    bool change = false;
    for (BasicBlock* bb = iterator.Next(change); bb != nullptr; bb = iterator.Next(change)) {
      change = cu_.mir_graph->EliminateNullChecks(bb);
    }
    cu_.mir_graph->EliminateNullChecksEnd();
  }

  NullCheckEliminationTest()
      : MirOptimizationTest() {
  }
};

#define DEF_SGET_SPUT_V0(bb, opcode, field_info) \
    { bb, opcode, field_info, 0u, 0u, 0u }

TEST_F(ClassInitCheckEliminationTest, SingleBlock) {
  static const SFieldDef sfields[] = {
      { 0u, 1u, 0u, 0u },
      { 1u, 1u, 1u, 1u },
      { 2u, 1u, 2u, 2u },
      { 3u, 1u, 3u, 3u },  // Same declaring class as sfield[4].
      { 4u, 1u, 3u, 4u },  // Same declaring class as sfield[3].
      { 5u, 0u, 0u, 0u },  // Unresolved.
  };
  static const MIRDef mirs[] = {
      DEF_SGET_SPUT_V0(3u, Instruction::SPUT, 5u),  // Unresolved.
      DEF_SGET_SPUT_V0(3u, Instruction::SPUT, 0u),
      DEF_SGET_SPUT_V0(3u, Instruction::SGET, 1u),
      DEF_SGET_SPUT_V0(3u, Instruction::SGET, 2u),
      DEF_SGET_SPUT_V0(3u, Instruction::SGET, 5u),  // Unresolved.
      DEF_SGET_SPUT_V0(3u, Instruction::SGET, 0u),
      DEF_SGET_SPUT_V0(3u, Instruction::SGET, 1u),
      DEF_SGET_SPUT_V0(3u, Instruction::SGET, 2u),
      DEF_SGET_SPUT_V0(3u, Instruction::SGET, 5u),  // Unresolved.
      DEF_SGET_SPUT_V0(3u, Instruction::SGET, 3u),
      DEF_SGET_SPUT_V0(3u, Instruction::SGET, 4u),
  };
  static const bool expected_ignore_clinit_check[] = {
      false, false, false, false, true, true, true, true, true, false, true
  };

  PrepareSFields(sfields);
  PrepareSingleBlock();
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
  static const MIRDef mirs[] = {
      // NOTE: MIRs here are ordered by unique tests. They will be put into appropriate blocks.
      DEF_SGET_SPUT_V0(3u, Instruction::SGET, 10u),  // Unresolved.
      DEF_SGET_SPUT_V0(3u, Instruction::SPUT, 10u),  // Unresolved.
      DEF_SGET_SPUT_V0(3u, Instruction::SPUT, 0u),
      DEF_SGET_SPUT_V0(6u, Instruction::SGET, 0u),  // Eliminated (BB #3 dominates #6).
      DEF_SGET_SPUT_V0(4u, Instruction::SPUT, 1u),
      DEF_SGET_SPUT_V0(6u, Instruction::SGET, 1u),  // Not eliminated (BB #4 doesn't dominate #6).
      DEF_SGET_SPUT_V0(3u, Instruction::SGET, 2u),
      DEF_SGET_SPUT_V0(4u, Instruction::SGET, 2u),  // Eliminated (BB #3 dominates #4).
      DEF_SGET_SPUT_V0(3u, Instruction::SGET, 3u),
      DEF_SGET_SPUT_V0(5u, Instruction::SGET, 3u),  // Eliminated (BB #3 dominates #5).
      DEF_SGET_SPUT_V0(3u, Instruction::SGET, 4u),
      DEF_SGET_SPUT_V0(6u, Instruction::SGET, 4u),  // Eliminated (BB #3 dominates #6).
      DEF_SGET_SPUT_V0(4u, Instruction::SGET, 5u),
      DEF_SGET_SPUT_V0(6u, Instruction::SGET, 5u),  // Not eliminated (BB #4 doesn't dominate #6).
      DEF_SGET_SPUT_V0(5u, Instruction::SGET, 6u),
      DEF_SGET_SPUT_V0(6u, Instruction::SGET, 6u),  // Not eliminated (BB #5 doesn't dominate #6).
      DEF_SGET_SPUT_V0(4u, Instruction::SGET, 7u),
      DEF_SGET_SPUT_V0(5u, Instruction::SGET, 7u),
      DEF_SGET_SPUT_V0(6u, Instruction::SGET, 7u),  // Eliminated (initialized in both #3 and #4).
      DEF_SGET_SPUT_V0(4u, Instruction::SGET, 8u),
      DEF_SGET_SPUT_V0(5u, Instruction::SGET, 9u),
      DEF_SGET_SPUT_V0(6u, Instruction::SGET, 8u),  // Eliminated (with sfield[9] in BB #5).
      DEF_SGET_SPUT_V0(6u, Instruction::SPUT, 9u),  // Eliminated (with sfield[8] in BB #4).
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
  PrepareDiamond();
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
  static const MIRDef mirs[] = {
      DEF_SGET_SPUT_V0(3u, Instruction::SGET, 0u),
      DEF_SGET_SPUT_V0(4u, Instruction::SGET, 1u),
      DEF_SGET_SPUT_V0(5u, Instruction::SGET, 0u),  // Eliminated.
      DEF_SGET_SPUT_V0(5u, Instruction::SGET, 1u),  // Eliminated.
  };
  static const bool expected_ignore_clinit_check[] = {
      false, false, true, true
  };

  PrepareSFields(sfields);
  PrepareLoop();
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
  static const MIRDef mirs[] = {
      DEF_SGET_SPUT_V0(3u, Instruction::SGET, 0u),  // Before the exception edge.
      DEF_SGET_SPUT_V0(3u, Instruction::SGET, 1u),  // Before the exception edge.
      DEF_SGET_SPUT_V0(4u, Instruction::SGET, 2u),  // After the exception edge.
      DEF_SGET_SPUT_V0(4u, Instruction::SGET, 3u),  // After the exception edge.
      DEF_SGET_SPUT_V0(5u, Instruction::SGET, 0u),  // In catch handler; clinit check eliminated.
      DEF_SGET_SPUT_V0(5u, Instruction::SGET, 2u),  // In catch handler; clinit check not eliminated.
      DEF_SGET_SPUT_V0(6u, Instruction::SGET, 0u),  // Class init check eliminated.
      DEF_SGET_SPUT_V0(6u, Instruction::SGET, 1u),  // Class init check eliminated.
      DEF_SGET_SPUT_V0(6u, Instruction::SGET, 2u),  // Class init check eliminated.
      DEF_SGET_SPUT_V0(6u, Instruction::SGET, 3u),  // Class init check not eliminated.
  };
  static const bool expected_ignore_clinit_check[] = {
      false, false, false, false, true, false, true, true, true, false
  };

  PrepareSFields(sfields);
  PrepareCatch();
  PrepareMIRs(mirs);
  PerformClassInitCheckElimination();
  ASSERT_EQ(arraysize(expected_ignore_clinit_check), mir_count_);
  for (size_t i = 0u; i != arraysize(mirs); ++i) {
    EXPECT_EQ(expected_ignore_clinit_check[i],
              (mirs_[i].optimization_flags & MIR_IGNORE_CLINIT_CHECK) != 0) << i;
  }
}

#define DEF_IGET_IPUT(bb, opcode, vA, vB, field_info) \
    { bb, opcode, field_info, vA, vB, 0u }
#define DEF_AGET_APUT(bb, opcode, vA, vB, vC) \
    { bb, opcode, 0u, vA, vB, vC }
#define DEF_INVOKE(bb, opcode, vC) \
    { bb, opcode, 0u, 0u, 0u, vC }
#define DEF_OTHER1(bb, opcode, vA) \
    { bb, opcode, 0u, vA, 0u, 0u }
#define DEF_OTHER2(bb, opcode, vA, vB) \
    { bb, opcode, 0u, vA, vB, 0u }

TEST_F(NullCheckEliminationTest, SingleBlock) {
  static const IFieldDef ifields[] = {
      { 0u, 1u, 0u, 0u },
      { 1u, 1u, 0u, 1u },
      { 2u, 1u, 0u, 2u },  // Object.
  };
  static const MIRDef mirs[] = {
      DEF_IGET_IPUT(3u, Instruction::IGET_OBJECT, 0u, 100u, 2u),
      DEF_IGET_IPUT(3u, Instruction::IGET, 1u, 0u, 1u),
      DEF_IGET_IPUT(3u, Instruction::IGET_OBJECT, 2u, 100u, 2u),  // Differs from 0u (no LVN here).
      DEF_IGET_IPUT(3u, Instruction::IGET, 3u, 2u, 1u),
      DEF_IGET_IPUT(3u, Instruction::IGET, 4u, 101u, 0u),
      DEF_IGET_IPUT(3u, Instruction::IGET, 5u, 102u, 0u),
      DEF_IGET_IPUT(3u, Instruction::IGET, 6u, 103u, 0u),
      DEF_IGET_IPUT(3u, Instruction::IGET, 7u, 103u, 1u),
      DEF_IGET_IPUT(3u, Instruction::IPUT, 8u, 104u, 0u),
      DEF_IGET_IPUT(3u, Instruction::IPUT, 9u, 104u, 1u),
      DEF_IGET_IPUT(3u, Instruction::IGET, 10u, 105u, 0u),
      DEF_IGET_IPUT(3u, Instruction::IPUT, 11u, 105u, 1u),
      DEF_IGET_IPUT(3u, Instruction::IPUT, 12u, 106u, 0u),
      DEF_IGET_IPUT(3u, Instruction::IGET, 13u, 106u, 1u),
      DEF_INVOKE(3u, Instruction::INVOKE_DIRECT, 107),
      DEF_IGET_IPUT(3u, Instruction::IGET, 15u, 107u, 1u),
      DEF_IGET_IPUT(3u, Instruction::IGET, 16u, 108u, 0u),
      DEF_INVOKE(3u, Instruction::INVOKE_DIRECT, 108),
      DEF_AGET_APUT(3u, Instruction::AGET, 18u, 109u, 110u),
      DEF_AGET_APUT(3u, Instruction::APUT, 19u, 109u, 111u),
      DEF_OTHER2(3u, Instruction::ARRAY_LENGTH, 20u, 112u),
      DEF_AGET_APUT(3u, Instruction::AGET, 21u, 112u, 113u),
      DEF_OTHER1(3u, Instruction::MONITOR_ENTER, 114u),
      DEF_OTHER1(3u, Instruction::MONITOR_EXIT, 114u),
  };
  static const bool expected_ignore_null_check[] = {
      false, false, true, false /* Not doing LVN. */,
      false, true /* Set before running NCE. */,
      false, true,  // IGET, IGET
      false, true,  // IPUT, IPUT
      false, true,  // IGET, IPUT
      false, true,  // IPUT, IGET
      false, true,  // INVOKE, IGET
      false, true,  // IGET, INVOKE
      false, true,  // AGET, APUT
      false, true,  // ARRAY_LENGTH, AGET
      false, true,  // MONITOR_ENTER, MONITOR_EXIT
  };

  PrepareIFields(ifields);
  PrepareSingleBlock();
  PrepareMIRs(mirs);

  // Mark IGET 5u as null-checked to test that NCE doesn't clear this flag.
  mirs_[5u].optimization_flags |= MIR_IGNORE_NULL_CHECK;

  PerformNullCheckElimination();
  ASSERT_EQ(arraysize(expected_ignore_null_check), mir_count_);
  for (size_t i = 0u; i != arraysize(mirs); ++i) {
    EXPECT_EQ(expected_ignore_null_check[i],
              (mirs_[i].optimization_flags & MIR_IGNORE_NULL_CHECK) != 0) << i;
  }
}

TEST_F(NullCheckEliminationTest, Diamond) {
  static const IFieldDef ifields[] = {
      { 0u, 1u, 0u, 0u },
      { 1u, 1u, 0u, 1u },
      { 2u, 1u, 0u, 2u },  // int[].
  };
  static const MIRDef mirs[] = {
      // NOTE: MIRs here are ordered by unique tests. They will be put into appropriate blocks.
      DEF_IGET_IPUT(3u, Instruction::IPUT, 0u, 100u, 0u),
      DEF_IGET_IPUT(6u, Instruction::IGET, 1u, 100u, 1u),  // Eliminated (BB #3 dominates #6).
      DEF_IGET_IPUT(3u, Instruction::IGET, 2u, 101u, 0u),
      DEF_IGET_IPUT(4u, Instruction::IPUT, 3u, 101u, 0u),  // Eliminated (BB #3 dominates #4).
      DEF_IGET_IPUT(3u, Instruction::IGET, 4u, 102u, 0u),
      DEF_IGET_IPUT(5u, Instruction::IPUT, 5u, 102u, 1u),  // Eliminated (BB #3 dominates #5).
      DEF_IGET_IPUT(4u, Instruction::IPUT, 6u, 103u, 0u),
      DEF_IGET_IPUT(6u, Instruction::IPUT, 7u, 103u, 1u),  // Not eliminated (going through BB #5).
      DEF_IGET_IPUT(5u, Instruction::IGET, 8u, 104u, 1u),
      DEF_IGET_IPUT(6u, Instruction::IGET, 9u, 104u, 0u),  // Not eliminated (going through BB #4).
      DEF_INVOKE(4u, Instruction::INVOKE_DIRECT, 105u),
      DEF_IGET_IPUT(5u, Instruction::IGET, 11u, 105u, 1u),
      DEF_IGET_IPUT(6u, Instruction::IPUT, 12u, 105u, 0u),  // Eliminated.
      DEF_IGET_IPUT(3u, Instruction::IGET_OBJECT, 13u, 106u, 2u),
      DEF_OTHER1(3u, Instruction::IF_EQZ, 13u),            // Last insn in the BB #3.
      DEF_OTHER2(5u, Instruction::NEW_ARRAY, 13u, 107u),
      DEF_AGET_APUT(6u, Instruction::AGET, 16u, 13u, 108u),  // Eliminated.
  };
  static const bool expected_ignore_null_check[] = {
      false, true,   // BB #3 IPUT, BB #6 IGET
      false, true,   // BB #3 IGET, BB #4 IPUT
      false, true,   // BB #3 IGET, BB #5 IPUT
      false, false,  // BB #4 IPUT, BB #6 IPUT
      false, false,  // BB #5 IGET, BB #6 IGET
      false, false, true,  // BB #4 INVOKE, BB #5 IGET, BB #6 IPUT
      false, false,  // BB #3 IGET_OBJECT & IF_EQZ
      false, true,   // BB #5 NEW_ARRAY, BB #6 AGET
  };

  PrepareIFields(ifields);
  PrepareDiamond();
  PrepareMIRs(mirs);
  PerformNullCheckElimination();
  ASSERT_EQ(arraysize(expected_ignore_null_check), mir_count_);
  for (size_t i = 0u; i != arraysize(mirs); ++i) {
    EXPECT_EQ(expected_ignore_null_check[i],
              (mirs_[i].optimization_flags & MIR_IGNORE_NULL_CHECK) != 0) << i;
  }
}

TEST_F(NullCheckEliminationTest, Loop) {
  static const IFieldDef ifields[] = {
      { 0u, 1u, 0u, 0u },
      { 1u, 1u, 1u, 1u },
  };
  static const MIRDef mirs[] = {
      DEF_IGET_IPUT(3u, Instruction::IGET, 0u, 100u, 0u),
      DEF_IGET_IPUT(4u, Instruction::IGET, 1u, 101u, 0u),
      DEF_IGET_IPUT(5u, Instruction::IGET, 2u, 100u, 1u),  // Eliminated.
      DEF_IGET_IPUT(5u, Instruction::IGET, 3u, 101u, 1u),  // Eliminated.
      DEF_IGET_IPUT(3u, Instruction::IGET, 4u, 102u, 0u),
      DEF_IGET_IPUT(4u, Instruction::IGET, 5u, 102u, 1u),  // Not eliminated (MOVE_OBJECT_16).
      DEF_OTHER2(4u, Instruction::MOVE_OBJECT_16, 102u, 103u),
  };
  static const bool expected_ignore_null_check[] = {
      false, false, true, true,
      false, false, false,
  };

  PrepareIFields(ifields);
  PrepareLoop();
  PrepareMIRs(mirs);
  PerformNullCheckElimination();
  ASSERT_EQ(arraysize(expected_ignore_null_check), mir_count_);
  for (size_t i = 0u; i != arraysize(mirs); ++i) {
    EXPECT_EQ(expected_ignore_null_check[i],
              (mirs_[i].optimization_flags & MIR_IGNORE_NULL_CHECK) != 0) << i;
  }
}

TEST_F(NullCheckEliminationTest, Catch) {
  static const IFieldDef ifields[] = {
      { 0u, 1u, 0u, 0u },
      { 1u, 1u, 1u, 1u },
  };
  static const MIRDef mirs[] = {
      DEF_IGET_IPUT(3u, Instruction::IGET, 0u, 100u, 0u),  // Before the exception edge.
      DEF_IGET_IPUT(3u, Instruction::IGET, 1u, 101u, 0u),  // Before the exception edge.
      DEF_IGET_IPUT(4u, Instruction::IGET, 2u, 102u, 0u),  // After the exception edge.
      DEF_IGET_IPUT(4u, Instruction::IGET, 3u, 103u, 0u),  // After the exception edge.
      DEF_IGET_IPUT(5u, Instruction::IGET, 4u, 100u, 1u),  // In catch handler; eliminated.
      DEF_IGET_IPUT(5u, Instruction::IGET, 5u, 102u, 1u),  // In catch handler; not eliminated.
      DEF_IGET_IPUT(6u, Instruction::IGET, 6u, 100u, 0u),  // Null check eliminated.
      DEF_IGET_IPUT(6u, Instruction::IGET, 6u, 101u, 1u),  // Null check eliminated.
      DEF_IGET_IPUT(6u, Instruction::IGET, 6u, 102u, 0u),  // Null check eliminated.
      DEF_IGET_IPUT(6u, Instruction::IGET, 6u, 103u, 1u),  // Null check not eliminated.
  };
  static const bool expected_ignore_null_check[] = {
      false, false, false, false, true, false, true, true, true, false
  };

  PrepareIFields(ifields);
  PrepareCatch();
  PrepareMIRs(mirs);
  PerformNullCheckElimination();
  ASSERT_EQ(arraysize(expected_ignore_null_check), mir_count_);
  for (size_t i = 0u; i != arraysize(mirs); ++i) {
    EXPECT_EQ(expected_ignore_null_check[i],
              (mirs_[i].optimization_flags & MIR_IGNORE_NULL_CHECK) != 0) << i;
  }
}

// Undefine MIR_DEF for null check elimination.
#undef MIR_DEF

}  // namespace art
