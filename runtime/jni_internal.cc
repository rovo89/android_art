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

#include <cstdarg>
#include <memory>
#include <utility>
#include <vector>

#include "atomic.h"
#include "base/allocator.h"
#include "base/logging.h"
#include "base/mutex.h"
#include "base/stl_util.h"
#include "class_linker-inl.h"
#include "dex_file-inl.h"
#include "gc_root.h"
#include "gc/accounting/card_table-inl.h"
#include "indirect_reference_table-inl.h"
#include "interpreter/interpreter.h"
#include "jni.h"
#include "mirror/art_field-inl.h"
#include "mirror/art_method-inl.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/string-inl.h"
#include "mirror/throwable.h"
#include "parsed_options.h"
#include "reflection.h"
#include "runtime.h"
#include "safe_map.h"
#include "scoped_thread_state_change.h"
#include "ScopedLocalRef.h"
#include "thread.h"
#include "utf.h"
#include "well_known_classes.h"

namespace art {

static const size_t kMonitorsInitial = 32;  // Arbitrary.
static const size_t kMonitorsMax = 4096;  // Arbitrary sanity check.

static const size_t kLocalsInitial = 64;  // Arbitrary.
static const size_t kLocalsMax = 512;  // Arbitrary sanity check.

static size_t gGlobalsInitial = 512;  // Arbitrary.
static size_t gGlobalsMax = 51200;  // Arbitrary sanity check. (Must fit in 16 bits.)

static const size_t kWeakGlobalsInitial = 16;  // Arbitrary.
static const size_t kWeakGlobalsMax = 51200;  // Arbitrary sanity check. (Must fit in 16 bits.)

static jweak AddWeakGlobalReference(ScopedObjectAccess& soa, mirror::Object* obj)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return soa.Vm()->AddWeakGlobalReference(soa.Self(), obj);
}

static bool IsBadJniVersion(int version) {
  // We don't support JNI_VERSION_1_1. These are the only other valid versions.
  return version != JNI_VERSION_1_2 && version != JNI_VERSION_1_4 && version != JNI_VERSION_1_6;
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

static void ThrowNoSuchMethodError(ScopedObjectAccess& soa, mirror::Class* c,
                                   const char* name, const char* sig, const char* kind)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ThrowLocation throw_location = soa.Self()->GetCurrentLocationForThrow();
  std::string temp;
  soa.Self()->ThrowNewExceptionF(throw_location, "Ljava/lang/NoSuchMethodError;",
                                 "no %s method \"%s.%s%s\"",
                                 kind, c->GetDescriptor(&temp), name, sig);
}

static void ReportInvalidJNINativeMethod(const ScopedObjectAccess& soa, mirror::Class* c,
                                         const char* kind, jint idx, bool return_errors)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  LOG(return_errors ? ERROR : FATAL) << "Failed to register native method in "
      << PrettyDescriptor(c) << " in " << c->GetDexCache()->GetLocation()->ToModifiedUtf8()
      << ": " << kind << " is null at index " << idx;
  ThrowLocation throw_location = soa.Self()->GetCurrentLocationForThrow();
  soa.Self()->ThrowNewExceptionF(throw_location, "Ljava/lang/NoSuchMethodError;",
                                 "%s is null at index %d", kind, idx);
}

static mirror::Class* EnsureInitialized(Thread* self, mirror::Class* klass)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (LIKELY(klass->IsInitialized())) {
    return klass;
  }
  StackHandleScope<1> hs(self);
  Handle<mirror::Class> h_klass(hs.NewHandle(klass));
  if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(h_klass, true, true)) {
    return nullptr;
  }
  return h_klass.Get();
}

static jmethodID FindMethodID(ScopedObjectAccess& soa, jclass jni_class,
                              const char* name, const char* sig, bool is_static)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::Class* c = EnsureInitialized(soa.Self(), soa.Decode<mirror::Class*>(jni_class));
  if (c == nullptr) {
    return nullptr;
  }
  mirror::ArtMethod* method = nullptr;
  if (is_static) {
    method = c->FindDirectMethod(name, sig);
  } else if (c->IsInterface()) {
    method = c->FindInterfaceMethod(name, sig);
  } else {
    method = c->FindVirtualMethod(name, sig);
    if (method == nullptr) {
      // No virtual method matching the signature.  Search declared
      // private methods and constructors.
      method = c->FindDeclaredDirectMethod(name, sig);
    }
  }
  if (method == nullptr || method->IsStatic() != is_static) {
    ThrowNoSuchMethodError(soa, c, name, sig, is_static ? "static" : "non-static");
    return nullptr;
  }
  return soa.EncodeMethod(method);
}

static mirror::ClassLoader* GetClassLoader(const ScopedObjectAccess& soa)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::ArtMethod* method = soa.Self()->GetCurrentMethod(nullptr);
  // If we are running Runtime.nativeLoad, use the overriding ClassLoader it set.
  if (method == soa.DecodeMethod(WellKnownClasses::java_lang_Runtime_nativeLoad)) {
    return soa.Self()->GetClassLoaderOverride();
  }
  // If we have a method, use its ClassLoader for context.
  if (method != nullptr) {
    return method->GetDeclaringClass()->GetClassLoader();
  }
  // We don't have a method, so try to use the system ClassLoader.
  mirror::ClassLoader* class_loader =
      soa.Decode<mirror::ClassLoader*>(Runtime::Current()->GetSystemClassLoader());
  if (class_loader != nullptr) {
    return class_loader;
  }
  // See if the override ClassLoader is set for gtests.
  class_loader = soa.Self()->GetClassLoaderOverride();
  if (class_loader != nullptr) {
    // If so, CommonCompilerTest should have set UseCompileTimeClassPath.
    CHECK(Runtime::Current()->UseCompileTimeClassPath());
    return class_loader;
  }
  // Use the BOOTCLASSPATH.
  return nullptr;
}

static jfieldID FindFieldID(const ScopedObjectAccess& soa, jclass jni_class, const char* name,
                            const char* sig, bool is_static)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::Class> c(
      hs.NewHandle(EnsureInitialized(soa.Self(), soa.Decode<mirror::Class*>(jni_class))));
  if (c.Get() == nullptr) {
    return nullptr;
  }
  mirror::ArtField* field = nullptr;
  mirror::Class* field_type;
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  if (sig[1] != '\0') {
    Handle<mirror::ClassLoader> class_loader(hs.NewHandle(c->GetClassLoader()));
    field_type = class_linker->FindClass(soa.Self(), sig, class_loader);
  } else {
    field_type = class_linker->FindPrimitiveClass(*sig);
  }
  if (field_type == nullptr) {
    // Failed to find type from the signature of the field.
    DCHECK(soa.Self()->IsExceptionPending());
    ThrowLocation throw_location;
    StackHandleScope<1> hs(soa.Self());
    Handle<mirror::Throwable> cause(hs.NewHandle(soa.Self()->GetException(&throw_location)));
    soa.Self()->ClearException();
    std::string temp;
    soa.Self()->ThrowNewExceptionF(throw_location, "Ljava/lang/NoSuchFieldError;",
                                   "no type \"%s\" found and so no field \"%s\" "
                                   "could be found in class \"%s\" or its superclasses", sig, name,
                                   c->GetDescriptor(&temp));
    soa.Self()->GetException(nullptr)->SetCause(cause.Get());
    return nullptr;
  }
  std::string temp;
  if (is_static) {
    field = mirror::Class::FindStaticField(soa.Self(), c, name,
                                           field_type->GetDescriptor(&temp));
  } else {
    field = c->FindInstanceField(name, field_type->GetDescriptor(&temp));
  }
  if (field == nullptr) {
    ThrowLocation throw_location = soa.Self()->GetCurrentLocationForThrow();
    soa.Self()->ThrowNewExceptionF(throw_location, "Ljava/lang/NoSuchFieldError;",
                                   "no \"%s\" field \"%s\" in class \"%s\" or its superclasses",
                                   sig, name, c->GetDescriptor(&temp));
    return nullptr;
  }
  return soa.EncodeField(field);
}

static void ThrowAIOOBE(ScopedObjectAccess& soa, mirror::Array* array, jsize start,
                        jsize length, const char* identifier)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  std::string type(PrettyTypeOf(array));
  ThrowLocation throw_location = soa.Self()->GetCurrentLocationForThrow();
  soa.Self()->ThrowNewExceptionF(throw_location, "Ljava/lang/ArrayIndexOutOfBoundsException;",
                                 "%s offset=%d length=%d %s.length=%d",
                                 type.c_str(), start, length, identifier, array->GetLength());
}

static void ThrowSIOOBE(ScopedObjectAccess& soa, jsize start, jsize length,
                        jsize array_length)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ThrowLocation throw_location = soa.Self()->GetCurrentLocationForThrow();
  soa.Self()->ThrowNewExceptionF(throw_location, "Ljava/lang/StringIndexOutOfBoundsException;",
                                 "offset=%d length=%d string.length()=%d", start, length,
                                 array_length);
}

int ThrowNewException(JNIEnv* env, jclass exception_class, const char* msg, jobject cause)
    LOCKS_EXCLUDED(Locks::mutator_lock_) {
  // Turn the const char* into a java.lang.String.
  ScopedLocalRef<jstring> s(env, env->NewStringUTF(msg));
  if (msg != nullptr && s.get() == nullptr) {
    return JNI_ERR;
  }

  // Choose an appropriate constructor and set up the arguments.
  jvalue args[2];
  const char* signature;
  if (msg == nullptr && cause == nullptr) {
    signature = "()V";
  } else if (msg != nullptr && cause == nullptr) {
    signature = "(Ljava/lang/String;)V";
    args[0].l = s.get();
  } else if (msg == nullptr && cause != nullptr) {
    signature = "(Ljava/lang/Throwable;)V";
    args[0].l = cause;
  } else {
    signature = "(Ljava/lang/String;Ljava/lang/Throwable;)V";
    args[0].l = s.get();
    args[1].l = cause;
  }
  jmethodID mid = env->GetMethodID(exception_class, "<init>", signature);
  if (mid == nullptr) {
    ScopedObjectAccess soa(env);
    LOG(ERROR) << "No <init>" << signature << " in "
        << PrettyClass(soa.Decode<mirror::Class*>(exception_class));
    return JNI_ERR;
  }

  ScopedLocalRef<jthrowable> exception(
      env, reinterpret_cast<jthrowable>(env->NewObjectA(exception_class, mid, args)));
  if (exception.get() == nullptr) {
    return JNI_ERR;
  }
  ScopedObjectAccess soa(env);
  ThrowLocation throw_location = soa.Self()->GetCurrentLocationForThrow();
  soa.Self()->SetException(throw_location, soa.Decode<mirror::Throwable*>(exception.get()));
  return JNI_OK;
}

static jint JII_AttachCurrentThread(JavaVM* vm, JNIEnv** p_env, void* raw_args, bool as_daemon) {
  if (vm == nullptr || p_env == nullptr) {
    return JNI_ERR;
  }

  // Return immediately if we're already attached.
  Thread* self = Thread::Current();
  if (self != nullptr) {
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
  const char* thread_name = nullptr;
  jobject thread_group = nullptr;
  if (args != nullptr) {
    if (IsBadJniVersion(args->version)) {
      LOG(ERROR) << "Bad JNI version passed to "
                 << (as_daemon ? "AttachCurrentThreadAsDaemon" : "AttachCurrentThread") << ": "
                 << args->version;
      return JNI_EVERSION;
    }
    thread_name = args->name;
    thread_group = args->group;
  }

  if (!runtime->AttachCurrentThread(thread_name, as_daemon, thread_group, !runtime->IsCompiler())) {
    *p_env = nullptr;
    return JNI_ERR;
  } else {
    *p_env = Thread::Current()->GetJniEnv();
    return JNI_OK;
  }
}

class SharedLibrary {
 public:
  SharedLibrary(const std::string& path, void* handle, mirror::Object* class_loader)
      : path_(path),
        handle_(handle),
        needs_native_bridge_(false),
        class_loader_(GcRoot<mirror::Object>(class_loader)),
        jni_on_load_lock_("JNI_OnLoad lock"),
        jni_on_load_cond_("JNI_OnLoad condition variable", jni_on_load_lock_),
        jni_on_load_thread_id_(Thread::Current()->GetThreadId()),
        jni_on_load_result_(kPending) {
  }

  mirror::Object* GetClassLoader() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return class_loader_.Read();
  }

  std::string GetPath() {
    return path_;
  }

  /*
   * Check the result of an earlier call to JNI_OnLoad on this library.
   * If the call has not yet finished in another thread, wait for it.
   */
  bool CheckOnLoadResult()
      LOCKS_EXCLUDED(jni_on_load_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    Thread* self = Thread::Current();
    self->TransitionFromRunnableToSuspended(kWaitingForJniOnLoad);
    bool okay;
    {
      MutexLock mu(self, jni_on_load_lock_);

      if (jni_on_load_thread_id_ == self->GetThreadId()) {
        // Check this so we don't end up waiting for ourselves.  We need to return "true" so the
        // caller can continue.
        LOG(INFO) << *self << " recursive attempt to load library " << "\"" << path_ << "\"";
        okay = true;
      } else {
        while (jni_on_load_result_ == kPending) {
          VLOG(jni) << "[" << *self << " waiting for \"" << path_ << "\" " << "JNI_OnLoad...]";
          jni_on_load_cond_.Wait(self);
        }

        okay = (jni_on_load_result_ == kOkay);
        VLOG(jni) << "[Earlier JNI_OnLoad for \"" << path_ << "\" "
            << (okay ? "succeeded" : "failed") << "]";
      }
    }
    self->TransitionFromSuspendedToRunnable();
    return okay;
  }

  void SetResult(bool result) LOCKS_EXCLUDED(jni_on_load_lock_) {
    Thread* self = Thread::Current();
    MutexLock mu(self, jni_on_load_lock_);

    jni_on_load_result_ = result ? kOkay : kFailed;
    jni_on_load_thread_id_ = 0;

    // Broadcast a wakeup to anybody sleeping on the condition variable.
    jni_on_load_cond_.Broadcast(self);
  }

  void SetNeedsNativeBridge() {
    needs_native_bridge_ = true;
  }

  bool NeedsNativeBridge() const {
    return needs_native_bridge_;
  }

  void* FindSymbol(const std::string& symbol_name) {
    return dlsym(handle_, symbol_name.c_str());
  }

  void* FindSymbolWithNativeBridge(const std::string& symbol_name, mirror::ArtMethod* m)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    CHECK(NeedsNativeBridge());

    uint32_t len = 0;
    const char* shorty = nullptr;
    if (m != nullptr) {
      shorty = m->GetShorty(&len);
    }
    return android::NativeBridgeGetTrampoline(handle_, symbol_name.c_str(), shorty, len);
  }

  void VisitRoots(RootCallback* visitor, void* arg) {
    if (!class_loader_.IsNull()) {
      class_loader_.VisitRoot(visitor, arg, 0, kRootVMInternal);
    }
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

  // True if a native bridge is required.
  bool needs_native_bridge_;

  // The ClassLoader this library is associated with.
  GcRoot<mirror::Object> class_loader_;

  // Guards remaining items.
  Mutex jni_on_load_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  // Wait for JNI_OnLoad in other thread.
  ConditionVariable jni_on_load_cond_ GUARDED_BY(jni_on_load_lock_);
  // Recursive invocation guard.
  uint32_t jni_on_load_thread_id_ GUARDED_BY(jni_on_load_lock_);
  // Result of earlier JNI_OnLoad call.
  JNI_OnLoadState jni_on_load_result_ GUARDED_BY(jni_on_load_lock_);
};

