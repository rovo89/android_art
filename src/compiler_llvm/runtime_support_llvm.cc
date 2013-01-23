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

#include "runtime_support_llvm.h"

#include "ScopedLocalRef.h"
#include "asm_support.h"
#include "class_linker.h"
#include "compiler_runtime_func_list.h"
#include "dex_file.h"
#include "dex_instruction.h"
#include "nth_caller_visitor.h"
#include "object.h"
#include "object_utils.h"
#include "reflection.h"
#include "runtime_support.h"
#include "runtime_support_func_list.h"
#include "scoped_thread_state_change.h"
#include "thread.h"
#include "thread_list.h"
#include "utils_llvm.h"
#include "verifier/method_verifier.h"
#include "well_known_classes.h"

#include <algorithm>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>

namespace art {

//----------------------------------------------------------------------------
// Thread
//----------------------------------------------------------------------------

// This is used by other runtime support functions, NOT FROM CODE. The REAL GetCurrentThread is
// implemented by IRBuilder. (So, ARM can't return R9 in this function.)
// TODO: Maybe remove these which are implemented by IRBuilder after refactor runtime support.
Thread* art_get_current_thread_from_code() {
#if defined(__i386__)
  Thread* ptr;
  __asm__ __volatile__("movl %%fs:(%1), %0"
      : "=r"(ptr)  // output
      : "r"(THREAD_SELF_OFFSET)  // input
      :);  // clobber
  return ptr;
#else
  return Thread::Current();
#endif
}

void* art_set_current_thread_from_code(void* thread_object_addr) {
  // Nothing to be done.
  return NULL;
}

void art_lock_object_from_code(Object* obj, Thread* thread)
    EXCLUSIVE_LOCK_FUNCTION(monitor_lock_) {
  DCHECK(obj != NULL);        // Assumed to have been checked before entry
  obj->MonitorEnter(thread);  // May block
  DCHECK(thread->HoldsLock(obj));
  // Only possible exception is NPE and is handled before entry
  DCHECK(!thread->IsExceptionPending());
}

void art_unlock_object_from_code(Object* obj, Thread* thread)
    UNLOCK_FUNCTION(monitor_lock_) {
  DCHECK(obj != NULL);  // Assumed to have been checked before entry
  // MonitorExit may throw exception
  obj->MonitorExit(thread);
}

void art_test_suspend_from_code(Thread* thread)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  CheckSuspend(thread);
}

ShadowFrame* art_push_shadow_frame_from_code(Thread* thread, ShadowFrame* new_shadow_frame,
                                             AbstractMethod* method, uint32_t num_vregs) {
  ShadowFrame* old_frame = thread->PushShadowFrame(new_shadow_frame);
  new_shadow_frame->SetMethod(method);
  new_shadow_frame->SetNumberOfVRegs(num_vregs);
  return old_frame;
}

void art_pop_shadow_frame_from_code(void*) {
  LOG(FATAL) << "Implemented by IRBuilder.";
}

void art_mark_gc_card_from_code(void *, void*) {
  LOG(FATAL) << "Implemented by IRBuilder.";
}

//----------------------------------------------------------------------------
// Exception
//----------------------------------------------------------------------------

bool art_is_exception_pending_from_code() {
  LOG(FATAL) << "Implemented by IRBuilder.";
  return false;
}

void art_throw_div_zero_from_code()
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Thread* thread = art_get_current_thread_from_code();
  thread->ThrowNewException("Ljava/lang/ArithmeticException;",
                            "divide by zero");
}

void art_throw_array_bounds_from_code(int32_t index, int32_t length)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Thread* thread = art_get_current_thread_from_code();
  thread->ThrowNewExceptionF("Ljava/lang/ArrayIndexOutOfBoundsException;",
                             "length=%d; index=%d", length, index);
}

