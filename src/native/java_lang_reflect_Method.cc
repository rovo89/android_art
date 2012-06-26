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

#include "class_linker.h"
#include "jni_internal.h"
#include "object.h"
#include "object_utils.h"
#include "reflection.h"
#include "scoped_jni_thread_state.h"

namespace art {

static jobject Method_invoke(JNIEnv* env, jobject javaMethod, jobject javaReceiver, jobject javaArgs) {
  ScopedJniThreadState ts(env);
  return InvokeMethod(ts, javaMethod, javaReceiver, javaArgs);
}

static jobject Method_getExceptionTypesNative(JNIEnv* env, jobject javaMethod) {
  ScopedJniThreadState ts(env);
  Method* proxy_method = ts.Decode<Object*>(javaMethod)->AsMethod();
  CHECK(proxy_method->GetDeclaringClass()->IsProxyClass());
  SynthesizedProxyClass* proxy_class =
      down_cast<SynthesizedProxyClass*>(proxy_method->GetDeclaringClass());
  int throws_index = -1;
  size_t num_virt_methods = proxy_class->NumVirtualMethods();
  for (size_t i = 0; i < num_virt_methods; i++) {
    if (proxy_class->GetVirtualMethod(i) == proxy_method) {
      throws_index = i;
      break;
    }
  }
  CHECK_NE(throws_index, -1);
  ObjectArray<Class>* declared_exceptions = proxy_class->GetThrows()->Get(throws_index);
  return ts.AddLocalReference<jobject>(declared_exceptions->Clone());
}

static jobject Method_findOverriddenMethodNative(JNIEnv* env, jobject javaMethod) {
  ScopedJniThreadState ts(env);
  Method* method = ts.Decode<Object*>(javaMethod)->AsMethod();
  return ts.AddLocalReference<jobject>(method->FindOverriddenMethod());
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(Method, invoke, "(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;"),
  NATIVE_METHOD(Method, getExceptionTypesNative, "()[Ljava/lang/Class;"),
  NATIVE_METHOD(Method, findOverriddenMethodNative, "()Ljava/lang/reflect/Method;"),
};

void register_java_lang_reflect_Method(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("java/lang/reflect/Method");
}

}  // namespace art
