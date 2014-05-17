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

#ifndef ART_RUNTIME_ENTRYPOINTS_ENTRYPOINT_UTILS_H_
#define ART_RUNTIME_ENTRYPOINTS_ENTRYPOINT_UTILS_H_

#include "base/macros.h"
#include "class_linker-inl.h"
#include "common_throws.h"
#include "dex_file.h"
#include "indirect_reference_table.h"
#include "invoke_type.h"
#include "jni_internal.h"
#include "mirror/art_method.h"
#include "mirror/array.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "mirror/throwable.h"
#include "object_utils.h"
#include "handle_scope-inl.h"
#include "thread.h"

namespace art {

namespace mirror {
  class Class;
  class ArtField;
  class Object;
}  // namespace mirror

// TODO: Fix no thread safety analysis when GCC can handle template specialization.
template <const bool kAccessCheck>
ALWAYS_INLINE static inline mirror::Class* CheckObjectAlloc(uint32_t type_idx,
                                                            mirror::ArtMethod* method,
                                                            Thread* self, bool* slow_path)
    NO_THREAD_SAFETY_ANALYSIS {
  mirror::Class* klass = method->GetDexCacheResolvedTypes()->GetWithoutChecks(type_idx);
  if (UNLIKELY(klass == NULL)) {
    klass = Runtime::Current()->GetClassLinker()->ResolveType(type_idx, method);
    *slow_path = true;
    if (klass == NULL) {
      DCHECK(self->IsExceptionPending());
      return nullptr;  // Failure
    }
  }
  if (kAccessCheck) {
    if (UNLIKELY(!klass->IsInstantiable())) {
      ThrowLocation throw_location = self->GetCurrentLocationForThrow();
      self->ThrowNewException(throw_location, "Ljava/lang/InstantiationError;",
                              PrettyDescriptor(klass).c_str());
      *slow_path = true;
      return nullptr;  // Failure
    }
    mirror::Class* referrer = method->GetDeclaringClass();
    if (UNLIKELY(!referrer->CanAccess(klass))) {
      ThrowIllegalAccessErrorClass(referrer, klass);
      *slow_path = true;
      return nullptr;  // Failure
    }
  }
  if (UNLIKELY(!klass->IsInitialized())) {
    StackHandleScope<1> hs(self);
    Handle<mirror::Class> h_klass(hs.NewHandle(klass));
    // EnsureInitialized (the class initializer) might cause a GC.
    // may cause us to suspend meaning that another thread may try to
    // change the allocator while we are stuck in the entrypoints of
    // an old allocator. Also, the class initialization may fail. To
    // handle these cases we mark the slow path boolean as true so
    // that the caller knows to check the allocator type to see if it
    // has changed and to null-check the return value in case the
    // initialization fails.
    *slow_path = true;
    if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(h_klass, true, true)) {
      DCHECK(self->IsExceptionPending());
      return nullptr;  // Failure
    }
    return h_klass.Get();
  }
  return klass;
}

// TODO: Fix no thread safety analysis when annotalysis is smarter.
ALWAYS_INLINE static inline mirror::Class* CheckClassInitializedForObjectAlloc(mirror::Class* klass,
                                                                               Thread* self, bool* slow_path)
    NO_THREAD_SAFETY_ANALYSIS {
  if (UNLIKELY(!klass->IsInitialized())) {
    StackHandleScope<1> hs(self);
    Handle<mirror::Class> h_class(hs.NewHandle(klass));
    // EnsureInitialized (the class initializer) might cause a GC.
    // may cause us to suspend meaning that another thread may try to
    // change the allocator while we are stuck in the entrypoints of
    // an old allocator. Also, the class initialization may fail. To
    // handle these cases we mark the slow path boolean as true so
    // that the caller knows to check the allocator type to see if it
    // has changed and to null-check the return value in case the
    // initialization fails.
    *slow_path = true;
    if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(h_class, true, true)) {
      DCHECK(self->IsExceptionPending());
      return nullptr;  // Failure
    }
    return h_class.Get();
  }
  return klass;
}

