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

static inline mirror::DexCache* FindDexCacheWithHint(Thread* self,
                                                     const DexFile& dex_file,
                                                     Handle<mirror::DexCache> hint_dex_cache)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  if (LIKELY(hint_dex_cache->GetDexFile() == &dex_file)) {
    return hint_dex_cache.Get();
  } else {
    return Runtime::Current()->GetClassLinker()->FindDexCache(self, dex_file);
  }
}

static inline ReferenceTypeInfo::TypeHandle GetRootHandle(StackHandleScopeCollection* handles,
                                                          ClassLinker::ClassRoot class_root,
                                                          ReferenceTypeInfo::TypeHandle* cache) {
  if (!ReferenceTypeInfo::IsValidHandle(*cache)) {
    // Mutator lock is required for NewHandle.
    ClassLinker* linker = Runtime::Current()->GetClassLinker();
    ScopedObjectAccess soa(Thread::Current());
    *cache = handles->NewHandle(linker->GetClassRoot(class_root));
  }
  return *cache;
}

// Returns true if klass is admissible to the propagation: non-null and resolved.
// For an array type, we also check if the component type is admissible.
static bool IsAdmissible(mirror::Class* klass) SHARED_REQUIRES(Locks::mutator_lock_) {
  return klass != nullptr && klass->IsResolved() &&
      (!klass->IsArrayClass() || IsAdmissible(klass->GetComponentType()));
}

ReferenceTypeInfo::TypeHandle ReferenceTypePropagation::HandleCache::GetObjectClassHandle() {
  return GetRootHandle(handles_, ClassLinker::kJavaLangObject, &object_class_handle_);
}

ReferenceTypeInfo::TypeHandle ReferenceTypePropagation::HandleCache::GetClassClassHandle() {
  return GetRootHandle(handles_, ClassLinker::kJavaLangClass, &class_class_handle_);
}

ReferenceTypeInfo::TypeHandle ReferenceTypePropagation::HandleCache::GetStringClassHandle() {
  return GetRootHandle(handles_, ClassLinker::kJavaLangString, &string_class_handle_);
}

ReferenceTypeInfo::TypeHandle ReferenceTypePropagation::HandleCache::GetThrowableClassHandle() {
  return GetRootHandle(handles_, ClassLinker::kJavaLangThrowable, &throwable_class_handle_);
}

class ReferenceTypePropagation::RTPVisitor : public HGraphDelegateVisitor {
 public:
  RTPVisitor(HGraph* graph,
             Handle<mirror::DexCache> hint_dex_cache,
             HandleCache* handle_cache,
             ArenaVector<HInstruction*>* worklist,
             bool is_first_run)
    : HGraphDelegateVisitor(graph),
      hint_dex_cache_(hint_dex_cache),
      handle_cache_(handle_cache),
      worklist_(worklist),
      is_first_run_(is_first_run) {}

  void VisitNewInstance(HNewInstance* new_instance) OVERRIDE;
  void VisitLoadClass(HLoadClass* load_class) OVERRIDE;
  void VisitClinitCheck(HClinitCheck* clinit_check) OVERRIDE;
  void VisitLoadString(HLoadString* instr) OVERRIDE;
  void VisitLoadException(HLoadException* instr) OVERRIDE;
  void VisitNewArray(HNewArray* instr) OVERRIDE;
  void VisitParameterValue(HParameterValue* instr) OVERRIDE;
  void UpdateFieldAccessTypeInfo(HInstruction* instr, const FieldInfo& info);
  void SetClassAsTypeInfo(HInstruction* instr, mirror::Class* klass, bool is_exact)
      SHARED_REQUIRES(Locks::mutator_lock_);
  void VisitInstanceFieldGet(HInstanceFieldGet* instr) OVERRIDE;
  void VisitStaticFieldGet(HStaticFieldGet* instr) OVERRIDE;
  void VisitUnresolvedInstanceFieldGet(HUnresolvedInstanceFieldGet* instr) OVERRIDE;
  void VisitUnresolvedStaticFieldGet(HUnresolvedStaticFieldGet* instr) OVERRIDE;
  void VisitInvoke(HInvoke* instr) OVERRIDE;
  void VisitArrayGet(HArrayGet* instr) OVERRIDE;
  void VisitCheckCast(HCheckCast* instr) OVERRIDE;
  void VisitBoundType(HBoundType* instr) OVERRIDE;
  void VisitNullCheck(HNullCheck* instr) OVERRIDE;
  void UpdateReferenceTypeInfo(HInstruction* instr,
                               uint16_t type_idx,
                               const DexFile& dex_file,
                               bool is_exact);

