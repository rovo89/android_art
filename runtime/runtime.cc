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

// sys/mount.h has to come before linux/fs.h due to redefinition of MS_RDONLY, MS_BIND, etc
#include <sys/mount.h>
#include <linux/fs.h>

#include <signal.h>
#include <sys/syscall.h>
#include <valgrind.h>

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>
#include <fcntl.h>

#include "arch/arm/quick_method_frame_info_arm.h"
#include "arch/arm/registers_arm.h"
#include "arch/arm64/quick_method_frame_info_arm64.h"
#include "arch/arm64/registers_arm64.h"
#include "arch/mips/quick_method_frame_info_mips.h"
#include "arch/mips/registers_mips.h"
#include "arch/x86/quick_method_frame_info_x86.h"
#include "arch/x86/registers_x86.h"
#include "arch/x86_64/quick_method_frame_info_x86_64.h"
#include "arch/x86_64/registers_x86_64.h"
#include "atomic.h"
#include "class_linker.h"
#include "debugger.h"
#include "gc/accounting/card_table-inl.h"
#include "gc/heap.h"
#include "gc/space/space.h"
#include "image.h"
#include "instrumentation.h"
#include "intern_table.h"
#include "jni_internal.h"
#include "mirror/art_field-inl.h"
#include "mirror/art_method-inl.h"
#include "mirror/array.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "mirror/stack_trace_element.h"
#include "mirror/throwable.h"
#include "monitor.h"
#include "parsed_options.h"
#include "oat_file.h"
#include "quick/quick_method_frame_info.h"
#include "reflection.h"
#include "ScopedLocalRef.h"
#include "scoped_thread_state_change.h"
#include "signal_catcher.h"
#include "signal_set.h"
#include "handle_scope-inl.h"
#include "thread.h"
#include "thread_list.h"
#include "trace.h"
#include "transaction.h"
#include "profiler.h"
#include "UniquePtr.h"
#include "verifier/method_verifier.h"
#include "well_known_classes.h"

#include "JniConstants.h"  // Last to avoid LOG redefinition in ics-mr1-plus-art.

#ifdef HAVE_ANDROID_OS
#include "cutils/properties.h"
#endif

namespace art {

static constexpr bool kEnableJavaStackTraceHandler = true;
const char* Runtime::kDefaultInstructionSetFeatures =
    STRINGIFY(ART_DEFAULT_INSTRUCTION_SET_FEATURES);
Runtime* Runtime::instance_ = NULL;

Runtime::Runtime()
    : pre_allocated_OutOfMemoryError_(nullptr),
      resolution_method_(nullptr),
      imt_conflict_method_(nullptr),
      default_imt_(nullptr),
      instruction_set_(kNone),
      compiler_callbacks_(nullptr),
      is_zygote_(false),
      is_concurrent_gc_enabled_(true),
      is_explicit_gc_disabled_(false),
      default_stack_size_(0),
      heap_(nullptr),
      max_spins_before_thin_lock_inflation_(Monitor::kDefaultMaxSpinsBeforeThinLockInflation),
      monitor_list_(nullptr),
      monitor_pool_(nullptr),
      thread_list_(nullptr),
      intern_table_(nullptr),
      class_linker_(nullptr),
      signal_catcher_(nullptr),
      java_vm_(nullptr),
      fault_message_lock_("Fault message lock"),
      fault_message_(""),
      method_verifier_lock_("Method verifiers lock"),
      threads_being_born_(0),
      shutdown_cond_(new ConditionVariable("Runtime shutdown", *Locks::runtime_shutdown_lock_)),
      shutting_down_(false),
      shutting_down_started_(false),
      started_(false),
      finished_starting_(false),
      vfprintf_(nullptr),
      exit_(nullptr),
      abort_(nullptr),
      stats_enabled_(false),
      running_on_valgrind_(RUNNING_ON_VALGRIND > 0),
      profile_(false),
      profile_period_s_(0),
      profile_duration_s_(0),
      profile_interval_us_(0),
      profile_backoff_coefficient_(0),
      profile_start_immediately_(true),
      method_trace_(false),
      method_trace_file_size_(0),
      instrumentation_(),
      use_compile_time_class_path_(false),
      main_thread_group_(nullptr),
      system_thread_group_(nullptr),
      system_class_loader_(nullptr),
      dump_gc_performance_on_shutdown_(false),
      preinitialization_transaction_(nullptr),
      null_pointer_handler_(nullptr),
      suspend_handler_(nullptr),
      stack_overflow_handler_(nullptr),
      verify_(false) {
  for (int i = 0; i < Runtime::kLastCalleeSaveType; i++) {
    callee_save_methods_[i] = nullptr;
  }
}

Runtime::~Runtime() {
  if (dump_gc_performance_on_shutdown_) {
    // This can't be called from the Heap destructor below because it
    // could call RosAlloc::InspectAll() which needs the thread_list
    // to be still alive.
    heap_->DumpGcPerformanceInfo(LOG(INFO));
  }

  Thread* self = Thread::Current();
  {
    MutexLock mu(self, *Locks::runtime_shutdown_lock_);
    shutting_down_started_ = true;
    while (threads_being_born_ > 0) {
      shutdown_cond_->Wait(self);
    }
    shutting_down_ = true;
  }
  Trace::Shutdown();

  // Make sure to let the GC complete if it is running.
  heap_->WaitForGcToComplete(gc::kGcCauseBackground, self);
  heap_->DeleteThreadPool();

  // Make sure our internal threads are dead before we start tearing down things they're using.
  Dbg::StopJdwp();
  delete signal_catcher_;

  // Make sure all other non-daemon threads have terminated, and all daemon threads are suspended.
  delete thread_list_;
  delete monitor_list_;
  delete monitor_pool_;
  delete class_linker_;
  delete heap_;
  delete intern_table_;
  delete java_vm_;
  Thread::Shutdown();
  QuasiAtomic::Shutdown();
  verifier::MethodVerifier::Shutdown();
  // TODO: acquire a static mutex on Runtime to avoid racing.
  CHECK(instance_ == nullptr || instance_ == this);
  instance_ = nullptr;

  delete null_pointer_handler_;
  delete suspend_handler_;
  delete stack_overflow_handler_;
}

struct AbortState {
  void Dump(std::ostream& os) NO_THREAD_SAFETY_ANALYSIS {
    if (gAborting > 1) {
      os << "Runtime aborting --- recursively, so no thread-specific detail!\n";
      return;
    }
    gAborting++;
    os << "Runtime aborting...\n";
    if (Runtime::Current() == NULL) {
      os << "(Runtime does not yet exist!)\n";
      return;
    }
    Thread* self = Thread::Current();
    if (self == nullptr) {
      os << "(Aborting thread was not attached to runtime!)\n";
      DumpKernelStack(os, GetTid(), "  kernel: ", false);
      DumpNativeStack(os, GetTid(), "  native: ", nullptr);
    } else {
      os << "Aborting thread:\n";
      if (Locks::mutator_lock_->IsExclusiveHeld(self) || Locks::mutator_lock_->IsSharedHeld(self)) {
        DumpThread(os, self);
      } else {
        if (Locks::mutator_lock_->SharedTryLock(self)) {
          DumpThread(os, self);
          Locks::mutator_lock_->SharedUnlock(self);
        }
      }
    }
    DumpAllThreads(os, self);
  }

