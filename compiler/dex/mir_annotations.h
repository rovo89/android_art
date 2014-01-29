/*
 * Copyright (C) 2014 The Android Open Source Project
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

#ifndef ART_COMPILER_DEX_MIR_ANNOTATIONS_H_
#define ART_COMPILER_DEX_MIR_ANNOTATIONS_H_

#include "base/macros.h"
#include "dex_file.h"
#include "offsets.h"

namespace art {

class DexCompilationUnit;

/*
 * Annotations are calculated from the perspective of the compilation unit that
 * accesses the fields or methods. Since they are stored with that unit, they do not
 * need to reference the dex file or method for which they have been calculated.
 * However, we do store the dex file, declaring class index and field index of the
 * resolved field to help distinguish between fields.
 */

class IFieldAnnotation {
 public:
  // For each requested instance field compute whether we can fast path the access with IGET/IPUT.
  // If yes (at least for IGET), computes the offset and volatility.
  static void Resolve(const DexCompilationUnit* mUnit, IFieldAnnotation* annotations, size_t count)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  // Construct an unresolved instance field annotation.
  explicit IFieldAnnotation(uint16_t field_idx)
      : field_idx_(field_idx),
        fast_get_(0u),
        fast_put_(0u),
        is_volatile_(1u),
        reserved_(0u),
        field_offset_(0u),
        declaring_dex_file_(nullptr),
        declaring_class_idx_(0u),
        declaring_field_idx_(0u) {
  }

  uint16_t FieldIndex() const {
    return field_idx_;
  }

  bool FastGet() const {
    return fast_get_ != 0u;
  }

  bool FastPut() const {
    return fast_put_ != 0u;
  }

  bool IsVolatile() const {
    return is_volatile_ != 0u;
  }

  MemberOffset FieldOffset() const {
    return field_offset_;
  }

  bool IsResolved() const {
    return declaring_dex_file_ != nullptr;
  }

  const DexFile* DeclaringDexFile() const {
    return declaring_dex_file_;
  }

  uint16_t DeclaringClassIndex() const {
    return declaring_class_idx_;
  }

  uint16_t DeclaringFieldIndex() const {
    return declaring_field_idx_;
  }

 private:
  // The field index in the compiling method's dex file.
  uint16_t field_idx_;
  // Can the compiling method fast-path IGET from this field?
  uint16_t fast_get_ : 1;
  // Can the compiling method fast-path IPUT from this field?
  uint16_t fast_put_ : 1;
  // Is the field volatile? Unknown if unresolved, so treated as volatile.
  uint16_t is_volatile_ : 1;
  // Reserved.
  uint16_t reserved_ : 13;
  // The member offset of the field, MemberOffset(static_cast<size_t>(-1)) if unresolved.
  MemberOffset field_offset_;
  // The dex file that defines the class containing the field and the field, nullptr if unresolved.
  const DexFile* declaring_dex_file_;
  // The type index of the class declaring the field, 0 if unresolved.
  uint16_t declaring_class_idx_;
  // The field index in the dex file that defines field, 0 if unresolved.
  uint16_t declaring_field_idx_;
};

class SFieldAnnotation {
 public:
  // For each requested static field compute whether we can fast path the access with SGET/SPUT.
  // If yes (at least for SGET), computes the offset and volatility, storage index, and whether
  // the access is from the same class or the class can be assumed initialized.
  static void Resolve(const DexCompilationUnit* mUnit, SFieldAnnotation* annotations, size_t count)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  // Construct an unresolved static field annotation.
  explicit SFieldAnnotation(uint16_t field_idx)
      : field_idx_(field_idx),
        fast_get_(0u),
        fast_put_(0u),
        is_volatile_(1u),
        is_referrers_class_(0u),
        is_initialized_(0u),
        reserved_(0u),
        field_offset_(0u),
        storage_index_(DexFile::kDexNoIndex),
        declaring_dex_file_(nullptr),
        declaring_class_idx_(0u),
        declaring_field_idx_(0u) {
  }

  uint16_t FieldIndex() const {
    return field_idx_;
  }

  bool FastGet() const {
    return fast_get_ != 0u;
  }

  bool FastPut() const {
    return fast_put_ != 0u;
  }

  bool IsVolatile() const {
    return is_volatile_ != 0u;
  }

  bool IsReferrersClass() const {
    return is_referrers_class_ != 0u;
  }

  bool IsInitialized() const {
    return is_initialized_ != 0u;
  }

  MemberOffset FieldOffset() const {
    return field_offset_;
  }

  uint32_t StorageIndex() const {
    return storage_index_;
  }

  bool IsResolved() const {
    return declaring_dex_file_ != nullptr;
  }

  const DexFile* DeclaringDexFile() const {
    return declaring_dex_file_;
  }

  uint16_t DeclaringClassIndex() const {
    return declaring_class_idx_;
  }

  uint16_t DeclaringFieldIndex() const {
    return declaring_field_idx_;
  }

 private:
  // The field index in the compiling method's dex file.
  uint16_t field_idx_;
  // Can the compiling method fast-path IGET from this field?
  uint16_t fast_get_ : 1;
  // Can the compiling method fast-path IPUT from this field?
  uint16_t fast_put_ : 1;
  // Is the field volatile? Unknown if unresolved, so treated as volatile (true).
  uint16_t is_volatile_ : 1;
  // Is the field in the referrer's class? false if unresolved.
  uint16_t is_referrers_class_ : 1;
  // Can we assume that the field's class is already initialized? false if unresolved.
  uint16_t is_initialized_ : 1;
  // Reserved.
  uint16_t reserved_ : 11;
  // The member offset of the field, static_cast<size_t>(-1) if unresolved.
  MemberOffset field_offset_;
  // The type index of the declaring class in the compiling method's dex file,
  // -1 if the field is unresolved or there's no appropriate TypeId in that dex file.
  uint32_t storage_index_;
  // The dex file that defines the class containing the field and the field, nullptr if unresolved.
  const DexFile* declaring_dex_file_;
  // The type index of the class declaring the field, 0 if unresolved.
  uint16_t declaring_class_idx_;
  // The field index in the dex file that defines field, 0 if unresolved.
  uint16_t declaring_field_idx_;
};

}  // namespace art

#endif  // ART_COMPILER_DEX_MIR_ANNOTATIONS_H_
