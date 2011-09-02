// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_INTERN_TABLE_H_
#define ART_SRC_INTERN_TABLE_H_

#include "unordered_map.h"

#include "heap.h"
#include "object.h"

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
  ~InternTable();

  // Interns a potentially new string in the 'strong' table. (See above.)
  const String* InternStrong(int32_t utf16_length, const char* utf8_data);

  // Interns a potentially new string in the 'weak' table. (See above.)
  const String* InternWeak(const String* s);

  // Register a String trusting that it is safe to intern.
  // Used when reinitializing InternTable from an image.
  void RegisterStrong(const String* s);

  // Removes all weak interns for which the predicate functor 'p' returns true.
  // (We use an explicit Predicate type rather than a template to keep implementation
  // out of the header file.)
  struct Predicate {
    virtual ~Predicate() {}
    virtual bool operator()(const String*) const = 0;
  };
  void RemoveWeakIf(const Predicate& p);

  bool ContainsWeak(const String* s);

  size_t Size() const;

  void VisitRoots(Heap::RootVisitor* visitor, void* arg) const;

 private:
  typedef std::tr1::unordered_multimap<int32_t, const String*> Table;

  const String* Insert(const String* s, bool is_strong);

  const String* Lookup(Table& table, const String* s, uint32_t hash_code);
  const String* Insert(Table& table, const String* s, uint32_t hash_code);
  void Remove(Table& table, const String* s, uint32_t hash_code);

  Table strong_interns_;
  Table weak_interns_;
  Mutex* intern_table_lock_;
};

}  // namespace art

#endif  // ART_SRC_CLASS_LINKER_H_