  void DumpThread(std::ostream& os, Thread* self) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    self->Dump(os);
    if (self->IsExceptionPending()) {
      ThrowLocation throw_location;
      mirror::Throwable* exception = self->GetException(&throw_location);
      os << "Pending exception " << PrettyTypeOf(exception)
          << " thrown by '" << throw_location.Dump() << "'\n"
          << exception->Dump();
    }
  }

  void DumpAllThreads(std::ostream& os, Thread* self) NO_THREAD_SAFETY_ANALYSIS {
    bool tll_already_held = Locks::thread_list_lock_->IsExclusiveHeld(self);
    bool ml_already_held = Locks::mutator_lock_->IsSharedHeld(self);
    if (!tll_already_held || !ml_already_held) {
      os << "Dumping all threads without appropriate locks held:"
          << (!tll_already_held ? " thread list lock" : "")
          << (!ml_already_held ? " mutator lock" : "")
          << "\n";
    }
    os << "All threads:\n";
    Runtime::Current()->GetThreadList()->DumpLocked(os);
  }
};

void Runtime::Abort() {
  gAborting++;  // set before taking any locks

  // Ensure that we don't have multiple threads trying to abort at once,
  // which would result in significantly worse diagnostics.
  MutexLock mu(Thread::Current(), *Locks::abort_lock_);

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

#if defined(__GLIBC__)
  // TODO: we ought to be able to use pthread_kill(3) here (or abort(3),
  // which POSIX defines in terms of raise(3), which POSIX defines in terms
  // of pthread_kill(3)). On Linux, though, libcorkscrew can't unwind through
  // libpthread, which means the stacks we dump would be useless. Calling
  // tgkill(2) directly avoids that.
  syscall(__NR_tgkill, getpid(), GetTid(), SIGABRT);
  // TODO: LLVM installs it's own SIGABRT handler so exit to be safe... Can we disable that in LLVM?
  // If not, we could use sigaction(3) before calling tgkill(2) and lose this call to exit(3).
  exit(1);
#else
  abort();
#endif
  // notreached
}

void Runtime::PreZygoteFork() {
  heap_->PreZygoteFork();
}

void Runtime::CallExitHook(jint status) {
  if (exit_ != NULL) {
    ScopedThreadStateChange tsc(Thread::Current(), kNative);
    exit_(status);
    LOG(WARNING) << "Exit hook returned instead of exiting!";
  }
}

void Runtime::SweepSystemWeaks(IsMarkedCallback* visitor, void* arg) {
  GetInternTable()->SweepInternTableWeaks(visitor, arg);
  GetMonitorList()->SweepMonitorList(visitor, arg);
  GetJavaVM()->SweepJniWeakGlobals(visitor, arg);
  Dbg::UpdateObjectPointers(visitor, arg);
}

bool Runtime::Create(const Options& options, bool ignore_unrecognized) {
  // TODO: acquire a static mutex on Runtime to avoid racing.
  if (Runtime::instance_ != NULL) {
    return false;
  }
  InitLogging(NULL);  // Calls Locks::Init() as a side effect.
  instance_ = new Runtime;
  if (!instance_->Init(options, ignore_unrecognized)) {
    delete instance_;
    instance_ = NULL;
    return false;
  }
  return true;
}

