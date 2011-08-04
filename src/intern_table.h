// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_INTERN_TABLE_H_
#define ART_SRC_INTERN_TABLE_H_

#include "unordered_map.h"

#include "heap.h"
#include "object.h"

namespace art {

class InternTable {
 public:
  InternTable();
  String* Intern(int32_t utf16_length, const char* utf8_data);
  void VisitRoots(Heap::RootVistor* root_visitor, void* arg);

 private:
  typedef std::tr1::unordered_multimap<int32_t, String*> Table;
  Table intern_table_;
  Mutex* intern_table_lock_;
};

}  // namespace art

#endif  // ART_SRC_CLASS_LINKER_H_
