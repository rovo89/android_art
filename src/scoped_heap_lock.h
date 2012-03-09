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

#ifndef ART_SRC_SCOPED_HEAP_LOCK_H_
#define ART_SRC_SCOPED_HEAP_LOCK_H_

#include "heap.h"
#include "macros.h"
#include "runtime.h"

namespace art {

class ScopedHeapLock {
 public:
  ScopedHeapLock() {
    Runtime::Current()->GetHeap()->Lock();
  }

  ~ScopedHeapLock() {
    Runtime::Current()->GetHeap()->Unlock();
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(ScopedHeapLock);
};

}  // namespace art

#endif  // ART_SRC_SCOPED_HEAP_LOCK_H_
