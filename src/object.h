// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_OBJECT_H_
#define ART_SRC_OBJECT_H_

#include <vector>

#include "UniquePtr.h"
#include "casts.h"
#include "constants.h"
#include "globals.h"
#include "logging.h"
#include "macros.h"
#include "monitor.h"
#include "monitor.h"
#include "offsets.h"
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
   ((static_cast<int>(rshift) * CLASS_OFFSET_ALIGNMENT) + CLASS_SMALLEST_OFFSET)


class Object {
 public:
  static bool InstanceOf(const Object* object, const Class* klass) {
    if (object == NULL) {
      return false;
    }
    return object->InstanceOf(klass);
  }

  Class* GetClass() const {
    DCHECK(klass_ != NULL);
    return klass_;
  }

  bool InstanceOf(const Class* klass) const;

  size_t SizeOf() const;

  Object* Clone() {
    UNIMPLEMENTED(FATAL);
    return NULL;
  }

  void MonitorEnter() {
    monitor_->Enter();
  }

  void MonitorExit() {
    monitor_->Exit();
  }

  void Notify() {
    monitor_->Notify();
  }

  void NotifyAll() {
    monitor_->NotifyAll();
  }

  void Wait() {
    monitor_->Wait();
  }

  void Wait(int64_t timeout) {
    monitor_->Wait(timeout);
  }

  void Wait(int64_t timeout, int32_t nanos) {
    monitor_->Wait(timeout, nanos);
  }

  Object* GetFieldObject(size_t field_offset) const {
    const byte* raw_addr = reinterpret_cast<const byte*>(this) + field_offset;
    return *reinterpret_cast<Object* const *>(raw_addr);
  }

  void SetFieldObject(size_t offset, const Object* new_value) {
    byte* raw_addr = reinterpret_cast<byte*>(this) + offset;
    *reinterpret_cast<const Object**>(raw_addr) = new_value;
    // TODO: write barrier
  }

  uint32_t GetField32(size_t field_offset) const {
    const byte* raw_addr = reinterpret_cast<const byte*>(this) + field_offset;
    return *reinterpret_cast<const uint32_t*>(raw_addr);
  }

  void SetField32(size_t offset, uint32_t new_value) {
    byte* raw_addr = reinterpret_cast<byte*>(this) + offset;
    *reinterpret_cast<uint32_t*>(raw_addr) = new_value;
  }

  uint64_t GetField64(size_t field_offset) const {
    const byte* raw_addr = reinterpret_cast<const byte*>(this) + field_offset;
    return *reinterpret_cast<const uint64_t*>(raw_addr);
  }

