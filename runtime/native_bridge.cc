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

#include "native_bridge.h"

#include <dlfcn.h>
#include <stdio.h>
#include "jni.h"

#include "base/mutex.h"
#include "mirror/art_method-inl.h"
#include "mirror/class-inl.h"
#include "scoped_thread_state_change.h"
#include "ScopedLocalRef.h"
#include "thread.h"

#ifdef HAVE_ANDROID_OS
#include "cutils/properties.h"
#endif


namespace art {

// Is native-bridge support enabled?
static constexpr bool kNativeBridgeEnabled = true;

// Default library name for native-bridge.
static constexpr const char* kDefaultNativeBridge = "libnativebridge.so";

#ifdef HAVE_ANDROID_OS
// TODO: This will be removed once we have native-bridge command-line arguments.

// Property that defines the library name of native-bridge.
static constexpr const char* kPropNativeBridge = "persist.native.bridge";

// Property that enables native-bridge.
static constexpr const char* kPropEnableNativeBridge = "persist.enable.native.bridge";
#endif

// The symbol name exposed by native-bridge with the type of NativeBridgeCallbacks.
static constexpr const char* kNativeBridgeInterfaceSymbol = "NativeBridgeItf";

// The library name we are supposed to load.
static std::string native_bridge_library_string = "";

// Whether a native bridge is available (loaded and ready).
static bool available = false;
// Whether we have already initialized (or tried to).
static bool initialized = false;

struct NativeBridgeCallbacks;
static NativeBridgeCallbacks* callbacks = nullptr;

// ART interfaces to native-bridge.
struct NativeBridgeArtCallbacks {
  // Get shorty of a Java method. The shorty is supposed to be persistent in memory.
  //
  // Parameters:
  //   env [IN] pointer to JNIenv.
  //   mid [IN] Java methodID.
  // Returns:
  //   short descriptor for method.
  const char* (*getMethodShorty)(JNIEnv* env, jmethodID mid);

  // Get number of native methods for specified class.
  //
  // Parameters:
  //   env [IN] pointer to JNIenv.
  //   clazz [IN] Java class object.
  // Returns:
  //   number of native methods.
  uint32_t (*getNativeMethodCount)(JNIEnv* env, jclass clazz);

  // Get at most 'method_count' native methods for specified class 'clazz'. Results are outputed
  // via 'methods' [OUT]. The signature pointer in JNINativeMethod is reused as the method shorty.
  //
  // Parameters:
  //   env [IN] pointer to JNIenv.
  //   clazz [IN] Java class object.
  //   methods [OUT] array of method with the name, shorty, and fnPtr.
  //   method_count [IN] max number of elements in methods.
  // Returns:
  //   number of method it actually wrote to methods.
  uint32_t (*getNativeMethods)(JNIEnv* env, jclass clazz, JNINativeMethod* methods,
                               uint32_t method_count);
};

// Native-bridge interfaces to ART
struct NativeBridgeCallbacks {
  // Initialize native-bridge. Native-bridge's internal implementation must ensure MT safety and
  // that the native-bridge is initialized only once. Thus it is OK to call this interface for an
  // already initialized native-bridge.
  //
  // Parameters:
  //   art_cbs [IN] the pointer to NativeBridgeArtCallbacks.
  // Returns:
  //   true iff initialization was successful.
  bool (*initialize)(NativeBridgeArtCallbacks* art_cbs);

  // Load a shared library that is supported by the native-bridge.
  //
  // Parameters:
  //   libpath [IN] path to the shared library
  //   flag [IN] the stardard RTLD_XXX defined in bionic dlfcn.h
  // Returns:
  //   The opaque handle of the shared library if sucessful, otherwise NULL
  void* (*loadLibrary)(const char* libpath, int flag);

  // Get a native-bridge trampoline for specified native method. The trampoline has same
  // sigature as the native method.
  //
  // Parameters:
  //   handle [IN] the handle returned from loadLibrary
  //   shorty [IN] short descriptor of native method
  //   len [IN] length of shorty
  // Returns:
  //   address of trampoline if successful, otherwise NULL
  void* (*getTrampoline)(void* handle, const char* name, const char* shorty, uint32_t len);

