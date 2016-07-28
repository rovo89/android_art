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

#include "inliner.h"

#include "art_method-inl.h"
#include "builder.h"
#include "class_linker.h"
#include "constant_folding.h"
#include "dead_code_elimination.h"
#include "dex/verified_method.h"
#include "dex/verification_results.h"
#include "driver/compiler_driver-inl.h"
#include "driver/compiler_options.h"
#include "driver/dex_compilation_unit.h"
#include "instruction_simplifier.h"
#include "intrinsics.h"
#include "jit/jit.h"
#include "jit/jit_code_cache.h"
#include "mirror/class_loader.h"
#include "mirror/dex_cache.h"
#include "nodes.h"
#include "optimizing_compiler.h"
#include "reference_type_propagation.h"
#include "register_allocator.h"
#include "quick/inline_method_analyser.h"
#include "sharpening.h"
#include "ssa_builder.h"
#include "ssa_phi_elimination.h"
#include "scoped_thread_state_change.h"
#include "thread.h"

namespace art {

static constexpr size_t kMaximumNumberOfHInstructions = 32;

// Limit the number of dex registers that we accumulate while inlining
// to avoid creating large amount of nested environments.
static constexpr size_t kMaximumNumberOfCumulatedDexRegisters = 64;

// Avoid inlining within a huge method due to memory pressure.
static constexpr size_t kMaximumCodeUnitSize = 4096;

void HInliner::Run() {
  const CompilerOptions& compiler_options = compiler_driver_->GetCompilerOptions();
  if ((compiler_options.GetInlineDepthLimit() == 0)
      || (compiler_options.GetInlineMaxCodeUnits() == 0)) {
    return;
  }
  if (caller_compilation_unit_.GetCodeItem()->insns_size_in_code_units_ > kMaximumCodeUnitSize) {
    return;
  }
  if (graph_->IsDebuggable()) {
    // For simplicity, we currently never inline when the graph is debuggable. This avoids
    // doing some logic in the runtime to discover if a method could have been inlined.
    return;
  }
  const ArenaVector<HBasicBlock*>& blocks = graph_->GetReversePostOrder();
  DCHECK(!blocks.empty());
  HBasicBlock* next_block = blocks[0];
  for (size_t i = 0; i < blocks.size(); ++i) {
    // Because we are changing the graph when inlining, we need to remember the next block.
    // This avoids doing the inlining work again on the inlined blocks.
    if (blocks[i] != next_block) {
      continue;
    }
    HBasicBlock* block = next_block;
    next_block = (i == blocks.size() - 1) ? nullptr : blocks[i + 1];
    for (HInstruction* instruction = block->GetFirstInstruction(); instruction != nullptr;) {
      HInstruction* next = instruction->GetNext();
      HInvoke* call = instruction->AsInvoke();
      // As long as the call is not intrinsified, it is worth trying to inline.
      if (call != nullptr && call->GetIntrinsic() == Intrinsics::kNone) {
        // We use the original invoke type to ensure the resolution of the called method
        // works properly.
        if (!TryInline(call)) {
          if (kIsDebugBuild && IsCompilingWithCoreImage()) {
            std::string callee_name =
                PrettyMethod(call->GetDexMethodIndex(), *outer_compilation_unit_.GetDexFile());
            bool should_inline = callee_name.find("$inline$") != std::string::npos;
            CHECK(!should_inline) << "Could not inline " << callee_name;
          }
        } else {
          if (kIsDebugBuild && IsCompilingWithCoreImage()) {
            std::string callee_name =
                PrettyMethod(call->GetDexMethodIndex(), *outer_compilation_unit_.GetDexFile());
            bool must_not_inline = callee_name.find("$noinline$") != std::string::npos;
            CHECK(!must_not_inline) << "Should not have inlined " << callee_name;
          }
        }
      }
      instruction = next;
    }
  }
}

static bool IsMethodOrDeclaringClassFinal(ArtMethod* method)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  return method->IsFinal() || method->GetDeclaringClass()->IsFinal();
}

/**
 * Given the `resolved_method` looked up in the dex cache, try to find
 * the actual runtime target of an interface or virtual call.
 * Return nullptr if the runtime target cannot be proven.
 */
static ArtMethod* FindVirtualOrInterfaceTarget(HInvoke* invoke, ArtMethod* resolved_method)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  if (IsMethodOrDeclaringClassFinal(resolved_method)) {
    // No need to lookup further, the resolved method will be the target.
    return resolved_method;
  }

  HInstruction* receiver = invoke->InputAt(0);
  if (receiver->IsNullCheck()) {
    // Due to multiple levels of inlining within the same pass, it might be that
    // null check does not have the reference type of the actual receiver.
    receiver = receiver->InputAt(0);
  }
  ReferenceTypeInfo info = receiver->GetReferenceTypeInfo();
  DCHECK(info.IsValid()) << "Invalid RTI for " << receiver->DebugName();
  if (!info.IsExact()) {
    // We currently only support inlining with known receivers.
    // TODO: Remove this check, we should be able to inline final methods
    // on unknown receivers.
    return nullptr;
  } else if (info.GetTypeHandle()->IsInterface()) {
    // Statically knowing that the receiver has an interface type cannot
    // help us find what is the target method.
    return nullptr;
  } else if (!resolved_method->GetDeclaringClass()->IsAssignableFrom(info.GetTypeHandle().Get())) {
    // The method that we're trying to call is not in the receiver's class or super classes.
    return nullptr;
  } else if (info.GetTypeHandle()->IsErroneous()) {
    // If the type is erroneous, do not go further, as we are going to query the vtable or
    // imt table, that we can only safely do on non-erroneous classes.
    return nullptr;
  }

  ClassLinker* cl = Runtime::Current()->GetClassLinker();
  size_t pointer_size = cl->GetImagePointerSize();
  if (invoke->IsInvokeInterface()) {
    resolved_method = info.GetTypeHandle()->FindVirtualMethodForInterface(
        resolved_method, pointer_size);
  } else {
    DCHECK(invoke->IsInvokeVirtual());
    resolved_method = info.GetTypeHandle()->FindVirtualMethodForVirtual(
        resolved_method, pointer_size);
  }

  if (resolved_method == nullptr) {
    // The information we had on the receiver was not enough to find
    // the target method. Since we check above the exact type of the receiver,
    // the only reason this can happen is an IncompatibleClassChangeError.
    return nullptr;
  } else if (!resolved_method->IsInvokable()) {
    // The information we had on the receiver was not enough to find
    // the target method. Since we check above the exact type of the receiver,
    // the only reason this can happen is an IncompatibleClassChangeError.
    return nullptr;
  } else if (IsMethodOrDeclaringClassFinal(resolved_method)) {
    // A final method has to be the target method.
    return resolved_method;
  } else if (info.IsExact()) {
    // If we found a method and the receiver's concrete type is statically
    // known, we know for sure the target.
    return resolved_method;
  } else {
    // Even if we did find a method, the receiver type was not enough to
    // statically find the runtime target.
    return nullptr;
  }
}

