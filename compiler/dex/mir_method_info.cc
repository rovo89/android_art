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

# include "mir_method_info.h"

#include "dex/compiler_ir.h"
#include "dex/quick/dex_file_method_inliner.h"
#include "dex/quick/dex_file_to_method_inliner_map.h"
#include "dex/verified_method.h"
#include "driver/compiler_driver.h"
#include "driver/dex_compilation_unit.h"
#include "driver/compiler_driver-inl.h"
#include "driver/compiler_options.h"
#include "mirror/class_loader.h"  // Only to allow casts in Handle<ClassLoader>.
#include "mirror/dex_cache.h"     // Only to allow casts in Handle<DexCache>.
#include "scoped_thread_state_change.h"
#include "handle_scope-inl.h"

namespace art {

void MirMethodLoweringInfo::Resolve(CompilerDriver* compiler_driver,
                                    const DexCompilationUnit* mUnit,
                                    MirMethodLoweringInfo* method_infos, size_t count) {
  if (kIsDebugBuild) {
    DCHECK(method_infos != nullptr);
    DCHECK_NE(count, 0u);
    for (auto it = method_infos, end = method_infos + count; it != end; ++it) {
      MirMethodLoweringInfo unresolved(it->MethodIndex(), it->GetInvokeType(), it->IsQuickened());
      unresolved.declaring_dex_file_ = it->declaring_dex_file_;
      unresolved.vtable_idx_ = it->vtable_idx_;
      if (it->target_dex_file_ != nullptr) {
        unresolved.target_dex_file_ = it->target_dex_file_;
        unresolved.target_method_idx_ = it->target_method_idx_;
      }
      if (kIsDebugBuild) {
        unresolved.CheckEquals(*it);
      }
    }
  }

  // We're going to resolve methods and check access in a tight loop. It's better to hold
  // the lock and needed references once than re-acquiring them again and again.
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<4> hs(soa.Self());
  Handle<mirror::DexCache> dex_cache(hs.NewHandle(compiler_driver->GetDexCache(mUnit)));
  Handle<mirror::ClassLoader> class_loader(
      hs.NewHandle(compiler_driver->GetClassLoader(soa, mUnit)));
  Handle<mirror::Class> referrer_class(hs.NewHandle(
      compiler_driver->ResolveCompilingMethodsClass(soa, dex_cache, class_loader, mUnit)));
  auto current_dex_cache(hs.NewHandle<mirror::DexCache>(nullptr));
  // Even if the referrer class is unresolved (i.e. we're compiling a method without class
  // definition) we still want to resolve methods and record all available info.
  Runtime* const runtime = Runtime::Current();
  const DexFile* const dex_file = mUnit->GetDexFile();
  const bool use_jit = runtime->UseJit();
  const VerifiedMethod* const verified_method = mUnit->GetVerifiedMethod();
  DexFileToMethodInlinerMap* inliner_map = compiler_driver->GetMethodInlinerMap();
  DexFileMethodInliner* default_inliner =
      (inliner_map != nullptr) ? inliner_map->GetMethodInliner(dex_file) : nullptr;

  for (auto it = method_infos, end = method_infos + count; it != end; ++it) {
    // For quickened invokes, the dex method idx is actually the mir offset.
    if (it->IsQuickened()) {
      const auto* dequicken_ref = verified_method->GetDequickenIndex(it->method_idx_);
      CHECK(dequicken_ref != nullptr);
      it->target_dex_file_ = dequicken_ref->dex_file;
      it->target_method_idx_ = dequicken_ref->index;
    }
    // Remember devirtualized invoke target and set the called method to the default.
    MethodReference devirt_ref(it->target_dex_file_, it->target_method_idx_);
    MethodReference* devirt_target = (it->target_dex_file_ != nullptr) ? &devirt_ref : nullptr;
    InvokeType invoke_type = it->GetInvokeType();
    ArtMethod* resolved_method = nullptr;

    bool string_init = false;
    if (default_inliner->IsStringInitMethodIndex(it->MethodIndex())) {
      string_init = true;
      invoke_type = kDirect;
    }

    if (!it->IsQuickened()) {
      it->target_dex_file_ = dex_file;
      it->target_method_idx_ = it->MethodIndex();
      current_dex_cache.Assign(dex_cache.Get());
      resolved_method = compiler_driver->ResolveMethod(soa, dex_cache, class_loader, mUnit,
                                                       it->target_method_idx_, invoke_type, true);
    } else {
      // The method index is actually the dex PC in this case.
      // Calculate the proper dex file and target method idx.
      CHECK(use_jit);
      CHECK_EQ(invoke_type, kVirtual);
      // Don't devirt if we are in a different dex file since we can't have direct invokes in
      // another dex file unless we always put a direct / patch pointer.
      devirt_target = nullptr;
      current_dex_cache.Assign(runtime->GetClassLinker()->FindDexCache(*it->target_dex_file_));
      CHECK(current_dex_cache.Get() != nullptr);
      DexCompilationUnit cu(
          mUnit->GetCompilationUnit(), mUnit->GetClassLoader(), mUnit->GetClassLinker(),
          *it->target_dex_file_, nullptr /* code_item not used */, 0u /* class_def_idx not used */,
          it->target_method_idx_, 0u /* access_flags not used */,
          nullptr /* verified_method not used */);
      resolved_method = compiler_driver->ResolveMethod(soa, current_dex_cache, class_loader, &cu,
                                                       it->target_method_idx_, invoke_type, false);
      if (resolved_method == nullptr) {
        // If the method is null then it should be a miranda method, in this case try
        // re-loading it, this time as an interface method. The actual miranda method is in the
        // vtable, but it will resolve to an interface method.
        resolved_method = compiler_driver->ResolveMethod(
            soa, current_dex_cache, class_loader, &cu, it->target_method_idx_, kInterface, false);
        CHECK(resolved_method != nullptr);
      }
      if (resolved_method != nullptr) {
        // Since this was a dequickened virtual, it is guaranteed to be resolved. However, it may be
        // resolved to an interface method. If this is the case then change the invoke type to
        // interface with the assumption that sharp_type will be kVirtual.
        if (resolved_method->GetInvokeType() == kInterface) {
          it->flags_ = (it->flags_ & ~(kInvokeTypeMask << kBitInvokeTypeBegin)) |
              (static_cast<uint16_t>(kInterface) << kBitInvokeTypeBegin);
        }
      }
    }
    if (UNLIKELY(resolved_method == nullptr)) {
      continue;
    }

    compiler_driver->GetResolvedMethodDexFileLocation(resolved_method,
        &it->declaring_dex_file_, &it->declaring_class_idx_, &it->declaring_method_idx_);
    if (!it->IsQuickened()) {
      // For quickened invoke virtuals we may have desharpened to an interface method which
      // wont give us the right method index, in this case blindly dispatch or else we can't
      // compile the method. Converting the invoke to interface dispatch doesn't work since we
      // have no way to get the dex method index for quickened invoke virtuals in the interface
      // trampolines.
      it->vtable_idx_ =
          compiler_driver->GetResolvedMethodVTableIndex(resolved_method, invoke_type);
    }

    MethodReference target_method(it->target_dex_file_, it->target_method_idx_);
    int fast_path_flags = compiler_driver->IsFastInvoke(
        soa, current_dex_cache, class_loader, mUnit, referrer_class.Get(), resolved_method,
        &invoke_type, &target_method, devirt_target, &it->direct_code_, &it->direct_method_);
    const bool is_referrers_class = referrer_class.Get() == resolved_method->GetDeclaringClass();
    const bool is_class_initialized =
        compiler_driver->IsMethodsClassInitialized(referrer_class.Get(), resolved_method);

    // Check if the target method is intrinsic or special.
    InlineMethodFlags is_intrinsic_or_special = kNoInlineMethodFlags;
    if (inliner_map != nullptr) {
      auto* inliner = (target_method.dex_file == dex_file)
          ? default_inliner
          : inliner_map->GetMethodInliner(target_method.dex_file);
      is_intrinsic_or_special = inliner->IsIntrinsicOrSpecial(target_method.dex_method_index);
    }

    uint16_t other_flags = it->flags_ &
        ~(kFlagFastPath | kFlagIsIntrinsic | kFlagIsSpecial | kFlagClassIsInitialized |
            (kInvokeTypeMask << kBitSharpTypeBegin));
    it->flags_ = other_flags |
        // String init path is a special always-fast path.
        (fast_path_flags != 0 || string_init ? kFlagFastPath : 0u) |
        ((is_intrinsic_or_special & kInlineIntrinsic) != 0 ? kFlagIsIntrinsic : 0u) |
        ((is_intrinsic_or_special & kInlineSpecial) != 0 ? kFlagIsSpecial : 0u) |
        (static_cast<uint16_t>(invoke_type) << kBitSharpTypeBegin) |
        (is_referrers_class ? kFlagIsReferrersClass : 0u) |
        (is_class_initialized ? kFlagClassIsInitialized : 0u);
    it->target_dex_file_ = target_method.dex_file;
    it->target_method_idx_ = target_method.dex_method_index;
    it->stats_flags_ = fast_path_flags;
    if (string_init) {
      it->direct_code_ = 0;
    }
  }
}

}  // namespace art
