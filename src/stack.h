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

#ifndef ART_SRC_STACK_H_
#define ART_SRC_STACK_H_

#include "macros.h"

#include <stdint.h>

namespace art {

class Method;
class Thread;

struct NativeToManagedRecord {
  NativeToManagedRecord* link_;
  void* last_top_of_managed_stack_;
  uintptr_t last_top_of_managed_stack_pc_;
};

// Iterator over managed frames up to the first native-to-managed transition.
class PACKED Frame {
 public:
  Frame() : sp_(NULL) {}

  Method* GetMethod() const {
    return (sp_ != NULL) ? *sp_ : NULL;
  }

  bool HasNext() const {
    return NextMethod() != NULL;
  }

  void Next();

  uintptr_t GetReturnPC() const;

  uintptr_t LoadCalleeSave(int num) const;

  uintptr_t GetVReg(Method* method, int vreg) const;

  Method** GetSP() const {
    return sp_;
  }

  // TODO: this is here for testing, remove when we have exception unit tests
  // that use the real stack
  void SetSP(Method** sp) {
    sp_ = sp;
  }

  // Is this a frame for a real method (native or with dex code)
  bool HasMethod() const;

 private:
  Method* NextMethod() const;

  friend class Thread;

  Method** sp_;
};

}  // namespace art

#endif  // ART_SRC_STACK_H_
