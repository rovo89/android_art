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

extern "C" uint32_t artGet32StaticFromCode(uint32_t field_idx, const Method* referrer,
                                           Thread* self, Method** sp) {
  Field* field = FindFieldFast(field_idx, referrer, true, false, sizeof(int32_t));
  if (LIKELY(field != NULL)) {
    return field->Get32(NULL);
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode(field_idx, referrer, self, true, true, false, sizeof(int32_t));
  if (LIKELY(field != NULL)) {
    return field->Get32(NULL);
  }
  return 0;  // Will throw exception by checking with Thread::Current
}

extern "C" uint64_t artGet64StaticFromCode(uint32_t field_idx, const Method* referrer,
                                           Thread* self, Method** sp) {
  Field* field = FindFieldFast(field_idx, referrer, true, false, sizeof(int64_t));
  if (LIKELY(field != NULL)) {
    return field->Get64(NULL);
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode(field_idx, referrer, self, true, true, false, sizeof(int64_t));
  if (LIKELY(field != NULL)) {
    return field->Get64(NULL);
  }
  return 0;  // Will throw exception by checking with Thread::Current
}

extern "C" Object* artGetObjStaticFromCode(uint32_t field_idx, const Method* referrer,
                                           Thread* self, Method** sp) {
  Field* field = FindFieldFast(field_idx, referrer, false, false, sizeof(Object*));
  if (LIKELY(field != NULL)) {
    return field->GetObj(NULL);
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode(field_idx, referrer, self, true, false, false, sizeof(Object*));
  if (LIKELY(field != NULL)) {
    return field->GetObj(NULL);
  }
  return NULL;  // Will throw exception by checking with Thread::Current
}

extern "C" uint32_t artGet32InstanceFromCode(uint32_t field_idx, Object* obj,
                                             const Method* referrer, Thread* self, Method** sp) {
  Field* field = FindFieldFast(field_idx, referrer, true, false, sizeof(int32_t));
  if (LIKELY(field != NULL && obj != NULL)) {
    return field->Get32(obj);
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode(field_idx, referrer, self, false, true, false, sizeof(int32_t));
  if (LIKELY(field != NULL)) {
    if (UNLIKELY(obj == NULL)) {
      ThrowNullPointerExceptionForFieldAccess(self, field, true);
    } else {
      return field->Get32(obj);
    }
  }
  return 0;  // Will throw exception by checking with Thread::Current
}

extern "C" uint64_t artGet64InstanceFromCode(uint32_t field_idx, Object* obj,
                                             const Method* referrer, Thread* self, Method** sp) {
  Field* field = FindFieldFast(field_idx, referrer, true, false, sizeof(int64_t));
  if (LIKELY(field != NULL && obj != NULL)) {
    return field->Get64(obj);
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode(field_idx, referrer, self, false, true, false, sizeof(int64_t));
  if (LIKELY(field != NULL)) {
    if (UNLIKELY(obj == NULL)) {
      ThrowNullPointerExceptionForFieldAccess(self, field, true);
    } else {
      return field->Get64(obj);
    }
  }
  return 0;  // Will throw exception by checking with Thread::Current
}

extern "C" Object* artGetObjInstanceFromCode(uint32_t field_idx, Object* obj,
                                              const Method* referrer, Thread* self, Method** sp) {
  Field* field = FindFieldFast(field_idx, referrer, false, false, sizeof(Object*));
  if (LIKELY(field != NULL && obj != NULL)) {
    return field->GetObj(obj);
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode(field_idx, referrer, self, false, false, false, sizeof(Object*));
  if (LIKELY(field != NULL)) {
    if (UNLIKELY(obj == NULL)) {
      ThrowNullPointerExceptionForFieldAccess(self, field, true);
    } else {
      return field->GetObj(obj);
    }
  }
  return NULL;  // Will throw exception by checking with Thread::Current
}

extern "C" int artSet32StaticFromCode(uint32_t field_idx, uint32_t new_value,
                                      const Method* referrer, Thread* self, Method** sp) {
  Field* field = FindFieldFast(field_idx, referrer, true, true, sizeof(int32_t));
  if (LIKELY(field != NULL)) {
    field->Set32(NULL, new_value);
    return 0;  // success
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode(field_idx, referrer, self, true, true, true, sizeof(int32_t));
  if (LIKELY(field != NULL)) {
    field->Set32(NULL, new_value);
    return 0;  // success
  }
  return -1;  // failure
}

extern "C" int artSet64StaticFromCode(uint32_t field_idx, const Method* referrer,
                                      uint64_t new_value, Thread* self, Method** sp) {
  Field* field = FindFieldFast(field_idx, referrer, true, true, sizeof(int64_t));
  if (LIKELY(field != NULL)) {
    field->Set64(NULL, new_value);
    return 0;  // success
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode(field_idx, referrer, self, true, true, true, sizeof(int64_t));
  if (LIKELY(field != NULL)) {
    field->Set64(NULL, new_value);
    return 0;  // success
  }
  return -1;  // failure
}

extern "C" int artSetObjStaticFromCode(uint32_t field_idx, Object* new_value,
                                       const Method* referrer, Thread* self, Method** sp) {
  Field* field = FindFieldFast(field_idx, referrer, false, true, sizeof(Object*));
  if (LIKELY(field != NULL)) {
    if (LIKELY(!FieldHelper(field).IsPrimitiveType())) {
      field->SetObj(NULL, new_value);
      return 0;  // success
    }
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode(field_idx, referrer, self, true, false, true, sizeof(Object*));
  if (LIKELY(field != NULL)) {
    field->SetObj(NULL, new_value);
    return 0;  // success
  }
  return -1;  // failure
}

extern "C" int artSet32InstanceFromCode(uint32_t field_idx, Object* obj, uint32_t new_value,
                                        const Method* referrer, Thread* self, Method** sp) {
  Field* field = FindFieldFast(field_idx, referrer, true, true, sizeof(int32_t));
  if (LIKELY(field != NULL && obj != NULL)) {
    field->Set32(obj, new_value);
    return 0;  // success
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode(field_idx, referrer, self, false, true, true, sizeof(int32_t));
  if (LIKELY(field != NULL)) {
    if (UNLIKELY(obj == NULL)) {
      ThrowNullPointerExceptionForFieldAccess(self, field, false);
    } else {
      field->Set32(obj, new_value);
      return 0;  // success
    }
  }
  return -1;  // failure
}

extern "C" int artSet64InstanceFromCode(uint32_t field_idx, Object* obj, uint64_t new_value,
                                        Thread* self, Method** sp) {
  Method* callee_save = Runtime::Current()->GetCalleeSaveMethod(Runtime::kRefsOnly);
  Method* referrer = sp[callee_save->GetFrameSizeInBytes() / sizeof(Method*)];
  Field* field = FindFieldFast(field_idx, referrer, true, true, sizeof(int64_t));
  if (LIKELY(field != NULL  && obj != NULL)) {
    field->Set64(obj, new_value);
    return 0;  // success
  }
  *sp = callee_save;
  self->SetTopOfStack(sp, 0);
  field = FindFieldFromCode(field_idx, referrer, self, false, true, true, sizeof(int64_t));
  if (LIKELY(field != NULL)) {
    if (UNLIKELY(obj == NULL)) {
      ThrowNullPointerExceptionForFieldAccess(self, field, false);
    } else {
      field->Set64(obj, new_value);
      return 0;  // success
    }
  }
  return -1;  // failure
}

extern "C" int artSetObjInstanceFromCode(uint32_t field_idx, Object* obj, Object* new_value,
                                         const Method* referrer, Thread* self, Method** sp) {
  Field* field = FindFieldFast(field_idx, referrer, false, true, sizeof(Object*));
  if (LIKELY(field != NULL && obj != NULL)) {
    field->SetObj(obj, new_value);
    return 0;  // success
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode(field_idx, referrer, self, false, false, true, sizeof(Object*));
  if (LIKELY(field != NULL)) {
    if (UNLIKELY(obj == NULL)) {
      ThrowNullPointerExceptionForFieldAccess(self, field, false);
    } else {
      field->SetObj(obj, new_value);
      return 0;  // success
    }
  }
  return -1;  // failure
}

}  // namespace art
