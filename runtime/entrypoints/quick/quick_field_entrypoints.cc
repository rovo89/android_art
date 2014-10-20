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
#include "entrypoints/entrypoint_utils-inl.h"
#include "mirror/art_field-inl.h"
#include "mirror/art_method-inl.h"
#include "mirror/class-inl.h"

#include <stdint.h>

namespace art {

extern "C" int8_t artGetByteStaticFromCode(uint32_t field_idx, mirror::ArtMethod* referrer,
                                           Thread* self)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);
  mirror::ArtField* field = FindFieldFast(field_idx, referrer, StaticPrimitiveRead,
                                          sizeof(int8_t));
  if (LIKELY(field != nullptr)) {
    return field->GetByte(field->GetDeclaringClass());
  }
  field = FindFieldFromCode<StaticPrimitiveRead, true>(field_idx, referrer, self, sizeof(int8_t));
  if (LIKELY(field != nullptr)) {
    return field->GetByte(field->GetDeclaringClass());
  }
  return 0;  // Will throw exception by checking with Thread::Current.
}

extern "C" uint8_t artGetBooleanStaticFromCode(uint32_t field_idx, mirror::ArtMethod* referrer,
                                               Thread* self)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);
  mirror::ArtField* field = FindFieldFast(field_idx, referrer, StaticPrimitiveRead,
                                          sizeof(int8_t));
  if (LIKELY(field != nullptr)) {
    return field->GetBoolean(field->GetDeclaringClass());
  }
  field = FindFieldFromCode<StaticPrimitiveRead, true>(field_idx, referrer, self, sizeof(int8_t));
  if (LIKELY(field != nullptr)) {
    return field->GetBoolean(field->GetDeclaringClass());
  }
  return 0;  // Will throw exception by checking with Thread::Current.
}

extern "C" int16_t artGetShortStaticFromCode(uint32_t field_idx, mirror::ArtMethod* referrer,
                                             Thread* self)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);
  mirror::ArtField* field = FindFieldFast(field_idx, referrer, StaticPrimitiveRead,
                                          sizeof(int16_t));
  if (LIKELY(field != nullptr)) {
    return field->GetShort(field->GetDeclaringClass());
  }
  field = FindFieldFromCode<StaticPrimitiveRead, true>(field_idx, referrer, self, sizeof(int16_t));
  if (LIKELY(field != nullptr)) {
    return field->GetShort(field->GetDeclaringClass());
  }
  return 0;  // Will throw exception by checking with Thread::Current.
}

extern "C" uint16_t artGetCharStaticFromCode(uint32_t field_idx,
                                             mirror::ArtMethod* referrer,
                                             Thread* self)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);
  mirror::ArtField* field = FindFieldFast(field_idx, referrer, StaticPrimitiveRead,
                                          sizeof(int16_t));
  if (LIKELY(field != nullptr)) {
    return field->GetChar(field->GetDeclaringClass());
  }
  field = FindFieldFromCode<StaticPrimitiveRead, true>(field_idx, referrer, self, sizeof(int16_t));
  if (LIKELY(field != nullptr)) {
    return field->GetChar(field->GetDeclaringClass());
  }
  return 0;  // Will throw exception by checking with Thread::Current.
}

extern "C" uint32_t artGet32StaticFromCode(uint32_t field_idx,
                                           mirror::ArtMethod* referrer,
                                           Thread* self)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);
  mirror::ArtField* field = FindFieldFast(field_idx, referrer, StaticPrimitiveRead,
                                          sizeof(int32_t));
  if (LIKELY(field != nullptr)) {
    return field->Get32(field->GetDeclaringClass());
  }
  field = FindFieldFromCode<StaticPrimitiveRead, true>(field_idx, referrer, self, sizeof(int32_t));
  if (LIKELY(field != nullptr)) {
    return field->Get32(field->GetDeclaringClass());
  }
  return 0;  // Will throw exception by checking with Thread::Current.
}

