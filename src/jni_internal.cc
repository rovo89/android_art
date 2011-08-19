// Copyright 2011 Google Inc. All Rights Reserved.

#include "jni_internal.h"

#include <cstdarg>
#include <dlfcn.h>
#include <sys/mman.h>
#include <utility>
#include <vector>

#include "assembler.h"
#include "class_linker.h"
#include "jni.h"
#include "logging.h"
#include "object.h"
#include "runtime.h"
#include "scoped_ptr.h"
#include "stringpiece.h"
#include "thread.h"

namespace art {

// This is private API, but with two different implementations: ARM and x86.
void CreateInvokeStub(Assembler* assembler, Method* method);

// TODO: this should be in our anonymous namespace, but is currently needed
// for testing in "jni_internal_test.cc".
bool EnsureInvokeStub(Method* method) {
  if (method->GetInvokeStub() != NULL) {
    return true;
  }
  // TODO: use signature to find a matching stub
  // TODO: failed, acquire a lock on the stub table
  Assembler assembler;
  CreateInvokeStub(&assembler, method);
  // TODO: store native_entry in the stub table
  int prot = PROT_READ | PROT_WRITE | PROT_EXEC;
  size_t length = assembler.CodeSize();
  void* addr = mmap(NULL, length, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (addr == MAP_FAILED) {
    PLOG(FATAL) << "mmap failed";
  }
  MemoryRegion region(addr, length);
  assembler.FinalizeInstructions(region);
  method->SetInvokeStub(reinterpret_cast<Method::InvokeStub*>(region.pointer()));
  return true;
}

// TODO: this can't be in our anonymous namespace because of the map in JavaVM.
class SharedLibrary {
public:
  SharedLibrary(const std::string& path, void* handle, Object* class_loader)
      : path_(path),
        handle_(handle),
        jni_on_load_lock_(Mutex::Create("JNI_OnLoad lock")),
        jni_on_load_tid_(Thread::Current()->GetId()),
        jni_on_load_result_(kPending) {
  }

  ~SharedLibrary() {
    delete jni_on_load_lock_;
  }

  Object* GetClassLoader() {
    return class_loader_;
  }

  /*
   * Check the result of an earlier call to JNI_OnLoad on this library.  If
   * the call has not yet finished in another thread, wait for it.
   */
  bool CheckOnLoadResult(JavaVMExt* vm) {
    Thread* self = Thread::Current();
    if (jni_on_load_tid_ == self->GetId()) {
      // Check this so we don't end up waiting for ourselves.  We need
      // to return "true" so the caller can continue.
      LOG(INFO) << *self << " recursive attempt to load library "
                << "\"" << path_ << "\"";
      return true;
    }

    UNIMPLEMENTED(ERROR) << "need to pthread_cond_wait!";
    // MutexLock mu(jni_on_load_lock_);
    while (jni_on_load_result_ == kPending) {
      if (vm->verbose_jni) {
        LOG(INFO) << "[" << *self << " waiting for \"" << path_ << "\" "
                  << "JNI_OnLoad...]";
      }
      Thread::State old_state = self->GetState();
      self->SetState(Thread::kWaiting); // TODO: VMWAIT
      // pthread_cond_wait(&jni_on_load_cond_, &jni_on_load_lock_);
      self->SetState(old_state);
    }

    bool okay = (jni_on_load_result_ == kOkay);
    if (vm->verbose_jni) {
      LOG(INFO) << "[Earlier JNI_OnLoad for \"" << path_ << "\" "
                << (okay ? "succeeded" : "failed") << "]";
    }
    return okay;
  }

  void SetResult(bool result) {
    jni_on_load_result_ = result ? kOkay : kFailed;
    jni_on_load_tid_ = 0;

    // Broadcast a wakeup to anybody sleeping on the condition variable.
    UNIMPLEMENTED(ERROR) << "missing pthread_cond_broadcast";
    // MutexLock mu(library->jni_on_load_lock_);
    // pthread_cond_broadcast(&library->jni_on_load_cond_);
  }

 private:
  enum JNI_OnLoadState {
    kPending,
    kFailed,
    kOkay,
  };

  // Path to library "/system/lib/libjni.so".
  std::string path_;

  // The void* returned by dlopen(3).
  void* handle_;

  // The ClassLoader this library is associated with.
  Object* class_loader_;

  // Guards remaining items.
  Mutex* jni_on_load_lock_;
  // Wait for JNI_OnLoad in other thread.
  pthread_cond_t jni_on_load_cond_;
  // Recursive invocation guard.
  uint32_t jni_on_load_tid_;
  // Result of earlier JNI_OnLoad call.
  JNI_OnLoadState jni_on_load_result_;
};

namespace {

// Entry/exit processing for all JNI calls.
//
// This performs the necessary thread state switching, lets us amortize the
// cost of working out the current thread, and lets us check (and repair) apps
// that are using a JNIEnv on the wrong thread.
class ScopedJniThreadState {
 public:
  explicit ScopedJniThreadState(JNIEnv* env)
      : env_(reinterpret_cast<JNIEnvExt*>(env)) {
    self_ = ThreadForEnv(env);
    self_->SetState(Thread::kRunnable);
  }

  ~ScopedJniThreadState() {
    self_->SetState(Thread::kNative);
  }

  JNIEnvExt* Env() {
    return env_;
  }

  Thread* Self() {
    return self_;
  }

 private:
  static Thread* ThreadForEnv(JNIEnv* env) {
    // TODO: need replacement for gDvmJni.
    bool workAroundAppJniBugs = true;
    Thread* env_self = reinterpret_cast<JNIEnvExt*>(env)->self;
    Thread* self = workAroundAppJniBugs ? Thread::Current() : env_self;
    if (self != env_self) {
      LOG(ERROR) << "JNI ERROR: JNIEnv for " << *env_self
          << " used on " << *self;
      // TODO: dump stack
    }
    return self;
  }

  JNIEnvExt* env_;
  Thread* self_;
  DISALLOW_COPY_AND_ASSIGN(ScopedJniThreadState);
};

/*
 * Add a local reference for an object to the current stack frame.  When
 * the native function returns, the reference will be discarded.
 *
 * We need to allow the same reference to be added multiple times.
 *
 * This will be called on otherwise unreferenced objects.  We cannot do
 * GC allocations here, and it's best if we don't grab a mutex.
 *
 * Returns the local reference (currently just the same pointer that was
 * passed in), or NULL on failure.
 */
template<typename T>
T AddLocalReference(ScopedJniThreadState& ts, Object* obj) {
  if (obj == NULL) {
    return NULL;
  }

  IndirectReferenceTable& locals = ts.Env()->locals;

  uint32_t cookie = IRT_FIRST_SEGMENT; // TODO
  IndirectRef ref = locals.Add(cookie, obj);
  if (ref == NULL) {
    // TODO: just change Add's DCHECK to CHECK and lose this?
    locals.Dump();
    LOG(FATAL) << "Failed adding to JNI local reference table "
               << "(has " << locals.Capacity() << " entries)";
    // TODO: dvmDumpThread(dvmThreadSelf(), false);
  }

#if 0 // TODO: fix this to understand PushLocalFrame, so we can turn it on.
  if (ts.Env()->check_jni) {
    size_t entry_count = locals.Capacity();
    if (entry_count > 16) {
      std::string class_name(PrettyDescriptor(obj->GetClass()->GetDescriptor()));
      LOG(WARNING) << "Warning: more than 16 JNI local references: "
                   << entry_count << " (most recent was a " << class_name << ")";
      locals.Dump();
      // TODO: dvmDumpThread(dvmThreadSelf(), false);
      // dvmAbort();
    }
  }
#endif

  if (false /*gDvmJni.workAroundAppJniBugs*/) { // TODO
    // Hand out direct pointers to support broken old apps.
    return reinterpret_cast<T>(obj);
  }

  return reinterpret_cast<T>(ref);
}

jweak AddWeakGlobalReference(ScopedJniThreadState& ts, Object* obj) {
  if (obj == NULL) {
    return NULL;
  }
  JavaVMExt* vm = Runtime::Current()->GetJavaVM();
  IndirectReferenceTable& weak_globals = vm->weak_globals;
  MutexLock mu(vm->weak_globals_lock);
  IndirectRef ref = weak_globals.Add(IRT_FIRST_SEGMENT, obj);
  return reinterpret_cast<jweak>(ref);
}

template<typename T>
T Decode(ScopedJniThreadState& ts, jobject obj) {
  if (obj == NULL) {
    return NULL;
  }

  IndirectRef ref = reinterpret_cast<IndirectRef>(obj);
  IndirectRefKind kind = GetIndirectRefKind(ref);
  Object* result;
  switch (kind) {
  case kLocal:
    {
      IndirectReferenceTable& locals = ts.Env()->locals;
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
  case kInvalid:
  default:
    // TODO: make stack handle blocks more efficient
    // Check if this is a local reference in a stack handle block
    if (ts.Self()->ShbContains(obj)) {
      return *reinterpret_cast<T*>(obj); // Read from stack handle block
    }
    if (false /*gDvmJni.workAroundAppJniBugs*/) { // TODO
      // Assume an invalid local reference is actually a direct pointer.
      return reinterpret_cast<T>(obj);
    }
    LOG(FATAL) << "Invalid indirect reference " << obj;
    return reinterpret_cast<T>(kInvalidIndirectRefObject);
  }

  if (result == NULL) {
    LOG(FATAL) << "JNI ERROR (app bug): use of deleted " << kind << ": "
               << obj;
  }
  return reinterpret_cast<T>(result);
}

Field* DecodeField(ScopedJniThreadState& ts, jfieldID fid) {
  return Decode<Field*>(ts, reinterpret_cast<jweak>(fid));
}

Method* DecodeMethod(ScopedJniThreadState& ts, jmethodID mid) {
  return Decode<Method*>(ts, reinterpret_cast<jweak>(mid));
}

byte* CreateArgArray(ScopedJniThreadState& ts, Method* method, va_list ap) {
  size_t num_bytes = method->NumArgArrayBytes();
  scoped_array<byte> arg_array(new byte[num_bytes]);
  const StringPiece& shorty = method->GetShorty();
  for (int i = 1, offset = 0; i < shorty.size(); ++i) {
    switch (shorty[i]) {
      case 'Z':
      case 'B':
      case 'C':
      case 'S':
      case 'I':
        *reinterpret_cast<int32_t*>(&arg_array[offset]) = va_arg(ap, jint);
        offset += 4;
        break;
      case 'F':
        *reinterpret_cast<float*>(&arg_array[offset]) = va_arg(ap, jdouble);
        offset += 4;
        break;
      case 'L': {
        Object* obj = Decode<Object*>(ts, va_arg(ap, jobject));
        *reinterpret_cast<Object**>(&arg_array[offset]) = obj;
        offset += sizeof(Object*);
        break;
      }
      case 'D':
        *reinterpret_cast<double*>(&arg_array[offset]) = va_arg(ap, jdouble);
        offset += 8;
        break;
      case 'J':
        *reinterpret_cast<int64_t*>(&arg_array[offset]) = va_arg(ap, jlong);
        offset += 8;
        break;
    }
  }
  return arg_array.release();
}

byte* CreateArgArray(ScopedJniThreadState& ts, Method* method, jvalue* args) {
  size_t num_bytes = method->NumArgArrayBytes();
  scoped_array<byte> arg_array(new byte[num_bytes]);
  const StringPiece& shorty = method->GetShorty();
  for (int i = 1, offset = 0; i < shorty.size(); ++i) {
    switch (shorty[i]) {
      case 'Z':
      case 'B':
      case 'C':
      case 'S':
      case 'I':
        *reinterpret_cast<uint32_t*>(&arg_array[offset]) = args[i - 1].i;
        offset += 4;
        break;
      case 'F':
        *reinterpret_cast<float*>(&arg_array[offset]) = args[i - 1].f;
        offset += 4;
        break;
      case 'L': {
        Object* obj = Decode<Object*>(ts, args[i - 1].l);
        *reinterpret_cast<Object**>(&arg_array[offset]) = obj;
        offset += sizeof(Object*);
        break;
      }
      case 'D':
        *reinterpret_cast<double*>(&arg_array[offset]) = args[i - 1].d;
        offset += 8;
        break;
      case 'J':
        *reinterpret_cast<uint64_t*>(&arg_array[offset]) = args[i - 1].j;
        offset += 8;
        break;
    }
  }
  return arg_array.release();
}

JValue InvokeWithArgArray(ScopedJniThreadState& ts, jobject obj,
                          jmethodID mid, byte* args) {
  Method* method = DecodeMethod(ts, mid);
  Object* rcvr = Decode<Object*>(ts, obj);
  Thread* self = ts.Self();

  // Push a transition back into managed code onto the linked list in thread
  CHECK_EQ(Thread::kRunnable, self->GetState());
  NativeToManagedRecord record;
  self->PushNativeToManagedRecord(&record);

  // Call the invoke stub associated with the method
  // Pass everything as arguments
  const Method::InvokeStub* stub = method->GetInvokeStub();
  CHECK(stub != NULL);
  JValue result;
  // TODO: we should always have code associated with a method
  if (method->GetCode()) {
    (*stub)(method, rcvr, self, args, &result);
  } else {
    // TODO: pretty print method here
    LOG(WARNING) << "Not invoking method with no associated code";
    result.j = 0;
  }
  // Pop transition
  self->PopNativeToManagedRecord(record);
  return result;
}

JValue InvokeWithJValues(ScopedJniThreadState& ts, jobject obj,
                         jmethodID mid, jvalue* args) {
  Method* method = DecodeMethod(ts, mid);
  scoped_array<byte> arg_array(CreateArgArray(ts, method, args));
  return InvokeWithArgArray(ts, obj, mid, arg_array.get());
}

JValue InvokeWithVarArgs(ScopedJniThreadState& ts, jobject obj,
                         jmethodID mid, va_list args) {
  Method* method = DecodeMethod(ts, mid);
  scoped_array<byte> arg_array(CreateArgArray(ts, method, args));
  return InvokeWithArgArray(ts, obj, mid, arg_array.get());
}

// Section 12.3.2 of the JNI spec describes JNI class descriptors. They're
// separated with slashes but aren't wrapped with "L;" like regular descriptors
// (i.e. "a/b/C" rather than "La/b/C;"). Arrays of reference types are an
// exception; there the "L;" must be present ("[La/b/C;"). Historically we've
// supported names with dots too (such as "a.b.C").
std::string NormalizeJniClassDescriptor(const char* name) {
  std::string result;
  // Add the missing "L;" if necessary.
  if (name[0] == '[') {
    result = name;
  } else {
    result += 'L';
    result += name;
    result += ';';
  }
  // Rewrite '.' as '/' for backwards compatibility.
  if (result.find('.') != std::string::npos) {
    LOG(WARNING) << "Call to JNI FindClass with dots in name: "
                 << "\"" << name << "\"";
    std::replace(result.begin(), result.end(), '.', '/');
  }
  return result;
}

jmethodID FindMethodID(ScopedJniThreadState& ts, jclass jni_class, const char* name, const char* sig, bool is_static) {
  Class* c = Decode<Class*>(ts, jni_class);
  if (!c->IsInitialized()) {
    // TODO: initialize the class
  }

  Method* method = NULL;
  if (is_static) {
    method = c->FindDirectMethod(name, sig);
  } else {
    method = c->FindVirtualMethod(name, sig);
    if (method == NULL) {
      // No virtual method matching the signature.  Search declared
      // private methods and constructors.
      method = c->FindDeclaredDirectMethod(name, sig);
    }
  }

  if (method == NULL || method->IsStatic() != is_static) {
    Thread* self = Thread::Current();
    std::string class_name(c->GetDescriptor().ToString());
    // TODO: try searching for the opposite kind of method from is_static
    // for better diagnostics?
    self->ThrowNewException("Ljava/lang/NoSuchMethodError;",
        "no %s method \"%s.%s%s\"", is_static ? "static" : "non-static",
        class_name.c_str(), name, sig);
    return NULL;
  }

  bool success = EnsureInvokeStub(method);
  if (!success) {
    // TODO: throw OutOfMemoryException
    return NULL;
  }

  return reinterpret_cast<jmethodID>(AddWeakGlobalReference(ts, method));
}

jfieldID FindFieldID(ScopedJniThreadState& ts, jclass jni_class, const char* name, const char* sig, bool is_static) {
  Class* c = Decode<Class*>(ts, jni_class);
  if (!c->IsInitialized()) {
    // TODO: initialize the class
  }

  Field* field = NULL;
  if (is_static) {
    field = c->FindStaticField(name, sig);
  } else {
    field = c->FindInstanceField(name, sig);
  }

  if (field == NULL) {
    Thread* self = Thread::Current();
    std::string class_name(c->GetDescriptor().ToString());
    self->ThrowNewException("Ljava/lang/NoSuchFieldError;",
        "no \"%s\" field \"%s\" in class \"%s\" or its superclasses", sig,
        name, class_name.c_str());
    return NULL;
  }

  jweak fid = AddWeakGlobalReference(ts, field);
  return reinterpret_cast<jfieldID>(fid);
}

template<typename JniT, typename ArtT>
JniT NewPrimitiveArray(ScopedJniThreadState& ts, jsize length) {
  CHECK_GE(length, 0); // TODO: ReportJniError
  ArtT* result = ArtT::Alloc(length);
  return AddLocalReference<JniT>(ts, result);
}

}  // namespace

class JNI {
 public:

  static jint GetVersion(JNIEnv* env) {
    ScopedJniThreadState ts(env);
    return JNI_VERSION_1_6;
  }

  static jclass DefineClass(JNIEnv* env, const char*, jobject, const jbyte*, jsize) {
    ScopedJniThreadState ts(env);
    LOG(WARNING) << "JNI DefineClass is not supported";
    return NULL;
  }

  static jclass FindClass(JNIEnv* env, const char* name) {
    ScopedJniThreadState ts(env);
    ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
    std::string descriptor(NormalizeJniClassDescriptor(name));
    // TODO: need to get the appropriate ClassLoader.
    Class* c = class_linker->FindClass(descriptor, NULL);
    return AddLocalReference<jclass>(ts, c);
  }

  static jmethodID FromReflectedMethod(JNIEnv* env, jobject java_method) {
    ScopedJniThreadState ts(env);
    Method* method = Decode<Method*>(ts, java_method);
    return reinterpret_cast<jmethodID>(AddWeakGlobalReference(ts, method));
  }

  static jfieldID FromReflectedField(JNIEnv* env, jobject java_field) {
    ScopedJniThreadState ts(env);
    Field* field = Decode<Field*>(ts, java_field);
    return reinterpret_cast<jfieldID>(AddWeakGlobalReference(ts, field));
  }

  static jobject ToReflectedMethod(JNIEnv* env, jclass, jmethodID mid, jboolean) {
    ScopedJniThreadState ts(env);
    Method* method = DecodeMethod(ts, mid);
    return AddLocalReference<jobject>(ts, method);
  }

  static jobject ToReflectedField(JNIEnv* env, jclass, jfieldID fid, jboolean) {
    ScopedJniThreadState ts(env);
    Field* field = DecodeField(ts, fid);
    return AddLocalReference<jobject>(ts, field);
  }

  static jclass GetSuperclass(JNIEnv* env, jclass sub) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return NULL;
  }

  static jboolean IsAssignableFrom(JNIEnv* env, jclass sub, jclass sup) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return JNI_FALSE;
  }

  static jint Throw(JNIEnv* env, jthrowable obj) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static jint ThrowNew(JNIEnv* env, jclass clazz, const char* msg) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static jthrowable ExceptionOccurred(JNIEnv* env) {
    ScopedJniThreadState ts(env);
    Object* exception = ts.Self()->GetException();
    if (exception == NULL) {
      return NULL;
    } else {
      // TODO: if adding a local reference failing causes the VM to abort
      // then the following check will never occur.
      jthrowable localException = AddLocalReference<jthrowable>(ts, exception);
      if (localException == NULL) {
        // We were unable to add a new local reference, and threw a new
        // exception.  We can't return "exception", because it's not a
        // local reference.  So we have to return NULL, indicating that
        // there was no exception, even though it's pretty much raining
        // exceptions in here.
        LOG(WARNING) << "JNI WARNING: addLocal/exception combo";
      }
      return localException;
    }
  }

  static void ExceptionDescribe(JNIEnv* env) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void ExceptionClear(JNIEnv* env) {
    ScopedJniThreadState ts(env);
    ts.Self()->ClearException();
  }

  static void FatalError(JNIEnv* env, const char* msg) {
    ScopedJniThreadState ts(env);
    LOG(FATAL) << "JNI FatalError called: " << msg;
  }

  static jint PushLocalFrame(JNIEnv* env, jint cap) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(WARNING) << "ignoring PushLocalFrame(" << cap << ")";
    return JNI_OK;
  }

