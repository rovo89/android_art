// Copyright 2011 Google Inc. All Rights Reserved.

#include "jni_internal.h"

#include <vector>
#include <utility>

#include "logging.h"
#include "runtime.h"
#include "scoped_ptr.h"
#include "stringpiece.h"
#include "thread.h"

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

extern "C" jint JNI_CreateJavaVM(JavaVM** p_vm, void** p_env, void* vm_args) {
  const JavaVMInitArgs* args = static_cast<JavaVMInitArgs*>(vm_args);
  if (args->version < JNI_VERSION_1_2) {
    return JNI_EVERSION;
  }
  Runtime::Options options;
  for (int i = 0; i < args->nOptions; ++i) {
    JavaVMOption* option = &args->options[i];
    options.push_back(std::make_pair(StringPiece(option->optionString),
                                     option->extraInfo));
  }
  bool ignore_unrecognized = args->ignoreUnrecognized;
  scoped_ptr<Runtime> runtime(Runtime::Create(options, ignore_unrecognized));
  if (runtime == NULL) {
    return JNI_ERR;
  } else {
    *p_env = reinterpret_cast<JNIEnv*>(Thread::Current()->GetJniEnv());
    *p_vm = reinterpret_cast<JavaVM*>(runtime.release());
    return JNI_OK;
  }
}

extern "C" jint JNI_GetCreatedJavaVMs(JavaVM** vmBuf, jsize bufLen,
                                      jsize* nVMs) {
  Runtime* runtime = Runtime::Current();
  if (runtime == NULL) {
    *nVMs = 0;
  } else {
    *nVMs = 1;
    vmBuf[0] = reinterpret_cast<JavaVM*>(runtime);
  }
  return JNI_OK;
}

// Historically unsupported.
extern "C" jint JNI_GetDefaultJavaVMInitArgs(void* vm_args) {
  return JNI_ERR;
}

jint JniInvoke::DestroyJavaVM(JavaVM* vm) {
  if (vm == NULL) {
    return JNI_ERR;
  } else {
    Runtime* runtime = reinterpret_cast<Runtime*>(vm);
    delete runtime;
    return JNI_OK;
  }
}

jint JniInvoke::AttachCurrentThread(JavaVM* vm, JNIEnv** p_env,
                                    void* thr_args) {
  if (vm == NULL || p_env == NULL) {
    return JNI_ERR;
  }
  Runtime* runtime = reinterpret_cast<Runtime*>(vm);
  JniEnvironment** jni_env = reinterpret_cast<JniEnvironment**>(p_env);
  const char* name = NULL;
  if (thr_args != NULL) {
    // TODO: check version
    name = static_cast<JavaVMAttachArgs*>(thr_args)->name;
    // TODO: thread group
  }
  bool success = runtime->AttachCurrentThread(name, jni_env);
  if (!success) {
    return JNI_ERR;
  } else {
    return JNI_OK;
  }
}

jint JniInvoke::DetachCurrentThread(JavaVM* vm) {
  if (vm == NULL) {
    return JNI_ERR;
  } else {
    Runtime* runtime = reinterpret_cast<Runtime*>(vm);
    runtime->DetachCurrentThread();
    return JNI_OK;
  }
}

jint JniInvoke::GetEnv(JavaVM *vm, void **env, jint version) {
  if (version < JNI_VERSION_1_1 || version > JNI_VERSION_1_6) {
    return JNI_EVERSION;
  }
  if (vm == NULL || env == NULL) {
    return JNI_ERR;
  }
  Thread* thread = Thread::Current();
  if (thread == NULL) {
    *env = NULL;
    return JNI_EDETACHED;
  }
  *env = thread->GetJniEnv();
  return JNI_OK;
}

jint JniInvoke::AttachCurrentThreadAsDaemon(JavaVM* vm, JNIEnv** p_env,
                                            void* thr_args) {
  if (vm == NULL || p_env == NULL) {
    return JNI_ERR;
  }
  Runtime* runtime = reinterpret_cast<Runtime*>(vm);
  JniEnvironment** jni_env = reinterpret_cast<JniEnvironment**>(p_env);
  const char* name = NULL;
  if (thr_args != NULL) {
    // TODO: check version
    name = static_cast<JavaVMAttachArgs*>(thr_args)->name;
    // TODO: thread group
  }
  bool success = runtime->AttachCurrentThreadAsDaemon(name, jni_env);
  if (!success) {
    return JNI_ERR;
  } else {
    return JNI_OK;
  }
}

}  // namespace art
