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

#include "deoptimize_stack_visitor.h"

#include "mirror/art_method-inl.h"
#include "object_utils.h"
#include "quick_exception_handler.h"
#include "handle_scope-inl.h"
#include "verifier/method_verifier.h"

namespace art {

bool DeoptimizeStackVisitor::VisitFrame() {
  exception_handler_->SetHandlerFrameId(GetFrameId());
  mirror::ArtMethod* method = GetMethod();
  if (method == nullptr) {
    // This is the upcall, we remember the frame and last pc so that we may long jump to them.
    exception_handler_->SetHandlerQuickFramePc(GetCurrentQuickFramePc());
    exception_handler_->SetHandlerQuickFrame(GetCurrentQuickFrame());
    return false;  // End stack walk.
  } else if (method->IsRuntimeMethod()) {
    // Ignore callee save method.
    DCHECK(method->IsCalleeSaveMethod());
    return true;
  } else {
    return HandleDeoptimization(method);
  }
}

bool DeoptimizeStackVisitor::HandleDeoptimization(mirror::ArtMethod* m) {
  MethodHelper mh(m);
  const DexFile::CodeItem* code_item = mh.GetCodeItem();
  CHECK(code_item != nullptr);
  uint16_t num_regs = code_item->registers_size_;
  uint32_t dex_pc = GetDexPc();
  const Instruction* inst = Instruction::At(code_item->insns_ + dex_pc);
  uint32_t new_dex_pc = dex_pc + inst->SizeInCodeUnits();
  ShadowFrame* new_frame = ShadowFrame::Create(num_regs, nullptr, m, new_dex_pc);
  StackHandleScope<2> hs(self_);
  Handle<mirror::DexCache> dex_cache(hs.NewHandle(mh.GetDexCache()));
  Handle<mirror::ClassLoader> class_loader(hs.NewHandle(mh.GetClassLoader()));
  verifier::MethodVerifier verifier(&mh.GetDexFile(), &dex_cache, &class_loader,
                                    &mh.GetClassDef(), code_item, m->GetDexMethodIndex(), m,
                                    m->GetAccessFlags(), false, true);
  verifier.Verify();
  std::vector<int32_t> kinds = verifier.DescribeVRegs(dex_pc);
  for (uint16_t reg = 0; reg < num_regs; ++reg) {
    VRegKind kind = static_cast<VRegKind>(kinds.at(reg * 2));
    switch (kind) {
      case kUndefined:
        new_frame->SetVReg(reg, 0xEBADDE09);
        break;
      case kConstant:
        new_frame->SetVReg(reg, kinds.at((reg * 2) + 1));
        break;
      case kReferenceVReg:
        new_frame->SetVRegReference(reg,
                                    reinterpret_cast<mirror::Object*>(GetVReg(m, reg, kind)));
        break;
      default:
        new_frame->SetVReg(reg, GetVReg(m, reg, kind));
        break;
    }
  }
  if (prev_shadow_frame_ != nullptr) {
    prev_shadow_frame_->SetLink(new_frame);
  } else {
    self_->SetDeoptimizationShadowFrame(new_frame);
  }
  prev_shadow_frame_ = new_frame;
  return true;
}

}  // namespace art