  static jobject PopLocalFrame(JNIEnv* env, jobject res) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(WARNING) << "ignoring PopLocalFrame " << res;
    return res;
  }

  static jobject NewGlobalRef(JNIEnv* env, jobject obj) {
    ScopedJniThreadState ts(env);
    if (obj == NULL) {
      return NULL;
    }

    JavaVMExt* vm = Runtime::Current()->GetJavaVM();
    IndirectReferenceTable& globals = vm->globals;
    MutexLock mu(vm->globals_lock);
    IndirectRef ref = globals.Add(IRT_FIRST_SEGMENT, Decode<Object*>(ts, obj));
    return reinterpret_cast<jobject>(ref);
  }

  static void DeleteGlobalRef(JNIEnv* env, jobject obj) {
    ScopedJniThreadState ts(env);
    if (obj == NULL) {
      return;
    }

    JavaVMExt* vm = Runtime::Current()->GetJavaVM();
    IndirectReferenceTable& globals = vm->globals;
    MutexLock mu(vm->globals_lock);

    if (!globals.Remove(IRT_FIRST_SEGMENT, obj)) {
      LOG(WARNING) << "JNI WARNING: DeleteGlobalRef(" << obj << ") "
                   << "failed to find entry";
    }
  }

  static jweak NewWeakGlobalRef(JNIEnv* env, jobject obj) {
    ScopedJniThreadState ts(env);
    return AddWeakGlobalReference(ts, Decode<Object*>(ts, obj));
  }

  static void DeleteWeakGlobalRef(JNIEnv* env, jweak obj) {
    ScopedJniThreadState ts(env);
    if (obj == NULL) {
      return;
    }

    JavaVMExt* vm = Runtime::Current()->GetJavaVM();
    IndirectReferenceTable& weak_globals = vm->weak_globals;
    MutexLock mu(vm->weak_globals_lock);

    if (!weak_globals.Remove(IRT_FIRST_SEGMENT, obj)) {
      LOG(WARNING) << "JNI WARNING: DeleteWeakGlobalRef(" << obj << ") "
                   << "failed to find entry";
    }
  }

  static jobject NewLocalRef(JNIEnv* env, jobject obj) {
    ScopedJniThreadState ts(env);
    if (obj == NULL) {
      return NULL;
    }

    IndirectReferenceTable& locals = ts.Env()->locals;

    uint32_t cookie = IRT_FIRST_SEGMENT; // TODO
    IndirectRef ref = locals.Add(cookie, Decode<Object*>(ts, obj));
    return reinterpret_cast<jobject>(ref);
  }

  static void DeleteLocalRef(JNIEnv* env, jobject obj) {
    ScopedJniThreadState ts(env);
    if (obj == NULL) {
      return;
    }

    IndirectReferenceTable& locals = ts.Env()->locals;

    uint32_t cookie = IRT_FIRST_SEGMENT; // TODO
    if (!locals.Remove(cookie, obj)) {
      // Attempting to delete a local reference that is not in the
      // topmost local reference frame is a no-op.  DeleteLocalRef returns
      // void and doesn't throw any exceptions, but we should probably
      // complain about it so the user will notice that things aren't
      // going quite the way they expect.
      LOG(WARNING) << "JNI WARNING: DeleteLocalRef(" << obj << ") "
                   << "failed to find entry";
    }
  }

  static jboolean IsSameObject(JNIEnv* env, jobject obj1, jobject obj2) {
    ScopedJniThreadState ts(env);
    return (Decode<Object*>(ts, obj1) == Decode<Object*>(ts, obj2))
        ? JNI_TRUE : JNI_FALSE;
  }

  static jint EnsureLocalCapacity(JNIEnv* env, jint) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static jobject AllocObject(JNIEnv* env, jclass clazz) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return NULL;
  }

  static jobject NewObject(JNIEnv* env, jclass clazz, jmethodID methodID, ...) {
    ScopedJniThreadState ts(env);
    va_list args;
    va_start(args, methodID);
    jobject result = NewObjectV(env, clazz, methodID, args);
    va_end(args);
    return result;
  }

  static jobject NewObjectV(JNIEnv* env,
      jclass clazz, jmethodID methodID, va_list args) {
    ScopedJniThreadState ts(env);
    Class* klass = Decode<Class*>(ts, clazz);
    Object* result = klass->NewInstance();
    jobject local_result = AddLocalReference<jobject>(ts, result);
    CallNonvirtualVoidMethodV(env, local_result, clazz, methodID, args);
    return local_result;
  }

  static jobject NewObjectA(JNIEnv* env,
      jclass clazz, jmethodID methodID, jvalue* args) {
    ScopedJniThreadState ts(env);
    Class* klass = Decode<Class*>(ts, clazz);
    Object* result = klass->NewInstance();
    jobject local_result = AddLocalReference<jobjectArray>(ts, result);
    CallNonvirtualVoidMethodA(env, local_result, clazz, methodID, args);
    return local_result;
  }

  static jclass GetObjectClass(JNIEnv* env, jobject obj) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return NULL;
  }

  static jboolean IsInstanceOf(JNIEnv* env, jobject jobj, jclass clazz) {
    ScopedJniThreadState ts(env);
    CHECK_NE(static_cast<jclass>(NULL), clazz);
    if (jobj == NULL) {
      // NB. JNI is different from regular Java instanceof in this respect
      return JNI_TRUE;
    } else {
      Object* obj = Decode<Object*>(ts, jobj);
      Class* klass = Decode<Class*>(ts, clazz);
      return Object::InstanceOf(obj, klass) ? JNI_TRUE : JNI_FALSE;
    }
  }

  static jmethodID GetMethodID(JNIEnv* env, jclass c, const char* name, const char* sig) {
    ScopedJniThreadState ts(env);
    return FindMethodID(ts, c, name, sig, false);
  }

  static jmethodID GetStaticMethodID(JNIEnv* env, jclass c, const char* name, const char* sig) {
    ScopedJniThreadState ts(env);
    return FindMethodID(ts, c, name, sig, true);
  }

  static jobject CallObjectMethod(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return NULL;
  }

  static jobject CallObjectMethodV(JNIEnv* env,
      jobject obj, jmethodID methodID, va_list args) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return NULL;
  }

  static jobject CallObjectMethodA(JNIEnv* env,
      jobject obj, jmethodID methodID, jvalue*  args) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return NULL;
  }

  static jboolean CallBooleanMethod(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return JNI_FALSE;
  }

  static jboolean CallBooleanMethodV(JNIEnv* env,
      jobject obj, jmethodID methodID, va_list args) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return JNI_FALSE;
  }

  static jboolean CallBooleanMethodA(JNIEnv* env,
      jobject obj, jmethodID methodID, jvalue*  args) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return JNI_FALSE;
  }

  static jbyte CallByteMethod(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static jbyte CallByteMethodV(JNIEnv* env,
      jobject obj, jmethodID methodID, va_list args) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static jbyte CallByteMethodA(JNIEnv* env,
      jobject obj, jmethodID methodID, jvalue* args) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static jchar CallCharMethod(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static jchar CallCharMethodV(JNIEnv* env,
      jobject obj, jmethodID methodID, va_list args) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static jchar CallCharMethodA(JNIEnv* env,
      jobject obj, jmethodID methodID, jvalue* args) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static jshort CallShortMethod(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static jshort CallShortMethodV(JNIEnv* env,
      jobject obj, jmethodID methodID, va_list args) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static jshort CallShortMethodA(JNIEnv* env,
      jobject obj, jmethodID methodID, jvalue* args) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static jint CallIntMethod(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static jint CallIntMethodV(JNIEnv* env,
      jobject obj, jmethodID methodID, va_list args) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static jint CallIntMethodA(JNIEnv* env,
      jobject obj, jmethodID methodID, jvalue* args) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static jlong CallLongMethod(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static jlong CallLongMethodV(JNIEnv* env,
      jobject obj, jmethodID methodID, va_list args) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static jlong CallLongMethodA(JNIEnv* env,
      jobject obj, jmethodID methodID, jvalue* args) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static jfloat CallFloatMethod(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static jfloat CallFloatMethodV(JNIEnv* env,
      jobject obj, jmethodID methodID, va_list args) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static jfloat CallFloatMethodA(JNIEnv* env,
      jobject obj, jmethodID methodID, jvalue* args) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static jdouble CallDoubleMethod(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static jdouble CallDoubleMethodV(JNIEnv* env,
      jobject obj, jmethodID methodID, va_list args) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static jdouble CallDoubleMethodA(JNIEnv* env,
      jobject obj, jmethodID methodID, jvalue* args) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static void CallVoidMethod(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void CallVoidMethodV(JNIEnv* env, jobject obj,
      jmethodID methodID, va_list args) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void CallVoidMethodA(JNIEnv* env, jobject obj,
      jmethodID methodID, jvalue*  args) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static jobject CallNonvirtualObjectMethod(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID methodID, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, methodID);
    JValue result = InvokeWithVarArgs(ts, obj, methodID, ap);
    jobject local_result = AddLocalReference<jobject>(ts, result.l);
    va_end(ap);
    return local_result;
  }

  static jobject CallNonvirtualObjectMethodV(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID methodID, va_list args) {
    ScopedJniThreadState ts(env);
    JValue result = InvokeWithVarArgs(ts, obj, methodID, args);
    return AddLocalReference<jobject>(ts, result.l);
  }

  static jobject CallNonvirtualObjectMethodA(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID methodID, jvalue*  args) {
    ScopedJniThreadState ts(env);
    JValue result = InvokeWithJValues(ts, obj, methodID, args);
    return AddLocalReference<jobject>(ts, result.l);
  }

  static jboolean CallNonvirtualBooleanMethod(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID methodID, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, methodID);
    JValue result = InvokeWithVarArgs(ts, obj, methodID, ap);
    va_end(ap);
    return result.z;
  }

  static jboolean CallNonvirtualBooleanMethodV(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID methodID, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(ts, obj, methodID, args).z;
  }

  static jboolean CallNonvirtualBooleanMethodA(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID methodID, jvalue*  args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(ts, obj, methodID, args).z;
  }

  static jbyte CallNonvirtualByteMethod(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID methodID, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, methodID);
    JValue result = InvokeWithVarArgs(ts, obj, methodID, ap);
    va_end(ap);
    return result.b;
  }

  static jbyte CallNonvirtualByteMethodV(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID methodID, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(ts, obj, methodID, args).b;
  }

  static jbyte CallNonvirtualByteMethodA(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID methodID, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(ts, obj, methodID, args).b;
  }

  static jchar CallNonvirtualCharMethod(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID methodID, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, methodID);
    JValue result = InvokeWithVarArgs(ts, obj, methodID, ap);
    va_end(ap);
    return result.c;
  }

  static jchar CallNonvirtualCharMethodV(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID methodID, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(ts, obj, methodID, args).c;
  }

  static jchar CallNonvirtualCharMethodA(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID methodID, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(ts, obj, methodID, args).c;
  }

  static jshort CallNonvirtualShortMethod(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID methodID, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, methodID);
    JValue result = InvokeWithVarArgs(ts, obj, methodID, ap);
    va_end(ap);
    return result.s;
  }

  static jshort CallNonvirtualShortMethodV(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID methodID, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(ts, obj, methodID, args).s;
  }

  static jshort CallNonvirtualShortMethodA(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID methodID, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(ts, obj, methodID, args).s;
  }

  static jint CallNonvirtualIntMethod(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID methodID, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, methodID);
    JValue result = InvokeWithVarArgs(ts, obj, methodID, ap);
    va_end(ap);
    return result.i;
  }

  static jint CallNonvirtualIntMethodV(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID methodID, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(ts, obj, methodID, args).i;
  }

  static jint CallNonvirtualIntMethodA(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID methodID, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(ts, obj, methodID, args).i;
  }

  static jlong CallNonvirtualLongMethod(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID methodID, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, methodID);
    JValue result = InvokeWithVarArgs(ts, obj, methodID, ap);
    va_end(ap);
    return result.j;
  }

  static jlong CallNonvirtualLongMethodV(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID methodID, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(ts, obj, methodID, args).j;
  }

  static jlong CallNonvirtualLongMethodA(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID methodID, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(ts, obj, methodID, args).j;
  }

  static jfloat CallNonvirtualFloatMethod(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID methodID, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, methodID);
    JValue result = InvokeWithVarArgs(ts, obj, methodID, ap);
    va_end(ap);
    return result.f;
  }

  static jfloat CallNonvirtualFloatMethodV(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID methodID, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(ts, obj, methodID, args).f;
  }

  static jfloat CallNonvirtualFloatMethodA(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID methodID, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(ts, obj, methodID, args).f;
  }

  static jdouble CallNonvirtualDoubleMethod(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID methodID, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, methodID);
    JValue result = InvokeWithVarArgs(ts, obj, methodID, ap);
    va_end(ap);
    return result.d;
  }

  static jdouble CallNonvirtualDoubleMethodV(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID methodID, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(ts, obj, methodID, args).d;
  }

  static jdouble CallNonvirtualDoubleMethodA(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID methodID, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(ts, obj, methodID, args).d;
  }

  static void CallNonvirtualVoidMethod(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID methodID, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, methodID);
    InvokeWithVarArgs(ts, obj, methodID, ap);
    va_end(ap);
  }

  static void CallNonvirtualVoidMethodV(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID methodID, va_list args) {
    ScopedJniThreadState ts(env);
    InvokeWithVarArgs(ts, obj, methodID, args);
  }

  static void CallNonvirtualVoidMethodA(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID methodID, jvalue*  args) {
    ScopedJniThreadState ts(env);
    InvokeWithJValues(ts, obj, methodID, args);
  }

  static jfieldID GetFieldID(JNIEnv* env,
      jclass c, const char* name, const char* sig) {
    ScopedJniThreadState ts(env);
    return FindFieldID(ts, c, name, sig, false);
  }


  static jfieldID GetStaticFieldID(JNIEnv* env,
      jclass c, const char* name, const char* sig) {
    ScopedJniThreadState ts(env);
    return FindFieldID(ts, c, name, sig, true);
  }

  static jobject GetObjectField(JNIEnv* env, jobject obj, jfieldID fieldID) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return NULL;
  }

  static jboolean GetBooleanField(JNIEnv* env, jobject obj, jfieldID fieldID) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return JNI_FALSE;
  }

  static jbyte GetByteField(JNIEnv* env, jobject obj, jfieldID fieldID) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static jchar GetCharField(JNIEnv* env, jobject obj, jfieldID fieldID) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static jshort GetShortField(JNIEnv* env, jobject obj, jfieldID fieldID) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static jint GetIntField(JNIEnv* env, jobject obj, jfieldID fieldID) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static jlong GetLongField(JNIEnv* env, jobject obj, jfieldID fieldID) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static jfloat GetFloatField(JNIEnv* env, jobject obj, jfieldID fieldID) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static jdouble GetDoubleField(JNIEnv* env, jobject obj, jfieldID fieldID) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static void SetObjectField(JNIEnv* env, jobject obj, jfieldID fieldID, jobject val) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void SetBooleanField(JNIEnv* env, jobject obj, jfieldID fieldID, jboolean val) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void SetByteField(JNIEnv* env, jobject obj, jfieldID fieldID, jbyte val) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void SetCharField(JNIEnv* env, jobject obj, jfieldID fieldID, jchar val) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void SetShortField(JNIEnv* env, jobject obj, jfieldID fieldID, jshort val) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void SetIntField(JNIEnv* env, jobject obj, jfieldID fieldID, jint val) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void SetLongField(JNIEnv* env, jobject obj, jfieldID fieldID, jlong val) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void SetFloatField(JNIEnv* env, jobject obj, jfieldID fieldID, jfloat val) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void SetDoubleField(JNIEnv* env, jobject obj, jfieldID fieldID, jdouble val) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static jobject CallStaticObjectMethod(JNIEnv* env,
      jclass clazz, jmethodID methodID, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, methodID);
    JValue result = InvokeWithVarArgs(ts, NULL, methodID, ap);
    jobject local_result = AddLocalReference<jobject>(ts, result.l);
    va_end(ap);
    return local_result;
  }

  static jobject CallStaticObjectMethodV(JNIEnv* env,
      jclass clazz, jmethodID methodID, va_list args) {
    ScopedJniThreadState ts(env);
    JValue result = InvokeWithVarArgs(ts, NULL, methodID, args);
    return AddLocalReference<jobject>(ts, result.l);
  }

  static jobject CallStaticObjectMethodA(JNIEnv* env,
      jclass clazz, jmethodID methodID, jvalue* args) {
    ScopedJniThreadState ts(env);
    JValue result = InvokeWithJValues(ts, NULL, methodID, args);
    return AddLocalReference<jobject>(ts, result.l);
  }

  static jboolean CallStaticBooleanMethod(JNIEnv* env,
      jclass clazz, jmethodID methodID, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, methodID);
    JValue result = InvokeWithVarArgs(ts, NULL, methodID, ap);
    va_end(ap);
    return result.z;
  }

  static jboolean CallStaticBooleanMethodV(JNIEnv* env,
      jclass clazz, jmethodID methodID, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(ts, NULL, methodID, args).z;
  }

  static jboolean CallStaticBooleanMethodA(JNIEnv* env,
      jclass clazz, jmethodID methodID, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(ts, NULL, methodID, args).z;
  }

  static jbyte CallStaticByteMethod(JNIEnv* env, jclass clazz, jmethodID methodID, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, methodID);
    JValue result = InvokeWithVarArgs(ts, NULL, methodID, ap);
    va_end(ap);
    return result.b;
  }

  static jbyte CallStaticByteMethodV(JNIEnv* env,
      jclass clazz, jmethodID methodID, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(ts, NULL, methodID, args).b;
  }

  static jbyte CallStaticByteMethodA(JNIEnv* env,
      jclass clazz, jmethodID methodID, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(ts, NULL, methodID, args).b;
  }

  static jchar CallStaticCharMethod(JNIEnv* env, jclass clazz, jmethodID methodID, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, methodID);
    JValue result = InvokeWithVarArgs(ts, NULL, methodID, ap);
    va_end(ap);
    return result.c;
  }

  static jchar CallStaticCharMethodV(JNIEnv* env,
      jclass clazz, jmethodID methodID, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(ts, NULL, methodID, args).c;
  }

  static jchar CallStaticCharMethodA(JNIEnv* env,
      jclass clazz, jmethodID methodID, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(ts, NULL, methodID, args).c;
  }

  static jshort CallStaticShortMethod(JNIEnv* env, jclass clazz, jmethodID methodID, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, methodID);
    JValue result = InvokeWithVarArgs(ts, NULL, methodID, ap);
    va_end(ap);
    return result.s;
  }

  static jshort CallStaticShortMethodV(JNIEnv* env,
      jclass clazz, jmethodID methodID, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(ts, NULL, methodID, args).s;
  }

  static jshort CallStaticShortMethodA(JNIEnv* env,
      jclass clazz, jmethodID methodID, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(ts, NULL, methodID, args).s;
  }

  static jint CallStaticIntMethod(JNIEnv* env, jclass clazz, jmethodID methodID, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, methodID);
    JValue result = InvokeWithVarArgs(ts, NULL, methodID, ap);
    va_end(ap);
    return result.i;
  }

  static jint CallStaticIntMethodV(JNIEnv* env,
      jclass clazz, jmethodID methodID, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(ts, NULL, methodID, args).i;
  }

  static jint CallStaticIntMethodA(JNIEnv* env,
      jclass clazz, jmethodID methodID, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(ts, NULL, methodID, args).i;
  }

  static jlong CallStaticLongMethod(JNIEnv* env, jclass clazz, jmethodID methodID, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, methodID);
    JValue result = InvokeWithVarArgs(ts, NULL, methodID, ap);
    va_end(ap);
    return result.j;
  }

  static jlong CallStaticLongMethodV(JNIEnv* env,
      jclass clazz, jmethodID methodID, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(ts, NULL, methodID, args).j;
  }

  static jlong CallStaticLongMethodA(JNIEnv* env,
      jclass clazz, jmethodID methodID, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(ts, NULL, methodID, args).j;
  }

  static jfloat CallStaticFloatMethod(JNIEnv* env, jclass cls, jmethodID methodID, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, methodID);
    JValue result = InvokeWithVarArgs(ts, NULL, methodID, ap);
    va_end(ap);
    return result.f;
  }

  static jfloat CallStaticFloatMethodV(JNIEnv* env,
      jclass clazz, jmethodID methodID, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(ts, NULL, methodID, args).f;
  }

  static jfloat CallStaticFloatMethodA(JNIEnv* env,
      jclass clazz, jmethodID methodID, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(ts, NULL, methodID, args).f;
  }

  static jdouble CallStaticDoubleMethod(JNIEnv* env, jclass cls, jmethodID methodID, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, methodID);
    JValue result = InvokeWithVarArgs(ts, NULL, methodID, ap);
    va_end(ap);
    return result.d;
  }

  static jdouble CallStaticDoubleMethodV(JNIEnv* env,
      jclass clazz, jmethodID methodID, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(ts, NULL, methodID, args).d;
  }

  static jdouble CallStaticDoubleMethodA(JNIEnv* env,
      jclass clazz, jmethodID methodID, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(ts, NULL, methodID, args).d;
  }

  static void CallStaticVoidMethod(JNIEnv* env, jclass cls, jmethodID methodID, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, methodID);
    InvokeWithVarArgs(ts, NULL, methodID, ap);
    va_end(ap);
  }

  static void CallStaticVoidMethodV(JNIEnv* env,
      jclass cls, jmethodID methodID, va_list args) {
    ScopedJniThreadState ts(env);
    InvokeWithVarArgs(ts, NULL, methodID, args);
  }

  static void CallStaticVoidMethodA(JNIEnv* env,
      jclass cls, jmethodID methodID, jvalue* args) {
    ScopedJniThreadState ts(env);
    InvokeWithJValues(ts, NULL, methodID, args);
  }

  static jobject GetStaticObjectField(JNIEnv* env, jclass clazz, jfieldID fieldID) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return NULL;
  }

  static jboolean GetStaticBooleanField(JNIEnv* env, jclass clazz, jfieldID fieldID) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return JNI_FALSE;
  }

  static jbyte GetStaticByteField(JNIEnv* env, jclass clazz, jfieldID fieldID) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static jchar GetStaticCharField(JNIEnv* env, jclass clazz, jfieldID fieldID) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static jshort GetStaticShortField(JNIEnv* env, jclass clazz, jfieldID fieldID) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static jint GetStaticIntField(JNIEnv* env, jclass clazz, jfieldID fieldID) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static jlong GetStaticLongField(JNIEnv* env, jclass clazz, jfieldID fieldID) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static jfloat GetStaticFloatField(JNIEnv* env, jclass clazz, jfieldID fieldID) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static jdouble GetStaticDoubleField(JNIEnv* env, jclass clazz, jfieldID fieldID) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static void SetStaticObjectField(JNIEnv* env,
      jclass clazz, jfieldID fieldID, jobject value) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void SetStaticBooleanField(JNIEnv* env,
      jclass clazz, jfieldID fieldID, jboolean value) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void SetStaticByteField(JNIEnv* env,
      jclass clazz, jfieldID fieldID, jbyte value) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void SetStaticCharField(JNIEnv* env,
      jclass clazz, jfieldID fieldID, jchar value) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void SetStaticShortField(JNIEnv* env,
      jclass clazz, jfieldID fieldID, jshort value) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void SetStaticIntField(JNIEnv* env,
      jclass clazz, jfieldID fieldID, jint value) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void SetStaticLongField(JNIEnv* env,
      jclass clazz, jfieldID fieldID, jlong value) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void SetStaticFloatField(JNIEnv* env,
      jclass clazz, jfieldID fieldID, jfloat value) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void SetStaticDoubleField(JNIEnv* env,
      jclass clazz, jfieldID fieldID, jdouble value) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static jstring NewString(JNIEnv* env, const jchar* unicode, jsize len) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return NULL;
  }

  static jsize GetStringLength(JNIEnv* env, jstring str) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static const jchar* GetStringChars(JNIEnv* env, jstring str, jboolean* isCopy) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return NULL;
  }

  static void ReleaseStringChars(JNIEnv* env, jstring str, const jchar* chars) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static jstring NewStringUTF(JNIEnv* env, const char* utf) {
    ScopedJniThreadState ts(env);
    if (utf == NULL) {
      return NULL;
    }
    String* result = String::AllocFromModifiedUtf8(utf);
    return AddLocalReference<jstring>(ts, result);
  }

  static jsize GetStringUTFLength(JNIEnv* env, jstring str) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static const char* GetStringUTFChars(JNIEnv* env, jstring str, jboolean* isCopy) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return NULL;
  }

  static void ReleaseStringUTFChars(JNIEnv* env, jstring str, const char* chars) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static jsize GetArrayLength(JNIEnv* env, jarray array) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static jobject GetObjectArrayElement(JNIEnv* env, jobjectArray array, jsize index) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return NULL;
  }

  static void SetObjectArrayElement(JNIEnv* env,
      jobjectArray java_array, jsize index, jobject java_value) {
    ScopedJniThreadState ts(env);
    ObjectArray<Object>* array = Decode<ObjectArray<Object>*>(ts, java_array);
    Object* value = Decode<Object*>(ts, java_value);
    array->Set(index, value);
  }

  static jbooleanArray NewBooleanArray(JNIEnv* env, jsize length) {
    ScopedJniThreadState ts(env);
    return NewPrimitiveArray<jbooleanArray, BooleanArray>(ts, length);
  }

  static jbyteArray NewByteArray(JNIEnv* env, jsize length) {
    ScopedJniThreadState ts(env);
    return NewPrimitiveArray<jbyteArray, ByteArray>(ts, length);
  }

  static jcharArray NewCharArray(JNIEnv* env, jsize length) {
    ScopedJniThreadState ts(env);
    return NewPrimitiveArray<jcharArray, CharArray>(ts, length);
  }

  static jdoubleArray NewDoubleArray(JNIEnv* env, jsize length) {
    ScopedJniThreadState ts(env);
    return NewPrimitiveArray<jdoubleArray, DoubleArray>(ts, length);
  }

  static jfloatArray NewFloatArray(JNIEnv* env, jsize length) {
    ScopedJniThreadState ts(env);
    return NewPrimitiveArray<jfloatArray, FloatArray>(ts, length);
  }

  static jintArray NewIntArray(JNIEnv* env, jsize length) {
    ScopedJniThreadState ts(env);
    return NewPrimitiveArray<jintArray, IntArray>(ts, length);
  }

  static jlongArray NewLongArray(JNIEnv* env, jsize length) {
    ScopedJniThreadState ts(env);
    return NewPrimitiveArray<jlongArray, LongArray>(ts, length);
  }

  static jobjectArray NewObjectArray(JNIEnv* env, jsize length, jclass element_jclass, jobject initial_element) {
    ScopedJniThreadState ts(env);
    CHECK_GE(length, 0); // TODO: ReportJniError

    // Compute the array class corresponding to the given element class.
    Class* element_class = Decode<Class*>(ts, element_jclass);
    std::string descriptor;
    descriptor += "[";
    descriptor += element_class->GetDescriptor().ToString();

    // Find the class.
    ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
    // TODO: need to get the appropriate ClassLoader.
    Class* array_class = class_linker->FindClass(descriptor, NULL);
    if (array_class == NULL) {
      return NULL;
    }

    ObjectArray<Object>* result = ObjectArray<Object>::Alloc(array_class, length);
    CHECK(initial_element == NULL);  // TODO: support initial_element
    return AddLocalReference<jobjectArray>(ts, result);
  }

  static jshortArray NewShortArray(JNIEnv* env, jsize length) {
    ScopedJniThreadState ts(env);
    return NewPrimitiveArray<jshortArray, ShortArray>(ts, length);
  }

  static jboolean* GetBooleanArrayElements(JNIEnv* env,
      jbooleanArray array, jboolean* isCopy) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return NULL;
  }

  static jbyte* GetByteArrayElements(JNIEnv* env, jbyteArray array, jboolean* isCopy) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return NULL;
  }

  static jchar* GetCharArrayElements(JNIEnv* env, jcharArray array, jboolean* isCopy) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return NULL;
  }

  static jshort* GetShortArrayElements(JNIEnv* env,
      jshortArray array, jboolean* isCopy) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return NULL;
  }

  static jint* GetIntArrayElements(JNIEnv* env, jintArray array, jboolean* isCopy) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return NULL;
  }

  static jlong* GetLongArrayElements(JNIEnv* env, jlongArray array, jboolean* isCopy) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return NULL;
  }

  static jfloat* GetFloatArrayElements(JNIEnv* env,
      jfloatArray array, jboolean* isCopy) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return NULL;
  }

  static jdouble* GetDoubleArrayElements(JNIEnv* env,
      jdoubleArray array, jboolean* isCopy) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return NULL;
  }

  static void ReleaseBooleanArrayElements(JNIEnv* env,
      jbooleanArray array, jboolean* elems, jint mode) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void ReleaseByteArrayElements(JNIEnv* env,
      jbyteArray array, jbyte* elems, jint mode) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void ReleaseCharArrayElements(JNIEnv* env,
      jcharArray array, jchar* elems, jint mode) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void ReleaseShortArrayElements(JNIEnv* env,
      jshortArray array, jshort* elems, jint mode) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void ReleaseIntArrayElements(JNIEnv* env,
      jintArray array, jint* elems, jint mode) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void ReleaseLongArrayElements(JNIEnv* env,
      jlongArray array, jlong* elems, jint mode) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void ReleaseFloatArrayElements(JNIEnv* env,
      jfloatArray array, jfloat* elems, jint mode) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void ReleaseDoubleArrayElements(JNIEnv* env,
      jdoubleArray array, jdouble* elems, jint mode) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void GetBooleanArrayRegion(JNIEnv* env,
      jbooleanArray array, jsize start, jsize l, jboolean* buf) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void GetByteArrayRegion(JNIEnv* env,
      jbyteArray array, jsize start, jsize len, jbyte* buf) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void GetCharArrayRegion(JNIEnv* env,
      jcharArray array, jsize start, jsize len, jchar* buf) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void GetShortArrayRegion(JNIEnv* env,
      jshortArray array, jsize start, jsize len, jshort* buf) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void GetIntArrayRegion(JNIEnv* env,
      jintArray array, jsize start, jsize len, jint* buf) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void GetLongArrayRegion(JNIEnv* env,
      jlongArray array, jsize start, jsize len, jlong* buf) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void GetFloatArrayRegion(JNIEnv* env,
      jfloatArray array, jsize start, jsize len, jfloat* buf) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void GetDoubleArrayRegion(JNIEnv* env,
      jdoubleArray array, jsize start, jsize len, jdouble* buf) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void SetBooleanArrayRegion(JNIEnv* env,
      jbooleanArray array, jsize start, jsize l, const jboolean* buf) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void SetByteArrayRegion(JNIEnv* env,
      jbyteArray array, jsize start, jsize len, const jbyte* buf) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void SetCharArrayRegion(JNIEnv* env,
      jcharArray array, jsize start, jsize len, const jchar* buf) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void SetShortArrayRegion(JNIEnv* env,
      jshortArray array, jsize start, jsize len, const jshort* buf) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void SetIntArrayRegion(JNIEnv* env,
      jintArray array, jsize start, jsize len, const jint* buf) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void SetLongArrayRegion(JNIEnv* env,
      jlongArray array, jsize start, jsize len, const jlong* buf) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void SetFloatArrayRegion(JNIEnv* env,
      jfloatArray array, jsize start, jsize len, const jfloat* buf) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void SetDoubleArrayRegion(JNIEnv* env,
      jdoubleArray array, jsize start, jsize len, const jdouble* buf) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static jint RegisterNatives(JNIEnv* env,
      jclass clazz, const JNINativeMethod* methods, jint nMethods) {
    ScopedJniThreadState ts(env);
    Class* klass = Decode<Class*>(ts, clazz);
    for(int i = 0; i < nMethods; i++) {
      const char* name = methods[i].name;
      const char* sig = methods[i].signature;

      if (*sig == '!') {
        // TODO: fast jni. it's too noisy to log all these.
        ++sig;
      }

      Method* method = klass->FindDirectMethod(name, sig);
      if (method == NULL) {
        method = klass->FindVirtualMethod(name, sig);
      }
      if (method == NULL) {
        Thread* self = Thread::Current();
        std::string class_name = klass->GetDescriptor().ToString();
        // TODO: pretty print method names through a single routine
        self->ThrowNewException("Ljava/lang/NoSuchMethodError;",
            "no method \"%s.%s%s\"",
            class_name.c_str(), name, sig);
        return JNI_ERR;
      } else if (!method->IsNative()) {
        Thread* self = Thread::Current();
        std::string class_name = klass->GetDescriptor().ToString();
        // TODO: pretty print method names through a single routine
        self->ThrowNewException("Ljava/lang/NoSuchMethodError;",
            "method \"%s.%s%s\" is not native",
            class_name.c_str(), name, sig);
        return JNI_ERR;
      }
      method->RegisterNative(methods[i].fnPtr);
    }
    return JNI_OK;
  }

  static jint UnregisterNatives(JNIEnv* env, jclass clazz) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static jint MonitorEnter(JNIEnv* env, jobject obj) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(WARNING);
    return 0;
  }

  static jint MonitorExit(JNIEnv* env, jobject obj) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(WARNING);
    return 0;
  }

  static jint GetJavaVM(JNIEnv* env, JavaVM** vm) {
    ScopedJniThreadState ts(env);
    Runtime* runtime = Runtime::Current();
    if (runtime != NULL) {
      *vm = reinterpret_cast<JavaVM*>(runtime->GetJavaVM());
    } else {
      *vm = NULL;
    }
    return (*vm != NULL) ? JNI_OK : JNI_ERR;
  }

  static void GetStringRegion(JNIEnv* env,
      jstring str, jsize start, jsize len, jchar* buf) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void GetStringUTFRegion(JNIEnv* env,
      jstring str, jsize start, jsize len, char* buf) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static void* GetPrimitiveArrayCritical(JNIEnv* env,
      jarray array, jboolean* isCopy) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return NULL;
  }

  static void ReleasePrimitiveArrayCritical(JNIEnv* env,
      jarray array, void* carray, jint mode) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static const jchar* GetStringCritical(JNIEnv* env, jstring s, jboolean* isCopy) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return NULL;
  }

  static void ReleaseStringCritical(JNIEnv* env, jstring s, const jchar* cstr) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
  }

  static jboolean ExceptionCheck(JNIEnv* env) {
    ScopedJniThreadState ts(env);
    return ts.Self()->IsExceptionPending() ? JNI_TRUE : JNI_FALSE;
  }

  static jobject NewDirectByteBuffer(JNIEnv* env, void* address, jlong capacity) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return NULL;
  }

  static void* GetDirectBufferAddress(JNIEnv* env, jobject buf) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return NULL;
  }

  static jlong GetDirectBufferCapacity(JNIEnv* env, jobject buf) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return 0;
  }

  static jobjectRefType GetObjectRefType(JNIEnv* env, jobject jobj) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(FATAL);
    return JNIInvalidRefType;
  }
};

