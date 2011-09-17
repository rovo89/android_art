// Copyright 2011 Google Inc. All Rights Reserved.

#include "context_x86.h"

#include "logging.h"

namespace art {
namespace x86 {

void X86Context::DoLongJump() {
#if defined(__i386__)
  // Load ESP and EIP
  asm volatile ( "movl %%esp, %0\n"
                 "jmp *%1"
      : // output
      : "m"(esp_), "r"(&eip_)  // input
      :);  // clobber
#else
  UNIMPLEMENTED(FATAL);
#endif
}

}  // namespace x86
}  // namespace art
