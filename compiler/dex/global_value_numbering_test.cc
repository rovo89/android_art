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

#include "compiler_internals.h"
#include "dataflow_iterator.h"
#include "dataflow_iterator-inl.h"
#include "global_value_numbering.h"
#include "local_value_numbering.h"
#include "gtest/gtest.h"

namespace art {

class GlobalValueNumberingTest : public testing::Test {
 protected:
  struct IFieldDef {
    uint16_t field_idx;
    uintptr_t declaring_dex_file;
    uint16_t declaring_field_idx;
    bool is_volatile;
  };

  struct SFieldDef {
    uint16_t field_idx;
    uintptr_t declaring_dex_file;
    uint16_t declaring_field_idx;
    bool is_volatile;
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
#define DEF_PHI2(bb, reg, src1, src2) \
    { bb, static_cast<Instruction::Code>(kMirOpPhi), 0, 0u, 2u, { src1, src2 }, 1, { reg } }

  void DoPrepareIFields(const IFieldDef* defs, size_t count) {
    cu_.mir_graph->ifield_lowering_infos_.Reset();
    cu_.mir_graph->ifield_lowering_infos_.Resize(count);
    for (size_t i = 0u; i != count; ++i) {
      const IFieldDef* def = &defs[i];
      MirIFieldLoweringInfo field_info(def->field_idx);
      if (def->declaring_dex_file != 0u) {
        field_info.declaring_dex_file_ = reinterpret_cast<const DexFile*>(def->declaring_dex_file);
        field_info.declaring_field_idx_ = def->declaring_field_idx;
        field_info.flags_ = 0u |  // Without kFlagIsStatic.
            (def->is_volatile ? MirIFieldLoweringInfo::kFlagIsVolatile : 0u);
      }
      cu_.mir_graph->ifield_lowering_infos_.Insert(field_info);
    }
  }

  template <size_t count>
  void PrepareIFields(const IFieldDef (&defs)[count]) {
    DoPrepareIFields(defs, count);
  }

  void DoPrepareSFields(const SFieldDef* defs, size_t count) {
    cu_.mir_graph->sfield_lowering_infos_.Reset();
    cu_.mir_graph->sfield_lowering_infos_.Resize(count);
    for (size_t i = 0u; i != count; ++i) {
      const SFieldDef* def = &defs[i];
      MirSFieldLoweringInfo field_info(def->field_idx);
      // Mark even unresolved fields as initialized.
      field_info.flags_ = MirSFieldLoweringInfo::kFlagIsStatic |
          MirSFieldLoweringInfo::kFlagIsInitialized;
      if (def->declaring_dex_file != 0u) {
        field_info.declaring_dex_file_ = reinterpret_cast<const DexFile*>(def->declaring_dex_file);
        field_info.declaring_field_idx_ = def->declaring_field_idx;
        field_info.flags_ |= (def->is_volatile ? MirSFieldLoweringInfo::kFlagIsVolatile : 0u);
      }
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
        bb->data_flow_info->live_in_v = live_in_v_;
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
    ssa_reps_.resize(count);
    for (size_t i = 0u; i != count; ++i) {
      const MIRDef* def = &defs[i];
      MIR* mir = &mirs_[i];
      ASSERT_LT(def->bbid, cu_.mir_graph->block_list_.Size());
      BasicBlock* bb = cu_.mir_graph->block_list_.Get(def->bbid);
      bb->AppendMIR(mir);
      mir->dalvikInsn.opcode = def->opcode;
      mir->dalvikInsn.vB = static_cast<int32_t>(def->value);
      mir->dalvikInsn.vB_wide = def->value;
      if (def->opcode >= Instruction::IGET && def->opcode <= Instruction::IPUT_SHORT) {
        ASSERT_LT(def->field_info, cu_.mir_graph->ifield_lowering_infos_.Size());
        mir->meta.ifield_lowering_info = def->field_info;
      } else if (def->opcode >= Instruction::SGET && def->opcode <= Instruction::SPUT_SHORT) {
        ASSERT_LT(def->field_info, cu_.mir_graph->sfield_lowering_infos_.Size());
        mir->meta.sfield_lowering_info = def->field_info;
      } else if (def->opcode == static_cast<Instruction::Code>(kMirOpPhi)) {
        mir->meta.phi_incoming = static_cast<BasicBlockId*>(
            allocator_->Alloc(def->num_uses * sizeof(BasicBlockId), kArenaAllocDFInfo));
        for (size_t i = 0; i != def->num_uses; ++i) {
          mir->meta.phi_incoming[i] = bb->predecessors->Get(i);
        }
      }
      mir->ssa_rep = &ssa_reps_[i];
      mir->ssa_rep->num_uses = def->num_uses;
      mir->ssa_rep->uses = const_cast<int32_t*>(def->uses);  // Not modified by LVN.
      mir->ssa_rep->fp_use = nullptr;  // Not used by LVN.
      mir->ssa_rep->num_defs = def->num_defs;
      mir->ssa_rep->defs = const_cast<int32_t*>(def->defs);  // Not modified by LVN.
      mir->ssa_rep->fp_def = nullptr;  // Not used by LVN.
      mir->dalvikInsn.opcode = def->opcode;
      mir->offset = i;  // LVN uses offset only for debug output
      mir->optimization_flags = 0u;
    }
    mirs_[count - 1u].next = nullptr;
  }

  template <size_t count>
  void PrepareMIRs(const MIRDef (&defs)[count]) {
    DoPrepareMIRs(defs, count);
  }

  void PerformGVN() {
    DoPerformGVN<LoopRepeatingTopologicalSortIterator>();
  }

  void PerformPreOrderDfsGVN() {
    DoPerformGVN<RepeatingPreOrderDfsIterator>();
  }

  template <typename IteratorType>
  void DoPerformGVN() {
    cu_.mir_graph->SSATransformationStart();
    cu_.mir_graph->ComputeDFSOrders();
    cu_.mir_graph->ComputeDominators();
    cu_.mir_graph->ComputeTopologicalSortOrder();
    cu_.mir_graph->SSATransformationEnd();
    ASSERT_TRUE(gvn_ == nullptr);
    gvn_.reset(new (allocator_.get()) GlobalValueNumbering(&cu_, allocator_.get()));
    ASSERT_FALSE(gvn_->CanModify());
    value_names_.resize(mir_count_, 0xffffu);
    IteratorType iterator(cu_.mir_graph.get());
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
    ASSERT_FALSE(gvn_->CanModify());
    gvn_->AllowModifications();
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

  GlobalValueNumberingTest()
      : pool_(),
        cu_(&pool_),
        mir_count_(0u),
        mirs_(nullptr),
        ssa_reps_(),
        allocator_(),
        gvn_(),
        value_names_(),
        live_in_v_(new (&cu_.arena) ArenaBitVector(&cu_.arena, kMaxSsaRegs, false, kBitMapMisc)) {
    cu_.mir_graph.reset(new MIRGraph(&cu_, &cu_.arena));
    cu_.access_flags = kAccStatic;  // Don't let "this" interfere with this test.
    allocator_.reset(ScopedArenaAllocator::Create(&cu_.arena_stack));
    // Bind all possible sregs to live vregs for test purposes.
    live_in_v_->SetInitialBits(kMaxSsaRegs);
    cu_.mir_graph->ssa_base_vregs_ = new (&cu_.arena) GrowableArray<int>(&cu_.arena, kMaxSsaRegs);
    cu_.mir_graph->ssa_subscripts_ = new (&cu_.arena) GrowableArray<int>(&cu_.arena, kMaxSsaRegs);
    for (unsigned int i = 0; i < kMaxSsaRegs; i++) {
      cu_.mir_graph->ssa_base_vregs_->Insert(i);
      cu_.mir_graph->ssa_subscripts_->Insert(0);
    }
  }

  static constexpr size_t kMaxSsaRegs = 16384u;

  ArenaPool pool_;
  CompilationUnit cu_;
  size_t mir_count_;
  MIR* mirs_;
  std::vector<SSARepresentation> ssa_reps_;
  std::unique_ptr<ScopedArenaAllocator> allocator_;
  std::unique_ptr<GlobalValueNumbering> gvn_;
  std::vector<uint16_t> value_names_;
  ArenaBitVector* live_in_v_;
};

class GlobalValueNumberingTestDiamond : public GlobalValueNumberingTest {
 public:
  GlobalValueNumberingTestDiamond();

 private:
  static const BBDef kDiamondBbs[];
};

const GlobalValueNumberingTest::BBDef GlobalValueNumberingTestDiamond::kDiamondBbs[] = {
    DEF_BB(kNullBlock, DEF_SUCC0(), DEF_PRED0()),
    DEF_BB(kEntryBlock, DEF_SUCC1(3), DEF_PRED0()),
    DEF_BB(kExitBlock, DEF_SUCC0(), DEF_PRED1(6)),
    DEF_BB(kDalvikByteCode, DEF_SUCC2(4, 5), DEF_PRED1(1)),  // Block #3, top of the diamond.
    DEF_BB(kDalvikByteCode, DEF_SUCC1(6), DEF_PRED1(3)),     // Block #4, left side.
    DEF_BB(kDalvikByteCode, DEF_SUCC1(6), DEF_PRED1(3)),     // Block #5, right side.
    DEF_BB(kDalvikByteCode, DEF_SUCC1(2), DEF_PRED2(4, 5)),  // Block #6, bottom.
};

GlobalValueNumberingTestDiamond::GlobalValueNumberingTestDiamond()
    : GlobalValueNumberingTest() {
  PrepareBasicBlocks(kDiamondBbs);
}

class GlobalValueNumberingTestLoop : public GlobalValueNumberingTest {
 public:
  GlobalValueNumberingTestLoop();

 private:
  static const BBDef kLoopBbs[];
};

const GlobalValueNumberingTest::BBDef GlobalValueNumberingTestLoop::kLoopBbs[] = {
    DEF_BB(kNullBlock, DEF_SUCC0(), DEF_PRED0()),
    DEF_BB(kEntryBlock, DEF_SUCC1(3), DEF_PRED0()),
    DEF_BB(kExitBlock, DEF_SUCC0(), DEF_PRED1(5)),
    DEF_BB(kDalvikByteCode, DEF_SUCC1(4), DEF_PRED1(1)),
    DEF_BB(kDalvikByteCode, DEF_SUCC2(5, 4), DEF_PRED2(3, 4)),  // "taken" loops to self.
    DEF_BB(kDalvikByteCode, DEF_SUCC1(2), DEF_PRED1(4)),
};

GlobalValueNumberingTestLoop::GlobalValueNumberingTestLoop()
    : GlobalValueNumberingTest() {
  PrepareBasicBlocks(kLoopBbs);
}

class GlobalValueNumberingTestCatch : public GlobalValueNumberingTest {
 public:
  GlobalValueNumberingTestCatch();

 private:
  static const BBDef kCatchBbs[];
};

const GlobalValueNumberingTest::BBDef GlobalValueNumberingTestCatch::kCatchBbs[] = {
    DEF_BB(kNullBlock, DEF_SUCC0(), DEF_PRED0()),
    DEF_BB(kEntryBlock, DEF_SUCC1(3), DEF_PRED0()),
    DEF_BB(kExitBlock, DEF_SUCC0(), DEF_PRED1(6)),
    DEF_BB(kDalvikByteCode, DEF_SUCC1(4), DEF_PRED1(1)),     // The top.
    DEF_BB(kDalvikByteCode, DEF_SUCC1(6), DEF_PRED1(3)),     // The throwing insn.
    DEF_BB(kDalvikByteCode, DEF_SUCC1(6), DEF_PRED1(3)),     // Catch handler.
    DEF_BB(kDalvikByteCode, DEF_SUCC1(2), DEF_PRED2(4, 5)),  // The merged block.
};

GlobalValueNumberingTestCatch::GlobalValueNumberingTestCatch()
    : GlobalValueNumberingTest() {
  PrepareBasicBlocks(kCatchBbs);
  // Mark catch handler.
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
}

class GlobalValueNumberingTestTwoConsecutiveLoops : public GlobalValueNumberingTest {
 public:
  GlobalValueNumberingTestTwoConsecutiveLoops();

 private:
  static const BBDef kTwoConsecutiveLoopsBbs[];
};

const GlobalValueNumberingTest::BBDef
GlobalValueNumberingTestTwoConsecutiveLoops::kTwoConsecutiveLoopsBbs[] = {
    DEF_BB(kNullBlock, DEF_SUCC0(), DEF_PRED0()),
    DEF_BB(kEntryBlock, DEF_SUCC1(3), DEF_PRED0()),
    DEF_BB(kExitBlock, DEF_SUCC0(), DEF_PRED1(9)),
    DEF_BB(kDalvikByteCode, DEF_SUCC1(4), DEF_PRED1(1)),
    DEF_BB(kDalvikByteCode, DEF_SUCC2(5, 6), DEF_PRED2(3, 5)),  // "taken" skips over the loop.
    DEF_BB(kDalvikByteCode, DEF_SUCC1(4), DEF_PRED1(4)),
    DEF_BB(kDalvikByteCode, DEF_SUCC1(7), DEF_PRED1(4)),
    DEF_BB(kDalvikByteCode, DEF_SUCC2(8, 9), DEF_PRED2(6, 8)),  // "taken" skips over the loop.
    DEF_BB(kDalvikByteCode, DEF_SUCC1(7), DEF_PRED1(7)),
    DEF_BB(kDalvikByteCode, DEF_SUCC1(2), DEF_PRED1(7)),
};

GlobalValueNumberingTestTwoConsecutiveLoops::GlobalValueNumberingTestTwoConsecutiveLoops()
    : GlobalValueNumberingTest() {
  PrepareBasicBlocks(kTwoConsecutiveLoopsBbs);
}

class GlobalValueNumberingTestTwoNestedLoops : public GlobalValueNumberingTest {
 public:
  GlobalValueNumberingTestTwoNestedLoops();

 private:
  static const BBDef kTwoNestedLoopsBbs[];
};

const GlobalValueNumberingTest::BBDef
GlobalValueNumberingTestTwoNestedLoops::kTwoNestedLoopsBbs[] = {
    DEF_BB(kNullBlock, DEF_SUCC0(), DEF_PRED0()),
    DEF_BB(kEntryBlock, DEF_SUCC1(3), DEF_PRED0()),
    DEF_BB(kExitBlock, DEF_SUCC0(), DEF_PRED1(8)),
    DEF_BB(kDalvikByteCode, DEF_SUCC1(4), DEF_PRED1(1)),
    DEF_BB(kDalvikByteCode, DEF_SUCC2(5, 8), DEF_PRED2(3, 7)),  // "taken" skips over the loop.
    DEF_BB(kDalvikByteCode, DEF_SUCC2(6, 7), DEF_PRED2(4, 6)),  // "taken" skips over the loop.
    DEF_BB(kDalvikByteCode, DEF_SUCC1(5), DEF_PRED1(5)),
    DEF_BB(kDalvikByteCode, DEF_SUCC1(4), DEF_PRED1(5)),
    DEF_BB(kDalvikByteCode, DEF_SUCC1(2), DEF_PRED1(4)),
};

GlobalValueNumberingTestTwoNestedLoops::GlobalValueNumberingTestTwoNestedLoops()
    : GlobalValueNumberingTest() {
  PrepareBasicBlocks(kTwoNestedLoopsBbs);
}

TEST_F(GlobalValueNumberingTestDiamond, NonAliasingIFields) {
  static const IFieldDef ifields[] = {
      { 0u, 1u, 0u, false },  // Int.
      { 1u, 1u, 1u, false },  // Int.
      { 2u, 1u, 2u, false },  // Int.
      { 3u, 1u, 3u, false },  // Int.
      { 4u, 1u, 4u, false },  // Short.
      { 5u, 1u, 5u, false },  // Char.
      { 6u, 0u, 0u, false },  // Unresolved, Short.
      { 7u, 1u, 7u, false },  // Int.
      { 8u, 0u, 0u, false },  // Unresolved, Int.
      { 9u, 1u, 9u, false },  // Int.
      { 10u, 1u, 10u, false },  // Int.
      { 11u, 1u, 11u, false },  // Int.
  };
  static const MIRDef mirs[] = {
      // NOTE: MIRs here are ordered by unique tests. They will be put into appropriate blocks.
      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 100u),
      DEF_IGET(3, Instruction::IGET, 1u, 100u, 0u),
      DEF_IGET(6, Instruction::IGET, 2u, 100u, 0u),   // Same as at the top.

      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 200u),
      DEF_IGET(4, Instruction::IGET, 4u, 200u, 1u),
      DEF_IGET(6, Instruction::IGET, 5u, 200u, 1u),   // Same as at the left side.

      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 300u),
      DEF_IGET(3, Instruction::IGET, 7u, 300u, 2u),
      DEF_CONST(5, Instruction::CONST, 8u, 1000),
      DEF_IPUT(5, Instruction::IPUT, 8u, 300u, 2u),
      DEF_IGET(6, Instruction::IGET, 10u, 300u, 2u),  // Differs from the top and the CONST.

      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 400u),
      DEF_IGET(3, Instruction::IGET, 12u, 400u, 3u),
      DEF_CONST(3, Instruction::CONST, 13u, 2000),
      DEF_IPUT(4, Instruction::IPUT, 13u, 400u, 3u),
      DEF_IPUT(5, Instruction::IPUT, 13u, 400u, 3u),
      DEF_IGET(6, Instruction::IGET, 16u, 400u, 3u),  // Differs from the top, equals the CONST.

      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 500u),
      DEF_IGET(3, Instruction::IGET_SHORT, 18u, 500u, 4u),
      DEF_IGET(3, Instruction::IGET_CHAR, 19u, 500u, 5u),
      DEF_IPUT(4, Instruction::IPUT_SHORT, 20u, 500u, 6u),  // Clobbers field #4, not #5.
      DEF_IGET(6, Instruction::IGET_SHORT, 21u, 500u, 4u),  // Differs from the top.
      DEF_IGET(6, Instruction::IGET_CHAR, 22u, 500u, 5u),   // Same as the top.

      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 600u),
      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 601u),
      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 602u),
      DEF_IGET(3, Instruction::IGET, 26u, 600u, 7u),
      DEF_IGET(3, Instruction::IGET, 27u, 601u, 7u),
      DEF_IPUT(4, Instruction::IPUT, 28u, 602u, 8u),  // Doesn't clobber field #7 for other refs.
      DEF_IGET(6, Instruction::IGET, 29u, 600u, 7u),  // Same as the top.
      DEF_IGET(6, Instruction::IGET, 30u, 601u, 7u),  // Same as the top.

      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 700u),
      DEF_CONST(4, Instruction::CONST, 32u, 3000),
      DEF_IPUT(4, Instruction::IPUT, 32u, 700u, 9u),
      DEF_IPUT(4, Instruction::IPUT, 32u, 700u, 10u),
      DEF_CONST(5, Instruction::CONST, 35u, 3001),
      DEF_IPUT(5, Instruction::IPUT, 35u, 700u, 9u),
      DEF_IPUT(5, Instruction::IPUT, 35u, 700u, 10u),
      DEF_IGET(6, Instruction::IGET, 38u, 700u, 9u),
      DEF_IGET(6, Instruction::IGET, 39u, 700u, 10u),  // Same value as read from field #9.

      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 800u),
      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 801u),
      DEF_CONST(4, Instruction::CONST, 42u, 3000),
      DEF_IPUT(4, Instruction::IPUT, 42u, 800u, 11u),
      DEF_IPUT(4, Instruction::IPUT, 42u, 801u, 11u),
      DEF_CONST(5, Instruction::CONST, 45u, 3001),
      DEF_IPUT(5, Instruction::IPUT, 45u, 800u, 11u),
      DEF_IPUT(5, Instruction::IPUT, 45u, 801u, 11u),
      DEF_IGET(6, Instruction::IGET, 48u, 800u, 11u),
      DEF_IGET(6, Instruction::IGET, 49u, 801u, 11u),  // Same value as read from ref 46u.

      // Invoke doesn't interfere with non-aliasing refs. There's one test above where a reference
      // escapes in the left BB (we let a reference escape if we use it to store to an unresolved
      // field) and the INVOKE in the right BB shouldn't interfere with that either.
      DEF_INVOKE1(5, Instruction::INVOKE_STATIC, 48u),
  };

  PrepareIFields(ifields);
  PrepareMIRs(mirs);
  PerformGVN();
  ASSERT_EQ(arraysize(mirs), value_names_.size());
  EXPECT_EQ(value_names_[1], value_names_[2]);

  EXPECT_EQ(value_names_[4], value_names_[5]);

  EXPECT_NE(value_names_[7], value_names_[10]);
  EXPECT_NE(value_names_[8], value_names_[10]);

  EXPECT_NE(value_names_[12], value_names_[16]);
  EXPECT_EQ(value_names_[13], value_names_[16]);

  EXPECT_NE(value_names_[18], value_names_[21]);
  EXPECT_EQ(value_names_[19], value_names_[22]);

  EXPECT_EQ(value_names_[26], value_names_[29]);
  EXPECT_EQ(value_names_[27], value_names_[30]);

  EXPECT_EQ(value_names_[38], value_names_[39]);

  EXPECT_EQ(value_names_[48], value_names_[49]);
}

