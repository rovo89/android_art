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
#include "class_linker-inl.h"
#include "dex_file-inl.h"
#include "dex_instruction.h"
#include "mirror/abstract_method-inl.h"
#include "mirror/class-inl.h"
#include "mirror/dex_cache-inl.h"
#include "mirror/field-inl.h"
#include "mirror/object.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "nth_caller_visitor.h"
#include "object_utils.h"
#include "reflection.h"
#include "runtime_support.h"
#include "runtime_support_func_list.h"
#include "scoped_thread_state_change.h"
#include "thread.h"
#include "thread_list.h"
#include "utils_llvm.h"
#include "verifier/dex_gc_map.h"
#include "verifier/method_verifier.h"
#include "well_known_classes.h"

#include <algorithm>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>

using namespace art;

extern "C" {

class ShadowFrameCopyVisitor : public StackVisitor {
 public:
  explicit ShadowFrameCopyVisitor(Thread* self) : StackVisitor(self, NULL), prev_frame_(NULL),
      top_frame_(NULL) {}

  bool VisitFrame() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    if (IsShadowFrame()) {
      ShadowFrame* cur_frame = GetCurrentShadowFrame();
      size_t num_regs = cur_frame->NumberOfVRegs();
      mirror::AbstractMethod* method = cur_frame->GetMethod();
      uint32_t dex_pc = cur_frame->GetDexPC();
      ShadowFrame* new_frame = ShadowFrame::Create(num_regs, NULL, method, dex_pc);

      const uint8_t* gc_map = method->GetNativeGcMap();
      uint32_t gc_map_length = static_cast<uint32_t>((gc_map[0] << 24) |
                                                     (gc_map[1] << 16) |
                                                     (gc_map[2] << 8) |
                                                     (gc_map[3] << 0));
      verifier::DexPcToReferenceMap dex_gc_map(gc_map + 4, gc_map_length);
      const uint8_t* reg_bitmap = dex_gc_map.FindBitMap(dex_pc);
      for (size_t reg = 0; reg < num_regs; ++reg) {
        if (TestBitmap(reg, reg_bitmap)) {
          new_frame->SetVRegReference(reg, cur_frame->GetVRegReference(reg));
        } else {
          new_frame->SetVReg(reg, cur_frame->GetVReg(reg));
        }
      }

      if (prev_frame_ != NULL) {
        prev_frame_->SetLink(new_frame);
      } else {
        top_frame_ = new_frame;
      }
      prev_frame_ = new_frame;
    }
    return true;
  }

  ShadowFrame* GetShadowFrameCopy() {
    return top_frame_;
  }

 private:
  static bool TestBitmap(int reg, const uint8_t* reg_vector) {
    return ((reg_vector[reg / 8] >> (reg % 8)) & 0x01) != 0;
  }

  ShadowFrame* prev_frame_;
  ShadowFrame* top_frame_;
};

//----------------------------------------------------------------------------
// Thread
//----------------------------------------------------------------------------

Thread* art_portable_get_current_thread_from_code() {
#if defined(__arm__) || defined(__i386__)
  LOG(FATAL) << "UNREACHABLE";
#endif
  return Thread::Current();
}

void* art_portable_set_current_thread_from_code(void* thread_object_addr) {
  // Hijacked to set r9 on ARM.
  LOG(FATAL) << "UNREACHABLE";
  return NULL;
}

void art_portable_lock_object_from_code(mirror::Object* obj, Thread* thread)
    EXCLUSIVE_LOCK_FUNCTION(monitor_lock_) {
  DCHECK(obj != NULL);        // Assumed to have been checked before entry
  obj->MonitorEnter(thread);  // May block
  DCHECK(thread->HoldsLock(obj));
  // Only possible exception is NPE and is handled before entry
  DCHECK(!thread->IsExceptionPending());
}

void art_portable_unlock_object_from_code(mirror::Object* obj, Thread* thread)
    UNLOCK_FUNCTION(monitor_lock_) {
  DCHECK(obj != NULL);  // Assumed to have been checked before entry
  // MonitorExit may throw exception
  obj->MonitorExit(thread);
}

