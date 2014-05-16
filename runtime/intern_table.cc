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

#include "intern_table.h"

#include "gc/space/image_space.h"
#include "mirror/dex_cache.h"
#include "mirror/object_array-inl.h"
#include "mirror/object-inl.h"
#include "mirror/string.h"
#include "thread.h"
#include "UniquePtrCompat.h"
#include "utf.h"

namespace art {

InternTable::InternTable()
    : log_new_roots_(false), allow_new_interns_(true),
      new_intern_condition_("New intern condition", *Locks::intern_table_lock_) {
}

size_t InternTable::Size() const {
  MutexLock mu(Thread::Current(), *Locks::intern_table_lock_);
  return strong_interns_.size() + weak_interns_.size();
}

void InternTable::DumpForSigQuit(std::ostream& os) const {
  MutexLock mu(Thread::Current(), *Locks::intern_table_lock_);
  os << "Intern table: " << strong_interns_.size() << " strong; "
     << weak_interns_.size() << " weak\n";
}

void InternTable::VisitRoots(RootCallback* callback, void* arg, VisitRootFlags flags) {
  MutexLock mu(Thread::Current(), *Locks::intern_table_lock_);
  if ((flags & kVisitRootFlagAllRoots) != 0) {
    for (auto& strong_intern : strong_interns_) {
      callback(reinterpret_cast<mirror::Object**>(&strong_intern.second), arg, 0,
               kRootInternedString);
      DCHECK(strong_intern.second != nullptr);
    }
  } else if ((flags & kVisitRootFlagNewRoots) != 0) {
    for (auto& pair : new_strong_intern_roots_) {
       mirror::String* old_ref = pair.second;
       callback(reinterpret_cast<mirror::Object**>(&pair.second), arg, 0, kRootInternedString);
       if (UNLIKELY(pair.second != old_ref)) {
         // Uh ohes, GC moved a root in the log. Need to search the strong interns and update the
         // corresponding object. This is slow, but luckily for us, this may only happen with a
         // concurrent moving GC.
         for (auto it = strong_interns_.lower_bound(pair.first), end = strong_interns_.end();
             it != end && it->first == pair.first; ++it) {
           // If the class stored matches the old class, update it to the new value.
           if (old_ref == it->second) {
             it->second = pair.second;
           }
         }
       }
     }
  }

  if ((flags & kVisitRootFlagClearRootLog) != 0) {
    new_strong_intern_roots_.clear();
  }
  if ((flags & kVisitRootFlagStartLoggingNewRoots) != 0) {
    log_new_roots_ = true;
  } else if ((flags & kVisitRootFlagStopLoggingNewRoots) != 0) {
    log_new_roots_ = false;
  }
  // Note: we deliberately don't visit the weak_interns_ table and the immutable image roots.
}

mirror::String* InternTable::Lookup(Table& table, mirror::String* s, int32_t hash_code) {
  Locks::intern_table_lock_->AssertHeld(Thread::Current());
  for (auto it = table.find(hash_code), end = table.end(); it != end; ++it) {
    mirror::String* existing_string = it->second;
    if (existing_string->Equals(s)) {
      return existing_string;
    }
  }
  return NULL;
}

mirror::String* InternTable::InsertStrong(mirror::String* s, int32_t hash_code) {
  Runtime* runtime = Runtime::Current();
  if (runtime->IsActiveTransaction()) {
    runtime->RecordStrongStringInsertion(s, hash_code);
  }
  if (log_new_roots_) {
    new_strong_intern_roots_.push_back(std::make_pair(hash_code, s));
  }
  strong_interns_.insert(std::make_pair(hash_code, s));
  return s;
}

mirror::String* InternTable::InsertWeak(mirror::String* s, int32_t hash_code) {
  Runtime* runtime = Runtime::Current();
  if (runtime->IsActiveTransaction()) {
    runtime->RecordWeakStringInsertion(s, hash_code);
  }
  weak_interns_.insert(std::make_pair(hash_code, s));
  return s;
}

void InternTable::RemoveWeak(mirror::String* s, int32_t hash_code) {
  Runtime* runtime = Runtime::Current();
  if (runtime->IsActiveTransaction()) {
    runtime->RecordWeakStringRemoval(s, hash_code);
  }
  Remove(weak_interns_, s, hash_code);
}

void InternTable::Remove(Table& table, mirror::String* s, int32_t hash_code) {
  for (auto it = table.find(hash_code), end = table.end(); it != end; ++it) {
    if (it->second == s) {
      table.erase(it);
      return;
    }
  }
}

// Insert/remove methods used to undo changes made during an aborted transaction.
mirror::String* InternTable::InsertStrongFromTransaction(mirror::String* s, int32_t hash_code) {
  DCHECK(!Runtime::Current()->IsActiveTransaction());
  return InsertStrong(s, hash_code);
}
mirror::String* InternTable::InsertWeakFromTransaction(mirror::String* s, int32_t hash_code) {
  DCHECK(!Runtime::Current()->IsActiveTransaction());
  return InsertWeak(s, hash_code);
}
void InternTable::RemoveStrongFromTransaction(mirror::String* s, int32_t hash_code) {
  DCHECK(!Runtime::Current()->IsActiveTransaction());
  Remove(strong_interns_, s, hash_code);
}
void InternTable::RemoveWeakFromTransaction(mirror::String* s, int32_t hash_code) {
  DCHECK(!Runtime::Current()->IsActiveTransaction());
  Remove(weak_interns_, s, hash_code);
}

static mirror::String* LookupStringFromImage(mirror::String* s)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  gc::space::ImageSpace* image = Runtime::Current()->GetHeap()->GetImageSpace();
  if (image == NULL) {
    return NULL;  // No image present.
  }
  mirror::Object* root = image->GetImageHeader().GetImageRoot(ImageHeader::kDexCaches);
  mirror::ObjectArray<mirror::DexCache>* dex_caches = root->AsObjectArray<mirror::DexCache>();
  const std::string utf8 = s->ToModifiedUtf8();
  for (int32_t i = 0; i < dex_caches->GetLength(); ++i) {
    mirror::DexCache* dex_cache = dex_caches->Get(i);
    const DexFile* dex_file = dex_cache->GetDexFile();
    // Binary search the dex file for the string index.
    const DexFile::StringId* string_id = dex_file->FindStringId(utf8.c_str());
    if (string_id != NULL) {
      uint32_t string_idx = dex_file->GetIndexForStringId(*string_id);
      mirror::String* image = dex_cache->GetResolvedString(string_idx);
      if (image != NULL) {
        return image;
      }
    }
  }
  return NULL;
}