  void SetField64(size_t offset, uint64_t new_value) {
    byte* raw_addr = reinterpret_cast<byte*>(this) + offset;
    *reinterpret_cast<uint64_t*>(raw_addr) = new_value;
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

 public:
  Class* klass_;

  Monitor* monitor_;

 private:
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

class AccessibleObject : public Object {
 private:
  // Field order required by test "ValidateFieldOrderOfJavaCppUnionClasses".
  uint32_t java_flag_;
};

class Field : public AccessibleObject {
 public:
  Class* GetDeclaringClass() const {
    DCHECK(declaring_class_ != NULL);
    return declaring_class_;
  }

  const String* GetName() const {
    DCHECK(name_ != NULL);
    return name_;
  }

  bool IsStatic() const {
    return (access_flags_ & kAccStatic) != 0;
  }

  char GetType() const {  // TODO: return type
    return GetDescriptor()[0];
  }

  const StringPiece& GetDescriptor() const {
    DCHECK_NE(0, descriptor_.size());
    return descriptor_;
  }

  uint32_t GetOffset() const {
    return offset_;
  }

  void SetOffset(size_t num_bytes) {
    offset_ = num_bytes;
  }

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

  // slow path routines for static field access when field was unresolved at compile time
  static uint32_t Get32StaticFromCode(uint32_t field_idx, const Method* referrer);
  static void Set32StaticFromCode(uint32_t field_idx, const Method* referrer, uint32_t new_value);
  static uint64_t Get64StaticFromCode(uint32_t field_idx, const Method* referrer);
  static void Set64StaticFromCode(uint32_t field_idx, const Method* referrer, uint64_t new_value);
  static Object* GetObjStaticFromCode(uint32_t field_idx, const Method* referrer);
  static void SetObjStaticFromCode(uint32_t field_idx, const Method* referrer, Object* new_value);

 public:  // TODO: private

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
  uint32_t offset_;
  Class* type_;

  // e.g. "I", "[C", "Landroid/os/Debug;"
  StringPiece descriptor_;

  uint32_t access_flags_;

 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(Field);
};

class Method : public AccessibleObject {
 public:
  // An function that invokes a method with an array of its arguments.
  typedef void InvokeStub(Method* method,
                          Object* obj,
                          Thread* thread,
                          byte* args,
                          JValue* result);

  // Returns the method name, e.g. "<init>" or "eatLunch"
  const String* GetName() const {
    DCHECK(name_ != NULL);
    return name_;
  }

  const String* GetSignature() const {
    DCHECK(signature_ != NULL);
    return signature_;
  }

  Class* GetDeclaringClass() const {
    DCHECK(declaring_class_ != NULL);
    return declaring_class_;
  }

  static MemberOffset DeclaringClassOffset() {
    return MemberOffset(OFFSETOF_MEMBER(Method, declaring_class_));
  }

  // Returns true if the method is declared public.
  bool IsPublic() const {
    return (access_flags_ & kAccPublic) != 0;
  }

  // Returns true if the method is declared private.
  bool IsPrivate() const {
    return (access_flags_ & kAccPrivate) != 0;
  }

  // Returns true if the method is declared static.
  bool IsStatic() const {
    return (access_flags_ & kAccStatic) != 0;
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
    return (access_flags_ & synchonized) != 0;
  }

  // Returns true if the method is declared final.
  bool IsFinal() const {
    return (access_flags_ & kAccFinal) != 0;
  }

  // Returns true if the method is declared native.
  bool IsNative() const {
    return (access_flags_ & kAccNative) != 0;
  }

  // Returns true if the method is declared abstract.
  bool IsAbstract() const {
    return (access_flags_ & kAccAbstract) != 0;
  }

  bool IsSynthetic() const {
    return (access_flags_ & kAccSynthetic) != 0;
  }

  // Number of argument registers required by the prototype.
  uint32_t NumArgRegisters() const;

  // Number of argument bytes required for densely packing the
  // arguments into an array of arguments.
  size_t NumArgArrayBytes() const;

  // Converts a native PC to a virtual PC.  TODO: this is a no-op
  // until we associate a PC mapping table with each method.
  uintptr_t ToDexPC(const uintptr_t pc) const {
    return pc;
  }

  // Converts a virtual PC to a native PC.  TODO: this is a no-op
  // until we associate a PC mapping table with each method.
  uintptr_t ToNativePC(const uintptr_t pc) const {
    return pc;
  }

 public:  // TODO: private
  // Field order required by test "ValidateFieldOrderOfJavaCppUnionClasses".
  // the class we are a part of
  Class* declaring_class_;
  ObjectArray<Class>* java_exception_types_;
  Object* java_formal_type_parameters_;
  Object* java_generic_exception_types_;
  Object* java_generic_parameter_types_;
  Object* java_generic_return_type_;
  Class* java_return_type_;
  const String* name_;
  ObjectArray<Class>* java_parameter_types_;
  uint32_t java_generic_types_are_initialized_;
  uint32_t java_slot_;

  const StringPiece& GetShorty() const {
    return shorty_;
  }

  bool IsReturnAReference() const {
    return (shorty_[0] == 'L') || (shorty_[0] == '[');
  }

  bool IsReturnAFloatOrDouble() const {
    return (shorty_[0] == 'F') || (shorty_[0] == 'D');
  }

  bool IsReturnAFloat() const {
    return shorty_[0] == 'F';
  }

  bool IsReturnADouble() const {
    return shorty_[0] == 'D';
  }

  bool IsReturnALong() const {
    return shorty_[0] == 'J';
  }

  bool IsReturnVoid() const {
    return shorty_[0] == 'V';
  }

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
    return (IsStatic() ? 0 : 1) + shorty_.length() - 1;
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

  bool HasCode() {
    return code_ != NULL;
  }

  const void* GetCode() {
    return code_;
  }

  void SetCode(const byte* compiled_code,
               size_t byte_count,
               InstructionSet set) {
    // Copy the code into an executable region.
    code_instruction_set_ = set;
    code_area_.reset(MemMap::Map(byte_count,
        PROT_READ | PROT_WRITE | PROT_EXEC));
    CHECK(code_area_.get());
    byte* code = code_area_->GetAddress();
    memcpy(code, compiled_code, byte_count);
    __builtin___clear_cache(code, code + byte_count);

    uintptr_t address = reinterpret_cast<uintptr_t>(code);
    if (code_instruction_set_ == kThumb2) {
        // Set the low-order bit so a BLX will switch to Thumb mode
        address |= 0x1;
    }
    code_ =  reinterpret_cast<void*>(address);
  }

  void SetFrameSizeInBytes(size_t frame_size_in_bytes) {
    frame_size_in_bytes_ = frame_size_in_bytes;
  }

  void SetReturnPcOffsetInBytes(size_t return_pc_offset_in_bytes) {
    return_pc_offset_in_bytes_ = return_pc_offset_in_bytes;
  }

  size_t GetFrameSizeInBytes() const {
    return frame_size_in_bytes_;
  }

  size_t GetReturnPcOffsetInBytes() const {
    return return_pc_offset_in_bytes_;
  }

  void SetCoreSpillMask(uint32_t core_spill_mask) {
    core_spill_mask_ = core_spill_mask;
  }

  static size_t GetCodeOffset() {
    return OFFSETOF_MEMBER(Method, code_);
  }

  void SetFpSpillMask(uint32_t fp_spill_mask) {
    fp_spill_mask_ = fp_spill_mask;
  }

  void RegisterNative(const void* native_method) {
    CHECK(native_method != NULL);
    native_method_ = native_method;
  }

  void UnregisterNative() {
    native_method_ = NULL;
  }

  static MemberOffset NativeMethodOffset() {
    return MemberOffset(OFFSETOF_MEMBER(Method, native_method_));
  }

  InvokeStub* GetInvokeStub() const {
    return invoke_stub_;
  }

  void SetInvokeStub(const InvokeStub* invoke_stub) {
    invoke_stub_ = invoke_stub;
  }

  static size_t GetInvokeStubOffset() {
    return OFFSETOF_MEMBER(Method, invoke_stub_);
  }

  bool HasSameNameAndDescriptor(const Method* that) const;

 public:  // TODO: private/const
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
  uint32_t code_off_;

  // The short-form method descriptor string.
  StringPiece shorty_;

  // short cuts to declaring_class_->dex_cache_ members for fast compiled code
  // access
  ObjectArray<const String>* dex_cache_strings_;
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
    return length_;
  }

  void SetLength(uint32_t length) {
    length_ = length;
  }

  static MemberOffset LengthOffset() {
    return MemberOffset(OFFSETOF_MEMBER(Array, length_));
  }

  static MemberOffset DataOffset() {
    return MemberOffset(OFFSETOF_MEMBER(Array, first_element_));
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
  static ObjectArray<T>* Alloc(Class* object_array_class,
                               int32_t length) {
    return Array::Alloc(object_array_class, length, sizeof(uint32_t))->AsObjectArray<T>();
  }

  T* const * GetData() const {
    return reinterpret_cast<T* const *>(&elements_);
  }

  T** GetData() {
    return reinterpret_cast<T**>(&elements_);
  }

  T* Get(int32_t i) const {
    if (!IsValidIndex(i)) {
      return NULL;
    }
    return GetData()[i];
  }

  void Set(int32_t i, T* object) {
    if (IsValidIndex(i)) {
      // TODO: ArrayStoreException
      GetData()[i] = object;  // TODO: write barrier
    }
  }

  static void Copy(ObjectArray<T>* src, int src_pos,
                   ObjectArray<T>* dst, int dst_pos,
                   size_t length) {
    for (size_t i = 0; i < length; i++) {
      dst->Set(dst_pos + i, src->Get(src_pos + i));
    }
  }

  ObjectArray<T>* CopyOf(int32_t new_length) {
    ObjectArray<T>* new_array = Alloc(klass_, new_length);
    Copy(this, 0, new_array, 0, std::min(GetLength(), new_length));
    return new_array;
  }

 private:
  // Location of first element.
  T* elements_[0];

  DISALLOW_IMPLICIT_CONSTRUCTORS(ObjectArray);
};

// Type for the InitializedStaticStorage table. Currently the Class
// provides the static storage. However, this might change to improve
// image sharing, so we use this type to avoid assumptions on the
// current storage.
class StaticStorageBase {};

// Class objects.
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
  // will use LinkClass to link all members, creating Field and Method
  // objects, setting up the vtable, etc. On success, the class is
  // marked kStatusResolved.

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
    kPrimNot = -1
  };

