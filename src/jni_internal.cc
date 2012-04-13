/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "jni_internal.h"

#include <dlfcn.h>
#include <sys/mman.h>

#include <cstdarg>
#include <utility>
#include <vector>

#include "class_linker.h"
#include "class_loader.h"
#include "jni.h"
#include "logging.h"
#include "object.h"
#include "object_utils.h"
#include "runtime.h"
#include "safe_map.h"
#include "scoped_jni_thread_state.h"
#include "ScopedLocalRef.h"
#include "stl_util.h"
#include "stringpiece.h"
#include "thread.h"
#include "UniquePtr.h"

namespace art {

static const size_t kMonitorsInitial = 32; // Arbitrary.
static const size_t kMonitorsMax = 4096; // Arbitrary sanity check.

static const size_t kLocalsInitial = 64; // Arbitrary.
static const size_t kLocalsMax = 512; // Arbitrary sanity check.

static const size_t kPinTableInitial = 16; // Arbitrary.
static const size_t kPinTableMax = 1024; // Arbitrary sanity check.

static size_t gGlobalsInitial = 512; // Arbitrary.
static size_t gGlobalsMax = 51200; // Arbitrary sanity check.

static const size_t kWeakGlobalsInitial = 16; // Arbitrary.
static const size_t kWeakGlobalsMax = 51200; // Arbitrary sanity check.

void SetJniGlobalsMax(size_t max) {
  if (max != 0) {
    gGlobalsMax = max;
    gGlobalsInitial = std::min(gGlobalsInitial, gGlobalsMax);
  }
}

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
T AddLocalReference(JNIEnv* public_env, const Object* const_obj) {
  // The jobject type hierarchy has no notion of const, so it's not worth carrying through.
  Object* obj = const_cast<Object*>(const_obj);

  if (obj == NULL) {
    return NULL;
  }

  DCHECK((reinterpret_cast<uintptr_t>(obj) & 0xffff0000) != 0xebad0000);

  JNIEnvExt* env = reinterpret_cast<JNIEnvExt*>(public_env);
  IndirectReferenceTable& locals = env->locals;

  uint32_t cookie = env->local_ref_cookie;
  IndirectRef ref = locals.Add(cookie, obj);
  if (ref == NULL) {
    // TODO: just change Add's DCHECK to CHECK and lose this?
    locals.Dump();
    LOG(FATAL) << "Failed adding to JNI local reference table "
               << "(has " << locals.Capacity() << " entries)";
  }

#if 0 // TODO: fix this to understand PushLocalFrame, so we can turn it on.
  if (env->check_jni) {
    size_t entry_count = locals.Capacity();
    if (entry_count > 16) {
      LOG(WARNING) << "Warning: more than 16 JNI local references: "
                   << entry_count << " (most recent was a " << PrettyTypeOf(obj) << ")";
      locals.Dump();
      // TODO: LOG(FATAL) instead.
    }
  }
#endif

  if (env->vm->work_around_app_jni_bugs) {
    // Hand out direct pointers to support broken old apps.
    return reinterpret_cast<T>(obj);
  }

  return reinterpret_cast<T>(ref);
}
// Explicit instantiations
template jclass AddLocalReference<jclass>(JNIEnv* public_env, const Object* const_obj);
template jobject AddLocalReference<jobject>(JNIEnv* public_env, const Object* const_obj);
template jobjectArray AddLocalReference<jobjectArray>(JNIEnv* public_env, const Object* const_obj);
template jstring AddLocalReference<jstring>(JNIEnv* public_env, const Object* const_obj);
template jthrowable AddLocalReference<jthrowable>(JNIEnv* public_env, const Object* const_obj);

// For external use.
template<typename T>
T Decode(JNIEnv* public_env, jobject obj) {
  JNIEnvExt* env = reinterpret_cast<JNIEnvExt*>(public_env);
  return reinterpret_cast<T>(env->self->DecodeJObject(obj));
}
// TODO: Change to use template when Mac OS build server no longer uses GCC 4.2.*.
Object* DecodeObj(JNIEnv* public_env, jobject obj) {
  JNIEnvExt* env = reinterpret_cast<JNIEnvExt*>(public_env);
  return reinterpret_cast<Object*>(env->self->DecodeJObject(obj));
}
// Explicit instantiations.
template Array* Decode<Array*>(JNIEnv*, jobject);
template Class* Decode<Class*>(JNIEnv*, jobject);
template ClassLoader* Decode<ClassLoader*>(JNIEnv*, jobject);
template Object* Decode<Object*>(JNIEnv*, jobject);
template ObjectArray<Class>* Decode<ObjectArray<Class>*>(JNIEnv*, jobject);
template ObjectArray<ObjectArray<Class> >* Decode<ObjectArray<ObjectArray<Class> >*>(JNIEnv*, jobject);
template ObjectArray<Object>* Decode<ObjectArray<Object>*>(JNIEnv*, jobject);
template ObjectArray<StackTraceElement>* Decode<ObjectArray<StackTraceElement>*>(JNIEnv*, jobject);
template ObjectArray<Method>* Decode<ObjectArray<Method>*>(JNIEnv*, jobject);
template String* Decode<String*>(JNIEnv*, jobject);
template Throwable* Decode<Throwable*>(JNIEnv*, jobject);

size_t NumArgArrayBytes(const char* shorty, uint32_t shorty_len) {
  size_t num_bytes = 0;
  for (size_t i = 1; i < shorty_len; ++i) {
    char ch = shorty[i];
    if (ch == 'D' || ch == 'J') {
      num_bytes += 8;
    } else if (ch == 'L') {
      // Argument is a reference or an array.  The shorty descriptor
      // does not distinguish between these types.
      num_bytes += sizeof(Object*);
    } else {
      num_bytes += 4;
    }
  }
  return num_bytes;
}

class ArgArray {
 public:
  explicit ArgArray(Method* method) {
    MethodHelper mh(method);
    shorty_ = mh.GetShorty();
    shorty_len_ = mh.GetShortyLength();
    if (shorty_len_ - 1 < kSmallArgArraySize) {
      arg_array_ = small_arg_array_;
    } else {
      large_arg_array_.reset(new JValue[shorty_len_ - 1]);
      arg_array_ = large_arg_array_.get();
    }
  }

  JValue* get() {
    return arg_array_;
  }

  void BuildArgArray(JNIEnv* public_env, va_list ap) {
    JNIEnvExt* env = reinterpret_cast<JNIEnvExt*>(public_env);
    for (size_t i = 1, offset = 0; i < shorty_len_; ++i, ++offset) {
      switch (shorty_[i]) {
        case 'Z':
          arg_array_[offset].SetZ(va_arg(ap, jint));
          break;
        case 'B':
          arg_array_[offset].SetB(va_arg(ap, jint));
          break;
        case 'C':
          arg_array_[offset].SetC(va_arg(ap, jint));
          break;
        case 'S':
          arg_array_[offset].SetS(va_arg(ap, jint));
          break;
        case 'I':
          arg_array_[offset].SetI(va_arg(ap, jint));
          break;
        case 'F':
          arg_array_[offset].SetF(va_arg(ap, jdouble));
          break;
        case 'L':
          arg_array_[offset].SetL(DecodeObj(env, va_arg(ap, jobject)));
          break;
        case 'D':
          arg_array_[offset].SetD(va_arg(ap, jdouble));
          break;
        case 'J':
          arg_array_[offset].SetJ(va_arg(ap, jlong));
          break;
      }
    }
  }

  void BuildArgArray(JNIEnv* public_env, jvalue* args) {
    JNIEnvExt* env = reinterpret_cast<JNIEnvExt*>(public_env);
    for (size_t i = 1, offset = 0; i < shorty_len_; ++i, ++offset) {
      switch (shorty_[i]) {
        case 'Z':
          arg_array_[offset].SetZ(args[offset].z);
          break;
        case 'B':
          arg_array_[offset].SetB(args[offset].b);
          break;
        case 'C':
          arg_array_[offset].SetC(args[offset].c);
          break;
        case 'S':
          arg_array_[offset].SetS(args[offset].s);
          break;
        case 'I':
          arg_array_[offset].SetI(args[offset].i);
          break;
        case 'F':
          arg_array_[offset].SetF(args[offset].f);
          break;
        case 'L':
          arg_array_[offset].SetL(DecodeObj(env, args[offset].l));
          break;
        case 'D':
          arg_array_[offset].SetD(args[offset].d);
          break;
        case 'J':
          arg_array_[offset].SetJ(args[offset].j);
          break;
      }
    }
  }

