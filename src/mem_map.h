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

#ifndef ART_SRC_MEM_MAP_H_
#define ART_SRC_MEM_MAP_H_

#include <stddef.h>
#include <sys/types.h>

#include "globals.h"

namespace art {

// Used to keep track of mmap segments.
class MemMap {
 public:

  // Request an anonymous region of a specified length and a requested base address.
  // Use NULL as the requested base address if you don't care.
  //
  // The word "anonymous" in this context means "not backed by a file". The supplied
  // 'ashmem_name' will be used -- on systems that support it -- to give the mapping
  // a name.
  //
  // On success, returns returns a MemMap instance.  On failure, returns a NULL;
  static MemMap* Map(const char* ashmem_name, byte* addr, size_t length, int prot);

  // Map part of a file, taking care of non-page aligned offsets.  The
  // "start" offset is absolute, not relative.
  //
  // On success, returns returns a MemMap instance.  On failure, returns a NULL;
  static MemMap* Map(size_t length, int prot, int flags, int fd, off_t start) {
    return Map(NULL, length, prot, flags, fd, start);
  }

  // Map part of a file, taking care of non-page aligned offsets.  The
  // "start" offset is absolute, not relative. This version allows
  // requesting a specific address for the base of the mapping.
  //
  // On success, returns returns a MemMap instance.  On failure, returns a NULL;
  static MemMap* Map(byte* addr, size_t length, int prot, int flags, int fd, off_t start);

  // Releases the memory mapping
  ~MemMap();

  byte* GetAddress() const {
    return addr_;
  }

  size_t GetLength() const {
    return length_;
  }

  byte* GetLimit() const {
    return addr_ + length_;
  }

 private:
  MemMap(byte* addr, size_t length, void* base_addr, size_t base_length);

  byte*  addr_;              // start of data
  size_t length_;            // length of data

  void*  base_addr_;         // page-aligned base address
  size_t base_length_;       // length of mapping
};

}  // namespace art

#endif  // ART_SRC_MEM_MAP_H_
