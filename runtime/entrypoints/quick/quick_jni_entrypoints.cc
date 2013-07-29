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

#include "dex_file-inl.h"
#include "entrypoints/entrypoint_utils.h"
#include "mirror/class-inl.h"
#include "mirror/abstract_method-inl.h"
#include "mirror/object.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "object_utils.h"
#include "scoped_thread_state_change.h"
#include "thread.h"

namespace art {

// Called on entry to JNI, transition out of Runnable and release share of mutator_lock_.
extern uint32_t JniMethodStart(Thread* self) {
  JNIEnvExt* env = self->GetJniEnv();
  DCHECK(env != NULL);
  uint32_t saved_local_ref_cookie = env->local_ref_cookie;
  env->local_ref_cookie = env->locals.GetSegmentState();
  self->TransitionFromRunnableToSuspended(kNative);
  return saved_local_ref_cookie;
}

extern uint32_t JniMethodStartSynchronized(jobject to_lock, Thread* self) {
  self->DecodeJObject(to_lock)->MonitorEnter(self);
  return JniMethodStart(self);
}

static void PopLocalReferences(uint32_t saved_local_ref_cookie, Thread* self) {
  JNIEnvExt* env = self->GetJniEnv();
  env->locals.SetSegmentState(env->local_ref_cookie);
  env->local_ref_cookie = saved_local_ref_cookie;
  self->PopSirt();
}

extern void JniMethodEnd(uint32_t saved_local_ref_cookie, Thread* self) {
  self->TransitionFromSuspendedToRunnable();
  PopLocalReferences(saved_local_ref_cookie, self);
}


extern void JniMethodEndSynchronized(uint32_t saved_local_ref_cookie, jobject locked,
                                     Thread* self) {
  self->TransitionFromSuspendedToRunnable();
  UnlockJniSynchronizedMethod(locked, self);  // Must decode before pop.
  PopLocalReferences(saved_local_ref_cookie, self);
}

extern mirror::Object* JniMethodEndWithReference(jobject result, uint32_t saved_local_ref_cookie,
                                                 Thread* self) {
  self->TransitionFromSuspendedToRunnable();
  mirror::Object* o = self->DecodeJObject(result);  // Must decode before pop.
  PopLocalReferences(saved_local_ref_cookie, self);
  // Process result.
  if (UNLIKELY(self->GetJniEnv()->check_jni)) {
    if (self->IsExceptionPending()) {
      return NULL;
    }
    CheckReferenceResult(o, self);
  }
  return o;
}

extern mirror::Object* JniMethodEndWithReferenceSynchronized(jobject result,
                                                             uint32_t saved_local_ref_cookie,
                                                             jobject locked, Thread* self) {
  self->TransitionFromSuspendedToRunnable();
  UnlockJniSynchronizedMethod(locked, self);  // Must decode before pop.
  mirror::Object* o = self->DecodeJObject(result);
  PopLocalReferences(saved_local_ref_cookie, self);
  // Process result.
  if (UNLIKELY(self->GetJniEnv()->check_jni)) {
    if (self->IsExceptionPending()) {
      return NULL;
    }
    CheckReferenceResult(o, self);
  }
  return o;
}

static void WorkAroundJniBugsForJobject(intptr_t* arg_ptr) {
  intptr_t value = *arg_ptr;
  mirror::Object** value_as_jni_rep = reinterpret_cast<mirror::Object**>(value);
  mirror::Object* value_as_work_around_rep = value_as_jni_rep != NULL ? *value_as_jni_rep : NULL;
  CHECK(Runtime::Current()->GetHeap()->IsHeapAddress(value_as_work_around_rep))
      << value_as_work_around_rep;
  *arg_ptr = reinterpret_cast<intptr_t>(value_as_work_around_rep);
}

extern "C" const void* artWorkAroundAppJniBugs(Thread* self, intptr_t* sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DCHECK(Thread::Current() == self);
  // TODO: this code is specific to ARM
  // On entry the stack pointed by sp is:
  // | arg3   | <- Calling JNI method's frame (and extra bit for out args)
  // | LR     |
  // | R3     |    arg2
  // | R2     |    arg1
  // | R1     |    jclass/jobject
  // | R0     |    JNIEnv
  // | unused |
  // | unused |
  // | unused | <- sp
  mirror::AbstractMethod* jni_method = self->GetCurrentMethod(NULL);
  DCHECK(jni_method->IsNative()) << PrettyMethod(jni_method);
  intptr_t* arg_ptr = sp + 4;  // pointer to r1 on stack
  // Fix up this/jclass argument
  WorkAroundJniBugsForJobject(arg_ptr);
  arg_ptr++;
  // Fix up jobject arguments
  MethodHelper mh(jni_method);
  int reg_num = 2;  // Current register being processed, -1 for stack arguments.
  for (uint32_t i = 1; i < mh.GetShortyLength(); i++) {
    char shorty_char = mh.GetShorty()[i];
    if (shorty_char == 'L') {
      WorkAroundJniBugsForJobject(arg_ptr);
    }
    if (shorty_char == 'J' || shorty_char == 'D') {
      if (reg_num == 2) {
        arg_ptr = sp + 8;  // skip to out arguments
        reg_num = -1;
      } else if (reg_num == 3) {
        arg_ptr = sp + 10;  // skip to out arguments plus 2 slots as long must be aligned
        reg_num = -1;
      } else {
        DCHECK_EQ(reg_num, -1);
        if ((reinterpret_cast<intptr_t>(arg_ptr) & 7) == 4) {
          arg_ptr += 3;  // unaligned, pad and move through stack arguments
        } else {
          arg_ptr += 2;  // aligned, move through stack arguments
        }
      }
    } else {
      if (reg_num == 2) {
        arg_ptr++;  // move through register arguments
        reg_num++;
      } else if (reg_num == 3) {
        arg_ptr = sp + 8;  // skip to outgoing stack arguments
        reg_num = -1;
      } else {
        DCHECK_EQ(reg_num, -1);
        arg_ptr++;  // move through stack arguments
      }
    }
  }
  // Load expected destination, see Method::RegisterNative
  const void* code = reinterpret_cast<const void*>(jni_method->GetNativeGcMap());
  if (UNLIKELY(code == NULL)) {
    code = GetJniDlsymLookupStub();
    jni_method->RegisterNative(self, code);
  }
  return code;
}

}  // namespace art
