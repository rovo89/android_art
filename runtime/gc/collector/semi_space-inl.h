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

#ifndef ART_RUNTIME_GC_COLLECTOR_SEMI_SPACE_INL_H_
#define ART_RUNTIME_GC_COLLECTOR_SEMI_SPACE_INL_H_

namespace art {
namespace gc {
namespace collector {

inline mirror::Object* SemiSpace::GetForwardingAddressInFromSpace(mirror::Object* obj) const {
  DCHECK(from_space_->HasAddress(obj));
  LockWord lock_word = obj->GetLockWord();
  if (lock_word.GetState() != LockWord::kForwardingAddress) {
    return nullptr;
  }
  return reinterpret_cast<mirror::Object*>(lock_word.ForwardingAddress());
}

}  // namespace collector
}  // namespace gc
}  // namespace art

#endif  // ART_RUNTIME_GC_COLLECTOR_SEMI_SPACE_INL_H_
