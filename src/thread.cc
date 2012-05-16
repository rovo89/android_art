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

#include "thread.h"

#include <dynamic_annotations.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/time.h>

#include <algorithm>
#include <bitset>
#include <cerrno>
#include <iostream>
#include <list>

#include "debugger.h"
#include "class_linker.h"
#include "class_loader.h"
#include "heap.h"
#include "jni_internal.h"
#include "monitor.h"
#include "oat/runtime/context.h"
#include "object.h"
#include "object_utils.h"
#include "reflection.h"
#include "runtime.h"
#include "runtime_support.h"
#include "ScopedLocalRef.h"
#include "scoped_jni_thread_state.h"
#include "shadow_frame.h"
#include "space.h"
#include "stack.h"
#include "stack_indirect_reference_table.h"
#include "thread_list.h"
#include "utils.h"
#include "verifier/gc_map.h"

namespace art {

pthread_key_t Thread::pthread_key_self_;

static Class* gThreadGroup = NULL;
static Class* gThreadLock = NULL;
static Field* gThread_daemon = NULL;
static Field* gThread_group = NULL;
static Field* gThread_lock = NULL;
static Field* gThread_name = NULL;
static Field* gThread_priority = NULL;
static Field* gThread_uncaughtHandler = NULL;
static Field* gThread_vmData = NULL;
static Field* gThreadGroup_mMain = NULL;
static Field* gThreadGroup_mSystem = NULL;
static Field* gThreadGroup_name = NULL;
static Field* gThreadLock_thread = NULL;
static Method* gThread_run = NULL;
static Method* gThreadGroup_removeThread = NULL;
static Method* gUncaughtExceptionHandler_uncaughtException = NULL;

static const char* kThreadNameDuringStartup = "<native thread without managed peer>";

void Thread::InitCardTable() {
  card_table_ = Runtime::Current()->GetHeap()->GetCardTable()->GetBiasedBegin();
}

#if !defined(__APPLE__)
static void UnimplementedEntryPoint() {
  UNIMPLEMENTED(FATAL);
}
#endif

void Thread::InitFunctionPointers() {
#if !defined(__APPLE__) // The Mac GCC is too old to accept this code.
  // Insert a placeholder so we can easily tell if we call an unimplemented entry point.
  uintptr_t* begin = reinterpret_cast<uintptr_t*>(&entrypoints_);
  uintptr_t* end = reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(begin) + sizeof(entrypoints_));
  for (uintptr_t* it = begin; it != end; ++it) {
    *it = reinterpret_cast<uintptr_t>(UnimplementedEntryPoint);
  }
#endif
  InitEntryPoints(&entrypoints_);
}

void Thread::SetDebuggerUpdatesEnabled(bool enabled) {
  LOG(INFO) << "Turning debugger updates " << (enabled ? "on" : "off") << " for " << *this;
#if !defined(ART_USE_LLVM_COMPILER)
  ChangeDebuggerEntryPoint(&entrypoints_, enabled);
#else
  UNIMPLEMENTED(FATAL);
#endif
}

void Thread::InitTid() {
  tid_ = ::art::GetTid();
}

void Thread::InitAfterFork() {
  InitTid();

#if defined(__BIONIC__)
  // Work around a bionic bug.
  struct bionic_pthread_internal_t {
    void*  next;
    void** pref;
    pthread_attr_t attr;
    pid_t kernel_id;
    // et cetera. we just need 'kernel_id' so we can stop here.
  };
  bionic_pthread_internal_t* self = reinterpret_cast<bionic_pthread_internal_t*>(pthread_self());
  if (self->kernel_id == tid_) {
    // TODO: if you see this logging, you can remove this code!
    LOG(INFO) << "*** this tree doesn't have the bionic pthread kernel_id bug";
  }
  self->kernel_id = tid_;
#endif
}

void* Thread::CreateCallback(void* arg) {
  Thread* self = reinterpret_cast<Thread*>(arg);
  self->Init();

  // Wait until it's safe to start running code. (There may have been a suspend-all
  // in progress while we were starting up.)
  Runtime* runtime = Runtime::Current();
  runtime->GetThreadList()->WaitForGo();

  {
    CHECK_EQ(self->GetState(), kRunnable);
    SirtRef<String> thread_name(self->GetThreadName());
    self->SetThreadName(thread_name->ToModifiedUtf8().c_str());
  }

  Dbg::PostThreadStart(self);

  // Invoke the 'run' method of our java.lang.Thread.
  CHECK(self->peer_ != NULL);
  Object* receiver = self->peer_;
  Method* m = receiver->GetClass()->FindVirtualMethodForVirtualOrInterface(gThread_run);
  m->Invoke(self, receiver, NULL, NULL);

  // Detach.
  runtime->GetThreadList()->Unregister();

  return NULL;
}

static void SetVmData(Object* managed_thread, Thread* native_thread) {
  gThread_vmData->SetInt(managed_thread, reinterpret_cast<uintptr_t>(native_thread));
}

Thread* Thread::FromManagedThread(Object* thread_peer) {
  return reinterpret_cast<Thread*>(static_cast<uintptr_t>(gThread_vmData->GetInt(thread_peer)));
}

Thread* Thread::FromManagedThread(JNIEnv* env, jobject java_thread) {
  return FromManagedThread(Decode<Object*>(env, java_thread));
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

  // It's likely that callers are trying to ensure they have at least a certain amount of
  // stack space, so we should add our reserved space on top of what they requested, rather
  // than implicitly take it away from them.
  stack_size += Thread::kStackOverflowReservedBytes;

  // Some systems require the stack size to be a multiple of the system page size, so round up.
  stack_size = RoundUp(stack_size, kPageSize);

  return stack_size;
}

static void SigAltStack(stack_t* new_stack, stack_t* old_stack) {
  if (sigaltstack(new_stack, old_stack) == -1) {
    PLOG(FATAL) << "sigaltstack failed";
  }
}

static void SetUpAlternateSignalStack() {
  // Create and set an alternate signal stack.
  stack_t ss;
  ss.ss_sp = new uint8_t[SIGSTKSZ];
  ss.ss_size = SIGSTKSZ;
  ss.ss_flags = 0;
  CHECK(ss.ss_sp != NULL);
  SigAltStack(&ss, NULL);

  // Double-check that it worked.
  ss.ss_sp = NULL;
  SigAltStack(NULL, &ss);
  VLOG(threads) << "Alternate signal stack is " << PrettySize(ss.ss_size) << " at " << ss.ss_sp;
}

static void TearDownAlternateSignalStack() {
  // Get the pointer so we can free the memory.
  stack_t ss;
  SigAltStack(NULL, &ss);
  uint8_t* allocated_signal_stack = reinterpret_cast<uint8_t*>(ss.ss_sp);

  // Tell the kernel to stop using it.
  ss.ss_sp = NULL;
  ss.ss_flags = SS_DISABLE;
  ss.ss_size = SIGSTKSZ; // Avoid ENOMEM failure with Mac OS' buggy libc.
  SigAltStack(&ss, NULL);

  // Free it.
  delete[] allocated_signal_stack;
}

void Thread::Create(Object* peer, size_t stack_size) {
  CHECK(peer != NULL);

  stack_size = FixStackSize(stack_size);

  Thread* native_thread = new Thread;
  native_thread->peer_ = peer;

  // Thread.start is synchronized, so we know that vmData is 0,
  // and know that we're not racing to assign it.
  SetVmData(peer, native_thread);

  {
    ScopedThreadStateChange tsc(Thread::Current(), kVmWait);
    pthread_t new_pthread;
    pthread_attr_t attr;
    CHECK_PTHREAD_CALL(pthread_attr_init, (&attr), "new thread");
    CHECK_PTHREAD_CALL(pthread_attr_setdetachstate, (&attr, PTHREAD_CREATE_DETACHED), "PTHREAD_CREATE_DETACHED");
    CHECK_PTHREAD_CALL(pthread_attr_setstacksize, (&attr, stack_size), stack_size);
    CHECK_PTHREAD_CALL(pthread_create, (&new_pthread, &attr, Thread::CreateCallback, native_thread), "new thread");
    CHECK_PTHREAD_CALL(pthread_attr_destroy, (&attr), "new thread");
  }

  // Let the child know when it's safe to start running.
  Runtime::Current()->GetThreadList()->SignalGo(native_thread);
}

void Thread::Init() {
  // This function does all the initialization that must be run by the native thread it applies to.
  // (When we create a new thread from managed code, we allocate the Thread* in Thread::Create so
  // we can handshake with the corresponding native thread when it's ready.) Check this native
  // thread hasn't been through here already...
  CHECK(Thread::Current() == NULL);

  SetUpAlternateSignalStack();
  InitCpu();
  InitFunctionPointers();
  InitCardTable();

  Runtime* runtime = Runtime::Current();
  CHECK(runtime != NULL);

  thin_lock_id_ = runtime->GetThreadList()->AllocThreadId();

  InitTid();
  InitStackHwm();

  CHECK_PTHREAD_CALL(pthread_setspecific, (Thread::pthread_key_self_, this), "attach");

  jni_env_ = new JNIEnvExt(this, runtime->GetJavaVM());

  runtime->GetThreadList()->Register();
}

Thread* Thread::Attach(const char* thread_name, bool as_daemon, Object* thread_group) {
  Thread* self = new Thread;
  self->Init();

  self->SetState(kNative);

  // If we're the main thread, ClassLinker won't be created until after we're attached,
  // so that thread needs a two-stage attach. Regular threads don't need this hack.
  // In the compiler, all threads need this hack, because no-one's going to be getting
  // a native peer!
  if (self->thin_lock_id_ != ThreadList::kMainId && !Runtime::Current()->IsCompiler()) {
    self->CreatePeer(thread_name, as_daemon, thread_group);
  } else {
    // These aren't necessary, but they improve diagnostics for unit tests & command-line tools.
    if (thread_name != NULL) {
      self->name_->assign(thread_name);
      ::art::SetThreadName(thread_name);
    }
  }

  self->GetJniEnv()->locals.AssertEmpty();
  return self;
}

