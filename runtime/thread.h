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

#ifndef ART_RUNTIME_THREAD_H_
#define ART_RUNTIME_THREAD_H_

#include <bitset>
#include <deque>
#include <iosfwd>
#include <list>
#include <string>

#include "base/macros.h"
#include "base/mutex.h"
#include "entrypoints/interpreter/interpreter_entrypoints.h"
#include "entrypoints/jni/jni_entrypoints.h"
#include "entrypoints/portable/portable_entrypoints.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "gc/allocator/rosalloc.h"
#include "globals.h"
#include "handle_scope.h"
#include "jvalue.h"
#include "object_callbacks.h"
#include "offsets.h"
#include "runtime_stats.h"
#include "stack.h"
#include "thread_state.h"
#include "throw_location.h"
#include "UniquePtr.h"

namespace art {

namespace gc {
namespace collector {
class SemiSpace;
}  // namespace collector
}  // namespace gc

namespace mirror {
  class ArtMethod;
  class Array;
  class Class;
  class ClassLoader;
  class Object;
  template<class T> class ObjectArray;
  template<class T> class PrimitiveArray;
  typedef PrimitiveArray<int32_t> IntArray;
  class StackTraceElement;
  class StaticStorageBase;
  class Throwable;
}  // namespace mirror
class BaseMutex;
class ClassLinker;
class Closure;
class Context;
struct DebugInvokeReq;
class DexFile;
class JavaVMExt;
struct JNIEnvExt;
class Monitor;
class Runtime;
class ScopedObjectAccess;
class ScopedObjectAccessUnchecked;
class ShadowFrame;
struct SingleStepControl;
class Thread;
class ThreadList;

// Thread priorities. These must match the Thread.MIN_PRIORITY,
// Thread.NORM_PRIORITY, and Thread.MAX_PRIORITY constants.
enum ThreadPriority {
  kMinThreadPriority = 1,
  kNormThreadPriority = 5,
  kMaxThreadPriority = 10,
};

enum ThreadFlag {
  kSuspendRequest   = 1,  // If set implies that suspend_count_ > 0 and the Thread should enter the
                          // safepoint handler.
  kCheckpointRequest = 2  // Request that the thread do some checkpoint work and then continue.
};

class Thread {
 public:
  // Space to throw a StackOverflowError in.
  // TODO: shrink reserved space, in particular for 64bit.
#if defined(__x86_64__)
  static constexpr size_t kStackOverflowReservedBytes = 24 * KB;
#elif defined(__aarch64__)
  // Worst-case, we would need about 2.6x the amount of x86_64 for many more registers.
  // But this one works rather well.
  static constexpr size_t kStackOverflowReservedBytes = 32 * KB;
#else
  static constexpr size_t kStackOverflowReservedBytes = 16 * KB;
#endif
  // How much of the reserved bytes is reserved for incoming signals.
  static constexpr size_t kStackOverflowSignalReservedBytes = 2 * KB;
  // How much of the reserved bytes we may temporarily use during stack overflow checks as an
  // optimization.
  static constexpr size_t kStackOverflowReservedUsableBytes =
      kStackOverflowReservedBytes - kStackOverflowSignalReservedBytes;

  // For implicit overflow checks we reserve an extra piece of memory at the bottom
  // of the stack (lowest memory).  The higher portion of the memory
  // is protected against reads and the lower is available for use while
  // throwing the StackOverflow exception.
  static constexpr size_t kStackOverflowProtectedSize = 32 * KB;
  static constexpr size_t kStackOverflowImplicitCheckSize = kStackOverflowProtectedSize +
    kStackOverflowReservedBytes;

  // Creates a new native thread corresponding to the given managed peer.
  // Used to implement Thread.start.
  static void CreateNativeThread(JNIEnv* env, jobject peer, size_t stack_size, bool daemon);

  // Attaches the calling native thread to the runtime, returning the new native peer.
  // Used to implement JNI AttachCurrentThread and AttachCurrentThreadAsDaemon calls.
  static Thread* Attach(const char* thread_name, bool as_daemon, jobject thread_group,
                        bool create_peer);

  // Reset internal state of child thread after fork.
  void InitAfterFork();

  static Thread* Current();

