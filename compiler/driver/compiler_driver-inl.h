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

#include "art_field-inl.h"
#include "art_method-inl.h"
#include "class_linker-inl.h"
#include "dex_compilation_unit.h"
#include "mirror/class_loader.h"
#include "mirror/dex_cache-inl.h"
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

inline mirror::Class* CompilerDriver::ResolveClass(
    const ScopedObjectAccess& soa, Handle<mirror::DexCache> dex_cache,
    Handle<mirror::ClassLoader> class_loader, uint16_t cls_index,
    const DexCompilationUnit* mUnit) {
  DCHECK_EQ(dex_cache->GetDexFile(), mUnit->GetDexFile());
  DCHECK_EQ(class_loader.Get(), soa.Decode<mirror::ClassLoader*>(mUnit->GetClassLoader()));
  mirror::Class* cls = mUnit->GetClassLinker()->ResolveType(
      *mUnit->GetDexFile(), cls_index, dex_cache, class_loader);
  DCHECK_EQ(cls == nullptr, soa.Self()->IsExceptionPending());
  if (UNLIKELY(cls == nullptr)) {
    // Clean up any exception left by type resolution.
    soa.Self()->ClearException();
  }
  return cls;
}

inline mirror::Class* CompilerDriver::ResolveCompilingMethodsClass(
    const ScopedObjectAccess& soa, Handle<mirror::DexCache> dex_cache,
    Handle<mirror::ClassLoader> class_loader, const DexCompilationUnit* mUnit) {
  DCHECK_EQ(dex_cache->GetDexFile(), mUnit->GetDexFile());
  DCHECK_EQ(class_loader.Get(), soa.Decode<mirror::ClassLoader*>(mUnit->GetClassLoader()));
  const DexFile::MethodId& referrer_method_id =
      mUnit->GetDexFile()->GetMethodId(mUnit->GetDexMethodIndex());
  return ResolveClass(soa, dex_cache, class_loader, referrer_method_id.class_idx_, mUnit);
}

inline ArtField* CompilerDriver::ResolveFieldWithDexFile(
    const ScopedObjectAccess& soa, Handle<mirror::DexCache> dex_cache,
    Handle<mirror::ClassLoader> class_loader, const DexFile* dex_file,
    uint32_t field_idx, bool is_static) {
  DCHECK_EQ(dex_cache->GetDexFile(), dex_file);
  ArtField* resolved_field = Runtime::Current()->GetClassLinker()->ResolveField(
      *dex_file, field_idx, dex_cache, class_loader, is_static);
  DCHECK_EQ(resolved_field == nullptr, soa.Self()->IsExceptionPending());
  if (UNLIKELY(resolved_field == nullptr)) {
    // Clean up any exception left by type resolution.
    soa.Self()->ClearException();
    return nullptr;
  }
  if (UNLIKELY(resolved_field->IsStatic() != is_static)) {
    // ClassLinker can return a field of the wrong kind directly from the DexCache.
    // Silently return null on such incompatible class change.
    return nullptr;
  }
  return resolved_field;
}

inline mirror::DexCache* CompilerDriver::FindDexCache(const DexFile* dex_file) {
  return Runtime::Current()->GetClassLinker()->FindDexCache(*dex_file);
}

inline ArtField* CompilerDriver::ResolveField(
    const ScopedObjectAccess& soa, Handle<mirror::DexCache> dex_cache,
    Handle<mirror::ClassLoader> class_loader, const DexCompilationUnit* mUnit,
    uint32_t field_idx, bool is_static) {
  DCHECK_EQ(class_loader.Get(), soa.Decode<mirror::ClassLoader*>(mUnit->GetClassLoader()));
  return ResolveFieldWithDexFile(soa, dex_cache, class_loader, mUnit->GetDexFile(), field_idx,
                                 is_static);
}