Object* Thread::GetMainThreadGroup() {
  if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(gThreadGroup, true, true)) {
    return NULL;
  }
  return gThreadGroup_mMain->GetObject(NULL);
}

Object* Thread::GetSystemThreadGroup() {
  if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(gThreadGroup, true, true)) {
    return NULL;
  }
  return gThreadGroup_mSystem->GetObject(NULL);
}

void Thread::CreatePeer(const char* name, bool as_daemon, Object* thread_group) {
  CHECK(Runtime::Current()->IsStarted());
  JNIEnv* env = jni_env_;

  if (thread_group == NULL) {
    thread_group = Thread::GetMainThreadGroup();
  }
  ScopedLocalRef<jobject> java_thread_group(env, AddLocalReference<jobject>(env, thread_group));
  ScopedLocalRef<jobject> thread_name(env, env->NewStringUTF(name));
  jint thread_priority = GetNativePriority();
  jboolean thread_is_daemon = as_daemon;

  ScopedLocalRef<jclass> c(env, env->FindClass("java/lang/Thread"));
  ScopedLocalRef<jobject> peer(env, env->AllocObject(c.get()));
  peer_ = DecodeJObject(peer.get());
  if (peer_ == NULL) {
    CHECK(IsExceptionPending());
    return;
  }
  jmethodID mid = env->GetMethodID(c.get(), "<init>", "(Ljava/lang/ThreadGroup;Ljava/lang/String;IZ)V");
  env->CallNonvirtualVoidMethod(peer.get(), c.get(), mid, java_thread_group.get(), thread_name.get(), thread_priority, thread_is_daemon);
  CHECK(!IsExceptionPending()) << " " << PrettyTypeOf(GetException());
  SetVmData(peer_, Thread::Current());

  SirtRef<String> peer_thread_name(GetThreadName());
  if (peer_thread_name.get() == NULL) {
    // The Thread constructor should have set the Thread.name to a
    // non-null value. However, because we can run without code
    // available (in the compiler, in tests), we manually assign the
    // fields the constructor should have set.
    gThread_daemon->SetBoolean(peer_, thread_is_daemon);
    gThread_group->SetObject(peer_, thread_group);
    gThread_name->SetObject(peer_, Decode<Object*>(env, thread_name.get()));
    gThread_priority->SetInt(peer_, thread_priority);
    peer_thread_name.reset(GetThreadName());
  }
  // thread_name may have been null, so don't trust this to be non-null
  if (peer_thread_name.get() != NULL) {
    SetThreadName(peer_thread_name->ToModifiedUtf8().c_str());
  }

  // Pre-allocate an OutOfMemoryError for the double-OOME case.
  ThrowNewException("Ljava/lang/OutOfMemoryError;",
      "OutOfMemoryError thrown while trying to throw OutOfMemoryError; no stack available");
  ScopedLocalRef<jthrowable> exception(env, env->ExceptionOccurred());
  env->ExceptionClear();
  pre_allocated_OutOfMemoryError_ = Decode<Throwable*>(env, exception.get());
}

void Thread::SetThreadName(const char* name) {
  name_->assign(name);
  ::art::SetThreadName(name);
  Dbg::DdmSendThreadNotification(this, CHUNK_TYPE("THNM"));
}

void Thread::InitStackHwm() {
  void* stack_base;
  size_t stack_size;
  GetThreadStack(stack_base, stack_size);

  // TODO: include this in the thread dumps; potentially useful in SIGQUIT output?
  VLOG(threads) << StringPrintf("Native stack is at %p (%s)", stack_base, PrettySize(stack_size).c_str());

  stack_begin_ = reinterpret_cast<byte*>(stack_base);
  stack_size_ = stack_size;

  if (stack_size_ <= kStackOverflowReservedBytes) {
    LOG(FATAL) << "Attempt to attach a thread with a too-small stack (" << stack_size_ << " bytes)";
  }

  // TODO: move this into the Linux GetThreadStack implementation.
#if !defined(__APPLE__)
  // If we're the main thread, check whether we were run with an unlimited stack. In that case,
  // glibc will have reported a 2GB stack for our 32-bit process, and our stack overflow detection
  // will be broken because we'll die long before we get close to 2GB.
  if (thin_lock_id_ == 1) {
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
      size_t old_stack_size = stack_size_;
      stack_size_ = default_stack_size;
      stack_begin_ += (old_stack_size - stack_size_);
      VLOG(threads) << "Limiting unlimited stack (reported as " << PrettySize(old_stack_size) << ")"
                    << " to " << PrettySize(stack_size_)
                    << " with base " << reinterpret_cast<void*>(stack_begin_);
    }
  }
#endif

  // Set stack_end_ to the bottom of the stack saving space of stack overflows
  ResetDefaultStackEnd();

  // Sanity check.
  int stack_variable;
  CHECK_GT(&stack_variable, reinterpret_cast<void*>(stack_end_));
}

void Thread::Dump(std::ostream& os, bool full) const {
  if (full) {
    DumpState(os);
    DumpStack(os);
  } else {
    os << "Thread[";
    if (GetThinLockId() != 0) {
      // If we're in kStarting, we won't have a thin lock id or tid yet.
      os << GetThinLockId()
         << ",tid=" << GetTid() << ',';
    }
    os << GetState()
       << ",Thread*=" << this
       << ",peer=" << peer_
       << ",\"" << *name_ << "\""
       << "]";
  }
}

String* Thread::GetThreadName() const {
  return (peer_ != NULL) ? reinterpret_cast<String*>(gThread_name->GetObject(peer_)) : NULL;
}

void Thread::GetThreadName(std::string& name) const {
  name.assign(*name_);
}

void Thread::DumpState(std::ostream& os) const {
  std::string group_name;
  int priority;
  bool is_daemon = false;

  if (peer_ != NULL) {
    priority = gThread_priority->GetInt(peer_);
    is_daemon = gThread_daemon->GetBoolean(peer_);

    Object* thread_group = GetThreadGroup();
    if (thread_group != NULL) {
      String* group_name_string = reinterpret_cast<String*>(gThreadGroup_name->GetObject(thread_group));
      group_name = (group_name_string != NULL) ? group_name_string->ToModifiedUtf8() : "<null>";
    }
  } else {
    priority = GetNativePriority();
  }

  int policy;
  sched_param sp;
  CHECK_PTHREAD_CALL(pthread_getschedparam, (pthread_self(), &policy, &sp), __FUNCTION__);

  std::string scheduler_group_name(GetSchedulerGroupName(GetTid()));
  if (scheduler_group_name.empty()) {
    scheduler_group_name = "default";
  }

  os << '"' << *name_ << '"';
  if (is_daemon) {
    os << " daemon";
  }
  os << " prio=" << priority
     << " tid=" << GetThinLockId()
     << " " << GetState() << "\n";

  os << "  | group=\"" << group_name << "\""
     << " sCount=" << suspend_count_
     << " dsCount=" << debug_suspend_count_
     << " obj=" << reinterpret_cast<void*>(peer_)
     << " self=" << reinterpret_cast<const void*>(this) << "\n";
  os << "  | sysTid=" << GetTid()
     << " nice=" << getpriority(PRIO_PROCESS, GetTid())
     << " sched=" << policy << "/" << sp.sched_priority
     << " cgrp=" << scheduler_group_name
     << " handle=" << pthread_self() << "\n";

  // Grab the scheduler stats for this thread.
  std::string scheduler_stats;
  if (ReadFileToString(StringPrintf("/proc/self/task/%d/schedstat", GetTid()), &scheduler_stats)) {
    scheduler_stats.resize(scheduler_stats.size() - 1); // Lose the trailing '\n'.
  } else {
    scheduler_stats = "0 0 0";
  }

  int utime = 0;
  int stime = 0;
  int task_cpu = 0;
  GetTaskStats(GetTid(), utime, stime, task_cpu);

  os << "  | schedstat=( " << scheduler_stats << " )"
     << " utm=" << utime
     << " stm=" << stime
     << " core=" << task_cpu
     << " HZ=" << sysconf(_SC_CLK_TCK) << "\n";

  os << "  | stackSize=" << PrettySize(stack_size_)
     << " stack=" << reinterpret_cast<void*>(stack_begin_) << "-" << reinterpret_cast<void*>(stack_end_) << "\n";
}

#if !defined(ART_USE_LLVM_COMPILER)
void Thread::PushNativeToManagedRecord(NativeToManagedRecord* record) {
  Method **sp = top_of_managed_stack_.GetSP();
#ifndef NDEBUG
  if (sp != NULL) {
    Method* m = *sp;
    Runtime::Current()->GetHeap()->VerifyObject(m);
    DCHECK((m == NULL) || m->IsMethod());
  }
#endif
  record->last_top_of_managed_stack_ = reinterpret_cast<void*>(sp);
  record->last_top_of_managed_stack_pc_ = top_of_managed_stack_pc_;
  record->link_ = native_to_managed_record_;
  native_to_managed_record_ = record;
  top_of_managed_stack_.SetSP(NULL);
}
#else
void Thread::PushNativeToManagedRecord(NativeToManagedRecord*) {
  LOG(FATAL) << "Called non-LLVM method with LLVM";
}
#endif

