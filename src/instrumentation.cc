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

#include "instrumentation.h"

#include <sys/uio.h>

#include "base/unix_file/fd_file.h"
#include "class_linker.h"
#include "debugger.h"
#include "dex_cache.h"
#if !defined(ART_USE_LLVM_COMPILER)
#include "oat/runtime/oat_support_entrypoints.h"
#endif
#include "object_utils.h"
#include "os.h"
#include "scoped_thread_state_change.h"
#include "thread.h"
#include "thread_list.h"
#include "trace.h"

namespace art {

static bool InstallStubsClassVisitor(Class* klass, void*)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
  for (size_t i = 0; i < klass->NumDirectMethods(); i++) {
    AbstractMethod* method = klass->GetDirectMethod(i);
    if (instrumentation->GetSavedCodeFromMap(method) == NULL) {
      instrumentation->SaveAndUpdateCode(method);
    }
  }

  for (size_t i = 0; i < klass->NumVirtualMethods(); i++) {
    AbstractMethod* method = klass->GetVirtualMethod(i);
    if (instrumentation->GetSavedCodeFromMap(method) == NULL) {
      instrumentation->SaveAndUpdateCode(method);
    }
  }
  return true;
}

static bool UninstallStubsClassVisitor(Class* klass, void*)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
  for (size_t i = 0; i < klass->NumDirectMethods(); i++) {
    AbstractMethod* method = klass->GetDirectMethod(i);
    if (instrumentation->GetSavedCodeFromMap(method) != NULL) {
      instrumentation->ResetSavedCode(method);
    }
  }

  for (size_t i = 0; i < klass->NumVirtualMethods(); i++) {
    AbstractMethod* method = klass->GetVirtualMethod(i);
    if (instrumentation->GetSavedCodeFromMap(method) != NULL) {
      instrumentation->ResetSavedCode(method);
    }
  }
  return true;
}

static void InstrumentationRestoreStack(Thread* self, void*) NO_THREAD_SAFETY_ANALYSIS {
  struct RestoreStackVisitor : public StackVisitor {
    RestoreStackVisitor(Thread* self)
        : StackVisitor(self->GetManagedStack(), self->GetInstrumentationStack(), NULL), self_(self) {}

    virtual bool VisitFrame() {
      if (self_->IsInstrumentationStackEmpty()) {
        return false;  // Stop.
      }
      uintptr_t pc = GetReturnPc();
      if (IsInstrumentationExitPc(pc)) {
        InstrumentationStackFrame instrumentation_frame = self_->PopInstrumentationStackFrame();
        SetReturnPc(instrumentation_frame.return_pc_);
        CHECK(GetMethod() == instrumentation_frame.method_);
      }
      return true;  // Continue.
    }

    Thread* self_;
  };
  RestoreStackVisitor visitor(self);
  visitor.WalkStack();
}

Instrumentation::~Instrumentation() {
  delete trace_;
}

void Instrumentation::InstallStubs() {
  Runtime::Current()->GetClassLinker()->VisitClasses(InstallStubsClassVisitor, NULL);
}

void Instrumentation::UninstallStubs() {
  Thread* self = Thread::Current();
  Locks::thread_list_lock_->AssertNotHeld(self);
  Runtime::Current()->GetClassLinker()->VisitClasses(UninstallStubsClassVisitor, NULL);
  MutexLock mu(self, *Locks::thread_list_lock_);
  Runtime::Current()->GetThreadList()->ForEach(InstrumentationRestoreStack, NULL);
}

void Instrumentation::AddSavedCodeToMap(const AbstractMethod* method, const void* code) {
  saved_code_map_.Put(method, code);
}

void Instrumentation::RemoveSavedCodeFromMap(const AbstractMethod* method) {
  saved_code_map_.erase(method);
}

const void* Instrumentation::GetSavedCodeFromMap(const AbstractMethod* method) {
  typedef SafeMap<const AbstractMethod*, const void*>::const_iterator It; // TODO: C++0x auto
  It it = saved_code_map_.find(method);
  if (it == saved_code_map_.end()) {
    return NULL;
  } else {
    return it->second;
  }
}

void Instrumentation::SaveAndUpdateCode(AbstractMethod* method) {
#if defined(ART_USE_LLVM_COMPILER)
  UNUSED(method);
  UNIMPLEMENTED(FATAL);
#else
  void* instrumentation_stub = GetInstrumentationEntryPoint();
  CHECK(GetSavedCodeFromMap(method) == NULL);
  AddSavedCodeToMap(method, method->GetCode());
  method->SetCode(instrumentation_stub);
#endif
}

void Instrumentation::ResetSavedCode(AbstractMethod* method) {
  CHECK(GetSavedCodeFromMap(method) != NULL);
  method->SetCode(GetSavedCodeFromMap(method));
  RemoveSavedCodeFromMap(method);
}

Trace* Instrumentation::GetTrace() const {
  return trace_;
}

void Instrumentation::SetTrace(Trace* trace) {
  trace_ = trace;
}

void Instrumentation::RemoveTrace() {
  delete trace_;
  trace_ = NULL;
}

uint32_t InstrumentationMethodUnwindFromCode(Thread* self) {
  Trace* trace = Runtime::Current()->GetInstrumentation()->GetTrace();
  InstrumentationStackFrame instrumentation_frame = self->PopInstrumentationStackFrame();
  AbstractMethod* method = instrumentation_frame.method_;
  uint32_t lr = instrumentation_frame.return_pc_;

  trace->LogMethodTraceEvent(self, method, Trace::kMethodTraceUnwind);

  return lr;
}

}  // namespace art
