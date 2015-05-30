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

#include "jit.h"

#include <dlfcn.h>

#include "art_method-inl.h"
#include "entrypoints/runtime_asm_entrypoints.h"
#include "interpreter/interpreter.h"
#include "jit_code_cache.h"
#include "jit_instrumentation.h"
#include "runtime.h"
#include "runtime_options.h"
#include "thread_list.h"
#include "utils.h"

namespace art {
namespace jit {

JitOptions* JitOptions::CreateFromRuntimeArguments(const RuntimeArgumentMap& options) {
  auto* jit_options = new JitOptions;
  jit_options->use_jit_ = options.GetOrDefault(RuntimeArgumentMap::UseJIT);
  jit_options->code_cache_capacity_ =
      options.GetOrDefault(RuntimeArgumentMap::JITCodeCacheCapacity);
  jit_options->compile_threshold_ =
      options.GetOrDefault(RuntimeArgumentMap::JITCompileThreshold);
  jit_options->dump_info_on_shutdown_ =
      options.Exists(RuntimeArgumentMap::DumpJITInfoOnShutdown);
  return jit_options;
}

void Jit::DumpInfo(std::ostream& os) {
  os << "Code cache size=" << PrettySize(code_cache_->CodeCacheSize())
     << " data cache size=" << PrettySize(code_cache_->DataCacheSize())
     << " num methods=" << code_cache_->NumMethods()
     << "\n";
  cumulative_timings_.Dump(os);
}

void Jit::AddTimingLogger(const TimingLogger& logger) {
  cumulative_timings_.AddLogger(logger);
}

Jit::Jit()
    : jit_library_handle_(nullptr), jit_compiler_handle_(nullptr), jit_load_(nullptr),
      jit_compile_method_(nullptr), dump_info_on_shutdown_(false),
      cumulative_timings_("JIT timings") {
}

Jit* Jit::Create(JitOptions* options, std::string* error_msg) {
  std::unique_ptr<Jit> jit(new Jit);
  jit->dump_info_on_shutdown_ = options->DumpJitInfoOnShutdown();
  if (!jit->LoadCompiler(error_msg)) {
    return nullptr;
  }
  jit->code_cache_.reset(JitCodeCache::Create(options->GetCodeCacheCapacity(), error_msg));
  if (jit->GetCodeCache() == nullptr) {
    return nullptr;
  }
  LOG(INFO) << "JIT created with code_cache_capacity="
      << PrettySize(options->GetCodeCacheCapacity())
      << " compile_threshold=" << options->GetCompileThreshold();
  return jit.release();
}

bool Jit::LoadCompiler(std::string* error_msg) {
  jit_library_handle_ = dlopen(
      kIsDebugBuild ? "libartd-compiler.so" : "libart-compiler.so", RTLD_NOW);
  if (jit_library_handle_ == nullptr) {
    std::ostringstream oss;
    oss << "JIT could not load libart-compiler.so: " << dlerror();
    *error_msg = oss.str();
    return false;
  }
  jit_load_ = reinterpret_cast<void* (*)(CompilerCallbacks**)>(
      dlsym(jit_library_handle_, "jit_load"));
  if (jit_load_ == nullptr) {
    dlclose(jit_library_handle_);
    *error_msg = "JIT couldn't find jit_load entry point";
    return false;
  }
  jit_unload_ = reinterpret_cast<void (*)(void*)>(
      dlsym(jit_library_handle_, "jit_unload"));
  if (jit_unload_ == nullptr) {
    dlclose(jit_library_handle_);
    *error_msg = "JIT couldn't find jit_unload entry point";
    return false;
  }
  jit_compile_method_ = reinterpret_cast<bool (*)(void*, ArtMethod*, Thread*)>(
      dlsym(jit_library_handle_, "jit_compile_method"));
  if (jit_compile_method_ == nullptr) {
    dlclose(jit_library_handle_);
    *error_msg = "JIT couldn't find jit_compile_method entry point";
    return false;
  }
  CompilerCallbacks* callbacks = nullptr;
  VLOG(jit) << "Calling JitLoad interpreter_only="
      << Runtime::Current()->GetInstrumentation()->InterpretOnly();
  jit_compiler_handle_ = (jit_load_)(&callbacks);
  if (jit_compiler_handle_ == nullptr) {
    dlclose(jit_library_handle_);
    *error_msg = "JIT couldn't load compiler";
    return false;
  }
  if (callbacks == nullptr) {
    dlclose(jit_library_handle_);
    *error_msg = "JIT compiler callbacks were not set";
    jit_compiler_handle_ = nullptr;
    return false;
  }
  compiler_callbacks_ = callbacks;
  return true;
}

bool Jit::CompileMethod(ArtMethod* method, Thread* self) {
  DCHECK(!method->IsRuntimeMethod());
  if (Dbg::IsDebuggerActive() && Dbg::MethodHasAnyBreakpoints(method)) {
    VLOG(jit) << "JIT not compiling " << PrettyMethod(method) << " due to breakpoint";
    return false;
  }
  const bool result = jit_compile_method_(jit_compiler_handle_, method, self);
  if (result) {
    method->SetEntryPointFromInterpreter(artInterpreterToCompiledCodeBridge);
  }
  return result;
}

void Jit::CreateThreadPool() {
  CHECK(instrumentation_cache_.get() != nullptr);
  instrumentation_cache_->CreateThreadPool();
}

void Jit::DeleteThreadPool() {
  if (instrumentation_cache_.get() != nullptr) {
    instrumentation_cache_->DeleteThreadPool();
  }
}

Jit::~Jit() {
  if (dump_info_on_shutdown_) {
    DumpInfo(LOG(INFO));
  }
  DeleteThreadPool();
  if (jit_compiler_handle_ != nullptr) {
    jit_unload_(jit_compiler_handle_);
  }
  if (jit_library_handle_ != nullptr) {
    dlclose(jit_library_handle_);
  }
}

void Jit::CreateInstrumentationCache(size_t compile_threshold) {
  CHECK_GT(compile_threshold, 0U);
  Runtime* const runtime = Runtime::Current();
  runtime->GetThreadList()->SuspendAll(__FUNCTION__);
  // Add Jit interpreter instrumentation, tells the interpreter when to notify the jit to compile
  // something.
  instrumentation_cache_.reset(new jit::JitInstrumentationCache(compile_threshold));
  runtime->GetInstrumentation()->AddListener(
      new jit::JitInstrumentationListener(instrumentation_cache_.get()),
      instrumentation::Instrumentation::kMethodEntered |
      instrumentation::Instrumentation::kBackwardBranch);
  runtime->GetThreadList()->ResumeAll();
}

}  // namespace jit
}  // namespace art
