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

jboolean AtomicLong_VMSupportsCS8(JNIEnv*, jclass) {
  return JNI_TRUE;
}

JNINativeMethod gMethods[] = {
  NATIVE_METHOD(AtomicLong, VMSupportsCS8, "()Z"),
};

}  // namespace

void register_java_util_concurrent_atomic_AtomicLong(JNIEnv* env) {
  jniRegisterNativeMethods(env, "java/util/concurrent/atomic/AtomicLong", gMethods, NELEM(gMethods));
}

}  // namespace art
