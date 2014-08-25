/*
 * Copyright (C) 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "allocator.h"

#include <inttypes.h>
#include <stdlib.h>

#include "atomic.h"
#include "base/logging.h"
#include "thread-inl.h"

namespace art {

Atomic<uint64_t> TrackedAllocators::bytes_used_[kAllocatorTagCount];
Atomic<uint64_t> TrackedAllocators::max_bytes_used_[kAllocatorTagCount];
Atomic<uint64_t> TrackedAllocators::total_bytes_used_[kAllocatorTagCount];

class MallocAllocator : public Allocator {
 public:
  explicit MallocAllocator() {}
  ~MallocAllocator() {}

  virtual void* Alloc(size_t size) {
    return calloc(sizeof(uint8_t), size);
  }

  virtual void Free(void* p) {
    free(p);
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(MallocAllocator);
};

MallocAllocator g_malloc_allocator;

class NoopAllocator : public Allocator {
 public:
  explicit NoopAllocator() {}
  ~NoopAllocator() {}

  virtual void* Alloc(size_t size) {
    LOG(FATAL) << "NoopAllocator::Alloc should not be called";
    return NULL;
  }

  virtual void Free(void* p) {
    // Noop.
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(NoopAllocator);
};

NoopAllocator g_noop_allocator;

Allocator* Allocator::GetMallocAllocator() {
  return &g_malloc_allocator;
}

Allocator* Allocator::GetNoopAllocator() {
  return &g_noop_allocator;
}

void TrackedAllocators::Dump(std::ostream& os) {
  if (kEnableTrackingAllocator) {
    os << "Dumping native memory usage\n";
    for (size_t i = 0; i < kAllocatorTagCount; ++i) {
      uint64_t bytes_used = bytes_used_[i].LoadRelaxed();
      uint64_t max_bytes_used = max_bytes_used_[i].LoadRelaxed();
      uint64_t total_bytes_used = total_bytes_used_[i].LoadRelaxed();
      if (total_bytes_used != 0) {
        os << static_cast<AllocatorTag>(i) << " active=" << bytes_used << " max="
           << max_bytes_used << " total=" << total_bytes_used << "\n";
      }
    }
  }
}

}  // namespace art
