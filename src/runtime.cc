// Copyright 2011 Google Inc. All Rights Reserved.

#include "runtime.h"

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

#include "UniquePtr.h"
#include "class_linker.h"
#include "heap.h"
#include "jni_internal.h"
#include "signal_catcher.h"
#include "thread.h"

// TODO: this drags in cutil/log.h, which conflicts with our logging.h.
#include "JniConstants.h"

namespace art {

Runtime* Runtime::instance_ = NULL;

Runtime::~Runtime() {
  // TODO: use smart pointers instead. (we'll need the pimpl idiom.)
  delete class_linker_;
  Heap::Destroy();
  delete signal_catcher_;
  delete thread_list_;
  delete java_vm_;
  Thread::Shutdown();
  // TODO: acquire a static mutex on Runtime to avoid racing.
  CHECK(instance_ == NULL || instance_ == this);
  instance_ = NULL;
}

void Runtime::Abort(const char* file, int line) {
  // Get any pending output out of the way.
  fflush(NULL);

  // Many people have difficulty distinguish aborts from crashes,
  // so be explicit.
  LogMessage(file, line, ERROR, -1).stream() << "Runtime aborting...";

  // Perform any platform-specific pre-abort actions.
  PlatformAbort(file, line);

  // use abort hook if we have one
  if (Runtime::Current() != NULL && Runtime::Current()->abort_ != NULL) {
    Runtime::Current()->abort_();
    // notreached
  }

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

// Parse a string of the form /[0-9]+[kKmMgG]?/, which is used to specify
// memory sizes.  [kK] indicates kilobytes, [mM] megabytes, and
// [gG] gigabytes.
//
// "s" should point just past the "-Xm?" part of the string.
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
            mul = KB;
          } else if (c == 'm' || c == 'M') {
            mul = MB;
          } else if (c == 'g' || c == 'G') {
            mul = GB;
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

void LoadJniLibrary(JavaVMExt* vm, const char* name) {
  // TODO: OS_SHARED_LIB_FORMAT_STR
  std::string mapped_name(StringPrintf("lib%s.so", name));
  std::string reason;
  if (!vm->LoadNativeLibrary(mapped_name, NULL, reason)) {
    LOG(FATAL) << "LoadNativeLibrary failed for \"" << mapped_name << "\": "
               << reason;
  }
}

const DexFile* Open(const std::string& filename) {
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
                         std::vector<const DexFile*>& boot_class_path_vector) {
  CHECK(boot_class_path_cstr != NULL);
  std::vector<std::string> parsed;
  Split(boot_class_path_cstr, ':', parsed);
  for (size_t i = 0; i < parsed.size(); ++i) {
    const DexFile* dex_file = Open(parsed[i]);
    if (dex_file != NULL) {
      boot_class_path_vector.push_back(dex_file);
    }
  }
}

Runtime::ParsedOptions* Runtime::ParsedOptions::Create(const Options& options, bool ignore_unrecognized) {
  UniquePtr<ParsedOptions> parsed(new ParsedOptions());
  const char* boot_class_path = getenv("BOOTCLASSPATH");
  parsed->boot_image_ = NULL;
#ifdef NDEBUG
  // -Xcheck:jni is off by default for regular builds...
  parsed->check_jni_ = false;
#else
  // ...but on by default in debug builds.
  parsed->check_jni_ = true;
#endif
  parsed->heap_initial_size_ = Heap::kInitialSize;
  parsed->heap_maximum_size_ = Heap::kMaximumSize;
  parsed->stack_size_ = Thread::kDefaultStackSize;

  parsed->hook_vfprintf_ = vfprintf;
  parsed->hook_exit_ = exit;
  parsed->hook_abort_ = abort;

  for (size_t i = 0; i < options.size(); ++i) {
    const StringPiece& option = options[i].first;
    if (option.starts_with("-Xbootclasspath:")) {
      boot_class_path = option.substr(strlen("-Xbootclasspath:")).data();
    } else if (option == "bootclasspath") {
      const void* dex_vector = options[i].second;
      const std::vector<const DexFile*>* v
          = reinterpret_cast<const std::vector<const DexFile*>*>(dex_vector);
      if (v == NULL) {
        if (ignore_unrecognized) {
          continue;
        }
        // TODO: usage
        LOG(FATAL) << "Could not parse " << option;
        return NULL;
      }
      parsed->boot_class_path_ = *v;
    } else if (option.starts_with("-Xbootimage:")) {
      parsed->boot_image_ = option.substr(strlen("-Xbootimage:")).data();
    } else if (option.starts_with("-Xcheck:jni")) {
      parsed->check_jni_ = true;
    } else if (option.starts_with("-Xms")) {
      size_t size = ParseMemoryOption(option.substr(strlen("-Xms")).data(), 1024);
      if (size == 0) {
        if (ignore_unrecognized) {
          continue;
        }
        // TODO: usage
        LOG(FATAL) << "Could not parse " << option;
        return NULL;
      }
      parsed->heap_initial_size_ = size;
    } else if (option.starts_with("-Xmx")) {
      size_t size = ParseMemoryOption(option.substr(strlen("-Xmx")).data(), 1024);
      if (size == 0) {
        if (ignore_unrecognized) {
          continue;
        }
        // TODO: usage
        LOG(FATAL) << "Could not parse " << option;
        return NULL;
      }
      parsed->heap_maximum_size_ = size;
    } else if (option.starts_with("-Xss")) {
      size_t size = ParseMemoryOption(option.substr(strlen("-Xss")).data(), 1);
      if (size == 0) {
        if (ignore_unrecognized) {
          continue;
        }
        // TODO: usage
        LOG(FATAL) << "Could not parse " << option;
        return NULL;
      }
      parsed->stack_size_ = size;
    } else if (option.starts_with("-D")) {
      parsed->properties_.push_back(option.substr(strlen("-D")).data());
    } else if (option.starts_with("-verbose:")) {
      std::vector<std::string> verbose_options;
      Split(option.substr(strlen("-verbose:")).data(), ',', verbose_options);
      for (size_t i = 0; i < verbose_options.size(); ++i) {
        parsed->verbose_.insert(verbose_options[i]);
      }
    } else if (option == "vfprintf") {
      parsed->hook_vfprintf_ = reinterpret_cast<int (*)(FILE *, const char*, va_list)>(options[i].second);
    } else if (option == "exit") {
      parsed->hook_exit_ = reinterpret_cast<void(*)(jint)>(options[i].second);
    } else if (option == "abort") {
      parsed->hook_abort_ = reinterpret_cast<void(*)()>(options[i].second);
    } else {
      if (!ignore_unrecognized) {
        // TODO: print usage via vfprintf
        LOG(FATAL) << "Unrecognized option " << option;
        return NULL;
      }
    }
  }

  if (boot_class_path == NULL) {
    boot_class_path = "";
  }
  if (parsed->boot_class_path_.size() == 0) {
    CreateBootClassPath(boot_class_path, parsed->boot_class_path_);
  }
  return parsed.release();
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
  UniquePtr<Runtime> runtime(new Runtime());
  bool success = runtime->Init(options, ignore_unrecognized);
  if (!success) {
    return NULL;
  }
  instance_ = runtime.release();

  // Most JNI libraries can just use System.loadLibrary, but you can't
  // if you're the library that implements System.loadLibrary!
  Thread* self = Thread::Current();
  Thread::State old_state = self->GetState();
  self->SetState(Thread::kNative);
  JniConstants::init(self->GetJniEnv());
  LoadJniLibrary(instance_->GetJavaVM(), "javacore");
  self->SetState(old_state);

  instance_->signal_catcher_ = new SignalCatcher;

  return instance_;
}

bool Runtime::Init(const Options& raw_options, bool ignore_unrecognized) {
  CHECK_EQ(sysconf(_SC_PAGE_SIZE), kPageSize);

  UniquePtr<ParsedOptions> options(ParsedOptions::Create(raw_options, ignore_unrecognized));
  if (options.get() == NULL) {
    return false;
  }
  vfprintf_ = options->hook_vfprintf_;
  exit_ = options->hook_exit_;
  abort_ = options->hook_abort_;

  stack_size_ = options->stack_size_;
  thread_list_ = ThreadList::Create();

  if (!Heap::Init(options->heap_initial_size_,
                  options->heap_maximum_size_,
                  options->boot_image_)) {
    return false;
  }

  BlockSignals();

  bool verbose_jni = options->verbose_.find("jni") != options->verbose_.end();
  java_vm_ = new JavaVMExt(this, options->check_jni_, verbose_jni);

  if (!Thread::Startup()) {
    return false;
  }
  Thread* current_thread = Thread::Attach(this);
  thread_list_->Register(current_thread);

  class_linker_ = ClassLinker::Create(options->boot_class_path_, Heap::GetBootSpace());

  return true;
}

void Runtime::DumpStatistics(std::ostream& os) {
  // TODO: dump other runtime statistics?
  os << "Loaded classes: " << class_linker_->NumLoadedClasses() << "\n";
  os << "Intern table size: " << class_linker_->GetInternTable().Size() << "\n";
  // LOGV("VM stats: meth=%d ifld=%d sfld=%d linear=%d",
  //    gDvm.numDeclaredMethods,
  //    gDvm.numDeclaredInstFields,
  //    gDvm.numDeclaredStaticFields,
  //    gDvm.pBootLoaderAlloc->curOffset);
  // LOGI("GC precise methods: %d", dvmPointerSetGetCount(gDvm.preciseMethods));
}

void Runtime::BlockSignals() {
  sigset_t sigset;
  if (sigemptyset(&sigset) == -1) {
    PLOG(FATAL) << "sigemptyset failed";
  }
  if (sigaddset(&sigset, SIGPIPE) == -1) {
    PLOG(ERROR) << "sigaddset SIGPIPE failed";
  }
  // SIGQUIT is used to dump the runtime's state (including stack traces).
  if (sigaddset(&sigset, SIGQUIT) == -1) {
    PLOG(ERROR) << "sigaddset SIGQUIT failed";
  }
  // SIGUSR1 is used to initiate a heap dump.
  if (sigaddset(&sigset, SIGUSR1) == -1) {
    PLOG(ERROR) << "sigaddset SIGUSR1 failed";
  }
  CHECK_EQ(sigprocmask(SIG_BLOCK, &sigset, NULL), 0);
}

bool Runtime::AttachCurrentThread(const char* name, JNIEnv** penv, bool as_daemon) {
  if (as_daemon) {
    UNIMPLEMENTED(WARNING) << "TODO: do something different for daemon threads";
  }
  return Thread::Attach(instance_) != NULL;
}

bool Runtime::DetachCurrentThread() {
  Thread* self = Thread::Current();
  thread_list_->Unregister(self);
  delete self;
  return true;
}

void Runtime::VisitRoots(Heap::RootVistor* root_visitor, void* arg) const {
  GetClassLinker()->VisitRoots(root_visitor, arg);
  UNIMPLEMENTED(WARNING) << "mark other roots including per thread state and thread stacks";
}

}  // namespace art
