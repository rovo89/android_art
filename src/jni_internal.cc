// Copyright 2011 Google Inc. All Rights Reserved.

#include "jni_internal.h"

#include <dlfcn.h>
#include <sys/mman.h>

#include <cstdarg>
#include <map>
#include <utility>
#include <vector>

#include "ScopedLocalRef.h"
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
void EnsureInvokeStub(Method* method) {
  if (method->GetInvokeStub() != NULL) {
    return;
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
    PLOG(FATAL) << "mmap failed for " << PrettyMethod(method, true);
  }
  MemoryRegion region(addr, length);
  assembler.FinalizeInstructions(region);
  method->SetInvokeStub(reinterpret_cast<Method::InvokeStub*>(region.pointer()));
}

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

  JavaVMExt* Vm() {
    return env_->vm;
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
      std::string class_descriptor(PrettyDescriptor(obj->GetClass()->GetDescriptor()));
      LOG(WARNING) << "Warning: more than 16 JNI local references: "
                   << entry_count << " (most recent was a " << class_descriptor << ")";
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
  JavaVMExt* vm = ts.Vm();
  IndirectReferenceTable& weak_globals = vm->weak_globals;
  MutexLock mu(vm->weak_globals_lock);
  IndirectRef ref = weak_globals.Add(IRT_FIRST_SEGMENT, obj);
  return reinterpret_cast<jweak>(ref);
}

template<typename T>
T Decode(ScopedJniThreadState& ts, jobject obj) {
  return reinterpret_cast<T>(ts.Self()->DecodeJObject(obj));
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

JValue InvokeWithArgArray(ScopedJniThreadState& ts, Object* receiver,
                          Method* method, byte* args) {
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
  if (method->HasCode()) {
    (*stub)(method, receiver, self, args, &result);
  } else {
    LOG(WARNING) << "Not invoking method with no associated code: "
                 << PrettyMethod(method, true);
    result.j = 0;
  }

  // Pop transition
  self->PopNativeToManagedRecord(record);
  return result;
}

JValue InvokeWithJValues(ScopedJniThreadState& ts, jobject obj,
                         jmethodID mid, jvalue* args) {
  Object* receiver = Decode<Object*>(ts, obj);
  Method* method = DecodeMethod(ts, mid);
  scoped_array<byte> arg_array(CreateArgArray(ts, method, args));
  return InvokeWithArgArray(ts, receiver, method, arg_array.get());
}

JValue InvokeWithVarArgs(ScopedJniThreadState& ts, jobject obj,
                         jmethodID mid, va_list args) {
  Object* receiver = Decode<Object*>(ts, obj);
  Method* method = DecodeMethod(ts, mid);
  scoped_array<byte> arg_array(CreateArgArray(ts, method, args));
  return InvokeWithArgArray(ts, receiver, method, arg_array.get());
}

Method* FindVirtualMethod(Object* receiver, Method* method) {
  return receiver->GetClass()->FindVirtualMethodForVirtualOrInterface(method);
}

JValue InvokeVirtualOrInterfaceWithJValues(ScopedJniThreadState& ts, jobject obj, jmethodID mid, jvalue* args) {
  Object* receiver = Decode<Object*>(ts, obj);
  Method* method = FindVirtualMethod(receiver, DecodeMethod(ts, mid));
  scoped_array<byte> arg_array(CreateArgArray(ts, method, args));
  return InvokeWithArgArray(ts, receiver, method, arg_array.get());
}

JValue InvokeVirtualOrInterfaceWithVarArgs(ScopedJniThreadState& ts, jobject obj, jmethodID mid, va_list args) {
  Object* receiver = Decode<Object*>(ts, obj);
  Method* method = FindVirtualMethod(receiver, DecodeMethod(ts, mid));
  scoped_array<byte> arg_array(CreateArgArray(ts, method, args));
  return InvokeWithArgArray(ts, receiver, method, arg_array.get());
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
  if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(c)) {
    return NULL;
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
    std::string method_name(PrettyMethod(method, true));
    // TODO: try searching for the opposite kind of method from is_static
    // for better diagnostics?
    self->ThrowNewException("Ljava/lang/NoSuchMethodError;",
        "no %s method %s", is_static ? "static" : "non-static",
        method_name.c_str());
    return NULL;
  }

  EnsureInvokeStub(method);

  return reinterpret_cast<jmethodID>(AddWeakGlobalReference(ts, method));
}

jfieldID FindFieldID(ScopedJniThreadState& ts, jclass jni_class, const char* name, const char* sig, bool is_static) {
  Class* c = Decode<Class*>(ts, jni_class);
  if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(c)) {
    return NULL;
  }

  Field* field = NULL;
  if (is_static) {
    field = c->FindStaticField(name, sig);
  } else {
    field = c->FindInstanceField(name, sig);
  }

  if (field == NULL) {
    Thread* self = Thread::Current();
    std::string class_descriptor(c->GetDescriptor()->ToModifiedUtf8());
    self->ThrowNewException("Ljava/lang/NoSuchFieldError;",
        "no \"%s\" field \"%s\" in class \"%s\" or its superclasses", sig,
        name, class_descriptor.c_str());
    return NULL;
  }

  jweak fid = AddWeakGlobalReference(ts, field);
  return reinterpret_cast<jfieldID>(fid);
}

void PinPrimitiveArray(ScopedJniThreadState& ts, const Array* array) {
  JavaVMExt* vm = ts.Vm();
  MutexLock mu(vm->pins_lock);
  vm->pin_table.Add(array);
}

void UnpinPrimitiveArray(ScopedJniThreadState& ts, const Array* array) {
  JavaVMExt* vm = ts.Vm();
  MutexLock mu(vm->pins_lock);
  vm->pin_table.Remove(array);
}

template<typename JniT, typename ArtT>
JniT NewPrimitiveArray(ScopedJniThreadState& ts, jsize length) {
  CHECK_GE(length, 0); // TODO: ReportJniError
  ArtT* result = ArtT::Alloc(length);
  return AddLocalReference<JniT>(ts, result);
}

template <typename ArrayT, typename CArrayT, typename ArtArrayT>
CArrayT GetPrimitiveArray(ScopedJniThreadState& ts, ArrayT java_array, jboolean* is_copy) {
  ArtArrayT* array = Decode<ArtArrayT*>(ts, java_array);
  PinPrimitiveArray(ts, array);
  if (is_copy != NULL) {
    *is_copy = JNI_FALSE;
  }
  return array->GetData();
}

template <typename ArrayT>
void ReleasePrimitiveArray(ScopedJniThreadState& ts, ArrayT java_array, jint mode) {
  if (mode != JNI_COMMIT) {
    Array* array = Decode<Array*>(ts, java_array);
    UnpinPrimitiveArray(ts, array);
  }
}

void ThrowAIOOBE(ScopedJniThreadState& ts, Array* array, jsize start, jsize length, const char* identifier) {
  std::string type(PrettyType(array));
  ts.Self()->ThrowNewException("Ljava/lang/ArrayIndexOutOfBoundsException;",
      "%s offset=%d length=%d %s.length=%d",
      type.c_str(), start, length, identifier, array->GetLength());
}
void ThrowSIOOBE(ScopedJniThreadState& ts, jsize start, jsize length, jsize array_length) {
  ts.Self()->ThrowNewException("Ljava/lang/StringIndexOutOfBoundsException;",
      "offset=%d length=%d string.length()=%d", start, length, array_length);
}

template <typename JavaArrayT, typename JavaT, typename ArrayT>
void GetPrimitiveArrayRegion(ScopedJniThreadState& ts, JavaArrayT java_array, jsize start, jsize length, JavaT* buf) {
  ArrayT* array = Decode<ArrayT*>(ts, java_array);
  if (start < 0 || length < 0 || start + length > array->GetLength()) {
    ThrowAIOOBE(ts, array, start, length, "src");
  } else {
    JavaT* data = array->GetData();
    memcpy(buf, data + start, length * sizeof(JavaT));
  }
}

template <typename JavaArrayT, typename JavaT, typename ArrayT>
void SetPrimitiveArrayRegion(ScopedJniThreadState& ts, JavaArrayT java_array, jsize start, jsize length, const JavaT* buf) {
  ArrayT* array = Decode<ArrayT*>(ts, java_array);
  if (start < 0 || length < 0 || start + length > array->GetLength()) {
    ThrowAIOOBE(ts, array, start, length, "dst");
  } else {
    JavaT* data = array->GetData();
    memcpy(data + start, buf, length * sizeof(JavaT));
  }
}

