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

#ifndef ART_COMPILER_DEX_QUICK_COMPILER_CALLBACKS_H_
#define ART_COMPILER_DEX_QUICK_COMPILER_CALLBACKS_H_

#include "compiler_callbacks.h"

namespace art {

class VerificationResults;
class DexFileToMethodInlinerMap;

class QuickCompilerCallbacks FINAL : public CompilerCallbacks {
  public:
    QuickCompilerCallbacks(VerificationResults* verification_results,
                           DexFileToMethodInlinerMap* method_inliner_map)
        : verification_results_(verification_results),
          method_inliner_map_(method_inliner_map) {
      CHECK(verification_results != nullptr);
      CHECK(method_inliner_map != nullptr);
    }

    ~QuickCompilerCallbacks() { }

    bool MethodVerified(verifier::MethodVerifier* verifier)
        SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) OVERRIDE;

    void ClassRejected(ClassReference ref) OVERRIDE;

    // We are running in an environment where we can call patchoat safely so we should.
    bool IsRelocationPossible() OVERRIDE {
      return true;
    }

  private:
    VerificationResults* const verification_results_;
    DexFileToMethodInlinerMap* const method_inliner_map_;
};

}  // namespace art

#endif  // ART_COMPILER_DEX_QUICK_COMPILER_CALLBACKS_H_
