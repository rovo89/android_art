/*
 * Copyright (C) 2014 The Android Open Source Project
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

#ifndef ART_COMPILER_UTILS_SWAP_SPACE_H_
#define ART_COMPILER_UTILS_SWAP_SPACE_H_

#include <cstdlib>
#include <list>
#include <set>
#include <stdint.h>
#include <stddef.h>

#include "base/logging.h"
#include "base/macros.h"
#include "base/mutex.h"
#include "mem_map.h"
#include "utils.h"
#include "utils/debug_stack.h"

namespace art {

// Chunk of space.
struct SpaceChunk {
  uint8_t* ptr;
  size_t size;

  uintptr_t Start() const {
    return reinterpret_cast<uintptr_t>(ptr);
  }
  uintptr_t End() const {
    return reinterpret_cast<uintptr_t>(ptr) + size;
  }
};

inline bool operator==(const SpaceChunk& lhs, const SpaceChunk& rhs) {
  return (lhs.size == rhs.size) && (lhs.ptr == rhs.ptr);
}

class SortChunkByPtr {
 public:
  bool operator()(const SpaceChunk& a, const SpaceChunk& b) const {
    return reinterpret_cast<uintptr_t>(a.ptr) < reinterpret_cast<uintptr_t>(b.ptr);
  }
};

// An arena pool that creates arenas backed by an mmaped file.
class SwapSpace {
 public:
  SwapSpace(int fd, size_t initial_size);
  ~SwapSpace();
  void* Alloc(size_t size) LOCKS_EXCLUDED(lock_);
  void Free(void* ptr, size_t size) LOCKS_EXCLUDED(lock_);

  size_t GetSize() {
    return size_;
  }

 private:
  SpaceChunk NewFileChunk(size_t min_size);

  int fd_;
  size_t size_;
  std::list<SpaceChunk> maps_;

  // NOTE: Boost.Bimap would be useful for the two following members.

  // Map start of a free chunk to its size.
  typedef std::set<SpaceChunk, SortChunkByPtr> FreeByStartSet;
  FreeByStartSet free_by_start_ GUARDED_BY(lock_);

  // Map size to an iterator to free_by_start_'s entry.
  typedef std::pair<size_t, FreeByStartSet::const_iterator> FreeBySizeEntry;
  struct FreeBySizeComparator {
    bool operator()(const FreeBySizeEntry& lhs, const FreeBySizeEntry& rhs) {
      if (lhs.first != rhs.first) {
        return lhs.first < rhs.first;
      } else {
        return lhs.second->Start() < rhs.second->Start();
      }
    }
  };
  typedef std::set<FreeBySizeEntry, FreeBySizeComparator> FreeBySizeSet;
  FreeBySizeSet free_by_size_ GUARDED_BY(lock_);

  mutable Mutex lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  DISALLOW_COPY_AND_ASSIGN(SwapSpace);
};

template <typename T> class SwapAllocator;

template <>
class SwapAllocator<void> {
 public:
  typedef void value_type;
  typedef void* pointer;
  typedef const void* const_pointer;

  template <typename U>
  struct rebind {
    typedef SwapAllocator<U> other;
  };

  explicit SwapAllocator(SwapSpace* swap_space) : swap_space_(swap_space) {}

  template <typename U>
  SwapAllocator(const SwapAllocator<U>& other) : swap_space_(other.swap_space_) {}

  SwapAllocator(const SwapAllocator& other) = default;
  SwapAllocator& operator=(const SwapAllocator& other) = default;
  ~SwapAllocator() = default;

 private:
  SwapSpace* swap_space_;

  template <typename U>
  friend class SwapAllocator;
};

template <typename T>
class SwapAllocator {
 public:
  typedef T value_type;
  typedef T* pointer;
  typedef T& reference;
  typedef const T* const_pointer;
  typedef const T& const_reference;
  typedef size_t size_type;
  typedef ptrdiff_t difference_type;

  template <typename U>
  struct rebind {
    typedef SwapAllocator<U> other;
  };

  explicit SwapAllocator(SwapSpace* swap_space) : swap_space_(swap_space) {}

  template <typename U>
  SwapAllocator(const SwapAllocator<U>& other) : swap_space_(other.swap_space_) {}

  SwapAllocator(const SwapAllocator& other) = default;
  SwapAllocator& operator=(const SwapAllocator& other) = default;
  ~SwapAllocator() = default;

  size_type max_size() const {
    return static_cast<size_type>(-1) / sizeof(T);
  }

  pointer address(reference x) const { return &x; }
  const_pointer address(const_reference x) const { return &x; }

  pointer allocate(size_type n, SwapAllocator<void>::pointer hint = nullptr) {
    DCHECK_LE(n, max_size());
    if (swap_space_ == nullptr) {
      return reinterpret_cast<T*>(malloc(n * sizeof(T)));
    } else {
      return reinterpret_cast<T*>(swap_space_->Alloc(n * sizeof(T)));
    }
  }
  void deallocate(pointer p, size_type n) {
    if (swap_space_ == nullptr) {
      free(p);
    } else {
      swap_space_->Free(p, n * sizeof(T));
    }
  }

  void construct(pointer p, const_reference val) {
    new (static_cast<void*>(p)) value_type(val);
  }
  template <class U, class... Args>
  void construct(U* p, Args&&... args) {
    ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
  }
  void destroy(pointer p) {
    p->~value_type();
  }

  inline bool operator==(SwapAllocator const& other) {
    return swap_space_ == other.swap_space_;
  }
  inline bool operator!=(SwapAllocator const& other) {
    return !operator==(other);
  }

 private:
  SwapSpace* swap_space_;

  template <typename U>
  friend class SwapAllocator;
};

template <typename T>
using SwapVector = std::vector<T, SwapAllocator<T>>;
template <typename T, typename Comparator>
using SwapSet = std::set<T, Comparator, SwapAllocator<T>>;

}  // namespace art

#endif  // ART_COMPILER_UTILS_SWAP_SPACE_H_
