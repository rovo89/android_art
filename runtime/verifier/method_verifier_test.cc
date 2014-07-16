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

#include "method_verifier.h"

#include <stdio.h>
#include <memory>

#include "class_linker-inl.h"
#include "common_runtime_test.h"
#include "dex_file.h"
#include "scoped_thread_state_change.h"

namespace art {
namespace verifier {

class MethodVerifierTest : public CommonRuntimeTest {
 protected:
  void VerifyClass(const std::string& descriptor)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    ASSERT_TRUE(descriptor != NULL);
    mirror::Class* klass = class_linker_->FindSystemClass(Thread::Current(), descriptor.c_str());

    // Verify the class
    std::string error_msg;
    ASSERT_TRUE(MethodVerifier::VerifyClass(klass, true, &error_msg) == MethodVerifier::kNoFailure)
        << error_msg;
  }

  void VerifyDexFile(const DexFile* dex)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    ASSERT_TRUE(dex != NULL);

    // Verify all the classes defined in this file
    for (size_t i = 0; i < dex->NumClassDefs(); i++) {
      const DexFile::ClassDef& class_def = dex->GetClassDef(i);
      const char* descriptor = dex->GetClassDescriptor(class_def);
      VerifyClass(descriptor);
    }
  }
};

TEST_F(MethodVerifierTest, LibCore) {
  ScopedObjectAccess soa(Thread::Current());
  VerifyDexFile(java_lang_dex_file_);
}

}  // namespace verifier
}  // namespace art