  // Given the context of a calling Method, use its DexCache to
  // resolve a type to a Class. If it cannot be resolved, throw an
  // error. If it can, use it to create an instance.
  static Object* AllocObjectFromCode(uint32_t type_idx, Method* method);

  // Creates a raw object instance but does not invoke the default constructor.
  Object* AllocObject();

  Class* GetSuperClass() const {
    return super_class_;
  }

  uint32_t GetSuperClassTypeIdx() const {
    return super_class_type_idx_;
  }

  bool HasSuperClass() const {
    return super_class_ != NULL;
  }

  bool IsAssignableFrom(const Class* klass) const {
    DCHECK(klass != NULL);
    if (this == klass) {
      return true;
    }
    if (IsInterface()) {
      return klass->Implements(this);
    }
    if (klass->IsArrayClass()) {
      return IsAssignableFromArray(klass);
    }
    return klass->IsSubClass(this);
  }

  const ClassLoader* GetClassLoader() const {
    return class_loader_;
  }

  DexCache* GetDexCache() const {
    return dex_cache_;
  }

  Class* GetComponentType() const {
    return component_type_;
  }

  static size_t GetTypeSize(String* descriptor);

  size_t GetComponentSize() const {
    return GetTypeSize(component_type_->descriptor_);
  }

  const String* GetDescriptor() const {
    DCHECK(descriptor_ != NULL);
    // DCHECK_NE(0, descriptor_->GetLength());  // TODO: keep?
    return descriptor_;
  }

