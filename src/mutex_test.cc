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

struct MutexTester {
  static void AssertDepth(Mutex& mu, uint32_t expected_depth) {
    ASSERT_EQ(expected_depth, mu.GetDepth());

    // This test is single-threaded, so we also know _who_ should hold the lock.
    if (expected_depth == 0) {
      mu.AssertNotHeld();
    } else {
      mu.AssertHeld();
    }
  }
};

TEST(Mutex, LockUnlock) {
  Mutex mu("test mutex");
  MutexTester::AssertDepth(mu, 0U);
  mu.Lock();
  MutexTester::AssertDepth(mu, 1U);
  mu.Unlock();
  MutexTester::AssertDepth(mu, 0U);
}

// GCC doesn't get recursive mutexes, so we have to turn off thread safety analysis.
static void TryLockUnlockTest() NO_THREAD_SAFETY_ANALYSIS {
  Mutex mu("test mutex");
  MutexTester::AssertDepth(mu, 0U);
  ASSERT_TRUE(mu.TryLock());
  MutexTester::AssertDepth(mu, 1U);
  mu.Unlock();
  MutexTester::AssertDepth(mu, 0U);
}

TEST(Mutex, TryLockUnlock) {
  TryLockUnlockTest();
}

// GCC doesn't get recursive mutexes, so we have to turn off thread safety analysis.
static void RecursiveLockUnlockTest() NO_THREAD_SAFETY_ANALYSIS {
  Mutex mu("test mutex");
  MutexTester::AssertDepth(mu, 0U);
  mu.Lock();
  MutexTester::AssertDepth(mu, 1U);
  mu.Lock();
  MutexTester::AssertDepth(mu, 2U);
  mu.Unlock();
  MutexTester::AssertDepth(mu, 1U);
  mu.Unlock();
  MutexTester::AssertDepth(mu, 0U);
}

TEST(Mutex, RecursiveLockUnlock) {
  RecursiveLockUnlockTest();
}

// GCC doesn't get recursive mutexes, so we have to turn off thread safety analysis.
static void RecursiveTryLockUnlockTest() NO_THREAD_SAFETY_ANALYSIS {
  Mutex mu("test mutex");
  MutexTester::AssertDepth(mu, 0U);
  ASSERT_TRUE(mu.TryLock());
  MutexTester::AssertDepth(mu, 1U);
  ASSERT_TRUE(mu.TryLock());
  MutexTester::AssertDepth(mu, 2U);
  mu.Unlock();
  MutexTester::AssertDepth(mu, 1U);
  mu.Unlock();
  MutexTester::AssertDepth(mu, 0U);
}

TEST(Mutex, RecursiveTryLockUnlock) {
  RecursiveTryLockUnlockTest();
}

}  // namespace art
