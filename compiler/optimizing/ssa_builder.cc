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

#include "ssa_builder.h"

#include "nodes.h"
#include "primitive_type_propagation.h"
#include "ssa_phi_elimination.h"

namespace art {

/**
 * A debuggable application may require to reviving phis, to ensure their
 * associated DEX register is available to a debugger. This class implements
 * the logic for statement (c) of the SsaBuilder (see ssa_builder.h). It
 * also makes sure that phis with incompatible input types are not revived
 * (statement (b) of the SsaBuilder).
 *
 * This phase must be run after detecting dead phis through the
 * DeadPhiElimination phase, and before deleting the dead phis.
 */
class DeadPhiHandling : public ValueObject {
 public:
  explicit DeadPhiHandling(HGraph* graph)
      : graph_(graph), worklist_(graph->GetArena(), kDefaultWorklistSize) {}

  void Run();

 private:
  void VisitBasicBlock(HBasicBlock* block);
  void ProcessWorklist();
  void AddToWorklist(HPhi* phi);
  void AddDependentInstructionsToWorklist(HPhi* phi);
  bool UpdateType(HPhi* phi);

  HGraph* const graph_;
  GrowableArray<HPhi*> worklist_;

  static constexpr size_t kDefaultWorklistSize = 8;

  DISALLOW_COPY_AND_ASSIGN(DeadPhiHandling);
};

bool DeadPhiHandling::UpdateType(HPhi* phi) {
  Primitive::Type existing = phi->GetType();
  DCHECK(phi->IsLive());

  bool conflict = false;
  Primitive::Type new_type = existing;
  for (size_t i = 0, e = phi->InputCount(); i < e; ++i) {
    HInstruction* input = phi->InputAt(i);
    if (input->IsPhi() && input->AsPhi()->IsDead()) {
      // We are doing a reverse post order visit of the graph, reviving
      // phis that have environment uses and updating their types. If an
      // input is a phi, and it is dead (because its input types are
      // conflicting), this phi must be marked dead as well.
      conflict = true;
      break;
    }
    Primitive::Type input_type = HPhi::ToPhiType(input->GetType());

    // The only acceptable transitions are:
    // - From void to typed: first time we update the type of this phi.
    // - From int to reference (or reference to int): the phi has to change
    //   to reference type. If the integer input cannot be converted to a
    //   reference input, the phi will remain dead.
    if (new_type == Primitive::kPrimVoid) {
      new_type = input_type;
    } else if (new_type == Primitive::kPrimNot && input_type == Primitive::kPrimInt) {
      HInstruction* equivalent = SsaBuilder::GetReferenceTypeEquivalent(input);
      if (equivalent == nullptr) {
        conflict = true;
        break;
      } else {
        phi->ReplaceInput(equivalent, i);
        if (equivalent->IsPhi()) {
          DCHECK_EQ(equivalent->GetType(), Primitive::kPrimNot);
          // We created a new phi, but that phi has the same inputs as the old phi. We
          // add it to the worklist to ensure its inputs can also be converted to reference.
          // If not, it will remain dead, and the algorithm will make the current phi dead
          // as well.
          equivalent->AsPhi()->SetLive();
          AddToWorklist(equivalent->AsPhi());
        }
      }
    } else if (new_type == Primitive::kPrimInt && input_type == Primitive::kPrimNot) {
      new_type = Primitive::kPrimNot;
      // Start over, we may request reference equivalents for the inputs of the phi.
      i = -1;
    } else if (new_type != input_type) {
      conflict = true;
      break;
    }
  }

  if (conflict) {
    phi->SetType(Primitive::kPrimVoid);
    phi->SetDead();
    return true;
  } else {
    DCHECK(phi->IsLive());
    phi->SetType(new_type);
    return existing != new_type;
  }
}

void DeadPhiHandling::VisitBasicBlock(HBasicBlock* block) {
  for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
    HPhi* phi = it.Current()->AsPhi();
    if (phi->IsDead() && phi->HasEnvironmentUses()) {
      phi->SetLive();
      if (block->IsLoopHeader()) {
        // Give a type to the loop phi, to guarantee convergence of the algorithm.
        phi->SetType(phi->InputAt(0)->GetType());
        AddToWorklist(phi);
      } else {
        // Because we are doing a reverse post order visit, all inputs of
        // this phi have been visited and therefore had their (initial) type set.
        UpdateType(phi);
      }
    }
  }
}

