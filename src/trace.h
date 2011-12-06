// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_TRACE_H_
#define ART_SRC_TRACE_H_

#include <map>

#include "globals.h"
#include "macros.h"

namespace art {

class Method;

struct TraceStackFrame {
  TraceStackFrame(Method* method, uintptr_t return_pc)
      : method_(method), return_pc_(return_pc) {
  }

  Method* method_;
  uintptr_t return_pc_;
};

class Trace {
 public:
  static void Start(const char* trace_filename, int trace_fd, int buffer_size, int flags, bool direct_to_ddms);
  static void Stop();

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

  static bool method_tracing_active_;

  // Maps a method to its original code pointer
  static std::map<const Method*, const void*> saved_code_map_;

  DISALLOW_COPY_AND_ASSIGN(Trace);
};

}  // namespace art

#endif  // ART_SRC_TRACE_H_
