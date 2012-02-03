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

#ifndef ART_SRC_DEX_VERIFY_H_
#define ART_SRC_DEX_VERIFY_H_

#include <deque>
#include <limits>
#include <map>
#include <vector>

#include "casts.h"
#include "compiler.h"
#include "dex_file.h"
#include "dex_instruction.h"
#include "macros.h"
#include "object.h"
#include "stl_util.h"
#include "UniquePtr.h"

namespace art {

namespace verifier {

class DexVerifier;
class PcToReferenceMap;
class RegTypeCache;

/*
 * Set this to enable dead code scanning. This is not required, but it's very useful when testing
 * changes to the verifier (to make sure we're not skipping over stuff). The only reason not to do
 * it is that it slightly increases the time required to perform verification.
 */
#ifndef NDEBUG
# define DEAD_CODE_SCAN  true
#else
# define DEAD_CODE_SCAN  false
#endif

/*
 * RegType holds information about the "type" of data held in a register.
 */
class RegType {
 public:
  enum Type {
    kRegTypeUnknown = 0,    /* initial state */
    kRegTypeConflict,       /* merge clash makes this reg's type unknowable */
    kRegTypeBoolean,        /* Z */
    kRegType1nrSTART = kRegTypeBoolean,
    kRegTypeIntegralSTART = kRegTypeBoolean,
    kRegTypeByte,           /* B */
    kRegTypeShort,          /* S */
    kRegTypeChar,           /* C */
    kRegTypeInteger,        /* I */
    kRegTypeIntegralEND = kRegTypeInteger,
    kRegTypeFloat,          /* F */
    kRegType1nrEND = kRegTypeFloat,
    kRegTypeLongLo,         /* J - lower-numbered register; endian-independent */
    kRegTypeLongHi,
    kRegTypeDoubleLo,       /* D */
    kRegTypeDoubleHi,
    kRegTypeConstLo,        /* const derived wide, lower half - could be long or double */
    kRegTypeConstHi,        /* const derived wide, upper half - could be long or double */
    kRegTypeLastFixedLocation = kRegTypeConstHi,
    kRegTypeConst,          /* 32-bit constant derived value - could be float or int */
    kRegTypeUnresolvedReference,        // Reference type that couldn't be resolved
    kRegTypeUninitializedReference,     // Freshly allocated reference type
    kRegTypeUninitializedThisReference, // Freshly allocated reference passed as "this"
    kRegTypeUnresolvedAndUninitializedReference, // Freshly allocated unresolved reference type
    kRegTypeReference,                  // Reference type
  };

  Type GetType() const {
    return type_;
  }

  bool IsUnknown() const { return type_ == kRegTypeUnknown; }
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
  bool IsReference() const { return type_ == kRegTypeReference; }
  bool IsUninitializedTypes() const {
    return IsUninitializedReference() || IsUninitializedThisReference() ||
        IsUnresolvedAndUninitializedReference();
  }
  bool IsUnresolvedTypes() const {
    return IsUnresolvedReference() || IsUnresolvedAndUninitializedReference();
  }
  bool IsLowHalf() const { return type_ == kRegTypeLongLo ||
                                  type_ == kRegTypeDoubleLo ||
                                  type_ == kRegTypeConstLo; }
  bool IsHighHalf() const { return type_ == kRegTypeLongHi ||
                                   type_ == kRegTypeDoubleHi ||
                                   type_ == kRegTypeConstHi; }

  bool IsLongOrDoubleTypes() const { return IsLowHalf(); }

  // Check this is the low half, and that type_h is its matching high-half
  bool CheckWidePair(const RegType& type_h) const {
    return IsLowHalf() && (type_h.type_ == type_ + 1);
  }

  // The high half that corresponds to this low half
  const RegType& HighHalf(RegTypeCache* cache) const;

  bool IsConstant() const { return type_ == kRegTypeConst; }
  bool IsLongConstant() const { return type_ == kRegTypeConstLo; }
  bool IsLongConstantHigh() const { return type_ == kRegTypeConstHi; }

  // If this is a 32-bit constant, what is the value? This value may just
  // approximate to the actual constant value by virtue of merging.
  int32_t ConstantValue() const {
    DCHECK(IsConstant());
    return allocation_pc_or_constant_;
  }

  bool IsZero()         const { return IsConstant() && ConstantValue() == 0; }
  bool IsOne()          const { return IsConstant() && ConstantValue() == 1; }
  bool IsConstantBoolean() const { return IsZero() || IsOne(); }
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
    return IsReference() || IsUnresolvedReference() || IsUninitializedReference() ||
        IsUninitializedThisReference() || IsUnresolvedAndUninitializedReference() || IsZero();
  }
  bool IsNonZeroReferenceTypes() const {
    return IsReference() || IsUnresolvedReference() || IsUninitializedReference() ||
        IsUninitializedThisReference();
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
    return type_ == kRegTypeLongHi || type_ == kRegTypeConstHi;
  }
  bool IsDoubleTypes() const {
    return IsDouble() || IsLongConstant();
  }
  bool IsDoubleHighTypes() const {
    return type_ == kRegTypeDoubleHi || type_ == kRegTypeConstHi;
  }