  static Thread* FromManagedThread(const ScopedObjectAccessUnchecked& ts,
                                   mirror::Object* thread_peer)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::thread_list_lock_)
      LOCKS_EXCLUDED(Locks::thread_suspend_count_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static Thread* FromManagedThread(const ScopedObjectAccessUnchecked& ts, jobject thread)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::thread_list_lock_)
      LOCKS_EXCLUDED(Locks::thread_suspend_count_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Translates 172 to pAllocArrayFromCode and so on.
  template<size_t size_of_pointers>
  static void DumpThreadOffset(std::ostream& os, uint32_t offset);

  // Dumps a one-line summary of thread state (used for operator<<).
  void ShortDump(std::ostream& os) const;

  // Dumps the detailed thread state and the thread stack (used for SIGQUIT).
  void Dump(std::ostream& os) const
      LOCKS_EXCLUDED(Locks::thread_suspend_count_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void DumpJavaStack(std::ostream& os) const
      LOCKS_EXCLUDED(Locks::thread_suspend_count_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Dumps the SIGQUIT per-thread header. 'thread' can be NULL for a non-attached thread, in which
  // case we use 'tid' to identify the thread, and we'll include as much information as we can.
  static void DumpState(std::ostream& os, const Thread* thread, pid_t tid)
      LOCKS_EXCLUDED(Locks::thread_suspend_count_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  ThreadState GetState() const {
    DCHECK_GE(tls32_.state_and_flags.as_struct.state, kTerminated);
    DCHECK_LE(tls32_.state_and_flags.as_struct.state, kSuspended);
    return static_cast<ThreadState>(tls32_.state_and_flags.as_struct.state);
  }

  ThreadState SetState(ThreadState new_state);

  int GetSuspendCount() const EXCLUSIVE_LOCKS_REQUIRED(Locks::thread_suspend_count_lock_) {
    return tls32_.suspend_count;
  }

  int GetDebugSuspendCount() const EXCLUSIVE_LOCKS_REQUIRED(Locks::thread_suspend_count_lock_) {
    return tls32_.debug_suspend_count;
  }

  bool IsSuspended() const {
    union StateAndFlags state_and_flags;
    state_and_flags.as_int = tls32_.state_and_flags.as_int;
    return state_and_flags.as_struct.state != kRunnable &&
        (state_and_flags.as_struct.flags & kSuspendRequest) != 0;
  }

  void ModifySuspendCount(Thread* self, int delta, bool for_debugger)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::thread_suspend_count_lock_);

  bool RequestCheckpoint(Closure* function)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::thread_suspend_count_lock_);

  // Called when thread detected that the thread_suspend_count_ was non-zero. Gives up share of
  // mutator_lock_ and waits until it is resumed and thread_suspend_count_ is zero.
  void FullSuspendCheck()
      LOCKS_EXCLUDED(Locks::thread_suspend_count_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Transition from non-runnable to runnable state acquiring share on mutator_lock_.
  ThreadState TransitionFromSuspendedToRunnable()
      LOCKS_EXCLUDED(Locks::thread_suspend_count_lock_)
      SHARED_LOCK_FUNCTION(Locks::mutator_lock_)
      ALWAYS_INLINE;

  // Transition from runnable into a state where mutator privileges are denied. Releases share of
  // mutator lock.
  void TransitionFromRunnableToSuspended(ThreadState new_state)
      LOCKS_EXCLUDED(Locks::thread_suspend_count_lock_)
      UNLOCK_FUNCTION(Locks::mutator_lock_)
      ALWAYS_INLINE;

  // Once called thread suspension will cause an assertion failure.
  const char* StartAssertNoThreadSuspension(const char* cause) {
    if (kIsDebugBuild) {
      CHECK(cause != NULL);
      const char* previous_cause = tlsPtr_.last_no_thread_suspension_cause;
      tls32_.no_thread_suspension++;
      tlsPtr_.last_no_thread_suspension_cause = cause;
      return previous_cause;
    } else {
      return nullptr;
    }
  }

  // End region where no thread suspension is expected.
  void EndAssertNoThreadSuspension(const char* old_cause) {
    if (kIsDebugBuild) {
      CHECK(old_cause != nullptr || tls32_.no_thread_suspension == 1);
      CHECK_GT(tls32_.no_thread_suspension, 0U);
      tls32_.no_thread_suspension--;
      tlsPtr_.last_no_thread_suspension_cause = old_cause;
    }
  }

  void AssertThreadSuspensionIsAllowable(bool check_locks = true) const;

  bool IsDaemon() const {
    return tls32_.daemon;
  }

  bool HoldsLock(mirror::Object*) const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

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

  uint32_t GetThreadId() const {
    return tls32_.thin_lock_thread_id;
  }

  pid_t GetTid() const {
    return tls32_.tid;
  }

  // Returns the java.lang.Thread's name, or NULL if this Thread* doesn't have a peer.
  mirror::String* GetThreadName(const ScopedObjectAccessUnchecked& ts) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Sets 'name' to the java.lang.Thread's name. This requires no transition to managed code,
  // allocation, or locking.
  void GetThreadName(std::string& name) const;

  // Sets the thread's name.
  void SetThreadName(const char* name) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Returns the thread-specific CPU-time clock in microseconds or -1 if unavailable.
  uint64_t GetCpuMicroTime() const;

  mirror::Object* GetPeer() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    CHECK(tlsPtr_.jpeer == nullptr);
    return tlsPtr_.opeer;
  }

  bool HasPeer() const {
    return tlsPtr_.jpeer != nullptr || tlsPtr_.opeer != nullptr;
  }

  RuntimeStats* GetStats() {
    return &tls64_.stats;
  }

  bool IsStillStarting() const;

  bool IsExceptionPending() const {
    return tlsPtr_.exception != nullptr;
  }

  mirror::Throwable* GetException(ThrowLocation* throw_location) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    if (throw_location != nullptr) {
      *throw_location = tlsPtr_.throw_location;
    }
    return tlsPtr_.exception;
  }

  void AssertNoPendingException() const;
  void AssertNoPendingExceptionForNewException(const char* msg) const;

  void SetException(const ThrowLocation& throw_location, mirror::Throwable* new_exception)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    CHECK(new_exception != NULL);
    // TODO: DCHECK(!IsExceptionPending());
    tlsPtr_.exception = new_exception;
    tlsPtr_.throw_location = throw_location;
  }