 private:
  Handle<mirror::DexCache> hint_dex_cache_;
  HandleCache* handle_cache_;
  ArenaVector<HInstruction*>* worklist_;
  const bool is_first_run_;
};

ReferenceTypePropagation::ReferenceTypePropagation(HGraph* graph,
                                                   Handle<mirror::DexCache> hint_dex_cache,
                                                   StackHandleScopeCollection* handles,
                                                   bool is_first_run,
                                                   const char* name)
    : HOptimization(graph, name),
      hint_dex_cache_(hint_dex_cache),
      handle_cache_(handles),
      worklist_(graph->GetArena()->Adapter(kArenaAllocReferenceTypePropagation)),
      is_first_run_(is_first_run) {
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

void ReferenceTypePropagation::Visit(HInstruction* instruction) {
  RTPVisitor visitor(graph_, hint_dex_cache_, &handle_cache_, &worklist_, is_first_run_);
  instruction->Accept(&visitor);
}

void ReferenceTypePropagation::Run() {
  worklist_.reserve(kDefaultWorklistSize);

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
  RTPVisitor visitor(graph_, hint_dex_cache_, &handle_cache_, &worklist_, is_first_run_);
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
      for (const HUseListNode<HInstruction*>& use : obj->GetUses()) {
        HInstruction* user = use.GetUser();
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

  const HUseList<HInstruction*>& uses = obj->GetUses();
  for (auto it = uses.begin(), end = uses.end(); it != end; /* ++it below */) {
    HInstruction* user = it->GetUser();
    size_t index = it->GetIndex();
    // Increment `it` now because `*it` may disappear thanks to user->ReplaceInput().
    ++it;
    if (notNullBlock->Dominates(user->GetBlock())) {
      if (bound_type == nullptr) {
        ScopedObjectAccess soa(Thread::Current());
        HInstruction* insert_point = notNullBlock->GetFirstInstruction();
        ReferenceTypeInfo object_rti = ReferenceTypeInfo::Create(
            handle_cache_.GetObjectClassHandle(), /* is_exact */ true);
        if (ShouldCreateBoundType(insert_point, obj, object_rti, nullptr, notNullBlock)) {
          bound_type = new (graph_->GetArena()) HBoundType(obj);
          bound_type->SetUpperBound(object_rti, /* bound_can_be_null */ false);
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
      user->ReplaceInput(bound_type, index);
    }
  }
}

// Returns true if one of the patterns below has been recognized. If so, the
// InstanceOf instruction together with the true branch of `ifInstruction` will
// be returned using the out parameters.
// Recognized patterns:
//   (1) patterns equivalent to `if (obj instanceof X)`
//     (a) InstanceOf -> Equal to 1 -> If
//     (b) InstanceOf -> NotEqual to 0 -> If
//     (c) InstanceOf -> If
//   (2) patterns equivalent to `if (!(obj instanceof X))`
//     (a) InstanceOf -> Equal to 0 -> If
//     (b) InstanceOf -> NotEqual to 1 -> If
//     (c) InstanceOf -> BooleanNot -> If
static bool MatchIfInstanceOf(HIf* ifInstruction,
                              /* out */ HInstanceOf** instanceOf,
                              /* out */ HBasicBlock** trueBranch) {
  HInstruction* input = ifInstruction->InputAt(0);

  if (input->IsEqual()) {
    HInstruction* rhs = input->AsEqual()->GetConstantRight();
    if (rhs != nullptr) {
      HInstruction* lhs = input->AsEqual()->GetLeastConstantLeft();
      if (lhs->IsInstanceOf() && rhs->IsIntConstant()) {
        if (rhs->AsIntConstant()->IsTrue()) {
          // Case (1a)
          *trueBranch = ifInstruction->IfTrueSuccessor();
        } else {
          // Case (2a)
          DCHECK(rhs->AsIntConstant()->IsFalse()) << rhs->AsIntConstant()->GetValue();
          *trueBranch = ifInstruction->IfFalseSuccessor();
        }
        *instanceOf = lhs->AsInstanceOf();
        return true;
      }
    }
  } else if (input->IsNotEqual()) {
    HInstruction* rhs = input->AsNotEqual()->GetConstantRight();
    if (rhs != nullptr) {
      HInstruction* lhs = input->AsNotEqual()->GetLeastConstantLeft();
      if (lhs->IsInstanceOf() && rhs->IsIntConstant()) {
        if (rhs->AsIntConstant()->IsFalse()) {
          // Case (1b)
          *trueBranch = ifInstruction->IfTrueSuccessor();
        } else {
          // Case (2b)
          DCHECK(rhs->AsIntConstant()->IsTrue()) << rhs->AsIntConstant()->GetValue();
          *trueBranch = ifInstruction->IfFalseSuccessor();
        }
        *instanceOf = lhs->AsInstanceOf();
        return true;
      }
    }
  } else if (input->IsInstanceOf()) {
    // Case (1c)
    *instanceOf = input->AsInstanceOf();
    *trueBranch = ifInstruction->IfTrueSuccessor();
    return true;
  } else if (input->IsBooleanNot()) {
    HInstruction* not_input = input->InputAt(0);
    if (not_input->IsInstanceOf()) {
      // Case (2c)
      *instanceOf = not_input->AsInstanceOf();
      *trueBranch = ifInstruction->IfFalseSuccessor();
      return true;
    }
  }

  return false;
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

  // Try to recognize common `if (instanceof)` and `if (!instanceof)` patterns.
  HInstanceOf* instanceOf = nullptr;
  HBasicBlock* instanceOfTrueBlock = nullptr;
  if (!MatchIfInstanceOf(ifInstruction, &instanceOf, &instanceOfTrueBlock)) {
    return;
  }

  HLoadClass* load_class = instanceOf->InputAt(1)->AsLoadClass();
  ReferenceTypeInfo class_rti = load_class->GetLoadedClassRTI();
  {
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
  const HUseList<HInstruction*>& uses = obj->GetUses();
  for (auto it = uses.begin(), end = uses.end(); it != end; /* ++it below */) {
    HInstruction* user = it->GetUser();
    size_t index = it->GetIndex();
    // Increment `it` now because `*it` may disappear thanks to user->ReplaceInput().
    ++it;
    if (instanceOfTrueBlock->Dominates(user->GetBlock())) {
      if (bound_type == nullptr) {
        ScopedObjectAccess soa(Thread::Current());
        HInstruction* insert_point = instanceOfTrueBlock->GetFirstInstruction();
        if (ShouldCreateBoundType(insert_point, obj, class_rti, nullptr, instanceOfTrueBlock)) {
          bound_type = new (graph_->GetArena()) HBoundType(obj);
          bound_type->SetUpperBound(class_rti, /* InstanceOf fails for null. */ false);
          instanceOfTrueBlock->InsertInstructionBefore(bound_type, insert_point);
        } else {
          // We already have a bound type on the position we would need to insert
          // the new one. The existing bound type should dominate all the users
          // (dchecked) so there's no need to continue.
          break;
        }
      }
      user->ReplaceInput(bound_type, index);
    }
  }
}

void ReferenceTypePropagation::RTPVisitor::SetClassAsTypeInfo(HInstruction* instr,
                                                              mirror::Class* klass,
                                                              bool is_exact) {
  if (instr->IsInvokeStaticOrDirect() && instr->AsInvokeStaticOrDirect()->IsStringInit()) {
    // Calls to String.<init> are replaced with a StringFactory.
    if (kIsDebugBuild) {
      HInvoke* invoke = instr->AsInvoke();
      ClassLinker* cl = Runtime::Current()->GetClassLinker();
      Thread* self = Thread::Current();
      StackHandleScope<2> hs(self);
      Handle<mirror::DexCache> dex_cache(
          hs.NewHandle(FindDexCacheWithHint(self, invoke->GetDexFile(), hint_dex_cache_)));
      // Use a null loader. We should probably use the compiling method's class loader,
      // but then we would need to pass it to RTPVisitor just for this debug check. Since
      // the method is from the String class, the null loader is good enough.
      Handle<mirror::ClassLoader> loader;
      ArtMethod* method = cl->ResolveMethod<ClassLinker::kNoICCECheckForCache>(
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
        ReferenceTypeInfo::Create(handle_cache_->GetStringClassHandle(), /* is_exact */ true));
  } else if (IsAdmissible(klass)) {
    ReferenceTypeInfo::TypeHandle handle = handle_cache_->NewHandle(klass);
    is_exact = is_exact || handle->CannotBeAssignedFromOtherTypes();
    instr->SetReferenceTypeInfo(ReferenceTypeInfo::Create(handle, is_exact));
  } else {
    instr->SetReferenceTypeInfo(instr->GetBlock()->GetGraph()->GetInexactObjectRti());
  }
}

void ReferenceTypePropagation::RTPVisitor::UpdateReferenceTypeInfo(HInstruction* instr,
                                                                   uint16_t type_idx,
                                                                   const DexFile& dex_file,
                                                                   bool is_exact) {
  DCHECK_EQ(instr->GetType(), Primitive::kPrimNot);

  ScopedObjectAccess soa(Thread::Current());
  mirror::DexCache* dex_cache = FindDexCacheWithHint(soa.Self(), dex_file, hint_dex_cache_);
  // Get type from dex cache assuming it was populated by the verifier.
  SetClassAsTypeInfo(instr, dex_cache->GetResolvedType(type_idx), is_exact);
}

void ReferenceTypePropagation::RTPVisitor::VisitNewInstance(HNewInstance* instr) {
  UpdateReferenceTypeInfo(instr, instr->GetTypeIndex(), instr->GetDexFile(), /* is_exact */ true);
}

void ReferenceTypePropagation::RTPVisitor::VisitNewArray(HNewArray* instr) {
  UpdateReferenceTypeInfo(instr, instr->GetTypeIndex(), instr->GetDexFile(), /* is_exact */ true);
}

static mirror::Class* GetClassFromDexCache(Thread* self,
                                           const DexFile& dex_file,
                                           uint16_t type_idx,
                                           Handle<mirror::DexCache> hint_dex_cache)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  mirror::DexCache* dex_cache = FindDexCacheWithHint(self, dex_file, hint_dex_cache);
  // Get type from dex cache assuming it was populated by the verifier.
  return dex_cache->GetResolvedType(type_idx);
}

void ReferenceTypePropagation::RTPVisitor::VisitParameterValue(HParameterValue* instr) {
  // We check if the existing type is valid: the inliner may have set it.
  if (instr->GetType() == Primitive::kPrimNot && !instr->GetReferenceTypeInfo().IsValid()) {
    ScopedObjectAccess soa(Thread::Current());
    mirror::Class* resolved_class = GetClassFromDexCache(soa.Self(),
                                                         instr->GetDexFile(),
                                                         instr->GetTypeIndex(),
                                                         hint_dex_cache_);
    SetClassAsTypeInfo(instr, resolved_class, /* is_exact */ false);
  }
}

void ReferenceTypePropagation::RTPVisitor::UpdateFieldAccessTypeInfo(HInstruction* instr,
                                                                     const FieldInfo& info) {
  if (instr->GetType() != Primitive::kPrimNot) {
    return;
  }

  ScopedObjectAccess soa(Thread::Current());
  mirror::Class* klass = nullptr;

  // The field index is unknown only during tests.
  if (info.GetFieldIndex() != kUnknownFieldIndex) {
    ClassLinker* cl = Runtime::Current()->GetClassLinker();
    ArtField* field = cl->GetResolvedField(info.GetFieldIndex(), info.GetDexCache().Get());
    // TODO: There are certain cases where we can't resolve the field.
    // b/21914925 is open to keep track of a repro case for this issue.
    if (field != nullptr) {
      klass = field->GetType<false>();
    }
  }

  SetClassAsTypeInfo(instr, klass, /* is_exact */ false);
}

void ReferenceTypePropagation::RTPVisitor::VisitInstanceFieldGet(HInstanceFieldGet* instr) {
  UpdateFieldAccessTypeInfo(instr, instr->GetFieldInfo());
}

void ReferenceTypePropagation::RTPVisitor::VisitStaticFieldGet(HStaticFieldGet* instr) {
  UpdateFieldAccessTypeInfo(instr, instr->GetFieldInfo());
}

void ReferenceTypePropagation::RTPVisitor::VisitUnresolvedInstanceFieldGet(
    HUnresolvedInstanceFieldGet* instr) {
  // TODO: Use descriptor to get the actual type.
  if (instr->GetFieldType() == Primitive::kPrimNot) {
    instr->SetReferenceTypeInfo(instr->GetBlock()->GetGraph()->GetInexactObjectRti());
  }
}

void ReferenceTypePropagation::RTPVisitor::VisitUnresolvedStaticFieldGet(
    HUnresolvedStaticFieldGet* instr) {
  // TODO: Use descriptor to get the actual type.
  if (instr->GetFieldType() == Primitive::kPrimNot) {
    instr->SetReferenceTypeInfo(instr->GetBlock()->GetGraph()->GetInexactObjectRti());
  }
}

void ReferenceTypePropagation::RTPVisitor::VisitLoadClass(HLoadClass* instr) {
  ScopedObjectAccess soa(Thread::Current());
  // Get type from dex cache assuming it was populated by the verifier.
  mirror::Class* resolved_class = GetClassFromDexCache(soa.Self(),
                                                       instr->GetDexFile(),
                                                       instr->GetTypeIndex(),
                                                       hint_dex_cache_);
  if (IsAdmissible(resolved_class)) {
    instr->SetLoadedClassRTI(ReferenceTypeInfo::Create(
        handle_cache_->NewHandle(resolved_class), /* is_exact */ true));
  }
  instr->SetReferenceTypeInfo(
      ReferenceTypeInfo::Create(handle_cache_->GetClassClassHandle(), /* is_exact */ true));
}

void ReferenceTypePropagation::RTPVisitor::VisitClinitCheck(HClinitCheck* instr) {
  instr->SetReferenceTypeInfo(instr->InputAt(0)->GetReferenceTypeInfo());
}

void ReferenceTypePropagation::RTPVisitor::VisitLoadString(HLoadString* instr) {
  instr->SetReferenceTypeInfo(
      ReferenceTypeInfo::Create(handle_cache_->GetStringClassHandle(), /* is_exact */ true));
}

void ReferenceTypePropagation::RTPVisitor::VisitLoadException(HLoadException* instr) {
  DCHECK(instr->GetBlock()->IsCatchBlock());
  TryCatchInformation* catch_info = instr->GetBlock()->GetTryCatchInformation();

  if (catch_info->IsCatchAllTypeIndex()) {
    instr->SetReferenceTypeInfo(
        ReferenceTypeInfo::Create(handle_cache_->GetThrowableClassHandle(), /* is_exact */ false));
  } else {
    UpdateReferenceTypeInfo(instr,
                            catch_info->GetCatchTypeIndex(),
                            catch_info->GetCatchDexFile(),
                            /* is_exact */ false);
  }
}

void ReferenceTypePropagation::RTPVisitor::VisitNullCheck(HNullCheck* instr) {
  ReferenceTypeInfo parent_rti = instr->InputAt(0)->GetReferenceTypeInfo();
  if (parent_rti.IsValid()) {
    instr->SetReferenceTypeInfo(parent_rti);
  }
}

void ReferenceTypePropagation::RTPVisitor::VisitBoundType(HBoundType* instr) {
  ReferenceTypeInfo class_rti = instr->GetUpperBound();
  if (class_rti.IsValid()) {
    ScopedObjectAccess soa(Thread::Current());
    // Narrow the type as much as possible.
    HInstruction* obj = instr->InputAt(0);
    ReferenceTypeInfo obj_rti = obj->GetReferenceTypeInfo();
    if (class_rti.GetTypeHandle()->CannotBeAssignedFromOtherTypes()) {
      instr->SetReferenceTypeInfo(
          ReferenceTypeInfo::Create(class_rti.GetTypeHandle(), /* is_exact */ true));
    } else if (obj_rti.IsValid()) {
      if (class_rti.IsSupertypeOf(obj_rti)) {
        // Object type is more specific.
        instr->SetReferenceTypeInfo(obj_rti);
      } else {
        // Upper bound is more specific.
        instr->SetReferenceTypeInfo(
            ReferenceTypeInfo::Create(class_rti.GetTypeHandle(), /* is_exact */ false));
      }
    } else {
      // Object not typed yet. Leave BoundType untyped for now rather than
      // assign the type conservatively.
    }
    instr->SetCanBeNull(obj->CanBeNull() && instr->GetUpperCanBeNull());
  } else {
    // The owner of the BoundType was already visited. If the class is unresolved,
    // the BoundType should have been removed from the data flow and this method
    // should remove it from the graph.
    DCHECK(!instr->HasUses());
    instr->GetBlock()->RemoveInstruction(instr);
  }
}

void ReferenceTypePropagation::RTPVisitor::VisitCheckCast(HCheckCast* check_cast) {
  HLoadClass* load_class = check_cast->InputAt(1)->AsLoadClass();
  ReferenceTypeInfo class_rti = load_class->GetLoadedClassRTI();
  HBoundType* bound_type = check_cast->GetNext()->AsBoundType();
  if (bound_type == nullptr || bound_type->GetUpperBound().IsValid()) {
    // The next instruction is not an uninitialized BoundType. This must be
    // an RTP pass after SsaBuilder and we do not need to do anything.
    return;
  }
  DCHECK_EQ(bound_type->InputAt(0), check_cast->InputAt(0));

  if (class_rti.IsValid()) {
    DCHECK(is_first_run_);
    // This is the first run of RTP and class is resolved.
    bound_type->SetUpperBound(class_rti, /* CheckCast succeeds for nulls. */ true);
  } else {
    // This is the first run of RTP and class is unresolved. Remove the binding.
    // The instruction itself is removed in VisitBoundType so as to not
    // invalidate HInstructionIterator.
    bound_type->ReplaceWith(bound_type->InputAt(0));
  }
}

void ReferenceTypePropagation::VisitPhi(HPhi* phi) {
  if (phi->IsDead() || phi->GetType() != Primitive::kPrimNot) {
    return;
  }

  if (phi->GetBlock()->IsLoopHeader()) {
    // Set the initial type for the phi. Use the non back edge input for reaching
    // a fixed point faster.
    HInstruction* first_input = phi->InputAt(0);
    ReferenceTypeInfo first_input_rti = first_input->GetReferenceTypeInfo();
    if (first_input_rti.IsValid() && !first_input->IsNullConstant()) {
      phi->SetCanBeNull(first_input->CanBeNull());
      phi->SetReferenceTypeInfo(first_input_rti);
    }
    AddToWorklist(phi);
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
    result_type_handle =
        handle_cache_.NewHandle(a_type_handle->GetCommonSuperClass(b_type_handle));
    is_exact = false;
  } else {
    // This can happen if:
    //    - both types are interfaces. TODO(calin): implement
    //    - one is an interface, the other a class, and the type does not implement the interface
    //      e.g:
    //        void foo(Interface i, boolean cond) {
    //          Object o = cond ? i : new Object();
    //        }
    result_type_handle = handle_cache_.GetObjectClassHandle();
    is_exact = false;
  }

  return ReferenceTypeInfo::Create(result_type_handle, is_exact);
}

void ReferenceTypePropagation::UpdateArrayGet(HArrayGet* instr, HandleCache* handle_cache) {
  DCHECK_EQ(Primitive::kPrimNot, instr->GetType());

  ReferenceTypeInfo parent_rti = instr->InputAt(0)->GetReferenceTypeInfo();
  if (!parent_rti.IsValid()) {
    return;
  }

  Handle<mirror::Class> handle = parent_rti.GetTypeHandle();
  if (handle->IsObjectArrayClass() && IsAdmissible(handle->GetComponentType())) {
    ReferenceTypeInfo::TypeHandle component_handle =
        handle_cache->NewHandle(handle->GetComponentType());
    bool is_exact = component_handle->CannotBeAssignedFromOtherTypes();
    instr->SetReferenceTypeInfo(ReferenceTypeInfo::Create(component_handle, is_exact));
  } else {
    // We don't know what the parent actually is, so we fallback to object.
    instr->SetReferenceTypeInfo(instr->GetBlock()->GetGraph()->GetInexactObjectRti());
  }
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
    // TODO: consider if it's worth "looking back" and binding the input object
    // to an array type.
    UpdateArrayGet(instr->AsArrayGet(), &handle_cache_);
  } else {
    LOG(FATAL) << "Invalid instruction (should not get here)";
  }

  return !previous_rti.IsEqual(instr->GetReferenceTypeInfo());
}

void ReferenceTypePropagation::RTPVisitor::VisitInvoke(HInvoke* instr) {
  if (instr->GetType() != Primitive::kPrimNot) {
    return;
  }

  ScopedObjectAccess soa(Thread::Current());
  ClassLinker* cl = Runtime::Current()->GetClassLinker();
  mirror::DexCache* dex_cache =
      FindDexCacheWithHint(soa.Self(), instr->GetDexFile(), hint_dex_cache_);
  size_t pointer_size = cl->GetImagePointerSize();
  ArtMethod* method = dex_cache->GetResolvedMethod(instr->GetDexMethodIndex(), pointer_size);
  mirror::Class* klass = (method == nullptr) ? nullptr : method->GetReturnType(false, pointer_size);
  SetClassAsTypeInfo(instr, klass, /* is_exact */ false);
}

void ReferenceTypePropagation::RTPVisitor::VisitArrayGet(HArrayGet* instr) {
  if (instr->GetType() != Primitive::kPrimNot) {
    return;
  }

  ScopedObjectAccess soa(Thread::Current());
  UpdateArrayGet(instr, handle_cache_);
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
    // Note that the input might be exact, in which case we know the branch leading
    // to the bound type is dead. We play it safe by not marking the bound type as
    // exact.
    bool is_exact = upper_bound_rti.GetTypeHandle()->CannotBeAssignedFromOtherTypes();
    new_rti = ReferenceTypeInfo::Create(upper_bound_rti.GetTypeHandle(), is_exact);
  }
  instr->SetReferenceTypeInfo(new_rti);
}

// NullConstant inputs are ignored during merging as they do not provide any useful information.
// If all the inputs are NullConstants then the type of the phi will be set to Object.
void ReferenceTypePropagation::UpdatePhi(HPhi* instr) {
  DCHECK(instr->IsLive());

  size_t input_count = instr->InputCount();
  size_t first_input_index_not_null = 0;
  while (first_input_index_not_null < input_count &&
      instr->InputAt(first_input_index_not_null)->IsNullConstant()) {
    first_input_index_not_null++;
  }
  if (first_input_index_not_null == input_count) {
    // All inputs are NullConstants, set the type to object.
    // This may happen in the presence of inlining.
    instr->SetReferenceTypeInfo(instr->GetBlock()->GetGraph()->GetInexactObjectRti());
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

  if (new_rti.IsValid()) {
    instr->SetReferenceTypeInfo(new_rti);
  }
}

// Re-computes and updates the nullability of the instruction. Returns whether or
// not the nullability was changed.
bool ReferenceTypePropagation::UpdateNullability(HInstruction* instr) {
  DCHECK((instr->IsPhi() && instr->AsPhi()->IsLive())
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
  for (const HUseListNode<HInstruction*>& use : instruction->GetUses()) {
    HInstruction* user = use.GetUser();
    if ((user->IsPhi() && user->AsPhi()->IsLive())
       || user->IsBoundType()
       || user->IsNullCheck()
       || (user->IsArrayGet() && (user->GetType() == Primitive::kPrimNot))) {
      AddToWorklist(user);
    }
  }
}

}  // namespace art
