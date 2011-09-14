// Copyright 2011 Google Inc. All Rights Reserved.

#include "context_x86.h"

namespace art {
namespace x86 {

void X86Context::DoLongJump() {
  // Load ESP and EIP
  asm volatile ( "movl %%esp, %0\n"
                 "jmp *%1"
      : // output
      : "m"(esp_), "r"(&eip_)  // input
      :);  // clobber
}

}  // namespace x86
}  // namespace art
