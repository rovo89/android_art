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

#define ATRACE_TAG ATRACE_TAG_DALVIK

#include "thread.h"

#include <cutils/trace.h>
#include <pthread.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/time.h>

#include <algorithm>
#include <bitset>
#include <cerrno>
#include <iostream>
#include <list>

#include "arch/context.h"
#include "base/mutex.h"
#include "class_linker.h"
#include "class_linker-inl.h"
#include "cutils/atomic.h"
#include "cutils/atomic-inline.h"
#include "debugger.h"
#include "dex_file-inl.h"
#include "entrypoints/entrypoint_utils.h"
#include "entrypoints/quick/quick_alloc_entrypoints.h"
#include "gc_map.h"
#include "gc/accounting/card_table-inl.h"
#include "gc/heap.h"
#include "gc/space/space.h"
#include "handle_scope.h"
#include "indirect_reference_table-inl.h"
#include "jni_internal.h"
#include "mirror/art_field-inl.h"
#include "mirror/art_method-inl.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "mirror/object_array-inl.h"
#include "mirror/stack_trace_element.h"
#include "monitor.h"
#include "object_utils.h"
#include "quick_exception_handler.h"
#include "quick/quick_method_frame_info.h"
#include "reflection.h"
#include "runtime.h"
#include "scoped_thread_state_change.h"
#include "ScopedLocalRef.h"
#include "ScopedUtfChars.h"
#include "handle_scope-inl.h"
#include "stack.h"
#include "thread-inl.h"
#include "thread_list.h"
#include "utils.h"
#include "verifier/dex_gc_map.h"
#include "verify_object-inl.h"
#include "vmap_table.h"
#include "well_known_classes.h"

namespace art {

bool Thread::is_started_ = false;
pthread_key_t Thread::pthread_key_self_;
ConditionVariable* Thread::resume_cond_ = nullptr;

static const char* kThreadNameDuringStartup = "<native thread without managed peer>";

void Thread::InitCardTable() {
  tlsPtr_.card_table = Runtime::Current()->GetHeap()->GetCardTable()->GetBiasedBegin();
}

static void UnimplementedEntryPoint() {
  UNIMPLEMENTED(FATAL);
}

void InitEntryPoints(InterpreterEntryPoints* ipoints, JniEntryPoints* jpoints,
                     PortableEntryPoints* ppoints, QuickEntryPoints* qpoints);

void Thread::InitTlsEntryPoints() {
  // Insert a placeholder so we can easily tell if we call an unimplemented entry point.
  uintptr_t* begin = reinterpret_cast<uintptr_t*>(&tlsPtr_.interpreter_entrypoints);
  uintptr_t* end = reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(begin) +
                                                sizeof(tlsPtr_.quick_entrypoints));
  for (uintptr_t* it = begin; it != end; ++it) {
    *it = reinterpret_cast<uintptr_t>(UnimplementedEntryPoint);
  }
  InitEntryPoints(&tlsPtr_.interpreter_entrypoints, &tlsPtr_.jni_entrypoints,
                  &tlsPtr_.portable_entrypoints, &tlsPtr_.quick_entrypoints);
}

void Thread::ResetQuickAllocEntryPointsForThread() {
  ResetQuickAllocEntryPoints(&tlsPtr_.quick_entrypoints);
}

void Thread::SetDeoptimizationShadowFrame(ShadowFrame* sf) {
  tlsPtr_.deoptimization_shadow_frame = sf;
}

void Thread::SetDeoptimizationReturnValue(const JValue& ret_val) {
  tls64_.deoptimization_return_value.SetJ(ret_val.GetJ());
}

ShadowFrame* Thread::GetAndClearDeoptimizationShadowFrame(JValue* ret_val) {
  ShadowFrame* sf = tlsPtr_.deoptimization_shadow_frame;
  tlsPtr_.deoptimization_shadow_frame = nullptr;
  ret_val->SetJ(tls64_.deoptimization_return_value.GetJ());
  return sf;
}

void Thread::InitTid() {
  tls32_.tid = ::art::GetTid();
}

void Thread::InitAfterFork() {
  // One thread (us) survived the fork, but we have a new tid so we need to
  // update the value stashed in this Thread*.
  InitTid();
}

void* Thread::CreateCallback(void* arg) {
  Thread* self = reinterpret_cast<Thread*>(arg);
  Runtime* runtime = Runtime::Current();
  if (runtime == nullptr) {
    LOG(ERROR) << "Thread attaching to non-existent runtime: " << *self;
    return nullptr;
  }
  {
    // TODO: pass self to MutexLock - requires self to equal Thread::Current(), which is only true
    //       after self->Init().
    MutexLock mu(nullptr, *Locks::runtime_shutdown_lock_);
    // Check that if we got here we cannot be shutting down (as shutdown should never have started
    // while threads are being born).
    CHECK(!runtime->IsShuttingDownLocked());
    self->Init(runtime->GetThreadList(), runtime->GetJavaVM());
    Runtime::Current()->EndThreadBirth();
  }
  {
    ScopedObjectAccess soa(self);

    // Copy peer into self, deleting global reference when done.
    CHECK(self->tlsPtr_.jpeer != nullptr);
    self->tlsPtr_.opeer = soa.Decode<mirror::Object*>(self->tlsPtr_.jpeer);
    self->GetJniEnv()->DeleteGlobalRef(self->tlsPtr_.jpeer);
    self->tlsPtr_.jpeer = nullptr;
    self->SetThreadName(self->GetThreadName(soa)->ToModifiedUtf8().c_str());
    Dbg::PostThreadStart(self);

    // Invoke the 'run' method of our java.lang.Thread.
    mirror::Object* receiver = self->tlsPtr_.opeer;
    jmethodID mid = WellKnownClasses::java_lang_Thread_run;
    InvokeVirtualOrInterfaceWithJValues(soa, receiver, mid, nullptr);
  }
  // Detach and delete self.
  Runtime::Current()->GetThreadList()->Unregister(self);

  return nullptr;
}

Thread* Thread::FromManagedThread(const ScopedObjectAccessUnchecked& soa,
                                  mirror::Object* thread_peer) {
  mirror::ArtField* f = soa.DecodeField(WellKnownClasses::java_lang_Thread_nativePeer);
  Thread* result = reinterpret_cast<Thread*>(static_cast<uintptr_t>(f->GetLong(thread_peer)));
  // Sanity check that if we have a result it is either suspended or we hold the thread_list_lock_
  // to stop it from going away.
  if (kIsDebugBuild) {
    MutexLock mu(soa.Self(), *Locks::thread_suspend_count_lock_);
    if (result != nullptr && !result->IsSuspended()) {
      Locks::thread_list_lock_->AssertHeld(soa.Self());
    }
  }
  return result;
}

Thread* Thread::FromManagedThread(const ScopedObjectAccessUnchecked& soa, jobject java_thread) {
  return FromManagedThread(soa, soa.Decode<mirror::Object*>(java_thread));
}

static size_t FixStackSize(size_t stack_size) {
  // A stack size of zero means "use the default".
  if (stack_size == 0) {
    stack_size = Runtime::Current()->GetDefaultStackSize();
  }

  // Dalvik used the bionic pthread default stack size for native threads,
  // so include that here to support apps that expect large native stacks.
  stack_size += 1 * MB;

  // It's not possible to request a stack smaller than the system-defined PTHREAD_STACK_MIN.
  if (stack_size < PTHREAD_STACK_MIN) {
    stack_size = PTHREAD_STACK_MIN;
  }

  if (Runtime::Current()->ExplicitStackOverflowChecks()) {
    // It's likely that callers are trying to ensure they have at least a certain amount of
    // stack space, so we should add our reserved space on top of what they requested, rather
    // than implicitly take it away from them.
    stack_size += Thread::kStackOverflowReservedBytes;
  } else {
    // If we are going to use implicit stack checks, allocate space for the protected
    // region at the bottom of the stack.
    stack_size += Thread::kStackOverflowImplicitCheckSize;
  }

  // Some systems require the stack size to be a multiple of the system page size, so round up.
  stack_size = RoundUp(stack_size, kPageSize);

  return stack_size;
}

// Install a protected region in the stack.  This is used to trigger a SIGSEGV if a stack
// overflow is detected.  It is located right below the stack_end_.  Just below that
// is the StackOverflow reserved region used when creating the StackOverflow
// exception.
void Thread::InstallImplicitProtection(bool is_main_stack) {
  byte* pregion = tlsPtr_.stack_end;

  constexpr uint32_t kMarker = 0xdadadada;
  uintptr_t *marker = reinterpret_cast<uintptr_t*>(pregion);
  if (*marker == kMarker) {
    // The region has already been set up.
    return;
  }
  // Add marker so that we can detect a second attempt to do this.
  *marker = kMarker;

  pregion -= kStackOverflowProtectedSize;

  // Touch the pages in the region to map them in.  Otherwise mprotect fails.  Only
  // need to do this on the main stack.  We only need to touch one byte per page.
  if (is_main_stack) {
    byte* start = pregion;
    byte* end = pregion + kStackOverflowProtectedSize;
    while (start < end) {
      *start = static_cast<byte>(0);
      start += kPageSize;
    }
  }

  VLOG(threads) << "installing stack protected region at " << std::hex <<
      static_cast<void*>(pregion) << " to " <<
      static_cast<void*>(pregion + kStackOverflowProtectedSize - 1);

  if (mprotect(pregion, kStackOverflowProtectedSize, PROT_NONE) == -1) {
    LOG(FATAL) << "Unable to create protected region in stack for implicit overflow check. Reason:"
        << strerror(errno);
  }

  // Tell the kernel that we won't be needing these pages any more.
  if (is_main_stack) {
    madvise(pregion, kStackOverflowProtectedSize, MADV_DONTNEED);
  }
}

void Thread::CreateNativeThread(JNIEnv* env, jobject java_peer, size_t stack_size, bool is_daemon) {
  CHECK(java_peer != nullptr);
  Thread* self = static_cast<JNIEnvExt*>(env)->self;
  Runtime* runtime = Runtime::Current();

  // Atomically start the birth of the thread ensuring the runtime isn't shutting down.
  bool thread_start_during_shutdown = false;
  {
    MutexLock mu(self, *Locks::runtime_shutdown_lock_);
    if (runtime->IsShuttingDownLocked()) {
      thread_start_during_shutdown = true;
    } else {
      runtime->StartThreadBirth();
    }
  }
  if (thread_start_during_shutdown) {
    ScopedLocalRef<jclass> error_class(env, env->FindClass("java/lang/InternalError"));
    env->ThrowNew(error_class.get(), "Thread starting during runtime shutdown");
    return;
  }

  Thread* child_thread = new Thread(is_daemon);
  // Use global JNI ref to hold peer live while child thread starts.
  child_thread->tlsPtr_.jpeer = env->NewGlobalRef(java_peer);
  stack_size = FixStackSize(stack_size);

  // Thread.start is synchronized, so we know that nativePeer is 0, and know that we're not racing to
  // assign it.
  env->SetLongField(java_peer, WellKnownClasses::java_lang_Thread_nativePeer,
                    reinterpret_cast<jlong>(child_thread));

  pthread_t new_pthread;
  pthread_attr_t attr;
  CHECK_PTHREAD_CALL(pthread_attr_init, (&attr), "new thread");
  CHECK_PTHREAD_CALL(pthread_attr_setdetachstate, (&attr, PTHREAD_CREATE_DETACHED), "PTHREAD_CREATE_DETACHED");
  CHECK_PTHREAD_CALL(pthread_attr_setstacksize, (&attr, stack_size), stack_size);
  int pthread_create_result = pthread_create(&new_pthread, &attr, Thread::CreateCallback, child_thread);
  CHECK_PTHREAD_CALL(pthread_attr_destroy, (&attr), "new thread");

  if (pthread_create_result != 0) {
    // pthread_create(3) failed, so clean up.
    {
      MutexLock mu(self, *Locks::runtime_shutdown_lock_);
      runtime->EndThreadBirth();
    }
    // Manually delete the global reference since Thread::Init will not have been run.
    env->DeleteGlobalRef(child_thread->tlsPtr_.jpeer);
    child_thread->tlsPtr_.jpeer = nullptr;
    delete child_thread;
    child_thread = nullptr;
    // TODO: remove from thread group?
    env->SetLongField(java_peer, WellKnownClasses::java_lang_Thread_nativePeer, 0);
    {
      std::string msg(StringPrintf("pthread_create (%s stack) failed: %s",
                                   PrettySize(stack_size).c_str(), strerror(pthread_create_result)));
      ScopedObjectAccess soa(env);
      soa.Self()->ThrowOutOfMemoryError(msg.c_str());
    }
  }
}

