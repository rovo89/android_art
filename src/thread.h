// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_THREAD_H_
#define ART_SRC_THREAD_H_

#include <pthread.h>

#include <iosfwd>
#include <list>

#include "dex_file.h"
#include "globals.h"
#include "jni_internal.h"
#include "logging.h"
#include "macros.h"
#include "mem_map.h"
#include "offsets.h"

namespace art {

class Array;
class Class;
class ClassLinker;
class ClassLoader;
class Method;
class Object;
class Runtime;
class Thread;
class ThreadList;
class Throwable;
class StackTraceElement;
class StaticStorageBase;

template<class T> class ObjectArray;
template<class T> class PrimitiveArray;
typedef PrimitiveArray<int32_t> IntArray;

class Mutex {
 public:
  virtual ~Mutex() {}

  void Lock();

  bool TryLock();

  void Unlock();

  const char* GetName() { return name_; }

  Thread* GetOwner() { return owner_; }

  static Mutex* Create(const char* name);

  // TODO: only needed because we lack a condition variable abstraction.
  pthread_mutex_t* GetImpl() { return &lock_impl_; }

 private:
  explicit Mutex(const char* name) : name_(name), owner_(NULL) {}

  void SetOwner(Thread* thread) { owner_ = thread; }

  const char* name_;

  Thread* owner_;

  pthread_mutex_t lock_impl_;

  DISALLOW_COPY_AND_ASSIGN(Mutex);
};

class MutexLock {
 public:
  explicit MutexLock(Mutex *mu) : mu_(mu) {
    mu_->Lock();
  }
  ~MutexLock() { mu_->Unlock(); }
 private:
  Mutex* const mu_;
  DISALLOW_COPY_AND_ASSIGN(MutexLock);
};

// Stack allocated indirect reference table, allocated within the bridge frame
// between managed and native code.
class StackIndirectReferenceTable {
 public:
  // Number of references contained within this SIRT
  size_t NumberOfReferences() {
    return number_of_references_;
  }

  // Link to previous SIRT or NULL
  StackIndirectReferenceTable* Link() {
    return link_;
  }

  Object** References() {
    return references_;
  }

  // Offset of length within SIRT, used by generated code
  static size_t NumberOfReferencesOffset() {
    return OFFSETOF_MEMBER(StackIndirectReferenceTable, number_of_references_);
  }

  // Offset of link within SIRT, used by generated code
  static size_t LinkOffset() {
    return OFFSETOF_MEMBER(StackIndirectReferenceTable, link_);
  }

 private:
  StackIndirectReferenceTable() {}

  size_t number_of_references_;
  StackIndirectReferenceTable* link_;

  // Fake array, really allocated and filled in by jni_compiler.
  Object* references_[0];

  DISALLOW_COPY_AND_ASSIGN(StackIndirectReferenceTable);
};

struct NativeToManagedRecord {
  NativeToManagedRecord* link;
  void* last_top_of_managed_stack;
};

// Iterator over managed frames up to the first native-to-managed transition
class Frame {
 public:
  Frame() : sp_(NULL) {}

  Method* GetMethod() const {
    return (sp_ != NULL) ? *sp_ : NULL;
  }

  bool HasNext() const {
    return NextMethod() != NULL;
  }

  void Next();

  uintptr_t GetPC() const;

  Method** GetSP() const {
    return sp_;
  }

  // TODO: this is here for testing, remove when we have exception unit tests
  // that use the real stack
  void SetSP(Method** sp) {
    sp_ = sp;
  }

 private:
  Method* NextMethod() const;

  friend class Thread;

  Method** sp_;
};

class Thread {
 public:
  enum State {
    kUnknown = -1,
    kNew,
    kRunnable,
    kBlocked,
    kWaiting,
    kTimedWaiting,
    kNative,
    kTerminated,
  };


  static const size_t kDefaultStackSize = 64 * KB;

