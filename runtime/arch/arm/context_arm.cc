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

#include "context_arm.h"

#include "mirror/art_method-inl.h"
#include "quick/quick_method_frame_info.h"
#include "utils.h"

namespace art {
namespace arm {

static constexpr uint32_t gZero = 0;

void ArmContext::Reset() {
  for (size_t i = 0; i < kNumberOfCoreRegisters; i++) {
    gprs_[i] = nullptr;
  }
  for (size_t i = 0; i < kNumberOfSRegisters; i++) {
    fprs_[i] = nullptr;
  }
  gprs_[SP] = &sp_;
  gprs_[PC] = &pc_;
  // Initialize registers with easy to spot debug values.
  sp_ = ArmContext::kBadGprBase + SP;
  pc_ = ArmContext::kBadGprBase + PC;
}

void ArmContext::FillCalleeSaves(const StackVisitor& fr) {
  mirror::ArtMethod* method = fr.GetMethod();
  const QuickMethodFrameInfo frame_info = method->GetQuickFrameInfo();
  size_t spill_count = POPCOUNT(frame_info.CoreSpillMask());
  size_t fp_spill_count = POPCOUNT(frame_info.FpSpillMask());
  if (spill_count > 0) {
    // Lowest number spill is farthest away, walk registers and fill into context
    int j = 1;
    for (size_t i = 0; i < kNumberOfCoreRegisters; i++) {
      if (((frame_info.CoreSpillMask() >> i) & 1) != 0) {
        gprs_[i] = fr.CalleeSaveAddress(spill_count - j, frame_info.FrameSizeInBytes());
        j++;
      }
    }
  }
  if (fp_spill_count > 0) {
    // Lowest number spill is farthest away, walk registers and fill into context
    int j = 1;
    for (size_t i = 0; i < kNumberOfSRegisters; i++) {
      if (((frame_info.FpSpillMask() >> i) & 1) != 0) {
        fprs_[i] = fr.CalleeSaveAddress(spill_count + fp_spill_count - j,
                                        frame_info.FrameSizeInBytes());
        j++;
      }
    }
  }
}

bool ArmContext::SetGPR(uint32_t reg, uintptr_t value) {
  DCHECK_LT(reg, static_cast<uint32_t>(kNumberOfCoreRegisters));
  DCHECK_NE(gprs_[reg], &gZero);  // Can't overwrite this static value since they are never reset.
  if (gprs_[reg] != nullptr) {
    *gprs_[reg] = value;
    return true;
  } else {
    return false;
  }
}

bool ArmContext::SetFPR(uint32_t reg, uintptr_t value) {
  DCHECK_LT(reg, static_cast<uint32_t>(kNumberOfSRegisters));
  DCHECK_NE(fprs_[reg], &gZero);  // Can't overwrite this static value since they are never reset.
  if (fprs_[reg] != nullptr) {
    *fprs_[reg] = value;
    return true;
  } else {
    return false;
  }
}

void ArmContext::SmashCallerSaves() {
  // This needs to be 0 because we want a null/zero return value.
  gprs_[R0] = const_cast<uint32_t*>(&gZero);
  gprs_[R1] = const_cast<uint32_t*>(&gZero);
  gprs_[R2] = nullptr;
  gprs_[R3] = nullptr;

  fprs_[S0] = nullptr;
  fprs_[S1] = nullptr;
  fprs_[S2] = nullptr;
  fprs_[S3] = nullptr;
  fprs_[S4] = nullptr;
  fprs_[S5] = nullptr;
  fprs_[S6] = nullptr;
  fprs_[S7] = nullptr;
  fprs_[S8] = nullptr;
  fprs_[S9] = nullptr;
  fprs_[S10] = nullptr;
  fprs_[S11] = nullptr;
  fprs_[S12] = nullptr;
  fprs_[S13] = nullptr;
  fprs_[S14] = nullptr;
  fprs_[S15] = nullptr;
}

extern "C" void art_quick_do_long_jump(uint32_t*, uint32_t*);

void ArmContext::DoLongJump() {
  uintptr_t gprs[kNumberOfCoreRegisters];
  uint32_t fprs[kNumberOfSRegisters];
  for (size_t i = 0; i < kNumberOfCoreRegisters; ++i) {
    gprs[i] = gprs_[i] != nullptr ? *gprs_[i] : ArmContext::kBadGprBase + i;
  }
  for (size_t i = 0; i < kNumberOfSRegisters; ++i) {
    fprs[i] = fprs_[i] != nullptr ? *fprs_[i] : ArmContext::kBadFprBase + i;
  }
  DCHECK_EQ(reinterpret_cast<uintptr_t>(Thread::Current()), gprs[TR]);
  art_quick_do_long_jump(gprs, fprs);
}

}  // namespace arm
}  // namespace art
