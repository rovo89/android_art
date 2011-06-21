// Copyright 2011 Google Inc. All Rights Reserved.

#include <stdint.h>
#include <string.h>
#include "src/globals.h"
#include "src/logging.h"
#include "src/memory_region.h"

namespace android {
namespace runtime {

void MemoryRegion::CopyFrom(size_t offset, const MemoryRegion& from) const {
  CHECK_NE(from.pointer(), NULL);
  CHECK_GT(from.size(), 0);
  CHECK_GE(this->size(), from.size());
  CHECK_LE(offset, this->size() - from.size());
  memmove(reinterpret_cast<void*>(start() + offset),
          from.pointer(), from.size());
}

} }  // namespace android::runtime
