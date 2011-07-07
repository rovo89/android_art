// Copyright 2011 Google Inc. All Rights Reserved.
// Author: cshapiro@google.com (Carl Shapiro)

#ifndef ART_SRC_THREAD_H_
#define ART_SRC_THREAD_H_

#include "src/globals.h"
#include "src/logging.h"
#include "src/macros.h"

namespace art {

class Object;
class Thread;

class Mutex {
 public:
  virtual ~Mutex() {}

  void Lock() {}

  bool TryLock() { return true; }

  void Unlock() {}

  const char* GetName() { return name_; }

  Thread* GetOwner() { return owner_; }

 public:  // TODO: protected
  explicit Mutex(const char* name) : name_(name), owner_(NULL) {}

  void SetOwner(Thread* thread) { owner_ = thread; }

 private:
  const char* name_;

  Thread* owner_;

  DISALLOW_COPY_AND_ASSIGN(Mutex);
};

class MutexLock {
 public:
  explicit MutexLock(Mutex *mu) : mu_(mu) {
    mu_->Lock();
  }
  ~MutexLock() { mu_->Unlock(); }
 private:
  Mutex* const mu_;
  DISALLOW_COPY_AND_ASSIGN(MutexLock);
};

class Thread {
 public:
  static Thread* Self() {
    static Thread self;
    return &self; // TODO
  }

  uint32_t GetThreadId() {
    return thread_id_;
  }

  bool IsExceptionPending() const {
    return false;  // TODO exception_ != NULL;
  }

  Object* GetException() const {
    return exception_;
  }

  void SetException(Object* new_exception) {
    CHECK(new_exception != NULL);
    // TODO: CHECK(exception_ == NULL);
    exception_ = new_exception;  // TODO
  }

  void ClearException() {
    exception_ = NULL;
  }

 private:
  Thread() : thread_id_(1234), exception_(NULL) {}
  ~Thread() {}

  uint32_t thread_id_;

  Object* exception_;

  DISALLOW_COPY_AND_ASSIGN(Thread);
};

}  // namespace art

#endif  // ART_SRC_THREAD_H_
