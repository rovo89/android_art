/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include "callee_save_frame.h"
#include "interpreter/interpreter.h"
#include "mirror/abstract_method-inl.h"
#include "mirror/object_array-inl.h"
#include "object_utils.h"
#include "stack.h"
#include "thread.h"
#include "verifier/method_verifier.h"

namespace art {

extern "C" uint64_t artDeoptimize(JValue ret_val, Thread* self, mirror::AbstractMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  // Return value may hold Object* so avoid suspension.
  const char* old_cause = self->StartAssertNoThreadSuspension("Deoptimizing stack frame");
  CHECK(old_cause == NULL);
  class DeoptimizationVisitor : public StackVisitor {
   public:
    DeoptimizationVisitor(Thread* thread, Context* context)
        : StackVisitor(thread, context), shadow_frame_(NULL), runtime_frames_(0) { }

    virtual bool VisitFrame() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
      mirror::AbstractMethod* m = GetMethod();
      if (m->IsRuntimeMethod()) {
        if (runtime_frames_ == 0) {
          runtime_frames_++;
          return true;  // Skip the callee save frame.
        } else {
          return false;  // Caller was an upcall.
        }
      }
      MethodHelper mh(m);
      const DexFile::CodeItem* code_item = mh.GetCodeItem();
      CHECK(code_item != NULL);
      uint16_t num_regs =  code_item->registers_size_;
      shadow_frame_ = ShadowFrame::Create(num_regs, NULL, m, GetDexPc());
      std::vector<int32_t> kinds = DescribeVRegs(m->GetDexMethodIndex(), &mh.GetDexFile(),
                                                 mh.GetDexCache(), mh.GetClassLoader(),
                                                 mh.GetClassDefIndex(), code_item, m,
                                                 m->GetAccessFlags(), GetDexPc());
      for(uint16_t reg = 0; reg < num_regs; reg++) {
        VRegKind kind = static_cast<VRegKind>(kinds.at(reg * 2));
        switch (kind) {
          case kUndefined:
            shadow_frame_->SetVReg(reg, 0xEBADDE09);
            break;
          case kConstant:
            shadow_frame_->SetVReg(reg, kinds.at((reg * 2) + 1));
            break;
          default:
            shadow_frame_->SetVReg(reg, GetVReg(m, reg, kind));
            break;
        }
      }
      return false;  // Stop now we have built the shadow frame.
    }

    std::vector<int32_t> DescribeVRegs(uint32_t dex_method_idx,
                                       const DexFile* dex_file,
                                       mirror::DexCache* dex_cache,
                                       mirror::ClassLoader* class_loader,
                                       uint32_t class_def_idx,
                                       const DexFile::CodeItem* code_item,
                                       mirror::AbstractMethod* method,
                                       uint32_t method_access_flags, uint32_t dex_pc)
        SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
      verifier::MethodVerifier verifier(dex_file, dex_cache, class_loader, class_def_idx, code_item,
                                        dex_method_idx, method, method_access_flags, true);
      verifier.Verify();
      return verifier.DescribeVRegs(dex_pc);
    }

    ShadowFrame* shadow_frame_;
    uint32_t runtime_frames_;
  } visitor(self, self->GetLongJumpContext());
  visitor.WalkStack(false);
  if (visitor.shadow_frame_ != NULL) {
    self->SetDeoptimizationShadowFrame(visitor.shadow_frame_, ret_val);
    return (*sp)->GetFrameSizeInBytes();
  } else {
    return 0;  // Caller was an upcall.
  }
}


extern "C" JValue artEnterInterpreterFromDeoptimize(Thread* self, mirror::AbstractMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  JValue return_value;
  UniquePtr<ShadowFrame> shadow_frame(self->GetAndClearDeoptimizationShadowFrame(&return_value));
  self->EndAssertNoThreadSuspension(NULL);
  return interpreter::EnterInterpreterFromDeoptimize(self, *shadow_frame.get(), return_value);
}

}  // namespace art
