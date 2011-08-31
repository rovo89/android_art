// Copyright 2011 Google Inc. All Rights Reserved.
// Author: enh@google.com (Elliott Hughes)

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
    old_thread_state_ = self_->SetState(Thread::kRunnable);
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
    Thread* env_self = full_env->self;
    Thread* self = full_env->work_around_app_jni_bugs ? Thread::Current() : env_self;
    if (self != env_self) {
      LOG(ERROR) << "JNI ERROR: JNIEnv for " << *env_self
                 << " used on " << *self;
      // TODO: dump stack
    }
    return self;
  }

  JNIEnvExt* env_;
  Thread* self_;
  Thread::State old_thread_state_;
  DISALLOW_COPY_AND_ASSIGN(ScopedJniThreadState);
};

}  // namespace art
