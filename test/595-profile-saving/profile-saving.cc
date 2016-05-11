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

#include "dex_file.h"

#include "art_method-inl.h"
#include "jit/offline_profiling_info.h"
#include "jit/profile_saver.h"
#include "jni.h"
#include "method_reference.h"
#include "mirror/class-inl.h"
#include "oat_file_assistant.h"
#include "oat_file_manager.h"
#include "scoped_thread_state_change.h"
#include "ScopedUtfChars.h"
#include "thread.h"

namespace art {
namespace {

class CreateProfilingInfoVisitor : public StackVisitor {
 public:
  explicit CreateProfilingInfoVisitor(Thread* thread, const char* method_name)
      SHARED_REQUIRES(Locks::mutator_lock_)
      : StackVisitor(thread, nullptr, StackVisitor::StackWalkKind::kIncludeInlinedFrames),
        method_name_(method_name) {}

  bool VisitFrame() SHARED_REQUIRES(Locks::mutator_lock_) {
    ArtMethod* m = GetMethod();
    std::string m_name(m->GetName());

    if (m_name.compare(method_name_) == 0) {
      ProfilingInfo::Create(Thread::Current(), m, /* retry_allocation */ true);
      method_index_ = m->GetDexMethodIndex();
      return false;
    }
    return true;
  }

  int method_index_ = -1;
  const char* const method_name_;
};

extern "C" JNIEXPORT jint JNICALL Java_Main_ensureProfilingInfo(JNIEnv* env,
                                                                jclass,
                                                                jstring method_name) {
  ScopedUtfChars chars(env, method_name);
  CHECK(chars.c_str() != nullptr);
  ScopedObjectAccess soa(Thread::Current());
  CreateProfilingInfoVisitor visitor(soa.Self(), chars.c_str());
  visitor.WalkStack();
  return visitor.method_index_;
}

extern "C" JNIEXPORT void JNICALL Java_Main_ensureProfileProcessing(JNIEnv*, jclass) {
  ProfileSaver::ForceProcessProfiles();
}

extern "C" JNIEXPORT jboolean JNICALL Java_Main_presentInProfile(
      JNIEnv* env, jclass cls, jstring filename, jint method_index) {
  ScopedUtfChars filename_chars(env, filename);
  CHECK(filename_chars.c_str() != nullptr);
  ScopedObjectAccess soa(Thread::Current());
  const DexFile* dex_file = soa.Decode<mirror::Class*>(cls)->GetDexCache()->GetDexFile();
  return ProfileSaver::HasSeenMethod(std::string(filename_chars.c_str()),
                                     dex_file,
                                     static_cast<uint16_t>(method_index));
}

}  // namespace
}  // namespace art
