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

#ifndef ART_SRC_THREAD_H_
#define ART_SRC_THREAD_H_

#include <pthread.h>

#include <bitset>
#include <iosfwd>
#include <list>
#include <string>

#include "dex_file.h"
#include "globals.h"
#include "jni_internal.h"
#include "logging.h"
#include "macros.h"
#include "mutex.h"
#include "mem_map.h"
#include "offsets.h"
#include "runtime_stats.h"
#include "stack.h"
#include "UniquePtr.h"

namespace art {

class Array;
class Class;
class ClassLinker;
class ClassLoader;
class Context;
class Method;
class Monitor;
class Object;
class Runtime;
class Thread;
class ThreadList;
class Throwable;
class StackIndirectReferenceTable;
class StackTraceElement;
class StaticStorageBase;

template<class T> class ObjectArray;
template<class T> class PrimitiveArray;
typedef PrimitiveArray<int32_t> IntArray;

class PACKED Thread {
 public:
  /* thread priorities, from java.lang.Thread */
  enum Priority {
    kMinPriority = 1,
    kNormPriority = 5,
    kMaxPriority = 10,
  };
  enum State {
    // These match up with JDWP values.
    kTerminated   = 0,        // TERMINATED
    kRunnable     = 1,        // RUNNABLE or running now
    kTimedWaiting = 2,        // TIMED_WAITING in Object.wait()
    kBlocked      = 3,        // BLOCKED on a monitor
    kWaiting      = 4,        // WAITING in Object.wait()
    // Non-JDWP states.
    kInitializing = 5,        // allocated, not yet running --- TODO: unnecessary?
    kStarting     = 6,        // native thread started, not yet ready to run managed code
    kNative       = 7,        // off in a JNI native method
    kVmWait       = 8,        // waiting on a VM resource
    kSuspended    = 9,        // suspended, usually by GC or debugger
  };

  // Space to throw a StackOverflowError in.
  static const size_t kStackOverflowReservedBytes = 4 * KB;

  static const size_t kDefaultStackSize = 64 * KB;

