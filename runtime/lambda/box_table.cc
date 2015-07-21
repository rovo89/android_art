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
#include "mirror/method.h"
#include "mirror/object-inl.h"
#include "thread.h"

#include <vector>

namespace art {
namespace lambda {

BoxTable::BoxTable()
  : allow_new_weaks_(true),
    new_weaks_condition_("lambda box table allowed weaks", *Locks::lambda_table_lock_) {}

mirror::Object* BoxTable::BoxLambda(const ClosureType& closure) {
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
    //   assert(a == f)
    ValueType value = FindBoxedLambda(closure);
    if (!value.IsNull()) {
      return value.Read();
    }

    // Otherwise we need to box ourselves and insert it into the hash map
  }

  // Release the lambda table lock here, so that thread suspension is allowed.

  // Convert the ArtMethod into a java.lang.reflect.Method which will serve
  // as the temporary 'boxed' version of the lambda. This is good enough
  // to check all the basic object identities that a boxed lambda must retain.

  // TODO: Boxing an innate lambda (i.e. made with create-lambda) should make a proxy class
  // TODO: Boxing a learned lambda (i.e. made with unbox-lambda) should return the original object
  mirror::Method* method_as_object =
      mirror::Method::CreateFromArtMethod(self, closure);
  // There are no thread suspension points after this, so we don't need to put it into a handle.

  if (UNLIKELY(method_as_object == nullptr)) {
    // Most likely an OOM has occurred.
    CHECK(self->IsExceptionPending());
    return nullptr;
  }

  // The method has been successfully boxed into an object, now insert it into the hash map.
  {
    MutexLock mu(self, *Locks::lambda_table_lock_);
    BlockUntilWeaksAllowed();

    // Lookup the object again, it's possible another thread already boxed it while
    // we were allocating the object before.
    ValueType value = FindBoxedLambda(closure);
    if (UNLIKELY(!value.IsNull())) {
      // Let the GC clean up method_as_object at a later time.
      return value.Read();
    }

    // Otherwise we should insert it into the hash map in this thread.
    map_.Insert(std::make_pair(closure, ValueType(method_as_object)));
  }

  return method_as_object;
}

bool BoxTable::UnboxLambda(mirror::Object* object, ClosureType* out_closure) {
  DCHECK(object != nullptr);
  *out_closure = nullptr;

  // Note that we do not need to access lambda_table_lock_ here
  // since we don't need to look at the map.

  mirror::Object* boxed_closure_object = object;

  // Raise ClassCastException if object is not instanceof java.lang.reflect.Method
  if (UNLIKELY(!boxed_closure_object->InstanceOf(mirror::Method::StaticClass()))) {
    ThrowClassCastException(mirror::Method::StaticClass(), boxed_closure_object->GetClass());
    return false;
  }

  // TODO(iam): We must check that the closure object extends/implements the type
  // specified in [type id]. This is not currently implemented since it's always a Method.

  // If we got this far, the inputs are valid.
  // Write out the java.lang.reflect.Method's embedded ArtMethod* into the vreg target.
  mirror::AbstractMethod* boxed_closure_as_method =
      down_cast<mirror::AbstractMethod*>(boxed_closure_object);

  ArtMethod* unboxed_closure = boxed_closure_as_method->GetArtMethod();
  DCHECK(unboxed_closure != nullptr);

  *out_closure = unboxed_closure;
  return true;
}

BoxTable::ValueType BoxTable::FindBoxedLambda(const ClosureType& closure) const {
  auto map_iterator = map_.Find(closure);
  if (map_iterator != map_.end()) {
    const std::pair<ClosureType, ValueType>& key_value_pair = *map_iterator;
    const ValueType& value = key_value_pair.second;

    DCHECK(!value.IsNull());  // Never store null boxes.
    return value;
  }

  return ValueType(nullptr);
}

void BoxTable::BlockUntilWeaksAllowed() {
  Thread* self = Thread::Current();
  while (UNLIKELY(allow_new_weaks_ == false)) {
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
    std::pair<ClosureType, ValueType>& key_value_pair = *map_iterator;

    const ValueType& old_value = key_value_pair.second;

    // This does not need a read barrier because this is called by GC.
    mirror::Object* old_value_raw = old_value.Read<kWithoutReadBarrier>();
    mirror::Object* new_value = visitor->IsMarked(old_value_raw);

    if (new_value == nullptr) {
      const ClosureType& closure = key_value_pair.first;
      // The object has been swept away.
      // Delete the entry from the map.
      map_iterator = map_.Erase(map_.Find(closure));
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
  Thread* self = Thread::Current();
  MutexLock mu(self, *Locks::lambda_table_lock_);

  allow_new_weaks_ = false;
}

void BoxTable::AllowNewWeakBoxedLambdas() {
  Thread* self = Thread::Current();
  MutexLock mu(self, *Locks::lambda_table_lock_);

  allow_new_weaks_ = true;
  new_weaks_condition_.Broadcast(self);
}

void BoxTable::EnsureNewWeakBoxedLambdasDisallowed() {
  Thread* self = Thread::Current();
  MutexLock mu(self, *Locks::lambda_table_lock_);
  CHECK_NE(allow_new_weaks_, false);
}

bool BoxTable::EqualsFn::operator()(const ClosureType& lhs, const ClosureType& rhs) const {
  // Nothing needs this right now, but leave this assertion for later when
  // we need to look at the references inside of the closure.
  if (kIsDebugBuild) {
    Locks::mutator_lock_->AssertSharedHeld(Thread::Current());
  }

  // TODO: Need rework to use read barriers once closures have references inside of them that can
  // move. Until then, it's safe to just compare the data inside of it directly.
  return lhs == rhs;
}

}  // namespace lambda
}  // namespace art
