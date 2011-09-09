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

jobject Throwable_nativeFillInStackTrace(JNIEnv* env, jclass) {
  UNIMPLEMENTED(WARNING);
  //Object* stackState = dvmFillInStackTrace(dvmThreadSelf());
  //return addLocalReference(env, stackState);
  return NULL;
}

jobjectArray Throwable_nativeGetStackTrace(JNIEnv* env, jclass, jobject javaStackState) {
  UNIMPLEMENTED(WARNING);
  //Object* stackState = dvmDecodeIndirectRef(env, javaStackState);
  //if (stackState == NULL) {
    //LOGW("getStackTrace() called but no trace available");
    //return NULL;   /* could throw NPE; currently caller will do so */
  //}
  //ArrayObject* elements = dvmGetStackTrace(stackState);
  //return reinterpret_cast<jobjectArray>(addLocalReference(env, elements));
  return NULL;
}

JNINativeMethod gMethods[] = {
  NATIVE_METHOD(Throwable, nativeFillInStackTrace, "()Ljava/lang/Object;"),
  NATIVE_METHOD(Throwable, nativeGetStackTrace, "(Ljava/lang/Object;)[Ljava/lang/StackTraceElement;"),
};

}  // namespace

void register_java_lang_Throwable(JNIEnv* env) {
  jniRegisterNativeMethods(env, "java/lang/Throwable", gMethods, NELEM(gMethods));
}

}  // namespace art
