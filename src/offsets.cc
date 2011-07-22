// Copyright 2011 Google Inc. All Rights Reserved.
// Author: cshapiro@google.com (Carl Shapiro)

#include "src/offsets.h"

#include <iostream>  // NOLINT

namespace art {

std::ostream& operator<<(std::ostream& os, const Offset& offs) {
  return os << offs.Int32Value();
}

}