static uint32_t FindClassIndexIn(mirror::Class* cls,
                                 const DexFile& dex_file,
                                 Handle<mirror::DexCache> dex_cache)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  uint32_t index = DexFile::kDexNoIndex;
  if (cls->GetDexCache() == nullptr) {
    DCHECK(cls->IsArrayClass()) << PrettyClass(cls);
    index = cls->FindTypeIndexInOtherDexFile(dex_file);
  } else if (cls->GetDexTypeIndex() == DexFile::kDexNoIndex16) {
    DCHECK(cls->IsProxyClass()) << PrettyClass(cls);
    // TODO: deal with proxy classes.
  } else if (IsSameDexFile(cls->GetDexFile(), dex_file)) {
    DCHECK_EQ(cls->GetDexCache(), dex_cache.Get());
    index = cls->GetDexTypeIndex();
    // Update the dex cache to ensure the class is in. The generated code will
    // consider it is. We make it safe by updating the dex cache, as other
    // dex files might also load the class, and there is no guarantee the dex
    // cache of the dex file of the class will be updated.
    if (dex_cache->GetResolvedType(index) == nullptr) {
      dex_cache->SetResolvedType(index, cls);
    }
  } else {
    index = cls->FindTypeIndexInOtherDexFile(dex_file);
    // We cannot guarantee the entry in the dex cache will resolve to the same class,
    // as there may be different class loaders. So only return the index if it's
    // the right class in the dex cache already.
    if (index != DexFile::kDexNoIndex && dex_cache->GetResolvedType(index) != cls) {
      index = DexFile::kDexNoIndex;
    }
  }

  return index;
}

class ScopedProfilingInfoInlineUse {
 public:
  explicit ScopedProfilingInfoInlineUse(ArtMethod* method, Thread* self)
      : method_(method),
        self_(self),
        // Fetch the profiling info ahead of using it. If it's null when fetching,
        // we should not call JitCodeCache::DoneInlining.
        profiling_info_(
            Runtime::Current()->GetJit()->GetCodeCache()->NotifyCompilerUse(method, self)) {
  }

  ~ScopedProfilingInfoInlineUse() {
    if (profiling_info_ != nullptr) {
      size_t pointer_size = Runtime::Current()->GetClassLinker()->GetImagePointerSize();
      DCHECK_EQ(profiling_info_, method_->GetProfilingInfo(pointer_size));
      Runtime::Current()->GetJit()->GetCodeCache()->DoneCompilerUse(method_, self_);
    }
  }

  ProfilingInfo* GetProfilingInfo() const { return profiling_info_; }

 private:
  ArtMethod* const method_;
  Thread* const self_;
  ProfilingInfo* const profiling_info_;
};

bool HInliner::TryInline(HInvoke* invoke_instruction) {
  if (invoke_instruction->IsInvokeUnresolved()) {
    return false;  // Don't bother to move further if we know the method is unresolved.
  }

  uint32_t method_index = invoke_instruction->GetDexMethodIndex();
  ScopedObjectAccess soa(Thread::Current());
  const DexFile& caller_dex_file = *caller_compilation_unit_.GetDexFile();
  VLOG(compiler) << "Try inlining " << PrettyMethod(method_index, caller_dex_file);

  ClassLinker* class_linker = caller_compilation_unit_.GetClassLinker();
  // We can query the dex cache directly. The verifier has populated it already.
  ArtMethod* resolved_method;
  ArtMethod* actual_method = nullptr;
  if (invoke_instruction->IsInvokeStaticOrDirect()) {
    if (invoke_instruction->AsInvokeStaticOrDirect()->IsStringInit()) {
      VLOG(compiler) << "Not inlining a String.<init> method";
      return false;
    }
    MethodReference ref = invoke_instruction->AsInvokeStaticOrDirect()->GetTargetMethod();
    mirror::DexCache* const dex_cache = IsSameDexFile(caller_dex_file, *ref.dex_file)
        ? caller_compilation_unit_.GetDexCache().Get()
        : class_linker->FindDexCache(soa.Self(), *ref.dex_file);
    resolved_method = dex_cache->GetResolvedMethod(
        ref.dex_method_index, class_linker->GetImagePointerSize());
    // actual_method == resolved_method for direct or static calls.
    actual_method = resolved_method;
  } else {
    resolved_method = caller_compilation_unit_.GetDexCache().Get()->GetResolvedMethod(
        method_index, class_linker->GetImagePointerSize());
    if (resolved_method != nullptr) {
      // Check if we can statically find the method.
      actual_method = FindVirtualOrInterfaceTarget(invoke_instruction, resolved_method);
    }
  }

  if (resolved_method == nullptr) {
    // TODO: Can this still happen?
    // Method cannot be resolved if it is in another dex file we do not have access to.
    VLOG(compiler) << "Method cannot be resolved " << PrettyMethod(method_index, caller_dex_file);
    return false;
  }

  if (actual_method != nullptr) {
    bool result = TryInlineAndReplace(invoke_instruction, actual_method, /* do_rtp */ true);
    if (result && !invoke_instruction->IsInvokeStaticOrDirect()) {
      MaybeRecordStat(kInlinedInvokeVirtualOrInterface);
    }
    return result;
  }

  DCHECK(!invoke_instruction->IsInvokeStaticOrDirect());

  // Check if we can use an inline cache.
  ArtMethod* caller = graph_->GetArtMethod();
  if (Runtime::Current()->UseJitCompilation()) {
    // Under JIT, we should always know the caller.
    DCHECK(caller != nullptr);
    ScopedProfilingInfoInlineUse spiis(caller, soa.Self());
    ProfilingInfo* profiling_info = spiis.GetProfilingInfo();
    if (profiling_info != nullptr) {
      const InlineCache& ic = *profiling_info->GetInlineCache(invoke_instruction->GetDexPc());
      if (ic.IsUninitialized()) {
        VLOG(compiler) << "Interface or virtual call to "
                       << PrettyMethod(method_index, caller_dex_file)
                       << " is not hit and not inlined";
        return false;
      } else if (ic.IsMonomorphic()) {
        MaybeRecordStat(kMonomorphicCall);
        if (outermost_graph_->IsCompilingOsr()) {
          // If we are compiling OSR, we pretend this call is polymorphic, as we may come from the
          // interpreter and it may have seen different receiver types.
          return TryInlinePolymorphicCall(invoke_instruction, resolved_method, ic);
        } else {
          return TryInlineMonomorphicCall(invoke_instruction, resolved_method, ic);
        }
      } else if (ic.IsPolymorphic()) {
        MaybeRecordStat(kPolymorphicCall);
        return TryInlinePolymorphicCall(invoke_instruction, resolved_method, ic);
      } else {
        DCHECK(ic.IsMegamorphic());
        VLOG(compiler) << "Interface or virtual call to "
                       << PrettyMethod(method_index, caller_dex_file)
                       << " is megamorphic and not inlined";
        MaybeRecordStat(kMegamorphicCall);
        return false;
      }
    }
  }

  VLOG(compiler) << "Interface or virtual call to "
                 << PrettyMethod(method_index, caller_dex_file)
                 << " could not be statically determined";
  return false;
}

HInstanceFieldGet* HInliner::BuildGetReceiverClass(ClassLinker* class_linker,
                                                   HInstruction* receiver,
                                                   uint32_t dex_pc) const {
  ArtField* field = class_linker->GetClassRoot(ClassLinker::kJavaLangObject)->GetInstanceField(0);
  DCHECK_EQ(std::string(field->GetName()), "shadow$_klass_");
  HInstanceFieldGet* result = new (graph_->GetArena()) HInstanceFieldGet(
      receiver,
      Primitive::kPrimNot,
      field->GetOffset(),
      field->IsVolatile(),
      field->GetDexFieldIndex(),
      field->GetDeclaringClass()->GetDexClassDefIndex(),
      *field->GetDexFile(),
      handles_->NewHandle(field->GetDexCache()),
      dex_pc);
  // The class of a field is effectively final, and does not have any memory dependencies.
  result->SetSideEffects(SideEffects::None());
  return result;
}

