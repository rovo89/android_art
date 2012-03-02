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

  const char* GetName() {
    return name_.c_str();
  }

  pthread_mutex_t* GetImpl() {
    return &mutex_;
  }

  void AssertHeld() {
#if !defined(__APPLE__)
    DCHECK_EQ(GetOwner(), GetTid());
#endif
  }

  void AssertNotHeld() {
#if !defined(__APPLE__)
    DCHECK_NE(GetOwner(), GetTid());
#endif
  }

  pid_t GetOwner();

  void AssertDepth(uint32_t depth) {
#if !defined(__APPLE__)
    DCHECK_EQ(depth, GetDepth());
#endif
  }

 private:
  static pid_t GetTid();

  uint32_t GetDepth();

  pthread_mutex_t mutex_;
  std::string name_;
  MutexRank rank_;

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
