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

#include <regex>

#include "base/arena_allocator.h"
#include "builder.h"
#include "induction_var_analysis.h"
#include "nodes.h"
#include "optimizing_unit_test.h"

namespace art {

/**
 * Fixture class for the InductionVarAnalysis tests.
 */
class InductionVarAnalysisTest : public CommonCompilerTest {
 public:
  InductionVarAnalysisTest() : pool_(), allocator_(&pool_) {
    graph_ = CreateGraph(&allocator_);
  }

  ~InductionVarAnalysisTest() { }

  // Builds single for-loop at depth d.
  void BuildForLoop(int d, int n) {
    ASSERT_LT(d, n);
    loop_preheader_[d] = new (&allocator_) HBasicBlock(graph_);
    graph_->AddBlock(loop_preheader_[d]);
    loop_header_[d] = new (&allocator_) HBasicBlock(graph_);
    graph_->AddBlock(loop_header_[d]);
    loop_preheader_[d]->AddSuccessor(loop_header_[d]);
    if (d < (n - 1)) {
      BuildForLoop(d + 1, n);
    }
    loop_body_[d] = new (&allocator_) HBasicBlock(graph_);
    graph_->AddBlock(loop_body_[d]);
    loop_body_[d]->AddSuccessor(loop_header_[d]);
    if (d < (n - 1)) {
      loop_header_[d]->AddSuccessor(loop_preheader_[d + 1]);
      loop_header_[d + 1]->AddSuccessor(loop_body_[d]);
    } else {
      loop_header_[d]->AddSuccessor(loop_body_[d]);
    }
  }

  // Builds a n-nested loop in CFG where each loop at depth 0 <= d < n
  // is defined as "for (int i_d = 0; i_d < 100; i_d++)". Tests can further
  // populate the loop with instructions to set up interesting scenarios.
  void BuildLoopNest(int n) {
    ASSERT_LE(n, 10);
    graph_->SetNumberOfVRegs(n + 3);

    // Build basic blocks with entry, nested loop, exit.
    entry_ = new (&allocator_) HBasicBlock(graph_);
    graph_->AddBlock(entry_);
    BuildForLoop(0, n);
    return_ = new (&allocator_) HBasicBlock(graph_);
    graph_->AddBlock(return_);
    exit_ = new (&allocator_) HBasicBlock(graph_);
    graph_->AddBlock(exit_);
    entry_->AddSuccessor(loop_preheader_[0]);
    loop_header_[0]->AddSuccessor(return_);
    return_->AddSuccessor(exit_);
    graph_->SetEntryBlock(entry_);
    graph_->SetExitBlock(exit_);

    // Provide entry and exit instructions.
    parameter_ = new (&allocator_) HParameterValue(
        graph_->GetDexFile(), 0, 0, Primitive::kPrimNot, true);
    entry_->AddInstruction(parameter_);
    constant0_ = graph_->GetIntConstant(0);
    constant1_ = graph_->GetIntConstant(1);
    constant100_ = graph_->GetIntConstant(100);
    induc_ = new (&allocator_) HLocal(n);
    entry_->AddInstruction(induc_);
    entry_->AddInstruction(new (&allocator_) HStoreLocal(induc_, constant0_));
    tmp_ = new (&allocator_) HLocal(n + 1);
    entry_->AddInstruction(tmp_);
    entry_->AddInstruction(new (&allocator_) HStoreLocal(tmp_, constant100_));
    dum_ = new (&allocator_) HLocal(n + 2);
    entry_->AddInstruction(dum_);
    return_->AddInstruction(new (&allocator_) HReturnVoid());
    exit_->AddInstruction(new (&allocator_) HExit());

    // Provide loop instructions.
    for (int d = 0; d < n; d++) {
      basic_[d] = new (&allocator_) HLocal(d);
      entry_->AddInstruction(basic_[d]);
      loop_preheader_[d]->AddInstruction(new (&allocator_) HStoreLocal(basic_[d], constant0_));
      loop_preheader_[d]->AddInstruction(new (&allocator_) HGoto());
      HInstruction* load = new (&allocator_) HLoadLocal(basic_[d], Primitive::kPrimInt);
      loop_header_[d]->AddInstruction(load);
      HInstruction* compare = new (&allocator_) HLessThan(load, constant100_);
      loop_header_[d]->AddInstruction(compare);
      loop_header_[d]->AddInstruction(new (&allocator_) HIf(compare));
      load = new (&allocator_) HLoadLocal(basic_[d], Primitive::kPrimInt);
      loop_body_[d]->AddInstruction(load);
      increment_[d] = new (&allocator_) HAdd(Primitive::kPrimInt, load, constant1_);
      loop_body_[d]->AddInstruction(increment_[d]);
      loop_body_[d]->AddInstruction(new (&allocator_) HStoreLocal(basic_[d], increment_[d]));
      loop_body_[d]->AddInstruction(new (&allocator_) HGoto());
    }
  }

