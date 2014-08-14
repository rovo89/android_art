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

#include "quick_exception_handler.h"

#include "arch/context.h"
#include "dex_instruction.h"
#include "entrypoints/entrypoint_utils.h"
#include "handle_scope-inl.h"
#include "mirror/art_method-inl.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "mirror/throwable.h"
#include "verifier/method_verifier.h"

namespace art {

static constexpr bool kDebugExceptionDelivery = false;
static constexpr size_t kInvalidFrameDepth = 0xffffffff;

QuickExceptionHandler::QuickExceptionHandler(Thread* self, bool is_deoptimization)
  : self_(self), context_(self->GetLongJumpContext()), is_deoptimization_(is_deoptimization),
    method_tracing_active_(is_deoptimization ||
                           Runtime::Current()->GetInstrumentation()->AreExitStubsInstalled()),
    handler_quick_frame_(nullptr), handler_quick_frame_pc_(0), handler_method_(nullptr),
    handler_dex_pc_(0), clear_exception_(false), handler_frame_depth_(kInvalidFrameDepth) {
}

// Finds catch handler or prepares for deoptimization.
class CatchBlockStackVisitor FINAL : public StackVisitor {
 public:
  CatchBlockStackVisitor(Thread* self, Context* context, Handle<mirror::Throwable>* exception,
                         QuickExceptionHandler* exception_handler)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : StackVisitor(self, context), self_(self), exception_(exception),
        exception_handler_(exception_handler) {
  }

  bool VisitFrame() OVERRIDE SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    mirror::ArtMethod* method = GetMethod();
    exception_handler_->SetHandlerFrameDepth(GetFrameDepth());
    if (method == nullptr) {
      // This is the upcall, we remember the frame and last pc so that we may long jump to them.
      exception_handler_->SetHandlerQuickFramePc(GetCurrentQuickFramePc());
      exception_handler_->SetHandlerQuickFrame(GetCurrentQuickFrame());
      uint32_t next_dex_pc;
      mirror::ArtMethod* next_art_method;
      bool has_next = GetNextMethodAndDexPc(&next_art_method, &next_dex_pc);
      // Report the method that did the down call as the handler.
      exception_handler_->SetHandlerDexPc(next_dex_pc);
      exception_handler_->SetHandlerMethod(next_art_method);
      if (!has_next) {
        // No next method? Check exception handler is set up for the unhandled exception handler
        // case.
        DCHECK_EQ(0U, exception_handler_->GetHandlerDexPc());
        DCHECK(nullptr == exception_handler_->GetHandlerMethod());
      }
      return false;  // End stack walk.
    }
    if (method->IsRuntimeMethod()) {
      // Ignore callee save method.
      DCHECK(method->IsCalleeSaveMethod());
      return true;
    }
    StackHandleScope<1> hs(self_);
    return HandleTryItems(hs.NewHandle(method));
  }

 private:
  bool HandleTryItems(Handle<mirror::ArtMethod> method)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    uint32_t dex_pc = DexFile::kDexNoIndex;
    if (!method->IsNative()) {
      dex_pc = GetDexPc();
    }
    if (dex_pc != DexFile::kDexNoIndex) {
      bool clear_exception = false;
      StackHandleScope<1> hs(Thread::Current());
      Handle<mirror::Class> to_find(hs.NewHandle((*exception_)->GetClass()));
      uint32_t found_dex_pc = mirror::ArtMethod::FindCatchBlock(method, to_find, dex_pc,
                                                                &clear_exception);
      exception_handler_->SetClearException(clear_exception);
      if (found_dex_pc != DexFile::kDexNoIndex) {
        exception_handler_->SetHandlerMethod(method.Get());
        exception_handler_->SetHandlerDexPc(found_dex_pc);
        exception_handler_->SetHandlerQuickFramePc(method->ToNativePc(found_dex_pc));
        exception_handler_->SetHandlerQuickFrame(GetCurrentQuickFrame());
        return false;  // End stack walk.
      }
    }
    return true;  // Continue stack walk.
  }

  Thread* const self_;
  // The exception we're looking for the catch block of.
  Handle<mirror::Throwable>* exception_;
  // The quick exception handler we're visiting for.
  QuickExceptionHandler* const exception_handler_;

  DISALLOW_COPY_AND_ASSIGN(CatchBlockStackVisitor);
};

