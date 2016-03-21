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

#ifndef ART_COMPILER_OPTIMIZING_INDUCTION_VAR_ANALYSIS_H_
#define ART_COMPILER_OPTIMIZING_INDUCTION_VAR_ANALYSIS_H_

#include <string>

#include "nodes.h"
#include "optimization.h"

namespace art {

/**
 * Induction variable analysis. This class does not have a direct public API.
 * Instead, the results of induction variable analysis can be queried through
 * friend classes, such as InductionVarRange.
 *
 * The analysis implementation is based on the paper by M. Gerlek et al.
 * "Beyond Induction Variables: Detecting and Classifying Sequences Using a Demand-Driven SSA Form"
 * (ACM Transactions on Programming Languages and Systems, Volume 17 Issue 1, Jan. 1995).
 */
class HInductionVarAnalysis : public HOptimization {
 public:
  explicit HInductionVarAnalysis(HGraph* graph);

  void Run() OVERRIDE;

 private:
  static constexpr const char* kInductionPassName = "induction_var_analysis";

  struct NodeInfo {
    explicit NodeInfo(uint32_t d) : depth(d), done(false) {}
    uint32_t depth;
    bool done;
  };

  enum InductionClass {
    kInvariant,
    kLinear,
    kWrapAround,
    kPeriodic
  };

  enum InductionOp {
    // No-operation: a true induction.
    kNop,
    // Various invariant operations.
    kAdd,
    kSub,
    kNeg,
    kMul,
    kDiv,
    kFetch,
    // Trip-counts.
    kTripCountInLoop,        // valid in full loop; loop is finite
    kTripCountInBody,        // valid in body only; loop is finite
    kTripCountInLoopUnsafe,  // valid in full loop; loop may be infinite
    kTripCountInBodyUnsafe,  // valid in body only; loop may be infinite
    // Comparisons for trip-count tests.
    kLT,
    kLE,
    kGT,
    kGE
  };

  /**
   * Defines a detected induction as:
   *   (1) invariant:
   *         operation: a + b, a - b, -b, a * b, a / b
   *       or:
   *         fetch: fetch from HIR
   *   (2) linear:
   *         nop: a * i + b
   *   (3) wrap-around
   *         nop: a, then defined by b
   *   (4) periodic
   *         nop: a, then defined by b (repeated when exhausted)
   *   (5) trip-count:
   *         tc: defined by a, taken-test in b
   */
  struct InductionInfo : public ArenaObject<kArenaAllocInductionVarAnalysis> {
    InductionInfo(InductionClass ic,
                  InductionOp op,
                  InductionInfo* a,
                  InductionInfo* b,
                  HInstruction* f,
                  Primitive::Type t)
        : induction_class(ic),
          operation(op),
          op_a(a),
          op_b(b),
          fetch(f),
          type(t) {}
    InductionClass induction_class;
    InductionOp operation;
    InductionInfo* op_a;
    InductionInfo* op_b;
    HInstruction* fetch;
    Primitive::Type type;  // precision of induction
  };

  bool IsVisitedNode(HInstruction* instruction) const {
    return map_.find(instruction) != map_.end();
  }

  InductionInfo* CreateInvariantOp(InductionOp op, InductionInfo* a, InductionInfo* b) {
    DCHECK(((op != kNeg && a != nullptr) || (op == kNeg && a == nullptr)) && b != nullptr);
    return CreateSimplifiedInvariant(op, a, b);
  }

  InductionInfo* CreateInvariantFetch(HInstruction* f) {
    DCHECK(f != nullptr);
    return new (graph_->GetArena())
        InductionInfo(kInvariant, kFetch, nullptr, nullptr, f, f->GetType());
  }

  InductionInfo* CreateTripCount(InductionOp op,
                                 InductionInfo* a,
                                 InductionInfo* b,
                                 Primitive::Type type) {
    DCHECK(a != nullptr);
    return new (graph_->GetArena()) InductionInfo(kInvariant, op, a, b, nullptr, type);
  }

  InductionInfo* CreateInduction(InductionClass ic,
                                 InductionInfo* a,
                                 InductionInfo* b,
                                 Primitive::Type type) {
    DCHECK(a != nullptr && b != nullptr);
    return new (graph_->GetArena()) InductionInfo(ic, kNop, a, b, nullptr, type);
  }

