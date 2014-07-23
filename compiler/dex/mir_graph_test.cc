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

#include "mir_graph.h"
#include "gtest/gtest.h"

namespace art {

class TopologicalSortOrderTest : public testing::Test {
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

  void ComputeTopologicalSortOrder() {
    cu_.mir_graph->SSATransformationStart();
    cu_.mir_graph->ComputeDFSOrders();
    cu_.mir_graph->ComputeDominators();
    cu_.mir_graph->ComputeTopologicalSortOrder();
    cu_.mir_graph->SSATransformationEnd();
    ASSERT_NE(cu_.mir_graph->topological_order_, nullptr);
    ASSERT_NE(cu_.mir_graph->topological_order_loop_ends_, nullptr);
    ASSERT_NE(cu_.mir_graph->topological_order_indexes_, nullptr);
    ASSERT_EQ(cu_.mir_graph->GetNumBlocks(), cu_.mir_graph->topological_order_indexes_->Size());
    for (size_t i = 0, size = cu_.mir_graph->GetTopologicalSortOrder()->Size(); i != size; ++i) {
      ASSERT_LT(cu_.mir_graph->topological_order_->Get(i), cu_.mir_graph->GetNumBlocks());
      BasicBlockId id = cu_.mir_graph->topological_order_->Get(i);
      EXPECT_EQ(i, cu_.mir_graph->topological_order_indexes_->Get(id));
    }
  }

  void DoCheckOrder(const BasicBlockId* ids, size_t count) {
    ASSERT_EQ(count, cu_.mir_graph->GetTopologicalSortOrder()->Size());
    for (size_t i = 0; i != count; ++i) {
      EXPECT_EQ(ids[i], cu_.mir_graph->GetTopologicalSortOrder()->Get(i)) << i;
    }
  }

  template <size_t count>
  void CheckOrder(const BasicBlockId (&ids)[count]) {
    DoCheckOrder(ids, count);
  }

  void DoCheckLoopEnds(const uint16_t* ends, size_t count) {
    for (size_t i = 0; i != count; ++i) {
      ASSERT_LT(i, cu_.mir_graph->GetTopologicalSortOrderLoopEnds()->Size());
      EXPECT_EQ(ends[i], cu_.mir_graph->GetTopologicalSortOrderLoopEnds()->Get(i)) << i;
    }
  }

  template <size_t count>
  void CheckLoopEnds(const uint16_t (&ends)[count]) {
    DoCheckLoopEnds(ends, count);
  }

  TopologicalSortOrderTest()
      : pool_(),
        cu_(&pool_) {
    cu_.mir_graph.reset(new MIRGraph(&cu_, &cu_.arena));
  }

