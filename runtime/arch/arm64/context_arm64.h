/*
 * Copyright (C) 2014 The Android Open Source Project
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

#ifndef ART_RUNTIME_ARCH_ARM64_CONTEXT_ARM64_H_
#define ART_RUNTIME_ARCH_ARM64_CONTEXT_ARM64_H_

#include "arch/context.h"
#include "base/logging.h"
#include "registers_arm64.h"

namespace art {
namespace arm64 {

class Arm64Context : public Context {
 public:
  Arm64Context() {
    Reset();
  }

  ~Arm64Context() {}

  void Reset();

  void FillCalleeSaves(const StackVisitor& fr);

  void SetSP(uintptr_t new_sp) {
    SetGPR(SP, new_sp);
  }

  void SetPC(uintptr_t new_lr) {
    SetGPR(LR, new_lr);
  }

  virtual uintptr_t* GetGPRAddress(uint32_t reg) {
    DCHECK_LT(reg, static_cast<uint32_t>(kNumberOfCoreRegisters));
    return gprs_[reg];
  }

  uintptr_t GetGPR(uint32_t reg) {
    DCHECK_LT(reg, static_cast<uint32_t>(kNumberOfCoreRegisters));
    return *gprs_[reg];
  }

  void SetGPR(uint32_t reg, uintptr_t value);
  void SmashCallerSaves();
  void DoLongJump();

 private:
  // Pointers to register locations, initialized to NULL or the specific registers below.
  uintptr_t* gprs_[kNumberOfCoreRegisters];
  uint64_t * fprs_[kNumberOfDRegisters];
  // Hold values for sp and pc if they are not located within a stack frame.
  uintptr_t sp_, pc_;
};

}  // namespace arm64
}  // namespace art

#endif  // ART_RUNTIME_ARCH_ARM64_CONTEXT_ARM64_H_
