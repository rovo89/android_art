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
#ifdef __linux__
#include <linux/fs.h>
#endif

#include <signal.h>
#include <sys/syscall.h>
#include <valgrind.h>

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
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
#include "elf_file.h"
#include "fault_handler.h"
#include "gc/accounting/card_table-inl.h"
#include "gc/heap.h"
#include "gc/space/image_space.h"
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
#include "native_bridge_art_interface.h"
#include "parsed_options.h"
#include "oat_file.h"
#include "os.h"
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
    : instruction_set_(kNone),
      compiler_callbacks_(nullptr),
      is_zygote_(false),
      must_relocate_(false),
      is_concurrent_gc_enabled_(true),
      is_explicit_gc_disabled_(false),
      dex2oat_enabled_(true),
      image_dex2oat_enabled_(true),
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
      profiler_started_(false),
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
      verify_(false),
      target_sdk_version_(0),
      implicit_null_checks_(false),
      implicit_so_checks_(false),
      implicit_suspend_checks_(false),
      native_bridge_art_callbacks_({GetMethodShorty, GetNativeMethodCount, GetNativeMethods}) {
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
  // Shut down background profiler before the runtime exits.
  if (profiler_started_) {
    BackgroundMethodSamplingProfiler::Shutdown();
  }

  // Shutdown the fault manager if it was initialized.
  fault_manager.Shutdown();

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
    Runtime* runtime = Runtime::Current();
    if (runtime != nullptr) {
      ThreadList* thread_list = runtime->GetThreadList();
      if (thread_list != nullptr) {
        bool tll_already_held = Locks::thread_list_lock_->IsExclusiveHeld(self);
        bool ml_already_held = Locks::mutator_lock_->IsSharedHeld(self);
        if (!tll_already_held || !ml_already_held) {
          os << "Dumping all threads without appropriate locks held:"
              << (!tll_already_held ? " thread list lock" : "")
              << (!ml_already_held ? " mutator lock" : "")
              << "\n";
        }
        os << "All threads:\n";
        thread_list->DumpLocked(os);
      }
    }
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
}

