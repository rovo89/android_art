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

#include "macros.h"
#include "reg_type.h"
#include "stl_util.h"

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

  const RegType& GetFromId(uint16_t id) {
    DCHECK_LT(id, entries_.size());
    RegType* result = entries_[id];
    DCHECK(result != NULL);
    return *result;
  }

  const RegType& From(RegType::Type type, const ClassLoader* loader, const char* descriptor);
  const RegType& FromClass(Class* klass);
  const RegType& FromCat1Const(int32_t value);
  const RegType& FromDescriptor(const ClassLoader* loader, const char* descriptor);
  const RegType& FromType(RegType::Type);

  const RegType& Boolean() { return FromType(RegType::kRegTypeBoolean); }
  const RegType& Byte()    { return FromType(RegType::kRegTypeByte); }
  const RegType& Char()    { return FromType(RegType::kRegTypeChar); }
  const RegType& Short()   { return FromType(RegType::kRegTypeShort); }
  const RegType& Integer() { return FromType(RegType::kRegTypeInteger); }
  const RegType& Float()   { return FromType(RegType::kRegTypeFloat); }
  const RegType& Long()    { return FromType(RegType::kRegTypeLongLo); }
  const RegType& Double()  { return FromType(RegType::kRegTypeDoubleLo); }

  const RegType& JavaLangClass()  { return From(RegType::kRegTypeReference, NULL, "Ljava/lang/Class;"); }
  const RegType& JavaLangObject() { return From(RegType::kRegTypeReference, NULL, "Ljava/lang/Object;"); }
  const RegType& JavaLangString() { return From(RegType::kRegTypeReference, NULL, "Ljava/lang/String;"); }
  const RegType& JavaLangThrowable() { return From(RegType::kRegTypeReference, NULL, "Ljava/lang/Throwable;"); }

  const RegType& Undefined(){ return FromType(RegType::kRegTypeUndefined); }
  const RegType& Conflict() { return FromType(RegType::kRegTypeConflict); }
  const RegType& ConstLo()  { return FromType(RegType::kRegTypeConstLo); }
  const RegType& Zero()     { return FromCat1Const(0); }

  const RegType& Uninitialized(const RegType& type, uint32_t allocation_pc);
  // Create an uninitialized 'this' argument for the given type.
  const RegType& UninitializedThisArgument(const RegType& type);
  const RegType& FromUninitialized(const RegType& uninit_type);

  // Representatives of various constant types. When merging constants we can't infer a type,
  // (an int may later be used as a float) so we select these representative values meaning future
  // merges won't know the exact constant value but have some notion of its size.
  const RegType& ByteConstant() { return FromCat1Const(std::numeric_limits<jbyte>::min()); }
  const RegType& ShortConstant() { return FromCat1Const(std::numeric_limits<jshort>::min()); }
  const RegType& IntConstant() { return FromCat1Const(std::numeric_limits<jint>::max()); }

  const RegType& GetComponentType(const RegType& array, const ClassLoader* loader);
 private:
  // The allocated entries
  std::vector<RegType*> entries_;

  DISALLOW_COPY_AND_ASSIGN(RegTypeCache);
};

}  // namespace verifier
}  // namespace art

#endif  // ART_SRC_VERIFIER_REG_TYPE_CACHE_H_