void art_throw_no_such_method_from_code(int32_t method_idx)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Thread* thread = art_get_current_thread_from_code();
  // We need the calling method as context for the method_idx
  AbstractMethod* method = thread->GetCurrentMethod();
  ThrowNoSuchMethodError(method_idx, method);
}

void art_throw_null_pointer_exception_from_code(uint32_t dex_pc)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Thread* thread = art_get_current_thread_from_code();
  NthCallerVisitor visitor(thread->GetManagedStack(), thread->GetInstrumentationStack(), 0);
  visitor.WalkStack();
  AbstractMethod* throw_method = visitor.caller;
  ThrowNullPointerExceptionFromDexPC(throw_method, dex_pc);
}

void art_throw_stack_overflow_from_code()
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Thread* thread = art_get_current_thread_from_code();
  ThrowStackOverflowError(thread);
}

void art_throw_exception_from_code(Object* exception)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Thread* thread = art_get_current_thread_from_code();
  thread->DeliverException(static_cast<Throwable*>(exception));
}

void* art_get_and_clear_exception(Thread* self)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DCHECK(self->IsExceptionPending());
  Throwable* exception = self->GetException();
  self->ClearException();
  return exception;
}

int32_t art_find_catch_block_from_code(AbstractMethod* current_method,
                                       uint32_t ti_offset)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Thread* thread = art_get_current_thread_from_code();
  Class* exception_type = thread->GetException()->GetClass();
  MethodHelper mh(current_method);
  const DexFile::CodeItem* code_item = mh.GetCodeItem();
  DCHECK_LT(ti_offset, code_item->tries_size_);
  const DexFile::TryItem* try_item = DexFile::GetTryItems(*code_item, ti_offset);

  int iter_index = 0;
  // Iterate over the catch handlers associated with dex_pc
  for (CatchHandlerIterator it(*code_item, *try_item); it.HasNext(); it.Next()) {
    uint16_t iter_type_idx = it.GetHandlerTypeIndex();
    // Catch all case
    if (iter_type_idx == DexFile::kDexNoIndex16) {
      return iter_index;
    }
    // Does this catch exception type apply?
    Class* iter_exception_type = mh.GetDexCacheResolvedType(iter_type_idx);
    if (iter_exception_type == NULL) {
      // The verifier should take care of resolving all exception classes early
      LOG(WARNING) << "Unresolved exception class when finding catch block: "
          << mh.GetTypeDescriptorFromTypeIdx(iter_type_idx);
    } else if (iter_exception_type->IsAssignableFrom(exception_type)) {
      return iter_index;
    }
    ++iter_index;
  }
  // Handler not found
  return -1;
}


//----------------------------------------------------------------------------
// Object Space
//----------------------------------------------------------------------------

Object* art_alloc_object_from_code(uint32_t type_idx,
                                   AbstractMethod* referrer,
                                   Thread* thread)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return AllocObjectFromCode(type_idx, referrer, thread, false);
}

Object* art_alloc_object_from_code_with_access_check(uint32_t type_idx,
                                                     AbstractMethod* referrer,
                                                     Thread* thread)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return AllocObjectFromCode(type_idx, referrer, thread, true);
}

Object* art_alloc_array_from_code(uint32_t type_idx,
                                  AbstractMethod* referrer,
                                  uint32_t length,
                                  Thread* self)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return AllocArrayFromCode(type_idx, referrer, length, self, false);
}

Object* art_alloc_array_from_code_with_access_check(uint32_t type_idx,
                                                    AbstractMethod* referrer,
                                                    uint32_t length,
                                                    Thread* self)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return AllocArrayFromCode(type_idx, referrer, length, self, true);
}

Object* art_check_and_alloc_array_from_code(uint32_t type_idx,
                                            AbstractMethod* referrer,
                                            uint32_t length,
                                            Thread* thread)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return CheckAndAllocArrayFromCode(type_idx, referrer, length, thread, false);
}

Object* art_check_and_alloc_array_from_code_with_access_check(uint32_t type_idx,
                                                              AbstractMethod* referrer,
                                                              uint32_t length,
                                                              Thread* thread)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return CheckAndAllocArrayFromCode(type_idx, referrer, length, thread, true);
}