void art_portable_test_suspend_from_code(Thread* self)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  CheckSuspend(self);
  if (Runtime::Current()->GetInstrumentation()->ShouldPortableCodeDeoptimize()) {
    // Save out the shadow frame to the heap
    ShadowFrameCopyVisitor visitor(self);
    visitor.WalkStack(true);
    self->SetDeoptimizationShadowFrame(visitor.GetShadowFrameCopy());
    self->SetDeoptimizationReturnValue(JValue());
    self->SetException(ThrowLocation(), reinterpret_cast<mirror::Throwable*>(-1));
  }
}

ShadowFrame* art_portable_push_shadow_frame_from_code(Thread* thread,
                                              ShadowFrame* new_shadow_frame,
                                              mirror::AbstractMethod* method,
                                              uint32_t num_vregs) {
  ShadowFrame* old_frame = thread->PushShadowFrame(new_shadow_frame);
  new_shadow_frame->SetMethod(method);
  new_shadow_frame->SetNumberOfVRegs(num_vregs);
  return old_frame;
}

void art_portable_pop_shadow_frame_from_code(void*) {
  LOG(FATAL) << "Implemented by IRBuilder.";
}

void art_portable_mark_gc_card_from_code(void *, void*) {
  LOG(FATAL) << "Implemented by IRBuilder.";
}

//----------------------------------------------------------------------------
// Exception
//----------------------------------------------------------------------------

bool art_portable_is_exception_pending_from_code() {
  LOG(FATAL) << "Implemented by IRBuilder.";
  return false;
}

void art_portable_throw_div_zero_from_code() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ThrowArithmeticExceptionDivideByZero();
}

void art_portable_throw_array_bounds_from_code(int32_t index, int32_t length)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ThrowArrayIndexOutOfBoundsException(index, length);
}

void art_portable_throw_no_such_method_from_code(int32_t method_idx)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ThrowNoSuchMethodError(method_idx);
}

void art_portable_throw_null_pointer_exception_from_code(uint32_t dex_pc)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  // TODO: remove dex_pc argument from caller.
  UNUSED(dex_pc);
  Thread* self = Thread::Current();
  ThrowLocation throw_location = self->GetCurrentLocationForThrow();
  ThrowNullPointerExceptionFromDexPC(throw_location);
}

void art_portable_throw_stack_overflow_from_code() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ThrowStackOverflowError(Thread::Current());
}

void art_portable_throw_exception_from_code(mirror::Throwable* exception)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Thread* self = Thread::Current();
  ThrowLocation throw_location = self->GetCurrentLocationForThrow();
  if (exception == NULL) {
    ThrowNullPointerException(NULL, "throw with null exception");
  } else {
    self->SetException(throw_location, exception);
  }
}

void* art_portable_get_and_clear_exception(Thread* self)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DCHECK(self->IsExceptionPending());
  // TODO: make this inline.
  mirror::Throwable* exception = self->GetException(NULL);
  self->ClearException();
  return exception;
}

int32_t art_portable_find_catch_block_from_code(mirror::AbstractMethod* current_method, uint32_t ti_offset)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Thread* self = Thread::Current();  // TODO: make an argument.
  ThrowLocation throw_location;
  mirror::Throwable* exception = self->GetException(&throw_location);
  // Check for special deoptimization exception.
  if (UNLIKELY(reinterpret_cast<int32_t>(exception) == -1)) {
    return -1;
  }
  mirror::Class* exception_type = exception->GetClass();
  MethodHelper mh(current_method);
  const DexFile::CodeItem* code_item = mh.GetCodeItem();
  DCHECK_LT(ti_offset, code_item->tries_size_);
  const DexFile::TryItem* try_item = DexFile::GetTryItems(*code_item, ti_offset);

  int iter_index = 0;
  int result = -1;
  uint32_t catch_dex_pc = -1;
  // Iterate over the catch handlers associated with dex_pc
  for (CatchHandlerIterator it(*code_item, *try_item); it.HasNext(); it.Next()) {
    uint16_t iter_type_idx = it.GetHandlerTypeIndex();
    // Catch all case
    if (iter_type_idx == DexFile::kDexNoIndex16) {
      catch_dex_pc = it.GetHandlerAddress();
      result = iter_index;
      break;
    }
    // Does this catch exception type apply?
    mirror::Class* iter_exception_type = mh.GetDexCacheResolvedType(iter_type_idx);
    if (UNLIKELY(iter_exception_type == NULL)) {
      // TODO: check, the verifier (class linker?) should take care of resolving all exception
      //       classes early.
      LOG(WARNING) << "Unresolved exception class when finding catch block: "
          << mh.GetTypeDescriptorFromTypeIdx(iter_type_idx);
    } else if (iter_exception_type->IsAssignableFrom(exception_type)) {
      catch_dex_pc = it.GetHandlerAddress();
      result = iter_index;
      break;
    }
    ++iter_index;
  }
  if (result != -1) {
    // Handler found.
    Runtime::Current()->GetInstrumentation()->ExceptionCaughtEvent(self,
                                                                   throw_location,
                                                                   current_method,
                                                                   catch_dex_pc,
                                                                   exception);
  }
  return result;
}


