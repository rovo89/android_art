/*
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef ART_SRC_GC_MAP_H_
#define ART_SRC_GC_MAP_H_

#include "logging.h"
#include "macros.h"
#include <stdint.h>

namespace art {

// Lightweight wrapper for native PC offset to reference bit maps.
class NativePcOffsetToReferenceMap {
 public:
  NativePcOffsetToReferenceMap(const uint8_t* data) : data_(data) {
    CHECK(data_ != NULL);
  }

  // The number of entries in the table.
  size_t NumEntries() const {
    return data_[2] | (data_[3] << 8);
  }

  // Return address of bitmap encoding what are live references.
  const uint8_t* GetBitMap(size_t index) const {
    size_t entry_offset = index * EntryWidth();
    return &Table()[entry_offset + NativeOffsetWidth()];
  }

  // Get the native PC encoded in the table at the given index.
  uintptr_t GetNativePcOffset(size_t index) const {
    size_t entry_offset = index * EntryWidth();
    uintptr_t result = 0;
    for (size_t i = 0; i < NativeOffsetWidth(); ++i) {
      result |= Table()[entry_offset + i] << (i * 8);
    }
    return result;
  }

  // Finds the bitmap associated with the native pc offset.
  const uint8_t* FindBitMap(uintptr_t native_pc_offset) {
    size_t num_entries = NumEntries();
    size_t index = Hash(native_pc_offset) % num_entries;
    while (GetNativePcOffset(index) != native_pc_offset) {
      index = (index + 1) % num_entries;
    }
    return GetBitMap(index);
  }

  static uint32_t Hash(uint32_t native_offset) {
    uint32_t hash = native_offset;
    hash ^= (hash >> 20) ^ (hash >> 12);
    hash ^= (hash >> 7) ^ (hash >> 4);
    return hash;
  }

  // The number of bytes used to encode registers.
  size_t RegWidth() const {
    return data_[1];
  }

 private:
  // Skip the size information at the beginning of data.
  const uint8_t* Table() const {
    return data_ + 4;
  }

  // Number of bytes used to encode a native offset.
  size_t NativeOffsetWidth() const {
    return data_[0];
  }

  // The width of an entry in the table.
  size_t EntryWidth() const {
    return NativeOffsetWidth() + RegWidth();
  }

  const uint8_t* const data_;  // The header and table data
};

}  // namespace art

#endif  // ART_SRC_GC_MAP_H_