inline void CompilerDriver::GetResolvedFieldDexFileLocation(
    ArtField* resolved_field, const DexFile** declaring_dex_file,
    uint16_t* declaring_class_idx, uint16_t* declaring_field_idx) {
  mirror::Class* declaring_class = resolved_field->GetDeclaringClass();
  *declaring_dex_file = declaring_class->GetDexCache()->GetDexFile();
  *declaring_class_idx = declaring_class->GetDexTypeIndex();
  *declaring_field_idx = resolved_field->GetDexFieldIndex();
}

inline bool CompilerDriver::IsFieldVolatile(ArtField* field) {
  return field->IsVolatile();
}

inline MemberOffset CompilerDriver::GetFieldOffset(ArtField* field) {
  return field->GetOffset();
}

inline std::pair<bool, bool> CompilerDriver::IsFastInstanceField(
    mirror::DexCache* dex_cache, mirror::Class* referrer_class,
    ArtField* resolved_field, uint16_t field_idx) {
  DCHECK(!resolved_field->IsStatic());
  mirror::Class* fields_class = resolved_field->GetDeclaringClass();
  // Keep these classes in sync with prepareSubclassReplacement() calls in libxposed-art.
  mirror::Class* super_class = fields_class->GetSuperClass();
  while (super_class != nullptr) {
    if (super_class->DescriptorEquals("Landroid/content/res/TypedArray;")) {
      VLOG(compiler) << "Preventing fast access to " << PrettyField(resolved_field);
      return std::make_pair(false, false);
    }
    super_class = super_class->GetSuperClass();
  }
  bool fast_get = referrer_class != nullptr &&
      referrer_class->CanAccessResolvedField(fields_class, resolved_field,
                                             dex_cache, field_idx);
  bool fast_put = fast_get && (!resolved_field->IsFinal() || fields_class == referrer_class);
  return std::make_pair(fast_get, fast_put);
}

template <typename ArtMember>
inline bool CompilerDriver::CanAccessResolvedMember(mirror::Class* referrer_class ATTRIBUTE_UNUSED,
                                                    mirror::Class* access_to ATTRIBUTE_UNUSED,
                                                    ArtMember* member ATTRIBUTE_UNUSED,
                                                    mirror::DexCache* dex_cache ATTRIBUTE_UNUSED,
                                                    uint32_t field_idx ATTRIBUTE_UNUSED) {
  // Not defined for ArtMember values other than ArtField or ArtMethod.
  UNREACHABLE();
}

template <>
inline bool CompilerDriver::CanAccessResolvedMember<ArtField>(mirror::Class* referrer_class,
                                                              mirror::Class* access_to,
                                                              ArtField* field,
                                                              mirror::DexCache* dex_cache,
                                                              uint32_t field_idx) {
  return referrer_class->CanAccessResolvedField(access_to, field, dex_cache, field_idx);
}

template <>
inline bool CompilerDriver::CanAccessResolvedMember<ArtMethod>(
    mirror::Class* referrer_class,
    mirror::Class* access_to,
    ArtMethod* method,
    mirror::DexCache* dex_cache,
    uint32_t field_idx) {
  return referrer_class->CanAccessResolvedMethod(access_to, method, dex_cache, field_idx);
}