// This exists mainly to keep implementation details out of the header file.
class Libraries {
 public:
  Libraries() {
  }

  ~Libraries() {
    STLDeleteValues(&libraries_);
  }

  void Dump(std::ostream& os) const {
    bool first = true;
    for (const auto& library : libraries_) {
      if (!first) {
        os << ' ';
      }
      first = false;
      os << library.first;
    }
  }

  size_t size() const {
    return libraries_.size();
  }

  SharedLibrary* Get(const std::string& path) {
    auto it = libraries_.find(path);
    return (it == libraries_.end()) ? nullptr : it->second;
  }

  void Put(const std::string& path, SharedLibrary* library) {
    libraries_.Put(path, library);
  }

  // See section 11.3 "Linking Native Methods" of the JNI spec.
  void* FindNativeMethod(mirror::ArtMethod* m, std::string& detail)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    std::string jni_short_name(JniShortName(m));
    std::string jni_long_name(JniLongName(m));
    const mirror::ClassLoader* declaring_class_loader = m->GetDeclaringClass()->GetClassLoader();
    for (const auto& lib : libraries_) {
      SharedLibrary* library = lib.second;
      if (library->GetClassLoader() != declaring_class_loader) {
        // We only search libraries loaded by the appropriate ClassLoader.
        continue;
      }
      // Try the short name then the long name...
      void* fn = nullptr;
      if (UNLIKELY(library->NeedsNativeBridge())) {
        fn = library->FindSymbolWithNativeBridge(jni_short_name, m);
        if (fn == nullptr) {
          fn = library->FindSymbolWithNativeBridge(jni_long_name, m);
        }
      } else {
        fn = library->FindSymbol(jni_short_name);
        if (fn == nullptr) {
          fn = library->FindSymbol(jni_long_name);
        }
      }
      if (fn != nullptr) {
        VLOG(jni) << "[Found native code for " << PrettyMethod(m)
                  << " in \"" << library->GetPath() << "\"]";
        return fn;
      }
    }
    detail += "No implementation found for ";
    detail += PrettyMethod(m);
    detail += " (tried " + jni_short_name + " and " + jni_long_name + ")";
    LOG(ERROR) << detail;
    return nullptr;
  }

  void VisitRoots(RootCallback* callback, void* arg) {
    for (auto& lib_pair : libraries_) {
      lib_pair.second->VisitRoots(callback, arg);
    }
  }

 private:
  AllocationTrackingSafeMap<std::string, SharedLibrary*, kAllocatorTagJNILibrarires> libraries_;
};

#define CHECK_NON_NULL_ARGUMENT(value) \
    CHECK_NON_NULL_ARGUMENT_FN_NAME(__FUNCTION__, value, nullptr)

#define CHECK_NON_NULL_ARGUMENT_RETURN_VOID(value) \
    CHECK_NON_NULL_ARGUMENT_FN_NAME(__FUNCTION__, value, )

#define CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(value) \
    CHECK_NON_NULL_ARGUMENT_FN_NAME(__FUNCTION__, value, 0)

#define CHECK_NON_NULL_ARGUMENT_RETURN(value, return_val) \
    CHECK_NON_NULL_ARGUMENT_FN_NAME(__FUNCTION__, value, return_val)

