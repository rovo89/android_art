// Copyright 2011 Google Inc. All Rights Reserved.

#include "src/globals.h"
#include "src/object.h"
#include "src/logging.h"

#include <string.h>

namespace art {

bool Class::IsInSamePackage(const char* descriptor1, const char* descriptor2) {
  size_t i = 0;
  while (descriptor1[i] != '\0' && descriptor1[i] == descriptor2[i]) {
    ++i;
  }
  if (strrchr(descriptor1 + i, '/') != NULL ||
      strrchr(descriptor2 + i, '/') != NULL ) {
    return false;
  } else {
    return true;
  }
}

bool Class::IsInSamePackage(const Class* that) const {
  const Class* klass1 = this;
  const Class* klass2 = that;
  if (klass1 == klass2) {
    return true;
  }
  // Class loaders must match.
  if (klass1->class_loader_ != klass2->class_loader_) {
    return false;
  }
  // Arrays are in the same package when their element classes are.
  if (klass1->IsArray()) {
    klass1 = klass1->array_element_class_;
  }
  if (klass2->IsArray()) {
    klass2 = klass2->array_element_class_;
  }
  // Compare the package part of the descriptor string.
  return IsInSamePackage(klass1->descriptor_, klass2->descriptor_);
}

uint32_t Method::NumArgRegisters() {
  CHECK(shorty_ != NULL);
  uint32_t num_registers = 0;
  for (size_t i = 1; shorty_[0] != '\0'; ++i) {
    char ch = shorty_[i];
    if (ch == 'D' || ch == 'J') {
      num_registers += 2;
    } else {
      num_registers += 1;
    }
  }
  return num_registers;
}

}  // namespace art
