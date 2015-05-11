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

#include "builder.h"
#include "class_linker.h"
#include "constant_folding.h"
#include "dead_code_elimination.h"
#include "driver/compiler_driver-inl.h"
#include "driver/dex_compilation_unit.h"
#include "instruction_simplifier.h"
#include "mirror/art_method-inl.h"
#include "mirror/class_loader.h"
#include "mirror/dex_cache.h"
#include "nodes.h"
#include "register_allocator.h"
#include "ssa_phi_elimination.h"
#include "scoped_thread_state_change.h"
#include "thread.h"
#include "dex/verified_method.h"
#include "dex/verification_results.h"

namespace art {

static constexpr int kMaxInlineCodeUnits = 100;
static constexpr int kDepthLimit = 5;

void HInliner::Run() {
  if (graph_->IsDebuggable()) {
    // For simplicity, we currently never inline when the graph is debuggable. This avoids
    // doing some logic in the runtime to discover if a method could have been inlined.
    return;
  }
  const GrowableArray<HBasicBlock*>& blocks = graph_->GetReversePostOrder();
  for (size_t i = 0; i < blocks.Size(); ++i) {
    HBasicBlock* block = blocks.Get(i);
    for (HInstruction* instruction = block->GetFirstInstruction(); instruction != nullptr;) {
      HInstruction* next = instruction->GetNext();
      HInvokeStaticOrDirect* call = instruction->AsInvokeStaticOrDirect();
      // As long as the call is not intrinsified, it is worth trying to inline.
      if (call != nullptr && call->GetIntrinsic() == Intrinsics::kNone) {
        // We use the original invoke type to ensure the resolution of the called method
        // works properly.
        if (!TryInline(call, call->GetDexMethodIndex(), call->GetOriginalInvokeType())) {
          if (kIsDebugBuild) {
            std::string callee_name =
                PrettyMethod(call->GetDexMethodIndex(), *outer_compilation_unit_.GetDexFile());
            bool should_inline = callee_name.find("$inline$") != std::string::npos;
            CHECK(!should_inline) << "Could not inline " << callee_name;
          }
        }
      }
      instruction = next;
    }
  }
}

bool HInliner::TryInline(HInvoke* invoke_instruction,
                         uint32_t method_index,
                         InvokeType invoke_type) const {
  ScopedObjectAccess soa(Thread::Current());
  const DexFile& caller_dex_file = *caller_compilation_unit_.GetDexFile();
  VLOG(compiler) << "Try inlining " << PrettyMethod(method_index, caller_dex_file);

  StackHandleScope<3> hs(soa.Self());
  Handle<mirror::DexCache> dex_cache(
      hs.NewHandle(caller_compilation_unit_.GetClassLinker()->FindDexCache(caller_dex_file)));
  Handle<mirror::ClassLoader> class_loader(hs.NewHandle(
      soa.Decode<mirror::ClassLoader*>(caller_compilation_unit_.GetClassLoader())));
  Handle<mirror::ArtMethod> resolved_method(hs.NewHandle(
      compiler_driver_->ResolveMethod(
          soa, dex_cache, class_loader, &caller_compilation_unit_, method_index, invoke_type)));

  if (resolved_method.Get() == nullptr) {
    VLOG(compiler) << "Method cannot be resolved " << PrettyMethod(method_index, caller_dex_file);
    return false;
  }

  bool can_use_dex_cache = true;
  const DexFile& outer_dex_file = *outer_compilation_unit_.GetDexFile();
  if (resolved_method->GetDexFile()->GetLocation().compare(outer_dex_file.GetLocation()) != 0) {
    can_use_dex_cache = false;
  }

  const DexFile::CodeItem* code_item = resolved_method->GetCodeItem();

  if (code_item == nullptr) {
    VLOG(compiler) << "Method " << PrettyMethod(method_index, caller_dex_file)
                   << " is not inlined because it is native";
    return false;
  }

  if (code_item->insns_size_in_code_units_ > kMaxInlineCodeUnits) {
    VLOG(compiler) << "Method " << PrettyMethod(method_index, caller_dex_file)
                   << " is too big to inline";
    return false;
  }

  if (code_item->tries_size_ != 0) {
    VLOG(compiler) << "Method " << PrettyMethod(method_index, caller_dex_file)
                   << " is not inlined because of try block";
    return false;
  }

  uint16_t class_def_idx = resolved_method->GetDeclaringClass()->GetDexClassDefIndex();
  if (!compiler_driver_->IsMethodVerifiedWithoutFailures(
        resolved_method->GetDexMethodIndex(), class_def_idx, *resolved_method->GetDexFile())) {
    VLOG(compiler) << "Method " << PrettyMethod(method_index, caller_dex_file)
                   << " couldn't be verified, so it cannot be inlined";
    return false;
  }

  if (resolved_method->ShouldNotInline()) {
    VLOG(compiler) << "Method " << PrettyMethod(method_index, caller_dex_file)
                   << " was already flagged as non inlineable";
    return false;
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

  if (!TryBuildAndInline(resolved_method, invoke_instruction, method_index, can_use_dex_cache)) {
    resolved_method->SetShouldNotInline();
    return false;
  }

  VLOG(compiler) << "Successfully inlined " << PrettyMethod(method_index, caller_dex_file);
  MaybeRecordStat(kInlinedInvoke);
  return true;
}

bool HInliner::TryBuildAndInline(Handle<mirror::ArtMethod> resolved_method,
                                 HInvoke* invoke_instruction,
                                 uint32_t method_index,
                                 bool can_use_dex_cache) const {
  ScopedObjectAccess soa(Thread::Current());
  const DexFile::CodeItem* code_item = resolved_method->GetCodeItem();
  const DexFile& caller_dex_file = *caller_compilation_unit_.GetDexFile();

  DexCompilationUnit dex_compilation_unit(
    nullptr,
    caller_compilation_unit_.GetClassLoader(),
    caller_compilation_unit_.GetClassLinker(),
    *resolved_method->GetDexFile(),
    code_item,
    resolved_method->GetDeclaringClass()->GetDexClassDefIndex(),
    resolved_method->GetDexMethodIndex(),
    resolved_method->GetAccessFlags(),
    nullptr);

  HGraph* callee_graph = new (graph_->GetArena()) HGraph(
      graph_->GetArena(),
      caller_dex_file,
      method_index,
      graph_->IsDebuggable(),
      graph_->GetCurrentInstructionId());

  OptimizingCompilerStats inline_stats;
  HGraphBuilder builder(callee_graph,
                        &dex_compilation_unit,
                        &outer_compilation_unit_,
                        resolved_method->GetDexFile(),
                        compiler_driver_,
                        &inline_stats);

  if (!builder.BuildGraph(*code_item)) {
    VLOG(compiler) << "Method " << PrettyMethod(method_index, caller_dex_file)
                   << " could not be built, so cannot be inlined";
    return false;
  }

  if (!RegisterAllocator::CanAllocateRegistersFor(*callee_graph,
                                                  compiler_driver_->GetInstructionSet())) {
    VLOG(compiler) << "Method " << PrettyMethod(method_index, caller_dex_file)
                   << " cannot be inlined because of the register allocator";
    return false;
  }

  if (!callee_graph->TryBuildingSsa()) {
    VLOG(compiler) << "Method " << PrettyMethod(method_index, caller_dex_file)
                   << " could not be transformed to SSA";
    return false;
  }

  // Run simple optimizations on the graph.
  HDeadCodeElimination dce(callee_graph, stats_);
  HConstantFolding fold(callee_graph);
  InstructionSimplifier simplify(callee_graph, stats_);

  HOptimization* optimizations[] = {
    &dce,
    &fold,
    &simplify,
  };

  for (size_t i = 0; i < arraysize(optimizations); ++i) {
    HOptimization* optimization = optimizations[i];
    optimization->Run();
  }

  if (depth_ + 1 < kDepthLimit) {
    HInliner inliner(callee_graph,
                     outer_compilation_unit_,
                     dex_compilation_unit,
                     compiler_driver_,
                     stats_,
                     depth_ + 1);
    inliner.Run();
  }

  HReversePostOrderIterator it(*callee_graph);
  it.Advance();  // Past the entry block, it does not contain instructions that prevent inlining.
  for (; !it.Done(); it.Advance()) {
    HBasicBlock* block = it.Current();
    if (block->IsLoopHeader()) {
      VLOG(compiler) << "Method " << PrettyMethod(method_index, caller_dex_file)
                     << " could not be inlined because it contains a loop";
      return false;
    }

    for (HInstructionIterator instr_it(block->GetInstructions());
         !instr_it.Done();
         instr_it.Advance()) {
      HInstruction* current = instr_it.Current();
      if (current->IsSuspendCheck()) {
        continue;
      }

      if (current->CanThrow()) {
        VLOG(compiler) << "Method " << PrettyMethod(method_index, caller_dex_file)
                       << " could not be inlined because " << current->DebugName()
                       << " can throw";
        return false;
      }

      if (current->NeedsEnvironment()) {
        VLOG(compiler) << "Method " << PrettyMethod(method_index, caller_dex_file)
                       << " could not be inlined because " << current->DebugName()
                       << " needs an environment";
        return false;
      }

      if (!can_use_dex_cache && current->NeedsDexCache()) {
        VLOG(compiler) << "Method " << PrettyMethod(method_index, caller_dex_file)
                       << " could not be inlined because " << current->DebugName()
                       << " it is in a different dex file and requires access to the dex cache";
        return false;
      }
    }
  }

  callee_graph->InlineInto(graph_, invoke_instruction);

  if (callee_graph->HasBoundsChecks()) {
    graph_->SetHasBoundsChecks(true);
  }

  return true;
}

}  // namespace art
