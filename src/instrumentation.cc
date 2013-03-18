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
#include "mirror/class-inl.h"
#include "mirror/dex_cache.h"
#include "mirror/abstract_method-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/object-inl.h"
#if !defined(ART_USE_PORTABLE_COMPILER)
#include "oat/runtime/oat_support_entrypoints.h"
#endif
#include "object_utils.h"
#include "os.h"
#include "scoped_thread_state_change.h"
#include "thread.h"
#include "thread_list.h"
#include "trace.h"

namespace art {

static bool InstallStubsClassVisitor(mirror::Class* klass, void*)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
  for (size_t i = 0; i < klass->NumDirectMethods(); i++) {
    mirror::AbstractMethod* method = klass->GetDirectMethod(i);
    if (instrumentation->GetSavedCodeFromMap(method) == NULL) {
      instrumentation->SaveAndUpdateCode(method);
    }
  }

  for (size_t i = 0; i < klass->NumVirtualMethods(); i++) {
    mirror::AbstractMethod* method = klass->GetVirtualMethod(i);
    if (instrumentation->GetSavedCodeFromMap(method) == NULL) {
      instrumentation->SaveAndUpdateCode(method);
    }
  }
  return true;
}

static bool UninstallStubsClassVisitor(mirror::Class* klass, void*)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
  for (size_t i = 0; i < klass->NumDirectMethods(); i++) {
    mirror::AbstractMethod* method = klass->GetDirectMethod(i);
    if (instrumentation->GetSavedCodeFromMap(method) != NULL) {
      instrumentation->ResetSavedCode(method);
    }
  }

  for (size_t i = 0; i < klass->NumVirtualMethods(); i++) {
    mirror::AbstractMethod* method = klass->GetVirtualMethod(i);
    if (instrumentation->GetSavedCodeFromMap(method) != NULL) {
      instrumentation->ResetSavedCode(method);
    }
  }
  return true;
}

void InstrumentationInstallStack(Thread* self, void* arg)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  struct InstallStackVisitor : public StackVisitor {
    InstallStackVisitor(Thread* self, uintptr_t instrumentation_exit_pc)
        : StackVisitor(self, NULL),  self_(self),
          instrumentation_exit_pc_(instrumentation_exit_pc) {}

    virtual bool VisitFrame() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
      if (GetCurrentQuickFrame() == NULL) {
        return true;  // Ignore shadow frames.
      }
      mirror::AbstractMethod* m = GetMethod();
      if (m == NULL) {
        return true; // Ignore upcalls.
      }
      if (m->GetDexMethodIndex() == DexFile::kDexNoIndex16) {
        return true;  // Ignore unresolved methods since they will be instrumented after resolution.
      }
      uintptr_t pc = GetReturnPc();
      InstrumentationStackFrame instrumentation_frame(m, pc, GetFrameId());
      self_->PushBackInstrumentationStackFrame(instrumentation_frame);
      SetReturnPc(instrumentation_exit_pc_);
      return true;  // Continue.
    }
    Thread* const self_;
    const uintptr_t instrumentation_exit_pc_;
  };
  uintptr_t instrumentation_exit_pc = GetInstrumentationExitPc();
  InstallStackVisitor visitor(self, instrumentation_exit_pc);
  visitor.WalkStack(true);
  Trace* trace = reinterpret_cast<Trace*>(arg);
  if (trace != NULL) {
    std::deque<InstrumentationStackFrame>::const_reverse_iterator it =
        self->GetInstrumentationStack()->rbegin();
    std::deque<InstrumentationStackFrame>::const_reverse_iterator end =
        self->GetInstrumentationStack()->rend();
    for (; it != end; ++it) {
      trace->LogMethodTraceEvent(self, (*it).method_, Trace::kMethodTraceEnter);
    }
  }
}