//----------------------------------------------------------------------------
// Object Space
//----------------------------------------------------------------------------

mirror::Object* art_portable_alloc_object_from_code(uint32_t type_idx,
                                            mirror::AbstractMethod* referrer,
                                            Thread* thread)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return AllocObjectFromCode(type_idx, referrer, thread, false);
}

mirror::Object* art_portable_alloc_object_from_code_with_access_check(uint32_t type_idx,
                                                              mirror::AbstractMethod* referrer,
                                                              Thread* thread)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return AllocObjectFromCode(type_idx, referrer, thread, true);
}

mirror::Object* art_portable_alloc_array_from_code(uint32_t type_idx,
                                           mirror::AbstractMethod* referrer,
                                           uint32_t length,
                                           Thread* self)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return AllocArrayFromCode(type_idx, referrer, length, self, false);
}

mirror::Object* art_portable_alloc_array_from_code_with_access_check(uint32_t type_idx,
                                                             mirror::AbstractMethod* referrer,
                                                             uint32_t length,
                                                             Thread* self)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return AllocArrayFromCode(type_idx, referrer, length, self, true);
}

mirror::Object* art_portable_check_and_alloc_array_from_code(uint32_t type_idx,
                                                     mirror::AbstractMethod* referrer,
                                                     uint32_t length,
                                                     Thread* thread)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return CheckAndAllocArrayFromCode(type_idx, referrer, length, thread, false);
}

mirror::Object* art_portable_check_and_alloc_array_from_code_with_access_check(uint32_t type_idx,
                                                                       mirror::AbstractMethod* referrer,
                                                                       uint32_t length,
                                                                       Thread* thread)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return CheckAndAllocArrayFromCode(type_idx, referrer, length, thread, true);
}

static mirror::AbstractMethod* FindMethodHelper(uint32_t method_idx, mirror::Object* this_object,
                                        mirror::AbstractMethod* caller_method, bool access_check,
                                        InvokeType type, Thread* thread)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::AbstractMethod* method = FindMethodFast(method_idx, this_object, caller_method, access_check, type);
  if (UNLIKELY(method == NULL)) {
    method = FindMethodFromCode(method_idx, this_object, caller_method,
                                thread, access_check, type);
    if (UNLIKELY(method == NULL)) {
      CHECK(thread->IsExceptionPending());
      return 0;  // failure
    }
  }
  DCHECK(!thread->IsExceptionPending());
  const void* code = method->GetEntryPointFromCompiledCode();

  // When we return, the caller will branch to this address, so it had better not be 0!
  if (UNLIKELY(code == NULL)) {
      MethodHelper mh(method);
      LOG(FATAL) << "Code was NULL in method: " << PrettyMethod(method)
                 << " location: " << mh.GetDexFile().GetLocation();
  }
  return method;
}

mirror::Object* art_portable_find_static_method_from_code_with_access_check(uint32_t method_idx,
                                                                    mirror::Object* this_object,
                                                                    mirror::AbstractMethod* referrer,
                                                                    Thread* thread)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return FindMethodHelper(method_idx, this_object, referrer, true, kStatic, thread);
}

