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

#include "art_field-inl.h"
#include "art_method-inl.h"
#include "base/mutex.h"
#include "class_linker-inl.h"
#include "dex_file-inl.h"
#include "gc/accounting/card_table-inl.h"
#include "mirror/class-inl.h"
#include "mirror/method.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "reflection.h"
#include "scoped_thread_state_change.h"
#include "ScopedLocalRef.h"
#include "well_known_classes.h"

namespace art {

static inline mirror::Class* CheckFilledNewArrayAlloc(uint32_t type_idx,
                                                      int32_t component_count,
                                                      ArtMethod* referrer,
                                                      Thread* self,
                                                      bool access_check)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (UNLIKELY(component_count < 0)) {
    ThrowNegativeArraySizeException(component_count);
    return nullptr;  // Failure
  }
  mirror::Class* klass = referrer->GetDexCacheResolvedType<false>(type_idx);
  if (UNLIKELY(klass == nullptr)) {  // Not in dex cache so try to resolve
    klass = Runtime::Current()->GetClassLinker()->ResolveType(type_idx, referrer);
    if (klass == nullptr) {  // Error
      DCHECK(self->IsExceptionPending());
      return nullptr;  // Failure
    }
  }
  if (UNLIKELY(klass->IsPrimitive() && !klass->IsPrimitiveInt())) {
    if (klass->IsPrimitiveLong() || klass->IsPrimitiveDouble()) {
      ThrowRuntimeException("Bad filled array request for type %s",
                            PrettyDescriptor(klass).c_str());
    } else {
      self->ThrowNewExceptionF(
          "Ljava/lang/InternalError;",
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
mirror::Array* CheckAndAllocArrayFromCode(uint32_t type_idx, int32_t component_count,
                                          ArtMethod* referrer, Thread* self,
                                          bool access_check,
                                          gc::AllocatorType /* allocator_type */) {
  mirror::Class* klass = CheckFilledNewArrayAlloc(type_idx, component_count, referrer, self,
                                                  access_check);
  if (UNLIKELY(klass == nullptr)) {
    return nullptr;
  }
  // Always go slow path for now, filled new array is not common.
  gc::Heap* heap = Runtime::Current()->GetHeap();
  // Use the current allocator type in case CheckFilledNewArrayAlloc caused us to suspend and then
  // the heap switched the allocator type while we were suspended.
  return mirror::Array::Alloc<false>(self, klass, component_count,
                                     klass->GetComponentSizeShift(),
                                     heap->GetCurrentAllocator());
}

// Helper function to allocate array for FILLED_NEW_ARRAY.
mirror::Array* CheckAndAllocArrayFromCodeInstrumented(uint32_t type_idx,
                                                      int32_t component_count,
                                                      ArtMethod* referrer,
                                                      Thread* self,
                                                      bool access_check,
                                                      gc::AllocatorType /* allocator_type */) {
  mirror::Class* klass = CheckFilledNewArrayAlloc(type_idx, component_count, referrer, self,
                                                  access_check);
  if (UNLIKELY(klass == nullptr)) {
    return nullptr;
  }
  gc::Heap* heap = Runtime::Current()->GetHeap();
  // Use the current allocator type in case CheckFilledNewArrayAlloc caused us to suspend and then
  // the heap switched the allocator type while we were suspended.
  return mirror::Array::Alloc<true>(self, klass, component_count,
                                    klass->GetComponentSizeShift(),
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
      } else {
        error_msg = "Could not create stack trace.";
      }
      // Throw the exception.
      self->SetException(reinterpret_cast<mirror::Throwable*>(self->DecodeJObject(exc.get())));
    } else {
      // Could not allocate a string object.
      error_msg = "Couldn't throw new StackOverflowError because JNI NewStringUTF failed.";
    }
  } else {
    error_msg = "Could not allocate StackOverflowError object.";
  }

  if (!error_msg.empty()) {
    LOG(WARNING) << error_msg;
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
  if (o == nullptr) {
    return;
  }
  // Make sure that the result is an instance of the type this method was expected to return.
  mirror::Class* return_type = self->GetCurrentMethod(nullptr)->GetReturnType();

  if (!o->InstanceOf(return_type)) {
    Runtime::Current()->GetJavaVM()->JniAbortF(nullptr,
                                               "attempt to return an instance of %s from %s",
                                               PrettyTypeOf(o).c_str(),
                                               PrettyMethod(self->GetCurrentMethod(nullptr)).c_str());
  }
}

JValue InvokeProxyInvocationHandler(ScopedObjectAccessAlreadyRunnable& soa, const char* shorty,
                                    jobject rcvr_jobj, jobject interface_method_jobj,
                                    std::vector<jvalue>& args) {
  DCHECK(soa.Env()->IsInstanceOf(rcvr_jobj, WellKnownClasses::java_lang_reflect_Proxy));

  // Build argument array possibly triggering GC.
  soa.Self()->AssertThreadSuspensionIsAllowable();
  jobjectArray args_jobj = nullptr;
  const JValue zero;
  int32_t target_sdk_version = Runtime::Current()->GetTargetSdkVersion();
  // Do not create empty arrays unless needed to maintain Dalvik bug compatibility.
  if (args.size() > 0 || (target_sdk_version > 0 && target_sdk_version <= 21)) {
    args_jobj = soa.Env()->NewObjectArray(args.size(), WellKnownClasses::java_lang_Object, nullptr);
    if (args_jobj == nullptr) {
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
        if (val == nullptr) {
          CHECK(soa.Self()->IsExceptionPending());
          return zero;
        }
        soa.Decode<mirror::ObjectArray<mirror::Object>* >(args_jobj)->Set<false>(i, val);
      }
    }
  }

  // Call Proxy.invoke(Proxy proxy, Method method, Object[] args).
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
    if (shorty[0] == 'V' || (shorty[0] == 'L' && result == nullptr)) {
      // Do nothing.
      return zero;
    } else {
      StackHandleScope<1> hs(soa.Self());
      auto h_interface_method(hs.NewHandle(soa.Decode<mirror::Method*>(interface_method_jobj)));
      // This can cause thread suspension.
      mirror::Class* result_type = h_interface_method->GetArtMethod()->GetReturnType();
      mirror::Object* result_ref = soa.Decode<mirror::Object*>(result);
      JValue result_unboxed;
      if (!UnboxPrimitiveForResult(result_ref, result_type, &result_unboxed)) {
        DCHECK(soa.Self()->IsExceptionPending());
        return zero;
      }
      return result_unboxed;
    }
  } else {
    // In the case of checked exceptions that aren't declared, the exception must be wrapped by
    // a UndeclaredThrowableException.
    mirror::Throwable* exception = soa.Self()->GetException();
    if (exception->IsCheckedException()) {
      mirror::Object* rcvr = soa.Decode<mirror::Object*>(rcvr_jobj);
      mirror::Class* proxy_class = rcvr->GetClass();
      mirror::Method* interface_method = soa.Decode<mirror::Method*>(interface_method_jobj);
      ArtMethod* proxy_method = rcvr->GetClass()->FindVirtualMethodForInterface(
          interface_method->GetArtMethod(), sizeof(void*));
      auto* virtual_methods = proxy_class->GetVirtualMethodsPtr();
      size_t num_virtuals = proxy_class->NumVirtualMethods();
      size_t method_size = ArtMethod::ObjectSize(sizeof(void*));
      int throws_index = (reinterpret_cast<uintptr_t>(proxy_method) -
          reinterpret_cast<uintptr_t>(virtual_methods)) / method_size;
      CHECK_LT(throws_index, static_cast<int>(num_virtuals));
      mirror::ObjectArray<mirror::Class>* declared_exceptions =
          proxy_class->GetThrows()->Get(throws_index);
      mirror::Class* exception_class = exception->GetClass();
      bool declares_exception = false;
      for (int32_t i = 0; i < declared_exceptions->GetLength() && !declares_exception; i++) {
        mirror::Class* declared_exception = declared_exceptions->Get(i);
        declares_exception = declared_exception->IsAssignableFrom(exception_class);
      }
      if (!declares_exception) {
        soa.Self()->ThrowNewWrappedException("Ljava/lang/reflect/UndeclaredThrowableException;",
                                             nullptr);
      }
    }
    return zero;
  }
}

bool FillArrayData(mirror::Object* obj, const Instruction::ArrayDataPayload* payload) {
  DCHECK_EQ(payload->ident, static_cast<uint16_t>(Instruction::kArrayDataSignature));
  if (UNLIKELY(obj == nullptr)) {
    ThrowNullPointerException("null array in FILL_ARRAY_DATA");
    return false;
  }
  mirror::Array* array = obj->AsArray();
  DCHECK(!array->IsObjectArray());
  if (UNLIKELY(static_cast<int32_t>(payload->element_count) > array->GetLength())) {
    Thread* self = Thread::Current();
    self->ThrowNewExceptionF("Ljava/lang/ArrayIndexOutOfBoundsException;",
                             "failed FILL_ARRAY_DATA; length=%d, index=%d",
                             array->GetLength(), payload->element_count);
    return false;
  }
  // Copy data from dex file to memory assuming both are little endian.
  uint32_t size_in_bytes = payload->element_count * payload->element_width;
  memcpy(array->GetRawData(payload->element_width, 0), payload->data, size_in_bytes);
  return true;
}

}  // namespace art
