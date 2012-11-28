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

#ifndef ART_SRC_OBJECT_H_
#define ART_SRC_OBJECT_H_

#include <iosfwd>
#include <vector>

#include "UniquePtr.h"
#include "atomic.h"
#include "casts.h"
#include "globals.h"
#include "heap.h"
#include "invoke_type.h"
#include "logging.h"
#include "macros.h"
#include "modifiers.h"
#include "offsets.h"
#include "primitive.h"
#include "runtime.h"
#include "stringpiece.h"
#include "thread.h"
#include "utf.h"

namespace art {

class Array;
class Class;
class ClassLoader;
class CodeAndDirectMethods;
class DexCache;
class Field;
class IfTable;
class Monitor;
class Member;
class AbstractMethod;
class Object;
class StaticStorageBase;
class String;
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

union JValue {
  // We default initialize JValue instances to all-zeros.
  JValue() : j(0) {}

  int8_t GetB() const { return b; }
  void SetB(int8_t new_b) {
    i = ((static_cast<int32_t>(new_b) << 24) >> 24); // Sign-extend.
  }

  uint16_t GetC() const { return c; }
  void SetC(uint16_t new_c) { c = new_c; }

  double GetD() const { return d; }
  void SetD(double new_d) { d = new_d; }

  float GetF() const { return f; }
  void SetF(float new_f) { f = new_f; }

  int32_t GetI() const { return i; }
  void SetI(int32_t new_i) { i = new_i; }

  int64_t GetJ() const { return j; }
  void SetJ(int64_t new_j) { j = new_j; }

  Object* GetL() const { return l; }
  void SetL(Object* new_l) { l = new_l; }

  int16_t GetS() const { return s; }
  void SetS(int16_t new_s) {
    i = ((static_cast<int32_t>(new_s) << 16) >> 16); // Sign-extend.
  }

  uint8_t GetZ() const { return z; }
  void SetZ(uint8_t new_z) { z = new_z; }

 private:
  uint8_t z;
  int8_t b;
  uint16_t c;
  int16_t s;
  int32_t i;
  int64_t j;
  float f;
  double d;
  Object* l;
};

#if defined(ART_USE_LLVM_COMPILER)
namespace compiler_llvm {
  class InferredRegCategoryMap;
} // namespace compiler_llvm
#endif

/*
 * Definitions for packing refOffsets in Class.
 */
/*
 * A magic value for refOffsets. Ignore the bits and walk the super
 * chain when this is the value.
 * [This is an unlikely "natural" value, since it would be 30 non-ref instance
 * fields followed by 2 ref instance fields.]
 */
#define CLASS_WALK_SUPER ((unsigned int)(3))
#define CLASS_BITS_PER_WORD (sizeof(unsigned long int) * 8)
#define CLASS_OFFSET_ALIGNMENT 4
#define CLASS_HIGH_BIT ((unsigned int)1 << (CLASS_BITS_PER_WORD - 1))
/*
 * Given an offset, return the bit number which would encode that offset.
 * Local use only.
 */
#define _CLASS_BIT_NUMBER_FROM_OFFSET(byteOffset) \
    ((unsigned int)(byteOffset) / \
     CLASS_OFFSET_ALIGNMENT)
/*
 * Is the given offset too large to be encoded?
 */
#define CLASS_CAN_ENCODE_OFFSET(byteOffset) \
    (_CLASS_BIT_NUMBER_FROM_OFFSET(byteOffset) < CLASS_BITS_PER_WORD)
/*
 * Return a single bit, encoding the offset.
 * Undefined if the offset is too large, as defined above.
 */
#define CLASS_BIT_FROM_OFFSET(byteOffset) \
    (CLASS_HIGH_BIT >> _CLASS_BIT_NUMBER_FROM_OFFSET(byteOffset))
/*
 * Return an offset, given a bit number as returned from CLZ.
 */
#define CLASS_OFFSET_FROM_CLZ(rshift) \
    MemberOffset((static_cast<int>(rshift) * CLASS_OFFSET_ALIGNMENT))

#define OFFSET_OF_OBJECT_MEMBER(type, field) \
    MemberOffset(OFFSETOF_MEMBER(type, field))

// Classes shared with the managed side of the world need to be packed
// so that they don't have extra platform specific padding.
#define MANAGED PACKED(4)

// C++ mirror of java.lang.Object
class MANAGED Object {
 public:
  static MemberOffset ClassOffset() {
    return OFFSET_OF_OBJECT_MEMBER(Object, klass_);
  }

  Class* GetClass() const {
    return GetFieldObject<Class*>(OFFSET_OF_OBJECT_MEMBER(Object, klass_), false);
  }

  void SetClass(Class* new_klass);

  bool InstanceOf(const Class* klass) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  size_t SizeOf() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  Object* Clone(Thread* self) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  int32_t IdentityHashCode() const {
  #ifdef MOVING_GARBAGE_COLLECTOR
    // TODO: we'll need to use the Object's internal concept of identity
      UNIMPLEMENTED(FATAL);
  #endif
    return reinterpret_cast<int32_t>(this);
  }

  static MemberOffset MonitorOffset() {
    return OFFSET_OF_OBJECT_MEMBER(Object, monitor_);
  }

  volatile int32_t* GetRawLockWordAddress() {
    byte* raw_addr = reinterpret_cast<byte*>(this) +
        OFFSET_OF_OBJECT_MEMBER(Object, monitor_).Int32Value();
    int32_t* word_addr = reinterpret_cast<int32_t*>(raw_addr);
    return const_cast<volatile int32_t*>(word_addr);
  }

  uint32_t GetThinLockId();

  void MonitorEnter(Thread* thread) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      EXCLUSIVE_LOCK_FUNCTION(monitor_lock_);

  bool MonitorExit(Thread* thread) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      UNLOCK_FUNCTION(monitor_lock_);

  void Notify() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void NotifyAll() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void Wait(int64_t timeout) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void Wait(int64_t timeout, int32_t nanos)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  bool IsClass() const;

  Class* AsClass() {
    DCHECK(IsClass());
    return down_cast<Class*>(this);
  }

  const Class* AsClass() const {
    DCHECK(IsClass());
    return down_cast<const Class*>(this);
  }

  bool IsObjectArray() const;

  template<class T>
  ObjectArray<T>* AsObjectArray();

  template<class T>
  const ObjectArray<T>* AsObjectArray() const;

  bool IsArrayInstance() const;

  Array* AsArray() {
    DCHECK(IsArrayInstance());
    return down_cast<Array*>(this);
  }

  const Array* AsArray() const {
    DCHECK(IsArrayInstance());
    return down_cast<const Array*>(this);
  }

  BooleanArray* AsBooleanArray();
  ByteArray* AsByteArray();
  CharArray* AsCharArray();
  ShortArray* AsShortArray();
  IntArray* AsIntArray();
  LongArray* AsLongArray();

  String* AsString();

  Throwable* AsThrowable() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  bool IsMethod() const;

  AbstractMethod* AsMethod() {
    DCHECK(IsMethod());
    return down_cast<AbstractMethod*>(this);
  }

  const AbstractMethod* AsMethod() const {
    DCHECK(IsMethod());
    return down_cast<const AbstractMethod*>(this);
  }

  bool IsField() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  Field* AsField() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(IsField());
    return down_cast<Field*>(this);
  }

  const Field* AsField() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(IsField());
    return down_cast<const Field*>(this);
  }

  bool IsReferenceInstance() const;

  bool IsWeakReferenceInstance() const;

  bool IsSoftReferenceInstance() const;

  bool IsFinalizerReferenceInstance() const;

  bool IsPhantomReferenceInstance() const;

  // Accessors for Java type fields
  template<class T>
  T GetFieldObject(MemberOffset field_offset, bool is_volatile) const {
    T result = reinterpret_cast<T>(GetField32(field_offset, is_volatile));
    Runtime::Current()->GetHeap()->VerifyObject(result);
    return result;
  }

  void SetFieldObject(MemberOffset field_offset, const Object* new_value, bool is_volatile,
                      bool this_is_valid = true) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    Runtime::Current()->GetHeap()->VerifyObject(new_value);
    SetField32(field_offset, reinterpret_cast<uint32_t>(new_value), is_volatile, this_is_valid);
    if (new_value != NULL) {
      CheckFieldAssignment(field_offset, new_value);
      Runtime::Current()->GetHeap()->WriteBarrierField(this, field_offset, new_value);
    }
  }

  uint32_t GetField32(MemberOffset field_offset, bool is_volatile) const {
    Runtime::Current()->GetHeap()->VerifyObject(this);
    const byte* raw_addr = reinterpret_cast<const byte*>(this) + field_offset.Int32Value();
    const int32_t* word_addr = reinterpret_cast<const int32_t*>(raw_addr);
    if (UNLIKELY(is_volatile)) {
      return android_atomic_acquire_load(word_addr);
    } else {
      return *word_addr;
    }
  }

  void SetField32(MemberOffset field_offset, uint32_t new_value, bool is_volatile, bool this_is_valid = true) {
    if (this_is_valid) {
      Runtime::Current()->GetHeap()->VerifyObject(this);
    }
    byte* raw_addr = reinterpret_cast<byte*>(this) + field_offset.Int32Value();
    uint32_t* word_addr = reinterpret_cast<uint32_t*>(raw_addr);
    if (UNLIKELY(is_volatile)) {
      /*
       * TODO: add an android_atomic_synchronization_store() function and
       * use it in the 32-bit volatile set handlers.  On some platforms we
       * can use a fast atomic instruction and avoid the barriers.
       */
      ANDROID_MEMBAR_STORE();
      *word_addr = new_value;
      ANDROID_MEMBAR_FULL();
    } else {
      *word_addr = new_value;
    }
  }

  uint64_t GetField64(MemberOffset field_offset, bool is_volatile) const {
    Runtime::Current()->GetHeap()->VerifyObject(this);
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

  void SetField64(MemberOffset field_offset, uint64_t new_value, bool is_volatile) {
    Runtime::Current()->GetHeap()->VerifyObject(this);
    byte* raw_addr = reinterpret_cast<byte*>(this) + field_offset.Int32Value();
    int64_t* addr = reinterpret_cast<int64_t*>(raw_addr);
    if (UNLIKELY(is_volatile)) {
      ANDROID_MEMBAR_STORE();
      QuasiAtomic::Swap64(new_value, addr);
      // Post-store barrier not required due to use of atomic op or mutex.
    } else {
      *addr = new_value;
    }
  }

 protected:
  // Accessors for non-Java type fields
  template<class T>
  T GetFieldPtr(MemberOffset field_offset, bool is_volatile) const {
    return reinterpret_cast<T>(GetField32(field_offset, is_volatile));
  }

  template<typename T>
  void SetFieldPtr(MemberOffset field_offset, T new_value, bool is_volatile, bool this_is_valid = true) {
    SetField32(field_offset, reinterpret_cast<uint32_t>(new_value), is_volatile, this_is_valid);
  }

 private:
#if VERIFY_OBJECT_ENABLED
  void CheckFieldAssignment(MemberOffset field_offset, const Object* new_value)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
#else
  void CheckFieldAssignment(MemberOffset, const Object*)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {}
#endif

  Class* klass_;

  uint32_t monitor_;

  friend class ImageWriter;  // for abusing monitor_ directly
  friend struct ObjectOffsets;  // for verifying offset information
  DISALLOW_IMPLICIT_CONSTRUCTORS(Object);
};