bool HInliner::TryInlineMonomorphicCall(HInvoke* invoke_instruction,
                                        ArtMethod* resolved_method,
                                        const InlineCache& ic) {
  DCHECK(invoke_instruction->IsInvokeVirtual() || invoke_instruction->IsInvokeInterface())
      << invoke_instruction->DebugName();

  const DexFile& caller_dex_file = *caller_compilation_unit_.GetDexFile();
  uint32_t class_index = FindClassIndexIn(
      ic.GetMonomorphicType(), caller_dex_file, caller_compilation_unit_.GetDexCache());
  if (class_index == DexFile::kDexNoIndex) {
    VLOG(compiler) << "Call to " << PrettyMethod(resolved_method)
                   << " from inline cache is not inlined because its class is not"
                   << " accessible to the caller";
    return false;
  }

  ClassLinker* class_linker = caller_compilation_unit_.GetClassLinker();
  size_t pointer_size = class_linker->GetImagePointerSize();
  if (invoke_instruction->IsInvokeInterface()) {
    resolved_method = ic.GetMonomorphicType()->FindVirtualMethodForInterface(
        resolved_method, pointer_size);
  } else {
    DCHECK(invoke_instruction->IsInvokeVirtual());
    resolved_method = ic.GetMonomorphicType()->FindVirtualMethodForVirtual(
        resolved_method, pointer_size);
  }
  DCHECK(resolved_method != nullptr);
  HInstruction* receiver = invoke_instruction->InputAt(0);
  HInstruction* cursor = invoke_instruction->GetPrevious();
  HBasicBlock* bb_cursor = invoke_instruction->GetBlock();

  if (!TryInlineAndReplace(invoke_instruction, resolved_method, /* do_rtp */ false)) {
    return false;
  }

  // We successfully inlined, now add a guard.
  bool is_referrer =
      (ic.GetMonomorphicType() == outermost_graph_->GetArtMethod()->GetDeclaringClass());
  AddTypeGuard(receiver,
               cursor,
               bb_cursor,
               class_index,
               is_referrer,
               invoke_instruction,
               /* with_deoptimization */ true);

  // Run type propagation to get the guard typed, and eventually propagate the
  // type of the receiver.
  ReferenceTypePropagation rtp_fixup(graph_,
                                     outer_compilation_unit_.GetDexCache(),
                                     handles_,
                                     /* is_first_run */ false);
  rtp_fixup.Run();

  MaybeRecordStat(kInlinedMonomorphicCall);
  return true;
}

HInstruction* HInliner::AddTypeGuard(HInstruction* receiver,
                                     HInstruction* cursor,
                                     HBasicBlock* bb_cursor,
                                     uint32_t class_index,
                                     bool is_referrer,
                                     HInstruction* invoke_instruction,
                                     bool with_deoptimization) {
  ClassLinker* class_linker = caller_compilation_unit_.GetClassLinker();
  HInstanceFieldGet* receiver_class = BuildGetReceiverClass(
      class_linker, receiver, invoke_instruction->GetDexPc());

  const DexFile& caller_dex_file = *caller_compilation_unit_.GetDexFile();
  // Note that we will just compare the classes, so we don't need Java semantics access checks.
  // Also, the caller of `AddTypeGuard` must have guaranteed that the class is in the dex cache.
  HLoadClass* load_class = new (graph_->GetArena()) HLoadClass(graph_->GetCurrentMethod(),
                                                               class_index,
                                                               caller_dex_file,
                                                               is_referrer,
                                                               invoke_instruction->GetDexPc(),
                                                               /* needs_access_check */ false,
                                                               /* is_in_dex_cache */ true);

  HNotEqual* compare = new (graph_->GetArena()) HNotEqual(load_class, receiver_class);
  // TODO: Extend reference type propagation to understand the guard.
  if (cursor != nullptr) {
    bb_cursor->InsertInstructionAfter(receiver_class, cursor);
  } else {
    bb_cursor->InsertInstructionBefore(receiver_class, bb_cursor->GetFirstInstruction());
  }
  bb_cursor->InsertInstructionAfter(load_class, receiver_class);
  bb_cursor->InsertInstructionAfter(compare, load_class);
  if (with_deoptimization) {
    HDeoptimize* deoptimize = new (graph_->GetArena()) HDeoptimize(
        compare, invoke_instruction->GetDexPc());
    bb_cursor->InsertInstructionAfter(deoptimize, compare);
    deoptimize->CopyEnvironmentFrom(invoke_instruction->GetEnvironment());
  }
  return compare;
}

bool HInliner::TryInlinePolymorphicCall(HInvoke* invoke_instruction,
                                        ArtMethod* resolved_method,
                                        const InlineCache& ic) {
  DCHECK(invoke_instruction->IsInvokeVirtual() || invoke_instruction->IsInvokeInterface())
      << invoke_instruction->DebugName();

  if (TryInlinePolymorphicCallToSameTarget(invoke_instruction, resolved_method, ic)) {
    return true;
  }

  ClassLinker* class_linker = caller_compilation_unit_.GetClassLinker();
  size_t pointer_size = class_linker->GetImagePointerSize();
  const DexFile& caller_dex_file = *caller_compilation_unit_.GetDexFile();

  bool all_targets_inlined = true;
  bool one_target_inlined = false;
  for (size_t i = 0; i < InlineCache::kIndividualCacheSize; ++i) {
    if (ic.GetTypeAt(i) == nullptr) {
      break;
    }
    ArtMethod* method = nullptr;
    if (invoke_instruction->IsInvokeInterface()) {
      method = ic.GetTypeAt(i)->FindVirtualMethodForInterface(
          resolved_method, pointer_size);
    } else {
      DCHECK(invoke_instruction->IsInvokeVirtual());
      method = ic.GetTypeAt(i)->FindVirtualMethodForVirtual(
          resolved_method, pointer_size);
    }

    HInstruction* receiver = invoke_instruction->InputAt(0);
    HInstruction* cursor = invoke_instruction->GetPrevious();
    HBasicBlock* bb_cursor = invoke_instruction->GetBlock();

    uint32_t class_index = FindClassIndexIn(
        ic.GetTypeAt(i), caller_dex_file, caller_compilation_unit_.GetDexCache());
    HInstruction* return_replacement = nullptr;
    if (class_index == DexFile::kDexNoIndex ||
        !TryBuildAndInline(invoke_instruction, method, &return_replacement)) {
      all_targets_inlined = false;
    } else {
      one_target_inlined = true;
      bool is_referrer = (ic.GetTypeAt(i) == outermost_graph_->GetArtMethod()->GetDeclaringClass());

      // If we have inlined all targets before, and this receiver is the last seen,
      // we deoptimize instead of keeping the original invoke instruction.
      bool deoptimize = all_targets_inlined &&
          (i != InlineCache::kIndividualCacheSize - 1) &&
          (ic.GetTypeAt(i + 1) == nullptr);

      if (outermost_graph_->IsCompilingOsr()) {
        // We do not support HDeoptimize in OSR methods.
        deoptimize = false;
      }
      HInstruction* compare = AddTypeGuard(
          receiver, cursor, bb_cursor, class_index, is_referrer, invoke_instruction, deoptimize);
      if (deoptimize) {
        if (return_replacement != nullptr) {
          invoke_instruction->ReplaceWith(return_replacement);
        }
        invoke_instruction->GetBlock()->RemoveInstruction(invoke_instruction);
        // Because the inline cache data can be populated concurrently, we force the end of the
        // iteration. Otherhwise, we could see a new receiver type.
        break;
      } else {
        CreateDiamondPatternForPolymorphicInline(compare, return_replacement, invoke_instruction);
      }
    }
  }

  if (!one_target_inlined) {
    VLOG(compiler) << "Call to " << PrettyMethod(resolved_method)
                   << " from inline cache is not inlined because none"
                   << " of its targets could be inlined";
    return false;
  }
  MaybeRecordStat(kInlinedPolymorphicCall);

  // Run type propagation to get the guards typed.
  ReferenceTypePropagation rtp_fixup(graph_,
                                     outer_compilation_unit_.GetDexCache(),
                                     handles_,
                                     /* is_first_run */ false);
  rtp_fixup.Run();
  return true;
}

