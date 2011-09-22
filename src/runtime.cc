// Copyright 2011 Google Inc. All Rights Reserved.

#include "runtime.h"

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

#include "UniquePtr.h"
#include "class_linker.h"
#include "heap.h"
#include "intern_table.h"
#include "jni_internal.h"
#include "signal_catcher.h"
#include "thread.h"
#include "thread_list.h"

// TODO: this drags in cutil/log.h, which conflicts with our logging.h.
#include "JniConstants.h"

namespace art {

Runtime* Runtime::instance_ = NULL;

Runtime::Runtime()
    : default_stack_size_(Thread::kDefaultStackSize),
      thread_list_(NULL),
      intern_table_(NULL),
      class_linker_(NULL),
      signal_catcher_(NULL),
      java_vm_(NULL),
      jni_stub_array_(NULL),
      callee_save_method_(NULL),
      started_(false),
      vfprintf_(NULL),
      exit_(NULL),
      abort_(NULL),
      stats_enabled_(false) {
}

Runtime::~Runtime() {
  // Make sure our internal threads are dead before we start tearing down things they're using.
  delete signal_catcher_;
  // TODO: GC thread.

  // Make sure all other non-daemon threads have terminated, and all daemon threads are suspended.
  delete thread_list_;

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

void LoadJniLibrary(JavaVMExt* vm, const char* name) {
  // TODO: OS_SHARED_LIB_FORMAT_STR
  std::string mapped_name(StringPrintf("lib%s.so", name));
  std::string reason;
  if (!vm->LoadNativeLibrary(mapped_name, NULL, reason)) {
    LOG(FATAL) << "LoadNativeLibrary failed for \"" << mapped_name << "\": "
               << reason;
  }
}

void CreateClassPath(const std::string& class_path,
                     std::vector<const DexFile*>& class_path_vector) {
  std::vector<std::string> parsed;
  Split(class_path, ':', parsed);
  for (size_t i = 0; i < parsed.size(); ++i) {
    const DexFile* dex_file = DexFile::Open(parsed[i], "");
    if (dex_file != NULL) {
      class_path_vector.push_back(dex_file);
    }
  }
}

Runtime::ParsedOptions* Runtime::ParsedOptions::Create(const Options& options, bool ignore_unrecognized) {
  UniquePtr<ParsedOptions> parsed(new ParsedOptions());
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
      parsed->boot_class_path_string_ = option.substr(strlen("-Xbootclasspath:")).data();
    } else if (option == "bootclasspath") {
      UNIMPLEMENTED(WARNING) << "what should VMRuntime.getBootClassPath return here?";
      const void* dex_vector = options[i].second;
      const std::vector<const DexFile*>* v
          = reinterpret_cast<const std::vector<const DexFile*>*>(dex_vector);
      if (v == NULL) {
        if (ignore_unrecognized) {
          continue;
        }
        // TODO: usage
        LOG(FATAL) << "Failed to parse " << option;
        return NULL;
      }
      parsed->boot_class_path_ = *v;
    } else if (option == "classpath") {
      const void* dex_vector = options[i].second;
      const std::vector<const DexFile*>* v
          = reinterpret_cast<const std::vector<const DexFile*>*>(dex_vector);
      if (v == NULL) {
        if (ignore_unrecognized) {
          continue;
        }
        // TODO: usage
        LOG(FATAL) << "Failed to parse " << option;
        return NULL;
      }
      parsed->class_path_ = *v;
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
    } else if (option.starts_with("-Xbootimage:")) {
      // TODO: remove when intern_addr_ is removed, just use -Ximage:
      parsed->boot_image_ = option.substr(strlen("-Xbootimage:")).data();
    } else if (option.starts_with("-Ximage:")) {
      parsed->images_.push_back(option.substr(strlen("-Ximage:")).data());
    } else if (option.starts_with("-Xcheck:jni")) {
      parsed->check_jni_ = true;
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
        LOG(ERROR) << "Unrecognized option " << option;
        // TODO: this should exit, but for now tolerate unknown options
        //return NULL;
      }
    }
  }

  // Consider it an error if both bootclasspath and -Xbootclasspath: are supplied.
  // TODO: remove bootclasspath and classpath which are mostly just used by tests?
  if (!parsed->boot_class_path_.empty() && !parsed->boot_class_path_string_.empty()) {
    // TODO: usage
    LOG(FATAL) << "bootclasspath and -Xbootclasspath: are mutually exclusive options.";
    return NULL;
  }
  if (!parsed->class_path_.empty() && !parsed->class_path_string_.empty()) {
    // TODO: usage
    LOG(FATAL) << "bootclasspath and -Xbootclasspath: are mutually exclusive options.";
    return NULL;
  }
  if (parsed->boot_class_path_.empty()) {
    if (parsed->boot_class_path_string_ == NULL) {
      const char* BOOTCLASSPATH = getenv("BOOTCLASSPATH");
      if (BOOTCLASSPATH != NULL) {
        parsed->boot_class_path_string_ = BOOTCLASSPATH;
      }
    }
    CreateClassPath(parsed->boot_class_path_string_, parsed->boot_class_path_);
  }

  if (parsed->class_path_.empty()) {
    if (parsed->class_path_string_ == NULL) {
      const char* CLASSPATH = getenv("CLASSPATH");
      if (CLASSPATH != NULL) {
        parsed->class_path_string_ = CLASSPATH;
      }
    }
    CreateClassPath(parsed->class_path_string_, parsed->class_path_);
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

void Runtime::Start() {
  started_ = true;

  InitNativeMethods();

  Thread::FinishStartup();

  RunImageClinits();

  StartDaemonThreads();
}

// initialize classes that have instances in the image but that have
// <clinit> methods so they could not be initialized by the compiler.
void Runtime::RunImageClinits() {
  Class* Field_class = class_linker_->FindSystemClass("Ljava/lang/reflect/Field;");
  CHECK(Field_class->FindDeclaredDirectMethod("<clinit>", "()V") != NULL);
  class_linker_->EnsureInitialized(Field_class, true);
  CHECK(!Thread::Current()->IsExceptionPending());
}

void Runtime::StartDaemonThreads() {
  signal_catcher_ = new SignalCatcher;

  Class* c = class_linker_->FindSystemClass("Ljava/lang/Daemons;");
  CHECK(c != NULL);
  Method* m = c->FindDirectMethod("start", "()V");
  CHECK(m != NULL);
  m->Invoke(Thread::Current(), NULL, NULL, NULL);
}

bool Runtime::IsStarted() {
  return started_;
}

bool Runtime::Init(const Options& raw_options, bool ignore_unrecognized) {
  CHECK_EQ(sysconf(_SC_PAGE_SIZE), kPageSize);

  UniquePtr<ParsedOptions> options(ParsedOptions::Create(raw_options, ignore_unrecognized));
  if (options.get() == NULL) {
    LOG(WARNING) << "Failed to parse options";
    return false;
  }

  boot_class_path_ = options->boot_class_path_string_;
  class_path_ = options->class_path_string_;
  properties_ = options->properties_;

  vfprintf_ = options->hook_vfprintf_;
  exit_ = options->hook_exit_;
  abort_ = options->hook_abort_;

  default_stack_size_ = options->stack_size_;

  thread_list_ = new ThreadList;
  intern_table_ = new InternTable;

  Heap::Init(options->heap_initial_size_, options->heap_maximum_size_,
      options->boot_image_, options->images_);

  BlockSignals();

  java_vm_ = new JavaVMExt(this, options.get());

  Thread::Startup();

  // ClassLinker needs an attached thread, but we can't fully attach a thread
  // without creating objects.
  Thread::Attach(this, "main", false);

  class_linker_ = ClassLinker::Create(options->boot_class_path_,
                                      options->class_path_,
                                      intern_table_,
                                      Heap::GetBootSpace());

  return true;
}

void Runtime::InitNativeMethods() {
  Thread* self = Thread::Current();
  JNIEnv* env = self->GetJniEnv();

  // Must be in the kNative state for JNI-based method registration.
  ScopedThreadStateChange tsc(self, Thread::kNative);

  JniConstants::init(env);

  // First set up the native methods provided by the runtime itself.
  RegisterRuntimeNativeMethods(env);

  // Now set up libcore, which is just a JNI library with a JNI_OnLoad.
  // Most JNI libraries can just use System.loadLibrary, but you can't
  // if you're the library that implements System.loadLibrary!
  LoadJniLibrary(instance_->GetJavaVM(), "javacore");
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
  //REGISTER(register_java_lang_reflect_AccessibleObject);
  REGISTER(register_java_lang_reflect_Array);
  //REGISTER(register_java_lang_reflect_Constructor);
  REGISTER(register_java_lang_reflect_Field);
  REGISTER(register_java_lang_reflect_Method);
  //REGISTER(register_java_lang_reflect_Proxy);
  REGISTER(register_java_util_concurrent_atomic_AtomicLong);
  //REGISTER(register_org_apache_harmony_dalvik_ddmc_DdmServer);
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

Method* Runtime::CreateCalleeSaveMethod(InstructionSet insns) {
  Class* method_class = Method::GetMethodClass();
  Method* method = down_cast<Method*>(method_class->AllocObject());
  method->SetDeclaringClass(method_class);
  method->SetName(intern_table_->InternStrong("$$$callee_save_method$$$"));
  method->SetSignature(intern_table_->InternStrong("()V"));
  method->SetCode(NULL, insns, NULL);
  if ((insns == kThumb2) || (insns == kArm)) {
    method->SetFrameSizeInBytes(64);
    method->SetReturnPcOffsetInBytes(60);
    method->SetCoreSpillMask((1 << art::arm::R1) |
                             (1 << art::arm::R2) |
                             (1 << art::arm::R3) |
                             (1 << art::arm::R4) |
                             (1 << art::arm::R5) |
                             (1 << art::arm::R6) |
                             (1 << art::arm::R7) |
                             (1 << art::arm::R8) |
                             (1 << art::arm::R9) |
                             (1 << art::arm::R10) |
                             (1 << art::arm::R11) |
                             (1 << art::arm::LR));
    method->SetFpSpillMask(0);
  } else if (insns == kX86) {
    method->SetFrameSizeInBytes(32);
    method->SetReturnPcOffsetInBytes(28);
    method->SetCoreSpillMask((1 << art::x86::EBX) |
                             (1 << art::x86::EBP) |
                             (1 << art::x86::ESI) |
                             (1 << art::x86::EDI));
    method->SetFpSpillMask(0);
  } else {
    UNIMPLEMENTED(FATAL);
  }
  return method;
}

void Runtime::VisitRoots(Heap::RootVisitor* visitor, void* arg) const {
  class_linker_->VisitRoots(visitor, arg);
  intern_table_->VisitRoots(visitor, arg);
  java_vm_->VisitRoots(visitor, arg);
  thread_list_->VisitRoots(visitor, arg);
  visitor(jni_stub_array_, arg);
  visitor(callee_save_method_, arg);

  //(*visitor)(&gDvm.outOfMemoryObj, 0, ROOT_VM_INTERNAL, arg);
  //(*visitor)(&gDvm.internalErrorObj, 0, ROOT_VM_INTERNAL, arg);
  //(*visitor)(&gDvm.noClassDefFoundErrorObj, 0, ROOT_VM_INTERNAL, arg);
  UNIMPLEMENTED(WARNING) << "some roots not marked";
}

}  // namespace art
