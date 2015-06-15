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

extern "C" JNIEXPORT jobject JNICALL Java_Main_cloneResolvedMethods(JNIEnv*, jclass, jclass cls) {
  ScopedObjectAccess soa(Thread::Current());
  return soa.Vm()->AddGlobalRef(
      soa.Self(),
      soa.Decode<mirror::Class*>(cls)->GetDexCache()->GetResolvedMethods()->Clone(soa.Self()));
}

extern "C" JNIEXPORT void JNICALL Java_Main_restoreResolvedMethods(
    JNIEnv*, jclass, jclass cls, jobject old_cache) {
  ScopedObjectAccess soa(Thread::Current());
  mirror::PointerArray* now = soa.Decode<mirror::Class*>(cls)->GetDexCache()->GetResolvedMethods();
  mirror::PointerArray* old = soa.Decode<mirror::PointerArray*>(old_cache);
  for (size_t i = 0, e = old->GetLength(); i < e; ++i) {
    now->SetElementPtrSize(i, old->GetElementPtrSize<void*>(i, sizeof(void*)), sizeof(void*));
  }
}

}  // namespace

}  // namespace art