// Given the context of a calling Method, use its DexCache to resolve a type to a Class. If it
// cannot be resolved, throw an error. If it can, use it to create an instance.
// When verification/compiler hasn't been able to verify access, optionally perform an access
// check.
// TODO: Fix NO_THREAD_SAFETY_ANALYSIS when GCC is smarter.
template <bool kAccessCheck, bool kInstrumented>
ALWAYS_INLINE static inline mirror::Object* AllocObjectFromCode(uint32_t type_idx,
                                                                mirror::ArtMethod* method,
                                                                Thread* self,
                                                                gc::AllocatorType allocator_type)
    NO_THREAD_SAFETY_ANALYSIS {
  bool slow_path = false;
  mirror::Class* klass = CheckObjectAlloc<kAccessCheck>(type_idx, method, self, &slow_path);
  if (UNLIKELY(slow_path)) {
    if (klass == nullptr) {
      return nullptr;
    }
    return klass->Alloc<kInstrumented>(self, Runtime::Current()->GetHeap()->GetCurrentAllocator());
  }
  DCHECK(klass != nullptr);
  return klass->Alloc<kInstrumented>(self, allocator_type);
}

// Given the context of a calling Method and a resolved class, create an instance.
// TODO: Fix NO_THREAD_SAFETY_ANALYSIS when GCC is smarter.
template <bool kInstrumented>
ALWAYS_INLINE static inline mirror::Object* AllocObjectFromCodeResolved(mirror::Class* klass,
                                                                        mirror::ArtMethod* method,
                                                                        Thread* self,
                                                                        gc::AllocatorType allocator_type)
    NO_THREAD_SAFETY_ANALYSIS {
  DCHECK(klass != nullptr);
  bool slow_path = false;
  klass = CheckClassInitializedForObjectAlloc(klass, self, &slow_path);
  if (UNLIKELY(slow_path)) {
    if (klass == nullptr) {
      return nullptr;
    }
    gc::Heap* heap = Runtime::Current()->GetHeap();
    // Pass in false since the object can not be finalizable.
    return klass->Alloc<kInstrumented, false>(self, heap->GetCurrentAllocator());
  }
  // Pass in false since the object can not be finalizable.
  return klass->Alloc<kInstrumented, false>(self, allocator_type);
}

// Given the context of a calling Method and an initialized class, create an instance.
// TODO: Fix NO_THREAD_SAFETY_ANALYSIS when GCC is smarter.
template <bool kInstrumented>
ALWAYS_INLINE static inline mirror::Object* AllocObjectFromCodeInitialized(mirror::Class* klass,
                                                                           mirror::ArtMethod* method,
                                                                           Thread* self,
                                                                           gc::AllocatorType allocator_type)
    NO_THREAD_SAFETY_ANALYSIS {
  DCHECK(klass != nullptr);
  // Pass in false since the object can not be finalizable.
  return klass->Alloc<kInstrumented, false>(self, allocator_type);
}


// TODO: Fix no thread safety analysis when GCC can handle template specialization.
template <bool kAccessCheck>
ALWAYS_INLINE static inline mirror::Class* CheckArrayAlloc(uint32_t type_idx,
                                                           mirror::ArtMethod* method,
                                                           int32_t component_count,
                                                           bool* slow_path)
    NO_THREAD_SAFETY_ANALYSIS {
  if (UNLIKELY(component_count < 0)) {
    ThrowNegativeArraySizeException(component_count);
    *slow_path = true;
    return nullptr;  // Failure
  }
  mirror::Class* klass = method->GetDexCacheResolvedTypes()->GetWithoutChecks(type_idx);
  if (UNLIKELY(klass == nullptr)) {  // Not in dex cache so try to resolve
    klass = Runtime::Current()->GetClassLinker()->ResolveType(type_idx, method);
    *slow_path = true;
    if (klass == nullptr) {  // Error
      DCHECK(Thread::Current()->IsExceptionPending());
      return nullptr;  // Failure
    }
    CHECK(klass->IsArrayClass()) << PrettyClass(klass);
  }
  if (kAccessCheck) {
    mirror::Class* referrer = method->GetDeclaringClass();
    if (UNLIKELY(!referrer->CanAccess(klass))) {
      ThrowIllegalAccessErrorClass(referrer, klass);
      *slow_path = true;
      return nullptr;  // Failure
    }
  }
  return klass;
}

// Given the context of a calling Method, use its DexCache to resolve a type to an array Class. If
// it cannot be resolved, throw an error. If it can, use it to create an array.
// When verification/compiler hasn't been able to verify access, optionally perform an access
// check.
// TODO: Fix no thread safety analysis when GCC can handle template specialization.
template <bool kAccessCheck, bool kInstrumented>
ALWAYS_INLINE static inline mirror::Array* AllocArrayFromCode(uint32_t type_idx,
                                                              mirror::ArtMethod* method,
                                                              int32_t component_count,
                                                              Thread* self,
                                                              gc::AllocatorType allocator_type)
    NO_THREAD_SAFETY_ANALYSIS {
  bool slow_path = false;
  mirror::Class* klass = CheckArrayAlloc<kAccessCheck>(type_idx, method, component_count,
                                                       &slow_path);
  if (UNLIKELY(slow_path)) {
    if (klass == nullptr) {
      return nullptr;
    }
    gc::Heap* heap = Runtime::Current()->GetHeap();
    return mirror::Array::Alloc<kInstrumented>(self, klass, component_count,
                                               klass->GetComponentSize(),
                                               heap->GetCurrentAllocator());
  }
  return mirror::Array::Alloc<kInstrumented>(self, klass, component_count,
                                             klass->GetComponentSize(), allocator_type);
}

