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

#ifndef ART_SRC_VERIFIER_REG_TYPE_H_
#define ART_SRC_VERIFIER_REG_TYPE_H_

#include "base/macros.h"
#include "object.h"

#include <stdint.h>

namespace art {
namespace verifier {

class RegTypeCache;

/*
 * RegType holds information about the "type" of data held in a register.
 */
class RegType {
 public:
  enum Type {
    // A special state that identifies a register as undefined.
    kRegTypeUndefined = 0,
    // The bottom type, used to denote the type of operations such as returning a void, throwing
    // an exception or merging incompatible types, such as an int and a long.
    kRegTypeConflict,
    kRegTypeBoolean,        // Z.
    kRegType1nrSTART = kRegTypeBoolean,
    kRegTypeIntegralSTART = kRegTypeBoolean,
    kRegTypeByte,           // B.
    kRegTypeShort,          // S.
    kRegTypeChar,           // C.
    kRegTypeInteger,        // I.
    kRegTypeIntegralEND = kRegTypeInteger,
    kRegTypeFloat,          // F.
    kRegType1nrEND = kRegTypeFloat,
    kRegTypeLongLo,         // J - lower-numbered register; endian-independent.
    kRegTypeLongHi,
    kRegTypeDoubleLo,       // D.
    kRegTypeDoubleHi,
    kRegTypeLastFixedLocation = kRegTypeDoubleHi,
    kRegTypePreciseConst,   // 32-bit constant - could be float or int.
    kRegTypeImpreciseConst, // 32-bit constant derived value - could be float or int.
    kRegTypePreciseConstLo, // Const wide, lower half - could be long or double.
    kRegTypePreciseConstHi, // Const wide, upper half - could be long or double.
    kRegTypeImpreciseConstLo, // Const derived wide, lower half - could be long or double.
    kRegTypeImpreciseConstHi, // Const derived wide, upper half - could be long or double.
    kRegTypeUnresolvedReference,        // Reference type that couldn't be resolved.
    kRegTypeUninitializedReference,     // Freshly allocated reference type.
    kRegTypeUninitializedThisReference, // Freshly allocated reference passed as "this".
    kRegTypeUnresolvedAndUninitializedReference, // Freshly allocated unresolved reference type.
                                        // Freshly allocated unresolved reference passed as "this".
    kRegTypeUnresolvedAndUninitializedThisReference,
    kRegTypeUnresolvedMergedReference,  // Tree of merged references (at least 1 is unresolved).
    kRegTypeUnresolvedSuperClass,       // Super class of an unresolved type.
    kRegTypeReference,                  // Reference type.
    kRegTypePreciseReference,           // Precisely the given type.
  };

  Type GetType() const {
    return type_;
  }

  bool IsUndefined() const { return type_ == kRegTypeUndefined; }
  bool IsConflict() const { return type_ == kRegTypeConflict; }
  bool IsBoolean() const { return type_ == kRegTypeBoolean; }
  bool IsByte()    const { return type_ == kRegTypeByte; }
  bool IsChar()    const { return type_ == kRegTypeChar; }
  bool IsShort()   const { return type_ == kRegTypeShort; }
  bool IsInteger() const { return type_ == kRegTypeInteger; }
  bool IsLong()    const { return type_ == kRegTypeLongLo; }
  bool IsFloat()   const { return type_ == kRegTypeFloat; }
  bool IsDouble()  const { return type_ == kRegTypeDoubleLo; }
  bool IsUnresolvedReference() const { return type_ == kRegTypeUnresolvedReference; }
  bool IsUninitializedReference() const { return type_ == kRegTypeUninitializedReference; }
  bool IsUninitializedThisReference() const { return type_ == kRegTypeUninitializedThisReference; }
  bool IsUnresolvedAndUninitializedReference() const {
    return type_ == kRegTypeUnresolvedAndUninitializedReference;
  }
  bool IsUnresolvedAndUninitializedThisReference() const {
    return type_ == kRegTypeUnresolvedAndUninitializedThisReference;
  }
  bool IsUnresolvedMergedReference() const {  return type_ == kRegTypeUnresolvedMergedReference; }
  bool IsUnresolvedSuperClass() const {  return type_ == kRegTypeUnresolvedSuperClass; }
  bool IsReference() const { return type_ == kRegTypeReference; }
  bool IsPreciseReference() const { return type_ == kRegTypePreciseReference; }

