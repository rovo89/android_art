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
#include "ScopedUtfChars.h"
#include "stack_map.h"

namespace art {

extern "C" JNIEXPORT void JNICALL Java_Main_waitUntilJitted(JNIEnv* env,
                                                            jclass,
                                                            jclass itf,
                                                            jstring method_name) {
  jit::Jit* jit = Runtime::Current()->GetJit();
  if (jit == nullptr) {
    return;
  }

  ScopedObjectAccess soa(Thread::Current());

  ScopedUtfChars chars(env, method_name);
  CHECK(chars.c_str() != nullptr);

  mirror::Class* klass = soa.Decode<mirror::Class*>(itf);
  ArtMethod* method = klass->FindDeclaredDirectMethodByName(chars.c_str(), sizeof(void*));

  jit::JitCodeCache* code_cache = jit->GetCodeCache();
  OatQuickMethodHeader* header = nullptr;
  while (true) {
    header = OatQuickMethodHeader::FromEntryPoint(method->GetEntryPointFromQuickCompiledCode());
    if (code_cache->ContainsPc(header->GetCode())) {
      break;
    } else {
      // yield to scheduler to give time to the JIT compiler.
      sched_yield();
    }
  }
}

}  // namespace art
