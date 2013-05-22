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

#ifndef ART_SRC_GC_MOD_UNION_TABLE_INL_H_
#define ART_SRC_GC_MOD_UNION_TABLE_INL_H_

#include "mod_union_table.h"

namespace art {

template <typename Implementation>
class ModUnionTableToZygoteAllocspace : public Implementation {
public:
  ModUnionTableToZygoteAllocspace(Heap* heap) : Implementation(heap) {
  }

  bool AddReference(const mirror::Object* /* obj */, const mirror::Object* ref) {
    const Spaces& spaces = Implementation::GetHeap()->GetSpaces();
    for (Spaces::const_iterator it = spaces.begin(); it != spaces.end(); ++it) {
      if ((*it)->Contains(ref)) {
        return (*it)->IsAllocSpace();
      }
    }
    // Assume it points to a large object.
    // TODO: Check.
    return true;
  }
};

template <typename Implementation>
class ModUnionTableToAllocspace : public Implementation {
public:
  ModUnionTableToAllocspace(Heap* heap) : Implementation(heap) {
  }

  bool AddReference(const mirror::Object* /* obj */, const mirror::Object* ref) {
    const Spaces& spaces = Implementation::GetHeap()->GetSpaces();
    for (Spaces::const_iterator it = spaces.begin(); it != spaces.end(); ++it) {
      if ((*it)->Contains(ref)) {
        return (*it)->GetGcRetentionPolicy() == kGcRetentionPolicyAlwaysCollect;
      }
    }
    // Assume it points to a large object.
    // TODO: Check.
    return true;
  }
};

}  // namespace art

#endif  // ART_SRC_GC_MOD_UNION_TABLE_INL_H_
