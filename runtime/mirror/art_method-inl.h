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

#include "art_field.h"
#include "class.h"
#include "class_linker-inl.h"
#include "dex_cache.h"
#include "dex_file.h"
#include "dex_file-inl.h"
#include "object-inl.h"
#include "object_array.h"
#include "oat.h"
#include "quick/quick_method_frame_info.h"
#include "read_barrier-inl.h"
#include "runtime-inl.h"

namespace art {
namespace mirror {

inline uint32_t ArtMethod::ClassSize() {
  uint32_t vtable_entries = Object::kVTableLength + 7;
  return Class::ComputeClassSize(true, vtable_entries, 0, 0, 0, 0, 0);
}

template<ReadBarrierOption kReadBarrierOption>
inline Class* ArtMethod::GetJavaLangReflectArtMethod() {
  DCHECK(!java_lang_reflect_ArtMethod_.IsNull());
  return java_lang_reflect_ArtMethod_.Read<kReadBarrierOption>();
}

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

inline uint16_t ArtMethod::GetMethodIndexDuringLinking() {
  return GetField32(OFFSET_OF_OBJECT_MEMBER(ArtMethod, method_index_));
}

inline uint32_t ArtMethod::GetDexMethodIndex() {
  DCHECK(GetDeclaringClass()->IsLoaded() || GetDeclaringClass()->IsErroneous());
  return GetField32(OFFSET_OF_OBJECT_MEMBER(ArtMethod, dex_method_index_));
}

inline ObjectArray<ArtMethod>* ArtMethod::GetDexCacheResolvedMethods() {
  return GetFieldObject<ObjectArray<ArtMethod>>(
      OFFSET_OF_OBJECT_MEMBER(ArtMethod, dex_cache_resolved_methods_));
}

inline ArtMethod* ArtMethod::GetDexCacheResolvedMethod(uint16_t method_index) {
  ArtMethod* method = GetDexCacheResolvedMethods()->Get(method_index);
  if (method != nullptr && !method->GetDeclaringClass()->IsErroneous()) {
    return method;
  } else {
    return nullptr;
  }
}

inline void ArtMethod::SetDexCacheResolvedMethod(uint16_t method_idx, ArtMethod* new_method) {
  GetDexCacheResolvedMethods()->Set<false>(method_idx, new_method);
}

inline bool ArtMethod::HasDexCacheResolvedMethods() {
  return GetDexCacheResolvedMethods() != nullptr;
}

inline bool ArtMethod::HasSameDexCacheResolvedMethods(ObjectArray<ArtMethod>* other_cache) {
  return GetDexCacheResolvedMethods() == other_cache;
}

inline bool ArtMethod::HasSameDexCacheResolvedMethods(ArtMethod* other) {
  return GetDexCacheResolvedMethods() == other->GetDexCacheResolvedMethods();
}


inline ObjectArray<Class>* ArtMethod::GetDexCacheResolvedTypes() {
  return GetFieldObject<ObjectArray<Class>>(
      OFFSET_OF_OBJECT_MEMBER(ArtMethod, dex_cache_resolved_types_));
}

template <bool kWithCheck>
inline Class* ArtMethod::GetDexCacheResolvedType(uint32_t type_index) {
  Class* klass;
  if (kWithCheck) {
    klass = GetDexCacheResolvedTypes()->Get(type_index);
  } else {
    klass = GetDexCacheResolvedTypes()->GetWithoutChecks(type_index);
  }
  return (klass != nullptr && !klass->IsErroneous()) ? klass : nullptr;
}

inline bool ArtMethod::HasDexCacheResolvedTypes() {
  return GetDexCacheResolvedTypes() != nullptr;
}

inline bool ArtMethod::HasSameDexCacheResolvedTypes(ObjectArray<Class>* other_cache) {
  return GetDexCacheResolvedTypes() == other_cache;
}

inline bool ArtMethod::HasSameDexCacheResolvedTypes(ArtMethod* other) {
  return GetDexCacheResolvedTypes() == other->GetDexCacheResolvedTypes();
}

inline mirror::Class* ArtMethod::GetClassFromTypeIndex(uint16_t type_idx, bool resolve) {
  mirror::Class* type = GetDexCacheResolvedType(type_idx);
  if (type == nullptr && resolve) {
    type = Runtime::Current()->GetClassLinker()->ResolveType(type_idx, this);
    CHECK(type != nullptr || Thread::Current()->IsExceptionPending());
  }
  return type;
}

inline uint32_t ArtMethod::GetCodeSize() {
  DCHECK(!IsRuntimeMethod() && !IsProxyMethod()) << PrettyMethod(this);
  return GetCodeSize(EntryPointToCodePointer(GetEntryPointFromQuickCompiledCode()));
}

inline uint32_t ArtMethod::GetCodeSize(const void* code) {
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
      // Constructors and static methods are called with invoke-direct.
      // Interface methods cannot be invoked with invoke-super.
      return IsConstructor() || IsStatic() || GetDeclaringClass()->IsInterface();
    case kInterface: {
      Class* methods_class = GetDeclaringClass();
      return IsDirect() || !(methods_class->IsInterface() || methods_class->IsObjectClass());
    }
    default:
      LOG(FATAL) << "Unreachable - invocation type: " << type;
      UNREACHABLE();
  }
}

inline uint32_t ArtMethod::GetQuickOatCodeOffset() {
  DCHECK(!Runtime::Current()->IsStarted());
  return PointerToLowMemUInt32(GetEntryPointFromQuickCompiledCode());
}

inline void ArtMethod::SetQuickOatCodeOffset(uint32_t code_offset) {
  DCHECK(!Runtime::Current()->IsStarted());
  SetEntryPointFromQuickCompiledCode(reinterpret_cast<void*>(code_offset));
}

inline const uint8_t* ArtMethod::GetMappingTable(size_t pointer_size) {
  const void* code_pointer = GetQuickOatCodePointer(pointer_size);
  if (code_pointer == nullptr) {
    return nullptr;
  }
  return GetMappingTable(code_pointer, pointer_size);
}

inline const uint8_t* ArtMethod::GetMappingTable(const void* code_pointer, size_t pointer_size) {
  DCHECK(code_pointer != nullptr);
  DCHECK_EQ(code_pointer, GetQuickOatCodePointer(pointer_size));
  uint32_t offset =
      reinterpret_cast<const OatQuickMethodHeader*>(code_pointer)[-1].mapping_table_offset_;
  if (UNLIKELY(offset == 0u)) {
    return nullptr;
  }
  return reinterpret_cast<const uint8_t*>(code_pointer) - offset;
}

inline const uint8_t* ArtMethod::GetVmapTable(size_t pointer_size) {
  const void* code_pointer = GetQuickOatCodePointer(pointer_size);
  if (code_pointer == nullptr) {
    return nullptr;
  }
  return GetVmapTable(code_pointer, pointer_size);
}

inline const uint8_t* ArtMethod::GetVmapTable(const void* code_pointer, size_t pointer_size) {
  CHECK(!IsOptimized(pointer_size)) << "Unimplemented vmap table for optimized compiler";
  DCHECK(code_pointer != nullptr);
  DCHECK_EQ(code_pointer, GetQuickOatCodePointer(pointer_size));
  uint32_t offset =
      reinterpret_cast<const OatQuickMethodHeader*>(code_pointer)[-1].vmap_table_offset_;
  if (UNLIKELY(offset == 0u)) {
    return nullptr;
  }
  return reinterpret_cast<const uint8_t*>(code_pointer) - offset;
}

inline CodeInfo ArtMethod::GetOptimizedCodeInfo() {
  DCHECK(IsOptimized(sizeof(void*)));
  const void* code_pointer = GetQuickOatCodePointer(sizeof(void*));
  DCHECK(code_pointer != nullptr);
  uint32_t offset =
      reinterpret_cast<const OatQuickMethodHeader*>(code_pointer)[-1].vmap_table_offset_;
  const void* data = reinterpret_cast<const void*>(reinterpret_cast<const uint8_t*>(code_pointer) - offset);
  return CodeInfo(data);
}

inline const uint8_t* ArtMethod::GetNativeGcMap(size_t pointer_size) {
  const void* code_pointer = GetQuickOatCodePointer(pointer_size);
  if (code_pointer == nullptr) {
    return nullptr;
  }
  return GetNativeGcMap(code_pointer, pointer_size);
}

inline const uint8_t* ArtMethod::GetNativeGcMap(const void* code_pointer, size_t pointer_size) {
  DCHECK(code_pointer != nullptr);
  DCHECK_EQ(code_pointer, GetQuickOatCodePointer(pointer_size));
  uint32_t offset =
      reinterpret_cast<const OatQuickMethodHeader*>(code_pointer)[-1].gc_map_offset_;
  if (UNLIKELY(offset == 0u)) {
    return nullptr;
  }
  return reinterpret_cast<const uint8_t*>(code_pointer) - offset;
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

inline bool ArtMethod::IsImtUnimplementedMethod() {
  bool result = this == Runtime::Current()->GetImtUnimplementedMethod();
  // Check that if we do think it is phony it looks like the imt unimplemented method.
  DCHECK(!result || IsRuntimeMethod());
  return result;
}

inline uintptr_t ArtMethod::NativeQuickPcOffset(const uintptr_t pc) {
  const void* code = Runtime::Current()->GetInstrumentation()->GetQuickCodeFor(
      this, sizeof(void*));
  return pc - reinterpret_cast<uintptr_t>(code);
}

inline QuickMethodFrameInfo ArtMethod::GetQuickFrameInfo(const void* code_pointer) {
  DCHECK(code_pointer != nullptr);
  DCHECK_EQ(code_pointer, GetQuickOatCodePointer(sizeof(void*)));
  return reinterpret_cast<const OatQuickMethodHeader*>(code_pointer)[-1].frame_info_;
}

inline const DexFile* ArtMethod::GetDexFile() {
  return GetDexCache()->GetDexFile();
}

inline const char* ArtMethod::GetDeclaringClassDescriptor() {
  mirror::ArtMethod* method = GetInterfaceMethodIfProxy();
  uint32_t dex_method_idx = method->GetDexMethodIndex();
  if (UNLIKELY(dex_method_idx == DexFile::kDexNoIndex)) {
    return "<runtime method>";
  }
  const DexFile* dex_file = method->GetDexFile();
  return dex_file->GetMethodDeclaringClassDescriptor(dex_file->GetMethodId(dex_method_idx));
}

inline const char* ArtMethod::GetShorty(uint32_t* out_length) {
  mirror::ArtMethod* method = GetInterfaceMethodIfProxy();
  const DexFile* dex_file = method->GetDexFile();
  return dex_file->GetMethodShorty(dex_file->GetMethodId(method->GetDexMethodIndex()), out_length);
}

inline const Signature ArtMethod::GetSignature() {
  mirror::ArtMethod* method = GetInterfaceMethodIfProxy();
  uint32_t dex_method_idx = method->GetDexMethodIndex();
  if (dex_method_idx != DexFile::kDexNoIndex) {
    const DexFile* dex_file = method->GetDexFile();
    return dex_file->GetMethodSignature(dex_file->GetMethodId(dex_method_idx));
  }
  return Signature::NoSignature();
}

inline const char* ArtMethod::GetName() {
  mirror::ArtMethod* method = GetInterfaceMethodIfProxy();
  uint32_t dex_method_idx = method->GetDexMethodIndex();
  if (LIKELY(dex_method_idx != DexFile::kDexNoIndex)) {
    const DexFile* dex_file = method->GetDexFile();
    return dex_file->GetMethodName(dex_file->GetMethodId(dex_method_idx));
  }
  Runtime* runtime = Runtime::Current();
  if (method == runtime->GetResolutionMethod()) {
    return "<runtime internal resolution method>";
  } else if (method == runtime->GetImtConflictMethod()) {
    return "<runtime internal imt conflict method>";
  } else if (method == runtime->GetCalleeSaveMethod(Runtime::kSaveAll)) {
    return "<runtime internal callee-save all registers method>";
  } else if (method == runtime->GetCalleeSaveMethod(Runtime::kRefsOnly)) {
    return "<runtime internal callee-save reference registers method>";
  } else if (method == runtime->GetCalleeSaveMethod(Runtime::kRefsAndArgs)) {
    return "<runtime internal callee-save reference and argument registers method>";
  } else {
    return "<unknown runtime internal method>";
  }
}

inline const DexFile::CodeItem* ArtMethod::GetCodeItem() {
  return GetDeclaringClass()->GetDexFile().GetCodeItem(GetCodeItemOffset());
}

inline bool ArtMethod::IsResolvedTypeIdx(uint16_t type_idx) {
  mirror::ArtMethod* method = GetInterfaceMethodIfProxy();
  return method->GetDexCacheResolvedType(type_idx) != nullptr;
}

inline int32_t ArtMethod::GetLineNumFromDexPC(uint32_t dex_pc) {
  mirror::ArtMethod* method = GetInterfaceMethodIfProxy();
  if (dex_pc == DexFile::kDexNoIndex) {
    return method->IsNative() ? -2 : -1;
  }
  return method->GetDexFile()->GetLineNumFromPC(method, dex_pc);
}

inline const DexFile::ProtoId& ArtMethod::GetPrototype() {
  mirror::ArtMethod* method = GetInterfaceMethodIfProxy();
  const DexFile* dex_file = method->GetDexFile();
  return dex_file->GetMethodPrototype(dex_file->GetMethodId(method->GetDexMethodIndex()));
}

inline const DexFile::TypeList* ArtMethod::GetParameterTypeList() {
  mirror::ArtMethod* method = GetInterfaceMethodIfProxy();
  const DexFile* dex_file = method->GetDexFile();
  const DexFile::ProtoId& proto = dex_file->GetMethodPrototype(
      dex_file->GetMethodId(method->GetDexMethodIndex()));
  return dex_file->GetProtoParameters(proto);
}

inline const char* ArtMethod::GetDeclaringClassSourceFile() {
  return GetInterfaceMethodIfProxy()->GetDeclaringClass()->GetSourceFile();
}

inline uint16_t ArtMethod::GetClassDefIndex() {
  return GetInterfaceMethodIfProxy()->GetDeclaringClass()->GetDexClassDefIndex();
}

inline const DexFile::ClassDef& ArtMethod::GetClassDef() {
  mirror::ArtMethod* method = GetInterfaceMethodIfProxy();
  return method->GetDexFile()->GetClassDef(GetClassDefIndex());
}

inline const char* ArtMethod::GetReturnTypeDescriptor() {
  mirror::ArtMethod* method = GetInterfaceMethodIfProxy();
  const DexFile* dex_file = method->GetDexFile();
  const DexFile::MethodId& method_id = dex_file->GetMethodId(method->GetDexMethodIndex());
  const DexFile::ProtoId& proto_id = dex_file->GetMethodPrototype(method_id);
  uint16_t return_type_idx = proto_id.return_type_idx_;
  return dex_file->GetTypeDescriptor(dex_file->GetTypeId(return_type_idx));
}

inline const char* ArtMethod::GetTypeDescriptorFromTypeIdx(uint16_t type_idx) {
  mirror::ArtMethod* method = GetInterfaceMethodIfProxy();
  const DexFile* dex_file = method->GetDexFile();
  return dex_file->GetTypeDescriptor(dex_file->GetTypeId(type_idx));
}

inline mirror::ClassLoader* ArtMethod::GetClassLoader() {
  return GetInterfaceMethodIfProxy()->GetDeclaringClass()->GetClassLoader();
}

inline mirror::DexCache* ArtMethod::GetDexCache() {
  return GetInterfaceMethodIfProxy()->GetDeclaringClass()->GetDexCache();
}

inline bool ArtMethod::IsProxyMethod() {
  return GetDeclaringClass()->IsProxyClass();
}

inline ArtMethod* ArtMethod::GetInterfaceMethodIfProxy() {
  if (LIKELY(!IsProxyMethod())) {
    return this;
  }
  mirror::Class* klass = GetDeclaringClass();
  mirror::ArtMethod* interface_method = GetDexCacheResolvedMethods()->Get(GetDexMethodIndex());
  DCHECK(interface_method != nullptr);
  DCHECK_EQ(interface_method,
            Runtime::Current()->GetClassLinker()->FindMethodForProxy(klass, this));
  return interface_method;
}

inline void ArtMethod::SetDexCacheResolvedMethods(ObjectArray<ArtMethod>* new_dex_cache_methods) {
  SetFieldObject<false>(OFFSET_OF_OBJECT_MEMBER(ArtMethod, dex_cache_resolved_methods_),
                        new_dex_cache_methods);
}

inline void ArtMethod::SetDexCacheResolvedTypes(ObjectArray<Class>* new_dex_cache_classes) {
  SetFieldObject<false>(OFFSET_OF_OBJECT_MEMBER(ArtMethod, dex_cache_resolved_types_),
                        new_dex_cache_classes);
}

inline mirror::Class* ArtMethod::GetReturnType(bool resolve) {
  DCHECK(!IsProxyMethod());
  const DexFile* dex_file = GetDexFile();
  const DexFile::MethodId& method_id = dex_file->GetMethodId(GetDexMethodIndex());
  const DexFile::ProtoId& proto_id = dex_file->GetMethodPrototype(method_id);
  uint16_t return_type_idx = proto_id.return_type_idx_;
  mirror::Class* type = GetDexCacheResolvedType(return_type_idx);
  if (type == nullptr && resolve) {
    type = Runtime::Current()->GetClassLinker()->ResolveType(return_type_idx, this);
    CHECK(type != nullptr || Thread::Current()->IsExceptionPending());
  }
  return type;
}

inline void ArtMethod::CheckObjectSizeEqualsMirrorSize() {
  // Using the default, check the class object size to make sure it matches the size of the
  // object.
  size_t this_size = sizeof(*this);
#ifdef ART_METHOD_HAS_PADDING_FIELD_ON_64_BIT
  this_size += sizeof(void*) - sizeof(uint32_t);
#endif
  DCHECK_EQ(GetClass()->GetObjectSize(), this_size);
}

}  // namespace mirror
}  // namespace art

#endif  // ART_RUNTIME_MIRROR_ART_METHOD_INL_H_
