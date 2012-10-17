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

#include "callee_save_frame.h"
#include "runtime_support.h"

#include <stdint.h>

namespace art {

extern "C" uint32_t artGet32StaticFromCode(uint32_t field_idx, const AbstractMethod* referrer,
                                           Thread* self, AbstractMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Field* field = FindFieldFast(field_idx, referrer, StaticPrimitiveRead, sizeof(int32_t));
  if (LIKELY(field != NULL)) {
    return field->Get32(field->GetDeclaringClass());
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode(field_idx, referrer, self, StaticPrimitiveRead, sizeof(int32_t));
  if (LIKELY(field != NULL)) {
    return field->Get32(field->GetDeclaringClass());
  }
  return 0;  // Will throw exception by checking with Thread::Current
}

extern "C" uint64_t artGet64StaticFromCode(uint32_t field_idx, const AbstractMethod* referrer,
                                           Thread* self, AbstractMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Field* field = FindFieldFast(field_idx, referrer, StaticPrimitiveRead, sizeof(int64_t));
  if (LIKELY(field != NULL)) {
    return field->Get64(field->GetDeclaringClass());
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode(field_idx, referrer, self, StaticPrimitiveRead, sizeof(int64_t));
  if (LIKELY(field != NULL)) {
    return field->Get64(field->GetDeclaringClass());
  }
  return 0;  // Will throw exception by checking with Thread::Current
}

extern "C" Object* artGetObjStaticFromCode(uint32_t field_idx, const AbstractMethod* referrer,
                                           Thread* self, AbstractMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Field* field = FindFieldFast(field_idx, referrer, StaticObjectRead, sizeof(Object*));
  if (LIKELY(field != NULL)) {
    return field->GetObj(field->GetDeclaringClass());
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode(field_idx, referrer, self, StaticObjectRead, sizeof(Object*));
  if (LIKELY(field != NULL)) {
    return field->GetObj(field->GetDeclaringClass());
  }
  return NULL;  // Will throw exception by checking with Thread::Current
}

extern "C" uint32_t artGet32InstanceFromCode(uint32_t field_idx, Object* obj,
                                             const AbstractMethod* referrer, Thread* self,
                                             AbstractMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Field* field = FindFieldFast(field_idx, referrer, InstancePrimitiveRead, sizeof(int32_t));
  if (LIKELY(field != NULL && obj != NULL)) {
    return field->Get32(obj);
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode(field_idx, referrer, self, InstancePrimitiveRead, sizeof(int32_t));
  if (LIKELY(field != NULL)) {
    if (UNLIKELY(obj == NULL)) {
      ThrowNullPointerExceptionForFieldAccess(field, true);
    } else {
      return field->Get32(obj);
    }
  }
  return 0;  // Will throw exception by checking with Thread::Current
}

extern "C" uint64_t artGet64InstanceFromCode(uint32_t field_idx, Object* obj,
                                             const AbstractMethod* referrer, Thread* self,
                                             AbstractMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Field* field = FindFieldFast(field_idx, referrer, InstancePrimitiveRead, sizeof(int64_t));
  if (LIKELY(field != NULL && obj != NULL)) {
    return field->Get64(obj);
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode(field_idx, referrer, self, InstancePrimitiveRead, sizeof(int64_t));
  if (LIKELY(field != NULL)) {
    if (UNLIKELY(obj == NULL)) {
      ThrowNullPointerExceptionForFieldAccess(field, true);
    } else {
      return field->Get64(obj);
    }
  }
  return 0;  // Will throw exception by checking with Thread::Current
}

extern "C" Object* artGetObjInstanceFromCode(uint32_t field_idx, Object* obj,
                                              const AbstractMethod* referrer, Thread* self,
                                              AbstractMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Field* field = FindFieldFast(field_idx, referrer, InstanceObjectRead, sizeof(Object*));
  if (LIKELY(field != NULL && obj != NULL)) {
    return field->GetObj(obj);
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode(field_idx, referrer, self, InstanceObjectRead, sizeof(Object*));
  if (LIKELY(field != NULL)) {
    if (UNLIKELY(obj == NULL)) {
      ThrowNullPointerExceptionForFieldAccess(field, true);
    } else {
      return field->GetObj(obj);
    }
  }
  return NULL;  // Will throw exception by checking with Thread::Current
}

extern "C" int artSet32StaticFromCode(uint32_t field_idx, uint32_t new_value,
                                      const AbstractMethod* referrer, Thread* self,
                                      AbstractMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Field* field = FindFieldFast(field_idx, referrer, StaticPrimitiveWrite, sizeof(int32_t));
  if (LIKELY(field != NULL)) {
    field->Set32(field->GetDeclaringClass(), new_value);
    return 0;  // success
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode(field_idx, referrer, self, StaticPrimitiveWrite, sizeof(int32_t));
  if (LIKELY(field != NULL)) {
    field->Set32(field->GetDeclaringClass(), new_value);
    return 0;  // success
  }
  return -1;  // failure
}

extern "C" int artSet64StaticFromCode(uint32_t field_idx, const AbstractMethod* referrer,
                                      uint64_t new_value, Thread* self, AbstractMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Field* field = FindFieldFast(field_idx, referrer, StaticPrimitiveWrite, sizeof(int64_t));
  if (LIKELY(field != NULL)) {
    field->Set64(field->GetDeclaringClass(), new_value);
    return 0;  // success
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode(field_idx, referrer, self, StaticPrimitiveWrite, sizeof(int64_t));
  if (LIKELY(field != NULL)) {
    field->Set64(field->GetDeclaringClass(), new_value);
    return 0;  // success
  }
  return -1;  // failure
}

extern "C" int artSetObjStaticFromCode(uint32_t field_idx, Object* new_value,
                                       const AbstractMethod* referrer, Thread* self,
                                       AbstractMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Field* field = FindFieldFast(field_idx, referrer, StaticObjectWrite, sizeof(Object*));
  if (LIKELY(field != NULL)) {
    if (LIKELY(!FieldHelper(field).IsPrimitiveType())) {
      field->SetObj(field->GetDeclaringClass(), new_value);
      return 0;  // success
    }
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode(field_idx, referrer, self, StaticObjectWrite, sizeof(Object*));
  if (LIKELY(field != NULL)) {
    field->SetObj(field->GetDeclaringClass(), new_value);
    return 0;  // success
  }
  return -1;  // failure
}

extern "C" int artSet32InstanceFromCode(uint32_t field_idx, Object* obj, uint32_t new_value,
                                        const AbstractMethod* referrer, Thread* self,
                                        AbstractMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Field* field = FindFieldFast(field_idx, referrer, InstancePrimitiveWrite, sizeof(int32_t));
  if (LIKELY(field != NULL && obj != NULL)) {
    field->Set32(obj, new_value);
    return 0;  // success
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode(field_idx, referrer, self, InstancePrimitiveWrite, sizeof(int32_t));
  if (LIKELY(field != NULL)) {
    if (UNLIKELY(obj == NULL)) {
      ThrowNullPointerExceptionForFieldAccess(field, false);
    } else {
      field->Set32(obj, new_value);
      return 0;  // success
    }
  }
  return -1;  // failure
}

extern "C" int artSet64InstanceFromCode(uint32_t field_idx, Object* obj, uint64_t new_value,
                                        Thread* self, AbstractMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  AbstractMethod* callee_save = Runtime::Current()->GetCalleeSaveMethod(Runtime::kRefsOnly);
  AbstractMethod* referrer = sp[callee_save->GetFrameSizeInBytes() / sizeof(AbstractMethod*)];
  Field* field = FindFieldFast(field_idx, referrer, InstancePrimitiveWrite, sizeof(int64_t));
  if (LIKELY(field != NULL  && obj != NULL)) {
    field->Set64(obj, new_value);
    return 0;  // success
  }
  *sp = callee_save;
  self->SetTopOfStack(sp, 0);
  field = FindFieldFromCode(field_idx, referrer, self, InstancePrimitiveWrite, sizeof(int64_t));
  if (LIKELY(field != NULL)) {
    if (UNLIKELY(obj == NULL)) {
      ThrowNullPointerExceptionForFieldAccess(field, false);
    } else {
      field->Set64(obj, new_value);
      return 0;  // success
    }
  }
  return -1;  // failure
}

extern "C" int artSetObjInstanceFromCode(uint32_t field_idx, Object* obj, Object* new_value,
                                         const AbstractMethod* referrer, Thread* self,
                                         AbstractMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Field* field = FindFieldFast(field_idx, referrer, InstanceObjectWrite, sizeof(Object*));
  if (LIKELY(field != NULL && obj != NULL)) {
    field->SetObj(obj, new_value);
    return 0;  // success
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode(field_idx, referrer, self, InstanceObjectWrite, sizeof(Object*));
  if (LIKELY(field != NULL)) {
    if (UNLIKELY(obj == NULL)) {
      ThrowNullPointerExceptionForFieldAccess(field, false);
    } else {
      field->SetObj(obj, new_value);
      return 0;  // success
    }
  }
  return -1;  // failure
}

}  // namespace art
