// Copyright 2011 Google Inc. All Rights Reserved.

#include "intern_table.h"

#include "scoped_ptr.h"

namespace art {

InternTable::InternTable() {
  intern_table_lock_ = Mutex::Create("InternTable::Lock");
}

void InternTable::VisitRoots(Heap::RootVistor* root_visitor, void* arg) {
  MutexLock mu(intern_table_lock_);
  typedef Table::const_iterator It; // TODO: C++0x auto
  for (It it = intern_table_.begin(), end = intern_table_.end(); it != end; ++it) {
      root_visitor(it->second, arg);
  }
}

String* InternTable::Intern(int32_t utf16_length, const char* utf8_data_in) {
  scoped_array<uint16_t> utf16_data_out(new uint16_t[utf16_length]);
  String::ConvertModifiedUtf8ToUtf16(utf16_data_out.get(), utf8_data_in);
  int32_t hash_code = String::ComputeUtf16Hash(utf16_data_out.get(), utf16_length);
  {
    MutexLock mu(intern_table_lock_);
    typedef Table::const_iterator It; // TODO: C++0x auto
    for (It it = intern_table_.find(hash_code), end = intern_table_.end(); it != end; ++it) {
      String* string = it->second;
      if (string->Equals(utf16_data_out.get(), 0, utf16_length)) {
        return string;
      }
    }
    String* new_string = String::AllocFromUtf16(utf16_length, utf16_data_out.get(), hash_code);
    intern_table_.insert(std::make_pair(hash_code, new_string));
    return new_string;
  }
}

}  // namespace art
