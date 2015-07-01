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

#include "reference_type_propagation.h"

#include "class_linker.h"
#include "class_linker-inl.h"
#include "mirror/class-inl.h"
#include "mirror/dex_cache.h"
#include "scoped_thread_state_change.h"

namespace art {

class RTPVisitor : public HGraphDelegateVisitor {
 public:
  RTPVisitor(HGraph* graph,
             StackHandleScopeCollection* handles,
             GrowableArray<HInstruction*>* worklist,
             ReferenceTypeInfo::TypeHandle object_class_handle,
             ReferenceTypeInfo::TypeHandle class_class_handle,
             ReferenceTypeInfo::TypeHandle string_class_handle)
    : HGraphDelegateVisitor(graph),
      handles_(handles),
      object_class_handle_(object_class_handle),
      class_class_handle_(class_class_handle),
      string_class_handle_(string_class_handle),
      worklist_(worklist) {}

  void VisitNullConstant(HNullConstant* null_constant) OVERRIDE;
  void VisitNewInstance(HNewInstance* new_instance) OVERRIDE;
  void VisitLoadClass(HLoadClass* load_class) OVERRIDE;
  void VisitClinitCheck(HClinitCheck* clinit_check) OVERRIDE;
  void VisitLoadString(HLoadString* instr) OVERRIDE;
  void VisitNewArray(HNewArray* instr) OVERRIDE;
  void VisitParameterValue(HParameterValue* instr) OVERRIDE;
  void UpdateFieldAccessTypeInfo(HInstruction* instr, const FieldInfo& info);
  void SetClassAsTypeInfo(HInstruction* instr, mirror::Class* klass, bool is_exact);
  void VisitInstanceFieldGet(HInstanceFieldGet* instr) OVERRIDE;
  void VisitStaticFieldGet(HStaticFieldGet* instr) OVERRIDE;
  void VisitInvoke(HInvoke* instr) OVERRIDE;
  void VisitArrayGet(HArrayGet* instr) OVERRIDE;
  void VisitNullCheck(HNullCheck* instr) OVERRIDE;

  void UpdateReferenceTypeInfo(HInstruction* instr,
                               uint16_t type_idx,
                               const DexFile& dex_file,
                               bool is_exact);

 private:
  StackHandleScopeCollection* handles_;
  ReferenceTypeInfo::TypeHandle object_class_handle_;
  ReferenceTypeInfo::TypeHandle class_class_handle_;
  ReferenceTypeInfo::TypeHandle string_class_handle_;
  GrowableArray<HInstruction*>* worklist_;

  static constexpr size_t kDefaultWorklistSize = 8;
};

ReferenceTypePropagation::ReferenceTypePropagation(HGraph* graph,
                                                   StackHandleScopeCollection* handles)
    : HOptimization(graph, kReferenceTypePropagationPassName),
      handles_(handles),
      worklist_(graph->GetArena(), kDefaultWorklistSize) {
  ClassLinker* linker = Runtime::Current()->GetClassLinker();
  object_class_handle_ = handles_->NewHandle(linker->GetClassRoot(ClassLinker::kJavaLangObject));
  string_class_handle_ = handles_->NewHandle(linker->GetClassRoot(ClassLinker::kJavaLangString));
  class_class_handle_ = handles_->NewHandle(linker->GetClassRoot(ClassLinker::kJavaLangClass));

  if (kIsDebugBuild) {
    ScopedObjectAccess soa(Thread::Current());
    DCHECK(ReferenceTypeInfo::IsValidHandle(object_class_handle_));
    DCHECK(ReferenceTypeInfo::IsValidHandle(class_class_handle_));
    DCHECK(ReferenceTypeInfo::IsValidHandle(string_class_handle_));
  }
}

void ReferenceTypePropagation::Run() {
  // To properly propagate type info we need to visit in the dominator-based order.
  // Reverse post order guarantees a node's dominators are visited first.
  // We take advantage of this order in `VisitBasicBlock`.
  for (HReversePostOrderIterator it(*graph_); !it.Done(); it.Advance()) {
    VisitBasicBlock(it.Current());
  }
  ProcessWorklist();

  if (kIsDebugBuild) {
    // TODO: move this to the graph checker.
    ScopedObjectAccess soa(Thread::Current());
    for (HReversePostOrderIterator it(*graph_); !it.Done(); it.Advance()) {
      HBasicBlock* block = it.Current();
      for (HInstructionIterator iti(block->GetInstructions()); !iti.Done(); iti.Advance()) {
        HInstruction* instr = iti.Current();
        if (instr->GetType() == Primitive::kPrimNot) {
          DCHECK(instr->GetReferenceTypeInfo().IsValid())
              << "Invalid RTI for instruction: " << instr->DebugName();
        }
      }
    }
  }
}

void ReferenceTypePropagation::VisitBasicBlock(HBasicBlock* block) {
  RTPVisitor visitor(graph_,
                     handles_,
                     &worklist_,
                     object_class_handle_,
                     class_class_handle_,
                     string_class_handle_);
  // Initialize exact types first for faster convergence.
  for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
    HInstruction* instr = it.Current();
    instr->Accept(&visitor);
  }

