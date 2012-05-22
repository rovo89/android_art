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
#include "oat/runtime/oat_support_entrypoints.h"
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
class ShadowFrame;
class StackIndirectReferenceTable;
class StackTraceElement;
class StaticStorageBase;
class Thread;
class ThreadList;
class Throwable;

template<class T> class ObjectArray;
template<class T> class PrimitiveArray;
typedef PrimitiveArray<int32_t> IntArray;

// Thread priorities. These must match the Thread.MIN_PRIORITY,
// Thread.NORM_PRIORITY, and Thread.MAX_PRIORITY constants.
enum ThreadPriority {
  kMinThreadPriority = 1,
  kNormThreadPriority = 5,
  kMaxThreadPriority = 10,
};

enum ThreadState {
  kTerminated   = 0, // Thread.TERMINATED     JDWP TS_ZOMBIE
  kRunnable     = 1, // Thread.RUNNABLE       JDWP TS_RUNNING
  kTimedWaiting = 2, // Thread.TIMED_WAITING  JDWP TS_WAIT    - in Object.wait() with a timeout
  kBlocked      = 3, // Thread.BLOCKED        JDWP TS_MONITOR - blocked on a monitor
  kWaiting      = 4, // Thread.WAITING        JDWP TS_WAIT    - in Object.wait()
  kStarting     = 5, // Thread.NEW                            - native thread started, not yet ready to run managed code
  kNative       = 6, //                                       - running in a JNI native method
  kVmWait       = 7, //                                       - waiting on an internal runtime resource
  kSuspended    = 8, //                                       - suspended by GC or debugger
};

class PACKED Thread {
 public:
  // Space to throw a StackOverflowError in.
#if !defined(ART_USE_LLVM_COMPILER)
  static const size_t kStackOverflowReservedBytes = 4 * KB;
#else  // LLVM_x86 requires more memory to throw stack overflow exception.
  static const size_t kStackOverflowReservedBytes = 8 * KB;
#endif

  static const size_t kDefaultStackSize = 16 * KB;

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

  // Translates 172 to pAllocArrayFromCode and so on.
  static void DumpThreadOffset(std::ostream& os, uint32_t offset, size_t size_of_pointers);

  // When full == true, dumps the detailed thread state and the thread stack (used for SIGQUIT).
  // When full == false, dumps a one-line summary of thread state (used for operator<<).
  void Dump(std::ostream& os, bool full = true) const;

  ThreadState GetState() const {
    return state_;
  }

  ThreadState SetState(ThreadState new_state);
  void SetStateWithoutSuspendCheck(ThreadState new_state);

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

  bool IsStillStarting() const;

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
  // Only the GC should call this.
  void ThrowOutOfMemoryError(const char* msg);

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

  // Number of references in SIRTs on this thread
  size_t NumSirtReferences();

  // Number of references allocated in ShadowFrames on this thread
  size_t NumShadowFrameReferences();

  // Number of references allocated in SIRTs & shadow frames on this thread
  size_t NumStackReferences() {
    return NumSirtReferences() + NumShadowFrameReferences();
  };

  // Is the given obj in this thread's stack indirect reference table?
  bool SirtContains(jobject obj);

  // Is the given obj in this thread's ShadowFrame?
  bool ShadowFrameContains(jobject obj);

  // Is the given obj in this thread's Sirts & ShadowFrames?
  bool StackReferencesContain(jobject obj);

  void SirtVisitRoots(Heap::RootVisitor* visitor, void* arg);

  void ShadowFrameVisitRoots(Heap::RootVisitor* visitor, void* arg);

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

  void PushShadowFrame(ShadowFrame* frame);
  ShadowFrame* PopShadowFrame();

  static ThreadOffset TopShadowFrameOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, top_shadow_frame_));
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

  // Top of linked list of shadow stack or NULL for none
  // Some backend may require shadow frame to ease the GC work.
  ShadowFrame* top_shadow_frame_;

  // Every thread may have an associated JNI environment
  JNIEnvExt* jni_env_;

  volatile ThreadState state_;

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
  EntryPoints entrypoints_;

 private:
  DISALLOW_COPY_AND_ASSIGN(Thread);
};

std::ostream& operator<<(std::ostream& os, const Thread& thread);
std::ostream& operator<<(std::ostream& os, const ThreadState& state);

class ScopedThreadStateChange {
 public:
  ScopedThreadStateChange(Thread* thread, ThreadState new_state) : thread_(thread) {
    old_thread_state_ = thread_->SetState(new_state);
  }

  ~ScopedThreadStateChange() {
    thread_->SetState(old_thread_state_);
  }

 private:
  Thread* thread_;
  ThreadState old_thread_state_;
  DISALLOW_COPY_AND_ASSIGN(ScopedThreadStateChange);
};

}  // namespace art

#endif  // ART_SRC_THREAD_H_