// C++ mirror of java.lang.reflect.Field
class MANAGED Field : public Object {
 public:
  Class* GetDeclaringClass() const;

  void SetDeclaringClass(Class *new_declaring_class) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  uint32_t GetAccessFlags() const;

  void SetAccessFlags(uint32_t new_access_flags) {
    SetField32(OFFSET_OF_OBJECT_MEMBER(Field, access_flags_), new_access_flags, false);
  }

  bool IsPublic() const {
    return (GetAccessFlags() & kAccPublic) != 0;
  }

  bool IsStatic() const {
    return (GetAccessFlags() & kAccStatic) != 0;
  }

  bool IsFinal() const {
    return (GetAccessFlags() & kAccFinal) != 0;
  }

  uint32_t GetDexFieldIndex() const {
    return GetField32(OFFSET_OF_OBJECT_MEMBER(Field, field_dex_idx_), false);
  }

  void SetDexFieldIndex(uint32_t new_idx) {
    SetField32(OFFSET_OF_OBJECT_MEMBER(Field, field_dex_idx_), new_idx, false);
  }

  // Offset to field within an Object
  MemberOffset GetOffset() const;

  static MemberOffset OffsetOffset() {
    return MemberOffset(OFFSETOF_MEMBER(Field, offset_));
  }

  MemberOffset GetOffsetDuringLinking() const;

  void SetOffset(MemberOffset num_bytes);

  // field access, null object for static fields
  bool GetBoolean(const Object* object) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void SetBoolean(Object* object, bool z) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  int8_t GetByte(const Object* object) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void SetByte(Object* object, int8_t b) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  uint16_t GetChar(const Object* object) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void SetChar(Object* object, uint16_t c) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  int16_t GetShort(const Object* object) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void SetShort(Object* object, int16_t s) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  int32_t GetInt(const Object* object) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void SetInt(Object* object, int32_t i) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  int64_t GetLong(const Object* object) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void SetLong(Object* object, int64_t j) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  float GetFloat(const Object* object) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void SetFloat(Object* object, float f) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  double GetDouble(const Object* object) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void SetDouble(Object* object, double d) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  Object* GetObject(const Object* object) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void SetObject(Object* object, const Object* l) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // raw field accesses
  uint32_t Get32(const Object* object) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void Set32(Object* object, uint32_t new_value) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  uint64_t Get64(const Object* object) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void Set64(Object* object, uint64_t new_value) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  Object* GetObj(const Object* object) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void SetObj(Object* object, const Object* new_value) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static Class* GetJavaLangReflectField() {
    DCHECK(java_lang_reflect_Field_ != NULL);
    return java_lang_reflect_Field_;
  }

  static void SetClass(Class* java_lang_reflect_Field);
  static void ResetClass();

  bool IsVolatile() const {
    return (GetAccessFlags() & kAccVolatile) != 0;
  }

 private:
  // Field order required by test "ValidateFieldOrderOfJavaCppUnionClasses".
  // The class we are a part of
  Class* declaring_class_;

  uint32_t access_flags_;

  // Dex cache index of field id
  uint32_t field_dex_idx_;

  // Offset of field within an instance or in the Class' static fields
  uint32_t offset_;

  static Class* java_lang_reflect_Field_;

  friend struct FieldOffsets;  // for verifying offset information
  DISALLOW_IMPLICIT_CONSTRUCTORS(Field);
};

// C++ mirror of java.lang.reflect.Method and java.lang.reflect.Constructor
class MANAGED AbstractMethod : public Object {
 public:
  // A function that invokes a method with an array of its arguments.
  typedef void InvokeStub(const AbstractMethod* method,
                          Object* obj,
                          Thread* thread,
                          JValue* args,
                          JValue* result);

  Class* GetDeclaringClass() const;

  void SetDeclaringClass(Class *new_declaring_class) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static MemberOffset DeclaringClassOffset() {
    return MemberOffset(OFFSETOF_MEMBER(AbstractMethod, declaring_class_));
  }

  uint32_t GetAccessFlags() const;

  void SetAccessFlags(uint32_t new_access_flags) {
    SetField32(OFFSET_OF_OBJECT_MEMBER(AbstractMethod, access_flags_), new_access_flags, false);
  }

  // Approximate what kind of method call would be used for this method.
  InvokeType GetInvokeType() const;

  // Returns true if the method is declared public.
  bool IsPublic() const {
    return (GetAccessFlags() & kAccPublic) != 0;
  }

  // Returns true if the method is declared private.
  bool IsPrivate() const {
    return (GetAccessFlags() & kAccPrivate) != 0;
  }

  // Returns true if the method is declared static.
  bool IsStatic() const {
    return (GetAccessFlags() & kAccStatic) != 0;
  }

  // Returns true if the method is a constructor.
  bool IsConstructor() const {
    return (GetAccessFlags() & kAccConstructor) != 0;
  }

  // Returns true if the method is static, private, or a constructor.
  bool IsDirect() const {
    return IsDirect(GetAccessFlags());
  }

  static bool IsDirect(uint32_t access_flags) {
    return (access_flags & (kAccStatic | kAccPrivate | kAccConstructor)) != 0;
  }

  // Returns true if the method is declared synchronized.
  bool IsSynchronized() const {
    uint32_t synchonized = kAccSynchronized | kAccDeclaredSynchronized;
    return (GetAccessFlags() & synchonized) != 0;
  }

  bool IsFinal() const {
    return (GetAccessFlags() & kAccFinal) != 0;
  }

  bool IsMiranda() const {
    return (GetAccessFlags() & kAccMiranda) != 0;
  }

  bool IsNative() const {
    return (GetAccessFlags() & kAccNative) != 0;
  }

  bool IsAbstract() const {
    return (GetAccessFlags() & kAccAbstract) != 0;
  }

  bool IsSynthetic() const {
    return (GetAccessFlags() & kAccSynthetic) != 0;
  }

  bool IsProxyMethod() const;

  bool CheckIncompatibleClassChange(InvokeType type);

  uint16_t GetMethodIndex() const;

  size_t GetVtableIndex() const {
    return GetMethodIndex();
  }

  void SetMethodIndex(uint16_t new_method_index) {
    SetField32(OFFSET_OF_OBJECT_MEMBER(AbstractMethod, method_index_), new_method_index, false);
  }

  static MemberOffset MethodIndexOffset() {
    return OFFSET_OF_OBJECT_MEMBER(AbstractMethod, method_index_);
  }

  uint32_t GetCodeItemOffset() const {
    return GetField32(OFFSET_OF_OBJECT_MEMBER(AbstractMethod, code_item_offset_), false);
  }

  void SetCodeItemOffset(uint32_t new_code_off) {
    SetField32(OFFSET_OF_OBJECT_MEMBER(AbstractMethod, code_item_offset_), new_code_off, false);
  }

  // Number of 32bit registers that would be required to hold all the arguments
  static size_t NumArgRegisters(const StringPiece& shorty);

  uint32_t GetDexMethodIndex() const;

  void SetDexMethodIndex(uint32_t new_idx) {
    SetField32(OFFSET_OF_OBJECT_MEMBER(AbstractMethod, method_dex_index_), new_idx, false);
  }

  ObjectArray<String>* GetDexCacheStrings() const;
  void SetDexCacheStrings(ObjectArray<String>* new_dex_cache_strings)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static MemberOffset DexCacheStringsOffset() {
    return OFFSET_OF_OBJECT_MEMBER(AbstractMethod, dex_cache_strings_);
  }

  static MemberOffset DexCacheResolvedMethodsOffset() {
    return OFFSET_OF_OBJECT_MEMBER(AbstractMethod, dex_cache_resolved_methods_);
  }

  static MemberOffset DexCacheResolvedTypesOffset() {
    return OFFSET_OF_OBJECT_MEMBER(AbstractMethod, dex_cache_resolved_types_);
  }

  static MemberOffset DexCacheInitializedStaticStorageOffset() {
    return OFFSET_OF_OBJECT_MEMBER(AbstractMethod,
        dex_cache_initialized_static_storage_);
  }

  ObjectArray<AbstractMethod>* GetDexCacheResolvedMethods() const;
  void SetDexCacheResolvedMethods(ObjectArray<AbstractMethod>* new_dex_cache_methods)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  ObjectArray<Class>* GetDexCacheResolvedTypes() const;
  void SetDexCacheResolvedTypes(ObjectArray<Class>* new_dex_cache_types)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  ObjectArray<StaticStorageBase>* GetDexCacheInitializedStaticStorage() const;
  void SetDexCacheInitializedStaticStorage(ObjectArray<StaticStorageBase>* new_value)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Find the method that this method overrides
  AbstractMethod* FindOverriddenMethod() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void Invoke(Thread* self, Object* receiver, JValue* args, JValue* result)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  const void* GetCode() const {
    return GetFieldPtr<const void*>(OFFSET_OF_OBJECT_MEMBER(AbstractMethod, code_), false);
  }

  void SetCode(const void* code) {
    SetFieldPtr<const void*>(OFFSET_OF_OBJECT_MEMBER(AbstractMethod, code_), code, false);
  }