void Thread::Init(ThreadList* thread_list, JavaVMExt* java_vm) {
  // This function does all the initialization that must be run by the native thread it applies to.
  // (When we create a new thread from managed code, we allocate the Thread* in Thread::Create so
  // we can handshake with the corresponding native thread when it's ready.) Check this native
  // thread hasn't been through here already...
  CHECK(Thread::Current() == nullptr);
  SetUpAlternateSignalStack();
  InitCpu();
  InitTlsEntryPoints();
  RemoveSuspendTrigger();
  InitCardTable();
  InitTid();
  // Set pthread_self_ ahead of pthread_setspecific, that makes Thread::Current function, this
  // avoids pthread_self_ ever being invalid when discovered from Thread::Current().
  tlsPtr_.pthread_self = pthread_self();
  CHECK(is_started_);
  CHECK_PTHREAD_CALL(pthread_setspecific, (Thread::pthread_key_self_, this), "attach self");
  DCHECK_EQ(Thread::Current(), this);

  tls32_.thin_lock_thread_id = thread_list->AllocThreadId(this);
  InitStackHwm();

  tlsPtr_.jni_env = new JNIEnvExt(this, java_vm);
  thread_list->Register(this);
}

Thread* Thread::Attach(const char* thread_name, bool as_daemon, jobject thread_group,
                       bool create_peer) {
  Thread* self;
  Runtime* runtime = Runtime::Current();
  if (runtime == nullptr) {
    LOG(ERROR) << "Thread attaching to non-existent runtime: " << thread_name;
    return nullptr;
  }
  {
    MutexLock mu(nullptr, *Locks::runtime_shutdown_lock_);
    if (runtime->IsShuttingDownLocked()) {
      LOG(ERROR) << "Thread attaching while runtime is shutting down: " << thread_name;
      return nullptr;
    } else {
      Runtime::Current()->StartThreadBirth();
      self = new Thread(as_daemon);
      self->Init(runtime->GetThreadList(), runtime->GetJavaVM());
      Runtime::Current()->EndThreadBirth();
    }
  }

  CHECK_NE(self->GetState(), kRunnable);
  self->SetState(kNative);

  // If we're the main thread, ClassLinker won't be created until after we're attached,
  // so that thread needs a two-stage attach. Regular threads don't need this hack.
  // In the compiler, all threads need this hack, because no-one's going to be getting
  // a native peer!
  if (create_peer) {
    self->CreatePeer(thread_name, as_daemon, thread_group);
  } else {
    // These aren't necessary, but they improve diagnostics for unit tests & command-line tools.
    if (thread_name != nullptr) {
      self->tlsPtr_.name->assign(thread_name);
      ::art::SetThreadName(thread_name);
    }
  }

  return self;
}

void Thread::CreatePeer(const char* name, bool as_daemon, jobject thread_group) {
  Runtime* runtime = Runtime::Current();
  CHECK(runtime->IsStarted());
  JNIEnv* env = tlsPtr_.jni_env;

  if (thread_group == nullptr) {
    thread_group = runtime->GetMainThreadGroup();
  }
  ScopedLocalRef<jobject> thread_name(env, env->NewStringUTF(name));
  jint thread_priority = GetNativePriority();
  jboolean thread_is_daemon = as_daemon;

  ScopedLocalRef<jobject> peer(env, env->AllocObject(WellKnownClasses::java_lang_Thread));
  if (peer.get() == nullptr) {
    CHECK(IsExceptionPending());
    return;
  }
  {
    ScopedObjectAccess soa(this);
    tlsPtr_.opeer = soa.Decode<mirror::Object*>(peer.get());
  }
  env->CallNonvirtualVoidMethod(peer.get(),
                                WellKnownClasses::java_lang_Thread,
                                WellKnownClasses::java_lang_Thread_init,
                                thread_group, thread_name.get(), thread_priority, thread_is_daemon);
  AssertNoPendingException();

  Thread* self = this;
  DCHECK_EQ(self, Thread::Current());
  env->SetLongField(peer.get(), WellKnownClasses::java_lang_Thread_nativePeer,
                    reinterpret_cast<jlong>(self));

  ScopedObjectAccess soa(self);
  StackHandleScope<1> hs(self);
  Handle<mirror::String> peer_thread_name(hs.NewHandle(GetThreadName(soa)));
  if (peer_thread_name.Get() == nullptr) {
    // The Thread constructor should have set the Thread.name to a
    // non-null value. However, because we can run without code
    // available (in the compiler, in tests), we manually assign the
    // fields the constructor should have set.
    if (runtime->IsActiveTransaction()) {
      InitPeer<true>(soa, thread_is_daemon, thread_group, thread_name.get(), thread_priority);
    } else {
      InitPeer<false>(soa, thread_is_daemon, thread_group, thread_name.get(), thread_priority);
    }
    peer_thread_name.Assign(GetThreadName(soa));
  }
  // 'thread_name' may have been null, so don't trust 'peer_thread_name' to be non-null.
  if (peer_thread_name.Get() != nullptr) {
    SetThreadName(peer_thread_name->ToModifiedUtf8().c_str());
  }
}

template<bool kTransactionActive>
void Thread::InitPeer(ScopedObjectAccess& soa, jboolean thread_is_daemon, jobject thread_group,
                      jobject thread_name, jint thread_priority) {
  soa.DecodeField(WellKnownClasses::java_lang_Thread_daemon)->
      SetBoolean<kTransactionActive>(tlsPtr_.opeer, thread_is_daemon);
  soa.DecodeField(WellKnownClasses::java_lang_Thread_group)->
      SetObject<kTransactionActive>(tlsPtr_.opeer, soa.Decode<mirror::Object*>(thread_group));
  soa.DecodeField(WellKnownClasses::java_lang_Thread_name)->
      SetObject<kTransactionActive>(tlsPtr_.opeer, soa.Decode<mirror::Object*>(thread_name));
  soa.DecodeField(WellKnownClasses::java_lang_Thread_priority)->
      SetInt<kTransactionActive>(tlsPtr_.opeer, thread_priority);
}

void Thread::SetThreadName(const char* name) {
  tlsPtr_.name->assign(name);
  ::art::SetThreadName(name);
  Dbg::DdmSendThreadNotification(this, CHUNK_TYPE("THNM"));
}

void Thread::InitStackHwm() {
  void* read_stack_base;
  size_t read_stack_size;
  GetThreadStack(tlsPtr_.pthread_self, &read_stack_base, &read_stack_size);

  // TODO: include this in the thread dumps; potentially useful in SIGQUIT output?
  VLOG(threads) << StringPrintf("Native stack is at %p (%s)", read_stack_base,
                                PrettySize(read_stack_size).c_str());

  tlsPtr_.stack_begin = reinterpret_cast<byte*>(read_stack_base);
  tlsPtr_.stack_size = read_stack_size;

  if (read_stack_size <= kStackOverflowReservedBytes) {
    LOG(FATAL) << "Attempt to attach a thread with a too-small stack (" << read_stack_size
        << " bytes)";
  }

  // TODO: move this into the Linux GetThreadStack implementation.
#if !defined(__APPLE__)
  // If we're the main thread, check whether we were run with an unlimited stack. In that case,
  // glibc will have reported a 2GB stack for our 32-bit process, and our stack overflow detection
  // will be broken because we'll die long before we get close to 2GB.
  bool is_main_thread = (::art::GetTid() == getpid());
  if (is_main_thread) {
    rlimit stack_limit;
    if (getrlimit(RLIMIT_STACK, &stack_limit) == -1) {
      PLOG(FATAL) << "getrlimit(RLIMIT_STACK) failed";
    }
    if (stack_limit.rlim_cur == RLIM_INFINITY) {
      // Find the default stack size for new threads...
      pthread_attr_t default_attributes;
      size_t default_stack_size;
      CHECK_PTHREAD_CALL(pthread_attr_init, (&default_attributes), "default stack size query");
      CHECK_PTHREAD_CALL(pthread_attr_getstacksize, (&default_attributes, &default_stack_size),
                         "default stack size query");
      CHECK_PTHREAD_CALL(pthread_attr_destroy, (&default_attributes), "default stack size query");

      // ...and use that as our limit.
      size_t old_stack_size = read_stack_size;
      tlsPtr_.stack_size = default_stack_size;
      tlsPtr_.stack_begin += (old_stack_size - default_stack_size);
      VLOG(threads) << "Limiting unlimited stack (reported as " << PrettySize(old_stack_size) << ")"
                    << " to " << PrettySize(default_stack_size)
                    << " with base " << reinterpret_cast<void*>(tlsPtr_.stack_begin);
    }
  }
#endif

  // Set stack_end_ to the bottom of the stack saving space of stack overflows
  bool implicit_stack_check = !Runtime::Current()->ExplicitStackOverflowChecks();
  ResetDefaultStackEnd(implicit_stack_check);

  // Install the protected region if we are doing implicit overflow checks.
  if (implicit_stack_check) {
    if (is_main_thread) {
      // The main thread has a 16K protected region at the bottom.  We need
      // to install our own region so we need to move the limits
      // of the stack to make room for it.
      constexpr uint32_t kDelta = 16 * KB;
      tlsPtr_.stack_begin += kDelta;
      tlsPtr_.stack_end += kDelta;
      tlsPtr_.stack_size -= kDelta;
    }
    InstallImplicitProtection(is_main_thread);
  }

  // Sanity check.
  int stack_variable;
  CHECK_GT(&stack_variable, reinterpret_cast<void*>(tlsPtr_.stack_end));
}

void Thread::ShortDump(std::ostream& os) const {
  os << "Thread[";
  if (GetThreadId() != 0) {
    // If we're in kStarting, we won't have a thin lock id or tid yet.
    os << GetThreadId()
             << ",tid=" << GetTid() << ',';
  }
  os << GetState()
           << ",Thread*=" << this
           << ",peer=" << tlsPtr_.opeer
           << ",\"" << *tlsPtr_.name << "\""
           << "]";
}

void Thread::Dump(std::ostream& os) const {
  DumpState(os);
  DumpStack(os);
}

mirror::String* Thread::GetThreadName(const ScopedObjectAccessUnchecked& soa) const {
  mirror::ArtField* f = soa.DecodeField(WellKnownClasses::java_lang_Thread_name);
  return (tlsPtr_.opeer != nullptr) ? reinterpret_cast<mirror::String*>(f->GetObject(tlsPtr_.opeer)) : nullptr;
}

void Thread::GetThreadName(std::string& name) const {
  name.assign(*tlsPtr_.name);
}

uint64_t Thread::GetCpuMicroTime() const {
#if defined(HAVE_POSIX_CLOCKS)
  clockid_t cpu_clock_id;
  pthread_getcpuclockid(tlsPtr_.pthread_self, &cpu_clock_id);
  timespec now;
  clock_gettime(cpu_clock_id, &now);
  return static_cast<uint64_t>(now.tv_sec) * UINT64_C(1000000) + now.tv_nsec / UINT64_C(1000);
#else
  UNIMPLEMENTED(WARNING);
  return -1;
#endif
}

