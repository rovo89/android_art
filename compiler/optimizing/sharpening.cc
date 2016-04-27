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

#include "base/casts.h"
#include "class_linker.h"
#include "code_generator.h"
#include "driver/dex_compilation_unit.h"
#include "utils/dex_cache_arrays_layout-inl.h"
#include "driver/compiler_driver.h"
#include "gc/heap.h"
#include "gc/space/image_space.h"
#include "handle_scope-inl.h"
#include "mirror/dex_cache.h"
#include "mirror/string.h"
#include "nodes.h"
#include "runtime.h"
#include "scoped_thread_state_change.h"

namespace art {

void HSharpening::Run() {
  // We don't care about the order of the blocks here.
  for (HBasicBlock* block : graph_->GetReversePostOrder()) {
    for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
      HInstruction* instruction = it.Current();
      if (instruction->IsInvokeStaticOrDirect()) {
        ProcessInvokeStaticOrDirect(instruction->AsInvokeStaticOrDirect());
      } else if (instruction->IsLoadString()) {
        ProcessLoadString(instruction->AsLoadString());
      }
      // TODO: Move the sharpening of invoke-virtual/-interface/-super from HGraphBuilder
      //       here. Rewrite it to avoid the CompilerDriver's reliance on verifier data
      //       because we know the type better when inlining.
      // TODO: HLoadClass - select better load kind if available.
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
  InvokeType original_invoke_type = invoke->GetOriginalInvokeType();
  InvokeType optimized_invoke_type = original_invoke_type;
  MethodReference target_method(&graph_->GetDexFile(), invoke->GetDexMethodIndex());
  int vtable_idx;
  uintptr_t direct_code, direct_method;
  bool success = compiler_driver_->ComputeInvokeInfo(
      &compilation_unit_,
      invoke->GetDexPc(),
      false /* update_stats: already updated in builder */,
      true /* enable_devirtualization */,
      &optimized_invoke_type,
      &target_method,
      &vtable_idx,
      &direct_code,
      &direct_method);
  if (!success) {
    // TODO: try using kDexCachePcRelative. It's always a valid method load
    // kind as long as it's supported by the codegen
    return;
  }
  invoke->SetOptimizedInvokeType(optimized_invoke_type);
  invoke->SetTargetMethod(target_method);

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
      DCHECK(!Runtime::Current()->UseJitCompilation());
      if (direct_method != static_cast<uintptr_t>(-1)) {  // Is the method pointer known now?
        method_load_kind = HInvokeStaticOrDirect::MethodLoadKind::kDirectAddress;
        method_load_data = direct_method;
      } else {  // The direct pointer will be known at link time.
        method_load_kind = HInvokeStaticOrDirect::MethodLoadKind::kDirectAddressWithFixup;
      }
    } else {  // Use dex cache.
      DCHECK_EQ(target_method.dex_file, &graph_->GetDexFile());
      if (use_pc_relative_instructions) {  // Can we use PC-relative access to the dex cache arrays?
        DCHECK(!Runtime::Current()->UseJitCompilation());
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
      DCHECK(!Runtime::Current()->UseJitCompilation());
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

void HSharpening::ProcessLoadString(HLoadString* load_string) {
  DCHECK_EQ(load_string->GetLoadKind(), HLoadString::LoadKind::kDexCacheViaMethod);
  DCHECK(!load_string->IsInDexCache());

  const DexFile& dex_file = load_string->GetDexFile();
  uint32_t string_index = load_string->GetStringIndex();

  bool is_in_dex_cache = false;
  HLoadString::LoadKind desired_load_kind;
  uint64_t address = 0u;  // String or dex cache element address.
  {
    Runtime* runtime = Runtime::Current();
    ClassLinker* class_linker = runtime->GetClassLinker();
    ScopedObjectAccess soa(Thread::Current());
    StackHandleScope<1> hs(soa.Self());
    Handle<mirror::DexCache> dex_cache = IsSameDexFile(dex_file, *compilation_unit_.GetDexFile())
        ? compilation_unit_.GetDexCache()
        : hs.NewHandle(class_linker->FindDexCache(soa.Self(), dex_file));

    if (compiler_driver_->IsBootImage()) {
      // Compiling boot image. Resolve the string and allocate it if needed.
      DCHECK(!runtime->UseJitCompilation());
      mirror::String* string = class_linker->ResolveString(dex_file, string_index, dex_cache);
      CHECK(string != nullptr);
      if (!compiler_driver_->GetSupportBootImageFixup()) {
        // MIPS/MIPS64 or compiler_driver_test. Do not sharpen.
        desired_load_kind = HLoadString::LoadKind::kDexCacheViaMethod;
      } else {
        DCHECK(ContainsElement(compiler_driver_->GetDexFilesForOatFile(), &dex_file));
        is_in_dex_cache = true;
        desired_load_kind = codegen_->GetCompilerOptions().GetCompilePic()
            ? HLoadString::LoadKind::kBootImageLinkTimePcRelative
            : HLoadString::LoadKind::kBootImageLinkTimeAddress;
      }
    } else if (runtime->UseJitCompilation()) {
      // TODO: Make sure we don't set the "compile PIC" flag for JIT as that's bogus.
      // DCHECK(!codegen_->GetCompilerOptions().GetCompilePic());
      mirror::String* string = dex_cache->GetResolvedString(string_index);
      is_in_dex_cache = (string != nullptr);
      if (string != nullptr && runtime->GetHeap()->ObjectIsInBootImageSpace(string)) {
        desired_load_kind = HLoadString::LoadKind::kBootImageAddress;
        address = reinterpret_cast64<uint64_t>(string);
      } else {
        // Note: If the string is not in the dex cache, the instruction needs environment
        // and will not be inlined across dex files. Within a dex file, the slow-path helper
        // loads the correct string and inlined frames are used correctly for OOM stack trace.
        // TODO: Write a test for this.
        desired_load_kind = HLoadString::LoadKind::kDexCacheAddress;
        void* dex_cache_element_address = &dex_cache->GetStrings()[string_index];
        address = reinterpret_cast64<uint64_t>(dex_cache_element_address);
      }
    } else {
      // AOT app compilation. Try to lookup the string without allocating if not found.
      mirror::String* string = class_linker->LookupString(dex_file, string_index, dex_cache);
      if (string != nullptr && runtime->GetHeap()->ObjectIsInBootImageSpace(string)) {
        if (codegen_->GetCompilerOptions().GetCompilePic()) {
          // Use PC-relative load from the dex cache if the dex file belongs
          // to the oat file that we're currently compiling.
          desired_load_kind = ContainsElement(compiler_driver_->GetDexFilesForOatFile(), &dex_file)
              ? HLoadString::LoadKind::kDexCachePcRelative
              : HLoadString::LoadKind::kDexCacheViaMethod;
        } else {
          desired_load_kind = HLoadString::LoadKind::kBootImageAddress;
          address = reinterpret_cast64<uint64_t>(string);
        }
      } else {
        // Not JIT and the string is not in boot image.
        desired_load_kind = HLoadString::LoadKind::kDexCachePcRelative;
      }
    }
  }
  if (is_in_dex_cache) {
    load_string->MarkInDexCache();
  }

  HLoadString::LoadKind load_kind = codegen_->GetSupportedLoadStringKind(desired_load_kind);
  switch (load_kind) {
    case HLoadString::LoadKind::kBootImageLinkTimeAddress:
    case HLoadString::LoadKind::kBootImageLinkTimePcRelative:
    case HLoadString::LoadKind::kDexCacheViaMethod:
      load_string->SetLoadKindWithStringReference(load_kind, dex_file, string_index);
      break;
    case HLoadString::LoadKind::kBootImageAddress:
    case HLoadString::LoadKind::kDexCacheAddress:
      DCHECK_NE(address, 0u);
      load_string->SetLoadKindWithAddress(load_kind, address);
      break;
    case HLoadString::LoadKind::kDexCachePcRelative: {
      size_t pointer_size = InstructionSetPointerSize(codegen_->GetInstructionSet());
      DexCacheArraysLayout layout(pointer_size, &dex_file);
      size_t element_index = layout.StringOffset(string_index);
      load_string->SetLoadKindWithDexCacheReference(load_kind, dex_file, element_index);
      break;
    }
  }
}

}  // namespace art
