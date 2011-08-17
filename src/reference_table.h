/*
 * Copyright (C) 2008 The Android Open Source Project
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

#ifndef ART_SRC_REFERENCE_TABLE_H_
#define ART_SRC_REFERENCE_TABLE_H_

#include <cstddef>
#include <iosfwd>
#include <string>
#include <vector>

namespace art {

class Object;

static const Object* const kInvalidIndirectRefObject = reinterpret_cast<Object*>(0xdead4321);
static const Object* const kClearedJniWeakGlobal = reinterpret_cast<Object*>(0xdead1234);

// Maintain a table of references.  Used for internal local references,
// JNI monitor references, and JNI pinned array references.
//
// None of the functions are synchronized.
class ReferenceTable {
 public:
  ReferenceTable(const char* name, size_t initial_size, size_t max_size);

  void Add(Object* obj);

  void Remove(Object* obj);

  size_t Size() const;

  void Dump() const;

 private:
  std::string name_;
  std::vector<Object*> entries_;
  size_t max_size_;
};

}  // namespace art

#endif  // ART_SRC_REFERENCE_TABLE_H_
