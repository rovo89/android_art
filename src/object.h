// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_OBJECT_H_
#define ART_SRC_OBJECT_H_

#include <vector>

#include "UniquePtr.h"
#include "casts.h"
#include "constants.h"
#include "globals.h"
#include "heap.h"
#include "logging.h"
#include "macros.h"
#include "monitor.h"
#include "monitor.h"
#include "offsets.h"
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
class InterfaceEntry;
class Monitor;
class Method;
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

static const uint32_t kAccPublic = 0x0001;  // class, field, method, ic
static const uint32_t kAccPrivate = 0x0002;  // field, method, ic
static const uint32_t kAccProtected = 0x0004;  // field, method, ic
static const uint32_t kAccStatic = 0x0008;  // field, method, ic
static const uint32_t kAccFinal = 0x0010;  // class, field, method, ic
static const uint32_t kAccSynchronized = 0x0020;  // method (only allowed on natives)
static const uint32_t kAccSuper = 0x0020;  // class (not used in Dalvik)
static const uint32_t kAccVolatile = 0x0040;  // field
static const uint32_t kAccBridge = 0x0040;  // method (1.5)
static const uint32_t kAccTransient = 0x0080;  // field
static const uint32_t kAccVarargs = 0x0080;  // method (1.5)
static const uint32_t kAccNative = 0x0100;  // method
static const uint32_t kAccInterface = 0x0200;  // class, ic
static const uint32_t kAccAbstract = 0x0400;  // class, method, ic
static const uint32_t kAccStrict = 0x0800;  // method
static const uint32_t kAccSynthetic = 0x1000;  // field, method, ic
static const uint32_t kAccAnnotation = 0x2000;  // class, ic (1.5)
static const uint32_t kAccEnum = 0x4000;  // class, field, ic (1.5)

static const uint32_t kAccMiranda = 0x8000;  // method

static const uint32_t kAccJavaFlagsMask = 0xffff;  // bits set from Java sources (low 16)

static const uint32_t kAccConstructor = 0x00010000;  // method (Dalvik only)
static const uint32_t kAccDeclaredSynchronized = 0x00020000;  // method (Dalvik only)

static const uint32_t kAccClassFlagsMask = (kAccPublic
                                            | kAccFinal
                                            | kAccInterface
                                            | kAccAbstract
                                            | kAccSynthetic
                                            | kAccAnnotation
                                            | kAccEnum);
static const uint32_t kAccInnerClassFlagsMask = (kAccClassFlagsMask
                                                 | kAccPrivate
                                                 | kAccProtected
                                                 | kAccStatic);
static const uint32_t kAccFieldFlagsMask = (kAccPublic
                                            | kAccPrivate
                                            | kAccProtected
                                            | kAccStatic
                                            | kAccFinal
                                            | kAccVolatile
                                            | kAccTransient
                                            | kAccSynthetic
                                            | kAccEnum);
static const uint32_t kAccMethodFlagsMask = (kAccPublic
                                             | kAccPrivate
                                             | kAccProtected
                                             | kAccStatic
                                             | kAccFinal
                                             | kAccSynchronized
                                             | kAccBridge
                                             | kAccVarargs
                                             | kAccNative
                                             | kAccAbstract
                                             | kAccStrict
                                             | kAccSynthetic
                                             | kAccConstructor
                                             | kAccDeclaredSynchronized);

// if only kAccClassIsReference is set, we have a soft reference
static const uint32_t kAccClassIsReference          = 0x8000000;  // class is a soft/weak/phantom ref
static const uint32_t kAccClassIsWeakReference      = 0x4000000;  // class is a weak reference
static const uint32_t kAccClassIsFinalizerReference = 0x2000000;  // class is a finalizer reference
static const uint32_t kAccClassIsPhantomReference   = 0x1000000;  // class is a phantom reference

static const uint32_t kAccReferenceFlagsMask = (kAccClassIsReference
                                                | kAccClassIsWeakReference
                                                | kAccClassIsFinalizerReference
                                                | kAccClassIsPhantomReference);

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
#define CLASS_SMALLEST_OFFSET (sizeof(struct Object))
#define CLASS_BITS_PER_WORD (sizeof(unsigned long int) * 8)
#define CLASS_OFFSET_ALIGNMENT 4
#define CLASS_HIGH_BIT ((unsigned int)1 << (CLASS_BITS_PER_WORD - 1))
/*
 * Given an offset, return the bit number which would encode that offset.
 * Local use only.
 */