  // Handle Phis.
  for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
    VisitPhi(it.Current()->AsPhi());
  }

  // Add extra nodes to bound types.
  BoundTypeForIfNotNull(block);
  BoundTypeForIfInstanceOf(block);
}

void ReferenceTypePropagation::BoundTypeForIfNotNull(HBasicBlock* block) {
  HIf* ifInstruction = block->GetLastInstruction()->AsIf();
  if (ifInstruction == nullptr) {
    return;
  }
  HInstruction* ifInput = ifInstruction->InputAt(0);
  if (!ifInput->IsNotEqual() && !ifInput->IsEqual()) {
    return;
  }
  HInstruction* input0 = ifInput->InputAt(0);
  HInstruction* input1 = ifInput->InputAt(1);
  HInstruction* obj = nullptr;

  if (input1->IsNullConstant()) {
    obj = input0;
  } else if (input0->IsNullConstant()) {
    obj = input1;
  } else {
    return;
  }

  // We only need to bound the type if we have uses in the relevant block.
  // So start with null and create the HBoundType lazily, only if it's needed.
  HBoundType* bound_type = nullptr;
  HBasicBlock* notNullBlock = ifInput->IsNotEqual()
      ? ifInstruction->IfTrueSuccessor()
      : ifInstruction->IfFalseSuccessor();

  for (HUseIterator<HInstruction*> it(obj->GetUses()); !it.Done(); it.Advance()) {
    HInstruction* user = it.Current()->GetUser();
    if (notNullBlock->Dominates(user->GetBlock())) {
      if (bound_type == nullptr) {
        bound_type = new (graph_->GetArena()) HBoundType(obj, obj->GetReferenceTypeInfo());
        notNullBlock->InsertInstructionBefore(bound_type, notNullBlock->GetFirstInstruction());
      }
      user->ReplaceInput(bound_type, it.Current()->GetIndex());
    }
  }
}

