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

class JitCompileTask : public Task {
 public:
  explicit JitCompileTask(ArtMethod* method, JitInstrumentationCache* cache)
      : method_(method), cache_(cache) {
  }

  virtual void Run(Thread* self) OVERRIDE {
    ScopedObjectAccess soa(self);
    VLOG(jit) << "JitCompileTask compiling method " << PrettyMethod(method_);
    if (Runtime::Current()->GetJit()->CompileMethod(method_, self)) {
      cache_->SignalCompiled(self, method_);
    } else {
      VLOG(jit) << "Failed to compile method " << PrettyMethod(method_);
    }
  }

  virtual void Finalize() OVERRIDE {
    delete this;
  }

 private:
  ArtMethod* const method_;
  JitInstrumentationCache* const cache_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(JitCompileTask);
};

JitInstrumentationCache::JitInstrumentationCache(size_t hot_method_threshold)
    : lock_("jit instrumentation lock"), hot_method_threshold_(hot_method_threshold) {
}

void JitInstrumentationCache::CreateThreadPool() {
  thread_pool_.reset(new ThreadPool("Jit thread pool", 1));
}

void JitInstrumentationCache::DeleteThreadPool() {
  thread_pool_.reset();
}

void JitInstrumentationCache::SignalCompiled(Thread* self, ArtMethod* method) {
  ScopedObjectAccessUnchecked soa(self);
  jmethodID method_id = soa.EncodeMethod(method);
  MutexLock mu(self, lock_);
  auto it = samples_.find(method_id);
  if (it != samples_.end()) {
    samples_.erase(it);
  }
}

void JitInstrumentationCache::AddSamples(Thread* self, ArtMethod* method, size_t count) {
  ScopedObjectAccessUnchecked soa(self);
  // Since we don't have on-stack replacement, some methods can remain in the interpreter longer
  // than we want resulting in samples even after the method is compiled.
  if (method->IsClassInitializer() || method->IsNative() ||
      Runtime::Current()->GetJit()->GetCodeCache()->ContainsMethod(method)) {
    return;
  }
  jmethodID method_id = soa.EncodeMethod(method);
  bool is_hot = false;
  {
    MutexLock mu(self, lock_);
    size_t sample_count = 0;
    auto it = samples_.find(method_id);
    if (it != samples_.end()) {
      it->second += count;
      sample_count = it->second;
    } else {
      sample_count = count;
      samples_.insert(std::make_pair(method_id, count));
    }
    // If we have enough samples, mark as hot and request Jit compilation.
    if (sample_count >= hot_method_threshold_ && sample_count - count < hot_method_threshold_) {
      is_hot = true;
    }
  }
  if (is_hot) {
    if (thread_pool_.get() != nullptr) {
      thread_pool_->AddTask(self, new JitCompileTask(
          method->GetInterfaceMethodIfProxy(sizeof(void*)), this));
      thread_pool_->StartWorkers(self);
    } else {
      VLOG(jit) << "Compiling hot method " << PrettyMethod(method);
      Runtime::Current()->GetJit()->CompileMethod(
          method->GetInterfaceMethodIfProxy(sizeof(void*)), self);
    }
  }
}

JitInstrumentationListener::JitInstrumentationListener(JitInstrumentationCache* cache)
    : instrumentation_cache_(cache) {
  CHECK(instrumentation_cache_ != nullptr);
}

}  // namespace jit
}  // namespace art
