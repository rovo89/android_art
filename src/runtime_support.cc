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

#include "runtime_support.h"

#include "reflection.h"
#include "scoped_thread_state_change.h"
#include "ScopedLocalRef.h"
#include "well_known_classes.h"

double art_l2d(int64_t l) {
  return static_cast<double>(l);
}

float art_l2f(int64_t l) {
  return static_cast<float>(l);
}

/*
 * Float/double conversion requires clamping to min and max of integer form.  If
 * target doesn't support this normally, use these.
 */
int64_t art_d2l(double d) {
  static const double kMaxLong = static_cast<double>(static_cast<int64_t>(0x7fffffffffffffffULL));
  static const double kMinLong = static_cast<double>(static_cast<int64_t>(0x8000000000000000ULL));
  if (d >= kMaxLong) {
    return static_cast<int64_t>(0x7fffffffffffffffULL);
  } else if (d <= kMinLong) {
    return static_cast<int64_t>(0x8000000000000000ULL);
  } else if (d != d)  { // NaN case
    return 0;
  } else {
    return static_cast<int64_t>(d);
  }
}

int64_t art_f2l(float f) {
  static const float kMaxLong = static_cast<float>(static_cast<int64_t>(0x7fffffffffffffffULL));
  static const float kMinLong = static_cast<float>(static_cast<int64_t>(0x8000000000000000ULL));
  if (f >= kMaxLong) {
    return static_cast<int64_t>(0x7fffffffffffffffULL);
  } else if (f <= kMinLong) {
    return static_cast<int64_t>(0x8000000000000000ULL);
  } else if (f != f) { // NaN case
    return 0;
  } else {
    return static_cast<int64_t>(f);
  }
}

int32_t art_d2i(double d) {
  static const double kMaxInt = static_cast<double>(static_cast<int32_t>(0x7fffffffUL));
  static const double kMinInt = static_cast<double>(static_cast<int32_t>(0x80000000UL));
  if (d >= kMaxInt) {
    return static_cast<int32_t>(0x7fffffffUL);
  } else if (d <= kMinInt) {
    return static_cast<int32_t>(0x80000000UL);
  } else if (d != d)  { // NaN case
    return 0;
  } else {
    return static_cast<int32_t>(d);
  }
}

int32_t art_f2i(float f) {
  static const float kMaxInt = static_cast<float>(static_cast<int32_t>(0x7fffffffUL));
  static const float kMinInt = static_cast<float>(static_cast<int32_t>(0x80000000UL));
  if (f >= kMaxInt) {
    return static_cast<int32_t>(0x7fffffffUL);
  } else if (f <= kMinInt) {
    return static_cast<int32_t>(0x80000000UL);
  } else if (f != f) { // NaN case
    return 0;
  } else {
    return static_cast<int32_t>(f);
  }
}