  // Runtime support function pointers
  void (*pDebugMe)(Method*, uint32_t);
  void* (*pMemcpy)(void*, const void*, size_t);
  uint64_t (*pShlLong)(uint64_t, uint32_t);
  uint64_t (*pShrLong)(uint64_t, uint32_t);
  uint64_t (*pUshrLong)(uint64_t, uint32_t);
  float (*pI2f)(int);
  int (*pF2iz)(float);
  float (*pD2f)(double);
  double (*pF2d)(float);
  double (*pI2d)(int);
  int (*pD2iz)(double);
  float (*pL2f)(long);
  double (*pL2d)(long);
  long long (*pF2l)(float);
  long long (*pD2l)(double);
  float (*pFadd)(float, float);
  float (*pFsub)(float, float);
  float (*pFdiv)(float, float);
  float (*pFmul)(float, float);
  float (*pFmodf)(float, float);
  double (*pDadd)(double, double);
  double (*pDsub)(double, double);
  double (*pDdiv)(double, double);
  double (*pDmul)(double, double);
  double (*pFmod)(double, double);
  int (*pIdivmod)(int, int);
  int (*pIdiv)(int, int);
  long long (*pLmul)(long long, long long);
  long long (*pLdivmod)(long long, long long);
  Array* (*pAllocFromCode)(uint32_t, Method*, int32_t);
  Array* (*pCheckAndAllocFromCode)(uint32_t, Method*, int32_t);
  Object* (*pAllocObjectFromCode)(uint32_t, Method*);
  uint32_t (*pGet32Static)(uint32_t, const Method*);
  void (*pSet32Static)(uint32_t, const Method*, uint32_t);
  uint64_t (*pGet64Static)(uint32_t, const Method*);
  void (*pSet64Static)(uint32_t, const Method*, uint64_t);
  Object* (*pGetObjStatic)(uint32_t, const Method*);
  void (*pSetObjStatic)(uint32_t, const Method*, Object*);
  void (*pCanPutArrayElementFromCode)(const Class*, const Class*);
  int (*pInstanceofNonTrivialFromCode) (const Class*, const Class*);
  Method* (*pFindInterfaceMethodInCache)(Class*, uint32_t, const Method*, struct DvmDex*);
  bool (*pUnlockObjectFromCode)(Thread*, Object*);
  void (*pLockObjectFromCode)(Thread*, Object*);
  void (*pThrowException)(Thread*, Throwable*);
  void (*pHandleFillArrayDataFromCode)(Array*, const uint16_t*);
  Class* (*pInitializeTypeFromCode)(uint32_t, Method*);
  void (*pResolveMethodFromCode)(Method*, uint32_t);
  void (*pInvokeInterfaceTrampoline)(void*, void*, void*, void*);
  StaticStorageBase* (*pInitializeStaticStorage)(uint32_t, const Method*);

  class StackVisitor {
   public:
    virtual ~StackVisitor() {}
    virtual bool VisitFrame(const Frame& frame) = 0;
  };

  // Creates a new thread.
  static Thread* Create(const Runtime* runtime);

  // Creates a new thread from the calling thread.
  static Thread* Attach(const Runtime* runtime);

  static Thread* Current() {
    void* thread = pthread_getspecific(Thread::pthread_key_self_);
    return reinterpret_cast<Thread*>(thread);
  }

  void Dump(std::ostream& os) const;

  State GetState() const {
    return state_;
  }

  State SetState(State new_state) {
    State old_state = state_;
    state_ = new_state;
    return old_state;
  }

  bool CanAccessDirectReferences() const {
    return state_ == kRunnable;
  }

  uint32_t GetId() const {
    return id_;
  }

  pid_t GetTid() const {
    return tid_;
  }

  pthread_t GetImpl() const {
    return handle_;
  }

  // Returns the Method* for the current method.
  // This is used by the JNI implementation for logging and diagnostic purposes.
  const Method* GetCurrentMethod() const {
    return top_of_managed_stack_.GetMethod();
  }

  bool IsExceptionPending() const {
    return exception_ != NULL;
  }

  Throwable* GetException() const {
    DCHECK(CanAccessDirectReferences());
    return exception_;
  }

  void SetException(Throwable* new_exception) {
    DCHECK(CanAccessDirectReferences());
    CHECK(new_exception != NULL);
    // TODO: CHECK(exception_ == NULL);
    exception_ = new_exception;  // TODO
  }

  void ClearException() {
    exception_ = NULL;
  }

  Frame GetTopOfStack() const {
    return top_of_managed_stack_;
  }

  // TODO: this is here for testing, remove when we have exception unit tests
  // that use the real stack
  void SetTopOfStack(void* stack) {
    top_of_managed_stack_.SetSP(reinterpret_cast<Method**>(stack));
  }

