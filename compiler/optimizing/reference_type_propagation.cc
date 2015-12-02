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

#include "class_linker-inl.h"
#include "mirror/class-inl.h"
#include "mirror/dex_cache.h"
#include "scoped_thread_state_change.h"

namespace art {

class RTPVisitor : public HGraphDelegateVisitor {
 public:
  RTPVisitor(HGraph* graph,
             StackHandleScopeCollection* handles,
             ArenaVector<HInstruction*>* worklist,
             ReferenceTypeInfo::TypeHandle object_class_handle,
             ReferenceTypeInfo::TypeHandle class_class_handle,
             ReferenceTypeInfo::TypeHandle string_class_handle,
             ReferenceTypeInfo::TypeHandle throwable_class_handle)
    : HGraphDelegateVisitor(graph),
      handles_(handles),
      object_class_handle_(object_class_handle),
      class_class_handle_(class_class_handle),
      string_class_handle_(string_class_handle),
      throwable_class_handle_(throwable_class_handle),
      worklist_(worklist) {}

  void VisitNullConstant(HNullConstant* null_constant) OVERRIDE;
  void VisitNewInstance(HNewInstance* new_instance) OVERRIDE;
  void VisitLoadClass(HLoadClass* load_class) OVERRIDE;
  void VisitClinitCheck(HClinitCheck* clinit_check) OVERRIDE;
  void VisitLoadString(HLoadString* instr) OVERRIDE;
  void VisitLoadException(HLoadException* instr) OVERRIDE;
  void VisitNewArray(HNewArray* instr) OVERRIDE;
  void VisitParameterValue(HParameterValue* instr) OVERRIDE;
  void UpdateFieldAccessTypeInfo(HInstruction* instr, const FieldInfo& info);
  void SetClassAsTypeInfo(HInstruction* instr, mirror::Class* klass, bool is_exact);
  void VisitInstanceFieldGet(HInstanceFieldGet* instr) OVERRIDE;
  void VisitStaticFieldGet(HStaticFieldGet* instr) OVERRIDE;
  void VisitUnresolvedInstanceFieldGet(HUnresolvedInstanceFieldGet* instr) OVERRIDE;
  void VisitUnresolvedStaticFieldGet(HUnresolvedStaticFieldGet* instr) OVERRIDE;
  void VisitInvoke(HInvoke* instr) OVERRIDE;
  void VisitArrayGet(HArrayGet* instr) OVERRIDE;
  void VisitCheckCast(HCheckCast* instr) OVERRIDE;
  void VisitNullCheck(HNullCheck* instr) OVERRIDE;
  void VisitFakeString(HFakeString* instr) OVERRIDE;
  void UpdateReferenceTypeInfo(HInstruction* instr,
                               uint16_t type_idx,
                               const DexFile& dex_file,
                               bool is_exact);

 private:
  StackHandleScopeCollection* handles_;
  ReferenceTypeInfo::TypeHandle object_class_handle_;
  ReferenceTypeInfo::TypeHandle class_class_handle_;
  ReferenceTypeInfo::TypeHandle string_class_handle_;
  ReferenceTypeInfo::TypeHandle throwable_class_handle_;
  ArenaVector<HInstruction*>* worklist_;

