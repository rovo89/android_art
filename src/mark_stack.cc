// Copyright 2011 Google Inc. All Rights Reserved.
// Author: cshapiro@google.com (Carl Shapiro)

#include "src/mark_stack.h"

#include <sys/mman.h>

#include "src/globals.h"
#include "src/logging.h"
#include "src/scoped_ptr.h"

namespace art {

MarkStack* MarkStack::Create(size_t maximum_size) {
  scoped_ptr<MarkStack> mark_stack(new MarkStack());
  bool success = mark_stack->Init(maximum_size);
  if (!success) {
    return NULL;
  } else {
    return mark_stack.release();
  }
}

bool MarkStack::Init(size_t maximum_size) {
  size_t length = 64 * MB;
  void* addr = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_PRIVATE, -1, 0);
  if (addr == MAP_FAILED) {
    PLOG(ERROR) << "mmap failed";
    return false;
  }
  base_ = reinterpret_cast<const Object**>(addr);
  limit_ = reinterpret_cast<const Object**>((byte*)addr + length);
  ptr_ = reinterpret_cast<Object const**>(addr);
  int result = madvise(addr, length, MADV_DONTNEED);
  if (result == -1) {
    PLOG(WARNING) << "madvise failed";
  }
  return true;
}

MarkStack::~MarkStack() {
  int result = munmap((void*)base_, limit_ - base_);
  if (result == -1) {
    PLOG(WARNING) << "munmap failed";
  }
}

}  // namespace art