void Thread::AtomicSetFlag(ThreadFlag flag) {
  android_atomic_or(flag, &tls32_.state_and_flags.as_int);
}

void Thread::AtomicClearFlag(ThreadFlag flag) {
  android_atomic_and(-1 ^ flag, &tls32_.state_and_flags.as_int);
}

// Attempt to rectify locks so that we dump thread list with required locks before exiting.
static void UnsafeLogFatalForSuspendCount(Thread* self, Thread* thread) NO_THREAD_SAFETY_ANALYSIS {
  LOG(ERROR) << *thread << " suspend count already zero.";
  Locks::thread_suspend_count_lock_->Unlock(self);
  if (!Locks::mutator_lock_->IsSharedHeld(self)) {
    Locks::mutator_lock_->SharedTryLock(self);
    if (!Locks::mutator_lock_->IsSharedHeld(self)) {
      LOG(WARNING) << "Dumping thread list without holding mutator_lock_";
    }
  }
  if (!Locks::thread_list_lock_->IsExclusiveHeld(self)) {
    Locks::thread_list_lock_->TryLock(self);
    if (!Locks::thread_list_lock_->IsExclusiveHeld(self)) {
      LOG(WARNING) << "Dumping thread list without holding thread_list_lock_";
    }
  }
  std::ostringstream ss;
  Runtime::Current()->GetThreadList()->DumpLocked(ss);
  LOG(FATAL) << ss.str();
}

void Thread::ModifySuspendCount(Thread* self, int delta, bool for_debugger) {
  if (kIsDebugBuild) {
    DCHECK(delta == -1 || delta == +1 || delta == -tls32_.debug_suspend_count)
          << delta << " " << tls32_.debug_suspend_count << " " << this;
    DCHECK_GE(tls32_.suspend_count, tls32_.debug_suspend_count) << this;
    Locks::thread_suspend_count_lock_->AssertHeld(self);
    if (this != self && !IsSuspended()) {
      Locks::thread_list_lock_->AssertHeld(self);
    }
  }
  if (UNLIKELY(delta < 0 && tls32_.suspend_count <= 0)) {
    UnsafeLogFatalForSuspendCount(self, this);
    return;
  }

  tls32_.suspend_count += delta;
  if (for_debugger) {
    tls32_.debug_suspend_count += delta;
  }

  if (tls32_.suspend_count == 0) {
    AtomicClearFlag(kSuspendRequest);
  } else {
    AtomicSetFlag(kSuspendRequest);
    TriggerSuspend();
  }
}

void Thread::RunCheckpointFunction() {
  Closure *checkpoints[kMaxCheckpoints];

  // Grab the suspend_count lock and copy the current set of
  // checkpoints.  Then clear the list and the flag.  The RequestCheckpoint
  // function will also grab this lock so we prevent a race between setting
  // the kCheckpointRequest flag and clearing it.
  {
    MutexLock mu(this, *Locks::thread_suspend_count_lock_);
    for (uint32_t i = 0; i < kMaxCheckpoints; ++i) {
      checkpoints[i] = tlsPtr_.checkpoint_functions[i];
      tlsPtr_.checkpoint_functions[i] = nullptr;
    }
    AtomicClearFlag(kCheckpointRequest);
  }

  // Outside the lock, run all the checkpoint functions that
  // we collected.
  bool found_checkpoint = false;
  for (uint32_t i = 0; i < kMaxCheckpoints; ++i) {
    if (checkpoints[i] != nullptr) {
      ATRACE_BEGIN("Checkpoint function");
      checkpoints[i]->Run(this);
      ATRACE_END();
      found_checkpoint = true;
    }
  }
  CHECK(found_checkpoint);
}

bool Thread::RequestCheckpoint(Closure* function) {
  union StateAndFlags old_state_and_flags;
  old_state_and_flags.as_int = tls32_.state_and_flags.as_int;
  if (old_state_and_flags.as_struct.state != kRunnable) {
    return false;  // Fail, thread is suspended and so can't run a checkpoint.
  }

  uint32_t available_checkpoint = kMaxCheckpoints;
  for (uint32_t i = 0 ; i < kMaxCheckpoints; ++i) {
    if (tlsPtr_.checkpoint_functions[i] == nullptr) {
      available_checkpoint = i;
      break;
    }
  }
  if (available_checkpoint == kMaxCheckpoints) {
    // No checkpoint functions available, we can't run a checkpoint
    return false;
  }
  tlsPtr_.checkpoint_functions[available_checkpoint] = function;

  // Checkpoint function installed now install flag bit.
  // We must be runnable to request a checkpoint.
  DCHECK_EQ(old_state_and_flags.as_struct.state, kRunnable);
  union StateAndFlags new_state_and_flags;
  new_state_and_flags.as_int = old_state_and_flags.as_int;
  new_state_and_flags.as_struct.flags |= kCheckpointRequest;
  int succeeded = android_atomic_acquire_cas(old_state_and_flags.as_int, new_state_and_flags.as_int,
                                             &tls32_.state_and_flags.as_int);
  if (UNLIKELY(succeeded != 0)) {
    // The thread changed state before the checkpoint was installed.
    CHECK_EQ(tlsPtr_.checkpoint_functions[available_checkpoint], function);
    tlsPtr_.checkpoint_functions[available_checkpoint] = nullptr;
  } else {
    CHECK_EQ(ReadFlag(kCheckpointRequest), true);
    TriggerSuspend();
  }
  return succeeded == 0;
}

void Thread::FullSuspendCheck() {
  VLOG(threads) << this << " self-suspending";
  ATRACE_BEGIN("Full suspend check");
  // Make thread appear suspended to other threads, release mutator_lock_.
  TransitionFromRunnableToSuspended(kSuspended);
  // Transition back to runnable noting requests to suspend, re-acquire share on mutator_lock_.
  TransitionFromSuspendedToRunnable();
  ATRACE_END();
  VLOG(threads) << this << " self-reviving";
}

void Thread::DumpState(std::ostream& os, const Thread* thread, pid_t tid) {
  std::string group_name;
  int priority;
  bool is_daemon = false;
  Thread* self = Thread::Current();

  // Don't do this if we are aborting since the GC may have all the threads suspended. This will
  // cause ScopedObjectAccessUnchecked to deadlock.
  if (gAborting == 0 && self != nullptr && thread != nullptr && thread->tlsPtr_.opeer != nullptr) {
    ScopedObjectAccessUnchecked soa(self);
    priority = soa.DecodeField(WellKnownClasses::java_lang_Thread_priority)
        ->GetInt(thread->tlsPtr_.opeer);
    is_daemon = soa.DecodeField(WellKnownClasses::java_lang_Thread_daemon)
        ->GetBoolean(thread->tlsPtr_.opeer);

    mirror::Object* thread_group =
        soa.DecodeField(WellKnownClasses::java_lang_Thread_group)->GetObject(thread->tlsPtr_.opeer);

    if (thread_group != nullptr) {
      mirror::ArtField* group_name_field =
          soa.DecodeField(WellKnownClasses::java_lang_ThreadGroup_name);
      mirror::String* group_name_string =
          reinterpret_cast<mirror::String*>(group_name_field->GetObject(thread_group));
      group_name = (group_name_string != nullptr) ? group_name_string->ToModifiedUtf8() : "<null>";
    }
  } else {
    priority = GetNativePriority();
  }

  std::string scheduler_group_name(GetSchedulerGroupName(tid));
  if (scheduler_group_name.empty()) {
    scheduler_group_name = "default";
  }

  if (thread != nullptr) {
    os << '"' << *thread->tlsPtr_.name << '"';
    if (is_daemon) {
      os << " daemon";
    }
    os << " prio=" << priority
       << " tid=" << thread->GetThreadId()
       << " " << thread->GetState();
    if (thread->IsStillStarting()) {
      os << " (still starting up)";
    }
    os << "\n";
  } else {
    os << '"' << ::art::GetThreadName(tid) << '"'
       << " prio=" << priority
       << " (not attached)\n";
  }

  if (thread != nullptr) {
    MutexLock mu(self, *Locks::thread_suspend_count_lock_);
    os << "  | group=\"" << group_name << "\""
       << " sCount=" << thread->tls32_.suspend_count
       << " dsCount=" << thread->tls32_.debug_suspend_count
       << " obj=" << reinterpret_cast<void*>(thread->tlsPtr_.opeer)
       << " self=" << reinterpret_cast<const void*>(thread) << "\n";
  }

  os << "  | sysTid=" << tid
     << " nice=" << getpriority(PRIO_PROCESS, tid)
     << " cgrp=" << scheduler_group_name;
  if (thread != nullptr) {
    int policy;
    sched_param sp;
    CHECK_PTHREAD_CALL(pthread_getschedparam, (thread->tlsPtr_.pthread_self, &policy, &sp),
                       __FUNCTION__);
    os << " sched=" << policy << "/" << sp.sched_priority
       << " handle=" << reinterpret_cast<void*>(thread->tlsPtr_.pthread_self);
  }
  os << "\n";

  // Grab the scheduler stats for this thread.
  std::string scheduler_stats;
  if (ReadFileToString(StringPrintf("/proc/self/task/%d/schedstat", tid), &scheduler_stats)) {
    scheduler_stats.resize(scheduler_stats.size() - 1);  // Lose the trailing '\n'.
  } else {
    scheduler_stats = "0 0 0";
  }

  char native_thread_state = '?';
  int utime = 0;
  int stime = 0;
  int task_cpu = 0;
  GetTaskStats(tid, &native_thread_state, &utime, &stime, &task_cpu);

  os << "  | state=" << native_thread_state
     << " schedstat=( " << scheduler_stats << " )"
     << " utm=" << utime
     << " stm=" << stime
     << " core=" << task_cpu
     << " HZ=" << sysconf(_SC_CLK_TCK) << "\n";
  if (thread != nullptr) {
    os << "  | stack=" << reinterpret_cast<void*>(thread->tlsPtr_.stack_begin) << "-"
        << reinterpret_cast<void*>(thread->tlsPtr_.stack_end) << " stackSize="
        << PrettySize(thread->tlsPtr_.stack_size) << "\n";
  }
}

void Thread::DumpState(std::ostream& os) const {
  Thread::DumpState(os, this, GetTid());
}

struct StackDumpVisitor : public StackVisitor {
  StackDumpVisitor(std::ostream& os, Thread* thread, Context* context, bool can_allocate)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : StackVisitor(thread, context), os(os), thread(thread), can_allocate(can_allocate),
        last_method(nullptr), last_line_number(0), repetition_count(0), frame_count(0) {
  }

  virtual ~StackDumpVisitor() {
    if (frame_count == 0) {
      os << "  (no managed stack frames)\n";
    }
  }

  bool VisitFrame() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    mirror::ArtMethod* m = GetMethod();
    if (m->IsRuntimeMethod()) {
      return true;
    }
    const int kMaxRepetition = 3;
    mirror::Class* c = m->GetDeclaringClass();
    mirror::DexCache* dex_cache = c->GetDexCache();
    int line_number = -1;
    if (dex_cache != nullptr) {  // be tolerant of bad input
      const DexFile& dex_file = *dex_cache->GetDexFile();
      line_number = dex_file.GetLineNumFromPC(m, GetDexPc(false));
    }
    if (line_number == last_line_number && last_method == m) {
      ++repetition_count;
    } else {
      if (repetition_count >= kMaxRepetition) {
        os << "  ... repeated " << (repetition_count - kMaxRepetition) << " times\n";
      }
      repetition_count = 0;
      last_line_number = line_number;
      last_method = m;
    }
    if (repetition_count < kMaxRepetition) {
      os << "  at " << PrettyMethod(m, false);
      if (m->IsNative()) {
        os << "(Native method)";
      } else {
        mh.ChangeMethod(m);
        const char* source_file(mh.GetDeclaringClassSourceFile());
        os << "(" << (source_file != nullptr ? source_file : "unavailable")
           << ":" << line_number << ")";
      }
      os << "\n";
      if (frame_count == 0) {
        Monitor::DescribeWait(os, thread);
      }
      if (can_allocate) {
        Monitor::VisitLocks(this, DumpLockedObject, &os);
      }
    }

