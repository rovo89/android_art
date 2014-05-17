/*
 * Copyright (C) 2008 The Android Open Source Project
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

#include "mem_map.h"

#include <inttypes.h>
#include <backtrace/BacktraceMap.h>

#include "UniquePtrCompat.h"
#include "base/stringprintf.h"
#include "ScopedFd.h"
#include "utils.h"

#define USE_ASHMEM 1

#ifdef USE_ASHMEM
#include <cutils/ashmem.h>
#endif

namespace art {

static std::ostream& operator<<(
    std::ostream& os,
    std::pair<BacktraceMap::const_iterator, BacktraceMap::const_iterator> iters) {
  for (BacktraceMap::const_iterator it = iters.first; it != iters.second; ++it) {
    os << StringPrintf("0x%08x-0x%08x %c%c%c %s\n",
                       static_cast<uint32_t>(it->start),
                       static_cast<uint32_t>(it->end),
                       (it->flags & PROT_READ) ? 'r' : '-',
                       (it->flags & PROT_WRITE) ? 'w' : '-',
                       (it->flags & PROT_EXEC) ? 'x' : '-', it->name.c_str());
  }
  return os;
}

#if defined(__LP64__) && !defined(__x86_64__)
// Where to start with low memory allocation. The first 64KB is protected by SELinux.
static constexpr uintptr_t LOW_MEM_START = 64 * KB;

uintptr_t MemMap::next_mem_pos_ = LOW_MEM_START;   // first page to check for low-mem extent
#endif

static bool CheckMapRequest(byte* expected_ptr, void* actual_ptr, size_t byte_count,
                            std::ostringstream* error_msg) {
  // Handled first by caller for more specific error messages.
  CHECK(actual_ptr != MAP_FAILED);

  if (expected_ptr == nullptr) {
    return true;
  }

  if (expected_ptr == actual_ptr) {
    return true;
  }

  // We asked for an address but didn't get what we wanted, all paths below here should fail.
  int result = munmap(actual_ptr, byte_count);
  if (result == -1) {
    PLOG(WARNING) << StringPrintf("munmap(%p, %zd) failed", actual_ptr, byte_count);
  }

  uintptr_t actual = reinterpret_cast<uintptr_t>(actual_ptr);
  uintptr_t expected = reinterpret_cast<uintptr_t>(expected_ptr);
  uintptr_t limit = expected + byte_count;

  UniquePtr<BacktraceMap> map(BacktraceMap::Create(getpid()));
  if (!map->Build()) {
    *error_msg << StringPrintf("Failed to build process map to determine why mmap returned "
                               "0x%08" PRIxPTR " instead of 0x%08" PRIxPTR, actual, expected);

    return false;
  }
  for (BacktraceMap::const_iterator it = map->begin(); it != map->end(); ++it) {
    if ((expected >= it->start && expected < it->end)  // start of new within old
        || (limit > it->start && limit < it->end)      // end of new within old
        || (expected <= it->start && limit > it->end)) {  // start/end of new includes all of old
      *error_msg
          << StringPrintf("Requested region 0x%08" PRIxPTR "-0x%08" PRIxPTR " overlaps with "
                          "existing map 0x%08" PRIxPTR "-0x%08" PRIxPTR " (%s)\n",
                          expected, limit,
                          static_cast<uintptr_t>(it->start), static_cast<uintptr_t>(it->end),
                          it->name.c_str())
          << std::make_pair(it, map->end());
      return false;
    }
  }
  *error_msg << StringPrintf("Failed to mmap at expected address, mapped at "
                             "0x%08" PRIxPTR " instead of 0x%08" PRIxPTR, actual, expected);
  return false;
}

MemMap* MemMap::MapAnonymous(const char* name, byte* expected, size_t byte_count, int prot,
                             bool low_4gb, std::string* error_msg) {
  if (byte_count == 0) {
    return new MemMap(name, nullptr, 0, nullptr, 0, prot);
  }
  size_t page_aligned_byte_count = RoundUp(byte_count, kPageSize);

#ifdef USE_ASHMEM
  // android_os_Debug.cpp read_mapinfo assumes all ashmem regions associated with the VM are
  // prefixed "dalvik-".
  std::string debug_friendly_name("dalvik-");
  debug_friendly_name += name;
  ScopedFd fd(ashmem_create_region(debug_friendly_name.c_str(), page_aligned_byte_count));
  if (fd.get() == -1) {
    *error_msg = StringPrintf("ashmem_create_region failed for '%s': %s", name, strerror(errno));
    return nullptr;
  }
  int flags = MAP_PRIVATE;
#else
  ScopedFd fd(-1);
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#endif

  // We need to store and potentially set an error number for pretty printing of errors
  int saved_errno = 0;

#ifdef __LP64__
  // When requesting low_4g memory and having an expectation, the requested range should fit into
  // 4GB.
  if (low_4gb && (
      // Start out of bounds.
      (reinterpret_cast<uintptr_t>(expected) >> 32) != 0 ||
      // End out of bounds. For simplicity, this will fail for the last page of memory.
      (reinterpret_cast<uintptr_t>(expected + page_aligned_byte_count) >> 32) != 0)) {
    *error_msg = StringPrintf("The requested address space (%p, %p) cannot fit in low_4gb",
                              expected, expected + page_aligned_byte_count);
    return nullptr;
  }
#endif

  // TODO:
  // A page allocator would be a useful abstraction here, as
  // 1) It is doubtful that MAP_32BIT on x86_64 is doing the right job for us
  // 2) The linear scheme, even with simple saving of the last known position, is very crude
#if defined(__LP64__) && !defined(__x86_64__)
  // MAP_32BIT only available on x86_64.
  void* actual = MAP_FAILED;
  if (low_4gb && expected == nullptr) {
    flags |= MAP_FIXED;

    bool first_run = true;

    for (uintptr_t ptr = next_mem_pos_; ptr < 4 * GB; ptr += kPageSize) {
      if (4U * GB - ptr < page_aligned_byte_count) {
        // Not enough memory until 4GB.
        if (first_run) {
          // Try another time from the bottom;
          ptr = LOW_MEM_START - kPageSize;
          first_run = false;
          continue;
        } else {
          // Second try failed.
          break;
        }
      }

      uintptr_t tail_ptr;

      // Check pages are free.
      bool safe = true;
      for (tail_ptr = ptr; tail_ptr < ptr + page_aligned_byte_count; tail_ptr += kPageSize) {
        if (msync(reinterpret_cast<void*>(tail_ptr), kPageSize, 0) == 0) {
          safe = false;
          break;
        } else {
          DCHECK_EQ(errno, ENOMEM);
        }
      }

      next_mem_pos_ = tail_ptr;  // update early, as we break out when we found and mapped a region

      if (safe == true) {
        actual = mmap(reinterpret_cast<void*>(ptr), page_aligned_byte_count, prot, flags, fd.get(),
                      0);
        if (actual != MAP_FAILED) {
          break;
        }
      } else {
        // Skip over last page.
        ptr = tail_ptr;
      }
    }

    if (actual == MAP_FAILED) {
      LOG(ERROR) << "Could not find contiguous low-memory space.";
      saved_errno = ENOMEM;
    }
  } else {
    actual = mmap(expected, page_aligned_byte_count, prot, flags, fd.get(), 0);
    saved_errno = errno;
  }

#else
#ifdef __x86_64__
  if (low_4gb && expected == nullptr) {
    flags |= MAP_32BIT;
  }
#endif

  void* actual = mmap(expected, page_aligned_byte_count, prot, flags, fd.get(), 0);
  saved_errno = errno;
#endif

  if (actual == MAP_FAILED) {
    std::string maps;
    ReadFileToString("/proc/self/maps", &maps);

    *error_msg = StringPrintf("Failed anonymous mmap(%p, %zd, 0x%x, 0x%x, %d, 0): %s\n%s",
                              expected, page_aligned_byte_count, prot, flags, fd.get(),
                              strerror(saved_errno), maps.c_str());
    return nullptr;
  }
  std::ostringstream check_map_request_error_msg;
  if (!CheckMapRequest(expected, actual, page_aligned_byte_count, &check_map_request_error_msg)) {
    *error_msg = check_map_request_error_msg.str();
    return nullptr;
  }
  return new MemMap(name, reinterpret_cast<byte*>(actual), byte_count, actual,
                    page_aligned_byte_count, prot);
}

MemMap* MemMap::MapFileAtAddress(byte* expected, size_t byte_count, int prot, int flags, int fd,
                                 off_t start, bool reuse, const char* filename,
                                 std::string* error_msg) {
  CHECK_NE(0, prot);
  CHECK_NE(0, flags & (MAP_SHARED | MAP_PRIVATE));
  if (reuse) {
    // reuse means it is okay that it overlaps an existing page mapping.
    // Only use this if you actually made the page reservation yourself.
    CHECK(expected != nullptr);
    flags |= MAP_FIXED;
  } else {
    CHECK_EQ(0, flags & MAP_FIXED);
  }

  if (byte_count == 0) {
    return new MemMap(filename, nullptr, 0, nullptr, 0, prot);
  }
  // Adjust 'offset' to be page-aligned as required by mmap.
  int page_offset = start % kPageSize;
  off_t page_aligned_offset = start - page_offset;
  // Adjust 'byte_count' to be page-aligned as we will map this anyway.
  size_t page_aligned_byte_count = RoundUp(byte_count + page_offset, kPageSize);
  // The 'expected' is modified (if specified, ie non-null) to be page aligned to the file but not
  // necessarily to virtual memory. mmap will page align 'expected' for us.
  byte* page_aligned_expected = (expected == nullptr) ? nullptr : (expected - page_offset);

  byte* actual = reinterpret_cast<byte*>(mmap(page_aligned_expected,
                                              page_aligned_byte_count,
                                              prot,
                                              flags,
                                              fd,
                                              page_aligned_offset));
  if (actual == MAP_FAILED) {
    auto saved_errno = errno;

    std::string maps;
    ReadFileToString("/proc/self/maps", &maps);

    *error_msg = StringPrintf("mmap(%p, %zd, 0x%x, 0x%x, %d, %" PRId64
                              ") of file '%s' failed: %s\n%s",
                              page_aligned_expected, page_aligned_byte_count, prot, flags, fd,
                              static_cast<int64_t>(page_aligned_offset), filename,
                              strerror(saved_errno), maps.c_str());
    return nullptr;
  }
  std::ostringstream check_map_request_error_msg;
  if (!CheckMapRequest(expected, actual, page_aligned_byte_count, &check_map_request_error_msg)) {
    *error_msg = check_map_request_error_msg.str();
    return nullptr;
  }
  return new MemMap(filename, actual + page_offset, byte_count, actual, page_aligned_byte_count,
                    prot);
}

MemMap::~MemMap() {
  if (base_begin_ == nullptr && base_size_ == 0) {
    return;
  }
  int result = munmap(base_begin_, base_size_);
  if (result == -1) {
    PLOG(FATAL) << "munmap failed";
  }
}

MemMap::MemMap(const std::string& name, byte* begin, size_t size, void* base_begin,
               size_t base_size, int prot)
    : name_(name), begin_(begin), size_(size), base_begin_(base_begin), base_size_(base_size),
      prot_(prot) {
  if (size_ == 0) {
    CHECK(begin_ == nullptr);
    CHECK(base_begin_ == nullptr);
    CHECK_EQ(base_size_, 0U);
  } else {
    CHECK(begin_ != nullptr);
    CHECK(base_begin_ != nullptr);
    CHECK_NE(base_size_, 0U);
  }
};

MemMap* MemMap::RemapAtEnd(byte* new_end, const char* tail_name, int tail_prot,
                           std::string* error_msg) {
  DCHECK_GE(new_end, Begin());
  DCHECK_LE(new_end, End());
  DCHECK_LE(begin_ + size_, reinterpret_cast<byte*>(base_begin_) + base_size_);
  DCHECK(IsAligned<kPageSize>(begin_));
  DCHECK(IsAligned<kPageSize>(base_begin_));
  DCHECK(IsAligned<kPageSize>(reinterpret_cast<byte*>(base_begin_) + base_size_));
  DCHECK(IsAligned<kPageSize>(new_end));
  byte* old_end = begin_ + size_;
  byte* old_base_end = reinterpret_cast<byte*>(base_begin_) + base_size_;
  byte* new_base_end = new_end;
  DCHECK_LE(new_base_end, old_base_end);
  if (new_base_end == old_base_end) {
    return new MemMap(tail_name, nullptr, 0, nullptr, 0, tail_prot);
  }
  size_ = new_end - reinterpret_cast<byte*>(begin_);
  base_size_ = new_base_end - reinterpret_cast<byte*>(base_begin_);
  DCHECK_LE(begin_ + size_, reinterpret_cast<byte*>(base_begin_) + base_size_);
  size_t tail_size = old_end - new_end;
  byte* tail_base_begin = new_base_end;
  size_t tail_base_size = old_base_end - new_base_end;
  DCHECK_EQ(tail_base_begin + tail_base_size, old_base_end);
  DCHECK(IsAligned<kPageSize>(tail_base_size));

#ifdef USE_ASHMEM
  // android_os_Debug.cpp read_mapinfo assumes all ashmem regions associated with the VM are
  // prefixed "dalvik-".
  std::string debug_friendly_name("dalvik-");
  debug_friendly_name += tail_name;
  ScopedFd fd(ashmem_create_region(debug_friendly_name.c_str(), tail_base_size));
  int flags = MAP_PRIVATE | MAP_FIXED;
  if (fd.get() == -1) {
    *error_msg = StringPrintf("ashmem_create_region failed for '%s': %s",
                              tail_name, strerror(errno));
    return nullptr;
  }
#else
  ScopedFd fd(-1);
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#endif

  // Unmap/map the tail region.
  int result = munmap(tail_base_begin, tail_base_size);
  if (result == -1) {
    std::string maps;
    ReadFileToString("/proc/self/maps", &maps);
    *error_msg = StringPrintf("munmap(%p, %zd) failed for '%s'\n%s",
                              tail_base_begin, tail_base_size, name_.c_str(),
                              maps.c_str());
    return nullptr;
  }
  // Don't cause memory allocation between the munmap and the mmap
  // calls. Otherwise, libc (or something else) might take this memory
  // region. Note this isn't perfect as there's no way to prevent
  // other threads to try to take this memory region here.
  byte* actual = reinterpret_cast<byte*>(mmap(tail_base_begin, tail_base_size, tail_prot,
                                              flags, fd.get(), 0));
  if (actual == MAP_FAILED) {
    std::string maps;
    ReadFileToString("/proc/self/maps", &maps);
    *error_msg = StringPrintf("anonymous mmap(%p, %zd, 0x%x, 0x%x, %d, 0) failed\n%s",
                              tail_base_begin, tail_base_size, tail_prot, flags, fd.get(),
                              maps.c_str());
    return nullptr;
  }
  return new MemMap(tail_name, actual, tail_size, actual, tail_base_size, tail_prot);
}

bool MemMap::Protect(int prot) {
  if (base_begin_ == nullptr && base_size_ == 0) {
    prot_ = prot;
    return true;
  }

  if (mprotect(base_begin_, base_size_, prot) == 0) {
    prot_ = prot;
    return true;
  }

  PLOG(ERROR) << "mprotect(" << reinterpret_cast<void*>(base_begin_) << ", " << base_size_ << ", "
              << prot << ") failed";
  return false;
}

std::ostream& operator<<(std::ostream& os, const MemMap& mem_map) {
  os << StringPrintf("[MemMap: %s prot=0x%x %p-%p]",
                     mem_map.GetName().c_str(), mem_map.GetProtect(),
                     mem_map.BaseBegin(), mem_map.BaseEnd());
  return os;
}

}  // namespace art
