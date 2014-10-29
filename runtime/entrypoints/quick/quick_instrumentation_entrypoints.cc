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

#include "callee_save_frame.h"
#include "entrypoints/runtime_asm_entrypoints.h"
#include "instruction_set.h"
#include "instrumentation.h"
#include "mirror/art_method-inl.h"
#include "mirror/object-inl.h"
#include "runtime.h"
#include "thread-inl.h"

namespace art {

extern "C" const void* artInstrumentationMethodEntryFromCode(mirror::ArtMethod* method,
                                                             mirror::Object* this_object,
                                                             Thread* self,
                                                             uintptr_t lr)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);
  instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
  const void* result;
  if (instrumentation->IsDeoptimized(method)) {
    result = GetQuickToInterpreterBridge();
  } else {
    result = instrumentation->GetQuickCodeFor(method);
  }
  DCHECK(!Runtime::Current()->GetClassLinker()->IsQuickToInterpreterBridge(result));
  bool interpreter_entry = (result == GetQuickToInterpreterBridge());
  instrumentation->PushInstrumentationStackFrame(self, method->IsStatic() ? nullptr : this_object,
                                                 method, lr, interpreter_entry);
  CHECK(result != NULL) << PrettyMethod(method);
  return result;
}

extern "C" TwoWordReturn artInstrumentationMethodExitFromCode(Thread* self,
                                                              StackReference<mirror::ArtMethod>* sp,
                                                              uint64_t gpr_result,
                                                              uint64_t fpr_result)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  // Compute address of return PC and sanity check that it currently holds 0.
  size_t return_pc_offset = GetCalleeSaveReturnPcOffset(kRuntimeISA, Runtime::kRefsOnly);
  uintptr_t* return_pc = reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(sp) +
                                                      return_pc_offset);
  CHECK_EQ(*return_pc, 0U);

  // Pop the frame filling in the return pc. The low half of the return value is 0 when
  // deoptimization shouldn't be performed with the high-half having the return address. When
  // deoptimization should be performed the low half is zero and the high-half the address of the
  // deoptimization entry point.
  instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
  TwoWordReturn return_or_deoptimize_pc = instrumentation->PopInstrumentationStackFrame(
      self, return_pc, gpr_result, fpr_result);
  return return_or_deoptimize_pc;
}

}  // namespace art
