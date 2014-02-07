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

#include <set>
#include <string>

#include "base/mutex.h"
#include "base/stl_util.h"
#include "base/stringprintf.h"

namespace art {

// A set of Keys that support a HashFunc returning HashType. Used to find duplicates of Key in the
// Add method. The data-structure is thread-safe through the use of internal locks, it also
// supports the lock being sharded.
template <typename Key, typename HashType, typename HashFunc, HashType kShard = 1>
class DedupeSet {
  typedef std::pair<HashType, Key*> HashedKey;

  class Comparator {
   public:
    bool operator()(const HashedKey& a, const HashedKey& b) const {
      if (a.first != b.first) {
        return a.first < b.first;
      } else {
        return *a.second < *b.second;
      }
    }
  };

 public:
  Key* Add(Thread* self, const Key& key) {
    HashType raw_hash = HashFunc()(key);
    HashType shard_hash = raw_hash / kShard;
    HashType shard_bin = raw_hash % kShard;
    HashedKey hashed_key(shard_hash, const_cast<Key*>(&key));
    MutexLock lock(self, *lock_[shard_bin]);
    auto it = keys_[shard_bin].find(hashed_key);
    if (it != keys_[shard_bin].end()) {
      return it->second;
    }
    hashed_key.second = new Key(key);
    keys_[shard_bin].insert(hashed_key);
    return hashed_key.second;
  }

  explicit DedupeSet(const char* set_name) {
    for (HashType i = 0; i < kShard; ++i) {
      std::ostringstream oss;
      oss << set_name << " lock " << i;
      lock_name_[i] = oss.str();
      lock_[i].reset(new Mutex(lock_name_[i].c_str()));
    }
  }

  ~DedupeSet() {
    for (HashType i = 0; i < kShard; ++i) {
      STLDeleteValues(&keys_[i]);
    }
  }

 private:
  std::string lock_name_[kShard];
  UniquePtr<Mutex> lock_[kShard];
  std::set<HashedKey, Comparator> keys_[kShard];

  DISALLOW_COPY_AND_ASSIGN(DedupeSet);
};

}  // namespace art

#endif  // ART_COMPILER_UTILS_DEDUPE_SET_H_
