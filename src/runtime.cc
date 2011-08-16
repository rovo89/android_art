// Copyright 2011 Google Inc. All Rights Reserved.

#include "runtime.h"

#include <cstdio>
#include <cstdlib>
#include <limits>
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



// Parse a string of the form /[0-9]+[kKmMgG]?/, which is used to specify
// memory sizes.  [kK] indicates kilobytes, [mM] megabytes, and
// [gG] gigabytes.
//
// "s" should point just past the "-Xm?" part of the string.
// "min" specifies the lowest acceptable value described by "s".
// "div" specifies a divisor, e.g. 1024 if the value must be a multiple
// of 1024.
//
// The spec says the -Xmx and -Xms options must be multiples of 1024.  It
// doesn't say anything about -Xss.
//
// Returns 0 (a useless size) if "s" is malformed or specifies a low or
// non-evenly-divisible value.
//
size_t ParseMemoryOption(const char *s, size_t div) {
  // strtoul accepts a leading [+-], which we don't want,
  // so make sure our string starts with a decimal digit.
  if (isdigit(*s)) {
    const char *s2;
    size_t val = strtoul(s, (char **)&s2, 10);
    if (s2 != s) {
      // s2 should be pointing just after the number.
      // If this is the end of the string, the user
      // has specified a number of bytes.  Otherwise,
      // there should be exactly one more character
      // that specifies a multiplier.
      if (*s2 != '\0') {
          // The remainder of the string is either a single multiplier
          // character, or nothing to indicate that the value is in
          // bytes.
          char c = *s2++;
          if (*s2 == '\0') {
            size_t mul;
            if (c == '\0') {
              mul = 1;
            } else if (c == 'k' || c == 'K') {
              mul = 1024;
            } else if (c == 'm' || c == 'M') {
              mul = 1024 * 1024;
            } else if (c == 'g' || c == 'G') {
              mul = 1024 * 1024 * 1024;
            } else {
              // Unknown multiplier character.
              return 0;
            }

            if (val <= std::numeric_limits<size_t>::max() / mul) {
              val *= mul;
            } else {
              // Clamp to a multiple of 1024.
              val = std::numeric_limits<size_t>::max() & ~(1024-1);
            }
          } else {
            // There's more than one character after the numeric part.
            return 0;
          }
      }
      // The man page says that a -Xm value must be a multiple of 1024.
      if (val % div == 0) {
        return val;
      }
    }
  }
  return 0;
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

void CreateBootClassPath(const char* boot_class_path_cstr,
                         std::vector<DexFile*>& boot_class_path_vector) {
  CHECK(boot_class_path_cstr != NULL);
  std::vector<std::string> parsed;
  ParseClassPath(boot_class_path_cstr, &parsed);
  for (size_t i = 0; i < parsed.size(); ++i) {
    DexFile* dex_file = Open(parsed[i]);
    if (dex_file != NULL) {
      boot_class_path_vector.push_back(dex_file);
    }
  }
}

Runtime::ParsedOptions::ParsedOptions(const Options& options, bool ignore_unrecognized) {
  const char* boot_class_path = getenv("BOOTCLASSPATH");
  boot_image_ = NULL;
  heap_initial_size_ = Heap::kInitialSize;
  heap_maximum_size_ = Heap::kMaximumSize;

  for (size_t i = 0; i < options.size(); ++i) {
    const StringPiece& option = options[i].first;
    // TODO parse -D, -verbose, vprintf, exit, abort
    if (option.starts_with("-Xbootclasspath:")) {
      boot_class_path = option.substr(strlen("-Xbootclasspath:")).data();
    } else if (option == "bootclasspath") {
      boot_class_path_ = *reinterpret_cast<const std::vector<DexFile*>*>(options[i].second);
    } else if (option.starts_with("-Xbootimage:")) {
      boot_image_ = option.substr(strlen("-Xbootimage:")).data();
    } else if (option.starts_with("-Xms")) {
      heap_initial_size_ = ParseMemoryOption(option.substr(strlen("-Xms")).data(), 1024);
    } else if (option.starts_with("-Xmx")) {
      heap_maximum_size_ = ParseMemoryOption(option.substr(strlen("-Xmx")).data(), 1024);
    } else {
      if (!ignore_unrecognized) {
        // TODO: indicate error for JNI_CreateJavaVM and print usage via vfprintf
        LOG(FATAL) << "Unrecognized option " << option;
      }
    }
  }

  if (boot_class_path == NULL) {
    boot_class_path = "";
  }
  if (boot_class_path_.size() == 0) {
    CreateBootClassPath(boot_class_path, boot_class_path_);
  }
}

Runtime* Runtime::Create(const std::vector<const DexFile*>& boot_class_path) {
  Runtime::Options options;
  options.push_back(std::make_pair("bootclasspath", &boot_class_path));
  return Runtime::Create(options, false);
}

Runtime* Runtime::Create(const Options& options, bool ignore_unrecognized) {
  // TODO: acquire a static mutex on Runtime to avoid racing.
  if (Runtime::instance_ != NULL) {
    return NULL;
  }
  scoped_ptr<Runtime> runtime(new Runtime());
  bool success = runtime->Init(options, ignore_unrecognized);
  if (!success) {
    return NULL;
  } else {
    return Runtime::instance_ = runtime.release();
  }
}

bool Runtime::Init(const Options& options, bool ignore_unrecognized) {
  ParsedOptions parsed_options(options, ignore_unrecognized);
  CHECK_EQ(kPageSize, sysconf(_SC_PAGE_SIZE));
  thread_list_ = ThreadList::Create();
  Heap::Init(parsed_options.heap_initial_size_,
             parsed_options.heap_maximum_size_);
  Thread::Init();
  Thread* current_thread = Thread::Attach();
  thread_list_->Register(current_thread);
  class_linker_ = ClassLinker::Create(parsed_options.boot_class_path_);
  java_vm_.reset(CreateJavaVM(this));
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
