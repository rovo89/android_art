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

#include "object.h"
#include "object_utils.h"
#include "reflection.h"
#include "runtime_support.h"
#include "scoped_thread_state_change.h"
#include "thread.h"
#include "well_known_classes.h"

#include "ScopedLocalRef.h"

#if defined(__arm__)
#define SP_OFFSET_IN_BYTES 12
#define FRAME_SIZE_IN_BYTES 48u
#define ARG2_OFFSET_IN_WORDS 11 // offset to 3rd arg; skip callee saves, LR, Method* and out arg spills for OUT0 to OUT2
#elif defined(__mips__)
#define SP_OFFSET_IN_BYTES 8
#define FRAME_SIZE_IN_BYTES 32u
#define ARG2_OFFSET_IN_WORDS 8 // offset to 3rd arg; skip callee saves, LR, Method* and out arg spills for OUT0 to OUT2
#elif defined(__i386__)
#define SP_OFFSET_IN_BYTES 8
#define FRAME_SIZE_IN_BYTES 32u
#define ARG2_OFFSET_IN_WORDS 8 // offset to 3rd arg; skip callee saves, LR, Method* and out arg spills for OUT0 to OUT2
#else
#error "Unsupported architecture"
#endif

namespace art {

// Handler for invocation on proxy methods. On entry a frame will exist for the proxy object method
// which is responsible for recording callee save registers. We explicitly handlerize incoming
// reference arguments (so they survive GC) and create a boxed argument array. Finally we invoke
// the invocation handler which is a field within the proxy object receiver.
extern "C" void artProxyInvokeHandler(Method* proxy_method, Object* receiver,
                                      Thread* self, byte* stack_args)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  // Register the top of the managed stack
  Method** proxy_sp = reinterpret_cast<Method**>(stack_args - SP_OFFSET_IN_BYTES);
  DCHECK_EQ(*proxy_sp, proxy_method);
  self->SetTopOfStack(proxy_sp, 0);
  DCHECK_EQ(proxy_method->GetFrameSizeInBytes(), FRAME_SIZE_IN_BYTES);
  // Start new JNI local reference state
  JNIEnvExt* env = self->GetJniEnv();
  ScopedObjectAccessUnchecked soa(env);
  ScopedJniEnvLocalRefState env_state(env);
  // Create local ref. copies of proxy method and the receiver
  jobject rcvr_jobj = soa.AddLocalReference<jobject>(receiver);
  jobject proxy_method_jobj = soa.AddLocalReference<jobject>(proxy_method);