#define CHECK_NON_NULL_ARGUMENT_FN_NAME(name, value, return_val) \
  if (UNLIKELY(value == nullptr)) { \
    JniAbortF(name, #value " == null"); \
    return return_val; \
  }

#define CHECK_NON_NULL_MEMCPY_ARGUMENT(length, value) \
  if (UNLIKELY(length != 0 && value == nullptr)) { \
    JniAbortF(__FUNCTION__, #value " == null"); \
    return; \
  }

class JNI {
 public:
  static jint GetVersion(JNIEnv*) {
    return JNI_VERSION_1_6;
  }

  static jclass DefineClass(JNIEnv*, const char*, jobject, const jbyte*, jsize) {
    LOG(WARNING) << "JNI DefineClass is not supported";
    return nullptr;
  }

  static jclass FindClass(JNIEnv* env, const char* name) {
    CHECK_NON_NULL_ARGUMENT(name);
    Runtime* runtime = Runtime::Current();
    ClassLinker* class_linker = runtime->GetClassLinker();
    std::string descriptor(NormalizeJniClassDescriptor(name));
    ScopedObjectAccess soa(env);
    mirror::Class* c = nullptr;
    if (runtime->IsStarted()) {
      StackHandleScope<1> hs(soa.Self());
      Handle<mirror::ClassLoader> class_loader(hs.NewHandle(GetClassLoader(soa)));
      c = class_linker->FindClass(soa.Self(), descriptor.c_str(), class_loader);
    } else {
      c = class_linker->FindSystemClass(soa.Self(), descriptor.c_str());
    }
    return soa.AddLocalReference<jclass>(c);
  }

  static jmethodID FromReflectedMethod(JNIEnv* env, jobject jlr_method) {
    CHECK_NON_NULL_ARGUMENT(jlr_method);
    ScopedObjectAccess soa(env);
    return soa.EncodeMethod(mirror::ArtMethod::FromReflectedMethod(soa, jlr_method));
  }

  static jfieldID FromReflectedField(JNIEnv* env, jobject jlr_field) {
    CHECK_NON_NULL_ARGUMENT(jlr_field);
    ScopedObjectAccess soa(env);
    return soa.EncodeField(mirror::ArtField::FromReflectedField(soa, jlr_field));
  }

  static jobject ToReflectedMethod(JNIEnv* env, jclass, jmethodID mid, jboolean) {
    CHECK_NON_NULL_ARGUMENT(mid);
    ScopedObjectAccess soa(env);
    mirror::ArtMethod* m = soa.DecodeMethod(mid);
    CHECK(!kMovingMethods);
    jobject art_method = soa.AddLocalReference<jobject>(m);
    jobject reflect_method;
    if (m->IsConstructor()) {
      reflect_method = env->AllocObject(WellKnownClasses::java_lang_reflect_Constructor);
    } else {
      reflect_method = env->AllocObject(WellKnownClasses::java_lang_reflect_Method);
    }
    if (env->ExceptionCheck()) {
      return nullptr;
    }
    SetObjectField(env, reflect_method,
                   WellKnownClasses::java_lang_reflect_AbstractMethod_artMethod, art_method);
    return reflect_method;
  }

  static jobject ToReflectedField(JNIEnv* env, jclass, jfieldID fid, jboolean) {
    CHECK_NON_NULL_ARGUMENT(fid);
    ScopedObjectAccess soa(env);
    mirror::ArtField* f = soa.DecodeField(fid);
    jobject art_field = soa.AddLocalReference<jobject>(f);
    jobject reflect_field = env->AllocObject(WellKnownClasses::java_lang_reflect_Field);
    if (env->ExceptionCheck()) {
      return nullptr;
    }
    SetObjectField(env, reflect_field,
                   WellKnownClasses::java_lang_reflect_Field_artField, art_field);
    return reflect_field;
  }

  static jclass GetObjectClass(JNIEnv* env, jobject java_object) {
    CHECK_NON_NULL_ARGUMENT(java_object);
    ScopedObjectAccess soa(env);
    mirror::Object* o = soa.Decode<mirror::Object*>(java_object);
    return soa.AddLocalReference<jclass>(o->GetClass());
  }

  static jclass GetSuperclass(JNIEnv* env, jclass java_class) {
    CHECK_NON_NULL_ARGUMENT(java_class);
    ScopedObjectAccess soa(env);
    mirror::Class* c = soa.Decode<mirror::Class*>(java_class);
    return soa.AddLocalReference<jclass>(c->GetSuperClass());
  }

  // Note: java_class1 should be safely castable to java_class2, and
  // not the other way around.
  static jboolean IsAssignableFrom(JNIEnv* env, jclass java_class1, jclass java_class2) {
    CHECK_NON_NULL_ARGUMENT_RETURN(java_class1, JNI_FALSE);
    CHECK_NON_NULL_ARGUMENT_RETURN(java_class2, JNI_FALSE);
    ScopedObjectAccess soa(env);
    mirror::Class* c1 = soa.Decode<mirror::Class*>(java_class1);
    mirror::Class* c2 = soa.Decode<mirror::Class*>(java_class2);
    return c2->IsAssignableFrom(c1) ? JNI_TRUE : JNI_FALSE;
  }

  static jboolean IsInstanceOf(JNIEnv* env, jobject jobj, jclass java_class) {
    CHECK_NON_NULL_ARGUMENT_RETURN(java_class, JNI_FALSE);
    if (jobj == nullptr) {
      // Note: JNI is different from regular Java instanceof in this respect
      return JNI_TRUE;
    } else {
      ScopedObjectAccess soa(env);
      mirror::Object* obj = soa.Decode<mirror::Object*>(jobj);
      mirror::Class* c = soa.Decode<mirror::Class*>(java_class);
      return obj->InstanceOf(c) ? JNI_TRUE : JNI_FALSE;
    }
  }

  static jint Throw(JNIEnv* env, jthrowable java_exception) {
    ScopedObjectAccess soa(env);
    mirror::Throwable* exception = soa.Decode<mirror::Throwable*>(java_exception);
    if (exception == nullptr) {
      return JNI_ERR;
    }
    ThrowLocation throw_location = soa.Self()->GetCurrentLocationForThrow();
    soa.Self()->SetException(throw_location, exception);
    return JNI_OK;
  }

  static jint ThrowNew(JNIEnv* env, jclass c, const char* msg) {
    CHECK_NON_NULL_ARGUMENT_RETURN(c, JNI_ERR);
    return ThrowNewException(env, c, msg, nullptr);
  }

  static jboolean ExceptionCheck(JNIEnv* env) {
    return static_cast<JNIEnvExt*>(env)->self->IsExceptionPending() ? JNI_TRUE : JNI_FALSE;
  }

  static void ExceptionClear(JNIEnv* env) {
    ScopedObjectAccess soa(env);
    soa.Self()->ClearException();
  }

  static void ExceptionDescribe(JNIEnv* env) {
    ScopedObjectAccess soa(env);

    // If we have no exception to describe, pass through.
    if (!soa.Self()->GetException(nullptr)) {
      return;
    }

    StackHandleScope<3> hs(soa.Self());
    // TODO: Use nullptr instead of null handles?
    auto old_throw_this_object(hs.NewHandle<mirror::Object>(nullptr));
    auto old_throw_method(hs.NewHandle<mirror::ArtMethod>(nullptr));
    auto old_exception(hs.NewHandle<mirror::Throwable>(nullptr));
    uint32_t old_throw_dex_pc;
    bool old_is_exception_reported;
    {
      ThrowLocation old_throw_location;
      mirror::Throwable* old_exception_obj = soa.Self()->GetException(&old_throw_location);
      old_throw_this_object.Assign(old_throw_location.GetThis());
      old_throw_method.Assign(old_throw_location.GetMethod());
      old_exception.Assign(old_exception_obj);
      old_throw_dex_pc = old_throw_location.GetDexPc();
      old_is_exception_reported = soa.Self()->IsExceptionReportedToInstrumentation();
      soa.Self()->ClearException();
    }
    ScopedLocalRef<jthrowable> exception(env,
                                         soa.AddLocalReference<jthrowable>(old_exception.Get()));
    ScopedLocalRef<jclass> exception_class(env, env->GetObjectClass(exception.get()));
    jmethodID mid = env->GetMethodID(exception_class.get(), "printStackTrace", "()V");
    if (mid == nullptr) {
      LOG(WARNING) << "JNI WARNING: no printStackTrace()V in "
                   << PrettyTypeOf(old_exception.Get());
    } else {
      env->CallVoidMethod(exception.get(), mid);
      if (soa.Self()->IsExceptionPending()) {
        LOG(WARNING) << "JNI WARNING: " << PrettyTypeOf(soa.Self()->GetException(nullptr))
                     << " thrown while calling printStackTrace";
        soa.Self()->ClearException();
      }
    }
    ThrowLocation gc_safe_throw_location(old_throw_this_object.Get(), old_throw_method.Get(),
                                         old_throw_dex_pc);

    soa.Self()->SetException(gc_safe_throw_location, old_exception.Get());
    soa.Self()->SetExceptionReportedToInstrumentation(old_is_exception_reported);
  }

  static jthrowable ExceptionOccurred(JNIEnv* env) {
    ScopedObjectAccess soa(env);
    mirror::Object* exception = soa.Self()->GetException(nullptr);
    return soa.AddLocalReference<jthrowable>(exception);
  }

  static void FatalError(JNIEnv*, const char* msg) {
    LOG(FATAL) << "JNI FatalError called: " << msg;
  }

  static jint PushLocalFrame(JNIEnv* env, jint capacity) {
    // TODO: SOA may not be necessary but I do it to please lock annotations.
    ScopedObjectAccess soa(env);
    if (EnsureLocalCapacity(soa, capacity, "PushLocalFrame") != JNI_OK) {
      return JNI_ERR;
    }
    static_cast<JNIEnvExt*>(env)->PushFrame(capacity);
    return JNI_OK;
  }

  static jobject PopLocalFrame(JNIEnv* env, jobject java_survivor) {
    ScopedObjectAccess soa(env);
    mirror::Object* survivor = soa.Decode<mirror::Object*>(java_survivor);
    soa.Env()->PopFrame();
    return soa.AddLocalReference<jobject>(survivor);
  }

  static jint EnsureLocalCapacity(JNIEnv* env, jint desired_capacity) {
    // TODO: SOA may not be necessary but I do it to please lock annotations.
    ScopedObjectAccess soa(env);
    return EnsureLocalCapacity(soa, desired_capacity, "EnsureLocalCapacity");
  }

  static jobject NewGlobalRef(JNIEnv* env, jobject obj) {
    ScopedObjectAccess soa(env);
    mirror::Object* decoded_obj = soa.Decode<mirror::Object*>(obj);
    // Check for null after decoding the object to handle cleared weak globals.
    if (decoded_obj == nullptr) {
      return nullptr;
    }
    JavaVMExt* vm = soa.Vm();
    IndirectReferenceTable& globals = vm->globals;
    WriterMutexLock mu(soa.Self(), vm->globals_lock);
    IndirectRef ref = globals.Add(IRT_FIRST_SEGMENT, decoded_obj);
    return reinterpret_cast<jobject>(ref);
  }

  static void DeleteGlobalRef(JNIEnv* env, jobject obj) {
    if (obj == nullptr) {
      return;
    }
    JavaVMExt* vm = reinterpret_cast<JNIEnvExt*>(env)->vm;
    IndirectReferenceTable& globals = vm->globals;
    Thread* self = reinterpret_cast<JNIEnvExt*>(env)->self;
    WriterMutexLock mu(self, vm->globals_lock);

    if (!globals.Remove(IRT_FIRST_SEGMENT, obj)) {
      LOG(WARNING) << "JNI WARNING: DeleteGlobalRef(" << obj << ") "
                   << "failed to find entry";
    }
  }

  static jweak NewWeakGlobalRef(JNIEnv* env, jobject obj) {
    ScopedObjectAccess soa(env);
    return AddWeakGlobalReference(soa, soa.Decode<mirror::Object*>(obj));
  }

  static void DeleteWeakGlobalRef(JNIEnv* env, jweak obj) {
    if (obj != nullptr) {
      ScopedObjectAccess soa(env);
      soa.Vm()->DeleteWeakGlobalRef(soa.Self(), obj);
    }
  }

  static jobject NewLocalRef(JNIEnv* env, jobject obj) {
    ScopedObjectAccess soa(env);
    mirror::Object* decoded_obj = soa.Decode<mirror::Object*>(obj);
    // Check for null after decoding the object to handle cleared weak globals.
    if (decoded_obj == nullptr) {
      return nullptr;
    }
    return soa.AddLocalReference<jobject>(decoded_obj);
  }

  static void DeleteLocalRef(JNIEnv* env, jobject obj) {
    if (obj == nullptr) {
      return;
    }
    ScopedObjectAccess soa(env);
    IndirectReferenceTable& locals = reinterpret_cast<JNIEnvExt*>(env)->locals;

    uint32_t cookie = reinterpret_cast<JNIEnvExt*>(env)->local_ref_cookie;
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
    if (obj1 == obj2) {
      return JNI_TRUE;
    } else {
      ScopedObjectAccess soa(env);
      return (soa.Decode<mirror::Object*>(obj1) == soa.Decode<mirror::Object*>(obj2))
              ? JNI_TRUE : JNI_FALSE;
    }
  }

  static jobject AllocObject(JNIEnv* env, jclass java_class) {
    CHECK_NON_NULL_ARGUMENT(java_class);
    ScopedObjectAccess soa(env);
    mirror::Class* c = EnsureInitialized(soa.Self(), soa.Decode<mirror::Class*>(java_class));
    if (c == nullptr) {
      return nullptr;
    }
    return soa.AddLocalReference<jobject>(c->AllocObject(soa.Self()));
  }

  static jobject NewObject(JNIEnv* env, jclass java_class, jmethodID mid, ...) {
    va_list args;
    va_start(args, mid);
    CHECK_NON_NULL_ARGUMENT(java_class);
    CHECK_NON_NULL_ARGUMENT(mid);
    jobject result = NewObjectV(env, java_class, mid, args);
    va_end(args);
    return result;
  }

  static jobject NewObjectV(JNIEnv* env, jclass java_class, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT(java_class);
    CHECK_NON_NULL_ARGUMENT(mid);
    ScopedObjectAccess soa(env);
    mirror::Class* c = EnsureInitialized(soa.Self(), soa.Decode<mirror::Class*>(java_class));
    if (c == nullptr) {
      return nullptr;
    }
    mirror::Object* result = c->AllocObject(soa.Self());
    if (result == nullptr) {
      return nullptr;
    }
    jobject local_result = soa.AddLocalReference<jobject>(result);
    CallNonvirtualVoidMethodV(env, local_result, java_class, mid, args);
    if (soa.Self()->IsExceptionPending()) {
      return nullptr;
    }
    return local_result;
  }

  static jobject NewObjectA(JNIEnv* env, jclass java_class, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT(java_class);
    CHECK_NON_NULL_ARGUMENT(mid);
    ScopedObjectAccess soa(env);
    mirror::Class* c = EnsureInitialized(soa.Self(), soa.Decode<mirror::Class*>(java_class));
    if (c == nullptr) {
      return nullptr;
    }
    mirror::Object* result = c->AllocObject(soa.Self());
    if (result == nullptr) {
      return nullptr;
    }
    jobject local_result = soa.AddLocalReference<jobjectArray>(result);
    CallNonvirtualVoidMethodA(env, local_result, java_class, mid, args);
    if (soa.Self()->IsExceptionPending()) {
      return nullptr;
    }
    return local_result;
  }

  static jmethodID GetMethodID(JNIEnv* env, jclass java_class, const char* name, const char* sig) {
    CHECK_NON_NULL_ARGUMENT(java_class);
    CHECK_NON_NULL_ARGUMENT(name);
    CHECK_NON_NULL_ARGUMENT(sig);
    ScopedObjectAccess soa(env);
    return FindMethodID(soa, java_class, name, sig, false);
  }

  static jmethodID GetStaticMethodID(JNIEnv* env, jclass java_class, const char* name,
                                     const char* sig) {
    CHECK_NON_NULL_ARGUMENT(java_class);
    CHECK_NON_NULL_ARGUMENT(name);
    CHECK_NON_NULL_ARGUMENT(sig);
    ScopedObjectAccess soa(env);
    return FindMethodID(soa, java_class, name, sig, true);
  }

  static jobject CallObjectMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT(obj);
    CHECK_NON_NULL_ARGUMENT(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, ap));
    va_end(ap);
    return soa.AddLocalReference<jobject>(result.GetL());
  }

  static jobject CallObjectMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT(obj);
    CHECK_NON_NULL_ARGUMENT(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, args));
    return soa.AddLocalReference<jobject>(result.GetL());
  }

  static jobject CallObjectMethodA(JNIEnv* env, jobject obj, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT(obj);
    CHECK_NON_NULL_ARGUMENT(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeVirtualOrInterfaceWithJValues(soa, soa.Decode<mirror::Object*>(obj), mid,
                                                      args));
    return soa.AddLocalReference<jobject>(result.GetL());
  }

  static jboolean CallBooleanMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, ap));
    va_end(ap);
    return result.GetZ();
  }

  static jboolean CallBooleanMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, args).GetZ();
  }

  static jboolean CallBooleanMethodA(JNIEnv* env, jobject obj, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithJValues(soa, soa.Decode<mirror::Object*>(obj), mid,
                                               args).GetZ();
  }

  static jbyte CallByteMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, ap));
    va_end(ap);
    return result.GetB();
  }

  static jbyte CallByteMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, args).GetB();
  }

  static jbyte CallByteMethodA(JNIEnv* env, jobject obj, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithJValues(soa, soa.Decode<mirror::Object*>(obj), mid,
                                               args).GetB();
  }

  static jchar CallCharMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, ap));
    va_end(ap);
    return result.GetC();
  }

  static jchar CallCharMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, args).GetC();
  }

  static jchar CallCharMethodA(JNIEnv* env, jobject obj, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithJValues(soa, soa.Decode<mirror::Object*>(obj), mid,
                                               args).GetC();
  }

  static jdouble CallDoubleMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, ap));
    va_end(ap);
    return result.GetD();
  }

  static jdouble CallDoubleMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, args).GetD();
  }

  static jdouble CallDoubleMethodA(JNIEnv* env, jobject obj, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithJValues(soa, soa.Decode<mirror::Object*>(obj), mid,
                                               args).GetD();
  }

  static jfloat CallFloatMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, ap));
    va_end(ap);
    return result.GetF();
  }

  static jfloat CallFloatMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, args).GetF();
  }

  static jfloat CallFloatMethodA(JNIEnv* env, jobject obj, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithJValues(soa, soa.Decode<mirror::Object*>(obj), mid,
                                               args).GetF();
  }

  static jint CallIntMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, ap));
    va_end(ap);
    return result.GetI();
  }

  static jint CallIntMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, args).GetI();
  }

  static jint CallIntMethodA(JNIEnv* env, jobject obj, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithJValues(soa, soa.Decode<mirror::Object*>(obj), mid,
                                               args).GetI();
  }

  static jlong CallLongMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, ap));
    va_end(ap);
    return result.GetJ();
  }

  static jlong CallLongMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, args).GetJ();
  }

  static jlong CallLongMethodA(JNIEnv* env, jobject obj, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithJValues(soa, soa.Decode<mirror::Object*>(obj), mid,
                                               args).GetJ();
  }

  static jshort CallShortMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, ap));
    va_end(ap);
    return result.GetS();
  }

  static jshort CallShortMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, args).GetS();
  }

  static jshort CallShortMethodA(JNIEnv* env, jobject obj, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithJValues(soa, soa.Decode<mirror::Object*>(obj), mid,
                                               args).GetS();
  }

  static void CallVoidMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(mid);
    ScopedObjectAccess soa(env);
    InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, ap);
    va_end(ap);
  }

  static void CallVoidMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(mid);
    ScopedObjectAccess soa(env);
    InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, args);
  }

  static void CallVoidMethodA(JNIEnv* env, jobject obj, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(mid);
    ScopedObjectAccess soa(env);
    InvokeVirtualOrInterfaceWithJValues(soa, soa.Decode<mirror::Object*>(obj), mid, args);
  }

  static jobject CallNonvirtualObjectMethod(JNIEnv* env, jobject obj, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT(obj);
    CHECK_NON_NULL_ARGUMENT(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, obj, mid, ap));
    jobject local_result = soa.AddLocalReference<jobject>(result.GetL());
    va_end(ap);
    return local_result;
  }

  static jobject CallNonvirtualObjectMethodV(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                             va_list args) {
    CHECK_NON_NULL_ARGUMENT(obj);
    CHECK_NON_NULL_ARGUMENT(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, obj, mid, args));
    return soa.AddLocalReference<jobject>(result.GetL());
  }

  static jobject CallNonvirtualObjectMethodA(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                             jvalue* args) {
    CHECK_NON_NULL_ARGUMENT(obj);
    CHECK_NON_NULL_ARGUMENT(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithJValues(soa, soa.Decode<mirror::Object*>(obj), mid, args));
    return soa.AddLocalReference<jobject>(result.GetL());
  }

  static jboolean CallNonvirtualBooleanMethod(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                              ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, obj, mid, ap));
    va_end(ap);
    return result.GetZ();
  }

  static jboolean CallNonvirtualBooleanMethodV(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                               va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, obj, mid, args).GetZ();
  }

  static jboolean CallNonvirtualBooleanMethodA(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                               jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, soa.Decode<mirror::Object*>(obj), mid, args).GetZ();
  }

  static jbyte CallNonvirtualByteMethod(JNIEnv* env, jobject obj, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, obj, mid, ap));
    va_end(ap);
    return result.GetB();
  }

  static jbyte CallNonvirtualByteMethodV(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                         va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, obj, mid, args).GetB();
  }

  static jbyte CallNonvirtualByteMethodA(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                         jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, soa.Decode<mirror::Object*>(obj), mid, args).GetB();
  }

  static jchar CallNonvirtualCharMethod(JNIEnv* env, jobject obj, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, obj, mid, ap));
    va_end(ap);
    return result.GetC();
  }

  static jchar CallNonvirtualCharMethodV(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                         va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, obj, mid, args).GetC();
  }

  static jchar CallNonvirtualCharMethodA(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                         jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, soa.Decode<mirror::Object*>(obj), mid, args).GetC();
  }

  static jshort CallNonvirtualShortMethod(JNIEnv* env, jobject obj, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, obj, mid, ap));
    va_end(ap);
    return result.GetS();
  }

  static jshort CallNonvirtualShortMethodV(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                           va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, obj, mid, args).GetS();
  }

  static jshort CallNonvirtualShortMethodA(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                           jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, soa.Decode<mirror::Object*>(obj), mid, args).GetS();
  }

  static jint CallNonvirtualIntMethod(JNIEnv* env, jobject obj, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, obj, mid, ap));
    va_end(ap);
    return result.GetI();
  }

  static jint CallNonvirtualIntMethodV(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                       va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, obj, mid, args).GetI();
  }

  static jint CallNonvirtualIntMethodA(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                       jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, soa.Decode<mirror::Object*>(obj), mid, args).GetI();
  }

  static jlong CallNonvirtualLongMethod(JNIEnv* env, jobject obj, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, obj, mid, ap));
    va_end(ap);
    return result.GetJ();
  }

  static jlong CallNonvirtualLongMethodV(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                         va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, obj, mid, args).GetJ();
  }

  static jlong CallNonvirtualLongMethodA(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                         jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, soa.Decode<mirror::Object*>(obj), mid, args).GetJ();
  }

  static jfloat CallNonvirtualFloatMethod(JNIEnv* env, jobject obj, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, obj, mid, ap));
    va_end(ap);
    return result.GetF();
  }

  static jfloat CallNonvirtualFloatMethodV(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                           va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, obj, mid, args).GetF();
  }

  static jfloat CallNonvirtualFloatMethodA(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                           jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, soa.Decode<mirror::Object*>(obj), mid, args).GetF();
  }

  static jdouble CallNonvirtualDoubleMethod(JNIEnv* env, jobject obj, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, obj, mid, ap));
    va_end(ap);
    return result.GetD();
  }

  static jdouble CallNonvirtualDoubleMethodV(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                             va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, obj, mid, args).GetD();
  }

  static jdouble CallNonvirtualDoubleMethodA(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                             jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, soa.Decode<mirror::Object*>(obj), mid, args).GetD();
  }

  static void CallNonvirtualVoidMethod(JNIEnv* env, jobject obj, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(mid);
    ScopedObjectAccess soa(env);
    InvokeWithVarArgs(soa, obj, mid, ap);
    va_end(ap);
  }

  static void CallNonvirtualVoidMethodV(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                        va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(mid);
    ScopedObjectAccess soa(env);
    InvokeWithVarArgs(soa, obj, mid, args);
  }

  static void CallNonvirtualVoidMethodA(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                        jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(mid);
    ScopedObjectAccess soa(env);
    InvokeWithJValues(soa, soa.Decode<mirror::Object*>(obj), mid, args);
  }

  static jfieldID GetFieldID(JNIEnv* env, jclass java_class, const char* name, const char* sig) {
    CHECK_NON_NULL_ARGUMENT(java_class);
    CHECK_NON_NULL_ARGUMENT(name);
    CHECK_NON_NULL_ARGUMENT(sig);
    ScopedObjectAccess soa(env);
    return FindFieldID(soa, java_class, name, sig, false);
  }

  static jfieldID GetStaticFieldID(JNIEnv* env, jclass java_class, const char* name,
                                   const char* sig) {
    CHECK_NON_NULL_ARGUMENT(java_class);
    CHECK_NON_NULL_ARGUMENT(name);
    CHECK_NON_NULL_ARGUMENT(sig);
    ScopedObjectAccess soa(env);
    return FindFieldID(soa, java_class, name, sig, true);
  }

  static jobject GetObjectField(JNIEnv* env, jobject obj, jfieldID fid) {
    CHECK_NON_NULL_ARGUMENT(obj);
    CHECK_NON_NULL_ARGUMENT(fid);
    ScopedObjectAccess soa(env);
    mirror::Object* o = soa.Decode<mirror::Object*>(obj);
    mirror::ArtField* f = soa.DecodeField(fid);
    return soa.AddLocalReference<jobject>(f->GetObject(o));
  }

  static jobject GetStaticObjectField(JNIEnv* env, jclass, jfieldID fid) {
    CHECK_NON_NULL_ARGUMENT(fid);
    ScopedObjectAccess soa(env);
    mirror::ArtField* f = soa.DecodeField(fid);
    return soa.AddLocalReference<jobject>(f->GetObject(f->GetDeclaringClass()));
  }

  static void SetObjectField(JNIEnv* env, jobject java_object, jfieldID fid, jobject java_value) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(java_object);
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(fid);
    ScopedObjectAccess soa(env);
    mirror::Object* o = soa.Decode<mirror::Object*>(java_object);
    mirror::Object* v = soa.Decode<mirror::Object*>(java_value);
    mirror::ArtField* f = soa.DecodeField(fid);
    f->SetObject<false>(o, v);
  }

  static void SetStaticObjectField(JNIEnv* env, jclass, jfieldID fid, jobject java_value) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(fid);
    ScopedObjectAccess soa(env);
    mirror::Object* v = soa.Decode<mirror::Object*>(java_value);
    mirror::ArtField* f = soa.DecodeField(fid);
    f->SetObject<false>(f->GetDeclaringClass(), v);
  }

