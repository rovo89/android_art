// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_OBJECT_H_
#define ART_SRC_OBJECT_H_

#include "src/globals.h"

namespace art {

class Array;
class Class;
class DexFile;
class IField;
class InterfaceEntry;
class Monitor;
class Method;
class SField;

class Object {
  Class* klass_;
  Monitor* lock_;
};

class Field {
  Class* klass_;
};

// Instance fields.
class IField : Field {
  // TODO
};

// Static fields.
class SField : Field {
  // TODO
};

// Class objects.
class Class : Object {
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
  uint32_t access_flags_;

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
  Class* array_element_class_;

  // For array classes, the number of array dimensions, e.g. int[][]
  // is 2.  Otherwise 0.
  int32_t array_rank_;

  // primitive type index, or PRIM_NOT (-1); set for generated prim classes
  PrimitiveType primitive_type_;

  // The superclass, or NULL if this is java.lang.Object or a
  // primitive type.
  Class* super_;

  // defining class loader, or NULL for the "bootstrap" system loader
  Object* class_loader_;

  // initiating class loader list
  // NOTE: for classes with low serialNumber, these are unused, and the
  // values are kept in a table in gDvm.
  //InitiatingLoaderList initiating_loader_list_;

  // array of interfaces this class implements directly
  int interface_count_;
  Class** interfaces_;

  // static, private, and <init> methods
  int direct_method_count_;
  Method* direct_methods_;

  // virtual methods defined in this class; invoked through vtable
  int virtual_method_count_;
  Method* virtual_methods_;

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
  int ifield_count_;
  int ifield_ref_count_; // number of fields that are object refs
  IField* ifields_;

  // bitmap of offsets of ifields
  uint32_t ref_offsets_;

  // source file name, if known.  Otherwise, NULL.
  const char* source_file_;

  // Static fields
  uint16_t num_sfields_;
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
  Class* klass;
};

}  // namespace art

#endif  // ART_SRC_OBJECT_H_
