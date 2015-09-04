/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "art_method-inl.h"
#include "jni.h"
#include "scoped_thread_state_change.h"
#include "stack.h"
#include "thread.h"

namespace art {

namespace {

extern "C" JNIEXPORT jobject JNICALL Java_Main_cloneResolvedMethods(JNIEnv* env,
                                                                    jclass,
                                                                    jclass cls) {
  ScopedObjectAccess soa(Thread::Current());
  mirror::DexCache* dex_cache = soa.Decode<mirror::Class*>(cls)->GetDexCache();
  size_t num_methods = dex_cache->NumResolvedMethods();
  ArtMethod** methods = dex_cache->GetResolvedMethods();
  CHECK_EQ(num_methods != 0u, methods != nullptr);
  if (num_methods == 0u) {
    return nullptr;
  }
  jarray array;
  if (sizeof(void*) == 4) {
    array = env->NewIntArray(num_methods);
  } else {
    array = env->NewLongArray(num_methods);
  }
  CHECK(array != nullptr);
  mirror::PointerArray* pointer_array = soa.Decode<mirror::PointerArray*>(array);
  for (size_t i = 0; i != num_methods; ++i) {
    ArtMethod* method = mirror::DexCache::GetElementPtrSize(methods, i, sizeof(void*));
    pointer_array->SetElementPtrSize(i, method, sizeof(void*));
  }
  return array;
}

extern "C" JNIEXPORT void JNICALL Java_Main_restoreResolvedMethods(
    JNIEnv*, jclass, jclass cls, jobject old_cache) {
  ScopedObjectAccess soa(Thread::Current());
  mirror::DexCache* dex_cache = soa.Decode<mirror::Class*>(cls)->GetDexCache();
  size_t num_methods = dex_cache->NumResolvedMethods();
  ArtMethod** methods = soa.Decode<mirror::Class*>(cls)->GetDexCache()->GetResolvedMethods();
  CHECK_EQ(num_methods != 0u, methods != nullptr);
  mirror::PointerArray* old = soa.Decode<mirror::PointerArray*>(old_cache);
  CHECK_EQ(methods != nullptr, old != nullptr);
  CHECK_EQ(num_methods, static_cast<size_t>(old->GetLength()));
  for (size_t i = 0; i != num_methods; ++i) {
    ArtMethod* method = old->GetElementPtrSize<ArtMethod*>(i, sizeof(void*));
    mirror::DexCache::SetElementPtrSize(methods, i, method, sizeof(void*));
  }
}

}  // namespace

}  // namespace art
