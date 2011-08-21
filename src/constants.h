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

#if defined(__i386__)
#include "constants_x86.h"
#elif defined(__arm__)
#include "constants_arm.h"
#endif

#endif  // ART_SRC_CONSTANTS_H_
