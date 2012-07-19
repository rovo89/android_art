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

#include "common_test.h"

namespace art {

class MutexTest : public CommonTest {};

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

TEST_F(MutexTest, LockUnlock) {
  Mutex mu("test mutex");
  MutexTester::AssertDepth(mu, 0U);
  mu.Lock();
  MutexTester::AssertDepth(mu, 1U);
  mu.Unlock();
  MutexTester::AssertDepth(mu, 0U);
}

// GCC has trouble with our mutex tests, so we have to turn off thread safety analysis.
static void TryLockUnlockTest() NO_THREAD_SAFETY_ANALYSIS {
  Mutex mu("test mutex");
  MutexTester::AssertDepth(mu, 0U);
  ASSERT_TRUE(mu.TryLock());
  MutexTester::AssertDepth(mu, 1U);
  mu.Unlock();
  MutexTester::AssertDepth(mu, 0U);
}

TEST_F(MutexTest, TryLockUnlock) {
  TryLockUnlockTest();
}

// GCC has trouble with our mutex tests, so we have to turn off thread safety analysis.
static void RecursiveLockUnlockTest() NO_THREAD_SAFETY_ANALYSIS {
  Mutex mu("test mutex", kDefaultMutexLevel, true);
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

TEST_F(MutexTest, RecursiveLockUnlock) {
  RecursiveLockUnlockTest();
}

// GCC has trouble with our mutex tests, so we have to turn off thread safety analysis.
static void RecursiveTryLockUnlockTest() NO_THREAD_SAFETY_ANALYSIS {
  Mutex mu("test mutex", kDefaultMutexLevel, true);
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

TEST_F(MutexTest, RecursiveTryLockUnlock) {
  RecursiveTryLockUnlockTest();
}


struct RecursiveLockWait {
  explicit RecursiveLockWait()
      : mu("test mutex", kDefaultMutexLevel, true), cv("test condition variable") {
  }

  static void* Callback(void* arg) {
    RecursiveLockWait* state = reinterpret_cast<RecursiveLockWait*>(arg);
    state->mu.Lock();
    state->cv.Signal();
    state->mu.Unlock();
    return NULL;
  }

  Mutex mu;
  ConditionVariable cv;
};

// GCC has trouble with our mutex tests, so we have to turn off thread safety analysis.
static void RecursiveLockWaitTest() NO_THREAD_SAFETY_ANALYSIS {
  RecursiveLockWait state;
  state.mu.Lock();
  state.mu.Lock();

  pthread_t pthread;
  int pthread_create_result = pthread_create(&pthread, NULL, RecursiveLockWait::Callback, &state);
  ASSERT_EQ(0, pthread_create_result);

  state.cv.Wait(state.mu);

  state.mu.Unlock();
  state.mu.Unlock();
}

// This ensures we don't hang when waiting on a recursively locked mutex,
// which is not supported with bare pthread_mutex_t.
TEST_F(MutexTest, RecursiveLockWait) {
  RecursiveLockWaitTest();
}

TEST_F(MutexTest, SharedLockUnlock) {
  ReaderWriterMutex mu("test rwmutex");
  mu.AssertNotHeld();
  mu.SharedLock();
  mu.AssertSharedHeld();
  mu.AssertNotExclusiveHeld();
  mu.SharedUnlock();
  mu.AssertNotHeld();
}

TEST_F(MutexTest, ExclusiveLockUnlock) {
  ReaderWriterMutex mu("test rwmutex");
  mu.AssertNotHeld();
  mu.ExclusiveLock();
  mu.AssertSharedHeld();
  mu.AssertExclusiveHeld();
  mu.ExclusiveUnlock();
  mu.AssertNotHeld();
}

// GCC has trouble with our mutex tests, so we have to turn off thread safety analysis.
static void SharedTryLockUnlockTest() NO_THREAD_SAFETY_ANALYSIS {
  ReaderWriterMutex mu("test rwmutex");
  mu.AssertNotHeld();
  ASSERT_TRUE(mu.SharedTryLock());
  mu.AssertSharedHeld();
  mu.SharedUnlock();
  mu.AssertNotHeld();
}

TEST_F(MutexTest, SharedTryLockUnlock) {
  SharedTryLockUnlockTest();
}

}  // namespace art
