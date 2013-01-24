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

#ifndef ART_SRC_MIRROR_CLASS_INL_H_
#define ART_SRC_MIRROR_CLASS_INL_H_

#include "class.h"

#include "abstract_method.h"
#include "field.h"
#include "iftable.h"
#include "object_array.h"
#include "runtime.h"
#include "string.h"

namespace art {
namespace mirror {

inline size_t Class::GetObjectSize() const {
  CHECK(!IsVariableSize()) << " class=" << PrettyTypeOf(this);
  DCHECK_EQ(sizeof(size_t), sizeof(int32_t));
  size_t result = GetField32(OFFSET_OF_OBJECT_MEMBER(Class, object_size_), false);
  CHECK_GE(result, sizeof(Object)) << " class=" << PrettyTypeOf(this);
  return result;
}

inline Class* Class::GetSuperClass() const {
  // Can only get super class for loaded classes (hack for when runtime is
  // initializing)
  DCHECK(IsLoaded() || !Runtime::Current()->IsStarted()) << IsLoaded();
  return GetFieldObject<Class*>(OFFSET_OF_OBJECT_MEMBER(Class, super_class_), false);
}

inline ObjectArray<AbstractMethod>* Class::GetDirectMethods() const {
  DCHECK(IsLoaded() || IsErroneous());
  return GetFieldObject<ObjectArray<AbstractMethod>*>(
      OFFSET_OF_OBJECT_MEMBER(Class, direct_methods_), false);
}

inline void Class::SetDirectMethods(ObjectArray<AbstractMethod>* new_direct_methods)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DCHECK(NULL == GetFieldObject<ObjectArray<AbstractMethod>*>(
      OFFSET_OF_OBJECT_MEMBER(Class, direct_methods_), false));
  DCHECK_NE(0, new_direct_methods->GetLength());
  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Class, direct_methods_),
                 new_direct_methods, false);
}

inline AbstractMethod* Class::GetDirectMethod(int32_t i) const
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return GetDirectMethods()->Get(i);
}

inline void Class::SetDirectMethod(uint32_t i, AbstractMethod* f)  // TODO: uint16_t
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_){
  ObjectArray<AbstractMethod>* direct_methods =
      GetFieldObject<ObjectArray<AbstractMethod>*>(
          OFFSET_OF_OBJECT_MEMBER(Class, direct_methods_), false);
  direct_methods->Set(i, f);
}

// Returns the number of static, private, and constructor methods.
inline size_t Class::NumDirectMethods() const {
  return (GetDirectMethods() != NULL) ? GetDirectMethods()->GetLength() : 0;
}

inline ObjectArray<AbstractMethod>* Class::GetVirtualMethods() const {
  DCHECK(IsLoaded() || IsErroneous());
  return GetFieldObject<ObjectArray<AbstractMethod>*>(
      OFFSET_OF_OBJECT_MEMBER(Class, virtual_methods_), false);
}

inline void Class::SetVirtualMethods(ObjectArray<AbstractMethod>* new_virtual_methods) {
  // TODO: we reassign virtual methods to grow the table for miranda
  // methods.. they should really just be assigned once
  DCHECK_NE(0, new_virtual_methods->GetLength());
  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Class, virtual_methods_),
                 new_virtual_methods, false);
}

inline size_t Class::NumVirtualMethods() const {
  return (GetVirtualMethods() != NULL) ? GetVirtualMethods()->GetLength() : 0;
}

inline AbstractMethod* Class::GetVirtualMethod(uint32_t i) const
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DCHECK(IsResolved() || IsErroneous());
  return GetVirtualMethods()->Get(i);
}

inline AbstractMethod* Class::GetVirtualMethodDuringLinking(uint32_t i) const
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DCHECK(IsLoaded() || IsErroneous());
  return GetVirtualMethods()->Get(i);
}

inline void Class::SetVirtualMethod(uint32_t i, AbstractMethod* f)  // TODO: uint16_t
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ObjectArray<AbstractMethod>* virtual_methods =
      GetFieldObject<ObjectArray<AbstractMethod>*>(
          OFFSET_OF_OBJECT_MEMBER(Class, virtual_methods_), false);
  virtual_methods->Set(i, f);
}

inline ObjectArray<AbstractMethod>* Class::GetVTable() const {
  DCHECK(IsResolved() || IsErroneous());
  return GetFieldObject<ObjectArray<AbstractMethod>*>(OFFSET_OF_OBJECT_MEMBER(Class, vtable_), false);
}

inline ObjectArray<AbstractMethod>* Class::GetVTableDuringLinking() const {
  DCHECK(IsLoaded() || IsErroneous());
  return GetFieldObject<ObjectArray<AbstractMethod>*>(OFFSET_OF_OBJECT_MEMBER(Class, vtable_), false);
}

inline void Class::SetVTable(ObjectArray<AbstractMethod>* new_vtable)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Class, vtable_), new_vtable, false);
}