  bool IsUninitializedTypes() const {
    return IsUninitializedReference() || IsUninitializedThisReference() ||
        IsUnresolvedAndUninitializedReference() || IsUnresolvedAndUninitializedThisReference();
  }
  bool IsUnresolvedTypes() const {
    return IsUnresolvedReference() || IsUnresolvedAndUninitializedReference() ||
        IsUnresolvedAndUninitializedThisReference() || IsUnresolvedMergedReference() ||
        IsUnresolvedSuperClass();
  }
  bool IsLowHalf() const {
    return type_ == kRegTypeLongLo ||
           type_ == kRegTypeDoubleLo ||
           type_ == kRegTypePreciseConstLo ||
           type_ == kRegTypeImpreciseConstLo;
  }
  bool IsHighHalf() const {
    return type_ == kRegTypeLongHi ||
           type_ == kRegTypeDoubleHi ||
           type_ == kRegTypePreciseConstHi ||
           type_ == kRegTypeImpreciseConstHi;
  }

  bool IsLongOrDoubleTypes() const { return IsLowHalf(); }

  // Check this is the low half, and that type_h is its matching high-half
  bool CheckWidePair(const RegType& type_h) const {
    if (IsLowHalf()) {
      return (type_h.type_ == type_ + 1) ||
          (IsPreciseConstantLo() && type_h.IsImpreciseConstantHi()) ||
          (IsImpreciseConstantLo() && type_h.IsPreciseConstantHi());
    }
    return false;
  }

  // The high half that corresponds to this low half
  const RegType& HighHalf(RegTypeCache* cache) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  bool IsPreciseConstant() const {
    return type_ == kRegTypePreciseConst;
  }
  bool IsPreciseConstantLo() const {
    return type_ == kRegTypePreciseConstLo;
  }
  bool IsPreciseConstantHi() const {
    return type_ == kRegTypePreciseConstHi;
  }
  bool IsImpreciseConstantLo() const {
    return type_ == kRegTypeImpreciseConstLo;
  }
  bool IsImpreciseConstantHi() const {
    return type_ == kRegTypeImpreciseConstHi;
  }
  bool IsConstant() const {
    return type_ == kRegTypePreciseConst || type_ == kRegTypeImpreciseConst;
  }

  bool IsConstantLo() const {
    return type_ == kRegTypePreciseConstLo || type_ == kRegTypeImpreciseConstLo;
  }
  bool IsLongConstant() const {
    return IsConstantLo();
  }
  bool IsConstantHi() const {
    return type_ == kRegTypePreciseConstHi || type_ == kRegTypeImpreciseConstHi;
  }
  bool IsLongConstantHigh() const {
    return IsConstantHi();
  }

  // If this is a 32-bit constant, what is the value? This value may be imprecise in which case
  // the value represents part of the integer range of values that may be held in the register.
  int32_t ConstantValue() const {
    DCHECK(IsConstant());
    return allocation_pc_or_constant_or_merged_types_;
  }
  int32_t ConstantValueLo() const {
    DCHECK(IsConstantLo());
    return allocation_pc_or_constant_or_merged_types_;
  }
  int32_t ConstantValueHi() const {
    DCHECK(IsConstantHi());
    return allocation_pc_or_constant_or_merged_types_;
  }

  bool IsZero()         const { return IsPreciseConstant() && ConstantValue() == 0; }
  bool IsOne()          const { return IsPreciseConstant() && ConstantValue() == 1; }
  bool IsConstantBoolean() const {
    return IsConstant() && ConstantValue() >= 0 && ConstantValue() <= 1;
  }
  bool IsConstantByte() const {
    return IsConstant() &&
        ConstantValue() >= std::numeric_limits<jbyte>::min() &&
        ConstantValue() <= std::numeric_limits<jbyte>::max();
  }
  bool IsConstantShort() const {
    return IsConstant() &&
        ConstantValue() >= std::numeric_limits<jshort>::min() &&
        ConstantValue() <= std::numeric_limits<jshort>::max();
  }
  bool IsConstantChar() const {
    return IsConstant() && ConstantValue() >= 0 &&
        ConstantValue() <= std::numeric_limits<jchar>::max();
  }

  bool IsReferenceTypes() const {
    return IsNonZeroReferenceTypes() || IsZero();
  }

  bool IsNonZeroReferenceTypes() const {
    return IsReference() || IsPreciseReference() || IsUnresolvedReference() ||
        IsUninitializedReference() || IsUninitializedThisReference() ||
        IsUnresolvedAndUninitializedReference() || IsUnresolvedAndUninitializedThisReference() ||
        IsUnresolvedMergedReference() || IsUnresolvedSuperClass();
  }

  bool IsCategory1Types() const {
    return (type_ >= kRegType1nrSTART && type_ <= kRegType1nrEND) || IsConstant();
  }

