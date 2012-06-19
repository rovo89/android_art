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

#include "jni_internal.h"

namespace art {

// Entry/exit processing for all JNI calls.
//
// This performs the necessary thread state switching, lets us amortize the
// cost of working out the current thread, and lets us check (and repair) apps
// that are using a JNIEnv on the wrong thread.
class ScopedJniThreadState {
 public:
  explicit ScopedJniThreadState(JNIEnv* env)
      : env_(reinterpret_cast<JNIEnvExt*>(env)) {
    self_ = ThreadForEnv(env);
    old_thread_state_ = self_->SetState(kRunnable);
    self_->VerifyStack();
  }

  ~ScopedJniThreadState() {
    self_->SetState(old_thread_state_);
  }

  JNIEnvExt* Env() {
    return env_;
  }

  Thread* Self() {
    return self_;
  }

  JavaVMExt* Vm() {
    return env_->vm;
  }

 private:
  static Thread* ThreadForEnv(JNIEnv* env) {
    JNIEnvExt* full_env(reinterpret_cast<JNIEnvExt*>(env));
    bool work_around_app_jni_bugs = full_env->vm->work_around_app_jni_bugs;
    Thread* env_self = full_env->self;
    Thread* self = work_around_app_jni_bugs ? Thread::Current() : env_self;
    if (!work_around_app_jni_bugs && self != env_self) {
      // TODO: pass through function name so we can use it here instead of NULL...
      JniAbortF(NULL, "JNIEnv for %s used on %s",
                ToStr<Thread>(*env_self).c_str(), ToStr<Thread>(*self).c_str());
    }
    return self;
  }

  JNIEnvExt* env_;
  Thread* self_;
  ThreadState old_thread_state_;
  DISALLOW_COPY_AND_ASSIGN(ScopedJniThreadState);
};

}  // namespace art