  ArenaPool pool_;
  CompilationUnit cu_;
};

TEST_F(TopologicalSortOrderTest, DoWhile) {
  const BBDef bbs[] = {
      DEF_BB(kNullBlock, DEF_SUCC0(), DEF_PRED0()),
      DEF_BB(kEntryBlock, DEF_SUCC1(3), DEF_PRED0()),
      DEF_BB(kExitBlock, DEF_SUCC0(), DEF_PRED1(5)),
      DEF_BB(kDalvikByteCode, DEF_SUCC1(4), DEF_PRED1(1)),
      DEF_BB(kDalvikByteCode, DEF_SUCC2(5, 4), DEF_PRED2(3, 4)),  // "taken" loops to self.
      DEF_BB(kDalvikByteCode, DEF_SUCC1(2), DEF_PRED1(4)),
  };
  const BasicBlockId expected_order[] = {
      1, 3, 4, 5, 2
  };
  const uint16_t loop_ends[] = {
      0, 0, 3, 0, 0
  };

  PrepareBasicBlocks(bbs);
  ComputeTopologicalSortOrder();
  CheckOrder(expected_order);
  CheckLoopEnds(loop_ends);
}

TEST_F(TopologicalSortOrderTest, While) {
  const BBDef bbs[] = {
      DEF_BB(kNullBlock, DEF_SUCC0(), DEF_PRED0()),
      DEF_BB(kEntryBlock, DEF_SUCC1(3), DEF_PRED0()),
      DEF_BB(kExitBlock, DEF_SUCC0(), DEF_PRED1(5)),
      DEF_BB(kDalvikByteCode, DEF_SUCC2(4, 5), DEF_PRED2(1, 4)),
      DEF_BB(kDalvikByteCode, DEF_SUCC1(3), DEF_PRED1(3)),     // Loops to 3.
      DEF_BB(kDalvikByteCode, DEF_SUCC1(2), DEF_PRED1(3)),
  };
  const BasicBlockId expected_order[] = {
      1, 3, 4, 5, 2
  };
  const uint16_t loop_ends[] = {
      0, 3, 0, 0, 0
  };

  PrepareBasicBlocks(bbs);
  ComputeTopologicalSortOrder();
  CheckOrder(expected_order);
  CheckLoopEnds(loop_ends);
}

TEST_F(TopologicalSortOrderTest, WhileWithTwoBackEdges) {
  const BBDef bbs[] = {
      DEF_BB(kNullBlock, DEF_SUCC0(), DEF_PRED0()),
      DEF_BB(kEntryBlock, DEF_SUCC1(3), DEF_PRED0()),
      DEF_BB(kExitBlock, DEF_SUCC0(), DEF_PRED1(6)),
      DEF_BB(kDalvikByteCode, DEF_SUCC2(4, 6), DEF_PRED3(1, 4, 5)),
      DEF_BB(kDalvikByteCode, DEF_SUCC2(5, 3), DEF_PRED1(3)),     // Loops to 3.
      DEF_BB(kDalvikByteCode, DEF_SUCC1(3), DEF_PRED1(4)),        // Loops to 3.
      DEF_BB(kDalvikByteCode, DEF_SUCC1(2), DEF_PRED1(3)),
  };
  const BasicBlockId expected_order[] = {
      1, 3, 4, 5, 6, 2
  };
  const uint16_t loop_ends[] = {
      0, 4, 0, 0, 0, 0
  };

  PrepareBasicBlocks(bbs);
  ComputeTopologicalSortOrder();
  CheckOrder(expected_order);
  CheckLoopEnds(loop_ends);
}

TEST_F(TopologicalSortOrderTest, NestedLoop) {
  const BBDef bbs[] = {
      DEF_BB(kNullBlock, DEF_SUCC0(), DEF_PRED0()),
      DEF_BB(kEntryBlock, DEF_SUCC1(3), DEF_PRED0()),
      DEF_BB(kExitBlock, DEF_SUCC0(), DEF_PRED1(7)),
      DEF_BB(kDalvikByteCode, DEF_SUCC2(4, 7), DEF_PRED2(1, 6)),
      DEF_BB(kDalvikByteCode, DEF_SUCC2(5, 6), DEF_PRED2(3, 5)),
      DEF_BB(kDalvikByteCode, DEF_SUCC1(4), DEF_PRED1(4)),            // Loops to 4.
      DEF_BB(kDalvikByteCode, DEF_SUCC1(3), DEF_PRED1(4)),            // Loops to 3.
      DEF_BB(kDalvikByteCode, DEF_SUCC1(2), DEF_PRED1(3)),
  };
  const BasicBlockId expected_order[] = {
      1, 3, 4, 5, 6, 7, 2
  };
  const uint16_t loop_ends[] = {
      0, 5, 4, 0, 0, 0, 0
  };

  PrepareBasicBlocks(bbs);
  ComputeTopologicalSortOrder();
  CheckOrder(expected_order);
  CheckLoopEnds(loop_ends);
}

TEST_F(TopologicalSortOrderTest, NestedLoopHeadLoops) {
  const BBDef bbs[] = {
      DEF_BB(kNullBlock, DEF_SUCC0(), DEF_PRED0()),
      DEF_BB(kEntryBlock, DEF_SUCC1(3), DEF_PRED0()),
      DEF_BB(kExitBlock, DEF_SUCC0(), DEF_PRED1(6)),
      DEF_BB(kDalvikByteCode, DEF_SUCC2(4, 6), DEF_PRED2(1, 4)),
      DEF_BB(kDalvikByteCode, DEF_SUCC2(5, 3), DEF_PRED2(3, 5)),      // Nested head, loops to 3.
      DEF_BB(kDalvikByteCode, DEF_SUCC1(4), DEF_PRED1(4)),            // Loops to 4.
      DEF_BB(kDalvikByteCode, DEF_SUCC1(2), DEF_PRED1(3)),
  };
  const BasicBlockId expected_order[] = {
      1, 3, 4, 5, 6, 2
  };
  const uint16_t loop_ends[] = {
      0, 4, 4, 0, 0, 0
  };

  PrepareBasicBlocks(bbs);
  ComputeTopologicalSortOrder();
  CheckOrder(expected_order);
  CheckLoopEnds(loop_ends);
}

TEST_F(TopologicalSortOrderTest, NestedLoopSameBackBranchBlock) {
  const BBDef bbs[] = {
      DEF_BB(kNullBlock, DEF_SUCC0(), DEF_PRED0()),
      DEF_BB(kEntryBlock, DEF_SUCC1(3), DEF_PRED0()),
      DEF_BB(kExitBlock, DEF_SUCC0(), DEF_PRED1(6)),
      DEF_BB(kDalvikByteCode, DEF_SUCC2(4, 6), DEF_PRED2(1, 5)),
      DEF_BB(kDalvikByteCode, DEF_SUCC1(5), DEF_PRED2(3, 5)),
      DEF_BB(kDalvikByteCode, DEF_SUCC2(4, 3), DEF_PRED1(4)),         // Loops to 4 and 3.
      DEF_BB(kDalvikByteCode, DEF_SUCC1(2), DEF_PRED1(3)),
  };
  const BasicBlockId expected_order[] = {
      1, 3, 4, 5, 6, 2
  };
  const uint16_t loop_ends[] = {
      0, 4, 4, 0, 0, 0
  };

  PrepareBasicBlocks(bbs);
  ComputeTopologicalSortOrder();
  CheckOrder(expected_order);
  CheckLoopEnds(loop_ends);
}

TEST_F(TopologicalSortOrderTest, TwoReorderedInnerLoops) {
  // This is a simplified version of real code graph where the branch from 8 to 5 must prevent
  // the block 5 from being considered a loop head before processing the loop 7-8.
  const BBDef bbs[] = {
      DEF_BB(kNullBlock, DEF_SUCC0(), DEF_PRED0()),
      DEF_BB(kEntryBlock, DEF_SUCC1(3), DEF_PRED0()),
      DEF_BB(kExitBlock, DEF_SUCC0(), DEF_PRED1(9)),
      DEF_BB(kDalvikByteCode, DEF_SUCC2(4, 9), DEF_PRED2(1, 5)),
      DEF_BB(kDalvikByteCode, DEF_SUCC2(5, 7), DEF_PRED1(3)),         // Branch over loop in 5.
      DEF_BB(kDalvikByteCode, DEF_SUCC2(6, 3), DEF_PRED3(4, 6, 8)),   // Loops to 4; inner loop.
      DEF_BB(kDalvikByteCode, DEF_SUCC1(5), DEF_PRED1(5)),            // Loops to 5.
      DEF_BB(kDalvikByteCode, DEF_SUCC1(8), DEF_PRED2(4, 8)),         // Loop head.
      DEF_BB(kDalvikByteCode, DEF_SUCC2(7, 5), DEF_PRED1(7)),         // Loops to 7; branches to 5.
      DEF_BB(kDalvikByteCode, DEF_SUCC1(2), DEF_PRED1(3)),
  };
  const BasicBlockId expected_order[] = {
      1, 3, 4, 7, 8, 5, 6, 9, 2
  };
  const uint16_t loop_ends[] = {
      0, 7, 0, 5, 0, 7, 0, 0, 0
  };

  PrepareBasicBlocks(bbs);
  ComputeTopologicalSortOrder();
  CheckOrder(expected_order);
  CheckLoopEnds(loop_ends);
}

TEST_F(TopologicalSortOrderTest, NestedLoopWithBackEdgeAfterOuterLoopBackEdge) {
  // This is a simplified version of real code graph. The back-edge from 7 to the inner
  // loop head 4 comes after the back-edge from 6 to the outer loop head 3. To make this
  // appear a bit more complex, there's also a back-edge from 5 to 4.
  const BBDef bbs[] = {
      DEF_BB(kNullBlock, DEF_SUCC0(), DEF_PRED0()),
      DEF_BB(kEntryBlock, DEF_SUCC1(3), DEF_PRED0()),
      DEF_BB(kExitBlock, DEF_SUCC0(), DEF_PRED1(7)),
      DEF_BB(kDalvikByteCode, DEF_SUCC1(4), DEF_PRED2(1, 6)),         // Outer loop head.
      DEF_BB(kDalvikByteCode, DEF_SUCC2(5, 6), DEF_PRED3(3, 5, 7)),   // Inner loop head.
      DEF_BB(kDalvikByteCode, DEF_SUCC1(4), DEF_PRED1(4)),            // Loops to inner loop head 4.
      DEF_BB(kDalvikByteCode, DEF_SUCC2(7, 3), DEF_PRED1(4)),         // Loops to outer loop head 3.
      DEF_BB(kDalvikByteCode, DEF_SUCC2(2, 4), DEF_PRED1(6)),         // Loops to inner loop head 4.
  };
  const BasicBlockId expected_order[] = {
      // NOTE: The 5 goes before 6 only because 5 is a "fall-through" from 4 while 6 is "taken".
      1, 3, 4, 5, 6, 7, 2
  };
  const uint16_t loop_ends[] = {
      0, 6, 6, 0, 0, 0, 0
  };

  PrepareBasicBlocks(bbs);
  ComputeTopologicalSortOrder();
  CheckOrder(expected_order);
  CheckLoopEnds(loop_ends);
}

TEST_F(TopologicalSortOrderTest, LoopWithTwoEntryPoints) {
  const BBDef bbs[] = {
      DEF_BB(kNullBlock, DEF_SUCC0(), DEF_PRED0()),
      DEF_BB(kEntryBlock, DEF_SUCC1(3), DEF_PRED0()),
      DEF_BB(kExitBlock, DEF_SUCC0(), DEF_PRED1(7)),
      DEF_BB(kDalvikByteCode, DEF_SUCC2(5, 4), DEF_PRED1(1)),
      DEF_BB(kDalvikByteCode, DEF_SUCC1(5), DEF_PRED2(3, 6)),  // Fall-back block is chosen as
      DEF_BB(kDalvikByteCode, DEF_SUCC1(6), DEF_PRED2(3, 4)),  // the earlier from these two.
      DEF_BB(kDalvikByteCode, DEF_SUCC2(4, 7), DEF_PRED1(5)),
      DEF_BB(kDalvikByteCode, DEF_SUCC1(2), DEF_PRED1(6)),
  };
  const BasicBlockId expected_order[] = {
      1, 3, 4, 5, 6, 7, 2
  };
  const uint16_t loop_ends[] = {
      0, 0, 5, 0, 0, 0, 0
  };

  PrepareBasicBlocks(bbs);
  ComputeTopologicalSortOrder();
  CheckOrder(expected_order);
  CheckLoopEnds(loop_ends);
}

}  // namespace art