#if !defined(ART_USE_LLVM_COMPILER)
void Thread::PopNativeToManagedRecord(const NativeToManagedRecord& record) {
  native_to_managed_record_ = record.link_;
  top_of_managed_stack_.SetSP(reinterpret_cast<Method**>(record.last_top_of_managed_stack_));
  top_of_managed_stack_pc_ = record.last_top_of_managed_stack_pc_;
}
#else
void Thread::PopNativeToManagedRecord(const NativeToManagedRecord&) {
  LOG(FATAL) << "Called non-LLVM method with LLVM";
}
#endif

struct StackDumpVisitor : public Thread::StackVisitor {
  StackDumpVisitor(std::ostream& os, const Thread* thread)
      : last_method(NULL), last_line_number(0), repetition_count(0), os(os), thread(thread),
        frame_count(0) {
  }

  virtual ~StackDumpVisitor() {
    if (frame_count == 0) {
      os << "  (no managed stack frames)\n";
    }
  }

  bool VisitFrame(const Frame& frame, uintptr_t pc) {
    if (!frame.HasMethod()) {
      return true;
    }
    const int kMaxRepetition = 3;
    Method* m = frame.GetMethod();
    Class* c = m->GetDeclaringClass();
    ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
    const DexCache* dex_cache = c->GetDexCache();
    int line_number = -1;
    if (dex_cache != NULL) {  // be tolerant of bad input
      const DexFile& dex_file = class_linker->FindDexFile(dex_cache);
      line_number = dex_file.GetLineNumFromPC(m, m->ToDexPC(pc));
    }
    if (line_number == last_line_number && last_method == m) {
      repetition_count++;
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
        os << "(" << (source_file != NULL ? source_file : "unavailable")
           << ":" << line_number << ")";
      }
      os << "\n";
    }

    if (frame_count++ == 0) {
      Monitor::DescribeWait(os, thread);
    }
    return true;
  }
  MethodHelper mh;
  Method* last_method;
  int last_line_number;
  int repetition_count;
  std::ostream& os;
  const Thread* thread;
  int frame_count;
};

void Thread::DumpStack(std::ostream& os) const {
  // If we're currently in native code, dump that stack before dumping the managed stack.
  if (GetState() == kNative || GetState() == kVmWait) {
    DumpKernelStack(os);
    DumpNativeStack(os);
  }
  StackDumpVisitor dumper(os, this);
  WalkStack(&dumper);
}

#if !defined(__APPLE__)
void Thread::DumpKernelStack(std::ostream& os) const {
  std::string kernel_stack_filename(StringPrintf("/proc/self/task/%d/stack", GetTid()));
  std::string kernel_stack;
  if (!ReadFileToString(kernel_stack_filename, &kernel_stack)) {
    os << "  (couldn't read " << kernel_stack_filename << ")";
  }

  std::vector<std::string> kernel_stack_frames;
  Split(kernel_stack, '\n', kernel_stack_frames);
  // We skip the last stack frame because it's always equivalent to "[<ffffffff>] 0xffffffff",
  // which looking at the source appears to be the kernel's way of saying "that's all, folks!".
  kernel_stack_frames.pop_back();
  for (size_t i = 0; i < kernel_stack_frames.size(); ++i) {
    os << "  kernel: " << kernel_stack_frames[i] << "\n";
  }
}
#else
// TODO: can we get the kernel stack on Mac OS?
void Thread::DumpKernelStack(std::ostream&) const {}
#endif

void Thread::SetStateWithoutSuspendCheck(ThreadState new_state) {
  volatile void* raw = reinterpret_cast<volatile void*>(&state_);
  volatile int32_t* addr = reinterpret_cast<volatile int32_t*>(raw);
  android_atomic_release_store(new_state, addr);
}

ThreadState Thread::SetState(ThreadState new_state) {
  ThreadState old_state = state_;
  if (old_state == new_state) {
    return old_state;
  }

  volatile void* raw = reinterpret_cast<volatile void*>(&state_);
  volatile int32_t* addr = reinterpret_cast<volatile int32_t*>(raw);

  if (new_state == kRunnable) {
    /*
     * Change our status to kRunnable.  The transition requires
     * that we check for pending suspension, because the runtime considers
     * us to be "asleep" in all other states, and another thread could
     * be performing a GC now.
     *
     * The order of operations is very significant here.  One way to
     * do this wrong is:
     *
     *   GCing thread                   Our thread (in kNative)
     *   ------------                   ----------------------
     *                                  check suspend count (== 0)
     *   SuspendAllThreads()
     *   grab suspend-count lock
     *   increment all suspend counts
     *   release suspend-count lock
     *   check thread state (== kNative)
     *   all are suspended, begin GC
     *                                  set state to kRunnable
     *                                  (continue executing)
     *
     * We can correct this by grabbing the suspend-count lock and
     * performing both of our operations (check suspend count, set
     * state) while holding it, now we need to grab a mutex on every
     * transition to kRunnable.
     *
     * What we do instead is change the order of operations so that
     * the transition to kRunnable happens first.  If we then detect
     * that the suspend count is nonzero, we switch to kSuspended.
     *
     * Appropriate compiler and memory barriers are required to ensure
     * that the operations are observed in the expected order.
     *
     * This does create a small window of opportunity where a GC in
     * progress could observe what appears to be a running thread (if
     * it happens to look between when we set to kRunnable and when we
     * switch to kSuspended).  At worst this only affects assertions
     * and thread logging.  (We could work around it with some sort
     * of intermediate "pre-running" state that is generally treated
     * as equivalent to running, but that doesn't seem worthwhile.)
     *
     * We can also solve this by combining the "status" and "suspend
     * count" fields into a single 32-bit value.  This trades the
     * store/load barrier on transition to kRunnable for an atomic RMW
     * op on all transitions and all suspend count updates (also, all
     * accesses to status or the thread count require bit-fiddling).
     * It also eliminates the brief transition through kRunnable when
     * the thread is supposed to be suspended.  This is possibly faster
     * on SMP and slightly more correct, but less convenient.
     */
    android_atomic_acquire_store(new_state, addr);
    ANNOTATE_IGNORE_READS_BEGIN();
    int suspend_count = suspend_count_;
    ANNOTATE_IGNORE_READS_END();
    if (suspend_count != 0) {
      Runtime::Current()->GetThreadList()->FullSuspendCheck(this);
    }
  } else {
    /*
     * Not changing to kRunnable. No additional work required.
     *
     * We use a releasing store to ensure that, if we were runnable,
     * any updates we previously made to objects on the managed heap
     * will be observed before the state change.
     */
    android_atomic_release_store(new_state, addr);
  }

  return old_state;
}

bool Thread::IsSuspended() {
  ANNOTATE_IGNORE_READS_BEGIN();
  int suspend_count = suspend_count_;
  ANNOTATE_IGNORE_READS_END();
  return suspend_count != 0 && GetState() != kRunnable;
}

static void ReportThreadSuspendTimeout(Thread* waiting_thread) {
  Runtime* runtime = Runtime::Current();
  std::ostringstream ss;
  ss << "Thread suspend timeout waiting for thread " << *waiting_thread << "\n";
  runtime->DumpLockHolders(ss);
  ss << "\n";
  runtime->GetThreadList()->DumpLocked(ss);
  LOG(FATAL) << ss.str();
}

void Thread::WaitUntilSuspended() {
  static const useconds_t kTimeoutUs = 30 * 1000000; // 30s.

  useconds_t total_delay = 0;
  useconds_t delay = 0;
  while (GetState() == kRunnable) {
    if (total_delay >= kTimeoutUs) {
      ReportThreadSuspendTimeout(this);
    }
    useconds_t new_delay = delay * 2;
    CHECK_GE(new_delay, delay);
    delay = new_delay;
    if (delay == 0) {
      sched_yield();
      delay = 10000;
    } else {
      usleep(delay);
      total_delay += delay;
    }
  }
}

void Thread::ThreadExitCallback(void* arg) {
  Thread* self = reinterpret_cast<Thread*>(arg);
  LOG(FATAL) << "Native thread exited without calling DetachCurrentThread: " << *self;
}

void Thread::Startup() {
  // Allocate a TLS slot.
  CHECK_PTHREAD_CALL(pthread_key_create, (&Thread::pthread_key_self_, Thread::ThreadExitCallback), "self key");

  // Double-check the TLS slot allocation.
  if (pthread_getspecific(pthread_key_self_) != NULL) {
    LOG(FATAL) << "Newly-created pthread TLS slot is not NULL";
  }
}

// TODO: make more accessible?
static Class* FindClassOrDie(ClassLinker* class_linker, const char* descriptor) {
  Class* c = class_linker->FindSystemClass(descriptor);
  CHECK(c != NULL) << descriptor;
  return c;
}

// TODO: make more accessible?
static Field* FindFieldOrDie(Class* c, const char* name, const char* descriptor) {
  Field* f = c->FindDeclaredInstanceField(name, descriptor);
  CHECK(f != NULL) << PrettyClass(c) << " " << name << " " << descriptor;
  return f;
}

// TODO: make more accessible?
static Method* FindMethodOrDie(Class* c, const char* name, const char* signature) {
  Method* m = c->FindVirtualMethod(name, signature);
  CHECK(m != NULL) << PrettyClass(c) << " " << name << " " << signature;
  return m;
}

// TODO: make more accessible?
static Field* FindStaticFieldOrDie(Class* c, const char* name, const char* descriptor) {
  Field* f = c->FindDeclaredStaticField(name, descriptor);
  CHECK(f != NULL) << PrettyClass(c) << " " << name << " " << descriptor;
  return f;
}

