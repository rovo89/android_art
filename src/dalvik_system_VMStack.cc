/*
 * Copyright (C) 2008 The Android Open Source Project
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
#include "object.h"

#include "JniConstants.h" // Last to avoid problems with LOG redefinition.

namespace art {

namespace {

jint VMStack_fillStackTraceElements(JNIEnv* env, jclass, jobject targetThread, jobjectArray javaSteArray) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

jobject VMStack_getCallingClassLoader(JNIEnv* env, jclass) {
  UNIMPLEMENTED(WARNING);
  return NULL;
}

jobjectArray VMStack_getClasses(JNIEnv* env, jclass, jint maxDepth) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jclass VMStack_getStackClass2(JNIEnv* env, jclass) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jobjectArray VMStack_getThreadStackTrace(JNIEnv* env, jclass, jobject targetThread) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(VMStack, fillStackTraceElements, "(Ljava/lang/Thread;[Ljava/lang/StackTraceElement;)I"),
  NATIVE_METHOD(VMStack, getCallingClassLoader, "()Ljava/lang/ClassLoader;"),
  NATIVE_METHOD(VMStack, getClasses, "(I)[Ljava/lang/Class;"),
  NATIVE_METHOD(VMStack, getStackClass2, "()Ljava/lang/Class;"),
  NATIVE_METHOD(VMStack, getThreadStackTrace, "(Ljava/lang/Thread;)[Ljava/lang/StackTraceElement;"),
};

}  // namespace

void register_dalvik_system_VMStack(JNIEnv* env) {
  jniRegisterNativeMethods(env, "dalvik/system/VMStack", gMethods, NELEM(gMethods));
}

}  // namespace art