void QuickExceptionHandler::FindCatch(const ThrowLocation& throw_location,
                                      mirror::Throwable* exception,
                                      bool is_exception_reported) {
  DCHECK(!is_deoptimization_);
  if (kDebugExceptionDelivery) {
    mirror::String* msg = exception->GetDetailMessage();
    std::string str_msg(msg != nullptr ? msg->ToModifiedUtf8() : "");
    self_->DumpStack(LOG(INFO) << "Delivering exception: " << PrettyTypeOf(exception)
                     << ": " << str_msg << "\n");
  }
  StackHandleScope<1> hs(self_);
  Handle<mirror::Throwable> exception_ref(hs.NewHandle(exception));

  // Walk the stack to find catch handler or prepare for deoptimization.
  CatchBlockStackVisitor visitor(self_, context_, &exception_ref, this);
  visitor.WalkStack(true);

  if (kDebugExceptionDelivery) {
    if (handler_quick_frame_->AsMirrorPtr() == nullptr) {
      LOG(INFO) << "Handler is upcall";
    }
    if (handler_method_ != nullptr) {
      const DexFile& dex_file = *handler_method_->GetDeclaringClass()->GetDexCache()->GetDexFile();
      int line_number = dex_file.GetLineNumFromPC(handler_method_, handler_dex_pc_);
      LOG(INFO) << "Handler: " << PrettyMethod(handler_method_) << " (line: " << line_number << ")";
    }
  }
  if (clear_exception_) {
    // Exception was cleared as part of delivery.
    DCHECK(!self_->IsExceptionPending());
  } else {
    // Put exception back in root set with clear throw location.
    self_->SetException(ThrowLocation(), exception_ref.Get());
    self_->SetExceptionReportedToInstrumentation(is_exception_reported);
  }
  // The debugger may suspend this thread and walk its stack. Let's do this before popping
  // instrumentation frames.
  if (!is_exception_reported) {
    instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
    instrumentation->ExceptionCaughtEvent(self_, throw_location, handler_method_, handler_dex_pc_,
                                          exception_ref.Get());
      // We're not catching this exception but let's remind we already reported the exception above
      // to avoid reporting it twice.
      self_->SetExceptionReportedToInstrumentation(true);
  }
  bool caught_exception = (handler_method_ != nullptr && handler_dex_pc_ != DexFile::kDexNoIndex);
  if (caught_exception) {
    // We're catching this exception so we finish reporting it. We do it here to avoid doing it
    // in the compiled code.
    self_->SetExceptionReportedToInstrumentation(false);
  }
}

// Prepares deoptimization.
class DeoptimizeStackVisitor FINAL : public StackVisitor {
 public:
  DeoptimizeStackVisitor(Thread* self, Context* context, QuickExceptionHandler* exception_handler)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : StackVisitor(self, context), self_(self), exception_handler_(exception_handler),
        prev_shadow_frame_(nullptr) {
    CHECK(!self_->HasDeoptimizationShadowFrame());
  }

  bool VisitFrame() OVERRIDE SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    exception_handler_->SetHandlerFrameDepth(GetFrameDepth());
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

 private:
  static VRegKind GetVRegKind(uint16_t reg, const std::vector<int32_t>& kinds) {
    return static_cast<VRegKind>(kinds.at(reg * 2));
  }

