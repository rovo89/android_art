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

#include "base/arena_allocator.h"
#include "builder.h"
#include "induction_var_analysis.h"
#include "induction_var_range.h"
#include "nodes.h"
#include "optimizing_unit_test.h"

namespace art {

using Value = InductionVarRange::Value;

/**
 * Fixture class for the InductionVarRange tests.
 */
class InductionVarRangeTest : public CommonCompilerTest {
 public:
  InductionVarRangeTest()
      : pool_(),
        allocator_(&pool_),
        graph_(CreateGraph(&allocator_)),
        iva_(new (&allocator_) HInductionVarAnalysis(graph_)),
        range_(iva_) {
    BuildGraph();
  }

  ~InductionVarRangeTest() { }

  void ExpectEqual(Value v1, Value v2) {
    EXPECT_EQ(v1.instruction, v2.instruction);
    EXPECT_EQ(v1.a_constant, v2.a_constant);
    EXPECT_EQ(v1.b_constant, v2.b_constant);
    EXPECT_EQ(v1.is_known, v2.is_known);
  }

  //
  // Construction methods.
  //

  /** Constructs bare minimum graph. */
  void BuildGraph() {
    graph_->SetNumberOfVRegs(1);
    entry_block_ = new (&allocator_) HBasicBlock(graph_);
    exit_block_ = new (&allocator_) HBasicBlock(graph_);
    graph_->AddBlock(entry_block_);
    graph_->AddBlock(exit_block_);
    graph_->SetEntryBlock(entry_block_);
    graph_->SetExitBlock(exit_block_);
    // Two parameters.
    x_ = new (&allocator_) HParameterValue(graph_->GetDexFile(), 0, 0, Primitive::kPrimInt);
    entry_block_->AddInstruction(x_);
    y_ = new (&allocator_) HParameterValue(graph_->GetDexFile(), 0, 0, Primitive::kPrimInt);
    entry_block_->AddInstruction(y_);
  }

  /** Constructs loop with given upper bound. */
  void BuildLoop(int32_t lower, HInstruction* upper, int32_t stride) {
    // Control flow.
    loop_preheader_ = new (&allocator_) HBasicBlock(graph_);
    graph_->AddBlock(loop_preheader_);
    HBasicBlock* loop_header = new (&allocator_) HBasicBlock(graph_);
    graph_->AddBlock(loop_header);
    HBasicBlock* loop_body = new (&allocator_) HBasicBlock(graph_);
    graph_->AddBlock(loop_body);
    HBasicBlock* return_block = new (&allocator_) HBasicBlock(graph_);
    graph_->AddBlock(return_block);
    entry_block_->AddSuccessor(loop_preheader_);
    loop_preheader_->AddSuccessor(loop_header);
    loop_header->AddSuccessor(loop_body);
    loop_header->AddSuccessor(return_block);
    loop_body->AddSuccessor(loop_header);
    return_block->AddSuccessor(exit_block_);
    // Instructions.
    loop_preheader_->AddInstruction(new (&allocator_) HGoto());
    HPhi* phi = new (&allocator_) HPhi(&allocator_, 0, 0, Primitive::kPrimInt);
    loop_header->AddPhi(phi);
    phi->AddInput(graph_->GetIntConstant(lower));  // i = l
    if (stride > 0) {
      condition_ = new (&allocator_) HLessThan(phi, upper);  // i < u
    } else {
      condition_ = new (&allocator_) HGreaterThan(phi, upper);  // i > u
    }
    loop_header->AddInstruction(condition_);
    loop_header->AddInstruction(new (&allocator_) HIf(condition_));
    increment_ = new (&allocator_) HAdd(Primitive::kPrimInt, phi, graph_->GetIntConstant(stride));
    loop_body->AddInstruction(increment_);  // i += s
    phi->AddInput(increment_);
    loop_body->AddInstruction(new (&allocator_) HGoto());
    return_block->AddInstruction(new (&allocator_) HReturnVoid());
    exit_block_->AddInstruction(new (&allocator_) HExit());
  }

  /** Constructs SSA and performs induction variable analysis. */
  void PerformInductionVarAnalysis() {
    graph_->BuildDominatorTree();
    iva_->Run();
  }

  /** Constructs an invariant. */
  HInductionVarAnalysis::InductionInfo* CreateInvariant(char opc,
                                                        HInductionVarAnalysis::InductionInfo* a,
                                                        HInductionVarAnalysis::InductionInfo* b) {
    HInductionVarAnalysis::InductionOp op;
    switch (opc) {
      case '+': op = HInductionVarAnalysis::kAdd; break;
      case '-': op = HInductionVarAnalysis::kSub; break;
      case 'n': op = HInductionVarAnalysis::kNeg; break;
      case '*': op = HInductionVarAnalysis::kMul; break;
      case '/': op = HInductionVarAnalysis::kDiv; break;
      default:  op = HInductionVarAnalysis::kNop; break;
    }
    return iva_->CreateInvariantOp(op, a, b);
  }

  /** Constructs a fetch. */
  HInductionVarAnalysis::InductionInfo* CreateFetch(HInstruction* fetch) {
    return iva_->CreateInvariantFetch(fetch);
  }

  /** Constructs a constant. */
  HInductionVarAnalysis::InductionInfo* CreateConst(int32_t c) {
    return CreateFetch(graph_->GetIntConstant(c));
  }

