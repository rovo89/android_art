// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_JNI_INTERNAL_H_
#define ART_SRC_JNI_INTERNAL_H_

#include "jni.h"

#include "heap.h"
#include "indirect_reference_table.h"
#include "macros.h"
#include "reference_table.h"

#include <string>

namespace art {

class ClassLoader;
class Libraries;
class Method;
class Mutex;
class Runtime;
class Thread;

void JniAbort(const char* jni_function_name);

template<typename T> T Decode(JNIEnv*, jobject);
template<typename T> T AddLocalReference(JNIEnv*, const Object*);

struct JavaVMExt : public JavaVM {
  JavaVMExt(Runtime* runtime, bool check_jni, bool verbose_jni);
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

  void VisitRoots(Heap::RootVisitor*, void*);

  Runtime* runtime;

  // Used for testing. By default, we'll LOG(FATAL) the reason.
  void (*check_jni_abort_hook)(const std::string& reason);

  bool check_jni;
  bool verbose_jni;
  bool force_copy;

  // Used to provide compatibility for apps that assumed direct references.
  bool work_around_app_jni_bugs;

  // Used to hold references to pinned primitive arrays.
  Mutex* pins_lock;
  ReferenceTable pin_table;

  // JNI global references.
  Mutex* globals_lock;
  IndirectReferenceTable globals;

  // JNI weak global references.
  Mutex* weak_globals_lock;
  IndirectReferenceTable weak_globals;

  Mutex* libraries_lock;
  Libraries* libraries;

  // Used by -Xcheck:jni.
  const JNIInvokeInterface* unchecked_functions;
};

struct JNIEnvExt : public JNIEnv {
  JNIEnvExt(Thread* self, JavaVMExt* vm);
  ~JNIEnvExt();

  Thread* self;
  JavaVMExt* vm;

  // Frequently-accessed fields cached from JavaVM.
  bool check_jni;
  bool work_around_app_jni_bugs;

  // How many nested "critical" JNI calls are we in?
  int critical;

  // Entered JNI monitors, for bulk exit on thread detach.
  ReferenceTable  monitors;

  // JNI local references.
  IndirectReferenceTable locals;

  // Used by -Xcheck:jni.
  const JNINativeInterface* unchecked_functions;
};

const JNINativeInterface* GetCheckJniNativeInterface();
const JNIInvokeInterface* GetCheckJniInvokeInterface();

}  // namespace art

std::ostream& operator<<(std::ostream& os, const jobjectRefType& rhs);

#endif  // ART_SRC_JNI_INTERNAL_H_
