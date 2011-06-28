// Copyright 2011 Google Inc. All Rights Reserved.

#include "src/globals.h"
#include "src/object.h"
#include "src/logging.h"

namespace art {

uint32_t Method::NumArgRegisters() {
  CHECK(shorty_ != NULL);
  uint32_t num_registers = 0;
  for (size_t i = 1; shorty_[0] != '\0'; ++i) {
    char ch = shorty_[i];
    if (ch == 'D' || ch == 'J') {
      num_registers += 2;
    } else {
      num_registers += 1;
    }
  }
  return num_registers;
}

}  // namespace art
