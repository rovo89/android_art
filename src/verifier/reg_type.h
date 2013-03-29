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
#include "primitive.h"

#include "jni.h"

#include <limits>
#include <stdint.h>
#include <set>
#include <string>

namespace art {
namespace mirror {
class Class;
}  // namespace mirror
namespace verifier {

class RegTypeCache;
/*
 * RegType holds information about the "type" of data held in a register.
 */
class RegType {
 public:
  // The high half that corresponds to this low half
  const RegType& HighHalf(RegTypeCache* cache) const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  inline virtual bool IsUndefined() const {
    return false;
  }
  inline virtual bool IsConflict() const {
    return false;
  }
  inline virtual bool IsBoolean() const {
    return false;
  }
  inline virtual bool IsByte() const {
    return false;
  }
  inline virtual bool IsChar() const {
    return false;
  }
  inline virtual bool IsShort() const {
    return false;
  }
  inline virtual bool IsInteger() const {
    return false;
  }
  inline virtual bool IsLongLo() const {
    return false;
  }
  inline virtual bool IsLongHi() const {
    return false;
  }
  inline virtual bool IsFloat() const {
    return false;
  }
  inline virtual bool IsDouble() const {
    return false;
  }
  inline virtual bool IsDoubleLo() const {
    return false;
  }
  inline virtual bool IsDoubleHi() const {
    return false;
  }
  inline virtual bool IsUnresolvedReference() const {
    return false;
  }
  inline virtual bool IsUninitializedReference() const {
    return false;
  }
  inline virtual bool IsUninitializedThisReference() const {
    return false;
  }
  inline virtual bool IsUnresolvedAndUninitializedReference() const {
    return false;
  }
  inline virtual bool IsUnresolvedAndUninitializedThisReference() const {
    return false;
  }
  inline virtual bool IsUnresolvedMergedReference() const {
    return false;
  }
  inline virtual bool IsUnresolvedSuperClass() const {
    return false;
  }
  inline virtual bool IsReference() const {
    return false;
  }
  inline virtual bool IsPreciseReference() const {
    return false;
  }
  inline virtual bool IsPreciseConstant() const {
    return false;
  }
  inline virtual bool IsPreciseConstantLo() const {
    return false;
  }
  inline virtual bool IsPreciseConstantHi() const {
    return false;
  }
  inline virtual bool IsImpreciseConstantLo() const {
    return false;
  }
  inline virtual bool IsImpreciseConstantHi() const {
    return false;
  }
  virtual bool IsImpreciseConstant() const {
    return false;
  }

  inline virtual bool IsConstantTypes() const {
    return false;
  }
  bool IsConstant() const {
    return (IsPreciseConstant() || IsImpreciseConstant());
  }
  bool IsConstantLo() const {
    return (IsPreciseConstantLo() || IsImpreciseConstantLo());
  }
  bool IsPrecise() const {
    return (IsPreciseConstantLo() || IsPreciseConstant() ||
            IsPreciseConstantHi());
  }
  bool IsLongConstant() const {
    return IsConstantLo();
  }
  bool IsConstantHi() const {
    return (IsPreciseConstantHi() || IsImpreciseConstantHi());
  }
  bool IsLongConstantHigh() const {
    return IsConstantHi();
  }
  bool IsUninitializedTypes() const {
    return IsUninitializedReference() || IsUninitializedThisReference() ||
           IsUnresolvedAndUninitializedReference() ||
           IsUnresolvedAndUninitializedThisReference();
  }
  bool IsUnresolvedTypes() const {
    return IsUnresolvedReference() || IsUnresolvedAndUninitializedReference() ||
           IsUnresolvedAndUninitializedThisReference() ||
           IsUnresolvedMergedReference() || IsUnresolvedSuperClass();
  }