TEST_F(GlobalValueNumberingTestDiamond, AliasingIFieldsSingleObject) {
  static const IFieldDef ifields[] = {
      { 0u, 1u, 0u, false },  // Int.
      { 1u, 1u, 1u, false },  // Int.
      { 2u, 1u, 2u, false },  // Int.
      { 3u, 1u, 3u, false },  // Int.
      { 4u, 1u, 4u, false },  // Short.
      { 5u, 1u, 5u, false },  // Char.
      { 6u, 0u, 0u, false },  // Unresolved, Short.
      { 7u, 1u, 7u, false },  // Int.
      { 8u, 1u, 8u, false },  // Int.
  };
  static const MIRDef mirs[] = {
      // NOTE: MIRs here are ordered by unique tests. They will be put into appropriate blocks.
      DEF_IGET(3, Instruction::IGET, 0u, 100u, 0u),
      DEF_IGET(6, Instruction::IGET, 1u, 100u, 0u),   // Same as at the top.

      DEF_IGET(4, Instruction::IGET, 2u, 100u, 1u),
      DEF_IGET(6, Instruction::IGET, 3u, 100u, 1u),   // Same as at the left side.

      DEF_IGET(3, Instruction::IGET, 4u, 100u, 2u),
      DEF_CONST(5, Instruction::CONST, 5u, 1000),
      DEF_IPUT(5, Instruction::IPUT, 5u, 100u, 2u),
      DEF_IGET(6, Instruction::IGET, 7u, 100u, 2u),   // Differs from the top and the CONST.

      DEF_IGET(3, Instruction::IGET, 8u, 100u, 3u),
      DEF_CONST(3, Instruction::CONST, 9u, 2000),
      DEF_IPUT(4, Instruction::IPUT, 9u, 100u, 3u),
      DEF_IPUT(5, Instruction::IPUT, 9u, 100u, 3u),
      DEF_IGET(6, Instruction::IGET, 12u, 100u, 3u),  // Differs from the top, equals the CONST.

      DEF_IGET(3, Instruction::IGET_SHORT, 13u, 100u, 4u),
      DEF_IGET(3, Instruction::IGET_CHAR, 14u, 100u, 5u),
      DEF_IPUT(4, Instruction::IPUT_SHORT, 15u, 100u, 6u),  // Clobbers field #4, not #5.
      DEF_IGET(6, Instruction::IGET_SHORT, 16u, 100u, 4u),  // Differs from the top.
      DEF_IGET(6, Instruction::IGET_CHAR, 17u, 100u, 5u),   // Same as the top.

      DEF_CONST(4, Instruction::CONST, 18u, 3000),
      DEF_IPUT(4, Instruction::IPUT, 18u, 100u, 7u),
      DEF_IPUT(4, Instruction::IPUT, 18u, 100u, 8u),
      DEF_CONST(5, Instruction::CONST, 21u, 3001),
      DEF_IPUT(5, Instruction::IPUT, 21u, 100u, 7u),
      DEF_IPUT(5, Instruction::IPUT, 21u, 100u, 8u),
      DEF_IGET(6, Instruction::IGET, 24u, 100u, 7u),
      DEF_IGET(6, Instruction::IGET, 25u, 100u, 8u),  // Same value as read from field #7.
  };

  PrepareIFields(ifields);
  PrepareMIRs(mirs);
  PerformGVN();
  ASSERT_EQ(arraysize(mirs), value_names_.size());
  EXPECT_EQ(value_names_[0], value_names_[1]);

  EXPECT_EQ(value_names_[2], value_names_[3]);

  EXPECT_NE(value_names_[4], value_names_[7]);
  EXPECT_NE(value_names_[5], value_names_[7]);

  EXPECT_NE(value_names_[8], value_names_[12]);
  EXPECT_EQ(value_names_[9], value_names_[12]);

  EXPECT_NE(value_names_[13], value_names_[16]);
  EXPECT_EQ(value_names_[14], value_names_[17]);

  EXPECT_EQ(value_names_[24], value_names_[25]);
}

TEST_F(GlobalValueNumberingTestDiamond, AliasingIFieldsTwoObjects) {
  static const IFieldDef ifields[] = {
      { 0u, 1u, 0u, false },  // Int.
      { 1u, 1u, 1u, false },  // Int.
      { 2u, 1u, 2u, false },  // Int.
      { 3u, 1u, 3u, false },  // Int.
      { 4u, 1u, 4u, false },  // Short.
      { 5u, 1u, 5u, false },  // Char.
      { 6u, 0u, 0u, false },  // Unresolved, Short.
      { 7u, 1u, 7u, false },  // Int.
      { 8u, 1u, 8u, false },  // Int.
  };
  static const MIRDef mirs[] = {
      // NOTE: MIRs here are ordered by unique tests. They will be put into appropriate blocks.
      DEF_IGET(3, Instruction::IGET, 0u, 100u, 0u),
      DEF_IPUT(4, Instruction::IPUT, 1u, 101u, 0u),   // May alias with the IGET at the top.
      DEF_IGET(6, Instruction::IGET, 2u, 100u, 0u),   // Differs from the top.

      DEF_IGET(3, Instruction::IGET, 3u, 100u, 1u),
      DEF_IPUT(5, Instruction::IPUT, 3u, 101u, 1u),   // If aliasing, stores the same value.
      DEF_IGET(6, Instruction::IGET, 5u, 100u, 1u),   // Same as the top.

      DEF_IGET(3, Instruction::IGET, 6u, 100u, 2u),
      DEF_CONST(5, Instruction::CONST, 7u, 1000),
      DEF_IPUT(5, Instruction::IPUT, 7u, 101u, 2u),
      DEF_IGET(6, Instruction::IGET, 9u, 100u, 2u),   // Differs from the top and the CONST.

      DEF_IGET(3, Instruction::IGET, 10u, 100u, 3u),
      DEF_CONST(3, Instruction::CONST, 11u, 2000),
      DEF_IPUT(4, Instruction::IPUT, 11u, 101u, 3u),
      DEF_IPUT(5, Instruction::IPUT, 11u, 101u, 3u),
      DEF_IGET(6, Instruction::IGET, 14u, 100u, 3u),  // Differs from the top and the CONST.

      DEF_IGET(3, Instruction::IGET_SHORT, 15u, 100u, 4u),
      DEF_IGET(3, Instruction::IGET_CHAR, 16u, 100u, 5u),
      DEF_IPUT(4, Instruction::IPUT_SHORT, 17u, 101u, 6u),  // Clobbers field #4, not #5.
      DEF_IGET(6, Instruction::IGET_SHORT, 18u, 100u, 4u),  // Differs from the top.
      DEF_IGET(6, Instruction::IGET_CHAR, 19u, 100u, 5u),   // Same as the top.

      DEF_CONST(4, Instruction::CONST, 20u, 3000),
      DEF_IPUT(4, Instruction::IPUT, 20u, 100u, 7u),
      DEF_IPUT(4, Instruction::IPUT, 20u, 101u, 8u),
      DEF_CONST(5, Instruction::CONST, 23u, 3001),
      DEF_IPUT(5, Instruction::IPUT, 23u, 100u, 7u),
      DEF_IPUT(5, Instruction::IPUT, 23u, 101u, 8u),
      DEF_IGET(6, Instruction::IGET, 26u, 100u, 7u),
      DEF_IGET(6, Instruction::IGET, 27u, 101u, 8u),  // Same value as read from field #7.
  };

  PrepareIFields(ifields);
  PrepareMIRs(mirs);
  PerformGVN();
  ASSERT_EQ(arraysize(mirs), value_names_.size());
  EXPECT_NE(value_names_[0], value_names_[2]);

  EXPECT_EQ(value_names_[3], value_names_[5]);

  EXPECT_NE(value_names_[6], value_names_[9]);
  EXPECT_NE(value_names_[7], value_names_[9]);

  EXPECT_NE(value_names_[10], value_names_[14]);
  EXPECT_NE(value_names_[10], value_names_[14]);

  EXPECT_NE(value_names_[15], value_names_[18]);
  EXPECT_EQ(value_names_[16], value_names_[19]);

  EXPECT_EQ(value_names_[26], value_names_[27]);
}

