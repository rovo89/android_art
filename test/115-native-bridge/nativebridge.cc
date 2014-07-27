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
#include "string.h"
#include "unistd.h"

#include "native_bridge.h"


// Native bridge interfaces...

struct NativeBridgeArtCallbacks {
  const char* (*getMethodShorty)(JNIEnv* env, jmethodID mid);
  int (*getNativeMethodCount)(JNIEnv* env, jclass clazz);
  int (*getNativeMethods)(JNIEnv* env, jclass clazz, JNINativeMethod* methods,
       uint32_t method_count);
};

struct NativeBridgeCallbacks {
  bool (*initialize)(NativeBridgeArtCallbacks* art_cbs);
  void* (*loadLibrary)(const char* libpath, int flag);
  void* (*getTrampoline)(void* handle, const char* name, const char* shorty, uint32_t len);
  bool (*isSupported)(const char* libpath);
};



static std::vector<void*> symbols;

// NativeBridgeCallbacks implementations
extern "C" bool native_bridge_initialize(NativeBridgeArtCallbacks* art_cbs) {
  printf("Native bridge initialized.\n");
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
  printf("Getting trampoline.\n");

  // The name here is actually the JNI name, so we can directly do the lookup.
  void* sym = dlsym(handle, name);
  if (sym != nullptr) {
    symbols.push_back(sym);
  }

  // As libarttest is the same arch as the host, we can actually directly use the code and do not
  // need to create a trampoline. :-)
  return sym;
}

extern "C" bool native_bridge_isSupported(const char* libpath) {
  printf("Checking for support.\n");

  if (libpath == nullptr) {
    return false;
  }
  // We don't want to hijack javacore. So we should get libarttest...
  return strcmp(libpath, "libjavacore.so") != 0;
}

NativeBridgeCallbacks NativeBridgeItf {
  .initialize = &native_bridge_initialize,
  .loadLibrary = &native_bridge_loadLibrary,
  .getTrampoline = &native_bridge_getTrampoline,
  .isSupported = &native_bridge_isSupported
};