void DeadPhiHandling::ProcessWorklist() {
  while (!worklist_.IsEmpty()) {
    HPhi* instruction = worklist_.Pop();
    // Note that the same equivalent phi can be added multiple times in the work list, if
    // used by multiple phis. The first call to `UpdateType` will know whether the phi is
    // dead or live.
    if (instruction->IsLive() && UpdateType(instruction)) {
      AddDependentInstructionsToWorklist(instruction);
    }
  }
}

void DeadPhiHandling::AddToWorklist(HPhi* instruction) {
  DCHECK(instruction->IsLive());
  worklist_.Add(instruction);
}

void DeadPhiHandling::AddDependentInstructionsToWorklist(HPhi* instruction) {
  for (HUseIterator<HInstruction*> it(instruction->GetUses()); !it.Done(); it.Advance()) {
    HPhi* phi = it.Current()->GetUser()->AsPhi();
    if (phi != nullptr && !phi->IsDead()) {
      AddToWorklist(phi);
    }
  }
}

void DeadPhiHandling::Run() {
  for (HReversePostOrderIterator it(*graph_); !it.Done(); it.Advance()) {
    VisitBasicBlock(it.Current());
  }
  ProcessWorklist();
}

static bool IsPhiEquivalentOf(HInstruction* instruction, HPhi* phi) {
  return instruction != nullptr
      && instruction->IsPhi()
      && instruction->AsPhi()->GetRegNumber() == phi->GetRegNumber();
}

void SsaBuilder::FixNullConstantType() {
  // The order doesn't matter here.
  for (HReversePostOrderIterator itb(*GetGraph()); !itb.Done(); itb.Advance()) {
    for (HInstructionIterator it(itb.Current()->GetInstructions()); !it.Done(); it.Advance()) {
      HInstruction* equality_instr = it.Current();
      if (!equality_instr->IsEqual() && !equality_instr->IsNotEqual()) {
        continue;
      }
      HInstruction* left = equality_instr->InputAt(0);
      HInstruction* right = equality_instr->InputAt(1);
      HInstruction* int_operand = nullptr;

      if ((left->GetType() == Primitive::kPrimNot) && (right->GetType() == Primitive::kPrimInt)) {
        int_operand = right;
      } else if ((right->GetType() == Primitive::kPrimNot)
                 && (left->GetType() == Primitive::kPrimInt)) {
        int_operand = left;
      } else {
        continue;
      }

      // If we got here, we are comparing against a reference and the int constant
      // should be replaced with a null constant.
      // Both type propagation and redundant phi elimination ensure `int_operand`
      // can only be the 0 constant.
      DCHECK(int_operand->IsIntConstant());
      DCHECK_EQ(0, int_operand->AsIntConstant()->GetValue());
      equality_instr->ReplaceInput(GetGraph()->GetNullConstant(), int_operand == right ? 1 : 0);
    }
  }
}

void SsaBuilder::EquivalentPhisCleanup() {
  // The order doesn't matter here.
  for (HReversePostOrderIterator itb(*GetGraph()); !itb.Done(); itb.Advance()) {
    for (HInstructionIterator it(itb.Current()->GetPhis()); !it.Done(); it.Advance()) {
      HPhi* phi = it.Current()->AsPhi();
      HPhi* next = phi->GetNextEquivalentPhiWithSameType();
      if (next != nullptr) {
        // Make sure we do not replace a live phi with a dead phi. A live phi has been
        // handled by the type propagation phase, unlike a dead phi.
        if (next->IsLive()) {
          phi->ReplaceWith(next);
        } else {
          next->ReplaceWith(phi);
        }
        DCHECK(next->GetNextEquivalentPhiWithSameType() == nullptr)
            << "More then one phi equivalent with type " << phi->GetType()
            << " found for phi" << phi->GetId();
      }
    }
  }
}

