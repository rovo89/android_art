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

#include "class.h"

#include "art_field-inl.h"
#include "art_method-inl.h"
#include "class_linker.h"
#include "class_loader.h"
#include "class-inl.h"
#include "dex_cache.h"
#include "dex_file-inl.h"
#include "gc/accounting/card_table-inl.h"
#include "handle_scope-inl.h"
#include "object_array-inl.h"
#include "object-inl.h"
#include "runtime.h"
#include "thread.h"
#include "throwable.h"
#include "utils.h"
#include "well_known_classes.h"

namespace art {
namespace mirror {

GcRoot<Class> Class::java_lang_Class_;

void Class::SetClassClass(Class* java_lang_Class) {
  CHECK(java_lang_Class_.IsNull())
      << java_lang_Class_.Read()
      << " " << java_lang_Class;
  CHECK(java_lang_Class != nullptr);
  java_lang_Class_ = GcRoot<Class>(java_lang_Class);
}

void Class::ResetClass() {
  CHECK(!java_lang_Class_.IsNull());
  java_lang_Class_ = GcRoot<Class>(nullptr);
}

void Class::VisitRoots(RootCallback* callback, void* arg) {
  if (!java_lang_Class_.IsNull()) {
    java_lang_Class_.VisitRoot(callback, arg, 0, kRootStickyClass);
  }
}

void Class::SetStatus(Status new_status, Thread* self) {
  Status old_status = GetStatus();
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  bool class_linker_initialized = class_linker != nullptr && class_linker->IsInitialized();
  if (LIKELY(class_linker_initialized)) {
    if (UNLIKELY(new_status <= old_status && new_status != kStatusError &&
                 new_status != kStatusRetired)) {
      LOG(FATAL) << "Unexpected change back of class status for " << PrettyClass(this) << " "
          << old_status << " -> " << new_status;
    }
    if (new_status >= kStatusResolved || old_status >= kStatusResolved) {
      // When classes are being resolved the resolution code should hold the lock.
      CHECK_EQ(GetLockOwnerThreadId(), self->GetThreadId())
            << "Attempt to change status of class while not holding its lock: "
            << PrettyClass(this) << " " << old_status << " -> " << new_status;
    }
  }
  if (UNLIKELY(new_status == kStatusError)) {
    CHECK_NE(GetStatus(), kStatusError)
        << "Attempt to set as erroneous an already erroneous class " << PrettyClass(this);

    // Stash current exception.
    StackHandleScope<3> hs(self);
    ThrowLocation old_throw_location;
    Handle<mirror::Throwable> old_exception(hs.NewHandle(self->GetException(&old_throw_location)));
    CHECK(old_exception.Get() != nullptr);
    Handle<mirror::Object> old_throw_this_object(hs.NewHandle(old_throw_location.GetThis()));
    Handle<mirror::ArtMethod> old_throw_method(hs.NewHandle(old_throw_location.GetMethod()));
    uint32_t old_throw_dex_pc = old_throw_location.GetDexPc();
    bool is_exception_reported = self->IsExceptionReportedToInstrumentation();
    Class* eiie_class;
    // Do't attempt to use FindClass if we have an OOM error since this can try to do more
    // allocations and may cause infinite loops.
    bool throw_eiie = (old_exception.Get() == nullptr);
    if (!throw_eiie) {
      std::string temp;
      const char* old_exception_descriptor = old_exception->GetClass()->GetDescriptor(&temp);
      throw_eiie = (strcmp(old_exception_descriptor, "Ljava/lang/OutOfMemoryError;") != 0);
    }
    if (throw_eiie) {
      // Clear exception to call FindSystemClass.
      self->ClearException();
      eiie_class = Runtime::Current()->GetClassLinker()->FindSystemClass(
          self, "Ljava/lang/ExceptionInInitializerError;");
      CHECK(!self->IsExceptionPending());
      // Only verification errors, not initialization problems, should set a verify error.
      // This is to ensure that ThrowEarlierClassFailure will throw NoClassDefFoundError in that
      // case.
      Class* exception_class = old_exception->GetClass();
      if (!eiie_class->IsAssignableFrom(exception_class)) {
        SetVerifyErrorClass(exception_class);
      }
    }

    // Restore exception.
    ThrowLocation gc_safe_throw_location(old_throw_this_object.Get(), old_throw_method.Get(),
                                         old_throw_dex_pc);
    self->SetException(gc_safe_throw_location, old_exception.Get());
    self->SetExceptionReportedToInstrumentation(is_exception_reported);
  }
  COMPILE_ASSERT(sizeof(Status) == sizeof(uint32_t), size_of_status_not_uint32);
  if (Runtime::Current()->IsActiveTransaction()) {
    SetField32Volatile<true>(OFFSET_OF_OBJECT_MEMBER(Class, status_), new_status);
  } else {
    SetField32Volatile<false>(OFFSET_OF_OBJECT_MEMBER(Class, status_), new_status);
  }

  if (!class_linker_initialized) {
    // When the class linker is being initialized its single threaded and by definition there can be
    // no waiters. During initialization classes may appear temporary but won't be retired as their
    // size was statically computed.
  } else {
    // Classes that are being resolved or initialized need to notify waiters that the class status
    // changed. See ClassLinker::EnsureResolved and ClassLinker::WaitForInitializeClass.
    if (IsTemp()) {
      // Class is a temporary one, ensure that waiters for resolution get notified of retirement
      // so that they can grab the new version of the class from the class linker's table.
      CHECK_LT(new_status, kStatusResolved) << PrettyDescriptor(this);
      if (new_status == kStatusRetired || new_status == kStatusError) {
        NotifyAll(self);
      }
    } else {
      CHECK_NE(new_status, kStatusRetired);
      if (old_status >= kStatusResolved || new_status >= kStatusResolved) {
        NotifyAll(self);
      }
    }
  }
}

void Class::SetDexCache(DexCache* new_dex_cache) {
  SetFieldObject<false>(OFFSET_OF_OBJECT_MEMBER(Class, dex_cache_), new_dex_cache);
}

void Class::SetClassSize(uint32_t new_class_size) {
  if (kIsDebugBuild && (new_class_size < GetClassSize())) {
    DumpClass(LOG(ERROR), kDumpClassFullDetail);
    CHECK_GE(new_class_size, GetClassSize()) << " class=" << PrettyTypeOf(this);
  }
  // Not called within a transaction.
  SetField32<false>(OFFSET_OF_OBJECT_MEMBER(Class, class_size_), new_class_size);
}

// Return the class' name. The exact format is bizarre, but it's the specified behavior for
// Class.getName: keywords for primitive types, regular "[I" form for primitive arrays (so "int"
// but "[I"), and arrays of reference types written between "L" and ";" but with dots rather than
// slashes (so "java.lang.String" but "[Ljava.lang.String;"). Madness.
String* Class::ComputeName(Handle<Class> h_this) {
  String* name = h_this->GetName();
  if (name != nullptr) {
    return name;
  }
  std::string temp;
  const char* descriptor = h_this->GetDescriptor(&temp);
  Thread* self = Thread::Current();
  if ((descriptor[0] != 'L') && (descriptor[0] != '[')) {
    // The descriptor indicates that this is the class for
    // a primitive type; special-case the return value.
    const char* c_name = nullptr;
    switch (descriptor[0]) {
    case 'Z': c_name = "boolean"; break;
    case 'B': c_name = "byte";    break;
    case 'C': c_name = "char";    break;
    case 'S': c_name = "short";   break;
    case 'I': c_name = "int";     break;
    case 'J': c_name = "long";    break;
    case 'F': c_name = "float";   break;
    case 'D': c_name = "double";  break;
    case 'V': c_name = "void";    break;
    default:
      LOG(FATAL) << "Unknown primitive type: " << PrintableChar(descriptor[0]);
    }
    name = String::AllocFromModifiedUtf8(self, c_name);
  } else {
    // Convert the UTF-8 name to a java.lang.String. The name must use '.' to separate package
    // components.
    name = String::AllocFromModifiedUtf8(self, DescriptorToDot(descriptor).c_str());
  }
  h_this->SetName(name);
  return name;
}

void Class::DumpClass(std::ostream& os, int flags) {
  if ((flags & kDumpClassFullDetail) == 0) {
    os << PrettyClass(this);
    if ((flags & kDumpClassClassLoader) != 0) {
      os << ' ' << GetClassLoader();
    }
    if ((flags & kDumpClassInitialized) != 0) {
      os << ' ' << GetStatus();
    }
    os << "\n";
    return;
  }

  Thread* self = Thread::Current();
  StackHandleScope<2> hs(self);
  Handle<mirror::Class> h_this(hs.NewHandle(this));
  Handle<mirror::Class> h_super(hs.NewHandle(GetSuperClass()));

  std::string temp;
  os << "----- " << (IsInterface() ? "interface" : "class") << " "
     << "'" << GetDescriptor(&temp) << "' cl=" << GetClassLoader() << " -----\n",
  os << "  objectSize=" << SizeOf() << " "
     << "(" << (h_super.Get() != nullptr ? h_super->SizeOf() : -1) << " from super)\n",
  os << StringPrintf("  access=0x%04x.%04x\n",
      GetAccessFlags() >> 16, GetAccessFlags() & kAccJavaFlagsMask);
  if (h_super.Get() != nullptr) {
    os << "  super='" << PrettyClass(h_super.Get()) << "' (cl=" << h_super->GetClassLoader()
       << ")\n";
  }
  if (IsArrayClass()) {
    os << "  componentType=" << PrettyClass(GetComponentType()) << "\n";
  }
  const size_t num_direct_interfaces = NumDirectInterfaces();
  if (num_direct_interfaces > 0) {
    os << "  interfaces (" << num_direct_interfaces << "):\n";
    for (size_t i = 0; i < num_direct_interfaces; ++i) {
      Class* interface = GetDirectInterface(self, h_this, i);
      const ClassLoader* cl = interface->GetClassLoader();
      os << StringPrintf("    %2zd: %s (cl=%p)\n", i, PrettyClass(interface).c_str(), cl);
    }
  }
  if (!IsLoaded()) {
    os << "  class not yet loaded";
  } else {
    // After this point, this may have moved due to GetDirectInterface.
    os << "  vtable (" << h_this->NumVirtualMethods() << " entries, "
        << (h_super.Get() != nullptr ? h_super->NumVirtualMethods() : 0) << " in super):\n";
    for (size_t i = 0; i < NumVirtualMethods(); ++i) {
      os << StringPrintf("    %2zd: %s\n", i,
                         PrettyMethod(h_this->GetVirtualMethodDuringLinking(i)).c_str());
    }
    os << "  direct methods (" << h_this->NumDirectMethods() << " entries):\n";
    for (size_t i = 0; i < h_this->NumDirectMethods(); ++i) {
      os << StringPrintf("    %2zd: %s\n", i, PrettyMethod(h_this->GetDirectMethod(i)).c_str());
    }
    if (h_this->NumStaticFields() > 0) {
      os << "  static fields (" << h_this->NumStaticFields() << " entries):\n";
      if (h_this->IsResolved() || h_this->IsErroneous()) {
        for (size_t i = 0; i < h_this->NumStaticFields(); ++i) {
          os << StringPrintf("    %2zd: %s\n", i, PrettyField(h_this->GetStaticField(i)).c_str());
        }
      } else {
        os << "    <not yet available>";
      }
    }
    if (h_this->NumInstanceFields() > 0) {
      os << "  instance fields (" << h_this->NumInstanceFields() << " entries):\n";
      if (h_this->IsResolved() || h_this->IsErroneous()) {
        for (size_t i = 0; i < h_this->NumInstanceFields(); ++i) {
          os << StringPrintf("    %2zd: %s\n", i, PrettyField(h_this->GetInstanceField(i)).c_str());
        }
      } else {
        os << "    <not yet available>";
      }
    }
  }
}

void Class::SetReferenceInstanceOffsets(uint32_t new_reference_offsets) {
  if (new_reference_offsets != CLASS_WALK_SUPER) {
    // Sanity check that the number of bits set in the reference offset bitmap
    // agrees with the number of references
    size_t count = 0;
    for (Class* c = this; c != nullptr; c = c->GetSuperClass()) {
      count += c->NumReferenceInstanceFieldsDuringLinking();
    }
    CHECK_EQ((size_t)POPCOUNT(new_reference_offsets), count);
  }
  // Not called within a transaction.
  SetField32<false>(OFFSET_OF_OBJECT_MEMBER(Class, reference_instance_offsets_),
                    new_reference_offsets);
}

void Class::SetReferenceStaticOffsets(uint32_t new_reference_offsets) {
  if (new_reference_offsets != CLASS_WALK_SUPER) {
    // Sanity check that the number of bits set in the reference offset bitmap
    // agrees with the number of references
    CHECK_EQ((size_t)POPCOUNT(new_reference_offsets),
             NumReferenceStaticFieldsDuringLinking());
  }
  // Not called within a transaction.
  SetField32<false>(OFFSET_OF_OBJECT_MEMBER(Class, reference_static_offsets_),
                    new_reference_offsets);
}

bool Class::IsInSamePackage(const StringPiece& descriptor1, const StringPiece& descriptor2) {
  size_t i = 0;
  while (descriptor1[i] != '\0' && descriptor1[i] == descriptor2[i]) {
    ++i;
  }
  if (descriptor1.find('/', i) != StringPiece::npos ||
      descriptor2.find('/', i) != StringPiece::npos) {
    return false;
  } else {
    return true;
  }
}

bool Class::IsInSamePackage(Class* that) {
  Class* klass1 = this;
  Class* klass2 = that;
  if (klass1 == klass2) {
    return true;
  }
  // Class loaders must match.
  if (klass1->GetClassLoader() != klass2->GetClassLoader()) {
    return false;
  }
  // Arrays are in the same package when their element classes are.
  while (klass1->IsArrayClass()) {
    klass1 = klass1->GetComponentType();
  }
  while (klass2->IsArrayClass()) {
    klass2 = klass2->GetComponentType();
  }
  // trivial check again for array types
  if (klass1 == klass2) {
    return true;
  }
  // Compare the package part of the descriptor string.
  std::string temp1, temp2;
  return IsInSamePackage(klass1->GetDescriptor(&temp1), klass2->GetDescriptor(&temp2));
}

bool Class::IsStringClass() const {
  return this == String::GetJavaLangString();
}

bool Class::IsThrowableClass() {
  return WellKnownClasses::ToClass(WellKnownClasses::java_lang_Throwable)->IsAssignableFrom(this);
}

void Class::SetClassLoader(ClassLoader* new_class_loader) {
  if (Runtime::Current()->IsActiveTransaction()) {
    SetFieldObject<true>(OFFSET_OF_OBJECT_MEMBER(Class, class_loader_), new_class_loader);
  } else {
    SetFieldObject<false>(OFFSET_OF_OBJECT_MEMBER(Class, class_loader_), new_class_loader);
  }
}

ArtMethod* Class::FindInterfaceMethod(const StringPiece& name, const StringPiece& signature) {
  // Check the current class before checking the interfaces.
  ArtMethod* method = FindDeclaredVirtualMethod(name, signature);
  if (method != nullptr) {
    return method;
  }

  int32_t iftable_count = GetIfTableCount();
  IfTable* iftable = GetIfTable();
  for (int32_t i = 0; i < iftable_count; ++i) {
    method = iftable->GetInterface(i)->FindDeclaredVirtualMethod(name, signature);
    if (method != nullptr) {
      return method;
    }
  }
  return nullptr;
}

ArtMethod* Class::FindInterfaceMethod(const StringPiece& name, const Signature& signature) {
  // Check the current class before checking the interfaces.
  ArtMethod* method = FindDeclaredVirtualMethod(name, signature);
  if (method != nullptr) {
    return method;
  }

  int32_t iftable_count = GetIfTableCount();
  IfTable* iftable = GetIfTable();
  for (int32_t i = 0; i < iftable_count; ++i) {
    method = iftable->GetInterface(i)->FindDeclaredVirtualMethod(name, signature);
    if (method != nullptr) {
      return method;
    }
  }
  return nullptr;
}

ArtMethod* Class::FindInterfaceMethod(const DexCache* dex_cache, uint32_t dex_method_idx) {
  // Check the current class before checking the interfaces.
  ArtMethod* method = FindDeclaredVirtualMethod(dex_cache, dex_method_idx);
  if (method != nullptr) {
    return method;
  }

  int32_t iftable_count = GetIfTableCount();
  IfTable* iftable = GetIfTable();
  for (int32_t i = 0; i < iftable_count; ++i) {
    method = iftable->GetInterface(i)->FindDeclaredVirtualMethod(dex_cache, dex_method_idx);
    if (method != nullptr) {
      return method;
    }
  }
  return nullptr;
}

ArtMethod* Class::FindDeclaredDirectMethod(const StringPiece& name, const StringPiece& signature) {
  for (size_t i = 0; i < NumDirectMethods(); ++i) {
    ArtMethod* method = GetDirectMethod(i);
    if (name == method->GetName() && method->GetSignature() == signature) {
      return method;
    }
  }
  return nullptr;
}

ArtMethod* Class::FindDeclaredDirectMethod(const StringPiece& name, const Signature& signature) {
  for (size_t i = 0; i < NumDirectMethods(); ++i) {
    ArtMethod* method = GetDirectMethod(i);
    if (name == method->GetName() && signature == method->GetSignature()) {
      return method;
    }
  }
  return nullptr;
}

ArtMethod* Class::FindDeclaredDirectMethod(const DexCache* dex_cache, uint32_t dex_method_idx) {
  if (GetDexCache() == dex_cache) {
    for (size_t i = 0; i < NumDirectMethods(); ++i) {
      ArtMethod* method = GetDirectMethod(i);
      if (method->GetDexMethodIndex() == dex_method_idx) {
        return method;
      }
    }
  }
  return nullptr;
}

ArtMethod* Class::FindDirectMethod(const StringPiece& name, const StringPiece& signature) {
  for (Class* klass = this; klass != nullptr; klass = klass->GetSuperClass()) {
    ArtMethod* method = klass->FindDeclaredDirectMethod(name, signature);
    if (method != nullptr) {
      return method;
    }
  }
  return nullptr;
}

ArtMethod* Class::FindDirectMethod(const StringPiece& name, const Signature& signature) {
  for (Class* klass = this; klass != nullptr; klass = klass->GetSuperClass()) {
    ArtMethod* method = klass->FindDeclaredDirectMethod(name, signature);
    if (method != nullptr) {
      return method;
    }
  }
  return nullptr;
}

ArtMethod* Class::FindDirectMethod(const DexCache* dex_cache, uint32_t dex_method_idx) {
  for (Class* klass = this; klass != nullptr; klass = klass->GetSuperClass()) {
    ArtMethod* method = klass->FindDeclaredDirectMethod(dex_cache, dex_method_idx);
    if (method != nullptr) {
      return method;
    }
  }
  return nullptr;
}

ArtMethod* Class::FindDeclaredVirtualMethod(const StringPiece& name, const StringPiece& signature) {
  for (size_t i = 0; i < NumVirtualMethods(); ++i) {
    ArtMethod* method = GetVirtualMethod(i);
    if (name == method->GetName() && method->GetSignature() == signature) {
      return method;
    }
  }
  return nullptr;
}

ArtMethod* Class::FindDeclaredVirtualMethod(const StringPiece& name, const Signature& signature) {
  for (size_t i = 0; i < NumVirtualMethods(); ++i) {
    ArtMethod* method = GetVirtualMethod(i);
    if (name == method->GetName() && signature == method->GetSignature()) {
      return method;
    }
  }
  return nullptr;
}

ArtMethod* Class::FindDeclaredVirtualMethod(const DexCache* dex_cache, uint32_t dex_method_idx) {
  if (GetDexCache() == dex_cache) {
    for (size_t i = 0; i < NumVirtualMethods(); ++i) {
      ArtMethod* method = GetVirtualMethod(i);
      if (method->GetDexMethodIndex() == dex_method_idx) {
        return method;
      }
    }
  }
  return nullptr;
}

ArtMethod* Class::FindVirtualMethod(const StringPiece& name, const StringPiece& signature) {
  for (Class* klass = this; klass != nullptr; klass = klass->GetSuperClass()) {
    ArtMethod* method = klass->FindDeclaredVirtualMethod(name, signature);
    if (method != nullptr) {
      return method;
    }
  }
  return nullptr;
}

ArtMethod* Class::FindVirtualMethod(const StringPiece& name, const Signature& signature) {
  for (Class* klass = this; klass != nullptr; klass = klass->GetSuperClass()) {
    ArtMethod* method = klass->FindDeclaredVirtualMethod(name, signature);
    if (method != nullptr) {
      return method;
    }
  }
  return nullptr;
}

ArtMethod* Class::FindVirtualMethod(const DexCache* dex_cache, uint32_t dex_method_idx) {
  for (Class* klass = this; klass != nullptr; klass = klass->GetSuperClass()) {
    ArtMethod* method = klass->FindDeclaredVirtualMethod(dex_cache, dex_method_idx);
    if (method != nullptr) {
      return method;
    }
  }
  return nullptr;
}

ArtMethod* Class::FindClassInitializer() {
  for (size_t i = 0; i < NumDirectMethods(); ++i) {
    ArtMethod* method = GetDirectMethod(i);
    if (method->IsClassInitializer()) {
      DCHECK_STREQ(method->GetName(), "<clinit>");
      DCHECK_STREQ(method->GetSignature().ToString().c_str(), "()V");
      return method;
    }
  }
  return nullptr;
}

ArtField* Class::FindDeclaredInstanceField(const StringPiece& name, const StringPiece& type) {
  // Is the field in this class?
  // Interfaces are not relevant because they can't contain instance fields.
  for (size_t i = 0; i < NumInstanceFields(); ++i) {
    ArtField* f = GetInstanceField(i);
    if (name == f->GetName() && type == f->GetTypeDescriptor()) {
      return f;
    }
  }
  return nullptr;
}

ArtField* Class::FindDeclaredInstanceField(const DexCache* dex_cache, uint32_t dex_field_idx) {
  if (GetDexCache() == dex_cache) {
    for (size_t i = 0; i < NumInstanceFields(); ++i) {
      ArtField* f = GetInstanceField(i);
      if (f->GetDexFieldIndex() == dex_field_idx) {
        return f;
      }
    }
  }
  return nullptr;
}

ArtField* Class::FindInstanceField(const StringPiece& name, const StringPiece& type) {
  // Is the field in this class, or any of its superclasses?
  // Interfaces are not relevant because they can't contain instance fields.
  for (Class* c = this; c != nullptr; c = c->GetSuperClass()) {
    ArtField* f = c->FindDeclaredInstanceField(name, type);
    if (f != nullptr) {
      return f;
    }
  }
  return nullptr;
}

ArtField* Class::FindInstanceField(const DexCache* dex_cache, uint32_t dex_field_idx) {
  // Is the field in this class, or any of its superclasses?
  // Interfaces are not relevant because they can't contain instance fields.
  for (Class* c = this; c != nullptr; c = c->GetSuperClass()) {
    ArtField* f = c->FindDeclaredInstanceField(dex_cache, dex_field_idx);
    if (f != nullptr) {
      return f;
    }
  }
  return nullptr;
}

ArtField* Class::FindDeclaredStaticField(const StringPiece& name, const StringPiece& type) {
  DCHECK(type != nullptr);
  for (size_t i = 0; i < NumStaticFields(); ++i) {
    ArtField* f = GetStaticField(i);
    if (name == f->GetName() && type == f->GetTypeDescriptor()) {
      return f;
    }
  }
  return nullptr;
}

ArtField* Class::FindDeclaredStaticField(const DexCache* dex_cache, uint32_t dex_field_idx) {
  if (dex_cache == GetDexCache()) {
    for (size_t i = 0; i < NumStaticFields(); ++i) {
      ArtField* f = GetStaticField(i);
      if (f->GetDexFieldIndex() == dex_field_idx) {
        return f;
      }
    }
  }
  return nullptr;
}

ArtField* Class::FindStaticField(Thread* self, Handle<Class> klass, const StringPiece& name,
                                 const StringPiece& type) {
  // Is the field in this class (or its interfaces), or any of its
  // superclasses (or their interfaces)?
  for (Class* k = klass.Get(); k != nullptr; k = k->GetSuperClass()) {
    // Is the field in this class?
    ArtField* f = k->FindDeclaredStaticField(name, type);
    if (f != nullptr) {
      return f;
    }
    // Wrap k incase it moves during GetDirectInterface.
    StackHandleScope<1> hs(self);
    HandleWrapper<mirror::Class> h_k(hs.NewHandleWrapper(&k));
    // Is this field in any of this class' interfaces?
    for (uint32_t i = 0; i < h_k->NumDirectInterfaces(); ++i) {
      StackHandleScope<1> hs(self);
      Handle<mirror::Class> interface(hs.NewHandle(GetDirectInterface(self, h_k, i)));
      f = FindStaticField(self, interface, name, type);
      if (f != nullptr) {
        return f;
      }
    }
  }
  return nullptr;
}

ArtField* Class::FindStaticField(Thread* self, Handle<Class> klass, const DexCache* dex_cache,
                                 uint32_t dex_field_idx) {
  for (Class* k = klass.Get(); k != nullptr; k = k->GetSuperClass()) {
    // Is the field in this class?
    ArtField* f = k->FindDeclaredStaticField(dex_cache, dex_field_idx);
    if (f != nullptr) {
      return f;
    }
    // Wrap k incase it moves during GetDirectInterface.
    StackHandleScope<1> hs(self);
    HandleWrapper<mirror::Class> h_k(hs.NewHandleWrapper(&k));
    // Is this field in any of this class' interfaces?
    for (uint32_t i = 0; i < h_k->NumDirectInterfaces(); ++i) {
      StackHandleScope<1> hs(self);
      Handle<mirror::Class> interface(hs.NewHandle(GetDirectInterface(self, h_k, i)));
      f = FindStaticField(self, interface, dex_cache, dex_field_idx);
      if (f != nullptr) {
        return f;
      }
    }
  }
  return nullptr;
}

ArtField* Class::FindField(Thread* self, Handle<Class> klass, const StringPiece& name,
                           const StringPiece& type) {
  // Find a field using the JLS field resolution order
  for (Class* k = klass.Get(); k != nullptr; k = k->GetSuperClass()) {
    // Is the field in this class?
    ArtField* f = k->FindDeclaredInstanceField(name, type);
    if (f != nullptr) {
      return f;
    }
    f = k->FindDeclaredStaticField(name, type);
    if (f != nullptr) {
      return f;
    }
    // Is this field in any of this class' interfaces?
    StackHandleScope<1> hs(self);
    HandleWrapper<mirror::Class> h_k(hs.NewHandleWrapper(&k));
    for (uint32_t i = 0; i < h_k->NumDirectInterfaces(); ++i) {
      StackHandleScope<1> hs(self);
      Handle<mirror::Class> interface(hs.NewHandle(GetDirectInterface(self, h_k, i)));
      f = interface->FindStaticField(self, interface, name, type);
      if (f != nullptr) {
        return f;
      }
    }
  }
  return nullptr;
}

static void SetPreverifiedFlagOnMethods(mirror::ObjectArray<mirror::ArtMethod>* methods)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (methods != nullptr) {
    for (int32_t index = 0, end = methods->GetLength(); index < end; ++index) {
      mirror::ArtMethod* method = methods->GetWithoutChecks(index);
      DCHECK(method != nullptr);
      if (!method->IsNative() && !method->IsAbstract()) {
        method->SetPreverified();
      }
    }
  }
}

void Class::SetPreverifiedFlagOnAllMethods() {
  DCHECK(IsVerified());
  SetPreverifiedFlagOnMethods(GetDirectMethods());
  SetPreverifiedFlagOnMethods(GetVirtualMethods());
}

const char* Class::GetDescriptor(std::string* storage) {
  if (IsPrimitive()) {
    return Primitive::Descriptor(GetPrimitiveType());
  } else if (IsArrayClass()) {
    return GetArrayDescriptor(storage);
  } else if (IsProxyClass()) {
    *storage = Runtime::Current()->GetClassLinker()->GetDescriptorForProxy(this);
    return storage->c_str();
  } else {
    const DexFile& dex_file = GetDexFile();
    const DexFile::TypeId& type_id = dex_file.GetTypeId(GetClassDef()->class_idx_);
    return dex_file.GetTypeDescriptor(type_id);
  }
}

const char* Class::GetArrayDescriptor(std::string* storage) {
  std::string temp;
  const char* elem_desc = GetComponentType()->GetDescriptor(&temp);
  *storage = "[";
  *storage += elem_desc;
  return storage->c_str();
}

const DexFile::ClassDef* Class::GetClassDef() {
  uint16_t class_def_idx = GetDexClassDefIndex();
  if (class_def_idx == DexFile::kDexNoIndex16) {
    return nullptr;
  }
  return &GetDexFile().GetClassDef(class_def_idx);
}

uint32_t Class::NumDirectInterfaces() {
  if (IsPrimitive()) {
    return 0;
  } else if (IsArrayClass()) {
    return 2;
  } else if (IsProxyClass()) {
    mirror::ObjectArray<mirror::Class>* interfaces = GetInterfaces();
    return interfaces != nullptr ? interfaces->GetLength() : 0;
  } else {
    const DexFile::TypeList* interfaces = GetInterfaceTypeList();
    if (interfaces == nullptr) {
      return 0;
    } else {
      return interfaces->Size();
    }
  }
}

uint16_t Class::GetDirectInterfaceTypeIdx(uint32_t idx) {
  DCHECK(!IsPrimitive());
  DCHECK(!IsArrayClass());
  return GetInterfaceTypeList()->GetTypeItem(idx).type_idx_;
}

mirror::Class* Class::GetDirectInterface(Thread* self, Handle<mirror::Class> klass, uint32_t idx) {
  DCHECK(klass.Get() != nullptr);
  DCHECK(!klass->IsPrimitive());
  if (klass->IsArrayClass()) {
    ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
    if (idx == 0) {
      return class_linker->FindSystemClass(self, "Ljava/lang/Cloneable;");
    } else {
      DCHECK_EQ(1U, idx);
      return class_linker->FindSystemClass(self, "Ljava/io/Serializable;");
    }
  } else if (klass->IsProxyClass()) {
    mirror::ObjectArray<mirror::Class>* interfaces = klass.Get()->GetInterfaces();
    DCHECK(interfaces != nullptr);
    return interfaces->Get(idx);
  } else {
    uint16_t type_idx = klass->GetDirectInterfaceTypeIdx(idx);
    mirror::Class* interface = klass->GetDexCache()->GetResolvedType(type_idx);
    if (interface == nullptr) {
      interface = Runtime::Current()->GetClassLinker()->ResolveType(klass->GetDexFile(), type_idx,
                                                                    klass.Get());
      CHECK(interface != nullptr || self->IsExceptionPending());
    }
    return interface;
  }
}

const char* Class::GetSourceFile() {
  const DexFile& dex_file = GetDexFile();
  const DexFile::ClassDef* dex_class_def = GetClassDef();
  if (dex_class_def == nullptr) {
    // Generated classes have no class def.
    return nullptr;
  }
  return dex_file.GetSourceFile(*dex_class_def);
}

std::string Class::GetLocation() {
  mirror::DexCache* dex_cache = GetDexCache();
  if (dex_cache != nullptr && !IsProxyClass()) {
    return dex_cache->GetLocation()->ToModifiedUtf8();
  }
  // Arrays and proxies are generated and have no corresponding dex file location.
  return "generated class";
}

const DexFile::TypeList* Class::GetInterfaceTypeList() {
  const DexFile::ClassDef* class_def = GetClassDef();
  if (class_def == nullptr) {
    return nullptr;
  }
  return GetDexFile().GetInterfacesList(*class_def);
}

void Class::PopulateEmbeddedImtAndVTable() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ObjectArray<ArtMethod>* table = GetImTable();
  if (table != nullptr) {
    for (uint32_t i = 0; i < kImtSize; i++) {
      SetEmbeddedImTableEntry(i, table->Get(i));
    }
  }

