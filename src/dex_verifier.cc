// Copyright 2011 Google Inc. All Rights Reserved.

#include "dex_verifier.h"

#include <iostream>

#include "class_linker.h"
#include "dex_cache.h"
#include "dex_file.h"
#include "dex_instruction.h"
#include "dex_instruction_visitor.h"
#include "dex_verifier.h"
#include "intern_table.h"
#include "logging.h"
#include "runtime.h"
#include "stringpiece.h"

namespace art {
namespace verifier {

static const bool gDebugVerify = false;

std::ostream& operator<<(std::ostream& os, const VerifyError& rhs) {
  return os << (int)rhs;
}

static const char* type_strings[] = {
    "Unknown",
    "Conflict",
    "Boolean",
    "Byte",
    "Short",
    "Char",
    "Integer",
    "Float",
    "Long (Low Half)",
    "Long (High Half)",
    "Double (Low Half)",
    "Double (High Half)",
    "64-bit Constant (Low Half)",
    "64-bit Constant (High Half)",
    "32-bit Constant",
    "Unresolved Reference",
    "Uninitialized Reference",
    "Uninitialized This Reference",
    "Unresolved And Uninitialized This Reference",
    "Reference",
};

std::string RegType::Dump() const {
  DCHECK(type_ >=  kRegTypeUnknown && type_ <= kRegTypeReference);
  std::string result;
  if (IsConstant()) {
    uint32_t val = ConstantValue();
    if (val == 0) {
      result = "Zero";
    } else {
      if(IsConstantShort()) {
        result = StringPrintf("32-bit Constant: %d", val);
      } else {
        result = StringPrintf("32-bit Constant: 0x%x", val);
      }
    }
  } else {
    result = type_strings[type_];
    if (IsReferenceTypes()) {
      result += ": ";
      if (IsUnresolvedReference()) {
        result += PrettyDescriptor(GetDescriptor());
      } else {
        result += PrettyDescriptor(GetClass()->GetDescriptor());
      }
    }
  }
  return result;
}

const RegType& RegType::HighHalf(RegTypeCache* cache) const {
  CHECK(IsLowHalf());
  if (type_ == kRegTypeLongLo) {
    return cache->FromType(kRegTypeLongHi);
  } else if (type_ == kRegTypeDoubleLo) {
    return cache->FromType(kRegTypeDoubleHi);
  } else {
    return cache->FromType(kRegTypeConstHi);
  }
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
 * invoke-interface. In the tradition of Java verifiers we defer the verification of interface
 * types until an invoke-interface call on the interface typed reference at runtime and allow
 * the perversion of any Object being assignable to an interface type (note, however, that we don't
 * allow assignment of Object or Interface to any concrete subclass of Object and are therefore type
 * safe; further the Join on a Object cannot result in a sub-class by definition).
 */
Class* RegType::ClassJoin(Class* s, Class* t) {
  DCHECK(!s->IsPrimitive()) << PrettyClass(s);
  DCHECK(!t->IsPrimitive()) << PrettyClass(t);
  if (s == t) {
    return s;
  } else if (s->IsAssignableFrom(t)) {
    return s;
  } else if (t->IsAssignableFrom(s)) {
    return t;
  } else if (s->IsArrayClass() && t->IsArrayClass()) {
    Class* s_ct = s->GetComponentType();
    Class* t_ct = t->GetComponentType();
    if (s_ct->IsPrimitive() || t_ct->IsPrimitive()) {
      // Given the types aren't the same, if either array is of primitive types then the only
      // common parent is java.lang.Object
      Class* result = s->GetSuperClass();  // short-cut to java.lang.Object
      DCHECK(result->IsObjectClass());
      return result;
    }
    Class* common_elem = ClassJoin(s_ct, t_ct);
    ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
    const ClassLoader* class_loader = s->GetClassLoader();
    std::string descriptor = "[" + common_elem->GetDescriptor()->ToModifiedUtf8();
    Class* array_class = class_linker->FindClass(descriptor.c_str(), class_loader);
    DCHECK(array_class != NULL);
    return array_class;
  } else {
    size_t s_depth = s->Depth();
    size_t t_depth = t->Depth();
    // Get s and t to the same depth in the hierarchy
    if (s_depth > t_depth) {
      while (s_depth > t_depth) {
        s = s->GetSuperClass();
        s_depth--;
      }
    } else {
      while (t_depth > s_depth) {
        t = t->GetSuperClass();
        t_depth--;
      }
    }
    // Go up the hierarchy until we get to the common parent
    while (s != t) {
      s = s->GetSuperClass();
      t = t->GetSuperClass();
    }
    return s;
  }
}

bool RegType::IsAssignableFrom(const RegType& src) const {
  if (Equals(src)) {
    return true;
  } else {
    switch (GetType()) {
      case RegType::kRegTypeBoolean:  return src.IsBooleanTypes();
      case RegType::kRegTypeByte:     return src.IsByteTypes();
      case RegType::kRegTypeShort:    return src.IsShortTypes();
      case RegType::kRegTypeChar:     return src.IsCharTypes();
      case RegType::kRegTypeInteger:  return src.IsIntegralTypes();
      case RegType::kRegTypeFloat:    return src.IsFloatTypes();
      case RegType::kRegTypeLongLo:   return src.IsLongTypes();
      case RegType::kRegTypeDoubleLo: return src.IsDoubleTypes();
      default:
        if (!IsReferenceTypes()) {
          LOG(FATAL) << "Unexpected register type in IsAssignableFrom: '" << src << "'";
        }
        if (src.IsZero()) {
          return true;  // all reference types can be assigned null
        } else if (!src.IsReferenceTypes()) {
          return false;  // expect src to be a reference type
        } else if (IsJavaLangObject()) {
          return true;  // all reference types can be assigned to Object
        } else if (!IsUnresolvedTypes() && GetClass()->IsInterface()) {
          return true;  // We allow assignment to any interface, see comment in ClassJoin
        } else if (!IsUnresolvedTypes() && !src.IsUnresolvedTypes() &&
                   GetClass()->IsAssignableFrom(src.GetClass())) {
          // We're assignable from the Class point-of-view
          return true;
        } else {
          return false;
        }
    }
  }
}

static const RegType& SelectNonConstant(const RegType& a, const RegType& b) {
  return a.IsConstant() ? b : a;
}

const RegType& RegType::Merge(const RegType& incoming_type, RegTypeCache* reg_types) const {
  DCHECK(!Equals(incoming_type));  // Trivial equality handled by caller
  if (IsUnknown() && incoming_type.IsUnknown()) {
    return *this;  // Unknown MERGE Unknown => Unknown
  } else if (IsConflict()) {
    return *this;  // Conflict MERGE * => Conflict
  } else if (incoming_type.IsConflict()) {
    return incoming_type;  // * MERGE Conflict => Conflict
  } else if (IsUnknown() || incoming_type.IsUnknown()) {
    return reg_types->Conflict();  // Unknown MERGE * => Conflict
  } else if(IsConstant() && incoming_type.IsConstant()) {
    int32_t val1 = ConstantValue();
    int32_t val2 = incoming_type.ConstantValue();
    if (val1 >= 0 && val2 >= 0) {
      // +ve1 MERGE +ve2 => MAX(+ve1, +ve2)
      if (val1 >= val2) {
        return *this;
      } else {
        return incoming_type;
      }
    } else if (val1 < 0 && val2 < 0) {
      // -ve1 MERGE -ve2 => MIN(-ve1, -ve2)
      if (val1 <= val2) {
        return *this;
      } else {
        return incoming_type;
      }
    } else {
      // Values are +ve and -ve, choose smallest signed type in which they both fit
      if (IsConstantByte()) {
        if (incoming_type.IsConstantByte()) {
          return reg_types->ByteConstant();
        } else if (incoming_type.IsConstantShort()) {
          return reg_types->ShortConstant();
        } else {
          return reg_types->IntConstant();
        }
      } else if (IsConstantShort()) {
        if (incoming_type.IsConstantShort()) {
          return reg_types->ShortConstant();
        } else {
          return reg_types->IntConstant();
        }
      } else {
        return reg_types->IntConstant();
      }
    }
  } else if (IsIntegralTypes() && incoming_type.IsIntegralTypes()) {
    if (IsBooleanTypes() && incoming_type.IsBooleanTypes()) {
      return reg_types->Boolean();  // boolean MERGE boolean => boolean
    }
    if (IsByteTypes() && incoming_type.IsByteTypes()) {
      return reg_types->Byte();  // byte MERGE byte => byte
    }
    if (IsShortTypes() && incoming_type.IsShortTypes()) {
      return reg_types->Short();  // short MERGE short => short
    }
    if (IsCharTypes() && incoming_type.IsCharTypes()) {
      return reg_types->Char();  // char MERGE char => char
    }
    return reg_types->Integer();  // int MERGE * => int
  } else if ((IsFloatTypes() && incoming_type.IsFloatTypes()) ||
             (IsLongTypes() && incoming_type.IsLongTypes()) ||
             (IsLongHighTypes() && incoming_type.IsLongHighTypes()) ||
             (IsDoubleTypes() && incoming_type.IsDoubleTypes()) ||
             (IsDoubleHighTypes() && incoming_type.IsDoubleHighTypes())) {
    // check constant case was handled prior to entry
    DCHECK(!IsConstant() || !incoming_type.IsConstant());
    // float/long/double MERGE float/long/double_constant => float/long/double
    return SelectNonConstant(*this, incoming_type);
  } else if (IsReferenceTypes() && incoming_type.IsReferenceTypes()) {
    if (IsZero() || incoming_type.IsZero()) {
      return SelectNonConstant(*this, incoming_type);  // 0 MERGE ref => ref
    } else if (IsJavaLangObject() || incoming_type.IsJavaLangObject()) {
      return reg_types->JavaLangObject();  // Object MERGE ref => Object
    } else if (IsUninitializedTypes() || incoming_type.IsUninitializedTypes() ||
               IsUnresolvedTypes() || incoming_type.IsUnresolvedTypes()) {
      // Can only merge an unresolved or uninitialized type with itself, 0 or Object, we've already
      // checked these so => Conflict
      return reg_types->Conflict();
    } else {  // Two reference types, compute Join
      Class* c1 = GetClass();
      Class* c2 = incoming_type.GetClass();
      DCHECK(c1 != NULL && !c1->IsPrimitive());
      DCHECK(c2 != NULL && !c2->IsPrimitive());
      Class* join_class = ClassJoin(c1, c2);
      if (c1 == join_class) {
        return *this;
      } else if (c2 == join_class) {
        return incoming_type;
      } else {
        return reg_types->FromClass(join_class);
      }
    }
  } else {
    return reg_types->Conflict();  // Unexpected types => Conflict
  }
}

static RegType::Type RegTypeFromPrimitiveType(Primitive::Type prim_type) {
  switch (prim_type) {
    case Primitive::kPrimBoolean: return RegType::kRegTypeBoolean;
    case Primitive::kPrimByte:    return RegType::kRegTypeByte;
    case Primitive::kPrimShort:   return RegType::kRegTypeShort;
    case Primitive::kPrimChar:    return RegType::kRegTypeChar;
    case Primitive::kPrimInt:     return RegType::kRegTypeInteger;
    case Primitive::kPrimLong:    return RegType::kRegTypeLongLo;
    case Primitive::kPrimFloat:   return RegType::kRegTypeFloat;
    case Primitive::kPrimDouble:  return RegType::kRegTypeDoubleLo;
    case Primitive::kPrimVoid:
    default:                      return RegType::kRegTypeUnknown;
  }
}

static RegType::Type RegTypeFromDescriptor(const std::string& descriptor) {
  if (descriptor.length() == 1) {
    switch (descriptor[0]) {
      case 'Z': return RegType::kRegTypeBoolean;
      case 'B': return RegType::kRegTypeByte;
      case 'S': return RegType::kRegTypeShort;
      case 'C': return RegType::kRegTypeChar;
      case 'I': return RegType::kRegTypeInteger;
      case 'J': return RegType::kRegTypeLongLo;
      case 'F': return RegType::kRegTypeFloat;
      case 'D': return RegType::kRegTypeDoubleLo;
      case 'V':
      default:  return RegType::kRegTypeUnknown;
    }
  } else if(descriptor[0] == 'L' || descriptor[0] == '[') {
    return RegType::kRegTypeReference;
  } else {
    return RegType::kRegTypeUnknown;
  }
}

std::ostream& operator<<(std::ostream& os, const RegType& rhs) {
  os << rhs.Dump();
  return os;
}

const RegType& RegTypeCache::FromDescriptor(const ClassLoader* loader,
                                            const std::string& descriptor) {
  return From(RegTypeFromDescriptor(descriptor), loader, descriptor);
}

const RegType& RegTypeCache::From(RegType::Type type, const ClassLoader* loader,
                                  const std::string& descriptor) {
  if (type <= RegType::kRegTypeLastFixedLocation) {
    // entries should be sized greater than primitive types
    DCHECK_GT(entries_.size(), static_cast<size_t>(type));
    RegType* entry = entries_[type];
    if (entry == NULL) {
      Class* klass = NULL;
      if (descriptor.size() != 0) {
        klass = Runtime::Current()->GetClassLinker()->FindSystemClass(descriptor);
      }
      entry = new RegType(type, klass, 0, type);
      entries_[type] = entry;
    }
    return *entry;
  } else {
    DCHECK (type == RegType::kRegTypeReference);
    for (size_t i = RegType::kRegTypeLastFixedLocation + 1; i < entries_.size(); i++) {
      RegType* cur_entry = entries_[i];
      // check resolved and unresolved references, ignore uninitialized references
      if (cur_entry->IsReference() && cur_entry->GetClass()->GetDescriptor()->Equals(descriptor)) {
        return *cur_entry;
      } else if (cur_entry->IsUnresolvedReference() &&
                 cur_entry->GetDescriptor()->Equals(descriptor)) {
        return *cur_entry;
      }
    }
    Class* klass = Runtime::Current()->GetClassLinker()->FindClass(descriptor, loader);
    if (klass != NULL) {
      // Able to resolve so create resolved register type
      RegType* entry = new RegType(type, klass, 0, entries_.size());
      entries_.push_back(entry);
      return *entry;
    } else {
      // Unable to resolve so create unresolved register type
      DCHECK(Thread::Current()->IsExceptionPending());
      Thread::Current()->ClearException();
      String* string_descriptor =
          Runtime::Current()->GetInternTable()->InternStrong(descriptor.c_str());
      RegType* entry = new RegType(RegType::kRegTypeUnresolvedReference, string_descriptor, 0,
                                   entries_.size());
      entries_.push_back(entry);
      return *entry;
    }
  }
}

const RegType& RegTypeCache::FromClass(Class* klass) {
  if (klass->IsPrimitive()) {
    RegType::Type type = RegTypeFromPrimitiveType(klass->GetPrimitiveType());
    // entries should be sized greater than primitive types
    DCHECK_GT(entries_.size(), static_cast<size_t>(type));
    RegType* entry = entries_[type];
    if (entry == NULL) {
      entry = new RegType(type, klass, 0, type);
      entries_[type] = entry;
    }
    return *entry;
  } else {
    for (size_t i = RegType::kRegTypeLastFixedLocation + 1; i < entries_.size(); i++) {
      RegType* cur_entry = entries_[i];
      if (cur_entry->IsReference() && cur_entry->GetClass() == klass) {
        return *cur_entry;
      }
    }
    RegType* entry = new RegType(RegType::kRegTypeReference, klass, 0, entries_.size());
    entries_.push_back(entry);
    return *entry;
  }
}

const RegType& RegTypeCache::Uninitialized(Class* klass, uint32_t allocation_pc) {
  for (size_t i = RegType::kRegTypeLastFixedLocation + 1; i < entries_.size(); i++) {
    RegType* cur_entry = entries_[i];
    if (cur_entry->IsUninitializedReference() && cur_entry->GetAllocationPc() == allocation_pc &&
        cur_entry->GetClass() == klass) {
      return *cur_entry;
    }
  }
  RegType* entry = new RegType(RegType::kRegTypeUninitializedReference, klass, allocation_pc, entries_.size());
  entries_.push_back(entry);
  return *entry;
}

const RegType& RegTypeCache::UninitializedThisArgument(Class* klass) {
  for (size_t i = RegType::kRegTypeLastFixedLocation + 1; i < entries_.size(); i++) {
    RegType* cur_entry = entries_[i];
    if (cur_entry->IsUninitializedThisReference() && cur_entry->GetClass() == klass) {
      return *cur_entry;
    }
  }
  RegType* entry = new RegType(RegType::kRegTypeUninitializedThisReference, klass, 0,
                               entries_.size());
  entries_.push_back(entry);
  return *entry;
}

const RegType& RegTypeCache::FromType(RegType::Type type) {
  CHECK(type < RegType::kRegTypeReference);
  switch (type) {
    case RegType::kRegTypeBoolean:  return From(type, NULL, "Z");
    case RegType::kRegTypeByte:     return From(type, NULL, "B");
    case RegType::kRegTypeShort:    return From(type, NULL, "S");
    case RegType::kRegTypeChar:     return From(type, NULL, "C");
    case RegType::kRegTypeInteger:  return From(type, NULL, "I");
    case RegType::kRegTypeFloat:    return From(type, NULL, "F");
    case RegType::kRegTypeLongLo:
    case RegType::kRegTypeLongHi:   return From(type, NULL, "J");
    case RegType::kRegTypeDoubleLo:
    case RegType::kRegTypeDoubleHi: return From(type, NULL, "D");
    default:                        return From(type, NULL, "");
  }
}

const RegType& RegTypeCache::FromCat1Const(int32_t value) {
  for (size_t i = RegType::kRegTypeLastFixedLocation + 1; i < entries_.size(); i++) {
    RegType* cur_entry = entries_[i];
    if (cur_entry->IsConstant() && cur_entry->ConstantValue() == value) {
      return *cur_entry;
    }
  }
  RegType* entry = new RegType(RegType::kRegTypeConst, NULL, value, entries_.size());
  entries_.push_back(entry);
  return *entry;
}

bool RegisterLine::CheckConstructorReturn() const {
  for (size_t i = 0; i < num_regs_; i++) {
    if (GetRegisterType(i).IsUninitializedThisReference()) {
      verifier_->Fail(VERIFY_ERROR_GENERIC)
          << "Constructor returning without calling superclass constructor";
      return false;
    }
  }
  return true;
}

void RegisterLine::SetRegisterType(uint32_t vdst, const RegType& new_type) {
  DCHECK(vdst < num_regs_);
  if (new_type.IsLowHalf()) {
    line_[vdst] = new_type.GetId();
    line_[vdst + 1] = new_type.HighHalf(verifier_->GetRegTypeCache()).GetId();
  } else if (new_type.IsHighHalf()) {
    /* should never set these explicitly */
    verifier_->Fail(VERIFY_ERROR_GENERIC) << "Explicit set of high register type";
  } else if (new_type.IsConflict()) {  // should only be set during a merge
    verifier_->Fail(VERIFY_ERROR_GENERIC) << "Set register to unknown type " << new_type;
  } else {
    line_[vdst] = new_type.GetId();
  }
  // Clear the monitor entry bits for this register.
  ClearAllRegToLockDepths(vdst);
}

void RegisterLine::SetResultTypeToUnknown() {
  uint16_t unknown_id = verifier_->GetRegTypeCache()->Unknown().GetId();
  result_[0] = unknown_id;
  result_[1] = unknown_id;
}

void RegisterLine::SetResultRegisterType(const RegType& new_type) {
  result_[0] = new_type.GetId();
  if(new_type.IsLowHalf()) {
    DCHECK_EQ(new_type.HighHalf(verifier_->GetRegTypeCache()).GetId(), new_type.GetId() + 1);
    result_[1] = new_type.GetId() + 1;
  } else {
    result_[1] = verifier_->GetRegTypeCache()->Unknown().GetId();
  }
}

const RegType& RegisterLine::GetRegisterType(uint32_t vsrc) const {
  // The register index was validated during the static pass, so we don't need to check it here.
  DCHECK_LT(vsrc, num_regs_);
  return verifier_->GetRegTypeCache()->GetFromId(line_[vsrc]);
}

const RegType& RegisterLine::GetInvocationThis(const Instruction::DecodedInstruction& dec_insn) {
  if (dec_insn.vA_ < 1) {
    verifier_->Fail(VERIFY_ERROR_GENERIC) << "invoke lacks 'this'";
    return verifier_->GetRegTypeCache()->Unknown();
  }
  /* get the element type of the array held in vsrc */
  const RegType& this_type = GetRegisterType(dec_insn.vC_);
  if (!this_type.IsReferenceTypes()) {
    verifier_->Fail(VERIFY_ERROR_GENERIC) << "tried to get class from non-reference register v"
                                          << dec_insn.vC_ << " (type=" << this_type << ")";
    return verifier_->GetRegTypeCache()->Unknown();
  }
  return this_type;
}

Class* RegisterLine::GetClassFromRegister(uint32_t vsrc) const {
  /* get the element type of the array held in vsrc */
  const RegType& type = GetRegisterType(vsrc);
  /* if "always zero", we allow it to fail at runtime */
  if (type.IsZero()) {
    return NULL;
  } else if (!type.IsReferenceTypes()) {
    verifier_->Fail(VERIFY_ERROR_GENERIC) << "tried to get class from non-ref register v" << vsrc
                                         << " (type=" << type << ")";
    return NULL;
  } else if (type.IsUninitializedReference()) {
    verifier_->Fail(VERIFY_ERROR_GENERIC) << "register " << vsrc << " holds uninitialized reference";
    return NULL;
  } else {
    return type.GetClass();
  }
}

bool RegisterLine::VerifyRegisterType(uint32_t vsrc, const RegType& check_type) {
  // Verify the src register type against the check type refining the type of the register
  const RegType& src_type = GetRegisterType(vsrc);
  if (!check_type.IsAssignableFrom(src_type)) {
    verifier_->Fail(VERIFY_ERROR_GENERIC) << "register v" << vsrc << " has type " << src_type
                                          << " but expected " << check_type;
    return false;
  }
  // The register at vsrc has a defined type, we know the lower-upper-bound, but this is less
  // precise than the subtype in vsrc so leave it for reference types. For primitive types
  // if they are a defined type then they are as precise as we can get, however, for constant
  // types we may wish to refine them. Unfortunately constant propagation has rendered this useless.
  return true;
}

void RegisterLine::MarkRefsAsInitialized(const RegType& uninit_type) {
  Class* klass = uninit_type.GetClass();
  if (klass == NULL) {
    verifier_->Fail(VERIFY_ERROR_GENERIC) << "Unable to find type=" << uninit_type;
  } else {
    const RegType& init_type = verifier_->GetRegTypeCache()->FromClass(klass);
    size_t changed = 0;
    for (size_t i = 0; i < num_regs_; i++) {
      if (GetRegisterType(i).Equals(uninit_type)) {
        line_[i] = init_type.GetId();
        changed++;
      }
    }
    DCHECK_GT(changed, 0u);
  }
}

void RegisterLine::MarkUninitRefsAsInvalid(const RegType& uninit_type) {
  for (size_t i = 0; i < num_regs_; i++) {
    if (GetRegisterType(i).Equals(uninit_type)) {
      line_[i] = verifier_->GetRegTypeCache()->Conflict().GetId();
      ClearAllRegToLockDepths(i);
    }
  }
}

void RegisterLine::CopyRegister1(uint32_t vdst, uint32_t vsrc, TypeCategory cat) {
  DCHECK(cat == kTypeCategory1nr || cat == kTypeCategoryRef);
  const RegType& type = GetRegisterType(vsrc);
  SetRegisterType(vdst, type);
  if ((cat == kTypeCategory1nr && !type.IsCategory1Types()) ||
      (cat == kTypeCategoryRef && !type.IsReferenceTypes())) {
    verifier_->Fail(VERIFY_ERROR_GENERIC) << "copy1 v" << vdst << "<-v" << vsrc << " type=" << type
                                          << " cat=" << static_cast<int>(cat);
  } else if (cat == kTypeCategoryRef) {
    CopyRegToLockDepth(vdst, vsrc);
  }
}

void RegisterLine::CopyRegister2(uint32_t vdst, uint32_t vsrc) {
  const RegType& type_l = GetRegisterType(vsrc);
  const RegType& type_h = GetRegisterType(vsrc + 1);

  if (!type_l.CheckWidePair(type_h)) {
    verifier_->Fail(VERIFY_ERROR_GENERIC) << "copy2 v" << vdst << "<-v" << vsrc
                                         << " type=" << type_l << "/" << type_h;
  } else {
    SetRegisterType(vdst, type_l);  // implicitly sets the second half
  }
}

void RegisterLine::CopyResultRegister1(uint32_t vdst, bool is_reference) {
  const RegType& type = verifier_->GetRegTypeCache()->GetFromId(result_[0]);
  if ((!is_reference && !type.IsCategory1Types()) ||
      (is_reference && !type.IsReferenceTypes())) {
    verifier_->Fail(VERIFY_ERROR_GENERIC)
        << "copyRes1 v" << vdst << "<- result0"  << " type=" << type;
  } else {
    DCHECK(verifier_->GetRegTypeCache()->GetFromId(result_[1]).IsUnknown());
    SetRegisterType(vdst, type);
    result_[0] = verifier_->GetRegTypeCache()->Unknown().GetId();
  }
}

/*
 * Implement "move-result-wide". Copy the category-2 value from the result
 * register to another register, and reset the result register.
 */
void RegisterLine::CopyResultRegister2(uint32_t vdst) {
  const RegType& type_l = verifier_->GetRegTypeCache()->GetFromId(result_[0]);
  const RegType& type_h = verifier_->GetRegTypeCache()->GetFromId(result_[1]);
  if (!type_l.IsCategory2Types()) {
    verifier_->Fail(VERIFY_ERROR_GENERIC)
        << "copyRes2 v" << vdst << "<- result0"  << " type=" << type_l;
  } else {
    DCHECK(type_l.CheckWidePair(type_h));  // Set should never allow this case
    SetRegisterType(vdst, type_l);  // also sets the high
    result_[0] = verifier_->GetRegTypeCache()->Unknown().GetId();
    result_[1] = verifier_->GetRegTypeCache()->Unknown().GetId();
  }
}

void RegisterLine::CheckUnaryOp(const Instruction::DecodedInstruction& dec_insn,
                                const RegType& dst_type, const RegType& src_type) {
  if (VerifyRegisterType(dec_insn.vB_, src_type)) {
    SetRegisterType(dec_insn.vA_, dst_type);
  }
}

void RegisterLine::CheckBinaryOp(const Instruction::DecodedInstruction& dec_insn,
                                 const RegType& dst_type,
                                 const RegType& src_type1, const RegType& src_type2,
                                 bool check_boolean_op) {
  if (VerifyRegisterType(dec_insn.vB_, src_type1) &&
      VerifyRegisterType(dec_insn.vC_, src_type2)) {
    if (check_boolean_op) {
      DCHECK(dst_type.IsInteger());
      if (GetRegisterType(dec_insn.vB_).IsBooleanTypes() &&
          GetRegisterType(dec_insn.vC_).IsBooleanTypes()) {
        SetRegisterType(dec_insn.vA_, verifier_->GetRegTypeCache()->Boolean());
        return;
      }
    }
    SetRegisterType(dec_insn.vA_, dst_type);
  }
}

void RegisterLine::CheckBinaryOp2addr(const Instruction::DecodedInstruction& dec_insn,
                                      const RegType& dst_type, const RegType& src_type1,
                                      const RegType& src_type2, bool check_boolean_op) {
  if (VerifyRegisterType(dec_insn.vA_, src_type1) &&
      VerifyRegisterType(dec_insn.vB_, src_type2)) {
    if (check_boolean_op) {
      DCHECK(dst_type.IsInteger());
      if (GetRegisterType(dec_insn.vA_).IsBooleanTypes() &&
          GetRegisterType(dec_insn.vB_).IsBooleanTypes()) {
        SetRegisterType(dec_insn.vA_, verifier_->GetRegTypeCache()->Boolean());
        return;
      }
    }
    SetRegisterType(dec_insn.vA_, dst_type);
  }
}

void RegisterLine::CheckLiteralOp(const Instruction::DecodedInstruction& dec_insn,
                                  const RegType& dst_type, const RegType& src_type,
                                  bool check_boolean_op) {
  if (VerifyRegisterType(dec_insn.vB_, src_type)) {
    if (check_boolean_op) {
      DCHECK(dst_type.IsInteger());
      /* check vB with the call, then check the constant manually */
      if (GetRegisterType(dec_insn.vB_).IsBooleanTypes() &&
          (dec_insn.vC_ == 0 || dec_insn.vC_ == 1)) {
        SetRegisterType(dec_insn.vA_, verifier_->GetRegTypeCache()->Boolean());
        return;
      }
    }
    SetRegisterType(dec_insn.vA_, dst_type);
  }
}

void RegisterLine::PushMonitor(uint32_t reg_idx, int32_t insn_idx) {
  const RegType& reg_type = GetRegisterType(reg_idx);
  if (!reg_type.IsReferenceTypes()) {
    verifier_->Fail(VERIFY_ERROR_GENERIC) << "monitor-enter on non-object (" << reg_type << ")";
  } else {
    SetRegToLockDepth(reg_idx, monitors_.size());
    monitors_.push(insn_idx);
  }
}

void RegisterLine::PopMonitor(uint32_t reg_idx) {
  const RegType& reg_type = GetRegisterType(reg_idx);
  if (!reg_type.IsReferenceTypes()) {
    verifier_->Fail(VERIFY_ERROR_GENERIC) << "monitor-exit on non-object (" << reg_type << ")";
  } else if (monitors_.empty()) {
    verifier_->Fail(VERIFY_ERROR_GENERIC) << "monitor-exit stack underflow";
  } else {
    monitors_.pop();
    if(!IsSetLockDepth(reg_idx, monitors_.size())) {
      // Bug 3215458: Locks and unlocks are on objects, if that object is a literal then before
      // format "036" the constant collector may create unlocks on the same object but referenced
      // via different registers.
      ((verifier_->DexFileVersion() >= 36) ? verifier_->Fail(VERIFY_ERROR_GENERIC)
                                           : verifier_->LogVerifyInfo())
            << "monitor-exit not unlocking the top of the monitor stack";
    } else {
      // Record the register was unlocked
      ClearRegToLockDepth(reg_idx, monitors_.size());
    }
  }
}

bool RegisterLine::VerifyMonitorStackEmpty() {
  if (MonitorStackDepth() != 0) {
    verifier_->Fail(VERIFY_ERROR_GENERIC) << "expected empty monitor stack";
    return false;
  } else {
    return true;
  }
}

bool RegisterLine::MergeRegisters(const RegisterLine* incoming_line) {
  bool changed = false;
  for (size_t idx = 0; idx < num_regs_; idx++) {
    if (line_[idx] != incoming_line->line_[idx]) {
      const RegType& incoming_reg_type = incoming_line->GetRegisterType(idx);
      const RegType& cur_type = GetRegisterType(idx);
      const RegType& new_type = cur_type.Merge(incoming_reg_type, verifier_->GetRegTypeCache());
      changed = changed || !cur_type.Equals(new_type);
      line_[idx] = new_type.GetId();
    }
  }
  if(monitors_ != incoming_line->monitors_) {
    verifier_->Fail(VERIFY_ERROR_GENERIC) << "mismatched stack depths (depth="
        << MonitorStackDepth() << ", incoming depth=" << incoming_line->MonitorStackDepth() << ")";
  } else if (reg_to_lock_depths_ != incoming_line->reg_to_lock_depths_) {
    for (uint32_t idx = 0; idx < num_regs_; idx++) {
      size_t depths = reg_to_lock_depths_.count(idx);
      size_t incoming_depths = incoming_line->reg_to_lock_depths_.count(idx);
      if (depths != incoming_depths) {
        if (depths == 0 || incoming_depths == 0) {
          reg_to_lock_depths_.erase(idx);
        } else {
          verifier_->Fail(VERIFY_ERROR_GENERIC) << "mismatched stack depths for register v" << idx
                                                << ": " << depths  << " != " << incoming_depths;
          break;
        }
      }
    }
  }
  return changed;
}

void RegisterLine::WriteReferenceBitMap(int8_t* data, size_t max_bytes) {
  for (size_t i = 0; i < num_regs_; i += 8) {
    uint8_t val = 0;
    for (size_t j = 0; j < 8 && (i + j) < num_regs_; j++) {
      // Note: we write 1 for a Reference but not for Null
      if (GetRegisterType(i + j).IsNonZeroReferenceTypes()) {
        val |= 1 << j;
      }
    }
    if (val != 0) {
      DCHECK_LT(i / 8, max_bytes);
      data[i / 8] = val;
    }
  }
}

std::ostream& operator<<(std::ostream& os, const RegisterLine& rhs) {
  os << rhs.Dump();
  return os;
}


void PcToRegisterLineTable::Init(RegisterTrackingMode mode, InsnFlags* flags,
                                 uint32_t insns_size, uint16_t registers_size,
                                 DexVerifier* verifier) {
  DCHECK_GT(insns_size, 0U);

  for (uint32_t i = 0; i < insns_size; i++) {
    bool interesting = false;
    switch (mode) {
      case kTrackRegsAll:
        interesting = flags[i].IsOpcode();
        break;
      case kTrackRegsGcPoints:
        interesting = flags[i].IsGcPoint() || flags[i].IsBranchTarget();
        break;
      case kTrackRegsBranches:
        interesting = flags[i].IsBranchTarget();
        break;
      default:
        break;
    }
    if (interesting) {
      pc_to_register_line_[i] = new RegisterLine(registers_size, verifier);
    }
  }
}

bool DexVerifier::VerifyClass(const Class* klass) {
  if (klass->IsVerified()) {
    return true;
  }
  Class* super = klass->GetSuperClass();
  if (super == NULL && !klass->GetDescriptor()->Equals("Ljava/lang/Object;")) {
    LOG(ERROR) << "Verifier rejected class " << PrettyClass(klass) << " that has no super class";
    return false;
  }
  if (super != NULL) {
    if (!super->IsVerified() && !super->IsErroneous()) {
      Runtime::Current()->GetClassLinker()->VerifyClass(super);
    }
    if (!super->IsVerified()) {
      LOG(ERROR) << "Verifier rejected class " << PrettyClass(klass)
                 << " that attempts to sub-class corrupt class " << PrettyClass(super);
      return false;
    } else if (super->IsFinal()) {
      LOG(ERROR) << "Verifier rejected class " << PrettyClass(klass)
                 << " that attempts to sub-class final class " << PrettyClass(super);
      return false;
    }
  }
  for (size_t i = 0; i < klass->NumDirectMethods(); ++i) {
    Method* method = klass->GetDirectMethod(i);
    if (!VerifyMethod(method)) {
      LOG(ERROR) << "Verifier rejected class " << PrettyClass(klass) << " due to bad method "
                 << PrettyMethod(method, true);
      return false;
    }
  }
  for (size_t i = 0; i < klass->NumVirtualMethods(); ++i) {
    Method* method = klass->GetVirtualMethod(i);
    if (!VerifyMethod(method)) {
      LOG(ERROR) << "Verifier rejected class " << PrettyClass(klass) << " due to bad method "
                 << PrettyMethod(method, true);
      return false;
    }
  }
  return true;
}

bool DexVerifier::VerifyMethod(Method* method) {
  DexVerifier verifier(method);
  bool success = verifier.Verify();
  // We expect either success and no verification error, or failure and a generic failure to
  // reject the class.
  if (success) {
    if (verifier.failure_ != VERIFY_ERROR_NONE) {
      LOG(FATAL) << "Unhandled failure in verification of " << PrettyMethod(method) << std::endl
                 << verifier.fail_messages_;
    }
  } else {
    LOG(INFO) << "Verification error in " << PrettyMethod(method) << " "
               << verifier.fail_messages_.str();
    if (gDebugVerify) {
      std::cout << std::endl << verifier.info_messages_.str();
      verifier.Dump(std::cout);
    }
    DCHECK_EQ(verifier.failure_, VERIFY_ERROR_GENERIC);
  }
  return success;
}

void DexVerifier::VerifyMethodAndDump(Method* method) {
  DexVerifier verifier(method);
  verifier.Verify();

  LogMessage log(__FILE__, __LINE__, INFO, -1);
  log.stream() << "Dump of method " << PrettyMethod(method) << " "
               << verifier.fail_messages_.str();
  log.stream() << std::endl << verifier.info_messages_.str();

  verifier.Dump(log.stream());
}

DexVerifier::DexVerifier(Method* method) : java_lang_throwable_(NULL), work_insn_idx_(-1),
                                           method_(method), failure_(VERIFY_ERROR_NONE),
                                           new_instance_count_(0), monitor_enter_count_(0) {
  const DexCache* dex_cache = method->GetDeclaringClass()->GetDexCache();
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  dex_file_ = &class_linker->FindDexFile(dex_cache);
  code_item_ = dex_file_->GetCodeItem(method->GetCodeItemOffset());
}

bool DexVerifier::Verify() {
  // If there aren't any instructions, make sure that's expected, then exit successfully.
  if (code_item_ == NULL) {
    if (!method_->IsNative() && !method_->IsAbstract()) {
      Fail(VERIFY_ERROR_GENERIC) << "zero-length code in concrete non-native method";
      return false;
    } else {
      return true;
    }
  }
  // Sanity-check the register counts. ins + locals = registers, so make sure that ins <= registers.
  if (code_item_->ins_size_ > code_item_->registers_size_) {
    Fail(VERIFY_ERROR_GENERIC) << "bad register counts (ins=" << code_item_->ins_size_
                               << " regs=" << code_item_->registers_size_;
    return false;
  }
  // Allocate and initialize an array to hold instruction data.
  insn_flags_.reset(new InsnFlags[code_item_->insns_size_in_code_units_]());
  // Run through the instructions and see if the width checks out.
  bool result = ComputeWidthsAndCountOps();
  // Flag instructions guarded by a "try" block and check exception handlers.
  result = result && ScanTryCatchBlocks();
  // Perform static instruction verification.
  result = result && VerifyInstructions();
  // Perform code flow analysis.
  result = result && VerifyCodeFlow();
  return result;
}

bool DexVerifier::ComputeWidthsAndCountOps() {
  const uint16_t* insns = code_item_->insns_;
  size_t insns_size = code_item_->insns_size_in_code_units_;
  const Instruction* inst = Instruction::At(insns);
  size_t new_instance_count = 0;
  size_t monitor_enter_count = 0;
  size_t dex_pc = 0;

  while (dex_pc < insns_size) {
    Instruction::Code opcode = inst->Opcode();
    if (opcode == Instruction::NEW_INSTANCE) {
      new_instance_count++;
    } else if (opcode == Instruction::MONITOR_ENTER) {
      monitor_enter_count++;
    }
    size_t inst_size = inst->SizeInCodeUnits();
    insn_flags_[dex_pc].SetLengthInCodeUnits(inst_size);
    dex_pc += inst_size;
    inst = inst->Next();
  }

  if (dex_pc != insns_size) {
    Fail(VERIFY_ERROR_GENERIC) << "code did not end where expected ("
        << dex_pc << " vs. " << insns_size << ")";
    return false;
  }

  new_instance_count_ = new_instance_count;
  monitor_enter_count_ = monitor_enter_count;
  return true;
}

bool DexVerifier::ScanTryCatchBlocks() {
  uint32_t tries_size = code_item_->tries_size_;
  if (tries_size == 0) {
    return true;
  }
  uint32_t insns_size = code_item_->insns_size_in_code_units_;
  const DexFile::TryItem* tries = DexFile::dexGetTryItems(*code_item_, 0);

  for (uint32_t idx = 0; idx < tries_size; idx++) {
    const DexFile::TryItem* try_item = &tries[idx];
    uint32_t start = try_item->start_addr_;
    uint32_t end = start + try_item->insn_count_;
    if ((start >= end) || (start >= insns_size) || (end > insns_size)) {
      Fail(VERIFY_ERROR_GENERIC) << "bad exception entry: startAddr=" << start
                                 << " endAddr=" << end << " (size=" << insns_size << ")";
      return false;
    }
    if (!insn_flags_[start].IsOpcode()) {
      Fail(VERIFY_ERROR_GENERIC) << "'try' block starts inside an instruction (" << start << ")";
      return false;
    }
    for (uint32_t dex_pc = start; dex_pc < end;
        dex_pc += insn_flags_[dex_pc].GetLengthInCodeUnits()) {
      insn_flags_[dex_pc].SetInTry();
    }
  }
  /* Iterate over each of the handlers to verify target addresses. */
  const byte* handlers_ptr = DexFile::dexGetCatchHandlerData(*code_item_, 0);
  uint32_t handlers_size = DecodeUnsignedLeb128(&handlers_ptr);
  for (uint32_t idx = 0; idx < handlers_size; idx++) {
    DexFile::CatchHandlerIterator iterator(handlers_ptr);
    for (; !iterator.HasNext(); iterator.Next()) {
      uint32_t dex_pc= iterator.Get().address_;
      if (!insn_flags_[dex_pc].IsOpcode()) {
        Fail(VERIFY_ERROR_GENERIC) << "exception handler starts at bad address (" << dex_pc << ")";
        return false;
      }
      insn_flags_[dex_pc].SetBranchTarget();
    }
    handlers_ptr = iterator.GetData();
  }
  return true;
}

bool DexVerifier::VerifyInstructions() {
  const Instruction* inst = Instruction::At(code_item_->insns_);

  /* Flag the start of the method as a branch target. */
  insn_flags_[0].SetBranchTarget();

  uint32_t insns_size = code_item_->insns_size_in_code_units_;
  for(uint32_t dex_pc = 0; dex_pc < insns_size;) {
    if (!VerifyInstruction(inst, dex_pc)) {
      DCHECK_NE(failure_, VERIFY_ERROR_NONE);
      fail_messages_ << "Rejecting opcode " << inst->DumpString(dex_file_) << " at " << dex_pc;
      return false;
    }
    /* Flag instructions that are garbage collection points */
    if (inst->IsBranch() || inst->IsSwitch() || inst->IsThrow() || inst->IsReturn()) {
      insn_flags_[dex_pc].SetGcPoint();
    }
    dex_pc += inst->SizeInCodeUnits();
    inst = inst->Next();
  }
  return true;
}

bool DexVerifier::VerifyInstruction(const Instruction* inst, uint32_t code_offset) {
  Instruction::DecodedInstruction dec_insn(inst);
  bool result = true;
  switch (inst->GetVerifyTypeArgumentA()) {
    case Instruction::kVerifyRegA:
      result = result && CheckRegisterIndex(dec_insn.vA_);
      break;
    case Instruction::kVerifyRegAWide:
      result = result && CheckWideRegisterIndex(dec_insn.vA_);
      break;
  }
  switch (inst->GetVerifyTypeArgumentB()) {
    case Instruction::kVerifyRegB:
      result = result && CheckRegisterIndex(dec_insn.vB_);
      break;
    case Instruction::kVerifyRegBField:
      result = result && CheckFieldIndex(dec_insn.vB_);
      break;
    case Instruction::kVerifyRegBMethod:
      result = result && CheckMethodIndex(dec_insn.vB_);
      break;
    case Instruction::kVerifyRegBNewInstance:
      result = result && CheckNewInstance(dec_insn.vB_);
      break;
    case Instruction::kVerifyRegBString:
      result = result && CheckStringIndex(dec_insn.vB_);
      break;
    case Instruction::kVerifyRegBType:
      result = result && CheckTypeIndex(dec_insn.vB_);
      break;
    case Instruction::kVerifyRegBWide:
      result = result && CheckWideRegisterIndex(dec_insn.vB_);
      break;
  }
  switch (inst->GetVerifyTypeArgumentC()) {
    case Instruction::kVerifyRegC:
      result = result && CheckRegisterIndex(dec_insn.vC_);
      break;
    case Instruction::kVerifyRegCField:
      result = result && CheckFieldIndex(dec_insn.vC_);
      break;
    case Instruction::kVerifyRegCNewArray:
      result = result && CheckNewArray(dec_insn.vC_);
      break;
    case Instruction::kVerifyRegCType:
      result = result && CheckTypeIndex(dec_insn.vC_);
      break;
    case Instruction::kVerifyRegCWide:
      result = result && CheckWideRegisterIndex(dec_insn.vC_);
      break;
  }
  switch (inst->GetVerifyExtraFlags()) {
    case Instruction::kVerifyArrayData:
      result = result && CheckArrayData(code_offset);
      break;
    case Instruction::kVerifyBranchTarget:
      result = result && CheckBranchTarget(code_offset);
      break;
    case Instruction::kVerifySwitchTargets:
      result = result && CheckSwitchTargets(code_offset);
      break;
    case Instruction::kVerifyVarArg:
      result = result && CheckVarArgRegs(dec_insn.vA_, dec_insn.arg_);
      break;
    case Instruction::kVerifyVarArgRange:
      result = result && CheckVarArgRangeRegs(dec_insn.vA_, dec_insn.vC_);
      break;
    case Instruction::kVerifyError:
      Fail(VERIFY_ERROR_GENERIC) << "unexpected opcode " << inst->Name();
      result = false;
      break;
  }
  return result;
}

bool DexVerifier::CheckRegisterIndex(uint32_t idx) {
  if (idx >= code_item_->registers_size_) {
    Fail(VERIFY_ERROR_GENERIC) << "register index out of range (" << idx << " >= "
                               << code_item_->registers_size_ << ")";
    return false;
  }
  return true;
}

bool DexVerifier::CheckWideRegisterIndex(uint32_t idx) {
  if (idx + 1 >= code_item_->registers_size_) {
    Fail(VERIFY_ERROR_GENERIC) << "wide register index out of range (" << idx
                               << "+1 >= " << code_item_->registers_size_ << ")";
    return false;
  }
  return true;
}

bool DexVerifier::CheckFieldIndex(uint32_t idx) {
  if (idx >= dex_file_->GetHeader().field_ids_size_) {
    Fail(VERIFY_ERROR_GENERIC) << "bad field index " << idx << " (max "
                               << dex_file_->GetHeader().field_ids_size_ << ")";
    return false;
  }
  return true;
}

bool DexVerifier::CheckMethodIndex(uint32_t idx) {
  if (idx >= dex_file_->GetHeader().method_ids_size_) {
    Fail(VERIFY_ERROR_GENERIC) << "bad method index " << idx << " (max "
                               << dex_file_->GetHeader().method_ids_size_ << ")";
    return false;
  }
  return true;
}

bool DexVerifier::CheckNewInstance(uint32_t idx) {
  if (idx >= dex_file_->GetHeader().type_ids_size_) {
    Fail(VERIFY_ERROR_GENERIC) << "bad type index " << idx << " (max "
                               << dex_file_->GetHeader().type_ids_size_ << ")";
    return false;
  }
  // We don't need the actual class, just a pointer to the class name.
  const char* descriptor = dex_file_->dexStringByTypeIdx(idx);
  if (descriptor[0] != 'L') {
    Fail(VERIFY_ERROR_GENERIC) << "can't call new-instance on type '" << descriptor << "'";
    return false;
  }
  return true;
}

bool DexVerifier::CheckStringIndex(uint32_t idx) {
  if (idx >= dex_file_->GetHeader().string_ids_size_) {
    Fail(VERIFY_ERROR_GENERIC) << "bad string index " << idx << " (max "
                               << dex_file_->GetHeader().string_ids_size_ << ")";
    return false;
  }
  return true;
}

bool DexVerifier::CheckTypeIndex(uint32_t idx) {
  if (idx >= dex_file_->GetHeader().type_ids_size_) {
    Fail(VERIFY_ERROR_GENERIC) << "bad type index " << idx << " (max "
                               << dex_file_->GetHeader().type_ids_size_ << ")";
    return false;
  }
  return true;
}

bool DexVerifier::CheckNewArray(uint32_t idx) {
  if (idx >= dex_file_->GetHeader().type_ids_size_) {
    Fail(VERIFY_ERROR_GENERIC) << "bad type index " << idx << " (max "
                               << dex_file_->GetHeader().type_ids_size_ << ")";
    return false;
  }
  int bracket_count = 0;
  const char* descriptor = dex_file_->dexStringByTypeIdx(idx);
  const char* cp = descriptor;
  while (*cp++ == '[') {
    bracket_count++;
  }
  if (bracket_count == 0) {
    /* The given class must be an array type. */
    Fail(VERIFY_ERROR_GENERIC) << "can't new-array class '" << descriptor << "' (not an array)";
    return false;
  } else if (bracket_count > 255) {
    /* It is illegal to create an array of more than 255 dimensions. */
    Fail(VERIFY_ERROR_GENERIC) << "can't new-array class '" << descriptor << "' (exceeds limit)";
    return false;
  }
  return true;
}

bool DexVerifier::CheckArrayData(uint32_t cur_offset) {
  const uint32_t insn_count = code_item_->insns_size_in_code_units_;
  const uint16_t* insns = code_item_->insns_ + cur_offset;
  const uint16_t* array_data;
  int32_t array_data_offset;

  DCHECK_LT(cur_offset, insn_count);
  /* make sure the start of the array data table is in range */
  array_data_offset = insns[1] | (((int32_t) insns[2]) << 16);
  if ((int32_t) cur_offset + array_data_offset < 0 ||
      cur_offset + array_data_offset + 2 >= insn_count) {
    Fail(VERIFY_ERROR_GENERIC) << "invalid array data start: at " << cur_offset
                               << ", data offset " << array_data_offset << ", count " << insn_count;
    return false;
  }
  /* offset to array data table is a relative branch-style offset */
  array_data = insns + array_data_offset;
  /* make sure the table is 32-bit aligned */
  if ((((uint32_t) array_data) & 0x03) != 0) {
    Fail(VERIFY_ERROR_GENERIC) << "unaligned array data table: at " << cur_offset
                               << ", data offset " << array_data_offset;
    return false;
  }
  uint32_t value_width = array_data[1];
  uint32_t value_count = *(uint32_t*) (&array_data[2]);
  uint32_t table_size = 4 + (value_width * value_count + 1) / 2;
  /* make sure the end of the switch is in range */
  if (cur_offset + array_data_offset + table_size > insn_count) {
    Fail(VERIFY_ERROR_GENERIC) << "invalid array data end: at " << cur_offset
                               << ", data offset " << array_data_offset << ", end "
                               << cur_offset + array_data_offset + table_size
                               << ", count " << insn_count;
    return false;
  }
  return true;
}

bool DexVerifier::CheckBranchTarget(uint32_t cur_offset) {
  int32_t offset;
  bool isConditional, selfOkay;
  if (!GetBranchOffset(cur_offset, &offset, &isConditional, &selfOkay)) {
    return false;
  }
  if (!selfOkay && offset == 0) {
    Fail(VERIFY_ERROR_GENERIC) << "branch offset of zero not allowed at" << (void*) cur_offset;
    return false;
  }
  // Check for 32-bit overflow. This isn't strictly necessary if we can depend on the VM to have
  // identical "wrap-around" behavior, but it's unwise to depend on that.
  if (((int64_t) cur_offset + (int64_t) offset) != (int64_t) (cur_offset + offset)) {
    Fail(VERIFY_ERROR_GENERIC) << "branch target overflow " << (void*) cur_offset << " +" << offset;
    return false;
  }
  const uint32_t insn_count = code_item_->insns_size_in_code_units_;
  int32_t abs_offset = cur_offset + offset;
  if (abs_offset < 0 || (uint32_t) abs_offset >= insn_count || !insn_flags_[abs_offset].IsOpcode()) {
    Fail(VERIFY_ERROR_GENERIC) << "invalid branch target " << offset << " (-> "
                               << (void*) abs_offset << ") at " << (void*) cur_offset;
    return false;
  }
  insn_flags_[abs_offset].SetBranchTarget();
  return true;
}

bool DexVerifier::GetBranchOffset(uint32_t cur_offset, int32_t* pOffset, bool* pConditional,
                                  bool* selfOkay) {
  const uint16_t* insns = code_item_->insns_ + cur_offset;
  *pConditional = false;
  *selfOkay = false;
  switch (*insns & 0xff) {
    case Instruction::GOTO:
      *pOffset = ((int16_t) *insns) >> 8;
      break;
    case Instruction::GOTO_32:
      *pOffset = insns[1] | (((uint32_t) insns[2]) << 16);
      *selfOkay = true;
      break;
    case Instruction::GOTO_16:
      *pOffset = (int16_t) insns[1];
      break;
    case Instruction::IF_EQ:
    case Instruction::IF_NE:
    case Instruction::IF_LT:
    case Instruction::IF_GE:
    case Instruction::IF_GT:
    case Instruction::IF_LE:
    case Instruction::IF_EQZ:
    case Instruction::IF_NEZ:
    case Instruction::IF_LTZ:
    case Instruction::IF_GEZ:
    case Instruction::IF_GTZ:
    case Instruction::IF_LEZ:
      *pOffset = (int16_t) insns[1];
      *pConditional = true;
      break;
    default:
      return false;
      break;
  }
  return true;
}

bool DexVerifier::CheckSwitchTargets(uint32_t cur_offset) {
  const uint32_t insn_count = code_item_->insns_size_in_code_units_;
  DCHECK_LT(cur_offset, insn_count);
  const uint16_t* insns = code_item_->insns_ + cur_offset;
  /* make sure the start of the switch is in range */
  int32_t switch_offset = insns[1] | ((int32_t) insns[2]) << 16;
  if ((int32_t) cur_offset + switch_offset < 0 || cur_offset + switch_offset + 2 >= insn_count) {
    Fail(VERIFY_ERROR_GENERIC) << "invalid switch start: at " << cur_offset
                               << ", switch offset " << switch_offset << ", count " << insn_count;
    return false;
  }
  /* offset to switch table is a relative branch-style offset */
  const uint16_t* switch_insns = insns + switch_offset;
  /* make sure the table is 32-bit aligned */
  if ((((uint32_t) switch_insns) & 0x03) != 0) {
    Fail(VERIFY_ERROR_GENERIC) << "unaligned switch table: at " << cur_offset
                               << ", switch offset " << switch_offset;
    return false;
  }
  uint32_t switch_count = switch_insns[1];
  int32_t keys_offset, targets_offset;
  uint16_t expected_signature;
  if ((*insns & 0xff) == Instruction::PACKED_SWITCH) {
    /* 0=sig, 1=count, 2/3=firstKey */
    targets_offset = 4;
    keys_offset = -1;
    expected_signature = Instruction::kPackedSwitchSignature;
  } else {
    /* 0=sig, 1=count, 2..count*2 = keys */
    keys_offset = 2;
    targets_offset = 2 + 2 * switch_count;
    expected_signature = Instruction::kSparseSwitchSignature;
  }
  uint32_t table_size = targets_offset + switch_count * 2;
  if (switch_insns[0] != expected_signature) {
    Fail(VERIFY_ERROR_GENERIC) << "wrong signature for switch table (" << (void*) switch_insns[0]
                               << ", wanted " << (void*) expected_signature << ")";
    return false;
  }
  /* make sure the end of the switch is in range */
  if (cur_offset + switch_offset + table_size > (uint32_t) insn_count) {
    Fail(VERIFY_ERROR_GENERIC) << "invalid switch end: at " << cur_offset << ", switch offset "
                               << switch_offset << ", end "
                               << (cur_offset + switch_offset + table_size)
                               << ", count " << insn_count;
    return false;
  }
  /* for a sparse switch, verify the keys are in ascending order */
  if (keys_offset > 0 && switch_count > 1) {
    int32_t last_key = switch_insns[keys_offset] | (switch_insns[keys_offset + 1] << 16);
    for (uint32_t targ = 1; targ < switch_count; targ++) {
      int32_t key = (int32_t) switch_insns[keys_offset + targ * 2] |
                    (int32_t) (switch_insns[keys_offset + targ * 2 + 1] << 16);
      if (key <= last_key) {
        Fail(VERIFY_ERROR_GENERIC) << "invalid packed switch: last key=" << last_key
                                   << ", this=" << key;
        return false;
      }
      last_key = key;
    }
  }
  /* verify each switch target */
  for (uint32_t targ = 0; targ < switch_count; targ++) {
    int32_t offset = (int32_t) switch_insns[targets_offset + targ * 2] |
                     (int32_t) (switch_insns[targets_offset + targ * 2 + 1] << 16);
    int32_t abs_offset = cur_offset + offset;
    if (abs_offset < 0 || abs_offset >= (int32_t) insn_count || !insn_flags_[abs_offset].IsOpcode()) {
      Fail(VERIFY_ERROR_GENERIC) << "invalid switch target " << offset << " (-> "
                                 << (void*) abs_offset << ") at "
                                 << (void*) cur_offset << "[" << targ << "]";
      return false;
    }
    insn_flags_[abs_offset].SetBranchTarget();
  }
  return true;
}

bool DexVerifier::CheckVarArgRegs(uint32_t vA, uint32_t arg[]) {
  if (vA > 5) {
    Fail(VERIFY_ERROR_GENERIC) << "invalid arg count (" << vA << ") in non-range invoke)";
    return false;
  }
  uint16_t registers_size = code_item_->registers_size_;
  for (uint32_t idx = 0; idx < vA; idx++) {
    if (arg[idx] > registers_size) {
      Fail(VERIFY_ERROR_GENERIC) << "invalid reg index (" << arg[idx]
                                 << ") in non-range invoke (> " << registers_size << ")";
      return false;
    }
  }

  return true;
}

bool DexVerifier::CheckVarArgRangeRegs(uint32_t vA, uint32_t vC) {
  uint16_t registers_size = code_item_->registers_size_;
  // vA/vC are unsigned 8-bit/16-bit quantities for /range instructions, so there's no risk of
  // integer overflow when adding them here.
  if (vA + vC > registers_size) {
    Fail(VERIFY_ERROR_GENERIC) << "invalid reg index " << vA << "+" << vC << " in range invoke (> "
                               << registers_size << ")";
    return false;
  }
  return true;
}

bool DexVerifier::VerifyCodeFlow() {
  uint16_t registers_size = code_item_->registers_size_;
  uint32_t insns_size = code_item_->insns_size_in_code_units_;

  if (registers_size * insns_size > 4*1024*1024) {
    Fail(VERIFY_ERROR_GENERIC) << "warning: method is huge (regs=" << registers_size
                               << " insns_size=" << insns_size << ")";
  }
  /* Create and initialize table holding register status */
  reg_table_.Init(PcToRegisterLineTable::kTrackRegsGcPoints, insn_flags_.get(), insns_size,
                  registers_size, this);

  work_line_.reset(new RegisterLine(registers_size, this));
  saved_line_.reset(new RegisterLine(registers_size, this));

  /* Initialize register types of method arguments. */
  if (!SetTypesFromSignature()) {
    DCHECK_NE(failure_, VERIFY_ERROR_NONE);
    fail_messages_ << "Bad signature in " << PrettyMethod(method_);
    return false;
  }
  /* Perform code flow verification. */
  if (!CodeFlowVerifyMethod()) {
    return false;
  }

  /* Generate a register map and add it to the method. */
  ByteArray* map = GenerateGcMap();
  if (map == NULL) {
    return false;  // Not a real failure, but a failure to encode
  }
  method_->SetGcMap(map);
#ifndef NDEBUG
  VerifyGcMap();
#endif
  return true;
}

void DexVerifier::Dump(std::ostream& os) {
  if (method_->IsNative()) {
    os << "Native method" << std::endl;
    return;
  }
  DCHECK(code_item_ != NULL);
  const Instruction* inst = Instruction::At(code_item_->insns_);
  for (size_t dex_pc = 0; dex_pc < code_item_->insns_size_in_code_units_;
      dex_pc += insn_flags_[dex_pc].GetLengthInCodeUnits()) {
    os << StringPrintf("0x%04x", dex_pc) << ": " << insn_flags_[dex_pc].Dump()
        << " " << inst->DumpHex(5) << " " << inst->DumpString(dex_file_) << std::endl;
    RegisterLine* reg_line = reg_table_.GetLine(dex_pc);
    if (reg_line != NULL) {
      os << reg_line->Dump() << std::endl;
    }
    inst = inst->Next();
  }
}

static bool IsPrimitiveDescriptor(char descriptor) {
  switch (descriptor) {
    case 'I':
    case 'C':
    case 'S':
    case 'B':
    case 'Z':
    case 'F':
    case 'D':
    case 'J':
      return true;
    default:
      return false;
  }
}

bool DexVerifier::SetTypesFromSignature() {
  RegisterLine* reg_line = reg_table_.GetLine(0);
  int arg_start = code_item_->registers_size_ - code_item_->ins_size_;
  size_t expected_args = code_item_->ins_size_;   /* long/double count as two */

  DCHECK_GE(arg_start, 0);      /* should have been verified earlier */
  //Include the "this" pointer.
  size_t cur_arg = 0;
  if (!method_->IsStatic()) {
    // If this is a constructor for a class other than java.lang.Object, mark the first ("this")
    // argument as uninitialized. This restricts field access until the superclass constructor is
    // called.
    Class* declaring_class = method_->GetDeclaringClass();
    if (method_->IsConstructor() && !declaring_class->IsObjectClass()) {
      reg_line->SetRegisterType(arg_start + cur_arg,
                                reg_types_.UninitializedThisArgument(declaring_class));
    } else {
      reg_line->SetRegisterType(arg_start + cur_arg, reg_types_.FromClass(declaring_class));
    }
    cur_arg++;
  }

  const DexFile::ProtoId& proto_id = dex_file_->GetProtoId(method_->GetProtoIdx());
  DexFile::ParameterIterator iterator(*dex_file_, proto_id);

  for (; iterator.HasNext(); iterator.Next()) {
    const char* descriptor = iterator.GetDescriptor();
    if (descriptor == NULL) {
      LOG(FATAL) << "Null descriptor";
    }
    if (cur_arg >= expected_args) {
      Fail(VERIFY_ERROR_GENERIC) << "expected " << expected_args
                                 << " args, found more (" << descriptor << ")";
      return false;
    }
    switch (descriptor[0]) {
      case 'L':
      case '[':
        // We assume that reference arguments are initialized. The only way it could be otherwise
        // (assuming the caller was verified) is if the current method is <init>, but in that case
        // it's effectively considered initialized the instant we reach here (in the sense that we
        // can return without doing anything or call virtual methods).
        {
          const RegType& reg_type =
              reg_types_.FromDescriptor(method_->GetDeclaringClass()->GetClassLoader(), descriptor);
          reg_line->SetRegisterType(arg_start + cur_arg, reg_type);
        }
        break;
      case 'Z':
        reg_line->SetRegisterType(arg_start + cur_arg, reg_types_.Boolean());
        break;
      case 'C':
        reg_line->SetRegisterType(arg_start + cur_arg, reg_types_.Char());
        break;
      case 'B':
        reg_line->SetRegisterType(arg_start + cur_arg, reg_types_.Byte());
        break;
      case 'I':
        reg_line->SetRegisterType(arg_start + cur_arg, reg_types_.Integer());
        break;
      case 'S':
        reg_line->SetRegisterType(arg_start + cur_arg, reg_types_.Short());
        break;
      case 'F':
        reg_line->SetRegisterType(arg_start + cur_arg, reg_types_.Float());
        break;
      case 'J':
      case 'D': {
        const RegType& low_half = descriptor[0] == 'J' ? reg_types_.Long() : reg_types_.Double();
        reg_line->SetRegisterType(arg_start + cur_arg, low_half);  // implicitly sets high-register
        cur_arg++;
        break;
      }
      default:
        Fail(VERIFY_ERROR_GENERIC) << "unexpected signature type char '" << descriptor << "'";
        return false;
    }
    cur_arg++;
  }
  if (cur_arg != expected_args) {
    Fail(VERIFY_ERROR_GENERIC) << "expected " << expected_args << " arguments, found " << cur_arg;
    return false;
  }
  const char* descriptor = dex_file_->GetReturnTypeDescriptor(proto_id);
  // Validate return type. We don't do the type lookup; just want to make sure that it has the right
  // format. Only major difference from the method argument format is that 'V' is supported.
  bool result;
  if (IsPrimitiveDescriptor(descriptor[0]) || descriptor[0] == 'V') {
    result = descriptor[1] == '\0';
  } else if (descriptor[0] == '[') { // single/multi-dimensional array of object/primitive
    size_t i = 0;
    do {
      i++;
    } while (descriptor[i] == '[');  // process leading [
    if (descriptor[i] == 'L') {  // object array
      do {
        i++;  // find closing ;
      } while (descriptor[i] != ';' && descriptor[i] != '\0');
      result = descriptor[i] == ';';
    } else {  // primitive array
      result = IsPrimitiveDescriptor(descriptor[i]) && descriptor[i + 1] == '\0';
    }
  } else if (descriptor[0] == 'L') {
    // could be more thorough here, but shouldn't be required
    size_t i = 0;
    do {
      i++;
    } while (descriptor[i] != ';' && descriptor[i] != '\0');
    result = descriptor[i] == ';';
  } else {
    result = false;
  }
  if (!result) {
    Fail(VERIFY_ERROR_GENERIC) << "unexpected char in return type descriptor '"
                               << descriptor << "'";
  }
  return result;
}

bool DexVerifier::CodeFlowVerifyMethod() {
  const uint16_t* insns = code_item_->insns_;
  const uint32_t insns_size = code_item_->insns_size_in_code_units_;

  /* Begin by marking the first instruction as "changed". */
  insn_flags_[0].SetChanged();
  uint32_t start_guess = 0;

  /* Continue until no instructions are marked "changed". */
  while (true) {
    // Find the first marked one. Use "start_guess" as a way to find one quickly.
    uint32_t insn_idx = start_guess;
    for (; insn_idx < insns_size; insn_idx++) {
      if (insn_flags_[insn_idx].IsChanged())
        break;
    }
    if (insn_idx == insns_size) {
      if (start_guess != 0) {
        /* try again, starting from the top */
        start_guess = 0;
        continue;
      } else {
        /* all flags are clear */
        break;
      }
    }
    // We carry the working set of registers from instruction to instruction. If this address can
    // be the target of a branch (or throw) instruction, or if we're skipping around chasing
    // "changed" flags, we need to load the set of registers from the table.
    // Because we always prefer to continue on to the next instruction, we should never have a
    // situation where we have a stray "changed" flag set on an instruction that isn't a branch
    // target.
    work_insn_idx_ = insn_idx;
    if (insn_flags_[insn_idx].IsBranchTarget()) {
      work_line_->CopyFromLine(reg_table_.GetLine(insn_idx));
    } else {
#ifndef NDEBUG
      /*
       * Sanity check: retrieve the stored register line (assuming
       * a full table) and make sure it actually matches.
       */
      RegisterLine* register_line = reg_table_.GetLine(insn_idx);
      if (register_line != NULL) {
        if (work_line_->CompareLine(register_line) != 0) {
          Dump(std::cout);
          std::cout << info_messages_.str();
          LOG(FATAL) << "work_line diverged in " << PrettyMethod(method_)
              << "@" << (void*)work_insn_idx_ << std::endl
              << " work_line=" << *work_line_ << std::endl
              << "  expected=" << *register_line;
        }
      }
#endif
    }
    if (!CodeFlowVerifyInstruction(&start_guess)) {
      fail_messages_ << std::endl << PrettyMethod(method_) << " failed to verify";
      return false;
    }
    /* Clear "changed" and mark as visited. */
    insn_flags_[insn_idx].SetVisited();
    insn_flags_[insn_idx].ClearChanged();
  }

  if (DEAD_CODE_SCAN && ((method_->GetAccessFlags() & kAccWritable) == 0)) {
    /*
     * Scan for dead code. There's nothing "evil" about dead code
     * (besides the wasted space), but it indicates a flaw somewhere
     * down the line, possibly in the verifier.
     *
     * If we've substituted "always throw" instructions into the stream,
     * we are almost certainly going to have some dead code.
     */
    int dead_start = -1;
    uint32_t insn_idx = 0;
    for (; insn_idx < insns_size; insn_idx += insn_flags_[insn_idx].GetLengthInCodeUnits()) {
      /*
       * Switch-statement data doesn't get "visited" by scanner. It
       * may or may not be preceded by a padding NOP (for alignment).
       */
      if (insns[insn_idx] == Instruction::kPackedSwitchSignature ||
          insns[insn_idx] == Instruction::kSparseSwitchSignature ||
          insns[insn_idx] == Instruction::kArrayDataSignature ||
          (insns[insn_idx] == Instruction::NOP &&
           (insns[insn_idx + 1] == Instruction::kPackedSwitchSignature ||
            insns[insn_idx + 1] == Instruction::kSparseSwitchSignature ||
            insns[insn_idx + 1] == Instruction::kArrayDataSignature))) {
        insn_flags_[insn_idx].SetVisited();
      }

      if (!insn_flags_[insn_idx].IsVisited()) {
        if (dead_start < 0)
          dead_start = insn_idx;
      } else if (dead_start >= 0) {
        LogVerifyInfo() << "dead code " << (void*) dead_start << "-" << (void*) (insn_idx - 1);
        dead_start = -1;
      }
    }
    if (dead_start >= 0) {
      LogVerifyInfo() << "dead code " << (void*) dead_start << "-" << (void*) (insn_idx - 1);
    }
  }
  return true;
}

bool DexVerifier::CodeFlowVerifyInstruction(uint32_t* start_guess) {
#ifdef VERIFIER_STATS
  if (CurrentInsnFlags().IsVisited()) {
    gDvm.verifierStats.instrsReexamined++;
  } else {
    gDvm.verifierStats.instrsExamined++;
  }
#endif

  /*
   * Once we finish decoding the instruction, we need to figure out where
   * we can go from here. There are three possible ways to transfer
   * control to another statement:
   *
   * (1) Continue to the next instruction. Applies to all but
   *     unconditional branches, method returns, and exception throws.
   * (2) Branch to one or more possible locations. Applies to branches
   *     and switch statements.
   * (3) Exception handlers. Applies to any instruction that can
   *     throw an exception that is handled by an encompassing "try"
   *     block.
   *
   * We can also return, in which case there is no successor instruction
   * from this point.
   *
   * The behavior can be determined from the OpcodeFlags.
   */
  const uint16_t* insns = code_item_->insns_ + work_insn_idx_;
  const Instruction* inst = Instruction::At(insns);
  Instruction::DecodedInstruction dec_insn(inst);
  int opcode_flag = inst->Flag();

  int32_t branch_target = 0;
  bool just_set_result = false;
  if (gDebugVerify) {
    // Generate processing back trace to debug verifier
    LogVerifyInfo() << "Processing " << inst->DumpString(dex_file_) << std::endl
                    << *work_line_.get() << std::endl;
  }

  /*
   * Make a copy of the previous register state. If the instruction
   * can throw an exception, we will copy/merge this into the "catch"
   * address rather than work_line, because we don't want the result
   * from the "successful" code path (e.g. a check-cast that "improves"
   * a type) to be visible to the exception handler.
   */
  if ((opcode_flag & Instruction::kThrow) != 0 && CurrentInsnFlags().IsInTry()) {
    saved_line_->CopyFromLine(work_line_.get());
  } else {
#ifndef NDEBUG
    saved_line_->FillWithGarbage();
#endif
  }

  switch (dec_insn.opcode_) {
    case Instruction::NOP:
      /*
       * A "pure" NOP has no effect on anything. Data tables start with
       * a signature that looks like a NOP; if we see one of these in
       * the course of executing code then we have a problem.
       */
      if (dec_insn.vA_ != 0) {
        Fail(VERIFY_ERROR_GENERIC) << "encountered data table in instruction stream";
      }
      break;

    case Instruction::MOVE:
    case Instruction::MOVE_FROM16:
    case Instruction::MOVE_16:
      work_line_->CopyRegister1(dec_insn.vA_, dec_insn.vB_, kTypeCategory1nr);
      break;
    case Instruction::MOVE_WIDE:
    case Instruction::MOVE_WIDE_FROM16:
    case Instruction::MOVE_WIDE_16:
      work_line_->CopyRegister2(dec_insn.vA_, dec_insn.vB_);
      break;
    case Instruction::MOVE_OBJECT:
    case Instruction::MOVE_OBJECT_FROM16:
    case Instruction::MOVE_OBJECT_16:
      work_line_->CopyRegister1(dec_insn.vA_, dec_insn.vB_, kTypeCategoryRef);
      break;

    /*
     * The move-result instructions copy data out of a "pseudo-register"
     * with the results from the last method invocation. In practice we
     * might want to hold the result in an actual CPU register, so the
     * Dalvik spec requires that these only appear immediately after an
     * invoke or filled-new-array.
     *
     * These calls invalidate the "result" register. (This is now
     * redundant with the reset done below, but it can make the debug info
     * easier to read in some cases.)
     */
    case Instruction::MOVE_RESULT:
      work_line_->CopyResultRegister1(dec_insn.vA_, false);
      break;
    case Instruction::MOVE_RESULT_WIDE:
      work_line_->CopyResultRegister2(dec_insn.vA_);
      break;
    case Instruction::MOVE_RESULT_OBJECT:
      work_line_->CopyResultRegister1(dec_insn.vA_, true);
      break;

    case Instruction::MOVE_EXCEPTION: {
      /*
       * This statement can only appear as the first instruction in an exception handler (though not
       * all exception handlers need to have one of these). We verify that as part of extracting the
       * exception type from the catch block list.
       */
      Class* res_class = GetCaughtExceptionType();
      if (res_class == NULL) {
        DCHECK(failure_ != VERIFY_ERROR_NONE);
      } else {
        work_line_->SetRegisterType(dec_insn.vA_, reg_types_.FromClass(res_class));
      }
      break;
    }
    case Instruction::RETURN_VOID:
      if (!method_->IsConstructor() || work_line_->CheckConstructorReturn()) {
        if (!GetMethodReturnType().IsUnknown()) {
          Fail(VERIFY_ERROR_GENERIC) << "return-void not expected";
        }
      }
      break;
    case Instruction::RETURN:
      if (!method_->IsConstructor() || work_line_->CheckConstructorReturn()) {
        /* check the method signature */
        const RegType& return_type = GetMethodReturnType();
        if (!return_type.IsCategory1Types()) {
          Fail(VERIFY_ERROR_GENERIC) << "unexpected non-category 1 return type " << return_type;
        } else {
          // Compilers may generate synthetic functions that write byte values into boolean fields.
          // Also, it may use integer values for boolean, byte, short, and character return types.
          const RegType& src_type = work_line_->GetRegisterType(dec_insn.vA_);
          bool use_src = ((return_type.IsBoolean() && src_type.IsByte()) ||
                          ((return_type.IsBoolean() || return_type.IsByte() ||
                           return_type.IsShort() || return_type.IsChar()) &&
                           src_type.IsInteger()));
          /* check the register contents */
          work_line_->VerifyRegisterType(dec_insn.vA_, use_src ? src_type : return_type);
          if (failure_ != VERIFY_ERROR_NONE) {
            fail_messages_ << " return-1nr on invalid register v" << dec_insn.vA_;
          }
        }
      }
      break;
    case Instruction::RETURN_WIDE:
      if (!method_->IsConstructor() || work_line_->CheckConstructorReturn()) {
        /* check the method signature */
        const RegType& return_type = GetMethodReturnType();
        if (!return_type.IsCategory2Types()) {
          Fail(VERIFY_ERROR_GENERIC) << "return-wide not expected";
        } else {
          /* check the register contents */
          work_line_->VerifyRegisterType(dec_insn.vA_, return_type);
          if (failure_ != VERIFY_ERROR_NONE) {
            fail_messages_ << " return-wide on invalid register pair v" << dec_insn.vA_;
          }
        }
      }
      break;
    case Instruction::RETURN_OBJECT:
      if (!method_->IsConstructor() || work_line_->CheckConstructorReturn()) {
        const RegType& return_type = GetMethodReturnType();
        if (!return_type.IsReferenceTypes()) {
          Fail(VERIFY_ERROR_GENERIC) << "return-object not expected";
        } else {
          /* return_type is the *expected* return type, not register value */
          DCHECK(!return_type.IsZero());
          DCHECK(!return_type.IsUninitializedReference());
          const RegType& reg_type = work_line_->GetRegisterType(dec_insn.vA_);
          // Disallow returning uninitialized values and verify that the reference in vAA is an
          // instance of the "return_type"
          if (reg_type.IsUninitializedTypes()) {
            Fail(VERIFY_ERROR_GENERIC) << "returning uninitialized object '" << reg_type << "'";
          } else if (!return_type.IsAssignableFrom(reg_type)) {
            Fail(VERIFY_ERROR_GENERIC) << "returning '" << reg_type
                << "', but expected from declaration '" << return_type << "'";
          }
        }
      }
      break;

    case Instruction::CONST_4:
    case Instruction::CONST_16:
    case Instruction::CONST:
      /* could be boolean, int, float, or a null reference */
      work_line_->SetRegisterType(dec_insn.vA_, reg_types_.FromCat1Const((int32_t) dec_insn.vB_));
      break;
    case Instruction::CONST_HIGH16:
      /* could be boolean, int, float, or a null reference */
      work_line_->SetRegisterType(dec_insn.vA_,
                                  reg_types_.FromCat1Const((int32_t) dec_insn.vB_ << 16));
      break;
    case Instruction::CONST_WIDE_16:
    case Instruction::CONST_WIDE_32:
    case Instruction::CONST_WIDE:
    case Instruction::CONST_WIDE_HIGH16:
      /* could be long or double; resolved upon use */
      work_line_->SetRegisterType(dec_insn.vA_, reg_types_.ConstLo());
      break;
    case Instruction::CONST_STRING:
    case Instruction::CONST_STRING_JUMBO:
      work_line_->SetRegisterType(dec_insn.vA_, reg_types_.JavaLangString());
      break;
    case Instruction::CONST_CLASS: {
      /* make sure we can resolve the class; access check is important */
      Class* res_class = ResolveClassAndCheckAccess(dec_insn.vB_);
      if (res_class == NULL) {
        const char* bad_class_desc = dex_file_->dexStringByTypeIdx(dec_insn.vB_);
        fail_messages_ << "unable to resolve const-class " << dec_insn.vB_
                       << " (" << bad_class_desc << ") in "
                       << PrettyDescriptor(method_->GetDeclaringClass()->GetDescriptor());
        DCHECK(failure_ != VERIFY_ERROR_GENERIC);
      } else {
        work_line_->SetRegisterType(dec_insn.vA_, reg_types_.JavaLangClass());
      }
      break;
    }
    case Instruction::MONITOR_ENTER:
      work_line_->PushMonitor(dec_insn.vA_, work_insn_idx_);
      break;
    case Instruction::MONITOR_EXIT:
      /*
       * monitor-exit instructions are odd. They can throw exceptions,
       * but when they do they act as if they succeeded and the PC is
       * pointing to the following instruction. (This behavior goes back
       * to the need to handle asynchronous exceptions, a now-deprecated
       * feature that Dalvik doesn't support.)
       *
       * In practice we don't need to worry about this. The only
       * exceptions that can be thrown from monitor-exit are for a
       * null reference and -exit without a matching -enter. If the
       * structured locking checks are working, the former would have
       * failed on the -enter instruction, and the latter is impossible.
       *
       * This is fortunate, because issue 3221411 prevents us from
       * chasing the "can throw" path when monitor verification is
       * enabled. If we can fully verify the locking we can ignore
       * some catch blocks (which will show up as "dead" code when
       * we skip them here); if we can't, then the code path could be
       * "live" so we still need to check it.
       */
      opcode_flag &= ~Instruction::kThrow;
      work_line_->PopMonitor(dec_insn.vA_);
      break;

    case Instruction::CHECK_CAST: {
      /*
       * If this instruction succeeds, we will promote register vA to
       * the type in vB. (This could be a demotion -- not expected, so
       * we don't try to address it.)
       *
       * If it fails, an exception is thrown, which we deal with later
       * by ignoring the update to dec_insn.vA_ when branching to a handler.
       */
      Class* res_class = ResolveClassAndCheckAccess(dec_insn.vB_);
      if (res_class == NULL) {
        const char* bad_class_desc = dex_file_->dexStringByTypeIdx(dec_insn.vB_);
        fail_messages_ << "unable to resolve check-cast " << dec_insn.vB_
                       << " (" << bad_class_desc << ") in "
                       << PrettyDescriptor(method_->GetDeclaringClass()->GetDescriptor());
        DCHECK(failure_ != VERIFY_ERROR_GENERIC);
      } else {
        const RegType& orig_type = work_line_->GetRegisterType(dec_insn.vA_);
        if (!orig_type.IsReferenceTypes()) {
          Fail(VERIFY_ERROR_GENERIC) << "check-cast on non-reference in v" << dec_insn.vA_;
        } else {
          work_line_->SetRegisterType(dec_insn.vA_, reg_types_.FromClass(res_class));
        }
      }
      break;
    }
    case Instruction::INSTANCE_OF: {
      /* make sure we're checking a reference type */
      const RegType& tmp_type = work_line_->GetRegisterType(dec_insn.vB_);
      if (!tmp_type.IsReferenceTypes()) {
        Fail(VERIFY_ERROR_GENERIC) << "vB not a reference (" << tmp_type << ")";
      } else {
        /* make sure we can resolve the class; access check is important */
        Class* res_class = ResolveClassAndCheckAccess(dec_insn.vC_);
        if (res_class == NULL) {
          const char* bad_class_desc = dex_file_->dexStringByTypeIdx(dec_insn.vC_);
          fail_messages_ << "unable to resolve instance of " << dec_insn.vC_
                         << " (" << bad_class_desc << ") in "
                         << PrettyDescriptor(method_->GetDeclaringClass()->GetDescriptor());
          DCHECK(failure_ != VERIFY_ERROR_GENERIC);
        } else {
          /* result is boolean */
          work_line_->SetRegisterType(dec_insn.vA_, reg_types_.Boolean());
        }
      }
      break;
    }
    case Instruction::ARRAY_LENGTH: {
      Class* res_class = work_line_->GetClassFromRegister(dec_insn.vB_);
      if (failure_ == VERIFY_ERROR_NONE) {
        if (res_class != NULL && !res_class->IsArrayClass()) {
          Fail(VERIFY_ERROR_GENERIC) << "array-length on non-array";
        } else {
          work_line_->SetRegisterType(dec_insn.vA_, reg_types_.Integer());
        }
      }
      break;
    }
    case Instruction::NEW_INSTANCE: {
      Class* res_class = ResolveClassAndCheckAccess(dec_insn.vB_);
      if (res_class == NULL) {
        const char* bad_class_desc = dex_file_->dexStringByTypeIdx(dec_insn.vB_);
        fail_messages_ << "unable to resolve new-instance " << dec_insn.vB_
                       << " (" << bad_class_desc << ") in "
                       << PrettyDescriptor(method_->GetDeclaringClass()->GetDescriptor());
        DCHECK(failure_ != VERIFY_ERROR_GENERIC);
      } else {
        /* can't create an instance of an interface or abstract class */
        if (res_class->IsPrimitive() || res_class->IsAbstract() || res_class->IsInterface()) {
          Fail(VERIFY_ERROR_INSTANTIATION)
              << "new-instance on primitive, interface or abstract class"
              << PrettyDescriptor(res_class->GetDescriptor());
        } else {
          const RegType& uninit_type = reg_types_.Uninitialized(res_class, work_insn_idx_);
          // Any registers holding previous allocations from this address that have not yet been
          // initialized must be marked invalid.
          work_line_->MarkUninitRefsAsInvalid(uninit_type);

          /* add the new uninitialized reference to the register state */
          work_line_->SetRegisterType(dec_insn.vA_, uninit_type);
        }
      }
      break;
    }
    case Instruction::NEW_ARRAY: {
      Class* res_class = ResolveClassAndCheckAccess(dec_insn.vC_);
      if (res_class == NULL) {
        const char* bad_class_desc = dex_file_->dexStringByTypeIdx(dec_insn.vC_);
        fail_messages_ << "unable to resolve new-array " << dec_insn.vC_
                       << " (" << bad_class_desc << ") in "
                       << PrettyDescriptor(method_->GetDeclaringClass()->GetDescriptor());
        DCHECK(failure_ != VERIFY_ERROR_GENERIC);
      } else if (!res_class->IsArrayClass()) {
        Fail(VERIFY_ERROR_GENERIC) << "new-array on non-array class";
      } else {
        /* make sure "size" register is valid type */
        work_line_->VerifyRegisterType(dec_insn.vB_, reg_types_.Integer());
        /* set register type to array class */
        work_line_->SetRegisterType(dec_insn.vA_, reg_types_.FromClass(res_class));
      }
      break;
    }
    case Instruction::FILLED_NEW_ARRAY:
    case Instruction::FILLED_NEW_ARRAY_RANGE: {
      Class* res_class = ResolveClassAndCheckAccess(dec_insn.vB_);
      if (res_class == NULL) {
        const char* bad_class_desc = dex_file_->dexStringByTypeIdx(dec_insn.vB_);
        fail_messages_ << "unable to resolve filled-array " << dec_insn.vB_
                       << " (" << bad_class_desc << ") in "
                       << PrettyDescriptor(method_->GetDeclaringClass()->GetDescriptor());
        DCHECK(failure_ != VERIFY_ERROR_GENERIC);
      } else if (!res_class->IsArrayClass()) {
        Fail(VERIFY_ERROR_GENERIC) << "filled-new-array on non-array class";
      } else {
        bool is_range = (dec_insn.opcode_ == Instruction::FILLED_NEW_ARRAY_RANGE);
        /* check the arguments to the instruction */
        VerifyFilledNewArrayRegs(dec_insn, res_class, is_range);
        /* filled-array result goes into "result" register */
        work_line_->SetResultRegisterType(reg_types_.FromClass(res_class));
        just_set_result = true;
      }
      break;
    }
    case Instruction::CMPL_FLOAT:
    case Instruction::CMPG_FLOAT:
      work_line_->VerifyRegisterType(dec_insn.vB_, reg_types_.Float());
      work_line_->VerifyRegisterType(dec_insn.vC_, reg_types_.Float());
      work_line_->SetRegisterType(dec_insn.vA_, reg_types_.Integer());
      break;
    case Instruction::CMPL_DOUBLE:
    case Instruction::CMPG_DOUBLE:
      work_line_->VerifyRegisterType(dec_insn.vB_, reg_types_.Double());
      work_line_->VerifyRegisterType(dec_insn.vC_, reg_types_.Double());
      work_line_->SetRegisterType(dec_insn.vA_, reg_types_.Integer());
      break;
    case Instruction::CMP_LONG:
      work_line_->VerifyRegisterType(dec_insn.vB_, reg_types_.Long());
      work_line_->VerifyRegisterType(dec_insn.vC_, reg_types_.Long());
      work_line_->SetRegisterType(dec_insn.vA_, reg_types_.Integer());
      break;
    case Instruction::THROW: {
      Class* res_class = work_line_->GetClassFromRegister(dec_insn.vA_);
      if (failure_ == VERIFY_ERROR_NONE && res_class != NULL) {
        if (!JavaLangThrowable()->IsAssignableFrom(res_class)) {
          Fail(VERIFY_ERROR_GENERIC) << "thrown class "
              << PrettyDescriptor(res_class->GetDescriptor()) << " not instanceof Throwable";
        }
      }
      break;
    }
    case Instruction::GOTO:
    case Instruction::GOTO_16:
    case Instruction::GOTO_32:
      /* no effect on or use of registers */
      break;

    case Instruction::PACKED_SWITCH:
    case Instruction::SPARSE_SWITCH:
      /* verify that vAA is an integer, or can be converted to one */
      work_line_->VerifyRegisterType(dec_insn.vA_, reg_types_.Integer());
      break;

    case Instruction::FILL_ARRAY_DATA: {
      /* Similar to the verification done for APUT */
      Class* res_class = work_line_->GetClassFromRegister(dec_insn.vA_);
      if (failure_ == VERIFY_ERROR_NONE) {
        /* res_class can be null if the reg type is Zero */
        if (res_class != NULL) {
          Class* component_type = res_class->GetComponentType();
          if (!res_class->IsArrayClass() || !component_type->IsPrimitive()  ||
              component_type->IsPrimitiveVoid()) {
            Fail(VERIFY_ERROR_GENERIC) << "invalid fill-array-data on "
                                       << PrettyDescriptor(res_class->GetDescriptor());
          } else {
            const RegType& value_type = reg_types_.FromClass(component_type);
            DCHECK(!value_type.IsUnknown());
            // Now verify if the element width in the table matches the element width declared in
            // the array
            const uint16_t* array_data = insns + (insns[1] | (((int32_t) insns[2]) << 16));
            if (array_data[0] != Instruction::kArrayDataSignature) {
              Fail(VERIFY_ERROR_GENERIC) << "invalid magic for array-data";
            } else {
              size_t elem_width = Primitive::ComponentSize(component_type->GetPrimitiveType());
              // Since we don't compress the data in Dex, expect to see equal width of data stored
              // in the table and expected from the array class.
              if (array_data[1] != elem_width) {
                Fail(VERIFY_ERROR_GENERIC) << "array-data size mismatch (" << array_data[1]
                                           << " vs " << elem_width << ")";
              }
            }
          }
        }
      }
      break;
    }
    case Instruction::IF_EQ:
    case Instruction::IF_NE: {
      const RegType& reg_type1 = work_line_->GetRegisterType(dec_insn.vA_);
      const RegType& reg_type2 = work_line_->GetRegisterType(dec_insn.vB_);
      bool mismatch = false;
      if (reg_type1.IsZero()) {  // zero then integral or reference expected
        mismatch = !reg_type2.IsReferenceTypes() && !reg_type2.IsIntegralTypes();
      } else if (reg_type1.IsReferenceTypes()) {  // both references?
        mismatch = !reg_type2.IsReferenceTypes();
      } else {  // both integral?
        mismatch = !reg_type1.IsIntegralTypes() || !reg_type2.IsIntegralTypes();
      }
      if (mismatch) {
        Fail(VERIFY_ERROR_GENERIC) << "args to if-eq/if-ne (" << reg_type1 << "," << reg_type2
                                   << ") must both be references or integral";
      }
      break;
    }
    case Instruction::IF_LT:
    case Instruction::IF_GE:
    case Instruction::IF_GT:
    case Instruction::IF_LE: {
      const RegType& reg_type1 = work_line_->GetRegisterType(dec_insn.vA_);
      const RegType& reg_type2 = work_line_->GetRegisterType(dec_insn.vB_);
      if (!reg_type1.IsIntegralTypes() || !reg_type2.IsIntegralTypes()) {
        Fail(VERIFY_ERROR_GENERIC) << "args to 'if' (" << reg_type1 << ","
                                   << reg_type2 << ") must be integral";
      }
      break;
    }
    case Instruction::IF_EQZ:
    case Instruction::IF_NEZ: {
      const RegType& reg_type = work_line_->GetRegisterType(dec_insn.vA_);
      if (!reg_type.IsReferenceTypes() && !reg_type.IsIntegralTypes()) {
        Fail(VERIFY_ERROR_GENERIC) << "type " << reg_type << " unexpected as arg to if-eqz/if-nez";
      }
      break;
    }
    case Instruction::IF_LTZ:
    case Instruction::IF_GEZ:
    case Instruction::IF_GTZ:
    case Instruction::IF_LEZ: {
      const RegType& reg_type = work_line_->GetRegisterType(dec_insn.vA_);
      if (!reg_type.IsIntegralTypes()) {
        Fail(VERIFY_ERROR_GENERIC) << "type " << reg_type
                                   << " unexpected as arg to if-ltz/if-gez/if-gtz/if-lez";
      }
      break;
    }
    case Instruction::AGET_BOOLEAN:
      VerifyAGet(dec_insn, reg_types_.Boolean(), true);
      break;
    case Instruction::AGET_BYTE:
      VerifyAGet(dec_insn, reg_types_.Byte(), true);
      break;
    case Instruction::AGET_CHAR:
      VerifyAGet(dec_insn, reg_types_.Char(), true);
      break;
    case Instruction::AGET_SHORT:
      VerifyAGet(dec_insn, reg_types_.Short(), true);
      break;
    case Instruction::AGET:
      VerifyAGet(dec_insn, reg_types_.Integer(), true);
      break;
    case Instruction::AGET_WIDE:
      VerifyAGet(dec_insn, reg_types_.Long(), true);
      break;
    case Instruction::AGET_OBJECT:
      VerifyAGet(dec_insn, reg_types_.JavaLangObject(), false);
      break;

    case Instruction::APUT_BOOLEAN:
      VerifyAPut(dec_insn, reg_types_.Boolean(), true);
      break;
    case Instruction::APUT_BYTE:
      VerifyAPut(dec_insn, reg_types_.Byte(), true);
      break;
    case Instruction::APUT_CHAR:
      VerifyAPut(dec_insn, reg_types_.Char(), true);
      break;
    case Instruction::APUT_SHORT:
      VerifyAPut(dec_insn, reg_types_.Short(), true);
      break;
    case Instruction::APUT:
      VerifyAPut(dec_insn, reg_types_.Integer(), true);
      break;
    case Instruction::APUT_WIDE:
      VerifyAPut(dec_insn, reg_types_.Long(), true);
      break;
    case Instruction::APUT_OBJECT:
      VerifyAPut(dec_insn, reg_types_.JavaLangObject(), false);
      break;

    case Instruction::IGET_BOOLEAN:
      VerifyISGet(dec_insn, reg_types_.Boolean(), true, false);
      break;
    case Instruction::IGET_BYTE:
      VerifyISGet(dec_insn, reg_types_.Byte(), true, false);
      break;
    case Instruction::IGET_CHAR:
      VerifyISGet(dec_insn, reg_types_.Char(), true, false);
      break;
    case Instruction::IGET_SHORT:
      VerifyISGet(dec_insn, reg_types_.Short(), true, false);
      break;
    case Instruction::IGET:
      VerifyISGet(dec_insn, reg_types_.Integer(), true, false);
      break;
    case Instruction::IGET_WIDE:
      VerifyISGet(dec_insn, reg_types_.Long(), true, false);
      break;
    case Instruction::IGET_OBJECT:
      VerifyISGet(dec_insn, reg_types_.JavaLangObject(), false, false);
      break;

    case Instruction::IPUT_BOOLEAN:
      VerifyISPut(dec_insn, reg_types_.Boolean(), true, false);
      break;
    case Instruction::IPUT_BYTE:
      VerifyISPut(dec_insn, reg_types_.Byte(), true, false);
      break;
    case Instruction::IPUT_CHAR:
      VerifyISPut(dec_insn, reg_types_.Char(), true, false);
      break;
    case Instruction::IPUT_SHORT:
      VerifyISPut(dec_insn, reg_types_.Short(), true, false);
      break;
    case Instruction::IPUT:
      VerifyISPut(dec_insn, reg_types_.Integer(), true, false);
      break;
    case Instruction::IPUT_WIDE:
      VerifyISPut(dec_insn, reg_types_.Long(), true, false);
      break;
    case Instruction::IPUT_OBJECT:
      VerifyISPut(dec_insn, reg_types_.JavaLangObject(), false, false);
      break;

    case Instruction::SGET_BOOLEAN:
      VerifyISGet(dec_insn, reg_types_.Boolean(), true, true);
      break;
    case Instruction::SGET_BYTE:
      VerifyISGet(dec_insn, reg_types_.Byte(), true, true);
      break;
    case Instruction::SGET_CHAR:
      VerifyISGet(dec_insn, reg_types_.Char(), true, true);
      break;
    case Instruction::SGET_SHORT:
      VerifyISGet(dec_insn, reg_types_.Short(), true, true);
      break;
    case Instruction::SGET:
      VerifyISGet(dec_insn, reg_types_.Integer(), true, true);
      break;
    case Instruction::SGET_WIDE:
      VerifyISGet(dec_insn, reg_types_.Long(), true, true);
      break;
    case Instruction::SGET_OBJECT:
      VerifyISGet(dec_insn, reg_types_.JavaLangObject(), false, true);
      break;

    case Instruction::SPUT_BOOLEAN:
      VerifyISPut(dec_insn, reg_types_.Boolean(), true, true);
      break;
    case Instruction::SPUT_BYTE:
      VerifyISPut(dec_insn, reg_types_.Byte(), true, true);
      break;
    case Instruction::SPUT_CHAR:
      VerifyISPut(dec_insn, reg_types_.Char(), true, true);
      break;
    case Instruction::SPUT_SHORT:
      VerifyISPut(dec_insn, reg_types_.Short(), true, true);
      break;
    case Instruction::SPUT:
      VerifyISPut(dec_insn, reg_types_.Integer(), true, true);
      break;
    case Instruction::SPUT_WIDE:
      VerifyISPut(dec_insn, reg_types_.Long(), true, true);
      break;
    case Instruction::SPUT_OBJECT:
      VerifyISPut(dec_insn, reg_types_.JavaLangObject(), false, true);
      break;

    case Instruction::INVOKE_VIRTUAL:
    case Instruction::INVOKE_VIRTUAL_RANGE:
    case Instruction::INVOKE_SUPER:
    case Instruction::INVOKE_SUPER_RANGE: {
      bool is_range = (dec_insn.opcode_ == Instruction::INVOKE_VIRTUAL_RANGE ||
                       dec_insn.opcode_ == Instruction::INVOKE_SUPER_RANGE);
      bool is_super =  (dec_insn.opcode_ == Instruction::INVOKE_SUPER ||
                        dec_insn.opcode_ == Instruction::INVOKE_SUPER_RANGE);
      Method* called_method = VerifyInvocationArgs(dec_insn, METHOD_VIRTUAL, is_range, is_super);
      if (failure_ == VERIFY_ERROR_NONE) {
        const RegType& return_type =
            reg_types_.FromDescriptor(called_method->GetDeclaringClass()->GetClassLoader(),
                                      called_method->GetReturnTypeDescriptor());
        work_line_->SetResultRegisterType(return_type);
        just_set_result = true;
      }
      break;
    }
    case Instruction::INVOKE_DIRECT:
    case Instruction::INVOKE_DIRECT_RANGE: {
      bool is_range = (dec_insn.opcode_ == Instruction::INVOKE_DIRECT_RANGE);
      Method* called_method = VerifyInvocationArgs(dec_insn, METHOD_DIRECT, is_range, false);
      if (failure_ == VERIFY_ERROR_NONE) {
        /*
         * Some additional checks when calling a constructor. We know from the invocation arg check
         * that the "this" argument is an instance of called_method->klass. Now we further restrict
         * that to require that called_method->klass is the same as this->klass or this->super,
         * allowing the latter only if the "this" argument is the same as the "this" argument to
         * this method (which implies that we're in a constructor ourselves).
         */
        if (called_method->IsConstructor()) {
          const RegType& this_type = work_line_->GetInvocationThis(dec_insn);
          if (failure_ != VERIFY_ERROR_NONE)
            break;

          /* no null refs allowed (?) */
          if (this_type.IsZero()) {
            Fail(VERIFY_ERROR_GENERIC) << "unable to initialize null ref";
            break;
          }
          Class* this_class = this_type.GetClass();
          DCHECK(this_class != NULL);

          /* must be in same class or in superclass */
          if (called_method->GetDeclaringClass() == this_class->GetSuperClass()) {
            if (this_class != method_->GetDeclaringClass()) {
              Fail(VERIFY_ERROR_GENERIC)
                  << "invoke-direct <init> on super only allowed for 'this' in <init>";
              break;
            }
          }  else if (called_method->GetDeclaringClass() != this_class) {
            Fail(VERIFY_ERROR_GENERIC) << "invoke-direct <init> must be on current class or super";
            break;
          }

          /* arg must be an uninitialized reference */
          if (!this_type.IsUninitializedTypes()) {
            Fail(VERIFY_ERROR_GENERIC) << "Expected initialization on uninitialized reference "
                << this_type;
            break;
          }

          /*
           * Replace the uninitialized reference with an initialized one. We need to do this for all
           * registers that have the same object instance in them, not just the "this" register.
           */
          work_line_->MarkRefsAsInitialized(this_type);
          if (failure_ != VERIFY_ERROR_NONE)
            break;
        }
        const RegType& return_type =
            reg_types_.FromDescriptor(called_method->GetDeclaringClass()->GetClassLoader(),
                                      called_method->GetReturnTypeDescriptor());
        work_line_->SetResultRegisterType(return_type);
        just_set_result = true;
      }
      break;
    }
    case Instruction::INVOKE_STATIC:
    case Instruction::INVOKE_STATIC_RANGE: {
        bool is_range = (dec_insn.opcode_ == Instruction::INVOKE_STATIC_RANGE);
        Method* called_method = VerifyInvocationArgs(dec_insn, METHOD_STATIC, is_range, false);
        if (failure_ == VERIFY_ERROR_NONE) {
          const RegType& return_type =
              reg_types_.FromDescriptor(called_method->GetDeclaringClass()->GetClassLoader(),
                                        called_method->GetReturnTypeDescriptor());
          work_line_->SetResultRegisterType(return_type);
          just_set_result = true;
        }
      }
      break;
    case Instruction::INVOKE_INTERFACE:
    case Instruction::INVOKE_INTERFACE_RANGE: {
      bool is_range =  (dec_insn.opcode_ == Instruction::INVOKE_INTERFACE_RANGE);
      Method* abs_method = VerifyInvocationArgs(dec_insn, METHOD_INTERFACE, is_range, false);
      if (failure_ == VERIFY_ERROR_NONE) {
        Class* called_interface = abs_method->GetDeclaringClass();
        if (!called_interface->IsInterface()) {
          Fail(VERIFY_ERROR_CLASS_CHANGE) << "expected interface class in invoke-interface '"
                                          << PrettyMethod(abs_method) << "'";
          break;
        } else {
          /* Get the type of the "this" arg, which should either be a sub-interface of called
           * interface or Object (see comments in RegType::JoinClass).
           */
          const RegType& this_type = work_line_->GetInvocationThis(dec_insn);
          if (failure_ == VERIFY_ERROR_NONE) {
            if (this_type.IsZero()) {
              /* null pointer always passes (and always fails at runtime) */
            } else {
              if (this_type.IsUninitializedTypes()) {
                Fail(VERIFY_ERROR_GENERIC) << "interface call on uninitialized object "
                    << this_type;
                break;
              }
              // In the past we have tried to assert that "called_interface" is assignable
              // from "this_type.GetClass()", however, as we do an imprecise Join
              // (RegType::JoinClass) we don't have full information on what interfaces are
              // implemented by "this_type". For example, two classes may implement the same
              // interfaces and have a common parent that doesn't implement the interface. The
              // join will set "this_type" to the parent class and a test that this implements
              // the interface will incorrectly fail.
            }
          }
        }
        /*
         * We don't have an object instance, so we can't find the concrete method. However, all of
         * the type information is in the abstract method, so we're good.
         */
        const RegType& return_type =
            reg_types_.FromDescriptor(abs_method->GetDeclaringClass()->GetClassLoader(),
                                      abs_method->GetReturnTypeDescriptor());
        work_line_->SetResultRegisterType(return_type);
        just_set_result = true;
      }
      break;
    }
    case Instruction::NEG_INT:
    case Instruction::NOT_INT:
      work_line_->CheckUnaryOp(dec_insn, reg_types_.Integer(), reg_types_.Integer());
      break;
    case Instruction::NEG_LONG:
    case Instruction::NOT_LONG:
      work_line_->CheckUnaryOp(dec_insn, reg_types_.Long(), reg_types_.Long());
      break;
    case Instruction::NEG_FLOAT:
      work_line_->CheckUnaryOp(dec_insn, reg_types_.Float(), reg_types_.Float());
      break;
    case Instruction::NEG_DOUBLE:
      work_line_->CheckUnaryOp(dec_insn, reg_types_.Double(), reg_types_.Double());
      break;
    case Instruction::INT_TO_LONG:
      work_line_->CheckUnaryOp(dec_insn, reg_types_.Long(), reg_types_.Integer());
      break;
    case Instruction::INT_TO_FLOAT:
      work_line_->CheckUnaryOp(dec_insn, reg_types_.Float(), reg_types_.Integer());
      break;
    case Instruction::INT_TO_DOUBLE:
      work_line_->CheckUnaryOp(dec_insn, reg_types_.Double(), reg_types_.Integer());
      break;
    case Instruction::LONG_TO_INT:
      work_line_->CheckUnaryOp(dec_insn, reg_types_.Integer(), reg_types_.Long());
      break;
    case Instruction::LONG_TO_FLOAT:
      work_line_->CheckUnaryOp(dec_insn, reg_types_.Float(), reg_types_.Long());
      break;
    case Instruction::LONG_TO_DOUBLE:
      work_line_->CheckUnaryOp(dec_insn, reg_types_.Double(), reg_types_.Long());
      break;
    case Instruction::FLOAT_TO_INT:
      work_line_->CheckUnaryOp(dec_insn, reg_types_.Integer(), reg_types_.Float());
      break;
    case Instruction::FLOAT_TO_LONG:
      work_line_->CheckUnaryOp(dec_insn, reg_types_.Long(), reg_types_.Float());
      break;
    case Instruction::FLOAT_TO_DOUBLE:
      work_line_->CheckUnaryOp(dec_insn, reg_types_.Double(), reg_types_.Float());
      break;
    case Instruction::DOUBLE_TO_INT:
      work_line_->CheckUnaryOp(dec_insn, reg_types_.Integer(), reg_types_.Double());
      break;
    case Instruction::DOUBLE_TO_LONG:
      work_line_->CheckUnaryOp(dec_insn, reg_types_.Long(), reg_types_.Double());
      break;
    case Instruction::DOUBLE_TO_FLOAT:
      work_line_->CheckUnaryOp(dec_insn, reg_types_.Float(), reg_types_.Double());
      break;
    case Instruction::INT_TO_BYTE:
      work_line_->CheckUnaryOp(dec_insn, reg_types_.Byte(), reg_types_.Integer());
      break;
    case Instruction::INT_TO_CHAR:
      work_line_->CheckUnaryOp(dec_insn, reg_types_.Char(), reg_types_.Integer());
      break;
    case Instruction::INT_TO_SHORT:
      work_line_->CheckUnaryOp(dec_insn, reg_types_.Short(), reg_types_.Integer());
      break;

    case Instruction::ADD_INT:
    case Instruction::SUB_INT:
    case Instruction::MUL_INT:
    case Instruction::REM_INT:
    case Instruction::DIV_INT:
    case Instruction::SHL_INT:
    case Instruction::SHR_INT:
    case Instruction::USHR_INT:
      work_line_->CheckBinaryOp(dec_insn, reg_types_.Integer(), reg_types_.Integer(), reg_types_.Integer(), false);
      break;
    case Instruction::AND_INT:
    case Instruction::OR_INT:
    case Instruction::XOR_INT:
      work_line_->CheckBinaryOp(dec_insn, reg_types_.Integer(), reg_types_.Integer(), reg_types_.Integer(), true);
      break;
    case Instruction::ADD_LONG:
    case Instruction::SUB_LONG:
    case Instruction::MUL_LONG:
    case Instruction::DIV_LONG:
    case Instruction::REM_LONG:
    case Instruction::AND_LONG:
    case Instruction::OR_LONG:
    case Instruction::XOR_LONG:
      work_line_->CheckBinaryOp(dec_insn, reg_types_.Long(), reg_types_.Long(), reg_types_.Long(), false);
      break;
    case Instruction::SHL_LONG:
    case Instruction::SHR_LONG:
    case Instruction::USHR_LONG:
      /* shift distance is Int, making these different from other binary operations */
      work_line_->CheckBinaryOp(dec_insn, reg_types_.Long(), reg_types_.Long(), reg_types_.Integer(), false);
      break;
    case Instruction::ADD_FLOAT:
    case Instruction::SUB_FLOAT:
    case Instruction::MUL_FLOAT:
    case Instruction::DIV_FLOAT:
    case Instruction::REM_FLOAT:
      work_line_->CheckBinaryOp(dec_insn, reg_types_.Float(), reg_types_.Float(), reg_types_.Float(), false);
      break;
    case Instruction::ADD_DOUBLE:
    case Instruction::SUB_DOUBLE:
    case Instruction::MUL_DOUBLE:
    case Instruction::DIV_DOUBLE:
    case Instruction::REM_DOUBLE:
      work_line_->CheckBinaryOp(dec_insn, reg_types_.Double(), reg_types_.Double(), reg_types_.Double(), false);
      break;
    case Instruction::ADD_INT_2ADDR:
    case Instruction::SUB_INT_2ADDR:
    case Instruction::MUL_INT_2ADDR:
    case Instruction::REM_INT_2ADDR:
    case Instruction::SHL_INT_2ADDR:
    case Instruction::SHR_INT_2ADDR:
    case Instruction::USHR_INT_2ADDR:
      work_line_->CheckBinaryOp2addr(dec_insn, reg_types_.Integer(), reg_types_.Integer(), reg_types_.Integer(), false);
      break;
    case Instruction::AND_INT_2ADDR:
    case Instruction::OR_INT_2ADDR:
    case Instruction::XOR_INT_2ADDR:
      work_line_->CheckBinaryOp2addr(dec_insn, reg_types_.Integer(), reg_types_.Integer(), reg_types_.Integer(), true);
      break;
    case Instruction::DIV_INT_2ADDR:
      work_line_->CheckBinaryOp2addr(dec_insn, reg_types_.Integer(), reg_types_.Integer(), reg_types_.Integer(), false);
      break;
    case Instruction::ADD_LONG_2ADDR:
    case Instruction::SUB_LONG_2ADDR:
    case Instruction::MUL_LONG_2ADDR:
    case Instruction::DIV_LONG_2ADDR:
    case Instruction::REM_LONG_2ADDR:
    case Instruction::AND_LONG_2ADDR:
    case Instruction::OR_LONG_2ADDR:
    case Instruction::XOR_LONG_2ADDR:
      work_line_->CheckBinaryOp2addr(dec_insn, reg_types_.Long(), reg_types_.Long(), reg_types_.Long(), false);
      break;
    case Instruction::SHL_LONG_2ADDR:
    case Instruction::SHR_LONG_2ADDR:
    case Instruction::USHR_LONG_2ADDR:
      work_line_->CheckBinaryOp2addr(dec_insn, reg_types_.Long(), reg_types_.Long(), reg_types_.Integer(), false);
      break;
    case Instruction::ADD_FLOAT_2ADDR:
    case Instruction::SUB_FLOAT_2ADDR:
    case Instruction::MUL_FLOAT_2ADDR:
    case Instruction::DIV_FLOAT_2ADDR:
    case Instruction::REM_FLOAT_2ADDR:
      work_line_->CheckBinaryOp2addr(dec_insn, reg_types_.Float(), reg_types_.Float(), reg_types_.Float(), false);
      break;
    case Instruction::ADD_DOUBLE_2ADDR:
    case Instruction::SUB_DOUBLE_2ADDR:
    case Instruction::MUL_DOUBLE_2ADDR:
    case Instruction::DIV_DOUBLE_2ADDR:
    case Instruction::REM_DOUBLE_2ADDR:
      work_line_->CheckBinaryOp2addr(dec_insn, reg_types_.Double(), reg_types_.Double(),  reg_types_.Double(), false);
      break;
    case Instruction::ADD_INT_LIT16:
    case Instruction::RSUB_INT:
    case Instruction::MUL_INT_LIT16:
    case Instruction::DIV_INT_LIT16:
    case Instruction::REM_INT_LIT16:
      work_line_->CheckLiteralOp(dec_insn, reg_types_.Integer(), reg_types_.Integer(), false);
      break;
    case Instruction::AND_INT_LIT16:
    case Instruction::OR_INT_LIT16:
    case Instruction::XOR_INT_LIT16:
      work_line_->CheckLiteralOp(dec_insn, reg_types_.Integer(), reg_types_.Integer(), true);
      break;
    case Instruction::ADD_INT_LIT8:
    case Instruction::RSUB_INT_LIT8:
    case Instruction::MUL_INT_LIT8:
    case Instruction::DIV_INT_LIT8:
    case Instruction::REM_INT_LIT8:
    case Instruction::SHL_INT_LIT8:
    case Instruction::SHR_INT_LIT8:
    case Instruction::USHR_INT_LIT8:
      work_line_->CheckLiteralOp(dec_insn, reg_types_.Integer(), reg_types_.Integer(), false);
      break;
    case Instruction::AND_INT_LIT8:
    case Instruction::OR_INT_LIT8:
    case Instruction::XOR_INT_LIT8:
      work_line_->CheckLiteralOp(dec_insn, reg_types_.Integer(), reg_types_.Integer(), true);
      break;

    /*
     * This falls into the general category of "optimized" instructions,
     * which don't generally appear during verification. Because it's
     * inserted in the course of verification, we can expect to see it here.
     */
    case Instruction::THROW_VERIFICATION_ERROR:
      break;

    /* These should never appear during verification. */
    case Instruction::UNUSED_EE:
    case Instruction::UNUSED_EF:
    case Instruction::UNUSED_F2:
    case Instruction::UNUSED_F3:
    case Instruction::UNUSED_F4:
    case Instruction::UNUSED_F5:
    case Instruction::UNUSED_F6:
    case Instruction::UNUSED_F7:
    case Instruction::UNUSED_F8:
    case Instruction::UNUSED_F9:
    case Instruction::UNUSED_FA:
    case Instruction::UNUSED_FB:
    case Instruction::UNUSED_F0:
    case Instruction::UNUSED_F1:
    case Instruction::UNUSED_E3:
    case Instruction::UNUSED_E8:
    case Instruction::UNUSED_E7:
    case Instruction::UNUSED_E4:
    case Instruction::UNUSED_E9:
    case Instruction::UNUSED_FC:
    case Instruction::UNUSED_E5:
    case Instruction::UNUSED_EA:
    case Instruction::UNUSED_FD:
    case Instruction::UNUSED_E6:
    case Instruction::UNUSED_EB:
    case Instruction::UNUSED_FE:
    case Instruction::UNUSED_3E:
    case Instruction::UNUSED_3F:
    case Instruction::UNUSED_40:
    case Instruction::UNUSED_41:
    case Instruction::UNUSED_42:
    case Instruction::UNUSED_43:
    case Instruction::UNUSED_73:
    case Instruction::UNUSED_79:
    case Instruction::UNUSED_7A:
    case Instruction::UNUSED_EC:
    case Instruction::UNUSED_FF:
      Fail(VERIFY_ERROR_GENERIC) << "Unexpected opcode " << inst->DumpString(dex_file_);
      break;

    /*
     * DO NOT add a "default" clause here. Without it the compiler will
     * complain if an instruction is missing (which is desirable).
     */
  }  // end - switch (dec_insn.opcode_)

  if (failure_ != VERIFY_ERROR_NONE) {
    if (failure_ == VERIFY_ERROR_GENERIC) {
      /* immediate failure, reject class */
      fail_messages_ << std::endl << "Rejecting opcode " << inst->DumpString(dex_file_);
      return false;
    } else {
      /* replace opcode and continue on */
      fail_messages_ << std::endl << "Replacing opcode " << inst->DumpString(dex_file_);
      ReplaceFailingInstruction();
      /* IMPORTANT: method->insns may have been changed */
      insns = code_item_->insns_ + work_insn_idx_;
      /* continue on as if we just handled a throw-verification-error */
      failure_ = VERIFY_ERROR_NONE;
      opcode_flag = Instruction::kThrow;
    }
  }
  /*
   * If we didn't just set the result register, clear it out. This ensures that you can only use
   * "move-result" immediately after the result is set. (We could check this statically, but it's
   * not expensive and it makes our debugging output cleaner.)
   */
  if (!just_set_result) {
    work_line_->SetResultTypeToUnknown();
  }

  /* Handle "continue". Tag the next consecutive instruction. */
  if ((opcode_flag & Instruction::kContinue) != 0) {
    uint32_t next_insn_idx = work_insn_idx_ + CurrentInsnFlags().GetLengthInCodeUnits();
    if (next_insn_idx >= code_item_->insns_size_in_code_units_) {
      Fail(VERIFY_ERROR_GENERIC) << "Execution can walk off end of code area";
      return false;
    }
    // The only way to get to a move-exception instruction is to get thrown there. Make sure the
    // next instruction isn't one.
    if (!CheckMoveException(code_item_->insns_, next_insn_idx)) {
      return false;
    }
    RegisterLine* next_line = reg_table_.GetLine(next_insn_idx);
    if (next_line != NULL) {
      // Merge registers into what we have for the next instruction, and set the "changed" flag if
      // needed.
      if (!UpdateRegisters(next_insn_idx, work_line_.get())) {
        return false;
      }
    } else {
      /*
       * We're not recording register data for the next instruction, so we don't know what the prior
       * state was. We have to assume that something has changed and re-evaluate it.
       */
      insn_flags_[next_insn_idx].SetChanged();
    }
  }

  /*
   * Handle "branch". Tag the branch target.
   *
   * NOTE: instructions like Instruction::EQZ provide information about the
   * state of the register when the branch is taken or not taken. For example,
   * somebody could get a reference field, check it for zero, and if the
   * branch is taken immediately store that register in a boolean field
   * since the value is known to be zero. We do not currently account for
   * that, and will reject the code.
   *
   * TODO: avoid re-fetching the branch target
   */
  if ((opcode_flag & Instruction::kBranch) != 0) {
    bool isConditional, selfOkay;
    if (!GetBranchOffset(work_insn_idx_, &branch_target, &isConditional, &selfOkay)) {
      /* should never happen after static verification */
      Fail(VERIFY_ERROR_GENERIC) << "bad branch";
      return false;
    }
    DCHECK_EQ(isConditional, (opcode_flag & Instruction::kContinue) != 0);
    if (!CheckMoveException(code_item_->insns_, work_insn_idx_ + branch_target)) {
      return false;
    }
    /* update branch target, set "changed" if appropriate */
    if (!UpdateRegisters(work_insn_idx_ + branch_target, work_line_.get())) {
      return false;
    }
  }

  /*
   * Handle "switch". Tag all possible branch targets.
   *
   * We've already verified that the table is structurally sound, so we
   * just need to walk through and tag the targets.
   */
  if ((opcode_flag & Instruction::kSwitch) != 0) {
    int offset_to_switch = insns[1] | (((int32_t) insns[2]) << 16);
    const uint16_t* switch_insns = insns + offset_to_switch;
    int switch_count = switch_insns[1];
    int offset_to_targets, targ;

    if ((*insns & 0xff) == Instruction::PACKED_SWITCH) {
      /* 0 = sig, 1 = count, 2/3 = first key */
      offset_to_targets = 4;
    } else {
      /* 0 = sig, 1 = count, 2..count * 2 = keys */
      DCHECK((*insns & 0xff) == Instruction::SPARSE_SWITCH);
      offset_to_targets = 2 + 2 * switch_count;
    }

    /* verify each switch target */
    for (targ = 0; targ < switch_count; targ++) {
      int offset;
      uint32_t abs_offset;

      /* offsets are 32-bit, and only partly endian-swapped */
      offset = switch_insns[offset_to_targets + targ * 2] |
         (((int32_t) switch_insns[offset_to_targets + targ * 2 + 1]) << 16);
      abs_offset = work_insn_idx_ + offset;
      DCHECK_LT(abs_offset, code_item_->insns_size_in_code_units_);
      if (!CheckMoveException(code_item_->insns_, abs_offset)) {
        return false;
      }
      if (!UpdateRegisters(abs_offset, work_line_.get()))
        return false;
    }
  }

  /*
   * Handle instructions that can throw and that are sitting in a "try" block. (If they're not in a
   * "try" block when they throw, control transfers out of the method.)
   */
  if ((opcode_flag & Instruction::kThrow) != 0 && insn_flags_[work_insn_idx_].IsInTry()) {
    bool within_catch_all = false;
    DexFile::CatchHandlerIterator iterator =
        DexFile::dexFindCatchHandler(*code_item_, work_insn_idx_);

    for (; !iterator.HasNext(); iterator.Next()) {
      if (iterator.Get().type_idx_ == DexFile::kDexNoIndex) {
        within_catch_all = true;
      }
      /*
       * Merge registers into the "catch" block. We want to use the "savedRegs" rather than
       * "work_regs", because at runtime the exception will be thrown before the instruction
       * modifies any registers.
       */
      if (!UpdateRegisters(iterator.Get().address_, saved_line_.get())) {
        return false;
      }
    }

    /*
     * If the monitor stack depth is nonzero, there must be a "catch all" handler for this
     * instruction. This does apply to monitor-exit because of async exception handling.
     */
    if (work_line_->MonitorStackDepth() > 0 && !within_catch_all) {
      /*
       * The state in work_line reflects the post-execution state. If the current instruction is a
       * monitor-enter and the monitor stack was empty, we don't need a catch-all (if it throws,
       * it will do so before grabbing the lock).
       */
      if (dec_insn.opcode_ != Instruction::MONITOR_ENTER || work_line_->MonitorStackDepth() != 1) {
        Fail(VERIFY_ERROR_GENERIC)
            << "expected to be within a catch-all for an instruction where a monitor is held";
        return false;
      }
    }
  }

  /* If we're returning from the method, make sure monitor stack is empty. */
  if ((opcode_flag & Instruction::kReturn) != 0) {
    if(!work_line_->VerifyMonitorStackEmpty()) {
      return false;
    }
  }

  /*
   * Update start_guess. Advance to the next instruction of that's
   * possible, otherwise use the branch target if one was found. If
   * neither of those exists we're in a return or throw; leave start_guess
   * alone and let the caller sort it out.
   */
  if ((opcode_flag & Instruction::kContinue) != 0) {
    *start_guess = work_insn_idx_ + insn_flags_[work_insn_idx_].GetLengthInCodeUnits();
  } else if ((opcode_flag & Instruction::kBranch) != 0) {
    /* we're still okay if branch_target is zero */
    *start_guess = work_insn_idx_ + branch_target;
  }

  DCHECK_LT(*start_guess, code_item_->insns_size_in_code_units_);
  DCHECK(insn_flags_[*start_guess].IsOpcode());

  return true;
}

Class* DexVerifier::ResolveClassAndCheckAccess(uint32_t class_idx) {
  const Class* referrer = method_->GetDeclaringClass();
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Class* res_class = class_linker->ResolveType(*dex_file_, class_idx, referrer);

  if (res_class == NULL) {
    Thread::Current()->ClearException();
    Fail(VERIFY_ERROR_NO_CLASS) << "can't find class with index " << (void*) class_idx;
  } else if (!referrer->CanAccess(res_class)) {   /* Check if access is allowed. */
    Fail(VERIFY_ERROR_ACCESS_CLASS) << "illegal class access: "
                                    << referrer->GetDescriptor()->ToModifiedUtf8() << " -> "
                                    << res_class->GetDescriptor()->ToModifiedUtf8();
  }
  return res_class;
}

Class* DexVerifier::GetCaughtExceptionType() {
  Class* common_super = NULL;
  if (code_item_->tries_size_ != 0) {
    const byte* handlers_ptr = DexFile::dexGetCatchHandlerData(*code_item_, 0);
    uint32_t handlers_size = DecodeUnsignedLeb128(&handlers_ptr);
    for (uint32_t i = 0; i < handlers_size; i++) {
      DexFile::CatchHandlerIterator iterator(handlers_ptr);
      for (; !iterator.HasNext(); iterator.Next()) {
        DexFile::CatchHandlerItem handler = iterator.Get();
        if (handler.address_ == (uint32_t) work_insn_idx_) {
          if (handler.type_idx_ == DexFile::kDexNoIndex) {
            common_super = JavaLangThrowable();
          } else {
            Class* klass = ResolveClassAndCheckAccess(handler.type_idx_);
            /* TODO: on error do we want to keep going?  If we don't fail this we run the risk of
             * having a non-Throwable introduced at runtime. However, that won't pass an instanceof
             * test, so is essentially harmless.
             */
            if (klass == NULL) {
              Fail(VERIFY_ERROR_GENERIC) << "unable to resolve exception class "
                                         << handler.type_idx_ << " ("
                                         << dex_file_->dexStringByTypeIdx(handler.type_idx_) << ")";
              return NULL;
            } else if(!JavaLangThrowable()->IsAssignableFrom(klass)) {
              Fail(VERIFY_ERROR_GENERIC) << "unexpected non-exception class " << PrettyClass(klass);
              return NULL;
            } else if (common_super == NULL) {
              common_super = klass;
            } else {
              common_super = RegType::ClassJoin(common_super, klass);
            }
          }
        }
      }
      handlers_ptr = iterator.GetData();
    }
  }
  if (common_super == NULL) {
    /* no catch blocks, or no catches with classes we can find */
    Fail(VERIFY_ERROR_GENERIC) << "unable to find exception handler";
  }
  return common_super;
}

Method* DexVerifier::ResolveMethodAndCheckAccess(uint32_t method_idx, bool is_direct) {
  Class* referrer = method_->GetDeclaringClass();
  DexCache* dex_cache = referrer->GetDexCache();
  Method* res_method = dex_cache->GetResolvedMethod(method_idx);
  if (res_method == NULL) {
    const DexFile::MethodId& method_id = dex_file_->GetMethodId(method_idx);
    Class* klass = ResolveClassAndCheckAccess(method_id.class_idx_);
    if (klass == NULL) {
      DCHECK(failure_ != VERIFY_ERROR_NONE);
      return NULL;
    }
    const char* name = dex_file_->GetMethodName(method_id);
    std::string signature(dex_file_->CreateMethodDescriptor(method_id.proto_idx_, NULL));
    if (is_direct) {
      res_method = klass->FindDirectMethod(name, signature);
    } else if (klass->IsInterface()) {
      res_method = klass->FindInterfaceMethod(name, signature);
    } else {
      res_method = klass->FindVirtualMethod(name, signature);
    }
    if (res_method != NULL) {
      dex_cache->SetResolvedMethod(method_idx, res_method);
    } else {
      Fail(VERIFY_ERROR_NO_METHOD) << "couldn't find method "
                                   << PrettyDescriptor(klass->GetDescriptor()) << "." << name
                                   << " " << signature;
      return NULL;
    }
  }
  /* Check if access is allowed. */
  if (!referrer->CanAccessMember(res_method->GetDeclaringClass(), res_method->GetAccessFlags())) {
    Fail(VERIFY_ERROR_ACCESS_METHOD) << "illegal method access (call " << PrettyMethod(res_method)
                                  << " from " << PrettyDescriptor(referrer->GetDescriptor()) << ")";
    return NULL;
  }
  return res_method;
}

Method* DexVerifier::VerifyInvocationArgs(const Instruction::DecodedInstruction& dec_insn,
                                          MethodType method_type, bool is_range, bool is_super) {
  // Resolve the method. This could be an abstract or concrete method depending on what sort of call
  // we're making.
  Method* res_method = ResolveMethodAndCheckAccess(dec_insn.vB_,
                           (method_type == METHOD_DIRECT || method_type == METHOD_STATIC));
  if (res_method == NULL) {
    const DexFile::MethodId& method_id = dex_file_->GetMethodId(dec_insn.vB_);
    const char* method_name = dex_file_->GetMethodName(method_id);
    std::string method_signature = dex_file_->GetMethodSignature(method_id);
    const char* class_descriptor = dex_file_->GetMethodDeclaringClassDescriptor(method_id);
    DCHECK_NE(failure_, VERIFY_ERROR_NONE);
    fail_messages_ << "unable to resolve method " << dec_insn.vB_ << ": "
                   << class_descriptor << "." << method_name << " " << method_signature;
    return NULL;
  }
  // Make sure calls to constructors are "direct". There are additional restrictions but we don't
  // enforce them here.
  if (res_method->IsConstructor() && method_type != METHOD_DIRECT) {
    Fail(VERIFY_ERROR_GENERIC) << "rejecting non-direct call to constructor "
                               << PrettyMethod(res_method);
    return NULL;
  }
  // See if the method type implied by the invoke instruction matches the access flags for the
  // target method.
  if ((method_type == METHOD_DIRECT && !res_method->IsDirect()) ||
      (method_type == METHOD_STATIC && !res_method->IsStatic()) ||
      ((method_type == METHOD_VIRTUAL || method_type == METHOD_INTERFACE) && res_method->IsDirect())
      ) {
    Fail(VERIFY_ERROR_GENERIC) << "invoke type does not match method type of "
                               << PrettyMethod(res_method);
    return NULL;
  }
  // If we're using invoke-super(method), make sure that the executing method's class' superclass
  // has a vtable entry for the target method.
  if (is_super) {
    DCHECK(method_type == METHOD_VIRTUAL);
    Class* super = method_->GetDeclaringClass()->GetSuperClass();
    if (super == NULL || res_method->GetMethodIndex() > super->GetVTable()->GetLength()) {
      if (super == NULL) {  // Only Object has no super class
        Fail(VERIFY_ERROR_NO_METHOD) << "invalid invoke-super from " << PrettyMethod(method_)
                                     << " to super " << PrettyMethod(res_method);
      } else {
        Fail(VERIFY_ERROR_NO_METHOD) << "invalid invoke-super from " << PrettyMethod(method_)
                                     << " to super " << PrettyDescriptor(super->GetDescriptor())
                                     << "." << res_method->GetName()->ToModifiedUtf8()
                                     << " " << res_method->GetSignature()->ToModifiedUtf8();

      }
      return NULL;
    }
  }
  // We use vAA as our expected arg count, rather than res_method->insSize, because we need to
  // match the call to the signature. Also, we might might be calling through an abstract method
  // definition (which doesn't have register count values).
  int expected_args = dec_insn.vA_;
  /* caught by static verifier */
  DCHECK(is_range || expected_args <= 5);
  if (expected_args > code_item_->outs_size_) {
    Fail(VERIFY_ERROR_GENERIC) << "invalid arg count (" << expected_args
        << ") exceeds outsSize (" << code_item_->outs_size_ << ")";
    return NULL;
  }
  std::string sig = res_method->GetSignature()->ToModifiedUtf8();
  if (sig[0] != '(') {
    Fail(VERIFY_ERROR_GENERIC) << "rejecting call to " << res_method
        << " as descriptor doesn't start with '(': " << sig;
    return NULL;
  }
  /*
   * Check the "this" argument, which must be an instance of the class
   * that declared the method. For an interface class, we don't do the
   * full interface merge, so we can't do a rigorous check here (which
   * is okay since we have to do it at runtime).
   */
  int actual_args = 0;
  if (!res_method->IsStatic()) {
    const RegType& actual_arg_type = work_line_->GetInvocationThis(dec_insn);
    if (failure_ != VERIFY_ERROR_NONE) {
      return NULL;
    }
    if (actual_arg_type.IsUninitializedReference() && !res_method->IsConstructor()) {
      Fail(VERIFY_ERROR_GENERIC) << "'this' arg must be initialized";
      return NULL;
    }
    if (method_type != METHOD_INTERFACE && !actual_arg_type.IsZero()) {
      const RegType& res_method_class = reg_types_.FromClass(res_method->GetDeclaringClass());
      if (!res_method_class.IsAssignableFrom(actual_arg_type)) {
        Fail(VERIFY_ERROR_GENERIC) << "'this' arg '" << actual_arg_type << "' not instance of '"
            << res_method_class << "'";
        return NULL;
      }
    }
    actual_args++;
  }
  /*
   * Process the target method's signature. This signature may or may not
   * have been verified, so we can't assume it's properly formed.
   */
  size_t sig_offset = 0;
  for (sig_offset = 1; sig_offset < sig.size() && sig[sig_offset] != ')'; sig_offset++) {
    if (actual_args >= expected_args) {
      Fail(VERIFY_ERROR_GENERIC) << "Rejecting invalid call to '" << PrettyMethod(res_method)
          << "'. Expected " << expected_args << " args, found more ("
          << sig.substr(sig_offset) << ")";
      return NULL;
    }
    std::string descriptor;
    if ((sig[sig_offset] == 'L') || (sig[sig_offset] == '[')) {
      size_t end;
      if (sig[sig_offset] == 'L') {
        end = sig.find(';', sig_offset);
      } else {
        for(end = sig_offset + 1; sig[end] == '['; end++) ;
        if (sig[end] == 'L') {
          end = sig.find(';', end);
        }
      }
      if (end == std::string::npos) {
        Fail(VERIFY_ERROR_GENERIC) << "Rejecting invocation of " << PrettyMethod(res_method)
            << "bad signature component '" << sig << "' (missing ';')";
        return NULL;
      }
      descriptor = sig.substr(sig_offset, end - sig_offset + 1);
      sig_offset = end;
    } else {
      descriptor = sig[sig_offset];
    }
    const RegType& reg_type =
        reg_types_.FromDescriptor(method_->GetDeclaringClass()->GetClassLoader(), descriptor);
    uint32_t get_reg = is_range ? dec_insn.vC_ + actual_args : dec_insn.arg_[actual_args];
    if (!work_line_->VerifyRegisterType(get_reg, reg_type)) {
      return NULL;
    }
    actual_args = reg_type.IsLongOrDoubleTypes() ? actual_args + 2 : actual_args + 1;
  }
  if (sig[sig_offset] != ')') {
    Fail(VERIFY_ERROR_GENERIC) << "invocation target: bad signature" << PrettyMethod(res_method);
    return NULL;
  }
  if (actual_args != expected_args) {
    Fail(VERIFY_ERROR_GENERIC) << "Rejecting invocation of " << PrettyMethod(res_method)
        << " expected " << expected_args << " args, found " << actual_args;
    return NULL;
  } else {
    return res_method;
  }
}

void DexVerifier::VerifyAGet(const Instruction::DecodedInstruction& dec_insn,
                             const RegType& insn_type, bool is_primitive) {
  const RegType& index_type = work_line_->GetRegisterType(dec_insn.vC_);
  if (!index_type.IsArrayIndexTypes()) {
    Fail(VERIFY_ERROR_GENERIC) << "Invalid reg type for array index (" << index_type << ")";
  } else {
    Class* array_class = work_line_->GetClassFromRegister(dec_insn.vB_);
    if (failure_ == VERIFY_ERROR_NONE) {
      if (array_class == NULL) {
        // Null array class; this code path will fail at runtime. Infer a merge-able type from the
        // instruction type. TODO: have a proper notion of bottom here.
        if (!is_primitive || insn_type.IsCategory1Types()) {
          // Reference or category 1
          work_line_->SetRegisterType(dec_insn.vA_, reg_types_.Zero());
        } else {
          // Category 2
          work_line_->SetRegisterType(dec_insn.vA_, reg_types_.ConstLo());
        }
      } else {
        /* verify the class */
        Class* component_class = array_class->GetComponentType();
        const RegType& component_type = reg_types_.FromClass(component_class);
        if (!array_class->IsArrayClass()) {
          Fail(VERIFY_ERROR_GENERIC) << "not array type "
              << PrettyDescriptor(array_class->GetDescriptor()) << " with aget";
        } else if (component_class->IsPrimitive() && !is_primitive) {
          Fail(VERIFY_ERROR_GENERIC) << "primitive array type "
                                     << PrettyDescriptor(array_class->GetDescriptor())
                                     << " source for aget-object";
        } else if (!component_class->IsPrimitive() && is_primitive) {
          Fail(VERIFY_ERROR_GENERIC) << "reference array type "
                                     << PrettyDescriptor(array_class->GetDescriptor())
                                     << " source for category 1 aget";
        } else if (is_primitive && !insn_type.Equals(component_type) &&
                   !((insn_type.IsInteger() && component_type.IsFloat()) ||
                     (insn_type.IsLong() && component_type.IsDouble()))) {
          Fail(VERIFY_ERROR_GENERIC) << "array type "
              << PrettyDescriptor(array_class->GetDescriptor())
              << " incompatible with aget of type " << insn_type;
        } else {
          // Use knowledge of the field type which is stronger than the type inferred from the
          // instruction, which can't differentiate object types and ints from floats, longs from
          // doubles.
          work_line_->SetRegisterType(dec_insn.vA_, component_type);
        }
      }
    }
  }
}

void DexVerifier::VerifyAPut(const Instruction::DecodedInstruction& dec_insn,
                             const RegType& insn_type, bool is_primitive) {
  const RegType& index_type = work_line_->GetRegisterType(dec_insn.vC_);
  if (!index_type.IsArrayIndexTypes()) {
    Fail(VERIFY_ERROR_GENERIC) << "Invalid reg type for array index (" << index_type << ")";
  } else {
    Class* array_class = work_line_->GetClassFromRegister(dec_insn.vB_);
    if (failure_ == VERIFY_ERROR_NONE) {
      if (array_class == NULL) {
        // Null array class; this code path will fail at runtime. Infer a merge-able type from the
        // instruction type.
      } else {
        /* verify the class */
        Class* component_class = array_class->GetComponentType();
        const RegType& component_type = reg_types_.FromClass(component_class);
        if (!array_class->IsArrayClass()) {
          Fail(VERIFY_ERROR_GENERIC) << "not array type "
              << PrettyDescriptor(array_class->GetDescriptor()) << " with aput";
        } else if (component_class->IsPrimitive() && !is_primitive) {
          Fail(VERIFY_ERROR_GENERIC) << "primitive array type "
                                     << PrettyDescriptor(array_class->GetDescriptor())
                                     << " source for aput-object";
        } else if (!component_class->IsPrimitive() && is_primitive) {
          Fail(VERIFY_ERROR_GENERIC) << "reference array type "
                                     << PrettyDescriptor(array_class->GetDescriptor())
                                     << " source for category 1 aput";
        } else if (is_primitive && !insn_type.Equals(component_type) &&
                   !((insn_type.IsInteger() && component_type.IsFloat()) ||
                     (insn_type.IsLong() && component_type.IsDouble()))) {
          Fail(VERIFY_ERROR_GENERIC) << "array type "
              << PrettyDescriptor(array_class->GetDescriptor())
              << " incompatible with aput of type " << insn_type;
        } else {
          // The instruction agrees with the type of array, confirm the value to be stored does too
          work_line_->VerifyRegisterType(dec_insn.vA_, component_type);
        }
      }
    }
  }
}

Field* DexVerifier::GetStaticField(int field_idx) {
  Field* field = Runtime::Current()->GetClassLinker()->ResolveField(field_idx, method_, true);
  if (field == NULL) {
    const DexFile::FieldId& field_id = dex_file_->GetFieldId(field_idx);
    Fail(VERIFY_ERROR_NO_FIELD) << "unable to resolve static field " << field_idx << " ("
                                << dex_file_->GetFieldName(field_id) << ") in "
                                << dex_file_->GetFieldDeclaringClassDescriptor(field_id);
    DCHECK(Thread::Current()->IsExceptionPending());
    Thread::Current()->ClearException();
    return NULL;
  } else if (!method_->GetDeclaringClass()->CanAccessMember(field->GetDeclaringClass(),
                                                            field->GetAccessFlags())) {
    Fail(VERIFY_ERROR_ACCESS_FIELD) << "cannot access static field " << PrettyField(field)
                                   << " from " << PrettyClass(method_->GetDeclaringClass());
    return NULL;
  } else if (!field->IsStatic()) {
    Fail(VERIFY_ERROR_CLASS_CHANGE) << "expected field " << PrettyField(field) << " to be static";
    return NULL;
  } else {
    return field;
  }
}

Field* DexVerifier::GetInstanceField(const RegType& obj_type, int field_idx) {
  Field* field = Runtime::Current()->GetClassLinker()->ResolveField(field_idx, method_, false);
  if (field == NULL) {
    const DexFile::FieldId& field_id = dex_file_->GetFieldId(field_idx);
    Fail(VERIFY_ERROR_NO_FIELD) << "unable to resolve instance field " << field_idx << " ("
                                << dex_file_->GetFieldName(field_id) << ") in "
                                << dex_file_->GetFieldDeclaringClassDescriptor(field_id);
    DCHECK(Thread::Current()->IsExceptionPending());
    Thread::Current()->ClearException();
    return NULL;
  } else if (!method_->GetDeclaringClass()->CanAccessMember(field->GetDeclaringClass(),
                                                            field->GetAccessFlags())) {
    Fail(VERIFY_ERROR_ACCESS_FIELD) << "cannot access instance field " << PrettyField(field)
                                    << " from " << PrettyClass(method_->GetDeclaringClass());
    return NULL;
  } else if (field->IsStatic()) {
    Fail(VERIFY_ERROR_CLASS_CHANGE) << "expected field " << PrettyField(field)
                                    << " to not be static";
    return NULL;
  } else if (obj_type.IsZero()) {
    // Cannot infer and check type, however, access will cause null pointer exception
    return field;
  } else if(obj_type.IsUninitializedReference() &&
            (!method_->IsConstructor() || method_->GetDeclaringClass() != obj_type.GetClass() ||
             field->GetDeclaringClass() != method_->GetDeclaringClass())) {
    // Field accesses through uninitialized references are only allowable for constructors where
    // the field is declared in this class
    Fail(VERIFY_ERROR_GENERIC) << "cannot access instance field " << PrettyField(field)
                               << " of a not fully initialized object within the context of "
                               << PrettyMethod(method_);
    return NULL;
  } else if(!field->GetDeclaringClass()->IsAssignableFrom(obj_type.GetClass())) {
    // Trying to access C1.field1 using reference of type C2, which is neither C1 or a sub-class
    // of C1. For resolution to occur the declared class of the field must be compatible with
    // obj_type, we've discovered this wasn't so, so report the field didn't exist.
    Fail(VERIFY_ERROR_NO_FIELD) << "cannot access instance field " << PrettyField(field)
                                << " from object of type " << PrettyClass(obj_type.GetClass());
    return NULL;
  } else {
    return field;
  }
}

void DexVerifier::VerifyISGet(const Instruction::DecodedInstruction& dec_insn,
                              const RegType& insn_type, bool is_primitive, bool is_static) {
  Field* field;
  if (is_static) {
    field = GetStaticField(dec_insn.vB_);
  } else {
    const RegType& object_type = work_line_->GetRegisterType(dec_insn.vB_);
    field = GetInstanceField(object_type, dec_insn.vC_);
  }
  if (field != NULL) {
    const RegType& field_type =
        reg_types_.FromDescriptor(field->GetDeclaringClass()->GetClassLoader(),
                                  field->GetTypeDescriptor());
    if (is_primitive) {
      if (field_type.Equals(insn_type) ||
          (field_type.IsFloat() && insn_type.IsIntegralTypes()) ||
          (field_type.IsDouble() && insn_type.IsLongTypes())) {
        // expected that read is of the correct primitive type or that int reads are reading
        // floats or long reads are reading doubles
      } else {
        // This is a global failure rather than a class change failure as the instructions and
        // the descriptors for the type should have been consistent within the same file at
        // compile time
        Fail(VERIFY_ERROR_GENERIC) << "expected field " << PrettyField(field)
                                   << " to be of type '" << insn_type
                                   << "' but found type '" << field_type << "' in get";
        return;
      }
    } else {
      if (!insn_type.IsAssignableFrom(field_type)) {
        Fail(VERIFY_ERROR_GENERIC) << "expected field " << PrettyField(field)
                                   << " to be compatible with type '" << insn_type
                                   << "' but found type '" << field_type
                                   << "' in get-object";
        return;
      }
    }
    work_line_->SetRegisterType(dec_insn.vA_, field_type);
  }
}

void DexVerifier::VerifyISPut(const Instruction::DecodedInstruction& dec_insn,
                              const RegType& insn_type, bool is_primitive, bool is_static) {
  Field* field;
  if (is_static) {
    field = GetStaticField(dec_insn.vB_);
  } else {
    const RegType& object_type = work_line_->GetRegisterType(dec_insn.vB_);
    field = GetInstanceField(object_type, dec_insn.vC_);
  }
  if (field != NULL) {
    if (field->IsFinal() && field->GetDeclaringClass() != method_->GetDeclaringClass()) {
      Fail(VERIFY_ERROR_ACCESS_FIELD) << "cannot modify final field " << PrettyField(field)
                               << " from other class " << PrettyClass(method_->GetDeclaringClass());
      return;
    }
    const RegType& field_type =
        reg_types_.FromDescriptor(field->GetDeclaringClass()->GetClassLoader(),
                                  field->GetTypeDescriptor());
    if (is_primitive) {
      // Primitive field assignability rules are weaker than regular assignability rules
      bool instruction_compatible;
      bool value_compatible;
      const RegType& value_type = work_line_->GetRegisterType(dec_insn.vA_);
      if (field_type.IsIntegralTypes()) {
        instruction_compatible = insn_type.IsIntegralTypes();
        value_compatible = value_type.IsIntegralTypes();
      } else if (field_type.IsFloat()) {
        instruction_compatible = insn_type.IsInteger();  // no [is]put-float, so expect [is]put-int
        value_compatible = value_type.IsFloatTypes();
      } else if (field_type.IsLong()) {
        instruction_compatible = insn_type.IsLong();
        value_compatible = value_type.IsLongTypes();
      } else if (field_type.IsDouble()) {
        instruction_compatible = insn_type.IsLong();  // no [is]put-double, so expect [is]put-long
        value_compatible = value_type.IsDoubleTypes();
      } else {
        instruction_compatible = false;  // reference field with primitive store
        value_compatible = false;  // unused
      }
      if (!instruction_compatible) {
        // This is a global failure rather than a class change failure as the instructions and
        // the descriptors for the type should have been consistent within the same file at
        // compile time
        Fail(VERIFY_ERROR_GENERIC) << "expected field " << PrettyField(field)
                                   << " to be of type '" << insn_type
                                   << "' but found type '" << field_type
                                   << "' in put";
        return;
      }
      if (!value_compatible) {
        Fail(VERIFY_ERROR_GENERIC) << "unexpected value in v" << dec_insn.vA_
                                   << " of type " << value_type
                                   << " but expected " << field_type
                                   << " for store to " << PrettyField(field) << " in put";
        return;
      }
    } else {
      if (!insn_type.IsAssignableFrom(field_type)) {
        Fail(VERIFY_ERROR_GENERIC) << "expected field " << PrettyField(field)
                                  << " to be compatible with type '" << insn_type
                                  << "' but found type '" << field_type
                                  << "' in put-object";
        return;
      }
      work_line_->VerifyRegisterType(dec_insn.vA_, field_type);
    }
  }
}

bool DexVerifier::CheckMoveException(const uint16_t* insns, int insn_idx) {
  if ((insns[insn_idx] & 0xff) == Instruction::MOVE_EXCEPTION) {
    Fail(VERIFY_ERROR_GENERIC) << "invalid use of move-exception";
    return false;
  }
  return true;
}

void DexVerifier::VerifyFilledNewArrayRegs(const Instruction::DecodedInstruction& dec_insn,
                                           Class* res_class, bool is_range) {
  DCHECK(res_class->IsArrayClass()) << PrettyClass(res_class);  // Checked before calling.
  /*
   * Verify each register. If "arg_count" is bad, VerifyRegisterType() will run off the end of the
   * list and fail. It's legal, if silly, for arg_count to be zero.
   */
  const RegType& expected_type = reg_types_.FromClass(res_class->GetComponentType());
  uint32_t arg_count = dec_insn.vA_;
  for (size_t ui = 0; ui < arg_count; ui++) {
    uint32_t get_reg;

    if (is_range)
      get_reg = dec_insn.vC_ + ui;
    else
      get_reg = dec_insn.arg_[ui];

    if (!work_line_->VerifyRegisterType(get_reg, expected_type)) {
      Fail(VERIFY_ERROR_GENERIC) << "filled-new-array arg " << ui << "(" << get_reg
                                 << ") not valid";
      return;
    }
  }
}

void DexVerifier::ReplaceFailingInstruction() {
  const Instruction* inst = Instruction::At(code_item_->insns_ + work_insn_idx_);
  DCHECK(inst->IsThrow()) << "Expected instruction that will throw " << inst->Name();
  VerifyErrorRefType ref_type;
  switch (inst->Opcode()) {
    case Instruction::CONST_CLASS:            // insn[1] == class ref, 2 code units (4 bytes)
    case Instruction::CHECK_CAST:
    case Instruction::INSTANCE_OF:
    case Instruction::NEW_INSTANCE:
    case Instruction::NEW_ARRAY:
    case Instruction::FILLED_NEW_ARRAY:       // insn[1] == class ref, 3 code units (6 bytes)
    case Instruction::FILLED_NEW_ARRAY_RANGE:
      ref_type = VERIFY_ERROR_REF_CLASS;
      break;
    case Instruction::IGET:                   // insn[1] == field ref, 2 code units (4 bytes)
    case Instruction::IGET_BOOLEAN:
    case Instruction::IGET_BYTE:
    case Instruction::IGET_CHAR:
    case Instruction::IGET_SHORT:
    case Instruction::IGET_WIDE:
    case Instruction::IGET_OBJECT:
    case Instruction::IPUT:
    case Instruction::IPUT_BOOLEAN:
    case Instruction::IPUT_BYTE:
    case Instruction::IPUT_CHAR:
    case Instruction::IPUT_SHORT:
    case Instruction::IPUT_WIDE:
    case Instruction::IPUT_OBJECT:
    case Instruction::SGET:
    case Instruction::SGET_BOOLEAN:
    case Instruction::SGET_BYTE:
    case Instruction::SGET_CHAR:
    case Instruction::SGET_SHORT:
    case Instruction::SGET_WIDE:
    case Instruction::SGET_OBJECT:
    case Instruction::SPUT:
    case Instruction::SPUT_BOOLEAN:
    case Instruction::SPUT_BYTE:
    case Instruction::SPUT_CHAR:
    case Instruction::SPUT_SHORT:
    case Instruction::SPUT_WIDE:
    case Instruction::SPUT_OBJECT:
      ref_type = VERIFY_ERROR_REF_FIELD;
      break;
    case Instruction::INVOKE_VIRTUAL:         // insn[1] == method ref, 3 code units (6 bytes)
    case Instruction::INVOKE_VIRTUAL_RANGE:
    case Instruction::INVOKE_SUPER:
    case Instruction::INVOKE_SUPER_RANGE:
    case Instruction::INVOKE_DIRECT:
    case Instruction::INVOKE_DIRECT_RANGE:
    case Instruction::INVOKE_STATIC:
    case Instruction::INVOKE_STATIC_RANGE:
    case Instruction::INVOKE_INTERFACE:
    case Instruction::INVOKE_INTERFACE_RANGE:
      ref_type = VERIFY_ERROR_REF_METHOD;
      break;
    default:
      LOG(FATAL) << "Error: verifier asked to replace instruction " << inst->DumpString(dex_file_);
      return;
  }
  uint16_t* insns = const_cast<uint16_t*>(code_item_->insns_);
  // THROW_VERIFICATION_ERROR is a 2 code unit instruction. We shouldn't be rewriting a 1 code unit
  // instruction, so assert it.
  size_t width = inst->SizeInCodeUnits();
  CHECK_GT(width, 1u);
  // If the instruction is larger than 2 code units, rewrite subqeuent code unit sized chunks with
  // NOPs
  for (size_t i = 2; i < width; i++) {
    insns[work_insn_idx_ + i] = Instruction::NOP;
  }
  // Encode the opcode, with the failure code in the high byte
  uint16_t new_instruction = Instruction::THROW_VERIFICATION_ERROR |
                             (failure_ << 8) |  // AA - component
                             (ref_type << (8 + kVerifyErrorRefTypeShift));
  insns[work_insn_idx_] = new_instruction;
  // The 2nd code unit (higher in memory) with the reference in, comes from the instruction we
  // rewrote, so nothing to do here.
  LOG(INFO) << "Verification error, replacing instructions in " << PrettyMethod(method_) << " "
            << fail_messages_.str();
  if (gDebugVerify) {
    std::cout << std::endl << info_messages_.str();
    Dump(std::cout);
  }
}

bool DexVerifier::UpdateRegisters(uint32_t next_insn, const RegisterLine* merge_line) {
  const bool merge_debug = true;
  bool changed = true;
  RegisterLine* target_line = reg_table_.GetLine(next_insn);
  if (!insn_flags_[next_insn].IsVisitedOrChanged()) {
    /*
     * We haven't processed this instruction before, and we haven't touched the registers here, so
     * there's nothing to "merge". Copy the registers over and mark it as changed. (This is the
     * only way a register can transition out of "unknown", so this is not just an optimization.)
     */
    target_line->CopyFromLine(merge_line);
  } else {
    UniquePtr<RegisterLine> copy(merge_debug ? new RegisterLine(target_line->NumRegs(), this) : NULL);
    copy->CopyFromLine(target_line);
    changed = target_line->MergeRegisters(merge_line);
    if (failure_ != VERIFY_ERROR_NONE) {
      return false;
    }
    if (gDebugVerify && changed) {
      LogVerifyInfo() << "Merging at [" << (void*)work_insn_idx_ << "] to [" <<(void*)next_insn << "]: " << std::endl
                      << *copy.get() << "  MERGE" << std::endl
                      << *merge_line << "  ==" << std::endl
                      << *target_line << std::endl;
    }
  }
  if (changed) {
    insn_flags_[next_insn].SetChanged();
  }
  return true;
}

void DexVerifier::ComputeGcMapSizes(size_t* gc_points, size_t* ref_bitmap_bits,
                                    size_t* log2_max_gc_pc) {
  size_t local_gc_points = 0;
  size_t max_insn = 0;
  size_t max_ref_reg = -1;
  for (size_t i = 0; i < code_item_->insns_size_in_code_units_; i++) {
    if (insn_flags_[i].IsGcPoint()) {
      local_gc_points++;
      max_insn = i;
      RegisterLine* line = reg_table_.GetLine(i);
      max_ref_reg = line->GetMaxNonZeroReferenceReg(max_ref_reg);
    }
  }
  *gc_points = local_gc_points;
  *ref_bitmap_bits = max_ref_reg + 1;  // if max register is 0 we need 1 bit to encode (ie +1)
  size_t i = 0;
  while ((1U << i) < max_insn) {
    i++;
  }
  *log2_max_gc_pc = i;
}

ByteArray* DexVerifier::GenerateGcMap() {
  size_t num_entries, ref_bitmap_bits, pc_bits;
  ComputeGcMapSizes(&num_entries, &ref_bitmap_bits, &pc_bits);
  // There's a single byte to encode the size of each bitmap
  if (ref_bitmap_bits >= (8 /* bits per byte */ * 256 /* max unsigned byte + 1 */ )) {
    // TODO: either a better GC map format or per method failures
    Fail(VERIFY_ERROR_GENERIC) << "Cannot encode GC map for method with "
       << ref_bitmap_bits << " registers";
    return NULL;
  }
  size_t ref_bitmap_bytes = (ref_bitmap_bits + 7) / 8;
  // There are 2 bytes to encode the number of entries
  if (num_entries >= 65536) {
    // TODO: either a better GC map format or per method failures
    Fail(VERIFY_ERROR_GENERIC) << "Cannot encode GC map for method with "
       << num_entries << " entries";
    return NULL;
  }
  size_t pc_bytes;
  RegisterMapFormat format;
  if (pc_bits < 8) {
    format = kRegMapFormatCompact8;
    pc_bytes = 1;
  } else if (pc_bits < 16) {
    format = kRegMapFormatCompact16;
    pc_bytes = 2;
  } else {
    // TODO: either a better GC map format or per method failures
    Fail(VERIFY_ERROR_GENERIC) << "Cannot encode GC map for method with "
       << (1 << pc_bits) << " instructions (number is rounded up to nearest power of 2)";
    return NULL;
  }
  size_t table_size = ((pc_bytes + ref_bitmap_bytes) * num_entries ) + 4;
  ByteArray* table = ByteArray::Alloc(table_size);
  if (table == NULL) {
    Fail(VERIFY_ERROR_GENERIC) << "Failed to encode GC map (size=" << table_size << ")";
    return NULL;
  }
  // Write table header
  table->Set(0, format);
  table->Set(1, ref_bitmap_bytes);
  table->Set(2, num_entries & 0xFF);
  table->Set(3, (num_entries >> 8) & 0xFF);
  // Write table data
  size_t offset = 4;
  for (size_t i = 0; i < code_item_->insns_size_in_code_units_; i++) {
    if (insn_flags_[i].IsGcPoint()) {
      table->Set(offset, i & 0xFF);
      offset++;
      if (pc_bytes == 2) {
        table->Set(offset, (i >> 8) & 0xFF);
        offset++;
      }
      RegisterLine* line = reg_table_.GetLine(i);
      line->WriteReferenceBitMap(table->GetData() + offset, ref_bitmap_bytes);
      offset += ref_bitmap_bytes;
    }
  }
  DCHECK(offset == table_size);
  return table;
}

void DexVerifier::VerifyGcMap() {
  // Check that for every GC point there is a map entry, there aren't entries for non-GC points,
  // that the table data is well formed and all references are marked (or not) in the bitmap
  PcToReferenceMap map(method_);
  size_t map_index = 0;
  for(size_t i = 0; i < code_item_->insns_size_in_code_units_; i++) {
    const uint8_t* reg_bitmap = map.FindBitMap(i, false);
    if (insn_flags_[i].IsGcPoint()) {
      CHECK_LT(map_index, map.NumEntries());
      CHECK_EQ(map.GetPC(map_index), i);
      CHECK_EQ(map.GetBitMap(map_index), reg_bitmap);
      map_index++;
      RegisterLine* line = reg_table_.GetLine(i);
      for(size_t j = 0; j < code_item_->registers_size_; j++) {
        if (line->GetRegisterType(j).IsNonZeroReferenceTypes()) {
          CHECK_LT(j / 8, map.RegWidth());
          CHECK_EQ((reg_bitmap[j / 8] >> (j % 8)) & 1, 1);
        } else if ((j / 8) < map.RegWidth()) {
          CHECK_EQ((reg_bitmap[j / 8] >> (j % 8)) & 1, 0);
        } else {
          // If a register doesn't contain a reference then the bitmap may be shorter than the line
        }
      }
    } else {
      CHECK(reg_bitmap == NULL);
    }
  }
}

Class* DexVerifier::JavaLangThrowable() {
  if (java_lang_throwable_ == NULL) {
    java_lang_throwable_ =
        Runtime::Current()->GetClassLinker()->FindSystemClass("Ljava/lang/Throwable;");
   DCHECK(java_lang_throwable_ != NULL);
  }
  return java_lang_throwable_;
}

const uint8_t* PcToReferenceMap::FindBitMap(uint16_t dex_pc, bool error_if_not_present) const {
  size_t num_entries = NumEntries();
  // Do linear or binary search?
  static const size_t kSearchThreshold = 8;
  if (num_entries < kSearchThreshold) {
    for (size_t i = 0; i < num_entries; i++)  {
      if (GetPC(i) == dex_pc) {
        return GetBitMap(i);
      }
    }
  } else {
    int lo = 0;
    int hi = num_entries -1;
    while (hi >= lo) {
      int mid = (hi + lo) / 2;
      int mid_pc = GetPC(mid);
      if (dex_pc > mid_pc) {
        lo = mid + 1;
      } else if (dex_pc < mid_pc) {
        hi = mid - 1;
      } else {
        return GetBitMap(mid);
      }
    }
  }
  if (error_if_not_present) {
    LOG(ERROR) << "Didn't find reference bit map for dex_pc " << dex_pc;
  }
  return NULL;
}

}  // namespace verifier
}  // namespace art
