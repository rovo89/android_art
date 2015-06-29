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

#ifndef ART_COMPILER_OPTIMIZING_GRAPH_CHECKER_H_
#define ART_COMPILER_OPTIMIZING_GRAPH_CHECKER_H_

#include "nodes.h"

#include <ostream>

namespace art {

// A control-flow graph visitor performing various checks.
class GraphChecker : public HGraphDelegateVisitor {
 public:
  GraphChecker(ArenaAllocator* allocator, HGraph* graph,
               const char* dump_prefix = "art::GraphChecker: ")
    : HGraphDelegateVisitor(graph),
      allocator_(allocator),
      dump_prefix_(dump_prefix),
      seen_ids_(allocator, graph->GetCurrentInstructionId(), false) {}

  // Check the whole graph (in insertion order).
  virtual void Run() { VisitInsertionOrder(); }

  // Check `block`.
  void VisitBasicBlock(HBasicBlock* block) OVERRIDE;

  // Check `instruction`.
  void VisitInstruction(HInstruction* instruction) OVERRIDE;

  // Perform control-flow graph checks on instruction.
  void VisitInvokeStaticOrDirect(HInvokeStaticOrDirect* invoke) OVERRIDE;

  // Check that the HasBoundsChecks() flag is set for bounds checks.
  void VisitBoundsCheck(HBoundsCheck* check) OVERRIDE;

  // Check that HCheckCast and HInstanceOf have HLoadClass as second input.
  void VisitCheckCast(HCheckCast* check) OVERRIDE;
  void VisitInstanceOf(HInstanceOf* check) OVERRIDE;

  // Was the last visit of the graph valid?
  bool IsValid() const {
    return errors_.empty();
  }

  // Get the list of detected errors.
  const std::vector<std::string>& GetErrors() const {
    return errors_;
  }

  // Print detected errors on output stream `os`.
  void Dump(std::ostream& os) const {
    for (size_t i = 0, e = errors_.size(); i < e; ++i) {
      os << dump_prefix_ << errors_[i] << std::endl;
    }
  }

 protected:
  // Report a new error.
  void AddError(const std::string& error) {
    errors_.push_back(error);
  }

  ArenaAllocator* const allocator_;
  // The block currently visited.
  HBasicBlock* current_block_ = nullptr;
  // Errors encountered while checking the graph.
  std::vector<std::string> errors_;

 private:
  // String displayed before dumped errors.
  const char* const dump_prefix_;
  ArenaBitVector seen_ids_;

  DISALLOW_COPY_AND_ASSIGN(GraphChecker);
};


// An SSA graph visitor performing various checks.
class SSAChecker : public GraphChecker {
 public:
  typedef GraphChecker super_type;

  // TODO: There's no need to pass a separate allocator as we could get it from the graph.
  SSAChecker(ArenaAllocator* allocator, HGraph* graph)
    : GraphChecker(allocator, graph, "art::SSAChecker: ") {}

  // Check the whole graph (in reverse post-order).
  void Run() OVERRIDE {
    // VisitReversePostOrder is used instead of VisitInsertionOrder,
    // as the latter might visit dead blocks removed by the dominator
    // computation.
    VisitReversePostOrder();
  }

  // Perform SSA form checks on `block`.
  void VisitBasicBlock(HBasicBlock* block) OVERRIDE;
  // Loop-related checks from block `loop_header`.
  void CheckLoop(HBasicBlock* loop_header);

  // Perform SSA form checks on instructions.
  void VisitInstruction(HInstruction* instruction) OVERRIDE;
  void VisitPhi(HPhi* phi) OVERRIDE;
  void VisitBinaryOperation(HBinaryOperation* op) OVERRIDE;
  void VisitCondition(HCondition* op) OVERRIDE;
  void VisitIf(HIf* instruction) OVERRIDE;
  void VisitBooleanNot(HBooleanNot* instruction) OVERRIDE;
  void VisitConstant(HConstant* instruction) OVERRIDE;

  void HandleBooleanInput(HInstruction* instruction, size_t input_index);

 private:
  DISALLOW_COPY_AND_ASSIGN(SSAChecker);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_GRAPH_CHECKER_H_
