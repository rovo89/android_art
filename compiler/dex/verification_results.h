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

#ifndef ART_COMPILER_DEX_VERIFICATION_RESULTS_H_
#define ART_COMPILER_DEX_VERIFICATION_RESULTS_H_

#include <stdint.h>
#include <set>
#include <vector>

#include "base/macros.h"
#include "base/mutex.h"
#include "class_reference.h"
#include "method_reference.h"
#include "safe_map.h"

namespace art {

namespace verifier {
class MethodVerifier;
}  // namespace verifier

class VerifiedMethod;

class VerificationResults {
  public:
    VerificationResults();
    ~VerificationResults();

    bool ProcessVerifiedMethod(verifier::MethodVerifier* method_verifier)
        SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
        LOCKS_EXCLUDED(verified_methods_lock_);

    const VerifiedMethod* GetVerifiedMethod(MethodReference ref)
        LOCKS_EXCLUDED(verified_methods_lock_);

    const std::vector<uint8_t>* GetDexGcMap(MethodReference ref)
        LOCKS_EXCLUDED(verified_methods_lock_);

    const MethodReference* GetDevirtMap(const MethodReference& ref, uint32_t dex_pc)
        LOCKS_EXCLUDED(verified_methods_lock_);

    // Returns true if the cast can statically be verified to be redundant
    // by using the check-cast elision peephole optimization in the verifier.
    bool IsSafeCast(MethodReference ref, uint32_t pc) LOCKS_EXCLUDED(safecast_map_lock_);

    void AddRejectedClass(ClassReference ref) LOCKS_EXCLUDED(rejected_classes_lock_);
    bool IsClassRejected(ClassReference ref) LOCKS_EXCLUDED(rejected_classes_lock_);

    static bool IsCandidateForCompilation(MethodReference& method_ref,
                                          const uint32_t access_flags);

  private:
    // Verified methods.
    typedef SafeMap<MethodReference, const VerifiedMethod*,
        MethodReferenceComparator> VerifiedMethodMap;
    ReaderWriterMutex verified_methods_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
    VerifiedMethodMap verified_methods_;

    // Rejected classes.
    ReaderWriterMutex rejected_classes_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
    std::set<ClassReference> rejected_classes_ GUARDED_BY(rejected_classes_lock_);
};

}  // namespace art

#endif  // ART_COMPILER_DEX_VERIFICATION_RESULTS_H_
