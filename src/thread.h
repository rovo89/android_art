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
#ifdef ART_USE_GREENLAND_COMPILER
#include "greenland/runtime_entry_points.h"
#endif

namespace art {

class Array;
class Class;
class ClassLinker;
class ClassLoader;
class Context;
struct DebugInvokeReq;
class Method;
class Monitor;
class Object;
class Runtime;
class ScopedObjectAccess;
class ScopedObjectAccessUnchecked;
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
  kTerminated                     = 0,   // Thread.TERMINATED     JDWP TS_ZOMBIE
  kRunnable                       = 1,   // Thread.RUNNABLE       JDWP TS_RUNNING
  kTimedWaiting                   = 2,   // Thread.TIMED_WAITING  JDWP TS_WAIT    - in Object.wait() with a timeout
  kBlocked                        = 3,   // Thread.BLOCKED        JDWP TS_MONITOR - blocked on a monitor
  kWaiting                        = 4,   // Thread.WAITING        JDWP TS_WAIT    - in Object.wait()
  kWaitingForGcToComplete         = 5,   // Thread.WAITING        JDWP TS_WAIT    - blocked waiting for GC
  kWaitingPerformingGc            = 6,   // Thread.WAITING        JDWP TS_WAIT    - performing GC
  kWaitingForDebuggerSend         = 7,   // Thread.WAITING        JDWP TS_WAIT    - blocked waiting for events to be sent
  kWaitingForDebuggerToAttach     = 8,   // Thread.WAITING        JDWP TS_WAIT    - blocked waiting for debugger to attach
  kWaitingInMainDebuggerLoop      = 9,   // Thread.WAITING        JDWP TS_WAIT    - blocking/reading/processing debugger events
  kWaitingForDebuggerSuspension   = 10,  // Thread.WAITING        JDWP TS_WAIT    - waiting for debugger suspend all
  kWaitingForJniOnLoad            = 11,  // Thread.WAITING        JDWP TS_WAIT    - waiting for execution of dlopen and JNI on load code
  kWaitingForSignalCatcherOutput  = 12,  // Thread.WAITING        JDWP TS_WAIT    - waiting for signal catcher IO to complete
  kWaitingInMainSignalCatcherLoop = 13,  // Thread.WAITING        JDWP TS_WAIT    - blocking/reading/processing signals
  kStarting                       = 14,  // Thread.NEW            JDWP TS_WAIT    - native thread started, not yet ready to run managed code
  kNative                         = 15,  // Thread.RUNNABLE       JDWP TS_RUNNING - running in a JNI native method
  kSuspended                      = 16,  // Thread.RUNNABLE       JDWP TS_RUNNING - suspended by GC or debugger
};

class PACKED Thread {
 public:
  // Space to throw a StackOverflowError in.
#if !defined(ART_USE_LLVM_COMPILER)
  static const size_t kStackOverflowReservedBytes = 4 * KB;
#else  // LLVM_x86 requires more memory to throw stack overflow exception.
  static const size_t kStackOverflowReservedBytes = 8 * KB;
#endif

  // Creates a new native thread corresponding to the given managed peer.
  // Used to implement Thread.start.
  static void CreateNativeThread(JNIEnv* env, jobject peer, size_t stack_size, bool daemon);

  // Attaches the calling native thread to the runtime, returning the new native peer.
  // Used to implement JNI AttachCurrentThread and AttachCurrentThreadAsDaemon calls.
  static Thread* Attach(const char* thread_name, bool as_daemon, jobject thread_group);

  // Reset internal state of child thread after fork.
  void InitAfterFork();

  static Thread* Current() __attribute__ ((pure)) {
    // We rely on Thread::Current returning NULL for a detached thread, so it's not obvious
    // that we can replace this with a direct %fs access on x86.
    void* thread = pthread_getspecific(Thread::pthread_key_self_);
    return reinterpret_cast<Thread*>(thread);
  }