template <typename ArtMember>
inline std::pair<bool, bool> CompilerDriver::IsClassOfStaticMemberAvailableToReferrer(
    mirror::DexCache* dex_cache,
    mirror::Class* referrer_class,
    ArtMember* resolved_member,
    uint16_t member_idx,
    uint32_t* storage_index) {
  DCHECK(resolved_member->IsStatic());
  if (LIKELY(referrer_class != nullptr)) {
    mirror::Class* members_class = resolved_member->GetDeclaringClass();
    if (members_class == referrer_class) {
      *storage_index = members_class->GetDexTypeIndex();
      return std::make_pair(true, true);
    }
    if (CanAccessResolvedMember<ArtMember>(
            referrer_class, members_class, resolved_member, dex_cache, member_idx)) {
      // We have the resolved member, we must make it into a index for the referrer
      // in its static storage (which may fail if it doesn't have a slot for it)
      // TODO: for images we can elide the static storage base null check
      // if we know there's a non-null entry in the image
      const DexFile* dex_file = dex_cache->GetDexFile();
      uint32_t storage_idx = DexFile::kDexNoIndex;
      if (LIKELY(members_class->GetDexCache() == dex_cache)) {
        // common case where the dex cache of both the referrer and the member are the same,
        // no need to search the dex file
        storage_idx = members_class->GetDexTypeIndex();
      } else {
        // Search dex file for localized ssb index, may fail if member's class is a parent
        // of the class mentioned in the dex file and there is no dex cache entry.
        std::string temp;
        const DexFile::StringId* string_id =
            dex_file->FindStringId(resolved_member->GetDeclaringClass()->GetDescriptor(&temp));
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
        *storage_index = storage_idx;
        return std::make_pair(true, !resolved_member->IsFinal());
      }
    }
  }
  // Conservative defaults.
  *storage_index = DexFile::kDexNoIndex;
  return std::make_pair(false, false);
}

inline std::pair<bool, bool> CompilerDriver::IsFastStaticField(
    mirror::DexCache* dex_cache, mirror::Class* referrer_class,
    ArtField* resolved_field, uint16_t field_idx, uint32_t* storage_index) {
  return IsClassOfStaticMemberAvailableToReferrer(
      dex_cache, referrer_class, resolved_field, field_idx, storage_index);
}

inline bool CompilerDriver::IsClassOfStaticMethodAvailableToReferrer(
    mirror::DexCache* dex_cache, mirror::Class* referrer_class,
    ArtMethod* resolved_method, uint16_t method_idx, uint32_t* storage_index) {
  std::pair<bool, bool> result = IsClassOfStaticMemberAvailableToReferrer(
      dex_cache, referrer_class, resolved_method, method_idx, storage_index);
  // Only the first member of `result` is meaningful, as there is no
  // "write access" to a method.
  return result.first;
}

inline bool CompilerDriver::IsStaticFieldInReferrerClass(mirror::Class* referrer_class,
                                                         ArtField* resolved_field) {
  DCHECK(resolved_field->IsStatic());
  mirror::Class* fields_class = resolved_field->GetDeclaringClass();
  return referrer_class == fields_class;
}

inline bool CompilerDriver::CanAssumeClassIsInitialized(mirror::Class* klass) {
  // Being loaded is a pre-requisite for being initialized but let's do the cheap check first.
  //
  // NOTE: When AOT compiling an app, we eagerly initialize app classes (and potentially their
  // super classes in the boot image) but only those that have a trivial initialization, i.e.
  // without <clinit>() or static values in the dex file for that class or any of its super
  // classes. So while we could see the klass as initialized during AOT compilation and have
  // it only loaded at runtime, the needed initialization would have to be trivial and
  // unobservable from Java, so we may as well treat it as initialized.
  if (!klass->IsInitialized()) {
    return false;
  }
  return CanAssumeClassIsLoaded(klass);
}

inline bool CompilerDriver::CanReferrerAssumeClassIsInitialized(mirror::Class* referrer_class,
                                                                mirror::Class* klass) {
  return (referrer_class != nullptr
          && !referrer_class->IsInterface()
          && referrer_class->IsSubClass(klass))
      || CanAssumeClassIsInitialized(klass);
}

inline bool CompilerDriver::IsStaticFieldsClassInitialized(mirror::Class* referrer_class,
                                                           ArtField* resolved_field) {
  DCHECK(resolved_field->IsStatic());
  mirror::Class* fields_class = resolved_field->GetDeclaringClass();
  return CanReferrerAssumeClassIsInitialized(referrer_class, fields_class);
}

