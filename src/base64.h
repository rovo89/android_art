// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_BASE64_H_
#define ART_SRC_BASE64_H_

#include "src/globals.h"

namespace art {

byte* DecodeBase64(const char* src, size_t size, size_t* dst_size);

}  // namespace art

#endif  // ART_SRC_BASE64_H_
