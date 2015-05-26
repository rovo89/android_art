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

#ifndef ART_COMPILER_UTILS_DEDUPE_SET_H_
#define ART_COMPILER_UTILS_DEDUPE_SET_H_

#include <algorithm>
#include <inttypes.h>
#include <memory>
#include <set>
#include <string>

#include "base/mutex.h"
#include "base/stl_util.h"
#include "base/stringprintf.h"
#include "base/time_utils.h"
#include "utils/swap_space.h"

namespace art {

// A set of Keys that support a HashFunc returning HashType. Used to find duplicates of Key in the
// Add method. The data-structure is thread-safe through the use of internal locks, it also
// supports the lock being sharded.
template <typename InKey, typename StoreKey, typename HashType, typename HashFunc,
          HashType kShard = 1>
class DedupeSet {
  typedef std::pair<HashType, const InKey*> HashedInKey;
  struct HashedKey {
    StoreKey* store_ptr;
    union {
      HashType store_hash;        // Valid if store_ptr != null.
      const HashedInKey* in_key;  // Valid if store_ptr == null.
    };
  };

  class Comparator {
   public:
    bool operator()(const HashedKey& a, const HashedKey& b) const {
      HashType a_hash = (a.store_ptr != nullptr) ? a.store_hash : a.in_key->first;
      HashType b_hash = (b.store_ptr != nullptr) ? b.store_hash : b.in_key->first;
      if (a_hash != b_hash) {
        return a_hash < b_hash;
      }
      if (a.store_ptr != nullptr && b.store_ptr != nullptr) {
        return std::lexicographical_compare(a.store_ptr->begin(), a.store_ptr->end(),
                                            b.store_ptr->begin(), b.store_ptr->end());
      } else if (a.store_ptr != nullptr && b.store_ptr == nullptr) {
        return std::lexicographical_compare(a.store_ptr->begin(), a.store_ptr->end(),
                                            b.in_key->second->begin(), b.in_key->second->end());
      } else if (a.store_ptr == nullptr && b.store_ptr != nullptr) {
        return std::lexicographical_compare(a.in_key->second->begin(), a.in_key->second->end(),
                                            b.store_ptr->begin(), b.store_ptr->end());
      } else {
        return std::lexicographical_compare(a.in_key->second->begin(), a.in_key->second->end(),
                                            b.in_key->second->begin(), b.in_key->second->end());
      }
    }
  };

 public:
  StoreKey* Add(Thread* self, const InKey& key) {
    uint64_t hash_start;
    if (kIsDebugBuild) {
      hash_start = NanoTime();
    }
    HashType raw_hash = HashFunc()(key);
    if (kIsDebugBuild) {
      uint64_t hash_end = NanoTime();
      hash_time_ += hash_end - hash_start;
    }
    HashType shard_hash = raw_hash / kShard;
    HashType shard_bin = raw_hash % kShard;
    HashedInKey hashed_in_key(shard_hash, &key);
    HashedKey hashed_key;
    hashed_key.store_ptr = nullptr;
    hashed_key.in_key = &hashed_in_key;
    MutexLock lock(self, *lock_[shard_bin]);
    auto it = keys_[shard_bin].find(hashed_key);
    if (it != keys_[shard_bin].end()) {
      DCHECK(it->store_ptr != nullptr);
      return it->store_ptr;
    }
    hashed_key.store_ptr = CreateStoreKey(key);
    hashed_key.store_hash = shard_hash;
    keys_[shard_bin].insert(hashed_key);
    return hashed_key.store_ptr;
  }

  explicit DedupeSet(const char* set_name, SwapAllocator<void>& alloc)
      : allocator_(alloc), hash_time_(0) {
    for (HashType i = 0; i < kShard; ++i) {
      std::ostringstream oss;
      oss << set_name << " lock " << i;
      lock_name_[i] = oss.str();
      lock_[i].reset(new Mutex(lock_name_[i].c_str()));
    }
  }

  ~DedupeSet() {
    // Have to manually free all pointers.
    for (auto& shard : keys_) {
      for (const auto& hashed_key : shard) {
        DCHECK(hashed_key.store_ptr != nullptr);
        DeleteStoreKey(hashed_key.store_ptr);
      }
    }
  }

  std::string DumpStats() const {
    size_t collision_sum = 0;
    size_t collision_max = 0;
    for (HashType shard = 0; shard < kShard; ++shard) {
      HashType last_hash = 0;
      size_t collision_cur_max = 0;
      for (const HashedKey& key : keys_[shard]) {
        DCHECK(key.store_ptr != nullptr);
        if (key.store_hash == last_hash) {
          collision_cur_max++;
          if (collision_cur_max > 1) {
            collision_sum++;
            if (collision_cur_max > collision_max) {
              collision_max = collision_cur_max;
            }
          }
        } else {
          collision_cur_max = 1;
          last_hash = key.store_hash;
        }
      }
    }
    return StringPrintf("%zu collisions, %zu max bucket size, %" PRIu64 " ns hash time",
                        collision_sum, collision_max, hash_time_);
  }

 private:
  StoreKey* CreateStoreKey(const InKey& key) {
    StoreKey* ret = allocator_.allocate(1);
    allocator_.construct(ret, key.begin(), key.end(), allocator_);
    return ret;
  }

  void DeleteStoreKey(StoreKey* key) {
    SwapAllocator<StoreKey> alloc(allocator_);
    alloc.destroy(key);
    alloc.deallocate(key, 1);
  }

  std::string lock_name_[kShard];
  std::unique_ptr<Mutex> lock_[kShard];
  std::set<HashedKey, Comparator> keys_[kShard];
  SwapAllocator<StoreKey> allocator_;
  uint64_t hash_time_;

  DISALLOW_COPY_AND_ASSIGN(DedupeSet);
};

}  // namespace art

#endif  // ART_COMPILER_UTILS_DEDUPE_SET_H_