static AbstractMethod* FindMethodHelper(uint32_t method_idx, Object* this_object,
                                        AbstractMethod* caller_method, bool access_check,
                                        InvokeType type, Thread* thread)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  AbstractMethod* method = FindMethodFast(method_idx, this_object, caller_method, access_check, type);
  if (UNLIKELY(method == NULL)) {
    method = FindMethodFromCode(method_idx, this_object, caller_method,
                                thread, access_check, type);
    if (UNLIKELY(method == NULL)) {
      CHECK(thread->IsExceptionPending());
      return 0;  // failure
    }
  }
  DCHECK(!thread->IsExceptionPending());
  const void* code = method->GetCode();

  // When we return, the caller will branch to this address, so it had better not be 0!
  if (UNLIKELY(code == NULL)) {
      MethodHelper mh(method);
      LOG(FATAL) << "Code was NULL in method: " << PrettyMethod(method)
                 << " location: " << mh.GetDexFile().GetLocation();
  }
  return method;
}

Object* art_find_static_method_from_code_with_access_check(uint32_t method_idx,
                                                           Object* this_object,
                                                           AbstractMethod* referrer,
                                                           Thread* thread)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return FindMethodHelper(method_idx, this_object, referrer, true, kStatic, thread);
}

Object* art_find_direct_method_from_code_with_access_check(uint32_t method_idx,
                                                           Object* this_object,
                                                           AbstractMethod* referrer,
                                                           Thread* thread)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return FindMethodHelper(method_idx, this_object, referrer, true, kDirect, thread);
}

Object* art_find_virtual_method_from_code_with_access_check(uint32_t method_idx,
                                                            Object* this_object,
                                                            AbstractMethod* referrer,
                                                            Thread* thread)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return FindMethodHelper(method_idx, this_object, referrer, true, kVirtual, thread);
}

Object* art_find_super_method_from_code_with_access_check(uint32_t method_idx,
                                                          Object* this_object,
                                                          AbstractMethod* referrer,
                                                          Thread* thread)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return FindMethodHelper(method_idx, this_object, referrer, true, kSuper, thread);
}

Object*
art_find_interface_method_from_code_with_access_check(uint32_t method_idx,
                                                      Object* this_object,
                                                      AbstractMethod* referrer,
                                                      Thread* thread)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return FindMethodHelper(method_idx, this_object, referrer, true, kInterface, thread);
}

Object* art_find_interface_method_from_code(uint32_t method_idx,
                                            Object* this_object,
                                            AbstractMethod* referrer,
                                            Thread* thread)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return FindMethodHelper(method_idx, this_object, referrer, false, kInterface, thread);
}

Object* art_initialize_static_storage_from_code(uint32_t type_idx,
                                                AbstractMethod* referrer,
                                                Thread* thread)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return ResolveVerifyAndClinit(type_idx, referrer, thread, true, false);
}

Object* art_initialize_type_from_code(uint32_t type_idx,
                                      AbstractMethod* referrer,
                                      Thread* thread)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return ResolveVerifyAndClinit(type_idx, referrer, thread, false, false);
}

Object* art_initialize_type_and_verify_access_from_code(uint32_t type_idx,
                                                        AbstractMethod* referrer,
                                                        Thread* thread)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  // Called when caller isn't guaranteed to have access to a type and the dex cache may be
  // unpopulated
  return ResolveVerifyAndClinit(type_idx, referrer, thread, false, true);
}

Object* art_resolve_string_from_code(AbstractMethod* referrer, uint32_t string_idx)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return ResolveStringFromCode(referrer, string_idx);
}

