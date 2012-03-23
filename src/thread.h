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
#include <vector>

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
#include "trace.h"
#include "UniquePtr.h"

namespace art {

class Array;
class Class;
class ClassLinker;
class ClassLoader;
class Context;
class DebugInvokeReq;
class Method;
class Monitor;
class Object;
class Runtime;
class StackIndirectReferenceTable;
class StackTraceElement;
class StaticStorageBase;
class Thread;
class ThreadList;
class Throwable;

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
    // These correspond to JDWP states (but needn't share the same values).
    kTerminated   = 0,        // TS_ZOMBIE
    kRunnable     = 1,        // TS_RUNNING
    kTimedWaiting = 2,        // TS_WAIT in Object.wait() with a timeout
    kBlocked      = 3,        // TS_MONITOR on a monitor
    kWaiting      = 4,        // TS_WAIT in Object.wait()
    // Non-JDWP states.
    kStarting     = 5,        // native thread started, not yet ready to run managed code
    kNative       = 6,        // off in a JNI native method
    kVmWait       = 7,        // waiting on an internal runtime resource
    kSuspended    = 8,        // suspended, usually by GC or debugger
  };

  // Space to throw a StackOverflowError in.
  static const size_t kStackOverflowReservedBytes = 4 * KB;

  static const size_t kDefaultStackSize = 96 * KB;

  class StackVisitor {
   public:
    virtual ~StackVisitor() {}
    // Return 'true' if we should continue to visit more frames, 'false' to stop.
    virtual bool VisitFrame(const Frame& frame, uintptr_t pc) = 0;
  };

  // Creates a new native thread corresponding to the given managed peer.
  // Used to implement Thread.start.
  static void Create(Object* peer, size_t stack_size);

  // Attaches the calling native thread to the runtime, returning the new native peer.
  // Used to implement JNI AttachCurrentThread and AttachCurrentThreadAsDaemon calls.
  static Thread* Attach(const char* thread_name, bool as_daemon, Object* thread_group);

  // Reset internal state of child thread after fork.
  void InitAfterFork();

  static Thread* Current() {
    void* thread = pthread_getspecific(Thread::pthread_key_self_);
    return reinterpret_cast<Thread*>(thread);
  }

  static Thread* FromManagedThread(Object* thread_peer);
  static Thread* FromManagedThread(JNIEnv* env, jobject thread);
  static uint32_t LockOwnerFromThreadLock(Object* thread_lock);

  // When full == true, dumps the detailed thread state and the thread stack (used for SIGQUIT).
  // When full == false, dumps a one-line summary of thread state (used for operator<<).
  void Dump(std::ostream& os, bool full = true) const;

  State GetState() const {
    return state_;
  }

  State SetState(State new_state);
  void SetStateWithoutSuspendCheck(State new_state);

  bool IsDaemon();
  bool IsSuspended();

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

  // Returns the "main" ThreadGroup, used when attaching user threads.
  static Object* GetMainThreadGroup();
  // Returns the "system" ThreadGroup, used when attaching our internal threads.
  static Object* GetSystemThreadGroup();

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

  // Returns the java.lang.Thread's name, or NULL if this Thread* doesn't have a peer.
  String* GetThreadName() const;

  // Sets 'name' to the java.lang.Thread's name. This requires no transition to managed code,
  // allocation, or locking.
  void GetThreadName(std::string& name) const;

  // Sets the thread's name.
  void SetThreadName(const char* name);

  Object* GetPeer() const {
    return peer_;
  }

  Object* GetThreadGroup() const;

  RuntimeStats* GetStats() {
    return &stats_;
  }

  int GetSuspendCount() const {
    return suspend_count_;
  }

  // Returns the current Method* and native PC (not dex PC) for this thread.
  Method* GetCurrentMethod(uintptr_t* pc = NULL, Method*** sp = NULL) const;

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
      __attribute__((format(printf, 3, 4)));

  void ThrowNewExceptionV(const char* exception_class_descriptor, const char* fmt, va_list ap);

  // OutOfMemoryError is special, because we need to pre-allocate an instance.
  void ThrowOutOfMemoryError(const char* msg);
  void ThrowOutOfMemoryError(Class* c, size_t byte_count);

  Frame FindExceptionHandler(void* throw_pc, void** handler_pc);

  void* FindExceptionHandlerInMethod(const Method* method,
                                     void* throw_pc,
                                     const DexFile& dex_file,
                                     ClassLinker* class_linker);

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
  void PushNativeToManagedRecord(NativeToManagedRecord* record);
  void PopNativeToManagedRecord(const NativeToManagedRecord& record);

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

