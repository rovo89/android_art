// Copyright 2011 Google Inc. All Rights Reserved.

#include "thread.h"

#include <pthread.h>
#include <sys/mman.h>

#include <algorithm>
#include <bitset>
#include <cerrno>
#include <iostream>
#include <list>

#include "class_linker.h"
#include "heap.h"
#include "jni_internal.h"
#include "object.h"
#include "runtime.h"
#include "runtime_support.h"
#include "scoped_jni_thread_state.h"
#include "thread_list.h"
#include "utils.h"

namespace art {

pthread_key_t Thread::pthread_key_self_;

// Temporary debugging hook for compiler.
void DebugMe(Method* method, uint32_t info) {
    LOG(INFO) << "DebugMe";
    if (method != NULL)
        LOG(INFO) << PrettyMethod(method);
    LOG(INFO) << "Info: " << info;
}

/*
 * TODO: placeholder for a method that can be called by the
 * invoke-interface trampoline to unwind and handle exception.  The
 * trampoline will arrange it so that the caller appears to be the
 * callsite of the failed invoke-interface.  See comments in
 * compiler/runtime_support.S
 */
extern "C" void artFailedInvokeInterface() {
    UNIMPLEMENTED(FATAL) << "Unimplemented exception throw";
}

// TODO: placeholder.  See comments in compiler/runtime_support.S
extern "C" uint64_t artFindInterfaceMethodInCache(uint32_t method_idx,
     Object* this_object , Method* caller_method)
{
    /*
     * Note: this_object has not yet been null-checked.  To match
     * the old-world state, nullcheck this_object and load
     * Class* this_class = this_object->GetClass().
     * See comments and possible thrown exceptions in old-world
     * Interp.cpp:dvmInterpFindInterfaceMethod, and complete with
     * new-world FindVirtualMethodForInterface.
     */
    UNIMPLEMENTED(FATAL) << "Unimplemented invoke interface";
    return 0LL;
}

// TODO: placeholder.  This is what generated code will call to throw
void ThrowException(Thread* thread, Throwable* exception) {
  /*
   * exception may be NULL, in which case this routine should
   * throw NPE.  NOTE: this is a convenience for generated code,
   * which previously did the null check inline and constructed
   * and threw a NPE if NULL.  This routine responsible for setting
   * exception_ in thread.
   */
  UNIMPLEMENTED(FATAL) << "Unimplemented exception throw: " << PrettyType(exception);
}

// TODO: placeholder.  Helper function to type
Class* InitializeTypeFromCode(uint32_t type_idx, Method* method) {
  /*
   * Should initialize & fix up method->dex_cache_resolved_types_[].
   * Returns initialized type.  Does not return normally if an exception
   * is thrown, but instead initiates the catch.  Should be similar to
   * ClassLinker::InitializeStaticStorageFromCode.
   */
  UNIMPLEMENTED(FATAL);
  return NULL;
}

// TODO: placeholder.  Helper function to resolve virtual method
void ResolveMethodFromCode(Method* method, uint32_t method_idx) {
    /*
     * Slow-path handler on invoke virtual method path in which
     * base method is unresolved at compile-time.  Doesn't need to
     * return anything - just either ensure that
     * method->dex_cache_resolved_methods_(method_idx) != NULL or
     * throw and unwind.  The caller will restart call sequence
     * from the beginning.
     */
}

// TODO: placeholder.  Helper function to alloc array for OP_FILLED_NEW_ARRAY
Array* CheckAndAllocFromCode(uint32_t type_index, Method* method, int32_t component_count) {
    /*
     * Just a wrapper around Array::AllocFromCode() that additionally
     * throws a runtime exception "bad Filled array req" for 'D' and 'J'.
     */
    UNIMPLEMENTED(WARNING) << "Need check that not 'D' or 'J'";
    return Array::AllocFromCode(type_index, method, component_count);
}

// TODO: placeholder (throw on failure)
void CheckCastFromCode(const Class* a, const Class* b) {
    if (a->IsAssignableFrom(b)) {
        return;
    }
    UNIMPLEMENTED(FATAL);
}

// TODO: placeholder
void UnlockObjectFromCode(Thread* thread, Object* obj) {
    // TODO: throw and unwind if lock not held
    // TODO: throw and unwind on NPE
    obj->MonitorExit(thread);
}

// TODO: placeholder
void LockObjectFromCode(Thread* thread, Object* obj) {
    obj->MonitorEnter(thread);
}

// TODO: placeholder
void CheckSuspendFromCode(Thread* thread) {
    /*
     * Code is at a safe point, suspend if needed.
     * Also, this is where a pending safepoint callback
     * would be fired.
     */
}

// TODO: placeholder
void StackOverflowFromCode(Method* method) {
  //NOTE: to save code space, this handler needs to look up its own Thread*
  UNIMPLEMENTED(FATAL) << "Stack overflow: " << PrettyMethod(method);
}

// TODO: placeholder
void ThrowNullPointerFromCode() {
  Thread::Current()->Dump(std::cerr);
  //NOTE: to save code space, this handler must look up caller's Method*
  UNIMPLEMENTED(FATAL) << "Null pointer exception";
}

// TODO: placeholder
void ThrowDivZeroFromCode() {
  UNIMPLEMENTED(FATAL) << "Divide by zero";
}

// TODO: placeholder
void ThrowArrayBoundsFromCode(int32_t index, int32_t limit) {
  UNIMPLEMENTED(FATAL) << "Bound check exception, idx: " << index << ", limit: " << limit;
}

// TODO: placeholder
void ThrowVerificationErrorFromCode(int32_t src1, int32_t ref) {
    UNIMPLEMENTED(FATAL) << "Verification error, src1: " << src1 <<
        " ref: " << ref;
}

// TODO: placeholder
void ThrowNegArraySizeFromCode(int32_t index) {
    UNIMPLEMENTED(FATAL) << "Negative array size: " << index;
}

// TODO: placeholder
void ThrowInternalErrorFromCode(int32_t errnum) {
    UNIMPLEMENTED(FATAL) << "Internal error: " << errnum;
}

// TODO: placeholder
void ThrowRuntimeExceptionFromCode(int32_t errnum) {
    UNIMPLEMENTED(FATAL) << "Internal error: " << errnum;
}

// TODO: placeholder
void ThrowNoSuchMethodFromCode(int32_t method_idx) {
    UNIMPLEMENTED(FATAL) << "No such method, idx: " << method_idx;
}

/*
 * Temporary placeholder.  Should include run-time checks for size
 * of fill data <= size of array.  If not, throw arrayOutOfBoundsException.
 * As with other new "FromCode" routines, this should return to the caller
 * only if no exception has been thrown.
 *
 * NOTE: When dealing with a raw dex file, the data to be copied uses
 * little-endian ordering.  Require that oat2dex do any required swapping
 * so this routine can get by with a memcpy().
 *
 * Format of the data:
 *  ushort ident = 0x0300   magic value
 *  ushort width            width of each element in the table
 *  uint   size             number of elements in the table
 *  ubyte  data[size*width] table of data values (may contain a single-byte
 *                          padding at the end)
 */
void HandleFillArrayDataFromCode(Array* array, const uint16_t* table) {
    uint32_t size = (uint32_t)table[2] | (((uint32_t)table[3]) << 16);
    uint32_t size_in_bytes = size * table[1];
    if (static_cast<int32_t>(size) > array->GetLength()) {
      ThrowArrayBoundsFromCode(array->GetLength(), size);
    }
    memcpy((char*)array + art::Array::DataOffset().Int32Value(),
           (char*)&table[4], size_in_bytes);
}

// TODO: move to more appropriate location
/*
 * Float/double conversion requires clamping to min and max of integer form.  If
 * target doesn't support this normally, use these.
 */
int64_t D2L(double d) {
    static const double kMaxLong = (double)(int64_t)0x7fffffffffffffffULL;
    static const double kMinLong = (double)(int64_t)0x8000000000000000ULL;
    if (d >= kMaxLong)
        return (int64_t)0x7fffffffffffffffULL;
    else if (d <= kMinLong)
        return (int64_t)0x8000000000000000ULL;
    else if (d != d) // NaN case
        return 0;
    else
        return (int64_t)d;
}

int64_t F2L(float f) {
    static const float kMaxLong = (float)(int64_t)0x7fffffffffffffffULL;
    static const float kMinLong = (float)(int64_t)0x8000000000000000ULL;
    if (f >= kMaxLong)
        return (int64_t)0x7fffffffffffffffULL;
    else if (f <= kMinLong)
        return (int64_t)0x8000000000000000ULL;
    else if (f != f) // NaN case
        return 0;
    else
        return (int64_t)f;
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
  pInvokeInterfaceTrampoline = art_invoke_interface_trampoline;
#endif
  pF2l = F2L;
  pD2l = D2L;
  pAllocFromCode = Array::AllocFromCode;
  pCheckAndAllocFromCode = CheckAndAllocFromCode;
  pAllocObjectFromCode = Class::AllocObjectFromCode;
  pMemcpy = memcpy;
  pHandleFillArrayDataFromCode = HandleFillArrayDataFromCode;
  pGet32Static = Field::Get32StaticFromCode;
  pSet32Static = Field::Set32StaticFromCode;
  pGet64Static = Field::Get64StaticFromCode;
  pSet64Static = Field::Set64StaticFromCode;
  pGetObjStatic = Field::GetObjStaticFromCode;
  pSetObjStatic = Field::SetObjStaticFromCode;
  pCanPutArrayElementFromCode = Class::CanPutArrayElementFromCode;
  pThrowException = ThrowException;
  pInitializeTypeFromCode = InitializeTypeFromCode;
  pResolveMethodFromCode = ResolveMethodFromCode;
  pInitializeStaticStorage = ClassLinker::InitializeStaticStorageFromCode;
  pInstanceofNonTrivialFromCode = Object::InstanceOf;
  pCheckCastFromCode = CheckCastFromCode;
  pLockObjectFromCode = LockObjectFromCode;
  pUnlockObjectFromCode = UnlockObjectFromCode;
  pFindFieldFromCode = Field::FindFieldFromCode;
  pCheckSuspendFromCode = CheckSuspendFromCode;
  pStackOverflowFromCode = StackOverflowFromCode;
  pThrowNullPointerFromCode = ThrowNullPointerFromCode;
  pThrowArrayBoundsFromCode = ThrowArrayBoundsFromCode;
  pThrowDivZeroFromCode = ThrowDivZeroFromCode;
  pThrowVerificationErrorFromCode = ThrowVerificationErrorFromCode;
  pThrowNegArraySizeFromCode = ThrowNegArraySizeFromCode;
  pThrowRuntimeExceptionFromCode = ThrowRuntimeExceptionFromCode;
  pThrowInternalErrorFromCode = ThrowInternalErrorFromCode;
  pThrowNoSuchMethodFromCode = ThrowNoSuchMethodFromCode;
  pDebugMe = DebugMe;
}

void Frame::Next() {
  byte* next_sp = reinterpret_cast<byte*>(sp_) +
      GetMethod()->GetFrameSizeInBytes();
  sp_ = reinterpret_cast<Method**>(next_sp);
}

uintptr_t Frame::GetPC() const {
  byte* pc_addr = reinterpret_cast<byte*>(sp_) +
      GetMethod()->GetReturnPcOffsetInBytes();
  return *reinterpret_cast<uintptr_t*>(pc_addr);
}

Method* Frame::NextMethod() const {
  byte* next_sp = reinterpret_cast<byte*>(sp_) +
      GetMethod()->GetFrameSizeInBytes();
  return *reinterpret_cast<Method**>(next_sp);
}

void* Thread::CreateCallback(void *arg) {
  Thread* self = reinterpret_cast<Thread*>(arg);
  Runtime* runtime = Runtime::Current();

  self->Attach(runtime);

  ClassLinker* class_linker = runtime->GetClassLinker();

  Class* thread_class = class_linker->FindSystemClass("Ljava/lang/Thread;");
  Class* string_class = class_linker->FindSystemClass("Ljava/lang/String;");

  Field* name_field = thread_class->FindDeclaredInstanceField("name", string_class);
  String* thread_name = reinterpret_cast<String*>(name_field->GetObject(self->peer_));
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
  Method* Thread_run = thread_class->FindVirtualMethod("run", "()V");
  Method* m = receiver->GetClass()->FindVirtualMethodForVirtualOrInterface(Thread_run);
  m->Invoke(self, receiver, NULL, NULL);

  // Detach.
  runtime->GetThreadList()->Unregister();

  return NULL;
}

void SetVmData(Object* managed_thread, Thread* native_thread) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();

