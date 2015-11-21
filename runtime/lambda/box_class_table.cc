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
#include "lambda/box_class_table.h"

#include "base/mutex.h"
#include "common_throws.h"
#include "gc_root-inl.h"
#include "lambda/closure.h"
#include "lambda/leaking_allocator.h"
#include "mirror/method.h"
#include "mirror/object-inl.h"
#include "thread.h"

#include <string>
#include <vector>

namespace art {
namespace lambda {

// Create the lambda proxy class given the name of the lambda interface (e.g. Ljava/lang/Runnable;)
// Also needs a proper class loader (or null for bootclasspath) where the proxy will be created
// into.
//
// The class must **not** have already been created.
// Returns a non-null ptr on success, otherwise returns null and has an exception set.
static mirror::Class* CreateClass(Thread* self,
                                  const std::string& class_name,
                                  const Handle<mirror::ClassLoader>& class_loader)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  ScopedObjectAccessUnchecked soa(self);
  StackHandleScope<2> hs(self);

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();

  // Find the java.lang.Class for our class name (from the class loader).
  Handle<mirror::Class> lambda_interface =
      hs.NewHandle(class_linker->FindClass(self, class_name.c_str(), class_loader));
  // TODO: use LookupClass in a loop
  // TODO: DCHECK That this doesn't actually cause the class to be loaded,
  //       since the create-lambda should've loaded it already
  DCHECK(lambda_interface.Get() != nullptr) << "CreateClass with class_name=" << class_name;
  DCHECK(lambda_interface->IsInterface()) << "CreateClass with class_name=" << class_name;
  jobject lambda_interface_class = soa.AddLocalReference<jobject>(lambda_interface.Get());

  // Look up java.lang.reflect.Proxy#getLambdaProxyClass method.
  Handle<mirror::Class> java_lang_reflect_proxy =
      hs.NewHandle(class_linker->FindSystemClass(soa.Self(), "Ljava/lang/reflect/Proxy;"));
  jclass java_lang_reflect_proxy_class =
      soa.AddLocalReference<jclass>(java_lang_reflect_proxy.Get());
  DCHECK(java_lang_reflect_proxy.Get() != nullptr);

  jmethodID proxy_factory_method_id =
      soa.Env()->GetStaticMethodID(java_lang_reflect_proxy_class,
                                  "getLambdaProxyClass",
                                  "(Ljava/lang/ClassLoader;Ljava/lang/Class;)Ljava/lang/Class;");
  DCHECK(!soa.Env()->ExceptionCheck());

  // Call into the java code to do the hard work of figuring out which methods and throws
  // our lambda interface proxy needs to implement. It then calls back into the class linker
  // on our behalf to make the proxy itself.
  jobject generated_lambda_proxy_class =
      soa.Env()->CallStaticObjectMethod(java_lang_reflect_proxy_class,
                                        proxy_factory_method_id,
                                        class_loader.ToJObject(),
                                        lambda_interface_class);

  // This can throw in which case we return null. Caller must handle.
  return soa.Decode<mirror::Class*>(generated_lambda_proxy_class);
}

BoxClassTable::BoxClassTable() {
}

BoxClassTable::~BoxClassTable() {
  // Don't need to do anything, classes are deleted automatically by GC
  // when the classloader is deleted.
  //
  // Our table will not outlive the classloader since the classloader owns it.
}

mirror::Class* BoxClassTable::GetOrCreateBoxClass(const char* class_name,
                                                  const Handle<mirror::ClassLoader>& class_loader) {
  DCHECK(class_name != nullptr);

  Thread* self = Thread::Current();

  std::string class_name_str = class_name;

  {
    MutexLock mu(self, *Locks::lambda_class_table_lock_);

    // Attempt to look up this class, it's possible it was already created previously.
    // If this is the case we *must* return the same class as before to maintain
    // referential equality between box instances.
    //
    // In managed code:
    //   Functional f = () -> 5;  // vF = create-lambda
    //   Object a = f;            // vA = box-lambda vA
    //   Object b = f;            // vB = box-lambda vB
    //   assert(a.getClass() == b.getClass())
    //   assert(a == b)
    ValueType value = FindBoxedClass(class_name_str);
    if (!value.IsNull()) {
      return value.Read();
    }
  }

  // Otherwise we need to generate a class ourselves and insert it into the hash map

  // Release the table lock here, which implicitly allows other threads to suspend
  // (since the GC callbacks will not block on trying to acquire our lock).
  // We also don't want to call into the class linker with the lock held because
  // our lock level is lower.
  self->AllowThreadSuspension();

  // Create a lambda proxy class, within the specified class loader.
  mirror::Class* lambda_proxy_class = CreateClass(self, class_name_str, class_loader);

  // There are no thread suspension points after this, so we don't need to put it into a handle.
  ScopedAssertNoThreadSuspension soants{self, "BoxClassTable::GetOrCreateBoxClass"};  // NOLINT:  [readability/braces] [4]

  if (UNLIKELY(lambda_proxy_class == nullptr)) {
    // Most likely an OOM has occurred.
    CHECK(self->IsExceptionPending());
    return nullptr;
  }

  {
    MutexLock mu(self, *Locks::lambda_class_table_lock_);

    // Possible, but unlikely, that someone already came in and made a proxy class
    // on another thread.
    ValueType value = FindBoxedClass(class_name_str);
    if (UNLIKELY(!value.IsNull())) {
      DCHECK_EQ(lambda_proxy_class, value.Read());
      return value.Read();
    }

    // Otherwise we made a brand new proxy class.
    // The class itself is cleaned up by the GC (e.g. class unloading) later.

    // Actually insert into the table.
    map_.Insert({std::move(class_name_str), ValueType(lambda_proxy_class)});
  }

  return lambda_proxy_class;
}

BoxClassTable::ValueType BoxClassTable::FindBoxedClass(const std::string& class_name) const {
  auto map_iterator = map_.Find(class_name);
  if (map_iterator != map_.end()) {
    const std::pair<UnorderedMapKeyType, ValueType>& key_value_pair = *map_iterator;
    const ValueType& value = key_value_pair.second;

    DCHECK(!value.IsNull());  // Never store null boxes.
    return value;
  }

  return ValueType(nullptr);
}

void BoxClassTable::EmptyFn::MakeEmpty(std::pair<UnorderedMapKeyType, ValueType>& item) const {
  item.first.clear();

  Locks::mutator_lock_->AssertSharedHeld(Thread::Current());
  item.second = ValueType();  // Also clear the GC root.
}

bool BoxClassTable::EmptyFn::IsEmpty(const std::pair<UnorderedMapKeyType, ValueType>& item) const {
  bool is_empty = item.first.empty();
  DCHECK_EQ(item.second.IsNull(), is_empty);

  return is_empty;
}

bool BoxClassTable::EqualsFn::operator()(const UnorderedMapKeyType& lhs,
                                         const UnorderedMapKeyType& rhs) const {
  // Be damn sure the classes don't just move around from under us.
  Locks::mutator_lock_->AssertSharedHeld(Thread::Current());

  // Being the same class name isn't enough, must also have the same class loader.
  // When we are in the same class loader, classes are equal via the pointer.
  return lhs == rhs;
}

size_t BoxClassTable::HashFn::operator()(const UnorderedMapKeyType& key) const {
  return std::hash<std::string>()(key);
}

}  // namespace lambda
}  // namespace art
