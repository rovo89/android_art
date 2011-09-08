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
#include "utils.h"

namespace art {

pthread_key_t Thread::pthread_key_self_;

// Temporary debugging hook for compiler.
static void DebugMe(Method* method, uint32_t info) {
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
extern "C" void artFailedInvokeInterface()
{
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
static void ThrowException(Thread* thread, Throwable* exception) {
    /*
     * exception may be NULL, in which case this routine should
     * throw NPE.  NOTE: this is a convenience for generated code,
     * which previuosly did the null check inline and constructed
     * and threw a NPE if NULL.  This routine responsible for setting
     * exception_ in thread.
     */
    UNIMPLEMENTED(FATAL) << "Unimplemented exception throw";
}

// TODO: placeholder.  Helper function to type
static Class* InitializeTypeFromCode(uint32_t type_idx, Method* method) {
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
static void ResolveMethodFromCode(Method* method, uint32_t method_idx) {
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
static Array* CheckAndAllocFromCode(uint32_t type_index, Method* method,
                                    int32_t component_count)
{
    /*
     * Just a wrapper around Array::AllocFromCode() that additionally
     * throws a runtime exception "bad Filled array req" for 'D' and 'J'.
     */
    UNIMPLEMENTED(WARNING) << "Need check that not 'D' or 'J'";
    return Array::AllocFromCode(type_index, method, component_count);
}

// TODO: placeholder (throw on failure)
static void CheckCastFromCode(const Class* a, const Class* b) {
    if (a->IsAssignableFrom(b)) {
        return;
    }
    UNIMPLEMENTED(FATAL);
}

// TODO: placeholder
static void UnlockObjectFromCode(Thread* thread, Object* obj) {
    // TODO: throw and unwind if lock not held
    // TODO: throw and unwind on NPE
    obj->MonitorExit();
}

// TODO: placeholder
static void LockObjectFromCode(Thread* thread, Object* obj) {
    // Need thread for ownership?
    obj->MonitorEnter();
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
  pF2l = F2L;
  pD2l = D2L;
  pLdivmod = __aeabi_ldivmod;
  pLmul = __aeabi_lmul;
  pInvokeInterfaceTrampoline = art_invoke_interface_trampoline;
#endif
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
  pDebugMe = DebugMe;
}

Mutex* Mutex::Create(const char* name) {
  Mutex* mu = new Mutex(name);
  int result = pthread_mutex_init(&mu->lock_impl_, NULL);
  CHECK_EQ(result, 0);
  return mu;
}

void Mutex::Lock() {
  int result = pthread_mutex_lock(&lock_impl_);
  CHECK_EQ(result, 0);
  SetOwner(Thread::Current());
}

bool Mutex::TryLock() {
  int result = pthread_mutex_lock(&lock_impl_);
  if (result == EBUSY) {
    return false;
  } else {
    CHECK_EQ(result, 0);
    SetOwner(Thread::Current());
    return true;
  }
}

void Mutex::Unlock() {
  DCHECK(HaveLock());
  int result = pthread_mutex_unlock(&lock_impl_);
  CHECK_EQ(result, 0);
  SetOwner(NULL);
}

bool Mutex::HaveLock() {
  return owner_ == Thread::Current();
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

void* ThreadStart(void *arg) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

Thread* Thread::Create(const Runtime* runtime) {
  UNIMPLEMENTED(FATAL) << "need to pass in a java.lang.Thread";

  size_t stack_size = runtime->GetStackSize();

  Thread* new_thread = new Thread;
  new_thread->InitCpu();

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

  errno = pthread_create(&new_thread->handle_, &attr, ThreadStart, new_thread);
  if (errno != 0) {
    PLOG(FATAL) << "pthread_create failed";
  }

  errno = pthread_attr_destroy(&attr);
  if (errno != 0) {
    PLOG(FATAL) << "pthread_attr_destroy failed";
  }

  // TODO: get the "daemon" field from the java.lang.Thread.
  // new_thread->is_daemon_ = dvmGetFieldBoolean(threadObj, gDvm.offJavaLangThread_daemon);

  return new_thread;
}

Thread* Thread::Attach(const Runtime* runtime, const char* name, bool as_daemon) {
  Thread* thread = new Thread;
  thread->InitCpu();

  thread->tid_ = ::art::GetTid();
  thread->handle_ = pthread_self();
  thread->is_daemon_ = as_daemon;

  thread->state_ = kRunnable;

  SetThreadName(name);

  errno = pthread_setspecific(Thread::pthread_key_self_, thread);
  if (errno != 0) {
    PLOG(FATAL) << "pthread_setspecific failed";
  }

  thread->jni_env_ = new JNIEnvExt(thread, runtime->GetJavaVM());

  return thread;
}

void Thread::Dump(std::ostream& os) const {
  /*
   * Get the java.lang.Thread object.  This function gets called from
   * some weird debug contexts, so it's possible that there's a GC in
   * progress on some other thread.  To decrease the chances of the
   * thread object being moved out from under us, we add the reference
   * to the tracked allocation list, which pins it in place.
   *
   * If threadObj is NULL, the thread is still in the process of being
   * attached to the VM, and there's really nothing interesting to
   * say about it yet.
   */
  os << "TODO: pin Thread before dumping\n";
#if 0
  // TODO: dalvikvm had this limitation, but we probably still want to do our best.
  if (peer_ == NULL) {
    LOGI("Can't dump thread %d: threadObj not set", threadId);
    return;
  }
  dvmAddTrackedAlloc(peer_, NULL);
#endif

  DumpState(os);
  DumpStack(os);

#if 0
  dvmReleaseTrackedAlloc(peer_, NULL);
#endif
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
  std::string thread_name("unknown");
  int priority = -1;

#if 0 // TODO
  nameStr = (StringObject*) dvmGetFieldObject(threadObj, gDvm.offJavaLangThread_name);
  threadName = dvmCreateCstrFromString(nameStr);
  priority = dvmGetFieldInt(threadObj, gDvm.offJavaLangThread_priority);
#else
  {
    // TODO: this may be truncated; we should use the java.lang.Thread 'name' field instead.
    std::string stats;
    if (ReadFileToString(StringPrintf("/proc/self/task/%d/stat", GetTid()).c_str(), &stats)) {
      size_t start = stats.find('(') + 1;
      size_t end = stats.find(')') - start;
      thread_name = stats.substr(start, end);
    }
  }
  priority = -1;
#endif

  int policy;
  sched_param sp;
  errno = pthread_getschedparam(handle_, &policy, &sp);
  if (errno != 0) {
    PLOG(FATAL) << "pthread_getschedparam failed";
  }

  std::string scheduler_group(GetSchedulerGroup(GetTid()));
  if (scheduler_group.empty()) {
    scheduler_group = "default";
  }

  std::string group_name("(null; initializing?)");
#if 0
  groupObj = (Object*) dvmGetFieldObject(threadObj, gDvm.offJavaLangThread_group);
  if (groupObj != NULL) {
    nameStr = (StringObject*) dvmGetFieldObject(groupObj, gDvm.offJavaLangThreadGroup_name);
    groupName = dvmCreateCstrFromString(nameStr);
  }
#else
  group_name = "TODO";
#endif

  os << '"' << thread_name << '"';
  if (is_daemon_) {
    os << " daemon";
  }
  os << " prio=" << priority
     << " tid=" << GetThinLockId()
     << " " << state_ << "\n";

  int suspend_count = 0; // TODO
  int debug_suspend_count = 0; // TODO
  void* peer_ = NULL; // TODO
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

void Thread::DumpStack(std::ostream& os) const {
  os << "UNIMPLEMENTED: Thread::DumpStack\n";
}

static void ThreadExitCheck(void* arg) {
  LG << "Thread exit check";
}

bool Thread::Startup() {
  // Allocate a TLS slot.
  errno = pthread_key_create(&Thread::pthread_key_self_, ThreadExitCheck);
  if (errno != 0) {
    PLOG(WARNING) << "pthread_key_create failed";
    return false;
  }

  // Double-check the TLS slot allocation.
  if (pthread_getspecific(pthread_key_self_) != NULL) {
    LOG(WARNING) << "newly-created pthread TLS slot is not NULL";
    return false;
  }

  // TODO: initialize other locks and condition variables

  return true;
}

void Thread::Shutdown() {
  errno = pthread_key_delete(Thread::pthread_key_self_);
  if (errno != 0) {
    PLOG(WARNING) << "pthread_key_delete failed";
  }
}

Thread::Thread()
    : peer_(NULL),
      top_of_managed_stack_(),
      native_to_managed_record_(NULL),
      top_sirt_(NULL),
      jni_env_(NULL),
      exception_(NULL),
      suspend_count_(0),
      class_loader_override_(NULL) {
  {
    ThreadListLock mu;
    thin_lock_id_ = Runtime::Current()->GetThreadList()->AllocThreadId();
  }
  InitFunctionPointers();
}

void MonitorExitVisitor(const Object* object, void*) {
  Object* entered_monitor = const_cast<Object*>(object);
  entered_monitor->MonitorExit();;
}

Thread::~Thread() {
  // TODO: check we're not calling the JNI DetachCurrentThread function from
  // a call stack that includes managed frames. (It's only valid if the stack is all-native.)

  // On thread detach, all monitors entered with JNI MonitorEnter are automatically exited.
  jni_env_->monitors.VisitRoots(MonitorExitVisitor, NULL);

  if (IsExceptionPending()) {
    UNIMPLEMENTED(FATAL) << "threadExitUncaughtException()";
  }

  // TODO: ThreadGroup.removeThread(this);

  // TODO: this.vmData = 0;

  // TODO: say "bye" to the debugger.
  //if (gDvm.debuggerConnected) {
  //   dvmDbgPostThreadDeath(self);
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
  CountStackDepthVisitor() : depth(0) {}
  virtual bool VisitFrame(const Frame&) {
    ++depth;
    return true;
  }

  int GetDepth() const {
    return depth;
  }

 private:
  uint32_t depth;
};

class BuildStackTraceVisitor : public Thread::StackVisitor {
 public:
  explicit BuildStackTraceVisitor(int depth) : count(0) {
    method_trace = Runtime::Current()->GetClassLinker()->AllocObjectArray<Method>(depth);
    pc_trace = IntArray::Alloc(depth);
  }

  virtual ~BuildStackTraceVisitor() {}

  virtual bool VisitFrame(const Frame& frame) {
    method_trace->Set(count, frame.GetMethod());
    pc_trace->Set(count, frame.GetPC());
    ++count;
    return true;
  }

  const Method* GetMethod(uint32_t i) {
    DCHECK(i < count);
    return method_trace->Get(i);
  }

  uintptr_t GetPC(uint32_t i) {
    DCHECK(i < count);
    return pc_trace->Get(i);
  }

 private:
  uint32_t count;
  ObjectArray<Method>* method_trace;
  IntArray* pc_trace;
};

void Thread::WalkStack(StackVisitor* visitor) {
  Frame frame = Thread::Current()->GetTopOfStack();
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

ObjectArray<StackTraceElement>* Thread::AllocStackTrace() {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();

  CountStackDepthVisitor count_visitor;
  WalkStack(&count_visitor);
  int32_t depth = count_visitor.GetDepth();

  BuildStackTraceVisitor build_trace_visitor(depth);
  WalkStack(&build_trace_visitor);

  ObjectArray<StackTraceElement>* java_traces = class_linker->AllocStackTraceElementArray(depth);

  for (int32_t i = 0; i < depth; ++i) {
    // Prepare parameter for StackTraceElement(String cls, String method, String file, int line)
    const Method* method = build_trace_visitor.GetMethod(i);
    const Class* klass = method->GetDeclaringClass();
    const DexFile& dex_file = class_linker->FindDexFile(klass->GetDexCache());
    String* readable_descriptor = String::AllocFromModifiedUtf8(
        PrettyDescriptor(klass->GetDescriptor()).c_str());

    StackTraceElement* obj =
        StackTraceElement::Alloc(readable_descriptor,
                                 method->GetName(),
                                 String::AllocFromModifiedUtf8(klass->GetSourceFile()),
                                 dex_file.GetLineNumFromPC(method,
                                     method->ToDexPC(build_trace_visitor.GetPC(i))));
    java_traces->Set(i, obj);
  }
  return java_traces;
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
  //(*visitor)(&thread->threadObj, threadId, ROOT_THREAD_OBJECT, arg);
  //(*visitor)(&thread->exception, threadId, ROOT_NATIVE_STACK, arg);
  jni_env_->locals.VisitRoots(visitor, arg);
  jni_env_->monitors.VisitRoots(visitor, arg);
  // visitThreadStack(visitor, thread, arg);
  UNIMPLEMENTED(WARNING) << "some per-Thread roots not visited";
}

static const char* kStateNames[] = {
  "New",
  "Runnable",
  "Blocked",
  "Waiting",
  "TimedWaiting",
  "Native",
  "Terminated",
};
std::ostream& operator<<(std::ostream& os, const Thread::State& state) {
  if (state >= Thread::kNew && state <= Thread::kTerminated) {
    os << kStateNames[state-Thread::kNew];
  } else {
    os << "State[" << static_cast<int>(state) << "]";
  }
  return os;
}

std::ostream& operator<<(std::ostream& os, const Thread& thread) {
  os << "Thread[" << &thread
     << ",pthread_t=" << thread.GetImpl()
     << ",tid=" << thread.GetTid()
     << ",id=" << thread.GetThinLockId()
     << ",state=" << thread.GetState() << "]";
  return os;
}

ThreadList* ThreadList::Create() {
  return new ThreadList;
}

ThreadList::ThreadList() {
  lock_ = Mutex::Create("ThreadList::Lock");
}

ThreadList::~ThreadList() {
  if (Contains(Thread::Current())) {
    Runtime::Current()->DetachCurrentThread();
  }

  // All threads should have exited and unregistered when we
  // reach this point. This means that all daemon threads had been
  // shutdown cleanly.
  // TODO: dump ThreadList if non-empty.
  CHECK_EQ(list_.size(), 0U);

  delete lock_;
  lock_ = NULL;
}

bool ThreadList::Contains(Thread* thread) {
  return find(list_.begin(), list_.end(), thread) != list_.end();
}

void ThreadList::Dump(std::ostream& os) {
  MutexLock mu(lock_);
  os << "DALVIK THREADS (" << list_.size() << "):\n";
  typedef std::list<Thread*>::const_iterator It; // TODO: C++0x auto
  for (It it = list_.begin(), end = list_.end(); it != end; ++it) {
    (*it)->Dump(os);
    os << "\n";
  }
}

void ThreadList::Register(Thread* thread) {
  //LOG(INFO) << "ThreadList::Register() " << *thread;
  MutexLock mu(lock_);
  CHECK(!Contains(thread));
  list_.push_back(thread);
}

void ThreadList::Unregister() {
  //LOG(INFO) << "ThreadList::Unregister() " << *Thread::Current();
  MutexLock mu(lock_);
  Thread* self = Thread::Current();
  CHECK(Contains(self));
  list_.remove(self);
  uint32_t thin_lock_id = self->thin_lock_id_;
  delete self;
  ReleaseThreadId(thin_lock_id);
}

void ThreadList::VisitRoots(Heap::RootVisitor* visitor, void* arg) const {
  MutexLock mu(lock_);
  typedef std::list<Thread*>::const_iterator It; // TODO: C++0x auto
  for (It it = list_.begin(), end = list_.end(); it != end; ++it) {
    (*it)->VisitRoots(visitor, arg);
  }
}

uint32_t ThreadList::AllocThreadId() {
  DCHECK(lock_->HaveLock());
  for (size_t i = 0; i < allocated_ids_.size(); ++i) {
    if (!allocated_ids_[i]) {
      allocated_ids_.set(i);
      return i + 1; // Zero is reserved to mean "invalid".
    }
  }
  LOG(FATAL) << "Out of internal thread ids";
  return 0;
}

void ThreadList::ReleaseThreadId(uint32_t id) {
  DCHECK(lock_->HaveLock());
  --id; // Zero is reserved to mean "invalid".
  DCHECK(allocated_ids_[id]) << id;
  allocated_ids_.reset(id);
}

}  // namespace