#define GET_PRIMITIVE_FIELD(fn, instance) \
  CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(instance); \
  CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(fid); \
  ScopedObjectAccess soa(env); \
  mirror::Object* o = soa.Decode<mirror::Object*>(instance); \
  mirror::ArtField* f = soa.DecodeField(fid); \
  return f->Get ##fn (o)

#define GET_STATIC_PRIMITIVE_FIELD(fn) \
  CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(fid); \
  ScopedObjectAccess soa(env); \
  mirror::ArtField* f = soa.DecodeField(fid); \
  return f->Get ##fn (f->GetDeclaringClass())

#define SET_PRIMITIVE_FIELD(fn, instance, value) \
  CHECK_NON_NULL_ARGUMENT_RETURN_VOID(instance); \
  CHECK_NON_NULL_ARGUMENT_RETURN_VOID(fid); \
  ScopedObjectAccess soa(env); \
  mirror::Object* o = soa.Decode<mirror::Object*>(instance); \
  mirror::ArtField* f = soa.DecodeField(fid); \
  f->Set ##fn <false>(o, value)

#define SET_STATIC_PRIMITIVE_FIELD(fn, value) \
  CHECK_NON_NULL_ARGUMENT_RETURN_VOID(fid); \
  ScopedObjectAccess soa(env); \
  mirror::ArtField* f = soa.DecodeField(fid); \
  f->Set ##fn <false>(f->GetDeclaringClass(), value)

  static jboolean GetBooleanField(JNIEnv* env, jobject obj, jfieldID fid) {
    GET_PRIMITIVE_FIELD(Boolean, obj);
  }

  static jbyte GetByteField(JNIEnv* env, jobject obj, jfieldID fid) {
    GET_PRIMITIVE_FIELD(Byte, obj);
  }

  static jchar GetCharField(JNIEnv* env, jobject obj, jfieldID fid) {
    GET_PRIMITIVE_FIELD(Char, obj);
  }

  static jshort GetShortField(JNIEnv* env, jobject obj, jfieldID fid) {
    GET_PRIMITIVE_FIELD(Short, obj);
  }

  static jint GetIntField(JNIEnv* env, jobject obj, jfieldID fid) {
    GET_PRIMITIVE_FIELD(Int, obj);
  }

  static jlong GetLongField(JNIEnv* env, jobject obj, jfieldID fid) {
    GET_PRIMITIVE_FIELD(Long, obj);
  }

  static jfloat GetFloatField(JNIEnv* env, jobject obj, jfieldID fid) {
    GET_PRIMITIVE_FIELD(Float, obj);
  }

  static jdouble GetDoubleField(JNIEnv* env, jobject obj, jfieldID fid) {
    GET_PRIMITIVE_FIELD(Double, obj);
  }

  static jboolean GetStaticBooleanField(JNIEnv* env, jclass, jfieldID fid) {
    GET_STATIC_PRIMITIVE_FIELD(Boolean);
  }

  static jbyte GetStaticByteField(JNIEnv* env, jclass, jfieldID fid) {
    GET_STATIC_PRIMITIVE_FIELD(Byte);
  }

  static jchar GetStaticCharField(JNIEnv* env, jclass, jfieldID fid) {
    GET_STATIC_PRIMITIVE_FIELD(Char);
  }

  static jshort GetStaticShortField(JNIEnv* env, jclass, jfieldID fid) {
    GET_STATIC_PRIMITIVE_FIELD(Short);
  }

  static jint GetStaticIntField(JNIEnv* env, jclass, jfieldID fid) {
    GET_STATIC_PRIMITIVE_FIELD(Int);
  }

  static jlong GetStaticLongField(JNIEnv* env, jclass, jfieldID fid) {
    GET_STATIC_PRIMITIVE_FIELD(Long);
  }

  static jfloat GetStaticFloatField(JNIEnv* env, jclass, jfieldID fid) {
    GET_STATIC_PRIMITIVE_FIELD(Float);
  }

  static jdouble GetStaticDoubleField(JNIEnv* env, jclass, jfieldID fid) {
    GET_STATIC_PRIMITIVE_FIELD(Double);
  }

  static void SetBooleanField(JNIEnv* env, jobject obj, jfieldID fid, jboolean v) {
    SET_PRIMITIVE_FIELD(Boolean, obj, v);
  }

  static void SetByteField(JNIEnv* env, jobject obj, jfieldID fid, jbyte v) {
    SET_PRIMITIVE_FIELD(Byte, obj, v);
  }

  static void SetCharField(JNIEnv* env, jobject obj, jfieldID fid, jchar v) {
    SET_PRIMITIVE_FIELD(Char, obj, v);
  }

  static void SetFloatField(JNIEnv* env, jobject obj, jfieldID fid, jfloat v) {
    SET_PRIMITIVE_FIELD(Float, obj, v);
  }

  static void SetDoubleField(JNIEnv* env, jobject obj, jfieldID fid, jdouble v) {
    SET_PRIMITIVE_FIELD(Double, obj, v);
  }

  static void SetIntField(JNIEnv* env, jobject obj, jfieldID fid, jint v) {
    SET_PRIMITIVE_FIELD(Int, obj, v);
  }

  static void SetLongField(JNIEnv* env, jobject obj, jfieldID fid, jlong v) {
    SET_PRIMITIVE_FIELD(Long, obj, v);
  }

  static void SetShortField(JNIEnv* env, jobject obj, jfieldID fid, jshort v) {
    SET_PRIMITIVE_FIELD(Short, obj, v);
  }

  static void SetStaticBooleanField(JNIEnv* env, jclass, jfieldID fid, jboolean v) {
    SET_STATIC_PRIMITIVE_FIELD(Boolean, v);
  }

  static void SetStaticByteField(JNIEnv* env, jclass, jfieldID fid, jbyte v) {
    SET_STATIC_PRIMITIVE_FIELD(Byte, v);
  }

  static void SetStaticCharField(JNIEnv* env, jclass, jfieldID fid, jchar v) {
    SET_STATIC_PRIMITIVE_FIELD(Char, v);
  }

  static void SetStaticFloatField(JNIEnv* env, jclass, jfieldID fid, jfloat v) {
    SET_STATIC_PRIMITIVE_FIELD(Float, v);
  }

  static void SetStaticDoubleField(JNIEnv* env, jclass, jfieldID fid, jdouble v) {
    SET_STATIC_PRIMITIVE_FIELD(Double, v);
  }

  static void SetStaticIntField(JNIEnv* env, jclass, jfieldID fid, jint v) {
    SET_STATIC_PRIMITIVE_FIELD(Int, v);
  }

  static void SetStaticLongField(JNIEnv* env, jclass, jfieldID fid, jlong v) {
    SET_STATIC_PRIMITIVE_FIELD(Long, v);
  }

  static void SetStaticShortField(JNIEnv* env, jclass, jfieldID fid, jshort v) {
    SET_STATIC_PRIMITIVE_FIELD(Short, v);
  }

  static jobject CallStaticObjectMethod(JNIEnv* env, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, nullptr, mid, ap));
    jobject local_result = soa.AddLocalReference<jobject>(result.GetL());
    va_end(ap);
    return local_result;
  }

  static jobject CallStaticObjectMethodV(JNIEnv* env, jclass, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, nullptr, mid, args));
    return soa.AddLocalReference<jobject>(result.GetL());
  }

  static jobject CallStaticObjectMethodA(JNIEnv* env, jclass, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithJValues(soa, nullptr, mid, args));
    return soa.AddLocalReference<jobject>(result.GetL());
  }

  static jboolean CallStaticBooleanMethod(JNIEnv* env, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, nullptr, mid, ap));
    va_end(ap);
    return result.GetZ();
  }

  static jboolean CallStaticBooleanMethodV(JNIEnv* env, jclass, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, nullptr, mid, args).GetZ();
  }

  static jboolean CallStaticBooleanMethodA(JNIEnv* env, jclass, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, nullptr, mid, args).GetZ();
  }

  static jbyte CallStaticByteMethod(JNIEnv* env, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, nullptr, mid, ap));
    va_end(ap);
    return result.GetB();
  }

  static jbyte CallStaticByteMethodV(JNIEnv* env, jclass, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, nullptr, mid, args).GetB();
  }

  static jbyte CallStaticByteMethodA(JNIEnv* env, jclass, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, nullptr, mid, args).GetB();
  }

  static jchar CallStaticCharMethod(JNIEnv* env, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, nullptr, mid, ap));
    va_end(ap);
    return result.GetC();
  }

  static jchar CallStaticCharMethodV(JNIEnv* env, jclass, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, nullptr, mid, args).GetC();
  }

  static jchar CallStaticCharMethodA(JNIEnv* env, jclass, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, nullptr, mid, args).GetC();
  }

  static jshort CallStaticShortMethod(JNIEnv* env, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, nullptr, mid, ap));
    va_end(ap);
    return result.GetS();
  }

  static jshort CallStaticShortMethodV(JNIEnv* env, jclass, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, nullptr, mid, args).GetS();
  }

  static jshort CallStaticShortMethodA(JNIEnv* env, jclass, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, nullptr, mid, args).GetS();
  }

  static jint CallStaticIntMethod(JNIEnv* env, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, nullptr, mid, ap));
    va_end(ap);
    return result.GetI();
  }

  static jint CallStaticIntMethodV(JNIEnv* env, jclass, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, nullptr, mid, args).GetI();
  }

  static jint CallStaticIntMethodA(JNIEnv* env, jclass, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, nullptr, mid, args).GetI();
  }

  static jlong CallStaticLongMethod(JNIEnv* env, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, nullptr, mid, ap));
    va_end(ap);
    return result.GetJ();
  }

  static jlong CallStaticLongMethodV(JNIEnv* env, jclass, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, nullptr, mid, args).GetJ();
  }

  static jlong CallStaticLongMethodA(JNIEnv* env, jclass, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, nullptr, mid, args).GetJ();
  }

  static jfloat CallStaticFloatMethod(JNIEnv* env, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, nullptr, mid, ap));
    va_end(ap);
    return result.GetF();
  }

  static jfloat CallStaticFloatMethodV(JNIEnv* env, jclass, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, nullptr, mid, args).GetF();
  }

  static jfloat CallStaticFloatMethodA(JNIEnv* env, jclass, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, nullptr, mid, args).GetF();
  }

  static jdouble CallStaticDoubleMethod(JNIEnv* env, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, nullptr, mid, ap));
    va_end(ap);
    return result.GetD();
  }

  static jdouble CallStaticDoubleMethodV(JNIEnv* env, jclass, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, nullptr, mid, args).GetD();
  }

  static jdouble CallStaticDoubleMethodA(JNIEnv* env, jclass, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, nullptr, mid, args).GetD();
  }

  static void CallStaticVoidMethod(JNIEnv* env, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(mid);
    ScopedObjectAccess soa(env);
    InvokeWithVarArgs(soa, nullptr, mid, ap);
    va_end(ap);
  }

  static void CallStaticVoidMethodV(JNIEnv* env, jclass, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(mid);
    ScopedObjectAccess soa(env);
    InvokeWithVarArgs(soa, nullptr, mid, args);
  }

  static void CallStaticVoidMethodA(JNIEnv* env, jclass, jmethodID mid, jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(mid);
    ScopedObjectAccess soa(env);
    InvokeWithJValues(soa, nullptr, mid, args);
  }

  static jstring NewString(JNIEnv* env, const jchar* chars, jsize char_count) {
    if (UNLIKELY(char_count < 0)) {
      JniAbortF("NewString", "char_count < 0: %d", char_count);
      return nullptr;
    }
    if (UNLIKELY(chars == nullptr && char_count > 0)) {
      JniAbortF("NewString", "chars == null && char_count > 0");
      return nullptr;
    }
    ScopedObjectAccess soa(env);
    mirror::String* result = mirror::String::AllocFromUtf16(soa.Self(), char_count, chars);
    return soa.AddLocalReference<jstring>(result);
  }

  static jstring NewStringUTF(JNIEnv* env, const char* utf) {
    if (utf == nullptr) {
      return nullptr;
    }
    ScopedObjectAccess soa(env);
    mirror::String* result = mirror::String::AllocFromModifiedUtf8(soa.Self(), utf);
    return soa.AddLocalReference<jstring>(result);
  }

  static jsize GetStringLength(JNIEnv* env, jstring java_string) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(java_string);
    ScopedObjectAccess soa(env);
    return soa.Decode<mirror::String*>(java_string)->GetLength();
  }

  static jsize GetStringUTFLength(JNIEnv* env, jstring java_string) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(java_string);
    ScopedObjectAccess soa(env);
    return soa.Decode<mirror::String*>(java_string)->GetUtfLength();
  }

  static void GetStringRegion(JNIEnv* env, jstring java_string, jsize start, jsize length,
                              jchar* buf) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(java_string);
    ScopedObjectAccess soa(env);
    mirror::String* s = soa.Decode<mirror::String*>(java_string);
    if (start < 0 || length < 0 || start + length > s->GetLength()) {
      ThrowSIOOBE(soa, start, length, s->GetLength());
    } else {
      CHECK_NON_NULL_MEMCPY_ARGUMENT(length, buf);
      const jchar* chars = s->GetCharArray()->GetData() + s->GetOffset();
      memcpy(buf, chars + start, length * sizeof(jchar));
    }
  }

  static void GetStringUTFRegion(JNIEnv* env, jstring java_string, jsize start, jsize length,
                                 char* buf) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(java_string);
    ScopedObjectAccess soa(env);
    mirror::String* s = soa.Decode<mirror::String*>(java_string);
    if (start < 0 || length < 0 || start + length > s->GetLength()) {
      ThrowSIOOBE(soa, start, length, s->GetLength());
    } else {
      CHECK_NON_NULL_MEMCPY_ARGUMENT(length, buf);
      const jchar* chars = s->GetCharArray()->GetData() + s->GetOffset();
      ConvertUtf16ToModifiedUtf8(buf, chars + start, length);
    }
  }

  static const jchar* GetStringChars(JNIEnv* env, jstring java_string, jboolean* is_copy) {
    CHECK_NON_NULL_ARGUMENT(java_string);
    ScopedObjectAccess soa(env);
    mirror::String* s = soa.Decode<mirror::String*>(java_string);
    mirror::CharArray* chars = s->GetCharArray();
    gc::Heap* heap = Runtime::Current()->GetHeap();
    if (heap->IsMovableObject(chars)) {
      if (is_copy != nullptr) {
        *is_copy = JNI_TRUE;
      }
      int32_t char_count = s->GetLength();
      int32_t offset = s->GetOffset();
      jchar* bytes = new jchar[char_count];
      for (int32_t i = 0; i < char_count; i++) {
        bytes[i] = chars->Get(i + offset);
      }
      return bytes;
    } else {
      if (is_copy != nullptr) {
        *is_copy = JNI_FALSE;
      }
      return static_cast<jchar*>(chars->GetData() + s->GetOffset());
    }
  }

  static void ReleaseStringChars(JNIEnv* env, jstring java_string, const jchar* chars) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(java_string);
    ScopedObjectAccess soa(env);
    mirror::String* s = soa.Decode<mirror::String*>(java_string);
    mirror::CharArray* s_chars = s->GetCharArray();
    if (chars != (s_chars->GetData() + s->GetOffset())) {
      delete[] chars;
    }
  }

  static const jchar* GetStringCritical(JNIEnv* env, jstring java_string, jboolean* is_copy) {
    CHECK_NON_NULL_ARGUMENT(java_string);
    ScopedObjectAccess soa(env);
    mirror::String* s = soa.Decode<mirror::String*>(java_string);
    mirror::CharArray* chars = s->GetCharArray();
    int32_t offset = s->GetOffset();
    gc::Heap* heap = Runtime::Current()->GetHeap();
    if (heap->IsMovableObject(chars)) {
      StackHandleScope<1> hs(soa.Self());
      HandleWrapper<mirror::CharArray> h(hs.NewHandleWrapper(&chars));
      heap->IncrementDisableMovingGC(soa.Self());
    }
    if (is_copy != nullptr) {
      *is_copy = JNI_FALSE;
    }
    return static_cast<jchar*>(chars->GetData() + offset);
  }

  static void ReleaseStringCritical(JNIEnv* env, jstring java_string, const jchar* chars) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(java_string);
    ScopedObjectAccess soa(env);
    gc::Heap* heap = Runtime::Current()->GetHeap();
    mirror::String* s = soa.Decode<mirror::String*>(java_string);
    mirror::CharArray* s_chars = s->GetCharArray();
    if (heap->IsMovableObject(s_chars)) {
      heap->DecrementDisableMovingGC(soa.Self());
    }
  }

  static const char* GetStringUTFChars(JNIEnv* env, jstring java_string, jboolean* is_copy) {
    if (java_string == nullptr) {
      return nullptr;
    }
    if (is_copy != nullptr) {
      *is_copy = JNI_TRUE;
    }
    ScopedObjectAccess soa(env);
    mirror::String* s = soa.Decode<mirror::String*>(java_string);
    size_t byte_count = s->GetUtfLength();
    char* bytes = new char[byte_count + 1];
    CHECK(bytes != nullptr);  // bionic aborts anyway.
    const uint16_t* chars = s->GetCharArray()->GetData() + s->GetOffset();
    ConvertUtf16ToModifiedUtf8(bytes, chars, s->GetLength());
    bytes[byte_count] = '\0';
    return bytes;
  }

  static void ReleaseStringUTFChars(JNIEnv* env, jstring, const char* chars) {
    delete[] chars;
  }

  static jsize GetArrayLength(JNIEnv* env, jarray java_array) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(java_array);
    ScopedObjectAccess soa(env);
    mirror::Object* obj = soa.Decode<mirror::Object*>(java_array);
    if (UNLIKELY(!obj->IsArrayInstance())) {
      JniAbortF("GetArrayLength", "not an array: %s", PrettyTypeOf(obj).c_str());
    }
    mirror::Array* array = obj->AsArray();
    return array->GetLength();
  }

  static jobject GetObjectArrayElement(JNIEnv* env, jobjectArray java_array, jsize index) {
    CHECK_NON_NULL_ARGUMENT(java_array);
    ScopedObjectAccess soa(env);
    mirror::ObjectArray<mirror::Object>* array =
        soa.Decode<mirror::ObjectArray<mirror::Object>*>(java_array);
    return soa.AddLocalReference<jobject>(array->Get(index));
  }

  static void SetObjectArrayElement(JNIEnv* env, jobjectArray java_array, jsize index,
                                    jobject java_value) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(java_array);
    ScopedObjectAccess soa(env);
    mirror::ObjectArray<mirror::Object>* array =
        soa.Decode<mirror::ObjectArray<mirror::Object>*>(java_array);
    mirror::Object* value = soa.Decode<mirror::Object*>(java_value);
    array->Set<false>(index, value);
  }

  static jbooleanArray NewBooleanArray(JNIEnv* env, jsize length) {
    return NewPrimitiveArray<jbooleanArray, mirror::BooleanArray>(env, length);
  }

  static jbyteArray NewByteArray(JNIEnv* env, jsize length) {
    return NewPrimitiveArray<jbyteArray, mirror::ByteArray>(env, length);
  }

  static jcharArray NewCharArray(JNIEnv* env, jsize length) {
    return NewPrimitiveArray<jcharArray, mirror::CharArray>(env, length);
  }

  static jdoubleArray NewDoubleArray(JNIEnv* env, jsize length) {
    return NewPrimitiveArray<jdoubleArray, mirror::DoubleArray>(env, length);
  }

  static jfloatArray NewFloatArray(JNIEnv* env, jsize length) {
    return NewPrimitiveArray<jfloatArray, mirror::FloatArray>(env, length);
  }

  static jintArray NewIntArray(JNIEnv* env, jsize length) {
    return NewPrimitiveArray<jintArray, mirror::IntArray>(env, length);
  }

  static jlongArray NewLongArray(JNIEnv* env, jsize length) {
    return NewPrimitiveArray<jlongArray, mirror::LongArray>(env, length);
  }

  static jobjectArray NewObjectArray(JNIEnv* env, jsize length, jclass element_jclass,
                                     jobject initial_element) {
    if (UNLIKELY(length < 0)) {
      JniAbortF("NewObjectArray", "negative array length: %d", length);
      return nullptr;
    }
    CHECK_NON_NULL_ARGUMENT(element_jclass);

    // Compute the array class corresponding to the given element class.
    ScopedObjectAccess soa(env);
    mirror::Class* array_class;
    {
      mirror::Class* element_class = soa.Decode<mirror::Class*>(element_jclass);
      if (UNLIKELY(element_class->IsPrimitive())) {
        JniAbortF("NewObjectArray", "not an object type: %s",
                  PrettyDescriptor(element_class).c_str());
        return nullptr;
      }
      ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
      array_class = class_linker->FindArrayClass(soa.Self(), &element_class);
      if (UNLIKELY(array_class == nullptr)) {
        return nullptr;
      }
    }

    // Allocate and initialize if necessary.
    mirror::ObjectArray<mirror::Object>* result =
        mirror::ObjectArray<mirror::Object>::Alloc(soa.Self(), array_class, length);
    if (result != nullptr && initial_element != nullptr) {
      mirror::Object* initial_object = soa.Decode<mirror::Object*>(initial_element);
      if (initial_object != nullptr) {
        mirror::Class* element_class = result->GetClass()->GetComponentType();
        if (UNLIKELY(!element_class->IsAssignableFrom(initial_object->GetClass()))) {
          JniAbortF("NewObjectArray", "cannot assign object of type '%s' to array with element "
                    "type of '%s'", PrettyDescriptor(initial_object->GetClass()).c_str(),
                    PrettyDescriptor(element_class).c_str());

        } else {
          for (jsize i = 0; i < length; ++i) {
            result->SetWithoutChecks<false>(i, initial_object);
          }
        }
      }
    }
    return soa.AddLocalReference<jobjectArray>(result);
  }

  static jshortArray NewShortArray(JNIEnv* env, jsize length) {
    return NewPrimitiveArray<jshortArray, mirror::ShortArray>(env, length);
  }

  static void* GetPrimitiveArrayCritical(JNIEnv* env, jarray java_array, jboolean* is_copy) {
    CHECK_NON_NULL_ARGUMENT(java_array);
    ScopedObjectAccess soa(env);
    mirror::Array* array = soa.Decode<mirror::Array*>(java_array);
    if (UNLIKELY(!array->GetClass()->IsPrimitiveArray())) {
      JniAbortF("GetPrimitiveArrayCritical", "expected primitive array, given %s",
                PrettyDescriptor(array->GetClass()).c_str());
      return nullptr;
    }
    gc::Heap* heap = Runtime::Current()->GetHeap();
    if (heap->IsMovableObject(array)) {
      heap->IncrementDisableMovingGC(soa.Self());
      // Re-decode in case the object moved since IncrementDisableGC waits for GC to complete.
      array = soa.Decode<mirror::Array*>(java_array);
    }
    if (is_copy != nullptr) {
      *is_copy = JNI_FALSE;
    }
    return array->GetRawData(array->GetClass()->GetComponentSize(), 0);
  }

  static void ReleasePrimitiveArrayCritical(JNIEnv* env, jarray java_array, void* elements,
                                            jint mode) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(java_array);
    ScopedObjectAccess soa(env);
    mirror::Array* array = soa.Decode<mirror::Array*>(java_array);
    if (UNLIKELY(!array->GetClass()->IsPrimitiveArray())) {
      JniAbortF("ReleasePrimitiveArrayCritical", "expected primitive array, given %s",
                PrettyDescriptor(array->GetClass()).c_str());
      return;
    }
    const size_t component_size = array->GetClass()->GetComponentSize();
    ReleasePrimitiveArray(soa, array, component_size, elements, mode);
  }

  static jboolean* GetBooleanArrayElements(JNIEnv* env, jbooleanArray array, jboolean* is_copy) {
    return GetPrimitiveArray<jbooleanArray, jboolean, mirror::BooleanArray>(env, array, is_copy);
  }

  static jbyte* GetByteArrayElements(JNIEnv* env, jbyteArray array, jboolean* is_copy) {
    return GetPrimitiveArray<jbyteArray, jbyte, mirror::ByteArray>(env, array, is_copy);
  }

  static jchar* GetCharArrayElements(JNIEnv* env, jcharArray array, jboolean* is_copy) {
    return GetPrimitiveArray<jcharArray, jchar, mirror::CharArray>(env, array, is_copy);
  }

  static jdouble* GetDoubleArrayElements(JNIEnv* env, jdoubleArray array, jboolean* is_copy) {
    return GetPrimitiveArray<jdoubleArray, jdouble, mirror::DoubleArray>(env, array, is_copy);
  }

  static jfloat* GetFloatArrayElements(JNIEnv* env, jfloatArray array, jboolean* is_copy) {
    return GetPrimitiveArray<jfloatArray, jfloat, mirror::FloatArray>(env, array, is_copy);
  }

  static jint* GetIntArrayElements(JNIEnv* env, jintArray array, jboolean* is_copy) {
    return GetPrimitiveArray<jintArray, jint, mirror::IntArray>(env, array, is_copy);
  }

  static jlong* GetLongArrayElements(JNIEnv* env, jlongArray array, jboolean* is_copy) {
    return GetPrimitiveArray<jlongArray, jlong, mirror::LongArray>(env, array, is_copy);
  }

  static jshort* GetShortArrayElements(JNIEnv* env, jshortArray array, jboolean* is_copy) {
    return GetPrimitiveArray<jshortArray, jshort, mirror::ShortArray>(env, array, is_copy);
  }

  static void ReleaseBooleanArrayElements(JNIEnv* env, jbooleanArray array, jboolean* elements,
                                          jint mode) {
    ReleasePrimitiveArray<jbooleanArray, jboolean, mirror::BooleanArray>(env, array, elements,
                                                                         mode);
  }

  static void ReleaseByteArrayElements(JNIEnv* env, jbyteArray array, jbyte* elements, jint mode) {
    ReleasePrimitiveArray<jbyteArray, jbyte, mirror::ByteArray>(env, array, elements, mode);
  }

  static void ReleaseCharArrayElements(JNIEnv* env, jcharArray array, jchar* elements, jint mode) {
    ReleasePrimitiveArray<jcharArray, jchar, mirror::CharArray>(env, array, elements, mode);
  }

  static void ReleaseDoubleArrayElements(JNIEnv* env, jdoubleArray array, jdouble* elements,
                                         jint mode) {
    ReleasePrimitiveArray<jdoubleArray, jdouble, mirror::DoubleArray>(env, array, elements, mode);
  }

  static void ReleaseFloatArrayElements(JNIEnv* env, jfloatArray array, jfloat* elements,
                                        jint mode) {
    ReleasePrimitiveArray<jfloatArray, jfloat, mirror::FloatArray>(env, array, elements, mode);
  }

  static void ReleaseIntArrayElements(JNIEnv* env, jintArray array, jint* elements, jint mode) {
    ReleasePrimitiveArray<jintArray, jint, mirror::IntArray>(env, array, elements, mode);
  }

  static void ReleaseLongArrayElements(JNIEnv* env, jlongArray array, jlong* elements, jint mode) {
    ReleasePrimitiveArray<jlongArray, jlong, mirror::LongArray>(env, array, elements, mode);
  }

  static void ReleaseShortArrayElements(JNIEnv* env, jshortArray array, jshort* elements,
                                        jint mode) {
    ReleasePrimitiveArray<jshortArray, jshort, mirror::ShortArray>(env, array, elements, mode);
  }

  static void GetBooleanArrayRegion(JNIEnv* env, jbooleanArray array, jsize start, jsize length,
                                    jboolean* buf) {
    GetPrimitiveArrayRegion<jbooleanArray, jboolean, mirror::BooleanArray>(env, array, start,
                                                                           length, buf);
  }

  static void GetByteArrayRegion(JNIEnv* env, jbyteArray array, jsize start, jsize length,
                                 jbyte* buf) {
    GetPrimitiveArrayRegion<jbyteArray, jbyte, mirror::ByteArray>(env, array, start, length, buf);
  }

  static void GetCharArrayRegion(JNIEnv* env, jcharArray array, jsize start, jsize length,
                                 jchar* buf) {
    GetPrimitiveArrayRegion<jcharArray, jchar, mirror::CharArray>(env, array, start, length, buf);
  }

  static void GetDoubleArrayRegion(JNIEnv* env, jdoubleArray array, jsize start, jsize length,
                                   jdouble* buf) {
    GetPrimitiveArrayRegion<jdoubleArray, jdouble, mirror::DoubleArray>(env, array, start, length,
                                                                        buf);
  }

  static void GetFloatArrayRegion(JNIEnv* env, jfloatArray array, jsize start, jsize length,
                                  jfloat* buf) {
    GetPrimitiveArrayRegion<jfloatArray, jfloat, mirror::FloatArray>(env, array, start, length,
                                                                     buf);
  }

  static void GetIntArrayRegion(JNIEnv* env, jintArray array, jsize start, jsize length,
                                jint* buf) {
    GetPrimitiveArrayRegion<jintArray, jint, mirror::IntArray>(env, array, start, length, buf);
  }

  static void GetLongArrayRegion(JNIEnv* env, jlongArray array, jsize start, jsize length,
                                 jlong* buf) {
    GetPrimitiveArrayRegion<jlongArray, jlong, mirror::LongArray>(env, array, start, length, buf);
  }

  static void GetShortArrayRegion(JNIEnv* env, jshortArray array, jsize start, jsize length,
                                  jshort* buf) {
    GetPrimitiveArrayRegion<jshortArray, jshort, mirror::ShortArray>(env, array, start, length,
                                                                     buf);
  }

  static void SetBooleanArrayRegion(JNIEnv* env, jbooleanArray array, jsize start, jsize length,
                                    const jboolean* buf) {
    SetPrimitiveArrayRegion<jbooleanArray, jboolean, mirror::BooleanArray>(env, array, start,
                                                                           length, buf);
  }

  static void SetByteArrayRegion(JNIEnv* env, jbyteArray array, jsize start, jsize length,
                                 const jbyte* buf) {
    SetPrimitiveArrayRegion<jbyteArray, jbyte, mirror::ByteArray>(env, array, start, length, buf);
  }

  static void SetCharArrayRegion(JNIEnv* env, jcharArray array, jsize start, jsize length,
                                 const jchar* buf) {
    SetPrimitiveArrayRegion<jcharArray, jchar, mirror::CharArray>(env, array, start, length, buf);
  }

  static void SetDoubleArrayRegion(JNIEnv* env, jdoubleArray array, jsize start, jsize length,
                                   const jdouble* buf) {
    SetPrimitiveArrayRegion<jdoubleArray, jdouble, mirror::DoubleArray>(env, array, start, length,
                                                                        buf);
  }

  static void SetFloatArrayRegion(JNIEnv* env, jfloatArray array, jsize start, jsize length,
                                  const jfloat* buf) {
    SetPrimitiveArrayRegion<jfloatArray, jfloat, mirror::FloatArray>(env, array, start, length,
                                                                     buf);
  }

  static void SetIntArrayRegion(JNIEnv* env, jintArray array, jsize start, jsize length,
                                const jint* buf) {
    SetPrimitiveArrayRegion<jintArray, jint, mirror::IntArray>(env, array, start, length, buf);
  }

  static void SetLongArrayRegion(JNIEnv* env, jlongArray array, jsize start, jsize length,
                                 const jlong* buf) {
    SetPrimitiveArrayRegion<jlongArray, jlong, mirror::LongArray>(env, array, start, length, buf);
  }

  static void SetShortArrayRegion(JNIEnv* env, jshortArray array, jsize start, jsize length,
                                  const jshort* buf) {
    SetPrimitiveArrayRegion<jshortArray, jshort, mirror::ShortArray>(env, array, start, length,
                                                                     buf);
  }

  static jint RegisterNatives(JNIEnv* env, jclass java_class, const JNINativeMethod* methods,
                              jint method_count) {
    return RegisterNativeMethods(env, java_class, methods, method_count, true);
  }

  static jint RegisterNativeMethods(JNIEnv* env, jclass java_class, const JNINativeMethod* methods,
                                    jint method_count, bool return_errors) {
    if (UNLIKELY(method_count < 0)) {
      JniAbortF("RegisterNatives", "negative method count: %d", method_count);
      return JNI_ERR;  // Not reached.
    }
    CHECK_NON_NULL_ARGUMENT_FN_NAME("RegisterNatives", java_class, JNI_ERR);
    ScopedObjectAccess soa(env);
    mirror::Class* c = soa.Decode<mirror::Class*>(java_class);
    if (UNLIKELY(method_count == 0)) {
      LOG(WARNING) << "JNI RegisterNativeMethods: attempt to register 0 native methods for "
          << PrettyDescriptor(c);
      return JNI_OK;
    }
    CHECK_NON_NULL_ARGUMENT_FN_NAME("RegisterNatives", methods, JNI_ERR);
    for (jint i = 0; i < method_count; ++i) {
      const char* name = methods[i].name;
      const char* sig = methods[i].signature;
      const void* fnPtr = methods[i].fnPtr;
      if (UNLIKELY(name == nullptr)) {
        ReportInvalidJNINativeMethod(soa, c, "method name", i, return_errors);
        return JNI_ERR;
      } else if (UNLIKELY(sig == nullptr)) {
        ReportInvalidJNINativeMethod(soa, c, "method signature", i, return_errors);
        return JNI_ERR;
      } else if (UNLIKELY(fnPtr == nullptr)) {
        ReportInvalidJNINativeMethod(soa, c, "native function", i, return_errors);
        return JNI_ERR;
      }
      bool is_fast = false;
      if (*sig == '!') {
        is_fast = true;
        ++sig;
      }

      mirror::ArtMethod* m = c->FindDirectMethod(name, sig);
      if (m == nullptr) {
        m = c->FindVirtualMethod(name, sig);
      }
      if (m == nullptr) {
        c->DumpClass(LOG(ERROR), mirror::Class::kDumpClassFullDetail);
        LOG(return_errors ? ERROR : FATAL) << "Failed to register native method "
            << PrettyDescriptor(c) << "." << name << sig << " in "
            << c->GetDexCache()->GetLocation()->ToModifiedUtf8();
        ThrowNoSuchMethodError(soa, c, name, sig, "static or non-static");
        return JNI_ERR;
      } else if (!m->IsNative()) {
        LOG(return_errors ? ERROR : FATAL) << "Failed to register non-native method "
            << PrettyDescriptor(c) << "." << name << sig
            << " as native";
        ThrowNoSuchMethodError(soa, c, name, sig, "native");
        return JNI_ERR;
      }

      VLOG(jni) << "[Registering JNI native method " << PrettyMethod(m) << "]";

      m->RegisterNative(soa.Self(), fnPtr, is_fast);
    }
    return JNI_OK;
  }

  static jint UnregisterNatives(JNIEnv* env, jclass java_class) {
    CHECK_NON_NULL_ARGUMENT_RETURN(java_class, JNI_ERR);
    ScopedObjectAccess soa(env);
    mirror::Class* c = soa.Decode<mirror::Class*>(java_class);

    VLOG(jni) << "[Unregistering JNI native methods for " << PrettyClass(c) << "]";

    size_t unregistered_count = 0;
    for (size_t i = 0; i < c->NumDirectMethods(); ++i) {
      mirror::ArtMethod* m = c->GetDirectMethod(i);
      if (m->IsNative()) {
        m->UnregisterNative(soa.Self());
        unregistered_count++;
      }
    }
    for (size_t i = 0; i < c->NumVirtualMethods(); ++i) {
      mirror::ArtMethod* m = c->GetVirtualMethod(i);
      if (m->IsNative()) {
        m->UnregisterNative(soa.Self());
        unregistered_count++;
      }
    }

    if (unregistered_count == 0) {
      LOG(WARNING) << "JNI UnregisterNatives: attempt to unregister native methods of class '"
          << PrettyDescriptor(c) << "' that contains no native methods";
    }
    return JNI_OK;
  }

  static jint MonitorEnter(JNIEnv* env, jobject java_object) NO_THREAD_SAFETY_ANALYSIS {
    CHECK_NON_NULL_ARGUMENT_RETURN(java_object, JNI_ERR);
    ScopedObjectAccess soa(env);
    mirror::Object* o = soa.Decode<mirror::Object*>(java_object);
    o = o->MonitorEnter(soa.Self());
    if (soa.Self()->IsExceptionPending()) {
      return JNI_ERR;
    }
    soa.Env()->monitors.Add(o);
    return JNI_OK;
  }

  static jint MonitorExit(JNIEnv* env, jobject java_object) NO_THREAD_SAFETY_ANALYSIS {
    CHECK_NON_NULL_ARGUMENT_RETURN(java_object, JNI_ERR);
    ScopedObjectAccess soa(env);
    mirror::Object* o = soa.Decode<mirror::Object*>(java_object);
    o->MonitorExit(soa.Self());
    if (soa.Self()->IsExceptionPending()) {
      return JNI_ERR;
    }
    soa.Env()->monitors.Remove(o);
    return JNI_OK;
  }

  static jint GetJavaVM(JNIEnv* env, JavaVM** vm) {
    CHECK_NON_NULL_ARGUMENT_RETURN(vm, JNI_ERR);
    Runtime* runtime = Runtime::Current();
    if (runtime != nullptr) {
      *vm = runtime->GetJavaVM();
    } else {
      *vm = nullptr;
    }
    return (*vm != nullptr) ? JNI_OK : JNI_ERR;
  }

  static jobject NewDirectByteBuffer(JNIEnv* env, void* address, jlong capacity) {
    if (capacity < 0) {
      JniAbortF("NewDirectByteBuffer", "negative buffer capacity: %" PRId64, capacity);
      return nullptr;
    }
    if (address == nullptr && capacity != 0) {
      JniAbortF("NewDirectByteBuffer", "non-zero capacity for nullptr pointer: %" PRId64, capacity);
      return nullptr;
    }

    // At the moment, the capacity of DirectByteBuffer is limited to a signed int.
    if (capacity > INT_MAX) {
      JniAbortF("NewDirectByteBuffer", "buffer capacity greater than maximum jint: %" PRId64, capacity);
      return nullptr;
    }
    jlong address_arg = reinterpret_cast<jlong>(address);
    jint capacity_arg = static_cast<jint>(capacity);

    jobject result = env->NewObject(WellKnownClasses::java_nio_DirectByteBuffer,
                                    WellKnownClasses::java_nio_DirectByteBuffer_init,
                                    address_arg, capacity_arg);
    return static_cast<JNIEnvExt*>(env)->self->IsExceptionPending() ? nullptr : result;
  }

  static void* GetDirectBufferAddress(JNIEnv* env, jobject java_buffer) {
    return reinterpret_cast<void*>(env->GetLongField(
        java_buffer, WellKnownClasses::java_nio_DirectByteBuffer_effectiveDirectAddress));
  }

  static jlong GetDirectBufferCapacity(JNIEnv* env, jobject java_buffer) {
    return static_cast<jlong>(env->GetIntField(
        java_buffer, WellKnownClasses::java_nio_DirectByteBuffer_capacity));
  }

  static jobjectRefType GetObjectRefType(JNIEnv* env, jobject java_object) {
    CHECK_NON_NULL_ARGUMENT_RETURN(java_object, JNIInvalidRefType);

    // Do we definitely know what kind of reference this is?
    IndirectRef ref = reinterpret_cast<IndirectRef>(java_object);
    IndirectRefKind kind = GetIndirectRefKind(ref);
    switch (kind) {
    case kLocal: {
      ScopedObjectAccess soa(env);
      // The local refs don't need a read barrier.
      if (static_cast<JNIEnvExt*>(env)->locals.Get<kWithoutReadBarrier>(ref) !=
          kInvalidIndirectRefObject) {
        return JNILocalRefType;
      }
      return JNIInvalidRefType;
    }
    case kGlobal:
      return JNIGlobalRefType;
    case kWeakGlobal:
      return JNIWeakGlobalRefType;
    case kHandleScopeOrInvalid:
      // Is it in a stack IRT?
      if (static_cast<JNIEnvExt*>(env)->self->HandleScopeContains(java_object)) {
        return JNILocalRefType;
      }
      return JNIInvalidRefType;
    }
    LOG(FATAL) << "IndirectRefKind[" << kind << "]";
    return JNIInvalidRefType;
  }

 private:
  static jint EnsureLocalCapacity(ScopedObjectAccess& soa, jint desired_capacity,
                                  const char* caller) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    // TODO: we should try to expand the table if necessary.
    if (desired_capacity < 0 || desired_capacity > static_cast<jint>(kLocalsMax)) {
      LOG(ERROR) << "Invalid capacity given to " << caller << ": " << desired_capacity;
      return JNI_ERR;
    }
    // TODO: this isn't quite right, since "capacity" includes holes.
    const size_t capacity = soa.Env()->locals.Capacity();
    bool okay = (static_cast<jint>(kLocalsMax - capacity) >= desired_capacity);
    if (!okay) {
      soa.Self()->ThrowOutOfMemoryError(caller);
    }
    return okay ? JNI_OK : JNI_ERR;
  }

  template<typename JniT, typename ArtT>
  static JniT NewPrimitiveArray(JNIEnv* env, jsize length) {
    if (UNLIKELY(length < 0)) {
      JniAbortF("NewPrimitiveArray", "negative array length: %d", length);
      return nullptr;
    }
    ScopedObjectAccess soa(env);
    ArtT* result = ArtT::Alloc(soa.Self(), length);
    return soa.AddLocalReference<JniT>(result);
  }

  template <typename JArrayT, typename ElementT, typename ArtArrayT>
  static ArtArrayT* DecodeAndCheckArrayType(ScopedObjectAccess& soa, JArrayT java_array,
                                           const char* fn_name, const char* operation)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    ArtArrayT* array = soa.Decode<ArtArrayT*>(java_array);
    if (UNLIKELY(ArtArrayT::GetArrayClass() != array->GetClass())) {
      JniAbortF(fn_name, "attempt to %s %s primitive array elements with an object of type %s",
                operation, PrettyDescriptor(ArtArrayT::GetArrayClass()->GetComponentType()).c_str(),
                PrettyDescriptor(array->GetClass()).c_str());
      return nullptr;
    }
    DCHECK_EQ(sizeof(ElementT), array->GetClass()->GetComponentSize());
    return array;
  }

  template <typename ArrayT, typename ElementT, typename ArtArrayT>
  static ElementT* GetPrimitiveArray(JNIEnv* env, ArrayT java_array, jboolean* is_copy) {
    CHECK_NON_NULL_ARGUMENT(java_array);
    ScopedObjectAccess soa(env);
    ArtArrayT* array = DecodeAndCheckArrayType<ArrayT, ElementT, ArtArrayT>(soa, java_array,
                                                                            "GetArrayElements",
                                                                            "get");
    if (UNLIKELY(array == nullptr)) {
      return nullptr;
    }
    // Only make a copy if necessary.
    if (Runtime::Current()->GetHeap()->IsMovableObject(array)) {
      if (is_copy != nullptr) {
        *is_copy = JNI_TRUE;
      }
      const size_t component_size = sizeof(ElementT);
      size_t size = array->GetLength() * component_size;
      void* data = new uint64_t[RoundUp(size, 8) / 8];
      memcpy(data, array->GetData(), size);
      return reinterpret_cast<ElementT*>(data);
    } else {
      if (is_copy != nullptr) {
        *is_copy = JNI_FALSE;
      }
      return reinterpret_cast<ElementT*>(array->GetData());
    }
  }

  template <typename ArrayT, typename ElementT, typename ArtArrayT>
  static void ReleasePrimitiveArray(JNIEnv* env, ArrayT java_array, ElementT* elements, jint mode) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(java_array);
    ScopedObjectAccess soa(env);
    ArtArrayT* array = DecodeAndCheckArrayType<ArrayT, ElementT, ArtArrayT>(soa, java_array,
                                                                            "ReleaseArrayElements",
                                                                            "release");
    if (array == nullptr) {
      return;
    }
    ReleasePrimitiveArray(soa, array, sizeof(ElementT), elements, mode);
  }

  static void ReleasePrimitiveArray(ScopedObjectAccess& soa, mirror::Array* array,
                                    size_t component_size, void* elements, jint mode)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    void* array_data = array->GetRawData(component_size, 0);
    gc::Heap* heap = Runtime::Current()->GetHeap();
    bool is_copy = array_data != elements;
    size_t bytes = array->GetLength() * component_size;
    VLOG(heap) << "Release primitive array " << soa.Env() << " array_data " << array_data
               << " elements " << elements;
    if (is_copy) {
      // Sanity check: If elements is not the same as the java array's data, it better not be a
      // heap address. TODO: This might be slow to check, may be worth keeping track of which
      // copies we make?
      if (heap->IsNonDiscontinuousSpaceHeapAddress(reinterpret_cast<mirror::Object*>(elements))) {
        JniAbortF("ReleaseArrayElements", "invalid element pointer %p, array elements are %p",
                  reinterpret_cast<void*>(elements), array_data);
        return;
      }
    }
    // Don't need to copy if we had a direct pointer.
    if (mode != JNI_ABORT && is_copy) {
      memcpy(array_data, elements, bytes);
    }
    if (mode != JNI_COMMIT) {
      if (is_copy) {
        delete[] reinterpret_cast<uint64_t*>(elements);
      } else if (heap->IsMovableObject(array)) {
        // Non copy to a movable object must means that we had disabled the moving GC.
        heap->DecrementDisableMovingGC(soa.Self());
      }
    }
  }

  template <typename JArrayT, typename ElementT, typename ArtArrayT>
  static void GetPrimitiveArrayRegion(JNIEnv* env, JArrayT java_array,
                                      jsize start, jsize length, ElementT* buf) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(java_array);
    ScopedObjectAccess soa(env);
    ArtArrayT* array =
        DecodeAndCheckArrayType<JArrayT, ElementT, ArtArrayT>(soa, java_array,
                                                              "GetPrimitiveArrayRegion",
                                                              "get region of");
    if (array != nullptr) {
      if (start < 0 || length < 0 || start + length > array->GetLength()) {
        ThrowAIOOBE(soa, array, start, length, "src");
      } else {
        CHECK_NON_NULL_MEMCPY_ARGUMENT(length, buf);
        ElementT* data = array->GetData();
        memcpy(buf, data + start, length * sizeof(ElementT));
      }
    }
  }

  template <typename JArrayT, typename ElementT, typename ArtArrayT>
  static void SetPrimitiveArrayRegion(JNIEnv* env, JArrayT java_array,
                                      jsize start, jsize length, const ElementT* buf) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(java_array);
    ScopedObjectAccess soa(env);
    ArtArrayT* array =
        DecodeAndCheckArrayType<JArrayT, ElementT, ArtArrayT>(soa, java_array,
                                                              "SetPrimitiveArrayRegion",
                                                              "set region of");
    if (array != nullptr) {
      if (start < 0 || length < 0 || start + length > array->GetLength()) {
        ThrowAIOOBE(soa, array, start, length, "dst");
      } else {
        CHECK_NON_NULL_MEMCPY_ARGUMENT(length, buf);
        ElementT* data = array->GetData();
        memcpy(data + start, buf, length * sizeof(ElementT));
      }
    }
  }
};

