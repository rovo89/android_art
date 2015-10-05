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

static bool asserts_enabled = true;

// public static native void disableStackFrameAsserts();
// Note: to globally disable asserts in unsupported configurations.

extern "C" JNIEXPORT void JNICALL Java_Main_disableStackFrameAsserts(JNIEnv* env ATTRIBUTE_UNUSED,
                                                                     jclass cls ATTRIBUTE_UNUSED) {
  asserts_enabled = false;
}


// public static native boolean isInterpreted();

extern "C" JNIEXPORT jboolean JNICALL Java_Main_isInterpreted(JNIEnv* env, jclass) {
  ScopedObjectAccess soa(env);
  NthCallerVisitor caller(soa.Self(), 1, false);
  caller.WalkStack();
  CHECK(caller.caller != nullptr);
  return caller.GetCurrentShadowFrame() != nullptr ? JNI_TRUE : JNI_FALSE;
}

// public static native void assertIsInterpreted();

extern "C" JNIEXPORT void JNICALL Java_Main_assertIsInterpreted(JNIEnv* env, jclass klass) {
  if (asserts_enabled) {
    CHECK(Java_Main_isInterpreted(env, klass));
  }
}


// public static native boolean isManaged();

extern "C" JNIEXPORT jboolean JNICALL Java_Main_isManaged(JNIEnv* env, jclass cls) {
  ScopedObjectAccess soa(env);

  mirror::Class* klass = soa.Decode<mirror::Class*>(cls);
  const DexFile& dex_file = klass->GetDexFile();
  const OatFile::OatDexFile* oat_dex_file = dex_file.GetOatDexFile();
  if (oat_dex_file == nullptr) {
    // No oat file, this must be a test configuration that doesn't compile at all. Ignore that the
    // result will be that we're running the interpreter.
    return JNI_FALSE;
  }

  NthCallerVisitor caller(soa.Self(), 1, false);
  caller.WalkStack();
  CHECK(caller.caller != nullptr);

  return caller.GetCurrentShadowFrame() != nullptr ? JNI_FALSE : JNI_TRUE;
}

// public static native void assertIsManaged();

extern "C" JNIEXPORT void JNICALL Java_Main_assertIsManaged(JNIEnv* env, jclass cls) {
  if (asserts_enabled) {
    CHECK(Java_Main_isManaged(env, cls));
  }
}

}  // namespace art
