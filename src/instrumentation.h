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

#ifndef ART_SRC_INSTRUMENTATION_H_
#define ART_SRC_INSTRUMENTATION_H_

#include <ostream>
#include <set>
#include <string>

#include "file.h"
#include "globals.h"
#include "macros.h"
#include "safe_map.h"
#include "trace.h"
#include "UniquePtr.h"

namespace art {

class AbstractMethod;
class Thread;

uint32_t InstrumentationMethodUnwindFromCode(Thread* self);

struct InstrumentationStackFrame {
  InstrumentationStackFrame(AbstractMethod* method, uintptr_t return_pc)
      : method_(method), return_pc_(return_pc) {
  }

  AbstractMethod* method_;
  uintptr_t return_pc_;
};

class Instrumentation {
 public:
  Instrumentation() {}
  ~Instrumentation();

  // Replaces code of each method with a pointer to a stub for method tracing.
  void InstallStubs();

  // Restores original code for each method and fixes the return values of each thread's stack.
  void UninstallStubs() LOCKS_EXCLUDED(Locks::thread_list_lock_);

  const void* GetSavedCodeFromMap(const AbstractMethod* method);
  void SaveAndUpdateCode(AbstractMethod* method);
  void ResetSavedCode(AbstractMethod* method);

  Trace* GetTrace() const;
  void SetTrace(Trace* trace);
  void RemoveTrace();

 private:
  void AddSavedCodeToMap(const AbstractMethod* method, const void* code);
  void RemoveSavedCodeFromMap(const AbstractMethod* method);

  // Maps a method to its original code pointer.
  SafeMap<const AbstractMethod*, const void*> saved_code_map_;

  Trace* trace_;

  DISALLOW_COPY_AND_ASSIGN(Instrumentation);
};

}  // namespace art

#endif  // ART_SRC_INSTRUMENTATION_H_
