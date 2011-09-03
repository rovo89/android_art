// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_CONSTANTS_H_
#define ART_SRC_CONSTANTS_H_

namespace art {

enum InstructionSet {
  kNone,
  kArm,
  kThumb2,
  kX86
};

}  // namespace art

#include "constants_x86.h"
#include "constants_arm.h"

#endif  // ART_SRC_CONSTANTS_H_