  Class* thread_class = class_linker->FindSystemClass("Ljava/lang/Thread;");
  Class* int_class = class_linker->FindPrimitiveClass('I');

  Field* vmData_field = thread_class->FindDeclaredInstanceField("vmData", int_class);

  vmData_field->SetInt(managed_thread, reinterpret_cast<uintptr_t>(native_thread));
}

void Thread::Create(Object* peer, size_t stack_size) {
  CHECK(peer != NULL);

  if (stack_size == 0) {
    stack_size = Runtime::Current()->GetDefaultStackSize();
  }

  Thread* native_thread = new Thread;
  native_thread->peer_ = peer;

  // Thread.start is synchronized, so we know that vmData is 0,
  // and know that we're not racing to assign it.
  SetVmData(peer, native_thread);

  pthread_attr_t attr;
  errno = pthread_attr_init(&attr);
  if (errno != 0) {
    PLOG(FATAL) << "pthread_attr_init failed";
  }

  errno = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (errno != 0) {
    PLOG(FATAL) << "pthread_attr_setdetachstate(PTHREAD_CREATE_DETACHED) failed";
  }

  errno = pthread_attr_setstacksize(&attr, stack_size);
  if (errno != 0) {
    PLOG(FATAL) << "pthread_attr_setstacksize(" << stack_size << ") failed";
  }

  errno = pthread_create(&native_thread->pthread_, &attr, Thread::CreateCallback, native_thread);
  if (errno != 0) {
    PLOG(FATAL) << "pthread_create failed";
  }

  errno = pthread_attr_destroy(&attr);
  if (errno != 0) {
    PLOG(FATAL) << "pthread_attr_destroy failed";
  }

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

  errno = pthread_setspecific(Thread::pthread_key_self_, this);
  if (errno != 0) {
    PLOG(FATAL) << "pthread_setspecific failed";
  }

  jni_env_ = new JNIEnvExt(this, runtime->GetJavaVM());

  runtime->GetThreadList()->Register(this);
}