int32_t art_set32_static_from_code(uint32_t field_idx, AbstractMethod* referrer, int32_t new_value)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Field* field = FindFieldFast(field_idx, referrer, StaticPrimitiveWrite, sizeof(uint32_t));
  if (LIKELY(field != NULL)) {
    field->Set32(field->GetDeclaringClass(), new_value);
    return 0;
  }
  field = FindFieldFromCode(field_idx, referrer, art_get_current_thread_from_code(),
                            StaticPrimitiveWrite, sizeof(uint32_t));
  if (LIKELY(field != NULL)) {
    field->Set32(field->GetDeclaringClass(), new_value);
    return 0;
  }
  return -1;
}

int32_t art_set64_static_from_code(uint32_t field_idx, AbstractMethod* referrer, int64_t new_value)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Field* field = FindFieldFast(field_idx, referrer, StaticPrimitiveWrite, sizeof(uint64_t));
  if (LIKELY(field != NULL)) {
    field->Set64(field->GetDeclaringClass(), new_value);
    return 0;
  }
  field = FindFieldFromCode(field_idx, referrer, art_get_current_thread_from_code(),
                            StaticPrimitiveWrite, sizeof(uint64_t));
  if (LIKELY(field != NULL)) {
    field->Set64(field->GetDeclaringClass(), new_value);
    return 0;
  }
  return -1;
}

int32_t art_set_obj_static_from_code(uint32_t field_idx, AbstractMethod* referrer, Object* new_value)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Field* field = FindFieldFast(field_idx, referrer, StaticObjectWrite, sizeof(Object*));
  if (LIKELY(field != NULL)) {
    field->SetObj(field->GetDeclaringClass(), new_value);
    return 0;
  }
  field = FindFieldFromCode(field_idx, referrer, art_get_current_thread_from_code(),
                            StaticObjectWrite, sizeof(Object*));
  if (LIKELY(field != NULL)) {
    field->SetObj(field->GetDeclaringClass(), new_value);
    return 0;
  }
  return -1;
}

int32_t art_get32_static_from_code(uint32_t field_idx, AbstractMethod* referrer)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Field* field = FindFieldFast(field_idx, referrer, StaticPrimitiveRead, sizeof(uint32_t));
  if (LIKELY(field != NULL)) {
    return field->Get32(field->GetDeclaringClass());
  }
  field = FindFieldFromCode(field_idx, referrer, art_get_current_thread_from_code(),
                            StaticPrimitiveRead, sizeof(uint32_t));
  if (LIKELY(field != NULL)) {
    return field->Get32(field->GetDeclaringClass());
  }
  return 0;
}

int64_t art_get64_static_from_code(uint32_t field_idx, AbstractMethod* referrer)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Field* field = FindFieldFast(field_idx, referrer, StaticPrimitiveRead, sizeof(uint64_t));
  if (LIKELY(field != NULL)) {
    return field->Get64(field->GetDeclaringClass());
  }
  field = FindFieldFromCode(field_idx, referrer, art_get_current_thread_from_code(),
                            StaticPrimitiveRead, sizeof(uint64_t));
  if (LIKELY(field != NULL)) {
    return field->Get64(field->GetDeclaringClass());
  }
  return 0;
}

Object* art_get_obj_static_from_code(uint32_t field_idx, AbstractMethod* referrer)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Field* field = FindFieldFast(field_idx, referrer, StaticObjectRead, sizeof(Object*));
  if (LIKELY(field != NULL)) {
    return field->GetObj(field->GetDeclaringClass());
  }
  field = FindFieldFromCode(field_idx, referrer, art_get_current_thread_from_code(),
                            StaticObjectRead, sizeof(Object*));
  if (LIKELY(field != NULL)) {
    return field->GetObj(field->GetDeclaringClass());
  }
  return 0;
}

int32_t art_set32_instance_from_code(uint32_t field_idx, AbstractMethod* referrer,
                                     Object* obj, uint32_t new_value)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Field* field = FindFieldFast(field_idx, referrer, InstancePrimitiveWrite, sizeof(uint32_t));
  if (LIKELY(field != NULL)) {
    field->Set32(obj, new_value);
    return 0;
  }
  field = FindFieldFromCode(field_idx, referrer, art_get_current_thread_from_code(),
                            InstancePrimitiveWrite, sizeof(uint32_t));
  if (LIKELY(field != NULL)) {
    field->Set32(obj, new_value);
    return 0;
  }
  return -1;
}

