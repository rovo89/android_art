// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_JNI_INTERNAL_H_
#define ART_SRC_JNI_INTERNAL_H_

#include "jni.h"

#include "assembler.h"
#include "macros.h"

namespace art {

class Thread;

JNIEnv* CreateJNIEnv();

struct JNIEnvExt {
  const struct JNINativeInterface* fns;  // Must be first.

  Thread* self;

  // Are we in a "critical" JNI call?
  bool critical;

  // Used to help call synchronized native methods.
  // TODO: make jni_compiler.cc do the indirection itself.
  void (*MonitorEnterHelper)(JNIEnv*, jobject);
  void (*MonitorExitHelper)(JNIEnv*, jobject);
};

class JniInvokeInterface {
 public:
  static struct JNIInvokeInterface* GetInterface() {
    return &invoke_interface_;
  }
 private:
  static jint DestroyJavaVM(JavaVM* vm);
  static jint AttachCurrentThread(JavaVM* vm, JNIEnv** penv, void* thr_args);
  static jint DetachCurrentThread(JavaVM* vm);
  static jint GetEnv(JavaVM* vm, void** penv, int version);
  static jint AttachCurrentThreadAsDaemon(JavaVM* vm,
                                          JNIEnv** penv,
                                          void* thr_args);
  static struct JNIInvokeInterface invoke_interface_;
  DISALLOW_IMPLICIT_CONSTRUCTORS(JniInvokeInterface);
};

}  // namespace art

#endif  // ART_SRC_JNI_INTERNAL_H_
