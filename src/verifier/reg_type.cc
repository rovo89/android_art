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
    "Unresolved And Uninitialized Reference",
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
      if (IsConstantShort()) {
        result = StringPrintf("32-bit Constant: %d", val);
      } else {
        result = StringPrintf("32-bit Constant: 0x%x", val);
      }
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
  } else if (IsConstant() && incoming_type.IsConstant()) {
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

std::ostream& operator<<(std::ostream& os, const RegType& rhs) {
  os << rhs.Dump();
  return os;
}

}  // verifier
}  // art
