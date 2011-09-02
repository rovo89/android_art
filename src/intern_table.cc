// Copyright 2011 Google Inc. All Rights Reserved.

#include "intern_table.h"

#include "UniquePtr.h"
#include "utf.h"

namespace art {

InternTable::InternTable() {
  intern_table_lock_ = Mutex::Create("InternTable::Lock");
}

InternTable::~InternTable() {
  delete intern_table_lock_;
}

size_t InternTable::Size() const {
  MutexLock mu(intern_table_lock_);
  return strong_interns_.size() + weak_interns_.size();
}

void InternTable::VisitRoots(Heap::RootVisitor* visitor, void* arg) const {
  MutexLock mu(intern_table_lock_);
  typedef Table::const_iterator It; // TODO: C++0x auto
  for (It it = strong_interns_.begin(), end = strong_interns_.end(); it != end; ++it) {
    visitor(it->second, arg);
  }
  // Note: we deliberately don't visit the weak_interns_ table.
}

const String* InternTable::Lookup(Table& table, const String* s, uint32_t hash_code) {
  // Requires the intern_table_lock_.
  typedef Table::const_iterator It; // TODO: C++0x auto
  for (It it = table.find(hash_code), end = table.end(); it != end; ++it) {
    const String* existing_string = it->second;
    if (existing_string->Equals(s)) {
      return existing_string;
    }
  }
  return NULL;
}

const String* InternTable::Insert(Table& table, const String* s, uint32_t hash_code) {
  // Requires the intern_table_lock_.
  table.insert(std::make_pair(hash_code, s));
  return s;
}

void InternTable::RegisterStrong(const String* s) {
  MutexLock mu(intern_table_lock_);
  Insert(strong_interns_, s, s->GetHashCode());
}

void InternTable::Remove(Table& table, const String* s, uint32_t hash_code) {
  // Requires the intern_table_lock_.
  typedef Table::const_iterator It; // TODO: C++0x auto
  for (It it = table.find(hash_code), end = table.end(); it != end; ++it) {
    if (it->second == s) {
      table.erase(it);
      return;
    }
  }
}

const String* InternTable::Insert(const String* s, bool is_strong) {
  MutexLock mu(intern_table_lock_);

  DCHECK(s != NULL);
  uint32_t hash_code = s->GetHashCode();

  if (is_strong) {
    // Check the strong table for a match.
    const String* strong = Lookup(strong_interns_, s, hash_code);
    if (strong != NULL) {
      return strong;
    }

    // There is no match in the strong table, check the weak table.
    const String* weak = Lookup(weak_interns_, s, hash_code);
    if (weak != NULL) {
      // A match was found in the weak table. Promote to the strong table.
      Remove(weak_interns_, weak, hash_code);
      return Insert(strong_interns_, weak, hash_code);
    }

    // No match in the strong table or the weak table. Insert into the strong table.
    return Insert(strong_interns_, s, hash_code);
  }

  // Check the strong table for a match.
  const String* strong = Lookup(strong_interns_, s, hash_code);
  if (strong != NULL) {
    return strong;
  }
  // Check the weak table for a match.
  const String* weak = Lookup(weak_interns_, s, hash_code);
  if (weak != NULL) {
    return weak;
  }
  // Insert into the weak table.
  return Insert(weak_interns_, s, hash_code);
}

const String* InternTable::InternStrong(int32_t utf16_length, const char* utf8_data) {
  return Insert(String::AllocFromModifiedUtf8(utf16_length, utf8_data), true);
}

const String* InternTable::InternWeak(const String* s) {
  return Insert(s, false);
}

bool InternTable::ContainsWeak(const String* s) {
  MutexLock mu(intern_table_lock_);
  const String* found = Lookup(weak_interns_, s, s->GetHashCode());
  return found == s;
}

void InternTable::RemoveWeakIf(const Predicate& predicate) {
  MutexLock mu(intern_table_lock_);
  typedef Table::const_iterator It; // TODO: C++0x auto
  for (It it = weak_interns_.begin(), end = weak_interns_.end(); it != end;) {
    if (predicate(it->second)) {
      weak_interns_.erase(it++);
    } else {
      ++it;
    }
  }
}

}  // namespace art