Thread* Thread::Attach(const Runtime* runtime, const char* name, bool as_daemon) {
  Thread* self = new Thread;
  self->Attach(runtime);

  self->SetState(Thread::kRunnable);

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
  ScopedThreadStateChange tsc(Thread::Current(), Thread::kNative);

  JNIEnv* env = jni_env_;

  const char* field_name = (GetThinLockId() == ThreadList::kMainId) ? "mMain" : "mSystem";
  jobject thread_group = GetWellKnownThreadGroup(env, field_name);
  jobject thread_name = env->NewStringUTF(name);
  jint thread_priority = GetNativePriority();
  jboolean thread_is_daemon = as_daemon;

  jclass c = env->FindClass("java/lang/Thread");
  jmethodID mid = env->GetMethodID(c, "<init>", "(Ljava/lang/ThreadGroup;Ljava/lang/String;IZ)V");

  jobject peer = env->NewObject(c, mid, thread_group, thread_name, thread_priority, thread_is_daemon);

  // Because we mostly run without code available (in the compiler, in tests), we
  // manually assign the fields the constructor should have set.
  // TODO: lose this.
  jfieldID fid;
  fid = env->GetFieldID(c, "group", "Ljava/lang/ThreadGroup;");
  env->SetObjectField(peer, fid, thread_group);
  fid = env->GetFieldID(c, "name", "Ljava/lang/String;");
  env->SetObjectField(peer, fid, thread_name);
  fid = env->GetFieldID(c, "priority", "I");
  env->SetIntField(peer, fid, thread_priority);
  fid = env->GetFieldID(c, "daemon", "Z");
  env->SetBooleanField(peer, fid, thread_is_daemon);

  peer_ = DecodeJObject(peer);
}