    ++frame_count;
    return true;
  }

  static void DumpLockedObject(mirror::Object* o, void* context)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    std::ostream& os = *reinterpret_cast<std::ostream*>(context);
    os << "  - locked ";
    if (o == nullptr) {
      os << "an unknown object";
    } else {
      if ((o->GetLockWord(false).GetState() == LockWord::kThinLocked) &&
          Locks::mutator_lock_->IsExclusiveHeld(Thread::Current())) {
        // Getting the identity hashcode here would result in lock inflation and suspension of the
        // current thread, which isn't safe if this is the only runnable thread.
        os << StringPrintf("<@addr=0x%" PRIxPTR "> (a %s)", reinterpret_cast<intptr_t>(o),
                           PrettyTypeOf(o).c_str());
      } else {
        os << StringPrintf("<0x%08x> (a %s)", o->IdentityHashCode(), PrettyTypeOf(o).c_str());
      }
    }
    os << "\n";
  }

  std::ostream& os;
  const Thread* thread;
  const bool can_allocate;
  MethodHelper mh;
  mirror::ArtMethod* last_method;
  int last_line_number;
  int repetition_count;
  int frame_count;
};

static bool ShouldShowNativeStack(const Thread* thread)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ThreadState state = thread->GetState();

  // In native code somewhere in the VM (one of the kWaitingFor* states)? That's interesting.
  if (state > kWaiting && state < kStarting) {
    return true;
  }

  // In an Object.wait variant or Thread.sleep? That's not interesting.
  if (state == kTimedWaiting || state == kSleeping || state == kWaiting) {
    return false;
  }

  // In some other native method? That's interesting.
  // We don't just check kNative because native methods will be in state kSuspended if they're
  // calling back into the VM, or kBlocked if they're blocked on a monitor, or one of the
  // thread-startup states if it's early enough in their life cycle (http://b/7432159).
  mirror::ArtMethod* current_method = thread->GetCurrentMethod(nullptr);
  return current_method != nullptr && current_method->IsNative();
}

void Thread::DumpJavaStack(std::ostream& os) const {
  UniquePtr<Context> context(Context::Create());
  StackDumpVisitor dumper(os, const_cast<Thread*>(this), context.get(),
                          !tls32_.throwing_OutOfMemoryError);
  dumper.WalkStack();
}

void Thread::DumpStack(std::ostream& os) const {
  // TODO: we call this code when dying but may not have suspended the thread ourself. The
  //       IsSuspended check is therefore racy with the use for dumping (normally we inhibit
  //       the race with the thread_suspend_count_lock_).
  // No point dumping for an abort in debug builds where we'll hit the not suspended check in stack.
  bool dump_for_abort = (gAborting > 0) && !kIsDebugBuild;
  if (this == Thread::Current() || IsSuspended() || dump_for_abort) {
    // If we're currently in native code, dump that stack before dumping the managed stack.
    if (dump_for_abort || ShouldShowNativeStack(this)) {
      DumpKernelStack(os, GetTid(), "  kernel: ", false);
      DumpNativeStack(os, GetTid(), "  native: ", GetCurrentMethod(nullptr));
    }
    DumpJavaStack(os);
  } else {
    os << "Not able to dump stack of thread that isn't suspended";
  }
}

void Thread::ThreadExitCallback(void* arg) {
  Thread* self = reinterpret_cast<Thread*>(arg);
  if (self->tls32_.thread_exit_check_count == 0) {
    LOG(WARNING) << "Native thread exiting without having called DetachCurrentThread (maybe it's "
        "going to use a pthread_key_create destructor?): " << *self;
    CHECK(is_started_);
    CHECK_PTHREAD_CALL(pthread_setspecific, (Thread::pthread_key_self_, self), "reattach self");
    self->tls32_.thread_exit_check_count = 1;
  } else {
    LOG(FATAL) << "Native thread exited without calling DetachCurrentThread: " << *self;
  }
}

void Thread::Startup() {
  CHECK(!is_started_);
  is_started_ = true;
  {
    // MutexLock to keep annotalysis happy.
    //
    // Note we use nullptr for the thread because Thread::Current can
    // return garbage since (is_started_ == true) and
    // Thread::pthread_key_self_ is not yet initialized.
    // This was seen on glibc.
    MutexLock mu(nullptr, *Locks::thread_suspend_count_lock_);
    resume_cond_ = new ConditionVariable("Thread resumption condition variable",
                                         *Locks::thread_suspend_count_lock_);
  }

  // Allocate a TLS slot.
  CHECK_PTHREAD_CALL(pthread_key_create, (&Thread::pthread_key_self_, Thread::ThreadExitCallback), "self key");

  // Double-check the TLS slot allocation.
  if (pthread_getspecific(pthread_key_self_) != nullptr) {
    LOG(FATAL) << "Newly-created pthread TLS slot is not nullptr";
  }
}

void Thread::FinishStartup() {
  Runtime* runtime = Runtime::Current();
  CHECK(runtime->IsStarted());

  // Finish attaching the main thread.
  ScopedObjectAccess soa(Thread::Current());
  Thread::Current()->CreatePeer("main", false, runtime->GetMainThreadGroup());

  Runtime::Current()->GetClassLinker()->RunRootClinits();
}

void Thread::Shutdown() {
  CHECK(is_started_);
  is_started_ = false;
  CHECK_PTHREAD_CALL(pthread_key_delete, (Thread::pthread_key_self_), "self key");
  MutexLock mu(Thread::Current(), *Locks::thread_suspend_count_lock_);
  if (resume_cond_ != nullptr) {
    delete resume_cond_;
    resume_cond_ = nullptr;
  }
}

Thread::Thread(bool daemon) : tls32_(daemon), wait_monitor_(nullptr), interrupted_(false) {
  wait_mutex_ = new Mutex("a thread wait mutex");
  wait_cond_ = new ConditionVariable("a thread wait condition variable", *wait_mutex_);
  tlsPtr_.debug_invoke_req = new DebugInvokeReq;
  tlsPtr_.single_step_control = new SingleStepControl;
  tlsPtr_.instrumentation_stack = new std::deque<instrumentation::InstrumentationStackFrame>;
  tlsPtr_.name = new std::string(kThreadNameDuringStartup);

  CHECK_EQ((sizeof(Thread) % 4), 0U) << sizeof(Thread);
  tls32_.state_and_flags.as_struct.flags = 0;
  tls32_.state_and_flags.as_struct.state = kNative;
  memset(&tlsPtr_.held_mutexes[0], 0, sizeof(tlsPtr_.held_mutexes));
  std::fill(tlsPtr_.rosalloc_runs,
            tlsPtr_.rosalloc_runs + gc::allocator::RosAlloc::kNumThreadLocalSizeBrackets,
            gc::allocator::RosAlloc::GetDedicatedFullRun());
  for (uint32_t i = 0; i < kMaxCheckpoints; ++i) {
    tlsPtr_.checkpoint_functions[i] = nullptr;
  }
}

bool Thread::IsStillStarting() const {
  // You might think you can check whether the state is kStarting, but for much of thread startup,
  // the thread is in kNative; it might also be in kVmWait.
  // You might think you can check whether the peer is nullptr, but the peer is actually created and
  // assigned fairly early on, and needs to be.
  // It turns out that the last thing to change is the thread name; that's a good proxy for "has
  // this thread _ever_ entered kRunnable".
  return (tlsPtr_.jpeer == nullptr && tlsPtr_.opeer == nullptr) ||
      (*tlsPtr_.name == kThreadNameDuringStartup);
}

void Thread::AssertNoPendingException() const {
  if (UNLIKELY(IsExceptionPending())) {
    ScopedObjectAccess soa(Thread::Current());
    mirror::Throwable* exception = GetException(nullptr);
    LOG(FATAL) << "No pending exception expected: " << exception->Dump();
  }
}

void Thread::AssertNoPendingExceptionForNewException(const char* msg) const {
  if (UNLIKELY(IsExceptionPending())) {
    ScopedObjectAccess soa(Thread::Current());
    mirror::Throwable* exception = GetException(nullptr);
    LOG(FATAL) << "Throwing new exception " << msg << " with unexpected pending exception: "
        << exception->Dump();
  }
}

static void MonitorExitVisitor(mirror::Object** object, void* arg, uint32_t /*thread_id*/,
                               RootType /*root_type*/)
    NO_THREAD_SAFETY_ANALYSIS {
  Thread* self = reinterpret_cast<Thread*>(arg);
  mirror::Object* entered_monitor = *object;
  if (self->HoldsLock(entered_monitor)) {
    LOG(WARNING) << "Calling MonitorExit on object "
                 << object << " (" << PrettyTypeOf(entered_monitor) << ")"
                 << " left locked by native thread "
                 << *Thread::Current() << " which is detaching";
    entered_monitor->MonitorExit(self);
  }
}

void Thread::Destroy() {
  Thread* self = this;
  DCHECK_EQ(self, Thread::Current());

  if (tlsPtr_.opeer != nullptr) {
    ScopedObjectAccess soa(self);
    // We may need to call user-supplied managed code, do this before final clean-up.
    HandleUncaughtExceptions(soa);
    RemoveFromThreadGroup(soa);

    // this.nativePeer = 0;
    if (Runtime::Current()->IsActiveTransaction()) {
      soa.DecodeField(WellKnownClasses::java_lang_Thread_nativePeer)
          ->SetLong<true>(tlsPtr_.opeer, 0);
    } else {
      soa.DecodeField(WellKnownClasses::java_lang_Thread_nativePeer)
          ->SetLong<false>(tlsPtr_.opeer, 0);
    }
    Dbg::PostThreadDeath(self);

    // Thread.join() is implemented as an Object.wait() on the Thread.lock object. Signal anyone
    // who is waiting.
    mirror::Object* lock =
        soa.DecodeField(WellKnownClasses::java_lang_Thread_lock)->GetObject(tlsPtr_.opeer);
    // (This conditional is only needed for tests, where Thread.lock won't have been set.)
    if (lock != nullptr) {
      StackHandleScope<1> hs(self);
      Handle<mirror::Object> h_obj(hs.NewHandle(lock));
      ObjectLock<mirror::Object> locker(self, &h_obj);
      locker.NotifyAll();
    }
  }

  // On thread detach, all monitors entered with JNI MonitorEnter are automatically exited.
  if (tlsPtr_.jni_env != nullptr) {
    tlsPtr_.jni_env->monitors.VisitRoots(MonitorExitVisitor, self, 0, kRootVMInternal);
  }
}

Thread::~Thread() {
  if (tlsPtr_.jni_env != nullptr && tlsPtr_.jpeer != nullptr) {
    // If pthread_create fails we don't have a jni env here.
    tlsPtr_.jni_env->DeleteGlobalRef(tlsPtr_.jpeer);
    tlsPtr_.jpeer = nullptr;
  }
  tlsPtr_.opeer = nullptr;

  bool initialized = (tlsPtr_.jni_env != nullptr);  // Did Thread::Init run?
  if (initialized) {
    delete tlsPtr_.jni_env;
    tlsPtr_.jni_env = nullptr;
  }
  CHECK_NE(GetState(), kRunnable);
  CHECK_NE(ReadFlag(kCheckpointRequest), true);
  CHECK(tlsPtr_.checkpoint_functions[0] == nullptr);
  CHECK(tlsPtr_.checkpoint_functions[1] == nullptr);
  CHECK(tlsPtr_.checkpoint_functions[2] == nullptr);

  // We may be deleting a still born thread.
  SetStateUnsafe(kTerminated);

  delete wait_cond_;
  delete wait_mutex_;

  if (tlsPtr_.long_jump_context != nullptr) {
    delete tlsPtr_.long_jump_context;
  }

  if (initialized) {
    CleanupCpu();
  }

  delete tlsPtr_.debug_invoke_req;
  delete tlsPtr_.single_step_control;
  delete tlsPtr_.instrumentation_stack;
  delete tlsPtr_.name;
  delete tlsPtr_.stack_trace_sample;

  Runtime::Current()->GetHeap()->RevokeThreadLocalBuffers(this);

  TearDownAlternateSignalStack();
}