extern "C" uint64_t artGet64StaticFromCode(uint32_t field_idx,
                                           mirror::ArtMethod* referrer,
                                           Thread* self)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);
  mirror::ArtField* field = FindFieldFast(field_idx, referrer, StaticPrimitiveRead,
                                          sizeof(int64_t));
  if (LIKELY(field != nullptr)) {
    return field->Get64(field->GetDeclaringClass());
  }
  field = FindFieldFromCode<StaticPrimitiveRead, true>(field_idx, referrer, self, sizeof(int64_t));
  if (LIKELY(field != nullptr)) {
    return field->Get64(field->GetDeclaringClass());
  }
  return 0;  // Will throw exception by checking with Thread::Current.
}

extern "C" mirror::Object* artGetObjStaticFromCode(uint32_t field_idx,
                                                   mirror::ArtMethod* referrer,
                                                   Thread* self)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);
  mirror::ArtField* field = FindFieldFast(field_idx, referrer, StaticObjectRead,
                                          sizeof(mirror::HeapReference<mirror::Object>));
  if (LIKELY(field != nullptr)) {
    return field->GetObj(field->GetDeclaringClass());
  }
  field = FindFieldFromCode<StaticObjectRead, true>(field_idx, referrer, self,
                                                    sizeof(mirror::HeapReference<mirror::Object>));
  if (LIKELY(field != nullptr)) {
    return field->GetObj(field->GetDeclaringClass());
  }
  return nullptr;  // Will throw exception by checking with Thread::Current.
}

extern "C" int8_t artGetByteInstanceFromCode(uint32_t field_idx, mirror::Object* obj,
                                             mirror::ArtMethod* referrer, Thread* self)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);
  mirror::ArtField* field = FindFieldFast(field_idx, referrer, InstancePrimitiveRead,
                                          sizeof(int8_t));
  if (LIKELY(field != nullptr && obj != nullptr)) {
    return field->GetByte(obj);
  }
  field = FindFieldFromCode<InstancePrimitiveRead, true>(field_idx, referrer, self,
                                                         sizeof(int8_t));
  if (LIKELY(field != nullptr)) {
    if (UNLIKELY(obj == nullptr)) {
      ThrowLocation throw_location = self->GetCurrentLocationForThrow();
      ThrowNullPointerExceptionForFieldAccess(throw_location, field, true);
    } else {
      return field->GetByte(obj);
    }
  }
  return 0;  // Will throw exception by checking with Thread::Current.
}

extern "C" uint8_t artGetBooleanInstanceFromCode(uint32_t field_idx, mirror::Object* obj,
                                                 mirror::ArtMethod* referrer, Thread* self)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);
  mirror::ArtField* field = FindFieldFast(field_idx, referrer, InstancePrimitiveRead,
                                          sizeof(int8_t));
  if (LIKELY(field != nullptr && obj != nullptr)) {
    return field->GetBoolean(obj);
  }
  field = FindFieldFromCode<InstancePrimitiveRead, true>(field_idx, referrer, self,
                                                         sizeof(int8_t));
  if (LIKELY(field != nullptr)) {
    if (UNLIKELY(obj == nullptr)) {
      ThrowLocation throw_location = self->GetCurrentLocationForThrow();
      ThrowNullPointerExceptionForFieldAccess(throw_location, field, true);
    } else {
      return field->GetBoolean(obj);
    }
  }
  return 0;  // Will throw exception by checking with Thread::Current.
}
extern "C" int16_t artGetShortInstanceFromCode(uint32_t field_idx, mirror::Object* obj,
                                               mirror::ArtMethod* referrer, Thread* self)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);
  mirror::ArtField* field = FindFieldFast(field_idx, referrer, InstancePrimitiveRead,
                                          sizeof(int16_t));
  if (LIKELY(field != nullptr && obj != nullptr)) {
    return field->GetShort(obj);
  }
  field = FindFieldFromCode<InstancePrimitiveRead, true>(field_idx, referrer, self,
                                                         sizeof(int16_t));
  if (LIKELY(field != nullptr)) {
    if (UNLIKELY(obj == nullptr)) {
      ThrowLocation throw_location = self->GetCurrentLocationForThrow();
      ThrowNullPointerExceptionForFieldAccess(throw_location, field, true);
    } else {
      return field->GetShort(obj);
    }
  }
  return 0;  // Will throw exception by checking with Thread::Current.
}

