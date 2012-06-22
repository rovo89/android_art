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
  // Initialize registers with easy to spot debug values.
  for (int i = 0; i < 8; i++) {
    gprs_[i] = kBadGprBase + i;
  }
  eip_ = 0xEBAD601F;
#endif
}

void X86Context::FillCalleeSaves(const StackVisitor& fr) {
  Method* method = fr.GetMethod();
  uint32_t core_spills = method->GetCoreSpillMask();
  size_t spill_count = __builtin_popcount(core_spills);
  DCHECK_EQ(method->GetFpSpillMask(), 0u);
  size_t frame_size = method->GetFrameSizeInBytes();
  if (spill_count > 0) {
    // Lowest number spill is furthest away, walk registers and fill into context.
    int j = 2;  // Offset j to skip return address spill.
    for (int i = 0; i < 8; i++) {
      if (((core_spills >> i) & 1) != 0) {
        gprs_[i] = fr.LoadCalleeSave(spill_count - j, frame_size);
        j++;
      }
    }
  }
}

void X86Context::SmashCallerSaves() {
  gprs_[EAX] = 0; // This needs to be 0 because we want a null/zero return value.
  gprs_[ECX] = kBadGprBase + ECX;
  gprs_[EDX] = kBadGprBase + EDX;
  gprs_[EBX] = kBadGprBase + EBX;
}

void X86Context::DoLongJump() {
#if defined(__i386__)
  // We push all the registers using memory-memory pushes, we then pop-all to get the registers
  // set up, we then pop esp which will move us down the stack to the delivery address. At the frame
  // where the exception will be delivered, we push EIP so that the return will take us to the
  // correct delivery instruction.
  gprs_[ESP] -= 4;
  *(reinterpret_cast<uintptr_t*>(gprs_[ESP])) = eip_;
  __asm__ __volatile__(
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