TEST_F(GlobalValueNumberingTestDiamond, SFields) {
  static const SFieldDef sfields[] = {
      { 0u, 1u, 0u, false },  // Int.
      { 1u, 1u, 1u, false },  // Int.
      { 2u, 1u, 2u, false },  // Int.
      { 3u, 1u, 3u, false },  // Int.
      { 4u, 1u, 4u, false },  // Short.
      { 5u, 1u, 5u, false },  // Char.
      { 6u, 0u, 0u, false },  // Unresolved, Short.
      { 7u, 1u, 7u, false },  // Int.
      { 8u, 1u, 8u, false },  // Int.
  };
  static const MIRDef mirs[] = {
      // NOTE: MIRs here are ordered by unique tests. They will be put into appropriate blocks.
      DEF_SGET(3, Instruction::SGET, 0u, 0u),
      DEF_SGET(6, Instruction::SGET, 1u, 0u),         // Same as at the top.

      DEF_SGET(4, Instruction::SGET, 2u, 1u),
      DEF_SGET(6, Instruction::SGET, 3u, 1u),         // Same as at the left side.

      DEF_SGET(3, Instruction::SGET, 4u, 2u),
      DEF_CONST(5, Instruction::CONST, 5u, 100),
      DEF_SPUT(5, Instruction::SPUT, 5u, 2u),
      DEF_SGET(6, Instruction::SGET, 7u, 2u),         // Differs from the top and the CONST.

      DEF_SGET(3, Instruction::SGET, 8u, 3u),
      DEF_CONST(3, Instruction::CONST, 9u, 200),
      DEF_SPUT(4, Instruction::SPUT, 9u, 3u),
      DEF_SPUT(5, Instruction::SPUT, 9u, 3u),
      DEF_SGET(6, Instruction::SGET, 12u, 3u),        // Differs from the top, equals the CONST.

      DEF_SGET(3, Instruction::SGET_SHORT, 13u, 4u),
      DEF_SGET(3, Instruction::SGET_CHAR, 14u, 5u),
      DEF_SPUT(4, Instruction::SPUT_SHORT, 15u, 6u),  // Clobbers field #4, not #5.
      DEF_SGET(6, Instruction::SGET_SHORT, 16u, 4u),  // Differs from the top.
      DEF_SGET(6, Instruction::SGET_CHAR, 17u, 5u),   // Same as the top.

      DEF_CONST(4, Instruction::CONST, 18u, 300),
      DEF_SPUT(4, Instruction::SPUT, 18u, 7u),
      DEF_SPUT(4, Instruction::SPUT, 18u, 8u),
      DEF_CONST(5, Instruction::CONST, 21u, 301),
      DEF_SPUT(5, Instruction::SPUT, 21u, 7u),
      DEF_SPUT(5, Instruction::SPUT, 21u, 8u),
      DEF_SGET(6, Instruction::SGET, 24u, 7u),
      DEF_SGET(6, Instruction::SGET, 25u, 8u),        // Same value as read from field #7.
  };

  PrepareSFields(sfields);
  PrepareMIRs(mirs);
  PerformGVN();
  ASSERT_EQ(arraysize(mirs), value_names_.size());
  EXPECT_EQ(value_names_[0], value_names_[1]);

  EXPECT_EQ(value_names_[2], value_names_[3]);

  EXPECT_NE(value_names_[4], value_names_[7]);
  EXPECT_NE(value_names_[5], value_names_[7]);

  EXPECT_NE(value_names_[8], value_names_[12]);
  EXPECT_EQ(value_names_[9], value_names_[12]);

  EXPECT_NE(value_names_[13], value_names_[16]);
  EXPECT_EQ(value_names_[14], value_names_[17]);

  EXPECT_EQ(value_names_[24], value_names_[25]);
}

TEST_F(GlobalValueNumberingTestDiamond, NonAliasingArrays) {
  static const MIRDef mirs[] = {
      // NOTE: MIRs here are ordered by unique tests. They will be put into appropriate blocks.
      DEF_UNIQUE_REF(3, Instruction::NEW_ARRAY, 100u),
      DEF_AGET(3, Instruction::AGET, 1u, 100u, 101u),
      DEF_AGET(6, Instruction::AGET, 2u, 100u, 101u),   // Same as at the top.

      DEF_UNIQUE_REF(3, Instruction::NEW_ARRAY, 200u),
      DEF_IGET(4, Instruction::AGET, 4u, 200u, 201u),
      DEF_IGET(6, Instruction::AGET, 5u, 200u, 201u),   // Same as at the left side.

      DEF_UNIQUE_REF(3, Instruction::NEW_ARRAY, 300u),
      DEF_AGET(3, Instruction::AGET, 7u, 300u, 301u),
      DEF_CONST(5, Instruction::CONST, 8u, 1000),
      DEF_APUT(5, Instruction::APUT, 8u, 300u, 301u),
      DEF_AGET(6, Instruction::AGET, 10u, 300u, 301u),  // Differs from the top and the CONST.

      DEF_UNIQUE_REF(3, Instruction::NEW_ARRAY, 400u),
      DEF_AGET(3, Instruction::AGET, 12u, 400u, 401u),
      DEF_CONST(3, Instruction::CONST, 13u, 2000),
      DEF_APUT(4, Instruction::APUT, 13u, 400u, 401u),
      DEF_APUT(5, Instruction::APUT, 13u, 400u, 401u),
      DEF_AGET(6, Instruction::AGET, 16u, 400u, 401u),  // Differs from the top, equals the CONST.

      DEF_UNIQUE_REF(3, Instruction::NEW_ARRAY, 500u),
      DEF_AGET(3, Instruction::AGET, 18u, 500u, 501u),
      DEF_APUT(4, Instruction::APUT, 19u, 500u, 502u),  // Clobbers value at index 501u.
      DEF_AGET(6, Instruction::AGET, 20u, 500u, 501u),  // Differs from the top.

      DEF_UNIQUE_REF(3, Instruction::NEW_ARRAY, 600u),
      DEF_CONST(4, Instruction::CONST, 22u, 3000),
      DEF_APUT(4, Instruction::APUT, 22u, 600u, 601u),
      DEF_APUT(4, Instruction::APUT, 22u, 600u, 602u),
      DEF_CONST(5, Instruction::CONST, 25u, 3001),
      DEF_APUT(5, Instruction::APUT, 25u, 600u, 601u),
      DEF_APUT(5, Instruction::APUT, 25u, 600u, 602u),
      DEF_AGET(6, Instruction::AGET, 28u, 600u, 601u),
      DEF_AGET(6, Instruction::AGET, 29u, 600u, 602u),  // Same value as read from index 601u.

      DEF_UNIQUE_REF(3, Instruction::NEW_ARRAY, 700u),
      DEF_UNIQUE_REF(3, Instruction::NEW_ARRAY, 701u),
      DEF_AGET(3, Instruction::AGET, 32u, 700u, 702u),
      DEF_APUT(4, Instruction::APUT, 33u, 701u, 702u),  // Doesn't interfere with unrelated array.
      DEF_AGET(6, Instruction::AGET, 34u, 700u, 702u),  // Same value as at the top.
  };

  PrepareMIRs(mirs);
  PerformGVN();
  ASSERT_EQ(arraysize(mirs), value_names_.size());
  EXPECT_EQ(value_names_[1], value_names_[2]);

  EXPECT_EQ(value_names_[4], value_names_[5]);

  EXPECT_NE(value_names_[7], value_names_[10]);
  EXPECT_NE(value_names_[8], value_names_[10]);

  EXPECT_NE(value_names_[12], value_names_[16]);
  EXPECT_EQ(value_names_[13], value_names_[16]);

  EXPECT_NE(value_names_[18], value_names_[20]);

  EXPECT_NE(value_names_[28], value_names_[22]);
  EXPECT_NE(value_names_[28], value_names_[25]);
  EXPECT_EQ(value_names_[28], value_names_[29]);

  EXPECT_EQ(value_names_[32], value_names_[34]);
}

TEST_F(GlobalValueNumberingTestDiamond, AliasingArrays) {
  static const MIRDef mirs[] = {
      // NOTE: MIRs here are ordered by unique tests. They will be put into appropriate blocks.
      // NOTE: We're also testing that these tests really do not interfere with each other.

      DEF_AGET(3, Instruction::AGET_BOOLEAN, 0u, 100u, 101u),
      DEF_AGET(6, Instruction::AGET_BOOLEAN, 1u, 100u, 101u),  // Same as at the top.

      DEF_IGET(4, Instruction::AGET_OBJECT, 2u, 200u, 201u),
      DEF_IGET(6, Instruction::AGET_OBJECT, 3u, 200u, 201u),  // Same as at the left side.

      DEF_AGET(3, Instruction::AGET_WIDE, 4u, 300u, 301u),
      DEF_CONST(5, Instruction::CONST_WIDE, 5u, 1000),
      DEF_APUT(5, Instruction::APUT_WIDE, 5u, 300u, 301u),
      DEF_AGET(6, Instruction::AGET_WIDE, 7u, 300u, 301u),  // Differs from the top and the CONST.

      DEF_AGET(3, Instruction::AGET_SHORT, 8u, 400u, 401u),
      DEF_CONST(3, Instruction::CONST, 9u, 2000),
      DEF_APUT(4, Instruction::APUT_SHORT, 9u, 400u, 401u),
      DEF_APUT(5, Instruction::APUT_SHORT, 9u, 400u, 401u),
      DEF_AGET(6, Instruction::AGET_SHORT, 12u, 400u, 401u),  // Differs from the top, == CONST.

      DEF_AGET(3, Instruction::AGET_CHAR, 13u, 500u, 501u),
      DEF_APUT(4, Instruction::APUT_CHAR, 14u, 500u, 502u),  // Clobbers value at index 501u.
      DEF_AGET(6, Instruction::AGET_CHAR, 15u, 500u, 501u),  // Differs from the top.

      DEF_AGET(3, Instruction::AGET_BYTE, 16u, 600u, 602u),
      DEF_APUT(4, Instruction::APUT_BYTE, 17u, 601u, 602u),  // Clobbers values in array 600u.
      DEF_AGET(6, Instruction::AGET_BYTE, 18u, 600u, 602u),  // Differs from the top.

      DEF_CONST(4, Instruction::CONST, 19u, 3000),
      DEF_APUT(4, Instruction::APUT, 19u, 700u, 701u),
      DEF_APUT(4, Instruction::APUT, 19u, 700u, 702u),
      DEF_CONST(5, Instruction::CONST, 22u, 3001),
      DEF_APUT(5, Instruction::APUT, 22u, 700u, 701u),
      DEF_APUT(5, Instruction::APUT, 22u, 700u, 702u),
      DEF_AGET(6, Instruction::AGET, 25u, 700u, 701u),
      DEF_AGET(6, Instruction::AGET, 26u, 700u, 702u),  // Same value as read from index 601u.
  };

  PrepareMIRs(mirs);
  PerformGVN();
  ASSERT_EQ(arraysize(mirs), value_names_.size());
  EXPECT_EQ(value_names_[0], value_names_[1]);

  EXPECT_EQ(value_names_[2], value_names_[3]);

  EXPECT_NE(value_names_[4], value_names_[7]);
  EXPECT_NE(value_names_[5], value_names_[7]);

  EXPECT_NE(value_names_[8], value_names_[12]);
  EXPECT_EQ(value_names_[9], value_names_[12]);

  EXPECT_NE(value_names_[13], value_names_[15]);

  EXPECT_NE(value_names_[16], value_names_[18]);

  EXPECT_NE(value_names_[25], value_names_[19]);
  EXPECT_NE(value_names_[25], value_names_[22]);
  EXPECT_EQ(value_names_[25], value_names_[26]);
}

TEST_F(GlobalValueNumberingTestDiamond, Phi) {
  static const MIRDef mirs[] = {
      DEF_CONST(3, Instruction::CONST, 0u, 1000),
      DEF_CONST(4, Instruction::CONST, 1u, 2000),
      DEF_CONST(5, Instruction::CONST, 2u, 3000),
      DEF_MOVE(4, Instruction::MOVE, 3u, 0u),
      DEF_MOVE(4, Instruction::MOVE, 4u, 1u),
      DEF_MOVE(5, Instruction::MOVE, 5u, 0u),
      DEF_MOVE(5, Instruction::MOVE, 6u, 2u),
      DEF_PHI2(6, 7u, 3u, 5u),    // Same as CONST 0u (1000).
      DEF_PHI2(6, 8u, 3u, 0u),    // Same as CONST 0u (1000).
      DEF_PHI2(6, 9u, 0u, 5u),    // Same as CONST 0u (1000).
      DEF_PHI2(6, 10u, 4u, 5u),   // Merge 1u (2000) and 0u (1000).
      DEF_PHI2(6, 11u, 1u, 5u),   // Merge 1u (2000) and 0u (1000).
      DEF_PHI2(6, 12u, 4u, 0u),   // Merge 1u (2000) and 0u (1000).
      DEF_PHI2(6, 13u, 1u, 0u),   // Merge 1u (2000) and 0u (1000).
      DEF_PHI2(6, 14u, 3u, 6u),   // Merge 0u (1000) and 2u (3000).
      DEF_PHI2(6, 15u, 0u, 6u),   // Merge 0u (1000) and 2u (3000).
      DEF_PHI2(6, 16u, 3u, 2u),   // Merge 0u (1000) and 2u (3000).
      DEF_PHI2(6, 17u, 0u, 2u),   // Merge 0u (1000) and 2u (3000).
      DEF_PHI2(6, 18u, 4u, 6u),   // Merge 1u (2000) and 2u (3000).
      DEF_PHI2(6, 19u, 1u, 6u),   // Merge 1u (2000) and 2u (3000).
      DEF_PHI2(6, 20u, 4u, 2u),   // Merge 1u (2000) and 2u (3000).
      DEF_PHI2(6, 21u, 1u, 2u),   // Merge 1u (2000) and 2u (3000).
  };

  PrepareMIRs(mirs);
  PerformGVN();
  ASSERT_EQ(arraysize(mirs), value_names_.size());
  EXPECT_EQ(value_names_[0], value_names_[7]);
  EXPECT_EQ(value_names_[0], value_names_[8]);
  EXPECT_EQ(value_names_[0], value_names_[9]);
  EXPECT_NE(value_names_[10], value_names_[0]);
  EXPECT_NE(value_names_[10], value_names_[1]);
  EXPECT_NE(value_names_[10], value_names_[2]);
  EXPECT_EQ(value_names_[10], value_names_[11]);
  EXPECT_EQ(value_names_[10], value_names_[12]);
  EXPECT_EQ(value_names_[10], value_names_[13]);
  EXPECT_NE(value_names_[14], value_names_[0]);
  EXPECT_NE(value_names_[14], value_names_[1]);
  EXPECT_NE(value_names_[14], value_names_[2]);
  EXPECT_NE(value_names_[14], value_names_[10]);
  EXPECT_EQ(value_names_[14], value_names_[15]);
  EXPECT_EQ(value_names_[14], value_names_[16]);
  EXPECT_EQ(value_names_[14], value_names_[17]);
  EXPECT_NE(value_names_[18], value_names_[0]);
  EXPECT_NE(value_names_[18], value_names_[1]);
  EXPECT_NE(value_names_[18], value_names_[2]);
  EXPECT_NE(value_names_[18], value_names_[10]);
  EXPECT_NE(value_names_[18], value_names_[14]);
  EXPECT_EQ(value_names_[18], value_names_[19]);
  EXPECT_EQ(value_names_[18], value_names_[20]);
  EXPECT_EQ(value_names_[18], value_names_[21]);
}