void SsaBuilder::BuildSsa() {
  // 1) Visit in reverse post order. We need to have all predecessors of a block visited
  // (with the exception of loops) in order to create the right environment for that
  // block. For loops, we create phis whose inputs will be set in 2).
  for (HReversePostOrderIterator it(*GetGraph()); !it.Done(); it.Advance()) {
    VisitBasicBlock(it.Current());
  }

  // 2) Set inputs of loop phis.
  for (size_t i = 0; i < loop_headers_.Size(); i++) {
    HBasicBlock* block = loop_headers_.Get(i);
    for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
      HPhi* phi = it.Current()->AsPhi();
      for (size_t pred = 0; pred < block->GetPredecessors().Size(); pred++) {
        HInstruction* input = ValueOfLocal(block->GetPredecessors().Get(pred), phi->GetRegNumber());
        phi->AddInput(input);
      }
    }
  }

  // 3) Mark dead phis. This will mark phis that are only used by environments:
  // at the DEX level, the type of these phis does not need to be consistent, but
  // our code generator will complain if the inputs of a phi do not have the same
  // type. The marking allows the type propagation to know which phis it needs
  // to handle. We mark but do not eliminate: the elimination will be done in
  // step 9).
  SsaDeadPhiElimination dead_phis_for_type_propagation(GetGraph());
  dead_phis_for_type_propagation.MarkDeadPhis();

  // 4) Propagate types of phis. At this point, phis are typed void in the general
  // case, or float/double/reference when we created an equivalent phi. So we
  // need to propagate the types across phis to give them a correct type.
  PrimitiveTypePropagation type_propagation(GetGraph());
  type_propagation.Run();

  // 5) When creating equivalent phis we copy the inputs of the original phi which
  // may be improperly typed. This was fixed during the type propagation in 4) but
  // as a result we may end up with two equivalent phis with the same type for
  // the same dex register. This pass cleans them up.
  EquivalentPhisCleanup();

  // 6) Mark dead phis again. Step 4) may have introduced new phis.
  // Step 5) might enable the death of new phis.
  SsaDeadPhiElimination dead_phis(GetGraph());
  dead_phis.MarkDeadPhis();

  // 7) Now that the graph is correctly typed, we can get rid of redundant phis.
  // Note that we cannot do this phase before type propagation, otherwise
  // we could get rid of phi equivalents, whose presence is a requirement for the
  // type propagation phase. Note that this is to satisfy statement (a) of the
  // SsaBuilder (see ssa_builder.h).
  SsaRedundantPhiElimination redundant_phi(GetGraph());
  redundant_phi.Run();

  // 8) Fix the type for null constants which are part of an equality comparison.
  // We need to do this after redundant phi elimination, to ensure the only cases
  // that we can see are reference comparison against 0. The redundant phi
  // elimination ensures we do not see a phi taking two 0 constants in a HEqual
  // or HNotEqual.
  FixNullConstantType();

  // 9) Make sure environments use the right phi "equivalent": a phi marked dead
  // can have a phi equivalent that is not dead. We must therefore update
  // all environment uses of the dead phi to use its equivalent. Note that there
  // can be multiple phis for the same Dex register that are live (for example
  // when merging constants), in which case it is OK for the environments
  // to just reference one.
  for (HReversePostOrderIterator it(*GetGraph()); !it.Done(); it.Advance()) {
    HBasicBlock* block = it.Current();
    for (HInstructionIterator it_phis(block->GetPhis()); !it_phis.Done(); it_phis.Advance()) {
      HPhi* phi = it_phis.Current()->AsPhi();
      // If the phi is not dead, or has no environment uses, there is nothing to do.
      if (!phi->IsDead() || !phi->HasEnvironmentUses()) continue;
      HInstruction* next = phi->GetNext();
      if (!IsPhiEquivalentOf(next, phi)) continue;
      if (next->AsPhi()->IsDead()) {
        // If the phi equivalent is dead, check if there is another one.
        next = next->GetNext();
        if (!IsPhiEquivalentOf(next, phi)) continue;
        // There can be at most two phi equivalents.
        DCHECK(!IsPhiEquivalentOf(next->GetNext(), phi));
        if (next->AsPhi()->IsDead()) continue;
      }
      // We found a live phi equivalent. Update the environment uses of `phi` with it.
      phi->ReplaceWith(next);
    }
  }

  // 10) Deal with phis to guarantee liveness of phis in case of a debuggable
  // application. This is for satisfying statement (c) of the SsaBuilder
  // (see ssa_builder.h).
  if (GetGraph()->IsDebuggable()) {
    DeadPhiHandling dead_phi_handler(GetGraph());
    dead_phi_handler.Run();
  }

  // 11) Now that the right phis are used for the environments, and we
  // have potentially revive dead phis in case of a debuggable application,
  // we can eliminate phis we do not need. Regardless of the debuggable status,
  // this phase is necessary for statement (b) of the SsaBuilder (see ssa_builder.h),
  // as well as for the code generation, which does not deal with phis of conflicting
  // input types.
  dead_phis.EliminateDeadPhis();

  // 12) Clear locals.
  for (HInstructionIterator it(GetGraph()->GetEntryBlock()->GetInstructions());
       !it.Done();
       it.Advance()) {
    HInstruction* current = it.Current();
    if (current->IsLocal()) {
      current->GetBlock()->RemoveInstruction(current);
    }
  }
}