  static constexpr size_t kDefaultWorklistSize = 8;
};

ReferenceTypePropagation::ReferenceTypePropagation(HGraph* graph,
                                                   StackHandleScopeCollection* handles,
                                                   const char* name)
    : HOptimization(graph, name),
      handles_(handles),
      worklist_(graph->GetArena()->Adapter(kArenaAllocReferenceTypePropagation)) {
  worklist_.reserve(kDefaultWorklistSize);
  // Mutator lock is required for NewHandle, but annotalysis ignores constructors.
  ScopedObjectAccess soa(Thread::Current());
  ClassLinker* linker = Runtime::Current()->GetClassLinker();
  object_class_handle_ = handles_->NewHandle(linker->GetClassRoot(ClassLinker::kJavaLangObject));
  string_class_handle_ = handles_->NewHandle(linker->GetClassRoot(ClassLinker::kJavaLangString));
  class_class_handle_ = handles_->NewHandle(linker->GetClassRoot(ClassLinker::kJavaLangClass));
  throwable_class_handle_ =
      handles_->NewHandle(linker->GetClassRoot(ClassLinker::kJavaLangThrowable));

  if (kIsDebugBuild) {
    DCHECK(ReferenceTypeInfo::IsValidHandle(object_class_handle_));
    DCHECK(ReferenceTypeInfo::IsValidHandle(class_class_handle_));
    DCHECK(ReferenceTypeInfo::IsValidHandle(string_class_handle_));
    DCHECK(ReferenceTypeInfo::IsValidHandle(throwable_class_handle_));
  }
}

void ReferenceTypePropagation::ValidateTypes() {
  // TODO: move this to the graph checker.
  if (kIsDebugBuild) {
    ScopedObjectAccess soa(Thread::Current());
    for (HReversePostOrderIterator it(*graph_); !it.Done(); it.Advance()) {
      HBasicBlock* block = it.Current();
      for (HInstructionIterator iti(block->GetInstructions()); !iti.Done(); iti.Advance()) {
        HInstruction* instr = iti.Current();
        if (instr->GetType() == Primitive::kPrimNot) {
          DCHECK(instr->GetReferenceTypeInfo().IsValid())
              << "Invalid RTI for instruction: " << instr->DebugName();
          if (instr->IsBoundType()) {
            DCHECK(instr->AsBoundType()->GetUpperBound().IsValid());
          } else if (instr->IsLoadClass()) {
            HLoadClass* cls = instr->AsLoadClass();
            DCHECK(cls->GetReferenceTypeInfo().IsExact());
            DCHECK(!cls->GetLoadedClassRTI().IsValid() || cls->GetLoadedClassRTI().IsExact());
          } else if (instr->IsNullCheck()) {
            DCHECK(instr->GetReferenceTypeInfo().IsEqual(instr->InputAt(0)->GetReferenceTypeInfo()))
                << "NullCheck " << instr->GetReferenceTypeInfo()
                << "Input(0) " << instr->InputAt(0)->GetReferenceTypeInfo();
          }
        }
      }
    }
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
  ValidateTypes();
}

void ReferenceTypePropagation::VisitBasicBlock(HBasicBlock* block) {
  RTPVisitor visitor(graph_,
                     handles_,
                     &worklist_,
                     object_class_handle_,
                     class_class_handle_,
                     string_class_handle_,
                     throwable_class_handle_);
  // Handle Phis first as there might be instructions in the same block who depend on them.
  for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
    VisitPhi(it.Current()->AsPhi());
  }

  // Handle instructions.
  for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
    HInstruction* instr = it.Current();
    instr->Accept(&visitor);
  }

  // Add extra nodes to bound types.
  BoundTypeForIfNotNull(block);
  BoundTypeForIfInstanceOf(block);
}

// Create a bound type for the given object narrowing the type as much as possible.
// The BoundType upper values for the super type and can_be_null will be taken from
// load_class.GetLoadedClassRTI() and upper_can_be_null.
static HBoundType* CreateBoundType(ArenaAllocator* arena,
                                   HInstruction* obj,
                                   HLoadClass* load_class,
                                   bool upper_can_be_null)
      SHARED_REQUIRES(Locks::mutator_lock_) {
  ReferenceTypeInfo obj_rti = obj->GetReferenceTypeInfo();
  ReferenceTypeInfo class_rti = load_class->GetLoadedClassRTI();
  DCHECK(class_rti.IsValid());
  HBoundType* bound_type = new (arena) HBoundType(obj, class_rti, upper_can_be_null);
  // Narrow the type as much as possible.
  if (class_rti.GetTypeHandle()->CannotBeAssignedFromOtherTypes()) {
    bound_type->SetReferenceTypeInfo(
        ReferenceTypeInfo::Create(class_rti.GetTypeHandle(), /* is_exact */ true));
  } else if (obj_rti.IsValid() && class_rti.IsSupertypeOf(obj_rti)) {
    bound_type->SetReferenceTypeInfo(obj_rti);
  } else {
    bound_type->SetReferenceTypeInfo(
        ReferenceTypeInfo::Create(class_rti.GetTypeHandle(), /* is_exact */ false));
  }
  if (upper_can_be_null) {
    bound_type->SetCanBeNull(obj->CanBeNull());
  }
  return bound_type;
}