  static Thread* FromManagedThread(const ScopedObjectAccessUnchecked& ts, Object* thread_peer)
      LOCKS_EXCLUDED(Locks::thread_suspend_count_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static Thread* FromManagedThread(const ScopedObjectAccessUnchecked& ts, jobject thread)
      LOCKS_EXCLUDED(Locks::thread_suspend_count_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Translates 172 to pAllocArrayFromCode and so on.
  static void DumpThreadOffset(std::ostream& os, uint32_t offset, size_t size_of_pointers);

  // Dumps a one-line summary of thread state (used for operator<<).
  void ShortDump(std::ostream& os) const;

  // Dumps the detailed thread state and the thread stack (used for SIGQUIT).
  void Dump(std::ostream& os) const
      LOCKS_EXCLUDED(Locks::thread_suspend_count_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Dumps the SIGQUIT per-thread header. 'thread' can be NULL for a non-attached thread, in which
  // case we use 'tid' to identify the thread, and we'll include as much information as we can.
  static void DumpState(std::ostream& os, const Thread* thread, pid_t tid)
      LOCKS_EXCLUDED(Locks::thread_suspend_count_lock_);

  ThreadState GetState() const
      EXCLUSIVE_LOCKS_REQUIRED(Locks::thread_suspend_count_lock_) {
    Locks::thread_suspend_count_lock_->AssertHeld();
    return state_;
  }

  ThreadState SetState(ThreadState new_state)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::thread_suspend_count_lock_) {
    Locks::thread_suspend_count_lock_->AssertHeld();
    ThreadState old_state = state_;
    if (new_state == kRunnable) {
      // Sanity, should never become runnable with a pending suspension and should always hold
      // share of mutator_lock_.
      CHECK_EQ(GetSuspendCount(), 0);
      Locks::mutator_lock_->AssertSharedHeld();
    }
    state_ = new_state;
    return old_state;
  }

  int GetSuspendCount() const
      EXCLUSIVE_LOCKS_REQUIRED(Locks::thread_suspend_count_lock_) {
    Locks::thread_suspend_count_lock_->AssertHeld();
    return suspend_count_;
  }

  int GetDebugSuspendCount() const
      EXCLUSIVE_LOCKS_REQUIRED(Locks::thread_suspend_count_lock_) {
    Locks::thread_suspend_count_lock_->AssertHeld();
    return debug_suspend_count_;
  }

  bool IsSuspended() const
      EXCLUSIVE_LOCKS_REQUIRED(Locks::thread_suspend_count_lock_) {
    int suspend_count = GetSuspendCount();
    return suspend_count != 0 && GetState() != kRunnable;
  }

  void ModifySuspendCount(int delta, bool for_debugger)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::thread_suspend_count_lock_);

  // Called when thread detected that the thread_suspend_count_ was non-zero. Gives up share of
  // mutator_lock_ and waits until it is resumed and thread_suspend_count_ is zero.
  void FullSuspendCheck()
      LOCKS_EXCLUDED(Locks::thread_suspend_count_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Transition from non-runnable to runnable state acquiring share on mutator_lock_.
  ThreadState TransitionFromSuspendedToRunnable()
      LOCKS_EXCLUDED(Locks::thread_suspend_count_lock_)
      SHARED_LOCK_FUNCTION(Locks::mutator_lock_);

  // Transition from runnable into a state where mutator privileges are denied. Releases share of
  // mutator lock.
  void TransitionFromRunnableToSuspended(ThreadState new_state)
      LOCKS_EXCLUDED(Locks::thread_suspend_count_lock_)
      UNLOCK_FUNCTION(Locks::mutator_lock_);

  // Wait for a debugger suspension on the thread associated with the given peer. Returns the
  // thread on success, else NULL. If the thread should be suspended then request_suspension should
  // be true on entry. If the suspension times out then *timeout is set to true.
  static Thread* SuspendForDebugger(jobject peer,  bool request_suspension, bool* timeout)
      LOCKS_EXCLUDED(Locks::mutator_lock_,
                     Locks::thread_list_lock_,
                     Locks::thread_suspend_count_lock_);

  // Once called thread suspension will cause an assertion failure.
#ifndef NDEBUG
  const char* StartAssertNoThreadSuspension(const char* cause) {
    CHECK(cause != NULL);
    const char* previous_cause = last_no_thread_suspension_cause_;
    no_thread_suspension_++;
    last_no_thread_suspension_cause_ = cause;
    return previous_cause;
  }
#else
  const char* StartAssertNoThreadSuspension(const char* cause) {
    CHECK(cause != NULL);
    return NULL;
  }
#endif

  // End region where no thread suspension is expected.
#ifndef NDEBUG
  void EndAssertNoThreadSuspension(const char* old_cause) {
    CHECK(old_cause != NULL || no_thread_suspension_ == 1);
    CHECK_GT(no_thread_suspension_, 0U);
    no_thread_suspension_--;
    last_no_thread_suspension_cause_ = old_cause;
  }
#else
  void EndAssertNoThreadSuspension(const char*) {
  }
#endif


#ifndef NDEBUG
  void AssertThreadSuspensionIsAllowable(bool check_locks = true) const;
#else
  void AssertThreadSuspensionIsAllowable(bool check_locks = true) const {
    check_locks = !check_locks;  // Keep GCC happy about unused parameters.
  }
#endif

  bool CanAccessDirectReferences() const {
#ifdef MOVING_GARBAGE_COLLECTOR
    // TODO: when we have a moving collector, we'll need: return state_ == kRunnable;
#endif
    return true;
  }

  bool IsDaemon() const {
    return daemon_;
  }

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

  uint32_t GetThinLockId() const {
    return thin_lock_id_;
  }

  pid_t GetTid() const {
    return tid_;
  }

  // Returns the java.lang.Thread's name, or NULL if this Thread* doesn't have a peer.
  String* GetThreadName(const ScopedObjectAccessUnchecked& ts) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Sets 'name' to the java.lang.Thread's name. This requires no transition to managed code,
  // allocation, or locking.
  void GetThreadName(std::string& name) const;

  // Sets the thread's name.
  void SetThreadName(const char* name) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  Object* GetPeer() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return peer_;
  }

  bool HasPeer() const {
    return peer_ != NULL;
  }

  Object* GetThreadGroup(const ScopedObjectAccessUnchecked& ts) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  RuntimeStats* GetStats() {
    return &stats_;
  }

  bool IsStillStarting() const;

  bool IsExceptionPending() const {
    return exception_ != NULL;
  }

  Throwable* GetException() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(CanAccessDirectReferences());
    return exception_;
  }

