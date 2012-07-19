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

#ifndef ART_SRC_TRACE_H_
#define ART_SRC_TRACE_H_

#include <ostream>
#include <set>
#include <string>

#include "file.h"
#include "globals.h"
#include "macros.h"
#include "safe_map.h"
#include "UniquePtr.h"

namespace art {

class Method;
class Thread;

uint32_t TraceMethodUnwindFromCode(Thread* self);

struct TraceStackFrame {
  TraceStackFrame(Method* method, uintptr_t return_pc)
      : method_(method), return_pc_(return_pc) {
  }

  Method* method_;
  uintptr_t return_pc_;
};

enum ProfilerClockSource {
  kProfilerClockSourceThreadCpu,
  kProfilerClockSourceWall,
  kProfilerClockSourceDual,
};

class Trace {
 public:
  enum TraceEvent {
    kMethodTraceEnter = 0,
    kMethodTraceExit = 1,
    kMethodTraceUnwind = 2,
  };

  enum TraceFlag {
    kTraceCountAllocs = 1,
  };

  static void SetDefaultClockSource(ProfilerClockSource clock_source);

  static void Start(const char* trace_filename, int trace_fd, int buffer_size, int flags, bool direct_to_ddms);
  static void Stop();
  static void Shutdown() NO_THREAD_SAFETY_ANALYSIS;  // TODO: implement appropriate locking.

  bool UseWallClock();
  bool UseThreadCpuClock();

  void LogMethodTraceEvent(Thread* self, const Method* method, TraceEvent event);

  void AddSavedCodeToMap(const Method* method, const void* code);
  void RemoveSavedCodeFromMap(const Method* method);
  const void* GetSavedCodeFromMap(const Method* method);

  void SaveAndUpdateCode(Method* method);
  void ResetSavedCode(Method* method);

 private:
  explicit Trace(File* trace_file, int buffer_size, int flags);

  void BeginTracing();
  void FinishTracing() SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);

  // Replaces code of each method with a pointer to a stub for method tracing.
  void InstallStubs();

  // Restores original code for each method and fixes the return values of each thread's stack.
  void UninstallStubs() LOCKS_EXCLUDED(GlobalSynchronization::thread_list_lock_);

  // Methods to output traced methods and threads.
  void GetVisitedMethods(size_t end_offset);
  void DumpMethodList(std::ostream& os) SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);
  void DumpThreadList(std::ostream& os) LOCKS_EXCLUDED(GlobalSynchronization::thread_list_lock_);

  // Maps a method to its original code pointer.
  SafeMap<const Method*, const void*> saved_code_map_;

  // Set of methods visited by the profiler.
  std::set<const Method*> visited_methods_;

  // Maps a thread to its clock base.
  SafeMap<Thread*, uint64_t> thread_clock_base_map_;

  // File to write trace data out to, NULL if direct to ddms.
  UniquePtr<File> trace_file_;

  // Buffer to store trace data.
  UniquePtr<uint8_t> buf_;

  // Flags enabling extra tracing of things such as alloc counts.
  int flags_;

  ProfilerClockSource clock_source_;

  bool overflow_;
  int buffer_size_;
  uint64_t start_time_;
  uint16_t trace_version_;
  uint16_t record_size_;

  volatile int32_t cur_offset_;

  DISALLOW_COPY_AND_ASSIGN(Trace);
};

}  // namespace art

#endif  // ART_SRC_TRACE_H_