#define _CLASS_BIT_NUMBER_FROM_OFFSET(byteOffset) \
    (((unsigned int)(byteOffset) - CLASS_SMALLEST_OFFSET) / \
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
    MemberOffset((static_cast<int>(rshift) * CLASS_OFFSET_ALIGNMENT) + \
                 CLASS_SMALLEST_OFFSET)

#define OFFSET_OF_OBJECT_MEMBER(type, field) \
    MemberOffset(OFFSETOF_MEMBER(type, field))

// C++ mirror of java.lang.Object
class Object {
 public:
  static bool InstanceOf(const Object* object, const Class* klass) {
    if (object == NULL) {
      return false;
    }
    return object->InstanceOf(klass);
  }

  static MemberOffset ClassOffset() {
    return OFFSET_OF_OBJECT_MEMBER(Object, klass_);
  }

  Class* GetClass() const {
    return
        GetFieldObject<Class*>(OFFSET_OF_OBJECT_MEMBER(Object, klass_), false);
  }

  void SetClass(Class* new_klass);

  bool InstanceOf(const Class* klass) const;

  size_t SizeOf() const;

  Object* Clone() {
    UNIMPLEMENTED(FATAL);
    return NULL;
  }

  static MemberOffset MonitorOffset() {
    return OFFSET_OF_OBJECT_MEMBER(Object, monitor_);
  }

  Monitor* GetMonitor() const {
    return GetFieldPtr<Monitor*>(
        OFFSET_OF_OBJECT_MEMBER(Object, monitor_), false);
  }

  void SetMonitor(Monitor* monitor) {
    // TODO: threading - compare-and-set
    SetFieldPtr(OFFSET_OF_OBJECT_MEMBER(Object, monitor_), monitor, false);
  }

  void MonitorEnter() {
    GetMonitor()->Enter();
  }

  void MonitorExit() {
    GetMonitor()->Exit();
  }

  void Notify() {
    GetMonitor()->Notify();
  }

  void NotifyAll() {
    GetMonitor()->NotifyAll();
  }

  void Wait() {
    GetMonitor()->Wait();
  }

  void Wait(int64_t timeout) {
    GetMonitor()->Wait(timeout);
  }

  void Wait(int64_t timeout, int32_t nanos) {
    GetMonitor()->Wait(timeout, nanos);
  }

  bool IsClass() const;

  Class* AsClass() {
    DCHECK(IsClass());
    return down_cast<Class*>(this);
  }

  const Class* AsClass() const {
    DCHECK(IsClass());
    return down_cast<const Class*>(this);
  }

  bool IsClassClass() const;

  bool IsObjectArray() const;

  template<class T>
  ObjectArray<T>* AsObjectArray() {
    DCHECK(IsObjectArray());
    return down_cast<ObjectArray<T>*>(this);
  }

  template<class T>
  const ObjectArray<T>* AsObjectArray() const {
    DCHECK(IsObjectArray());
    return down_cast<const ObjectArray<T>*>(this);
  }

  bool IsArrayInstance() const;

  Array* AsArray() {
    DCHECK(IsArrayInstance());
    return down_cast<Array*>(this);
  }

  const Array* AsArray() const {
    DCHECK(IsArrayInstance());
    return down_cast<const Array*>(this);
  }

  bool IsString() const;

  String* AsString() {
    DCHECK(IsString());
    return down_cast<String*>(this);
  }

  bool IsMethod() const;

  Method* AsMethod() {
    DCHECK(IsMethod());
    return down_cast<Method*>(this);
  }

  const Method* AsMethod() const {
    DCHECK(IsMethod());
    return down_cast<const Method*>(this);
  }

  bool IsField() const;

  Field* AsField() {
    DCHECK(IsField());
    return down_cast<Field*>(this);
  }

  const Field* AsField() const {
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
    Heap::VerifyObject(this);
    DCHECK(Thread::Current() == NULL ||
           Thread::Current()->CanAccessDirectReferences());
    const byte* raw_addr = reinterpret_cast<const byte*>(this) +
        field_offset.Int32Value();
    if (is_volatile) {
      UNIMPLEMENTED(WARNING);
    }
    T result = *reinterpret_cast<T const *>(raw_addr);
    Heap::VerifyObject(result);
    return result;
  }

  void SetFieldObject(MemberOffset offset, const Object* new_value,
                      bool is_volatile) {
    // Avoid verifying this when initializing the Class*
    if (offset.Int32Value() != ClassOffset().Int32Value()) {
      Heap::VerifyObject(this);
    }
    Heap::VerifyObject(new_value);
    byte* raw_addr = reinterpret_cast<byte*>(this) + offset.Int32Value();
    if (is_volatile) {
      UNIMPLEMENTED(WARNING);
    }
    *reinterpret_cast<const Object**>(raw_addr) = new_value;
    // TODO: write barrier
  }

  uint32_t GetField32(MemberOffset field_offset, bool is_volatile) const {
    Heap::VerifyObject(this);
    const byte* raw_addr = reinterpret_cast<const byte*>(this) +
        field_offset.Int32Value();
    if (is_volatile) {
      UNIMPLEMENTED(WARNING);
    }
    return *reinterpret_cast<const uint32_t*>(raw_addr);
  }

  void SetField32(MemberOffset offset, uint32_t new_value, bool is_volatile) {
    Heap::VerifyObject(this);
    byte* raw_addr = reinterpret_cast<byte*>(this) + offset.Int32Value();
    if (is_volatile) {
      UNIMPLEMENTED(WARNING);
    }
    *reinterpret_cast<uint32_t*>(raw_addr) = new_value;
  }

  uint64_t GetField64(MemberOffset field_offset, bool is_volatile) const {
    Heap::VerifyObject(this);
    const byte* raw_addr = reinterpret_cast<const byte*>(this) +
        field_offset.Int32Value();
    if (is_volatile) {
      UNIMPLEMENTED(WARNING);
    }
    return *reinterpret_cast<const uint64_t*>(raw_addr);
  }

  void SetField64(MemberOffset offset, uint64_t new_value,
                  bool is_volatile = false) {
    Heap::VerifyObject(this);
    byte* raw_addr = reinterpret_cast<byte*>(this) + offset.Int32Value();
    if (is_volatile) {
      UNIMPLEMENTED(WARNING);
    }
    *reinterpret_cast<uint64_t*>(raw_addr) = new_value;
  }

 protected:
  // Accessors for non-Java type fields
  uint16_t GetField16(MemberOffset field_offset, bool is_volatile) const {
    Heap::VerifyObject(this);
    const byte* raw_addr = reinterpret_cast<const byte*>(this) +
        field_offset.Int32Value();
    if (is_volatile) {
      UNIMPLEMENTED(WARNING);
    }
    return *reinterpret_cast<const uint16_t*>(raw_addr);
  }

  void SetField16(MemberOffset offset, uint16_t new_value, bool is_volatile) {
    Heap::VerifyObject(this);
    byte* raw_addr = reinterpret_cast<byte*>(this) + offset.Int32Value();
    if (is_volatile) {
      UNIMPLEMENTED(WARNING);
    }
    *reinterpret_cast<uint16_t*>(raw_addr) = new_value;
  }

  template<class T>
  T GetFieldPtr(MemberOffset field_offset, bool is_volatile) const {
    Heap::VerifyObject(this);
    const byte* raw_addr = reinterpret_cast<const byte*>(this) +
        field_offset.Int32Value();
    if (is_volatile) {
      UNIMPLEMENTED(WARNING);
    }
    return *reinterpret_cast<T const *>(raw_addr);
  }

  template<typename T>
  void SetFieldPtr(MemberOffset offset, T new_value, bool is_volatile) {
    Heap::VerifyObject(this);
    byte* raw_addr = reinterpret_cast<byte*>(this) + offset.Int32Value();
    if (is_volatile) {
      UNIMPLEMENTED(WARNING);
    }
    *reinterpret_cast<T*>(raw_addr) = new_value;
  }

 private:
  Class* klass_;

  Monitor* monitor_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(Object);
};

class ObjectLock {
 public:
  explicit ObjectLock(Object* object) : obj_(object) {
    CHECK(object != NULL);
    obj_->MonitorEnter();
  }

  ~ObjectLock() {
    obj_->MonitorExit();
  }

  void Wait(int64_t millis = 0) {
    return obj_->Wait(millis);
  }

  void Notify() {
    obj_->Notify();
  }

  void NotifyAll() {
    obj_->NotifyAll();
  }

 private:
  Object* obj_;
  DISALLOW_COPY_AND_ASSIGN(ObjectLock);
};

// C++ mirror of java.lang.reflect.AccessibleObject
class AccessibleObject : public Object {
 private:
  // Field order required by test "ValidateFieldOrderOfJavaCppUnionClasses".
  uint32_t java_flag_;  // can accessibility checks be bypassed
};

// C++ mirror of java.lang.reflect.Field
class Field : public AccessibleObject {
 public:
  Class* GetDeclaringClass() const;

  void SetDeclaringClass(Class *new_declaring_class);

  const String* GetName() const;

  void SetName(String* new_name);

  uint32_t GetAccessFlags() const;

  void SetAccessFlags(uint32_t new_access_flags) {
    SetField32(OFFSET_OF_OBJECT_MEMBER(Field, access_flags_), new_access_flags,
               false);
  }

  bool IsStatic() const {
    return (GetAccessFlags() & kAccStatic) != 0;
  }

  uint32_t GetTypeIdx() const;

  void SetTypeIdx(uint32_t type_idx);

  // Gets type using type index and resolved types in the dex cache, may be null
  // if type isn't yet resolved
  Class* GetTypeDuringLinking() const;

  // Performs full resolution, may return null and set exceptions if type cannot
  // be resolved
  Class* GetType() const;

  // Offset to field within an Object
  MemberOffset GetOffset() const;

  MemberOffset GetOffsetDuringLinking() const;

  void SetOffset(MemberOffset num_bytes);

  // field access, null object for static fields
  bool GetBoolean(const Object* object) const;
  void SetBoolean(Object* object, bool z) const;
  int8_t GetByte(const Object* object) const;
  void SetByte(Object* object, int8_t b) const;
  uint16_t GetChar(const Object* object) const;
  void SetChar(Object* object, uint16_t c) const;
  uint16_t GetShort(const Object* object) const;
  void SetShort(Object* object, uint16_t s) const;
  int32_t GetInt(const Object* object) const;
  void SetInt(Object* object, int32_t i) const;
  int64_t GetLong(const Object* object) const;
  void SetLong(Object* object, int64_t j) const;
  float GetFloat(const Object* object) const;
  void SetFloat(Object* object, float f) const;
  double GetDouble(const Object* object) const;
  void SetDouble(Object* object, double d) const;
  Object* GetObject(const Object* object) const;
  void SetObject(Object* object, const Object* l) const;

  // Slow path routines for static field access when field was unresolved at
  // compile time
  static uint32_t Get32StaticFromCode(uint32_t field_idx,
                                      const Method* referrer);
  static void Set32StaticFromCode(uint32_t field_idx, const Method* referrer,
                                  uint32_t new_value);
  static uint64_t Get64StaticFromCode(uint32_t field_idx,
                                      const Method* referrer);
  static void Set64StaticFromCode(uint32_t field_idx, const Method* referrer,
                                  uint64_t new_value);
  static Object* GetObjStaticFromCode(uint32_t field_idx,
                                      const Method* referrer);
  static void SetObjStaticFromCode(uint32_t field_idx, const Method* referrer,
                                   Object* new_value);

  static Class* GetJavaLangReflectField() {
    DCHECK(java_lang_reflect_Field_ != NULL);
    return java_lang_reflect_Field_;
  }

  static void SetClass(Class* java_lang_reflect_Field);
  static void ResetClass();

 private:
  bool IsVolatile() const {
    return (GetAccessFlags() & kAccVolatile) != 0;
  }

  // private implementation of field access using raw data
  uint32_t Get32(const Object* object) const;
  void Set32(Object* object, uint32_t new_value) const;
  uint64_t Get64(const Object* object) const;
  void Set64(Object* object, uint64_t new_value) const;
  Object* GetObj(const Object* object) const;
  void SetObj(Object* object, const Object* new_value) const;

  // Field order required by test "ValidateFieldOrderOfJavaCppUnionClasses".
  // The class in which this field is declared.
  Class* declaring_class_;
  Object* generic_type_;
  uint32_t generic_types_are_initialized_;
  const String* name_;
  // Offset of field within an instance or in the Class' static fields
  uint32_t offset_;
  // Type of the field
  // TODO: unused by ART (which uses the type_idx below), remove
  Class* type_;

  // TODO: expose these fields in the Java version of this Object
  uint32_t access_flags_;
  // Dex cache index of resolved type
  uint32_t type_idx_;

  static Class* java_lang_reflect_Field_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(Field);
};

// C++ mirror of java.lang.reflect.Method
class Method : public AccessibleObject {
 public:
  // An function that invokes a method with an array of its arguments.
  typedef void InvokeStub(Method* method,
                          Object* obj,
                          Thread* thread,
                          byte* args,
                          JValue* result);

  Class* GetDeclaringClass() const;

  void SetDeclaringClass(Class *new_declaring_class);

  static MemberOffset DeclaringClassOffset() {
    return MemberOffset(OFFSETOF_MEMBER(Method, declaring_class_));
  }

  // Returns the method name, e.g. "<init>" or "eatLunch"
  const String* GetName() const;

  void SetName(String* new_name);

  const char* GetShorty() const;

  void SetShorty(const char* new_shorty) {
    DCHECK(NULL == GetFieldPtr<const char*>(
        OFFSET_OF_OBJECT_MEMBER(Method, shorty_), false));
    DCHECK_LE(1u, strlen(new_shorty));
    SetFieldPtr(OFFSET_OF_OBJECT_MEMBER(Method, shorty_), new_shorty, false);
  }

  const String* GetSignature() const;

  void SetSignature(String* new_signature);

  bool HasSameNameAndDescriptor(const Method* that) const;

  uint32_t GetAccessFlags() const;

  void SetAccessFlags(uint32_t new_access_flags) {
    SetField32(OFFSET_OF_OBJECT_MEMBER(Method, access_flags_), new_access_flags,
               false);
  }

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
    return (access_flags_ & kAccConstructor) != 0;
  }

  // Returns true if the method is static, private, or a constructor.
  bool IsDirect() const {
    return IsStatic() || IsPrivate() || IsConstructor();
  }

  // Returns true if the method is declared synchronized.
  bool IsSynchronized() const {
    uint32_t synchonized = kAccSynchronized | kAccDeclaredSynchronized;
    return (GetAccessFlags() & synchonized) != 0;
  }

  // Returns true if the method is declared final.
  bool IsFinal() const {
    return (GetAccessFlags() & kAccFinal) != 0;
  }

  // Returns true if the method is declared native.
  bool IsNative() const {
    return (GetAccessFlags() & kAccNative) != 0;
  }

  // Returns true if the method is declared abstract.
  bool IsAbstract() const {
    return (GetAccessFlags() & kAccAbstract) != 0;
  }

  bool IsSynthetic() const {
    return (GetAccessFlags() & kAccSynthetic) != 0;
  }

  uint16_t GetMethodIndex() const;

  size_t GetVtableIndex() const {
    return GetMethodIndex();
  }

  void SetMethodIndex(uint16_t new_method_index) {
    SetField16(OFFSET_OF_OBJECT_MEMBER(Method, method_index_),
               new_method_index, false);
  }

  static MemberOffset MethodIndexOffset() {
    return OFFSET_OF_OBJECT_MEMBER(Method, method_index_);
  }

  uint32_t GetCodeItemOffset() const {
    return GetField32(OFFSET_OF_OBJECT_MEMBER(Method, code_item_offset_), false);
  }

  void SetCodeItemOffset(uint32_t new_code_off) {
    SetField32(OFFSET_OF_OBJECT_MEMBER(Method, code_item_offset_),
               new_code_off, false);
  }

  // Number of 32bit registers that would be required to hold all the arguments
  static size_t NumArgRegisters(const StringPiece& shorty);

  // Number of argument bytes required for densely packing the
  // arguments into an array of arguments.
  size_t NumArgArrayBytes() const;

  uint16_t NumRegisters() const;

  void SetNumRegisters(uint16_t new_num_registers) {
    SetField16(OFFSET_OF_OBJECT_MEMBER(Method, num_registers_),
               new_num_registers, false);
  }

  uint16_t NumIns() const;

  void SetNumIns(uint16_t new_num_ins) {
    SetField16(OFFSET_OF_OBJECT_MEMBER(Method, num_ins_),
               new_num_ins, false);
  }

  uint16_t NumOuts() const;

  void SetNumOuts(uint16_t new_num_outs) {
    SetField16(OFFSET_OF_OBJECT_MEMBER(Method, num_outs_),
               new_num_outs, false);
  }

  uint32_t GetProtoIdx() const;

  void SetProtoIdx(uint32_t new_proto_idx) {
    SetField32(OFFSET_OF_OBJECT_MEMBER(Method, proto_idx_), new_proto_idx, false);
  }

  ObjectArray<String>* GetDexCacheStrings() const;
  void SetDexCacheStrings(ObjectArray<String>* new_dex_cache_strings);

  static MemberOffset DexCacheStringsOffset() {
    return OFFSET_OF_OBJECT_MEMBER(Method, dex_cache_strings_);
  }

  ObjectArray<Class>* GetDexCacheResolvedTypes() const;
  void SetDexCacheResolvedTypes(ObjectArray<Class>* new_dex_cache_types);

  ObjectArray<Method>* GetDexCacheResolvedMethods() const;
  void SetDexCacheResolvedMethods(ObjectArray<Method>* new_dex_cache_methods);

  ObjectArray<Field>* GetDexCacheResolvedFields() const;
  void SetDexCacheResolvedFields(ObjectArray<Field>* new_dex_cache_fields);

  CodeAndDirectMethods* GetDexCacheCodeAndDirectMethods() const;
  void SetDexCacheCodeAndDirectMethods(CodeAndDirectMethods* new_value);

  ObjectArray<StaticStorageBase>* GetDexCacheInitializedStaticStorage() const;
  void SetDexCacheInitializedStaticStorage(ObjectArray<StaticStorageBase>* new_value);

  void SetReturnTypeIdx(uint32_t new_return_type_idx);

  Class* GetReturnType() const;

  bool IsReturnAReference() const;

  bool IsReturnAFloat() const;

  bool IsReturnADouble() const;

  bool IsReturnAFloatOrDouble() const {
    return IsReturnAFloat() || IsReturnADouble();
  }

  bool IsReturnALong() const;

  bool IsReturnVoid() const;

  // "Args" may refer to any of the 3 levels of "Args."
  // To avoid confusion, our code will denote which "Args" clearly:
  //  1. UserArgs: Args that a user see.
  //  2. Args: Logical JVM-level Args. E.g., the first in Args will be the
  //       receiver.
  //  3. CConvArgs: Calling Convention Args, which is physical-level Args.
  //       E.g., the first in Args is Method* for both static and non-static
  //       methods. And CConvArgs doesn't deal with the receiver because
  //       receiver is hardwired in an implicit register, so CConvArgs doesn't
  //       need to deal with it.
  //
  // The number of Args that should be supplied to this method
  size_t NumArgs() const {
    // "1 +" because the first in Args is the receiver.
    // "- 1" because we don't count the return type.
    return (IsStatic() ? 0 : 1) + strlen(GetShorty()) - 1;
  }

  // The number of reference arguments to this method including implicit this
  // pointer.
  size_t NumReferenceArgs() const;

  // The number of long or double arguments.
  size_t NumLongOrDoubleArgs() const;

  // The number of reference arguments to this method before the given
  // parameter index
  size_t NumReferenceArgsBefore(unsigned int param) const;

  // Is the given method parameter a reference?
  bool IsParamAReference(unsigned int param) const;

  // Is the given method parameter a long or double?
  bool IsParamALongOrDouble(unsigned int param) const;

  // Size in bytes of the given parameter
  size_t ParamSize(unsigned int param) const;

  // Size in bytes of the return value
  size_t ReturnSize() const;

  const void* GetCode() const {
    return GetFieldPtr<const void*>(OFFSET_OF_OBJECT_MEMBER(Method, code_), false);
  }

  bool HasCode() const {
    return GetCode() != NULL;
  }

  void SetCode(const byte* compiled_code,
               size_t byte_count,
               InstructionSet set);

  static MemberOffset GetCodeOffset() {
    return OFFSET_OF_OBJECT_MEMBER(Method, code_);
  }

  size_t GetFrameSizeInBytes() const {
    DCHECK(sizeof(size_t) == sizeof(uint32_t));
    size_t result = GetField32(
        OFFSET_OF_OBJECT_MEMBER(Method, frame_size_in_bytes_), false);
    DCHECK_LE(static_cast<size_t>(kStackAlignment), result);
    return result;
  }

  void SetFrameSizeInBytes(size_t new_frame_size_in_bytes) {
    DCHECK(sizeof(size_t) == sizeof(uint32_t));
    DCHECK_LE(static_cast<size_t>(kStackAlignment), new_frame_size_in_bytes);
    SetField32(OFFSET_OF_OBJECT_MEMBER(Method, frame_size_in_bytes_),
               new_frame_size_in_bytes, false);
  }

  size_t GetReturnPcOffsetInBytes() const {
    DCHECK(sizeof(size_t) == sizeof(uint32_t));
    return GetField32(
        OFFSET_OF_OBJECT_MEMBER(Method, return_pc_offset_in_bytes_), false);
  }

  void SetReturnPcOffsetInBytes(size_t return_pc_offset_in_bytes) {
    DCHECK(sizeof(size_t) == sizeof(uint32_t));
    DCHECK_LT(return_pc_offset_in_bytes, GetFrameSizeInBytes());
    SetField32(OFFSET_OF_OBJECT_MEMBER(Method, return_pc_offset_in_bytes_),
               return_pc_offset_in_bytes, false);
  }

  void RegisterNative(const void* native_method) {
    CHECK(native_method != NULL);
    SetFieldPtr<const void*>(OFFSET_OF_OBJECT_MEMBER(Method, native_method_),
                             native_method, false);
  }

  void UnregisterNative() {
    SetFieldPtr<const void*>(OFFSET_OF_OBJECT_MEMBER(Method, native_method_),
                             NULL, false);
  }

  static MemberOffset NativeMethodOffset() {
    return OFFSET_OF_OBJECT_MEMBER(Method, native_method_);
  }

  // Native to managed invocation stub entry point
  InvokeStub* GetInvokeStub() const {
    InvokeStub* result = GetFieldPtr<InvokeStub*>(
        OFFSET_OF_OBJECT_MEMBER(Method, invoke_stub_), false);
    // TODO: DCHECK(result != NULL);  should be ahead of time compiled
    return result;
  }

  static MemberOffset GetInvokeStubOffset() {
    return OFFSET_OF_OBJECT_MEMBER(Method, invoke_stub_);
  }

  void SetInvokeStub(const InvokeStub* new_invoke_stub) {
    SetFieldPtr<const InvokeStub*>(
        OFFSET_OF_OBJECT_MEMBER(Method, invoke_stub_), new_invoke_stub, false);
  }

  void SetFpSpillMask(uint32_t fp_spill_mask) {
    // Computed during compilation
    SetField32(OFFSET_OF_OBJECT_MEMBER(Method, fp_spill_mask_),
               fp_spill_mask, false);
  }

  void SetCoreSpillMask(uint32_t core_spill_mask) {
    // Computed during compilation
    SetField32(OFFSET_OF_OBJECT_MEMBER(Method, core_spill_mask_),
               core_spill_mask, false);
  }

  // Converts a native PC to a dex PC.  TODO: this is a no-op
  // until we associate a PC mapping table with each method.
  uintptr_t ToDexPC(const uintptr_t pc) const {
    return pc;
  }

  // Converts a dex PC to a native PC.  TODO: this is a no-op
  // until we associate a PC mapping table with each method.
  uintptr_t ToNativePC(const uintptr_t pc) const {
    return pc;
  }

  static Class* GetJavaLangReflectMethod() {
    DCHECK(java_lang_reflect_Method_ != NULL);
    return java_lang_reflect_Method_;
  }

  static void SetClass(Class* java_lang_reflect_Method);
  static void ResetClass();

 private:
  uint32_t GetReturnTypeIdx() const;

  // TODO: the image writer should know the offsets of these fields as they
  // should appear in the libcore Java mirror
  friend class ImageWriter;

  // Field order required by test "ValidateFieldOrderOfJavaCppUnionClasses".
  // the class we are a part of
  Class* declaring_class_;
  ObjectArray<Class>* java_exception_types_;
  Object* java_formal_type_parameters_;
  Object* java_generic_exception_types_;
  Object* java_generic_parameter_types_;
  Object* java_generic_return_type_;
  Class* java_return_type_;  // Unused by ART
  String* name_;
  ObjectArray<Class>* java_parameter_types_;
  uint32_t java_generic_types_are_initialized_;
  uint32_t java_slot_;

  // TODO: start of non-Java mirror fields, place these in the Java piece

  // access flags; low 16 bits are defined by spec (could be uint16_t?)
  uint32_t access_flags_;

  // For concrete virtual methods, this is the offset of the method
  // in Class::vtable_.
  //
  // For abstract methods in an interface class, this is the offset
  // of the method in "iftable_[n]->method_index_array_".
  uint16_t method_index_;

  // Method bounds; not needed for an abstract method.
  //
  // For a native method, we compute the size of the argument list, and
  // set "insSize" and "registerSize" equal to it.
  uint16_t num_registers_;  // ins + locals
  uint16_t num_outs_;
  uint16_t num_ins_;

  // Total size in bytes of the frame
  size_t frame_size_in_bytes_;

  // Architecture-dependent register spill masks
  uint32_t core_spill_mask_;
  uint32_t fp_spill_mask_;

  // The method descriptor.  This represents the parameters a method
  // takes and value it returns.  This string is a list of the type
  // descriptors for the parameters enclosed in parenthesis followed
  // by the return type descriptor.  For example, for the method
  //
  //   Object mymethod(int i, double d, Thread t)
  //
  // the method descriptor would be
  //
  //   (IDLjava/lang/Thread;)Ljava/lang/Object;
  String* signature_;

  // Method prototype descriptor string (return and argument types).
  uint32_t proto_idx_;

  // Offset to the CodeItem.
  uint32_t code_item_offset_;

  // Index of the return type
  uint32_t java_return_type_idx_;

  // The short-form method descriptor string. TODO: make String*
  const char* shorty_;

  // short cuts to declaring_class_->dex_cache_ members for fast compiled code
  // access
  ObjectArray<String>* dex_cache_strings_;
  ObjectArray<Class>* dex_cache_resolved_types_;
  ObjectArray<Method>* dex_cache_resolved_methods_;
  ObjectArray<Field>* dex_cache_resolved_fields_;
  CodeAndDirectMethods* dex_cache_code_and_direct_methods_;
  ObjectArray<StaticStorageBase>* dex_cache_initialized_static_storage_;

 private:
  // Compiled code associated with this method
  UniquePtr<MemMap> code_area_;
  const void* code_;

  // Instruction set of the compiled code
  InstructionSet code_instruction_set_;

  // Size in bytes of compiled code associated with this method
  const uint32_t code_size_;

  // Offset of return PC within frame for compiled code (in bytes)
  // Offset of PC within compiled code (in bytes)
  size_t return_pc_offset_in_bytes_;

  // Any native method registered with this method
  const void* native_method_;

  // Native invocation stub entry point.
  const InvokeStub* invoke_stub_;

  static Class* java_lang_reflect_Method_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(Method);
};

