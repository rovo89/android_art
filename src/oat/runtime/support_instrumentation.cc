/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include "instrumentation.h"
#include "runtime.h"
#include "thread.h"
#include "trace.h"

namespace art {

extern "C" const void* artInstrumentationMethodEntryFromCode(AbstractMethod* method, Thread* self,
                                                             uintptr_t lr) {
  Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
  Trace* trace = instrumentation->GetTrace();
  InstrumentationStackFrame instrumentation_frame = InstrumentationStackFrame(method, lr);
  self->PushInstrumentationStackFrame(instrumentation_frame);

  trace->LogMethodTraceEvent(self, method, Trace::kMethodTraceEnter);

  return instrumentation->GetSavedCodeFromMap(method);
}

extern "C" uintptr_t artInstrumentationMethodExitFromCode() {
  Trace* trace = Runtime::Current()->GetInstrumentation()->GetTrace();
  InstrumentationStackFrame instrumentation_frame = Thread::Current()->PopInstrumentationStackFrame();
  AbstractMethod* method = instrumentation_frame.method_;
  uintptr_t lr = instrumentation_frame.return_pc_;

  trace->LogMethodTraceEvent(Thread::Current(), method, Trace::kMethodTraceExit);

  return lr;
}

}  // namespace art
