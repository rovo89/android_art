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

#include "runtime.h"
#include "thread.h"
#include "trace.h"

namespace art {

extern "C" const void* artTraceMethodEntryFromCode(AbstractMethod* method, Thread* self, uintptr_t lr) {
  Trace* tracer = Runtime::Current()->GetTracer();
  TraceStackFrame trace_frame = TraceStackFrame(method, lr);
  self->PushTraceStackFrame(trace_frame);

  tracer->LogMethodTraceEvent(self, method, Trace::kMethodTraceEnter);

  return tracer->GetSavedCodeFromMap(method);
}

extern "C" uintptr_t artTraceMethodExitFromCode() {
  Trace* tracer = Runtime::Current()->GetTracer();
  TraceStackFrame trace_frame = Thread::Current()->PopTraceStackFrame();
  AbstractMethod* method = trace_frame.method_;
  uintptr_t lr = trace_frame.return_pc_;

  tracer->LogMethodTraceEvent(Thread::Current(), method, Trace::kMethodTraceExit);

  return lr;
}

}  // namespace art
