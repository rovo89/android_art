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

namespace art {

static jobject Object_internalClone(JNIEnv* env, jobject javaThis) {
  ScopedObjectAccess soa(env);
  Object* o = soa.Decode<Object*>(javaThis);
  return soa.AddLocalReference<jobject>(o->Clone(soa.Self()));
}

static void Object_notify(JNIEnv* env, jobject javaThis) {
  ScopedObjectAccess soa(env);
  Object* o = soa.Decode<Object*>(javaThis);
  o->Notify();
}

static void Object_notifyAll(JNIEnv* env, jobject javaThis) {
  ScopedObjectAccess soa(env);
  Object* o = soa.Decode<Object*>(javaThis);
  o->NotifyAll();
}

static void Object_wait(JNIEnv* env, jobject javaThis, jlong ms, jint ns) {
  ScopedObjectAccess soa(env);
  Object* o = soa.Decode<Object*>(javaThis);
  o->Wait(ms, ns);
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(Object, internalClone, "(Ljava/lang/Cloneable;)Ljava/lang/Object;"),
  NATIVE_METHOD(Object, notify, "()V"),
  NATIVE_METHOD(Object, notifyAll, "()V"),
  NATIVE_METHOD(Object, wait, "(JI)V"),
};

void register_java_lang_Object(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("java/lang/Object");
}

}  // namespace art
