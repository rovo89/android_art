/*
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef ART_SRC_VERIFIER_REG_TYPE_CACHE_H_
#define ART_SRC_VERIFIER_REG_TYPE_CACHE_H_

#include "base/macros.h"
#include "base/stl_util.h"
#include "reg_type.h"

namespace art {
namespace verifier {

class RegTypeCache {
 public:
  explicit RegTypeCache() : entries_(RegType::kRegTypeLastFixedLocation + 1) {
    Undefined();  // ensure Undefined is initialized
  }
  ~RegTypeCache() {
    STLDeleteElements(&entries_);
  }

  const RegType& GetFromId(uint16_t id) const {
    DCHECK_LT(id, entries_.size());
    RegType* result = entries_[id];
    DCHECK(result != NULL);
    return *result;
  }

  const RegType& From(RegType::Type type, ClassLoader* loader, const char* descriptor, bool precise)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  const RegType& FromClass(Class* klass, bool precise)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  const RegType& FromCat1Const(int32_t value, bool precise)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  const RegType& FromCat2ConstLo(int32_t value, bool precise);
  const RegType& FromCat2ConstHi(int32_t value, bool precise);
  const RegType& FromDescriptor(ClassLoader* loader, const char* descriptor, bool precise)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  const RegType& FromType(RegType::Type)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  const RegType& FromUnresolvedMerge(const RegType& left, const RegType& right);
  const RegType& FromUnresolvedSuperClass(const RegType& child);

  const RegType& Boolean() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return FromType(RegType::kRegTypeBoolean);
  }
  const RegType& Byte() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return FromType(RegType::kRegTypeByte);
  }
  const RegType& Char() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return FromType(RegType::kRegTypeChar);
  }
  const RegType& Short() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return FromType(RegType::kRegTypeShort);
  }
  const RegType& Integer() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return FromType(RegType::kRegTypeInteger);
  }
  const RegType& Float() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return FromType(RegType::kRegTypeFloat);
  }
  const RegType& LongLo() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return FromType(RegType::kRegTypeLongLo);
  }
  const RegType& LongHi() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return FromType(RegType::kRegTypeLongHi);
  }
  const RegType& DoubleLo() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return FromType(RegType::kRegTypeDoubleLo);
  }
  const RegType& DoubleHi() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return FromType(RegType::kRegTypeDoubleHi);
  }

  const RegType& JavaLangClass(bool precise) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return From(precise ? RegType::kRegTypeReference
                        : RegType::kRegTypePreciseReference,
                NULL, "Ljava/lang/Class;", precise);
  }
  const RegType& JavaLangObject(bool precise) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return From(precise ? RegType::kRegTypeReference
                        : RegType::kRegTypePreciseReference,
                NULL, "Ljava/lang/Object;", precise);
  }
  const RegType& JavaLangString() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    // String is final and therefore always precise.
    return From(RegType::kRegTypePreciseReference, NULL, "Ljava/lang/String;", true);
  }
  const RegType& JavaLangThrowable(bool precise) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return From(precise ? RegType::kRegTypeReference
                        : RegType::kRegTypePreciseReference,
                NULL, "Ljava/lang/Throwable;", precise);
  }

  const RegType& Undefined() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return FromType(RegType::kRegTypeUndefined);
  }
  const RegType& Conflict() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return FromType(RegType::kRegTypeConflict);
  }
  const RegType& Zero() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return FromCat1Const(0, true);
  }

  const RegType& Uninitialized(const RegType& type, uint32_t allocation_pc);
  // Create an uninitialized 'this' argument for the given type.
  const RegType& UninitializedThisArgument(const RegType& type);
  const RegType& FromUninitialized(const RegType& uninit_type);

  // Representatives of various constant types. When merging constants we can't infer a type,
  // (an int may later be used as a float) so we select these representative values meaning future
  // merges won't know the exact constant value but have some notion of its size.
  const RegType& ByteConstant() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return FromCat1Const(std::numeric_limits<jbyte>::min(), false);
  }
  const RegType& ShortConstant() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return FromCat1Const(std::numeric_limits<jshort>::min(), false);
  }
  const RegType& IntConstant() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return FromCat1Const(std::numeric_limits<jint>::max(), false);
  }

  const RegType& GetComponentType(const RegType& array, ClassLoader* loader)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void Dump(std::ostream& os) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

 private:
  // The allocated entries
  std::vector<RegType*> entries_;

  DISALLOW_COPY_AND_ASSIGN(RegTypeCache);
};

}  // namespace verifier
}  // namespace art

#endif  // ART_SRC_VERIFIER_REG_TYPE_CACHE_H_
