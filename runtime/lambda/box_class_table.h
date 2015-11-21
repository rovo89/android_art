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
#ifndef ART_RUNTIME_LAMBDA_BOX_CLASS_TABLE_H_
#define ART_RUNTIME_LAMBDA_BOX_CLASS_TABLE_H_

#include "base/allocator.h"
#include "base/hash_map.h"
#include "gc_root.h"
#include "base/macros.h"
#include "base/mutex.h"
#include "object_callbacks.h"

#include <stdint.h>

namespace art {

class ArtMethod;  // forward declaration
template<class T> class Handle;  // forward declaration

namespace mirror {
class Class;  // forward declaration
class ClassLoader;  // forward declaration
class LambdaProxy;  // forward declaration
class Object;  // forward declaration
}  // namespace mirror

namespace lambda {
struct Closure;  // forward declaration

/*
 * Store a table of boxed lambdas. This is required to maintain object referential equality
 * when a lambda is re-boxed.
 *
 * Conceptually, we store a mapping of Class Name -> Weak Reference<Class>.
 * When too many objects get GCd, we shrink the underlying table to use less space.
 */
class BoxClassTable FINAL {
 public:
  // TODO: This should take a LambdaArtMethod instead, read class name from that.
  // Note: null class_loader means bootclasspath.
  mirror::Class* GetOrCreateBoxClass(const char* class_name,
                                     const Handle<mirror::ClassLoader>& class_loader)
      REQUIRES(!Locks::lambda_class_table_lock_, !Roles::uninterruptible_)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Sweep strong references to lambda class boxes. Update the addresses if the objects
  // have been moved, and delete them from the table if the objects have been cleaned up.
  template <typename Visitor>
  void VisitRoots(const Visitor& visitor)
      NO_THREAD_SAFETY_ANALYSIS  // for object marking requiring heap bitmap lock
      REQUIRES(!Locks::lambda_class_table_lock_)
      SHARED_REQUIRES(Locks::mutator_lock_);

  BoxClassTable();
  ~BoxClassTable();

 private:
  // We only store strong GC roots in our table.
  using ValueType = GcRoot<mirror::Class>;

  // Attempt to look up the class in the map, or return null if it's not there yet.
  ValueType FindBoxedClass(const std::string& class_name) const
      SHARED_REQUIRES(Locks::lambda_class_table_lock_);

  // Store the key as a string so that we can have our own copy of the class name.
  using UnorderedMapKeyType = std::string;

  // EmptyFn implementation for art::HashMap
  struct EmptyFn {
    void MakeEmpty(std::pair<UnorderedMapKeyType, ValueType>& item) const
        NO_THREAD_SAFETY_ANALYSIS;
        // SHARED_REQUIRES(Locks::mutator_lock_);

    bool IsEmpty(const std::pair<UnorderedMapKeyType, ValueType>& item) const;
  };

  // HashFn implementation for art::HashMap
  struct HashFn {
    size_t operator()(const UnorderedMapKeyType& key) const
        NO_THREAD_SAFETY_ANALYSIS;
        // SHARED_REQUIRES(Locks::mutator_lock_);
  };

  // EqualsFn implementation for art::HashMap
  struct EqualsFn {
    bool operator()(const UnorderedMapKeyType& lhs, const UnorderedMapKeyType& rhs) const
        NO_THREAD_SAFETY_ANALYSIS;
        // SHARED_REQUIRES(Locks::mutator_lock_);
  };

  using UnorderedMap = art::HashMap<UnorderedMapKeyType,
                                    ValueType,
                                    EmptyFn,
                                    HashFn,
                                    EqualsFn,
                                    TrackingAllocator<std::pair<UnorderedMapKeyType, ValueType>,
                                                      kAllocatorTagLambdaProxyClassBoxTable>>;

  // Map of strong GC roots (lambda interface name -> lambda proxy class)
  UnorderedMap map_ GUARDED_BY(Locks::lambda_class_table_lock_);

  // Shrink the map when we get below this load factor.
  // (This is an arbitrary value that should be large enough to prevent aggressive map erases
  // from shrinking the table too often.)
  static constexpr double kMinimumLoadFactor = UnorderedMap::kDefaultMinLoadFactor / 2;

  DISALLOW_COPY_AND_ASSIGN(BoxClassTable);
};

}  // namespace lambda
}  // namespace art

#endif  // ART_RUNTIME_LAMBDA_BOX_CLASS_TABLE_H_