  bool IsCategory2Types() const {
    return IsLowHalf();  // Don't expect explicit testing of high halves
  }

  bool IsBooleanTypes() const { return IsBoolean() || IsConstantBoolean(); }
  bool IsByteTypes() const { return IsByte() || IsBoolean() || IsConstantByte(); }
  bool IsShortTypes() const { return IsShort() || IsByte() || IsBoolean() || IsConstantShort(); }
  bool IsCharTypes() const { return IsChar() || IsBooleanTypes() || IsConstantChar(); }
  bool IsIntegralTypes() const {
    return (type_ >= kRegTypeIntegralSTART && type_ <= kRegTypeIntegralEND) || IsConstant();
  }
  bool IsArrayIndexTypes() const { return IsIntegralTypes(); }

  // Float type may be derived from any constant type
  bool IsFloatTypes() const { return IsFloat() || IsConstant(); }

  bool IsLongTypes() const {
    return IsLong() || IsLongConstant();
  }
  bool IsLongHighTypes() const {
    return type_ == kRegTypeLongHi ||
           type_ == kRegTypePreciseConstHi ||
           type_ == kRegTypeImpreciseConstHi;
  }
  bool IsDoubleTypes() const {
    return IsDouble() || IsLongConstant();
  }
  bool IsDoubleHighTypes() const {
    return type_ == kRegTypeDoubleHi ||
           type_ == kRegTypePreciseConstHi ||
           type_ == kRegTypeImpreciseConstHi;
  }

  uint32_t GetAllocationPc() const {
    DCHECK(IsUninitializedTypes());
    return allocation_pc_or_constant_or_merged_types_;
  }

  bool HasClass() const {
    return IsReference() || IsPreciseReference();
  }

  Class* GetClass() const {
    DCHECK(!IsUnresolvedReference());
    DCHECK(klass_or_descriptor_ != NULL);
    DCHECK(klass_or_descriptor_->IsClass());
    return down_cast<Class*>(klass_or_descriptor_);
  }

  bool IsJavaLangObject() const {
    return IsReference() && GetClass()->IsObjectClass();
  }

  bool IsArrayTypes() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    if (IsUnresolvedTypes() && !IsUnresolvedMergedReference() && !IsUnresolvedSuperClass()) {
      return GetDescriptor()->CharAt(0) == '[';
    } else if (HasClass()) {
      return GetClass()->IsArrayClass();
    } else {
      return false;
    }
  }

  bool IsObjectArrayTypes() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    if (IsUnresolvedTypes() && !IsUnresolvedMergedReference() && !IsUnresolvedSuperClass()) {
      // Primitive arrays will always resolve
      DCHECK(GetDescriptor()->CharAt(1) == 'L' || GetDescriptor()->CharAt(1) == '[');
      return GetDescriptor()->CharAt(0) == '[';
    } else if (HasClass()) {
      Class* type = GetClass();
      return type->IsArrayClass() && !type->GetComponentType()->IsPrimitive();
    } else {
      return false;
    }
  }

  Primitive::Type GetPrimitiveType() const {
    if (IsNonZeroReferenceTypes()) {
      return Primitive::kPrimNot;
    } else if (IsBooleanTypes()) {
      return Primitive::kPrimBoolean;
    } else if (IsByteTypes()) {
      return Primitive::kPrimByte;
    } else if (IsShortTypes()) {
      return Primitive::kPrimShort;
    } else if (IsCharTypes()) {
      return Primitive::kPrimChar;
    } else if (IsFloat()) {
      return Primitive::kPrimFloat;
    } else if (IsIntegralTypes()) {
      return Primitive::kPrimInt;
    } else if (IsDouble()) {
      return Primitive::kPrimDouble;
    } else {
      DCHECK(IsLongTypes());
      return Primitive::kPrimLong;
    }
  }

  bool IsJavaLangObjectArray() const {
    if (HasClass()) {
      Class* type = GetClass();
      return type->IsArrayClass() && type->GetComponentType()->IsObjectClass();
    }
    return false;
  }

  bool IsInstantiableTypes() const {
    return IsUnresolvedTypes() || (IsNonZeroReferenceTypes() && GetClass()->IsInstantiable());
  }

  String* GetDescriptor() const {
    DCHECK(IsUnresolvedTypes() && !IsUnresolvedMergedReference() && !IsUnresolvedSuperClass());
    DCHECK(klass_or_descriptor_ != NULL);
    DCHECK(klass_or_descriptor_->GetClass()->IsStringClass());
    return down_cast<String*>(klass_or_descriptor_);
  }

