// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_JNI_INTERNAL_H_
#define ART_SRC_JNI_INTERNAL_H_

#include "jni.h"

#include "heap.h"
#include "indirect_reference_table.h"
#include "macros.h"
#include "mutex.h"
#include "reference_table.h"
#include "runtime.h"

#include <string>

namespace art {

class ClassLoader;
class Field;
union JValue;
class Libraries;
class Method;
class Thread;

void JniAbort(const char* jni_function_name);
void* FindNativeMethod(Thread* thread);

template<typename T> T Decode(JNIEnv*, jobject);
template<typename T> T AddLocalReference(JNIEnv*, const Object*);

inline Field* DecodeField(jfieldID fid) {
#ifdef MOVING_GARBAGE_COLLECTOR
  // TODO: we should make these unique weak globals if Field instances can ever move.
  UNIMPLEMENTED(WARNING);
#endif
  return reinterpret_cast<Field*>(fid);
}

inline jfieldID EncodeField(Field* field) {
#ifdef MOVING_GARBAGE_COLLECTOR
  UNIMPLEMENTED(WARNING);
#endif
  return reinterpret_cast<jfieldID>(field);
}

inline Method* DecodeMethod(jmethodID mid) {
#ifdef MOVING_GARBAGE_COLLECTOR
  // TODO: we should make these unique weak globals if Method instances can ever move.
  UNIMPLEMENTED(WARNING);
#endif
  return reinterpret_cast<Method*>(mid);
}

inline jmethodID EncodeMethod(Method* method) {
#ifdef MOVING_GARBAGE_COLLECTOR
  UNIMPLEMENTED(WARNING);
#endif
  return reinterpret_cast<jmethodID>(method);
}

JValue InvokeWithJValues(JNIEnv* env, jobject obj, jmethodID mid, jvalue* args);

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

  void DumpReferenceTables();

  void VisitRoots(Heap::RootVisitor*, void*);

  Runtime* runtime;

  // Used for testing. By default, we'll LOG(FATAL) the reason.
  void (*check_jni_abort_hook)(const std::string& reason);

  // Extra checking.
  bool check_jni;
  bool force_copy;

  // Extra diagnostics.
  bool verbose_jni;
  bool log_third_party_jni;
  std::string trace;

  // Used to provide compatibility for apps that assumed direct references.
  bool work_around_app_jni_bugs;

  // Used to hold references to pinned primitive arrays.
  Mutex pins_lock;
  ReferenceTable pin_table;

  // JNI global references.
  Mutex globals_lock;
  IndirectReferenceTable globals;

  // JNI weak global references.
  Mutex weak_globals_lock;
  IndirectReferenceTable weak_globals;

  Mutex libraries_lock;
  Libraries* libraries;

  // Used by -Xcheck:jni.
  const JNIInvokeInterface* unchecked_functions;
};

struct JNIEnvExt : public JNIEnv {
  JNIEnvExt(Thread* self, JavaVMExt* vm);
  ~JNIEnvExt();

  void DumpReferenceTables();

  static Offset SegmentStateOffset() {
    return Offset(OFFSETOF_MEMBER(JNIEnvExt, locals) +
                  IndirectReferenceTable::SegmentStateOffset().Int32Value());
  }

  static Offset LocalRefCookieOffset() {
    return Offset(OFFSETOF_MEMBER(JNIEnvExt, local_ref_cookie));
  }

  Thread* const self;
  JavaVMExt* vm;

  // Cookie used when using the local indirect reference table
  uint32_t local_ref_cookie;

  // JNI local references.
  IndirectReferenceTable locals;

  // Frequently-accessed fields cached from JavaVM.
  bool check_jni;
  bool work_around_app_jni_bugs;

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
  ScopedJniEnvLocalRefState(JNIEnvExt* env) : env_(env) {
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
