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

#include "entrypoints/entrypoint_utils.h"

#include "base/mutex.h"
#include "class_linker-inl.h"
#include "dex_file-inl.h"
#include "gc/accounting/card_table-inl.h"
#include "method_helper-inl.h"
#include "mirror/art_field-inl.h"
#include "mirror/art_method-inl.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "reflection.h"
#include "scoped_thread_state_change.h"
#include "ScopedLocalRef.h"
#include "well_known_classes.h"

namespace art {

static inline mirror::Class* CheckFilledNewArrayAlloc(uint32_t type_idx,
                                                      mirror::ArtMethod* referrer,
                                                      int32_t component_count,
                                                      Thread* self,
                                                      bool access_check)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (UNLIKELY(component_count < 0)) {
    ThrowNegativeArraySizeException(component_count);
    return nullptr;  // Failure
  }
  mirror::Class* klass = referrer->GetDexCacheResolvedType<false>(type_idx);
  if (UNLIKELY(klass == NULL)) {  // Not in dex cache so try to resolve
    klass = Runtime::Current()->GetClassLinker()->ResolveType(type_idx, referrer);
    if (klass == NULL) {  // Error
      DCHECK(self->IsExceptionPending());
      return nullptr;  // Failure
    }
  }
  if (UNLIKELY(klass->IsPrimitive() && !klass->IsPrimitiveInt())) {
    if (klass->IsPrimitiveLong() || klass->IsPrimitiveDouble()) {
      ThrowRuntimeException("Bad filled array request for type %s",
                            PrettyDescriptor(klass).c_str());
    } else {
      ThrowLocation throw_location = self->GetCurrentLocationForThrow();
      DCHECK(throw_location.GetMethod() == referrer);
      self->ThrowNewExceptionF(
          throw_location, "Ljava/lang/InternalError;",
          "Found type %s; filled-new-array not implemented for anything but 'int'",
          PrettyDescriptor(klass).c_str());
    }
    return nullptr;  // Failure
  }
  if (access_check) {
    mirror::Class* referrer_klass = referrer->GetDeclaringClass();
    if (UNLIKELY(!referrer_klass->CanAccess(klass))) {
      ThrowIllegalAccessErrorClass(referrer_klass, klass);
      return nullptr;  // Failure
    }
  }
  DCHECK(klass->IsArrayClass()) << PrettyClass(klass);
  return klass;
}

// Helper function to allocate array for FILLED_NEW_ARRAY.
mirror::Array* CheckAndAllocArrayFromCode(uint32_t type_idx, mirror::ArtMethod* referrer,
                                          int32_t component_count, Thread* self,
                                          bool access_check,
                                          gc::AllocatorType /* allocator_type */) {
  mirror::Class* klass = CheckFilledNewArrayAlloc(type_idx, referrer, component_count, self,
                                                  access_check);
  if (UNLIKELY(klass == nullptr)) {
    return nullptr;
  }
  // Always go slow path for now, filled new array is not common.
  gc::Heap* heap = Runtime::Current()->GetHeap();
  // Use the current allocator type in case CheckFilledNewArrayAlloc caused us to suspend and then
  // the heap switched the allocator type while we were suspended.
  return mirror::Array::Alloc<false>(self, klass, component_count, klass->GetComponentSize(),
                                     heap->GetCurrentAllocator());
}

// Helper function to allocate array for FILLED_NEW_ARRAY.
mirror::Array* CheckAndAllocArrayFromCodeInstrumented(uint32_t type_idx,
                                                      mirror::ArtMethod* referrer,
                                                      int32_t component_count,
                                                      Thread* self,
                                                      bool access_check,
                                                      gc::AllocatorType /* allocator_type */) {
  mirror::Class* klass = CheckFilledNewArrayAlloc(type_idx, referrer, component_count, self,
                                                  access_check);
  if (UNLIKELY(klass == nullptr)) {
    return nullptr;
  }
  gc::Heap* heap = Runtime::Current()->GetHeap();
  // Use the current allocator type in case CheckFilledNewArrayAlloc caused us to suspend and then
  // the heap switched the allocator type while we were suspended.
  return mirror::Array::Alloc<true>(self, klass, component_count, klass->GetComponentSize(),
                                    heap->GetCurrentAllocator());
}

