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

#include <string>

#include "atomic.h"
#include "UniquePtr.h"
#include "logging.h"
#include "macros.h"
#include "mem_map.h"

namespace art {

class Object;

class MarkStack {
 public:
  // Length is in bytes.
  static MarkStack* Create(const std::string& name, size_t length);

  ~MarkStack();

  void Push(const Object* obj) {
    DCHECK(obj != NULL);
    DCHECK_NE(ptr_, limit_);
    *ptr_ = obj;
    ++ptr_;
  }

  // Beware: Atomic pushes and pops don't mix well.
  void AtomicPush(const Object* obj) {
    DCHECK(obj != NULL);
    DCHECK_NE(ptr_, limit_);
    DCHECK_EQ(sizeof(ptr_), sizeof(int32_t));
    int32_t* ptr  = reinterpret_cast<int32_t*>(&ptr_);
    *reinterpret_cast<const Object**>(android_atomic_add(sizeof(*ptr_), ptr)) = obj;
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

  size_t Size() const {
    DCHECK_GE(ptr_, begin_);
    return ptr_ - begin_;
  }

  const Object* Get(size_t index) const {
    DCHECK_LT(index, Size());
    return begin_[index];
  }

  Object** Begin() {
    return const_cast<Object**>(begin_);
  }

  void Reset();

 private:
  MarkStack(const std::string& name) : name_(name), begin_(NULL), limit_(NULL), ptr_(NULL) {}

  void Init(size_t length);

  // Name of the mark stack.
  const std::string& name_;

  // Memory mapping of the mark stack.
  UniquePtr<MemMap> mem_map_;

  // Base of the mark stack.
  const Object* const* begin_;

  // Exclusive limit of the mark stack.
  const Object* const* limit_;

  // Pointer to the top of the mark stack.
  const Object** ptr_;

  DISALLOW_COPY_AND_ASSIGN(MarkStack);
};

}  // namespace art

#endif  // ART_SRC_MARK_STACK_H_
