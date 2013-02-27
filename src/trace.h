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

#include "base/macros.h"
#include "globals.h"
#include "instrumentation.h"
#include "os.h"
#include "safe_map.h"
#include "UniquePtr.h"

namespace art {

namespace mirror {
class AbstractMethod;
}  // namespace mirror
class Thread;

enum ProfilerClockSource {
  kProfilerClockSourceThreadCpu,
  kProfilerClockSourceWall,
  kProfilerClockSourceDual,  // Both wall and thread CPU clocks.
};

class Trace : public instrumentation::InstrumentationListener {
 public:
  enum TraceFlag {
    kTraceCountAllocs = 1,
  };

  static void SetDefaultClockSource(ProfilerClockSource clock_source);

  static void Start(const char* trace_filename, int trace_fd, int buffer_size, int flags,
                    bool direct_to_ddms)
  LOCKS_EXCLUDED(Locks::mutator_lock_,
                 Locks::thread_list_lock_,
                 Locks::thread_suspend_count_lock_,
                 Locks::trace_lock_);
  static void Stop() LOCKS_EXCLUDED(Locks::trace_lock_);
  static void Shutdown() LOCKS_EXCLUDED(Locks::trace_lock_);
  static bool IsMethodTracingActive() LOCKS_EXCLUDED(Locks::trace_lock_);

  bool UseWallClock();
  bool UseThreadCpuClock();

  virtual void MethodEntered(Thread* thread, mirror::Object* this_object,
                             const mirror::AbstractMethod* method, uint32_t dex_pc)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  virtual void MethodExited(Thread* thread, mirror::Object* this_object,
                            const mirror::AbstractMethod* method, uint32_t dex_pc,
                            const JValue& return_value)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  virtual void MethodUnwind(Thread* thread, const mirror::AbstractMethod* method, uint32_t dex_pc)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  virtual void DexPcMoved(Thread* thread, mirror::Object* this_object,
                          const mirror::AbstractMethod* method, uint32_t new_dex_pc)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  virtual void ExceptionCaught(Thread* thread, const ThrowLocation& throw_location,
                               mirror::AbstractMethod* catch_method, uint32_t catch_dex_pc,
                               mirror::Throwable* exception_object)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
 private:
  explicit Trace(File* trace_file, int buffer_size, int flags);

  void FinishTracing() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void LogMethodTraceEvent(Thread* thread, const mirror::AbstractMethod* method,
                           instrumentation::Instrumentation::InstrumentationEvent event);

  // Methods to output traced methods and threads.
  void GetVisitedMethods(size_t end_offset, std::set<mirror::AbstractMethod*>* visited_methods);
  void DumpMethodList(std::ostream& os, const std::set<mirror::AbstractMethod*>& visited_methods)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void DumpThreadList(std::ostream& os) LOCKS_EXCLUDED(Locks::thread_list_lock_);

  // Singleton instance of the Trace or NULL when no method tracing is active.
  static Trace* the_trace_ GUARDED_BY(Locks::trace_lock_);

  // The default profiler clock source.
  static ProfilerClockSource default_clock_source_;

  // Maps a thread to its clock base.
  SafeMap<Thread*, uint64_t> thread_clock_base_map_;

  // File to write trace data out to, NULL if direct to ddms.
  UniquePtr<File> trace_file_;

  // Buffer to store trace data.
  UniquePtr<uint8_t> buf_;

  // Flags enabling extra tracing of things such as alloc counts.
  const int flags_;

  const ProfilerClockSource clock_source_;

  // Size of buf_.
  const int buffer_size_;

  // Time trace was created.
  const uint64_t start_time_;

  // Offset into buf_.
  volatile int32_t cur_offset_;

  // Did we overflow the buffer recording traces?
  bool overflow_;

  DISALLOW_COPY_AND_ASSIGN(Trace);
};

}  // namespace art

#endif  // ART_SRC_TRACE_H_
