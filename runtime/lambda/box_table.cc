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
#include "lambda/box_table.h"

#include "base/mutex.h"
#include "common_throws.h"
#include "gc_root-inl.h"
#include "lambda/box_class_table.h"
#include "lambda/closure.h"
#include "lambda/leaking_allocator.h"
#include "mirror/lambda_proxy.h"
#include "mirror/method.h"
#include "mirror/object-inl.h"
#include "thread.h"

#include <vector>

namespace art {
namespace lambda {
// All closures are boxed into a subtype of LambdaProxy which implements the lambda's interface.
using BoxedClosurePointerType = mirror::LambdaProxy*;

// Returns the base class for all boxed closures.
// Note that concrete closure boxes are actually a subtype of mirror::LambdaProxy.
static mirror::Class* GetBoxedClosureBaseClass() SHARED_REQUIRES(Locks::mutator_lock_) {
  return Runtime::Current()->GetClassLinker()->GetClassRoot(ClassLinker::kJavaLangLambdaProxy);
}

namespace {
  // Convenience functions to allocating/deleting box table copies of the closures.
  struct ClosureAllocator {
    // Deletes a Closure that was allocated through ::Allocate.
    static void Delete(Closure* ptr) {
      delete[] reinterpret_cast<char*>(ptr);
    }

    // Returns a well-aligned pointer to a newly allocated Closure on the 'new' heap.
    static Closure* Allocate(size_t size) {
      DCHECK_GE(size, sizeof(Closure));

      // TODO: Maybe point to the interior of the boxed closure object after we add proxy support?
      Closure* closure = reinterpret_cast<Closure*>(new char[size]);
      DCHECK_ALIGNED(closure, alignof(Closure));
      return closure;
    }
  };

  struct DeleterForClosure {
    void operator()(Closure* closure) const {
      ClosureAllocator::Delete(closure);
    }
  };

