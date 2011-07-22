// Copyright 2011 Google Inc. All Rights Reserved.

#include "src/runtime.h"

#include <cstdio>
#include <cstdlib>

#include "src/class_linker.h"
#include "src/heap.h"
#include "src/thread.h"

namespace art {

Runtime::~Runtime() {
  // TODO: use a smart pointer instead.
  delete class_linker_;
  Heap::Destroy();
  delete thread_list_;
}

void Runtime::Abort(const char* file, int line) {
  // Get any pending output out of the way.
  fflush(NULL);

  // Many people have difficulty distinguish aborts from crashes,
  // so be explicit.
  LogMessage(file, line, ERROR, -1).stream() << "Runtime aborting...";

  // TODO: if we support an abort hook, call it here.

  // Perform any platform-specific pre-abort actions.
  PlatformAbort(file, line);

  // If we call abort(3) on a device, all threads in the process
  // receive SIGABRT.  debuggerd dumps the stack trace of the main
  // thread, whether or not that was the thread that failed.  By
  // stuffing a value into a bogus address, we cause a segmentation
  // fault in the current thread, and get a useful log from debuggerd.
  // We can also trivially tell the difference between a VM crash and
  // a deliberate abort by looking at the fault address.
  *reinterpret_cast<char*>(0xdeadd00d) = 38;
  abort();

  // notreached
}

Runtime* Runtime::Create() {
  scoped_ptr<Runtime> runtime(new Runtime());
  bool success = runtime->Init();
  if (!success) {
    return NULL;
  } else {
    return runtime.release();
  }
}

bool Runtime::Init() {
  thread_list_ = ThreadList::Create();
  Heap::Init(Heap::kStartupSize, Heap::kMaximumSize);
  Thread::Init();
  Thread* current_thread = Thread::Attach();
  thread_list_->Register(current_thread);
  class_linker_ = ClassLinker::Create();
  return true;
}

bool Runtime::AttachCurrentThread() {
  return Thread::Attach() != NULL;
}

bool Runtime::DetachCurrentThread() {
  LOG(WARNING) << "Unimplemented: Runtime::DetachCurrentThread";
  return true;
}

}  // namespace art