HInstruction* SsaBuilder::ValueOfLocal(HBasicBlock* block, size_t local) {
  return GetLocalsFor(block)->Get(local);
}

void SsaBuilder::VisitBasicBlock(HBasicBlock* block) {
  current_locals_ = GetLocalsFor(block);

  if (block->IsLoopHeader()) {
    // If the block is a loop header, we know we only have visited the pre header
    // because we are visiting in reverse post order. We create phis for all initialized
    // locals from the pre header. Their inputs will be populated at the end of
    // the analysis.
    for (size_t local = 0; local < current_locals_->Size(); local++) {
      HInstruction* incoming = ValueOfLocal(block->GetLoopInformation()->GetPreHeader(), local);
      if (incoming != nullptr) {
        HPhi* phi = new (GetGraph()->GetArena()) HPhi(
            GetGraph()->GetArena(), local, 0, Primitive::kPrimVoid);
        block->AddPhi(phi);
        current_locals_->Put(local, phi);
      }
    }
    // Save the loop header so that the last phase of the analysis knows which
    // blocks need to be updated.
    loop_headers_.Add(block);
  } else if (block->GetPredecessors().Size() > 0) {
    // All predecessors have already been visited because we are visiting in reverse post order.
    // We merge the values of all locals, creating phis if those values differ.
    for (size_t local = 0; local < current_locals_->Size(); local++) {
      bool one_predecessor_has_no_value = false;
      bool is_different = false;
      HInstruction* value = ValueOfLocal(block->GetPredecessors().Get(0), local);

      for (size_t i = 0, e = block->GetPredecessors().Size(); i < e; ++i) {
        HInstruction* current = ValueOfLocal(block->GetPredecessors().Get(i), local);
        if (current == nullptr) {
          one_predecessor_has_no_value = true;
          break;
        } else if (current != value) {
          is_different = true;
        }
      }

      if (one_predecessor_has_no_value) {
        // If one predecessor has no value for this local, we trust the verifier has
        // successfully checked that there is a store dominating any read after this block.
        continue;
      }

      if (is_different) {
        HPhi* phi = new (GetGraph()->GetArena()) HPhi(
            GetGraph()->GetArena(), local, block->GetPredecessors().Size(), Primitive::kPrimVoid);
        for (size_t i = 0; i < block->GetPredecessors().Size(); i++) {
          HInstruction* pred_value = ValueOfLocal(block->GetPredecessors().Get(i), local);
          phi->SetRawInputAt(i, pred_value);
        }
        block->AddPhi(phi);
        value = phi;
      }
      current_locals_->Put(local, value);
    }
  }

  // Visit all instructions. The instructions of interest are:
  // - HLoadLocal: replace them with the current value of the local.
  // - HStoreLocal: update current value of the local and remove the instruction.
  // - Instructions that require an environment: populate their environment
  //   with the current values of the locals.
  for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
    it.Current()->Accept(this);
  }
}

