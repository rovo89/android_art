// Copyright 2011 Google Inc. All Rights Reserved.

#include "offsets.h"

#include <iostream>  // NOLINT

namespace art {

std::ostream& operator<<(std::ostream& os, const Offset& offs) {
  return os << offs.Int32Value();
}

}