template <bool kAccessCheck, bool kInstrumented>
ALWAYS_INLINE static inline mirror::Array* AllocArrayFromCodeResolved(mirror::Class* klass,
                                                                      mirror::ArtMethod* method,
                                                                      int32_t component_count,
                                                                      Thread* self,
                                                                      gc::AllocatorType allocator_type)
    NO_THREAD_SAFETY_ANALYSIS {
  DCHECK(klass != nullptr);
  if (UNLIKELY(component_count < 0)) {
    ThrowNegativeArraySizeException(component_count);
    return nullptr;  // Failure
  }
  if (kAccessCheck) {
    mirror::Class* referrer = method->GetDeclaringClass();
    if (UNLIKELY(!referrer->CanAccess(klass))) {
      ThrowIllegalAccessErrorClass(referrer, klass);
      return nullptr;  // Failure
    }
  }
  // No need to retry a slow-path allocation as the above code won't cause a GC or thread
  // suspension.
  return mirror::Array::Alloc<kInstrumented>(self, klass, component_count,
                                             klass->GetComponentSize(), allocator_type);
}

extern mirror::Array* CheckAndAllocArrayFromCode(uint32_t type_idx, mirror::ArtMethod* method,
                                                 int32_t component_count, Thread* self,
                                                 bool access_check,
                                                 gc::AllocatorType allocator_type)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

extern mirror::Array* CheckAndAllocArrayFromCodeInstrumented(uint32_t type_idx,
                                                             mirror::ArtMethod* method,
                                                             int32_t component_count, Thread* self,
                                                             bool access_check,
                                                             gc::AllocatorType allocator_type)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

// Type of find field operation for fast and slow case.
enum FindFieldType {
  InstanceObjectRead,
  InstanceObjectWrite,
  InstancePrimitiveRead,
  InstancePrimitiveWrite,
  StaticObjectRead,
  StaticObjectWrite,
  StaticPrimitiveRead,
  StaticPrimitiveWrite,
};

template<FindFieldType type, bool access_check>
static inline mirror::ArtField* FindFieldFromCode(uint32_t field_idx, mirror::ArtMethod* referrer,
                                                  Thread* self, size_t expected_size) {
  bool is_primitive;
  bool is_set;
  bool is_static;
  switch (type) {
    case InstanceObjectRead:     is_primitive = false; is_set = false; is_static = false; break;
    case InstanceObjectWrite:    is_primitive = false; is_set = true;  is_static = false; break;
    case InstancePrimitiveRead:  is_primitive = true;  is_set = false; is_static = false; break;
    case InstancePrimitiveWrite: is_primitive = true;  is_set = true;  is_static = false; break;
    case StaticObjectRead:       is_primitive = false; is_set = false; is_static = true;  break;
    case StaticObjectWrite:      is_primitive = false; is_set = true;  is_static = true;  break;
    case StaticPrimitiveRead:    is_primitive = true;  is_set = false; is_static = true;  break;
    case StaticPrimitiveWrite:   // Keep GCC happy by having a default handler, fall-through.
    default:                     is_primitive = true;  is_set = true;  is_static = true;  break;
  }
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  mirror::ArtField* resolved_field = class_linker->ResolveField(field_idx, referrer, is_static);
  if (UNLIKELY(resolved_field == nullptr)) {
    DCHECK(self->IsExceptionPending());  // Throw exception and unwind.
    return nullptr;  // Failure.
  }
  mirror::Class* fields_class = resolved_field->GetDeclaringClass();
  if (access_check) {
    if (UNLIKELY(resolved_field->IsStatic() != is_static)) {
      ThrowIncompatibleClassChangeErrorField(resolved_field, is_static, referrer);
      return nullptr;
    }
    mirror::Class* referring_class = referrer->GetDeclaringClass();
    if (UNLIKELY(!referring_class->CheckResolvedFieldAccess(fields_class, resolved_field,
                                                            field_idx))) {
      DCHECK(self->IsExceptionPending());  // Throw exception and unwind.
      return nullptr;  // Failure.
    }
    if (UNLIKELY(is_set && resolved_field->IsFinal() && (fields_class != referring_class))) {
      ThrowIllegalAccessErrorFinalField(referrer, resolved_field);
      return nullptr;  // Failure.
    } else {
      FieldHelper fh(resolved_field);
      if (UNLIKELY(fh.IsPrimitiveType() != is_primitive || fh.FieldSize() != expected_size)) {
        ThrowLocation throw_location = self->GetCurrentLocationForThrow();
        DCHECK(throw_location.GetMethod() == referrer);
        self->ThrowNewExceptionF(throw_location, "Ljava/lang/NoSuchFieldError;",
                                 "Attempted read of %zd-bit %s on field '%s'",
                                 expected_size * (32 / sizeof(int32_t)),
                                 is_primitive ? "primitive" : "non-primitive",
                                 PrettyField(resolved_field, true).c_str());
        return nullptr;  // Failure.
      }
    }
  }
  if (!is_static) {
    // instance fields must be being accessed on an initialized class
    return resolved_field;
  } else {
    // If the class is initialized we're done.
    if (LIKELY(fields_class->IsInitialized())) {
      return resolved_field;
    } else {
      StackHandleScope<1> hs(self);
      Handle<mirror::Class> h_class(hs.NewHandle(fields_class));
      if (LIKELY(class_linker->EnsureInitialized(h_class, true, true))) {
        // Otherwise let's ensure the class is initialized before resolving the field.
        return resolved_field;
      }
      DCHECK(self->IsExceptionPending());  // Throw exception and unwind
      return nullptr;  // Failure.
    }
  }
}

