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
#include "class-inl.h"
#include "class_linker.h"
#include "class_loader.h"
#include "dex_cache.h"
#include "dex_file-inl.h"
#include "gc/accounting/card_table-inl.h"
#include "object-inl.h"
#include "object_array-inl.h"
#include "object_utils.h"
#include "runtime.h"
#include "handle_scope-inl.h"
#include "thread.h"
#include "throwable.h"
#include "utils.h"
#include "well_known_classes.h"

namespace art {
namespace mirror {

Class* Class::java_lang_Class_ = NULL;

void Class::SetClassClass(Class* java_lang_Class) {
  CHECK(java_lang_Class_ == NULL) << java_lang_Class_ << " " << java_lang_Class;
  CHECK(java_lang_Class != NULL);
  java_lang_Class_ = java_lang_Class;
}

void Class::ResetClass() {
  CHECK(java_lang_Class_ != NULL);
  java_lang_Class_ = NULL;
}

void Class::VisitRoots(RootCallback* callback, void* arg) {
  if (java_lang_Class_ != nullptr) {
    callback(reinterpret_cast<mirror::Object**>(&java_lang_Class_), arg, 0, kRootStickyClass);
  }
}

void Class::SetStatus(Status new_status, Thread* self) {
  Status old_status = GetStatus();
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  bool class_linker_initialized = class_linker != nullptr && class_linker->IsInitialized();
  if (LIKELY(class_linker_initialized)) {
    if (UNLIKELY(new_status <= old_status && new_status != kStatusError)) {
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

    // clear exception to call FindSystemClass
    self->ClearException();
    ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
    Class* eiie_class = class_linker->FindSystemClass(self,
                                                      "Ljava/lang/ExceptionInInitializerError;");
    CHECK(!self->IsExceptionPending());

    // Only verification errors, not initialization problems, should set a verify error.
    // This is to ensure that ThrowEarlierClassFailure will throw NoClassDefFoundError in that case.
    Class* exception_class = old_exception->GetClass();
    if (!eiie_class->IsAssignableFrom(exception_class)) {
      SetVerifyErrorClass(exception_class);
    }

    // Restore exception.
    ThrowLocation gc_safe_throw_location(old_throw_this_object.Get(), old_throw_method.Get(),
                                         old_throw_dex_pc);

    self->SetException(gc_safe_throw_location, old_exception.Get());
  }
  CHECK(sizeof(Status) == sizeof(uint32_t)) << PrettyClass(this);
  if (Runtime::Current()->IsActiveTransaction()) {
    SetField32<true>(OFFSET_OF_OBJECT_MEMBER(Class, status_), new_status);
  } else {
    SetField32<false>(OFFSET_OF_OBJECT_MEMBER(Class, status_), new_status);
  }
  // Classes that are being resolved or initialized need to notify waiters that the class status
  // changed. See ClassLinker::EnsureResolved and ClassLinker::WaitForInitializeClass.
  if ((old_status >= kStatusResolved || new_status >= kStatusResolved) &&
      class_linker_initialized) {
    NotifyAll(self);
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
  std::string descriptor(h_this->GetDescriptor());
  Thread* self = Thread::Current();
  if ((descriptor[0] != 'L') && (descriptor[0] != '[')) {
    // The descriptor indicates that this is the class for
    // a primitive type; special-case the return value.
    const char* c_name = NULL;
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
    if (descriptor.size() > 2 && descriptor[0] == 'L' && descriptor[descriptor.size() - 1] == ';') {
      descriptor.erase(0, 1);
      descriptor.erase(descriptor.size() - 1);
    }
    std::replace(descriptor.begin(), descriptor.end(), '/', '.');
    name = String::AllocFromModifiedUtf8(self, descriptor.c_str());
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

  os << "----- " << (IsInterface() ? "interface" : "class") << " "
     << "'" << GetDescriptor() << "' cl=" << GetClassLoader() << " -----\n",
  os << "  objectSize=" << SizeOf() << " "
     << "(" << (h_super.Get() != NULL ? h_super->SizeOf() : -1) << " from super)\n",
  os << StringPrintf("  access=0x%04x.%04x\n",
      GetAccessFlags() >> 16, GetAccessFlags() & kAccJavaFlagsMask);
  if (h_super.Get() != NULL) {
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
  // After this point, this may have moved due to GetDirectInterface.
  os << "  vtable (" << h_this->NumVirtualMethods() << " entries, "
     << (h_super.Get() != NULL ? h_super->NumVirtualMethods() : 0) << " in super):\n";
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

void Class::SetReferenceInstanceOffsets(uint32_t new_reference_offsets) {
  if (new_reference_offsets != CLASS_WALK_SUPER) {
    // Sanity check that the number of bits set in the reference offset bitmap
    // agrees with the number of references
    size_t count = 0;
    for (Class* c = this; c != NULL; c = c->GetSuperClass()) {
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
  return IsInSamePackage(klass1->GetDescriptor().c_str(), klass2->GetDescriptor().c_str());
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

ArtMethod* Class::FindInterfaceMethod(const StringPiece& name, const Signature& signature) {
  // Check the current class before checking the interfaces.
  ArtMethod* method = FindDeclaredVirtualMethod(name, signature);
  if (method != NULL) {
    return method;
  }

  int32_t iftable_count = GetIfTableCount();
  IfTable* iftable = GetIfTable();
  for (int32_t i = 0; i < iftable_count; i++) {
    method = iftable->GetInterface(i)->FindDeclaredVirtualMethod(name, signature);
    if (method != NULL) {
      return method;
    }
  }
  return NULL;
}

ArtMethod* Class::FindInterfaceMethod(const DexCache* dex_cache, uint32_t dex_method_idx) {
  // Check the current class before checking the interfaces.
  ArtMethod* method = FindDeclaredVirtualMethod(dex_cache, dex_method_idx);
  if (method != NULL) {
    return method;
  }

  int32_t iftable_count = GetIfTableCount();
  IfTable* iftable = GetIfTable();
  for (int32_t i = 0; i < iftable_count; i++) {
    method = iftable->GetInterface(i)->FindDeclaredVirtualMethod(dex_cache, dex_method_idx);
    if (method != NULL) {
      return method;
    }
  }
  return NULL;
}

ArtMethod* Class::FindDeclaredDirectMethod(const StringPiece& name, const StringPiece& signature) {
  MethodHelper mh;
  for (size_t i = 0; i < NumDirectMethods(); ++i) {
    ArtMethod* method = GetDirectMethod(i);
    mh.ChangeMethod(method);
    if (name == mh.GetName() && mh.GetSignature() == signature) {
      return method;
    }
  }
  return NULL;
}

ArtMethod* Class::FindDeclaredDirectMethod(const StringPiece& name, const Signature& signature) {
  MethodHelper mh;
  for (size_t i = 0; i < NumDirectMethods(); ++i) {
    ArtMethod* method = GetDirectMethod(i);
    mh.ChangeMethod(method);
    if (name == mh.GetName() && signature == mh.GetSignature()) {
      return method;
    }
  }
  return NULL;
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
  return NULL;
}

ArtMethod* Class::FindDirectMethod(const StringPiece& name, const StringPiece& signature) {
  for (Class* klass = this; klass != NULL; klass = klass->GetSuperClass()) {
    ArtMethod* method = klass->FindDeclaredDirectMethod(name, signature);
    if (method != NULL) {
      return method;
    }
  }
  return NULL;
}

ArtMethod* Class::FindDirectMethod(const StringPiece& name, const Signature& signature) {
  for (Class* klass = this; klass != NULL; klass = klass->GetSuperClass()) {
    ArtMethod* method = klass->FindDeclaredDirectMethod(name, signature);
    if (method != NULL) {
      return method;
    }
  }
  return NULL;
}

ArtMethod* Class::FindDirectMethod(const DexCache* dex_cache, uint32_t dex_method_idx) {
  for (Class* klass = this; klass != NULL; klass = klass->GetSuperClass()) {
    ArtMethod* method = klass->FindDeclaredDirectMethod(dex_cache, dex_method_idx);
    if (method != NULL) {
      return method;
    }
  }
  return NULL;
}

ArtMethod* Class::FindDeclaredVirtualMethod(const StringPiece& name, const StringPiece& signature) {
  MethodHelper mh;
  for (size_t i = 0; i < NumVirtualMethods(); ++i) {
    ArtMethod* method = GetVirtualMethod(i);
    mh.ChangeMethod(method);
    if (name == mh.GetName() && mh.GetSignature() == signature) {
      return method;
    }
  }
  return NULL;
}

ArtMethod* Class::FindDeclaredVirtualMethod(const StringPiece& name,
                                            const Signature& signature) {
  MethodHelper mh;
  for (size_t i = 0; i < NumVirtualMethods(); ++i) {
    ArtMethod* method = GetVirtualMethod(i);
    mh.ChangeMethod(method);
    if (name == mh.GetName() && signature == mh.GetSignature()) {
      return method;
    }
  }
  return NULL;
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
  return NULL;
}

ArtMethod* Class::FindVirtualMethod(const StringPiece& name, const StringPiece& signature) {
  for (Class* klass = this; klass != NULL; klass = klass->GetSuperClass()) {
    ArtMethod* method = klass->FindDeclaredVirtualMethod(name, signature);
    if (method != NULL) {
      return method;
    }
  }
  return NULL;
}

ArtMethod* Class::FindVirtualMethod(const StringPiece& name, const Signature& signature) {
  for (Class* klass = this; klass != NULL; klass = klass->GetSuperClass()) {
    ArtMethod* method = klass->FindDeclaredVirtualMethod(name, signature);
    if (method != NULL) {
      return method;
    }
  }
  return NULL;
}

ArtMethod* Class::FindVirtualMethod(const DexCache* dex_cache, uint32_t dex_method_idx) {
  for (Class* klass = this; klass != NULL; klass = klass->GetSuperClass()) {
    ArtMethod* method = klass->FindDeclaredVirtualMethod(dex_cache, dex_method_idx);
    if (method != NULL) {
      return method;
    }
  }
  return NULL;
}

ArtMethod* Class::FindClassInitializer() {
  for (size_t i = 0; i < NumDirectMethods(); ++i) {
    ArtMethod* method = GetDirectMethod(i);
    if (method->IsConstructor() && method->IsStatic()) {
      if (kIsDebugBuild) {
        MethodHelper mh(method);
        CHECK(mh.IsClassInitializer());
        CHECK_STREQ(mh.GetName(), "<clinit>");
        CHECK_STREQ(mh.GetSignature().ToString().c_str(), "()V");
      }
      return method;
    }
  }
  return NULL;
}

ArtField* Class::FindDeclaredInstanceField(const StringPiece& name, const StringPiece& type) {
  // Is the field in this class?
  // Interfaces are not relevant because they can't contain instance fields.
  FieldHelper fh;
  for (size_t i = 0; i < NumInstanceFields(); ++i) {
    ArtField* f = GetInstanceField(i);
    fh.ChangeField(f);
    if (name == fh.GetName() && type == fh.GetTypeDescriptor()) {
      return f;
    }
  }
  return NULL;
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
  return NULL;
}

ArtField* Class::FindInstanceField(const StringPiece& name, const StringPiece& type) {
  // Is the field in this class, or any of its superclasses?
  // Interfaces are not relevant because they can't contain instance fields.
  for (Class* c = this; c != NULL; c = c->GetSuperClass()) {
    ArtField* f = c->FindDeclaredInstanceField(name, type);
    if (f != NULL) {
      return f;
    }
  }
  return NULL;
}

ArtField* Class::FindInstanceField(const DexCache* dex_cache, uint32_t dex_field_idx) {
  // Is the field in this class, or any of its superclasses?
  // Interfaces are not relevant because they can't contain instance fields.
  for (Class* c = this; c != NULL; c = c->GetSuperClass()) {
    ArtField* f = c->FindDeclaredInstanceField(dex_cache, dex_field_idx);
    if (f != NULL) {
      return f;
    }
  }
  return NULL;
}

ArtField* Class::FindDeclaredStaticField(const StringPiece& name, const StringPiece& type) {
  DCHECK(type != NULL);
  FieldHelper fh;
  for (size_t i = 0; i < NumStaticFields(); ++i) {
    ArtField* f = GetStaticField(i);
    fh.ChangeField(f);
    if (name == fh.GetName() && type == fh.GetTypeDescriptor()) {
      return f;
    }
  }
  return NULL;
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
  return NULL;
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
    if (f != NULL) {
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
  for (Class* k = klass.Get(); k != NULL; k = k->GetSuperClass()) {
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
  if (methods != NULL) {
    for (int32_t index = 0, end = methods->GetLength(); index < end; ++index) {
      mirror::ArtMethod* method = methods->GetWithoutChecks(index);
      DCHECK(method != NULL);
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

std::string Class::GetDescriptor() {
  if (UNLIKELY(IsArrayClass())) {
    return GetArrayDescriptor();
  } else if (UNLIKELY(IsPrimitive())) {
    return Primitive::Descriptor(GetPrimitiveType());
  } else if (UNLIKELY(IsProxyClass())) {
    return Runtime::Current()->GetClassLinker()->GetDescriptorForProxy(this);
  } else {
    const DexFile& dex_file = GetDexFile();
    const DexFile::TypeId& type_id = dex_file.GetTypeId(GetClassDef()->class_idx_);
    return dex_file.GetTypeDescriptor(type_id);
  }
}

std::string Class::GetArrayDescriptor() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return "[" + GetComponentType()->GetDescriptor();
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
    mirror::SynthesizedProxyClass* proxy_class=
        reinterpret_cast<mirror::SynthesizedProxyClass*>(this);
    mirror::ObjectArray<mirror::Class>* interfaces = proxy_class->GetInterfaces();
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
    mirror::SynthesizedProxyClass* proxy_class =
        reinterpret_cast<mirror::SynthesizedProxyClass*>(klass.Get());
    mirror::ObjectArray<mirror::Class>* interfaces = proxy_class->GetInterfaces();
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
  std::string descriptor(GetDescriptor());
  const DexFile& dex_file = GetDexFile();
  const DexFile::ClassDef* dex_class_def = GetClassDef();
  CHECK(dex_class_def != nullptr) << "No class def for class " << PrettyClass(this);
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

}  // namespace mirror
}  // namespace art
