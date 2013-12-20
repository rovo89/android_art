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

#include <ctime>

#include "object.h"

#include "art_field.h"
#include "art_field-inl.h"
#include "array-inl.h"
#include "class.h"
#include "class-inl.h"
#include "class_linker-inl.h"
#include "gc/accounting/card_table-inl.h"
#include "gc/heap.h"
#include "iftable-inl.h"
#include "monitor.h"
#include "object-inl.h"
#include "object_array-inl.h"
#include "object_utils.h"
#include "runtime.h"
#include "sirt_ref.h"
#include "throwable.h"
#include "well_known_classes.h"

namespace art {
namespace mirror {

static Object* CopyObject(Thread* self, mirror::Object* dest, mirror::Object* src, size_t num_bytes)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  // Copy instance data.  We assume memcpy copies by words.
  // TODO: expose and use move32.
  byte* src_bytes = reinterpret_cast<byte*>(src);
  byte* dst_bytes = reinterpret_cast<byte*>(dest);
  size_t offset = sizeof(Object);
  memcpy(dst_bytes + offset, src_bytes + offset, num_bytes - offset);
  gc::Heap* heap = Runtime::Current()->GetHeap();
  // Perform write barriers on copied object references.
  Class* c = src->GetClass();
  if (c->IsArrayClass()) {
    if (!c->GetComponentType()->IsPrimitive()) {
      const ObjectArray<Object>* array = dest->AsObjectArray<Object>();
      heap->WriteBarrierArray(dest, 0, array->GetLength());
    }
  } else {
    heap->WriteBarrierEveryFieldOf(dest);
  }
  if (c->IsFinalizable()) {
    SirtRef<Object> sirt_dest(self, dest);
    heap->AddFinalizerReference(self, dest);
    return sirt_dest.get();
  }
  return dest;
}

Object* Object::Clone(Thread* self) {
  CHECK(!IsClass()) << "Can't clone classes.";
  // Object::SizeOf gets the right size even if we're an array. Using c->AllocObject() here would
  // be wrong.
  gc::Heap* heap = Runtime::Current()->GetHeap();
  size_t num_bytes = SizeOf();
  SirtRef<Object> this_object(self, this);
  Object* copy;
  if (heap->IsMovableObject(this)) {
    copy = heap->AllocObject<true>(self, GetClass(), num_bytes);
  } else {
    copy = heap->AllocNonMovableObject<true>(self, GetClass(), num_bytes);
  }
  if (LIKELY(copy != nullptr)) {
    return CopyObject(self, copy, this_object.get(), num_bytes);
  }
  return copy;
}

int32_t Object::GenerateIdentityHashCode() {
  static AtomicInteger seed(987654321 + std::time(nullptr));
  int32_t expected_value, new_value;
  do {
    expected_value = static_cast<uint32_t>(seed.Load());
    new_value = expected_value * 1103515245 + 12345;
  } while ((expected_value & LockWord::kHashMask) == 0 ||
      !seed.CompareAndSwap(expected_value, new_value));
  return expected_value & LockWord::kHashMask;
}

int32_t Object::IdentityHashCode() const {
  mirror::Object* current_this = const_cast<mirror::Object*>(this);
  while (true) {
    LockWord lw = current_this->GetLockWord();
    switch (lw.GetState()) {
      case LockWord::kUnlocked: {
        // Try to compare and swap in a new hash, if we succeed we will return the hash on the next
        // loop iteration.
        LockWord hash_word(LockWord::FromHashCode(GenerateIdentityHashCode()));
        DCHECK_EQ(hash_word.GetState(), LockWord::kHashCode);
        if (const_cast<Object*>(this)->CasLockWord(lw, hash_word)) {
          return hash_word.GetHashCode();
        }
        break;
      }
      case LockWord::kThinLocked: {
        // Inflate the thin lock to a monitor and stick the hash code inside of the monitor.
        Thread* self = Thread::Current();
        SirtRef<mirror::Object> sirt_this(self, current_this);
        Monitor::InflateThinLocked(self, sirt_this, lw, GenerateIdentityHashCode());
        // A GC may have occurred when we switched to kBlocked.
        current_this = sirt_this.get();
        break;
      }
      case LockWord::kFatLocked: {
        // Already inflated, return the has stored in the monitor.
        Monitor* monitor = lw.FatLockMonitor();
        DCHECK(monitor != nullptr);
        return monitor->GetHashCode();
      }
      case LockWord::kHashCode: {
        return lw.GetHashCode();
      }
      default: {
        LOG(FATAL) << "Invalid state during hashcode " << lw.GetState();
        break;
      }
    }
  }
  LOG(FATAL) << "Unreachable";
  return 0;
}

void Object::CheckFieldAssignmentImpl(MemberOffset field_offset, const Object* new_value) {
  const Class* c = GetClass();
  if (Runtime::Current()->GetClassLinker() == NULL ||
      !Runtime::Current()->GetHeap()->IsObjectValidationEnabled() ||
      !c->IsResolved()) {
    return;
  }
  for (const Class* cur = c; cur != NULL; cur = cur->GetSuperClass()) {
    ObjectArray<ArtField>* fields = cur->GetIFields();
    if (fields != NULL) {
      size_t num_ref_ifields = cur->NumReferenceInstanceFields();
      for (size_t i = 0; i < num_ref_ifields; ++i) {
        ArtField* field = fields->Get(i);
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
    ObjectArray<ArtField>* fields = AsClass()->GetSFields();
    if (fields != NULL) {
      size_t num_ref_sfields = AsClass()->NumReferenceStaticFields();
      for (size_t i = 0; i < num_ref_sfields; ++i) {
        ArtField* field = fields->Get(i);
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

}  // namespace mirror
}  // namespace art
