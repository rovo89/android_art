/*
 * Copyright (C) 2014 The Android Open Source Project
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

#include "mir_annotations.h"

#include <string.h>

#include "base/logging.h"
#include "class_linker.h"
#include "compiler_ir.h"
#include "driver/dex_compilation_unit.h"
#include "mirror/class.h"
#include "mirror/class-inl.h"
#include "mirror/art_field.h"
#include "mirror/art_field-inl.h"
#include "mirror/art_method.h"
#include "mirror/art_method-inl.h"
#include "scoped_thread_state_change.h"

namespace art {

void IFieldAnnotation::Resolve(const DexCompilationUnit* mUnit,
                               IFieldAnnotation* annotations, size_t count) {
  if (kIsDebugBuild) {
    DCHECK(annotations != nullptr);
    DCHECK_NE(count, 0u);
    for (auto it = annotations, end = annotations + count; it != end; ++it) {
      IFieldAnnotation unresolved(it->field_idx_);
      DCHECK_EQ(memcmp(&unresolved, &*it, sizeof(*it)), 0);
    }
  }

  const DexFile* dex_file = mUnit->GetDexFile();
  ClassLinker* class_linker = mUnit->GetClassLinker();
  uint32_t referrer_class_idx = dex_file->GetMethodId(mUnit->GetDexMethodIndex()).class_idx_;

  // We're going to resolve fields and check access in a tight loop. It's better to hold
  // the lock and needed references once than re-acquiring them again and again.
  ScopedObjectAccess soa(Thread::Current());
  SirtRef<mirror::DexCache> dex_cache(soa.Self(), class_linker->FindDexCache(*dex_file));
  SirtRef<mirror::ClassLoader> class_loader(
      soa.Self(), soa.Decode<mirror::ClassLoader*>(mUnit->GetClassLoader()));
  SirtRef<mirror::Class> referrer_class(soa.Self(),
      class_linker->ResolveType(*dex_file, referrer_class_idx, dex_cache, class_loader));
  if (UNLIKELY(referrer_class.get() == nullptr)) {
    // Clean up any exception left by type resolution
    DCHECK(soa.Self()->IsExceptionPending());
    soa.Self()->ClearException();
    // We're compiling a method without class definition. We may still resolve fields
    // and update annotations, so fall through and check again in the loop.
  }

  for (auto it = annotations, end = annotations + count; it != end; ++it) {
    uint32_t field_idx = it->field_idx_;
    mirror::ArtField* resolved_field =
        class_linker->ResolveField(*dex_file, field_idx, dex_cache, class_loader, false);
    if (UNLIKELY(resolved_field == nullptr)) {
      DCHECK(soa.Self()->IsExceptionPending());
      soa.Self()->ClearException();
      continue;
    }
    DCHECK(!soa.Self()->IsExceptionPending());
    if (UNLIKELY(resolved_field->IsStatic())) {
      continue;
    }
    mirror::Class* fields_class = resolved_field->GetDeclaringClass();
    it->is_volatile_ = resolved_field->IsVolatile() ? 1u : 0u;
    it->field_offset_ = resolved_field->GetOffset();
    it->declaring_dex_file_ = fields_class->GetDexCache()->GetDexFile();
    it->declaring_class_idx_ = fields_class->GetDexTypeIndex();
    it->declaring_field_idx_ = resolved_field->GetDexFieldIndex();
    if (UNLIKELY(referrer_class.get() == nullptr)) {
      continue;
    }
    if (referrer_class->CanAccessResolvedField(fields_class, resolved_field,
                                               dex_cache.get(), field_idx)) {
      it->fast_get_ = 1u;
      if (!resolved_field->IsFinal() || fields_class == referrer_class.get()) {
        it->fast_put_ = 1u;
      }
    }
  }
}

void SFieldAnnotation::Resolve(const DexCompilationUnit* mUnit,
                               SFieldAnnotation* annotations, size_t count) {
  if (kIsDebugBuild) {
    DCHECK(annotations != nullptr);
    DCHECK_NE(count, 0u);
    for (auto it = annotations, end = annotations + count; it != end; ++it) {
      SFieldAnnotation unresolved(it->field_idx_);
      DCHECK_EQ(memcmp(&unresolved, &*it, sizeof(*it)), 0);
    }
  }

  const DexFile* dex_file = mUnit->GetDexFile();
  ClassLinker* class_linker = mUnit->GetClassLinker();
  uint32_t referrer_class_idx = dex_file->GetMethodId(mUnit->GetDexMethodIndex()).class_idx_;

  // We're going to resolve fields and check access in a tight loop. It's better to hold
  // the lock and needed references once than re-acquiring them again and again.
  ScopedObjectAccess soa(Thread::Current());
  SirtRef<mirror::DexCache> dex_cache(soa.Self(), class_linker->FindDexCache(*dex_file));
  SirtRef<mirror::ClassLoader> class_loader(
      soa.Self(), soa.Decode<mirror::ClassLoader*>(mUnit->GetClassLoader()));
  SirtRef<mirror::Class> referrer_class(soa.Self(),
      class_linker->ResolveType(*dex_file, referrer_class_idx, dex_cache, class_loader));
  if (UNLIKELY(referrer_class.get() == nullptr)) {
    DCHECK(soa.Self()->IsExceptionPending());
    soa.Self()->ClearException();
    // We're compiling a method without class definition. We may still resolve fields
    // and update annotations, so fall through and check again in the loop.
  }

  for (auto it = annotations, end = annotations + count; it != end; ++it) {
    uint32_t field_idx = it->field_idx_;
    mirror::ArtField* resolved_field =
        class_linker->ResolveField(*dex_file, field_idx, dex_cache, class_loader, true);
    if (UNLIKELY(resolved_field == nullptr)) {
      // Clean up the exception left by field resolution
      DCHECK(soa.Self()->IsExceptionPending());
      soa.Self()->ClearException();
      continue;
    }
    DCHECK(!soa.Self()->IsExceptionPending());
    if (UNLIKELY(!resolved_field->IsStatic())) {
      continue;
    }
    mirror::Class* fields_class = resolved_field->GetDeclaringClass();
    it->is_volatile_ = resolved_field->IsVolatile() ? 1u : 0u;
    it->field_offset_ = resolved_field->GetOffset();
    it->declaring_dex_file_ = fields_class->GetDexCache()->GetDexFile();
    it->declaring_class_idx_ = fields_class->GetDexTypeIndex();
    it->declaring_field_idx_ = resolved_field->GetDexFieldIndex();
    if (UNLIKELY(referrer_class.get() == nullptr)) {
      continue;
    }
    if (fields_class == referrer_class.get()) {
      it->fast_get_ = 1u;
      it->fast_put_ = 1u;
      it->is_referrers_class_ = 1u;  // implies no worrying about class initialization
      it->is_initialized_ = 1u;
      it->storage_index_ = fields_class->GetDexTypeIndex();
      continue;
    }
    if (referrer_class->CanAccessResolvedField(fields_class, resolved_field,
                                               dex_cache.get(), field_idx)) {
      // We have the resolved field, we must make it into a index for the referrer
      // in its static storage (which may fail if it doesn't have a slot for it)
      // TODO: for images we can elide the static storage base null check
      // if we know there's a non-null entry in the image
      if (LIKELY(fields_class->GetDexCache() == dex_cache.get())) {
        // common case where the dex cache of both the referrer and the field are the same,
        // no need to search the dex file
        it->storage_index_ = fields_class->GetDexTypeIndex();
      } else {
        // Search dex file for localized ssb index, may fail if field's class is a parent
        // of the class mentioned in the dex file and there is no dex cache entry.
        const DexFile::StringId* string_id =
            dex_file->FindStringId(FieldHelper(resolved_field).GetDeclaringClassDescriptor());
        if (string_id == nullptr) {
          continue;
        }
        const DexFile::TypeId* type_id =
           dex_file->FindTypeId(dex_file->GetIndexForStringId(*string_id));
        if (type_id == nullptr) {
          continue;
        }
        // medium path, needs check of static storage base being initialized
        it->storage_index_ = dex_file->GetIndexForTypeId(*type_id);
      }
      it->fast_get_ = 1u;
      it->fast_put_ = resolved_field->IsFinal() ? 0u : 1u;
      DCHECK_EQ(it->is_referrers_class_, 0u);
      it->is_initialized_ = fields_class->IsInitialized() &&
          mUnit->GetCompilationUnit()->compiler_driver->CanAssumeTypeIsPresentInDexCache(
              *dex_file, it->storage_index_);
    }
  }
}

}  // namespace art
