// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_OBJECT_H_
#define ART_SRC_OBJECT_H_

#include "constants.h"
#include "casts.h"
#include "globals.h"
#include "logging.h"
#include "macros.h"
#include "offsets.h"
#include "stringpiece.h"
#include "monitor.h"

namespace art {

class Array;
class Class;
class DexFile;
class InstanceField;
class InterfaceEntry;
class Monitor;
class Method;
class Object;
class ObjectArray;
class StaticField;

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
  Class* GetClass() const {
    return klass_;
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

  const Object* GetFieldObject(size_t field_offset) const {
    const byte* raw_addr = reinterpret_cast<const byte*>(this) + field_offset;
    return *reinterpret_cast<Object* const*>(raw_addr);
  }

  Object* GetFieldObject(size_t field_offset) {
    return const_cast<Object*>(GetFieldObject(field_offset));
  }

  void SetFieldObject(size_t offset, Object* new_value) {
    byte* raw_addr = reinterpret_cast<byte*>(this) + offset;
    *reinterpret_cast<Object**>(raw_addr) = new_value;
    // TODO: write barrier
  }

  bool IsClass() const {
    LOG(FATAL) << "Unimplemented";
    return true;
  }

  Class* AsClass() {
    return down_cast<Class*>(this);
  }

  const Class* AsClass() const {
    return down_cast<const Class*>(this);
  }

  bool IsObjectArray() const {
    LOG(FATAL) << "Unimplemented";
    return true;
  }

  const ObjectArray* AsObjectArray() const {
    return down_cast<const ObjectArray*>(this);
  }

  bool IsReference() const {
    LOG(FATAL) << "Unimplemented";
    return true;
  }

  bool IsWeakReference() const {
    LOG(FATAL) << "Unimplemented";
    return true;
  }

  bool IsSoftReference() const {
    LOG(FATAL) << "Unimplemented";
    return true;
  }

  bool IsFinalizerReference() const {
    LOG(FATAL) << "Unimplemented";
    return true;
  }

  bool IsPhantomReference() const {
    LOG(FATAL) << "Unimplemented";
    return true;
  }

  bool IsArray() const {
    LOG(FATAL) << "Unimplemented";
    return true;
  }

 public:
  Class* klass_;

  Monitor* monitor_;

 private:
  Object();
  DISALLOW_COPY_AND_ASSIGN(Object);
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

class Field : public Object {
 public:
  Class* GetDeclaringClass() const {
    return declaring_class_;
  }

  const char* GetName() const {
    return name_;
  }

  char GetType() const {  // TODO: return type
    return signature_[0];
  }

  const char* GetSignature() const {
    return signature_;
  }

 public:  // TODO: private
#define FIELD_FIELD_SLOTS 1+6
  // AccessibleObject #0 flag
  // Field #0 declaringClass
  // Field #1 genericType
  // Field #2 genericTypesAreInitialized
  // Field #3 name
  // Field #4 slot
  // Field #5 type
  uint32_t instance_data_[FIELD_FIELD_SLOTS];
#undef FIELD_FIELD_SLOTS

  // The class in which this field is declared.
  Class* declaring_class_;

  const char* name_;

  // e.g. "I", "[C", "Landroid/os/Debug;"
  const char* signature_;

  uint32_t access_flags_;

 private:
  Field();
};

// Instance fields.
class InstanceField : public Field {
 public:
  uint32_t GetOffset() const {
    return offset_;
  }

  void SetOffset(size_t num_bytes) {
    offset_ = num_bytes;
  }

 private:
  size_t offset_;
  InstanceField();
};

// Static fields.
class StaticField : public Field {
 public:
  void SetBoolean(bool z) {
    CHECK_EQ(GetType(), 'Z');
    value_.z = z;
  }

  void SetByte(int8_t b) {
    CHECK_EQ(GetType(), 'B');
    value_.b = b;
  }

  void SetChar(uint16_t c) {
    CHECK_EQ(GetType(), 'C');
    value_.c = c;
  }

  void SetShort(uint16_t s) {
    CHECK_EQ(GetType(), 'S');
    value_.s = s;
  }

  void SetInt(int32_t i) {
    CHECK_EQ(GetType(), 'I');
    value_.i = i;
  }

  int64_t GetLong() {
    CHECK_EQ(GetType(), 'J');
    return value_.j;
  }

  void SetLong(int64_t j) {
    CHECK_EQ(GetType(), 'J');
    value_.j = j;
  }

  void SetFloat(float f) {
    CHECK_EQ(GetType(), 'F');
    value_.f = f;
  }

  void SetDouble(double d) {
    CHECK_EQ(GetType(), 'D');
    value_.d = d;
  }

  Object* GetObject() {
    return value_.l;
  }

  const Object* GetObject() const {
    return value_.l;
  }

  void SetObject(Object* l) {
    CHECK(GetType() == 'L' || GetType() == '[');
    value_.l = l;
    // TODO: write barrier
  }

 private:
  JValue value_;
  StaticField();
};

class Method : public Object {
 public:
  // Returns the method name.
  // TODO: example
  const StringPiece& GetName() const {
    return name_;
  }

  Class* GetDeclaringClass() const {
    return declaring_class_;
  }

  static MemberOffset ClassOffset() {
    return MemberOffset(OFFSETOF_MEMBER(Method, klass_));
  }

  // const char* GetReturnTypeDescriptor() const {
  //   return declaring_class_->GetDexFile_->GetRaw()
  //          ->dexStringByTypeIdx(proto_id_.return_type_id_);
  // }

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

 public:  // TODO: private
#define METHOD_FIELD_SLOTS 1+11
  // AccessibleObject #0 flag
  // Method #0  declaringClass
  // Method #1  exceptionTypes
  // Method #2  formalTypeParameters
  // Method #3  genericExceptionTypes
  // Method #4  genericParameterTypes
  // Method #5  genericReturnType
  // Method #6  genericTypesAreInitialized
  // Method #7  name
  // Method #8  parameterTypes
  // Method #9  returnType
  // Method #10 slot
  uint32_t instance_data_[METHOD_FIELD_SLOTS];
#undef METHOD_FIELD_SLOTS

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

  // The number of arguments that should be supplied to this method
  size_t NumArgs() const {
    return (IsStatic() ? 0 : 1) + shorty_.length() - 1;
  }

  // The number of reference arguments to this method including implicit this
  // pointer
  size_t NumReferenceArgs() const;

  // The number of long or double arguments
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

  void SetCode(const void* code) {
    code_ = code;
  }

  const void* GetCode() const {
    return code_;
  }

  void RegisterNative(const void* native_method) {
    native_method_ = native_method;
  }

  static MemberOffset NativeMethodOffset() {
    return MemberOffset(OFFSETOF_MEMBER(Method, native_method_));
  }

 public:  // TODO: private/const
  // the class we are a part of
  Class* declaring_class_;

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

  // method name, e.g. "<init>" or "eatLunch"
  StringPiece name_;

  // Method prototype descriptor string (return and argument types).
  uint32_t proto_idx_;

  // The short-form method descriptor string.
  StringPiece shorty_;

  // A pointer to the memory-mapped DEX code.
  const uint16_t* insns_;

 private:
  Method();

  // Compiled code associated with this method
  const void* code_;

  // Any native method registered with this method
  const void* native_method_;
};

// Class objects.
class Class : public Object {
 public:
  enum Status {
    kStatusError = -1,
    kStatusNotReady = 0,
    kStatusIdx = 1,  // loaded, DEX idx in super or ifaces
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

  Class* GetSuperClass() const {
    return super_class_;
  }

  uint32_t GetSuperClassIdx() const {
    return super_class_idx_;
  }

  bool HasSuperClass() const {
    return super_class_ != NULL;
  }

  Object* GetClassLoader() const {
    return class_loader_;
  }

  DexFile* GetDexFile() const {
    return dex_file_;
  }

  Class* GetComponentType() const {
    DCHECK(IsArray());
    return component_type_;
  }

  const StringPiece& GetDescriptor() const {
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

  bool IsLoaded() const {
    return GetStatus() >= kStatusLoaded;
  }

  // Returns true if this class is in the same packages as that class.
  bool IsInSamePackage(const Class* that) const;

  static bool IsInSamePackage(const StringPiece& descriptor1,
                              const StringPiece& descriptor2);

  // Returns true if this class represents an array class.
  bool IsArray() const {
    return descriptor_[0] == '[';  // TODO: avoid parsing the descriptor
  }

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

  // Returns true if this class can access that class.
  bool CanAccess(const Class* that) const {
    return that->IsPublic() || this->IsInSamePackage(that);
  }

  // Returns the size in bytes of a class object instance with the
  // given number of static fields.
  // static size_t Size(size_t num_sfields) {
  //   return OFFSETOF_MEMBER(Class, sfields_) +
  //          sizeof(StaticField) * num_sfields;
  // }

  // Returns the number of static, private, and constructor methods.
  size_t NumDirectMethods() const {
    return num_direct_methods_;
  }

  Method* GetDirectMethod(uint32_t i) const {
    return direct_methods_[i];
  }

  // Returns the number of non-inherited virtual methods.
  size_t NumVirtualMethods() const {
    return num_virtual_methods_;
  }

  Method* GetVirtualMethod(uint32_t i) const {
    return virtual_methods_[i];
  }

  size_t NumInstanceFields() const {
    return num_instance_fields_;
  }

  // Returns the number of instance fields containing reference types.
  size_t NumReferenceInstanceFields() const {
    return num_reference_instance_fields_;
  }

  InstanceField* GetInstanceField(uint32_t i) {  // TODO: uint16_t
    DCHECK_LT(i, num_instance_fields_);
    return ifields_[i];
  }

  void SetInstanceField(uint32_t i, InstanceField* f) {  // TODO: uint16_t
    DCHECK_LT(i, num_instance_fields_);
    ifields_[i] = f;
  }

  size_t NumStaticFields() const {
    return num_static_fields_;
  }

  StaticField* GetStaticField(uint32_t i) const {  // TODO: uint16_t
    DCHECK_LT(i, num_static_fields_);
    return sfields_[i];
  }

  uint32_t GetReferenceOffsets() const {
    return reference_offsets_;
  }

  void SetReferenceOffsets(uint32_t new_reference_offsets) {
    reference_offsets_ = new_reference_offsets;
  }

  Method* FindDirectMethod(const StringPiece& name) const;

  Method* FindVirtualMethod(const StringPiece& name) const;

  size_t NumInterfaces() const {
    return interface_count_;
  }

  Class* GetInterface(uint32_t i) const {
    DCHECK_LT(i, interface_count_);
    return interfaces_[i];
  }

  Method* FindDirectMethodLocally(const StringPiece& name,
                                  const StringPiece& descriptor) const;

 public:  // TODO: private
  // leave space for instance data; we could access fields directly if
  // we freeze the definition of java/lang/Class
#define CLASS_FIELD_SLOTS 1
  // Class.#0 name
  uint32_t instance_data_[CLASS_FIELD_SLOTS];
#undef CLASS_FIELD_SLOTS

  // UTF-8 descriptor for the class from constant pool
  // ("Ljava/lang/Class;"), or on heap if generated ("[C")
  StringPiece descriptor_;

  // Proxy classes have their descriptor allocated on the native heap.
  // When this field is non-NULL it must be explicitly freed.
  std::string* descriptor_alloc_;

  // access flags; low 16 bits are defined by VM spec
  uint32_t access_flags_;  // TODO: make an instance field?

  // DexFile from which we came; needed to resolve constant pool entries
  // (will be NULL for VM-generated, e.g. arrays and primitive classes)
  DexFile* dex_file_;

  // state of class initialization
  Status status_;

  // if class verify fails, we must return same error on subsequent tries
  Class* verify_error_class_;

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
  Object* class_loader_;  // TODO: make an instance field

  // initiating class loader list
  // NOTE: for classes with low serialNumber, these are unused, and the
  // values are kept in a table in gDvm.
  // InitiatingLoaderList initiating_loader_list_;

  // array of interfaces this class implements directly
  size_t interface_count_;
  Class** interfaces_;

  // static, private, and <init> methods
  size_t num_direct_methods_;
  Method** direct_methods_;

  // virtual methods defined in this class; invoked through vtable
  size_t num_virtual_methods_;
  Method** virtual_methods_;

  // Virtual method table (vtable), for use by "invoke-virtual".  The
  // vtable from the superclass is copied in, and virtual methods from
  // our class either replace those from the super or are appended.
  size_t vtable_count_;
  Method** vtable_;

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
  // the beginning of the field list.  ifieldRefCount specifies the
  // number of reference fields.
  size_t num_instance_fields_;

  // number of fields that are object refs
  size_t num_reference_instance_fields_;
  InstanceField** ifields_;

  // Bitmap of offsets of ifields.
  uint32_t reference_offsets_;

  // source file name, if known.  Otherwise, NULL.
  const char* source_file_;

  // Static fields
  size_t num_static_fields_;
  StaticField** sfields_;

 private:
  Class();
};
std::ostream& operator<<(std::ostream& os, const Class::Status& rhs);

class DataObject : public Object {
 public:
  uint32_t fields_[0];
 private:
  DataObject();
};

class Array : public Object {
 public:
  uint32_t GetLength() const {
    return length_;
  }
  void SetLength(uint32_t length) {
    length_ = length;
  }
  const void* GetData() const {
    return &elements_;
  }
  void* GetData() {
    return &elements_;
  }

 private:
  // The number of array elements.
  uint32_t length_;
  // Location of first element.
  uint32_t elements_[0];
  Array();
};

class CharArray : public Array {
 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(CharArray);
};

class ObjectArray : public Array {
 public:
  Object* Get(uint32_t i) const {
    DCHECK_LT(i, GetLength());
    Object* const * data = reinterpret_cast<Object* const *>(GetData());
    return data[i];
  }
  void Set(uint32_t i, Object* object) {
    DCHECK_LT(i, GetLength());
    Object** data = reinterpret_cast<Object**>(GetData());
    data[i] = object;
  }
 private:
  ObjectArray();
};

class String : public Object {
 public:
  CharArray* array_;

  uint32_t hash_code_;

  uint32_t offset_;

  uint32_t count_;

 private:
  String();
};

class InterfaceEntry {
 public:
  Class* GetClass() const {
    return klass_;
  };

  void SetClass(Class* klass) {
    klass_ = klass;
  };

 private:
  // Points to the interface class.
  Class* klass_;

 public:  // TODO: private
  // Index into array of vtable offsets.  This points into the
  // ifviPool, which holds the vtables for all interfaces declared by
  // this class.
  uint32_t* method_index_array_;
};

}  // namespace art

#endif  // ART_SRC_OBJECT_H_
