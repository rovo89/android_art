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

#include "base/dumpable.h"
#include "base/mutex.h"
#include "base/stl_util.h"
#include "check_jni.h"
#include "fault_handler.h"
#include "indirect_reference_table-inl.h"
#include "mirror/art_method.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "nativebridge/native_bridge.h"
#include "java_vm_ext.h"
#include "parsed_options.h"
#include "runtime-inl.h"
#include "ScopedLocalRef.h"
#include "scoped_thread_state_change.h"
#include "thread-inl.h"
#include "thread_list.h"

namespace art {

static size_t gGlobalsInitial = 512;  // Arbitrary.
static size_t gGlobalsMax = 51200;  // Arbitrary sanity check. (Must fit in 16 bits.)

static const size_t kWeakGlobalsInitial = 16;  // Arbitrary.
static const size_t kWeakGlobalsMax = 51200;  // Arbitrary sanity check. (Must fit in 16 bits.)

static bool IsBadJniVersion(int version) {
  // We don't support JNI_VERSION_1_1. These are the only other valid versions.
  return version != JNI_VERSION_1_2 && version != JNI_VERSION_1_4 && version != JNI_VERSION_1_6;
}

class SharedLibrary {
 public:
  SharedLibrary(JNIEnv* env, Thread* self, const std::string& path, void* handle,
                jobject class_loader)
      : path_(path),
        handle_(handle),
        needs_native_bridge_(false),
        class_loader_(env->NewGlobalRef(class_loader)),
        jni_on_load_lock_("JNI_OnLoad lock"),
        jni_on_load_cond_("JNI_OnLoad condition variable", jni_on_load_lock_),
        jni_on_load_thread_id_(self->GetThreadId()),
        jni_on_load_result_(kPending) {
  }

  ~SharedLibrary() {
    Thread* self = Thread::Current();
    if (self != nullptr) {
      self->GetJniEnv()->DeleteGlobalRef(class_loader_);
    }
  }

  jobject GetClassLoader() const {
    return class_loader_;
  }

  const std::string& GetPath() const {
    return path_;
  }

