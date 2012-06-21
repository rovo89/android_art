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

#ifndef ART_SRC_SCOPED_THREAD_LIST_LOCK_RELEASER_H_
#define ART_SRC_SCOPED_THREAD_LIST_LOCK_RELEASER_H_

#include "macros.h"

namespace art {

class ScopedThreadListLockReleaser {
 public:
  ScopedThreadListLockReleaser();
  ~ScopedThreadListLockReleaser();

 private:
  // Whether or not we unlocked the thread list lock.
  bool unlocked_;

  DISALLOW_COPY_AND_ASSIGN(ScopedThreadListLockReleaser);
};

}  // namespace art

#endif  // ART_SRC_SCOPED_THREAD_LIST_LOCK_RELEASER_H_
