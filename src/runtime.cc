// Copyright 2011 Google Inc. All Rights Reserved.

#include "runtime.h"

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

#include "class_linker.h"
#include "class_loader.h"
#include "debugger.h"
#include "heap.h"
#include "image.h"
#include "intern_table.h"
#include "jni_internal.h"
#include "monitor.h"
#include "oat_file.h"
#include "ScopedLocalRef.h"
#include "signal_catcher.h"
#include "space.h"
#include "thread.h"
#include "thread_list.h"
#include "UniquePtr.h"

// TODO: this drags in cutil/log.h, which conflicts with our logging.h.
#include "JniConstants.h"

namespace art {

Runtime* Runtime::instance_ = NULL;

Runtime::Runtime()
    : verbose_startup_(false),
      is_zygote_(false),
      default_stack_size_(Thread::kDefaultStackSize),
      monitor_list_(NULL),
      thread_list_(NULL),
      intern_table_(NULL),
      class_linker_(NULL),
      signal_catcher_(NULL),
      java_vm_(NULL),
      jni_stub_array_(NULL),
      abstract_method_error_stub_array_(NULL),
      started_(false),
      vfprintf_(NULL),
      exit_(NULL),
      abort_(NULL),
      stats_enabled_(false) {
  for (int i = 0; i < Runtime::kLastTrampolineMethodType; i++) {
    resolution_stub_array_[i] = NULL;
  }
  for (int i = 0; i < Runtime::kLastCalleeSaveType; i++) {
    callee_save_method_[i] = NULL;
  }
}

Runtime::~Runtime() {
  Dbg::StopJdwp();

  // Make sure our internal threads are dead before we start tearing down things they're using.
  delete signal_catcher_;
  // TODO: GC thread.

  // Make sure all other non-daemon threads have terminated, and all daemon threads are suspended.
  delete thread_list_;
  delete monitor_list_;

  delete class_linker_;
  Heap::Destroy();
  delete intern_table_;
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

void Runtime::CallExitHook(jint status) {
  if (exit_ != NULL) {
    ScopedThreadStateChange tsc(Thread::Current(), Thread::kNative);
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

size_t ParseIntegerOrDie(const StringPiece& s) {
  StringPiece::size_type colon = s.find(':');
  if (colon == StringPiece::npos) {
    LOG(FATAL) << "Missing integer: " << s;
  }
  const char* begin = &s.data()[colon + 1];
  char* end;
  size_t result = strtoul(begin, &end, 10);
  if (begin == end || *end != '\0') {
    LOG(FATAL) << "Failed to parse integer in: " << s;
  }
  return result;
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

Runtime::ParsedOptions* Runtime::ParsedOptions::Create(const Options& options, bool ignore_unrecognized) {
  UniquePtr<ParsedOptions> parsed(new ParsedOptions());
  bool compiler = false;
  const char* boot_class_path = getenv("BOOTCLASSPATH");
  if (boot_class_path != NULL) {
    parsed->boot_class_path_ = getenv("BOOTCLASSPATH");
  }
  const char* class_path = getenv("CLASSPATH");
  if (class_path != NULL) {
    parsed->class_path_ = getenv("CLASSPATH");
  }
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

  parsed->is_zygote_ = false;

  parsed->jni_globals_max_ = 0;
  parsed->lock_profiling_threshold_ = 0;
  parsed->hook_is_sensitive_thread_ = NULL;

  parsed->hook_vfprintf_ = vfprintf;
  parsed->hook_exit_ = exit;
  parsed->hook_abort_ = abort;

  for (size_t i = 0; i < options.size(); ++i) {
    const StringPiece& option = options[i].first;
    if (true && options[0].first == "-Xzygote") {
      LOG(INFO) << "option[" << i << "]=" << option;
    }
    if (option.starts_with("-Xbootclasspath:")) {
      parsed->boot_class_path_ = option.substr(strlen("-Xbootclasspath:")).data();
    } else if (option == "-classpath" || option == "-cp") {
      // TODO: support -Djava.class.path
      i++;
      if (i == options.size()) {
        // TODO: usage
        LOG(FATAL) << "Missing required class path value for " << option;
        return NULL;
      }
      const StringPiece& value = options[i].first;
      parsed->class_path_ = value.data();
    } else if (option.starts_with("-Ximage:")) {
      parsed->images_.push_back(option.substr(strlen("-Ximage:")).data());
    } else if (option.starts_with("-Xcheck:jni")) {
      parsed->check_jni_ = true;
    } else if (option.starts_with("-Xrunjdwp:") || option.starts_with("-agentlib:jdwp=")) {
      std::string tail = option.substr(option[1] == 'X' ? 10 : 15).ToString();
      if (tail == "help" || !Dbg::ParseJdwpOptions(tail)) {
        LOG(FATAL) << "Example: -Xrunjdwp:transport=dt_socket,address=8000,server=y\n"
                   << "Example: -Xrunjdwp:transport=dt_socket,address=localhost:6500,server=n";
        return NULL;
      }
    } else if (option.starts_with("-Xms")) {
      size_t size = ParseMemoryOption(option.substr(strlen("-Xms")).data(), 1024);
      if (size == 0) {
        if (ignore_unrecognized) {
          continue;
        }
        // TODO: usage
        LOG(FATAL) << "Failed to parse " << option;
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
        LOG(FATAL) << "Failed to parse " << option;
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
        LOG(FATAL) << "Failed to parse " << option;
        return NULL;
      }
      parsed->stack_size_ = size;
    } else if (option.starts_with("-D")) {
      parsed->properties_.push_back(option.substr(strlen("-D")).data());
    } else if (option.starts_with("-Xjnitrace:")) {
      parsed->jni_trace_ = option.substr(strlen("-Xjnitrace:")).data();
    } else if (option == "compiler") {
      compiler = true;
    } else if (option == "-Xzygote") {
      parsed->is_zygote_ = true;
    } else if (option.starts_with("-verbose:")) {
      std::vector<std::string> verbose_options;
      Split(option.substr(strlen("-verbose:")).data(), ',', verbose_options);
      for (size_t i = 0; i < verbose_options.size(); ++i) {
        parsed->verbose_.insert(verbose_options[i]);
      }
    } else if (option.starts_with("-Xjnigreflimit:")) {
      parsed->jni_globals_max_ = ParseIntegerOrDie(option);
    } else if (option.starts_with("-Xlockprofthreshold:")) {
      parsed->lock_profiling_threshold_ = ParseIntegerOrDie(option);
    } else if (option.starts_with("-Xstacktracefile:")) {
// always show stack traces in debug builds
#ifdef NDEBUG
      parsed->stack_trace_file_ = option.substr(strlen("-Xstacktracefile:")).data();
#endif
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
    } else {
      if (!ignore_unrecognized) {
        // TODO: print usage via vfprintf
        LOG(ERROR) << "Unrecognized option " << option;
        // TODO: this should exit, but for now tolerate unknown options
        //return NULL;
      }
    }
  }

  if (!compiler && parsed->images_.empty()) {
    parsed->images_.push_back("/data/art-cache/boot.art");
  }

  LOG(INFO) << "CheckJNI is " << (parsed->check_jni_ ? "on" : "off");

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
  if (ClassLoader::UseCompileTimeClassPath()) {
    return;
  }

  Thread* self = Thread::Current();

  // Must be in the kNative state for calling native methods.
  CHECK_EQ(self->GetState(), Thread::kNative);

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
}

void Runtime::Start() {
  if (IsVerboseStartup()) {
    LOG(INFO) << "Runtime::Start entering";
  }

  CHECK(host_prefix_.empty()) << host_prefix_;

  InitNativeMethods();

  Thread::FinishStartup();

  class_linker_->RunRootClinits();

  // Class::AllocObject asserts that all objects allocated better be
  // initialized after Runtime::IsStarted is true, so this needs to
  // come after ClassLinker::RunRootClinits.
  started_ = true;

  if (!is_zygote_) {
    DidForkFromZygote();
  }

  StartDaemonThreads();

  CreateSystemClassLoader();

  Thread::Current()->GetJniEnv()->locals.AssertEmpty();

  if (IsVerboseStartup()) {
    LOG(INFO) << "Runtime::Start exiting";
  }
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
  if (IsVerboseStartup()) {
    LOG(INFO) << "Runtime::StartDaemonThreads entering";
  }

  Thread* self = Thread::Current();

  // Must be in the kNative state for calling native methods.
  CHECK_EQ(self->GetState(), Thread::kNative);

  JNIEnv* env = self->GetJniEnv();
  ScopedLocalRef<jclass> c(env, env->FindClass("java/lang/Daemons"));
  CHECK(c.get() != NULL);
  jmethodID mid = env->GetStaticMethodID(c.get(), "start", "()V");
  CHECK(mid != NULL);
  env->CallStaticVoidMethod(c.get(), mid);

  if (IsVerboseStartup()) {
    LOG(INFO) << "Runtime::StartDaemonThreads exiting";
  }
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
  verbose_startup_ = options->IsVerbose("startup");
  if (IsVerboseStartup()) {
    LOG(INFO) << "Runtime::Init -verbose:startup enabled";
  }

  SetJniGlobalsMax(options->jni_globals_max_);
  Monitor::Init(options->IsVerbose("monitor"), options->lock_profiling_threshold_, options->hook_is_sensitive_thread_);

  host_prefix_ = options->host_prefix_;
  boot_class_path_ = options->boot_class_path_;
  class_path_ = options->class_path_;
  properties_ = options->properties_;

  is_zygote_ = options->is_zygote_;

  vfprintf_ = options->hook_vfprintf_;
  exit_ = options->hook_exit_;
  abort_ = options->hook_abort_;

  default_stack_size_ = options->stack_size_;
  stack_trace_file_ = options->stack_trace_file_;

  monitor_list_ = new MonitorList;
  thread_list_ = new ThreadList(options->IsVerbose("thread"));
  intern_table_ = new InternTable;

  Heap::Init(options->IsVerbose("heap"),
             options->IsVerbose("gc"),
             options->heap_initial_size_,
             options->heap_maximum_size_,
             options->images_);

  BlockSignals();

  java_vm_ = new JavaVMExt(this, options.get());

  Thread::Startup();

  // ClassLinker needs an attached thread, but we can't fully attach a thread
  // without creating objects.
  Thread::Attach(this, "main", false);

  CHECK_GE(Heap::GetSpaces().size(), 1U);
  class_linker_ = ((Heap::GetSpaces()[0]->IsImageSpace())
                   ? ClassLinker::Create(intern_table_)
                   : ClassLinker::Create(options->boot_class_path_, intern_table_));

  if (IsVerboseStartup()) {
    LOG(INFO) << "Runtime::Init exiting";
  }
  return true;
}

void Runtime::InitNativeMethods() {
  if (IsVerboseStartup()) {
    LOG(INFO) << "Runtime::InitNativeMethods entering";
  }
  Thread* self = Thread::Current();
  JNIEnv* env = self->GetJniEnv();

  // Must be in the kNative state for calling native methods (JNI_OnLoad code).
  CHECK_EQ(self->GetState(), Thread::kNative);

  // First set up JniConstants, which is used by both the runtime's built-in native
  // methods and libcore.
  JniConstants::init(env);

  // Then set up the native methods provided by the runtime itself.
  RegisterRuntimeNativeMethods(env);

  // Then set up libcore, which is just a regular JNI library with a regular JNI_OnLoad.
  // Most JNI libraries can just use System.loadLibrary, but libcore can't because it's
  // the library that implements System.loadLibrary!
  LoadJniLibrary(instance_->GetJavaVM(), "javacore");
  if (IsVerboseStartup()) {
    LOG(INFO) << "Runtime::InitNativeMethods exiting";
  }
}

void Runtime::RegisterRuntimeNativeMethods(JNIEnv* env) {
#define REGISTER(FN) extern void FN(JNIEnv*); FN(env)
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
  REGISTER(register_java_lang_Throwable);
  REGISTER(register_java_lang_VMClassLoader);
  REGISTER(register_java_lang_reflect_Array);
  REGISTER(register_java_lang_reflect_Constructor);
  REGISTER(register_java_lang_reflect_Field);
  REGISTER(register_java_lang_reflect_Method);
  REGISTER(register_java_lang_reflect_Proxy);
  REGISTER(register_java_util_concurrent_atomic_AtomicLong);
  REGISTER(register_org_apache_harmony_dalvik_ddmc_DdmServer);
  //REGISTER(register_org_apache_harmony_dalvik_ddmc_DdmVmInternal);
  REGISTER(register_sun_misc_Unsafe);
#undef REGISTER
}

void Runtime::Dump(std::ostream& os) {
  // TODO: dump other runtime statistics?
  os << "Loaded classes: " << class_linker_->NumLoadedClasses() << "\n";
  os << "Intern table size: " << GetInternTable()->Size() << "\n";
  // LOGV("VM stats: meth=%d ifld=%d sfld=%d linear=%d",
  //    gDvm.numDeclaredMethods,
  //    gDvm.numDeclaredInstFields,
  //    gDvm.numDeclaredStaticFields,
  //    gDvm.pBootLoaderAlloc->curOffset);
  // LOGI("GC precise methods: %d", dvmPointerSetGetCount(gDvm.preciseMethods));
  os << "\n";

  thread_list_->Dump(os);
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
    return (int) (stats->class_init_time_ns / 1000);
  case KIND_EXT_ALLOCATED_OBJECTS:
  case KIND_EXT_ALLOCATED_BYTES:
  case KIND_EXT_FREED_OBJECTS:
  case KIND_EXT_FREED_BYTES:
    return 0;  // backward compatibility
  default:
    CHECK(false);
    return -1; // unreachable
  }
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

void Runtime::AttachCurrentThread(const char* name, bool as_daemon) {
  Thread::Attach(instance_, name, as_daemon);
}

void Runtime::DetachCurrentThread() {
  // TODO: check we're not calling DetachCurrentThread from a call stack that
  // includes managed frames. (It's only valid if the stack is all-native.)
  thread_list_->Unregister();
}

void Runtime::VisitRoots(Heap::RootVisitor* visitor, void* arg) const {
  class_linker_->VisitRoots(visitor, arg);
  intern_table_->VisitRoots(visitor, arg);
  java_vm_->VisitRoots(visitor, arg);
  thread_list_->VisitRoots(visitor, arg);
  visitor(jni_stub_array_, arg);
  visitor(abstract_method_error_stub_array_, arg);
  for (int i = 0; i < Runtime::kLastTrampolineMethodType; i++) {
    visitor(resolution_stub_array_[i], arg);
  }
  for (int i = 0; i < Runtime::kLastCalleeSaveType; i++) {
    visitor(callee_save_method_[i], arg);
  }
}

bool Runtime::HasJniStubArray() const {
  return jni_stub_array_ != NULL;
}

ByteArray* Runtime::GetJniStubArray() const {
  CHECK(jni_stub_array_ != NULL);
  return jni_stub_array_;
}

void Runtime::SetJniStubArray(ByteArray* jni_stub_array) {
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
    return Runtime::kInstanceMethod;
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

Method* Runtime::CreateCalleeSaveMethod(InstructionSet insns, CalleeSaveType type) {
  Class* method_class = Method::GetMethodClass();
  SirtRef<Method> method(down_cast<Method*>(method_class->AllocObject()));
  method->SetDeclaringClass(method_class);
  const char* name;
  if (type == kSaveAll) {
    name = "$$$callee_save_method$$$";
  } else if (type == kRefsOnly) {
    name = "$$$refs_only_callee_save_method$$$";
  } else {
    DCHECK(type == kRefsAndArgs);
    name = "$$$refs_and_args_callee_save_method$$$";
  }
  method->SetName(intern_table_->InternStrong(name));
  CHECK(method->GetName() != NULL);
  method->SetSignature(intern_table_->InternStrong("()V"));
  CHECK(method->GetSignature() != NULL);
  method->SetCode(NULL);
  if ((insns == kThumb2) || (insns == kArm)) {
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
  } else if (insns == kX86) {
    method->SetFrameSizeInBytes(32);
    method->SetCoreSpillMask((1 << art::x86::EBX) | (1 << art::x86::EBP) | (1 << art::x86::ESI) |
                             (1 << art::x86::EDI));
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

}  // namespace art
