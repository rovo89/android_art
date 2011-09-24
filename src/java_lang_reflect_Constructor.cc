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
#include "class_linker.h"
#include "object.h"
#include "reflection.h"

#include "JniConstants.h" // Last to avoid problems with LOG redefinition.

namespace art {

namespace {

/*
 * We get here through Constructor.newInstance().  The Constructor object
 * would not be available if the constructor weren't public (per the
 * definition of Class.getConstructor), so we can skip the method access
 * check.  We can also safely assume the constructor isn't associated
 * with an interface, array, or primitive class.
 */
jobject Constructor_constructNative(JNIEnv* env, jobject javaMethod, jobjectArray javaArgs, jclass javaDeclaringClass, jobjectArray javaParams, jint, jboolean) {
  Class* c = Decode<Class*>(env, javaDeclaringClass);
  if (c->IsAbstract()) {
    Thread::Current()->ThrowNewException("Ljava/lang/InstantiationException;",
        "Can't instantiate abstract class %s", PrettyDescriptor(c->GetDescriptor()).c_str());
    return NULL;
  }

  Object* receiver = c->AllocObject();
  if (receiver == NULL) {
    return NULL;
  }

  jobject javaReceiver = AddLocalReference<jobject>(env, receiver);
  InvokeMethod(env, javaMethod, javaReceiver, javaArgs, javaParams);

  // Constructors are ()V methods, so we shouldn't touch the result of InvokeMethod.
  return javaReceiver;
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(Constructor, constructNative, "([Ljava/lang/Object;Ljava/lang/Class;[Ljava/lang/Class;IZ)Ljava/lang/Object;"),
};

}  // namespace

void register_java_lang_reflect_Constructor(JNIEnv* env) {
  jniRegisterNativeMethods(env, "java/lang/reflect/Constructor", gMethods, NELEM(gMethods));
}

}  // namespace art
