// Copyright 2011 Google Inc. All Rights Reserved.

#include "src/jni_internal.h"
#include "src/logging.h"

namespace art {

static void JniMonitorEnter(JniEnvironment*, jobject) {
  LOG(WARNING) << "Unimplemented: JNI Monitor Enter";
}

static void JniMonitorExit(JniEnvironment*, jobject) {
  LOG(WARNING) << "Unimplemented: JNI Monitor Exit";
}

JniEnvironment::JniEnvironment() {
  monitor_enter_ = &JniMonitorEnter;
  monitor_exit_ = &JniMonitorExit;
}

}  // namespace art