// Detects if `block` is the True block for the pattern
// `if (x instanceof ClassX) { }`
// If that's the case insert an HBoundType instruction to bound the type of `x`
// to `ClassX` in the scope of the dominated blocks.
void ReferenceTypePropagation::BoundTypeForIfInstanceOf(HBasicBlock* block) {
  HIf* ifInstruction = block->GetLastInstruction()->AsIf();
  if (ifInstruction == nullptr) {
    return;
  }
  HInstruction* ifInput = ifInstruction->InputAt(0);
  HInstruction* instanceOf = nullptr;
  HBasicBlock* instanceOfTrueBlock = nullptr;

  // The instruction simplifier has transformed:
  //   - `if (a instanceof A)` into an HIf with an HInstanceOf input
  //   - `if (!(a instanceof A)` into an HIf with an HBooleanNot input (which in turn
  //     has an HInstanceOf input)
  // So we should not see the usual HEqual here.
  if (ifInput->IsInstanceOf()) {
    instanceOf = ifInput;
    instanceOfTrueBlock = ifInstruction->IfTrueSuccessor();
  } else if (ifInput->IsBooleanNot() && ifInput->InputAt(0)->IsInstanceOf()) {
    instanceOf = ifInput->InputAt(0);
    instanceOfTrueBlock = ifInstruction->IfFalseSuccessor();
  } else {
    return;
  }

  // We only need to bound the type if we have uses in the relevant block.
  // So start with null and create the HBoundType lazily, only if it's needed.
  HBoundType* bound_type = nullptr;

  HInstruction* obj = instanceOf->InputAt(0);
  if (obj->GetReferenceTypeInfo().IsExact() && !obj->IsPhi()) {
    // This method is being called while doing a fixed-point calculation
    // over phis. Non-phis instruction whose type is already known do
    // not need to be bound to another type.
    // Not that this also prevents replacing `HLoadClass` with a `HBoundType`.
    // `HCheckCast` and `HInstanceOf` expect a `HLoadClass` as a second
    // input.
    return;
  }
  for (HUseIterator<HInstruction*> it(obj->GetUses()); !it.Done(); it.Advance()) {
    HInstruction* user = it.Current()->GetUser();
    if (instanceOfTrueBlock->Dominates(user->GetBlock())) {
      if (bound_type == nullptr) {
        HLoadClass* load_class = instanceOf->InputAt(1)->AsLoadClass();

        ReferenceTypeInfo obj_rti = obj->GetReferenceTypeInfo();
        ReferenceTypeInfo class_rti = load_class->GetLoadedClassRTI();
        bound_type = new (graph_->GetArena()) HBoundType(obj, class_rti);

        // Narrow the type as much as possible.
        {
          ScopedObjectAccess soa(Thread::Current());
          if (!load_class->IsResolved() || class_rti.IsSupertypeOf(obj_rti)) {
            bound_type->SetReferenceTypeInfo(obj_rti);
          } else {
            bound_type->SetReferenceTypeInfo(
                ReferenceTypeInfo::Create(class_rti.GetTypeHandle(), /* is_exact */ false));
          }
        }

        instanceOfTrueBlock->InsertInstructionBefore(
            bound_type, instanceOfTrueBlock->GetFirstInstruction());
      }
      user->ReplaceInput(bound_type, it.Current()->GetIndex());
    }
  }
}

void RTPVisitor::SetClassAsTypeInfo(HInstruction* instr,
                                    mirror::Class* klass,
                                    bool is_exact) {
  if (instr->IsInvokeStaticOrDirect() && instr->AsInvokeStaticOrDirect()->IsStringInit()) {
    // Calls to String.<init> are replaced with a StringFactory.
    if (kIsDebugBuild) {
      ScopedObjectAccess soa(Thread::Current());
      ClassLinker* cl = Runtime::Current()->GetClassLinker();
      mirror::DexCache* dex_cache = cl->FindDexCache(instr->AsInvoke()->GetDexFile());
      ArtMethod* method = dex_cache->GetResolvedMethod(
          instr->AsInvoke()->GetDexMethodIndex(), cl->GetImagePointerSize());
      DCHECK(method != nullptr);
      mirror::Class* declaring_class = method->GetDeclaringClass();
      DCHECK(declaring_class != nullptr);
      DCHECK(declaring_class->IsStringClass())
          << "Expected String class: " << PrettyDescriptor(declaring_class);
      DCHECK(method->IsConstructor())
          << "Expected String.<init>: " << PrettyMethod(method);
    }
    instr->SetReferenceTypeInfo(
      ReferenceTypeInfo::Create(string_class_handle_, /* is_exact */ true));
  } else if (klass != nullptr) {
    ScopedObjectAccess soa(Thread::Current());
    ReferenceTypeInfo::TypeHandle handle = handles_->NewHandle(klass);
    is_exact = is_exact || klass->IsFinal();
    instr->SetReferenceTypeInfo(ReferenceTypeInfo::Create(handle, is_exact));
  } else {
    instr->SetReferenceTypeInfo(
      ReferenceTypeInfo::Create(object_class_handle_, /* is_exact */ false));
  }
}

void RTPVisitor::UpdateReferenceTypeInfo(HInstruction* instr,
                                         uint16_t type_idx,
                                         const DexFile& dex_file,
                                         bool is_exact) {
  DCHECK_EQ(instr->GetType(), Primitive::kPrimNot);

  ScopedObjectAccess soa(Thread::Current());
  mirror::DexCache* dex_cache = Runtime::Current()->GetClassLinker()->FindDexCache(dex_file);
  // Get type from dex cache assuming it was populated by the verifier.
  SetClassAsTypeInfo(instr, dex_cache->GetResolvedType(type_idx), is_exact);
}

