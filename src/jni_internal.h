// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_JNI_INTERNAL_H_
#define ART_SRC_JNI_INTERNAL_H_

#include "jni.h"

#include "assembler.h"
#include "macros.h"

namespace art {

class Runtime;
class Thread;

JavaVM* CreateJavaVM(Runtime* runtime);
JNIEnv* CreateJNIEnv();

struct JavaVMExt {
  // Must be first to correspond with JNIEnv.
  const struct JNIInvokeInterface* fns;

  Runtime* runtime;
};

struct JNIEnvExt {
  // Must be first to correspond with JavaVM.
  const struct JNINativeInterface* fns;

  Thread* self;

  // Are we in a "critical" JNI call?
  bool critical;

  // Used to help call synchronized native methods.
  // TODO: make jni_compiler.cc do the indirection itself.
  void (*MonitorEnterHelper)(JNIEnv*, jobject);
  void (*MonitorExitHelper)(JNIEnv*, jobject);
};

}  // namespace art

#endif  // ART_SRC_JNI_INTERNAL_H_
