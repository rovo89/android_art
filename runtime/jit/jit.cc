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
#include "debugger.h"
#include "entrypoints/runtime_asm_entrypoints.h"
#include "interpreter/interpreter.h"
#include "jit_code_cache.h"
#include "jit_instrumentation.h"
#include "oat_file_manager.h"
#include "oat_quick_method_header.h"
#include "offline_profiling_info.h"
#include "profile_saver.h"
#include "runtime.h"
#include "runtime_options.h"
#include "stack_map.h"
#include "utils.h"

namespace art {
namespace jit {

static constexpr bool kEnableOnStackReplacement = true;

JitOptions* JitOptions::CreateFromRuntimeArguments(const RuntimeArgumentMap& options) {
  auto* jit_options = new JitOptions;
  jit_options->use_jit_ = options.GetOrDefault(RuntimeArgumentMap::UseJIT);
  jit_options->code_cache_initial_capacity_ =
      options.GetOrDefault(RuntimeArgumentMap::JITCodeCacheInitialCapacity);
  jit_options->code_cache_max_capacity_ =
      options.GetOrDefault(RuntimeArgumentMap::JITCodeCacheMaxCapacity);
  jit_options->compile_threshold_ =
      options.GetOrDefault(RuntimeArgumentMap::JITCompileThreshold);
  // TODO(ngeoffray): Make this a proper option.
  jit_options->osr_threshold_ = jit_options->compile_threshold_ * 2;
  jit_options->warmup_threshold_ =
      options.GetOrDefault(RuntimeArgumentMap::JITWarmupThreshold);
  jit_options->dump_info_on_shutdown_ =
      options.Exists(RuntimeArgumentMap::DumpJITInfoOnShutdown);
  jit_options->save_profiling_info_ =
      options.GetOrDefault(RuntimeArgumentMap::JITSaveProfilingInfo);;
  return jit_options;
}

void Jit::DumpInfo(std::ostream& os) {
  os << "JIT code cache size=" << PrettySize(code_cache_->CodeCacheSize()) << "\n"
     << "JIT data cache size=" << PrettySize(code_cache_->DataCacheSize()) << "\n"
     << "JIT current capacity=" << PrettySize(code_cache_->GetCurrentCapacity()) << "\n"
     << "JIT number of compiled code=" << code_cache_->NumberOfCompiledCode() << "\n"
     << "JIT total number of compilations=" << code_cache_->NumberOfCompilations() << "\n"
     << "JIT total number of osr compilations=" << code_cache_->NumberOfOsrCompilations() << "\n";
  cumulative_timings_.Dump(os);
}

void Jit::AddTimingLogger(const TimingLogger& logger) {
  cumulative_timings_.AddLogger(logger);
}

Jit::Jit() : jit_library_handle_(nullptr),
             jit_compiler_handle_(nullptr),
             jit_load_(nullptr),
             jit_compile_method_(nullptr),
             dump_info_on_shutdown_(false),
             cumulative_timings_("JIT timings"),
             save_profiling_info_(false),
             generate_debug_info_(false) {
}

Jit* Jit::Create(JitOptions* options, std::string* error_msg) {
  std::unique_ptr<Jit> jit(new Jit);
  jit->dump_info_on_shutdown_ = options->DumpJitInfoOnShutdown();
  if (!jit->LoadCompiler(error_msg)) {
    return nullptr;
  }
  jit->code_cache_.reset(JitCodeCache::Create(
      options->GetCodeCacheInitialCapacity(),
      options->GetCodeCacheMaxCapacity(),
      jit->generate_debug_info_,
      error_msg));
  if (jit->GetCodeCache() == nullptr) {
    return nullptr;
  }
  jit->save_profiling_info_ = options->GetSaveProfilingInfo();
  LOG(INFO) << "JIT created with initial_capacity="
      << PrettySize(options->GetCodeCacheInitialCapacity())
      << ", max_capacity=" << PrettySize(options->GetCodeCacheMaxCapacity())
      << ", compile_threshold=" << options->GetCompileThreshold()
      << ", save_profiling_info=" << options->GetSaveProfilingInfo();
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
  jit_load_ = reinterpret_cast<void* (*)(bool*)>(dlsym(jit_library_handle_, "jit_load"));
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
  jit_compile_method_ = reinterpret_cast<bool (*)(void*, ArtMethod*, Thread*, bool)>(
      dlsym(jit_library_handle_, "jit_compile_method"));
  if (jit_compile_method_ == nullptr) {
    dlclose(jit_library_handle_);
    *error_msg = "JIT couldn't find jit_compile_method entry point";
    return false;
  }
  jit_types_loaded_ = reinterpret_cast<void (*)(void*, mirror::Class**, size_t)>(
      dlsym(jit_library_handle_, "jit_types_loaded"));
  if (jit_types_loaded_ == nullptr) {
    dlclose(jit_library_handle_);
    *error_msg = "JIT couldn't find jit_types_loaded entry point";
    return false;
  }
  bool will_generate_debug_symbols = false;
  VLOG(jit) << "Calling JitLoad interpreter_only="
      << Runtime::Current()->GetInstrumentation()->InterpretOnly();
  jit_compiler_handle_ = (jit_load_)(&will_generate_debug_symbols);
  if (jit_compiler_handle_ == nullptr) {
    dlclose(jit_library_handle_);
    *error_msg = "JIT couldn't load compiler";
    return false;
  }
  generate_debug_info_ = will_generate_debug_symbols;
  return true;
}

bool Jit::CompileMethod(ArtMethod* method, Thread* self, bool osr) {
  DCHECK(!method->IsRuntimeMethod());

  // Don't compile the method if it has breakpoints.
  if (Dbg::IsDebuggerActive() && Dbg::MethodHasAnyBreakpoints(method)) {
    VLOG(jit) << "JIT not compiling " << PrettyMethod(method) << " due to breakpoint";
    return false;
  }

  // Don't compile the method if we are supposed to be deoptimized.
  instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
  if (instrumentation->AreAllMethodsDeoptimized() || instrumentation->IsDeoptimized(method)) {
    VLOG(jit) << "JIT not compiling " << PrettyMethod(method) << " due to deoptimization";
    return false;
  }

  // If we get a request to compile a proxy method, we pass the actual Java method
  // of that proxy method, as the compiler does not expect a proxy method.
  ArtMethod* method_to_compile = method->GetInterfaceMethodIfProxy(sizeof(void*));
  if (!code_cache_->NotifyCompilationOf(method_to_compile, self, osr)) {
    VLOG(jit) << "JIT not compiling " << PrettyMethod(method) << " due to code cache";
    return false;
  }
  bool success = jit_compile_method_(jit_compiler_handle_, method_to_compile, self, osr);
  code_cache_->DoneCompiling(method_to_compile, self);
  return success;
}

void Jit::CreateThreadPool() {
  CHECK(instrumentation_cache_.get() != nullptr);
  instrumentation_cache_->CreateThreadPool();
}

void Jit::DeleteThreadPool() {
  if (instrumentation_cache_.get() != nullptr) {
    instrumentation_cache_->DeleteThreadPool(Thread::Current());
  }
}

void Jit::StartProfileSaver(const std::string& filename,
                            const std::vector<std::string>& code_paths,
                            const std::string& foreign_dex_profile_path,
                            const std::string& app_dir) {
  if (save_profiling_info_) {
    ProfileSaver::Start(filename, code_cache_.get(), code_paths, foreign_dex_profile_path, app_dir);
  }
}

void Jit::StopProfileSaver() {
  if (save_profiling_info_ && ProfileSaver::IsStarted()) {
    ProfileSaver::Stop();
  }
}

bool Jit::JitAtFirstUse() {
  if (instrumentation_cache_ != nullptr) {
    return instrumentation_cache_->HotMethodThreshold() == 0;
  }
  return false;
}

bool Jit::CanInvokeCompiledCode(ArtMethod* method) {
  return code_cache_->ContainsPc(method->GetEntryPointFromQuickCompiledCode());
}

Jit::~Jit() {
  DCHECK(!save_profiling_info_ || !ProfileSaver::IsStarted());
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

void Jit::CreateInstrumentationCache(size_t compile_threshold,
                                     size_t warmup_threshold,
                                     size_t osr_threshold) {
  instrumentation_cache_.reset(
      new jit::JitInstrumentationCache(compile_threshold, warmup_threshold, osr_threshold));
}

void Jit::NewTypeLoadedIfUsingJit(mirror::Class* type) {
  jit::Jit* jit = Runtime::Current()->GetJit();
  if (jit != nullptr && jit->generate_debug_info_) {
    DCHECK(jit->jit_types_loaded_ != nullptr);
    jit->jit_types_loaded_(jit->jit_compiler_handle_, &type, 1);
  }
}

void Jit::DumpTypeInfoForLoadedTypes(ClassLinker* linker) {
  struct CollectClasses : public ClassVisitor {
    bool operator()(mirror::Class* klass) override {
      classes_.push_back(klass);
      return true;
    }
    std::vector<mirror::Class*> classes_;
  };

  if (generate_debug_info_) {
    ScopedObjectAccess so(Thread::Current());

    CollectClasses visitor;
    linker->VisitClasses(&visitor);
    jit_types_loaded_(jit_compiler_handle_, visitor.classes_.data(), visitor.classes_.size());
  }
}

extern "C" void art_quick_osr_stub(void** stack,
                                   uint32_t stack_size_in_bytes,
                                   const uint8_t* native_pc,
                                   JValue* result,
                                   const char* shorty,
                                   Thread* self);

bool Jit::MaybeDoOnStackReplacement(Thread* thread,
                                    ArtMethod* method,
                                    uint32_t dex_pc,
                                    int32_t dex_pc_offset,
                                    JValue* result) {
  if (!kEnableOnStackReplacement) {
    return false;
  }

  Jit* jit = Runtime::Current()->GetJit();
  if (jit == nullptr) {
    return false;
  }

  if (kRuntimeISA == kMips || kRuntimeISA == kMips64) {
    VLOG(jit) << "OSR not supported on this platform: " << kRuntimeISA;
    return false;
  }

  if (UNLIKELY(__builtin_frame_address(0) < thread->GetStackEnd())) {
    // Don't attempt to do an OSR if we are close to the stack limit. Since
    // the interpreter frames are still on stack, OSR has the potential
    // to stack overflow even for a simple loop.
    // b/27094810.
    return false;
  }

  // Get the actual Java method if this method is from a proxy class. The compiler
  // and the JIT code cache do not expect methods from proxy classes.
  method = method->GetInterfaceMethodIfProxy(sizeof(void*));

  // Cheap check if the method has been compiled already. That's an indicator that we should
  // osr into it.
  if (!jit->GetCodeCache()->ContainsPc(method->GetEntryPointFromQuickCompiledCode())) {
    return false;
  }

  // Fetch some data before looking up for an OSR method. We don't want thread
  // suspension once we hold an OSR method, as the JIT code cache could delete the OSR
  // method while we are being suspended.
  const size_t number_of_vregs = method->GetCodeItem()->registers_size_;
  const char* shorty = method->GetShorty();
  std::string method_name(VLOG_IS_ON(jit) ? PrettyMethod(method) : "");
  void** memory = nullptr;
  size_t frame_size = 0;
  ShadowFrame* shadow_frame = nullptr;
  const uint8_t* native_pc = nullptr;

  {
    ScopedAssertNoThreadSuspension sts(thread, "Holding OSR method");
    const OatQuickMethodHeader* osr_method = jit->GetCodeCache()->LookupOsrMethodHeader(method);
    if (osr_method == nullptr) {
      // No osr method yet, just return to the interpreter.
      return false;
    }

    CodeInfo code_info = osr_method->GetOptimizedCodeInfo();
    StackMapEncoding encoding = code_info.ExtractEncoding();

    // Find stack map starting at the target dex_pc.
    StackMap stack_map = code_info.GetOsrStackMapForDexPc(dex_pc + dex_pc_offset, encoding);
    if (!stack_map.IsValid()) {
      // There is no OSR stack map for this dex pc offset. Just return to the interpreter in the
      // hope that the next branch has one.
      return false;
    }

    // We found a stack map, now fill the frame with dex register values from the interpreter's
    // shadow frame.
    DexRegisterMap vreg_map =
        code_info.GetDexRegisterMapOf(stack_map, encoding, number_of_vregs);

    frame_size = osr_method->GetFrameSizeInBytes();

    // Allocate memory to put shadow frame values. The osr stub will copy that memory to
    // stack.
    // Note that we could pass the shadow frame to the stub, and let it copy the values there,
    // but that is engineering complexity not worth the effort for something like OSR.
    memory = reinterpret_cast<void**>(malloc(frame_size));
    CHECK(memory != nullptr);
    memset(memory, 0, frame_size);

    // Art ABI: ArtMethod is at the bottom of the stack.
    memory[0] = method;

    shadow_frame = thread->PopShadowFrame();
    if (!vreg_map.IsValid()) {
      // If we don't have a dex register map, then there are no live dex registers at
      // this dex pc.
    } else {
      for (uint16_t vreg = 0; vreg < number_of_vregs; ++vreg) {
        DexRegisterLocation::Kind location =
            vreg_map.GetLocationKind(vreg, number_of_vregs, code_info, encoding);
        if (location == DexRegisterLocation::Kind::kNone) {
          // Dex register is dead or uninitialized.
          continue;
        }

        if (location == DexRegisterLocation::Kind::kConstant) {
          // We skip constants because the compiled code knows how to handle them.
          continue;
        }

        DCHECK(location == DexRegisterLocation::Kind::kInStack)
            << DexRegisterLocation::PrettyDescriptor(location);

        int32_t vreg_value = shadow_frame->GetVReg(vreg);
        int32_t slot_offset = vreg_map.GetStackOffsetInBytes(vreg,
                                                             number_of_vregs,
                                                             code_info,
                                                             encoding);
        DCHECK_LT(slot_offset, static_cast<int32_t>(frame_size));
        DCHECK_GT(slot_offset, 0);
        (reinterpret_cast<int32_t*>(memory))[slot_offset / sizeof(int32_t)] = vreg_value;
      }
    }

    native_pc = stack_map.GetNativePcOffset(encoding) + osr_method->GetEntryPoint();
    VLOG(jit) << "Jumping to "
              << method_name
              << "@"
              << std::hex << reinterpret_cast<uintptr_t>(native_pc);
  }

  {
    ManagedStack fragment;
    thread->PushManagedStackFragment(&fragment);
    (*art_quick_osr_stub)(memory,
                          frame_size,
                          native_pc,
                          result,
                          shorty,
                          thread);

    if (UNLIKELY(thread->GetException() == Thread::GetDeoptimizationException())) {
      thread->DeoptimizeWithDeoptimizationException(result);
    }
    thread->PopManagedStackFragment(fragment);
  }
  free(memory);
  thread->PushShadowFrame(shadow_frame);
  VLOG(jit) << "Done running OSR code for " << method_name;
  return true;
}

}  // namespace jit
}  // namespace art