void HInliner::CreateDiamondPatternForPolymorphicInline(HInstruction* compare,
                                                        HInstruction* return_replacement,
                                                        HInstruction* invoke_instruction) {
  uint32_t dex_pc = invoke_instruction->GetDexPc();
  HBasicBlock* cursor_block = compare->GetBlock();
  HBasicBlock* original_invoke_block = invoke_instruction->GetBlock();
  ArenaAllocator* allocator = graph_->GetArena();

  // Spit the block after the compare: `cursor_block` will now be the start of the diamond,
  // and the returned block is the start of the then branch (that could contain multiple blocks).
  HBasicBlock* then = cursor_block->SplitAfterForInlining(compare);

  // Split the block containing the invoke before and after the invoke. The returned block
  // of the split before will contain the invoke and will be the otherwise branch of
  // the diamond. The returned block of the split after will be the merge block
  // of the diamond.
  HBasicBlock* end_then = invoke_instruction->GetBlock();
  HBasicBlock* otherwise = end_then->SplitBeforeForInlining(invoke_instruction);
  HBasicBlock* merge = otherwise->SplitAfterForInlining(invoke_instruction);

  // If the methods we are inlining return a value, we create a phi in the merge block
  // that will have the `invoke_instruction and the `return_replacement` as inputs.
  if (return_replacement != nullptr) {
    HPhi* phi = new (allocator) HPhi(
        allocator, kNoRegNumber, 0, HPhi::ToPhiType(invoke_instruction->GetType()), dex_pc);
    merge->AddPhi(phi);
    invoke_instruction->ReplaceWith(phi);
    phi->AddInput(return_replacement);
    phi->AddInput(invoke_instruction);
  }

  // Add the control flow instructions.
  otherwise->AddInstruction(new (allocator) HGoto(dex_pc));
  end_then->AddInstruction(new (allocator) HGoto(dex_pc));
  cursor_block->AddInstruction(new (allocator) HIf(compare, dex_pc));

  // Add the newly created blocks to the graph.
  graph_->AddBlock(then);
  graph_->AddBlock(otherwise);
  graph_->AddBlock(merge);

  // Set up successor (and implictly predecessor) relations.
  cursor_block->AddSuccessor(otherwise);
  cursor_block->AddSuccessor(then);
  end_then->AddSuccessor(merge);
  otherwise->AddSuccessor(merge);

  // Set up dominance information.
  then->SetDominator(cursor_block);
  cursor_block->AddDominatedBlock(then);
  otherwise->SetDominator(cursor_block);
  cursor_block->AddDominatedBlock(otherwise);
  merge->SetDominator(cursor_block);
  cursor_block->AddDominatedBlock(merge);

  // Update the revert post order.
  size_t index = IndexOfElement(graph_->reverse_post_order_, cursor_block);
  MakeRoomFor(&graph_->reverse_post_order_, 1, index);
  graph_->reverse_post_order_[++index] = then;
  index = IndexOfElement(graph_->reverse_post_order_, end_then);
  MakeRoomFor(&graph_->reverse_post_order_, 2, index);
  graph_->reverse_post_order_[++index] = otherwise;
  graph_->reverse_post_order_[++index] = merge;


  graph_->UpdateLoopAndTryInformationOfNewBlock(
      then, original_invoke_block, /* replace_if_back_edge */ false);
  graph_->UpdateLoopAndTryInformationOfNewBlock(
      otherwise, original_invoke_block, /* replace_if_back_edge */ false);

  // In case the original invoke location was a back edge, we need to update
  // the loop to now have the merge block as a back edge.
  graph_->UpdateLoopAndTryInformationOfNewBlock(
      merge, original_invoke_block, /* replace_if_back_edge */ true);
}

bool HInliner::TryInlinePolymorphicCallToSameTarget(HInvoke* invoke_instruction,
                                                    ArtMethod* resolved_method,
                                                    const InlineCache& ic) {
  // This optimization only works under JIT for now.
  DCHECK(Runtime::Current()->UseJitCompilation());
  if (graph_->GetInstructionSet() == kMips64) {
    // TODO: Support HClassTableGet for mips64.
    return false;
  }
  ClassLinker* class_linker = caller_compilation_unit_.GetClassLinker();
  size_t pointer_size = class_linker->GetImagePointerSize();

  DCHECK(resolved_method != nullptr);
  ArtMethod* actual_method = nullptr;
  size_t method_index = invoke_instruction->IsInvokeVirtual()
      ? invoke_instruction->AsInvokeVirtual()->GetVTableIndex()
      : invoke_instruction->AsInvokeInterface()->GetImtIndex();

  // Check whether we are actually calling the same method among
  // the different types seen.
  for (size_t i = 0; i < InlineCache::kIndividualCacheSize; ++i) {
    if (ic.GetTypeAt(i) == nullptr) {
      break;
    }
    ArtMethod* new_method = nullptr;
    if (invoke_instruction->IsInvokeInterface()) {
      new_method = ic.GetTypeAt(i)->GetEmbeddedImTableEntry(
          method_index % mirror::Class::kImtSize, pointer_size);
      if (new_method->IsRuntimeMethod()) {
        // Bail out as soon as we see a conflict trampoline in one of the target's
        // interface table.
        return false;
      }
    } else {
      DCHECK(invoke_instruction->IsInvokeVirtual());
      new_method = ic.GetTypeAt(i)->GetEmbeddedVTableEntry(method_index, pointer_size);
    }
    DCHECK(new_method != nullptr);
    if (actual_method == nullptr) {
      actual_method = new_method;
    } else if (actual_method != new_method) {
      // Different methods, bailout.
      VLOG(compiler) << "Call to " << PrettyMethod(resolved_method)
                     << " from inline cache is not inlined because it resolves"
                     << " to different methods";
      return false;
    }
  }

  HInstruction* receiver = invoke_instruction->InputAt(0);
  HInstruction* cursor = invoke_instruction->GetPrevious();
  HBasicBlock* bb_cursor = invoke_instruction->GetBlock();

  HInstruction* return_replacement = nullptr;
  if (!TryBuildAndInline(invoke_instruction, actual_method, &return_replacement)) {
    return false;
  }

  // We successfully inlined, now add a guard.
  HInstanceFieldGet* receiver_class = BuildGetReceiverClass(
      class_linker, receiver, invoke_instruction->GetDexPc());

  Primitive::Type type = Is64BitInstructionSet(graph_->GetInstructionSet())
      ? Primitive::kPrimLong
      : Primitive::kPrimInt;
  HClassTableGet* class_table_get = new (graph_->GetArena()) HClassTableGet(
      receiver_class,
      type,
      invoke_instruction->IsInvokeVirtual() ? HClassTableGet::TableKind::kVTable
                                            : HClassTableGet::TableKind::kIMTable,
      method_index,
      invoke_instruction->GetDexPc());

  HConstant* constant;
  if (type == Primitive::kPrimLong) {
    constant = graph_->GetLongConstant(
        reinterpret_cast<intptr_t>(actual_method), invoke_instruction->GetDexPc());
  } else {
    constant = graph_->GetIntConstant(
        reinterpret_cast<intptr_t>(actual_method), invoke_instruction->GetDexPc());
  }

  HNotEqual* compare = new (graph_->GetArena()) HNotEqual(class_table_get, constant);
  if (cursor != nullptr) {
    bb_cursor->InsertInstructionAfter(receiver_class, cursor);
  } else {
    bb_cursor->InsertInstructionBefore(receiver_class, bb_cursor->GetFirstInstruction());
  }
  bb_cursor->InsertInstructionAfter(class_table_get, receiver_class);
  bb_cursor->InsertInstructionAfter(compare, class_table_get);

  if (outermost_graph_->IsCompilingOsr()) {
    CreateDiamondPatternForPolymorphicInline(compare, return_replacement, invoke_instruction);
  } else {
    // TODO: Extend reference type propagation to understand the guard.
    HDeoptimize* deoptimize = new (graph_->GetArena()) HDeoptimize(
        compare, invoke_instruction->GetDexPc());
    bb_cursor->InsertInstructionAfter(deoptimize, compare);
    deoptimize->CopyEnvironmentFrom(invoke_instruction->GetEnvironment());
    if (return_replacement != nullptr) {
      invoke_instruction->ReplaceWith(return_replacement);
    }
    invoke_instruction->GetBlock()->RemoveInstruction(invoke_instruction);
  }

  // Run type propagation to get the guard typed.
  ReferenceTypePropagation rtp_fixup(graph_,
                                     outer_compilation_unit_.GetDexCache(),
                                     handles_,
                                     /* is_first_run */ false);
  rtp_fixup.Run();

  MaybeRecordStat(kInlinedPolymorphicCall);

  return true;
}

