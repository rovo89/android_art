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

#include "catch_block_stack_visitor.h"

#include "dex_instruction.h"
#include "mirror/art_method-inl.h"
#include "quick_exception_handler.h"
#include "handle_scope-inl.h"
#include "verifier/method_verifier.h"

namespace art {

bool CatchBlockStackVisitor::VisitFrame() {
  exception_handler_->SetHandlerFrameId(GetFrameId());
  mirror::ArtMethod* method = GetMethod();
  if (method == nullptr) {
    // This is the upcall, we remember the frame and last pc so that we may long jump to them.
    exception_handler_->SetHandlerQuickFramePc(GetCurrentQuickFramePc());
    exception_handler_->SetHandlerQuickFrame(GetCurrentQuickFrame());
    return false;  // End stack walk.
  } else {
    if (method->IsRuntimeMethod()) {
      // Ignore callee save method.
      DCHECK(method->IsCalleeSaveMethod());
      return true;
    } else {
      return HandleTryItems(method);
    }
  }
}

bool CatchBlockStackVisitor::HandleTryItems(mirror::ArtMethod* method) {
  uint32_t dex_pc = DexFile::kDexNoIndex;
  if (!method->IsNative()) {
    dex_pc = GetDexPc();
  }
  if (dex_pc != DexFile::kDexNoIndex) {
    bool clear_exception = false;
    bool exc_changed = false;
    StackHandleScope<1> hs(Thread::Current());
    Handle<mirror::Class> to_find(hs.NewHandle((*exception_)->GetClass()));
    uint32_t found_dex_pc = method->FindCatchBlock(to_find, dex_pc, &clear_exception,
                                                   &exc_changed);
    if (UNLIKELY(exc_changed)) {
      DCHECK_EQ(DexFile::kDexNoIndex, found_dex_pc);
      exception_->Assign(self_->GetException(nullptr));  // TODO: Throw location?
      // There is a new context installed, delete it.
      delete self_->GetLongJumpContext();
    }
    exception_handler_->SetClearException(clear_exception);
    if (found_dex_pc != DexFile::kDexNoIndex) {
      exception_handler_->SetHandlerDexPc(found_dex_pc);
      exception_handler_->SetHandlerQuickFramePc(method->ToNativePc(found_dex_pc));
      exception_handler_->SetHandlerQuickFrame(GetCurrentQuickFrame());
      return false;  // End stack walk.
    }
  }
  return true;  // Continue stack walk.
}

}  // namespace art