inline AbstractMethod* Class::FindVirtualMethodForVirtual(AbstractMethod* method) const
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DCHECK(!method->GetDeclaringClass()->IsInterface());
  // The argument method may from a super class.
  // Use the index to a potentially overridden one for this instance's class.
  return GetVTable()->Get(method->GetMethodIndex());
}

inline AbstractMethod* Class::FindVirtualMethodForSuper(AbstractMethod* method) const
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DCHECK(!method->GetDeclaringClass()->IsInterface());
  return GetSuperClass()->GetVTable()->Get(method->GetMethodIndex());
}

inline AbstractMethod* Class::FindVirtualMethodForVirtualOrInterface(AbstractMethod* method) const {
  if (method->IsDirect()) {
    return method;
  }
  if (method->GetDeclaringClass()->IsInterface()) {
    return FindVirtualMethodForInterface(method);
  }
  return FindVirtualMethodForVirtual(method);
}

inline IfTable* Class::GetIfTable() const {
  return GetFieldObject<IfTable*>(OFFSET_OF_OBJECT_MEMBER(Class, iftable_), false);
}

inline int32_t Class::GetIfTableCount() const {
  IfTable* iftable = GetIfTable();
  if (iftable == NULL) {
    return 0;
  }
  return iftable->Count();
}

inline void Class::SetIfTable(IfTable* new_iftable) {
  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Class, iftable_), new_iftable, false);
}

inline ObjectArray<Field>* Class::GetIFields() const {
  DCHECK(IsLoaded() || IsErroneous());
  return GetFieldObject<ObjectArray<Field>*>(OFFSET_OF_OBJECT_MEMBER(Class, ifields_), false);
}

inline void Class::SetIFields(ObjectArray<Field>* new_ifields)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DCHECK(NULL == GetFieldObject<ObjectArray<Field>*>(
      OFFSET_OF_OBJECT_MEMBER(Class, ifields_), false));
  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Class, ifields_), new_ifields, false);
}

inline ObjectArray<Field>* Class::GetSFields() const {
  DCHECK(IsLoaded() || IsErroneous());
  return GetFieldObject<ObjectArray<Field>*>(OFFSET_OF_OBJECT_MEMBER(Class, sfields_), false);
}

inline void Class::SetSFields(ObjectArray<Field>* new_sfields)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DCHECK(NULL == GetFieldObject<ObjectArray<Field>*>(
      OFFSET_OF_OBJECT_MEMBER(Class, sfields_), false));
  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Class, sfields_), new_sfields, false);
}

inline size_t Class::NumStaticFields() const {
  return (GetSFields() != NULL) ? GetSFields()->GetLength() : 0;
}

inline Field* Class::GetStaticField(uint32_t i) const  // TODO: uint16_t
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return GetSFields()->Get(i);
}

inline void Class::SetStaticField(uint32_t i, Field* f)  // TODO: uint16_t
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ObjectArray<Field>* sfields= GetFieldObject<ObjectArray<Field>*>(
      OFFSET_OF_OBJECT_MEMBER(Class, sfields_), false);
  sfields->Set(i, f);
}

inline size_t Class::NumInstanceFields() const {
  return (GetIFields() != NULL) ? GetIFields()->GetLength() : 0;
}

inline Field* Class::GetInstanceField(uint32_t i) const  // TODO: uint16_t
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_){
  DCHECK_NE(NumInstanceFields(), 0U);
  return GetIFields()->Get(i);
}

inline void Class::SetInstanceField(uint32_t i, Field* f)  // TODO: uint16_t
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_){
  ObjectArray<Field>* ifields= GetFieldObject<ObjectArray<Field>*>(
      OFFSET_OF_OBJECT_MEMBER(Class, ifields_), false);
  ifields->Set(i, f);
}

inline void Class::SetVerifyErrorClass(Class* klass) {
  CHECK(klass != NULL) << PrettyClass(this);
  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Class, verify_error_class_), klass, false);
}

inline uint32_t Class::GetAccessFlags() const {
  // Check class is loaded or this is java.lang.String that has a
  // circularity issue during loading the names of its members
  DCHECK(IsLoaded() || IsErroneous() ||
         this == String::GetJavaLangString() ||
         this == Field::GetJavaLangReflectField() ||
         this == AbstractMethod::GetConstructorClass() ||
         this == AbstractMethod::GetMethodClass());
  return GetField32(OFFSET_OF_OBJECT_MEMBER(Class, access_flags_), false);
}

inline String* Class::GetName() const {
  return GetFieldObject<String*>(OFFSET_OF_OBJECT_MEMBER(Class, name_), false);
}
inline void Class::SetName(String* name) {
  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Class, name_), name, false);
}

}  // namespace mirror
}  // namespace art

#endif  // ART_SRC_MIRROR_CLASS_INL_H_