bool HInliner::TryInlineAndReplace(HInvoke* invoke_instruction, ArtMethod* method, bool do_rtp) {
  HInstruction* return_replacement = nullptr;
  if (!TryBuildAndInline(invoke_instruction, method, &return_replacement)) {
    return false;
  }
  if (return_replacement != nullptr) {
    invoke_instruction->ReplaceWith(return_replacement);
  }
  invoke_instruction->GetBlock()->RemoveInstruction(invoke_instruction);
  FixUpReturnReferenceType(invoke_instruction, method, return_replacement, do_rtp);
  return true;
}

bool HInliner::TryBuildAndInline(HInvoke* invoke_instruction,
                                 ArtMethod* method,
                                 HInstruction** return_replacement) {
  if (method->IsProxyMethod()) {
    VLOG(compiler) << "Method " << PrettyMethod(method)
                   << " is not inlined because of unimplemented inline support for proxy methods.";
    return false;
  }

  // Check whether we're allowed to inline. The outermost compilation unit is the relevant
  // dex file here (though the transitivity of an inline chain would allow checking the calller).
  if (!compiler_driver_->MayInline(method->GetDexFile(),
                                   outer_compilation_unit_.GetDexFile())) {
    if (TryPatternSubstitution(invoke_instruction, method, return_replacement)) {
      VLOG(compiler) << "Successfully replaced pattern of invoke " << PrettyMethod(method);
      MaybeRecordStat(kReplacedInvokeWithSimplePattern);
      return true;
    }
    VLOG(compiler) << "Won't inline " << PrettyMethod(method) << " in "
                   << outer_compilation_unit_.GetDexFile()->GetLocation() << " ("
                   << caller_compilation_unit_.GetDexFile()->GetLocation() << ") from "
                   << method->GetDexFile()->GetLocation();
    return false;
  }

  bool same_dex_file = IsSameDexFile(*outer_compilation_unit_.GetDexFile(), *method->GetDexFile());

  const DexFile::CodeItem* code_item = method->GetCodeItem();

  if (code_item == nullptr) {
    VLOG(compiler) << "Method " << PrettyMethod(method)
                   << " is not inlined because it is native";
    return false;
  }

  size_t inline_max_code_units = compiler_driver_->GetCompilerOptions().GetInlineMaxCodeUnits();
  if (code_item->insns_size_in_code_units_ > inline_max_code_units) {
    VLOG(compiler) << "Method " << PrettyMethod(method)
                   << " is too big to inline: "
                   << code_item->insns_size_in_code_units_
                   << " > "
                   << inline_max_code_units;
    return false;
  }

  if (code_item->tries_size_ != 0) {
    VLOG(compiler) << "Method " << PrettyMethod(method)
                   << " is not inlined because of try block";
    return false;
  }

  if (!method->IsCompilable()) {
    VLOG(compiler) << "Method " << PrettyMethod(method)
                   << " has soft failures un-handled by the compiler, so it cannot be inlined";
  }

  if (!method->GetDeclaringClass()->IsVerified()) {
    uint16_t class_def_idx = method->GetDeclaringClass()->GetDexClassDefIndex();
    if (Runtime::Current()->UseJitCompilation() ||
        !compiler_driver_->IsMethodVerifiedWithoutFailures(
            method->GetDexMethodIndex(), class_def_idx, *method->GetDexFile())) {
      VLOG(compiler) << "Method " << PrettyMethod(method)
                     << " couldn't be verified, so it cannot be inlined";
      return false;
    }
  }

  if (invoke_instruction->IsInvokeStaticOrDirect() &&
      invoke_instruction->AsInvokeStaticOrDirect()->IsStaticWithImplicitClinitCheck()) {
    // Case of a static method that cannot be inlined because it implicitly
    // requires an initialization check of its declaring class.
    VLOG(compiler) << "Method " << PrettyMethod(method)
                   << " is not inlined because it is static and requires a clinit"
                   << " check that cannot be emitted due to Dex cache limitations";
    return false;
  }

  if (!TryBuildAndInlineHelper(invoke_instruction, method, same_dex_file, return_replacement)) {
    return false;
  }

  VLOG(compiler) << "Successfully inlined " << PrettyMethod(method);
  MaybeRecordStat(kInlinedInvoke);
  return true;
}

static HInstruction* GetInvokeInputForArgVRegIndex(HInvoke* invoke_instruction,
                                                   size_t arg_vreg_index)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  size_t input_index = 0;
  for (size_t i = 0; i < arg_vreg_index; ++i, ++input_index) {
    DCHECK_LT(input_index, invoke_instruction->GetNumberOfArguments());
    if (Primitive::Is64BitType(invoke_instruction->InputAt(input_index)->GetType())) {
      ++i;
      DCHECK_NE(i, arg_vreg_index);
    }
  }
  DCHECK_LT(input_index, invoke_instruction->GetNumberOfArguments());
  return invoke_instruction->InputAt(input_index);
}