int32_t art_set64_instance_from_code(uint32_t field_idx, AbstractMethod* referrer,
                                     Object* obj, int64_t new_value)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Field* field = FindFieldFast(field_idx, referrer, InstancePrimitiveWrite, sizeof(uint64_t));
  if (LIKELY(field != NULL)) {
    field->Set64(obj, new_value);
    return 0;
  }
  field = FindFieldFromCode(field_idx, referrer, art_get_current_thread_from_code(),
                            InstancePrimitiveWrite, sizeof(uint64_t));
  if (LIKELY(field != NULL)) {
    field->Set64(obj, new_value);
    return 0;
  }
  return -1;
}

int32_t art_set_obj_instance_from_code(uint32_t field_idx, AbstractMethod* referrer,
                                       Object* obj, Object* new_value)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Field* field = FindFieldFast(field_idx, referrer, InstanceObjectWrite, sizeof(Object*));
  if (LIKELY(field != NULL)) {
    field->SetObj(obj, new_value);
    return 0;
  }
  field = FindFieldFromCode(field_idx, referrer, art_get_current_thread_from_code(),
                            InstanceObjectWrite, sizeof(Object*));
  if (LIKELY(field != NULL)) {
    field->SetObj(obj, new_value);
    return 0;
  }
  return -1;
}

int32_t art_get32_instance_from_code(uint32_t field_idx, AbstractMethod* referrer, Object* obj)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Field* field = FindFieldFast(field_idx, referrer, InstancePrimitiveRead, sizeof(uint32_t));
  if (LIKELY(field != NULL)) {
    return field->Get32(obj);
  }
  field = FindFieldFromCode(field_idx, referrer, art_get_current_thread_from_code(),
                            InstancePrimitiveRead, sizeof(uint32_t));
  if (LIKELY(field != NULL)) {
    return field->Get32(obj);
  }
  return 0;
}

int64_t art_get64_instance_from_code(uint32_t field_idx, AbstractMethod* referrer, Object* obj)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Field* field = FindFieldFast(field_idx, referrer, InstancePrimitiveRead, sizeof(uint64_t));
  if (LIKELY(field != NULL)) {
    return field->Get64(obj);
  }
  field = FindFieldFromCode(field_idx, referrer, art_get_current_thread_from_code(),
                            InstancePrimitiveRead, sizeof(uint64_t));
  if (LIKELY(field != NULL)) {
    return field->Get64(obj);
  }
  return 0;
}

Object* art_get_obj_instance_from_code(uint32_t field_idx, AbstractMethod* referrer, Object* obj)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Field* field = FindFieldFast(field_idx, referrer, InstanceObjectRead, sizeof(Object*));
  if (LIKELY(field != NULL)) {
    return field->GetObj(obj);
  }
  field = FindFieldFromCode(field_idx, referrer, art_get_current_thread_from_code(),
                            InstanceObjectRead, sizeof(Object*));
  if (LIKELY(field != NULL)) {
    return field->GetObj(obj);
  }
  return 0;
}

