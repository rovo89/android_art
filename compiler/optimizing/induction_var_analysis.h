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
 * Induction variable analysis.
 *
 * Based on the paper by M. Gerlek et al.
 * "Beyond Induction Variables: Detecting and Classifying Sequences Using a Demand-Driven SSA Form"
 * (ACM Transactions on Programming Languages and Systems, Volume 17 Issue 1, Jan. 1995).
 */
class HInductionVarAnalysis : public HOptimization {
 public:
  explicit HInductionVarAnalysis(HGraph* graph);

  // TODO: design public API useful in later phases

  /**
   * Returns string representation of induction found for the instruction
   * in the given loop (for testing and debugging only).
   */
  std::string InductionToString(HLoopInformation* loop, HInstruction* instruction) {
    return InductionToString(LookupInfo(loop, instruction));
  }

  void Run() OVERRIDE;

 private:
  static constexpr const char* kInductionPassName = "induction_var_analysis";

  struct NodeInfo {
    explicit NodeInfo(uint32_t d) : depth(d), done(false) {}
    uint32_t depth;
    bool done;
  };

  enum InductionClass {
    kNone,
    kInvariant,
    kLinear,
    kWrapAround,
    kPeriodic,
    kMonotonic
  };

  enum InductionOp {
    kNop,  // no-operation: a true induction
    kAdd,
    kSub,
    kNeg,
    kMul,
    kDiv,
    kFetch
  };

  /**
   * Defines a detected induction as:
   *   (1) invariant:
   *         operation: a + b, a - b, -b, a * b, a / b
   *       or
   *         fetch: fetch from HIR
   *   (2) linear:
   *         nop: a * i + b
   *   (3) wrap-around
   *         nop: a, then defined by b
   *   (4) periodic
   *         nop: a, then defined by b (repeated when exhausted)
   *   (5) monotonic
   *         // TODO: determine representation
   */
  struct InductionInfo : public ArenaObject<kArenaAllocMisc> {
    InductionInfo(InductionClass ic,
                  InductionOp op,
                  InductionInfo* a,
                  InductionInfo* b,
                  HInstruction* f)
        : induction_class(ic),
          operation(op),
          op_a(a),
          op_b(b),
          fetch(f) {}
    InductionClass induction_class;
    InductionOp operation;
    InductionInfo* op_a;
    InductionInfo* op_b;
    HInstruction* fetch;
  };

  inline bool IsVisitedNode(int id) const {
    return map_.find(id) != map_.end();
  }

  inline InductionInfo* NewInductionInfo(
      InductionClass c,
      InductionOp op,
      InductionInfo* a,
      InductionInfo* b,
      HInstruction* i) {
    return new (graph_->GetArena()) InductionInfo(c, op, a, b, i);
  }

  // Methods for analysis.
  void VisitLoop(HLoopInformation* loop);
  void VisitNode(HLoopInformation* loop, HInstruction* instruction);
  uint32_t VisitDescendant(HLoopInformation* loop, HInstruction* instruction);
  void ClassifyTrivial(HLoopInformation* loop, HInstruction* instruction);
  void ClassifyNonTrivial(HLoopInformation* loop);

  // Transfer operations.
  InductionInfo* TransferPhi(InductionInfo* a, InductionInfo* b);
  InductionInfo* TransferAddSub(InductionInfo* a, InductionInfo* b, InductionOp op);
  InductionInfo* TransferMul(InductionInfo* a, InductionInfo* b);
  InductionInfo* TransferNeg(InductionInfo* a);
  InductionInfo* TransferCycleOverPhi(HInstruction* phi);
  InductionInfo* TransferCycleOverAddSub(HLoopInformation* loop,
                                         HInstruction* x,
                                         HInstruction* y,
                                         InductionOp op,
                                         bool first);

  // Assign and lookup.
  void PutInfo(int loop_id, int id, InductionInfo* info);
  InductionInfo* GetInfo(int loop_id, int id);
  void AssignInfo(HLoopInformation* loop, HInstruction* instruction, InductionInfo* info);
  InductionInfo* LookupInfo(HLoopInformation* loop, HInstruction* instruction);
  bool InductionEqual(InductionInfo* info1, InductionInfo* info2);
  std::string InductionToString(InductionInfo* info);

  // Bookkeeping during and after analysis.
  // TODO: fine tune data structures, only keep relevant data

  uint32_t global_depth_;

  ArenaVector<HInstruction*> stack_;
  ArenaVector<HInstruction*> scc_;

  // Mappings of instruction id to node and induction information.
  ArenaSafeMap<int, NodeInfo> map_;
  ArenaSafeMap<int, InductionInfo*> cycle_;

  // Mapping from loop id to mapping of instruction id to induction information.
  ArenaSafeMap<int, ArenaSafeMap<int, InductionInfo*>> induction_;

  DISALLOW_COPY_AND_ASSIGN(HInductionVarAnalysis);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_INDUCTION_VAR_ANALYSIS_H_
