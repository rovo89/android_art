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

#ifndef ART_RUNTIME_HANDLE_H_
#define ART_RUNTIME_HANDLE_H_

#include "base/casts.h"
#include "base/logging.h"
#include "base/macros.h"
#include "stack.h"

namespace art {

class Thread;

template<class T>
class Handle {
 public:
  Handle() : reference_(nullptr) {
  }
  Handle(const Handle<T>& handle) ALWAYS_INLINE : reference_(handle.reference_) {
  }
  Handle<T>& operator=(const Handle<T>& handle) ALWAYS_INLINE {
    reference_ = handle.reference_;
    return *this;
  }
  explicit Handle(StackReference<T>* reference) ALWAYS_INLINE : reference_(reference) {
  }
  T& operator*() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) ALWAYS_INLINE {
    return *Get();
  }
  T* operator->() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) ALWAYS_INLINE {
    return Get();
  }
  T* Get() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) ALWAYS_INLINE {
    return reference_->AsMirrorPtr();
  }
  T* Assign(T* reference) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) ALWAYS_INLINE {
    T* old = reference_->AsMirrorPtr();
    reference_->Assign(reference);
    return old;
  }
  jobject ToJObject() const ALWAYS_INLINE {
    return reinterpret_cast<jobject>(reference_);
  }

 private:
  StackReference<T>* reference_;

  template<typename S>
  explicit Handle(StackReference<S>* reference)
      : reference_(reinterpret_cast<StackReference<T>*>(reference)) {
  }

  template<typename S>
  explicit Handle(const Handle<S>& handle)
      : reference_(reinterpret_cast<StackReference<T>*>(handle.reference_)) {
  }

  template<class S> friend class Handle;
  friend class HandleScope;
  template<class S> friend class HandleWrapper;
  template<size_t kNumReferences> friend class StackHandleScope;
};

}  // namespace art

#endif  // ART_RUNTIME_HANDLE_H_
