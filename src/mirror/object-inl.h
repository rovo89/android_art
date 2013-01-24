/*
 * Copyright (C) 2011 The Android Open Source Project
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

#ifndef ART_SRC_MIRROR_OBJECT_INL_H_
#define ART_SRC_MIRROR_OBJECT_INL_H_

#include "object.h"

#include "abstract_method.h"
#include "atomic.h"
#include "array.h"
#include "field.h"
#include "class.h"
#include "runtime.h"

namespace art {
namespace mirror {

inline Class* Object::GetClass() const {
  return GetFieldObject<Class*>(OFFSET_OF_OBJECT_MEMBER(Object, klass_), false);
}

inline void Object::SetClass(Class* new_klass) {
  // new_klass may be NULL prior to class linker initialization
  // We don't mark the card since the class is guaranteed to be referenced from another location.
  // Proxy classes are held live by the class loader, and other classes are roots of the class
  // linker.
  SetFieldPtr(OFFSET_OF_OBJECT_MEMBER(Object, klass_), new_klass, false, false);
}

inline bool Object::InstanceOf(const Class* klass) const {
  DCHECK(klass != NULL);
  DCHECK(GetClass() != NULL);
  return klass->IsAssignableFrom(GetClass());
}

inline bool Object::IsClass() const {
  Class* java_lang_Class = GetClass()->GetClass();
  return GetClass() == java_lang_Class;
}

inline Class* Object::AsClass() {
  DCHECK(IsClass());
  return down_cast<Class*>(this);
}

inline const Class* Object::AsClass() const {
  DCHECK(IsClass());
  return down_cast<const Class*>(this);
}

inline bool Object::IsObjectArray() const {
  return IsArrayInstance() && !GetClass()->GetComponentType()->IsPrimitive();
}

template<class T>
inline ObjectArray<T>* Object::AsObjectArray() {
  DCHECK(IsObjectArray());
  return down_cast<ObjectArray<T>*>(this);
}

template<class T>
inline const ObjectArray<T>* Object::AsObjectArray() const {
  DCHECK(IsObjectArray());
  return down_cast<const ObjectArray<T>*>(this);
}

inline bool Object::IsArrayInstance() const {
  return GetClass()->IsArrayClass();
}

inline bool Object::IsField() const {
  return GetClass()->IsFieldClass();
}

inline Field* Object::AsField() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DCHECK(IsField());
  return down_cast<Field*>(this);
}

inline const Field* Object::AsField() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DCHECK(IsField());
  return down_cast<const Field*>(this);
}

inline bool Object::IsMethod() const {
  return GetClass()->IsMethodClass();
}

inline AbstractMethod* Object::AsMethod() {
  DCHECK(IsMethod());
  return down_cast<AbstractMethod*>(this);
}

inline const AbstractMethod* Object::AsMethod() const {
  DCHECK(IsMethod());
  return down_cast<const AbstractMethod*>(this);
}

inline bool Object::IsReferenceInstance() const {
  return GetClass()->IsReferenceClass();
}

inline bool Object::IsWeakReferenceInstance() const {
  return GetClass()->IsWeakReferenceClass();
}

inline bool Object::IsSoftReferenceInstance() const {
  return GetClass()->IsSoftReferenceClass();
}

inline bool Object::IsFinalizerReferenceInstance() const {
  return GetClass()->IsFinalizerReferenceClass();
}

inline bool Object::IsPhantomReferenceInstance() const {
  return GetClass()->IsPhantomReferenceClass();
}

inline size_t Object::SizeOf() const {
  size_t result;
  if (IsArrayInstance()) {
    result = AsArray()->SizeOf();
  } else if (IsClass()) {
    result = AsClass()->SizeOf();
  } else {
    result = GetClass()->GetObjectSize();
  }
  DCHECK(!IsField()  || result == sizeof(Field));
  DCHECK(!IsMethod() || result == sizeof(AbstractMethod));
  return result;
}

inline uint64_t Object::GetField64(MemberOffset field_offset, bool is_volatile) const {
  VerifyObject(this);
  const byte* raw_addr = reinterpret_cast<const byte*>(this) + field_offset.Int32Value();
  const int64_t* addr = reinterpret_cast<const int64_t*>(raw_addr);
  if (UNLIKELY(is_volatile)) {
    uint64_t result = QuasiAtomic::Read64(addr);
    ANDROID_MEMBAR_FULL();
    return result;
  } else {
    return *addr;
  }
}

inline void Object::SetField64(MemberOffset field_offset, uint64_t new_value, bool is_volatile) {
  VerifyObject(this);
  byte* raw_addr = reinterpret_cast<byte*>(this) + field_offset.Int32Value();
  int64_t* addr = reinterpret_cast<int64_t*>(raw_addr);
  if (UNLIKELY(is_volatile)) {
    ANDROID_MEMBAR_STORE();
    QuasiAtomic::Write64(addr, new_value);
    // Post-store barrier not required due to use of atomic op or mutex.
  } else {
    *addr = new_value;
  }
}

inline void Object::WriteBarrierField(const Object* dst, MemberOffset field_offset,
                                      const Object* new_value) {
  Runtime::Current()->GetHeap()->WriteBarrierField(dst, field_offset, new_value);
}

}  // namespace mirror
}  // namespace art

#endif  // ART_SRC_MIRROR_OBJECT_INL_H_
