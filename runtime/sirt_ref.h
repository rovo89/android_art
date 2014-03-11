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

#ifndef ART_RUNTIME_SIRT_REF_H_
#define ART_RUNTIME_SIRT_REF_H_

#include "base/casts.h"
#include "base/logging.h"
#include "base/macros.h"
#include "stack_indirect_reference_table.h"
#include "thread.h"

namespace art {

template<class T>
class SirtRef {
 public:
  SirtRef(Thread* self, T* object) : self_(self), sirt_(object) {
    self_->PushSirt(&sirt_);
  }
  ~SirtRef() {
    StackIndirectReferenceTable* top_sirt = self_->PopSirt();
    DCHECK_EQ(top_sirt, &sirt_);
  }

  T& operator*() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return *get();
  }
  T* operator->() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return get();
  }
  T* get() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return down_cast<T*>(sirt_.GetReference(0));
  }

  // Returns the old reference.
  T* reset(T* object = nullptr) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    T* old_ref = get();
    sirt_.SetReference(0, object);
    return old_ref;
  }

 private:
  Thread* const self_;
  StackIndirectReferenceTable sirt_;

  DISALLOW_COPY_AND_ASSIGN(SirtRef);
};

}  // namespace art

#endif  // ART_RUNTIME_SIRT_REF_H_