jclass InitDirectByteBufferClass(JNIEnv* env) {
  ScopedLocalRef<jclass> buffer_class(env, env->FindClass("java/nio/ReadWriteDirectByteBuffer"));
  CHECK(buffer_class.get() != NULL);
  return reinterpret_cast<jclass>(env->NewGlobalRef(buffer_class.get()));
}

jclass GetDirectByteBufferClass(JNIEnv* env) {
  static jclass buffer_class = InitDirectByteBufferClass(env);
  return buffer_class;
}

jint JII_AttachCurrentThread(JavaVM* vm, JNIEnv** p_env, void* thr_args, bool as_daemon) {
  if (vm == NULL || p_env == NULL) {
    return JNI_ERR;
  }

  JavaVMAttachArgs* in_args = static_cast<JavaVMAttachArgs*>(thr_args);
  JavaVMAttachArgs args;
  if (thr_args == NULL) {
    // Allow the v1.1 calling convention.
    args.version = JNI_VERSION_1_2;
    args.name = NULL;
    args.group = NULL; // TODO: get "main" thread group
  } else {
    args.version = in_args->version;
    args.name = in_args->name;
    if (in_args->group != NULL) {
      UNIMPLEMENTED(WARNING) << "thr_args->group != NULL";
      args.group = NULL; // TODO: decode in_args->group
    } else {
      args.group = NULL; // TODO: get "main" thread group
    }
  }
  CHECK_GE(args.version, JNI_VERSION_1_2);

  Runtime* runtime = reinterpret_cast<JavaVMExt*>(vm)->runtime;
  return runtime->AttachCurrentThread(args.name, p_env, as_daemon) ? JNI_OK : JNI_ERR;
}

class SharedLibrary {
 public:
  SharedLibrary(const std::string& path, void* handle, Object* class_loader)
      : path_(path),
        handle_(handle),
        jni_on_load_lock_(Mutex::Create("JNI_OnLoad lock")),
        jni_on_load_tid_(Thread::Current()->GetId()),
        jni_on_load_result_(kPending) {
    pthread_cond_init(&jni_on_load_cond_, NULL);
  }

  ~SharedLibrary() {
    delete jni_on_load_lock_;
  }

  Object* GetClassLoader() {
    return class_loader_;
  }

  std::string GetPath() {
    return path_;
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

    MutexLock mu(jni_on_load_lock_);
    while (jni_on_load_result_ == kPending) {
      if (vm->verbose_jni) {
        LOG(INFO) << "[" << *self << " waiting for \"" << path_ << "\" "
                  << "JNI_OnLoad...]";
      }
      Thread::State old_state = self->GetState();
      self->SetState(Thread::kWaiting); // TODO: VMWAIT
      pthread_cond_wait(&jni_on_load_cond_, jni_on_load_lock_->GetImpl());
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
    MutexLock mu(jni_on_load_lock_);
    pthread_cond_broadcast(&jni_on_load_cond_);
  }

  void* FindSymbol(const std::string& symbol_name) {
    return dlsym(handle_, symbol_name.c_str());
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

}  // namespace

// This exists mainly to keep implementation details out of the header file.
class Libraries {
 public:
  Libraries() {
  }

  ~Libraries() {
    // Delete our map values. (The keys will be cleaned up by the map itself.)
    for (It it = libraries_.begin(); it != libraries_.end(); ++it) {
      delete it->second;
    }
  }

  SharedLibrary* Get(const std::string& path) {
    return libraries_[path];
  }

  void Put(const std::string& path, SharedLibrary* library) {
    libraries_[path] = library;
  }

  // See section 11.3 "Linking Native Methods" of the JNI spec.
  void* FindNativeMethod(const Method* m) {
    std::string jni_short_name(JniShortName(m));
    std::string jni_long_name(JniLongName(m));
    const ClassLoader* declaring_class_loader = m->GetDeclaringClass()->GetClassLoader();
    for (It it = libraries_.begin(); it != libraries_.end(); ++it) {
      SharedLibrary* library = it->second;
      if (library->GetClassLoader() != declaring_class_loader) {
        // We only search libraries loaded by the appropriate ClassLoader.
        continue;
      }
      // Try the short name then the long name...
      void* fn = library->FindSymbol(jni_short_name);
      if (fn == NULL) {
        fn = library->FindSymbol(jni_long_name);
      }
      if (fn != NULL) {
        if (Runtime::Current()->GetJavaVM()->verbose_jni) {
          LOG(INFO) << "[Found native code for " << PrettyMethod(m, true)
                    << " in \"" << library->GetPath() << "\"]";
        }
        return fn;
      }
    }
    std::string detail;
    detail += "No implementation found for ";
    detail += PrettyMethod(m, true);
    LOG(ERROR) << detail;
    Thread::Current()->ThrowNewException("Ljava/lang/UnsatisfiedLinkError;",
        "%s", detail.c_str());
    return NULL;
  }

 private:
  typedef std::map<std::string, SharedLibrary*>::iterator It; // TODO: C++0x auto

  std::map<std::string, SharedLibrary*> libraries_;
};

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
    const ClassLoader* cl = ts.Self()->GetClassLoaderOverride();
    Class* c = class_linker->FindClass(descriptor, cl);
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

  static jclass GetObjectClass(JNIEnv* env, jobject java_object) {
    ScopedJniThreadState ts(env);
    Object* o = Decode<Object*>(ts, java_object);
    return AddLocalReference<jclass>(ts, o->GetClass());
  }

  static jclass GetSuperclass(JNIEnv* env, jclass java_class) {
    ScopedJniThreadState ts(env);
    Class* c = Decode<Class*>(ts, java_class);
    return AddLocalReference<jclass>(ts, c->GetSuperClass());
  }

  static jboolean IsAssignableFrom(JNIEnv* env, jclass java_class1, jclass java_class2) {
    ScopedJniThreadState ts(env);
    Class* c1 = Decode<Class*>(ts, java_class1);
    Class* c2 = Decode<Class*>(ts, java_class2);
    return c1->IsAssignableFrom(c2) ? JNI_TRUE : JNI_FALSE;
  }

  static jboolean IsInstanceOf(JNIEnv* env, jobject jobj, jclass clazz) {
    ScopedJniThreadState ts(env);
    CHECK_NE(static_cast<jclass>(NULL), clazz); // TODO: ReportJniError
    if (jobj == NULL) {
      // NB. JNI is different from regular Java instanceof in this respect
      return JNI_TRUE;
    } else {
      Object* obj = Decode<Object*>(ts, jobj);
      Class* klass = Decode<Class*>(ts, clazz);
      return Object::InstanceOf(obj, klass) ? JNI_TRUE : JNI_FALSE;
    }
  }

  static jint Throw(JNIEnv* env, jthrowable java_exception) {
    ScopedJniThreadState ts(env);
    Throwable* exception = Decode<Throwable*>(ts, java_exception);
    if (exception == NULL) {
      return JNI_ERR;
    }
    ts.Self()->SetException(exception);
    return JNI_OK;
  }

  static jint ThrowNew(JNIEnv* env, jclass c, const char* msg) {
    ScopedJniThreadState ts(env);
    // TODO: check for a pending exception to decide what constructor to call.
    jmethodID mid = env->GetMethodID(c, "<init>", "(Ljava/lang/String;)V");
    if (mid == NULL) {
      return JNI_ERR;
    }
    ScopedLocalRef<jstring> s(env, env->NewStringUTF(msg));
    if (s.get() == NULL) {
      return JNI_ERR;
    }

    jvalue args[1];
    args[0].l = s.get();
    ScopedLocalRef<jthrowable> exception(env, reinterpret_cast<jthrowable>(env->NewObjectA(c, mid, args)));
    if (exception.get() == NULL) {
      return JNI_ERR;
    }

    LOG(INFO) << "Throwing " << PrettyType(Decode<Throwable*>(ts, exception.get()))
              << ": " << msg;
    ts.Self()->SetException(Decode<Throwable*>(ts, exception.get()));

    return JNI_OK;
  }

  static jboolean ExceptionCheck(JNIEnv* env) {
    ScopedJniThreadState ts(env);
    return ts.Self()->IsExceptionPending() ? JNI_TRUE : JNI_FALSE;
  }

  static void ExceptionClear(JNIEnv* env) {
    ScopedJniThreadState ts(env);
    ts.Self()->ClearException();
  }

  static void ExceptionDescribe(JNIEnv* env) {
    ScopedJniThreadState ts(env);

    Thread* self = ts.Self();
    Throwable* original_exception = self->GetException();
    self->ClearException();

    ScopedLocalRef<jthrowable> exception(env, AddLocalReference<jthrowable>(ts, original_exception));
    ScopedLocalRef<jclass> exception_class(env, env->GetObjectClass(exception.get()));
    jmethodID mid = env->GetMethodID(exception_class.get(), "printStackTrace", "()V");
    if (mid == NULL) {
      LOG(WARNING) << "JNI WARNING: no printStackTrace()V in "
                   << PrettyType(original_exception);
    } else {
      env->CallVoidMethod(exception.get(), mid);
      if (self->IsExceptionPending()) {
        LOG(WARNING) << "JNI WARNING: " << PrettyType(self->GetException())
                     << " thrown while calling printStackTrace";
        self->ClearException();
      }
    }

    self->SetException(original_exception);
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

  static jint EnsureLocalCapacity(JNIEnv* env, jint cap) {
    ScopedJniThreadState ts(env);
    UNIMPLEMENTED(WARNING) << "ignoring EnsureLocalCapacity(" << cap << ")";
    return 0;
  }

  static jobject NewGlobalRef(JNIEnv* env, jobject obj) {
    ScopedJniThreadState ts(env);
    if (obj == NULL) {
      return NULL;
    }

    JavaVMExt* vm = ts.Vm();
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

    JavaVMExt* vm = ts.Vm();
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

    JavaVMExt* vm = ts.Vm();
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

  static jobject AllocObject(JNIEnv* env, jclass java_class) {
    ScopedJniThreadState ts(env);
    Class* c = Decode<Class*>(ts, java_class);
    if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(c)) {
      return NULL;
    }
    return AddLocalReference<jobject>(ts, c->NewInstance());
  }

  static jobject NewObject(JNIEnv* env, jclass clazz, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list args;
    va_start(args, mid);
    jobject result = NewObjectV(env, clazz, mid, args);
    va_end(args);
    return result;
  }

  static jobject NewObjectV(JNIEnv* env, jclass java_class, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    Class* c = Decode<Class*>(ts, java_class);
    if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(c)) {
      return NULL;
    }
    Object* result = c->NewInstance();
    jobject local_result = AddLocalReference<jobject>(ts, result);
    CallNonvirtualVoidMethodV(env, local_result, java_class, mid, args);
    return local_result;
  }

  static jobject NewObjectA(JNIEnv* env, jclass java_class, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    Class* c = Decode<Class*>(ts, java_class);
    if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(c)) {
      return NULL;
    }
    Object* result = c->NewInstance();
    jobject local_result = AddLocalReference<jobjectArray>(ts, result);
    CallNonvirtualVoidMethodA(env, local_result, java_class, mid, args);
    return local_result;
  }

