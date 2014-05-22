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
#include "mirror/art_method.h"
#include "mirror/art_method-inl.h"
#include "mirror/class_loader.h"
#include "mirror/dex_cache.h"
#include "mirror/dex_cache-inl.h"
#include "mirror/art_field-inl.h"
#include "scoped_thread_state_change.h"
#include "handle_scope-inl.h"

namespace art {

inline mirror::DexCache* CompilerDriver::GetDexCache(const DexCompilationUnit* mUnit) {
  return mUnit->GetClassLinker()->FindDexCache(*mUnit->GetDexFile());
}

inline mirror::ClassLoader* CompilerDriver::GetClassLoader(ScopedObjectAccess& soa,
                                                           const DexCompilationUnit* mUnit) {
  return soa.Decode<mirror::ClassLoader*>(mUnit->GetClassLoader());
}

inline mirror::Class* CompilerDriver::ResolveCompilingMethodsClass(
    ScopedObjectAccess& soa, const Handle<mirror::DexCache>& dex_cache,
    const Handle<mirror::ClassLoader>& class_loader, const DexCompilationUnit* mUnit) {
  DCHECK_EQ(dex_cache->GetDexFile(), mUnit->GetDexFile());
  DCHECK_EQ(class_loader.Get(), soa.Decode<mirror::ClassLoader*>(mUnit->GetClassLoader()));
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
    ScopedObjectAccess& soa, const Handle<mirror::DexCache>& dex_cache,
    const Handle<mirror::ClassLoader>& class_loader, const DexCompilationUnit* mUnit,
    uint32_t field_idx, bool is_static) {
  DCHECK_EQ(dex_cache->GetDexFile(), mUnit->GetDexFile());
  DCHECK_EQ(class_loader.Get(), soa.Decode<mirror::ClassLoader*>(mUnit->GetClassLoader()));
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

inline mirror::ArtMethod* CompilerDriver::ResolveMethod(
    ScopedObjectAccess& soa, const Handle<mirror::DexCache>& dex_cache,
    const Handle<mirror::ClassLoader>& class_loader, const DexCompilationUnit* mUnit,
    uint32_t method_idx, InvokeType invoke_type) {
  DCHECK(dex_cache->GetDexFile() == mUnit->GetDexFile());
  DCHECK(class_loader.Get() == soa.Decode<mirror::ClassLoader*>(mUnit->GetClassLoader()));
  mirror::ArtMethod* resolved_method = mUnit->GetClassLinker()->ResolveMethod(
      *mUnit->GetDexFile(), method_idx, dex_cache, class_loader, nullptr, invoke_type);
  DCHECK_EQ(resolved_method == nullptr, soa.Self()->IsExceptionPending());
  if (UNLIKELY(resolved_method == nullptr)) {
    // Clean up any exception left by type resolution.
    soa.Self()->ClearException();
    return nullptr;
  }
  if (UNLIKELY(resolved_method->CheckIncompatibleClassChange(invoke_type))) {
    // Silently return nullptr on incompatible class change.
    return nullptr;
  }
  return resolved_method;
}

inline void CompilerDriver::GetResolvedMethodDexFileLocation(
    mirror::ArtMethod* resolved_method, const DexFile** declaring_dex_file,
    uint16_t* declaring_class_idx, uint16_t* declaring_method_idx) {
  mirror::Class* declaring_class = resolved_method->GetDeclaringClass();
  *declaring_dex_file = declaring_class->GetDexCache()->GetDexFile();
  *declaring_class_idx = declaring_class->GetDexTypeIndex();
  *declaring_method_idx = resolved_method->GetDexMethodIndex();
}

inline uint16_t CompilerDriver::GetResolvedMethodVTableIndex(
    mirror::ArtMethod* resolved_method, InvokeType type) {
  if (type == kVirtual || type == kSuper) {
    return resolved_method->GetMethodIndex();
  } else if (type == kInterface) {
    return resolved_method->GetDexMethodIndex();
  } else {
    return DexFile::kDexNoIndex16;
  }
}

inline int CompilerDriver::IsFastInvoke(
    ScopedObjectAccess& soa, const Handle<mirror::DexCache>& dex_cache,
    const Handle<mirror::ClassLoader>& class_loader, const DexCompilationUnit* mUnit,
    mirror::Class* referrer_class, mirror::ArtMethod* resolved_method, InvokeType* invoke_type,
    MethodReference* target_method, const MethodReference* devirt_target,
    uintptr_t* direct_code, uintptr_t* direct_method) {
  // Don't try to fast-path if we don't understand the caller's class.
  if (UNLIKELY(referrer_class == nullptr)) {
    return 0;
  }
  mirror::Class* methods_class = resolved_method->GetDeclaringClass();
  if (UNLIKELY(!referrer_class->CanAccessResolvedMethod(methods_class, resolved_method,
                                                        dex_cache.Get(),
                                                        target_method->dex_method_index))) {
    return 0;
  }

  // Sharpen a virtual call into a direct call when the target is known not to have been
  // overridden (ie is final).
  bool can_sharpen_virtual_based_on_type =
      (*invoke_type == kVirtual) && (resolved_method->IsFinal() || methods_class->IsFinal());
  // For invoke-super, ensure the vtable index will be correct to dispatch in the vtable of
  // the super class.
  bool can_sharpen_super_based_on_type = (*invoke_type == kSuper) &&
      (referrer_class != methods_class) && referrer_class->IsSubClass(methods_class) &&
      resolved_method->GetMethodIndex() < methods_class->GetVTable()->GetLength() &&
      (methods_class->GetVTable()->Get(resolved_method->GetMethodIndex()) == resolved_method);

  if (can_sharpen_virtual_based_on_type || can_sharpen_super_based_on_type) {
    // Sharpen a virtual call into a direct call. The method_idx is into referrer's
    // dex cache, check that this resolved method is where we expect it.
    CHECK(target_method->dex_file == mUnit->GetDexFile());
    DCHECK(dex_cache.Get() == mUnit->GetClassLinker()->FindDexCache(*mUnit->GetDexFile()));
    CHECK(referrer_class->GetDexCache()->GetResolvedMethod(target_method->dex_method_index) ==
        resolved_method) << PrettyMethod(resolved_method);
    int stats_flags = kFlagMethodResolved;
    GetCodeAndMethodForDirectCall(invoke_type, kDirect, false, referrer_class, resolved_method,
                                  &stats_flags, target_method, direct_code, direct_method);
    DCHECK_NE(*invoke_type, kSuper) << PrettyMethod(resolved_method);
    if (*invoke_type == kDirect) {
      stats_flags |= kFlagsMethodResolvedVirtualMadeDirect;
    }
    return stats_flags;
  }

  if ((*invoke_type == kVirtual || *invoke_type == kInterface) && devirt_target != nullptr) {
    // Post-verification callback recorded a more precise invoke target based on its type info.
    mirror::ArtMethod* called_method;
    ClassLinker* class_linker = mUnit->GetClassLinker();
    if (LIKELY(devirt_target->dex_file == mUnit->GetDexFile())) {
      called_method = class_linker->ResolveMethod(*devirt_target->dex_file,
                                                  devirt_target->dex_method_index,
                                                  dex_cache, class_loader, NULL, kVirtual);
    } else {
      StackHandleScope<1> hs(soa.Self());
      Handle<mirror::DexCache> target_dex_cache(
          hs.NewHandle(class_linker->FindDexCache(*devirt_target->dex_file)));
      called_method = class_linker->ResolveMethod(*devirt_target->dex_file,
                                                  devirt_target->dex_method_index,
                                                  target_dex_cache, class_loader, NULL, kVirtual);
    }
    CHECK(called_method != NULL);
    CHECK(!called_method->IsAbstract());
    int stats_flags = kFlagMethodResolved;
    GetCodeAndMethodForDirectCall(invoke_type, kDirect, true, referrer_class, called_method,
                                  &stats_flags, target_method, direct_code, direct_method);
    DCHECK_NE(*invoke_type, kSuper);
    if (*invoke_type == kDirect) {
      stats_flags |= kFlagsMethodResolvedPreciseTypeDevirtualization;
    }
    return stats_flags;
  }

  if (UNLIKELY(*invoke_type == kSuper)) {
    // Unsharpened super calls are suspicious so go slow-path.
    return 0;
  }

  // Sharpening failed so generate a regular resolved method dispatch.
  int stats_flags = kFlagMethodResolved;
  GetCodeAndMethodForDirectCall(invoke_type, *invoke_type, false, referrer_class, resolved_method,
                                &stats_flags, target_method, direct_code, direct_method);
  return stats_flags;
}

inline bool CompilerDriver::NeedsClassInitialization(mirror::Class* referrer_class,
                                                     mirror::ArtMethod* resolved_method) {
  if (!resolved_method->IsStatic()) {
    return false;
  }
  mirror::Class* methods_class = resolved_method->GetDeclaringClass();
  // NOTE: Unlike in IsFastStaticField(), we don't check CanAssumeTypeIsPresentInDexCache() here.
  return methods_class != referrer_class && !methods_class->IsInitialized();
}

}  // namespace art

#endif  // ART_COMPILER_DRIVER_COMPILER_DRIVER_INL_H_
