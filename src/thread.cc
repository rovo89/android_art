// Copyright 2011 Google Inc. All Rights Reserved.

#include "thread.h"

#include <pthread.h>
#include <sys/mman.h>
#include <algorithm>
#include <cerrno>
#include <list>

#include "class_linker.h"
#include "jni_internal.h"
#include "object.h"
#include "runtime.h"
#include "utils.h"

namespace art {

pthread_key_t Thread::pthread_key_self_;

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
      GetMethod()->GetFrameSize();
  sp_ = reinterpret_cast<const Method**>(next_sp);
}

void* Frame::GetPC() const {
  byte* pc_addr = reinterpret_cast<byte*>(sp_) +
      GetMethod()->GetReturnPcOffset();
  return reinterpret_cast<void*>(pc_addr);
}

const Method* Frame::NextMethod() const {
  byte* next_sp = reinterpret_cast<byte*>(sp_) +
      GetMethod()->GetFrameSize();
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

  JavaVMExt* vm = runtime->GetJavaVM();
  CHECK(vm != NULL);
  bool check_jni = vm->check_jni;
  thread->jni_env_ = reinterpret_cast<JNIEnv*>(new JNIEnvExt(thread, check_jni));

  return thread;
}

static void ThreadExitCheck(void* arg) {
  LG << "Thread exit check";
}

bool Thread::Init() {
  // Allocate a TLS slot.
  if (pthread_key_create(&Thread::pthread_key_self_, ThreadExitCheck) != 0) {
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

size_t Thread::NumShbHandles() {
  size_t count = 0;
  for (StackHandleBlock* cur = top_shb_; cur; cur = cur->Link()) {
    count += cur->NumberOfReferences();
  }
  return count;
}

bool Thread::ShbContains(jobject obj) {
  Object** shb_entry = reinterpret_cast<Object**>(obj);
  for (StackHandleBlock* cur = top_shb_; cur; cur = cur->Link()) {
    size_t num_refs = cur->NumberOfReferences();
    DCHECK_GT(num_refs, 0u); // A SHB should always have a jobject/jclass
    if ((&cur->Handles()[0] >= shb_entry) &&
        (shb_entry <= (&cur->Handles()[num_refs-1]))) {
      return true;
    }
  }
  return false;
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
  // Make sure that all threads have exited and unregistered when we
  // reach this point. This means that all daemon threads had been
  // shutdown cleanly.
  CHECK_LE(list_.size(), 1U);
  // TODO: wait for all other threads to unregister
  CHECK(list_.size() == 0 || list_.front() == Thread::Current());
  // TODO: detach the current thread
  delete lock_;
  lock_ = NULL;
}

void ThreadList::Register(Thread* thread) {
  MutexLock mu(lock_);
  CHECK(find(list_.begin(), list_.end(), thread) == list_.end());
  list_.push_front(thread);
}

void ThreadList::Unregister(Thread* thread) {
  MutexLock mu(lock_);
  CHECK(find(list_.begin(), list_.end(), thread) != list_.end());
  list_.remove(thread);
}

}  // namespace
