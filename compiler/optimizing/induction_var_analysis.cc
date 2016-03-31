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
#include "induction_var_range.h"

namespace art {

/**
 * Since graph traversal may enter a SCC at any position, an initial representation may be rotated,
 * along dependences, viz. any of (a, b, c, d), (d, a, b, c)  (c, d, a, b), (b, c, d, a) assuming
 * a chain of dependences (mutual independent items may occur in arbitrary order). For proper
 * classification, the lexicographically first entry-phi is rotated to the front.
 */
static void RotateEntryPhiFirst(HLoopInformation* loop,
                                ArenaVector<HInstruction*>* scc,
                                ArenaVector<HInstruction*>* new_scc) {
  // Find very first entry-phi.
  const HInstructionList& phis = loop->GetHeader()->GetPhis();
  HInstruction* phi = nullptr;
  size_t phi_pos = -1;
  const size_t size = scc->size();
  for (size_t i = 0; i < size; i++) {
    HInstruction* other = (*scc)[i];
    if (other->IsLoopHeaderPhi() && (phi == nullptr || phis.FoundBefore(other, phi))) {
      phi = other;
      phi_pos = i;
    }
  }

  // If found, bring that entry-phi to front.
  if (phi != nullptr) {
    new_scc->clear();
    for (size_t i = 0; i < size; i++) {
      new_scc->push_back((*scc)[phi_pos]);
      if (++phi_pos >= size) phi_pos = 0;
    }
    DCHECK_EQ(size, new_scc->size());
    scc->swap(*new_scc);
  }
}

/**
 * Returns true if the from/to types denote a narrowing, integral conversion (precision loss).
 */
static bool IsNarrowingIntegralConversion(Primitive::Type from, Primitive::Type to) {
  switch (from) {
    case Primitive::kPrimLong:
      return to == Primitive::kPrimByte || to == Primitive::kPrimShort
          || to == Primitive::kPrimChar || to == Primitive::kPrimInt;
    case Primitive::kPrimInt:
      return to == Primitive::kPrimByte || to == Primitive::kPrimShort
          || to == Primitive::kPrimChar;
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
      return to == Primitive::kPrimByte;
    default:
      return false;
  }
}

/**
 * Returns narrowest data type.
 */
static Primitive::Type Narrowest(Primitive::Type type1, Primitive::Type type2) {
  return Primitive::ComponentSize(type1) <= Primitive::ComponentSize(type2) ? type1 : type2;
}

//
// Class methods.
//

HInductionVarAnalysis::HInductionVarAnalysis(HGraph* graph)
    : HOptimization(graph, kInductionPassName),
      global_depth_(0),
      stack_(graph->GetArena()->Adapter(kArenaAllocInductionVarAnalysis)),
      scc_(graph->GetArena()->Adapter(kArenaAllocInductionVarAnalysis)),
      map_(std::less<HInstruction*>(),
           graph->GetArena()->Adapter(kArenaAllocInductionVarAnalysis)),
      cycle_(std::less<HInstruction*>(),
             graph->GetArena()->Adapter(kArenaAllocInductionVarAnalysis)),
      induction_(std::less<HLoopInformation*>(),
                 graph->GetArena()->Adapter(kArenaAllocInductionVarAnalysis)) {
}

void HInductionVarAnalysis::Run() {
  // Detects sequence variables (generalized induction variables) during an outer to inner
  // traversal of all loops using Gerlek's algorithm. The order is important to enable
  // range analysis on outer loop while visiting inner loops.
  for (HReversePostOrderIterator it_graph(*graph_); !it_graph.Done(); it_graph.Advance()) {
    HBasicBlock* graph_block = it_graph.Current();
    // Don't analyze irreducible loops.
    // TODO(ajcbik): could/should we remove this restriction?
    if (graph_block->IsLoopHeader() && !graph_block->GetLoopInformation()->IsIrreducible()) {
      VisitLoop(graph_block->GetLoopInformation());
    }
  }
}

void HInductionVarAnalysis::VisitLoop(HLoopInformation* loop) {
  // Find strongly connected components (SSCs) in the SSA graph of this loop using Tarjan's
  // algorithm. Due to the descendant-first nature, classification happens "on-demand".
  global_depth_ = 0;
  DCHECK(stack_.empty());
  map_.clear();

  for (HBlocksInLoopIterator it_loop(*loop); !it_loop.Done(); it_loop.Advance()) {
    HBasicBlock* loop_block = it_loop.Current();
    DCHECK(loop_block->IsInLoop());
    if (loop_block->GetLoopInformation() != loop) {
      continue;  // Inner loops already visited.
    }
    // Visit phi-operations and instructions.
    for (HInstructionIterator it(loop_block->GetPhis()); !it.Done(); it.Advance()) {
      HInstruction* instruction = it.Current();
      if (!IsVisitedNode(instruction)) {
        VisitNode(loop, instruction);
      }
    }
    for (HInstructionIterator it(loop_block->GetInstructions()); !it.Done(); it.Advance()) {
      HInstruction* instruction = it.Current();
      if (!IsVisitedNode(instruction)) {
        VisitNode(loop, instruction);
      }
    }
  }

  DCHECK(stack_.empty());
  map_.clear();

  // Determine the loop's trip-count.
  VisitControl(loop);
}

void HInductionVarAnalysis::VisitNode(HLoopInformation* loop, HInstruction* instruction) {
  const uint32_t d1 = ++global_depth_;
  map_.Put(instruction, NodeInfo(d1));
  stack_.push_back(instruction);

  // Visit all descendants.
  uint32_t low = d1;
  for (size_t i = 0, count = instruction->InputCount(); i < count; ++i) {
    low = std::min(low, VisitDescendant(loop, instruction->InputAt(i)));
  }

  // Lower or found SCC?
  if (low < d1) {
    map_.find(instruction)->second.depth = low;
  } else {
    scc_.clear();
    cycle_.clear();

    // Pop the stack to build the SCC for classification.
    while (!stack_.empty()) {
      HInstruction* x = stack_.back();
      scc_.push_back(x);
      stack_.pop_back();
      map_.find(x)->second.done = true;
      if (x == instruction) {
        break;
      }
    }

    // Type of induction.
    type_ = scc_[0]->GetType();

    // Classify the SCC.
    if (scc_.size() == 1 && !scc_[0]->IsLoopHeaderPhi()) {
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
  if (!IsVisitedNode(instruction)) {
    VisitNode(loop, instruction);
    return map_.find(instruction)->second.depth;
  } else {
    auto it = map_.find(instruction);
    return it->second.done ? global_depth_ : it->second.depth;
  }
}

void HInductionVarAnalysis::ClassifyTrivial(HLoopInformation* loop, HInstruction* instruction) {
  InductionInfo* info = nullptr;
  if (instruction->IsPhi()) {
    info = TransferPhi(loop, instruction, /* input_index */ 0);
  } else if (instruction->IsAdd()) {
    info = TransferAddSub(LookupInfo(loop, instruction->InputAt(0)),
                          LookupInfo(loop, instruction->InputAt(1)), kAdd);
  } else if (instruction->IsSub()) {
    info = TransferAddSub(LookupInfo(loop, instruction->InputAt(0)),
                          LookupInfo(loop, instruction->InputAt(1)), kSub);
  } else if (instruction->IsMul()) {
    info = TransferMul(LookupInfo(loop, instruction->InputAt(0)),
                       LookupInfo(loop, instruction->InputAt(1)));
  } else if (instruction->IsShl()) {
    info = TransferShl(LookupInfo(loop, instruction->InputAt(0)),
                       LookupInfo(loop, instruction->InputAt(1)),
                       instruction->InputAt(0)->GetType());
  } else if (instruction->IsNeg()) {
    info = TransferNeg(LookupInfo(loop, instruction->InputAt(0)));
  } else if (instruction->IsTypeConversion()) {
    info = TransferCnv(LookupInfo(loop, instruction->InputAt(0)),
                       instruction->AsTypeConversion()->GetInputType(),
                       instruction->AsTypeConversion()->GetResultType());

  } else if (instruction->IsBoundsCheck()) {
    info = LookupInfo(loop, instruction->InputAt(0));  // Pass-through.
  }

  // Successfully classified?
  if (info != nullptr) {
    AssignInfo(loop, instruction, info);
  }
}

void HInductionVarAnalysis::ClassifyNonTrivial(HLoopInformation* loop) {
  const size_t size = scc_.size();
  DCHECK_GE(size, 1u);

  // Rotate proper entry-phi to front.
  if (size > 1) {
    ArenaVector<HInstruction*> other(graph_->GetArena()->Adapter(kArenaAllocInductionVarAnalysis));
    RotateEntryPhiFirst(loop, &scc_, &other);
  }

  // Analyze from entry-phi onwards.
  HInstruction* phi = scc_[0];
  if (!phi->IsLoopHeaderPhi()) {
    return;
  }

  // External link should be loop invariant.
  InductionInfo* initial = LookupInfo(loop, phi->InputAt(0));
  if (initial == nullptr || initial->induction_class != kInvariant) {
    return;
  }

  // Singleton is wrap-around induction if all internal links have the same meaning.
  if (size == 1) {
    InductionInfo* update = TransferPhi(loop, phi, /* input_index */ 1);
    if (update != nullptr) {
      AssignInfo(loop, phi, CreateInduction(kWrapAround, initial, update, type_));
    }
    return;
  }

  // Inspect remainder of the cycle that resides in scc_. The cycle_ mapping assigns
  // temporary meaning to its nodes, seeded from the phi instruction and back.
  for (size_t i = 1; i < size; i++) {
    HInstruction* instruction = scc_[i];
    InductionInfo* update = nullptr;
    if (instruction->IsPhi()) {
      update = SolvePhiAllInputs(loop, phi, instruction);
    } else if (instruction->IsAdd()) {
      update = SolveAddSub(
          loop, phi, instruction, instruction->InputAt(0), instruction->InputAt(1), kAdd, true);
    } else if (instruction->IsSub()) {
      update = SolveAddSub(
          loop, phi, instruction, instruction->InputAt(0), instruction->InputAt(1), kSub, true);
    } else if (instruction->IsTypeConversion()) {
      update = SolveCnv(instruction->AsTypeConversion());
    }
    if (update == nullptr) {
      return;
    }
    cycle_.Put(instruction, update);
  }

  // Success if all internal links received the same temporary meaning.
  InductionInfo* induction = SolvePhi(phi, /* input_index */ 1);
  if (induction != nullptr) {
    switch (induction->induction_class) {
      case kInvariant:
        // Classify first phi and then the rest of the cycle "on-demand".
        // Statements are scanned in order.
        AssignInfo(loop, phi, CreateInduction(kLinear, induction, initial, type_));
        for (size_t i = 1; i < size; i++) {
          ClassifyTrivial(loop, scc_[i]);
        }
        break;
      case kPeriodic:
        // Classify all elements in the cycle with the found periodic induction while
        // rotating each first element to the end. Lastly, phi is classified.
        // Statements are scanned in reverse order.
        for (size_t i = size - 1; i >= 1; i--) {
          AssignInfo(loop, scc_[i], induction);
          induction = RotatePeriodicInduction(induction->op_b, induction->op_a);
        }
        AssignInfo(loop, phi, induction);
        break;
      default:
        break;
    }
  }
}

HInductionVarAnalysis::InductionInfo* HInductionVarAnalysis::RotatePeriodicInduction(
    InductionInfo* induction,
    InductionInfo* last) {
  // Rotates a periodic induction of the form
  //   (a, b, c, d, e)
  // into
  //   (b, c, d, e, a)
  // in preparation of assigning this to the previous variable in the sequence.
  if (induction->induction_class == kInvariant) {
    return CreateInduction(kPeriodic, induction, last, type_);
  }
  return CreateInduction(
      kPeriodic, induction->op_a, RotatePeriodicInduction(induction->op_b, last), type_);
}

HInductionVarAnalysis::InductionInfo* HInductionVarAnalysis::TransferPhi(HLoopInformation* loop,
                                                                         HInstruction* phi,
                                                                         size_t input_index) {
  // Match all phi inputs from input_index onwards exactly.
  const size_t count = phi->InputCount();
  DCHECK_LT(input_index, count);
  InductionInfo* a = LookupInfo(loop, phi->InputAt(input_index));
  for (size_t i = input_index + 1; i < count; i++) {
    InductionInfo* b = LookupInfo(loop, phi->InputAt(i));
    if (!InductionEqual(a, b)) {
      return nullptr;
    }
  }
  return a;
}

HInductionVarAnalysis::InductionInfo* HInductionVarAnalysis::TransferAddSub(InductionInfo* a,
                                                                            InductionInfo* b,
                                                                            InductionOp op) {
  // Transfer over an addition or subtraction: any invariant, linear, wrap-around, or periodic
  // can be combined with an invariant to yield a similar result. Even two linear inputs can
  // be combined. All other combinations fail, however.
  if (a != nullptr && b != nullptr) {
    if (a->induction_class == kInvariant && b->induction_class == kInvariant) {
      return CreateInvariantOp(op, a, b);
    } else if (a->induction_class == kLinear && b->induction_class == kLinear) {
      return CreateInduction(kLinear,
                             TransferAddSub(a->op_a, b->op_a, op),
                             TransferAddSub(a->op_b, b->op_b, op),
                             type_);
    } else if (a->induction_class == kInvariant) {
      InductionInfo* new_a = b->op_a;
      InductionInfo* new_b = TransferAddSub(a, b->op_b, op);
      if (b->induction_class != kLinear) {
        DCHECK(b->induction_class == kWrapAround || b->induction_class == kPeriodic);
        new_a = TransferAddSub(a, new_a, op);
      } else if (op == kSub) {  // Negation required.
        new_a = TransferNeg(new_a);
      }
      return CreateInduction(b->induction_class, new_a, new_b, type_);
    } else if (b->induction_class == kInvariant) {
      InductionInfo* new_a = a->op_a;
      InductionInfo* new_b = TransferAddSub(a->op_b, b, op);
      if (a->induction_class != kLinear) {
        DCHECK(a->induction_class == kWrapAround || a->induction_class == kPeriodic);
        new_a = TransferAddSub(new_a, b, op);
      }
      return CreateInduction(a->induction_class, new_a, new_b, type_);
    }
  }
  return nullptr;
}

HInductionVarAnalysis::InductionInfo* HInductionVarAnalysis::TransferMul(InductionInfo* a,
                                                                         InductionInfo* b) {
  // Transfer over a multiplication: any invariant, linear, wrap-around, or periodic
  // can be multiplied with an invariant to yield a similar but multiplied result.
  // Two non-invariant inputs cannot be multiplied, however.
  if (a != nullptr && b != nullptr) {
    if (a->induction_class == kInvariant && b->induction_class == kInvariant) {
      return CreateInvariantOp(kMul, a, b);
    } else if (a->induction_class == kInvariant) {
      return CreateInduction(b->induction_class,
                             TransferMul(a, b->op_a),
                             TransferMul(a, b->op_b),
                             type_);
    } else if (b->induction_class == kInvariant) {
      return CreateInduction(a->induction_class,
                             TransferMul(a->op_a, b),
                             TransferMul(a->op_b, b),
                             type_);
    }
  }
  return nullptr;
}

HInductionVarAnalysis::InductionInfo* HInductionVarAnalysis::TransferShl(InductionInfo* a,
                                                                         InductionInfo* b,
                                                                         Primitive::Type type) {
  // Transfer over a shift left: treat shift by restricted constant as equivalent multiplication.
  int64_t value = -1;
  if (a != nullptr && IsExact(b, &value)) {
    // Obtain the constant needed for the multiplication. This yields an existing instruction
    // if the constants is already there. Otherwise, this has a side effect on the HIR.
    // The restriction on the shift factor avoids generating a negative constant
    // (viz. 1 << 31 and 1L << 63 set the sign bit). The code assumes that generalization
    // for shift factors outside [0,32) and [0,64) ranges is done by earlier simplification.
    if ((type == Primitive::kPrimInt  && 0 <= value && value < 31) ||
        (type == Primitive::kPrimLong && 0 <= value && value < 63)) {
      return TransferMul(a, CreateConstant(1 << value, type));
    }
  }
  return nullptr;
}

HInductionVarAnalysis::InductionInfo* HInductionVarAnalysis::TransferNeg(InductionInfo* a) {
  // Transfer over a unary negation: an invariant, linear, wrap-around, or periodic input
  // yields a similar but negated induction as result.
  if (a != nullptr) {
    if (a->induction_class == kInvariant) {
      return CreateInvariantOp(kNeg, nullptr, a);
    }
    return CreateInduction(a->induction_class, TransferNeg(a->op_a), TransferNeg(a->op_b), type_);
  }
  return nullptr;
}

HInductionVarAnalysis::InductionInfo* HInductionVarAnalysis::TransferCnv(InductionInfo* a,
                                                                         Primitive::Type from,
                                                                         Primitive::Type to) {
  if (a != nullptr) {
    // Allow narrowing conversion in certain cases.
    if (IsNarrowingIntegralConversion(from, to)) {
      if (a->induction_class == kLinear) {
        if (a->type == to || (a->type == from && IsNarrowingIntegralConversion(from, to))) {
          return CreateInduction(kLinear, a->op_a, a->op_b, to);
        }
      }
      // TODO: other cases useful too?
    }
  }
  return nullptr;
}

HInductionVarAnalysis::InductionInfo* HInductionVarAnalysis::SolvePhi(HInstruction* phi,
                                                                      size_t input_index) {
  // Match all phi inputs from input_index onwards exactly.
  const size_t count = phi->InputCount();
  DCHECK_LT(input_index, count);
  auto ita = cycle_.find(phi->InputAt(input_index));
  if (ita != cycle_.end()) {
    for (size_t i = input_index + 1; i < count; i++) {
      auto itb = cycle_.find(phi->InputAt(i));
      if (itb == cycle_.end() ||
          !HInductionVarAnalysis::InductionEqual(ita->second, itb->second)) {
        return nullptr;
      }
    }
    return ita->second;
  }
  return nullptr;
}

HInductionVarAnalysis::InductionInfo* HInductionVarAnalysis::SolvePhiAllInputs(
    HLoopInformation* loop,
    HInstruction* entry_phi,
    HInstruction* phi) {
  // Match all phi inputs.
  InductionInfo* match = SolvePhi(phi, /* input_index */ 0);
  if (match != nullptr) {
    return match;
  }

  // Otherwise, try to solve for a periodic seeded from phi onward.
  // Only tight multi-statement cycles are considered in order to
  // simplify rotating the periodic during the final classification.
  if (phi->IsLoopHeaderPhi() && phi->InputCount() == 2) {
    InductionInfo* a = LookupInfo(loop, phi->InputAt(0));
    if (a != nullptr && a->induction_class == kInvariant) {
      if (phi->InputAt(1) == entry_phi) {
        InductionInfo* initial = LookupInfo(loop, entry_phi->InputAt(0));
        return CreateInduction(kPeriodic, a, initial, type_);
      }
      InductionInfo* b = SolvePhi(phi, /* input_index */ 1);
      if (b != nullptr && b->induction_class == kPeriodic) {
        return CreateInduction(kPeriodic, a, b, type_);
      }
    }
  }
  return nullptr;
}

HInductionVarAnalysis::InductionInfo* HInductionVarAnalysis::SolveAddSub(HLoopInformation* loop,
                                                                         HInstruction* entry_phi,
                                                                         HInstruction* instruction,
                                                                         HInstruction* x,
                                                                         HInstruction* y,
                                                                         InductionOp op,
                                                                         bool is_first_call) {
  // Solve within a cycle over an addition or subtraction: adding or subtracting an
  // invariant value, seeded from phi, keeps adding to the stride of the induction.
  InductionInfo* b = LookupInfo(loop, y);
  if (b != nullptr && b->induction_class == kInvariant) {
    if (x == entry_phi) {
      return (op == kAdd) ? b : CreateInvariantOp(kNeg, nullptr, b);
    }
    auto it = cycle_.find(x);
    if (it != cycle_.end()) {
      InductionInfo* a = it->second;
      if (a->induction_class == kInvariant) {
        return CreateInvariantOp(op, a, b);
      }
    }
  }

  // Try some alternatives before failing.
  if (op == kAdd) {
    // Try the other way around for an addition if considered for first time.
    if (is_first_call) {
      return SolveAddSub(loop, entry_phi, instruction, y, x, op, false);
    }
  } else if (op == kSub) {
    // Solve within a tight cycle that is formed by exactly two instructions,
    // one phi and one update, for a periodic idiom of the form k = c - k;
    if (y == entry_phi && entry_phi->InputCount() == 2 && instruction == entry_phi->InputAt(1)) {
      InductionInfo* a = LookupInfo(loop, x);
      if (a != nullptr && a->induction_class == kInvariant) {
        InductionInfo* initial = LookupInfo(loop, entry_phi->InputAt(0));
        return CreateInduction(kPeriodic, CreateInvariantOp(kSub, a, initial), initial, type_);
      }
    }
  }

  return nullptr;
}

HInductionVarAnalysis::InductionInfo* HInductionVarAnalysis::SolveCnv(HTypeConversion* conversion) {
  Primitive::Type from = conversion->GetInputType();
  Primitive::Type to = conversion->GetResultType();
  // A narrowing conversion is allowed within the cycle of a linear induction, provided that the
  // narrowest encountered type is recorded with the induction to account for the precision loss.
  if (IsNarrowingIntegralConversion(from, to)) {
    auto it = cycle_.find(conversion->GetInput());
    if (it != cycle_.end() && it->second->induction_class == kInvariant) {
      type_ = Narrowest(type_, to);
      return it->second;
    }
  }
  return nullptr;
}

void HInductionVarAnalysis::VisitControl(HLoopInformation* loop) {
  HInstruction* control = loop->GetHeader()->GetLastInstruction();
  if (control->IsIf()) {
    HIf* ifs = control->AsIf();
    HBasicBlock* if_true = ifs->IfTrueSuccessor();
    HBasicBlock* if_false = ifs->IfFalseSuccessor();
    HInstruction* if_expr = ifs->InputAt(0);
    // Determine if loop has following structure in header.
    // loop-header: ....
    //              if (condition) goto X
    if (if_expr->IsCondition()) {
      HCondition* condition = if_expr->AsCondition();
      InductionInfo* a = LookupInfo(loop, condition->InputAt(0));
      InductionInfo* b = LookupInfo(loop, condition->InputAt(1));
      Primitive::Type type = condition->InputAt(0)->GetType();
      // Determine if the loop control uses a known sequence on an if-exit (X outside) or on
      // an if-iterate (X inside), expressed as if-iterate when passed into VisitCondition().
      if (a == nullptr || b == nullptr) {
        return;  // Loop control is not a sequence.
      } else if (if_true->GetLoopInformation() != loop && if_false->GetLoopInformation() == loop) {
        VisitCondition(loop, a, b, type, condition->GetOppositeCondition());
      } else if (if_true->GetLoopInformation() == loop && if_false->GetLoopInformation() != loop) {
        VisitCondition(loop, a, b, type, condition->GetCondition());
      }
    }
  }
}

void HInductionVarAnalysis::VisitCondition(HLoopInformation* loop,
                                           InductionInfo* a,
                                           InductionInfo* b,
                                           Primitive::Type type,
                                           IfCondition cmp) {
  if (a->induction_class == kInvariant && b->induction_class == kLinear) {
    // Swap condition if induction is at right-hand-side (e.g. U > i is same as i < U).
    switch (cmp) {
      case kCondLT: VisitCondition(loop, b, a, type, kCondGT); break;
      case kCondLE: VisitCondition(loop, b, a, type, kCondGE); break;
      case kCondGT: VisitCondition(loop, b, a, type, kCondLT); break;
      case kCondGE: VisitCondition(loop, b, a, type, kCondLE); break;
      case kCondNE: VisitCondition(loop, b, a, type, kCondNE); break;
      default: break;
    }
  } else if (a->induction_class == kLinear && b->induction_class == kInvariant) {
    // Analyze condition with induction at left-hand-side (e.g. i < U).
    InductionInfo* lower_expr = a->op_b;
    InductionInfo* upper_expr = b;
    InductionInfo* stride_expr = a->op_a;
    // Constant stride?
    int64_t stride_value = 0;
    if (!IsExact(stride_expr, &stride_value)) {
      return;
    }
    // Rewrite condition i != U into strict end condition i < U or i > U if this end condition
    // is reached exactly (tested by verifying if the loop has a unit stride and the non-strict
    // condition would be always taken).
    if (cmp == kCondNE && ((stride_value == +1 && IsTaken(lower_expr, upper_expr, kCondLE)) ||
                           (stride_value == -1 && IsTaken(lower_expr, upper_expr, kCondGE)))) {
      cmp = stride_value > 0 ? kCondLT : kCondGT;
    }
    // Only accept integral condition. A mismatch between the type of condition and the induction
    // is only allowed if the, necessarily narrower, induction range fits the narrower control.
    if (type != Primitive::kPrimInt && type != Primitive::kPrimLong) {
      return;  // not integral
    } else if (type != a->type &&
               !FitsNarrowerControl(lower_expr, upper_expr, stride_value, a->type, cmp)) {
      return;  // mismatched type
    }
    // Normalize a linear loop control with a nonzero stride:
    //   stride > 0, either i < U or i <= U
    //   stride < 0, either i > U or i >= U
    if ((stride_value > 0 && (cmp == kCondLT || cmp == kCondLE)) ||
        (stride_value < 0 && (cmp == kCondGT || cmp == kCondGE))) {
      VisitTripCount(loop, lower_expr, upper_expr, stride_expr, stride_value, type, cmp);
    }
  }
}

void HInductionVarAnalysis::VisitTripCount(HLoopInformation* loop,
                                           InductionInfo* lower_expr,
                                           InductionInfo* upper_expr,
                                           InductionInfo* stride_expr,
                                           int64_t stride_value,
                                           Primitive::Type type,
                                           IfCondition cmp) {
  // Any loop of the general form:
  //
  //    for (i = L; i <= U; i += S) // S > 0
  // or for (i = L; i >= U; i += S) // S < 0
  //      .. i ..
  //
  // can be normalized into:
  //
  //    for (n = 0; n < TC; n++) // where TC = (U + S - L) / S
  //      .. L + S * n ..
  //
  // taking the following into consideration:
  //
  // (1) Using the same precision, the TC (trip-count) expression should be interpreted as
  //     an unsigned entity, for example, as in the following loop that uses the full range:
  //     for (int i = INT_MIN; i < INT_MAX; i++) // TC = UINT_MAX
  // (2) The TC is only valid if the loop is taken, otherwise TC = 0, as in:
  //     for (int i = 12; i < U; i++) // TC = 0 when U < 12
  //     If this cannot be determined at compile-time, the TC is only valid within the
  //     loop-body proper, not the loop-header unless enforced with an explicit taken-test.
  // (3) The TC is only valid if the loop is finite, otherwise TC has no value, as in:
  //     for (int i = 0; i <= U; i++) // TC = Inf when U = INT_MAX
  //     If this cannot be determined at compile-time, the TC is only valid when enforced
  //     with an explicit finite-test.
  // (4) For loops which early-exits, the TC forms an upper bound, as in:
  //     for (int i = 0; i < 10 && ....; i++) // TC <= 10
  InductionInfo* trip_count = upper_expr;
  const bool is_taken = IsTaken(lower_expr, upper_expr, cmp);
  const bool is_finite = IsFinite(upper_expr, stride_value, type, cmp);
  const bool cancels = (cmp == kCondLT || cmp == kCondGT) && std::abs(stride_value) == 1;
  if (!cancels) {
    // Convert exclusive integral inequality into inclusive integral inequality,
    // viz. condition i < U is i <= U - 1 and condition i > U is i >= U + 1.
    if (cmp == kCondLT) {
      trip_count = CreateInvariantOp(kSub, trip_count, CreateConstant(1, type));
    } else if (cmp == kCondGT) {
      trip_count = CreateInvariantOp(kAdd, trip_count, CreateConstant(1, type));
    }
    // Compensate for stride.
    trip_count = CreateInvariantOp(kAdd, trip_count, stride_expr);
  }
  trip_count = CreateInvariantOp(
      kDiv, CreateInvariantOp(kSub, trip_count, lower_expr), stride_expr);
  // Assign the trip-count expression to the loop control. Clients that use the information
  // should be aware that the expression is only valid under the conditions listed above.
  InductionOp tcKind = kTripCountInBodyUnsafe;  // needs both tests
  if (is_taken && is_finite) {
    tcKind = kTripCountInLoop;  // needs neither test
  } else if (is_finite) {
    tcKind = kTripCountInBody;  // needs taken-test
  } else if (is_taken) {
    tcKind = kTripCountInLoopUnsafe;  // needs finite-test
  }
  InductionOp op = kNop;
  switch (cmp) {
    case kCondLT: op = kLT; break;
    case kCondLE: op = kLE; break;
    case kCondGT: op = kGT; break;
    case kCondGE: op = kGE; break;
    default:      LOG(FATAL) << "CONDITION UNREACHABLE";
  }
  InductionInfo* taken_test = CreateInvariantOp(op, lower_expr, upper_expr);
  AssignInfo(loop,
             loop->GetHeader()->GetLastInstruction(),
             CreateTripCount(tcKind, trip_count, taken_test, type));
}

bool HInductionVarAnalysis::IsTaken(InductionInfo* lower_expr,
                                    InductionInfo* upper_expr,
                                    IfCondition cmp) {
  int64_t lower_value;
  int64_t upper_value;
  switch (cmp) {
    case kCondLT:
      return IsAtMost(lower_expr, &lower_value)
          && IsAtLeast(upper_expr, &upper_value)
          && lower_value < upper_value;
    case kCondLE:
      return IsAtMost(lower_expr, &lower_value)
          && IsAtLeast(upper_expr, &upper_value)
          && lower_value <= upper_value;
    case kCondGT:
      return IsAtLeast(lower_expr, &lower_value)
          && IsAtMost(upper_expr, &upper_value)
          && lower_value > upper_value;
    case kCondGE:
      return IsAtLeast(lower_expr, &lower_value)
          && IsAtMost(upper_expr, &upper_value)
          && lower_value >= upper_value;
    default:
      LOG(FATAL) << "CONDITION UNREACHABLE";
  }
  return false;  // not certain, may be untaken
}

bool HInductionVarAnalysis::IsFinite(InductionInfo* upper_expr,
                                     int64_t stride_value,
                                     Primitive::Type type,
                                     IfCondition cmp) {
  const int64_t min = Primitive::MinValueOfIntegralType(type);
  const int64_t max = Primitive::MaxValueOfIntegralType(type);
  // Some rules under which it is certain at compile-time that the loop is finite.
  int64_t value;
  switch (cmp) {
    case kCondLT:
      return stride_value == 1 ||
          (IsAtMost(upper_expr, &value) && value <= (max - stride_value + 1));
    case kCondLE:
      return (IsAtMost(upper_expr, &value) && value <= (max - stride_value));
    case kCondGT:
      return stride_value == -1 ||
          (IsAtLeast(upper_expr, &value) && value >= (min - stride_value - 1));
    case kCondGE:
      return (IsAtLeast(upper_expr, &value) && value >= (min - stride_value));
    default:
      LOG(FATAL) << "CONDITION UNREACHABLE";
  }
  return false;  // not certain, may be infinite
}

bool HInductionVarAnalysis::FitsNarrowerControl(InductionInfo* lower_expr,
                                                InductionInfo* upper_expr,
                                                int64_t stride_value,
                                                Primitive::Type type,
                                                IfCondition cmp) {
  int64_t min = Primitive::MinValueOfIntegralType(type);
  int64_t max = Primitive::MaxValueOfIntegralType(type);
  // Inclusive test need one extra.
  if (stride_value != 1 && stride_value != -1) {
    return false;  // non-unit stride
  } else if (cmp == kCondLE) {
    max--;
  } else if (cmp == kCondGE) {
    min++;
  }
  // Do both bounds fit the range?
  // Note: The `value` is initialized to please valgrind - the compiler can reorder
  // the return value check with the `value` check, b/27651442 .
  int64_t value = 0;
  return IsAtLeast(lower_expr, &value) && value >= min &&
         IsAtMost(lower_expr, &value)  && value <= max &&
         IsAtLeast(upper_expr, &value) && value >= min &&
         IsAtMost(upper_expr, &value)  && value <= max;
}

void HInductionVarAnalysis::AssignInfo(HLoopInformation* loop,
                                       HInstruction* instruction,
                                       InductionInfo* info) {
  auto it = induction_.find(loop);
  if (it == induction_.end()) {
    it = induction_.Put(loop,
                        ArenaSafeMap<HInstruction*, InductionInfo*>(
                            std::less<HInstruction*>(),
                            graph_->GetArena()->Adapter(kArenaAllocInductionVarAnalysis)));
  }
  it->second.Put(instruction, info);
}

HInductionVarAnalysis::InductionInfo* HInductionVarAnalysis::LookupInfo(HLoopInformation* loop,
                                                                        HInstruction* instruction) {
  auto it = induction_.find(loop);
  if (it != induction_.end()) {
    auto loop_it = it->second.find(instruction);
    if (loop_it != it->second.end()) {
      return loop_it->second;
    }
  }
  if (loop->IsDefinedOutOfTheLoop(instruction)) {
    InductionInfo* info = CreateInvariantFetch(instruction);
    AssignInfo(loop, instruction, info);
    return info;
  }
  return nullptr;
}

HInductionVarAnalysis::InductionInfo* HInductionVarAnalysis::CreateConstant(int64_t value,
                                                                            Primitive::Type type) {
  if (type == Primitive::kPrimInt) {
    return CreateInvariantFetch(graph_->GetIntConstant(value));
  }
  DCHECK_EQ(type, Primitive::kPrimLong);
  return CreateInvariantFetch(graph_->GetLongConstant(value));
}

HInductionVarAnalysis::InductionInfo* HInductionVarAnalysis::CreateSimplifiedInvariant(
    InductionOp op,
    InductionInfo* a,
    InductionInfo* b) {
  // Perform some light-weight simplifications during construction of a new invariant.
  // This often safes memory and yields a more concise representation of the induction.
  // More exhaustive simplifications are done by later phases once induction nodes are
  // translated back into HIR code (e.g. by loop optimizations or BCE).
  int64_t value = -1;
  if (IsExact(a, &value)) {
    if (value == 0) {
      // Simplify 0 + b = b, 0 * b = 0.
      if (op == kAdd) {
        return b;
      } else if (op == kMul) {
        return a;
      }
    } else if (op == kMul) {
      // Simplify 1 * b = b, -1 * b = -b
      if (value == 1) {
        return b;
      } else if (value == -1) {
        return CreateSimplifiedInvariant(kNeg, nullptr, b);
      }
    }
  }
  if (IsExact(b, &value)) {
    if (value == 0) {
      // Simplify a + 0 = a, a - 0 = a, a * 0 = 0, -0 = 0.
      if (op == kAdd || op == kSub) {
        return a;
      } else if (op == kMul || op == kNeg) {
        return b;
      }
    } else if (op == kMul || op == kDiv) {
      // Simplify a * 1 = a, a / 1 = a, a * -1 = -a, a / -1 = -a
      if (value == 1) {
        return a;
      } else if (value == -1) {
        return CreateSimplifiedInvariant(kNeg, nullptr, a);
      }
    }
  } else if (b->operation == kNeg) {
    // Simplify a + (-b) = a - b, a - (-b) = a + b, -(-b) = b.
    if (op == kAdd) {
      return CreateSimplifiedInvariant(kSub, a, b->op_b);
    } else if (op == kSub) {
      return CreateSimplifiedInvariant(kAdd, a, b->op_b);
    } else if (op == kNeg) {
      return b->op_b;
    }
  } else if (b->operation == kSub) {
    // Simplify - (a - b) = b - a.
    if (op == kNeg) {
      return CreateSimplifiedInvariant(kSub, b->op_b, b->op_a);
    }
  }
  return new (graph_->GetArena()) InductionInfo(kInvariant, op, a, b, nullptr, b->type);
}

bool HInductionVarAnalysis::IsExact(InductionInfo* info, int64_t* value) {
  return InductionVarRange(this).IsConstant(info, InductionVarRange::kExact, value);
}

bool HInductionVarAnalysis::IsAtMost(InductionInfo* info, int64_t* value) {
  return InductionVarRange(this).IsConstant(info, InductionVarRange::kAtMost, value);
}

bool HInductionVarAnalysis::IsAtLeast(InductionInfo* info, int64_t* value) {
  return InductionVarRange(this).IsConstant(info, InductionVarRange::kAtLeast, value);
}

bool HInductionVarAnalysis::InductionEqual(InductionInfo* info1,
                                           InductionInfo* info2) {
  // Test structural equality only, without accounting for simplifications.
  if (info1 != nullptr && info2 != nullptr) {
    return
        info1->induction_class == info2->induction_class &&
        info1->operation       == info2->operation       &&
        info1->fetch           == info2->fetch           &&
        info1->type            == info2->type            &&
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
        case kNop:   inv += " @ ";  break;
        case kAdd:   inv += " + ";  break;
        case kSub:
        case kNeg:   inv += " - ";  break;
        case kMul:   inv += " * ";  break;
        case kDiv:   inv += " / ";  break;
        case kLT:    inv += " < ";  break;
        case kLE:    inv += " <= "; break;
        case kGT:    inv += " > ";  break;
        case kGE:    inv += " >= "; break;
        case kFetch:
          DCHECK(info->fetch);
          if (info->fetch->IsIntConstant()) {
            inv += std::to_string(info->fetch->AsIntConstant()->GetValue());
          } else if (info->fetch->IsLongConstant()) {
            inv += std::to_string(info->fetch->AsLongConstant()->GetValue());
          } else {
            inv += std::to_string(info->fetch->GetId()) + ":" + info->fetch->DebugName();
          }
          break;
        case kTripCountInLoop:       inv += " (TC-loop) ";        break;
        case kTripCountInBody:       inv += " (TC-body) ";        break;
        case kTripCountInLoopUnsafe: inv += " (TC-loop-unsafe) "; break;
        case kTripCountInBodyUnsafe: inv += " (TC-body-unsafe) "; break;
      }
      inv += InductionToString(info->op_b);
      inv += ")";
      return inv;
    } else {
      DCHECK(info->operation == kNop);
      if (info->induction_class == kLinear) {
        return "(" + InductionToString(info->op_a) + " * i + " +
                     InductionToString(info->op_b) + "):" +
                     Primitive::PrettyDescriptor(info->type);
      } else if (info->induction_class == kWrapAround) {
        return "wrap(" + InductionToString(info->op_a) + ", " +
                         InductionToString(info->op_b) + "):" +
                         Primitive::PrettyDescriptor(info->type);
      } else if (info->induction_class == kPeriodic) {
        return "periodic(" + InductionToString(info->op_a) + ", " +
                             InductionToString(info->op_b) + "):" +
                             Primitive::PrettyDescriptor(info->type);
      }
    }
  }
  return "";
}

}  // namespace art
