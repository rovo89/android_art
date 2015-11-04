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

void SsaBuilder::SetLoopPhiInputs() {
  for (HBasicBlock* block : loop_headers_) {
    for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
      HPhi* phi = it.Current()->AsPhi();
      for (HBasicBlock* predecessor : block->GetPredecessors()) {
        HInstruction* input = ValueOfLocal(predecessor, phi->GetRegNumber());
        phi->AddInput(input);
      }
    }
  }
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
        // Make sure we do not replace a live phi with a dead phi. A live phi
        // has been handled by the type propagation phase, unlike a dead phi.
        if (next->IsLive()) {
          phi->ReplaceWith(next);
          phi->SetDead();
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

void SsaBuilder::FixEnvironmentPhis() {
  for (HReversePostOrderIterator it(*GetGraph()); !it.Done(); it.Advance()) {
    HBasicBlock* block = it.Current();
    for (HInstructionIterator it_phis(block->GetPhis()); !it_phis.Done(); it_phis.Advance()) {
      HPhi* phi = it_phis.Current()->AsPhi();
      // If the phi is not dead, or has no environment uses, there is nothing to do.
      if (!phi->IsDead() || !phi->HasEnvironmentUses()) continue;
      HInstruction* next = phi->GetNext();
      if (!phi->IsVRegEquivalentOf(next)) continue;
      if (next->AsPhi()->IsDead()) {
        // If the phi equivalent is dead, check if there is another one.
        next = next->GetNext();
        if (!phi->IsVRegEquivalentOf(next)) continue;
        // There can be at most two phi equivalents.
        DCHECK(!phi->IsVRegEquivalentOf(next->GetNext()));
        if (next->AsPhi()->IsDead()) continue;
      }
      // We found a live phi equivalent. Update the environment uses of `phi` with it.
      phi->ReplaceWith(next);
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
  SetLoopPhiInputs();

  // 3) Propagate types of phis. At this point, phis are typed void in the general
  // case, or float/double/reference if we created an equivalent phi. So we need
  // to propagate the types across phis to give them a correct type. If a type
  // conflict is detected in this stage, the phi is marked dead.
  PrimitiveTypePropagation(GetGraph()).Run();

  // 4) When creating equivalent phis we copy the inputs of the original phi which
  // may be improperly typed. This was fixed during the type propagation in 4) but
  // as a result we may end up with two equivalent phis with the same type for
  // the same dex register. This pass cleans them up.
  EquivalentPhisCleanup();

  // 5) Mark dead phis. This will mark phis which are not used by instructions or
  // other live phis. If compiling as debuggable code, phis will also be kept live
  // if they have an environment use.
  SsaDeadPhiElimination dead_phis(GetGraph());
  dead_phis.MarkDeadPhis();

  // 6) Make sure environments use the right phi equivalent: a phi marked dead
  // can have a phi equivalent that is not dead. In that case we have to replace
  // it with the live equivalent because deoptimization and try/catch rely on
  // environments containing values of all live vregs at that point. Note that
  // there can be multiple phis for the same Dex register that are live
  // (for example when merging constants), in which case it is okay for the
  // environments to just reference one.
  FixEnvironmentPhis();

  // 7) Now that the right phis are used for the environments, we can eliminate
  // phis we do not need. Regardless of the debuggable status, this phase is
  /// necessary for statement (b) of the SsaBuilder (see ssa_builder.h), as well
  // as for the code generation, which does not deal with phis of conflicting
  // input types.
  dead_phis.EliminateDeadPhis();

  // 8) Now that the graph is correctly typed, we can get rid of redundant phis.
  // Note that we cannot do this phase before type propagation, otherwise
  // we could get rid of phi equivalents, whose presence is a requirement for the
  // type propagation phase. Note that this is to satisfy statement (a) of the
  // SsaBuilder (see ssa_builder.h).
  SsaRedundantPhiElimination(GetGraph()).Run();

  // 9) Fix the type for null constants which are part of an equality comparison.
  // We need to do this after redundant phi elimination, to ensure the only cases
  // that we can see are reference comparison against 0. The redundant phi
  // elimination ensures we do not see a phi taking two 0 constants in a HEqual
  // or HNotEqual.
  FixNullConstantType();

  // 10) Clear locals.
  for (HInstructionIterator it(GetGraph()->GetEntryBlock()->GetInstructions());
       !it.Done();
       it.Advance()) {
    HInstruction* current = it.Current();
    if (current->IsLocal()) {
      current->GetBlock()->RemoveInstruction(current);
    }
  }
}

ArenaVector<HInstruction*>* SsaBuilder::GetLocalsFor(HBasicBlock* block) {
  ArenaVector<HInstruction*>* locals = &locals_for_[block->GetBlockId()];
  const size_t vregs = GetGraph()->GetNumberOfVRegs();
  if (locals->empty() && vregs != 0u) {
    locals->resize(vregs, nullptr);

    if (block->IsCatchBlock()) {
      ArenaAllocator* arena = GetGraph()->GetArena();
      // We record incoming inputs of catch phis at throwing instructions and
      // must therefore eagerly create the phis. Phis for undefined vregs will
      // be deleted when the first throwing instruction with the vreg undefined
      // is encountered. Unused phis will be removed by dead phi analysis.
      for (size_t i = 0; i < vregs; ++i) {
        // No point in creating the catch phi if it is already undefined at
        // the first throwing instruction.
        if ((*current_locals_)[i] != nullptr) {
          HPhi* phi = new (arena) HPhi(arena, i, 0, Primitive::kPrimVoid);
          block->AddPhi(phi);
          (*locals)[i] = phi;
        }
      }
    }
  }
  return locals;
}

HInstruction* SsaBuilder::ValueOfLocal(HBasicBlock* block, size_t local) {
  ArenaVector<HInstruction*>* locals = GetLocalsFor(block);
  return (*locals)[local];
}

void SsaBuilder::VisitBasicBlock(HBasicBlock* block) {
  current_locals_ = GetLocalsFor(block);

  if (block->IsCatchBlock()) {
    // Catch phis were already created and inputs collected from throwing sites.
    if (kIsDebugBuild) {
      // Make sure there was at least one throwing instruction which initialized
      // locals (guaranteed by HGraphBuilder) and that all try blocks have been
      // visited already (from HTryBoundary scoping and reverse post order).
      bool throwing_instruction_found = false;
      bool catch_block_visited = false;
      for (HReversePostOrderIterator it(*GetGraph()); !it.Done(); it.Advance()) {
        HBasicBlock* current = it.Current();
        if (current == block) {
          catch_block_visited = true;
        } else if (current->IsTryBlock() &&
                   current->GetTryCatchInformation()->GetTryEntry().HasExceptionHandler(*block)) {
          DCHECK(!catch_block_visited) << "Catch block visited before its try block.";
          throwing_instruction_found |= current->HasThrowingInstructions();
        }
      }
      DCHECK(throwing_instruction_found) << "No instructions throwing into a live catch block.";
    }
  } else if (block->IsLoopHeader()) {
    // If the block is a loop header, we know we only have visited the pre header
    // because we are visiting in reverse post order. We create phis for all initialized
    // locals from the pre header. Their inputs will be populated at the end of
    // the analysis.
    for (size_t local = 0; local < current_locals_->size(); ++local) {
      HInstruction* incoming = ValueOfLocal(block->GetLoopInformation()->GetPreHeader(), local);
      if (incoming != nullptr) {
        HPhi* phi = new (GetGraph()->GetArena()) HPhi(
            GetGraph()->GetArena(), local, 0, Primitive::kPrimVoid);
        block->AddPhi(phi);
        (*current_locals_)[local] = phi;
      }
    }
    // Save the loop header so that the last phase of the analysis knows which
    // blocks need to be updated.
    loop_headers_.push_back(block);
  } else if (block->GetPredecessors().size() > 0) {
    // All predecessors have already been visited because we are visiting in reverse post order.
    // We merge the values of all locals, creating phis if those values differ.
    for (size_t local = 0; local < current_locals_->size(); ++local) {
      bool one_predecessor_has_no_value = false;
      bool is_different = false;
      HInstruction* value = ValueOfLocal(block->GetPredecessors()[0], local);

      for (HBasicBlock* predecessor : block->GetPredecessors()) {
        HInstruction* current = ValueOfLocal(predecessor, local);
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
            GetGraph()->GetArena(), local, block->GetPredecessors().size(), Primitive::kPrimVoid);
        for (size_t i = 0; i < block->GetPredecessors().size(); i++) {
          HInstruction* pred_value = ValueOfLocal(block->GetPredecessors()[i], local);
          phi->SetRawInputAt(i, pred_value);
        }
        block->AddPhi(phi);
        value = phi;
      }
      (*current_locals_)[local] = value;
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
  DCHECK(phi->IsLive()) << "Cannot get equivalent of a dead phi since it would create a live one.";

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
      // Copy the inputs. Note that the graph may not be correctly typed
      // by doing this copy, but the type propagation phase will fix it.
      new_phi->SetRawInputAt(i, phi->InputAt(i));
    }
    phi->GetBlock()->InsertPhiAfter(new_phi, phi);
    DCHECK(new_phi->IsLive());
    return new_phi;
  } else {
    DCHECK_EQ(next->GetType(), type);
    // An existing equivalent was found. If it is dead, conflict was previously
    // identified and we return nullptr instead.
    return next->AsPhi()->IsLive() ? next->AsPhi() : nullptr;
  }
}

HInstruction* SsaBuilder::GetFloatOrDoubleEquivalent(HInstruction* user,
                                                     HInstruction* value,
                                                     Primitive::Type type) {
  if (value->IsArrayGet()) {
    HArrayGet* aget = value->AsArrayGet();
    if (aget->GetType() != type && aget->IsTypeFixed()) {
      // Requested a float/double equivalent of ArrayGet with int/long uses.
      // Must be a phi with type conflict.
      DCHECK(user->IsPhi());
      return nullptr;
    }
    aget->SetType(type);
    return aget;
  } else if (value->IsLongConstant()) {
    return GetDoubleEquivalent(value->AsLongConstant());
  } else if (value->IsIntConstant()) {
    return GetFloatEquivalent(value->AsIntConstant());
  } else if (value->IsPhi()) {
    return GetFloatDoubleOrReferenceEquivalentOfPhi(value->AsPhi(), type);
  } else {
    return nullptr;
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
  HInstruction* value = (*current_locals_)[load->GetLocal()->GetRegNumber()];
  // If the operation requests a specific type, we make sure its input is of that type.
  if (load->GetType() != value->GetType()) {
    if (load->GetType() == Primitive::kPrimFloat || load->GetType() == Primitive::kPrimDouble) {
      value = GetFloatOrDoubleEquivalent(load, value, load->GetType());
    } else if (load->GetType() == Primitive::kPrimNot) {
      value = GetReferenceTypeEquivalent(value);
    }
  }

  // If value is HArrayGet, check if uses of the HLoadLocal disambiguate its
  // type between int/long and float/double.
  if (value->IsArrayGet() && !value->AsArrayGet()->IsTypeFixed()) {
    for (HUseIterator<HInstruction*> use_it(load->GetUses()); !use_it.Done(); use_it.Advance()) {
      HInstruction* user = use_it.Current()->GetUser();
      if (!user->IsStoreLocal() &&
          !user->IsPhi() &&
          (!user->IsArraySet() || user->AsArraySet()->GetIndex() == value)) {
        value->AsArrayGet()->FixType();
        break;
      }
    }
  }

  load->ReplaceWith(value);
  load->GetBlock()->RemoveInstruction(load);
}

void SsaBuilder::VisitStoreLocal(HStoreLocal* store) {
  (*current_locals_)[store->GetLocal()->GetRegNumber()] = store->InputAt(1);
  store->GetBlock()->RemoveInstruction(store);
}

void SsaBuilder::VisitInstruction(HInstruction* instruction) {
  if (instruction->NeedsEnvironment()) {
    HEnvironment* environment = new (GetGraph()->GetArena()) HEnvironment(
        GetGraph()->GetArena(),
        current_locals_->size(),
        GetGraph()->GetDexFile(),
        GetGraph()->GetMethodIdx(),
        instruction->GetDexPc(),
        GetGraph()->GetInvokeType(),
        instruction);
    environment->CopyFrom(*current_locals_);
    instruction->SetRawEnvironment(environment);
  }

  // If in a try block, propagate values of locals into catch blocks.
  if (instruction->CanThrowIntoCatchBlock()) {
    const HTryBoundary& try_entry =
        instruction->GetBlock()->GetTryCatchInformation()->GetTryEntry();
    for (HExceptionHandlerIterator it(try_entry); !it.Done(); it.Advance()) {
      HBasicBlock* catch_block = it.Current();
      ArenaVector<HInstruction*>* handler_locals = GetLocalsFor(catch_block);
      DCHECK_EQ(handler_locals->size(), current_locals_->size());
      for (size_t vreg = 0, e = current_locals_->size(); vreg < e; ++vreg) {
        HInstruction* handler_value = (*handler_locals)[vreg];
        if (handler_value == nullptr) {
          // Vreg was undefined at a previously encountered throwing instruction
          // and the catch phi was deleted. Do not record the local value.
          continue;
        }
        DCHECK(handler_value->IsPhi());

        HInstruction* local_value = (*current_locals_)[vreg];
        if (local_value == nullptr) {
          // This is the first instruction throwing into `catch_block` where
          // `vreg` is undefined. Delete the catch phi.
          catch_block->RemovePhi(handler_value->AsPhi());
          (*handler_locals)[vreg] = nullptr;
        } else {
          // Vreg has been defined at all instructions throwing into `catch_block`
          // encountered so far. Record the local value in the catch phi.
          handler_value->AsPhi()->AddInput(local_value);
        }
      }
    }
  }
}

void SsaBuilder::VisitTemporary(HTemporary* temp) {
  // Temporaries are only used by the baseline register allocator.
  temp->GetBlock()->RemoveInstruction(temp);
}

}  // namespace art
