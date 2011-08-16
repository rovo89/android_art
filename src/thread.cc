// Copyright 2011 Google Inc. All Rights Reserved.

#include "thread.h"

#include <pthread.h>
#include <sys/mman.h>
#include <algorithm>
#include <cerrno>
#include <list>

#include "class_linker.h"
#include "object.h"
#include "runtime.h"
#include "utils.h"

namespace art {

pthread_key_t Thread::pthread_key_self_;

Mutex* Mutex::Create(const char* name) {
  Mutex* mu = new Mutex(name);
  int result = pthread_mutex_init(&mu->lock_impl_, NULL);
  CHECK_EQ(0, result);
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

void* ThreadStart(void *arg) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

Thread* Thread::Create(size_t stack_size) {
  int prot = PROT_READ | PROT_WRITE;
  // TODO: require the stack size to be page aligned?
  size_t length = RoundUp(stack_size, 0x1000);
  void* stack_limit = mmap(NULL, length, prot, MAP_PRIVATE, -1, 0);
  if (stack_limit == MAP_FAILED) {
    LOG(FATAL) << "mmap";
    return false;
  }

  Thread* new_thread = new Thread;
  new_thread->InitCpu();
  new_thread->stack_limit_ = static_cast<byte*>(stack_limit);
  new_thread->stack_base_ = new_thread->stack_limit_ + length;

  pthread_attr_t attr;
  int result = pthread_attr_init(&attr);
  CHECK_EQ(result, 0);

  result = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  CHECK_EQ(result, 0);

  pthread_t handle;
  result = pthread_create(&handle, &attr, ThreadStart, new_thread);
  CHECK_EQ(result, 0);

  result = pthread_attr_destroy(&attr);
  CHECK_EQ(result, 0);

  return new_thread;
}

Thread* Thread::Attach() {
  Thread* thread = new Thread;
  thread->InitCpu();
  thread->stack_limit_ = reinterpret_cast<byte*>(-1);  // TODO: getrlimit
  uintptr_t addr = reinterpret_cast<uintptr_t>(&thread);  // TODO: ask pthreads
  uintptr_t stack_base = RoundUp(addr, kPageSize);
  thread->stack_base_ = reinterpret_cast<byte*>(stack_base);
  // TODO: set the stack size

  thread->handle_ = pthread_self();

  thread->state_ = kRunnable;

  errno = pthread_setspecific(Thread::pthread_key_self_, thread);
  if (errno != 0) {
      PLOG(FATAL) << "pthread_setspecific failed";
  }

  thread->jni_env_ = CreateJNIEnv();

  return thread;
}

static void ThreadExitCheck(void* arg) {
  LG << "Thread exit check";
}

bool Thread::Init() {
  // Allocate a TLS slot.
  if (pthread_key_create(&Thread::pthread_key_self_, ThreadExitCheck) != 0) {
    PLOG(WARNING) << "pthread_key_create failed";
    return false;
  }

  // Double-check the TLS slot allocation.
  if (pthread_getspecific(pthread_key_self_) != NULL) {
    LOG(WARNING) << "newly-created pthread TLS slot is not NULL";
    return false;
  }

  // TODO: initialize other locks and condition variables

  return true;
}

void ThrowNewException(Thread* thread, const char* exception_class_name, const char* msg) {
  CHECK(thread != NULL);

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Class* exception_class = class_linker->FindSystemClass(exception_class_name);
  CHECK(exception_class != NULL);

  Object* exception = exception_class->NewInstance();
  CHECK(exception != NULL);

  size_t char_count = String::ModifiedUtf8Len(msg);
  String* java_msg = String::AllocFromModifiedUtf8(char_count, msg);
  CHECK(java_msg != NULL);

  // TODO: what if there's already a pending exception?
  // TODO: support the other constructors.
  Method* ctor = exception_class->FindDirectMethod("<init>", "(Ljava/lang/String;)V");

  // TODO: need to *call* the constructor!
  UNIMPLEMENTED(WARNING) << "can't call "
                         << exception_class->GetDescriptor() << "."
                         << ctor->GetDescriptor() << " "
                         << "\"" << msg << "\"";

  thread->SetException(exception);
}

void ThrowNewExceptionV(Thread* thread, const char* exception_class_name, const char* fmt, va_list args) {
  char msg[512];
  vsnprintf(msg, sizeof(msg), fmt, args);
  ThrowNewException(thread, exception_class_name, msg);
}

void Thread::ThrowNewException(const char* exception_class_name, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  ThrowNewExceptionV(this, exception_class_name, fmt, args);
  va_end(args);
}

static const char* kStateNames[] = {
  "New",
  "Runnable",
  "Blocked",
  "Waiting",
  "TimedWaiting",
  "Native",
  "Terminated",
};
std::ostream& operator<<(std::ostream& os, const Thread::State& state) {
  if (state >= Thread::kNew && state <= Thread::kTerminated) {
    os << kStateNames[state-Thread::kNew];
  } else {
    os << "State[" << static_cast<int>(state) << "]";
  }
  return os;
}

std::ostream& operator<<(std::ostream& os, const Thread& thread) {
  os << "Thread[" << &thread
      << ",id=" << thread.GetId()
      << ",tid=" << thread.GetNativeId()
      << ",state=" << thread.GetState() << "]";
  return os;
}

ThreadList* ThreadList::Create() {
  return new ThreadList;
}

ThreadList::ThreadList() {
  lock_ = Mutex::Create("ThreadList::Lock");
}

ThreadList::~ThreadList() {
  // Make sure that all threads have exited and unregistered when we
  // reach this point. This means that all daemon threads had been
  // shutdown cleanly.
  CHECK_EQ(list_.size(), 1U);
  // TODO: wait for all other threads to unregister
  CHECK_EQ(list_.front(), Thread::Current());
  // TODO: detach the current thread
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

}  // namespace