  bool HandleDeoptimization(mirror::ArtMethod* m) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    const DexFile::CodeItem* code_item = m->GetCodeItem();
    CHECK(code_item != nullptr);
    uint16_t num_regs = code_item->registers_size_;
    uint32_t dex_pc = GetDexPc();
    const Instruction* inst = Instruction::At(code_item->insns_ + dex_pc);
    uint32_t new_dex_pc = dex_pc + inst->SizeInCodeUnits();
    ShadowFrame* new_frame = ShadowFrame::Create(num_regs, nullptr, m, new_dex_pc);
    StackHandleScope<2> hs(self_);
    mirror::Class* declaring_class = m->GetDeclaringClass();
    Handle<mirror::DexCache> h_dex_cache(hs.NewHandle(declaring_class->GetDexCache()));
    Handle<mirror::ClassLoader> h_class_loader(hs.NewHandle(declaring_class->GetClassLoader()));
    verifier::MethodVerifier verifier(h_dex_cache->GetDexFile(), &h_dex_cache, &h_class_loader,
                                      &m->GetClassDef(), code_item, m->GetDexMethodIndex(), m,
                                      m->GetAccessFlags(), false, true, true);
    verifier.Verify();
    const std::vector<int32_t> kinds(verifier.DescribeVRegs(dex_pc));
    for (uint16_t reg = 0; reg < num_regs; ++reg) {
      VRegKind kind = GetVRegKind(reg, kinds);
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
        case kLongLoVReg:
          if (GetVRegKind(reg + 1, kinds), kLongHiVReg) {
            // Treat it as a "long" register pair.
            new_frame->SetVRegLong(reg, GetVRegPair(m, reg, kLongLoVReg, kLongHiVReg));
          } else {
            new_frame->SetVReg(reg, GetVReg(m, reg, kind));
          }
          break;
        case kLongHiVReg:
          if (GetVRegKind(reg - 1, kinds), kLongLoVReg) {
            // Nothing to do: we treated it as a "long" register pair.
          } else {
            new_frame->SetVReg(reg, GetVReg(m, reg, kind));
          }
          break;
        case kDoubleLoVReg:
          if (GetVRegKind(reg + 1, kinds), kDoubleHiVReg) {
            // Treat it as a "double" register pair.
            new_frame->SetVRegLong(reg, GetVRegPair(m, reg, kDoubleLoVReg, kDoubleHiVReg));
          } else {
            new_frame->SetVReg(reg, GetVReg(m, reg, kind));
          }
          break;
        case kDoubleHiVReg:
          if (GetVRegKind(reg - 1, kinds), kDoubleLoVReg) {
            // Nothing to do: we treated it as a "double" register pair.
          } else {
            new_frame->SetVReg(reg, GetVReg(m, reg, kind));
          }
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

  Thread* const self_;
  QuickExceptionHandler* const exception_handler_;
  ShadowFrame* prev_shadow_frame_;

  DISALLOW_COPY_AND_ASSIGN(DeoptimizeStackVisitor);
};

void QuickExceptionHandler::DeoptimizeStack() {
  DCHECK(is_deoptimization_);
  if (kDebugExceptionDelivery) {
    self_->DumpStack(LOG(INFO) << "Deoptimizing: ");
  }

  DeoptimizeStackVisitor visitor(self_, context_, this);
  visitor.WalkStack(true);

  // Restore deoptimization exception
  self_->SetException(ThrowLocation(), Thread::GetDeoptimizationException());
}

// Unwinds all instrumentation stack frame prior to catch handler or upcall.
class InstrumentationStackVisitor : public StackVisitor {
 public:
  InstrumentationStackVisitor(Thread* self, bool is_deoptimization, size_t frame_depth)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : StackVisitor(self, nullptr),
        self_(self), frame_depth_(frame_depth),
        instrumentation_frames_to_pop_(0) {
    CHECK_NE(frame_depth_, kInvalidFrameDepth);
  }

  bool VisitFrame() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    size_t current_frame_depth = GetFrameDepth();
    if (current_frame_depth < frame_depth_) {
      CHECK(GetMethod() != nullptr);
      if (UNLIKELY(GetQuickInstrumentationExitPc() == GetReturnPc())) {
        ++instrumentation_frames_to_pop_;
      }
      return true;
    } else {
      // We reached the frame of the catch handler or the upcall.
      return false;
    }
  }

  size_t GetInstrumentationFramesToPop() const {
    return instrumentation_frames_to_pop_;
  }

 private:
  Thread* const self_;
  const size_t frame_depth_;
  size_t instrumentation_frames_to_pop_;

  DISALLOW_COPY_AND_ASSIGN(InstrumentationStackVisitor);
};

void QuickExceptionHandler::UpdateInstrumentationStack() {
  if (method_tracing_active_) {
    InstrumentationStackVisitor visitor(self_, is_deoptimization_, handler_frame_depth_);
    visitor.WalkStack(true);

    size_t instrumentation_frames_to_pop = visitor.GetInstrumentationFramesToPop();
    instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
    for (size_t i = 0; i < instrumentation_frames_to_pop; ++i) {
      instrumentation->PopMethodForUnwind(self_, is_deoptimization_);
    }
  }
}

void QuickExceptionHandler::DoLongJump() {
  // Place context back on thread so it will be available when we continue.
  self_->ReleaseLongJumpContext(context_);
  context_->SetSP(reinterpret_cast<uintptr_t>(handler_quick_frame_));
  CHECK_NE(handler_quick_frame_pc_, 0u);
  context_->SetPC(handler_quick_frame_pc_);
  context_->SmashCallerSaves();
  context_->DoLongJump();
}

}  // namespace art