  uint32_t GetCodeSize() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(!IsRuntimeMethod() && !IsProxyMethod()) << PrettyMethod(this);
    uintptr_t code = reinterpret_cast<uintptr_t>(GetCode());
    if (code == 0) {
      return 0;
    }
    // TODO: make this Thumb2 specific
    code &= ~0x1;
    return reinterpret_cast<uint32_t*>(code)[-1];
  }

  bool IsWithinCode(uintptr_t pc) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    uintptr_t code = reinterpret_cast<uintptr_t>(GetCode());
    if (code == 0) {
      return pc == 0;
    }
    /*
     * During a stack walk, a return PC may point to the end of the code + 1
     * (in the case that the last instruction is a call that isn't expected to
     * return.  Thus, we check <= code + GetCodeSize().
     */
    return (code <= pc && pc <= code + GetCodeSize());
  }

  void AssertPcIsWithinCode(uintptr_t pc) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  uint32_t GetOatCodeOffset() const {
    DCHECK(!Runtime::Current()->IsStarted());
    return reinterpret_cast<uint32_t>(GetCode());
  }

  void SetOatCodeOffset(uint32_t code_offset) {
    DCHECK(!Runtime::Current()->IsStarted());
    SetCode(reinterpret_cast<void*>(code_offset));
  }

  static MemberOffset GetCodeOffset() {
    return OFFSET_OF_OBJECT_MEMBER(AbstractMethod, code_);
  }

  const uint32_t* GetMappingTable() const {
    const uint32_t* map = GetMappingTableRaw();
    if (map == NULL) {
      return map;
    }
    return map + 1;
  }

  uint32_t GetPcToDexMappingTableLength() const {
    const uint32_t* map = GetMappingTableRaw();
    if (map == NULL) {
      return 0;
    }
    return map[2];
  }

  const uint32_t* GetPcToDexMappingTable() const {
    const uint32_t* map = GetMappingTableRaw();
    if (map == NULL) {
      return map;
    }
    return map + 3;
  }


  uint32_t GetDexToPcMappingTableLength() const {
    const uint32_t* map = GetMappingTableRaw();
    if (map == NULL) {
      return 0;
    }
    return map[1] - map[2];
  }

  const uint32_t* GetDexToPcMappingTable() const {
    const uint32_t* map = GetMappingTableRaw();
    if (map == NULL) {
      return map;
    }
    return map + 3 + map[2];
  }


  const uint32_t* GetMappingTableRaw() const {
    return GetFieldPtr<const uint32_t*>(OFFSET_OF_OBJECT_MEMBER(AbstractMethod, mapping_table_), false);
  }

  void SetMappingTable(const uint32_t* mapping_table) {
    SetFieldPtr<const uint32_t*>(OFFSET_OF_OBJECT_MEMBER(AbstractMethod, mapping_table_),
                                 mapping_table, false);
  }

  uint32_t GetOatMappingTableOffset() const {
    DCHECK(!Runtime::Current()->IsStarted());
    return reinterpret_cast<uint32_t>(GetMappingTableRaw());
  }

  void SetOatMappingTableOffset(uint32_t mapping_table_offset) {
    DCHECK(!Runtime::Current()->IsStarted());
    SetMappingTable(reinterpret_cast<const uint32_t*>(mapping_table_offset));
  }

  // Callers should wrap the uint16_t* in a VmapTable instance for convenient access.
  const uint16_t* GetVmapTableRaw() const {
    return GetFieldPtr<const uint16_t*>(OFFSET_OF_OBJECT_MEMBER(AbstractMethod, vmap_table_), false);
  }

  void SetVmapTable(const uint16_t* vmap_table) {
    SetFieldPtr<const uint16_t*>(OFFSET_OF_OBJECT_MEMBER(AbstractMethod, vmap_table_), vmap_table, false);
  }

  uint32_t GetOatVmapTableOffset() const {
    DCHECK(!Runtime::Current()->IsStarted());
    return reinterpret_cast<uint32_t>(GetVmapTableRaw());
  }

  void SetOatVmapTableOffset(uint32_t vmap_table_offset) {
    DCHECK(!Runtime::Current()->IsStarted());
    SetVmapTable(reinterpret_cast<uint16_t*>(vmap_table_offset));
  }

  const uint8_t* GetNativeGcMap() const {
    return GetFieldPtr<uint8_t*>(OFFSET_OF_OBJECT_MEMBER(AbstractMethod, native_gc_map_), false);
  }
  void SetNativeGcMap(const uint8_t* data) {
    SetFieldPtr<const uint8_t*>(OFFSET_OF_OBJECT_MEMBER(AbstractMethod, native_gc_map_), data,
        false);
  }

  // When building the oat need a convenient place to stuff the offset of the native GC map.
  void SetOatNativeGcMapOffset(uint32_t gc_map_offset) {
    DCHECK(!Runtime::Current()->IsStarted());
    SetNativeGcMap(reinterpret_cast<uint8_t*>(gc_map_offset));
  }

  uint32_t GetOatNativeGcMapOffset() const {
    DCHECK(!Runtime::Current()->IsStarted());
    return reinterpret_cast<uint32_t>(GetNativeGcMap());
  }

  size_t GetFrameSizeInBytes() const {
    DCHECK_EQ(sizeof(size_t), sizeof(uint32_t));
    size_t result = GetField32(OFFSET_OF_OBJECT_MEMBER(AbstractMethod, frame_size_in_bytes_), false);
    DCHECK_LE(static_cast<size_t>(kStackAlignment), result);
    return result;
  }

  void SetFrameSizeInBytes(size_t new_frame_size_in_bytes) {
    DCHECK_EQ(sizeof(size_t), sizeof(uint32_t));
    SetField32(OFFSET_OF_OBJECT_MEMBER(AbstractMethod, frame_size_in_bytes_),
               new_frame_size_in_bytes, false);
  }

  size_t GetReturnPcOffsetInBytes() const {
    return GetFrameSizeInBytes() - kPointerSize;
  }

  bool IsRegistered() const;

  void RegisterNative(Thread* self, const void* native_method)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void UnregisterNative(Thread* self) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static MemberOffset NativeMethodOffset() {
    return OFFSET_OF_OBJECT_MEMBER(AbstractMethod, native_method_);
  }

  const void* GetNativeMethod() const {
    return reinterpret_cast<const void*>(GetField32(NativeMethodOffset(), false));
  }

  // Native to managed invocation stub entry point
  InvokeStub* GetInvokeStub() const {
    InvokeStub* result = GetFieldPtr<InvokeStub*>(
        OFFSET_OF_OBJECT_MEMBER(AbstractMethod, invoke_stub_), false);
    // TODO: DCHECK(result != NULL);  should be ahead of time compiled
    return result;
  }

  void SetInvokeStub(InvokeStub* invoke_stub) {
    SetFieldPtr<InvokeStub*>(OFFSET_OF_OBJECT_MEMBER(AbstractMethod, invoke_stub_),
                             invoke_stub, false);
  }

  uint32_t GetInvokeStubSize() const {
    uintptr_t invoke_stub = reinterpret_cast<uintptr_t>(GetInvokeStub());
    if (invoke_stub == 0) {
      return 0;
    }
    // TODO: make this Thumb2 specific
    invoke_stub &= ~0x1;
    return reinterpret_cast<const uint32_t*>(invoke_stub)[-1];
  }

  uint32_t GetOatInvokeStubOffset() const {
    DCHECK(!Runtime::Current()->IsStarted());
    return reinterpret_cast<uint32_t>(GetInvokeStub());
  }

  void SetOatInvokeStubOffset(uint32_t invoke_stub_offset) {
    DCHECK(!Runtime::Current()->IsStarted());
    SetInvokeStub(reinterpret_cast<InvokeStub*>(invoke_stub_offset));
  }

  static MemberOffset GetInvokeStubOffset() {
    return OFFSET_OF_OBJECT_MEMBER(AbstractMethod, invoke_stub_);
  }

  static MemberOffset GetMethodIndexOffset() {
    return OFFSET_OF_OBJECT_MEMBER(AbstractMethod, method_index_);
  }

  uint32_t GetCoreSpillMask() const {
    return GetField32(OFFSET_OF_OBJECT_MEMBER(AbstractMethod, core_spill_mask_), false);
  }

  void SetCoreSpillMask(uint32_t core_spill_mask) {
    // Computed during compilation
    SetField32(OFFSET_OF_OBJECT_MEMBER(AbstractMethod, core_spill_mask_), core_spill_mask, false);
  }

  uint32_t GetFpSpillMask() const {
    return GetField32(OFFSET_OF_OBJECT_MEMBER(AbstractMethod, fp_spill_mask_), false);
  }

  void SetFpSpillMask(uint32_t fp_spill_mask) {
    // Computed during compilation
    SetField32(OFFSET_OF_OBJECT_MEMBER(AbstractMethod, fp_spill_mask_), fp_spill_mask, false);
  }

  // Is this a CalleSaveMethod or ResolutionMethod and therefore doesn't adhere to normal
  // conventions for a method of managed code. Returns false for Proxy methods.
  bool IsRuntimeMethod() const {
    return GetDexMethodIndex() == DexFile::kDexNoIndex16;
  }

  // Is this a hand crafted method used for something like describing callee saves?
  bool IsCalleeSaveMethod() const {
    if (!IsRuntimeMethod()) {
      return false;
    }
    Runtime* runtime = Runtime::Current();
    bool result = false;
    for (int i = 0; i < Runtime::kLastCalleeSaveType; i++) {
      if (this == runtime->GetCalleeSaveMethod(Runtime::CalleeSaveType(i))) {
        result = true;
        break;
      }
    }
    return result;
  }

  bool IsResolutionMethod() const {
    bool result = this == Runtime::Current()->GetResolutionMethod();
    // Check that if we do think it is phony it looks like the resolution method
    DCHECK(!result || GetDexMethodIndex() == DexFile::kDexNoIndex16);
    return result;
  }

  uintptr_t NativePcOffset(const uintptr_t pc) const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Converts a native PC to a dex PC.
  uint32_t ToDexPc(const uintptr_t pc) const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Converts a dex PC to a native PC.
  uintptr_t ToNativePc(const uint32_t dex_pc) const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Converts a dex PC to the first corresponding safepoint PC.
  uintptr_t ToFirstNativeSafepointPc(const uint32_t dex_pc)
      const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Find the catch block for the given exception type and dex_pc
  uint32_t FindCatchBlock(Class* exception_type, uint32_t dex_pc) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static void SetClasses(Class* java_lang_reflect_Constructor, Class* java_lang_reflect_Method);

  static Class* GetConstructorClass() {
    return java_lang_reflect_Constructor_;
  }

  static Class* GetMethodClass() {
    return java_lang_reflect_Method_;
  }

  static void ResetClasses();

 protected:
  // Field order required by test "ValidateFieldOrderOfJavaCppUnionClasses".
  // The class we are a part of
  Class* declaring_class_;

  // short cuts to declaring_class_->dex_cache_ member for fast compiled code access
  ObjectArray<StaticStorageBase>* dex_cache_initialized_static_storage_;

  // short cuts to declaring_class_->dex_cache_ member for fast compiled code access
  ObjectArray<Class>* dex_cache_resolved_methods_;

  // short cuts to declaring_class_->dex_cache_ member for fast compiled code access
  ObjectArray<Class>* dex_cache_resolved_types_;

  // short cuts to declaring_class_->dex_cache_ member for fast compiled code access
  ObjectArray<String>* dex_cache_strings_;

  // Access flags; low 16 bits are defined by spec.
  uint32_t access_flags_;

  // Compiled code associated with this method for callers from managed code.
  // May be compiled managed code or a bridge for invoking a native method.
  const void* code_;

  // Offset to the CodeItem.
  uint32_t code_item_offset_;

  // Architecture-dependent register spill mask
  uint32_t core_spill_mask_;

  // Architecture-dependent register spill mask
  uint32_t fp_spill_mask_;

  // Total size in bytes of the frame
  size_t frame_size_in_bytes_;

  // Garbage collection map of native PC offsets to reference bitmaps.
  const uint8_t* native_gc_map_;

  // Native invocation stub entry point for calling from native to managed code.
  InvokeStub* invoke_stub_;

  // Mapping from native pc to dex pc
  const uint32_t* mapping_table_;

  // Index into method_ids of the dex file associated with this method
  uint32_t method_dex_index_;

  // For concrete virtual methods, this is the offset of the method in Class::vtable_.
  //
  // For abstract methods in an interface class, this is the offset of the method in
  // "iftable_->Get(n)->GetMethodArray()".
  //
  // For static and direct methods this is the index in the direct methods table.
  uint32_t method_index_;

  // The target native method registered with this method
  const void* native_method_;

  // When a register is promoted into a register, the spill mask holds which registers hold dex
  // registers. The first promoted register's corresponding dex register is vmap_table_[1], the Nth
  // is vmap_table_[N]. vmap_table_[0] holds the length of the table.
  const uint16_t* vmap_table_;

  static Class* java_lang_reflect_Constructor_;
  static Class* java_lang_reflect_Method_;

  friend class ImageWriter;  // for relocating code_ and invoke_stub_
  friend struct AbstractMethodOffsets;  // for verifying offset information
  friend struct ConstructorMethodOffsets;  // for verifying offset information
  friend struct MethodOffsets;  // for verifying offset information
  DISALLOW_IMPLICIT_CONSTRUCTORS(AbstractMethod);
};

class MANAGED Method : public AbstractMethod {

};

class MANAGED Constructor : public AbstractMethod {

};

class MANAGED Array : public Object {
 public:
  // A convenience for code that doesn't know the component size,
  // and doesn't want to have to work it out itself.
  static Array* Alloc(Thread* self, Class* array_class, int32_t component_count)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static Array* Alloc(Thread* self, Class* array_class, int32_t component_count,
                      size_t component_size)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static Array* CreateMultiArray(Thread* self, Class* element_class, IntArray* dimensions)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  size_t SizeOf() const;