void Thread::HandleUncaughtExceptions(ScopedObjectAccess& soa) {
  if (!IsExceptionPending()) {
    return;
  }
  ScopedLocalRef<jobject> peer(tlsPtr_.jni_env, soa.AddLocalReference<jobject>(tlsPtr_.opeer));
  ScopedThreadStateChange tsc(this, kNative);

  // Get and clear the exception.
  ScopedLocalRef<jthrowable> exception(tlsPtr_.jni_env, tlsPtr_.jni_env->ExceptionOccurred());
  tlsPtr_.jni_env->ExceptionClear();

  // If the thread has its own handler, use that.
  ScopedLocalRef<jobject> handler(tlsPtr_.jni_env,
                                  tlsPtr_.jni_env->GetObjectField(peer.get(),
                                      WellKnownClasses::java_lang_Thread_uncaughtHandler));
  if (handler.get() == nullptr) {
    // Otherwise use the thread group's default handler.
    handler.reset(tlsPtr_.jni_env->GetObjectField(peer.get(),
                                                  WellKnownClasses::java_lang_Thread_group));
  }

  // Call the handler.
  tlsPtr_.jni_env->CallVoidMethod(handler.get(),
      WellKnownClasses::java_lang_Thread$UncaughtExceptionHandler_uncaughtException,
      peer.get(), exception.get());

  // If the handler threw, clear that exception too.
  tlsPtr_.jni_env->ExceptionClear();
}

void Thread::RemoveFromThreadGroup(ScopedObjectAccess& soa) {
  // this.group.removeThread(this);
  // group can be null if we're in the compiler or a test.
  mirror::Object* ogroup = soa.DecodeField(WellKnownClasses::java_lang_Thread_group)
      ->GetObject(tlsPtr_.opeer);
  if (ogroup != nullptr) {
    ScopedLocalRef<jobject> group(soa.Env(), soa.AddLocalReference<jobject>(ogroup));
    ScopedLocalRef<jobject> peer(soa.Env(), soa.AddLocalReference<jobject>(tlsPtr_.opeer));
    ScopedThreadStateChange tsc(soa.Self(), kNative);
    tlsPtr_.jni_env->CallVoidMethod(group.get(),
                                    WellKnownClasses::java_lang_ThreadGroup_removeThread,
                                    peer.get());
  }
}

size_t Thread::NumHandleReferences() {
  size_t count = 0;
  for (HandleScope* cur = tlsPtr_.top_handle_scope; cur; cur = cur->GetLink()) {
    count += cur->NumberOfReferences();
  }
  return count;
}

bool Thread::HandleScopeContains(jobject obj) const {
  StackReference<mirror::Object>* hs_entry =
      reinterpret_cast<StackReference<mirror::Object>*>(obj);
  for (HandleScope* cur = tlsPtr_.top_handle_scope; cur; cur = cur->GetLink()) {
    if (cur->Contains(hs_entry)) {
      return true;
    }
  }
  // JNI code invoked from portable code uses shadow frames rather than the handle scope.
  return tlsPtr_.managed_stack.ShadowFramesContain(hs_entry);
}

void Thread::HandleScopeVisitRoots(RootCallback* visitor, void* arg, uint32_t thread_id) {
  for (HandleScope* cur = tlsPtr_.top_handle_scope; cur; cur = cur->GetLink()) {
    size_t num_refs = cur->NumberOfReferences();
    for (size_t j = 0; j < num_refs; ++j) {
      mirror::Object* object = cur->GetReference(j);
      if (object != nullptr) {
        mirror::Object* old_obj = object;
        visitor(&object, arg, thread_id, kRootNativeStack);
        if (old_obj != object) {
          cur->SetReference(j, object);
        }
      }
    }
  }
}

mirror::Object* Thread::DecodeJObject(jobject obj) const {
  Locks::mutator_lock_->AssertSharedHeld(this);
  if (obj == nullptr) {
    return nullptr;
  }
  IndirectRef ref = reinterpret_cast<IndirectRef>(obj);
  IndirectRefKind kind = GetIndirectRefKind(ref);
  mirror::Object* result;
  // The "kinds" below are sorted by the frequency we expect to encounter them.
  if (kind == kLocal) {
    IndirectReferenceTable& locals = tlsPtr_.jni_env->locals;
    result = locals.Get(ref);
  } else if (kind == kHandleScopeOrInvalid) {
    // TODO: make stack indirect reference table lookup more efficient.
    // Check if this is a local reference in the handle scope.
    if (LIKELY(HandleScopeContains(obj))) {
      // Read from handle scope.
      result = reinterpret_cast<StackReference<mirror::Object>*>(obj)->AsMirrorPtr();
      VerifyObject(result);
    } else {
      result = kInvalidIndirectRefObject;
    }
  } else if (kind == kGlobal) {
    JavaVMExt* const vm = Runtime::Current()->GetJavaVM();
    result = vm->globals.SynchronizedGet(const_cast<Thread*>(this), &vm->globals_lock, ref);
  } else {
    DCHECK_EQ(kind, kWeakGlobal);
    result = Runtime::Current()->GetJavaVM()->DecodeWeakGlobal(const_cast<Thread*>(this), ref);
    if (result == kClearedJniWeakGlobal) {
      // This is a special case where it's okay to return nullptr.
      return nullptr;
    }
  }

  if (UNLIKELY(result == nullptr)) {
    JniAbortF(nullptr, "use of deleted %s %p", ToStr<IndirectRefKind>(kind).c_str(), obj);
  }
  return result;
}

// Implements java.lang.Thread.interrupted.
bool Thread::Interrupted() {
  MutexLock mu(Thread::Current(), *wait_mutex_);
  bool interrupted = IsInterruptedLocked();
  SetInterruptedLocked(false);
  return interrupted;
}

// Implements java.lang.Thread.isInterrupted.
bool Thread::IsInterrupted() {
  MutexLock mu(Thread::Current(), *wait_mutex_);
  return IsInterruptedLocked();
}

void Thread::Interrupt(Thread* self) {
  MutexLock mu(self, *wait_mutex_);
  if (interrupted_) {
    return;
  }
  interrupted_ = true;
  NotifyLocked(self);
}

void Thread::Notify() {
  Thread* self = Thread::Current();
  MutexLock mu(self, *wait_mutex_);
  NotifyLocked(self);
}

void Thread::NotifyLocked(Thread* self) {
  if (wait_monitor_ != nullptr) {
    wait_cond_->Signal(self);
  }
}

class CountStackDepthVisitor : public StackVisitor {
 public:
  explicit CountStackDepthVisitor(Thread* thread)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : StackVisitor(thread, nullptr),
        depth_(0), skip_depth_(0), skipping_(true) {}

  bool VisitFrame() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    // We want to skip frames up to and including the exception's constructor.
    // Note we also skip the frame if it doesn't have a method (namely the callee
    // save frame)
    mirror::ArtMethod* m = GetMethod();
    if (skipping_ && !m->IsRuntimeMethod() &&
        !mirror::Throwable::GetJavaLangThrowable()->IsAssignableFrom(m->GetDeclaringClass())) {
      skipping_ = false;
    }
    if (!skipping_) {
      if (!m->IsRuntimeMethod()) {  // Ignore runtime frames (in particular callee save).
        ++depth_;
      }
    } else {
      ++skip_depth_;
    }
    return true;
  }

  int GetDepth() const {
    return depth_;
  }

  int GetSkipDepth() const {
    return skip_depth_;
  }

 private:
  uint32_t depth_;
  uint32_t skip_depth_;
  bool skipping_;
};

template<bool kTransactionActive>
class BuildInternalStackTraceVisitor : public StackVisitor {
 public:
  explicit BuildInternalStackTraceVisitor(Thread* self, Thread* thread, int skip_depth)
      : StackVisitor(thread, nullptr), self_(self),
        skip_depth_(skip_depth), count_(0), dex_pc_trace_(nullptr), method_trace_(nullptr) {}

  bool Init(int depth)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    // Allocate method trace with an extra slot that will hold the PC trace
    StackHandleScope<1> hs(self_);
    ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
    Handle<mirror::ObjectArray<mirror::Object>> method_trace(
        hs.NewHandle(class_linker->AllocObjectArray<mirror::Object>(self_, depth + 1)));
    if (method_trace.Get() == nullptr) {
      return false;
    }
    mirror::IntArray* dex_pc_trace = mirror::IntArray::Alloc(self_, depth);
    if (dex_pc_trace == nullptr) {
      return false;
    }
    // Save PC trace in last element of method trace, also places it into the
    // object graph.
    // We are called from native: use non-transactional mode.
    method_trace->Set<kTransactionActive>(depth, dex_pc_trace);
    // Set the Object*s and assert that no thread suspension is now possible.
    const char* last_no_suspend_cause =
        self_->StartAssertNoThreadSuspension("Building internal stack trace");
    CHECK(last_no_suspend_cause == nullptr) << last_no_suspend_cause;
    method_trace_ = method_trace.Get();
    dex_pc_trace_ = dex_pc_trace;
    return true;
  }

  virtual ~BuildInternalStackTraceVisitor() {
    if (method_trace_ != nullptr) {
      self_->EndAssertNoThreadSuspension(nullptr);
    }
  }

  bool VisitFrame() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    if (method_trace_ == nullptr || dex_pc_trace_ == nullptr) {
      return true;  // We're probably trying to fillInStackTrace for an OutOfMemoryError.
    }
    if (skip_depth_ > 0) {
      skip_depth_--;
      return true;
    }
    mirror::ArtMethod* m = GetMethod();
    if (m->IsRuntimeMethod()) {
      return true;  // Ignore runtime frames (in particular callee save).
    }
    method_trace_->Set<kTransactionActive>(count_, m);
    dex_pc_trace_->Set<kTransactionActive>(count_,
        m->IsProxyMethod() ? DexFile::kDexNoIndex : GetDexPc());
    ++count_;
    return true;
  }

  mirror::ObjectArray<mirror::Object>* GetInternalStackTrace() const {
    return method_trace_;
  }

 private:
  Thread* const self_;
  // How many more frames to skip.
  int32_t skip_depth_;
  // Current position down stack trace.
  uint32_t count_;
  // Array of dex PC values.
  mirror::IntArray* dex_pc_trace_;
  // An array of the methods on the stack, the last entry is a reference to the PC trace.
  mirror::ObjectArray<mirror::Object>* method_trace_;
};

template<bool kTransactionActive>
jobject Thread::CreateInternalStackTrace(const ScopedObjectAccessUnchecked& soa) const {
  // Compute depth of stack
  CountStackDepthVisitor count_visitor(const_cast<Thread*>(this));
  count_visitor.WalkStack();
  int32_t depth = count_visitor.GetDepth();
  int32_t skip_depth = count_visitor.GetSkipDepth();

  // Build internal stack trace.
  BuildInternalStackTraceVisitor<kTransactionActive> build_trace_visitor(soa.Self(),
                                                                         const_cast<Thread*>(this),
                                                                         skip_depth);
  if (!build_trace_visitor.Init(depth)) {
    return nullptr;  // Allocation failed.
  }
  build_trace_visitor.WalkStack();
  mirror::ObjectArray<mirror::Object>* trace = build_trace_visitor.GetInternalStackTrace();
  if (kIsDebugBuild) {
    for (int32_t i = 0; i < trace->GetLength(); ++i) {
      CHECK(trace->Get(i) != nullptr);
    }
  }
  return soa.AddLocalReference<jobjectArray>(trace);
}
template jobject Thread::CreateInternalStackTrace<false>(const ScopedObjectAccessUnchecked& soa) const;
template jobject Thread::CreateInternalStackTrace<true>(const ScopedObjectAccessUnchecked& soa) const;