// Explicit template declarations of FindFieldFromCode for all field access types.
#define EXPLICIT_FIND_FIELD_FROM_CODE_TEMPLATE_DECL(_type, _access_check) \
template SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) ALWAYS_INLINE \
mirror::ArtField* FindFieldFromCode<_type, _access_check>(uint32_t field_idx, \
                                                          mirror::ArtMethod* referrer, \
                                                          Thread* self, size_t expected_size) \

#define EXPLICIT_FIND_FIELD_FROM_CODE_TYPED_TEMPLATE_DECL(_type) \
    EXPLICIT_FIND_FIELD_FROM_CODE_TEMPLATE_DECL(_type, false); \
    EXPLICIT_FIND_FIELD_FROM_CODE_TEMPLATE_DECL(_type, true)

EXPLICIT_FIND_FIELD_FROM_CODE_TYPED_TEMPLATE_DECL(InstanceObjectRead);
EXPLICIT_FIND_FIELD_FROM_CODE_TYPED_TEMPLATE_DECL(InstanceObjectWrite);
EXPLICIT_FIND_FIELD_FROM_CODE_TYPED_TEMPLATE_DECL(InstancePrimitiveRead);
EXPLICIT_FIND_FIELD_FROM_CODE_TYPED_TEMPLATE_DECL(InstancePrimitiveWrite);
EXPLICIT_FIND_FIELD_FROM_CODE_TYPED_TEMPLATE_DECL(StaticObjectRead);
EXPLICIT_FIND_FIELD_FROM_CODE_TYPED_TEMPLATE_DECL(StaticObjectWrite);
EXPLICIT_FIND_FIELD_FROM_CODE_TYPED_TEMPLATE_DECL(StaticPrimitiveRead);
EXPLICIT_FIND_FIELD_FROM_CODE_TYPED_TEMPLATE_DECL(StaticPrimitiveWrite);

#undef EXPLICIT_FIND_FIELD_FROM_CODE_TYPED_TEMPLATE_DECL
#undef EXPLICIT_FIND_FIELD_FROM_CODE_TEMPLATE_DECL

