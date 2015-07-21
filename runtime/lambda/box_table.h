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
#ifndef ART_RUNTIME_LAMBDA_BOX_TABLE_H_
#define ART_RUNTIME_LAMBDA_BOX_TABLE_H_

#include "base/allocator.h"
#include "base/hash_map.h"
#include "gc_root.h"
#include "base/macros.h"
#include "base/mutex.h"
#include "object_callbacks.h"

#include <stdint.h>

namespace art {

class ArtMethod;  // forward declaration

namespace mirror {
class Object;  // forward declaration
}  // namespace mirror

namespace lambda {

/*
 * Store a table of boxed lambdas. This is required to maintain object referential equality
 * when a lambda is re-boxed.
 *
 * Conceptually, we store a mapping of Closures -> Weak Reference<Boxed Lambda Object>.
 * When too many objects get GCd, we shrink the underlying table to use less space.
 */
class BoxTable FINAL {
 public:
  using ClosureType = art::ArtMethod*;

  // Boxes a closure into an object. Returns null and throws an exception on failure.
  mirror::Object* BoxLambda(const ClosureType& closure)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      LOCKS_EXCLUDED(Locks::lambda_table_lock_);

  // Unboxes an object back into the lambda. Returns false and throws an exception on failure.
  bool UnboxLambda(mirror::Object* object, ClosureType* out_closure)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Sweep weak references to lambda boxes. Update the addresses if the objects have been
  // moved, and delete them from the table if the objects have been cleaned up.
  void SweepWeakBoxedLambdas(IsMarkedVisitor* visitor)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      LOCKS_EXCLUDED(Locks::lambda_table_lock_);

  // GC callback: Temporarily block anyone from touching the map.
  void DisallowNewWeakBoxedLambdas()
      LOCKS_EXCLUDED(Locks::lambda_table_lock_);

  // GC callback: Unblock any readers who have been queued waiting to touch the map.
  void AllowNewWeakBoxedLambdas()
      LOCKS_EXCLUDED(Locks::lambda_table_lock_);

  // GC callback: Verify that the state is now blocking anyone from touching the map.
  void EnsureNewWeakBoxedLambdasDisallowed()
      LOCKS_EXCLUDED(Locks::lambda_table_lock_);

  BoxTable();
  ~BoxTable() = default;

 private:
  // FIXME: This needs to be a GcRoot.
  // Explanation:
  // - After all threads are suspended (exclusive mutator lock),
  //   the concurrent-copying GC can move objects from the "from" space to the "to" space.
  // If an object is moved at that time and *before* SweepSystemWeaks are called then
  // we don't know if the move has happened yet.
  // Successive reads will then (incorrectly) look at the objects in the "from" space,
  // which is a problem since the objects have been already forwarded and mutations
  // would not be visible in the right space.
  // Instead, use a GcRoot here which will be automatically updated by the GC.
  //
  // Also, any reads should be protected by a read barrier to always give us the "to" space address.
  using ValueType = GcRoot<mirror::Object>;

  // Attempt to look up the lambda in the map, or return null if it's not there yet.
  ValueType FindBoxedLambda(const ClosureType& closure) const
      SHARED_LOCKS_REQUIRED(Locks::lambda_table_lock_);

  // If the GC has come in and temporarily disallowed touching weaks, block until is it allowed.
  void BlockUntilWeaksAllowed()
      SHARED_LOCKS_REQUIRED(Locks::lambda_table_lock_);

  // EmptyFn implementation for art::HashMap
  struct EmptyFn {
    void MakeEmpty(std::pair<ClosureType, ValueType>& item) const {
      item.first = nullptr;
    }
    bool IsEmpty(const std::pair<ClosureType, ValueType>& item) const {
      return item.first == nullptr;
    }
  };

  // HashFn implementation for art::HashMap
  struct HashFn {
    size_t operator()(const ClosureType& key) const {
      // TODO(iam): Rewrite hash function when ClosureType is no longer an ArtMethod*
      return static_cast<size_t>(reinterpret_cast<uintptr_t>(key));
    }
  };

  // EqualsFn implementation for art::HashMap
  struct EqualsFn {
    bool operator()(const ClosureType& lhs, const ClosureType& rhs) const;
  };

  using UnorderedMap = art::HashMap<ClosureType,
                                    ValueType,
                                    EmptyFn,
                                    HashFn,
                                    EqualsFn,
                                    TrackingAllocator<std::pair<ClosureType, ValueType>,
                                                      kAllocatorTagLambdaBoxTable>>;

  UnorderedMap map_                                          GUARDED_BY(Locks::lambda_table_lock_);
  bool allow_new_weaks_                                      GUARDED_BY(Locks::lambda_table_lock_);
  ConditionVariable new_weaks_condition_                     GUARDED_BY(Locks::lambda_table_lock_);

  // Shrink the map when we get below this load factor.
  // (This is an arbitrary value that should be large enough to prevent aggressive map erases
  // from shrinking the table too often.)
  static constexpr double kMinimumLoadFactor = UnorderedMap::kDefaultMinLoadFactor / 2;

  DISALLOW_COPY_AND_ASSIGN(BoxTable);
};

}  // namespace lambda
}  // namespace art

#endif  // ART_RUNTIME_LAMBDA_BOX_TABLE_H_