extern "C" uint16_t artGetCharInstanceFromCode(uint32_t field_idx, mirror::Object* obj,
                                               mirror::ArtMethod* referrer, Thread* self)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);
  mirror::ArtField* field = FindFieldFast(field_idx, referrer, InstancePrimitiveRead,
                                          sizeof(int16_t));
  if (LIKELY(field != nullptr && obj != nullptr)) {
    return field->GetChar(obj);
  }
  field = FindFieldFromCode<InstancePrimitiveRead, true>(field_idx, referrer, self,
                                                         sizeof(int16_t));
  if (LIKELY(field != nullptr)) {
    if (UNLIKELY(obj == nullptr)) {
      ThrowLocation throw_location = self->GetCurrentLocationForThrow();
      ThrowNullPointerExceptionForFieldAccess(throw_location, field, true);
    } else {
      return field->GetChar(obj);
    }
  }
  return 0;  // Will throw exception by checking with Thread::Current.
}

extern "C" uint32_t artGet32InstanceFromCode(uint32_t field_idx, mirror::Object* obj,
                                             mirror::ArtMethod* referrer, Thread* self)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);
  mirror::ArtField* field = FindFieldFast(field_idx, referrer, InstancePrimitiveRead,
                                          sizeof(int32_t));
  if (LIKELY(field != nullptr && obj != nullptr)) {
    return field->Get32(obj);
  }
  field = FindFieldFromCode<InstancePrimitiveRead, true>(field_idx, referrer, self,
                                                         sizeof(int32_t));
  if (LIKELY(field != nullptr)) {
    if (UNLIKELY(obj == nullptr)) {
      ThrowLocation throw_location = self->GetCurrentLocationForThrow();
      ThrowNullPointerExceptionForFieldAccess(throw_location, field, true);
    } else {
      return field->Get32(obj);
    }
  }
  return 0;  // Will throw exception by checking with Thread::Current.
}

extern "C" uint64_t artGet64InstanceFromCode(uint32_t field_idx, mirror::Object* obj,
                                             mirror::ArtMethod* referrer, Thread* self)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);
  mirror::ArtField* field = FindFieldFast(field_idx, referrer, InstancePrimitiveRead,
                                          sizeof(int64_t));
  if (LIKELY(field != nullptr && obj != nullptr)) {
    return field->Get64(obj);
  }
  field = FindFieldFromCode<InstancePrimitiveRead, true>(field_idx, referrer, self,
                                                         sizeof(int64_t));
  if (LIKELY(field != nullptr)) {
    if (UNLIKELY(obj == nullptr)) {
      ThrowLocation throw_location = self->GetCurrentLocationForThrow();
      ThrowNullPointerExceptionForFieldAccess(throw_location, field, true);
    } else {
      return field->Get64(obj);
    }
  }
  return 0;  // Will throw exception by checking with Thread::Current.
}

extern "C" mirror::Object* artGetObjInstanceFromCode(uint32_t field_idx, mirror::Object* obj,
                                                     mirror::ArtMethod* referrer,
                                                     Thread* self)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);
  mirror::ArtField* field = FindFieldFast(field_idx, referrer, InstanceObjectRead,
                                          sizeof(mirror::HeapReference<mirror::Object>));
  if (LIKELY(field != nullptr && obj != nullptr)) {
    return field->GetObj(obj);
  }
  field = FindFieldFromCode<InstanceObjectRead, true>(field_idx, referrer, self,
                                                      sizeof(mirror::HeapReference<mirror::Object>));
  if (LIKELY(field != nullptr)) {
    if (UNLIKELY(obj == nullptr)) {
      ThrowLocation throw_location = self->GetCurrentLocationForThrow();
      ThrowNullPointerExceptionForFieldAccess(throw_location, field, true);
    } else {
      return field->GetObj(obj);
    }
  }
  return nullptr;  // Will throw exception by checking with Thread::Current.
}