mirror::Object* art_portable_find_direct_method_from_code_with_access_check(uint32_t method_idx,
                                                                    mirror::Object* this_object,
                                                                    mirror::AbstractMethod* referrer,
                                                                    Thread* thread)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return FindMethodHelper(method_idx, this_object, referrer, true, kDirect, thread);
}

mirror::Object* art_portable_find_virtual_method_from_code_with_access_check(uint32_t method_idx,
                                                                     mirror::Object* this_object,
                                                                     mirror::AbstractMethod* referrer,
                                                                     Thread* thread)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return FindMethodHelper(method_idx, this_object, referrer, true, kVirtual, thread);
}

mirror::Object* art_portable_find_super_method_from_code_with_access_check(uint32_t method_idx,
                                                                   mirror::Object* this_object,
                                                                   mirror::AbstractMethod* referrer,
                                                                   Thread* thread)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return FindMethodHelper(method_idx, this_object, referrer, true, kSuper, thread);
}

mirror::Object*
art_portable_find_interface_method_from_code_with_access_check(uint32_t method_idx,
                                                       mirror::Object* this_object,
                                                       mirror::AbstractMethod* referrer,
                                                       Thread* thread)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return FindMethodHelper(method_idx, this_object, referrer, true, kInterface, thread);
}

mirror::Object* art_portable_find_interface_method_from_code(uint32_t method_idx,
                                                     mirror::Object* this_object,
                                                     mirror::AbstractMethod* referrer,
                                                     Thread* thread)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return FindMethodHelper(method_idx, this_object, referrer, false, kInterface, thread);
}

mirror::Object* art_portable_initialize_static_storage_from_code(uint32_t type_idx,
                                                         mirror::AbstractMethod* referrer,
                                                         Thread* thread)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return ResolveVerifyAndClinit(type_idx, referrer, thread, true, false);
}

mirror::Object* art_portable_initialize_type_from_code(uint32_t type_idx,
                                               mirror::AbstractMethod* referrer,
                                               Thread* thread)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return ResolveVerifyAndClinit(type_idx, referrer, thread, false, false);
}

mirror::Object* art_portable_initialize_type_and_verify_access_from_code(uint32_t type_idx,
                                                                 mirror::AbstractMethod* referrer,
                                                                 Thread* thread)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  // Called when caller isn't guaranteed to have access to a type and the dex cache may be
  // unpopulated
  return ResolveVerifyAndClinit(type_idx, referrer, thread, false, true);
}

mirror::Object* art_portable_resolve_string_from_code(mirror::AbstractMethod* referrer, uint32_t string_idx)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return ResolveStringFromCode(referrer, string_idx);
}

int32_t art_portable_set32_static_from_code(uint32_t field_idx, mirror::AbstractMethod* referrer, int32_t new_value)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::Field* field = FindFieldFast(field_idx, referrer, StaticPrimitiveWrite, sizeof(uint32_t));
  if (LIKELY(field != NULL)) {
    field->Set32(field->GetDeclaringClass(), new_value);
    return 0;
  }
  field = FindFieldFromCode(field_idx, referrer, Thread::Current(),
                            StaticPrimitiveWrite, sizeof(uint32_t), true);
  if (LIKELY(field != NULL)) {
    field->Set32(field->GetDeclaringClass(), new_value);
    return 0;
  }
  return -1;
}

int32_t art_portable_set64_static_from_code(uint32_t field_idx, mirror::AbstractMethod* referrer, int64_t new_value)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::Field* field = FindFieldFast(field_idx, referrer, StaticPrimitiveWrite, sizeof(uint64_t));
  if (LIKELY(field != NULL)) {
    field->Set64(field->GetDeclaringClass(), new_value);
    return 0;
  }
  field = FindFieldFromCode(field_idx, referrer, Thread::Current(),
                            StaticPrimitiveWrite, sizeof(uint64_t), true);
  if (LIKELY(field != NULL)) {
    field->Set64(field->GetDeclaringClass(), new_value);
    return 0;
  }
  return -1;
}

