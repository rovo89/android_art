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

#ifndef ART_SRC_RUNTIME_H_
#define ART_SRC_RUNTIME_H_

#include <jni.h>
#include <stdio.h>

#include <iosfwd>
#include <string>
#include <utility>
#include <vector>

#include "globals.h"
#include "heap.h"
#include "instruction_set.h"
#include "macros.h"
#include "runtime_stats.h"
#include "safe_map.h"
#include "stringpiece.h"

#if defined(ART_USE_LLVM_COMPILER)
#include "compiler_llvm/procedure_linkage_table.h"
#endif

namespace art {

template<class T> class PrimitiveArray;
typedef PrimitiveArray<int8_t> ByteArray;
class ClassLinker;
class ClassLoader;
class DexFile;
class Heap;
class InternTable;
class JavaVMExt;
class Method;
class MonitorList;
class SignalCatcher;
class String;
class ThreadList;
class Throwable;
class Trace;

class Runtime {
 public:
  typedef std::vector<std::pair<std::string, const void*> > Options;

  class ParsedOptions {
   public:
    // returns null if problem parsing and ignore_unrecognized is false
    static ParsedOptions* Create(const Options& options, bool ignore_unrecognized);

    const std::vector<const DexFile*>* boot_class_path_;
    std::string boot_class_path_string_;
    std::string class_path_string_;
    std::string host_prefix_;
    std::string image_;
    bool check_jni_;
    std::string jni_trace_;
    bool is_compiler_;
    bool is_zygote_;
    bool is_concurrent_gc_enabled_;
    size_t heap_initial_size_;
    size_t heap_maximum_size_;
    size_t heap_growth_limit_;
    size_t stack_size_;
    size_t jni_globals_max_;
    size_t lock_profiling_threshold_;
    std::string stack_trace_file_;
    bool method_trace_;
    std::string method_trace_file_;
    size_t method_trace_file_size_;
    bool (*hook_is_sensitive_thread_)();
    jint (*hook_vfprintf_)(FILE* stream, const char* format, va_list ap);
    void (*hook_exit_)(jint status);
    void (*hook_abort_)();
    std::vector<std::string> properties_;

   private:
    ParsedOptions() {}
  };

  // Creates and initializes a new runtime.
  static Runtime* Create(const Options& options, bool ignore_unrecognized);

  bool IsCompiler() const {
    return is_compiler_;
  }

  bool IsZygote() const {
    return is_zygote_;
  }

  bool IsConcurrentGcEnabled() const {
    return is_concurrent_gc_enabled_;
  }

  const std::string& GetHostPrefix() const {
    DCHECK(!IsStarted());
    return host_prefix_;
  }

  // Starts a runtime, which may cause threads to be started and code to run.
  void Start();

  bool IsShuttingDown() const {
    return shutting_down_;
  }

  bool IsStarted() const {
    return started_;
  }

  bool IsFinishedStarting() const {
    return finished_starting_;
  }

  static Runtime* Current() {
    return instance_;
  }

  // Aborts semi-cleanly. Used in the implementation of LOG(FATAL), which most
  // callers should prefer.
  // This isn't marked ((noreturn)) because then gcc will merge multiple calls
  // in a single function together. This reduces code size slightly, but means
  // that the native stack trace we get may point at the wrong call site.
  static void Abort();

  // Returns the "main" ThreadGroup, used when attaching user threads.
  jobject GetMainThreadGroup() const {
    return main_thread_group_;
  }

  // Returns the "system" ThreadGroup, used when attaching our internal threads.
  jobject GetSystemThreadGroup() const {
    return system_thread_group_;
  }

  // Attaches the calling native thread to the runtime.
  void AttachCurrentThread(const char* thread_name, bool as_daemon, jobject thread_group);

  void CallExitHook(jint status);

  // Detaches the current native thread from the runtime.
  void DetachCurrentThread();

  void DumpForSigQuit(std::ostream& os);
  void DumpLockHolders(std::ostream& os);

