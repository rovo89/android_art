// Copyright 2011 Google Inc. All Rights Reserved.

#include "src/runtime.h"

#include "src/class_linker.h"
#include "src/heap.h"
#include "src/thread.h"

namespace art {

Runtime::~Runtime() {
  // TODO: use a smart pointer instead.
  delete class_linker_;
  delete heap_;
  delete thread_list_;
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
  heap_ = Heap::Create();
  Thread::Init();
  Thread* current_thread = Thread::Attach();
  thread_list_->Register(current_thread);
  class_linker_ = ClassLinker::Create();
  return true;
}

bool AttachCurrentThread() {
  LOG(FATAL) << "Unimplemented";
  return false;
}

bool DetachCurrentThread() {
  LOG(FATAL) << "Unimplemented";
  return false;
}

}  // namespace art
