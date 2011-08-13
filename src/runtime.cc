// Copyright 2011 Google Inc. All Rights Reserved.

#include "runtime.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "class_linker.h"
#include "heap.h"
#include "scoped_ptr.h"
#include "thread.h"

namespace art {

Runtime* Runtime::instance_ = NULL;

Runtime::~Runtime() {
  // TODO: use a smart pointer instead.
  delete class_linker_;
  Heap::Destroy();
  delete thread_list_;
  // TODO: acquire a static mutex on Runtime to avoid racing.
  CHECK(instance_ == this);
  instance_ = NULL;
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

// Splits a colon delimited list of pathname elements into a vector of
// strings.  Empty strings will be omitted.
void ParseClassPath(const char* class_path, std::vector<std::string>* vec) {
  CHECK(vec != NULL);
  scoped_ptr_malloc<char> tmp(strdup(class_path));
  char* full = tmp.get();
  char* p = full;
  while (p != NULL) {
    p = strpbrk(full, ":");
    if (p != NULL) {
      p[0] = '\0';
    }
    if (full[0] != '\0') {
      vec->push_back(std::string(full));
    }
    if (p != NULL) {
      full = p + 1;
    }
  }
}

// TODO: move option processing elsewhere.
const char* FindBootClassPath(const Runtime::Options& options) {
  const char* boot_class_path = getenv("BOOTCLASSPATH");
  const char* flag = "-Xbootclasspath:";
  for (size_t i = 0; i < options.size(); ++i) {
    const StringPiece& option = options[i].first;
    if (option.starts_with(flag)) {
      boot_class_path = option.substr(strlen(flag)).data();
    }
  }
  if (boot_class_path == NULL) {
    return "";
  } else {
    return boot_class_path;
  }
}

DexFile* Open(const std::string& filename) {
  if (filename.size() < 4) {
    LOG(WARNING) << "Ignoring short classpath entry '" << filename << "'";
    return NULL;
  }
  std::string suffix(filename.substr(filename.size() - 4));
  if (suffix == ".zip" || suffix == ".jar" || suffix == ".apk") {
    return DexFile::OpenZip(filename);
  } else {
    return DexFile::OpenFile(filename);
  }
}

void CreateBootClassPath(const Runtime::Options& options,
                         std::vector<DexFile*>* boot_class_path) {
  CHECK(boot_class_path != NULL);
  const char* str = FindBootClassPath(options);
  std::vector<std::string> parsed;
  ParseClassPath(str, &parsed);
  for (size_t i = 0; i < parsed.size(); ++i) {
    DexFile* dex_file = Open(parsed[i]);
    if (dex_file != NULL) {
      boot_class_path->push_back(dex_file);
    }
  }
}

// TODO: do something with ignore_unrecognized when we parse the option
// strings for real.
Runtime* Runtime::Create(const Options& options, bool ignore_unrecognized) {
  std::vector<DexFile*> boot_class_path;
  CreateBootClassPath(options, &boot_class_path);
  return Runtime::Create(boot_class_path);
}

Runtime* Runtime::Create(const std::vector<DexFile*>& boot_class_path) {
  // TODO: acquire a static mutex on Runtime to avoid racing.
  if (Runtime::instance_ != NULL) {
    return NULL;
  }
  scoped_ptr<Runtime> runtime(new Runtime());
  bool success = runtime->Init(boot_class_path);
  if (!success) {
    return NULL;
  } else {
    return Runtime::instance_ = runtime.release();
  }
}

bool Runtime::Init(const std::vector<DexFile*>& boot_class_path) {
  CHECK_EQ(kPageSize, sysconf(_SC_PAGE_SIZE));
  thread_list_ = ThreadList::Create();
  Heap::Init(Heap::kStartupSize, Heap::kMaximumSize);
  Thread::Init();
  Thread* current_thread = Thread::Attach();
  thread_list_->Register(current_thread);
  class_linker_ = ClassLinker::Create(boot_class_path);
  return true;
}

bool Runtime::AttachCurrentThread(const char* name, JNIEnv** penv) {
  return Thread::Attach() != NULL;
}

bool Runtime::AttachCurrentThreadAsDaemon(const char* name, JNIEnv** penv) {
  // TODO: do something different for daemon threads.
  return Thread::Attach() != NULL;
}

bool Runtime::DetachCurrentThread() {
  UNIMPLEMENTED(WARNING);
  return true;
}

}  // namespace art
