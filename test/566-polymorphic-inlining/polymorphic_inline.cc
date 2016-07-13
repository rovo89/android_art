/*
 * Copyright (C) 2016 The Android Open Source Project
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

#include "art_method.h"
#include "jit/jit.h"
#include "jit/jit_code_cache.h"
#include "oat_quick_method_header.h"
#include "scoped_thread_state_change.h"
#include "stack_map.h"

namespace art {

static void do_checks(jclass cls, const char* method_name) {
  ScopedObjectAccess soa(Thread::Current());
  mirror::Class* klass = soa.Decode<mirror::Class*>(cls);
  jit::Jit* jit = Runtime::Current()->GetJit();
  jit::JitCodeCache* code_cache = jit->GetCodeCache();
  ArtMethod* method = klass->FindDeclaredDirectMethodByName(method_name, sizeof(void*));

  OatQuickMethodHeader* header = nullptr;
  // Infinite loop... Test harness will have its own timeout.
  while (true) {
    header = OatQuickMethodHeader::FromEntryPoint(method->GetEntryPointFromQuickCompiledCode());
    if (code_cache->ContainsPc(header->GetCode())) {
      break;
    } else {
      // sleep one second to give time to the JIT compiler.
      sleep(1);
    }
  }

  CodeInfo info = header->GetOptimizedCodeInfo();
  CodeInfoEncoding encoding = info.ExtractEncoding();
  CHECK(info.HasInlineInfo(encoding));
}

extern "C" JNIEXPORT void JNICALL Java_Main_ensureJittedAndPolymorphicInline(JNIEnv*, jclass cls) {
  jit::Jit* jit = Runtime::Current()->GetJit();
  if (jit == nullptr) {
    return;
  }

  if (kIsDebugBuild) {
    // A debug build might often compile the methods without profiling informations filled.
    return;
  }

  do_checks(cls, "testInvokeVirtual");
  do_checks(cls, "testInvokeInterface");
  do_checks(cls, "testInvokeInterface2");
  do_checks(cls, "$noinline$testInlineToSameTarget");
}

}  // namespace art