  ~Runtime();

  const std::string& GetBootClassPathString() const {
    return boot_class_path_string_;
  }

  const std::string& GetClassPathString() const {
    return class_path_string_;
  }

  ClassLinker* GetClassLinker() const {
    return class_linker_;
  }

  size_t GetDefaultStackSize() const {
    return default_stack_size_;
  }

  Heap* GetHeap() const {
    return heap_;
  }

  InternTable* GetInternTable() const {
    return intern_table_;
  }

  JavaVMExt* GetJavaVM() const {
    return java_vm_;
  }

  MonitorList* GetMonitorList() const {
    return monitor_list_;
  }

  Throwable* GetPreAllocatedOutOfMemoryError() {
    return pre_allocated_OutOfMemoryError_;
  }

  const std::vector<std::string>& GetProperties() const {
    return properties_;
  }

  ThreadList* GetThreadList() const {
    return thread_list_;
  }

  const char* GetVersion() const {
    return "2.0.0";
  }

  void VisitRoots(Heap::RootVisitor* visitor, void* arg) const;

  bool HasJniDlsymLookupStub() const {
    return jni_stub_array_ != NULL;
  }

  ByteArray* GetJniDlsymLookupStub() const {
    CHECK(HasJniDlsymLookupStub());
    return jni_stub_array_;
  }

  void SetJniDlsymLookupStub(ByteArray* jni_stub_array);

  bool HasAbstractMethodErrorStubArray() const {
    return abstract_method_error_stub_array_ != NULL;
  }

  ByteArray* GetAbstractMethodErrorStubArray() const {
    CHECK(abstract_method_error_stub_array_ != NULL);
    return abstract_method_error_stub_array_;
  }

  void SetAbstractMethodErrorStubArray(ByteArray* abstract_method_error_stub_array);

  enum TrampolineType {
    kStaticMethod,
    kUnknownMethod,
    kLastTrampolineMethodType  // Value used for iteration
  };

  bool HasResolutionStubArray(TrampolineType type) const {
    return resolution_stub_array_[type] != NULL;
  }

  ByteArray* GetResolutionStubArray(TrampolineType type) const {
    CHECK(HasResolutionStubArray(type));
    DCHECK_LT(static_cast<int>(type), static_cast<int>(kLastTrampolineMethodType));
    return resolution_stub_array_[type];
  }

  void SetResolutionStubArray(ByteArray* resolution_stub_array, TrampolineType type);

  // Returns a special method that calls into a trampoline for runtime method resolution
  Method* GetResolutionMethod() const {
    CHECK(HasResolutionMethod());
    return resolution_method_;
  }

  bool HasResolutionMethod() const {
    return resolution_method_ != NULL;
  }

  void SetResolutionMethod(Method* method) {
    resolution_method_ = method;
  }

  Method* CreateResolutionMethod();

  // Returns a special method that describes all callee saves being spilled to the stack.
  enum CalleeSaveType {
    kSaveAll,
    kRefsOnly,
    kRefsAndArgs,
    kLastCalleeSaveType  // Value used for iteration
  };

  bool HasCalleeSaveMethod(CalleeSaveType type) const {
    return callee_save_methods_[type] != NULL;
  }

  Method* GetCalleeSaveMethod(CalleeSaveType type) const {
    CHECK(HasCalleeSaveMethod(type));
    return callee_save_methods_[type];
  }

  void SetCalleeSaveMethod(Method* method, CalleeSaveType type);

  Method* CreateCalleeSaveMethod(InstructionSet instruction_set, CalleeSaveType type);

  Method* CreateRefOnlyCalleeSaveMethod(InstructionSet instruction_set);
  Method* CreateRefAndArgsCalleeSaveMethod(InstructionSet instruction_set);

  int32_t GetStat(int kind);

  RuntimeStats* GetStats() {
    return &stats_;
  }

