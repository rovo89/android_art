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

#include "jit_compiler.h"

#include "art_method-inl.h"
#include "arch/instruction_set.h"
#include "arch/instruction_set_features.h"
#include "base/stringpiece.h"
#include "base/time_utils.h"
#include "base/timing_logger.h"
#include "base/unix_file/fd_file.h"
#include "compiler_callbacks.h"
#include "dex/pass_manager.h"
#include "dex/quick_compiler_callbacks.h"
#include "driver/compiler_driver.h"
#include "driver/compiler_options.h"
#include "jit/jit.h"
#include "jit/jit_code_cache.h"
#include "oat_file-inl.h"
#include "oat_quick_method_header.h"
#include "object_lock.h"
#include "thread_list.h"
#include "verifier/method_verifier-inl.h"

namespace art {
namespace jit {

JitCompiler* JitCompiler::Create() {
  return new JitCompiler();
}

extern "C" void* jit_load(CompilerCallbacks** callbacks, bool* generate_debug_info) {
  VLOG(jit) << "loading jit compiler";
  auto* const jit_compiler = JitCompiler::Create();
  CHECK(jit_compiler != nullptr);
  *callbacks = jit_compiler->GetCompilerCallbacks();
  *generate_debug_info = jit_compiler->GetCompilerOptions()->GetGenerateDebugInfo();
  VLOG(jit) << "Done loading jit compiler";
  return jit_compiler;
}

extern "C" void jit_unload(void* handle) {
  DCHECK(handle != nullptr);
  delete reinterpret_cast<JitCompiler*>(handle);
}

extern "C" bool jit_compile_method(void* handle, ArtMethod* method, Thread* self)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  auto* jit_compiler = reinterpret_cast<JitCompiler*>(handle);
  DCHECK(jit_compiler != nullptr);
  return jit_compiler->CompileMethod(self, method);
}

// Callers of this method assume it has NO_RETURN.
NO_RETURN static void Usage(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::string error;
  StringAppendV(&error, fmt, ap);
  LOG(FATAL) << error;
  va_end(ap);
  exit(EXIT_FAILURE);
}

JitCompiler::JitCompiler() : total_time_(0) {
  compiler_options_.reset(new CompilerOptions(
      CompilerOptions::kDefaultCompilerFilter,
      CompilerOptions::kDefaultHugeMethodThreshold,
      CompilerOptions::kDefaultLargeMethodThreshold,
      CompilerOptions::kDefaultSmallMethodThreshold,
      CompilerOptions::kDefaultTinyMethodThreshold,
      CompilerOptions::kDefaultNumDexMethodsThreshold,
      CompilerOptions::kDefaultInlineDepthLimit,
      CompilerOptions::kDefaultInlineMaxCodeUnits,
      /* no_inline_from */ nullptr,
      /* include_patch_information */ false,
      CompilerOptions::kDefaultTopKProfileThreshold,
      Runtime::Current()->IsDebuggable(),
      CompilerOptions::kDefaultGenerateDebugInfo,
      /* implicit_null_checks */ true,
      /* implicit_so_checks */ true,
      /* implicit_suspend_checks */ false,
      /* pic */ true,  // TODO: Support non-PIC in optimizing.
      /* verbose_methods */ nullptr,
      /* init_failure_output */ nullptr,
      /* abort_on_hard_verifier_failure */ false,
      /* dump_cfg_file_name */ "",
      /* dump_cfg_append */ false));
  for (const std::string& argument : Runtime::Current()->GetCompilerOptions()) {
    compiler_options_->ParseCompilerOption(argument, Usage);
  }
  const InstructionSet instruction_set = kRuntimeISA;
  for (const StringPiece option : Runtime::Current()->GetCompilerOptions()) {
    VLOG(compiler) << "JIT compiler option " << option;
    std::string error_msg;
    if (option.starts_with("--instruction-set-variant=")) {
      StringPiece str = option.substr(strlen("--instruction-set-variant=")).data();
      VLOG(compiler) << "JIT instruction set variant " << str;
      instruction_set_features_.reset(InstructionSetFeatures::FromVariant(
          instruction_set, str.as_string(), &error_msg));
      if (instruction_set_features_ == nullptr) {
        LOG(WARNING) << "Error parsing " << option << " message=" << error_msg;
      }
    } else if (option.starts_with("--instruction-set-features=")) {
      StringPiece str = option.substr(strlen("--instruction-set-features=")).data();
      VLOG(compiler) << "JIT instruction set features " << str;
      if (instruction_set_features_.get() == nullptr) {
        instruction_set_features_.reset(InstructionSetFeatures::FromVariant(
            instruction_set, "default", &error_msg));
        if (instruction_set_features_ == nullptr) {
          LOG(WARNING) << "Error parsing " << option << " message=" << error_msg;
        }
      }
      instruction_set_features_.reset(
          instruction_set_features_->AddFeaturesFromString(str.as_string(), &error_msg));
      if (instruction_set_features_ == nullptr) {
        LOG(WARNING) << "Error parsing " << option << " message=" << error_msg;
      }
    }
  }
  if (instruction_set_features_ == nullptr) {
    instruction_set_features_.reset(InstructionSetFeatures::FromCppDefines());
  }
  cumulative_logger_.reset(new CumulativeLogger("jit times"));
  verification_results_.reset(new VerificationResults(compiler_options_.get()));
  method_inliner_map_.reset(new DexFileToMethodInlinerMap);
  callbacks_.reset(new QuickCompilerCallbacks(verification_results_.get(),
                                              method_inliner_map_.get(),
                                              CompilerCallbacks::CallbackMode::kCompileApp));
  compiler_driver_.reset(new CompilerDriver(
      compiler_options_.get(),
      verification_results_.get(),
      method_inliner_map_.get(),
      Compiler::kOptimizing,
      instruction_set,
      instruction_set_features_.get(),
      /* image */ false,
      /* image_classes */ nullptr,
      /* compiled_classes */ nullptr,
      /* compiled_methods */ nullptr,
      /* thread_count */ 1,
      /* dump_stats */ false,
      /* dump_passes */ false,
      cumulative_logger_.get(),
      /* swap_fd */ -1,
      /* dex to oat map */ nullptr,
      /* profile_compilation_info */ nullptr));
  // Disable dedupe so we can remove compiled methods.
  compiler_driver_->SetDedupeEnabled(false);
  compiler_driver_->SetSupportBootImageFixup(false);

  if (compiler_options_->GetGenerateDebugInfo()) {
#ifdef __ANDROID__
    const char* prefix = GetAndroidData();
#else
    const char* prefix = "/tmp";
#endif
    DCHECK_EQ(compiler_driver_->GetThreadCount(), 1u)
        << "Generating debug info only works with one compiler thread";
    std::string perf_filename = std::string(prefix) + "/perf-" + std::to_string(getpid()) + ".map";
    perf_file_.reset(OS::CreateEmptyFileWriteOnly(perf_filename.c_str()));
    if (perf_file_ == nullptr) {
      LOG(FATAL) << "Could not create perf file at " << perf_filename;
    }
  }
}

JitCompiler::~JitCompiler() {
  if (perf_file_ != nullptr) {
    UNUSED(perf_file_->Flush());
    UNUSED(perf_file_->Close());
  }
}

bool JitCompiler::CompileMethod(Thread* self, ArtMethod* method) {
  TimingLogger logger("JIT compiler timing logger", true, VLOG_IS_ON(jit));
  const uint64_t start_time = NanoTime();
  StackHandleScope<2> hs(self);
  self->AssertNoPendingException();
  Runtime* runtime = Runtime::Current();

  // Ensure the class is initialized.
  Handle<mirror::Class> h_class(hs.NewHandle(method->GetDeclaringClass()));
  if (!runtime->GetClassLinker()->EnsureInitialized(self, h_class, true, true)) {
    VLOG(jit) << "JIT failed to initialize " << PrettyMethod(method);
    return false;
  }

  // Do the compilation.
  bool success = false;
  {
    TimingLogger::ScopedTiming t2("Compiling", &logger);
    // If we get a request to compile a proxy method, we pass the actual Java method
    // of that proxy method, as the compiler does not expect a proxy method.
    ArtMethod* method_to_compile = method->GetInterfaceMethodIfProxy(sizeof(void*));
    JitCodeCache* const code_cache = runtime->GetJit()->GetCodeCache();
    success = compiler_driver_->GetCompiler()->JitCompile(self, code_cache, method_to_compile);
    if (success && compiler_options_->GetGenerateDebugInfo()) {
      const void* ptr = method_to_compile->GetEntryPointFromQuickCompiledCode();
      std::ostringstream stream;
      stream << std::hex
             << reinterpret_cast<uintptr_t>(ptr)
             << " "
             << code_cache->GetMemorySizeOfCodePointer(ptr)
             << " "
             << PrettyMethod(method_to_compile)
             << std::endl;
      std::string str = stream.str();
      bool res = perf_file_->WriteFully(str.c_str(), str.size());
      CHECK(res);
    }
  }

  // Trim maps to reduce memory usage.
  // TODO: measure how much this increases compile time.
  {
    TimingLogger::ScopedTiming t2("TrimMaps", &logger);
    runtime->GetArenaPool()->TrimMaps();
  }

  total_time_ += NanoTime() - start_time;
  runtime->GetJit()->AddTimingLogger(logger);
  return success;
}

CompilerCallbacks* JitCompiler::GetCompilerCallbacks() const {
  return callbacks_.get();
}

}  // namespace jit
}  // namespace art