static const struct JNINativeInterface gNativeInterface = {
  NULL,  // reserved0.
  NULL,  // reserved1.
  NULL,  // reserved2.
  NULL,  // reserved3.
  JNI::GetVersion,
  JNI::DefineClass,
  JNI::FindClass,
  JNI::FromReflectedMethod,
  JNI::FromReflectedField,
  JNI::ToReflectedMethod,
  JNI::GetSuperclass,
  JNI::IsAssignableFrom,
  JNI::ToReflectedField,
  JNI::Throw,
  JNI::ThrowNew,
  JNI::ExceptionOccurred,
  JNI::ExceptionDescribe,
  JNI::ExceptionClear,
  JNI::FatalError,
  JNI::PushLocalFrame,
  JNI::PopLocalFrame,
  JNI::NewGlobalRef,
  JNI::DeleteGlobalRef,
  JNI::DeleteLocalRef,
  JNI::IsSameObject,
  JNI::NewLocalRef,
  JNI::EnsureLocalCapacity,
  JNI::AllocObject,
  JNI::NewObject,
  JNI::NewObjectV,
  JNI::NewObjectA,
  JNI::GetObjectClass,
  JNI::IsInstanceOf,
  JNI::GetMethodID,
  JNI::CallObjectMethod,
  JNI::CallObjectMethodV,
  JNI::CallObjectMethodA,
  JNI::CallBooleanMethod,
  JNI::CallBooleanMethodV,
  JNI::CallBooleanMethodA,
  JNI::CallByteMethod,
  JNI::CallByteMethodV,
  JNI::CallByteMethodA,
  JNI::CallCharMethod,
  JNI::CallCharMethodV,
  JNI::CallCharMethodA,
  JNI::CallShortMethod,
  JNI::CallShortMethodV,
  JNI::CallShortMethodA,
  JNI::CallIntMethod,
  JNI::CallIntMethodV,
  JNI::CallIntMethodA,
  JNI::CallLongMethod,
  JNI::CallLongMethodV,
  JNI::CallLongMethodA,
  JNI::CallFloatMethod,
  JNI::CallFloatMethodV,
  JNI::CallFloatMethodA,
  JNI::CallDoubleMethod,
  JNI::CallDoubleMethodV,
  JNI::CallDoubleMethodA,
  JNI::CallVoidMethod,
  JNI::CallVoidMethodV,
  JNI::CallVoidMethodA,
  JNI::CallNonvirtualObjectMethod,
  JNI::CallNonvirtualObjectMethodV,
  JNI::CallNonvirtualObjectMethodA,
  JNI::CallNonvirtualBooleanMethod,
  JNI::CallNonvirtualBooleanMethodV,
  JNI::CallNonvirtualBooleanMethodA,
  JNI::CallNonvirtualByteMethod,
  JNI::CallNonvirtualByteMethodV,
  JNI::CallNonvirtualByteMethodA,
  JNI::CallNonvirtualCharMethod,
  JNI::CallNonvirtualCharMethodV,
  JNI::CallNonvirtualCharMethodA,
  JNI::CallNonvirtualShortMethod,
  JNI::CallNonvirtualShortMethodV,
  JNI::CallNonvirtualShortMethodA,
  JNI::CallNonvirtualIntMethod,
  JNI::CallNonvirtualIntMethodV,
  JNI::CallNonvirtualIntMethodA,
  JNI::CallNonvirtualLongMethod,
  JNI::CallNonvirtualLongMethodV,
  JNI::CallNonvirtualLongMethodA,
  JNI::CallNonvirtualFloatMethod,
  JNI::CallNonvirtualFloatMethodV,
  JNI::CallNonvirtualFloatMethodA,
  JNI::CallNonvirtualDoubleMethod,
  JNI::CallNonvirtualDoubleMethodV,
  JNI::CallNonvirtualDoubleMethodA,
  JNI::CallNonvirtualVoidMethod,
  JNI::CallNonvirtualVoidMethodV,
  JNI::CallNonvirtualVoidMethodA,
  JNI::GetFieldID,
  JNI::GetObjectField,
  JNI::GetBooleanField,
  JNI::GetByteField,
  JNI::GetCharField,
  JNI::GetShortField,
  JNI::GetIntField,
  JNI::GetLongField,
  JNI::GetFloatField,
  JNI::GetDoubleField,
  JNI::SetObjectField,
  JNI::SetBooleanField,
  JNI::SetByteField,
  JNI::SetCharField,
  JNI::SetShortField,
  JNI::SetIntField,
  JNI::SetLongField,
  JNI::SetFloatField,
  JNI::SetDoubleField,
  JNI::GetStaticMethodID,
  JNI::CallStaticObjectMethod,
  JNI::CallStaticObjectMethodV,
  JNI::CallStaticObjectMethodA,
  JNI::CallStaticBooleanMethod,
  JNI::CallStaticBooleanMethodV,
  JNI::CallStaticBooleanMethodA,
  JNI::CallStaticByteMethod,
  JNI::CallStaticByteMethodV,
  JNI::CallStaticByteMethodA,
  JNI::CallStaticCharMethod,
  JNI::CallStaticCharMethodV,
  JNI::CallStaticCharMethodA,
  JNI::CallStaticShortMethod,
  JNI::CallStaticShortMethodV,
  JNI::CallStaticShortMethodA,
  JNI::CallStaticIntMethod,
  JNI::CallStaticIntMethodV,
  JNI::CallStaticIntMethodA,
  JNI::CallStaticLongMethod,
  JNI::CallStaticLongMethodV,
  JNI::CallStaticLongMethodA,
  JNI::CallStaticFloatMethod,
  JNI::CallStaticFloatMethodV,
  JNI::CallStaticFloatMethodA,
  JNI::CallStaticDoubleMethod,
  JNI::CallStaticDoubleMethodV,
  JNI::CallStaticDoubleMethodA,
  JNI::CallStaticVoidMethod,
  JNI::CallStaticVoidMethodV,
  JNI::CallStaticVoidMethodA,
  JNI::GetStaticFieldID,
  JNI::GetStaticObjectField,
  JNI::GetStaticBooleanField,
  JNI::GetStaticByteField,
  JNI::GetStaticCharField,
  JNI::GetStaticShortField,
  JNI::GetStaticIntField,
  JNI::GetStaticLongField,
  JNI::GetStaticFloatField,
  JNI::GetStaticDoubleField,
  JNI::SetStaticObjectField,
  JNI::SetStaticBooleanField,
  JNI::SetStaticByteField,
  JNI::SetStaticCharField,
  JNI::SetStaticShortField,
  JNI::SetStaticIntField,
  JNI::SetStaticLongField,
  JNI::SetStaticFloatField,
  JNI::SetStaticDoubleField,
  JNI::NewString,
  JNI::GetStringLength,
  JNI::GetStringChars,
  JNI::ReleaseStringChars,
  JNI::NewStringUTF,
  JNI::GetStringUTFLength,
  JNI::GetStringUTFChars,
  JNI::ReleaseStringUTFChars,
  JNI::GetArrayLength,
  JNI::NewObjectArray,
  JNI::GetObjectArrayElement,
  JNI::SetObjectArrayElement,
  JNI::NewBooleanArray,
  JNI::NewByteArray,
  JNI::NewCharArray,
  JNI::NewShortArray,
  JNI::NewIntArray,
  JNI::NewLongArray,
  JNI::NewFloatArray,
  JNI::NewDoubleArray,
  JNI::GetBooleanArrayElements,
  JNI::GetByteArrayElements,
  JNI::GetCharArrayElements,
  JNI::GetShortArrayElements,
  JNI::GetIntArrayElements,
  JNI::GetLongArrayElements,
  JNI::GetFloatArrayElements,
  JNI::GetDoubleArrayElements,
  JNI::ReleaseBooleanArrayElements,
  JNI::ReleaseByteArrayElements,
  JNI::ReleaseCharArrayElements,
  JNI::ReleaseShortArrayElements,
  JNI::ReleaseIntArrayElements,
  JNI::ReleaseLongArrayElements,
  JNI::ReleaseFloatArrayElements,
  JNI::ReleaseDoubleArrayElements,
  JNI::GetBooleanArrayRegion,
  JNI::GetByteArrayRegion,
  JNI::GetCharArrayRegion,
  JNI::GetShortArrayRegion,
  JNI::GetIntArrayRegion,
  JNI::GetLongArrayRegion,
  JNI::GetFloatArrayRegion,
  JNI::GetDoubleArrayRegion,
  JNI::SetBooleanArrayRegion,
  JNI::SetByteArrayRegion,
  JNI::SetCharArrayRegion,
  JNI::SetShortArrayRegion,
  JNI::SetIntArrayRegion,
  JNI::SetLongArrayRegion,
  JNI::SetFloatArrayRegion,
  JNI::SetDoubleArrayRegion,
  JNI::RegisterNatives,
  JNI::UnregisterNatives,
  JNI::MonitorEnter,
  JNI::MonitorExit,
  JNI::GetJavaVM,
  JNI::GetStringRegion,
  JNI::GetStringUTFRegion,
  JNI::GetPrimitiveArrayCritical,
  JNI::ReleasePrimitiveArrayCritical,
  JNI::GetStringCritical,
  JNI::ReleaseStringCritical,
  JNI::NewWeakGlobalRef,
  JNI::DeleteWeakGlobalRef,
  JNI::ExceptionCheck,
  JNI::NewDirectByteBuffer,
  JNI::GetDirectBufferAddress,
  JNI::GetDirectBufferCapacity,
  JNI::GetObjectRefType,
};

