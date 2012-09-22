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

#include "context_mips.h"

#include "object.h"

namespace art {
namespace mips {

MipsContext::MipsContext() {
#ifndef NDEBUG
  // Initialize registers with easy to spot debug values.
  for (int i = 0; i < 32; i++) {
    gprs_[i] = kBadGprBase + i;
  }
  for (int i = 0; i < 32; i++) {
    fprs_[i] = kBadGprBase + i;
  }
  pc_ = 0xEBAD601F;
#endif
}

void MipsContext::FillCalleeSaves(const StackVisitor& fr) {
  AbstractMethod* method = fr.GetMethod();
  uint32_t core_spills = method->GetCoreSpillMask();
  uint32_t fp_core_spills = method->GetFpSpillMask();
  size_t spill_count = __builtin_popcount(core_spills);
  size_t fp_spill_count = __builtin_popcount(fp_core_spills);
  size_t frame_size = method->GetFrameSizeInBytes();
  if (spill_count > 0) {
    // Lowest number spill is furthest away, walk registers and fill into context.
    int j = 1;
    for (int i = 0; i < 32; i++) {
      if (((core_spills >> i) & 1) != 0) {
        gprs_[i] = fr.LoadCalleeSave(spill_count - j, frame_size);
        j++;
      }
    }
  }
  if (fp_spill_count > 0) {
    // Lowest number spill is furthest away, walk registers and fill into context.
    int j = 1;
    for (int i = 0; i < 32; i++) {
      if (((fp_core_spills >> i) & 1) != 0) {
        fprs_[i] = fr.LoadCalleeSave(spill_count +fp_spill_count - j, frame_size);
        j++;
      }
    }
  }
}

void MipsContext::SmashCallerSaves() {
  gprs_[V0] = 0; // This needs to be 0 because we want a null/zero return value.
  gprs_[V1] = 0; // This needs to be 0 because we want a null/zero return value.
  gprs_[A1] = kBadGprBase + A1;
  gprs_[A2] = kBadGprBase + A2;
  gprs_[A3] = kBadGprBase + A3;
  gprs_[RA] = kBadGprBase + RA;
}

extern "C" void art_do_long_jump(uint32_t*, uint32_t*);

void MipsContext::DoLongJump() {
  art_do_long_jump(&gprs_[ZERO], &fprs_[F0]);
}

}  // namespace mips
}  // namespace art