TEST_F(GlobalValueNumberingTestLoop, NonAliasingIFields) {
  static const IFieldDef ifields[] = {
      { 0u, 1u, 0u, false },  // Int.
      { 1u, 1u, 1u, false },  // Int.
      { 2u, 1u, 2u, false },  // Int.
      { 3u, 1u, 3u, false },  // Int.
      { 4u, 1u, 4u, false },  // Int.
      { 5u, 1u, 5u, false },  // Short.
      { 6u, 1u, 6u, false },  // Char.
      { 7u, 0u, 0u, false },  // Unresolved, Short.
      { 8u, 1u, 8u, false },  // Int.
      { 9u, 0u, 0u, false },  // Unresolved, Int.
      { 10u, 1u, 10u, false },  // Int.
      { 11u, 1u, 11u, false },  // Int.
  };
  static const MIRDef mirs[] = {
      // NOTE: MIRs here are ordered by unique tests. They will be put into appropriate blocks.
      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 100u),
      DEF_IGET(3, Instruction::IGET, 1u, 100u, 0u),
      DEF_IGET(4, Instruction::IGET, 2u, 100u, 0u),   // Same as at the top.
      DEF_IGET(5, Instruction::IGET, 3u, 100u, 0u),   // Same as at the top.

      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 200u),
      DEF_IGET(3, Instruction::IGET, 5u, 200u, 1u),
      DEF_IGET(4, Instruction::IGET, 6u, 200u, 1u),   // Differs from top...
      DEF_IPUT(4, Instruction::IPUT, 7u, 200u, 1u),   // Because of this IPUT.
      DEF_IGET(5, Instruction::IGET, 8u, 200u, 1u),   // Differs from top and the loop IGET.

      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 300u),
      DEF_IGET(3, Instruction::IGET, 10u, 300u, 2u),
      DEF_IPUT(4, Instruction::IPUT, 11u, 300u, 2u),  // Because of this IPUT...
      DEF_IGET(4, Instruction::IGET, 12u, 300u, 2u),  // Differs from top.
      DEF_IGET(5, Instruction::IGET, 13u, 300u, 2u),  // Differs from top but same as the loop IGET.

      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 400u),
      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 401u),
      DEF_CONST(3, Instruction::CONST, 16u, 3000),
      DEF_IPUT(3, Instruction::IPUT, 16u, 400u, 3u),
      DEF_IPUT(3, Instruction::IPUT, 16u, 400u, 4u),
      DEF_IPUT(3, Instruction::IPUT, 16u, 401u, 3u),
      DEF_IGET(4, Instruction::IGET, 20u, 400u, 3u),  // Differs from 16u and 23u.
      DEF_IGET(4, Instruction::IGET, 21u, 400u, 4u),  // Same as 20u.
      DEF_IGET(4, Instruction::IGET, 22u, 401u, 3u),  // Same as 20u.
      DEF_CONST(4, Instruction::CONST, 23u, 4000),
      DEF_IPUT(4, Instruction::IPUT, 23u, 400u, 3u),
      DEF_IPUT(4, Instruction::IPUT, 23u, 400u, 4u),
      DEF_IPUT(4, Instruction::IPUT, 23u, 401u, 3u),
      DEF_IGET(5, Instruction::IGET, 27u, 400u, 3u),  // Differs from 16u and 20u...
      DEF_IGET(5, Instruction::IGET, 28u, 400u, 4u),  // and same as the CONST 23u
      DEF_IGET(5, Instruction::IGET, 29u, 400u, 4u),  // and same as the CONST 23u.

      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 500u),
      DEF_IGET(3, Instruction::IGET_SHORT, 31u, 500u, 5u),
      DEF_IGET(3, Instruction::IGET_CHAR, 32u, 500u, 6u),
      DEF_IPUT(4, Instruction::IPUT_SHORT, 33u, 500u, 7u),  // Clobbers field #5, not #6.
      DEF_IGET(5, Instruction::IGET_SHORT, 34u, 500u, 5u),  // Differs from the top.
      DEF_IGET(5, Instruction::IGET_CHAR, 35u, 500u, 6u),   // Same as the top.

      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 600u),
      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 601u),
      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 602u),
      DEF_IGET(3, Instruction::IGET, 39u, 600u, 8u),
      DEF_IGET(3, Instruction::IGET, 40u, 601u, 8u),
      DEF_IPUT(4, Instruction::IPUT, 41u, 602u, 9u),  // Doesn't clobber field #8 for other refs.
      DEF_IGET(5, Instruction::IGET, 42u, 600u, 8u),  // Same as the top.
      DEF_IGET(5, Instruction::IGET, 43u, 601u, 8u),  // Same as the top.

      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 700u),
      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 701u),
      DEF_CONST(3, Instruction::CONST, 46u, 3000),
      DEF_IPUT(3, Instruction::IPUT, 46u, 700u, 10u),
      DEF_IPUT(3, Instruction::IPUT, 46u, 700u, 11u),
      DEF_IPUT(3, Instruction::IPUT, 46u, 701u, 10u),
      DEF_IGET(4, Instruction::IGET, 50u, 700u, 10u),  // Differs from the CONSTs 46u and 53u.
      DEF_IGET(4, Instruction::IGET, 51u, 700u, 11u),  // Same as 50u.
      DEF_IGET(4, Instruction::IGET, 52u, 701u, 10u),  // Same as 50u.
      DEF_CONST(4, Instruction::CONST, 53u, 3001),
      DEF_IPUT(4, Instruction::IPUT, 53u, 700u, 10u),
      DEF_IPUT(4, Instruction::IPUT, 53u, 700u, 11u),
      DEF_IPUT(4, Instruction::IPUT, 53u, 701u, 10u),
      DEF_IGET(5, Instruction::IGET, 57u, 700u, 10u),  // Same as the CONST 53u.
      DEF_IGET(5, Instruction::IGET, 58u, 700u, 11u),  // Same as the CONST 53u.
      DEF_IGET(5, Instruction::IGET, 59u, 701u, 10u),  // Same as the CONST 53u.
  };

  PrepareIFields(ifields);
  PrepareMIRs(mirs);
  PerformGVN();
  ASSERT_EQ(arraysize(mirs), value_names_.size());
  EXPECT_EQ(value_names_[1], value_names_[2]);
  EXPECT_EQ(value_names_[1], value_names_[3]);

  EXPECT_NE(value_names_[5], value_names_[6]);
  EXPECT_NE(value_names_[5], value_names_[7]);
  EXPECT_NE(value_names_[6], value_names_[7]);

  EXPECT_NE(value_names_[10], value_names_[12]);
  EXPECT_EQ(value_names_[12], value_names_[13]);

  EXPECT_NE(value_names_[20], value_names_[16]);
  EXPECT_NE(value_names_[20], value_names_[23]);
  EXPECT_EQ(value_names_[20], value_names_[21]);
  EXPECT_EQ(value_names_[20], value_names_[22]);
  EXPECT_NE(value_names_[27], value_names_[16]);
  EXPECT_NE(value_names_[27], value_names_[20]);
  EXPECT_EQ(value_names_[27], value_names_[28]);
  EXPECT_EQ(value_names_[27], value_names_[29]);

  EXPECT_NE(value_names_[31], value_names_[34]);
  EXPECT_EQ(value_names_[32], value_names_[35]);

  EXPECT_EQ(value_names_[39], value_names_[42]);
  EXPECT_EQ(value_names_[40], value_names_[43]);

  EXPECT_NE(value_names_[50], value_names_[46]);
  EXPECT_NE(value_names_[50], value_names_[53]);
  EXPECT_EQ(value_names_[50], value_names_[51]);
  EXPECT_EQ(value_names_[50], value_names_[52]);
  EXPECT_EQ(value_names_[57], value_names_[53]);
  EXPECT_EQ(value_names_[58], value_names_[53]);
  EXPECT_EQ(value_names_[59], value_names_[53]);
}

TEST_F(GlobalValueNumberingTestLoop, AliasingIFieldsSingleObject) {
  static const IFieldDef ifields[] = {
      { 0u, 1u, 0u, false },  // Int.
      { 1u, 1u, 1u, false },  // Int.
      { 2u, 1u, 2u, false },  // Int.
      { 3u, 1u, 3u, false },  // Int.
      { 4u, 1u, 4u, false },  // Int.
      { 5u, 1u, 5u, false },  // Short.
      { 6u, 1u, 6u, false },  // Char.
      { 7u, 0u, 0u, false },  // Unresolved, Short.
  };
  static const MIRDef mirs[] = {
      // NOTE: MIRs here are ordered by unique tests. They will be put into appropriate blocks.
      DEF_IGET(3, Instruction::IGET, 0u, 100u, 0u),
      DEF_IGET(4, Instruction::IGET, 1u, 100u, 0u),   // Same as at the top.
      DEF_IGET(5, Instruction::IGET, 2u, 100u, 0u),   // Same as at the top.

      DEF_IGET(3, Instruction::IGET, 3u, 100u, 1u),
      DEF_IGET(4, Instruction::IGET, 4u, 100u, 1u),   // Differs from top...
      DEF_IPUT(4, Instruction::IPUT, 5u, 100u, 1u),   // Because of this IPUT.
      DEF_IGET(5, Instruction::IGET, 6u, 100u, 1u),   // Differs from top and the loop IGET.

      DEF_IGET(3, Instruction::IGET, 7u, 100u, 2u),
      DEF_IPUT(4, Instruction::IPUT, 8u, 100u, 2u),   // Because of this IPUT...
      DEF_IGET(4, Instruction::IGET, 9u, 100u, 2u),   // Differs from top.
      DEF_IGET(5, Instruction::IGET, 10u, 100u, 2u),  // Differs from top but same as the loop IGET.

      DEF_CONST(3, Instruction::CONST, 11u, 3000),
      DEF_IPUT(3, Instruction::IPUT, 11u, 100u, 3u),
      DEF_IPUT(3, Instruction::IPUT, 11u, 100u, 4u),
      DEF_IGET(4, Instruction::IGET, 14u, 100u, 3u),  // Differs from 11u and 16u.
      DEF_IGET(4, Instruction::IGET, 15u, 100u, 4u),  // Same as 14u.
      DEF_CONST(4, Instruction::CONST, 16u, 4000),
      DEF_IPUT(4, Instruction::IPUT, 16u, 100u, 3u),
      DEF_IPUT(4, Instruction::IPUT, 16u, 100u, 4u),
      DEF_IGET(5, Instruction::IGET, 19u, 100u, 3u),  // Differs from 11u and 14u...
      DEF_IGET(5, Instruction::IGET, 20u, 100u, 4u),  // and same as the CONST 16u.

      DEF_IGET(3, Instruction::IGET_SHORT, 21u, 100u, 5u),
      DEF_IGET(3, Instruction::IGET_CHAR, 22u, 100u, 6u),
      DEF_IPUT(4, Instruction::IPUT_SHORT, 23u, 100u, 7u),  // Clobbers field #5, not #6.
      DEF_IGET(5, Instruction::IGET_SHORT, 24u, 100u, 5u),  // Differs from the top.
      DEF_IGET(5, Instruction::IGET_CHAR, 25u, 100u, 6u),   // Same as the top.
  };

  PrepareIFields(ifields);
  PrepareMIRs(mirs);
  PerformGVN();
  ASSERT_EQ(arraysize(mirs), value_names_.size());
  EXPECT_EQ(value_names_[0], value_names_[1]);
  EXPECT_EQ(value_names_[0], value_names_[2]);

  EXPECT_NE(value_names_[3], value_names_[4]);
  EXPECT_NE(value_names_[3], value_names_[6]);
  EXPECT_NE(value_names_[4], value_names_[6]);

  EXPECT_NE(value_names_[7], value_names_[9]);
  EXPECT_EQ(value_names_[9], value_names_[10]);

  EXPECT_NE(value_names_[14], value_names_[11]);
  EXPECT_NE(value_names_[14], value_names_[16]);
  EXPECT_EQ(value_names_[14], value_names_[15]);
  EXPECT_NE(value_names_[19], value_names_[11]);
  EXPECT_NE(value_names_[19], value_names_[14]);
  EXPECT_EQ(value_names_[19], value_names_[16]);
  EXPECT_EQ(value_names_[19], value_names_[20]);

  EXPECT_NE(value_names_[21], value_names_[24]);
  EXPECT_EQ(value_names_[22], value_names_[25]);
}

