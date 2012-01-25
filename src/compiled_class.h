// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_COMPILED_CLASS_H_
#define ART_SRC_COMPILED_CLASS_H_

#include "object.h"

namespace art {

class CompiledClass {
 public:
  explicit CompiledClass(Class::Status status) : status_(status) {}
  ~CompiledClass() {}
  Class::Status GetStatus() const {
    return status_;
  }
 private:
  const Class::Status status_;
};

}  // namespace art

#endif  // ART_SRC_COMPILED_CLASS_H_