void InternTable::AllowNewInterns() {
  Thread* self = Thread::Current();
  MutexLock mu(self, *Locks::intern_table_lock_);
  allow_new_interns_ = true;
  new_intern_condition_.Broadcast(self);
}

void InternTable::DisallowNewInterns() {
  Thread* self = Thread::Current();
  MutexLock mu(self, *Locks::intern_table_lock_);
  allow_new_interns_ = false;
}

mirror::String* InternTable::Insert(mirror::String* s, bool is_strong) {
  Thread* self = Thread::Current();
  MutexLock mu(self, *Locks::intern_table_lock_);

  DCHECK(s != NULL);
  uint32_t hash_code = s->GetHashCode();

  while (UNLIKELY(!allow_new_interns_)) {
    new_intern_condition_.WaitHoldingLocks(self);
  }

  if (is_strong) {
    // Check the strong table for a match.
    mirror::String* strong = Lookup(strong_interns_, s, hash_code);
    if (strong != NULL) {
      return strong;
    }

    // Check the image for a match.
    mirror::String* image = LookupStringFromImage(s);
    if (image != NULL) {
      return InsertStrong(image, hash_code);
    }

    // There is no match in the strong table, check the weak table.
    mirror::String* weak = Lookup(weak_interns_, s, hash_code);
    if (weak != NULL) {
      // A match was found in the weak table. Promote to the strong table.
      RemoveWeak(weak, hash_code);
      return InsertStrong(weak, hash_code);
    }

    // No match in the strong table or the weak table. Insert into the strong
    // table.
    return InsertStrong(s, hash_code);
  }

  // Check the strong table for a match.
  mirror::String* strong = Lookup(strong_interns_, s, hash_code);
  if (strong != NULL) {
    return strong;
  }
  // Check the image for a match.
  mirror::String* image = LookupStringFromImage(s);
  if (image != NULL) {
    return InsertWeak(image, hash_code);
  }
  // Check the weak table for a match.
  mirror::String* weak = Lookup(weak_interns_, s, hash_code);
  if (weak != NULL) {
    return weak;
  }
  // Insert into the weak table.
  return InsertWeak(s, hash_code);
}

mirror::String* InternTable::InternStrong(int32_t utf16_length, const char* utf8_data) {
  DCHECK(utf8_data != nullptr);
  return InternStrong(mirror::String::AllocFromModifiedUtf8(
      Thread::Current(), utf16_length, utf8_data));
}

mirror::String* InternTable::InternStrong(const char* utf8_data) {
  DCHECK(utf8_data != nullptr);
  return InternStrong(mirror::String::AllocFromModifiedUtf8(Thread::Current(), utf8_data));
}

mirror::String* InternTable::InternStrong(mirror::String* s) {
  if (s == nullptr) {
    return nullptr;
  }
  return Insert(s, true);
}

mirror::String* InternTable::InternWeak(mirror::String* s) {
  if (s == nullptr) {
    return nullptr;
  }
  return Insert(s, false);
}

bool InternTable::ContainsWeak(mirror::String* s) {
  MutexLock mu(Thread::Current(), *Locks::intern_table_lock_);
  const mirror::String* found = Lookup(weak_interns_, s, s->GetHashCode());
  return found == s;
}

void InternTable::SweepInternTableWeaks(IsMarkedCallback* callback, void* arg) {
  MutexLock mu(Thread::Current(), *Locks::intern_table_lock_);
  for (auto it = weak_interns_.begin(), end = weak_interns_.end(); it != end;) {
    mirror::Object* object = it->second;
    mirror::Object* new_object = callback(object, arg);
    if (new_object == nullptr) {
      // TODO: use it = weak_interns_.erase(it) when we get a c++11 stl.
      weak_interns_.erase(it++);
    } else {
      it->second = down_cast<mirror::String*>(new_object);
      ++it;
    }
  }
}

}  // namespace art