static const size_t kMonitorsInitial = 32; // Arbitrary.
static const size_t kMonitorsMax = 4096; // Arbitrary sanity check.

static const size_t kLocalsInitial = 64; // Arbitrary.
static const size_t kLocalsMax = 512; // Arbitrary sanity check.

JNIEnvExt::JNIEnvExt(Thread* self, bool check_jni)
    : fns(&gNativeInterface),
      self(self),
      check_jni(check_jni),
      critical(false),
      monitors("monitors", kMonitorsInitial, kMonitorsMax),
      locals(kLocalsInitial, kLocalsMax, kLocal) {
}

// JNI Invocation interface.

extern "C" jint JNI_CreateJavaVM(JavaVM** p_vm, void** p_env, void* vm_args) {
  const JavaVMInitArgs* args = static_cast<JavaVMInitArgs*>(vm_args);
  if (args->version < JNI_VERSION_1_2) {
    return JNI_EVERSION;
  }
  Runtime::Options options;
  for (int i = 0; i < args->nOptions; ++i) {
    JavaVMOption* option = &args->options[i];
    options.push_back(std::make_pair(StringPiece(option->optionString),
                                     option->extraInfo));
  }
  bool ignore_unrecognized = args->ignoreUnrecognized;
  Runtime* runtime = Runtime::Create(options, ignore_unrecognized);
  if (runtime == NULL) {
    return JNI_ERR;
  } else {
    *p_env = reinterpret_cast<JNIEnv*>(Thread::Current()->GetJniEnv());
    *p_vm = reinterpret_cast<JavaVM*>(runtime->GetJavaVM());
    return JNI_OK;
  }
}