  using UniqueClosurePtr = std::unique_ptr<Closure, DeleterForClosure>;
}  // namespace

BoxTable::BoxTable()
  : allow_new_weaks_(true),
    new_weaks_condition_("lambda box table allowed weaks", *Locks::lambda_table_lock_) {}

BoxTable::~BoxTable() {
  // Free all the copies of our closures.
  for (auto map_iterator = map_.begin(); map_iterator != map_.end(); ) {
    std::pair<UnorderedMapKeyType, ValueType>& key_value_pair = *map_iterator;

    Closure* closure = key_value_pair.first;

    // Remove from the map first, so that it doesn't try to access dangling pointer.
    map_iterator = map_.Erase(map_iterator);

    // Safe to delete, no dangling pointers.
    ClosureAllocator::Delete(closure);
  }
}

mirror::Object* BoxTable::BoxLambda(const ClosureType& closure,
                                    const char* class_name,
                                    mirror::ClassLoader* class_loader) {
  Thread* self = Thread::Current();

  {
    // TODO: Switch to ReaderMutexLock if ConditionVariable ever supports RW Mutexes
    /*Reader*/MutexLock mu(self, *Locks::lambda_table_lock_);
    BlockUntilWeaksAllowed();

    // Attempt to look up this object, it's possible it was already boxed previously.
    // If this is the case we *must* return the same object as before to maintain
    // referential equality.
    //
    // In managed code:
    //   Functional f = () -> 5;  // vF = create-lambda
    //   Object a = f;            // vA = box-lambda vA
    //   Object b = f;            // vB = box-lambda vB
    //   assert(a == b)
    ValueType value = FindBoxedLambda(closure);
    if (!value.IsNull()) {
      return value.Read();
    }

    // Otherwise we need to box ourselves and insert it into the hash map
  }

  // Convert the Closure into a managed object instance, whose supertype of java.lang.LambdaProxy.

  // TODO: Boxing a learned lambda (i.e. made with unbox-lambda) should return the original object
  StackHandleScope<2> hs{self};  // NOLINT: [readability/braces] [4]

  Handle<mirror::ClassLoader> class_loader_handle = hs.NewHandle(class_loader);

  // Release the lambda table lock here, so that thread suspension is allowed.
  self->AllowThreadSuspension();

  lambda::BoxClassTable* lambda_box_class_table;

  // Find the lambda box class table, which can be in the system class loader if classloader is null
  if (class_loader == nullptr) {
    ScopedObjectAccessUnchecked soa(self);
    mirror::ClassLoader* system_class_loader =
        soa.Decode<mirror::ClassLoader*>(Runtime::Current()->GetSystemClassLoader());
    lambda_box_class_table = system_class_loader->GetLambdaProxyCache();
  } else {
    lambda_box_class_table = class_loader_handle->GetLambdaProxyCache();
    // OK: can't be deleted while we hold a handle to the class loader.
  }
  DCHECK(lambda_box_class_table != nullptr);

  Handle<mirror::Class> closure_class(hs.NewHandle(
      lambda_box_class_table->GetOrCreateBoxClass(class_name, class_loader_handle)));
  if (UNLIKELY(closure_class.Get() == nullptr)) {
    // Most likely an OOM has occurred.
    self->AssertPendingException();
    return nullptr;
  }

  BoxedClosurePointerType closure_as_object = nullptr;
  UniqueClosurePtr closure_table_copy;
  // Create an instance of the class, and assign the pointer to the closure into it.
  {
    closure_as_object = down_cast<BoxedClosurePointerType>(closure_class->AllocObject(self));
    if (UNLIKELY(closure_as_object == nullptr)) {
      self->AssertPendingOOMException();
      return nullptr;
    }

    // Make a copy of the closure that we will store in the hash map.
    // The proxy instance will also point to this same hash map.
    // Note that the closure pointer is cleaned up only after the proxy is GCd.
    closure_table_copy.reset(ClosureAllocator::Allocate(closure->GetSize()));
    closure_as_object->SetClosure(closure_table_copy.get());
  }

  // There are no thread suspension points after this, so we don't need to put it into a handle.
  ScopedAssertNoThreadSuspension soants{self,                                                    // NOLINT: [whitespace/braces] [5]
                                        "box lambda table - box lambda - no more suspensions"};  // NOLINT: [whitespace/braces] [5]

  // Write the raw closure data into the proxy instance's copy of the closure.
  closure->CopyTo(closure_table_copy.get(),
                  closure->GetSize());

  // The method has been successfully boxed into an object, now insert it into the hash map.
  {
    MutexLock mu(self, *Locks::lambda_table_lock_);
    BlockUntilWeaksAllowed();

    // Lookup the object again, it's possible another thread already boxed it while
    // we were allocating the object before.
    ValueType value = FindBoxedLambda(closure);
    if (UNLIKELY(!value.IsNull())) {
      // Let the GC clean up closure_as_object at a later time.
      // (We will not see this object when sweeping, it wasn't inserted yet.)
      closure_as_object->SetClosure(nullptr);
      return value.Read();
    }

    // Otherwise we need to insert it into the hash map in this thread.

    // The closure_table_copy is deleted by us manually when we erase it from the map.

    // Actually insert into the table.
    map_.Insert({closure_table_copy.release(), ValueType(closure_as_object)});
  }

  return closure_as_object;
}

bool BoxTable::UnboxLambda(mirror::Object* object, ClosureType* out_closure) {
  DCHECK(object != nullptr);
  *out_closure = nullptr;

  Thread* self = Thread::Current();

  // Note that we do not need to access lambda_table_lock_ here
  // since we don't need to look at the map.

  mirror::Object* boxed_closure_object = object;

  // Raise ClassCastException if object is not instanceof LambdaProxy
  if (UNLIKELY(!boxed_closure_object->InstanceOf(GetBoxedClosureBaseClass()))) {
    ThrowClassCastException(GetBoxedClosureBaseClass(), boxed_closure_object->GetClass());
    return false;
  }

  // TODO(iam): We must check that the closure object extends/implements the type
  // specified in [type id]. This is not currently implemented since the type id is unavailable.

  // If we got this far, the inputs are valid.
  // Shuffle the java.lang.LambdaProxy back into a raw closure, then allocate it, copy,
  // and return it.
  BoxedClosurePointerType boxed_closure =
      down_cast<BoxedClosurePointerType>(boxed_closure_object);

  DCHECK_ALIGNED(boxed_closure->GetClosure(), alignof(Closure));
  const Closure* aligned_interior_closure = boxed_closure->GetClosure();
  DCHECK(aligned_interior_closure != nullptr);

  // TODO: we probably don't need to make a copy here later on, once there's GC support.

  // Allocate a copy that can "escape" and copy the closure data into that.
  Closure* unboxed_closure =
      LeakingAllocator::MakeFlexibleInstance<Closure>(self, aligned_interior_closure->GetSize());
  DCHECK_ALIGNED(unboxed_closure, alignof(Closure));
  // TODO: don't just memcpy the closure, it's unsafe when we add references to the mix.
  memcpy(unboxed_closure, aligned_interior_closure, aligned_interior_closure->GetSize());

  DCHECK_EQ(unboxed_closure->GetSize(), aligned_interior_closure->GetSize());

  *out_closure = unboxed_closure;
  return true;
}

BoxTable::ValueType BoxTable::FindBoxedLambda(const ClosureType& closure) const {
  auto map_iterator = map_.Find(closure);
  if (map_iterator != map_.end()) {
    const std::pair<UnorderedMapKeyType, ValueType>& key_value_pair = *map_iterator;
    const ValueType& value = key_value_pair.second;

    DCHECK(!value.IsNull());  // Never store null boxes.
    return value;
  }

  return ValueType(nullptr);
}

void BoxTable::BlockUntilWeaksAllowed() {
  Thread* self = Thread::Current();
  while (UNLIKELY((!kUseReadBarrier && !allow_new_weaks_) ||
                  (kUseReadBarrier && !self->GetWeakRefAccessEnabled()))) {
    new_weaks_condition_.WaitHoldingLocks(self);  // wait while holding mutator lock
  }
}

void BoxTable::SweepWeakBoxedLambdas(IsMarkedVisitor* visitor) {
  DCHECK(visitor != nullptr);

  Thread* self = Thread::Current();
  MutexLock mu(self, *Locks::lambda_table_lock_);

  /*
   * Visit every weak root in our lambda box table.
   * Remove unmarked objects, update marked objects to new address.
   */
  std::vector<ClosureType> remove_list;
  for (auto map_iterator = map_.begin(); map_iterator != map_.end(); ) {
    std::pair<UnorderedMapKeyType, ValueType>& key_value_pair = *map_iterator;

    const ValueType& old_value = key_value_pair.second;

    // This does not need a read barrier because this is called by GC.
    mirror::Object* old_value_raw = old_value.Read<kWithoutReadBarrier>();
    mirror::Object* new_value = visitor->IsMarked(old_value_raw);

    if (new_value == nullptr) {
      // The object has been swept away.
      Closure* closure = key_value_pair.first;

      // Delete the entry from the map.
      // (Remove from map first to avoid accessing dangling pointer).
      map_iterator = map_.Erase(map_iterator);

      // Clean up the memory by deleting the closure.
      ClosureAllocator::Delete(closure);

    } else {
      // The object has been moved.
      // Update the map.
      key_value_pair.second = ValueType(new_value);
      ++map_iterator;
    }
  }

  // Occasionally shrink the map to avoid growing very large.
  if (map_.CalculateLoadFactor() < kMinimumLoadFactor) {
    map_.ShrinkToMaximumLoad();
  }
}

void BoxTable::DisallowNewWeakBoxedLambdas() {
  CHECK(!kUseReadBarrier);
  Thread* self = Thread::Current();
  MutexLock mu(self, *Locks::lambda_table_lock_);

  allow_new_weaks_ = false;
}

void BoxTable::AllowNewWeakBoxedLambdas() {
  CHECK(!kUseReadBarrier);
  Thread* self = Thread::Current();
  MutexLock mu(self, *Locks::lambda_table_lock_);

  allow_new_weaks_ = true;
  new_weaks_condition_.Broadcast(self);
}

void BoxTable::BroadcastForNewWeakBoxedLambdas() {
  CHECK(kUseReadBarrier);
  Thread* self = Thread::Current();
  MutexLock mu(self, *Locks::lambda_table_lock_);
  new_weaks_condition_.Broadcast(self);
}

void BoxTable::EmptyFn::MakeEmpty(std::pair<UnorderedMapKeyType, ValueType>& item) const {
  item.first = nullptr;

  Locks::mutator_lock_->AssertSharedHeld(Thread::Current());
  item.second = ValueType();  // Also clear the GC root.
}

bool BoxTable::EmptyFn::IsEmpty(const std::pair<UnorderedMapKeyType, ValueType>& item) const {
  bool is_empty = item.first == nullptr;
  DCHECK_EQ(item.second.IsNull(), is_empty);

  return is_empty;
}

bool BoxTable::EqualsFn::operator()(const UnorderedMapKeyType& lhs,
                                    const UnorderedMapKeyType& rhs) const {
  // Nothing needs this right now, but leave this assertion for later when
  // we need to look at the references inside of the closure.
  Locks::mutator_lock_->AssertSharedHeld(Thread::Current());

  return lhs->ReferenceEquals(rhs);
}

size_t BoxTable::HashFn::operator()(const UnorderedMapKeyType& key) const {
  const lambda::Closure* closure = key;
  DCHECK_ALIGNED(closure, alignof(lambda::Closure));

  // Need to hold mutator_lock_ before calling into Closure::GetHashCode.
  Locks::mutator_lock_->AssertSharedHeld(Thread::Current());
  return closure->GetHashCode();
}

}  // namespace lambda
}  // namespace art
