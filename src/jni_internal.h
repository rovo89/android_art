// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_JNI_INTERNAL_H_
#define ART_SRC_JNI_INTERNAL_H_

#include "jni.h"

#include "assembler.h"
#include "macros.h"
#include "reference_table.h"

namespace art {

class Runtime;
class Thread;

struct JavaVMExt {
  JavaVMExt(Runtime* runtime, bool check_jni);

  // Must be first to correspond with JNIEnv.
  const struct JNIInvokeInterface* fns;

  Runtime* runtime;

  bool check_jni;

  // Used to hold references to pinned primitive arrays.
  ReferenceTable pin_table;
};

struct JNIEnvExt {
  JNIEnvExt(Thread* self, bool check_jni);

  // Must be first to correspond with JavaVM.
  const struct JNINativeInterface* fns;

  Thread* self;

  bool check_jni;

  // Are we in a "critical" JNI call?
  bool critical;

  // Entered JNI monitors, for bulk exit on thread detach.
  ReferenceTable  monitor_table;
};

}  // namespace art

#endif  // ART_SRC_JNI_INTERNAL_H_
