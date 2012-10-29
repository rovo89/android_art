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
#include "scoped_thread_state_change.h"
#include "ScopedLocalRef.h"

namespace art {

static jint String_compareTo(JNIEnv* env, jobject javaThis, jobject javaRhs) {
  if (UNLIKELY(javaRhs == NULL)) {
    ScopedLocalRef<jclass> npe(env, env->FindClass("java/lang/NullPointerException"));
    env->ThrowNew(npe.get(), "rhs == null");
    return -1;
  } else {
    ScopedObjectAccess soa(env);
    return soa.Decode<String*>(javaThis)->CompareTo(soa.Decode<String*>(javaRhs));
  }
}

static jint String_fastIndexOf(JNIEnv* env, jobject java_this, jint ch, jint start) {
  ScopedObjectAccess soa(env);
  // This method does not handle supplementary characters. They're dealt with in managed code.
  DCHECK_LE(ch, 0xffff);

  String* s = soa.Decode<String*>(java_this);
  return s->FastIndexOf(ch, start);
}

static jstring String_intern(JNIEnv* env, jobject javaThis) {
  ScopedObjectAccess soa(env);
  String* s = soa.Decode<String*>(javaThis);
  String* result = s->Intern();
  return soa.AddLocalReference<jstring>(result);
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(String, compareTo, "(Ljava/lang/String;)I"),
  NATIVE_METHOD(String, fastIndexOf, "(II)I"),
  NATIVE_METHOD(String, intern, "()Ljava/lang/String;"),
};

void register_java_lang_String(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("java/lang/String");
}

}  // namespace art
