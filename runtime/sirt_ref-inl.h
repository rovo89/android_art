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

#ifndef ART_RUNTIME_SIRT_REF_INL_H_
#define ART_RUNTIME_SIRT_REF_INL_H_

#include "sirt_ref.h"

#include "verify_object-inl.h"

namespace art {

template<class T> inline SirtRef<T>::SirtRef(Thread* self, T* object) : self_(self), sirt_(object) {
  VerifyObject(object);
  self_->PushSirt(&sirt_);
}

template<class T> inline SirtRef<T>::~SirtRef() {
  StackIndirectReferenceTable* top_sirt = self_->PopSirt();
  DCHECK_EQ(top_sirt, &sirt_);
}

template<class T> inline T* SirtRef<T>::reset(T* object) {
  VerifyObject(object);
  T* old_ref = get();
  sirt_.SetReference(0, object);
  return old_ref;
}

}  // namespace art

#endif  // ART_RUNTIME_SIRT_REF_INL_H_
