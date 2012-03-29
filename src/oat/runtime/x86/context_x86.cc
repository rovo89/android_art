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

#include "context_x86.h"

#include "object.h"

namespace art {
namespace x86 {

X86Context::X86Context() {
#ifndef NDEBUG
  // Initialize registers with easy to spot debug values
  for (int i = 0; i < 8; i++) {
    gprs_[i] = 0xEBAD6070+i;
  }
  eip_ = 0xEBAD601F;
#endif
}

void X86Context::FillCalleeSaves(const Frame& fr) {
  Method* method = fr.GetMethod();
  uint32_t core_spills = method->GetCoreSpillMask();
  size_t spill_count = __builtin_popcount(core_spills);
  CHECK_EQ(method->GetFpSpillMask(), 0u);
  if (spill_count > 0) {
    // Lowest number spill is furthest away, walk registers and fill into context
    int j = 1;
    for (int i = 0; i < 8; i++) {
      if (((core_spills >> i) & 1) != 0) {
        gprs_[i] = fr.LoadCalleeSave(spill_count - j);
        j++;
      }
    }
  }
}

void X86Context::DoLongJump() {
#if defined(__i386__)
  // Load ESP and EIP
  gprs_[ESP] -= 4;  // push EIP for return
  *(reinterpret_cast<uintptr_t*>(gprs_[ESP])) = eip_;
  asm volatile(
      "pushl %4\n\t"
      "pushl %0\n\t"
      "pushl %1\n\t"
      "pushl %2\n\t"
      "pushl %3\n\t"
      "pushl %4\n\t"
      "pushl %5\n\t"
      "pushl %6\n\t"
      "pushl %7\n\t"
      "popal\n\t"
      "popl %%esp\n\t"
      "ret\n\t"
      :  //output
      : "g"(gprs_[EAX]), "g"(gprs_[ECX]), "g"(gprs_[EDX]), "g"(gprs_[EBX]),
        "g"(gprs_[ESP]), "g"(gprs_[EBP]), "g"(gprs_[ESI]), "g"(gprs_[EDI])
      :);  // clobber
#else
    UNIMPLEMENTED(FATAL);
#endif
}

}  // namespace x86
}  // namespace art
