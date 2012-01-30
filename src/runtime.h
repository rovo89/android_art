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

#include <stdio.h>

#include <iosfwd>
#include <string>
#include <utility>
#include <vector>

#include <jni.h>

#include "constants.h"
#include "heap.h"
#include "globals.h"
#include "macros.h"
#include "runtime_stats.h"
#include "stringpiece.h"

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
class Trace;

class Runtime {
 public:

  typedef std::vector<std::pair<StringPiece, const void*> > Options;

  class ParsedOptions {
   public:
    // returns null if problem parsing and ignore_unrecognized is false
    static ParsedOptions* Create(const Options& options, bool ignore_unrecognized);

    std::string boot_class_path_;
    std::string class_path_;
    std::string host_prefix_;
    std::vector<std::string> images_;
    bool check_jni_;
    std::string jni_trace_;
    bool is_zygote_;
    size_t heap_initial_size_;
    size_t heap_maximum_size_;
    size_t heap_growth_limit_;
    size_t stack_size_;
    size_t jni_globals_max_;
    size_t lock_profiling_threshold_;
    std::string stack_trace_file_;
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

  bool IsZygote() const {
    return is_zygote_;
  }

  const std::string& GetHostPrefix() const {
    DCHECK(!IsStarted());
    return host_prefix_;
  }

  // Starts a runtime, which may cause threads to be started and code to run.
  void Start();

  bool IsShuttingDown() const;
  bool IsStarted() const;

  static Runtime* Current() {
    return instance_;
  }

  // Compiles a dex file.
  static void Compile(const StringPiece& filename);

  // Aborts semi-cleanly. Used in the implementation of LOG(FATAL), which most
  // callers should prefer.
  // This isn't marked ((noreturn)) because then gcc will merge multiple calls
  // in a single function together. This reduces code size slightly, but means
  // that the native stack trace we get may point at the wrong call site.
  static void Abort(const char* file, int line);

  // Attaches the current native thread to the runtime.
  void AttachCurrentThread(const char* name, bool as_daemon);

  void CallExitHook(jint status);

  // Detaches the current native thread from the runtime.
  void DetachCurrentThread();

  void Dump(std::ostream& os);
  void DumpLockHolders(std::ostream& os);

  ~Runtime();

  const std::string& GetBootClassPath() const {
    return boot_class_path_;
  }

  ClassLinker* GetClassLinker() const {
    return class_linker_;
  }

  const std::string& GetClassPath() const {
    return class_path_;
  }

  size_t GetDefaultStackSize() const {
    return default_stack_size_;
  }

  InternTable* GetInternTable() const {
    return intern_table_;
  }

  JavaVMExt* GetJavaVM() const {
    return java_vm_;
  }

  const std::vector<std::string>& GetProperties() const {
    return properties_;
  }

  MonitorList* GetMonitorList() const {
    return monitor_list_;
  }

  ThreadList* GetThreadList() const {
    return thread_list_;
  }

  const char* GetVersion() const {
    return "2.0.0";
  }

  void VisitRoots(Heap::RootVisitor* visitor, void* arg) const;

  bool HasJniDlsymLookupStub() const;
  ByteArray* GetJniDlsymLookupStub() const;
  void SetJniDlsymLookupStub(ByteArray* jni_stub_array);

  bool HasAbstractMethodErrorStubArray() const;
  ByteArray* GetAbstractMethodErrorStubArray() const;
  void SetAbstractMethodErrorStubArray(ByteArray* abstract_method_error_stub_array);

  enum TrampolineType {
    kInstanceMethod,
    kStaticMethod,
    kUnknownMethod,
    kLastTrampolineMethodType  // Value used for iteration
  };
  static TrampolineType GetTrampolineType(Method* method);
  bool HasResolutionStubArray(TrampolineType type) const;
  ByteArray* GetResolutionStubArray(TrampolineType type) const;
  void SetResolutionStubArray(ByteArray* resolution_stub_array, TrampolineType type);

  // Returns a special method that describes all callee saves being spilled to the stack.
  enum CalleeSaveType {
    kSaveAll,
    kRefsOnly,
    kRefsAndArgs,
    kLastCalleeSaveType  // Value used for iteration
  };
  Method* CreateCalleeSaveMethod(InstructionSet instruction_set, CalleeSaveType type);
  bool HasCalleeSaveMethod(CalleeSaveType type) const;
  Method* GetCalleeSaveMethod(CalleeSaveType type) const;
  void SetCalleeSaveMethod(Method* method, CalleeSaveType type);

  Method* CreateRefOnlyCalleeSaveMethod(InstructionSet instruction_set);
  Method* CreateRefAndArgsCalleeSaveMethod(InstructionSet instruction_set);

  int32_t GetStat(int kind);

  RuntimeStats* GetStats();

  bool HasStatsEnabled() const {
    return stats_enabled_;
  }

  void ResetStats(int kinds);

  void SetStatsEnabled(bool new_state);

  void DidForkFromZygote();

  void EnableMethodTracing(Trace* tracer);
  void DisableMethodTracing();
  bool IsMethodTracingActive() const;
  Trace* GetTracer() const;

 private:
  static void PlatformAbort(const char*, int);

  Runtime();

  void BlockSignals();

  bool Init(const Options& options, bool ignore_unrecognized);
  void InitNativeMethods();
  void RegisterRuntimeNativeMethods(JNIEnv* env);

  void StartDaemonThreads();
  void StartSignalCatcher();

  bool is_zygote_;

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

  std::string boot_class_path_;
  std::string class_path_;
  std::vector<std::string> properties_;

  // The default stack size for managed threads created by the runtime.
  size_t default_stack_size_;

  MonitorList* monitor_list_;

  ThreadList* thread_list_;

  InternTable* intern_table_;

  ClassLinker* class_linker_;

  SignalCatcher* signal_catcher_;
  std::string stack_trace_file_;

  JavaVMExt* java_vm_;

  ByteArray* jni_stub_array_;

  ByteArray* abstract_method_error_stub_array_;

  ByteArray* resolution_stub_array_[kLastTrampolineMethodType];

  Method* callee_save_method_[kLastCalleeSaveType];

  // As returned by ClassLoader.getSystemClassLoader()
  ClassLoader* system_class_loader_;

  bool shutting_down_;
  bool started_;

  // Hooks supported by JNI_CreateJavaVM
  jint (*vfprintf_)(FILE* stream, const char* format, va_list ap);
  void (*exit_)(jint status);
  void (*abort_)();

  bool stats_enabled_;
  RuntimeStats stats_;

  Trace* tracer_;

  // A pointer to the active runtime or NULL.
  static Runtime* instance_;

  static Mutex abort_lock_;

  DISALLOW_COPY_AND_ASSIGN(Runtime);
};

}  // namespace art

#endif  // ART_SRC_RUNTIME_H_
