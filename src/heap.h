// Copyright 2011 Google Inc. All Rights Reserved.
// Author: cshapiro@google.com (Carl Shapiro)

#ifndef ART_SRC_HEAP_H_
#define ART_SRC_HEAP_H_

#include "src/globals.h"
#include "src/object.h"

namespace art {

class Heap {
 public:
  static Class* AllocClass(size_t size) {
    byte* raw = new byte[size];
    memset(raw, 0, size);
    return reinterpret_cast<Class*>(raw);
  }
};

}  // namespace art

#endif  // ART_SRC_HEAP_H_
