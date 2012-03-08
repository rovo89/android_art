// Copyright 2012 Google Inc. All Rights Reserved.

#ifndef ART_SRC_RUNTIME_SUPPORT_COMMON_H_
#define ART_SRC_RUNTIME_SUPPORT_COMMON_H_

#include "class_linker.h"
#include "constants.h"
#include "object.h"
#include "object_utils.h"
#include "thread.h"

#include <stdint.h>

namespace art {

class Array;
class Class;
class Field;
class Method;
class Object;

static void ThrowNewIllegalAccessErrorClass(Thread* self,
                                                   Class* referrer,
                                                   Class* accessed) {
  self->ThrowNewExceptionF("Ljava/lang/IllegalAccessError;",
                           "illegal class access: '%s' -> '%s'",
                           PrettyDescriptor(referrer).c_str(),
                           PrettyDescriptor(accessed).c_str());
}

static void
ThrowNewIllegalAccessErrorClassForMethodDispatch(Thread* self,
                                                 Class* referrer,
                                                 Class* accessed,
                                                 const Method* caller,
                                                 const Method* called,
                                                 InvokeType type) {
  std::ostringstream type_stream;
  type_stream << type;
  self->ThrowNewExceptionF("Ljava/lang/IllegalAccessError;",
                           "illegal class access ('%s' -> '%s')"
                           "in attempt to invoke %s method '%s' from '%s'",
                           PrettyDescriptor(referrer).c_str(),
                           PrettyDescriptor(accessed).c_str(),
                           type_stream.str().c_str(),
                           PrettyMethod(called).c_str(),
                           PrettyMethod(caller).c_str());
}

static void
ThrowNewIncompatibleClassChangeErrorClassForInterfaceDispatch(Thread* self,
                                                              const Method* referrer,
                                                              const Method* interface_method,
                                                              Object* this_object) {
  self->ThrowNewExceptionF("Ljava/lang/IncompatibleClassChangeError;",
                           "class '%s' does not implement interface '%s' in call to '%s' from '%s'",
                           PrettyDescriptor(this_object->GetClass()).c_str(),
                           PrettyDescriptor(interface_method->GetDeclaringClass()).c_str(),
                           PrettyMethod(interface_method).c_str(), PrettyMethod(referrer).c_str());
}

static void ThrowNewIllegalAccessErrorField(Thread* self,
                                                   Class* referrer,
                                                   Field* accessed) {
  self->ThrowNewExceptionF("Ljava/lang/IllegalAccessError;",
                           "Field '%s' is inaccessible to class '%s'",
                           PrettyField(accessed, false).c_str(),
                           PrettyDescriptor(referrer).c_str());
}

static void ThrowNewIllegalAccessErrorFinalField(Thread* self,
                                                        const Method* referrer,
                                                        Field* accessed) {
  self->ThrowNewExceptionF("Ljava/lang/IllegalAccessError;",
                           "Final field '%s' cannot be written to by method '%s'",
                           PrettyField(accessed, false).c_str(),
                           PrettyMethod(referrer).c_str());
}

static void ThrowNewIllegalAccessErrorMethod(Thread* self,
                                                    Class* referrer,
                                                    Method* accessed) {
  self->ThrowNewExceptionF("Ljava/lang/IllegalAccessError;",
                           "Method '%s' is inaccessible to class '%s'",
                           PrettyMethod(accessed).c_str(),
                           PrettyDescriptor(referrer).c_str());
}

static void ThrowNullPointerExceptionForFieldAccess(Thread* self,
                                                           Field* field,
                                                           bool is_read) {
  self->ThrowNewExceptionF("Ljava/lang/NullPointerException;",
                           "Attempt to %s field '%s' on a null object reference",
                           is_read ? "read from" : "write to",
                           PrettyField(field, true).c_str());
}

static void ThrowNullPointerExceptionForMethodAccess(Thread* self,
                                                            Method* caller,
                                                            uint32_t method_idx,
                                                            InvokeType type) {
  const DexFile& dex_file =
      Runtime::Current()->GetClassLinker()->FindDexFile(caller->GetDeclaringClass()->GetDexCache());
  std::ostringstream type_stream;
  type_stream << type;
  self->ThrowNewExceptionF("Ljava/lang/NullPointerException;",
                           "Attempt to invoke %s method '%s' from '%s' on a null object reference",
                           type_stream.str().c_str(),
                           PrettyMethod(method_idx, dex_file, true).c_str(),
                           PrettyMethod(caller).c_str());
}

// Given the context of a calling Method, use its DexCache to resolve a type to a Class. If it
// cannot be resolved, throw an error. If it can, use it to create an instance.
// When verification/compiler hasn't been able to verify access, optionally perform an access
// check.
static Object* AllocObjectFromCode(uint32_t type_idx, Method* method, Thread* self,
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
  if (!runtime->GetClassLinker()->EnsureInitialized(klass, true)) {
    DCHECK(self->IsExceptionPending());
    return NULL;  // Failure
  }
  return klass->AllocObject();
}

// Given the context of a calling Method, use its DexCache to resolve a type to an array Class. If
// it cannot be resolved, throw an error. If it can, use it to create an array.
// When verification/compiler hasn't been able to verify access, optionally perform an access
// check.
static Array* AllocArrayFromCode(uint32_t type_idx, Method* method, int32_t component_count,
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
static Field* FindFieldFast(uint32_t field_idx, const Method* referrer, bool is_primitive,
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
static Method* FindMethodFast(uint32_t method_idx, Object* this_object, const Method* referrer,
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

}  // namespace art

#endif  // ART_SRC_RUNTIME_SUPPORT_COMMON_H_
