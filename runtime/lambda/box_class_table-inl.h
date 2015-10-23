/*
 * Copyright (C) 2015 The Android Open Source Project
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

#ifndef ART_RUNTIME_LAMBDA_BOX_CLASS_TABLE_INL_H_
#define ART_RUNTIME_LAMBDA_BOX_CLASS_TABLE_INL_H_

#include "lambda/box_class_table.h"
#include "thread.h"

namespace art {
namespace lambda {

template <typename Visitor>
inline void BoxClassTable::VisitRoots(const Visitor& visitor) {
  MutexLock mu(Thread::Current(), *Locks::lambda_class_table_lock_);
  for (std::pair<UnorderedMapKeyType, ValueType>& key_value : map_) {
    ValueType& gc_root = key_value.second;
    visitor.VisitRoot(gc_root.AddressWithoutBarrier());
  }
}

}  // namespace lambda
}  // namespace art

#endif  // ART_RUNTIME_LAMBDA_BOX_CLASS_TABLE_INL_H_
