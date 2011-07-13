// Copyright 2011 Google Inc. All Rights Reserved.
// Author: cshapiro@google.com (Carl Shapiro)

#include "src/class_linker.h"

#include <vector>
#include <utility>

#include "src/casts.h"
#include "src/dex_verifier.h"
#include "src/heap.h"
#include "src/logging.h"
#include "src/monitor.h"
#include "src/object.h"
#include "src/raw_dex_file.h"
#include "src/scoped_ptr.h"
#include "src/thread.h"
#include "src/utils.h"

namespace art {

void ClassLinker::Init() {
  // Allocate and partially initialize the Object and Class classes.
  // Initialization will be completed when the definitions are loaded.
  java_lang_Object_ = Heap::AllocClass(NULL);
  java_lang_Object_->descriptor_ = "Ljava/lang/Object;";
  java_lang_Class_ = Heap::AllocClass(NULL);
  java_lang_Class_->descriptor_ = "Ljava/lang/Class;";

  // Allocate and initialize the primitive type classes.
  primitive_byte_ = CreatePrimitiveClass(kTypeByte, "B");
  primitive_char_ = CreatePrimitiveClass(kTypeChar, "C");
  primitive_double_ = CreatePrimitiveClass(kTypeDouble, "D");
  primitive_float_ = CreatePrimitiveClass(kTypeFloat, "F");
  primitive_int_ = CreatePrimitiveClass(kTypeInt, "I");
  primitive_long_ = CreatePrimitiveClass(kTypeLong, "J");
  primitive_short_ = CreatePrimitiveClass(kTypeShort, "S");
  primitive_boolean_ = CreatePrimitiveClass(kTypeBoolean, "Z");
  primitive_void_ = CreatePrimitiveClass(kTypeVoid, "V");
}

Class* ClassLinker::FindClass(const char* descriptor,
                              Object* class_loader,
                              DexFile* dex_file) {
  Thread* self = Thread::Current();
  CHECK(!self->IsExceptionPending());
  // Find the class in the loaded classes table.
  Class* klass = LookupClass(descriptor, class_loader);
  if (klass == NULL) {
    // Class is not yet loaded.
    if (dex_file == NULL) {
      // No .dex file specified, search the class path.
      dex_file = FindInClassPath(descriptor);
      if (dex_file == NULL) {
        LG << "Class " << descriptor << " really not found";
        return NULL;
      }
    }
    // Load the class from the dex file.
    if (!strcmp(descriptor, "Ljava/lang/Object;")) {
      klass = java_lang_Object_;
      klass->dex_file_ = dex_file;
    } else if (!strcmp(descriptor, "Ljava/lang/Class;")) {
      klass = java_lang_Class_;
      klass->dex_file_ = dex_file;
    } else {
      klass = Heap::AllocClass(dex_file);
    }
    bool is_loaded = LoadClass(descriptor, klass);
    if (!is_loaded) {
      // TODO: this occurs only when a dex file is provided.
      LG << "Class not found";  // TODO: NoClassDefFoundError
      return NULL;
    }
    // Check for a pending exception during load
    if (self->IsExceptionPending()) {
      // TODO: free native allocations in klass
      return NULL;
    }
    {
      ObjectLock lock(klass);
      klass->clinit_thread_id_ = self->GetId();
      // Add the newly loaded class to the loaded classes table.
      bool success = InsertClass(klass);
      if (!success) {
        // We may fail to insert if we raced with another thread.
        klass->clinit_thread_id_ = 0;
        // TODO: free native allocations in klass
        klass = LookupClass(descriptor, class_loader);
        CHECK(klass != NULL);
      } else {
        // Link the class.
        if (!LinkClass(klass)) {
          // Linking failed.
          // TODO: CHECK(self->IsExceptionPending());
          lock.NotifyAll();
          return NULL;
        }
      }
    }
  }
  // Link the class if it has not already been linked.
  if (!klass->IsLinked() && !klass->IsErroneous()) {
    ObjectLock lock(klass);
    // Check for circular dependencies between classes.
    if (!klass->IsLinked() && klass->clinit_thread_id_ == self->GetId()) {
      LG << "Recursive link";  // TODO: ClassCircularityError
      return NULL;
    }
    // Wait for the pending initialization to complete.
    while (!klass->IsLinked() && !klass->IsErroneous()) {
      lock.Wait();
    }
  }
  if (klass->IsErroneous()) {
    LG << "EarlierClassFailure";  // TODO: EarlierClassFailure
    return NULL;
  }
  // Return the loaded class.  No exceptions should be pending.
  CHECK(!self->IsExceptionPending());
  return klass;
}

bool ClassLinker::LoadClass(const char* descriptor, Class* klass) {
  const RawDexFile* raw = klass->GetDexFile()->GetRaw();
  const RawDexFile::ClassDef* class_def = raw->FindClassDef(descriptor);
  if (class_def == NULL) {
    return false;
  } else {
    return LoadClass(*class_def, klass);
  }
}

bool ClassLinker::LoadClass(const RawDexFile::ClassDef& class_def, Class* klass) {
  CHECK(klass != NULL);
  CHECK(klass->dex_file_ != NULL);
  const RawDexFile* raw = klass->GetDexFile()->GetRaw();
  const byte* class_data = raw->GetClassData(class_def);
  RawDexFile::ClassDataHeader header = raw->ReadClassDataHeader(&class_data);

  const char* descriptor = raw->GetClassDescriptor(class_def);
  CHECK(descriptor != NULL);

  klass->klass_ = java_lang_Class_;
  klass->descriptor_.set(descriptor);
  klass->descriptor_alloc_ = NULL;
  klass->access_flags_ = class_def.access_flags_;
  klass->class_loader_ = NULL;  // TODO
  klass->primitive_type_ = Class::kPrimNot;
  klass->status_ = Class::kStatusIdx;

  klass->super_class_ = NULL;
  klass->super_class_idx_ = class_def.superclass_idx_;

  klass->num_sfields_ = header.static_fields_size_;
  klass->num_ifields_ = header.instance_fields_size_;
  klass->num_direct_methods_ = header.direct_methods_size_;
  klass->num_virtual_methods_ = header.virtual_methods_size_;

  klass->source_file_ = raw->dexGetSourceFile(class_def);

  // Load class interfaces.
  LoadInterfaces(class_def, klass);

  // Load static fields.
  if (klass->num_sfields_ != 0) {
    // TODO: allocate on the object heap.
    klass->sfields_ = new StaticField[klass->NumStaticFields()]();
    uint32_t last_idx = 0;
    for (size_t i = 0; i < klass->num_sfields_; ++i) {
      RawDexFile::Field raw_field;
      raw->dexReadClassDataField(&class_data, &raw_field, &last_idx);
      LoadField(klass, raw_field, &klass->sfields_[i]);
    }
  }

  // Load instance fields.
  if (klass->NumInstanceFields() != 0) {
    // TODO: allocate on the object heap.
    klass->ifields_ = new InstanceField[klass->NumInstanceFields()]();
    uint32_t last_idx = 0;
    for (size_t i = 0; i < klass->NumInstanceFields(); ++i) {
      RawDexFile::Field raw_field;
      raw->dexReadClassDataField(&class_data, &raw_field, &last_idx);
      LoadField(klass, raw_field, klass->GetInstanceField(i));
    }
  }

  // Load direct methods.
  if (klass->NumDirectMethods() != 0) {
    // TODO: append direct methods to class object
    klass->direct_methods_ = new Method[klass->NumDirectMethods()]();
    uint32_t last_idx = 0;
    for (size_t i = 0; i < klass->NumDirectMethods(); ++i) {
      RawDexFile::Method raw_method;
      raw->dexReadClassDataMethod(&class_data, &raw_method, &last_idx);
      LoadMethod(klass, raw_method, klass->GetDirectMethod(i));
      // TODO: register maps
    }
  }

  // Load virtual methods.
  if (klass->NumVirtualMethods() != 0) {
    // TODO: append virtual methods to class object
    klass->virtual_methods_ = new Method[klass->NumVirtualMethods()]();
    uint32_t last_idx = 0;
    for (size_t i = 0; i < klass->NumVirtualMethods(); ++i) {
      RawDexFile::Method raw_method;
      raw->dexReadClassDataMethod(&class_data, &raw_method, &last_idx);
      LoadMethod(klass, raw_method, klass->GetVirtualMethod(i));
      // TODO: register maps
    }
  }

  return klass;
}

void ClassLinker::LoadInterfaces(const RawDexFile::ClassDef& class_def,
                                 Class* klass) {
  const RawDexFile* raw = klass->GetDexFile()->GetRaw();
  const RawDexFile::TypeList* list = raw->GetInterfacesList(class_def);
  if (list != NULL) {
    klass->interface_count_ = list->Size();
    // TODO: allocate the interfaces array on the object heap.
    klass->interfaces_ = new Class*[list->Size()]();
    for (size_t i = 0; i < list->Size(); ++i) {
      const RawDexFile::TypeItem& type_item = list->GetTypeItem(i);
      klass->interfaces_[i] = reinterpret_cast<Class*>(type_item.type_idx_);
    }
  }
}

void ClassLinker::LoadField(Class* klass, const RawDexFile::Field& src,
                            Field* dst) {
  const RawDexFile* raw = klass->GetDexFile()->GetRaw();
  const RawDexFile::FieldId& field_id = raw->GetFieldId(src.field_idx_);
  dst->klass_ = klass;
  dst->name_ = raw->dexStringById(field_id.name_idx_);
  dst->signature_ = raw->dexStringByTypeIdx(field_id.type_idx_);
  dst->access_flags_ = src.access_flags_;
}

void ClassLinker::LoadMethod(Class* klass, const RawDexFile::Method& src,
                             Method* dst) {
  const RawDexFile* raw = klass->GetDexFile()->GetRaw();
  const RawDexFile::MethodId& method_id = raw->GetMethodId(src.method_idx_);
  dst->klass_ = klass;
  dst->name_.set(raw->dexStringById(method_id.name_idx_));
  dst->proto_idx_ = method_id.proto_idx_;
  dst->shorty_.set(raw->GetShorty(method_id.proto_idx_));
  dst->access_flags_ = src.access_flags_;

  // TODO: check for finalize method

  const RawDexFile::CodeItem* code_item = raw->GetCodeItem(src);
  if (code_item != NULL) {
    dst->num_registers_ = code_item->registers_size_;
    dst->num_ins_ = code_item->ins_size_;
    dst->num_outs_ = code_item->outs_size_;
    dst->insns_ = code_item->insns_;
  } else {
    uint16_t num_args = dst->NumArgRegisters();
    if (!dst->IsStatic()) {
      ++num_args;
    }
    dst->num_registers_ = dst->num_ins_ + num_args;
    // TODO: native methods
  }
}

DexFile* ClassLinker::FindInClassPath(const char* descriptor) {
  for (size_t i = 0; i != class_path_.size(); ++i) {
    DexFile* dex_file = class_path_[i];
    if (dex_file->HasClass(descriptor)) {
      return dex_file;
    }
  }
  return NULL;
}

void ClassLinker::AppendToClassPath(DexFile* dex_file) {
  class_path_.push_back(dex_file);
}

Class* ClassLinker::CreatePrimitiveClass(JType type, const char* descriptor) {
  Class* klass = Heap::AllocClass(NULL);
  CHECK(klass != NULL);
  klass->super_class_ = java_lang_Class_;
  klass->access_flags_ = kAccPublic | kAccFinal | kAccAbstract;
  klass->descriptor_ = descriptor;
  klass->status_ = Class::kStatusInitialized;
  return klass;
}

Class* ClassLinker::FindPrimitiveClass(JType type) {
  switch (type) {
    case kTypeByte:
      CHECK(primitive_byte_ != NULL);
      return primitive_byte_;
    case kTypeChar:
      CHECK(primitive_char_ != NULL);
      return primitive_char_;
    case kTypeDouble:
      CHECK(primitive_double_ != NULL);
      return primitive_double_;
    case kTypeFloat:
      CHECK(primitive_float_ != NULL);
      return primitive_float_;
    case kTypeInt:
      CHECK(primitive_int_ != NULL);
      return primitive_int_;
    case kTypeLong:
      CHECK(primitive_long_ != NULL);
      return primitive_long_;
    case kTypeShort:
      CHECK(primitive_short_ != NULL);
      return primitive_short_;
    case kTypeBoolean:
      CHECK(primitive_boolean_ != NULL);
      return primitive_boolean_;
    case kTypeVoid:
      CHECK(primitive_void_ != NULL);
      return primitive_void_;
    default:
      LOG(FATAL) << "Unknown primitive type " << static_cast<int>(type);
  };
  return NULL;  // Not reachable.
}

bool ClassLinker::InsertClass(Class* klass) {
  // TODO: acquire classes_lock_
  const char* key = klass->GetDescriptor().data();
  bool success = classes_.insert(std::make_pair(key, klass)).second;
  // TODO: release classes_lock_
  return success;
}

Class* ClassLinker::LookupClass(const char* descriptor, Object* class_loader) {
  // TODO: acquire classes_lock_
  Table::iterator it = classes_.find(descriptor);
  // TODO: release classes_lock_
  if (it == classes_.end()) {
    return NULL;
  } else {
    return (*it).second;
  }
}

bool ClassLinker::InitializeClass(Class* klass) {
  CHECK(klass->GetStatus() == Class::kStatusResolved ||
        klass->GetStatus() == Class::kStatusError);

  Thread* self = Thread::Current();

  {
    ObjectLock lock(klass);

    if (klass->GetStatus() < Class::kStatusVerified) {
      if (klass->IsErroneous()) {
        LG << "re-initializing failed class";  // TODO: throw
        return false;
      }

      CHECK(klass->GetStatus() == Class::kStatusResolved);

      klass->status_ = Class::kStatusVerifying;
      if (!DexVerify::VerifyClass(klass)) {
        LG << "Verification failed";  // TODO: ThrowVerifyError
        Object* exception = self->GetException();
        klass->SetObjectAt(OFFSETOF_MEMBER(Class, verify_error_class_),
                           exception->GetClass());
        klass->SetStatus(Class::kStatusError);
        return false;
      }

      klass->SetStatus(Class::kStatusVerified);
    }

    if (klass->status_ == Class::kStatusInitialized) {
      return true;
    }

    while (klass->status_ == Class::kStatusInitializing) {
      // we caught somebody else in the act; was it us?
      if (klass->clinit_thread_id_ == self->GetId()) {
        LG << "recursive <clinit>";
        return true;
      }

      CHECK(!self->IsExceptionPending());

      lock.Wait();  // TODO: check for interruption

      // When we wake up, repeat the test for init-in-progress.  If
      // there's an exception pending (only possible if
      // "interruptShouldThrow" was set), bail out.
      if (self->IsExceptionPending()) {
        CHECK(false);
        LG << "Exception in initialization.";  // TODO: ExceptionInInitializerError
        klass->SetStatus(Class::kStatusError);
        return false;
      }
      if (klass->GetStatus() == Class::kStatusInitializing) {
        continue;
      }
      assert(klass->GetStatus() == Class::kStatusInitialized ||
             klass->GetStatus() == Class::kStatusError);
      if (klass->IsErroneous()) {
        /*
         * The caller wants an exception, but it was thrown in a
         * different thread.  Synthesize one here.
         */
        LG << "<clinit> failed";  // TODO: throw UnsatisfiedLinkError
        return false;
      }
      return true;  // otherwise, initialized
    }

    // see if we failed previously
    if (klass->IsErroneous()) {
      // might be wise to unlock before throwing; depends on which class
      // it is that we have locked

      // TODO: throwEarlierClassFailure(klass);
      return false;
    }

    if (!ValidateSuperClassDescriptors(klass)) {
      klass->SetStatus(Class::kStatusError);
      return false;
    }

    assert(klass->status < CLASS_INITIALIZING);

    klass->clinit_thread_id_ = self->GetId();
    klass->status_ = Class::kStatusInitializing;
  }

  if (!InitializeSuperClass(klass)) {
    return false;
  }

  InitializeStaticFields(klass);

  Method* clinit = klass->FindDirectMethodLocally("<clinit>", "()V");
  if (clinit != NULL) {
  } else {
    // JValue unused;
    // TODO: dvmCallMethod(self, method, NULL, &unused);
    //CHECK(!"unimplemented");
  }

  {
    ObjectLock lock(klass);

    if (self->IsExceptionPending()) {
      klass->SetStatus(Class::kStatusError);
    } else {
      klass->SetStatus(Class::kStatusInitialized);
    }
    lock.NotifyAll();
  }

  return true;
}

bool ClassLinker::ValidateSuperClassDescriptors(const Class* klass) {
  if (klass->IsInterface()) {
    return true;
  }
  // begin with the methods local to the superclass
  if (klass->HasSuperClass() &&
      klass->GetClassLoader() != klass->GetSuperClass()->GetClassLoader()) {
    const Class* super = klass->GetSuperClass();
    for (int i = super->NumVirtualMethods() - 1; i >= 0; --i) {
      const Method* method = klass->GetVirtualMethod(i);
      if (method != super->GetVirtualMethod(i) &&
          !HasSameMethodDescriptorClasses(method, super, klass)) {
        LG << "Classes resolve differently in superclass";
        return false;
      }
    }
  }
  for (size_t i = 0; i < klass->iftable_count_; ++i) {
    const InterfaceEntry* iftable = &klass->iftable_[i];
    Class* interface = iftable->GetClass();
    if (klass->GetClassLoader() != interface->GetClassLoader()) {
      for (size_t j = 0; j < interface->NumVirtualMethods(); ++j) {
        uint32_t vtable_index = iftable->method_index_array_[j];
        const Method* method = klass->GetVirtualMethod(vtable_index);
        if (!HasSameMethodDescriptorClasses(method, interface,
                                            method->GetClass())) {
          LG << "Classes resolve differently in interface";  // TODO: LinkageError
          return false;
        }
      }
    }
  }
  return true;
}

bool ClassLinker::HasSameMethodDescriptorClasses(const Method* method,
                                                 const Class* klass1,
                                                 const Class* klass2) {
  const RawDexFile* raw = method->GetClass()->GetDexFile()->GetRaw();
  const RawDexFile::ProtoId& proto_id = raw->GetProtoId(method->proto_idx_);
  RawDexFile::ParameterIterator *it;
  for (it = raw->GetParameterIterator(proto_id); it->HasNext(); it->Next()) {
    const char* descriptor = it->GetDescriptor();
    if (descriptor == NULL) {
      break;
    }
    if (descriptor[0] == 'L' || descriptor[0] == '[') {
      // Found a non-primitive type.
      if (!HasSameDescriptorClasses(descriptor, klass1, klass2)) {
        return false;
      }
    }
  }
  // Check the return type
  const char* descriptor = raw->GetReturnTypeDescriptor(proto_id);
  if (descriptor[0] == 'L' || descriptor[0] == '[') {
    if (HasSameDescriptorClasses(descriptor, klass1, klass2)) {
      return false;
    }
  }
  return true;
}

// Returns true if classes referenced by the descriptor are the
// same classes in klass1 as they are in klass2.
bool ClassLinker::HasSameDescriptorClasses(const char* descriptor,
                                           const Class* klass1,
                                           const Class* klass2) {
  CHECK(descriptor != NULL);
  CHECK(klass1 != NULL);
  CHECK(klass2 != NULL);
#if 0
  Class* found1 = FindClassNoInit(descriptor, klass1->GetClassLoader());
  // TODO: found1 == NULL
  Class* found2 = FindClassNoInit(descriptor, klass2->GetClassLoader());
  // TODO: found2 == NULL
  // TODO: lookup found1 in initiating loader list
  if (found1 == NULL || found2 == NULL) {
    Thread::Current()->ClearException();
    if (found1 == found2) {
      return true;
    } else {
      return false;
    }
  }
#endif
  return true;
}

bool ClassLinker::InitializeSuperClass(Class* klass) {
  CHECK(klass != NULL);
  // TODO: assert klass lock is acquired
  if (!klass->IsInterface() && klass->HasSuperClass()) {
    Class* super_class = klass->GetSuperClass();
    if (super_class->GetStatus() != Class::kStatusInitialized) {
      CHECK(!super_class->IsInterface());
      klass->MonitorExit();
      bool super_initialized = InitializeClass(super_class);
      klass->MonitorEnter();
      // TODO: check for a pending exception
      if (!super_initialized) {
        klass->SetStatus(Class::kStatusError);
        klass->NotifyAll();
        return false;
      }
    }
  }
  return true;
}

void ClassLinker::InitializeStaticFields(Class* klass) {
  size_t num_static_fields = klass->NumStaticFields();
  if (num_static_fields == 0) {
    return;
  }
  DexFile* dex_file = klass->GetDexFile();
  if (dex_file == NULL) {
    return;
  }
  const char* descriptor = klass->GetDescriptor().data();
  const RawDexFile* raw = dex_file->GetRaw();
  const RawDexFile::ClassDef* class_def = raw->FindClassDef(descriptor);
  CHECK(class_def != NULL);
  const byte* addr = raw->GetEncodedArray(*class_def);
  size_t array_size = DecodeUnsignedLeb128(&addr);
  for (size_t i = 0; i < array_size; ++i) {
    StaticField* field = klass->GetStaticField(i);
    JValue value;
    RawDexFile::ValueType type = raw->ReadEncodedValue(&addr, &value);
    switch (type) {
      case RawDexFile::kByte:
        field->SetByte(value.b);
        break;
      case RawDexFile::kShort:
        field->SetShort(value.s);
        break;
      case RawDexFile::kChar:
        field->SetChar(value.c);
        break;
      case RawDexFile::kInt:
        field->SetInt(value.i);
        break;
      case RawDexFile::kLong:
        field->SetLong(value.j);
        break;
      case RawDexFile::kFloat:
        field->SetFloat(value.f);
        break;
      case RawDexFile::kDouble:
        field->SetDouble(value.d);
        break;
      case RawDexFile::kString: {
        uint32_t string_idx = value.i;
        String* resolved = ResolveString(klass, string_idx);
        field->SetObject(resolved);
        break;
      }
      case RawDexFile::kBoolean:
        field->SetBoolean(value.z);
        break;
      case RawDexFile::kNull:
        field->SetObject(value.l);
        break;
      default:
        LOG(FATAL) << "Unknown type " << static_cast<int>(type);
    }
  }
}

bool ClassLinker::LinkClass(Class* klass) {
  CHECK(klass->status_ == Class::kStatusIdx ||
        klass->status_ == Class::kStatusLoaded);
  if (klass->status_ == Class::kStatusIdx) {
    if (!LinkInterfaces(klass)) {
      return false;
    }
  }
  if (!LinkSuperClass(klass)) {
    return false;
  }
  if (!LinkMethods(klass)) {
    return false;
  }
  if (!LinkInstanceFields(klass)) {
    return false;
  }
  CreateReferenceOffsets(klass);
  CHECK_EQ(klass->status_, Class::kStatusLoaded);
  klass->status_ = Class::kStatusResolved;
  return true;
}

bool ClassLinker::LinkInterfaces(Class* klass) {
  scoped_array<uint32_t> interfaces_idx;
  // TODO: store interfaces_idx in the Class object
  // TODO: move this outside of link interfaces
  if (klass->interface_count_ > 0) {
    size_t length = klass->interface_count_ * sizeof(klass->interfaces_[0]);
    interfaces_idx.reset(new uint32_t[klass->interface_count_]);
    memcpy(interfaces_idx.get(), klass->interfaces_, length);
    memset(klass->interfaces_, 0xFF, length);
  }
  // Mark the class as loaded.
  klass->status_ = Class::kStatusLoaded;
  if (klass->super_class_idx_ != RawDexFile::kDexNoIndex) {
    Class* super_class = ResolveClass(klass, klass->super_class_idx_);
    if (super_class == NULL) {
      LG << "Failed to resolve superclass";
      return false;
    }
    klass->super_class_ = super_class;  // TODO: write barrier
  }
  if (klass->interface_count_ > 0) {
    for (size_t i = 0; i < klass->interface_count_; ++i) {
      uint32_t idx = interfaces_idx[i];
      klass->interfaces_[i] = ResolveClass(klass, idx);
      if (klass->interfaces_[i] == NULL) {
        LG << "Failed to resolve interface";
        return false;
      }
      // Verify
      if (!klass->CanAccess(klass->interfaces_[i])) {
        LG << "Inaccessible interface";
        return false;
      }
    }
  }
  return true;
}

bool ClassLinker::LinkSuperClass(Class* klass) {
  CHECK(!klass->IsPrimitive());
  const Class* super = klass->GetSuperClass();
  if (klass->GetDescriptor() == "Ljava/lang/Object;") {
    if (super != NULL) {
      LG << "Superclass must not be defined";  // TODO: ClassFormatError
      return false;
    }
    // TODO: clear finalize attribute
    return true;
  }
  if (super == NULL) {
    LG << "No superclass defined";  // TODO: LinkageError
    return false;
  }
  // Verify
  if (super->IsFinal()) {
    LG << "Superclass is declared final";  // TODO: IncompatibleClassChangeError
    return false;
  }
  if (super->IsInterface()) {
    LG << "Superclass is an interface";  // TODO: IncompatibleClassChangeError
    return false;
  }
  if (!klass->CanAccess(super)) {
    LG << "Superclass is inaccessible";  // TODO: IllegalAccessError
    return false;
  }
  return true;
}

// Populate the class vtable and itable.
bool ClassLinker::LinkMethods(Class* klass) {
  if (klass->IsInterface()) {
    // No vtable.
    size_t count = klass->NumVirtualMethods();
    if (!IsUint(16, count)) {
      LG << "Too many methods on interface";  // TODO: VirtualMachineError
      return false;
    }
    for (size_t i = 0; i < count; ++i) {
      klass->GetVirtualMethod(i)->method_index_ = i;
    }
  } else {
    // Link virtual method tables
    LinkVirtualMethods(klass);

    // Link interface method tables
    LinkInterfaceMethods(klass);

    // Insert stubs.
    LinkAbstractMethods(klass);
  }
  return true;
}

bool ClassLinker::LinkVirtualMethods(Class* klass) {
  uint32_t max_count = klass->NumVirtualMethods();
  if (klass->GetSuperClass() != NULL) {
    max_count += klass->GetSuperClass()->NumVirtualMethods();
  } else {
    CHECK(klass->GetDescriptor() == "Ljava/lang/Object;");
  }
  // TODO: do not assign to the vtable field until it is fully constructed.
  // TODO: make this a vector<Method*> instead?
  klass->vtable_ = new Method*[max_count];
  if (klass->HasSuperClass()) {
    memcpy(klass->vtable_,
           klass->GetSuperClass()->vtable_,
           klass->GetSuperClass()->vtable_count_ * sizeof(Method*));
    size_t actual_count = klass->GetSuperClass()->vtable_count_;
    // See if any of our virtual methods override the superclass.
    for (size_t i = 0; i < klass->NumVirtualMethods(); ++i) {
      Method* local_method = klass->GetVirtualMethod(i);
      size_t j = 0;
      for (; j < klass->GetSuperClass()->vtable_count_; ++j) {
        const Method* super_method = klass->vtable_[j];
        if (local_method->HasSameNameAndPrototype(super_method)) {
          // Verify
          if (super_method->IsFinal()) {
            LG << "Method overrides final method"; // TODO: VirtualMachineError
            return false;
          }
          klass->vtable_[j] = local_method;
          local_method->method_index_ = j;
          break;
        }
      }
      if (j == klass->GetSuperClass()->vtable_count_) {
        // Not overriding, append.
        klass->vtable_[actual_count] = local_method;
        local_method->method_index_ = actual_count;
        actual_count += 1;
      }
    }
    if (!IsUint(16, actual_count)) {
      LG << "Too many methods defined on class";  // TODO: VirtualMachineError
      return false;
    }
    CHECK_LE(actual_count, max_count);
    if (actual_count < max_count) {
      Method** new_vtable = new Method*[actual_count];
      memcpy(new_vtable, klass->vtable_, actual_count * sizeof(Method*));
      delete[] klass->vtable_;
      klass->vtable_ = new_vtable;
      LG << "shrunk vtable: "
         << "was " << max_count << ", "
         << "now " << actual_count;
    }
    klass->vtable_count_ = actual_count;
  } else {
    CHECK(klass->GetDescriptor() == "Ljava/lang/Object;");
    if (!IsUint(16, klass->NumVirtualMethods())) {
      LG << "Too many methods";  // TODO: VirtualMachineError
      return false;
    }
    for (size_t i = 0; i < klass->NumVirtualMethods(); ++i) {
      klass->vtable_[i] = klass->GetVirtualMethod(i);
      klass->GetVirtualMethod(i)->method_index_ = i & 0xFFFF;
    }
    klass->vtable_count_ = klass->NumVirtualMethods();
  }
  return true;
}

bool ClassLinker::LinkInterfaceMethods(Class* klass) {
  int pool_offset = 0;
  int pool_size = 0;
  int miranda_count = 0;
  int miranda_alloc = 0;
  size_t super_ifcount;
  if (klass->HasSuperClass()) {
    super_ifcount = klass->GetSuperClass()->iftable_count_;
  } else {
    super_ifcount = 0;
  }
  size_t ifCount = super_ifcount;
  ifCount += klass->interface_count_;
  for (size_t i = 0; i < klass->interface_count_; i++) {
    ifCount += klass->interfaces_[i]->iftable_count_;
  }
  if (ifCount == 0) {
    assert(klass->iftable_count_ == 0);
    assert(klass->iftable == NULL);
    return true;
  }
  klass->iftable_ = new InterfaceEntry[ifCount * sizeof(InterfaceEntry)];
  memset(klass->iftable_, 0x00, sizeof(InterfaceEntry) * ifCount);
  if (super_ifcount != 0) {
    memcpy(klass->iftable_, klass->GetSuperClass()->iftable_,
           sizeof(InterfaceEntry) * super_ifcount);
  }
  // Flatten the interface inheritance hierarchy.
  size_t idx = super_ifcount;
  for (size_t i = 0; i < klass->interface_count_; i++) {
    Class* interf = klass->interfaces_[i];
    assert(interf != NULL);
    if (!interf->IsInterface()) {
      LG << "Class implements non-interface class";  // TODO: IncompatibleClassChangeError
      return false;
    }
    klass->iftable_[idx++].SetClass(interf);
    for (size_t j = 0; j < interf->iftable_count_; j++) {
      klass->iftable_[idx++].SetClass(interf->iftable_[j].GetClass());
    }
  }
  CHECK_EQ(idx, ifCount);
  klass->iftable_count_ = ifCount;
  if (klass->IsInterface() || super_ifcount == ifCount) {
    return true;
  }
  for (size_t i = super_ifcount; i < ifCount; i++) {
    pool_size += klass->iftable_[i].GetClass()->NumVirtualMethods();
  }
  if (pool_size == 0) {
    return true;
  }
  klass->ifvi_pool_count_ = pool_size;
  klass->ifvi_pool_ = new uint32_t[pool_size];
  std::vector<Method*> miranda_list;
  for (size_t i = super_ifcount; i < ifCount; ++i) {
    klass->iftable_[i].method_index_array_ = klass->ifvi_pool_ + pool_offset;
    Class* interface = klass->iftable_[i].GetClass();
    pool_offset += interface->NumVirtualMethods();    // end here
    for (size_t j = 0; j < interface->NumVirtualMethods(); ++j) {
      Method* interface_method = interface->GetVirtualMethod(j);
      int k;  // must be signed
      for (k = klass->vtable_count_ - 1; k >= 0; --k) {
        if (interface_method->HasSameNameAndPrototype(klass->vtable_[k])) {
          if (!klass->vtable_[k]->IsPublic()) {
            LG << "Implementation not public";
            return false;
          }
          klass->iftable_[i].method_index_array_[j] = k;
          break;
        }
      }
      if (k < 0) {
        if (miranda_count == miranda_alloc) {
          miranda_alloc += 8;
          if (miranda_list.empty()) {
            miranda_list.resize(miranda_alloc);
          } else {
            miranda_list.resize(miranda_alloc);
          }
        }
        int mir;
        for (mir = 0; mir < miranda_count; mir++) {
          if (miranda_list[mir]->HasSameNameAndPrototype(interface_method)) {
            break;
          }
        }
        // point the interface table at a phantom slot index
        klass->iftable_[i].method_index_array_[j] = klass->vtable_count_ + mir;
        if (mir == miranda_count) {
          miranda_list[miranda_count++] = interface_method;
        }
      }
    }
  }
  if (miranda_count != 0) {
    Method* newVirtualMethods;
    Method* meth;
    int oldMethodCount, oldVtableCount;
    if (klass->virtual_methods_ == NULL) {
      newVirtualMethods = new Method[klass->NumVirtualMethods() + miranda_count];

    } else {
      newVirtualMethods = new Method[klass->NumVirtualMethods() + miranda_count];
      memcpy(newVirtualMethods,
             klass->virtual_methods_,
             klass->NumVirtualMethods() * sizeof(Method));

    }
    if (newVirtualMethods != klass->virtual_methods_) {
      Method* meth = newVirtualMethods;
      for (size_t i = 0; i < klass->NumVirtualMethods(); i++, meth++) {
        klass->vtable_[meth->method_index_] = meth;
      }
    }
    oldMethodCount = klass->NumVirtualMethods();
    klass->virtual_methods_ = newVirtualMethods;
    klass->num_virtual_methods_ += miranda_count;

    CHECK(klass->vtable_ != NULL);
    oldVtableCount = klass->vtable_count_;
    klass->vtable_count_ += miranda_count;

    meth = klass->virtual_methods_ + oldMethodCount;
    for (int i = 0; i < miranda_count; i++, meth++) {
      memcpy(meth, miranda_list[i], sizeof(Method));
      meth->klass_ = klass;
      meth->access_flags_ |= kAccMiranda;
      meth->method_index_ = 0xFFFF & (oldVtableCount + i);
      klass->vtable_[oldVtableCount + i] = meth;
    }
  }
  return true;
}

void ClassLinker::LinkAbstractMethods(Class* klass) {
  for (size_t i = 0; i < klass->NumVirtualMethods(); ++i) {
    Method* method = klass->GetVirtualMethod(i);
    if (method->IsAbstract()) {
      method->insns_ = reinterpret_cast<uint16_t*>(0xFFFFFFFF);  // TODO: AbstractMethodError
    }
  }
}

bool ClassLinker::LinkInstanceFields(Class* klass) {
  int fieldOffset;
  if (klass->GetSuperClass() != NULL) {
    fieldOffset = klass->GetSuperClass()->object_size_;
  } else {
    fieldOffset = OFFSETOF_MEMBER(DataObject, fields_);
  }
  // Move references to the front.
  klass->num_reference_ifields_ = 0;
  size_t i = 0;
  size_t j = klass->NumInstanceFields() - 1;
  for (size_t i = 0; i < klass->NumInstanceFields(); i++) {
    InstanceField* pField = klass->GetInstanceField(i);
    char c = pField->GetType();

    if (c != '[' && c != 'L') {
      while (j > i) {
        InstanceField* refField = klass->GetInstanceField(j--);
        char rc = refField->GetType();
        if (rc == '[' || rc == 'L') {
          pField->Swap(refField);
          c = rc;
          klass->num_reference_ifields_++;
          break;
        }
      }
    } else {
      klass->num_reference_ifields_++;
    }
    if (c != '[' && c != 'L') {
      break;
    }
    pField->SetOffset(fieldOffset);
    fieldOffset += sizeof(uint32_t);
  }

  // Now we want to pack all of the double-wide fields together.  If
  // we're not aligned, though, we want to shuffle one 32-bit field
  // into place.  If we can't find one, we'll have to pad it.
  if (i != klass->NumInstanceFields() && (fieldOffset & 0x04) != 0) {
    InstanceField* pField = klass->GetInstanceField(i);
    char c = pField->GetType();

    if (c != 'J' && c != 'D') {
      // The field that comes next is 32-bit, so just advance past it.
      assert(c != '[' && c != 'L');
      pField->SetOffset(fieldOffset);
      fieldOffset += sizeof(uint32_t);
      i++;
    } else {
      // Next field is 64-bit, so search for a 32-bit field we can
      // swap into it.
      bool found = false;
      j = klass->NumInstanceFields() - 1;
      while (j > i) {
        InstanceField* singleField = klass->GetInstanceField(j--);
        char rc = singleField->GetType();
        if (rc != 'J' && rc != 'D') {
          pField->Swap(singleField);
          pField->SetOffset(fieldOffset);
          fieldOffset += sizeof(uint32_t);
          found = true;
          i++;
          break;
        }
      }
      if (!found) {
        fieldOffset += sizeof(uint32_t);
      }
    }
  }

  // Alignment is good, shuffle any double-wide fields forward, and
  // finish assigning field offsets to all fields.
  assert(i == klass->NumInstanceFields() || (fieldOffset & 0x04) == 0);
  j = klass->NumInstanceFields() - 1;
  for ( ; i < klass->NumInstanceFields(); i++) {
    InstanceField* pField = klass->GetInstanceField(i);
    char c = pField->GetType();
    if (c != 'D' && c != 'J') {
      while (j > i) {
        InstanceField* doubleField = klass->GetInstanceField(j--);
        char rc = doubleField->GetType();
        if (rc == 'D' || rc == 'J') {
          pField->Swap(doubleField);
          c = rc;
          break;
        }
      }
    } else {
      // This is a double-wide field, leave it be.
    }

    pField->SetOffset(fieldOffset);
    fieldOffset += sizeof(uint32_t);
    if (c == 'J' || c == 'D')
      fieldOffset += sizeof(uint32_t);
  }

#ifndef NDEBUG
  /* Make sure that all reference fields appear before
   * non-reference fields, and all double-wide fields are aligned.
   */
  j = 0;  // seen non-ref
  for (i = 0; i < klass->NumInstanceFields(); i++) {
    InstanceField *pField = &klass->ifields[i];
    char c = pField->GetType();

    if (c == 'D' || c == 'J') {
      assert((pField->offset_ & 0x07) == 0);
    }

    if (c != '[' && c != 'L') {
      if (!j) {
        assert(i == klass->num_reference_ifields_);
        j = 1;
      }
    } else if (j) {
      assert(false);
    }
  }
  if (!j) {
    assert(klass->num_reference_ifields_ == klass->NumInstanceFields());
  }
#endif

  klass->object_size_ = fieldOffset;
  return true;
}

//  Set the bitmap of reference offsets, refOffsets, from the ifields
//  list.
void ClassLinker::CreateReferenceOffsets(Class* klass) {
  uint32_t reference_offsets = 0;
  if (klass->HasSuperClass()) {
    reference_offsets = klass->GetSuperClass()->GetReferenceOffsets();
  }
  // If our superclass overflowed, we don't stand a chance.
  if (reference_offsets != CLASS_WALK_SUPER) {
    // All of the fields that contain object references are guaranteed
    // to be at the beginning of the ifields list.
    for (size_t i = 0; i < klass->NumReferenceInstanceFields(); ++i) {
      // Note that, per the comment on struct InstField, f->byteOffset
      // is the offset from the beginning of obj, not the offset into
      // obj->instanceData.
      const InstanceField* field = klass->GetInstanceField(i);
      size_t byte_offset = field->GetOffset();
      CHECK_GE(byte_offset, CLASS_SMALLEST_OFFSET);
      CHECK_EQ(byte_offset & (CLASS_OFFSET_ALIGNMENT - 1), 0);
      if (CLASS_CAN_ENCODE_OFFSET(byte_offset)) {
        uint32_t new_bit = CLASS_BIT_FROM_OFFSET(byte_offset);
        CHECK_NE(new_bit, 0);
        reference_offsets |= new_bit;
      } else {
        reference_offsets = CLASS_WALK_SUPER;
        break;
      }
    }
    klass->SetReferenceOffsets(reference_offsets);
  }
}

Class* ClassLinker::ResolveClass(const Class* referrer, uint32_t class_idx) {
  DexFile* dex_file = referrer->GetDexFile();
  Class* resolved = dex_file->GetResolvedClass(class_idx);
  if (resolved != NULL) {
    return resolved;
  }
  const char* descriptor = dex_file->GetRaw()->dexStringByTypeIdx(class_idx);
  if (descriptor[0] != '\0' && descriptor[1] == '\0') {
    JType type = static_cast<JType>(descriptor[0]);
    resolved = FindPrimitiveClass(type);
  } else {
    resolved = FindClass(descriptor, referrer->GetClassLoader(), NULL);
  }
  if (resolved != NULL) {
    Class* check = resolved->IsArray() ? resolved->component_type_ : resolved;
    if (referrer->GetDexFile() != check->GetDexFile()) {
      if (check->GetClassLoader() != NULL) {
        LG << "Class resolved by unexpected DEX";  // TODO: IllegalAccessError
        return NULL;
      }
    }
    dex_file->SetResolvedClass(resolved, class_idx);
  } else {
    CHECK(Thread::Current()->IsExceptionPending());
  }
  return resolved;
}

Method* ResolveMethod(const Class* referrer, uint32_t method_idx,
                      /*MethodType*/ int method_type) {
  CHECK(false);
  return NULL;
}

String* ClassLinker::ResolveString(const Class* referring, uint32_t string_idx) {
  const RawDexFile* raw = referring->GetDexFile()->GetRaw();
  const RawDexFile::StringId& string_id = raw->GetStringId(string_idx);
  const char* string_data = raw->GetStringData(string_id);
  String* new_string = Heap::AllocStringFromModifiedUtf8(string_data);
  // TODO: intern the new string
  referring->GetDexFile()->SetResolvedString(new_string, string_idx);
  return new_string;
}

}  // namespace art