inline ArtMethod* CompilerDriver::ResolveMethod(
    ScopedObjectAccess& soa, Handle<mirror::DexCache> dex_cache,
    Handle<mirror::ClassLoader> class_loader, const DexCompilationUnit* mUnit,
    uint32_t method_idx, InvokeType invoke_type, bool check_incompatible_class_change) {
  DCHECK_EQ(dex_cache->GetDexFile(), mUnit->GetDexFile());
  DCHECK_EQ(class_loader.Get(), soa.Decode<mirror::ClassLoader*>(mUnit->GetClassLoader()));
  ArtMethod* resolved_method = mUnit->GetClassLinker()->ResolveMethod(
      *mUnit->GetDexFile(), method_idx, dex_cache, class_loader, nullptr, invoke_type);
  DCHECK_EQ(resolved_method == nullptr, soa.Self()->IsExceptionPending());
  if (UNLIKELY(resolved_method == nullptr)) {
    // Clean up any exception left by type resolution.
    soa.Self()->ClearException();
    return nullptr;
  }
  if (check_incompatible_class_change &&
      UNLIKELY(resolved_method->CheckIncompatibleClassChange(invoke_type))) {
    // Silently return null on incompatible class change.
    return nullptr;
  }
  return resolved_method;
}

inline void CompilerDriver::GetResolvedMethodDexFileLocation(
    ArtMethod* resolved_method, const DexFile** declaring_dex_file,
    uint16_t* declaring_class_idx, uint16_t* declaring_method_idx) {
  mirror::Class* declaring_class = resolved_method->GetDeclaringClass();
  *declaring_dex_file = declaring_class->GetDexCache()->GetDexFile();
  *declaring_class_idx = declaring_class->GetDexTypeIndex();
  *declaring_method_idx = resolved_method->GetDexMethodIndex();
}

inline uint16_t CompilerDriver::GetResolvedMethodVTableIndex(
    ArtMethod* resolved_method, InvokeType type) {
  if (type == kVirtual || type == kSuper) {
    return resolved_method->GetMethodIndex();
  } else if (type == kInterface) {
    return resolved_method->GetDexMethodIndex();
  } else {
    return DexFile::kDexNoIndex16;
  }
}

