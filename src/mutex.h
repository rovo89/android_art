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

#ifndef ART_SRC_MUTEX_H_
#define ART_SRC_MUTEX_H_

#include <pthread.h>
#include <stdint.h>

#include <iosfwd>
#include <string>

#include "gtest/gtest.h"
#include "logging.h"
#include "macros.h"

namespace art {

enum MutexRank {
  kNoMutexRank = -1,
  kHeapLock = 0,
  kThreadListLock = 1,
  kThreadSuspendCountLock = 2,
  kMaxMutexRank = kThreadSuspendCountLock,
};
std::ostream& operator<<(std::ostream& os, const MutexRank& rhs);

class Mutex {
 public:
  explicit Mutex(const char* name, MutexRank rank = kNoMutexRank);
  ~Mutex();

  void Lock();

  bool TryLock();

  void Unlock();

#if !defined(NDEBUG)
  void AssertHeld();
  void AssertNotHeld();
#else
  void AssertHeld() {}
  void AssertNotHeld() {}
#endif

  uint64_t GetOwner();

 private:
  uint32_t GetDepth();

  pthread_mutex_t mutex_;
  const std::string name_;
  const MutexRank rank_;

  friend class ConditionVariable;
  friend class MutexTester;
  DISALLOW_COPY_AND_ASSIGN(Mutex);
};

class MutexLock {
 public:
  explicit MutexLock(Mutex& mu) : mu_(mu) {
    mu_.Lock();
  }

  ~MutexLock() {
    mu_.Unlock();
  }

 private:
  Mutex& mu_;
  DISALLOW_COPY_AND_ASSIGN(MutexLock);
};

class ConditionVariable {
 public:
  explicit ConditionVariable(const std::string& name);
  ~ConditionVariable();

  void Broadcast();
  void Signal();
  void Wait(Mutex& mutex);
  void TimedWait(Mutex& mutex, const timespec& ts);

 private:
  pthread_cond_t cond_;
  std::string name_;
  DISALLOW_COPY_AND_ASSIGN(ConditionVariable);
};

}  // namespace art

#endif  // ART_SRC_MUTEX_H_