template<InvokeType type, bool access_check>
static inline mirror::ArtMethod* FindMethodFromCode(uint32_t method_idx,
                                                    mirror::Object* this_object,
                                                    mirror::ArtMethod* referrer, Thread* self) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  StackHandleScope<1> hs(self);
  Handle<mirror::Object> handle_scope_this(hs.NewHandle(type == kStatic ? nullptr : this_object));
  mirror::ArtMethod* resolved_method = class_linker->ResolveMethod(method_idx, referrer, type);
  if (UNLIKELY(resolved_method == nullptr)) {
    DCHECK(self->IsExceptionPending());  // Throw exception and unwind.
    return nullptr;  // Failure.
  } else if (UNLIKELY(handle_scope_this.Get() == nullptr && type != kStatic)) {
    // Maintain interpreter-like semantics where NullPointerException is thrown
    // after potential NoSuchMethodError from class linker.
    ThrowLocation throw_location = self->GetCurrentLocationForThrow();
    DCHECK(referrer == throw_location.GetMethod());
    ThrowNullPointerExceptionForMethodAccess(throw_location, method_idx, type);
    return nullptr;  // Failure.
  } else if (access_check) {
    // Incompatible class change should have been handled in resolve method.
    if (UNLIKELY(resolved_method->CheckIncompatibleClassChange(type))) {
      ThrowIncompatibleClassChangeError(type, resolved_method->GetInvokeType(), resolved_method,
                                        referrer);
      return nullptr;  // Failure.
    }
    mirror::Class* methods_class = resolved_method->GetDeclaringClass();
    mirror::Class* referring_class = referrer->GetDeclaringClass();
    bool can_access_resolved_method =
        referring_class->CheckResolvedMethodAccess<type>(methods_class, resolved_method,
                                                         method_idx);
    if (UNLIKELY(!can_access_resolved_method)) {
      DCHECK(self->IsExceptionPending());  // Throw exception and unwind.
      return nullptr;  // Failure.
    }
  }
  switch (type) {
    case kStatic:
    case kDirect:
      return resolved_method;
    case kVirtual: {
      mirror::ObjectArray<mirror::ArtMethod>* vtable = handle_scope_this->GetClass()->GetVTable();
      uint16_t vtable_index = resolved_method->GetMethodIndex();
      if (access_check &&
          (vtable == nullptr || vtable_index >= static_cast<uint32_t>(vtable->GetLength()))) {
        // Behavior to agree with that of the verifier.
        MethodHelper mh(resolved_method);
        ThrowNoSuchMethodError(type, resolved_method->GetDeclaringClass(), mh.GetName(),
                               mh.GetSignature());
        return nullptr;  // Failure.
      }
      DCHECK(vtable != nullptr);
      return vtable->GetWithoutChecks(vtable_index);
    }
    case kSuper: {
      mirror::Class* super_class = referrer->GetDeclaringClass()->GetSuperClass();
      uint16_t vtable_index = resolved_method->GetMethodIndex();
      mirror::ObjectArray<mirror::ArtMethod>* vtable;
      if (access_check) {
        // Check existence of super class.
        vtable = (super_class != nullptr) ? super_class->GetVTable() : nullptr;
        if (vtable == nullptr || vtable_index >= static_cast<uint32_t>(vtable->GetLength())) {
          // Behavior to agree with that of the verifier.
          MethodHelper mh(resolved_method);
          ThrowNoSuchMethodError(type, resolved_method->GetDeclaringClass(), mh.GetName(),
                                 mh.GetSignature());
          return nullptr;  // Failure.
        }
      } else {
        // Super class must exist.
        DCHECK(super_class != nullptr);
        vtable = super_class->GetVTable();
      }
      DCHECK(vtable != nullptr);
      return vtable->GetWithoutChecks(vtable_index);
    }
    case kInterface: {
      uint32_t imt_index = resolved_method->GetDexMethodIndex() % ClassLinker::kImtSize;
      mirror::ObjectArray<mirror::ArtMethod>* imt_table = handle_scope_this->GetClass()->GetImTable();
      mirror::ArtMethod* imt_method = imt_table->Get(imt_index);
      if (!imt_method->IsImtConflictMethod()) {
        return imt_method;
      } else {
        mirror::ArtMethod* interface_method =
            handle_scope_this->GetClass()->FindVirtualMethodForInterface(resolved_method);
        if (UNLIKELY(interface_method == nullptr)) {
          ThrowIncompatibleClassChangeErrorClassForInterfaceDispatch(resolved_method,
                                                                     handle_scope_this.Get(), referrer);
          return nullptr;  // Failure.
        } else {
          return interface_method;
        }
      }
    }
    default:
      LOG(FATAL) << "Unknown invoke type " << type;
      return nullptr;  // Failure.
  }
}

// Explicit template declarations of FindMethodFromCode for all invoke types.
#define EXPLICIT_FIND_METHOD_FROM_CODE_TEMPLATE_DECL(_type, _access_check)                 \
  template SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) ALWAYS_INLINE                       \
  mirror::ArtMethod* FindMethodFromCode<_type, _access_check>(uint32_t method_idx,         \
                                                              mirror::Object* this_object, \
                                                              mirror::ArtMethod* referrer, \
                                                              Thread* self)
