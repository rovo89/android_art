// Copyright 2011 Google Inc. All Rights Reserved.

#include "mark_stack.h"

#include <sys/mman.h>

#include "UniquePtr.h"
#include "globals.h"
#include "logging.h"

namespace art {

MarkStack* MarkStack::Create() {
  UniquePtr<MarkStack> mark_stack(new MarkStack);
  mark_stack->Init();
  return mark_stack.release();
}

void MarkStack::Init() {
  size_t length = 64 * MB;
  mem_map_.reset(MemMap::MapAnonymous("dalvik-mark-stack", NULL, length, PROT_READ | PROT_WRITE));
  CHECK(mem_map_.get() != NULL) << "MemMap::Map() failed; aborting";
  byte* addr = mem_map_->GetAddress();
  base_ = reinterpret_cast<const Object**>(addr);
  limit_ = reinterpret_cast<const Object**>(addr + length);
  ptr_ = reinterpret_cast<Object const**>(addr);
  int result = madvise(addr, length, MADV_DONTNEED);
  if (result == -1) {
    PLOG(WARNING) << "madvise failed";
  }
}

MarkStack::~MarkStack() {}

}  // namespace art
