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

#include "context_x86_64.h"

#include "mirror/art_method.h"
#include "mirror/object-inl.h"
#include "stack.h"

namespace art {
namespace x86_64 {

static const uintptr_t gZero = 0;

void X86_64Context::Reset() {
  for (int i = 0; i < kNumberOfCpuRegisters; i++) {
    gprs_[i] = NULL;
  }
  gprs_[RSP] = &rsp_;
  // Initialize registers with easy to spot debug values.
  rsp_ = X86_64Context::kBadGprBase + RSP;
  rip_ = X86_64Context::kBadGprBase + kNumberOfCpuRegisters;
}

void X86_64Context::FillCalleeSaves(const StackVisitor& fr) {
  mirror::ArtMethod* method = fr.GetMethod();
  uint32_t core_spills = method->GetCoreSpillMask();
  size_t spill_count = __builtin_popcount(core_spills);
  DCHECK_EQ(method->GetFpSpillMask(), 0u);
  size_t frame_size = method->GetFrameSizeInBytes();
  if (spill_count > 0) {
    // Lowest number spill is farthest away, walk registers and fill into context.
    int j = 2;  // Offset j to skip return address spill.
    for (int i = 0; i < kNumberOfCpuRegisters; i++) {
      if (((core_spills >> i) & 1) != 0) {
        gprs_[i] = fr.CalleeSaveAddress(spill_count - j, frame_size);
        j++;
      }
    }
  }
}

void X86_64Context::SmashCallerSaves() {
  // This needs to be 0 because we want a null/zero return value.
  gprs_[RAX] = const_cast<uintptr_t*>(&gZero);
  gprs_[RDX] = const_cast<uintptr_t*>(&gZero);
  gprs_[RCX] = nullptr;
  gprs_[RBX] = nullptr;
}

void X86_64Context::SetGPR(uint32_t reg, uintptr_t value) {
  CHECK_LT(reg, static_cast<uint32_t>(kNumberOfCpuRegisters));
  CHECK_NE(gprs_[reg], &gZero);
  CHECK(gprs_[reg] != NULL);
  *gprs_[reg] = value;
}

void X86_64Context::DoLongJump() {
  UNIMPLEMENTED(FATAL);
}

}  // namespace x86_64
}  // namespace art
