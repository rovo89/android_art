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

#ifndef ART_RUNTIME_QUICK_EXCEPTION_HANDLER_H_
#define ART_RUNTIME_QUICK_EXCEPTION_HANDLER_H_

#include "base/logging.h"
#include "base/mutex.h"

namespace art {

namespace mirror {
class ArtMethod;
class Throwable;
}  // namespace mirror
class Context;
class Thread;
class ThrowLocation;
class ShadowFrame;

static constexpr bool kDebugExceptionDelivery = false;
static constexpr size_t kInvalidFrameId = 0xffffffff;

// Manages exception delivery for Quick backend. Not used by Portable backend.
class QuickExceptionHandler {
 public:
  QuickExceptionHandler(Thread* self, bool is_deoptimization)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  ~QuickExceptionHandler() {
    LOG(FATAL) << "UNREACHABLE";  // Expected to take long jump.
  }

  void FindCatch(const ThrowLocation& throw_location, mirror::Throwable* exception)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void DeoptimizeStack() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void UpdateInstrumentationStack() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void DoLongJump() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void SetHandlerQuickFrame(mirror::ArtMethod** handler_quick_frame) {
    handler_quick_frame_ = handler_quick_frame;
  }

  void SetHandlerQuickFramePc(uintptr_t handler_quick_frame_pc) {
    handler_quick_frame_pc_ = handler_quick_frame_pc;
  }

  void SetHandlerDexPc(uint32_t dex_pc) {
    handler_dex_pc_ = dex_pc;
  }

  void SetClearException(bool clear_exception) {
    clear_exception_ = clear_exception;
  }

  void SetHandlerFrameId(size_t frame_id) {
    handler_frame_id_ = frame_id;
  }

 private:
  Thread* const self_;
  Context* const context_;
  const bool is_deoptimization_;
  // Is method tracing active?
  const bool method_tracing_active_;
  // Quick frame with found handler or last frame if no handler found.
  mirror::ArtMethod** handler_quick_frame_;
  // PC to branch to for the handler.
  uintptr_t handler_quick_frame_pc_;
  // Associated dex PC.
  uint32_t handler_dex_pc_;
  // Should the exception be cleared as the catch block has no move-exception?
  bool clear_exception_;
  // Frame id of the catch handler or the upcall.
  size_t handler_frame_id_;

  DISALLOW_COPY_AND_ASSIGN(QuickExceptionHandler);
};

}  // namespace art
#endif  // ART_RUNTIME_QUICK_EXCEPTION_HANDLER_H_
