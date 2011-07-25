// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_RUNTIME_H_
#define ART_SRC_RUNTIME_H_

#include <vector>

#include "globals.h"
#include "macros.h"
#include "dex_file.h"
#include "stringpiece.h"

namespace art {

class ClassLinker;
class Heap;
class ThreadList;

class Runtime {
 public:
  // Creates and initializes a new runtime.
  static Runtime* Create(std::vector<DexFile*> boot_class_path);

  // Compiles a dex file.
  static void Compile(const StringPiece& filename);

  // Aborts semi-cleanly. Used in the implementation of LOG(FATAL), which most
  // callers should prefer.
  // This isn't marked ((noreturn)) because then gcc will merge multiple calls
  // in a single function together. This reduces code size slightly, but means
  // that the native stack trace we get may point at the wrong call site.
  static void Abort(const char* file, int line);

  // Attaches the current native thread to the runtime.
  bool AttachCurrentThread();

  // Detaches the current native thread from the runtime.
  bool DetachCurrentThread();

  ~Runtime();

  ClassLinker* GetClassLinker() {
    return class_linker_;
  }

 private:
  static void PlatformAbort(const char*, int);

  Runtime() : class_linker_(NULL), thread_list_(NULL) {}

  // Initializes a new uninitialized runtime.
  bool Init(std::vector<DexFile*> boot_class_path);

  ClassLinker* class_linker_;

  ThreadList* thread_list_;

  DISALLOW_COPY_AND_ASSIGN(Runtime);
};

}  // namespace art

#endif  // ART_SRC_RUNTIME_H_