class Array : public Object {
 public:
  static size_t SizeOf(size_t component_count,
                       size_t component_size) {
    return sizeof(Array) + component_count * component_size;
  }

  // Given the context of a calling Method, use its DexCache to
  // resolve a type to an array Class. If it cannot be resolved, throw
  // an error. If it can, use it to create an array.
  static Array* AllocFromCode(uint32_t type_idx, Method* method, int32_t component_count);

  // A convenience for code that doesn't know the component size,
  // and doesn't want to have to work it out itself.
  static Array* Alloc(Class* array_class, int32_t component_count);

  static Array* Alloc(Class* array_class, int32_t component_count, size_t component_size);

  size_t SizeOf() const;

  int32_t GetLength() const {
    return GetField32(OFFSET_OF_OBJECT_MEMBER(Array, length_), false);
  }

  void SetLength(int32_t length) {
    CHECK_LE(0, length);
    SetField32(OFFSET_OF_OBJECT_MEMBER(Array, length_), length, false);
  }

  static MemberOffset LengthOffset() {
    return OFFSET_OF_OBJECT_MEMBER(Array, length_);
  }

  static MemberOffset DataOffset() {
    return OFFSET_OF_OBJECT_MEMBER(Array, first_element_);
  }

  void* GetRawData() {
    return reinterpret_cast<void*>(first_element_);
  }