extern "C" jint JNI_GetCreatedJavaVMs(JavaVM** vms, jsize, jsize* vm_count) {
  Runtime* runtime = Runtime::Current();
  if (runtime == NULL) {
    *vm_count = 0;
  } else {
    *vm_count = 1;
    vms[0] = reinterpret_cast<JavaVM*>(runtime->GetJavaVM());
  }
  return JNI_OK;
}

// Historically unsupported.
extern "C" jint JNI_GetDefaultJavaVMInitArgs(void* vm_args) {
  return JNI_ERR;
}

class JII {
 public:
  static jint DestroyJavaVM(JavaVM* vm) {
    if (vm == NULL) {
      return JNI_ERR;
    } else {
      JavaVMExt* raw_vm = reinterpret_cast<JavaVMExt*>(vm);
      delete raw_vm->runtime;
      return JNI_OK;
    }
  }

  static jint AttachCurrentThread(JavaVM* vm, JNIEnv** p_env, void* thr_args) {
    if (vm == NULL || p_env == NULL) {
      return JNI_ERR;
    }
    JavaVMExt* raw_vm = reinterpret_cast<JavaVMExt*>(vm);
    Runtime* runtime = raw_vm->runtime;
    const char* name = NULL;
    if (thr_args != NULL) {
      // TODO: check version
      name = static_cast<JavaVMAttachArgs*>(thr_args)->name;
      // TODO: thread group
    }
    bool success = runtime->AttachCurrentThread(name, p_env);
    if (!success) {
      return JNI_ERR;
    } else {
      return JNI_OK;
    }
  }

