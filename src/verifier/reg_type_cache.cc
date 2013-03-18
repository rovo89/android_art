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

#include "reg_type_cache.h"

#include "dex_file-inl.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "object_utils.h"

namespace art {
namespace verifier {

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
    default:                      return RegType::kRegTypeConflict;
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
      default:  return RegType::kRegTypeConflict;
    }
  } else if (descriptor[0] == 'L' || descriptor[0] == '[') {
    return RegType::kRegTypeReference;
  } else {
    return RegType::kRegTypeConflict;
  }
}

const RegType& RegTypeCache::FromDescriptor(mirror::ClassLoader* loader, const char* descriptor,
                                            bool precise) {
  return From(RegTypeFromDescriptor(descriptor), loader, descriptor, precise);
}

static bool MatchingPrecisionForClass(RegType* entry, bool precise)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return (entry->IsPreciseReference() == precise) || (entry->GetClass()->IsFinal() && !precise);
}

const RegType& RegTypeCache::From(RegType::Type type, mirror::ClassLoader* loader,
                                  const char* descriptor, bool precise) {
  if (type <= RegType::kRegTypeLastFixedLocation) {
    // entries should be sized greater than primitive types
    DCHECK_GT(entries_.size(), static_cast<size_t>(type));
    RegType* entry = entries_[type];
    if (entry == NULL) {
      mirror::Class* c = NULL;
      if (strlen(descriptor) != 0) {
        c = Runtime::Current()->GetClassLinker()->FindSystemClass(descriptor);
      }
      entry = new RegType(type, c, 0, type);
      entries_[type] = entry;
    }
    return *entry;
  } else {
    DCHECK(type == RegType::kRegTypeReference || type == RegType::kRegTypePreciseReference);
    ClassHelper kh;
    for (size_t i = RegType::kRegTypeLastFixedLocation + 1; i < entries_.size(); i++) {
      RegType* cur_entry = entries_[i];
      // check resolved and unresolved references, ignore uninitialized references
      if (cur_entry->HasClass()) {
        kh.ChangeClass(cur_entry->GetClass());
        if (MatchingPrecisionForClass(cur_entry, precise) &&
            (strcmp(descriptor, kh.GetDescriptor()) == 0)) {
          return *cur_entry;
        }
      } else if (cur_entry->IsUnresolvedReference() &&
                 cur_entry->GetDescriptor() == descriptor) {
        return *cur_entry;
      }
    }
    ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
    mirror::Class* c;
    if (can_load_classes_) {
      c = class_linker->FindClass(descriptor, loader);
    } else {
      c = class_linker->LookupClass(descriptor, loader);
    }
    if (c != NULL) {
      // Able to resolve so create resolved register type that is precise if we
      // know the type is final.
      RegType* entry = new RegType(c->IsFinal() ? RegType::kRegTypePreciseReference
                                                : RegType::kRegTypeReference,
                                   c, 0, entries_.size());
      entries_.push_back(entry);
      return *entry;
    } else {
      // TODO: we assume unresolved, but we may be able to do better by validating whether the
      // descriptor string is valid
      // Unable to resolve so create unresolved register type
      if (can_load_classes_) {
        DCHECK(Thread::Current()->IsExceptionPending());
        Thread::Current()->ClearException();
      } else {
        DCHECK(!Thread::Current()->IsExceptionPending());
      }
      if (IsValidDescriptor(descriptor)) {
        RegType* entry =
            new RegType(RegType::kRegTypeUnresolvedReference, descriptor, 0, entries_.size());
        entries_.push_back(entry);
        return *entry;
      } else {
        // The descriptor is broken return the unknown type as there's nothing sensible that
        // could be done at runtime
        return Conflict();
      }
    }
  }
}

const RegType& RegTypeCache::FromClass(mirror::Class* klass, bool precise) {
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
      if ((cur_entry->HasClass()) &&
          MatchingPrecisionForClass(cur_entry, precise) && cur_entry->GetClass() == klass) {
        return *cur_entry;
      }
    }
    RegType* entry = new RegType(precise ? RegType::kRegTypePreciseReference
                                         : RegType::kRegTypeReference,
                                 klass, 0, entries_.size());
    entries_.push_back(entry);
    return *entry;
  }
}