  int32_t GetLength() const {
    return GetField32(OFFSET_OF_OBJECT_MEMBER(Array, length_), false);
  }

  void SetLength(int32_t length) {
    CHECK_GE(length, 0);
    SetField32(OFFSET_OF_OBJECT_MEMBER(Array, length_), length, false);
  }

  static MemberOffset LengthOffset() {
    return OFFSET_OF_OBJECT_MEMBER(Array, length_);
  }

  static MemberOffset DataOffset(size_t component_size) {
    if (component_size != sizeof(int64_t)) {
      return OFFSET_OF_OBJECT_MEMBER(Array, first_element_);
    } else {
      // Align longs and doubles.
      return MemberOffset(OFFSETOF_MEMBER(Array, first_element_) + 4);
    }
  }

  void* GetRawData(size_t component_size) {
    intptr_t data = reinterpret_cast<intptr_t>(this) + DataOffset(component_size).Int32Value();
    return reinterpret_cast<void*>(data);
  }

  const void* GetRawData(size_t component_size) const {
    intptr_t data = reinterpret_cast<intptr_t>(this) + DataOffset(component_size).Int32Value();
    return reinterpret_cast<const void*>(data);
  }

 protected:
  bool IsValidIndex(int32_t index) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    if (UNLIKELY(index < 0 || index >= GetLength())) {
      return ThrowArrayIndexOutOfBoundsException(index);
    }
    return true;
  }

 protected:
  bool ThrowArrayIndexOutOfBoundsException(int32_t index) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  bool ThrowArrayStoreException(Object* object) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

 private:
  // The number of array elements.
  int32_t length_;
  // Marker for the data (used by generated code)
  uint32_t first_element_[0];

  DISALLOW_IMPLICIT_CONSTRUCTORS(Array);
};

template<class T>
class MANAGED ObjectArray : public Array {
 public:
  static ObjectArray<T>* Alloc(Thread* self, Class* object_array_class, int32_t length)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  T* Get(int32_t i) const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void Set(int32_t i, T* object) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Set element without bound and element type checks, to be used in limited
  // circumstances, such as during boot image writing
  void SetWithoutChecks(int32_t i, T* object) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Set element without bound and element type checks, to be used in limited circumstances, such
  // as during boot image writing. Does not do write barrier.
  void SetPtrWithoutChecks(int32_t i, T* object) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  T* GetWithoutChecks(int32_t i) const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static void Copy(const ObjectArray<T>* src, int src_pos,
                   ObjectArray<T>* dst, int dst_pos,
                   size_t length)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  ObjectArray<T>* CopyOf(Thread* self, int32_t new_length)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(ObjectArray);
};

template<class T>
ObjectArray<T>* ObjectArray<T>::Alloc(Thread* self, Class* object_array_class, int32_t length) {
  Array* array = Array::Alloc(self, object_array_class, length, sizeof(Object*));
  if (UNLIKELY(array == NULL)) {
    return NULL;
  } else {
    return array->AsObjectArray<T>();
  }
}

template<class T>
inline T* ObjectArray<T>::Get(int32_t i) const {
  if (UNLIKELY(!IsValidIndex(i))) {
    return NULL;
  }
  MemberOffset data_offset(DataOffset(sizeof(Object*)).Int32Value() + i * sizeof(Object*));
  return GetFieldObject<T*>(data_offset, false);
}

template<class T>
ObjectArray<T>* ObjectArray<T>::CopyOf(Thread* self, int32_t new_length) {
  ObjectArray<T>* new_array = Alloc(self, GetClass(), new_length);
  Copy(this, 0, new_array, 0, std::min(GetLength(), new_length));
  return new_array;
}

class MANAGED IfTable : public ObjectArray<Object> {
 public:
  Class* GetInterface(int32_t i) const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    Class* interface = Get((i * kMax) + kInterface)->AsClass();
    DCHECK(interface != NULL);
    return interface;
  }

  void SetInterface(int32_t i, Class* interface) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  ObjectArray<AbstractMethod>* GetMethodArray(int32_t i) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    ObjectArray<AbstractMethod>* method_array =
        down_cast<ObjectArray<AbstractMethod>*>(Get((i * kMax) + kMethodArray));
    DCHECK(method_array != NULL);
    return method_array;
  }

  size_t GetMethodArrayCount(int32_t i) const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    ObjectArray<AbstractMethod>* method_array =
        down_cast<ObjectArray<AbstractMethod>*>(Get((i * kMax) + kMethodArray));
    if (method_array == NULL) {
      return 0;
    }
    return method_array->GetLength();
  }

  void SetMethodArray(int32_t i, ObjectArray<AbstractMethod>* new_ma)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(new_ma != NULL);
    DCHECK(Get((i * kMax) + kMethodArray) == NULL);
    Set((i * kMax) + kMethodArray, new_ma);
  }

  size_t Count() const {
    return GetLength() / kMax;
  }

  enum {
    // Points to the interface class.
    kInterface   = 0,
    // Method pointers into the vtable, allow fast map from interface method index to concrete
    // instance method.
    kMethodArray = 1,
    kMax         = 2,
  };

 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(IfTable);
};

// Type for the InitializedStaticStorage table. Currently the Class
// provides the static storage. However, this might change to an Array
// to improve image sharing, so we use this type to avoid assumptions
// on the current storage.
class MANAGED StaticStorageBase : public Object {
};

// C++ mirror of java.lang.Class
class MANAGED Class : public StaticStorageBase {
 public:
  // Class Status
  //
  // kStatusNotReady: If a Class cannot be found in the class table by
  // FindClass, it allocates an new one with AllocClass in the
  // kStatusNotReady and calls LoadClass. Note if it does find a
  // class, it may not be kStatusResolved and it will try to push it
  // forward toward kStatusResolved.
  //
  // kStatusIdx: LoadClass populates with Class with information from
  // the DexFile, moving the status to kStatusIdx, indicating that the
  // Class value in super_class_ has not been populated. The new Class
  // can then be inserted into the classes table.
  //
  // kStatusLoaded: After taking a lock on Class, the ClassLinker will
  // attempt to move a kStatusIdx class forward to kStatusLoaded by
  // using ResolveClass to initialize the super_class_ and ensuring the
  // interfaces are resolved.
  //
  // kStatusResolved: Still holding the lock on Class, the ClassLinker
  // shows linking is complete and fields of the Class populated by making
  // it kStatusResolved. Java allows circularities of the form where a super
  // class has a field that is of the type of the sub class. We need to be able
  // to fully resolve super classes while resolving types for fields.
  //
  // kStatusRetryVerificationAtRuntime: The verifier sets a class to
  // this state if it encounters a soft failure at compile time. This
  // often happens when there are unresolved classes in other dex
  // files, and this status marks a class as needing to be verified
  // again at runtime.
  //
  // TODO: Explain the other states
  enum Status {
    kStatusError = -1,
    kStatusNotReady = 0,
    kStatusIdx = 1,  // Loaded, DEX idx in super_class_type_idx_ and interfaces_type_idx_.
    kStatusLoaded = 2,  // DEX idx values resolved.
    kStatusResolved = 3,  // Part of linking.
    kStatusVerifying = 4,  // In the process of being verified.
    kStatusRetryVerificationAtRuntime = 5,  // Compile time verification failed, retry at runtime.
    kStatusVerifyingAtRuntime = 6,  // Retrying verification at runtime.
    kStatusVerified = 7,  // Logically part of linking; done pre-init.
    kStatusInitializing = 8,  // Class init in progress.
    kStatusInitialized = 9,  // Ready to go.
  };

  Status GetStatus() const {
    DCHECK_EQ(sizeof(Status), sizeof(uint32_t));
    return static_cast<Status>(GetField32(OFFSET_OF_OBJECT_MEMBER(Class, status_), false));
  }

  void SetStatus(Status new_status) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Returns true if the class has failed to link.
  bool IsErroneous() const {
    return GetStatus() == kStatusError;
  }

  // Returns true if the class has been loaded.
  bool IsIdxLoaded() const {
    return GetStatus() >= kStatusIdx;
  }

  // Returns true if the class has been loaded.
  bool IsLoaded() const {
    return GetStatus() >= kStatusLoaded;
  }

  // Returns true if the class has been linked.
  bool IsResolved() const {
    return GetStatus() >= kStatusResolved;
  }

  // Returns true if the class was compile-time verified.
  bool IsCompileTimeVerified() const {
    return GetStatus() >= kStatusRetryVerificationAtRuntime;
  }

  // Returns true if the class has been verified.
  bool IsVerified() const {
    return GetStatus() >= kStatusVerified;
  }

  // Returns true if the class is initializing.
  bool IsInitializing() const {
    return GetStatus() >= kStatusInitializing;
  }

  // Returns true if the class is initialized.
  bool IsInitialized() const {
    return GetStatus() == kStatusInitialized;
  }

  uint32_t GetAccessFlags() const;

  void SetAccessFlags(uint32_t new_access_flags) {
    SetField32(OFFSET_OF_OBJECT_MEMBER(Class, access_flags_), new_access_flags, false);
  }

  // Returns true if the class is an interface.
  bool IsInterface() const {
    return (GetAccessFlags() & kAccInterface) != 0;
  }

  // Returns true if the class is declared public.
  bool IsPublic() const {
    return (GetAccessFlags() & kAccPublic) != 0;
  }

  // Returns true if the class is declared final.
  bool IsFinal() const {
    return (GetAccessFlags() & kAccFinal) != 0;
  }

  bool IsFinalizable() const {
    return (GetAccessFlags() & kAccClassIsFinalizable) != 0;
  }

  void SetFinalizable() {
    uint32_t flags = GetField32(OFFSET_OF_OBJECT_MEMBER(Class, access_flags_), false);
    SetAccessFlags(flags | kAccClassIsFinalizable);
  }

  // Returns true if the class is abstract.
  bool IsAbstract() const {
    return (GetAccessFlags() & kAccAbstract) != 0;
  }

  // Returns true if the class is an annotation.
  bool IsAnnotation() const {
    return (GetAccessFlags() & kAccAnnotation) != 0;
  }

  // Returns true if the class is synthetic.
  bool IsSynthetic() const {
    return (GetAccessFlags() & kAccSynthetic) != 0;
  }

  bool IsReferenceClass() const {
    return (GetAccessFlags() & kAccClassIsReference) != 0;
  }

  bool IsWeakReferenceClass() const {
    return (GetAccessFlags() & kAccClassIsWeakReference) != 0;
  }

  bool IsSoftReferenceClass() const {
    return (GetAccessFlags() & kAccReferenceFlagsMask) == kAccClassIsReference;
  }

  bool IsFinalizerReferenceClass() const {
    return (GetAccessFlags() & kAccClassIsFinalizerReference) != 0;
  }

  bool IsPhantomReferenceClass() const {
    return (GetAccessFlags() & kAccClassIsPhantomReference) != 0;
  }