void RTPVisitor::VisitNullConstant(HNullConstant* instr) {
  instr->SetReferenceTypeInfo(
      ReferenceTypeInfo::Create(object_class_handle_, /* is_exact */ false));
}

void RTPVisitor::VisitNewInstance(HNewInstance* instr) {
  UpdateReferenceTypeInfo(instr, instr->GetTypeIndex(), instr->GetDexFile(), /* is_exact */ true);
}

void RTPVisitor::VisitNewArray(HNewArray* instr) {
  UpdateReferenceTypeInfo(instr, instr->GetTypeIndex(), instr->GetDexFile(), /* is_exact */ true);
}

void RTPVisitor::VisitParameterValue(HParameterValue* instr) {
  if (instr->GetType() == Primitive::kPrimNot) {
    // TODO: parse the signature and add precise types for the parameters.
    SetClassAsTypeInfo(instr, nullptr, /* is_exact */ false);
  }
}

void RTPVisitor::UpdateFieldAccessTypeInfo(HInstruction* instr,
                                           const FieldInfo& info) {
  // The field index is unknown only during tests.
  if (instr->GetType() != Primitive::kPrimNot || info.GetFieldIndex() == kUnknownFieldIndex) {
    return;
  }

  ScopedObjectAccess soa(Thread::Current());
  ClassLinker* cl = Runtime::Current()->GetClassLinker();
  mirror::DexCache* dex_cache = cl->FindDexCache(info.GetDexFile());
  ArtField* field = cl->GetResolvedField(info.GetFieldIndex(), dex_cache);
  // TODO: There are certain cases where we can't resolve the field.
  // b/21914925 is open to keep track of a repro case for this issue.
  mirror::Class* klass = (field == nullptr) ? nullptr : field->GetType<false>();
  SetClassAsTypeInfo(instr, klass, /* is_exact */ false);
}

void RTPVisitor::VisitInstanceFieldGet(HInstanceFieldGet* instr) {
  UpdateFieldAccessTypeInfo(instr, instr->GetFieldInfo());
}

void RTPVisitor::VisitStaticFieldGet(HStaticFieldGet* instr) {
  UpdateFieldAccessTypeInfo(instr, instr->GetFieldInfo());
}

void RTPVisitor::VisitLoadClass(HLoadClass* instr) {
  ScopedObjectAccess soa(Thread::Current());
  mirror::DexCache* dex_cache =
      Runtime::Current()->GetClassLinker()->FindDexCache(instr->GetDexFile());
  // Get type from dex cache assuming it was populated by the verifier.
  mirror::Class* resolved_class = dex_cache->GetResolvedType(instr->GetTypeIndex());
  DCHECK(resolved_class != nullptr);
  ReferenceTypeInfo::TypeHandle handle = handles_->NewHandle(resolved_class);
  instr->SetLoadedClassRTI(ReferenceTypeInfo::Create(handle, /* is_exact */ true));
  instr->SetReferenceTypeInfo(ReferenceTypeInfo::Create(class_class_handle_, /* is_exact */ true));
}

void RTPVisitor::VisitClinitCheck(HClinitCheck* instr) {
  instr->SetReferenceTypeInfo(instr->InputAt(0)->GetReferenceTypeInfo());
}

void RTPVisitor::VisitLoadString(HLoadString* instr) {
  instr->SetReferenceTypeInfo(ReferenceTypeInfo::Create(string_class_handle_, /* is_exact */ true));
}

void RTPVisitor::VisitNullCheck(HNullCheck* instr) {
  ScopedObjectAccess soa(Thread::Current());

  HInstruction* parent = instr->InputAt(0);
  ReferenceTypeInfo parent_rti = parent->GetReferenceTypeInfo();
  if (!parent_rti.IsValid()) {
    // Parent could be a Phi or an ArrayGet and we might not have any valid
    // information on them at this point.
    DCHECK(parent->IsPhi() || parent->IsArrayGet()) << parent->DebugName();
    worklist_->Add(instr);
    return;
  }
  instr->SetReferenceTypeInfo(parent->GetReferenceTypeInfo());
}