 protected:
  bool IsValidIndex(int32_t index) const {
    if (index < 0 || index >= length_) {
      Thread* self = Thread::Current();
      self->ThrowNewException("Ljava/lang/ArrayIndexOutOfBoundsException;",
          "length=%i; index=%i", length_, index);
      return false;
    }
    return true;
  }

 private:
  // The number of array elements.
  int32_t length_;
  // Padding to ensure the first member defined by a subclass begins on a 8-byte boundary
  int32_t padding_;
  // Marker for the data (used by generated code)
  uint32_t first_element_[0];

  DISALLOW_IMPLICIT_CONSTRUCTORS(Array);
};

template<class T>
class ObjectArray : public Array {
 public:
  static ObjectArray<T>* Alloc(Class* object_array_class, int32_t length);

  T* Get(int32_t i) const;

  void Set(int32_t i, T* object);

  // Set element without bound and element type checks, to be used in limited
  // circumstances, such as during boot image writing
  void SetWithoutChecks(int32_t i, T* object);

  static void Copy(const ObjectArray<T>* src, int src_pos,
                   ObjectArray<T>* dst, int dst_pos,
                   size_t length);

  ObjectArray<T>* CopyOf(int32_t new_length);

 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(ObjectArray);
};

template<class T>
ObjectArray<T>* ObjectArray<T>::Alloc(Class* object_array_class, int32_t length) {
  return Array::Alloc(object_array_class, length, sizeof(uint32_t))->AsObjectArray<T>();
}

template<class T>
T* ObjectArray<T>::Get(int32_t i) const {
  if (!IsValidIndex(i)) {
    return NULL;
  }
  MemberOffset data_offset(DataOffset().Int32Value() + i * sizeof(Object*));
  return GetFieldObject<T*>(data_offset, false);
}

template<class T>
ObjectArray<T>* ObjectArray<T>::CopyOf(int32_t new_length) {
  ObjectArray<T>* new_array = Alloc(GetClass(), new_length);
  Copy(this, 0, new_array, 0, std::min(GetLength(), new_length));
  return new_array;
}

// Type for the InitializedStaticStorage table. Currently the Class
// provides the static storage. However, this might change to improve
// image sharing, so we use this type to avoid assumptions on the
// current storage.
class StaticStorageBase {};

// C++ mirror of java.lang.Class
class Class : public Object, public StaticStorageBase {
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
  // Class values in super_class_ and interfaces_ have not been
  // populated based on super_class_type_idx_ and
  // interfaces_type_idx_. The new Class can then be inserted into the
  // classes table.
  //
  // kStatusLoaded: After taking a lock on Class, the ClassLinker will
  // attempt to move a kStatusIdx class forward to kStatusLoaded by
  // using ResolveClass to initialize the super_class_ and interfaces_.
  //
  // kStatusResolved: Still holding the lock on Class, the ClassLinker
  // shows linking is complete and fields of the Class populated by making
  // it kStatusResolved. Java allows circularities of the form where a super
  // class has a field that is of the type of the sub class. We need to be able
  // to fully resolve super classes while resolving types for fields.

  enum Status {
    kStatusError = -1,
    kStatusNotReady = 0,
    kStatusIdx = 1,  // loaded, DEX idx in super_class_type_idx_ and interfaces_type_idx_
    kStatusLoaded = 2,  // DEX idx values resolved
    kStatusResolved = 3,  // part of linking
    kStatusVerifying = 4,  // in the process of being verified
    kStatusVerified = 5,  // logically part of linking; done pre-init
    kStatusInitializing = 6,  // class init in progress
    kStatusInitialized = 7,  // ready to go
  };

  enum PrimitiveType {
    kPrimNot = 0,
    kPrimBoolean,
    kPrimByte,
    kPrimChar,
    kPrimShort,
    kPrimInt,
    kPrimLong,
    kPrimFloat,
    kPrimDouble,
    kPrimVoid,
  };

  Status GetStatus() const {
    CHECK(sizeof(Status) == sizeof(uint32_t));
    return static_cast<Status>(
        GetField32(OFFSET_OF_OBJECT_MEMBER(Class, status_), false));
  }

  void SetStatus(Status new_status);

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
  bool IsLinked() const {
    return GetStatus() >= kStatusResolved;
  }

  // Returns true if the class has been verified.
  bool IsVerified() const {
    return GetStatus() >= kStatusVerified;
  }

  // Returns true if the class is initialized.
  bool IsInitialized() const {
    return GetStatus() == kStatusInitialized;
  }

  uint32_t GetAccessFlags() const;

  void SetAccessFlags(uint32_t new_access_flags) {
    SetField32(OFFSET_OF_OBJECT_MEMBER(Class, access_flags_), new_access_flags,
               false);
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
    return (GetAccessFlags() & ~kAccReferenceFlagsMask) == kAccClassIsReference;
  }

  bool IsFinalizerReferenceClass() const {
    return (GetAccessFlags() & kAccClassIsFinalizerReference) != 0;
  }

  bool IsPhantomReferenceClass() const {
    return (GetAccessFlags() & kAccClassIsPhantomReference) != 0;
  }

  PrimitiveType GetPrimitiveType() const {
    CHECK(sizeof(PrimitiveType) == sizeof(int32_t));
    return static_cast<PrimitiveType>(
        GetField32(OFFSET_OF_OBJECT_MEMBER(Class, primitive_type_), false));
  }

  void SetPrimitiveType(PrimitiveType new_type) {
    CHECK(sizeof(PrimitiveType) == sizeof(int32_t));
    SetField32(OFFSET_OF_OBJECT_MEMBER(Class, primitive_type_), new_type,
               false);
  }

  // Returns true if the class is a primitive type.
  bool IsPrimitive() const {
    return GetPrimitiveType() != kPrimNot;
  }

  bool IsPrimitiveBoolean() const {
    return GetPrimitiveType() == kPrimBoolean;
  }

  bool IsPrimitiveByte() const {
    return GetPrimitiveType() == kPrimByte;
  }

  bool IsPrimitiveChar() const {
    return GetPrimitiveType() == kPrimChar;
  }

  bool IsPrimitiveShort() const {
    return GetPrimitiveType() == kPrimShort;
  }

  bool IsPrimitiveInt() const {
    return GetPrimitiveType() == kPrimInt;
  }

  bool IsPrimitiveLong() const {
    return GetPrimitiveType() == kPrimLong;
  }

  bool IsPrimitiveFloat() const {
    return GetPrimitiveType() == kPrimFloat;
  }

  bool IsPrimitiveDouble() const {
    return GetPrimitiveType() == kPrimDouble;
  }

  bool IsPrimitiveVoid() const {
    return GetPrimitiveType() == kPrimVoid;
  }

  size_t PrimitiveSize() const;

  bool IsArrayClass() const {
    return GetArrayRank() != 0;
  }

  int32_t GetArrayRank() const {
    int32_t result = GetField32(OFFSET_OF_OBJECT_MEMBER(Class, array_rank_),
                                false);
    return result;
  }

  void SetArrayRank(int32_t new_array_rank) {
    DCHECK_EQ(0, GetArrayRank());
    SetField32(OFFSET_OF_OBJECT_MEMBER(Class, array_rank_), new_array_rank,
               false);
  }

  Class* GetComponentType() const {
    DCHECK(IsArrayClass());
    return GetFieldObject<Class*>(
        OFFSET_OF_OBJECT_MEMBER(Class, component_type_), false);
  }