  void ThrowNewException(const char* exception_class_descriptor, const char* fmt, ...)
      __attribute__ ((format(printf, 3, 4)));

  // This exception is special, because we need to pre-allocate an instance.
  void ThrowOutOfMemoryError();

  Frame FindExceptionHandler(void* throw_pc, void** handler_pc);

  void* FindExceptionHandlerInMethod(const Method* method,
                                     void* throw_pc,
                                     const DexFile& dex_file,
                                     ClassLinker* class_linker);
  static ThreadOffset SelfOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, self_));
  }

  // Offset of exception within Thread, used by generated code
  static ThreadOffset ExceptionOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, exception_));
  }

  // Offset of id within Thread, used by generated code
  static ThreadOffset IdOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, id_));
  }

  // Offset of card_table within Thread, used by generated code
  static ThreadOffset CardTableOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, card_table_));
  }

  void SetName(const char* name);

  void Suspend();

  bool IsSuspended();

  void Resume();

  static bool Startup();
  static void Shutdown();

  static ThreadOffset SuspendCountOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, suspend_count_));
  }

  // Offset of state within Thread, used by generated code
  static ThreadOffset StateOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, state_));
  }

  // JNI methods
  JNIEnvExt* GetJniEnv() const {
    return jni_env_;
  }

  // Offset of JNI environment within Thread, used by generated code
  static ThreadOffset JniEnvOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, jni_env_));
  }

  // Offset of top of managed stack address, used by generated code
  static ThreadOffset TopOfManagedStackOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, top_of_managed_stack_) +
                        OFFSETOF_MEMBER(Frame, sp_));
  }

  // Offset of top stack indirect reference table within Thread, used by
  // generated code
  static ThreadOffset TopSirtOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, top_sirt_));
  }

  // Number of references allocated in SIRTs on this thread
  size_t NumSirtReferences();

  // Is the given obj in this thread's stack indirect reference table?
  bool SirtContains(jobject obj);

  // Convert a jobject into a Object*
  Object* DecodeJObject(jobject obj);

  // Offset of exception_entry_point_ within Thread, used by generated code
  static ThreadOffset ExceptionEntryPointOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, exception_entry_point_));
  }

  void RegisterExceptionEntryPoint(void (*handler)(Method**)) {
    exception_entry_point_ = handler;
  }

  // Offset of suspend_count_entry_point_ within Thread, used by generated code
  static ThreadOffset SuspendCountEntryPointOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, suspend_count_entry_point_));
  }

  void RegisterSuspendCountEntryPoint(void (*handler)(Method**)) {
    suspend_count_entry_point_ = handler;
  }

  // Increasing the suspend count, will cause the thread to run to safepoint
  void IncrementSuspendCount() { suspend_count_++; }
  void DecrementSuspendCount() { suspend_count_--; }

  // Linked list recording transitions from native to managed code
  void PushNativeToManagedRecord(NativeToManagedRecord* record) {
    record->last_top_of_managed_stack = reinterpret_cast<void*>(top_of_managed_stack_.GetSP());
    record->link = native_to_managed_record_;
    native_to_managed_record_ = record;
    top_of_managed_stack_.SetSP(NULL);
  }
  void PopNativeToManagedRecord(const NativeToManagedRecord& record) {
    native_to_managed_record_ = record.link;
    top_of_managed_stack_.SetSP(reinterpret_cast<Method**>(record.last_top_of_managed_stack));
  }

  const ClassLoader* GetClassLoaderOverride() {
    // TODO: need to place the class_loader_override_ in a handle
    // DCHECK(CanAccessDirectReferences());
    return class_loader_override_;
  }

  void SetClassLoaderOverride(const ClassLoader* class_loader_override) {
    class_loader_override_ = class_loader_override;
  }

  // Allocate stack trace
  ObjectArray<StackTraceElement>* AllocStackTrace();

  void VisitRoots(Heap::RootVisitor* visitor, void* arg) const;

 private:
  Thread()
      : id_(1234),
        top_of_managed_stack_(),
        native_to_managed_record_(NULL),
        top_sirt_(NULL),
        jni_env_(NULL),
        exception_(NULL),
        suspend_count_(0),
        class_loader_override_(NULL) {
    InitFunctionPointers();
  }

  ~Thread();
  friend class Runtime;  // For ~Thread.

  void DumpState(std::ostream& os) const;
  void DumpStack(std::ostream& os) const;

  void InitCpu();
  void InitFunctionPointers();

  void WalkStack(StackVisitor* visitor);

  // Managed thread id.
  uint32_t id_;

  // System thread id.
  pid_t tid_;

  // Native thread handle.
  pthread_t handle_;

  // FIXME: placeholder for the gc cardTable
  uint32_t card_table_;

  // Top of the managed stack, written out prior to the state transition from
  // kRunnable to kNative. Uses include to give the starting point for scanning
  // a managed stack when a thread is in native code.
  Frame top_of_managed_stack_;

  // A linked list (of stack allocated records) recording transitions from
  // native to managed code.
  NativeToManagedRecord* native_to_managed_record_;

  // Top of linked list of stack indirect reference tables or NULL for none
  StackIndirectReferenceTable* top_sirt_;

  // Every thread may have an associated JNI environment
  JNIEnvExt* jni_env_;

  State state_;

  // Initialized to "this". On certain architectures (such as x86) reading
  // off of Thread::Current is easy but getting the address of Thread::Current
  // is hard. This field can be read off of Thread::Current to give the address.
  Thread* self_;

  Runtime* runtime_;

  // The pending exception or NULL.
  Throwable* exception_;

  // A non-zero value is used to tell the current thread to enter a safe point
  // at the next poll.
  int suspend_count_;

  // Needed to get the right ClassLoader in JNI_OnLoad, but also
  // useful for testing.
  const ClassLoader* class_loader_override_;

  // TLS key used to retrieve the VM thread object.
  static pthread_key_t pthread_key_self_;

  // Entry point called when exception_ is set
  void (*exception_entry_point_)(Method** frame);

  // Entry point called when suspend_count_ is non-zero
  void (*suspend_count_entry_point_)(Method** frame);

  DISALLOW_COPY_AND_ASSIGN(Thread);
};
std::ostream& operator<<(std::ostream& os, const Thread& thread);
std::ostream& operator<<(std::ostream& os, const Thread::State& state);

