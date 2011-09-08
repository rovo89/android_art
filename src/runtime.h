// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_RUNTIME_H_
#define ART_SRC_RUNTIME_H_

#include <stdio.h>

#include <iosfwd>
#include <string>
#include <utility>
#include <vector>

#include <jni.h>

#include "heap.h"
#include "globals.h"
#include "macros.h"
#include "stringpiece.h"
#include "unordered_set.h"

namespace art {

class ClassLinker;
class DexFile;
class Heap;
class InternTable;
class JavaVMExt;
class SignalCatcher;
class String;
class ThreadList;

class Runtime {
 public:

  typedef std::vector<std::pair<StringPiece, const void*> > Options;

  class ParsedOptions {
   public:
    // returns null if problem parsing and ignore_unrecognized is false
    static ParsedOptions* Create(const Options& options, bool ignore_unrecognized);

    std::vector<const DexFile*> boot_class_path_;
    std::vector<const DexFile*> class_path_;
    const char* boot_image_;
    std::vector<const char*> images_;
    bool check_jni_;
    std::string jni_trace_;
    size_t heap_initial_size_;
    size_t heap_maximum_size_;
    size_t stack_size_;
    jint (*hook_vfprintf_)(FILE* stream, const char* format, va_list ap);
    void (*hook_exit_)(jint status);
    void (*hook_abort_)();
    std::tr1::unordered_set<std::string> verbose_;
    std::vector<std::string> properties_;

    bool IsVerbose(const std::string& key) const {
      return verbose_.find(key) != verbose_.end();
    }

   private:
    ParsedOptions() {}
  };

  // Creates and initializes a new runtime.
  static Runtime* Create(const Options& options, bool ignore_unrecognized);

  // Starts a runtime, which may cause threads to be started and code to run.
  void Start();

  bool IsStarted();

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
  void AttachCurrentThread(const char* name, bool as_daemon);

  void CallExitHook(jint status);

  // Detaches the current native thread from the runtime.
  void DetachCurrentThread();

  void DumpStatistics(std::ostream& os);

  ~Runtime();

  size_t GetStackSize() const {
    return stack_size_;
  }

  ClassLinker* GetClassLinker() const {
    return class_linker_;
  }

  InternTable* GetInternTable() const {
    return intern_table_;
  }

  JavaVMExt* GetJavaVM() const {
    return java_vm_;
  }

  ThreadList* GetThreadList() const {
    return thread_list_;
  }

  void VisitRoots(Heap::RootVisitor* visitor, void* arg) const;

 private:
  static void PlatformAbort(const char*, int);

  Runtime();

  void BlockSignals();

  bool Init(const Options& options, bool ignore_unrecognized);
  void InitLibraries();
  void RegisterRuntimeNativeMethods(JNIEnv*);

  // The default stack size for managed threads created by the runtime.
  size_t stack_size_;

  ThreadList* thread_list_;

  InternTable* intern_table_;

  ClassLinker* class_linker_;

  SignalCatcher* signal_catcher_;

  JavaVMExt* java_vm_;

  bool started_;

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
