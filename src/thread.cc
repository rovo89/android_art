// Copyright 2011 Google Inc. All Rights Reserved.

#include "src/thread.h"

#include <algorithm>
#include <list>
#include <errno.h>
#include <pthread.h>

#include "src/runtime.h"

namespace art {

pthread_key_t Thread::pthread_key_self_;

Mutex* Mutex::Create(const char* name) {
  Mutex* mu = new Mutex(name);
  int result = pthread_mutex_init(&mu->lock_impl_, NULL);
  CHECK(result == 0);
  return mu;
}

void Mutex::Lock() {
  int result = pthread_mutex_lock(&lock_impl_);
  CHECK_EQ(result, 0);
  SetOwner(Thread::Current());
}

bool Mutex::TryLock() {
  int result = pthread_mutex_lock(&lock_impl_);
  if (result == EBUSY) {
    return false;
  } else {
    CHECK_EQ(result, 0);
    SetOwner(Thread::Current());
    return true;
  }
}

void Mutex::Unlock() {
  CHECK(GetOwner() == Thread::Current());
  int result = pthread_mutex_unlock(&lock_impl_);
  CHECK_EQ(result, 0);
  SetOwner(Thread::Current());
}

Thread* Thread::Create(const char* name) {
  LOG(FATAL) << "Unimplemented";
  return NULL;
}

static void ThreadExitCheck(void* arg) {
  LG << "Thread exit check";
}

bool Thread::Init() {
  // Allocate a TLS slot.
  if (pthread_key_create(&Thread::pthread_key_self_, ThreadExitCheck) != 0) {
    LOG(WARN) << "pthread_key_create failed";
    return false;
  }

  // Double-check the TLS slot allocation.
  if (pthread_getspecific(pthread_key_self_) != NULL) {
    LOG(WARN) << "newly-created pthread TLS slot is not NULL";
    return false;
  }

  // TODO: initialize other locks and condition variables

  return true;
}

ThreadList::ThreadList() {
  lock_ = Mutex::Create("ThreadList::Lock");
}

ThreadList::~ThreadList() {
  // Make sure that all threads have exited and unregistered when we
  // reach this point. This means that all daemon threads had been
  // shutdown cleanly.
  CHECK_EQ(list_.size(), 0);
  delete lock_;
  lock_ = NULL;
}

void ThreadList::Register(Thread* thread) {
  MutexLock mu(lock_);
  CHECK(find(list_.begin(), list_.end(), thread) == list_.end());
  list_.push_front(thread);
}

void ThreadList::Unregister(Thread* thread) {
  MutexLock mu(lock_);
  CHECK(find(list_.begin(), list_.end(), thread) != list_.end());
  list_.remove(thread);
}

void ThreadList::Init(Runtime* runtime) {
  ThreadList* thread_list = new ThreadList();
  runtime->SetThreadList(thread_list);
}

}  // namespace