extern "C" int artSet8StaticFromCode(uint32_t field_idx, uint32_t new_value,
                                     mirror::ArtMethod* referrer, Thread* self)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);
  mirror::ArtField* field = FindFieldFast(field_idx, referrer, StaticPrimitiveWrite,
                                          sizeof(int8_t));
  if (LIKELY(field != nullptr)) {
    Primitive::Type type = field->GetTypeAsPrimitiveType();
    // Compiled code can't use transactional mode.
    if (type == Primitive::kPrimBoolean) {
      field->SetBoolean<false>(field->GetDeclaringClass(), new_value);
    } else {
      DCHECK_EQ(Primitive::kPrimByte, type);
      field->SetByte<false>(field->GetDeclaringClass(), new_value);
    }
    return 0;  // success
  }
  field = FindFieldFromCode<StaticPrimitiveWrite, true>(field_idx, referrer, self, sizeof(int8_t));
  if (LIKELY(field != nullptr)) {
    Primitive::Type type = field->GetTypeAsPrimitiveType();
    // Compiled code can't use transactional mode.
    if (type == Primitive::kPrimBoolean) {
      field->SetBoolean<false>(field->GetDeclaringClass(), new_value);
    } else {
      DCHECK_EQ(Primitive::kPrimByte, type);
      field->SetByte<false>(field->GetDeclaringClass(), new_value);
    }
    return 0;  // success
  }
  return -1;  // failure
}

extern "C" int artSet16StaticFromCode(uint32_t field_idx, uint16_t new_value,
                                      mirror::ArtMethod* referrer, Thread* self)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);
  mirror::ArtField* field = FindFieldFast(field_idx, referrer, StaticPrimitiveWrite,
                                          sizeof(int16_t));
  if (LIKELY(field != nullptr)) {
    Primitive::Type type = field->GetTypeAsPrimitiveType();
    // Compiled code can't use transactional mode.
    if (type == Primitive::kPrimChar) {
      field->SetChar<false>(field->GetDeclaringClass(), new_value);
    } else {
      DCHECK_EQ(Primitive::kPrimShort, type);
      field->SetShort<false>(field->GetDeclaringClass(), new_value);
    }
    return 0;  // success
  }
  field = FindFieldFromCode<StaticPrimitiveWrite, true>(field_idx, referrer, self, sizeof(int16_t));
  if (LIKELY(field != nullptr)) {
    Primitive::Type type = field->GetTypeAsPrimitiveType();
    // Compiled code can't use transactional mode.
    if (type == Primitive::kPrimChar) {
      field->SetChar<false>(field->GetDeclaringClass(), new_value);
    } else {
      DCHECK_EQ(Primitive::kPrimShort, type);
      field->SetShort<false>(field->GetDeclaringClass(), new_value);
    }
    return 0;  // success
  }
  return -1;  // failure
}

extern "C" int artSet32StaticFromCode(uint32_t field_idx, uint32_t new_value,
                                      mirror::ArtMethod* referrer, Thread* self)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);
  mirror::ArtField* field = FindFieldFast(field_idx, referrer, StaticPrimitiveWrite,
                                          sizeof(int32_t));
  if (LIKELY(field != nullptr)) {
    // Compiled code can't use transactional mode.
    field->Set32<false>(field->GetDeclaringClass(), new_value);
    return 0;  // success
  }
  field = FindFieldFromCode<StaticPrimitiveWrite, true>(field_idx, referrer, self, sizeof(int32_t));
  if (LIKELY(field != nullptr)) {
    // Compiled code can't use transactional mode.
    field->Set32<false>(field->GetDeclaringClass(), new_value);
    return 0;  // success
  }
  return -1;  // failure
}