 private:
  enum { kSmallArgArraySize = 16 };
  const char* shorty_;
  uint32_t shorty_len_;
  JValue* arg_array_;
  JValue small_arg_array_[kSmallArgArraySize];
  UniquePtr<JValue[]> large_arg_array_;
};

static jweak AddWeakGlobalReference(ScopedJniThreadState& ts, Object* obj) {
  if (obj == NULL) {
    return NULL;
  }
  JavaVMExt* vm = ts.Vm();
  IndirectReferenceTable& weak_globals = vm->weak_globals;
  MutexLock mu(vm->weak_globals_lock);
  IndirectRef ref = weak_globals.Add(IRT_FIRST_SEGMENT, obj);
  return reinterpret_cast<jweak>(ref);
}

// For internal use.
template<typename T>
static T Decode(ScopedJniThreadState& ts, jobject obj) {
  return reinterpret_cast<T>(ts.Self()->DecodeJObject(obj));
}

static void CheckMethodArguments(Method* m, JValue* args) {
  MethodHelper mh(m);
  ObjectArray<Class>* parameter_types = mh.GetParameterTypes();
  CHECK(parameter_types != NULL);
  size_t error_count = 0;
  for (int i = 0; i < parameter_types->GetLength(); ++i) {
    Class* parameter_type = parameter_types->Get(i);
    // TODO: check primitives are in range.
    if (!parameter_type->IsPrimitive()) {
      Object* argument = args[i].GetL();
      if (argument != NULL && !argument->InstanceOf(parameter_type)) {
        LOG(ERROR) << "JNI ERROR (app bug): attempt to pass an instance of "
                   << PrettyTypeOf(argument) << " as argument " << (i + 1) << " to " << PrettyMethod(m);
        ++error_count;
      }
    }
  }
  if (error_count > 0) {
    // TODO: pass the JNI function name (such as "CallVoidMethodV") through so we can call JniAbort
    // with an argument.
    JniAbort(NULL);
  }
}

static JValue InvokeWithArgArray(JNIEnv* public_env, Object* receiver, Method* method, JValue* args) {
  JNIEnvExt* env = reinterpret_cast<JNIEnvExt*>(public_env);
  if (UNLIKELY(env->check_jni)) {
    CheckMethodArguments(method, args);
  }
  JValue result;
  method->Invoke(env->self, receiver, args, &result);
  return result;
}

static JValue InvokeWithVarArgs(JNIEnv* public_env, jobject obj, jmethodID mid, va_list args) {
  JNIEnvExt* env = reinterpret_cast<JNIEnvExt*>(public_env);
  Object* receiver = DecodeObj(env, obj);
  Method* method = DecodeMethod(mid);
  ArgArray arg_array(method);
  arg_array.BuildArgArray(env, args);
  return InvokeWithArgArray(env, receiver, method, arg_array.get());
}

static Method* FindVirtualMethod(Object* receiver, Method* method) {
  return receiver->GetClass()->FindVirtualMethodForVirtualOrInterface(method);
}

static JValue InvokeVirtualOrInterfaceWithJValues(JNIEnv* public_env, jobject obj, jmethodID mid,
                                                  jvalue* args) {
  JNIEnvExt* env = reinterpret_cast<JNIEnvExt*>(public_env);
  Object* receiver = DecodeObj(env, obj);
  Method* method = FindVirtualMethod(receiver, DecodeMethod(mid));
  ArgArray arg_array(method);
  arg_array.BuildArgArray(env, args);
  return InvokeWithArgArray(env, receiver, method, arg_array.get());
}

static JValue InvokeVirtualOrInterfaceWithVarArgs(JNIEnv* public_env, jobject obj, jmethodID mid,
                                                  va_list args) {
  JNIEnvExt* env = reinterpret_cast<JNIEnvExt*>(public_env);
  Object* receiver = DecodeObj(env, obj);
  Method* method = FindVirtualMethod(receiver, DecodeMethod(mid));
  ArgArray arg_array(method);
  arg_array.BuildArgArray(env, args);
  return InvokeWithArgArray(env, receiver, method, arg_array.get());
}

// Section 12.3.2 of the JNI spec describes JNI class descriptors. They're
// separated with slashes but aren't wrapped with "L;" like regular descriptors
// (i.e. "a/b/C" rather than "La/b/C;"). Arrays of reference types are an
// exception; there the "L;" must be present ("[La/b/C;"). Historically we've
// supported names with dots too (such as "a.b.C").
static std::string NormalizeJniClassDescriptor(const char* name) {
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

static void ThrowNoSuchMethodError(ScopedJniThreadState& ts, Class* c, const char* name, const char* sig, const char* kind) {
  ts.Self()->ThrowNewExceptionF("Ljava/lang/NoSuchMethodError;",
      "no %s method \"%s.%s%s\"", kind, ClassHelper(c).GetDescriptor(), name, sig);
}

static jmethodID FindMethodID(ScopedJniThreadState& ts, jclass jni_class, const char* name, const char* sig, bool is_static) {
  Class* c = Decode<Class*>(ts, jni_class);
  if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(c, true, true)) {
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
    ThrowNoSuchMethodError(ts, c, name, sig, is_static ? "static" : "non-static");
    return NULL;
  }

  return EncodeMethod(method);
}

static const ClassLoader* GetClassLoader(Thread* self) {
  Method* method = self->GetCurrentMethod();
  if (method == NULL || PrettyMethod(method, false) == "java.lang.Runtime.nativeLoad") {
    return self->GetClassLoaderOverride();
  }
  return method->GetDeclaringClass()->GetClassLoader();
}

static jfieldID FindFieldID(ScopedJniThreadState& ts, jclass jni_class, const char* name, const char* sig, bool is_static) {
  Class* c = Decode<Class*>(ts, jni_class);
  if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(c, true, true)) {
    return NULL;
  }

  Field* field = NULL;
  Class* field_type;
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  if (sig[1] != '\0') {
    const ClassLoader* cl = GetClassLoader(ts.Self());
    field_type = class_linker->FindClass(sig, cl);
  } else {
    field_type = class_linker->FindPrimitiveClass(*sig);
  }
  if (field_type == NULL) {
    // Failed to find type from the signature of the field.
    DCHECK(ts.Self()->IsExceptionPending());
    ts.Self()->ClearException();
    ts.Self()->ThrowNewExceptionF("Ljava/lang/NoSuchFieldError;",
        "no type \"%s\" found and so no field \"%s\" could be found in class "
        "\"%s\" or its superclasses", sig, name, ClassHelper(c).GetDescriptor());
    return NULL;
  }
  if (is_static) {
    field = c->FindStaticField(name, ClassHelper(field_type).GetDescriptor());
  } else {
    field = c->FindInstanceField(name, ClassHelper(field_type).GetDescriptor());
  }
  if (field == NULL) {
    ts.Self()->ThrowNewExceptionF("Ljava/lang/NoSuchFieldError;",
        "no \"%s\" field \"%s\" in class \"%s\" or its superclasses", sig,
        name, ClassHelper(c).GetDescriptor());
    return NULL;
  }
  return EncodeField(field);
}

static void PinPrimitiveArray(ScopedJniThreadState& ts, const Array* array) {
  JavaVMExt* vm = ts.Vm();
  MutexLock mu(vm->pins_lock);
  vm->pin_table.Add(array);
}

static void UnpinPrimitiveArray(ScopedJniThreadState& ts, const Array* array) {
  JavaVMExt* vm = ts.Vm();
  MutexLock mu(vm->pins_lock);
  vm->pin_table.Remove(array);
}

template<typename JniT, typename ArtT>
static JniT NewPrimitiveArray(ScopedJniThreadState& ts, jsize length) {
  CHECK_GE(length, 0); // TODO: ReportJniError
  ArtT* result = ArtT::Alloc(length);
  return AddLocalReference<JniT>(ts.Env(), result);
}

template <typename ArrayT, typename CArrayT, typename ArtArrayT>
static CArrayT GetPrimitiveArray(ScopedJniThreadState& ts, ArrayT java_array, jboolean* is_copy) {
  ArtArrayT* array = Decode<ArtArrayT*>(ts, java_array);
  PinPrimitiveArray(ts, array);
  if (is_copy != NULL) {
    *is_copy = JNI_FALSE;
  }
  return array->GetData();
}

