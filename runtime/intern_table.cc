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

#include <memory>

#include "gc_root-inl.h"
#include "gc/space/image_space.h"
#include "mirror/dex_cache.h"
#include "mirror/object_array-inl.h"
#include "mirror/object-inl.h"
#include "mirror/string-inl.h"
#include "thread.h"
#include "utf.h"

namespace art {

InternTable::InternTable()
    : image_added_to_intern_table_(false), log_new_roots_(false),
      allow_new_interns_(true),
      new_intern_condition_("New intern condition", *Locks::intern_table_lock_) {
}

size_t InternTable::Size() const {
  MutexLock mu(Thread::Current(), *Locks::intern_table_lock_);
  return strong_interns_.Size() + weak_interns_.Size();
}

size_t InternTable::StrongSize() const {
  MutexLock mu(Thread::Current(), *Locks::intern_table_lock_);
  return strong_interns_.Size();
}

size_t InternTable::WeakSize() const {
  MutexLock mu(Thread::Current(), *Locks::intern_table_lock_);
  return weak_interns_.Size();
}

void InternTable::DumpForSigQuit(std::ostream& os) const {
  os << "Intern table: " << StrongSize() << " strong; " << WeakSize() << " weak\n";
}

void InternTable::VisitRoots(RootVisitor* visitor, VisitRootFlags flags) {
  MutexLock mu(Thread::Current(), *Locks::intern_table_lock_);
  if ((flags & kVisitRootFlagAllRoots) != 0) {
    strong_interns_.VisitRoots(visitor);
  } else if ((flags & kVisitRootFlagNewRoots) != 0) {
    for (auto& root : new_strong_intern_roots_) {
      mirror::String* old_ref = root.Read<kWithoutReadBarrier>();
      root.VisitRoot(visitor, RootInfo(kRootInternedString));
      mirror::String* new_ref = root.Read<kWithoutReadBarrier>();
      if (new_ref != old_ref) {
        // The GC moved a root in the log. Need to search the strong interns and update the
        // corresponding object. This is slow, but luckily for us, this may only happen with a
        // concurrent moving GC.
        strong_interns_.Remove(old_ref);
        strong_interns_.Insert(new_ref);
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

mirror::String* InternTable::LookupStrong(mirror::String* s) {
  return strong_interns_.Find(s);
}

mirror::String* InternTable::LookupWeak(mirror::String* s) {
  return weak_interns_.Find(s);
}

void InternTable::SwapPostZygoteWithPreZygote() {
  MutexLock mu(Thread::Current(), *Locks::intern_table_lock_);
  weak_interns_.SwapPostZygoteWithPreZygote();
  strong_interns_.SwapPostZygoteWithPreZygote();
}

mirror::String* InternTable::InsertStrong(mirror::String* s) {
  Runtime* runtime = Runtime::Current();
  if (runtime->IsActiveTransaction()) {
    runtime->RecordStrongStringInsertion(s);
  }
  if (log_new_roots_) {
    new_strong_intern_roots_.push_back(GcRoot<mirror::String>(s));
  }
  strong_interns_.Insert(s);
  return s;
}

mirror::String* InternTable::InsertWeak(mirror::String* s) {
  Runtime* runtime = Runtime::Current();
  if (runtime->IsActiveTransaction()) {
    runtime->RecordWeakStringInsertion(s);
  }
  weak_interns_.Insert(s);
  return s;
}

void InternTable::RemoveStrong(mirror::String* s) {
  strong_interns_.Remove(s);
}

void InternTable::RemoveWeak(mirror::String* s) {
  Runtime* runtime = Runtime::Current();
  if (runtime->IsActiveTransaction()) {
    runtime->RecordWeakStringRemoval(s);
  }
  weak_interns_.Remove(s);
}

// Insert/remove methods used to undo changes made during an aborted transaction.
mirror::String* InternTable::InsertStrongFromTransaction(mirror::String* s) {
  DCHECK(!Runtime::Current()->IsActiveTransaction());
  return InsertStrong(s);
}
mirror::String* InternTable::InsertWeakFromTransaction(mirror::String* s) {
  DCHECK(!Runtime::Current()->IsActiveTransaction());
  return InsertWeak(s);
}
void InternTable::RemoveStrongFromTransaction(mirror::String* s) {
  DCHECK(!Runtime::Current()->IsActiveTransaction());
  RemoveStrong(s);
}
void InternTable::RemoveWeakFromTransaction(mirror::String* s) {
  DCHECK(!Runtime::Current()->IsActiveTransaction());
  RemoveWeak(s);
}

void InternTable::AddImageStringsToTable(gc::space::ImageSpace* image_space) {
  CHECK(image_space != nullptr);
  MutexLock mu(Thread::Current(), *Locks::intern_table_lock_);
  if (!image_added_to_intern_table_) {
    const ImageHeader* const header = &image_space->GetImageHeader();
    // Check if we have the interned strings section.
    const ImageSection& section = header->GetImageSection(ImageHeader::kSectionInternedStrings);
    if (section.Size() > 0) {
      ReadFromMemoryLocked(image_space->Begin() + section.Offset());
    } else {
      // TODO: Delete this logic?
      mirror::Object* root = header->GetImageRoot(ImageHeader::kDexCaches);
      mirror::ObjectArray<mirror::DexCache>* dex_caches = root->AsObjectArray<mirror::DexCache>();
      for (int32_t i = 0; i < dex_caches->GetLength(); ++i) {
        mirror::DexCache* dex_cache = dex_caches->Get(i);
        const DexFile* dex_file = dex_cache->GetDexFile();
        const size_t num_strings = dex_file->NumStringIds();
        for (size_t j = 0; j < num_strings; ++j) {
          mirror::String* image_string = dex_cache->GetResolvedString(j);
          if (image_string != nullptr) {
            mirror::String* found = LookupStrong(image_string);
            if (found == nullptr) {
              InsertStrong(image_string);
            } else {
              DCHECK_EQ(found, image_string);
            }
          }
        }
      }
    }
    image_added_to_intern_table_ = true;
  }
}

mirror::String* InternTable::LookupStringFromImage(mirror::String* s)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (image_added_to_intern_table_) {
    return nullptr;
  }
  gc::space::ImageSpace* image = Runtime::Current()->GetHeap()->GetImageSpace();
  if (image == nullptr) {
    return nullptr;  // No image present.
  }
  mirror::Object* root = image->GetImageHeader().GetImageRoot(ImageHeader::kDexCaches);
  mirror::ObjectArray<mirror::DexCache>* dex_caches = root->AsObjectArray<mirror::DexCache>();
  const std::string utf8 = s->ToModifiedUtf8();
  for (int32_t i = 0; i < dex_caches->GetLength(); ++i) {
    mirror::DexCache* dex_cache = dex_caches->Get(i);
    const DexFile* dex_file = dex_cache->GetDexFile();
    // Binary search the dex file for the string index.
    const DexFile::StringId* string_id = dex_file->FindStringId(utf8.c_str());
    if (string_id != nullptr) {
      uint32_t string_idx = dex_file->GetIndexForStringId(*string_id);
      // GetResolvedString() contains a RB.
      mirror::String* image_string = dex_cache->GetResolvedString(string_idx);
      if (image_string != nullptr) {
        return image_string;
      }
    }
  }
  return nullptr;
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

void InternTable::EnsureNewInternsDisallowed() {
  // Lock and unlock once to ensure that no threads are still in the
  // middle of adding new interns.
  MutexLock mu(Thread::Current(), *Locks::intern_table_lock_);
  CHECK(!allow_new_interns_);
}

mirror::String* InternTable::Insert(mirror::String* s, bool is_strong) {
  if (s == nullptr) {
    return nullptr;
  }
  Thread* self = Thread::Current();
  MutexLock mu(self, *Locks::intern_table_lock_);
  while (UNLIKELY(!allow_new_interns_)) {
    new_intern_condition_.WaitHoldingLocks(self);
  }
  // Check the strong table for a match.
  mirror::String* strong = LookupStrong(s);
  if (strong != nullptr) {
    return strong;
  }
  // There is no match in the strong table, check the weak table.
  mirror::String* weak = LookupWeak(s);
  if (weak != nullptr) {
    if (is_strong) {
      // A match was found in the weak table. Promote to the strong table.
      RemoveWeak(weak);
      return InsertStrong(weak);
    }
    return weak;
  }
  // Check the image for a match.
  mirror::String* image = LookupStringFromImage(s);
  if (image != nullptr) {
    return is_strong ? InsertStrong(image) : InsertWeak(image);
  }
  // No match in the strong table or the weak table. Insert into the strong / weak table.
  return is_strong ? InsertStrong(s) : InsertWeak(s);
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
  return Insert(s, true);
}

mirror::String* InternTable::InternWeak(mirror::String* s) {
  return Insert(s, false);
}

bool InternTable::ContainsWeak(mirror::String* s) {
  MutexLock mu(Thread::Current(), *Locks::intern_table_lock_);
  return LookupWeak(s) == s;
}

void InternTable::SweepInternTableWeaks(IsMarkedCallback* callback, void* arg) {
  MutexLock mu(Thread::Current(), *Locks::intern_table_lock_);
  weak_interns_.SweepWeaks(callback, arg);
}

void InternTable::AddImageInternTable(gc::space::ImageSpace* image_space) {
  const ImageSection& intern_section = image_space->GetImageHeader().GetImageSection(
      ImageHeader::kSectionInternedStrings);
  // Read the string tables from the image.
  const uint8_t* ptr = image_space->Begin() + intern_section.Offset();
  const size_t offset = ReadFromMemory(ptr);
  CHECK_LE(offset, intern_section.Size());
}

size_t InternTable::ReadFromMemory(const uint8_t* ptr) {
  MutexLock mu(Thread::Current(), *Locks::intern_table_lock_);
  return ReadFromMemoryLocked(ptr);
}

size_t InternTable::ReadFromMemoryLocked(const uint8_t* ptr) {
  return strong_interns_.ReadIntoPreZygoteTable(ptr);
}

size_t InternTable::WriteToMemory(uint8_t* ptr) {
  MutexLock mu(Thread::Current(), *Locks::intern_table_lock_);
  return strong_interns_.WriteFromPostZygoteTable(ptr);
}

std::size_t InternTable::StringHashEquals::operator()(const GcRoot<mirror::String>& root) const {
  if (kIsDebugBuild) {
    Locks::mutator_lock_->AssertSharedHeld(Thread::Current());
  }
  return static_cast<size_t>(root.Read()->GetHashCode());
}

bool InternTable::StringHashEquals::operator()(const GcRoot<mirror::String>& a,
                                               const GcRoot<mirror::String>& b) const {
  if (kIsDebugBuild) {
    Locks::mutator_lock_->AssertSharedHeld(Thread::Current());
  }
  return a.Read()->Equals(b.Read());
}

size_t InternTable::Table::ReadIntoPreZygoteTable(const uint8_t* ptr) {
  CHECK_EQ(pre_zygote_table_.Size(), 0u);
  size_t read_count = 0;
  pre_zygote_table_ = UnorderedSet(ptr, false /* make copy */, &read_count);
  return read_count;
}

size_t InternTable::Table::WriteFromPostZygoteTable(uint8_t* ptr) {
  return post_zygote_table_.WriteToMemory(ptr);
}

void InternTable::Table::Remove(mirror::String* s) {
  auto it = post_zygote_table_.Find(GcRoot<mirror::String>(s));
  if (it != post_zygote_table_.end()) {
    post_zygote_table_.Erase(it);
  } else {
    it = pre_zygote_table_.Find(GcRoot<mirror::String>(s));
    DCHECK(it != pre_zygote_table_.end());
    pre_zygote_table_.Erase(it);
  }
}

mirror::String* InternTable::Table::Find(mirror::String* s) {
  Locks::intern_table_lock_->AssertHeld(Thread::Current());
  auto it = pre_zygote_table_.Find(GcRoot<mirror::String>(s));
  if (it != pre_zygote_table_.end()) {
    return it->Read();
  }
  it = post_zygote_table_.Find(GcRoot<mirror::String>(s));
  if (it != post_zygote_table_.end()) {
    return it->Read();
  }
  return nullptr;
}

void InternTable::Table::SwapPostZygoteWithPreZygote() {
  if (pre_zygote_table_.Empty()) {
    std::swap(pre_zygote_table_, post_zygote_table_);
    VLOG(heap) << "Swapping " << pre_zygote_table_.Size() << " interns to the pre zygote table";
  } else {
    // This case happens if read the intern table from the image.
    VLOG(heap) << "Not swapping due to non-empty pre_zygote_table_";
  }
}

void InternTable::Table::Insert(mirror::String* s) {
  // Always insert the post zygote table, this gets swapped when we create the zygote to be the
  // pre zygote table.
  post_zygote_table_.Insert(GcRoot<mirror::String>(s));
}

void InternTable::Table::VisitRoots(RootVisitor* visitor) {
  BufferedRootVisitor<kDefaultBufferedRootCount> buffered_visitor(
      visitor, RootInfo(kRootInternedString));
  for (auto& intern : pre_zygote_table_) {
    buffered_visitor.VisitRoot(intern);
  }
  for (auto& intern : post_zygote_table_) {
    buffered_visitor.VisitRoot(intern);
  }
}

void InternTable::Table::SweepWeaks(IsMarkedCallback* callback, void* arg) {
  SweepWeaks(&pre_zygote_table_, callback, arg);
  SweepWeaks(&post_zygote_table_, callback, arg);
}

void InternTable::Table::SweepWeaks(UnorderedSet* set, IsMarkedCallback* callback, void* arg) {
  for (auto it = set->begin(), end = set->end(); it != end;) {
    // This does not need a read barrier because this is called by GC.
    mirror::Object* object = it->Read<kWithoutReadBarrier>();
    mirror::Object* new_object = callback(object, arg);
    if (new_object == nullptr) {
      it = set->Erase(it);
    } else {
      *it = GcRoot<mirror::String>(new_object->AsString());
      ++it;
    }
  }
}

size_t InternTable::Table::Size() const {
  return pre_zygote_table_.Size() + post_zygote_table_.Size();
}

}  // namespace art