extern "C" int artSet64StaticFromCode(uint32_t field_idx, mirror::ArtMethod* referrer,
                                      uint64_t new_value, Thread* self)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);
  mirror::ArtField* field = FindFieldFast(field_idx, referrer, StaticPrimitiveWrite,
                                          sizeof(int64_t));
  if (LIKELY(field != nullptr)) {
    // Compiled code can't use transactional mode.
    field->Set64<false>(field->GetDeclaringClass(), new_value);
    return 0;  // success
  }
  field = FindFieldFromCode<StaticPrimitiveWrite, true>(field_idx, referrer, self, sizeof(int64_t));
  if (LIKELY(field != nullptr)) {
    // Compiled code can't use transactional mode.
    field->Set64<false>(field->GetDeclaringClass(), new_value);
    return 0;  // success
  }
  return -1;  // failure
}

extern "C" int artSetObjStaticFromCode(uint32_t field_idx, mirror::Object* new_value,
                                       mirror::ArtMethod* referrer, Thread* self)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);
  mirror::ArtField* field = FindFieldFast(field_idx, referrer, StaticObjectWrite,
                                          sizeof(mirror::HeapReference<mirror::Object>));
  if (LIKELY(field != nullptr)) {
    if (LIKELY(!field->IsPrimitiveType())) {
      // Compiled code can't use transactional mode.
      field->SetObj<false>(field->GetDeclaringClass(), new_value);
      return 0;  // success
    }
  }
  field = FindFieldFromCode<StaticObjectWrite, true>(field_idx, referrer, self,
                                                     sizeof(mirror::HeapReference<mirror::Object>));
  if (LIKELY(field != nullptr)) {
    // Compiled code can't use transactional mode.
    field->SetObj<false>(field->GetDeclaringClass(), new_value);
    return 0;  // success
  }
  return -1;  // failure
}

extern "C" int artSet8InstanceFromCode(uint32_t field_idx, mirror::Object* obj, uint8_t new_value,
                                       mirror::ArtMethod* referrer, Thread* self)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);
  mirror::ArtField* field = FindFieldFast(field_idx, referrer, InstancePrimitiveWrite,
                                          sizeof(int8_t));
  if (LIKELY(field != nullptr && obj != nullptr)) {
    Primitive::Type type = field->GetTypeAsPrimitiveType();
    // Compiled code can't use transactional mode.
    if (type == Primitive::kPrimBoolean) {
      field->SetBoolean<false>(obj, new_value);
    } else {
      DCHECK_EQ(Primitive::kPrimByte, type);
      field->SetByte<false>(obj, new_value);
    }
    return 0;  // success
  }
  {
    StackHandleScope<1> hs(self);
    HandleWrapper<mirror::Object> h_obj(hs.NewHandleWrapper(&obj));
    field = FindFieldFromCode<InstancePrimitiveWrite, true>(field_idx, referrer, self,
                                                            sizeof(int8_t));
  }
  if (LIKELY(field != nullptr)) {
    if (UNLIKELY(obj == nullptr)) {
      ThrowLocation throw_location = self->GetCurrentLocationForThrow();
      ThrowNullPointerExceptionForFieldAccess(throw_location, field, false);
    } else {
      Primitive::Type type = field->GetTypeAsPrimitiveType();
      // Compiled code can't use transactional mode.
      if (type == Primitive::kPrimBoolean) {
        field->SetBoolean<false>(obj, new_value);
      } else {
        field->SetByte<false>(obj, new_value);
      }
      return 0;  // success
    }
  }
  return -1;  // failure
}

