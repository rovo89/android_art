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

#ifndef ART_RUNTIME_MIRROR_CLASS_INL_H_
#define ART_RUNTIME_MIRROR_CLASS_INL_H_

#include "class.h"

#include "art_field.h"
#include "art_method.h"
#include "class_linker-inl.h"
#include "class_loader.h"
#include "common_throws.h"
#include "dex_cache.h"
#include "dex_file.h"
#include "gc/heap-inl.h"
#include "iftable.h"
#include "object_array-inl.h"
#include "runtime.h"
#include "string.h"

namespace art {
namespace mirror {

template<VerifyObjectFlags kVerifyFlags, ReadBarrierOption kReadBarrierOption>
inline uint32_t Class::GetObjectSize() {
  if (kIsDebugBuild) {
    // Use a local variable as (D)CHECK can't handle the space between
    // the two template params.
    bool is_variable_size = IsVariableSize<kVerifyFlags, kReadBarrierOption>();
    CHECK(!is_variable_size) << " class=" << PrettyTypeOf(this);
  }
  return GetField32(OFFSET_OF_OBJECT_MEMBER(Class, object_size_));
}

inline Class* Class::GetSuperClass() {
  // Can only get super class for loaded classes (hack for when runtime is
  // initializing)
  DCHECK(IsLoaded() || IsErroneous() || !Runtime::Current()->IsStarted()) << IsLoaded();
  return GetFieldObject<Class>(OFFSET_OF_OBJECT_MEMBER(Class, super_class_));
}

inline ClassLoader* Class::GetClassLoader() {
  return GetFieldObject<ClassLoader>(OFFSET_OF_OBJECT_MEMBER(Class, class_loader_));
}

template<VerifyObjectFlags kVerifyFlags>
inline DexCache* Class::GetDexCache() {
  return GetFieldObject<DexCache, kVerifyFlags>(OFFSET_OF_OBJECT_MEMBER(Class, dex_cache_));
}

inline ObjectArray<ArtMethod>* Class::GetDirectMethods() {
  DCHECK(IsLoaded() || IsErroneous());
  return GetFieldObject<ObjectArray<ArtMethod> >(OFFSET_OF_OBJECT_MEMBER(Class, direct_methods_));
}

inline void Class::SetDirectMethods(ObjectArray<ArtMethod>* new_direct_methods)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DCHECK(NULL == GetFieldObject<ObjectArray<ArtMethod> >(
      OFFSET_OF_OBJECT_MEMBER(Class, direct_methods_)));
  DCHECK_NE(0, new_direct_methods->GetLength());
  SetFieldObject<false>(OFFSET_OF_OBJECT_MEMBER(Class, direct_methods_), new_direct_methods);
}

inline ArtMethod* Class::GetDirectMethod(int32_t i) {
  return GetDirectMethods()->Get(i);
}

inline void Class::SetDirectMethod(uint32_t i, ArtMethod* f)  // TODO: uint16_t
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ObjectArray<ArtMethod>* direct_methods =
      GetFieldObject<ObjectArray<ArtMethod> >(OFFSET_OF_OBJECT_MEMBER(Class, direct_methods_));
  direct_methods->Set<false>(i, f);
}

// Returns the number of static, private, and constructor methods.
inline uint32_t Class::NumDirectMethods() {
  return (GetDirectMethods() != NULL) ? GetDirectMethods()->GetLength() : 0;
}

template<VerifyObjectFlags kVerifyFlags>
inline ObjectArray<ArtMethod>* Class::GetVirtualMethods() {
  DCHECK(IsLoaded() || IsErroneous());
  return GetFieldObject<ObjectArray<ArtMethod> >(OFFSET_OF_OBJECT_MEMBER(Class, virtual_methods_));
}

inline void Class::SetVirtualMethods(ObjectArray<ArtMethod>* new_virtual_methods) {
  // TODO: we reassign virtual methods to grow the table for miranda
  // methods.. they should really just be assigned once.
  DCHECK_NE(0, new_virtual_methods->GetLength());
  SetFieldObject<false>(OFFSET_OF_OBJECT_MEMBER(Class, virtual_methods_), new_virtual_methods);
}

inline uint32_t Class::NumVirtualMethods() {
  return (GetVirtualMethods() != NULL) ? GetVirtualMethods()->GetLength() : 0;
}

template<VerifyObjectFlags kVerifyFlags>
inline ArtMethod* Class::GetVirtualMethod(uint32_t i) {
  DCHECK(IsResolved<kVerifyFlags>() || IsErroneous<kVerifyFlags>());
  return GetVirtualMethods()->Get(i);
}