template <typename ArrayT>
static void ReleasePrimitiveArray(ScopedJniThreadState& ts, ArrayT java_array, jint mode) {
  if (mode != JNI_COMMIT) {
    Array* array = Decode<Array*>(ts, java_array);
    UnpinPrimitiveArray(ts, array);
  }
}

static void ThrowAIOOBE(ScopedJniThreadState& ts, Array* array, jsize start, jsize length, const char* identifier) {
  std::string type(PrettyTypeOf(array));
  ts.Self()->ThrowNewExceptionF("Ljava/lang/ArrayIndexOutOfBoundsException;",
      "%s offset=%d length=%d %s.length=%d",
      type.c_str(), start, length, identifier, array->GetLength());
}

static void ThrowSIOOBE(ScopedJniThreadState& ts, jsize start, jsize length, jsize array_length) {
  ts.Self()->ThrowNewExceptionF("Ljava/lang/StringIndexOutOfBoundsException;",
      "offset=%d length=%d string.length()=%d", start, length, array_length);
}

template <typename JavaArrayT, typename JavaT, typename ArrayT>
static void GetPrimitiveArrayRegion(ScopedJniThreadState& ts, JavaArrayT java_array, jsize start, jsize length, JavaT* buf) {
  ArrayT* array = Decode<ArrayT*>(ts, java_array);
  if (start < 0 || length < 0 || start + length > array->GetLength()) {
    ThrowAIOOBE(ts, array, start, length, "src");
  } else {
    JavaT* data = array->GetData();
    memcpy(buf, data + start, length * sizeof(JavaT));
  }
}

template <typename JavaArrayT, typename JavaT, typename ArrayT>
static void SetPrimitiveArrayRegion(ScopedJniThreadState& ts, JavaArrayT java_array, jsize start, jsize length, const JavaT* buf) {
  ArrayT* array = Decode<ArrayT*>(ts, java_array);
  if (start < 0 || length < 0 || start + length > array->GetLength()) {
    ThrowAIOOBE(ts, array, start, length, "dst");
  } else {
    JavaT* data = array->GetData();
    memcpy(data + start, buf, length * sizeof(JavaT));
  }
}

static jclass InitDirectByteBufferClass(JNIEnv* env) {
  ScopedLocalRef<jclass> buffer_class(env, env->FindClass("java/nio/ReadWriteDirectByteBuffer"));
  CHECK(buffer_class.get() != NULL);
  return reinterpret_cast<jclass>(env->NewGlobalRef(buffer_class.get()));
}

static jclass GetDirectByteBufferClass(JNIEnv* env) {
  static jclass buffer_class = InitDirectByteBufferClass(env);
  return buffer_class;
}

static jint JII_AttachCurrentThread(JavaVM* vm, JNIEnv** p_env, void* raw_args, bool as_daemon) {
  if (vm == NULL || p_env == NULL) {
    return JNI_ERR;
  }

  // Return immediately if we're already attached.
  Thread* self = Thread::Current();
  if (self != NULL) {
    *p_env = self->GetJniEnv();
    return JNI_OK;
  }

  Runtime* runtime = reinterpret_cast<JavaVMExt*>(vm)->runtime;

  // No threads allowed in zygote mode.
  if (runtime->IsZygote()) {
    LOG(ERROR) << "Attempt to attach a thread in the zygote";
    return JNI_ERR;
  }

  JavaVMAttachArgs* args = static_cast<JavaVMAttachArgs*>(raw_args);
  const char* thread_name = NULL;
  Object* thread_group = NULL;
  if (args != NULL) {
    CHECK_GE(args->version, JNI_VERSION_1_2);
    thread_name = args->name;
    thread_group = static_cast<Thread*>(NULL)->DecodeJObject(args->group);
  }

  runtime->AttachCurrentThread(thread_name, as_daemon, thread_group);
  *p_env = Thread::Current()->GetJniEnv();
  return JNI_OK;
}

class SharedLibrary {
 public:
  SharedLibrary(const std::string& path, void* handle, Object* class_loader)
      : path_(path),
        handle_(handle),
        class_loader_(class_loader),
        jni_on_load_lock_("JNI_OnLoad lock"),
        jni_on_load_cond_("JNI_OnLoad condition variable"),
        jni_on_load_thread_id_(Thread::Current()->GetThinLockId()),
        jni_on_load_result_(kPending) {
  }

  Object* GetClassLoader() {
    return class_loader_;
  }

  std::string GetPath() {
    return path_;
  }

  /*
   * Check the result of an earlier call to JNI_OnLoad on this library.
   * If the call has not yet finished in another thread, wait for it.
   */
  bool CheckOnLoadResult() {
    Thread* self = Thread::Current();
    if (jni_on_load_thread_id_ == self->GetThinLockId()) {
      // Check this so we don't end up waiting for ourselves.  We need
      // to return "true" so the caller can continue.
      LOG(INFO) << *self << " recursive attempt to load library "
                << "\"" << path_ << "\"";
      return true;
    }

    MutexLock mu(jni_on_load_lock_);
    while (jni_on_load_result_ == kPending) {
      VLOG(jni) << "[" << *self << " waiting for \"" << path_ << "\" "
                << "JNI_OnLoad...]";
      ScopedThreadStateChange tsc(self, kVmWait);
      jni_on_load_cond_.Wait(jni_on_load_lock_);
    }

    bool okay = (jni_on_load_result_ == kOkay);
    VLOG(jni) << "[Earlier JNI_OnLoad for \"" << path_ << "\" "
              << (okay ? "succeeded" : "failed") << "]";
    return okay;
  }

  void SetResult(bool result) {
    jni_on_load_result_ = result ? kOkay : kFailed;
    jni_on_load_thread_id_ = 0;

    // Broadcast a wakeup to anybody sleeping on the condition variable.
    MutexLock mu(jni_on_load_lock_);
    jni_on_load_cond_.Broadcast();
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
  Mutex jni_on_load_lock_;
  // Wait for JNI_OnLoad in other thread.
  ConditionVariable jni_on_load_cond_;
  // Recursive invocation guard.
  uint32_t jni_on_load_thread_id_;
  // Result of earlier JNI_OnLoad call.
  JNI_OnLoadState jni_on_load_result_;
};

// This exists mainly to keep implementation details out of the header file.
class Libraries {
 public:
  Libraries() {
  }

  ~Libraries() {
    STLDeleteValues(&libraries_);
  }

  SharedLibrary* Get(const std::string& path) {
    It it = libraries_.find(path);
    return (it == libraries_.end()) ? NULL : it->second;
  }

  void Put(const std::string& path, SharedLibrary* library) {
    libraries_.Put(path, library);
  }

  // See section 11.3 "Linking Native Methods" of the JNI spec.
  void* FindNativeMethod(const Method* m, std::string& detail) {
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
        VLOG(jni) << "[Found native code for " << PrettyMethod(m)
                  << " in \"" << library->GetPath() << "\"]";
        return fn;
      }
    }
    detail += "No implementation found for ";
    detail += PrettyMethod(m);
    detail += " (tried " + jni_short_name + " and " + jni_long_name + ")";
    LOG(ERROR) << detail;
    return NULL;
  }

 private:
  typedef SafeMap<std::string, SharedLibrary*>::iterator It; // TODO: C++0x auto

  SafeMap<std::string, SharedLibrary*> libraries_;
};

JValue InvokeWithJValues(JNIEnv* public_env, jobject obj, jmethodID mid, jvalue* args) {
  JNIEnvExt* env = reinterpret_cast<JNIEnvExt*>(public_env);
  Object* receiver = Decode<Object*>(env, obj);
  Method* method = DecodeMethod(mid);
  ArgArray arg_array(method);
  arg_array.BuildArgArray(env, args);
  return InvokeWithArgArray(env, receiver, method, arg_array.get());
}