int32_t art_portable_set_obj_static_from_code(uint32_t field_idx, mirror::AbstractMethod* referrer, mirror::Object* new_value)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::Field* field = FindFieldFast(field_idx, referrer, StaticObjectWrite, sizeof(mirror::Object*));
  if (LIKELY(field != NULL)) {
    field->SetObj(field->GetDeclaringClass(), new_value);
    return 0;
  }
  field = FindFieldFromCode(field_idx, referrer, Thread::Current(),
                            StaticObjectWrite, sizeof(mirror::Object*), true);
  if (LIKELY(field != NULL)) {
    field->SetObj(field->GetDeclaringClass(), new_value);
    return 0;
  }
  return -1;
}

int32_t art_portable_get32_static_from_code(uint32_t field_idx, mirror::AbstractMethod* referrer)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::Field* field = FindFieldFast(field_idx, referrer, StaticPrimitiveRead, sizeof(uint32_t));
  if (LIKELY(field != NULL)) {
    return field->Get32(field->GetDeclaringClass());
  }
  field = FindFieldFromCode(field_idx, referrer, Thread::Current(),
                            StaticPrimitiveRead, sizeof(uint32_t), true);
  if (LIKELY(field != NULL)) {
    return field->Get32(field->GetDeclaringClass());
  }
  return 0;
}

int64_t art_portable_get64_static_from_code(uint32_t field_idx, mirror::AbstractMethod* referrer)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::Field* field = FindFieldFast(field_idx, referrer, StaticPrimitiveRead, sizeof(uint64_t));
  if (LIKELY(field != NULL)) {
    return field->Get64(field->GetDeclaringClass());
  }
  field = FindFieldFromCode(field_idx, referrer, Thread::Current(),
                            StaticPrimitiveRead, sizeof(uint64_t), true);
  if (LIKELY(field != NULL)) {
    return field->Get64(field->GetDeclaringClass());
  }
  return 0;
}

mirror::Object* art_portable_get_obj_static_from_code(uint32_t field_idx, mirror::AbstractMethod* referrer)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::Field* field = FindFieldFast(field_idx, referrer, StaticObjectRead, sizeof(mirror::Object*));
  if (LIKELY(field != NULL)) {
    return field->GetObj(field->GetDeclaringClass());
  }
  field = FindFieldFromCode(field_idx, referrer, Thread::Current(),
                            StaticObjectRead, sizeof(mirror::Object*), true);
  if (LIKELY(field != NULL)) {
    return field->GetObj(field->GetDeclaringClass());
  }
  return 0;
}

int32_t art_portable_set32_instance_from_code(uint32_t field_idx, mirror::AbstractMethod* referrer,
                                     mirror::Object* obj, uint32_t new_value)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::Field* field = FindFieldFast(field_idx, referrer, InstancePrimitiveWrite, sizeof(uint32_t));
  if (LIKELY(field != NULL)) {
    field->Set32(obj, new_value);
    return 0;
  }
  field = FindFieldFromCode(field_idx, referrer, Thread::Current(),
                            InstancePrimitiveWrite, sizeof(uint32_t), true);
  if (LIKELY(field != NULL)) {
    field->Set32(obj, new_value);
    return 0;
  }
  return -1;
}

int32_t art_portable_set64_instance_from_code(uint32_t field_idx, mirror::AbstractMethod* referrer,
                                      mirror::Object* obj, int64_t new_value)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::Field* field = FindFieldFast(field_idx, referrer, InstancePrimitiveWrite, sizeof(uint64_t));
  if (LIKELY(field != NULL)) {
    field->Set64(obj, new_value);
    return 0;
  }
  field = FindFieldFromCode(field_idx, referrer, Thread::Current(),
                            InstancePrimitiveWrite, sizeof(uint64_t), true);
  if (LIKELY(field != NULL)) {
    field->Set64(obj, new_value);
    return 0;
  }
  return -1;
}

int32_t art_portable_set_obj_instance_from_code(uint32_t field_idx, mirror::AbstractMethod* referrer,
                                        mirror::Object* obj, mirror::Object* new_value)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::Field* field = FindFieldFast(field_idx, referrer, InstanceObjectWrite, sizeof(mirror::Object*));
  if (LIKELY(field != NULL)) {
    field->SetObj(obj, new_value);
    return 0;
  }
  field = FindFieldFromCode(field_idx, referrer, Thread::Current(),
                            InstanceObjectWrite, sizeof(mirror::Object*), true);
  if (LIKELY(field != NULL)) {
    field->SetObj(obj, new_value);
    return 0;
  }
  return -1;
}