  /*
   * Check the result of an earlier call to JNI_OnLoad on this library.
   * If the call has not yet finished in another thread, wait for it.
   */
  bool CheckOnLoadResult()
      LOCKS_EXCLUDED(jni_on_load_lock_) {
    Thread* self = Thread::Current();
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

  void* FindSymbolWithNativeBridge(const std::string& symbol_name, const char* shorty) {
    CHECK(NeedsNativeBridge());

    uint32_t len = 0;
    return android::NativeBridgeGetTrampoline(handle_, symbol_name.c_str(), shorty, len);
  }

 private:
  enum JNI_OnLoadState {
    kPending,
    kFailed,
    kOkay,
  };

  // Path to library "/system/lib/libjni.so".
  const std::string path_;

  // The void* returned by dlopen(3).
  void* const handle_;

  // True if a native bridge is required.
  bool needs_native_bridge_;

  // The ClassLoader this library is associated with, a global JNI reference that is
  // created/deleted with the scope of the library.
  const jobject class_loader_;

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
      EXCLUSIVE_LOCKS_REQUIRED(Locks::jni_libraries_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    std::string jni_short_name(JniShortName(m));
    std::string jni_long_name(JniLongName(m));
    const mirror::ClassLoader* declaring_class_loader = m->GetDeclaringClass()->GetClassLoader();
    ScopedObjectAccessUnchecked soa(Thread::Current());
    for (const auto& lib : libraries_) {
      SharedLibrary* library = lib.second;
      if (soa.Decode<mirror::ClassLoader*>(library->GetClassLoader()) != declaring_class_loader) {
        // We only search libraries loaded by the appropriate ClassLoader.
        continue;
      }
      // Try the short name then the long name...
      void* fn;
      if (library->NeedsNativeBridge()) {
        const char* shorty = m->GetShorty();
        fn = library->FindSymbolWithNativeBridge(jni_short_name, shorty);
        if (fn == nullptr) {
          fn = library->FindSymbolWithNativeBridge(jni_long_name, shorty);
        }
      } else {
        fn = library->FindSymbol(jni_short_name);
        if (fn == nullptr) {
          fn = library->FindSymbol(jni_long_name);
        }
      }
      if (fn == nullptr) {
        fn = library->FindSymbol(jni_long_name);
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

 private:
  AllocationTrackingSafeMap<std::string, SharedLibrary*, kAllocatorTagJNILibrarires> libraries_;
};


class JII {
 public:
  static jint DestroyJavaVM(JavaVM* vm) {
    if (vm == nullptr) {
      return JNI_ERR;
    }
    JavaVMExt* raw_vm = reinterpret_cast<JavaVMExt*>(vm);
    delete raw_vm->GetRuntime();
    return JNI_OK;
  }

  static jint AttachCurrentThread(JavaVM* vm, JNIEnv** p_env, void* thr_args) {
    return AttachCurrentThreadInternal(vm, p_env, thr_args, false);
  }

  static jint AttachCurrentThreadAsDaemon(JavaVM* vm, JNIEnv** p_env, void* thr_args) {
    return AttachCurrentThreadInternal(vm, p_env, thr_args, true);
  }

  static jint DetachCurrentThread(JavaVM* vm) {
    if (vm == nullptr || Thread::Current() == nullptr) {
      return JNI_ERR;
    }
    JavaVMExt* raw_vm = reinterpret_cast<JavaVMExt*>(vm);
    Runtime* runtime = raw_vm->GetRuntime();
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

 private:
  static jint AttachCurrentThreadInternal(JavaVM* vm, JNIEnv** p_env, void* raw_args, bool as_daemon) {
    if (vm == nullptr || p_env == nullptr) {
      return JNI_ERR;
    }

    // Return immediately if we're already attached.
    Thread* self = Thread::Current();
    if (self != nullptr) {
      *p_env = self->GetJniEnv();
      return JNI_OK;
    }

    Runtime* runtime = reinterpret_cast<JavaVMExt*>(vm)->GetRuntime();

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
    : runtime_(runtime),
      check_jni_abort_hook_(nullptr),
      check_jni_abort_hook_data_(nullptr),
      check_jni_(false),  // Initialized properly in the constructor body below.
      force_copy_(options->force_copy_),
      tracing_enabled_(!options->jni_trace_.empty() || VLOG_IS_ON(third_party_jni)),
      trace_(options->jni_trace_),
      globals_lock_("JNI global reference table lock"),
      globals_(gGlobalsInitial, gGlobalsMax, kGlobal),
      libraries_(new Libraries),
      unchecked_functions_(&gJniInvokeInterface),
      weak_globals_lock_("JNI weak global reference table lock"),
      weak_globals_(kWeakGlobalsInitial, kWeakGlobalsMax, kWeakGlobal),
      allow_new_weak_globals_(true),
      weak_globals_add_condition_("weak globals add condition", weak_globals_lock_) {
  functions = unchecked_functions_;
  if (options->check_jni_) {
    SetCheckJniEnabled(true);
  }
}

JavaVMExt::~JavaVMExt() {
}

void JavaVMExt::JniAbort(const char* jni_function_name, const char* msg) {
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);
  mirror::ArtMethod* current_method = self->GetCurrentMethod(nullptr);

  std::ostringstream os;
  os << "JNI DETECTED ERROR IN APPLICATION: " << msg;

  if (jni_function_name != nullptr) {
    os << "\n    in call to " << jni_function_name;
  }
  // TODO: is this useful given that we're about to dump the calling thread's stack?
  if (current_method != nullptr) {
    os << "\n    from " << PrettyMethod(current_method);
  }
  os << "\n";
  self->Dump(os);

  if (check_jni_abort_hook_ != nullptr) {
    check_jni_abort_hook_(check_jni_abort_hook_data_, os.str());
  } else {
    // Ensure that we get a native stack trace for this thread.
    self->TransitionFromRunnableToSuspended(kNative);
    LOG(FATAL) << os.str();
    self->TransitionFromSuspendedToRunnable();  // Unreachable, keep annotalysis happy.
  }
}

void JavaVMExt::JniAbortV(const char* jni_function_name, const char* fmt, va_list ap) {
  std::string msg;
  StringAppendV(&msg, fmt, ap);
  JniAbort(jni_function_name, msg.c_str());
}

void JavaVMExt::JniAbortF(const char* jni_function_name, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  JniAbortV(jni_function_name, fmt, args);
  va_end(args);
}

bool JavaVMExt::ShouldTrace(mirror::ArtMethod* method) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  // Fast where no tracing is enabled.
  if (trace_.empty() && !VLOG_IS_ON(third_party_jni)) {
    return false;
  }
  // Perform checks based on class name.
  StringPiece class_name(method->GetDeclaringClassDescriptor());
  if (!trace_.empty() && class_name.find(trace_) != std::string::npos) {
    return true;
  }
  if (!VLOG_IS_ON(third_party_jni)) {
    return false;
  }
  // Return true if we're trying to log all third-party JNI activity and 'method' doesn't look
  // like part of Android.
  static const char* gBuiltInPrefixes[] = {
      "Landroid/",
      "Lcom/android/",
      "Lcom/google/android/",
      "Ldalvik/",
      "Ljava/",
      "Ljavax/",
      "Llibcore/",
      "Lorg/apache/harmony/",
  };
  for (size_t i = 0; i < arraysize(gBuiltInPrefixes); ++i) {
    if (class_name.starts_with(gBuiltInPrefixes[i])) {
      return false;
    }
  }
  return true;
}

jobject JavaVMExt::AddGlobalRef(Thread* self, mirror::Object* obj) {
  // Check for null after decoding the object to handle cleared weak globals.
  if (obj == nullptr) {
    return nullptr;
  }
  WriterMutexLock mu(self, globals_lock_);
  IndirectRef ref = globals_.Add(IRT_FIRST_SEGMENT, obj);
  return reinterpret_cast<jobject>(ref);
}

jweak JavaVMExt::AddWeakGlobalRef(Thread* self, mirror::Object* obj) {
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

void JavaVMExt::DeleteGlobalRef(Thread* self, jobject obj) {
  if (obj == nullptr) {
    return;
  }
  WriterMutexLock mu(self, globals_lock_);
  if (!globals_.Remove(IRT_FIRST_SEGMENT, obj)) {
    LOG(WARNING) << "JNI WARNING: DeleteGlobalRef(" << obj << ") "
                 << "failed to find entry";
  }
}

void JavaVMExt::DeleteWeakGlobalRef(Thread* self, jweak obj) {
  if (obj == nullptr) {
    return;
  }
  MutexLock mu(self, weak_globals_lock_);
  if (!weak_globals_.Remove(IRT_FIRST_SEGMENT, obj)) {
    LOG(WARNING) << "JNI WARNING: DeleteWeakGlobalRef(" << obj << ") "
                 << "failed to find entry";
  }
}

static void ThreadEnableCheckJni(Thread* thread, void* arg) {
  bool* check_jni = reinterpret_cast<bool*>(arg);
  thread->GetJniEnv()->SetCheckJniEnabled(*check_jni);
}

bool JavaVMExt::SetCheckJniEnabled(bool enabled) {
  bool old_check_jni = check_jni_;
  check_jni_ = enabled;
  functions = enabled ? GetCheckJniInvokeInterface() : unchecked_functions_;
  MutexLock mu(Thread::Current(), *Locks::thread_list_lock_);
  runtime_->GetThreadList()->ForEach(ThreadEnableCheckJni, &check_jni_);
  return old_check_jni;
}

void JavaVMExt::DumpForSigQuit(std::ostream& os) {
  os << "JNI: CheckJNI is " << (check_jni_ ? "on" : "off");
  if (force_copy_) {
    os << " (with forcecopy)";
  }
  Thread* self = Thread::Current();
  {
    ReaderMutexLock mu(self, globals_lock_);
    os << "; globals=" << globals_.Capacity();
  }
  {
    MutexLock mu(self, weak_globals_lock_);
    if (weak_globals_.Capacity() > 0) {
      os << " (plus " << weak_globals_.Capacity() << " weak)";
    }
  }
  os << '\n';

  {
    MutexLock mu(self, *Locks::jni_libraries_lock_);
    os << "Libraries: " << Dumpable<Libraries>(*libraries_) << " (" << libraries_->size() << ")\n";
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

mirror::Object* JavaVMExt::DecodeGlobal(Thread* self, IndirectRef ref) {
  return globals_.SynchronizedGet(self, &globals_lock_, ref);
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
    ReaderMutexLock mu(self, globals_lock_);
    globals_.Dump(os);
  }
  {
    MutexLock mu(self, weak_globals_lock_);
    weak_globals_.Dump(os);
  }
}

bool JavaVMExt::LoadNativeLibrary(JNIEnv* env, const std::string& path, jobject class_loader,
                                  std::string* error_msg) {
  error_msg->clear();

  // See if we've already loaded this library.  If we have, and the class loader
  // matches, return successfully without doing anything.
  // TODO: for better results we should canonicalize the pathname (or even compare
  // inodes). This implementation is fine if everybody is using System.loadLibrary.
  SharedLibrary* library;
  Thread* self = Thread::Current();
  {
    // TODO: move the locking (and more of this logic) into Libraries.
    MutexLock mu(self, *Locks::jni_libraries_lock_);
    library = libraries_->Get(path);
  }
  if (library != nullptr) {
    if (env->IsSameObject(library->GetClassLoader(), class_loader) == JNI_FALSE) {
      // The library will be associated with class_loader. The JNI
      // spec says we can't load the same library into more than one
      // class loader.
      StringAppendF(error_msg, "Shared library \"%s\" already opened by "
          "ClassLoader %p; can't open in ClassLoader %p",
          path.c_str(), library->GetClassLoader(), class_loader);
      LOG(WARNING) << error_msg;
      return false;
    }
    VLOG(jni) << "[Shared library \"" << path << "\" already loaded in "
              << " ClassLoader " << class_loader << "]";
    if (!library->CheckOnLoadResult()) {
      StringAppendF(error_msg, "JNI_OnLoad failed on a previous attempt "
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

  Locks::mutator_lock_->AssertNotHeld(self);
  const char* path_str = path.empty() ? nullptr : path.c_str();
  void* handle = dlopen(path_str, RTLD_LAZY);
  bool needs_native_bridge = false;
  if (handle == nullptr) {
    if (android::NativeBridgeIsSupported(path_str)) {
      handle = android::NativeBridgeLoadLibrary(path_str, RTLD_LAZY);
      needs_native_bridge = true;
    }
  }

  VLOG(jni) << "[Call to dlopen(\"" << path << "\", RTLD_LAZY) returned " << handle << "]";

  if (handle == nullptr) {
    *error_msg = dlerror();
    LOG(ERROR) << "dlopen(\"" << path << "\", RTLD_LAZY) failed: " << *error_msg;
    return false;
  }

  if (env->ExceptionCheck() == JNI_TRUE) {
    LOG(ERROR) << "Unexpected exception:";
    env->ExceptionDescribe();
    env->ExceptionClear();
  }
  // Create a new entry.
  // TODO: move the locking (and more of this logic) into Libraries.
  bool created_library = false;
  {
    // Create SharedLibrary ahead of taking the libraries lock to maintain lock ordering.
    std::unique_ptr<SharedLibrary> new_library(
        new SharedLibrary(env, self, path, handle, class_loader));
    MutexLock mu(self, *Locks::jni_libraries_lock_);
    library = libraries_->Get(path);
    if (library == nullptr) {  // We won race to get libraries_lock.
      library = new_library.release();
      libraries_->Put(path, library);
      created_library = true;
    }
  }
  if (!created_library) {
    LOG(INFO) << "WOW: we lost a race to add shared library: "
        << "\"" << path << "\" ClassLoader=" << class_loader;
    return library->CheckOnLoadResult();
  }
  VLOG(jni) << "[Added shared library \"" << path << "\" for ClassLoader " << class_loader << "]";

  bool was_successful = false;
  void* sym;
  if (needs_native_bridge) {
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
    ScopedLocalRef<jobject> old_class_loader(env, env->NewLocalRef(self->GetClassLoaderOverride()));
    self->SetClassLoaderOverride(class_loader);

    VLOG(jni) << "[Calling JNI_OnLoad in \"" << path << "\"]";
    typedef int (*JNI_OnLoadFn)(JavaVM*, void*);
    JNI_OnLoadFn jni_on_load = reinterpret_cast<JNI_OnLoadFn>(sym);
    int version = (*jni_on_load)(this, nullptr);

    if (runtime_->GetTargetSdkVersion() != 0 && runtime_->GetTargetSdkVersion() <= 21) {
      fault_manager.EnsureArtActionInFrontOfSignalChain();
    }

    self->SetClassLoaderOverride(old_class_loader.get());

    if (version == JNI_ERR) {
      StringAppendF(error_msg, "JNI_ERR returned from JNI_OnLoad in \"%s\"", path.c_str());
    } else if (IsBadJniVersion(version)) {
      StringAppendF(error_msg, "Bad JNI version returned from JNI_OnLoad in \"%s\": %d",
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
  CHECK(c->IsInitializing()) << c->GetStatus() << " " << PrettyMethod(m);
  std::string detail;
  void* native_method;
  Thread* self = Thread::Current();
  {
    MutexLock mu(self, *Locks::jni_libraries_lock_);
    native_method = libraries_->FindNativeMethod(m, detail);
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
    if (obj == nullptr) {
      // Need to skip null here to distinguish between null entries
      // and cleared weak ref entries.
      continue;
    }
    mirror::Object* new_obj = callback(obj, arg);
    if (new_obj == nullptr) {
      new_obj = Runtime::Current()->GetClearedJniWeakGlobal();
    }
    *entry = new_obj;
  }
}

void JavaVMExt::VisitRoots(RootCallback* callback, void* arg) {
  Thread* self = Thread::Current();
  {
    ReaderMutexLock mu(self, globals_lock_);
    globals_.VisitRoots(callback, arg, 0, kRootJNIGlobal);
  }
  // The weak_globals table is visited by the GC itself (because it mutates the table).
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

}  // namespace art
