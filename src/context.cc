// Copyright 2011 Google Inc. All Rights Reserved.

#include "context.h"

#include "context_arm.h"
#include "context_x86.h"

namespace art {

Context* Context::Create() {
#if defined(__arm__)
  return new arm::ArmContext();
#else
  return new x86::X86Context();
#endif
}

}  // namespace art
