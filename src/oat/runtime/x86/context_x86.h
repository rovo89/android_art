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

#ifndef ART_SRC_OAT_RUNTIME_X86_CONTEXT_X86_H_
#define ART_SRC_OAT_RUNTIME_X86_CONTEXT_X86_H_

#include "constants_x86.h"
#include "oat/runtime/context.h"

namespace art {
namespace x86 {

class X86Context : public Context {
 public:
  X86Context();
  virtual ~X86Context() {}

  // No callee saves on X86
  virtual void FillCalleeSaves(const StackVisitor& fr);

  virtual void SetSP(uintptr_t new_sp) {
    gprs_[ESP] = new_sp;
  }

  virtual void SetPC(uintptr_t new_pc) {
    eip_ = new_pc;
  }

  virtual uintptr_t GetGPR(uint32_t reg) {
    CHECK_GE(reg, 0u);
    CHECK_LT(reg, 8u);
    return gprs_[reg];
  }

  virtual void SmashCallerSaves();
  virtual void DoLongJump();

 private:
  uintptr_t gprs_[8];
  uintptr_t eip_;
};
}  // namespace x86
}  // namespace art

#endif  // ART_SRC_OAT_RUNTIME_X86_CONTEXT_X86_H_