void ThrowStackOverflowError(Thread* self) {
  if (self->IsHandlingStackOverflow()) {
    LOG(ERROR) << "Recursive stack overflow.";
    // We don't fail here because SetStackEndForStackOverflow will print better diagnostics.
  }

  self->SetStackEndForStackOverflow();  // Allow space on the stack for constructor to execute.
  JNIEnvExt* env = self->GetJniEnv();
  std::string msg("stack size ");
  msg += PrettySize(self->GetStackSize());

  // Avoid running Java code for exception initialization.
  // TODO: Checks to make this a bit less brittle.

  std::string error_msg;

  // Allocate an uninitialized object.
  ScopedLocalRef<jobject> exc(env,
                              env->AllocObject(WellKnownClasses::java_lang_StackOverflowError));
  if (exc.get() != nullptr) {
    // "Initialize".
    // StackOverflowError -> VirtualMachineError -> Error -> Throwable -> Object.
    // Only Throwable has "custom" fields:
    //   String detailMessage.
    //   Throwable cause (= this).
    //   List<Throwable> suppressedExceptions (= Collections.emptyList()).
    //   Object stackState;
    //   StackTraceElement[] stackTrace;
    // Only Throwable has a non-empty constructor:
    //   this.stackTrace = EmptyArray.STACK_TRACE_ELEMENT;
    //   fillInStackTrace();

    // detailMessage.
    // TODO: Use String::FromModifiedUTF...?
    ScopedLocalRef<jstring> s(env, env->NewStringUTF(msg.c_str()));
    if (s.get() != nullptr) {
      env->SetObjectField(exc.get(), WellKnownClasses::java_lang_Throwable_detailMessage, s.get());

      // cause.
      env->SetObjectField(exc.get(), WellKnownClasses::java_lang_Throwable_cause, exc.get());

      // suppressedExceptions.
      ScopedLocalRef<jobject> emptylist(env, env->GetStaticObjectField(
          WellKnownClasses::java_util_Collections,
          WellKnownClasses::java_util_Collections_EMPTY_LIST));
      CHECK(emptylist.get() != nullptr);
      env->SetObjectField(exc.get(),
                          WellKnownClasses::java_lang_Throwable_suppressedExceptions,
                          emptylist.get());

      // stackState is set as result of fillInStackTrace. fillInStackTrace calls
      // nativeFillInStackTrace.
      ScopedLocalRef<jobject> stack_state_val(env, nullptr);
      {
        ScopedObjectAccessUnchecked soa(env);
        stack_state_val.reset(soa.Self()->CreateInternalStackTrace<false>(soa));
      }
      if (stack_state_val.get() != nullptr) {
        env->SetObjectField(exc.get(),
                            WellKnownClasses::java_lang_Throwable_stackState,
                            stack_state_val.get());

        // stackTrace.
        ScopedLocalRef<jobject> stack_trace_elem(env, env->GetStaticObjectField(
            WellKnownClasses::libcore_util_EmptyArray,
            WellKnownClasses::libcore_util_EmptyArray_STACK_TRACE_ELEMENT));
        env->SetObjectField(exc.get(),
                            WellKnownClasses::java_lang_Throwable_stackTrace,
                            stack_trace_elem.get());

        // Throw the exception.
        ThrowLocation throw_location = self->GetCurrentLocationForThrow();
        self->SetException(throw_location,
            reinterpret_cast<mirror::Throwable*>(self->DecodeJObject(exc.get())));
      } else {
        error_msg = "Could not create stack trace.";
      }
    } else {
      // Could not allocate a string object.
      error_msg = "Couldn't throw new StackOverflowError because JNI NewStringUTF failed.";
    }
  } else {
    error_msg = "Could not allocate StackOverflowError object.";
  }

  if (!error_msg.empty()) {
    LOG(ERROR) << error_msg;
    CHECK(self->IsExceptionPending());
  }

  bool explicit_overflow_check = Runtime::Current()->ExplicitStackOverflowChecks();
  self->ResetDefaultStackEnd();  // Return to default stack size.

  // And restore protection if implicit checks are on.
  if (!explicit_overflow_check) {
    self->ProtectStack();
  }
}