  // Runtime support function pointers
  void (*pDebugMe)(Method*, uint32_t);
  void* (*pMemcpy)(void*, const void*, size_t);
  uint64_t (*pShlLong)(uint64_t, uint32_t);
  uint64_t (*pShrLong)(uint64_t, uint32_t);
  uint64_t (*pUshrLong)(uint64_t, uint32_t);
  float (*pI2f)(int);
  int (*pF2iz)(float);
  float (*pD2f)(double);
  double (*pF2d)(float);
  double (*pI2d)(int);
  int (*pD2iz)(double);
  float (*pL2f)(long);
  double (*pL2d)(long);
  long long (*pF2l)(float);
  long long (*pD2l)(double);
  float (*pFadd)(float, float);
  float (*pFsub)(float, float);
  float (*pFdiv)(float, float);
  float (*pFmul)(float, float);
  float (*pFmodf)(float, float);
  double (*pDadd)(double, double);
  double (*pDsub)(double, double);
  double (*pDdiv)(double, double);
  double (*pDmul)(double, double);
  double (*pFmod)(double, double);
  int (*pIdivmod)(int, int);
  int (*pIdiv)(int, int);
  long long (*pLmul)(long long, long long);
  long long (*pLdivmod)(long long, long long);
  void (*pCheckSuspendFromCode)(Thread*);  // Stub that is called when the suspend count is non-zero
  void (*pTestSuspendFromCode)();  // Stub that is periodically called to test the suspend count
  void* (*pAllocObjectFromCode)(uint32_t, void*);
  void* (*pAllocArrayFromCode)(uint32_t, void*, int32_t);
  void (*pCanPutArrayElementFromCode)(void*, void*);
  void* (*pCheckAndAllocArrayFromCode)(uint32_t, void*, int32_t);
  void (*pCheckCastFromCode) (void*, void*);
  Object* (*pDecodeJObjectInThread)(Thread* thread, jobject obj);
  void (*pDeliverException)(void*);
  void* (*pFindInstanceFieldFromCode)(uint32_t, void*);
  Method* (*pFindInterfaceMethodInCache)(Class*, uint32_t, const Method*, struct DvmDex*);
  void* (*pFindNativeMethod)(Thread* thread);
  int32_t (*pGet32Static)(uint32_t, void*);
  int64_t (*pGet64Static)(uint32_t, void*);
  void* (*pGetObjStatic)(uint32_t, void*);
  void (*pHandleFillArrayDataFromCode)(void*, void*);
  void* (*pInitializeStaticStorage)(uint32_t, void*);
  uint32_t (*pInstanceofNonTrivialFromCode) (const Class*, const Class*);
  void (*pInvokeInterfaceTrampoline)(uint32_t, void*);
  Class* (*pInitializeTypeFromCode)(uint32_t, Method*);
  void (*pLockObjectFromCode)(void*);
  void (*pObjectInit)(Object*);
  void (*pResolveMethodFromCode)(Method*, uint32_t);
  void* (*pResolveStringFromCode)(void*, uint32_t);
  int (*pSet32Static)(uint32_t, void*, int32_t);
  int (*pSet64Static)(uint32_t, void*, int64_t);
  int (*pSetObjStatic)(uint32_t, void*, void*);
  void (*pThrowStackOverflowFromCode)(void*);
  void (*pThrowNullPointerFromCode)();
  void (*pThrowArrayBoundsFromCode)(int32_t, int32_t);
  void (*pThrowDivZeroFromCode)();
  void (*pThrowVerificationErrorFromCode)(int32_t, int32_t);
  void (*pThrowNegArraySizeFromCode)(int32_t);
  void (*pThrowNoSuchMethodFromCode)(int32_t);
  void (*pThrowAbstractMethodErrorFromCode)(Method* method, Thread* thread, Method** sp);
  void (*pUnlockObjectFromCode)(void*);
  void* (*pUnresolvedDirectMethodTrampolineFromCode)(int32_t, void*, Thread*,
                                                     Runtime::TrampolineType);

  class StackVisitor {
   public:
    virtual ~StackVisitor() {}
    virtual void VisitFrame(const Frame& frame, uintptr_t pc) = 0;
  };

  // Creates a new thread.
  static void Create(Object* peer, size_t stack_size);

  // Creates a new thread from the calling thread.
  static Thread* Attach(const Runtime* runtime, const char* name, bool as_daemon);

  // Reset internal state of child thread after fork.
  void InitAfterFork();

  static Thread* Current() {
    void* thread = pthread_getspecific(Thread::pthread_key_self_);
    return reinterpret_cast<Thread*>(thread);
  }

  static Thread* FromManagedThread(JNIEnv* env, jobject thread);
  static uint32_t LockOwnerFromThreadLock(Object* thread_lock);

  void Dump(std::ostream& os) const;

  State GetState() const {
    return state_;
  }

  State SetState(State new_state);

  bool IsDaemon();

  void WaitUntilSuspended();

  bool HoldsLock(Object*);

  /*
   * Changes the priority of this thread to match that of the java.lang.Thread object.
   *
   * We map a priority value from 1-10 to Linux "nice" values, where lower
   * numbers indicate higher priority.
   */
  void SetNativePriority(int newPriority);

  /*
   * Returns the thread priority for the current thread by querying the system.
   * This is useful when attaching a thread through JNI.
   *
   * Returns a value from 1 to 10 (compatible with java.lang.Thread values).
   */
  static int GetNativePriority();

  bool CanAccessDirectReferences() const {
#ifdef MOVING_GARBAGE_COLLECTOR
    // TODO: when we have a moving collector, we'll need: return state_ == kRunnable;
#endif
    return true;
  }

  uint32_t GetThinLockId() const {
    return thin_lock_id_;
  }

  pid_t GetTid() const {
    return tid_;
  }

  Object* GetPeer() const {
    return peer_;
  }

  RuntimeStats* GetStats() {
    return &stats_;
  }

