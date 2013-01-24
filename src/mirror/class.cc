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

#include "abstract_method-inl.h"
#include "class-inl.h"
#include "class_linker.h"
#include "class_loader.h"
#include "dex_cache.h"
#include "field-inl.h"
#include "gc/card_table-inl.h"
#include "object-inl.h"
#include "object_array-inl.h"
#include "object_utils.h"
#include "runtime.h"
#include "sirt_ref.h"
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

void Class::SetStatus(Status new_status) {
  CHECK(new_status > GetStatus() || new_status == kStatusError || !Runtime::Current()->IsStarted())
      << PrettyClass(this) << " " << GetStatus() << " -> " << new_status;
  CHECK(sizeof(Status) == sizeof(uint32_t)) << PrettyClass(this);
  if (new_status > kStatusResolved) {
    CHECK_EQ(GetThinLockId(), Thread::Current()->GetThinLockId()) << PrettyClass(this);
  }
  if (new_status == kStatusError) {
    CHECK_NE(GetStatus(), kStatusError) << PrettyClass(this);

    // stash current exception
    Thread* self = Thread::Current();
    SirtRef<Throwable> exception(self, self->GetException());
    CHECK(exception.get() != NULL);

    // clear exception to call FindSystemClass
    self->ClearException();
    ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
    Class* eiie_class = class_linker->FindSystemClass("Ljava/lang/ExceptionInInitializerError;");
    CHECK(!self->IsExceptionPending());

    // only verification errors, not initialization problems, should set a verify error.
    // this is to ensure that ThrowEarlierClassFailure will throw NoClassDefFoundError in that case.
    Class* exception_class = exception->GetClass();
    if (!eiie_class->IsAssignableFrom(exception_class)) {
      SetVerifyErrorClass(exception_class);
    }

    // restore exception
    self->SetException(exception.get());
  }
  return SetField32(OFFSET_OF_OBJECT_MEMBER(Class, status_), new_status, false);
}

DexCache* Class::GetDexCache() const {
  return GetFieldObject<DexCache*>(OFFSET_OF_OBJECT_MEMBER(Class, dex_cache_), false);
}

void Class::SetDexCache(DexCache* new_dex_cache) {
  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Class, dex_cache_), new_dex_cache, false);
}

Object* Class::AllocObject(Thread* self) {
  DCHECK(!IsArrayClass()) << PrettyClass(this);
  DCHECK(IsInstantiable()) << PrettyClass(this);
  // TODO: decide whether we want this check. It currently fails during bootstrap.
  // DCHECK(!Runtime::Current()->IsStarted() || IsInitializing()) << PrettyClass(this);
  DCHECK_GE(this->object_size_, sizeof(Object));
  return Runtime::Current()->GetHeap()->AllocObject(self, this, this->object_size_);
}

void Class::SetClassSize(size_t new_class_size) {
  DCHECK_GE(new_class_size, GetClassSize()) << " class=" << PrettyTypeOf(this);
  SetField32(OFFSET_OF_OBJECT_MEMBER(Class, class_size_), new_class_size, false);
}

// Return the class' name. The exact format is bizarre, but it's the specified behavior for
// Class.getName: keywords for primitive types, regular "[I" form for primitive arrays (so "int"
// but "[I"), and arrays of reference types written between "L" and ";" but with dots rather than
// slashes (so "java.lang.String" but "[Ljava.lang.String;"). Madness.
String* Class::ComputeName() {
  String* name = GetName();
  if (name != NULL) {
    return name;
  }
  std::string descriptor(ClassHelper(this).GetDescriptor());
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
    name = String::AllocFromModifiedUtf8(Thread::Current(), c_name);
  } else {
    // Convert the UTF-8 name to a java.lang.String. The name must use '.' to separate package
    // components.
    if (descriptor.size() > 2 && descriptor[0] == 'L' && descriptor[descriptor.size() - 1] == ';') {
      descriptor.erase(0, 1);
      descriptor.erase(descriptor.size() - 1);
    }
    std::replace(descriptor.begin(), descriptor.end(), '/', '.');
    name = String::AllocFromModifiedUtf8(Thread::Current(), descriptor.c_str());
  }
  SetName(name);
  return name;
}

