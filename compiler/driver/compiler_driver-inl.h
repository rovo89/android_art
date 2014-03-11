/*
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef ART_COMPILER_DRIVER_COMPILER_DRIVER_INL_H_
#define ART_COMPILER_DRIVER_COMPILER_DRIVER_INL_H_

#include "compiler_driver.h"
#include "dex/compiler_ir.h"
#include "mirror/art_field.h"
#include "mirror/art_field-inl.h"
#include "mirror/class_loader.h"
#include "mirror/dex_cache.h"
#include "mirror/art_field-inl.h"
#include "scoped_thread_state_change.h"

namespace art {

inline mirror::DexCache* CompilerDriver::GetDexCache(const DexCompilationUnit* mUnit) {
  return mUnit->GetClassLinker()->FindDexCache(*mUnit->GetDexFile());
}

inline mirror::ClassLoader* CompilerDriver::GetClassLoader(ScopedObjectAccess& soa,
                                                           const DexCompilationUnit* mUnit) {
  return soa.Decode<mirror::ClassLoader*>(mUnit->GetClassLoader());
}

inline mirror::Class* CompilerDriver::ResolveCompilingMethodsClass(
    ScopedObjectAccess& soa, const SirtRef<mirror::DexCache>& dex_cache,
    const SirtRef<mirror::ClassLoader>& class_loader, const DexCompilationUnit* mUnit) {
  DCHECK(dex_cache->GetDexFile() == mUnit->GetDexFile());
  DCHECK(class_loader.get() == soa.Decode<mirror::ClassLoader*>(mUnit->GetClassLoader()));
  const DexFile::MethodId& referrer_method_id =
      mUnit->GetDexFile()->GetMethodId(mUnit->GetDexMethodIndex());
  mirror::Class* referrer_class = mUnit->GetClassLinker()->ResolveType(
      *mUnit->GetDexFile(), referrer_method_id.class_idx_, dex_cache, class_loader);
  DCHECK_EQ(referrer_class == nullptr, soa.Self()->IsExceptionPending());
  if (UNLIKELY(referrer_class == nullptr)) {
    // Clean up any exception left by type resolution.
    soa.Self()->ClearException();
  }
  return referrer_class;
}

inline mirror::ArtField* CompilerDriver::ResolveField(
    ScopedObjectAccess& soa, const SirtRef<mirror::DexCache>& dex_cache,
    const SirtRef<mirror::ClassLoader>& class_loader, const DexCompilationUnit* mUnit,
    uint32_t field_idx, bool is_static) {
  DCHECK(dex_cache->GetDexFile() == mUnit->GetDexFile());
  DCHECK(class_loader.get() == soa.Decode<mirror::ClassLoader*>(mUnit->GetClassLoader()));
  mirror::ArtField* resolved_field = mUnit->GetClassLinker()->ResolveField(
      *mUnit->GetDexFile(), field_idx, dex_cache, class_loader, is_static);
  DCHECK_EQ(resolved_field == nullptr, soa.Self()->IsExceptionPending());
  if (UNLIKELY(resolved_field == nullptr)) {
    // Clean up any exception left by type resolution.
    soa.Self()->ClearException();
    return nullptr;
  }
  if (UNLIKELY(resolved_field->IsStatic() != is_static)) {
    // ClassLinker can return a field of the wrong kind directly from the DexCache.
    // Silently return nullptr on such incompatible class change.
    return nullptr;
  }
  return resolved_field;
}

inline void CompilerDriver::GetResolvedFieldDexFileLocation(
    mirror::ArtField* resolved_field, const DexFile** declaring_dex_file,
    uint16_t* declaring_class_idx, uint16_t* declaring_field_idx) {
  mirror::Class* declaring_class = resolved_field->GetDeclaringClass();
  *declaring_dex_file = declaring_class->GetDexCache()->GetDexFile();
  *declaring_class_idx = declaring_class->GetDexTypeIndex();
  *declaring_field_idx = resolved_field->GetDexFieldIndex();
}

inline bool CompilerDriver::IsFieldVolatile(mirror::ArtField* field) {
  return field->IsVolatile();
}

inline std::pair<bool, bool> CompilerDriver::IsFastInstanceField(
    mirror::DexCache* dex_cache, mirror::Class* referrer_class,
    mirror::ArtField* resolved_field, uint16_t field_idx, MemberOffset* field_offset) {
  DCHECK(!resolved_field->IsStatic());
  mirror::Class* fields_class = resolved_field->GetDeclaringClass();
  bool fast_get = referrer_class != nullptr &&
      referrer_class->CanAccessResolvedField(fields_class, resolved_field,
                                             dex_cache, field_idx);
  bool fast_put = fast_get && (!resolved_field->IsFinal() || fields_class == referrer_class);
  *field_offset = fast_get ? resolved_field->GetOffset() : MemberOffset(0u);
  return std::make_pair(fast_get, fast_put);
}

inline std::pair<bool, bool> CompilerDriver::IsFastStaticField(
    mirror::DexCache* dex_cache, mirror::Class* referrer_class,
    mirror::ArtField* resolved_field, uint16_t field_idx, MemberOffset* field_offset,
    uint32_t* storage_index, bool* is_referrers_class, bool* is_initialized) {
  DCHECK(resolved_field->IsStatic());
  if (LIKELY(referrer_class != nullptr)) {
    mirror::Class* fields_class = resolved_field->GetDeclaringClass();
    if (fields_class == referrer_class) {
      *field_offset = resolved_field->GetOffset();
      *storage_index = fields_class->GetDexTypeIndex();
      *is_referrers_class = true;  // implies no worrying about class initialization
      *is_initialized = true;
      return std::make_pair(true, true);
    }
    if (referrer_class->CanAccessResolvedField(fields_class, resolved_field,
                                               dex_cache, field_idx)) {
      // We have the resolved field, we must make it into a index for the referrer
      // in its static storage (which may fail if it doesn't have a slot for it)
      // TODO: for images we can elide the static storage base null check
      // if we know there's a non-null entry in the image
      const DexFile* dex_file = dex_cache->GetDexFile();
      uint32_t storage_idx = DexFile::kDexNoIndex;
      if (LIKELY(fields_class->GetDexCache() == dex_cache)) {
        // common case where the dex cache of both the referrer and the field are the same,
        // no need to search the dex file
        storage_idx = fields_class->GetDexTypeIndex();
      } else {
        // Search dex file for localized ssb index, may fail if field's class is a parent
        // of the class mentioned in the dex file and there is no dex cache entry.
        const DexFile::StringId* string_id =
            dex_file->FindStringId(FieldHelper(resolved_field).GetDeclaringClassDescriptor());
        if (string_id != nullptr) {
          const DexFile::TypeId* type_id =
             dex_file->FindTypeId(dex_file->GetIndexForStringId(*string_id));
          if (type_id != nullptr) {
            // medium path, needs check of static storage base being initialized
            storage_idx = dex_file->GetIndexForTypeId(*type_id);
          }
        }
      }
      if (storage_idx != DexFile::kDexNoIndex) {
        *field_offset = resolved_field->GetOffset();
        *storage_index = storage_idx;
        *is_referrers_class = false;
        *is_initialized = fields_class->IsInitialized() &&
            CanAssumeTypeIsPresentInDexCache(*dex_file, storage_idx);
        return std::make_pair(true, !resolved_field->IsFinal());
      }
    }
  }
  // Conservative defaults.
  *field_offset = MemberOffset(0u);
  *storage_index = DexFile::kDexNoIndex;
  *is_referrers_class = false;
  *is_initialized = false;
  return std::make_pair(false, false);
}

}  // namespace art

#endif  // ART_COMPILER_DRIVER_COMPILER_DRIVER_INL_H_