void Thread::FinishStartup() {
  CHECK(Runtime::Current()->IsStarted());
  Thread* self = Thread::Current();

  // Need to be kRunnable for FindClass
  ScopedThreadStateChange tsc(self, kRunnable);

  // Now the ClassLinker is ready, we can find the various Class*, Field*, and Method*s we need.
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();

  Class* Thread_class = FindClassOrDie(class_linker, "Ljava/lang/Thread;");
  Class* UncaughtExceptionHandler_class = FindClassOrDie(class_linker, "Ljava/lang/Thread$UncaughtExceptionHandler;");
  gThreadGroup = FindClassOrDie(class_linker, "Ljava/lang/ThreadGroup;");
  gThreadLock = FindClassOrDie(class_linker, "Ljava/lang/ThreadLock;");

  gThread_daemon = FindFieldOrDie(Thread_class, "daemon", "Z");
  gThread_group = FindFieldOrDie(Thread_class, "group", "Ljava/lang/ThreadGroup;");
  gThread_lock = FindFieldOrDie(Thread_class, "lock", "Ljava/lang/ThreadLock;");
  gThread_name = FindFieldOrDie(Thread_class, "name", "Ljava/lang/String;");
  gThread_priority = FindFieldOrDie(Thread_class, "priority", "I");
  gThread_uncaughtHandler = FindFieldOrDie(Thread_class, "uncaughtHandler", "Ljava/lang/Thread$UncaughtExceptionHandler;");
  gThread_vmData = FindFieldOrDie(Thread_class, "vmData", "I");
  gThreadGroup_name = FindFieldOrDie(gThreadGroup, "name", "Ljava/lang/String;");
  gThreadGroup_mMain = FindStaticFieldOrDie(gThreadGroup, "mMain", "Ljava/lang/ThreadGroup;");
  gThreadGroup_mSystem = FindStaticFieldOrDie(gThreadGroup, "mSystem", "Ljava/lang/ThreadGroup;");
  gThreadLock_thread = FindFieldOrDie(gThreadLock, "thread", "Ljava/lang/Thread;");

  gThread_run = FindMethodOrDie(Thread_class, "run", "()V");
  gThreadGroup_removeThread = FindMethodOrDie(gThreadGroup, "removeThread", "(Ljava/lang/Thread;)V");
  gUncaughtExceptionHandler_uncaughtException = FindMethodOrDie(UncaughtExceptionHandler_class,
      "uncaughtException", "(Ljava/lang/Thread;Ljava/lang/Throwable;)V");

  // Finish attaching the main thread.
  Thread::Current()->CreatePeer("main", false, Thread::GetMainThreadGroup());

  InitBoxingMethods();
  class_linker->RunRootClinits();
}

void Thread::Shutdown() {
  CHECK_PTHREAD_CALL(pthread_key_delete, (Thread::pthread_key_self_), "self key");
}

uint32_t Thread::LockOwnerFromThreadLock(Object* thread_lock) {
  if (thread_lock == NULL || thread_lock->GetClass() != gThreadLock) {
    return ThreadList::kInvalidId;
  }
  Object* managed_thread = gThreadLock_thread->GetObject(thread_lock);
  if (managed_thread == NULL) {
    return ThreadList::kInvalidId;
  }
  uintptr_t vmData = static_cast<uintptr_t>(gThread_vmData->GetInt(managed_thread));
  Thread* thread = reinterpret_cast<Thread*>(vmData);
  if (thread == NULL) {
    return ThreadList::kInvalidId;
  }
  return thread->GetThinLockId();
}

Thread::Thread()
    : thin_lock_id_(0),
      tid_(0),
      peer_(NULL),
      top_of_managed_stack_(),
      top_of_managed_stack_pc_(0),
      wait_mutex_(new Mutex("a thread wait mutex")),
      wait_cond_(new ConditionVariable("a thread wait condition variable")),
      wait_monitor_(NULL),
      interrupted_(false),
      wait_next_(NULL),
      monitor_enter_object_(NULL),
      card_table_(0),
      stack_end_(NULL),
      native_to_managed_record_(NULL),
      top_sirt_(NULL),
      top_shadow_frame_(NULL),
      jni_env_(NULL),
      state_(kNative),
      self_(NULL),
      runtime_(NULL),
      exception_(NULL),
      suspend_count_(0),
      debug_suspend_count_(0),
      class_loader_override_(NULL),
      long_jump_context_(NULL),
      throwing_OutOfMemoryError_(false),
      pre_allocated_OutOfMemoryError_(NULL),
      debug_invoke_req_(new DebugInvokeReq),
      trace_stack_(new std::vector<TraceStackFrame>),
      name_(new std::string(kThreadNameDuringStartup)) {
  CHECK_EQ((sizeof(Thread) % 4), 0U) << sizeof(Thread);
  memset(&held_mutexes_[0], 0, sizeof(held_mutexes_));
}

bool Thread::IsStillStarting() const {
  // You might think you can check whether the state is kStarting, but for much of thread startup,
  // the thread might also be in kVmWait.
  // You might think you can check whether the peer is NULL, but the peer is actually created and
  // assigned fairly early on, and needs to be.
  // It turns out that the last thing to change is the thread name; that's a good proxy for "has
  // this thread _ever_ entered kRunnable".
  return (*name_ == kThreadNameDuringStartup);
}

static void MonitorExitVisitor(const Object* object, void*) {
  Object* entered_monitor = const_cast<Object*>(object);
  LOG(WARNING) << "Calling MonitorExit on object " << object << " (" << PrettyTypeOf(object) << ")"
               << " left locked by native thread " << *Thread::Current() << " which is detaching";
  entered_monitor->MonitorExit(Thread::Current());
}

void Thread::Destroy() {
  // On thread detach, all monitors entered with JNI MonitorEnter are automatically exited.
  if (jni_env_ != NULL) {
    jni_env_->monitors.VisitRoots(MonitorExitVisitor, NULL);
  }

  if (peer_ != NULL) {
    Thread* self = this;

    // We may need to call user-supplied managed code.
    SetState(kRunnable);

    HandleUncaughtExceptions();
    RemoveFromThreadGroup();

    // this.vmData = 0;
    SetVmData(peer_, NULL);

    Dbg::PostThreadDeath(self);

    // Thread.join() is implemented as an Object.wait() on the Thread.lock
    // object. Signal anyone who is waiting.
    Object* lock = gThread_lock->GetObject(peer_);
    // (This conditional is only needed for tests, where Thread.lock won't have been set.)
    if (lock != NULL) {
      lock->MonitorEnter(self);
      lock->NotifyAll();
      lock->MonitorExit(self);
    }
  }
}

Thread::~Thread() {
  delete jni_env_;
  jni_env_ = NULL;

  SetState(kTerminated);

  delete wait_cond_;
  delete wait_mutex_;

#if !defined(ART_USE_LLVM_COMPILER)
  delete long_jump_context_;
#endif

  delete debug_invoke_req_;
  delete trace_stack_;
  delete name_;

  TearDownAlternateSignalStack();
}

void Thread::HandleUncaughtExceptions() {
  if (!IsExceptionPending()) {
    return;
  }

  // Get and clear the exception.
  Object* exception = GetException();
  ClearException();

  // If the thread has its own handler, use that.
  Object* handler = gThread_uncaughtHandler->GetObject(peer_);
  if (handler == NULL) {
    // Otherwise use the thread group's default handler.
    handler = GetThreadGroup();
  }

  // Call the handler.
  Method* m = handler->GetClass()->FindVirtualMethodForVirtualOrInterface(gUncaughtExceptionHandler_uncaughtException);
  JValue args[2];
  args[0].SetL(peer_);
  args[1].SetL(exception);
  m->Invoke(this, handler, args, NULL);

  // If the handler threw, clear that exception too.
  ClearException();
}

Object* Thread::GetThreadGroup() const {
  return gThread_group->GetObject(peer_);
}

void Thread::RemoveFromThreadGroup() {
  // this.group.removeThread(this);
  // group can be null if we're in the compiler or a test.
  Object* group = GetThreadGroup();
  if (group != NULL) {
    Method* m = group->GetClass()->FindVirtualMethodForVirtualOrInterface(gThreadGroup_removeThread);
    JValue args[1];
    args[0].SetL(peer_);
    m->Invoke(this, group, args, NULL);
  }
}

size_t Thread::NumSirtReferences() {
  size_t count = 0;
  for (StackIndirectReferenceTable* cur = top_sirt_; cur; cur = cur->GetLink()) {
    count += cur->NumberOfReferences();
  }
  return count;
}

size_t Thread::NumShadowFrameReferences() {
  size_t count = 0;
  for (ShadowFrame* cur = top_shadow_frame_; cur; cur = cur->GetLink()) {
    count += cur->NumberOfReferences();
  }
  return count;
}

bool Thread::SirtContains(jobject obj) {
  Object** sirt_entry = reinterpret_cast<Object**>(obj);
  for (StackIndirectReferenceTable* cur = top_sirt_; cur; cur = cur->GetLink()) {
    if (cur->Contains(sirt_entry)) {
      return true;
    }
  }
  return false;
}

bool Thread::ShadowFrameContains(jobject obj) {
  Object** shadow_frame_entry = reinterpret_cast<Object**>(obj);
  for (ShadowFrame* cur = top_shadow_frame_; cur; cur = cur->GetLink()) {
    if (cur->Contains(shadow_frame_entry)) {
      return true;
    }
  }
  return false;
}

bool Thread::StackReferencesContain(jobject obj) {
  return SirtContains(obj) || ShadowFrameContains(obj);
}

void Thread::SirtVisitRoots(Heap::RootVisitor* visitor, void* arg) {
  for (StackIndirectReferenceTable* cur = top_sirt_; cur; cur = cur->GetLink()) {
    size_t num_refs = cur->NumberOfReferences();
    for (size_t j = 0; j < num_refs; j++) {
      Object* object = cur->GetReference(j);
      if (object != NULL) {
        visitor(object, arg);
      }
    }
  }
}