// Try to recognize known simple patterns and replace invoke call with appropriate instructions.
bool HInliner::TryPatternSubstitution(HInvoke* invoke_instruction,
                                      ArtMethod* resolved_method,
                                      HInstruction** return_replacement) {
  InlineMethod inline_method;
  if (!InlineMethodAnalyser::AnalyseMethodCode(resolved_method, &inline_method)) {
    return false;
  }

  switch (inline_method.opcode) {
    case kInlineOpNop:
      DCHECK_EQ(invoke_instruction->GetType(), Primitive::kPrimVoid);
      *return_replacement = nullptr;
      break;
    case kInlineOpReturnArg:
      *return_replacement = GetInvokeInputForArgVRegIndex(invoke_instruction,
                                                          inline_method.d.return_data.arg);
      break;
    case kInlineOpNonWideConst:
      if (resolved_method->GetShorty()[0] == 'L') {
        DCHECK_EQ(inline_method.d.data, 0u);
        *return_replacement = graph_->GetNullConstant();
      } else {
        *return_replacement = graph_->GetIntConstant(static_cast<int32_t>(inline_method.d.data));
      }
      break;
    case kInlineOpIGet: {
      const InlineIGetIPutData& data = inline_method.d.ifield_data;
      if (data.method_is_static || data.object_arg != 0u) {
        // TODO: Needs null check.
        return false;
      }
      Handle<mirror::DexCache> dex_cache(handles_->NewHandle(resolved_method->GetDexCache()));
      HInstruction* obj = GetInvokeInputForArgVRegIndex(invoke_instruction, data.object_arg);
      HInstanceFieldGet* iget = CreateInstanceFieldGet(dex_cache, data.field_idx, obj);
      DCHECK_EQ(iget->GetFieldOffset().Uint32Value(), data.field_offset);
      DCHECK_EQ(iget->IsVolatile() ? 1u : 0u, data.is_volatile);
      invoke_instruction->GetBlock()->InsertInstructionBefore(iget, invoke_instruction);
      *return_replacement = iget;
      break;
    }
    case kInlineOpIPut: {
      const InlineIGetIPutData& data = inline_method.d.ifield_data;
      if (data.method_is_static || data.object_arg != 0u) {
        // TODO: Needs null check.
        return false;
      }
      Handle<mirror::DexCache> dex_cache(handles_->NewHandle(resolved_method->GetDexCache()));
      HInstruction* obj = GetInvokeInputForArgVRegIndex(invoke_instruction, data.object_arg);
      HInstruction* value = GetInvokeInputForArgVRegIndex(invoke_instruction, data.src_arg);
      HInstanceFieldSet* iput = CreateInstanceFieldSet(dex_cache, data.field_idx, obj, value);
      DCHECK_EQ(iput->GetFieldOffset().Uint32Value(), data.field_offset);
      DCHECK_EQ(iput->IsVolatile() ? 1u : 0u, data.is_volatile);
      invoke_instruction->GetBlock()->InsertInstructionBefore(iput, invoke_instruction);
      if (data.return_arg_plus1 != 0u) {
        size_t return_arg = data.return_arg_plus1 - 1u;
        *return_replacement = GetInvokeInputForArgVRegIndex(invoke_instruction, return_arg);
      }
      break;
    }
    case kInlineOpConstructor: {
      const InlineConstructorData& data = inline_method.d.constructor_data;
      // Get the indexes to arrays for easier processing.
      uint16_t iput_field_indexes[] = {
          data.iput0_field_index, data.iput1_field_index, data.iput2_field_index
      };
      uint16_t iput_args[] = { data.iput0_arg, data.iput1_arg, data.iput2_arg };
      static_assert(arraysize(iput_args) == arraysize(iput_field_indexes), "Size mismatch");
      // Count valid field indexes.
      size_t number_of_iputs = 0u;
      while (number_of_iputs != arraysize(iput_field_indexes) &&
          iput_field_indexes[number_of_iputs] != DexFile::kDexNoIndex16) {
        // Check that there are no duplicate valid field indexes.
        DCHECK_EQ(0, std::count(iput_field_indexes + number_of_iputs + 1,
                                iput_field_indexes + arraysize(iput_field_indexes),
                                iput_field_indexes[number_of_iputs]));
        ++number_of_iputs;
      }
      // Check that there are no valid field indexes in the rest of the array.
      DCHECK_EQ(0, std::count_if(iput_field_indexes + number_of_iputs,
                                 iput_field_indexes + arraysize(iput_field_indexes),
                                 [](uint16_t index) { return index != DexFile::kDexNoIndex16; }));

      // Create HInstanceFieldSet for each IPUT that stores non-zero data.
      Handle<mirror::DexCache> dex_cache;
      HInstruction* obj = GetInvokeInputForArgVRegIndex(invoke_instruction, /* this */ 0u);
      bool needs_constructor_barrier = false;
      for (size_t i = 0; i != number_of_iputs; ++i) {
        HInstruction* value = GetInvokeInputForArgVRegIndex(invoke_instruction, iput_args[i]);
        if (!value->IsConstant() || !value->AsConstant()->IsZeroBitPattern()) {
          if (dex_cache.GetReference() == nullptr) {
            dex_cache = handles_->NewHandle(resolved_method->GetDexCache());
          }
          uint16_t field_index = iput_field_indexes[i];
          HInstanceFieldSet* iput = CreateInstanceFieldSet(dex_cache, field_index, obj, value);
          invoke_instruction->GetBlock()->InsertInstructionBefore(iput, invoke_instruction);

          // Check whether the field is final. If it is, we need to add a barrier.
          size_t pointer_size = InstructionSetPointerSize(codegen_->GetInstructionSet());
          ArtField* resolved_field = dex_cache->GetResolvedField(field_index, pointer_size);
          DCHECK(resolved_field != nullptr);
          if (resolved_field->IsFinal()) {
            needs_constructor_barrier = true;
          }
        }
      }
      if (needs_constructor_barrier) {
        HMemoryBarrier* barrier = new (graph_->GetArena()) HMemoryBarrier(kStoreStore, kNoDexPc);
        invoke_instruction->GetBlock()->InsertInstructionBefore(barrier, invoke_instruction);
      }
      *return_replacement = nullptr;
      break;
    }
    default:
      LOG(FATAL) << "UNREACHABLE";
      UNREACHABLE();
  }
  return true;
}

HInstanceFieldGet* HInliner::CreateInstanceFieldGet(Handle<mirror::DexCache> dex_cache,
                                                    uint32_t field_index,
                                                    HInstruction* obj)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  size_t pointer_size = InstructionSetPointerSize(codegen_->GetInstructionSet());
  ArtField* resolved_field = dex_cache->GetResolvedField(field_index, pointer_size);
  DCHECK(resolved_field != nullptr);
  HInstanceFieldGet* iget = new (graph_->GetArena()) HInstanceFieldGet(
      obj,
      resolved_field->GetTypeAsPrimitiveType(),
      resolved_field->GetOffset(),
      resolved_field->IsVolatile(),
      field_index,
      resolved_field->GetDeclaringClass()->GetDexClassDefIndex(),
      *dex_cache->GetDexFile(),
      dex_cache,
      // Read barrier generates a runtime call in slow path and we need a valid
      // dex pc for the associated stack map. 0 is bogus but valid. Bug: 26854537.
      /* dex_pc */ 0);
  if (iget->GetType() == Primitive::kPrimNot) {
    // Use the same dex_cache that we used for field lookup as the hint_dex_cache.
    ReferenceTypePropagation rtp(graph_, dex_cache, handles_, /* is_first_run */ false);
    rtp.Visit(iget);
  }
  return iget;
}

HInstanceFieldSet* HInliner::CreateInstanceFieldSet(Handle<mirror::DexCache> dex_cache,
                                                    uint32_t field_index,
                                                    HInstruction* obj,
                                                    HInstruction* value)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  size_t pointer_size = InstructionSetPointerSize(codegen_->GetInstructionSet());
  ArtField* resolved_field = dex_cache->GetResolvedField(field_index, pointer_size);
  DCHECK(resolved_field != nullptr);
  HInstanceFieldSet* iput = new (graph_->GetArena()) HInstanceFieldSet(
      obj,
      value,
      resolved_field->GetTypeAsPrimitiveType(),
      resolved_field->GetOffset(),
      resolved_field->IsVolatile(),
      field_index,
      resolved_field->GetDeclaringClass()->GetDexClassDefIndex(),
      *dex_cache->GetDexFile(),
      dex_cache,
      // Read barrier generates a runtime call in slow path and we need a valid
      // dex pc for the associated stack map. 0 is bogus but valid. Bug: 26854537.
      /* dex_pc */ 0);
  return iput;
}

