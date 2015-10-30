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

#include "sharpening.h"

#include "code_generator.h"
#include "utils/dex_cache_arrays_layout-inl.h"
#include "driver/compiler_driver.h"
#include "nodes.h"
#include "runtime.h"

namespace art {

void HSharpening::Run() {
  // We don't care about the order of the blocks here.
  for (HBasicBlock* block : graph_->GetReversePostOrder()) {
    for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
      HInstruction* instruction = it.Current();
      if (instruction->IsInvokeStaticOrDirect()) {
        ProcessInvokeStaticOrDirect(instruction->AsInvokeStaticOrDirect());
      }
      // TODO: Move the sharpening of invoke-virtual/-interface/-super from HGraphBuilder
      //       here. Rewrite it to avoid the CompilerDriver's reliance on verifier data
      //       because we know the type better when inlining.
      // TODO: HLoadClass, HLoadString - select PC relative dex cache array access if
      //       available.
    }
  }
}

void HSharpening::ProcessInvokeStaticOrDirect(HInvokeStaticOrDirect* invoke) {
  if (invoke->IsStringInit()) {
    // Not using the dex cache arrays. But we could still try to use a better dispatch...
    // TODO: Use direct_method and direct_code for the appropriate StringFactory method.
    return;
  }

  // TODO: Avoid CompilerDriver.
  InvokeType invoke_type = invoke->GetOriginalInvokeType();
  MethodReference target_method(&graph_->GetDexFile(), invoke->GetDexMethodIndex());
  int vtable_idx;
  uintptr_t direct_code, direct_method;
  bool success = compiler_driver_->ComputeInvokeInfo(
      &compilation_unit_,
      invoke->GetDexPc(),
      false /* update_stats: already updated in builder */,
      true /* enable_devirtualization */,
      &invoke_type,
      &target_method,
      &vtable_idx,
      &direct_code,
      &direct_method);
  DCHECK(success);
  DCHECK_EQ(invoke_type, invoke->GetInvokeType());
  DCHECK_EQ(target_method.dex_file, invoke->GetTargetMethod().dex_file);
  DCHECK_EQ(target_method.dex_method_index, invoke->GetTargetMethod().dex_method_index);

  HInvokeStaticOrDirect::MethodLoadKind method_load_kind;
  HInvokeStaticOrDirect::CodePtrLocation code_ptr_location;
  uint64_t method_load_data = 0u;
  uint64_t direct_code_ptr = 0u;

  HGraph* outer_graph = codegen_->GetGraph();
  if (target_method.dex_file == &outer_graph->GetDexFile() &&
      target_method.dex_method_index == outer_graph->GetMethodIdx()) {
    method_load_kind = HInvokeStaticOrDirect::MethodLoadKind::kRecursive;
    code_ptr_location = HInvokeStaticOrDirect::CodePtrLocation::kCallSelf;
  } else {
    bool use_pc_relative_instructions =
        ((direct_method == 0u || direct_code == static_cast<uintptr_t>(-1))) &&
        ContainsElement(compiler_driver_->GetDexFilesForOatFile(), target_method.dex_file);
    if (direct_method != 0u) {  // Should we use a direct pointer to the method?
      // Note: For JIT, kDirectAddressWithFixup doesn't make sense at all and while
      // kDirectAddress would be fine for image methods, we don't support it at the moment.
      DCHECK(!Runtime::Current()->UseJit());
      if (direct_method != static_cast<uintptr_t>(-1)) {  // Is the method pointer known now?
        method_load_kind = HInvokeStaticOrDirect::MethodLoadKind::kDirectAddress;
        method_load_data = direct_method;
      } else {  // The direct pointer will be known at link time.
        method_load_kind = HInvokeStaticOrDirect::MethodLoadKind::kDirectAddressWithFixup;
      }
    } else {  // Use dex cache.
      DCHECK_EQ(target_method.dex_file, &graph_->GetDexFile());
      if (use_pc_relative_instructions) {  // Can we use PC-relative access to the dex cache arrays?
        DCHECK(!Runtime::Current()->UseJit());
        method_load_kind = HInvokeStaticOrDirect::MethodLoadKind::kDexCachePcRelative;
        DexCacheArraysLayout layout(GetInstructionSetPointerSize(codegen_->GetInstructionSet()),
                                    &graph_->GetDexFile());
        method_load_data = layout.MethodOffset(target_method.dex_method_index);
      } else {  // We must go through the ArtMethod's pointer to resolved methods.
        method_load_kind = HInvokeStaticOrDirect::MethodLoadKind::kDexCacheViaMethod;
      }
    }
    if (direct_code != 0u) {  // Should we use a direct pointer to the code?
      // Note: For JIT, kCallPCRelative and kCallDirectWithFixup don't make sense at all and
      // while kCallDirect would be fine for image methods, we don't support it at the moment.
      DCHECK(!Runtime::Current()->UseJit());
      if (direct_code != static_cast<uintptr_t>(-1)) {  // Is the code pointer known now?
        code_ptr_location = HInvokeStaticOrDirect::CodePtrLocation::kCallDirect;
        direct_code_ptr = direct_code;
      } else if (use_pc_relative_instructions) {
        // Use PC-relative calls for invokes within a multi-dex oat file.
        code_ptr_location = HInvokeStaticOrDirect::CodePtrLocation::kCallPCRelative;
      } else {  // The direct pointer will be known at link time.
        // NOTE: This is used for app->boot calls when compiling an app against
        // a relocatable but not yet relocated image.
        code_ptr_location = HInvokeStaticOrDirect::CodePtrLocation::kCallDirectWithFixup;
      }
    } else {  // We must use the code pointer from the ArtMethod.
      code_ptr_location = HInvokeStaticOrDirect::CodePtrLocation::kCallArtMethod;
    }
  }

  if (graph_->IsDebuggable()) {
    // For debuggable apps always use the code pointer from ArtMethod
    // so that we don't circumvent instrumentation stubs if installed.
    code_ptr_location = HInvokeStaticOrDirect::CodePtrLocation::kCallArtMethod;
  }

  HInvokeStaticOrDirect::DispatchInfo desired_dispatch_info = {
      method_load_kind, code_ptr_location, method_load_data, direct_code_ptr
  };
  HInvokeStaticOrDirect::DispatchInfo dispatch_info =
      codegen_->GetSupportedInvokeStaticOrDirectDispatch(desired_dispatch_info,
                                                         invoke->GetTargetMethod());
  invoke->SetDispatchInfo(dispatch_info);
}

}  // namespace art
