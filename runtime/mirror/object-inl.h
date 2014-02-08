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

#ifndef ART_RUNTIME_MIRROR_OBJECT_INL_H_
#define ART_RUNTIME_MIRROR_OBJECT_INL_H_

#include "object.h"

#include "art_field.h"
#include "art_method.h"
#include "atomic.h"
#include "array-inl.h"
#include "class.h"
#include "lock_word-inl.h"
#include "monitor.h"
#include "runtime.h"
#include "throwable.h"

namespace art {
namespace mirror {

inline Class* Object::GetClass() {
  return GetFieldObject<Class>(OFFSET_OF_OBJECT_MEMBER(Object, klass_), false);
}

inline void Object::SetClass(Class* new_klass) {
  // new_klass may be NULL prior to class linker initialization.
  // We don't mark the card as this occurs as part of object allocation. Not all objects have
  // backing cards, such as large objects.
  SetFieldObjectWithoutWriteBarrier(OFFSET_OF_OBJECT_MEMBER(Object, klass_), new_klass, false, false);
}

inline LockWord Object::GetLockWord() {
  return LockWord(GetField32(OFFSET_OF_OBJECT_MEMBER(Object, monitor_), true));
}

inline void Object::SetLockWord(LockWord new_val) {
  SetField32(OFFSET_OF_OBJECT_MEMBER(Object, monitor_), new_val.GetValue(), true);
}

inline bool Object::CasLockWord(LockWord old_val, LockWord new_val) {
  return CasField32(OFFSET_OF_OBJECT_MEMBER(Object, monitor_), old_val.GetValue(),
                    new_val.GetValue());
}

inline uint32_t Object::GetLockOwnerThreadId() {
  return Monitor::GetLockOwnerThreadId(this);
}

inline void Object::MonitorEnter(Thread* self) {
  Monitor::MonitorEnter(self, this);
}

inline bool Object::MonitorExit(Thread* self) {
  return Monitor::MonitorExit(self, this);
}

inline void Object::Notify(Thread* self) {
  Monitor::Notify(self, this);
}

inline void Object::NotifyAll(Thread* self) {
  Monitor::NotifyAll(self, this);
}

inline void Object::Wait(Thread* self) {
  Monitor::Wait(self, this, 0, 0, true, kWaiting);
}

inline void Object::Wait(Thread* self, int64_t ms, int32_t ns) {
  Monitor::Wait(self, this, ms, ns, true, kTimedWaiting);
}

inline bool Object::VerifierInstanceOf(Class* klass) {
  DCHECK(klass != NULL);
  DCHECK(GetClass() != NULL);
  return klass->IsInterface() || InstanceOf(klass);
}

inline bool Object::InstanceOf(Class* klass) {
  DCHECK(klass != NULL);
  DCHECK(GetClass() != NULL);
  return klass->IsAssignableFrom(GetClass());
}

inline bool Object::IsClass() {
  Class* java_lang_Class = GetClass()->GetClass();
  return GetClass() == java_lang_Class;
}

inline Class* Object::AsClass() {
  DCHECK(IsClass());
  return down_cast<Class*>(this);
}

inline bool Object::IsObjectArray() {
  return IsArrayInstance() && !GetClass()->GetComponentType()->IsPrimitive();
}

template<class T>
inline ObjectArray<T>* Object::AsObjectArray() {
  DCHECK(IsObjectArray());
  return down_cast<ObjectArray<T>*>(this);
}

inline bool Object::IsArrayInstance() {
  return GetClass()->IsArrayClass();
}

inline bool Object::IsArtField() {
  return GetClass()->IsArtFieldClass();
}

inline ArtField* Object::AsArtField() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DCHECK(IsArtField());
  return down_cast<ArtField*>(this);
}

inline bool Object::IsArtMethod() {
  return GetClass()->IsArtMethodClass();
}

inline ArtMethod* Object::AsArtMethod() {
  DCHECK(IsArtMethod());
  return down_cast<ArtMethod*>(this);
}

inline bool Object::IsReferenceInstance() {
  return GetClass()->IsReferenceClass();
}

inline Array* Object::AsArray() {
  DCHECK(IsArrayInstance());
  return down_cast<Array*>(this);
}

inline BooleanArray* Object::AsBooleanArray() {
  DCHECK(GetClass()->IsArrayClass());
  DCHECK(GetClass()->GetComponentType()->IsPrimitiveBoolean());
  return down_cast<BooleanArray*>(this);
}

inline ByteArray* Object::AsByteArray() {
  DCHECK(GetClass()->IsArrayClass());
  DCHECK(GetClass()->GetComponentType()->IsPrimitiveByte());
  return down_cast<ByteArray*>(this);
}

inline ByteArray* Object::AsByteSizedArray() {
  DCHECK(GetClass()->IsArrayClass());
  DCHECK(GetClass()->GetComponentType()->IsPrimitiveByte() ||
         GetClass()->GetComponentType()->IsPrimitiveBoolean());
  return down_cast<ByteArray*>(this);
}

inline CharArray* Object::AsCharArray() {
  DCHECK(GetClass()->IsArrayClass());
  DCHECK(GetClass()->GetComponentType()->IsPrimitiveChar());
  return down_cast<CharArray*>(this);
}

inline ShortArray* Object::AsShortArray() {
  DCHECK(GetClass()->IsArrayClass());
  DCHECK(GetClass()->GetComponentType()->IsPrimitiveShort());
  return down_cast<ShortArray*>(this);
}

inline ShortArray* Object::AsShortSizedArray() {
  DCHECK(GetClass()->IsArrayClass());
  DCHECK(GetClass()->GetComponentType()->IsPrimitiveShort() ||
         GetClass()->GetComponentType()->IsPrimitiveChar());
  return down_cast<ShortArray*>(this);
}

inline IntArray* Object::AsIntArray() {
  DCHECK(GetClass()->IsArrayClass());
  DCHECK(GetClass()->GetComponentType()->IsPrimitiveInt() ||
         GetClass()->GetComponentType()->IsPrimitiveFloat());
  return down_cast<IntArray*>(this);
}

inline LongArray* Object::AsLongArray() {
  DCHECK(GetClass()->IsArrayClass());
  DCHECK(GetClass()->GetComponentType()->IsPrimitiveLong() ||
         GetClass()->GetComponentType()->IsPrimitiveDouble());
  return down_cast<LongArray*>(this);
}

inline String* Object::AsString() {
  DCHECK(GetClass()->IsStringClass());
  return down_cast<String*>(this);
}

inline Throwable* Object::AsThrowable() {
  DCHECK(GetClass()->IsThrowableClass());
  return down_cast<Throwable*>(this);
}

inline bool Object::IsWeakReferenceInstance() {
  return GetClass()->IsWeakReferenceClass();
}

inline bool Object::IsSoftReferenceInstance() {
  return GetClass()->IsSoftReferenceClass();
}

inline bool Object::IsFinalizerReferenceInstance() {
  return GetClass()->IsFinalizerReferenceClass();
}

inline bool Object::IsPhantomReferenceInstance() {
  return GetClass()->IsPhantomReferenceClass();
}

inline size_t Object::SizeOf() {
  size_t result;
  if (IsArrayInstance()) {
    result = AsArray()->SizeOf();
  } else if (IsClass()) {
    result = AsClass()->SizeOf();
  } else {
    result = GetClass()->GetObjectSize();
  }
  DCHECK_GE(result, sizeof(Object)) << " class=" << PrettyTypeOf(GetClass());
  DCHECK(!IsArtField()  || result == sizeof(ArtField));
  DCHECK(!IsArtMethod() || result == sizeof(ArtMethod));
  return result;
}

inline uint32_t Object::GetField32(MemberOffset field_offset, bool is_volatile) {
  VerifyObject(this);
  const byte* raw_addr = reinterpret_cast<const byte*>(this) + field_offset.Int32Value();
  const int32_t* word_addr = reinterpret_cast<const int32_t*>(raw_addr);
  if (UNLIKELY(is_volatile)) {
    int32_t result = *(reinterpret_cast<volatile int32_t*>(const_cast<int32_t*>(word_addr)));
    QuasiAtomic::MembarLoadLoad();  // Ensure volatile loads don't re-order.
    return result;
  } else {
    return *word_addr;
  }
}

inline void Object::SetField32(MemberOffset field_offset, uint32_t new_value, bool is_volatile,
                               bool this_is_valid) {
  if (this_is_valid) {
    VerifyObject(this);
  }
  byte* raw_addr = reinterpret_cast<byte*>(this) + field_offset.Int32Value();
  uint32_t* word_addr = reinterpret_cast<uint32_t*>(raw_addr);
  if (UNLIKELY(is_volatile)) {
    QuasiAtomic::MembarStoreStore();  // Ensure this store occurs after others in the queue.
    *word_addr = new_value;
    QuasiAtomic::MembarStoreLoad();  // Ensure this store occurs before any volatile loads.
  } else {
    *word_addr = new_value;
  }
}

inline bool Object::CasField32(MemberOffset field_offset, uint32_t old_value, uint32_t new_value) {
  VerifyObject(this);
  byte* raw_addr = reinterpret_cast<byte*>(this) + field_offset.Int32Value();
  volatile uint32_t* addr = reinterpret_cast<volatile uint32_t*>(raw_addr);
  return __sync_bool_compare_and_swap(addr, old_value, new_value);
}

inline uint64_t Object::GetField64(MemberOffset field_offset, bool is_volatile) {
  VerifyObject(this);
  const byte* raw_addr = reinterpret_cast<const byte*>(this) + field_offset.Int32Value();
  const int64_t* addr = reinterpret_cast<const int64_t*>(raw_addr);
  if (UNLIKELY(is_volatile)) {
    uint64_t result = QuasiAtomic::Read64(addr);
    QuasiAtomic::MembarLoadLoad();  // Ensure volatile loads don't re-order.
    return result;
  } else {
    return *addr;
  }
}

inline void Object::SetField64(MemberOffset field_offset, uint64_t new_value, bool is_volatile,
                               bool this_is_valid) {
  if (this_is_valid) {
    VerifyObject(this);
  }
  byte* raw_addr = reinterpret_cast<byte*>(this) + field_offset.Int32Value();
  int64_t* addr = reinterpret_cast<int64_t*>(raw_addr);
  if (UNLIKELY(is_volatile)) {
    QuasiAtomic::MembarStoreStore();  // Ensure this store occurs after others in the queue.
    QuasiAtomic::Write64(addr, new_value);
    if (!QuasiAtomic::LongAtomicsUseMutexes()) {
      QuasiAtomic::MembarStoreLoad();  // Ensure this store occurs before any volatile loads.
    } else {
      // Fence from from mutex is enough.
    }
  } else {
    *addr = new_value;
  }
}

inline bool Object::CasField64(MemberOffset field_offset, uint64_t old_value, uint64_t new_value) {
  VerifyObject(this);
  byte* raw_addr = reinterpret_cast<byte*>(this) + field_offset.Int32Value();
  volatile uint64_t* addr = reinterpret_cast<volatile uint64_t*>(raw_addr);
  return __sync_bool_compare_and_swap(addr, old_value, new_value);
}

template<class T>
inline T* Object::GetFieldObject(MemberOffset field_offset, bool is_volatile) {
  VerifyObject(this);
  byte* raw_addr = reinterpret_cast<byte*>(this) + field_offset.Int32Value();
  HeapReference<T>* objref_addr = reinterpret_cast<HeapReference<T>*>(raw_addr);
  HeapReference<T> objref = *objref_addr;

  if (UNLIKELY(is_volatile)) {
    QuasiAtomic::MembarLoadLoad();  // Ensure loads don't re-order.
  }
  T* result = objref.AsMirrorPtr();
  VerifyObject(result);
  return result;
}

inline void Object::SetFieldObjectWithoutWriteBarrier(MemberOffset field_offset, Object* new_value,
                                                      bool is_volatile, bool this_is_valid) {
  if (this_is_valid) {
    VerifyObject(this);
  }
  VerifyObject(new_value);
  HeapReference<Object> objref(HeapReference<Object>::FromMirrorPtr(new_value));
  byte* raw_addr = reinterpret_cast<byte*>(this) + field_offset.Int32Value();
  HeapReference<Object>* objref_addr = reinterpret_cast<HeapReference<Object>*>(raw_addr);
  if (UNLIKELY(is_volatile)) {
    QuasiAtomic::MembarStoreStore();  // Ensure this store occurs after others in the queue.
    objref_addr->Assign(new_value);
    QuasiAtomic::MembarStoreLoad();  // Ensure this store occurs before any loads.
  } else {
    objref_addr->Assign(new_value);
  }
}

inline void Object::SetFieldObject(MemberOffset field_offset, Object* new_value, bool is_volatile,
                                   bool this_is_valid) {
  SetFieldObjectWithoutWriteBarrier(field_offset, new_value, is_volatile, this_is_valid);
  if (new_value != nullptr) {
    CheckFieldAssignment(field_offset, new_value);
    Runtime::Current()->GetHeap()->WriteBarrierField(this, field_offset, new_value);
  }
}

inline bool Object::CasFieldObject(MemberOffset field_offset, Object* old_value, Object* new_value) {
  VerifyObject(this);
  byte* raw_addr = reinterpret_cast<byte*>(this) + field_offset.Int32Value();
  volatile uint32_t* addr = reinterpret_cast<volatile uint32_t*>(raw_addr);
  HeapReference<Object> old_ref(HeapReference<Object>::FromMirrorPtr(old_value));
  HeapReference<Object> new_ref(HeapReference<Object>::FromMirrorPtr(new_value));
  bool success =  __sync_bool_compare_and_swap(addr, old_ref.reference_, new_ref.reference_);
  if (success) {
    Runtime::Current()->GetHeap()->WriteBarrierField(this, field_offset, new_value);
  }
  return success;
}

inline void Object::VerifyObject(Object* obj) {
  if (kIsDebugBuild) {
    Runtime::Current()->GetHeap()->VerifyObject(obj);
  }
}

}  // namespace mirror
}  // namespace art

#endif  // ART_RUNTIME_MIRROR_OBJECT_INL_H_
