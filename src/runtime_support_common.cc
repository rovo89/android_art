// Copyright 2012 Google Inc. All Rights Reserved.

#include "runtime_support_common.h"


namespace art {

// Helper function to allocate array for FILLED_NEW_ARRAY.
Array* CheckAndAllocArrayFromCode(uint32_t type_idx, Method* method, int32_t component_count,
                                  Thread* self, bool access_check) {
  if (UNLIKELY(component_count < 0)) {
    self->ThrowNewExceptionF("Ljava/lang/NegativeArraySizeException;", "%d", component_count);
    return NULL;  // Failure
  }
  Class* klass = method->GetDexCacheResolvedTypes()->Get(type_idx);
  if (UNLIKELY(klass == NULL)) {  // Not in dex cache so try to resolve
    klass = Runtime::Current()->GetClassLinker()->ResolveType(type_idx, method);
    if (klass == NULL) {  // Error
      DCHECK(Thread::Current()->IsExceptionPending());
      return NULL;  // Failure
    }
  }
  if (UNLIKELY(klass->IsPrimitive() && !klass->IsPrimitiveInt())) {
    if (klass->IsPrimitiveLong() || klass->IsPrimitiveDouble()) {
      Thread::Current()->ThrowNewExceptionF("Ljava/lang/RuntimeException;",
                                            "Bad filled array request for type %s",
                                            PrettyDescriptor(klass).c_str());
    } else {
      Thread::Current()->ThrowNewExceptionF("Ljava/lang/InternalError;",
                                            "Found type %s; filled-new-array not implemented for anything but \'int\'",
                                            PrettyDescriptor(klass).c_str());
    }
    return NULL;  // Failure
  } else {
    if (access_check) {
      Class* referrer = method->GetDeclaringClass();
      if (UNLIKELY(!referrer->CanAccess(klass))) {
        ThrowNewIllegalAccessErrorClass(self, referrer, klass);
        return NULL;  // Failure
      }
    }
    DCHECK(klass->IsArrayClass()) << PrettyClass(klass);
    return Array::Alloc(klass, component_count);
  }
}

// Slow path field resolution and declaring class initialization
Field* FindFieldFromCode(uint32_t field_idx, const Method* referrer, Thread* self,
                         bool is_static, bool is_primitive, bool is_set, size_t expected_size) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Field* resolved_field = class_linker->ResolveField(field_idx, referrer, is_static);
  if (UNLIKELY(resolved_field == NULL)) {
    DCHECK(self->IsExceptionPending());  // Throw exception and unwind
    return NULL;  // failure
  } else {
    Class* fields_class = resolved_field->GetDeclaringClass();
    Class* referring_class = referrer->GetDeclaringClass();
    if (UNLIKELY(!referring_class->CanAccess(fields_class))) {
      ThrowNewIllegalAccessErrorClass(self, referring_class, fields_class);
      return NULL;  // failure
    } else if (UNLIKELY(!referring_class->CanAccessMember(fields_class,
                                                          resolved_field->GetAccessFlags()))) {
      ThrowNewIllegalAccessErrorField(self, referring_class, resolved_field);
      return NULL;  // failure
    } else if (UNLIKELY(is_set && resolved_field->IsFinal() && (fields_class != referring_class))) {
      ThrowNewIllegalAccessErrorFinalField(self, referrer, resolved_field);
      return NULL;  // failure
    } else {
      FieldHelper fh(resolved_field);
      if (UNLIKELY(fh.IsPrimitiveType() != is_primitive ||
                   fh.FieldSize() != expected_size)) {
        self->ThrowNewExceptionF("Ljava/lang/NoSuchFieldError;",
                                 "Attempted read of %zd-bit %s on field '%s'",
                                 expected_size * (32 / sizeof(int32_t)),
                                 is_primitive ? "primitive" : "non-primitive",
                                 PrettyField(resolved_field, true).c_str());
        return NULL;  // failure
      } else if (!is_static) {
        // instance fields must be being accessed on an initialized class
        return resolved_field;
      } else {
        // If the class is already initializing, we must be inside <clinit>, or
        // we'd still be waiting for the lock.
        if (fields_class->IsInitializing()) {
          return resolved_field;
        } else if (Runtime::Current()->GetClassLinker()->EnsureInitialized(fields_class, true)) {
          return resolved_field;
        } else {
          DCHECK(self->IsExceptionPending());  // Throw exception and unwind
          return NULL;  // failure
        }
      }
    }
  }
}

