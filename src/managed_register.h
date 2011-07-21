// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_MANAGED_REGISTER_H_
#define ART_SRC_MANAGED_REGISTER_H_

#if defined(__i386__)
#include "managed_register_x86.h"
#elif defined(__arm__)
#include "managed_register_arm.h"
#else
#error Unknown architecture.
#endif

#endif  // ART_SRC_MANAGED_REGISTER_H_