  /** Constructs a trip-count. */
  HInductionVarAnalysis::InductionInfo* CreateTripCount(int32_t tc, bool in_loop, bool safe) {
    Primitive::Type type = Primitive::kPrimInt;
    if (in_loop && safe) {
      return iva_->CreateTripCount(
          HInductionVarAnalysis::kTripCountInLoop, CreateConst(tc), nullptr, type);
    } else if (in_loop) {
      return iva_->CreateTripCount(
          HInductionVarAnalysis::kTripCountInLoopUnsafe, CreateConst(tc), nullptr, type);
    } else if (safe) {
      return iva_->CreateTripCount(
          HInductionVarAnalysis::kTripCountInBody, CreateConst(tc), nullptr, type);
    } else {
      return iva_->CreateTripCount(
          HInductionVarAnalysis::kTripCountInBodyUnsafe, CreateConst(tc), nullptr, type);
    }
  }

  /** Constructs a linear a * i + b induction. */
  HInductionVarAnalysis::InductionInfo* CreateLinear(int32_t a, int32_t b) {
    return iva_->CreateInduction(
        HInductionVarAnalysis::kLinear, CreateConst(a), CreateConst(b), Primitive::kPrimInt);
  }

  /** Constructs a range [lo, hi] using a periodic induction. */
  HInductionVarAnalysis::InductionInfo* CreateRange(int32_t lo, int32_t hi) {
    return iva_->CreateInduction(
        HInductionVarAnalysis::kPeriodic, CreateConst(lo), CreateConst(hi), Primitive::kPrimInt);
  }

  /** Constructs a wrap-around induction consisting of a constant, followed info */
  HInductionVarAnalysis::InductionInfo* CreateWrapAround(
      int32_t initial,
      HInductionVarAnalysis::InductionInfo* info) {
    return iva_->CreateInduction(
        HInductionVarAnalysis::kWrapAround, CreateConst(initial), info, Primitive::kPrimInt);
  }

  /** Constructs a wrap-around induction consisting of a constant, followed by a range. */
  HInductionVarAnalysis::InductionInfo* CreateWrapAround(int32_t initial, int32_t lo, int32_t hi) {
    return CreateWrapAround(initial, CreateRange(lo, hi));
  }

  //
  // Relay methods.
  //

  bool NeedsTripCount(HInductionVarAnalysis::InductionInfo* info) {
    return range_.NeedsTripCount(info);
  }

  bool IsBodyTripCount(HInductionVarAnalysis::InductionInfo* trip) {
    return range_.IsBodyTripCount(trip);
  }

  bool IsUnsafeTripCount(HInductionVarAnalysis::InductionInfo* trip) {
    return range_.IsUnsafeTripCount(trip);
  }

  Value GetMin(HInductionVarAnalysis::InductionInfo* info,
               HInductionVarAnalysis::InductionInfo* induc) {
    return range_.GetVal(info, induc, /* in_body */ true, /* is_min */ true);
  }

  Value GetMax(HInductionVarAnalysis::InductionInfo* info,
               HInductionVarAnalysis::InductionInfo* induc) {
    return range_.GetVal(info, induc, /* in_body */ true, /* is_min */ false);
  }

  Value GetMul(HInductionVarAnalysis::InductionInfo* info1,
               HInductionVarAnalysis::InductionInfo* info2,
               bool is_min) {
    return range_.GetMul(info1, info2, nullptr, /* in_body */ true, is_min);
  }

  Value GetDiv(HInductionVarAnalysis::InductionInfo* info1,
               HInductionVarAnalysis::InductionInfo* info2,
               bool is_min) {
    return range_.GetDiv(info1, info2, nullptr, /* in_body */ true, is_min);
  }

  bool IsExact(HInductionVarAnalysis::InductionInfo* info, int64_t* value) {
    return range_.IsConstant(info, InductionVarRange::kExact, value);
  }

  bool IsAtMost(HInductionVarAnalysis::InductionInfo* info, int64_t* value) {
    return range_.IsConstant(info, InductionVarRange::kAtMost, value);
  }

  bool IsAtLeast(HInductionVarAnalysis::InductionInfo* info, int64_t* value) {
    return range_.IsConstant(info, InductionVarRange::kAtLeast, value);
  }

  Value AddValue(Value v1, Value v2) { return range_.AddValue(v1, v2); }
  Value SubValue(Value v1, Value v2) { return range_.SubValue(v1, v2); }
  Value MulValue(Value v1, Value v2) { return range_.MulValue(v1, v2); }
  Value DivValue(Value v1, Value v2) { return range_.DivValue(v1, v2); }
  Value MinValue(Value v1, Value v2) { return range_.MergeVal(v1, v2, true); }
  Value MaxValue(Value v1, Value v2) { return range_.MergeVal(v1, v2, false); }

  // General building fields.
  ArenaPool pool_;
  ArenaAllocator allocator_;
  HGraph* graph_;
  HBasicBlock* entry_block_;
  HBasicBlock* exit_block_;
  HBasicBlock* loop_preheader_;
  HInductionVarAnalysis* iva_;
  InductionVarRange range_;