jobject CreateSystemClassLoader() {
  if (Runtime::Current()->UseCompileTimeClassPath()) {
    return NULL;
  }

  ScopedObjectAccess soa(Thread::Current());
  ClassLinker* cl = Runtime::Current()->GetClassLinker();

  StackHandleScope<3> hs(soa.Self());
  Handle<mirror::Class> class_loader_class(
      hs.NewHandle(soa.Decode<mirror::Class*>(WellKnownClasses::java_lang_ClassLoader)));
  CHECK(cl->EnsureInitialized(class_loader_class, true, true));

  mirror::ArtMethod* getSystemClassLoader =
      class_loader_class->FindDirectMethod("getSystemClassLoader", "()Ljava/lang/ClassLoader;");
  CHECK(getSystemClassLoader != NULL);

  JValue result = InvokeWithJValues(soa, nullptr, soa.EncodeMethod(getSystemClassLoader), nullptr);
  Handle<mirror::ClassLoader> class_loader(
      hs.NewHandle(down_cast<mirror::ClassLoader*>(result.GetL())));
  CHECK(class_loader.Get() != nullptr);
  JNIEnv* env = soa.Self()->GetJniEnv();
  ScopedLocalRef<jobject> system_class_loader(env,
                                              soa.AddLocalReference<jobject>(class_loader.Get()));
  CHECK(system_class_loader.get() != nullptr);

  soa.Self()->SetClassLoaderOverride(class_loader.Get());

  Handle<mirror::Class> thread_class(
      hs.NewHandle(soa.Decode<mirror::Class*>(WellKnownClasses::java_lang_Thread)));
  CHECK(cl->EnsureInitialized(thread_class, true, true));

  mirror::ArtField* contextClassLoader =
      thread_class->FindDeclaredInstanceField("contextClassLoader", "Ljava/lang/ClassLoader;");
  CHECK(contextClassLoader != NULL);

  // We can't run in a transaction yet.
  contextClassLoader->SetObject<false>(soa.Self()->GetPeer(), class_loader.Get());

  return env->NewGlobalRef(system_class_loader.get());
}

bool Runtime::Start() {
  VLOG(startup) << "Runtime::Start entering";

  // Restore main thread state to kNative as expected by native code.
  Thread* self = Thread::Current();
  self->TransitionFromRunnableToSuspended(kNative);

  started_ = true;

  // InitNativeMethods needs to be after started_ so that the classes
  // it touches will have methods linked to the oat file if necessary.
  InitNativeMethods();

  // Initialize well known thread group values that may be accessed threads while attaching.
  InitThreadGroups(self);

  Thread::FinishStartup();

  if (is_zygote_) {
    if (!InitZygote()) {
      return false;
    }
  } else {
    DidForkFromZygote();
  }

  StartDaemonThreads();

  system_class_loader_ = CreateSystemClassLoader();

  self->GetJniEnv()->locals.AssertEmpty();

  VLOG(startup) << "Runtime::Start exiting";

  finished_starting_ = true;

  if (profile_) {
    // User has asked for a profile using -Xprofile
    // Create the profile file if it doesn't exist.
    int fd = open(profile_output_filename_.c_str(), O_RDWR|O_CREAT|O_EXCL, 0660);
    if (fd >= 0) {
      close(fd);
    }
    StartProfiler(profile_output_filename_.c_str(), "");
  }

  return true;
}

void Runtime::EndThreadBirth() EXCLUSIVE_LOCKS_REQUIRED(Locks::runtime_shutdown_lock_) {
  DCHECK_GT(threads_being_born_, 0U);
  threads_being_born_--;
  if (shutting_down_started_ && threads_being_born_ == 0) {
    shutdown_cond_->Broadcast(Thread::Current());
  }
}

// Do zygote-mode-only initialization.
bool Runtime::InitZygote() {
  // zygote goes into its own process group
  setpgid(0, 0);

  // See storage config details at http://source.android.com/tech/storage/
  // Create private mount namespace shared by all children
  if (unshare(CLONE_NEWNS) == -1) {
    PLOG(WARNING) << "Failed to unshare()";
    return false;
  }

  // Mark rootfs as being a slave so that changes from default
  // namespace only flow into our children.
  if (mount("rootfs", "/", NULL, (MS_SLAVE | MS_REC), NULL) == -1) {
    PLOG(WARNING) << "Failed to mount() rootfs as MS_SLAVE";
    return false;
  }

  // Create a staging tmpfs that is shared by our children; they will
  // bind mount storage into their respective private namespaces, which
  // are isolated from each other.
  const char* target_base = getenv("EMULATED_STORAGE_TARGET");
  if (target_base != NULL) {
    if (mount("tmpfs", target_base, "tmpfs", MS_NOSUID | MS_NODEV,
              "uid=0,gid=1028,mode=0751") == -1) {
      LOG(WARNING) << "Failed to mount tmpfs to " << target_base;
      return false;
    }
  }

  return true;
}

void Runtime::DidForkFromZygote() {
  is_zygote_ = false;

  // Create the thread pool.
  heap_->CreateThreadPool();

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

bool Runtime::IsShuttingDown(Thread* self) {
  MutexLock mu(self, *Locks::runtime_shutdown_lock_);
  return IsShuttingDownLocked();
}

void Runtime::StartDaemonThreads() {
  VLOG(startup) << "Runtime::StartDaemonThreads entering";

  Thread* self = Thread::Current();

  // Must be in the kNative state for calling native methods.
  CHECK_EQ(self->GetState(), kNative);

  JNIEnv* env = self->GetJniEnv();
  env->CallStaticVoidMethod(WellKnownClasses::java_lang_Daemons,
                            WellKnownClasses::java_lang_Daemons_start);
  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    LOG(FATAL) << "Error starting java.lang.Daemons";
  }

  VLOG(startup) << "Runtime::StartDaemonThreads exiting";
}

