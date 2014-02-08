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

#ifndef ART_RUNTIME_MIRROR_OBJECT_H_
#define ART_RUNTIME_MIRROR_OBJECT_H_

#include "base/casts.h"
#include "base/logging.h"
#include "base/macros.h"
#include "cutils/atomic-inline.h"
#include "object_reference.h"
#include "offsets.h"

namespace art {

class ImageWriter;
class LockWord;
class Monitor;
struct ObjectOffsets;
class Thread;
template <typename T> class SirtRef;

namespace mirror {

class ArtField;
class ArtMethod;
class Array;
class Class;
template<class T> class ObjectArray;
template<class T> class PrimitiveArray;
typedef PrimitiveArray<uint8_t> BooleanArray;
typedef PrimitiveArray<int8_t> ByteArray;
typedef PrimitiveArray<uint16_t> CharArray;
typedef PrimitiveArray<double> DoubleArray;
typedef PrimitiveArray<float> FloatArray;
typedef PrimitiveArray<int32_t> IntArray;
typedef PrimitiveArray<int64_t> LongArray;
typedef PrimitiveArray<int16_t> ShortArray;
class String;
class Throwable;

// Fields within mirror objects aren't accessed directly so that the appropriate amount of
// handshaking is done with GC (for example, read and write barriers). This macro is used to
// compute an offset for the Set/Get methods defined in Object that can safely access fields.
#define OFFSET_OF_OBJECT_MEMBER(type, field) \
    MemberOffset(OFFSETOF_MEMBER(type, field))

constexpr bool kCheckFieldAssignments = false;

// C++ mirror of java.lang.Object
class MANAGED Object {
 public:
  static MemberOffset ClassOffset() {
    return OFFSET_OF_OBJECT_MEMBER(Object, klass_);
  }

  Class* GetClass() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void SetClass(Class* new_klass) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // The verifier treats all interfaces as java.lang.Object and relies on runtime checks in
  // invoke-interface to detect incompatible interface types.
  bool VerifierInstanceOf(Class* klass) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  bool InstanceOf(Class* klass) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  size_t SizeOf() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  Object* Clone(Thread* self) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  int32_t IdentityHashCode() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static MemberOffset MonitorOffset() {
    return OFFSET_OF_OBJECT_MEMBER(Object, monitor_);
  }

  LockWord GetLockWord();
  void SetLockWord(LockWord new_val);
  bool CasLockWord(LockWord old_val, LockWord new_val) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  uint32_t GetLockOwnerThreadId();

  void MonitorEnter(Thread* self) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      EXCLUSIVE_LOCK_FUNCTION(monitor_lock_);

  bool MonitorExit(Thread* self) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      UNLOCK_FUNCTION(monitor_lock_);

  void Notify(Thread* self) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void NotifyAll(Thread* self) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void Wait(Thread* self) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void Wait(Thread* self, int64_t timeout, int32_t nanos) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  bool IsClass() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  Class* AsClass() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  bool IsObjectArray() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  template<class T>
  ObjectArray<T>* AsObjectArray() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  bool IsArrayInstance() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  Array* AsArray() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  BooleanArray* AsBooleanArray() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  ByteArray* AsByteArray() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  ByteArray* AsByteSizedArray() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  CharArray* AsCharArray() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  ShortArray* AsShortArray() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  ShortArray* AsShortSizedArray() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  IntArray* AsIntArray() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  LongArray* AsLongArray() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  String* AsString() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  Throwable* AsThrowable() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  bool IsArtMethod() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  ArtMethod* AsArtMethod() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  bool IsArtField() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  ArtField* AsArtField() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  bool IsReferenceInstance() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  bool IsWeakReferenceInstance() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  bool IsSoftReferenceInstance() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  bool IsFinalizerReferenceInstance() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  bool IsPhantomReferenceInstance() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Accessor for Java type fields.
  template<class T> T* GetFieldObject(MemberOffset field_offset, bool is_volatile)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void SetFieldObjectWithoutWriteBarrier(MemberOffset field_offset, Object* new_value,
                                         bool is_volatile, bool this_is_valid = true)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void SetFieldObject(MemberOffset field_offset, Object* new_value, bool is_volatile,
                      bool this_is_valid = true)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  bool CasFieldObject(MemberOffset field_offset, Object* old_value, Object* new_value)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  HeapReference<Object>* GetFieldObjectReferenceAddr(MemberOffset field_offset) ALWAYS_INLINE {
    VerifyObject(this);
    return reinterpret_cast<HeapReference<Object>*>(reinterpret_cast<byte*>(this) +
        field_offset.Int32Value());
  }

  int32_t GetField32(MemberOffset field_offset, bool is_volatile);

  void SetField32(MemberOffset field_offset, int32_t new_value, bool is_volatile,
                  bool this_is_valid = true);

  bool CasField32(MemberOffset field_offset, int32_t old_value, int32_t new_value)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  int64_t GetField64(MemberOffset field_offset, bool is_volatile);

  void SetField64(MemberOffset field_offset, int64_t new_value, bool is_volatile,
                  bool this_is_valid = true);

  bool CasField64(MemberOffset field_offset, int64_t old_value, int64_t new_value)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  template<typename T>
  void SetFieldPtr(MemberOffset field_offset, T new_value, bool is_volatile,
                   bool this_is_valid = true) {
#ifndef __LP64__
    SetField32(field_offset, reinterpret_cast<int32_t>(new_value), is_volatile, this_is_valid);
#else
    SetField64(field_offset, reinterpret_cast<int64_t>(new_value), is_volatile, this_is_valid);
#endif
  }

 protected:
  // Accessors for non-Java type fields
  template<class T>
  T GetFieldPtr(MemberOffset field_offset, bool is_volatile) {
#ifndef __LP64__
    return reinterpret_cast<T>(GetField32(field_offset, is_volatile));
#else
    return reinterpret_cast<T>(GetField64(field_offset, is_volatile));
#endif
  }

 private:
  static void VerifyObject(Object* obj) ALWAYS_INLINE;
  // Verify the type correctness of stores to fields.
  void CheckFieldAssignmentImpl(MemberOffset field_offset, Object* new_value)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void CheckFieldAssignment(MemberOffset field_offset, Object* new_value)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    if (kCheckFieldAssignments) {
      CheckFieldAssignmentImpl(field_offset, new_value);
    }
  }

  // Generate an identity hash code.
  static int32_t GenerateIdentityHashCode();

  // The Class representing the type of the object.
  HeapReference<Class> klass_;
  // Monitor and hash code information.
  uint32_t monitor_;

  friend class art::ImageWriter;
  friend class art::Monitor;
  friend struct art::ObjectOffsets;  // for verifying offset information
  DISALLOW_IMPLICIT_CONSTRUCTORS(Object);
};

}  // namespace mirror
}  // namespace art

#endif  // ART_RUNTIME_MIRROR_OBJECT_H_