bool HInliner::TryBuildAndInlineHelper(HInvoke* invoke_instruction,
                                       ArtMethod* resolved_method,
                                       bool same_dex_file,
                                       HInstruction** return_replacement) {
  ScopedObjectAccess soa(Thread::Current());
  const DexFile::CodeItem* code_item = resolved_method->GetCodeItem();
  const DexFile& callee_dex_file = *resolved_method->GetDexFile();
  uint32_t method_index = resolved_method->GetDexMethodIndex();
  ClassLinker* class_linker = caller_compilation_unit_.GetClassLinker();
  Handle<mirror::DexCache> dex_cache(handles_->NewHandle(resolved_method->GetDexCache()));
  Handle<mirror::ClassLoader> class_loader(handles_->NewHandle(
      resolved_method->GetDeclaringClass()->GetClassLoader()));

  DexCompilationUnit dex_compilation_unit(
      class_loader.ToJObject(),
      class_linker,
      callee_dex_file,
      code_item,
      resolved_method->GetDeclaringClass()->GetDexClassDefIndex(),
      method_index,
      resolved_method->GetAccessFlags(),
      /* verified_method */ nullptr,
      dex_cache);

  bool requires_ctor_barrier = false;

  if (dex_compilation_unit.IsConstructor()) {
    // If it's a super invocation and we already generate a barrier there's no need
    // to generate another one.
    // We identify super calls by looking at the "this" pointer. If its value is the
    // same as the local "this" pointer then we must have a super invocation.
    bool is_super_invocation = invoke_instruction->InputAt(0)->IsParameterValue()
        && invoke_instruction->InputAt(0)->AsParameterValue()->IsThis();
    if (is_super_invocation && graph_->ShouldGenerateConstructorBarrier()) {
      requires_ctor_barrier = false;
    } else {
      Thread* self = Thread::Current();
      requires_ctor_barrier = compiler_driver_->RequiresConstructorBarrier(self,
          dex_compilation_unit.GetDexFile(),
          dex_compilation_unit.GetClassDefIndex());
    }
  }

  InvokeType invoke_type = invoke_instruction->GetOriginalInvokeType();
  if (invoke_type == kInterface) {
    // We have statically resolved the dispatch. To please the class linker
    // at runtime, we change this call as if it was a virtual call.
    invoke_type = kVirtual;
  }

  const int32_t caller_instruction_counter = graph_->GetCurrentInstructionId();
  HGraph* callee_graph = new (graph_->GetArena()) HGraph(
      graph_->GetArena(),
      callee_dex_file,
      method_index,
      requires_ctor_barrier,
      compiler_driver_->GetInstructionSet(),
      invoke_type,
      graph_->IsDebuggable(),
      /* osr */ false,
      caller_instruction_counter);
  callee_graph->SetArtMethod(resolved_method);

  // When they are needed, allocate `inline_stats` on the heap instead
  // of on the stack, as Clang might produce a stack frame too large
  // for this function, that would not fit the requirements of the
  // `-Wframe-larger-than` option.
  std::unique_ptr<OptimizingCompilerStats> inline_stats =
      (stats_ == nullptr) ? nullptr : MakeUnique<OptimizingCompilerStats>();
  HGraphBuilder builder(callee_graph,
                        &dex_compilation_unit,
                        &outer_compilation_unit_,
                        resolved_method->GetDexFile(),
                        *code_item,
                        compiler_driver_,
                        inline_stats.get(),
                        resolved_method->GetQuickenedInfo(),
                        dex_cache,
                        handles_);

  if (builder.BuildGraph() != kAnalysisSuccess) {
    VLOG(compiler) << "Method " << PrettyMethod(method_index, callee_dex_file)
                   << " could not be built, so cannot be inlined";
    return false;
  }

  if (!RegisterAllocator::CanAllocateRegistersFor(*callee_graph,
                                                  compiler_driver_->GetInstructionSet())) {
    VLOG(compiler) << "Method " << PrettyMethod(method_index, callee_dex_file)
                   << " cannot be inlined because of the register allocator";
    return false;
  }

  size_t parameter_index = 0;
  for (HInstructionIterator instructions(callee_graph->GetEntryBlock()->GetInstructions());
       !instructions.Done();
       instructions.Advance()) {
    HInstruction* current = instructions.Current();
    if (current->IsParameterValue()) {
      HInstruction* argument = invoke_instruction->InputAt(parameter_index++);
      if (argument->IsNullConstant()) {
        current->ReplaceWith(callee_graph->GetNullConstant());
      } else if (argument->IsIntConstant()) {
        current->ReplaceWith(callee_graph->GetIntConstant(argument->AsIntConstant()->GetValue()));
      } else if (argument->IsLongConstant()) {
        current->ReplaceWith(callee_graph->GetLongConstant(argument->AsLongConstant()->GetValue()));
      } else if (argument->IsFloatConstant()) {
        current->ReplaceWith(
            callee_graph->GetFloatConstant(argument->AsFloatConstant()->GetValue()));
      } else if (argument->IsDoubleConstant()) {
        current->ReplaceWith(
            callee_graph->GetDoubleConstant(argument->AsDoubleConstant()->GetValue()));
      } else if (argument->GetType() == Primitive::kPrimNot) {
        current->SetReferenceTypeInfo(argument->GetReferenceTypeInfo());
        current->AsParameterValue()->SetCanBeNull(argument->CanBeNull());
      }
    }
  }

  size_t number_of_instructions_budget = kMaximumNumberOfHInstructions;
  size_t number_of_inlined_instructions =
      RunOptimizations(callee_graph, code_item, dex_compilation_unit);
  number_of_instructions_budget += number_of_inlined_instructions;

  // TODO: We should abort only if all predecessors throw. However,
  // HGraph::InlineInto currently does not handle an exit block with
  // a throw predecessor.
  HBasicBlock* exit_block = callee_graph->GetExitBlock();
  if (exit_block == nullptr) {
    VLOG(compiler) << "Method " << PrettyMethod(method_index, callee_dex_file)
                   << " could not be inlined because it has an infinite loop";
    return false;
  }

  bool has_throw_predecessor = false;
  for (HBasicBlock* predecessor : exit_block->GetPredecessors()) {
    if (predecessor->GetLastInstruction()->IsThrow()) {
      has_throw_predecessor = true;
      break;
    }
  }
  if (has_throw_predecessor) {
    VLOG(compiler) << "Method " << PrettyMethod(method_index, callee_dex_file)
                   << " could not be inlined because one branch always throws";
    return false;
  }

  HReversePostOrderIterator it(*callee_graph);
  it.Advance();  // Past the entry block, it does not contain instructions that prevent inlining.
  size_t number_of_instructions = 0;

  bool can_inline_environment =
      total_number_of_dex_registers_ < kMaximumNumberOfCumulatedDexRegisters;

  for (; !it.Done(); it.Advance()) {
    HBasicBlock* block = it.Current();

    if (block->IsLoopHeader() && block->GetLoopInformation()->IsIrreducible()) {
      // Don't inline methods with irreducible loops, they could prevent some
      // optimizations to run.
      VLOG(compiler) << "Method " << PrettyMethod(method_index, callee_dex_file)
                     << " could not be inlined because it contains an irreducible loop";
      return false;
    }

    for (HInstructionIterator instr_it(block->GetInstructions());
         !instr_it.Done();
         instr_it.Advance()) {
      if (number_of_instructions++ == number_of_instructions_budget) {
        VLOG(compiler) << "Method " << PrettyMethod(method_index, callee_dex_file)
                       << " is not inlined because its caller has reached"
                       << " its instruction budget limit.";
        return false;
      }
      HInstruction* current = instr_it.Current();
      if (!can_inline_environment && current->NeedsEnvironment()) {
        VLOG(compiler) << "Method " << PrettyMethod(method_index, callee_dex_file)
                       << " is not inlined because its caller has reached"
                       << " its environment budget limit.";
        return false;
      }

      if (current->IsInvokeInterface()) {
        // Disable inlining of interface calls. The cost in case of entering the
        // resolution conflict is currently too high.
        VLOG(compiler) << "Method " << PrettyMethod(method_index, callee_dex_file)
                       << " could not be inlined because it has an interface call.";
        return false;
      }

      if (!same_dex_file && current->NeedsEnvironment()) {
        VLOG(compiler) << "Method " << PrettyMethod(method_index, callee_dex_file)
                       << " could not be inlined because " << current->DebugName()
                       << " needs an environment and is in a different dex file";
        return false;
      }

      if (!same_dex_file && current->NeedsDexCacheOfDeclaringClass()) {
        VLOG(compiler) << "Method " << PrettyMethod(method_index, callee_dex_file)
                       << " could not be inlined because " << current->DebugName()
                       << " it is in a different dex file and requires access to the dex cache";
        return false;
      }

      if (current->IsNewInstance() &&
          (current->AsNewInstance()->GetEntrypoint() == kQuickAllocObjectWithAccessCheck)) {
        VLOG(compiler) << "Method " << PrettyMethod(method_index, callee_dex_file)
                       << " could not be inlined because it is using an entrypoint"
                       << " with access checks";
        // Allocation entrypoint does not handle inlined frames.
        return false;
      }

      if (current->IsNewArray() &&
          (current->AsNewArray()->GetEntrypoint() == kQuickAllocArrayWithAccessCheck)) {
        VLOG(compiler) << "Method " << PrettyMethod(method_index, callee_dex_file)
                       << " could not be inlined because it is using an entrypoint"
                       << " with access checks";
        // Allocation entrypoint does not handle inlined frames.
        return false;
      }

      if (current->IsUnresolvedStaticFieldGet() ||
          current->IsUnresolvedInstanceFieldGet() ||
          current->IsUnresolvedStaticFieldSet() ||
          current->IsUnresolvedInstanceFieldSet()) {
        // Entrypoint for unresolved fields does not handle inlined frames.
        VLOG(compiler) << "Method " << PrettyMethod(method_index, callee_dex_file)
                       << " could not be inlined because it is using an unresolved"
                       << " entrypoint";
        return false;
      }
    }
  }
  number_of_inlined_instructions_ += number_of_instructions;

  DCHECK_EQ(caller_instruction_counter, graph_->GetCurrentInstructionId())
      << "No instructions can be added to the outer graph while inner graph is being built";

  const int32_t callee_instruction_counter = callee_graph->GetCurrentInstructionId();
  graph_->SetCurrentInstructionId(callee_instruction_counter);
  *return_replacement = callee_graph->InlineInto(graph_, invoke_instruction);

  DCHECK_EQ(callee_instruction_counter, callee_graph->GetCurrentInstructionId())
      << "No instructions can be added to the inner graph during inlining into the outer graph";

  return true;
}