JValue InvokeWithJValues(Thread* self, Object* receiver, Method* m, JValue* args) {
  return InvokeWithArgArray(self->GetJniEnv(), receiver, m, args);
}

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
    Runtime* runtime = Runtime::Current();
    ClassLinker* class_linker = runtime->GetClassLinker();
    std::string descriptor(NormalizeJniClassDescriptor(name));
    Class* c = NULL;
    if (runtime->IsStarted()) {
      const ClassLoader* cl = GetClassLoader(ts.Self());
      c = class_linker->FindClass(descriptor.c_str(), cl);
    } else {
      c = class_linker->FindSystemClass(descriptor.c_str());
    }
    return AddLocalReference<jclass>(env, c);
  }

  static jmethodID FromReflectedMethod(JNIEnv* env, jobject java_method) {
    ScopedJniThreadState ts(env);
    Method* method = Decode<Method*>(ts, java_method);
    return EncodeMethod(method);
  }

  static jfieldID FromReflectedField(JNIEnv* env, jobject java_field) {
    ScopedJniThreadState ts(env);
    Field* field = Decode<Field*>(ts, java_field);
    return EncodeField(field);
  }

  static jobject ToReflectedMethod(JNIEnv* env, jclass, jmethodID mid, jboolean) {
    ScopedJniThreadState ts(env);
    Method* method = DecodeMethod(mid);
    return AddLocalReference<jobject>(env, method);
  }

  static jobject ToReflectedField(JNIEnv* env, jclass, jfieldID fid, jboolean) {
    ScopedJniThreadState ts(env);
    Field* field = DecodeField(fid);
    return AddLocalReference<jobject>(env, field);
  }

  static jclass GetObjectClass(JNIEnv* env, jobject java_object) {
    ScopedJniThreadState ts(env);
    Object* o = Decode<Object*>(ts, java_object);
    return AddLocalReference<jclass>(env, o->GetClass());
  }

  static jclass GetSuperclass(JNIEnv* env, jclass java_class) {
    ScopedJniThreadState ts(env);
    Class* c = Decode<Class*>(ts, java_class);
    return AddLocalReference<jclass>(env, c->GetSuperClass());
  }

  static jboolean IsAssignableFrom(JNIEnv* env, jclass java_class1, jclass java_class2) {
    ScopedJniThreadState ts(env);
    Class* c1 = Decode<Class*>(ts, java_class1);
    Class* c2 = Decode<Class*>(ts, java_class2);
    return c1->IsAssignableFrom(c2) ? JNI_TRUE : JNI_FALSE;
  }

  static jboolean IsInstanceOf(JNIEnv* env, jobject jobj, jclass java_class) {
    ScopedJniThreadState ts(env);
    CHECK_NE(static_cast<jclass>(NULL), java_class); // TODO: ReportJniError
    if (jobj == NULL) {
      // Note: JNI is different from regular Java instanceof in this respect
      return JNI_TRUE;
    } else {
      Object* obj = Decode<Object*>(ts, jobj);
      Class* c = Decode<Class*>(ts, java_class);
      return obj->InstanceOf(c) ? JNI_TRUE : JNI_FALSE;
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
    jmethodID mid = ((msg != NULL)
                     ? env->GetMethodID(c, "<init>", "(Ljava/lang/String;)V")
                     : env->GetMethodID(c, "<init>", "()V"));
    if (mid == NULL) {
      return JNI_ERR;
    }
    ScopedLocalRef<jstring> s(env, env->NewStringUTF(msg));
    if (msg != NULL && s.get() == NULL) {
      return JNI_ERR;
    }

    jvalue args[1];
    args[0].l = s.get();
    ScopedLocalRef<jthrowable> exception(env, reinterpret_cast<jthrowable>(env->NewObjectA(c, mid, args)));
    if (exception.get() == NULL) {
      return JNI_ERR;
    }

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

    ScopedLocalRef<jthrowable> exception(env, AddLocalReference<jthrowable>(env, original_exception));
    ScopedLocalRef<jclass> exception_class(env, env->GetObjectClass(exception.get()));
    jmethodID mid = env->GetMethodID(exception_class.get(), "printStackTrace", "()V");
    if (mid == NULL) {
      LOG(WARNING) << "JNI WARNING: no printStackTrace()V in "
                   << PrettyTypeOf(original_exception);
    } else {
      env->CallVoidMethod(exception.get(), mid);
      if (self->IsExceptionPending()) {
        LOG(WARNING) << "JNI WARNING: " << PrettyTypeOf(self->GetException())
                     << " thrown while calling printStackTrace";
        self->ClearException();
      }
    }

    self->SetException(original_exception);
  }

  static jthrowable ExceptionOccurred(JNIEnv* env) {
    ScopedJniThreadState ts(env);
    Object* exception = ts.Self()->GetException();
    return (exception != NULL) ? AddLocalReference<jthrowable>(env, exception) : NULL;
  }

  static void FatalError(JNIEnv* env, const char* msg) {
    ScopedJniThreadState ts(env);
    LOG(FATAL) << "JNI FatalError called: " << msg;
  }

  static jint PushLocalFrame(JNIEnv* env, jint capacity) {
    ScopedJniThreadState ts(env);
    if (EnsureLocalCapacity(ts, capacity, "PushLocalFrame") != JNI_OK) {
      return JNI_ERR;
    }
    ts.Env()->PushFrame(capacity);
    return JNI_OK;
  }

  static jobject PopLocalFrame(JNIEnv* env, jobject java_survivor) {
    ScopedJniThreadState ts(env);
    Object* survivor = Decode<Object*>(ts, java_survivor);
    ts.Env()->PopFrame();
    return AddLocalReference<jobject>(env, survivor);
  }

  static jint EnsureLocalCapacity(JNIEnv* env, jint desired_capacity) {
    ScopedJniThreadState ts(env);
    return EnsureLocalCapacity(ts, desired_capacity, "EnsureLocalCapacity");
  }

  static jint EnsureLocalCapacity(ScopedJniThreadState& ts, jint desired_capacity, const char* caller) {
    // TODO: we should try to expand the table if necessary.
    if (desired_capacity < 1 || desired_capacity > static_cast<jint>(kLocalsMax)) {
      LOG(ERROR) << "Invalid capacity given to " << caller << ": " << desired_capacity;
      return JNI_ERR;
    }
    // TODO: this isn't quite right, since "capacity" includes holes.
    size_t capacity = ts.Env()->locals.Capacity();
    bool okay = (static_cast<jint>(kLocalsMax - capacity) >= desired_capacity);
    if (!okay) {
      ts.Self()->ThrowOutOfMemoryError(caller);
    }
    return okay ? JNI_OK : JNI_ERR;
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

    uint32_t cookie = ts.Env()->local_ref_cookie;
    IndirectRef ref = locals.Add(cookie, Decode<Object*>(ts, obj));
    return reinterpret_cast<jobject>(ref);
  }

  static void DeleteLocalRef(JNIEnv* env, jobject obj) {
    ScopedJniThreadState ts(env);
    if (obj == NULL) {
      return;
    }

    IndirectReferenceTable& locals = ts.Env()->locals;

    uint32_t cookie = ts.Env()->local_ref_cookie;
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
    if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(c, true, true)) {
      return NULL;
    }
    return AddLocalReference<jobject>(env, c->AllocObject());
  }

  static jobject NewObject(JNIEnv* env, jclass c, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list args;
    va_start(args, mid);
    jobject result = NewObjectV(env, c, mid, args);
    va_end(args);
    return result;
  }

  static jobject NewObjectV(JNIEnv* env, jclass java_class, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    Class* c = Decode<Class*>(ts, java_class);
    if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(c, true, true)) {
      return NULL;
    }
    Object* result = c->AllocObject();
    if (result == NULL) {
      return NULL;
    }
    jobject local_result = AddLocalReference<jobject>(env, result);
    CallNonvirtualVoidMethodV(env, local_result, java_class, mid, args);
    if (!ts.Self()->IsExceptionPending()) {
      return local_result;
    } else {
      return NULL;
    }
  }

  static jobject NewObjectA(JNIEnv* env, jclass java_class, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    Class* c = Decode<Class*>(ts, java_class);
    if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(c, true, true)) {
      return NULL;
    }
    Object* result = c->AllocObject();
    if (result == NULL) {
      return NULL;
    }
    jobject local_result = AddLocalReference<jobjectArray>(env, result);
    CallNonvirtualVoidMethodA(env, local_result, java_class, mid, args);
    if (!ts.Self()->IsExceptionPending()) {
      return local_result;
    } else {
      return NULL;
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

  static jobject CallObjectMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result(InvokeVirtualOrInterfaceWithVarArgs(env, obj, mid, ap));
    va_end(ap);
    return AddLocalReference<jobject>(env, result.GetL());
  }

  static jobject CallObjectMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    JValue result(InvokeVirtualOrInterfaceWithVarArgs(env, obj, mid, args));
    return AddLocalReference<jobject>(env, result.GetL());
  }

  static jobject CallObjectMethodA(JNIEnv* env, jobject obj, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    JValue result(InvokeVirtualOrInterfaceWithJValues(env, obj, mid, args));
    return AddLocalReference<jobject>(env, result.GetL());
  }

  static jboolean CallBooleanMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result(InvokeVirtualOrInterfaceWithVarArgs(env, obj, mid, ap));
    va_end(ap);
    return result.GetZ();
  }

  static jboolean CallBooleanMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeVirtualOrInterfaceWithVarArgs(env, obj, mid, args).GetZ();
  }

  static jboolean CallBooleanMethodA(JNIEnv* env, jobject obj, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeVirtualOrInterfaceWithJValues(env, obj, mid, args).GetZ();
  }

  static jbyte CallByteMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result(InvokeVirtualOrInterfaceWithVarArgs(env, obj, mid, ap));
    va_end(ap);
    return result.GetB();
  }

  static jbyte CallByteMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeVirtualOrInterfaceWithVarArgs(env, obj, mid, args).GetB();
  }

  static jbyte CallByteMethodA(JNIEnv* env, jobject obj, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeVirtualOrInterfaceWithJValues(env, obj, mid, args).GetB();
  }

  static jchar CallCharMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result(InvokeVirtualOrInterfaceWithVarArgs(env, obj, mid, ap));
    va_end(ap);
    return result.GetC();
  }

  static jchar CallCharMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeVirtualOrInterfaceWithVarArgs(env, obj, mid, args).GetC();
  }

  static jchar CallCharMethodA(JNIEnv* env, jobject obj, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeVirtualOrInterfaceWithJValues(env, obj, mid, args).GetC();
  }

  static jdouble CallDoubleMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result(InvokeVirtualOrInterfaceWithVarArgs(env, obj, mid, ap));
    va_end(ap);
    return result.GetD();
  }

  static jdouble CallDoubleMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeVirtualOrInterfaceWithVarArgs(env, obj, mid, args).GetD();
  }

  static jdouble CallDoubleMethodA(JNIEnv* env, jobject obj, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeVirtualOrInterfaceWithJValues(env, obj, mid, args).GetD();
  }

  static jfloat CallFloatMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result(InvokeVirtualOrInterfaceWithVarArgs(env, obj, mid, ap));
    va_end(ap);
    return result.GetF();
  }

  static jfloat CallFloatMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeVirtualOrInterfaceWithVarArgs(env, obj, mid, args).GetF();
  }

  static jfloat CallFloatMethodA(JNIEnv* env, jobject obj, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeVirtualOrInterfaceWithJValues(env, obj, mid, args).GetF();
  }

  static jint CallIntMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result(InvokeVirtualOrInterfaceWithVarArgs(env, obj, mid, ap));
    va_end(ap);
    return result.GetI();
  }

  static jint CallIntMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeVirtualOrInterfaceWithVarArgs(env, obj, mid, args).GetI();
  }

  static jint CallIntMethodA(JNIEnv* env, jobject obj, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeVirtualOrInterfaceWithJValues(env, obj, mid, args).GetI();
  }

  static jlong CallLongMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result(InvokeVirtualOrInterfaceWithVarArgs(env, obj, mid, ap));
    va_end(ap);
    return result.GetJ();
  }

  static jlong CallLongMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeVirtualOrInterfaceWithVarArgs(env, obj, mid, args).GetJ();
  }

  static jlong CallLongMethodA(JNIEnv* env, jobject obj, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeVirtualOrInterfaceWithJValues(env, obj, mid, args).GetJ();
  }

  static jshort CallShortMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result(InvokeVirtualOrInterfaceWithVarArgs(env, obj, mid, ap));
    va_end(ap);
    return result.GetS();
  }

  static jshort CallShortMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeVirtualOrInterfaceWithVarArgs(env, obj, mid, args).GetS();
  }

  static jshort CallShortMethodA(JNIEnv* env, jobject obj, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeVirtualOrInterfaceWithJValues(env, obj, mid, args).GetS();
  }

  static void CallVoidMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result(InvokeVirtualOrInterfaceWithVarArgs(env, obj, mid, ap));
    va_end(ap);
  }

  static void CallVoidMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    InvokeVirtualOrInterfaceWithVarArgs(env, obj, mid, args);
  }

  static void CallVoidMethodA(JNIEnv* env, jobject obj, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    InvokeVirtualOrInterfaceWithJValues(env, obj, mid, args);
  }

  static jobject CallNonvirtualObjectMethod(JNIEnv* env, jobject obj, jclass, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result(InvokeWithVarArgs(env, obj, mid, ap));
    jobject local_result = AddLocalReference<jobject>(env, result.GetL());
    va_end(ap);
    return local_result;
  }

  static jobject CallNonvirtualObjectMethodV(JNIEnv* env,
      jobject obj, jclass, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    JValue result(InvokeWithVarArgs(env, obj, mid, args));
    return AddLocalReference<jobject>(env, result.GetL());
  }

  static jobject CallNonvirtualObjectMethodA(JNIEnv* env,
      jobject obj, jclass, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    JValue result(InvokeWithJValues(env, obj, mid, args));
    return AddLocalReference<jobject>(env, result.GetL());
  }

  static jboolean CallNonvirtualBooleanMethod(JNIEnv* env,
      jobject obj, jclass, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result(InvokeWithVarArgs(env, obj, mid, ap));
    va_end(ap);
    return result.GetZ();
  }

  static jboolean CallNonvirtualBooleanMethodV(JNIEnv* env,
      jobject obj, jclass, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(env, obj, mid, args).GetZ();
  }

  static jboolean CallNonvirtualBooleanMethodA(JNIEnv* env,
      jobject obj, jclass, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(env, obj, mid, args).GetZ();
  }

  static jbyte CallNonvirtualByteMethod(JNIEnv* env, jobject obj, jclass, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result(InvokeWithVarArgs(env, obj, mid, ap));
    va_end(ap);
    return result.GetB();
  }

  static jbyte CallNonvirtualByteMethodV(JNIEnv* env,
      jobject obj, jclass, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(env, obj, mid, args).GetB();
  }

  static jbyte CallNonvirtualByteMethodA(JNIEnv* env,
      jobject obj, jclass, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(env, obj, mid, args).GetB();
  }

  static jchar CallNonvirtualCharMethod(JNIEnv* env, jobject obj, jclass, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result(InvokeWithVarArgs(env, obj, mid, ap));
    va_end(ap);
    return result.GetC();
  }

  static jchar CallNonvirtualCharMethodV(JNIEnv* env,
      jobject obj, jclass, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(env, obj, mid, args).GetC();
  }

  static jchar CallNonvirtualCharMethodA(JNIEnv* env,
      jobject obj, jclass, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(env, obj, mid, args).GetC();
  }

  static jshort CallNonvirtualShortMethod(JNIEnv* env, jobject obj, jclass, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result(InvokeWithVarArgs(env, obj, mid, ap));
    va_end(ap);
    return result.GetS();
  }

  static jshort CallNonvirtualShortMethodV(JNIEnv* env,
      jobject obj, jclass, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(env, obj, mid, args).GetS();
  }

  static jshort CallNonvirtualShortMethodA(JNIEnv* env,
      jobject obj, jclass, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(env, obj, mid, args).GetS();
  }

  static jint CallNonvirtualIntMethod(JNIEnv* env, jobject obj, jclass, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result(InvokeWithVarArgs(env, obj, mid, ap));
    va_end(ap);
    return result.GetI();
  }

  static jint CallNonvirtualIntMethodV(JNIEnv* env,
      jobject obj, jclass, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(env, obj, mid, args).GetI();
  }

  static jint CallNonvirtualIntMethodA(JNIEnv* env,
      jobject obj, jclass, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(env, obj, mid, args).GetI();
  }

  static jlong CallNonvirtualLongMethod(JNIEnv* env, jobject obj, jclass, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result(InvokeWithVarArgs(env, obj, mid, ap));
    va_end(ap);
    return result.GetJ();
  }

  static jlong CallNonvirtualLongMethodV(JNIEnv* env,
      jobject obj, jclass, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(env, obj, mid, args).GetJ();
  }

  static jlong CallNonvirtualLongMethodA(JNIEnv* env,
      jobject obj, jclass, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(env, obj, mid, args).GetJ();
  }

  static jfloat CallNonvirtualFloatMethod(JNIEnv* env, jobject obj, jclass, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result(InvokeWithVarArgs(env, obj, mid, ap));
    va_end(ap);
    return result.GetF();
  }

  static jfloat CallNonvirtualFloatMethodV(JNIEnv* env,
      jobject obj, jclass, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(env, obj, mid, args).GetF();
  }

  static jfloat CallNonvirtualFloatMethodA(JNIEnv* env,
      jobject obj, jclass, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(env, obj, mid, args).GetF();
  }

  static jdouble CallNonvirtualDoubleMethod(JNIEnv* env, jobject obj, jclass, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result(InvokeWithVarArgs(env, obj, mid, ap));
    va_end(ap);
    return result.GetD();
  }

  static jdouble CallNonvirtualDoubleMethodV(JNIEnv* env,
      jobject obj, jclass, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(env, obj, mid, args).GetD();
  }

  static jdouble CallNonvirtualDoubleMethodA(JNIEnv* env,
      jobject obj, jclass, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(env, obj, mid, args).GetD();
  }

  static void CallNonvirtualVoidMethod(JNIEnv* env, jobject obj, jclass, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    InvokeWithVarArgs(env, obj, mid, ap);
    va_end(ap);
  }

  static void CallNonvirtualVoidMethodV(JNIEnv* env,
      jobject obj, jclass, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    InvokeWithVarArgs(env, obj, mid, args);
  }

  static void CallNonvirtualVoidMethodA(JNIEnv* env,
      jobject obj, jclass, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    InvokeWithJValues(env, obj, mid, args);
  }

  static jfieldID GetFieldID(JNIEnv* env, jclass c, const char* name, const char* sig) {
    ScopedJniThreadState ts(env);
    return FindFieldID(ts, c, name, sig, false);
  }


  static jfieldID GetStaticFieldID(JNIEnv* env, jclass c, const char* name, const char* sig) {
    ScopedJniThreadState ts(env);
    return FindFieldID(ts, c, name, sig, true);
  }

  static jobject GetObjectField(JNIEnv* env, jobject obj, jfieldID fid) {
    ScopedJniThreadState ts(env);
    Object* o = Decode<Object*>(ts, obj);
    Field* f = DecodeField(fid);
    return AddLocalReference<jobject>(env, f->GetObject(o));
  }

  static jobject GetStaticObjectField(JNIEnv* env, jclass, jfieldID fid) {
    ScopedJniThreadState ts(env);
    Field* f = DecodeField(fid);
    return AddLocalReference<jobject>(env, f->GetObject(NULL));
  }

  static void SetObjectField(JNIEnv* env, jobject java_object, jfieldID fid, jobject java_value) {
    ScopedJniThreadState ts(env);
    Object* o = Decode<Object*>(ts, java_object);
    Object* v = Decode<Object*>(ts, java_value);
    Field* f = DecodeField(fid);
    f->SetObject(o, v);
  }

  static void SetStaticObjectField(JNIEnv* env, jclass, jfieldID fid, jobject java_value) {
    ScopedJniThreadState ts(env);
    Object* v = Decode<Object*>(ts, java_value);
    Field* f = DecodeField(fid);
    f->SetObject(NULL, v);
  }

