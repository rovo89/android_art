// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_TRACE_H_
#define ART_SRC_TRACE_H_

#include <map>
#include <ostream>
#include <set>
#include <string>

#include "file.h"
#include "globals.h"
#include "macros.h"

namespace art {

class Method;
class Thread;

struct TraceStackFrame {
  TraceStackFrame(Method* method, uintptr_t return_pc)
      : method_(method), return_pc_(return_pc) {
  }

  Method* method_;
  uintptr_t return_pc_;
};

class Trace {
 public:

  enum TraceEvent {
    kMethodTraceEnter = 0,
    kMethodTraceExit = 1,
    kMethodTraceUnwind = 2,
  };

  static void Start(const char* trace_filename, int trace_fd, int buffer_size, int flags, bool direct_to_ddms);
  static void Stop();

  static void LogMethodTraceEvent(Thread* self, const Method* method, TraceEvent event);

  static bool IsMethodTracingActive();
  static void SetMethodTracingActive(bool value);

  static void AddSavedCodeToMap(const Method* method, const void* code);
  static void RemoveSavedCodeFromMap(const Method* method);
  static const void* GetSavedCodeFromMap(const Method* method);

  static void SaveAndUpdateCode(Method* method, const void* new_code);
  static void ResetSavedCode(Method* method);

 private:
  // Replaces code of each method with a pointer to a stub for method tracing.
  static void InstallStubs();

  // Restores original code for each method and fixes the return values of each thread's stack.
  static void UninstallStubs();

  // Methods to output traced methods and threads.
  static void GetVisitedMethods(size_t end_offset);
  static void DumpMethodList(std::ostream& os);
  static void DumpThreadList(std::ostream& os);

  static bool method_tracing_active_;

  // Maps a method to its original code pointer
  static std::map<const Method*, const void*> saved_code_map_;

  // Set of methods visited by the profiler
  static std::set<const Method*> visited_methods_;

  // Maps a thread to its clock base
  static std::map<Thread*, uint64_t> thread_clock_base_map_;

  static uint8_t* buf_;
  static File* trace_file_;
  static bool direct_to_ddms_;
  static int buffer_size_;
  static uint64_t start_time_;
  static bool overflow_;
  static uint16_t trace_version_;
  static uint16_t record_size_;
  static volatile int32_t cur_offset_;

  DISALLOW_COPY_AND_ASSIGN(Trace);
};

}  // namespace art

#endif  // ART_SRC_TRACE_H_