  // Returns the Method* for the current method.
  // This is used by the JNI implementation for logging and diagnostic purposes.
  const Method* GetCurrentMethod() const;

  bool IsExceptionPending() const {
    return exception_ != NULL;
  }

  Throwable* GetException() const {
    DCHECK(CanAccessDirectReferences());
    return exception_;
  }

  void SetException(Throwable* new_exception) {
    DCHECK(CanAccessDirectReferences());
    CHECK(new_exception != NULL);
    // TODO: CHECK(exception_ == NULL);
    exception_ = new_exception;  // TODO
  }

  void ClearException() {
    exception_ = NULL;
  }

  // Find catch block and perform long jump to appropriate exception handle
  void DeliverException();

  Context* GetLongJumpContext();

  Frame GetTopOfStack() const {
    return top_of_managed_stack_;
  }

  // TODO: this is here for testing, remove when we have exception unit tests
  // that use the real stack
  void SetTopOfStack(void* stack, uintptr_t pc) {
    top_of_managed_stack_.SetSP(reinterpret_cast<Method**>(stack));
    top_of_managed_stack_pc_ = pc;
  }

  void SetTopOfStackPC(uintptr_t pc) {
    top_of_managed_stack_pc_ = pc;
  }

  // 'msg' may be NULL.
  void ThrowNewException(const char* exception_class_descriptor, const char* msg);

  void ThrowNewExceptionF(const char* exception_class_descriptor, const char* fmt, ...)
      __attribute__ ((format(printf, 3, 4)));

  void ThrowNewExceptionV(const char* exception_class_descriptor, const char* fmt, va_list ap);

  // This exception is special, because we need to pre-allocate an instance.
  void ThrowOutOfMemoryError(Class* c, size_t byte_count);

  Frame FindExceptionHandler(void* throw_pc, void** handler_pc);

  void* FindExceptionHandlerInMethod(const Method* method,
                                     void* throw_pc,
                                     const DexFile& dex_file,
                                     ClassLinker* class_linker);

  void SetName(const char* name);

  static void Startup();
  static void FinishStartup();
  static void Shutdown();

  // JNI methods
  JNIEnvExt* GetJniEnv() const {
    return jni_env_;
  }

  // Number of references allocated in SIRTs on this thread
  size_t NumSirtReferences();

  // Is the given obj in this thread's stack indirect reference table?
  bool SirtContains(jobject obj);

  void SirtVisitRoots(Heap::RootVisitor* visitor, void* arg);

  // Convert a jobject into a Object*
  Object* DecodeJObject(jobject obj);

  // Implements java.lang.Thread.interrupted.
  bool Interrupted() {
    MutexLock mu(*wait_mutex_);
    bool interrupted = interrupted_;
    interrupted_ = false;
    return interrupted;
  }

  // Implements java.lang.Thread.isInterrupted.
  bool IsInterrupted() {
    MutexLock mu(*wait_mutex_);
    return interrupted_;
  }

  void Interrupt() {
    MutexLock mu(*wait_mutex_);
    if (interrupted_) {
      return;
    }
    interrupted_ = true;
    NotifyLocked();
  }

  void Notify() {
    MutexLock mu(*wait_mutex_);
    NotifyLocked();
  }

  // Linked list recording transitions from native to managed code
  void PushNativeToManagedRecord(NativeToManagedRecord* record) {
    record->last_top_of_managed_stack_ = reinterpret_cast<void*>(top_of_managed_stack_.GetSP());
    record->last_top_of_managed_stack_pc_ = top_of_managed_stack_pc_;
    record->link_ = native_to_managed_record_;
    native_to_managed_record_ = record;
    top_of_managed_stack_.SetSP(NULL);
  }

  void PopNativeToManagedRecord(const NativeToManagedRecord& record) {
    native_to_managed_record_ = record.link_;
    top_of_managed_stack_.SetSP(reinterpret_cast<Method**>(record.last_top_of_managed_stack_));
    top_of_managed_stack_pc_ = record.last_top_of_managed_stack_pc_;
  }