  void SetComponentType(Class* new_component_type) {
    DCHECK(GetComponentType() == NULL);
    DCHECK(new_component_type != NULL);
    SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Class, component_type_),
                   new_component_type, false);
  }

  size_t GetComponentSize() const {
    return GetTypeSize(GetComponentType()->GetDescriptor());
  }

  bool IsObjectClass() const {
    return !IsPrimitive() && GetSuperClass() == NULL;
  }

  static bool CanPutArrayElementFromCode(const Class* elementClass, const Class* arrayClass);

  // Given the context of a calling Method, use its DexCache to
  // resolve a type to a Class. If it cannot be resolved, throw an
  // error. If it can, use it to create an instance.
  static Object* AllocObjectFromCode(uint32_t type_idx, Method* method);

  // Creates a raw object instance but does not invoke the default constructor.
  Object* AllocObject();

  static size_t GetTypeSize(const String* descriptor);

  const String* GetDescriptor() const {
    const String* result = GetFieldObject<const String*>(
        OFFSET_OF_OBJECT_MEMBER(Class, descriptor_), false);
    // DCHECK(result != NULL);  // may be NULL prior to class linker initialization
    // DCHECK_NE(0, result->GetLength());  // TODO: keep?
    return result;
  }

  void SetDescriptor(String* new_descriptor);

  bool IsVariableSize() const {
    // Classes and arrays vary in size, and so the object_size_ field cannot
    // be used to get their instance size
    return IsClassClass() || IsArrayClass();
  }

  size_t SizeOf() const {
    CHECK(sizeof(size_t) == sizeof(int32_t));
    return GetField32(OFFSET_OF_OBJECT_MEMBER(Class, class_size_), false);
  }

  size_t GetClassSize() const {
    CHECK(sizeof(size_t) == sizeof(uint32_t));
    return GetField32(OFFSET_OF_OBJECT_MEMBER(Class, class_size_), false);
  }

  void SetClassSize(size_t new_class_size) {
    DCHECK_GE(new_class_size, GetClassSize());
    SetField32(OFFSET_OF_OBJECT_MEMBER(Class, class_size_), new_class_size,
               false);
  }

  size_t GetObjectSize() const {
    CHECK(!IsVariableSize());
    CHECK(sizeof(size_t) == sizeof(int32_t));
    size_t result = GetField32(OFFSET_OF_OBJECT_MEMBER(Class, object_size_),
                               false);
    CHECK_GE(result, sizeof(Object));
    return result;
  }

  void SetObjectSize(size_t new_object_size) {
    DCHECK(!IsVariableSize());
    CHECK(sizeof(size_t) == sizeof(int32_t));
    return SetField32(OFFSET_OF_OBJECT_MEMBER(Class, object_size_),
                      new_object_size, false);
  }

  // Returns true if this class is in the same packages as that class.
  bool IsInSamePackage(const Class* that) const;

  static bool IsInSamePackage(const String* descriptor1,
                              const String* descriptor2);

  // Returns true if this class can access that class.
  bool CanAccess(const Class* that) const {
    return that->IsPublic() || this->IsInSamePackage(that);
  }

  bool IsAssignableFrom(const Class* klass) const {
    DCHECK(klass != NULL);
    if (this == klass) {
      // Can always assign to things of the same type
      return true;
    } else if(IsObjectClass()) {
      // Can assign any reference to java.lang.Object
      return !klass->IsPrimitive();
    } else if (IsInterface()) {
      return klass->Implements(this);
    } else if (klass->IsArrayClass()) {
      return IsAssignableFromArray(klass);
    } else {
      return klass->IsSubClass(this);
    }
  }

  Class* GetSuperClass() const {
    // Can only get super class for loaded classes (hack for when runtime is
    // initializing)
    DCHECK(IsLoaded() || Runtime::Current() == NULL);
    return GetFieldObject<Class*>(
        OFFSET_OF_OBJECT_MEMBER(Class, super_class_), false);
  }

  void SetSuperClass(Class *new_super_class) {
    // super class is assigned once, except during class linker initialization
    Class* old_super_class = GetFieldObject<Class*>(
        OFFSET_OF_OBJECT_MEMBER(Class, super_class_), false);
    DCHECK(old_super_class == NULL || old_super_class == new_super_class);
    DCHECK(new_super_class != NULL);
    SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Class, super_class_),
        new_super_class, false);
  }

  bool HasSuperClass() const {
    return GetSuperClass() != NULL;
  }

  uint32_t GetSuperClassTypeIdx() const {
    DCHECK(IsIdxLoaded());
    return GetField32(OFFSET_OF_OBJECT_MEMBER(Class, super_class_type_idx_),
                      false);
  }

  void SetSuperClassTypeIdx(int32_t new_super_class_idx) {
    SetField32(OFFSET_OF_OBJECT_MEMBER(Class, super_class_type_idx_),
               new_super_class_idx, false);
  }

  const ClassLoader* GetClassLoader() const;

  void SetClassLoader(const ClassLoader* new_cl);

  static MemberOffset DexCacheOffset() {
    return MemberOffset(OFFSETOF_MEMBER(Class, dex_cache_));
  }

  DexCache* GetDexCache() const;

  void SetDexCache(DexCache* new_dex_cache);

  ObjectArray<Method>* GetDirectMethods() const {
    DCHECK(IsLoaded());
    return GetFieldObject<ObjectArray<Method>*>(
        OFFSET_OF_OBJECT_MEMBER(Class, direct_methods_), false);
  }

  void SetDirectMethods(ObjectArray<Method>* new_direct_methods) {
    DCHECK(NULL == GetFieldObject<ObjectArray<Method>*>(
        OFFSET_OF_OBJECT_MEMBER(Class, direct_methods_), false));
    DCHECK_NE(0, new_direct_methods->GetLength());
    SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Class, direct_methods_),
                   new_direct_methods, false);
  }

  Method* GetDirectMethod(int32_t i) const {
    return GetDirectMethods()->Get(i);
  }

  void SetDirectMethod(uint32_t i, Method* f) {  // TODO: uint16_t
    ObjectArray<Method>* direct_methods =
        GetFieldObject<ObjectArray<Method>*>(
            OFFSET_OF_OBJECT_MEMBER(Class, direct_methods_), false);
    direct_methods->Set(i, f);
  }

  // Returns the number of static, private, and constructor methods.
  size_t NumDirectMethods() const {
    return (GetDirectMethods() != NULL) ? GetDirectMethods()->GetLength() : 0;
  }

  ObjectArray<Method>* GetVirtualMethods() const {
    DCHECK(IsLoaded());
    return GetFieldObject<ObjectArray<Method>*>(
        OFFSET_OF_OBJECT_MEMBER(Class, virtual_methods_), false);
  }

  void SetVirtualMethods(ObjectArray<Method>* new_virtual_methods) {
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

  Method* GetVirtualMethod(uint32_t i) const {
    DCHECK(IsLinked());
    return GetVirtualMethods()->Get(i);
  }

  Method* GetVirtualMethodDuringLinking(uint32_t i) const {
    DCHECK(IsLoaded());
    return GetVirtualMethods()->Get(i);
  }

  void SetVirtualMethod(uint32_t i, Method* f) {  // TODO: uint16_t
    ObjectArray<Method>* virtual_methods =
        GetFieldObject<ObjectArray<Method>*>(
            OFFSET_OF_OBJECT_MEMBER(Class, virtual_methods_), false);
    virtual_methods->Set(i, f);
  }

  ObjectArray<Method>* GetVTable() const {
    DCHECK(IsLinked());
    return GetFieldObject<ObjectArray<Method>*>(
        OFFSET_OF_OBJECT_MEMBER(Class, vtable_), false);
  }

  ObjectArray<Method>* GetVTableDuringLinking() const {
    DCHECK(IsLoaded());
    return GetFieldObject<ObjectArray<Method>*>(
        OFFSET_OF_OBJECT_MEMBER(Class, vtable_), false);
  }

  void SetVTable(ObjectArray<Method>* new_vtable) {
    SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Class, vtable_), new_vtable, false);
  }

  static MemberOffset VTableOffset() {
    return OFFSET_OF_OBJECT_MEMBER(Class, vtable_);
  }

  // Given a method implemented by this class but potentially from a
  // super class, return the specific implementation
  // method for this class.
  Method* FindVirtualMethodForVirtual(Method* method) {
    DCHECK(!method->GetDeclaringClass()->IsInterface());
    // The argument method may from a super class.
    // Use the index to a potentially overridden one for this instance's class.
    return GetVTable()->Get(method->GetMethodIndex());
  }

  // Given a method implemented by this class, but potentially from a
  // super class or interface, return the specific implementation
  // method for this class.
  Method* FindVirtualMethodForInterface(Method* method);

  Method* FindVirtualMethodForVirtualOrInterface(Method* method) {
    if (method->GetDeclaringClass()->IsInterface()) {
      return FindVirtualMethodForInterface(method);
    }
    return FindVirtualMethodForVirtual(method);
  }

  Method* FindDeclaredVirtualMethod(const StringPiece& name,
                                    const StringPiece& descriptor);

  Method* FindVirtualMethod(const StringPiece& name,
                            const StringPiece& descriptor);

  Method* FindDeclaredDirectMethod(const StringPiece& name,
                                   const StringPiece& signature);

  Method* FindDirectMethod(const StringPiece& name,
                           const StringPiece& signature);

  size_t NumInterfaces() const {
    CHECK(IsIdxLoaded()); // used during loading
    ObjectArray<Class>* interfaces = GetFieldObject<ObjectArray<Class>*>(
        OFFSET_OF_OBJECT_MEMBER(Class, interfaces_), false);
    return (interfaces != NULL) ? interfaces->GetLength() : 0;
  }

  IntArray* GetInterfacesTypeIdx() const {
    CHECK(IsIdxLoaded());
    return GetFieldObject<IntArray*>(
        OFFSET_OF_OBJECT_MEMBER(Class, interfaces_type_idx_), false);
  }

  void SetInterfacesTypeIdx(IntArray* new_interfaces_idx);

  ObjectArray<Class>* GetInterfaces() const {
    CHECK(IsLoaded());
    return GetFieldObject<ObjectArray<Class>*>(
        OFFSET_OF_OBJECT_MEMBER(Class, interfaces_), false);
  }

  void SetInterfaces(ObjectArray<Class>* new_interfaces) {
    DCHECK(NULL == GetFieldObject<Object*>(
        OFFSET_OF_OBJECT_MEMBER(Class, interfaces_), false));
    SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Class, interfaces_),
                   new_interfaces, false);
  }

  void SetInterface(uint32_t i, Class* f) {  // TODO: uint16_t
    DCHECK_NE(NumInterfaces(), 0U);
    ObjectArray<Class>* interfaces =
        GetFieldObject<ObjectArray<Class>*>(
            OFFSET_OF_OBJECT_MEMBER(Class, interfaces_), false);
    interfaces->Set(i, f);
  }

  Class* GetInterface(uint32_t i) const {
    DCHECK_NE(NumInterfaces(), 0U);
    return GetInterfaces()->Get(i);
  }

  size_t GetIFTableCount() const {
    DCHECK(IsLinked());
    DCHECK(sizeof(size_t) == sizeof(int32_t));
    return GetField32(OFFSET_OF_OBJECT_MEMBER(Class, iftable_count_), false);
  }

  void SetIFTableCount(size_t new_iftable_count) {
    DCHECK(sizeof(size_t) == sizeof(int32_t));
    SetField32(OFFSET_OF_OBJECT_MEMBER(Class, iftable_count_),
               new_iftable_count, false);
  }

  InterfaceEntry* GetIFTable() const {
    DCHECK(IsLinked());
    return GetFieldPtr<InterfaceEntry*>(
        OFFSET_OF_OBJECT_MEMBER(Class, iftable_), false);
  }

  void SetIFTable(InterfaceEntry* new_iftable) {
    SetFieldPtr<InterfaceEntry*>(OFFSET_OF_OBJECT_MEMBER(Class, iftable_),
                                 new_iftable, false);
  }

  size_t GetIfviPoolCount() const {
    DCHECK(IsLinked());
    CHECK(sizeof(size_t) == sizeof(int32_t));
    return GetField32(OFFSET_OF_OBJECT_MEMBER(Class, ifvi_pool_count_), false);
  }

  void SetIfviPoolCount(size_t new_count) {
    CHECK(sizeof(size_t) == sizeof(int32_t));
    SetField32(OFFSET_OF_OBJECT_MEMBER(Class, ifvi_pool_count_), new_count,
               false);
  }

  uint32_t* GetIfviPool() const {
    DCHECK(IsLinked());
    return GetFieldPtr<uint32_t*>(
        OFFSET_OF_OBJECT_MEMBER(Class, ifvi_pool_), false);
  }

  void SetIfviPool(uint32_t* new_pool) {
    SetFieldPtr(OFFSET_OF_OBJECT_MEMBER(Class, ifvi_pool_), new_pool, false);
  }

  // Get instance fields
  ObjectArray<Field>* GetIFields() const {
    DCHECK(IsLoaded());
    return GetFieldObject<ObjectArray<Field>*>(
        OFFSET_OF_OBJECT_MEMBER(Class, ifields_), false);
  }

  void SetIFields(ObjectArray<Field>* new_ifields) {
    DCHECK(NULL == GetFieldObject<ObjectArray<Field>*>(
        OFFSET_OF_OBJECT_MEMBER(Class, ifields_), false));
    SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Class, ifields_),
                   new_ifields, false);
  }

  size_t NumInstanceFields() const {
    return (GetIFields() != NULL) ? GetIFields()->GetLength() : 0;
  }

  Field* GetInstanceField(uint32_t i) const {  // TODO: uint16_t
    DCHECK_NE(NumInstanceFields(), 0U);
    return GetIFields()->Get(i);
  }

  void SetInstanceField(uint32_t i, Field* f) {  // TODO: uint16_t
    ObjectArray<Field>* ifields= GetFieldObject<ObjectArray<Field>*>(
        OFFSET_OF_OBJECT_MEMBER(Class, ifields_), false);
    ifields->Set(i, f);
  }

  // Returns the number of instance fields containing reference types.
  size_t NumReferenceInstanceFields() const {
    DCHECK(IsLinked());
    DCHECK(sizeof(size_t) == sizeof(int32_t));
    return GetField32(
        OFFSET_OF_OBJECT_MEMBER(Class, num_reference_instance_fields_), false);
  }

  size_t NumReferenceInstanceFieldsDuringLinking() const {
    DCHECK(IsLoaded());
    DCHECK(sizeof(size_t) == sizeof(int32_t));
    return GetField32(
        OFFSET_OF_OBJECT_MEMBER(Class, num_reference_instance_fields_), false);
  }

  void SetNumReferenceInstanceFields(size_t new_num) {
    DCHECK(sizeof(size_t) == sizeof(int32_t));
    SetField32(OFFSET_OF_OBJECT_MEMBER(Class, num_reference_instance_fields_),
               new_num, false);
  }

  uint32_t GetReferenceInstanceOffsets() const {
    DCHECK(IsLinked());
    return GetField32(
        OFFSET_OF_OBJECT_MEMBER(Class, reference_instance_offsets_), false);
  }

  void SetReferenceInstanceOffsets(uint32_t new_reference_offsets);

  // Beginning of static field data
  static MemberOffset FieldsOffset() {
    return OFFSET_OF_OBJECT_MEMBER(Class, fields_);
  }

  // Returns the number of static fields containing reference types.
  size_t NumReferenceStaticFields() const {
    DCHECK(IsLinked());
    DCHECK(sizeof(size_t) == sizeof(int32_t));
    return GetField32(
        OFFSET_OF_OBJECT_MEMBER(Class, num_reference_static_fields_), false);
  }

  size_t NumReferenceStaticFieldsDuringLinking() const {
    DCHECK(IsLoaded());
    DCHECK(sizeof(size_t) == sizeof(int32_t));
    return GetField32(
        OFFSET_OF_OBJECT_MEMBER(Class, num_reference_static_fields_), false);
  }

  void SetNumReferenceStaticFields(size_t new_num) {
    DCHECK(sizeof(size_t) == sizeof(int32_t));
    SetField32(OFFSET_OF_OBJECT_MEMBER(Class, num_reference_static_fields_),
               new_num, false);
  }

  ObjectArray<Field>* GetSFields() const {
    DCHECK(IsLoaded());
    return GetFieldObject<ObjectArray<Field>*>(
        OFFSET_OF_OBJECT_MEMBER(Class, sfields_), false);
  }

  void SetSFields(ObjectArray<Field>* new_sfields) {
    DCHECK(NULL == GetFieldObject<ObjectArray<Field>*>(
        OFFSET_OF_OBJECT_MEMBER(Class, sfields_), false));
    SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Class, sfields_),
                   new_sfields, false);
  }

  size_t NumStaticFields() const {
    return (GetSFields() != NULL) ? GetSFields()->GetLength() : 0;
  }

  Field* GetStaticField(uint32_t i) const {  // TODO: uint16_t
    return GetSFields()->Get(i);
  }

  void SetStaticField(uint32_t i, Field* f) {  // TODO: uint16_t
    ObjectArray<Field>* sfields= GetFieldObject<ObjectArray<Field>*>(
        OFFSET_OF_OBJECT_MEMBER(Class, sfields_), false);
    sfields->Set(i, f);
  }

  uint32_t GetReferenceStaticOffsets() const {
    return GetField32(
        OFFSET_OF_OBJECT_MEMBER(Class, reference_static_offsets_), false);
  }

  void SetReferenceStaticOffsets(uint32_t new_reference_offsets);

  // Finds the given instance field in this class or a superclass.
  Field* FindInstanceField(const StringPiece& name, Class* type);

  Field* FindDeclaredInstanceField(const StringPiece& name, Class* type);

  // Finds the given static field in this class or a superclass.
  Field* FindStaticField(const StringPiece& name, Class* type);

  Field* FindDeclaredStaticField(const StringPiece& name, Class* type);

  uint32_t GetClinitThreadId() const {
    DCHECK(IsIdxLoaded());
    return GetField32(OFFSET_OF_OBJECT_MEMBER(Class, clinit_thread_id_), false);
  }

  void SetClinitThreadId(uint32_t new_clinit_thread_id) {
    SetField32(OFFSET_OF_OBJECT_MEMBER(Class, clinit_thread_id_),
               new_clinit_thread_id, false);
  }

  Class* GetVerifyErrorClass() const {
    DCHECK(IsErroneous());
    return GetFieldObject<Class*>(
        OFFSET_OF_OBJECT_MEMBER(Class, verify_error_class_), false);
  }

  void SetVerifyErrorClass(Class* klass) {
    klass->SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Class, verify_error_class_),
                          klass, false);
  }

  const char* GetSourceFile() const {
    DCHECK(IsLoaded());
    return GetFieldPtr<const char*>(
        OFFSET_OF_OBJECT_MEMBER(Class, source_file_), false);
  }

  void SetSourceFile(const char* new_source_file) {
    SetFieldPtr<const char*>(OFFSET_OF_OBJECT_MEMBER(Class, source_file_),
                             new_source_file, false);
  }

 private:
  bool Implements(const Class* klass) const;
  bool IsArrayAssignableFromArray(const Class* klass) const;
  bool IsAssignableFromArray(const Class* klass) const;
  bool IsSubClass(const Class* klass) const;

  // TODO: the image writer should know the offsets of these fields as they
  // should appear in the libcore Java mirror
  friend class ImageWriter;

  // descriptor for the class such as "java.lang.Class" or "[C"
  String* name_;  // TODO initialize

  // descriptor for the class such as "Ljava/lang/Class;" or "[C"
  String* descriptor_;

  // access flags; low 16 bits are defined by VM spec
  uint32_t access_flags_;  // TODO: make an instance field?

  // DexCache of resolved constant pool entries
  // (will be NULL for VM-generated, e.g. arrays and primitive classes)
  DexCache* dex_cache_;

  // state of class initialization
  Status status_;

  // If class verify fails, we must return same error on subsequent tries.
  // Update with SetVerifyErrorClass to ensure a write barrier is used.
  const Class* verify_error_class_;

  // threadId, used to check for recursive <clinit> invocation
  uint32_t clinit_thread_id_;

  // Total object size; used when allocating storage on gc heap.  (For
  // interfaces and abstract classes this will be zero.)
  size_t object_size_;

  // For array classes, the class object for base element, for
  // instanceof/checkcast (for String[][][], this will be String).
  // Otherwise, NULL.
  Class* component_type_;  // TODO: make an instance field

  // For array classes, the number of array dimensions, e.g. int[][]
  // is 2.  Otherwise 0.
  int32_t array_rank_;

  // primitive type index, or PRIM_NOT (-1); set for generated prim classes
  PrimitiveType primitive_type_;

  // The superclass, or NULL if this is java.lang.Object or a
  // primitive type.
  Class* super_class_;  // TODO: make an instance field
  uint32_t super_class_type_idx_;

  // defining class loader, or NULL for the "bootstrap" system loader
  const ClassLoader* class_loader_;  // TODO: make an instance field

  // initiating class loader list
  // NOTE: for classes with low serialNumber, these are unused, and the
  // values are kept in a table in gDvm.
  // InitiatingLoaderList initiating_loader_list_;

  // array of interfaces this class implements directly
  ObjectArray<Class>* interfaces_;
  IntArray* interfaces_type_idx_;

  // static, private, and <init> methods
  ObjectArray<Method>* direct_methods_;

  // virtual methods defined in this class; invoked through vtable
  ObjectArray<Method>* virtual_methods_;

  // Virtual method table (vtable), for use by "invoke-virtual".  The
  // vtable from the superclass is copied in, and virtual methods from
  // our class either replace those from the super or are appended.
  ObjectArray<Method>* vtable_;

  // Interface table (iftable_), one entry per interface supported by
  // this class.  That means one entry for each interface we support
  // directly, indirectly via superclass, or indirectly via
  // superinterface.  This will be null if neither we nor our
  // superclass implement any interfaces.
  //
  // Why we need this: given "class Foo implements Face", declare
  // "Face faceObj = new Foo()".  Invoke faceObj.blah(), where "blah"
  // is part of the Face interface.  We can't easily use a single
  // vtable.
  //
  // For every interface a concrete class implements, we create a list
  // of virtualMethod indices for the methods in the interface.
  size_t iftable_count_;
  // TODO convert to ObjectArray<?>
  InterfaceEntry* iftable_;

  // The interface vtable indices for iftable get stored here.  By
  // placing them all in a single pool for each class that implements
  // interfaces, we decrease the number of allocations.
  size_t ifvi_pool_count_;
  // TODO convert to IntArray
  uint32_t* ifvi_pool_;

  // instance fields
  //
  // These describe the layout of the contents of a
  // DataObject-compatible Object.  Note that only the fields directly
  // declared by this class are listed in ifields; fields declared by
  // a superclass are listed in the superclass's Class.ifields.
  //
  // All instance fields that refer to objects are guaranteed to be at
  // the beginning of the field list.  num_reference_instance_fields_
  // specifies the number of reference fields.
  ObjectArray<Field>* ifields_;

  // number of instance fields that are object refs
  size_t num_reference_instance_fields_;

  // Bitmap of offsets of ifields.
  uint32_t reference_instance_offsets_;

  // source file name, if known.  Otherwise, NULL.
  const char* source_file_;

  // Static fields
  ObjectArray<Field>* sfields_;

  // number of static fields that are object refs
  size_t num_reference_static_fields_;

  // Bitmap of offsets of sfields.
  uint32_t reference_static_offsets_;

  // Total class size; used when allocating storage on gc heap.
  size_t class_size_;

  // Location of first static field.
  uint32_t fields_[0];

 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(Class);
};