  bool IsLowHalf() const {
    return (IsLongLo() || IsDoubleLo() || IsPreciseConstantLo() ||
            IsImpreciseConstantLo());
  }
  bool IsHighHalf() const {
    return (IsLongHi() || IsDoubleHi() || IsPreciseConstantHi() ||
            IsImpreciseConstantHi());
  }
  bool IsLongOrDoubleTypes() const {
    return IsLowHalf();
  }
  // Check this is the low half, and that type_h is its matching high-half
  inline bool CheckWidePair(const RegType& type_h) const {
    return true;
    if (IsLowHalf()) {
      return ((IsPreciseConstantLo() && type_h.IsImpreciseConstantHi()) ||
              (IsImpreciseConstantLo() && type_h.IsPreciseConstantHi()) ||
              (IsDoubleLo() && type_h.IsDoubleHi()) ||
              (IsLongLo() && type_h.IsLongHi()));
    }
    return false;
  }
  bool IsConstantBoolean() const {
    return IsConstant() && (ConstantValue() >= 0) && (ConstantValue() <= 1);
  }
  inline virtual bool IsConstantChar() const {
    return false;
  }
  inline virtual bool IsConstantByte() const {
    return false;
  }
  inline virtual bool IsConstantShort() const {
    return false;
  }
  inline virtual bool IsOne() const {
    return false;
  }
  inline virtual bool IsZero() const {
    return false;
  }
  bool IsReferenceTypes() const {
    return IsNonZeroReferenceTypes() || IsZero();
  }
  bool IsNonZeroReferenceTypes() const {
    return IsReference() || IsPreciseReference() ||
           IsUninitializedReference() || IsUninitializedThisReference() ||
           IsUnresolvedReference() || IsUnresolvedAndUninitializedReference() ||
           IsUnresolvedAndUninitializedThisReference() ||
           IsUnresolvedMergedReference() || IsUnresolvedSuperClass();
  }
  bool IsCategory1Types() const {
    return (IsBoolean() || IsByte() || IsShort() || IsChar() || IsInteger() ||
            IsFloat() || IsConstant());
  }
  bool IsCategory2Types() const {
    return IsLowHalf();  // Don't expect explicit testing of high halves
  }
  bool IsBooleanTypes() const {
    return IsBoolean() || IsConstantBoolean();
  }
  bool IsByteTypes() const {
    return IsByte() || IsBoolean() || IsConstantByte();
  }
  bool IsShortTypes() const {
    return IsShort() || IsByte() || IsBoolean() || IsConstantShort();
  }
  bool IsCharTypes() const {
    return IsChar() || IsBooleanTypes() || IsConstantChar();
  }
  bool IsIntegralTypes() const {
    return (IsBoolean() || IsByte() || IsShort() || IsChar() || IsInteger() || IsConstant());
  }
  inline virtual int32_t ConstantValue() const {
    CHECK(IsConstant());
    return -1;
  }
  inline virtual int32_t ConstantValueLo() const {
    CHECK(IsConstantLo());
    return -1;
  }
  inline virtual int32_t ConstantValueHi() const {
    CHECK(IsConstantHi());
    return -1;
  }
  bool IsArrayIndexTypes() const {
    return IsIntegralTypes();
  }
  // Float type may be derived from any constant type
  bool IsFloatTypes() const {
    return IsFloat() || IsConstant();
  }
  bool IsLongTypes() const {
    return IsLongLo() || IsLongConstant();
  }
  bool IsLongHighTypes() const {
    return (IsLongConstantHigh() || IsPreciseConstantHi() ||
            IsImpreciseConstantHi());
  }
  bool IsDoubleTypes() const {
    return IsDoubleLo() || IsLongConstant();
  }
  bool IsDoubleHighTypes() const {
    return (IsDoubleHi() || IsPreciseConstantHi() || IsImpreciseConstantHi());
  }
  inline virtual bool IsLong() const {
    return false;
  }
  bool HasClass() const {
    return IsReference() || IsPreciseReference() || IsUninitializedReference() ||
           IsUninitializedThisReference();
  }
  bool IsJavaLangObject() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  bool IsArrayTypes() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  bool IsObjectArrayTypes() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  Primitive::Type GetPrimitiveType() const ;
  bool IsJavaLangObjectArray() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  bool IsInstantiableTypes() const;
  std::string GetDescriptor() const {
    DCHECK(IsUnresolvedTypes() && !IsUnresolvedMergedReference() && !IsUnresolvedSuperClass());
    return descriptor_;
  }
  uint16_t GetId() const {
    return cache_id_;
  }
  const RegType& GetSuperClass(RegTypeCache* cache) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  virtual std::string Dump() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) = 0;
  // Can this type access other?
  bool CanAccess(const RegType& other) const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  // Can this type access a member with the given properties?
  bool CanAccessMember(mirror::Class* klass, uint32_t access_flags) const
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  // Can this type be assigned by src?
  bool IsAssignableFrom(const RegType& src) const  SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  bool Equals(const RegType& other) const {
    return GetId() == other.GetId();
  }
  // Compute the merge of this register from one edge (path) with incoming_type from another.
  virtual const RegType& Merge(const RegType& incoming_type, RegTypeCache* reg_types) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  mirror::Class* GetClass() const {
    DCHECK(!IsUnresolvedReference());
    DCHECK(klass_ != NULL);
    DCHECK(HasClass());
    return klass_;
  }
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

