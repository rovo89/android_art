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
#include "dex_file-inl.h"
#include "entrypoints/entrypoint_utils.h"
#include "mirror/art_field-inl.h"
#include "mirror/art_method-inl.h"
#include "mirror/class-inl.h"

#include <stdint.h>

namespace art {

extern "C" uint32_t artGet32StaticFromCode(uint32_t field_idx,
                                           mirror::ArtMethod* referrer,
                                           Thread* self, mirror::ArtMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::ArtField* field = FindFieldFast(field_idx, referrer, StaticPrimitiveRead,
                                          sizeof(int32_t));
  if (LIKELY(field != NULL)) {
    return field->Get32(field->GetDeclaringClass());
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode<StaticPrimitiveRead, true>(field_idx, referrer, self, sizeof(int32_t));
  if (LIKELY(field != NULL)) {
    return field->Get32(field->GetDeclaringClass());
  }
  return 0;  // Will throw exception by checking with Thread::Current
}

extern "C" uint64_t artGet64StaticFromCode(uint32_t field_idx,
                                           mirror::ArtMethod* referrer,
                                           Thread* self, mirror::ArtMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::ArtField* field = FindFieldFast(field_idx, referrer, StaticPrimitiveRead,
                                          sizeof(int64_t));
  if (LIKELY(field != NULL)) {
    return field->Get64(field->GetDeclaringClass());
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode<StaticPrimitiveRead, true>(field_idx, referrer, self, sizeof(int64_t));
  if (LIKELY(field != NULL)) {
    return field->Get64(field->GetDeclaringClass());
  }
  return 0;  // Will throw exception by checking with Thread::Current
}

extern "C" mirror::Object* artGetObjStaticFromCode(uint32_t field_idx,
                                                   mirror::ArtMethod* referrer,
                                                   Thread* self, mirror::ArtMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::ArtField* field = FindFieldFast(field_idx, referrer, StaticObjectRead,
                                          sizeof(mirror::HeapReference<mirror::Object>));
  if (LIKELY(field != NULL)) {
    return field->GetObj(field->GetDeclaringClass());
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode<StaticObjectRead, true>(field_idx, referrer, self,
                                                    sizeof(mirror::HeapReference<mirror::Object>));
  if (LIKELY(field != NULL)) {
    return field->GetObj(field->GetDeclaringClass());
  }
  return NULL;  // Will throw exception by checking with Thread::Current
}

extern "C" uint32_t artGet32InstanceFromCode(uint32_t field_idx, mirror::Object* obj,
                                             mirror::ArtMethod* referrer, Thread* self,
                                             mirror::ArtMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::ArtField* field = FindFieldFast(field_idx, referrer, InstancePrimitiveRead,
                                          sizeof(int32_t));
  if (LIKELY(field != NULL && obj != NULL)) {
    return field->Get32(obj);
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode<InstancePrimitiveRead, true>(field_idx, referrer, self,
                                                         sizeof(int32_t));
  if (LIKELY(field != NULL)) {
    if (UNLIKELY(obj == NULL)) {
      ThrowLocation throw_location = self->GetCurrentLocationForThrow();
      ThrowNullPointerExceptionForFieldAccess(throw_location, field, true);
    } else {
      return field->Get32(obj);
    }
  }
  return 0;  // Will throw exception by checking with Thread::Current
}

extern "C" uint64_t artGet64InstanceFromCode(uint32_t field_idx, mirror::Object* obj,
                                             mirror::ArtMethod* referrer, Thread* self,
                                             mirror::ArtMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::ArtField* field = FindFieldFast(field_idx, referrer, InstancePrimitiveRead,
                                          sizeof(int64_t));
  if (LIKELY(field != NULL && obj != NULL)) {
    return field->Get64(obj);
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode<InstancePrimitiveRead, true>(field_idx, referrer, self,
                                                         sizeof(int64_t));
  if (LIKELY(field != NULL)) {
    if (UNLIKELY(obj == NULL)) {
      ThrowLocation throw_location = self->GetCurrentLocationForThrow();
      ThrowNullPointerExceptionForFieldAccess(throw_location, field, true);
    } else {
      return field->Get64(obj);
    }
  }
  return 0;  // Will throw exception by checking with Thread::Current
}

extern "C" mirror::Object* artGetObjInstanceFromCode(uint32_t field_idx, mirror::Object* obj,
                                                     mirror::ArtMethod* referrer,
                                                     Thread* self,
                                                     mirror::ArtMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::ArtField* field = FindFieldFast(field_idx, referrer, InstanceObjectRead,
                                          sizeof(mirror::HeapReference<mirror::Object>));
  if (LIKELY(field != NULL && obj != NULL)) {
    return field->GetObj(obj);
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode<InstanceObjectRead, true>(field_idx, referrer, self,
                                                      sizeof(mirror::HeapReference<mirror::Object>));
  if (LIKELY(field != NULL)) {
    if (UNLIKELY(obj == NULL)) {
      ThrowLocation throw_location = self->GetCurrentLocationForThrow();
      ThrowNullPointerExceptionForFieldAccess(throw_location, field, true);
    } else {
      return field->GetObj(obj);
    }
  }
  return NULL;  // Will throw exception by checking with Thread::Current
}

extern "C" int artSet32StaticFromCode(uint32_t field_idx, uint32_t new_value,
                                      mirror::ArtMethod* referrer, Thread* self,
                                      mirror::ArtMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::ArtField* field = FindFieldFast(field_idx, referrer, StaticPrimitiveWrite,
                                          sizeof(int32_t));
  if (LIKELY(field != NULL)) {
    // Compiled code can't use transactional mode.
    field->Set32<false>(field->GetDeclaringClass(), new_value);
    return 0;  // success
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode<StaticPrimitiveWrite, true>(field_idx, referrer, self, sizeof(int32_t));
  if (LIKELY(field != NULL)) {
    // Compiled code can't use transactional mode.
    field->Set32<false>(field->GetDeclaringClass(), new_value);
    return 0;  // success
  }
  return -1;  // failure
}

extern "C" int artSet64StaticFromCode(uint32_t field_idx, mirror::ArtMethod* referrer,
                                      uint64_t new_value, Thread* self, mirror::ArtMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::ArtField* field = FindFieldFast(field_idx, referrer, StaticPrimitiveWrite,
                                          sizeof(int64_t));
  if (LIKELY(field != NULL)) {
    // Compiled code can't use transactional mode.
    field->Set64<false>(field->GetDeclaringClass(), new_value);
    return 0;  // success
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode<StaticPrimitiveWrite, true>(field_idx, referrer, self, sizeof(int64_t));
  if (LIKELY(field != NULL)) {
    // Compiled code can't use transactional mode.
    field->Set64<false>(field->GetDeclaringClass(), new_value);
    return 0;  // success
  }
  return -1;  // failure
}

extern "C" int artSetObjStaticFromCode(uint32_t field_idx, mirror::Object* new_value,
                                       mirror::ArtMethod* referrer, Thread* self,
                                       mirror::ArtMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::ArtField* field = FindFieldFast(field_idx, referrer, StaticObjectWrite,
                                          sizeof(mirror::HeapReference<mirror::Object>));
  if (LIKELY(field != NULL)) {
    if (LIKELY(!FieldHelper(field).IsPrimitiveType())) {
      // Compiled code can't use transactional mode.
      field->SetObj<false>(field->GetDeclaringClass(), new_value);
      return 0;  // success
    }
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode<StaticObjectWrite, true>(field_idx, referrer, self,
                                                     sizeof(mirror::HeapReference<mirror::Object>));
  if (LIKELY(field != NULL)) {
    // Compiled code can't use transactional mode.
    field->SetObj<false>(field->GetDeclaringClass(), new_value);
    return 0;  // success
  }
  return -1;  // failure
}

extern "C" int artSet32InstanceFromCode(uint32_t field_idx, mirror::Object* obj, uint32_t new_value,
                                        mirror::ArtMethod* referrer, Thread* self,
                                        mirror::ArtMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::ArtField* field = FindFieldFast(field_idx, referrer, InstancePrimitiveWrite,
                                          sizeof(int32_t));
  if (LIKELY(field != NULL && obj != NULL)) {
    // Compiled code can't use transactional mode.
    field->Set32<false>(obj, new_value);
    return 0;  // success
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode<InstancePrimitiveWrite, true>(field_idx, referrer, self,
                                                          sizeof(int32_t));
  if (LIKELY(field != NULL)) {
    if (UNLIKELY(obj == NULL)) {
      ThrowLocation throw_location = self->GetCurrentLocationForThrow();
      ThrowNullPointerExceptionForFieldAccess(throw_location, field, false);
    } else {
      // Compiled code can't use transactional mode.
      field->Set32<false>(obj, new_value);
      return 0;  // success
    }
  }
  return -1;  // failure
}

extern "C" int artSet64InstanceFromCode(uint32_t field_idx, mirror::Object* obj, uint64_t new_value,
                                        Thread* self, mirror::ArtMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::ArtMethod* callee_save = Runtime::Current()->GetCalleeSaveMethod(Runtime::kRefsOnly);
  mirror::ArtMethod* referrer =
      sp[callee_save->GetFrameSizeInBytes() / sizeof(mirror::ArtMethod*)];
  mirror::ArtField* field = FindFieldFast(field_idx, referrer, InstancePrimitiveWrite,
                                          sizeof(int64_t));
  if (LIKELY(field != NULL  && obj != NULL)) {
    // Compiled code can't use transactional mode.
    field->Set64<false>(obj, new_value);
    return 0;  // success
  }
  *sp = callee_save;
  self->SetTopOfStack(sp, 0);
  field = FindFieldFromCode<InstancePrimitiveWrite, true>(field_idx, referrer, self,
                                                          sizeof(int64_t));
  if (LIKELY(field != NULL)) {
    if (UNLIKELY(obj == NULL)) {
      ThrowLocation throw_location = self->GetCurrentLocationForThrow();
      ThrowNullPointerExceptionForFieldAccess(throw_location, field, false);
    } else {
      // Compiled code can't use transactional mode.
      field->Set64<false>(obj, new_value);
      return 0;  // success
    }
  }
  return -1;  // failure
}

extern "C" int artSetObjInstanceFromCode(uint32_t field_idx, mirror::Object* obj,
                                         mirror::Object* new_value,
                                         mirror::ArtMethod* referrer, Thread* self,
                                         mirror::ArtMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::ArtField* field = FindFieldFast(field_idx, referrer, InstanceObjectWrite,
                                          sizeof(mirror::HeapReference<mirror::Object>));
  if (LIKELY(field != NULL && obj != NULL)) {
    // Compiled code can't use transactional mode.
    field->SetObj<false>(obj, new_value);
    return 0;  // success
  }
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  field = FindFieldFromCode<InstanceObjectWrite, true>(field_idx, referrer, self,
                                                       sizeof(mirror::HeapReference<mirror::Object>));
  if (LIKELY(field != NULL)) {
    if (UNLIKELY(obj == NULL)) {
      ThrowLocation throw_location = self->GetCurrentLocationForThrow();
      ThrowNullPointerExceptionForFieldAccess(throw_location, field, false);
    } else {
      // Compiled code can't use transactional mode.
      field->SetObj<false>(obj, new_value);
      return 0;  // success
    }
  }
  return -1;  // failure
}

}  // namespace art
