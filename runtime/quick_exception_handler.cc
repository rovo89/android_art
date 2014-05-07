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

#include "catch_block_stack_visitor.h"
#include "deoptimize_stack_visitor.h"
#include "entrypoints/entrypoint_utils.h"
#include "mirror/art_method-inl.h"
#include "handle_scope-inl.h"

namespace art {

QuickExceptionHandler::QuickExceptionHandler(Thread* self, bool is_deoptimization)
  : self_(self), context_(self->GetLongJumpContext()), is_deoptimization_(is_deoptimization),
    method_tracing_active_(is_deoptimization ||
                           Runtime::Current()->GetInstrumentation()->AreExitStubsInstalled()),
    handler_quick_frame_(nullptr), handler_quick_frame_pc_(0), handler_dex_pc_(0),
    clear_exception_(false), handler_frame_id_(kInvalidFrameId) {
}

void QuickExceptionHandler::FindCatch(const ThrowLocation& throw_location,
                                      mirror::Throwable* exception) {
  DCHECK(!is_deoptimization_);
  StackHandleScope<1> hs(self_);
  Handle<mirror::Throwable> exception_ref(hs.NewHandle(exception));

  // Walk the stack to find catch handler or prepare for deoptimization.
  CatchBlockStackVisitor visitor(self_, context_, &exception_ref, this);
  visitor.WalkStack(true);

  mirror::ArtMethod* catch_method = *handler_quick_frame_;
  if (kDebugExceptionDelivery) {
    if (catch_method == nullptr) {
      LOG(INFO) << "Handler is upcall";
    } else {
      const DexFile& dex_file = *catch_method->GetDeclaringClass()->GetDexCache()->GetDexFile();
      int line_number = dex_file.GetLineNumFromPC(catch_method, handler_dex_pc_);
      LOG(INFO) << "Handler: " << PrettyMethod(catch_method) << " (line: " << line_number << ")";
    }
  }
  if (clear_exception_) {
    // Exception was cleared as part of delivery.
    DCHECK(!self_->IsExceptionPending());
  } else {
    // Put exception back in root set with clear throw location.
    self_->SetException(ThrowLocation(), exception_ref.Get());
  }
  // The debugger may suspend this thread and walk its stack. Let's do this before popping
  // instrumentation frames.
  instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
  instrumentation->ExceptionCaughtEvent(self_, throw_location, catch_method, handler_dex_pc_,
                                        exception_ref.Get());
}

void QuickExceptionHandler::DeoptimizeStack() {
  DCHECK(is_deoptimization_);

  DeoptimizeStackVisitor visitor(self_, context_, this);
  visitor.WalkStack(true);

  // Restore deoptimization exception
  self_->SetException(ThrowLocation(), Thread::GetDeoptimizationException());
}

// Unwinds all instrumentation stack frame prior to catch handler or upcall.
class InstrumentationStackVisitor : public StackVisitor {
 public:
  InstrumentationStackVisitor(Thread* self, bool is_deoptimization, size_t frame_id)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : StackVisitor(self, nullptr),
        self_(self), frame_id_(frame_id),
        instrumentation_frames_to_pop_(0) {
    CHECK_NE(frame_id_, kInvalidFrameId);
  }

  bool VisitFrame() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    size_t current_frame_id = GetFrameId();
    if (current_frame_id > frame_id_) {
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
  const size_t frame_id_;
  size_t instrumentation_frames_to_pop_;

  DISALLOW_COPY_AND_ASSIGN(InstrumentationStackVisitor);
};

void QuickExceptionHandler::UpdateInstrumentationStack() {
  if (method_tracing_active_) {
    InstrumentationStackVisitor visitor(self_, is_deoptimization_, handler_frame_id_);
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
