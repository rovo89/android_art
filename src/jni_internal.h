// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_JNI_INTERNAL_H_
#define ART_SRC_JNI_INTERNAL_H_

#include "jni.h"

#include "assembler.h"
#include "macros.h"

namespace art {

// TODO: This is a place holder for a true JNIEnv used to provide limited
// functionality for the JNI compiler
class JniEnvironment {
 public:
  explicit JniEnvironment();

  static Offset MonitorEnterOffset() {
    return Offset(OFFSETOF_MEMBER(JniEnvironment, monitor_enter_));
  }

  static Offset MonitorExitOffset() {
    return Offset(OFFSETOF_MEMBER(JniEnvironment, monitor_exit_));
  }

 private:
  void (*monitor_enter_)(JniEnvironment*, jobject);
  void (*monitor_exit_)(JniEnvironment*, jobject);

  DISALLOW_COPY_AND_ASSIGN(JniEnvironment);
};

class JniInvoke {
 public:
  // Index 3
  int DestroyJavaVM(JavaVM* vm);

  // Index 4
  int AttachCurrentThread(JavaVM* vm, JNIEnv** penv, void* thr_args);

  // Index 5
  int DetachCurrentThread(JavaVM* vm);

  // Index 6
  int GetEnv(JavaVM* vm, void** penv, int version);

  // Index 7
  int AttachCurrentThreadAsDaemon(JavaVM* vm, JNIEnv** penv, void* thr_args);

 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(JniInvoke);
};

}  // namespace art
#endif  // ART_SRC_JNI_INTERNAL_H_
