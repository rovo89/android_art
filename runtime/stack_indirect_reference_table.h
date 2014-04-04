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

#ifndef ART_RUNTIME_STACK_INDIRECT_REFERENCE_TABLE_H_
#define ART_RUNTIME_STACK_INDIRECT_REFERENCE_TABLE_H_

#include "base/logging.h"
#include "base/macros.h"
#include "stack.h"

namespace art {
namespace mirror {
class Object;
}
class Thread;

// Stack allocated indirect reference table. It can allocated within
// the bridge frame between managed and native code backed by stack
// storage or manually allocated by SirtRef to hold one reference.
class StackIndirectReferenceTable {
 public:
  explicit StackIndirectReferenceTable(mirror::Object* object) :
      link_(NULL), number_of_references_(1) {
    references_[0].Assign(object);
  }

  ~StackIndirectReferenceTable() {}

  // Number of references contained within this SIRT.
  uint32_t NumberOfReferences() const {
    return number_of_references_;
  }

  // Returns the size of a StackIndirectReferenceTable containing num_references sirts.
  static size_t SizeOf(uint32_t num_references) {
    size_t header_size = OFFSETOF_MEMBER(StackIndirectReferenceTable, references_);
    size_t data_size = sizeof(StackReference<mirror::Object>) * num_references;
    return header_size + data_size;
  }

  // Get the size of the SIRT for the number of entries, with padding added for potential alignment.
  static size_t GetAlignedSirtSize(uint32_t num_references) {
    size_t sirt_size = SizeOf(num_references);
    return RoundUp(sirt_size, 8);
  }

  // Get the size of the SIRT for the number of entries, with padding added for potential alignment.
  static size_t GetAlignedSirtSizeTarget(size_t pointer_size, uint32_t num_references) {
    // Assume that the layout is packed.
    size_t header_size = pointer_size + sizeof(uint32_t);
    // This assumes there is no layout change between 32 and 64b.
    size_t data_size = sizeof(StackReference<mirror::Object>) * num_references;
    size_t sirt_size = header_size + data_size;
    return RoundUp(sirt_size, 8);
  }

  // Link to previous SIRT or NULL.
  StackIndirectReferenceTable* GetLink() const {
    return link_;
  }

  void SetLink(StackIndirectReferenceTable* sirt) {
    DCHECK_NE(this, sirt);
    link_ = sirt;
  }

  // Sets the number_of_references_ field for constructing tables out of raw memory. Warning: will
  // not resize anything.
  void SetNumberOfReferences(uint32_t num_references) {
    number_of_references_ = num_references;
  }

  mirror::Object* GetReference(size_t i) const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK_LT(i, number_of_references_);
    return references_[i].AsMirrorPtr();
  }

  StackReference<mirror::Object>* GetStackReference(size_t i)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK_LT(i, number_of_references_);
    return &references_[i];
  }

  void SetReference(size_t i, mirror::Object* object) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK_LT(i, number_of_references_);
    references_[i].Assign(object);
  }

  bool Contains(StackReference<mirror::Object>* sirt_entry) const {
    // A SIRT should always contain something. One created by the
    // jni_compiler should have a jobject/jclass as a native method is
    // passed in a this pointer or a class
    DCHECK_GT(number_of_references_, 0U);
    return ((&references_[0] <= sirt_entry)
            && (sirt_entry <= (&references_[number_of_references_ - 1])));
  }

  // Offset of link within SIRT, used by generated code
  static size_t LinkOffset() {
    return OFFSETOF_MEMBER(StackIndirectReferenceTable, link_);
  }

  // Offset of length within SIRT, used by generated code
  static uint32_t NumberOfReferencesOffset() {
    return OFFSETOF_MEMBER(StackIndirectReferenceTable, number_of_references_);
  }

  // Offset of link within SIRT, used by generated code
  static size_t ReferencesOffset() {
    return OFFSETOF_MEMBER(StackIndirectReferenceTable, references_);
  }

 private:
  StackIndirectReferenceTable() {}

  StackIndirectReferenceTable* link_;
  uint32_t number_of_references_;

  // number_of_references_ are available if this is allocated and filled in by jni_compiler.
  StackReference<mirror::Object> references_[1];

  DISALLOW_COPY_AND_ASSIGN(StackIndirectReferenceTable);
};

}  // namespace art

#endif  // ART_RUNTIME_STACK_INDIRECT_REFERENCE_TABLE_H_