extern "C" int artSet16InstanceFromCode(uint32_t field_idx, mirror::Object* obj, uint16_t new_value,
                                        mirror::ArtMethod* referrer, Thread* self)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);
  mirror::ArtField* field = FindFieldFast(field_idx, referrer, InstancePrimitiveWrite,
                                          sizeof(int16_t));
  if (LIKELY(field != nullptr && obj != nullptr)) {
    Primitive::Type type = field->GetTypeAsPrimitiveType();
    // Compiled code can't use transactional mode.
    if (type == Primitive::kPrimChar) {
      field->SetChar<false>(obj, new_value);
    } else {
      DCHECK_EQ(Primitive::kPrimShort, type);
      field->SetShort<false>(obj, new_value);
    }
    return 0;  // success
  }
  {
    StackHandleScope<1> hs(self);
    HandleWrapper<mirror::Object> h_obj(hs.NewHandleWrapper(&obj));
    field = FindFieldFromCode<InstancePrimitiveWrite, true>(field_idx, referrer, self,
                                                            sizeof(int16_t));
  }
  if (LIKELY(field != nullptr)) {
    if (UNLIKELY(obj == nullptr)) {
      ThrowLocation throw_location = self->GetCurrentLocationForThrow();
      ThrowNullPointerExceptionForFieldAccess(throw_location, field, false);
    } else {
      Primitive::Type type = field->GetTypeAsPrimitiveType();
      // Compiled code can't use transactional mode.
      if (type == Primitive::kPrimChar) {
        field->SetChar<false>(obj, new_value);
      } else {
        DCHECK_EQ(Primitive::kPrimShort, type);
        field->SetShort<false>(obj, new_value);
      }
      return 0;  // success
    }
  }
  return -1;  // failure
}

extern "C" int artSet32InstanceFromCode(uint32_t field_idx, mirror::Object* obj, uint32_t new_value,
                                        mirror::ArtMethod* referrer, Thread* self)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);
  mirror::ArtField* field = FindFieldFast(field_idx, referrer, InstancePrimitiveWrite,
                                          sizeof(int32_t));
  if (LIKELY(field != nullptr && obj != nullptr)) {
    // Compiled code can't use transactional mode.
    field->Set32<false>(obj, new_value);
    return 0;  // success
  }
  {
    StackHandleScope<1> hs(self);
    HandleWrapper<mirror::Object> h_obj(hs.NewHandleWrapper(&obj));
    field = FindFieldFromCode<InstancePrimitiveWrite, true>(field_idx, referrer, self,
                                                            sizeof(int32_t));
  }
  if (LIKELY(field != nullptr)) {
    if (UNLIKELY(obj == nullptr)) {
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
                                        mirror::ArtMethod* referrer, Thread* self)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);
  mirror::ArtField* field = FindFieldFast(field_idx, referrer, InstancePrimitiveWrite,
                                          sizeof(int64_t));
  if (LIKELY(field != nullptr  && obj != nullptr)) {
    // Compiled code can't use transactional mode.
    field->Set64<false>(obj, new_value);
    return 0;  // success
  }
  field = FindFieldFromCode<InstancePrimitiveWrite, true>(field_idx, referrer, self,
                                                          sizeof(int64_t));
  if (LIKELY(field != nullptr)) {
    if (UNLIKELY(obj == nullptr)) {
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
                                         mirror::ArtMethod* referrer, Thread* self)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);
  mirror::ArtField* field = FindFieldFast(field_idx, referrer, InstanceObjectWrite,
                                          sizeof(mirror::HeapReference<mirror::Object>));
  if (LIKELY(field != nullptr && obj != nullptr)) {
    // Compiled code can't use transactional mode.
    field->SetObj<false>(obj, new_value);
    return 0;  // success
  }
  field = FindFieldFromCode<InstanceObjectWrite, true>(field_idx, referrer, self,
                                                       sizeof(mirror::HeapReference<mirror::Object>));
  if (LIKELY(field != nullptr)) {
    if (UNLIKELY(obj == nullptr)) {
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