  const ClassLoader* GetClassLoaderOverride() {
    // TODO: need to place the class_loader_override_ in a handle
    // DCHECK(CanAccessDirectReferences());
    return class_loader_override_;
  }

  void SetClassLoaderOverride(const ClassLoader* class_loader_override) {
    class_loader_override_ = class_loader_override;
  }

  // Create the internal representation of a stack trace, that is more time
  // and space efficient to compute than the StackTraceElement[]
  jobject CreateInternalStackTrace(JNIEnv* env) const;

  // Convert an internal stack trace representation (returned by CreateInternalStackTrace) to a
  // StackTraceElement[]. If output_array is NULL, a new array is created, otherwise as many
  // frames as will fit are written into the given array. If stack_depth is non-NULL, it's updated
  // with the number of valid frames in the returned array.
  static jobjectArray InternalStackTraceToStackTraceElementArray(JNIEnv* env, jobject internal,
      jobjectArray output_array = NULL, int* stack_depth = NULL);

  void VisitRoots(Heap::RootVisitor* visitor, void* arg);

  //
  // Offsets of various members of native Thread class, used by compiled code.
  //

  static ThreadOffset SelfOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, self_));
  }

  static ThreadOffset ExceptionOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, exception_));
  }

  static ThreadOffset ThinLockIdOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, thin_lock_id_));
  }

  static ThreadOffset CardTableOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, card_table_));
  }

  static ThreadOffset SuspendCountOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, suspend_count_));
  }

  static ThreadOffset StateOffset() {
    return ThreadOffset(OFFSETOF_VOLATILE_MEMBER(Thread, state_));
  }

  // Size of stack less any space reserved for stack overflow
  size_t GetStackSize() {
    return stack_size_ - (stack_end_ - stack_base_);
  }

  // Set the stack end to that to be used during a stack overflow
  void SetStackEndForStackOverflow() {
    // During stack overflow we allow use of the full stack
    if (stack_end_ == stack_base_) {
      DumpStack(std::cerr);
      LOG(FATAL) << "Need to increase kStackOverflowReservedBytes (currently "
                 << kStackOverflowReservedBytes << ")";
    }

    stack_end_ = stack_base_;
  }

  // Set the stack end to that to be used during regular execution
  void ResetDefaultStackEnd() {
    // Our stacks grow down, so we want stack_end_ to be near there, but reserving enough room
    // to throw a StackOverflowError.
    stack_end_ = stack_base_ + kStackOverflowReservedBytes;
  }

  static ThreadOffset StackEndOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, stack_end_));
  }

  static ThreadOffset JniEnvOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, jni_env_));
  }

  static ThreadOffset TopOfManagedStackOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, top_of_managed_stack_) +
        OFFSETOF_MEMBER(Frame, sp_));
  }

  static ThreadOffset TopOfManagedStackPcOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, top_of_managed_stack_pc_));
  }

  static ThreadOffset TopSirtOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, top_sirt_));
  }

  void WalkStack(StackVisitor* visitor) const;

 private:
  Thread();
  ~Thread();
  friend class ThreadList;  // For ~Thread.

  void CreatePeer(const char* name, bool as_daemon);
  friend class Runtime; // For CreatePeer.

  void DumpState(std::ostream& os) const;
  void DumpStack(std::ostream& os) const;

  void Attach(const Runtime* runtime);
  static void* CreateCallback(void* arg);

  void InitCpu();
  void InitFunctionPointers();
  void InitTid();
  void InitPthreadKeySelf();
  void InitStackHwm();

  void NotifyLocked() {
    if (wait_monitor_ != NULL) {
      wait_cond_->Signal();
    }
  }

  static void ThreadExitCallback(void* arg);

  void WalkStackUntilUpCall(StackVisitor* visitor, bool include_upcall) const;

  // Thin lock thread id. This is a small integer used by the thin lock implementation.
  // This is not to be confused with the native thread's tid, nor is it the value returned
  // by java.lang.Thread.getId --- this is a distinct value, used only for locking. One
  // important difference between this id and the ids visible to managed code is that these
  // ones get reused (to ensure that they fit in the number of bits available).
  uint32_t thin_lock_id_;

  // System thread id.
  pid_t tid_;

  // Our managed peer (an instance of java.lang.Thread).
  Object* peer_;

  // The top_of_managed_stack_ and top_of_managed_stack_pc_ fields are accessed from
  // compiled code, so we keep them early in the structure to (a) avoid having to keep
  // fixing the assembler offsets and (b) improve the chances that these will still be aligned.

  // Top of the managed stack, written out prior to the state transition from
  // kRunnable to kNative. Uses include giving the starting point for scanning
  // a managed stack when a thread is in native code.
  Frame top_of_managed_stack_;
  // PC corresponding to the call out of the top_of_managed_stack_ frame
  uintptr_t top_of_managed_stack_pc_;

  // Guards the 'interrupted_' and 'wait_monitor_' members.
  mutable Mutex* wait_mutex_;
  ConditionVariable* wait_cond_;
  // Pointer to the monitor lock we're currently waiting on (or NULL), guarded by wait_mutex_.
  Monitor* wait_monitor_;
  // Thread "interrupted" status; stays raised until queried or thrown, guarded by wait_mutex_.
  uint32_t interrupted_;
  // The next thread in the wait set this thread is part of.
  Thread* wait_next_;
  // If we're blocked in MonitorEnter, this is the object we're trying to lock.
  Object* monitor_enter_object_;

  friend class Monitor;

  RuntimeStats stats_;

  // FIXME: placeholder for the gc cardTable
  uint32_t card_table_;

  // The end of this thread's stack. This is the lowest safely-addressable address on the stack.
  // We leave extra space so there's room for the code that throws StackOverflowError.
  byte* stack_end_;

  // Size of the stack
  size_t stack_size_;

  // The "lowest addressable byte" of the stack
  byte* stack_base_;

  // A linked list (of stack allocated records) recording transitions from
  // native to managed code.
  NativeToManagedRecord* native_to_managed_record_;

  // Top of linked list of stack indirect reference tables or NULL for none
  StackIndirectReferenceTable* top_sirt_;

  // Every thread may have an associated JNI environment
  JNIEnvExt* jni_env_;

  volatile State state_;

  // Initialized to "this". On certain architectures (such as x86) reading
  // off of Thread::Current is easy but getting the address of Thread::Current
  // is hard. This field can be read off of Thread::Current to give the address.
  Thread* self_;

  Runtime* runtime_;

  // The pending exception or NULL.
  Throwable* exception_;

  // A non-zero value is used to tell the current thread to enter a safe point
  // at the next poll.
  int suspend_count_;

  // Needed to get the right ClassLoader in JNI_OnLoad, but also
  // useful for testing.
  const ClassLoader* class_loader_override_;

  // Thread local, lazily allocated, long jump context. Used to deliver exceptions.
  Context* long_jump_context_;

  // A boolean telling us whether we're recursively throwing OOME.
  uint32_t throwing_OutOfMemoryError_;

  Throwable* pre_allocated_OutOfMemoryError_;

  // TLS key used to retrieve the VM thread object.
  static pthread_key_t pthread_key_self_;

  DISALLOW_COPY_AND_ASSIGN(Thread);
};

std::ostream& operator<<(std::ostream& os, const Thread& thread);
std::ostream& operator<<(std::ostream& os, const Thread::State& state);

class ScopedThreadStateChange {
 public:
  ScopedThreadStateChange(Thread* thread, Thread::State new_state) : thread_(thread) {
    old_thread_state_ = thread_->SetState(new_state);
  }

  ~ScopedThreadStateChange() {
    thread_->SetState(old_thread_state_);
  }

 private:
  Thread* thread_;
  Thread::State old_thread_state_;
  DISALLOW_COPY_AND_ASSIGN(ScopedThreadStateChange);
};

}  // namespace art

#endif  // ART_SRC_THREAD_H_