bool Runtime::Create(const RuntimeOptions& options, bool ignore_unrecognized) {
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

std::string Runtime::GetPatchoatExecutable() const {
  if (!patchoat_executable_.empty()) {
    return patchoat_executable_;
  }
  std::string patchoat_executable_(GetAndroidRoot());
  patchoat_executable_ += (kIsDebugBuild ? "/bin/patchoatd" : "/bin/patchoat");
  return patchoat_executable_;
}

std::string Runtime::GetCompilerExecutable() const {
  if (!compiler_executable_.empty()) {
    return compiler_executable_;
  }
  std::string compiler_executable(GetAndroidRoot());
  compiler_executable += (kIsDebugBuild ? "/bin/dex2oatd" : "/bin/dex2oat");
  return compiler_executable;
}

bool Runtime::Start() {
  VLOG(startup) << "Runtime::Start entering";

  // Restore main thread state to kNative as expected by native code.
  Thread* self = Thread::Current();

  self->TransitionFromRunnableToSuspended(kNative);

  started_ = true;

  if (!IsImageDex2OatEnabled() || !Runtime::Current()->GetHeap()->HasImageSpace()) {
    ScopedObjectAccess soa(Thread::Current());
    StackHandleScope<1> hs(soa.Self());
    auto klass(hs.NewHandle<mirror::Class>(mirror::Class::GetJavaLangClass()));
    class_linker_->EnsureInitialized(klass, true, true);
  }

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
    DidForkFromZygote(NativeBridgeAction::kInitialize);
  }

  StartDaemonThreads();

  system_class_loader_ = CreateSystemClassLoader();

  {
    ScopedObjectAccess soa(self);
    self->GetJniEnv()->locals.AssertEmpty();
  }

  VLOG(startup) << "Runtime::Start exiting";
  finished_starting_ = true;

  if (profiler_options_.IsEnabled() && !profile_output_filename_.empty()) {
    // User has asked for a profile using -Xenable-profiler.
    // Create the profile file if it doesn't exist.
    int fd = open(profile_output_filename_.c_str(), O_RDWR|O_CREAT|O_EXCL, 0660);
    if (fd >= 0) {
      close(fd);
    } else if (errno != EEXIST) {
      LOG(INFO) << "Failed to access the profile file. Profiler disabled.";
      return true;
    }
    StartProfiler(profile_output_filename_.c_str());
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
#ifdef __linux__
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
#else
  UNIMPLEMENTED(FATAL);
  return false;
#endif
}

void Runtime::DidForkFromZygote(NativeBridgeAction action) {
  is_zygote_ = false;

  switch (action) {
    case NativeBridgeAction::kUnload:
      android::UnloadNativeBridge();
      break;

    case NativeBridgeAction::kInitialize:
      android::InitializeNativeBridge();
      break;
  }

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

static bool OpenDexFilesFromImage(const std::vector<std::string>& dex_filenames,
                                  const std::string& image_location,
                                  std::vector<const DexFile*>& dex_files,
                                  size_t* failures) {
  std::string system_filename;
  bool has_system = false;
  std::string cache_filename_unused;
  bool dalvik_cache_exists_unused;
  bool has_cache_unused;
  bool found_image = gc::space::ImageSpace::FindImageFilename(image_location.c_str(),
                                                              kRuntimeISA,
                                                              &system_filename,
                                                              &has_system,
                                                              &cache_filename_unused,
                                                              &dalvik_cache_exists_unused,
                                                              &has_cache_unused);
  *failures = 0;
  if (!found_image || !has_system) {
    return false;
  }
  std::string error_msg;
  // We are falling back to non-executable use of the oat file because patching failed, presumably
  // due to lack of space.
  std::string oat_filename = ImageHeader::GetOatLocationFromImageLocation(system_filename.c_str());
  std::string oat_location = ImageHeader::GetOatLocationFromImageLocation(image_location.c_str());
  std::unique_ptr<File> file(OS::OpenFileForReading(oat_filename.c_str()));
  if (file.get() == nullptr) {
    return false;
  }
  std::unique_ptr<ElfFile> elf_file(ElfFile::Open(file.release(), false, false, &error_msg));
  if (elf_file.get() == nullptr) {
    return false;
  }
  std::unique_ptr<OatFile> oat_file(OatFile::OpenWithElfFile(elf_file.release(), oat_location,
                                                             &error_msg));
  if (oat_file.get() == nullptr) {
    LOG(INFO) << "Unable to use '" << oat_filename << "' because " << error_msg;
    return false;
  }

  for (const OatFile::OatDexFile* oat_dex_file : oat_file->GetOatDexFiles()) {
    if (oat_dex_file == nullptr) {
      *failures += 1;
      continue;
    }
    const DexFile* dex_file = oat_dex_file->OpenDexFile(&error_msg);
    if (dex_file == nullptr) {
      *failures += 1;
    } else {
      dex_files.push_back(dex_file);
    }
  }
  Runtime::Current()->GetClassLinker()->RegisterOatFile(oat_file.release());
  return true;
}


static size_t OpenDexFiles(const std::vector<std::string>& dex_filenames,
                           const std::string& image_location,
                           std::vector<const DexFile*>& dex_files) {
  size_t failure_count = 0;
  if (!image_location.empty() && OpenDexFilesFromImage(dex_filenames, image_location, dex_files,
                                                       &failure_count)) {
    return failure_count;
  }
  failure_count = 0;
  for (size_t i = 0; i < dex_filenames.size(); i++) {
    const char* dex_filename = dex_filenames[i].c_str();
    std::string error_msg;
    if (!OS::FileExists(dex_filename)) {
      LOG(WARNING) << "Skipping non-existent dex file '" << dex_filename << "'";
      continue;
    }
    if (!DexFile::Open(dex_filename, dex_filename, &error_msg, &dex_files)) {
      LOG(WARNING) << "Failed to open .dex from file '" << dex_filename << "': " << error_msg;
      ++failure_count;
    }
  }
  return failure_count;
}

bool Runtime::Init(const RuntimeOptions& raw_options, bool ignore_unrecognized) {
  CHECK_EQ(sysconf(_SC_PAGE_SIZE), kPageSize);

  std::unique_ptr<ParsedOptions> options(ParsedOptions::Create(raw_options, ignore_unrecognized));
  if (options.get() == nullptr) {
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
  patchoat_executable_ = options->patchoat_executable_;
  must_relocate_ = options->must_relocate_;
  is_zygote_ = options->is_zygote_;
  is_explicit_gc_disabled_ = options->is_explicit_gc_disabled_;
  dex2oat_enabled_ = options->dex2oat_enabled_;
  image_dex2oat_enabled_ = options->image_dex2oat_enabled_;

  vfprintf_ = options->hook_vfprintf_;
  exit_ = options->hook_exit_;
  abort_ = options->hook_abort_;

  default_stack_size_ = options->stack_size_;
  stack_trace_file_ = options->stack_trace_file_;

  compiler_executable_ = options->compiler_executable_;
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

  heap_ = new gc::Heap(options->heap_initial_size_,
                       options->heap_growth_limit_,
                       options->heap_min_free_,
                       options->heap_max_free_,
                       options->heap_target_utilization_,
                       options->foreground_heap_growth_multiplier_,
                       options->heap_maximum_size_,
                       options->heap_non_moving_space_capacity_,
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
                       options->verify_post_gc_rosalloc_,
                       options->use_homogeneous_space_compaction_for_oom_,
                       options->min_interval_homogeneous_space_compaction_by_oom_);

  dump_gc_performance_on_shutdown_ = options->dump_gc_performance_on_shutdown_;

  BlockSignals();
  InitPlatformSignalHandlers();

  // Change the implicit checks flags based on runtime architecture.
  switch (kRuntimeISA) {
    case kArm:
    case kThumb2:
    case kX86:
    case kArm64:
    case kX86_64:
      implicit_null_checks_ = true;
      implicit_so_checks_ = true;
      break;
    default:
      // Keep the defaults.
      break;
  }

  if (implicit_null_checks_ || implicit_so_checks_ || implicit_suspend_checks_) {
    fault_manager.Init();

    // These need to be in a specific order.  The null point check handler must be
    // after the suspend check and stack overflow check handlers.
    if (implicit_suspend_checks_) {
      suspend_handler_ = new SuspensionHandler(&fault_manager);
    }

    if (implicit_so_checks_) {
      stack_overflow_handler_ = new StackOverflowHandler(&fault_manager);
    }

    if (implicit_null_checks_) {
      null_pointer_handler_ = new NullPointerHandler(&fault_manager);
    }

    if (kEnableJavaStackTraceHandler) {
      new JavaStackTraceHandler(&fault_manager);
    }
  }

  java_vm_ = new JavaVMExt(this, options.get());

  Thread::Startup();

  // ClassLinker needs an attached thread, but we can't fully attach a thread without creating
  // objects. We can't supply a thread group yet; it will be fixed later. Since we are the main
  // thread, we do not get a java peer.
  Thread* self = Thread::Attach("main", false, nullptr, false);
  CHECK_EQ(self->GetThreadId(), ThreadList::kMainThreadId);
  CHECK(self != nullptr);

  // Set us to runnable so tools using a runtime can allocate and GC by default
  self->TransitionFromSuspendedToRunnable();

  // Now we're attached, we can take the heap locks and validate the heap.
  GetHeap()->EnableObjectValidation();

  CHECK_GE(GetHeap()->GetContinuousSpaces().size(), 1U);
  class_linker_ = new ClassLinker(intern_table_);
  if (GetHeap()->HasImageSpace()) {
    class_linker_->InitFromImage();
    if (kIsDebugBuild) {
      GetHeap()->GetImageSpace()->VerifyImageAllocations();
    }
  } else if (!IsCompiler() || !image_dex2oat_enabled_) {
    std::vector<std::string> dex_filenames;
    Split(boot_class_path_string_, ':', dex_filenames);
    std::vector<const DexFile*> boot_class_path;
    OpenDexFiles(dex_filenames, options->image_, boot_class_path);
    class_linker_->InitWithoutImage(boot_class_path);
    // TODO: Should we move the following to InitWithoutImage?
    SetInstructionSet(kRuntimeISA);
    for (int i = 0; i < Runtime::kLastCalleeSaveType; i++) {
      Runtime::CalleeSaveType type = Runtime::CalleeSaveType(i);
      if (!HasCalleeSaveMethod(type)) {
        SetCalleeSaveMethod(CreateCalleeSaveMethod(type), type);
      }
    }
  } else {
    CHECK(options->boot_class_path_ != nullptr);
    CHECK_NE(options->boot_class_path_->size(), 0U);
    class_linker_->InitWithoutImage(*options->boot_class_path_);
  }
  CHECK(class_linker_ != nullptr);
  verifier::MethodVerifier::Init();

  method_trace_ = options->method_trace_;
  method_trace_file_ = options->method_trace_file_;
  method_trace_file_size_ = options->method_trace_file_size_;

  profile_output_filename_ = options->profile_output_filename_;
  profiler_options_ = options->profiler_options_;

  // TODO: move this to just be an Trace::Start argument
  Trace::SetDefaultClockSource(options->profile_clock_source_);

  if (options->method_trace_) {
    ScopedThreadStateChange tsc(self, kWaitingForMethodTracingStart);
    Trace::Start(options->method_trace_file_.c_str(), -1, options->method_trace_file_size_, 0,
                 false, false, 0);
  }

  // Pre-allocate an OutOfMemoryError for the double-OOME case.
  self->ThrowNewException(ThrowLocation(), "Ljava/lang/OutOfMemoryError;",
                          "OutOfMemoryError thrown while trying to throw OutOfMemoryError; "
                          "no stack available");
  pre_allocated_OutOfMemoryError_ = GcRoot<mirror::Throwable>(self->GetException(NULL));
  self->ClearException();

  // Pre-allocate a NoClassDefFoundError for the common case of failing to find a system class
  // ahead of checking the application's class loader.
  self->ThrowNewException(ThrowLocation(), "Ljava/lang/NoClassDefFoundError;",
                          "Class not found using the boot class loader; no stack available");
  pre_allocated_NoClassDefFoundError_ = GcRoot<mirror::Throwable>(self->GetException(NULL));
  self->ClearException();

  // Look for a native bridge.
  //
  // The intended flow here is, in the case of a running system:
  //
  // Runtime::Init() (zygote):
  //   LoadNativeBridge -> dlopen from cmd line parameter.
  //  |
  //  V
  // Runtime::Start() (zygote):
  //   No-op wrt native bridge.
  //  |
  //  | start app
  //  V
  // DidForkFromZygote(action)
  //   action = kUnload -> dlclose native bridge.
  //   action = kInitialize -> initialize library
  //
  //
  // The intended flow here is, in the case of a simple dalvikvm call:
  //
  // Runtime::Init():
  //   LoadNativeBridge -> dlopen from cmd line parameter.
  //  |
  //  V
  // Runtime::Start():
  //   DidForkFromZygote(kInitialize) -> try to initialize any native bridge given.
  //   No-op wrt native bridge.
  native_bridge_library_filename_ = options->native_bridge_library_filename_;
  android::LoadNativeBridge(native_bridge_library_filename_.c_str(), &native_bridge_art_callbacks_);
  VLOG(startup) << "Runtime::Setup native bridge library: "
                << (native_bridge_library_filename_.empty() ?
                    "(empty)" : native_bridge_library_filename_);

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
  REGISTER(register_java_lang_ref_FinalizerReference);
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
  TrackedAllocators::Dump(os);
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

void Runtime::SetStatsEnabled(bool new_state, bool suspended) {
  if (new_state == true) {
    GetStats()->Clear(~0);
    // TODO: wouldn't it make more sense to clear _all_ threads' stats?
    Thread::Current()->GetStats()->Clear(~0);
    GetInstrumentation()->InstrumentQuickAllocEntryPoints(suspended);
  } else {
    GetInstrumentation()->UninstrumentQuickAllocEntryPoints(suspended);
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
  return Thread::Attach(thread_name, as_daemon, thread_group, create_peer) != NULL;
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

mirror::Throwable* Runtime::GetPreAllocatedOutOfMemoryError() {
  mirror::Throwable* oome = pre_allocated_OutOfMemoryError_.Read();
  if (oome == nullptr) {
    LOG(ERROR) << "Failed to return pre-allocated OOME";
  }
  return oome;
}

mirror::Throwable* Runtime::GetPreAllocatedNoClassDefFoundError() {
  mirror::Throwable* ncdfe = pre_allocated_NoClassDefFoundError_.Read();
  if (ncdfe == nullptr) {
    LOG(ERROR) << "Failed to return pre-allocated NoClassDefFoundError";
  }
  return ncdfe;
}

void Runtime::VisitConstantRoots(RootCallback* callback, void* arg) {
  // Visit the classes held as static in mirror classes, these can be visited concurrently and only
  // need to be visited once per GC since they never change.
  mirror::ArtField::VisitRoots(callback, arg);
  mirror::ArtMethod::VisitRoots(callback, arg);
  mirror::Class::VisitRoots(callback, arg);
  mirror::Reference::VisitRoots(callback, arg);
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
  if ((flags & kVisitRootFlagNewRoots) == 0) {
    // Guaranteed to have no new roots in the constant roots.
    VisitConstantRoots(callback, arg);
  }
}

void Runtime::VisitNonThreadRoots(RootCallback* callback, void* arg) {
  java_vm_->VisitRoots(callback, arg);
  if (!pre_allocated_OutOfMemoryError_.IsNull()) {
    pre_allocated_OutOfMemoryError_.VisitRoot(callback, arg, 0, kRootVMInternal);
    DCHECK(!pre_allocated_OutOfMemoryError_.IsNull());
  }
  resolution_method_.VisitRoot(callback, arg, 0, kRootVMInternal);
  DCHECK(!resolution_method_.IsNull());
  if (!pre_allocated_NoClassDefFoundError_.IsNull()) {
    pre_allocated_NoClassDefFoundError_.VisitRoot(callback, arg, 0, kRootVMInternal);
    DCHECK(!pre_allocated_NoClassDefFoundError_.IsNull());
  }
  if (HasImtConflictMethod()) {
    imt_conflict_method_.VisitRoot(callback, arg, 0, kRootVMInternal);
  }
  if (HasDefaultImt()) {
    default_imt_.VisitRoot(callback, arg, 0, kRootVMInternal);
  }
  for (int i = 0; i < Runtime::kLastCalleeSaveType; i++) {
    if (!callee_save_methods_[i].IsNull()) {
      callee_save_methods_[i].VisitRoot(callback, arg, 0, kRootVMInternal);
    }
  }
  verifier::MethodVerifier::VisitStaticRoots(callback, arg);
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
#if defined(ART_USE_PORTABLE_COMPILER)
    method->SetEntryPointFromPortableCompiledCode(nullptr);
#endif
    method->SetEntryPointFromQuickCompiledCode(nullptr);
  } else {
#if defined(ART_USE_PORTABLE_COMPILER)
    method->SetEntryPointFromPortableCompiledCode(class_linker->GetPortableImtConflictTrampoline());
#endif
    method->SetEntryPointFromQuickCompiledCode(class_linker->GetQuickImtConflictTrampoline());
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
#if defined(ART_USE_PORTABLE_COMPILER)
    method->SetEntryPointFromPortableCompiledCode(nullptr);
#endif
    method->SetEntryPointFromQuickCompiledCode(nullptr);
  } else {
#if defined(ART_USE_PORTABLE_COMPILER)
    method->SetEntryPointFromPortableCompiledCode(class_linker->GetPortableResolutionTrampoline());
#endif
    method->SetEntryPointFromQuickCompiledCode(class_linker->GetQuickResolutionTrampoline());
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
#if defined(ART_USE_PORTABLE_COMPILER)
  method->SetEntryPointFromPortableCompiledCode(nullptr);
#endif
  method->SetEntryPointFromQuickCompiledCode(nullptr);
  DCHECK_NE(instruction_set_, kNone);
  return method.Get();
}

void Runtime::DisallowNewSystemWeaks() {
  monitor_list_->DisallowNewMonitors();
  intern_table_->DisallowNewInterns();
  java_vm_->DisallowNewWeakGlobals();
}

void Runtime::AllowNewSystemWeaks() {
  monitor_list_->AllowNewMonitors();
  intern_table_->AllowNewInterns();
  java_vm_->AllowNewWeakGlobals();
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
  callee_save_methods_[type] = GcRoot<mirror::ArtMethod>(method);
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

void Runtime::StartProfiler(const char* profile_output_filename) {
  profile_output_filename_ = profile_output_filename;
  profiler_started_ =
    BackgroundMethodSamplingProfiler::Start(profile_output_filename_, profiler_options_);
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

void Runtime::RecordStrongStringInsertion(mirror::String* s) const {
  DCHECK(IsCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transaction_->RecordStrongStringInsertion(s);
}

void Runtime::RecordWeakStringInsertion(mirror::String* s) const {
  DCHECK(IsCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transaction_->RecordWeakStringInsertion(s);
}

void Runtime::RecordStrongStringRemoval(mirror::String* s) const {
  DCHECK(IsCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transaction_->RecordStrongStringRemoval(s);
}

void Runtime::RecordWeakStringRemoval(mirror::String* s) const {
  DCHECK(IsCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transaction_->RecordWeakStringRemoval(s);
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