inline ArtMethod* Class::GetVirtualMethodDuringLinking(uint32_t i) {
  DCHECK(IsLoaded() || IsErroneous());
  return GetVirtualMethods()->Get(i);
}

inline void Class::SetVirtualMethod(uint32_t i, ArtMethod* f)  // TODO: uint16_t
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ObjectArray<ArtMethod>* virtual_methods =
      GetFieldObject<ObjectArray<ArtMethod> >(OFFSET_OF_OBJECT_MEMBER(Class, virtual_methods_));
  virtual_methods->Set<false>(i, f);
}

inline ObjectArray<ArtMethod>* Class::GetVTable() {
  DCHECK(IsResolved() || IsErroneous());
  return GetFieldObject<ObjectArray<ArtMethod> >(OFFSET_OF_OBJECT_MEMBER(Class, vtable_));
}

inline ObjectArray<ArtMethod>* Class::GetVTableDuringLinking() {
  DCHECK(IsLoaded() || IsErroneous());
  return GetFieldObject<ObjectArray<ArtMethod> >(OFFSET_OF_OBJECT_MEMBER(Class, vtable_));
}

inline void Class::SetVTable(ObjectArray<ArtMethod>* new_vtable) {
  SetFieldObject<false>(OFFSET_OF_OBJECT_MEMBER(Class, vtable_), new_vtable);
}

inline ObjectArray<ArtMethod>* Class::GetImTable() {
  return GetFieldObject<ObjectArray<ArtMethod> >(OFFSET_OF_OBJECT_MEMBER(Class, imtable_));
}

inline void Class::SetImTable(ObjectArray<ArtMethod>* new_imtable) {
  SetFieldObject<false>(OFFSET_OF_OBJECT_MEMBER(Class, imtable_), new_imtable);
}

