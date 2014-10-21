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
#include "ssa_type_propagation.h"

namespace art {

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

  // 3) Propagate types of phis.
  SsaTypePropagation type_propagation(GetGraph());
  type_propagation.Run();

  // 4) Clear locals.
  // TODO: Move this to a dead code eliminator phase.
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
          HInstruction* value = ValueOfLocal(block->GetPredecessors().Get(i), local);
          phi->SetRawInputAt(i, value);
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
static HFloatConstant* GetFloatEquivalent(HIntConstant* constant) {
  // We place the floating point constant next to this constant.
  HFloatConstant* result = constant->GetNext()->AsFloatConstant();
  if (result == nullptr) {
    HGraph* graph = constant->GetBlock()->GetGraph();
    ArenaAllocator* allocator = graph->GetArena();
    result = new (allocator) HFloatConstant(bit_cast<int32_t, float>(constant->GetValue()));
    constant->GetBlock()->InsertInstructionBefore(result, constant->GetNext());
  } else {
    // If there is already a constant with the expected type, we know it is
    // the floating point equivalent of this constant.
    DCHECK_EQ((bit_cast<float, int32_t>(result->GetValue())), constant->GetValue());
  }
  return result;
}

/**
 * Wide constants in the Dex format are not typed. So the builder types them as
 * longs, but when doing the SSA form, we might realize the constant
 * is used for floating point operations. We create a floating-point equivalent
 * constant to make the operations correctly typed.
 */
static HDoubleConstant* GetDoubleEquivalent(HLongConstant* constant) {
  // We place the floating point constant next to this constant.
  HDoubleConstant* result = constant->GetNext()->AsDoubleConstant();
  if (result == nullptr) {
    HGraph* graph = constant->GetBlock()->GetGraph();
    ArenaAllocator* allocator = graph->GetArena();
    result = new (allocator) HDoubleConstant(bit_cast<int64_t, double>(constant->GetValue()));
    constant->GetBlock()->InsertInstructionBefore(result, constant->GetNext());
  } else {
    // If there is already a constant with the expected type, we know it is
    // the floating point equivalent of this constant.
    DCHECK_EQ((bit_cast<double, int64_t>(result->GetValue())), constant->GetValue());
  }
  return result;
}

/**
 * Because of Dex format, we might end up having the same phi being
 * used for non floating point operations and floating point operations. Because
 * we want the graph to be correctly typed (and thereafter avoid moves between
 * floating point registers and core registers), we need to create a copy of the
 * phi with a floating point type.
 */
static HPhi* GetFloatOrDoubleEquivalentOfPhi(HPhi* phi, Primitive::Type type) {
  // We place the floating point phi next to this phi.
  HInstruction* next = phi->GetNext();
  if (next == nullptr
      || (next->GetType() != Primitive::kPrimDouble && next->GetType() != Primitive::kPrimFloat)) {
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
    // If there is already a phi with the expected type, we know it is the floating
    // point equivalent of this phi.
    DCHECK_EQ(next->AsPhi()->GetRegNumber(), phi->GetRegNumber());
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
    return GetFloatOrDoubleEquivalentOfPhi(value->AsPhi(), type);
  } else {
    // For other instructions, we assume the verifier has checked that the dex format is correctly
    // typed and the value in a dex register will not be used for both floating point and
    // non-floating point operations. So the only reason an instruction would want a floating
    // point equivalent is for an unused phi that will be removed by the dead phi elimination phase.
    DCHECK(user->IsPhi());
    return value;
  }
}

void SsaBuilder::VisitLoadLocal(HLoadLocal* load) {
  HInstruction* value = current_locals_->Get(load->GetLocal()->GetRegNumber());
  if (load->GetType() != value->GetType()
      && (load->GetType() == Primitive::kPrimFloat || load->GetType() == Primitive::kPrimDouble)) {
    // If the operation requests a specific type, we make sure its input is of that type.
    value = GetFloatOrDoubleEquivalent(load, value, load->GetType());
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
      GetGraph()->GetArena(), current_locals_->Size());
  environment->Populate(*current_locals_);
  instruction->SetEnvironment(environment);
}

}  // namespace art
