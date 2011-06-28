// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_OBJECT_H_
#define ART_SRC_OBJECT_H_

#include "src/globals.h"
#include "src/macros.h"

namespace art {

class Array;
class Class;
class DexFile;
class IField;
class InterfaceEntry;
class Monitor;
class Method;
class Object;
class SField;

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

static const uint32_t kAccPublic = 0x0001; // class, field, method, ic
static const uint32_t kAccPrivate = 0x0002; // field, method, ic
static const uint32_t kAccProtected = 0x0004; // field, method, ic
static const uint32_t kAccStatic = 0x0008; // field, method, ic
static const uint32_t kAccFinal = 0x0010; // class, field, method, ic
static const uint32_t kAccSynchronized = 0x0020; // method (only allowed on natives)
static const uint32_t kAccSuper = 0x0020; // class (not used in Dalvik)
static const uint32_t kAccVolatile = 0x0040; // field
static const uint32_t kAccBridge = 0x0040; // method (1.5)
static const uint32_t kAccTransient = 0x0080; // field
static const uint32_t kAccVarargs = 0x0080; // method (1.5)
static const uint32_t kAccNative = 0x0100; // method
static const uint32_t kAccInterface = 0x0200; // class, ic
static const uint32_t kAccAbstract = 0x0400; // class, method, ic
static const uint32_t kAccStrict = 0x0800; // method
static const uint32_t kAccSynthetic = 0x1000; // field, method, ic
static const uint32_t kAccAnnotation = 0x2000; // class, ic (1.5)
static const uint32_t kAccEnum = 0x4000; // class, field, ic (1.5)

static const uint32_t kAccConstructor = 0x00010000; // method (Dalvik only)
static const uint32_t kAccDeclaredSynchronized = 0x00020000; // method (Dalvik only)


class Object {
 public:
  Class* klass_;
  Monitor* lock_;
};

class Field {
 public:
  // The class in which this field is declared.
  Class* klass_;

  const char* name_;

  // e.g. "I", "[C", "Landroid/os/Debug;"
  const char* signature_;

  uint32_t access_flags_;
};

// Instance fields.
class IField : public Field {
  uint32_t offset_;
};

// Static fields.
class SField : public Field {
  JValue value_;
};

// Class objects.
class Class : public Object {
 public:
  enum ClassStatus {
    kClassError = -1,
    kClassNotReady = 0,
    kClassIdx = 1,  // loaded, DEX idx in super or ifaces
    kClassLoaded = 2,  // DEX idx values resolved
    kClassResolved = 3,  // part of linking
    kClassVerifying = 4,  // in the process of being verified
    kClassVerified = 5,  // logically part of linking; done pre-init
    kClassInitializing = 6,  // class init in progress
    kClassInitialized = 7,  // ready to go
  };

  enum PrimitiveType {
    kPrimNot = -1
  };

  // Returns the size in bytes of a class object instance with the
  // given number of static fields.
  static size_t Size(size_t num_sfields) {
    return OFFSETOF_MEMBER(Class, sfields_) + sizeof(SField) * num_sfields;
  }

  uint32_t NumDirectMethods() {
    return num_dmethods_;
  }

  uint32_t NumVirtualMethods() {
    return num_vmethods_;
  }


 public: // TODO: private
  // leave space for instance data; we could access fields directly if
  // we freeze the definition of java/lang/Class
#define CLASS_FIELD_SLOTS   4
  uint32_t instance_data_[CLASS_FIELD_SLOTS];
#undef CLASS_FIELD_SLOTS

  // UTF-8 descriptor for the class from constant pool
  // ("Ljava/lang/Class;"), or on heap if generated ("[C")
  const char* descriptor_;

  // Proxy classes have their descriptor allocated on the native heap.
  // When this field is non-NULL it must be explicitly freed.
  char* descriptor_alloc_;

  // access flags; low 16 bits are defined by VM spec
  uint32_t access_flags_;  // TODO: make an instance field?

  // VM-unique class serial number, nonzero, set very early
  //uint32_t serial_number_;

  // DexFile from which we came; needed to resolve constant pool entries
  // (will be NULL for VM-generated, e.g. arrays and primitive classes)
  DexFile* dex_file_;

  // state of class initialization
  ClassStatus status_;

  // if class verify fails, we must return same error on subsequent tries
  Class* verify_error_class_;

  // threadId, used to check for recursive <clinit> invocation
  uint32_t clinit_thread_id_;

