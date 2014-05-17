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
#include "quick/quick_method_frame_info.h"
#include "runtime-inl.h"

namespace art {
namespace mirror {

inline Class* ArtMethod::GetDeclaringClass() {
  Class* result = GetFieldObject<Class>(OFFSET_OF_OBJECT_MEMBER(ArtMethod, declaring_class_));
  DCHECK(result != NULL) << this;
  DCHECK(result->IsIdxLoaded() || result->IsErroneous()) << this;
  return result;
}

inline void ArtMethod::SetDeclaringClass(Class *new_declaring_class) {
  SetFieldObject<false>(OFFSET_OF_OBJECT_MEMBER(ArtMethod, declaring_class_),
                        new_declaring_class);
}

inline uint32_t ArtMethod::GetAccessFlags() {
  DCHECK(GetDeclaringClass()->IsIdxLoaded() || GetDeclaringClass()->IsErroneous());
  return GetField32(OFFSET_OF_OBJECT_MEMBER(ArtMethod, access_flags_));
}

inline uint16_t ArtMethod::GetMethodIndex() {
  DCHECK(GetDeclaringClass()->IsResolved() || GetDeclaringClass()->IsErroneous());
  return GetField32(OFFSET_OF_OBJECT_MEMBER(ArtMethod, method_index_));
}

inline uint32_t ArtMethod::GetDexMethodIndex() {
#ifdef ART_SEA_IR_MODE
  // TODO: Re-add this check for (PORTABLE + SMALL + ) SEA IR when PORTABLE IS fixed!
  // DCHECK(GetDeclaringClass()->IsLoaded() || GetDeclaringClass()->IsErroneous());
#else
  DCHECK(GetDeclaringClass()->IsLoaded() || GetDeclaringClass()->IsErroneous());
#endif
  return GetField32(OFFSET_OF_OBJECT_MEMBER(ArtMethod, dex_method_index_));
}

inline ObjectArray<String>* ArtMethod::GetDexCacheStrings() {
  return GetFieldObject<ObjectArray<String> >(
      OFFSET_OF_OBJECT_MEMBER(ArtMethod, dex_cache_strings_));
}

inline ObjectArray<ArtMethod>* ArtMethod::GetDexCacheResolvedMethods() {
  return GetFieldObject<ObjectArray<ArtMethod> >(
      OFFSET_OF_OBJECT_MEMBER(ArtMethod, dex_cache_resolved_methods_));
}

inline ObjectArray<Class>* ArtMethod::GetDexCacheResolvedTypes() {
  return GetFieldObject<ObjectArray<Class> >(
      OFFSET_OF_OBJECT_MEMBER(ArtMethod, dex_cache_resolved_types_));
}

inline uint32_t ArtMethod::GetCodeSize() {
  DCHECK(!IsRuntimeMethod() && !IsProxyMethod()) << PrettyMethod(this);
  const void* code = EntryPointToCodePointer(GetEntryPointFromQuickCompiledCode());
  if (code == nullptr) {
    return 0u;
  }
  return reinterpret_cast<const OatQuickMethodHeader*>(code)[-1].code_size_;
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
      OFFSET_OF_OBJECT_MEMBER(ArtMethod, entry_point_from_jni_), native_method);
}

inline QuickMethodFrameInfo ArtMethod::GetQuickFrameInfo() {
  if (UNLIKELY(IsPortableCompiled())) {
    // Portable compiled dex bytecode or jni stub.
    return QuickMethodFrameInfo(kStackAlignment, 0u, 0u);
  }
  Runtime* runtime = Runtime::Current();
  if (UNLIKELY(IsAbstract()) || UNLIKELY(IsProxyMethod())) {
    return runtime->GetCalleeSaveMethodFrameInfo(Runtime::kRefsAndArgs);
  }
  if (UNLIKELY(IsRuntimeMethod())) {
    return runtime->GetRuntimeMethodFrameInfo(this);
  }

  const void* entry_point = runtime->GetInstrumentation()->GetQuickCodeFor(this);
  // On failure, instead of nullptr we get the quick-generic-jni-trampoline for native method
  // indicating the generic JNI, or the quick-to-interpreter-bridge (but not the trampoline)
  // for non-native methods. And we really shouldn't see a failure for non-native methods here.
  DCHECK(entry_point != GetQuickToInterpreterBridgeTrampoline(runtime->GetClassLinker()));
  CHECK(entry_point != GetQuickToInterpreterBridge());

  if (UNLIKELY(entry_point == runtime->GetClassLinker()->GetQuickGenericJniTrampoline())) {
    // Generic JNI frame.
    DCHECK(IsNative());
    uint32_t handle_refs = MethodHelper(this).GetNumberOfReferenceArgsWithoutReceiver() + 1;
    size_t scope_size = HandleScope::GetAlignedHandleScopeSize(handle_refs);
    QuickMethodFrameInfo callee_info = runtime->GetCalleeSaveMethodFrameInfo(Runtime::kRefsAndArgs);
    return QuickMethodFrameInfo(callee_info.FrameSizeInBytes() + scope_size,
                                callee_info.CoreSpillMask(), callee_info.FpSpillMask());
  }

  const void* code_pointer = EntryPointToCodePointer(entry_point);
  return reinterpret_cast<const OatQuickMethodHeader*>(code_pointer)[-1].frame_info_;
}

}  // namespace mirror
}  // namespace art

#endif  // ART_RUNTIME_MIRROR_ART_METHOD_INL_H_