void art_fill_array_data_from_code(AbstractMethod* method, uint32_t dex_pc,
                                   Array* array, uint32_t payload_offset)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  // Test: Is array equal to null? (Guard NullPointerException)
  if (UNLIKELY(array == NULL)) {
    art_throw_null_pointer_exception_from_code(dex_pc);
    return;
  }

  // Find the payload from the CodeItem
  MethodHelper mh(method);
  const DexFile::CodeItem* code_item = mh.GetCodeItem();

  DCHECK_GT(code_item->insns_size_in_code_units_, payload_offset);

  const Instruction::ArrayDataPayload* payload =
    reinterpret_cast<const Instruction::ArrayDataPayload*>(
        code_item->insns_ + payload_offset);

  DCHECK_EQ(payload->ident,
            static_cast<uint16_t>(Instruction::kArrayDataSignature));

  // Test: Is array big enough?
  uint32_t array_len = static_cast<uint32_t>(array->GetLength());
  if (UNLIKELY(array_len < payload->element_count)) {
    int32_t last_index = payload->element_count - 1;
    art_throw_array_bounds_from_code(array_len, last_index);
    return;
  }

  // Copy the data
  size_t size = payload->element_width * payload->element_count;
  memcpy(array->GetRawData(payload->element_width), payload->data, size);
}



//----------------------------------------------------------------------------
// Type checking, in the nature of casting
//----------------------------------------------------------------------------

int32_t art_is_assignable_from_code(const Class* dest_type, const Class* src_type)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DCHECK(dest_type != NULL);
  DCHECK(src_type != NULL);
  return dest_type->IsAssignableFrom(src_type) ? 1 : 0;
}

void art_check_cast_from_code(const Class* dest_type, const Class* src_type)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DCHECK(dest_type->IsClass()) << PrettyClass(dest_type);
  DCHECK(src_type->IsClass()) << PrettyClass(src_type);
  if (UNLIKELY(!dest_type->IsAssignableFrom(src_type))) {
    Thread* thread = art_get_current_thread_from_code();
    thread->ThrowNewExceptionF("Ljava/lang/ClassCastException;",
                               "%s cannot be cast to %s",
                               PrettyDescriptor(src_type).c_str(),
                               PrettyDescriptor(dest_type).c_str());
  }
}

void art_check_put_array_element_from_code(const Object* element, const Object* array)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (element == NULL) {
    return;
  }
  DCHECK(array != NULL);
  Class* array_class = array->GetClass();
  DCHECK(array_class != NULL);
  Class* component_type = array_class->GetComponentType();
  Class* element_class = element->GetClass();
  if (UNLIKELY(!component_type->IsAssignableFrom(element_class))) {
    Thread* thread = art_get_current_thread_from_code();
    thread->ThrowNewExceptionF("Ljava/lang/ArrayStoreException;",
                               "%s cannot be stored in an array of type %s",
                               PrettyDescriptor(element_class).c_str(),
                               PrettyDescriptor(array_class).c_str());
  }
  return;
}

//----------------------------------------------------------------------------
// JNI
//----------------------------------------------------------------------------

// Called on entry to JNI, transition out of Runnable and release share of mutator_lock_.
uint32_t art_jni_method_start(Thread* self)
    UNLOCK_FUNCTION(GlobalSynchronizatio::mutator_lock_) {
  JNIEnvExt* env = self->GetJniEnv();
  uint32_t saved_local_ref_cookie = env->local_ref_cookie;
  env->local_ref_cookie = env->locals.GetSegmentState();
  self->TransitionFromRunnableToSuspended(kNative);
  return saved_local_ref_cookie;
}

uint32_t art_jni_method_start_synchronized(jobject to_lock, Thread* self)
    UNLOCK_FUNCTION(Locks::mutator_lock_) {
  self->DecodeJObject(to_lock)->MonitorEnter(self);
  return art_jni_method_start(self);
}

static inline void PopLocalReferences(uint32_t saved_local_ref_cookie, Thread* self) {
  JNIEnvExt* env = self->GetJniEnv();
  env->locals.SetSegmentState(env->local_ref_cookie);
  env->local_ref_cookie = saved_local_ref_cookie;
}

void art_jni_method_end(uint32_t saved_local_ref_cookie, Thread* self)
    SHARED_LOCK_FUNCTION(Locks::mutator_lock_) {
  self->TransitionFromSuspendedToRunnable();
  PopLocalReferences(saved_local_ref_cookie, self);
}


