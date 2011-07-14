// Copyright 2011 Google Inc. All Rights Reserved.
// Author: cshapiro@google.com (Carl Shapiro)

#ifndef ART_SRC_THREAD_H_
#define ART_SRC_THREAD_H_

#include <list>
#include <pthread.h>

#include "src/globals.h"
#include "src/logging.h"
#include "src/macros.h"
#include "src/runtime.h"

namespace art {

class Heap;
class Object;
class Runtime;
class Thread;
class ThreadList;

class Mutex {
 public:
  virtual ~Mutex() {}

  void Lock();

  bool TryLock();

  void Unlock();

  const char* GetName() { return name_; }

  Thread* GetOwner() { return owner_; }

  static Mutex* Create(const char* name);

 public:  // TODO: protected
  explicit Mutex(const char* name) : name_(name), owner_(NULL) {}

  void SetOwner(Thread* thread) { owner_ = thread; }

 private:
  const char* name_;

  Thread* owner_;

  pthread_mutex_t lock_impl_;

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
  enum State {
    kUnknown = -1,
    kNew,
    kRunnable,
    kBlocked,
    kWaiting,
    kTimedWaiting,
    kTerminated,
  };

  static const size_t kDefaultStackSize = 64 * KB;

  // Creates a new thread.
  static Thread* Create(size_t stack_size);

  // Creates a new thread from the calling thread.
  static Thread* Attach();

  static Thread* Current() {
    static Thread self;
    return &self; // TODO
  }

  uint32_t GetId() const {
    return id_;
  }

  pid_t GetNativeId() const {
    return native_id_;
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

  void SetName(const char* name);

  void Suspend();

  bool IsSuspended();

  void Resume();

  static bool Init();

  State GetState() {
    return state_;
  }

  void SetState(State new_state) {
    state_ = new_state;
  }

 private:
  Thread() : id_(1234), exception_(NULL) {}
  ~Thread() {}

  State state_;

  uint32_t id_;

  pid_t native_id_;

  pthread_t handle_;

  Object* exception_;

  byte* stack_base_;
  byte* stack_limit_;

  static pthread_key_t pthread_key_self_;

  DISALLOW_COPY_AND_ASSIGN(Thread);
};

class ThreadList {
 public:
  static const int kMaxId = 0xFFFF;
  static const int kInvalidId = 0;
  static const int kMainId = 1;

  static ThreadList* Create();

  ~ThreadList();

  void Register(Thread* thread);

  void Unregister(Thread* thread);

  void Lock() {
    lock_->Lock();
  }

  void Unlock() {
    lock_->Unlock();
  };

 private:
  ThreadList();

  std::list<Thread*> list_;

  Mutex* lock_;

  DISALLOW_COPY_AND_ASSIGN(ThreadList);
};

class ThreadListLock {
 public:
  ThreadListLock(ThreadList* thread_list, Thread* current_thread)
      : thread_list_(thread_list) {
    if (current_thread == NULL) {  // try to get it from TLS
      current_thread = Thread::Current();
    }
    Thread::State old_state;
    if (current_thread != NULL) {
      old_state = current_thread->GetState();
      current_thread->SetState(Thread::kWaiting);  // TODO: VMWAIT
    } else {
      // happens during VM shutdown
      old_state = Thread::kUnknown;  // TODO: something else
    }
    thread_list_->Lock();
    if (current_thread != NULL) {
      current_thread->SetState(old_state);
    }
  }

  ~ThreadListLock() {
    thread_list_->Unlock();
  }

 private:
  ThreadList* thread_list_;

  DISALLOW_COPY_AND_ASSIGN(ThreadListLock);
};

}  // namespace art

#endif  // ART_SRC_THREAD_H_