TEST_F(GlobalValueNumberingTestLoop, AliasingIFieldsTwoObjects) {
  static const IFieldDef ifields[] = {
      { 0u, 1u, 0u, false },  // Int.
      { 1u, 1u, 1u, false },  // Int.
      { 2u, 1u, 2u, false },  // Int.
      { 3u, 1u, 3u, false },  // Short.
      { 4u, 1u, 4u, false },  // Char.
      { 5u, 0u, 0u, false },  // Unresolved, Short.
      { 6u, 1u, 6u, false },  // Int.
      { 7u, 1u, 7u, false },  // Int.
  };
  static const MIRDef mirs[] = {
      // NOTE: MIRs here are ordered by unique tests. They will be put into appropriate blocks.
      DEF_IGET(3, Instruction::IGET, 0u, 100u, 0u),
      DEF_IPUT(4, Instruction::IPUT, 1u, 101u, 0u),   // May alias with the IGET at the top.
      DEF_IGET(5, Instruction::IGET, 2u, 100u, 0u),   // Differs from the top.

      DEF_IGET(3, Instruction::IGET, 3u, 100u, 1u),
      DEF_IPUT(4, Instruction::IPUT, 3u, 101u, 1u),   // If aliasing, stores the same value.
      DEF_IGET(5, Instruction::IGET, 5u, 100u, 1u),   // Same as the top.

      DEF_IGET(3, Instruction::IGET, 6u, 100u, 2u),
      DEF_CONST(4, Instruction::CONST, 7u, 1000),
      DEF_IPUT(4, Instruction::IPUT, 7u, 101u, 2u),
      DEF_IGET(5, Instruction::IGET, 9u, 100u, 2u),   // Differs from the top and the CONST.

      DEF_IGET(3, Instruction::IGET_SHORT, 10u, 100u, 3u),
      DEF_IGET(3, Instruction::IGET_CHAR, 11u, 100u, 4u),
      DEF_IPUT(4, Instruction::IPUT_SHORT, 12u, 101u, 5u),  // Clobbers field #3, not #4.
      DEF_IGET(5, Instruction::IGET_SHORT, 13u, 100u, 3u),  // Differs from the top.
      DEF_IGET(5, Instruction::IGET_CHAR, 14u, 100u, 4u),   // Same as the top.

      DEF_CONST(3, Instruction::CONST, 15u, 3000),
      DEF_IPUT(3, Instruction::IPUT, 15u, 100u, 6u),
      DEF_IPUT(3, Instruction::IPUT, 15u, 100u, 7u),
      DEF_IPUT(3, Instruction::IPUT, 15u, 101u, 6u),
      DEF_IGET(4, Instruction::IGET, 19u, 100u, 6u),  // Differs from CONSTs 15u and 22u.
      DEF_IGET(4, Instruction::IGET, 20u, 100u, 7u),  // Same value as 19u.
      DEF_IGET(4, Instruction::IGET, 21u, 101u, 6u),  // Same value as read from field #7.
      DEF_CONST(4, Instruction::CONST, 22u, 3001),
      DEF_IPUT(4, Instruction::IPUT, 22u, 100u, 6u),
      DEF_IPUT(4, Instruction::IPUT, 22u, 100u, 7u),
      DEF_IPUT(4, Instruction::IPUT, 22u, 101u, 6u),
      DEF_IGET(5, Instruction::IGET, 26u, 100u, 6u),  // Same as CONST 22u.
      DEF_IGET(5, Instruction::IGET, 27u, 100u, 7u),  // Same as CONST 22u.
      DEF_IGET(5, Instruction::IGET, 28u, 101u, 6u),  // Same as CONST 22u.
  };

  PrepareIFields(ifields);
  PrepareMIRs(mirs);
  PerformGVN();
  ASSERT_EQ(arraysize(mirs), value_names_.size());
  EXPECT_NE(value_names_[0], value_names_[2]);

  EXPECT_EQ(value_names_[3], value_names_[5]);

  EXPECT_NE(value_names_[6], value_names_[9]);
  EXPECT_NE(value_names_[7], value_names_[9]);

  EXPECT_NE(value_names_[10], value_names_[13]);
  EXPECT_EQ(value_names_[11], value_names_[14]);

  EXPECT_NE(value_names_[19], value_names_[15]);
  EXPECT_NE(value_names_[19], value_names_[22]);
  EXPECT_EQ(value_names_[22], value_names_[26]);
  EXPECT_EQ(value_names_[22], value_names_[27]);
  EXPECT_EQ(value_names_[22], value_names_[28]);
}

TEST_F(GlobalValueNumberingTestLoop, IFieldToBaseDependency) {
  static const IFieldDef ifields[] = {
      { 0u, 1u, 0u, false },  // Int.
  };
  static const MIRDef mirs[] = {
      // For the IGET that loads sreg 3u using base 2u, the following IPUT creates a dependency
      // from the field value to the base. However, this dependency does not result in an
      // infinite loop since the merge of the field value for base 0u gets assigned a value name
      // based only on the base 0u, not on the actual value, and breaks the dependency cycle.
      DEF_IGET(3, Instruction::IGET, 0u, 100u, 0u),
      DEF_IGET(3, Instruction::IGET, 1u, 0u, 0u),
      DEF_IGET(4, Instruction::IGET, 2u, 0u, 0u),
      DEF_IGET(4, Instruction::IGET, 3u, 2u, 0u),
      DEF_IPUT(4, Instruction::IPUT, 3u, 0u, 0u),
      DEF_IGET(5, Instruction::IGET, 5u, 0u, 0u),
  };

  PrepareIFields(ifields);
  PrepareMIRs(mirs);
  PerformGVN();
  ASSERT_EQ(arraysize(mirs), value_names_.size());
  EXPECT_NE(value_names_[1], value_names_[2]);
  EXPECT_EQ(value_names_[3], value_names_[5]);
}

TEST_F(GlobalValueNumberingTestLoop, SFields) {
  static const SFieldDef sfields[] = {
      { 0u, 1u, 0u, false },  // Int.
      { 1u, 1u, 1u, false },  // Int.
      { 2u, 1u, 2u, false },  // Int.
  };
  static const MIRDef mirs[] = {
      // NOTE: MIRs here are ordered by unique tests. They will be put into appropriate blocks.
      DEF_SGET(3, Instruction::SGET, 0u, 0u),
      DEF_SGET(4, Instruction::SGET, 1u, 0u),         // Same as at the top.
      DEF_SGET(5, Instruction::SGET, 2u, 0u),         // Same as at the top.

      DEF_SGET(3, Instruction::SGET, 3u, 1u),
      DEF_SGET(4, Instruction::SGET, 4u, 1u),         // Differs from top...
      DEF_SPUT(4, Instruction::SPUT, 5u, 1u),         // Because of this SPUT.
      DEF_SGET(5, Instruction::SGET, 6u, 1u),         // Differs from top and the loop SGET.

      DEF_SGET(3, Instruction::SGET, 7u, 2u),
      DEF_SPUT(4, Instruction::SPUT, 8u, 2u),         // Because of this SPUT...
      DEF_SGET(4, Instruction::SGET, 9u, 2u),         // Differs from top.
      DEF_SGET(5, Instruction::SGET, 10u, 2u),        // Differs from top but same as the loop SGET.
  };

  PrepareSFields(sfields);
  PrepareMIRs(mirs);
  PerformGVN();
  ASSERT_EQ(arraysize(mirs), value_names_.size());
  EXPECT_EQ(value_names_[0], value_names_[1]);
  EXPECT_EQ(value_names_[0], value_names_[2]);

  EXPECT_NE(value_names_[3], value_names_[4]);
  EXPECT_NE(value_names_[3], value_names_[6]);
  EXPECT_NE(value_names_[4], value_names_[5]);

  EXPECT_NE(value_names_[7], value_names_[9]);
  EXPECT_EQ(value_names_[9], value_names_[10]);
}

TEST_F(GlobalValueNumberingTestLoop, NonAliasingArrays) {
  static const MIRDef mirs[] = {
      // NOTE: MIRs here are ordered by unique tests. They will be put into appropriate blocks.
      DEF_UNIQUE_REF(3, Instruction::NEW_ARRAY, 100u),
      DEF_AGET(3, Instruction::AGET, 1u, 100u, 101u),
      DEF_AGET(4, Instruction::AGET, 2u, 100u, 101u),   // Same as at the top.
      DEF_AGET(5, Instruction::AGET, 3u, 100u, 101u),   // Same as at the top.

      DEF_UNIQUE_REF(3, Instruction::NEW_ARRAY, 200u),
      DEF_AGET(3, Instruction::AGET, 5u, 200u, 201u),
      DEF_AGET(4, Instruction::AGET, 6u, 200u, 201u),  // Differs from top...
      DEF_APUT(4, Instruction::APUT, 7u, 200u, 201u),  // Because of this IPUT.
      DEF_AGET(5, Instruction::AGET, 8u, 200u, 201u),  // Differs from top and the loop AGET.

      DEF_UNIQUE_REF(3, Instruction::NEW_ARRAY, 300u),
      DEF_AGET(3, Instruction::AGET, 10u, 300u, 301u),
      DEF_APUT(4, Instruction::APUT, 11u, 300u, 301u),  // Because of this IPUT...
      DEF_AGET(4, Instruction::AGET, 12u, 300u, 301u),  // Differs from top.
      DEF_AGET(5, Instruction::AGET, 13u, 300u, 301u),  // Differs from top but == the loop AGET.

      DEF_UNIQUE_REF(3, Instruction::NEW_ARRAY, 400u),
      DEF_CONST(3, Instruction::CONST, 15u, 3000),
      DEF_APUT(3, Instruction::APUT, 15u, 400u, 401u),
      DEF_APUT(3, Instruction::APUT, 15u, 400u, 402u),
      DEF_AGET(4, Instruction::AGET, 18u, 400u, 401u),  // Differs from 15u and 20u.
      DEF_AGET(4, Instruction::AGET, 19u, 400u, 402u),  // Same as 18u.
      DEF_CONST(4, Instruction::CONST, 20u, 4000),
      DEF_APUT(4, Instruction::APUT, 20u, 400u, 401u),
      DEF_APUT(4, Instruction::APUT, 20u, 400u, 402u),
      DEF_AGET(5, Instruction::AGET, 23u, 400u, 401u),  // Differs from 15u and 18u...
      DEF_AGET(5, Instruction::AGET, 24u, 400u, 402u),  // and same as the CONST 20u.

      DEF_UNIQUE_REF(3, Instruction::NEW_ARRAY, 500u),
      DEF_AGET(3, Instruction::AGET, 26u, 500u, 501u),
      DEF_APUT(4, Instruction::APUT, 27u, 500u, 502u),  // Clobbers element at index 501u.
      DEF_AGET(5, Instruction::AGET, 28u, 500u, 501u),  // Differs from the top.
  };

  PrepareMIRs(mirs);
  PerformGVN();
  ASSERT_EQ(arraysize(mirs), value_names_.size());
  EXPECT_EQ(value_names_[1], value_names_[2]);
  EXPECT_EQ(value_names_[1], value_names_[3]);

  EXPECT_NE(value_names_[5], value_names_[6]);
  EXPECT_NE(value_names_[5], value_names_[8]);
  EXPECT_NE(value_names_[6], value_names_[8]);

  EXPECT_NE(value_names_[10], value_names_[12]);
  EXPECT_EQ(value_names_[12], value_names_[13]);

  EXPECT_NE(value_names_[18], value_names_[15]);
  EXPECT_NE(value_names_[18], value_names_[20]);
  EXPECT_EQ(value_names_[18], value_names_[19]);
  EXPECT_NE(value_names_[23], value_names_[15]);
  EXPECT_NE(value_names_[23], value_names_[18]);
  EXPECT_EQ(value_names_[23], value_names_[20]);
  EXPECT_EQ(value_names_[23], value_names_[24]);

  EXPECT_NE(value_names_[26], value_names_[28]);
}

TEST_F(GlobalValueNumberingTestLoop, AliasingArrays) {
  static const MIRDef mirs[] = {
      // NOTE: MIRs here are ordered by unique tests. They will be put into appropriate blocks.
      DEF_AGET(3, Instruction::AGET_WIDE, 0u, 100u, 101u),
      DEF_AGET(4, Instruction::AGET_WIDE, 1u, 100u, 101u),   // Same as at the top.
      DEF_AGET(5, Instruction::AGET_WIDE, 2u, 100u, 101u),   // Same as at the top.

      DEF_AGET(3, Instruction::AGET_BYTE, 3u, 200u, 201u),
      DEF_AGET(4, Instruction::AGET_BYTE, 4u, 200u, 201u),  // Differs from top...
      DEF_APUT(4, Instruction::APUT_BYTE, 5u, 200u, 201u),  // Because of this IPUT.
      DEF_AGET(5, Instruction::AGET_BYTE, 6u, 200u, 201u),  // Differs from top and the loop AGET.

      DEF_AGET(3, Instruction::AGET, 7u, 300u, 301u),
      DEF_APUT(4, Instruction::APUT, 8u, 300u, 301u),   // Because of this IPUT...
      DEF_AGET(4, Instruction::AGET, 9u, 300u, 301u),   // Differs from top.
      DEF_AGET(5, Instruction::AGET, 10u, 300u, 301u),  // Differs from top but == the loop AGET.

      DEF_CONST(3, Instruction::CONST, 11u, 3000),
      DEF_APUT(3, Instruction::APUT_CHAR, 11u, 400u, 401u),
      DEF_APUT(3, Instruction::APUT_CHAR, 11u, 400u, 402u),
      DEF_AGET(4, Instruction::AGET_CHAR, 14u, 400u, 401u),  // Differs from 11u and 16u.
      DEF_AGET(4, Instruction::AGET_CHAR, 15u, 400u, 402u),  // Same as 14u.
      DEF_CONST(4, Instruction::CONST, 16u, 4000),
      DEF_APUT(4, Instruction::APUT_CHAR, 16u, 400u, 401u),
      DEF_APUT(4, Instruction::APUT_CHAR, 16u, 400u, 402u),
      DEF_AGET(5, Instruction::AGET_CHAR, 19u, 400u, 401u),  // Differs from 11u and 14u...
      DEF_AGET(5, Instruction::AGET_CHAR, 20u, 400u, 402u),  // and same as the CONST 16u.

      DEF_AGET(3, Instruction::AGET_SHORT, 21u, 500u, 501u),
      DEF_APUT(4, Instruction::APUT_SHORT, 22u, 500u, 502u),  // Clobbers element at index 501u.
      DEF_AGET(5, Instruction::AGET_SHORT, 23u, 500u, 501u),  // Differs from the top.

      DEF_AGET(3, Instruction::AGET_OBJECT, 24u, 600u, 601u),
      DEF_APUT(4, Instruction::APUT_OBJECT, 25u, 601u, 602u),  // Clobbers 600u/601u.
      DEF_AGET(5, Instruction::AGET_OBJECT, 26u, 600u, 601u),  // Differs from the top.

      DEF_AGET(3, Instruction::AGET_BOOLEAN, 27u, 700u, 701u),
      DEF_APUT(4, Instruction::APUT_BOOLEAN, 27u, 701u, 702u),  // Storing the same value.
      DEF_AGET(5, Instruction::AGET_BOOLEAN, 29u, 700u, 701u),  // Differs from the top.
  };

  PrepareMIRs(mirs);
  PerformGVN();
  ASSERT_EQ(arraysize(mirs), value_names_.size());
  EXPECT_EQ(value_names_[0], value_names_[1]);
  EXPECT_EQ(value_names_[0], value_names_[2]);

  EXPECT_NE(value_names_[3], value_names_[4]);
  EXPECT_NE(value_names_[3], value_names_[6]);
  EXPECT_NE(value_names_[4], value_names_[6]);

  EXPECT_NE(value_names_[7], value_names_[9]);
  EXPECT_EQ(value_names_[9], value_names_[10]);

  EXPECT_NE(value_names_[14], value_names_[11]);
  EXPECT_NE(value_names_[14], value_names_[16]);
  EXPECT_EQ(value_names_[14], value_names_[15]);
  EXPECT_NE(value_names_[19], value_names_[11]);
  EXPECT_NE(value_names_[19], value_names_[14]);
  EXPECT_EQ(value_names_[19], value_names_[16]);
  EXPECT_EQ(value_names_[19], value_names_[20]);

  EXPECT_NE(value_names_[21], value_names_[23]);

  EXPECT_NE(value_names_[24], value_names_[26]);

  EXPECT_EQ(value_names_[27], value_names_[29]);
}