void CheckReferenceResult(mirror::Object* o, Thread* self) {
  if (o == NULL) {
    return;
  }
  mirror::ArtMethod* m = self->GetCurrentMethod(NULL);
  if (o == kInvalidIndirectRefObject) {
    JniAbortF(NULL, "invalid reference returned from %s", PrettyMethod(m).c_str());
  }
  // Make sure that the result is an instance of the type this method was expected to return.
  StackHandleScope<1> hs(self);
  Handle<mirror::ArtMethod> h_m(hs.NewHandle(m));
  mirror::Class* return_type = MethodHelper(h_m).GetReturnType();

  if (!o->InstanceOf(return_type)) {
    JniAbortF(NULL, "attempt to return an instance of %s from %s", PrettyTypeOf(o).c_str(),
              PrettyMethod(h_m.Get()).c_str());
  }
}

JValue InvokeProxyInvocationHandler(ScopedObjectAccessAlreadyRunnable& soa, const char* shorty,
                                    jobject rcvr_jobj, jobject interface_method_jobj,
                                    std::vector<jvalue>& args) {
  DCHECK(soa.Env()->IsInstanceOf(rcvr_jobj, WellKnownClasses::java_lang_reflect_Proxy));

  // Build argument array possibly triggering GC.
  soa.Self()->AssertThreadSuspensionIsAllowable();
  jobjectArray args_jobj = NULL;
  const JValue zero;
  int32_t target_sdk_version = Runtime::Current()->GetTargetSdkVersion();
  // Do not create empty arrays unless needed to maintain Dalvik bug compatibility.
  if (args.size() > 0 || (target_sdk_version > 0 && target_sdk_version <= 21)) {
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
        mirror::Object* val = BoxPrimitive(Primitive::GetType(shorty[i + 1]), jv);
        if (val == NULL) {
          CHECK(soa.Self()->IsExceptionPending());
          return zero;
        }
        soa.Decode<mirror::ObjectArray<mirror::Object>* >(args_jobj)->Set<false>(i, val);
      }
    }
  }

  // Call Proxy.invoke(Proxy proxy, ArtMethod method, Object[] args).
  jvalue invocation_args[3];
  invocation_args[0].l = rcvr_jobj;
  invocation_args[1].l = interface_method_jobj;
  invocation_args[2].l = args_jobj;
  jobject result =
      soa.Env()->CallStaticObjectMethodA(WellKnownClasses::java_lang_reflect_Proxy,
                                         WellKnownClasses::java_lang_reflect_Proxy_invoke,
                                         invocation_args);

  // Unbox result and handle error conditions.
  if (LIKELY(!soa.Self()->IsExceptionPending())) {
    if (shorty[0] == 'V' || (shorty[0] == 'L' && result == NULL)) {
      // Do nothing.
      return zero;
    } else {
      StackHandleScope<1> hs(soa.Self());
      MethodHelper mh_interface_method(
          hs.NewHandle(soa.Decode<mirror::ArtMethod*>(interface_method_jobj)));
      // This can cause thread suspension.
      mirror::Class* result_type = mh_interface_method.GetReturnType();
      mirror::Object* result_ref = soa.Decode<mirror::Object*>(result);
      mirror::Object* rcvr = soa.Decode<mirror::Object*>(rcvr_jobj);
      mirror::ArtMethod* proxy_method;
      if (mh_interface_method.GetMethod()->GetDeclaringClass()->IsInterface()) {
        proxy_method = rcvr->GetClass()->FindVirtualMethodForInterface(
            mh_interface_method.GetMethod());
      } else {
        // Proxy dispatch to a method defined in Object.
        DCHECK(mh_interface_method.GetMethod()->GetDeclaringClass()->IsObjectClass());
        proxy_method = mh_interface_method.GetMethod();
      }
      ThrowLocation throw_location(rcvr, proxy_method, -1);
      JValue result_unboxed;
      if (!UnboxPrimitiveForResult(throw_location, result_ref, result_type, &result_unboxed)) {
        DCHECK(soa.Self()->IsExceptionPending());
        return zero;
      }
      return result_unboxed;
    }
  } else {
    // In the case of checked exceptions that aren't declared, the exception must be wrapped by
    // a UndeclaredThrowableException.
    mirror::Throwable* exception = soa.Self()->GetException(NULL);
    if (exception->IsCheckedException()) {
      mirror::Object* rcvr = soa.Decode<mirror::Object*>(rcvr_jobj);
      mirror::Class* proxy_class = rcvr->GetClass();
      mirror::ArtMethod* interface_method =
          soa.Decode<mirror::ArtMethod*>(interface_method_jobj);
      mirror::ArtMethod* proxy_method =
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
      mirror::ObjectArray<mirror::Class>* declared_exceptions =
          proxy_class->GetThrows()->Get(throws_index);
      mirror::Class* exception_class = exception->GetClass();
      bool declares_exception = false;
      for (int i = 0; i < declared_exceptions->GetLength() && !declares_exception; i++) {
        mirror::Class* declared_exception = declared_exceptions->Get(i);
        declares_exception = declared_exception->IsAssignableFrom(exception_class);
      }
      if (!declares_exception) {
        ThrowLocation throw_location(rcvr, proxy_method, -1);
        soa.Self()->ThrowNewWrappedException(throw_location,
                                             "Ljava/lang/reflect/UndeclaredThrowableException;",
                                             NULL);
      }
    }
    return zero;
  }
}