  static jint AttachCurrentThreadAsDaemon(JavaVM* vm, JNIEnv** p_env, void* thr_args) {
    if (vm == NULL || p_env == NULL) {
      return JNI_ERR;
    }
    JavaVMExt* raw_vm = reinterpret_cast<JavaVMExt*>(vm);
    Runtime* runtime = raw_vm->runtime;
    const char* name = NULL;
    if (thr_args != NULL) {
      // TODO: check version
      name = static_cast<JavaVMAttachArgs*>(thr_args)->name;
      // TODO: thread group
    }
    bool success = runtime->AttachCurrentThreadAsDaemon(name, p_env);
    if (!success) {
      return JNI_ERR;
    } else {
      return JNI_OK;
    }
  }

  static jint DetachCurrentThread(JavaVM* vm) {
    if (vm == NULL) {
      return JNI_ERR;
    } else {
      JavaVMExt* raw_vm = reinterpret_cast<JavaVMExt*>(vm);
      Runtime* runtime = raw_vm->runtime;
      runtime->DetachCurrentThread();
      return JNI_OK;
    }
  }

  static jint GetEnv(JavaVM* vm, void** env, jint version) {
    if (version < JNI_VERSION_1_1 || version > JNI_VERSION_1_6) {
      return JNI_EVERSION;
    }
    if (vm == NULL || env == NULL) {
      return JNI_ERR;
    }
    Thread* thread = Thread::Current();
    if (thread == NULL) {
      *env = NULL;
      return JNI_EDETACHED;
    }
    *env = thread->GetJniEnv();
    return JNI_OK;
  }
};

