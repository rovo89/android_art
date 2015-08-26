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

#include "induction_var_analysis.h"

namespace art {

/**
 * Returns true if instruction is invariant within the given loop.
 */
static bool IsLoopInvariant(HLoopInformation* loop, HInstruction* instruction) {
  HLoopInformation* other_loop = instruction->GetBlock()->GetLoopInformation();
  if (other_loop != loop) {
    // If instruction does not occur in same loop, it is invariant
    // if it appears in an outer loop (including no loop at all).
    return other_loop == nullptr || loop->IsIn(*other_loop);
  }
  return false;
}

/**
 * Returns true if instruction is proper entry-phi-operation for given loop
 * (referred to as mu-operation in Gerlek's paper).
 */
static bool IsEntryPhi(HLoopInformation* loop, HInstruction* instruction) {
  return
      instruction->IsPhi() &&
      instruction->InputCount() == 2 &&
      instruction->GetBlock() == loop->GetHeader();
}

//
// Class methods.
//

HInductionVarAnalysis::HInductionVarAnalysis(HGraph* graph)
    : HOptimization(graph, kInductionPassName),
      global_depth_(0),
      stack_(graph->GetArena()->Adapter()),
      scc_(graph->GetArena()->Adapter()),
      map_(std::less<int>(), graph->GetArena()->Adapter()),
      cycle_(std::less<int>(), graph->GetArena()->Adapter()),
      induction_(std::less<int>(), graph->GetArena()->Adapter()) {
}

void HInductionVarAnalysis::Run() {
  // Detects sequence variables (generalized induction variables) during an
  // inner-loop-first traversal of all loops using Gerlek's algorithm.
  for (HPostOrderIterator it_graph(*graph_); !it_graph.Done(); it_graph.Advance()) {
    HBasicBlock* graph_block = it_graph.Current();
    if (graph_block->IsLoopHeader()) {
      VisitLoop(graph_block->GetLoopInformation());
    }
  }
}

void HInductionVarAnalysis::VisitLoop(HLoopInformation* loop) {
  // Find strongly connected components (SSCs) in the SSA graph of this loop using Tarjan's
  // algorithm. Due to the descendant-first nature, classification happens "on-demand".
  global_depth_ = 0;
  CHECK(stack_.empty());
  map_.clear();

  for (HBlocksInLoopIterator it_loop(*loop); !it_loop.Done(); it_loop.Advance()) {
    HBasicBlock* loop_block = it_loop.Current();
    CHECK(loop_block->IsInLoop());
    if (loop_block->GetLoopInformation() != loop) {
      continue;  // Inner loops already visited.
    }
    // Visit phi-operations and instructions.
    for (HInstructionIterator it(loop_block->GetPhis()); !it.Done(); it.Advance()) {
      HInstruction* instruction = it.Current();
      if (!IsVisitedNode(instruction->GetId())) {
        VisitNode(loop, instruction);
      }
    }
    for (HInstructionIterator it(loop_block->GetInstructions()); !it.Done(); it.Advance()) {
      HInstruction* instruction = it.Current();
      if (!IsVisitedNode(instruction->GetId())) {
        VisitNode(loop, instruction);
      }
    }
  }

  CHECK(stack_.empty());
  map_.clear();
}

void HInductionVarAnalysis::VisitNode(HLoopInformation* loop, HInstruction* instruction) {
  const int id = instruction->GetId();
  const uint32_t d1 = ++global_depth_;
  map_.Put(id, NodeInfo(d1));
  stack_.push_back(instruction);

  // Visit all descendants.
  uint32_t low = d1;
  for (size_t i = 0, count = instruction->InputCount(); i < count; ++i) {
    low = std::min(low, VisitDescendant(loop, instruction->InputAt(i)));
  }

  // Lower or found SCC?
  if (low < d1) {
    map_.find(id)->second.depth = low;
  } else {
    scc_.clear();
    cycle_.clear();

    // Pop the stack to build the SCC for classification.
    while (!stack_.empty()) {
      HInstruction* x = stack_.back();
      scc_.push_back(x);
      stack_.pop_back();
      map_.find(x->GetId())->second.done = true;
      if (x == instruction) {
        break;
      }
    }

    // Classify the SCC.
    if (scc_.size() == 1 && !IsEntryPhi(loop, scc_[0])) {
      ClassifyTrivial(loop, scc_[0]);
    } else {
      ClassifyNonTrivial(loop);
    }

    scc_.clear();
    cycle_.clear();
  }
}

uint32_t HInductionVarAnalysis::VisitDescendant(HLoopInformation* loop, HInstruction* instruction) {
  // If the definition is either outside the loop (loop invariant entry value)
  // or assigned in inner loop (inner exit value), the traversal stops.
  HLoopInformation* otherLoop = instruction->GetBlock()->GetLoopInformation();
  if (otherLoop != loop) {
    return global_depth_;
  }

  // Inspect descendant node.
  const int id = instruction->GetId();
  if (!IsVisitedNode(id)) {
    VisitNode(loop, instruction);
    return map_.find(id)->second.depth;
  } else {
    auto it = map_.find(id);
    return it->second.done ? global_depth_ : it->second.depth;
  }
}

void HInductionVarAnalysis::ClassifyTrivial(HLoopInformation* loop, HInstruction* instruction) {
  InductionInfo* info = nullptr;
  if (instruction->IsPhi()) {
    for (size_t i = 1, count = instruction->InputCount(); i < count; i++) {
      info = TransferPhi(LookupInfo(loop, instruction->InputAt(0)),
                         LookupInfo(loop, instruction->InputAt(i)));
    }
  } else if (instruction->IsAdd()) {
    info = TransferAddSub(LookupInfo(loop, instruction->InputAt(0)),
                          LookupInfo(loop, instruction->InputAt(1)), kAdd);
  } else if (instruction->IsSub()) {
    info = TransferAddSub(LookupInfo(loop, instruction->InputAt(0)),
                          LookupInfo(loop, instruction->InputAt(1)), kSub);
  } else if (instruction->IsMul()) {
    info = TransferMul(LookupInfo(loop, instruction->InputAt(0)),
                       LookupInfo(loop, instruction->InputAt(1)));
  } else if (instruction->IsNeg()) {
    info = TransferNeg(LookupInfo(loop, instruction->InputAt(0)));
  }

  // Successfully classified?
  if (info != nullptr) {
    AssignInfo(loop, instruction, info);
  }
}

void HInductionVarAnalysis::ClassifyNonTrivial(HLoopInformation* loop) {
  const size_t size = scc_.size();
  CHECK_GE(size, 1u);
  HInstruction* phi = scc_[size - 1];
  if (!IsEntryPhi(loop, phi)) {
    return;
  }
  HInstruction* external = phi->InputAt(0);
  HInstruction* internal = phi->InputAt(1);
  InductionInfo* initial = LookupInfo(loop, external);
  if (initial == nullptr || initial->induction_class != kInvariant) {
    return;
  }

  // Singleton entry-phi-operation may be a wrap-around induction.
  if (size == 1) {
    InductionInfo* update = LookupInfo(loop, internal);
    if (update != nullptr) {
      AssignInfo(loop, phi, NewInductionInfo(kWrapAround, kNop, initial, update, nullptr));
    }
    return;
  }

  // Inspect remainder of the cycle that resides in scc_. The cycle_ mapping assigns
  // temporary meaning to its nodes.
  cycle_.Overwrite(phi->GetId(), nullptr);
  for (size_t i = 0; i < size - 1; i++) {
    HInstruction* operation = scc_[i];
    InductionInfo* update = nullptr;
    if (operation->IsPhi()) {
      update = TransferCycleOverPhi(operation);
    } else if (operation->IsAdd()) {
      update = TransferCycleOverAddSub(loop, operation->InputAt(0), operation->InputAt(1), kAdd, true);
    } else if (operation->IsSub()) {
      update = TransferCycleOverAddSub(loop, operation->InputAt(0), operation->InputAt(1), kSub, true);
    }
    if (update == nullptr) {
      return;
    }
    cycle_.Overwrite(operation->GetId(), update);
  }

  // Success if the internal link received accumulated nonzero update.
  auto it = cycle_.find(internal->GetId());
  if (it != cycle_.end() && it->second != nullptr) {
    // Classify header phi and feed the cycle "on-demand".
    AssignInfo(loop, phi, NewInductionInfo(kLinear, kNop, it->second, initial, nullptr));
    for (size_t i = 0; i < size - 1; i++) {
      ClassifyTrivial(loop, scc_[i]);
    }
  }
}

HInductionVarAnalysis::InductionInfo* HInductionVarAnalysis::TransferPhi(InductionInfo* a,
                                                                         InductionInfo* b) {
  // Transfer over a phi: if both inputs are identical, result is input.
  if (InductionEqual(a, b)) {
    return a;
  }
  return nullptr;
}

HInductionVarAnalysis::InductionInfo* HInductionVarAnalysis::TransferAddSub(InductionInfo* a,
                                                                            InductionInfo* b,
                                                                            InductionOp op) {
  // Transfer over an addition or subtraction: invariant or linear
  // inputs combine into new invariant or linear result.
  if (a != nullptr && b != nullptr) {
    if (a->induction_class == kInvariant && b->induction_class == kInvariant) {
      return NewInductionInfo(kInvariant, op, a, b, nullptr);
    } else if (a->induction_class == kLinear && b->induction_class == kInvariant) {
      return NewInductionInfo(
          kLinear,
          kNop,
          a->op_a,
          NewInductionInfo(kInvariant, op, a->op_b, b, nullptr),
          nullptr);
    } else if (a->induction_class == kInvariant && b->induction_class == kLinear) {
      InductionInfo* ba = b->op_a;
      if (op == kSub) {  // negation required
        ba = NewInductionInfo(kInvariant, kNeg, nullptr, ba, nullptr);
      }
      return NewInductionInfo(
          kLinear,
          kNop,
          ba,
          NewInductionInfo(kInvariant, op, a, b->op_b, nullptr),
          nullptr);
    } else if (a->induction_class == kLinear && b->induction_class == kLinear) {
      return NewInductionInfo(
          kLinear,
          kNop,
          NewInductionInfo(kInvariant, op, a->op_a, b->op_a, nullptr),
          NewInductionInfo(kInvariant, op, a->op_b, b->op_b, nullptr),
          nullptr);
    }
  }
  return nullptr;
}

HInductionVarAnalysis::InductionInfo* HInductionVarAnalysis::TransferMul(InductionInfo* a,
                                                                         InductionInfo* b) {
  // Transfer over a multiplication: invariant or linear
  // inputs combine into new invariant or linear result.
  // Two linear inputs would become quadratic.
  if (a != nullptr && b != nullptr) {
    if (a->induction_class == kInvariant && b->induction_class == kInvariant) {
      return NewInductionInfo(kInvariant, kMul, a, b, nullptr);
    } else if (a->induction_class == kLinear && b->induction_class == kInvariant) {
      return NewInductionInfo(
          kLinear,
          kNop,
          NewInductionInfo(kInvariant, kMul, a->op_a, b, nullptr),
          NewInductionInfo(kInvariant, kMul, a->op_b, b, nullptr),
          nullptr);
    } else if (a->induction_class == kInvariant && b->induction_class == kLinear) {
      return NewInductionInfo(
          kLinear,
          kNop,
          NewInductionInfo(kInvariant, kMul, a, b->op_a, nullptr),
          NewInductionInfo(kInvariant, kMul, a, b->op_b, nullptr),
          nullptr);
    }
  }
  return nullptr;
}

HInductionVarAnalysis::InductionInfo* HInductionVarAnalysis::TransferNeg(InductionInfo* a) {
  // Transfer over a unary negation: invariant or linear input
  // yields a similar, but negated result.
  if (a != nullptr) {
    if (a->induction_class == kInvariant) {
      return NewInductionInfo(kInvariant, kNeg, nullptr, a, nullptr);
    } else if (a->induction_class == kLinear) {
      return NewInductionInfo(
          kLinear,
          kNop,
          NewInductionInfo(kInvariant, kNeg, nullptr, a->op_a, nullptr),
          NewInductionInfo(kInvariant, kNeg, nullptr, a->op_b, nullptr),
          nullptr);
    }
  }
  return nullptr;
}

HInductionVarAnalysis::InductionInfo* HInductionVarAnalysis::TransferCycleOverPhi(HInstruction* phi) {
  // Transfer within a cycle over a phi: only identical inputs
  // can be combined into that input as result.
  const size_t count = phi->InputCount();
  CHECK_GT(count, 0u);
  auto ita = cycle_.find(phi->InputAt(0)->GetId());
  if (ita != cycle_.end()) {
    InductionInfo* a = ita->second;
    for (size_t i = 1; i < count; i++) {
      auto itb = cycle_.find(phi->InputAt(i)->GetId());
      if (itb == cycle_.end() ||!HInductionVarAnalysis::InductionEqual(a, itb->second)) {
        return nullptr;
      }
    }
    return a;
  }
  return nullptr;
}

HInductionVarAnalysis::InductionInfo* HInductionVarAnalysis::TransferCycleOverAddSub(
    HLoopInformation* loop,
    HInstruction* x,
    HInstruction* y,
    InductionOp op,
    bool first) {
  // Transfer within a cycle over an addition or subtraction: adding or
  // subtracting an invariant value adds to the stride of the induction,
  // starting with the phi value denoted by the unusual nullptr value.
  auto it = cycle_.find(x->GetId());
  if (it != cycle_.end()) {
    InductionInfo* a = it->second;
    InductionInfo* b = LookupInfo(loop, y);
    if (b != nullptr && b->induction_class == kInvariant) {
      if (a == nullptr) {
        if (op == kSub) {  // negation required
          return NewInductionInfo(kInvariant, kNeg, nullptr, b, nullptr);
        }
        return b;
      } else if (a->induction_class == kInvariant) {
        return NewInductionInfo(kInvariant, op, a, b, nullptr);
      }
    }
  }
  // On failure, try alternatives.
  if (op == kAdd) {
    // Try the other way around for an addition.
    if (first) {
      return TransferCycleOverAddSub(loop, y, x, op, false);
    }
  }
  return nullptr;
}

void HInductionVarAnalysis::PutInfo(int loop_id, int id, InductionInfo* info) {
  auto it = induction_.find(loop_id);
  if (it == induction_.end()) {
    it = induction_.Put(
        loop_id, ArenaSafeMap<int, InductionInfo*>(std::less<int>(), graph_->GetArena()->Adapter()));
  }
  it->second.Overwrite(id, info);
}

HInductionVarAnalysis::InductionInfo* HInductionVarAnalysis::GetInfo(int loop_id, int id) {
  auto it = induction_.find(loop_id);
  if (it != induction_.end()) {
    auto loop_it = it->second.find(id);
    if (loop_it != it->second.end()) {
      return loop_it->second;
    }
  }
  return nullptr;
}

void HInductionVarAnalysis::AssignInfo(HLoopInformation* loop,
                                       HInstruction* instruction,
                                       InductionInfo* info) {
  const int loopId = loop->GetHeader()->GetBlockId();
  const int id = instruction->GetId();
  PutInfo(loopId, id, info);
}

HInductionVarAnalysis::InductionInfo*
HInductionVarAnalysis::LookupInfo(HLoopInformation* loop,
                                  HInstruction* instruction) {
  const int loop_id = loop->GetHeader()->GetBlockId();
  const int id = instruction->GetId();
  InductionInfo* info = GetInfo(loop_id, id);
  if (info == nullptr && IsLoopInvariant(loop, instruction)) {
    info = NewInductionInfo(kInvariant, kFetch, nullptr, nullptr, instruction);
    PutInfo(loop_id, id, info);
  }
  return info;
}

bool HInductionVarAnalysis::InductionEqual(InductionInfo* info1,
                                           InductionInfo* info2) {
  // Test structural equality only, without accounting for simplifications.
  if (info1 != nullptr && info2 != nullptr) {
    return
        info1->induction_class == info2->induction_class &&
        info1->operation       == info2->operation       &&
        info1->fetch           == info2->fetch           &&
        InductionEqual(info1->op_a, info2->op_a)         &&
        InductionEqual(info1->op_b, info2->op_b);
  }
  // Otherwise only two nullptrs are considered equal.
  return info1 == info2;
}

std::string HInductionVarAnalysis::InductionToString(InductionInfo* info) {
  if (info != nullptr) {
    if (info->induction_class == kInvariant) {
      std::string inv = "(";
      inv += InductionToString(info->op_a);
      switch (info->operation) {
        case kNop: inv += " ? "; break;
        case kAdd: inv += " + "; break;
        case kSub:
        case kNeg: inv += " - "; break;
        case kMul: inv += " * "; break;
        case kDiv: inv += " / "; break;
        case kFetch:
          CHECK(info->fetch != nullptr);
          inv += std::to_string(info->fetch->GetId()) + ":" + info->fetch->DebugName();
          break;
      }
      inv += InductionToString(info->op_b);
      return inv + ")";
    } else {
      CHECK(info->operation == kNop);
      if (info->induction_class == kLinear) {
        return "(" + InductionToString(info->op_a) + " * i + " +
                     InductionToString(info->op_b) + ")";
      } else if (info->induction_class == kWrapAround) {
        return "wrap(" + InductionToString(info->op_a) + ", " +
                         InductionToString(info->op_b) + ")";
      } else if (info->induction_class == kPeriodic) {
        return "periodic(" + InductionToString(info->op_a) + ", " +
                             InductionToString(info->op_b) + ")";
      }
    }
  }
  return "";
}

}  // namespace art
