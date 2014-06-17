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

#include "mirror/art_method-inl.h"
#include "mirror/object-inl.h"
#include "quick/quick_method_frame_info.h"
#include "stack.h"

namespace art {
namespace x86_64 {

static const uintptr_t gZero = 0;

void X86_64Context::Reset() {
  for (size_t i = 0; i < kNumberOfCpuRegisters; ++i) {
    gprs_[i] = nullptr;
  }
  for (size_t i = 0; i < kNumberOfFloatRegisters; ++i) {
    fprs_[i] = nullptr;
  }
  gprs_[RSP] = &rsp_;
  // Initialize registers with easy to spot debug values.
  rsp_ = X86_64Context::kBadGprBase + RSP;
  rip_ = X86_64Context::kBadGprBase + kNumberOfCpuRegisters;
}

void X86_64Context::FillCalleeSaves(const StackVisitor& fr) {
  mirror::ArtMethod* method = fr.GetMethod();
  const QuickMethodFrameInfo frame_info = method->GetQuickFrameInfo();
  size_t spill_count = POPCOUNT(frame_info.CoreSpillMask());
  size_t fp_spill_count = POPCOUNT(frame_info.FpSpillMask());
  if (spill_count > 0) {
    // Lowest number spill is farthest away, walk registers and fill into context.
    size_t j = 2;  // Offset j to skip return address spill.
    for (size_t i = 0; i < kNumberOfCpuRegisters; ++i) {
      if (((frame_info.CoreSpillMask() >> i) & 1) != 0) {
        gprs_[i] = fr.CalleeSaveAddress(spill_count - j, frame_info.FrameSizeInBytes());
        j++;
      }
    }
  }
  if (fp_spill_count > 0) {
    // Lowest number spill is farthest away, walk registers and fill into context.
    size_t j = 2;  // Offset j to skip return address spill.
    for (size_t i = 0; i < kNumberOfFloatRegisters; ++i) {
      if (((frame_info.FpSpillMask() >> i) & 1) != 0) {
        fprs_[i] = fr.CalleeSaveAddress(spill_count + fp_spill_count - j,
                                        frame_info.FrameSizeInBytes());
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
  gprs_[RSI] = nullptr;
  gprs_[RDI] = nullptr;
  gprs_[R8] = nullptr;
  gprs_[R9] = nullptr;
  gprs_[R10] = nullptr;
  gprs_[R11] = nullptr;
}

void X86_64Context::SetGPR(uint32_t reg, uintptr_t value) {
  CHECK_LT(reg, static_cast<uint32_t>(kNumberOfCpuRegisters));
  CHECK_NE(gprs_[reg], &gZero);
  CHECK(gprs_[reg] != NULL);
  *gprs_[reg] = value;
}

void X86_64Context::DoLongJump() {
#if defined(__x86_64__)
  // Array of GPR values, filled from the context backward for the long jump pop. We add a slot at
  // the top for the stack pointer that doesn't get popped in a pop-all.
  volatile uintptr_t gprs[kNumberOfCpuRegisters + 1];
  for (size_t i = 0; i < kNumberOfCpuRegisters; ++i) {
    gprs[kNumberOfCpuRegisters - i - 1] = gprs_[i] != NULL ? *gprs_[i] : X86_64Context::kBadGprBase + i;
  }
  // We want to load the stack pointer one slot below so that the ret will pop eip.
  uintptr_t rsp = gprs[kNumberOfCpuRegisters - RSP - 1] - kWordSize;
  gprs[kNumberOfCpuRegisters] = rsp;
  *(reinterpret_cast<uintptr_t*>(rsp)) = rip_;
  __asm__ __volatile__(
      "movq %0, %%rsp\n\t"  // RSP points to gprs.
      "popq %%r15\n\t"       // Load all registers except RSP and RIP with values in gprs.
      "popq %%r14\n\t"
      "popq %%r13\n\t"
      "popq %%r12\n\t"
      "popq %%r11\n\t"
      "popq %%r10\n\t"
      "popq %%r9\n\t"
      "popq %%r8\n\t"
      "popq %%rdi\n\t"
      "popq %%rsi\n\t"
      "popq %%rbp\n\t"
      "addq $8, %%rsp\n\t"
      "popq %%rbx\n\t"
      "popq %%rdx\n\t"
      "popq %%rcx\n\t"
      "popq %%rax\n\t"
      "popq %%rsp\n\t"      // Load stack pointer.
      "ret\n\t"             // From higher in the stack pop rip.
      :  // output.
      : "g"(&gprs[0])  // input.
      :);  // clobber.
#else
  UNIMPLEMENTED(FATAL);
#endif
}

}  // namespace x86_64
}  // namespace art
