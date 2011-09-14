// Copyright 2011 Google Inc. All Rights Reserved.

#include "context_x86.h"

#include "object.h"

namespace art {
namespace x86 {

X86Context::X86Context() {
  for (int i=0; i < 8; i++) {
    gprs_[i] = 0xEBAD6070+i;
  }
}

void X86Context::FillCalleeSaves(const Frame& fr) {
  Method* method = fr.GetMethod();
  uint32_t core_spills = method->GetCoreSpillMask();
  size_t spill_count = __builtin_popcount(core_spills);
  CHECK_EQ(method->GetFpSpillMask(), 0u);
  if (spill_count > 0) {
    // Lowest number spill is furthest away, walk registers and fill into context
    int j = 1;
    for(int i = 0; i < 8; i++) {
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
  *((uintptr_t*)(gprs_[ESP])) = eip_;
  asm volatile (
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
