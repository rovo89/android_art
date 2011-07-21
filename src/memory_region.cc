// Copyright 2011 Google Inc. All Rights Reserved.

#include <stdint.h>
#include <string.h>
#include "globals.h"
#include "logging.h"
#include "memory_region.h"

namespace art {

void MemoryRegion::CopyFrom(size_t offset, const MemoryRegion& from) const {
  CHECK(from.pointer() != NULL);
  CHECK_GT(from.size(), 0U);
  CHECK_GE(this->size(), from.size());
  CHECK_LE(offset, this->size() - from.size());
  memmove(reinterpret_cast<void*>(start() + offset),
          from.pointer(), from.size());
}

}  // namespace art