std::ostream& operator<<(std::ostream& os, const Class::Status& rhs);

inline void Object::SetClass(Class* new_klass) {
  // new_klass may be NULL prior to class linker initialization
  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Object, klass_), new_klass, false);
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

inline bool Object::IsClassClass() const {
  Class* java_lang_Class = GetClass()->GetClass();
  return this == java_lang_Class;
}

inline bool Object::IsObjectArray() const {
  return IsArrayInstance() && !GetClass()->GetComponentType()->IsPrimitive();
}

inline bool Object::IsArrayInstance() const {
  return GetClass()->IsArrayClass();
}

inline bool Object::IsField() const {
  Class* java_lang_Class = klass_->klass_;
  Class* java_lang_reflect_Field =
      java_lang_Class->GetInstanceField(0)->GetClass();
  return GetClass() == java_lang_reflect_Field;
}

inline bool Object::IsMethod() const {
  Class* java_lang_Class = GetClass()->GetClass();
  Class* java_lang_reflect_Method =
      java_lang_Class->GetDirectMethod(0)->GetClass();
  return GetClass() == java_lang_reflect_Method;
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
  DCHECK(!IsMethod() || result == sizeof(Method));
  return result;
}

inline void Field::SetOffset(MemberOffset num_bytes) {
  DCHECK(GetDeclaringClass()->IsLoaded());
  DCHECK_LE(CLASS_SMALLEST_OFFSET, num_bytes.Uint32Value());
 Class* type = GetTypeDuringLinking();
  if (type != NULL && (type->IsPrimitiveDouble() || type->IsPrimitiveLong())) {
    DCHECK(IsAligned(num_bytes.Uint32Value(), 8));
  }
  SetField32(OFFSET_OF_OBJECT_MEMBER(Field, offset_),
             num_bytes.Uint32Value(), false);
}