int32_t art_portable_get32_instance_from_code(uint32_t field_idx, mirror::AbstractMethod* referrer, mirror::Object* obj)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::Field* field = FindFieldFast(field_idx, referrer, InstancePrimitiveRead, sizeof(uint32_t));
  if (LIKELY(field != NULL)) {
    return field->Get32(obj);
  }
  field = FindFieldFromCode(field_idx, referrer, Thread::Current(),
                            InstancePrimitiveRead, sizeof(uint32_t), true);
  if (LIKELY(field != NULL)) {
    return field->Get32(obj);
  }
  return 0;
}

int64_t art_portable_get64_instance_from_code(uint32_t field_idx, mirror::AbstractMethod* referrer, mirror::Object* obj)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::Field* field = FindFieldFast(field_idx, referrer, InstancePrimitiveRead, sizeof(uint64_t));
  if (LIKELY(field != NULL)) {
    return field->Get64(obj);
  }
  field = FindFieldFromCode(field_idx, referrer, Thread::Current(),
                            InstancePrimitiveRead, sizeof(uint64_t), true);
  if (LIKELY(field != NULL)) {
    return field->Get64(obj);
  }
  return 0;
}

mirror::Object* art_portable_get_obj_instance_from_code(uint32_t field_idx, mirror::AbstractMethod* referrer, mirror::Object* obj)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::Field* field = FindFieldFast(field_idx, referrer, InstanceObjectRead, sizeof(mirror::Object*));
  if (LIKELY(field != NULL)) {
    return field->GetObj(obj);
  }
  field = FindFieldFromCode(field_idx, referrer, Thread::Current(),
                            InstanceObjectRead, sizeof(mirror::Object*), true);
  if (LIKELY(field != NULL)) {
    return field->GetObj(obj);
  }
  return 0;
}

void art_portable_fill_array_data_from_code(mirror::AbstractMethod* method, uint32_t dex_pc,
                                    mirror::Array* array, uint32_t payload_offset)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  // Test: Is array equal to null? (Guard NullPointerException)
  if (UNLIKELY(array == NULL)) {
    art_portable_throw_null_pointer_exception_from_code(dex_pc);
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
    art_portable_throw_array_bounds_from_code(array_len, last_index);
    return;
  }

  // Copy the data
  size_t size = payload->element_width * payload->element_count;
  memcpy(array->GetRawData(payload->element_width), payload->data, size);
}



//----------------------------------------------------------------------------
// Type checking, in the nature of casting
//----------------------------------------------------------------------------

int32_t art_portable_is_assignable_from_code(const mirror::Class* dest_type, const mirror::Class* src_type)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DCHECK(dest_type != NULL);
  DCHECK(src_type != NULL);
  return dest_type->IsAssignableFrom(src_type) ? 1 : 0;
}

void art_portable_check_cast_from_code(const mirror::Class* dest_type, const mirror::Class* src_type)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DCHECK(dest_type->IsClass()) << PrettyClass(dest_type);
  DCHECK(src_type->IsClass()) << PrettyClass(src_type);
  if (UNLIKELY(!dest_type->IsAssignableFrom(src_type))) {
    ThrowClassCastException(dest_type, src_type);
  }
}

void art_portable_check_put_array_element_from_code(const mirror::Object* element,
                                                    const mirror::Object* array)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (element == NULL) {
    return;
  }
  DCHECK(array != NULL);
  mirror::Class* array_class = array->GetClass();
  DCHECK(array_class != NULL);
  mirror::Class* component_type = array_class->GetComponentType();
  mirror::Class* element_class = element->GetClass();
  if (UNLIKELY(!component_type->IsAssignableFrom(element_class))) {
    ThrowArrayStoreException(element_class, array_class);
  }
  return;
}

//----------------------------------------------------------------------------
// JNI
//----------------------------------------------------------------------------

