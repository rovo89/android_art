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
#include "reference_type_propagation.h"
#include "ssa_phi_elimination.h"

namespace art {

void SsaBuilder::SetLoopHeaderPhiInputs() {
  for (size_t i = loop_headers_.size(); i > 0; --i) {
    HBasicBlock* block = loop_headers_[i - 1];
    for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
      HPhi* phi = it.Current()->AsPhi();
      size_t vreg = phi->GetRegNumber();
      for (HBasicBlock* predecessor : block->GetPredecessors()) {
        HInstruction* value = ValueOfLocal(predecessor, vreg);
        if (value == nullptr) {
          // Vreg is undefined at this predecessor. Mark it dead and leave with
          // fewer inputs than predecessors. SsaChecker will fail if not removed.
          phi->SetDead();
          break;
        } else {
          phi->AddInput(value);
        }
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
      DCHECK(int_operand->IsIntConstant()) << int_operand->DebugName();
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

static void AddDependentInstructionsToWorklist(HInstruction* instruction,
                                               ArenaVector<HPhi*>* worklist) {
  // If `instruction` is a dead phi, type conflict was just identified. All its
  // live phi users, and transitively users of those users, therefore need to be
  // marked dead/conflicting too, so we add them to the worklist. Otherwise we
  // add users whose type does not match and needs to be updated.
  bool add_all_live_phis = instruction->IsPhi() && instruction->AsPhi()->IsDead();
  for (HUseIterator<HInstruction*> it(instruction->GetUses()); !it.Done(); it.Advance()) {
    HInstruction* user = it.Current()->GetUser();
    if (user->IsPhi() && user->AsPhi()->IsLive()) {
      if (add_all_live_phis || user->GetType() != instruction->GetType()) {
        worklist->push_back(user->AsPhi());
      }
    }
  }
}

// Find a candidate primitive type for `phi` by merging the type of its inputs.
// Return false if conflict is identified.
static bool TypePhiFromInputs(HPhi* phi) {
  Primitive::Type common_type = phi->GetType();

  for (HInputIterator it(phi); !it.Done(); it.Advance()) {
    HInstruction* input = it.Current();
    if (input->IsPhi() && input->AsPhi()->IsDead()) {
      // Phis are constructed live so if an input is a dead phi, it must have
      // been made dead due to type conflict. Mark this phi conflicting too.
      return false;
    }

    Primitive::Type input_type = HPhi::ToPhiType(input->GetType());
    if (common_type == input_type) {
      // No change in type.
    } else if (Primitive::Is64BitType(common_type) != Primitive::Is64BitType(input_type)) {
      // Types are of different sizes, e.g. int vs. long. Must be a conflict.
      return false;
    } else if (Primitive::IsIntegralType(common_type)) {
      // Previous inputs were integral, this one is not but is of the same size.
      // This does not imply conflict since some bytecode instruction types are
      // ambiguous. TypeInputsOfPhi will either type them or detect a conflict.
      DCHECK(Primitive::IsFloatingPointType(input_type) || input_type == Primitive::kPrimNot);
      common_type = input_type;
    } else if (Primitive::IsIntegralType(input_type)) {
      // Input is integral, common type is not. Same as in the previous case, if
      // there is a conflict, it will be detected during TypeInputsOfPhi.
      DCHECK(Primitive::IsFloatingPointType(common_type) || common_type == Primitive::kPrimNot);
    } else {
      // Combining float and reference types. Clearly a conflict.
      DCHECK((common_type == Primitive::kPrimFloat && input_type == Primitive::kPrimNot) ||
             (common_type == Primitive::kPrimNot && input_type == Primitive::kPrimFloat));
      return false;
    }
  }

  // We have found a candidate type for the phi. Set it and return true. We may
  // still discover conflict whilst typing the individual inputs in TypeInputsOfPhi.
  phi->SetType(common_type);
  return true;
}

// Replace inputs of `phi` to match its type. Return false if conflict is identified.
bool SsaBuilder::TypeInputsOfPhi(HPhi* phi, ArenaVector<HPhi*>* worklist) {
  Primitive::Type common_type = phi->GetType();
  if (common_type == Primitive::kPrimVoid || Primitive::IsIntegralType(common_type)) {
    // Phi either contains only other untyped phis (common_type == kPrimVoid),
    // or `common_type` is integral and we do not need to retype ambiguous inputs
    // because they are always constructed with the integral type candidate.
    if (kIsDebugBuild) {
      for (size_t i = 0, e = phi->InputCount(); i < e; ++i) {
        HInstruction* input = phi->InputAt(i);
        if (common_type == Primitive::kPrimVoid) {
          DCHECK(input->IsPhi() && input->GetType() == Primitive::kPrimVoid);
        } else {
          DCHECK((input->IsPhi() && input->GetType() == Primitive::kPrimVoid) ||
                 HPhi::ToPhiType(input->GetType()) == common_type);
        }
      }
    }
    // Inputs did not need to be replaced, hence no conflict. Report success.
    return true;
  } else {
    DCHECK(common_type == Primitive::kPrimNot || Primitive::IsFloatingPointType(common_type));
    for (size_t i = 0, e = phi->InputCount(); i < e; ++i) {
      HInstruction* input = phi->InputAt(i);
      if (input->GetType() != common_type) {
        // Input type does not match phi's type. Try to retype the input or
        // generate a suitably typed equivalent.
        HInstruction* equivalent = (common_type == Primitive::kPrimNot)
            ? GetReferenceTypeEquivalent(input)
            : GetFloatOrDoubleEquivalent(input, common_type);
        if (equivalent == nullptr) {
          // Input could not be typed. Report conflict.
          return false;
        }
        // Make sure the input did not change its type and we do not need to
        // update its users.
        DCHECK_NE(input, equivalent);

        phi->ReplaceInput(equivalent, i);
        if (equivalent->IsPhi()) {
          worklist->push_back(equivalent->AsPhi());
        }
      }
    }
    // All inputs either matched the type of the phi or we successfully replaced
    // them with a suitable equivalent. Report success.
    return true;
  }
}

// Attempt to set the primitive type of `phi` to match its inputs. Return whether
// it was changed by the algorithm or not.
bool SsaBuilder::UpdatePrimitiveType(HPhi* phi, ArenaVector<HPhi*>* worklist) {
  DCHECK(phi->IsLive());
  Primitive::Type original_type = phi->GetType();

  // Try to type the phi in two stages:
  // (1) find a candidate type for the phi by merging types of all its inputs,
  // (2) try to type the phi's inputs to that candidate type.
  // Either of these stages may detect a type conflict and fail, in which case
  // we immediately abort.
  if (!TypePhiFromInputs(phi) || !TypeInputsOfPhi(phi, worklist)) {
    // Conflict detected. Mark the phi dead and return true because it changed.
    phi->SetDead();
    return true;
  }

  // Return true if the type of the phi has changed.
  return phi->GetType() != original_type;
}

void SsaBuilder::RunPrimitiveTypePropagation() {
  ArenaVector<HPhi*> worklist(GetGraph()->GetArena()->Adapter());

  for (HReversePostOrderIterator it(*GetGraph()); !it.Done(); it.Advance()) {
    HBasicBlock* block = it.Current();
    if (block->IsLoopHeader()) {
      for (HInstructionIterator phi_it(block->GetPhis()); !phi_it.Done(); phi_it.Advance()) {
        HPhi* phi = phi_it.Current()->AsPhi();
        if (phi->IsLive()) {
          worklist.push_back(phi);
        }
      }
    } else {
      for (HInstructionIterator phi_it(block->GetPhis()); !phi_it.Done(); phi_it.Advance()) {
        // Eagerly compute the type of the phi, for quicker convergence. Note
        // that we don't need to add users to the worklist because we are
        // doing a reverse post-order visit, therefore either the phi users are
        // non-loop phi and will be visited later in the visit, or are loop-phis,
        // and they are already in the work list.
        HPhi* phi = phi_it.Current()->AsPhi();
        if (phi->IsLive()) {
          UpdatePrimitiveType(phi, &worklist);
        }
      }
    }
  }

  ProcessPrimitiveTypePropagationWorklist(&worklist);
  EquivalentPhisCleanup();
}

void SsaBuilder::ProcessPrimitiveTypePropagationWorklist(ArenaVector<HPhi*>* worklist) {
  // Process worklist
  while (!worklist->empty()) {
    HPhi* phi = worklist->back();
    worklist->pop_back();
    // The phi could have been made dead as a result of conflicts while in the
    // worklist. If it is now dead, there is no point in updating its type.
    if (phi->IsLive() && UpdatePrimitiveType(phi, worklist)) {
      AddDependentInstructionsToWorklist(phi, worklist);
    }
  }
}

static HArrayGet* FindFloatOrDoubleEquivalentOfArrayGet(HArrayGet* aget) {
  Primitive::Type type = aget->GetType();
  DCHECK(Primitive::IsIntOrLongType(type));
  HArrayGet* next = aget->GetNext()->AsArrayGet();
  return (next != nullptr && next->IsEquivalentOf(aget)) ? next : nullptr;
}

static HArrayGet* CreateFloatOrDoubleEquivalentOfArrayGet(HArrayGet* aget) {
  Primitive::Type type = aget->GetType();
  DCHECK(Primitive::IsIntOrLongType(type));
  DCHECK(FindFloatOrDoubleEquivalentOfArrayGet(aget) == nullptr);

  HArrayGet* equivalent = new (aget->GetBlock()->GetGraph()->GetArena()) HArrayGet(
      aget->GetArray(),
      aget->GetIndex(),
      type == Primitive::kPrimInt ? Primitive::kPrimFloat : Primitive::kPrimDouble,
      aget->GetDexPc());
  aget->GetBlock()->InsertInstructionAfter(equivalent, aget);
  return equivalent;
}

static Primitive::Type GetPrimitiveArrayComponentType(HInstruction* array)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  ReferenceTypeInfo array_type = array->GetReferenceTypeInfo();
  DCHECK(array_type.IsPrimitiveArrayClass());
  return array_type.GetTypeHandle()->GetComponentType()->GetPrimitiveType();
}

bool SsaBuilder::FixAmbiguousArrayOps() {
  if (ambiguous_agets_.empty() && ambiguous_asets_.empty()) {
    return true;
  }

  // The wrong ArrayGet equivalent may still have Phi uses coming from ArraySet
  // uses (because they are untyped) and environment uses (if --debuggable).
  // After resolving all ambiguous ArrayGets, we will re-run primitive type
  // propagation on the Phis which need to be updated.
  ArenaVector<HPhi*> worklist(GetGraph()->GetArena()->Adapter());

  {
    ScopedObjectAccess soa(Thread::Current());

    for (HArrayGet* aget_int : ambiguous_agets_) {
      HInstruction* array = aget_int->GetArray();
      if (!array->GetReferenceTypeInfo().IsPrimitiveArrayClass()) {
        // RTP did not type the input array. Bail.
        return false;
      }

      HArrayGet* aget_float = FindFloatOrDoubleEquivalentOfArrayGet(aget_int);
      Primitive::Type array_type = GetPrimitiveArrayComponentType(array);
      DCHECK_EQ(Primitive::Is64BitType(aget_int->GetType()), Primitive::Is64BitType(array_type));

      if (Primitive::IsIntOrLongType(array_type)) {
        if (aget_float != nullptr) {
          // There is a float/double equivalent. We must replace it and re-run
          // primitive type propagation on all dependent instructions.
          aget_float->ReplaceWith(aget_int);
          aget_float->GetBlock()->RemoveInstruction(aget_float);
          AddDependentInstructionsToWorklist(aget_int, &worklist);
        }
      } else {
        DCHECK(Primitive::IsFloatingPointType(array_type));
        if (aget_float == nullptr) {
          // This is a float/double ArrayGet but there were no typed uses which
          // would create the typed equivalent. Create it now.
          aget_float = CreateFloatOrDoubleEquivalentOfArrayGet(aget_int);
        }
        // Replace the original int/long instruction. Note that it may have phi
        // uses, environment uses, as well as real uses (from untyped ArraySets).
        // We need to re-run primitive type propagation on its dependent instructions.
        aget_int->ReplaceWith(aget_float);
        aget_int->GetBlock()->RemoveInstruction(aget_int);
        AddDependentInstructionsToWorklist(aget_float, &worklist);
      }
    }

    // Set a flag stating that types of ArrayGets have been resolved. Requesting
    // equivalent of the wrong type with GetFloatOrDoubleEquivalentOfArrayGet
    // will fail from now on.
    agets_fixed_ = true;

    for (HArraySet* aset : ambiguous_asets_) {
      HInstruction* array = aset->GetArray();
      if (!array->GetReferenceTypeInfo().IsPrimitiveArrayClass()) {
        // RTP did not type the input array. Bail.
        return false;
      }

      HInstruction* value = aset->GetValue();
      Primitive::Type value_type = value->GetType();
      Primitive::Type array_type = GetPrimitiveArrayComponentType(array);
      DCHECK_EQ(Primitive::Is64BitType(value_type), Primitive::Is64BitType(array_type));

      if (Primitive::IsFloatingPointType(array_type)) {
        if (!Primitive::IsFloatingPointType(value_type)) {
          DCHECK(Primitive::IsIntegralType(value_type));
          // Array elements are floating-point but the value has not been replaced
          // with its floating-point equivalent. The replacement must always
          // succeed in code validated by the verifier.
          HInstruction* equivalent = GetFloatOrDoubleEquivalent(value, array_type);
          DCHECK(equivalent != nullptr);
          aset->ReplaceInput(equivalent, /* input_index */ 2);
          if (equivalent->IsPhi()) {
            // Returned equivalent is a phi which may not have had its inputs
            // replaced yet. We need to run primitive type propagation on it.
            worklist.push_back(equivalent->AsPhi());
          }
        }
      } else {
        // Array elements are integral and the value assigned to it initially
        // was integral too. Nothing to do.
        DCHECK(Primitive::IsIntegralType(array_type));
        DCHECK(Primitive::IsIntegralType(value_type));
      }
    }
  }

  if (!worklist.empty()) {
    ProcessPrimitiveTypePropagationWorklist(&worklist);
    EquivalentPhisCleanup();
  }

  return true;
}

void SsaBuilder::RemoveRedundantUninitializedStrings() {
  if (GetGraph()->IsDebuggable()) {
    // Do not perform the optimization for consistency with the interpreter
    // which always allocates an object for new-instance of String.
    return;
  }

  for (HNewInstance* new_instance : uninitialized_strings_) {
    DCHECK(new_instance->IsStringAlloc());

    // Replace NewInstance of String with NullConstant if not used prior to
    // calling StringFactory. In case of deoptimization, the interpreter is
    // expected to skip null check on the `this` argument of the StringFactory call.
    if (!new_instance->HasNonEnvironmentUses()) {
      new_instance->ReplaceWith(GetGraph()->GetNullConstant());
      new_instance->GetBlock()->RemoveInstruction(new_instance);

      // Remove LoadClass if not needed any more.
      HLoadClass* load_class = new_instance->InputAt(0)->AsLoadClass();
      DCHECK(load_class != nullptr);
      DCHECK(!load_class->NeedsAccessCheck()) << "String class is always accessible";
      if (!load_class->HasUses()) {
        load_class->GetBlock()->RemoveInstruction(load_class);
      }
    }
  }
}

GraphAnalysisResult SsaBuilder::BuildSsa() {
  DCHECK(!GetGraph()->IsInSsaForm());

  // 1) Visit in reverse post order. We need to have all predecessors of a block
  // visited (with the exception of loops) in order to create the right environment
  // for that block. For loops, we create phis whose inputs will be set in 2).
  for (HReversePostOrderIterator it(*GetGraph()); !it.Done(); it.Advance()) {
    VisitBasicBlock(it.Current());
  }

  // 2) Set inputs of loop header phis.
  SetLoopHeaderPhiInputs();

  // 3) Propagate types of phis. At this point, phis are typed void in the general
  // case, or float/double/reference if we created an equivalent phi. So we need
  // to propagate the types across phis to give them a correct type. If a type
  // conflict is detected in this stage, the phi is marked dead.
  RunPrimitiveTypePropagation();

  // 4) Now that the correct primitive types have been assigned, we can get rid
  // of redundant phis. Note that we cannot do this phase before type propagation,
  // otherwise we could get rid of phi equivalents, whose presence is a requirement
  // for the type propagation phase. Note that this is to satisfy statement (a)
  // of the SsaBuilder (see ssa_builder.h).
  SsaRedundantPhiElimination(GetGraph()).Run();

  // 5) Fix the type for null constants which are part of an equality comparison.
  // We need to do this after redundant phi elimination, to ensure the only cases
  // that we can see are reference comparison against 0. The redundant phi
  // elimination ensures we do not see a phi taking two 0 constants in a HEqual
  // or HNotEqual.
  FixNullConstantType();

  // 6) Compute type of reference type instructions. The pass assumes that
  // NullConstant has been fixed up.
  ReferenceTypePropagation(GetGraph(), handles_, /* is_first_run */ true).Run();

  // 7) Step 1) duplicated ArrayGet instructions with ambiguous type (int/float
  // or long/double) and marked ArraySets with ambiguous input type. Now that RTP
  // computed the type of the array input, the ambiguity can be resolved and the
  // correct equivalents kept.
  if (!FixAmbiguousArrayOps()) {
    return kAnalysisFailAmbiguousArrayOp;
  }

  // 8) Mark dead phis. This will mark phis which are not used by instructions
  // or other live phis. If compiling as debuggable code, phis will also be kept
  // live if they have an environment use.
  SsaDeadPhiElimination dead_phi_elimimation(GetGraph());
  dead_phi_elimimation.MarkDeadPhis();

  // 9) Make sure environments use the right phi equivalent: a phi marked dead
  // can have a phi equivalent that is not dead. In that case we have to replace
  // it with the live equivalent because deoptimization and try/catch rely on
  // environments containing values of all live vregs at that point. Note that
  // there can be multiple phis for the same Dex register that are live
  // (for example when merging constants), in which case it is okay for the
  // environments to just reference one.
  FixEnvironmentPhis();

  // 10) Now that the right phis are used for the environments, we can eliminate
  // phis we do not need. Regardless of the debuggable status, this phase is
  /// necessary for statement (b) of the SsaBuilder (see ssa_builder.h), as well
  // as for the code generation, which does not deal with phis of conflicting
  // input types.
  dead_phi_elimimation.EliminateDeadPhis();

  // 11) Step 1) replaced uses of NewInstances of String with the results of
  // their corresponding StringFactory calls. Unless the String objects are used
  // before they are initialized, they can be replaced with NullConstant.
  // Note that this optimization is valid only if unsimplified code does not use
  // the uninitialized value because we assume execution can be deoptimized at
  // any safepoint. We must therefore perform it before any other optimizations.
  RemoveRedundantUninitializedStrings();

  // 12) Clear locals.
  for (HInstructionIterator it(GetGraph()->GetEntryBlock()->GetInstructions());
       !it.Done();
       it.Advance()) {
    HInstruction* current = it.Current();
    if (current->IsLocal()) {
      current->GetBlock()->RemoveInstruction(current);
    }
  }

  GetGraph()->SetInSsaForm();
  return kAnalysisSuccess;
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
        HInstruction* current_local_value = (*current_locals_)[i];
        if (current_local_value != nullptr) {
          HPhi* phi = new (arena) HPhi(
              arena,
              i,
              0,
              current_local_value->GetType());
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
            GetGraph()->GetArena(),
            local,
            0,
            incoming->GetType());
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
        HInstruction* first_input = ValueOfLocal(block->GetPredecessors()[0], local);
        HPhi* phi = new (GetGraph()->GetArena()) HPhi(
            GetGraph()->GetArena(),
            local,
            block->GetPredecessors().size(),
            first_input->GetType());
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
    // An existing equivalent was found. If it is dead, conflict was previously
    // identified and we return nullptr instead.
    HPhi* next_phi = next->AsPhi();
    DCHECK_EQ(next_phi->GetType(), type);
    return next_phi->IsLive() ? next_phi : nullptr;
  }
}

HArrayGet* SsaBuilder::GetFloatOrDoubleEquivalentOfArrayGet(HArrayGet* aget) {
  DCHECK(Primitive::IsIntegralType(aget->GetType()));

  if (!Primitive::IsIntOrLongType(aget->GetType())) {
    // Cannot type boolean, char, byte, short to float/double.
    return nullptr;
  }

  DCHECK(ContainsElement(ambiguous_agets_, aget));
  if (agets_fixed_) {
    // This used to be an ambiguous ArrayGet but its type has been resolved to
    // int/long. Requesting a float/double equivalent should lead to a conflict.
    if (kIsDebugBuild) {
      ScopedObjectAccess soa(Thread::Current());
      DCHECK(Primitive::IsIntOrLongType(GetPrimitiveArrayComponentType(aget->GetArray())));
    }
    return nullptr;
  } else {
    // This is an ambiguous ArrayGet which has not been resolved yet. Return an
    // equivalent float/double instruction to use until it is resolved.
    HArrayGet* equivalent = FindFloatOrDoubleEquivalentOfArrayGet(aget);
    return (equivalent == nullptr) ? CreateFloatOrDoubleEquivalentOfArrayGet(aget) : equivalent;
  }
}

HInstruction* SsaBuilder::GetFloatOrDoubleEquivalent(HInstruction* value, Primitive::Type type) {
  if (value->IsArrayGet()) {
    return GetFloatOrDoubleEquivalentOfArrayGet(value->AsArrayGet());
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
  Primitive::Type load_type = load->GetType();
  HInstruction* value = (*current_locals_)[load->GetLocal()->GetRegNumber()];
  // If the operation requests a specific type, we make sure its input is of that type.
  if (load_type != value->GetType()) {
    if (load_type == Primitive::kPrimFloat || load_type == Primitive::kPrimDouble) {
      value = GetFloatOrDoubleEquivalent(value, load_type);
    } else if (load_type == Primitive::kPrimNot) {
      value = GetReferenceTypeEquivalent(value);
    }
  }

  load->ReplaceWith(value);
  load->GetBlock()->RemoveInstruction(load);
}

void SsaBuilder::VisitStoreLocal(HStoreLocal* store) {
  uint32_t reg_number = store->GetLocal()->GetRegNumber();
  HInstruction* stored_value = store->InputAt(1);
  Primitive::Type stored_type = stored_value->GetType();
  DCHECK_NE(stored_type, Primitive::kPrimVoid);

  // Storing into vreg `reg_number` may implicitly invalidate the surrounding
  // registers. Consider the following cases:
  // (1) Storing a wide value must overwrite previous values in both `reg_number`
  //     and `reg_number+1`. We store `nullptr` in `reg_number+1`.
  // (2) If vreg `reg_number-1` holds a wide value, writing into `reg_number`
  //     must invalidate it. We store `nullptr` in `reg_number-1`.
  // Consequently, storing a wide value into the high vreg of another wide value
  // will invalidate both `reg_number-1` and `reg_number+1`.

  if (reg_number != 0) {
    HInstruction* local_low = (*current_locals_)[reg_number - 1];
    if (local_low != nullptr && Primitive::Is64BitType(local_low->GetType())) {
      // The vreg we are storing into was previously the high vreg of a pair.
      // We need to invalidate its low vreg.
      DCHECK((*current_locals_)[reg_number] == nullptr);
      (*current_locals_)[reg_number - 1] = nullptr;
    }
  }

  (*current_locals_)[reg_number] = stored_value;
  if (Primitive::Is64BitType(stored_type)) {
    // We are storing a pair. Invalidate the instruction in the high vreg.
    (*current_locals_)[reg_number + 1] = nullptr;
  }

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
    for (HBasicBlock* catch_block : try_entry.GetExceptionHandlers()) {
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

void SsaBuilder::VisitArrayGet(HArrayGet* aget) {
  Primitive::Type type = aget->GetType();
  DCHECK(!Primitive::IsFloatingPointType(type));
  if (Primitive::IsIntOrLongType(type)) {
    ambiguous_agets_.push_back(aget);
  }
  VisitInstruction(aget);
}

void SsaBuilder::VisitArraySet(HArraySet* aset) {
  Primitive::Type type = aset->GetValue()->GetType();
  if (Primitive::IsIntOrLongType(type)) {
    ambiguous_asets_.push_back(aset);
  }
  VisitInstruction(aset);
}

void SsaBuilder::VisitInvokeStaticOrDirect(HInvokeStaticOrDirect* invoke) {
  VisitInstruction(invoke);

  if (invoke->IsStringInit()) {
    // This is a StringFactory call which acts as a String constructor. Its
    // result replaces the empty String pre-allocated by NewInstance.
    HInstruction* arg_this = invoke->GetAndRemoveThisArgumentOfStringInit();

    // Replacing the NewInstance might render it redundant. Keep a list of these
    // to be visited once it is clear whether it is has remaining uses.
    if (arg_this->IsNewInstance()) {
      uninitialized_strings_.push_back(arg_this->AsNewInstance());
    } else {
      DCHECK(arg_this->IsPhi());
      // NewInstance is not the direct input of the StringFactory call. It might
      // be redundant but optimizing this case is not worth the effort.
    }

    // Walk over all vregs and replace any occurrence of `arg_this` with `invoke`.
    for (size_t vreg = 0, e = current_locals_->size(); vreg < e; ++vreg) {
      if ((*current_locals_)[vreg] == arg_this) {
        (*current_locals_)[vreg] = invoke;
      }
    }
  }
}

}  // namespace art