TEST_F(GlobalValueNumberingTestLoop, Phi) {
  static const MIRDef mirs[] = {
      DEF_CONST(3, Instruction::CONST, 0u, 1000),
      DEF_PHI2(4, 1u, 0u, 6u),                     // Merge CONST 0u (1000) with the same.
      DEF_PHI2(4, 2u, 0u, 7u),                     // Merge CONST 0u (1000) with the Phi itself.
      DEF_PHI2(4, 3u, 0u, 8u),                     // Merge CONST 0u (1000) and CONST 4u (2000).
      DEF_PHI2(4, 4u, 0u, 9u),                     // Merge CONST 0u (1000) and Phi 3u.
      DEF_CONST(4, Instruction::CONST, 5u, 2000),
      DEF_MOVE(4, Instruction::MOVE, 6u, 0u),
      DEF_MOVE(4, Instruction::MOVE, 7u, 2u),
      DEF_MOVE(4, Instruction::MOVE, 8u, 5u),
      DEF_MOVE(4, Instruction::MOVE, 9u, 3u),
  };

  PrepareMIRs(mirs);
  PerformGVN();
  ASSERT_EQ(arraysize(mirs), value_names_.size());
  EXPECT_EQ(value_names_[1], value_names_[0]);
  EXPECT_EQ(value_names_[2], value_names_[0]);

  EXPECT_NE(value_names_[3], value_names_[0]);
  EXPECT_NE(value_names_[3], value_names_[5]);
  EXPECT_NE(value_names_[4], value_names_[0]);
  EXPECT_NE(value_names_[4], value_names_[5]);
  EXPECT_NE(value_names_[4], value_names_[3]);
}

TEST_F(GlobalValueNumberingTestCatch, IFields) {
  static const IFieldDef ifields[] = {
      { 0u, 1u, 0u, false },
      { 1u, 1u, 1u, false },
  };
  static const MIRDef mirs[] = {
      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 200u),
      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 201u),
      DEF_IGET(3, Instruction::IGET, 2u, 100u, 0u),
      DEF_IGET(3, Instruction::IGET, 3u, 200u, 0u),
      DEF_IGET(3, Instruction::IGET, 4u, 201u, 0u),
      DEF_INVOKE1(4, Instruction::INVOKE_STATIC, 201u),     // Clobbering catch, 201u escapes.
      DEF_IGET(4, Instruction::IGET, 6u, 100u, 0u),         // Differs from IGET 2u.
      DEF_IPUT(4, Instruction::IPUT, 6u, 100u, 1u),
      DEF_IPUT(4, Instruction::IPUT, 6u, 101u, 0u),
      DEF_IPUT(4, Instruction::IPUT, 6u, 200u, 0u),
      DEF_IGET(5, Instruction::IGET, 10u, 100u, 0u),        // Differs from IGETs 2u and 6u.
      DEF_IGET(5, Instruction::IGET, 11u, 200u, 0u),        // Same as the top.
      DEF_IGET(5, Instruction::IGET, 12u, 201u, 0u),        // Differs from the top, 201u escaped.
      DEF_IPUT(5, Instruction::IPUT, 10u, 100u, 1u),
      DEF_IPUT(5, Instruction::IPUT, 10u, 101u, 0u),
      DEF_IPUT(5, Instruction::IPUT, 10u, 200u, 0u),
      DEF_IGET(6, Instruction::IGET, 16u, 100u, 0u),        // Differs from IGETs 2u, 6u and 10u.
      DEF_IGET(6, Instruction::IGET, 17u, 100u, 1u),        // Same as IGET 16u.
      DEF_IGET(6, Instruction::IGET, 18u, 101u, 0u),        // Same as IGET 16u.
      DEF_IGET(6, Instruction::IGET, 19u, 200u, 0u),        // Same as IGET 16u.
  };

  PrepareIFields(ifields);
  PrepareMIRs(mirs);
  PerformGVN();
  ASSERT_EQ(arraysize(mirs), value_names_.size());
  EXPECT_NE(value_names_[2], value_names_[6]);
  EXPECT_NE(value_names_[2], value_names_[10]);
  EXPECT_NE(value_names_[6], value_names_[10]);
  EXPECT_EQ(value_names_[3], value_names_[11]);
  EXPECT_NE(value_names_[4], value_names_[12]);

  EXPECT_NE(value_names_[2], value_names_[16]);
  EXPECT_NE(value_names_[6], value_names_[16]);
  EXPECT_NE(value_names_[10], value_names_[16]);
  EXPECT_EQ(value_names_[16], value_names_[17]);
  EXPECT_EQ(value_names_[16], value_names_[18]);
  EXPECT_EQ(value_names_[16], value_names_[19]);
}

TEST_F(GlobalValueNumberingTestCatch, SFields) {
  static const SFieldDef sfields[] = {
      { 0u, 1u, 0u, false },
      { 1u, 1u, 1u, false },
  };
  static const MIRDef mirs[] = {
      DEF_SGET(3, Instruction::SGET, 0u, 0u),
      DEF_INVOKE1(4, Instruction::INVOKE_STATIC, 100u),     // Clobbering catch.
      DEF_SGET(4, Instruction::SGET, 2u, 0u),               // Differs from SGET 0u.
      DEF_SPUT(4, Instruction::SPUT, 2u, 1u),
      DEF_SGET(5, Instruction::SGET, 4u, 0u),               // Differs from SGETs 0u and 2u.
      DEF_SPUT(5, Instruction::SPUT, 4u, 1u),
      DEF_SGET(6, Instruction::SGET, 6u, 0u),               // Differs from SGETs 0u, 2u and 4u.
      DEF_SGET(6, Instruction::SGET, 7u, 1u),               // Same as field #1.
  };

  PrepareSFields(sfields);
  PrepareMIRs(mirs);
  PerformGVN();
  ASSERT_EQ(arraysize(mirs), value_names_.size());
  EXPECT_NE(value_names_[0], value_names_[2]);
  EXPECT_NE(value_names_[0], value_names_[4]);
  EXPECT_NE(value_names_[2], value_names_[4]);
  EXPECT_NE(value_names_[0], value_names_[6]);
  EXPECT_NE(value_names_[2], value_names_[6]);
  EXPECT_NE(value_names_[4], value_names_[6]);
  EXPECT_EQ(value_names_[6], value_names_[7]);
}

TEST_F(GlobalValueNumberingTestCatch, Arrays) {
  static const MIRDef mirs[] = {
      DEF_UNIQUE_REF(3, Instruction::NEW_ARRAY, 200u),
      DEF_UNIQUE_REF(3, Instruction::NEW_ARRAY, 201u),
      DEF_AGET(3, Instruction::AGET, 2u, 100u, 101u),
      DEF_AGET(3, Instruction::AGET, 3u, 200u, 202u),
      DEF_AGET(3, Instruction::AGET, 4u, 200u, 203u),
      DEF_AGET(3, Instruction::AGET, 5u, 201u, 202u),
      DEF_AGET(3, Instruction::AGET, 6u, 201u, 203u),
      DEF_INVOKE1(4, Instruction::INVOKE_STATIC, 201u),     // Clobbering catch, 201u escapes.
      DEF_AGET(4, Instruction::AGET, 8u, 100u, 101u),       // Differs from AGET 2u.
      DEF_APUT(4, Instruction::APUT, 8u, 100u, 102u),
      DEF_APUT(4, Instruction::APUT, 8u, 200u, 202u),
      DEF_APUT(4, Instruction::APUT, 8u, 200u, 203u),
      DEF_APUT(4, Instruction::APUT, 8u, 201u, 202u),
      DEF_APUT(4, Instruction::APUT, 8u, 201u, 203u),
      DEF_AGET(5, Instruction::AGET, 14u, 100u, 101u),      // Differs from AGETs 2u and 8u.
      DEF_AGET(5, Instruction::AGET, 15u, 200u, 202u),      // Same as AGET 3u.
      DEF_AGET(5, Instruction::AGET, 16u, 200u, 203u),      // Same as AGET 4u.
      DEF_AGET(5, Instruction::AGET, 17u, 201u, 202u),      // Differs from AGET 5u.
      DEF_AGET(5, Instruction::AGET, 18u, 201u, 203u),      // Differs from AGET 6u.
      DEF_APUT(5, Instruction::APUT, 14u, 100u, 102u),
      DEF_APUT(5, Instruction::APUT, 14u, 200u, 202u),
      DEF_APUT(5, Instruction::APUT, 14u, 200u, 203u),
      DEF_APUT(5, Instruction::APUT, 14u, 201u, 202u),
      DEF_APUT(5, Instruction::APUT, 14u, 201u, 203u),
      DEF_AGET(6, Instruction::AGET, 24u, 100u, 101u),      // Differs from AGETs 2u, 8u and 14u.
      DEF_AGET(6, Instruction::AGET, 25u, 100u, 101u),      // Same as AGET 24u.
      DEF_AGET(6, Instruction::AGET, 26u, 200u, 202u),      // Same as AGET 24u.
      DEF_AGET(6, Instruction::AGET, 27u, 200u, 203u),      // Same as AGET 24u.
      DEF_AGET(6, Instruction::AGET, 28u, 201u, 202u),      // Same as AGET 24u.
      DEF_AGET(6, Instruction::AGET, 29u, 201u, 203u),      // Same as AGET 24u.
  };

  PrepareMIRs(mirs);
  PerformGVN();
  ASSERT_EQ(arraysize(mirs), value_names_.size());
  EXPECT_NE(value_names_[2], value_names_[8]);
  EXPECT_NE(value_names_[2], value_names_[14]);
  EXPECT_NE(value_names_[8], value_names_[14]);
  EXPECT_EQ(value_names_[3], value_names_[15]);
  EXPECT_EQ(value_names_[4], value_names_[16]);
  EXPECT_NE(value_names_[5], value_names_[17]);
  EXPECT_NE(value_names_[6], value_names_[18]);
  EXPECT_NE(value_names_[2], value_names_[24]);
  EXPECT_NE(value_names_[8], value_names_[24]);
  EXPECT_NE(value_names_[14], value_names_[24]);
  EXPECT_EQ(value_names_[24], value_names_[25]);
  EXPECT_EQ(value_names_[24], value_names_[26]);
  EXPECT_EQ(value_names_[24], value_names_[27]);
  EXPECT_EQ(value_names_[24], value_names_[28]);
  EXPECT_EQ(value_names_[24], value_names_[29]);
}

TEST_F(GlobalValueNumberingTestCatch, Phi) {
  static const MIRDef mirs[] = {
      DEF_CONST(3, Instruction::CONST, 0u, 1000),
      DEF_CONST(3, Instruction::CONST, 1u, 2000),
      DEF_MOVE(3, Instruction::MOVE, 2u, 1u),
      DEF_INVOKE1(4, Instruction::INVOKE_STATIC, 100u),     // Clobbering catch.
      DEF_CONST(5, Instruction::CONST, 4u, 1000),
      DEF_CONST(5, Instruction::CONST, 5u, 3000),
      DEF_MOVE(5, Instruction::MOVE, 6u, 5u),
      DEF_PHI2(6, 7u, 0u, 4u),
      DEF_PHI2(6, 8u, 0u, 5u),
      DEF_PHI2(6, 9u, 0u, 6u),
      DEF_PHI2(6, 10u, 1u, 4u),
      DEF_PHI2(6, 11u, 1u, 5u),
      DEF_PHI2(6, 12u, 1u, 6u),
      DEF_PHI2(6, 13u, 2u, 4u),
      DEF_PHI2(6, 14u, 2u, 5u),
      DEF_PHI2(6, 15u, 2u, 6u),
  };
  PrepareMIRs(mirs);
  PerformGVN();
  ASSERT_EQ(arraysize(mirs), value_names_.size());
  ASSERT_EQ(value_names_[4], value_names_[0]);  // Both CONSTs are 1000.
  EXPECT_EQ(value_names_[7], value_names_[0]);  // Merging CONST 0u and CONST 4u, both 1000.
  EXPECT_NE(value_names_[8], value_names_[0]);
  EXPECT_NE(value_names_[8], value_names_[5]);
  EXPECT_EQ(value_names_[9], value_names_[8]);
  EXPECT_NE(value_names_[10], value_names_[1]);
  EXPECT_NE(value_names_[10], value_names_[4]);
  EXPECT_NE(value_names_[10], value_names_[8]);
  EXPECT_NE(value_names_[11], value_names_[1]);
  EXPECT_NE(value_names_[11], value_names_[5]);
  EXPECT_NE(value_names_[11], value_names_[8]);
  EXPECT_NE(value_names_[11], value_names_[10]);
  EXPECT_EQ(value_names_[12], value_names_[11]);
  EXPECT_EQ(value_names_[13], value_names_[10]);
  EXPECT_EQ(value_names_[14], value_names_[11]);
  EXPECT_EQ(value_names_[15], value_names_[11]);
}