  static mirror::Class* ClassJoin(mirror::Class* s, mirror::Class* t)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  RegType(mirror::Class* klass, std::string descriptor, uint16_t cache_id)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : descriptor_(descriptor), klass_(klass), cache_id_(cache_id) {
  }
  inline virtual ~RegType() {
  }
  virtual void CheckInvariants() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  friend class RegTypeCache;

 protected:
  const std::string descriptor_;
  mirror::Class* klass_;
  const uint16_t cache_id_;

  DISALLOW_COPY_AND_ASSIGN(RegType);
};

class ConflictType : public RegType {
 public:
  inline virtual bool IsConflict() const {
    return true;
  }
  virtual std::string Dump() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static ConflictType* CreateInstance(mirror::Class* klass, std::string& descriptor,
                                      uint16_t cache_id) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static ConflictType* GetInstance();
  static void Destroy();
 private:
  ConflictType(mirror::Class* klass, std::string& descriptor, uint16_t cache_id)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static ConflictType* instance_;
};

class UndefinedType : public RegType {
 public:
  inline virtual bool IsUndefined() const {
    return true;
  }
  virtual std::string Dump() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static UndefinedType* CreateInstance(mirror::Class* klass, std::string& descriptor,
                                       uint16_t cache_id)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static UndefinedType* GetInstance();
  static void Destroy();
 private:
  UndefinedType(mirror::Class* klass, std::string& descriptor, uint16_t cache_id)
     SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : RegType(klass, descriptor, cache_id) {
  }
  virtual const RegType& Merge(const RegType& incoming_type, RegTypeCache* reg_types) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static UndefinedType* instance_;
};

class IntegerType : public RegType {
 public:
  inline virtual bool IsInteger() const {
    return true;
  }
  virtual std::string Dump() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static IntegerType* CreateInstance(mirror::Class* klass, std::string& descriptor,
                                     uint16_t cache_id)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static IntegerType* GetInstance();
  static void Destroy();
 private:
  IntegerType(mirror::Class* klass, std::string& descriptor, uint16_t cache_id)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static IntegerType* instance_;
};

class BooleanType : public RegType {
 public:
  inline virtual bool IsBoolean() const {
    return true;
  }
  virtual std::string Dump() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static BooleanType* CreateInstance(mirror::Class* klass, std::string& descriptor,
                                     uint16_t cache_id)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static BooleanType* GetInstance();
  static void Destroy();
 private:
  BooleanType(mirror::Class* klass, std::string& descriptor, uint16_t cache_id)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static BooleanType* instance;
};

class ByteType : public RegType {
 public:
  inline virtual bool IsByte() const {
    return true;
  }
  virtual std::string Dump() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static ByteType* CreateInstance(mirror::Class* klass, std::string& descriptor,
                                  uint16_t cache_id)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static ByteType* GetInstance();
  static void Destroy();
 private:
  ByteType(mirror::Class* klass, std::string& descriptor, uint16_t cache_id)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static ByteType* instance_;
};

class ShortType : public RegType {
 public:
  inline virtual bool IsShort() const {
    return true;
  }
  virtual std::string Dump() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static ShortType* CreateInstance(mirror::Class* klass, std::string& descriptor,
                                   uint16_t cache_id)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static ShortType* GetInstance();
  static void Destroy();
 private:
  ShortType(mirror::Class* klass, std::string& descriptor, uint16_t cache_id)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static ShortType* instance_;
};

class CharType : public RegType {
 public:
  inline virtual bool IsChar() const {
    return true;
  }
  virtual std::string Dump() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static CharType* CreateInstance(mirror::Class* klass, std::string& descriptor,
                                  uint16_t cache_id)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static CharType* GetInstance();
  static void Destroy();
 private:
  CharType(mirror::Class* klass, std::string& descriptor, uint16_t cache_id)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static CharType* instance_;
};

class FloatType : public RegType {
 public:
  inline virtual bool IsFloat() const {
    return true;
  }
  virtual std::string Dump() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static FloatType* CreateInstance(mirror::Class* klass, std::string& descriptor,
                                   uint16_t cache_id)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static FloatType* GetInstance();
  static void Destroy();
 private:
  FloatType(mirror::Class* klass, std::string& descriptor, uint16_t cache_id)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static FloatType* instance_;
};

class LongLoType : public RegType {
 public:
  virtual std::string Dump() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  inline virtual bool IsLongLo() const {
    return true;
  }
  inline virtual bool IsLong() const {
    return true;
  }
  static LongLoType* CreateInstance(mirror::Class* klass, std::string& descriptor,
                                    uint16_t cache_id)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
   static LongLoType* GetInstance();
  static void Destroy();
 private:
  LongLoType(mirror::Class* klass, std::string& descriptor, uint16_t cache_id)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static LongLoType* instance_;
};

class LongHiType : public RegType {
 public:
  virtual std::string Dump() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  inline virtual bool IsLongHi() const {
    return true;
  }
  static LongHiType* CreateInstance(mirror::Class* klass, std::string& descriptor,
                                    uint16_t cache_id)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static LongHiType* GetInstance();
  static void Destroy();
 private:
  LongHiType(mirror::Class* klass, std::string& descriptor, uint16_t cache_id)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static LongHiType* instance_;
};

class DoubleLoType : public RegType {
 public:
  virtual std::string Dump() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  inline virtual bool IsDoubleLo() const {
    return true;
  }
  inline virtual bool IsDouble() const {
    return true;
  }
  static DoubleLoType* CreateInstance(mirror::Class* klass, std::string& descriptor,
                                      uint16_t cache_id)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static DoubleLoType* GetInstance();
  static void Destroy();
 private:
  DoubleLoType(mirror::Class* klass, std::string& descriptor, uint16_t cache_id)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static DoubleLoType* instance_;
};

class DoubleHiType : public RegType {
 public:
  virtual std::string Dump() const;
  inline virtual bool IsDoubleHi() const {
    return true;
  }
  static DoubleHiType* CreateInstance(mirror::Class* klass, std::string& descriptor,
                                      uint16_t cache_id)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static DoubleHiType* GetInstance();
  static void Destroy();
 private:
  DoubleHiType(mirror::Class* klass, std::string& descriptor, uint16_t cache_id)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static DoubleHiType* instance_;
};

class ConstantType : public RegType {
 public:
  ConstantType(uint32_t constat, uint16_t cache_id) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  inline virtual ~ConstantType() {
  }
  const uint32_t constant_;
  // If this is a 32-bit constant, what is the value? This value may be imprecise in which case
  // the value represents part of the integer range of values that may be held in the register.
  virtual int32_t ConstantValue() const;
  virtual int32_t ConstantValueLo() const;
  virtual int32_t ConstantValueHi() const;