  void ClearException() {
    tlsPtr_.exception = nullptr;
    tlsPtr_.throw_location.Clear();
  }

  // Find catch block and perform long jump to appropriate exception handle
  void QuickDeliverException() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  Context* GetLongJumpContext();
  void ReleaseLongJumpContext(Context* context) {
    DCHECK(tlsPtr_.long_jump_context == nullptr);
    tlsPtr_.long_jump_context = context;
  }

  mirror::ArtMethod* GetCurrentMethod(uint32_t* dex_pc) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  ThrowLocation GetCurrentLocationForThrow() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void SetTopOfStack(mirror::ArtMethod** top_method, uintptr_t pc) {
    tlsPtr_.managed_stack.SetTopQuickFrame(top_method);
    tlsPtr_.managed_stack.SetTopQuickFramePc(pc);
  }

  void SetTopOfShadowStack(ShadowFrame* top) {
    tlsPtr_.managed_stack.SetTopShadowFrame(top);
  }

  bool HasManagedStack() const {
    return (tlsPtr_.managed_stack.GetTopQuickFrame() != nullptr) ||
        (tlsPtr_.managed_stack.GetTopShadowFrame() != nullptr);
  }

  // If 'msg' is NULL, no detail message is set.
  void ThrowNewException(const ThrowLocation& throw_location,
                         const char* exception_class_descriptor, const char* msg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // If 'msg' is NULL, no detail message is set. An exception must be pending, and will be
  // used as the new exception's cause.
  void ThrowNewWrappedException(const ThrowLocation& throw_location,
                                const char* exception_class_descriptor,
                                const char* msg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void ThrowNewExceptionF(const ThrowLocation& throw_location,
                          const char* exception_class_descriptor, const char* fmt, ...)
      __attribute__((format(printf, 4, 5)))
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void ThrowNewExceptionV(const ThrowLocation& throw_location,
                          const char* exception_class_descriptor, const char* fmt, va_list ap)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // OutOfMemoryError is special, because we need to pre-allocate an instance.
  // Only the GC should call this.
  void ThrowOutOfMemoryError(const char* msg) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static void Startup();
  static void FinishStartup();
  static void Shutdown();

  // JNI methods
  JNIEnvExt* GetJniEnv() const {
    return tlsPtr_.jni_env;
  }

  // Convert a jobject into a Object*
  mirror::Object* DecodeJObject(jobject obj) const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  mirror::Object* GetMonitorEnterObject() const {
    return tlsPtr_.monitor_enter_object;
  }

  void SetMonitorEnterObject(mirror::Object* obj) {
    tlsPtr_.monitor_enter_object = obj;
  }

  // Implements java.lang.Thread.interrupted.
  bool Interrupted() LOCKS_EXCLUDED(wait_mutex_);
  // Implements java.lang.Thread.isInterrupted.
  bool IsInterrupted() LOCKS_EXCLUDED(wait_mutex_);
  bool IsInterruptedLocked() EXCLUSIVE_LOCKS_REQUIRED(wait_mutex_) {
    return interrupted_;
  }
  void Interrupt(Thread* self) LOCKS_EXCLUDED(wait_mutex_);
  void SetInterruptedLocked(bool i) EXCLUSIVE_LOCKS_REQUIRED(wait_mutex_) {
    interrupted_ = i;
  }
  void Notify() LOCKS_EXCLUDED(wait_mutex_);

 private:
  void NotifyLocked(Thread* self) EXCLUSIVE_LOCKS_REQUIRED(wait_mutex_);

 public:
  Mutex* GetWaitMutex() const LOCK_RETURNED(wait_mutex_) {
    return wait_mutex_;
  }

  ConditionVariable* GetWaitConditionVariable() const EXCLUSIVE_LOCKS_REQUIRED(wait_mutex_) {
    return wait_cond_;
  }

  Monitor* GetWaitMonitor() const EXCLUSIVE_LOCKS_REQUIRED(wait_mutex_) {
    return wait_monitor_;
  }

  void SetWaitMonitor(Monitor* mon) EXCLUSIVE_LOCKS_REQUIRED(wait_mutex_) {
    wait_monitor_ = mon;
  }


  // Waiter link-list support.
  Thread* GetWaitNext() const {
    return tlsPtr_.wait_next;
  }

  void SetWaitNext(Thread* next) {
    tlsPtr_.wait_next = next;
  }

  mirror::ClassLoader* GetClassLoaderOverride() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return tlsPtr_.class_loader_override;
  }

  void SetClassLoaderOverride(mirror::ClassLoader* class_loader_override)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Create the internal representation of a stack trace, that is more time
  // and space efficient to compute than the StackTraceElement[].
  template<bool kTransactionActive>
  jobject CreateInternalStackTrace(const ScopedObjectAccessUnchecked& soa) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Convert an internal stack trace representation (returned by CreateInternalStackTrace) to a
  // StackTraceElement[]. If output_array is NULL, a new array is created, otherwise as many
  // frames as will fit are written into the given array. If stack_depth is non-NULL, it's updated
  // with the number of valid frames in the returned array.
  static jobjectArray InternalStackTraceToStackTraceElementArray(const ScopedObjectAccess& soa,
      jobject internal, jobjectArray output_array = nullptr, int* stack_depth = nullptr)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void VisitRoots(RootCallback* visitor, void* arg) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  ALWAYS_INLINE void VerifyStack() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  //
  // Offsets of various members of native Thread class, used by compiled code.
  //

  template<size_t pointer_size>
  static ThreadOffset<pointer_size> ThinLockIdOffset() {
    return ThreadOffset<pointer_size>(
        OFFSETOF_MEMBER(Thread, tls32_) +
        OFFSETOF_MEMBER(tls_32bit_sized_values, thin_lock_thread_id));
  }

  template<size_t pointer_size>
  static ThreadOffset<pointer_size> ThreadFlagsOffset() {
    return ThreadOffset<pointer_size>(
        OFFSETOF_MEMBER(Thread, tls32_) +
        OFFSETOF_MEMBER(tls_32bit_sized_values, state_and_flags));
  }

 private:
  template<size_t pointer_size>
  static ThreadOffset<pointer_size> ThreadOffsetFromTlsPtr(size_t tls_ptr_offset) {
    size_t base = OFFSETOF_MEMBER(Thread, tlsPtr_);
    size_t scale;
    size_t shrink;
    if (pointer_size == sizeof(void*)) {
      scale = 1;
      shrink = 1;
    } else if (pointer_size > sizeof(void*)) {
      scale = pointer_size / sizeof(void*);
      shrink = 1;
    } else {
      DCHECK_GT(sizeof(void*), pointer_size);
      scale = 1;
      shrink = sizeof(void*) / pointer_size;
    }
    return ThreadOffset<pointer_size>(base + ((tls_ptr_offset * scale) / shrink));
  }

 public:
  template<size_t pointer_size>
  static ThreadOffset<pointer_size> QuickEntryPointOffset(size_t quick_entrypoint_offset) {
    return ThreadOffsetFromTlsPtr<pointer_size>(
        OFFSETOF_MEMBER(tls_ptr_sized_values, quick_entrypoints) + quick_entrypoint_offset);
  }

  template<size_t pointer_size>
  static ThreadOffset<pointer_size> InterpreterEntryPointOffset(size_t interp_entrypoint_offset) {
    return ThreadOffsetFromTlsPtr<pointer_size>(
        OFFSETOF_MEMBER(tls_ptr_sized_values, interpreter_entrypoints) + interp_entrypoint_offset);
  }

  template<size_t pointer_size>
  static ThreadOffset<pointer_size> JniEntryPointOffset(size_t jni_entrypoint_offset) {
    return ThreadOffsetFromTlsPtr<pointer_size>(
        OFFSETOF_MEMBER(tls_ptr_sized_values, jni_entrypoints) + jni_entrypoint_offset);
  }

  template<size_t pointer_size>
  static ThreadOffset<pointer_size> PortableEntryPointOffset(size_t port_entrypoint_offset) {
    return ThreadOffsetFromTlsPtr<pointer_size>(
        OFFSETOF_MEMBER(tls_ptr_sized_values, portable_entrypoints) + port_entrypoint_offset);
  }

  template<size_t pointer_size>
  static ThreadOffset<pointer_size> SelfOffset() {
    return ThreadOffsetFromTlsPtr<pointer_size>(OFFSETOF_MEMBER(tls_ptr_sized_values, self));
  }

  template<size_t pointer_size>
  static ThreadOffset<pointer_size> ExceptionOffset() {
    return ThreadOffsetFromTlsPtr<pointer_size>(OFFSETOF_MEMBER(tls_ptr_sized_values, exception));
  }

  template<size_t pointer_size>
  static ThreadOffset<pointer_size> PeerOffset() {
    return ThreadOffsetFromTlsPtr<pointer_size>(OFFSETOF_MEMBER(tls_ptr_sized_values, opeer));
  }


  template<size_t pointer_size>
  static ThreadOffset<pointer_size> CardTableOffset() {
    return ThreadOffsetFromTlsPtr<pointer_size>(OFFSETOF_MEMBER(tls_ptr_sized_values, card_table));
  }

  template<size_t pointer_size>
  static ThreadOffset<pointer_size> ThreadSuspendTriggerOffset() {
    return ThreadOffsetFromTlsPtr<pointer_size>(
        OFFSETOF_MEMBER(tls_ptr_sized_values, suspend_trigger));
  }

  // Size of stack less any space reserved for stack overflow
  size_t GetStackSize() const {
    return tlsPtr_.stack_size - (tlsPtr_.stack_end - tlsPtr_.stack_begin);
  }

  byte* GetStackEnd() const {
    return tlsPtr_.stack_end;
  }

  // Set the stack end to that to be used during a stack overflow
  void SetStackEndForStackOverflow() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Set the stack end to that to be used during regular execution
  void ResetDefaultStackEnd(bool implicit_overflow_check) {
    // Our stacks grow down, so we want stack_end_ to be near there, but reserving enough room
    // to throw a StackOverflowError.
    if (implicit_overflow_check) {
      // For implicit checks we also need to add in the protected region above the
      // overflow region.
      tlsPtr_.stack_end = tlsPtr_.stack_begin + kStackOverflowImplicitCheckSize;
    } else {
      tlsPtr_.stack_end = tlsPtr_.stack_begin + kStackOverflowReservedBytes;
    }
  }

  // Install the protected region for implicit stack checks.
  void InstallImplicitProtection(bool is_main_stack);

  bool IsHandlingStackOverflow() const {
    return tlsPtr_.stack_end == tlsPtr_.stack_begin;
  }

  template<size_t pointer_size>
  static ThreadOffset<pointer_size> StackEndOffset() {
    return ThreadOffsetFromTlsPtr<pointer_size>(
        OFFSETOF_MEMBER(tls_ptr_sized_values, stack_end));
  }

  template<size_t pointer_size>
  static ThreadOffset<pointer_size> JniEnvOffset() {
    return ThreadOffsetFromTlsPtr<pointer_size>(
        OFFSETOF_MEMBER(tls_ptr_sized_values, jni_env));
  }

  template<size_t pointer_size>
  static ThreadOffset<pointer_size> TopOfManagedStackOffset() {
    return ThreadOffsetFromTlsPtr<pointer_size>(
        OFFSETOF_MEMBER(tls_ptr_sized_values, managed_stack) +
        ManagedStack::TopQuickFrameOffset());
  }

  template<size_t pointer_size>
  static ThreadOffset<pointer_size> TopOfManagedStackPcOffset() {
    return ThreadOffsetFromTlsPtr<pointer_size>(
        OFFSETOF_MEMBER(tls_ptr_sized_values, managed_stack) +
        ManagedStack::TopQuickFramePcOffset());
  }

  const ManagedStack* GetManagedStack() const {
    return &tlsPtr_.managed_stack;
  }

  // Linked list recording fragments of managed stack.
  void PushManagedStackFragment(ManagedStack* fragment) {
    tlsPtr_.managed_stack.PushManagedStackFragment(fragment);
  }
  void PopManagedStackFragment(const ManagedStack& fragment) {
    tlsPtr_.managed_stack.PopManagedStackFragment(fragment);
  }

  ShadowFrame* PushShadowFrame(ShadowFrame* new_top_frame) {
    return tlsPtr_.managed_stack.PushShadowFrame(new_top_frame);
  }

  ShadowFrame* PopShadowFrame() {
    return tlsPtr_.managed_stack.PopShadowFrame();
  }

  template<size_t pointer_size>
  static ThreadOffset<pointer_size> TopShadowFrameOffset() {
    return ThreadOffsetFromTlsPtr<pointer_size>(
        OFFSETOF_MEMBER(tls_ptr_sized_values, managed_stack) +
        ManagedStack::TopShadowFrameOffset());
  }

  // Number of references allocated in JNI ShadowFrames on this thread.
  size_t NumJniShadowFrameReferences() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return tlsPtr_.managed_stack.NumJniShadowFrameReferences();
  }

