// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_CASTS_H_
#define ART_SRC_CASTS_H_

#include <string.h>
#include "src/macros.h"

namespace art {

template <class Dest, class Source>
inline Dest bit_cast(const Source& source) {
  // Compile time assertion: sizeof(Dest) == sizeof(Source)
  // A compile error here means your Dest and Source have different sizes.
  COMPILE_ASSERT(sizeof(Dest) == sizeof(Source), verify_sizes_are_equal);
  Dest dest;
  memcpy(&dest, &source, sizeof(dest));
  return dest;
}

}  // namespace art

#endif  // ART_SRC_CASTS_H_