  // Placing into local references incoming arguments from the caller's register arguments,
  // replacing original Object* with jobject
  MethodHelper proxy_mh(proxy_method);
  const size_t num_params = proxy_mh.NumArgs();
  size_t args_in_regs = 0;
  for (size_t i = 1; i < num_params; i++) {  // skip receiver
    args_in_regs = args_in_regs + (proxy_mh.IsParamALongOrDouble(i) ? 2 : 1);
    if (args_in_regs > 2) {
      args_in_regs = 2;
      break;
    }
  }
  size_t cur_arg = 0;  // current stack location to read
  size_t param_index = 1;  // skip receiver
  while (cur_arg < args_in_regs && param_index < num_params) {
    if (proxy_mh.IsParamAReference(param_index)) {
      Object* obj = *reinterpret_cast<Object**>(stack_args + (cur_arg * kPointerSize));
      jobject jobj = soa.AddLocalReference<jobject>(obj);
      *reinterpret_cast<jobject*>(stack_args + (cur_arg * kPointerSize)) = jobj;
    }
    cur_arg = cur_arg + (proxy_mh.IsParamALongOrDouble(param_index) ? 2 : 1);
    param_index++;
  }
  // Placing into local references incoming arguments from the caller's stack arguments
  cur_arg += ARG2_OFFSET_IN_WORDS;
  while (param_index < num_params) {
    if (proxy_mh.IsParamAReference(param_index)) {
      Object* obj = *reinterpret_cast<Object**>(stack_args + (cur_arg * kPointerSize));
      jobject jobj = soa.AddLocalReference<jobject>(obj);
      *reinterpret_cast<jobject*>(stack_args + (cur_arg * kPointerSize)) = jobj;
    }
    cur_arg = cur_arg + (proxy_mh.IsParamALongOrDouble(param_index) ? 2 : 1);
    param_index++;
  }
  // Set up arguments array and place in local IRT during boxing (which may allocate/GC)
  jvalue args_jobj[3];
  args_jobj[0].l = rcvr_jobj;
  args_jobj[1].l = proxy_method_jobj;
  // Args array, if no arguments then NULL (don't include receiver in argument count)
  args_jobj[2].l = NULL;
  ObjectArray<Object>* args = NULL;
  if ((num_params - 1) > 0) {
    args = Runtime::Current()->GetClassLinker()->AllocObjectArray<Object>(num_params - 1);
    if (args == NULL) {
      CHECK(self->IsExceptionPending());
      return;
    }
    args_jobj[2].l = soa.AddLocalReference<jobjectArray>(args);
  }
  // Convert proxy method into expected interface method
  Method* interface_method = proxy_method->FindOverriddenMethod();
  DCHECK(interface_method != NULL);
  DCHECK(!interface_method->IsProxyMethod()) << PrettyMethod(interface_method);
  args_jobj[1].l = soa.AddLocalReference<jobject>(interface_method);
  // Box arguments
  cur_arg = 0;  // reset stack location to read to start
  // reset index, will index into param type array which doesn't include the receiver
  param_index = 0;
  ObjectArray<Class>* param_types = proxy_mh.GetParameterTypes();
  if (param_types == NULL) {
    CHECK(self->IsExceptionPending());
    return;
  }
  // Check number of parameter types agrees with number from the Method - less 1 for the receiver.
  DCHECK_EQ(static_cast<size_t>(param_types->GetLength()), num_params - 1);
  while (cur_arg < args_in_regs && param_index < (num_params - 1)) {
    Class* param_type = param_types->Get(param_index);
    Object* obj;
    if (!param_type->IsPrimitive()) {
      obj = self->DecodeJObject(*reinterpret_cast<jobject*>(stack_args + (cur_arg * kPointerSize)));
    } else {
      JValue val = *reinterpret_cast<JValue*>(stack_args + (cur_arg * kPointerSize));
      if (cur_arg == 1 && (param_type->IsPrimitiveLong() || param_type->IsPrimitiveDouble())) {
        // long/double split over regs and stack, mask in high half from stack arguments
        uint64_t high_half = *reinterpret_cast<uint32_t*>(stack_args + ((ARG2_OFFSET_IN_WORDS + 2) * kPointerSize));
        val.SetJ((val.GetJ() & 0xffffffffULL) | (high_half << 32));
      }
      BoxPrimitive(param_type->GetPrimitiveType(), val);
      if (self->IsExceptionPending()) {
        return;
      }
      obj = val.GetL();
    }
    args->Set(param_index, obj);
    cur_arg = cur_arg + (param_type->IsPrimitiveLong() || param_type->IsPrimitiveDouble() ? 2 : 1);
    param_index++;
  }
  // Placing into local references incoming arguments from the caller's stack arguments
  cur_arg += ARG2_OFFSET_IN_WORDS;
  while (param_index < (num_params - 1)) {
    Class* param_type = param_types->Get(param_index);
    Object* obj;
    if (!param_type->IsPrimitive()) {
      obj = self->DecodeJObject(*reinterpret_cast<jobject*>(stack_args + (cur_arg * kPointerSize)));
    } else {
      JValue val = *reinterpret_cast<JValue*>(stack_args + (cur_arg * kPointerSize));
      BoxPrimitive(param_type->GetPrimitiveType(), val);
      if (self->IsExceptionPending()) {
        return;
      }
      obj = val.GetL();
    }
    args->Set(param_index, obj);
    cur_arg = cur_arg + (param_type->IsPrimitiveLong() || param_type->IsPrimitiveDouble() ? 2 : 1);
    param_index++;
  }
  // Get the InvocationHandler method and the field that holds it within the Proxy object
  DCHECK(env->IsInstanceOf(rcvr_jobj, WellKnownClasses::java_lang_reflect_Proxy));
  jobject inv_hand = env->GetObjectField(rcvr_jobj, WellKnownClasses::java_lang_reflect_Proxy_h);
  // Call InvocationHandler.invoke
  jobject result = env->CallObjectMethodA(inv_hand, WellKnownClasses::java_lang_reflect_InvocationHandler_invoke, args_jobj);
  // Place result in stack args
  if (!self->IsExceptionPending()) {
    Object* result_ref = self->DecodeJObject(result);
    if (result_ref != NULL) {
      JValue result_unboxed;
      bool unboxed_okay = UnboxPrimitiveForResult(result_ref, proxy_mh.GetReturnType(), result_unboxed);
      if (!unboxed_okay) {
        self->ClearException();
        self->ThrowNewExceptionF("Ljava/lang/ClassCastException;",
                                 "Couldn't convert result of type %s to %s",
                                 PrettyTypeOf(result_ref).c_str(),
                                 PrettyDescriptor(proxy_mh.GetReturnType()).c_str());
        return;
      }
      *reinterpret_cast<JValue*>(stack_args) = result_unboxed;
    } else {
      *reinterpret_cast<jobject*>(stack_args) = NULL;
    }
  } else {
    // In the case of checked exceptions that aren't declared, the exception must be wrapped by
    // a UndeclaredThrowableException.
    Throwable* exception = self->GetException();
    if (exception->IsCheckedException()) {
      SynthesizedProxyClass* proxy_class =
          down_cast<SynthesizedProxyClass*>(proxy_method->GetDeclaringClass());
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
        self->ThrowNewWrappedException("Ljava/lang/reflect/UndeclaredThrowableException;", NULL);
      }
    }
  }
}

}  // namespace art
