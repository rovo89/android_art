// Copyright 2011 Google Inc. All Rights Reserved.

#include "context_arm.h"

#include "object.h"

namespace art {
namespace arm {

ArmContext::ArmContext() {
#ifndef NDEBUG
  for (int i=0; i < 16; i++) {
    gprs_[i] = 0xEBAD6070+i;
  }
#endif
  memset(fprs_, 0, sizeof(fprs_));
}

void ArmContext::FillCalleeSaves(const Frame& fr) {
  Method* method = fr.GetMethod();
  uint32_t core_spills = method->GetCoreSpillMask();
  size_t spill_count = __builtin_popcount(core_spills);
  CHECK_EQ(method->GetFpSpillMask(), 0u);
  if (spill_count > 0) {
    // Lowest number spill is furthest away, walk registers and fill into context
    int j = 1;
    for(int i = 0; i < 16; i++) {
      if (((core_spills >> i) & 1) != 0) {
        gprs_[i] = fr.LoadCalleeSave(spill_count - j);
        j++;
      }
    }
  }
}

void ArmContext::DoLongJump() {
#if defined(__arm__)
  // TODO: Load all GPRs and FPRs, currently the code restores registers R4 to PC
  asm volatile ( "mov %%r0, %0\n"
                 "mov %%r1, %1\n"
                 "ldm %%r0, {%%r4, %%r5, %%r6, %%r7,%%r8,%%r9,%%r10,%%r11,%%r12,%%r13,%%r14}\n"
                 "mov %%pc,%%r1\n"
      : // output
      : "r"(&gprs_[4]), "r"(gprs_[R15])  // input
#if 0  // TODO: FPRs..
        "w0" (fprs_[0] ), "w1" (fprs_[1] ), "w2" (fprs_[2] ), "w3" (fprs_[3]),
        "w4" (fprs_[4] ), "w5" (fprs_[5] ), "w6" (fprs_[6] ), "w7" (fprs_[7]),
        "w8" (fprs_[8] ), "w9" (fprs_[9] ), "w10"(fprs_[10]), "w11"(fprs_[11]),
        "w12"(fprs_[12]), "w13"(fprs_[13]), "w14"(fprs_[14]), "w15"(fprs_[15]),
        "w16"(fprs_[16]), "w17"(fprs_[17]), "w18"(fprs_[18]), "w19"(fprs_[19]),
        "w20"(fprs_[20]), "w21"(fprs_[21]), "w22"(fprs_[22]), "w23"(fprs_[23]),
        "w24"(fprs_[24]), "w25"(fprs_[25]), "w26"(fprs_[26]), "w27"(fprs_[27]),
        "w28"(fprs_[28]), "w29"(fprs_[29]), "w30"(fprs_[30]), "w31"(fprs_[31])
#endif
      :);  // clobber
#else
  UNIMPLEMENTED(FATAL);
#endif
}

}  // namespace arm
}  // namespace art
