/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "runtime.h"

#include <signal.h>

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>
#include <fstream>

#include "class_linker.h"
#include "class_loader.h"
#include "constants_arm.h"
#include "constants_x86.h"
#include "debugger.h"
#include "heap.h"
#include "image.h"
#include "intern_table.h"
#include "jni_internal.h"
#include "monitor.h"
#include "oat_file.h"
#include "ScopedLocalRef.h"
#include "signal_catcher.h"
#include "signal_set.h"
#include "space.h"
#include "thread.h"
#include "thread_list.h"
#include "trace.h"
#include "UniquePtr.h"
#include "verifier/method_verifier.h"

// TODO: this drags in cutil/log.h, which conflicts with our logging.h.
#include "JniConstants.h"

namespace art {

Runtime* Runtime::instance_ = NULL;

Runtime::Runtime()
    : is_compiler_(false),
      is_zygote_(false),
      default_stack_size_(Thread::kDefaultStackSize),
      heap_(NULL),
      monitor_list_(NULL),
      thread_list_(NULL),
      intern_table_(NULL),
      class_linker_(NULL),
      signal_catcher_(NULL),
      java_vm_(NULL),
      jni_stub_array_(NULL),
      abstract_method_error_stub_array_(NULL),
      resolution_method_(NULL),
      system_class_loader_(NULL),
      shutting_down_(false),
      started_(false),
      vfprintf_(NULL),
      exit_(NULL),
      abort_(NULL),
      stats_enabled_(false),
      method_trace_(0),
      method_trace_file_size_(0),
      tracer_(NULL),
      use_compile_time_class_path_(false) {
  for (int i = 0; i < Runtime::kLastTrampolineMethodType; i++) {
    resolution_stub_array_[i] = NULL;
  }
  for (int i = 0; i < Runtime::kLastCalleeSaveType; i++) {
    callee_save_method_[i] = NULL;
  }
}

Runtime::~Runtime() {
  shutting_down_ = true;

  if (IsMethodTracingActive()) {
    Trace::Shutdown();
  }

  // Make sure our internal threads are dead before we start tearing down things they're using.
  Dbg::StopJdwp();
  delete signal_catcher_;

  // Make sure all other non-daemon threads have terminated, and all daemon threads are suspended.
  delete thread_list_;
  delete monitor_list_;

  delete class_linker_;
  delete heap_;
#if defined(ART_USE_LLVM_COMPILER)
  verifier::MethodVerifier::DeleteInferredRegCategoryMaps();
#endif
  verifier::MethodVerifier::DeleteGcMaps();
  delete intern_table_;
  delete java_vm_;
  Thread::Shutdown();
  // TODO: acquire a static mutex on Runtime to avoid racing.
  CHECK(instance_ == NULL || instance_ == this);
  instance_ = NULL;
}

static bool gAborting = false;

struct AbortState {
  void Dump(std::ostream& os) {
    if (gAborting) {
      os << "Runtime aborting --- recursively, so no thread-specific detail!\n";
      return;
    }
    gAborting = true;
    os << "Runtime aborting...\n";
    if (Runtime::Current() == NULL) {
      os << "(Runtime does not yet exist!)\n";
      return;
    }
    Thread* self = Thread::Current();
    if (self == NULL) {
      os << "(Aborting thread was not attached to runtime!)\n";
    } else {
      self->Dump(os);
      if (self->IsExceptionPending()) {
        os << "Pending " << PrettyTypeOf(self->GetException()) << " on thread:\n"
           << self->GetException()->Dump();
      }
    }
  }
};

static Mutex& GetAbortLock() {
  static Mutex abort_lock("abort lock");
  return abort_lock;
}

void Runtime::Abort() {
  // Ensure that we don't have multiple threads trying to abort at once,
  // which would result in significantly worse diagnostics.
  MutexLock mu(GetAbortLock());

  // Get any pending output out of the way.
  fflush(NULL);

  // Many people have difficulty distinguish aborts from crashes,
  // so be explicit.
  AbortState state;
  LOG(INTERNAL_FATAL) << Dumpable<AbortState>(state);

  // Call the abort hook if we have one.
  if (Runtime::Current() != NULL && Runtime::Current()->abort_ != NULL) {
    LOG(INTERNAL_FATAL) << "Calling abort hook...";
    Runtime::Current()->abort_();
    // notreached
    LOG(INTERNAL_FATAL) << "Unexpectedly returned from abort hook!";
  }

  // TODO: finish merging patches to fix abort(3) in bionic, then lose this!
  // Bionic doesn't implement POSIX semantics for abort(3) in a multi-threaded
  // process, so if we call abort(3) on a device, all threads in the process
  // receive SIGABRT.  debuggerd dumps the stack trace of the main
  // thread, whether or not that was the thread that failed.  By
  // stuffing a value into a bogus address, we cause a segmentation
  // fault in the current thread, and get a useful log from debuggerd.
  // We can also trivially tell the difference between a crash and
  // a deliberate abort by looking at the fault address.
  *reinterpret_cast<char*>(0xdeadd00d) = 38;
  abort();
  // notreached
}

void Runtime::CallExitHook(jint status) {
  if (exit_ != NULL) {
    ScopedThreadStateChange tsc(Thread::Current(), kNative);
    exit_(status);
    LOG(WARNING) << "Exit hook returned instead of exiting!";
  }
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
size_t ParseMemoryOption(const char* s, size_t div) {
  // strtoul accepts a leading [+-], which we don't want,
  // so make sure our string starts with a decimal digit.
  if (isdigit(*s)) {
    char* s2;
    size_t val = strtoul(s, &s2, 10);
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

size_t ParseIntegerOrDie(const std::string& s) {
  std::string::size_type colon = s.find(':');
  if (colon == std::string::npos) {
    LOG(FATAL) << "Missing integer: " << s;
  }
  const char* begin = &s[colon + 1];
  char* end;
  size_t result = strtoul(begin, &end, 10);
  if (begin == end || *end != '\0') {
    LOG(FATAL) << "Failed to parse integer in: " << s;
  }
  return result;
}

void LoadJniLibrary(JavaVMExt* vm, const char* name) {
  std::string mapped_name(StringPrintf(OS_SHARED_LIB_FORMAT_STR, name));
  std::string reason;
  if (!vm->LoadNativeLibrary(mapped_name, NULL, reason)) {
    LOG(FATAL) << "LoadNativeLibrary failed for \"" << mapped_name << "\": "
               << reason;
  }
}

Runtime::ParsedOptions* Runtime::ParsedOptions::Create(const Options& options, bool ignore_unrecognized) {
  UniquePtr<ParsedOptions> parsed(new ParsedOptions());
  const char* boot_class_path_string = getenv("BOOTCLASSPATH");
  if (boot_class_path_string != NULL) {
    parsed->boot_class_path_string_ = boot_class_path_string;
  }
  const char* class_path_string = getenv("CLASSPATH");
  if (class_path_string != NULL) {
    parsed->class_path_string_ = class_path_string;
  }
  // -Xcheck:jni is off by default for regular builds but on by default in debug builds.
  parsed->check_jni_ = kIsDebugBuild;

  parsed->heap_initial_size_ = Heap::kInitialSize;
  parsed->heap_maximum_size_ = Heap::kMaximumSize;
  parsed->heap_growth_limit_ = 0;  // 0 means no growth limit
  parsed->stack_size_ = Thread::kDefaultStackSize;

  parsed->is_compiler_ = false;
  parsed->is_zygote_ = false;

  parsed->jni_globals_max_ = 0;
  parsed->lock_profiling_threshold_ = 0;
  parsed->hook_is_sensitive_thread_ = NULL;

  parsed->hook_vfprintf_ = vfprintf;
  parsed->hook_exit_ = exit;
  parsed->hook_abort_ = NULL; // We don't call abort(3) by default; see Runtime::Abort.
#if defined(__APPLE__)
  parsed->hook_abort_ = abort; // On the Mac, abort(3) gives better results; see Runtime::InitPlatformSignalHandlers.
#endif

//  gLogVerbosity.class_linker = true; // TODO: don't check this in!
//  gLogVerbosity.compiler = true; // TODO: don't check this in!
//  gLogVerbosity.heap = true; // TODO: don't check this in!
//  gLogVerbosity.gc = true; // TODO: don't check this in!
//  gLogVerbosity.jdwp = true; // TODO: don't check this in!
//  gLogVerbosity.jni = true; // TODO: don't check this in!
//  gLogVerbosity.monitor = true; // TODO: don't check this in!
//  gLogVerbosity.startup = true; // TODO: don't check this in!
//  gLogVerbosity.third_party_jni = true; // TODO: don't check this in!
//  gLogVerbosity.threads = true; // TODO: don't check this in!

  parsed->method_trace_ = false;
  parsed->method_trace_file_ = "/data/method-trace-file.bin";
  parsed->method_trace_file_size_ = 10 * MB;

  for (size_t i = 0; i < options.size(); ++i) {
    const std::string option(options[i].first);
    if (true && options[0].first == "-Xzygote") {
      LOG(INFO) << "option[" << i << "]=" << option;
    }
    if (StartsWith(option, "-Xbootclasspath:")) {
      parsed->boot_class_path_string_ = option.substr(strlen("-Xbootclasspath:")).data();
    } else if (option == "-classpath" || option == "-cp") {
      // TODO: support -Djava.class.path
      i++;
      if (i == options.size()) {
        // TODO: usage
        LOG(FATAL) << "Missing required class path value for " << option;
        return NULL;
      }
      const StringPiece& value = options[i].first;
      parsed->class_path_string_ = value.data();
    } else if (option == "bootclasspath") {
      parsed->boot_class_path_
          = reinterpret_cast<const std::vector<const DexFile*>*>(options[i].second);
    } else if (StartsWith(option, "-Ximage:")) {
      parsed->image_ = option.substr(strlen("-Ximage:")).data();
    } else if (StartsWith(option, "-Xcheck:jni")) {
      parsed->check_jni_ = true;
    } else if (StartsWith(option, "-Xrunjdwp:") || StartsWith(option, "-agentlib:jdwp=")) {
      std::string tail(option.substr(option[1] == 'X' ? 10 : 15));
      if (tail == "help" || !Dbg::ParseJdwpOptions(tail)) {
        LOG(FATAL) << "Example: -Xrunjdwp:transport=dt_socket,address=8000,server=y\n"
                   << "Example: -Xrunjdwp:transport=dt_socket,address=localhost:6500,server=n";
        return NULL;
      }
    } else if (StartsWith(option, "-Xms")) {
      size_t size = ParseMemoryOption(option.substr(strlen("-Xms")).c_str(), 1024);
      if (size == 0) {
        if (ignore_unrecognized) {
          continue;
        }
        // TODO: usage
        LOG(FATAL) << "Failed to parse " << option;
        return NULL;
      }
      parsed->heap_initial_size_ = size;
    } else if (StartsWith(option, "-Xmx")) {
      size_t size = ParseMemoryOption(option.substr(strlen("-Xmx")).c_str(), 1024);
      if (size == 0) {
        if (ignore_unrecognized) {
          continue;
        }
        // TODO: usage
        LOG(FATAL) << "Failed to parse " << option;
        return NULL;
      }
      parsed->heap_maximum_size_ = size;
    } else if (StartsWith(option, "-XX:HeapGrowthLimit=")) {
      size_t size = ParseMemoryOption(option.substr(strlen("-XX:HeapGrowthLimit=")).c_str(), 1024);
      if (size == 0) {
        if (ignore_unrecognized) {
          continue;
        }
        // TODO: usage
        LOG(FATAL) << "Failed to parse " << option;
        return NULL;
      }
      parsed->heap_growth_limit_ = size;
    } else if (StartsWith(option, "-Xss")) {
      size_t size = ParseMemoryOption(option.substr(strlen("-Xss")).c_str(), 1);
      if (size == 0) {
        if (ignore_unrecognized) {
          continue;
        }
        // TODO: usage
        LOG(FATAL) << "Failed to parse " << option;
        return NULL;
      }
      parsed->stack_size_ = size;
    } else if (StartsWith(option, "-D")) {
      parsed->properties_.push_back(option.substr(strlen("-D")));
    } else if (StartsWith(option, "-Xjnitrace:")) {
      parsed->jni_trace_ = option.substr(strlen("-Xjnitrace:"));
    } else if (option == "compiler") {
      parsed->is_compiler_ = true;
    } else if (option == "-Xzygote") {
      parsed->is_zygote_ = true;
    } else if (StartsWith(option, "-verbose:")) {
      std::vector<std::string> verbose_options;
      Split(option.substr(strlen("-verbose:")), ',', verbose_options);
      for (size_t i = 0; i < verbose_options.size(); ++i) {
        if (verbose_options[i] == "class") {
          gLogVerbosity.class_linker = true;
        } else if (verbose_options[i] == "compiler") {
          gLogVerbosity.compiler = true;
        } else if (verbose_options[i] == "heap") {
          gLogVerbosity.heap = true;
        } else if (verbose_options[i] == "gc") {
          gLogVerbosity.gc = true;
        } else if (verbose_options[i] == "jdwp") {
          gLogVerbosity.jdwp = true;
        } else if (verbose_options[i] == "jni") {
          gLogVerbosity.jni = true;
        } else if (verbose_options[i] == "monitor") {
          gLogVerbosity.monitor = true;
        } else if (verbose_options[i] == "startup") {
          gLogVerbosity.startup = true;
        } else if (verbose_options[i] == "third-party-jni") {
          gLogVerbosity.third_party_jni = true;
        } else if (verbose_options[i] == "threads") {
          gLogVerbosity.threads = true;
        } else if (StartsWith(verbose_options[i], "log-to=")) {
          std::string log_file_name(verbose_options[i].substr(strlen("log-to=")));
          std::ofstream* log_file = new std::ofstream(log_file_name.c_str());
          if (log_file->is_open() && log_file->good()) {
            gLogVerbosity.SetLoggingStream(log_file);
          } else {
            LOG(ERROR) << "Fail to open log file: \"" << log_file_name << "\","
                       << " use default logging stream.";
          }
        } else {
          LOG(WARNING) << "Ignoring unknown -verbose option: " << verbose_options[i];
        }
      }
    } else if (StartsWith(option, "-Xjnigreflimit:")) {
      parsed->jni_globals_max_ = ParseIntegerOrDie(option);
    } else if (StartsWith(option, "-Xlockprofthreshold:")) {
      parsed->lock_profiling_threshold_ = ParseIntegerOrDie(option);
    } else if (StartsWith(option, "-Xstacktracefile:")) {
      if (kIsDebugBuild) {
        // Ignore the zygote and always show stack traces in debug builds.
      } else {
        parsed->stack_trace_file_ = option.substr(strlen("-Xstacktracefile:"));
      }
    } else if (option == "sensitiveThread") {
      parsed->hook_is_sensitive_thread_ = reinterpret_cast<bool (*)()>(options[i].second);
    } else if (option == "vfprintf") {
      parsed->hook_vfprintf_ = reinterpret_cast<int (*)(FILE *, const char*, va_list)>(options[i].second);
    } else if (option == "exit") {
      parsed->hook_exit_ = reinterpret_cast<void(*)(jint)>(options[i].second);
    } else if (option == "abort") {
      parsed->hook_abort_ = reinterpret_cast<void(*)()>(options[i].second);
    } else if (option == "host-prefix") {
      parsed->host_prefix_ = reinterpret_cast<const char*>(options[i].second);
    } else if (option == "-Xgenregmap" || option == "-Xgc:precise") {
      // We silently ignore these for backwards compatibility.
    } else if (option == "-Xmethod-trace") {
      parsed->method_trace_ = true;
    } else if (StartsWith(option, "-Xmethod-trace-file:")) {
      parsed->method_trace_file_ = option.substr(strlen("-Xmethod-trace-file:"));
    } else if (StartsWith(option, "-Xmethod-trace-file-size:")) {
      parsed->method_trace_file_size_ = ParseIntegerOrDie(option);
    } else {
      if (!ignore_unrecognized) {
        // TODO: print usage via vfprintf
        LOG(ERROR) << "Unrecognized option " << option;
        // TODO: this should exit, but for now tolerate unknown options
        //return NULL;
      }
    }
  }

  if (!parsed->is_compiler_ && parsed->image_.empty()) {
    parsed->image_ += GetAndroidRoot();
    parsed->image_ += "/framework/boot.art";
  }
  if (parsed->heap_growth_limit_ == 0) {
    parsed->heap_growth_limit_ = parsed->heap_maximum_size_;
  }

  return parsed.release();
}

Runtime* Runtime::Create(const Options& options, bool ignore_unrecognized) {
  // TODO: acquire a static mutex on Runtime to avoid racing.
  if (Runtime::instance_ != NULL) {
    return NULL;
  }
  instance_ = new Runtime;
  if (!instance_->Init(options, ignore_unrecognized)) {
    delete instance_;
    instance_ = NULL;
  }
  return instance_;
}

void CreateSystemClassLoader() {
  if (Runtime::Current()->UseCompileTimeClassPath()) {
    return;
  }

  Thread* self = Thread::Current();

  // Must be in the kNative state for calling native methods.
  CHECK_EQ(self->GetState(), kNative);

  JNIEnv* env = self->GetJniEnv();
  ScopedLocalRef<jclass> ClassLoader_class(env, env->FindClass("java/lang/ClassLoader"));
  CHECK(ClassLoader_class.get() != NULL);
  jmethodID getSystemClassLoader = env->GetStaticMethodID(ClassLoader_class.get(),
                                                          "getSystemClassLoader",
                                                          "()Ljava/lang/ClassLoader;");
  CHECK(getSystemClassLoader != NULL);
  ScopedLocalRef<jobject> class_loader(env, env->CallStaticObjectMethod(ClassLoader_class.get(),
                                                                        getSystemClassLoader));
  CHECK(class_loader.get() != NULL);

  Thread::Current()->SetClassLoaderOverride(Decode<ClassLoader*>(env, class_loader.get()));

  ScopedLocalRef<jclass> Thread_class(env, env->FindClass("java/lang/Thread"));
  CHECK(Thread_class.get() != NULL);
  jfieldID contextClassLoader = env->GetFieldID(Thread_class.get(),
                                                "contextClassLoader",
                                                "Ljava/lang/ClassLoader;");
  CHECK(contextClassLoader != NULL);
  ScopedLocalRef<jobject> self_jobject(env, AddLocalReference<jobject>(env, self->GetPeer()));
  env->SetObjectField(self_jobject.get(), contextClassLoader, class_loader.get());
}

void Runtime::Start() {
  VLOG(startup) << "Runtime::Start entering";

  CHECK(host_prefix_.empty()) << host_prefix_;

  // Relocate the OatFiles (ELF images)
  class_linker_->RelocateExecutable();

  // Restore main thread state to kNative as expected by native code
  Thread::Current()->SetState(kNative);

  started_ = true;

  // InitNativeMethods needs to be after started_ so that the classes
  // it touches will have methods linked to the oat file if necessary.
  InitNativeMethods();

  Thread::FinishStartup();

  if (!is_zygote_) {
    DidForkFromZygote();
  }

  StartDaemonThreads();

  CreateSystemClassLoader();

  Thread::Current()->GetJniEnv()->locals.AssertEmpty();

  VLOG(startup) << "Runtime::Start exiting";
}

void Runtime::DidForkFromZygote() {
  is_zygote_ = false;

  StartSignalCatcher();

  // Start the JDWP thread. If the command-line debugger flags specified "suspend=y",
  // this will pause the runtime, so we probably want this to come last.
  Dbg::StartJdwp();
}

void Runtime::StartSignalCatcher() {
  if (!is_zygote_) {
    signal_catcher_ = new SignalCatcher(stack_trace_file_);
  }
}

void Runtime::StartDaemonThreads() {
  VLOG(startup) << "Runtime::StartDaemonThreads entering";

  Thread* self = Thread::Current();

  // Must be in the kNative state for calling native methods.
  CHECK_EQ(self->GetState(), kNative);

  JNIEnv* env = self->GetJniEnv();
  ScopedLocalRef<jclass> c(env, env->FindClass("java/lang/Daemons"));
  CHECK(c.get() != NULL);
  jmethodID mid = env->GetStaticMethodID(c.get(), "start", "()V");
  CHECK(mid != NULL);
  env->CallStaticVoidMethod(c.get(), mid);
  CHECK(!env->ExceptionCheck());

  VLOG(startup) << "Runtime::StartDaemonThreads exiting";
}

bool Runtime::IsShuttingDown() const {
  return shutting_down_;
}

bool Runtime::IsStarted() const {
  return started_;
}

bool Runtime::Init(const Options& raw_options, bool ignore_unrecognized) {
  CHECK_EQ(sysconf(_SC_PAGE_SIZE), kPageSize);

  UniquePtr<ParsedOptions> options(ParsedOptions::Create(raw_options, ignore_unrecognized));
  if (options.get() == NULL) {
    LOG(ERROR) << "Failed to parse options";
    return false;
  }
  VLOG(startup) << "Runtime::Init -verbose:startup enabled";

  SetJniGlobalsMax(options->jni_globals_max_);
  Monitor::Init(options->lock_profiling_threshold_, options->hook_is_sensitive_thread_);

  host_prefix_ = options->host_prefix_;
  boot_class_path_string_ = options->boot_class_path_string_;
  class_path_string_ = options->class_path_string_;
  properties_ = options->properties_;

  is_compiler_ = options->is_compiler_;
  is_zygote_ = options->is_zygote_;

  vfprintf_ = options->hook_vfprintf_;
  exit_ = options->hook_exit_;
  abort_ = options->hook_abort_;

  default_stack_size_ = options->stack_size_;
  stack_trace_file_ = options->stack_trace_file_;

  monitor_list_ = new MonitorList;
  thread_list_ = new ThreadList;
  intern_table_ = new InternTable;

  verifier::MethodVerifier::InitGcMaps();

#if defined(ART_USE_LLVM_COMPILER)
  verifier::MethodVerifier::InitInferredRegCategoryMaps();
#endif

  heap_ = new Heap(options->heap_initial_size_,
                   options->heap_growth_limit_,
                   options->heap_maximum_size_,
                   options->image_);

  BlockSignals();
  InitPlatformSignalHandlers();

  java_vm_ = new JavaVMExt(this, options.get());

  Thread::Startup();

  // ClassLinker needs an attached thread, but we can't fully attach a thread
  // without creating objects. We can't supply a thread group yet; it will be fixed later.
  Thread::Attach("main", false, NULL);

  // Set us to runnable so tools using a runtime can allocate and GC by default
  Thread::Current()->SetState(kRunnable);

  // Now we're attached, we can take the heap lock and validate the heap.
  GetHeap()->EnableObjectValidation();

  CHECK_GE(GetHeap()->GetSpaces().size(), 1U);
  if (GetHeap()->GetSpaces()[0]->IsImageSpace()) {
    class_linker_ = ClassLinker::CreateFromImage(intern_table_);
  } else {
    CHECK(options->boot_class_path_ != NULL);
    CHECK_NE(options->boot_class_path_->size(), 0U);
    class_linker_ = ClassLinker::CreateFromCompiler(*options->boot_class_path_, intern_table_);
  }
  CHECK(class_linker_ != NULL);

  method_trace_ = options->method_trace_;
  method_trace_file_ = options->method_trace_file_;
  method_trace_file_size_ = options->method_trace_file_size_;

  if (options->method_trace_) {
    Trace::Start(options->method_trace_file_.c_str(), -1, options->method_trace_file_size_, 0, false);
  }

  VLOG(startup) << "Runtime::Init exiting";
  return true;
}

void Runtime::InitNativeMethods() {
  VLOG(startup) << "Runtime::InitNativeMethods entering";
  Thread* self = Thread::Current();
  JNIEnv* env = self->GetJniEnv();

  // Must be in the kNative state for calling native methods (JNI_OnLoad code).
  CHECK_EQ(self->GetState(), kNative);

  // First set up JniConstants, which is used by both the runtime's built-in native
  // methods and libcore.
  JniConstants::init(env);

  // Then set up the native methods provided by the runtime itself.
  RegisterRuntimeNativeMethods(env);

  // Then set up libcore, which is just a regular JNI library with a regular JNI_OnLoad.
  // Most JNI libraries can just use System.loadLibrary, but libcore can't because it's
  // the library that implements System.loadLibrary!
  LoadJniLibrary(instance_->GetJavaVM(), "javacore");
  VLOG(startup) << "Runtime::InitNativeMethods exiting";
}

void Runtime::RegisterRuntimeNativeMethods(JNIEnv* env) {
#define REGISTER(FN) extern void FN(JNIEnv*); FN(env)
  // Register Throwable first so that registration of other native methods can throw exceptions
  REGISTER(register_java_lang_Throwable);
  REGISTER(register_dalvik_system_DexFile);
  REGISTER(register_dalvik_system_VMDebug);
  REGISTER(register_dalvik_system_VMRuntime);
  REGISTER(register_dalvik_system_VMStack);
  REGISTER(register_dalvik_system_Zygote);
  REGISTER(register_java_lang_Class);
  REGISTER(register_java_lang_Object);
  REGISTER(register_java_lang_Runtime);
  REGISTER(register_java_lang_String);
  REGISTER(register_java_lang_System);
  REGISTER(register_java_lang_Thread);
  REGISTER(register_java_lang_VMClassLoader);
  REGISTER(register_java_lang_reflect_Array);
  REGISTER(register_java_lang_reflect_Constructor);
  REGISTER(register_java_lang_reflect_Field);
  REGISTER(register_java_lang_reflect_Method);
  REGISTER(register_java_lang_reflect_Proxy);
  REGISTER(register_java_util_concurrent_atomic_AtomicLong);
  REGISTER(register_org_apache_harmony_dalvik_ddmc_DdmServer);
  REGISTER(register_org_apache_harmony_dalvik_ddmc_DdmVmInternal);
  REGISTER(register_sun_misc_Unsafe);
#undef REGISTER
}

void Runtime::DumpForSigQuit(std::ostream& os) {
  GetClassLinker()->DumpForSigQuit(os);
  GetInternTable()->DumpForSigQuit(os);
  GetJavaVM()->DumpForSigQuit(os);
  GetHeap()->DumpForSigQuit(os);
  os << "\n";

  thread_list_->DumpForSigQuit(os);
}

void Runtime::DumpLockHolders(std::ostream& os) {
  pid_t heap_lock_owner = GetHeap()->GetLockOwner();
  pid_t thread_list_lock_owner = GetThreadList()->GetLockOwner();
  pid_t classes_lock_owner = GetClassLinker()->GetClassesLockOwner();
  pid_t dex_lock_owner = GetClassLinker()->GetDexLockOwner();
  if ((heap_lock_owner | thread_list_lock_owner | classes_lock_owner | dex_lock_owner) != 0) {
    os << "Heap lock owner tid: " << heap_lock_owner << "\n"
       << "ThreadList lock owner tid: " << thread_list_lock_owner << "\n"
       << "ClassLinker classes lock owner tid: " << classes_lock_owner << "\n"
       << "ClassLinker dex lock owner tid: " << dex_lock_owner << "\n";
  }
}

void Runtime::SetStatsEnabled(bool new_state) {
  if (new_state == true) {
    GetStats()->Clear(~0);
    // TODO: wouldn't it make more sense to clear _all_ threads' stats?
    Thread::Current()->GetStats()->Clear(~0);
  }
  stats_enabled_ = new_state;
}

void Runtime::ResetStats(int kinds) {
  GetStats()->Clear(kinds & 0xffff);
  // TODO: wouldn't it make more sense to clear _all_ threads' stats?
  Thread::Current()->GetStats()->Clear(kinds >> 16);
}

RuntimeStats* Runtime::GetStats() {
  return &stats_;
}

int32_t Runtime::GetStat(int kind) {
  RuntimeStats* stats;
  if (kind < (1<<16)) {
    stats = GetStats();
  } else {
    stats = Thread::Current()->GetStats();
    kind >>= 16;
  }
  switch (kind) {
  case KIND_ALLOCATED_OBJECTS:
    return stats->allocated_objects;
  case KIND_ALLOCATED_BYTES:
    return stats->allocated_bytes;
  case KIND_FREED_OBJECTS:
    return stats->freed_objects;
  case KIND_FREED_BYTES:
    return stats->freed_bytes;
  case KIND_GC_INVOCATIONS:
    return stats->gc_for_alloc_count;
  case KIND_CLASS_INIT_COUNT:
    return stats->class_init_count;
  case KIND_CLASS_INIT_TIME:
    // Convert ns to us, reduce to 32 bits.
    return static_cast<int>(stats->class_init_time_ns / 1000);
  case KIND_EXT_ALLOCATED_OBJECTS:
  case KIND_EXT_ALLOCATED_BYTES:
  case KIND_EXT_FREED_OBJECTS:
  case KIND_EXT_FREED_BYTES:
    return 0;  // backward compatibility
  default:
    LOG(FATAL) << "Unknown statistic " << kind;
    return -1; // unreachable
  }
}

void Runtime::BlockSignals() {
  SignalSet signals;
  signals.Add(SIGPIPE);
  // SIGQUIT is used to dump the runtime's state (including stack traces).
  signals.Add(SIGQUIT);
  // SIGUSR1 is used to initiate a GC.
  signals.Add(SIGUSR1);
  signals.Block();
}

void Runtime::AttachCurrentThread(const char* thread_name, bool as_daemon, Object* thread_group) {
  Thread::Attach(thread_name, as_daemon, thread_group);
  if (thread_name == NULL) {
    LOG(WARNING) << *Thread::Current() << " attached without supplying a name";
  }
}

void Runtime::DetachCurrentThread() {
  if (Thread::Current()->GetTopOfStack().GetSP() != NULL) {
    LOG(FATAL) << *Thread::Current() << " attempting to detach while still running code";
  }
  thread_list_->Unregister();
}

void Runtime::VisitRoots(Heap::RootVisitor* visitor, void* arg) const {
  Dbg::VisitRoots(visitor, arg);
  class_linker_->VisitRoots(visitor, arg);
  intern_table_->VisitRoots(visitor, arg);
  java_vm_->VisitRoots(visitor, arg);
  thread_list_->VisitRoots(visitor, arg);
  visitor(jni_stub_array_, arg);
  visitor(abstract_method_error_stub_array_, arg);
  for (int i = 0; i < Runtime::kLastTrampolineMethodType; i++) {
    visitor(resolution_stub_array_[i], arg);
  }
  visitor(resolution_method_, arg);
  for (int i = 0; i < Runtime::kLastCalleeSaveType; i++) {
    visitor(callee_save_method_[i], arg);
  }
}

bool Runtime::HasJniDlsymLookupStub() const {
  return jni_stub_array_ != NULL;
}

ByteArray* Runtime::GetJniDlsymLookupStub() const {
  CHECK(jni_stub_array_ != NULL);
  return jni_stub_array_;
}

void Runtime::SetJniDlsymLookupStub(ByteArray* jni_stub_array) {
  CHECK(jni_stub_array != NULL)  << " jni_stub_array=" << jni_stub_array;
  CHECK(jni_stub_array_ == NULL || jni_stub_array_ == jni_stub_array)
      << "jni_stub_array_=" << jni_stub_array_ << " jni_stub_array=" << jni_stub_array;
  jni_stub_array_ = jni_stub_array;
}

bool Runtime::HasAbstractMethodErrorStubArray() const {
  return abstract_method_error_stub_array_ != NULL;
}

ByteArray* Runtime::GetAbstractMethodErrorStubArray() const {
  CHECK(abstract_method_error_stub_array_ != NULL);
  return abstract_method_error_stub_array_;
}

void Runtime::SetAbstractMethodErrorStubArray(ByteArray* abstract_method_error_stub_array) {
  CHECK(abstract_method_error_stub_array != NULL);
  CHECK(abstract_method_error_stub_array_ == NULL || abstract_method_error_stub_array_ == abstract_method_error_stub_array);
  abstract_method_error_stub_array_ = abstract_method_error_stub_array;
}


Runtime::TrampolineType Runtime::GetTrampolineType(Method* method) {
  if (method == NULL) {
    return Runtime::kUnknownMethod;
  } else if (method->IsStatic()) {
    return Runtime::kStaticMethod;
  } else {
    return Runtime::kUnknownMethod;
  }
}

bool Runtime::HasResolutionStubArray(TrampolineType type) const {
  return resolution_stub_array_[type] != NULL;
}

ByteArray* Runtime::GetResolutionStubArray(TrampolineType type) const {
  CHECK(HasResolutionStubArray(type));
  DCHECK_LT(static_cast<int>(type), static_cast<int>(kLastTrampolineMethodType));
  return resolution_stub_array_[type];
}

void Runtime::SetResolutionStubArray(ByteArray* resolution_stub_array, TrampolineType type) {
  CHECK(resolution_stub_array != NULL);
  CHECK(!HasResolutionStubArray(type) || resolution_stub_array_[type] == resolution_stub_array);
  resolution_stub_array_[type] = resolution_stub_array;
}

Method* Runtime::CreateResolutionMethod() {
  Class* method_class = Method::GetMethodClass();
  SirtRef<Method> method(down_cast<Method*>(method_class->AllocObject()));
  method->SetDeclaringClass(method_class);
  // TODO: use a special method for resolution method saves
  method->SetDexMethodIndex(DexFile::kDexNoIndex16);
  ByteArray* unknown_resolution_stub = GetResolutionStubArray(kUnknownMethod);
  CHECK(unknown_resolution_stub != NULL);
  method->SetCode(unknown_resolution_stub->GetData());
  return method.get();
}

bool Runtime::HasResolutionMethod() const {
  return resolution_method_ != NULL;
}

// Returns a special method that calls into a trampoline for runtime method resolution
Method* Runtime::GetResolutionMethod() const {
  CHECK(HasResolutionMethod());
  return resolution_method_;
}

void Runtime::SetResolutionMethod(Method* method) {
  resolution_method_ = method;
}

Method* Runtime::CreateCalleeSaveMethod(InstructionSet instruction_set, CalleeSaveType type) {
  Class* method_class = Method::GetMethodClass();
  SirtRef<Method> method(down_cast<Method*>(method_class->AllocObject()));
  method->SetDeclaringClass(method_class);
  // TODO: use a special method for callee saves
  method->SetDexMethodIndex(DexFile::kDexNoIndex16);
  method->SetCode(NULL);
  if ((instruction_set == kThumb2) || (instruction_set == kArm)) {
    uint32_t ref_spills = (1 << art::arm::R5) | (1 << art::arm::R6)  | (1 << art::arm::R7) |
                          (1 << art::arm::R8) | (1 << art::arm::R10) | (1 << art::arm::R11);
    uint32_t arg_spills = (1 << art::arm::R1) | (1 << art::arm::R2) | (1 << art::arm::R3);
    uint32_t all_spills = (1 << art::arm::R4) | (1 << art::arm::R9);
    uint32_t core_spills = ref_spills | (type == kRefsAndArgs ? arg_spills :0) |
                           (type == kSaveAll ? all_spills :0) | (1 << art::arm::LR);
    uint32_t fp_all_spills = (1 << art::arm::S0)  | (1 << art::arm::S1)  | (1 << art::arm::S2) |
                             (1 << art::arm::S3)  | (1 << art::arm::S4)  | (1 << art::arm::S5) |
                             (1 << art::arm::S6)  | (1 << art::arm::S7)  | (1 << art::arm::S8) |
                             (1 << art::arm::S9)  | (1 << art::arm::S10) | (1 << art::arm::S11) |
                             (1 << art::arm::S12) | (1 << art::arm::S13) | (1 << art::arm::S14) |
                             (1 << art::arm::S15) | (1 << art::arm::S16) | (1 << art::arm::S17) |
                             (1 << art::arm::S18) | (1 << art::arm::S19) | (1 << art::arm::S20) |
                             (1 << art::arm::S21) | (1 << art::arm::S22) | (1 << art::arm::S23) |
                             (1 << art::arm::S24) | (1 << art::arm::S25) | (1 << art::arm::S26) |
                             (1 << art::arm::S27) | (1 << art::arm::S28) | (1 << art::arm::S29) |
                             (1 << art::arm::S30) | (1 << art::arm::S31);
    uint32_t fp_spills = type == kSaveAll ? fp_all_spills : 0;
    size_t frame_size = RoundUp((__builtin_popcount(core_spills) /* gprs */ +
                                 __builtin_popcount(fp_spills) /* fprs */ +
                                 1 /* Method* */) * kPointerSize, kStackAlignment);
    method->SetFrameSizeInBytes(frame_size);
    method->SetCoreSpillMask(core_spills);
    method->SetFpSpillMask(fp_spills);
  } else if (instruction_set == kX86) {
    uint32_t ref_spills = (1 << art::x86::EBP) | (1 << art::x86::ESI) | (1 << art::x86::EDI);
    uint32_t arg_spills = (1 << art::x86::ECX) | (1 << art::x86::EDX) | (1 << art::x86::EBX);
    uint32_t core_spills = ref_spills | (type == kRefsAndArgs ? arg_spills : 0) |
                         (1 << art::x86::kNumberOfCpuRegisters);  // fake return address callee save
    size_t frame_size = RoundUp((__builtin_popcount(core_spills) /* gprs */ +
                                 1 /* Method* */) * kPointerSize, kStackAlignment);
    method->SetFrameSizeInBytes(frame_size);
    method->SetCoreSpillMask(core_spills);
    method->SetFpSpillMask(0);
  } else {
    UNIMPLEMENTED(FATAL);
  }
  return method.get();
}

bool Runtime::HasCalleeSaveMethod(CalleeSaveType type) const {
  return callee_save_method_[type] != NULL;
}

// Returns a special method that describes all callee saves being spilled to the stack.
Method* Runtime::GetCalleeSaveMethod(CalleeSaveType type) const {
  CHECK(HasCalleeSaveMethod(type));
  return callee_save_method_[type];
}

void Runtime::SetCalleeSaveMethod(Method* method, CalleeSaveType type) {
  DCHECK_LT(static_cast<int>(type), static_cast<int>(kLastCalleeSaveType));
  callee_save_method_[type] = method;
}

void Runtime::EnableMethodTracing(Trace* tracer) {
  CHECK(!IsMethodTracingActive());
  tracer_ = tracer;
}

void Runtime::DisableMethodTracing() {
  CHECK(IsMethodTracingActive());
  delete tracer_;
  tracer_ = NULL;
}

bool Runtime::IsMethodTracingActive() const {
  return (tracer_ != NULL);
}

Trace* Runtime::GetTracer() const {
  CHECK(IsMethodTracingActive());
  return tracer_;
}

const std::vector<const DexFile*>& Runtime::GetCompileTimeClassPath(const ClassLoader* class_loader) {
  if (class_loader == NULL) {
    return GetClassLinker()->GetBootClassPath();
  }
  CHECK(UseCompileTimeClassPath());
  CompileTimeClassPaths::const_iterator it = compile_time_class_paths_.find(class_loader);
  CHECK(it != compile_time_class_paths_.end());
  return it->second;
}

void Runtime::SetCompileTimeClassPath(const ClassLoader* class_loader, std::vector<const DexFile*>& class_path) {
  CHECK(!IsStarted());
  use_compile_time_class_path_ = true;
  compile_time_class_paths_.Put(class_loader, class_path);
}

}  // namespace art