#define EXPLICIT_FIND_METHOD_FROM_CODE_TYPED_TEMPLATE_DECL(_type) \
    EXPLICIT_FIND_METHOD_FROM_CODE_TEMPLATE_DECL(_type, false);   \
    EXPLICIT_FIND_METHOD_FROM_CODE_TEMPLATE_DECL(_type, true)

EXPLICIT_FIND_METHOD_FROM_CODE_TYPED_TEMPLATE_DECL(kStatic);
EXPLICIT_FIND_METHOD_FROM_CODE_TYPED_TEMPLATE_DECL(kDirect);
EXPLICIT_FIND_METHOD_FROM_CODE_TYPED_TEMPLATE_DECL(kVirtual);
EXPLICIT_FIND_METHOD_FROM_CODE_TYPED_TEMPLATE_DECL(kSuper);
EXPLICIT_FIND_METHOD_FROM_CODE_TYPED_TEMPLATE_DECL(kInterface);

#undef EXPLICIT_FIND_METHOD_FROM_CODE_TYPED_TEMPLATE_DECL
#undef EXPLICIT_FIND_METHOD_FROM_CODE_TEMPLATE_DECL

// Fast path field resolution that can't initialize classes or throw exceptions.
static inline mirror::ArtField* FindFieldFast(uint32_t field_idx,
                                              mirror::ArtMethod* referrer,
                                              FindFieldType type, size_t expected_size)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::ArtField* resolved_field =
      referrer->GetDeclaringClass()->GetDexCache()->GetResolvedField(field_idx);
  if (UNLIKELY(resolved_field == NULL)) {
    return NULL;
  }
  mirror::Class* fields_class = resolved_field->GetDeclaringClass();
  // Check class is initiliazed or initializing.
  if (UNLIKELY(!fields_class->IsInitializing())) {
    return NULL;
  }
  // Check for incompatible class change.
  bool is_primitive;
  bool is_set;
  bool is_static;
  switch (type) {
    case InstanceObjectRead:     is_primitive = false; is_set = false; is_static = false; break;
    case InstanceObjectWrite:    is_primitive = false; is_set = true;  is_static = false; break;
    case InstancePrimitiveRead:  is_primitive = true;  is_set = false; is_static = false; break;
    case InstancePrimitiveWrite: is_primitive = true;  is_set = true;  is_static = false; break;
    case StaticObjectRead:       is_primitive = false; is_set = false; is_static = true;  break;
    case StaticObjectWrite:      is_primitive = false; is_set = true;  is_static = true;  break;
    case StaticPrimitiveRead:    is_primitive = true;  is_set = false; is_static = true;  break;
    case StaticPrimitiveWrite:   is_primitive = true;  is_set = true;  is_static = true;  break;
    default:
      LOG(FATAL) << "UNREACHABLE";  // Assignment below to avoid GCC warnings.
      is_primitive = true;
      is_set = true;
      is_static = true;
      break;
  }
  if (UNLIKELY(resolved_field->IsStatic() != is_static)) {
    // Incompatible class change.
    return NULL;
  }
  mirror::Class* referring_class = referrer->GetDeclaringClass();
  if (UNLIKELY(!referring_class->CanAccess(fields_class) ||
               !referring_class->CanAccessMember(fields_class,
                                                 resolved_field->GetAccessFlags()) ||
               (is_set && resolved_field->IsFinal() && (fields_class != referring_class)))) {
    // Illegal access.
    return NULL;
  }
  FieldHelper fh(resolved_field);
  if (UNLIKELY(fh.IsPrimitiveType() != is_primitive ||
               fh.FieldSize() != expected_size)) {
    return NULL;
  }
  return resolved_field;
}