  bool IsZero() const {
    return IsPreciseConstant() && ConstantValue() == 0;
  }
  bool IsOne() const {
    return IsPreciseConstant() && ConstantValue() == 1;
  }

  bool IsConstantChar() const {
    return IsConstant() && ConstantValue() >= 0 &&
           ConstantValue() <= std::numeric_limits<jchar>::max();
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
  inline virtual bool IsConstantTypes() const { return true; }
};
class PreciseConstType : public ConstantType {
 public:
  PreciseConstType(uint32_t constat, uint16_t cache_id) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : ConstantType(constat, cache_id) {
  }

  inline virtual bool IsPreciseConstant() const {
    return true;
  }

  virtual std::string Dump() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
};
class PreciseConstLoType : public ConstantType {
 public:
  PreciseConstLoType(uint32_t constat, uint16_t cache_id)
     SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : ConstantType(constat, cache_id) {
  }
  inline virtual bool IsPreciseConstantLo() const {
    return true;
  }
  virtual std::string Dump() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
};
class PreciseConstHiType : public ConstantType {
 public:
  PreciseConstHiType(uint32_t constat, uint16_t cache_id)
     SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : ConstantType(constat, cache_id) {
  }
  inline virtual bool IsPreciseConstantHi() const {
    return true;
  }
  virtual std::string Dump() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
};

class ImpreciseConstType : public ConstantType {
 public:
  ImpreciseConstType(uint32_t constat, uint16_t cache_id)
     SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  virtual  bool IsImpreciseConstant() const {
    return true;
  }
  virtual std::string Dump() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
};
class ImpreciseConstLoType : public ConstantType {
 public:
  ImpreciseConstLoType(uint32_t constat, uint16_t cache_id)
     SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) : ConstantType(constat, cache_id) {
  }
  inline virtual bool IsImpreciseConstantLo() const {
    return true;
  }
  virtual std::string Dump() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
};
class ImpreciseConstHiType : public ConstantType {
 public:
  ImpreciseConstHiType(uint32_t constat, uint16_t cache_id)
     SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) : ConstantType(constat, cache_id) {
  }
  inline virtual bool IsImpreciseConstantHi() const {
    return true;
  }
  virtual std::string Dump() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
};
class UninitializedType : public RegType {
 public:
  UninitializedType(mirror::Class* klass, std::string& descriptor, uint32_t allocation_pc,
                    uint16_t cache_id);
  inline virtual ~UninitializedType() {
  }

