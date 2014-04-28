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

#ifndef ART_RUNTIME_MIRROR_ART_METHOD_INL_H_
#define ART_RUNTIME_MIRROR_ART_METHOD_INL_H_

#include "art_method.h"

#include "dex_file.h"
#include "entrypoints/entrypoint_utils.h"
#include "object_array.h"
#include "oat.h"
#include "runtime.h"

namespace art {
namespace mirror {

inline Class* ArtMethod::GetDeclaringClass() {
  Class* result = GetFieldObject<Class>(OFFSET_OF_OBJECT_MEMBER(ArtMethod, declaring_class_),
                                        false);
  DCHECK(result != NULL) << this;
  DCHECK(result->IsIdxLoaded() || result->IsErroneous()) << this;
  return result;
}

inline void ArtMethod::SetDeclaringClass(Class *new_declaring_class) {
  SetFieldObject<false>(OFFSET_OF_OBJECT_MEMBER(ArtMethod, declaring_class_),
                        new_declaring_class, false);
}

inline uint32_t ArtMethod::GetAccessFlags() {
  DCHECK(GetDeclaringClass()->IsIdxLoaded() || GetDeclaringClass()->IsErroneous());
  return GetField32(OFFSET_OF_OBJECT_MEMBER(ArtMethod, access_flags_), false);
}

inline uint16_t ArtMethod::GetMethodIndex() {
  DCHECK(GetDeclaringClass()->IsResolved() || GetDeclaringClass()->IsErroneous());
  return GetField32(OFFSET_OF_OBJECT_MEMBER(ArtMethod, method_index_), false);
}

inline uint32_t ArtMethod::GetDexMethodIndex() {
#ifdef ART_SEA_IR_MODE
  // TODO: Re-add this check for (PORTABLE + SMALL + ) SEA IR when PORTABLE IS fixed!
  // DCHECK(GetDeclaringClass()->IsLoaded() || GetDeclaringClass()->IsErroneous());
#else
  DCHECK(GetDeclaringClass()->IsLoaded() || GetDeclaringClass()->IsErroneous());
#endif
  return GetField32(OFFSET_OF_OBJECT_MEMBER(ArtMethod, dex_method_index_), false);
}

inline ObjectArray<String>* ArtMethod::GetDexCacheStrings() {
  return GetFieldObject<ObjectArray<String> >(
      OFFSET_OF_OBJECT_MEMBER(ArtMethod, dex_cache_strings_), false);
}

inline ObjectArray<ArtMethod>* ArtMethod::GetDexCacheResolvedMethods() {
  return GetFieldObject<ObjectArray<ArtMethod> >(
      OFFSET_OF_OBJECT_MEMBER(ArtMethod, dex_cache_resolved_methods_), false);
}

inline ObjectArray<Class>* ArtMethod::GetDexCacheResolvedTypes() {
  return GetFieldObject<ObjectArray<Class> >(
      OFFSET_OF_OBJECT_MEMBER(ArtMethod, dex_cache_resolved_types_), false);
}

inline uint32_t ArtMethod::GetCodeSize() {
  DCHECK(!IsRuntimeMethod() && !IsProxyMethod()) << PrettyMethod(this);
  const void* code = EntryPointToCodePointer(GetEntryPointFromQuickCompiledCode());
  if (code == nullptr) {
    return 0u;
  }
  return reinterpret_cast<const OatMethodHeader*>(code)[-1].code_size_;
}

inline bool ArtMethod::CheckIncompatibleClassChange(InvokeType type) {
  switch (type) {
    case kStatic:
      return !IsStatic();
    case kDirect:
      return !IsDirect() || IsStatic();
    case kVirtual: {
      Class* methods_class = GetDeclaringClass();
      return IsDirect() || (methods_class->IsInterface() && !IsMiranda());
    }
    case kSuper:
      return false;  // TODO: appropriate checks for call to super class.
    case kInterface: {
      Class* methods_class = GetDeclaringClass();
      return IsDirect() || !(methods_class->IsInterface() || methods_class->IsObjectClass());
    }
    default:
      LOG(FATAL) << "Unreachable - invocation type: " << type;
      return true;
  }
}

inline void ArtMethod::AssertPcIsWithinQuickCode(uintptr_t pc) {
  if (!kIsDebugBuild) {
    return;
  }
  if (IsNative() || IsRuntimeMethod() || IsProxyMethod()) {
    return;
  }
  if (pc == GetQuickInstrumentationExitPc()) {
    return;
  }
  const void* code = GetEntryPointFromQuickCompiledCode();
  if (code == GetQuickToInterpreterBridge() || code == GetQuickInstrumentationEntryPoint()) {
    return;
  }
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  if (code == GetQuickResolutionTrampoline(class_linker) ||
      code == GetQuickToInterpreterBridgeTrampoline(class_linker)) {
    return;
  }
  DCHECK(IsWithinQuickCode(pc))
      << PrettyMethod(this)
      << " pc=" << std::hex << pc
      << " code=" << code
      << " size=" << GetCodeSize();
}

inline uint32_t ArtMethod::GetQuickOatCodeOffset() {
  DCHECK(!Runtime::Current()->IsStarted());
  return PointerToLowMemUInt32(GetEntryPointFromQuickCompiledCode());
}

inline uint32_t ArtMethod::GetPortableOatCodeOffset() {
  DCHECK(!Runtime::Current()->IsStarted());
  return PointerToLowMemUInt32(GetEntryPointFromPortableCompiledCode());
}

inline void ArtMethod::SetQuickOatCodeOffset(uint32_t code_offset) {
  DCHECK(!Runtime::Current()->IsStarted());
  SetEntryPointFromQuickCompiledCode(reinterpret_cast<void*>(code_offset));
}

inline void ArtMethod::SetPortableOatCodeOffset(uint32_t code_offset) {
  DCHECK(!Runtime::Current()->IsStarted());
  SetEntryPointFromPortableCompiledCode(reinterpret_cast<void*>(code_offset));
}

inline void ArtMethod::SetOatNativeGcMapOffset(uint32_t gc_map_offset) {
  DCHECK(!Runtime::Current()->IsStarted());
  SetNativeGcMap(reinterpret_cast<uint8_t*>(gc_map_offset));
}

inline uint32_t ArtMethod::GetOatNativeGcMapOffset() {
  DCHECK(!Runtime::Current()->IsStarted());
  return PointerToLowMemUInt32(GetNativeGcMap());
}

inline bool ArtMethod::IsRuntimeMethod() {
  return GetDexMethodIndex() == DexFile::kDexNoIndex;
}

inline bool ArtMethod::IsCalleeSaveMethod() {
  if (!IsRuntimeMethod()) {
    return false;
  }
  Runtime* runtime = Runtime::Current();
  bool result = false;
  for (int i = 0; i < Runtime::kLastCalleeSaveType; i++) {
    if (this == runtime->GetCalleeSaveMethod(Runtime::CalleeSaveType(i))) {
      result = true;
      break;
    }
  }
  return result;
}

inline bool ArtMethod::IsResolutionMethod() {
  bool result = this == Runtime::Current()->GetResolutionMethod();
  // Check that if we do think it is phony it looks like the resolution method.
  DCHECK(!result || IsRuntimeMethod());
  return result;
}

inline bool ArtMethod::IsImtConflictMethod() {
  bool result = this == Runtime::Current()->GetImtConflictMethod();
  // Check that if we do think it is phony it looks like the imt conflict method.
  DCHECK(!result || IsRuntimeMethod());
  return result;
}

template<VerifyObjectFlags kVerifyFlags>
inline void ArtMethod::SetNativeMethod(const void* native_method) {
  SetFieldPtr<false, true, kVerifyFlags>(
      OFFSET_OF_OBJECT_MEMBER(ArtMethod, entry_point_from_jni_), native_method, false);
}

}  // namespace mirror
}  // namespace art

#endif  // ART_RUNTIME_MIRROR_ART_METHOD_INL_H_