TEST_F(GlobalValueNumberingTest, NullCheckIFields) {
  static const IFieldDef ifields[] = {
      { 0u, 1u, 0u, false },  // Object.
      { 1u, 1u, 1u, false },  // Object.
  };
  static const BBDef bbs[] = {
      DEF_BB(kNullBlock, DEF_SUCC0(), DEF_PRED0()),
      DEF_BB(kEntryBlock, DEF_SUCC1(3), DEF_PRED0()),
      DEF_BB(kExitBlock, DEF_SUCC0(), DEF_PRED1(5)),
      DEF_BB(kDalvikByteCode, DEF_SUCC2(4, 5), DEF_PRED1(1)),  // 4 is fall-through, 5 is taken.
      DEF_BB(kDalvikByteCode, DEF_SUCC1(5), DEF_PRED1(3)),
      DEF_BB(kDalvikByteCode, DEF_SUCC1(2), DEF_PRED2(3, 4)),
  };
  static const MIRDef mirs[] = {
      DEF_IGET(3, Instruction::IGET_OBJECT, 0u, 100u, 0u),
      DEF_IGET(3, Instruction::IGET_OBJECT, 1u, 100u, 1u),
      DEF_IGET(3, Instruction::IGET_OBJECT, 2u, 101u, 0u),
      DEF_IFZ(3, Instruction::IF_NEZ, 0u),            // Null-check for field #0 for taken.
      DEF_UNIQUE_REF(4, Instruction::NEW_ARRAY, 4u),
      DEF_IPUT(4, Instruction::IPUT_OBJECT, 4u, 100u, 0u),
      DEF_IPUT(4, Instruction::IPUT_OBJECT, 4u, 100u, 1u),
      DEF_IPUT(4, Instruction::IPUT_OBJECT, 4u, 101u, 0u),
      DEF_IGET(5, Instruction::IGET_OBJECT, 8u, 100u, 0u),   // 100u/#0, IF_NEZ/NEW_ARRAY.
      DEF_IGET(5, Instruction::IGET_OBJECT, 9u, 100u, 1u),   // 100u/#1, -/NEW_ARRAY.
      DEF_IGET(5, Instruction::IGET_OBJECT, 10u, 101u, 0u),  // 101u/#0, -/NEW_ARRAY.
      DEF_CONST(5, Instruction::CONST, 11u, 0),
      DEF_AGET(5, Instruction::AGET, 12u, 8u, 11u),   // Null-check eliminated.
      DEF_AGET(5, Instruction::AGET, 13u, 9u, 11u),   // Null-check kept.
      DEF_AGET(5, Instruction::AGET, 14u, 10u, 11u),  // Null-check kept.
  };
  static const bool expected_ignore_null_check[] = {
      false, true, false, false,                      // BB #3; unimportant.
      false, true, true, true,                        // BB #4; unimportant.
      true, true, true, false, true, false, false,    // BB #5; only the last three are important.
  };

  PrepareIFields(ifields);
  PrepareBasicBlocks(bbs);
  PrepareMIRs(mirs);
  PerformGVN();
  ASSERT_EQ(arraysize(mirs), value_names_.size());
  PerformGVNCodeModifications();
  ASSERT_EQ(arraysize(expected_ignore_null_check), mir_count_);
  for (size_t i = 0u; i != arraysize(mirs); ++i) {
    EXPECT_EQ(expected_ignore_null_check[i],
              (mirs_[i].optimization_flags & MIR_IGNORE_NULL_CHECK) != 0) << i;
  }
}

TEST_F(GlobalValueNumberingTest, NullCheckSFields) {
  static const SFieldDef sfields[] = {
      { 0u, 1u, 0u, false },  // Object.
      { 1u, 1u, 1u, false },  // Object.
  };
  static const BBDef bbs[] = {
      DEF_BB(kNullBlock, DEF_SUCC0(), DEF_PRED0()),
      DEF_BB(kEntryBlock, DEF_SUCC1(3), DEF_PRED0()),
      DEF_BB(kExitBlock, DEF_SUCC0(), DEF_PRED1(5)),
      DEF_BB(kDalvikByteCode, DEF_SUCC2(4, 5), DEF_PRED1(1)),  // 4 is fall-through, 5 is taken.
      DEF_BB(kDalvikByteCode, DEF_SUCC1(5), DEF_PRED1(3)),
      DEF_BB(kDalvikByteCode, DEF_SUCC1(2), DEF_PRED2(3, 4)),
  };
  static const MIRDef mirs[] = {
      DEF_SGET(3, Instruction::SGET_OBJECT, 0u, 0u),
      DEF_SGET(3, Instruction::SGET_OBJECT, 1u, 1u),
      DEF_IFZ(3, Instruction::IF_NEZ, 0u),            // Null-check for field #0 for taken.
      DEF_UNIQUE_REF(4, Instruction::NEW_ARRAY, 3u),
      DEF_SPUT(4, Instruction::SPUT_OBJECT, 3u, 0u),
      DEF_SPUT(4, Instruction::SPUT_OBJECT, 3u, 1u),
      DEF_SGET(5, Instruction::SGET_OBJECT, 6u, 0u),  // Field #0 is null-checked, IF_NEZ/NEW_ARRAY.
      DEF_SGET(5, Instruction::SGET_OBJECT, 7u, 1u),  // Field #1 is not null-checked, -/NEW_ARRAY.
      DEF_CONST(5, Instruction::CONST, 8u, 0),
      DEF_AGET(5, Instruction::AGET, 9u, 6u, 8u),     // Null-check eliminated.
      DEF_AGET(5, Instruction::AGET, 10u, 7u, 8u),    // Null-check kept.
  };
  static const bool expected_ignore_null_check[] = {
      false, false, false, false, false, false, false, false, false, true, false
  };

  PrepareSFields(sfields);
  PrepareBasicBlocks(bbs);
  PrepareMIRs(mirs);
  PerformGVN();
  ASSERT_EQ(arraysize(mirs), value_names_.size());
  PerformGVNCodeModifications();
  ASSERT_EQ(arraysize(expected_ignore_null_check), mir_count_);
  for (size_t i = 0u; i != arraysize(mirs); ++i) {
    EXPECT_EQ(expected_ignore_null_check[i],
              (mirs_[i].optimization_flags & MIR_IGNORE_NULL_CHECK) != 0) << i;
  }
}

TEST_F(GlobalValueNumberingTest, NullCheckArrays) {
  static const BBDef bbs[] = {
      DEF_BB(kNullBlock, DEF_SUCC0(), DEF_PRED0()),
      DEF_BB(kEntryBlock, DEF_SUCC1(3), DEF_PRED0()),
      DEF_BB(kExitBlock, DEF_SUCC0(), DEF_PRED1(5)),
      DEF_BB(kDalvikByteCode, DEF_SUCC2(4, 5), DEF_PRED1(1)),  // 4 is fall-through, 5 is taken.
      DEF_BB(kDalvikByteCode, DEF_SUCC1(5), DEF_PRED1(3)),
      DEF_BB(kDalvikByteCode, DEF_SUCC1(2), DEF_PRED2(3, 4)),
  };
  static const MIRDef mirs[] = {
      DEF_AGET(3, Instruction::AGET_OBJECT, 0u, 100u, 102u),
      DEF_AGET(3, Instruction::AGET_OBJECT, 1u, 100u, 103u),
      DEF_AGET(3, Instruction::AGET_OBJECT, 2u, 101u, 102u),
      DEF_IFZ(3, Instruction::IF_NEZ, 0u),            // Null-check for field #0 for taken.
      DEF_UNIQUE_REF(4, Instruction::NEW_ARRAY, 4u),
      DEF_APUT(4, Instruction::APUT_OBJECT, 4u, 100u, 102u),
      DEF_APUT(4, Instruction::APUT_OBJECT, 4u, 100u, 103u),
      DEF_APUT(4, Instruction::APUT_OBJECT, 4u, 101u, 102u),
      DEF_AGET(5, Instruction::AGET_OBJECT, 8u, 100u, 102u),   // Null-checked, IF_NEZ/NEW_ARRAY.
      DEF_AGET(5, Instruction::AGET_OBJECT, 9u, 100u, 103u),   // Not null-checked, -/NEW_ARRAY.
      DEF_AGET(5, Instruction::AGET_OBJECT, 10u, 101u, 102u),  // Not null-checked, -/NEW_ARRAY.
      DEF_CONST(5, Instruction::CONST, 11u, 0),
      DEF_AGET(5, Instruction::AGET, 12u, 8u, 11u),    // Null-check eliminated.
      DEF_AGET(5, Instruction::AGET, 13u, 9u, 11u),    // Null-check kept.
      DEF_AGET(5, Instruction::AGET, 14u, 10u, 11u),   // Null-check kept.
  };
  static const bool expected_ignore_null_check[] = {
      false, true, false, false,                      // BB #3; unimportant.
      false, true, true, true,                        // BB #4; unimportant.
      true, true, true, false, true, false, false,    // BB #5; only the last three are important.
  };

  PrepareBasicBlocks(bbs);
  PrepareMIRs(mirs);
  PerformGVN();
  ASSERT_EQ(arraysize(mirs), value_names_.size());
  PerformGVNCodeModifications();
  ASSERT_EQ(arraysize(expected_ignore_null_check), mir_count_);
  for (size_t i = 0u; i != arraysize(mirs); ++i) {
    EXPECT_EQ(expected_ignore_null_check[i],
              (mirs_[i].optimization_flags & MIR_IGNORE_NULL_CHECK) != 0) << i;
  }
}

TEST_F(GlobalValueNumberingTestDiamond, RangeCheckArrays) {
  // NOTE: We don't merge range checks when we merge value names for Phis or memory locations.
  static const MIRDef mirs[] = {
      DEF_AGET(4, Instruction::AGET, 0u, 100u, 101u),
      DEF_AGET(5, Instruction::AGET, 1u, 100u, 101u),
      DEF_APUT(6, Instruction::APUT, 2u, 100u, 101u),

      DEF_AGET(4, Instruction::AGET, 3u, 200u, 201u),
      DEF_AGET(5, Instruction::AGET, 4u, 200u, 202u),
      DEF_APUT(6, Instruction::APUT, 5u, 200u, 201u),

      DEF_AGET(4, Instruction::AGET, 6u, 300u, 302u),
      DEF_AGET(5, Instruction::AGET, 7u, 301u, 302u),
      DEF_APUT(6, Instruction::APUT, 8u, 300u, 302u),
  };
  static const bool expected_ignore_null_check[] = {
      false, false, true,
      false, false, true,
      false, false, false,
  };
  static const bool expected_ignore_range_check[] = {
      false, false, true,
      false, false, false,
      false, false, false,
  };

  PrepareMIRs(mirs);
  PerformGVN();
  ASSERT_EQ(arraysize(mirs), value_names_.size());
  PerformGVNCodeModifications();
  ASSERT_EQ(arraysize(expected_ignore_null_check), mir_count_);
  ASSERT_EQ(arraysize(expected_ignore_range_check), mir_count_);
  for (size_t i = 0u; i != arraysize(mirs); ++i) {
    EXPECT_EQ(expected_ignore_null_check[i],
              (mirs_[i].optimization_flags & MIR_IGNORE_NULL_CHECK) != 0) << i;
    EXPECT_EQ(expected_ignore_range_check[i],
              (mirs_[i].optimization_flags & MIR_IGNORE_RANGE_CHECK) != 0) << i;
  }
}

TEST_F(GlobalValueNumberingTestDiamond, MergeSameValueInDifferentMemoryLocations) {
  static const IFieldDef ifields[] = {
      { 0u, 1u, 0u, false },  // Int.
      { 1u, 1u, 1u, false },  // Int.
  };
  static const SFieldDef sfields[] = {
      { 0u, 1u, 0u, false },  // Int.
      { 1u, 1u, 1u, false },  // Int.
  };
  static const MIRDef mirs[] = {
      DEF_UNIQUE_REF(3, Instruction::NEW_INSTANCE, 100u),
      DEF_UNIQUE_REF(3, Instruction::NEW_ARRAY, 200u),
      DEF_CONST(4, Instruction::CONST, 2u, 1000),
      DEF_IPUT(4, Instruction::IPUT, 2u, 100u, 0u),
      DEF_IPUT(4, Instruction::IPUT, 2u, 100u, 1u),
      DEF_IPUT(4, Instruction::IPUT, 2u, 101u, 0u),
      DEF_APUT(4, Instruction::APUT, 2u, 200u, 202u),
      DEF_APUT(4, Instruction::APUT, 2u, 200u, 203u),
      DEF_APUT(4, Instruction::APUT, 2u, 201u, 202u),
      DEF_APUT(4, Instruction::APUT, 2u, 201u, 203u),
      DEF_SPUT(4, Instruction::SPUT, 2u, 0u),
      DEF_SPUT(4, Instruction::SPUT, 2u, 1u),
      DEF_CONST(5, Instruction::CONST, 12u, 2000),
      DEF_IPUT(5, Instruction::IPUT, 12u, 100u, 0u),
      DEF_IPUT(5, Instruction::IPUT, 12u, 100u, 1u),
      DEF_IPUT(5, Instruction::IPUT, 12u, 101u, 0u),
      DEF_APUT(5, Instruction::APUT, 12u, 200u, 202u),
      DEF_APUT(5, Instruction::APUT, 12u, 200u, 203u),
      DEF_APUT(5, Instruction::APUT, 12u, 201u, 202u),
      DEF_APUT(5, Instruction::APUT, 12u, 201u, 203u),
      DEF_SPUT(5, Instruction::SPUT, 12u, 0u),
      DEF_SPUT(5, Instruction::SPUT, 12u, 1u),
      DEF_PHI2(6, 22u, 2u, 12u),
      DEF_IGET(6, Instruction::IGET, 23u, 100u, 0u),
      DEF_IGET(6, Instruction::IGET, 24u, 100u, 1u),
      DEF_IGET(6, Instruction::IGET, 25u, 101u, 0u),
      DEF_AGET(6, Instruction::AGET, 26u, 200u, 202u),
      DEF_AGET(6, Instruction::AGET, 27u, 200u, 203u),
      DEF_AGET(6, Instruction::AGET, 28u, 201u, 202u),
      DEF_AGET(6, Instruction::AGET, 29u, 201u, 203u),
      DEF_SGET(6, Instruction::SGET, 30u, 0u),
      DEF_SGET(6, Instruction::SGET, 31u, 1u),
  };
  PrepareIFields(ifields);
  PrepareSFields(sfields);
  PrepareMIRs(mirs);
  PerformGVN();
  ASSERT_EQ(arraysize(mirs), value_names_.size());
  EXPECT_NE(value_names_[2], value_names_[12]);
  EXPECT_NE(value_names_[2], value_names_[22]);
  EXPECT_NE(value_names_[12], value_names_[22]);
  for (size_t i = 23; i != arraysize(mirs); ++i) {
    EXPECT_EQ(value_names_[22], value_names_[i]) << i;
  }
}