void art_jni_method_end_synchronized(uint32_t saved_local_ref_cookie, jobject locked,
                                     Thread* self)
    SHARED_LOCK_FUNCTION(Locks::mutator_lock_) {
  self->TransitionFromSuspendedToRunnable();
  UnlockJniSynchronizedMethod(locked, self);  // Must decode before pop.
  PopLocalReferences(saved_local_ref_cookie, self);
}

Object* art_jni_method_end_with_reference(jobject result, uint32_t saved_local_ref_cookie,
                                          Thread* self)
    SHARED_LOCK_FUNCTION(Locks::mutator_lock_) {
  self->TransitionFromSuspendedToRunnable();
  Object* o = self->DecodeJObject(result);  // Must decode before pop.
  PopLocalReferences(saved_local_ref_cookie, self);
  // Process result.
  if (UNLIKELY(self->GetJniEnv()->check_jni)) {
    if (self->IsExceptionPending()) {
      return NULL;
    }
    CheckReferenceResult(o, self);
  }
  return o;
}

Object* art_jni_method_end_with_reference_synchronized(jobject result,
                                                       uint32_t saved_local_ref_cookie,
                                                       jobject locked, Thread* self)
    SHARED_LOCK_FUNCTION(Locks::mutator_lock_) {
  self->TransitionFromSuspendedToRunnable();
  UnlockJniSynchronizedMethod(locked, self);  // Must decode before pop.
  Object* o = self->DecodeJObject(result);
  PopLocalReferences(saved_local_ref_cookie, self);
  // Process result.
  if (UNLIKELY(self->GetJniEnv()->check_jni)) {
    if (self->IsExceptionPending()) {
      return NULL;
    }
    CheckReferenceResult(o, self);
  }
  return o;
}

//----------------------------------------------------------------------------
// Runtime Support Function Lookup Callback
//----------------------------------------------------------------------------

#define EXTERNAL_LINKAGE(NAME, RETURN_TYPE, ...) \
extern "C" RETURN_TYPE NAME(__VA_ARGS__);
COMPILER_RUNTIME_FUNC_LIST_NATIVE(EXTERNAL_LINKAGE)
#undef EXTERNAL_LINKAGE

static void* art_find_compiler_runtime_func(const char* name) {
// TODO: If target support some math func, use the target's version. (e.g. art_d2i -> __aeabi_d2iz)
  static const char* const names[] = {
#define DEFINE_ENTRY(NAME, RETURN_TYPE, ...) #NAME ,
    COMPILER_RUNTIME_FUNC_LIST_NATIVE(DEFINE_ENTRY)
#undef DEFINE_ENTRY
  };

  static void* const funcs[] = {
#define DEFINE_ENTRY(NAME, RETURN_TYPE, ...) \
    reinterpret_cast<void*>(static_cast<RETURN_TYPE (*)(__VA_ARGS__)>(NAME)) ,
    COMPILER_RUNTIME_FUNC_LIST_NATIVE(DEFINE_ENTRY)
#undef DEFINE_ENTRY
  };

  static const size_t num_entries = sizeof(names) / sizeof(const char* const);

  const char* const* const names_begin = names;
  const char* const* const names_end = names + num_entries;

  const char* const* name_lbound_ptr =
      std::lower_bound(names_begin, names_end, name,
                       CStringLessThanComparator());

  if (name_lbound_ptr < names_end && strcmp(*name_lbound_ptr, name) == 0) {
    return funcs[name_lbound_ptr - names_begin];
  } else {
    return NULL;
  }
}