void ReferenceTypePropagation::VisitPhi(HPhi* phi) {
  if (phi->GetType() != Primitive::kPrimNot) {
    return;
  }

  if (phi->GetBlock()->IsLoopHeader()) {
    // Set the initial type for the phi. Use the non back edge input for reaching
    // a fixed point faster.
    AddToWorklist(phi);
    phi->SetCanBeNull(phi->InputAt(0)->CanBeNull());
    phi->SetReferenceTypeInfo(phi->InputAt(0)->GetReferenceTypeInfo());
  } else {
    // Eagerly compute the type of the phi, for quicker convergence. Note
    // that we don't need to add users to the worklist because we are
    // doing a reverse post-order visit, therefore either the phi users are
    // non-loop phi and will be visited later in the visit, or are loop-phis,
    // and they are already in the work list.
    UpdateNullability(phi);
    UpdateReferenceTypeInfo(phi);
  }
}

ReferenceTypeInfo ReferenceTypePropagation::MergeTypes(const ReferenceTypeInfo& a,
                                                       const ReferenceTypeInfo& b) {
  if (!b.IsValid()) {
    return a;
  }
  if (!a.IsValid()) {
    return b;
  }

  bool is_exact = a.IsExact() && b.IsExact();
  Handle<mirror::Class> type_handle;

  if (a.GetTypeHandle().Get() == b.GetTypeHandle().Get()) {
    type_handle = a.GetTypeHandle();
  } else if (a.IsSupertypeOf(b)) {
    type_handle = a.GetTypeHandle();
    is_exact = false;
  } else if (b.IsSupertypeOf(a)) {
    type_handle = b.GetTypeHandle();
    is_exact = false;
  } else {
    // TODO: Find the first common super class.
    type_handle = object_class_handle_;
    is_exact = false;
  }

  return ReferenceTypeInfo::Create(type_handle, is_exact);
}

static void UpdateArrayGet(HArrayGet* instr,
                           StackHandleScopeCollection* handles,
                           ReferenceTypeInfo::TypeHandle object_class_handle)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DCHECK_EQ(Primitive::kPrimNot, instr->GetType());

  HInstruction* array = instr->InputAt(0);
  ReferenceTypeInfo parent_rti = array->GetReferenceTypeInfo();
  if (!parent_rti.IsValid()) {
    DCHECK(array->IsPhi() || array->IsNullCheck() || array->IsArrayGet());
    // The array could be a Phi/NullCheck/ArrayGet for which we might have not have
    // valid information at this point.
    return;
  }

  Handle<mirror::Class> handle = parent_rti.GetTypeHandle();
  if (handle->IsObjectArrayClass()) {
    ReferenceTypeInfo::TypeHandle component_handle = handles->NewHandle(handle->GetComponentType());
    instr->SetReferenceTypeInfo(
        ReferenceTypeInfo::Create(component_handle, /* is_exact */ false));
  } else {
    // We don't know what the parent actually is, so we fallback to object.
    instr->SetReferenceTypeInfo(
        ReferenceTypeInfo::Create(object_class_handle, /* is_exact */ false));
  }

  return;
}

bool ReferenceTypePropagation::UpdateReferenceTypeInfo(HInstruction* instr) {
  ScopedObjectAccess soa(Thread::Current());

  ReferenceTypeInfo previous_rti = instr->GetReferenceTypeInfo();
  if (instr->IsBoundType()) {
    UpdateBoundType(instr->AsBoundType());
  } else if (instr->IsPhi()) {
    UpdatePhi(instr->AsPhi());
  } else if (instr->IsNullCheck()) {
    ReferenceTypeInfo parent_rti = instr->InputAt(0)->GetReferenceTypeInfo();
    if (parent_rti.IsValid()) {
      instr->SetReferenceTypeInfo(parent_rti);
    }
  } else if (instr->IsArrayGet()) {
    // TODO: consider if it's worth "looking back" and bounding the input object
    // to an array type.
    UpdateArrayGet(instr->AsArrayGet(), handles_, object_class_handle_);
  } else {
    LOG(FATAL) << "Invalid instruction (should not get here)";
  }

  return !previous_rti.IsEqual(instr->GetReferenceTypeInfo());
}