  size_t SizeOf() const {
    return class_size_;
  }

  Status GetStatus() const {
    return status_;
  }

  void SetStatus(Status new_status) {
    // TODO: validate transition
    status_ = new_status;
  }

  // Returns true if the class has failed to link.
  bool IsErroneous() const {
    return GetStatus() == kStatusError;
  }

  // Returns true if the class has been verified.
  bool IsVerified() const {
    return GetStatus() >= kStatusVerified;
  }

  // Returns true if the class has been linked.
  bool IsLinked() const {
    return GetStatus() >= kStatusResolved;
  }

  // Returns true if the class has been loaded.
  bool IsLoaded() const {
    return GetStatus() >= kStatusLoaded;
  }

  // Returns true if the class is initialized.
  bool IsInitialized() const {
    return GetStatus() == kStatusInitialized;
  }

  // Returns true if this class is in the same packages as that class.
  bool IsInSamePackage(const Class* that) const;

  static bool IsInSamePackage(const String* descriptor1,
                              const String* descriptor2);

  // Returns true if this class represents an array class.
  bool IsArrayClass() const;

  // Returns true if the class is an interface.
  bool IsInterface() const {
    return (access_flags_ & kAccInterface) != 0;
  }

  // Returns true if the class is declared public.
  bool IsPublic() const {
    return (access_flags_ & kAccPublic) != 0;
  }

