/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "array-inl.h"
#include "class.h"
#include "class-inl.h"
#include "field.h"
#include "field-inl.h"
#include "gc/card_table-inl.h"
#include "heap.h"
#include "monitor.h"
#include "object-inl.h"
#include "object_array.h"
#include "object_utils.h"
#include "runtime.h"
#include "sirt_ref.h"
#include "throwable.h"
#include "well_known_classes.h"

namespace art {
namespace mirror {

Array* Object::AsArray() {
  DCHECK(IsArrayInstance());
  return down_cast<Array*>(this);
}

const Array* Object::AsArray() const {
  DCHECK(IsArrayInstance());
  return down_cast<const Array*>(this);
}

BooleanArray* Object::AsBooleanArray() {
  DCHECK(GetClass()->IsArrayClass());
  DCHECK(GetClass()->GetComponentType()->IsPrimitiveBoolean());
  return down_cast<BooleanArray*>(this);
}

ByteArray* Object::AsByteArray() {
  DCHECK(GetClass()->IsArrayClass());
  DCHECK(GetClass()->GetComponentType()->IsPrimitiveByte());
  return down_cast<ByteArray*>(this);
}

CharArray* Object::AsCharArray() {
  DCHECK(GetClass()->IsArrayClass());
  DCHECK(GetClass()->GetComponentType()->IsPrimitiveChar());
  return down_cast<CharArray*>(this);
}

ShortArray* Object::AsShortArray() {
  DCHECK(GetClass()->IsArrayClass());
  DCHECK(GetClass()->GetComponentType()->IsPrimitiveShort());
  return down_cast<ShortArray*>(this);
}

IntArray* Object::AsIntArray() {
  DCHECK(GetClass()->IsArrayClass());
  DCHECK(GetClass()->GetComponentType()->IsPrimitiveInt() ||
         GetClass()->GetComponentType()->IsPrimitiveFloat());
  return down_cast<IntArray*>(this);
}

LongArray* Object::AsLongArray() {
  DCHECK(GetClass()->IsArrayClass());
  DCHECK(GetClass()->GetComponentType()->IsPrimitiveLong() ||
         GetClass()->GetComponentType()->IsPrimitiveDouble());
  return down_cast<LongArray*>(this);
}

String* Object::AsString() {
  DCHECK(GetClass()->IsStringClass());
  return down_cast<String*>(this);
}

Throwable* Object::AsThrowable() {
  DCHECK(GetClass()->IsThrowableClass());
  return down_cast<Throwable*>(this);
}

Object* Object::Clone(Thread* self) {
  Class* c = GetClass();
  DCHECK(!c->IsClassClass());

  // Object::SizeOf gets the right size even if we're an array.
  // Using c->AllocObject() here would be wrong.
  size_t num_bytes = SizeOf();
  Heap* heap = Runtime::Current()->GetHeap();
  SirtRef<Object> copy(self, heap->AllocObject(self, c, num_bytes));
  if (copy.get() == NULL) {
    return NULL;
  }

  // Copy instance data.  We assume memcpy copies by words.
  // TODO: expose and use move32.
  byte* src_bytes = reinterpret_cast<byte*>(this);
  byte* dst_bytes = reinterpret_cast<byte*>(copy.get());
  size_t offset = sizeof(Object);
  memcpy(dst_bytes + offset, src_bytes + offset, num_bytes - offset);

  // Perform write barriers on copied object references.
  if (c->IsArrayClass()) {
    if (!c->GetComponentType()->IsPrimitive()) {
      const ObjectArray<Object>* array = copy->AsObjectArray<Object>();
      heap->WriteBarrierArray(copy.get(), 0, array->GetLength());
    }
  } else {
    for (const Class* klass = c; klass != NULL; klass = klass->GetSuperClass()) {
      size_t num_reference_fields = klass->NumReferenceInstanceFields();
      for (size_t i = 0; i < num_reference_fields; ++i) {
        Field* field = klass->GetInstanceField(i);
        MemberOffset field_offset = field->GetOffset();
        const Object* ref = copy->GetFieldObject<const Object*>(field_offset, false);
        heap->WriteBarrierField(copy.get(), field_offset, ref);
      }
    }
  }

  if (c->IsFinalizable()) {
    heap->AddFinalizerReference(Thread::Current(), copy.get());
  }

  return copy.get();
}

uint32_t Object::GetThinLockId() {
  return Monitor::GetThinLockId(monitor_);
}

void Object::MonitorEnter(Thread* thread) {
  Monitor::MonitorEnter(thread, this);
}

bool Object::MonitorExit(Thread* thread) {
  return Monitor::MonitorExit(thread, this);
}

void Object::Notify() {
  Monitor::Notify(Thread::Current(), this);
}

void Object::NotifyAll() {
  Monitor::NotifyAll(Thread::Current(), this);
}

void Object::Wait() {
  Monitor::Wait(Thread::Current(), this, 0, 0, true, kWaiting);
}

void Object::Wait(int64_t ms, int32_t ns) {
  Monitor::Wait(Thread::Current(), this, ms, ns, true, kTimedWaiting);
}

#if VERIFY_OBJECT_ENABLED
void Object::CheckFieldAssignment(MemberOffset field_offset, const Object* new_value) {
  const Class* c = GetClass();
  if (Runtime::Current()->GetClassLinker() == NULL ||
      !Runtime::Current()->GetHeap()->IsObjectValidationEnabled() ||
      !c->IsResolved()) {
    return;
  }
  for (const Class* cur = c; cur != NULL; cur = cur->GetSuperClass()) {
    ObjectArray<Field>* fields = cur->GetIFields();
    if (fields != NULL) {
      size_t num_ref_ifields = cur->NumReferenceInstanceFields();
      for (size_t i = 0; i < num_ref_ifields; ++i) {
        Field* field = fields->Get(i);
        if (field->GetOffset().Int32Value() == field_offset.Int32Value()) {
          FieldHelper fh(field);
          CHECK(fh.GetType()->IsAssignableFrom(new_value->GetClass()));
          return;
        }
      }
    }
  }
  if (c->IsArrayClass()) {
    // Bounds and assign-ability done in the array setter.
    return;
  }
  if (IsClass()) {
    ObjectArray<Field>* fields = AsClass()->GetSFields();
    if (fields != NULL) {
      size_t num_ref_sfields = AsClass()->NumReferenceStaticFields();
      for (size_t i = 0; i < num_ref_sfields; ++i) {
        Field* field = fields->Get(i);
        if (field->GetOffset().Int32Value() == field_offset.Int32Value()) {
          FieldHelper fh(field);
          CHECK(fh.GetType()->IsAssignableFrom(new_value->GetClass()));
          return;
        }
      }
    }
  }
  LOG(FATAL) << "Failed to find field for assignment to " << reinterpret_cast<void*>(this)
      << " of type " << PrettyDescriptor(c) << " at offset " << field_offset;
}
#endif

}  // namespace mirror
}  // namespace art
