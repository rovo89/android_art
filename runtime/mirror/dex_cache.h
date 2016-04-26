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

#ifndef ART_RUNTIME_MIRROR_DEX_CACHE_H_
#define ART_RUNTIME_MIRROR_DEX_CACHE_H_

#include "array.h"
#include "art_field.h"
#include "art_method.h"
#include "class.h"
#include "object.h"
#include "object_array.h"
#include "utils.h"

namespace art {

struct DexCacheOffsets;
class DexFile;
class ImageWriter;
union JValue;

namespace mirror {

class String;

// C++ mirror of java.lang.DexCache.
class MANAGED DexCache FINAL : public Object {
 public:
  // Size of java.lang.DexCache.class.
  static uint32_t ClassSize(size_t pointer_size);

  // Size of an instance of java.lang.DexCache not including referenced values.
  static uint32_t InstanceSize() {
    return sizeof(DexCache) + (IsSamsungROM() ? 8 : 0);
  }

  void Init(const DexFile* dex_file, String* location, ObjectArray<String>* strings,
            ObjectArray<Class>* types, PointerArray* methods, PointerArray* fields,
            size_t pointer_size) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void Fixup(ArtMethod* trampoline, size_t pointer_size)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  String* GetLocation() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return GetFieldObject<String>(LocationOffset());
  }

  static MemberOffset LocationOffset() {
    return MemberOffset(OFFSETOF_MEMBER(DexCache, location_) + (IsSamsungROM() ? 4 : 0));
  }

  static MemberOffset DexOffset() {
    return OFFSET_OF_OBJECT_MEMBER(DexCache, dex_);
  }

  static MemberOffset StringsOffset() {
    return MemberOffset(OFFSETOF_MEMBER(DexCache, strings_) + (IsSamsungROM() ? 4 : 0));
  }

  static MemberOffset ResolvedFieldsOffset() {
    return MemberOffset(OFFSETOF_MEMBER(DexCache, resolved_fields_) + (IsSamsungROM() ? 4 : 0));
  }

  static MemberOffset ResolvedMethodsOffset() {
    return MemberOffset(OFFSETOF_MEMBER(DexCache, resolved_methods_) + (IsSamsungROM() ? 4 : 0));
  }

  static MemberOffset ResolvedTypesOffset() {
    return MemberOffset(OFFSETOF_MEMBER(DexCache, resolved_types_) + (IsSamsungROM() ? 4 : 0));
  }

  static MemberOffset DexFileOffset() {
    return MemberOffset(OFFSETOF_MEMBER(DexCache, dex_file_) + (IsSamsungROM() ? 8 : 0));
  }

  size_t NumStrings() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return GetStrings()->GetLength();
  }

  size_t NumResolvedTypes() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return GetResolvedTypes()->GetLength();
  }

  size_t NumResolvedMethods() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return GetResolvedMethods()->GetLength();
  }

  size_t NumResolvedFields() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return GetResolvedFields()->GetLength();
  }

  String* GetResolvedString(uint32_t string_idx) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return GetStrings()->Get(string_idx);
  }

  void SetResolvedString(uint32_t string_idx, String* resolved) ALWAYS_INLINE
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    // TODO default transaction support.
    GetStrings()->Set(string_idx, resolved);
  }

  Class* GetResolvedType(uint32_t type_idx) ALWAYS_INLINE
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return GetResolvedTypes()->Get(type_idx);
  }

  void SetResolvedType(uint32_t type_idx, Class* resolved)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  ALWAYS_INLINE ArtMethod* GetResolvedMethod(uint32_t method_idx, size_t ptr_size)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  ALWAYS_INLINE void SetResolvedMethod(uint32_t method_idx, ArtMethod* resolved, size_t ptr_size)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Pointer sized variant, used for patching.
  ALWAYS_INLINE ArtField* GetResolvedField(uint32_t idx, size_t ptr_size)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Pointer sized variant, used for patching.
  ALWAYS_INLINE void SetResolvedField(uint32_t idx, ArtField* field, size_t ptr_size)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  ObjectArray<String>* GetStrings() ALWAYS_INLINE SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return GetFieldObject<ObjectArray<String>>(StringsOffset());
  }

  ObjectArray<Class>* GetResolvedTypes() ALWAYS_INLINE SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return GetFieldObject<ObjectArray<Class>>(
        ResolvedTypesOffset());
  }

  PointerArray* GetResolvedMethods() ALWAYS_INLINE SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return GetFieldObject<PointerArray>(ResolvedMethodsOffset());
  }

  PointerArray* GetResolvedFields() ALWAYS_INLINE SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return GetFieldObject<PointerArray>(ResolvedFieldsOffset());
  }

  const DexFile* GetDexFile() ALWAYS_INLINE SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return GetFieldPtr<const DexFile*>(DexFileOffset());
  }

  void SetDexFile(const DexFile* dex_file) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      ALWAYS_INLINE {
    return SetFieldPtr<false>(DexFileOffset(), dex_file);
  }

 private:
  HeapReference<Object> dex_;
  HeapReference<String> location_;
  // Either an int array or long array based on runtime ISA since these arrays hold pointers.
  HeapReference<PointerArray> resolved_fields_;
  HeapReference<PointerArray> resolved_methods_;
  HeapReference<ObjectArray<Class>> resolved_types_;
  HeapReference<ObjectArray<String>> strings_;
  uint64_t dex_file_;

  friend struct art::DexCacheOffsets;  // for verifying offset information
  DISALLOW_IMPLICIT_CONSTRUCTORS(DexCache);
};

}  // namespace mirror
}  // namespace art

#endif  // ART_RUNTIME_MIRROR_DEX_CACHE_H_