  String* GetName() const;  // Returns the cached name.
  void SetName(String* name) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);  // Sets the cached name.
  // Computes the name, then sets the cached value.
  String* ComputeName() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  bool IsProxyClass() const {
    // Read access flags without using getter as whether something is a proxy can be check in
    // any loaded state
    // TODO: switch to a check if the super class is java.lang.reflect.Proxy?
    uint32_t access_flags = GetField32(OFFSET_OF_OBJECT_MEMBER(Class, access_flags_), false);
    return (access_flags & kAccClassIsProxy) != 0;
  }

  Primitive::Type GetPrimitiveType() const {
    DCHECK_EQ(sizeof(Primitive::Type), sizeof(int32_t));
    return static_cast<Primitive::Type>(
        GetField32(OFFSET_OF_OBJECT_MEMBER(Class, primitive_type_), false));
  }

  void SetPrimitiveType(Primitive::Type new_type) {
    DCHECK_EQ(sizeof(Primitive::Type), sizeof(int32_t));
    SetField32(OFFSET_OF_OBJECT_MEMBER(Class, primitive_type_), new_type, false);
  }

  // Returns true if the class is a primitive type.
  bool IsPrimitive() const {
    return GetPrimitiveType() != Primitive::kPrimNot;
  }

  bool IsPrimitiveBoolean() const {
    return GetPrimitiveType() == Primitive::kPrimBoolean;
  }

  bool IsPrimitiveByte() const {
    return GetPrimitiveType() == Primitive::kPrimByte;
  }

  bool IsPrimitiveChar() const {
    return GetPrimitiveType() == Primitive::kPrimChar;
  }

  bool IsPrimitiveShort() const {
    return GetPrimitiveType() == Primitive::kPrimShort;
  }

  bool IsPrimitiveInt() const {
    return GetPrimitiveType() == Primitive::kPrimInt;
  }

  bool IsPrimitiveLong() const {
    return GetPrimitiveType() == Primitive::kPrimLong;
  }

  bool IsPrimitiveFloat() const {
    return GetPrimitiveType() == Primitive::kPrimFloat;
  }

  bool IsPrimitiveDouble() const {
    return GetPrimitiveType() == Primitive::kPrimDouble;
  }

  bool IsPrimitiveVoid() const {
    return GetPrimitiveType() == Primitive::kPrimVoid;
  }

  bool IsPrimitiveArray() const {
    return IsArrayClass() && GetComponentType()->IsPrimitive();
  }

  // Depth of class from java.lang.Object
  size_t Depth() {
    size_t depth = 0;
    for (Class* klass = this; klass->GetSuperClass() != NULL; klass = klass->GetSuperClass()) {
      depth++;
    }
    return depth;
  }

  bool IsArrayClass() const {
    return GetComponentType() != NULL;
  }

  bool IsClassClass() const;

  bool IsStringClass() const;

  bool IsThrowableClass() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  bool IsFieldClass() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  bool IsMethodClass() const;

  Class* GetComponentType() const {
    return GetFieldObject<Class*>(OFFSET_OF_OBJECT_MEMBER(Class, component_type_), false);
  }

  void SetComponentType(Class* new_component_type) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(GetComponentType() == NULL);
    DCHECK(new_component_type != NULL);
    SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Class, component_type_), new_component_type, false);
  }

  size_t GetComponentSize() const {
    return Primitive::ComponentSize(GetComponentType()->GetPrimitiveType());
  }

  bool IsObjectClass() const {
    return !IsPrimitive() && GetSuperClass() == NULL;
  }
  bool IsInstantiable() const {
    return !IsPrimitive() && !IsInterface() && !IsAbstract();
  }

  bool IsObjectArrayClass() const {
    return GetComponentType() != NULL && !GetComponentType()->IsPrimitive();
  }

  // Creates a raw object instance but does not invoke the default constructor.
  Object* AllocObject(Thread* self) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  bool IsVariableSize() const {
    // Classes and arrays vary in size, and so the object_size_ field cannot
    // be used to get their instance size
    return IsClassClass() || IsArrayClass();
  }

  size_t SizeOf() const {
    DCHECK_EQ(sizeof(size_t), sizeof(int32_t));
    return GetField32(OFFSET_OF_OBJECT_MEMBER(Class, class_size_), false);
  }

  size_t GetClassSize() const {
    DCHECK_EQ(sizeof(size_t), sizeof(uint32_t));
    return GetField32(OFFSET_OF_OBJECT_MEMBER(Class, class_size_), false);
  }

  void SetClassSize(size_t new_class_size)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  size_t GetObjectSize() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    CHECK(!IsVariableSize()) << " class=" << PrettyTypeOf(this);
    DCHECK_EQ(sizeof(size_t), sizeof(int32_t));
    size_t result = GetField32(OFFSET_OF_OBJECT_MEMBER(Class, object_size_), false);
    CHECK_GE(result, sizeof(Object)) << " class=" << PrettyTypeOf(this);
    return result;
  }

  void SetObjectSize(size_t new_object_size) {
    DCHECK(!IsVariableSize());
    DCHECK_EQ(sizeof(size_t), sizeof(int32_t));
    return SetField32(OFFSET_OF_OBJECT_MEMBER(Class, object_size_), new_object_size, false);
  }

  // Returns true if this class is in the same packages as that class.
  bool IsInSamePackage(const Class* that) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static bool IsInSamePackage(const StringPiece& descriptor1, const StringPiece& descriptor2);

  // Returns true if this class can access that class.
  bool CanAccess(Class* that) const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return that->IsPublic() || this->IsInSamePackage(that);
  }

  // Can this class access a member in the provided class with the provided member access flags?
  // Note that access to the class isn't checked in case the declaring class is protected and the
  // method has been exposed by a public sub-class
  bool CanAccessMember(Class* access_to, uint32_t member_flags) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    // Classes can access all of their own members
    if (this == access_to) {
      return true;
    }
    // Public members are trivially accessible
    if (member_flags & kAccPublic) {
      return true;
    }
    // Private members are trivially not accessible
    if (member_flags & kAccPrivate) {
      return false;
    }
    // Check for protected access from a sub-class, which may or may not be in the same package.
    if (member_flags & kAccProtected) {
      if (this->IsSubClass(access_to)) {
        return true;
      }
    }
    // Allow protected access from other classes in the same package.
    return this->IsInSamePackage(access_to);
  }

  bool IsSubClass(const Class* klass) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Can src be assigned to this class? For example, String can be assigned to Object (by an
  // upcast), however, an Object cannot be assigned to a String as a potentially exception throwing
  // downcast would be necessary. Similarly for interfaces, a class that implements (or an interface
  // that extends) another can be assigned to its parent, but not vice-versa. All Classes may assign
  // to themselves. Classes for primitive types may not assign to each other.
  bool IsAssignableFrom(const Class* src) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(src != NULL);
    if (this == src) {
      // Can always assign to things of the same type.
      return true;
    } else if (IsObjectClass()) {
      // Can assign any reference to java.lang.Object.
      return !src->IsPrimitive();
    } else if (IsInterface()) {
      return src->Implements(this);
    } else if (src->IsArrayClass()) {
      return IsAssignableFromArray(src);
    } else {
      return !src->IsInterface() && src->IsSubClass(this);
    }
  }

  Class* GetSuperClass() const {
    // Can only get super class for loaded classes (hack for when runtime is
    // initializing)
    DCHECK(IsLoaded() || !Runtime::Current()->IsStarted()) << IsLoaded();
    return GetFieldObject<Class*>(OFFSET_OF_OBJECT_MEMBER(Class, super_class_), false);
  }

  void SetSuperClass(Class *new_super_class) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    // super class is assigned once, except during class linker initialization
    Class* old_super_class = GetFieldObject<Class*>(
        OFFSET_OF_OBJECT_MEMBER(Class, super_class_), false);
    DCHECK(old_super_class == NULL || old_super_class == new_super_class);
    DCHECK(new_super_class != NULL);
    SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Class, super_class_), new_super_class, false);
  }

  bool HasSuperClass() const {
    return GetSuperClass() != NULL;
  }

  static MemberOffset SuperClassOffset() {
    return MemberOffset(OFFSETOF_MEMBER(Class, super_class_));
  }

  ClassLoader* GetClassLoader() const;

  void SetClassLoader(ClassLoader* new_cl) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static MemberOffset DexCacheOffset() {
    return MemberOffset(OFFSETOF_MEMBER(Class, dex_cache_));
  }

  enum {
    kDumpClassFullDetail = 1,
    kDumpClassClassLoader = (1 << 1),
    kDumpClassInitialized = (1 << 2),
  };

  void DumpClass(std::ostream& os, int flags) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  DexCache* GetDexCache() const;

  void SetDexCache(DexCache* new_dex_cache) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  ObjectArray<AbstractMethod>* GetDirectMethods() const {
    DCHECK(IsLoaded() || IsErroneous());
    return GetFieldObject<ObjectArray<AbstractMethod>*>(
        OFFSET_OF_OBJECT_MEMBER(Class, direct_methods_), false);
  }

  void SetDirectMethods(ObjectArray<AbstractMethod>* new_direct_methods)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(NULL == GetFieldObject<ObjectArray<AbstractMethod>*>(
        OFFSET_OF_OBJECT_MEMBER(Class, direct_methods_), false));
    DCHECK_NE(0, new_direct_methods->GetLength());
    SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Class, direct_methods_),
                   new_direct_methods, false);
  }

  AbstractMethod* GetDirectMethod(int32_t i) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return GetDirectMethods()->Get(i);
  }

  void SetDirectMethod(uint32_t i, AbstractMethod* f)  // TODO: uint16_t
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_){
    ObjectArray<AbstractMethod>* direct_methods =
        GetFieldObject<ObjectArray<AbstractMethod>*>(
            OFFSET_OF_OBJECT_MEMBER(Class, direct_methods_), false);
    direct_methods->Set(i, f);
  }

  // Returns the number of static, private, and constructor methods.
  size_t NumDirectMethods() const {
    return (GetDirectMethods() != NULL) ? GetDirectMethods()->GetLength() : 0;
  }

  ObjectArray<AbstractMethod>* GetVirtualMethods() const {
    DCHECK(IsLoaded() || IsErroneous());
    return GetFieldObject<ObjectArray<AbstractMethod>*>(
        OFFSET_OF_OBJECT_MEMBER(Class, virtual_methods_), false);
  }

  void SetVirtualMethods(ObjectArray<AbstractMethod>* new_virtual_methods)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    // TODO: we reassign virtual methods to grow the table for miranda
    // methods.. they should really just be assigned once
    DCHECK_NE(0, new_virtual_methods->GetLength());
    SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Class, virtual_methods_),
                   new_virtual_methods, false);
  }

  // Returns the number of non-inherited virtual methods.
  size_t NumVirtualMethods() const {
    return (GetVirtualMethods() != NULL) ? GetVirtualMethods()->GetLength() : 0;
  }

  AbstractMethod* GetVirtualMethod(uint32_t i) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(IsResolved() || IsErroneous());
    return GetVirtualMethods()->Get(i);
  }

  AbstractMethod* GetVirtualMethodDuringLinking(uint32_t i) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(IsLoaded() || IsErroneous());
    return GetVirtualMethods()->Get(i);
  }

  void SetVirtualMethod(uint32_t i, AbstractMethod* f)  // TODO: uint16_t
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    ObjectArray<AbstractMethod>* virtual_methods =
        GetFieldObject<ObjectArray<AbstractMethod>*>(
            OFFSET_OF_OBJECT_MEMBER(Class, virtual_methods_), false);
    virtual_methods->Set(i, f);
  }

  ObjectArray<AbstractMethod>* GetVTable() const {
    DCHECK(IsResolved() || IsErroneous());
    return GetFieldObject<ObjectArray<AbstractMethod>*>(OFFSET_OF_OBJECT_MEMBER(Class, vtable_), false);
  }

  ObjectArray<AbstractMethod>* GetVTableDuringLinking() const {
    DCHECK(IsLoaded() || IsErroneous());
    return GetFieldObject<ObjectArray<AbstractMethod>*>(OFFSET_OF_OBJECT_MEMBER(Class, vtable_), false);
  }

  void SetVTable(ObjectArray<AbstractMethod>* new_vtable)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Class, vtable_), new_vtable, false);
  }

  static MemberOffset VTableOffset() {
    return OFFSET_OF_OBJECT_MEMBER(Class, vtable_);
  }

  // Given a method implemented by this class but potentially from a
  // super class, return the specific implementation
  // method for this class.
  AbstractMethod* FindVirtualMethodForVirtual(AbstractMethod* method)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(!method->GetDeclaringClass()->IsInterface());
    // The argument method may from a super class.
    // Use the index to a potentially overridden one for this instance's class.
    return GetVTable()->Get(method->GetMethodIndex());
  }

  // Given a method implemented by this class' super class, return the specific implementation
  // method for this class.
  AbstractMethod* FindVirtualMethodForSuper(AbstractMethod* method)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(!method->GetDeclaringClass()->IsInterface());
    return GetSuperClass()->GetVTable()->Get(method->GetMethodIndex());
  }

  // Given a method implemented by this class, but potentially from a
  // super class or interface, return the specific implementation
  // method for this class.
  AbstractMethod* FindVirtualMethodForInterface(AbstractMethod* method)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  AbstractMethod* FindInterfaceMethod(const StringPiece& name, const StringPiece& descriptor) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  AbstractMethod* FindInterfaceMethod(const DexCache* dex_cache, uint32_t dex_method_idx) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  AbstractMethod* FindVirtualMethodForVirtualOrInterface(AbstractMethod* method)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    if (method->IsDirect()) {
      return method;
    }
    if (method->GetDeclaringClass()->IsInterface()) {
      return FindVirtualMethodForInterface(method);
    }
    return FindVirtualMethodForVirtual(method);
  }

  AbstractMethod* FindDeclaredVirtualMethod(const StringPiece& name, const StringPiece& signature) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  AbstractMethod* FindDeclaredVirtualMethod(const DexCache* dex_cache, uint32_t dex_method_idx) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  AbstractMethod* FindVirtualMethod(const StringPiece& name, const StringPiece& descriptor) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  AbstractMethod* FindVirtualMethod(const DexCache* dex_cache, uint32_t dex_method_idx) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  AbstractMethod* FindDeclaredDirectMethod(const StringPiece& name, const StringPiece& signature) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  AbstractMethod* FindDeclaredDirectMethod(const DexCache* dex_cache, uint32_t dex_method_idx) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  AbstractMethod* FindDirectMethod(const StringPiece& name, const StringPiece& signature) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  AbstractMethod* FindDirectMethod(const DexCache* dex_cache, uint32_t dex_method_idx) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  int32_t GetIfTableCount() const {
    IfTable* iftable = GetIfTable();
    if (iftable == NULL) {
      return 0;
    }
    return iftable->Count();
  }

  IfTable* GetIfTable() const {
    return GetFieldObject<IfTable*>(OFFSET_OF_OBJECT_MEMBER(Class, iftable_), false);
  }

  void SetIfTable(IfTable* new_iftable)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Class, iftable_), new_iftable, false);
  }

  // Get instance fields of the class (See also GetSFields).
  ObjectArray<Field>* GetIFields() const {
    DCHECK(IsLoaded() || IsErroneous());
    return GetFieldObject<ObjectArray<Field>*>(OFFSET_OF_OBJECT_MEMBER(Class, ifields_), false);
  }

  void SetIFields(ObjectArray<Field>* new_ifields) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(NULL == GetFieldObject<ObjectArray<Field>*>(
        OFFSET_OF_OBJECT_MEMBER(Class, ifields_), false));
    SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Class, ifields_), new_ifields, false);
  }

  size_t NumInstanceFields() const {
    return (GetIFields() != NULL) ? GetIFields()->GetLength() : 0;
  }

  Field* GetInstanceField(uint32_t i) const  // TODO: uint16_t
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_){
    DCHECK_NE(NumInstanceFields(), 0U);
    return GetIFields()->Get(i);
  }

  void SetInstanceField(uint32_t i, Field* f)  // TODO: uint16_t
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_){
    ObjectArray<Field>* ifields= GetFieldObject<ObjectArray<Field>*>(
        OFFSET_OF_OBJECT_MEMBER(Class, ifields_), false);
    ifields->Set(i, f);
  }

  // Returns the number of instance fields containing reference types.
  size_t NumReferenceInstanceFields() const {
    DCHECK(IsResolved() || IsErroneous());
    DCHECK_EQ(sizeof(size_t), sizeof(int32_t));
    return GetField32(OFFSET_OF_OBJECT_MEMBER(Class, num_reference_instance_fields_), false);
  }

  size_t NumReferenceInstanceFieldsDuringLinking() const {
    DCHECK(IsLoaded() || IsErroneous());
    DCHECK_EQ(sizeof(size_t), sizeof(int32_t));
    return GetField32(OFFSET_OF_OBJECT_MEMBER(Class, num_reference_instance_fields_), false);
  }

  void SetNumReferenceInstanceFields(size_t new_num) {
    DCHECK_EQ(sizeof(size_t), sizeof(int32_t));
    SetField32(OFFSET_OF_OBJECT_MEMBER(Class, num_reference_instance_fields_), new_num, false);
  }

  uint32_t GetReferenceInstanceOffsets() const {
    DCHECK(IsResolved() || IsErroneous());
    return GetField32(OFFSET_OF_OBJECT_MEMBER(Class, reference_instance_offsets_), false);
  }

  void SetReferenceInstanceOffsets(uint32_t new_reference_offsets);

  // Beginning of static field data
  static MemberOffset FieldsOffset() {
    return OFFSET_OF_OBJECT_MEMBER(Class, fields_);
  }

  // Returns the number of static fields containing reference types.
  size_t NumReferenceStaticFields() const {
    DCHECK(IsResolved() || IsErroneous());
    DCHECK_EQ(sizeof(size_t), sizeof(int32_t));
    return GetField32(OFFSET_OF_OBJECT_MEMBER(Class, num_reference_static_fields_), false);
  }

  size_t NumReferenceStaticFieldsDuringLinking() const {
    DCHECK(IsLoaded() || IsErroneous());
    DCHECK_EQ(sizeof(size_t), sizeof(int32_t));
    return GetField32(OFFSET_OF_OBJECT_MEMBER(Class, num_reference_static_fields_), false);
  }

  void SetNumReferenceStaticFields(size_t new_num) {
    DCHECK_EQ(sizeof(size_t), sizeof(int32_t));
    SetField32(OFFSET_OF_OBJECT_MEMBER(Class, num_reference_static_fields_), new_num, false);
  }

  // Gets the static fields of the class.
  ObjectArray<Field>* GetSFields() const {
    DCHECK(IsLoaded() || IsErroneous());
    return GetFieldObject<ObjectArray<Field>*>(OFFSET_OF_OBJECT_MEMBER(Class, sfields_), false);
  }

  void SetSFields(ObjectArray<Field>* new_sfields) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(NULL == GetFieldObject<ObjectArray<Field>*>(
        OFFSET_OF_OBJECT_MEMBER(Class, sfields_), false));
    SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Class, sfields_), new_sfields, false);
  }

  size_t NumStaticFields() const {
    return (GetSFields() != NULL) ? GetSFields()->GetLength() : 0;
  }

  Field* GetStaticField(uint32_t i) const  // TODO: uint16_t
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return GetSFields()->Get(i);
  }

  void SetStaticField(uint32_t i, Field* f)  // TODO: uint16_t
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    ObjectArray<Field>* sfields= GetFieldObject<ObjectArray<Field>*>(
        OFFSET_OF_OBJECT_MEMBER(Class, sfields_), false);
    sfields->Set(i, f);
  }

  uint32_t GetReferenceStaticOffsets() const {
    return GetField32(OFFSET_OF_OBJECT_MEMBER(Class, reference_static_offsets_), false);
  }

  void SetReferenceStaticOffsets(uint32_t new_reference_offsets);

  // Find a static or instance field using the JLS resolution order
  Field* FindField(const StringPiece& name, const StringPiece& type)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Finds the given instance field in this class or a superclass.
  Field* FindInstanceField(const StringPiece& name, const StringPiece& type)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Finds the given instance field in this class or a superclass, only searches classes that
  // have the same dex cache.
  Field* FindInstanceField(const DexCache* dex_cache, uint32_t dex_field_idx)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  Field* FindDeclaredInstanceField(const StringPiece& name, const StringPiece& type)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  Field* FindDeclaredInstanceField(const DexCache* dex_cache, uint32_t dex_field_idx)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Finds the given static field in this class or a superclass.
  Field* FindStaticField(const StringPiece& name, const StringPiece& type)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Finds the given static field in this class or superclass, only searches classes that
  // have the same dex cache.
  Field* FindStaticField(const DexCache* dex_cache, uint32_t dex_field_idx)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  Field* FindDeclaredStaticField(const StringPiece& name, const StringPiece& type)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  Field* FindDeclaredStaticField(const DexCache* dex_cache, uint32_t dex_field_idx)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  pid_t GetClinitThreadId() const {
    DCHECK(IsIdxLoaded() || IsErroneous());
    return GetField32(OFFSET_OF_OBJECT_MEMBER(Class, clinit_thread_id_), false);
  }

  void SetClinitThreadId(pid_t new_clinit_thread_id) {
    SetField32(OFFSET_OF_OBJECT_MEMBER(Class, clinit_thread_id_), new_clinit_thread_id, false);
  }

  Class* GetVerifyErrorClass() const {
    // DCHECK(IsErroneous());
    return GetFieldObject<Class*>(OFFSET_OF_OBJECT_MEMBER(Class, verify_error_class_), false);
  }

  uint16_t GetDexTypeIndex() const {
    return GetField32(OFFSET_OF_OBJECT_MEMBER(Class, dex_type_idx_), false);
  }

  void SetDexTypeIndex(uint16_t type_idx) {
    SetField32(OFFSET_OF_OBJECT_MEMBER(Class, dex_type_idx_), type_idx, false);
  }

  static Class* GetJavaLangClass() {
    DCHECK(java_lang_Class_ != NULL);
    return java_lang_Class_;
  }

  // Can't call this SetClass or else gets called instead of Object::SetClass in places.
  static void SetClassClass(Class* java_lang_Class);
  static void ResetClass();

 private:
  void SetVerifyErrorClass(Class* klass) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    CHECK(klass != NULL) << PrettyClass(this);
    SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Class, verify_error_class_), klass, false);
  }

  bool Implements(const Class* klass) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  bool IsArrayAssignableFromArray(const Class* klass) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  bool IsAssignableFromArray(const Class* klass) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // defining class loader, or NULL for the "bootstrap" system loader
  ClassLoader* class_loader_;

  // For array classes, the component class object for instanceof/checkcast
  // (for String[][][], this will be String[][]). NULL for non-array classes.
  Class* component_type_;

  // DexCache of resolved constant pool entries (will be NULL for classes generated by the
  // runtime such as arrays and primitive classes).
  DexCache* dex_cache_;

  // static, private, and <init> methods
  ObjectArray<AbstractMethod>* direct_methods_;

  // instance fields
  //
  // These describe the layout of the contents of an Object.
  // Note that only the fields directly declared by this class are
  // listed in ifields; fields declared by a superclass are listed in
  // the superclass's Class.ifields.
  //
  // All instance fields that refer to objects are guaranteed to be at
  // the beginning of the field list.  num_reference_instance_fields_
  // specifies the number of reference fields.
  ObjectArray<Field>* ifields_;

  // The interface table (iftable_) contains pairs of a interface class and an array of the
  // interface methods. There is one pair per interface supported by this class.  That means one
  // pair for each interface we support directly, indirectly via superclass, or indirectly via a
  // superinterface.  This will be null if neither we nor our superclass implement any interfaces.
  //
  // Why we need this: given "class Foo implements Face", declare "Face faceObj = new Foo()".
  // Invoke faceObj.blah(), where "blah" is part of the Face interface.  We can't easily use a
  // single vtable.
  //
  // For every interface a concrete class implements, we create an array of the concrete vtable_
  // methods for the methods in the interface.
  IfTable* iftable_;

  // descriptor for the class such as "java.lang.Class" or "[C". Lazily initialized by ComputeName
  String* name_;

  // Static fields
  ObjectArray<Field>* sfields_;

  // The superclass, or NULL if this is java.lang.Object, an interface or primitive type.
  Class* super_class_;

  // If class verify fails, we must return same error on subsequent tries.
  Class* verify_error_class_;

  // virtual methods defined in this class; invoked through vtable
  ObjectArray<AbstractMethod>* virtual_methods_;

  // Virtual method table (vtable), for use by "invoke-virtual".  The vtable from the superclass is
  // copied in, and virtual methods from our class either replace those from the super or are
  // appended. For abstract classes, methods may be created in the vtable that aren't in
  // virtual_ methods_ for miranda methods.
  ObjectArray<AbstractMethod>* vtable_;

  // access flags; low 16 bits are defined by VM spec
  uint32_t access_flags_;

  // Total size of the Class instance; used when allocating storage on gc heap.
  // See also object_size_.
  size_t class_size_;

  // tid used to check for recursive <clinit> invocation
  pid_t clinit_thread_id_;

  // type index from dex file
  // TODO: really 16bits
  uint32_t dex_type_idx_;

  // number of instance fields that are object refs
  size_t num_reference_instance_fields_;

  // number of static fields that are object refs
  size_t num_reference_static_fields_;

  // Total object size; used when allocating storage on gc heap.
  // (For interfaces and abstract classes this will be zero.)
  // See also class_size_.
  size_t object_size_;

  // primitive type value, or Primitive::kPrimNot (0); set for generated prim classes
  Primitive::Type primitive_type_;

  // Bitmap of offsets of ifields.
  uint32_t reference_instance_offsets_;

  // Bitmap of offsets of sfields.
  uint32_t reference_static_offsets_;

  // state of class initialization
  Status status_;

  // TODO: ?
  // initiating class loader list
  // NOTE: for classes with low serialNumber, these are unused, and the
  // values are kept in a table in gDvm.
  // InitiatingLoaderList initiating_loader_list_;

  // Location of first static field.
  uint32_t fields_[0];

  // java.lang.Class
  static Class* java_lang_Class_;

  friend struct ClassOffsets;  // for verifying offset information
  DISALLOW_IMPLICIT_CONSTRUCTORS(Class);
};

