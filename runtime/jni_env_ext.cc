/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "jni_env_ext.h"

#include "check_jni.h"
#include "indirect_reference_table.h"
#include "java_vm_ext.h"
#include "jni_internal.h"

namespace art {

static constexpr size_t kMonitorsInitial = 32;  // Arbitrary.
static constexpr size_t kMonitorsMax = 4096;  // Arbitrary sanity check.

static constexpr size_t kLocalsInitial = 64;  // Arbitrary.

// Checking "locals" requires the mutator lock, but at creation time we're really only interested
// in validity, which isn't changing. To avoid grabbing the mutator lock, factored out and tagged
// with NO_THREAD_SAFETY_ANALYSIS.
static bool CheckLocalsValid(JNIEnvExt* in) NO_THREAD_SAFETY_ANALYSIS {
  if (in == nullptr) {
    return false;
  }
  return in->locals.IsValid();
}

JNIEnvExt* JNIEnvExt::Create(Thread* self_in, JavaVMExt* vm_in) {
  std::unique_ptr<JNIEnvExt> ret(new JNIEnvExt(self_in, vm_in));
  if (CheckLocalsValid(ret.get())) {
    return ret.release();
  }
  return nullptr;
}

JNIEnvExt::JNIEnvExt(Thread* self_in, JavaVMExt* vm_in)
    : self(self_in),
      vm(vm_in),
      local_ref_cookie(IRT_FIRST_SEGMENT),
      locals(kLocalsInitial, kLocalsMax, kLocal, false),
      check_jni(false),
      critical(0),
      monitors("monitors", kMonitorsInitial, kMonitorsMax) {
  functions = unchecked_functions = GetJniNativeInterface();
  if (vm->IsCheckJniEnabled()) {
    SetCheckJniEnabled(true);
  }
}

JNIEnvExt::~JNIEnvExt() {
}

jobject JNIEnvExt::NewLocalRef(mirror::Object* obj) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (obj == nullptr) {
    return nullptr;
  }
  return reinterpret_cast<jobject>(locals.Add(local_ref_cookie, obj));
}

void JNIEnvExt::DeleteLocalRef(jobject obj) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (obj != nullptr) {
    locals.Remove(local_ref_cookie, reinterpret_cast<IndirectRef>(obj));
  }
}

void JNIEnvExt::SetCheckJniEnabled(bool enabled) {
  check_jni = enabled;
  functions = enabled ? GetCheckJniNativeInterface() : GetJniNativeInterface();
}

void JNIEnvExt::DumpReferenceTables(std::ostream& os) {
  locals.Dump(os);
  monitors.Dump(os);
}

void JNIEnvExt::PushFrame(int capacity) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  UNUSED(capacity);  // cpplint gets confused with (int) and thinks its a cast.
  // TODO: take 'capacity' into account.
  stacked_local_ref_cookies.push_back(local_ref_cookie);
  local_ref_cookie = locals.GetSegmentState();
}

void JNIEnvExt::PopFrame() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  locals.SetSegmentState(local_ref_cookie);
  local_ref_cookie = stacked_local_ref_cookies.back();
  stacked_local_ref_cookies.pop_back();
}

Offset JNIEnvExt::SegmentStateOffset() {
  return Offset(OFFSETOF_MEMBER(JNIEnvExt, locals) +
                IndirectReferenceTable::SegmentStateOffset().Int32Value());
}

}  // namespace art