inline bool Class::Implements(Class* klass) {
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
inline bool Class::IsArrayAssignableFromArray(Class* src) {
  DCHECK(IsArrayClass())  << PrettyClass(this);
  DCHECK(src->IsArrayClass()) << PrettyClass(src);
  return GetComponentType()->IsAssignableFrom(src->GetComponentType());
}

inline bool Class::IsAssignableFromArray(Class* src) {
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

template <bool throw_on_failure, bool use_referrers_cache>
inline bool Class::ResolvedFieldAccessTest(Class* access_to, ArtField* field,
                                           uint32_t field_idx, DexCache* dex_cache) {
  DCHECK_EQ(use_referrers_cache, dex_cache == nullptr);
  if (UNLIKELY(!this->CanAccess(access_to))) {
    // The referrer class can't access the field's declaring class but may still be able
    // to access the field if the FieldId specifies an accessible subclass of the declaring
    // class rather than the declaring class itself.
    DexCache* referrer_dex_cache = use_referrers_cache ? this->GetDexCache() : dex_cache;
    uint32_t class_idx = referrer_dex_cache->GetDexFile()->GetFieldId(field_idx).class_idx_;
    // The referenced class has already been resolved with the field, get it from the dex cache.
    Class* dex_access_to = referrer_dex_cache->GetResolvedType(class_idx);
    DCHECK(dex_access_to != nullptr);
    if (UNLIKELY(!this->CanAccess(dex_access_to))) {
      if (throw_on_failure) {
        ThrowIllegalAccessErrorClass(this, dex_access_to);
      }
      return false;
    }
    DCHECK_EQ(this->CanAccessMember(access_to, field->GetAccessFlags()),
              this->CanAccessMember(dex_access_to, field->GetAccessFlags()));
  }
  if (LIKELY(this->CanAccessMember(access_to, field->GetAccessFlags()))) {
    return true;
  }
  if (throw_on_failure) {
    ThrowIllegalAccessErrorField(this, field);
  }
  return false;
}

template <bool throw_on_failure, bool use_referrers_cache, InvokeType throw_invoke_type>
inline bool Class::ResolvedMethodAccessTest(Class* access_to, ArtMethod* method,
                                            uint32_t method_idx, DexCache* dex_cache) {
  COMPILE_ASSERT(throw_on_failure || throw_invoke_type == kStatic, non_default_throw_invoke_type);
  DCHECK_EQ(use_referrers_cache, dex_cache == nullptr);
  if (UNLIKELY(!this->CanAccess(access_to))) {
    // The referrer class can't access the method's declaring class but may still be able
    // to access the method if the MethodId specifies an accessible subclass of the declaring
    // class rather than the declaring class itself.
    DexCache* referrer_dex_cache = use_referrers_cache ? this->GetDexCache() : dex_cache;
    uint32_t class_idx = referrer_dex_cache->GetDexFile()->GetMethodId(method_idx).class_idx_;
    // The referenced class has already been resolved with the method, get it from the dex cache.
    Class* dex_access_to = referrer_dex_cache->GetResolvedType(class_idx);
    DCHECK(dex_access_to != nullptr);
    if (UNLIKELY(!this->CanAccess(dex_access_to))) {
      if (throw_on_failure) {
        ThrowIllegalAccessErrorClassForMethodDispatch(this, dex_access_to,
                                                      method, throw_invoke_type);
      }
      return false;
    }
    DCHECK_EQ(this->CanAccessMember(access_to, method->GetAccessFlags()),
              this->CanAccessMember(dex_access_to, method->GetAccessFlags()));
  }
  if (LIKELY(this->CanAccessMember(access_to, method->GetAccessFlags()))) {
    return true;
  }
  if (throw_on_failure) {
    ThrowIllegalAccessErrorMethod(this, method);
  }
  return false;
}

inline bool Class::CanAccessResolvedField(Class* access_to, ArtField* field,
                                          DexCache* dex_cache, uint32_t field_idx) {
  return ResolvedFieldAccessTest<false, false>(access_to, field, field_idx, dex_cache);
}

inline bool Class::CheckResolvedFieldAccess(Class* access_to, ArtField* field,
                                            uint32_t field_idx) {
  return ResolvedFieldAccessTest<true, true>(access_to, field, field_idx, nullptr);
}

inline bool Class::CanAccessResolvedMethod(Class* access_to, ArtMethod* method,
                                           DexCache* dex_cache, uint32_t method_idx) {
  return ResolvedMethodAccessTest<false, false, kStatic>(access_to, method, method_idx, dex_cache);
}

template <InvokeType throw_invoke_type>
inline bool Class::CheckResolvedMethodAccess(Class* access_to, ArtMethod* method,
                                             uint32_t method_idx) {
  return ResolvedMethodAccessTest<true, true, throw_invoke_type>(access_to, method, method_idx,
                                                                 nullptr);
}

inline bool Class::IsSubClass(Class* klass) {
  DCHECK(!IsInterface()) << PrettyClass(this);
  DCHECK(!IsArrayClass()) << PrettyClass(this);
  Class* current = this;
  do {
    if (current == klass) {
      return true;
    }
    current = current->GetSuperClass();
  } while (current != NULL);
  return false;
}

inline ArtMethod* Class::FindVirtualMethodForInterface(ArtMethod* method) {
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

inline ArtMethod* Class::FindVirtualMethodForVirtual(ArtMethod* method) {
  DCHECK(!method->GetDeclaringClass()->IsInterface() || method->IsMiranda());
  // The argument method may from a super class.
  // Use the index to a potentially overridden one for this instance's class.
  return GetVTable()->Get(method->GetMethodIndex());
}

inline ArtMethod* Class::FindVirtualMethodForSuper(ArtMethod* method) {
  DCHECK(!method->GetDeclaringClass()->IsInterface());
  return GetSuperClass()->GetVTable()->Get(method->GetMethodIndex());
}

inline ArtMethod* Class::FindVirtualMethodForVirtualOrInterface(ArtMethod* method) {
  if (method->IsDirect()) {
    return method;
  }
  if (method->GetDeclaringClass()->IsInterface() && !method->IsMiranda()) {
    return FindVirtualMethodForInterface(method);
  }
  return FindVirtualMethodForVirtual(method);
}

inline IfTable* Class::GetIfTable() {
  return GetFieldObject<IfTable>(OFFSET_OF_OBJECT_MEMBER(Class, iftable_));
}

inline int32_t Class::GetIfTableCount() {
  IfTable* iftable = GetIfTable();
  if (iftable == NULL) {
    return 0;
  }
  return iftable->Count();
}

inline void Class::SetIfTable(IfTable* new_iftable) {
  SetFieldObject<false>(OFFSET_OF_OBJECT_MEMBER(Class, iftable_), new_iftable);
}

inline ObjectArray<ArtField>* Class::GetIFields() {
  DCHECK(IsLoaded() || IsErroneous());
  return GetFieldObject<ObjectArray<ArtField>>(OFFSET_OF_OBJECT_MEMBER(Class, ifields_));
}

inline void Class::SetIFields(ObjectArray<ArtField>* new_ifields)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DCHECK(NULL == GetFieldObject<ObjectArray<ArtField> >(OFFSET_OF_OBJECT_MEMBER(Class, ifields_)));
  SetFieldObject<false>(OFFSET_OF_OBJECT_MEMBER(Class, ifields_), new_ifields);
}

inline ObjectArray<ArtField>* Class::GetSFields() {
  DCHECK(IsLoaded() || IsErroneous());
  return GetFieldObject<ObjectArray<ArtField> >(OFFSET_OF_OBJECT_MEMBER(Class, sfields_));
}

inline void Class::SetSFields(ObjectArray<ArtField>* new_sfields)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DCHECK(NULL == GetFieldObject<ObjectArray<ArtField> >(OFFSET_OF_OBJECT_MEMBER(Class, sfields_)));
  SetFieldObject<false>(OFFSET_OF_OBJECT_MEMBER(Class, sfields_), new_sfields);
}

inline uint32_t Class::NumStaticFields() {
  return (GetSFields() != NULL) ? GetSFields()->GetLength() : 0;
}


inline ArtField* Class::GetStaticField(uint32_t i)  // TODO: uint16_t
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return GetSFields()->GetWithoutChecks(i);
}

