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

#ifndef ART_SRC_STACK_INDIRECT_REFERENCE_TABLE_H_
#define ART_SRC_STACK_INDIRECT_REFERENCE_TABLE_H_

namespace art {

#include "macros.h"

class Object;

// Stack allocated indirect reference table, allocated within the bridge frame
// between managed and native code.
class StackIndirectReferenceTable {
public:
  // Number of references contained within this SIRT
  size_t NumberOfReferences() {
    return number_of_references_;
  }

  // Link to previous SIRT or NULL
  StackIndirectReferenceTable* Link() {
    return link_;
  }

  Object** References() {
    return references_;
  }

  // Offset of length within SIRT, used by generated code
  static size_t NumberOfReferencesOffset() {
    return OFFSETOF_MEMBER(StackIndirectReferenceTable, number_of_references_);
  }

  // Offset of link within SIRT, used by generated code
  static size_t LinkOffset() {
    return OFFSETOF_MEMBER(StackIndirectReferenceTable, link_);
  }

private:
  StackIndirectReferenceTable() {}

  size_t number_of_references_;
  StackIndirectReferenceTable* link_;

  // Fake array, really allocated and filled in by jni_compiler.
  Object* references_[0];

  DISALLOW_COPY_AND_ASSIGN(StackIndirectReferenceTable);
};

}  // namespace art

#endif  // ART_SRC_STACK_INDIRECT_REFERENCE_TABLE_H_
