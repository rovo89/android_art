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

static const uint64_t gZero = 0;

void Arm64Context::Reset() {
  for (size_t i = 0; i < kNumberOfCoreRegisters; i++) {
    gprs_[i] = NULL;
  }
  for (size_t i = 0; i < kNumberOfDRegisters; i++) {
    fprs_[i] = NULL;
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

void Arm64Context::SetGPR(uint32_t reg, uintptr_t value) {
  DCHECK_LT(reg, static_cast<uint32_t>(kNumberOfCoreRegisters));
  DCHECK_NE(gprs_[reg], &gZero);  // Can't overwrite this static value since they are never reset.
  DCHECK(gprs_[reg] != NULL);
  *gprs_[reg] = value;
}

void Arm64Context::SmashCallerSaves() {
  // This needs to be 0 because we want a null/zero return value.
  gprs_[X0] = const_cast<uint64_t*>(&gZero);
  gprs_[X1] = NULL;
  gprs_[X2] = NULL;
  gprs_[X3] = NULL;
  gprs_[X4] = NULL;
  gprs_[X5] = NULL;
  gprs_[X6] = NULL;
  gprs_[X7] = NULL;
  gprs_[X8] = NULL;
  gprs_[X9] = NULL;
  gprs_[X10] = NULL;
  gprs_[X11] = NULL;
  gprs_[X12] = NULL;
  gprs_[X13] = NULL;
  gprs_[X14] = NULL;
  gprs_[X15] = NULL;

  fprs_[D8] = NULL;
  fprs_[D9] = NULL;
  fprs_[D10] = NULL;
  fprs_[D11] = NULL;
  fprs_[D12] = NULL;
  fprs_[D13] = NULL;
  fprs_[D14] = NULL;
  fprs_[D15] = NULL;
}

extern "C" void art_quick_do_long_jump(uint64_t*, uint64_t*);

void Arm64Context::DoLongJump() {
  uint64_t gprs[32];
  uint64_t fprs[32];

  // Do not use kNumberOfCoreRegisters, as this is with the distinction of SP and XZR
  for (size_t i = 0; i < 32; ++i) {
    gprs[i] = gprs_[i] != NULL ? *gprs_[i] : Arm64Context::kBadGprBase + i;
  }
  for (size_t i = 0; i < kNumberOfDRegisters; ++i) {
    fprs[i] = fprs_[i] != NULL ? *fprs_[i] : Arm64Context::kBadGprBase + i;
  }
  DCHECK_EQ(reinterpret_cast<uintptr_t>(Thread::Current()), gprs[TR]);
  art_quick_do_long_jump(gprs, fprs);
}

}  // namespace arm64
}  // namespace art
