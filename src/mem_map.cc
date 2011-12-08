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

#include <sys/mman.h>

#include "ScopedFd.h"
#include "utils.h"

#define USE_ASHMEM 1

#ifdef USE_ASHMEM
#include <cutils/ashmem.h>
#endif

namespace art {

size_t ParseHex(const std::string& string) {
  CHECK_EQ(8U, string.size());
  const char* str = string.c_str();
  char* end;
  size_t value = strtoul(str, &end, 16);
  CHECK(end != str) << "Failed to parse hexadecimal value from " << string;
  CHECK_EQ(*end, '\0') << "Failed to parse hexadecimal value from " << string;
  return value;
}

void CheckMapRequest(byte* addr, size_t length) {
#ifndef NDEBUG
  if (addr == NULL) {
    return;
  }
  size_t base = reinterpret_cast<size_t>(addr);
  size_t limit = base + length;

  std::string maps;
  bool read = ReadFileToString("/proc/self/maps", &maps);
  if (!read) {
    PLOG(FATAL) << "Failed to read /proc/self/maps";
  }
  // Quick and dirty parse of output like shown below. We only focus
  // on grabbing the two 32-bit hex values at the start of each line
  // and will fail on wider addresses found on 64-bit systems.

  // 00008000-0001f000 r-xp 00000000 b3:01 273        /system/bin/toolbox
  // 0001f000-00021000 rw-p 00017000 b3:01 273        /system/bin/toolbox
  // 00021000-00029000 rw-p 00000000 00:00 0          [heap]
  // 40011000-40053000 r-xp 00000000 b3:01 1050       /system/lib/libc.so
  // 40053000-40056000 rw-p 00042000 b3:01 1050       /system/lib/libc.so
  // 40056000-40061000 rw-p 00000000 00:00 0
  // 40061000-40063000 r-xp 00000000 b3:01 1107       /system/lib/libusbhost.so
  // 40063000-40064000 rw-p 00002000 b3:01 1107       /system/lib/libusbhost.so
  // 4009d000-400a0000 r-xp 00000000 b3:01 1022       /system/lib/liblog.so
  // 400a0000-400a1000 rw-p 00003000 b3:01 1022       /system/lib/liblog.so
  // 400b7000-400cc000 r-xp 00000000 b3:01 932        /system/lib/libm.so
  // 400cc000-400cd000 rw-p 00015000 b3:01 932        /system/lib/libm.so
  // 400cf000-400d0000 r--p 00000000 00:00 0
  // 400e4000-400ec000 r--s 00000000 00:0b 388        /dev/__properties__ (deleted)
  // 400ec000-400fa000 r-xp 00000000 b3:01 1101       /system/lib/libcutils.so
  // 400fa000-400fb000 rw-p 0000e000 b3:01 1101       /system/lib/libcutils.so
  // 400fb000-4010a000 rw-p 00000000 00:00 0
  // 4010d000-4010e000 r-xp 00000000 b3:01 929        /system/lib/libstdc++.so
  // 4010e000-4010f000 rw-p 00001000 b3:01 929        /system/lib/libstdc++.so
  // b0001000-b0009000 r-xp 00001000 b3:01 1098       /system/bin/linker
  // b0009000-b000a000 rw-p 00009000 b3:01 1098       /system/bin/linker
  // b000a000-b0015000 rw-p 00000000 00:00 0
  // bee35000-bee56000 rw-p 00000000 00:00 0          [stack]
  // ffff0000-ffff1000 r-xp 00000000 00:00 0          [vectors]

  for (size_t i = 0; i < maps.size(); i++) {
    size_t remaining = maps.size() - i;
    if (remaining < 8+1+8) {  // 00008000-0001f000
      LOG(FATAL) << "Failed to parse at pos " << i << "\n" << maps;
    }
    std::string start_str(maps.substr(i, 8));
    std::string end_str(maps.substr(i+1+8, 8));
    uint32_t start = ParseHex(start_str);
    uint32_t end = ParseHex(end_str);
    CHECK(!(base >= start && base < end)       // start of new within old
          && !(limit > start && limit < end)  // end of new within old
          && !(base <= start && limit > end))  // start/end of new includes all of old
        << StringPrintf("Requested region %08x-%08x overlaps with existing map %08x-%08x\n",
                        base, limit, start, end)
        << maps;
    i += 8+1+8;
    i = maps.find('\n', i);
    CHECK(i != std::string::npos) << "Failed to find newline from pos " << i << "\n" << maps;
  }
#endif
}

MemMap* MemMap::MapAnonymous(const char* name, byte* addr, size_t length, int prot) {
  CHECK_NE(0U, length);
  CHECK_NE(0, prot);
  size_t page_aligned_size = RoundUp(length, kPageSize);
  CheckMapRequest(addr, page_aligned_size);

#ifdef USE_ASHMEM
  ScopedFd fd(ashmem_create_region(name, page_aligned_size));
  int flags = MAP_PRIVATE;
  if (fd.get() == -1) {
    PLOG(ERROR) << "ashmem_create_region failed (" << name << ")";
    return NULL;
  }
#else
  ScopedFd fd(-1);
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#endif

  byte* actual = reinterpret_cast<byte*>(mmap(addr, page_aligned_size, prot, flags, fd.get(), 0));
  if (actual == MAP_FAILED) {
    PLOG(ERROR) << "mmap failed (" << name << ")";
    return NULL;
  }
  return new MemMap(actual, length, actual, page_aligned_size);
}

MemMap* MemMap::MapFileAtAddress(byte* addr, size_t length, int prot, int flags, int fd, off_t start) {
  CHECK_NE(0U, length);
  CHECK_NE(0, prot);
  CHECK_NE(0, flags & (MAP_SHARED | MAP_PRIVATE));
  // adjust to be page-aligned
  int page_offset = start % kPageSize;
  off_t page_aligned_offset = start - page_offset;
  size_t page_aligned_size = RoundUp(length + page_offset, kPageSize);
  CheckMapRequest(addr, page_aligned_size);
  byte* actual = reinterpret_cast<byte*>(mmap(addr,
                                              page_aligned_size,
                                              prot,
                                              flags,
                                              fd,
                                              page_aligned_offset));
  if (actual == MAP_FAILED) {
    PLOG(ERROR) << "mmap failed";
    return NULL;
  }
  return new MemMap(actual + page_offset, length, actual, page_aligned_size);
}

MemMap::~MemMap() {
  if (base_addr_ == NULL && base_length_ == 0) {
    return;
  }
  int result = munmap(base_addr_, base_length_);
  base_addr_ = NULL;
  base_length_ = 0;
  if (result == -1) {
    PLOG(FATAL) << "munmap failed";
  }
}

MemMap::MemMap(byte* addr, size_t length, void* base_addr, size_t base_length)
    : addr_(addr), length_(length), base_addr_(base_addr), base_length_(base_length) {
  CHECK(addr_ != NULL);
  CHECK_NE(length_, 0U);
  CHECK(base_addr_ != NULL);
  CHECK_NE(base_length_, 0U);
};

}  // namespace art
