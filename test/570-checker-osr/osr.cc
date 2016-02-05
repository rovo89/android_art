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

class OsrVisitor : public StackVisitor {
 public:
  explicit OsrVisitor(Thread* thread)
      SHARED_REQUIRES(Locks::mutator_lock_)
      : StackVisitor(thread, nullptr, StackVisitor::StackWalkKind::kIncludeInlinedFrames),
        in_osr_method_(false) {}

  bool VisitFrame() SHARED_REQUIRES(Locks::mutator_lock_) {
    ArtMethod* m = GetMethod();
    std::string m_name(m->GetName());

    if ((m_name.compare("$noinline$returnInt") == 0) ||
        (m_name.compare("$noinline$returnFloat") == 0) ||
        (m_name.compare("$noinline$returnDouble") == 0) ||
        (m_name.compare("$noinline$returnLong") == 0) ||
        (m_name.compare("$noinline$deopt") == 0)) {
      const OatQuickMethodHeader* header =
          Runtime::Current()->GetJit()->GetCodeCache()->LookupOsrMethodHeader(m);
      if (header != nullptr && header == GetCurrentOatQuickMethodHeader()) {
        in_osr_method_ = true;
      }
      return false;
    }
    return true;
  }

  bool in_osr_method_;
};

extern "C" JNIEXPORT jboolean JNICALL Java_Main_ensureInOsrCode(JNIEnv*, jclass) {
  jit::Jit* jit = Runtime::Current()->GetJit();
  if (jit == nullptr) {
    // Just return true for non-jit configurations to stop the infinite loop.
    return JNI_TRUE;
  }
  ScopedObjectAccess soa(Thread::Current());
  OsrVisitor visitor(soa.Self());
  visitor.WalkStack();
  return visitor.in_osr_method_;
}

}  // namespace art