struct JNIInvokeInterface gInvokeInterface = {
  NULL,  // reserved0
  NULL,  // reserved1
  NULL,  // reserved2
  JII::DestroyJavaVM,
  JII::AttachCurrentThread,
  JII::DetachCurrentThread,
  JII::GetEnv,
  JII::AttachCurrentThreadAsDaemon
};

static const size_t kPinTableInitialSize = 16;
static const size_t kPinTableMaxSize = 1024;

static const size_t kGlobalsInitial = 512; // Arbitrary.
static const size_t kGlobalsMax = 51200; // Arbitrary sanity check.

static const size_t kWeakGlobalsInitial = 16; // Arbitrary.
static const size_t kWeakGlobalsMax = 51200; // Arbitrary sanity check.

JavaVMExt::JavaVMExt(Runtime* runtime, bool check_jni, bool verbose_jni)
    : fns(&gInvokeInterface),
      runtime(runtime),
      check_jni(check_jni),
      verbose_jni(verbose_jni),
      pin_table("pin table", kPinTableInitialSize, kPinTableMaxSize),
      globals_lock(Mutex::Create("JNI global reference table lock")),
      globals(kGlobalsInitial, kGlobalsMax, kGlobal),
      weak_globals_lock(Mutex::Create("JNI weak global reference table lock")),
      weak_globals(kWeakGlobalsInitial, kWeakGlobalsMax, kWeakGlobal) {
}

