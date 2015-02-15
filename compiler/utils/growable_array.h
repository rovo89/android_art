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

#ifndef ART_COMPILER_UTILS_GROWABLE_ARRAY_H_
#define ART_COMPILER_UTILS_GROWABLE_ARRAY_H_

#include <stdint.h>
#include <stddef.h>
#include "arena_allocator.h"

namespace art {

// Type of growable list for memory tuning.
enum OatListKind {
  kGrowableArrayMisc = 0,
  kGrowableArrayBlockList,
  kGrowableArraySSAtoDalvikMap,
  kGrowableArrayDfsOrder,
  kGrowableArrayDfsPostOrder,
  kGrowableArrayDomPostOrderTraversal,
  kGrowableArraySwitchTables,
  kGrowableArrayFillArrayData,
  kGrowableArraySuccessorBlocks,
  kGrowableArrayPredecessors,
  kGrowableArraySlowPaths,
  kGNumListKinds
};

template<typename T>
class GrowableArray {
  public:
    class Iterator {
      public:
        explicit Iterator(GrowableArray* g_list)
          : idx_(0),
            g_list_(g_list) {}

        explicit Iterator()
          : idx_(0),
            g_list_(nullptr) {}

        // NOTE: returns 0/NULL when no next.
        // TODO: redo to make usage consistent with other iterators.
        T Next() {
          DCHECK(g_list_ != nullptr);
          if (idx_ >= g_list_->Size()) {
            return 0;
          } else {
            return g_list_->Get(idx_++);
          }
        }

        void Reset() {
          idx_ = 0;
        }

        void Reset(GrowableArray* g_list) {
          idx_ = 0;
          g_list_ = g_list;
        }

        size_t GetIndex() const {
          return idx_;
        }

      private:
        size_t idx_;
        GrowableArray* g_list_;
    };

    GrowableArray(ArenaAllocator* arena, size_t init_length, OatListKind kind = kGrowableArrayMisc)
      : arena_(arena),
        num_allocated_(init_length),
        num_used_(0),
        kind_(kind) {
      elem_list_ = static_cast<T*>(arena_->Alloc(sizeof(T) * init_length,
                                                 kArenaAllocGrowableArray));
    };


    // Expand the list size to at least new length.
    void Resize(size_t new_length) {
      if (new_length <= num_allocated_) return;
      // If it's a small list double the size, else grow 1.5x.
      size_t target_length =
          (num_allocated_ < 128) ? num_allocated_ << 1 : num_allocated_ + (num_allocated_ >> 1);
      if (new_length > target_length) {
         target_length = new_length;
      }
      T* new_array = static_cast<T*>(arena_->Alloc(sizeof(T) * target_length,
                                                   kArenaAllocGrowableArray));
      memcpy(new_array, elem_list_, sizeof(T) * num_allocated_);
      num_allocated_ = target_length;
      elem_list_ = new_array;
    };

    // NOTE: does not return storage, just resets use count.
    void Reset() {
      num_used_ = 0;
    }

    // Insert an element to the end of a list, resizing if necessary.
    void Insert(T elem) {
      if (num_used_ == num_allocated_) {
        Resize(num_used_ + 1);
      }
      elem_list_[num_used_++] = elem;
    }

    void InsertAt(size_t index, T elem) {
      DCHECK(index <= Size());
      Insert(elem);
      for (size_t i = Size() - 1; i > index; --i) {
        elem_list_[i] = elem_list_[i - 1];
      }
      elem_list_[index] = elem;
    }

    void Add(T elem) {
      Insert(elem);
    }

    T Get(size_t index) const {
      DCHECK_LT(index, num_used_);
      return elem_list_[index];
    };

    // Overwrite existing element at position index.  List must be large enough.
    void Put(size_t index, T elem) {
      DCHECK_LT(index, num_used_);
      elem_list_[index] = elem;
    }

    void Increment(size_t index) {
      DCHECK_LT(index, num_used_);
      elem_list_[index]++;
    }

    /*
     * Remove an existing element from list.  If there are more than one copy
     * of the element, only the first one encountered will be deleted.
     */
    // TODO: consider renaming this.
    void Delete(T element) {
      bool found = false;
      for (size_t i = 0; i < num_used_ - 1; i++) {
        if (!found && elem_list_[i] == element) {
          found = true;
        }
        if (found) {
          elem_list_[i] = elem_list_[i+1];
        }
      }
      // We should either have found the element, or it was the last (unscanned) element.
      DCHECK(found || (element == elem_list_[num_used_ - 1]));
      // if element is not in array, don't touch anything
      if(!found && element != elem_list_[num_used_ - 1])
          return;
      num_used_--;
    };

    void DeleteAt(size_t index) {
      for (size_t i = index; i < num_used_ - 1; i++) {
        elem_list_[i] = elem_list_[i + 1];
      }
      num_used_--;
    };

    size_t GetNumAllocated() const { return num_allocated_; }

    size_t Size() const { return num_used_; }

    bool IsEmpty() const { return num_used_ == 0; }

    T Pop() {
      DCHECK_GE(num_used_, (size_t)0);
      return elem_list_[--num_used_];
    }

    T Peek() const {
      DCHECK_GE(num_used_, (size_t)0);
      return elem_list_[num_used_ - 1];
    }

    void SetSize(size_t new_size) {
      Resize(new_size);
      num_used_ = new_size;
    }

    T* GetRawStorage() const { return elem_list_; }

    static void* operator new(size_t size, ArenaAllocator* arena) {
      return arena->Alloc(sizeof(GrowableArray<T>), kArenaAllocGrowableArray);
    };
    static void operator delete(void* p) {}  // Nop.

  private:
    ArenaAllocator* const arena_;
    size_t num_allocated_;
    size_t num_used_;
    OatListKind kind_;
    T* elem_list_;
};

}  // namespace art

#endif  // ART_COMPILER_UTILS_GROWABLE_ARRAY_H_
