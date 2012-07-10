/*
 * Copyright (C) 2011 The Android Open Source Project
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

#ifndef ART_SRC_MARK_STACK_H_
#define ART_SRC_MARK_STACK_H_

#include "UniquePtr.h"
#include "logging.h"
#include "macros.h"
#include "mem_map.h"

namespace art {

class Object;

class MarkStack {
 public:
  static MarkStack* Create();

  ~MarkStack();

  void Push(const Object* obj) {
    DCHECK(obj != NULL);
    DCHECK_NE(ptr_, limit_);
    *ptr_ = obj;
    ++ptr_;
  }

  const Object* Pop() {
    DCHECK_NE(ptr_, begin_);
    --ptr_;
    DCHECK(*ptr_ != NULL);
    return *ptr_;
  }

  bool IsEmpty() const {
    return ptr_ == begin_;
  }

  void Reset();

 private:
  MarkStack() : begin_(NULL), limit_(NULL), ptr_(NULL) {}

  void Init();

  // Memory mapping of the mark stack.
  UniquePtr<MemMap> mem_map_;

  // Base of the mark stack.
  const Object* const* begin_;

  // Exclusive limit of the mark stack.
  const Object* const* limit_;

  // Pointer to the top of the mark stack.
  const Object**  ptr_;

  DISALLOW_COPY_AND_ASSIGN(MarkStack);
};

}  // namespace art

#endif  // ART_SRC_MARK_STACK_H_