/**
 * Constants in the Dex format are not typed. So the builder types them as
 * integers, but when doing the SSA form, we might realize the constant
 * is used for floating point operations. We create a floating-point equivalent
 * constant to make the operations correctly typed.
 */
HFloatConstant* SsaBuilder::GetFloatEquivalent(HIntConstant* constant) {
  // We place the floating point constant next to this constant.
  HFloatConstant* result = constant->GetNext()->AsFloatConstant();
  if (result == nullptr) {
    HGraph* graph = constant->GetBlock()->GetGraph();
    ArenaAllocator* allocator = graph->GetArena();
    result = new (allocator) HFloatConstant(bit_cast<float, int32_t>(constant->GetValue()));
    constant->GetBlock()->InsertInstructionBefore(result, constant->GetNext());
    graph->CacheFloatConstant(result);
  } else {
    // If there is already a constant with the expected type, we know it is
    // the floating point equivalent of this constant.
    DCHECK_EQ((bit_cast<int32_t, float>(result->GetValue())), constant->GetValue());
  }
  return result;
}

/**
 * Wide constants in the Dex format are not typed. So the builder types them as
 * longs, but when doing the SSA form, we might realize the constant
 * is used for floating point operations. We create a floating-point equivalent
 * constant to make the operations correctly typed.
 */
HDoubleConstant* SsaBuilder::GetDoubleEquivalent(HLongConstant* constant) {
  // We place the floating point constant next to this constant.
  HDoubleConstant* result = constant->GetNext()->AsDoubleConstant();
  if (result == nullptr) {
    HGraph* graph = constant->GetBlock()->GetGraph();
    ArenaAllocator* allocator = graph->GetArena();
    result = new (allocator) HDoubleConstant(bit_cast<double, int64_t>(constant->GetValue()));
    constant->GetBlock()->InsertInstructionBefore(result, constant->GetNext());
    graph->CacheDoubleConstant(result);
  } else {
    // If there is already a constant with the expected type, we know it is
    // the floating point equivalent of this constant.
    DCHECK_EQ((bit_cast<int64_t, double>(result->GetValue())), constant->GetValue());
  }
  return result;
}

/**
 * Because of Dex format, we might end up having the same phi being
 * used for non floating point operations and floating point / reference operations.
 * Because we want the graph to be correctly typed (and thereafter avoid moves between
 * floating point registers and core registers), we need to create a copy of the
 * phi with a floating point / reference type.
 */