// Fast path method resolution that can't throw exceptions.
static inline mirror::ArtMethod* FindMethodFast(uint32_t method_idx,
                                                mirror::Object* this_object,
                                                mirror::ArtMethod* referrer,
                                                bool access_check, InvokeType type)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  bool is_direct = type == kStatic || type == kDirect;
  if (UNLIKELY(this_object == NULL && !is_direct)) {
    return NULL;
  }
  mirror::ArtMethod* resolved_method =
      referrer->GetDeclaringClass()->GetDexCache()->GetResolvedMethod(method_idx);
  if (UNLIKELY(resolved_method == NULL)) {
    return NULL;
  }
  if (access_check) {
    // Check for incompatible class change errors and access.
    bool icce = resolved_method->CheckIncompatibleClassChange(type);
    if (UNLIKELY(icce)) {
      return NULL;
    }
    mirror::Class* methods_class = resolved_method->GetDeclaringClass();
    mirror::Class* referring_class = referrer->GetDeclaringClass();
    if (UNLIKELY(!referring_class->CanAccess(methods_class) ||
                 !referring_class->CanAccessMember(methods_class,
                                                   resolved_method->GetAccessFlags()))) {
      // Potential illegal access, may need to refine the method's class.
      return NULL;
    }
  }
  if (type == kInterface) {  // Most common form of slow path dispatch.
    return this_object->GetClass()->FindVirtualMethodForInterface(resolved_method);
  } else if (is_direct) {
    return resolved_method;
  } else if (type == kSuper) {
    return referrer->GetDeclaringClass()->GetSuperClass()->GetVTable()->
        Get(resolved_method->GetMethodIndex());
  } else {
    DCHECK(type == kVirtual);
    return this_object->GetClass()->GetVTable()->Get(resolved_method->GetMethodIndex());
  }
}

static inline mirror::Class* ResolveVerifyAndClinit(uint32_t type_idx,
                                                    mirror::ArtMethod* referrer,
                                                    Thread* self, bool can_run_clinit,
                                                    bool verify_access)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  mirror::Class* klass = class_linker->ResolveType(type_idx, referrer);
  if (UNLIKELY(klass == nullptr)) {
    CHECK(self->IsExceptionPending());
    return nullptr;  // Failure - Indicate to caller to deliver exception
  }
  // Perform access check if necessary.
  mirror::Class* referring_class = referrer->GetDeclaringClass();
  if (verify_access && UNLIKELY(!referring_class->CanAccess(klass))) {
    ThrowIllegalAccessErrorClass(referring_class, klass);
    return nullptr;  // Failure - Indicate to caller to deliver exception
  }
  // If we're just implementing const-class, we shouldn't call <clinit>.
  if (!can_run_clinit) {
    return klass;
  }
  // If we are the <clinit> of this class, just return our storage.
  //
  // Do not set the DexCache InitializedStaticStorage, since that implies <clinit> has finished
  // running.
  if (klass == referring_class && referrer->IsConstructor() && referrer->IsStatic()) {
    return klass;
  }
  StackHandleScope<1> hs(self);
  Handle<mirror::Class> h_class(hs.NewHandle(klass));
  if (!class_linker->EnsureInitialized(h_class, true, true)) {
    CHECK(self->IsExceptionPending());
    return nullptr;  // Failure - Indicate to caller to deliver exception
  }
  return h_class.Get();
}

extern void ThrowStackOverflowError(Thread* self) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

static inline mirror::String* ResolveStringFromCode(mirror::ArtMethod* referrer,
                                                    uint32_t string_idx)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  return class_linker->ResolveString(string_idx, referrer);
}

static inline void UnlockJniSynchronizedMethod(jobject locked, Thread* self)
    NO_THREAD_SAFETY_ANALYSIS /* SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) */ {
  // Save any pending exception over monitor exit call.
  mirror::Throwable* saved_exception = NULL;
  ThrowLocation saved_throw_location;
  if (UNLIKELY(self->IsExceptionPending())) {
    saved_exception = self->GetException(&saved_throw_location);
    self->ClearException();
  }
  // Decode locked object and unlock, before popping local references.
  self->DecodeJObject(locked)->MonitorExit(self);
  if (UNLIKELY(self->IsExceptionPending())) {
    LOG(FATAL) << "Synchronized JNI code returning with an exception:\n"
        << saved_exception->Dump()
        << "\nEncountered second exception during implicit MonitorExit:\n"
        << self->GetException(NULL)->Dump();
  }
  // Restore pending exception.
  if (saved_exception != NULL) {
    self->SetException(saved_throw_location, saved_exception);
  }
}

static inline void CheckReferenceResult(mirror::Object* o, Thread* self)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (o == NULL) {
    return;
  }
  mirror::ArtMethod* m = self->GetCurrentMethod(NULL);
  if (o == kInvalidIndirectRefObject) {
    JniAbortF(NULL, "invalid reference returned from %s", PrettyMethod(m).c_str());
  }
  // Make sure that the result is an instance of the type this method was expected to return.
  mirror::Class* return_type = MethodHelper(m).GetReturnType();

  if (!o->InstanceOf(return_type)) {
    JniAbortF(NULL, "attempt to return an instance of %s from %s",
              PrettyTypeOf(o).c_str(), PrettyMethod(m).c_str());
  }
}