  static jmethodID GetMethodID(JNIEnv* env, jclass c, const char* name, const char* sig) {
    ScopedJniThreadState ts(env);
    return FindMethodID(ts, c, name, sig, false);
  }

  static jmethodID GetStaticMethodID(JNIEnv* env, jclass c, const char* name, const char* sig) {
    ScopedJniThreadState ts(env);
    return FindMethodID(ts, c, name, sig, true);
  }

  static jobject CallObjectMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result = InvokeVirtualOrInterfaceWithVarArgs(ts, obj, mid, ap);
    va_end(ap);
    return AddLocalReference<jobject>(ts, result.l);
  }

  static jobject CallObjectMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    JValue result = InvokeVirtualOrInterfaceWithVarArgs(ts, obj, mid, args);
    return AddLocalReference<jobject>(ts, result.l);
  }

  static jobject CallObjectMethodA(JNIEnv* env, jobject obj, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    JValue result = InvokeVirtualOrInterfaceWithJValues(ts, obj, mid, args);
    return AddLocalReference<jobject>(ts, result.l);
  }

  static jboolean CallBooleanMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result = InvokeVirtualOrInterfaceWithVarArgs(ts, obj, mid, ap);
    va_end(ap);
    return result.z;
  }

  static jboolean CallBooleanMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeVirtualOrInterfaceWithVarArgs(ts, obj, mid, args).z;
  }

  static jboolean CallBooleanMethodA(JNIEnv* env, jobject obj, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeVirtualOrInterfaceWithJValues(ts, obj, mid, args).z;
  }

  static jbyte CallByteMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result = InvokeVirtualOrInterfaceWithVarArgs(ts, obj, mid, ap);
    va_end(ap);
    return result.b;
  }

  static jbyte CallByteMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeVirtualOrInterfaceWithVarArgs(ts, obj, mid, args).b;
  }

  static jbyte CallByteMethodA(JNIEnv* env, jobject obj, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeVirtualOrInterfaceWithJValues(ts, obj, mid, args).b;
  }

  static jchar CallCharMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result = InvokeVirtualOrInterfaceWithVarArgs(ts, obj, mid, ap);
    va_end(ap);
    return result.c;
  }

  static jchar CallCharMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeVirtualOrInterfaceWithVarArgs(ts, obj, mid, args).c;
  }

  static jchar CallCharMethodA(JNIEnv* env, jobject obj, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeVirtualOrInterfaceWithJValues(ts, obj, mid, args).c;
  }

  static jdouble CallDoubleMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result = InvokeVirtualOrInterfaceWithVarArgs(ts, obj, mid, ap);
    va_end(ap);
    return result.d;
  }

  static jdouble CallDoubleMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeVirtualOrInterfaceWithVarArgs(ts, obj, mid, args).d;
  }

  static jdouble CallDoubleMethodA(JNIEnv* env, jobject obj, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeVirtualOrInterfaceWithJValues(ts, obj, mid, args).d;
  }

  static jfloat CallFloatMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result = InvokeVirtualOrInterfaceWithVarArgs(ts, obj, mid, ap);
    va_end(ap);
    return result.f;
  }

  static jfloat CallFloatMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeVirtualOrInterfaceWithVarArgs(ts, obj, mid, args).f;
  }

  static jfloat CallFloatMethodA(JNIEnv* env, jobject obj, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeVirtualOrInterfaceWithJValues(ts, obj, mid, args).f;
  }

  static jint CallIntMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result = InvokeVirtualOrInterfaceWithVarArgs(ts, obj, mid, ap);
    va_end(ap);
    return result.i;
  }

  static jint CallIntMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeVirtualOrInterfaceWithVarArgs(ts, obj, mid, args).i;
  }

  static jint CallIntMethodA(JNIEnv* env, jobject obj, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeVirtualOrInterfaceWithJValues(ts, obj, mid, args).i;
  }

  static jlong CallLongMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result = InvokeVirtualOrInterfaceWithVarArgs(ts, obj, mid, ap);
    va_end(ap);
    return result.j;
  }

  static jlong CallLongMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeVirtualOrInterfaceWithVarArgs(ts, obj, mid, args).j;
  }

  static jlong CallLongMethodA(JNIEnv* env, jobject obj, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeVirtualOrInterfaceWithJValues(ts, obj, mid, args).j;
  }

  static jshort CallShortMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result = InvokeVirtualOrInterfaceWithVarArgs(ts, obj, mid, ap);
    va_end(ap);
    return result.s;
  }

  static jshort CallShortMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeVirtualOrInterfaceWithVarArgs(ts, obj, mid, args).s;
  }

  static jshort CallShortMethodA(JNIEnv* env, jobject obj, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeVirtualOrInterfaceWithJValues(ts, obj, mid, args).s;
  }

  static void CallVoidMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result = InvokeVirtualOrInterfaceWithVarArgs(ts, obj, mid, ap);
    va_end(ap);
  }

  static void CallVoidMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    InvokeVirtualOrInterfaceWithVarArgs(ts, obj, mid, args);
  }

  static void CallVoidMethodA(JNIEnv* env, jobject obj, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    InvokeVirtualOrInterfaceWithJValues(ts, obj, mid, args);
  }

  static jobject CallNonvirtualObjectMethod(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result = InvokeWithVarArgs(ts, obj, mid, ap);
    jobject local_result = AddLocalReference<jobject>(ts, result.l);
    va_end(ap);
    return local_result;
  }

  static jobject CallNonvirtualObjectMethodV(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    JValue result = InvokeWithVarArgs(ts, obj, mid, args);
    return AddLocalReference<jobject>(ts, result.l);
  }

  static jobject CallNonvirtualObjectMethodA(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    JValue result = InvokeWithJValues(ts, obj, mid, args);
    return AddLocalReference<jobject>(ts, result.l);
  }

  static jboolean CallNonvirtualBooleanMethod(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result = InvokeWithVarArgs(ts, obj, mid, ap);
    va_end(ap);
    return result.z;
  }

  static jboolean CallNonvirtualBooleanMethodV(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(ts, obj, mid, args).z;
  }

  static jboolean CallNonvirtualBooleanMethodA(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(ts, obj, mid, args).z;
  }

  static jbyte CallNonvirtualByteMethod(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result = InvokeWithVarArgs(ts, obj, mid, ap);
    va_end(ap);
    return result.b;
  }

  static jbyte CallNonvirtualByteMethodV(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(ts, obj, mid, args).b;
  }

  static jbyte CallNonvirtualByteMethodA(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(ts, obj, mid, args).b;
  }

  static jchar CallNonvirtualCharMethod(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result = InvokeWithVarArgs(ts, obj, mid, ap);
    va_end(ap);
    return result.c;
  }

  static jchar CallNonvirtualCharMethodV(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(ts, obj, mid, args).c;
  }

  static jchar CallNonvirtualCharMethodA(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(ts, obj, mid, args).c;
  }

  static jshort CallNonvirtualShortMethod(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result = InvokeWithVarArgs(ts, obj, mid, ap);
    va_end(ap);
    return result.s;
  }

  static jshort CallNonvirtualShortMethodV(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(ts, obj, mid, args).s;
  }

  static jshort CallNonvirtualShortMethodA(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(ts, obj, mid, args).s;
  }

  static jint CallNonvirtualIntMethod(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result = InvokeWithVarArgs(ts, obj, mid, ap);
    va_end(ap);
    return result.i;
  }

  static jint CallNonvirtualIntMethodV(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(ts, obj, mid, args).i;
  }

  static jint CallNonvirtualIntMethodA(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(ts, obj, mid, args).i;
  }

  static jlong CallNonvirtualLongMethod(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result = InvokeWithVarArgs(ts, obj, mid, ap);
    va_end(ap);
    return result.j;
  }

  static jlong CallNonvirtualLongMethodV(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(ts, obj, mid, args).j;
  }

  static jlong CallNonvirtualLongMethodA(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(ts, obj, mid, args).j;
  }

  static jfloat CallNonvirtualFloatMethod(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result = InvokeWithVarArgs(ts, obj, mid, ap);
    va_end(ap);
    return result.f;
  }

  static jfloat CallNonvirtualFloatMethodV(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(ts, obj, mid, args).f;
  }

  static jfloat CallNonvirtualFloatMethodA(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(ts, obj, mid, args).f;
  }

  static jdouble CallNonvirtualDoubleMethod(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result = InvokeWithVarArgs(ts, obj, mid, ap);
    va_end(ap);
    return result.d;
  }

  static jdouble CallNonvirtualDoubleMethodV(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(ts, obj, mid, args).d;
  }

  static jdouble CallNonvirtualDoubleMethodA(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(ts, obj, mid, args).d;
  }

  static void CallNonvirtualVoidMethod(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    InvokeWithVarArgs(ts, obj, mid, ap);
    va_end(ap);
  }

  static void CallNonvirtualVoidMethodV(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    InvokeWithVarArgs(ts, obj, mid, args);
  }

  static void CallNonvirtualVoidMethodA(JNIEnv* env,
      jobject obj, jclass clazz, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    InvokeWithJValues(ts, obj, mid, args);
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

  static jobject GetObjectField(JNIEnv* env, jobject obj, jfieldID fid) {
    ScopedJniThreadState ts(env);
    Object* o = Decode<Object*>(ts, obj);
    Field* f = DecodeField(ts, fid);
    return AddLocalReference<jobject>(ts, f->GetObject(o));
  }

  static jobject GetStaticObjectField(JNIEnv* env, jclass, jfieldID fid) {
    ScopedJniThreadState ts(env);
    Field* f = DecodeField(ts, fid);
    return AddLocalReference<jobject>(ts, f->GetObject(NULL));
  }

  static void SetObjectField(JNIEnv* env, jobject java_object, jfieldID fid, jobject java_value) {
    ScopedJniThreadState ts(env);
    Object* o = Decode<Object*>(ts, java_object);
    Object* v = Decode<Object*>(ts, java_value);
    Field* f = DecodeField(ts, fid);
    f->SetObject(o, v);
  }

  static void SetStaticObjectField(JNIEnv* env, jclass, jfieldID fid, jobject java_value) {
    ScopedJniThreadState ts(env);
    Object* v = Decode<Object*>(ts, java_value);
    Field* f = DecodeField(ts, fid);
    f->SetObject(NULL, v);
  }

#define GET_PRIMITIVE_FIELD(fn, instance) \
  ScopedJniThreadState ts(env); \
  Object* o = Decode<Object*>(ts, instance); \
  Field* f = DecodeField(ts, fid); \
  return f->fn(o)

#define SET_PRIMITIVE_FIELD(fn, instance, value) \
  ScopedJniThreadState ts(env); \
  Object* o = Decode<Object*>(ts, instance); \
  Field* f = DecodeField(ts, fid); \
  f->fn(o, value)

  static jboolean GetBooleanField(JNIEnv* env, jobject obj, jfieldID fid) {
    GET_PRIMITIVE_FIELD(GetBoolean, obj);
  }

  static jbyte GetByteField(JNIEnv* env, jobject obj, jfieldID fid) {
    GET_PRIMITIVE_FIELD(GetByte, obj);
  }

  static jchar GetCharField(JNIEnv* env, jobject obj, jfieldID fid) {
    GET_PRIMITIVE_FIELD(GetChar, obj);
  }

  static jshort GetShortField(JNIEnv* env, jobject obj, jfieldID fid) {
    GET_PRIMITIVE_FIELD(GetShort, obj);
  }

  static jint GetIntField(JNIEnv* env, jobject obj, jfieldID fid) {
    GET_PRIMITIVE_FIELD(GetInt, obj);
  }

  static jlong GetLongField(JNIEnv* env, jobject obj, jfieldID fid) {
    GET_PRIMITIVE_FIELD(GetLong, obj);
  }

  static jfloat GetFloatField(JNIEnv* env, jobject obj, jfieldID fid) {
    GET_PRIMITIVE_FIELD(GetFloat, obj);
  }

  static jdouble GetDoubleField(JNIEnv* env, jobject obj, jfieldID fid) {
    GET_PRIMITIVE_FIELD(GetDouble, obj);
  }

  static jboolean GetStaticBooleanField(JNIEnv* env, jclass clazz, jfieldID fid) {
    GET_PRIMITIVE_FIELD(GetBoolean, NULL);
  }

  static jbyte GetStaticByteField(JNIEnv* env, jclass clazz, jfieldID fid) {
    GET_PRIMITIVE_FIELD(GetByte, NULL);
  }

  static jchar GetStaticCharField(JNIEnv* env, jclass clazz, jfieldID fid) {
    GET_PRIMITIVE_FIELD(GetChar, NULL);
  }

  static jshort GetStaticShortField(JNIEnv* env, jclass clazz, jfieldID fid) {
    GET_PRIMITIVE_FIELD(GetShort, NULL);
  }

  static jint GetStaticIntField(JNIEnv* env, jclass clazz, jfieldID fid) {
    GET_PRIMITIVE_FIELD(GetInt, NULL);
  }

  static jlong GetStaticLongField(JNIEnv* env, jclass clazz, jfieldID fid) {
    GET_PRIMITIVE_FIELD(GetLong, NULL);
  }

  static jfloat GetStaticFloatField(JNIEnv* env, jclass clazz, jfieldID fid) {
    GET_PRIMITIVE_FIELD(GetFloat, NULL);
  }

  static jdouble GetStaticDoubleField(JNIEnv* env, jclass clazz, jfieldID fid) {
    GET_PRIMITIVE_FIELD(GetDouble, NULL);
  }

  static void SetBooleanField(JNIEnv* env, jobject obj, jfieldID fid, jboolean v) {
    SET_PRIMITIVE_FIELD(SetBoolean, obj, v);
  }

  static void SetByteField(JNIEnv* env, jobject obj, jfieldID fid, jbyte v) {
    SET_PRIMITIVE_FIELD(SetByte, obj, v);
  }

  static void SetCharField(JNIEnv* env, jobject obj, jfieldID fid, jchar v) {
    SET_PRIMITIVE_FIELD(SetChar, obj, v);
  }

  static void SetFloatField(JNIEnv* env, jobject obj, jfieldID fid, jfloat v) {
    SET_PRIMITIVE_FIELD(SetFloat, obj, v);
  }

  static void SetDoubleField(JNIEnv* env, jobject obj, jfieldID fid, jdouble v) {
    SET_PRIMITIVE_FIELD(SetDouble, obj, v);
  }

  static void SetIntField(JNIEnv* env, jobject obj, jfieldID fid, jint v) {
    SET_PRIMITIVE_FIELD(SetInt, obj, v);
  }

  static void SetLongField(JNIEnv* env, jobject obj, jfieldID fid, jlong v) {
    SET_PRIMITIVE_FIELD(SetLong, obj, v);
  }

  static void SetShortField(JNIEnv* env, jobject obj, jfieldID fid, jshort v) {
    SET_PRIMITIVE_FIELD(SetShort, obj, v);
  }

  static void SetStaticBooleanField(JNIEnv* env, jclass, jfieldID fid, jboolean v) {
    SET_PRIMITIVE_FIELD(SetBoolean, NULL, v);
  }

  static void SetStaticByteField(JNIEnv* env, jclass, jfieldID fid, jbyte v) {
    SET_PRIMITIVE_FIELD(SetByte, NULL, v);
  }

  static void SetStaticCharField(JNIEnv* env, jclass, jfieldID fid, jchar v) {
    SET_PRIMITIVE_FIELD(SetChar, NULL, v);
  }

  static void SetStaticFloatField(JNIEnv* env, jclass, jfieldID fid, jfloat v) {
    SET_PRIMITIVE_FIELD(SetFloat, NULL, v);
  }

  static void SetStaticDoubleField(JNIEnv* env, jclass, jfieldID fid, jdouble v) {
    SET_PRIMITIVE_FIELD(SetDouble, NULL, v);
  }

  static void SetStaticIntField(JNIEnv* env, jclass, jfieldID fid, jint v) {
    SET_PRIMITIVE_FIELD(SetInt, NULL, v);
  }

  static void SetStaticLongField(JNIEnv* env, jclass, jfieldID fid, jlong v) {
    SET_PRIMITIVE_FIELD(SetLong, NULL, v);
  }

  static void SetStaticShortField(JNIEnv* env, jclass, jfieldID fid, jshort v) {
    SET_PRIMITIVE_FIELD(SetShort, NULL, v);
  }

  static jobject CallStaticObjectMethod(JNIEnv* env,
      jclass clazz, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result = InvokeWithVarArgs(ts, NULL, mid, ap);
    jobject local_result = AddLocalReference<jobject>(ts, result.l);
    va_end(ap);
    return local_result;
  }

  static jobject CallStaticObjectMethodV(JNIEnv* env,
      jclass clazz, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    JValue result = InvokeWithVarArgs(ts, NULL, mid, args);
    return AddLocalReference<jobject>(ts, result.l);
  }

  static jobject CallStaticObjectMethodA(JNIEnv* env,
      jclass clazz, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    JValue result = InvokeWithJValues(ts, NULL, mid, args);
    return AddLocalReference<jobject>(ts, result.l);
  }

  static jboolean CallStaticBooleanMethod(JNIEnv* env,
      jclass clazz, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result = InvokeWithVarArgs(ts, NULL, mid, ap);
    va_end(ap);
    return result.z;
  }

  static jboolean CallStaticBooleanMethodV(JNIEnv* env,
      jclass clazz, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(ts, NULL, mid, args).z;
  }

  static jboolean CallStaticBooleanMethodA(JNIEnv* env,
      jclass clazz, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(ts, NULL, mid, args).z;
  }

  static jbyte CallStaticByteMethod(JNIEnv* env, jclass clazz, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result = InvokeWithVarArgs(ts, NULL, mid, ap);
    va_end(ap);
    return result.b;
  }

  static jbyte CallStaticByteMethodV(JNIEnv* env,
      jclass clazz, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(ts, NULL, mid, args).b;
  }

  static jbyte CallStaticByteMethodA(JNIEnv* env,
      jclass clazz, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(ts, NULL, mid, args).b;
  }

  static jchar CallStaticCharMethod(JNIEnv* env, jclass clazz, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result = InvokeWithVarArgs(ts, NULL, mid, ap);
    va_end(ap);
    return result.c;
  }

  static jchar CallStaticCharMethodV(JNIEnv* env,
      jclass clazz, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(ts, NULL, mid, args).c;
  }

  static jchar CallStaticCharMethodA(JNIEnv* env,
      jclass clazz, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(ts, NULL, mid, args).c;
  }

  static jshort CallStaticShortMethod(JNIEnv* env, jclass clazz, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result = InvokeWithVarArgs(ts, NULL, mid, ap);
    va_end(ap);
    return result.s;
  }

  static jshort CallStaticShortMethodV(JNIEnv* env,
      jclass clazz, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(ts, NULL, mid, args).s;
  }

  static jshort CallStaticShortMethodA(JNIEnv* env,
      jclass clazz, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(ts, NULL, mid, args).s;
  }

  static jint CallStaticIntMethod(JNIEnv* env, jclass clazz, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result = InvokeWithVarArgs(ts, NULL, mid, ap);
    va_end(ap);
    return result.i;
  }

  static jint CallStaticIntMethodV(JNIEnv* env,
      jclass clazz, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(ts, NULL, mid, args).i;
  }

  static jint CallStaticIntMethodA(JNIEnv* env,
      jclass clazz, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(ts, NULL, mid, args).i;
  }

  static jlong CallStaticLongMethod(JNIEnv* env, jclass clazz, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result = InvokeWithVarArgs(ts, NULL, mid, ap);
    va_end(ap);
    return result.j;
  }

  static jlong CallStaticLongMethodV(JNIEnv* env,
      jclass clazz, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(ts, NULL, mid, args).j;
  }

  static jlong CallStaticLongMethodA(JNIEnv* env,
      jclass clazz, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(ts, NULL, mid, args).j;
  }

  static jfloat CallStaticFloatMethod(JNIEnv* env, jclass cls, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result = InvokeWithVarArgs(ts, NULL, mid, ap);
    va_end(ap);
    return result.f;
  }

  static jfloat CallStaticFloatMethodV(JNIEnv* env,
      jclass clazz, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(ts, NULL, mid, args).f;
  }

  static jfloat CallStaticFloatMethodA(JNIEnv* env,
      jclass clazz, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(ts, NULL, mid, args).f;
  }

  static jdouble CallStaticDoubleMethod(JNIEnv* env, jclass cls, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result = InvokeWithVarArgs(ts, NULL, mid, ap);
    va_end(ap);
    return result.d;
  }

  static jdouble CallStaticDoubleMethodV(JNIEnv* env,
      jclass clazz, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(ts, NULL, mid, args).d;
  }

  static jdouble CallStaticDoubleMethodA(JNIEnv* env,
      jclass clazz, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(ts, NULL, mid, args).d;
  }

  static void CallStaticVoidMethod(JNIEnv* env, jclass cls, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    InvokeWithVarArgs(ts, NULL, mid, ap);
    va_end(ap);
  }

  static void CallStaticVoidMethodV(JNIEnv* env,
      jclass cls, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    InvokeWithVarArgs(ts, NULL, mid, args);
  }

  static void CallStaticVoidMethodA(JNIEnv* env,
      jclass cls, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    InvokeWithJValues(ts, NULL, mid, args);
  }

  static jstring NewString(JNIEnv* env, const jchar* chars, jsize char_count) {
    ScopedJniThreadState ts(env);
    if (chars == NULL && char_count == 0) {
      return NULL;
    }
    String* result = String::AllocFromUtf16(char_count, chars);
    return AddLocalReference<jstring>(ts, result);
  }

  static jstring NewStringUTF(JNIEnv* env, const char* utf) {
    ScopedJniThreadState ts(env);
    if (utf == NULL) {
      return NULL;
    }
    String* result = String::AllocFromModifiedUtf8(utf);
    return AddLocalReference<jstring>(ts, result);
  }

  static jsize GetStringLength(JNIEnv* env, jstring java_string) {
    ScopedJniThreadState ts(env);
    return Decode<String*>(ts, java_string)->GetLength();
  }

  static jsize GetStringUTFLength(JNIEnv* env, jstring java_string) {
    ScopedJniThreadState ts(env);
    return Decode<String*>(ts, java_string)->GetUtfLength();
  }

  static void GetStringRegion(JNIEnv* env, jstring java_string, jsize start, jsize length, jchar* buf) {
    ScopedJniThreadState ts(env);
    String* s = Decode<String*>(ts, java_string);
    if (start < 0 || length < 0 || start + length > s->GetLength()) {
      ThrowSIOOBE(ts, start, length, s->GetLength());
    } else {
      const jchar* chars = s->GetCharArray()->GetData() + s->GetOffset();
      memcpy(buf, chars + start, length * sizeof(jchar));
    }
  }

  static void GetStringUTFRegion(JNIEnv* env, jstring java_string, jsize start, jsize length, char* buf) {
    ScopedJniThreadState ts(env);
    String* s = Decode<String*>(ts, java_string);
    if (start < 0 || length < 0 || start + length > s->GetLength()) {
      ThrowSIOOBE(ts, start, length, s->GetLength());
    } else {
      const jchar* chars = s->GetCharArray()->GetData() + s->GetOffset();
      ConvertUtf16ToModifiedUtf8(buf, chars + start, length);
    }
  }

  static const jchar* GetStringChars(JNIEnv* env, jstring java_string, jboolean* is_copy) {
    ScopedJniThreadState ts(env);
    String* s = Decode<String*>(ts, java_string);
    const CharArray* chars = s->GetCharArray();
    PinPrimitiveArray(ts, chars);
    if (is_copy != NULL) {
      *is_copy = JNI_FALSE;
    }
    return chars->GetData() + s->GetOffset();
  }

  static void ReleaseStringChars(JNIEnv* env, jstring java_string, const jchar* chars) {
    ScopedJniThreadState ts(env);
    UnpinPrimitiveArray(ts, Decode<String*>(ts, java_string)->GetCharArray());
  }

  static const jchar* GetStringCritical(JNIEnv* env, jstring java_string, jboolean* is_copy) {
    ScopedJniThreadState ts(env);
    return GetStringChars(env, java_string, is_copy);
  }

  static void ReleaseStringCritical(JNIEnv* env, jstring java_string, const jchar* chars) {
    ScopedJniThreadState ts(env);
    return ReleaseStringChars(env, java_string, chars);
  }

  static const char* GetStringUTFChars(JNIEnv* env, jstring java_string, jboolean* is_copy) {
    ScopedJniThreadState ts(env);
    if (java_string == NULL) {
      return NULL;
    }
    if (is_copy != NULL) {
      *is_copy = JNI_TRUE;
    }
    String* s = Decode<String*>(ts, java_string);
    size_t byte_count = s->GetUtfLength();
    char* bytes = new char[byte_count + 1];
    if (bytes == NULL) {
      ts.Self()->ThrowOutOfMemoryError();
      return NULL;
    }
    const uint16_t* chars = s->GetCharArray()->GetData() + s->GetOffset();
    ConvertUtf16ToModifiedUtf8(bytes, chars, s->GetLength());
    bytes[byte_count] = '\0';
    return bytes;
  }

  static void ReleaseStringUTFChars(JNIEnv* env, jstring, const char* chars) {
    ScopedJniThreadState ts(env);
    delete[] chars;
  }

  static jsize GetArrayLength(JNIEnv* env, jarray java_array) {
    ScopedJniThreadState ts(env);
    Object* obj = Decode<Object*>(ts, java_array);
    CHECK(obj->IsArrayInstance()); // TODO: ReportJniError
    Array* array = obj->AsArray();
    return array->GetLength();
  }

  static jobject GetObjectArrayElement(JNIEnv* env, jobjectArray java_array, jsize index) {
    ScopedJniThreadState ts(env);
    ObjectArray<Object>* array = Decode<ObjectArray<Object>*>(ts, java_array);
    return AddLocalReference<jobject>(ts, array->Get(index));
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
    descriptor += element_class->GetDescriptor()->ToModifiedUtf8();

    // Find the class.
    ScopedLocalRef<jclass> java_array_class(env, FindClass(env, descriptor.c_str()));
    if (java_array_class.get() == NULL) {
      return NULL;
    }

    // Allocate and initialize if necessary.
    Class* array_class = Decode<Class*>(ts, java_array_class.get());
    ObjectArray<Object>* result = ObjectArray<Object>::Alloc(array_class, length);
    if (initial_element != NULL) {
      Object* initial_object = Decode<Object*>(ts, initial_element);
      for (jsize i = 0; i < length; ++i) {
        result->Set(i, initial_object);
      }
    }
    return AddLocalReference<jobjectArray>(ts, result);
  }

  static jshortArray NewShortArray(JNIEnv* env, jsize length) {
    ScopedJniThreadState ts(env);
    return NewPrimitiveArray<jshortArray, ShortArray>(ts, length);
  }

  static void* GetPrimitiveArrayCritical(JNIEnv* env, jarray array, jboolean* is_copy) {
    ScopedJniThreadState ts(env);
    return GetPrimitiveArray<jarray, jbyte*, ByteArray>(ts, array, is_copy);
  }

  static void ReleasePrimitiveArrayCritical(JNIEnv* env, jarray array, void* data, jint mode) {
    ScopedJniThreadState ts(env);
    ReleasePrimitiveArray(ts, array, mode);
  }

  static jboolean* GetBooleanArrayElements(JNIEnv* env, jbooleanArray array, jboolean* is_copy) {
    ScopedJniThreadState ts(env);
    return GetPrimitiveArray<jbooleanArray, jboolean*, BooleanArray>(ts, array, is_copy);
  }

  static jbyte* GetByteArrayElements(JNIEnv* env, jbyteArray array, jboolean* is_copy) {
    ScopedJniThreadState ts(env);
    return GetPrimitiveArray<jbyteArray, jbyte*, ByteArray>(ts, array, is_copy);
  }

  static jchar* GetCharArrayElements(JNIEnv* env, jcharArray array, jboolean* is_copy) {
    ScopedJniThreadState ts(env);
    return GetPrimitiveArray<jcharArray, jchar*, CharArray>(ts, array, is_copy);
  }

  static jdouble* GetDoubleArrayElements(JNIEnv* env, jdoubleArray array, jboolean* is_copy) {
    ScopedJniThreadState ts(env);
    return GetPrimitiveArray<jdoubleArray, jdouble*, DoubleArray>(ts, array, is_copy);
  }

  static jfloat* GetFloatArrayElements(JNIEnv* env, jfloatArray array, jboolean* is_copy) {
    ScopedJniThreadState ts(env);
    return GetPrimitiveArray<jfloatArray, jfloat*, FloatArray>(ts, array, is_copy);
  }

  static jint* GetIntArrayElements(JNIEnv* env, jintArray array, jboolean* is_copy) {
    ScopedJniThreadState ts(env);
    return GetPrimitiveArray<jintArray, jint*, IntArray>(ts, array, is_copy);
  }

  static jlong* GetLongArrayElements(JNIEnv* env, jlongArray array, jboolean* is_copy) {
    ScopedJniThreadState ts(env);
    return GetPrimitiveArray<jlongArray, jlong*, LongArray>(ts, array, is_copy);
  }

  static jshort* GetShortArrayElements(JNIEnv* env, jshortArray array, jboolean* is_copy) {
    ScopedJniThreadState ts(env);
    return GetPrimitiveArray<jshortArray, jshort*, ShortArray>(ts, array, is_copy);
  }

  static void ReleaseBooleanArrayElements(JNIEnv* env, jbooleanArray array, jboolean* data, jint mode) {
    ScopedJniThreadState ts(env);
    ReleasePrimitiveArray(ts, array, mode);
  }

  static void ReleaseByteArrayElements(JNIEnv* env, jbyteArray array, jbyte* data, jint mode) {
    ScopedJniThreadState ts(env);
    ReleasePrimitiveArray(ts, array, mode);
  }

  static void ReleaseCharArrayElements(JNIEnv* env, jcharArray array, jchar* data, jint mode) {
    ScopedJniThreadState ts(env);
    ReleasePrimitiveArray(ts, array, mode);
  }

  static void ReleaseDoubleArrayElements(JNIEnv* env, jdoubleArray array, jdouble* data, jint mode) {
    ScopedJniThreadState ts(env);
    ReleasePrimitiveArray(ts, array, mode);
  }

  static void ReleaseFloatArrayElements(JNIEnv* env, jfloatArray array, jfloat* data, jint mode) {
    ScopedJniThreadState ts(env);
    ReleasePrimitiveArray(ts, array, mode);
  }

  static void ReleaseIntArrayElements(JNIEnv* env, jintArray array, jint* data, jint mode) {
    ScopedJniThreadState ts(env);
    ReleasePrimitiveArray(ts, array, mode);
  }

  static void ReleaseLongArrayElements(JNIEnv* env, jlongArray array, jlong* data, jint mode) {
    ScopedJniThreadState ts(env);
    ReleasePrimitiveArray(ts, array, mode);
  }

  static void ReleaseShortArrayElements(JNIEnv* env, jshortArray array, jshort* data, jint mode) {
    ScopedJniThreadState ts(env);
    ReleasePrimitiveArray(ts, array, mode);
  }

  static void GetBooleanArrayRegion(JNIEnv* env, jbooleanArray array, jsize start, jsize length, jboolean* buf) {
    ScopedJniThreadState ts(env);
    GetPrimitiveArrayRegion<jbooleanArray, jboolean, BooleanArray>(ts, array, start, length, buf);
  }

  static void GetByteArrayRegion(JNIEnv* env, jbyteArray array, jsize start, jsize length, jbyte* buf) {
    ScopedJniThreadState ts(env);
    GetPrimitiveArrayRegion<jbyteArray, jbyte, ByteArray>(ts, array, start, length, buf);
  }

  static void GetCharArrayRegion(JNIEnv* env, jcharArray array, jsize start, jsize length, jchar* buf) {
    ScopedJniThreadState ts(env);
    GetPrimitiveArrayRegion<jcharArray, jchar, CharArray>(ts, array, start, length, buf);
  }

  static void GetDoubleArrayRegion(JNIEnv* env, jdoubleArray array, jsize start, jsize length, jdouble* buf) {
    ScopedJniThreadState ts(env);
    GetPrimitiveArrayRegion<jdoubleArray, jdouble, DoubleArray>(ts, array, start, length, buf);
  }

  static void GetFloatArrayRegion(JNIEnv* env, jfloatArray array, jsize start, jsize length, jfloat* buf) {
    ScopedJniThreadState ts(env);
    GetPrimitiveArrayRegion<jfloatArray, jfloat, FloatArray>(ts, array, start, length, buf);
  }

  static void GetIntArrayRegion(JNIEnv* env, jintArray array, jsize start, jsize length, jint* buf) {
    ScopedJniThreadState ts(env);
    GetPrimitiveArrayRegion<jintArray, jint, IntArray>(ts, array, start, length, buf);
  }

  static void GetLongArrayRegion(JNIEnv* env, jlongArray array, jsize start, jsize length, jlong* buf) {
    ScopedJniThreadState ts(env);
    GetPrimitiveArrayRegion<jlongArray, jlong, LongArray>(ts, array, start, length, buf);
  }

  static void GetShortArrayRegion(JNIEnv* env, jshortArray array, jsize start, jsize length, jshort* buf) {
    ScopedJniThreadState ts(env);
    GetPrimitiveArrayRegion<jshortArray, jshort, ShortArray>(ts, array, start, length, buf);
  }

  static void SetBooleanArrayRegion(JNIEnv* env, jbooleanArray array, jsize start, jsize length, const jboolean* buf) {
    ScopedJniThreadState ts(env);
    SetPrimitiveArrayRegion<jbooleanArray, jboolean, BooleanArray>(ts, array, start, length, buf);
  }

  static void SetByteArrayRegion(JNIEnv* env, jbyteArray array, jsize start, jsize length, const jbyte* buf) {
    ScopedJniThreadState ts(env);
    SetPrimitiveArrayRegion<jbyteArray, jbyte, ByteArray>(ts, array, start, length, buf);
  }

  static void SetCharArrayRegion(JNIEnv* env, jcharArray array, jsize start, jsize length, const jchar* buf) {
    ScopedJniThreadState ts(env);
    SetPrimitiveArrayRegion<jcharArray, jchar, CharArray>(ts, array, start, length, buf);
  }

  static void SetDoubleArrayRegion(JNIEnv* env, jdoubleArray array, jsize start, jsize length, const jdouble* buf) {
    ScopedJniThreadState ts(env);
    SetPrimitiveArrayRegion<jdoubleArray, jdouble, DoubleArray>(ts, array, start, length, buf);
  }

  static void SetFloatArrayRegion(JNIEnv* env, jfloatArray array, jsize start, jsize length, const jfloat* buf) {
    ScopedJniThreadState ts(env);
    SetPrimitiveArrayRegion<jfloatArray, jfloat, FloatArray>(ts, array, start, length, buf);
  }

  static void SetIntArrayRegion(JNIEnv* env, jintArray array, jsize start, jsize length, const jint* buf) {
    ScopedJniThreadState ts(env);
    SetPrimitiveArrayRegion<jintArray, jint, IntArray>(ts, array, start, length, buf);
  }

  static void SetLongArrayRegion(JNIEnv* env, jlongArray array, jsize start, jsize length, const jlong* buf) {
    ScopedJniThreadState ts(env);
    SetPrimitiveArrayRegion<jlongArray, jlong, LongArray>(ts, array, start, length, buf);
  }

  static void SetShortArrayRegion(JNIEnv* env, jshortArray array, jsize start, jsize length, const jshort* buf) {
    ScopedJniThreadState ts(env);
    SetPrimitiveArrayRegion<jshortArray, jshort, ShortArray>(ts, array, start, length, buf);
  }

  static jint RegisterNatives(JNIEnv* env, jclass java_class, const JNINativeMethod* methods, jint method_count) {
    ScopedJniThreadState ts(env);
    Class* c = Decode<Class*>(ts, java_class);

    for (int i = 0; i < method_count; i++) {
      const char* name = methods[i].name;
      const char* sig = methods[i].signature;

      if (*sig == '!') {
        // TODO: fast jni. it's too noisy to log all these.
        ++sig;
      }

      Method* m = c->FindDirectMethod(name, sig);
      if (m == NULL) {
        m = c->FindVirtualMethod(name, sig);
      }
      if (m == NULL) {
        Thread* self = Thread::Current();
        std::string class_descriptor(c->GetDescriptor()->ToModifiedUtf8());
        self->ThrowNewException("Ljava/lang/NoSuchMethodError;",
            "no method \"%s.%s%s\"",
            class_descriptor.c_str(), name, sig);
        return JNI_ERR;
      } else if (!m->IsNative()) {
        Thread* self = Thread::Current();
        std::string class_descriptor(c->GetDescriptor()->ToModifiedUtf8());
        self->ThrowNewException("Ljava/lang/NoSuchMethodError;",
            "method \"%s.%s%s\" is not native",
            class_descriptor.c_str(), name, sig);
        return JNI_ERR;
      }

      if (ts.Vm()->verbose_jni) {
        LOG(INFO) << "[Registering JNI native method "
                  << PrettyMethod(m, true) << "]";
      }

      m->RegisterNative(methods[i].fnPtr);
    }
    return JNI_OK;
  }

  static jint UnregisterNatives(JNIEnv* env, jclass java_class) {
    ScopedJniThreadState ts(env);
    Class* c = Decode<Class*>(ts, java_class);

    if (ts.Vm()->verbose_jni) {
      LOG(INFO) << "[Unregistering JNI native methods for "
                << PrettyDescriptor(c->GetDescriptor()) << "]";
    }

    for (size_t i = 0; i < c->NumDirectMethods(); ++i) {
      Method* m = c->GetDirectMethod(i);
      if (m->IsNative()) {
        m->UnregisterNative();
      }
    }
    for (size_t i = 0; i < c->NumVirtualMethods(); ++i) {
      Method* m = c->GetVirtualMethod(i);
      if (m->IsNative()) {
        m->UnregisterNative();
      }
    }

    return JNI_OK;
  }

  static jint MonitorEnter(JNIEnv* env, jobject java_object) {
    ScopedJniThreadState ts(env);
    Decode<Object*>(ts, java_object)->MonitorEnter();
    return ts.Self()->IsExceptionPending() ? JNI_ERR : JNI_OK;
  }

  static jint MonitorExit(JNIEnv* env, jobject java_object) {
    ScopedJniThreadState ts(env);
    Decode<Object*>(ts, java_object)->MonitorEnter();
    return ts.Self()->IsExceptionPending() ? JNI_ERR : JNI_OK;
  }

  static jint GetJavaVM(JNIEnv* env, JavaVM** vm) {
    ScopedJniThreadState ts(env);
    Runtime* runtime = Runtime::Current();
    if (runtime != NULL) {
      *vm = runtime->GetJavaVM();
    } else {
      *vm = NULL;
    }
    return (*vm != NULL) ? JNI_OK : JNI_ERR;
  }

  static jobject NewDirectByteBuffer(JNIEnv* env, void* address, jlong capacity) {
    ScopedJniThreadState ts(env);

    // The address may not be NULL, and the capacity must be > 0.
    CHECK(address != NULL); // TODO: ReportJniError
    CHECK_GT(capacity, 0); // TODO: ReportJniError

    jclass buffer_class = GetDirectByteBufferClass(env);
    jmethodID mid = env->GetMethodID(buffer_class, "<init>", "(II)V");
    if (mid == NULL) {
      return NULL;
    }

    // At the moment, the Java side is limited to 32 bits.
    CHECK_LE(reinterpret_cast<uintptr_t>(address), 0xffffffff);
    CHECK_LE(capacity, 0xffffffff);
    jint address_arg = reinterpret_cast<jint>(address);
    jint capacity_arg = static_cast<jint>(capacity);

    jobject result = env->NewObject(buffer_class, mid, address_arg, capacity_arg);
    return ts.Self()->IsExceptionPending() ? NULL : result;
  }

  static void* GetDirectBufferAddress(JNIEnv* env, jobject java_buffer) {
    ScopedJniThreadState ts(env);
    static jfieldID fid = env->GetFieldID(GetDirectByteBufferClass(env), "effectiveDirectAddress", "I");
    return reinterpret_cast<void*>(env->GetIntField(java_buffer, fid));
  }

  static jlong GetDirectBufferCapacity(JNIEnv* env, jobject java_buffer) {
    ScopedJniThreadState ts(env);
    static jfieldID fid = env->GetFieldID(GetDirectByteBufferClass(env), "capacity", "I");
    return static_cast<jlong>(env->GetIntField(java_buffer, fid));
  }

  static jobjectRefType GetObjectRefType(JNIEnv* env, jobject java_object) {
    ScopedJniThreadState ts(env);

    CHECK(java_object != NULL); // TODO: ReportJniError

    // Do we definitely know what kind of reference this is?
    IndirectRef ref = reinterpret_cast<IndirectRef>(java_object);
    IndirectRefKind kind = GetIndirectRefKind(ref);
    switch (kind) {
    case kLocal:
      return JNILocalRefType;
    case kGlobal:
      return JNIGlobalRefType;
    case kWeakGlobal:
      return JNIWeakGlobalRefType;
    case kSirtOrInvalid:
      // Is it in a stack IRT?
      if (ts.Self()->SirtContains(java_object)) {
        return JNILocalRefType;
      }

      // If we're handing out direct pointers, check whether it's a direct pointer
      // to a local reference.
      // TODO: replace 'false' with the replacement for gDvmJni.workAroundAppJniBugs
      if (false && Decode<Object*>(ts, java_object) == reinterpret_cast<Object*>(java_object)) {
        if (ts.Env()->locals.Contains(java_object)) {
          return JNILocalRefType;
        }
      }

      return JNIInvalidRefType;
    }
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

JNIEnvExt::JNIEnvExt(Thread* self, JavaVMExt* vm)
    : self(self),
      vm(vm),
      check_jni(vm->check_jni),
      critical(false),
      monitors("monitors", kMonitorsInitial, kMonitorsMax),
      locals(kLocalsInitial, kLocalsMax, kLocal) {
  functions = &gNativeInterface;
}

JNIEnvExt::~JNIEnvExt() {
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
    *p_env = Thread::Current()->GetJniEnv();
    *p_vm = runtime->GetJavaVM();
    return JNI_OK;
  }
}

extern "C" jint JNI_GetCreatedJavaVMs(JavaVM** vms, jsize, jsize* vm_count) {
  Runtime* runtime = Runtime::Current();
  if (runtime == NULL) {
    *vm_count = 0;
  } else {
    *vm_count = 1;
    vms[0] = runtime->GetJavaVM();
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
    return JII_AttachCurrentThread(vm, p_env, thr_args, false);
  }

  static jint AttachCurrentThreadAsDaemon(JavaVM* vm, JNIEnv** p_env, void* thr_args) {
    return JII_AttachCurrentThread(vm, p_env, thr_args, true);
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
    : runtime(runtime),
      check_jni(check_jni),
      verbose_jni(verbose_jni),
      pins_lock(Mutex::Create("JNI pin table lock")),
      pin_table("pin table", kPinTableInitialSize, kPinTableMaxSize),
      globals_lock(Mutex::Create("JNI global reference table lock")),
      globals(kGlobalsInitial, kGlobalsMax, kGlobal),
      weak_globals_lock(Mutex::Create("JNI weak global reference table lock")),
      weak_globals(kWeakGlobalsInitial, kWeakGlobalsMax, kWeakGlobal),
      libraries_lock(Mutex::Create("JNI shared libraries map lock")),
      libraries(new Libraries) {
  functions = &gInvokeInterface;
}

JavaVMExt::~JavaVMExt() {
  delete pins_lock;
  delete globals_lock;
  delete weak_globals_lock;
  delete libraries_lock;
  delete libraries;
}

bool JavaVMExt::LoadNativeLibrary(const std::string& path, ClassLoader* class_loader, std::string& detail) {
  detail.clear();

  // See if we've already loaded this library.  If we have, and the class loader
  // matches, return successfully without doing anything.
  // TODO: for better results we should canonicalize the pathname (or even compare
  // inodes). This implementation is fine if everybody is using System.loadLibrary.
  SharedLibrary* library;
  {
    // TODO: move the locking (and more of this logic) into Libraries.
    MutexLock mu(libraries_lock);
    library = libraries->Get(path);
  }
  if (library != NULL) {
    if (library->GetClassLoader() != class_loader) {
      // The library will be associated with class_loader. The JNI
      // spec says we can't load the same library into more than one
      // class loader.
      StringAppendF(&detail, "Shared library \"%s\" already opened by "
          "ClassLoader %p; can't open in ClassLoader %p",
          path.c_str(), library->GetClassLoader(), class_loader);
      LOG(WARNING) << detail;
      return false;
    }
    if (verbose_jni) {
      LOG(INFO) << "[Shared library \"" << path << "\" already loaded in "
                << "ClassLoader " << class_loader << "]";
    }
    if (!library->CheckOnLoadResult(this)) {
      StringAppendF(&detail, "JNI_OnLoad failed on a previous attempt "
          "to load \"%s\"", path.c_str());
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
    detail = dlerror();
    return false;
  }

  // Create a new entry.
  {
    // TODO: move the locking (and more of this logic) into Libraries.
    MutexLock mu(libraries_lock);
    library = libraries->Get(path);
    if (library != NULL) {
      LOG(INFO) << "WOW: we lost a race to add shared library: "
                << "\"" << path << "\" ClassLoader=" << class_loader;
      return library->CheckOnLoadResult(this);
    }
    library = new SharedLibrary(path, handle, class_loader);
    libraries->Put(path, library);
  }

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
    typedef int (*JNI_OnLoadFn)(JavaVM*, void*);
    JNI_OnLoadFn jni_on_load = reinterpret_cast<JNI_OnLoadFn>(sym);
    const ClassLoader* old_class_loader = self->GetClassLoaderOverride();
    self->SetClassLoaderOverride(class_loader);

    old_state = self->GetState();
    self->SetState(Thread::kNative);
    if (verbose_jni) {
      LOG(INFO) << "[Calling JNI_OnLoad in \"" << path << "\"]";
    }
    int version = (*jni_on_load)(this, NULL);
    self->SetState(old_state);

    self->SetClassLoaderOverride(old_class_loader);;

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

void* JavaVMExt::FindCodeForNativeMethod(Method* m) {
  CHECK(m->IsNative());

  Class* c = m->GetDeclaringClass();

  // If this is a static method, it could be called before the class
  // has been initialized.
  if (m->IsStatic()) {
    if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(c)) {
      return NULL;
    }
  } else {
    CHECK_GE(c->GetStatus(), Class::kStatusInitializing);
  }

  MutexLock mu(libraries_lock);
  return libraries->FindNativeMethod(m);
}

}  // namespace art

std::ostream& operator<<(std::ostream& os, const jobjectRefType& rhs) {
  switch (rhs) {
  case JNIInvalidRefType:
    os << "JNIInvalidRefType";
    return os;
  case JNILocalRefType:
    os << "JNILocalRefType";
    return os;
  case JNIGlobalRefType:
    os << "JNIGlobalRefType";
    return os;
  case JNIWeakGlobalRefType:
    os << "JNIWeakGlobalRefType";
    return os;
  }
}