HPhi* SsaBuilder::GetFloatDoubleOrReferenceEquivalentOfPhi(HPhi* phi, Primitive::Type type) {
  // We place the floating point /reference phi next to this phi.
  HInstruction* next = phi->GetNext();
  if (next != nullptr
      && next->AsPhi()->GetRegNumber() == phi->GetRegNumber()
      && next->GetType() != type) {
    // Move to the next phi to see if it is the one we are looking for.
    next = next->GetNext();
  }

  if (next == nullptr
      || (next->AsPhi()->GetRegNumber() != phi->GetRegNumber())
      || (next->GetType() != type)) {
    ArenaAllocator* allocator = phi->GetBlock()->GetGraph()->GetArena();
    HPhi* new_phi = new (allocator) HPhi(allocator, phi->GetRegNumber(), phi->InputCount(), type);
    for (size_t i = 0, e = phi->InputCount(); i < e; ++i) {
      // Copy the inputs. Note that the graph may not be correctly typed by doing this copy,
      // but the type propagation phase will fix it.
      new_phi->SetRawInputAt(i, phi->InputAt(i));
    }
    phi->GetBlock()->InsertPhiAfter(new_phi, phi);
    return new_phi;
  } else {
    DCHECK_EQ(next->GetType(), type);
    return next->AsPhi();
  }
}

HInstruction* SsaBuilder::GetFloatOrDoubleEquivalent(HInstruction* user,
                                                     HInstruction* value,
                                                     Primitive::Type type) {
  if (value->IsArrayGet()) {
    // The verifier has checked that values in arrays cannot be used for both
    // floating point and non-floating point operations. It is therefore safe to just
    // change the type of the operation.
    value->AsArrayGet()->SetType(type);
    return value;
  } else if (value->IsLongConstant()) {
    return GetDoubleEquivalent(value->AsLongConstant());
  } else if (value->IsIntConstant()) {
    return GetFloatEquivalent(value->AsIntConstant());
  } else if (value->IsPhi()) {
    return GetFloatDoubleOrReferenceEquivalentOfPhi(value->AsPhi(), type);
  } else {
    // For other instructions, we assume the verifier has checked that the dex format is correctly
    // typed and the value in a dex register will not be used for both floating point and
    // non-floating point operations. So the only reason an instruction would want a floating
    // point equivalent is for an unused phi that will be removed by the dead phi elimination phase.
    DCHECK(user->IsPhi());
    return value;
  }
}

HInstruction* SsaBuilder::GetReferenceTypeEquivalent(HInstruction* value) {
  if (value->IsIntConstant() && value->AsIntConstant()->GetValue() == 0) {
    return value->GetBlock()->GetGraph()->GetNullConstant();
  } else if (value->IsPhi()) {
    return GetFloatDoubleOrReferenceEquivalentOfPhi(value->AsPhi(), Primitive::kPrimNot);
  } else {
    return nullptr;
  }
}

void SsaBuilder::VisitLoadLocal(HLoadLocal* load) {
  HInstruction* value = current_locals_->Get(load->GetLocal()->GetRegNumber());
  // If the operation requests a specific type, we make sure its input is of that type.
  if (load->GetType() != value->GetType()) {
    if (load->GetType() == Primitive::kPrimFloat || load->GetType() == Primitive::kPrimDouble) {
      value = GetFloatOrDoubleEquivalent(load, value, load->GetType());
    } else if (load->GetType() == Primitive::kPrimNot) {
      value = GetReferenceTypeEquivalent(value);
    }
  }
  load->ReplaceWith(value);
  load->GetBlock()->RemoveInstruction(load);
}

void SsaBuilder::VisitStoreLocal(HStoreLocal* store) {
  current_locals_->Put(store->GetLocal()->GetRegNumber(), store->InputAt(1));
  store->GetBlock()->RemoveInstruction(store);
}

void SsaBuilder::VisitInstruction(HInstruction* instruction) {
  if (!instruction->NeedsEnvironment()) {
    return;
  }
  HEnvironment* environment = new (GetGraph()->GetArena()) HEnvironment(
      GetGraph()->GetArena(),
      current_locals_->Size(),
      GetGraph()->GetDexFile(),
      GetGraph()->GetMethodIdx(),
      instruction->GetDexPc());
  environment->CopyFrom(*current_locals_);
  instruction->SetRawEnvironment(environment);
}

void SsaBuilder::VisitTemporary(HTemporary* temp) {
  // Temporaries are only used by the baseline register allocator.
  temp->GetBlock()->RemoveInstruction(temp);
}

}  // namespace art
