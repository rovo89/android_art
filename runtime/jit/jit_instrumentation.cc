/*
 * Copyright 2014 The Android Open Source Project
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

#include "jit_instrumentation.h"

#include "art_method-inl.h"
#include "jit.h"
#include "jit_code_cache.h"
#include "scoped_thread_state_change.h"

namespace art {
namespace jit {

class JitCompileTask FINAL : public Task {
 public:
  explicit JitCompileTask(ArtMethod* method) : method_(method) {
    ScopedObjectAccess soa(Thread::Current());
    // Add a global ref to the class to prevent class unloading until compilation is done.
    klass_ = soa.Vm()->AddGlobalRef(soa.Self(), method_->GetDeclaringClass());
    CHECK(klass_ != nullptr);
  }

  ~JitCompileTask() {
    ScopedObjectAccess soa(Thread::Current());
    soa.Vm()->DeleteGlobalRef(soa.Self(), klass_);
  }

  void Run(Thread* self) OVERRIDE {
    ScopedObjectAccess soa(self);
    VLOG(jit) << "JitCompileTask compiling method " << PrettyMethod(method_);
    if (!Runtime::Current()->GetJit()->CompileMethod(method_, self)) {
      VLOG(jit) << "Failed to compile method " << PrettyMethod(method_);
    }
  }

  void Finalize() OVERRIDE {
    delete this;
  }

 private:
  ArtMethod* const method_;
  jobject klass_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(JitCompileTask);
};

JitInstrumentationCache::JitInstrumentationCache(size_t hot_method_threshold,
                                                 size_t warm_method_threshold)
    : hot_method_threshold_(hot_method_threshold),
      warm_method_threshold_(warm_method_threshold) {
}

void JitInstrumentationCache::CreateThreadPool() {
  thread_pool_.reset(new ThreadPool("Jit thread pool", 1));
}

void JitInstrumentationCache::DeleteThreadPool() {
  DCHECK(Runtime::Current()->IsShuttingDown(Thread::Current()));
  thread_pool_.reset();
}

void JitInstrumentationCache::AddSamples(Thread* self, ArtMethod* method, size_t) {
  ScopedObjectAccessUnchecked soa(self);
  // Since we don't have on-stack replacement, some methods can remain in the interpreter longer
  // than we want resulting in samples even after the method is compiled.
  if (method->IsClassInitializer() || method->IsNative() ||
      Runtime::Current()->GetJit()->GetCodeCache()->ContainsMethod(method)) {
    return;
  }
  if (thread_pool_.get() == nullptr) {
    DCHECK(Runtime::Current()->IsShuttingDown(self));
    return;
  }
  uint16_t sample_count = method->IncrementCounter();
  if (sample_count == warm_method_threshold_) {
    ProfilingInfo* info = method->CreateProfilingInfo();
    if (info != nullptr) {
      VLOG(jit) << "Start profiling " << PrettyMethod(method);
    }
  }
  if (sample_count == hot_method_threshold_) {
    thread_pool_->AddTask(self, new JitCompileTask(
        method->GetInterfaceMethodIfProxy(sizeof(void*))));
    thread_pool_->StartWorkers(self);
  }
}

JitInstrumentationListener::JitInstrumentationListener(JitInstrumentationCache* cache)
    : instrumentation_cache_(cache) {
  CHECK(instrumentation_cache_ != nullptr);
}

void JitInstrumentationListener::InvokeVirtualOrInterface(Thread* thread,
                                                          mirror::Object* this_object,
                                                          ArtMethod* caller,
                                                          uint32_t dex_pc,
                                                          ArtMethod* callee ATTRIBUTE_UNUSED) {
  DCHECK(this_object != nullptr);
  ProfilingInfo* info = caller->GetProfilingInfo(sizeof(void*));
  if (info != nullptr) {
    info->AddInvokeInfo(thread, dex_pc, this_object->GetClass());
  }
}

void JitInstrumentationCache::WaitForCompilationToFinish(Thread* self) {
  thread_pool_->Wait(self, false, false);
}

}  // namespace jit
}  // namespace art