  // Returns true if the class is declared final.
  bool IsFinal() const {
    return (access_flags_ & kAccFinal) != 0;
  }

  // Returns true if the class is abstract.
  bool IsAbstract() const {
    return (access_flags_ & kAccAbstract) != 0;
  }

  // Returns true if the class is an annotation.
  bool IsAnnotation() const {
    return (access_flags_ & kAccAnnotation) != 0;
  }

  // Returns true if the class is a primitive type.
  bool IsPrimitive() const {
    return primitive_type_ != kPrimNot;
  }

  // Returns true if the class is synthetic.
  bool IsSynthetic() const {
    return (access_flags_ & kAccSynthetic) != 0;
  }

  bool IsReference() const {
    return (access_flags_ & kAccClassIsReference) != 0;
  }

  bool IsWeakReference() const {
    return (access_flags_ & kAccClassIsWeakReference) != 0;
  }

  bool IsSoftReference() const {
    return (access_flags_ & ~kAccReferenceFlagsMask) == kAccClassIsReference;
  }

  bool IsFinalizerReference() const {
    return (access_flags_ & kAccClassIsFinalizerReference) != 0;
  }

  bool IsPhantomReference() const {
    return (access_flags_ & kAccClassIsPhantomReference) != 0;
  }

  // Returns true if this class can access that class.
  bool CanAccess(const Class* that) const {
    return that->IsPublic() || this->IsInSamePackage(that);
  }

  static bool CanPutArrayElementNoThrow(const Class* elementClass, const Class* arrayClass);

  // Returns the number of static, private, and constructor methods.
  size_t NumDirectMethods() const {
    return (direct_methods_ != NULL) ? direct_methods_->GetLength() : 0;
  }

  Method* GetDirectMethod(int32_t i) const {
    DCHECK_NE(NumDirectMethods(), 0U);
    return direct_methods_->Get(i);
  }

  void SetDirectMethod(uint32_t i, Method* f) {  // TODO: uint16_t
    DCHECK_NE(NumDirectMethods(), 0U);
    direct_methods_->Set(i, f);
  }

  Method* FindDeclaredDirectMethod(const StringPiece& name,
                                   const StringPiece& signature);

  Method* FindDirectMethod(const StringPiece& name,
                           const StringPiece& signature);

  // Returns the number of non-inherited virtual methods.
  size_t NumVirtualMethods() const {
    return (virtual_methods_ != NULL) ? virtual_methods_->GetLength() : 0;
  }

  Method* GetVirtualMethod(uint32_t i) const {
    DCHECK_NE(NumVirtualMethods(), 0U);
    return virtual_methods_->Get(i);
  }

  void SetVirtualMethod(uint32_t i, Method* f) {  // TODO: uint16_t
    DCHECK_NE(NumVirtualMethods(), 0U);
    virtual_methods_->Set(i, f);
  }

