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
#include "read_barrier-inl.h"
#include "runtime.h"
#include "reference.h"
#include "throwable.h"

namespace art {
namespace mirror {

template<VerifyObjectFlags kVerifyFlags>
inline Class* Object::GetClass() {
  return GetFieldObject<Class, kVerifyFlags>(OFFSET_OF_OBJECT_MEMBER(Object, klass_), false);
}

template<VerifyObjectFlags kVerifyFlags>
inline void Object::SetClass(Class* new_klass) {
  // new_klass may be NULL prior to class linker initialization.
  // We don't mark the card as this occurs as part of object allocation. Not all objects have
  // backing cards, such as large objects.
  // We use non transactional version since we can't undo this write. We also disable checking as
  // we may run in transaction mode here.
  SetFieldObjectWithoutWriteBarrier<false, false,
      static_cast<VerifyObjectFlags>(kVerifyFlags & ~kVerifyThis)>(
      OFFSET_OF_OBJECT_MEMBER(Object, klass_), new_klass, false);
}

inline LockWord Object::GetLockWord(bool as_volatile) {
  return LockWord(GetField32(OFFSET_OF_OBJECT_MEMBER(Object, monitor_), as_volatile));
}

inline void Object::SetLockWord(LockWord new_val, bool as_volatile) {
  // Force use of non-transactional mode and do not check.
  SetField32<false, false>(OFFSET_OF_OBJECT_MEMBER(Object, monitor_), new_val.GetValue(),
                           as_volatile);
}

inline bool Object::CasLockWord(LockWord old_val, LockWord new_val) {
  // Force use of non-transactional mode and do not check.
  return CasField32<false, false>(OFFSET_OF_OBJECT_MEMBER(Object, monitor_), old_val.GetValue(),
                                  new_val.GetValue());
}

inline uint32_t Object::GetLockOwnerThreadId() {
  return Monitor::GetLockOwnerThreadId(this);
}

inline mirror::Object* Object::MonitorEnter(Thread* self) {
  return Monitor::MonitorEnter(self, this);
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

inline Object* Object::GetReadBarrierPointer() {
#ifdef USE_BAKER_OR_BROOKS_READ_BARRIER
  DCHECK(kUseBakerOrBrooksReadBarrier);
  return GetFieldObject<Object, kVerifyNone, false>(OFFSET_OF_OBJECT_MEMBER(Object, x_rb_ptr_), false);
#else
  LOG(FATAL) << "Unreachable";
  return nullptr;
#endif
}

inline void Object::SetReadBarrierPointer(Object* rb_pointer) {
#ifdef USE_BAKER_OR_BROOKS_READ_BARRIER
  DCHECK(kUseBakerOrBrooksReadBarrier);
  // We don't mark the card as this occurs as part of object allocation. Not all objects have
  // backing cards, such as large objects.
  SetFieldObjectWithoutWriteBarrier<false, false, kVerifyNone>(
      OFFSET_OF_OBJECT_MEMBER(Object, x_rb_ptr_), rb_pointer, false);
#else
  LOG(FATAL) << "Unreachable";
#endif
}

inline void Object::AssertReadBarrierPointer() const {
  if (kUseBakerReadBarrier) {
    Object* obj = const_cast<Object*>(this);
    DCHECK(obj->GetReadBarrierPointer() == nullptr)
        << "Bad Baker pointer: obj=" << reinterpret_cast<void*>(obj)
        << " ptr=" << reinterpret_cast<void*>(obj->GetReadBarrierPointer());
  } else if (kUseBrooksReadBarrier) {
    Object* obj = const_cast<Object*>(this);
    DCHECK_EQ(obj, obj->GetReadBarrierPointer())
        << "Bad Brooks pointer: obj=" << reinterpret_cast<void*>(obj)
        << " ptr=" << reinterpret_cast<void*>(obj->GetReadBarrierPointer());
  } else {
    LOG(FATAL) << "Unreachable";
  }
}

template<VerifyObjectFlags kVerifyFlags>
inline bool Object::VerifierInstanceOf(Class* klass) {
  DCHECK(klass != NULL);
  DCHECK(GetClass<kVerifyFlags>() != NULL);
  return klass->IsInterface() || InstanceOf(klass);
}

template<VerifyObjectFlags kVerifyFlags>
inline bool Object::InstanceOf(Class* klass) {
  DCHECK(klass != NULL);
  DCHECK(GetClass<kVerifyNone>() != NULL);
  return klass->IsAssignableFrom(GetClass<kVerifyFlags>());
}

template<VerifyObjectFlags kVerifyFlags>
inline bool Object::IsClass() {
  Class* java_lang_Class = GetClass<kVerifyFlags>()->GetClass();
  return GetClass<static_cast<VerifyObjectFlags>(kVerifyFlags & ~kVerifyThis)>() ==
      java_lang_Class;
}

template<VerifyObjectFlags kVerifyFlags>
inline Class* Object::AsClass() {
  DCHECK(IsClass<kVerifyFlags>());
  return down_cast<Class*>(this);
}

template<VerifyObjectFlags kVerifyFlags>
inline bool Object::IsObjectArray() {
  constexpr auto kNewFlags = static_cast<VerifyObjectFlags>(kVerifyFlags & ~kVerifyThis);
  return IsArrayInstance<kVerifyFlags>() &&
      !GetClass<kNewFlags>()->template GetComponentType<kNewFlags>()->IsPrimitive();
}

template<class T, VerifyObjectFlags kVerifyFlags>
inline ObjectArray<T>* Object::AsObjectArray() {
  DCHECK(IsObjectArray<kVerifyFlags>());
  return down_cast<ObjectArray<T>*>(this);
}

template<VerifyObjectFlags kVerifyFlags>
inline bool Object::IsArrayInstance() {
  return GetClass<kVerifyFlags>()->IsArrayClass();
}

template<VerifyObjectFlags kVerifyFlags>
inline bool Object::IsArtField() {
  return GetClass<kVerifyFlags>()->IsArtFieldClass();
}

template<VerifyObjectFlags kVerifyFlags>
inline ArtField* Object::AsArtField() {
  DCHECK(IsArtField<kVerifyFlags>());
  return down_cast<ArtField*>(this);
}

template<VerifyObjectFlags kVerifyFlags>
inline bool Object::IsArtMethod() {
  return GetClass<kVerifyFlags>()->IsArtMethodClass();
}

template<VerifyObjectFlags kVerifyFlags>
inline ArtMethod* Object::AsArtMethod() {
  DCHECK(IsArtMethod<kVerifyFlags>());
  return down_cast<ArtMethod*>(this);
}

template<VerifyObjectFlags kVerifyFlags>
inline bool Object::IsReferenceInstance() {
  return GetClass<kVerifyFlags>()->IsReferenceClass();
}

template<VerifyObjectFlags kVerifyFlags>
inline Reference* Object::AsReference() {
  DCHECK(IsReferenceInstance<kVerifyFlags>());
  return down_cast<Reference*>(this);
}

template<VerifyObjectFlags kVerifyFlags>
inline Array* Object::AsArray() {
  DCHECK(IsArrayInstance<kVerifyFlags>());
  return down_cast<Array*>(this);
}

template<VerifyObjectFlags kVerifyFlags>
inline BooleanArray* Object::AsBooleanArray() {
  constexpr auto kNewFlags = static_cast<VerifyObjectFlags>(kVerifyFlags & ~kVerifyThis);
  DCHECK(GetClass<kVerifyFlags>()->IsArrayClass());
  DCHECK(GetClass<kNewFlags>()->GetComponentType()->IsPrimitiveBoolean());
  return down_cast<BooleanArray*>(this);
}

template<VerifyObjectFlags kVerifyFlags>
inline ByteArray* Object::AsByteArray() {
  static const VerifyObjectFlags kNewFlags = static_cast<VerifyObjectFlags>(kVerifyFlags & ~kVerifyThis);
  DCHECK(GetClass<kVerifyFlags>()->IsArrayClass());
  DCHECK(GetClass<kNewFlags>()->template GetComponentType<kNewFlags>()->IsPrimitiveByte());
  return down_cast<ByteArray*>(this);
}

template<VerifyObjectFlags kVerifyFlags>
inline ByteArray* Object::AsByteSizedArray() {
  constexpr VerifyObjectFlags kNewFlags = static_cast<VerifyObjectFlags>(kVerifyFlags & ~kVerifyThis);
  DCHECK(GetClass<kVerifyFlags>()->IsArrayClass());
  DCHECK(GetClass<kNewFlags>()->template GetComponentType<kNewFlags>()->IsPrimitiveByte() ||
         GetClass<kNewFlags>()->template GetComponentType<kNewFlags>()->IsPrimitiveBoolean());
  return down_cast<ByteArray*>(this);
}

template<VerifyObjectFlags kVerifyFlags>
inline CharArray* Object::AsCharArray() {
  constexpr auto kNewFlags = static_cast<VerifyObjectFlags>(kVerifyFlags & ~kVerifyThis);
  DCHECK(GetClass<kVerifyFlags>()->IsArrayClass());
  DCHECK(GetClass<kNewFlags>()->template GetComponentType<kNewFlags>()->IsPrimitiveChar());
  return down_cast<CharArray*>(this);
}

template<VerifyObjectFlags kVerifyFlags>
inline ShortArray* Object::AsShortArray() {
  constexpr auto kNewFlags = static_cast<VerifyObjectFlags>(kVerifyFlags & ~kVerifyThis);
  DCHECK(GetClass<kVerifyFlags>()->IsArrayClass());
  DCHECK(GetClass<kNewFlags>()->template GetComponentType<kNewFlags>()->IsPrimitiveShort());
  return down_cast<ShortArray*>(this);
}

template<VerifyObjectFlags kVerifyFlags>
inline ShortArray* Object::AsShortSizedArray() {
  constexpr auto kNewFlags = static_cast<VerifyObjectFlags>(kVerifyFlags & ~kVerifyThis);
  DCHECK(GetClass<kVerifyFlags>()->IsArrayClass());
  DCHECK(GetClass<kNewFlags>()->template GetComponentType<kNewFlags>()->IsPrimitiveShort() ||
         GetClass<kNewFlags>()->template GetComponentType<kNewFlags>()->IsPrimitiveChar());
  return down_cast<ShortArray*>(this);
}

template<VerifyObjectFlags kVerifyFlags>
inline IntArray* Object::AsIntArray() {
  constexpr auto kNewFlags = static_cast<VerifyObjectFlags>(kVerifyFlags & ~kVerifyThis);
  DCHECK(GetClass<kVerifyFlags>()->IsArrayClass());
  DCHECK(GetClass<kNewFlags>()->template GetComponentType<kNewFlags>()->IsPrimitiveInt() ||
         GetClass<kNewFlags>()->template GetComponentType<kNewFlags>()->IsPrimitiveFloat());
  return down_cast<IntArray*>(this);
}

template<VerifyObjectFlags kVerifyFlags>
inline LongArray* Object::AsLongArray() {
  constexpr auto kNewFlags = static_cast<VerifyObjectFlags>(kVerifyFlags & ~kVerifyThis);
  DCHECK(GetClass<kVerifyFlags>()->IsArrayClass());
  DCHECK(GetClass<kNewFlags>()->template GetComponentType<kNewFlags>()->IsPrimitiveLong() ||
         GetClass<kNewFlags>()->template GetComponentType<kNewFlags>()->IsPrimitiveDouble());
  return down_cast<LongArray*>(this);
}

template<VerifyObjectFlags kVerifyFlags>
inline FloatArray* Object::AsFloatArray() {
  constexpr auto kNewFlags = static_cast<VerifyObjectFlags>(kVerifyFlags & ~kVerifyThis);
  DCHECK(GetClass<kVerifyFlags>()->IsArrayClass());
  DCHECK(GetClass<kNewFlags>()->template GetComponentType<kNewFlags>()->IsPrimitiveFloat());
  return down_cast<FloatArray*>(this);
}

template<VerifyObjectFlags kVerifyFlags>
inline DoubleArray* Object::AsDoubleArray() {
  constexpr auto kNewFlags = static_cast<VerifyObjectFlags>(kVerifyFlags & ~kVerifyThis);
  DCHECK(GetClass<kVerifyFlags>()->IsArrayClass());
  DCHECK(GetClass<kNewFlags>()->template GetComponentType<kNewFlags>()->IsPrimitiveDouble());
  return down_cast<DoubleArray*>(this);
}

template<VerifyObjectFlags kVerifyFlags>
inline String* Object::AsString() {
  DCHECK(GetClass<kVerifyFlags>()->IsStringClass());
  return down_cast<String*>(this);
}

template<VerifyObjectFlags kVerifyFlags>
inline Throwable* Object::AsThrowable() {
  DCHECK(GetClass<kVerifyFlags>()->IsThrowableClass());
  return down_cast<Throwable*>(this);
}

template<VerifyObjectFlags kVerifyFlags>
inline bool Object::IsWeakReferenceInstance() {
  return GetClass<kVerifyFlags>()->IsWeakReferenceClass();
}

template<VerifyObjectFlags kVerifyFlags>
inline bool Object::IsSoftReferenceInstance() {
  return GetClass<kVerifyFlags>()->IsSoftReferenceClass();
}

template<VerifyObjectFlags kVerifyFlags>
inline bool Object::IsFinalizerReferenceInstance() {
  return GetClass<kVerifyFlags>()->IsFinalizerReferenceClass();
}

template<VerifyObjectFlags kVerifyFlags>
inline FinalizerReference* Object::AsFinalizerReference() {
  DCHECK(IsFinalizerReferenceInstance<kVerifyFlags>());
  return down_cast<FinalizerReference*>(this);
}

template<VerifyObjectFlags kVerifyFlags>
inline bool Object::IsPhantomReferenceInstance() {
  return GetClass<kVerifyFlags>()->IsPhantomReferenceClass();
}

template<VerifyObjectFlags kVerifyFlags>
inline size_t Object::SizeOf() {
  size_t result;
  constexpr auto kNewFlags = static_cast<VerifyObjectFlags>(kVerifyFlags & ~kVerifyThis);
  if (IsArrayInstance<kVerifyFlags>()) {
    result = AsArray<kNewFlags>()->template SizeOf<kNewFlags>();
  } else if (IsClass<kNewFlags>()) {
    result = AsClass<kNewFlags>()->template SizeOf<kNewFlags>();
  } else {
    result = GetClass<kNewFlags>()->GetObjectSize();
  }
  DCHECK_GE(result, sizeof(Object)) << " class=" << PrettyTypeOf(GetClass<kNewFlags>());
  DCHECK(!IsArtField<kNewFlags>()  || result == sizeof(ArtField));
  DCHECK(!IsArtMethod<kNewFlags>() || result == sizeof(ArtMethod));
  return result;
}

template<VerifyObjectFlags kVerifyFlags>
inline int32_t Object::GetField32(MemberOffset field_offset, bool is_volatile) {
  if (kVerifyFlags & kVerifyThis) {
    VerifyObject(this);
  }
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

template<bool kTransactionActive, bool kCheckTransaction, VerifyObjectFlags kVerifyFlags>
inline void Object::SetField32(MemberOffset field_offset, int32_t new_value, bool is_volatile) {
  if (kCheckTransaction) {
    DCHECK_EQ(kTransactionActive, Runtime::Current()->IsActiveTransaction());
  }
  if (kTransactionActive) {
    Runtime::Current()->RecordWriteField32(this, field_offset, GetField32(field_offset, is_volatile),
                                           is_volatile);
  }
  if (kVerifyFlags & kVerifyThis) {
    VerifyObject(this);
  }
  byte* raw_addr = reinterpret_cast<byte*>(this) + field_offset.Int32Value();
  int32_t* word_addr = reinterpret_cast<int32_t*>(raw_addr);
  if (UNLIKELY(is_volatile)) {
    QuasiAtomic::MembarStoreStore();  // Ensure this store occurs after others in the queue.
    *word_addr = new_value;
    QuasiAtomic::MembarStoreLoad();  // Ensure this store occurs before any volatile loads.
  } else {
    *word_addr = new_value;
  }
}

template<bool kTransactionActive, bool kCheckTransaction, VerifyObjectFlags kVerifyFlags>
inline bool Object::CasField32(MemberOffset field_offset, int32_t old_value, int32_t new_value) {
  if (kCheckTransaction) {
    DCHECK_EQ(kTransactionActive, Runtime::Current()->IsActiveTransaction());
  }
  if (kTransactionActive) {
    Runtime::Current()->RecordWriteField32(this, field_offset, old_value, true);
  }
  if (kVerifyFlags & kVerifyThis) {
    VerifyObject(this);
  }
  byte* raw_addr = reinterpret_cast<byte*>(this) + field_offset.Int32Value();
  volatile int32_t* addr = reinterpret_cast<volatile int32_t*>(raw_addr);
  return __sync_bool_compare_and_swap(addr, old_value, new_value);
}

template<VerifyObjectFlags kVerifyFlags>
inline int64_t Object::GetField64(MemberOffset field_offset, bool is_volatile) {
  if (kVerifyFlags & kVerifyThis) {
    VerifyObject(this);
  }
  const byte* raw_addr = reinterpret_cast<const byte*>(this) + field_offset.Int32Value();
  const int64_t* addr = reinterpret_cast<const int64_t*>(raw_addr);
  if (UNLIKELY(is_volatile)) {
    int64_t result = QuasiAtomic::Read64(addr);
    QuasiAtomic::MembarLoadLoad();  // Ensure volatile loads don't re-order.
    return result;
  } else {
    return *addr;
  }
}

template<bool kTransactionActive, bool kCheckTransaction, VerifyObjectFlags kVerifyFlags>
inline void Object::SetField64(MemberOffset field_offset, int64_t new_value, bool is_volatile) {
  if (kCheckTransaction) {
    DCHECK_EQ(kTransactionActive, Runtime::Current()->IsActiveTransaction());
  }
  if (kTransactionActive) {
    Runtime::Current()->RecordWriteField64(this, field_offset, GetField64(field_offset, is_volatile),
                                           is_volatile);
  }
  if (kVerifyFlags & kVerifyThis) {
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

template<bool kTransactionActive, bool kCheckTransaction, VerifyObjectFlags kVerifyFlags>
inline bool Object::CasField64(MemberOffset field_offset, int64_t old_value, int64_t new_value) {
  if (kCheckTransaction) {
    DCHECK_EQ(kTransactionActive, Runtime::Current()->IsActiveTransaction());
  }
  if (kTransactionActive) {
    Runtime::Current()->RecordWriteField64(this, field_offset, old_value, true);
  }
  if (kVerifyFlags & kVerifyThis) {
    VerifyObject(this);
  }
  byte* raw_addr = reinterpret_cast<byte*>(this) + field_offset.Int32Value();
  volatile int64_t* addr = reinterpret_cast<volatile int64_t*>(raw_addr);
  return QuasiAtomic::Cas64(old_value, new_value, addr);
}

template<class T, VerifyObjectFlags kVerifyFlags, bool kDoReadBarrier>
inline T* Object::GetFieldObject(MemberOffset field_offset, bool is_volatile) {
  if (kVerifyFlags & kVerifyThis) {
    VerifyObject(this);
  }
  byte* raw_addr = reinterpret_cast<byte*>(this) + field_offset.Int32Value();
  HeapReference<T>* objref_addr = reinterpret_cast<HeapReference<T>*>(raw_addr);
  T* result = ReadBarrier::Barrier<T, kDoReadBarrier>(this, field_offset, objref_addr);
  if (UNLIKELY(is_volatile)) {
    QuasiAtomic::MembarLoadLoad();  // Ensure loads don't re-order.
  }
  if (kVerifyFlags & kVerifyReads) {
    VerifyObject(result);
  }
  return result;
}

template<bool kTransactionActive, bool kCheckTransaction, VerifyObjectFlags kVerifyFlags>
inline void Object::SetFieldObjectWithoutWriteBarrier(MemberOffset field_offset, Object* new_value,
                                                      bool is_volatile) {
  if (kCheckTransaction) {
    DCHECK_EQ(kTransactionActive, Runtime::Current()->IsActiveTransaction());
  }
  if (kTransactionActive) {
    Runtime::Current()->RecordWriteFieldReference(this, field_offset,
                                                  GetFieldObject<Object>(field_offset, is_volatile),
                                                  true);
  }
  if (kVerifyFlags & kVerifyThis) {
    VerifyObject(this);
  }
  if (kVerifyFlags & kVerifyWrites) {
    VerifyObject(new_value);
  }
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

template<bool kTransactionActive, bool kCheckTransaction, VerifyObjectFlags kVerifyFlags>
inline void Object::SetFieldObject(MemberOffset field_offset, Object* new_value, bool is_volatile) {
  SetFieldObjectWithoutWriteBarrier<kTransactionActive, kCheckTransaction, kVerifyFlags>(
      field_offset, new_value, is_volatile);
  if (new_value != nullptr) {
    CheckFieldAssignment(field_offset, new_value);
    Runtime::Current()->GetHeap()->WriteBarrierField(this, field_offset, new_value);
  }
}

template <VerifyObjectFlags kVerifyFlags>
inline HeapReference<Object>* Object::GetFieldObjectReferenceAddr(MemberOffset field_offset) {
  if (kVerifyFlags & kVerifyThis) {
    VerifyObject(this);
  }
  return reinterpret_cast<HeapReference<Object>*>(reinterpret_cast<byte*>(this) +
      field_offset.Int32Value());
}

template<bool kTransactionActive, bool kCheckTransaction, VerifyObjectFlags kVerifyFlags>
inline bool Object::CasFieldObject(MemberOffset field_offset, Object* old_value,
                                   Object* new_value) {
  if (kCheckTransaction) {
    DCHECK_EQ(kTransactionActive, Runtime::Current()->IsActiveTransaction());
  }
  if (kVerifyFlags & kVerifyThis) {
    VerifyObject(this);
  }
  if (kVerifyFlags & kVerifyWrites) {
    VerifyObject(new_value);
  }
  if (kVerifyFlags & kVerifyReads) {
    VerifyObject(old_value);
  }
  if (kTransactionActive) {
    Runtime::Current()->RecordWriteFieldReference(this, field_offset, old_value, true);
  }
  byte* raw_addr = reinterpret_cast<byte*>(this) + field_offset.Int32Value();
  volatile int32_t* addr = reinterpret_cast<volatile int32_t*>(raw_addr);
  HeapReference<Object> old_ref(HeapReference<Object>::FromMirrorPtr(old_value));
  HeapReference<Object> new_ref(HeapReference<Object>::FromMirrorPtr(new_value));
  bool success =  __sync_bool_compare_and_swap(addr, old_ref.reference_, new_ref.reference_);
  if (success) {
    Runtime::Current()->GetHeap()->WriteBarrierField(this, field_offset, new_value);
  }
  return success;
}

template<bool kVisitClass, bool kIsStatic, typename Visitor>
inline void Object::VisitFieldsReferences(uint32_t ref_offsets, const Visitor& visitor) {
  if (LIKELY(ref_offsets != CLASS_WALK_SUPER)) {
    if (!kVisitClass) {
     // Mask out the class from the reference offsets.
      ref_offsets ^= kWordHighBitMask;
    }
    DCHECK_EQ(ClassOffset().Uint32Value(), 0U);
    // Found a reference offset bitmap. Visit the specified offsets.
    while (ref_offsets != 0) {
      size_t right_shift = CLZ(ref_offsets);
      MemberOffset field_offset = CLASS_OFFSET_FROM_CLZ(right_shift);
      visitor(this, field_offset, kIsStatic);
      ref_offsets &= ~(CLASS_HIGH_BIT >> right_shift);
    }
  } else {
    // There is no reference offset bitmap.  In the non-static case, walk up the class
    // inheritance hierarchy and find reference offsets the hard way. In the static case, just
    // consider this class.
    for (mirror::Class* klass = kIsStatic ? AsClass() : GetClass(); klass != nullptr;
        klass = kIsStatic ? nullptr : klass->GetSuperClass()) {
      size_t num_reference_fields =
          kIsStatic ? klass->NumReferenceStaticFields() : klass->NumReferenceInstanceFields();
      for (size_t i = 0; i < num_reference_fields; ++i) {
        mirror::ArtField* field = kIsStatic ? klass->GetStaticField(i)
            : klass->GetInstanceField(i);
        MemberOffset field_offset = field->GetOffset();
        // TODO: Do a simpler check?
        if (!kVisitClass && UNLIKELY(field_offset.Uint32Value() == ClassOffset().Uint32Value())) {
          continue;
        }
        visitor(this, field_offset, kIsStatic);
      }
    }
  }
}

template<bool kVisitClass, typename Visitor>
inline void Object::VisitInstanceFieldsReferences(mirror::Class* klass, const Visitor& visitor) {
  VisitFieldsReferences<kVisitClass, false>(
      klass->GetReferenceInstanceOffsets<kVerifyNone>(), visitor);
}

template<bool kVisitClass, typename Visitor>
inline void Object::VisitStaticFieldsReferences(mirror::Class* klass, const Visitor& visitor) {
  klass->VisitFieldsReferences<kVisitClass, true>(
      klass->GetReferenceStaticOffsets<kVerifyNone>(), visitor);
}

template <const bool kVisitClass, VerifyObjectFlags kVerifyFlags, typename Visitor,
    typename JavaLangRefVisitor>
inline void Object::VisitReferences(const Visitor& visitor,
                                    const JavaLangRefVisitor& ref_visitor) {
  mirror::Class* klass = GetClass<kVerifyFlags>();
  if (UNLIKELY(klass == Class::GetJavaLangClass())) {
    DCHECK_EQ(klass->GetClass<kVerifyNone>(), Class::GetJavaLangClass());
    AsClass<kVerifyNone>()->VisitReferences<kVisitClass>(klass, visitor);
  } else if (UNLIKELY(klass->IsArrayClass<kVerifyFlags>())) {
    if (klass->IsObjectArrayClass<kVerifyNone>()) {
      AsObjectArray<mirror::Object, kVerifyNone>()->VisitReferences<kVisitClass>(visitor);
    } else if (kVisitClass) {
      visitor(this, ClassOffset(), false);
    }
  } else {
    VisitInstanceFieldsReferences<kVisitClass>(klass, visitor);
    if (UNLIKELY(klass->IsReferenceClass<kVerifyNone>())) {
      ref_visitor(klass, AsReference());
    }
  }
}

}  // namespace mirror
}  // namespace art

#endif  // ART_RUNTIME_MIRROR_OBJECT_INL_H_