inline void Class::SetStaticField(uint32_t i, ArtField* f)  // TODO: uint16_t
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ObjectArray<ArtField>* sfields= GetFieldObject<ObjectArray<ArtField> >(
      OFFSET_OF_OBJECT_MEMBER(Class, sfields_));
  sfields->Set<false>(i, f);
}

inline uint32_t Class::NumInstanceFields() {
  return (GetIFields() != NULL) ? GetIFields()->GetLength() : 0;
}

inline ArtField* Class::GetInstanceField(uint32_t i) {  // TODO: uint16_t
  DCHECK_NE(NumInstanceFields(), 0U);
  return GetIFields()->GetWithoutChecks(i);
}

inline void Class::SetInstanceField(uint32_t i, ArtField* f)  // TODO: uint16_t
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ObjectArray<ArtField>* ifields= GetFieldObject<ObjectArray<ArtField> >(
      OFFSET_OF_OBJECT_MEMBER(Class, ifields_));
  ifields->Set<false>(i, f);
}

template<VerifyObjectFlags kVerifyFlags>
inline uint32_t Class::GetReferenceInstanceOffsets() {
  DCHECK(IsResolved<kVerifyFlags>() || IsErroneous<kVerifyFlags>());
  return GetField32<kVerifyFlags>(OFFSET_OF_OBJECT_MEMBER(Class, reference_instance_offsets_));
}

inline void Class::SetClinitThreadId(pid_t new_clinit_thread_id) {
  if (Runtime::Current()->IsActiveTransaction()) {
    SetField32<true>(OFFSET_OF_OBJECT_MEMBER(Class, clinit_thread_id_), new_clinit_thread_id);
  } else {
    SetField32<false>(OFFSET_OF_OBJECT_MEMBER(Class, clinit_thread_id_), new_clinit_thread_id);
  }
}

inline void Class::SetVerifyErrorClass(Class* klass) {
  CHECK(klass != NULL) << PrettyClass(this);
  if (Runtime::Current()->IsActiveTransaction()) {
    SetFieldObject<true>(OFFSET_OF_OBJECT_MEMBER(Class, verify_error_class_), klass);
  } else {
    SetFieldObject<false>(OFFSET_OF_OBJECT_MEMBER(Class, verify_error_class_), klass);
  }
}

template<VerifyObjectFlags kVerifyFlags>
inline uint32_t Class::GetAccessFlags() {
  // Check class is loaded or this is java.lang.String that has a
  // circularity issue during loading the names of its members
  DCHECK(IsLoaded<kVerifyFlags>() ||
         IsErroneous<static_cast<VerifyObjectFlags>(kVerifyFlags & ~kVerifyThis)>() ||
         this == String::GetJavaLangString() ||
         this == ArtField::GetJavaLangReflectArtField() ||
         this == ArtMethod::GetJavaLangReflectArtMethod());
  return GetField32<kVerifyFlags>(OFFSET_OF_OBJECT_MEMBER(Class, access_flags_));
}

inline String* Class::GetName() {
  return GetFieldObject<String>(OFFSET_OF_OBJECT_MEMBER(Class, name_));
}
inline void Class::SetName(String* name) {
  if (Runtime::Current()->IsActiveTransaction()) {
    SetFieldObject<true>(OFFSET_OF_OBJECT_MEMBER(Class, name_), name);
  } else {
    SetFieldObject<false>(OFFSET_OF_OBJECT_MEMBER(Class, name_), name);
  }
}

