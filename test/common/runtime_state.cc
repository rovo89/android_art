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

#include "jni.h"

#include "base/logging.h"
#include "dex_file-inl.h"
#include "mirror/class-inl.h"
#include "nth_caller_visitor.h"
#include "runtime.h"
#include "scoped_thread_state_change.h"
#include "stack.h"
#include "thread-inl.h"

namespace art {

// public static native boolean hasOatFile();

extern "C" JNIEXPORT jboolean JNICALL Java_Main_hasOatFile(JNIEnv* env, jclass cls) {
  ScopedObjectAccess soa(env);

  mirror::Class* klass = soa.Decode<mirror::Class*>(cls);
  const DexFile& dex_file = klass->GetDexFile();
  const OatFile::OatDexFile* oat_dex_file = dex_file.GetOatDexFile();
  return (oat_dex_file != nullptr) ? JNI_TRUE : JNI_FALSE;
}

// public static native boolean runtimeIsSoftFail();

extern "C" JNIEXPORT jboolean JNICALL Java_Main_runtimeIsSoftFail(JNIEnv* env ATTRIBUTE_UNUSED,
                                                                  jclass cls ATTRIBUTE_UNUSED) {
  return Runtime::Current()->IsVerificationSoftFail() ? JNI_TRUE : JNI_FALSE;
}

// public static native boolean isDex2OatEnabled();

extern "C" JNIEXPORT jboolean JNICALL Java_Main_isDex2OatEnabled(JNIEnv* env ATTRIBUTE_UNUSED,
                                                                 jclass cls ATTRIBUTE_UNUSED) {
  return Runtime::Current()->IsDex2OatEnabled();
}

// public static native boolean hasImage();

extern "C" JNIEXPORT jboolean JNICALL Java_Main_hasImage(JNIEnv* env ATTRIBUTE_UNUSED,
                                                         jclass cls ATTRIBUTE_UNUSED) {
  return Runtime::Current()->GetHeap()->HasImageSpace();
}

// public static native boolean isImageDex2OatEnabled();

extern "C" JNIEXPORT jboolean JNICALL Java_Main_isImageDex2OatEnabled(JNIEnv* env ATTRIBUTE_UNUSED,
                                                                      jclass cls ATTRIBUTE_UNUSED) {
  return Runtime::Current()->IsImageDex2OatEnabled();
}

}  // namespace art
