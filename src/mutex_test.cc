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

#include "mutex.h"

#include "gtest/gtest.h"

namespace art {

TEST(Mutex, LockUnlock) {
  Mutex mu("test mutex");
  mu.AssertDepth(0U);
  mu.Lock();
  mu.AssertDepth(1U);
  mu.Unlock();
  mu.AssertDepth(0U);
}

TEST(Mutex, TryLockUnlock) {
  Mutex mu("test mutex");
  mu.AssertDepth(0U);
  mu.TryLock();
  mu.AssertDepth(1U);
  mu.Unlock();
  mu.AssertDepth(0U);
}

TEST(Mutex, RecursiveLockUnlock) {
  Mutex mu("test mutex");
  mu.AssertDepth(0U);
  mu.Lock();
  mu.AssertDepth(1U);
  mu.Lock();
  mu.AssertDepth(2U);
  mu.Unlock();
  mu.AssertDepth(1U);
  mu.Unlock();
  mu.AssertDepth(0U);
}

TEST(Mutex, RecursiveTryLockUnlock) {
  Mutex mu("test mutex");
  mu.AssertDepth(0U);
  mu.TryLock();
  mu.AssertDepth(1U);
  mu.TryLock();
  mu.AssertDepth(2U);
  mu.Unlock();
  mu.AssertDepth(1U);
  mu.Unlock();
  mu.AssertDepth(0U);
}

}  // namespace art