inline int CompilerDriver::IsFastInvoke(
    ScopedObjectAccess& soa, Handle<mirror::DexCache> dex_cache,
    Handle<mirror::ClassLoader> class_loader, const DexCompilationUnit* mUnit,
    mirror::Class* referrer_class, ArtMethod* resolved_method, InvokeType* invoke_type,
    MethodReference* target_method, const MethodReference* devirt_target,
    uintptr_t* direct_code, uintptr_t* direct_method, bool is_quickened) {
  // Don't try to fast-path if we don't understand the caller's class.
  if (UNLIKELY(referrer_class == nullptr)) {
    return 0;
  }
  // Quickened calls are already sharpened, possibly to classes that are not accessible.
  // Skip access checks and further attempts to sharpen the call.
  if (is_quickened) {
    return kFlagMethodResolved;
  }
  mirror::Class* methods_class = resolved_method->GetDeclaringClass();
  if (UNLIKELY(!referrer_class->CanAccessResolvedMethod(methods_class, resolved_method,
                                                        dex_cache.Get(),
                                                        target_method->dex_method_index))) {
    return 0;
  }
  // Sharpen a virtual call into a direct call when the target is known not to have been
  // overridden (ie is final).
  const bool same_dex_file = target_method->dex_file == mUnit->GetDexFile();
  bool can_sharpen_virtual_based_on_type = same_dex_file &&
      (*invoke_type == kVirtual) && (resolved_method->IsFinal() || methods_class->IsFinal());
  // For invoke-super, ensure the vtable index will be correct to dispatch in the vtable of
  // the super class.
  const size_t pointer_size = InstructionSetPointerSize(GetInstructionSet());
  bool can_sharpen_super_based_on_type = same_dex_file && (*invoke_type == kSuper) &&
      (referrer_class != methods_class) && referrer_class->IsSubClass(methods_class) &&
      resolved_method->GetMethodIndex() < methods_class->GetVTableLength() &&
      (methods_class->GetVTableEntry(
          resolved_method->GetMethodIndex(), pointer_size) == resolved_method) &&
      !resolved_method->IsAbstract();

  if (can_sharpen_virtual_based_on_type || can_sharpen_super_based_on_type) {
    // Sharpen a virtual call into a direct call. The method_idx is into referrer's
    // dex cache, check that this resolved method is where we expect it.
    CHECK_EQ(target_method->dex_file, mUnit->GetDexFile());
    DCHECK_EQ(dex_cache.Get(), mUnit->GetClassLinker()->FindDexCache(*mUnit->GetDexFile()));
    CHECK_EQ(referrer_class->GetDexCache()->GetResolvedMethod(
        target_method->dex_method_index, pointer_size),
             resolved_method) << PrettyMethod(resolved_method);
    int stats_flags = kFlagMethodResolved;
    GetCodeAndMethodForDirectCall(/*out*/invoke_type,
                                  kDirect,  // Sharp type
                                  false,    // The dex cache is guaranteed to be available
                                  referrer_class, resolved_method,
                                  /*out*/&stats_flags,
                                  target_method,
                                  /*out*/direct_code,
                                  /*out*/direct_method);
    DCHECK_NE(*invoke_type, kSuper) << PrettyMethod(resolved_method);
    if (*invoke_type == kDirect) {
      stats_flags |= kFlagsMethodResolvedVirtualMadeDirect;
    }
    return stats_flags;
  }

  if ((*invoke_type == kVirtual || *invoke_type == kInterface) && devirt_target != nullptr) {
    // Post-verification callback recorded a more precise invoke target based on its type info.
    ArtMethod* called_method;
    ClassLinker* class_linker = mUnit->GetClassLinker();
    if (LIKELY(devirt_target->dex_file == mUnit->GetDexFile())) {
      called_method = class_linker->ResolveMethod(
          *devirt_target->dex_file, devirt_target->dex_method_index, dex_cache, class_loader,
          nullptr, kVirtual);
    } else {
      StackHandleScope<1> hs(soa.Self());
      auto target_dex_cache(hs.NewHandle(class_linker->FindDexCache(*devirt_target->dex_file)));
      called_method = class_linker->ResolveMethod(
          *devirt_target->dex_file, devirt_target->dex_method_index, target_dex_cache,
          class_loader, nullptr, kVirtual);
    }
    CHECK(called_method != nullptr);
    CHECK(!called_method->IsAbstract());
    int stats_flags = kFlagMethodResolved;
    GetCodeAndMethodForDirectCall(/*out*/invoke_type,
                                  kDirect,  // Sharp type
                                  true,     // The dex cache may not be available
                                  referrer_class, called_method,
                                  /*out*/&stats_flags,
                                  target_method,
                                  /*out*/direct_code,
                                  /*out*/direct_method);
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
  GetCodeAndMethodForDirectCall(/*out*/invoke_type,
                                *invoke_type,  // Sharp type
                                false,         // The dex cache is guaranteed to be available
                                referrer_class, resolved_method,
                                /*out*/&stats_flags,
                                target_method,
                                /*out*/direct_code,
                                /*out*/direct_method);
  return stats_flags;
}

inline bool CompilerDriver::IsMethodsClassInitialized(mirror::Class* referrer_class,
                                                      ArtMethod* resolved_method) {
  if (!resolved_method->IsStatic()) {
    return true;
  }
  mirror::Class* methods_class = resolved_method->GetDeclaringClass();
  return CanReferrerAssumeClassIsInitialized(referrer_class, methods_class);
}

}  // namespace art

#endif  // ART_COMPILER_DRIVER_COMPILER_DRIVER_INL_H_
