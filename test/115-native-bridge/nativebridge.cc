/*
 * Copyright (C) 2014 The Android Open Source Project
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

// A simple implementation of the native-bridge interface.

#include <algorithm>
#include <dlfcn.h>
#include <vector>

#include "jni.h"
#include "stdio.h"
#include "unistd.h"

#include "nativebridge/native_bridge.h"

struct NativeBridgeMethod {
  const char* name;
  const char* signature;
  bool static_method;
  void* fnPtr;
  void* trampoline;
};

static NativeBridgeMethod* find_native_bridge_method(const char *name);
static const android::NativeBridgeRuntimeCallbacks* gNativeBridgeArtCallbacks;

static jint trampoline_JNI_OnLoad(JavaVM* vm, void* reserved) {
  JNIEnv* env = nullptr;
  typedef jint (*FnPtr_t)(JavaVM*, void*);
  FnPtr_t fnPtr = reinterpret_cast<FnPtr_t>(find_native_bridge_method("JNI_OnLoad")->fnPtr);

  vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
  if (env == nullptr) {
    return 0;
  }

  jclass klass = env->FindClass("Main");
  if (klass != nullptr) {
    int i, count1, count2;
    count1 = gNativeBridgeArtCallbacks->getNativeMethodCount(env, klass);
    std::unique_ptr<JNINativeMethod[]> methods(new JNINativeMethod[count1]);
    if (methods == nullptr) {
      return 0;
    }
    count2 = gNativeBridgeArtCallbacks->getNativeMethods(env, klass, methods.get(), count1);
    if (count1 == count2) {
      printf("Test ART callbacks: all JNI function number is %d.\n", count1);
    }

    for (i = 0; i < count1; i++) {
      NativeBridgeMethod* nb_method = find_native_bridge_method(methods[i].name);
      if (nb_method != nullptr) {
        jmethodID mid = nullptr;
        if (nb_method->static_method) {
          mid = env->GetStaticMethodID(klass, methods[i].name, nb_method->signature);
        } else {
          mid = env->GetMethodID(klass, methods[i].name, nb_method->signature);
        }
        if (mid != nullptr) {
          const char* shorty = gNativeBridgeArtCallbacks->getMethodShorty(env, mid);
          if (strcmp(shorty, methods[i].signature) == 0) {
            printf("    name:%s, signature:%s, shorty:%s.\n",
                   methods[i].name, nb_method->signature, shorty);
          }
        }
      }
    }
    methods.release();
  }

  printf("%s called!\n", __FUNCTION__);
  return fnPtr(vm, reserved);
}

static void trampoline_Java_Main_testFindClassOnAttachedNativeThread(JNIEnv* env,
                                                                     jclass klass) {
  typedef void (*FnPtr_t)(JNIEnv*, jclass);
  FnPtr_t fnPtr = reinterpret_cast<FnPtr_t>
    (find_native_bridge_method("testFindClassOnAttachedNativeThread")->fnPtr);
  printf("%s called!\n", __FUNCTION__);
  return fnPtr(env, klass);
}

static void trampoline_Java_Main_testFindFieldOnAttachedNativeThreadNative(JNIEnv* env,
                                                                           jclass klass) {
  typedef void (*FnPtr_t)(JNIEnv*, jclass);
  FnPtr_t fnPtr = reinterpret_cast<FnPtr_t>
    (find_native_bridge_method("testFindFieldOnAttachedNativeThreadNative")->fnPtr);
  printf("%s called!\n", __FUNCTION__);
  return fnPtr(env, klass);
}

static void trampoline_Java_Main_testCallStaticVoidMethodOnSubClassNative(JNIEnv* env,
                                                                          jclass klass) {
  typedef void (*FnPtr_t)(JNIEnv*, jclass);
  FnPtr_t fnPtr = reinterpret_cast<FnPtr_t>
    (find_native_bridge_method("testCallStaticVoidMethodOnSubClassNative")->fnPtr);
  printf("%s called!\n", __FUNCTION__);
  return fnPtr(env, klass);
}

static jobject trampoline_Java_Main_testGetMirandaMethodNative(JNIEnv* env, jclass klass) {
  typedef jobject (*FnPtr_t)(JNIEnv*, jclass);
  FnPtr_t fnPtr = reinterpret_cast<FnPtr_t>
    (find_native_bridge_method("testGetMirandaMethodNative")->fnPtr);
  printf("%s called!\n", __FUNCTION__);
  return fnPtr(env, klass);
}

static void trampoline_Java_Main_testZeroLengthByteBuffers(JNIEnv* env, jclass klass) {
  typedef void (*FnPtr_t)(JNIEnv*, jclass);
  FnPtr_t fnPtr = reinterpret_cast<FnPtr_t>
    (find_native_bridge_method("testZeroLengthByteBuffers")->fnPtr);
  printf("%s called!\n", __FUNCTION__);
  return fnPtr(env, klass);
}

static jbyte trampoline_Java_Main_byteMethod(JNIEnv* env, jclass klass, jbyte b1, jbyte b2,
                                             jbyte b3, jbyte b4, jbyte b5, jbyte b6,
                                             jbyte b7, jbyte b8, jbyte b9, jbyte b10) {
  typedef jbyte (*FnPtr_t)(JNIEnv*, jclass, jbyte, jbyte, jbyte, jbyte, jbyte,
                           jbyte, jbyte, jbyte, jbyte, jbyte);
  FnPtr_t fnPtr = reinterpret_cast<FnPtr_t>(find_native_bridge_method("byteMethod")->fnPtr);
  printf("%s called!\n", __FUNCTION__);
  return fnPtr(env, klass, b1, b2, b3, b4, b5, b6, b7, b8, b9, b10);
}

static jshort trampoline_Java_Main_shortMethod(JNIEnv* env, jclass klass, jshort s1, jshort s2,
                                               jshort s3, jshort s4, jshort s5, jshort s6,
                                               jshort s7, jshort s8, jshort s9, jshort s10) {
  typedef jshort (*FnPtr_t)(JNIEnv*, jclass, jshort, jshort, jshort, jshort, jshort,
                            jshort, jshort, jshort, jshort, jshort);
  FnPtr_t fnPtr = reinterpret_cast<FnPtr_t>(find_native_bridge_method("shortMethod")->fnPtr);
  printf("%s called!\n", __FUNCTION__);
  return fnPtr(env, klass, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10);
}

static jboolean trampoline_Java_Main_booleanMethod(JNIEnv* env, jclass klass, jboolean b1,
                                                   jboolean b2, jboolean b3, jboolean b4,
                                                   jboolean b5, jboolean b6, jboolean b7,
                                                   jboolean b8, jboolean b9, jboolean b10) {
  typedef jboolean (*FnPtr_t)(JNIEnv*, jclass, jboolean, jboolean, jboolean, jboolean, jboolean,
                              jboolean, jboolean, jboolean, jboolean, jboolean);
  FnPtr_t fnPtr = reinterpret_cast<FnPtr_t>(find_native_bridge_method("booleanMethod")->fnPtr);
  printf("%s called!\n", __FUNCTION__);
  return fnPtr(env, klass, b1, b2, b3, b4, b5, b6, b7, b8, b9, b10);
}

static jchar trampoline_Java_Main_charMethod(JNIEnv* env, jclass klass, jchar c1, jchar c2,
                                             jchar c3, jchar c4, jchar c5, jchar c6,
                                             jchar c7, jchar c8, jchar c9, jchar c10) {
  typedef jchar (*FnPtr_t)(JNIEnv*, jclass, jchar, jchar, jchar, jchar, jchar,
                           jchar, jchar, jchar, jchar, jchar);
  FnPtr_t fnPtr = reinterpret_cast<FnPtr_t>(find_native_bridge_method("charMethod")->fnPtr);
  printf("%s called!\n", __FUNCTION__);
  return fnPtr(env, klass, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10);
}

NativeBridgeMethod gNativeBridgeMethods[] = {
  { "JNI_OnLoad", "", true, nullptr,
    reinterpret_cast<void*>(trampoline_JNI_OnLoad) },
  { "booleanMethod", "(ZZZZZZZZZZ)Z", true, nullptr,
    reinterpret_cast<void*>(trampoline_Java_Main_booleanMethod) },
  { "byteMethod", "(BBBBBBBBBB)B", true, nullptr,
    reinterpret_cast<void*>(trampoline_Java_Main_byteMethod) },
  { "charMethod", "(CCCCCCCCCC)C", true, nullptr,
    reinterpret_cast<void*>(trampoline_Java_Main_charMethod) },
  { "shortMethod", "(SSSSSSSSSS)S", true, nullptr,
    reinterpret_cast<void*>(trampoline_Java_Main_shortMethod) },
  { "testCallStaticVoidMethodOnSubClassNative", "()V", true, nullptr,
    reinterpret_cast<void*>(trampoline_Java_Main_testCallStaticVoidMethodOnSubClassNative) },
  { "testFindClassOnAttachedNativeThread", "()V", true, nullptr,
    reinterpret_cast<void*>(trampoline_Java_Main_testFindClassOnAttachedNativeThread) },
  { "testFindFieldOnAttachedNativeThreadNative", "()V", true, nullptr,
    reinterpret_cast<void*>(trampoline_Java_Main_testFindFieldOnAttachedNativeThreadNative) },
  { "testGetMirandaMethodNative", "()Ljava/lang/reflect/Method;", true, nullptr,
    reinterpret_cast<void*>(trampoline_Java_Main_testGetMirandaMethodNative) },
  { "testZeroLengthByteBuffers", "()V", true, nullptr,
    reinterpret_cast<void*>(trampoline_Java_Main_testZeroLengthByteBuffers) },
};

static NativeBridgeMethod* find_native_bridge_method(const char *name) {
  const char* pname = name;
  if (strncmp(name, "Java_Main_", 10) == 0) {
    pname += 10;
  }

  for (size_t i = 0; i < sizeof(gNativeBridgeMethods) / sizeof(gNativeBridgeMethods[0]); i++) {
    if (strcmp(pname, gNativeBridgeMethods[i].name) == 0) {
      return &gNativeBridgeMethods[i];
    }
  }
  return nullptr;
}

// NativeBridgeCallbacks implementations
extern "C" bool native_bridge_initialize(const android::NativeBridgeRuntimeCallbacks* art_cbs,
                                         const char* private_dir, const char* isa) {
  if (art_cbs != nullptr) {
    gNativeBridgeArtCallbacks = art_cbs;
    printf("Native bridge initialized.\n");
  }
  return true;
}

extern "C" void* native_bridge_loadLibrary(const char* libpath, int flag) {
  size_t len = strlen(libpath);
  char* tmp = new char[len + 10];
  strncpy(tmp, libpath, len);
  tmp[len - 3] = '2';
  tmp[len - 2] = '.';
  tmp[len - 1] = 's';
  tmp[len] = 'o';
  tmp[len + 1] = 0;
  void* handle = dlopen(tmp, flag);
  delete[] tmp;

  if (handle == nullptr) {
    printf("Handle = nullptr!\n");
    printf("Was looking for %s.\n", libpath);
    printf("Error = %s.\n", dlerror());
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) != nullptr) {
      printf("Current working dir: %s\n", cwd);
    }
  }
  return handle;
}

extern "C" void* native_bridge_getTrampoline(void* handle, const char* name, const char* shorty,
                                             uint32_t len) {
  printf("Getting trampoline for %s with shorty %s.\n", name, shorty);

  // The name here is actually the JNI name, so we can directly do the lookup.
  void* sym = dlsym(handle, name);
  NativeBridgeMethod* method = find_native_bridge_method(name);
  if (method == nullptr)
    return nullptr;
  method->fnPtr = sym;

  return method->trampoline;
}

extern "C" bool native_bridge_isSupported(const char* libpath) {
  printf("Checking for support.\n");

  if (libpath == nullptr) {
    return false;
  }
  // We don't want to hijack javacore. So we should get libarttest...
  return strcmp(libpath, "libjavacore.so") != 0;
}

namespace android {

// Environment values required by the apps running with native bridge.
struct NativeBridgeRuntimeValues {
  const char* os_arch;
  const char* cpu_abi;
  const char* cpu_abi2;
  const char* *supported_abis;
  int32_t abi_count;
};

}  // namespace android

const char* supported_abis[] = {
    "supported1", "supported2", "supported3"
};

const struct android::NativeBridgeRuntimeValues nb_env {
    .os_arch = "os.arch",
    .cpu_abi = "cpu_abi",
    .cpu_abi2 = "cpu_abi2",
    .supported_abis = supported_abis,
    .abi_count = 3
};

extern "C" const struct android::NativeBridgeRuntimeValues* native_bridge_getAppEnv(
    const char* abi) {
  printf("Checking for getEnvValues.\n");

  if (abi == nullptr) {
    return nullptr;
  }

  return &nb_env;
}

// "NativeBridgeItf" is effectively an API (it is the name of the symbol that will be loaded
// by the native bridge library).
android::NativeBridgeCallbacks NativeBridgeItf {
  .version = 1,
  .initialize = &native_bridge_initialize,
  .loadLibrary = &native_bridge_loadLibrary,
  .getTrampoline = &native_bridge_getTrampoline,
  .isSupported = &native_bridge_isSupported,
  .getAppEnv = &native_bridge_getAppEnv
};
