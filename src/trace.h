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
#include "UniquePtr.h"

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

  void LogMethodTraceEvent(Thread* self, const Method* method, TraceEvent event);

  void AddSavedCodeToMap(const Method* method, const void* code);
  void RemoveSavedCodeFromMap(const Method* method);
  const void* GetSavedCodeFromMap(const Method* method);

  void SaveAndUpdateCode(Method* method, const void* new_code);
  void ResetSavedCode(Method* method);

 private:
  explicit Trace(File* trace_file, int buffer_size)
      : trace_file_(trace_file), buf_(new uint8_t[buffer_size]()), overflow_(false), buffer_size_(buffer_size),
        start_time_(0), trace_version_(0), record_size_(0), cur_offset_(0) {
  }

  void BeginTracing();
  void FinishTracing();

  // Replaces code of each method with a pointer to a stub for method tracing.
  void InstallStubs();

  // Restores original code for each method and fixes the return values of each thread's stack.
  void UninstallStubs();

  // Methods to output traced methods and threads.
  void GetVisitedMethods(size_t end_offset);
  void DumpMethodList(std::ostream& os);
  void DumpThreadList(std::ostream& os);

  // Maps a method to its original code pointer.
  std::map<const Method*, const void*> saved_code_map_;

  // Set of methods visited by the profiler.
  std::set<const Method*> visited_methods_;

  // Maps a thread to its clock base.
  std::map<Thread*, uint64_t> thread_clock_base_map_;

  // File to write trace data out to, NULL if direct to ddms.
  UniquePtr<File> trace_file_;

  // Buffer to store trace data.
  UniquePtr<uint8_t> buf_;

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
