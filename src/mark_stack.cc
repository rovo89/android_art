// Copyright 2011 Google Inc. All Rights Reserved.

#include "mark_stack.h"

#include <sys/mman.h>

#include "globals.h"
#include "logging.h"
#include "scoped_ptr.h"

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
  mem_map_.reset(MemMap::Map(length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS));
  if (mem_map_ == NULL) {
    PLOG(ERROR) << "mmap failed";
    return false;
  }
  byte* addr = mem_map_->GetAddress();
  base_ = reinterpret_cast<const Object**>(addr);
  limit_ = reinterpret_cast<const Object**>(addr + length);
  ptr_ = reinterpret_cast<Object const**>(addr);
  int result = madvise(addr, length, MADV_DONTNEED);
  if (result == -1) {
    PLOG(WARNING) << "madvise failed";
  }
  return true;
}

MarkStack::~MarkStack() {}

}  // namespace art
