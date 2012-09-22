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

#ifndef ART_SRC_NTH_CALLER_VISITOR_H_
#define ART_SRC_NTH_CALLER_VISITOR_H_

#include "object.h"
#include "thread.h"

namespace art {

// Walks up the stack 'n' callers, when used with Thread::WalkStack.
struct NthCallerVisitor : public StackVisitor {
  NthCallerVisitor(const ManagedStack* stack, const std::vector<TraceStackFrame>* trace_stack, size_t n)
      : StackVisitor(stack, trace_stack, NULL), n(n), count(0), caller(NULL) {}

  bool VisitFrame() {
    DCHECK(caller == NULL);
    if (count++ == n) {
      caller = GetMethod();
      return false;
    }
    return true;
  }

  size_t n;
  size_t count;
  AbstractMethod* caller;
};

}  // namespace art

#endif  // ART_SRC_NTH_CALLER_VISITOR_H_