void Thread::ShadowFrameVisitRoots(Heap::RootVisitor* visitor, void* arg) {
  for (ShadowFrame* cur = top_shadow_frame_; cur; cur = cur->GetLink()) {
    size_t num_refs = cur->NumberOfReferences();
    for (size_t j = 0; j < num_refs; j++) {
      Object* object = cur->GetReference(j);
      if (object != NULL) {
        visitor(object, arg);
      }
    }
  }
}

Object* Thread::DecodeJObject(jobject obj) {
  DCHECK(CanAccessDirectReferences());
  if (obj == NULL) {
    return NULL;
  }
  IndirectRef ref = reinterpret_cast<IndirectRef>(obj);
  IndirectRefKind kind = GetIndirectRefKind(ref);
  Object* result;
  switch (kind) {
  case kLocal:
    {
      IndirectReferenceTable& locals = jni_env_->locals;
      result = const_cast<Object*>(locals.Get(ref));
      break;
    }
  case kGlobal:
    {
      JavaVMExt* vm = Runtime::Current()->GetJavaVM();
      IndirectReferenceTable& globals = vm->globals;
      MutexLock mu(vm->globals_lock);
      result = const_cast<Object*>(globals.Get(ref));
      break;
    }
  case kWeakGlobal:
    {
      JavaVMExt* vm = Runtime::Current()->GetJavaVM();
      IndirectReferenceTable& weak_globals = vm->weak_globals;
      MutexLock mu(vm->weak_globals_lock);
      result = const_cast<Object*>(weak_globals.Get(ref));
      if (result == kClearedJniWeakGlobal) {
        // This is a special case where it's okay to return NULL.
        return NULL;
      }
      break;
    }
  case kSirtOrInvalid:
  default:
    // TODO: make stack indirect reference table lookup more efficient
    // Check if this is a local reference in the SIRT
    if (StackReferencesContain(obj)) {
      result = *reinterpret_cast<Object**>(obj);  // Read from SIRT
    } else if (Runtime::Current()->GetJavaVM()->work_around_app_jni_bugs) {
      // Assume an invalid local reference is actually a direct pointer.
      result = reinterpret_cast<Object*>(obj);
    } else {
      result = kInvalidIndirectRefObject;
    }
  }

  if (result == NULL) {
    LOG(ERROR) << "JNI ERROR (app bug): use of deleted " << kind << ": " << obj;
    JniAbort(NULL);
  } else {
    if (result != kInvalidIndirectRefObject) {
      Runtime::Current()->GetHeap()->VerifyObject(result);
    }
  }
  return result;
}

class CountStackDepthVisitor : public Thread::StackVisitor {
 public:
  CountStackDepthVisitor() : depth_(0), skip_depth_(0), skipping_(true) {}

