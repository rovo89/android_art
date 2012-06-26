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

#ifndef ART_SRC_JNI_INTERNAL_H_
#define ART_SRC_JNI_INTERNAL_H_

#include "jni.h"

#include "heap.h"
#include "indirect_reference_table.h"
#include "macros.h"
#include "mutex.h"
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

class ClassLoader;
class Field;
union JValue;
class Libraries;
class Method;
class ScopedJniThreadState;
class Thread;

void SetJniGlobalsMax(size_t max);
void JniAbortF(const char* jni_function_name, const char* fmt, ...);
void* FindNativeMethod(Thread* thread);
void RegisterNativeMethods(JNIEnv* env, const char* jni_class_name, const JNINativeMethod* methods, size_t method_count);

size_t NumArgArrayBytes(const char* shorty, uint32_t shorty_len);
JValue InvokeWithJValues(const ScopedJniThreadState&, jobject obj, jmethodID mid, jvalue* args);
JValue InvokeWithJValues(const ScopedJniThreadState&, Object* receiver, Method* m, JValue* args);

int ThrowNewException(JNIEnv* env, jclass exception_class, const char* msg, jobject cause);

struct JavaVMExt : public JavaVM {
  JavaVMExt(Runtime* runtime, Runtime::ParsedOptions* options);
  ~JavaVMExt();

  /**
   * Loads the given shared library. 'path' is an absolute pathname.
   *
   * Returns 'true' on success. On failure, sets 'detail' to a
   * human-readable description of the error.
   */
  bool LoadNativeLibrary(const std::string& path, ClassLoader* class_loader, std::string& detail);

  /**
   * Returns a pointer to the code for the native method 'm', found
   * using dlsym(3) on every native library that's been loaded so far.
   */
  void* FindCodeForNativeMethod(Method* m);

  void DumpForSigQuit(std::ostream& os);

  void DumpReferenceTables(std::ostream& os);

  void SetCheckJniEnabled(bool enabled);

  void VisitRoots(Heap::RootVisitor*, void*);

  Runtime* runtime;

  // Used for testing. By default, we'll LOG(FATAL) the reason.
  void (*check_jni_abort_hook)(void* data, const std::string& reason);
  void* check_jni_abort_hook_data;

  // Extra checking.
  bool check_jni;
  bool force_copy;

  // Extra diagnostics.
  std::string trace;

  // Used to provide compatibility for apps that assumed direct references.
  bool work_around_app_jni_bugs;

  // Used to hold references to pinned primitive arrays.
  Mutex pins_lock;
  ReferenceTable pin_table GUARDED_BY(pins_lock);

  // JNI global references.
  Mutex globals_lock;
  IndirectReferenceTable globals GUARDED_BY(globals_lock);

  // JNI weak global references.
  Mutex weak_globals_lock;
  IndirectReferenceTable weak_globals GUARDED_BY(weak_globals_lock);

  Mutex libraries_lock;
  Libraries* libraries GUARDED_BY(libraries_lock);

  // Used by -Xcheck:jni.
  const JNIInvokeInterface* unchecked_functions;
};

struct JNIEnvExt : public JNIEnv {
  JNIEnvExt(Thread* self, JavaVMExt* vm);
  ~JNIEnvExt();

  void DumpReferenceTables(std::ostream& os);

  void SetCheckJniEnabled(bool enabled);

  void PushFrame(int capacity);
  void PopFrame();

  static Offset SegmentStateOffset() {
    return Offset(OFFSETOF_MEMBER(JNIEnvExt, locals) +
                  IndirectReferenceTable::SegmentStateOffset().Int32Value());
  }

  static Offset LocalRefCookieOffset() {
    return Offset(OFFSETOF_MEMBER(JNIEnvExt, local_ref_cookie));
  }

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

}  // namespace art

std::ostream& operator<<(std::ostream& os, const jobjectRefType& rhs);

#endif  // ART_SRC_JNI_INTERNAL_H_
