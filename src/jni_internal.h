// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_JNI_INTERNAL_H_
#define ART_SRC_JNI_INTERNAL_H_

#include "jni.h"

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

  Runtime* runtime;

  bool check_jni;
  bool verbose_jni;

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
};

struct JNIEnvExt : public JNIEnv {
  JNIEnvExt(Thread* self, JavaVMExt* vm);
  ~JNIEnvExt();

  Thread* self;
  JavaVMExt* vm;

  bool check_jni;

  // Are we in a "critical" JNI call?
  bool critical;

  // Entered JNI monitors, for bulk exit on thread detach.
  ReferenceTable  monitors;

  // JNI local references.
  IndirectReferenceTable locals;
};

}  // namespace art

std::ostream& operator<<(std::ostream& os, const jobjectRefType& rhs);

#endif  // ART_SRC_JNI_INTERNAL_H_