  uint16_t GetId() const {
    return cache_id_;
  }

  // The top of a tree of merged types.
  std::pair<uint16_t, uint16_t> GetTopMergedTypes() const {
    DCHECK(IsUnresolvedMergedReference());
    uint16_t type1 = static_cast<uint16_t>(allocation_pc_or_constant_or_merged_types_ & 0xFFFF);
    uint16_t type2 = static_cast<uint16_t>(allocation_pc_or_constant_or_merged_types_ >> 16);
    return std::pair<uint16_t, uint16_t>(type1, type2);
  }

  // The complete set of merged types.
  std::set<uint16_t> GetMergedTypes(const RegTypeCache* cache) const;

  uint16_t GetUnresolvedSuperClassChildId() const {
    DCHECK(IsUnresolvedSuperClass());
    return static_cast<uint16_t>(allocation_pc_or_constant_or_merged_types_ & 0xFFFF);
  }

  const RegType& GetSuperClass(RegTypeCache* cache) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  std::string Dump(const RegTypeCache* reg_types = NULL) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Can this type access other?
  bool CanAccess(const RegType& other) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  // Can this type access a member with the given properties?
  bool CanAccessMember(Class* klass, uint32_t access_flags) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Can this type be assigned by src?
  bool IsAssignableFrom(const RegType& src) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  bool Equals(const RegType& other) const { return GetId() == other.GetId(); }

  // Compute the merge of this register from one edge (path) with incoming_type from another.
  const RegType& Merge(const RegType& incoming_type, RegTypeCache* reg_types) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  /*
   * A basic Join operation on classes. For a pair of types S and T the Join, written S v T = J, is
   * S <: J, T <: J and for-all U such that S <: U, T <: U then J <: U. That is J is the parent of
   * S and T such that there isn't a parent of both S and T that isn't also the parent of J (ie J
   * is the deepest (lowest upper bound) parent of S and T).
   *
   * This operation applies for regular classes and arrays, however, for interface types there needn't
   * be a partial ordering on the types. We could solve the problem of a lack of a partial order by
   * introducing sets of types, however, the only operation permissible on an interface is
   * invoke-interface. In the tradition of Java verifiers [1] we defer the verification of interface
   * types until an invoke-interface call on the interface typed reference at runtime and allow
   * the perversion of Object being assignable to an interface type (note, however, that we don't
   * allow assignment of Object or Interface to any concrete class and are therefore type safe).
   *
   * [1] Java bytecode verification: algorithms and formalizations, Xavier Leroy
   */
  static Class* ClassJoin(Class* s, Class* t)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

 private:
  friend class RegTypeCache;

  RegType(Type type, Object* klass_or_descriptor,
          uint32_t allocation_pc_or_constant_or_merged_types, uint16_t cache_id)
      : type_(type), klass_or_descriptor_(klass_or_descriptor),
        allocation_pc_or_constant_or_merged_types_(allocation_pc_or_constant_or_merged_types),
        cache_id_(cache_id) {
    DCHECK(IsConstant() || IsConstantLo() || IsConstantHi() ||
           IsUninitializedTypes() || IsUnresolvedMergedReference() || IsUnresolvedSuperClass() ||
           allocation_pc_or_constant_or_merged_types == 0);
    if (!IsConstant() && !IsLongConstant() && !IsLongConstantHigh() && !IsUndefined() &&
        !IsConflict() && !IsUnresolvedMergedReference() && !IsUnresolvedSuperClass()) {
      DCHECK(klass_or_descriptor != NULL);
      DCHECK(IsUnresolvedTypes() || klass_or_descriptor_->IsClass());
      DCHECK(!IsUnresolvedTypes() || klass_or_descriptor_->GetClass()->IsStringClass());
    }
  }

  const Type type_;  // The current type of the register

  // If known the type of the register, else a String for the descriptor
  Object* klass_or_descriptor_;

  // Overloaded field that:
  //   - if IsConstant() holds a 32bit constant value
  //   - is IsReference() holds the allocation_pc or kInitArgAddr for an initialized reference or
  //     kUninitThisArgAddr for an uninitialized this ptr
  const uint32_t allocation_pc_or_constant_or_merged_types_;

  // A RegType cache densely encodes types, this is the location in the cache for this type
  const uint16_t cache_id_;

  DISALLOW_COPY_AND_ASSIGN(RegType);
};
std::ostream& operator<<(std::ostream& os, const RegType& rhs);

}  // namespace verifier
}  // namespace art

#endif  // ART_SRC_VERIFIER_REG_TYPE_H_
