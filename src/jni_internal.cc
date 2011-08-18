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

enum JNI_OnLoadState {
  kPending = 0,     /* initial state, must be zero */
  kFailed,
  kOkay,
};

struct SharedLibrary {
  SharedLibrary() : jni_on_load_lock(Mutex::Create("JNI_OnLoad lock")) {
  }

  ~SharedLibrary() {
    delete jni_on_load_lock;
  }

  // Path to library "/system/lib/libjni.so".
  std::string path;

  // The void* returned by dlopen(3).
  void* handle;

  // The ClassLoader this library is associated with.
  Object* class_loader;

  // Guards remaining items.
  Mutex* jni_on_load_lock;
  // Wait for JNI_OnLoad in other thread.
  pthread_cond_t  jni_on_load_cond;
  // Recursive invocation guard.
  uint32_t jni_on_load_tid;
  // Result of earlier JNI_OnLoad call.
  JNI_OnLoadState jni_on_load_result;
};

/*
 * Check the result of an earlier call to JNI_OnLoad on this library.  If
 * the call has not yet finished in another thread, wait for it.
 */
bool CheckOnLoadResult(JavaVMExt* vm, SharedLibrary* library) {
  Thread* self = Thread::Current();
  if (library->jni_on_load_tid == self->GetId()) {
    // Check this so we don't end up waiting for ourselves.  We need
    // to return "true" so the caller can continue.
    LOG(INFO) << *self << " recursive attempt to load library "
              << "\"" << library->path << "\"";
    return true;
  }

  UNIMPLEMENTED(ERROR) << "need to pthread_cond_wait!";
  // MutexLock mu(library->jni_on_load_lock);
  while (library->jni_on_load_result == kPending) {
    if (vm->verbose_jni) {
      LOG(INFO) << "[" << *self << " waiting for \"" << library->path << "\" "
                << "JNI_OnLoad...]";
    }
    Thread::State old_state = self->GetState();
    self->SetState(Thread::kWaiting); // TODO: VMWAIT
    // pthread_cond_wait(&library->jni_on_load_cond, &library->jni_on_load_lock);
    self->SetState(old_state);
  }

  bool okay = (library->jni_on_load_result == kOkay);
  if (vm->verbose_jni) {
    LOG(INFO) << "[Earlier JNI_OnLoad for \"" << library->path << "\" "
              << (okay ? "succeeded" : "failed") << "]";
  }
  return okay;
}

typedef int (*JNI_OnLoadFn)(JavaVM*, void*);

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
    if (library->class_loader != class_loader) {
      LOG(WARNING) << "Shared library \"" << path << "\" already opened by "
                   << "ClassLoader " << library->class_loader << "; "
                   << "can't open in " << class_loader;
      *detail = strdup("already opened by different ClassLoader");
      return false;
    }
    if (verbose_jni) {
      LOG(INFO) << "[Shared library \"" << path << "\" already loaded in "
                << "ClassLoader " << class_loader << "]";
    }
    if (!CheckOnLoadResult(this, library)) {
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
  library = new SharedLibrary;
  library->path = path;
  library->handle = handle;
  library->class_loader = class_loader;
  UNIMPLEMENTED(ERROR) << "missing pthread_cond_init";
  // pthread_cond_init(&library->onLoadCond, NULL);
  library->jni_on_load_tid = self->GetId();

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

    library->jni_on_load_result = result ? kOkay : kFailed;
    library->jni_on_load_tid = 0;

    // Broadcast a wakeup to anybody sleeping on the condition variable.
    UNIMPLEMENTED(ERROR) << "missing pthread_cond_broadcast";
    // MutexLock mu(library->jni_on_load_lock);
    // pthread_cond_broadcast(&library->jni_on_load_cond);
    return result;
  }
}

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

void CreateInvokeStub(Assembler* assembler, Method* method);

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

JValue InvokeWithArgArray(ScopedJniThreadState& ts,
    Object* obj, jmethodID method_id, byte* args) {
  // TODO: DecodeReference
  Method* method = reinterpret_cast<Method*>(method_id);
  // Call the invoke stub associated with the method
  // Pass everything as arguments
  const Method::InvokeStub* stub = method->GetInvokeStub();
  CHECK(stub != NULL);
  JValue result;
  (*stub)(method, obj, ts.Self(), args, &result);
  return result;
}

JValue InvokeWithJValues(ScopedJniThreadState& ts,
    Object* obj, jmethodID method_id, jvalue* args) {
  Method* method = reinterpret_cast<Method*>(method_id);
  scoped_array<byte> arg_array(CreateArgArray(ts, method, args));
  return InvokeWithArgArray(ts, obj, method_id, arg_array.get());
}

JValue InvokeWithVarArgs(ScopedJniThreadState& ts,
    Object* obj, jmethodID method_id, va_list args) {
  Method* method = reinterpret_cast<Method*>(method_id);
  scoped_array<byte> arg_array(CreateArgArray(ts, method, args));
  return InvokeWithArgArray(ts, obj, method_id, arg_array.get());
}

jint GetVersion(JNIEnv* env) {
  ScopedJniThreadState ts(env);
  return JNI_VERSION_1_6;
}

jclass DefineClass(JNIEnv* env, const char*, jobject, const jbyte*, jsize) {
  ScopedJniThreadState ts(env);
  LOG(WARNING) << "JNI DefineClass is not supported";
  return NULL;
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

jclass FindClass(JNIEnv* env, const char* name) {
  ScopedJniThreadState ts(env);
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  std::string descriptor(NormalizeJniClassDescriptor(name));
  // TODO: need to get the appropriate ClassLoader.
  Class* c = class_linker->FindClass(descriptor, NULL);
  return AddLocalReference<jclass>(ts, c);
}

jmethodID FromReflectedMethod(JNIEnv* env, jobject method) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jfieldID FromReflectedField(JNIEnv* env, jobject field) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jobject ToReflectedMethod(JNIEnv* env, jclass cls,
    jmethodID methodID, jboolean isStatic) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jclass GetSuperclass(JNIEnv* env, jclass sub) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jboolean IsAssignableFrom(JNIEnv* env, jclass sub, jclass sup) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return JNI_FALSE;
}