std::ostream& operator<<(std::ostream& os, const Class::Status& rhs);

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

inline bool Object::IsMethod() const {
  return GetClass()->IsMethodClass();
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

inline Class* Field::GetDeclaringClass() const {
  Class* result = GetFieldObject<Class*>(OFFSET_OF_OBJECT_MEMBER(Field, declaring_class_), false);
  DCHECK(result != NULL);
  DCHECK(result->IsLoaded() || result->IsErroneous());
  return result;
}

inline void Field::SetDeclaringClass(Class *new_declaring_class) {
  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Field, declaring_class_), new_declaring_class, false);
}

inline Class* AbstractMethod::GetDeclaringClass() const {
  Class* result = GetFieldObject<Class*>(OFFSET_OF_OBJECT_MEMBER(AbstractMethod, declaring_class_), false);
  DCHECK(result != NULL) << this;
  DCHECK(result->IsIdxLoaded() || result->IsErroneous()) << this;
  return result;
}

inline void AbstractMethod::SetDeclaringClass(Class *new_declaring_class) {
  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(AbstractMethod, declaring_class_), new_declaring_class, false);
}

inline size_t Array::SizeOf() const {
  // This is safe from overflow because the array was already allocated, so we know it's sane.
  size_t component_size = GetClass()->GetComponentSize();
  int32_t component_count = GetLength();
  size_t header_size = sizeof(Object) + (component_size == sizeof(int64_t) ? 8 : 4);
  size_t data_size = component_count * component_size;
  return header_size + data_size;
}

