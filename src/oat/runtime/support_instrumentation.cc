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

#include "base/logging.h"
#include "instrumentation.h"
#include "runtime.h"
#include "thread.h"
#include "trace.h"

namespace art {

extern "C" const void* artInstrumentationMethodEntryFromCode(AbstractMethod* method, Thread* self,
                                                             AbstractMethod** sp, uintptr_t lr)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (*sp != NULL) {
    self->SetTopOfStack(sp, lr);
  }
  self->VerifyStack();
  Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
  // +1 as frame id's start at 1, +1 as we haven't yet built this method's frame.
  const size_t kFrameIdAdjust = 2;
  size_t frame_id = StackVisitor::ComputeNumFrames(self->GetManagedStack(),
                                                   self->GetInstrumentationStack()) + kFrameIdAdjust;
  InstrumentationStackFrame instrumentation_frame(method, lr, frame_id);
  self->PushInstrumentationStackFrame(instrumentation_frame);

  Trace* trace = instrumentation->GetTrace();
  if (trace != NULL) {
    trace->LogMethodTraceEvent(self, method, Trace::kMethodTraceEnter);
  }

  return instrumentation->GetSavedCodeFromMap(method);
}

extern "C" uint64_t artInstrumentationMethodExitFromCode(Thread* self, AbstractMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (*sp != NULL) {
    self->SetTopOfStack(sp, 0);
  }
  self->VerifyStack();

  /*
  // TODO: ComputeNumFrames currently fails here, so it's disabled.
  // +1 as frame id's start at 1, +1 as we want the called frame not the frame being returned into.
  const size_t kFrameIdAdjust = 2;
  size_t frame_id = StackVisitor::ComputeNumFrames(self->GetManagedStack(),
                                                   self->GetInstrumentationStack()) + kFrameIdAdjust;
  */
  InstrumentationStackFrame instrumentation_frame;
  instrumentation_frame = self->PopInstrumentationStackFrame();
  /*
  if (frame_id != instrumentation_frame.frame_id_) {
    LOG(ERROR) << "Expected frame_id=" << frame_id << " but found " << instrumentation_frame.frame_id_;
    StackVisitor::DescribeStack(self->GetManagedStack(), self->GetInstrumentationStack());
  }
  */
  Runtime* runtime = Runtime::Current();
  if (runtime->IsMethodTracingActive()) {
    Trace* trace = runtime->GetInstrumentation()->GetTrace();
    trace->LogMethodTraceEvent(self, instrumentation_frame.method_, Trace::kMethodTraceExit);
  }
  if (self->ReadFlag(kEnterInterpreter)) {
    return static_cast<uint64_t>(GetDeoptimizationEntryPoint()) |
        (static_cast<uint64_t>(instrumentation_frame.return_pc_) << 32);
  } else {
    return instrumentation_frame.return_pc_;
  }
}

}  // namespace art