#if VERIFY_OBJECT_ENABLED
  void VerifyStack();
#else
  void VerifyStack() {}
#endif

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
    return stack_size_ - (stack_end_ - stack_begin_);
  }

  // Set the stack end to that to be used during a stack overflow
  void SetStackEndForStackOverflow() {
    // During stack overflow we allow use of the full stack
    if (stack_end_ == stack_begin_) {
      DumpStack(std::cerr);
      LOG(FATAL) << "Need to increase kStackOverflowReservedBytes (currently "
                 << kStackOverflowReservedBytes << ")";
    }

    stack_end_ = stack_begin_;
  }

  // Set the stack end to that to be used during regular execution
  void ResetDefaultStackEnd() {
    // Our stacks grow down, so we want stack_end_ to be near there, but reserving enough room
    // to throw a StackOverflowError.
    stack_end_ = stack_begin_ + kStackOverflowReservedBytes;
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

  void PushSirt(StackIndirectReferenceTable* sirt);
  StackIndirectReferenceTable* PopSirt();

  static ThreadOffset TopSirtOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, top_sirt_));
  }

  void WalkStack(StackVisitor* visitor, bool include_upcalls = false) const;

  DebugInvokeReq* GetInvokeReq() {
    return debug_invoke_req_;
  }

  void SetDebuggerUpdatesEnabled(bool enabled);

  bool IsTraceStackEmpty() const {
    return trace_stack_->empty();
  }

  TraceStackFrame GetTraceStackFrame(uint32_t depth) const {
    return trace_stack_->at(trace_stack_->size() - depth - 1);
  }

  void PushTraceStackFrame(const TraceStackFrame& frame) {
    trace_stack_->push_back(frame);
  }

  TraceStackFrame PopTraceStackFrame() {
    TraceStackFrame frame = trace_stack_->back();
    trace_stack_->pop_back();
    return frame;
  }

  void CheckSafeToLockOrUnlock(MutexRank rank, bool is_locking);
  void CheckSafeToWait(MutexRank rank);

 private:
  Thread();
  ~Thread();
  void Destroy();
  friend class ThreadList;  // For ~Thread and Destroy.

  void CreatePeer(const char* name, bool as_daemon, Object* thread_group);
  friend class Runtime; // For CreatePeer.

  void DumpState(std::ostream& os) const;
  void DumpStack(std::ostream& os) const;
  void DumpNativeStack(std::ostream& os) const;

  // Out-of-line conveniences for debugging in gdb.
  static Thread* CurrentFromGdb(); // Like Thread::Current.
  void DumpFromGdb() const; // Like Thread::Dump(std::cerr).

  static void* CreateCallback(void* arg);

  void HandleUncaughtExceptions();
  void RemoveFromThreadGroup();

  void Init();
  void InitCardTable();
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

  // The biased card table, see CardTable for details
  byte* card_table_;

  // The end of this thread's stack. This is the lowest safely-addressable address on the stack.
  // We leave extra space so there's room for the code that throws StackOverflowError.
  byte* stack_end_;

  // Size of the stack
  size_t stack_size_;

  // The "lowest addressable byte" of the stack
  byte* stack_begin_;

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
  // How much of 'suspend_count_' is by request of the debugger, used to set things right
  // when the debugger detaches. Must be <= suspend_count_.
  int debug_suspend_count_;

  // Needed to get the right ClassLoader in JNI_OnLoad, but also
  // useful for testing.
  const ClassLoader* class_loader_override_;

  // Thread local, lazily allocated, long jump context. Used to deliver exceptions.
  Context* long_jump_context_;

  // A boolean telling us whether we're recursively throwing OOME.
  uint32_t throwing_OutOfMemoryError_;

  Throwable* pre_allocated_OutOfMemoryError_;

  // JDWP invoke-during-breakpoint support.
  DebugInvokeReq* debug_invoke_req_;

  // TLS key used to retrieve the Thread*.
  static pthread_key_t pthread_key_self_;

  // Additional stack used by method tracer to store method and return pc values.
  // Stored as a pointer since std::vector is not PACKED.
  std::vector<TraceStackFrame>* trace_stack_;

  // A cached copy of the java.lang.Thread's name.
  std::string* name_;

  uint32_t held_mutexes_[kMaxMutexRank + 1];

 public:
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
  float (*pL2f)(int64_t);
  double (*pL2d)(int64_t);
  int64_t (*pF2l)(float);
  int64_t (*pD2l)(double);
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
  int64_t (*pLadd)(int64_t, int64_t);
  int64_t (*pLsub)(int64_t, int64_t);
  int64_t (*pLand)(int64_t, int64_t);
  int64_t (*pLor)(int64_t, int64_t);
  int64_t (*pLxor)(int64_t, int64_t);
  int64_t (*pLmul)(int64_t, int64_t);
  int64_t (*pLdivmod)(int64_t, int64_t);
  void (*pCheckSuspendFromCode)(Thread*);  // Stub that is called when the suspend count is non-zero
  void (*pTestSuspendFromCode)();  // Stub that is periodically called to test the suspend count
  void* (*pAllocObjectFromCode)(uint32_t, void*);
  void* (*pAllocObjectFromCodeWithAccessCheck)(uint32_t, void*);
  void* (*pAllocArrayFromCode)(uint32_t, void*, int32_t);
  void* (*pAllocArrayFromCodeWithAccessCheck)(uint32_t, void*, int32_t);
  void (*pCanPutArrayElementFromCode)(void*, void*);
  void* (*pCheckAndAllocArrayFromCode)(uint32_t, void*, int32_t);
  void* (*pCheckAndAllocArrayFromCodeWithAccessCheck)(uint32_t, void*, int32_t);
  void (*pCheckCastFromCode)(void*, void*);
  Object* (*pDecodeJObjectInThread)(Thread* thread, jobject obj);
  void (*pDeliverException)(void*);
  Method* (*pFindInterfaceMethodInCache)(Class*, uint32_t, const Method*, struct DvmDex*);
  void* (*pFindNativeMethod)(Thread* thread);
  int32_t (*pGet32Instance)(uint32_t, void*);
  int64_t (*pGet64Instance)(uint32_t, void*);
  void* (*pGetObjInstance)(uint32_t, void*);
  int32_t (*pGet32Static)(uint32_t);
  int64_t (*pGet64Static)(uint32_t);
  void* (*pGetObjStatic)(uint32_t);
  void (*pHandleFillArrayDataFromCode)(void*, void*);
  void* (*pInitializeStaticStorage)(uint32_t, void*);
  uint32_t (*pInstanceofNonTrivialFromCode)(const Class*, const Class*);
  void (*pInvokeDirectTrampolineWithAccessCheck)(uint32_t, void*);
  void (*pInvokeInterfaceTrampoline)(uint32_t, void*);
  void (*pInvokeInterfaceTrampolineWithAccessCheck)(uint32_t, void*);
  void (*pInvokeStaticTrampolineWithAccessCheck)(uint32_t, void*);
  void (*pInvokeSuperTrampolineWithAccessCheck)(uint32_t, void*);
  void (*pInvokeVirtualTrampolineWithAccessCheck)(uint32_t, void*);
  void* (*pInitializeTypeFromCode)(uint32_t, void*);
  void* (*pInitializeTypeAndVerifyAccessFromCode)(uint32_t, void*);
  void (*pLockObjectFromCode)(void*);
  void* (*pResolveStringFromCode)(void*, uint32_t);
  int (*pSet32Instance)(uint32_t, void*, int32_t);  // field_idx, obj, src
  int (*pSet64Instance)(uint32_t, void*, int64_t);
  int (*pSetObjInstance)(uint32_t, void*, void*);
  int (*pSet32Static)(uint32_t, int32_t);
  int (*pSet64Static)(uint32_t, int64_t);
  int (*pSetObjStatic)(uint32_t, void*);
  void (*pThrowStackOverflowFromCode)(void*);
  void (*pThrowNullPointerFromCode)();
  void (*pThrowArrayBoundsFromCode)(int32_t, int32_t);
  void (*pThrowDivZeroFromCode)();
  void (*pThrowVerificationErrorFromCode)(int32_t, int32_t);
  void (*pThrowNegArraySizeFromCode)(int32_t);
  void (*pThrowNoSuchMethodFromCode)(int32_t);
  void (*pThrowAbstractMethodErrorFromCode)(Method* method, Thread* thread, Method** sp);
  void (*pUnlockObjectFromCode)(void*);
  const void* (*pUnresolvedDirectMethodTrampolineFromCode)(Method*, Method**, Thread*,
                                                           Runtime::TrampolineType);
  void (*pUpdateDebuggerFromCode)(void*, void*, int32_t, void*);
  bool (*pCmplFloat)(float, float);
  bool (*pCmpgFloat)(float, float);
  bool (*pCmplDouble)(double, double);
  bool (*pCmpgDouble)(double, double);
  int (*pIndexOf)(void*, uint32_t, uint32_t, uint32_t);
  int (*pStringCompareTo)(void*, void*);
  int (*pMemcmp16)(void*, void*, int32_t);

 private:
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