inline Class* Field::GetDeclaringClass() const {
  Class* result = GetFieldObject<Class*>(
      OFFSET_OF_OBJECT_MEMBER(Field, declaring_class_), false);
  DCHECK(result != NULL);
  DCHECK(result->IsLoaded());
  return result;
}

inline void Field::SetDeclaringClass(Class *new_declaring_class) {
  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Field, declaring_class_),
                 new_declaring_class, false);
}

inline Class* Method::GetDeclaringClass() const {
  Class* result =
      GetFieldObject<Class*>(
          OFFSET_OF_OBJECT_MEMBER(Method, declaring_class_), false);
  DCHECK(result != NULL);
  DCHECK(result->IsLoaded());
  return result;
}

inline void Method::SetDeclaringClass(Class *new_declaring_class) {
  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Method, declaring_class_),
                 new_declaring_class, false);
}

inline uint32_t Method::GetReturnTypeIdx() const {
  DCHECK(GetDeclaringClass()->IsLinked());
  return GetField32(OFFSET_OF_OBJECT_MEMBER(Method, java_return_type_idx_),
                    false);
}

inline bool Method::IsReturnAReference() const {
  return !GetReturnType()->IsPrimitive();
}

inline bool Method::IsReturnAFloat() const {
  return GetReturnType()->IsPrimitiveFloat();
}

inline bool Method::IsReturnADouble() const {
  return GetReturnType()->IsPrimitiveDouble();
}

inline bool Method::IsReturnALong() const {
  return GetReturnType()->IsPrimitiveLong();
}

inline bool Method::IsReturnVoid() const {
  return GetReturnType()->IsPrimitiveVoid();
}

inline size_t Array::SizeOf() const {
  return SizeOf(GetLength(), GetClass()->GetComponentSize());
}

template<class T>
void ObjectArray<T>::Set(int32_t i, T* object) {
  if (IsValidIndex(i)) {
    if (object != NULL) {
      Class* element_class = GetClass()->GetComponentType();
      DCHECK(!element_class->IsPrimitive());
      // TODO: ArrayStoreException
      CHECK(object->InstanceOf(element_class));
    }
    MemberOffset data_offset(DataOffset().Int32Value() + i * sizeof(Object*));
    SetFieldObject(data_offset, object, false);
  }
}

template<class T>
void ObjectArray<T>::SetWithoutChecks(int32_t i, T* object) {
  DCHECK(IsValidIndex(i));
  MemberOffset data_offset(DataOffset().Int32Value() + i * sizeof(Object*));
  SetFieldObject(data_offset, object, false);
}

template<class T>
void ObjectArray<T>::Copy(const ObjectArray<T>* src, int src_pos,
                          ObjectArray<T>* dst, int dst_pos,
                          size_t length) {
  if (src->IsValidIndex(src_pos) &&
      src->IsValidIndex(src_pos+length-1) &&
      dst->IsValidIndex(dst_pos) &&
      dst->IsValidIndex(dst_pos+length-1)) {
    MemberOffset src_offset(DataOffset().Int32Value() +
                            src_pos * sizeof(Object*));
    MemberOffset dst_offset(DataOffset().Int32Value() +
                            dst_pos * sizeof(Object*));
    Class* array_class = dst->GetClass();
    if (array_class == src->GetClass()) {
      // No need for array store checks if arrays are of the same type
      for (size_t i=0; i < length; i++) {
        Object* object = src->GetFieldObject<Object*>(src_offset, false);
        dst->SetFieldObject(dst_offset, object, false);
        src_offset = MemberOffset(src_offset.Uint32Value() + sizeof(Object*));
        dst_offset = MemberOffset(dst_offset.Uint32Value() + sizeof(Object*));
      }
    } else {
      Class* element_class = array_class->GetComponentType();
      CHECK(!element_class->IsPrimitive());
      for (size_t i=0; i < length; i++) {
        Object* object = src->GetFieldObject<Object*>(src_offset, false);
        // TODO: ArrayStoreException
        CHECK(object == NULL || object->InstanceOf(element_class));
        dst->SetFieldObject(dst_offset, object, false);
        src_offset = MemberOffset(src_offset.Uint32Value() + sizeof(Object*));
        dst_offset = MemberOffset(dst_offset.Uint32Value() + sizeof(Object*));
      }
    }
    // TODO: bulk write barrier
  }
}

class ClassClass : public Class {
 private:
  // Padding to ensure the 64-bit serialVersionUID_ begins on a 8-byte boundary
  int32_t padding_;
  int64_t serialVersionUID_;
  DISALLOW_IMPLICIT_CONSTRUCTORS(ClassClass);
};

class StringClass : public Class {
 private:
  CharArray* ASCII_;
  Object* CASE_INSENSITIVE_ORDER_;
  uint32_t REPLACEMENT_CHAR_;
  int64_t serialVersionUID;
  DISALLOW_IMPLICIT_CONSTRUCTORS(StringClass);
};

