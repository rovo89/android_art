// Copyright 2011 Google Inc. All Rights Reserved.

#include "trace.h"

#include "class_linker.h"
#include "dex_cache.h"
#include "runtime_support.h"
#include "thread.h"

namespace art {

#if defined(__arm__)
static bool InstallStubsClassVisitor(Class* klass, void* trace_stub) {
  for (size_t i = 0; i < klass->NumDirectMethods(); i++) {
    Method* method = klass->GetDirectMethod(i);
    if (method->GetCode() != trace_stub) {
      Trace::SaveAndUpdateCode(method, trace_stub);
    }
  }

  for (size_t i = 0; i < klass->NumVirtualMethods(); i++) {
    Method* method = klass->GetVirtualMethod(i);
    if (method->GetCode() != trace_stub) {
      Trace::SaveAndUpdateCode(method, trace_stub);
    }
  }

  if (!klass->IsArrayClass() && !klass->IsPrimitive()) {
    CodeAndDirectMethods* c_and_dm = klass->GetDexCache()->GetCodeAndDirectMethods();
    for (size_t i = 0; i < c_and_dm->NumCodeAndDirectMethods(); i++) {
      Method* method = c_and_dm->GetResolvedMethod(i);
      if (method != NULL && (size_t) method != i) {
        c_and_dm->SetResolvedDirectMethodTraceEntry(i, trace_stub);
      }
    }
  }
  return true;
}

static bool UninstallStubsClassVisitor(Class* klass, void* trace_stub) {
  for (size_t i = 0; i < klass->NumDirectMethods(); i++) {
    Method* method = klass->GetDirectMethod(i);
    if (Trace::GetSavedCodeFromMap(method) != NULL) {
      Trace::ResetSavedCode(method);
    }
  }

  for (size_t i = 0; i < klass->NumVirtualMethods(); i++) {
    Method* method = klass->GetVirtualMethod(i);
    if (Trace::GetSavedCodeFromMap(method) != NULL) {
      Trace::ResetSavedCode(method);
    }
  }

  if (!klass->IsArrayClass() && !klass->IsPrimitive()) {
    CodeAndDirectMethods* c_and_dm = klass->GetDexCache()->GetCodeAndDirectMethods();
    for (size_t i = 0; i < c_and_dm->NumCodeAndDirectMethods(); i++) {
      const void* code = c_and_dm->GetResolvedCode(i);
      if (code == trace_stub) {
        Method* method = klass->GetDexCache()->GetResolvedMethod(i);
        if (Trace::GetSavedCodeFromMap(method) != NULL) {
          Trace::ResetSavedCode(method);
        }
        c_and_dm->SetResolvedDirectMethod(i, method);
      }
    }
  }
  return true;
}

static void TraceRestoreStack(Thread* t, void*) {
  uintptr_t trace_exit = reinterpret_cast<uintptr_t>(art_trace_exit_from_code);

  Frame frame = t->GetTopOfStack();
  if (frame.GetSP() != 0) {
    for ( ; frame.GetMethod() != 0; frame.Next()) {
      if (t->IsTraceStackEmpty()) {
        break;
      }
      uintptr_t pc = frame.GetReturnPC();
      Method* method = frame.GetMethod();
      if (trace_exit == pc) {
        TraceStackFrame trace_frame = t->PopTraceStackFrame();
        frame.SetReturnPC(trace_frame.return_pc_);
        CHECK(method == trace_frame.method_);
      }
    }
  }
}
#endif

bool Trace::method_tracing_active_ = false;
std::map<const Method*, const void*> Trace::saved_code_map_;

void Trace::AddSavedCodeToMap(const Method* method, const void* code) {
  saved_code_map_.insert(std::make_pair(method, code));
}

void Trace::RemoveSavedCodeFromMap(const Method* method) {
  saved_code_map_.erase(method);
}

const void* Trace::GetSavedCodeFromMap(const Method* method) {
  return saved_code_map_.find(method)->second;
}

void Trace::SaveAndUpdateCode(Method* method, const void* new_code) {
  CHECK(GetSavedCodeFromMap(method) == NULL);
  AddSavedCodeToMap(method, method->GetCode());
  method->SetCode(new_code);
}

void Trace::ResetSavedCode(Method* method) {
  CHECK(GetSavedCodeFromMap(method) != NULL);
  method->SetCode(GetSavedCodeFromMap(method));
  RemoveSavedCodeFromMap(method);
}

bool Trace::IsMethodTracingActive() {
  return method_tracing_active_;
}

void Trace::SetMethodTracingActive(bool value) {
  method_tracing_active_ = value;
}

void Trace::Start(const char* trace_filename, int trace_fd, int buffer_size, int flags, bool direct_to_ddms) {
  LOG(INFO) << "Starting method tracing...";
  if (IsMethodTracingActive()) {
    // TODO: Stop the trace, then start it up again instead of returning
    LOG(INFO) << "Trace already in progress, stopping";
    return;
  }

  SetMethodTracingActive(true);
  InstallStubs();
  LOG(INFO) << "Method tracing started";
}

void Trace::Stop() {
  LOG(INFO) << "Stopping method tracing...";
  if (!IsMethodTracingActive()) {
    LOG(INFO) << "Trace stop requested, but not running";
    return;
  }

  UninstallStubs();
  SetMethodTracingActive(false);
  LOG(INFO) << "Method tracing stopped";
}

void Trace::InstallStubs() {
#if defined(__arm__)
  {
    ScopedThreadStateChange tsc(Thread::Current(), Thread::kRunnable);
    Runtime::Current()->GetThreadList()->SuspendAll(false);
  }

  void* trace_stub = reinterpret_cast<void*>(art_trace_entry_from_code);
  Runtime::Current()->GetClassLinker()->VisitClasses(InstallStubsClassVisitor, trace_stub);

  Runtime::Current()->GetThreadList()->ResumeAll(false);
#else
  UNIMPLEMENTED(WARNING);
#endif
}

void Trace::UninstallStubs() {
#if defined(__arm__)
  {
    ScopedThreadStateChange tsc(Thread::Current(), Thread::kRunnable);
    Runtime::Current()->GetThreadList()->SuspendAll(false);
  }

  void* trace_stub = reinterpret_cast<void*>(art_trace_entry_from_code);
  Runtime::Current()->GetClassLinker()->VisitClasses(UninstallStubsClassVisitor, trace_stub);

  // Restore stacks of all threads
  {
    ScopedThreadListLock thread_list_lock;
    Runtime::Current()->GetThreadList()->ForEach(TraceRestoreStack, NULL);
  }

  Runtime::Current()->GetThreadList()->ResumeAll(false);
#else
  UNIMPLEMENTED(WARNING);
#endif
}

}  // namespace art