void RTPVisitor::VisitInvoke(HInvoke* instr) {
  if (instr->GetType() != Primitive::kPrimNot) {
    return;
  }

  ScopedObjectAccess soa(Thread::Current());
  ClassLinker* cl = Runtime::Current()->GetClassLinker();
  mirror::DexCache* dex_cache = cl->FindDexCache(instr->GetDexFile());
  ArtMethod* method = dex_cache->GetResolvedMethod(
      instr->GetDexMethodIndex(), cl->GetImagePointerSize());
  mirror::Class* klass = method == nullptr ? nullptr : method->GetReturnType(false);
  SetClassAsTypeInfo(instr, klass, /* is_exact */ false);
}

void RTPVisitor::VisitArrayGet(HArrayGet* instr) {
  if (instr->GetType() != Primitive::kPrimNot) {
    return;
  }
  ScopedObjectAccess soa(Thread::Current());
  UpdateArrayGet(instr, handles_, object_class_handle_);
  if (!instr->GetReferenceTypeInfo().IsValid()) {
    // If the RTI is not valid it means it depends on instruction for which we
    // don't have the final information before the fix point iteration. So we
    // need to add it to the worklist.
    worklist_->Add(instr);
  }
}

void ReferenceTypePropagation::UpdateBoundType(HBoundType* instr) {
  ReferenceTypeInfo new_rti = instr->InputAt(0)->GetReferenceTypeInfo();
  // Be sure that we don't go over the bounded type.
  ReferenceTypeInfo bound_rti = instr->GetBoundType();
  if (!bound_rti.IsSupertypeOf(new_rti)) {
    new_rti = bound_rti;
  }
  instr->SetReferenceTypeInfo(new_rti);
}

void ReferenceTypePropagation::UpdatePhi(HPhi* instr) {
  ReferenceTypeInfo new_rti = instr->InputAt(0)->GetReferenceTypeInfo();
  if (new_rti.IsObjectClass() && !new_rti.IsExact()) {
    // Early return if we are Object and inexact.
    instr->SetReferenceTypeInfo(new_rti);
    return;
  }
  for (size_t i = 1; i < instr->InputCount(); i++) {
    new_rti = MergeTypes(new_rti, instr->InputAt(i)->GetReferenceTypeInfo());
    if (new_rti.IsObjectClass()) {
      if (!new_rti.IsExact()) {
        break;
      } else {
        continue;
      }
    }
  }
  instr->SetReferenceTypeInfo(new_rti);
}

// Re-computes and updates the nullability of the instruction. Returns whether or
// not the nullability was changed.
bool ReferenceTypePropagation::UpdateNullability(HInstruction* instr) {
  DCHECK(instr->IsPhi()
      || instr->IsBoundType()
      || instr->IsNullCheck()
      || instr->IsArrayGet());

  if (!instr->IsPhi()) {
    return false;
  }

  HPhi* phi = instr->AsPhi();
  bool existing_can_be_null = phi->CanBeNull();
  bool new_can_be_null = false;
  for (size_t i = 0; i < phi->InputCount(); i++) {
    new_can_be_null |= phi->InputAt(i)->CanBeNull();
  }
  phi->SetCanBeNull(new_can_be_null);

  return existing_can_be_null != new_can_be_null;
}

void ReferenceTypePropagation::ProcessWorklist() {
  while (!worklist_.IsEmpty()) {
    HInstruction* instruction = worklist_.Pop();
    if (UpdateNullability(instruction) || UpdateReferenceTypeInfo(instruction)) {
      AddDependentInstructionsToWorklist(instruction);
    }
  }
}

void ReferenceTypePropagation::AddToWorklist(HInstruction* instruction) {
  DCHECK_EQ(instruction->GetType(), Primitive::kPrimNot)
      << instruction->DebugName() << ":" << instruction->GetType();
  worklist_.Add(instruction);
}

void ReferenceTypePropagation::AddDependentInstructionsToWorklist(HInstruction* instruction) {
  for (HUseIterator<HInstruction*> it(instruction->GetUses()); !it.Done(); it.Advance()) {
    HInstruction* user = it.Current()->GetUser();
    if (user->IsPhi()
       || user->IsBoundType()
       || user->IsNullCheck()
       || (user->IsArrayGet() && (user->GetType() == Primitive::kPrimNot))) {
        AddToWorklist(user);
    }
  }
}
}  // namespace art
