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
    float_constant0_ = graph_->GetFloatConstant(0.0f);
    return_->AddInstruction(new (&allocator_) HReturnVoid());
    exit_->AddInstruction(new (&allocator_) HExit());

    // Provide loop instructions.
    for (int d = 0; d < n; d++) {
      basic_[d] = new (&allocator_) HPhi(&allocator_, d, 0, Primitive::kPrimInt);
      loop_preheader_[d]->AddInstruction(new (&allocator_) HGoto());
      loop_header_[d]->AddPhi(basic_[d]);
      HInstruction* compare = new (&allocator_) HLessThan(basic_[d], constant100_);
      loop_header_[d]->AddInstruction(compare);
      loop_header_[d]->AddInstruction(new (&allocator_) HIf(compare));
      increment_[d] = new (&allocator_) HAdd(Primitive::kPrimInt, basic_[d], constant1_);
      loop_body_[d]->AddInstruction(increment_[d]);
      loop_body_[d]->AddInstruction(new (&allocator_) HGoto());

      basic_[d]->AddInput(constant0_);
      basic_[d]->AddInput(increment_[d]);
    }
  }

  // Builds if-statement at depth d.
  HPhi* BuildIf(int d, HBasicBlock** ifT, HBasicBlock **ifF) {
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

    HPhi* select_phi = new (&allocator_) HPhi(&allocator_, -1, 0, Primitive::kPrimInt);
    loop_body_[d]->AddPhi(select_phi);
    return select_phi;
  }

  // Inserts instruction right before increment at depth d.
  HInstruction* InsertInstruction(HInstruction* instruction, int d) {
    loop_body_[d]->InsertInstructionBefore(instruction, increment_[d]);
    return instruction;
  }

  // Inserts a phi to loop header at depth d and returns it.
  HPhi* InsertLoopPhi(int vreg, int d) {
    HPhi* phi = new (&allocator_) HPhi(&allocator_, vreg, 0, Primitive::kPrimInt);
    loop_header_[d]->AddPhi(phi);
    return phi;
  }

  // Inserts an array store with given `subscript` at depth d to
  // enable tests to inspect the computed induction at that point easily.
  HInstruction* InsertArrayStore(HInstruction* subscript, int d) {
    // ArraySet is given a float value in order to avoid SsaBuilder typing
    // it from the array's non-existent reference type info.
    return InsertInstruction(new (&allocator_) HArraySet(
        parameter_, subscript, float_constant0_, Primitive::kPrimFloat, 0), d);
  }

  // Returns induction information of instruction in loop at depth d.
  std::string GetInductionInfo(HInstruction* instruction, int d) {
    return HInductionVarAnalysis::InductionToString(
        iva_->LookupInfo(loop_body_[d]->GetLoopInformation(), instruction));
  }

  // Returns true if instructions have identical induction.
  bool HaveSameInduction(HInstruction* instruction1, HInstruction* instruction2) {
    return HInductionVarAnalysis::InductionEqual(
      iva_->LookupInfo(loop_body_[0]->GetLoopInformation(), instruction1),
      iva_->LookupInfo(loop_body_[0]->GetLoopInformation(), instruction2));
  }

  // Performs InductionVarAnalysis (after proper set up).
  void PerformInductionVarAnalysis() {
    graph_->BuildDominatorTree();
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
  HInstruction* float_constant0_;

  // Loop specifics.
  HBasicBlock* loop_preheader_[10];
  HBasicBlock* loop_header_[10];
  HBasicBlock* loop_body_[10];
  HInstruction* increment_[10];
  HPhi* basic_[10];  // "vreg_d", the "i_d"
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
  graph_->BuildDominatorTree();

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

  EXPECT_STREQ("((1) * i + (0)):PrimInt", GetInductionInfo(store->InputAt(1), 0).c_str());
  EXPECT_STREQ("((1) * i + (1)):PrimInt", GetInductionInfo(increment_[0], 0).c_str());

  // Offset matters!
  EXPECT_FALSE(HaveSameInduction(store->InputAt(1), increment_[0]));

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
      new (&allocator_) HAdd(Primitive::kPrimInt, constant100_, basic_[0]), 0);
  HInstruction *sub = InsertInstruction(
      new (&allocator_) HSub(Primitive::kPrimInt, constant100_, basic_[0]), 0);
  HInstruction *mul = InsertInstruction(
      new (&allocator_) HMul(Primitive::kPrimInt, constant100_, basic_[0]), 0);
  HInstruction *shl = InsertInstruction(
      new (&allocator_) HShl(Primitive::kPrimInt, basic_[0], constant1_), 0);
  HInstruction *neg = InsertInstruction(
      new (&allocator_) HNeg(Primitive::kPrimInt, basic_[0]), 0);
  PerformInductionVarAnalysis();

  EXPECT_STREQ("((1) * i + (100)):PrimInt", GetInductionInfo(add, 0).c_str());
  EXPECT_STREQ("(( - (1)) * i + (100)):PrimInt", GetInductionInfo(sub, 0).c_str());
  EXPECT_STREQ("((100) * i + (0)):PrimInt", GetInductionInfo(mul, 0).c_str());
  EXPECT_STREQ("((2) * i + (0)):PrimInt", GetInductionInfo(shl, 0).c_str());
  EXPECT_STREQ("(( - (1)) * i + (0)):PrimInt", GetInductionInfo(neg, 0).c_str());
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
  HPhi* k = InsertLoopPhi(0, 0);
  k->AddInput(constant0_);

  HInstruction *add = InsertInstruction(
      new (&allocator_) HAdd(Primitive::kPrimInt, k, constant100_), 0);
  HInstruction* store1 = InsertArrayStore(add, 0);
  HInstruction *sub = InsertInstruction(
      new (&allocator_) HSub(Primitive::kPrimInt, add, constant1_), 0);
  HInstruction* store2 = InsertArrayStore(sub, 0);
  k->AddInput(sub);
  PerformInductionVarAnalysis();

  EXPECT_STREQ("(((100) - (1)) * i + (100)):PrimInt",
               GetInductionInfo(store1->InputAt(1), 0).c_str());
  EXPECT_STREQ("(((100) - (1)) * i + ((100) - (1))):PrimInt",
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
  HPhi* k_header = InsertLoopPhi(0, 0);
  k_header->AddInput(constant0_);

  HBasicBlock* ifTrue;
  HBasicBlock* ifFalse;
  HPhi* k_body = BuildIf(0, &ifTrue, &ifFalse);

  // True-branch.
  HInstruction* inc1 = new (&allocator_) HAdd(Primitive::kPrimInt, k_header, constant1_);
  ifTrue->AddInstruction(inc1);
  k_body->AddInput(inc1);
  // False-branch.
  HInstruction* inc2 = new (&allocator_) HAdd(Primitive::kPrimInt, k_header, constant1_);
  ifFalse->AddInstruction(inc2);
  k_body->AddInput(inc2);
  // Merge over a phi.
  HInstruction* store = InsertArrayStore(k_body, 0);
  k_header->AddInput(k_body);
  PerformInductionVarAnalysis();

  EXPECT_STREQ("((1) * i + (1)):PrimInt", GetInductionInfo(store->InputAt(1), 0).c_str());

  // Both increments get same induction.
  EXPECT_TRUE(HaveSameInduction(store->InputAt(1), inc1));
  EXPECT_TRUE(HaveSameInduction(store->InputAt(1), inc2));
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
  HPhi* k = BuildIf(0, &ifTrue, &ifFalse);

  // True-branch.
  HInstruction* inc1 = new (&allocator_) HAdd(Primitive::kPrimInt, basic_[0], constant1_);
  ifTrue->AddInstruction(inc1);
  k->AddInput(inc1);
  // False-branch.
  HInstruction* inc2 = new (&allocator_) HAdd(Primitive::kPrimInt, basic_[0], constant1_);
  ifFalse->AddInstruction(inc2);
  k->AddInput(inc2);
  // Merge over a phi.
  HInstruction* store = InsertArrayStore(k, 0);
  PerformInductionVarAnalysis();

  EXPECT_STREQ("((1) * i + (1)):PrimInt", GetInductionInfo(store->InputAt(1), 0).c_str());
}