bool Runtime::Init(const Options& raw_options, bool ignore_unrecognized) {
  CHECK_EQ(sysconf(_SC_PAGE_SIZE), kPageSize);

  UniquePtr<ParsedOptions> options(ParsedOptions::Create(raw_options, ignore_unrecognized));
  if (options.get() == NULL) {
    LOG(ERROR) << "Failed to parse options";
    return false;
  }
  VLOG(startup) << "Runtime::Init -verbose:startup enabled";

  QuasiAtomic::Startup();

  Monitor::Init(options->lock_profiling_threshold_, options->hook_is_sensitive_thread_);

  boot_class_path_string_ = options->boot_class_path_string_;
  class_path_string_ = options->class_path_string_;
  properties_ = options->properties_;

  compiler_callbacks_ = options->compiler_callbacks_;
  is_zygote_ = options->is_zygote_;
  is_explicit_gc_disabled_ = options->is_explicit_gc_disabled_;

  vfprintf_ = options->hook_vfprintf_;
  exit_ = options->hook_exit_;
  abort_ = options->hook_abort_;

  default_stack_size_ = options->stack_size_;
  stack_trace_file_ = options->stack_trace_file_;

  compiler_options_ = options->compiler_options_;
  image_compiler_options_ = options->image_compiler_options_;

  max_spins_before_thin_lock_inflation_ = options->max_spins_before_thin_lock_inflation_;

  monitor_list_ = new MonitorList;
  monitor_pool_ = MonitorPool::Create();
  thread_list_ = new ThreadList;
  intern_table_ = new InternTable;

  verify_ = options->verify_;

  if (options->interpreter_only_) {
    GetInstrumentation()->ForceInterpretOnly();
  }

  if (options->explicit_checks_ != (ParsedOptions::kExplicitSuspendCheck |
        ParsedOptions::kExplicitNullCheck |
        ParsedOptions::kExplicitStackOverflowCheck) || kEnableJavaStackTraceHandler) {
    fault_manager.Init();

    // These need to be in a specific order.  The null point check handler must be
    // after the suspend check and stack overflow check handlers.
    if ((options->explicit_checks_ & ParsedOptions::kExplicitSuspendCheck) == 0) {
      suspend_handler_ = new SuspensionHandler(&fault_manager);
    }

    if ((options->explicit_checks_ & ParsedOptions::kExplicitStackOverflowCheck) == 0) {
      stack_overflow_handler_ = new StackOverflowHandler(&fault_manager);
    }

    if ((options->explicit_checks_ & ParsedOptions::kExplicitNullCheck) == 0) {
      null_pointer_handler_ = new NullPointerHandler(&fault_manager);
    }

    if (kEnableJavaStackTraceHandler) {
      new JavaStackTraceHandler(&fault_manager);
    }
  }

  heap_ = new gc::Heap(options->heap_initial_size_,
                       options->heap_growth_limit_,
                       options->heap_min_free_,
                       options->heap_max_free_,
                       options->heap_target_utilization_,
                       options->foreground_heap_growth_multiplier_,
                       options->heap_maximum_size_,
                       options->image_,
                       options->image_isa_,
                       options->collector_type_,
                       options->background_collector_type_,
                       options->parallel_gc_threads_,
                       options->conc_gc_threads_,
                       options->low_memory_mode_,
                       options->long_pause_log_threshold_,
                       options->long_gc_log_threshold_,
                       options->ignore_max_footprint_,
                       options->use_tlab_,
                       options->verify_pre_gc_heap_,
                       options->verify_pre_sweeping_heap_,
                       options->verify_post_gc_heap_,
                       options->verify_pre_gc_rosalloc_,
                       options->verify_pre_sweeping_rosalloc_,
                       options->verify_post_gc_rosalloc_);

  dump_gc_performance_on_shutdown_ = options->dump_gc_performance_on_shutdown_;

  BlockSignals();
  InitPlatformSignalHandlers();

  java_vm_ = new JavaVMExt(this, options.get());

  Thread::Startup();

  // ClassLinker needs an attached thread, but we can't fully attach a thread without creating
  // objects. We can't supply a thread group yet; it will be fixed later. Since we are the main
  // thread, we do not get a java peer.
  Thread* self = Thread::Attach("main", false, NULL, false);
  CHECK_EQ(self->GetThreadId(), ThreadList::kMainThreadId);
  CHECK(self != NULL);

  // Set us to runnable so tools using a runtime can allocate and GC by default
  self->TransitionFromSuspendedToRunnable();

  // Now we're attached, we can take the heap locks and validate the heap.
  GetHeap()->EnableObjectValidation();

  CHECK_GE(GetHeap()->GetContinuousSpaces().size(), 1U);
  class_linker_ = new ClassLinker(intern_table_);
  if (GetHeap()->HasImageSpace()) {
    class_linker_->InitFromImage();
  } else {
    CHECK(options->boot_class_path_ != NULL);
    CHECK_NE(options->boot_class_path_->size(), 0U);
    class_linker_->InitFromCompiler(*options->boot_class_path_);
  }
  CHECK(class_linker_ != NULL);
  verifier::MethodVerifier::Init();

  method_trace_ = options->method_trace_;
  method_trace_file_ = options->method_trace_file_;
  method_trace_file_size_ = options->method_trace_file_size_;

  // Extract the profile options.
  // TODO: move into a Trace options struct?
  profile_period_s_ = options->profile_period_s_;
  profile_duration_s_ = options->profile_duration_s_;
  profile_interval_us_ = options->profile_interval_us_;
  profile_backoff_coefficient_ = options->profile_backoff_coefficient_;
  profile_start_immediately_ = options->profile_start_immediately_;
  profile_ = options->profile_;
  profile_output_filename_ = options->profile_output_filename_;
  // TODO: move this to just be an Trace::Start argument
  Trace::SetDefaultClockSource(options->profile_clock_source_);

  if (options->method_trace_) {
    Trace::Start(options->method_trace_file_.c_str(), -1, options->method_trace_file_size_, 0,
                 false, false, 0);
  }

  // Pre-allocate an OutOfMemoryError for the double-OOME case.
  self->ThrowNewException(ThrowLocation(), "Ljava/lang/OutOfMemoryError;",
                          "OutOfMemoryError thrown while trying to throw OutOfMemoryError; "
                          "no stack available");
  pre_allocated_OutOfMemoryError_ = self->GetException(NULL);
  self->ClearException();

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
  WellKnownClasses::Init(env);

  // Then set up the native methods provided by the runtime itself.
  RegisterRuntimeNativeMethods(env);

  // Then set up libcore, which is just a regular JNI library with a regular JNI_OnLoad.
  // Most JNI libraries can just use System.loadLibrary, but libcore can't because it's
  // the library that implements System.loadLibrary!
  {
    std::string mapped_name(StringPrintf(OS_SHARED_LIB_FORMAT_STR, "javacore"));
    std::string reason;
    self->TransitionFromSuspendedToRunnable();
    StackHandleScope<1> hs(self);
    auto class_loader(hs.NewHandle<mirror::ClassLoader>(nullptr));
    if (!instance_->java_vm_->LoadNativeLibrary(mapped_name, class_loader, &reason)) {
      LOG(FATAL) << "LoadNativeLibrary failed for \"" << mapped_name << "\": " << reason;
    }
    self->TransitionFromRunnableToSuspended(kNative);
  }

  // Initialize well known classes that may invoke runtime native methods.
  WellKnownClasses::LateInit(env);

  VLOG(startup) << "Runtime::InitNativeMethods exiting";
}

