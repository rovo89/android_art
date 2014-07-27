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

#ifndef ART_RUNTIME_NATIVE_BRIDGE_H_
#define ART_RUNTIME_NATIVE_BRIDGE_H_

#include "base/mutex.h"

#include <string>

namespace art {

struct NativeBridgeCallbacks;

class NativeBridge {
 public:
  // Initialize the native bridge, if any. Should be called by Runtime::Init(). An empty string
  // signals that we do not want to load a native bridge.
  static void SetNativeBridgeLibraryString(std::string& native_bridge_library_string);

  // Load a shared library that is supported by the native-bridge.
  static void* LoadLibrary(const char* libpath, int flag);
  // Get a native-bridge trampoline for specified native method.
  static void* GetTrampoline(void* handle, const char* name, const char* shorty, uint32_t len);
  // True if native library is valid and is for an ABI that is supported by native-bridge.
  static bool IsSupported(const char* libpath);

 private:
  static bool Initialize();

  // The library name we are supposed to load.
  static std::string native_bridge_library_string_;

  // Whether we have already initialized (or tried to).
  static bool initialized_ GUARDED_BY(lock_);
  static Mutex lock_;

  // Whether a native bridge is available (loaded and ready).
  static bool available_;

  static NativeBridgeCallbacks* callbacks_;
};

};  // namespace art

#endif  // ART_RUNTIME_NATIVE_BRIDGE_H_