  table = GetVTableDuringLinking();
  CHECK(table != nullptr) << PrettyClass(this);
  SetEmbeddedVTableLength(table->GetLength());
  for (int32_t i = 0; i < table->GetLength(); i++) {
    SetEmbeddedVTableEntry(i, table->Get(i));
  }

  SetImTable(nullptr);
  // Keep java.lang.Object class's vtable around for since it's easier
  // to be reused by array classes during their linking.
  if (!IsObjectClass()) {
    SetVTable(nullptr);
  }
}

// The pre-fence visitor for Class::CopyOf().
class CopyClassVisitor {
 public:
  explicit CopyClassVisitor(Thread* self, Handle<mirror::Class>* orig,
                            size_t new_length, size_t copy_bytes)
      : self_(self), orig_(orig), new_length_(new_length),
        copy_bytes_(copy_bytes) {
  }

  void operator()(Object* obj, size_t usable_size) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    UNUSED(usable_size);
    mirror::Class* new_class_obj = obj->AsClass();
    mirror::Object::CopyObject(self_, new_class_obj, orig_->Get(), copy_bytes_);
    new_class_obj->SetStatus(Class::kStatusResolving, self_);
    new_class_obj->PopulateEmbeddedImtAndVTable();
    new_class_obj->SetClassSize(new_length_);
  }

 private:
  Thread* const self_;
  Handle<mirror::Class>* const orig_;
  const size_t new_length_;
  const size_t copy_bytes_;
  DISALLOW_COPY_AND_ASSIGN(CopyClassVisitor);
};

Class* Class::CopyOf(Thread* self, int32_t new_length) {
  DCHECK_GE(new_length, static_cast<int32_t>(sizeof(Class)));
  // We may get copied by a compacting GC.
  StackHandleScope<1> hs(self);
  Handle<mirror::Class> h_this(hs.NewHandle(this));
  gc::Heap* heap = Runtime::Current()->GetHeap();
  // The num_bytes (3rd param) is sizeof(Class) as opposed to SizeOf()
  // to skip copying the tail part that we will overwrite here.
  CopyClassVisitor visitor(self, &h_this, new_length, sizeof(Class));

  mirror::Object* new_class =
      kMovingClasses
         ? heap->AllocObject<true>(self, java_lang_Class_.Read(), new_length, visitor)
         : heap->AllocNonMovableObject<true>(self, java_lang_Class_.Read(), new_length, visitor);
  if (UNLIKELY(new_class == nullptr)) {
    CHECK(self->IsExceptionPending());  // Expect an OOME.
    return NULL;
  }

  return new_class->AsClass();
}

}  // namespace mirror
}  // namespace art
