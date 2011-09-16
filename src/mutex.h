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
#include <string>

#include "logging.h"
#include "macros.h"

namespace art {

class Mutex {
 public:
  explicit Mutex(const char* name);
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
#ifdef __BIONIC__
    DCHECK_EQ(GetOwner(), GetTid());
#endif
  }

  void AssertNotHeld() {
#ifdef __BIONIC__
    DCHECK_NE(GetOwner(), GetTid());
#endif
  }

 private:
  pid_t GetOwner();
  pid_t GetTid();

  std::string name_;

  pthread_mutex_t mutex_;

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
  ConditionVariable(const std::string& name);
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
