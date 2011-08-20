// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_OBJECT_H_
#define ART_SRC_OBJECT_H_

#include "constants.h"
#include "casts.h"
#include "dex_file.h"
#include "globals.h"
#include "heap.h"
#include "logging.h"
#include "macros.h"
#include "offsets.h"
#include "stringpiece.h"
#include "monitor.h"

namespace art {

class Array;
class Class;
class DexCache;
class Field;
class InterfaceEntry;
class Monitor;
class Method;
class Object;
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

  const Object* GetFieldObject(size_t field_offset) const {
    Object* that = const_cast<Object*>(this);
    Object* other = that->GetFieldObject(field_offset);
    return const_cast<const Object*>(other);
  }

  Object* GetFieldObject(size_t field_offset) {
    byte* raw_addr = reinterpret_cast<byte*>(this) + field_offset;
    return *reinterpret_cast<Object**>(raw_addr);
  }

  void SetFieldObject(size_t offset, Object* new_value) {
    byte* raw_addr = reinterpret_cast<byte*>(this) + offset;
    *reinterpret_cast<Object**>(raw_addr) = new_value;
    // TODO: write barrier
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

  bool IsReference() const {
    UNIMPLEMENTED(FATAL);
    return true;
  }

  bool IsWeakReference() const {
    UNIMPLEMENTED(FATAL);
    return true;
  }

  bool IsSoftReference() const {
    UNIMPLEMENTED(FATAL);
    return true;
  }

  bool IsFinalizerReference() const {
    UNIMPLEMENTED(FATAL);
    return true;
  }

  bool IsPhantomReference() const {
    UNIMPLEMENTED(FATAL);
    return true;
  }

  bool IsArray() const;

  Array* AsArray() {
    DCHECK(IsArray());
    return down_cast<Array*>(this);
  }

  const Array* AsArray() const {
    DCHECK(IsArray());
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

  bool IsField() const;

  Field* AsField() {
    DCHECK(IsField());
    return down_cast<Field*>(this);
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

  // static field access
  bool GetBoolean();
  void SetBoolean(bool z);
  int8_t GetByte();
  void SetByte(int8_t b);
  uint16_t GetChar();
  void SetChar(uint16_t c);
  uint16_t GetShort();
  void SetShort(uint16_t s);
  int32_t GetInt();
  void SetInt(int32_t i);
  int64_t GetLong();
  void SetLong(int64_t j);
  float GetFloat();
  void SetFloat(float f);
  double GetDouble();
  void SetDouble(double d);
  Object* GetObject();
  const Object* GetObject() const;
  void SetObject(Object* l);

 public:  // TODO: private
  // Field order required by test "ValidateFieldOrderOfJavaCppUnionClasses".
  // The class in which this field is declared.
  Class* declaring_class_;
  Object* generic_type_;
  uint32_t generic_types_are_initialized_;
  String* name_;
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
  uint32_t NumArgRegisters();

  // Number of argument bytes required for densely packing the
  // arguments into an array of arguments.
  size_t NumArgArrayBytes();

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
  String* name_;
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

  const void* GetCode() const {
    return code_;
  }

  void SetCode(const void* code) {
    code_ = code;
  }

  static size_t GetCodeOffset() {
    return OFFSETOF_MEMBER(Method, code_);
  }

  void RegisterNative(const void* native_method) {
    native_method_ = native_method;
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
  // in "vtable".
  //
  // For abstract methods in an interface class, this is the offset
  // of the method in "iftable[n]->methodIndexArray".
  uint16_t method_index_;

  // Method bounds; not needed for an abstract method.
  //
  // For a native method, we compute the size of the argument list, and
  // set "insSize" and "registerSize" equal to it.
  uint16_t num_registers_;  // ins + locals
  uint16_t num_outs_;
  uint16_t num_ins_;

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

 private:
  // Compiled code associated with this method
  const void* code_;

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

  // A convenience for code that doesn't know the component size,
  // and doesn't want to have to work it out itself.
  static Array* Alloc(Class* array_class, size_t component_count);

  static Array* Alloc(Class* array_class,
                      size_t component_count,
                      size_t component_size) {
    size_t size = SizeOf(component_count, component_size);
    Array* array = down_cast<Array*>(Heap::AllocObject(array_class, size));
    if (array != NULL) {
      array->SetLength(component_count);
    }
    return array;
  }

  size_t SizeOf() const;

  int32_t GetLength() const {
    return length_;
  }

  void SetLength(uint32_t length) {
    length_ = length;
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

  DISALLOW_IMPLICIT_CONSTRUCTORS(Array);
};

template<class T>
class ObjectArray : public Array {
 public:
  static ObjectArray<T>* Alloc(Class* object_array_class,
                               size_t length) {
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

// ClassLoader objects.
class ClassLoader : public Object {
 public:
  std::vector<const DexFile*>& GetClassPath() {
    return class_path_;
  }
  void SetClassPath(std::vector<const DexFile*>& class_path) {
    DCHECK_EQ(0U, class_path_.size());
    class_path_ = class_path;
  }

 private:
  // Field order required by test "ValidateFieldOrderOfJavaCppUnionClasses".
  Object* packages_;
  ClassLoader* parent_;

  // TODO: remove once we can create a real PathClassLoader
  std::vector<const DexFile*> class_path_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(ClassLoader);
};

class BaseDexClassLoader : public ClassLoader {
 private:
  // Field order required by test "ValidateFieldOrderOfJavaCppUnionClasses".
  String* original_path_;
  Object* path_list_;
  DISALLOW_IMPLICIT_CONSTRUCTORS(BaseDexClassLoader);
};

class PathClassLoader : public BaseDexClassLoader {
 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(PathClassLoader);
};

// Class objects.
class Class : public Object {
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
  // populated based on super_class_idx_ and interfaces_idx_. The new
  // Class can then be inserted into the classes table.
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
    kStatusIdx = 1,  // loaded, DEX idx in super_class_idx_ and interfaces_idx_
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

  Object* NewInstance() {
    return Heap::AllocObject(this, this->object_size_);
  }

  Class* GetSuperClass() const {
    return super_class_;
  }

  uint32_t GetSuperClassIdx() const {
    return super_class_idx_;
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
    if (klass->IsArray()) {
      return IsAssignableFromArray(klass);
    }
    return klass->IsSubClass(this);
  }

  ClassLoader* GetClassLoader() const {
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
  bool IsArray() const;

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

  // Returns true if this class can access that class.
  bool CanAccess(const Class* that) const {
    return that->IsPublic() || this->IsInSamePackage(that);
  }

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
                                   const StringPiece& descriptor);

  Method* FindDirectMethod(const StringPiece& name,
                           const StringPiece& descriptor);

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

  // Finds the given instance field in this class or a superclass.
  Field* FindInstanceField(const StringPiece& name,
      const StringPiece& descriptor);

  Field* FindDeclaredInstanceField(const StringPiece& name,
      const StringPiece& descriptor);

  // Finds the given static field in this class or a superclass.
  Field* FindStaticField(const StringPiece& name,
      const StringPiece& descriptor);

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

  uint32_t GetReferenceOffsets() const {
    return reference_offsets_;
  }

  void SetReferenceOffsets(uint32_t new_reference_offsets) {
    reference_offsets_ = new_reference_offsets;
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
  uint32_t super_class_idx_;

  // defining class loader, or NULL for the "bootstrap" system loader
  ClassLoader* class_loader_;  // TODO: make an instance field

  // initiating class loader list
  // NOTE: for classes with low serialNumber, these are unused, and the
  // values are kept in a table in gDvm.
  // InitiatingLoaderList initiating_loader_list_;

  // array of interfaces this class implements directly
  ObjectArray<Class>* interfaces_;
  uint32_t* interfaces_idx_;

  // static, private, and <init> methods
  ObjectArray<Method>* direct_methods_;

  // virtual methods defined in this class; invoked through vtable
  ObjectArray<Method>* virtual_methods_;

  // Virtual method table (vtable), for use by "invoke-virtual".  The
  // vtable from the superclass is copied in, and virtual methods from
  // our class either replace those from the super or are appended.
  ObjectArray<Method>* vtable_;

  // Interface table (iftable), one entry per interface supported by
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
  InterfaceEntry* iftable_;

  // The interface vtable indices for iftable get stored here.  By
  // placing them all in a single pool for each class that implements
  // interfaces, we decrease the number of allocations.
  size_t ifvi_pool_count_;
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

  // number of fields that are object refs
  size_t num_reference_instance_fields_;

  // Bitmap of offsets of ifields.
  uint32_t reference_offsets_;

  // source file name, if known.  Otherwise, NULL.
  const char* source_file_;

  // Static fields
  ObjectArray<Field>* sfields_;

  // static field storage
  //
  // Each static field is stored in one of three arrays:
  //  o references are stored in static_references_
  //  o doubles and longs are stored in static_64bit_primitives_
  //  o everything else is in static_32bit_primitives_
  // Static fields select their array using their type and their index using the
  // Field->slot_ member. Storing static fields in arrays avoids the need for a
  // special case in the GC.
  ObjectArray<Object>* static_references_;
  IntArray* static_32bit_primitives_;
  LongArray* static_64bit_primitives_;

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

inline bool Object::IsObjectArray() const {
  return IsArray() && !klass_->component_type_->IsPrimitive();
}

inline bool Object::IsArray() const {
  return klass_->IsArray();
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
  if (IsArray()) {
    return AsArray()->SizeOf();
  }
  return klass_->object_size_;
}

inline size_t Array::SizeOf() const {
  return SizeOf(GetLength(), klass_->GetComponentSize());
}

class DataObject : public Object {
 public:
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

  uint32_t GetOffset() const {
    return offset_;
  }

  uint32_t GetLength() const {
    return count_;
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
                                int32_t hash_code) {
    String* string = Alloc(GetJavaLangString(),
                           utf16_length);
    // TODO: use 16-bit wide memset variant
    for (int i = 0; i < utf16_length; i++ ) {
        string->array_->Set(i, utf16_data_in[i]);
    }
    string->ComputeHashCode();
    return string;
  }

  static String* AllocFromModifiedUtf8(const char* utf) {
    size_t char_count = ModifiedUtf8Len(utf);
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

  static String* Alloc(Class* java_lang_String,
                       int32_t utf16_length) {
    return Alloc(java_lang_String, CharArray::Alloc(utf16_length));
  }

  static String* Alloc(Class* java_lang_String,
                       CharArray* array) {
    String* string = down_cast<String*>(java_lang_String->NewInstance());
    string->array_ = array;
    string->count_ = array->GetLength();
    return string;
  }

  // Convert Modified UTF-8 to UTF-16
  // http://en.wikipedia.org/wiki/UTF-8#Modified_UTF-8
  static void ConvertModifiedUtf8ToUtf16(uint16_t* utf16_data_out, const char* utf8_data_in) {
    while (*utf8_data_in != '\0') {
      *utf16_data_out++ = GetUtf16FromUtf8(&utf8_data_in);
    }
  }

  // Retrieve the next UTF-16 character from a UTF-8 string.
  //
  // Advances "*pUtf8Ptr" to the start of the next character.
  //
  // WARNING: If a string is corrupted by dropping a '\0' in the middle
  // of a 3-byte sequence, you can end up overrunning the buffer with
  // reads (and possibly with the writes if the length was computed and
  // cached before the damage). For performance reasons, this function
  // assumes that the string being parsed is known to be valid (e.g., by
  // already being verified). Most strings we process here are coming
  // out of dex files or other internal translations, so the only real
  // risk comes from the JNI NewStringUTF call.
  static uint16_t GetUtf16FromUtf8(const char** utf8_data_in) {
    uint8_t one = *(*utf8_data_in)++;
    if ((one & 0x80) == 0) {
      // one-byte encoding
      return one;
    }
    // two- or three-byte encoding
    uint8_t two = *(*utf8_data_in)++;
    if ((one & 0x20) == 0) {
      // two-byte encoding
      return ((one & 0x1f) << 6) |
              (two & 0x3f);
    }
    // three-byte encoding
    uint8_t three = *(*utf8_data_in)++;
    return ((one & 0x0f) << 12) |
            ((two & 0x3f) << 6) |
            (three & 0x3f);
  }

  // Like "strlen", but for strings encoded with "modified" UTF-8.
  //
  // The value returned is the number of characters, which may or may not
  // be the same as the number of bytes.
  //
  // (If this needs optimizing, try: mask against 0xa0, shift right 5,
  // get increment {1-3} from table of 8 values.)
  static size_t ModifiedUtf8Len(const char* utf8) {
    size_t len = 0;
    int ic;
    while ((ic = *utf8++) != '\0') {
      len++;
      if ((ic & 0x80) == 0) {
        // one-byte encoding
        continue;
      }
      // two- or three-byte encoding
      utf8++;
      if ((ic & 0x20) == 0) {
        // two-byte encoding
        continue;
      }
      // three-byte encoding
      utf8++;
    }
    return len;
  }

  // The java/lang/String.computeHashCode() algorithm
  static int32_t ComputeUtf16Hash(const uint16_t* string_data, size_t string_length) {
    int32_t hash = 0;
    while (string_length--) {
      hash = hash * 31 + *string_data++;
    }
    return hash;
  }

  void ComputeHashCode() {
    hash_code_ = ComputeUtf16Hash(array_->GetData(), count_);
  }

  // TODO: do we need this overload? give it a more intention-revealing name.
  bool Equals(const char* modified_utf8) const {
    for (uint32_t i = 0; i < GetLength(); ++i) {
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
    for (uint32_t i = 0; i < that->GetLength(); ++i) {
      if (this->CharAt(i) != that->CharAt(i)) {
        return false;
      }
    }
    return true;
  }

  // TODO: do we need this overload? give it a more intention-revealing name.
  bool Equals(const uint16_t* that_chars, uint32_t that_offset, uint32_t that_length) const {
    if (this->GetLength() != that_length) {
      return false;
    }
    for (uint32_t i = 0; i < that_length; ++i) {
      if (this->CharAt(i) != that_chars[that_offset + i]) {
        return false;
      }
    }
    return true;
  }

  // Create a modified UTF-8 encoded std::string from a java/lang/String object.
  std::string ToModifiedUtf8() const {
    std::string result;
    for (uint32_t i = 0; i < GetLength(); i++) {
      uint16_t ch = CharAt(i);
      // The most common case is (ch > 0 && ch <= 0x7f).
      if (ch == 0 || ch > 0x7f) {
        if (ch > 0x07ff) {
          result.push_back((ch >> 12) | 0xe0);
          result.push_back(((ch >> 6) & 0x3f) | 0x80);
          result.push_back((ch & 0x3f) | 0x80);
        } else { // (ch > 0x7f || ch == 0)
          result.push_back((ch >> 6) | 0xc0);
          result.push_back((ch & 0x3f) | 0x80);
        }
      } else {
        result.push_back(ch);
      }
    }
    return result;
  }


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

inline bool Class::IsArray() const {
  return GetDescriptor()->CharAt(0) == '[';  // TODO: avoid parsing the descriptor
}

class InterfaceEntry {
 public:
  InterfaceEntry() : klass_(NULL), method_index_array_(NULL) {
  }

  Class* GetClass() const {
    return klass_;
  }

  void SetClass(Class* klass) {
    klass_ = klass;
  }

 private:
  // Points to the interface class.
  Class* klass_;

 public:  // TODO: private
  // Index into array of vtable offsets.  This points into the
  // ifviPool, which holds the vtables for all interfaces declared by
  // this class.
  uint32_t* method_index_array_;

 private:
  DISALLOW_COPY_AND_ASSIGN(InterfaceEntry);
};

}  // namespace art

#endif  // ART_SRC_OBJECT_H_
