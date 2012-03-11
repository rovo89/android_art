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

#include "dex_file.h"
#include "jni.h"
#include "macros.h"

#include <stdint.h>

namespace art {

class Method;
class Thread;

jobject GetThreadStack(JNIEnv*, Thread*);

struct NativeToManagedRecord {
  NativeToManagedRecord* link_;
  void* last_top_of_managed_stack_;
  uintptr_t last_top_of_managed_stack_pc_;
};

// Iterator over managed frames up to the first native-to-managed transition.
class PACKED Frame {
 public:
  Frame() : sp_(NULL) {}

  Frame(Method** sp) : sp_(sp) {}

  Method* GetMethod() const {
    return (sp_ != NULL) ? *sp_ : NULL;
  }

  bool HasNext() const {
    return NextMethod() != NULL;
  }

  void Next();

  uintptr_t GetReturnPC() const;

  void SetReturnPC(uintptr_t pc);

  uintptr_t LoadCalleeSave(int num) const;

  static int GetVRegOffset(const DexFile::CodeItem* code_item, uint32_t core_spills,
                           uint32_t fp_spills, size_t frame_size, int reg);

  uint32_t GetVReg(const DexFile::CodeItem* code_item, uint32_t core_spills, uint32_t fp_spills,
                    size_t frame_size, int vreg) const;

  uint32_t GetVReg(Method* m, int vreg) const;
  void SetVReg(Method* method, int vreg, uint32_t new_value);

  Method** GetSP() const {
    return sp_;
  }

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