#define GET_PRIMITIVE_FIELD(fn, instance) \
  ScopedJniThreadState ts(env); \
  Object* o = Decode<Object*>(ts, instance); \
  Field* f = DecodeField(fid); \
  return f->fn(o)

#define SET_PRIMITIVE_FIELD(fn, instance, value) \
  ScopedJniThreadState ts(env); \
  Object* o = Decode<Object*>(ts, instance); \
  Field* f = DecodeField(fid); \
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

  static jboolean GetStaticBooleanField(JNIEnv* env, jclass, jfieldID fid) {
    GET_PRIMITIVE_FIELD(GetBoolean, NULL);
  }

  static jbyte GetStaticByteField(JNIEnv* env, jclass, jfieldID fid) {
    GET_PRIMITIVE_FIELD(GetByte, NULL);
  }

  static jchar GetStaticCharField(JNIEnv* env, jclass, jfieldID fid) {
    GET_PRIMITIVE_FIELD(GetChar, NULL);
  }

  static jshort GetStaticShortField(JNIEnv* env, jclass, jfieldID fid) {
    GET_PRIMITIVE_FIELD(GetShort, NULL);
  }

  static jint GetStaticIntField(JNIEnv* env, jclass, jfieldID fid) {
    GET_PRIMITIVE_FIELD(GetInt, NULL);
  }

  static jlong GetStaticLongField(JNIEnv* env, jclass, jfieldID fid) {
    GET_PRIMITIVE_FIELD(GetLong, NULL);
  }

  static jfloat GetStaticFloatField(JNIEnv* env, jclass, jfieldID fid) {
    GET_PRIMITIVE_FIELD(GetFloat, NULL);
  }

  static jdouble GetStaticDoubleField(JNIEnv* env, jclass, jfieldID fid) {
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

  static jobject CallStaticObjectMethod(JNIEnv* env, jclass, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result(InvokeWithVarArgs(env, NULL, mid, ap));
    jobject local_result = AddLocalReference<jobject>(env, result.GetL());
    va_end(ap);
    return local_result;
  }

  static jobject CallStaticObjectMethodV(JNIEnv* env, jclass, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    JValue result(InvokeWithVarArgs(env, NULL, mid, args));
    return AddLocalReference<jobject>(env, result.GetL());
  }

  static jobject CallStaticObjectMethodA(JNIEnv* env, jclass, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    JValue result(InvokeWithJValues(env, NULL, mid, args));
    return AddLocalReference<jobject>(env, result.GetL());
  }

  static jboolean CallStaticBooleanMethod(JNIEnv* env, jclass, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result(InvokeWithVarArgs(env, NULL, mid, ap));
    va_end(ap);
    return result.GetZ();
  }

  static jboolean CallStaticBooleanMethodV(JNIEnv* env, jclass, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(env, NULL, mid, args).GetZ();
  }

  static jboolean CallStaticBooleanMethodA(JNIEnv* env, jclass, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(env, NULL, mid, args).GetZ();
  }

  static jbyte CallStaticByteMethod(JNIEnv* env, jclass, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result(InvokeWithVarArgs(env, NULL, mid, ap));
    va_end(ap);
    return result.GetB();
  }

  static jbyte CallStaticByteMethodV(JNIEnv* env, jclass, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(env, NULL, mid, args).GetB();
  }

  static jbyte CallStaticByteMethodA(JNIEnv* env, jclass, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(env, NULL, mid, args).GetB();
  }

  static jchar CallStaticCharMethod(JNIEnv* env, jclass, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result(InvokeWithVarArgs(env, NULL, mid, ap));
    va_end(ap);
    return result.GetC();
  }

  static jchar CallStaticCharMethodV(JNIEnv* env, jclass, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(env, NULL, mid, args).GetC();
  }

  static jchar CallStaticCharMethodA(JNIEnv* env, jclass, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(env, NULL, mid, args).GetC();
  }

  static jshort CallStaticShortMethod(JNIEnv* env, jclass, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result(InvokeWithVarArgs(env, NULL, mid, ap));
    va_end(ap);
    return result.GetS();
  }

  static jshort CallStaticShortMethodV(JNIEnv* env, jclass, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(env, NULL, mid, args).GetS();
  }

  static jshort CallStaticShortMethodA(JNIEnv* env, jclass, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(env, NULL, mid, args).GetS();
  }

  static jint CallStaticIntMethod(JNIEnv* env, jclass, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result(InvokeWithVarArgs(env, NULL, mid, ap));
    va_end(ap);
    return result.GetI();
  }

  static jint CallStaticIntMethodV(JNIEnv* env, jclass, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(env, NULL, mid, args).GetI();
  }

  static jint CallStaticIntMethodA(JNIEnv* env, jclass, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(env, NULL, mid, args).GetI();
  }

  static jlong CallStaticLongMethod(JNIEnv* env, jclass, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result(InvokeWithVarArgs(env, NULL, mid, ap));
    va_end(ap);
    return result.GetJ();
  }

  static jlong CallStaticLongMethodV(JNIEnv* env, jclass, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(env, NULL, mid, args).GetJ();
  }

  static jlong CallStaticLongMethodA(JNIEnv* env, jclass, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(env, NULL, mid, args).GetJ();
  }

  static jfloat CallStaticFloatMethod(JNIEnv* env, jclass, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result(InvokeWithVarArgs(env, NULL, mid, ap));
    va_end(ap);
    return result.GetF();
  }

  static jfloat CallStaticFloatMethodV(JNIEnv* env, jclass, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(env, NULL, mid, args).GetF();
  }

  static jfloat CallStaticFloatMethodA(JNIEnv* env, jclass, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(env, NULL, mid, args).GetF();
  }

  static jdouble CallStaticDoubleMethod(JNIEnv* env, jclass, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    JValue result(InvokeWithVarArgs(env, NULL, mid, ap));
    va_end(ap);
    return result.GetD();
  }

  static jdouble CallStaticDoubleMethodV(JNIEnv* env, jclass, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    return InvokeWithVarArgs(env, NULL, mid, args).GetD();
  }

  static jdouble CallStaticDoubleMethodA(JNIEnv* env, jclass, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    return InvokeWithJValues(env, NULL, mid, args).GetD();
  }

  static void CallStaticVoidMethod(JNIEnv* env, jclass, jmethodID mid, ...) {
    ScopedJniThreadState ts(env);
    va_list ap;
    va_start(ap, mid);
    InvokeWithVarArgs(env, NULL, mid, ap);
    va_end(ap);
  }

  static void CallStaticVoidMethodV(JNIEnv* env, jclass, jmethodID mid, va_list args) {
    ScopedJniThreadState ts(env);
    InvokeWithVarArgs(env, NULL, mid, args);
  }

  static void CallStaticVoidMethodA(JNIEnv* env, jclass, jmethodID mid, jvalue* args) {
    ScopedJniThreadState ts(env);
    InvokeWithJValues(env, NULL, mid, args);
  }

  static jstring NewString(JNIEnv* env, const jchar* chars, jsize char_count) {
    ScopedJniThreadState ts(env);
    String* result = String::AllocFromUtf16(char_count, chars);
    return AddLocalReference<jstring>(env, result);
  }

  static jstring NewStringUTF(JNIEnv* env, const char* utf) {
    ScopedJniThreadState ts(env);
    if (utf == NULL) {
      return NULL;
    }
    String* result = String::AllocFromModifiedUtf8(utf);
    return AddLocalReference<jstring>(env, result);
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

  static void ReleaseStringChars(JNIEnv* env, jstring java_string, const jchar*) {
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
    CHECK(bytes != NULL); // bionic aborts anyway.
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
    return AddLocalReference<jobject>(env, array->Get(index));
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
    descriptor += ClassHelper(element_class).GetDescriptor();

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
    return AddLocalReference<jobjectArray>(env, result);
  }

  static jshortArray NewShortArray(JNIEnv* env, jsize length) {
    ScopedJniThreadState ts(env);
    return NewPrimitiveArray<jshortArray, ShortArray>(ts, length);
  }

  static void* GetPrimitiveArrayCritical(JNIEnv* env, jarray java_array, jboolean* is_copy) {
    ScopedJniThreadState ts(env);
    Array* array = Decode<Array*>(ts, java_array);
    PinPrimitiveArray(ts, array);
    if (is_copy != NULL) {
      *is_copy = JNI_FALSE;
    }
    return array->GetRawData(array->GetClass()->GetComponentSize());
  }

  static void ReleasePrimitiveArrayCritical(JNIEnv* env, jarray array, void*, jint mode) {
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

  static void ReleaseBooleanArrayElements(JNIEnv* env, jbooleanArray array, jboolean*, jint mode) {
    ScopedJniThreadState ts(env);
    ReleasePrimitiveArray(ts, array, mode);
  }

  static void ReleaseByteArrayElements(JNIEnv* env, jbyteArray array, jbyte*, jint mode) {
    ScopedJniThreadState ts(env);
    ReleasePrimitiveArray(ts, array, mode);
  }

  static void ReleaseCharArrayElements(JNIEnv* env, jcharArray array, jchar*, jint mode) {
    ScopedJniThreadState ts(env);
    ReleasePrimitiveArray(ts, array, mode);
  }

  static void ReleaseDoubleArrayElements(JNIEnv* env, jdoubleArray array, jdouble*, jint mode) {
    ScopedJniThreadState ts(env);
    ReleasePrimitiveArray(ts, array, mode);
  }

  static void ReleaseFloatArrayElements(JNIEnv* env, jfloatArray array, jfloat*, jint mode) {
    ScopedJniThreadState ts(env);
    ReleasePrimitiveArray(ts, array, mode);
  }

  static void ReleaseIntArrayElements(JNIEnv* env, jintArray array, jint*, jint mode) {
    ScopedJniThreadState ts(env);
    ReleasePrimitiveArray(ts, array, mode);
  }

  static void ReleaseLongArrayElements(JNIEnv* env, jlongArray array, jlong*, jint mode) {
    ScopedJniThreadState ts(env);
    ReleasePrimitiveArray(ts, array, mode);
  }

  static void ReleaseShortArrayElements(JNIEnv* env, jshortArray array, jshort*, jint mode) {
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
        LOG(INFO) << "Failed to register native method " << name << sig;
        ThrowNoSuchMethodError(ts, c, name, sig, "static or non-static");
        return JNI_ERR;
      } else if (!m->IsNative()) {
        LOG(INFO) << "Failed to register non-native method " << name << sig << " as native";
        ThrowNoSuchMethodError(ts, c, name, sig, "native");
        return JNI_ERR;
      }

      VLOG(jni) << "[Registering JNI native method " << PrettyMethod(m) << "]";

      m->RegisterNative(ts.Self(), methods[i].fnPtr);
    }
    return JNI_OK;
  }

  static jint UnregisterNatives(JNIEnv* env, jclass java_class) {
    ScopedJniThreadState ts(env);
    Class* c = Decode<Class*>(ts, java_class);

    VLOG(jni) << "[Unregistering JNI native methods for " << PrettyClass(c) << "]";

    for (size_t i = 0; i < c->NumDirectMethods(); ++i) {
      Method* m = c->GetDirectMethod(i);
      if (m->IsNative()) {
        m->UnregisterNative(ts.Self());
      }
    }
    for (size_t i = 0; i < c->NumVirtualMethods(); ++i) {
      Method* m = c->GetVirtualMethod(i);
      if (m->IsNative()) {
        m->UnregisterNative(ts.Self());
      }
    }

    return JNI_OK;
  }

  static jint MonitorEnter(JNIEnv* env, jobject java_object) {
    ScopedJniThreadState ts(env);
    Object* o = Decode<Object*>(ts, java_object);
    o->MonitorEnter(ts.Self());
    if (ts.Self()->IsExceptionPending()) {
      return JNI_ERR;
    }
    ts.Env()->monitors.Add(o);
    return JNI_OK;
  }

  static jint MonitorExit(JNIEnv* env, jobject java_object) {
    ScopedJniThreadState ts(env);
    Object* o = Decode<Object*>(ts, java_object);
    o->MonitorExit(ts.Self());
    if (ts.Self()->IsExceptionPending()) {
      return JNI_ERR;
    }
    ts.Env()->monitors.Remove(o);
    return JNI_OK;
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
      if (ts.Env()->locals.Get(ref) != kInvalidIndirectRefObject) {
        return JNILocalRefType;
      }
      return JNIInvalidRefType;
    case kGlobal:
      return JNIGlobalRefType;
    case kWeakGlobal:
      return JNIWeakGlobalRefType;
    case kSirtOrInvalid:
      // Is it in a stack IRT?
      if (ts.Self()->StackReferencesContain(java_object)) {
        return JNILocalRefType;
      }

      if (!ts.Vm()->work_around_app_jni_bugs) {
        return JNIInvalidRefType;
      }

      // If we're handing out direct pointers, check whether it's a direct pointer
      // to a local reference.
      if (Decode<Object*>(ts, java_object) == reinterpret_cast<Object*>(java_object)) {
        if (ts.Env()->locals.ContainsDirectPointer(reinterpret_cast<Object*>(java_object))) {
          return JNILocalRefType;
        }
      }

      return JNIInvalidRefType;
    }
    LOG(FATAL) << "IndirectRefKind[" << kind << "]";
    return JNIInvalidRefType;
  }
};

