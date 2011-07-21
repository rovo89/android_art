// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_JNI_INTERNAL_H_
#define ART_SRC_JNI_INTERNAL_H_

#include "jni.h"
#include "src/assembler.h"
#include "src/macros.h"

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
};

}  // namespace art
#endif  // ART_SRC_JNI_INTERNAL_H_