  uint32_t GetAllocationPc() const {
    DCHECK(IsUninitializedTypes());
    return allocation_pc_;
  }
  virtual void CheckInvariants() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

 private:
  const uint32_t allocation_pc_;
};

class UninitialisedReferenceType : public UninitializedType {
 public:
  UninitialisedReferenceType(mirror::Class* klass, std::string& descriptor, uint32_t allocation_pc,
                             uint16_t cache_id) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  inline virtual bool IsUninitializedReference() const {
    return true;
  }
  virtual std::string Dump() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
};

class UnresolvedUninitializedRefType : public UninitializedType {
 public:
  UnresolvedUninitializedRefType(std::string& descriptor, uint32_t allocation_pc,
                                 uint16_t cache_id) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  virtual void CheckInvariants() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  inline virtual bool IsUnresolvedAndUninitializedReference() const {
    return true;
  }
  virtual std::string Dump() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
};

class UninitialisedThisReferenceType : public UninitializedType {
 public:
  UninitialisedThisReferenceType(mirror::Class* klass, std::string& descriptor, uint16_t cache_id)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  virtual std::string Dump() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  virtual void CheckInvariants() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  inline virtual bool IsUninitializedThisReference() const {
    return true;
  }
};
class UnresolvedUninitialisedThisRefType : public UninitializedType {
 public:
  UnresolvedUninitialisedThisRefType(std::string& descriptor, uint16_t cache_id)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  virtual void CheckInvariants() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  virtual std::string Dump() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  inline virtual bool IsUnresolvedAndUninitializedThisReference() const {
    return true;
  }
};

class ReferenceType : public RegType {
 public:
  ReferenceType(mirror::Class* klass, std::string& descriptor, uint16_t cache_id)
     SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  inline virtual bool IsReference() const {
    return true;
  }
  virtual std::string Dump() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
};

class PreciseReferenceType : public RegType {
 public:
  PreciseReferenceType(mirror::Class* klass, std::string& descriptor, uint16_t cache_id)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  virtual std::string Dump() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  inline virtual bool IsPreciseReference() const {
    return true;
  }
};

class UnresolvedReferenceType : public RegType {
 public:
  UnresolvedReferenceType(std::string& descriptor, uint16_t cache_id)
     SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) : RegType(NULL, descriptor, cache_id) {
  }
  virtual std::string Dump() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  virtual void CheckInvariants() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  inline virtual bool IsUnresolvedReference() const {
    return true;
  }
};

class UnresolvedSuperClass : public RegType {
 public:
  UnresolvedSuperClass(uint16_t child_id, RegTypeCache* reg_type_cache, uint16_t cache_id)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : RegType(NULL, "", cache_id), unresolved_child_id_(child_id),
        reg_type_cache_(reg_type_cache) {
  }
  inline virtual bool IsUnresolvedSuperClass() const {
    return true;
  }
  uint16_t GetUnresolvedSuperClassChildId() const {
    DCHECK(IsUnresolvedSuperClass());
    return static_cast<uint16_t>(unresolved_child_id_ & 0xFFFF);
  }
  virtual void CheckInvariants() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  virtual std::string Dump() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

 private:
 const uint16_t unresolved_child_id_;
 const RegTypeCache* const reg_type_cache_;
};

class UnresolvedMergedType : public RegType {
 public:
  UnresolvedMergedType(uint16_t left_id, uint16_t right_id, const RegTypeCache* reg_type_cache, uint16_t cache_id)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : RegType(NULL, "", cache_id), reg_type_cache_(reg_type_cache) ,merged_types_(left_id, right_id) {
  }
  // The top of a tree of merged types.
  std::pair<uint16_t, uint16_t> GetTopMergedTypes() const {
    DCHECK(IsUnresolvedMergedReference());
    return merged_types_;
  }
  // The complete set of merged types.
  std::set<uint16_t> GetMergedTypes() const;
  inline virtual bool IsUnresolvedMergedReference() const {
    return true;
  }
  virtual void CheckInvariants() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  virtual std::string Dump() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

 private:
  const RegTypeCache* const reg_type_cache_;
  const std::pair<uint16_t, uint16_t> merged_types_;
};

std::ostream& operator<<(std::ostream& os, const RegType& rhs)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
}  // namespace verifier
}  // namespace art

#endif  // ART_SRC_VERIFIER_REG_TYPE_H_
