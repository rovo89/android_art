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

#ifndef ART_SRC_RUNTIME_SUPPORT_H_
#define ART_SRC_RUNTIME_SUPPORT_H_

#include "class_linker.h"
#include "dex_file.h"
#include "dex_verifier.h"
#include "invoke_type.h"
#include "object.h"
#include "object_utils.h"
#include "thread.h"

extern "C" void art_proxy_invoke_handler();
extern "C" void art_work_around_app_jni_bugs();

namespace art {

class Array;
class Class;
class Field;
class Method;
class Object;

// Helpers to give consistent descriptive exception messages
void ThrowNewIllegalAccessErrorClass(Thread* self, Class* referrer, Class* accessed);
void ThrowNewIllegalAccessErrorClassForMethodDispatch(Thread* self, Class* referrer,
                                                      Class* accessed,
                                                      const Method* caller,
                                                      const Method* called,
                                                      InvokeType type);
void ThrowNewIncompatibleClassChangeErrorClassForInterfaceDispatch(Thread* self,
                                                                   const Method* referrer,
                                                                   const Method* interface_method,
                                                                   Object* this_object);
void ThrowNewIllegalAccessErrorField(Thread* self, Class* referrer, Field* accessed);
void ThrowNewIllegalAccessErrorFinalField(Thread* self, const Method* referrer, Field* accessed);

void ThrowNewIllegalAccessErrorMethod(Thread* self, Class* referrer, Method* accessed);
void ThrowNullPointerExceptionForFieldAccess(Thread* self, Field* field, bool is_read);
void ThrowNullPointerExceptionForMethodAccess(Thread* self, Method* caller, uint32_t method_idx,
                                              InvokeType type);
void ThrowNullPointerExceptionFromDexPC(Thread* self, Method* caller, uint32_t dex_pc);

std::string FieldNameFromIndex(const Method* method, uint32_t ref,
                               verifier::VerifyErrorRefType ref_type, bool access);
std::string MethodNameFromIndex(const Method* method, uint32_t ref,
                                verifier::VerifyErrorRefType ref_type, bool access);

// Given the context of a calling Method, use its DexCache to resolve a type to a Class. If it
// cannot be resolved, throw an error. If it can, use it to create an instance.
// When verification/compiler hasn't been able to verify access, optionally perform an access
// check.
static inline Object* AllocObjectFromCode(uint32_t type_idx, Method* method, Thread* self,
                                          bool access_check) {
  Class* klass = method->GetDexCacheResolvedTypes()->Get(type_idx);
  Runtime* runtime = Runtime::Current();
  if (UNLIKELY(klass == NULL)) {
    klass = runtime->GetClassLinker()->ResolveType(type_idx, method);
    if (klass == NULL) {
      DCHECK(self->IsExceptionPending());
      return NULL;  // Failure
    }
  }
  if (access_check) {
    if (UNLIKELY(!klass->IsInstantiable())) {
      self->ThrowNewException("Ljava/lang/InstantiationError;",
                              PrettyDescriptor(klass).c_str());
      return NULL;  // Failure
    }
    Class* referrer = method->GetDeclaringClass();
    if (UNLIKELY(!referrer->CanAccess(klass))) {
      ThrowNewIllegalAccessErrorClass(self, referrer, klass);
      return NULL;  // Failure
    }
  }
  if (!runtime->GetClassLinker()->EnsureInitialized(klass, true, true)) {
    DCHECK(self->IsExceptionPending());
    return NULL;  // Failure
  }
  return klass->AllocObject();
}

// Given the context of a calling Method, use its DexCache to resolve a type to an array Class. If
// it cannot be resolved, throw an error. If it can, use it to create an array.
// When verification/compiler hasn't been able to verify access, optionally perform an access
// check.
static inline Array* AllocArrayFromCode(uint32_t type_idx, Method* method, int32_t component_count,
                                        Thread* self, bool access_check) {
  if (UNLIKELY(component_count < 0)) {
    Thread::Current()->ThrowNewExceptionF("Ljava/lang/NegativeArraySizeException;", "%d",
                                          component_count);
    return NULL;  // Failure
  }
  Class* klass = method->GetDexCacheResolvedTypes()->Get(type_idx);
  if (UNLIKELY(klass == NULL)) {  // Not in dex cache so try to resolve
    klass = Runtime::Current()->GetClassLinker()->ResolveType(type_idx, method);
    if (klass == NULL) {  // Error
      DCHECK(Thread::Current()->IsExceptionPending());
      return NULL;  // Failure
    }
    CHECK(klass->IsArrayClass()) << PrettyClass(klass);
  }
  if (access_check) {
    Class* referrer = method->GetDeclaringClass();
    if (UNLIKELY(!referrer->CanAccess(klass))) {
      ThrowNewIllegalAccessErrorClass(self, referrer, klass);
      return NULL;  // Failure
    }
  }
  return Array::Alloc(klass, component_count);
}

extern Array* CheckAndAllocArrayFromCode(uint32_t type_idx, Method* method, int32_t component_count,
                                         Thread* self, bool access_check);

extern Field* FindFieldFromCode(uint32_t field_idx, const Method* referrer, Thread* self,
                                bool is_static, bool is_primitive, bool is_set,
                                size_t expected_size);

// Fast path field resolution that can't throw exceptions
static inline Field* FindFieldFast(uint32_t field_idx, const Method* referrer, bool is_primitive,
                                   size_t expected_size, bool is_set) {
  Field* resolved_field = referrer->GetDeclaringClass()->GetDexCache()->GetResolvedField(field_idx);
  if (UNLIKELY(resolved_field == NULL)) {
    return NULL;
  }
  Class* fields_class = resolved_field->GetDeclaringClass();
  // Check class is initiliazed or initializing
  if (UNLIKELY(!fields_class->IsInitializing())) {
    return NULL;
  }
  Class* referring_class = referrer->GetDeclaringClass();
  if (UNLIKELY(!referring_class->CanAccess(fields_class) ||
               !referring_class->CanAccessMember(fields_class,
                                                 resolved_field->GetAccessFlags()) ||
               (is_set && resolved_field->IsFinal() && (fields_class != referring_class)))) {
    // illegal access
    return NULL;
  }
  FieldHelper fh(resolved_field);
  if (UNLIKELY(fh.IsPrimitiveType() != is_primitive ||
               fh.FieldSize() != expected_size)) {
    return NULL;
  }
  return resolved_field;
}

// Fast path method resolution that can't throw exceptions
static inline Method* FindMethodFast(uint32_t method_idx, Object* this_object, const Method* referrer,
                              bool access_check, InvokeType type) {
  bool is_direct = type == kStatic || type == kDirect;
  if (UNLIKELY(this_object == NULL && !is_direct)) {
    return NULL;
  }
  Method* resolved_method =
      referrer->GetDeclaringClass()->GetDexCache()->GetResolvedMethod(method_idx);
  if (UNLIKELY(resolved_method == NULL)) {
    return NULL;
  }
  if (access_check) {
    Class* methods_class = resolved_method->GetDeclaringClass();
    Class* referring_class = referrer->GetDeclaringClass();
    if (UNLIKELY(!referring_class->CanAccess(methods_class) ||
                 !referring_class->CanAccessMember(methods_class,
                                                   resolved_method->GetAccessFlags()))) {
      // potential illegal access
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

extern Method* FindMethodFromCode(uint32_t method_idx, Object* this_object, const Method* referrer,
                                  Thread* self, bool access_check, InvokeType type);

extern Class* ResolveVerifyAndClinit(uint32_t type_idx, const Method* referrer, Thread* self,
                                     bool can_run_clinit, bool verify_access);

static inline String* ResolveStringFromCode(const Method* referrer, uint32_t string_idx) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  return class_linker->ResolveString(string_idx, referrer);
}

}  // namespace art

#endif  // ART_SRC_RUNTIME_SUPPORT_H_
