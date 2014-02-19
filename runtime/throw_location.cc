/*
 * Copyright (C) 2013 The Android Open Source Project
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

#include "throw_location.h"

#include "mirror/art_method-inl.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "object_utils.h"
#include "utils.h"

namespace art {

std::string ThrowLocation::Dump() const {
  if (method_ != nullptr) {
    return StringPrintf("%s:%d", PrettyMethod(method_).c_str(),
                        MethodHelper(method_).GetLineNumFromDexPC(dex_pc_));
  } else {
    return "unknown throw location";
  }
}

void ThrowLocation::VisitRoots(RootCallback* visitor, void* arg) {
  if (this_object_ != nullptr) {
    visitor(&this_object_, arg, 0, kRootVMInternal);
    DCHECK(this_object_ != nullptr);
  }
  if (method_ != nullptr) {
    visitor(reinterpret_cast<mirror::Object**>(&method_), arg, 0, kRootVMInternal);
    DCHECK(method_ != nullptr);
  }
}

}  // namespace art