// Check if we should create a bound type for the given object at the specified
// position. Because of inlining and the fact we run RTP more than once and we
// might have a HBoundType already. If we do, we should not create a new one.
// In this case we also assert that there are no other uses of the object (except
// the bound type) dominated by the specified dominator_instr or dominator_block.
static bool ShouldCreateBoundType(HInstruction* position,
                                  HInstruction* obj,
                                  ReferenceTypeInfo upper_bound,
                                  HInstruction* dominator_instr,
                                  HBasicBlock* dominator_block)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  // If the position where we should insert the bound type is not already a
  // a bound type then we need to create one.
  if (position == nullptr || !position->IsBoundType()) {
    return true;
  }

  HBoundType* existing_bound_type = position->AsBoundType();
  if (existing_bound_type->GetUpperBound().IsSupertypeOf(upper_bound)) {
    if (kIsDebugBuild) {
      // Check that the existing HBoundType dominates all the uses.
      for (HUseIterator<HInstruction*> it(obj->GetUses()); !it.Done(); it.Advance()) {
        HInstruction* user = it.Current()->GetUser();
        if (dominator_instr != nullptr) {
          DCHECK(!dominator_instr->StrictlyDominates(user)
              || user == existing_bound_type
              || existing_bound_type->StrictlyDominates(user));
        } else if (dominator_block != nullptr) {
          DCHECK(!dominator_block->Dominates(user->GetBlock())
              || user == existing_bound_type
              || existing_bound_type->StrictlyDominates(user));
        }
      }
    }
  } else {
    // TODO: if the current bound type is a refinement we could update the
    // existing_bound_type with the a new upper limit. However, we also need to
    // update its users and have access to the work list.
  }
  return false;
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

  if (!obj->CanBeNull() || obj->IsNullConstant()) {
    // Null check is dead code and will be removed by DCE.
    return;
  }
  DCHECK(!obj->IsLoadClass()) << "We should not replace HLoadClass instructions";

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
        ScopedObjectAccess soa(Thread::Current());
        HInstruction* insert_point = notNullBlock->GetFirstInstruction();
        ReferenceTypeInfo object_rti = ReferenceTypeInfo::Create(
            object_class_handle_, /* is_exact */ true);
        if (ShouldCreateBoundType(insert_point, obj, object_rti, nullptr, notNullBlock)) {
          bound_type = new (graph_->GetArena()) HBoundType(
              obj, object_rti, /* bound_can_be_null */ false);
          if (obj->GetReferenceTypeInfo().IsValid()) {
            bound_type->SetReferenceTypeInfo(obj->GetReferenceTypeInfo());
          }
          notNullBlock->InsertInstructionBefore(bound_type, insert_point);
        } else {
          // We already have a bound type on the position we would need to insert
          // the new one. The existing bound type should dominate all the users
          // (dchecked) so there's no need to continue.
          break;
        }
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

  HLoadClass* load_class = instanceOf->InputAt(1)->AsLoadClass();
  ReferenceTypeInfo class_rti = load_class->GetLoadedClassRTI();
  {
    ScopedObjectAccess soa(Thread::Current());
    if (!class_rti.IsValid()) {
      // He have loaded an unresolved class. Don't bother bounding the type.
      return;
    }
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
  DCHECK(!obj->IsLoadClass()) << "We should not replace HLoadClass instructions";
  for (HUseIterator<HInstruction*> it(obj->GetUses()); !it.Done(); it.Advance()) {
    HInstruction* user = it.Current()->GetUser();
    if (instanceOfTrueBlock->Dominates(user->GetBlock())) {
      if (bound_type == nullptr) {
        ScopedObjectAccess soa(Thread::Current());
        HInstruction* insert_point = instanceOfTrueBlock->GetFirstInstruction();
        if (ShouldCreateBoundType(insert_point, obj, class_rti, nullptr, instanceOfTrueBlock)) {
          bound_type = CreateBoundType(
              graph_->GetArena(),
              obj,
              load_class,
              false /* InstanceOf ensures the object is not null. */);
          instanceOfTrueBlock->InsertInstructionBefore(bound_type, insert_point);
        } else {
          // We already have a bound type on the position we would need to insert
          // the new one. The existing bound type should dominate all the users
          // (dchecked) so there's no need to continue.
          break;
        }
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
      HInvoke* invoke = instr->AsInvoke();
      ClassLinker* cl = Runtime::Current()->GetClassLinker();
      ScopedObjectAccess soa(Thread::Current());
      StackHandleScope<2> hs(soa.Self());
      Handle<mirror::DexCache> dex_cache(
          hs.NewHandle(cl->FindDexCache(soa.Self(), invoke->GetDexFile(), false)));
      // Use a null loader. We should probably use the compiling method's class loader,
      // but then we would need to pass it to RTPVisitor just for this debug check. Since
      // the method is from the String class, the null loader is good enough.
      Handle<mirror::ClassLoader> loader;
      ArtMethod* method = cl->ResolveMethod(
          invoke->GetDexFile(), invoke->GetDexMethodIndex(), dex_cache, loader, nullptr, kDirect);
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
    is_exact = is_exact || klass->CannotBeAssignedFromOtherTypes();
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
  mirror::DexCache* dex_cache = Runtime::Current()->GetClassLinker()->FindDexCache(
      soa.Self(), dex_file, false);
  // Get type from dex cache assuming it was populated by the verifier.
  SetClassAsTypeInfo(instr, dex_cache->GetResolvedType(type_idx), is_exact);
}

void RTPVisitor::VisitNullConstant(HNullConstant* instr) {
  // TODO: The null constant could be bound contextually (e.g. based on return statements)
  // to a more precise type.
  instr->SetReferenceTypeInfo(
      ReferenceTypeInfo::Create(object_class_handle_, /* is_exact */ false));
}

void RTPVisitor::VisitNewInstance(HNewInstance* instr) {
  UpdateReferenceTypeInfo(instr, instr->GetTypeIndex(), instr->GetDexFile(), /* is_exact */ true);
}

void RTPVisitor::VisitNewArray(HNewArray* instr) {
  UpdateReferenceTypeInfo(instr, instr->GetTypeIndex(), instr->GetDexFile(), /* is_exact */ true);
}

static mirror::Class* GetClassFromDexCache(Thread* self, const DexFile& dex_file, uint16_t type_idx)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  mirror::DexCache* dex_cache =
      Runtime::Current()->GetClassLinker()->FindDexCache(self, dex_file, false);
  // Get type from dex cache assuming it was populated by the verifier.
  return dex_cache->GetResolvedType(type_idx);
}

void RTPVisitor::VisitParameterValue(HParameterValue* instr) {
  ScopedObjectAccess soa(Thread::Current());
  // We check if the existing type is valid: the inliner may have set it.
  if (instr->GetType() == Primitive::kPrimNot && !instr->GetReferenceTypeInfo().IsValid()) {
    mirror::Class* resolved_class =
        GetClassFromDexCache(soa.Self(), instr->GetDexFile(), instr->GetTypeIndex());
    SetClassAsTypeInfo(instr, resolved_class, /* is_exact */ false);
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
  ArtField* field = cl->GetResolvedField(info.GetFieldIndex(), info.GetDexCache().Get());
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

void RTPVisitor::VisitUnresolvedInstanceFieldGet(HUnresolvedInstanceFieldGet* instr) {
  // TODO: Use descriptor to get the actual type.
  if (instr->GetFieldType() == Primitive::kPrimNot) {
    instr->SetReferenceTypeInfo(
      ReferenceTypeInfo::Create(object_class_handle_, /* is_exact */ false));
  }
}

void RTPVisitor::VisitUnresolvedStaticFieldGet(HUnresolvedStaticFieldGet* instr) {
  // TODO: Use descriptor to get the actual type.
  if (instr->GetFieldType() == Primitive::kPrimNot) {
    instr->SetReferenceTypeInfo(
      ReferenceTypeInfo::Create(object_class_handle_, /* is_exact */ false));
  }
}

void RTPVisitor::VisitLoadClass(HLoadClass* instr) {
  ScopedObjectAccess soa(Thread::Current());
  // Get type from dex cache assuming it was populated by the verifier.
  mirror::Class* resolved_class =
      GetClassFromDexCache(soa.Self(), instr->GetDexFile(), instr->GetTypeIndex());
  if (resolved_class != nullptr) {
    instr->SetLoadedClassRTI(ReferenceTypeInfo::Create(
        handles_->NewHandle(resolved_class), /* is_exact */ true));
  }
  instr->SetReferenceTypeInfo(ReferenceTypeInfo::Create(class_class_handle_, /* is_exact */ true));
}

void RTPVisitor::VisitClinitCheck(HClinitCheck* instr) {
  instr->SetReferenceTypeInfo(instr->InputAt(0)->GetReferenceTypeInfo());
}

void RTPVisitor::VisitLoadString(HLoadString* instr) {
  instr->SetReferenceTypeInfo(ReferenceTypeInfo::Create(string_class_handle_, /* is_exact */ true));
}

void RTPVisitor::VisitLoadException(HLoadException* instr) {
  DCHECK(instr->GetBlock()->IsCatchBlock());
  TryCatchInformation* catch_info = instr->GetBlock()->GetTryCatchInformation();

  if (catch_info->IsCatchAllTypeIndex()) {
    instr->SetReferenceTypeInfo(ReferenceTypeInfo::Create(throwable_class_handle_,
                                                          /* is_exact */ false));
  } else {
    UpdateReferenceTypeInfo(instr,
                            catch_info->GetCatchTypeIndex(),
                            catch_info->GetCatchDexFile(),
                            /* is_exact */ false);
  }
}

void RTPVisitor::VisitNullCheck(HNullCheck* instr) {
  ScopedObjectAccess soa(Thread::Current());
  ReferenceTypeInfo parent_rti = instr->InputAt(0)->GetReferenceTypeInfo();
  DCHECK(parent_rti.IsValid());
  instr->SetReferenceTypeInfo(parent_rti);
}

void RTPVisitor::VisitFakeString(HFakeString* instr) {
  instr->SetReferenceTypeInfo(ReferenceTypeInfo::Create(string_class_handle_, /* is_exact */ true));
}

void RTPVisitor::VisitCheckCast(HCheckCast* check_cast) {
  HLoadClass* load_class = check_cast->InputAt(1)->AsLoadClass();
  ReferenceTypeInfo class_rti = load_class->GetLoadedClassRTI();
  {
    ScopedObjectAccess soa(Thread::Current());
    if (!class_rti.IsValid()) {
      // He have loaded an unresolved class. Don't bother bounding the type.
      return;
    }
  }
  HInstruction* obj = check_cast->InputAt(0);
  HBoundType* bound_type = nullptr;
  for (HUseIterator<HInstruction*> it(obj->GetUses()); !it.Done(); it.Advance()) {
    HInstruction* user = it.Current()->GetUser();
    if (check_cast->StrictlyDominates(user)) {
      if (bound_type == nullptr) {
        ScopedObjectAccess soa(Thread::Current());
        if (ShouldCreateBoundType(check_cast->GetNext(), obj, class_rti, check_cast, nullptr)) {
          bound_type = CreateBoundType(
              GetGraph()->GetArena(),
              obj,
              load_class,
              true /* CheckCast succeeds for nulls. */);
          check_cast->GetBlock()->InsertInstructionAfter(bound_type, check_cast);
        } else {
          // Update nullability of the existing bound type, which may not have known
          // that its input was not null when it was being created.
          bound_type = check_cast->GetNext()->AsBoundType();
          bound_type->SetCanBeNull(obj->CanBeNull());
          // We already have a bound type on the position we would need to insert
          // the new one. The existing bound type should dominate all the users
          // (dchecked) so there's no need to continue.
          break;
        }
      }
      user->ReplaceInput(bound_type, it.Current()->GetIndex());
    }
  }
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
  ReferenceTypeInfo::TypeHandle result_type_handle;
  ReferenceTypeInfo::TypeHandle a_type_handle = a.GetTypeHandle();
  ReferenceTypeInfo::TypeHandle b_type_handle = b.GetTypeHandle();
  bool a_is_interface = a_type_handle->IsInterface();
  bool b_is_interface = b_type_handle->IsInterface();

  if (a.GetTypeHandle().Get() == b.GetTypeHandle().Get()) {
    result_type_handle = a_type_handle;
  } else if (a.IsSupertypeOf(b)) {
    result_type_handle = a_type_handle;
    is_exact = false;
  } else if (b.IsSupertypeOf(a)) {
    result_type_handle = b_type_handle;
    is_exact = false;
  } else if (!a_is_interface && !b_is_interface) {
    result_type_handle = handles_->NewHandle(a_type_handle->GetCommonSuperClass(b_type_handle));
    is_exact = false;
  } else {
    // This can happen if:
    //    - both types are interfaces. TODO(calin): implement
    //    - one is an interface, the other a class, and the type does not implement the interface
    //      e.g:
    //        void foo(Interface i, boolean cond) {
    //          Object o = cond ? i : new Object();
    //        }
    result_type_handle = object_class_handle_;
    is_exact = false;
  }

  return ReferenceTypeInfo::Create(result_type_handle, is_exact);
}

static void UpdateArrayGet(HArrayGet* instr,
                           StackHandleScopeCollection* handles,
                           ReferenceTypeInfo::TypeHandle object_class_handle)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  DCHECK_EQ(Primitive::kPrimNot, instr->GetType());

  ReferenceTypeInfo parent_rti = instr->InputAt(0)->GetReferenceTypeInfo();
  DCHECK(parent_rti.IsValid());

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
  mirror::DexCache* dex_cache = cl->FindDexCache(soa.Self(), instr->GetDexFile());
  size_t pointer_size = cl->GetImagePointerSize();
  ArtMethod* method = dex_cache->GetResolvedMethod(instr->GetDexMethodIndex(), pointer_size);
  mirror::Class* klass = (method == nullptr) ? nullptr : method->GetReturnType(false, pointer_size);
  SetClassAsTypeInfo(instr, klass, /* is_exact */ false);
}

void RTPVisitor::VisitArrayGet(HArrayGet* instr) {
  if (instr->GetType() != Primitive::kPrimNot) {
    return;
  }
  ScopedObjectAccess soa(Thread::Current());
  UpdateArrayGet(instr, handles_, object_class_handle_);
  if (!instr->GetReferenceTypeInfo().IsValid()) {
    worklist_->push_back(instr);
  }
}

void ReferenceTypePropagation::UpdateBoundType(HBoundType* instr) {
  ReferenceTypeInfo new_rti = instr->InputAt(0)->GetReferenceTypeInfo();
  if (!new_rti.IsValid()) {
    return;  // No new info yet.
  }

  // Make sure that we don't go over the bounded type.
  ReferenceTypeInfo upper_bound_rti = instr->GetUpperBound();
  if (!upper_bound_rti.IsSupertypeOf(new_rti)) {
    new_rti = upper_bound_rti;
  }
  instr->SetReferenceTypeInfo(new_rti);
}

// NullConstant inputs are ignored during merging as they do not provide any useful information.
// If all the inputs are NullConstants then the type of the phi will be set to Object.
void ReferenceTypePropagation::UpdatePhi(HPhi* instr) {
  size_t input_count = instr->InputCount();
  size_t first_input_index_not_null = 0;
  while (first_input_index_not_null < input_count &&
      instr->InputAt(first_input_index_not_null)->IsNullConstant()) {
    first_input_index_not_null++;
  }
  if (first_input_index_not_null == input_count) {
    // All inputs are NullConstants, set the type to object.
    // This may happen in the presence of inlining.
    instr->SetReferenceTypeInfo(
        ReferenceTypeInfo::Create(object_class_handle_, /* is_exact */ false));
    return;
  }

  ReferenceTypeInfo new_rti = instr->InputAt(first_input_index_not_null)->GetReferenceTypeInfo();

  if (new_rti.IsValid() && new_rti.IsObjectClass() && !new_rti.IsExact()) {
    // Early return if we are Object and inexact.
    instr->SetReferenceTypeInfo(new_rti);
    return;
  }

  for (size_t i = first_input_index_not_null + 1; i < input_count; i++) {
    if (instr->InputAt(i)->IsNullConstant()) {
      continue;
    }
    new_rti = MergeTypes(new_rti, instr->InputAt(i)->GetReferenceTypeInfo());
    if (new_rti.IsValid() && new_rti.IsObjectClass()) {
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

  if (!instr->IsPhi() && !instr->IsBoundType()) {
    return false;
  }

  bool existing_can_be_null = instr->CanBeNull();
  if (instr->IsPhi()) {
    HPhi* phi = instr->AsPhi();
    bool new_can_be_null = false;
    for (size_t i = 0; i < phi->InputCount(); i++) {
      if (phi->InputAt(i)->CanBeNull()) {
        new_can_be_null = true;
        break;
      }
    }
    phi->SetCanBeNull(new_can_be_null);
  } else if (instr->IsBoundType()) {
    HBoundType* bound_type = instr->AsBoundType();
    bound_type->SetCanBeNull(instr->InputAt(0)->CanBeNull() && bound_type->GetUpperCanBeNull());
  }
  return existing_can_be_null != instr->CanBeNull();
}

void ReferenceTypePropagation::ProcessWorklist() {
  while (!worklist_.empty()) {
    HInstruction* instruction = worklist_.back();
    worklist_.pop_back();
    bool updated_nullability = UpdateNullability(instruction);
    bool updated_reference_type = UpdateReferenceTypeInfo(instruction);
    if (updated_nullability || updated_reference_type) {
      AddDependentInstructionsToWorklist(instruction);
    }
  }
}

void ReferenceTypePropagation::AddToWorklist(HInstruction* instruction) {
  DCHECK_EQ(instruction->GetType(), Primitive::kPrimNot)
      << instruction->DebugName() << ":" << instruction->GetType();
  worklist_.push_back(instruction);
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