  // Instructions.
  HInstruction* condition_;
  HInstruction* increment_;
  HInstruction* x_;
  HInstruction* y_;
};

//
// Tests on private methods.
//

TEST_F(InductionVarRangeTest, IsConstant) {
  int64_t value;
  // Constant.
  EXPECT_TRUE(IsExact(CreateConst(12345), &value));
  EXPECT_EQ(12345, value);
  EXPECT_TRUE(IsAtMost(CreateConst(12345), &value));
  EXPECT_EQ(12345, value);
  EXPECT_TRUE(IsAtLeast(CreateConst(12345), &value));
  EXPECT_EQ(12345, value);
  // Constant trivial range.
  EXPECT_TRUE(IsExact(CreateRange(111, 111), &value));
  EXPECT_EQ(111, value);
  EXPECT_TRUE(IsAtMost(CreateRange(111, 111), &value));
  EXPECT_EQ(111, value);
  EXPECT_TRUE(IsAtLeast(CreateRange(111, 111), &value));
  EXPECT_EQ(111, value);
  // Constant non-trivial range.
  EXPECT_FALSE(IsExact(CreateRange(11, 22), &value));
  EXPECT_TRUE(IsAtMost(CreateRange(11, 22), &value));
  EXPECT_EQ(22, value);
  EXPECT_TRUE(IsAtLeast(CreateRange(11, 22), &value));
  EXPECT_EQ(11, value);
  // Symbolic.
  EXPECT_FALSE(IsExact(CreateFetch(x_), &value));
  EXPECT_FALSE(IsAtMost(CreateFetch(x_), &value));
  EXPECT_FALSE(IsAtLeast(CreateFetch(x_), &value));
}

TEST_F(InductionVarRangeTest, TripCountProperties) {
  EXPECT_FALSE(NeedsTripCount(nullptr));
  EXPECT_FALSE(NeedsTripCount(CreateConst(1)));
  EXPECT_TRUE(NeedsTripCount(CreateLinear(1, 1)));
  EXPECT_FALSE(NeedsTripCount(CreateWrapAround(1, 2, 3)));
  EXPECT_TRUE(NeedsTripCount(CreateWrapAround(1, CreateLinear(1, 1))));

  EXPECT_FALSE(IsBodyTripCount(nullptr));
  EXPECT_FALSE(IsBodyTripCount(CreateTripCount(100, true, true)));
  EXPECT_FALSE(IsBodyTripCount(CreateTripCount(100, true, false)));
  EXPECT_TRUE(IsBodyTripCount(CreateTripCount(100, false, true)));
  EXPECT_TRUE(IsBodyTripCount(CreateTripCount(100, false, false)));

  EXPECT_FALSE(IsUnsafeTripCount(nullptr));
  EXPECT_FALSE(IsUnsafeTripCount(CreateTripCount(100, true, true)));
  EXPECT_TRUE(IsUnsafeTripCount(CreateTripCount(100, true, false)));
  EXPECT_FALSE(IsUnsafeTripCount(CreateTripCount(100, false, true)));
  EXPECT_TRUE(IsUnsafeTripCount(CreateTripCount(100, false, false)));
}

TEST_F(InductionVarRangeTest, GetMinMaxNull) {
  ExpectEqual(Value(), GetMin(nullptr, nullptr));
  ExpectEqual(Value(), GetMax(nullptr, nullptr));
}

TEST_F(InductionVarRangeTest, GetMinMaxAdd) {
  ExpectEqual(Value(12),
              GetMin(CreateInvariant('+', CreateConst(2), CreateRange(10, 20)), nullptr));
  ExpectEqual(Value(22),
              GetMax(CreateInvariant('+', CreateConst(2), CreateRange(10, 20)), nullptr));
  ExpectEqual(Value(x_, 1, -20),
              GetMin(CreateInvariant('+', CreateFetch(x_), CreateRange(-20, -10)), nullptr));
  ExpectEqual(Value(x_, 1, -10),
              GetMax(CreateInvariant('+', CreateFetch(x_), CreateRange(-20, -10)), nullptr));
  ExpectEqual(Value(x_, 1, 10),
              GetMin(CreateInvariant('+', CreateRange(10, 20), CreateFetch(x_)), nullptr));
  ExpectEqual(Value(x_, 1, 20),
              GetMax(CreateInvariant('+', CreateRange(10, 20), CreateFetch(x_)), nullptr));
  ExpectEqual(Value(5),
              GetMin(CreateInvariant('+', CreateRange(-5, -1), CreateRange(10, 20)), nullptr));
  ExpectEqual(Value(19),
              GetMax(CreateInvariant('+', CreateRange(-5, -1), CreateRange(10, 20)), nullptr));
}

TEST_F(InductionVarRangeTest, GetMinMaxSub) {
  ExpectEqual(Value(-18),
              GetMin(CreateInvariant('-', CreateConst(2), CreateRange(10, 20)), nullptr));
  ExpectEqual(Value(-8),
              GetMax(CreateInvariant('-', CreateConst(2), CreateRange(10, 20)), nullptr));
  ExpectEqual(Value(x_, 1, 10),
              GetMin(CreateInvariant('-', CreateFetch(x_), CreateRange(-20, -10)), nullptr));
  ExpectEqual(Value(x_, 1, 20),
              GetMax(CreateInvariant('-', CreateFetch(x_), CreateRange(-20, -10)), nullptr));
  ExpectEqual(Value(x_, -1, 10),
              GetMin(CreateInvariant('-', CreateRange(10, 20), CreateFetch(x_)), nullptr));
  ExpectEqual(Value(x_, -1, 20),
              GetMax(CreateInvariant('-', CreateRange(10, 20), CreateFetch(x_)), nullptr));
  ExpectEqual(Value(-25),
              GetMin(CreateInvariant('-', CreateRange(-5, -1), CreateRange(10, 20)), nullptr));
  ExpectEqual(Value(-11),
              GetMax(CreateInvariant('-', CreateRange(-5, -1), CreateRange(10, 20)), nullptr));
}

TEST_F(InductionVarRangeTest, GetMinMaxNeg) {
  ExpectEqual(Value(-20), GetMin(CreateInvariant('n', nullptr, CreateRange(10, 20)), nullptr));
  ExpectEqual(Value(-10), GetMax(CreateInvariant('n', nullptr, CreateRange(10, 20)), nullptr));
  ExpectEqual(Value(10), GetMin(CreateInvariant('n', nullptr, CreateRange(-20, -10)), nullptr));
  ExpectEqual(Value(20), GetMax(CreateInvariant('n', nullptr, CreateRange(-20, -10)), nullptr));
  ExpectEqual(Value(x_, -1, 0), GetMin(CreateInvariant('n', nullptr, CreateFetch(x_)), nullptr));
  ExpectEqual(Value(x_, -1, 0), GetMax(CreateInvariant('n', nullptr, CreateFetch(x_)), nullptr));
}

TEST_F(InductionVarRangeTest, GetMinMaxMul) {
  ExpectEqual(Value(20),
              GetMin(CreateInvariant('*', CreateConst(2), CreateRange(10, 20)), nullptr));
  ExpectEqual(Value(40),
              GetMax(CreateInvariant('*', CreateConst(2), CreateRange(10, 20)), nullptr));
}

TEST_F(InductionVarRangeTest, GetMinMaxDiv) {
  ExpectEqual(Value(3),
              GetMin(CreateInvariant('/', CreateRange(12, 20), CreateConst(4)), nullptr));
  ExpectEqual(Value(5),
              GetMax(CreateInvariant('/', CreateRange(12, 20), CreateConst(4)), nullptr));
}

TEST_F(InductionVarRangeTest, GetMinMaxConstant) {
  ExpectEqual(Value(12345), GetMin(CreateConst(12345), nullptr));
  ExpectEqual(Value(12345), GetMax(CreateConst(12345), nullptr));
}

TEST_F(InductionVarRangeTest, GetMinMaxFetch) {
  ExpectEqual(Value(x_, 1, 0), GetMin(CreateFetch(x_), nullptr));
  ExpectEqual(Value(x_, 1, 0), GetMax(CreateFetch(x_), nullptr));
}

TEST_F(InductionVarRangeTest, GetMinMaxLinear) {
  ExpectEqual(Value(20), GetMin(CreateLinear(10, 20), CreateTripCount(100, true, true)));
  ExpectEqual(Value(1010), GetMax(CreateLinear(10, 20), CreateTripCount(100, true, true)));
  ExpectEqual(Value(-970), GetMin(CreateLinear(-10, 20), CreateTripCount(100, true, true)));
  ExpectEqual(Value(20), GetMax(CreateLinear(-10, 20), CreateTripCount(100, true, true)));
}

TEST_F(InductionVarRangeTest, GetMinMaxWrapAround) {
  ExpectEqual(Value(-5), GetMin(CreateWrapAround(-5, -1, 10), nullptr));
  ExpectEqual(Value(10), GetMax(CreateWrapAround(-5, -1, 10), nullptr));
  ExpectEqual(Value(-1), GetMin(CreateWrapAround(2, -1, 10), nullptr));
  ExpectEqual(Value(10), GetMax(CreateWrapAround(2, -1, 10), nullptr));
  ExpectEqual(Value(-1), GetMin(CreateWrapAround(20, -1, 10), nullptr));
  ExpectEqual(Value(20), GetMax(CreateWrapAround(20, -1, 10), nullptr));
}

TEST_F(InductionVarRangeTest, GetMinMaxPeriodic) {
  ExpectEqual(Value(-2), GetMin(CreateRange(-2, 99), nullptr));
  ExpectEqual(Value(99), GetMax(CreateRange(-2, 99), nullptr));
}

TEST_F(InductionVarRangeTest, GetMulMin) {
  ExpectEqual(Value(-14), GetMul(CreateConst(2), CreateRange(-7, 8), true));
  ExpectEqual(Value(-16), GetMul(CreateConst(-2), CreateRange(-7, 8), true));
  ExpectEqual(Value(-14), GetMul(CreateRange(-7, 8), CreateConst(2), true));
  ExpectEqual(Value(-16), GetMul(CreateRange(-7, 8), CreateConst(-2), true));
  ExpectEqual(Value(6), GetMul(CreateRange(2, 10), CreateRange(3, 5), true));
  ExpectEqual(Value(-50), GetMul(CreateRange(2, 10), CreateRange(-5, -3), true));
  ExpectEqual(Value(), GetMul(CreateRange(2, 10), CreateRange(-1, 1), true));
  ExpectEqual(Value(-50), GetMul(CreateRange(-10, -2), CreateRange(3, 5), true));
  ExpectEqual(Value(6), GetMul(CreateRange(-10, -2), CreateRange(-5, -3), true));
  ExpectEqual(Value(), GetMul(CreateRange(-10, -2), CreateRange(-1, 1), true));
  ExpectEqual(Value(), GetMul(CreateRange(-1, 1), CreateRange(2, 10), true));
  ExpectEqual(Value(), GetMul(CreateRange(-1, 1), CreateRange(-10, -2), true));
  ExpectEqual(Value(), GetMul(CreateRange(-1, 1), CreateRange(-1, 1), true));
}

TEST_F(InductionVarRangeTest, GetMulMax) {
  ExpectEqual(Value(16), GetMul(CreateConst(2), CreateRange(-7, 8), false));
  ExpectEqual(Value(14), GetMul(CreateConst(-2), CreateRange(-7, 8), false));
  ExpectEqual(Value(16), GetMul(CreateRange(-7, 8), CreateConst(2), false));
  ExpectEqual(Value(14), GetMul(CreateRange(-7, 8), CreateConst(-2), false));
  ExpectEqual(Value(50), GetMul(CreateRange(2, 10), CreateRange(3, 5), false));
  ExpectEqual(Value(-6), GetMul(CreateRange(2, 10), CreateRange(-5, -3), false));
  ExpectEqual(Value(), GetMul(CreateRange(2, 10), CreateRange(-1, 1), false));
  ExpectEqual(Value(-6), GetMul(CreateRange(-10, -2), CreateRange(3, 5), false));
  ExpectEqual(Value(50), GetMul(CreateRange(-10, -2), CreateRange(-5, -3), false));
  ExpectEqual(Value(), GetMul(CreateRange(-10, -2), CreateRange(-1, 1), false));
  ExpectEqual(Value(), GetMul(CreateRange(-1, 1), CreateRange(2, 10), false));
  ExpectEqual(Value(), GetMul(CreateRange(-1, 1), CreateRange(-10, -2), false));
  ExpectEqual(Value(), GetMul(CreateRange(-1, 1), CreateRange(-1, 1), false));
}

TEST_F(InductionVarRangeTest, GetDivMin) {
  ExpectEqual(Value(-5), GetDiv(CreateRange(-10, 20), CreateConst(2), true));
  ExpectEqual(Value(-10), GetDiv(CreateRange(-10, 20), CreateConst(-2), true));
  ExpectEqual(Value(10), GetDiv(CreateRange(40, 1000), CreateRange(2, 4), true));
  ExpectEqual(Value(-500), GetDiv(CreateRange(40, 1000), CreateRange(-4, -2), true));
  ExpectEqual(Value(), GetDiv(CreateRange(40, 1000), CreateRange(-1, 1), true));
  ExpectEqual(Value(-500), GetDiv(CreateRange(-1000, -40), CreateRange(2, 4), true));
  ExpectEqual(Value(10), GetDiv(CreateRange(-1000, -40), CreateRange(-4, -2), true));
  ExpectEqual(Value(), GetDiv(CreateRange(-1000, -40), CreateRange(-1, 1), true));
  ExpectEqual(Value(), GetDiv(CreateRange(-1, 1), CreateRange(40, 1000), true));
  ExpectEqual(Value(), GetDiv(CreateRange(-1, 1), CreateRange(-1000, -40), true));
  ExpectEqual(Value(), GetDiv(CreateRange(-1, 1), CreateRange(-1, 1), true));
}

TEST_F(InductionVarRangeTest, GetDivMax) {
  ExpectEqual(Value(10), GetDiv(CreateRange(-10, 20), CreateConst(2), false));
  ExpectEqual(Value(5), GetDiv(CreateRange(-10, 20), CreateConst(-2), false));
  ExpectEqual(Value(500), GetDiv(CreateRange(40, 1000), CreateRange(2, 4), false));
  ExpectEqual(Value(-10), GetDiv(CreateRange(40, 1000), CreateRange(-4, -2), false));
  ExpectEqual(Value(), GetDiv(CreateRange(40, 1000), CreateRange(-1, 1), false));
  ExpectEqual(Value(-10), GetDiv(CreateRange(-1000, -40), CreateRange(2, 4), false));
  ExpectEqual(Value(500), GetDiv(CreateRange(-1000, -40), CreateRange(-4, -2), false));
  ExpectEqual(Value(), GetDiv(CreateRange(-1000, -40), CreateRange(-1, 1), false));
  ExpectEqual(Value(), GetDiv(CreateRange(-1, 1), CreateRange(40, 1000), false));
  ExpectEqual(Value(), GetDiv(CreateRange(-1, 1), CreateRange(-1000, 40), false));
  ExpectEqual(Value(), GetDiv(CreateRange(-1, 1), CreateRange(-1, 1), false));
}

TEST_F(InductionVarRangeTest, AddValue) {
  ExpectEqual(Value(110), AddValue(Value(10), Value(100)));
  ExpectEqual(Value(-5), AddValue(Value(x_, 1, -4), Value(x_, -1, -1)));
  ExpectEqual(Value(x_, 3, -5), AddValue(Value(x_, 2, -4), Value(x_, 1, -1)));
  ExpectEqual(Value(), AddValue(Value(x_, 1, 5), Value(y_, 1, -7)));
  ExpectEqual(Value(x_, 1, 23), AddValue(Value(x_, 1, 20), Value(3)));
  ExpectEqual(Value(y_, 1, 5), AddValue(Value(55), Value(y_, 1, -50)));
  const int32_t max_value = std::numeric_limits<int32_t>::max();
  ExpectEqual(Value(max_value), AddValue(Value(max_value - 5), Value(5)));
  ExpectEqual(Value(), AddValue(Value(max_value - 5), Value(6)));  // unsafe
}

TEST_F(InductionVarRangeTest, SubValue) {
  ExpectEqual(Value(-90), SubValue(Value(10), Value(100)));
  ExpectEqual(Value(-3), SubValue(Value(x_, 1, -4), Value(x_, 1, -1)));
  ExpectEqual(Value(x_, 2, -3), SubValue(Value(x_, 3, -4), Value(x_, 1, -1)));
  ExpectEqual(Value(), SubValue(Value(x_, 1, 5), Value(y_, 1, -7)));
  ExpectEqual(Value(x_, 1, 17), SubValue(Value(x_, 1, 20), Value(3)));
  ExpectEqual(Value(y_, -4, 105), SubValue(Value(55), Value(y_, 4, -50)));
  const int32_t min_value = std::numeric_limits<int32_t>::min();
  ExpectEqual(Value(min_value), SubValue(Value(min_value + 5), Value(5)));
  ExpectEqual(Value(), SubValue(Value(min_value + 5), Value(6)));  // unsafe
}

TEST_F(InductionVarRangeTest, MulValue) {
  ExpectEqual(Value(1000), MulValue(Value(10), Value(100)));
  ExpectEqual(Value(), MulValue(Value(x_, 1, -4), Value(x_, 1, -1)));
  ExpectEqual(Value(), MulValue(Value(x_, 1, 5), Value(y_, 1, -7)));
  ExpectEqual(Value(x_, 9, 60), MulValue(Value(x_, 3, 20), Value(3)));
  ExpectEqual(Value(y_, 55, -110), MulValue(Value(55), Value(y_, 1, -2)));
  ExpectEqual(Value(), MulValue(Value(90000), Value(-90000)));  // unsafe
}

TEST_F(InductionVarRangeTest, MulValueSpecial) {
  const int32_t min_value = std::numeric_limits<int32_t>::min();
  const int32_t max_value = std::numeric_limits<int32_t>::max();

  // Unsafe.
  ExpectEqual(Value(), MulValue(Value(min_value), Value(min_value)));
  ExpectEqual(Value(), MulValue(Value(min_value), Value(-1)));
  ExpectEqual(Value(), MulValue(Value(min_value), Value(max_value)));
  ExpectEqual(Value(), MulValue(Value(max_value), Value(max_value)));

  // Safe.
  ExpectEqual(Value(min_value), MulValue(Value(min_value), Value(1)));
  ExpectEqual(Value(max_value), MulValue(Value(max_value), Value(1)));
  ExpectEqual(Value(-max_value), MulValue(Value(max_value), Value(-1)));
  ExpectEqual(Value(-1), MulValue(Value(1), Value(-1)));
  ExpectEqual(Value(1), MulValue(Value(-1), Value(-1)));
}

TEST_F(InductionVarRangeTest, DivValue) {
  ExpectEqual(Value(25), DivValue(Value(100), Value(4)));
  ExpectEqual(Value(), DivValue(Value(x_, 1, -4), Value(x_, 1, -1)));
  ExpectEqual(Value(), DivValue(Value(x_, 1, 5), Value(y_, 1, -7)));
  ExpectEqual(Value(), DivValue(Value(x_, 12, 24), Value(3)));
  ExpectEqual(Value(), DivValue(Value(55), Value(y_, 1, -50)));
  ExpectEqual(Value(), DivValue(Value(1), Value(0)));  // unsafe
}

TEST_F(InductionVarRangeTest, DivValueSpecial) {
  const int32_t min_value = std::numeric_limits<int32_t>::min();
  const int32_t max_value = std::numeric_limits<int32_t>::max();

  // Unsafe.
  ExpectEqual(Value(), DivValue(Value(min_value), Value(-1)));

  // Safe.
  ExpectEqual(Value(1), DivValue(Value(min_value), Value(min_value)));
  ExpectEqual(Value(1), DivValue(Value(max_value), Value(max_value)));
  ExpectEqual(Value(min_value), DivValue(Value(min_value), Value(1)));
  ExpectEqual(Value(max_value), DivValue(Value(max_value), Value(1)));
  ExpectEqual(Value(-max_value), DivValue(Value(max_value), Value(-1)));
  ExpectEqual(Value(-1), DivValue(Value(1), Value(-1)));
  ExpectEqual(Value(1), DivValue(Value(-1), Value(-1)));
}

TEST_F(InductionVarRangeTest, MinValue) {
  ExpectEqual(Value(10), MinValue(Value(10), Value(100)));
  ExpectEqual(Value(x_, 1, -4), MinValue(Value(x_, 1, -4), Value(x_, 1, -1)));
  ExpectEqual(Value(x_, 4, -4), MinValue(Value(x_, 4, -4), Value(x_, 4, -1)));
  ExpectEqual(Value(), MinValue(Value(x_, 1, 5), Value(y_, 1, -7)));
  ExpectEqual(Value(), MinValue(Value(x_, 1, 20), Value(3)));
  ExpectEqual(Value(), MinValue(Value(55), Value(y_, 1, -50)));
}

TEST_F(InductionVarRangeTest, MaxValue) {
  ExpectEqual(Value(100), MaxValue(Value(10), Value(100)));
  ExpectEqual(Value(x_, 1, -1), MaxValue(Value(x_, 1, -4), Value(x_, 1, -1)));
  ExpectEqual(Value(x_, 4, -1), MaxValue(Value(x_, 4, -4), Value(x_, 4, -1)));
  ExpectEqual(Value(), MaxValue(Value(x_, 1, 5), Value(y_, 1, -7)));
  ExpectEqual(Value(), MaxValue(Value(x_, 1, 20), Value(3)));
  ExpectEqual(Value(), MaxValue(Value(55), Value(y_, 1, -50)));
}

//
// Tests on public methods.
//

TEST_F(InductionVarRangeTest, ConstantTripCountUp) {
  BuildLoop(0, graph_->GetIntConstant(1000), 1);
  PerformInductionVarAnalysis();

  Value v1, v2;
  bool needs_finite_test = true;

  // In context of header: known.
  range_.GetInductionRange(condition_, condition_->InputAt(0), &v1, &v2, &needs_finite_test);
  EXPECT_FALSE(needs_finite_test);
  ExpectEqual(Value(0), v1);
  ExpectEqual(Value(1000), v2);
  EXPECT_FALSE(range_.RefineOuter(&v1, &v2));

  // In context of loop-body: known.
  range_.GetInductionRange(increment_, condition_->InputAt(0), &v1, &v2, &needs_finite_test);
  EXPECT_FALSE(needs_finite_test);
  ExpectEqual(Value(0), v1);
  ExpectEqual(Value(999), v2);
  EXPECT_FALSE(range_.RefineOuter(&v1, &v2));
  range_.GetInductionRange(increment_, increment_, &v1, &v2, &needs_finite_test);
  EXPECT_FALSE(needs_finite_test);
  ExpectEqual(Value(1), v1);
  ExpectEqual(Value(1000), v2);
  EXPECT_FALSE(range_.RefineOuter(&v1, &v2));
}

TEST_F(InductionVarRangeTest, ConstantTripCountDown) {
  BuildLoop(1000, graph_->GetIntConstant(0), -1);
  PerformInductionVarAnalysis();

  Value v1, v2;
  bool needs_finite_test = true;

  // In context of header: known.
  range_.GetInductionRange(condition_, condition_->InputAt(0), &v1, &v2, &needs_finite_test);
  EXPECT_FALSE(needs_finite_test);
  ExpectEqual(Value(0), v1);
  ExpectEqual(Value(1000), v2);
  EXPECT_FALSE(range_.RefineOuter(&v1, &v2));

  // In context of loop-body: known.
  range_.GetInductionRange(increment_, condition_->InputAt(0), &v1, &v2, &needs_finite_test);
  EXPECT_FALSE(needs_finite_test);
  ExpectEqual(Value(1), v1);
  ExpectEqual(Value(1000), v2);
  EXPECT_FALSE(range_.RefineOuter(&v1, &v2));
  range_.GetInductionRange(increment_, increment_, &v1, &v2, &needs_finite_test);
  EXPECT_FALSE(needs_finite_test);
  ExpectEqual(Value(0), v1);
  ExpectEqual(Value(999), v2);
  EXPECT_FALSE(range_.RefineOuter(&v1, &v2));
}

TEST_F(InductionVarRangeTest, SymbolicTripCountUp) {
  BuildLoop(0, x_, 1);
  PerformInductionVarAnalysis();

  Value v1, v2;
  bool needs_finite_test = true;
  bool needs_taken_test = true;

  // In context of header: upper unknown.
  range_.GetInductionRange(condition_, condition_->InputAt(0), &v1, &v2, &needs_finite_test);
  EXPECT_FALSE(needs_finite_test);
  ExpectEqual(Value(0), v1);
  ExpectEqual(Value(), v2);
  EXPECT_FALSE(range_.RefineOuter(&v1, &v2));

  // In context of loop-body: known.
  range_.GetInductionRange(increment_, condition_->InputAt(0), &v1, &v2, &needs_finite_test);
  EXPECT_FALSE(needs_finite_test);
  ExpectEqual(Value(0), v1);
  ExpectEqual(Value(x_, 1, -1), v2);
  EXPECT_FALSE(range_.RefineOuter(&v1, &v2));
  range_.GetInductionRange(increment_, increment_, &v1, &v2, &needs_finite_test);
  EXPECT_FALSE(needs_finite_test);
  ExpectEqual(Value(1), v1);
  ExpectEqual(Value(x_, 1, 0), v2);
  EXPECT_FALSE(range_.RefineOuter(&v1, &v2));

  HInstruction* lower = nullptr;
  HInstruction* upper = nullptr;
  HInstruction* taken = nullptr;

  // Can generate code in context of loop-body only.
  EXPECT_FALSE(range_.CanGenerateCode(
      condition_, condition_->InputAt(0), &needs_finite_test, &needs_taken_test));
  ASSERT_TRUE(range_.CanGenerateCode(
      increment_, condition_->InputAt(0), &needs_finite_test, &needs_taken_test));
  EXPECT_FALSE(needs_finite_test);
  EXPECT_TRUE(needs_taken_test);

  // Generates code.
  range_.GenerateRangeCode(
      increment_, condition_->InputAt(0), graph_, loop_preheader_, &lower, &upper);

  // Verify lower is 0+0.
  ASSERT_TRUE(lower != nullptr);
  ASSERT_TRUE(lower->IsAdd());
  ASSERT_TRUE(lower->InputAt(0)->IsIntConstant());
  EXPECT_EQ(0, lower->InputAt(0)->AsIntConstant()->GetValue());
  ASSERT_TRUE(lower->InputAt(1)->IsIntConstant());
  EXPECT_EQ(0, lower->InputAt(1)->AsIntConstant()->GetValue());

  // Verify upper is (V-1)+0.
  ASSERT_TRUE(upper != nullptr);
  ASSERT_TRUE(upper->IsAdd());
  ASSERT_TRUE(upper->InputAt(0)->IsSub());
  EXPECT_TRUE(upper->InputAt(0)->InputAt(0)->IsParameterValue());
  ASSERT_TRUE(upper->InputAt(0)->InputAt(1)->IsIntConstant());
  EXPECT_EQ(1, upper->InputAt(0)->InputAt(1)->AsIntConstant()->GetValue());
  ASSERT_TRUE(upper->InputAt(1)->IsIntConstant());
  EXPECT_EQ(0, upper->InputAt(1)->AsIntConstant()->GetValue());

  // Verify taken-test is 0<V.
  range_.GenerateTakenTest(increment_, graph_, loop_preheader_, &taken);
  ASSERT_TRUE(taken != nullptr);
  ASSERT_TRUE(taken->IsLessThan());
  ASSERT_TRUE(taken->InputAt(0)->IsIntConstant());
  EXPECT_EQ(0, taken->InputAt(0)->AsIntConstant()->GetValue());
  EXPECT_TRUE(taken->InputAt(1)->IsParameterValue());
}

TEST_F(InductionVarRangeTest, SymbolicTripCountDown) {
  BuildLoop(1000, x_, -1);
  PerformInductionVarAnalysis();

  Value v1, v2;
  bool needs_finite_test = true;
  bool needs_taken_test = true;

  // In context of header: lower unknown.
  range_.GetInductionRange(condition_, condition_->InputAt(0), &v1, &v2, &needs_finite_test);
  EXPECT_FALSE(needs_finite_test);
  ExpectEqual(Value(), v1);
  ExpectEqual(Value(1000), v2);
  EXPECT_FALSE(range_.RefineOuter(&v1, &v2));

  // In context of loop-body: known.
  range_.GetInductionRange(increment_, condition_->InputAt(0), &v1, &v2, &needs_finite_test);
  EXPECT_FALSE(needs_finite_test);
  ExpectEqual(Value(x_, 1, 1), v1);
  ExpectEqual(Value(1000), v2);
  EXPECT_FALSE(range_.RefineOuter(&v1, &v2));
  range_.GetInductionRange(increment_, increment_, &v1, &v2, &needs_finite_test);
  EXPECT_FALSE(needs_finite_test);
  ExpectEqual(Value(x_, 1, 0), v1);
  ExpectEqual(Value(999), v2);
  EXPECT_FALSE(range_.RefineOuter(&v1, &v2));

  HInstruction* lower = nullptr;
  HInstruction* upper = nullptr;
  HInstruction* taken = nullptr;

  // Can generate code in context of loop-body only.
  EXPECT_FALSE(range_.CanGenerateCode(
      condition_, condition_->InputAt(0), &needs_finite_test, &needs_taken_test));
  ASSERT_TRUE(range_.CanGenerateCode(
      increment_, condition_->InputAt(0), &needs_finite_test, &needs_taken_test));
  EXPECT_FALSE(needs_finite_test);
  EXPECT_TRUE(needs_taken_test);

  // Generates code.
  range_.GenerateRangeCode(
      increment_, condition_->InputAt(0), graph_, loop_preheader_, &lower, &upper);

  // Verify lower is 1000-((1000-V)-1).
  ASSERT_TRUE(lower != nullptr);
  ASSERT_TRUE(lower->IsSub());
  ASSERT_TRUE(lower->InputAt(0)->IsIntConstant());
  EXPECT_EQ(1000, lower->InputAt(0)->AsIntConstant()->GetValue());
  lower = lower->InputAt(1);
  ASSERT_TRUE(lower->IsSub());
  ASSERT_TRUE(lower->InputAt(1)->IsIntConstant());
  EXPECT_EQ(1, lower->InputAt(1)->AsIntConstant()->GetValue());
  lower = lower->InputAt(0);
  ASSERT_TRUE(lower->IsSub());
  ASSERT_TRUE(lower->InputAt(0)->IsIntConstant());
  EXPECT_EQ(1000, lower->InputAt(0)->AsIntConstant()->GetValue());
  EXPECT_TRUE(lower->InputAt(1)->IsParameterValue());

  // Verify upper is 1000-0.
  ASSERT_TRUE(upper != nullptr);
  ASSERT_TRUE(upper->IsSub());
  ASSERT_TRUE(upper->InputAt(0)->IsIntConstant());
  EXPECT_EQ(1000, upper->InputAt(0)->AsIntConstant()->GetValue());
  ASSERT_TRUE(upper->InputAt(1)->IsIntConstant());
  EXPECT_EQ(0, upper->InputAt(1)->AsIntConstant()->GetValue());

  // Verify taken-test is 1000>V.
  range_.GenerateTakenTest(increment_, graph_, loop_preheader_, &taken);
  ASSERT_TRUE(taken != nullptr);
  ASSERT_TRUE(taken->IsGreaterThan());
  ASSERT_TRUE(taken->InputAt(0)->IsIntConstant());
  EXPECT_EQ(1000, taken->InputAt(0)->AsIntConstant()->GetValue());
  EXPECT_TRUE(taken->InputAt(1)->IsParameterValue());
}

}  // namespace art