class FieldClass : public Class {
 private:
  Object* ORDER_BY_NAME_AND_DECLARING_CLASS_;
  uint32_t TYPE_BOOLEAN_;
  uint32_t TYPE_BYTE_;
  uint32_t TYPE_CHAR_;
  uint32_t TYPE_DOUBLE_;
  uint32_t TYPE_FLOAT_;
  uint32_t TYPE_INTEGER_;
  uint32_t TYPE_LONG_;
  uint32_t TYPE_SHORT_;
  DISALLOW_IMPLICIT_CONSTRUCTORS(FieldClass);
};

class MethodClass : public Class {
 private:
  int32_t DECLARED_;
  int32_t PUBLIC_;
  DISALLOW_IMPLICIT_CONSTRUCTORS(MethodClass);
};

class DataObject : public Object {
 public:
  // Location of first instance field.
  uint32_t fields_[0];
 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(DataObject);
};

template<class T>
class PrimitiveArray : public Array {
 public:
  typedef T ElementType;

  static PrimitiveArray<T>* Alloc(size_t length);

  const T* GetData() const {
    return reinterpret_cast<const T*>(&elements_);
  }

  T* GetData() {
    return reinterpret_cast<T*>(&elements_);
  }

  T Get(int32_t i) const {
    if (!IsValidIndex(i)) {
      return T(0);
    }
    return GetData()[i];
  }

  void Set(int32_t i, T value) {
    // TODO: ArrayStoreException
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
  // Location of first element.
  T elements_[0];

  static Class* array_class_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(PrimitiveArray);
};

inline void Class::SetInterfacesTypeIdx(IntArray* new_interfaces_idx) {
  DCHECK(NULL == GetFieldObject<IntArray*>(
      OFFSET_OF_OBJECT_MEMBER(Class, interfaces_type_idx_), false));
  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Class, interfaces_type_idx_),
                 new_interfaces_idx, false);
}

// C++ mirror of java.lang.String
class String : public Object {
 public:
  const CharArray* GetCharArray() const {
    const CharArray* result = GetFieldObject<const CharArray*>(
        OFFSET_OF_OBJECT_MEMBER(String, array_), false);
    DCHECK(result != NULL);
    return result;
  }

  int32_t GetOffset() const {
    int32_t result = GetField32(
        OFFSET_OF_OBJECT_MEMBER(String, offset_), false);
    DCHECK_LE(0, result);
    return result;
  }

  int32_t GetLength() const;

  int32_t GetHashCode() const;

  void ComputeHashCode() {
    SetHashCode(ComputeUtf16Hash(GetCharArray(), GetOffset(), GetLength()));
  }

  int32_t GetUtfLength() const {
    return CountUtf8Bytes(GetCharArray()->GetData(), GetLength());
  }

  uint16_t CharAt(int32_t index) const;

  const String* Intern() const;

  static String* AllocFromUtf16(int32_t utf16_length,
                                const uint16_t* utf16_data_in,
                                int32_t hash_code = 0);

  static String* AllocFromModifiedUtf8(const char* utf);

  static String* AllocFromModifiedUtf8(int32_t utf16_length,
                                       const char* utf8_data_in);

  static String* Alloc(Class* java_lang_String, int32_t utf16_length);

  static String* Alloc(Class* java_lang_String, CharArray* array);

  bool Equals(const char* modified_utf8) const;

  // TODO: do we need this overload? give it a more intention-revealing name.
  bool Equals(const StringPiece& modified_utf8) const;

  bool Equals(const String* that) const;

  // TODO: do we need this overload? give it a more intention-revealing name.
  bool Equals(const uint16_t* that_chars, int32_t that_offset,
              int32_t that_length) const;

  // Create a modified UTF-8 encoded std::string from a java/lang/String object.
  std::string ToModifiedUtf8() const;

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

  void SetArray(CharArray* new_array) {
    DCHECK(new_array != NULL);
    SetFieldObject(OFFSET_OF_OBJECT_MEMBER(String, array_), new_array, false);
  }

  // Field order required by test "ValidateFieldOrderOfJavaCppUnionClasses".
  CharArray* array_;

  uint32_t hash_code_;

  int32_t offset_;

  int32_t count_;

  static Class* java_lang_String_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(String);
};

inline const String* Field::GetName() const {
   DCHECK(GetDeclaringClass()->IsLoaded());
   String* result =
       GetFieldObject<String*>(OFFSET_OF_OBJECT_MEMBER(Field, name_), false);
   DCHECK(result != NULL);
   return result;
}

inline void Field::SetName(String* new_name) {
  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Field, name_),
                 new_name, false);

}

inline uint32_t Field::GetAccessFlags() const {
  DCHECK(GetDeclaringClass()->IsLoaded());
  return GetField32(OFFSET_OF_OBJECT_MEMBER(Field, access_flags_), false);
}

inline uint32_t Field::GetTypeIdx() const {
  DCHECK(GetDeclaringClass()->IsIdxLoaded());
  return GetField32(OFFSET_OF_OBJECT_MEMBER(Field, type_idx_), false);
}

inline MemberOffset Field::GetOffset() const {
  DCHECK(GetDeclaringClass()->IsLinked());
  return MemberOffset(
      GetField32(OFFSET_OF_OBJECT_MEMBER(Field, offset_), false));
}

inline MemberOffset Field::GetOffsetDuringLinking() const {
  DCHECK(GetDeclaringClass()->IsLoaded());
  return MemberOffset(
      GetField32(OFFSET_OF_OBJECT_MEMBER(Field, offset_), false));
}

inline const String* Method::GetName() const {
  DCHECK(GetDeclaringClass()->IsLoaded());
  const String* result =
      GetFieldObject<const String*>(
          OFFSET_OF_OBJECT_MEMBER(Method, name_), false);
  DCHECK(result != NULL);
  return result;
}

inline void Method::SetName(String* new_name) {
  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Method, name_),
                 new_name, false);

}

inline const char* Method::GetShorty() const {
  DCHECK(GetDeclaringClass()->IsLoaded());
  return GetFieldPtr<const char*>(
      OFFSET_OF_OBJECT_MEMBER(Method, shorty_), false);
}

inline const String* Method::GetSignature() const {
  DCHECK(GetDeclaringClass()->IsLoaded());
  const String* result =
      GetFieldObject<const String*>(
          OFFSET_OF_OBJECT_MEMBER(Method, signature_), false);
  DCHECK(result != NULL);
  return result;
}

inline void Method::SetSignature(String* new_signature) {
  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Method, signature_),
                 new_signature, false);
}

inline uint32_t Class::GetAccessFlags() const {
  // Check class is loaded or this is java.lang.String that has a
  // circularity issue during loading the names of its members
  DCHECK(IsLoaded() || this == String::GetJavaLangString() ||
         this == Field::GetJavaLangReflectField() ||
         this == Method::GetJavaLangReflectMethod());
  return GetField32(OFFSET_OF_OBJECT_MEMBER(Class, access_flags_), false);
}

inline void Class::SetDescriptor(String* new_descriptor) {
  DCHECK(new_descriptor != NULL);
  DCHECK_NE(0, new_descriptor->GetLength());
  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Class, descriptor_),
                 new_descriptor, false);
}

inline uint32_t Method::GetAccessFlags() const {
  DCHECK(GetDeclaringClass()->IsLoaded());
  return GetField32(OFFSET_OF_OBJECT_MEMBER(Method, access_flags_), false);
}

inline uint16_t Method::GetMethodIndex() const {
  DCHECK(GetDeclaringClass()->IsLinked());
  return GetField16(OFFSET_OF_OBJECT_MEMBER(Method, method_index_), false);
}

inline uint16_t Method::NumRegisters() const {
  DCHECK(GetDeclaringClass()->IsLoaded());
  return GetField16(OFFSET_OF_OBJECT_MEMBER(Method, num_registers_), false);
}

inline uint16_t Method::NumIns() const {
  DCHECK(GetDeclaringClass()->IsLoaded());
  return GetField16(OFFSET_OF_OBJECT_MEMBER(Method, num_ins_), false);
}

inline uint16_t Method::NumOuts() const {
  DCHECK(GetDeclaringClass()->IsLoaded());
  return GetField16(OFFSET_OF_OBJECT_MEMBER(Method, num_outs_), false);
}

inline uint32_t Method::GetProtoIdx() const {
  DCHECK(GetDeclaringClass()->IsLoaded());
  return GetField32(OFFSET_OF_OBJECT_MEMBER(Method, proto_idx_), false);
}

// C++ mirror of java.lang.Throwable
class Throwable : public Object {
 private:
  // Field order required by test "ValidateFieldOrderOfJavaCppUnionClasses".
  Throwable* cause_;
  String* detail_message_;
  Object* stack_state_; // Note this is Java volatile:
  Object* stack_trace_;
  Object* suppressed_exceptions_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(Throwable);
};

// C++ mirror of java.lang.StackTraceElement
class StackTraceElement : public Object {
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

  static StackTraceElement* Alloc(const String* declaring_class,
                                  const String* method_name,
                                  const String* file_name,
                                  int32_t line_number);

  static void SetClass(Class* java_lang_StackTraceElement);

  static void ResetClass();

 private:
  // Field order required by test "ValidateFieldOrderOfJavaCppUnionClasses".
  const String* declaring_class_;
  const String* file_name_;
  const String* method_name_;
  int32_t line_number_;

  static Class* GetStackTraceElement() {
    DCHECK(java_lang_StackTraceElement_ != NULL);
    return java_lang_StackTraceElement_;
  }

  static Class* java_lang_StackTraceElement_;
  DISALLOW_IMPLICIT_CONSTRUCTORS(StackTraceElement);
};

class InterfaceEntry {
 public:
  InterfaceEntry() : interface_(NULL), method_index_array_(NULL) {
  }

  Class* GetInterface() const {
    DCHECK(interface_ != NULL);
    return interface_;
  }

  void SetInterface(Class* interface) {
    DCHECK(interface->IsInterface());
    interface_ = interface;
  }

  uint32_t* GetMethodIndexArray() const {
    return method_index_array_;
  }

  void SetMethodIndexArray(uint32_t* new_mia) {
    method_index_array_ = new_mia;
  }

 private:
  // Points to the interface class.
  Class* interface_;

  // Index into array of vtable offsets.  This points into the
  // ifvi_pool_, which holds the vtables for all interfaces declared by
  // this class.
  uint32_t* method_index_array_;

  DISALLOW_COPY_AND_ASSIGN(InterfaceEntry);
};

}  // namespace art

#endif  // ART_SRC_OBJECT_H_