  // Number of references in handle scope on this thread.
  size_t NumHandleReferences();

  // Number of references allocated in handle scopes & JNI shadow frames on this thread.
  size_t NumStackReferences() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return NumHandleReferences() + NumJniShadowFrameReferences();
  };

  // Is the given obj in this thread's stack indirect reference table?
  bool HandleScopeContains(jobject obj) const;

  void HandleScopeVisitRoots(RootCallback* visitor, void* arg, uint32_t thread_id)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  HandleScope* GetTopHandleScope() {
    return tlsPtr_.top_handle_scope;
  }

  void PushHandleScope(HandleScope* handle_scope) {
    handle_scope->SetLink(tlsPtr_.top_handle_scope);
    tlsPtr_.top_handle_scope = handle_scope;
  }

  HandleScope* PopHandleScope() {
    HandleScope* handle_scope = tlsPtr_.top_handle_scope;
    DCHECK(handle_scope != nullptr);
    tlsPtr_.top_handle_scope = tlsPtr_.top_handle_scope->GetLink();
    return handle_scope;
  }

  template<size_t pointer_size>
  static ThreadOffset<pointer_size> TopHandleScopeOffset() {
    return ThreadOffsetFromTlsPtr<pointer_size>(OFFSETOF_MEMBER(tls_ptr_sized_values,
                                                                top_handle_scope));
  }

  DebugInvokeReq* GetInvokeReq() const {
    return tlsPtr_.debug_invoke_req;
  }

  SingleStepControl* GetSingleStepControl() const {
    return tlsPtr_.single_step_control;
  }

  // Returns the fake exception used to activate deoptimization.
  static mirror::Throwable* GetDeoptimizationException() {
    return reinterpret_cast<mirror::Throwable*>(-1);
  }

  void SetDeoptimizationShadowFrame(ShadowFrame* sf);
  void SetDeoptimizationReturnValue(const JValue& ret_val);

  ShadowFrame* GetAndClearDeoptimizationShadowFrame(JValue* ret_val);

  bool HasDeoptimizationShadowFrame() const {
    return tlsPtr_.deoptimization_shadow_frame != nullptr;
  }

  std::deque<instrumentation::InstrumentationStackFrame>* GetInstrumentationStack() {
    return tlsPtr_.instrumentation_stack;
  }

  std::vector<mirror::ArtMethod*>* GetStackTraceSample() const {
    return tlsPtr_.stack_trace_sample;
  }

  void SetStackTraceSample(std::vector<mirror::ArtMethod*>* sample) {
    tlsPtr_.stack_trace_sample = sample;
  }

  uint64_t GetTraceClockBase() const {
    return tls64_.trace_clock_base;
  }

  void SetTraceClockBase(uint64_t clock_base) {
    tls64_.trace_clock_base = clock_base;
  }

  BaseMutex* GetHeldMutex(LockLevel level) const {
    return tlsPtr_.held_mutexes[level];
  }

  void SetHeldMutex(LockLevel level, BaseMutex* mutex) {
    tlsPtr_.held_mutexes[level] = mutex;
  }

  void RunCheckpointFunction();

  bool ReadFlag(ThreadFlag flag) const {
    return (tls32_.state_and_flags.as_struct.flags & flag) != 0;
  }

  bool TestAllFlags() const {
    return (tls32_.state_and_flags.as_struct.flags != 0);
  }

  void AtomicSetFlag(ThreadFlag flag);

  void AtomicClearFlag(ThreadFlag flag);

  void ResetQuickAllocEntryPointsForThread();

  // Returns the remaining space in the TLAB.
  size_t TlabSize() const;
  // Doesn't check that there is room.
  mirror::Object* AllocTlab(size_t bytes);
  void SetTlab(byte* start, byte* end);
  bool HasTlab() const;

  // Remove the suspend trigger for this thread by making the suspend_trigger_ TLS value
  // equal to a valid pointer.
  // TODO: does this need to atomic?  I don't think so.
  void RemoveSuspendTrigger() {
    tlsPtr_.suspend_trigger = reinterpret_cast<uintptr_t*>(&tlsPtr_.suspend_trigger);
  }

  // Trigger a suspend check by making the suspend_trigger_ TLS value an invalid pointer.
  // The next time a suspend check is done, it will load from the value at this address
  // and trigger a SIGSEGV.
  void TriggerSuspend() {
    tlsPtr_.suspend_trigger = nullptr;
  }


  // Push an object onto the allocation stack.
  bool PushOnThreadLocalAllocationStack(mirror::Object* obj);

  // Set the thread local allocation pointers to the given pointers.
  void SetThreadLocalAllocationStack(mirror::Object** start, mirror::Object** end);

  // Resets the thread local allocation pointers.
  void RevokeThreadLocalAllocationStack();

  size_t GetThreadLocalBytesAllocated() const {
    return tlsPtr_.thread_local_pos - tlsPtr_.thread_local_start;
  }

  size_t GetThreadLocalObjectsAllocated() const {
    return tlsPtr_.thread_local_objects;
  }

  void* GetRosAllocRun(size_t index) const {
    return tlsPtr_.rosalloc_runs[index];
  }

  void SetRosAllocRun(size_t index, void* run) {
    tlsPtr_.rosalloc_runs[index] = run;
  }

 private:
  explicit Thread(bool daemon);
  ~Thread() LOCKS_EXCLUDED(Locks::mutator_lock_,
                           Locks::thread_suspend_count_lock_);
  void Destroy();

  void CreatePeer(const char* name, bool as_daemon, jobject thread_group);

  template<bool kTransactionActive>
  void InitPeer(ScopedObjectAccess& soa, jboolean thread_is_daemon, jobject thread_group,
                jobject thread_name, jint thread_priority)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Avoid use, callers should use SetState. Used only by SignalCatcher::HandleSigQuit, ~Thread and
  // Dbg::Disconnected.
  ThreadState SetStateUnsafe(ThreadState new_state) {
    ThreadState old_state = GetState();
    tls32_.state_and_flags.as_struct.state = new_state;
    return old_state;
  }

  void VerifyStackImpl() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void DumpState(std::ostream& os) const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void DumpStack(std::ostream& os) const
      LOCKS_EXCLUDED(Locks::thread_suspend_count_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Out-of-line conveniences for debugging in gdb.
  static Thread* CurrentFromGdb();  // Like Thread::Current.
  // Like Thread::Dump(std::cerr).
  void DumpFromGdb() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static void* CreateCallback(void* arg);

  void HandleUncaughtExceptions(ScopedObjectAccess& soa)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void RemoveFromThreadGroup(ScopedObjectAccess& soa) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void Init(ThreadList*, JavaVMExt*) EXCLUSIVE_LOCKS_REQUIRED(Locks::runtime_shutdown_lock_);
  void InitCardTable();
  void InitCpu();
  void CleanupCpu();
  void InitTlsEntryPoints();
  void InitTid();
  void InitPthreadKeySelf();
  void InitStackHwm();

  void SetUpAlternateSignalStack();
  void TearDownAlternateSignalStack();

  // 32 bits of atomically changed state and flags. Keeping as 32 bits allows and atomic CAS to
  // change from being Suspended to Runnable without a suspend request occurring.
  union PACKED(4) StateAndFlags {
    StateAndFlags() {}
    struct PACKED(4) {
      // Bitfield of flag values. Must be changed atomically so that flag values aren't lost. See
      // ThreadFlags for bit field meanings.
      volatile uint16_t flags;
      // Holds the ThreadState. May be changed non-atomically between Suspended (ie not Runnable)
      // transitions. Changing to Runnable requires that the suspend_request be part of the atomic
      // operation. If a thread is suspended and a suspend_request is present, a thread may not
      // change to Runnable as a GC or other operation is in progress.
      volatile uint16_t state;
    } as_struct;
    volatile int32_t as_int;

   private:
    // gcc does not handle struct with volatile member assignments correctly.
    // See http://gcc.gnu.org/bugzilla/show_bug.cgi?id=47409
    DISALLOW_COPY_AND_ASSIGN(StateAndFlags);
  };

  static void ThreadExitCallback(void* arg);

  // Maximum number of checkpoint functions.
  static constexpr uint32_t kMaxCheckpoints = 3;

  // Has Thread::Startup been called?
  static bool is_started_;

  // TLS key used to retrieve the Thread*.
  static pthread_key_t pthread_key_self_;

  // Used to notify threads that they should attempt to resume, they will suspend again if
  // their suspend count is > 0.
  static ConditionVariable* resume_cond_ GUARDED_BY(Locks::thread_suspend_count_lock_);

  /***********************************************************************************************/
  // Thread local storage. Fields are grouped by size to enable 32 <-> 64 searching to account for
  // pointer size differences. To encourage shorter encoding, more frequently used values appear
  // first if possible.
  /***********************************************************************************************/

  struct PACKED(4)  tls_32bit_sized_values {
    // We have no control over the size of 'bool', but want our boolean fields
    // to be 4-byte quantities.
    typedef uint32_t bool32_t;

    explicit tls_32bit_sized_values(bool is_daemon) :
      suspend_count(0), debug_suspend_count(0), thin_lock_thread_id(0), tid(0),
      daemon(is_daemon), throwing_OutOfMemoryError(false), no_thread_suspension(0),
      thread_exit_check_count(0) {
    }

    union StateAndFlags state_and_flags;
    COMPILE_ASSERT(sizeof(union StateAndFlags) == sizeof(int32_t),
                   sizeof_state_and_flags_and_int32_are_different);

    // A non-zero value is used to tell the current thread to enter a safe point
    // at the next poll.
    int suspend_count GUARDED_BY(Locks::thread_suspend_count_lock_);

    // How much of 'suspend_count_' is by request of the debugger, used to set things right
    // when the debugger detaches. Must be <= suspend_count_.
    int debug_suspend_count GUARDED_BY(Locks::thread_suspend_count_lock_);

    // Thin lock thread id. This is a small integer used by the thin lock implementation.
    // This is not to be confused with the native thread's tid, nor is it the value returned
    // by java.lang.Thread.getId --- this is a distinct value, used only for locking. One
    // important difference between this id and the ids visible to managed code is that these
    // ones get reused (to ensure that they fit in the number of bits available).
    uint32_t thin_lock_thread_id;

    // System thread id.
    uint32_t tid;

    // Is the thread a daemon?
    const bool32_t daemon;

    // A boolean telling us whether we're recursively throwing OOME.
    bool32_t throwing_OutOfMemoryError;

    // A positive value implies we're in a region where thread suspension isn't expected.
    uint32_t no_thread_suspension;

    // How many times has our pthread key's destructor been called?
    uint32_t thread_exit_check_count;
  } tls32_;

  struct PACKED(8) tls_64bit_sized_values {
    tls_64bit_sized_values() : trace_clock_base(0), deoptimization_return_value() {
    }

    // The clock base used for tracing.
    uint64_t trace_clock_base;

    // Return value used by deoptimization.
    JValue deoptimization_return_value;

    RuntimeStats stats;
  } tls64_;

  struct PACKED(4) tls_ptr_sized_values {
      tls_ptr_sized_values() : card_table(nullptr), exception(nullptr), stack_end(nullptr),
      managed_stack(), suspend_trigger(nullptr), jni_env(nullptr), self(nullptr), opeer(nullptr),
      jpeer(nullptr), stack_begin(nullptr), stack_size(0), throw_location(),
      stack_trace_sample(nullptr), wait_next(nullptr), monitor_enter_object(nullptr),
      top_handle_scope(nullptr), class_loader_override(nullptr), long_jump_context(nullptr),
      instrumentation_stack(nullptr), debug_invoke_req(nullptr), single_step_control(nullptr),
      deoptimization_shadow_frame(nullptr), name(nullptr), pthread_self(0),
      last_no_thread_suspension_cause(nullptr), thread_local_start(nullptr),
      thread_local_pos(nullptr), thread_local_end(nullptr), thread_local_objects(0),
      thread_local_alloc_stack_top(nullptr), thread_local_alloc_stack_end(nullptr) {
    }

    // The biased card table, see CardTable for details.
    byte* card_table;

    // The pending exception or NULL.
    mirror::Throwable* exception;

    // The end of this thread's stack. This is the lowest safely-addressable address on the stack.
    // We leave extra space so there's room for the code that throws StackOverflowError.
    byte* stack_end;

    // The top of the managed stack often manipulated directly by compiler generated code.
    ManagedStack managed_stack;

    // In certain modes, setting this to 0 will trigger a SEGV and thus a suspend check.  It is
    // normally set to the address of itself.
    uintptr_t* suspend_trigger;

    // Every thread may have an associated JNI environment
    JNIEnvExt* jni_env;

    // Initialized to "this". On certain architectures (such as x86) reading off of Thread::Current
    // is easy but getting the address of Thread::Current is hard. This field can be read off of
    // Thread::Current to give the address.
    Thread* self;

    // Our managed peer (an instance of java.lang.Thread). The jobject version is used during thread
    // start up, until the thread is registered and the local opeer_ is used.
    mirror::Object* opeer;
    jobject jpeer;

    // The "lowest addressable byte" of the stack.
    byte* stack_begin;

    // Size of the stack.
    size_t stack_size;

    // The location the current exception was thrown from.
    ThrowLocation throw_location;

    // Pointer to previous stack trace captured by sampling profiler.
    std::vector<mirror::ArtMethod*>* stack_trace_sample;

    // The next thread in the wait set this thread is part of or NULL if not waiting.
    Thread* wait_next;

    // If we're blocked in MonitorEnter, this is the object we're trying to lock.
    mirror::Object* monitor_enter_object;

    // Top of linked list of handle scopes or nullptr for none.
    HandleScope* top_handle_scope;

    // Needed to get the right ClassLoader in JNI_OnLoad, but also
    // useful for testing.
    mirror::ClassLoader* class_loader_override;

    // Thread local, lazily allocated, long jump context. Used to deliver exceptions.
    Context* long_jump_context;

    // Additional stack used by method instrumentation to store method and return pc values.
    // Stored as a pointer since std::deque is not PACKED.
    std::deque<instrumentation::InstrumentationStackFrame>* instrumentation_stack;

    // JDWP invoke-during-breakpoint support.
    DebugInvokeReq* debug_invoke_req;

    // JDWP single-stepping support.
    SingleStepControl* single_step_control;

    // Shadow frame stack that is used temporarily during the deoptimization of a method.
    ShadowFrame* deoptimization_shadow_frame;

    // A cached copy of the java.lang.Thread's name.
    std::string* name;

    // A cached pthread_t for the pthread underlying this Thread*.
    pthread_t pthread_self;

    // Support for Mutex lock hierarchy bug detection.
    BaseMutex* held_mutexes[kLockLevelCount];

    // If no_thread_suspension_ is > 0, what is causing that assertion.
    const char* last_no_thread_suspension_cause;

    // Pending checkpoint function or NULL if non-pending. Installation guarding by
    // Locks::thread_suspend_count_lock_.
    Closure* checkpoint_functions[kMaxCheckpoints];

    // Entrypoint function pointers.
    // TODO: move this to more of a global offset table model to avoid per-thread duplication.
    InterpreterEntryPoints interpreter_entrypoints;
    JniEntryPoints jni_entrypoints;
    PortableEntryPoints portable_entrypoints;
    QuickEntryPoints quick_entrypoints;

    // Thread-local allocation pointer.
    byte* thread_local_start;
    byte* thread_local_pos;
    byte* thread_local_end;
    size_t thread_local_objects;

    // There are RosAlloc::kNumThreadLocalSizeBrackets thread-local size brackets per thread.
    void* rosalloc_runs[gc::allocator::RosAlloc::kNumThreadLocalSizeBrackets];

    // Thread-local allocation stack data/routines.
    mirror::Object** thread_local_alloc_stack_top;
    mirror::Object** thread_local_alloc_stack_end;
  } tlsPtr_;

  // Guards the 'interrupted_' and 'wait_monitor_' members.
  Mutex* wait_mutex_ DEFAULT_MUTEX_ACQUIRED_AFTER;

  // Condition variable waited upon during a wait.
  ConditionVariable* wait_cond_ GUARDED_BY(wait_mutex_);
  // Pointer to the monitor lock we're currently waiting on or NULL if not waiting.
  Monitor* wait_monitor_ GUARDED_BY(wait_mutex_);

  // Thread "interrupted" status; stays raised until queried or thrown.
  bool interrupted_ GUARDED_BY(wait_mutex_);

  friend class Dbg;  // For SetStateUnsafe.
  friend class gc::collector::SemiSpace;  // For getting stack traces.
  friend class Runtime;  // For CreatePeer.
  friend class ScopedThreadStateChange;
  friend class SignalCatcher;  // For SetStateUnsafe.
  friend class StubTest;  // For accessing entrypoints.
  friend class ThreadList;  // For ~Thread and Destroy.

  DISALLOW_COPY_AND_ASSIGN(Thread);
};

std::ostream& operator<<(std::ostream& os, const Thread& thread);
std::ostream& operator<<(std::ostream& os, const ThreadState& state);

}  // namespace art

#endif  // ART_RUNTIME_THREAD_H_