// Handler for invocation on proxy methods. Create a boxed argument array and invoke the invocation
// handler which is a field within the proxy object receiver. The var args encode the arguments
// with the last argument being a pointer to a JValue to store the result in.
void art_proxy_invoke_handler_from_code(AbstractMethod* proxy_method, ...)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  va_list ap;
  va_start(ap, proxy_method);

  Object* receiver = va_arg(ap, Object*);
  Thread* self = va_arg(ap, Thread*);
  MethodHelper proxy_mh(proxy_method);

  // Ensure we don't get thread suspension until the object arguments are safely in jobjects.
  const char* old_cause =
      self->StartAssertNoThreadSuspension("Adding to IRT proxy object arguments");
  self->VerifyStack();

  // Start new JNI local reference state.
  JNIEnvExt* env = self->GetJniEnv();
  ScopedObjectAccessUnchecked soa(env);
  ScopedJniEnvLocalRefState env_state(env);

  // Create local ref. copies of the receiver.
  jobject rcvr_jobj = soa.AddLocalReference<jobject>(receiver);

  // Convert proxy method into expected interface method.
  AbstractMethod* interface_method = proxy_method->FindOverriddenMethod();
  DCHECK(interface_method != NULL);
  DCHECK(!interface_method->IsProxyMethod()) << PrettyMethod(interface_method);
  jobject interface_method_jobj = soa.AddLocalReference<jobject>(interface_method);

  // Record arguments and turn Object* arguments into jobject to survive GC.
  std::vector<jvalue> args;
  const size_t num_params = proxy_mh.NumArgs();
  for (size_t i = 1; i < num_params; ++i) {
    jvalue val;
    switch (proxy_mh.GetParamPrimitiveType(i)) {
      case Primitive::kPrimNot:
        val.l = soa.AddLocalReference<jobject>(va_arg(ap, Object*));
        break;
      case Primitive::kPrimBoolean:  // Fall-through.
      case Primitive::kPrimByte:     // Fall-through.
      case Primitive::kPrimChar:     // Fall-through.
      case Primitive::kPrimShort:    // Fall-through.
      case Primitive::kPrimInt:      // Fall-through.
        val.i = va_arg(ap, jint);
        break;
      case Primitive::kPrimFloat:
        // TODO: should this be jdouble? Floats aren't passed to var arg routines.
        val.i = va_arg(ap, jint);
        break;
      case Primitive::kPrimDouble:
        val.d = (va_arg(ap, jdouble));
        break;
      case Primitive::kPrimLong:
        val.j = (va_arg(ap, jlong));
        break;
      case Primitive::kPrimVoid:
        LOG(FATAL) << "UNREACHABLE";
        val.j = 0;
        break;
    }
    args.push_back(val);
  }
  self->EndAssertNoThreadSuspension(old_cause);
  JValue* result_location = NULL;
  const char* shorty = proxy_mh.GetShorty();
  if (shorty[0] != 'V') {
    result_location = va_arg(ap, JValue*);
  }
  va_end(ap);
  JValue result = InvokeProxyInvocationHandler(soa, shorty, rcvr_jobj, interface_method_jobj, args);
  if (result_location != NULL) {
    *result_location = result;
  }
}

void* art_find_runtime_support_func(void* context, const char* name) {
  struct func_entry_t {
    const char* name;
    size_t name_len;
    void* addr;
  };

  static struct func_entry_t const tab[] = {
#define DEFINE_ENTRY(ID, NAME) \
    { #NAME, sizeof(#NAME) - 1, reinterpret_cast<void*>(NAME) },
    RUNTIME_SUPPORT_FUNC_LIST(DEFINE_ENTRY)
#undef DEFINE_ENTRY
  };

  static size_t const tab_size = sizeof(tab) / sizeof(struct func_entry_t);

  // Search the compiler runtime (such as __divdi3)
  void* result = art_find_compiler_runtime_func(name);
  if (result != NULL) {
    return result;
  }

  // Note: Since our table is small, we are using trivial O(n) searching
  // function.  For bigger table, it will be better to use a binary
  // search or hash function.
  size_t i;
  size_t name_len = strlen(name);
  for (i = 0; i < tab_size; ++i) {
    if (name_len == tab[i].name_len && strcmp(name, tab[i].name) == 0) {
      return tab[i].addr;
    }
  }

  LOG(FATAL) << "Error: Can't find symbol " << name;
  return 0;
}

}  // namespace art
