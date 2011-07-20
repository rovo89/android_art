// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_CONSTANTS_H_
#define ART_SRC_CONSTANTS_H_

#if defined(__i386__)
#include "src/constants_x86.h"
#elif defined(__arm__)
#include "src/constants_arm.h"
#endif

#endif  // ART_SRC_CONSTANTS_H_