jobjectArray Thread::InternalStackTraceToStackTraceElementArray(const ScopedObjectAccess& soa,
    jobject internal, jobjectArray output_array, int* stack_depth) {
  // Decode the internal stack trace into the depth, method trace and PC trace
  int32_t depth = soa.Decode<mirror::ObjectArray<mirror::Object>*>(internal)->GetLength() - 1;

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();

  jobjectArray result;

  if (output_array != nullptr) {
    // Reuse the array we were given.
    result = output_array;
    // ...adjusting the number of frames we'll write to not exceed the array length.
    const int32_t traces_length =
        soa.Decode<mirror::ObjectArray<mirror::StackTraceElement>*>(result)->GetLength();
    depth = std::min(depth, traces_length);
  } else {
    // Create java_trace array and place in local reference table
    mirror::ObjectArray<mirror::StackTraceElement>* java_traces =
        class_linker->AllocStackTraceElementArray(soa.Self(), depth);
    if (java_traces == nullptr) {
      return nullptr;
    }
    result = soa.AddLocalReference<jobjectArray>(java_traces);
  }

  if (stack_depth != nullptr) {
    *stack_depth = depth;
  }

  for (int32_t i = 0; i < depth; ++i) {
    mirror::ObjectArray<mirror::Object>* method_trace =
          soa.Decode<mirror::ObjectArray<mirror::Object>*>(internal);
    // Prepare parameters for StackTraceElement(String cls, String method, String file, int line)
    mirror::ArtMethod* method = down_cast<mirror::ArtMethod*>(method_trace->Get(i));
    MethodHelper mh(method);
    int32_t line_number;
    StackHandleScope<3> hs(soa.Self());
    auto class_name_object(hs.NewHandle<mirror::String>(nullptr));
    auto source_name_object(hs.NewHandle<mirror::String>(nullptr));
    if (method->IsProxyMethod()) {
      line_number = -1;
      class_name_object.Assign(method->GetDeclaringClass()->GetName());
      // source_name_object intentionally left null for proxy methods
    } else {
      mirror::IntArray* pc_trace = down_cast<mirror::IntArray*>(method_trace->Get(depth));
      uint32_t dex_pc = pc_trace->Get(i);
      line_number = mh.GetLineNumFromDexPC(dex_pc);
      // Allocate element, potentially triggering GC
      // TODO: reuse class_name_object via Class::name_?
      const char* descriptor = mh.GetDeclaringClassDescriptor();
      CHECK(descriptor != nullptr);
      std::string class_name(PrettyDescriptor(descriptor));
      class_name_object.Assign(mirror::String::AllocFromModifiedUtf8(soa.Self(), class_name.c_str()));
      if (class_name_object.Get() == nullptr) {
        return nullptr;
      }
      const char* source_file = mh.GetDeclaringClassSourceFile();
      if (source_file != nullptr) {
        source_name_object.Assign(mirror::String::AllocFromModifiedUtf8(soa.Self(), source_file));
        if (source_name_object.Get() == nullptr) {
          return nullptr;
        }
      }
    }
    const char* method_name = mh.GetName();
    CHECK(method_name != nullptr);
    Handle<mirror::String> method_name_object(
        hs.NewHandle(mirror::String::AllocFromModifiedUtf8(soa.Self(), method_name)));
    if (method_name_object.Get() == nullptr) {
      return nullptr;
    }
    mirror::StackTraceElement* obj = mirror::StackTraceElement::Alloc(
        soa.Self(), class_name_object, method_name_object, source_name_object, line_number);
    if (obj == nullptr) {
      return nullptr;
    }
    // We are called from native: use non-transactional mode.
    soa.Decode<mirror::ObjectArray<mirror::StackTraceElement>*>(result)->Set<false>(i, obj);
  }
  return result;
}

void Thread::ThrowNewExceptionF(const ThrowLocation& throw_location,
                                const char* exception_class_descriptor, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  ThrowNewExceptionV(throw_location, exception_class_descriptor,
                     fmt, args);
  va_end(args);
}

void Thread::ThrowNewExceptionV(const ThrowLocation& throw_location,
                                const char* exception_class_descriptor,
                                const char* fmt, va_list ap) {
  std::string msg;
  StringAppendV(&msg, fmt, ap);
  ThrowNewException(throw_location, exception_class_descriptor, msg.c_str());
}

void Thread::ThrowNewException(const ThrowLocation& throw_location, const char* exception_class_descriptor,
                               const char* msg) {
  // Callers should either clear or call ThrowNewWrappedException.
  AssertNoPendingExceptionForNewException(msg);
  ThrowNewWrappedException(throw_location, exception_class_descriptor, msg);
}

void Thread::ThrowNewWrappedException(const ThrowLocation& throw_location,
                                      const char* exception_class_descriptor,
                                      const char* msg) {
  DCHECK_EQ(this, Thread::Current());
  ScopedObjectAccessUnchecked soa(this);
  StackHandleScope<5> hs(soa.Self());
  // Ensure we don't forget arguments over object allocation.
  Handle<mirror::Object> saved_throw_this(hs.NewHandle(throw_location.GetThis()));
  Handle<mirror::ArtMethod> saved_throw_method(hs.NewHandle(throw_location.GetMethod()));
  // Ignore the cause throw location. TODO: should we report this as a re-throw?
  ScopedLocalRef<jobject> cause(GetJniEnv(), soa.AddLocalReference<jobject>(GetException(nullptr)));
  ClearException();
  Runtime* runtime = Runtime::Current();

  mirror::ClassLoader* cl = nullptr;
  if (saved_throw_method.Get() != nullptr) {
    cl = saved_throw_method.Get()->GetDeclaringClass()->GetClassLoader();
  }
  Handle<mirror::ClassLoader> class_loader(hs.NewHandle(cl));
  Handle<mirror::Class> exception_class(
      hs.NewHandle(runtime->GetClassLinker()->FindClass(this, exception_class_descriptor,
                                                        class_loader)));
  if (UNLIKELY(exception_class.Get() == nullptr)) {
    CHECK(IsExceptionPending());
    LOG(ERROR) << "No exception class " << PrettyDescriptor(exception_class_descriptor);
    return;
  }

  if (UNLIKELY(!runtime->GetClassLinker()->EnsureInitialized(exception_class, true, true))) {
    DCHECK(IsExceptionPending());
    return;
  }
  DCHECK(!runtime->IsStarted() || exception_class->IsThrowableClass());
  Handle<mirror::Throwable> exception(
      hs.NewHandle(down_cast<mirror::Throwable*>(exception_class->AllocObject(this))));

  // If we couldn't allocate the exception, throw the pre-allocated out of memory exception.
  if (exception.Get() == nullptr) {
    ThrowLocation gc_safe_throw_location(saved_throw_this.Get(), saved_throw_method.Get(),
                                         throw_location.GetDexPc());
    SetException(gc_safe_throw_location, Runtime::Current()->GetPreAllocatedOutOfMemoryError());
    return;
  }

  // Choose an appropriate constructor and set up the arguments.
  const char* signature;
  ScopedLocalRef<jstring> msg_string(GetJniEnv(), nullptr);
  if (msg != nullptr) {
    // Ensure we remember this and the method over the String allocation.
    msg_string.reset(
        soa.AddLocalReference<jstring>(mirror::String::AllocFromModifiedUtf8(this, msg)));
    if (UNLIKELY(msg_string.get() == nullptr)) {
      CHECK(IsExceptionPending());  // OOME.
      return;
    }
    if (cause.get() == nullptr) {
      signature = "(Ljava/lang/String;)V";
    } else {
      signature = "(Ljava/lang/String;Ljava/lang/Throwable;)V";
    }
  } else {
    if (cause.get() == nullptr) {
      signature = "()V";
    } else {
      signature = "(Ljava/lang/Throwable;)V";
    }
  }
  mirror::ArtMethod* exception_init_method =
      exception_class->FindDeclaredDirectMethod("<init>", signature);

  CHECK(exception_init_method != nullptr) << "No <init>" << signature << " in "
      << PrettyDescriptor(exception_class_descriptor);

  if (UNLIKELY(!runtime->IsStarted())) {
    // Something is trying to throw an exception without a started runtime, which is the common
    // case in the compiler. We won't be able to invoke the constructor of the exception, so set
    // the exception fields directly.
    if (msg != nullptr) {
      exception->SetDetailMessage(down_cast<mirror::String*>(DecodeJObject(msg_string.get())));
    }
    if (cause.get() != nullptr) {
      exception->SetCause(down_cast<mirror::Throwable*>(DecodeJObject(cause.get())));
    }
    ScopedLocalRef<jobject> trace(GetJniEnv(),
                                  Runtime::Current()->IsActiveTransaction()
                                      ? CreateInternalStackTrace<true>(soa)
                                      : CreateInternalStackTrace<false>(soa));
    if (trace.get() != nullptr) {
      exception->SetStackState(down_cast<mirror::Throwable*>(DecodeJObject(trace.get())));
    }
    ThrowLocation gc_safe_throw_location(saved_throw_this.Get(), saved_throw_method.Get(),
                                         throw_location.GetDexPc());
    SetException(gc_safe_throw_location, exception.Get());
  } else {
    jvalue jv_args[2];
    size_t i = 0;

    if (msg != nullptr) {
      jv_args[i].l = msg_string.get();
      ++i;
    }
    if (cause.get() != nullptr) {
      jv_args[i].l = cause.get();
      ++i;
    }
    InvokeWithJValues(soa, exception.Get(), soa.EncodeMethod(exception_init_method), jv_args);
    if (LIKELY(!IsExceptionPending())) {
      ThrowLocation gc_safe_throw_location(saved_throw_this.Get(), saved_throw_method.Get(),
                                           throw_location.GetDexPc());
      SetException(gc_safe_throw_location, exception.Get());
    }
  }
}

void Thread::ThrowOutOfMemoryError(const char* msg) {
  LOG(ERROR) << StringPrintf("Throwing OutOfMemoryError \"%s\"%s",
      msg, (tls32_.throwing_OutOfMemoryError ? " (recursive case)" : ""));
  ThrowLocation throw_location = GetCurrentLocationForThrow();
  if (!tls32_.throwing_OutOfMemoryError) {
    tls32_.throwing_OutOfMemoryError = true;
    ThrowNewException(throw_location, "Ljava/lang/OutOfMemoryError;", msg);
    tls32_.throwing_OutOfMemoryError = false;
  } else {
    Dump(LOG(ERROR));  // The pre-allocated OOME has no stack, so help out and log one.
    SetException(throw_location, Runtime::Current()->GetPreAllocatedOutOfMemoryError());
  }
}

Thread* Thread::CurrentFromGdb() {
  return Thread::Current();
}

void Thread::DumpFromGdb() const {
  std::ostringstream ss;
  Dump(ss);
  std::string str(ss.str());
  // log to stderr for debugging command line processes
  std::cerr << str;
#ifdef HAVE_ANDROID_OS
  // log to logcat for debugging frameworks processes
  LOG(INFO) << str;
#endif
}

// Explicitly instantiate 32 and 64bit thread offset dumping support.
template void Thread::DumpThreadOffset<4>(std::ostream& os, uint32_t offset);
template void Thread::DumpThreadOffset<8>(std::ostream& os, uint32_t offset);