  void AssertNoPendingException() const;

  void SetException(Throwable* new_exception)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(CanAccessDirectReferences());
    CHECK(new_exception != NULL);
    // TODO: CHECK(exception_ == NULL);
    exception_ = new_exception;  // TODO
  }

  void ClearException() {
    exception_ = NULL;
  }

  // Find catch block and perform long jump to appropriate exception handle
  void DeliverException() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  Context* GetLongJumpContext();
  void ReleaseLongJumpContext(Context* context) {
    DCHECK(long_jump_context_ == NULL);
    long_jump_context_ = context;
  }

  Method* GetCurrentMethod(uint32_t* dex_pc = NULL, size_t* frame_id = NULL) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void SetTopOfStack(void* stack, uintptr_t pc) {
    Method** top_method = reinterpret_cast<Method**>(stack);
    managed_stack_.SetTopQuickFrame(top_method);
    managed_stack_.SetTopQuickFramePc(pc);
  }

  bool HasManagedStack() const {
    return managed_stack_.GetTopQuickFrame() != NULL || managed_stack_.GetTopShadowFrame() != NULL;
  }

  // If 'msg' is NULL, no detail message is set.
  void ThrowNewException(const char* exception_class_descriptor, const char* msg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // If 'msg' is NULL, no detail message is set. An exception must be pending, and will be
  // used as the new exception's cause.
  void ThrowNewWrappedException(const char* exception_class_descriptor, const char* msg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void ThrowNewExceptionF(const char* exception_class_descriptor, const char* fmt, ...)
      __attribute__((format(printf, 3, 4)))
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void ThrowNewExceptionV(const char* exception_class_descriptor, const char* fmt, va_list ap)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // OutOfMemoryError is special, because we need to pre-allocate an instance.
  // Only the GC should call this.
  void ThrowOutOfMemoryError(const char* msg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  //QuickFrameIterator FindExceptionHandler(void* throw_pc, void** handler_pc);

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

  // Convert a jobject into a Object*
  Object* DecodeJObject(jobject obj)
      LOCKS_EXCLUDED(JavaVMExt::globals_lock,
                     JavaVMExt::weak_globals_lock)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

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

  ClassLoader* GetClassLoaderOverride()
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(CanAccessDirectReferences());
    return class_loader_override_;
  }

  void SetClassLoaderOverride(ClassLoader* class_loader_override) {
    class_loader_override_ = class_loader_override;
  }

  // Create the internal representation of a stack trace, that is more time
  // and space efficient to compute than the StackTraceElement[]
  jobject CreateInternalStackTrace(const ScopedObjectAccess& soa) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Convert an internal stack trace representation (returned by CreateInternalStackTrace) to a
  // StackTraceElement[]. If output_array is NULL, a new array is created, otherwise as many
  // frames as will fit are written into the given array. If stack_depth is non-NULL, it's updated
  // with the number of valid frames in the returned array.
  static jobjectArray InternalStackTraceToStackTraceElementArray(JNIEnv* env, jobject internal,
      jobjectArray output_array = NULL, int* stack_depth = NULL);

  void VisitRoots(Heap::RootVisitor* visitor, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

#if VERIFY_OBJECT_ENABLED
  void VerifyStack() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
#else
  void VerifyStack() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_){}
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
  void SetStackEndForStackOverflow() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

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
    return ThreadOffset(OFFSETOF_MEMBER(Thread, managed_stack_) +
                        ManagedStack::TopQuickFrameOffset());
  }

  static ThreadOffset TopOfManagedStackPcOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, managed_stack_) +
                        ManagedStack::TopQuickFramePcOffset());
  }

  const ManagedStack* GetManagedStack() const {
    return &managed_stack_;
  }

  // Linked list recording fragments of managed stack.
  void PushManagedStackFragment(ManagedStack* fragment) {
    managed_stack_.PushManagedStackFragment(fragment);
  }
  void PopManagedStackFragment(const ManagedStack& fragment) {
    managed_stack_.PopManagedStackFragment(fragment);
  }

  ShadowFrame* PushShadowFrame(ShadowFrame* new_top_frame) {
    return managed_stack_.PushShadowFrame(new_top_frame);
  }

  ShadowFrame* PopShadowFrame() {
    return managed_stack_.PopShadowFrame();
  }

  static ThreadOffset TopShadowFrameOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, managed_stack_) +
                        ManagedStack::TopShadowFrameOffset());
  }

  // Number of references allocated in ShadowFrames on this thread
  size_t NumShadowFrameReferences() const {
    return managed_stack_.NumShadowFrameReferences();
  }

  // Number of references in SIRTs on this thread
  size_t NumSirtReferences();

  // Number of references allocated in SIRTs & shadow frames on this thread
  size_t NumStackReferences() {
    return NumSirtReferences() + NumShadowFrameReferences();
  };

  // Is the given obj in this thread's stack indirect reference table?
  bool SirtContains(jobject obj);

  void SirtVisitRoots(Heap::RootVisitor* visitor, void* arg);

  void PushSirt(StackIndirectReferenceTable* sirt);
  StackIndirectReferenceTable* PopSirt();

  static ThreadOffset TopSirtOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, top_sirt_));
  }

  DebugInvokeReq* GetInvokeReq() {
    return debug_invoke_req_;
  }

  void SetDebuggerUpdatesEnabled(bool enabled);

  const std::vector<TraceStackFrame>* GetTraceStack() const {
    return trace_stack_;
  }

  bool IsTraceStackEmpty() const {
    return trace_stack_->empty();
  }

  void PushTraceStackFrame(const TraceStackFrame& frame) {
    trace_stack_->push_back(frame);
  }

  TraceStackFrame PopTraceStackFrame() {
    TraceStackFrame frame = trace_stack_->back();
    trace_stack_->pop_back();
    return frame;
  }

  BaseMutex* GetHeldMutex(MutexLevel level) const {
    return held_mutexes_[level];
  }

  void SetHeldMutex(MutexLevel level, BaseMutex* mutex) {
    held_mutexes_[level] = mutex;
  }

 private:
  // We have no control over the size of 'bool', but want our boolean fields
  // to be 4-byte quantities.
  typedef uint32_t bool32_t;

  explicit Thread(bool daemon);
  ~Thread() LOCKS_EXCLUDED(Locks::mutator_lock_,
                           Locks::thread_suspend_count_lock_);
  void Destroy();
  friend class ThreadList;  // For ~Thread and Destroy.

  void CreatePeer(const char* name, bool as_daemon, jobject thread_group);
  friend class Runtime; // For CreatePeer.

  // TODO: remove, callers should use GetState and hold the appropriate locks. Used only by
  //       ShortDump, TransitionFromSuspendedToRunnable and ScopedThreadStateChange.
  ThreadState GetStateUnsafe() const NO_THREAD_SAFETY_ANALYSIS {
    return state_;
  }

  // TODO: remove, callers should use SetState and hold the appropriate locks. Used only by
  //       TransitionFromRunnableToSuspended and ScopedThreadStateChange that don't need to observe
  //       suspend counts in situations where they know that the thread is already suspended.
  ThreadState SetStateUnsafe(ThreadState new_state) NO_THREAD_SAFETY_ANALYSIS {
    ThreadState old_state = state_;
    state_ = new_state;
    return old_state;
  }

  // TODO: remove, callers should use GetSuspendCount and hold the appropriate locks. Used only by
  //       TransitionFromSuspendedToRunnable that covers any data race. Note, this call is similar
  //       to the reads done in managed code.
  int GetSuspendCountUnsafe() const NO_THREAD_SAFETY_ANALYSIS {
    return suspend_count_;
  }

  void DumpState(std::ostream& os) const;
  void DumpStack(std::ostream& os) const
      LOCKS_EXCLUDED(Locks::thread_suspend_count_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Out-of-line conveniences for debugging in gdb.
  static Thread* CurrentFromGdb(); // Like Thread::Current.
  // Like Thread::Dump(std::cerr).
  void DumpFromGdb() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static void* CreateCallback(void* arg);

  void HandleUncaughtExceptions(const ScopedObjectAccess& soa)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void RemoveFromThreadGroup(const ScopedObjectAccess& soa)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void Init();
  void InitCardTable();
  void InitCpu();
  void InitFunctionPointers();
  void InitTid();
  void InitPthreadKeySelf();
  void InitStackHwm();

  void NotifyLocked() EXCLUSIVE_LOCKS_REQUIRED(wait_mutex_) {
    if (wait_monitor_ != NULL) {
      wait_cond_->Signal();
    }
  }

  static void ThreadExitCallback(void* arg);

  // TLS key used to retrieve the Thread*.
  static pthread_key_t pthread_key_self_;

  // Used to notify threads that they should attempt to resume, they will suspend again if
  // their suspend count is > 0.
  static ConditionVariable* resume_cond_
      GUARDED_BY(Locks::thread_suspend_count_lock_);

  // --- Frequently accessed fields first for short offsets ---

  // A non-zero value is used to tell the current thread to enter a safe point
  // at the next poll.
  int suspend_count_ GUARDED_BY(Locks::thread_suspend_count_lock_);

  // The biased card table, see CardTable for details
  byte* card_table_;

  // The pending exception or NULL.
  Throwable* exception_;

  // The end of this thread's stack. This is the lowest safely-addressable address on the stack.
  // We leave extra space so there's room for the code that throws StackOverflowError.
  byte* stack_end_;

  // The top of the managed stack often manipulated directly by compiler generated code.
  ManagedStack managed_stack_;

  // Every thread may have an associated JNI environment
  JNIEnvExt* jni_env_;

  // Initialized to "this". On certain architectures (such as x86) reading
  // off of Thread::Current is easy but getting the address of Thread::Current
  // is hard. This field can be read off of Thread::Current to give the address.
  Thread* self_;

  volatile ThreadState state_ GUARDED_BY(Locks::thread_suspend_count_lock_);

  // Our managed peer (an instance of java.lang.Thread).
  Object* peer_;

  // The "lowest addressable byte" of the stack
  byte* stack_begin_;

  // Size of the stack
  size_t stack_size_;

  // Thin lock thread id. This is a small integer used by the thin lock implementation.
  // This is not to be confused with the native thread's tid, nor is it the value returned
  // by java.lang.Thread.getId --- this is a distinct value, used only for locking. One
  // important difference between this id and the ids visible to managed code is that these
  // ones get reused (to ensure that they fit in the number of bits available).
  uint32_t thin_lock_id_;

  // System thread id.
  pid_t tid_;

  // Guards the 'interrupted_' and 'wait_monitor_' members.
  mutable Mutex* wait_mutex_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  ConditionVariable* wait_cond_ GUARDED_BY(wait_mutex_);
  // Pointer to the monitor lock we're currently waiting on (or NULL).
  Monitor* wait_monitor_ GUARDED_BY(wait_mutex_);
  // Thread "interrupted" status; stays raised until queried or thrown.
  bool32_t interrupted_ GUARDED_BY(wait_mutex_);
  // The next thread in the wait set this thread is part of.
  Thread* wait_next_;
  // If we're blocked in MonitorEnter, this is the object we're trying to lock.
  Object* monitor_enter_object_;

  friend class Monitor;

  // Top of linked list of stack indirect reference tables or NULL for none
  StackIndirectReferenceTable* top_sirt_;

  Runtime* runtime_;

  RuntimeStats stats_;

  // Needed to get the right ClassLoader in JNI_OnLoad, but also
  // useful for testing.
  ClassLoader* class_loader_override_;

  // Thread local, lazily allocated, long jump context. Used to deliver exceptions.
  Context* long_jump_context_;

  // A boolean telling us whether we're recursively throwing OOME.
  bool32_t throwing_OutOfMemoryError_;

  // How much of 'suspend_count_' is by request of the debugger, used to set things right
  // when the debugger detaches. Must be <= suspend_count_.
  int debug_suspend_count_ GUARDED_BY(Locks::thread_suspend_count_lock_);

  // JDWP invoke-during-breakpoint support.
  DebugInvokeReq* debug_invoke_req_;

  // Additional stack used by method tracer to store method and return pc values.
  // Stored as a pointer since std::vector is not PACKED.
  std::vector<TraceStackFrame>* trace_stack_;

  // A cached copy of the java.lang.Thread's name.
  std::string* name_;

  // Is the thread a daemon?
  const bool32_t daemon_;

  // A cached pthread_t for the pthread underlying this Thread*.
  pthread_t pthread_self_;

  // Support for Mutex lock hierarchy bug detection.
  BaseMutex* held_mutexes_[kMaxMutexLevel + 1];

  // A positive value implies we're in a region where thread suspension isn't expected.
  uint32_t no_thread_suspension_;

  // Cause for last suspension.
  const char* last_no_thread_suspension_cause_;

 public:
  // Runtime support function pointers
  // TODO: move this near the top, since changing its offset requires all oats to be recompiled!
  EntryPoints entrypoints_;
#ifdef ART_USE_GREENLAND_COMPILER
  RuntimeEntryPoints runtime_entry_points_;
#endif

 private:
  // How many times has our pthread key's destructor been called?
  uint32_t thread_exit_check_count_;

  friend class ScopedThreadStateChange;

  DISALLOW_COPY_AND_ASSIGN(Thread);
};

std::ostream& operator<<(std::ostream& os, const Thread& thread);
std::ostream& operator<<(std::ostream& os, const ThreadState& state);

}  // namespace art

#endif  // ART_SRC_THREAD_H_
