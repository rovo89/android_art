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

#ifndef ART_SRC_INTERN_TABLE_H_
#define ART_SRC_INTERN_TABLE_H_

#include <iosfwd>

#include "heap.h"
#include "mutex.h"
#include "object.h"
#include "safe_map.h"

namespace art {

/**
 * Used to intern strings.
 *
 * There are actually two tables: one that holds strong references to its strings, and one that
 * holds weak references. The former is used for string literals, for which there is an effective
 * reference from the constant pool. The latter is used for strings interned at runtime via
 * String.intern. Some code (XML parsers being a prime example) relies on being able to intern
 * arbitrarily many strings for the duration of a parse without permanently increasing the memory
 * footprint.
 */
class InternTable {
 public:
  InternTable();

  // Interns a potentially new string in the 'strong' table. (See above.)
  String* InternStrong(int32_t utf16_length, const char* utf8_data);

  // Interns a potentially new string in the 'strong' table. (See above.)
  String* InternStrong(const char* utf8_data);

  // Interns a potentially new string in the 'strong' table. (See above.)
  String* InternStrong(String* s);

  // Interns a potentially new string in the 'weak' table. (See above.)
  String* InternWeak(String* s);

  // Register a String trusting that it is safe to intern.
  // Used when reinitializing InternTable from an image.
  void RegisterStrong(String* s);

  void SweepInternTableWeaks(Heap::IsMarkedTester is_marked, void* arg);

  bool ContainsWeak(String* s);

  size_t Size() const;

  void VisitRoots(Heap::RootVisitor* visitor, void* arg) const;

  void DumpForSigQuit(std::ostream& os) const;

 private:
  typedef std::multimap<int32_t, String*> Table;

  String* Insert(String* s, bool is_strong);

  String* Lookup(Table& table, String* s, uint32_t hash_code);
  String* Insert(Table& table, String* s, uint32_t hash_code);
  void Remove(Table& table, const String* s, uint32_t hash_code);

  mutable Mutex intern_table_lock_;
  Table image_strong_interns_ GUARDED_BY(intern_table_lock_);
  Table strong_interns_ GUARDED_BY(intern_table_lock_);
  Table weak_interns_ GUARDED_BY(intern_table_lock_);
};

}  // namespace art

#endif  // ART_SRC_CLASS_LINKER_H_