class ThreadList {
 public:
  static const int kMaxId = 0xFFFF;
  static const int kInvalidId = 0;
  static const int kMainId = 1;

  static ThreadList* Create();

  ~ThreadList();

  void Dump(std::ostream& os);

  void Register(Thread* thread);

  void Unregister(Thread* thread);

  bool Contains(Thread* thread);

  void Lock() {
    lock_->Lock();
  }

  void Unlock() {
    lock_->Unlock();
  };

  void VisitRoots(Heap::RootVisitor* visitor, void* arg) const;

 private:
  ThreadList();

  std::list<Thread*> list_;

  Mutex* lock_;

  DISALLOW_COPY_AND_ASSIGN(ThreadList);
};

class ThreadListLock {
 public:
  ThreadListLock(ThreadList* thread_list, Thread* current_thread)
      : thread_list_(thread_list) {
    if (current_thread == NULL) {  // try to get it from TLS
      current_thread = Thread::Current();
    }
    Thread::State old_state;
    if (current_thread != NULL) {
      old_state = current_thread->SetState(Thread::kWaiting);  // TODO: VMWAIT
    } else {
      // happens during VM shutdown
      old_state = Thread::kUnknown;  // TODO: something else
    }
    thread_list_->Lock();
    if (current_thread != NULL) {
      current_thread->SetState(old_state);
    }
  }

  ~ThreadListLock() {
    thread_list_->Unlock();
  }

 private:
  ThreadList* thread_list_;

  DISALLOW_COPY_AND_ASSIGN(ThreadListLock);
};

class ScopedThreadStateChange {
 public:
  ScopedThreadStateChange(Thread* thread, Thread::State new_state) : thread_(thread) {
    old_thread_state_ = thread_->SetState(new_state);
  }

  ~ScopedThreadStateChange() {
    thread_->SetState(old_thread_state_);
  }

 private:
  Thread* thread_;
  Thread::State old_thread_state_;
  DISALLOW_COPY_AND_ASSIGN(ScopedThreadStateChange);
};

}  // namespace art

#endif  // ART_SRC_THREAD_H_