  // Total object size; used when allocating storage on gc heap.  (For
  // interfaces and abstract classes this will be zero.)
  uint32_t object_size_;

  // For array classes, the class object for base element, for
  // instanceof/checkcast (for String[][][], this will be String).
  // Otherwise, NULL.
  Class* array_element_class_;  // TODO: make an instance field

  // For array classes, the number of array dimensions, e.g. int[][]
  // is 2.  Otherwise 0.
  int32_t array_rank_;

  // primitive type index, or PRIM_NOT (-1); set for generated prim classes
  PrimitiveType primitive_type_;

  // The superclass, or NULL if this is java.lang.Object or a
  // primitive type.
  Class* super_;  // TODO: make an instance field

  // defining class loader, or NULL for the "bootstrap" system loader
  Object* class_loader_;  // TODO: make an instance field

  // initiating class loader list
  // NOTE: for classes with low serialNumber, these are unused, and the
  // values are kept in a table in gDvm.
  //InitiatingLoaderList initiating_loader_list_;

  // array of interfaces this class implements directly
  int interface_count_;
  Class** interfaces_;

  // static, private, and <init> methods
  uint32_t num_dmethods_;
  Method* dmethods_;

  // virtual methods defined in this class; invoked through vtable
  uint32_t num_vmethods_;
  Method* vmethods_;

  // Virtual method table (vtable), for use by "invoke-virtual".  The
  // vtable from the superclass is copied in, and virtual methods from
  // our class either replace those from the super or are appended.
  int32_t vtable_count_;
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
  int iftable_count_;
  InterfaceEntry* iftable_;

  // The interface vtable indices for iftable get stored here.  By
  // placing them all in a single pool for each class that implements
  // interfaces, we decrease the number of allocations.
  int ifvipool_count;
  int* ifvipool_;

  // instance fields
  //
  // These describe the layout of the contents of a
  // DataObject-compatible Object.  Note that only the fields directly
  // declared by this class are listed in ifields; fields declared by
  // a superclass are listed in the superclass's ClassObject.ifields.
  //
  // All instance fields that refer to objects are guaranteed to be at
  // the beginning of the field list.  ifieldRefCount specifies the
  // number of reference fields.
  uint32_t num_ifields_;

  // number of fields that are object refs
  uint32_t num_reference_ifields_;
  IField* ifields_;

  // bitmap of offsets of ifields
  uint32_t ref_offsets_;

  // source file name, if known.  Otherwise, NULL.
  const char* source_file_;

  // Static fields
  uint32_t num_sfields_;
  SField sfields_[];  // MUST be last item
};

class String : Object {
 public:
  Array* array_;

  uint32_t hash_code_;

  uint32_t offset_;

  uint32_t count_;
};

class Array : Object {
 public:
  // The number of array elements.
  uint32_t length_;
};

class Method {
 public:
  // Returns true if the method is declared public.
  bool IsPublic() {
    return (access_flags_ & kAccPublic) != 0;
  }

  // Returns true if the method is declared private.
  bool IsPrivate() {
    return (access_flags_ & kAccPrivate) != 0;
  }

  // Returns true if the method is declared static.
  bool IsStatic() {
    return (access_flags_ & kAccStatic) != 0;
  }

  // Returns true if the method is declared synchronized.
  bool IsSynchronized() {
    uint32_t synchonized = kAccSynchronized | kAccDeclaredSynchronized;
    return (access_flags_ & synchonized) != 0;
  }

  // Returns true if the method is declared final.
  bool IsFinal() {
    return (access_flags_ & kAccFinal) != 0;
  }

  // Returns true if the method is declared native.
  bool IsNative() {
    return (access_flags_ & kAccNative) != 0;
  }

  // Returns true if the method is declared abstract.
  bool IsAbstract() {
    return (access_flags_ & kAccAbstract) != 0;
  }

  bool IsSynthetic() {
    return (access_flags_ & kAccSynthetic) != 0;
  }

  // Number of argument registers required by the prototype.
  uint32_t NumArgRegisters();

 public:  // TODO: private
  // the class we are a part of
  Class* klass_;

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
  const char* name_;

  // A pointer to the DEX file this class was loaded from or NULL for
  // proxy objects.
  DexFile* dex_file_;

  // Method prototype descriptor string (return and argument types).
  uint32_t proto_idx_;

  // The short-form method descriptor string.
  const char* shorty_;

  // A pointer to the memory-mapped DEX code.
  const uint16_t* insns_;
};

}  // namespace art

#endif  // ART_SRC_OBJECT_H_