TEST_F(InductionVarAnalysisTest, FindFirstOrderWrapAroundInduction) {
  // Setup:
  // k = 0;
  // for (int i = 0; i < 100; i++) {
  //   a[k] = 0;
  //   k = 100 - i;
  // }
  BuildLoopNest(1);
  HPhi* k = InsertLoopPhi(0, 0);
  k->AddInput(constant0_);

  HInstruction* store = InsertArrayStore(k, 0);
  HInstruction *sub = InsertInstruction(
      new (&allocator_) HSub(Primitive::kPrimInt, constant100_, basic_[0]), 0);
  k->AddInput(sub);
  PerformInductionVarAnalysis();

  EXPECT_STREQ("wrap((0), (( - (1)) * i + (100)):PrimInt):PrimInt",
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
  HPhi* k = InsertLoopPhi(0, 0);
  k->AddInput(constant0_);
  HPhi* t = InsertLoopPhi(1, 0);
  t->AddInput(constant100_);

  HInstruction* store = InsertArrayStore(k, 0);
  k->AddInput(t);
  HInstruction *sub = InsertInstruction(
      new (&allocator_) HSub(Primitive::kPrimInt, constant100_, basic_[0], 0), 0);
  t->AddInput(sub);
  PerformInductionVarAnalysis();

  EXPECT_STREQ("wrap((0), wrap((100), (( - (1)) * i + (100)):PrimInt):PrimInt):PrimInt",
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
  HPhi* k = InsertLoopPhi(0, 0);
  k->AddInput(constant0_);

  HInstruction *add = InsertInstruction(
      new (&allocator_) HAdd(Primitive::kPrimInt, k, constant100_), 0);
  HInstruction *sub = InsertInstruction(
      new (&allocator_) HSub(Primitive::kPrimInt, k, constant100_), 0);
  HInstruction *mul = InsertInstruction(
      new (&allocator_) HMul(Primitive::kPrimInt, k, constant100_), 0);
  HInstruction *shl = InsertInstruction(
      new (&allocator_) HShl(Primitive::kPrimInt, k, constant1_), 0);
  HInstruction *neg = InsertInstruction(
      new (&allocator_) HNeg(Primitive::kPrimInt, k), 0);
  k->AddInput(
      InsertInstruction(new (&allocator_) HShl(Primitive::kPrimInt, basic_[0], constant1_), 0));
  PerformInductionVarAnalysis();

  EXPECT_STREQ("wrap((100), ((2) * i + (100)):PrimInt):PrimInt",
               GetInductionInfo(add, 0).c_str());
  EXPECT_STREQ("wrap(((0) - (100)), ((2) * i + ((0) - (100))):PrimInt):PrimInt",
               GetInductionInfo(sub, 0).c_str());
  EXPECT_STREQ("wrap((0), (((2) * (100)) * i + (0)):PrimInt):PrimInt",
               GetInductionInfo(mul, 0).c_str());
  EXPECT_STREQ("wrap((0), (((2) * (2)) * i + (0)):PrimInt):PrimInt",
               GetInductionInfo(shl, 0).c_str());
  EXPECT_STREQ("wrap((0), (( - (2)) * i + (0)):PrimInt):PrimInt",
               GetInductionInfo(neg, 0).c_str());
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
  HPhi* k = InsertLoopPhi(0, 0);
  k->AddInput(constant0_);
  HPhi* t = InsertLoopPhi(1, 0);
  t->AddInput(constant100_);

  HInstruction* store1 = InsertArrayStore(k, 0);
  HInstruction* store2 = InsertArrayStore(t, 0);
  k->AddInput(t);
  t->AddInput(k);
  PerformInductionVarAnalysis();

  EXPECT_STREQ("periodic((0), (100)):PrimInt", GetInductionInfo(store1->InputAt(1), 0).c_str());
  EXPECT_STREQ("periodic((100), (0)):PrimInt", GetInductionInfo(store2->InputAt(1), 0).c_str());
}

TEST_F(InductionVarAnalysisTest, FindIdiomaticPeriodicInduction) {
  // Setup:
  // k = 0;
  // for (int i = 0; i < 100; i++) {
  //   a[k] = 0;
  //   k = 1 - k;
  // }
  BuildLoopNest(1);
  HPhi* k = InsertLoopPhi(0, 0);
  k->AddInput(constant0_);

  HInstruction* store = InsertArrayStore(k, 0);
  HInstruction *sub = InsertInstruction(
      new (&allocator_) HSub(Primitive::kPrimInt, constant1_, k), 0);
  k->AddInput(sub);
  PerformInductionVarAnalysis();

  EXPECT_STREQ("periodic((0), (1)):PrimInt", GetInductionInfo(store->InputAt(1), 0).c_str());
  EXPECT_STREQ("periodic((1), (0)):PrimInt", GetInductionInfo(sub, 0).c_str());
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
  HPhi* k_header = InsertLoopPhi(0, 0);
  k_header->AddInput(constant0_);

  HInstruction* k_body = InsertInstruction(
      new (&allocator_) HSub(Primitive::kPrimInt, constant1_, k_header), 0);
  k_header->AddInput(k_body);

  // Derived expressions.
  HInstruction *add = InsertInstruction(
      new (&allocator_) HAdd(Primitive::kPrimInt, k_body, constant100_), 0);
  HInstruction *sub = InsertInstruction(
      new (&allocator_) HSub(Primitive::kPrimInt, k_body, constant100_), 0);
  HInstruction *mul = InsertInstruction(
      new (&allocator_) HMul(Primitive::kPrimInt, k_body, constant100_), 0);
  HInstruction *shl = InsertInstruction(
      new (&allocator_) HShl(Primitive::kPrimInt, k_body, constant1_), 0);
  HInstruction *neg = InsertInstruction(
      new (&allocator_) HNeg(Primitive::kPrimInt, k_body), 0);
  PerformInductionVarAnalysis();

  EXPECT_STREQ("periodic(((1) + (100)), (100)):PrimInt", GetInductionInfo(add, 0).c_str());
  EXPECT_STREQ("periodic(((1) - (100)), ((0) - (100))):PrimInt", GetInductionInfo(sub, 0).c_str());
  EXPECT_STREQ("periodic((100), (0)):PrimInt", GetInductionInfo(mul, 0).c_str());
  EXPECT_STREQ("periodic((2), (0)):PrimInt", GetInductionInfo(shl, 0).c_str());
  EXPECT_STREQ("periodic(( - (1)), (0)):PrimInt", GetInductionInfo(neg, 0).c_str());
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

  HPhi* k[10];
  for (int d = 0; d < 10; d++) {
    k[d] = InsertLoopPhi(0, d);
  }

  HInstruction *inc = InsertInstruction(
      new (&allocator_) HAdd(Primitive::kPrimInt, constant1_, k[9]), 9);
  HInstruction* store = InsertArrayStore(inc, 9);

  for (int d = 0; d < 10; d++) {
    k[d]->AddInput((d != 0) ? k[d - 1] : constant0_);
    k[d]->AddInput((d != 9) ? k[d + 1] : inc);
  }
  PerformInductionVarAnalysis();

  // Avoid exact phi number, since that depends on the SSA building phase.
  std::regex r("\\(\\(1\\) \\* i \\+ "
               "\\(\\(1\\) \\+ \\(\\d+:Phi\\)\\)\\):PrimInt");

  for (int d = 0; d < 10; d++) {
    if (d == 9) {
      EXPECT_TRUE(std::regex_match(GetInductionInfo(store->InputAt(1), d), r));
    } else {
      EXPECT_STREQ("", GetInductionInfo(store->InputAt(1), d).c_str());
    }
    EXPECT_STREQ("((1) * i + (1)):PrimInt", GetInductionInfo(increment_[d], d).c_str());
    // Trip-count.
    EXPECT_STREQ("((100) (TC-loop) ((0) < (100)))",
                 GetInductionInfo(loop_header_[d]->GetLastInstruction(), d).c_str());
  }
}

TEST_F(InductionVarAnalysisTest, ByteInductionIntLoopControl) {
  // Setup:
  // for (int i = 0; i < 100; i++) {
  //   k = (byte) i;
  //   a[k] = 0;
  //   a[i] = 0;
  // }
  BuildLoopNest(1);
  HInstruction *conv = InsertInstruction(
      new (&allocator_) HTypeConversion(Primitive::kPrimByte, basic_[0], -1), 0);
  HInstruction* store1 = InsertArrayStore(conv, 0);
  HInstruction* store2 = InsertArrayStore(basic_[0], 0);
  PerformInductionVarAnalysis();

  // Regular int induction (i) is "transferred" over conversion into byte induction (k).
  EXPECT_STREQ("((1) * i + (0)):PrimByte", GetInductionInfo(store1->InputAt(1), 0).c_str());
  EXPECT_STREQ("((1) * i + (0)):PrimInt",  GetInductionInfo(store2->InputAt(1), 0).c_str());
  EXPECT_STREQ("((1) * i + (1)):PrimInt",  GetInductionInfo(increment_[0], 0).c_str());

  // Type matters!
  EXPECT_FALSE(HaveSameInduction(store1->InputAt(1), store2->InputAt(1)));

  // Trip-count.
  EXPECT_STREQ("((100) (TC-loop) ((0) < (100)))",
               GetInductionInfo(loop_header_[0]->GetLastInstruction(), 0).c_str());
}

TEST_F(InductionVarAnalysisTest, ByteLoopControl1) {
  // Setup:
  // for (byte i = -128; i < 127; i++) {  // just fits!
  // }
  BuildLoopNest(1);
  basic_[0]->ReplaceInput(graph_->GetIntConstant(-128), 0);
  HInstruction* ifs = loop_header_[0]->GetLastInstruction()->GetPrevious();
  ifs->ReplaceInput(graph_->GetIntConstant(127), 1);
  HInstruction* conv = new(&allocator_) HTypeConversion(Primitive::kPrimByte, increment_[0], -1);
  loop_body_[0]->InsertInstructionBefore(conv, increment_[0]->GetNext());
  basic_[0]->ReplaceInput(conv, 1);
  PerformInductionVarAnalysis();

  EXPECT_STREQ("((1) * i + ((-128) + (1))):PrimByte", GetInductionInfo(increment_[0], 0).c_str());
  // Trip-count.
  EXPECT_STREQ("(((127) - (-128)) (TC-loop) ((-128) < (127)))",
               GetInductionInfo(loop_header_[0]->GetLastInstruction(), 0).c_str());
}

TEST_F(InductionVarAnalysisTest, ByteLoopControl2) {
  // Setup:
  // for (byte i = -128; i < 128; i++) {  // infinite loop!
  // }
  BuildLoopNest(1);
  basic_[0]->ReplaceInput(graph_->GetIntConstant(-128), 0);
  HInstruction* ifs = loop_header_[0]->GetLastInstruction()->GetPrevious();
  ifs->ReplaceInput(graph_->GetIntConstant(128), 1);
  HInstruction* conv = new(&allocator_) HTypeConversion(Primitive::kPrimByte, increment_[0], -1);
  loop_body_[0]->InsertInstructionBefore(conv, increment_[0]->GetNext());
  basic_[0]->ReplaceInput(conv, 1);
  PerformInductionVarAnalysis();

  EXPECT_STREQ("((1) * i + ((-128) + (1))):PrimByte", GetInductionInfo(increment_[0], 0).c_str());
  // Trip-count undefined.
  EXPECT_STREQ("", GetInductionInfo(loop_header_[0]->GetLastInstruction(), 0).c_str());
}

TEST_F(InductionVarAnalysisTest, ShortLoopControl1) {
  // Setup:
  // for (short i = -32768; i < 32767; i++) {  // just fits!
  // }
  BuildLoopNest(1);
  basic_[0]->ReplaceInput(graph_->GetIntConstant(-32768), 0);
  HInstruction* ifs = loop_header_[0]->GetLastInstruction()->GetPrevious();
  ifs->ReplaceInput(graph_->GetIntConstant(32767), 1);
  HInstruction* conv = new(&allocator_) HTypeConversion(Primitive::kPrimShort, increment_[0], -1);
  loop_body_[0]->InsertInstructionBefore(conv, increment_[0]->GetNext());
  basic_[0]->ReplaceInput(conv, 1);
  PerformInductionVarAnalysis();

  EXPECT_STREQ("((1) * i + ((-32768) + (1))):PrimShort",
               GetInductionInfo(increment_[0], 0).c_str());
  // Trip-count.
  EXPECT_STREQ("(((32767) - (-32768)) (TC-loop) ((-32768) < (32767)))",
               GetInductionInfo(loop_header_[0]->GetLastInstruction(), 0).c_str());
}

TEST_F(InductionVarAnalysisTest, ShortLoopControl2) {
  // Setup:
  // for (short i = -32768; i < 32768; i++) {  // infinite loop!
  // }
  BuildLoopNest(1);
  basic_[0]->ReplaceInput(graph_->GetIntConstant(-32768), 0);
  HInstruction* ifs = loop_header_[0]->GetLastInstruction()->GetPrevious();
  ifs->ReplaceInput(graph_->GetIntConstant(32768), 1);
  HInstruction* conv = new(&allocator_) HTypeConversion(Primitive::kPrimShort, increment_[0], -1);
  loop_body_[0]->InsertInstructionBefore(conv, increment_[0]->GetNext());
  basic_[0]->ReplaceInput(conv, 1);
  PerformInductionVarAnalysis();

  EXPECT_STREQ("((1) * i + ((-32768) + (1))):PrimShort",
               GetInductionInfo(increment_[0], 0).c_str());
  // Trip-count undefined.
  EXPECT_STREQ("", GetInductionInfo(loop_header_[0]->GetLastInstruction(), 0).c_str());
}

TEST_F(InductionVarAnalysisTest, CharLoopControl1) {
  // Setup:
  // for (char i = 0; i < 65535; i++) {  // just fits!
  // }
  BuildLoopNest(1);
  HInstruction* ifs = loop_header_[0]->GetLastInstruction()->GetPrevious();
  ifs->ReplaceInput(graph_->GetIntConstant(65535), 1);
  HInstruction* conv = new(&allocator_) HTypeConversion(Primitive::kPrimChar, increment_[0], -1);
  loop_body_[0]->InsertInstructionBefore(conv, increment_[0]->GetNext());
  basic_[0]->ReplaceInput(conv, 1);
  PerformInductionVarAnalysis();

  EXPECT_STREQ("((1) * i + (1)):PrimChar", GetInductionInfo(increment_[0], 0).c_str());
  // Trip-count.
  EXPECT_STREQ("((65535) (TC-loop) ((0) < (65535)))",
               GetInductionInfo(loop_header_[0]->GetLastInstruction(), 0).c_str());
}

TEST_F(InductionVarAnalysisTest, CharLoopControl2) {
  // Setup:
  // for (char i = 0; i < 65536; i++) {  // infinite loop!
  // }
  BuildLoopNest(1);
  HInstruction* ifs = loop_header_[0]->GetLastInstruction()->GetPrevious();
  ifs->ReplaceInput(graph_->GetIntConstant(65536), 1);
  HInstruction* conv = new(&allocator_) HTypeConversion(Primitive::kPrimChar, increment_[0], -1);
  loop_body_[0]->InsertInstructionBefore(conv, increment_[0]->GetNext());
  basic_[0]->ReplaceInput(conv, 1);
  PerformInductionVarAnalysis();

  EXPECT_STREQ("((1) * i + (1)):PrimChar", GetInductionInfo(increment_[0], 0).c_str());
  // Trip-count undefined.
  EXPECT_STREQ("", GetInductionInfo(loop_header_[0]->GetLastInstruction(), 0).c_str());
}

}  // namespace art