JValue InvokeXposedHandleHookedMethod(ScopedObjectAccessAlreadyRunnable& soa, const char* shorty,
                                    jobject rcvr_jobj, jmethodID method,
                                    std::vector<jvalue>& args) {
  // Build argument array possibly triggering GC.
  soa.Self()->AssertThreadSuspensionIsAllowable();
  jobjectArray args_jobj = NULL;
  const JValue zero;
  int32_t target_sdk_version = Runtime::Current()->GetTargetSdkVersion();
  // Do not create empty arrays unless needed to maintain Dalvik bug compatibility.
  if (args.size() > 0 || (target_sdk_version > 0 && target_sdk_version <= 21)) {
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
        mirror::Object* val = BoxPrimitive(Primitive::GetType(shorty[i + 1]), jv);
        if (val == NULL) {
          CHECK(soa.Self()->IsExceptionPending());
          return zero;
        }
        soa.Decode<mirror::ObjectArray<mirror::Object>* >(args_jobj)->Set<false>(i, val);
      }
    }
  }

  XposedHookInfo* hookInfo = soa.DecodeMethod(method)->GetXposedHookInfo();

  // Call XposedBridge.handleHookedMethod(Member method, int originalMethodId, Object additionalInfoObj,
  //                                      Object thisObject, Object[] args)
  jvalue invocation_args[5];
  invocation_args[0].l = hookInfo->reflectedMethod;
  invocation_args[1].i = 0;
  invocation_args[2].l = hookInfo->additionalInfo;
  invocation_args[3].l = rcvr_jobj;
  invocation_args[4].l = args_jobj;
  jobject result =
      soa.Env()->CallStaticObjectMethodA(mirror::ArtMethod::xposed_callback_class,
                                         mirror::ArtMethod::xposed_callback_method,
                                         invocation_args);


  // Unbox the result if necessary and return it.
  if (UNLIKELY(soa.Self()->IsExceptionPending())) {
    return zero;
  } else {
    if (shorty[0] == 'V' || (shorty[0] == 'L' && result == NULL)) {
      return zero;
    }
    StackHandleScope<1> hs(soa.Self());
    MethodHelper mh_method(hs.NewHandle(soa.DecodeMethod(method)));
    // This can cause thread suspension.
    mirror::Object* rcvr = soa.Decode<mirror::Object*>(rcvr_jobj);
    ThrowLocation throw_location(rcvr, mh_method.GetMethod(), -1);
    mirror::Object* result_ref = soa.Decode<mirror::Object*>(result);
    mirror::Class* result_type = mh_method.GetReturnType();
    JValue result_unboxed;
    if (!UnboxPrimitiveForResult(throw_location, result_ref, result_type, &result_unboxed)) {
      DCHECK(soa.Self()->IsExceptionPending());
      return zero;
    }
    return result_unboxed;
  }
}

}  // namespace art
