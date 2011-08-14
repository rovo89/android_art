// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_OFFSETS_H_
#define ART_SRC_OFFSETS_H_

#include <iostream>  // NOLINT
#include "globals.h"

namespace art {

// Allow the meaning of offsets to be strongly typed
class Offset {
 public:
  explicit Offset(size_t val) : val_(val) {}
  int32_t Int32Value() const {
    return static_cast<int32_t>(val_);
  }
  uint32_t Uint32Value() const {
    return static_cast<uint32_t>(val_);
  }
 protected:
  size_t val_;
};
std::ostream& operator<<(std::ostream& os, const Offset& offs);

// Offsets relative to the current frame
class FrameOffset : public Offset {
 public:
  explicit FrameOffset(size_t val) : Offset(val) {}
  bool operator>(FrameOffset other) const { return val_ > other.val_; }
  bool operator<(FrameOffset other) const { return val_ < other.val_; }
};

// Offsets relative to the current running thread
class ThreadOffset : public Offset {
 public:
  explicit ThreadOffset(size_t val) : Offset(val) {}
};

// Offsets relative to an object
class MemberOffset : public Offset {
 public:
  explicit MemberOffset(size_t val) : Offset(val) {}
};

}  // namespace art

#endif  // ART_SRC_OFFSETS_H_