static inline void CheckSuspend(Thread* thread) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  for (;;) {
    if (thread->ReadFlag(kCheckpointRequest)) {
      thread->RunCheckpointFunction();
    } else if (thread->ReadFlag(kSuspendRequest)) {
      thread->FullSuspendCheck();
    } else {
      break;
    }
  }
}

JValue InvokeProxyInvocationHandler(ScopedObjectAccessUnchecked& soa, const char* shorty,
                                    jobject rcvr_jobj, jobject interface_art_method_jobj,
                                    std::vector<jvalue>& args)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

// Entry point for deoptimization.
extern "C" void art_quick_deoptimize();
static inline uintptr_t GetQuickDeoptimizationEntryPoint() {
  return reinterpret_cast<uintptr_t>(art_quick_deoptimize);
}

// Return address of instrumentation stub.
extern "C" void art_quick_instrumentation_entry(void*);
static inline void* GetQuickInstrumentationEntryPoint() {
  return reinterpret_cast<void*>(art_quick_instrumentation_entry);
}

// The return_pc of instrumentation exit stub.
extern "C" void art_quick_instrumentation_exit();
static inline uintptr_t GetQuickInstrumentationExitPc() {
  return reinterpret_cast<uintptr_t>(art_quick_instrumentation_exit);
}

extern "C" void art_portable_to_interpreter_bridge(mirror::ArtMethod*);
static inline const void* GetPortableToInterpreterBridge() {
  return reinterpret_cast<void*>(art_portable_to_interpreter_bridge);
}

static inline const void* GetPortableToQuickBridge() {
  // TODO: portable to quick bridge. Bug: 8196384
  return GetPortableToInterpreterBridge();
}

extern "C" void art_quick_to_interpreter_bridge(mirror::ArtMethod*);
static inline const void* GetQuickToInterpreterBridge() {
  return reinterpret_cast<void*>(art_quick_to_interpreter_bridge);
}

static inline const void* GetQuickToPortableBridge() {
  // TODO: quick to portable bridge. Bug: 8196384
  return GetQuickToInterpreterBridge();
}

static inline const void* GetPortableResolutionTrampoline(ClassLinker* class_linker) {
  return class_linker->GetPortableResolutionTrampoline();
}

static inline const void* GetQuickResolutionTrampoline(ClassLinker* class_linker) {
  return class_linker->GetQuickResolutionTrampoline();
}

static inline const void* GetPortableImtConflictTrampoline(ClassLinker* class_linker) {
  return class_linker->GetPortableImtConflictTrampoline();
}

static inline const void* GetQuickImtConflictTrampoline(ClassLinker* class_linker) {
  return class_linker->GetQuickImtConflictTrampoline();
}

static inline const void* GetQuickToInterpreterBridgeTrampoline(ClassLinker* class_linker) {
  return class_linker->GetQuickToInterpreterBridgeTrampoline();
}

extern "C" void art_portable_proxy_invoke_handler();
static inline const void* GetPortableProxyInvokeHandler() {
  return reinterpret_cast<void*>(art_portable_proxy_invoke_handler);
}

extern "C" void art_quick_proxy_invoke_handler();
static inline const void* GetQuickProxyInvokeHandler() {
  return reinterpret_cast<void*>(art_quick_proxy_invoke_handler);
}

extern "C" void* art_jni_dlsym_lookup_stub(JNIEnv*, jobject);
static inline void* GetJniDlsymLookupStub() {
  return reinterpret_cast<void*>(art_jni_dlsym_lookup_stub);
}

template <typename INT_TYPE, typename FLOAT_TYPE>
static inline INT_TYPE art_float_to_integral(FLOAT_TYPE f) {
  const INT_TYPE kMaxInt = static_cast<INT_TYPE>(std::numeric_limits<INT_TYPE>::max());
  const INT_TYPE kMinInt = static_cast<INT_TYPE>(std::numeric_limits<INT_TYPE>::min());
  const FLOAT_TYPE kMaxIntAsFloat = static_cast<FLOAT_TYPE>(kMaxInt);
  const FLOAT_TYPE kMinIntAsFloat = static_cast<FLOAT_TYPE>(kMinInt);
  if (LIKELY(f > kMinIntAsFloat)) {
     if (LIKELY(f < kMaxIntAsFloat)) {
       return static_cast<INT_TYPE>(f);
     } else {
       return kMaxInt;
     }
  } else {
    return (f != f) ? 0 : kMinInt;  // f != f implies NaN
  }
}

}  // namespace art

#endif  // ART_RUNTIME_ENTRYPOINTS_ENTRYPOINT_UTILS_H_
