// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_STRUTIL_H_
#define ART_SRC_STRUTIL_H_

#include <string.h>

namespace art {

// Key comparison function for C strings.
struct CStringLt {
  bool operator()(const char* s1, const char* s2) const {
    return strcmp(s1, s2) < 0;
  }
};

// Key equality function for C strings.
struct CStringEq {
  bool operator()(const char* s1, const char* s2) const {
    return strcmp(s1, s2) == 0;
  }
};

}  // namespace art

#endif  // ART_SRC_STRUTIL_H_