static void InstrumentationRestoreStack(Thread* self, void*)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  struct RestoreStackVisitor : public StackVisitor {
    RestoreStackVisitor(Thread* self, uintptr_t instrumentation_exit_pc)
        : StackVisitor(self, NULL), self_(self),
          instrumentation_exit_pc_(instrumentation_exit_pc) {}

    virtual bool VisitFrame() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
      if (self_->IsInstrumentationStackEmpty()) {
        return false;  // Stop.
      }
      mirror::AbstractMethod* m = GetMethod();
      if (m == NULL) {
        return true;  // Ignore upcalls.
      }
      uintptr_t pc = GetReturnPc();
      if (pc == instrumentation_exit_pc_) {
        InstrumentationStackFrame instrumentation_frame = self_->PopInstrumentationStackFrame();
        SetReturnPc(instrumentation_frame.return_pc_);
        CHECK(m == instrumentation_frame.method_);
        CHECK_EQ(GetFrameId(), instrumentation_frame.frame_id_);
        Runtime* runtime = Runtime::Current();
        if (runtime->IsMethodTracingActive()) {
          Trace* trace = runtime->GetInstrumentation()->GetTrace();
          trace->LogMethodTraceEvent(self_, m, Trace::kMethodTraceExit);
        }
      }
      return true;  // Continue.
    }
    Thread* const self_;
    const uintptr_t instrumentation_exit_pc_;
  };
  uintptr_t instrumentation_exit_pc = GetInstrumentationExitPc();
  RestoreStackVisitor visitor(self, instrumentation_exit_pc);
  visitor.WalkStack(true);
}

Instrumentation::~Instrumentation() {
  delete trace_;
}

void Instrumentation::InstallStubs() {
  Thread* self = Thread::Current();
  Locks::thread_list_lock_->AssertNotHeld(self);
  Runtime::Current()->GetClassLinker()->VisitClasses(InstallStubsClassVisitor, NULL);
  MutexLock mu(self, *Locks::thread_list_lock_);
  Runtime::Current()->GetThreadList()->ForEach(InstrumentationInstallStack, GetTrace());
}

void Instrumentation::UninstallStubs() {
  Thread* self = Thread::Current();
  Locks::thread_list_lock_->AssertNotHeld(self);
  Runtime::Current()->GetClassLinker()->VisitClasses(UninstallStubsClassVisitor, NULL);
  MutexLock mu(self, *Locks::thread_list_lock_);
  Runtime::Current()->GetThreadList()->ForEach(InstrumentationRestoreStack, NULL);
}

void Instrumentation::AddSavedCodeToMap(const mirror::AbstractMethod* method, const void* code) {
  saved_code_map_.Put(method, code);
}

void Instrumentation::RemoveSavedCodeFromMap(const mirror::AbstractMethod* method) {
  saved_code_map_.erase(method);
}

const void* Instrumentation::GetSavedCodeFromMap(const mirror::AbstractMethod* method) {
  typedef SafeMap<const mirror::AbstractMethod*, const void*>::const_iterator It; // TODO: C++0x auto
  It it = saved_code_map_.find(method);
  if (it == saved_code_map_.end()) {
    return NULL;
  } else {
    return it->second;
  }
}

void Instrumentation::SaveAndUpdateCode(mirror::AbstractMethod* method) {
#if defined(ART_USE_PORTABLE_COMPILER)
  UNUSED(method);
  UNIMPLEMENTED(FATAL);
#else
  void* instrumentation_stub = GetInstrumentationEntryPoint();
  CHECK(GetSavedCodeFromMap(method) == NULL);
  AddSavedCodeToMap(method, method->GetCode());
  method->SetCode(instrumentation_stub);
#endif
}

void Instrumentation::ResetSavedCode(mirror::AbstractMethod* method) {
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
  mirror::AbstractMethod* method = instrumentation_frame.method_;
  uint32_t lr = instrumentation_frame.return_pc_;

  trace->LogMethodTraceEvent(self, method, Trace::kMethodTraceUnwind);

  return lr;
}

}  // namespace art