namespace art {

// Helper function to allocate array for FILLED_NEW_ARRAY.
Array* CheckAndAllocArrayFromCode(uint32_t type_idx, AbstractMethod* method, int32_t component_count,
                                  Thread* self, bool access_check) {
  if (UNLIKELY(component_count < 0)) {
    self->ThrowNewExceptionF("Ljava/lang/NegativeArraySizeException;", "%d", component_count);
    return NULL;  // Failure
  }
  Class* klass = method->GetDexCacheResolvedTypes()->Get(type_idx);
  if (UNLIKELY(klass == NULL)) {  // Not in dex cache so try to resolve
    klass = Runtime::Current()->GetClassLinker()->ResolveType(type_idx, method);
    if (klass == NULL) {  // Error
      DCHECK(self->IsExceptionPending());
      return NULL;  // Failure
    }
  }
  if (UNLIKELY(klass->IsPrimitive() && !klass->IsPrimitiveInt())) {
    if (klass->IsPrimitiveLong() || klass->IsPrimitiveDouble()) {
      self->ThrowNewExceptionF("Ljava/lang/RuntimeException;",
                               "Bad filled array request for type %s",
                                PrettyDescriptor(klass).c_str());
    } else {
      self->ThrowNewExceptionF("Ljava/lang/InternalError;",
                               "Found type %s; filled-new-array not implemented for anything but \'int\'",
                               PrettyDescriptor(klass).c_str());
    }
    return NULL;  // Failure
  } else {
    if (access_check) {
      Class* referrer = method->GetDeclaringClass();
      if (UNLIKELY(!referrer->CanAccess(klass))) {
        ThrowIllegalAccessErrorClass(referrer, klass);
        return NULL;  // Failure
      }
    }
    DCHECK(klass->IsArrayClass()) << PrettyClass(klass);
    return Array::Alloc(self, klass, component_count);
  }
}

Field* FindFieldFromCode(uint32_t field_idx, const AbstractMethod* referrer, Thread* self,
                         FindFieldType type, size_t expected_size) {
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
  Field* resolved_field = class_linker->ResolveField(field_idx, referrer, is_static);
  if (UNLIKELY(resolved_field == NULL)) {
    DCHECK(self->IsExceptionPending());  // Throw exception and unwind.
    return NULL;  // Failure.
  } else {
    if (resolved_field->IsStatic() != is_static) {
      ThrowIncompatibleClassChangeErrorField(resolved_field, is_static, referrer);
      return NULL;
    }
    Class* fields_class = resolved_field->GetDeclaringClass();
    Class* referring_class = referrer->GetDeclaringClass();
    if (UNLIKELY(!referring_class->CanAccess(fields_class) ||
                 !referring_class->CanAccessMember(fields_class,
                                                   resolved_field->GetAccessFlags()))) {
      // The referring class can't access the resolved field, this may occur as a result of a
      // protected field being made public by a sub-class. Resort to the dex file to determine
      // the correct class for the access check.
      const DexFile& dex_file = *referring_class->GetDexCache()->GetDexFile();
      fields_class = class_linker->ResolveType(dex_file,
                                               dex_file.GetFieldId(field_idx).class_idx_,
                                               referring_class);
      if (UNLIKELY(!referring_class->CanAccess(fields_class))) {
        ThrowIllegalAccessErrorClass(referring_class, fields_class);
        return NULL;  // failure
      } else if (UNLIKELY(!referring_class->CanAccessMember(fields_class,
                                                            resolved_field->GetAccessFlags()))) {
        ThrowIllegalAccessErrorField(referring_class, resolved_field);
        return NULL;  // failure
      }
    }
    if (UNLIKELY(is_set && resolved_field->IsFinal() && (fields_class != referring_class))) {
      ThrowIllegalAccessErrorFinalField(referrer, resolved_field);
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
        } else if (Runtime::Current()->GetClassLinker()->EnsureInitialized(fields_class, true, true)) {
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
AbstractMethod* FindMethodFromCode(uint32_t method_idx, Object* this_object, AbstractMethod* referrer,
                           Thread* self, bool access_check, InvokeType type) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  bool is_direct = type == kStatic || type == kDirect;
  AbstractMethod* resolved_method = class_linker->ResolveMethod(method_idx, referrer, type);
  if (UNLIKELY(resolved_method == NULL)) {
    DCHECK(self->IsExceptionPending());  // Throw exception and unwind.
    return NULL;  // Failure.
  } else if (UNLIKELY(this_object == NULL && type != kStatic)) {
    // Maintain interpreter-like semantics where NullPointerException is thrown
    // after potential NoSuchMethodError from class linker.
    ThrowNullPointerExceptionForMethodAccess(referrer, method_idx, type);
    return NULL;  // Failure.
  } else {
    if (!access_check) {
      if (is_direct) {
        return resolved_method;
      } else if (type == kInterface) {
        AbstractMethod* interface_method =
            this_object->GetClass()->FindVirtualMethodForInterface(resolved_method);
        if (UNLIKELY(interface_method == NULL)) {
          ThrowIncompatibleClassChangeErrorClassForInterfaceDispatch(resolved_method, this_object,
                                                                     referrer);
          return NULL;  // Failure.
        } else {
          return interface_method;
        }
      } else {
        ObjectArray<AbstractMethod>* vtable;
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
      // Incompatible class change should have been handled in resolve method.
      if (UNLIKELY(resolved_method->CheckIncompatibleClassChange(type))) {
        ThrowIncompatibleClassChangeError(type, resolved_method->GetInvokeType(), resolved_method,
                                          referrer);
        return NULL;  // Failure.
      }
      Class* methods_class = resolved_method->GetDeclaringClass();
      Class* referring_class = referrer->GetDeclaringClass();
      if (UNLIKELY(!referring_class->CanAccess(methods_class) ||
                   !referring_class->CanAccessMember(methods_class,
                                                     resolved_method->GetAccessFlags()))) {
        // The referring class can't access the resolved method, this may occur as a result of a
        // protected method being made public by implementing an interface that re-declares the
        // method public. Resort to the dex file to determine the correct class for the access check
        const DexFile& dex_file = *referring_class->GetDexCache()->GetDexFile();
        methods_class = class_linker->ResolveType(dex_file,
                                                  dex_file.GetMethodId(method_idx).class_idx_,
                                                  referring_class);
        if (UNLIKELY(!referring_class->CanAccess(methods_class))) {
          ThrowIllegalAccessErrorClassForMethodDispatch(referring_class, methods_class,
                                                        referrer, resolved_method, type);
          return NULL;  // Failure.
        } else if (UNLIKELY(!referring_class->CanAccessMember(methods_class,
                                                              resolved_method->GetAccessFlags()))) {
          ThrowIllegalAccessErrorMethod(referring_class, resolved_method);
          return NULL;  // Failure.
        }
      }
      if (is_direct) {
        return resolved_method;
      } else if (type == kInterface) {
        AbstractMethod* interface_method =
            this_object->GetClass()->FindVirtualMethodForInterface(resolved_method);
        if (UNLIKELY(interface_method == NULL)) {
          ThrowIncompatibleClassChangeErrorClassForInterfaceDispatch(resolved_method, this_object,
                                                                     referrer);
          return NULL;  // Failure.
        } else {
          return interface_method;
        }
      } else {
        ObjectArray<AbstractMethod>* vtable;
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
          // Behavior to agree with that of the verifier.
          MethodHelper mh(resolved_method);
          ThrowNoSuchMethodError(type, resolved_method->GetDeclaringClass(), mh.GetName(),
                                 mh.GetSignature(), referrer);
          return NULL;  // Failure.
        }
      }
    }
  }
}

Class* ResolveVerifyAndClinit(uint32_t type_idx, const AbstractMethod* referrer, Thread* self,
                               bool can_run_clinit, bool verify_access) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Class* klass = class_linker->ResolveType(type_idx, referrer);
  if (UNLIKELY(klass == NULL)) {
    CHECK(self->IsExceptionPending());
    return NULL;  // Failure - Indicate to caller to deliver exception
  }
  // Perform access check if necessary.
  Class* referring_class = referrer->GetDeclaringClass();
  if (verify_access && UNLIKELY(!referring_class->CanAccess(klass))) {
    ThrowIllegalAccessErrorClass(referring_class, klass);
    return NULL;  // Failure - Indicate to caller to deliver exception
  }
  // If we're just implementing const-class, we shouldn't call <clinit>.
  if (!can_run_clinit) {
    return klass;
  }
  // If we are the <clinit> of this class, just return our storage.
  //
  // Do not set the DexCache InitializedStaticStorage, since that implies <clinit> has finished
  // running.
  if (klass == referring_class && MethodHelper(referrer).IsClassInitializer()) {
    return klass;
  }
  if (!class_linker->EnsureInitialized(klass, true, true)) {
    CHECK(self->IsExceptionPending());
    return NULL;  // Failure - Indicate to caller to deliver exception
  }
  referrer->GetDexCacheInitializedStaticStorage()->Set(type_idx, klass);
  return klass;
}

void ThrowStackOverflowError(Thread* self) {
  CHECK(!self->IsHandlingStackOverflow()) << "Recursive stack overflow.";
  // Remove extra entry pushed onto second stack during method tracing.
  if (Runtime::Current()->IsMethodTracingActive()) {
    InstrumentationMethodUnwindFromCode(self);
  }
  self->SetStackEndForStackOverflow();  // Allow space on the stack for constructor to execute.
  JNIEnvExt* env = self->GetJniEnv();
  std::string msg("stack size ");
  msg += PrettySize(self->GetStackSize());
  // Use low-level JNI routine and pre-baked error class to avoid class linking operations that
  // would consume more stack.
  int rc = ::art::ThrowNewException(env, WellKnownClasses::java_lang_StackOverflowError,
                                    msg.c_str(), NULL);
  if (rc != JNI_OK) {
    // TODO: ThrowNewException failed presumably because of an OOME, we continue to throw the OOME
    //       or die in the CHECK below. We may want to throw a pre-baked StackOverflowError
    //       instead.
    LOG(ERROR) << "Couldn't throw new StackOverflowError because JNI ThrowNew failed.";
    CHECK(self->IsExceptionPending());
  }
  self->ResetDefaultStackEnd();  // Return to default stack size.
}

JValue InvokeProxyInvocationHandler(ScopedObjectAccessUnchecked& soa, const char* shorty,
                                    jobject rcvr_jobj, jobject interface_method_jobj,
                                    std::vector<jvalue>& args) {
  DCHECK(soa.Env()->IsInstanceOf(rcvr_jobj, WellKnownClasses::java_lang_reflect_Proxy));

  // Build argument array possibly triggering GC.
  soa.Self()->AssertThreadSuspensionIsAllowable();
  jobjectArray args_jobj = NULL;
  const JValue zero;
  if (args.size() > 0) {
    args_jobj = soa.Env()->NewObjectArray(args.size(), WellKnownClasses::java_lang_Object, NULL);
    if (args_jobj == NULL) {
      CHECK(soa.Self()->IsExceptionPending());
      return zero;
    }
    for (size_t i = 0; i < args.size(); ++i) {
      if (shorty[i + 1] == 'L') {
        jobject val = args.at(i).l;
        soa.Env()->SetObjectArrayElement(args_jobj, i, val);
      } else {
        JValue jv;
        jv.SetJ(args.at(i).j);
        Object* val = BoxPrimitive(Primitive::GetType(shorty[i + 1]), jv);
        if (val == NULL) {
          CHECK(soa.Self()->IsExceptionPending());
          return zero;
        }
        soa.Decode<ObjectArray<Object>* >(args_jobj)->Set(i, val);
      }
    }
  }

  // Call InvocationHandler.invoke(Object proxy, Method method, Object[] args).
  jobject inv_hand = soa.Env()->GetObjectField(rcvr_jobj,
                                               WellKnownClasses::java_lang_reflect_Proxy_h);
  jvalue invocation_args[3];
  invocation_args[0].l = rcvr_jobj;
  invocation_args[1].l = interface_method_jobj;
  invocation_args[2].l = args_jobj;
  jobject result =
      soa.Env()->CallObjectMethodA(inv_hand,
                                   WellKnownClasses::java_lang_reflect_InvocationHandler_invoke,
                                   invocation_args);

  // Unbox result and handle error conditions.
  if (!soa.Self()->IsExceptionPending()) {
    if (shorty[0] == 'V' || result == NULL) {
      // Do nothing.
      return zero;
    } else {
      JValue result_unboxed;
      MethodHelper mh(soa.Decode<AbstractMethod*>(interface_method_jobj));
      Class* result_type = mh.GetReturnType();
      Object* result_ref = soa.Decode<Object*>(result);
      bool unboxed_okay = UnboxPrimitiveForResult(result_ref, result_type, result_unboxed);
      if (!unboxed_okay) {
        soa.Self()->ThrowNewWrappedException("Ljava/lang/ClassCastException;",
                                             StringPrintf("Couldn't convert result of type %s to %s",
                                                          PrettyTypeOf(result_ref).c_str(),
                                                          PrettyDescriptor(result_type).c_str()
                                             ).c_str());
      }
      return result_unboxed;
    }
  } else {
    // In the case of checked exceptions that aren't declared, the exception must be wrapped by
    // a UndeclaredThrowableException.
    Throwable* exception = soa.Self()->GetException();
    if (exception->IsCheckedException()) {
      Object* rcvr = soa.Decode<Object*>(rcvr_jobj);
      SynthesizedProxyClass* proxy_class = down_cast<SynthesizedProxyClass*>(rcvr->GetClass());
      AbstractMethod* interface_method = soa.Decode<AbstractMethod*>(interface_method_jobj);
      AbstractMethod* proxy_method =
          rcvr->GetClass()->FindVirtualMethodForInterface(interface_method);
      int throws_index = -1;
      size_t num_virt_methods = proxy_class->NumVirtualMethods();
      for (size_t i = 0; i < num_virt_methods; i++) {
        if (proxy_class->GetVirtualMethod(i) == proxy_method) {
          throws_index = i;
          break;
        }
      }
      CHECK_NE(throws_index, -1);
      ObjectArray<Class>* declared_exceptions = proxy_class->GetThrows()->Get(throws_index);
      Class* exception_class = exception->GetClass();
      bool declares_exception = false;
      for (int i = 0; i < declared_exceptions->GetLength() && !declares_exception; i++) {
        Class* declared_exception = declared_exceptions->Get(i);
        declares_exception = declared_exception->IsAssignableFrom(exception_class);
      }
      if (!declares_exception) {
        soa.Self()->ThrowNewWrappedException("Ljava/lang/reflect/UndeclaredThrowableException;",
                                             NULL);
      }
    }
    return zero;
  }
}

}  // namespace art