jobject ToReflectedField(JNIEnv* env, jclass cls,
    jfieldID fieldID, jboolean isStatic) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jint Throw(JNIEnv* env, jthrowable obj) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jint ThrowNew(JNIEnv* env, jclass clazz, const char* msg) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jthrowable ExceptionOccurred(JNIEnv* env) {
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

void ExceptionDescribe(JNIEnv* env) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void ExceptionClear(JNIEnv* env) {
  ScopedJniThreadState ts(env);
  ts.Self()->ClearException();
}

void FatalError(JNIEnv* env, const char* msg) {
  ScopedJniThreadState ts(env);
  LOG(FATAL) << "JNI FatalError called: " << msg;
}

jint PushLocalFrame(JNIEnv* env, jint cap) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(WARNING) << "ignoring PushLocalFrame(" << cap << ")";
  return JNI_OK;
}

jobject PopLocalFrame(JNIEnv* env, jobject res) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(WARNING) << "ignoring PopLocalFrame " << res;
  return res;
}

jobject NewGlobalRef(JNIEnv* env, jobject obj) {
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

void DeleteGlobalRef(JNIEnv* env, jobject obj) {
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

jweak NewWeakGlobalRef(JNIEnv* env, jobject obj) {
  ScopedJniThreadState ts(env);
  if (obj == NULL) {
    return NULL;
  }

  JavaVMExt* vm = Runtime::Current()->GetJavaVM();
  IndirectReferenceTable& weak_globals = vm->weak_globals;
  MutexLock mu(vm->weak_globals_lock);
  IndirectRef ref = weak_globals.Add(IRT_FIRST_SEGMENT, Decode<Object*>(ts, obj));
  return reinterpret_cast<jobject>(ref);
}

void DeleteWeakGlobalRef(JNIEnv* env, jweak obj) {
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

jobject NewLocalRef(JNIEnv* env, jobject obj) {
  ScopedJniThreadState ts(env);
  if (obj == NULL) {
    return NULL;
  }

  IndirectReferenceTable& locals = ts.Env()->locals;

  uint32_t cookie = IRT_FIRST_SEGMENT; // TODO
  IndirectRef ref = locals.Add(cookie, Decode<Object*>(ts, obj));
  return reinterpret_cast<jobject>(ref);
}

void DeleteLocalRef(JNIEnv* env, jobject obj) {
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

jboolean IsSameObject(JNIEnv* env, jobject obj1, jobject obj2) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return JNI_FALSE;
}

jint EnsureLocalCapacity(JNIEnv* env, jint) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jobject AllocObject(JNIEnv* env, jclass clazz) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jobject NewObject(JNIEnv* env, jclass clazz, jmethodID methodID, ...) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jobject NewObjectV(JNIEnv* env,
    jclass clazz, jmethodID methodID, va_list args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jobject NewObjectA(JNIEnv* env,
    jclass clazz, jmethodID methodID, jvalue* args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jclass GetObjectClass(JNIEnv* env, jobject obj) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jboolean IsInstanceOf(JNIEnv* env, jobject jobj, jclass clazz) {
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

jmethodID GetMethodID(JNIEnv* env,
    jclass clazz, const char* name, const char* sig) {
  ScopedJniThreadState ts(env);
  Class* klass = Decode<Class*>(ts, clazz);
  if (!klass->IsInitialized()) {
    // TODO: initialize the class
  }
  Method* method = klass->FindVirtualMethod(name, sig);
  if (method == NULL) {
    // No virtual method matching the signature.  Search declared
    // private methods and constructors.
    method = klass->FindDeclaredDirectMethod(name, sig);
  }
  if (method == NULL) {
    Thread* self = Thread::Current();
    std::string class_name = klass->GetDescriptor().ToString();
    // TODO: pretty print method names through a single routine
    self->ThrowNewException("Ljava/lang/NoSuchMethodError;",
                            "no method \"%s.%s%s\"",
                            class_name.c_str(), name, sig);
    return NULL;
  } else if (method->IsStatic()) {
    Thread* self = Thread::Current();
    std::string class_name = klass->GetDescriptor().ToString();
    // TODO: pretty print method names through a single routine
    self->ThrowNewException("Ljava/lang/NoSuchMethodError;",
                            "method \"%s.%s%s\" is static",
                            class_name.c_str(), name, sig);
    return NULL;
  } else {
    // TODO: create a JNI weak global reference for method
    bool success = EnsureInvokeStub(method);
    if (!success) {
      // TODO: throw OutOfMemoryException
      return NULL;
    }
    return reinterpret_cast<jmethodID>(method);
  }
}

jobject CallObjectMethod(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jobject CallObjectMethodV(JNIEnv* env,
    jobject obj, jmethodID methodID, va_list args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jobject CallObjectMethodA(JNIEnv* env,
    jobject obj, jmethodID methodID, jvalue*  args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jboolean CallBooleanMethod(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return JNI_FALSE;
}

jboolean CallBooleanMethodV(JNIEnv* env,
    jobject obj, jmethodID methodID, va_list args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return JNI_FALSE;
}

jboolean CallBooleanMethodA(JNIEnv* env,
    jobject obj, jmethodID methodID, jvalue*  args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return JNI_FALSE;
}

jbyte CallByteMethod(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jbyte CallByteMethodV(JNIEnv* env,
    jobject obj, jmethodID methodID, va_list args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jbyte CallByteMethodA(JNIEnv* env,
    jobject obj, jmethodID methodID, jvalue* args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jchar CallCharMethod(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jchar CallCharMethodV(JNIEnv* env,
    jobject obj, jmethodID methodID, va_list args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jchar CallCharMethodA(JNIEnv* env,
    jobject obj, jmethodID methodID, jvalue* args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jshort CallShortMethod(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jshort CallShortMethodV(JNIEnv* env,
    jobject obj, jmethodID methodID, va_list args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jshort CallShortMethodA(JNIEnv* env,
    jobject obj, jmethodID methodID, jvalue* args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jint CallIntMethod(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jint CallIntMethodV(JNIEnv* env,
    jobject obj, jmethodID methodID, va_list args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jint CallIntMethodA(JNIEnv* env,
    jobject obj, jmethodID methodID, jvalue* args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jlong CallLongMethod(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jlong CallLongMethodV(JNIEnv* env,
    jobject obj, jmethodID methodID, va_list args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jlong CallLongMethodA(JNIEnv* env,
    jobject obj, jmethodID methodID, jvalue* args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jfloat CallFloatMethod(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jfloat CallFloatMethodV(JNIEnv* env,
    jobject obj, jmethodID methodID, va_list args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jfloat CallFloatMethodA(JNIEnv* env,
    jobject obj, jmethodID methodID, jvalue* args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jdouble CallDoubleMethod(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jdouble CallDoubleMethodV(JNIEnv* env,
    jobject obj, jmethodID methodID, va_list args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jdouble CallDoubleMethodA(JNIEnv* env,
    jobject obj, jmethodID methodID, jvalue* args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

void CallVoidMethod(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void CallVoidMethodV(JNIEnv* env, jobject obj,
    jmethodID methodID, va_list args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void CallVoidMethodA(JNIEnv* env, jobject obj,
    jmethodID methodID, jvalue*  args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

jobject CallNonvirtualObjectMethod(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, ...) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jobject CallNonvirtualObjectMethodV(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, va_list args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jobject CallNonvirtualObjectMethodA(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, jvalue*  args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jboolean CallNonvirtualBooleanMethod(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, ...) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return JNI_FALSE;
}

jboolean CallNonvirtualBooleanMethodV(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, va_list args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return JNI_FALSE;
}

jboolean CallNonvirtualBooleanMethodA(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, jvalue*  args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return JNI_FALSE;
}

jbyte CallNonvirtualByteMethod(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, ...) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jbyte CallNonvirtualByteMethodV(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, va_list args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jbyte CallNonvirtualByteMethodA(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, jvalue* args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jchar CallNonvirtualCharMethod(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, ...) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jchar CallNonvirtualCharMethodV(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, va_list args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jchar CallNonvirtualCharMethodA(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, jvalue* args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jshort CallNonvirtualShortMethod(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, ...) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jshort CallNonvirtualShortMethodV(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, va_list args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jshort CallNonvirtualShortMethodA(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, jvalue* args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jint CallNonvirtualIntMethod(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, ...) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jint CallNonvirtualIntMethodV(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, va_list args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jint CallNonvirtualIntMethodA(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, jvalue* args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jlong CallNonvirtualLongMethod(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, ...) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jlong CallNonvirtualLongMethodV(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, va_list args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jlong CallNonvirtualLongMethodA(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, jvalue* args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jfloat CallNonvirtualFloatMethod(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, ...) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jfloat CallNonvirtualFloatMethodV(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, va_list args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jfloat CallNonvirtualFloatMethodA(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, jvalue* args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jdouble CallNonvirtualDoubleMethod(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, ...) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jdouble CallNonvirtualDoubleMethodV(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, va_list args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jdouble CallNonvirtualDoubleMethodA(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, jvalue* args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

void CallNonvirtualVoidMethod(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, ...) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void CallNonvirtualVoidMethodV(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, va_list args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void CallNonvirtualVoidMethodA(JNIEnv* env,
    jobject obj, jclass clazz, jmethodID methodID, jvalue*  args) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

jfieldID GetFieldID(JNIEnv* env,
    jclass clazz, const char* name, const char* sig) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jobject GetObjectField(JNIEnv* env, jobject obj, jfieldID fieldID) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jboolean GetBooleanField(JNIEnv* env, jobject obj, jfieldID fieldID) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return JNI_FALSE;
}

jbyte GetByteField(JNIEnv* env, jobject obj, jfieldID fieldID) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jchar GetCharField(JNIEnv* env, jobject obj, jfieldID fieldID) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jshort GetShortField(JNIEnv* env, jobject obj, jfieldID fieldID) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jint GetIntField(JNIEnv* env, jobject obj, jfieldID fieldID) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jlong GetLongField(JNIEnv* env, jobject obj, jfieldID fieldID) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jfloat GetFloatField(JNIEnv* env, jobject obj, jfieldID fieldID) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jdouble GetDoubleField(JNIEnv* env, jobject obj, jfieldID fieldID) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

void SetObjectField(JNIEnv* env, jobject obj, jfieldID fieldID, jobject val) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void SetBooleanField(JNIEnv* env, jobject obj, jfieldID fieldID, jboolean val) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void SetByteField(JNIEnv* env, jobject obj, jfieldID fieldID, jbyte val) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void SetCharField(JNIEnv* env, jobject obj, jfieldID fieldID, jchar val) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void SetShortField(JNIEnv* env, jobject obj, jfieldID fieldID, jshort val) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void SetIntField(JNIEnv* env, jobject obj, jfieldID fieldID, jint val) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void SetLongField(JNIEnv* env, jobject obj, jfieldID fieldID, jlong val) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void SetFloatField(JNIEnv* env, jobject obj, jfieldID fieldID, jfloat val) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void SetDoubleField(JNIEnv* env, jobject obj, jfieldID fieldID, jdouble val) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

jmethodID GetStaticMethodID(JNIEnv* env,
    jclass clazz, const char* name, const char* sig) {
  ScopedJniThreadState ts(env);
  Class* klass = Decode<Class*>(ts, clazz);
  if (!klass->IsInitialized()) {
    // TODO: initialize the class
  }
  Method* method = klass->FindDirectMethod(name, sig);
  if (method == NULL) {
    Thread* self = Thread::Current();
    std::string class_name = klass->GetDescriptor().ToString();
    // TODO: pretty print method names through a single routine
    // TODO: may want to FindVirtualMethod to give more informative error
    // message here
    self->ThrowNewException("Ljava/lang/NoSuchMethodError;",
                            "no method \"%s.%s%s\"",
                            class_name.c_str(), name, sig);
    return NULL;
  } else if (!method->IsStatic()) {
    Thread* self = Thread::Current();
    std::string class_name = klass->GetDescriptor().ToString();
    // TODO: pretty print method names through a single routine
    self->ThrowNewException("Ljava/lang/NoSuchMethodError;",
                            "method \"%s.%s%s\" is not static",
                            class_name.c_str(), name, sig);
    return NULL;
  } else {
    // TODO: create a JNI weak global reference for method
    bool success = EnsureInvokeStub(method);
    if (!success) {
      // TODO: throw OutOfMemoryException
      return NULL;
    }
    return reinterpret_cast<jmethodID>(method);
  }
}

jobject CallStaticObjectMethod(JNIEnv* env,
    jclass clazz, jmethodID methodID, ...) {
  ScopedJniThreadState ts(env);
  va_list ap;
  va_start(ap, methodID);
  JValue result = InvokeWithVarArgs(ts, NULL, methodID, ap);
  return AddLocalReference<jobject>(ts, result.l);
}

jobject CallStaticObjectMethodV(JNIEnv* env,
    jclass clazz, jmethodID methodID, va_list args) {
  ScopedJniThreadState ts(env);
  JValue result = InvokeWithVarArgs(ts, NULL, methodID, args);
  return AddLocalReference<jobject>(ts, result.l);
}

jobject CallStaticObjectMethodA(JNIEnv* env,
    jclass clazz, jmethodID methodID, jvalue* args) {
  ScopedJniThreadState ts(env);
  JValue result = InvokeWithJValues(ts, NULL, methodID, args);
  return AddLocalReference<jobject>(ts, result.l);
}

jboolean CallStaticBooleanMethod(JNIEnv* env,
    jclass clazz, jmethodID methodID, ...) {
  ScopedJniThreadState ts(env);
  va_list ap;
  va_start(ap, methodID);
  return InvokeWithVarArgs(ts, NULL, methodID, ap).z;
}

jboolean CallStaticBooleanMethodV(JNIEnv* env,
    jclass clazz, jmethodID methodID, va_list args) {
  ScopedJniThreadState ts(env);
  return InvokeWithVarArgs(ts, NULL, methodID, args).z;
}

jboolean CallStaticBooleanMethodA(JNIEnv* env,
    jclass clazz, jmethodID methodID, jvalue* args) {
  ScopedJniThreadState ts(env);
  return InvokeWithJValues(ts, NULL, methodID, args).z;
}

jbyte CallStaticByteMethod(JNIEnv* env, jclass clazz, jmethodID methodID, ...) {
  ScopedJniThreadState ts(env);
  va_list ap;
  va_start(ap, methodID);
  return InvokeWithVarArgs(ts, NULL, methodID, ap).b;
}

jbyte CallStaticByteMethodV(JNIEnv* env,
    jclass clazz, jmethodID methodID, va_list args) {
  ScopedJniThreadState ts(env);
  return InvokeWithVarArgs(ts, NULL, methodID, args).b;
}

jbyte CallStaticByteMethodA(JNIEnv* env,
    jclass clazz, jmethodID methodID, jvalue* args) {
  ScopedJniThreadState ts(env);
  return InvokeWithJValues(ts, NULL, methodID, args).b;
}

jchar CallStaticCharMethod(JNIEnv* env, jclass clazz, jmethodID methodID, ...) {
  ScopedJniThreadState ts(env);
  va_list ap;
  va_start(ap, methodID);
  return InvokeWithVarArgs(ts, NULL, methodID, ap).c;
}

jchar CallStaticCharMethodV(JNIEnv* env,
    jclass clazz, jmethodID methodID, va_list args) {
  ScopedJniThreadState ts(env);
  return InvokeWithVarArgs(ts, NULL, methodID, args).c;
}

jchar CallStaticCharMethodA(JNIEnv* env,
    jclass clazz, jmethodID methodID, jvalue* args) {
  ScopedJniThreadState ts(env);
  return InvokeWithJValues(ts, NULL, methodID, args).c;
}

jshort CallStaticShortMethod(JNIEnv* env, jclass clazz, jmethodID methodID, ...) {
  ScopedJniThreadState ts(env);
  va_list ap;
  va_start(ap, methodID);
  return InvokeWithVarArgs(ts, NULL, methodID, ap).s;
}

jshort CallStaticShortMethodV(JNIEnv* env,
    jclass clazz, jmethodID methodID, va_list args) {
  ScopedJniThreadState ts(env);
  return InvokeWithVarArgs(ts, NULL, methodID, args).s;
}

jshort CallStaticShortMethodA(JNIEnv* env,
    jclass clazz, jmethodID methodID, jvalue* args) {
  ScopedJniThreadState ts(env);
  return InvokeWithJValues(ts, NULL, methodID, args).s;
}

jint CallStaticIntMethod(JNIEnv* env, jclass clazz, jmethodID methodID, ...) {
  ScopedJniThreadState ts(env);
  va_list ap;
  va_start(ap, methodID);
  return InvokeWithVarArgs(ts, NULL, methodID, ap).i;
}

jint CallStaticIntMethodV(JNIEnv* env,
    jclass clazz, jmethodID methodID, va_list args) {
  ScopedJniThreadState ts(env);
  return InvokeWithVarArgs(ts, NULL, methodID, args).i;
}

jint CallStaticIntMethodA(JNIEnv* env,
    jclass clazz, jmethodID methodID, jvalue* args) {
  ScopedJniThreadState ts(env);
  return InvokeWithJValues(ts, NULL, methodID, args).i;
}

jlong CallStaticLongMethod(JNIEnv* env, jclass clazz, jmethodID methodID, ...) {
  ScopedJniThreadState ts(env);
  va_list ap;
  va_start(ap, methodID);
  return InvokeWithVarArgs(ts, NULL, methodID, ap).j;
}

jlong CallStaticLongMethodV(JNIEnv* env,
    jclass clazz, jmethodID methodID, va_list args) {
  ScopedJniThreadState ts(env);
  return InvokeWithVarArgs(ts, NULL, methodID, args).j;
}

jlong CallStaticLongMethodA(JNIEnv* env,
    jclass clazz, jmethodID methodID, jvalue* args) {
  ScopedJniThreadState ts(env);
  return InvokeWithJValues(ts, NULL, methodID, args).j;
}

jfloat CallStaticFloatMethod(JNIEnv* env, jclass cls, jmethodID methodID, ...) {
  ScopedJniThreadState ts(env);
  va_list ap;
  va_start(ap, methodID);
  return InvokeWithVarArgs(ts, NULL, methodID, ap).f;
}

jfloat CallStaticFloatMethodV(JNIEnv* env,
    jclass clazz, jmethodID methodID, va_list args) {
  ScopedJniThreadState ts(env);
  return InvokeWithVarArgs(ts, NULL, methodID, args).f;
}

jfloat CallStaticFloatMethodA(JNIEnv* env,
    jclass clazz, jmethodID methodID, jvalue* args) {
  ScopedJniThreadState ts(env);
  return InvokeWithJValues(ts, NULL, methodID, args).f;
}

jdouble CallStaticDoubleMethod(JNIEnv* env, jclass cls, jmethodID methodID, ...) {
  ScopedJniThreadState ts(env);
  va_list ap;
  va_start(ap, methodID);
  return InvokeWithVarArgs(ts, NULL, methodID, ap).d;
}

jdouble CallStaticDoubleMethodV(JNIEnv* env,
    jclass clazz, jmethodID methodID, va_list args) {
  ScopedJniThreadState ts(env);
  return InvokeWithVarArgs(ts, NULL, methodID, args).d;
}

jdouble CallStaticDoubleMethodA(JNIEnv* env,
    jclass clazz, jmethodID methodID, jvalue* args) {
  ScopedJniThreadState ts(env);
  return InvokeWithJValues(ts, NULL, methodID, args).d;
}

void CallStaticVoidMethod(JNIEnv* env, jclass cls, jmethodID methodID, ...) {
  ScopedJniThreadState ts(env);
  va_list ap;
  va_start(ap, methodID);
  InvokeWithVarArgs(ts, NULL, methodID, ap);
}

void CallStaticVoidMethodV(JNIEnv* env,
    jclass cls, jmethodID methodID, va_list args) {
  ScopedJniThreadState ts(env);
  InvokeWithVarArgs(ts, NULL, methodID, args);
}

void CallStaticVoidMethodA(JNIEnv* env,
    jclass cls, jmethodID methodID, jvalue* args) {
  ScopedJniThreadState ts(env);
  InvokeWithJValues(ts, NULL, methodID, args);
}

jfieldID GetStaticFieldID(JNIEnv* env,
    jclass clazz, const char* name, const char* sig) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jobject GetStaticObjectField(JNIEnv* env, jclass clazz, jfieldID fieldID) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jboolean GetStaticBooleanField(JNIEnv* env, jclass clazz, jfieldID fieldID) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return JNI_FALSE;
}

jbyte GetStaticByteField(JNIEnv* env, jclass clazz, jfieldID fieldID) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jchar GetStaticCharField(JNIEnv* env, jclass clazz, jfieldID fieldID) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jshort GetStaticShortField(JNIEnv* env, jclass clazz, jfieldID fieldID) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jint GetStaticIntField(JNIEnv* env, jclass clazz, jfieldID fieldID) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jlong GetStaticLongField(JNIEnv* env, jclass clazz, jfieldID fieldID) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jfloat GetStaticFloatField(JNIEnv* env, jclass clazz, jfieldID fieldID) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jdouble GetStaticDoubleField(JNIEnv* env, jclass clazz, jfieldID fieldID) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

void SetStaticObjectField(JNIEnv* env,
    jclass clazz, jfieldID fieldID, jobject value) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void SetStaticBooleanField(JNIEnv* env,
    jclass clazz, jfieldID fieldID, jboolean value) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void SetStaticByteField(JNIEnv* env,
    jclass clazz, jfieldID fieldID, jbyte value) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void SetStaticCharField(JNIEnv* env,
    jclass clazz, jfieldID fieldID, jchar value) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void SetStaticShortField(JNIEnv* env,
    jclass clazz, jfieldID fieldID, jshort value) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void SetStaticIntField(JNIEnv* env,
    jclass clazz, jfieldID fieldID, jint value) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void SetStaticLongField(JNIEnv* env,
    jclass clazz, jfieldID fieldID, jlong value) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void SetStaticFloatField(JNIEnv* env,
    jclass clazz, jfieldID fieldID, jfloat value) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void SetStaticDoubleField(JNIEnv* env,
    jclass clazz, jfieldID fieldID, jdouble value) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

jstring NewString(JNIEnv* env, const jchar* unicode, jsize len) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jsize GetStringLength(JNIEnv* env, jstring str) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

const jchar* GetStringChars(JNIEnv* env, jstring str, jboolean* isCopy) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return NULL;
}

void ReleaseStringChars(JNIEnv* env, jstring str, const jchar* chars) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

jstring NewStringUTF(JNIEnv* env, const char* utf) {
  ScopedJniThreadState ts(env);
  if (utf == NULL) {
    return NULL;
  }
  size_t char_count = String::ModifiedUtf8Len(utf);
  String* result = String::AllocFromModifiedUtf8(char_count, utf);
  return AddLocalReference<jstring>(ts, result);
}

jsize GetStringUTFLength(JNIEnv* env, jstring str) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

const char* GetStringUTFChars(JNIEnv* env, jstring str, jboolean* isCopy) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return NULL;
}

void ReleaseStringUTFChars(JNIEnv* env, jstring str, const char* chars) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

jsize GetArrayLength(JNIEnv* env, jarray array) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jobject GetObjectArrayElement(JNIEnv* env, jobjectArray array, jsize index) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return NULL;
}

void SetObjectArrayElement(JNIEnv* env,
    jobjectArray java_array, jsize index, jobject java_value) {
  ScopedJniThreadState ts(env);
  ObjectArray<Object>* array = Decode<ObjectArray<Object>*>(ts, java_array);
  Object* value = Decode<Object*>(ts, java_value);
  array->Set(index, value);
}

template<typename JniT, typename ArtT>
JniT NewPrimitiveArray(ScopedJniThreadState& ts, jsize length) {
  CHECK_GE(length, 0); // TODO: ReportJniError
  ArtT* result = ArtT::Alloc(length);
  return AddLocalReference<JniT>(ts, result);
}

jbooleanArray NewBooleanArray(JNIEnv* env, jsize length) {
  ScopedJniThreadState ts(env);
  return NewPrimitiveArray<jbooleanArray, BooleanArray>(ts, length);
}

jbyteArray NewByteArray(JNIEnv* env, jsize length) {
  ScopedJniThreadState ts(env);
  return NewPrimitiveArray<jbyteArray, ByteArray>(ts, length);
}

jcharArray NewCharArray(JNIEnv* env, jsize length) {
  ScopedJniThreadState ts(env);
  return NewPrimitiveArray<jcharArray, CharArray>(ts, length);
}

jdoubleArray NewDoubleArray(JNIEnv* env, jsize length) {
  ScopedJniThreadState ts(env);
  return NewPrimitiveArray<jdoubleArray, DoubleArray>(ts, length);
}

jfloatArray NewFloatArray(JNIEnv* env, jsize length) {
  ScopedJniThreadState ts(env);
  return NewPrimitiveArray<jfloatArray, FloatArray>(ts, length);
}

jintArray NewIntArray(JNIEnv* env, jsize length) {
  ScopedJniThreadState ts(env);
  return NewPrimitiveArray<jintArray, IntArray>(ts, length);
}

jlongArray NewLongArray(JNIEnv* env, jsize length) {
  ScopedJniThreadState ts(env);
  return NewPrimitiveArray<jlongArray, LongArray>(ts, length);
}

jobjectArray NewObjectArray(JNIEnv* env, jsize length, jclass element_jclass, jobject initial_element) {
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

jshortArray NewShortArray(JNIEnv* env, jsize length) {
  ScopedJniThreadState ts(env);
  return NewPrimitiveArray<jshortArray, ShortArray>(ts, length);
}

jboolean* GetBooleanArrayElements(JNIEnv* env,
    jbooleanArray array, jboolean* isCopy) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jbyte* GetByteArrayElements(JNIEnv* env, jbyteArray array, jboolean* isCopy) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jchar* GetCharArrayElements(JNIEnv* env, jcharArray array, jboolean* isCopy) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jshort* GetShortArrayElements(JNIEnv* env,
    jshortArray array, jboolean* isCopy) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jint* GetIntArrayElements(JNIEnv* env, jintArray array, jboolean* isCopy) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jlong* GetLongArrayElements(JNIEnv* env, jlongArray array, jboolean* isCopy) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jfloat* GetFloatArrayElements(JNIEnv* env,
    jfloatArray array, jboolean* isCopy) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jdouble* GetDoubleArrayElements(JNIEnv* env,
    jdoubleArray array, jboolean* isCopy) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return NULL;
}

void ReleaseBooleanArrayElements(JNIEnv* env,
    jbooleanArray array, jboolean* elems, jint mode) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void ReleaseByteArrayElements(JNIEnv* env,
    jbyteArray array, jbyte* elems, jint mode) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void ReleaseCharArrayElements(JNIEnv* env,
    jcharArray array, jchar* elems, jint mode) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void ReleaseShortArrayElements(JNIEnv* env,
    jshortArray array, jshort* elems, jint mode) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void ReleaseIntArrayElements(JNIEnv* env,
    jintArray array, jint* elems, jint mode) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void ReleaseLongArrayElements(JNIEnv* env,
    jlongArray array, jlong* elems, jint mode) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void ReleaseFloatArrayElements(JNIEnv* env,
    jfloatArray array, jfloat* elems, jint mode) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void ReleaseDoubleArrayElements(JNIEnv* env,
    jdoubleArray array, jdouble* elems, jint mode) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void GetBooleanArrayRegion(JNIEnv* env,
    jbooleanArray array, jsize start, jsize l, jboolean* buf) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void GetByteArrayRegion(JNIEnv* env,
    jbyteArray array, jsize start, jsize len, jbyte* buf) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void GetCharArrayRegion(JNIEnv* env,
    jcharArray array, jsize start, jsize len, jchar* buf) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void GetShortArrayRegion(JNIEnv* env,
    jshortArray array, jsize start, jsize len, jshort* buf) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void GetIntArrayRegion(JNIEnv* env,
    jintArray array, jsize start, jsize len, jint* buf) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void GetLongArrayRegion(JNIEnv* env,
    jlongArray array, jsize start, jsize len, jlong* buf) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void GetFloatArrayRegion(JNIEnv* env,
    jfloatArray array, jsize start, jsize len, jfloat* buf) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void GetDoubleArrayRegion(JNIEnv* env,
    jdoubleArray array, jsize start, jsize len, jdouble* buf) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void SetBooleanArrayRegion(JNIEnv* env,
    jbooleanArray array, jsize start, jsize l, const jboolean* buf) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void SetByteArrayRegion(JNIEnv* env,
    jbyteArray array, jsize start, jsize len, const jbyte* buf) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void SetCharArrayRegion(JNIEnv* env,
    jcharArray array, jsize start, jsize len, const jchar* buf) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void SetShortArrayRegion(JNIEnv* env,
    jshortArray array, jsize start, jsize len, const jshort* buf) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void SetIntArrayRegion(JNIEnv* env,
    jintArray array, jsize start, jsize len, const jint* buf) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void SetLongArrayRegion(JNIEnv* env,
    jlongArray array, jsize start, jsize len, const jlong* buf) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void SetFloatArrayRegion(JNIEnv* env,
    jfloatArray array, jsize start, jsize len, const jfloat* buf) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void SetDoubleArrayRegion(JNIEnv* env,
    jdoubleArray array, jsize start, jsize len, const jdouble* buf) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

jint RegisterNatives(JNIEnv* env,
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

jint UnregisterNatives(JNIEnv* env, jclass clazz) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jint MonitorEnter(JNIEnv* env, jobject obj) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(WARNING);
  return 0;
}

jint MonitorExit(JNIEnv* env, jobject obj) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(WARNING);
  return 0;
}

jint GetJavaVM(JNIEnv* env, JavaVM** vm) {
  ScopedJniThreadState ts(env);
  Runtime* runtime = Runtime::Current();
  if (runtime != NULL) {
    *vm = reinterpret_cast<JavaVM*>(runtime->GetJavaVM());
  } else {
    *vm = NULL;
  }
  return (*vm != NULL) ? JNI_OK : JNI_ERR;
}

void GetStringRegion(JNIEnv* env,
    jstring str, jsize start, jsize len, jchar* buf) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void GetStringUTFRegion(JNIEnv* env,
    jstring str, jsize start, jsize len, char* buf) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

void* GetPrimitiveArrayCritical(JNIEnv* env,
    jarray array, jboolean* isCopy) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return NULL;
}

void ReleasePrimitiveArrayCritical(JNIEnv* env,
    jarray array, void* carray, jint mode) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

const jchar* GetStringCritical(JNIEnv* env, jstring s, jboolean* isCopy) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return NULL;
}

void ReleaseStringCritical(JNIEnv* env, jstring s, const jchar* cstr) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
}

jboolean ExceptionCheck(JNIEnv* env) {
  ScopedJniThreadState ts(env);
  return ts.Self()->IsExceptionPending() ? JNI_TRUE : JNI_FALSE;
}

jobject NewDirectByteBuffer(JNIEnv* env, void* address, jlong capacity) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return NULL;
}


void* GetDirectBufferAddress(JNIEnv* env, jobject buf) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return NULL;
}

jlong GetDirectBufferCapacity(JNIEnv* env, jobject buf) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return 0;
}

jobjectRefType GetObjectRefType(JNIEnv* env, jobject jobj) {
  ScopedJniThreadState ts(env);
  UNIMPLEMENTED(FATAL);
  return JNIInvalidRefType;
}

static const struct JNINativeInterface gNativeInterface = {
  NULL,  // reserved0.
  NULL,  // reserved1.
  NULL,  // reserved2.
  NULL,  // reserved3.
  GetVersion,
  DefineClass,
  FindClass,
  FromReflectedMethod,
  FromReflectedField,
  ToReflectedMethod,
  GetSuperclass,
  IsAssignableFrom,
  ToReflectedField,
  Throw,
  ThrowNew,
  ExceptionOccurred,
  ExceptionDescribe,
  ExceptionClear,
  FatalError,
  PushLocalFrame,
  PopLocalFrame,
  NewGlobalRef,
  DeleteGlobalRef,
  DeleteLocalRef,
  IsSameObject,
  NewLocalRef,
  EnsureLocalCapacity,
  AllocObject,
  NewObject,
  NewObjectV,
  NewObjectA,
  GetObjectClass,
  IsInstanceOf,
  GetMethodID,
  CallObjectMethod,
  CallObjectMethodV,
  CallObjectMethodA,
  CallBooleanMethod,
  CallBooleanMethodV,
  CallBooleanMethodA,
  CallByteMethod,
  CallByteMethodV,
  CallByteMethodA,
  CallCharMethod,
  CallCharMethodV,
  CallCharMethodA,
  CallShortMethod,
  CallShortMethodV,
  CallShortMethodA,
  CallIntMethod,
  CallIntMethodV,
  CallIntMethodA,
  CallLongMethod,
  CallLongMethodV,
  CallLongMethodA,
  CallFloatMethod,
  CallFloatMethodV,
  CallFloatMethodA,
  CallDoubleMethod,
  CallDoubleMethodV,
  CallDoubleMethodA,
  CallVoidMethod,
  CallVoidMethodV,
  CallVoidMethodA,
  CallNonvirtualObjectMethod,
  CallNonvirtualObjectMethodV,
  CallNonvirtualObjectMethodA,
  CallNonvirtualBooleanMethod,
  CallNonvirtualBooleanMethodV,
  CallNonvirtualBooleanMethodA,
  CallNonvirtualByteMethod,
  CallNonvirtualByteMethodV,
  CallNonvirtualByteMethodA,
  CallNonvirtualCharMethod,
  CallNonvirtualCharMethodV,
  CallNonvirtualCharMethodA,
  CallNonvirtualShortMethod,
  CallNonvirtualShortMethodV,
  CallNonvirtualShortMethodA,
  CallNonvirtualIntMethod,
  CallNonvirtualIntMethodV,
  CallNonvirtualIntMethodA,
  CallNonvirtualLongMethod,
  CallNonvirtualLongMethodV,
  CallNonvirtualLongMethodA,
  CallNonvirtualFloatMethod,
  CallNonvirtualFloatMethodV,
  CallNonvirtualFloatMethodA,
  CallNonvirtualDoubleMethod,
  CallNonvirtualDoubleMethodV,
  CallNonvirtualDoubleMethodA,
  CallNonvirtualVoidMethod,
  CallNonvirtualVoidMethodV,
  CallNonvirtualVoidMethodA,
  GetFieldID,
  GetObjectField,
  GetBooleanField,
  GetByteField,
  GetCharField,
  GetShortField,
  GetIntField,
  GetLongField,
  GetFloatField,
  GetDoubleField,
  SetObjectField,
  SetBooleanField,
  SetByteField,
  SetCharField,
  SetShortField,
  SetIntField,
  SetLongField,
  SetFloatField,
  SetDoubleField,
  GetStaticMethodID,
  CallStaticObjectMethod,
  CallStaticObjectMethodV,
  CallStaticObjectMethodA,
  CallStaticBooleanMethod,
  CallStaticBooleanMethodV,
  CallStaticBooleanMethodA,
  CallStaticByteMethod,
  CallStaticByteMethodV,
  CallStaticByteMethodA,
  CallStaticCharMethod,
  CallStaticCharMethodV,
  CallStaticCharMethodA,
  CallStaticShortMethod,
  CallStaticShortMethodV,
  CallStaticShortMethodA,
  CallStaticIntMethod,
  CallStaticIntMethodV,
  CallStaticIntMethodA,
  CallStaticLongMethod,
  CallStaticLongMethodV,
  CallStaticLongMethodA,
  CallStaticFloatMethod,
  CallStaticFloatMethodV,
  CallStaticFloatMethodA,
  CallStaticDoubleMethod,
  CallStaticDoubleMethodV,
  CallStaticDoubleMethodA,
  CallStaticVoidMethod,
  CallStaticVoidMethodV,
  CallStaticVoidMethodA,
  GetStaticFieldID,
  GetStaticObjectField,
  GetStaticBooleanField,
  GetStaticByteField,
  GetStaticCharField,
  GetStaticShortField,
  GetStaticIntField,
  GetStaticLongField,
  GetStaticFloatField,
  GetStaticDoubleField,
  SetStaticObjectField,
  SetStaticBooleanField,
  SetStaticByteField,
  SetStaticCharField,
  SetStaticShortField,
  SetStaticIntField,
  SetStaticLongField,
  SetStaticFloatField,
  SetStaticDoubleField,
  NewString,
  GetStringLength,
  GetStringChars,
  ReleaseStringChars,
  NewStringUTF,
  GetStringUTFLength,
  GetStringUTFChars,
  ReleaseStringUTFChars,
  GetArrayLength,
  NewObjectArray,
  GetObjectArrayElement,
  SetObjectArrayElement,
  NewBooleanArray,
  NewByteArray,
  NewCharArray,
  NewShortArray,
  NewIntArray,
  NewLongArray,
  NewFloatArray,
  NewDoubleArray,
  GetBooleanArrayElements,
  GetByteArrayElements,
  GetCharArrayElements,
  GetShortArrayElements,
  GetIntArrayElements,
  GetLongArrayElements,
  GetFloatArrayElements,
  GetDoubleArrayElements,
  ReleaseBooleanArrayElements,
  ReleaseByteArrayElements,
  ReleaseCharArrayElements,
  ReleaseShortArrayElements,
  ReleaseIntArrayElements,
  ReleaseLongArrayElements,
  ReleaseFloatArrayElements,
  ReleaseDoubleArrayElements,
  GetBooleanArrayRegion,
  GetByteArrayRegion,
  GetCharArrayRegion,
  GetShortArrayRegion,
  GetIntArrayRegion,
  GetLongArrayRegion,
  GetFloatArrayRegion,
  GetDoubleArrayRegion,
  SetBooleanArrayRegion,
  SetByteArrayRegion,
  SetCharArrayRegion,
  SetShortArrayRegion,
  SetIntArrayRegion,
  SetLongArrayRegion,
  SetFloatArrayRegion,
  SetDoubleArrayRegion,
  RegisterNatives,
  UnregisterNatives,
  MonitorEnter,
  MonitorExit,
  GetJavaVM,
  GetStringRegion,
  GetStringUTFRegion,
  GetPrimitiveArrayCritical,
  ReleasePrimitiveArrayCritical,
  GetStringCritical,
  ReleaseStringCritical,
  NewWeakGlobalRef,
  DeleteWeakGlobalRef,
  ExceptionCheck,
  NewDirectByteBuffer,
  GetDirectBufferAddress,
  GetDirectBufferCapacity,
  GetObjectRefType,
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

jint DestroyJavaVM(JavaVM* vm) {
  if (vm == NULL) {
    return JNI_ERR;
  } else {
    JavaVMExt* raw_vm = reinterpret_cast<JavaVMExt*>(vm);
    delete raw_vm->runtime;
    return JNI_OK;
  }
}

jint AttachCurrentThread(JavaVM* vm, JNIEnv** p_env, void* thr_args) {
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

jint DetachCurrentThread(JavaVM* vm) {
  if (vm == NULL) {
    return JNI_ERR;
  } else {
    JavaVMExt* raw_vm = reinterpret_cast<JavaVMExt*>(vm);
    Runtime* runtime = raw_vm->runtime;
    runtime->DetachCurrentThread();
    return JNI_OK;
  }
}

jint GetEnv(JavaVM* vm, void** env, jint version) {
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

jint AttachCurrentThreadAsDaemon(JavaVM* vm, JNIEnv** p_env, void* thr_args) {
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

struct JNIInvokeInterface gInvokeInterface = {
  NULL,  // reserved0
  NULL,  // reserved1
  NULL,  // reserved2
  DestroyJavaVM,
  AttachCurrentThread,
  DetachCurrentThread,
  GetEnv,
  AttachCurrentThreadAsDaemon
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

}  // namespace art
