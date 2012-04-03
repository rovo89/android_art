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
struct NthCallerVisitor : public Thread::StackVisitor {
  NthCallerVisitor(size_t n) : n(n), count(0), declaring_class(NULL), class_loader(NULL) {}
  bool VisitFrame(const Frame& f, uintptr_t) {
    DCHECK(class_loader == NULL);
    if (count++ == n) {
      Method* m = f.GetMethod();
      declaring_class = m->GetDeclaringClass();
      class_loader = declaring_class->GetClassLoader();
      return false;
    }
    return true;
  }
  size_t n;
  size_t count;
  Class* declaring_class;
  Object* class_loader;
};

}  // namespace art

#endif  // ART_SRC_NTH_CALLER_VISITOR_H_