  // Methods for analysis.
  void VisitLoop(HLoopInformation* loop);
  void VisitNode(HLoopInformation* loop, HInstruction* instruction);
  uint32_t VisitDescendant(HLoopInformation* loop, HInstruction* instruction);
  void ClassifyTrivial(HLoopInformation* loop, HInstruction* instruction);
  void ClassifyNonTrivial(HLoopInformation* loop);
  InductionInfo* RotatePeriodicInduction(InductionInfo* induction, InductionInfo* last);

  // Transfer operations.
  InductionInfo* TransferPhi(HLoopInformation* loop, HInstruction* phi, size_t input_index);
  InductionInfo* TransferAddSub(InductionInfo* a, InductionInfo* b, InductionOp op);
  InductionInfo* TransferMul(InductionInfo* a, InductionInfo* b);
  InductionInfo* TransferShl(InductionInfo* a, InductionInfo* b, Primitive::Type type);
  InductionInfo* TransferNeg(InductionInfo* a);
  InductionInfo* TransferCnv(InductionInfo* a, Primitive::Type from, Primitive::Type to);

  // Solvers.
  InductionInfo* SolvePhi(HInstruction* phi, size_t input_index);
  InductionInfo* SolvePhiAllInputs(HLoopInformation* loop,
                                   HInstruction* entry_phi,
                                   HInstruction* phi);
  InductionInfo* SolveAddSub(HLoopInformation* loop,
                             HInstruction* entry_phi,
                             HInstruction* instruction,
                             HInstruction* x,
                             HInstruction* y,
                             InductionOp op,
                             bool is_first_call);
  InductionInfo* SolveCnv(HTypeConversion* conversion);

  // Trip count information.
  void VisitControl(HLoopInformation* loop);
  void VisitCondition(HLoopInformation* loop,
                      InductionInfo* a,
                      InductionInfo* b,
                      Primitive::Type type,
                      IfCondition cmp);
  void VisitTripCount(HLoopInformation* loop,
                      InductionInfo* lower_expr,
                      InductionInfo* upper_expr,
                      InductionInfo* stride,
                      int64_t stride_value,
                      Primitive::Type type,
                      IfCondition cmp);
  bool IsTaken(InductionInfo* lower_expr, InductionInfo* upper_expr, IfCondition cmp);
  bool IsFinite(InductionInfo* upper_expr,
                int64_t stride_value,
                Primitive::Type type,
                IfCondition cmp);
  bool FitsNarrowerControl(InductionInfo* lower_expr,
                           InductionInfo* upper_expr,
                           int64_t stride_value,
                           Primitive::Type type,
                           IfCondition cmp);

  // Assign and lookup.
  void AssignInfo(HLoopInformation* loop, HInstruction* instruction, InductionInfo* info);
  InductionInfo* LookupInfo(HLoopInformation* loop, HInstruction* instruction);
  InductionInfo* CreateConstant(int64_t value, Primitive::Type type);
  InductionInfo* CreateSimplifiedInvariant(InductionOp op, InductionInfo* a, InductionInfo* b);

  // Constants.
  bool IsExact(InductionInfo* info, /*out*/ int64_t* value);
  bool IsAtMost(InductionInfo* info, /*out*/ int64_t* value);
  bool IsAtLeast(InductionInfo* info, /*out*/ int64_t* value);

  // Helpers.
  static bool InductionEqual(InductionInfo* info1, InductionInfo* info2);
  static std::string InductionToString(InductionInfo* info);

  // TODO: fine tune the following data structures, only keep relevant data.

  // Temporary book-keeping during the analysis.
  uint32_t global_depth_;
  ArenaVector<HInstruction*> stack_;
  ArenaVector<HInstruction*> scc_;
  ArenaSafeMap<HInstruction*, NodeInfo> map_;
  ArenaSafeMap<HInstruction*, InductionInfo*> cycle_;
  Primitive::Type type_;

  /**
   * Maintains the results of the analysis as a mapping from loops to a mapping from instructions
   * to the induction information for that instruction in that loop.
   */
  ArenaSafeMap<HLoopInformation*, ArenaSafeMap<HInstruction*, InductionInfo*>> induction_;

  friend class InductionVarAnalysisTest;
  friend class InductionVarRange;
  friend class InductionVarRangeTest;

  DISALLOW_COPY_AND_ASSIGN(HInductionVarAnalysis);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_INDUCTION_VAR_ANALYSIS_H_
