/*
 * Copyright (C) 2013 The Android Open Source Project
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

#include "object_registry.h"

#include "mirror/class.h"
#include "scoped_thread_state_change.h"

namespace art {

mirror::Object* const ObjectRegistry::kInvalidObject = reinterpret_cast<mirror::Object*>(1);

std::ostream& operator<<(std::ostream& os, const ObjectRegistryEntry& rhs) {
  os << "ObjectRegistryEntry[" << rhs.jni_reference_type
     << ",reference=" << rhs.jni_reference
     << ",count=" << rhs.reference_count
     << ",id=" << rhs.id << "]";
  return os;
}

ObjectRegistry::ObjectRegistry()
    : lock_("ObjectRegistry lock", kJdwpObjectRegistryLock), next_id_(1) {
}

JDWP::RefTypeId ObjectRegistry::AddRefType(mirror::Class* c) {
  return InternalAdd(c);
}

JDWP::ObjectId ObjectRegistry::Add(mirror::Object* o) {
  return InternalAdd(o);
}

JDWP::ObjectId ObjectRegistry::InternalAdd(mirror::Object* o) {
  if (o == nullptr) {
    return 0;
  }

  // Call IdentityHashCode here to avoid a lock level violation between lock_ and monitor_lock.
  int32_t identity_hash_code = o->IdentityHashCode();
  ScopedObjectAccessUnchecked soa(Thread::Current());
  MutexLock mu(soa.Self(), lock_);
  ObjectRegistryEntry* entry = nullptr;
  if (ContainsLocked(soa.Self(), o, identity_hash_code, &entry)) {
    // This object was already in our map.
    ++entry->reference_count;
  } else {
    entry = new ObjectRegistryEntry;
    entry->jni_reference_type = JNIWeakGlobalRefType;
    entry->jni_reference = nullptr;
    entry->reference_count = 0;
    entry->id = 0;
    entry->identity_hash_code = identity_hash_code;
    object_to_entry_.insert(std::make_pair(identity_hash_code, entry));

    // This object isn't in the registry yet, so add it.
    JNIEnv* env = soa.Env();

    jobject local_reference = soa.AddLocalReference<jobject>(o);

    entry->jni_reference_type = JNIWeakGlobalRefType;
    entry->jni_reference = env->NewWeakGlobalRef(local_reference);
    entry->reference_count = 1;
    entry->id = next_id_++;

    id_to_entry_.Put(entry->id, entry);

    env->DeleteLocalRef(local_reference);
  }
  return entry->id;
}

bool ObjectRegistry::Contains(mirror::Object* o, ObjectRegistryEntry** out_entry) {
  if (o == nullptr) {
    return false;
  }
  // Call IdentityHashCode here to avoid a lock level violation between lock_ and monitor_lock.
  int32_t identity_hash_code = o->IdentityHashCode();
  Thread* self = Thread::Current();
  MutexLock mu(self, lock_);
  return ContainsLocked(self, o, identity_hash_code, out_entry);
}

bool ObjectRegistry::ContainsLocked(Thread* self, mirror::Object* o, int32_t identity_hash_code,
                                    ObjectRegistryEntry** out_entry) {
  DCHECK(o != nullptr);
  for (auto it = object_to_entry_.lower_bound(identity_hash_code), end = object_to_entry_.end();
       it != end && it->first == identity_hash_code; ++it) {
    ObjectRegistryEntry* entry = it->second;
    if (o == self->DecodeJObject(entry->jni_reference)) {
      if (out_entry != nullptr) {
        *out_entry = entry;
      }
      return true;
    }
  }
  return false;
}

void ObjectRegistry::Clear() {
  Thread* self = Thread::Current();
  MutexLock mu(self, lock_);
  VLOG(jdwp) << "Object registry contained " << object_to_entry_.size() << " entries";
  // Delete all the JNI references.
  JNIEnv* env = self->GetJniEnv();
  for (const auto& pair : object_to_entry_) {
    const ObjectRegistryEntry* entry = pair.second;
    if (entry->jni_reference_type == JNIWeakGlobalRefType) {
      env->DeleteWeakGlobalRef(entry->jni_reference);
    } else {
      env->DeleteGlobalRef(entry->jni_reference);
    }
    delete entry;
  }
  // Clear the maps.
  object_to_entry_.clear();
  id_to_entry_.clear();
}

mirror::Object* ObjectRegistry::InternalGet(JDWP::ObjectId id) {
  Thread* self = Thread::Current();
  MutexLock mu(self, lock_);
  auto it = id_to_entry_.find(id);
  if (it == id_to_entry_.end()) {
    return kInvalidObject;
  }
  ObjectRegistryEntry& entry = *it->second;
  return self->DecodeJObject(entry.jni_reference);
}

jobject ObjectRegistry::GetJObject(JDWP::ObjectId id) {
  if (id == 0) {
    return NULL;
  }
  Thread* self = Thread::Current();
  MutexLock mu(self, lock_);
  auto it = id_to_entry_.find(id);
  CHECK(it != id_to_entry_.end()) << id;
  ObjectRegistryEntry& entry = *it->second;
  return entry.jni_reference;
}

void ObjectRegistry::DisableCollection(JDWP::ObjectId id) {
  Thread* self = Thread::Current();
  MutexLock mu(self, lock_);
  auto it = id_to_entry_.find(id);
  CHECK(it != id_to_entry_.end());
  Promote(*it->second);
}

void ObjectRegistry::EnableCollection(JDWP::ObjectId id) {
  Thread* self = Thread::Current();
  MutexLock mu(self, lock_);
  auto it = id_to_entry_.find(id);
  CHECK(it != id_to_entry_.end());
  Demote(*it->second);
}

void ObjectRegistry::Demote(ObjectRegistryEntry& entry) {
  if (entry.jni_reference_type == JNIGlobalRefType) {
    Thread* self = Thread::Current();
    JNIEnv* env = self->GetJniEnv();
    jobject global = entry.jni_reference;
    entry.jni_reference = env->NewWeakGlobalRef(entry.jni_reference);
    entry.jni_reference_type = JNIWeakGlobalRefType;
    env->DeleteGlobalRef(global);
  }
}

void ObjectRegistry::Promote(ObjectRegistryEntry& entry) {
  if (entry.jni_reference_type == JNIWeakGlobalRefType) {
    Thread* self = Thread::Current();
    JNIEnv* env = self->GetJniEnv();
    jobject weak = entry.jni_reference;
    entry.jni_reference = env->NewGlobalRef(entry.jni_reference);
    entry.jni_reference_type = JNIGlobalRefType;
    env->DeleteWeakGlobalRef(weak);
  }
}

bool ObjectRegistry::IsCollected(JDWP::ObjectId id) {
  Thread* self = Thread::Current();
  MutexLock mu(self, lock_);
  auto it = id_to_entry_.find(id);
  CHECK(it != id_to_entry_.end());
  ObjectRegistryEntry& entry = *it->second;
  if (entry.jni_reference_type == JNIWeakGlobalRefType) {
    JNIEnv* env = self->GetJniEnv();
    return env->IsSameObject(entry.jni_reference, NULL);  // Has the jweak been collected?
  } else {
    return false;  // We hold a strong reference, so we know this is live.
  }
}

void ObjectRegistry::DisposeObject(JDWP::ObjectId id, uint32_t reference_count) {
  Thread* self = Thread::Current();
  MutexLock mu(self, lock_);
  auto it = id_to_entry_.find(id);
  if (it == id_to_entry_.end()) {
    return;
  }
  ObjectRegistryEntry* entry = it->second;
  entry->reference_count -= reference_count;
  if (entry->reference_count <= 0) {
    JNIEnv* env = self->GetJniEnv();
    // Erase the object from the maps. Note object may be null if it's
    // a weak ref and the GC has cleared it.
    int32_t hash_code = entry->identity_hash_code;
    for (auto it = object_to_entry_.lower_bound(hash_code), end = object_to_entry_.end();
         it != end && it->first == hash_code; ++it) {
      if (entry == it->second) {
        object_to_entry_.erase(it);
        break;
      }
    }
    if (entry->jni_reference_type == JNIWeakGlobalRefType) {
      env->DeleteWeakGlobalRef(entry->jni_reference);
    } else {
      env->DeleteGlobalRef(entry->jni_reference);
    }
    id_to_entry_.erase(id);
    delete entry;
  }
}

}  // namespace art
