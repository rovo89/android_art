// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_RUNTIME_H_
#define ART_SRC_RUNTIME_H_

#include "src/globals.h"
#include "src/macros.h"
#include "src/stringpiece.h"

namespace art {

class ClassLinker;
class Heap;
class ThreadList;

class Runtime {
 public:
  // Creates and initializes a new runtime.
  static Runtime* Create();

  // Compiles a dex file.
  static void Compile(const StringPiece& filename);

  // Attaches the current native thread to the runtime.
  bool AttachCurrentThread();

  // Detaches the current native thread from the runtime.
  bool DetachCurrentThread();

  ~Runtime();

 private:
  Runtime() : class_linker_(NULL), heap_(NULL), thread_list_(NULL) {}

  // Initializes a new uninitialized runtime.
  bool Init();

  ClassLinker* class_linker_;

  Heap* heap_;

  ThreadList* thread_list_;

  DISALLOW_COPY_AND_ASSIGN(Runtime);
};

}  // namespace art

#endif  // ART_SRC_RUNTIME_H_