void Runtime::InitThreadGroups(Thread* self) {
  JNIEnvExt* env = self->GetJniEnv();
  ScopedJniEnvLocalRefState env_state(env);
  main_thread_group_ =
      env->NewGlobalRef(env->GetStaticObjectField(
          WellKnownClasses::java_lang_ThreadGroup,
          WellKnownClasses::java_lang_ThreadGroup_mainThreadGroup));
  CHECK(main_thread_group_ != NULL || IsCompiler());
  system_thread_group_ =
      env->NewGlobalRef(env->GetStaticObjectField(
          WellKnownClasses::java_lang_ThreadGroup,
          WellKnownClasses::java_lang_ThreadGroup_systemThreadGroup));
  CHECK(system_thread_group_ != NULL || IsCompiler());
}

jobject Runtime::GetMainThreadGroup() const {
  CHECK(main_thread_group_ != NULL || IsCompiler());
  return main_thread_group_;
}

jobject Runtime::GetSystemThreadGroup() const {
  CHECK(system_thread_group_ != NULL || IsCompiler());
  return system_thread_group_;
}

jobject Runtime::GetSystemClassLoader() const {
  CHECK(system_class_loader_ != NULL || IsCompiler());
  return system_class_loader_;
}

void Runtime::RegisterRuntimeNativeMethods(JNIEnv* env) {
#define REGISTER(FN) extern void FN(JNIEnv*); FN(env)
  // Register Throwable first so that registration of other native methods can throw exceptions
  REGISTER(register_java_lang_Throwable);
  REGISTER(register_dalvik_system_DexFile);
  REGISTER(register_dalvik_system_VMDebug);
  REGISTER(register_dalvik_system_VMRuntime);
  REGISTER(register_dalvik_system_VMStack);
  REGISTER(register_dalvik_system_ZygoteHooks);
  REGISTER(register_java_lang_Class);
  REGISTER(register_java_lang_DexCache);
  REGISTER(register_java_lang_Object);
  REGISTER(register_java_lang_Runtime);
  REGISTER(register_java_lang_String);
  REGISTER(register_java_lang_System);
  REGISTER(register_java_lang_Thread);
  REGISTER(register_java_lang_VMClassLoader);
  REGISTER(register_java_lang_ref_Reference);
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
  BaseMutex::DumpAll(os);
}

void Runtime::DumpLockHolders(std::ostream& os) {
  uint64_t mutator_lock_owner = Locks::mutator_lock_->GetExclusiveOwnerTid();
  pid_t thread_list_lock_owner = GetThreadList()->GetLockOwner();
  pid_t classes_lock_owner = GetClassLinker()->GetClassesLockOwner();
  pid_t dex_lock_owner = GetClassLinker()->GetDexLockOwner();
  if ((thread_list_lock_owner | classes_lock_owner | dex_lock_owner) != 0) {
    os << "Mutator lock exclusive owner tid: " << mutator_lock_owner << "\n"
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
    GetInstrumentation()->InstrumentQuickAllocEntryPoints();
  } else {
    GetInstrumentation()->UninstrumentQuickAllocEntryPoints();
  }
  stats_enabled_ = new_state;
}