  bool HasStatsEnabled() const {
    return stats_enabled_;
  }

  void ResetStats(int kinds);

  void SetStatsEnabled(bool new_state);

  void DidForkFromZygote();

  void EnableMethodTracing(Trace* tracer);
  void DisableMethodTracing();

  bool IsMethodTracingActive() const {
    return tracer_ != NULL;
  }

  Trace* GetTracer() const {
    CHECK(IsMethodTracingActive());
    return tracer_;
  }

  bool UseCompileTimeClassPath() const {
    return use_compile_time_class_path_;
  }

  const std::vector<const DexFile*>& GetCompileTimeClassPath(const ClassLoader* class_loader);
  void SetCompileTimeClassPath(const ClassLoader* class_loader, std::vector<const DexFile*>& class_path);

 private:
  static void InitPlatformSignalHandlers();

  Runtime();

  void BlockSignals();

  bool Init(const Options& options, bool ignore_unrecognized);
  void InitNativeMethods();
  void InitThreadGroups(Thread* self);
  void RegisterRuntimeNativeMethods(JNIEnv* env);

  void StartDaemonThreads();
  void StartSignalCatcher();

  // A pointer to the active runtime or NULL.
  static Runtime* instance_;

  bool is_compiler_;
  bool is_zygote_;
  bool is_concurrent_gc_enabled_;

  // The host prefix is used during cross compilation. It is removed
  // from the start of host paths such as:
  //    $ANDROID_PRODUCT_OUT/system/framework/boot.oat
  // to produce target paths such as
  //    /system/framework/boot.oat
  // Similarly it is prepended to target paths to arrive back at a
  // host past. In both cases this is necessary because image and oat
  // files embedded expect paths of dependent files (an image points
  // to an oat file and an oat files to one or more dex files). These
  // files contain the expected target path.
  std::string host_prefix_;

  std::string boot_class_path_string_;
  std::string class_path_string_;
  std::vector<std::string> properties_;

  // The default stack size for managed threads created by the runtime.
  size_t default_stack_size_;

  Heap* heap_;

  MonitorList* monitor_list_;

  ThreadList* thread_list_;

  InternTable* intern_table_;

  ClassLinker* class_linker_;

  SignalCatcher* signal_catcher_;
  std::string stack_trace_file_;

  JavaVMExt* java_vm_;

  Throwable* pre_allocated_OutOfMemoryError_;

  ByteArray* jni_stub_array_;

  ByteArray* abstract_method_error_stub_array_;

  ByteArray* resolution_stub_array_[kLastTrampolineMethodType];

  Method* callee_save_methods_[kLastCalleeSaveType];

  Method* resolution_method_;

  // As returned by ClassLoader.getSystemClassLoader()
  ClassLoader* system_class_loader_;

  bool shutting_down_;
  bool started_;

  // New flag added which tells us if the runtime has finished starting. If
  // this flag is set then the Daemon threads are created and the class loader
  // is created. This flag is needed for knowing if its safe to request CMS.
  bool finished_starting_;

  // Hooks supported by JNI_CreateJavaVM
  jint (*vfprintf_)(FILE* stream, const char* format, va_list ap);
  void (*exit_)(jint status);
  void (*abort_)();

  bool stats_enabled_;
  RuntimeStats stats_;

  bool method_trace_;
  std::string method_trace_file_;
  size_t method_trace_file_size_;
  Trace* tracer_;

  typedef SafeMap<const ClassLoader*, std::vector<const DexFile*> > CompileTimeClassPaths;
  CompileTimeClassPaths compile_time_class_paths_;
  bool use_compile_time_class_path_;

  jobject main_thread_group_;
  jobject system_thread_group_;
#if defined(ART_USE_LLVM_COMPILER)
  compiler_llvm::ProcedureLinkageTable plt_;
#endif

  DISALLOW_COPY_AND_ASSIGN(Runtime);
};

}  // namespace art

#endif  // ART_SRC_RUNTIME_H_
