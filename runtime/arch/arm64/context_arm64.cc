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

#include <stdint.h>

#include "context_arm64.h"

#include "mirror/art_method-inl.h"
#include "mirror/object-inl.h"
#include "quick/quick_method_frame_info.h"
#include "stack.h"
#include "thread.h"


namespace art {
namespace arm64 {

static constexpr uint64_t gZero = 0;

void Arm64Context::Reset() {
  for (size_t i = 0; i < kNumberOfCoreRegisters; i++) {
    gprs_[i] = nullptr;
  }
  for (size_t i = 0; i < kNumberOfDRegisters; i++) {
    fprs_[i] = nullptr;
  }
  gprs_[SP] = &sp_;
  gprs_[LR] = &pc_;
  // Initialize registers with easy to spot debug values.
  sp_ = Arm64Context::kBadGprBase + SP;
  pc_ = Arm64Context::kBadGprBase + LR;
}

void Arm64Context::FillCalleeSaves(const StackVisitor& fr) {
  mirror::ArtMethod* method = fr.GetMethod();
  const QuickMethodFrameInfo frame_info = method->GetQuickFrameInfo();
  size_t spill_count = POPCOUNT(frame_info.CoreSpillMask());
  size_t fp_spill_count = POPCOUNT(frame_info.FpSpillMask());
  if (spill_count > 0) {
    // Lowest number spill is farthest away, walk registers and fill into context.
    int j = 1;
    for (size_t i = 0; i < kNumberOfCoreRegisters; i++) {
      if (((frame_info.CoreSpillMask() >> i) & 1) != 0) {
        gprs_[i] = fr.CalleeSaveAddress(spill_count  - j, frame_info.FrameSizeInBytes());
        j++;
      }
    }
  }

  if (fp_spill_count > 0) {
    // Lowest number spill is farthest away, walk registers and fill into context.
    int j = 1;
    for (size_t i = 0; i < kNumberOfDRegisters; i++) {
      if (((frame_info.FpSpillMask() >> i) & 1) != 0) {
        fprs_[i] = fr.CalleeSaveAddress(spill_count + fp_spill_count - j,
                                        frame_info.FrameSizeInBytes());
        j++;
      }
    }
  }
}

bool Arm64Context::SetGPR(uint32_t reg, uintptr_t value) {
  DCHECK_LT(reg, static_cast<uint32_t>(kNumberOfCoreRegisters));
  DCHECK_NE(gprs_[reg], &gZero);  // Can't overwrite this static value since they are never reset.
  if (gprs_[reg] != nullptr) {
    *gprs_[reg] = value;
    return true;
  } else {
    return false;
  }
}

bool Arm64Context::SetFPR(uint32_t reg, uintptr_t value) {
  DCHECK_LT(reg, static_cast<uint32_t>(kNumberOfDRegisters));
  DCHECK_NE(fprs_[reg], &gZero);  // Can't overwrite this static value since they are never reset.
  if (fprs_[reg] != nullptr) {
    *fprs_[reg] = value;
    return true;
  } else {
    return false;
  }
}

void Arm64Context::SmashCallerSaves() {
  // This needs to be 0 because we want a null/zero return value.
  gprs_[X0] = const_cast<uint64_t*>(&gZero);
  gprs_[X1] = nullptr;
  gprs_[X2] = nullptr;
  gprs_[X3] = nullptr;
  gprs_[X4] = nullptr;
  gprs_[X5] = nullptr;
  gprs_[X6] = nullptr;
  gprs_[X7] = nullptr;
  gprs_[X8] = nullptr;
  gprs_[X9] = nullptr;
  gprs_[X10] = nullptr;
  gprs_[X11] = nullptr;
  gprs_[X12] = nullptr;
  gprs_[X13] = nullptr;
  gprs_[X14] = nullptr;
  gprs_[X15] = nullptr;

  // d0-d7, d16-d31 are caller-saved; d8-d15 are callee-saved.

  fprs_[D0] = nullptr;
  fprs_[D1] = nullptr;
  fprs_[D2] = nullptr;
  fprs_[D3] = nullptr;
  fprs_[D4] = nullptr;
  fprs_[D5] = nullptr;
  fprs_[D6] = nullptr;
  fprs_[D7] = nullptr;

  fprs_[D16] = nullptr;
  fprs_[D17] = nullptr;
  fprs_[D18] = nullptr;
  fprs_[D19] = nullptr;
  fprs_[D20] = nullptr;
  fprs_[D21] = nullptr;
  fprs_[D22] = nullptr;
  fprs_[D23] = nullptr;
  fprs_[D24] = nullptr;
  fprs_[D25] = nullptr;
  fprs_[D26] = nullptr;
  fprs_[D27] = nullptr;
  fprs_[D28] = nullptr;
  fprs_[D29] = nullptr;
  fprs_[D30] = nullptr;
  fprs_[D31] = nullptr;
}

extern "C" void art_quick_do_long_jump(uint64_t*, uint64_t*);

void Arm64Context::DoLongJump() {
  uint64_t gprs[32];
  uint64_t fprs[kNumberOfDRegisters];

  // Do not use kNumberOfCoreRegisters, as this is with the distinction of SP and XZR
  for (size_t i = 0; i < 32; ++i) {
    gprs[i] = gprs_[i] != nullptr ? *gprs_[i] : Arm64Context::kBadGprBase + i;
  }
  for (size_t i = 0; i < kNumberOfDRegisters; ++i) {
    fprs[i] = fprs_[i] != nullptr ? *fprs_[i] : Arm64Context::kBadGprBase + i;
  }
  DCHECK_EQ(reinterpret_cast<uintptr_t>(Thread::Current()), gprs[TR]);
  art_quick_do_long_jump(gprs, fprs);
}

}  // namespace arm64
}  // namespace art