const JNINativeInterface gJniNativeInterface = {
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

JNIEnvExt::JNIEnvExt(Thread* self, JavaVMExt* vm)
    : self(self),
      vm(vm),
      local_ref_cookie(IRT_FIRST_SEGMENT),
      locals(kLocalsInitial, kLocalsMax, kLocal),
      check_jni(false),
      critical(false),
      monitors("monitors", kMonitorsInitial, kMonitorsMax) {
  functions = unchecked_functions = &gJniNativeInterface;
  if (vm->check_jni) {
    SetCheckJniEnabled(true);
  }
  // The JniEnv local reference values must be at a consistent offset or else cross-compilation
  // errors will ensue.
  CHECK_EQ(JNIEnvExt::LocalRefCookieOffset().Int32Value(), 12);
  CHECK_EQ(JNIEnvExt::SegmentStateOffset().Int32Value(), 16);
}

JNIEnvExt::~JNIEnvExt() {
}

void JNIEnvExt::SetCheckJniEnabled(bool enabled) {
  check_jni = enabled;
  functions = enabled ? GetCheckJniNativeInterface() : &gJniNativeInterface;
}

void JNIEnvExt::DumpReferenceTables() {
  locals.Dump();
  monitors.Dump();
}

void JNIEnvExt::PushFrame(int /*capacity*/) {
  // TODO: take 'capacity' into account.
  stacked_local_ref_cookies.push_back(local_ref_cookie);
  local_ref_cookie = locals.GetSegmentState();
}

void JNIEnvExt::PopFrame() {
  locals.SetSegmentState(local_ref_cookie);
  local_ref_cookie = stacked_local_ref_cookies.back();
  stacked_local_ref_cookies.pop_back();
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
    options.push_back(std::make_pair(std::string(option->optionString), option->extraInfo));
  }
  bool ignore_unrecognized = args->ignoreUnrecognized;
  Runtime* runtime = Runtime::Create(options, ignore_unrecognized);
  if (runtime == NULL) {
    return JNI_ERR;
  }
  runtime->Start();
  *p_env = Thread::Current()->GetJniEnv();
  *p_vm = runtime->GetJavaVM();
  return JNI_OK;
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
extern "C" jint JNI_GetDefaultJavaVMInitArgs(void* /*vm_args*/) {
  return JNI_ERR;
}

class JII {
 public:
  static jint DestroyJavaVM(JavaVM* vm) {
    if (vm == NULL) {
      return JNI_ERR;
    }
    JavaVMExt* raw_vm = reinterpret_cast<JavaVMExt*>(vm);
    delete raw_vm->runtime;
    return JNI_OK;
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
    }
    JavaVMExt* raw_vm = reinterpret_cast<JavaVMExt*>(vm);
    Runtime* runtime = raw_vm->runtime;
    runtime->DetachCurrentThread();
    return JNI_OK;
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

const JNIInvokeInterface gJniInvokeInterface = {
  NULL,  // reserved0
  NULL,  // reserved1
  NULL,  // reserved2
  JII::DestroyJavaVM,
  JII::AttachCurrentThread,
  JII::DetachCurrentThread,
  JII::GetEnv,
  JII::AttachCurrentThreadAsDaemon
};

JavaVMExt::JavaVMExt(Runtime* runtime, Runtime::ParsedOptions* options)
    : runtime(runtime),
      check_jni_abort_hook(NULL),
      check_jni_abort_hook_data(NULL),
      check_jni(false),
      force_copy(false), // TODO: add a way to enable this
      trace(options->jni_trace_),
      work_around_app_jni_bugs(false),
      pins_lock("JNI pin table lock"),
      pin_table("pin table", kPinTableInitial, kPinTableMax),
      globals_lock("JNI global reference table lock"),
      globals(gGlobalsInitial, gGlobalsMax, kGlobal),
      weak_globals_lock("JNI weak global reference table lock"),
      weak_globals(kWeakGlobalsInitial, kWeakGlobalsMax, kWeakGlobal),
      libraries_lock("JNI shared libraries map lock"),
      libraries(new Libraries) {
  functions = unchecked_functions = &gJniInvokeInterface;
  if (options->check_jni_) {
    SetCheckJniEnabled(true);
  }
}

JavaVMExt::~JavaVMExt() {
  delete libraries;
}

void JavaVMExt::SetCheckJniEnabled(bool enabled) {
  check_jni = enabled;
  functions = enabled ? GetCheckJniInvokeInterface() : &gJniInvokeInterface;
}

void JavaVMExt::DumpReferenceTables() {
  {
    MutexLock mu(globals_lock);
    globals.Dump();
  }
  {
    MutexLock mu(weak_globals_lock);
    weak_globals.Dump();
  }
  {
    MutexLock mu(pins_lock);
    pin_table.Dump();
  }
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
    VLOG(jni) << "[Shared library \"" << path << "\" already loaded in "
              << "ClassLoader " << class_loader << "]";
    if (!library->CheckOnLoadResult()) {
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
  // want to switch from kRunnable to kVmWait while it executes.  This allows
  // the GC to ignore us.
  Thread* self = Thread::Current();
  void* handle = NULL;
  {
    ScopedThreadStateChange tsc(self, kVmWait);
    handle = dlopen(path.empty() ? NULL : path.c_str(), RTLD_LAZY);
  }

  VLOG(jni) << "[Call to dlopen(\"" << path << "\") returned " << handle << "]";

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
      return library->CheckOnLoadResult();
    }
    library = new SharedLibrary(path, handle, class_loader);
    libraries->Put(path, library);
  }

  VLOG(jni) << "[Added shared library \"" << path << "\" for ClassLoader " << class_loader << "]";

  bool result = true;
  void* sym = dlsym(handle, "JNI_OnLoad");
  if (sym == NULL) {
    VLOG(jni) << "[No JNI_OnLoad found in \"" << path << "\"]";
  } else {
    // Call JNI_OnLoad.  We have to override the current class
    // loader, which will always be "null" since the stuff at the
    // top of the stack is around Runtime.loadLibrary().  (See
    // the comments in the JNI FindClass function.)
    typedef int (*JNI_OnLoadFn)(JavaVM*, void*);
    JNI_OnLoadFn jni_on_load = reinterpret_cast<JNI_OnLoadFn>(sym);
    const ClassLoader* old_class_loader = self->GetClassLoaderOverride();
    self->SetClassLoaderOverride(class_loader);

    int version = 0;
    {
      ScopedThreadStateChange tsc(self, kNative);
      VLOG(jni) << "[Calling JNI_OnLoad in \"" << path << "\"]";
      version = (*jni_on_load)(this, NULL);
    }

    self->SetClassLoaderOverride(old_class_loader);

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
      VLOG(jni) << "[Returned " << (result ? "successfully" : "failure")
                << " from JNI_OnLoad in \"" << path << "\"]";
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
    if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(c, true, true)) {
      return NULL;
    }
  } else {
    CHECK(c->GetStatus() >= Class::kStatusInitializing) << c->GetStatus() << " " << PrettyMethod(m);
  }

  std::string detail;
  void* native_method;
  {
    MutexLock mu(libraries_lock);
    native_method = libraries->FindNativeMethod(m, detail);
  }
  // throwing can cause libraries_lock to be reacquired
  if (native_method == NULL) {
    Thread::Current()->ThrowNewException("Ljava/lang/UnsatisfiedLinkError;", detail.c_str());
  }
  return native_method;
}

void JavaVMExt::VisitRoots(Heap::RootVisitor* visitor, void* arg) {
  {
    MutexLock mu(globals_lock);
    globals.VisitRoots(visitor, arg);
  }
  {
    MutexLock mu(pins_lock);
    pin_table.VisitRoots(visitor, arg);
  }
  // The weak_globals table is visited by the GC itself (because it mutates the table).
}

jclass CacheClass(JNIEnv* env, const char* jni_class_name) {
  ScopedLocalRef<jclass> c(env, env->FindClass(jni_class_name));
  if (c.get() == NULL) {
    return NULL;
  }
  return reinterpret_cast<jclass>(env->NewGlobalRef(c.get()));
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
  default:
    LOG(FATAL) << "jobjectRefType[" << static_cast<int>(rhs) << "]";
    return os;
  }
}
