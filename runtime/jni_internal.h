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

#ifndef ART_RUNTIME_JNI_INTERNAL_H_
#define ART_RUNTIME_JNI_INTERNAL_H_

#include "jni.h"

#include "base/macros.h"
#include "base/mutex.h"
#include "indirect_reference_table.h"
#include "object_callbacks.h"
#include "reference_table.h"
#include "runtime.h"

#include <iosfwd>
#include <string>

#ifndef NATIVE_METHOD
#define NATIVE_METHOD(className, functionName, signature) \
  { #functionName, signature, reinterpret_cast<void*>(className ## _ ## functionName) }
#endif
#define REGISTER_NATIVE_METHODS(jni_class_name) \
  RegisterNativeMethods(env, jni_class_name, gMethods, arraysize(gMethods))

namespace art {
namespace mirror {
  class ArtField;
  class ArtMethod;
  class ClassLoader;
}  // namespace mirror
union JValue;
class Libraries;
class ParsedOptions;
class ScopedObjectAccess;
template<class T> class Handle;
class Thread;

void JniAbortF(const char* jni_function_name, const char* fmt, ...)
    __attribute__((__format__(__printf__, 2, 3)));
void RegisterNativeMethods(JNIEnv* env, const char* jni_class_name, const JNINativeMethod* methods,
                           jint method_count);

int ThrowNewException(JNIEnv* env, jclass exception_class, const char* msg, jobject cause);

class JavaVMExt : public JavaVM {
 public:
  JavaVMExt(Runtime* runtime, ParsedOptions* options);
  ~JavaVMExt();

  /**
   * Loads the given shared library. 'path' is an absolute pathname.
   *
   * Returns 'true' on success. On failure, sets 'detail' to a
   * human-readable description of the error.
   */
  bool LoadNativeLibrary(const std::string& path, const Handle<mirror::ClassLoader>& class_loader,
                         std::string* detail)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  /**
   * Returns a pointer to the code for the native method 'm', found
   * using dlsym(3) on every native library that's been loaded so far.
   */
  void* FindCodeForNativeMethod(mirror::ArtMethod* m)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void DumpForSigQuit(std::ostream& os);

  void DumpReferenceTables(std::ostream& os)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void SetCheckJniEnabled(bool enabled);

  void VisitRoots(RootCallback* callback, void* arg);

  void DisallowNewWeakGlobals() EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_);
  void AllowNewWeakGlobals() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  jweak AddWeakGlobalReference(Thread* self, mirror::Object* obj)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void DeleteWeakGlobalRef(Thread* self, jweak obj)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void SweepJniWeakGlobals(IsMarkedCallback* callback, void* arg);
  mirror::Object* DecodeWeakGlobal(Thread* self, IndirectRef ref)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  Runtime* runtime;

  // Used for testing. By default, we'll LOG(FATAL) the reason.
  void (*check_jni_abort_hook)(void* data, const std::string& reason);
  void* check_jni_abort_hook_data;

  // Extra checking.
  bool check_jni;
  bool force_copy;

  // Extra diagnostics.
  std::string trace;

  // Used to hold references to pinned primitive arrays.
  Mutex pins_lock DEFAULT_MUTEX_ACQUIRED_AFTER;
  ReferenceTable pin_table GUARDED_BY(pins_lock);

  // JNI global references.
  ReaderWriterMutex globals_lock DEFAULT_MUTEX_ACQUIRED_AFTER;
  // Not guarded by globals_lock since we sometimes use SynchronizedGet in Thread::DecodeJObject.
  IndirectReferenceTable globals;

  Mutex libraries_lock DEFAULT_MUTEX_ACQUIRED_AFTER;
  Libraries* libraries GUARDED_BY(libraries_lock);

  // Used by -Xcheck:jni.
  const JNIInvokeInterface* unchecked_functions;

 private:
  // TODO: Make the other members of this class also private.
  // JNI weak global references.
  Mutex weak_globals_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  IndirectReferenceTable weak_globals_ GUARDED_BY(weak_globals_lock_);
  bool allow_new_weak_globals_ GUARDED_BY(weak_globals_lock_);
  ConditionVariable weak_globals_add_condition_ GUARDED_BY(weak_globals_lock_);
};

struct JNIEnvExt : public JNIEnv {
  JNIEnvExt(Thread* self, JavaVMExt* vm);
  ~JNIEnvExt();

  void DumpReferenceTables(std::ostream& os)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void SetCheckJniEnabled(bool enabled);

  void PushFrame(int capacity);
  void PopFrame();

  template<typename T>
  T AddLocalReference(mirror::Object* obj)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static Offset SegmentStateOffset();

  static Offset LocalRefCookieOffset() {
    return Offset(OFFSETOF_MEMBER(JNIEnvExt, local_ref_cookie));
  }

  static Offset SelfOffset() {
    return Offset(OFFSETOF_MEMBER(JNIEnvExt, self));
  }

  jobject NewLocalRef(mirror::Object* obj) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void DeleteLocalRef(jobject obj) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  Thread* const self;
  JavaVMExt* vm;

  // Cookie used when using the local indirect reference table.
  uint32_t local_ref_cookie;

  // JNI local references.
  IndirectReferenceTable locals;

  // Stack of cookies corresponding to PushLocalFrame/PopLocalFrame calls.
  // TODO: to avoid leaks (and bugs), we need to clear this vector on entry (or return)
  // to a native method.
  std::vector<uint32_t> stacked_local_ref_cookies;

  // Frequently-accessed fields cached from JavaVM.
  bool check_jni;

  // How many nested "critical" JNI calls are we in?
  int critical;

  // Entered JNI monitors, for bulk exit on thread detach.
  ReferenceTable monitors;

  // Used by -Xcheck:jni.
  const JNINativeInterface* unchecked_functions;
};

const JNINativeInterface* GetCheckJniNativeInterface();
const JNIInvokeInterface* GetCheckJniInvokeInterface();

// Used to save and restore the JNIEnvExt state when not going through code created by the JNI
// compiler
class ScopedJniEnvLocalRefState {
 public:
  explicit ScopedJniEnvLocalRefState(JNIEnvExt* env) : env_(env) {
    saved_local_ref_cookie_ = env->local_ref_cookie;
    env->local_ref_cookie = env->locals.GetSegmentState();
  }

  ~ScopedJniEnvLocalRefState() {
    env_->locals.SetSegmentState(env_->local_ref_cookie);
    env_->local_ref_cookie = saved_local_ref_cookie_;
  }

 private:
  JNIEnvExt* env_;
  uint32_t saved_local_ref_cookie_;
  DISALLOW_COPY_AND_ASSIGN(ScopedJniEnvLocalRefState);
};

template<typename T>
inline T JNIEnvExt::AddLocalReference(mirror::Object* obj) {
  IndirectRef ref = locals.Add(local_ref_cookie, obj);

  // TODO: fix this to understand PushLocalFrame, so we can turn it on.
  if (false) {
    if (check_jni) {
      size_t entry_count = locals.Capacity();
      if (entry_count > 16) {
        locals.Dump(LOG(WARNING) << "Warning: more than 16 JNI local references: "
            << entry_count << " (most recent was a " << PrettyTypeOf(obj) << ")\n");
        // TODO: LOG(FATAL) in a later release?
      }
    }
  }

  return reinterpret_cast<T>(ref);
}

}  // namespace art

std::ostream& operator<<(std::ostream& os, const jobjectRefType& rhs);
#endif  // ART_RUNTIME_JNI_INTERNAL_H_
