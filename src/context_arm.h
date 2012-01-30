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

#ifndef ART_SRC_CONTEXT_ARM_H_
#define ART_SRC_CONTEXT_ARM_H_

#include "constants_arm.h"
#include "context.h"

namespace art {
namespace arm {

class ArmContext : public Context {
 public:
  ArmContext();
  virtual ~ArmContext() {}

  virtual void FillCalleeSaves(const Frame& fr);

  virtual void SetSP(uintptr_t new_sp) {
    gprs_[SP] = new_sp;
  }

  virtual void SetPC(uintptr_t new_pc) {
    gprs_[PC] = new_pc;
  }

  virtual uintptr_t GetGPR(uint32_t reg) {
    CHECK_GE(reg, 0u);
    CHECK_LT(reg, 16u);
    return gprs_[reg];
  }

  virtual void DoLongJump();

 private:
  uintptr_t gprs_[16];
  uint32_t fprs_[32];
};

}  // namespace arm
}  // namespace art

#endif  // ART_SRC_CONTEXT_ARM_H_
