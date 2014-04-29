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

#ifndef ART_RUNTIME_DEOPTIMIZE_STACK_VISITOR_H_
#define ART_RUNTIME_DEOPTIMIZE_STACK_VISITOR_H_

#include "base/mutex.h"
#include "stack.h"
#include "thread.h"

namespace art {

namespace mirror {
class ArtMethod;
}  // namespace mirror
class QuickExceptionHandler;
class Thread;

// Prepares deoptimization.
class DeoptimizeStackVisitor FINAL : public StackVisitor {
 public:
  DeoptimizeStackVisitor(Thread* self, Context* context, QuickExceptionHandler* exception_handler)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : StackVisitor(self, context), self_(self), exception_handler_(exception_handler),
        prev_shadow_frame_(nullptr) {
    CHECK(!self_->HasDeoptimizationShadowFrame());
  }

  bool VisitFrame() OVERRIDE SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

 private:
  bool HandleDeoptimization(mirror::ArtMethod* m) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  Thread* const self_;
  QuickExceptionHandler* const exception_handler_;
  ShadowFrame* prev_shadow_frame_;

  DISALLOW_COPY_AND_ASSIGN(DeoptimizeStackVisitor);
};

}  // namespace art
#endif  // ART_RUNTIME_DEOPTIMIZE_STACK_VISITOR_H_