template<VerifyObjectFlags kVerifyFlags>
inline Primitive::Type Class::GetPrimitiveType() {
  DCHECK_EQ(sizeof(Primitive::Type), sizeof(int32_t));
  return static_cast<Primitive::Type>(
      GetField32<kVerifyFlags>(OFFSET_OF_OBJECT_MEMBER(Class, primitive_type_)));
}

inline void Class::CheckObjectAlloc() {
  DCHECK(!IsArrayClass())
      << PrettyClass(this)
      << "A array shouldn't be allocated through this "
      << "as it requires a pre-fence visitor that sets the class size.";
  DCHECK(!IsClassClass())
      << PrettyClass(this)
      << "A class object shouldn't be allocated through this "
      << "as it requires a pre-fence visitor that sets the class size.";
  DCHECK(IsInstantiable()) << PrettyClass(this);
  // TODO: decide whether we want this check. It currently fails during bootstrap.
  // DCHECK(!Runtime::Current()->IsStarted() || IsInitializing()) << PrettyClass(this);
  DCHECK_GE(this->object_size_, sizeof(Object));
}

template<bool kIsInstrumented, bool kCheckAddFinalizer>
inline Object* Class::Alloc(Thread* self, gc::AllocatorType allocator_type) {
  CheckObjectAlloc();
  gc::Heap* heap = Runtime::Current()->GetHeap();
  const bool add_finalizer = kCheckAddFinalizer && IsFinalizable();
  if (!kCheckAddFinalizer) {
    DCHECK(!IsFinalizable());
  }
  mirror::Object* obj =
      heap->AllocObjectWithAllocator<kIsInstrumented, false>(self, this, this->object_size_,
                                                             allocator_type, VoidFunctor());
  if (add_finalizer && LIKELY(obj != nullptr)) {
    heap->AddFinalizerReference(self, &obj);
  }
  return obj;
}

inline Object* Class::AllocObject(Thread* self) {
  return Alloc<true>(self, Runtime::Current()->GetHeap()->GetCurrentAllocator());
}

inline Object* Class::AllocNonMovableObject(Thread* self) {
  return Alloc<true>(self, Runtime::Current()->GetHeap()->GetCurrentNonMovingAllocator());
}

template <bool kVisitClass, typename Visitor>
inline void Class::VisitReferences(mirror::Class* klass, const Visitor& visitor) {
  VisitInstanceFieldsReferences<kVisitClass>(klass, visitor);
  VisitStaticFieldsReferences<kVisitClass>(this, visitor);
}

template<ReadBarrierOption kReadBarrierOption>
inline bool Class::IsArtFieldClass() {
  Class* java_lang_Class = GetClass<kVerifyNone, kReadBarrierOption>();
  Class* java_lang_reflect_ArtField =
      java_lang_Class->GetInstanceField(0)->GetClass<kVerifyNone, kReadBarrierOption>();
  return this == java_lang_reflect_ArtField;
}

template<ReadBarrierOption kReadBarrierOption>
inline bool Class::IsArtMethodClass() {
  return this == ArtMethod::GetJavaLangReflectArtMethod<kReadBarrierOption>();
}

template<VerifyObjectFlags kVerifyFlags, ReadBarrierOption kReadBarrierOption>
inline bool Class::IsClassClass() {
  Class* java_lang_Class = GetClass<kVerifyFlags, kReadBarrierOption>()->
      template GetClass<kVerifyFlags, kReadBarrierOption>();
  return this == java_lang_Class;
}

inline const DexFile& Class::GetDexFile() {
  return *GetDexCache()->GetDexFile();
}

inline bool Class::DescriptorEquals(const char* match) {
  if (UNLIKELY(IsArrayClass())) {
    return match[0] == '[' && GetComponentType()->DescriptorEquals(match + 1);
  } else if (UNLIKELY(IsPrimitive())) {
    return strcmp(Primitive::Descriptor(GetPrimitiveType()), match) == 0;
  } else if (UNLIKELY(IsProxyClass())) {
    return Runtime::Current()->GetClassLinker()->GetDescriptorForProxy(this) == match;
  } else {
    const DexFile& dex_file = GetDexFile();
    const DexFile::TypeId& type_id = dex_file.GetTypeId(GetClassDef()->class_idx_);
    return strcmp(dex_file.GetTypeDescriptor(type_id), match) == 0;
  }
}

}  // namespace mirror
}  // namespace art

#endif  // ART_RUNTIME_MIRROR_CLASS_INL_H_
