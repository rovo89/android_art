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

#ifndef ART_SRC_DEX_CACHE_H_
#define ART_SRC_DEX_CACHE_H_

#include "dex_file.h"
#include "globals.h"
#include "macros.h"
#include "object.h"

namespace art {

class Class;
class Field;
class ImageWriter;
class Method;
class String;
union JValue;

class MANAGED DexCache : public ObjectArray<Object> {
 public:
  void Init(String* location,
            ObjectArray<String>* strings,
            ObjectArray<Class>* types,
            ObjectArray<Method>* methods,
            ObjectArray<Field>* fields,
            ObjectArray<StaticStorageBase>* initialized_static_storage)
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);

  void Fixup(Method* trampoline) SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);

  String* GetLocation() const SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_) {
    return Get(kLocation)->AsString();
  }

  static MemberOffset StringsOffset() {
    return MemberOffset(DataOffset(sizeof(Object*)).Int32Value() +
                        kStrings * sizeof(Object*));
  }

  static MemberOffset ResolvedFieldsOffset() {
    return MemberOffset(DataOffset(sizeof(Object*)).Int32Value() +
                        kResolvedFields * sizeof(Object*));
  }

  static MemberOffset ResolvedMethodsOffset() {
    return MemberOffset(DataOffset(sizeof(Object*)).Int32Value() +
                        kResolvedMethods * sizeof(Object*));
  }

  size_t NumStrings() const SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_) {
    return GetStrings()->GetLength();
  }

  size_t NumResolvedTypes() const SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_) {
    return GetResolvedTypes()->GetLength();
  }

  size_t NumResolvedMethods() const SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_) {
    return GetResolvedMethods()->GetLength();
  }

  size_t NumResolvedFields() const SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_) {
    return GetResolvedFields()->GetLength();
  }

  size_t NumInitializedStaticStorage() const
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_) {
    return GetInitializedStaticStorage()->GetLength();
  }

  String* GetResolvedString(uint32_t string_idx) const
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_) {
    return GetStrings()->Get(string_idx);
  }

  void SetResolvedString(uint32_t string_idx, String* resolved)
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_) {
    GetStrings()->Set(string_idx, resolved);
  }

  Class* GetResolvedType(uint32_t type_idx) const
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_) {
    return GetResolvedTypes()->Get(type_idx);
  }

  void SetResolvedType(uint32_t type_idx, Class* resolved)
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_) {
    GetResolvedTypes()->Set(type_idx, resolved);
  }

  Method* GetResolvedMethod(uint32_t method_idx) const
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_) {
    Method* method = GetResolvedMethods()->Get(method_idx);
    // Hide resolution trampoline methods from the caller
    if (method != NULL && method->GetDexMethodIndex() == DexFile::kDexNoIndex16) {
      DCHECK(method == Runtime::Current()->GetResolutionMethod());
      return NULL;
    } else {
      return method;
    }
  }

  void SetResolvedMethod(uint32_t method_idx, Method* resolved)
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_) {
    GetResolvedMethods()->Set(method_idx, resolved);
  }

  Field* GetResolvedField(uint32_t field_idx) const
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_) {
    return GetResolvedFields()->Get(field_idx);
  }

  void SetResolvedField(uint32_t field_idx, Field* resolved)
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_) {
    GetResolvedFields()->Set(field_idx, resolved);
  }

  ObjectArray<String>* GetStrings() const
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_) {
    return static_cast<ObjectArray<String>*>(GetNonNull(kStrings));
  }
  ObjectArray<Class>* GetResolvedTypes() const
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_) {
    return static_cast<ObjectArray<Class>*>(GetNonNull(kResolvedTypes));
  }
  ObjectArray<Method>* GetResolvedMethods() const
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_) {
    return static_cast<ObjectArray<Method>*>(GetNonNull(kResolvedMethods));
  }
  ObjectArray<Field>* GetResolvedFields() const
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_) {
    return static_cast<ObjectArray<Field>*>(GetNonNull(kResolvedFields));
  }
  ObjectArray<StaticStorageBase>* GetInitializedStaticStorage() const
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_) {
    return static_cast<ObjectArray<StaticStorageBase>*>(GetNonNull(kInitializedStaticStorage));
  }

  static size_t LengthAsArray() {
    return kMax;
  }

 private:
  enum ArrayIndex {
    kLocation                 = 0,
    kStrings                  = 1,
    kResolvedTypes            = 2,
    kResolvedMethods          = 3,
    kResolvedFields           = 4,
    kInitializedStaticStorage = 5,
    kMax                      = 6,
  };

  Object* GetNonNull(ArrayIndex array_index) const
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_) {
    Object* obj = Get(array_index);
    DCHECK(obj != NULL);
    return obj;
  }

  DISALLOW_IMPLICIT_CONSTRUCTORS(DexCache);
};

}  // namespace art

#endif  // ART_SRC_DEX_CACHE_H_
