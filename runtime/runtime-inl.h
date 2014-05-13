/*
 * Copyright (C) 2014 The Android Open Source Project
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

#ifndef ART_RUNTIME_RUNTIME_INL_H_
#define ART_RUNTIME_RUNTIME_INL_H_

#include "runtime.h"

namespace art {

inline QuickMethodFrameInfo Runtime::GetRuntimeMethodFrameInfo(mirror::ArtMethod* method) const {
  DCHECK(method != nullptr);
  // Cannot be imt-conflict-method or resolution-method.
  DCHECK(method != GetImtConflictMethod());
  DCHECK(method != GetResolutionMethod());
  // Don't use GetCalleeSaveMethod(), some tests don't set all callee save methods.
  if (method == callee_save_methods_[Runtime::kRefsAndArgs]) {
    return GetCalleeSaveMethodFrameInfo(Runtime::kRefsAndArgs);
  } else if (method == callee_save_methods_[Runtime::kSaveAll]) {
    return GetCalleeSaveMethodFrameInfo(Runtime::kSaveAll);
  } else {
    DCHECK(method == callee_save_methods_[Runtime::kRefsOnly]);
    return GetCalleeSaveMethodFrameInfo(Runtime::kRefsOnly);
  }
}

}  // namespace art

#endif  // ART_RUNTIME_RUNTIME_INL_H_
