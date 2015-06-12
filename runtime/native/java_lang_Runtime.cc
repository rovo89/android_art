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

#include "java_lang_Runtime.h"

#include <dlfcn.h>
#include <limits.h>
#include <unistd.h>

#include "base/macros.h"
#include "gc/heap.h"
#include "handle_scope-inl.h"
#include "jni_internal.h"
#include "mirror/class_loader.h"
#include "runtime.h"
#include "scoped_thread_state_change.h"
#include "ScopedUtfChars.h"
#include "verify_object-inl.h"

#include <sstream>
#ifdef HAVE_ANDROID_OS
// This function is provided by android linker.
extern "C" void android_update_LD_LIBRARY_PATH(const char* ld_library_path);
#endif  // HAVE_ANDROID_OS

namespace art {

static void Runtime_gc(JNIEnv*, jclass) {
  if (Runtime::Current()->IsExplicitGcDisabled()) {
      LOG(INFO) << "Explicit GC skipped.";
      return;
  }
  Runtime::Current()->GetHeap()->CollectGarbage(false);
}

NO_RETURN static void Runtime_nativeExit(JNIEnv*, jclass, jint status) {
  LOG(INFO) << "System.exit called, status: " << status;
  Runtime::Current()->CallExitHook(status);
  exit(status);
}

static void SetLdLibraryPath(JNIEnv* env, jstring javaLdLibraryPathJstr, jstring javaDexPathJstr) {
#ifdef HAVE_ANDROID_OS
  std::stringstream ss;
  if (javaLdLibraryPathJstr != nullptr) {
    ScopedUtfChars javaLdLibraryPath(env, javaLdLibraryPathJstr);
    if (javaLdLibraryPath.c_str() != nullptr) {
      ss << javaLdLibraryPath.c_str();
    }
  }

  if (javaDexPathJstr != nullptr) {
    ScopedUtfChars javaDexPath(env, javaDexPathJstr);
    if (javaDexPath.c_str() != nullptr) {
      std::vector<std::string> dexPathVector;
      Split(javaDexPath.c_str(), ':', &dexPathVector);

      for (auto abi : art::Runtime::Current()->GetCpuAbilist()) {
        for (auto zip_path : dexPathVector) {
          // Native libraries live under lib/<abi>/ inside .apk file.
          ss << ":" << zip_path << "!" << "lib/" << abi;
        }
      }
    }
  }

  std::string ldLibraryPathStr = ss.str();
  const char* ldLibraryPath = ldLibraryPathStr.c_str();
  if (*ldLibraryPath == ':') {
    ++ldLibraryPath;
  }

  android_update_LD_LIBRARY_PATH(ldLibraryPath);
#else
  LOG(WARNING) << "android_update_LD_LIBRARY_PATH not found; .so dependencies will not work!";
  UNUSED(javaLdLibraryPathJstr, javaDexPathJstr, env);
#endif
}

static jstring Runtime_nativeLoad(JNIEnv* env, jclass, jstring javaFilename, jobject javaLoader,
                                  jstring javaLdLibraryPathJstr, jstring javaDexPathJstr) {
  ScopedUtfChars filename(env, javaFilename);
  if (filename.c_str() == nullptr) {
    return nullptr;
  }

  SetLdLibraryPath(env, javaLdLibraryPathJstr, javaDexPathJstr);

  std::string error_msg;
  {
    JavaVMExt* vm = Runtime::Current()->GetJavaVM();
    bool success = vm->LoadNativeLibrary(env, filename.c_str(), javaLoader, &error_msg);
    if (success) {
      return nullptr;
    }
  }

  // Don't let a pending exception from JNI_OnLoad cause a CheckJNI issue with NewStringUTF.
  env->ExceptionClear();
  return env->NewStringUTF(error_msg.c_str());
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
  NATIVE_METHOD(Runtime, freeMemory, "!()J"),
  NATIVE_METHOD(Runtime, gc, "()V"),
  NATIVE_METHOD(Runtime, maxMemory, "!()J"),
  NATIVE_METHOD(Runtime, nativeExit, "(I)V"),
  NATIVE_METHOD(Runtime, nativeLoad, "(Ljava/lang/String;Ljava/lang/ClassLoader;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"),
  NATIVE_METHOD(Runtime, totalMemory, "!()J"),
};

void register_java_lang_Runtime(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("java/lang/Runtime");
}

}  // namespace art