// Slow path method resolution
Method* FindMethodFromCode(uint32_t method_idx, Object* this_object, const Method* referrer,
                           Thread* self, bool access_check, InvokeType type) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  bool is_direct = type == kStatic || type == kDirect;
  Method* resolved_method = class_linker->ResolveMethod(method_idx, referrer, is_direct);
  if (UNLIKELY(resolved_method == NULL)) {
    DCHECK(self->IsExceptionPending());  // Throw exception and unwind
    return NULL;  // failure
  } else {
    if (!access_check) {
      if (is_direct) {
        return resolved_method;
      } else if (type == kInterface) {
        Method* interface_method =
            this_object->GetClass()->FindVirtualMethodForInterface(resolved_method);
        if (UNLIKELY(interface_method == NULL)) {
          ThrowNewIncompatibleClassChangeErrorClassForInterfaceDispatch(self, referrer,
                                                                        resolved_method,
                                                                        this_object);
          return NULL;  // failure
        } else {
          return interface_method;
        }
      } else {
        ObjectArray<Method>* vtable;
        uint16_t vtable_index = resolved_method->GetMethodIndex();
        if (type == kSuper) {
          vtable = referrer->GetDeclaringClass()->GetSuperClass()->GetVTable();
        } else {
          vtable = this_object->GetClass()->GetVTable();
        }
        // TODO: eliminate bounds check?
        return vtable->Get(vtable_index);
      }
    } else {
      Class* methods_class = resolved_method->GetDeclaringClass();
      Class* referring_class = referrer->GetDeclaringClass();
      if (UNLIKELY(!referring_class->CanAccess(methods_class) ||
                   !referring_class->CanAccessMember(methods_class,
                                                     resolved_method->GetAccessFlags()))) {
        // The referring class can't access the resolved method, this may occur as a result of a
        // protected method being made public by implementing an interface that re-declares the
        // method public. Resort to the dex file to determine the correct class for the access check
        const DexFile& dex_file = class_linker->FindDexFile(referring_class->GetDexCache());
        methods_class = class_linker->ResolveType(dex_file,
                                                  dex_file.GetMethodId(method_idx).class_idx_,
                                                  referring_class);
        if (UNLIKELY(!referring_class->CanAccess(methods_class))) {
          ThrowNewIllegalAccessErrorClassForMethodDispatch(self, referring_class, methods_class,
                                                           referrer, resolved_method, type);
          return NULL;  // failure
        } else if (UNLIKELY(!referring_class->CanAccessMember(methods_class,
                                                              resolved_method->GetAccessFlags()))) {
          ThrowNewIllegalAccessErrorMethod(self, referring_class, resolved_method);
          return NULL;  // failure
        }
      }
      if (is_direct) {
        return resolved_method;
      } else if (type == kInterface) {
        Method* interface_method =
            this_object->GetClass()->FindVirtualMethodForInterface(resolved_method);
        if (UNLIKELY(interface_method == NULL)) {
          ThrowNewIncompatibleClassChangeErrorClassForInterfaceDispatch(self, referrer,
                                                                        resolved_method,
                                                                        this_object);
          return NULL;  // failure
        } else {
          return interface_method;
        }
      } else {
        ObjectArray<Method>* vtable;
        uint16_t vtable_index = resolved_method->GetMethodIndex();
        if (type == kSuper) {
          Class* super_class = referring_class->GetSuperClass();
          if (LIKELY(super_class != NULL)) {
            vtable = referring_class->GetSuperClass()->GetVTable();
          } else {
            vtable = NULL;
          }
        } else {
          vtable = this_object->GetClass()->GetVTable();
        }
        if (LIKELY(vtable != NULL &&
                   vtable_index < static_cast<uint32_t>(vtable->GetLength()))) {
          return vtable->GetWithoutChecks(vtable_index);
        } else {
          // Behavior to agree with that of the verifier
          self->ThrowNewExceptionF("Ljava/lang/NoSuchMethodError;",
                                   "attempt to invoke %s method '%s' from '%s'"
                                   " using incorrect form of method dispatch",
                                   (type == kSuper ? "super class" : "virtual"),
                                   PrettyMethod(resolved_method).c_str(),
                                   PrettyMethod(referrer).c_str());
          return NULL;  // failure
        }
      }
    }
  }
}

}  // namespace art