TEST_F(GlobalValueNumberingTest, InfiniteLocationLoop) {
  // This is a pattern that lead to an infinite loop during the GVN development. This has been
  // fixed by rewriting the merging of AliasingValues to merge only locations read from or
  // written to in each incoming LVN rather than merging all locations read from or written to
  // in any incoming LVN. It also showed up only when the GVN used the DFS ordering instead of
  // the "topological" ordering but, since the "topological" ordering is not really topological
  // when there are cycles and an optimizing Java compiler (or a tool like proguard) could
  // theoretically create any sort of flow graph, this could have shown up in real code.
  //
  // While we were merging all the locations:
  // The first time the Phi evaluates to the same value name as CONST 0u.  After the second
  // evaluation, when the BB #9 has been processed, the Phi receives its own value name.
  // However, the index from the first evaluation keeps disappearing and reappearing in the
  // LVN's aliasing_array_value_map_'s load_value_map for BBs #9, #4, #5, #7 because of the
  // DFS ordering of LVN evaluation.
  static const IFieldDef ifields[] = {
      { 0u, 1u, 0u, false },  // Object.
  };
  static const BBDef bbs[] = {
      DEF_BB(kNullBlock, DEF_SUCC0(), DEF_PRED0()),
      DEF_BB(kEntryBlock, DEF_SUCC1(3), DEF_PRED0()),
      DEF_BB(kExitBlock, DEF_SUCC0(), DEF_PRED1(4)),
      DEF_BB(kDalvikByteCode, DEF_SUCC1(4), DEF_PRED1(1)),
      DEF_BB(kDalvikByteCode, DEF_SUCC2(5, 2), DEF_PRED2(3, 9)),
      DEF_BB(kDalvikByteCode, DEF_SUCC2(6, 7), DEF_PRED1(4)),
      DEF_BB(kDalvikByteCode, DEF_SUCC1(9), DEF_PRED1(5)),
      DEF_BB(kDalvikByteCode, DEF_SUCC2(8, 9), DEF_PRED1(5)),
      DEF_BB(kDalvikByteCode, DEF_SUCC1(9), DEF_PRED1(7)),
      DEF_BB(kDalvikByteCode, DEF_SUCC1(4), DEF_PRED3(6, 7, 8)),
  };
  static const MIRDef mirs[] = {
      DEF_CONST(3, Instruction::CONST, 0u, 0),
      DEF_PHI2(4, 1u, 0u, 10u),
      DEF_INVOKE1(6, Instruction::INVOKE_STATIC, 100u),
      DEF_IGET(6, Instruction::IGET_OBJECT, 3u, 100u, 0u),
      DEF_CONST(6, Instruction::CONST, 4u, 1000),
      DEF_APUT(6, Instruction::APUT, 4u, 3u, 1u),            // Index is Phi 1u.
      DEF_INVOKE1(8, Instruction::INVOKE_STATIC, 100u),
      DEF_IGET(8, Instruction::IGET_OBJECT, 7u, 100u, 0u),
      DEF_CONST(8, Instruction::CONST, 8u, 2000),
      DEF_APUT(8, Instruction::APUT, 9u, 7u, 1u),            // Index is Phi 1u.
      DEF_CONST(9, Instruction::CONST, 10u, 3000),
  };
  PrepareIFields(ifields);
  PrepareBasicBlocks(bbs);
  PrepareMIRs(mirs);
  // Using DFS order for this test. The GVN result should not depend on the used ordering
  // once the GVN actually converges. But creating a test for this convergence issue with
  // the topological ordering could be a very challenging task.
  PerformPreOrderDfsGVN();
}

TEST_F(GlobalValueNumberingTestTwoConsecutiveLoops, IFieldAndPhi) {
  static const IFieldDef ifields[] = {
      { 0u, 1u, 0u, false },  // Int.
  };
  static const MIRDef mirs[] = {
      DEF_MOVE(3, Instruction::MOVE_OBJECT, 0u, 100u),
      DEF_IPUT(3, Instruction::IPUT_OBJECT, 0u, 200u, 0u),
      DEF_PHI2(4, 2u, 0u, 3u),
      DEF_MOVE(5, Instruction::MOVE_OBJECT, 3u, 300u),
      DEF_IPUT(5, Instruction::IPUT_OBJECT, 3u, 200u, 0u),
      DEF_MOVE(6, Instruction::MOVE_OBJECT, 5u, 2u),
      DEF_IGET(6, Instruction::IGET_OBJECT, 6u, 200u, 0u),
      DEF_MOVE(7, Instruction::MOVE_OBJECT, 7u, 5u),
      DEF_IGET(7, Instruction::IGET_OBJECT, 8u, 200u, 0u),
      DEF_MOVE(8, Instruction::MOVE_OBJECT, 9u, 5u),
      DEF_IGET(8, Instruction::IGET_OBJECT, 10u, 200u, 0u),
      DEF_MOVE(9, Instruction::MOVE_OBJECT, 11u, 5u),
      DEF_IGET(9, Instruction::IGET_OBJECT, 12u, 200u, 0u),
  };

  PrepareIFields(ifields);
  PrepareMIRs(mirs);
  PerformGVN();
  ASSERT_EQ(arraysize(mirs), value_names_.size());
  EXPECT_NE(value_names_[0], value_names_[3]);
  EXPECT_NE(value_names_[0], value_names_[2]);
  EXPECT_NE(value_names_[3], value_names_[2]);
  EXPECT_EQ(value_names_[2], value_names_[5]);
  EXPECT_EQ(value_names_[5], value_names_[6]);
  EXPECT_EQ(value_names_[5], value_names_[7]);
  EXPECT_EQ(value_names_[5], value_names_[8]);
  EXPECT_EQ(value_names_[5], value_names_[9]);
  EXPECT_EQ(value_names_[5], value_names_[10]);
  EXPECT_EQ(value_names_[5], value_names_[11]);
  EXPECT_EQ(value_names_[5], value_names_[12]);
}

TEST_F(GlobalValueNumberingTestTwoConsecutiveLoops, NullCheck) {
  static const IFieldDef ifields[] = {
      { 0u, 1u, 0u, false },  // Int.
  };
  static const SFieldDef sfields[] = {
      { 0u, 1u, 0u, false },  // Int.
  };
  static const MIRDef mirs[] = {
      DEF_MOVE(3, Instruction::MOVE_OBJECT, 0u, 100u),
      DEF_IGET(3, Instruction::IGET_OBJECT, 1u, 200u, 0u),
      DEF_SGET(3, Instruction::SGET_OBJECT, 2u, 0u),
      DEF_AGET(3, Instruction::AGET_OBJECT, 3u, 300u, 201u),
      DEF_PHI2(4, 4u, 0u, 8u),
      DEF_IGET(5, Instruction::IGET_OBJECT, 5u, 200u, 0u),
      DEF_SGET(5, Instruction::SGET_OBJECT, 6u, 0u),
      DEF_AGET(5, Instruction::AGET_OBJECT, 7u, 300u, 201u),
      DEF_MOVE(5, Instruction::MOVE_OBJECT, 8u, 400u),
      DEF_IPUT(5, Instruction::IPUT_OBJECT, 4u, 200u, 0u),          // PUT the Phi 4u.
      DEF_SPUT(5, Instruction::SPUT_OBJECT, 4u, 0u),                // PUT the Phi 4u.
      DEF_APUT(5, Instruction::APUT_OBJECT, 4u, 300u, 201u),        // PUT the Phi 4u.
      DEF_MOVE(6, Instruction::MOVE_OBJECT, 12u, 4u),
      DEF_IGET(6, Instruction::IGET_OBJECT, 13u, 200u, 0u),
      DEF_SGET(6, Instruction::SGET_OBJECT, 14u, 0u),
      DEF_AGET(6, Instruction::AGET_OBJECT, 15u, 300u, 201u),
      DEF_AGET(6, Instruction::AGET_OBJECT, 16u, 12u, 600u),
      DEF_AGET(6, Instruction::AGET_OBJECT, 17u, 13u, 600u),
      DEF_AGET(6, Instruction::AGET_OBJECT, 18u, 14u, 600u),
      DEF_AGET(6, Instruction::AGET_OBJECT, 19u, 15u, 600u),
      DEF_MOVE(8, Instruction::MOVE_OBJECT, 20u, 12u),
      DEF_IGET(8, Instruction::IGET_OBJECT, 21u, 200u, 0u),
      DEF_SGET(8, Instruction::SGET_OBJECT, 22u, 0u),
      DEF_AGET(8, Instruction::AGET_OBJECT, 23u, 300u, 201u),
      DEF_AGET(8, Instruction::AGET_OBJECT, 24u, 12u, 600u),
      DEF_AGET(8, Instruction::AGET_OBJECT, 25u, 13u, 600u),
      DEF_AGET(8, Instruction::AGET_OBJECT, 26u, 14u, 600u),
      DEF_AGET(8, Instruction::AGET_OBJECT, 27u, 15u, 600u),
      DEF_MOVE(9, Instruction::MOVE_OBJECT, 28u, 12u),
      DEF_IGET(9, Instruction::IGET_OBJECT, 29u, 200u, 0u),
      DEF_SGET(9, Instruction::SGET_OBJECT, 30u, 0u),
      DEF_AGET(9, Instruction::AGET_OBJECT, 31u, 300u, 201u),
      DEF_AGET(9, Instruction::AGET_OBJECT, 32u, 12u, 600u),
      DEF_AGET(9, Instruction::AGET_OBJECT, 33u, 13u, 600u),
      DEF_AGET(9, Instruction::AGET_OBJECT, 34u, 14u, 600u),
      DEF_AGET(9, Instruction::AGET_OBJECT, 35u, 15u, 600u),
  };
  static const bool expected_ignore_null_check[] = {
      false, false, false, false,                                   // BB #3.
      false, true, false, true, false, true, false, true,           // BBs #4 and #5.
      false, true, false, true, false, false, false, false,         // BB #6.
      false, true, false, true, true, true, true, true,             // BB #7.
      false, true, false, true, true, true, true, true,             // BB #8.
  };
  static const bool expected_ignore_range_check[] = {
      false, false, false, false,                                   // BB #3.
      false, false, false, true, false, false, false, true,         // BBs #4 and #5.
      false, false, false, true, false, false, false, false,        // BB #6.
      false, false, false, true, true, true, true, true,            // BB #7.
      false, false, false, true, true, true, true, true,            // BB #8.
  };

  PrepareIFields(ifields);
  PrepareSFields(sfields);
  PrepareMIRs(mirs);
  PerformGVN();
  ASSERT_EQ(arraysize(mirs), value_names_.size());
  EXPECT_NE(value_names_[0], value_names_[4]);
  EXPECT_NE(value_names_[1], value_names_[5]);
  EXPECT_NE(value_names_[2], value_names_[6]);
  EXPECT_NE(value_names_[3], value_names_[7]);
  EXPECT_NE(value_names_[4], value_names_[8]);
  EXPECT_EQ(value_names_[4], value_names_[12]);
  EXPECT_EQ(value_names_[5], value_names_[13]);
  EXPECT_EQ(value_names_[6], value_names_[14]);
  EXPECT_EQ(value_names_[7], value_names_[15]);
  EXPECT_EQ(value_names_[12], value_names_[20]);
  EXPECT_EQ(value_names_[13], value_names_[21]);
  EXPECT_EQ(value_names_[14], value_names_[22]);
  EXPECT_EQ(value_names_[15], value_names_[23]);
  EXPECT_EQ(value_names_[12], value_names_[28]);
  EXPECT_EQ(value_names_[13], value_names_[29]);
  EXPECT_EQ(value_names_[14], value_names_[30]);
  EXPECT_EQ(value_names_[15], value_names_[31]);
  PerformGVNCodeModifications();
  for (size_t i = 0u; i != arraysize(mirs); ++i) {
    EXPECT_EQ(expected_ignore_null_check[i],
              (mirs_[i].optimization_flags & MIR_IGNORE_NULL_CHECK) != 0) << i;
    EXPECT_EQ(expected_ignore_range_check[i],
              (mirs_[i].optimization_flags & MIR_IGNORE_RANGE_CHECK) != 0) << i;
  }
}

TEST_F(GlobalValueNumberingTestTwoNestedLoops, IFieldAndPhi) {
  static const IFieldDef ifields[] = {
      { 0u, 1u, 0u, false },  // Int.
  };
  static const MIRDef mirs[] = {
      DEF_MOVE(3, Instruction::MOVE_OBJECT, 0u, 100u),
      DEF_IPUT(3, Instruction::IPUT_OBJECT, 0u, 200u, 0u),
      DEF_PHI2(4, 2u, 0u, 11u),
      DEF_MOVE(4, Instruction::MOVE_OBJECT, 3u, 2u),
      DEF_IGET(4, Instruction::IGET_OBJECT, 4u, 200u, 0u),
      DEF_MOVE(5, Instruction::MOVE_OBJECT, 5u, 3u),
      DEF_IGET(5, Instruction::IGET_OBJECT, 6u, 200u, 0u),
      DEF_MOVE(6, Instruction::MOVE_OBJECT, 7u, 3u),
      DEF_IGET(6, Instruction::IGET_OBJECT, 8u, 200u, 0u),
      DEF_MOVE(7, Instruction::MOVE_OBJECT, 9u, 3u),
      DEF_IGET(7, Instruction::IGET_OBJECT, 10u, 200u, 0u),
      DEF_MOVE(7, Instruction::MOVE_OBJECT, 11u, 300u),
      DEF_IPUT(7, Instruction::IPUT_OBJECT, 11u, 200u, 0u),
      DEF_MOVE(8, Instruction::MOVE_OBJECT, 13u, 3u),
      DEF_IGET(8, Instruction::IGET_OBJECT, 14u, 200u, 0u),
  };

  PrepareIFields(ifields);
  PrepareMIRs(mirs);
  PerformGVN();
  ASSERT_EQ(arraysize(mirs), value_names_.size());
  EXPECT_NE(value_names_[0], value_names_[11]);
  EXPECT_NE(value_names_[0], value_names_[2]);
  EXPECT_NE(value_names_[11], value_names_[2]);
  EXPECT_EQ(value_names_[2], value_names_[3]);
  EXPECT_EQ(value_names_[3], value_names_[4]);
  EXPECT_EQ(value_names_[3], value_names_[5]);
  EXPECT_EQ(value_names_[3], value_names_[6]);
  EXPECT_EQ(value_names_[3], value_names_[7]);
  EXPECT_EQ(value_names_[3], value_names_[8]);
  EXPECT_EQ(value_names_[3], value_names_[9]);
  EXPECT_EQ(value_names_[3], value_names_[10]);
  EXPECT_EQ(value_names_[3], value_names_[13]);
  EXPECT_EQ(value_names_[3], value_names_[14]);
}

TEST_F(GlobalValueNumberingTest, NormalPathToCatchEntry) {
  // When there's an empty catch block, all the exception paths lead to the next block in
  // the normal path and we can also have normal "taken" or "fall-through" branches to that
  // path. Check that LocalValueNumbering::PruneNonAliasingRefsForCatch() can handle it.
  static const BBDef bbs[] = {
      DEF_BB(kNullBlock, DEF_SUCC0(), DEF_PRED0()),
      DEF_BB(kEntryBlock, DEF_SUCC1(3), DEF_PRED0()),
      DEF_BB(kExitBlock, DEF_SUCC0(), DEF_PRED1(5)),
      DEF_BB(kDalvikByteCode, DEF_SUCC1(4), DEF_PRED1(1)),
      DEF_BB(kDalvikByteCode, DEF_SUCC1(5), DEF_PRED1(3)),
      DEF_BB(kDalvikByteCode, DEF_SUCC1(2), DEF_PRED2(3, 4)),
  };
  static const MIRDef mirs[] = {
      DEF_INVOKE1(4, Instruction::INVOKE_STATIC, 100u),
  };
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
  BasicBlock* merge_block = cu_.mir_graph->GetBasicBlock(4u);
  std::swap(merge_block->taken, merge_block->fall_through);
  PrepareMIRs(mirs);
  PerformGVN();
}

}  // namespace art