void Runtime::ResetStats(int kinds) {
  GetStats()->Clear(kinds & 0xffff);
  // TODO: wouldn't it make more sense to clear _all_ threads' stats?
  Thread::Current()->GetStats()->Clear(kinds >> 16);
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
    return -1;  // unreachable
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

bool Runtime::AttachCurrentThread(const char* thread_name, bool as_daemon, jobject thread_group,
                                  bool create_peer) {
  bool success = Thread::Attach(thread_name, as_daemon, thread_group, create_peer) != NULL;
  if (thread_name == NULL) {
    LOG(WARNING) << *Thread::Current() << " attached without supplying a name";
  }
  return success;
}

void Runtime::DetachCurrentThread() {
  Thread* self = Thread::Current();
  if (self == NULL) {
    LOG(FATAL) << "attempting to detach thread that is not attached";
  }
  if (self->HasManagedStack()) {
    LOG(FATAL) << *Thread::Current() << " attempting to detach while still running code";
  }
  thread_list_->Unregister(self);
}

  mirror::Throwable* Runtime::GetPreAllocatedOutOfMemoryError() const {
  if (pre_allocated_OutOfMemoryError_ == NULL) {
    LOG(ERROR) << "Failed to return pre-allocated OOME";
  }
  return pre_allocated_OutOfMemoryError_;
}

void Runtime::VisitConstantRoots(RootCallback* callback, void* arg) {
  // Visit the classes held as static in mirror classes, these can be visited concurrently and only
  // need to be visited once per GC since they never change.
  mirror::ArtField::VisitRoots(callback, arg);
  mirror::ArtMethod::VisitRoots(callback, arg);
  mirror::Class::VisitRoots(callback, arg);
  mirror::StackTraceElement::VisitRoots(callback, arg);
  mirror::String::VisitRoots(callback, arg);
  mirror::Throwable::VisitRoots(callback, arg);
  // Visit all the primitive array types classes.
  mirror::PrimitiveArray<uint8_t>::VisitRoots(callback, arg);   // BooleanArray
  mirror::PrimitiveArray<int8_t>::VisitRoots(callback, arg);    // ByteArray
  mirror::PrimitiveArray<uint16_t>::VisitRoots(callback, arg);  // CharArray
  mirror::PrimitiveArray<double>::VisitRoots(callback, arg);    // DoubleArray
  mirror::PrimitiveArray<float>::VisitRoots(callback, arg);     // FloatArray
  mirror::PrimitiveArray<int32_t>::VisitRoots(callback, arg);   // IntArray
  mirror::PrimitiveArray<int64_t>::VisitRoots(callback, arg);   // LongArray
  mirror::PrimitiveArray<int16_t>::VisitRoots(callback, arg);   // ShortArray
}

void Runtime::VisitConcurrentRoots(RootCallback* callback, void* arg, VisitRootFlags flags) {
  intern_table_->VisitRoots(callback, arg, flags);
  class_linker_->VisitRoots(callback, arg, flags);
  Dbg::VisitRoots(callback, arg);
  if ((flags & kVisitRootFlagNewRoots) == 0) {
    // Guaranteed to have no new roots in the constant roots.
    VisitConstantRoots(callback, arg);
  }
}

void Runtime::VisitNonThreadRoots(RootCallback* callback, void* arg) {
  java_vm_->VisitRoots(callback, arg);
  if (pre_allocated_OutOfMemoryError_ != nullptr) {
    callback(reinterpret_cast<mirror::Object**>(&pre_allocated_OutOfMemoryError_), arg, 0,
             kRootVMInternal);
    DCHECK(pre_allocated_OutOfMemoryError_ != nullptr);
  }
  callback(reinterpret_cast<mirror::Object**>(&resolution_method_), arg, 0, kRootVMInternal);
  DCHECK(resolution_method_ != nullptr);
  if (HasImtConflictMethod()) {
    callback(reinterpret_cast<mirror::Object**>(&imt_conflict_method_), arg, 0, kRootVMInternal);
  }
  if (HasDefaultImt()) {
    callback(reinterpret_cast<mirror::Object**>(&default_imt_), arg, 0, kRootVMInternal);
  }
  for (int i = 0; i < Runtime::kLastCalleeSaveType; i++) {
    if (callee_save_methods_[i] != nullptr) {
      callback(reinterpret_cast<mirror::Object**>(&callee_save_methods_[i]), arg, 0,
               kRootVMInternal);
    }
  }
  {
    MutexLock mu(Thread::Current(), method_verifier_lock_);
    for (verifier::MethodVerifier* verifier : method_verifiers_) {
      verifier->VisitRoots(callback, arg);
    }
  }
  if (preinitialization_transaction_ != nullptr) {
    preinitialization_transaction_->VisitRoots(callback, arg);
  }
  instrumentation_.VisitRoots(callback, arg);
}

void Runtime::VisitNonConcurrentRoots(RootCallback* callback, void* arg) {
  thread_list_->VisitRoots(callback, arg);
  VisitNonThreadRoots(callback, arg);
}

void Runtime::VisitRoots(RootCallback* callback, void* arg, VisitRootFlags flags) {
  VisitNonConcurrentRoots(callback, arg);
  VisitConcurrentRoots(callback, arg, flags);
}

mirror::ObjectArray<mirror::ArtMethod>* Runtime::CreateDefaultImt(ClassLinker* cl) {
  Thread* self = Thread::Current();
  StackHandleScope<1> hs(self);
  Handle<mirror::ObjectArray<mirror::ArtMethod>> imtable(
      hs.NewHandle(cl->AllocArtMethodArray(self, 64)));
  mirror::ArtMethod* imt_conflict_method = Runtime::Current()->GetImtConflictMethod();
  for (size_t i = 0; i < static_cast<size_t>(imtable->GetLength()); i++) {
    imtable->Set<false>(i, imt_conflict_method);
  }
  return imtable.Get();
}

mirror::ArtMethod* Runtime::CreateImtConflictMethod() {
  Thread* self = Thread::Current();
  Runtime* runtime = Runtime::Current();
  ClassLinker* class_linker = runtime->GetClassLinker();
  StackHandleScope<1> hs(self);
  Handle<mirror::ArtMethod> method(hs.NewHandle(class_linker->AllocArtMethod(self)));
  method->SetDeclaringClass(mirror::ArtMethod::GetJavaLangReflectArtMethod());
  // TODO: use a special method for imt conflict method saves.
  method->SetDexMethodIndex(DexFile::kDexNoIndex);
  // When compiling, the code pointer will get set later when the image is loaded.
  if (runtime->IsCompiler()) {
    method->SetEntryPointFromPortableCompiledCode(nullptr);
    method->SetEntryPointFromQuickCompiledCode(nullptr);
  } else {
    method->SetEntryPointFromPortableCompiledCode(GetPortableImtConflictTrampoline(class_linker));
    method->SetEntryPointFromQuickCompiledCode(GetQuickImtConflictTrampoline(class_linker));
  }
  return method.Get();
}

mirror::ArtMethod* Runtime::CreateResolutionMethod() {
  Thread* self = Thread::Current();
  Runtime* runtime = Runtime::Current();
  ClassLinker* class_linker = runtime->GetClassLinker();
  StackHandleScope<1> hs(self);
  Handle<mirror::ArtMethod> method(hs.NewHandle(class_linker->AllocArtMethod(self)));
  method->SetDeclaringClass(mirror::ArtMethod::GetJavaLangReflectArtMethod());
  // TODO: use a special method for resolution method saves
  method->SetDexMethodIndex(DexFile::kDexNoIndex);
  // When compiling, the code pointer will get set later when the image is loaded.
  if (runtime->IsCompiler()) {
    method->SetEntryPointFromPortableCompiledCode(nullptr);
    method->SetEntryPointFromQuickCompiledCode(nullptr);
  } else {
    method->SetEntryPointFromPortableCompiledCode(GetPortableResolutionTrampoline(class_linker));
    method->SetEntryPointFromQuickCompiledCode(GetQuickResolutionTrampoline(class_linker));
  }
  return method.Get();
}

mirror::ArtMethod* Runtime::CreateCalleeSaveMethod(CalleeSaveType type) {
  Thread* self = Thread::Current();
  Runtime* runtime = Runtime::Current();
  ClassLinker* class_linker = runtime->GetClassLinker();
  StackHandleScope<1> hs(self);
  Handle<mirror::ArtMethod> method(hs.NewHandle(class_linker->AllocArtMethod(self)));
  method->SetDeclaringClass(mirror::ArtMethod::GetJavaLangReflectArtMethod());
  // TODO: use a special method for callee saves
  method->SetDexMethodIndex(DexFile::kDexNoIndex);
  method->SetEntryPointFromPortableCompiledCode(nullptr);
  method->SetEntryPointFromQuickCompiledCode(nullptr);
  DCHECK_NE(instruction_set_, kNone);
  return method.Get();
}

void Runtime::DisallowNewSystemWeaks() {
  monitor_list_->DisallowNewMonitors();
  intern_table_->DisallowNewInterns();
  java_vm_->DisallowNewWeakGlobals();
  Dbg::DisallowNewObjectRegistryObjects();
}

void Runtime::AllowNewSystemWeaks() {
  monitor_list_->AllowNewMonitors();
  intern_table_->AllowNewInterns();
  java_vm_->AllowNewWeakGlobals();
  Dbg::AllowNewObjectRegistryObjects();
}

void Runtime::SetInstructionSet(InstructionSet instruction_set) {
  instruction_set_ = instruction_set;
  if ((instruction_set_ == kThumb2) || (instruction_set_ == kArm)) {
    for (int i = 0; i != kLastCalleeSaveType; ++i) {
      CalleeSaveType type = static_cast<CalleeSaveType>(i);
      callee_save_method_frame_infos_[i] = arm::ArmCalleeSaveMethodFrameInfo(type);
    }
  } else if (instruction_set_ == kMips) {
    for (int i = 0; i != kLastCalleeSaveType; ++i) {
      CalleeSaveType type = static_cast<CalleeSaveType>(i);
      callee_save_method_frame_infos_[i] = mips::MipsCalleeSaveMethodFrameInfo(type);
    }
  } else if (instruction_set_ == kX86) {
    for (int i = 0; i != kLastCalleeSaveType; ++i) {
      CalleeSaveType type = static_cast<CalleeSaveType>(i);
      callee_save_method_frame_infos_[i] = x86::X86CalleeSaveMethodFrameInfo(type);
    }
  } else if (instruction_set_ == kX86_64) {
    for (int i = 0; i != kLastCalleeSaveType; ++i) {
      CalleeSaveType type = static_cast<CalleeSaveType>(i);
      callee_save_method_frame_infos_[i] = x86_64::X86_64CalleeSaveMethodFrameInfo(type);
    }
  } else if (instruction_set_ == kArm64) {
    for (int i = 0; i != kLastCalleeSaveType; ++i) {
      CalleeSaveType type = static_cast<CalleeSaveType>(i);
      callee_save_method_frame_infos_[i] = arm64::Arm64CalleeSaveMethodFrameInfo(type);
    }
  } else {
    UNIMPLEMENTED(FATAL) << instruction_set_;
  }
}

void Runtime::SetCalleeSaveMethod(mirror::ArtMethod* method, CalleeSaveType type) {
  DCHECK_LT(static_cast<int>(type), static_cast<int>(kLastCalleeSaveType));
  callee_save_methods_[type] = method;
}

const std::vector<const DexFile*>& Runtime::GetCompileTimeClassPath(jobject class_loader) {
  if (class_loader == NULL) {
    return GetClassLinker()->GetBootClassPath();
  }
  CHECK(UseCompileTimeClassPath());
  CompileTimeClassPaths::const_iterator it = compile_time_class_paths_.find(class_loader);
  CHECK(it != compile_time_class_paths_.end());
  return it->second;
}

void Runtime::SetCompileTimeClassPath(jobject class_loader,
                                      std::vector<const DexFile*>& class_path) {
  CHECK(!IsStarted());
  use_compile_time_class_path_ = true;
  compile_time_class_paths_.Put(class_loader, class_path);
}

void Runtime::AddMethodVerifier(verifier::MethodVerifier* verifier) {
  DCHECK(verifier != nullptr);
  MutexLock mu(Thread::Current(), method_verifier_lock_);
  method_verifiers_.insert(verifier);
}

void Runtime::RemoveMethodVerifier(verifier::MethodVerifier* verifier) {
  DCHECK(verifier != nullptr);
  MutexLock mu(Thread::Current(), method_verifier_lock_);
  auto it = method_verifiers_.find(verifier);
  CHECK(it != method_verifiers_.end());
  method_verifiers_.erase(it);
}

void Runtime::StartProfiler(const char* appDir, const char* procName) {
  BackgroundMethodSamplingProfiler::Start(profile_period_s_, profile_duration_s_, appDir,
      procName, profile_interval_us_, profile_backoff_coefficient_, profile_start_immediately_);
}

// Transaction support.
void Runtime::EnterTransactionMode(Transaction* transaction) {
  DCHECK(IsCompiler());
  DCHECK(transaction != nullptr);
  DCHECK(!IsActiveTransaction());
  preinitialization_transaction_ = transaction;
}

void Runtime::ExitTransactionMode() {
  DCHECK(IsCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transaction_ = nullptr;
}

void Runtime::RecordWriteField32(mirror::Object* obj, MemberOffset field_offset,
                                 uint32_t value, bool is_volatile) const {
  DCHECK(IsCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transaction_->RecordWriteField32(obj, field_offset, value, is_volatile);
}

void Runtime::RecordWriteField64(mirror::Object* obj, MemberOffset field_offset,
                                 uint64_t value, bool is_volatile) const {
  DCHECK(IsCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transaction_->RecordWriteField64(obj, field_offset, value, is_volatile);
}

void Runtime::RecordWriteFieldReference(mirror::Object* obj, MemberOffset field_offset,
                                        mirror::Object* value, bool is_volatile) const {
  DCHECK(IsCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transaction_->RecordWriteFieldReference(obj, field_offset, value, is_volatile);
}

void Runtime::RecordWriteArray(mirror::Array* array, size_t index, uint64_t value) const {
  DCHECK(IsCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transaction_->RecordWriteArray(array, index, value);
}

void Runtime::RecordStrongStringInsertion(mirror::String* s, uint32_t hash_code) const {
  DCHECK(IsCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transaction_->RecordStrongStringInsertion(s, hash_code);
}

void Runtime::RecordWeakStringInsertion(mirror::String* s, uint32_t hash_code) const {
  DCHECK(IsCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transaction_->RecordWeakStringInsertion(s, hash_code);
}

void Runtime::RecordStrongStringRemoval(mirror::String* s, uint32_t hash_code) const {
  DCHECK(IsCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transaction_->RecordStrongStringRemoval(s, hash_code);
}

void Runtime::RecordWeakStringRemoval(mirror::String* s, uint32_t hash_code) const {
  DCHECK(IsCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transaction_->RecordWeakStringRemoval(s, hash_code);
}

void Runtime::SetFaultMessage(const std::string& message) {
  MutexLock mu(Thread::Current(), fault_message_lock_);
  fault_message_ = message;
}

void Runtime::AddCurrentRuntimeFeaturesAsDex2OatArguments(std::vector<std::string>* argv)
    const {
  if (GetInstrumentation()->InterpretOnly()) {
    argv->push_back("--compiler-filter=interpret-only");
  }

  argv->push_back("--runtime-arg");
  std::string checkstr = "-implicit-checks";

  int nchecks = 0;
  char checksep = ':';

  if (!ExplicitNullChecks()) {
    checkstr += checksep;
    checksep = ',';
    checkstr += "null";
    ++nchecks;
  }
  if (!ExplicitSuspendChecks()) {
    checkstr += checksep;
    checksep = ',';
    checkstr += "suspend";
    ++nchecks;
  }

  if (!ExplicitStackOverflowChecks()) {
    checkstr += checksep;
    checksep = ',';
    checkstr += "stack";
    ++nchecks;
  }

  if (nchecks == 0) {
    checkstr += ":none";
  }
  argv->push_back(checkstr);

  // Make the dex2oat instruction set match that of the launching runtime. If we have multiple
  // architecture support, dex2oat may be compiled as a different instruction-set than that
  // currently being executed.
  std::string instruction_set("--instruction-set=");
  instruction_set += GetInstructionSetString(kRuntimeISA);
  argv->push_back(instruction_set);

  std::string features("--instruction-set-features=");
  features += GetDefaultInstructionSetFeatures();
  argv->push_back(features);
}

void Runtime::UpdateProfilerState(int state) {
  VLOG(profiler) << "Profiler state updated to " << state;
}
}  // namespace art
