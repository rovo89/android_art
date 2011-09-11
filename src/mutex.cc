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

#include "mutex.h"

#include <errno.h>

#include "logging.h"
#include "utils.h"

namespace art {

Mutex::Mutex(const char* name) : name_(name) {
#ifndef NDEBUG
  pthread_mutexattr_t debug_attributes;
  errno = pthread_mutexattr_init(&debug_attributes);
  if (errno != 0) {
    PLOG(FATAL) << "pthread_mutexattr_init failed";
  }
#if VERIFY_OBJECT_ENABLED
  errno = pthread_mutexattr_settype(&debug_attributes, PTHREAD_MUTEX_RECURSIVE);
#else
  errno = pthread_mutexattr_settype(&debug_attributes, PTHREAD_MUTEX_ERRORCHECK);
#endif
  if (errno != 0) {
    PLOG(FATAL) << "pthread_mutexattr_settype failed";
  }
  errno = pthread_mutex_init(&mutex_, &debug_attributes);
  if (errno != 0) {
    PLOG(FATAL) << "pthread_mutex_init failed";
  }
  errno = pthread_mutexattr_destroy(&debug_attributes);
  if (errno != 0) {
    PLOG(FATAL) << "pthread_mutexattr_destroy failed";
  }
#else
  errno = pthread_mutex_init(&mutex_, NULL);
  if (errno != 0) {
    PLOG(FATAL) << "pthread_mutex_init failed";
  }
#endif
}

Mutex::~Mutex() {
  errno = pthread_mutex_destroy(&mutex_);
  if (errno != 0) {
    PLOG(FATAL) << "pthread_mutex_destroy failed";
  }
}

void Mutex::Lock() {
  int result = pthread_mutex_lock(&mutex_);
  if (result != 0) {
    errno = result;
    PLOG(FATAL) << "pthread_mutex_lock failed";
  }
}

bool Mutex::TryLock() {
  int result = pthread_mutex_trylock(&mutex_);
  if (result == EBUSY) {
    return false;
  }
  if (result != 0) {
    errno = result;
    PLOG(FATAL) << "pthread_mutex_trylock failed";
  }
  return true;
}

void Mutex::Unlock() {
  int result = pthread_mutex_unlock(&mutex_);
  if (result != 0) {
    errno = result;
    PLOG(FATAL) << "pthread_mutex_unlock failed";
  }
}

pid_t Mutex::GetOwner() {
#ifdef __BIONIC__
  return static_cast<pid_t>((mutex_.value >> 16) & 0xffff);
#else
  UNIMPLEMENTED(FATAL);
  return 0;
#endif
}

pid_t Mutex::GetTid() {
  return art::GetTid();
}

}  // namespace
