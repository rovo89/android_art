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

#include "reference.h"

namespace art {
namespace mirror {

Class* Reference::java_lang_ref_Reference_ = nullptr;

void Reference::SetClass(Class* java_lang_ref_Reference) {
  CHECK(java_lang_ref_Reference_ == nullptr);
  CHECK(java_lang_ref_Reference != nullptr);
  java_lang_ref_Reference_ = java_lang_ref_Reference;
}

void Reference::ResetClass() {
  CHECK(java_lang_ref_Reference_ != nullptr);
  java_lang_ref_Reference_ = nullptr;
}

void Reference::VisitRoots(RootCallback* callback, void* arg) {
  if (java_lang_ref_Reference_ != nullptr) {
    callback(reinterpret_cast<mirror::Object**>(&java_lang_ref_Reference_),
             arg, 0, kRootStickyClass);
  }
}

}  // namespace mirror
}  // namespace art
