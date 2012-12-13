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

#include "base/macros.h"
#include "dex_file.h"
#include "globals.h"
#include "object.h"

namespace art {

class Class;
class Field;
class ImageWriter;
class AbstractMethod;
class String;
union JValue;

class MANAGED DexCacheClass : public Class {
 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(DexCacheClass);
};

class MANAGED DexCache : public Object {
 public:
  void Init(const DexFile* dex_file,
            String* location,
            ObjectArray<String>* strings,
            ObjectArray<Class>* types,
            ObjectArray<AbstractMethod>* methods,
            ObjectArray<Field>* fields,
            ObjectArray<StaticStorageBase>* initialized_static_storage)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void Fixup(AbstractMethod* trampoline) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  String* GetLocation() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return GetFieldObject<String*>(OFFSET_OF_OBJECT_MEMBER(DexCache, location_), false);
  }

  static MemberOffset StringsOffset() {
    return OFFSET_OF_OBJECT_MEMBER(DexCache, strings_);
  }

  static MemberOffset ResolvedFieldsOffset() {
    return OFFSET_OF_OBJECT_MEMBER(DexCache, resolved_fields_);
  }

  static MemberOffset ResolvedMethodsOffset() {
    return OFFSET_OF_OBJECT_MEMBER(DexCache, resolved_methods_);
  }

  size_t NumStrings() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return GetStrings()->GetLength();
  }

  size_t NumResolvedTypes() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return GetResolvedTypes()->GetLength();
  }

  size_t NumResolvedMethods() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return GetResolvedMethods()->GetLength();
  }

  size_t NumResolvedFields() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return GetResolvedFields()->GetLength();
  }

  size_t NumInitializedStaticStorage() const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return GetInitializedStaticStorage()->GetLength();
  }

  String* GetResolvedString(uint32_t string_idx) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return GetStrings()->Get(string_idx);
  }

  void SetResolvedString(uint32_t string_idx, String* resolved)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    GetStrings()->Set(string_idx, resolved);
  }

  Class* GetResolvedType(uint32_t type_idx) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return GetResolvedTypes()->Get(type_idx);
  }

  void SetResolvedType(uint32_t type_idx, Class* resolved)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    GetResolvedTypes()->Set(type_idx, resolved);
  }

  AbstractMethod* GetResolvedMethod(uint32_t method_idx) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    AbstractMethod* method = GetResolvedMethods()->Get(method_idx);
    // Hide resolution trampoline methods from the caller
    if (method != NULL && method->GetDexMethodIndex() == DexFile::kDexNoIndex16) {
      DCHECK(method == Runtime::Current()->GetResolutionMethod());
      return NULL;
    } else {
      return method;
    }
  }

  void SetResolvedMethod(uint32_t method_idx, AbstractMethod* resolved)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    GetResolvedMethods()->Set(method_idx, resolved);
  }

  Field* GetResolvedField(uint32_t field_idx) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return GetResolvedFields()->Get(field_idx);
  }

  void SetResolvedField(uint32_t field_idx, Field* resolved)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    GetResolvedFields()->Set(field_idx, resolved);
  }

  ObjectArray<String>* GetStrings() const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return GetFieldObject< ObjectArray<String>* >(StringsOffset(), false);
  }

  ObjectArray<Class>* GetResolvedTypes() const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return GetFieldObject< ObjectArray<Class>* >(
        OFFSET_OF_OBJECT_MEMBER(DexCache, resolved_types_), false);
  }

  ObjectArray<AbstractMethod>* GetResolvedMethods() const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return GetFieldObject< ObjectArray<AbstractMethod>* >(ResolvedMethodsOffset(), false);
  }

  ObjectArray<Field>* GetResolvedFields() const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return GetFieldObject< ObjectArray<Field>* >(ResolvedFieldsOffset(), false);
  }

  ObjectArray<StaticStorageBase>* GetInitializedStaticStorage() const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return GetFieldObject< ObjectArray<StaticStorageBase>* >(
        OFFSET_OF_OBJECT_MEMBER(DexCache, initialized_static_storage_), false);
  }

  const DexFile* GetDexFile() const {
    return GetFieldPtr<const DexFile*>(OFFSET_OF_OBJECT_MEMBER(DexCache, dex_file_), false);
  }

  void SetDexFile(const DexFile* dex_file) {
    return SetFieldPtr(OFFSET_OF_OBJECT_MEMBER(DexCache, dex_file_), dex_file, false);
  }

 private:
  ObjectArray<StaticStorageBase>* initialized_static_storage_;
  String* location_;
  ObjectArray<Object>* resolved_fields_;
  ObjectArray<AbstractMethod>* resolved_methods_;
  ObjectArray<Class>* resolved_types_;
  ObjectArray<String>* strings_;
  uint32_t dex_file_;

  friend struct DexCacheOffsets; // for verifying offset information
  DISALLOW_IMPLICIT_CONSTRUCTORS(DexCache);
};

}  // namespace art

#endif  // ART_SRC_DEX_CACHE_H_
