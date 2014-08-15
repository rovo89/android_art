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

#include "native_bridge_art_interface.h"

#include "mirror/art_method-inl.h"
#include "mirror/class-inl.h"
#include "scoped_thread_state_change.h"

namespace art {

const char* GetMethodShorty(JNIEnv* env, jmethodID mid) {
  ScopedObjectAccess soa(env);
  StackHandleScope<1> scope(soa.Self());
  mirror::ArtMethod* m = soa.DecodeMethod(mid);
  MethodHelper mh(scope.NewHandle(m));
  return mh.GetShorty();
}

uint32_t GetNativeMethodCount(JNIEnv* env, jclass clazz) {
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

uint32_t GetNativeMethods(JNIEnv* env, jclass clazz, JNINativeMethod* methods,
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

};  // namespace art