const JNINativeInterface gJniNativeInterface = {
  nullptr,  // reserved0.
  nullptr,  // reserved1.
  nullptr,  // reserved2.
  nullptr,  // reserved3.
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
      critical(0),
      monitors("monitors", kMonitorsInitial, kMonitorsMax) {
  functions = unchecked_functions = &gJniNativeInterface;
  if (vm->check_jni) {
    SetCheckJniEnabled(true);
  }
}

JNIEnvExt::~JNIEnvExt() {
}

jobject JNIEnvExt::NewLocalRef(mirror::Object* obj) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (obj == nullptr) {
    return nullptr;
  }
  return reinterpret_cast<jobject>(locals.Add(local_ref_cookie, obj));
}

void JNIEnvExt::DeleteLocalRef(jobject obj) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (obj != nullptr) {
    locals.Remove(local_ref_cookie, reinterpret_cast<IndirectRef>(obj));
  }
}
void JNIEnvExt::SetCheckJniEnabled(bool enabled) {
  check_jni = enabled;
  functions = enabled ? GetCheckJniNativeInterface() : &gJniNativeInterface;
}

void JNIEnvExt::DumpReferenceTables(std::ostream& os) {
  locals.Dump(os);
  monitors.Dump(os);
}

void JNIEnvExt::PushFrame(int capacity) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  UNUSED(capacity);  // cpplint gets confused with (int) and thinks its a cast.
  // TODO: take 'capacity' into account.
  stacked_local_ref_cookies.push_back(local_ref_cookie);
  local_ref_cookie = locals.GetSegmentState();
}

