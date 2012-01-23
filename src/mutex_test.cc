// Copyright 2012 Google Inc. All Rights Reserved.

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
