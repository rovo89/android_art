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
#include "driver/compiler_driver-inl.h"
#include "driver/compiler_options.h"
#include "driver/dex_compilation_unit.h"
#include "instruction_simplifier.h"
#include "intrinsics.h"
#include "mirror/class_loader.h"
#include "mirror/dex_cache.h"
#include "nodes.h"
#include "optimizing_compiler.h"
#include "reference_type_propagation.h"
#include "register_allocator.h"
#include "sharpening.h"
#include "ssa_phi_elimination.h"
#include "scoped_thread_state_change.h"
#include "thread.h"
#include "dex/verified_method.h"
#include "dex/verification_results.h"

namespace art {

static constexpr size_t kMaximumNumberOfHInstructions = 12;

void HInliner::Run() {
  const CompilerOptions& compiler_options = compiler_driver_->GetCompilerOptions();
  if ((compiler_options.GetInlineDepthLimit() == 0)
      || (compiler_options.GetInlineMaxCodeUnits() == 0)) {
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

static uint32_t FindMethodIndexIn(ArtMethod* method,
                                  const DexFile& dex_file,
                                  uint32_t referrer_index)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  if (method->GetDexFile()->GetLocation().compare(dex_file.GetLocation()) == 0) {
    return method->GetDexMethodIndex();
  } else {
    return method->FindDexMethodIndexInOtherDexFile(dex_file, referrer_index);
  }
}

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
  if (invoke_instruction->IsInvokeStaticOrDirect()) {
    MethodReference ref = invoke_instruction->AsInvokeStaticOrDirect()->GetTargetMethod();
    mirror::DexCache* const dex_cache = (&caller_dex_file == ref.dex_file)
        ? caller_compilation_unit_.GetDexCache().Get()
        : class_linker->FindDexCache(soa.Self(), *ref.dex_file);
    resolved_method = dex_cache->GetResolvedMethod(
        ref.dex_method_index, class_linker->GetImagePointerSize());
  } else {
    resolved_method = caller_compilation_unit_.GetDexCache().Get()->GetResolvedMethod(
        method_index, class_linker->GetImagePointerSize());
  }

  if (resolved_method == nullptr) {
    // TODO: Can this still happen?
    // Method cannot be resolved if it is in another dex file we do not have access to.
    VLOG(compiler) << "Method cannot be resolved " << PrettyMethod(method_index, caller_dex_file);
    return false;
  }

  if (!invoke_instruction->IsInvokeStaticOrDirect()) {
    resolved_method = FindVirtualOrInterfaceTarget(invoke_instruction, resolved_method);
    if (resolved_method == nullptr) {
      VLOG(compiler) << "Interface or virtual call to "
                     << PrettyMethod(method_index, caller_dex_file)
                     << " could not be statically determined";
      return false;
    }
    // We have found a method, but we need to find where that method is for the caller's
    // dex file.
    method_index = FindMethodIndexIn(resolved_method, caller_dex_file, method_index);
    if (method_index == DexFile::kDexNoIndex) {
      VLOG(compiler) << "Interface or virtual call to "
                     << PrettyMethod(resolved_method)
                     << " cannot be inlined because unaccessible to caller";
      return false;
    }
  }

  bool same_dex_file =
      IsSameDexFile(*outer_compilation_unit_.GetDexFile(), *resolved_method->GetDexFile());

  const DexFile::CodeItem* code_item = resolved_method->GetCodeItem();

  if (code_item == nullptr) {
    VLOG(compiler) << "Method " << PrettyMethod(method_index, caller_dex_file)
                   << " is not inlined because it is native";
    return false;
  }

  size_t inline_max_code_units = compiler_driver_->GetCompilerOptions().GetInlineMaxCodeUnits();
  if (code_item->insns_size_in_code_units_ > inline_max_code_units) {
    VLOG(compiler) << "Method " << PrettyMethod(method_index, caller_dex_file)
                   << " is too big to inline";
    return false;
  }

  if (code_item->tries_size_ != 0) {
    VLOG(compiler) << "Method " << PrettyMethod(method_index, caller_dex_file)
                   << " is not inlined because of try block";
    return false;
  }

  if (!resolved_method->GetDeclaringClass()->IsVerified()) {
    uint16_t class_def_idx = resolved_method->GetDeclaringClass()->GetDexClassDefIndex();
    if (!compiler_driver_->IsMethodVerifiedWithoutFailures(
          resolved_method->GetDexMethodIndex(), class_def_idx, *resolved_method->GetDexFile())) {
      VLOG(compiler) << "Method " << PrettyMethod(method_index, caller_dex_file)
                     << " couldn't be verified, so it cannot be inlined";
      return false;
    }
  }

  if (invoke_instruction->IsInvokeStaticOrDirect() &&
      invoke_instruction->AsInvokeStaticOrDirect()->IsStaticWithImplicitClinitCheck()) {
    // Case of a static method that cannot be inlined because it implicitly
    // requires an initialization check of its declaring class.
    VLOG(compiler) << "Method " << PrettyMethod(method_index, caller_dex_file)
                   << " is not inlined because it is static and requires a clinit"
                   << " check that cannot be emitted due to Dex cache limitations";
    return false;
  }

  if (!TryBuildAndInline(resolved_method, invoke_instruction, same_dex_file)) {
    return false;
  }

  VLOG(compiler) << "Successfully inlined " << PrettyMethod(method_index, caller_dex_file);
  MaybeRecordStat(kInlinedInvoke);
  return true;
}

bool HInliner::TryBuildAndInline(ArtMethod* resolved_method,
                                 HInvoke* invoke_instruction,
                                 bool same_dex_file) {
  ScopedObjectAccess soa(Thread::Current());
  const DexFile::CodeItem* code_item = resolved_method->GetCodeItem();
  const DexFile& callee_dex_file = *resolved_method->GetDexFile();
  uint32_t method_index = resolved_method->GetDexMethodIndex();
  ClassLinker* class_linker = caller_compilation_unit_.GetClassLinker();
  Handle<mirror::DexCache> dex_cache(handles_->NewHandle(resolved_method->GetDexCache()));
  DexCompilationUnit dex_compilation_unit(
    nullptr,
    caller_compilation_unit_.GetClassLoader(),
    class_linker,
    callee_dex_file,
    code_item,
    resolved_method->GetDeclaringClass()->GetDexClassDefIndex(),
    method_index,
    resolved_method->GetAccessFlags(),
    compiler_driver_->GetVerifiedMethod(&callee_dex_file, method_index),
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
  HGraph* callee_graph = new (graph_->GetArena()) HGraph(
      graph_->GetArena(),
      callee_dex_file,
      method_index,
      requires_ctor_barrier,
      compiler_driver_->GetInstructionSet(),
      invoke_type,
      graph_->IsDebuggable(),
      graph_->GetCurrentInstructionId());

  OptimizingCompilerStats inline_stats;
  HGraphBuilder builder(callee_graph,
                        &dex_compilation_unit,
                        &outer_compilation_unit_,
                        resolved_method->GetDexFile(),
                        compiler_driver_,
                        &inline_stats,
                        resolved_method->GetQuickenedInfo(),
                        dex_cache);

  if (!builder.BuildGraph(*code_item)) {
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

  if (!callee_graph->TryBuildingSsa()) {
    VLOG(compiler) << "Method " << PrettyMethod(method_index, callee_dex_file)
                   << " could not be transformed to SSA";
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

  // Run simple optimizations on the graph.
  HDeadCodeElimination dce(callee_graph, stats_);
  HConstantFolding fold(callee_graph);
  ReferenceTypePropagation type_propagation(callee_graph, handles_);
  HSharpening sharpening(callee_graph, codegen_, dex_compilation_unit, compiler_driver_);
  InstructionSimplifier simplify(callee_graph, stats_);
  IntrinsicsRecognizer intrinsics(callee_graph, compiler_driver_);

  HOptimization* optimizations[] = {
    &intrinsics,
    &type_propagation,
    &sharpening,
    &simplify,
    &fold,
    &dce,
  };

  for (size_t i = 0; i < arraysize(optimizations); ++i) {
    HOptimization* optimization = optimizations[i];
    optimization->Run();
  }

  size_t number_of_instructions_budget = kMaximumNumberOfHInstructions;
  if (depth_ + 1 < compiler_driver_->GetCompilerOptions().GetInlineDepthLimit()) {
    HInliner inliner(callee_graph,
                     codegen_,
                     outer_compilation_unit_,
                     dex_compilation_unit,
                     compiler_driver_,
                     handles_,
                     stats_,
                     depth_ + 1);
    inliner.Run();
    number_of_instructions_budget += inliner.number_of_inlined_instructions_;
  }

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
  for (; !it.Done(); it.Advance()) {
    HBasicBlock* block = it.Current();
    if (block->IsLoopHeader()) {
      VLOG(compiler) << "Method " << PrettyMethod(method_index, callee_dex_file)
                     << " could not be inlined because it contains a loop";
      return false;
    }

    for (HInstructionIterator instr_it(block->GetInstructions());
         !instr_it.Done();
         instr_it.Advance()) {
      if (number_of_instructions++ ==  number_of_instructions_budget) {
        VLOG(compiler) << "Method " << PrettyMethod(method_index, callee_dex_file)
                       << " could not be inlined because it is too big.";
        return false;
      }
      HInstruction* current = instr_it.Current();

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
        // Allocation entrypoint does not handle inlined frames.
        return false;
      }

      if (current->IsNewArray() &&
          (current->AsNewArray()->GetEntrypoint() == kQuickAllocArrayWithAccessCheck)) {
        // Allocation entrypoint does not handle inlined frames.
        return false;
      }

      if (current->IsUnresolvedStaticFieldGet() ||
          current->IsUnresolvedInstanceFieldGet() ||
          current->IsUnresolvedStaticFieldSet() ||
          current->IsUnresolvedInstanceFieldSet()) {
        // Entrypoint for unresolved fields does not handle inlined frames.
        return false;
      }
    }
  }
  number_of_inlined_instructions_ += number_of_instructions;

  HInstruction* return_replacement = callee_graph->InlineInto(graph_, invoke_instruction);
  if (return_replacement != nullptr) {
    DCHECK_EQ(graph_, return_replacement->GetBlock()->GetGraph());
  }

  // When merging the graph we might create a new NullConstant in the caller graph which does
  // not have the chance to be typed. We assign the correct type here so that we can keep the
  // assertion that every reference has a valid type. This also simplifies checks along the way.
  HNullConstant* null_constant = graph_->GetNullConstant();
  if (!null_constant->GetReferenceTypeInfo().IsValid()) {
    ReferenceTypeInfo::TypeHandle obj_handle =
            handles_->NewHandle(class_linker->GetClassRoot(ClassLinker::kJavaLangObject));
    null_constant->SetReferenceTypeInfo(
            ReferenceTypeInfo::Create(obj_handle, false /* is_exact */));
  }

  if ((return_replacement != nullptr)
      && (return_replacement->GetType() == Primitive::kPrimNot)) {
    if (!return_replacement->GetReferenceTypeInfo().IsValid()) {
      // Make sure that we have a valid type for the return. We may get an invalid one when
      // we inline invokes with multiple branches and create a Phi for the result.
      // TODO: we could be more precise by merging the phi inputs but that requires
      // some functionality from the reference type propagation.
      DCHECK(return_replacement->IsPhi());
      size_t pointer_size = Runtime::Current()->GetClassLinker()->GetImagePointerSize();
      ReferenceTypeInfo::TypeHandle return_handle =
        handles_->NewHandle(resolved_method->GetReturnType(true /* resolve */, pointer_size));
      return_replacement->SetReferenceTypeInfo(ReferenceTypeInfo::Create(
         return_handle, return_handle->CannotBeAssignedFromOtherTypes() /* is_exact */));
    }
  }

  return true;
}

}  // namespace art