size_t HInliner::RunOptimizations(HGraph* callee_graph,
                                  const DexFile::CodeItem* code_item,
                                  const DexCompilationUnit& dex_compilation_unit) {
  // Note: if the outermost_graph_ is being compiled OSR, we should not run any
  // optimization that could lead to a HDeoptimize. The following optimizations do not.
  HDeadCodeElimination dce(callee_graph, stats_);
  HConstantFolding fold(callee_graph);
  HSharpening sharpening(callee_graph, codegen_, dex_compilation_unit, compiler_driver_);
  InstructionSimplifier simplify(callee_graph, stats_);
  IntrinsicsRecognizer intrinsics(callee_graph, compiler_driver_, stats_);

  HOptimization* optimizations[] = {
    &intrinsics,
    &sharpening,
    &simplify,
    &fold,
    &dce,
  };

  for (size_t i = 0; i < arraysize(optimizations); ++i) {
    HOptimization* optimization = optimizations[i];
    optimization->Run();
  }

  size_t number_of_inlined_instructions = 0u;
  if (depth_ + 1 < compiler_driver_->GetCompilerOptions().GetInlineDepthLimit()) {
    HInliner inliner(callee_graph,
                     outermost_graph_,
                     codegen_,
                     outer_compilation_unit_,
                     dex_compilation_unit,
                     compiler_driver_,
                     handles_,
                     stats_,
                     total_number_of_dex_registers_ + code_item->registers_size_,
                     depth_ + 1);
    inliner.Run();
    number_of_inlined_instructions += inliner.number_of_inlined_instructions_;
  }

  return number_of_inlined_instructions;
}

void HInliner::FixUpReturnReferenceType(HInvoke* invoke_instruction,
                                        ArtMethod* resolved_method,
                                        HInstruction* return_replacement,
                                        bool do_rtp) {
  // Check the integrity of reference types and run another type propagation if needed.
  if (return_replacement != nullptr) {
    if (return_replacement->GetType() == Primitive::kPrimNot) {
      if (!return_replacement->GetReferenceTypeInfo().IsValid()) {
        // Make sure that we have a valid type for the return. We may get an invalid one when
        // we inline invokes with multiple branches and create a Phi for the result.
        // TODO: we could be more precise by merging the phi inputs but that requires
        // some functionality from the reference type propagation.
        DCHECK(return_replacement->IsPhi());
        size_t pointer_size = Runtime::Current()->GetClassLinker()->GetImagePointerSize();
        mirror::Class* cls = resolved_method->GetReturnType(false /* resolve */, pointer_size);
        if (cls != nullptr && !cls->IsErroneous()) {
          ReferenceTypeInfo::TypeHandle return_handle = handles_->NewHandle(cls);
          return_replacement->SetReferenceTypeInfo(ReferenceTypeInfo::Create(
              return_handle, return_handle->CannotBeAssignedFromOtherTypes() /* is_exact */));
        } else {
          // Return inexact object type on failures.
          return_replacement->SetReferenceTypeInfo(graph_->GetInexactObjectRti());
        }
      }

      if (do_rtp) {
        // If the return type is a refinement of the declared type run the type propagation again.
        ReferenceTypeInfo return_rti = return_replacement->GetReferenceTypeInfo();
        ReferenceTypeInfo invoke_rti = invoke_instruction->GetReferenceTypeInfo();
        if (invoke_rti.IsStrictSupertypeOf(return_rti)
            || (return_rti.IsExact() && !invoke_rti.IsExact())
            || !return_replacement->CanBeNull()) {
          ReferenceTypePropagation(graph_,
                                   outer_compilation_unit_.GetDexCache(),
                                   handles_,
                                   /* is_first_run */ false).Run();
        }
      }
    } else if (return_replacement->IsInstanceOf()) {
      if (do_rtp) {
        // Inlining InstanceOf into an If may put a tighter bound on reference types.
        ReferenceTypePropagation(graph_,
                                 outer_compilation_unit_.GetDexCache(),
                                 handles_,
                                 /* is_first_run */ false).Run();
      }
    }
  }
}

}  // namespace art
