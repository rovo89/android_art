// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_RUNTIME_H_
#define ART_SRC_RUNTIME_H_

#include <vector>
#include <utility>

#include "globals.h"
#include "macros.h"
#include "stringpiece.h"

namespace art {

class ClassLinker;
class DexFile;
class Heap;
class JniEnvironment;
class ThreadList;

class Runtime {
 public:
  typedef std::vector<std::pair<StringPiece, void*> > Options;

  // Creates and initializes a new runtime.
  static Runtime* Create(const Options& options, bool ignore_unrecognized);
  static Runtime* Create(const std::vector<DexFile*>& boot_class_path);

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
  bool AttachCurrentThread(const char* name, JniEnvironment** jni_env);
  bool AttachCurrentThreadAsDaemon(const char* name, JniEnvironment** jni_env);

  // Detaches the current native thread from the runtime.
  bool DetachCurrentThread();

  ~Runtime();

  ClassLinker* GetClassLinker() {
    return class_linker_;
  }

  void SetVfprintfHook(void* hook);

  void SetExitHook(void* hook);

  void SetAbortHook(void* hook);

 private:
  static void PlatformAbort(const char*, int);

  Runtime() : thread_list_(NULL), class_linker_(NULL) {}

  // Initializes a new uninitialized runtime.
  bool Init(const std::vector<DexFile*>& boot_class_path);

  ThreadList* thread_list_;

  ClassLinker* class_linker_;

  // A pointer to the active runtime or NULL.
  static Runtime* instance_;

  DISALLOW_COPY_AND_ASSIGN(Runtime);
};

}  // namespace art

#endif  // ART_SRC_RUNTIME_H_
