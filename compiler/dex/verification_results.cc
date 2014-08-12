/*
 * Copyright (C) 2013 The Android Open Source Project
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

#include "verification_results.h"

#include "base/stl_util.h"
#include "base/mutex.h"
#include "base/mutex-inl.h"
#include "driver/compiler_driver.h"
#include "driver/compiler_options.h"
#include "thread.h"
#include "thread-inl.h"
#include "verified_method.h"
#include "verifier/method_verifier.h"
#include "verifier/method_verifier-inl.h"

namespace art {

VerificationResults::VerificationResults(const CompilerOptions* compiler_options)
    : compiler_options_(compiler_options),
      verified_methods_lock_("compiler verified methods lock"),
      verified_methods_(),
      rejected_classes_lock_("compiler rejected classes lock"),
      rejected_classes_() {
  UNUSED(compiler_options);
}

VerificationResults::~VerificationResults() {
  Thread* self = Thread::Current();
  {
    WriterMutexLock mu(self, verified_methods_lock_);
    STLDeleteValues(&verified_methods_);
  }
}

bool VerificationResults::ProcessVerifiedMethod(verifier::MethodVerifier* method_verifier) {
  DCHECK(method_verifier != NULL);
  MethodReference ref = method_verifier->GetMethodReference();
  bool compile = IsCandidateForCompilation(ref, method_verifier->GetAccessFlags());
  // TODO: Check also for virtual/interface invokes when DEX-to-DEX supports devirtualization.
  if (!compile && !method_verifier->HasCheckCasts()) {
    return true;
  }

  const VerifiedMethod* verified_method = VerifiedMethod::Create(method_verifier, compile);
  if (verified_method == nullptr) {
    DCHECK(method_verifier->HasFailures());
    return false;
  }

  WriterMutexLock mu(Thread::Current(), verified_methods_lock_);
  auto it = verified_methods_.find(ref);
  if (it != verified_methods_.end()) {
    // TODO: Investigate why are we doing the work again for this method and try to avoid it.
    LOG(WARNING) << "Method processed more than once: "
        << PrettyMethod(ref.dex_method_index, *ref.dex_file);
    DCHECK_EQ(it->second->GetDevirtMap().size(), verified_method->GetDevirtMap().size());
    DCHECK_EQ(it->second->GetSafeCastSet().size(), verified_method->GetSafeCastSet().size());
    DCHECK_EQ(it->second->GetDexGcMap().size(), verified_method->GetDexGcMap().size());
    delete it->second;
    verified_methods_.erase(it);
  }
  verified_methods_.Put(ref, verified_method);
  DCHECK(verified_methods_.find(ref) != verified_methods_.end());
  return true;
}

const VerifiedMethod* VerificationResults::GetVerifiedMethod(MethodReference ref) {
  ReaderMutexLock mu(Thread::Current(), verified_methods_lock_);
  auto it = verified_methods_.find(ref);
  return (it != verified_methods_.end()) ? it->second : nullptr;
}

void VerificationResults::AddRejectedClass(ClassReference ref) {
  {
    WriterMutexLock mu(Thread::Current(), rejected_classes_lock_);
    rejected_classes_.insert(ref);
  }
  DCHECK(IsClassRejected(ref));
}

bool VerificationResults::IsClassRejected(ClassReference ref) {
  ReaderMutexLock mu(Thread::Current(), rejected_classes_lock_);
  return (rejected_classes_.find(ref) != rejected_classes_.end());
}

bool VerificationResults::IsCandidateForCompilation(MethodReference& method_ref,
                                                    const uint32_t access_flags) {
#ifdef ART_SEA_IR_MODE
  bool use_sea = compiler_options_->GetSeaIrMode();
  use_sea = use_sea && (std::string::npos != PrettyMethod(
                        method_ref.dex_method_index, *(method_ref.dex_file)).find("fibonacci"));
  if (use_sea) {
    return true;
  }
#endif
  if (!compiler_options_->IsCompilationEnabled()) {
    return false;
  }
  // Don't compile class initializers, ever.
  if (((access_flags & kAccConstructor) != 0) && ((access_flags & kAccStatic) != 0)) {
    return false;
  }
  return true;
}

}  // namespace art
