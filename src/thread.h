// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_THREAD_H_
#define ART_SRC_THREAD_H_

#include <pthread.h>

#include <bitset>
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
  ~Mutex();

  void Lock();

  bool TryLock();

  void Unlock();

  const char* GetName() { return name_; }

  static Mutex* Create(const char* name);

  pthread_mutex_t* GetImpl() { return &mutex_; }

 private:
  explicit Mutex(const char* name) : name_(name) {}

  const char* name_;

  pthread_mutex_t mutex_;

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
  bool (*pInstanceofNonTrivialFromCode) (const Object*, const Class*);
  void (*pCheckCastFromCode) (const Class*, const Class*);
  Method* (*pFindInterfaceMethodInCache)(Class*, uint32_t, const Method*, struct DvmDex*);
  void (*pUnlockObjectFromCode)(Thread*, Object*);
  void (*pLockObjectFromCode)(Thread*, Object*);
  void (*pThrowException)(Thread*, Throwable*);
  void (*pHandleFillArrayDataFromCode)(Array*, const uint16_t*);
  Class* (*pInitializeTypeFromCode)(uint32_t, Method*);
  void (*pResolveMethodFromCode)(Method*, uint32_t);
  void (*pInvokeInterfaceTrampoline)(void*, void*, void*, void*);
  StaticStorageBase* (*pInitializeStaticStorage)(uint32_t, const Method*);
  Field* (*pFindFieldFromCode)(uint32_t, const Method*);
  void (*pCheckSuspendFromCode)(Thread*);

  class StackVisitor {
   public:
    virtual ~StackVisitor() {}
    virtual bool VisitFrame(const Frame& frame) = 0;
  };

  // Creates a new thread.
  static Thread* Create(const Runtime* runtime);

  // Creates a new thread from the calling thread.
  static Thread* Attach(const Runtime* runtime, const char* name, bool as_daemon);

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
    // TODO: when we have a moving collector, we'll need: return state_ == kRunnable;
    return true;
  }

  uint32_t GetThinLockId() const {
    return thin_lock_id_;
  }

  pid_t GetTid() const {
    return tid_;
  }

  pthread_t GetImpl() const {
    return pthread_;
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

  void SetName(const char* name);

  void Suspend();

  bool IsSuspended();

  void Resume();

  static void Startup();
  static void Shutdown();

  // JNI methods
  JNIEnvExt* GetJniEnv() const {
    return jni_env_;
  }

  // Number of references allocated in SIRTs on this thread
  size_t NumSirtReferences();

  // Is the given obj in this thread's stack indirect reference table?
  bool SirtContains(jobject obj);

  // Convert a jobject into a Object*
  Object* DecodeJObject(jobject obj);

  void RegisterExceptionEntryPoint(void (*handler)(Method**)) {
    exception_entry_point_ = handler;
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

  //
  // Offsets of various members of native Thread class, used by compiled code.
  //

  static ThreadOffset SelfOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, self_));
  }

  static ThreadOffset ExceptionOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, exception_));
  }

  static ThreadOffset IdOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, thin_lock_id_));
  }

  static ThreadOffset CardTableOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, card_table_));
  }

  static ThreadOffset SuspendCountOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, suspend_count_));
  }

  static ThreadOffset StateOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, state_));
  }

  static ThreadOffset StackHwmOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, stack_hwm_));
  }

  static ThreadOffset JniEnvOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, jni_env_));
  }

  static ThreadOffset TopOfManagedStackOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, top_of_managed_stack_) +
        OFFSETOF_MEMBER(Frame, sp_));
  }

  static ThreadOffset TopSirtOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, top_sirt_));
  }

  static ThreadOffset ExceptionEntryPointOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, exception_entry_point_));
  }

  static ThreadOffset SuspendCountEntryPointOffset() {
    return ThreadOffset(OFFSETOF_MEMBER(Thread, suspend_count_entry_point_));
  }

 private:
  Thread();
  ~Thread();
  friend class ThreadList;  // For ~Thread.

  void CreatePeer(const char* name, bool as_daemon);
  friend class Runtime; // For CreatePeer.

  void DumpState(std::ostream& os) const;
  void DumpStack(std::ostream& os) const;

  void InitCpu();
  void InitFunctionPointers();
  void InitStackHwm();

  static void ThreadExitCallback(void* arg);

  void WalkStack(StackVisitor* visitor);

  // Thin lock thread id. This is a small integer used by the thin lock implementation.
  // This is not to be confused with the native thread's tid, nor is it the value returned
  // by java.lang.Thread.getId --- this is a distinct value, used only for locking. One
  // important difference between this id and the ids visible to managed code is that these
  // ones get reused (to ensure that they fit in the number of bits available).
  uint32_t thin_lock_id_;

  // System thread id.
  pid_t tid_;

  // Native thread handle.
  pthread_t pthread_;

  bool is_daemon_;

  // Our managed peer (an instance of java.lang.Thread).
  Object* peer_;

  // FIXME: placeholder for the gc cardTable
  uint32_t card_table_;

  // The high water mark for this thread's stack.
  byte* stack_hwm_;

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
  static const uint32_t kMaxThreadId = 0xFFFF;
  static const uint32_t kInvalidId = 0;
  static const uint32_t kMainId = 1;

  static ThreadList* Create();

  ~ThreadList();

  void Dump(std::ostream& os);

  void Register(Thread* thread);

  void Unregister();

  bool Contains(Thread* thread);

  void VisitRoots(Heap::RootVisitor* visitor, void* arg) const;

 private:
  ThreadList();

  uint32_t AllocThreadId();
  void ReleaseThreadId(uint32_t id);

  void Lock() {
    lock_->Lock();
  }

  void Unlock() {
    lock_->Unlock();
  }

  Mutex* lock_;

  std::bitset<kMaxThreadId> allocated_ids_;
  std::list<Thread*> list_;

  friend class Thread;
  friend class ThreadListLock;

  DISALLOW_COPY_AND_ASSIGN(ThreadList);
};

class ThreadListLock {
 public:
  ThreadListLock(Thread* self = NULL) {
    if (self == NULL) {
      // Try to get it from TLS.
      self = Thread::Current();
    }
    Thread::State old_state;
    if (self != NULL) {
      old_state = self->SetState(Thread::kWaiting);  // TODO: VMWAIT
    } else {
      // This happens during VM shutdown.
      old_state = Thread::kUnknown;
    }
    Runtime::Current()->GetThreadList()->Lock();
    if (self != NULL) {
      self->SetState(old_state);
    }
  }

  ~ThreadListLock() {
    Runtime::Current()->GetThreadList()->Unlock();
  }

 private:
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