const RegType& RegTypeCache::FromUnresolvedMerge(const RegType& left, const RegType& right) {
  std::set<uint16_t> types;
  if (left.IsUnresolvedMergedReference()) {
    types = left.GetMergedTypes(this);
  } else {
    types.insert(left.GetId());
  }
  if (right.IsUnresolvedMergedReference()) {
    std::set<uint16_t> right_types = right.GetMergedTypes(this);
    types.insert(right_types.begin(), right_types.end());
  } else {
    types.insert(right.GetId());
  }
  // Check if entry already exists.
  for (size_t i = RegType::kRegTypeLastFixedLocation + 1; i < entries_.size(); i++) {
    RegType* cur_entry = entries_[i];
    if (cur_entry->IsUnresolvedMergedReference()) {
      std::set<uint16_t> cur_entry_types = cur_entry->GetMergedTypes(this);
      if (cur_entry_types == types) {
        return *cur_entry;
      }
    }
  }
  // Create entry.
  uint32_t merged_ids = static_cast<uint32_t>(left.GetId()) << 16 |
                        static_cast<uint32_t>(right.GetId());
  RegType* entry = new RegType(RegType::kRegTypeUnresolvedMergedReference, NULL, merged_ids,
                               entries_.size());
  entries_.push_back(entry);
#ifndef DEBUG
  std::set<uint16_t> check_types = entry->GetMergedTypes(this);
  CHECK(check_types == types);
#endif
  return *entry;
}

const RegType& RegTypeCache::FromUnresolvedSuperClass(const RegType& child) {
  // Check if entry already exists.
   for (size_t i = RegType::kRegTypeLastFixedLocation + 1; i < entries_.size(); i++) {
     RegType* cur_entry = entries_[i];
     if (cur_entry->IsUnresolvedSuperClass()) {
       uint16_t unresolved_super_child_id = cur_entry->GetUnresolvedSuperClassChildId();
       if (unresolved_super_child_id == child.GetId()) {
         return *cur_entry;
       }
     }
   }
   // Create entry.
   RegType* entry = new RegType(RegType::kRegTypeUnresolvedSuperClass, NULL, child.GetId(),
                                entries_.size());
   entries_.push_back(entry);
   return *entry;
}

const RegType& RegTypeCache::Uninitialized(const RegType& type, uint32_t allocation_pc) {
  RegType* entry;
  if (type.IsUnresolvedTypes()) {
    std::string descriptor(type.GetDescriptor());
    for (size_t i = RegType::kRegTypeLastFixedLocation + 1; i < entries_.size(); i++) {
      RegType* cur_entry = entries_[i];
      if (cur_entry->IsUnresolvedAndUninitializedReference() &&
          cur_entry->GetAllocationPc() == allocation_pc &&
          cur_entry->GetDescriptor() == descriptor) {
        return *cur_entry;
      }
    }
    entry = new RegType(RegType::kRegTypeUnresolvedAndUninitializedReference,
                        descriptor, allocation_pc, entries_.size());
  } else {
    mirror::Class* klass = type.GetClass();
    for (size_t i = RegType::kRegTypeLastFixedLocation + 1; i < entries_.size(); i++) {
      RegType* cur_entry = entries_[i];
      if (cur_entry->IsUninitializedReference() &&
          cur_entry->GetAllocationPc() == allocation_pc &&
          cur_entry->GetClass() == klass) {
        return *cur_entry;
      }
    }
    entry = new RegType(RegType::kRegTypeUninitializedReference,
                        klass, allocation_pc, entries_.size());
  }
  entries_.push_back(entry);
  return *entry;
}

const RegType& RegTypeCache::FromUninitialized(const RegType& uninit_type) {
  RegType* entry;
  if (uninit_type.IsUnresolvedTypes()) {
    std::string descriptor(uninit_type.GetDescriptor());
    for (size_t i = RegType::kRegTypeLastFixedLocation + 1; i < entries_.size(); i++) {
      RegType* cur_entry = entries_[i];
      if (cur_entry->IsUnresolvedReference() && cur_entry->GetDescriptor() == descriptor) {
        return *cur_entry;
      }
    }
    entry = new RegType(RegType::kRegTypeUnresolvedReference, descriptor, 0, entries_.size());
  } else {
    mirror::Class* klass = uninit_type.GetClass();
    for (size_t i = RegType::kRegTypeLastFixedLocation + 1; i < entries_.size(); i++) {
      RegType* cur_entry = entries_[i];
      if (cur_entry->IsPreciseReference() && cur_entry->GetClass() == klass) {
        return *cur_entry;
      }
    }
    entry = new RegType(RegType::kRegTypePreciseReference, klass, 0, entries_.size());
  }
  entries_.push_back(entry);
  return *entry;
}

const RegType& RegTypeCache::ByteConstant() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return FromCat1Const(std::numeric_limits<jbyte>::min(), false);
}

const RegType& RegTypeCache::ShortConstant() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return FromCat1Const(std::numeric_limits<jshort>::min(), false);
}

const RegType& RegTypeCache::IntConstant() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return FromCat1Const(std::numeric_limits<jint>::max(), false);
}

