// Copyright 2011 Google Inc. All Rights Reserved.

#include "intern_table.h"

#include "UniquePtr.h"
#include "utf.h"

namespace art {

InternTable::InternTable() : intern_table_lock_("InternTable lock") {
}

size_t InternTable::Size() const {
  MutexLock mu(intern_table_lock_);
  return strong_interns_.size() + weak_interns_.size();
}

void InternTable::DumpForSigQuit(std::ostream& os) const {
  MutexLock mu(intern_table_lock_);
  os << "Intern table: " << strong_interns_.size() << " strong; "
     << weak_interns_.size() << " weak; "
     << image_strong_interns_.size() << " image strong\n";
}

void InternTable::VisitRoots(Heap::RootVisitor* visitor, void* arg) const {
  MutexLock mu(intern_table_lock_);
  typedef Table::const_iterator It; // TODO: C++0x auto
  for (It it = strong_interns_.begin(), end = strong_interns_.end(); it != end; ++it) {
    visitor(it->second, arg);
  }
  // Note: we deliberately don't visit the weak_interns_ table and the immutable image roots.
}

String* InternTable::Lookup(Table& table, String* s, uint32_t hash_code) {
  intern_table_lock_.AssertHeld();
  typedef Table::const_iterator It; // TODO: C++0x auto
  for (It it = table.find(hash_code), end = table.end(); it != end; ++it) {
    String* existing_string = it->second;
    if (existing_string->Equals(s)) {
      return existing_string;
    }
  }
  return NULL;
}

String* InternTable::Insert(Table& table, String* s, uint32_t hash_code) {
  intern_table_lock_.AssertHeld();
  table.insert(std::make_pair(hash_code, s));
  return s;
}

void InternTable::RegisterStrong(String* s) {
  MutexLock mu(intern_table_lock_);
  Insert(image_strong_interns_, s, s->GetHashCode());
}

void InternTable::Remove(Table& table, const String* s, uint32_t hash_code) {
  intern_table_lock_.AssertHeld();
  typedef Table::const_iterator It; // TODO: C++0x auto
  for (It it = table.find(hash_code), end = table.end(); it != end; ++it) {
    if (it->second == s) {
      table.erase(it);
      return;
    }
  }
}

String* InternTable::Insert(String* s, bool is_strong) {
  MutexLock mu(intern_table_lock_);

  DCHECK(s != NULL);
  uint32_t hash_code = s->GetHashCode();

  if (is_strong) {
    // Check the strong tables for a match.
    String* strong = Lookup(strong_interns_, s, hash_code);
    if (strong != NULL) {
      return strong;
    }
    strong = Lookup(image_strong_interns_, s, hash_code);
    if (strong != NULL) {
      return strong;
    }

    // There is no match in the strong table, check the weak table.
    String* weak = Lookup(weak_interns_, s, hash_code);
    if (weak != NULL) {
      // A match was found in the weak table. Promote to the strong table.
      Remove(weak_interns_, weak, hash_code);
      return Insert(strong_interns_, weak, hash_code);
    }

    // No match in the strong table or the weak table. Insert into the strong table.
    return Insert(strong_interns_, s, hash_code);
  }

  // Check the strong table for a match.
  String* strong = Lookup(strong_interns_, s, hash_code);
  if (strong != NULL) {
    return strong;
  }
  // Check the weak table for a match.
  String* weak = Lookup(weak_interns_, s, hash_code);
  if (weak != NULL) {
    return weak;
  }
  // Insert into the weak table.
  return Insert(weak_interns_, s, hash_code);
}

String* InternTable::InternStrong(int32_t utf16_length, const char* utf8_data) {
  return InternStrong(String::AllocFromModifiedUtf8(utf16_length, utf8_data));
}

String* InternTable::InternStrong(const char* utf8_data) {
  return InternStrong(String::AllocFromModifiedUtf8(utf8_data));
}

String* InternTable::InternStrong(String* s) {
  if (s == NULL) {
    return NULL;
  }
  return Insert(s, true);
}

String* InternTable::InternWeak(String* s) {
  if (s == NULL) {
    return NULL;
  }
  return Insert(s, false);
}

bool InternTable::ContainsWeak(String* s) {
  MutexLock mu(intern_table_lock_);
  const String* found = Lookup(weak_interns_, s, s->GetHashCode());
  return found == s;
}

void InternTable::SweepInternTableWeaks(Heap::IsMarkedTester is_marked, void* arg) {
  MutexLock mu(intern_table_lock_);
  typedef Table::const_iterator It; // TODO: C++0x auto
  for (It it = weak_interns_.begin(), end = weak_interns_.end(); it != end;) {
    Object* object = it->second;
    if (!is_marked(object, arg)) {
      weak_interns_.erase(it++);
    } else {
      ++it;
    }
  }
}

}  // namespace art
