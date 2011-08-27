// Copyright 2011 Google Inc. All Rights Reserved.

#include "thread.h"

#include <pthread.h>
#include <sys/mman.h>
#include <algorithm>
#include <cerrno>
#include <list>

#include "class_linker.h"
#include "heap.h"
#include "jni_internal.h"
#include "object.h"
#include "runtime.h"
#include "utils.h"
#include "runtime_support.h"

namespace art {

/* desktop Linux needs a little help with gettid() */
#if !defined(HAVE_ANDROID_OS)
#define __KERNEL__
# include <linux/unistd.h>
#ifdef _syscall0
_syscall0(pid_t, gettid)
#else
pid_t gettid() { return syscall(__NR_gettid);}
#endif
#undef __KERNEL__
#endif

pthread_key_t Thread::pthread_key_self_;

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
  pArtF2l = artF2L;
  pArtD2l = artD2L;
  pLdivmod = __aeabi_ldivmod;
  pLmul = __aeabi_lmul;
#endif
  pAllocFromCode = Array::AllocFromCode;
  pNewInstanceFromCode = Class::NewInstanceFromCode;
  pMemcpy = memcpy;
  pArtHandleFillArrayDataNoThrow = artHandleFillArrayDataNoThrow;
  pGet32Static = Field::Get32StaticFromCode;
  pSet32Static = Field::Set32StaticFromCode;
  pGet64Static = Field::Get64StaticFromCode;
  pSet64Static = Field::Set64StaticFromCode;
  pGetObjStatic = Field::GetObjStaticFromCode;
  pSetObjStatic = Field::SetObjStaticFromCode;
#if 0
bool (Thread::*pArtUnlockObject)(struct Thread*, struct Object*);
bool (Thread::*pArtCanPutArrayElementNoThrow)(const struct ClassObject*,
      const struct ClassObject*);
int (Thread::*pArtInstanceofNonTrivialNoThrow)
  (const struct ClassObject*, const struct ClassObject*);
int (Thread::*pArtInstanceofNonTrivial) (const struct ClassObject*,
     const struct ClassObject*);
struct Method* (Thread::*pArtFindInterfaceMethodInCache)(ClassObject*, uint32_t,
      const struct Method*, struct DvmDex*);
bool (Thread::*pArtUnlockObjectNoThrow)(struct Thread*, struct Object*);
void (Thread::*pArtLockObjectNoThrow)(struct Thread*, struct Object*);
struct Object* (Thread::*pArtAllocObjectNoThrow)(struct ClassObject*, int);
void (Thread::*pArtThrowException)(struct Thread*, struct Object*);
bool (Thread::*pArtHandleFillArrayDataNoThrow)(struct ArrayObject*, const uint16_t*);
#endif
}

Mutex* Mutex::Create(const char* name) {
  Mutex* mu = new Mutex(name);
  int result = pthread_mutex_init(&mu->lock_impl_, NULL);
  CHECK_EQ(0, result);
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
  CHECK(GetOwner() == Thread::Current());
  int result = pthread_mutex_unlock(&lock_impl_);
  CHECK_EQ(result, 0);
  SetOwner(NULL);
}

void Frame::Next() {
  byte* next_sp = reinterpret_cast<byte*>(sp_) +
      GetMethod()->GetFrameSizeInBytes();
  sp_ = reinterpret_cast<const Method**>(next_sp);
}

uintptr_t Frame::GetPC() const {
  byte* pc_addr = reinterpret_cast<byte*>(sp_) +
      GetMethod()->GetReturnPcOffsetInBytes();
  return *reinterpret_cast<uintptr_t*>(pc_addr);
}

const Method* Frame::NextMethod() const {
  byte* next_sp = reinterpret_cast<byte*>(sp_) +
      GetMethod()->GetFrameSizeInBytes();
  return *reinterpret_cast<const Method**>(next_sp);
}

void* ThreadStart(void *arg) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

Thread* Thread::Create(const Runtime* runtime) {
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

  return new_thread;
}

Thread* Thread::Attach(const Runtime* runtime) {
  Thread* thread = new Thread;
  thread->InitCpu();

  thread->handle_ = pthread_self();

  thread->state_ = kRunnable;

  errno = pthread_setspecific(Thread::pthread_key_self_, thread);
  if (errno != 0) {
    PLOG(FATAL) << "pthread_setspecific failed";
  }

  thread->jni_env_ = new JNIEnvExt(thread, runtime->GetJavaVM());

  return thread;
}

pid_t Thread::GetTid() const {
  return gettid();
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

Thread::~Thread() {
  delete jni_env_;
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
    if ((&cur->References()[0] >= sirt_entry) &&
        (sirt_entry <= (&cur->References()[num_refs-1]))) {
      return true;
    }
  }
  return false;
}

Object* Thread::DecodeJObject(jobject obj) {
  // TODO: Only allowed to hold Object* when in the runnable state
  // DCHECK(state_ == kRunnable);
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
      result = locals.Get(ref);
      break;
    }
  case kGlobal:
    {
      JavaVMExt* vm = Runtime::Current()->GetJavaVM();
      IndirectReferenceTable& globals = vm->globals;
      MutexLock mu(vm->globals_lock);
      result = globals.Get(ref);
      break;
    }
  case kWeakGlobal:
    {
      JavaVMExt* vm = Runtime::Current()->GetJavaVM();
      IndirectReferenceTable& weak_globals = vm->weak_globals;
      MutexLock mu(vm->weak_globals_lock);
      result = weak_globals.Get(ref);
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
      result = *reinterpret_cast<Object**>(obj); // Read from SIRT
    } else if (false /*gDvmJni.workAroundAppJniBugs*/) { // TODO
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
  BuildStackTraceVisitor(int depth) : count(0) {
    method_trace = Runtime::Current()->GetClassLinker()->AllocObjectArray<const Method>(depth);
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
  ObjectArray<const Method>* method_trace;
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
    frame.SetSP(reinterpret_cast<const art::Method**>(record->last_top_of_managed_stack));  // last_tos should return Frame instead of sp?
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
        PrettyDescriptor(klass->GetDescriptor()).c_str()
        );

    StackTraceElement* obj =
        StackTraceElement::Alloc(readable_descriptor,
                                 method->GetName(),
                                 String::AllocFromModifiedUtf8(klass->source_file_),
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
  CHECK(exception_class_descriptor[0] == 'L');
  std::string descriptor(exception_class_descriptor + 1);
  CHECK(descriptor[descriptor.length() - 1] == ';');
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
  const DexFile::CodeItem* code_item = dex_file.GetCodeItem(method->code_off_);
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
     << ",id=" << thread.GetId()
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

void ThreadList::Register(Thread* thread) {
  MutexLock mu(lock_);
  CHECK(!Contains(thread));
  list_.push_front(thread);
}

void ThreadList::Unregister(Thread* thread) {
  MutexLock mu(lock_);
  CHECK(Contains(thread));
  list_.remove(thread);
}

}  // namespace