  // Builds if-statement at depth d.
  void BuildIf(int d, HBasicBlock** ifT, HBasicBlock **ifF) {
    HBasicBlock* cond = new (&allocator_) HBasicBlock(graph_);
    HBasicBlock* ifTrue = new (&allocator_) HBasicBlock(graph_);
    HBasicBlock* ifFalse = new (&allocator_) HBasicBlock(graph_);
    graph_->AddBlock(cond);
    graph_->AddBlock(ifTrue);
    graph_->AddBlock(ifFalse);
    // Conditional split.
    loop_header_[d]->ReplaceSuccessor(loop_body_[d], cond);
    cond->AddSuccessor(ifTrue);
    cond->AddSuccessor(ifFalse);
    ifTrue->AddSuccessor(loop_body_[d]);
    ifFalse->AddSuccessor(loop_body_[d]);
    cond->AddInstruction(new (&allocator_) HIf(parameter_));
    *ifT = ifTrue;
    *ifF = ifFalse;
  }

  // Inserts instruction right before increment at depth d.
  HInstruction* InsertInstruction(HInstruction* instruction, int d) {
    loop_body_[d]->InsertInstructionBefore(instruction, increment_[d]);
    return instruction;
  }

  // Inserts local load at depth d.
  HInstruction* InsertLocalLoad(HLocal* local, int d) {
    return InsertInstruction(new (&allocator_) HLoadLocal(local, Primitive::kPrimInt), d);
  }

  // Inserts local store at depth d.
  HInstruction* InsertLocalStore(HLocal* local, HInstruction* rhs, int d) {
    return InsertInstruction(new (&allocator_) HStoreLocal(local, rhs), d);
  }

  // Inserts an array store with given local as subscript at depth d to
  // enable tests to inspect the computed induction at that point easily.
  HInstruction* InsertArrayStore(HLocal* subscript, int d) {
    HInstruction* load = InsertInstruction(
        new (&allocator_) HLoadLocal(subscript, Primitive::kPrimInt), d);
    return InsertInstruction(new (&allocator_) HArraySet(
        parameter_, load, constant0_, Primitive::kPrimInt, 0), d);
  }

  // Returns induction information of instruction in loop at depth d.
  std::string GetInductionInfo(HInstruction* instruction, int d) {
    return HInductionVarAnalysis::InductionToString(
        iva_->LookupInfo(loop_body_[d]->GetLoopInformation(), instruction));
  }

  // Performs InductionVarAnalysis (after proper set up).
  void PerformInductionVarAnalysis() {
    TransformToSsa(graph_);
    iva_ = new (&allocator_) HInductionVarAnalysis(graph_);
    iva_->Run();
  }

  // General building fields.
  ArenaPool pool_;
  ArenaAllocator allocator_;
  HGraph* graph_;
  HInductionVarAnalysis* iva_;

