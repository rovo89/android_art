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
#include "catch_finder.h"
#include "sirt_ref.h"
#include "verifier/method_verifier.h"

namespace art {

bool CatchBlockStackVisitor::VisitFrame() {
  catch_finder_->SetHandlerFrameId(GetFrameId());
  mirror::ArtMethod* method = GetMethod();
  if (method == nullptr) {
    // This is the upcall, we remember the frame and last pc so that we may long jump to them.
    catch_finder_->SetHandlerQuickFramePc(GetCurrentQuickFramePc());
    catch_finder_->SetHandlerQuickFrame(GetCurrentQuickFrame());
    return false;  // End stack walk.
  } else {
    if (method->IsRuntimeMethod()) {
      // Ignore callee save method.
      DCHECK(method->IsCalleeSaveMethod());
      return true;
    } else if (is_deoptimization_) {
      return HandleDeoptimization(method);
    } else {
      return HandleTryItems(method);
    }
  }
}

bool CatchBlockStackVisitor::HandleTryItems(mirror::ArtMethod* method) {
  uint32_t dex_pc = DexFile::kDexNoIndex;
  if (method->IsNative()) {
    ++native_method_count_;
  } else {
    dex_pc = GetDexPc();
  }
  if (dex_pc != DexFile::kDexNoIndex) {
    bool clear_exception = false;
    SirtRef<mirror::Class> sirt_method_to_find(Thread::Current(), to_find_);
    uint32_t found_dex_pc = method->FindCatchBlock(sirt_method_to_find, dex_pc, &clear_exception);
    to_find_ = sirt_method_to_find.get();
    catch_finder_->SetClearException(clear_exception);
    if (found_dex_pc != DexFile::kDexNoIndex) {
      catch_finder_->SetHandlerDexPc(found_dex_pc);
      catch_finder_->SetHandlerQuickFramePc(method->ToNativePc(found_dex_pc));
      catch_finder_->SetHandlerQuickFrame(GetCurrentQuickFrame());
      return false;  // End stack walk.
    }
  }
  return true;  // Continue stack walk.
}

bool CatchBlockStackVisitor::HandleDeoptimization(mirror::ArtMethod* m) {
  MethodHelper mh(m);
  const DexFile::CodeItem* code_item = mh.GetCodeItem();
  CHECK(code_item != nullptr);
  uint16_t num_regs = code_item->registers_size_;
  uint32_t dex_pc = GetDexPc();
  const Instruction* inst = Instruction::At(code_item->insns_ + dex_pc);
  uint32_t new_dex_pc = dex_pc + inst->SizeInCodeUnits();
  ShadowFrame* new_frame = ShadowFrame::Create(num_regs, nullptr, m, new_dex_pc);
  SirtRef<mirror::DexCache> dex_cache(self_, mh.GetDexCache());
  SirtRef<mirror::ClassLoader> class_loader(self_, mh.GetClassLoader());
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
    catch_finder_->SetTopShadowFrame(new_frame);
  }
  prev_shadow_frame_ = new_frame;
  return true;
}

}  // namespace art
