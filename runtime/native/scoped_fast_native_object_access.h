/*
 * Copyright (C) 2013 The Android Open Source Project
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

#ifndef ART_RUNTIME_NATIVE_SCOPED_FAST_NATIVE_OBJECT_ACCESS_H_
#define ART_RUNTIME_NATIVE_SCOPED_FAST_NATIVE_OBJECT_ACCESS_H_

#include "base/casts.h"
#include "jni_internal.h"
#include "thread-inl.h"
#include "mirror/art_method.h"

namespace art {

// Variant of ScopedObjectAccess that does no runnable transitions. Should only be used by "fast"
// JNI methods.
class ScopedFastNativeObjectAccess {
 public:
  explicit ScopedFastNativeObjectAccess(JNIEnv* env)
    LOCKS_EXCLUDED(Locks::thread_suspend_count_lock_)
    SHARED_LOCK_FUNCTION(Locks::mutator_lock_) ALWAYS_INLINE
     : env_(down_cast<JNIEnvExt*>(env)), self_(ThreadForEnv(env)) {
    Locks::mutator_lock_->AssertSharedHeld(Self());
    DCHECK((*Self()->GetManagedStack()->GetTopQuickFrame())->IsFastNative());
    // Don't work with raw objects in non-runnable states.
    DCHECK_EQ(Self()->GetState(), kRunnable);
  }

  ~ScopedFastNativeObjectAccess() UNLOCK_FUNCTION(Locks::mutator_lock_) ALWAYS_INLINE {
  }

  Thread* Self() const {
    return self_;
  }

  JNIEnvExt* Env() const {
    return env_;
  }

  template<typename T>
  T Decode(jobject obj) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    Locks::mutator_lock_->AssertSharedHeld(Self());
    // Don't work with raw objects in non-runnable states.
    DCHECK_EQ(Self()->GetState(), kRunnable);
    return down_cast<T>(Self()->DecodeJObject(obj));
  }

  mirror::ArtField* DecodeField(jfieldID fid) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    Locks::mutator_lock_->AssertSharedHeld(Self());
    // Don't work with raw objects in non-runnable states.
    DCHECK_EQ(Self()->GetState(), kRunnable);
#ifdef MOVING_GARBAGE_COLLECTOR
    // TODO: we should make these unique weak globals if Field instances can ever move.
    UNIMPLEMENTED(WARNING);
#endif
    return reinterpret_cast<mirror::ArtField*>(fid);
  }

  /*
   * Variant of ScopedObjectAccessUnched::AddLocalReference that without JNI work arounds
   * or check JNI that should be being used by fast native methods.
   */
  template<typename T>
  T AddLocalReference(mirror::Object* obj) const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    Locks::mutator_lock_->AssertSharedHeld(Self());
    // Don't work with raw objects in non-runnable states.
    DCHECK_EQ(Self()->GetState(), kRunnable);
    if (obj == NULL) {
      return NULL;
    }

    DCHECK_NE((reinterpret_cast<uintptr_t>(obj) & 0xffff0000), 0xebad0000);

    IndirectReferenceTable& locals = Env()->locals;

    uint32_t cookie = Env()->local_ref_cookie;
    IndirectRef ref = locals.Add(cookie, obj);

    return reinterpret_cast<T>(ref);
  }

 private:
  JNIEnvExt* const env_;
  Thread* const self_;
};

}  // namespace art

#endif  // ART_RUNTIME_NATIVE_SCOPED_FAST_NATIVE_OBJECT_ACCESS_H_