  // Given a method implemented by this class but potentially from a
  // super class, return the specific implementation
  // method for this class.
  Method* FindVirtualMethodForVirtual(Method* method) {
    DCHECK(!method->GetDeclaringClass()->IsInterface());
    // The argument method may from a super class.
    // Use the index to a potentially overriden one for this instance's class.
    return vtable_->Get(method->method_index_);
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

  size_t NumInstanceFields() const {
    return (ifields_ != NULL) ? ifields_->GetLength() : 0;
  }

  // Returns the number of instance fields containing reference types.
  size_t NumReferenceInstanceFields() const {
    return num_reference_instance_fields_;
  }

  // Returns the number of static fields containing reference types.
  size_t NumReferenceStaticFields() const {
    return num_reference_static_fields_;
  }

  // Finds the given instance field in this class or a superclass.
  Field* FindInstanceField(const StringPiece& name,
      const StringPiece& descriptor);

  // Finds the given instance field in this class.
  Field* FindDeclaredInstanceField(const StringPiece& name,
      const StringPiece& descriptor);

  // Finds the given static field in this class or a superclass.
  Field* FindStaticField(const StringPiece& name,
      const StringPiece& descriptor);

  // Finds the given static field in this class.
  Field* FindDeclaredStaticField(const StringPiece& name,
      const StringPiece& descriptor);

  Field* GetInstanceField(uint32_t i) const {  // TODO: uint16_t
    DCHECK_NE(NumInstanceFields(), 0U);
    return ifields_->Get(i);
  }

  void SetInstanceField(uint32_t i, Field* f) {  // TODO: uint16_t
    DCHECK_NE(NumInstanceFields(), 0U);
    ifields_->Set(i, f);
  }

  size_t NumStaticFields() const {
    return (sfields_ != NULL) ? sfields_->GetLength() : 0;
  }

  Field* GetStaticField(uint32_t i) const {  // TODO: uint16_t
    DCHECK_NE(NumStaticFields(), 0U);
    return sfields_->Get(i);
  }

  void SetStaticField(uint32_t i, Field* f) {  // TODO: uint16_t
    DCHECK_NE(NumStaticFields(), 0U);
    sfields_->Set(i, f);
  }

  uint32_t GetReferenceInstanceOffsets() const {
    return reference_instance_offsets_;
  }

  void SetReferenceInstanceOffsets(uint32_t new_reference_offsets) {
    reference_instance_offsets_ = new_reference_offsets;
  }

  uint32_t GetReferenceStaticOffsets() const {
    return reference_static_offsets_;
  }

  void SetReferenceStaticOffsets(uint32_t new_reference_offsets) {
    reference_static_offsets_ = new_reference_offsets;
  }

  size_t NumInterfaces() const {
    return (interfaces_ != NULL) ? interfaces_->GetLength() : 0;
  }

  Class* GetInterface(uint32_t i) const {
    DCHECK_NE(NumInterfaces(), 0U);
    return interfaces_->Get(i);
  }

  void SetInterface(uint32_t i, Class* f) {  // TODO: uint16_t
    DCHECK_NE(NumInterfaces(), 0U);
    interfaces_->Set(i, f);
  }

  void SetVerifyErrorClass(Class* klass) {
    // Note SetFieldObject is used rather than verify_error_class_ directly for the barrier
    size_t field_offset = OFFSETOF_MEMBER(Class, verify_error_class_);
    klass->SetFieldObject(field_offset, klass);
  }

 private:
  bool Implements(const Class* klass) const;
  bool IsArrayAssignableFromArray(const Class* klass) const;
  bool IsAssignableFromArray(const Class* klass) const;
  bool IsSubClass(const Class* klass) const;

 public:  // TODO: private
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

inline bool Object::InstanceOf(const Class* klass) const {
  DCHECK(klass != NULL);
  DCHECK(klass_ != NULL);
  return klass->IsAssignableFrom(klass_);
}

inline bool Object::IsClass() const {
  Class* java_lang_Class = klass_->klass_;
  return klass_ == java_lang_Class;
}

inline bool Object::IsClassClass() const {
  Class* java_lang_Class = klass_->klass_;
  return this == java_lang_Class;
}

inline bool Object::IsObjectArray() const {
  return IsArrayInstance() && !klass_->component_type_->IsPrimitive();
}

inline bool Object::IsArrayInstance() const {
  return klass_->IsArrayClass();
}

inline bool Object::IsField() const {
  Class* java_lang_Class = klass_->klass_;
  Class* java_lang_reflect_Field = java_lang_Class->GetInstanceField(0)->klass_;
  return klass_ == java_lang_reflect_Field;
}

inline bool Object::IsMethod() const {
  Class* java_lang_Class = klass_->klass_;
  Class* java_lang_reflect_Method = java_lang_Class->GetDirectMethod(0)->klass_;
  return klass_ == java_lang_reflect_Method;
}

inline size_t Object::SizeOf() const {
  if (IsArrayInstance()) {
    return AsArray()->SizeOf();
  }
  if (IsClass()) {
    return AsClass()->SizeOf();
  }
  return klass_->object_size_;
}

inline size_t Array::SizeOf() const {
  return SizeOf(GetLength(), klass_->GetComponentSize());
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

class String : public Object {
 public:
  const CharArray* GetCharArray() const {
    DCHECK(array_ != NULL);
    return array_;
  }

  uint32_t GetHashCode() const {
    return hash_code_;
  }

  int32_t GetOffset() const {
    return offset_;
  }

  int32_t GetLength() const {
    return count_;
  }

  int32_t GetUtfLength() const {
    return CountUtf8Bytes(array_->GetData(), count_);
  }

  // TODO: do we need this? Equals is the only caller, and could
  // bounds check itself.
  uint16_t CharAt(int32_t index) const {
    if (index < 0 || index >= count_) {
      Thread* self = Thread::Current();
      self->ThrowNewException("Ljava/lang/StringIndexOutOfBoundsException;",
          "length=%i; index=%i", count_, index);
      return 0;
    }
    return GetCharArray()->Get(index + GetOffset());
  }

  static String* AllocFromUtf16(int32_t utf16_length,
                                const uint16_t* utf16_data_in,
                                int32_t hash_code = 0) {
    String* string = Alloc(GetJavaLangString(),
                           utf16_length);
    // TODO: use 16-bit wide memset variant
    for (int i = 0; i < utf16_length; i++ ) {
        string->array_->Set(i, utf16_data_in[i]);
    }
    if (hash_code != 0) {
      string->hash_code_ = hash_code;
    } else {
      string->ComputeHashCode();
    }
    return string;
  }

  static String* AllocFromModifiedUtf8(const char* utf) {
    size_t char_count = CountModifiedUtf8Chars(utf);
    return AllocFromModifiedUtf8(char_count, utf);
  }

  static String* AllocFromModifiedUtf8(int32_t utf16_length,
                                       const char* utf8_data_in) {
    String* string = Alloc(GetJavaLangString(), utf16_length);
    uint16_t* utf16_data_out = string->array_->GetData();
    ConvertModifiedUtf8ToUtf16(utf16_data_out, utf8_data_in);
    string->ComputeHashCode();
    return string;
  }

  static void SetClass(Class* java_lang_String);
  static void ResetClass();

  static String* Alloc(Class* java_lang_String, int32_t utf16_length) {
    return Alloc(java_lang_String, CharArray::Alloc(utf16_length));
  }

  static String* Alloc(Class* java_lang_String, CharArray* array) {
    String* string = down_cast<String*>(java_lang_String->AllocObject());
    string->array_ = array;
    string->count_ = array->GetLength();
    return string;
  }

  void ComputeHashCode() {
    hash_code_ = ComputeUtf16Hash(array_->GetData(), count_);
  }

  // TODO: do we need this overload? give it a more intention-revealing name.
  bool Equals(const char* modified_utf8) const {
    for (int32_t i = 0; i < GetLength(); ++i) {
      uint16_t ch = GetUtf16FromUtf8(&modified_utf8);
      if (ch == '\0' || ch != CharAt(i)) {
        return false;
      }
    }
    return *modified_utf8 == '\0';
  }

  // TODO: do we need this overload? give it a more intention-revealing name.
  bool Equals(const StringPiece& modified_utf8) const {
    // TODO: do not assume C-string representation.
    return Equals(modified_utf8.data());
  }

  bool Equals(const String* that) const {
    // TODO: short circuit on hash_code_
    if (this->GetLength() != that->GetLength()) {
      return false;
    }
    for (int32_t i = 0; i < that->GetLength(); ++i) {
      if (this->CharAt(i) != that->CharAt(i)) {
        return false;
      }
    }
    return true;
  }

  // TODO: do we need this overload? give it a more intention-revealing name.
  bool Equals(const uint16_t* that_chars, int32_t that_offset, int32_t that_length) const {
    if (this->GetLength() != that_length) {
      return false;
    }
    for (int32_t i = 0; i < that_length; ++i) {
      if (this->CharAt(i) != that_chars[that_offset + i]) {
        return false;
      }
    }
    return true;
  }

  // Create a modified UTF-8 encoded std::string from a java/lang/String object.
  std::string ToModifiedUtf8() const {
    uint16_t* chars = array_->GetData() + offset_;
    size_t byte_count(CountUtf8Bytes(chars, count_));
    std::string result(byte_count, char(0));
    ConvertUtf16ToModifiedUtf8(&result[0], chars, count_);
    return result;
  }

  const String* Intern() const;

 private:
  // Field order required by test "ValidateFieldOrderOfJavaCppUnionClasses".
  CharArray* array_;

  uint32_t hash_code_;

  int32_t offset_;

  int32_t count_;

  static Class* GetJavaLangString() {
    DCHECK(java_lang_String_ != NULL);
    return java_lang_String_;
  }

  static Class* java_lang_String_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(String);
};

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

class StackTraceElement : public Object {
 public:
  const String* GetDeclaringClass() const {
    return declaring_class_;
  }

  const String* GetMethodName() const {
    return method_name_;
  }

  const String* GetFileName() const {
    return file_name_;
  }

  int32_t GetLineNumber() const {
    return line_number_;
  }

  static StackTraceElement* Alloc(const String* declaring_class, const String* method_name,
                                  const String* file_name, int32_t line_number) {
    StackTraceElement* trace = down_cast<StackTraceElement*>(GetStackTraceElement()->AllocObject());
    trace->declaring_class_ = declaring_class;
    trace->method_name_ = method_name;
    trace->file_name_ = file_name;
    trace->line_number_ = line_number;
    return trace;
  }

  static void SetClass(Class* java_lang_StackTraceElement);

  static void ResetClass();

 private:
  const String* declaring_class_;
  const String* method_name_;
  const String* file_name_;
  int32_t line_number_;

  static Class* GetStackTraceElement() {
    DCHECK(java_lang_StackTraceElement_ != NULL);
    return java_lang_StackTraceElement_;
  }

  static Class* java_lang_StackTraceElement_;
  DISALLOW_IMPLICIT_CONSTRUCTORS(StackTraceElement);
};

inline bool Object::IsString() const {
  // TODO use "klass_ == String::GetJavaLangString()" instead?
  return klass_ == klass_->descriptor_->klass_;
}

inline size_t Class::GetTypeSize(String* descriptor) {
  switch (descriptor->CharAt(0)) {
  case 'B': return 1;  // byte
  case 'C': return 2;  // char
  case 'D': return 8;  // double
  case 'F': return 4;  // float
  case 'I': return 4;  // int
  case 'J': return 8;  // long
  case 'S': return 2;  // short
  case 'Z': return 1;  // boolean
  case 'L': return sizeof(Object*);
  case '[': return sizeof(Array*);
  default:
    LOG(ERROR) << "Unknown type " << descriptor;
    return 0;
  }
}

inline bool Class::IsArrayClass() const {
  return array_rank_ != 0;
}

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

 private:
  // Points to the interface class.
  Class* interface_;

 public:  // TODO: private
  // Index into array of vtable offsets.  This points into the
  // ifvi_pool_, which holds the vtables for all interfaces declared by
  // this class.
  uint32_t* method_index_array_;

 private:
  DISALLOW_COPY_AND_ASSIGN(InterfaceEntry);
};

}  // namespace art

#endif  // ART_SRC_OBJECT_H_