JavaVMExt::~JavaVMExt() {
  delete globals_lock;
  delete weak_globals_lock;
}

/*
 * Load native code from the specified absolute pathname.  Per the spec,
 * if we've already loaded a library with the specified pathname, we
 * return without doing anything.
 *
 * TODO? for better results we should absolutify the pathname.  For fully
 * correct results we should stat to get the inode and compare that.  The
 * existing implementation is fine so long as everybody is using
 * System.loadLibrary.
 *
 * The library will be associated with the specified class loader.  The JNI
 * spec says we can't load the same library into more than one class loader.
 *
 * Returns "true" on success. On failure, sets *detail to a
 * human-readable description of the error or NULL if no detail is
 * available; ownership of the string is transferred to the caller.
 */
bool JavaVMExt::LoadNativeLibrary(const std::string& path, Object* class_loader, char** detail) {
  *detail = NULL;

  // See if we've already loaded this library.  If we have, and the class loader
  // matches, return successfully without doing anything.
  SharedLibrary* library = libraries[path];
  if (library != NULL) {
    if (library->GetClassLoader() != class_loader) {
      LOG(WARNING) << "Shared library \"" << path << "\" already opened by "
                   << "ClassLoader " << library->GetClassLoader() << "; "
                   << "can't open in " << class_loader;
      *detail = strdup("already opened by different ClassLoader");
      return false;
    }
    if (verbose_jni) {
      LOG(INFO) << "[Shared library \"" << path << "\" already loaded in "
                << "ClassLoader " << class_loader << "]";
    }
    if (!library->CheckOnLoadResult(this)) {
      *detail = strdup("JNI_OnLoad failed before");
      return false;
    }
    return true;
  }

  // Open the shared library.  Because we're using a full path, the system
  // doesn't have to search through LD_LIBRARY_PATH.  (It may do so to
  // resolve this library's dependencies though.)

  // Failures here are expected when java.library.path has several entries
  // and we have to hunt for the lib.

  // The current version of the dynamic linker prints detailed information
  // about dlopen() failures.  Some things to check if the message is
  // cryptic:
  //   - make sure the library exists on the device
  //   - verify that the right path is being opened (the debug log message
  //     above can help with that)
  //   - check to see if the library is valid (e.g. not zero bytes long)
  //   - check config/prelink-linux-arm.map to ensure that the library
  //     is listed and is not being overrun by the previous entry (if
  //     loading suddenly stops working on a prelinked library, this is
  //     a good one to check)
  //   - write a trivial app that calls sleep() then dlopen(), attach
  //     to it with "strace -p <pid>" while it sleeps, and watch for
  //     attempts to open nonexistent dependent shared libs

  // TODO: automate some of these checks!

  // This can execute slowly for a large library on a busy system, so we
  // want to switch from RUNNING to VMWAIT while it executes.  This allows
  // the GC to ignore us.
  Thread* self = Thread::Current();
  Thread::State old_state = self->GetState();
  self->SetState(Thread::kWaiting); // TODO: VMWAIT
  void* handle = dlopen(path.c_str(), RTLD_LAZY);
  self->SetState(old_state);

  if (verbose_jni) {
    LOG(INFO) << "[Call to dlopen(\"" << path << "\") returned " << handle << "]";
  }

  if (handle == NULL) {
    *detail = strdup(dlerror());
    return false;
  }

  // Create a new entry.
  library = new SharedLibrary(path, handle, class_loader);
  UNIMPLEMENTED(ERROR) << "missing pthread_cond_init";
  // pthread_cond_init(&library->onLoadCond, NULL);

  libraries[path] = library;

  //  if (pNewEntry != pActualEntry) {
  //    LOG(INFO) << "WOW: we lost a race to add a shared library (\"" << path << "\" ClassLoader=" << class_loader <<")";
  //    freeSharedLibEntry(pNewEntry);
  //    return CheckOnLoadResult(this, pActualEntry);
  //  } else
  {
    if (verbose_jni) {
      LOG(INFO) << "[Added shared library \"" << path << "\" for ClassLoader " << class_loader << "]";
    }

    bool result = true;
    void* sym = dlsym(handle, "JNI_OnLoad");
    if (sym == NULL) {
      if (verbose_jni) {
        LOG(INFO) << "[No JNI_OnLoad found in \"" << path << "\"]";
      }
    } else {
      // Call JNI_OnLoad.  We have to override the current class
      // loader, which will always be "null" since the stuff at the
      // top of the stack is around Runtime.loadLibrary().  (See
      // the comments in the JNI FindClass function.)
      UNIMPLEMENTED(WARNING) << "need to override current class loader";
      typedef int (*JNI_OnLoadFn)(JavaVM*, void*);
      JNI_OnLoadFn jni_on_load = reinterpret_cast<JNI_OnLoadFn>(sym);
      //Object* prevOverride = self->classLoaderOverride;
      //self->classLoaderOverride = classLoader;

      old_state = self->GetState();
      self->SetState(Thread::kNative);
      if (verbose_jni) {
        LOG(INFO) << "[Calling JNI_OnLoad in \"" << path << "\"]";
      }
      int version = (*jni_on_load)(reinterpret_cast<JavaVM*>(this), NULL);
      self->SetState(old_state);

      UNIMPLEMENTED(WARNING) << "need to restore current class loader";
      //self->classLoaderOverride = prevOverride;

      if (version != JNI_VERSION_1_2 &&
      version != JNI_VERSION_1_4 &&
      version != JNI_VERSION_1_6) {
        LOG(WARNING) << "JNI_OnLoad in \"" << path << "\" returned "
                     << "bad version: " << version;
        // It's unwise to call dlclose() here, but we can mark it
        // as bad and ensure that future load attempts will fail.
        // We don't know how far JNI_OnLoad got, so there could
        // be some partially-initialized stuff accessible through
        // newly-registered native method calls.  We could try to
        // unregister them, but that doesn't seem worthwhile.
        result = false;
      } else {
        if (verbose_jni) {
          LOG(INFO) << "[Returned " << (result ? "successfully" : "failure")
                    << " from JNI_OnLoad in \"" << path << "\"]";
        }
      }
    }

    library->SetResult(result);
    return result;
  }
}

}  // namespace art
