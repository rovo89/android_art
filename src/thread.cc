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
#include "context.h"
#include "dex_verifier.h"
#include "heap.h"
#include "jni_internal.h"
#include "monitor.h"
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

void Thread::InitCardTable() {
  card_table_ = Runtime::Current()->GetHeap()->GetCardTable()->GetBiasedBegin();
}

void Thread::InitFunctionPointers() {
#if defined(__mips)
  pShlLong = art_shl_long;
  pShrLong = art_shr_long;
  pUshrLong = art_ushr_long;
  pI2f = __floatsisf;
  pF2iz = __fixsfi;
  pD2f = __truncdfsf2;
  pF2d = __extendsfdfs;
  pD2iz = __fixdfsi;
  pL2f = __floatdisf;
  pL2d = __floatdidf;
  pFadd = __addsf3;
  pFsub = __subsf3;
  pFdiv = divsf3;
  pFmul = __mulsf3;
  pFmodf = fmodf;
  pDadd = __adddf3;
  pDsub = __subdf3;
  pDdiv = __divdf3;
  pDmul = muldf3;
  pFmod = fmod;
  pCmpgFloat = CmpgFloat;
  pCmplFloat = CmplFloat;
  pCmpgDouble = CmpgDouble;
  pCmplDouble = CmplDouble;
#endif
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
  pLadd = NULL;
  pLsub = NULL;
  pLand = NULL;
  pLor = NULL;
  pLxor = NULL;
  pLdivmod = __aeabi_ldivmod;
  pLmul = __aeabi_lmul;
  pAllocArrayFromCode = art_alloc_array_from_code;
  pAllocArrayFromCodeWithAccessCheck = art_alloc_array_from_code_with_access_check;
  pAllocObjectFromCode = art_alloc_object_from_code;
  pAllocObjectFromCodeWithAccessCheck = art_alloc_object_from_code_with_access_check;
  pCanPutArrayElementFromCode = art_can_put_array_element_from_code;
  pCheckAndAllocArrayFromCode = art_check_and_alloc_array_from_code;
  pCheckAndAllocArrayFromCodeWithAccessCheck = art_check_and_alloc_array_from_code_with_access_check;
  pCheckCastFromCode = art_check_cast_from_code;
  pGet32Instance = art_get32_instance_from_code;
  pGet64Instance = art_get64_instance_from_code;
  pGetObjInstance = art_get_obj_instance_from_code;
  pGet32Static = art_get32_static_from_code;
  pGet64Static = art_get64_static_from_code;
  pGetObjStatic = art_get_obj_static_from_code;
  pHandleFillArrayDataFromCode = art_handle_fill_data_from_code;
  pInitializeStaticStorage = art_initialize_static_storage_from_code;
  pInitializeTypeFromCode = art_initialize_type_from_code;
  pInitializeTypeAndVerifyAccessFromCode = art_initialize_type_and_verify_access_from_code;
  pInvokeDirectTrampolineWithAccessCheck = art_invoke_direct_trampoline_with_access_check;
  pInvokeInterfaceTrampoline = art_invoke_interface_trampoline;
  pInvokeInterfaceTrampolineWithAccessCheck = art_invoke_interface_trampoline_with_access_check;
  pInvokeStaticTrampolineWithAccessCheck = art_invoke_static_trampoline_with_access_check;
  pInvokeSuperTrampolineWithAccessCheck = art_invoke_super_trampoline_with_access_check;
  pInvokeVirtualTrampolineWithAccessCheck = art_invoke_virtual_trampoline_with_access_check;
  pLockObjectFromCode = art_lock_object_from_code;
  pResolveStringFromCode = art_resolve_string_from_code;
  pSet32Instance = art_set32_instance_from_code;
  pSet64Instance = art_set64_instance_from_code;
  pSetObjInstance = art_set_obj_instance_from_code;
  pSet32Static = art_set32_static_from_code;
  pSet64Static = art_set64_static_from_code;
  pSetObjStatic = art_set_obj_static_from_code;
  pTestSuspendFromCode = art_test_suspend;
  pThrowArrayBoundsFromCode = art_throw_array_bounds_from_code;
  pThrowDivZeroFromCode = art_throw_div_zero_from_code;
  pThrowNegArraySizeFromCode = art_throw_neg_array_size_from_code;
  pThrowNoSuchMethodFromCode = art_throw_no_such_method_from_code;
  pThrowNullPointerFromCode = art_throw_null_pointer_exception_from_code;
  pThrowStackOverflowFromCode = art_throw_stack_overflow_from_code;
  pThrowVerificationErrorFromCode = art_throw_verification_error_from_code;
  pUnlockObjectFromCode = art_unlock_object_from_code;
  pUpdateDebuggerFromCode = NULL;  // To enable, set to art_update_debugger
#endif
#if defined(__i386__)
  pShlLong = NULL;
  pShrLong = NULL;
  pUshrLong = NULL;
  pIdiv = NULL;
  pIdivmod = NULL;
  pI2f = NULL;
  pF2iz = NULL;
  pD2f = NULL;
  pF2d = NULL;
  pD2iz = NULL;
  pL2f = NULL;
  pL2d = NULL;
  pFadd = NULL;
  pFsub = NULL;
  pFdiv = NULL;
  pFmul = NULL;
  pFmodf = NULL;
  pDadd = NULL;
  pDsub = NULL;
  pDdiv = NULL;
  pDmul = NULL;
  pFmod = NULL;
  pLadd = NULL;
  pLsub = NULL;
  pLand = NULL;
  pLor = NULL;
  pLxor = NULL;
  pLdivmod = NULL;
  pLmul = NULL;
  pAllocArrayFromCode = NULL;
  pAllocArrayFromCodeWithAccessCheck = NULL;
  pAllocObjectFromCode = NULL;
  pAllocObjectFromCodeWithAccessCheck = NULL;
  pCanPutArrayElementFromCode = NULL;
  pCheckAndAllocArrayFromCode = NULL;
  pCheckAndAllocArrayFromCodeWithAccessCheck = NULL;
  pCheckCastFromCode = NULL;
  pGet32Instance = NULL;
  pGet64Instance = NULL;
  pGetObjInstance = NULL;
  pGet32Static = NULL;
  pGet64Static = NULL;
  pGetObjStatic = NULL;
  pHandleFillArrayDataFromCode = NULL;
  pInitializeStaticStorage = NULL;
  pInitializeTypeFromCode = NULL;
  pInitializeTypeAndVerifyAccessFromCode = NULL;
  pInvokeDirectTrampolineWithAccessCheck = NULL;
  pInvokeInterfaceTrampoline = NULL;
  pInvokeInterfaceTrampolineWithAccessCheck = NULL;
  pInvokeStaticTrampolineWithAccessCheck = NULL;
  pInvokeSuperTrampolineWithAccessCheck = NULL;
  pInvokeVirtualTrampolineWithAccessCheck = NULL;
  pLockObjectFromCode = NULL;
  pResolveStringFromCode = NULL;
  pSet32Instance = NULL;
  pSet64Instance = NULL;
  pSetObjInstance = NULL;
  pSet32Static = NULL;
  pSet64Static = NULL;
  pSetObjStatic = NULL;
  pTestSuspendFromCode = NULL;
  pThrowArrayBoundsFromCode = NULL;
  pThrowDivZeroFromCode = NULL;
  pThrowNegArraySizeFromCode = NULL;
  pThrowNoSuchMethodFromCode = NULL;
  pThrowNullPointerFromCode = NULL;
  pThrowStackOverflowFromCode = NULL;
  pThrowVerificationErrorFromCode = NULL;
  pUnlockObjectFromCode = NULL;
  pUpdateDebuggerFromCode = NULL;  // To enable, set to art_update_debugger
#endif
  pF2l = F2L;
  pD2l = D2L;
  pMemcpy = memcpy;
  pCheckSuspendFromCode = CheckSuspendFromCode;
  pDebugMe = DebugMe;
  pDecodeJObjectInThread = DecodeJObjectInThread;
  pDeliverException = art_deliver_exception_from_code;
  pFindNativeMethod = FindNativeMethod;
  pInstanceofNonTrivialFromCode = IsAssignableFromCode;
  pThrowAbstractMethodErrorFromCode = ThrowAbstractMethodErrorFromCode;
  pUnresolvedDirectMethodTrampolineFromCode = UnresolvedDirectMethodTrampolineFromCode;
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
  reinterpret_cast<bionic_pthread_internal_t*>(pthread_self())->kernel_id = tid_;
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
    CHECK_EQ(self->GetState(), Thread::kRunnable);
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

void SetVmData(Object* managed_thread, Thread* native_thread) {
  gThread_vmData->SetInt(managed_thread, reinterpret_cast<uintptr_t>(native_thread));
}

Thread* Thread::FromManagedThread(Object* thread_peer) {
  return reinterpret_cast<Thread*>(static_cast<uintptr_t>(gThread_vmData->GetInt(thread_peer)));
}

Thread* Thread::FromManagedThread(JNIEnv* env, jobject java_thread) {
  return FromManagedThread(Decode<Object*>(env, java_thread));
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

  {
    ScopedThreadStateChange tsc(Thread::Current(), Thread::kVmWait);
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

  self->SetState(Thread::kNative);

  // If we're the main thread, ClassLinker won't be created until after we're attached,
  // so that thread needs a two-stage attach. Regular threads don't need this hack.
  // In the compiler, all threads need this hack, because no-one's going to be getting
  // a native peer!
  if (self->thin_lock_id_ != ThreadList::kMainId && !Runtime::Current()->IsCompiler()) {
    self->CreatePeer(thread_name, as_daemon, thread_group);
  } else {
    // These aren't necessary, but they improve diagnostics for unit tests & command-line tools.
    self->name_->assign(thread_name);
    ::art::SetThreadName(thread_name);
  }

  self->GetJniEnv()->locals.AssertEmpty();
  return self;
}

Object* Thread::GetMainThreadGroup() {
  if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(gThreadGroup, true)) {
    return NULL;
  }
  return gThreadGroup_mMain->GetObject(NULL);
}

Object* Thread::GetSystemThreadGroup() {
  if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(gThreadGroup, true)) {
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
#if defined(__APPLE__)
  // Only needed to run code. Try implementing this with pthread_get_stacksize_np and pthread_get_stackaddr_np.
  UNIMPLEMENTED(WARNING);
#else
  pthread_attr_t attributes;
  CHECK_PTHREAD_CALL(pthread_getattr_np, (pthread_self(), &attributes), __FUNCTION__);

  void* temp_stack_base;
  CHECK_PTHREAD_CALL(pthread_attr_getstack, (&attributes, &temp_stack_base, &stack_size_),
                     __FUNCTION__);
  stack_begin_ = reinterpret_cast<byte*>(temp_stack_base);

  if (stack_size_ <= kStackOverflowReservedBytes) {
    LOG(FATAL) << "Attempt to attach a thread with a too-small stack (" << stack_size_ << " bytes)";
  }

  // Set stack_end_ to the bottom of the stack saving space of stack overflows
  ResetDefaultStackEnd();

  // Sanity check.
  int stack_variable;
  CHECK_GT(&stack_variable, (void*) stack_end_);

  CHECK_PTHREAD_CALL(pthread_attr_destroy, (&attributes), __FUNCTION__);
#endif
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
  if (ReadFileToString(StringPrintf("/proc/self/task/%d/schedstat", GetTid()).c_str(), &scheduler_stats)) {
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
}

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

void Thread::PopNativeToManagedRecord(const NativeToManagedRecord& record) {
  native_to_managed_record_ = record.link_;
  top_of_managed_stack_.SetSP(reinterpret_cast<Method**>(record.last_top_of_managed_stack_));
  top_of_managed_stack_pc_ = record.last_top_of_managed_stack_pc_;
}

struct StackDumpVisitor : public Thread::StackVisitor {
  StackDumpVisitor(std::ostream& os, const Thread* thread)
      : last_method(NULL), last_line_number(0), repetition_count(0), os(os), thread(thread),
        frame_count(0) {
  }

  virtual ~StackDumpVisitor() {
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
  if (GetState() == Thread::kNative || GetState() == Thread::kVmWait) {
    DumpNativeStack(os);
  }
  StackDumpVisitor dumper(os, this);
  WalkStack(&dumper);
}

void Thread::SetStateWithoutSuspendCheck(Thread::State new_state) {
  volatile void* raw = reinterpret_cast<volatile void*>(&state_);
  volatile int32_t* addr = reinterpret_cast<volatile int32_t*>(raw);
  android_atomic_release_store(new_state, addr);
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

bool Thread::IsSuspended() {
  ANNOTATE_IGNORE_READS_BEGIN();
  int suspend_count = suspend_count_;
  ANNOTATE_IGNORE_READS_END();
  return suspend_count != 0 && GetState() != Thread::kRunnable;
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
  while (GetState() == Thread::kRunnable) {
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
Field* FindFieldOrDie(Class* c, const char* name, const char* descriptor) {
  Field* f = c->FindDeclaredInstanceField(name, descriptor);
  CHECK(f != NULL) << PrettyClass(c) << " " << name << " " << descriptor;
  return f;
}

// TODO: make more accessible?
Method* FindMethodOrDie(Class* c, const char* name, const char* signature) {
  Method* m = c->FindVirtualMethod(name, signature);
  CHECK(m != NULL) << PrettyClass(c) << " " << name << " " << signature;
  return m;
}

// TODO: make more accessible?
Field* FindStaticFieldOrDie(Class* c, const char* name, const char* descriptor) {
  Field* f = c->FindDeclaredStaticField(name, descriptor);
  CHECK(f != NULL) << PrettyClass(c) << " " << name << " " << descriptor;
  return f;
}

void Thread::FinishStartup() {
  CHECK(Runtime::Current()->IsStarted());
  Thread* self = Thread::Current();

  // Need to be kRunnable for FindClass
  ScopedThreadStateChange tsc(self, Thread::kRunnable);

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
      top_shadow_frame_(NULL),
      jni_env_(NULL),
      state_(Thread::kNative),
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
      name_(new std::string("<native thread without managed peer>")) {
  CHECK_EQ((sizeof(Thread) % 4), 0U) << sizeof(Thread);
  memset(&held_mutexes_[0], 0, sizeof(held_mutexes_));
}

void MonitorExitVisitor(const Object* object, void*) {
  Object* entered_monitor = const_cast<Object*>(object);
  entered_monitor->MonitorExit(Thread::Current());
}

Thread::~Thread() {
  // On thread detach, all monitors entered with JNI MonitorEnter are automatically exited.
  if (jni_env_ != NULL) {
    jni_env_->monitors.VisitRoots(MonitorExitVisitor, NULL);
  }

  if (peer_ != NULL) {

    // this.vmData = 0;
    SetVmData(peer_, NULL);

    Dbg::PostThreadDeath(this);

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

  delete debug_invoke_req_;
  delete trace_stack_;
  delete name_;
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
  args[0].l = peer_;
  args[1].l = exception;
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
    args[0].l = peer_;
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
#if defined(__arm__)
  uint32_t trace_stack_depth = 0;
#endif
  // TODO: enable this CHECK after native_to_managed_record_ is initialized during startup.
  // CHECK(native_to_managed_record_ != NULL);
  NativeToManagedRecord* record = native_to_managed_record_;

  while (frame.GetSP() != NULL) {
    for ( ; frame.GetMethod() != NULL; frame.Next()) {
      frame.GetMethod()->AssertPcIsWithinCode(pc);
      bool should_continue = visitor->VisitFrame(frame, pc);
      if (!should_continue) {
        return;
      }
      pc = ManglePc(frame.GetReturnPC());
      if (Runtime::Current()->IsMethodTracingActive()) {
#if defined(__arm__)
        uintptr_t trace_exit = reinterpret_cast<uintptr_t>(art_trace_exit_from_code);
        if (ManglePc(trace_exit) == pc) {
          TraceStackFrame trace_frame = GetTraceStackFrame(trace_stack_depth++);
          CHECK(trace_frame.method_ == frame.GetMethod());
          pc = ManglePc(trace_frame.return_pc_);
        }
#endif
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
    bool should_continue = visitor->VisitFrame(frame, cur->GetLineNumber());
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
#if !defined(ART_USE_LLVM_COMPILER)
    int32_t line_number = mh.GetLineNumFromNativePC(native_pc);
#else
    int32_t line_number = native_pc; // LLVM stored line_number in the ShadowFrame
#endif
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

void Thread::ThrowOutOfMemoryError(Class* c, size_t byte_count) {
  std::string msg(StringPrintf("Failed to allocate a %zd-byte %s", byte_count,
      PrettyDescriptor(c).c_str()));
  ThrowOutOfMemoryError(msg.c_str());
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

class CatchBlockStackVisitor : public Thread::StackVisitor {
 public:
  CatchBlockStackVisitor(Class* to_find, Context* ljc)
      : to_find_(to_find), long_jump_context_(ljc), native_method_count_(0) {
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
      return false;
    }
    uint32_t dex_pc = DexFile::kDexNoIndex;
    if (method->IsCalleeSaveMethod()) {
      // ignore callee save method
    } else if (method->IsNative()) {
      native_method_count_++;
    } else {
      // Unwind stack during method tracing
      if (Runtime::Current()->IsMethodTracingActive()) {
#if defined(__arm__)
        uintptr_t trace_exit = reinterpret_cast<uintptr_t>(art_trace_exit_from_code);
        if (ManglePc(trace_exit) == pc) {
          pc = ManglePc(TraceMethodUnwindFromCode(Thread::Current()));
        }
#else
        UNIMPLEMENTED(WARNING);
#endif
      }
      dex_pc = method->ToDexPC(pc);
    }
    if (dex_pc != DexFile::kDexNoIndex) {
      uint32_t found_dex_pc = method->FindCatchBlock(to_find_, dex_pc);
      if (found_dex_pc != DexFile::kDexNoIndex) {
        handler_pc_ = method->ToNativePC(found_dex_pc);
        handler_frame_ = fr;
        return false;
      }
    }
    // Caller may be handler, fill in callee saves in context
    long_jump_context_->FillCalleeSaves(fr);
    return true;
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
};

void Thread::DeliverException() {
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
  long_jump_context->DoLongJump();
  LOG(FATAL) << "UNREACHABLE";
}

Context* Thread::GetLongJumpContext() {
  Context* result = long_jump_context_;
  if (result == NULL) {
    result = Context::Create();
    long_jump_context_ = result;
  }
  return result;
}

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

bool Thread::HoldsLock(Object* object) {
  if (object == NULL) {
    return false;
  }
  return object->GetThinLockId() == thin_lock_id_;
}

bool Thread::IsDaemon() {
  return gThread_daemon->GetBoolean(peer_);
}

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

  // Cheat and steal the long jump context. Assume that we are not doing a GC during exception
  // delivery.
  Context* context = GetLongJumpContext();
  // Visit roots on this thread's stack
  ReferenceMapVisitor mapper(context, visitor, arg);
  WalkStack(&mapper);
}

#if VERIFY_OBJECT_ENABLED
void VerifyObject(const Object* obj, void*) {
  Runtime::Current()->GetHeap()->VerifyObject(obj);
}

void Thread::VerifyStack() {
  UniquePtr<Context> context(Context::Create());
  ReferenceMapVisitor mapper(context.get(), VerifyObject, NULL);
  WalkStack(&mapper);
}
#endif

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
  thread.Dump(os, false);
  return os;
}

void Thread::CheckSafeToLockOrUnlock(MutexRank rank, bool is_locking) {
  if (is_locking) {
    if (held_mutexes_[rank] == 0) {
      bool bad_mutexes_held = false;
      for (int i = kMaxMutexRank; i > rank; --i) {
        if (held_mutexes_[i] != 0) {
          LOG(ERROR) << "holding " << static_cast<MutexRank>(i) << " while " << (is_locking ? "locking" : "unlocking") << " " << rank;
          bad_mutexes_held = true;
        }
      }
      CHECK(!bad_mutexes_held);
    }
    ++held_mutexes_[rank];
  } else {
    CHECK_GT(held_mutexes_[rank], 0U);
    --held_mutexes_[rank];
  }
}

void Thread::CheckSafeToWait(MutexRank rank) {
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