  uint32_t GetAllocationPc() const {
    DCHECK(IsUninitializedTypes());
    return allocation_pc_or_constant_;
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

  bool IsArrayTypes() const {
    if (IsUnresolvedTypes()) {
      return GetDescriptor()->CharAt(0) == '[';
    } else if (!IsConstant()) {
      return GetClass()->IsArrayClass();
    } else {
      return false;
    }
  }

  bool IsObjectArrayTypes() const {
    if (IsUnresolvedTypes()) {
      // Primitive arrays will always resolve
      DCHECK(GetDescriptor()->CharAt(1) == 'L' || GetDescriptor()->CharAt(1) == '[');
      return GetDescriptor()->CharAt(0) == '[';
    } else if (!IsConstant()) {
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
    if (IsReference()) {
      Class* type = GetClass();
      return type->IsArrayClass() && type->GetComponentType()->IsObjectClass();
    }
    return false;
  }

  bool IsInstantiableTypes() const {
    return IsUnresolvedTypes() || (IsNonZeroReferenceTypes() && GetClass()->IsInstantiable());
  }

  String* GetDescriptor() const {
    DCHECK(IsUnresolvedTypes());
    DCHECK(klass_or_descriptor_ != NULL);
    DCHECK(klass_or_descriptor_->GetClass()->IsStringClass());
    return down_cast<String*>(klass_or_descriptor_);
  }

  uint16_t GetId() const {
    return cache_id_;
  }

  std::string Dump() const;

  bool IsAssignableFrom(const RegType& src) const;

  const RegType& Merge(const RegType& incoming_type, RegTypeCache* reg_types) const;

  bool Equals(const RegType& other) const { return GetId() == other.GetId(); }

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
  static Class* ClassJoin(Class* s, Class* t);

 private:
  friend class RegTypeCache;

  RegType(Type type, Object* klass_or_descriptor, uint32_t allocation_pc_or_constant, uint16_t cache_id) :
    type_(type), klass_or_descriptor_(klass_or_descriptor), allocation_pc_or_constant_(allocation_pc_or_constant),
    cache_id_(cache_id) {
    DCHECK(IsConstant() || IsUninitializedTypes() || allocation_pc_or_constant == 0);
    if (!IsConstant() && !IsLongConstant() && !IsLongConstantHigh() && !IsUnknown() &&
        !IsConflict()) {
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
  const uint32_t allocation_pc_or_constant_;

  // A RegType cache densely encodes types, this is the location in the cache for this type
  const uint16_t cache_id_;

  DISALLOW_COPY_AND_ASSIGN(RegType);
};
std::ostream& operator<<(std::ostream& os, const RegType& rhs);

class RegTypeCache {
 public:
  explicit RegTypeCache() : entries_(RegType::kRegTypeLastFixedLocation + 1) {
    Unknown();  // ensure Unknown is initialized
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

  const RegType& Unknown()  { return FromType(RegType::kRegTypeUnknown); }
  const RegType& Conflict() { return FromType(RegType::kRegTypeConflict); }
  const RegType& ConstLo()  { return FromType(RegType::kRegTypeConstLo); }
  const RegType& Zero()     { return FromCat1Const(0); }

  const RegType& Uninitialized(const RegType& type, uint32_t allocation_pc);
  const RegType& UninitializedThisArgument(Class* klass);
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

class InsnFlags {
 public:
  InsnFlags() : length_(0), flags_(0) {}

  void SetLengthInCodeUnits(size_t length) {
    CHECK_LT(length, 65536u);
    length_ = length;
  }
  size_t GetLengthInCodeUnits() {
    return length_;
  }
  bool IsOpcode() const {
    return length_ != 0;
  }

  void SetInTry() {
    flags_ |= 1 << kInsnFlagInTry;
  }
  void ClearInTry() {
    flags_ &= ~(1 << kInsnFlagInTry);
  }
  bool IsInTry() const {
    return (flags_ & (1 << kInsnFlagInTry)) != 0;
  }

  void SetBranchTarget() {
    flags_ |= 1 << kInsnFlagBranchTarget;
  }
  void ClearBranchTarget() {
    flags_ &= ~(1 << kInsnFlagBranchTarget);
  }
  bool IsBranchTarget() const {
    return (flags_ & (1 << kInsnFlagBranchTarget)) != 0;
  }

  void SetGcPoint() {
    flags_ |= 1 << kInsnFlagGcPoint;
  }
  void ClearGcPoint() {
    flags_ &= ~(1 << kInsnFlagGcPoint);
  }
  bool IsGcPoint() const {
    return (flags_ & (1 << kInsnFlagGcPoint)) != 0;
  }

  void SetVisited() {
    flags_ |= 1 << kInsnFlagVisited;
  }
  void ClearVisited() {
    flags_ &= ~(1 << kInsnFlagVisited);
  }
  bool IsVisited() const {
    return (flags_ & (1 << kInsnFlagVisited)) != 0;
  }

  void SetChanged() {
    flags_ |= 1 << kInsnFlagChanged;
  }
  void ClearChanged() {
    flags_ &= ~(1 << kInsnFlagChanged);
  }
  bool IsChanged() const {
    return (flags_ & (1 << kInsnFlagChanged)) != 0;
  }

  bool IsVisitedOrChanged() const {
    return IsVisited() || IsChanged();
  }

  std::string Dump() {
    char encoding[6];
    if (!IsOpcode()) {
      strncpy(encoding, "XXXXX", sizeof(encoding));
    } else {
      strncpy(encoding, "-----", sizeof(encoding));
      if (IsInTry())        encoding[kInsnFlagInTry] = 'T';
      if (IsBranchTarget()) encoding[kInsnFlagBranchTarget] = 'B';
      if (IsGcPoint())      encoding[kInsnFlagGcPoint] = 'G';
      if (IsVisited())      encoding[kInsnFlagVisited] = 'V';
      if (IsChanged())      encoding[kInsnFlagChanged] = 'C';
    }
    return std::string(encoding);
  }
 private:
  enum InsnFlag {
    kInsnFlagInTry,
    kInsnFlagBranchTarget,
    kInsnFlagGcPoint,
    kInsnFlagVisited,
    kInsnFlagChanged,
  };

  // Size of instruction in code units
  uint16_t length_;
  uint8_t flags_;
};

/*
 * "Direct" and "virtual" methods are stored independently. The type of call used to invoke the
 * method determines which list we search, and whether we travel up into superclasses.
 *
 * (<clinit>, <init>, and methods declared "private" or "static" are stored in the "direct" list.
 * All others are stored in the "virtual" list.)
 */
enum MethodType {
  METHOD_UNKNOWN  = 0,
  METHOD_DIRECT,      // <init>, private
  METHOD_STATIC,      // static
  METHOD_VIRTUAL,     // virtual, super
  METHOD_INTERFACE    // interface
};

const int kRegTypeUninitMask = 0xff;
const int kRegTypeUninitShift = 8;

/*
 * Register type categories, for type checking.
 *
 * The spec says category 1 includes boolean, byte, char, short, int, float, reference, and
 * returnAddress. Category 2 includes long and double.
 *
 * We treat object references separately, so we have "category1nr". We don't support jsr/ret, so
 * there is no "returnAddress" type.
 */
enum TypeCategory {
  kTypeCategoryUnknown = 0,
  kTypeCategory1nr = 1,         // boolean, byte, char, short, int, float
  kTypeCategory2 = 2,           // long, double
  kTypeCategoryRef = 3,         // object reference
};

/*
 * An enumeration of problems that can turn up during verification.
 * VERIFY_ERROR_GENERIC denotes a failure that causes the entire class to be rejected. Other errors
 * denote verification errors that cause bytecode to be rewritten to fail at runtime.
 */
enum VerifyError {
  VERIFY_ERROR_NONE = 0,      /* no error; must be zero */
  VERIFY_ERROR_GENERIC,       /* VerifyError */

  VERIFY_ERROR_NO_CLASS,      /* NoClassDefFoundError */
  VERIFY_ERROR_NO_FIELD,      /* NoSuchFieldError */
  VERIFY_ERROR_NO_METHOD,     /* NoSuchMethodError */
  VERIFY_ERROR_ACCESS_CLASS,  /* IllegalAccessError */
  VERIFY_ERROR_ACCESS_FIELD,  /* IllegalAccessError */
  VERIFY_ERROR_ACCESS_METHOD, /* IllegalAccessError */
  VERIFY_ERROR_CLASS_CHANGE,  /* IncompatibleClassChangeError */
  VERIFY_ERROR_INSTANTIATION, /* InstantiationError */
};
std::ostream& operator<<(std::ostream& os, const VerifyError& rhs);

/*
 * Identifies the type of reference in the instruction that generated the verify error
 * (e.g. VERIFY_ERROR_ACCESS_CLASS could come from a method, field, or class reference).
 *
 * This must fit in two bits.
 */
enum VerifyErrorRefType {
  VERIFY_ERROR_REF_CLASS  = 0,
  VERIFY_ERROR_REF_FIELD  = 1,
  VERIFY_ERROR_REF_METHOD = 2,
};
const int kVerifyErrorRefTypeShift = 6;

/*
 * Format enumeration for RegisterMap data area.
 */
enum RegisterMapFormat {
  kRegMapFormatUnknown = 0,
  kRegMapFormatNone,          /* indicates no map data follows */
  kRegMapFormatCompact8,      /* compact layout, 8-bit addresses */
  kRegMapFormatCompact16,     /* compact layout, 16-bit addresses */
};

// During verification, we associate one of these with every "interesting" instruction. We track
// the status of all registers, and (if the method has any monitor-enter instructions) maintain a
// stack of entered monitors (identified by code unit offset).
// If live-precise register maps are enabled, the "liveRegs" vector will be populated. Unlike the
// other lists of registers here, we do not track the liveness of the method result register
// (which is not visible to the GC).
class RegisterLine {
 public:
  RegisterLine(size_t num_regs, DexVerifier* verifier) :
    line_(new uint16_t[num_regs]), verifier_(verifier), num_regs_(num_regs) {
    memset(line_.get(), 0, num_regs_ * sizeof(uint16_t));
    result_[0] = RegType::kRegTypeUnknown;
    result_[1] = RegType::kRegTypeUnknown;
  }

  // Implement category-1 "move" instructions. Copy a 32-bit value from "vsrc" to "vdst".
  void CopyRegister1(uint32_t vdst, uint32_t vsrc, TypeCategory cat);

  // Implement category-2 "move" instructions. Copy a 64-bit value from "vsrc" to "vdst". This
  // copies both halves of the register.
  void CopyRegister2(uint32_t vdst, uint32_t vsrc);

  // Implement "move-result". Copy the category-1 value from the result register to another
  // register, and reset the result register.
  void CopyResultRegister1(uint32_t vdst, bool is_reference);

  // Implement "move-result-wide". Copy the category-2 value from the result register to another
  // register, and reset the result register.
  void CopyResultRegister2(uint32_t vdst);

  // Set the invisible result register to unknown
  void SetResultTypeToUnknown();

  // Set the type of register N, verifying that the register is valid.  If "newType" is the "Lo"
  // part of a 64-bit value, register N+1 will be set to "newType+1".
  // The register index was validated during the static pass, so we don't need to check it here.
  void SetRegisterType(uint32_t vdst, const RegType& new_type);

  /* Set the type of the "result" register. */
  void SetResultRegisterType(const RegType& new_type);

  // Get the type of register vsrc.
  const RegType& GetRegisterType(uint32_t vsrc) const;

  bool VerifyRegisterType(uint32_t vsrc, const RegType& check_type);

  void CopyFromLine(const RegisterLine* src) {
    DCHECK_EQ(num_regs_, src->num_regs_);
    memcpy(line_.get(), src->line_.get(), num_regs_ * sizeof(uint16_t));
    monitors_ = src->monitors_;
    reg_to_lock_depths_ = src->reg_to_lock_depths_;
  }

  std::string Dump() const {
    std::string result;
    for (size_t i = 0; i < num_regs_; i++) {
      result += StringPrintf("%zd:[", i);
      result += GetRegisterType(i).Dump();
      result += "],";
    }
    typedef std::deque<uint32_t>::const_iterator It; // TODO: C++0x auto
    for (It it = monitors_.begin(), end = monitors_.end(); it != end ; ++it) {
      result += StringPrintf("{%d},", *it);
    }
    return result;
  }

  void FillWithGarbage() {
    memset(line_.get(), 0xf1, num_regs_ * sizeof(uint16_t));
    while (!monitors_.empty()) {
      monitors_.pop_back();
    }
    reg_to_lock_depths_.clear();
  }

  /*
   * We're creating a new instance of class C at address A. Any registers holding instances
   * previously created at address A must be initialized by now. If not, we mark them as "conflict"
   * to prevent them from being used (otherwise, MarkRefsAsInitialized would mark the old ones and
   * the new ones at the same time).
   */
  void MarkUninitRefsAsInvalid(const RegType& uninit_type);

  /*
   * Update all registers holding "uninit_type" to instead hold the corresponding initialized
   * reference type. This is called when an appropriate constructor is invoked -- all copies of
   * the reference must be marked as initialized.
   */
  void MarkRefsAsInitialized(const RegType& uninit_type);

  /*
   * Check constraints on constructor return. Specifically, make sure that the "this" argument got
   * initialized.
   * The "this" argument to <init> uses code offset kUninitThisArgAddr, which puts it at the start
   * of the list in slot 0. If we see a register with an uninitialized slot 0 reference, we know it
   * somehow didn't get initialized.
   */
  bool CheckConstructorReturn() const;

  // Compare two register lines. Returns 0 if they match.
  // Using this for a sort is unwise, since the value can change based on machine endianness.
  int CompareLine(const RegisterLine* line2) const {
    DCHECK(monitors_ == line2->monitors_);
    // TODO: DCHECK(reg_to_lock_depths_ == line2->reg_to_lock_depths_);
    return memcmp(line_.get(), line2->line_.get(), num_regs_ * sizeof(uint16_t));
  }

  size_t NumRegs() const {
    return num_regs_;
  }

  /*
   * Get the "this" pointer from a non-static method invocation. This returns the RegType so the
   * caller can decide whether it needs the reference to be initialized or not. (Can also return
   * kRegTypeZero if the reference can only be zero at this point.)
   *
   * The argument count is in vA, and the first argument is in vC, for both "simple" and "range"
   * versions. We just need to make sure vA is >= 1 and then return vC.
   */
  const RegType& GetInvocationThis(const Instruction::DecodedInstruction& dec_insn);

  /*
   * Verify types for a simple two-register instruction (e.g. "neg-int").
   * "dst_type" is stored into vA, and "src_type" is verified against vB.
   */
  void CheckUnaryOp(const Instruction::DecodedInstruction& dec_insn,
                    const RegType& dst_type, const RegType& src_type);

  /*
   * Verify types for a simple three-register instruction (e.g. "add-int").
   * "dst_type" is stored into vA, and "src_type1"/"src_type2" are verified
   * against vB/vC.
   */
  void CheckBinaryOp(const Instruction::DecodedInstruction& dec_insn,
                     const RegType& dst_type, const RegType& src_type1, const RegType& src_type2,
                     bool check_boolean_op);

  /*
   * Verify types for a binary "2addr" operation. "src_type1"/"src_type2"
   * are verified against vA/vB, then "dst_type" is stored into vA.
   */
  void CheckBinaryOp2addr(const Instruction::DecodedInstruction& dec_insn,
                          const RegType& dst_type,
                          const RegType& src_type1, const RegType& src_type2,
                          bool check_boolean_op);

  /*
   * Verify types for A two-register instruction with a literal constant (e.g. "add-int/lit8").
   * "dst_type" is stored into vA, and "src_type" is verified against vB.
   *
   * If "check_boolean_op" is set, we use the constant value in vC.
   */
  void CheckLiteralOp(const Instruction::DecodedInstruction& dec_insn,
                      const RegType& dst_type, const RegType& src_type, bool check_boolean_op);

  // Verify/push monitor onto the monitor stack, locking the value in reg_idx at location insn_idx.
  void PushMonitor(uint32_t reg_idx, int32_t insn_idx);

  // Verify/pop monitor from monitor stack ensuring that we believe the monitor is locked
  void PopMonitor(uint32_t reg_idx);

  // Stack of currently held monitors and where they were locked
  size_t MonitorStackDepth() const {
    return monitors_.size();
  }

  // We expect no monitors to be held at certain points, such a method returns. Verify the stack
  // is empty, failing and returning false if not.
  bool VerifyMonitorStackEmpty();

  bool MergeRegisters(const RegisterLine* incoming_line);

  size_t GetMaxNonZeroReferenceReg(size_t max_ref_reg) {
    size_t i = static_cast<int>(max_ref_reg) < 0 ? 0 : max_ref_reg;
    for (; i < num_regs_; i++) {
      if (GetRegisterType(i).IsNonZeroReferenceTypes()) {
        max_ref_reg = i;
      }
    }
    return max_ref_reg;
  }

  // Write a bit at each register location that holds a reference
  void WriteReferenceBitMap(std::vector<uint8_t>& data, size_t max_bytes);
 private:

  void CopyRegToLockDepth(size_t dst, size_t src) {
    if (reg_to_lock_depths_.count(src) > 0) {
      uint32_t depths = reg_to_lock_depths_[src];
      reg_to_lock_depths_[dst] = depths;
    }
  }

  bool IsSetLockDepth(size_t reg, size_t depth) {
    if (reg_to_lock_depths_.count(reg) > 0) {
      uint32_t depths = reg_to_lock_depths_[reg];
      return (depths & (1 << depth)) != 0;
    } else {
      return false;
    }
  }

  void SetRegToLockDepth(size_t reg, size_t depth) {
    CHECK_LT(depth, 32u);
    DCHECK(!IsSetLockDepth(reg, depth));
    uint32_t depths;
    if (reg_to_lock_depths_.count(reg) > 0) {
      depths = reg_to_lock_depths_[reg];
      depths = depths | (1 << depth);
    } else {
      depths = 1 << depth;
    }
    reg_to_lock_depths_[reg] = depths;
  }

  void ClearRegToLockDepth(size_t reg, size_t depth) {
    CHECK_LT(depth, 32u);
    DCHECK(IsSetLockDepth(reg, depth));
    uint32_t depths = reg_to_lock_depths_[reg];
    depths = depths ^ (1 << depth);
    if (depths != 0) {
      reg_to_lock_depths_[reg] = depths;
    } else {
      reg_to_lock_depths_.erase(reg);
    }
  }

  void ClearAllRegToLockDepths(size_t reg) {
    reg_to_lock_depths_.erase(reg);
  }

  // Storage for the result register's type, valid after an invocation
  uint16_t result_[2];

  // An array of RegType Ids associated with each dex register
  UniquePtr<uint16_t[]> line_;

  // Back link to the verifier
  DexVerifier* verifier_;

  // Length of reg_types_
  const size_t num_regs_;
  // A stack of monitor enter locations
  std::deque<uint32_t> monitors_;
  // A map from register to a bit vector of indices into the monitors_ stack. As we pop the monitor
  // stack we verify that monitor-enter/exit are correctly nested. That is, if there was a
  // monitor-enter on v5 and then on v6, we expect the monitor-exit to be on v6 then on v5
  std::map<uint32_t, uint32_t> reg_to_lock_depths_;
};
std::ostream& operator<<(std::ostream& os, const RegisterLine& rhs);

class PcToRegisterLineTable {
 public:
  // We don't need to store the register data for many instructions, because we either only need
  // it at branch points (for verification) or GC points and branches (for verification +
  // type-precise register analysis).
  enum RegisterTrackingMode {
    kTrackRegsBranches,
    kTrackRegsGcPoints,
    kTrackRegsAll,
  };
  PcToRegisterLineTable() {}
  ~PcToRegisterLineTable() {
    STLDeleteValues(&pc_to_register_line_);
  }

  // Initialize the RegisterTable. Every instruction address can have a different set of information
  // about what's in which register, but for verification purposes we only need to store it at
  // branch target addresses (because we merge into that).
  void Init(RegisterTrackingMode mode, InsnFlags* flags, uint32_t insns_size,
            uint16_t registers_size, DexVerifier* verifier);

  RegisterLine* GetLine(size_t idx) {
    return pc_to_register_line_[idx];
  }

 private:
  // Map from a dex pc to the register status associated with it
  std::map<int32_t, RegisterLine*> pc_to_register_line_;

  // Number of registers we track for each instruction. This is equal to the method's declared
  // "registersSize" plus kExtraRegs (2).
  size_t insn_reg_count_plus_;
};



// The verifier
class DexVerifier {
 public:
  /* Verify a class. Returns "true" on success. */
  static bool VerifyClass(const Class* klass, std::string& error);
  /*
   * Perform verification on a single method.
   *
   * We do this in three passes:
   *  (1) Walk through all code units, determining instruction locations,
   *      widths, and other characteristics.
   *  (2) Walk through all code units, performing static checks on
   *      operands.
   *  (3) Iterate through the method, checking type safety and looking
   *      for code flow problems.
   *
   * Some checks may be bypassed depending on the verification mode. We can't
   * turn this stuff off completely if we want to do "exact" GC.
   *
   * Confirmed here:
   * - code array must not be empty
   * Confirmed by ComputeWidthsAndCountOps():
   * - opcode of first instruction begins at index 0
   * - only documented instructions may appear
   * - each instruction follows the last
   * - last byte of last instruction is at (code_length-1)
   */
  static bool VerifyMethod(Method* method);
  static void VerifyMethodAndDump(Method* method);

  uint8_t EncodePcToReferenceMapData() const;

  uint32_t DexFileVersion() const {
    return dex_file_->GetVersion();
  }

  RegTypeCache* GetRegTypeCache() {
    return &reg_types_;
  }

  // Verification failed
  std::ostream& Fail(VerifyError error);

  // Log for verification information
  std::ostream& LogVerifyInfo() {
    return info_messages_ << "VFY: " << PrettyMethod(method_)
                          << '[' << reinterpret_cast<void*>(work_insn_idx_) << "] : ";
  }

  // Dump the state of the verifier, namely each instruction, what flags are set on it, register
  // information
  void Dump(std::ostream& os);

  static const std::vector<uint8_t>* GetGcMap(Compiler::MethodReference ref);
  static void DeleteGcMaps();

 private:

  explicit DexVerifier(Method* method);

  bool Verify();

  /*
   * Compute the width of the instruction at each address in the instruction stream, and store it in
   * insn_flags_. Addresses that are in the middle of an instruction, or that are part of switch
   * table data, are not touched (so the caller should probably initialize "insn_flags" to zero).
   *
   * The "new_instance_count_" and "monitor_enter_count_" fields in vdata are also set.
   *
   * Performs some static checks, notably:
   * - opcode of first instruction begins at index 0
   * - only documented instructions may appear
   * - each instruction follows the last
   * - last byte of last instruction is at (code_length-1)
   *
   * Logs an error and returns "false" on failure.
   */
  bool ComputeWidthsAndCountOps();

  /*
   * Set the "in try" flags for all instructions protected by "try" statements. Also sets the
   * "branch target" flags for exception handlers.
   *
   * Call this after widths have been set in "insn_flags".
   *
   * Returns "false" if something in the exception table looks fishy, but we're expecting the
   * exception table to be somewhat sane.
   */
  bool ScanTryCatchBlocks();

  /*
   * Perform static verification on all instructions in a method.
   *
   * Walks through instructions in a method calling VerifyInstruction on each.
   */
  bool VerifyInstructions();

  /*
   * Perform static verification on an instruction.
   *
   * As a side effect, this sets the "branch target" flags in InsnFlags.
   *
   * "(CF)" items are handled during code-flow analysis.
   *
   * v3 4.10.1
   * - target of each jump and branch instruction must be valid
   * - targets of switch statements must be valid
   * - operands referencing constant pool entries must be valid
   * - (CF) operands of getfield, putfield, getstatic, putstatic must be valid
   * - (CF) operands of method invocation instructions must be valid
   * - (CF) only invoke-direct can call a method starting with '<'
   * - (CF) <clinit> must never be called explicitly
   * - operands of instanceof, checkcast, new (and variants) must be valid
   * - new-array[-type] limited to 255 dimensions
   * - can't use "new" on an array class
   * - (?) limit dimensions in multi-array creation
   * - local variable load/store register values must be in valid range
   *
   * v3 4.11.1.2
   * - branches must be within the bounds of the code array
   * - targets of all control-flow instructions are the start of an instruction
   * - register accesses fall within range of allocated registers
   * - (N/A) access to constant pool must be of appropriate type
   * - code does not end in the middle of an instruction
   * - execution cannot fall off the end of the code
   * - (earlier) for each exception handler, the "try" area must begin and
   *   end at the start of an instruction (end can be at the end of the code)
   * - (earlier) for each exception handler, the handler must start at a valid
   *   instruction
   */
  bool VerifyInstruction(const Instruction* inst, uint32_t code_offset);

  /* Ensure that the register index is valid for this code item. */
  bool CheckRegisterIndex(uint32_t idx);

  /* Ensure that the wide register index is valid for this code item. */
  bool CheckWideRegisterIndex(uint32_t idx);

  // Perform static checks on a field get or set instruction. All we do here is ensure that the
  // field index is in the valid range.
  bool CheckFieldIndex(uint32_t idx);

  // Perform static checks on a method invocation instruction. All we do here is ensure that the
  // method index is in the valid range.
  bool CheckMethodIndex(uint32_t idx);

  // Perform static checks on a "new-instance" instruction. Specifically, make sure the class
  // reference isn't for an array class.
  bool CheckNewInstance(uint32_t idx);

  /* Ensure that the string index is in the valid range. */
  bool CheckStringIndex(uint32_t idx);

  // Perform static checks on an instruction that takes a class constant. Ensure that the class
  // index is in the valid range.
  bool CheckTypeIndex(uint32_t idx);

  // Perform static checks on a "new-array" instruction. Specifically, make sure they aren't
  // creating an array of arrays that causes the number of dimensions to exceed 255.
  bool CheckNewArray(uint32_t idx);

  // Verify an array data table. "cur_offset" is the offset of the fill-array-data instruction.
  bool CheckArrayData(uint32_t cur_offset);

  // Verify that the target of a branch instruction is valid. We don't expect code to jump directly
  // into an exception handler, but it's valid to do so as long as the target isn't a
  // "move-exception" instruction. We verify that in a later stage.
  // The dex format forbids certain instructions from branching to themselves.
  // Updates "insnFlags", setting the "branch target" flag.
  bool CheckBranchTarget(uint32_t cur_offset);

  // Verify a switch table. "cur_offset" is the offset of the switch instruction.
  // Updates "insnFlags", setting the "branch target" flag.
  bool CheckSwitchTargets(uint32_t cur_offset);

  // Check the register indices used in a "vararg" instruction, such as invoke-virtual or
  // filled-new-array.
  // - vA holds word count (0-5), args[] have values.
  // There are some tests we don't do here, e.g. we don't try to verify that invoking a method that
  // takes a double is done with consecutive registers. This requires parsing the target method
  // signature, which we will be doing later on during the code flow analysis.
  bool CheckVarArgRegs(uint32_t vA, uint32_t arg[]);

  // Check the register indices used in a "vararg/range" instruction, such as invoke-virtual/range
  // or filled-new-array/range.
  // - vA holds word count, vC holds index of first reg.
  bool CheckVarArgRangeRegs(uint32_t vA, uint32_t vC);

  // Extract the relative offset from a branch instruction.
  // Returns "false" on failure (e.g. this isn't a branch instruction).
  bool GetBranchOffset(uint32_t cur_offset, int32_t* pOffset, bool* pConditional,
                       bool* selfOkay);

  /* Perform detailed code-flow analysis on a single method. */
  bool VerifyCodeFlow();

  // Set the register types for the first instruction in the method based on the method signature.
  // This has the side-effect of validating the signature.
  bool SetTypesFromSignature();

  /*
   * Perform code flow on a method.
   *
   * The basic strategy is as outlined in v3 4.11.1.2: set the "changed" bit on the first
   * instruction, process it (setting additional "changed" bits), and repeat until there are no
   * more.
   *
   * v3 4.11.1.1
   * - (N/A) operand stack is always the same size
   * - operand stack [registers] contain the correct types of values
   * - local variables [registers] contain the correct types of values
   * - methods are invoked with the appropriate arguments
   * - fields are assigned using values of appropriate types
   * - opcodes have the correct type values in operand registers
   * - there is never an uninitialized class instance in a local variable in code protected by an
   *   exception handler (operand stack is okay, because the operand stack is discarded when an
   *   exception is thrown) [can't know what's a local var w/o the debug info -- should fall out of
   *   register typing]
   *
   * v3 4.11.1.2
   * - execution cannot fall off the end of the code
   *
   * (We also do many of the items described in the "static checks" sections, because it's easier to
   * do them here.)
   *
   * We need an array of RegType values, one per register, for every instruction. If the method uses
   * monitor-enter, we need extra data for every register, and a stack for every "interesting"
   * instruction. In theory this could become quite large -- up to several megabytes for a monster
   * function.
   *
   * NOTE:
   * The spec forbids backward branches when there's an uninitialized reference in a register. The
   * idea is to prevent something like this:
   *   loop:
   *     move r1, r0
   *     new-instance r0, MyClass
   *     ...
   *     if-eq rN, loop  // once
   *   initialize r0
   *
   * This leaves us with two different instances, both allocated by the same instruction, but only
   * one is initialized. The scheme outlined in v3 4.11.1.4 wouldn't catch this, so they work around
   * it by preventing backward branches. We achieve identical results without restricting code
   * reordering by specifying that you can't execute the new-instance instruction if a register
   * contains an uninitialized instance created by that same instruction.
   */
  bool CodeFlowVerifyMethod();

  /*
   * Perform verification for a single instruction.
   *
   * This requires fully decoding the instruction to determine the effect it has on registers.
   *
   * Finds zero or more following instructions and sets the "changed" flag if execution at that
   * point needs to be (re-)evaluated. Register changes are merged into "reg_types_" at the target
   * addresses. Does not set or clear any other flags in "insn_flags_".
   */
  bool CodeFlowVerifyInstruction(uint32_t* start_guess);

  // Perform verification of an aget instruction. The destination register's type will be set to
  // be that of component type of the array unless the array type is unknown, in which case a
  // bottom type inferred from the type of instruction is used. is_primitive is false for an
  // aget-object.
  void VerifyAGet(const Instruction::DecodedInstruction& insn, const RegType& insn_type,
                  bool is_primitive);

  // Perform verification of an aput instruction.
  void VerifyAPut(const Instruction::DecodedInstruction& insn, const RegType& insn_type,
                  bool is_primitive);

  // Lookup instance field and fail for resolution violations
  Field* GetInstanceField(const RegType& obj_type, int field_idx);

  // Lookup static field and fail for resolution violations
  Field* GetStaticField(int field_idx);

  // Perform verification of an iget or sget instruction.
  void VerifyISGet(const Instruction::DecodedInstruction& insn, const RegType& insn_type,
                   bool is_primitive, bool is_static);

  // Perform verification of an iput or sput instruction.
  void VerifyISPut(const Instruction::DecodedInstruction& insn, const RegType& insn_type,
                   bool is_primitive, bool is_static);

  // Verify that the arguments in a filled-new-array instruction are valid.
  void VerifyFilledNewArrayRegs(const Instruction::DecodedInstruction& dec_insn,
                                const RegType& res_type, bool is_range);

  // Resolves a class based on an index and performs access checks to ensure the referrer can
  // access the resolved class.
  const RegType& ResolveClassAndCheckAccess(uint32_t class_idx);

  /*
   * For the "move-exception" instruction at "work_insn_idx_", which must be at an exception handler
   * address, determine the Join of all exceptions that can land here. Fails if no matching
   * exception handler can be found or if the Join of exception types fails.
   */
  const RegType& GetCaughtExceptionType();

  /*
   * Resolves a method based on an index and performs access checks to ensure
   * the referrer can access the resolved method.
   * Does not throw exceptions.
   */
  Method* ResolveMethodAndCheckAccess(uint32_t method_idx,  bool is_direct);

  /*
   * Verify the arguments to a method. We're executing in "method", making
   * a call to the method reference in vB.
   *
   * If this is a "direct" invoke, we allow calls to <init>. For calls to
   * <init>, the first argument may be an uninitialized reference. Otherwise,
   * calls to anything starting with '<' will be rejected, as will any
   * uninitialized reference arguments.
   *
   * For non-static method calls, this will verify that the method call is
   * appropriate for the "this" argument.
   *
   * The method reference is in vBBBB. The "is_range" parameter determines
   * whether we use 0-4 "args" values or a range of registers defined by
   * vAA and vCCCC.
   *
   * Widening conversions on integers and references are allowed, but
   * narrowing conversions are not.
   *
   * Returns the resolved method on success, NULL on failure (with *failure
   * set appropriately).
   */
  Method* VerifyInvocationArgs(const Instruction::DecodedInstruction& dec_insn,
                               MethodType method_type, bool is_range, bool is_super);

  /*
   * Return the register type for the method. We can't just use the already-computed
   * DalvikJniReturnType, because if it's a reference type we need to do the class lookup.
   * Returned references are assumed to be initialized. Returns kRegTypeUnknown for "void".
   */
  const RegType& GetMethodReturnType();

  /*
   * Verify that the target instruction is not "move-exception". It's important that the only way
   * to execute a move-exception is as the first instruction of an exception handler.
   * Returns "true" if all is well, "false" if the target instruction is move-exception.
   */
  bool CheckMoveException(const uint16_t* insns, int insn_idx);

  /*
   * Replace an instruction with "throw-verification-error". This allows us to
   * defer error reporting until the code path is first used.
   */
  void ReplaceFailingInstruction();

  /*
  * Control can transfer to "next_insn". Merge the registers from merge_line into the table at
  * next_insn, and set the changed flag on the target address if any of the registers were changed.
  * Returns "false" if an error is encountered.
  */
  bool UpdateRegisters(uint32_t next_insn, const RegisterLine* merge_line);

  /*
   * Generate the GC map for a method that has just been verified (i.e. we're doing this as part of
   * verification). For type-precise determination we have all the data we need, so we just need to
   * encode it in some clever fashion.
   * Returns a pointer to a newly-allocated RegisterMap, or NULL on failure.
   */
  const std::vector<uint8_t>* GenerateGcMap();

  // Verify that the GC map associated with method_ is well formed
  void VerifyGcMap(const std::vector<uint8_t>& data);

  // Compute sizes for GC map data
  void ComputeGcMapSizes(size_t* gc_points, size_t* ref_bitmap_bits, size_t* log2_max_gc_pc);

  InsnFlags CurrentInsnFlags() {
    return insn_flags_[work_insn_idx_];
  }

  typedef std::map<const Compiler::MethodReference, const std::vector<uint8_t>*> GcMapTable;
  // All the GC maps that the verifier has created
  static Mutex gc_maps_lock_;
  static GcMapTable gc_maps_;
  static void SetGcMap(Compiler::MethodReference ref, const std::vector<uint8_t>& gc_map);

  RegTypeCache reg_types_;

  PcToRegisterLineTable reg_table_;

  // Storage for the register status we're currently working on.
  UniquePtr<RegisterLine> work_line_;

  // The address of the instruction we're currently working on, note that this is in 2 byte
  // quantities
  uint32_t work_insn_idx_;

  // Storage for the register status we're saving for later.
  UniquePtr<RegisterLine> saved_line_;

  Method* method_;  // The method we're working on.
  const DexFile* dex_file_;  // The dex file containing the method.
  const DexFile::CodeItem* code_item_;  // The code item containing the code for the method.
  UniquePtr<InsnFlags[]> insn_flags_;  // Instruction widths and flags, one entry per code unit.

  // The type of any error that occurs
  VerifyError failure_;

  // Failure message log
  std::ostringstream fail_messages_;
  // Info message log
  std::ostringstream info_messages_;

  // The number of occurrences of specific opcodes.
  size_t new_instance_count_;
  size_t monitor_enter_count_;
};

// Lightweight wrapper for PC to reference bit maps.
class PcToReferenceMap {
 public:
  PcToReferenceMap(const uint8_t* data, size_t data_length) {
    data_ = data;
    CHECK(data_ != NULL);
    // Check the size of the table agrees with the number of entries
    size_t data_size = data_length - 4;
    DCHECK_EQ(EntryWidth() * NumEntries(), data_size);
  }

  // The number of entries in the table
  size_t NumEntries() const {
    return GetData()[2] | (GetData()[3] << 8);
  }

  // Get the PC at the given index
  uint16_t GetPC(size_t index) const {
    size_t entry_offset = index * EntryWidth();
    if (PcWidth() == 1) {
      return Table()[entry_offset];
    } else {
      return Table()[entry_offset] | (Table()[entry_offset + 1] << 8);
    }
  }

  // Return address of bitmap encoding what are live references
  const uint8_t* GetBitMap(size_t index) const {
    size_t entry_offset = index * EntryWidth();
    return &Table()[entry_offset + PcWidth()];
  }

  // Find the bitmap associated with the given dex pc
  const uint8_t* FindBitMap(uint16_t dex_pc, bool error_if_not_present = true) const;

  // The number of bytes used to encode registers
  size_t RegWidth() const {
    return GetData()[1];
  }

 private:
  // Table of num_entries * (dex pc, bitmap)
  const uint8_t* Table() const {
    return GetData() + 4;
  }

  // The format of the table of the PCs for the table
  RegisterMapFormat Format() const {
    return static_cast<RegisterMapFormat>(GetData()[0]);
  }

  // Number of bytes used to encode a dex pc
  size_t PcWidth() const {
    RegisterMapFormat format = Format();
    switch (format) {
      case kRegMapFormatCompact8:
        return 1;
      case kRegMapFormatCompact16:
        return 2;
      default:
        LOG(FATAL) << "Invalid format " << static_cast<int>(format);
        return -1;
    }
  }

  // The width of an entry in the table
  size_t EntryWidth() const {
    return PcWidth() + RegWidth();
  }

  const uint8_t* GetData() const {
    return data_;
  }
  const uint8_t* data_;  // The header and table data
};

}  // namespace verifier
}  // namespace art

#endif  // ART_SRC_DEX_VERIFY_H_
