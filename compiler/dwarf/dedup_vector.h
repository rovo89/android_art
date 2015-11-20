/*
 * Copyright (C) 2015 The Android Open Source Project
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

#ifndef ART_COMPILER_DWARF_DEDUP_VECTOR_H_
#define ART_COMPILER_DWARF_DEDUP_VECTOR_H_

#include <vector>
#include <unordered_map>

namespace art {
namespace dwarf {
  class DedupVector {
   public:
    // Returns an offset to previously inserted identical block of data,
    // or appends the data at the end of the vector and returns offset to it.
    size_t Insert(const uint8_t* ptr, size_t num_bytes) {
      // See http://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function
      uint32_t hash = 2166136261u;
      for (size_t i = 0; i < num_bytes; i++) {
        hash = (hash ^ ptr[i]) * 16777619u;
      }
      // Try to find existing copy of the data.
      const auto& range = hash_to_offset_.equal_range(hash);
      for (auto it = range.first; it != range.second; ++it) {
        const size_t offset = it->second;
        if (offset + num_bytes <= vector_.size() &&
            memcmp(vector_.data() + offset, ptr, num_bytes) == 0) {
          return offset;
        }
      }
      // Append the data at the end of the vector.
      const size_t new_offset = vector_.size();
      hash_to_offset_.emplace(hash, new_offset);
      vector_.insert(vector_.end(), ptr, ptr + num_bytes);
      return new_offset;
    }

    const std::vector<uint8_t>& Data() const { return vector_; }

   private:
    struct IdentityHash {
      size_t operator()(uint32_t v) const { return v; }
    };

    // We store the full hash as the key to simplify growing of the table.
    // It avoids storing or referencing the actual data in the hash-table.
    std::unordered_multimap<uint32_t, size_t, IdentityHash> hash_to_offset_;

    std::vector<uint8_t> vector_;
  };
}  // namespace dwarf
}  // namespace art

#endif  // ART_COMPILER_DWARF_DEDUP_VECTOR_H_