template<class T>
void ObjectArray<T>::Set(int32_t i, T* object) {
  if (LIKELY(IsValidIndex(i))) {
    if (object != NULL) {
      Class* element_class = GetClass()->GetComponentType();
      if (UNLIKELY(!object->InstanceOf(element_class))) {
        ThrowArrayStoreException(object);
        return;
      }
    }
    MemberOffset data_offset(DataOffset(sizeof(Object*)).Int32Value() + i * sizeof(Object*));
    SetFieldObject(data_offset, object, false);
  }
}

template<class T>
void ObjectArray<T>::SetWithoutChecks(int32_t i, T* object) {
  DCHECK(IsValidIndex(i));
  MemberOffset data_offset(DataOffset(sizeof(Object*)).Int32Value() + i * sizeof(Object*));
  SetFieldObject(data_offset, object, false);
}

template<class T>
void ObjectArray<T>::SetPtrWithoutChecks(int32_t i, T* object) {
  DCHECK(IsValidIndex(i));
  MemberOffset data_offset(DataOffset(sizeof(Object*)).Int32Value() + i * sizeof(Object*));
  SetFieldPtr(data_offset, object, false);
}

template<class T>
T* ObjectArray<T>::GetWithoutChecks(int32_t i) const {
  DCHECK(IsValidIndex(i));
  MemberOffset data_offset(DataOffset(sizeof(Object*)).Int32Value() + i * sizeof(Object*));
  return GetFieldObject<T*>(data_offset, false);
}

template<class T>
void ObjectArray<T>::Copy(const ObjectArray<T>* src, int src_pos,
                          ObjectArray<T>* dst, int dst_pos,
                          size_t length) {
  if (src->IsValidIndex(src_pos) &&
      src->IsValidIndex(src_pos+length-1) &&
      dst->IsValidIndex(dst_pos) &&
      dst->IsValidIndex(dst_pos+length-1)) {
    MemberOffset src_offset(DataOffset(sizeof(Object*)).Int32Value() + src_pos * sizeof(Object*));
    MemberOffset dst_offset(DataOffset(sizeof(Object*)).Int32Value() + dst_pos * sizeof(Object*));
    Class* array_class = dst->GetClass();
    Heap* heap = Runtime::Current()->GetHeap();
    if (array_class == src->GetClass()) {
      // No need for array store checks if arrays are of the same type
      for (size_t i = 0; i < length; i++) {
        Object* object = src->GetFieldObject<Object*>(src_offset, false);
        heap->VerifyObject(object);
        // directly set field, we do a bulk write barrier at the end
        dst->SetField32(dst_offset, reinterpret_cast<uint32_t>(object), false, true);
        src_offset = MemberOffset(src_offset.Uint32Value() + sizeof(Object*));
        dst_offset = MemberOffset(dst_offset.Uint32Value() + sizeof(Object*));
      }
    } else {
      Class* element_class = array_class->GetComponentType();
      CHECK(!element_class->IsPrimitive());
      for (size_t i = 0; i < length; i++) {
        Object* object = src->GetFieldObject<Object*>(src_offset, false);
        if (object != NULL && !object->InstanceOf(element_class)) {
          dst->ThrowArrayStoreException(object);
          return;
        }
        heap->VerifyObject(object);
        // directly set field, we do a bulk write barrier at the end
        dst->SetField32(dst_offset, reinterpret_cast<uint32_t>(object), false, true);
        src_offset = MemberOffset(src_offset.Uint32Value() + sizeof(Object*));
        dst_offset = MemberOffset(dst_offset.Uint32Value() + sizeof(Object*));
      }
    }
    heap->WriteBarrierArray(dst, dst_pos, length);
  }
}

inline void IfTable::SetInterface(int32_t i, Class* interface) {
  DCHECK(interface != NULL);
  DCHECK(interface->IsInterface());
  DCHECK(Get((i * kMax) + kInterface) == NULL);
  Set((i * kMax) + kInterface, interface);
}

class MANAGED ClassClass : public Class {
 private:
  int32_t padding_;
  int64_t serialVersionUID_;
  friend struct ClassClassOffsets;  // for verifying offset information
  DISALLOW_IMPLICIT_CONSTRUCTORS(ClassClass);
};

class MANAGED StringClass : public Class {
 private:
  CharArray* ASCII_;
  Object* CASE_INSENSITIVE_ORDER_;
  uint32_t REPLACEMENT_CHAR_;
  int64_t serialVersionUID_;
  friend struct StringClassOffsets;  // for verifying offset information
  DISALLOW_IMPLICIT_CONSTRUCTORS(StringClass);
};

class MANAGED FieldClass : public Class {
 private:
  Object* ORDER_BY_NAME_AND_DECLARING_CLASS_;
  friend struct FieldClassOffsets;  // for verifying offset information
  DISALLOW_IMPLICIT_CONSTRUCTORS(FieldClass);
};

class MANAGED MethodClass : public Class {
 private:
  Object* ORDER_BY_SIGNATURE_;
  friend struct MethodClassOffsets;  // for verifying offset information
  DISALLOW_IMPLICIT_CONSTRUCTORS(MethodClass);
};

template<class T>
class MANAGED PrimitiveArray : public Array {
 public:
  typedef T ElementType;

  static PrimitiveArray<T>* Alloc(Thread* self, size_t length)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  const T* GetData() const {
    intptr_t data = reinterpret_cast<intptr_t>(this) + DataOffset(sizeof(T)).Int32Value();
    return reinterpret_cast<T*>(data);
  }

  T* GetData() {
    intptr_t data = reinterpret_cast<intptr_t>(this) + DataOffset(sizeof(T)).Int32Value();
    return reinterpret_cast<T*>(data);
  }

  T Get(int32_t i) const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    if (!IsValidIndex(i)) {
      return T(0);
    }
    return GetData()[i];
  }

  void Set(int32_t i, T value) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    if (IsValidIndex(i)) {
      GetData()[i] = value;
    }
  }

  static void SetArrayClass(Class* array_class) {
    CHECK(array_class_ == NULL);
    CHECK(array_class != NULL);
    array_class_ = array_class;
  }

  static void ResetArrayClass() {
    CHECK(array_class_ != NULL);
    array_class_ = NULL;
  }

 private:
  static Class* array_class_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(PrimitiveArray);
};

// C++ mirror of java.lang.String
class MANAGED String : public Object {
 public:
  static MemberOffset CountOffset() {
    return OFFSET_OF_OBJECT_MEMBER(String, count_);
  }

  static MemberOffset ValueOffset() {
    return OFFSET_OF_OBJECT_MEMBER(String, array_);
  }

  static MemberOffset OffsetOffset() {
    return OFFSET_OF_OBJECT_MEMBER(String, offset_);
  }

  const CharArray* GetCharArray() const {
    return GetFieldObject<const CharArray*>(ValueOffset(), false);
  }

  int32_t GetOffset() const {
    int32_t result = GetField32(OffsetOffset(), false);
    DCHECK_LE(0, result);
    return result;
  }

