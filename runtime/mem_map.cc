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

#include <backtrace/backtrace.h>

#include "base/stringprintf.h"
#include "ScopedFd.h"
#include "utils.h"

#define USE_ASHMEM 1

#ifdef USE_ASHMEM
#include <cutils/ashmem.h>
#endif

namespace art {

#if !defined(NDEBUG)

static std::ostream& operator<<(std::ostream& os, backtrace_map_info_t* rhs) {
  for (backtrace_map_info_t* m = rhs; m != NULL; m = m->next) {
    os << StringPrintf("0x%08x-0x%08x %c%c %s\n",
                       static_cast<uint32_t>(m->start),
                       static_cast<uint32_t>(m->end),
                       m->is_readable ? 'r' : '-', m->is_executable ? 'x' : '-', m->name);
  }
  return os;
}

static void CheckMapRequest(byte* addr, size_t byte_count) {
  if (addr == NULL) {
    return;
  }

  uint32_t base = reinterpret_cast<size_t>(addr);
  uint32_t limit = base + byte_count;

  backtrace_map_info_t* map_info_list = backtrace_create_map_info_list(getpid());
  for (backtrace_map_info_t* m = map_info_list; m != NULL; m = m->next) {
    CHECK(!(base >= m->start && base < m->end)     // start of new within old
        && !(limit > m->start && limit < m->end)   // end of new within old
        && !(base <= m->start && limit > m->end))  // start/end of new includes all of old
        << StringPrintf("Requested region 0x%08x-0x%08x overlaps with existing map 0x%08x-0x%08x (%s)\n",
                        base, limit,
                        static_cast<uint32_t>(m->start), static_cast<uint32_t>(m->end), m->name)
        << map_info_list;
  }
  backtrace_destroy_map_info_list(map_info_list);
}

#else
static void CheckMapRequest(byte*, size_t) { }
#endif

MemMap* MemMap::MapAnonymous(const char* name, byte* addr, size_t byte_count, int prot,
                             std::string* error_msg) {
  if (byte_count == 0) {
    return new MemMap(name, NULL, 0, NULL, 0, prot);
  }
  size_t page_aligned_byte_count = RoundUp(byte_count, kPageSize);
  CheckMapRequest(addr, page_aligned_byte_count);

#ifdef USE_ASHMEM
  // android_os_Debug.cpp read_mapinfo assumes all ashmem regions associated with the VM are
  // prefixed "dalvik-".
  std::string debug_friendly_name("dalvik-");
  debug_friendly_name += name;
  ScopedFd fd(ashmem_create_region(debug_friendly_name.c_str(), page_aligned_byte_count));
  int flags = MAP_PRIVATE;
  if (fd.get() == -1) {
    *error_msg = StringPrintf("ashmem_create_region failed for '%s': %s", name, strerror(errno));
    return nullptr;
  }
#else
  ScopedFd fd(-1);
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#endif

  byte* actual = reinterpret_cast<byte*>(mmap(addr, page_aligned_byte_count, prot, flags, fd.get(), 0));
  if (actual == MAP_FAILED) {
    std::string maps;
    ReadFileToString("/proc/self/maps", &maps);
    *error_msg = StringPrintf("anonymous mmap(%p, %zd, %x, %x, %d, 0) failed\n%s",
                              addr, page_aligned_byte_count, prot, flags, fd.get(),
                              maps.c_str());
    return nullptr;
  }
  return new MemMap(name, actual, byte_count, actual, page_aligned_byte_count, prot);
}

MemMap* MemMap::MapFileAtAddress(byte* addr, size_t byte_count, int prot, int flags, int fd,
                                 off_t start, bool reuse, const char* filename,
                                 std::string* error_msg) {
  CHECK_NE(0, prot);
  CHECK_NE(0, flags & (MAP_SHARED | MAP_PRIVATE));
  if (byte_count == 0) {
    return new MemMap("file", NULL, 0, NULL, 0, prot);
  }
  // Adjust 'offset' to be page-aligned as required by mmap.
  int page_offset = start % kPageSize;
  off_t page_aligned_offset = start - page_offset;
  // Adjust 'byte_count' to be page-aligned as we will map this anyway.
  size_t page_aligned_byte_count = RoundUp(byte_count + page_offset, kPageSize);
  // The 'addr' is modified (if specified, ie non-null) to be page aligned to the file but not
  // necessarily to virtual memory. mmap will page align 'addr' for us.
  byte* page_aligned_addr = (addr == NULL) ? NULL : (addr - page_offset);
  if (!reuse) {
    // reuse means it is okay that it overlaps an existing page mapping.
    // Only use this if you actually made the page reservation yourself.
    CheckMapRequest(page_aligned_addr, page_aligned_byte_count);
  } else {
    CHECK(addr != NULL);
  }
  byte* actual = reinterpret_cast<byte*>(mmap(page_aligned_addr,
                                              page_aligned_byte_count,
                                              prot,
                                              flags,
                                              fd,
                                              page_aligned_offset));
  if (actual == MAP_FAILED) {
    std::string strerr(strerror(errno));
    std::string maps;
    ReadFileToString("/proc/self/maps", &maps);
    *error_msg = StringPrintf("mmap(%p, %zd, %x, %x, %d, %lld) of file '%s' failed: %s\n%s",
                              page_aligned_addr, page_aligned_byte_count, prot, flags, fd,
                              static_cast<int64_t>(page_aligned_offset), filename, strerr.c_str(),
                              maps.c_str());
    return NULL;
  }
  return new MemMap("file", actual + page_offset, byte_count, actual, page_aligned_byte_count,
                    prot);
}

MemMap::~MemMap() {
  if (base_begin_ == NULL && base_size_ == 0) {
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
    CHECK(begin_ == NULL);
    CHECK(base_begin_ == NULL);
    CHECK_EQ(base_size_, 0U);
  } else {
    CHECK(begin_ != NULL);
    CHECK(base_begin_ != NULL);
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
    return new MemMap(tail_name, NULL, 0, NULL, 0, tail_prot);
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
  int flags = MAP_PRIVATE;
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
    *error_msg = StringPrintf("anonymous mmap(%p, %zd, %x, %x, %d, 0) failed\n%s",
                              tail_base_begin, tail_base_size, tail_prot, flags, fd.get(),
                              maps.c_str());
    return nullptr;
  }
  return new MemMap(tail_name, actual, tail_size, actual, tail_base_size, tail_prot);
}

bool MemMap::Protect(int prot) {
  if (base_begin_ == NULL && base_size_ == 0) {
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

}  // namespace art
