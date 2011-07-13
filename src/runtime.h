// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_RUNTIME_H_
#define ART_SRC_RUNTIME_H_

#include "src/thread.h"

namespace art {

class Runtime {
 public:
  static bool Startup();
  static void Shutdown();

  static void Compile(const char* filename);

  void SetThreadList(ThreadList* thread_list) {
    thread_list_ = thread_list;
  }

 private:
  ThreadList* thread_list_;
};

}  // namespace art

#endif  // ART_SRC_RUNTIME_H_
