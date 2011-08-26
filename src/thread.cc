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

pthread_key_t Thread::pthread_key_self_;

void Thread::InitFunctionPointers() {
#if defined(__arm__)
  pShlLong = art_shl_long;
  pShrLong = art_shr_long;
  pUshrLong = art_ushr_long;
#endif
  pArtAllocArrayByClass = Array::Alloc;
  pMemcpy = memcpy;
#if 0
//void* (Thread::*pMemcpy)(void*, const void*, size_t) /* = memcpy*/ ;
float (Thread::*pI2f)(int);
int (Thread::*pF2iz)(float);
float (Thread::*pD2f)(double);
double (Thread::*pF2d)(float);
double (Thread::*pI2d)(int);
int (Thread::*pD2iz)(double);
float (Thread::*pL2f)(long);
double (Thread::*pL2d)(long);
long long (Thread::*pArtF2l)(float);
long long (Thread::*pArtD2l)(double);
float (Thread::*pFadd)(float, float);
float (Thread::*pFsub)(float, float);
float (Thread::*pFdiv)(float, float);
float (Thread::*pFmul)(float, float);
float (Thread::*pFmodf)(float, float);
double (Thread::*pDadd)(double, double);
double (Thread::*pDsub)(double, double);
double (Thread::*pDdiv)(double, double);
double (Thread::*pDmul)(double, double);
double (Thread::*pFmod)(double, double);
int (Thread::*pIdivmod)(int, int);
int (Thread::*pIdiv)(int, int);
long long (Thread::*pLdivmod)(long long, long long);
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

void* Frame::GetPC() const {
  byte* pc_addr = reinterpret_cast<byte*>(sp_) +
      GetMethod()->GetReturnPcOffsetInBytes();
  return reinterpret_cast<void*>(pc_addr);
}

const Method* Frame::NextMethod() const {
  byte* next_sp = reinterpret_cast<byte*>(sp_) +
      GetMethod()->GetFrameSizeInBytes();
  return reinterpret_cast<const Method*>(next_sp);
}

void* ThreadStart(void *arg) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

Thread* Thread::Create(const Runtime* runtime) {
  size_t stack_size = runtime->GetStackSize();
  scoped_ptr<MemMap> stack(MemMap::Map(stack_size, PROT_READ | PROT_WRITE));
  if (stack == NULL) {
    LOG(FATAL) << "failed to allocate thread stack";
    // notreached
    return NULL;
  }

  Thread* new_thread = new Thread;
  new_thread->InitCpu();
  new_thread->stack_.reset(stack.release());
  // Since stacks are assumed to grown downward the base is the limit and the limit is the base.
  new_thread->stack_limit_ = stack->GetAddress();
  new_thread->stack_base_ = stack->GetLimit();

  pthread_attr_t attr;
  int result = pthread_attr_init(&attr);
  CHECK_EQ(result, 0);

  result = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  CHECK_EQ(result, 0);

  pthread_t handle;
  result = pthread_create(&handle, &attr, ThreadStart, new_thread);
  CHECK_EQ(result, 0);

  result = pthread_attr_destroy(&attr);
  CHECK_EQ(result, 0);

  return new_thread;
}

Thread* Thread::Attach(const Runtime* runtime) {
  Thread* thread = new Thread;
  thread->InitCpu();
  thread->stack_limit_ = reinterpret_cast<byte*>(-1);  // TODO: getrlimit
  uintptr_t addr = reinterpret_cast<uintptr_t>(&thread);  // TODO: ask pthreads
  uintptr_t stack_base = RoundUp(addr, kPageSize);
  thread->stack_base_ = reinterpret_cast<byte*>(stack_base);
  // TODO: set the stack size

  thread->handle_ = pthread_self();

  thread->state_ = kRunnable;

  errno = pthread_setspecific(Thread::pthread_key_self_, thread);
  if (errno != 0) {
    PLOG(FATAL) << "pthread_setspecific failed";
  }

  thread->jni_env_ = new JNIEnvExt(thread, runtime->GetJavaVM());

  return thread;
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
      LOG(FATAL) << "Invalid indirect reference " << obj;
      result = reinterpret_cast<Object*>(kInvalidIndirectRefObject);
    }
  }

  if (result == NULL) {
    LOG(FATAL) << "JNI ERROR (app bug): use of deleted " << kind << ": "
               << obj;
  }
  Heap::VerifyObject(result);
  return result;
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
      << ",id=" << thread.GetId()
      << ",tid=" << thread.GetNativeId()
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
