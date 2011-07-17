// Copyright 2011 Google Inc. All Rights Reserved.
// Author: enh@google.com (Elliott Hughes)

#include "runtime.h"

namespace art {

void Runtime::PlatformAbort(const char*, int) {
  // On a device, debuggerd will give us a stack trace. Nothing to do here.
}

}  // namespace art
