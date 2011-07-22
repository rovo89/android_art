// Copyright 2011 Google Inc. All Rights Reserved.
// Author: cshapiro@google.com (Carl Shapiro)

#include "src/space.h"

#include <sys/mman.h>

#include "src/logging.h"
#include "src/mspace.h"
#include "src/scoped_ptr.h"
#include "src/utils.h"

namespace art {

Space* Space::Create(size_t startup_size, size_t maximum_size) {
  scoped_ptr<Space> space(new Space(startup_size, maximum_size));
  bool success = space->Init();
  if (!success) {
    return NULL;
  } else {
    return space.release();
  }
}

void* Space::CreateMallocSpace(void* base,
                               size_t startup_size,
                               size_t maximum_size) {
  errno = 0;
  bool is_locked = false;
  size_t commit_size = startup_size / 2;
  void* msp = create_contiguous_mspace_with_base(commit_size, maximum_size,
                                                 is_locked, base);
  if (msp != NULL) {
    // Do not permit the heap grow past the starting size without our
    // intervention.
    mspace_set_max_allowed_footprint(msp, startup_size);
  } else {
    // There is no guarantee that errno has meaning when the call
    // fails, but it often does.
    PLOG(ERROR) << "create_contiguous_mspace_with_base failed";
  }
  return msp;
}

bool Space::Init() {
  if (!(startup_size_ <= maximum_size_)) {
    return false;
  }
  size_t length = RoundUp(maximum_size_, 4096);
  int prot = PROT_READ | PROT_WRITE;
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
  void* base = mmap(NULL, length, prot, flags, -1, 0);
  if (base == MAP_FAILED) {
    PLOG(ERROR) << "mmap failed";
    return false;
  }
  base_ = static_cast<byte*>(base);
  limit_ = base_ + length;
  mspace_ = CreateMallocSpace(base, startup_size_, maximum_size_);
  if (mspace_ == NULL) {
    munmap(base_, length);
    return false;
  }
  return true;
}

Space::~Space() {
  if (base_ == NULL) {
    return;
  }
  int result = munmap(base_, limit_ - base_);
  if (result == -1) {
    PLOG(WARNING) << "munmap failed";
  }
}

Object* Space::AllocWithoutGrowth(size_t num_bytes) {
  return reinterpret_cast<Object*>(mspace_calloc(mspace_, 1, num_bytes));
}

Object* Space::AllocWithGrowth(size_t num_bytes) {
  // Grow as much as possible within the mspace.
  size_t max_allowed = maximum_size_;
  mspace_set_max_allowed_footprint(mspace_, max_allowed);
  // Try the allocation.
  void* ptr = AllocWithoutGrowth(num_bytes);
  // Shrink back down as small as possible.
  size_t footprint = mspace_footprint(mspace_);
  mspace_set_max_allowed_footprint(mspace_, footprint);
  // Return the new allocation or NULL.
  return reinterpret_cast<Object*>(ptr);
}

size_t Space::Free(void* ptr) {
  DCHECK(ptr != NULL);
  size_t num_bytes = mspace_usable_size(mspace_, ptr);
  mspace_free(mspace_, ptr);
  return num_bytes;
}

void Space::DontNeed(void* start, void* end, void* num_bytes) {
  start = (void*)RoundUp((uintptr_t)start, 4096);
  end = (void*)RoundDown((uintptr_t)end, 4096);
  if (start >= end) {
    return;
  }
  size_t length = reinterpret_cast<byte*>(end) - reinterpret_cast<byte*>(start);
  int result = madvise(start, length, MADV_DONTNEED);
  if (result == -1) {
    PLOG(WARNING) << "madvise failed";
  } else {
    *reinterpret_cast<size_t*>(num_bytes) += length;
  }
}

void Space::Trim() {
  CHECK(mspace_ != NULL);
  mspace_trim(mspace_, 0);
  size_t num_bytes_released = 0;
  mspace_walk_free_pages(mspace_, DontNeed, &num_bytes_released);
}

size_t Space::MaxAllowedFootprint() {
  return mspace_max_allowed_footprint(mspace_);
}

void Space::Grow(size_t new_size) {
}

}  // namespace art