template<size_t ptr_size>
void Thread::DumpThreadOffset(std::ostream& os, uint32_t offset) {
#define DO_THREAD_OFFSET(x, y) \
    if (offset == x.Uint32Value()) { \
      os << y; \
      return; \
    }
  DO_THREAD_OFFSET(ThreadFlagsOffset<ptr_size>(), "state_and_flags")
  DO_THREAD_OFFSET(CardTableOffset<ptr_size>(), "card_table")
  DO_THREAD_OFFSET(ExceptionOffset<ptr_size>(), "exception")
  DO_THREAD_OFFSET(PeerOffset<ptr_size>(), "peer");
  DO_THREAD_OFFSET(JniEnvOffset<ptr_size>(), "jni_env")
  DO_THREAD_OFFSET(SelfOffset<ptr_size>(), "self")
  DO_THREAD_OFFSET(StackEndOffset<ptr_size>(), "stack_end")
  DO_THREAD_OFFSET(ThinLockIdOffset<ptr_size>(), "thin_lock_thread_id")
  DO_THREAD_OFFSET(TopOfManagedStackOffset<ptr_size>(), "top_quick_frame_method")
  DO_THREAD_OFFSET(TopOfManagedStackPcOffset<ptr_size>(), "top_quick_frame_pc")
  DO_THREAD_OFFSET(TopShadowFrameOffset<ptr_size>(), "top_shadow_frame")
  DO_THREAD_OFFSET(TopHandleScopeOffset<ptr_size>(), "top_handle_scope")
  DO_THREAD_OFFSET(ThreadSuspendTriggerOffset<ptr_size>(), "suspend_trigger")
#undef DO_THREAD_OFFSET

#define INTERPRETER_ENTRY_POINT_INFO(x) \
    if (INTERPRETER_ENTRYPOINT_OFFSET(ptr_size, x).Uint32Value() == offset) { \
      os << #x; \
      return; \
    }
  INTERPRETER_ENTRY_POINT_INFO(pInterpreterToInterpreterBridge)
  INTERPRETER_ENTRY_POINT_INFO(pInterpreterToCompiledCodeBridge)
#undef INTERPRETER_ENTRY_POINT_INFO

#define JNI_ENTRY_POINT_INFO(x) \
    if (JNI_ENTRYPOINT_OFFSET(ptr_size, x).Uint32Value() == offset) { \
      os << #x; \
      return; \
    }
  JNI_ENTRY_POINT_INFO(pDlsymLookup)
#undef JNI_ENTRY_POINT_INFO

#define PORTABLE_ENTRY_POINT_INFO(x) \
    if (PORTABLE_ENTRYPOINT_OFFSET(ptr_size, x).Uint32Value() == offset) { \
      os << #x; \
      return; \
    }
  PORTABLE_ENTRY_POINT_INFO(pPortableImtConflictTrampoline)
  PORTABLE_ENTRY_POINT_INFO(pPortableResolutionTrampoline)
  PORTABLE_ENTRY_POINT_INFO(pPortableToInterpreterBridge)
#undef PORTABLE_ENTRY_POINT_INFO

#define QUICK_ENTRY_POINT_INFO(x) \
    if (QUICK_ENTRYPOINT_OFFSET(ptr_size, x).Uint32Value() == offset) { \
      os << #x; \
      return; \
    }
  QUICK_ENTRY_POINT_INFO(pAllocArray)
  QUICK_ENTRY_POINT_INFO(pAllocArrayResolved)
  QUICK_ENTRY_POINT_INFO(pAllocArrayWithAccessCheck)
  QUICK_ENTRY_POINT_INFO(pAllocObject)
  QUICK_ENTRY_POINT_INFO(pAllocObjectResolved)
  QUICK_ENTRY_POINT_INFO(pAllocObjectInitialized)
  QUICK_ENTRY_POINT_INFO(pAllocObjectWithAccessCheck)
  QUICK_ENTRY_POINT_INFO(pCheckAndAllocArray)
  QUICK_ENTRY_POINT_INFO(pCheckAndAllocArrayWithAccessCheck)
  QUICK_ENTRY_POINT_INFO(pInstanceofNonTrivial)
  QUICK_ENTRY_POINT_INFO(pCheckCast)
  QUICK_ENTRY_POINT_INFO(pInitializeStaticStorage)
  QUICK_ENTRY_POINT_INFO(pInitializeTypeAndVerifyAccess)
  QUICK_ENTRY_POINT_INFO(pInitializeType)
  QUICK_ENTRY_POINT_INFO(pResolveString)
  QUICK_ENTRY_POINT_INFO(pSet32Instance)
  QUICK_ENTRY_POINT_INFO(pSet32Static)
  QUICK_ENTRY_POINT_INFO(pSet64Instance)
  QUICK_ENTRY_POINT_INFO(pSet64Static)
  QUICK_ENTRY_POINT_INFO(pSetObjInstance)
  QUICK_ENTRY_POINT_INFO(pSetObjStatic)
  QUICK_ENTRY_POINT_INFO(pGet32Instance)
  QUICK_ENTRY_POINT_INFO(pGet32Static)
  QUICK_ENTRY_POINT_INFO(pGet64Instance)
  QUICK_ENTRY_POINT_INFO(pGet64Static)
  QUICK_ENTRY_POINT_INFO(pGetObjInstance)
  QUICK_ENTRY_POINT_INFO(pGetObjStatic)
  QUICK_ENTRY_POINT_INFO(pAputObjectWithNullAndBoundCheck)
  QUICK_ENTRY_POINT_INFO(pAputObjectWithBoundCheck)
  QUICK_ENTRY_POINT_INFO(pAputObject)
  QUICK_ENTRY_POINT_INFO(pHandleFillArrayData)
  QUICK_ENTRY_POINT_INFO(pJniMethodStart)
  QUICK_ENTRY_POINT_INFO(pJniMethodStartSynchronized)
  QUICK_ENTRY_POINT_INFO(pJniMethodEnd)
  QUICK_ENTRY_POINT_INFO(pJniMethodEndSynchronized)
  QUICK_ENTRY_POINT_INFO(pJniMethodEndWithReference)
  QUICK_ENTRY_POINT_INFO(pJniMethodEndWithReferenceSynchronized)
  QUICK_ENTRY_POINT_INFO(pQuickGenericJniTrampoline)
  QUICK_ENTRY_POINT_INFO(pLockObject)
  QUICK_ENTRY_POINT_INFO(pUnlockObject)
  QUICK_ENTRY_POINT_INFO(pCmpgDouble)
  QUICK_ENTRY_POINT_INFO(pCmpgFloat)
  QUICK_ENTRY_POINT_INFO(pCmplDouble)
  QUICK_ENTRY_POINT_INFO(pCmplFloat)
  QUICK_ENTRY_POINT_INFO(pFmod)
  QUICK_ENTRY_POINT_INFO(pSqrt)
  QUICK_ENTRY_POINT_INFO(pL2d)
  QUICK_ENTRY_POINT_INFO(pFmodf)
  QUICK_ENTRY_POINT_INFO(pL2f)
  QUICK_ENTRY_POINT_INFO(pD2iz)
  QUICK_ENTRY_POINT_INFO(pF2iz)
  QUICK_ENTRY_POINT_INFO(pIdivmod)
  QUICK_ENTRY_POINT_INFO(pD2l)
  QUICK_ENTRY_POINT_INFO(pF2l)
  QUICK_ENTRY_POINT_INFO(pLdiv)
  QUICK_ENTRY_POINT_INFO(pLmod)
  QUICK_ENTRY_POINT_INFO(pLmul)
  QUICK_ENTRY_POINT_INFO(pShlLong)
  QUICK_ENTRY_POINT_INFO(pShrLong)
  QUICK_ENTRY_POINT_INFO(pUshrLong)
  QUICK_ENTRY_POINT_INFO(pIndexOf)
  QUICK_ENTRY_POINT_INFO(pMemcmp16)
  QUICK_ENTRY_POINT_INFO(pStringCompareTo)
  QUICK_ENTRY_POINT_INFO(pMemcpy)
  QUICK_ENTRY_POINT_INFO(pQuickImtConflictTrampoline)
  QUICK_ENTRY_POINT_INFO(pQuickResolutionTrampoline)
  QUICK_ENTRY_POINT_INFO(pQuickToInterpreterBridge)
  QUICK_ENTRY_POINT_INFO(pInvokeDirectTrampolineWithAccessCheck)
  QUICK_ENTRY_POINT_INFO(pInvokeInterfaceTrampolineWithAccessCheck)
  QUICK_ENTRY_POINT_INFO(pInvokeStaticTrampolineWithAccessCheck)
  QUICK_ENTRY_POINT_INFO(pInvokeSuperTrampolineWithAccessCheck)
  QUICK_ENTRY_POINT_INFO(pInvokeVirtualTrampolineWithAccessCheck)
  QUICK_ENTRY_POINT_INFO(pCheckSuspend)
  QUICK_ENTRY_POINT_INFO(pTestSuspend)
  QUICK_ENTRY_POINT_INFO(pDeliverException)
  QUICK_ENTRY_POINT_INFO(pThrowArrayBounds)
  QUICK_ENTRY_POINT_INFO(pThrowDivZero)
  QUICK_ENTRY_POINT_INFO(pThrowNoSuchMethod)
  QUICK_ENTRY_POINT_INFO(pThrowNullPointer)
  QUICK_ENTRY_POINT_INFO(pThrowStackOverflow)
#undef QUICK_ENTRY_POINT_INFO

  os << offset;
}

void Thread::QuickDeliverException() {
  // Get exception from thread.
  ThrowLocation throw_location;
  mirror::Throwable* exception = GetException(&throw_location);
  CHECK(exception != nullptr);
  // Don't leave exception visible while we try to find the handler, which may cause class
  // resolution.
  ClearException();
  bool is_deoptimization = (exception == GetDeoptimizationException());
  if (kDebugExceptionDelivery) {
    if (!is_deoptimization) {
      mirror::String* msg = exception->GetDetailMessage();
      std::string str_msg(msg != nullptr ? msg->ToModifiedUtf8() : "");
      DumpStack(LOG(INFO) << "Delivering exception: " << PrettyTypeOf(exception)
                << ": " << str_msg << "\n");
    } else {
      DumpStack(LOG(INFO) << "Deoptimizing: ");
    }
  }
  QuickExceptionHandler exception_handler(this, is_deoptimization);
  if (is_deoptimization) {
    exception_handler.DeoptimizeStack();
  } else {
    exception_handler.FindCatch(throw_location, exception);
  }
  exception_handler.UpdateInstrumentationStack();
  exception_handler.DoLongJump();
  LOG(FATAL) << "UNREACHABLE";
}

Context* Thread::GetLongJumpContext() {
  Context* result = tlsPtr_.long_jump_context;
  if (result == nullptr) {
    result = Context::Create();
  } else {
    tlsPtr_.long_jump_context = nullptr;  // Avoid context being shared.
    result->Reset();
  }
  return result;
}

struct CurrentMethodVisitor FINAL : public StackVisitor {
  CurrentMethodVisitor(Thread* thread, Context* context)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : StackVisitor(thread, context), this_object_(nullptr), method_(nullptr), dex_pc_(0) {}
  bool VisitFrame() OVERRIDE SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    mirror::ArtMethod* m = GetMethod();
    if (m->IsRuntimeMethod()) {
      // Continue if this is a runtime method.
      return true;
    }
    if (context_ != nullptr) {
      this_object_ = GetThisObject();
    }
    method_ = m;
    dex_pc_ = GetDexPc();
    return false;
  }
  mirror::Object* this_object_;
  mirror::ArtMethod* method_;
  uint32_t dex_pc_;
};

mirror::ArtMethod* Thread::GetCurrentMethod(uint32_t* dex_pc) const {
  CurrentMethodVisitor visitor(const_cast<Thread*>(this), nullptr);
  visitor.WalkStack(false);
  if (dex_pc != nullptr) {
    *dex_pc = visitor.dex_pc_;
  }
  return visitor.method_;
}