void Class::DumpClass(std::ostream& os, int flags) const {
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

  Class* super = GetSuperClass();
  ClassHelper kh(this);
  os << "----- " << (IsInterface() ? "interface" : "class") << " "
     << "'" << kh.GetDescriptor() << "' cl=" << GetClassLoader() << " -----\n",
  os << "  objectSize=" << SizeOf() << " "
     << "(" << (super != NULL ? super->SizeOf() : -1) << " from super)\n",
  os << StringPrintf("  access=0x%04x.%04x\n",
      GetAccessFlags() >> 16, GetAccessFlags() & kAccJavaFlagsMask);
  if (super != NULL) {
    os << "  super='" << PrettyClass(super) << "' (cl=" << super->GetClassLoader() << ")\n";
  }
  if (IsArrayClass()) {
    os << "  componentType=" << PrettyClass(GetComponentType()) << "\n";
  }
  if (kh.NumDirectInterfaces() > 0) {
    os << "  interfaces (" << kh.NumDirectInterfaces() << "):\n";
    for (size_t i = 0; i < kh.NumDirectInterfaces(); ++i) {
      Class* interface = kh.GetDirectInterface(i);
      const ClassLoader* cl = interface->GetClassLoader();
      os << StringPrintf("    %2zd: %s (cl=%p)\n", i, PrettyClass(interface).c_str(), cl);
    }
  }
  os << "  vtable (" << NumVirtualMethods() << " entries, "
     << (super != NULL ? super->NumVirtualMethods() : 0) << " in super):\n";
  for (size_t i = 0; i < NumVirtualMethods(); ++i) {
    os << StringPrintf("    %2zd: %s\n", i, PrettyMethod(GetVirtualMethodDuringLinking(i)).c_str());
  }
  os << "  direct methods (" << NumDirectMethods() << " entries):\n";
  for (size_t i = 0; i < NumDirectMethods(); ++i) {
    os << StringPrintf("    %2zd: %s\n", i, PrettyMethod(GetDirectMethod(i)).c_str());
  }
  if (NumStaticFields() > 0) {
    os << "  static fields (" << NumStaticFields() << " entries):\n";
    if (IsResolved() || IsErroneous()) {
      for (size_t i = 0; i < NumStaticFields(); ++i) {
        os << StringPrintf("    %2zd: %s\n", i, PrettyField(GetStaticField(i)).c_str());
      }
    } else {
      os << "    <not yet available>";
    }
  }
  if (NumInstanceFields() > 0) {
    os << "  instance fields (" << NumInstanceFields() << " entries):\n";
    if (IsResolved() || IsErroneous()) {
      for (size_t i = 0; i < NumInstanceFields(); ++i) {
        os << StringPrintf("    %2zd: %s\n", i, PrettyField(GetInstanceField(i)).c_str());
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
    CHECK_EQ((size_t)__builtin_popcount(new_reference_offsets), count);
  }
  SetField32(OFFSET_OF_OBJECT_MEMBER(Class, reference_instance_offsets_),
             new_reference_offsets, false);
}

void Class::SetReferenceStaticOffsets(uint32_t new_reference_offsets) {
  if (new_reference_offsets != CLASS_WALK_SUPER) {
    // Sanity check that the number of bits set in the reference offset bitmap
    // agrees with the number of references
    CHECK_EQ((size_t)__builtin_popcount(new_reference_offsets),
             NumReferenceStaticFieldsDuringLinking());
  }
  SetField32(OFFSET_OF_OBJECT_MEMBER(Class, reference_static_offsets_),
             new_reference_offsets, false);
}

bool Class::Implements(const Class* klass) const {
  DCHECK(klass != NULL);
  DCHECK(klass->IsInterface()) << PrettyClass(this);
  // All interfaces implemented directly and by our superclass, and
  // recursively all super-interfaces of those interfaces, are listed
  // in iftable_, so we can just do a linear scan through that.
  int32_t iftable_count = GetIfTableCount();
  IfTable* iftable = GetIfTable();
  for (int32_t i = 0; i < iftable_count; i++) {
    if (iftable->GetInterface(i) == klass) {
      return true;
    }
  }
  return false;
}

// Determine whether "this" is assignable from "src", where both of these
// are array classes.
//
// Consider an array class, e.g. Y[][], where Y is a subclass of X.
//   Y[][]            = Y[][] --> true (identity)
//   X[][]            = Y[][] --> true (element superclass)
//   Y                = Y[][] --> false
//   Y[]              = Y[][] --> false
//   Object           = Y[][] --> true (everything is an object)
//   Object[]         = Y[][] --> true
//   Object[][]       = Y[][] --> true
//   Object[][][]     = Y[][] --> false (too many []s)
//   Serializable     = Y[][] --> true (all arrays are Serializable)
//   Serializable[]   = Y[][] --> true
//   Serializable[][] = Y[][] --> false (unless Y is Serializable)
//
// Don't forget about primitive types.
//   Object[]         = int[] --> false
//
bool Class::IsArrayAssignableFromArray(const Class* src) const {
  DCHECK(IsArrayClass())  << PrettyClass(this);
  DCHECK(src->IsArrayClass()) << PrettyClass(src);
  return GetComponentType()->IsAssignableFrom(src->GetComponentType());
}

bool Class::IsAssignableFromArray(const Class* src) const {
  DCHECK(!IsInterface()) << PrettyClass(this);  // handled first in IsAssignableFrom
  DCHECK(src->IsArrayClass()) << PrettyClass(src);
  if (!IsArrayClass()) {
    // If "this" is not also an array, it must be Object.
    // src's super should be java_lang_Object, since it is an array.
    Class* java_lang_Object = src->GetSuperClass();
    DCHECK(java_lang_Object != NULL) << PrettyClass(src);
    DCHECK(java_lang_Object->GetSuperClass() == NULL) << PrettyClass(src);
    return this == java_lang_Object;
  }
  return IsArrayAssignableFromArray(src);
}

bool Class::IsSubClass(const Class* klass) const {
  DCHECK(!IsInterface()) << PrettyClass(this);
  DCHECK(!IsArrayClass()) << PrettyClass(this);
  const Class* current = this;
  do {
    if (current == klass) {
      return true;
    }
    current = current->GetSuperClass();
  } while (current != NULL);
  return false;
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

bool Class::IsInSamePackage(const Class* that) const {
  const Class* klass1 = this;
  const Class* klass2 = that;
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
  // Compare the package part of the descriptor string.
  ClassHelper kh(klass1);
  std::string descriptor1(kh.GetDescriptor());
  kh.ChangeClass(klass2);
  std::string descriptor2(kh.GetDescriptor());
  return IsInSamePackage(descriptor1, descriptor2);
}

bool Class::IsClassClass() const {
  Class* java_lang_Class = GetClass()->GetClass();
  return this == java_lang_Class;
}

bool Class::IsStringClass() const {
  return this == String::GetJavaLangString();
}

bool Class::IsThrowableClass() const {
  return WellKnownClasses::ToClass(WellKnownClasses::java_lang_Throwable)->IsAssignableFrom(this);
}

bool Class::IsFieldClass() const {
  Class* java_lang_Class = GetClass();
  Class* java_lang_reflect_Field = java_lang_Class->GetInstanceField(0)->GetClass();
  return this == java_lang_reflect_Field;

}

bool Class::IsMethodClass() const {
  return (this == AbstractMethod::GetMethodClass()) ||
      (this == AbstractMethod::GetConstructorClass());

}

ClassLoader* Class::GetClassLoader() const {
  return GetFieldObject<ClassLoader*>(OFFSET_OF_OBJECT_MEMBER(Class, class_loader_), false);
}

void Class::SetClassLoader(ClassLoader* new_class_loader) {
  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Class, class_loader_), new_class_loader, false);
}

AbstractMethod* Class::FindVirtualMethodForInterface(AbstractMethod* method) const {
  Class* declaring_class = method->GetDeclaringClass();
  DCHECK(declaring_class != NULL) << PrettyClass(this);
  DCHECK(declaring_class->IsInterface()) << PrettyMethod(method);
  // TODO cache to improve lookup speed
  int32_t iftable_count = GetIfTableCount();
  IfTable* iftable = GetIfTable();
  for (int32_t i = 0; i < iftable_count; i++) {
    if (iftable->GetInterface(i) == declaring_class) {
      return iftable->GetMethodArray(i)->Get(method->GetMethodIndex());
    }
  }
  return NULL;
}

AbstractMethod* Class::FindInterfaceMethod(const StringPiece& name, const StringPiece& signature) const {
  // Check the current class before checking the interfaces.
  AbstractMethod* method = FindDeclaredVirtualMethod(name, signature);
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

AbstractMethod* Class::FindInterfaceMethod(const DexCache* dex_cache, uint32_t dex_method_idx) const {
  // Check the current class before checking the interfaces.
  AbstractMethod* method = FindDeclaredVirtualMethod(dex_cache, dex_method_idx);
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


AbstractMethod* Class::FindDeclaredDirectMethod(const StringPiece& name, const StringPiece& signature) const {
  MethodHelper mh;
  for (size_t i = 0; i < NumDirectMethods(); ++i) {
    AbstractMethod* method = GetDirectMethod(i);
    mh.ChangeMethod(method);
    if (name == mh.GetName() && signature == mh.GetSignature()) {
      return method;
    }
  }
  return NULL;
}

AbstractMethod* Class::FindDeclaredDirectMethod(const DexCache* dex_cache, uint32_t dex_method_idx) const {
  if (GetDexCache() == dex_cache) {
    for (size_t i = 0; i < NumDirectMethods(); ++i) {
      AbstractMethod* method = GetDirectMethod(i);
      if (method->GetDexMethodIndex() == dex_method_idx) {
        return method;
      }
    }
  }
  return NULL;
}

AbstractMethod* Class::FindDirectMethod(const StringPiece& name, const StringPiece& signature) const {
  for (const Class* klass = this; klass != NULL; klass = klass->GetSuperClass()) {
    AbstractMethod* method = klass->FindDeclaredDirectMethod(name, signature);
    if (method != NULL) {
      return method;
    }
  }
  return NULL;
}

AbstractMethod* Class::FindDirectMethod(const DexCache* dex_cache, uint32_t dex_method_idx) const {
  for (const Class* klass = this; klass != NULL; klass = klass->GetSuperClass()) {
    AbstractMethod* method = klass->FindDeclaredDirectMethod(dex_cache, dex_method_idx);
    if (method != NULL) {
      return method;
    }
  }
  return NULL;
}

AbstractMethod* Class::FindDeclaredVirtualMethod(const StringPiece& name,
                                         const StringPiece& signature) const {
  MethodHelper mh;
  for (size_t i = 0; i < NumVirtualMethods(); ++i) {
    AbstractMethod* method = GetVirtualMethod(i);
    mh.ChangeMethod(method);
    if (name == mh.GetName() && signature == mh.GetSignature()) {
      return method;
    }
  }
  return NULL;
}

AbstractMethod* Class::FindDeclaredVirtualMethod(const DexCache* dex_cache, uint32_t dex_method_idx) const {
  if (GetDexCache() == dex_cache) {
    for (size_t i = 0; i < NumVirtualMethods(); ++i) {
      AbstractMethod* method = GetVirtualMethod(i);
      if (method->GetDexMethodIndex() == dex_method_idx) {
        return method;
      }
    }
  }
  return NULL;
}

AbstractMethod* Class::FindVirtualMethod(const StringPiece& name, const StringPiece& signature) const {
  for (const Class* klass = this; klass != NULL; klass = klass->GetSuperClass()) {
    AbstractMethod* method = klass->FindDeclaredVirtualMethod(name, signature);
    if (method != NULL) {
      return method;
    }
  }
  return NULL;
}

AbstractMethod* Class::FindVirtualMethod(const DexCache* dex_cache, uint32_t dex_method_idx) const {
  for (const Class* klass = this; klass != NULL; klass = klass->GetSuperClass()) {
    AbstractMethod* method = klass->FindDeclaredVirtualMethod(dex_cache, dex_method_idx);
    if (method != NULL) {
      return method;
    }
  }
  return NULL;
}

Field* Class::FindDeclaredInstanceField(const StringPiece& name, const StringPiece& type) {
  // Is the field in this class?
  // Interfaces are not relevant because they can't contain instance fields.
  FieldHelper fh;
  for (size_t i = 0; i < NumInstanceFields(); ++i) {
    Field* f = GetInstanceField(i);
    fh.ChangeField(f);
    if (name == fh.GetName() && type == fh.GetTypeDescriptor()) {
      return f;
    }
  }
  return NULL;
}

Field* Class::FindDeclaredInstanceField(const DexCache* dex_cache, uint32_t dex_field_idx) {
  if (GetDexCache() == dex_cache) {
    for (size_t i = 0; i < NumInstanceFields(); ++i) {
      Field* f = GetInstanceField(i);
      if (f->GetDexFieldIndex() == dex_field_idx) {
        return f;
      }
    }
  }
  return NULL;
}

Field* Class::FindInstanceField(const StringPiece& name, const StringPiece& type) {
  // Is the field in this class, or any of its superclasses?
  // Interfaces are not relevant because they can't contain instance fields.
  for (Class* c = this; c != NULL; c = c->GetSuperClass()) {
    Field* f = c->FindDeclaredInstanceField(name, type);
    if (f != NULL) {
      return f;
    }
  }
  return NULL;
}

Field* Class::FindInstanceField(const DexCache* dex_cache, uint32_t dex_field_idx) {
  // Is the field in this class, or any of its superclasses?
  // Interfaces are not relevant because they can't contain instance fields.
  for (Class* c = this; c != NULL; c = c->GetSuperClass()) {
    Field* f = c->FindDeclaredInstanceField(dex_cache, dex_field_idx);
    if (f != NULL) {
      return f;
    }
  }
  return NULL;
}

Field* Class::FindDeclaredStaticField(const StringPiece& name, const StringPiece& type) {
  DCHECK(type != NULL);
  FieldHelper fh;
  for (size_t i = 0; i < NumStaticFields(); ++i) {
    Field* f = GetStaticField(i);
    fh.ChangeField(f);
    if (name == fh.GetName() && type == fh.GetTypeDescriptor()) {
      return f;
    }
  }
  return NULL;
}

Field* Class::FindDeclaredStaticField(const DexCache* dex_cache, uint32_t dex_field_idx) {
  if (dex_cache == GetDexCache()) {
    for (size_t i = 0; i < NumStaticFields(); ++i) {
      Field* f = GetStaticField(i);
      if (f->GetDexFieldIndex() == dex_field_idx) {
        return f;
      }
    }
  }
  return NULL;
}

Field* Class::FindStaticField(const StringPiece& name, const StringPiece& type) {
  // Is the field in this class (or its interfaces), or any of its
  // superclasses (or their interfaces)?
  ClassHelper kh;
  for (Class* k = this; k != NULL; k = k->GetSuperClass()) {
    // Is the field in this class?
    Field* f = k->FindDeclaredStaticField(name, type);
    if (f != NULL) {
      return f;
    }
    // Is this field in any of this class' interfaces?
    kh.ChangeClass(k);
    for (uint32_t i = 0; i < kh.NumDirectInterfaces(); ++i) {
      Class* interface = kh.GetDirectInterface(i);
      f = interface->FindStaticField(name, type);
      if (f != NULL) {
        return f;
      }
    }
  }
  return NULL;
}

Field* Class::FindStaticField(const DexCache* dex_cache, uint32_t dex_field_idx) {
  ClassHelper kh;
  for (Class* k = this; k != NULL; k = k->GetSuperClass()) {
    // Is the field in this class?
    Field* f = k->FindDeclaredStaticField(dex_cache, dex_field_idx);
    if (f != NULL) {
      return f;
    }
    // Is this field in any of this class' interfaces?
    kh.ChangeClass(k);
    for (uint32_t i = 0; i < kh.NumDirectInterfaces(); ++i) {
      Class* interface = kh.GetDirectInterface(i);
      f = interface->FindStaticField(dex_cache, dex_field_idx);
      if (f != NULL) {
        return f;
      }
    }
  }
  return NULL;
}

Field* Class::FindField(const StringPiece& name, const StringPiece& type) {
  // Find a field using the JLS field resolution order
  ClassHelper kh;
  for (Class* k = this; k != NULL; k = k->GetSuperClass()) {
    // Is the field in this class?
    Field* f = k->FindDeclaredInstanceField(name, type);
    if (f != NULL) {
      return f;
    }
    f = k->FindDeclaredStaticField(name, type);
    if (f != NULL) {
      return f;
    }
    // Is this field in any of this class' interfaces?
    kh.ChangeClass(k);
    for (uint32_t i = 0; i < kh.NumDirectInterfaces(); ++i) {
      Class* interface = kh.GetDirectInterface(i);
      f = interface->FindStaticField(name, type);
      if (f != NULL) {
        return f;
      }
    }
  }
  return NULL;
}

}  // namespace mirror
}  // namespace art
