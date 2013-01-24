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

#include "base/macros.h"
#include "safe_map.h"

#include <stdint.h>

namespace art {

namespace mirror {
class AbstractMethod;
}
class Thread;
class Trace;

uint32_t InstrumentationMethodUnwindFromCode(Thread* self);

struct InstrumentationStackFrame {
  InstrumentationStackFrame() : method_(NULL), return_pc_(0), frame_id_(0) {}
  InstrumentationStackFrame(mirror::AbstractMethod* method, uintptr_t return_pc, size_t frame_id)
      : method_(method), return_pc_(return_pc), frame_id_(frame_id) {
  }
  mirror::AbstractMethod* method_;
  uintptr_t return_pc_;
  size_t frame_id_;
};

class Instrumentation {
 public:
  Instrumentation() {}
  ~Instrumentation();

  // Replaces code of each method with a pointer to a stub for method tracing.
  void InstallStubs() LOCKS_EXCLUDED(Locks::thread_list_lock_);

  // Restores original code for each method and fixes the return values of each thread's stack.
  void UninstallStubs() LOCKS_EXCLUDED(Locks::thread_list_lock_);

  const void* GetSavedCodeFromMap(const mirror::AbstractMethod* method);
  void SaveAndUpdateCode(mirror::AbstractMethod* method);
  void ResetSavedCode(mirror::AbstractMethod* method);

  Trace* GetTrace() const;
  void SetTrace(Trace* trace);
  void RemoveTrace();

 private:
  void AddSavedCodeToMap(const mirror::AbstractMethod* method, const void* code);
  void RemoveSavedCodeFromMap(const mirror::AbstractMethod* method);

  // Maps a method to its original code pointer.
  SafeMap<const mirror::AbstractMethod*, const void*> saved_code_map_;

  Trace* trace_;

  DISALLOW_COPY_AND_ASSIGN(Instrumentation);
};

}  // namespace art

#endif  // ART_SRC_INSTRUMENTATION_H_