ThrowLocation Thread::GetCurrentLocationForThrow() {
  Context* context = GetLongJumpContext();
  CurrentMethodVisitor visitor(this, context);
  visitor.WalkStack(false);
  ReleaseLongJumpContext(context);
  return ThrowLocation(visitor.this_object_, visitor.method_, visitor.dex_pc_);
}

bool Thread::HoldsLock(mirror::Object* object) const {
  if (object == nullptr) {
    return false;
  }
  return object->GetLockOwnerThreadId() == GetThreadId();
}

// RootVisitor parameters are: (const Object* obj, size_t vreg, const StackVisitor* visitor).
template <typename RootVisitor>
class ReferenceMapVisitor : public StackVisitor {
 public:
  ReferenceMapVisitor(Thread* thread, Context* context, const RootVisitor& visitor)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : StackVisitor(thread, context), visitor_(visitor) {}

  bool VisitFrame() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    if (false) {
      LOG(INFO) << "Visiting stack roots in " << PrettyMethod(GetMethod())
                << StringPrintf("@ PC:%04x", GetDexPc());
    }
    ShadowFrame* shadow_frame = GetCurrentShadowFrame();
    if (shadow_frame != nullptr) {
      VisitShadowFrame(shadow_frame);
    } else {
      VisitQuickFrame();
    }
    return true;
  }

  void VisitShadowFrame(ShadowFrame* shadow_frame) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    mirror::ArtMethod* m = shadow_frame->GetMethod();
    size_t num_regs = shadow_frame->NumberOfVRegs();
    if (m->IsNative() || shadow_frame->HasReferenceArray()) {
      // handle scope for JNI or References for interpreter.
      for (size_t reg = 0; reg < num_regs; ++reg) {
        mirror::Object* ref = shadow_frame->GetVRegReference(reg);
        if (ref != nullptr) {
          mirror::Object* new_ref = ref;
          visitor_(&new_ref, reg, this);
          if (new_ref != ref) {
            shadow_frame->SetVRegReference(reg, new_ref);
          }
        }
      }
    } else {
      // Java method.
      // Portable path use DexGcMap and store in Method.native_gc_map_.
      const uint8_t* gc_map = m->GetNativeGcMap();
      CHECK(gc_map != nullptr) << PrettyMethod(m);
      verifier::DexPcToReferenceMap dex_gc_map(gc_map);
      uint32_t dex_pc = shadow_frame->GetDexPC();
      const uint8_t* reg_bitmap = dex_gc_map.FindBitMap(dex_pc);
      DCHECK(reg_bitmap != nullptr);
      num_regs = std::min(dex_gc_map.RegWidth() * 8, num_regs);
      for (size_t reg = 0; reg < num_regs; ++reg) {
        if (TestBitmap(reg, reg_bitmap)) {
          mirror::Object* ref = shadow_frame->GetVRegReference(reg);
          if (ref != nullptr) {
            mirror::Object* new_ref = ref;
            visitor_(&new_ref, reg, this);
            if (new_ref != ref) {
              shadow_frame->SetVRegReference(reg, new_ref);
            }
          }
        }
      }
    }
  }

 private:
  void VisitQuickFrame() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    mirror::ArtMethod* m = GetMethod();
    // Process register map (which native and runtime methods don't have)
    if (!m->IsNative() && !m->IsRuntimeMethod() && !m->IsProxyMethod()) {
      const uint8_t* native_gc_map = m->GetNativeGcMap();
      CHECK(native_gc_map != nullptr) << PrettyMethod(m);
      mh_.ChangeMethod(m);
      const DexFile::CodeItem* code_item = mh_.GetCodeItem();
      DCHECK(code_item != nullptr) << PrettyMethod(m);  // Can't be nullptr or how would we compile its instructions?
      NativePcOffsetToReferenceMap map(native_gc_map);
      size_t num_regs = std::min(map.RegWidth() * 8,
                                 static_cast<size_t>(code_item->registers_size_));
      if (num_regs > 0) {
        const uint8_t* reg_bitmap = map.FindBitMap(GetNativePcOffset());
        DCHECK(reg_bitmap != nullptr);
        const VmapTable vmap_table(m->GetVmapTable());
        QuickMethodFrameInfo frame_info = m->GetQuickFrameInfo();
        // For all dex registers in the bitmap
        mirror::ArtMethod** cur_quick_frame = GetCurrentQuickFrame();
        DCHECK(cur_quick_frame != nullptr);
        for (size_t reg = 0; reg < num_regs; ++reg) {
          // Does this register hold a reference?
          if (TestBitmap(reg, reg_bitmap)) {
            uint32_t vmap_offset;
            if (vmap_table.IsInContext(reg, kReferenceVReg, &vmap_offset)) {
              int vmap_reg = vmap_table.ComputeRegister(frame_info.CoreSpillMask(), vmap_offset,
                                                        kReferenceVReg);
              // This is sound as spilled GPRs will be word sized (ie 32 or 64bit).
              mirror::Object** ref_addr = reinterpret_cast<mirror::Object**>(GetGPRAddress(vmap_reg));
              if (*ref_addr != nullptr) {
                visitor_(ref_addr, reg, this);
              }
            } else {
              StackReference<mirror::Object>* ref_addr =
                  reinterpret_cast<StackReference<mirror::Object>*>(
                      GetVRegAddr(cur_quick_frame, code_item, frame_info.CoreSpillMask(),
                                  frame_info.FpSpillMask(), frame_info.FrameSizeInBytes(), reg));
              mirror::Object* ref = ref_addr->AsMirrorPtr();
              if (ref != nullptr) {
                mirror::Object* new_ref = ref;
                visitor_(&new_ref, reg, this);
                if (ref != new_ref) {
                  ref_addr->Assign(new_ref);
                }
              }
            }
          }
        }
      }
    }
  }

  static bool TestBitmap(size_t reg, const uint8_t* reg_vector) {
    return ((reg_vector[reg / kBitsPerByte] >> (reg % kBitsPerByte)) & 0x01) != 0;
  }

  // Visitor for when we visit a root.
  const RootVisitor& visitor_;

  // A method helper we keep around to avoid dex file/cache re-computations.
  MethodHelper mh_;
};

class RootCallbackVisitor {
 public:
  RootCallbackVisitor(RootCallback* callback, void* arg, uint32_t tid)
     : callback_(callback), arg_(arg), tid_(tid) {}

  void operator()(mirror::Object** obj, size_t, const StackVisitor*) const {
    callback_(obj, arg_, tid_, kRootJavaFrame);
  }

 private:
  RootCallback* const callback_;
  void* const arg_;
  const uint32_t tid_;
};

void Thread::SetClassLoaderOverride(mirror::ClassLoader* class_loader_override) {
  VerifyObject(class_loader_override);
  tlsPtr_.class_loader_override = class_loader_override;
}

void Thread::VisitRoots(RootCallback* visitor, void* arg) {
  uint32_t thread_id = GetThreadId();
  if (tlsPtr_.opeer != nullptr) {
    visitor(&tlsPtr_.opeer, arg, thread_id, kRootThreadObject);
  }
  if (tlsPtr_.exception != nullptr && tlsPtr_.exception != GetDeoptimizationException()) {
    visitor(reinterpret_cast<mirror::Object**>(&tlsPtr_.exception), arg, thread_id, kRootNativeStack);
  }
  tlsPtr_.throw_location.VisitRoots(visitor, arg);
  if (tlsPtr_.class_loader_override != nullptr) {
    visitor(reinterpret_cast<mirror::Object**>(&tlsPtr_.class_loader_override), arg, thread_id,
            kRootNativeStack);
  }
  if (tlsPtr_.monitor_enter_object != nullptr) {
    visitor(&tlsPtr_.monitor_enter_object, arg, thread_id, kRootNativeStack);
  }
  tlsPtr_.jni_env->locals.VisitRoots(visitor, arg, thread_id, kRootJNILocal);
  tlsPtr_.jni_env->monitors.VisitRoots(visitor, arg, thread_id, kRootJNIMonitor);
  HandleScopeVisitRoots(visitor, arg, thread_id);
  if (tlsPtr_.debug_invoke_req != nullptr) {
    tlsPtr_.debug_invoke_req->VisitRoots(visitor, arg, thread_id, kRootDebugger);
  }
  if (tlsPtr_.single_step_control != nullptr) {
    tlsPtr_.single_step_control->VisitRoots(visitor, arg, thread_id, kRootDebugger);
  }
  if (tlsPtr_.deoptimization_shadow_frame != nullptr) {
    RootCallbackVisitor visitorToCallback(visitor, arg, thread_id);
    ReferenceMapVisitor<RootCallbackVisitor> mapper(this, nullptr, visitorToCallback);
    for (ShadowFrame* shadow_frame = tlsPtr_.deoptimization_shadow_frame; shadow_frame != nullptr;
        shadow_frame = shadow_frame->GetLink()) {
      mapper.VisitShadowFrame(shadow_frame);
    }
  }
  // Visit roots on this thread's stack
  Context* context = GetLongJumpContext();
  RootCallbackVisitor visitorToCallback(visitor, arg, thread_id);
  ReferenceMapVisitor<RootCallbackVisitor> mapper(this, context, visitorToCallback);
  mapper.WalkStack();
  ReleaseLongJumpContext(context);
  for (instrumentation::InstrumentationStackFrame& frame : *GetInstrumentationStack()) {
    if (frame.this_object_ != nullptr) {
      visitor(&frame.this_object_, arg, thread_id, kRootJavaFrame);
    }
    DCHECK(frame.method_ != nullptr);
    visitor(reinterpret_cast<mirror::Object**>(&frame.method_), arg, thread_id, kRootJavaFrame);
  }
}

static void VerifyRoot(mirror::Object** root, void* /*arg*/, uint32_t /*thread_id*/,
                       RootType /*root_type*/) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  VerifyObject(*root);
}

void Thread::VerifyStackImpl() {
  UniquePtr<Context> context(Context::Create());
  RootCallbackVisitor visitorToCallback(VerifyRoot, Runtime::Current()->GetHeap(), GetThreadId());
  ReferenceMapVisitor<RootCallbackVisitor> mapper(this, context.get(), visitorToCallback);
  mapper.WalkStack();
}

// Set the stack end to that to be used during a stack overflow
void Thread::SetStackEndForStackOverflow() {
  // During stack overflow we allow use of the full stack.
  if (tlsPtr_.stack_end == tlsPtr_.stack_begin) {
    // However, we seem to have already extended to use the full stack.
    LOG(ERROR) << "Need to increase kStackOverflowReservedBytes (currently "
               << kStackOverflowReservedBytes << ")?";
    DumpStack(LOG(ERROR));
    LOG(FATAL) << "Recursive stack overflow.";
  }

  tlsPtr_.stack_end = tlsPtr_.stack_begin;
}

void Thread::SetTlab(byte* start, byte* end) {
  DCHECK_LE(start, end);
  tlsPtr_.thread_local_start = start;
  tlsPtr_.thread_local_pos  = tlsPtr_.thread_local_start;
  tlsPtr_.thread_local_end = end;
  tlsPtr_.thread_local_objects = 0;
}

bool Thread::HasTlab() const {
  bool has_tlab = tlsPtr_.thread_local_pos != nullptr;
  if (has_tlab) {
    DCHECK(tlsPtr_.thread_local_start != nullptr && tlsPtr_.thread_local_end != nullptr);
  } else {
    DCHECK(tlsPtr_.thread_local_start == nullptr && tlsPtr_.thread_local_end == nullptr);
  }
  return has_tlab;
}

std::ostream& operator<<(std::ostream& os, const Thread& thread) {
  thread.ShortDump(os);
  return os;
}

}  // namespace art