void JNIEnvExt::PopFrame() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  locals.SetSegmentState(local_ref_cookie);
  local_ref_cookie = stacked_local_ref_cookies.back();
  stacked_local_ref_cookies.pop_back();
}

Offset JNIEnvExt::SegmentStateOffset() {
  return Offset(OFFSETOF_MEMBER(JNIEnvExt, locals) +
                IndirectReferenceTable::SegmentStateOffset().Int32Value());
}

// JNI Invocation interface.

extern "C" jint JNI_CreateJavaVM(JavaVM** p_vm, JNIEnv** p_env, void* vm_args) {
  const JavaVMInitArgs* args = static_cast<JavaVMInitArgs*>(vm_args);
  if (IsBadJniVersion(args->version)) {
    LOG(ERROR) << "Bad JNI version passed to CreateJavaVM: " << args->version;
    return JNI_EVERSION;
  }
  RuntimeOptions options;
  for (int i = 0; i < args->nOptions; ++i) {
    JavaVMOption* option = &args->options[i];
    options.push_back(std::make_pair(std::string(option->optionString), option->extraInfo));
  }
  bool ignore_unrecognized = args->ignoreUnrecognized;
  if (!Runtime::Create(options, ignore_unrecognized)) {
    return JNI_ERR;
  }
  Runtime* runtime = Runtime::Current();
  bool started = runtime->Start();
  if (!started) {
    delete Thread::Current()->GetJniEnv();
    delete runtime->GetJavaVM();
    LOG(WARNING) << "CreateJavaVM failed";
    return JNI_ERR;
  }
  *p_env = Thread::Current()->GetJniEnv();
  *p_vm = runtime->GetJavaVM();
  return JNI_OK;
}

