/*
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef ART_SRC_OAT_RUNTIME_OAT_RUNTIME_STUB_H_
#define ART_SRC_OAT_RUNTIME_OAT_RUNTIME_STUB_H_

#include "runtime.h"

namespace art {
namespace mirror {
template<class T> class PrimitiveArray;
typedef PrimitiveArray<int8_t> ByteArray;
}  // namespace mirror

namespace arm {
  mirror::ByteArray* CreateAbstractMethodErrorStub()
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  mirror::ByteArray* ArmCreateResolutionTrampoline(Runtime::TrampolineType type)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  mirror::ByteArray* CreateJniDlsymLookupStub()
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
}

namespace mips {
  mirror::ByteArray* CreateAbstractMethodErrorStub()
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  mirror::ByteArray* MipsCreateResolutionTrampoline(Runtime::TrampolineType type)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  mirror::ByteArray* CreateJniDlsymLookupStub()
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
}

namespace x86 {
  mirror::ByteArray* CreateAbstractMethodErrorStub()
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  mirror::ByteArray* X86CreateResolutionTrampoline(Runtime::TrampolineType type)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  mirror::ByteArray* CreateJniDlsymLookupStub()
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
}

}  // namespace art

#endif  // ART_SRC_OAT_RUNTIME_OAT_RUNTIME_STUB_H_
