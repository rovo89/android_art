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
#include "jit/jit.h"
#include "jit/jit_code_cache.h"
#include "mirror/class-inl.h"
#include "nth_caller_visitor.h"
#include "oat_quick_method_header.h"
#include "runtime.h"
#include "scoped_thread_state_change.h"
#include "ScopedUtfChars.h"
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
  return Runtime::Current()->GetHeap()->HasBootImageSpace();
}

// public static native boolean isImageDex2OatEnabled();

extern "C" JNIEXPORT jboolean JNICALL Java_Main_isImageDex2OatEnabled(JNIEnv* env ATTRIBUTE_UNUSED,
                                                                      jclass cls ATTRIBUTE_UNUSED) {
  return Runtime::Current()->IsImageDex2OatEnabled();
}

// public static native boolean compiledWithOptimizing();
// Did we use the optimizing compiler to compile this?

extern "C" JNIEXPORT jboolean JNICALL Java_Main_compiledWithOptimizing(JNIEnv* env, jclass cls) {
  ScopedObjectAccess soa(env);

  mirror::Class* klass = soa.Decode<mirror::Class*>(cls);
  const DexFile& dex_file = klass->GetDexFile();
  const OatFile::OatDexFile* oat_dex_file = dex_file.GetOatDexFile();
  if (oat_dex_file == nullptr) {
    // Could be JIT, which also uses optimizing, but conservatively say no.
    return JNI_FALSE;
  }
  const OatFile* oat_file = oat_dex_file->GetOatFile();
  CHECK(oat_file != nullptr);

  const char* cmd_line = oat_file->GetOatHeader().GetStoreValueByKey(OatHeader::kDex2OatCmdLineKey);
  CHECK(cmd_line != nullptr);  // Huh? This should not happen.

  // Check the backend.
  constexpr const char* kCompilerBackend = "--compiler-backend=";
  const char* backend = strstr(cmd_line, kCompilerBackend);
  if (backend != nullptr) {
    // If it's set, make sure it's optimizing.
    backend += strlen(kCompilerBackend);
    if (strncmp(backend, "Optimizing", strlen("Optimizing")) != 0) {
      return JNI_FALSE;
    }
  }

  // Check the filter.
  constexpr const char* kCompilerFilter = "--compiler-filter=";
  const char* filter = strstr(cmd_line, kCompilerFilter);
  if (filter != nullptr) {
    // If it's set, make sure it's not interpret-only|verify-none|verify-at-runtime.
    // Note: The space filter might have an impact on the test, but ignore that for now.
    filter += strlen(kCompilerFilter);
    constexpr const char* kInterpretOnly = "interpret-only";
    constexpr const char* kVerifyNone = "verify-none";
    constexpr const char* kVerifyAtRuntime = "verify-at-runtime";
    if (strncmp(filter, kInterpretOnly, strlen(kInterpretOnly)) == 0 ||
        strncmp(filter, kVerifyNone, strlen(kVerifyNone)) == 0 ||
        strncmp(filter, kVerifyAtRuntime, strlen(kVerifyAtRuntime)) == 0) {
      return JNI_FALSE;
    }
  }

  return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL Java_Main_ensureJitCompiled(JNIEnv* env,
                                                             jclass,
                                                             jclass cls,
                                                             jstring method_name) {
  jit::Jit* jit = Runtime::Current()->GetJit();
  if (jit == nullptr) {
    return;
  }

  ScopedObjectAccess soa(Thread::Current());

  ScopedUtfChars chars(env, method_name);
  CHECK(chars.c_str() != nullptr);

  mirror::Class* klass = soa.Decode<mirror::Class*>(cls);
  ArtMethod* method = klass->FindDeclaredDirectMethodByName(chars.c_str(), sizeof(void*));

  jit::JitCodeCache* code_cache = jit->GetCodeCache();
  OatQuickMethodHeader* header = nullptr;
  // Make sure there is a profiling info, required by the compiler.
  ProfilingInfo::Create(soa.Self(), method, /* retry_allocation */ true);
  while (true) {
    header = OatQuickMethodHeader::FromEntryPoint(method->GetEntryPointFromQuickCompiledCode());
    if (code_cache->ContainsPc(header->GetCode())) {
      break;
    } else {
      // Sleep to yield to the compiler thread.
      usleep(1000);
      // Will either ensure it's compiled or do the compilation itself.
      jit->CompileMethod(method, soa.Self(), /* osr */ false);
    }
  }
}

}  // namespace art