  bool VisitFrame(const Frame& frame, uintptr_t /*pc*/) {
    // We want to skip frames up to and including the exception's constructor.
    // Note we also skip the frame if it doesn't have a method (namely the callee
    // save frame)
    if (skipping_ && frame.HasMethod() &&
        !Throwable::GetJavaLangThrowable()->IsAssignableFrom(frame.GetMethod()->GetDeclaringClass())) {
      skipping_ = false;
    }
    if (!skipping_) {
      if (frame.HasMethod()) {  // ignore callee save frames
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

class BuildInternalStackTraceVisitor : public Thread::StackVisitor {
 public:
  explicit BuildInternalStackTraceVisitor(int skip_depth)
      : skip_depth_(skip_depth), count_(0), pc_trace_(NULL), method_trace_(NULL), local_ref_(NULL) {
  }

  bool Init(int depth, ScopedJniThreadState& ts) {
    // Allocate method trace with an extra slot that will hold the PC trace
    method_trace_ = Runtime::Current()->GetClassLinker()->AllocObjectArray<Object>(depth + 1);
    if (method_trace_ == NULL) {
      return false;
    }
    // Register a local reference as IntArray::Alloc may trigger GC
    local_ref_ = AddLocalReference<jobject>(ts.Env(), method_trace_);
    pc_trace_ = IntArray::Alloc(depth);
    if (pc_trace_ == NULL) {
      return false;
    }
#ifdef MOVING_GARBAGE_COLLECTOR
    // Re-read after potential GC
    method_trace_ = Decode<ObjectArray<Object>*>(ts.Env(), local_ref_);
#endif
    // Save PC trace in last element of method trace, also places it into the
    // object graph.
    method_trace_->Set(depth, pc_trace_);
    return true;
  }

  virtual ~BuildInternalStackTraceVisitor() {}

  bool VisitFrame(const Frame& frame, uintptr_t pc) {
    if (method_trace_ == NULL || pc_trace_ == NULL) {
      return true; // We're probably trying to fillInStackTrace for an OutOfMemoryError.
    }
    if (skip_depth_ > 0) {
      skip_depth_--;
      return true;
    }
    if (!frame.HasMethod()) {
      return true;  // ignore callee save frames
    }
    method_trace_->Set(count_, frame.GetMethod());
    pc_trace_->Set(count_, pc);
    ++count_;
    return true;
  }

  jobject GetInternalStackTrace() const {
    return local_ref_;
  }

 private:
  // How many more frames to skip.
  int32_t skip_depth_;
  // Current position down stack trace
  uint32_t count_;
  // Array of return PC values
  IntArray* pc_trace_;
  // An array of the methods on the stack, the last entry is a reference to the
  // PC trace
  ObjectArray<Object>* method_trace_;
  // Local indirect reference table entry for method trace
  jobject local_ref_;
};

#if !defined(ART_USE_LLVM_COMPILER)
// TODO: remove this.
static uintptr_t ManglePc(uintptr_t pc) {
  // Move the PC back 2 bytes as a call will frequently terminate the
  // decoding of a particular instruction and we want to make sure we
  // get the Dex PC of the instruction with the call and not the
  // instruction following.
  if (pc > 0) { pc -= 2; }
  return pc;
}
#endif

// TODO: remove this.
static uintptr_t DemanglePc(uintptr_t pc) {
  // Revert mangling for the case where we need the PC to return to the upcall
  if (pc > 0) { pc +=  2; }
  return pc;
}

void Thread::PushShadowFrame(ShadowFrame* frame) {
  frame->SetLink(top_shadow_frame_);
  top_shadow_frame_ = frame;
}

ShadowFrame* Thread::PopShadowFrame() {
  CHECK(top_shadow_frame_ != NULL);
  ShadowFrame* frame = top_shadow_frame_;
  top_shadow_frame_ = frame->GetLink();
  return frame;
}

void Thread::PushSirt(StackIndirectReferenceTable* sirt) {
  sirt->SetLink(top_sirt_);
  top_sirt_ = sirt;
}

StackIndirectReferenceTable* Thread::PopSirt() {
  CHECK(top_sirt_ != NULL);
  StackIndirectReferenceTable* sirt = top_sirt_;
  top_sirt_ = top_sirt_->GetLink();
  return sirt;
}

#if !defined(ART_USE_LLVM_COMPILER) // LLVM use ShadowFrame

void Thread::WalkStack(StackVisitor* visitor, bool include_upcalls) const {
  Frame frame = GetTopOfStack();
  uintptr_t pc = ManglePc(top_of_managed_stack_pc_);
  uint32_t trace_stack_depth = 0;
  // TODO: enable this CHECK after native_to_managed_record_ is initialized during startup.
  // CHECK(native_to_managed_record_ != NULL);
  NativeToManagedRecord* record = native_to_managed_record_;
  bool method_tracing_active = Runtime::Current()->IsMethodTracingActive();
  while (frame.GetSP() != NULL) {
    for ( ; frame.GetMethod() != NULL; frame.Next()) {
      frame.GetMethod()->AssertPcIsWithinCode(pc);
      bool should_continue = visitor->VisitFrame(frame, pc);
      if (UNLIKELY(!should_continue)) {
        return;
      }
      uintptr_t return_pc = frame.GetReturnPC();
      if (LIKELY(!method_tracing_active)) {
        pc = ManglePc(return_pc);
      } else {
        // While profiling, the return pc is restored from the side stack, except when walking
        // the stack for an exception where the side stack will be unwound in VisitFrame.
        if (IsTraceExitPc(return_pc) && !include_upcalls) {
          TraceStackFrame trace_frame = GetTraceStackFrame(trace_stack_depth++);
          CHECK(trace_frame.method_ == frame.GetMethod());
          pc = ManglePc(trace_frame.return_pc_);
        } else {
          pc = ManglePc(return_pc);
        }
      }
    }
    if (include_upcalls) {
      bool should_continue = visitor->VisitFrame(frame, pc);
      if (!should_continue) {
        return;
      }
    }
    if (record == NULL) {
      return;
    }
    // last_tos should return Frame instead of sp?
    frame.SetSP(reinterpret_cast<Method**>(record->last_top_of_managed_stack_));
    pc = ManglePc(record->last_top_of_managed_stack_pc_);
    record = record->link_;
  }
}

#else // defined(ART_USE_LLVM_COMPILER) // LLVM uses ShadowFrame

void Thread::WalkStack(StackVisitor* visitor, bool /*include_upcalls*/) const {
  for (ShadowFrame* cur = top_shadow_frame_; cur; cur = cur->GetLink()) {
    Frame frame;
    frame.SetSP(reinterpret_cast<Method**>(reinterpret_cast<byte*>(cur) +
                                           ShadowFrame::MethodOffset()));
    bool should_continue = visitor->VisitFrame(frame, cur->GetDexPC());
    if (!should_continue) {
      return;
    }
  }
}

/*
 *                                |                        |
 *                                |                        |
 *                                |                        |
 *                                |      .                 |
 *                                |      .                 |
 *                                |      .                 |
 *                                |      .                 |
 *                                | Method*                |
 *                                |      .                 |
 *                                |      .                 | <-- top_shadow_frame_   (ShadowFrame*)
 *                              / +------------------------+
 *                              ->|      .                 |
 *                              . |      .                 |
 *                              . |      .                 |
 *                               /+------------------------+
 *                              / |      .                 |
 *                             /  |      .                 |
 *     ---                     |  |      .                 |
 *      |                      |  |      .                 |
 *                             |  | Method*                | <-- frame.GetSP() (Method**)
 *  ShadowFrame                \  |      .                 |
 *      |                       ->|      .                 | <-- cur           (ShadowFrame*)
 *     ---                       /+------------------------+
 *                              / |      .                 |
 *                             /  |      .                 |
 *     ---                     |  |      .                 |
 *      |       cur->GetLink() |  |      .                 |
 *                             |  | Method*                |
 *   ShadowFrame               \  |      .                 |
 *      |                       ->|      .                 |
 *     ---                        +------------------------+
 *                                |      .                 |
 *                                |      .                 |
 *                                |      .                 |
 *                                +========================+
 */

#endif

jobject Thread::CreateInternalStackTrace(JNIEnv* env) const {
  // Compute depth of stack
  CountStackDepthVisitor count_visitor;
  WalkStack(&count_visitor);
  int32_t depth = count_visitor.GetDepth();
  int32_t skip_depth = count_visitor.GetSkipDepth();

  // Transition into runnable state to work on Object*/Array*
  ScopedJniThreadState ts(env);

  // Build internal stack trace
  BuildInternalStackTraceVisitor build_trace_visitor(skip_depth);
  if (!build_trace_visitor.Init(depth, ts)) {
    return NULL;  // Allocation failed
  }
  WalkStack(&build_trace_visitor);
  return build_trace_visitor.GetInternalStackTrace();
}

jobjectArray Thread::InternalStackTraceToStackTraceElementArray(JNIEnv* env, jobject internal,
    jobjectArray output_array, int* stack_depth) {
  // Transition into runnable state to work on Object*/Array*
  ScopedJniThreadState ts(env);
  // Decode the internal stack trace into the depth, method trace and PC trace
  ObjectArray<Object>* method_trace =
      down_cast<ObjectArray<Object>*>(Decode<Object*>(ts.Env(), internal));
  int32_t depth = method_trace->GetLength() - 1;
  IntArray* pc_trace = down_cast<IntArray*>(method_trace->Get(depth));

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();

  jobjectArray result;
  ObjectArray<StackTraceElement>* java_traces;
  if (output_array != NULL) {
    // Reuse the array we were given.
    result = output_array;
    java_traces = reinterpret_cast<ObjectArray<StackTraceElement>*>(Decode<Array*>(env,
        output_array));
    // ...adjusting the number of frames we'll write to not exceed the array length.
    depth = std::min(depth, java_traces->GetLength());
  } else {
    // Create java_trace array and place in local reference table
    java_traces = class_linker->AllocStackTraceElementArray(depth);
    if (java_traces == NULL) {
      return NULL;
    }
    result = AddLocalReference<jobjectArray>(ts.Env(), java_traces);
  }

  if (stack_depth != NULL) {
    *stack_depth = depth;
  }

  MethodHelper mh;
  for (int32_t i = 0; i < depth; ++i) {
    // Prepare parameters for StackTraceElement(String cls, String method, String file, int line)
    Method* method = down_cast<Method*>(method_trace->Get(i));
    mh.ChangeMethod(method);
    uint32_t native_pc = pc_trace->Get(i);
    int32_t line_number = mh.GetLineNumFromNativePC(native_pc);
    // Allocate element, potentially triggering GC
    // TODO: reuse class_name_object via Class::name_?
    const char* descriptor = mh.GetDeclaringClassDescriptor();
    CHECK(descriptor != NULL);
    std::string class_name(PrettyDescriptor(descriptor));
    SirtRef<String> class_name_object(String::AllocFromModifiedUtf8(class_name.c_str()));
    if (class_name_object.get() == NULL) {
      return NULL;
    }
    const char* method_name = mh.GetName();
    CHECK(method_name != NULL);
    SirtRef<String> method_name_object(String::AllocFromModifiedUtf8(method_name));
    if (method_name_object.get() == NULL) {
      return NULL;
    }
    const char* source_file = mh.GetDeclaringClassSourceFile();
    SirtRef<String> source_name_object(String::AllocFromModifiedUtf8(source_file));
    StackTraceElement* obj = StackTraceElement::Alloc(class_name_object.get(),
                                                      method_name_object.get(),
                                                      source_name_object.get(),
                                                      line_number);
    if (obj == NULL) {
      return NULL;
    }
#ifdef MOVING_GARBAGE_COLLECTOR
    // Re-read after potential GC
    java_traces = Decode<ObjectArray<Object>*>(ts.Env(), result);
    method_trace = down_cast<ObjectArray<Object>*>(Decode<Object*>(ts.Env(), internal));
    pc_trace = down_cast<IntArray*>(method_trace->Get(depth));
#endif
    java_traces->Set(i, obj);
  }
  return result;
}

void Thread::ThrowNewExceptionF(const char* exception_class_descriptor, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  ThrowNewExceptionV(exception_class_descriptor, fmt, args);
  va_end(args);
}

void Thread::ThrowNewExceptionV(const char* exception_class_descriptor, const char* fmt, va_list ap) {
  std::string msg;
  StringAppendV(&msg, fmt, ap);
  ThrowNewException(exception_class_descriptor, msg.c_str());
}

void Thread::ThrowNewException(const char* exception_class_descriptor, const char* msg) {
  // Convert "Ljava/lang/Exception;" into JNI-style "java/lang/Exception".
  CHECK_EQ('L', exception_class_descriptor[0]);
  std::string descriptor(exception_class_descriptor + 1);
  CHECK_EQ(';', descriptor[descriptor.length() - 1]);
  descriptor.erase(descriptor.length() - 1);

  JNIEnv* env = GetJniEnv();
  ScopedLocalRef<jclass> exception_class(env, env->FindClass(descriptor.c_str()));
  if (exception_class.get() == NULL) {
    LOG(ERROR) << "Couldn't throw new " << descriptor << " because JNI FindClass failed: "
               << PrettyTypeOf(GetException());
    CHECK(IsExceptionPending());
    return;
  }
  if (!Runtime::Current()->IsStarted()) {
    // Something is trying to throw an exception without a started
    // runtime, which is the common case in the compiler. We won't be
    // able to invoke the constructor of the exception, so use
    // AllocObject which will not invoke a constructor.
    ScopedLocalRef<jthrowable> exception(
        env, reinterpret_cast<jthrowable>(env->AllocObject(exception_class.get())));
    if (exception.get() != NULL) {
      ScopedJniThreadState ts(env);
      Throwable* t = reinterpret_cast<Throwable*>(ts.Self()->DecodeJObject(exception.get()));
      t->SetDetailMessage(String::AllocFromModifiedUtf8(msg));
      ts.Self()->SetException(t);
    } else {
      LOG(ERROR) << "Couldn't throw new " << descriptor << " because JNI AllocObject failed: "
                 << PrettyTypeOf(GetException());
      CHECK(IsExceptionPending());
    }
    return;
  }
  int rc = env->ThrowNew(exception_class.get(), msg);
  if (rc != JNI_OK) {
    LOG(ERROR) << "Couldn't throw new " << descriptor << " because JNI ThrowNew failed: "
               << PrettyTypeOf(GetException());
    CHECK(IsExceptionPending());
  }
}

void Thread::ThrowOutOfMemoryError(const char* msg) {
  LOG(ERROR) << StringPrintf("Throwing OutOfMemoryError \"%s\"%s",
      msg, (throwing_OutOfMemoryError_ ? " (recursive case)" : ""));
  if (!throwing_OutOfMemoryError_) {
    throwing_OutOfMemoryError_ = true;
    ThrowNewException("Ljava/lang/OutOfMemoryError;", NULL);
  } else {
    SetException(pre_allocated_OutOfMemoryError_);
  }
  throwing_OutOfMemoryError_ = false;
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

struct EntryPointInfo {
  uint32_t offset;
  const char* name;
};
#define ENTRY_POINT_INFO(x) { ENTRYPOINT_OFFSET(x), #x }
static const EntryPointInfo gThreadEntryPointInfo[] = {
  ENTRY_POINT_INFO(pAllocArrayFromCode),
  ENTRY_POINT_INFO(pAllocArrayFromCodeWithAccessCheck),
  ENTRY_POINT_INFO(pAllocObjectFromCode),
  ENTRY_POINT_INFO(pAllocObjectFromCodeWithAccessCheck),
  ENTRY_POINT_INFO(pCheckAndAllocArrayFromCode),
  ENTRY_POINT_INFO(pCheckAndAllocArrayFromCodeWithAccessCheck),
  ENTRY_POINT_INFO(pInstanceofNonTrivialFromCode),
  ENTRY_POINT_INFO(pCanPutArrayElementFromCode),
  ENTRY_POINT_INFO(pCheckCastFromCode),
  ENTRY_POINT_INFO(pDebugMe),
  ENTRY_POINT_INFO(pUpdateDebuggerFromCode),
  ENTRY_POINT_INFO(pInitializeStaticStorage),
  ENTRY_POINT_INFO(pInitializeTypeAndVerifyAccessFromCode),
  ENTRY_POINT_INFO(pInitializeTypeFromCode),
  ENTRY_POINT_INFO(pResolveStringFromCode),
  ENTRY_POINT_INFO(pSet32Instance),
  ENTRY_POINT_INFO(pSet32Static),
  ENTRY_POINT_INFO(pSet64Instance),
  ENTRY_POINT_INFO(pSet64Static),
  ENTRY_POINT_INFO(pSetObjInstance),
  ENTRY_POINT_INFO(pSetObjStatic),
  ENTRY_POINT_INFO(pGet32Instance),
  ENTRY_POINT_INFO(pGet32Static),
  ENTRY_POINT_INFO(pGet64Instance),
  ENTRY_POINT_INFO(pGet64Static),
  ENTRY_POINT_INFO(pGetObjInstance),
  ENTRY_POINT_INFO(pGetObjStatic),
  ENTRY_POINT_INFO(pHandleFillArrayDataFromCode),
  ENTRY_POINT_INFO(pDecodeJObjectInThread),
  ENTRY_POINT_INFO(pFindNativeMethod),
  ENTRY_POINT_INFO(pLockObjectFromCode),
  ENTRY_POINT_INFO(pUnlockObjectFromCode),
  ENTRY_POINT_INFO(pCmpgDouble),
  ENTRY_POINT_INFO(pCmpgFloat),
  ENTRY_POINT_INFO(pCmplDouble),
  ENTRY_POINT_INFO(pCmplFloat),
  ENTRY_POINT_INFO(pDadd),
  ENTRY_POINT_INFO(pDdiv),
  ENTRY_POINT_INFO(pDmul),
  ENTRY_POINT_INFO(pDsub),
  ENTRY_POINT_INFO(pF2d),
  ENTRY_POINT_INFO(pFmod),
  ENTRY_POINT_INFO(pI2d),
  ENTRY_POINT_INFO(pL2d),
  ENTRY_POINT_INFO(pD2f),
  ENTRY_POINT_INFO(pFadd),
  ENTRY_POINT_INFO(pFdiv),
  ENTRY_POINT_INFO(pFmodf),
  ENTRY_POINT_INFO(pFmul),
  ENTRY_POINT_INFO(pFsub),
  ENTRY_POINT_INFO(pI2f),
  ENTRY_POINT_INFO(pL2f),
  ENTRY_POINT_INFO(pD2iz),
  ENTRY_POINT_INFO(pF2iz),
  ENTRY_POINT_INFO(pIdivmod),
  ENTRY_POINT_INFO(pD2l),
  ENTRY_POINT_INFO(pF2l),
  ENTRY_POINT_INFO(pLdiv),
  ENTRY_POINT_INFO(pLdivmod),
  ENTRY_POINT_INFO(pLmul),
  ENTRY_POINT_INFO(pShlLong),
  ENTRY_POINT_INFO(pShrLong),
  ENTRY_POINT_INFO(pUshrLong),
  ENTRY_POINT_INFO(pIndexOf),
  ENTRY_POINT_INFO(pMemcmp16),
  ENTRY_POINT_INFO(pStringCompareTo),
  ENTRY_POINT_INFO(pMemcpy),
  ENTRY_POINT_INFO(pUnresolvedDirectMethodTrampolineFromCode),
  ENTRY_POINT_INFO(pInvokeDirectTrampolineWithAccessCheck),
  ENTRY_POINT_INFO(pInvokeInterfaceTrampoline),
  ENTRY_POINT_INFO(pInvokeInterfaceTrampolineWithAccessCheck),
  ENTRY_POINT_INFO(pInvokeStaticTrampolineWithAccessCheck),
  ENTRY_POINT_INFO(pInvokeSuperTrampolineWithAccessCheck),
  ENTRY_POINT_INFO(pInvokeVirtualTrampolineWithAccessCheck),
  ENTRY_POINT_INFO(pCheckSuspendFromCode),
  ENTRY_POINT_INFO(pTestSuspendFromCode),
  ENTRY_POINT_INFO(pDeliverException),
  ENTRY_POINT_INFO(pThrowAbstractMethodErrorFromCode),
  ENTRY_POINT_INFO(pThrowArrayBoundsFromCode),
  ENTRY_POINT_INFO(pThrowDivZeroFromCode),
  ENTRY_POINT_INFO(pThrowNoSuchMethodFromCode),
  ENTRY_POINT_INFO(pThrowNullPointerFromCode),
  ENTRY_POINT_INFO(pThrowStackOverflowFromCode),
  ENTRY_POINT_INFO(pThrowVerificationErrorFromCode),
};
#undef ENTRY_POINT_INFO

void Thread::DumpThreadOffset(std::ostream& os, uint32_t offset, size_t size_of_pointers) {
  CHECK_EQ(size_of_pointers, 4U); // TODO: support 64-bit targets.

#define DO_THREAD_OFFSET(x) if (offset == static_cast<uint32_t>(OFFSETOF_VOLATILE_MEMBER(Thread, x))) { os << # x; return; }
  DO_THREAD_OFFSET(card_table_);
  DO_THREAD_OFFSET(exception_);
  DO_THREAD_OFFSET(jni_env_);
  DO_THREAD_OFFSET(self_);
  DO_THREAD_OFFSET(stack_end_);
  DO_THREAD_OFFSET(state_);
  DO_THREAD_OFFSET(suspend_count_);
  DO_THREAD_OFFSET(thin_lock_id_);
  DO_THREAD_OFFSET(top_of_managed_stack_);
  DO_THREAD_OFFSET(top_of_managed_stack_pc_);
  DO_THREAD_OFFSET(top_sirt_);
#undef DO_THREAD_OFFSET

  size_t entry_point_count = arraysize(gThreadEntryPointInfo);
  CHECK_EQ(entry_point_count * size_of_pointers, sizeof(EntryPoints));
  uint32_t expected_offset = OFFSETOF_MEMBER(Thread, entrypoints_);
  for (size_t i = 0; i < entry_point_count; ++i) {
    CHECK_EQ(gThreadEntryPointInfo[i].offset, expected_offset);
    expected_offset += size_of_pointers;
    if (gThreadEntryPointInfo[i].offset == offset) {
      os << gThreadEntryPointInfo[i].name;
      return;
    }
  }
  os << offset;
}

class CatchBlockStackVisitor : public Thread::StackVisitor {
 public:
  CatchBlockStackVisitor(Class* to_find, Context* ljc)
      : to_find_(to_find), long_jump_context_(ljc), native_method_count_(0),
        method_tracing_active_(Runtime::Current()->IsMethodTracingActive()) {
#ifndef NDEBUG
    handler_pc_ = 0xEBADC0DE;
    handler_frame_.SetSP(reinterpret_cast<Method**>(0xEBADF00D));
#endif
  }

  bool VisitFrame(const Frame& fr, uintptr_t pc) {
    Method* method = fr.GetMethod();
    if (method == NULL) {
      // This is the upcall, we remember the frame and last_pc so that we may
      // long jump to them
      handler_pc_ = DemanglePc(pc);
      handler_frame_ = fr;
      return false;  // End stack walk.
    }
    uint32_t dex_pc = DexFile::kDexNoIndex;
    if (method->IsRuntimeMethod()) {
      // ignore callee save method
      DCHECK(method->IsCalleeSaveMethod());
    } else if (method->IsNative()) {
      native_method_count_++;
    } else {
      // Unwind stack when an exception occurs during method tracing
      if (UNLIKELY(method_tracing_active_)) {
#if !defined(ART_USE_LLVM_COMPILER)
        if (IsTraceExitPc(DemanglePc(pc))) {
          pc = ManglePc(TraceMethodUnwindFromCode(Thread::Current()));
        }
#else
        UNIMPLEMENTED(FATAL);
#endif
      }
      dex_pc = method->ToDexPC(pc);
    }
    if (dex_pc != DexFile::kDexNoIndex) {
      uint32_t found_dex_pc = method->FindCatchBlock(to_find_, dex_pc);
      if (found_dex_pc != DexFile::kDexNoIndex) {
        handler_pc_ = method->ToNativePC(found_dex_pc);
        handler_frame_ = fr;
        return false;  // End stack walk.
      }
    }
#if !defined(ART_USE_LLVM_COMPILER)
    // Caller may be handler, fill in callee saves in context
    long_jump_context_->FillCalleeSaves(fr);
#endif
    return true;  // Continue stack walk.
  }

  // The type of the exception catch block to find
  Class* to_find_;
  // Frame with found handler or last frame if no handler found
  Frame handler_frame_;
  // PC to branch to for the handler
  uintptr_t handler_pc_;
  // Context that will be the target of the long jump
  Context* long_jump_context_;
  // Number of native methods passed in crawl (equates to number of SIRTs to pop)
  uint32_t native_method_count_;
  // Is method tracing active?
  const bool method_tracing_active_;
};

void Thread::DeliverException() {
#if !defined(ART_USE_LLVM_COMPILER)
  const bool kDebugExceptionDelivery = false;
  Throwable* exception = GetException();  // Get exception from thread
  CHECK(exception != NULL);
  // Don't leave exception visible while we try to find the handler, which may cause class
  // resolution.
  ClearException();
  if (kDebugExceptionDelivery) {
    String* msg = exception->GetDetailMessage();
    std::string str_msg(msg != NULL ? msg->ToModifiedUtf8() : "");
    DumpStack(LOG(INFO) << "Delivering exception: " << PrettyTypeOf(exception)
                        << ": " << str_msg << std::endl);
  }

  Context* long_jump_context = GetLongJumpContext();
  CatchBlockStackVisitor catch_finder(exception->GetClass(), long_jump_context);
  WalkStack(&catch_finder, true);

  Method** sp;
  uintptr_t throw_native_pc;
  Method* throw_method = GetCurrentMethod(&throw_native_pc, &sp);
  uintptr_t catch_native_pc = catch_finder.handler_pc_;
  Method* catch_method = catch_finder.handler_frame_.GetMethod();
  Dbg::PostException(sp, throw_method, throw_native_pc, catch_method, catch_native_pc, exception);

  if (kDebugExceptionDelivery) {
    if (catch_method == NULL) {
      LOG(INFO) << "Handler is upcall";
    } else {
      ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
      const DexFile& dex_file =
          class_linker->FindDexFile(catch_method->GetDeclaringClass()->GetDexCache());
      int line_number = dex_file.GetLineNumFromPC(catch_method,
          catch_method->ToDexPC(catch_finder.handler_pc_));
      LOG(INFO) << "Handler: " << PrettyMethod(catch_method) << " (line: " << line_number << ")";
    }
  }
  SetException(exception);
  CHECK_NE(catch_native_pc, 0u);
  long_jump_context->SetSP(reinterpret_cast<uintptr_t>(catch_finder.handler_frame_.GetSP()));
  long_jump_context->SetPC(catch_native_pc);
  long_jump_context->SmashCallerSaves();
  long_jump_context->DoLongJump();
#endif
  LOG(FATAL) << "UNREACHABLE";
}

Context* Thread::GetLongJumpContext() {
  Context* result = long_jump_context_;
#if !defined(ART_USE_LLVM_COMPILER)
  if (result == NULL) {
    result = Context::Create();
    long_jump_context_ = result;
  }
#endif
  return result;
}

#if !defined(ART_USE_LLVM_COMPILER)
Method* Thread::GetCurrentMethod(uintptr_t* pc, Method*** sp) const {
  Frame f = top_of_managed_stack_;
  Method* m = f.GetMethod();
  uintptr_t native_pc = top_of_managed_stack_pc_;

  // We use JNI internally for exception throwing, so it's possible to arrive
  // here via a "FromCode" function, in which case there's a synthetic
  // callee-save method at the top of the stack. These shouldn't be user-visible,
  // so if we find one, skip it and return the compiled method underneath.
  if (m != NULL && m->IsCalleeSaveMethod()) {
    native_pc = f.GetReturnPC();
    f.Next();
    m = f.GetMethod();
  }
  if (pc != NULL) {
    *pc = (m != NULL) ? ManglePc(native_pc) : 0;
  }
  if (sp != NULL) {
    *sp = f.GetSP();
  }
  return m;
}
#else
Method* Thread::GetCurrentMethod(uintptr_t*, Method***) const {
  ShadowFrame* frame = top_shadow_frame_;
  if (frame == NULL) {
    return NULL;
  }
  return frame->GetMethod();
}
#endif

bool Thread::HoldsLock(Object* object) {
  if (object == NULL) {
    return false;
  }
  return object->GetThinLockId() == thin_lock_id_;
}

bool Thread::IsDaemon() {
  return gThread_daemon->GetBoolean(peer_);
}

#if !defined(ART_USE_LLVM_COMPILER)
class ReferenceMapVisitor : public Thread::StackVisitor {
 public:
  ReferenceMapVisitor(Context* context, Heap::RootVisitor* root_visitor, void* arg) :
    context_(context), root_visitor_(root_visitor), arg_(arg) {
  }

  bool VisitFrame(const Frame& frame, uintptr_t pc) {
    Method* m = frame.GetMethod();
    if (false) {
      LOG(INFO) << "Visiting stack roots in " << PrettyMethod(m)
                << StringPrintf("@ PC:%04x", m->ToDexPC(pc));
    }
    // Process register map (which native and callee save methods don't have)
    if (!m->IsNative() && !m->IsCalleeSaveMethod() && !m->IsProxyMethod()) {
      CHECK(m->GetGcMap() != NULL) << PrettyMethod(m);
      CHECK_NE(0U, m->GetGcMapLength()) << PrettyMethod(m);
      verifier::PcToReferenceMap map(m->GetGcMap(), m->GetGcMapLength());
      const uint8_t* reg_bitmap = map.FindBitMap(m->ToDexPC(pc));
      CHECK(reg_bitmap != NULL);
      const VmapTable vmap_table(m->GetVmapTableRaw());
      const DexFile::CodeItem* code_item = MethodHelper(m).GetCodeItem();
      DCHECK(code_item != NULL);  // can't be NULL or how would we compile its instructions?
      uint32_t core_spills = m->GetCoreSpillMask();
      uint32_t fp_spills = m->GetFpSpillMask();
      size_t frame_size = m->GetFrameSizeInBytes();
      // For all dex registers in the bitmap
      size_t num_regs = std::min(map.RegWidth() * 8,
                                 static_cast<size_t>(code_item->registers_size_));
      for (size_t reg = 0; reg < num_regs; ++reg) {
        // Does this register hold a reference?
        if (TestBitmap(reg, reg_bitmap)) {
          uint32_t vmap_offset;
          Object* ref;
          if (vmap_table.IsInContext(reg, vmap_offset)) {
            // Compute the register we need to load from the context
            uint32_t spill_mask = m->GetCoreSpillMask();
            CHECK_LT(vmap_offset, static_cast<uint32_t>(__builtin_popcount(spill_mask)));
            uint32_t matches = 0;
            uint32_t spill_shifts = 0;
            while (matches != (vmap_offset + 1)) {
              DCHECK_NE(spill_mask, 0u);
              matches += spill_mask & 1;  // Add 1 if the low bit is set
              spill_mask >>= 1;
              spill_shifts++;
            }
            spill_shifts--;  // wind back one as we want the last match
            ref = reinterpret_cast<Object*>(context_->GetGPR(spill_shifts));
          } else {
            ref = reinterpret_cast<Object*>(frame.GetVReg(code_item, core_spills, fp_spills,
                                                          frame_size, reg));
          }
          if (ref != NULL) {
            root_visitor_(ref, arg_);
          }
        }
      }
    }
    context_->FillCalleeSaves(frame);
    return true;
  }

 private:
  bool TestBitmap(int reg, const uint8_t* reg_vector) {
    return ((reg_vector[reg / 8] >> (reg % 8)) & 0x01) != 0;
  }

  // Context used to build up picture of callee saves
  Context* context_;
  // Call-back when we visit a root
  Heap::RootVisitor* root_visitor_;
  // Argument to call-back
  void* arg_;
};
#endif

void Thread::VisitRoots(Heap::RootVisitor* visitor, void* arg) {
  if (exception_ != NULL) {
    visitor(exception_, arg);
  }
  if (peer_ != NULL) {
    visitor(peer_, arg);
  }
  if (pre_allocated_OutOfMemoryError_ != NULL) {
    visitor(pre_allocated_OutOfMemoryError_, arg);
  }
  if (class_loader_override_ != NULL) {
    visitor(class_loader_override_, arg);
  }
  jni_env_->locals.VisitRoots(visitor, arg);
  jni_env_->monitors.VisitRoots(visitor, arg);

  SirtVisitRoots(visitor, arg);
  ShadowFrameVisitRoots(visitor, arg);

#if !defined(ART_USE_LLVM_COMPILER)
  // Cheat and steal the long jump context. Assume that we are not doing a GC during exception
  // delivery.
  Context* context = GetLongJumpContext();
  // Visit roots on this thread's stack
  ReferenceMapVisitor mapper(context, visitor, arg);
  WalkStack(&mapper);
#endif
}

#if VERIFY_OBJECT_ENABLED
static void VerifyObject(const Object* obj, void*) {
  Runtime::Current()->GetHeap()->VerifyObject(obj);
}

void Thread::VerifyStack() {
#if !defined(ART_USE_LLVM_COMPILER)
  UniquePtr<Context> context(Context::Create());
  ReferenceMapVisitor mapper(context.get(), VerifyObject, NULL);
  WalkStack(&mapper);
#endif
}
#endif

std::ostream& operator<<(std::ostream& os, const Thread& thread) {
  thread.Dump(os, false);
  return os;
}

void Thread::CheckSafeToLockOrUnlock(MutexRank rank, bool is_locking) {
  if (this == NULL) {
    CHECK(Runtime::Current()->IsShuttingDown());
    return;
  }
  if (is_locking) {
    if (held_mutexes_[rank] == 0) {
      bool bad_mutexes_held = false;
      for (int i = kMaxMutexRank; i > rank; --i) {
        if (held_mutexes_[i] != 0) {
          LOG(ERROR) << "holding " << static_cast<MutexRank>(i) << " while " << (is_locking ? "locking" : "unlocking") << " " << rank;
          bad_mutexes_held = true;
        }
      }
      CHECK(!bad_mutexes_held) << rank;
    }
    ++held_mutexes_[rank];
  } else {
    CHECK_GT(held_mutexes_[rank], 0U) << rank;
    --held_mutexes_[rank];
  }
}

void Thread::CheckSafeToWait(MutexRank rank) {
  if (this == NULL) {
    CHECK(Runtime::Current()->IsShuttingDown());
    return;
  }
  bool bad_mutexes_held = false;
  for (int i = kMaxMutexRank; i >= 0; --i) {
    if (i != rank && held_mutexes_[i] != 0) {
      LOG(ERROR) << "holding " << static_cast<MutexRank>(i) << " while doing condition variable wait on " << rank;
      bad_mutexes_held = true;
    }
  }
  if (held_mutexes_[rank] == 0) {
    LOG(ERROR) << "*not* holding " << rank << " while doing condition variable wait on it";
    bad_mutexes_held = true;
  }
  CHECK(!bad_mutexes_held);
}

}  // namespace art
