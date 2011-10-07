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
#include <sys/mman.h>

#include <algorithm>
#include <bitset>
#include <cerrno>
#include <iostream>
#include <list>

#include "class_linker.h"
#include "context.h"
#include "dex_verifier.h"
#include "heap.h"
#include "jni_internal.h"
#include "monitor.h"
#include "object.h"
#include "runtime.h"
#include "runtime_support.h"
#include "scoped_jni_thread_state.h"
#include "stack.h"
#include "stack_indirect_reference_table.h"
#include "thread_list.h"
#include "utils.h"

namespace art {

pthread_key_t Thread::pthread_key_self_;

static Class* gThreadLock = NULL;
static Class* gThrowable = NULL;
static Field* gThread_daemon = NULL;
static Field* gThread_group = NULL;
static Field* gThread_lock = NULL;
static Field* gThread_name = NULL;
static Field* gThread_priority = NULL;
static Field* gThread_uncaughtHandler = NULL;
static Field* gThread_vmData = NULL;
static Field* gThreadGroup_name = NULL;
static Field* gThreadLock_thread = NULL;
static Method* gThread_run = NULL;
static Method* gThreadGroup_removeThread = NULL;
static Method* gUncaughtExceptionHandler_uncaughtException = NULL;

// TODO: flesh out and move to appropriate location
String* ResolveStringFromCode(Method* method, int32_t string_idx) {
  UNIMPLEMENTED(FATAL) << "Resolve string; handle OOM";
  return NULL;  // Must return valid string or if exception, doesn't return
}

// TODO: move to appropriate location
static void ObjectInitFromCode(Object* o) {
  Class* c = o->GetClass();
  if (c->IsFinalizable()) {
    Heap::AddFinalizerReference(o);
  }
  /*
   * NOTE: once debugger/profiler support is added, we'll need to check
   * here and branch to actual compiled object.<init> to handle any
   * breakpoint/logging activites if either is active.
   */
}

void Thread::InitFunctionPointers() {
#if defined(__arm__)
  pShlLong = art_shl_long;
  pShrLong = art_shr_long;
  pUshrLong = art_ushr_long;
  pIdiv = __aeabi_idiv;
  pIdivmod = __aeabi_idivmod;
  pI2f = __aeabi_i2f;
  pF2iz = __aeabi_f2iz;
  pD2f = __aeabi_d2f;
  pF2d = __aeabi_f2d;
  pD2iz = __aeabi_d2iz;
  pL2f = __aeabi_l2f;
  pL2d = __aeabi_l2d;
  pFadd = __aeabi_fadd;
  pFsub = __aeabi_fsub;
  pFdiv = __aeabi_fdiv;
  pFmul = __aeabi_fmul;
  pFmodf = fmodf;
  pDadd = __aeabi_dadd;
  pDsub = __aeabi_dsub;
  pDdiv = __aeabi_ddiv;
  pDmul = __aeabi_dmul;
  pFmod = fmod;
  pLdivmod = __aeabi_ldivmod;
  pLmul = __aeabi_lmul;
  pAllocObjectFromCode = art_alloc_object_from_code;
  pAllocArrayFromCode = art_alloc_array_from_code;
  pCanPutArrayElementFromCode = art_can_put_array_element_from_code;
  pCheckAndAllocArrayFromCode = art_check_and_alloc_array_from_code;
  pCheckCastFromCode = art_check_cast_from_code;
  pHandleFillArrayDataFromCode = art_handle_fill_data_from_code;
  pInitializeStaticStorage = art_initialize_static_storage_from_code;
  pInvokeInterfaceTrampoline = art_invoke_interface_trampoline;
  pTestSuspendFromCode = art_test_suspend;
  pThrowArrayBoundsFromCode = art_throw_array_bounds_from_code;
  pThrowDivZeroFromCode = art_throw_div_zero_from_code;
  pThrowNegArraySizeFromCode = art_throw_neg_array_size_from_code;
  pThrowNoSuchMethodFromCode = art_throw_no_such_method_from_code;
  pThrowNullPointerFromCode = art_throw_null_pointer_exception_from_code;
  pThrowStackOverflowFromCode = art_throw_stack_overflow_from_code;
  pThrowVerificationErrorFromCode = art_throw_verification_error_from_code;
  pLockObjectFromCode = art_lock_object_from_code;
  pUnlockObjectFromCode = art_unlock_object_from_code;
#endif
  pDeliverException = art_deliver_exception_from_code;
  pThrowAbstractMethodErrorFromCode = ThrowAbstractMethodErrorFromCode;
  pUnresolvedDirectMethodTrampolineFromCode = UnresolvedDirectMethodTrampolineFromCode;
  pF2l = F2L;
  pD2l = D2L;
  pMemcpy = memcpy;
  pGet32Static = Field::Get32StaticFromCode;
  pSet32Static = Field::Set32StaticFromCode;
  pGet64Static = Field::Get64StaticFromCode;
  pSet64Static = Field::Set64StaticFromCode;
  pGetObjStatic = Field::GetObjStaticFromCode;
  pSetObjStatic = Field::SetObjStaticFromCode;
  pInitializeTypeFromCode = InitializeTypeFromCode;
  pResolveMethodFromCode = ResolveMethodFromCode;
  pInstanceofNonTrivialFromCode = Class::IsAssignableFromCode;
  pFindInstanceFieldFromCode = Field::FindInstanceFieldFromCode;
  pCheckSuspendFromCode = artCheckSuspendFromJni;
  pFindNativeMethod = FindNativeMethod;
  pDecodeJObjectInThread = DecodeJObjectInThread;
  pResolveStringFromCode = ResolveStringFromCode;
  pObjectInit = ObjectInitFromCode;
  pDebugMe = DebugMe;
}

void* Thread::CreateCallback(void* arg) {
  Thread* self = reinterpret_cast<Thread*>(arg);
  Runtime* runtime = Runtime::Current();

  self->Attach(runtime);

  String* thread_name = reinterpret_cast<String*>(gThread_name->GetObject(self->peer_));
  if (thread_name != NULL) {
    SetThreadName(thread_name->ToModifiedUtf8().c_str());
  }

  // Wait until it's safe to start running code. (There may have been a suspend-all
  // in progress while we were starting up.)
  runtime->GetThreadList()->WaitForGo();

  // TODO: say "hi" to the debugger.
  //if (gDvm.debuggerConnected) {
  //  dvmDbgPostThreadStart(self);
  //}

  // Invoke the 'run' method of our java.lang.Thread.
  CHECK(self->peer_ != NULL);
  Object* receiver = self->peer_;
  Method* m = receiver->GetClass()->FindVirtualMethodForVirtualOrInterface(gThread_run);
  m->Invoke(self, receiver, NULL, NULL);

  // Detach.
  runtime->GetThreadList()->Unregister();

  return NULL;
}

void SetVmData(Object* managed_thread, Thread* native_thread) {
  gThread_vmData->SetInt(managed_thread, reinterpret_cast<uintptr_t>(native_thread));
}

Thread* Thread::FromManagedThread(JNIEnv* env, jobject java_thread) {
  Object* thread = Decode<Object*>(env, java_thread);
  return reinterpret_cast<Thread*>(static_cast<uintptr_t>(gThread_vmData->GetInt(thread)));
}

size_t FixStackSize(size_t stack_size) {
  // A stack size of zero means "use the default".
  if (stack_size == 0) {
    stack_size = Runtime::Current()->GetDefaultStackSize();
  }

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

void Thread::Create(Object* peer, size_t stack_size) {
  CHECK(peer != NULL);

  stack_size = FixStackSize(stack_size);

  Thread* native_thread = new Thread;
  native_thread->peer_ = peer;

  // Thread.start is synchronized, so we know that vmData is 0,
  // and know that we're not racing to assign it.
  SetVmData(peer, native_thread);

  pthread_attr_t attr;
  CHECK_PTHREAD_CALL(pthread_attr_init, (&attr), "new thread");
  CHECK_PTHREAD_CALL(pthread_attr_setdetachstate, (&attr, PTHREAD_CREATE_DETACHED), "PTHREAD_CREATE_DETACHED");
  CHECK_PTHREAD_CALL(pthread_attr_setstacksize, (&attr, stack_size), stack_size);
  CHECK_PTHREAD_CALL(pthread_create, (&native_thread->pthread_, &attr, Thread::CreateCallback, native_thread), "new thread");
  CHECK_PTHREAD_CALL(pthread_attr_destroy, (&attr), "new thread");

  // Let the child know when it's safe to start running.
  Runtime::Current()->GetThreadList()->SignalGo(native_thread);
}

void Thread::Attach(const Runtime* runtime) {
  InitCpu();
  InitFunctionPointers();

  thin_lock_id_ = Runtime::Current()->GetThreadList()->AllocThreadId();

  tid_ = ::art::GetTid();
  pthread_ = pthread_self();

  InitStackHwm();

  CHECK_PTHREAD_CALL(pthread_setspecific, (Thread::pthread_key_self_, this), "attach");

  jni_env_ = new JNIEnvExt(this, runtime->GetJavaVM());

  runtime->GetThreadList()->Register();
}

Thread* Thread::Attach(const Runtime* runtime, const char* name, bool as_daemon) {
  Thread* self = new Thread;
  self->Attach(runtime);

  self->SetState(Thread::kNative);

  SetThreadName(name);

  // If we're the main thread, ClassLinker won't be created until after we're attached,
  // so that thread needs a two-stage attach. Regular threads don't need this hack.
  if (self->thin_lock_id_ != ThreadList::kMainId) {
    self->CreatePeer(name, as_daemon);
  }

  return self;
}

jobject GetWellKnownThreadGroup(JNIEnv* env, const char* field_name) {
  jclass thread_group_class = env->FindClass("java/lang/ThreadGroup");
  jfieldID fid = env->GetStaticFieldID(thread_group_class, field_name, "Ljava/lang/ThreadGroup;");
  jobject thread_group = env->GetStaticObjectField(thread_group_class, fid);
  // This will be null in the compiler (and tests), but never in a running system.
  //CHECK(thread_group != NULL) << "java.lang.ThreadGroup." << field_name << " not initialized";
  return thread_group;
}

void Thread::CreatePeer(const char* name, bool as_daemon) {
  JNIEnv* env = jni_env_;

  const char* field_name = (GetThinLockId() == ThreadList::kMainId) ? "mMain" : "mSystem";
  jobject thread_group = GetWellKnownThreadGroup(env, field_name);
  jobject thread_name = env->NewStringUTF(name);
  jint thread_priority = GetNativePriority();
  jboolean thread_is_daemon = as_daemon;

  jclass c = env->FindClass("java/lang/Thread");
  jmethodID mid = env->GetMethodID(c, "<init>", "(Ljava/lang/ThreadGroup;Ljava/lang/String;IZ)V");

  jobject peer = env->NewObject(c, mid, thread_group, thread_name, thread_priority, thread_is_daemon);
  peer_ = DecodeJObject(peer);
  SetVmData(peer_, Thread::Current());

  // Because we mostly run without code available (in the compiler, in tests), we
  // manually assign the fields the constructor should have set.
  // TODO: lose this.
  gThread_daemon->SetBoolean(peer_, thread_is_daemon);
  gThread_group->SetObject(peer_, Decode<Object*>(env, thread_group));
  gThread_name->SetObject(peer_, Decode<Object*>(env, thread_name));
  gThread_priority->SetInt(peer_, thread_priority);
}

void Thread::InitStackHwm() {
  pthread_attr_t attributes;
  CHECK_PTHREAD_CALL(pthread_getattr_np, (pthread_, &attributes), __FUNCTION__);

  void* temp_stack_base;
  CHECK_PTHREAD_CALL(pthread_attr_getstack, (&attributes, &temp_stack_base, &stack_size_),
                     __FUNCTION__);
  stack_base_ = reinterpret_cast<byte*>(temp_stack_base);

  if (stack_size_ <= kStackOverflowReservedBytes) {
    LOG(FATAL) << "attempt to attach a thread with a too-small stack (" << stack_size_ << " bytes)";
  }

  // Set stack_end_ to the bottom of the stack saving space of stack overflows
  ResetDefaultStackEnd();

  // Sanity check.
  int stack_variable;
  CHECK_GT(&stack_variable, (void*) stack_end_);

  CHECK_PTHREAD_CALL(pthread_attr_destroy, (&attributes), __FUNCTION__);
}

void Thread::Dump(std::ostream& os) const {
  DumpState(os);
  DumpStack(os);
}

std::string GetSchedulerGroup(pid_t tid) {
  // /proc/<pid>/group looks like this:
  // 2:devices:/
  // 1:cpuacct,cpu:/
  // We want the third field from the line whose second field contains the "cpu" token.
  std::string cgroup_file;
  if (!ReadFileToString("/proc/self/cgroup", &cgroup_file)) {
    return "";
  }
  std::vector<std::string> cgroup_lines;
  Split(cgroup_file, '\n', cgroup_lines);
  for (size_t i = 0; i < cgroup_lines.size(); ++i) {
    std::vector<std::string> cgroup_fields;
    Split(cgroup_lines[i], ':', cgroup_fields);
    std::vector<std::string> cgroups;
    Split(cgroup_fields[1], ',', cgroups);
    for (size_t i = 0; i < cgroups.size(); ++i) {
      if (cgroups[i] == "cpu") {
        return cgroup_fields[2].substr(1); // Skip the leading slash.
      }
    }
  }
  return "";
}

void Thread::DumpState(std::ostream& os) const {
  std::string thread_name("<native thread without managed peer>");
  std::string group_name;
  int priority;
  bool is_daemon = false;

  if (peer_ != NULL) {
    String* thread_name_string = reinterpret_cast<String*>(gThread_name->GetObject(peer_));
    thread_name = (thread_name_string != NULL) ? thread_name_string->ToModifiedUtf8() : "<null>";
    priority = gThread_priority->GetInt(peer_);
    is_daemon = gThread_daemon->GetBoolean(peer_);

    Object* thread_group = gThread_group->GetObject(peer_);
    if (thread_group != NULL) {
      String* group_name_string = reinterpret_cast<String*>(gThreadGroup_name->GetObject(thread_group));
      group_name = (group_name_string != NULL) ? group_name_string->ToModifiedUtf8() : "<null>";
    }
  } else {
    // This name may be truncated, but it's the best we can do in the absence of a managed peer.
    std::string stats;
    if (ReadFileToString(StringPrintf("/proc/self/task/%d/stat", GetTid()).c_str(), &stats)) {
      size_t start = stats.find('(') + 1;
      size_t end = stats.find(')') - start;
      thread_name = stats.substr(start, end);
    }
    priority = GetNativePriority();
  }

  int policy;
  sched_param sp;
  CHECK_PTHREAD_CALL(pthread_getschedparam, (pthread_, &policy, &sp), __FUNCTION__);

  std::string scheduler_group(GetSchedulerGroup(GetTid()));
  if (scheduler_group.empty()) {
    scheduler_group = "default";
  }

  os << '"' << thread_name << '"';
  if (is_daemon) {
    os << " daemon";
  }
  os << " prio=" << priority
     << " tid=" << GetThinLockId()
     << " " << GetState() << "\n";

  int debug_suspend_count = 0; // TODO
  os << "  | group=\"" << group_name << "\""
     << " sCount=" << suspend_count_
     << " dsCount=" << debug_suspend_count
     << " obj=" << reinterpret_cast<void*>(peer_)
     << " self=" << reinterpret_cast<const void*>(this) << "\n";
  os << "  | sysTid=" << GetTid()
     << " nice=" << getpriority(PRIO_PROCESS, GetTid())
     << " sched=" << policy << "/" << sp.sched_priority
     << " cgrp=" << scheduler_group
     << " handle=" << GetImpl() << "\n";

  // Grab the scheduler stats for this thread.
  std::string scheduler_stats;
  if (ReadFileToString(StringPrintf("/proc/self/task/%d/schedstat", GetTid()).c_str(), &scheduler_stats)) {
    scheduler_stats.resize(scheduler_stats.size() - 1); // Lose the trailing '\n'.
  } else {
    scheduler_stats = "0 0 0";
  }

  int utime = 0;
  int stime = 0;
  int task_cpu = 0;
  std::string stats;
  if (ReadFileToString(StringPrintf("/proc/self/task/%d/stat", GetTid()).c_str(), &stats)) {
    // Skip the command, which may contain spaces.
    stats = stats.substr(stats.find(')') + 2);
    // Extract the three fields we care about.
    std::vector<std::string> fields;
    Split(stats, ' ', fields);
    utime = strtoull(fields[11].c_str(), NULL, 10);
    stime = strtoull(fields[12].c_str(), NULL, 10);
    task_cpu = strtoull(fields[36].c_str(), NULL, 10);
  }

  os << "  | schedstat=( " << scheduler_stats << " )"
     << " utm=" << utime
     << " stm=" << stime
     << " core=" << task_cpu
     << " HZ=" << sysconf(_SC_CLK_TCK) << "\n";
}

struct StackDumpVisitor : public Thread::StackVisitor {
  StackDumpVisitor(std::ostream& os, const Thread* thread)
      : os(os), thread(thread), frame_count(0) {
  }

  virtual ~StackDumpVisitor() {
  }

  void VisitFrame(const Frame& frame, uintptr_t pc) {
    if (!frame.HasMethod()) {
      return;
    }

    Method* m = frame.GetMethod();
    Class* c = m->GetDeclaringClass();
    ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
    const DexFile& dex_file = class_linker->FindDexFile(c->GetDexCache());

    os << "  at " << PrettyMethod(m, false);
    if (m->IsNative()) {
      os << "(Native method)";
    } else {
      int line_number = dex_file.GetLineNumFromPC(m, m->ToDexPC(pc));
      os << "(" << c->GetSourceFile()->ToModifiedUtf8() << ":" << line_number << ")";
    }
    os << "\n";

    if (frame_count++ == 0) {
      Monitor::DescribeWait(os, thread);
    }
  }

  std::ostream& os;
  const Thread* thread;
  int frame_count;
};

void Thread::DumpStack(std::ostream& os) const {
  StackDumpVisitor dumper(os, this);
  WalkStack(&dumper);
}

Thread::State Thread::SetState(Thread::State new_state) {
  Thread::State old_state = state_;
  if (old_state == new_state) {
    return old_state;
  }

  volatile void* raw = reinterpret_cast<volatile void*>(&state_);
  volatile int32_t* addr = reinterpret_cast<volatile int32_t*>(raw);

  if (new_state == Thread::kRunnable) {
    /*
     * Change our status to Thread::kRunnable.  The transition requires
     * that we check for pending suspension, because the VM considers
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
    if (ANNOTATE_UNPROTECTED_READ(suspend_count_) != 0) {
      Runtime::Current()->GetThreadList()->FullSuspendCheck(this);
    }
  } else {
    /*
     * Not changing to Thread::kRunnable. No additional work required.
     *
     * We use a releasing store to ensure that, if we were runnable,
     * any updates we previously made to objects on the managed heap
     * will be observed before the state change.
     */
    android_atomic_release_store(new_state, addr);
  }

  return old_state;
}

void Thread::WaitUntilSuspended() {
  // TODO: dalvik dropped the waiting thread's priority after a while.
  // TODO: dalvik timed out and aborted.
  useconds_t delay = 0;
  while (GetState() == Thread::kRunnable) {
    useconds_t new_delay = delay * 2;
    CHECK_GE(new_delay, delay);
    delay = new_delay;
    if (delay == 0) {
      sched_yield();
      delay = 10000;
    } else {
      usleep(delay);
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
    LOG(FATAL) << "newly-created pthread TLS slot is not NULL";
  }
}

// TODO: make more accessible?
Class* FindPrimitiveClassOrDie(ClassLinker* class_linker, char descriptor) {
  Class* c = class_linker->FindPrimitiveClass(descriptor);
  CHECK(c != NULL) << descriptor;
  return c;
}

// TODO: make more accessible?
Class* FindClassOrDie(ClassLinker* class_linker, const char* descriptor) {
  Class* c = class_linker->FindSystemClass(descriptor);
  CHECK(c != NULL) << descriptor;
  return c;
}

// TODO: make more accessible?
Field* FindFieldOrDie(Class* c, const char* name, Class* type) {
  Field* f = c->FindDeclaredInstanceField(name, type);
  CHECK(f != NULL) << PrettyClass(c) << " " << name << " " << PrettyClass(type);
  return f;
}

// TODO: make more accessible?
Method* FindMethodOrDie(Class* c, const char* name, const char* signature) {
  Method* m = c->FindVirtualMethod(name, signature);
  CHECK(m != NULL) << PrettyClass(c) << " " << name << " " << signature;
  return m;
}

void Thread::FinishStartup() {
  // Now the ClassLinker is ready, we can find the various Class*, Field*, and Method*s we need.
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();

  Class* boolean_class = FindPrimitiveClassOrDie(class_linker, 'Z');
  Class* int_class = FindPrimitiveClassOrDie(class_linker, 'I');
  Class* String_class = FindClassOrDie(class_linker, "Ljava/lang/String;");
  Class* Thread_class = FindClassOrDie(class_linker, "Ljava/lang/Thread;");
  Class* ThreadGroup_class = FindClassOrDie(class_linker, "Ljava/lang/ThreadGroup;");
  Class* UncaughtExceptionHandler_class = FindClassOrDie(class_linker, "Ljava/lang/Thread$UncaughtExceptionHandler;");
  gThreadLock = FindClassOrDie(class_linker, "Ljava/lang/ThreadLock;");
  gThrowable = FindClassOrDie(class_linker, "Ljava/lang/Throwable;");

  gThread_daemon = FindFieldOrDie(Thread_class, "daemon", boolean_class);
  gThread_group = FindFieldOrDie(Thread_class, "group", ThreadGroup_class);
  gThread_lock = FindFieldOrDie(Thread_class, "lock", gThreadLock);
  gThread_name = FindFieldOrDie(Thread_class, "name", String_class);
  gThread_priority = FindFieldOrDie(Thread_class, "priority", int_class);
  gThread_uncaughtHandler = FindFieldOrDie(Thread_class, "uncaughtHandler", UncaughtExceptionHandler_class);
  gThread_vmData = FindFieldOrDie(Thread_class, "vmData", int_class);
  gThreadGroup_name = FindFieldOrDie(ThreadGroup_class, "name", String_class);
  gThreadLock_thread = FindFieldOrDie(gThreadLock, "thread", Thread_class);

  gThread_run = FindMethodOrDie(Thread_class, "run", "()V");
  gThreadGroup_removeThread = FindMethodOrDie(ThreadGroup_class, "removeThread", "(Ljava/lang/Thread;)V");
  gUncaughtExceptionHandler_uncaughtException = FindMethodOrDie(UncaughtExceptionHandler_class,
      "uncaughtException", "(Ljava/lang/Thread;Ljava/lang/Throwable;)V");

  // Finish attaching the main thread.
  Thread::Current()->CreatePeer("main", false);
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
    : peer_(NULL),
      top_of_managed_stack_(),
      top_of_managed_stack_pc_(0),
      wait_mutex_(new Mutex("Thread wait mutex")),
      wait_cond_(new ConditionVariable("Thread wait condition variable")),
      wait_monitor_(NULL),
      interrupted_(false),
      wait_next_(NULL),
      monitor_enter_object_(NULL),
      card_table_(0),
      stack_end_(NULL),
      native_to_managed_record_(NULL),
      top_sirt_(NULL),
      jni_env_(NULL),
      state_(Thread::kNative),
      self_(NULL),
      runtime_(NULL),
      exception_(NULL),
      suspend_count_(0),
      class_loader_override_(NULL),
      long_jump_context_(NULL),
      throwing_OOME_(false) {
  CHECK_EQ((sizeof(Thread) % 4), 0U) << sizeof(Thread);
}

void MonitorExitVisitor(const Object* object, void*) {
  Object* entered_monitor = const_cast<Object*>(object);
  entered_monitor->MonitorExit(Thread::Current());
}

Thread::~Thread() {
  SetState(Thread::kRunnable);

  // On thread detach, all monitors entered with JNI MonitorEnter are automatically exited.
  if (jni_env_ != NULL) {
    jni_env_->monitors.VisitRoots(MonitorExitVisitor, NULL);
  }

  if (peer_ != NULL) {
    Object* group = gThread_group->GetObject(peer_);

    // Handle any pending exception.
    if (IsExceptionPending()) {
      // Get and clear the exception.
      Object* exception = GetException();
      ClearException();

      // If the thread has its own handler, use that.
      Object* handler = gThread_uncaughtHandler->GetObject(peer_);
      if (handler == NULL) {
        // Otherwise use the thread group's default handler.
        handler = group;
      }

      // Call the handler.
      Method* m = handler->GetClass()->FindVirtualMethodForVirtualOrInterface(gUncaughtExceptionHandler_uncaughtException);
      Object* args[2];
      args[0] = peer_;
      args[1] = exception;
      m->Invoke(this, handler, reinterpret_cast<byte*>(&args), NULL);

      // If the handler threw, clear that exception too.
      ClearException();
    }

    // this.group.removeThread(this);
    // group can be null if we're in the compiler or a test.
    if (group != NULL) {
      Method* m = group->GetClass()->FindVirtualMethodForVirtualOrInterface(gThreadGroup_removeThread);
      Object* args = peer_;
      m->Invoke(this, group, reinterpret_cast<byte*>(&args), NULL);
    }

    // this.vmData = 0;
    SetVmData(peer_, NULL);

    // TODO: say "bye" to the debugger.
    //if (gDvm.debuggerConnected) {
    //  dvmDbgPostThreadDeath(self);
    //}

    // Thread.join() is implemented as an Object.wait() on the Thread.lock
    // object. Signal anyone who is waiting.
    Thread* self = Thread::Current();
    Object* lock = gThread_lock->GetObject(peer_);
    // (This conditional is only needed for tests, where Thread.lock won't have been set.)
    if (lock != NULL) {
      lock->MonitorEnter(self);
      lock->NotifyAll();
      lock->MonitorExit(self);
    }
  }

  delete jni_env_;
  jni_env_ = NULL;

  SetState(Thread::kTerminated);

  delete wait_cond_;
  delete wait_mutex_;

  delete long_jump_context_;
}

size_t Thread::NumSirtReferences() {
  size_t count = 0;
  for (StackIndirectReferenceTable* cur = top_sirt_; cur; cur = cur->Link()) {
    count += cur->NumberOfReferences();
  }
  return count;
}

bool Thread::SirtContains(jobject obj) {
  Object** sirt_entry = reinterpret_cast<Object**>(obj);
  for (StackIndirectReferenceTable* cur = top_sirt_; cur; cur = cur->Link()) {
    size_t num_refs = cur->NumberOfReferences();
    // A SIRT should always have a jobject/jclass as a native method is passed
    // in a this pointer or a class
    DCHECK_GT(num_refs, 0u);
    if ((&cur->References()[0] <= sirt_entry) &&
        (sirt_entry <= (&cur->References()[num_refs - 1]))) {
      return true;
    }
  }
  return false;
}

void Thread::SirtVisitRoots(Heap::RootVisitor* visitor, void* arg) {
  for (StackIndirectReferenceTable* cur = top_sirt_; cur; cur = cur->Link()) {
    size_t num_refs = cur->NumberOfReferences();
    for (size_t j = 0; j < num_refs; j++) {
      visitor(cur->References()[j], arg);
    }
  }
}

void Thread::PopSirt() {
  CHECK(top_sirt_ != NULL);
  top_sirt_ = top_sirt_->Link();
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
    if (SirtContains(obj)) {
      result = *reinterpret_cast<Object**>(obj);  // Read from SIRT
    } else if (jni_env_->work_around_app_jni_bugs) {
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
      Heap::VerifyObject(result);
    }
  }
  return result;
}

class CountStackDepthVisitor : public Thread::StackVisitor {
 public:
  CountStackDepthVisitor() : depth_(0), skip_depth_(0), skipping_(true) {}

  virtual void VisitFrame(const Frame& frame, uintptr_t pc) {
    // We want to skip frames up to and including the exception's constructor.
    // Note we also skip the frame if it doesn't have a method (namely the callee
    // save frame)
    DCHECK(gThrowable != NULL);
    if (skipping_ && frame.HasMethod() && !gThrowable->IsAssignableFrom(frame.GetMethod()->GetDeclaringClass())) {
      skipping_ = false;
    }
    if (!skipping_) {
      ++depth_;
    } else {
      ++skip_depth_;
    }
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
  explicit BuildInternalStackTraceVisitor(int depth, int skip_depth, ScopedJniThreadState& ts)
      : skip_depth_(skip_depth), count_(0) {
    // Allocate method trace with an extra slot that will hold the PC trace
    method_trace_ = Runtime::Current()->GetClassLinker()->AllocObjectArray<Object>(depth + 1);
    // Register a local reference as IntArray::Alloc may trigger GC
    local_ref_ = AddLocalReference<jobject>(ts.Env(), method_trace_);
    pc_trace_ = IntArray::Alloc(depth);
#ifdef MOVING_GARBAGE_COLLECTOR
    // Re-read after potential GC
    method_trace = Decode<ObjectArray<Object>*>(ts.Env(), local_ref_);
#endif
    // Save PC trace in last element of method trace, also places it into the
    // object graph.
    method_trace_->Set(depth, pc_trace_);
  }

  virtual ~BuildInternalStackTraceVisitor() {}

  virtual void VisitFrame(const Frame& frame, uintptr_t pc) {
    if (skip_depth_ > 0) {
      skip_depth_--;
      return;
    }
    method_trace_->Set(count_, frame.GetMethod());
    pc_trace_->Set(count_, pc);
    ++count_;
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

// TODO: remove this.
uintptr_t ManglePc(uintptr_t pc) {
  // Move the PC back 2 bytes as a call will frequently terminate the
  // decoding of a particular instruction and we want to make sure we
  // get the Dex PC of the instruction with the call and not the
  // instruction following.
  if (pc > 0) { pc -= 2; }
  return pc;
}

// TODO: remove this.
uintptr_t DemanglePc(uintptr_t pc) {
  // Revert mangling for the case where we need the PC to return to the upcall
  return pc + 2;
}

void Thread::WalkStack(StackVisitor* visitor) const {
  Frame frame = GetTopOfStack();
  uintptr_t pc = ManglePc(top_of_managed_stack_pc_);
  // TODO: enable this CHECK after native_to_managed_record_ is initialized during startup.
  // CHECK(native_to_managed_record_ != NULL);
  NativeToManagedRecord* record = native_to_managed_record_;

  while (frame.GetSP() != 0) {
    for ( ; frame.GetMethod() != 0; frame.Next()) {
      // DCHECK(frame.GetMethod()->IsWithinCode(pc));  // TODO: restore IsWithinCode
      visitor->VisitFrame(frame, pc);
      pc = ManglePc(frame.GetReturnPC());
    }
    if (record == NULL) {
      break;
    }
    // last_tos should return Frame instead of sp?
    frame.SetSP(reinterpret_cast<Method**>(record->last_top_of_managed_stack_));
    pc = ManglePc(record->last_top_of_managed_stack_pc_);
    record = record->link_;
  }
}

void Thread::WalkStackUntilUpCall(StackVisitor* visitor, bool include_upcall) const {
  Frame frame = GetTopOfStack();
  uintptr_t pc = ManglePc(top_of_managed_stack_pc_);

  if (frame.GetSP() != 0) {
    for ( ; frame.GetMethod() != 0; frame.Next()) {
      // DCHECK(frame.GetMethod()->IsWithinCode(pc));  // TODO: restore IsWithinCode
      visitor->VisitFrame(frame, pc);
      pc = ManglePc(frame.GetReturnPC());
    }
    if (include_upcall) {
      visitor->VisitFrame(frame, pc);
    }
  }
}

jobject Thread::CreateInternalStackTrace(JNIEnv* env) const {
  // Compute depth of stack
  CountStackDepthVisitor count_visitor;
  WalkStack(&count_visitor);
  int32_t depth = count_visitor.GetDepth();
  int32_t skip_depth = count_visitor.GetSkipDepth();

  // Transition into runnable state to work on Object*/Array*
  ScopedJniThreadState ts(env);

  // Build internal stack trace
  BuildInternalStackTraceVisitor build_trace_visitor(depth, skip_depth, ts);
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
  int32_t depth = method_trace->GetLength()-1;
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
    result = AddLocalReference<jobjectArray>(ts.Env(), java_traces);
  }

  if (stack_depth != NULL) {
    *stack_depth = depth;
  }

  for (int32_t i = 0; i < depth; ++i) {
    // Prepare parameters for StackTraceElement(String cls, String method, String file, int line)
    Method* method = down_cast<Method*>(method_trace->Get(i));
    uint32_t native_pc = pc_trace->Get(i);
    Class* klass = method->GetDeclaringClass();
    const DexFile& dex_file = class_linker->FindDexFile(klass->GetDexCache());
    std::string class_name(PrettyDescriptor(klass->GetDescriptor()));

    // Allocate element, potentially triggering GC
    StackTraceElement* obj =
        StackTraceElement::Alloc(String::AllocFromModifiedUtf8(class_name.c_str()),
                                 method->GetName(),
                                 klass->GetSourceFile(),
                                 dex_file.GetLineNumFromPC(method,
                                     method->ToDexPC(native_pc)));
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
  jclass exception_class = env->FindClass(descriptor.c_str());
  CHECK(exception_class != NULL) << "descriptor=\"" << descriptor << "\"";
  int rc = env->ThrowNew(exception_class, msg);
  CHECK_EQ(rc, JNI_OK);
  env->DeleteLocalRef(exception_class);
}

void Thread::ThrowOutOfMemoryError(Class* c, size_t byte_count) {
  if (!throwing_OOME_) {
    throwing_OOME_ = true;
    ThrowNewException("Ljava/lang/OutOfMemoryError;", NULL);
    LOG(ERROR) << "Failed to allocate a " << PrettyDescriptor(c->GetDescriptor()) << " (" << byte_count << " bytes)";
  } else {
    UNIMPLEMENTED(FATAL) << "throw one i prepared earlier...";
  }
  throwing_OOME_ = false;
}

class CatchBlockStackVisitor : public Thread::StackVisitor {
 public:
  CatchBlockStackVisitor(Class* to_find, Context* ljc)
      : found_(false), to_find_(to_find), long_jump_context_(ljc), native_method_count_(0) {
#ifndef NDEBUG
    handler_pc_ = 0xEBADC0DE;
    handler_frame_.SetSP(reinterpret_cast<Method**>(0xEBADF00D));
#endif
  }

  virtual void VisitFrame(const Frame& fr, uintptr_t pc) {
    if (!found_) {
      Method* method = fr.GetMethod();
      if (method == NULL) {
        // This is the upcall, we remember the frame and last_pc so that we may
        // long jump to them
        handler_pc_ = DemanglePc(pc);
        handler_frame_ = fr;
        return;
      }
      uint32_t dex_pc = DexFile::kDexNoIndex;
      if (method->IsCalleeSaveMethod()) {
        // ignore callee save method
      } else if (method->IsNative()) {
        native_method_count_++;
      } else {
        dex_pc = method->ToDexPC(pc);
      }
      if (dex_pc != DexFile::kDexNoIndex) {
        uint32_t found_dex_pc = method->FindCatchBlock(to_find_, dex_pc);
        if (found_dex_pc != DexFile::kDexNoIndex) {
          found_ = true;
          handler_pc_ = method->ToNativePC(found_dex_pc);
          handler_frame_ = fr;
        }
      }
      if (!found_) {
        // Caller may be handler, fill in callee saves in context
        long_jump_context_->FillCalleeSaves(fr);
      }
    }
  }

  // Did we find a catch block yet?
  bool found_;
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
};

void Thread::DeliverException() {
  Throwable *exception = GetException();  // Set exception on thread
  CHECK(exception != NULL);

  Context* long_jump_context = GetLongJumpContext();
  CatchBlockStackVisitor catch_finder(exception->GetClass(), long_jump_context);
  WalkStackUntilUpCall(&catch_finder, true);

  // Pop any SIRT
  if (catch_finder.native_method_count_ == 1) {
    PopSirt();
  } else {
    // We only expect the stack crawl to have passed 1 native method as it's terminated
    // by an up call
    DCHECK_EQ(catch_finder.native_method_count_, 0u);
  }
  long_jump_context->SetSP(reinterpret_cast<intptr_t>(catch_finder.handler_frame_.GetSP()));
  long_jump_context->SetPC(catch_finder.handler_pc_);
  long_jump_context->DoLongJump();
}

Context* Thread::GetLongJumpContext() {
  Context* result = long_jump_context_;
  if (result == NULL) {
    result = Context::Create();
    long_jump_context_ = result;
  }
  return result;
}

bool Thread::HoldsLock(Object* object) {
  if (object == NULL) {
    return false;
  }
  return object->GetLockOwner() == thin_lock_id_;
}

bool Thread::IsDaemon() {
  return gThread_daemon->GetBoolean(peer_);
}

class ReferenceMapVisitor : public Thread::StackVisitor {
 public:
  ReferenceMapVisitor(Context* context, Heap::RootVisitor* root_visitor, void* arg) :
    context_(context), root_visitor_(root_visitor), arg_(arg) {
  }

  void VisitFrame(const Frame& frame, uintptr_t pc) {
    Method* m = frame.GetMethod();
    // Process register map (which native and callee save methods don't have)
    if (!m->IsNative() && !m->IsCalleeSaveMethod()) {
      UniquePtr<art::DexVerifier::RegisterMap> map(art::DexVerifier::GetExpandedRegisterMap(m));
      const uint8_t* reg_bitmap = art::DexVerifier::RegisterMapGetLine(map.get(), m->ToDexPC(pc));
      LOG(INFO) << "Visiting stack roots in " << PrettyMethod(m, false)
                << "@ PC: " << m->ToDexPC(pc);
      CHECK(reg_bitmap != NULL);
      const uint16_t* vmap = m->GetVmapTable();
      // For all dex registers
      for (int reg = 0; reg < m->NumRegisters(); ++reg) {
        // Does this register hold a reference?
        if (TestBitmap(reg, reg_bitmap)) {
          // Is the reference in the context or on the stack?
          bool in_context = false;
          uint32_t vmap_offset = 0xEBAD0FF5;
          // TODO: take advantage of the registers being ordered
          for (int i = 0; i < m->GetVmapTableLength(); i++) {
            if (vmap[i] == reg) {
              in_context = true;
              vmap_offset = i;
              break;
            }
          }
          Object* ref;
          if (in_context) {
            // Compute the register we need to load from the context
            uint32_t spill_mask = m->GetCoreSpillMask();
            uint32_t matches = 0;
            uint32_t spill_shifts = 0;
            while (matches != (vmap_offset + 1)) {
              CHECK_NE(spill_mask, 0u);
              matches += spill_mask & 1;  // Add 1 if the low bit is set
              spill_mask >>= 1;
              spill_shifts++;
            }
            spill_shifts--;  // wind back one as we want the last match
            ref = reinterpret_cast<Object*>(context_->GetGPR(spill_shifts));
          } else {
            ref = reinterpret_cast<Object*>(frame.GetVReg(m ,reg));
          }
          if (ref != NULL) {
            root_visitor_(ref, arg_);
          }
        }
      }
    }
    context_->FillCalleeSaves(frame);
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

void Thread::VisitRoots(Heap::RootVisitor* visitor, void* arg) {
  if (exception_ != NULL) {
    visitor(exception_, arg);
  }
  if (peer_ != NULL) {
    visitor(peer_, arg);
  }
  jni_env_->locals.VisitRoots(visitor, arg);
  jni_env_->monitors.VisitRoots(visitor, arg);

  SirtVisitRoots(visitor, arg);

  // Cheat and steal the long jump context. Assume that we are not doing a GC during exception
  // delivery.
  Context* context = GetLongJumpContext();
  // Visit roots on this thread's stack
  ReferenceMapVisitor mapper(context, visitor, arg);
  WalkStack(&mapper);
}

static const char* kStateNames[] = {
  "Terminated",
  "Runnable",
  "TimedWaiting",
  "Blocked",
  "Waiting",
  "Initializing",
  "Starting",
  "Native",
  "VmWait",
  "Suspended",
};
std::ostream& operator<<(std::ostream& os, const Thread::State& state) {
  int32_t int_state = static_cast<int32_t>(state);
  if (state >= Thread::kTerminated && state <= Thread::kSuspended) {
    os << kStateNames[int_state];
  } else {
    os << "State[" << int_state << "]";
  }
  return os;
}

std::ostream& operator<<(std::ostream& os, const Thread& thread) {
  os << "Thread[" << &thread
     << ",pthread_t=" << thread.GetImpl()
     << ",tid=" << thread.GetTid()
     << ",id=" << thread.GetThinLockId()
     << ",state=" << thread.GetState()
     << ",peer=" << thread.GetPeer()
     << "]";
  return os;
}

}  // namespace art