void Thread::InitStackHwm() {
  pthread_attr_t attributes;
  errno = pthread_getattr_np(pthread_, &attributes);
  if (errno != 0) {
    PLOG(FATAL) << "pthread_getattr_np failed";
  }

  void* stack_base;
  size_t stack_size;
  errno = pthread_attr_getstack(&attributes, &stack_base, &stack_size);
  if (errno != 0) {
    PLOG(FATAL) << "pthread_attr_getstack failed";
  }

  if (stack_size <= kStackOverflowReservedBytes) {
    LOG(FATAL) << "attempt to attach a thread with a too-small stack (" << stack_size << " bytes)";
  }

  // stack_base is the "lowest addressable byte" of the stack.
  // Our stacks grow down, so we want stack_end_ to be near there, but reserving enough room
  // to throw a StackOverflowError.
  stack_end_ = reinterpret_cast<byte*>(stack_base) + kStackOverflowReservedBytes;

  // Sanity check.
  int stack_variable;
  CHECK_GT(&stack_variable, (void*) stack_end_);

  errno = pthread_attr_destroy(&attributes);
  if (errno != 0) {
    PLOG(FATAL) << "pthread_attr_destroy failed";
  }
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
    ClassLinker* class_linker = Runtime::Current()->GetClassLinker();

    Class* boolean_class = class_linker->FindPrimitiveClass('Z');
    Class* int_class = class_linker->FindPrimitiveClass('I');
    Class* string_class = class_linker->FindSystemClass("Ljava/lang/String;");
    Class* thread_class = class_linker->FindSystemClass("Ljava/lang/Thread;");
    Class* thread_group_class = class_linker->FindSystemClass("Ljava/lang/ThreadGroup;");

    Field* name_field = thread_class->FindDeclaredInstanceField("name", string_class);
    Field* priority_field = thread_class->FindDeclaredInstanceField("priority", int_class);
    Field* daemon_field = thread_class->FindDeclaredInstanceField("daemon", boolean_class);
    Field* thread_group_field = thread_class->FindDeclaredInstanceField("group", thread_group_class);

    String* thread_name_string = reinterpret_cast<String*>(name_field->GetObject(peer_));
    thread_name = (thread_name_string != NULL) ? thread_name_string->ToModifiedUtf8() : "<null>";
    priority = priority_field->GetInt(peer_);
    is_daemon = daemon_field->GetBoolean(peer_);

    Object* thread_group = thread_group_field->GetObject(peer_);
    if (thread_group != NULL) {
      Field* name_field = thread_group_class->FindDeclaredInstanceField("name", string_class);
      String* group_name_string = reinterpret_cast<String*>(name_field->GetObject(thread_group));
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
  errno = pthread_getschedparam(pthread_, &policy, &sp);
  if (errno != 0) {
    PLOG(FATAL) << "pthread_getschedparam failed";
  }

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

  int suspend_count = 0; // TODO
  int debug_suspend_count = 0; // TODO
  os << "  | group=\"" << group_name << "\""
     << " sCount=" << suspend_count
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
  StackDumpVisitor(std::ostream& os) : os(os) {
  }

  ~StackDumpVisitor() {
  }

  void VisitFrame(const Frame& frame) {
    ClassLinker* class_linker = Runtime::Current()->GetClassLinker();

    Method* m = frame.GetMethod();
    Class* c = m->GetDeclaringClass();
    const DexFile& dex_file = class_linker->FindDexFile(c->GetDexCache());

    os << "  at " << PrettyMethod(m, false);
    if (m->IsNative()) {
      os << "(Native method)";
    } else {
      int line_number = dex_file.GetLineNumFromPC(m, m->ToDexPC(frame.GetPC()));
      os << "(" << c->GetSourceFile()->ToModifiedUtf8() << ":" << line_number << ")";
    }
    os << "\n";
  }

  std::ostream& os;
};

void Thread::DumpStack(std::ostream& os) const {
  StackDumpVisitor dumper(os);
  WalkStack(&dumper);
}

void Thread::ThreadExitCallback(void* arg) {
  Thread* self = reinterpret_cast<Thread*>(arg);
  LOG(FATAL) << "Native thread exited without calling DetachCurrentThread: " << *self;
}

void Thread::Startup() {
  // Allocate a TLS slot.
  errno = pthread_key_create(&Thread::pthread_key_self_, Thread::ThreadExitCallback);
  if (errno != 0) {
    PLOG(FATAL) << "pthread_key_create failed";
  }

  // Double-check the TLS slot allocation.
  if (pthread_getspecific(pthread_key_self_) != NULL) {
    LOG(FATAL) << "newly-created pthread TLS slot is not NULL";
  }

  // TODO: initialize other locks and condition variables
}

void Thread::Shutdown() {
  errno = pthread_key_delete(Thread::pthread_key_self_);
  if (errno != 0) {
    PLOG(WARNING) << "pthread_key_delete failed";
  }
}

Thread::Thread()
    : peer_(NULL),
      wait_mutex_("Thread wait mutex"),
      wait_monitor_(NULL),
      interrupted_(false),
      stack_end_(NULL),
      top_of_managed_stack_(),
      native_to_managed_record_(NULL),
      top_sirt_(NULL),
      jni_env_(NULL),
      state_(Thread::kUnknown),
      exception_(NULL),
      suspend_count_(0),
      class_loader_override_(NULL) {
}

void MonitorExitVisitor(const Object* object, void*) {
  Object* entered_monitor = const_cast<Object*>(object);
  entered_monitor->MonitorExit();
}

Thread::~Thread() {
  // TODO: check we're not calling the JNI DetachCurrentThread function from
  // a call stack that includes managed frames. (It's only valid if the stack is all-native.)

  // On thread detach, all monitors entered with JNI MonitorEnter are automatically exited.
  if (jni_env_ != NULL) {
    jni_env_->monitors.VisitRoots(MonitorExitVisitor, NULL);
  }

  if (IsExceptionPending()) {
    UNIMPLEMENTED(FATAL) << "threadExitUncaughtException()";
  }

  // TODO: ThreadGroup.removeThread(this);

  if (peer_ != NULL) {
    SetVmData(peer_, NULL);
  }

  // TODO: say "bye" to the debugger.
  //if (gDvm.debuggerConnected) {
  //  dvmDbgPostThreadDeath(self);
  //}

  // Thread.join() is implemented as an Object.wait() on the Thread.lock
  // object. Signal anyone who is waiting.
  //Object* lock = dvmGetFieldObject(self->threadObj, gDvm.offJavaLangThread_lock);
  //dvmLockObject(self, lock);
  //dvmObjectNotifyAll(self, lock);
  //dvmUnlockObject(self, lock);
  //lock = NULL;

  delete jni_env_;
  jni_env_ = NULL;

  SetState(Thread::kTerminated);
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
  CountStackDepthVisitor() : depth_(0) {}

  virtual void VisitFrame(const Frame&) {
    ++depth_;
  }

  int GetDepth() const {
    return depth_;
  }

 private:
  uint32_t depth_;
};

//
class BuildInternalStackTraceVisitor : public Thread::StackVisitor {
 public:
  explicit BuildInternalStackTraceVisitor(int depth, ScopedJniThreadState& ts) : count_(0) {
    // Allocate method trace with an extra slot that will hold the PC trace
    method_trace_ = Runtime::Current()->GetClassLinker()->
        AllocObjectArray<Object>(depth + 1);
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

  virtual void VisitFrame(const Frame& frame) {
    method_trace_->Set(count_, frame.GetMethod());
    pc_trace_->Set(count_, frame.GetPC());
    ++count_;
  }

  jobject GetInternalStackTrace() const {
    return local_ref_;
  }

 private:
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

void Thread::WalkStack(StackVisitor* visitor) const {
  Frame frame = GetTopOfStack();
  // TODO: enable this CHECK after native_to_managed_record_ is initialized during startup.
  // CHECK(native_to_managed_record_ != NULL);
  NativeToManagedRecord* record = native_to_managed_record_;

  while (frame.GetSP()) {
    for ( ; frame.GetMethod() != 0; frame.Next()) {
      visitor->VisitFrame(frame);
    }
    if (record == NULL) {
      break;
    }
    frame.SetSP(reinterpret_cast<art::Method**>(record->last_top_of_managed_stack));  // last_tos should return Frame instead of sp?
    record = record->link;
  }
}

jobject Thread::CreateInternalStackTrace() const {
  // Compute depth of stack
  CountStackDepthVisitor count_visitor;
  WalkStack(&count_visitor);
  int32_t depth = count_visitor.GetDepth();

  // Transition into runnable state to work on Object*/Array*
  ScopedJniThreadState ts(jni_env_);

  // Build internal stack trace
  BuildInternalStackTraceVisitor build_trace_visitor(depth, ts);
  WalkStack(&build_trace_visitor);

  return build_trace_visitor.GetInternalStackTrace();
}

jobjectArray Thread::InternalStackTraceToStackTraceElementArray(jobject internal,
                                                                JNIEnv* env) {
  // Transition into runnable state to work on Object*/Array*
  ScopedJniThreadState ts(env);

  // Decode the internal stack trace into the depth, method trace and PC trace
  ObjectArray<Object>* method_trace =
      down_cast<ObjectArray<Object>*>(Decode<Object*>(ts.Env(), internal));
  int32_t depth = method_trace->GetLength()-1;
  IntArray* pc_trace = down_cast<IntArray*>(method_trace->Get(depth));

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();

  // Create java_trace array and place in local reference table
  ObjectArray<StackTraceElement>* java_traces =
      class_linker->AllocStackTraceElementArray(depth);
  jobjectArray result = AddLocalReference<jobjectArray>(ts.Env(), java_traces);

  for (int32_t i = 0; i < depth; ++i) {
    // Prepare parameters for StackTraceElement(String cls, String method, String file, int line)
    Method* method = down_cast<Method*>(method_trace->Get(i));
    uint32_t native_pc = pc_trace->Get(i);
    Class* klass = method->GetDeclaringClass();
    const DexFile& dex_file = class_linker->FindDexFile(klass->GetDexCache());
    String* readable_descriptor = String::AllocFromModifiedUtf8(
        PrettyDescriptor(klass->GetDescriptor()).c_str());

    // Allocate element, potentially triggering GC
    StackTraceElement* obj =
        StackTraceElement::Alloc(readable_descriptor,
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

void Thread::ThrowNewException(const char* exception_class_descriptor, const char* fmt, ...) {
  std::string msg;
  va_list args;
  va_start(args, fmt);
  StringAppendV(&msg, fmt, args);
  va_end(args);

  // Convert "Ljava/lang/Exception;" into JNI-style "java/lang/Exception".
  CHECK_EQ('L', exception_class_descriptor[0]);
  std::string descriptor(exception_class_descriptor + 1);
  CHECK_EQ(';', descriptor[descriptor.length() - 1]);
  descriptor.erase(descriptor.length() - 1);

  JNIEnv* env = GetJniEnv();
  jclass exception_class = env->FindClass(descriptor.c_str());
  CHECK(exception_class != NULL) << "descriptor=\"" << descriptor << "\"";
  int rc = env->ThrowNew(exception_class, msg.c_str());
  CHECK_EQ(rc, JNI_OK);
}

void Thread::ThrowOutOfMemoryError() {
  UNIMPLEMENTED(FATAL);
}

Frame Thread::FindExceptionHandler(void* throw_pc, void** handler_pc) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  DCHECK(class_linker != NULL);

  Frame cur_frame = GetTopOfStack();
  for (int unwind_depth = 0; ; unwind_depth++) {
    const Method* cur_method = cur_frame.GetMethod();
    DexCache* dex_cache = cur_method->GetDeclaringClass()->GetDexCache();
    const DexFile& dex_file = class_linker->FindDexFile(dex_cache);

    void* handler_addr = FindExceptionHandlerInMethod(cur_method,
                                                      throw_pc,
                                                      dex_file,
                                                      class_linker);
    if (handler_addr) {
      *handler_pc = handler_addr;
      return cur_frame;
    } else {
      // Check if we are at the last frame
      if (cur_frame.HasNext()) {
        cur_frame.Next();
      } else {
        // Either at the top of stack or next frame is native.
        break;
      }
    }
  }
  *handler_pc = NULL;
  return Frame();
}

void* Thread::FindExceptionHandlerInMethod(const Method* method,
                                           void* throw_pc,
                                           const DexFile& dex_file,
                                           ClassLinker* class_linker) {
  Throwable* exception_obj = exception_;
  exception_ = NULL;

  intptr_t dex_pc = -1;
  const DexFile::CodeItem* code_item = dex_file.GetCodeItem(method->GetCodeItemOffset());
  DexFile::CatchHandlerIterator iter;
  for (iter = dex_file.dexFindCatchHandler(*code_item,
                                           method->ToDexPC(reinterpret_cast<intptr_t>(throw_pc)));
       !iter.HasNext();
       iter.Next()) {
    Class* klass = class_linker->FindSystemClass(dex_file.dexStringByTypeIdx(iter.Get().type_idx_));
    DCHECK(klass != NULL);
    if (exception_obj->InstanceOf(klass)) {
      dex_pc = iter.Get().address_;
      break;
    }
  }

  exception_ = exception_obj;
  if (iter.HasNext()) {
    return NULL;
  } else {
    return reinterpret_cast<void*>( method->ToNativePC(dex_pc) );
  }
}

void Thread::VisitRoots(Heap::RootVisitor* visitor, void* arg) const {
  if (exception_ != NULL) {
    visitor(exception_, arg);
  }
  if (peer_ != NULL) {
    visitor(peer_, arg);
  }
  jni_env_->locals.VisitRoots(visitor, arg);
  jni_env_->monitors.VisitRoots(visitor, arg);
  // visitThreadStack(visitor, thread, arg);
  UNIMPLEMENTED(WARNING) << "some per-Thread roots not visited";
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
  int int_state = static_cast<int>(state);
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
