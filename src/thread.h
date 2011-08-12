// Copyright 2011 Google Inc. All Rights Reserved.
// Author: cshapiro@google.com (Carl Shapiro)

#ifndef ART_SRC_THREAD_H_
#define ART_SRC_THREAD_H_

#include <pthread.h>
#include <list>

#include "globals.h"
#include "jni_internal.h"
#include "logging.h"
#include "macros.h"
#include "offsets.h"
#include "runtime.h"

#include "jni.h"

namespace art {

class Method;
class Object;
class Runtime;
class StackHandleBlock;
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

// Stack handle blocks are allocated within the bridge frame between managed
// and native code.
class StackHandleBlock {
 public:
  // Number of references contained within this SHB
  size_t NumberOfReferences() {
    return number_of_references_;
  }

  // Link to previous SHB or NULL
  StackHandleBlock* Link() {
    return link_;
  }

  // Offset of length within SHB, used by generated code
  static size_t NumberOfReferencesOffset() {
    return OFFSETOF_MEMBER(StackHandleBlock, number_of_references_);
  }

  // Offset of link within SHB, used by generated code
  static size_t LinkOffset() {
    return OFFSETOF_MEMBER(StackHandleBlock, link_);
  }

 private:
  StackHandleBlock() {}

  size_t number_of_references_;
  StackHandleBlock* link_;

  DISALLOW_COPY_AND_ASSIGN(StackHandleBlock);
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
    kNative,
    kTerminated,
  };

  static const size_t kDefaultStackSize = 64 * KB;

  // Creates a new thread.
  static Thread* Create(size_t stack_size);

  // Creates a new thread from the calling thread.
  static Thread* Attach();

  static Thread* Current() {
    void* thread = pthread_getspecific(Thread::pthread_key_self_);
    return reinterpret_cast<Thread*>(thread);
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

  // Offset of exception within Thread, used by generated code
  static ThreadOffset ExceptionOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, exception_));
  }

  void SetName(const char* name);

  void Suspend();

  bool IsSuspended();

  void Resume();

  static bool Init();

  Runtime* GetRuntime() const {
    return runtime_;
  }

  State GetState() {
    return state_;
  }

  void SetState(State new_state) {
    state_ = new_state;
  }

  static ThreadOffset SuspendCountOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, suspend_count_));
  }

  // Offset of state within Thread, used by generated code
  static ThreadOffset StateOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, state_));
  }

  // JNI methods
  JNIEnv* GetJniEnv() const {
    return jni_env_;
  }

  // Offset of JNI environment within Thread, used by generated code
  static ThreadOffset JniEnvOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, jni_env_));
  }

  // Offset of top of managed stack address, used by generated code
  static ThreadOffset TopOfManagedStackOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, top_of_managed_stack_));
  }

  // Offset of top stack handle block within Thread, used by generated code
  static ThreadOffset TopShbOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, top_shb_));
  }

  // Number of references allocated in StackHandleBlocks on this thread
  size_t NumShbHandles() {
    size_t count = 0;
    for (StackHandleBlock* cur = top_shb_; cur; cur = cur->Link()) {
      count += cur->NumberOfReferences();
    }
    return count;
  }

  // Offset of exception_entry_point_ within Thread, used by generated code
  static ThreadOffset ExceptionEntryPointOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, exception_entry_point_));
  }

  void RegisterExceptionEntryPoint(void (*handler)(Method**)) {
    exception_entry_point_ = handler;
  }

  // Offset of suspend_count_entry_point_ within Thread, used by generated code
  static ThreadOffset SuspendCountEntryPointOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, suspend_count_entry_point_));
  }

  void RegisterSuspendCountEntryPoint(void (*handler)(Method**)) {
    suspend_count_entry_point_ = handler;
  }

  // Increasing the suspend count, will cause the thread to run to safepoint
  void IncrementSuspendCount() { suspend_count_++; }
  void DecrementSuspendCount() { suspend_count_--; }

 private:
  Thread()
      : id_(1234), top_shb_(NULL), exception_(NULL), suspend_count_(0) {
    jni_env_ = CreateJNIEnv();
  }

  ~Thread() {
    delete jni_env_;
  }

  void InitCpu();

  // Managed thread id.
  uint32_t id_;

  // Top of the managed stack, written out prior to the state transition from
  // kRunnable to kNative. Uses include to give the starting point for scanning
  // a managed stack when a thread is in native code.
  void* top_of_managed_stack_;

  // Top of linked list of stack handle blocks or NULL for none
  StackHandleBlock* top_shb_;

  // Every thread may have an associated JNI environment
  JNIEnv* jni_env_;

  State state_;

  // Native (kernel) thread id.
  pid_t native_id_;

  // Native thread handle.
  pthread_t handle_;

  // Initialized to "this". On certain architectures (such as x86) reading
  // off of Thread::Current is easy but getting the address of Thread::Current
  // is hard. This field can be read off of Thread::Current to give the address.
  Thread* self_;

  Runtime* runtime_;

  // The pending exception or NULL.
  Object* exception_;

  // A non-zero value is used to tell the current thread to enter a safe point
  // at the next poll.
  int suspend_count_;

  // The inclusive base of the control stack.
  byte* stack_base_;

  // The exclusive limit of the control stack.
  byte* stack_limit_;

  // TLS key used to retrieve the VM thread object.
  static pthread_key_t pthread_key_self_;

  // Entry point called when exception_ is set
  void (*exception_entry_point_)(Method** frame);

  // Entry point called when suspend_count_ is non-zero
  void (*suspend_count_entry_point_)(Method** frame);

  DISALLOW_COPY_AND_ASSIGN(Thread);
};
std::ostream& operator<<(std::ostream& os, const Thread::State& state);

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