  int32_t GetLength() const;

  int32_t GetHashCode() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void ComputeHashCode() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    SetHashCode(ComputeUtf16Hash(GetCharArray(), GetOffset(), GetLength()));
  }

  int32_t GetUtfLength() const {
    return CountUtf8Bytes(GetCharArray()->GetData() + GetOffset(), GetLength());
  }

  uint16_t CharAt(int32_t index) const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  String* Intern() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static String* AllocFromUtf16(Thread* self,
                                int32_t utf16_length,
                                const uint16_t* utf16_data_in,
                                int32_t hash_code = 0)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static String* AllocFromModifiedUtf8(Thread* self, const char* utf)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static String* AllocFromModifiedUtf8(Thread* self, int32_t utf16_length,
                                       const char* utf8_data_in)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static String* Alloc(Thread* self, Class* java_lang_String, int32_t utf16_length)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static String* Alloc(Thread* self, Class* java_lang_String, CharArray* array)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  bool Equals(const char* modified_utf8) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // TODO: do we need this overload? give it a more intention-revealing name.
  bool Equals(const StringPiece& modified_utf8) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  bool Equals(const String* that) const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Compare UTF-16 code point values not in a locale-sensitive manner
  int Compare(int32_t utf16_length, const char* utf8_data_in);

  // TODO: do we need this overload? give it a more intention-revealing name.
  bool Equals(const uint16_t* that_chars, int32_t that_offset,
              int32_t that_length) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Create a modified UTF-8 encoded std::string from a java/lang/String object.
  std::string ToModifiedUtf8() const;

  int32_t FastIndexOf(int32_t ch, int32_t start) {
    int32_t count = GetLength();
    if (start < 0) {
      start = 0;
    } else if (start > count) {
      start = count;
    }
    const uint16_t* chars = GetCharArray()->GetData() + GetOffset();
    const uint16_t* p = chars + start;
    const uint16_t* end = chars + count;
    while (p < end) {
      if (*p++ == ch) {
        return (p - 1) - chars;
      }
    }
    return -1;
  }

  int32_t CompareTo(String* other) const;

  static Class* GetJavaLangString() {
    DCHECK(java_lang_String_ != NULL);
    return java_lang_String_;
  }

  static void SetClass(Class* java_lang_String);
  static void ResetClass();

 private:
  void SetHashCode(int32_t new_hash_code) {
    DCHECK_EQ(0u,
              GetField32(OFFSET_OF_OBJECT_MEMBER(String, hash_code_), false));
    SetField32(OFFSET_OF_OBJECT_MEMBER(String, hash_code_),
               new_hash_code, false);
  }

  void SetCount(int32_t new_count) {
    DCHECK_LE(0, new_count);
    SetField32(OFFSET_OF_OBJECT_MEMBER(String, count_), new_count, false);
  }

  void SetOffset(int32_t new_offset) {
    DCHECK_LE(0, new_offset);
    DCHECK_GE(GetLength(), new_offset);
    SetField32(OFFSET_OF_OBJECT_MEMBER(String, offset_), new_offset, false);
  }

  void SetArray(CharArray* new_array) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(new_array != NULL);
    SetFieldObject(OFFSET_OF_OBJECT_MEMBER(String, array_), new_array, false);
  }

  // Field order required by test "ValidateFieldOrderOfJavaCppUnionClasses".
  CharArray* array_;

  int32_t count_;

  uint32_t hash_code_;

  int32_t offset_;

  static Class* java_lang_String_;

  friend struct StringOffsets;  // for verifying offset information
  FRIEND_TEST(ObjectTest, StringLength);  // for SetOffset and SetCount
  DISALLOW_IMPLICIT_CONSTRUCTORS(String);
};

// TODO: remove? only used in a unit test of itself.
struct StringHashCode {
  int32_t operator()(String* string) const {
    return string->GetHashCode();
  }
};

inline uint32_t Field::GetAccessFlags() const {
  DCHECK(GetDeclaringClass()->IsLoaded() || GetDeclaringClass()->IsErroneous());
  return GetField32(OFFSET_OF_OBJECT_MEMBER(Field, access_flags_), false);
}

inline MemberOffset Field::GetOffset() const {
  DCHECK(GetDeclaringClass()->IsResolved() || GetDeclaringClass()->IsErroneous());
  return MemberOffset(GetField32(OFFSET_OF_OBJECT_MEMBER(Field, offset_), false));
}

inline MemberOffset Field::GetOffsetDuringLinking() const {
  DCHECK(GetDeclaringClass()->IsLoaded() || GetDeclaringClass()->IsErroneous());
  return MemberOffset(GetField32(OFFSET_OF_OBJECT_MEMBER(Field, offset_), false));
}

inline uint32_t Class::GetAccessFlags() const {
  // Check class is loaded or this is java.lang.String that has a
  // circularity issue during loading the names of its members
  DCHECK(IsLoaded() || IsErroneous() ||
      this == String::GetJavaLangString() ||
      this == Field::GetJavaLangReflectField() ||
      this == AbstractMethod::GetConstructorClass() ||
      this == AbstractMethod::GetMethodClass());
  return GetField32(OFFSET_OF_OBJECT_MEMBER(Class, access_flags_), false);
}

inline uint32_t AbstractMethod::GetAccessFlags() const {
  DCHECK(GetDeclaringClass()->IsIdxLoaded() || GetDeclaringClass()->IsErroneous());
  return GetField32(OFFSET_OF_OBJECT_MEMBER(AbstractMethod, access_flags_), false);
}

inline uint16_t AbstractMethod::GetMethodIndex() const {
  DCHECK(GetDeclaringClass()->IsResolved() || GetDeclaringClass()->IsErroneous());
  return GetField32(OFFSET_OF_OBJECT_MEMBER(AbstractMethod, method_index_), false);
}

inline uint32_t AbstractMethod::GetDexMethodIndex() const {
  DCHECK(GetDeclaringClass()->IsLoaded() || GetDeclaringClass()->IsErroneous());
  return GetField32(OFFSET_OF_OBJECT_MEMBER(AbstractMethod, method_dex_index_), false);
}

inline bool AbstractMethod::CheckIncompatibleClassChange(InvokeType type) {
  switch (type) {
    case kStatic:
      return !IsStatic();
    case kDirect:
      return !IsDirect() || IsStatic();
    case kVirtual: {
      Class* methods_class = GetDeclaringClass();
      return IsDirect() || (methods_class->IsInterface() && !IsMiranda());
    }
    case kSuper:
      return false;  // TODO: appropriate checks for call to super class.
    case kInterface: {
      Class* methods_class = GetDeclaringClass();
      return IsDirect() || !(methods_class->IsInterface() || methods_class->IsObjectClass());
    }
    default:
      LOG(FATAL) << "Unreachable - invocation type: " << type;
      return true;
  }
}

inline void AbstractMethod::AssertPcIsWithinCode(uintptr_t pc) const {
  if (!kIsDebugBuild) {
    return;
  }
  if (IsNative() || IsRuntimeMethod() || IsProxyMethod()) {
    return;
  }
  Runtime* runtime = Runtime::Current();
  if (GetCode() == runtime->GetResolutionStubArray(Runtime::kStaticMethod)->GetData()) {
      return;
  }
  DCHECK(IsWithinCode(pc))
      << PrettyMethod(this)
      << " pc=" << std::hex << pc
      << " code=" << GetCode()
      << " size=" << GetCodeSize();
}

inline String* Class::GetName() const {
  return GetFieldObject<String*>(OFFSET_OF_OBJECT_MEMBER(Class, name_), false);
}
inline void Class::SetName(String* name) {
  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Class, name_), name, false);
}

// C++ mirror of java.lang.Throwable
class MANAGED Throwable : public Object {
 public:
  void SetDetailMessage(String* new_detail_message) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Throwable, detail_message_), new_detail_message, false);
  }
  String* GetDetailMessage() const {
    return GetFieldObject<String*>(OFFSET_OF_OBJECT_MEMBER(Throwable, detail_message_), false);
  }
  std::string Dump() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // This is a runtime version of initCause, you shouldn't use it if initCause may have been
  // overridden. Also it asserts rather than throwing exceptions. Currently this is only used
  // in cases like the verifier where the checks cannot fail and initCause isn't overridden.
  void SetCause(Throwable* cause) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  bool IsCheckedException() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static Class* GetJavaLangThrowable() {
    DCHECK(java_lang_Throwable_ != NULL);
    return java_lang_Throwable_;
  }

  static void SetClass(Class* java_lang_Throwable);
  static void ResetClass();

 private:
  Object* GetStackState() const {
    return GetFieldObject<Object*>(OFFSET_OF_OBJECT_MEMBER(Throwable, stack_state_), true);
  }

  // Field order required by test "ValidateFieldOrderOfJavaCppUnionClasses".
  Throwable* cause_;
  String* detail_message_;
  Object* stack_state_; // Note this is Java volatile:
  Object* stack_trace_;
  Object* suppressed_exceptions_;

  static Class* java_lang_Throwable_;

  friend struct ThrowableOffsets;  // for verifying offset information
  DISALLOW_IMPLICIT_CONSTRUCTORS(Throwable);
};

// C++ mirror of java.lang.StackTraceElement
class MANAGED StackTraceElement : public Object {
 public:
  const String* GetDeclaringClass() const {
    return GetFieldObject<const String*>(
        OFFSET_OF_OBJECT_MEMBER(StackTraceElement, declaring_class_), false);
  }

  const String* GetMethodName() const {
    return GetFieldObject<const String*>(
        OFFSET_OF_OBJECT_MEMBER(StackTraceElement, method_name_), false);
  }

  const String* GetFileName() const {
    return GetFieldObject<const String*>(
        OFFSET_OF_OBJECT_MEMBER(StackTraceElement, file_name_), false);
  }

  int32_t GetLineNumber() const {
    return GetField32(
        OFFSET_OF_OBJECT_MEMBER(StackTraceElement, line_number_), false);
  }

  static StackTraceElement* Alloc(Thread* self,
                                  String* declaring_class,
                                  String* method_name,
                                  String* file_name,
                                  int32_t line_number)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static void SetClass(Class* java_lang_StackTraceElement);

  static void ResetClass();

 private:
  // Field order required by test "ValidateFieldOrderOfJavaCppUnionClasses".
  String* declaring_class_;
  String* file_name_;
  String* method_name_;
  int32_t line_number_;

  static Class* GetStackTraceElement() {
    DCHECK(java_lang_StackTraceElement_ != NULL);
    return java_lang_StackTraceElement_;
  }

  static Class* java_lang_StackTraceElement_;

  friend struct StackTraceElementOffsets;  // for verifying offset information
  DISALLOW_IMPLICIT_CONSTRUCTORS(StackTraceElement);
};

class MANAGED SynthesizedProxyClass : public Class {
 public:
  ObjectArray<Class>* GetInterfaces() {
    return interfaces_;
  }

  ObjectArray<ObjectArray<Class> >* GetThrows() {
    return throws_;
  }

 private:
  ObjectArray<Class>* interfaces_;
  ObjectArray<ObjectArray<Class> >* throws_;
  DISALLOW_IMPLICIT_CONSTRUCTORS(SynthesizedProxyClass);
};

class MANAGED Proxy : public Object {
 private:
  Object* h_;

  friend struct ProxyOffsets;  // for verifying offset information
  DISALLOW_IMPLICIT_CONSTRUCTORS(Proxy);
};

}  // namespace art

#endif  // ART_SRC_OBJECT_H_
