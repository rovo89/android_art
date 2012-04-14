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
#include "class_linker.h"

#include "JniConstants.h" // Last to avoid problems with LOG redefinition.

namespace art {

static jclass Proxy_generateProxy(JNIEnv* env, jclass, jstring javaName, jobjectArray javaInterfaces, jobject javaLoader, jobjectArray javaMethods, jobjectArray javaThrows) {
  // Allocates Class so transition thread state to runnable
  ScopedThreadStateChange tsc(Thread::Current(), kRunnable);
  String* name = Decode<String*>(env, javaName);
  ObjectArray<Class>* interfaces = Decode<ObjectArray<Class>*>(env, javaInterfaces);
  ClassLoader* loader = Decode<ClassLoader*>(env, javaLoader);
  ObjectArray<Method>* methods = Decode<ObjectArray<Method>*>(env, javaMethods);
  ObjectArray<ObjectArray<Class> >* throws = Decode<ObjectArray<ObjectArray<Class> >*>(env, javaThrows);
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Class* result = class_linker->CreateProxyClass(name, interfaces, loader, methods, throws);
  return AddLocalReference<jclass>(env, result);
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(Proxy, generateProxy, "(Ljava/lang/String;[Ljava/lang/Class;Ljava/lang/ClassLoader;[Ljava/lang/reflect/Method;[[Ljava/lang/Class;)Ljava/lang/Class;"),
};

void register_java_lang_reflect_Proxy(JNIEnv* env) {
  jniRegisterNativeMethods(env, "java/lang/reflect/Proxy", gMethods, NELEM(gMethods));
}

}  // namespace art