// Called on entry to JNI, transition out of Runnable and release share of mutator_lock_.
uint32_t art_portable_jni_method_start(Thread* self)
    UNLOCK_FUNCTION(GlobalSynchronizatio::mutator_lock_) {
  JNIEnvExt* env = self->GetJniEnv();
  uint32_t saved_local_ref_cookie = env->local_ref_cookie;
  env->local_ref_cookie = env->locals.GetSegmentState();
  self->TransitionFromRunnableToSuspended(kNative);
  return saved_local_ref_cookie;
}

uint32_t art_portable_jni_method_start_synchronized(jobject to_lock, Thread* self)
    UNLOCK_FUNCTION(Locks::mutator_lock_) {
  self->DecodeJObject(to_lock)->MonitorEnter(self);
  return art_portable_jni_method_start(self);
}

static inline void PopLocalReferences(uint32_t saved_local_ref_cookie, Thread* self) {
  JNIEnvExt* env = self->GetJniEnv();
  env->locals.SetSegmentState(env->local_ref_cookie);
  env->local_ref_cookie = saved_local_ref_cookie;
}

void art_portable_jni_method_end(uint32_t saved_local_ref_cookie, Thread* self)
    SHARED_LOCK_FUNCTION(Locks::mutator_lock_) {
  self->TransitionFromSuspendedToRunnable();
  PopLocalReferences(saved_local_ref_cookie, self);
}


void art_portable_jni_method_end_synchronized(uint32_t saved_local_ref_cookie,
                                      jobject locked,
                                      Thread* self)
    SHARED_LOCK_FUNCTION(Locks::mutator_lock_) {
  self->TransitionFromSuspendedToRunnable();
  UnlockJniSynchronizedMethod(locked, self);  // Must decode before pop.
  PopLocalReferences(saved_local_ref_cookie, self);
}

mirror::Object* art_portable_jni_method_end_with_reference(jobject result, uint32_t saved_local_ref_cookie,
                                                   Thread* self)
    SHARED_LOCK_FUNCTION(Locks::mutator_lock_) {
  self->TransitionFromSuspendedToRunnable();
  mirror::Object* o = self->DecodeJObject(result);  // Must decode before pop.
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

mirror::Object* art_portable_jni_method_end_with_reference_synchronized(jobject result,
                                                                uint32_t saved_local_ref_cookie,
                                                                jobject locked, Thread* self)
    SHARED_LOCK_FUNCTION(Locks::mutator_lock_) {
  self->TransitionFromSuspendedToRunnable();
  UnlockJniSynchronizedMethod(locked, self);  // Must decode before pop.
  mirror::Object* o = self->DecodeJObject(result);
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

// Handler for invocation on proxy methods. Create a boxed argument array and invoke the invocation
// handler which is a field within the proxy object receiver. The var args encode the arguments
// with the last argument being a pointer to a JValue to store the result in.
void art_portable_proxy_invoke_handler_from_code(mirror::AbstractMethod* proxy_method, ...)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  va_list ap;
  va_start(ap, proxy_method);

  mirror::Object* receiver = va_arg(ap, mirror::Object*);
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
  mirror::AbstractMethod* interface_method = proxy_method->FindOverriddenMethod();
  DCHECK(interface_method != NULL);
  DCHECK(!interface_method->IsProxyMethod()) << PrettyMethod(interface_method);
  jobject interface_method_jobj = soa.AddLocalReference<jobject>(interface_method);

  // Record arguments and turn mirror::Object* arguments into jobject to survive GC.
  std::vector<jvalue> args;
  const size_t num_params = proxy_mh.NumArgs();
  for (size_t i = 1; i < num_params; ++i) {
    jvalue val;
    switch (proxy_mh.GetParamPrimitiveType(i)) {
      case Primitive::kPrimNot:
        val.l = soa.AddLocalReference<jobject>(va_arg(ap, mirror::Object*));
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

//----------------------------------------------------------------------------
// Memory barrier
//----------------------------------------------------------------------------

void art_portable_constructor_barrier() {
  LOG(FATAL) << "Implemented by IRBuilder.";
}

}  // extern "C"
