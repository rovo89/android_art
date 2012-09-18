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
#include "class_loader.h"
#include "jni_internal.h"
#include "object.h"
#include "scoped_thread_state_change.h"

namespace art {

static jclass Proxy_generateProxy(JNIEnv* env, jclass, jstring javaName, jobjectArray javaInterfaces, jobject javaLoader, jobjectArray javaMethods, jobjectArray javaThrows) {
  ScopedObjectAccess soa(env);
  String* name = soa.Decode<String*>(javaName);
  ObjectArray<Class>* interfaces = soa.Decode<ObjectArray<Class>*>(javaInterfaces);
  ClassLoader* loader = soa.Decode<ClassLoader*>(javaLoader);
  ObjectArray<AbstractMethod>* methods = soa.Decode<ObjectArray<AbstractMethod>*>(javaMethods);
  ObjectArray<ObjectArray<Class> >* throws = soa.Decode<ObjectArray<ObjectArray<Class> >*>(javaThrows);
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Class* result = class_linker->CreateProxyClass(name, interfaces, loader, methods, throws);
  return soa.AddLocalReference<jclass>(result);
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(Proxy, generateProxy, "(Ljava/lang/String;[Ljava/lang/Class;Ljava/lang/ClassLoader;[Ljava/lang/reflect/Method;[[Ljava/lang/Class;)Ljava/lang/Class;"),
};

void register_java_lang_reflect_Proxy(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("java/lang/reflect/Proxy");
}

}  // namespace art
