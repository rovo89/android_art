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

#include "reg_type.h"

#include "object_utils.h"
#include "reg_type_cache.h"

namespace art {
namespace verifier {

static const char* type_strings[] = {
    "Undefined",
    "Conflict",
    "boolean",
    "byte",
    "short",
    "char",
    "int",
    "float",
    "long (Low Half)",
    "long (High Half)",
    "double (Low Half)",
    "double (High Half)",
    "Precise 32-bit Constant",
    "Imprecise 32-bit Constant",
    "Precise 64-bit Constant (Low Half)",
    "Precise 64-bit Constant (High Half)",
    "Imprecise 64-bit Constant (Low Half)",
    "Imprecise 64-bit Constant (High Half)",
    "Unresolved Reference",
    "Uninitialized Reference",
    "Uninitialized This Reference",
    "Unresolved And Uninitialized Reference",
    "Unresolved And Uninitialized This Reference",
    "Unresolved Merged References",
    "Unresolved Super Class",
    "Reference",
    "Precise Reference",
};

std::string RegType::Dump(const RegTypeCache* reg_types) const {
  DCHECK(type_ >=  kRegTypeUndefined && type_ <= kRegTypePreciseReference);
  DCHECK(arraysize(type_strings) == (kRegTypePreciseReference + 1));
  std::string result;
  if (IsUnresolvedMergedReference()) {
    if (reg_types == NULL) {
      std::pair<uint16_t, uint16_t> refs = GetTopMergedTypes();
      result += StringPrintf("UnresolvedMergedReferences(%d, %d)", refs.first, refs.second);
    } else {
      std::set<uint16_t> types = GetMergedTypes(reg_types);
      result += "UnresolvedMergedReferences(";
      typedef std::set<uint16_t>::const_iterator It;  // TODO: C++0x auto
      It it = types.begin();
      result += reg_types->GetFromId(*it).Dump(reg_types);
      for(++it; it != types.end(); ++it) {
        result += ", ";
        result += reg_types->GetFromId(*it).Dump(reg_types);
      }
      result += ")";
    }
  } else if (IsUnresolvedSuperClass()) {
    uint16_t super_type_id = GetUnresolvedSuperClassChildId();
    if (reg_types == NULL) {
      result += StringPrintf("UnresolvedSuperClass(%d)", super_type_id);
    } else {
      result += "UnresolvedSuperClass(";
      result += reg_types->GetFromId(super_type_id).Dump(reg_types);
      result += ")";
    }
  } else if (IsConstant()) {
    uint32_t val = ConstantValue();
    if (val == 0) {
      CHECK(IsPreciseConstant());
      result = "Zero/null";
    } else {
      result = IsPreciseConstant() ? "Precise " : "Imprecise ";
      if (IsConstantShort()) {
        result += StringPrintf("Constant: %d", val);
      } else {
        result += StringPrintf("Constant: 0x%x", val);
      }
    }
  } else if (IsConstantLo()) {
    int32_t val = ConstantValueLo();
    result = IsPreciseConstantLo() ? "Precise " : "Imprecise ";
    if (val >= std::numeric_limits<jshort>::min() &&
        val <= std::numeric_limits<jshort>::max()) {
      result += StringPrintf("Low-half Constant: %d", val);
    } else {
      result += StringPrintf("Low-half Constant: 0x%x", val);
    }
  } else if (IsConstantHi()) {
    int32_t val = ConstantValueHi();
    result = IsPreciseConstantHi() ? "Precise " : "Imprecise ";
    if (val >= std::numeric_limits<jshort>::min() &&
        val <= std::numeric_limits<jshort>::max()) {
      result += StringPrintf("High-half Constant: %d", val);
    } else {
      result += StringPrintf("High-half Constant: 0x%x", val);
    }
  } else {
    result = type_strings[type_];
    if (IsReferenceTypes()) {
      result += ": ";
      if (IsUnresolvedTypes()) {
        result += PrettyDescriptor(GetDescriptor());
      } else {
        result += PrettyDescriptor(GetClass());
      }
    }
  }
  return result;
}

const RegType& RegType::HighHalf(RegTypeCache* cache) const {
  DCHECK(IsLowHalf());
  if (type_ == kRegTypeLongLo) {
    return cache->FromType(kRegTypeLongHi);
  } else if (type_ == kRegTypeDoubleLo) {
    return cache->FromType(kRegTypeDoubleHi);
  } else {
    DCHECK_EQ(type_, kRegTypeImpreciseConstLo);
    return cache->FromType(kRegTypeImpreciseConstHi);
  }
}

std::set<uint16_t> RegType::GetMergedTypes(const RegTypeCache* cache) const {
  std::pair<uint16_t, uint16_t> refs = GetTopMergedTypes();
  const RegType& left = cache->GetFromId(refs.first);
  const RegType& right = cache->GetFromId(refs.second);
  std::set<uint16_t> types;
  if (left.IsUnresolvedMergedReference()) {
    types = left.GetMergedTypes(cache);
  } else {
    types.insert(refs.first);
  }
  if (right.IsUnresolvedMergedReference()) {
    std::set<uint16_t> right_types = right.GetMergedTypes(cache);
    types.insert(right_types.begin(), right_types.end());
  } else {
    types.insert(refs.second);
  }
#ifndef NDEBUG
  typedef std::set<uint16_t>::const_iterator It;  // TODO: C++0x auto
  for(It it = types.begin(); it != types.end(); ++it) {
    CHECK(!cache->GetFromId(*it).IsUnresolvedMergedReference());
  }
#endif
  return types;
}

const RegType& RegType::GetSuperClass(RegTypeCache* cache) const {
  if (!IsUnresolvedTypes()) {
    Class* super_klass = GetClass()->GetSuperClass();
    if (super_klass != NULL) {
      return cache->FromClass(super_klass, IsPreciseReference());
    } else {
      return cache->Zero();
    }
  } else {
    if (!IsUnresolvedMergedReference() && !IsUnresolvedSuperClass() &&
        GetDescriptor()->CharAt(0) == '[') {
      // Super class of all arrays is Object.
      return cache->JavaLangObject(true);
    } else {
      return cache->FromUnresolvedSuperClass(*this);
    }
  }
}

bool RegType::CanAccess(const RegType& other) const {
  if (Equals(other)) {
    return true;  // Trivial accessibility.
  } else {
    bool this_unresolved = IsUnresolvedTypes();
    bool other_unresolved = other.IsUnresolvedTypes();
    if (!this_unresolved && !other_unresolved) {
      return GetClass()->CanAccess(other.GetClass());
    } else if (!other_unresolved) {
      return other.GetClass()->IsPublic();  // Be conservative, only allow if other is public.
    } else {
      return false; // More complicated test not possible on unresolved types, be conservative.
    }
  }
}

bool RegType::CanAccessMember(Class* klass, uint32_t access_flags) const {
  if (access_flags & kAccPublic) {
    return true;
  }
  if (!IsUnresolvedTypes()) {
    return GetClass()->CanAccessMember(klass, access_flags);
  } else {
    return false;  // More complicated test not possible on unresolved types, be conservative.
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
        } else if (IsJavaLangObjectArray()) {
          return src.IsObjectArrayTypes();  // All reference arrays may be assigned to Object[]
        } else if (!IsUnresolvedTypes() && !src.IsUnresolvedTypes() &&
                   GetClass()->IsAssignableFrom(src.GetClass())) {
          // We're assignable from the Class point-of-view
          return true;
        } else {
          // TODO: unresolved types are only assignable for null, Object and equality currently.
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
  if (IsUndefined() && incoming_type.IsUndefined()) {
    return *this;  // Undefined MERGE Undefined => Undefined
  } else if (IsConflict()) {
    return *this;  // Conflict MERGE * => Conflict
  } else if (incoming_type.IsConflict()) {
    return incoming_type;  // * MERGE Conflict => Conflict
  } else if (IsUndefined() || incoming_type.IsUndefined()) {
    return reg_types->Conflict();  // Unknown MERGE * => Conflict
  } else if (IsConstant() && incoming_type.IsConstant()) {
    int32_t val1 = ConstantValue();
    int32_t val2 = incoming_type.ConstantValue();
    if (val1 >= 0 && val2 >= 0) {
      // +ve1 MERGE +ve2 => MAX(+ve1, +ve2)
      if (val1 >= val2) {
        if (!IsPreciseConstant()) {
          return *this;
        } else {
          return reg_types->FromCat1Const(val1, false);
        }
      } else {
        if (!incoming_type.IsPreciseConstant()) {
          return incoming_type;
        } else {
          return reg_types->FromCat1Const(val2, false);
        }
      }
    } else if (val1 < 0 && val2 < 0) {
      // -ve1 MERGE -ve2 => MIN(-ve1, -ve2)
      if (val1 <= val2) {
        if (!IsPreciseConstant()) {
          return *this;
        } else {
          return reg_types->FromCat1Const(val1, false);
        }
      } else {
        if (!incoming_type.IsPreciseConstant()) {
          return incoming_type;
        } else {
          return reg_types->FromCat1Const(val2, false);
        }
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
  } else if (IsConstantLo() && incoming_type.IsConstantLo()) {
    int32_t val1 = ConstantValueLo();
    int32_t val2 = incoming_type.ConstantValueLo();
    return reg_types->FromCat2ConstLo(val1 | val2, false);
  } else if (IsConstantHi() && incoming_type.IsConstantHi()) {
    int32_t val1 = ConstantValueHi();
    int32_t val2 = incoming_type.ConstantValueHi();
    return reg_types->FromCat2ConstHi(val1 | val2, false);
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
      return reg_types->JavaLangObject(false);  // Object MERGE ref => Object
    } else if (IsUnresolvedTypes() || incoming_type.IsUnresolvedTypes()) {
      // We know how to merge an unresolved type with itself, 0 or Object. In this case we
      // have two sub-classes and don't know how to merge. Create a new string-based unresolved
      // type that reflects our lack of knowledge and that allows the rest of the unresolved
      // mechanics to continue.
      return reg_types->FromUnresolvedMerge(*this, incoming_type);
    } else if (IsUninitializedTypes() || incoming_type.IsUninitializedTypes()) {
      // Something that is uninitialized hasn't had its constructor called. Mark any merge
      // of this type with something that is initialized as conflicting. The cases of a merge
      // with itself, 0 or Object are handled above.
      return reg_types->Conflict();
    } else {  // Two reference types, compute Join
      Class* c1 = GetClass();
      Class* c2 = incoming_type.GetClass();
      DCHECK(c1 != NULL && !c1->IsPrimitive());
      DCHECK(c2 != NULL && !c2->IsPrimitive());
      Class* join_class = ClassJoin(c1, c2);
      if (c1 == join_class && !IsPreciseReference()) {
        return *this;
      } else if (c2 == join_class && !incoming_type.IsPreciseReference()) {
        return incoming_type;
      } else {
        return reg_types->FromClass(join_class, false);
      }
    }
  } else {
    return reg_types->Conflict();  // Unexpected types => Conflict
  }
}

// See comment in reg_type.h
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
    ClassLoader* class_loader = s->GetClassLoader();
    std::string descriptor("[");
    descriptor += ClassHelper(common_elem).GetDescriptor();
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

std::ostream& operator<<(std::ostream& os, const RegType& rhs)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  os << rhs.Dump();
  return os;
}

}  // namespace verifier
}  // namespace art
