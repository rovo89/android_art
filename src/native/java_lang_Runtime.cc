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

#include <limits.h>
#include <unistd.h>

#include "heap.h"
#include "jni_internal.h"
#include "object.h"
#include "runtime.h"
#include "ScopedUtfChars.h"

namespace art {

static void Runtime_gc(JNIEnv*, jclass) {
  ScopedThreadStateChange tsc(Thread::Current(), kRunnable);
  Runtime::Current()->GetHeap()->CollectGarbage(false);
}

static void Runtime_nativeExit(JNIEnv*, jclass, jint status) {
  Runtime::Current()->CallExitHook(status);
  exit(status);
}

/*
 * static String nativeLoad(String filename, ClassLoader loader)
 *
 * Load the specified full path as a dynamic library filled with
 * JNI-compatible methods. Returns null on success, or a failure
 * message on failure.
 */
static jstring Runtime_nativeLoad(JNIEnv* env, jclass, jstring javaFilename, jobject javaLoader) {
  ScopedUtfChars filename(env, javaFilename);
  if (filename.c_str() == NULL) {
    return NULL;
  }

  ClassLoader* classLoader = Decode<ClassLoader*>(env, javaLoader);
  std::string detail;
  JavaVMExt* vm = Runtime::Current()->GetJavaVM();
  bool success = vm->LoadNativeLibrary(filename.c_str(), classLoader, detail);
  if (success) {
    return NULL;
  }

  return env->NewStringUTF(detail.c_str());
}

static jlong Runtime_maxMemory(JNIEnv*, jclass) {
  return Runtime::Current()->GetHeap()->GetMaxMemory();
}

static jlong Runtime_totalMemory(JNIEnv*, jclass) {
  return Runtime::Current()->GetHeap()->GetTotalMemory();
}

static jlong Runtime_freeMemory(JNIEnv*, jclass) {
  return Runtime::Current()->GetHeap()->GetFreeMemory();
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(Runtime, freeMemory, "()J"),
  NATIVE_METHOD(Runtime, gc, "()V"),
  NATIVE_METHOD(Runtime, maxMemory, "()J"),
  NATIVE_METHOD(Runtime, nativeExit, "(I)V"),
  NATIVE_METHOD(Runtime, nativeLoad, "(Ljava/lang/String;Ljava/lang/ClassLoader;)Ljava/lang/String;"),
  NATIVE_METHOD(Runtime, totalMemory, "()J"),
};

void register_java_lang_Runtime(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("java/lang/Runtime");
}

}  // namespace art