extern "C" jint JNI_GetCreatedJavaVMs(JavaVM** vms, jsize, jsize* vm_count) {
  Runtime* runtime = Runtime::Current();
  if (runtime == nullptr) {
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
    if (vm == nullptr) {
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
    if (vm == nullptr || Thread::Current() == nullptr) {
      return JNI_ERR;
    }
    JavaVMExt* raw_vm = reinterpret_cast<JavaVMExt*>(vm);
    Runtime* runtime = raw_vm->runtime;
    runtime->DetachCurrentThread();
    return JNI_OK;
  }

  static jint GetEnv(JavaVM* vm, void** env, jint version) {
    // GetEnv always returns a JNIEnv* for the most current supported JNI version,
    // and unlike other calls that take a JNI version doesn't care if you supply
    // JNI_VERSION_1_1, which we don't otherwise support.
    if (IsBadJniVersion(version) && version != JNI_VERSION_1_1) {
      LOG(ERROR) << "Bad JNI version passed to GetEnv: " << version;
      return JNI_EVERSION;
    }
    if (vm == nullptr || env == nullptr) {
      return JNI_ERR;
    }
    Thread* thread = Thread::Current();
    if (thread == nullptr) {
      *env = nullptr;
      return JNI_EDETACHED;
    }
    *env = thread->GetJniEnv();
    return JNI_OK;
  }
};

const JNIInvokeInterface gJniInvokeInterface = {
  nullptr,  // reserved0
  nullptr,  // reserved1
  nullptr,  // reserved2
  JII::DestroyJavaVM,
  JII::AttachCurrentThread,
  JII::DetachCurrentThread,
  JII::GetEnv,
  JII::AttachCurrentThreadAsDaemon
};

JavaVMExt::JavaVMExt(Runtime* runtime, ParsedOptions* options)
    : runtime(runtime),
      check_jni_abort_hook(nullptr),
      check_jni_abort_hook_data(nullptr),
      check_jni(false),
      force_copy(false),  // TODO: add a way to enable this
      trace(options->jni_trace_),
      globals_lock("JNI global reference table lock"),
      globals(gGlobalsInitial, gGlobalsMax, kGlobal),
      libraries_lock("JNI shared libraries map lock", kLoadLibraryLock),
      libraries(new Libraries),
      weak_globals_lock_("JNI weak global reference table lock"),
      weak_globals_(kWeakGlobalsInitial, kWeakGlobalsMax, kWeakGlobal),
      allow_new_weak_globals_(true),
      weak_globals_add_condition_("weak globals add condition", weak_globals_lock_) {
  functions = unchecked_functions = &gJniInvokeInterface;
  if (options->check_jni_) {
    SetCheckJniEnabled(true);
  }
}

JavaVMExt::~JavaVMExt() {
  delete libraries;
}

jweak JavaVMExt::AddWeakGlobalReference(Thread* self, mirror::Object* obj) {
  if (obj == nullptr) {
    return nullptr;
  }
  MutexLock mu(self, weak_globals_lock_);
  while (UNLIKELY(!allow_new_weak_globals_)) {
    weak_globals_add_condition_.WaitHoldingLocks(self);
  }
  IndirectRef ref = weak_globals_.Add(IRT_FIRST_SEGMENT, obj);
  return reinterpret_cast<jweak>(ref);
}

void JavaVMExt::DeleteWeakGlobalRef(Thread* self, jweak obj) {
  MutexLock mu(self, weak_globals_lock_);
  if (!weak_globals_.Remove(IRT_FIRST_SEGMENT, obj)) {
    LOG(WARNING) << "JNI WARNING: DeleteWeakGlobalRef(" << obj << ") "
                 << "failed to find entry";
  }
}

void JavaVMExt::SetCheckJniEnabled(bool enabled) {
  check_jni = enabled;
  functions = enabled ? GetCheckJniInvokeInterface() : &gJniInvokeInterface;
}

void JavaVMExt::DumpForSigQuit(std::ostream& os) {
  os << "JNI: CheckJNI is " << (check_jni ? "on" : "off");
  if (force_copy) {
    os << " (with forcecopy)";
  }
  Thread* self = Thread::Current();
  {
    ReaderMutexLock mu(self, globals_lock);
    os << "; globals=" << globals.Capacity();
  }
  {
    MutexLock mu(self, weak_globals_lock_);
    if (weak_globals_.Capacity() > 0) {
      os << " (plus " << weak_globals_.Capacity() << " weak)";
    }
  }
  os << '\n';

  {
    MutexLock mu(self, libraries_lock);
    os << "Libraries: " << Dumpable<Libraries>(*libraries) << " (" << libraries->size() << ")\n";
  }
}

void JavaVMExt::DisallowNewWeakGlobals() {
  MutexLock mu(Thread::Current(), weak_globals_lock_);
  allow_new_weak_globals_ = false;
}

void JavaVMExt::AllowNewWeakGlobals() {
  Thread* self = Thread::Current();
  MutexLock mu(self, weak_globals_lock_);
  allow_new_weak_globals_ = true;
  weak_globals_add_condition_.Broadcast(self);
}

mirror::Object* JavaVMExt::DecodeWeakGlobal(Thread* self, IndirectRef ref) {
  MutexLock mu(self, weak_globals_lock_);
  while (UNLIKELY(!allow_new_weak_globals_)) {
    weak_globals_add_condition_.WaitHoldingLocks(self);
  }
  return weak_globals_.Get(ref);
}

void JavaVMExt::DumpReferenceTables(std::ostream& os) {
  Thread* self = Thread::Current();
  {
    ReaderMutexLock mu(self, globals_lock);
    globals.Dump(os);
  }
  {
    MutexLock mu(self, weak_globals_lock_);
    weak_globals_.Dump(os);
  }
}

bool JavaVMExt::LoadNativeLibrary(const std::string& path,
                                  Handle<mirror::ClassLoader> class_loader,
                                  std::string* detail) {
  detail->clear();

  // See if we've already loaded this library.  If we have, and the class loader
  // matches, return successfully without doing anything.
  // TODO: for better results we should canonicalize the pathname (or even compare
  // inodes). This implementation is fine if everybody is using System.loadLibrary.
  SharedLibrary* library;
  Thread* self = Thread::Current();
  {
    // TODO: move the locking (and more of this logic) into Libraries.
    MutexLock mu(self, libraries_lock);
    library = libraries->Get(path);
  }
  if (library != nullptr) {
    if (library->GetClassLoader() != class_loader.Get()) {
      // The library will be associated with class_loader. The JNI
      // spec says we can't load the same library into more than one
      // class loader.
      StringAppendF(detail, "Shared library \"%s\" already opened by "
          "ClassLoader %p; can't open in ClassLoader %p",
          path.c_str(), library->GetClassLoader(), class_loader.Get());
      LOG(WARNING) << detail;
      return false;
    }
    VLOG(jni) << "[Shared library \"" << path << "\" already loaded in "
              << "ClassLoader " << class_loader.Get() << "]";
    if (!library->CheckOnLoadResult()) {
      StringAppendF(detail, "JNI_OnLoad failed on a previous attempt "
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

  // Below we dlopen but there is no paired dlclose, this would be necessary if we supported
  // class unloading. Libraries will only be unloaded when the reference count (incremented by
  // dlopen) becomes zero from dlclose.

  // This can execute slowly for a large library on a busy system, so we
  // want to switch from kRunnable while it executes.  This allows the GC to ignore us.
  self->TransitionFromRunnableToSuspended(kWaitingForJniOnLoad);
  const char* path_str = path.empty() ? nullptr : path.c_str();
  void* handle = dlopen(path_str, RTLD_LAZY);
  bool needs_native_bridge = false;
  if (handle == nullptr) {
    if (android::NativeBridgeIsSupported(path_str)) {
      handle = android::NativeBridgeLoadLibrary(path_str, RTLD_LAZY);
      needs_native_bridge = true;
    }
  }
  self->TransitionFromSuspendedToRunnable();

  VLOG(jni) << "[Call to dlopen(\"" << path << "\", RTLD_LAZY) returned " << handle << "]";

  if (handle == nullptr) {
    *detail = dlerror();
    LOG(ERROR) << "dlopen(\"" << path << "\", RTLD_LAZY) failed: " << *detail;
    return false;
  }

  // Create a new entry.
  // TODO: move the locking (and more of this logic) into Libraries.
  bool created_library = false;
  {
    MutexLock mu(self, libraries_lock);
    library = libraries->Get(path);
    if (library == nullptr) {  // We won race to get libraries_lock
      library = new SharedLibrary(path, handle, class_loader.Get());
      libraries->Put(path, library);
      created_library = true;
    }
  }
  if (!created_library) {
    LOG(INFO) << "WOW: we lost a race to add shared library: "
        << "\"" << path << "\" ClassLoader=" << class_loader.Get();
    return library->CheckOnLoadResult();
  }

  VLOG(jni) << "[Added shared library \"" << path << "\" for ClassLoader " << class_loader.Get()
      << "]";

  bool was_successful = false;
  void* sym = nullptr;
  if (UNLIKELY(needs_native_bridge)) {
    library->SetNeedsNativeBridge();
    sym = library->FindSymbolWithNativeBridge("JNI_OnLoad", nullptr);
  } else {
    sym = dlsym(handle, "JNI_OnLoad");
  }

  if (sym == nullptr) {
    VLOG(jni) << "[No JNI_OnLoad found in \"" << path << "\"]";
    was_successful = true;
  } else {
    // Call JNI_OnLoad.  We have to override the current class
    // loader, which will always be "null" since the stuff at the
    // top of the stack is around Runtime.loadLibrary().  (See
    // the comments in the JNI FindClass function.)
    typedef int (*JNI_OnLoadFn)(JavaVM*, void*);
    JNI_OnLoadFn jni_on_load = reinterpret_cast<JNI_OnLoadFn>(sym);
    StackHandleScope<1> hs(self);
    Handle<mirror::ClassLoader> old_class_loader(hs.NewHandle(self->GetClassLoaderOverride()));
    self->SetClassLoaderOverride(class_loader.Get());

    int version = 0;
    {
      ScopedThreadStateChange tsc(self, kNative);
      VLOG(jni) << "[Calling JNI_OnLoad in \"" << path << "\"]";
      version = (*jni_on_load)(this, nullptr);
    }

    self->SetClassLoaderOverride(old_class_loader.Get());

    if (version == JNI_ERR) {
      StringAppendF(detail, "JNI_ERR returned from JNI_OnLoad in \"%s\"", path.c_str());
    } else if (IsBadJniVersion(version)) {
      StringAppendF(detail, "Bad JNI version returned from JNI_OnLoad in \"%s\": %d",
                    path.c_str(), version);
      // It's unwise to call dlclose() here, but we can mark it
      // as bad and ensure that future load attempts will fail.
      // We don't know how far JNI_OnLoad got, so there could
      // be some partially-initialized stuff accessible through
      // newly-registered native method calls.  We could try to
      // unregister them, but that doesn't seem worthwhile.
    } else {
      was_successful = true;
    }
    VLOG(jni) << "[Returned " << (was_successful ? "successfully" : "failure")
              << " from JNI_OnLoad in \"" << path << "\"]";
  }

  library->SetResult(was_successful);
  return was_successful;
}

void* JavaVMExt::FindCodeForNativeMethod(mirror::ArtMethod* m) {
  CHECK(m->IsNative());
  mirror::Class* c = m->GetDeclaringClass();
  // If this is a static method, it could be called before the class has been initialized.
  if (m->IsStatic()) {
    c = EnsureInitialized(Thread::Current(), c);
    if (c == nullptr) {
      return nullptr;
    }
  } else {
    CHECK(c->IsInitializing()) << c->GetStatus() << " " << PrettyMethod(m);
  }
  std::string detail;
  void* native_method;
  Thread* self = Thread::Current();
  {
    MutexLock mu(self, libraries_lock);
    native_method = libraries->FindNativeMethod(m, detail);
  }
  // Throwing can cause libraries_lock to be reacquired.
  if (native_method == nullptr) {
    ThrowLocation throw_location = self->GetCurrentLocationForThrow();
    self->ThrowNewException(throw_location, "Ljava/lang/UnsatisfiedLinkError;", detail.c_str());
  }
  return native_method;
}

void JavaVMExt::SweepJniWeakGlobals(IsMarkedCallback* callback, void* arg) {
  MutexLock mu(Thread::Current(), weak_globals_lock_);
  for (mirror::Object** entry : weak_globals_) {
    // Since this is called by the GC, we don't need a read barrier.
    mirror::Object* obj = *entry;
    mirror::Object* new_obj = callback(obj, arg);
    if (new_obj == nullptr) {
      new_obj = kClearedJniWeakGlobal;
    }
    *entry = new_obj;
  }
}

void JavaVMExt::VisitRoots(RootCallback* callback, void* arg) {
  Thread* self = Thread::Current();
  {
    ReaderMutexLock mu(self, globals_lock);
    globals.VisitRoots(callback, arg, 0, kRootJNIGlobal);
  }
  {
    MutexLock mu(self, libraries_lock);
    // Libraries contains shared libraries which hold a pointer to a class loader.
    libraries->VisitRoots(callback, arg);
  }
  // The weak_globals table is visited by the GC itself (because it mutates the table).
}

void RegisterNativeMethods(JNIEnv* env, const char* jni_class_name, const JNINativeMethod* methods,
                           jint method_count) {
  ScopedLocalRef<jclass> c(env, env->FindClass(jni_class_name));
  if (c.get() == nullptr) {
    LOG(FATAL) << "Couldn't find class: " << jni_class_name;
  }
  JNI::RegisterNativeMethods(env, c.get(), methods, method_count, false);
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