const RegType& RegTypeCache::UninitializedThisArgument(const RegType& type) {
  // TODO: implement descriptor version.
  RegType* entry;
  if (type.IsUnresolvedTypes()) {
    std::string descriptor(type.GetDescriptor());
    for (size_t i = RegType::kRegTypeLastFixedLocation + 1; i < entries_.size(); i++) {
      RegType* cur_entry = entries_[i];
      if (cur_entry->IsUnresolvedAndUninitializedThisReference() &&
          cur_entry->GetDescriptor() == descriptor) {
        return *cur_entry;
      }
    }
    entry = new RegType(RegType::kRegTypeUnresolvedAndUninitializedThisReference, descriptor, 0,
                        entries_.size());
  } else {
    mirror::Class* klass = type.GetClass();
    for (size_t i = RegType::kRegTypeLastFixedLocation + 1; i < entries_.size(); i++) {
      RegType* cur_entry = entries_[i];
      if (cur_entry->IsUninitializedThisReference() && cur_entry->GetClass() == klass) {
        return *cur_entry;
      }
    }
    entry = new RegType(RegType::kRegTypeUninitializedThisReference, klass, 0, entries_.size());
  }
  entries_.push_back(entry);
  return *entry;
}

const RegType& RegTypeCache::FromType(RegType::Type type) {
  CHECK(type < RegType::kRegTypeReference);
  switch (type) {
    case RegType::kRegTypeBoolean:  return From(type, NULL, "Z", true);
    case RegType::kRegTypeByte:     return From(type, NULL, "B", true);
    case RegType::kRegTypeShort:    return From(type, NULL, "S", true);
    case RegType::kRegTypeChar:     return From(type, NULL, "C", true);
    case RegType::kRegTypeInteger:  return From(type, NULL, "I", true);
    case RegType::kRegTypeFloat:    return From(type, NULL, "F", true);
    case RegType::kRegTypeLongLo:
    case RegType::kRegTypeLongHi:   return From(type, NULL, "J", true);
    case RegType::kRegTypeDoubleLo:
    case RegType::kRegTypeDoubleHi: return From(type, NULL, "D", true);
    default:                        return From(type, NULL, "", true);
  }
}

const RegType& RegTypeCache::FromCat1Const(int32_t value, bool precise) {
  RegType::Type wanted_type =
      precise ? RegType::kRegTypePreciseConst : RegType::kRegTypeImpreciseConst;
  for (size_t i = RegType::kRegTypeLastFixedLocation + 1; i < entries_.size(); i++) {
    RegType* cur_entry = entries_[i];
    if (cur_entry->GetType() == wanted_type && cur_entry->ConstantValue() == value) {
      return *cur_entry;
    }
  }
  RegType* entry = new RegType(wanted_type, NULL, value, entries_.size());
  entries_.push_back(entry);
  return *entry;
}

const RegType& RegTypeCache::FromCat2ConstLo(int32_t value, bool precise) {
  RegType::Type wanted_type =
      precise ? RegType::kRegTypePreciseConstLo : RegType::kRegTypeImpreciseConstLo;
  for (size_t i = RegType::kRegTypeLastFixedLocation + 1; i < entries_.size(); i++) {
    RegType* cur_entry = entries_[i];
    if (cur_entry->GetType() == wanted_type && cur_entry->ConstantValueLo() == value) {
      return *cur_entry;
    }
  }
  RegType* entry = new RegType(wanted_type, NULL, value, entries_.size());
  entries_.push_back(entry);
  return *entry;
}

const RegType& RegTypeCache::FromCat2ConstHi(int32_t value, bool precise) {
  RegType::Type wanted_type =
      precise ? RegType::kRegTypePreciseConstHi : RegType::kRegTypeImpreciseConstHi;
  for (size_t i = RegType::kRegTypeLastFixedLocation + 1; i < entries_.size(); i++) {
    RegType* cur_entry = entries_[i];
    if (cur_entry->GetType() == wanted_type && cur_entry->ConstantValueHi() == value) {
      return *cur_entry;
    }
  }
  RegType* entry = new RegType(wanted_type, NULL, value, entries_.size());
  entries_.push_back(entry);
  return *entry;
}

const RegType& RegTypeCache::GetComponentType(const RegType& array, mirror::ClassLoader* loader) {
  CHECK(array.IsArrayTypes());
  if (array.IsUnresolvedTypes()) {
    std::string descriptor(array.GetDescriptor());
    std::string component(descriptor.substr(1, descriptor.size() - 1));
    return FromDescriptor(loader, component.c_str(), false);
  } else {
    mirror::Class* klass = array.GetClass()->GetComponentType();
    return FromClass(klass, klass->IsFinal());
  }
}

void RegTypeCache::Dump(std::ostream& os) {
  for (size_t i = 0; i < entries_.size(); i++) {
    RegType* cur_entry = entries_[i];
    if (cur_entry != NULL) {
      os << i << ": " << cur_entry->Dump() << "\n";
    }
  }
}

}  // namespace verifier
}  // namespace art
