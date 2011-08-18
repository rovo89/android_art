// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_RUNTIME_H_
#define ART_SRC_RUNTIME_H_

#include <set>
#include <string>
#include <utility>
#include <vector>

#include "globals.h"
#include "jni_internal.h"
#include "macros.h"
#include "scoped_ptr.h"
#include "stringpiece.h"

namespace art {

class ClassLinker;
class DexFile;
class Heap;
class String;
class ThreadList;

class Runtime {
 public:

  typedef std::vector<std::pair<StringPiece, const void*> > Options;

  class ParsedOptions {
   public:
    // returns null if problem parsing and ignore_unrecognized is false
    static ParsedOptions* Create(const Options& options, bool ignore_unrecognized);

    std::vector<DexFile*> boot_class_path_;
    const char* boot_image_;
    bool check_jni_;
    size_t heap_initial_size_;
    size_t heap_maximum_size_;
    jint (*hook_vfprintf_)(FILE* stream, const char* format, va_list ap);
    void (*hook_exit_)(jint status);
    void (*hook_abort_)();
    std::set<std::string> verbose_;
    std::vector<std::string> properties_;

   private:
    ParsedOptions() {};
  };

  // Creates and initializes a new runtime.
  static Runtime* Create(const Options& options, bool ignore_unrecognized);
  static Runtime* Create(const std::vector<const DexFile*>& boot_class_path);

  static Runtime* Current() {
    return instance_;
  }

  // Compiles a dex file.
  static void Compile(const StringPiece& filename);

  // Aborts semi-cleanly. Used in the implementation of LOG(FATAL), which most
  // callers should prefer.
  // This isn't marked ((noreturn)) because then gcc will merge multiple calls
  // in a single function together. This reduces code size slightly, but means
  // that the native stack trace we get may point at the wrong call site.
  static void Abort(const char* file, int line);

  // Attaches the current native thread to the runtime.
  bool AttachCurrentThread(const char* name, JNIEnv** jni_env);
  bool AttachCurrentThreadAsDaemon(const char* name, JNIEnv** jni_env);

  // Detaches the current native thread from the runtime.
  bool DetachCurrentThread();

  ~Runtime();

  ClassLinker* GetClassLinker() {
    return class_linker_;
  }

  JavaVMExt* GetJavaVM() const {
    return java_vm_.get();
  }

 private:
  static void PlatformAbort(const char*, int);

  Runtime() : thread_list_(NULL), class_linker_(NULL) {}

  // Initializes a new uninitialized runtime.
  bool Init(const Options& options, bool ignore_unrecognized);

  ThreadList* thread_list_;

  ClassLinker* class_linker_;

  scoped_ptr<JavaVMExt> java_vm_;

  // Hooks supported by JNI_CreateJavaVM
  jint (*vfprintf_)(FILE* stream, const char* format, va_list ap);
  void (*exit_)(jint status);
  void (*abort_)();

  // A pointer to the active runtime or NULL.
  static Runtime* instance_;

  DISALLOW_COPY_AND_ASSIGN(Runtime);
};

}  // namespace art

#endif  // ART_SRC_RUNTIME_H_