  // Fixed basic blocks and instructions.
  HBasicBlock* entry_;
  HBasicBlock* return_;
  HBasicBlock* exit_;
  HInstruction* parameter_;  // "this"
  HInstruction* constant0_;
  HInstruction* constant1_;
  HInstruction* constant100_;
  HLocal* induc_;  // "vreg_n", the "k"
  HLocal* tmp_;    // "vreg_n+1"
  HLocal* dum_;    // "vreg_n+2"

  // Loop specifics.
  HBasicBlock* loop_preheader_[10];
  HBasicBlock* loop_header_[10];
  HBasicBlock* loop_body_[10];
  HInstruction* increment_[10];
  HLocal* basic_[10];  // "vreg_d", the "i_d"
};

//
// The actual InductionVarAnalysis tests.
//

TEST_F(InductionVarAnalysisTest, ProperLoopSetup) {
  // Setup:
  // for (int i_0 = 0; i_0 < 100; i_0++) {
  //   ..
  //     for (int i_9 = 0; i_9 < 100; i_9++) {
  //     }
  //   ..
  // }
  BuildLoopNest(10);
  TransformToSsa(graph_);
  ASSERT_EQ(entry_->GetLoopInformation(), nullptr);
  for (int d = 0; d < 1; d++) {
    ASSERT_EQ(loop_preheader_[d]->GetLoopInformation(),
              (d == 0) ? nullptr
                       : loop_header_[d - 1]->GetLoopInformation());
    ASSERT_NE(loop_header_[d]->GetLoopInformation(), nullptr);
    ASSERT_NE(loop_body_[d]->GetLoopInformation(), nullptr);
    ASSERT_EQ(loop_header_[d]->GetLoopInformation(),
              loop_body_[d]->GetLoopInformation());
  }
  ASSERT_EQ(exit_->GetLoopInformation(), nullptr);
}

TEST_F(InductionVarAnalysisTest, FindBasicInduction) {
  // Setup:
  // for (int i = 0; i < 100; i++) {
  //   a[i] = 0;
  // }
  BuildLoopNest(1);
  HInstruction* store = InsertArrayStore(basic_[0], 0);
  PerformInductionVarAnalysis();

  EXPECT_STREQ("((1) * i + (0))", GetInductionInfo(store->InputAt(1), 0).c_str());
  EXPECT_STREQ("((1) * i + (1))", GetInductionInfo(increment_[0], 0).c_str());

  // Trip-count.
  EXPECT_STREQ("((100) (TC-loop) ((0) < (100)))",
               GetInductionInfo(loop_header_[0]->GetLastInstruction(), 0).c_str());
}

TEST_F(InductionVarAnalysisTest, FindDerivedInduction) {
  // Setup:
  // for (int i = 0; i < 100; i++) {
  //   k = 100 + i;
  //   k = 100 - i;
  //   k = 100 * i;
  //   k = i << 1;
  //   k = - i;
  // }
  BuildLoopNest(1);
  HInstruction *add = InsertInstruction(
      new (&allocator_) HAdd(Primitive::kPrimInt, constant100_, InsertLocalLoad(basic_[0], 0)), 0);
  InsertLocalStore(induc_, add, 0);
  HInstruction *sub = InsertInstruction(
      new (&allocator_) HSub(Primitive::kPrimInt, constant100_, InsertLocalLoad(basic_[0], 0)), 0);
  InsertLocalStore(induc_, sub, 0);
  HInstruction *mul = InsertInstruction(
      new (&allocator_) HMul(Primitive::kPrimInt, constant100_, InsertLocalLoad(basic_[0], 0)), 0);
  InsertLocalStore(induc_, mul, 0);
  HInstruction *shl = InsertInstruction(
      new (&allocator_) HShl(Primitive::kPrimInt, InsertLocalLoad(basic_[0], 0), constant1_), 0);
  InsertLocalStore(induc_, shl, 0);
  HInstruction *neg = InsertInstruction(
      new (&allocator_) HNeg(Primitive::kPrimInt, InsertLocalLoad(basic_[0], 0)), 0);
  InsertLocalStore(induc_, neg, 0);
  PerformInductionVarAnalysis();

  EXPECT_STREQ("((1) * i + (100))", GetInductionInfo(add, 0).c_str());
  EXPECT_STREQ("(( - (1)) * i + (100))", GetInductionInfo(sub, 0).c_str());
  EXPECT_STREQ("((100) * i + (0))", GetInductionInfo(mul, 0).c_str());
  EXPECT_STREQ("((2) * i + (0))", GetInductionInfo(shl, 0).c_str());
  EXPECT_STREQ("(( - (1)) * i + (0))", GetInductionInfo(neg, 0).c_str());
}

TEST_F(InductionVarAnalysisTest, FindChainInduction) {
  // Setup:
  // k = 0;
  // for (int i = 0; i < 100; i++) {
  //   k = k + 100;
  //   a[k] = 0;
  //   k = k - 1;
  //   a[k] = 0;
  // }
  BuildLoopNest(1);
  HInstruction *add = InsertInstruction(
      new (&allocator_) HAdd(Primitive::kPrimInt, InsertLocalLoad(induc_, 0), constant100_), 0);
  InsertLocalStore(induc_, add, 0);
  HInstruction* store1 = InsertArrayStore(induc_, 0);
  HInstruction *sub = InsertInstruction(
      new (&allocator_) HSub(Primitive::kPrimInt, InsertLocalLoad(induc_, 0), constant1_), 0);
  InsertLocalStore(induc_, sub, 0);
  HInstruction* store2 = InsertArrayStore(induc_, 0);
  PerformInductionVarAnalysis();

  EXPECT_STREQ("(((100) - (1)) * i + (100))",
               GetInductionInfo(store1->InputAt(1), 0).c_str());
  EXPECT_STREQ("(((100) - (1)) * i + ((100) - (1)))",
               GetInductionInfo(store2->InputAt(1), 0).c_str());
}

TEST_F(InductionVarAnalysisTest, FindTwoWayBasicInduction) {
  // Setup:
  // k = 0;
  // for (int i = 0; i < 100; i++) {
  //   if () k = k + 1;
  //   else  k = k + 1;
  //   a[k] = 0;
  // }
  BuildLoopNest(1);
  HBasicBlock* ifTrue;
  HBasicBlock* ifFalse;
  BuildIf(0, &ifTrue, &ifFalse);
  // True-branch.
  HInstruction* load1 = new (&allocator_) HLoadLocal(induc_, Primitive::kPrimInt);
  ifTrue->AddInstruction(load1);
  HInstruction* inc1 = new (&allocator_) HAdd(Primitive::kPrimInt, load1, constant1_);
  ifTrue->AddInstruction(inc1);
  ifTrue->AddInstruction(new (&allocator_) HStoreLocal(induc_, inc1));
  // False-branch.
  HInstruction* load2 = new (&allocator_) HLoadLocal(induc_, Primitive::kPrimInt);
  ifFalse->AddInstruction(load2);
  HInstruction* inc2 = new (&allocator_) HAdd(Primitive::kPrimInt, load2, constant1_);
  ifFalse->AddInstruction(inc2);
  ifFalse->AddInstruction(new (&allocator_) HStoreLocal(induc_, inc2));
  // Merge over a phi.
  HInstruction* store = InsertArrayStore(induc_, 0);
  PerformInductionVarAnalysis();

  EXPECT_STREQ("((1) * i + (1))", GetInductionInfo(store->InputAt(1), 0).c_str());
}

TEST_F(InductionVarAnalysisTest, FindTwoWayDerivedInduction) {
  // Setup:
  // for (int i = 0; i < 100; i++) {
  //   if () k = i + 1;
  //   else  k = i + 1;
  //   a[k] = 0;
  // }
  BuildLoopNest(1);
  HBasicBlock* ifTrue;
  HBasicBlock* ifFalse;
  BuildIf(0, &ifTrue, &ifFalse);
  // True-branch.
  HInstruction* load1 = new (&allocator_) HLoadLocal(basic_[0], Primitive::kPrimInt);
  ifTrue->AddInstruction(load1);
  HInstruction* inc1 = new (&allocator_) HAdd(Primitive::kPrimInt, load1, constant1_);
  ifTrue->AddInstruction(inc1);
  ifTrue->AddInstruction(new (&allocator_) HStoreLocal(induc_, inc1));
  // False-branch.
  HInstruction* load2 = new (&allocator_) HLoadLocal(basic_[0], Primitive::kPrimInt);
  ifFalse->AddInstruction(load2);
  HInstruction* inc2 = new (&allocator_) HAdd(Primitive::kPrimInt, load2, constant1_);
  ifFalse->AddInstruction(inc2);
  ifFalse->AddInstruction(new (&allocator_) HStoreLocal(induc_, inc2));
  // Merge over a phi.
  HInstruction* store = InsertArrayStore(induc_, 0);
  PerformInductionVarAnalysis();

  EXPECT_STREQ("((1) * i + (1))", GetInductionInfo(store->InputAt(1), 0).c_str());
}

TEST_F(InductionVarAnalysisTest, FindFirstOrderWrapAroundInduction) {
  // Setup:
  // k = 0;
  // for (int i = 0; i < 100; i++) {
  //   a[k] = 0;
  //   k = 100 - i;
  // }
  BuildLoopNest(1);
  HInstruction* store = InsertArrayStore(induc_, 0);
  HInstruction *sub = InsertInstruction(
      new (&allocator_) HSub(Primitive::kPrimInt, constant100_, InsertLocalLoad(basic_[0], 0)), 0);
  InsertLocalStore(induc_, sub, 0);
  PerformInductionVarAnalysis();

  EXPECT_STREQ("wrap((0), (( - (1)) * i + (100)))",
               GetInductionInfo(store->InputAt(1), 0).c_str());
}

TEST_F(InductionVarAnalysisTest, FindSecondOrderWrapAroundInduction) {
  // Setup:
  // k = 0;
  // t = 100;
  // for (int i = 0; i < 100; i++) {
  //   a[k] = 0;
  //   k = t;
  //   t = 100 - i;
  // }
  BuildLoopNest(1);
  HInstruction* store = InsertArrayStore(induc_, 0);
  InsertLocalStore(induc_, InsertLocalLoad(tmp_, 0), 0);
  HInstruction *sub = InsertInstruction(
      new (&allocator_) HSub(Primitive::kPrimInt, constant100_, InsertLocalLoad(basic_[0], 0)), 0);
  InsertLocalStore(tmp_, sub, 0);
  PerformInductionVarAnalysis();

  EXPECT_STREQ("wrap((0), wrap((100), (( - (1)) * i + (100))))",
               GetInductionInfo(store->InputAt(1), 0).c_str());
}

TEST_F(InductionVarAnalysisTest, FindWrapAroundDerivedInduction) {
  // Setup:
  // k = 0;
  // for (int i = 0; i < 100; i++) {
  //   t = k + 100;
  //   t = k - 100;
  //   t = k * 100;
  //   t = k << 1;
  //   t = - k;
  //   k = i << 1;
  // }
  BuildLoopNest(1);
  HInstruction *add = InsertInstruction(
      new (&allocator_) HAdd(Primitive::kPrimInt, InsertLocalLoad(induc_, 0), constant100_), 0);
  InsertLocalStore(tmp_, add, 0);
  HInstruction *sub = InsertInstruction(
      new (&allocator_) HSub(Primitive::kPrimInt, InsertLocalLoad(induc_, 0), constant100_), 0);
  InsertLocalStore(tmp_, sub, 0);
  HInstruction *mul = InsertInstruction(
      new (&allocator_) HMul(Primitive::kPrimInt, InsertLocalLoad(induc_, 0), constant100_), 0);
  InsertLocalStore(tmp_, mul, 0);
  HInstruction *shl = InsertInstruction(
      new (&allocator_) HShl(Primitive::kPrimInt, InsertLocalLoad(induc_, 0), constant1_), 0);
  InsertLocalStore(tmp_, shl, 0);
  HInstruction *neg = InsertInstruction(
      new (&allocator_) HNeg(Primitive::kPrimInt, InsertLocalLoad(induc_, 0)), 0);
  InsertLocalStore(tmp_, neg, 0);
  InsertLocalStore(
      induc_,
      InsertInstruction(
          new (&allocator_)
          HShl(Primitive::kPrimInt, InsertLocalLoad(basic_[0], 0), constant1_), 0), 0);
  PerformInductionVarAnalysis();

  EXPECT_STREQ("wrap((100), ((2) * i + (100)))", GetInductionInfo(add, 0).c_str());
  EXPECT_STREQ("wrap(((0) - (100)), ((2) * i + ((0) - (100))))", GetInductionInfo(sub, 0).c_str());
  EXPECT_STREQ("wrap((0), (((2) * (100)) * i + (0)))", GetInductionInfo(mul, 0).c_str());
  EXPECT_STREQ("wrap((0), (((2) * (2)) * i + (0)))", GetInductionInfo(shl, 0).c_str());
  EXPECT_STREQ("wrap((0), (( - (2)) * i + (0)))", GetInductionInfo(neg, 0).c_str());
}

TEST_F(InductionVarAnalysisTest, FindPeriodicInduction) {
  // Setup:
  // k = 0;
  // t = 100;
  // for (int i = 0; i < 100; i++) {
  //   a[k] = 0;
  //   a[t] = 0;
  //   // Swap t <-> k.
  //   d = t;
  //   t = k;
  //   k = d;
  // }
  BuildLoopNest(1);
  HInstruction* store1 = InsertArrayStore(induc_, 0);
  HInstruction* store2 = InsertArrayStore(tmp_, 0);
  InsertLocalStore(dum_, InsertLocalLoad(tmp_, 0), 0);
  InsertLocalStore(tmp_, InsertLocalLoad(induc_, 0), 0);
  InsertLocalStore(induc_, InsertLocalLoad(dum_, 0), 0);
  PerformInductionVarAnalysis();

  EXPECT_STREQ("periodic((0), (100))", GetInductionInfo(store1->InputAt(1), 0).c_str());
  EXPECT_STREQ("periodic((100), (0))", GetInductionInfo(store2->InputAt(1), 0).c_str());
}

TEST_F(InductionVarAnalysisTest, FindIdiomaticPeriodicInduction) {
  // Setup:
  // k = 0;
  // for (int i = 0; i < 100; i++) {
  //   a[k] = 0;
  //   k = 1 - k;
  // }
  BuildLoopNest(1);
  HInstruction* store = InsertArrayStore(induc_, 0);
  HInstruction *sub = InsertInstruction(
      new (&allocator_) HSub(Primitive::kPrimInt, constant1_, InsertLocalLoad(induc_, 0)), 0);
  InsertLocalStore(induc_, sub, 0);
  PerformInductionVarAnalysis();

  EXPECT_STREQ("periodic((0), (1))", GetInductionInfo(store->InputAt(1), 0).c_str());
  EXPECT_STREQ("periodic((1), (0))", GetInductionInfo(sub, 0).c_str());
}

TEST_F(InductionVarAnalysisTest, FindDerivedPeriodicInduction) {
  // Setup:
  // k = 0;
  // for (int i = 0; i < 100; i++) {
  //   k = 1 - k;
  //   t = k + 100;
  //   t = k - 100;
  //   t = k * 100;
  //   t = k << 1;
  //   t = - k;
  // }
  BuildLoopNest(1);
  InsertLocalStore(
      induc_,
      InsertInstruction(new (&allocator_)
                        HSub(Primitive::kPrimInt, constant1_, InsertLocalLoad(induc_, 0)), 0), 0);
  // Derived expressions.
  HInstruction *add = InsertInstruction(
      new (&allocator_) HAdd(Primitive::kPrimInt, InsertLocalLoad(induc_, 0), constant100_), 0);
  InsertLocalStore(tmp_, add, 0);
  HInstruction *sub = InsertInstruction(
      new (&allocator_) HSub(Primitive::kPrimInt, InsertLocalLoad(induc_, 0), constant100_), 0);
  InsertLocalStore(tmp_, sub, 0);
  HInstruction *mul = InsertInstruction(
      new (&allocator_) HMul(Primitive::kPrimInt, InsertLocalLoad(induc_, 0), constant100_), 0);
  InsertLocalStore(tmp_, mul, 0);
  HInstruction *shl = InsertInstruction(
      new (&allocator_) HShl(Primitive::kPrimInt, InsertLocalLoad(induc_, 0), constant1_), 0);
  InsertLocalStore(tmp_, shl, 0);
  HInstruction *neg = InsertInstruction(
      new (&allocator_) HNeg(Primitive::kPrimInt, InsertLocalLoad(induc_, 0)), 0);
  InsertLocalStore(tmp_, neg, 0);
  PerformInductionVarAnalysis();

  EXPECT_STREQ("periodic(((1) + (100)), (100))", GetInductionInfo(add, 0).c_str());
  EXPECT_STREQ("periodic(((1) - (100)), ((0) - (100)))", GetInductionInfo(sub, 0).c_str());
  EXPECT_STREQ("periodic((100), (0))", GetInductionInfo(mul, 0).c_str());
  EXPECT_STREQ("periodic((2), (0))", GetInductionInfo(shl, 0).c_str());
  EXPECT_STREQ("periodic(( - (1)), (0))", GetInductionInfo(neg, 0).c_str());
}

TEST_F(InductionVarAnalysisTest, FindDeepLoopInduction) {
  // Setup:
  // k = 0;
  // for (int i_0 = 0; i_0 < 100; i_0++) {
  //   ..
  //     for (int i_9 = 0; i_9 < 100; i_9++) {
  //       k = 1 + k;
  //       a[k] = 0;
  //     }
  //   ..
  // }
  BuildLoopNest(10);
  HInstruction *inc = InsertInstruction(
      new (&allocator_) HAdd(Primitive::kPrimInt, constant1_, InsertLocalLoad(induc_, 9)), 9);
  InsertLocalStore(induc_, inc, 9);
  HInstruction* store = InsertArrayStore(induc_, 9);
  PerformInductionVarAnalysis();

  // Avoid exact phi number, since that depends on the SSA building phase.
  std::regex r("\\(\\(1\\) \\* i \\+ "
               "\\(\\(1\\) \\+ \\(\\d+:Phi\\)\\)\\)");

  for (int d = 0; d < 10; d++) {
    if (d == 9) {
      EXPECT_TRUE(std::regex_match(GetInductionInfo(store->InputAt(1), d), r));
    } else {
      EXPECT_STREQ("", GetInductionInfo(store->InputAt(1), d).c_str());
    }
    EXPECT_STREQ("((1) * i + (1))", GetInductionInfo(increment_[d], d).c_str());
    // Trip-count.
    EXPECT_STREQ("((100) (TC-loop) ((0) < (100)))",
                 GetInductionInfo(loop_header_[d]->GetLastInstruction(), d).c_str());
  }
}

}  // namespace art