  // Check whether native library is valid and is for an ABI that is supported by native-bridge.
  //
  // Parameters:
  //   libpath [IN] path to the shared library
  // Returns:
  //   TRUE if library is supported by native-bridge, FALSE otherwise
  bool (*isSupported)(const char* libpath);
};

static const char* GetMethodShorty(JNIEnv* env, jmethodID mid) {
  ScopedObjectAccess soa(env);
  StackHandleScope<1> scope(soa.Self());
  mirror::ArtMethod* m = soa.DecodeMethod(mid);
  MethodHelper mh(scope.NewHandle(m));
  return mh.GetShorty();
}

static uint32_t GetNativeMethodCount(JNIEnv* env, jclass clazz) {
  if (clazz == nullptr)
    return 0;

  ScopedObjectAccess soa(env);
  mirror::Class* c = soa.Decode<mirror::Class*>(clazz);

  uint32_t native_method_count = 0;
  for (uint32_t i = 0; i < c->NumDirectMethods(); ++i) {
    mirror::ArtMethod* m = c->GetDirectMethod(i);
    if (m->IsNative()) {
      native_method_count++;
    }
  }
  for (uint32_t i = 0; i < c->NumVirtualMethods(); ++i) {
    mirror::ArtMethod* m = c->GetVirtualMethod(i);
    if (m->IsNative()) {
      native_method_count++;
    }
  }
  return native_method_count;
}

static uint32_t GetNativeMethods(JNIEnv* env, jclass clazz, JNINativeMethod* methods,
                               uint32_t method_count) {
  if ((clazz == nullptr) || (methods == nullptr)) {
    return 0;
  }
  ScopedObjectAccess soa(env);
  mirror::Class* c = soa.Decode<mirror::Class*>(clazz);

  uint32_t count = 0;
  for (uint32_t i = 0; i < c->NumDirectMethods(); ++i) {
    mirror::ArtMethod* m = c->GetDirectMethod(i);
    if (m->IsNative()) {
      if (count < method_count) {
        methods[count].name = m->GetName();
        methods[count].signature = m->GetShorty();
        methods[count].fnPtr = const_cast<void*>(m->GetNativeMethod());
        count++;
      } else {
        LOG(WARNING) << "Output native method array too small. Skipping " << PrettyMethod(m);
      }
    }
  }
  for (uint32_t i = 0; i < c->NumVirtualMethods(); ++i) {
    mirror::ArtMethod* m = c->GetVirtualMethod(i);
    if (m->IsNative()) {
      if (count < method_count) {
        methods[count].name = m->GetName();
        methods[count].signature = m->GetShorty();
        methods[count].fnPtr = const_cast<void*>(m->GetNativeMethod());
        count++;
      } else {
        LOG(WARNING) << "Output native method array too small. Skipping " << PrettyMethod(m);
      }
    }
  }
  return count;
}

NativeBridgeArtCallbacks NativeBridgeArtItf = {
  GetMethodShorty,
  GetNativeMethodCount,
  GetNativeMethods
};

void SetNativeBridgeLibraryString(const std::string& nb_library_string) {
  native_bridge_library_string = nb_library_string;
  // TODO: when given an empty string, set initialized_ to true and available_ to false. This
  //       change is dependent on the property removal in Initialize().
}

bool NativeBridgeInitialize() {
  if (!kNativeBridgeEnabled) {
    return false;
  }
  // TODO: Missing annotalysis static lock ordering of DEFAULT_MUTEX_ACQUIRED, place lock into
  // global order or remove.
  static Mutex lock("native bridge lock");
  MutexLock mu(Thread::Current(), lock);

  if (initialized) {
    // Somebody did it before.
    return available;
  }

  available = false;

  const char* libnb_path;

  if (!native_bridge_library_string.empty()) {
    libnb_path = native_bridge_library_string.c_str();
  } else {
    // TODO: Remove this once the frameworks side is completely implemented.

    libnb_path = kDefaultNativeBridge;
#ifdef HAVE_ANDROID_OS
    char prop_buf[PROP_VALUE_MAX];
    property_get(kPropEnableNativeBridge, prop_buf, "false");
    if (strcmp(prop_buf, "true") != 0) {
      initialized = true;
      return false;
    }

    // If prop persist.native.bridge set, overwrite the default name.
    int name_len = property_get(kPropNativeBridge, prop_buf, kDefaultNativeBridge);
    if (name_len > 0)
      libnb_path = prop_buf;
#endif
  }

  void* handle = dlopen(libnb_path, RTLD_LAZY);
  if (handle != nullptr) {
    callbacks = reinterpret_cast<NativeBridgeCallbacks*>(dlsym(handle,
                                                               kNativeBridgeInterfaceSymbol));

    if (callbacks != nullptr) {
      available = callbacks->initialize(&NativeBridgeArtItf);
    }

    if (!available) {
      dlclose(handle);
    }
  }

  initialized = true;

  return available;
}

void* NativeBridgeLoadLibrary(const char* libpath, int flag) {
  if (NativeBridgeInitialize()) {
    return callbacks->loadLibrary(libpath, flag);
  }
  return nullptr;
}

void* NativeBridgeGetTrampoline(void* handle, const char* name, const char* shorty,
                                  uint32_t len) {
  if (NativeBridgeInitialize()) {
    return callbacks->getTrampoline(handle, name, shorty, len);
  }
  return nullptr;
}

bool NativeBridgeIsSupported(const char* libpath) {
  if (NativeBridgeInitialize()) {
    return callbacks->isSupported(libpath);
  }
  return false;
}

};  // namespace art
