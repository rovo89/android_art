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

// public static native void assertCallerIsInterpreted();

extern "C" JNIEXPORT void JNICALL Java_Main_assertCallerIsInterpreted(JNIEnv* env, jclass) {
  LOG(INFO) << "assertCallerIsInterpreted";

  ScopedObjectAccess soa(env);
  NthCallerVisitor caller(soa.Self(), 1, false);
  caller.WalkStack();
  CHECK(caller.caller != nullptr);
  LOG(INFO) << PrettyMethod(caller.caller);
  CHECK(caller.GetCurrentShadowFrame() != nullptr);
}

// public static native void assertCallerIsManaged();

extern "C" JNIEXPORT void JNICALL Java_Main_assertCallerIsManaged(JNIEnv* env, jclass cls) {
  // Note: needs some smarts to not fail if there is no managed code, at all.
  LOG(INFO) << "assertCallerIsManaged";

  ScopedObjectAccess soa(env);

  mirror::Class* klass = soa.Decode<mirror::Class*>(cls);
  const DexFile& dex_file = klass->GetDexFile();
  const OatFile::OatDexFile* oat_dex_file = dex_file.GetOatDexFile();
  if (oat_dex_file == nullptr) {
    // No oat file, this must be a test configuration that doesn't compile at all. Ignore that the
    // result will be that we're running the interpreter.
    return;
  }

  NthCallerVisitor caller(soa.Self(), 1, false);
  caller.WalkStack();
  CHECK(caller.caller != nullptr);
  LOG(INFO) << PrettyMethod(caller.caller);

  if (caller.GetCurrentShadowFrame() == nullptr) {
    // Not a shadow frame, this looks good.
    return;
  }

  // This could be an interpret-only or a verify-at-runtime compilation, or a read-barrier variant,
  // or... It's not really safe to just reject now. Let's look at the access flags. If the method
  // was successfully verified, its access flags should be set to mark it preverified, except when
  // we're running soft-fail tests.
  if (Runtime::Current()->IsVerificationSoftFail()) {
    // Soft-fail config. Everything should be running with interpreter access checks, potentially.
    return;
  }
  CHECK(caller.caller->IsPreverified());
}

}  // namespace art
