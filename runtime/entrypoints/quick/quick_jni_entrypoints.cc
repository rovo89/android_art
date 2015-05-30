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

#include "art_method-inl.h"
#include "entrypoints/entrypoint_utils-inl.h"
#include "mirror/object-inl.h"
#include "thread-inl.h"
#include "verify_object-inl.h"

namespace art {

extern void ReadBarrierJni(mirror::CompressedReference<mirror::Object>* handle_on_stack,
                           Thread* self ATTRIBUTE_UNUSED) {
  // Call the read barrier and update the handle.
  mirror::Object* to_ref = ReadBarrier::BarrierForRoot(handle_on_stack);
  handle_on_stack->Assign(to_ref);
}

// Called on entry to JNI, transition out of Runnable and release share of mutator_lock_.
extern uint32_t JniMethodStart(Thread* self) {
  JNIEnvExt* env = self->GetJniEnv();
  DCHECK(env != nullptr);
  uint32_t saved_local_ref_cookie = env->local_ref_cookie;
  env->local_ref_cookie = env->locals.GetSegmentState();
  ArtMethod* native_method = *self->GetManagedStack()->GetTopQuickFrame();
  if (!native_method->IsFastNative()) {
    // When not fast JNI we transition out of runnable.
    self->TransitionFromRunnableToSuspended(kNative);
  }
  return saved_local_ref_cookie;
}

extern uint32_t JniMethodStartSynchronized(jobject to_lock, Thread* self) {
  self->DecodeJObject(to_lock)->MonitorEnter(self);
  return JniMethodStart(self);
}

// TODO: NO_THREAD_SAFETY_ANALYSIS due to different control paths depending on fast JNI.
static void GoToRunnable(Thread* self) NO_THREAD_SAFETY_ANALYSIS {
  ArtMethod* native_method = *self->GetManagedStack()->GetTopQuickFrame();
  bool is_fast = native_method->IsFastNative();
  if (!is_fast) {
    self->TransitionFromSuspendedToRunnable();
  } else if (UNLIKELY(self->TestAllFlags())) {
    // In fast JNI mode we never transitioned out of runnable. Perform a suspend check if there
    // is a flag raised.
    DCHECK(Locks::mutator_lock_->IsSharedHeld(self));
    self->CheckSuspend();
  }
}

static void PopLocalReferences(uint32_t saved_local_ref_cookie, Thread* self)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  JNIEnvExt* env = self->GetJniEnv();
  env->locals.SetSegmentState(env->local_ref_cookie);
  env->local_ref_cookie = saved_local_ref_cookie;
  self->PopHandleScope();
}

extern void JniMethodEnd(uint32_t saved_local_ref_cookie, Thread* self) {
  GoToRunnable(self);
  PopLocalReferences(saved_local_ref_cookie, self);
}

extern void JniMethodEndSynchronized(uint32_t saved_local_ref_cookie, jobject locked,
                                     Thread* self) {
  GoToRunnable(self);
  UnlockJniSynchronizedMethod(locked, self);  // Must decode before pop.
  PopLocalReferences(saved_local_ref_cookie, self);
}

// Common result handling for EndWithReference.
static mirror::Object* JniMethodEndWithReferenceHandleResult(jobject result,
                                                             uint32_t saved_local_ref_cookie,
                                                             Thread* self)
    NO_THREAD_SAFETY_ANALYSIS {
  // Must decode before pop. The 'result' may not be valid in case of an exception, though.
  mirror::Object* o = self->IsExceptionPending() ? nullptr : self->DecodeJObject(result);
  PopLocalReferences(saved_local_ref_cookie, self);
  // Process result.
  if (UNLIKELY(self->GetJniEnv()->check_jni)) {
    CheckReferenceResult(o, self);
  }
  VerifyObject(o);
  return o;
}

extern mirror::Object* JniMethodEndWithReference(jobject result, uint32_t saved_local_ref_cookie,
                                                 Thread* self) {
  GoToRunnable(self);
  return JniMethodEndWithReferenceHandleResult(result, saved_local_ref_cookie, self);
}

extern mirror::Object* JniMethodEndWithReferenceSynchronized(jobject result,
                                                             uint32_t saved_local_ref_cookie,
                                                             jobject locked, Thread* self) {
  GoToRunnable(self);
  UnlockJniSynchronizedMethod(locked, self);
  return JniMethodEndWithReferenceHandleResult(result, saved_local_ref_cookie, self);
}

}  // namespace art
