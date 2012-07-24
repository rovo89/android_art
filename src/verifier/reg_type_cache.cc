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

const RegType& RegTypeCache::FromDescriptor(ClassLoader* loader, const char* descriptor) {
  return From(RegTypeFromDescriptor(descriptor), loader, descriptor);
}

const RegType& RegTypeCache::From(RegType::Type type, ClassLoader* loader, const char* descriptor) {
  if (type <= RegType::kRegTypeLastFixedLocation) {
    // entries should be sized greater than primitive types
    DCHECK_GT(entries_.size(), static_cast<size_t>(type));
    RegType* entry = entries_[type];
    if (entry == NULL) {
      Class* klass = NULL;
      if (strlen(descriptor) != 0) {
        klass = Runtime::Current()->GetClassLinker()->FindSystemClass(descriptor);
      }
      entry = new RegType(type, klass, 0, type);
      entries_[type] = entry;
    }
    return *entry;
  } else {
    DCHECK(type == RegType::kRegTypeReference);
    ClassHelper kh;
    for (size_t i = RegType::kRegTypeLastFixedLocation + 1; i < entries_.size(); i++) {
      RegType* cur_entry = entries_[i];
      // check resolved and unresolved references, ignore uninitialized references
      if (cur_entry->IsReference()) {
        kh.ChangeClass(cur_entry->GetClass());
        if (strcmp(descriptor, kh.GetDescriptor()) == 0) {
          return *cur_entry;
        }
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
      // TODO: we assume unresolved, but we may be able to do better by validating whether the
      // descriptor string is valid
      // Unable to resolve so create unresolved register type
      DCHECK(Thread::Current()->IsExceptionPending());
      Thread::Current()->ClearException();
      if (IsValidDescriptor(descriptor)) {
        String* string_descriptor =
            Runtime::Current()->GetInternTable()->InternStrong(descriptor);
        RegType* entry = new RegType(RegType::kRegTypeUnresolvedReference, string_descriptor, 0,
                                     entries_.size());
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
    String* descriptor = type.GetDescriptor();
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
    Class* klass = type.GetClass();
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
    String* descriptor = uninit_type.GetDescriptor();
    for (size_t i = RegType::kRegTypeLastFixedLocation + 1; i < entries_.size(); i++) {
      RegType* cur_entry = entries_[i];
      if (cur_entry->IsUnresolvedReference() && cur_entry->GetDescriptor() == descriptor) {
        return *cur_entry;
      }
    }
    entry = new RegType(RegType::kRegTypeUnresolvedReference, descriptor, 0, entries_.size());
  } else {
    Class* klass = uninit_type.GetClass();
    for (size_t i = RegType::kRegTypeLastFixedLocation + 1; i < entries_.size(); i++) {
      RegType* cur_entry = entries_[i];
      if (cur_entry->IsReference() && cur_entry->GetClass() == klass) {
        return *cur_entry;
      }
    }
    entry = new RegType(RegType::kRegTypeReference, klass, 0, entries_.size());
  }
  entries_.push_back(entry);
  return *entry;
}

const RegType& RegTypeCache::UninitializedThisArgument(const RegType& type) {
  // TODO: implement descriptor version.
  RegType* entry;
  if (type.IsUnresolvedTypes()) {
    String* descriptor = type.GetDescriptor();
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
    Class* klass = type.GetClass();
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

const RegType& RegTypeCache::GetComponentType(const RegType& array, ClassLoader* loader) {
  CHECK(array.IsArrayTypes());
  if (array.IsUnresolvedTypes()) {
    std::string descriptor(array.GetDescriptor()->ToModifiedUtf8());
    std::string component(descriptor.substr(1, descriptor.size() - 1));
    return FromDescriptor(loader, component.c_str());
  } else {
    return FromClass(array.GetClass()->GetComponentType());
  }
}

}  // namespace verifier
}  // namespace art
