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

#ifndef ART_RUNTIME_ARCH_ARM64_QUICK_METHOD_FRAME_INFO_ARM64_H_
#define ART_RUNTIME_ARCH_ARM64_QUICK_METHOD_FRAME_INFO_ARM64_H_

#include "quick/quick_method_frame_info.h"
#include "registers_arm64.h"
#include "runtime.h"  // for Runtime::CalleeSaveType.

namespace art {
namespace arm64 {

// Callee saved registers
static constexpr uint32_t kArm64CalleeSaveRefSpills =
    (1 << art::arm64::X19) | (1 << art::arm64::X20) | (1 << art::arm64::X21) |
    (1 << art::arm64::X22) | (1 << art::arm64::X23) | (1 << art::arm64::X24) |
    (1 << art::arm64::X25) | (1 << art::arm64::X26) | (1 << art::arm64::X27) |
    (1 << art::arm64::X28);
// X0 is the method pointer. Not saved.
static constexpr uint32_t kArm64CalleeSaveArgSpills =
    (1 << art::arm64::X1) | (1 << art::arm64::X2) | (1 << art::arm64::X3) |
    (1 << art::arm64::X4) | (1 << art::arm64::X5) | (1 << art::arm64::X6) |
    (1 << art::arm64::X7);
// TODO  This is conservative. Only ALL should include the thread register.
// The thread register is not preserved by the aapcs64.
// LR is always saved.
static constexpr uint32_t kArm64CalleeSaveAllSpills =  0;  // (1 << art::arm64::LR);

// Save callee-saved floating point registers. Rest are scratch/parameters.
static constexpr uint32_t kArm64CalleeSaveFpArgSpills =
    (1 << art::arm64::D0) | (1 << art::arm64::D1) | (1 << art::arm64::D2) |
    (1 << art::arm64::D3) | (1 << art::arm64::D4) | (1 << art::arm64::D5) |
    (1 << art::arm64::D6) | (1 << art::arm64::D7);
static constexpr uint32_t kArm64CalleeSaveFpRefSpills =
    (1 << art::arm64::D8)  | (1 << art::arm64::D9)  | (1 << art::arm64::D10) |
    (1 << art::arm64::D11)  | (1 << art::arm64::D12)  | (1 << art::arm64::D13) |
    (1 << art::arm64::D14)  | (1 << art::arm64::D15);
static constexpr uint32_t kArm64FpAllSpills =
    kArm64CalleeSaveFpArgSpills |
    (1 << art::arm64::D16)  | (1 << art::arm64::D17) | (1 << art::arm64::D18) |
    (1 << art::arm64::D19)  | (1 << art::arm64::D20) | (1 << art::arm64::D21) |
    (1 << art::arm64::D22)  | (1 << art::arm64::D23) | (1 << art::arm64::D24) |
    (1 << art::arm64::D25)  | (1 << art::arm64::D26) | (1 << art::arm64::D27) |
    (1 << art::arm64::D28)  | (1 << art::arm64::D29) | (1 << art::arm64::D30) |
    (1 << art::arm64::D31);

constexpr uint32_t Arm64CalleeSaveCoreSpills(Runtime::CalleeSaveType type) {
  return kArm64CalleeSaveRefSpills |
      (type == Runtime::kRefsAndArgs ? kArm64CalleeSaveArgSpills : 0) |
      (type == Runtime::kSaveAll ? kArm64CalleeSaveAllSpills : 0) | (1 << art::arm64::FP) |
      (1 << art::arm64::X18) | (1 << art::arm64::LR);
}

constexpr uint32_t Arm64CalleeSaveFpSpills(Runtime::CalleeSaveType type) {
  return kArm64CalleeSaveFpRefSpills |
      (type == Runtime::kRefsAndArgs ? kArm64CalleeSaveFpArgSpills: 0) |
      (type == Runtime::kSaveAll ? kArm64FpAllSpills : 0);
}

constexpr uint32_t Arm64CalleeSaveFrameSize(Runtime::CalleeSaveType type) {
  return RoundUp((POPCOUNT(Arm64CalleeSaveCoreSpills(type)) /* gprs */ +
                  POPCOUNT(Arm64CalleeSaveFpSpills(type)) /* fprs */ +
                  1 /* Method* */) * kArm64PointerSize, kStackAlignment);
}

constexpr QuickMethodFrameInfo Arm64CalleeSaveMethodFrameInfo(Runtime::CalleeSaveType type) {
  return QuickMethodFrameInfo(Arm64CalleeSaveFrameSize(type),
                              Arm64CalleeSaveCoreSpills(type),
                              Arm64CalleeSaveFpSpills(type));
}

}  // namespace arm64
}  // namespace art

#endif  // ART_RUNTIME_ARCH_ARM64_QUICK_METHOD_FRAME_INFO_ARM64_H_
