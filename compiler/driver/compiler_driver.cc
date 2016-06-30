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

#include "compiler_driver.h"

#include <unordered_set>
#include <vector>
#include <unistd.h>

#ifndef __APPLE__
#include <malloc.h>  // For mallinfo
#endif

#include "art_field-inl.h"
#include "art_method-inl.h"
#include "base/bit_vector.h"
#include "base/stl_util.h"
#include "base/systrace.h"
#include "base/time_utils.h"
#include "base/timing_logger.h"
#include "class_linker-inl.h"
#include "compiled_class.h"
#include "compiled_method.h"
#include "compiler.h"
#include "compiler_driver-inl.h"
#include "dex_compilation_unit.h"
#include "dex_file-inl.h"
#include "dex_instruction-inl.h"
#include "dex/dex_to_dex_compiler.h"
#include "dex/verification_results.h"
#include "dex/verified_method.h"
#include "dex/quick/dex_file_method_inliner.h"
#include "dex/quick/dex_file_to_method_inliner_map.h"
#include "driver/compiler_options.h"
#include "jni_internal.h"
#include "object_lock.h"
#include "profiler.h"
#include "runtime.h"
#include "gc/accounting/card_table-inl.h"
#include "gc/accounting/heap_bitmap.h"
#include "gc/space/image_space.h"
#include "gc/space/space.h"
#include "mirror/class_loader.h"
#include "mirror/class-inl.h"
#include "mirror/dex_cache-inl.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/throwable.h"
#include "scoped_thread_state_change.h"
#include "ScopedLocalRef.h"
#include "handle_scope-inl.h"
#include "thread.h"
#include "thread_list.h"
#include "thread_pool.h"
#include "trampolines/trampoline_compiler.h"
#include "transaction.h"
#include "utils/array_ref.h"
#include "utils/dex_cache_arrays_layout-inl.h"
#include "utils/swap_space.h"
#include "verifier/method_verifier.h"
#include "verifier/method_verifier-inl.h"

namespace art {

static constexpr bool kTimeCompileMethod = !kIsDebugBuild;

// Whether classes-to-compile and methods-to-compile are only applied to the boot image, or, when
// given, too all compilations.
static constexpr bool kRestrictCompilationFiltersToImage = true;

// Print additional info during profile guided compilation.
static constexpr bool kDebugProfileGuidedCompilation = false;

static double Percentage(size_t x, size_t y) {
  return 100.0 * (static_cast<double>(x)) / (static_cast<double>(x + y));
}

static void DumpStat(size_t x, size_t y, const char* str) {
  if (x == 0 && y == 0) {
    return;
  }
  LOG(INFO) << Percentage(x, y) << "% of " << str << " for " << (x + y) << " cases";
}

class CompilerDriver::AOTCompilationStats {
 public:
  AOTCompilationStats()
      : stats_lock_("AOT compilation statistics lock"),
        types_in_dex_cache_(0), types_not_in_dex_cache_(0),
        strings_in_dex_cache_(0), strings_not_in_dex_cache_(0),
        resolved_types_(0), unresolved_types_(0),
        resolved_instance_fields_(0), unresolved_instance_fields_(0),
        resolved_local_static_fields_(0), resolved_static_fields_(0), unresolved_static_fields_(0),
        type_based_devirtualization_(0),
        safe_casts_(0), not_safe_casts_(0) {
    for (size_t i = 0; i <= kMaxInvokeType; i++) {
      resolved_methods_[i] = 0;
      unresolved_methods_[i] = 0;
      virtual_made_direct_[i] = 0;
      direct_calls_to_boot_[i] = 0;
      direct_methods_to_boot_[i] = 0;
    }
  }

  void Dump() {
    DumpStat(types_in_dex_cache_, types_not_in_dex_cache_, "types known to be in dex cache");
    DumpStat(strings_in_dex_cache_, strings_not_in_dex_cache_, "strings known to be in dex cache");
    DumpStat(resolved_types_, unresolved_types_, "types resolved");
    DumpStat(resolved_instance_fields_, unresolved_instance_fields_, "instance fields resolved");
    DumpStat(resolved_local_static_fields_ + resolved_static_fields_, unresolved_static_fields_,
             "static fields resolved");
    DumpStat(resolved_local_static_fields_, resolved_static_fields_ + unresolved_static_fields_,
             "static fields local to a class");
    DumpStat(safe_casts_, not_safe_casts_, "check-casts removed based on type information");
    // Note, the code below subtracts the stat value so that when added to the stat value we have
    // 100% of samples. TODO: clean this up.
    DumpStat(type_based_devirtualization_,
             resolved_methods_[kVirtual] + unresolved_methods_[kVirtual] +
             resolved_methods_[kInterface] + unresolved_methods_[kInterface] -
             type_based_devirtualization_,
             "virtual/interface calls made direct based on type information");

    for (size_t i = 0; i <= kMaxInvokeType; i++) {
      std::ostringstream oss;
      oss << static_cast<InvokeType>(i) << " methods were AOT resolved";
      DumpStat(resolved_methods_[i], unresolved_methods_[i], oss.str().c_str());
      if (virtual_made_direct_[i] > 0) {
        std::ostringstream oss2;
        oss2 << static_cast<InvokeType>(i) << " methods made direct";
        DumpStat(virtual_made_direct_[i],
                 resolved_methods_[i] + unresolved_methods_[i] - virtual_made_direct_[i],
                 oss2.str().c_str());
      }
      if (direct_calls_to_boot_[i] > 0) {
        std::ostringstream oss2;
        oss2 << static_cast<InvokeType>(i) << " method calls are direct into boot";
        DumpStat(direct_calls_to_boot_[i],
                 resolved_methods_[i] + unresolved_methods_[i] - direct_calls_to_boot_[i],
                 oss2.str().c_str());
      }
      if (direct_methods_to_boot_[i] > 0) {
        std::ostringstream oss2;
        oss2 << static_cast<InvokeType>(i) << " method calls have methods in boot";
        DumpStat(direct_methods_to_boot_[i],
                 resolved_methods_[i] + unresolved_methods_[i] - direct_methods_to_boot_[i],
                 oss2.str().c_str());
      }
    }
  }

// Allow lossy statistics in non-debug builds.
#ifndef NDEBUG
#define STATS_LOCK() MutexLock mu(Thread::Current(), stats_lock_)
#else
#define STATS_LOCK()
#endif

  void TypeInDexCache() REQUIRES(!stats_lock_) {
    STATS_LOCK();
    types_in_dex_cache_++;
  }

  void TypeNotInDexCache() REQUIRES(!stats_lock_) {
    STATS_LOCK();
    types_not_in_dex_cache_++;
  }

  void StringInDexCache() REQUIRES(!stats_lock_) {
    STATS_LOCK();
    strings_in_dex_cache_++;
  }

  void StringNotInDexCache() REQUIRES(!stats_lock_) {
    STATS_LOCK();
    strings_not_in_dex_cache_++;
  }

  void TypeDoesntNeedAccessCheck() REQUIRES(!stats_lock_) {
    STATS_LOCK();
    resolved_types_++;
  }

  void TypeNeedsAccessCheck() REQUIRES(!stats_lock_) {
    STATS_LOCK();
    unresolved_types_++;
  }

  void ResolvedInstanceField() REQUIRES(!stats_lock_) {
    STATS_LOCK();
    resolved_instance_fields_++;
  }

  void UnresolvedInstanceField() REQUIRES(!stats_lock_) {
    STATS_LOCK();
    unresolved_instance_fields_++;
  }

  void ResolvedLocalStaticField() REQUIRES(!stats_lock_) {
    STATS_LOCK();
    resolved_local_static_fields_++;
  }

  void ResolvedStaticField() REQUIRES(!stats_lock_) {
    STATS_LOCK();
    resolved_static_fields_++;
  }

  void UnresolvedStaticField() REQUIRES(!stats_lock_) {
    STATS_LOCK();
    unresolved_static_fields_++;
  }

  // Indicate that type information from the verifier led to devirtualization.
  void PreciseTypeDevirtualization() REQUIRES(!stats_lock_) {
    STATS_LOCK();
    type_based_devirtualization_++;
  }

  // Indicate that a method of the given type was resolved at compile time.
  void ResolvedMethod(InvokeType type) REQUIRES(!stats_lock_) {
    DCHECK_LE(type, kMaxInvokeType);
    STATS_LOCK();
    resolved_methods_[type]++;
  }

  // Indicate that a method of the given type was unresolved at compile time as it was in an
  // unknown dex file.
  void UnresolvedMethod(InvokeType type) REQUIRES(!stats_lock_) {
    DCHECK_LE(type, kMaxInvokeType);
    STATS_LOCK();
    unresolved_methods_[type]++;
  }

  // Indicate that a type of virtual method dispatch has been converted into a direct method
  // dispatch.
  void VirtualMadeDirect(InvokeType type) REQUIRES(!stats_lock_) {
    DCHECK(type == kVirtual || type == kInterface || type == kSuper);
    STATS_LOCK();
    virtual_made_direct_[type]++;
  }

  // Indicate that a method of the given type was able to call directly into boot.
  void DirectCallsToBoot(InvokeType type) REQUIRES(!stats_lock_) {
    DCHECK_LE(type, kMaxInvokeType);
    STATS_LOCK();
    direct_calls_to_boot_[type]++;
  }

  // Indicate that a method of the given type was able to be resolved directly from boot.
  void DirectMethodsToBoot(InvokeType type) REQUIRES(!stats_lock_) {
    DCHECK_LE(type, kMaxInvokeType);
    STATS_LOCK();
    direct_methods_to_boot_[type]++;
  }

  void ProcessedInvoke(InvokeType type, int flags) REQUIRES(!stats_lock_) {
    STATS_LOCK();
    if (flags == 0) {
      unresolved_methods_[type]++;
    } else {
      DCHECK_NE((flags & kFlagMethodResolved), 0);
      resolved_methods_[type]++;
      if ((flags & kFlagVirtualMadeDirect) != 0) {
        virtual_made_direct_[type]++;
        if ((flags & kFlagPreciseTypeDevirtualization) != 0) {
          type_based_devirtualization_++;
        }
      } else {
        DCHECK_EQ((flags & kFlagPreciseTypeDevirtualization), 0);
      }
      if ((flags & kFlagDirectCallToBoot) != 0) {
        direct_calls_to_boot_[type]++;
      }
      if ((flags & kFlagDirectMethodToBoot) != 0) {
        direct_methods_to_boot_[type]++;
      }
    }
  }

  // A check-cast could be eliminated due to verifier type analysis.
  void SafeCast() REQUIRES(!stats_lock_) {
    STATS_LOCK();
    safe_casts_++;
  }

  // A check-cast couldn't be eliminated due to verifier type analysis.
  void NotASafeCast() REQUIRES(!stats_lock_) {
    STATS_LOCK();
    not_safe_casts_++;
  }

 private:
  Mutex stats_lock_;

  size_t types_in_dex_cache_;
  size_t types_not_in_dex_cache_;

  size_t strings_in_dex_cache_;
  size_t strings_not_in_dex_cache_;

  size_t resolved_types_;
  size_t unresolved_types_;

  size_t resolved_instance_fields_;
  size_t unresolved_instance_fields_;

  size_t resolved_local_static_fields_;
  size_t resolved_static_fields_;
  size_t unresolved_static_fields_;
  // Type based devirtualization for invoke interface and virtual.
  size_t type_based_devirtualization_;

  size_t resolved_methods_[kMaxInvokeType + 1];
  size_t unresolved_methods_[kMaxInvokeType + 1];
  size_t virtual_made_direct_[kMaxInvokeType + 1];
  size_t direct_calls_to_boot_[kMaxInvokeType + 1];
  size_t direct_methods_to_boot_[kMaxInvokeType + 1];

  size_t safe_casts_;
  size_t not_safe_casts_;

  DISALLOW_COPY_AND_ASSIGN(AOTCompilationStats);
};

class CompilerDriver::DexFileMethodSet {
 public:
  explicit DexFileMethodSet(const DexFile& dex_file)
    : dex_file_(dex_file),
      method_indexes_(dex_file.NumMethodIds(), false, Allocator::GetMallocAllocator()) {
  }
  DexFileMethodSet(DexFileMethodSet&& other) = default;

  const DexFile& GetDexFile() const { return dex_file_; }

  BitVector& GetMethodIndexes() { return method_indexes_; }
  const BitVector& GetMethodIndexes() const { return method_indexes_; }

 private:
  const DexFile& dex_file_;
  BitVector method_indexes_;
};

CompilerDriver::CompilerDriver(
    const CompilerOptions* compiler_options,
    VerificationResults* verification_results,
    DexFileToMethodInlinerMap* method_inliner_map,
    Compiler::Kind compiler_kind,
    InstructionSet instruction_set,
    const InstructionSetFeatures* instruction_set_features,
    bool boot_image,
    bool app_image,
    std::unordered_set<std::string>* image_classes,
    std::unordered_set<std::string>* compiled_classes,
    std::unordered_set<std::string>* compiled_methods,
    size_t thread_count,
    bool dump_stats,
    bool dump_passes,
    CumulativeLogger* timer,
    int swap_fd,
    const ProfileCompilationInfo* profile_compilation_info)
    : compiler_options_(compiler_options),
      verification_results_(verification_results),
      method_inliner_map_(method_inliner_map),
      compiler_(Compiler::Create(this, compiler_kind)),
      compiler_kind_(compiler_kind),
      instruction_set_(instruction_set),
      instruction_set_features_(instruction_set_features),
      requires_constructor_barrier_lock_("constructor barrier lock"),
      compiled_classes_lock_("compiled classes lock"),
      compiled_methods_lock_("compiled method lock"),
      compiled_methods_(MethodTable::key_compare()),
      non_relative_linker_patch_count_(0u),
      boot_image_(boot_image),
      app_image_(app_image),
      image_classes_(image_classes),
      classes_to_compile_(compiled_classes),
      methods_to_compile_(compiled_methods),
      had_hard_verifier_failure_(false),
      parallel_thread_count_(thread_count),
      stats_(new AOTCompilationStats),
      dump_stats_(dump_stats),
      dump_passes_(dump_passes),
      timings_logger_(timer),
      compiler_context_(nullptr),
      support_boot_image_fixup_(instruction_set != kMips && instruction_set != kMips64),
      dex_files_for_oat_file_(nullptr),
      compiled_method_storage_(swap_fd),
      profile_compilation_info_(profile_compilation_info),
      max_arena_alloc_(0),
      dex_to_dex_references_lock_("dex-to-dex references lock"),
      dex_to_dex_references_(),
      current_dex_to_dex_methods_(nullptr) {
  DCHECK(compiler_options_ != nullptr);
  DCHECK(method_inliner_map_ != nullptr);

  compiler_->Init();

  if (compiler_options->VerifyOnlyProfile()) {
    CHECK(profile_compilation_info_ != nullptr) << "Requires profile";
  }
  if (boot_image_) {
    CHECK(image_classes_.get() != nullptr) << "Expected image classes for boot image";
  }
}

CompilerDriver::~CompilerDriver() {
  Thread* self = Thread::Current();
  {
    MutexLock mu(self, compiled_classes_lock_);
    STLDeleteValues(&compiled_classes_);
  }
  {
    MutexLock mu(self, compiled_methods_lock_);
    for (auto& pair : compiled_methods_) {
      CompiledMethod::ReleaseSwapAllocatedCompiledMethod(this, pair.second);
    }
  }
  compiler_->UnInit();
}


#define CREATE_TRAMPOLINE(type, abi, offset) \
    if (Is64BitInstructionSet(instruction_set_)) { \
      return CreateTrampoline64(instruction_set_, abi, \
                                type ## _ENTRYPOINT_OFFSET(8, offset)); \
    } else { \
      return CreateTrampoline32(instruction_set_, abi, \
                                type ## _ENTRYPOINT_OFFSET(4, offset)); \
    }

std::unique_ptr<const std::vector<uint8_t>> CompilerDriver::CreateJniDlsymLookup() const {
  CREATE_TRAMPOLINE(JNI, kJniAbi, pDlsymLookup)
}

std::unique_ptr<const std::vector<uint8_t>> CompilerDriver::CreateQuickGenericJniTrampoline()
    const {
  CREATE_TRAMPOLINE(QUICK, kQuickAbi, pQuickGenericJniTrampoline)
}

std::unique_ptr<const std::vector<uint8_t>> CompilerDriver::CreateQuickImtConflictTrampoline()
    const {
  CREATE_TRAMPOLINE(QUICK, kQuickAbi, pQuickImtConflictTrampoline)
}

std::unique_ptr<const std::vector<uint8_t>> CompilerDriver::CreateQuickResolutionTrampoline()
    const {
  CREATE_TRAMPOLINE(QUICK, kQuickAbi, pQuickResolutionTrampoline)
}

std::unique_ptr<const std::vector<uint8_t>> CompilerDriver::CreateQuickToInterpreterBridge()
    const {
  CREATE_TRAMPOLINE(QUICK, kQuickAbi, pQuickToInterpreterBridge)
}
#undef CREATE_TRAMPOLINE

void CompilerDriver::CompileAll(jobject class_loader,
                                const std::vector<const DexFile*>& dex_files,
                                TimingLogger* timings) {
  DCHECK(!Runtime::Current()->IsStarted());

  InitializeThreadPools();

  VLOG(compiler) << "Before precompile " << GetMemoryUsageString(false);
  // Precompile:
  // 1) Load image classes
  // 2) Resolve all classes
  // 3) Attempt to verify all classes
  // 4) Attempt to initialize image classes, and trivially initialized classes
  PreCompile(class_loader, dex_files, timings);
  // Compile:
  // 1) Compile all classes and methods enabled for compilation. May fall back to dex-to-dex
  //    compilation.
  if (!GetCompilerOptions().VerifyAtRuntime()) {
    Compile(class_loader, dex_files, timings);
  }
  if (dump_stats_) {
    stats_->Dump();
  }

  FreeThreadPools();
}

static optimizer::DexToDexCompilationLevel GetDexToDexCompilationLevel(
    Thread* self, const CompilerDriver& driver, Handle<mirror::ClassLoader> class_loader,
    const DexFile& dex_file, const DexFile::ClassDef& class_def)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  auto* const runtime = Runtime::Current();
  if (runtime->UseJitCompilation() || driver.GetCompilerOptions().VerifyAtRuntime()) {
    // Verify at runtime shouldn't dex to dex since we didn't resolve of verify.
    return optimizer::DexToDexCompilationLevel::kDontDexToDexCompile;
  }
  const char* descriptor = dex_file.GetClassDescriptor(class_def);
  ClassLinker* class_linker = runtime->GetClassLinker();
  mirror::Class* klass = class_linker->FindClass(self, descriptor, class_loader);
  if (klass == nullptr) {
    CHECK(self->IsExceptionPending());
    self->ClearException();
    return optimizer::DexToDexCompilationLevel::kDontDexToDexCompile;
  }
  // DexToDex at the kOptimize level may introduce quickened opcodes, which replace symbolic
  // references with actual offsets. We cannot re-verify such instructions.
  //
  // We store the verification information in the class status in the oat file, which the linker
  // can validate (checksums) and use to skip load-time verification. It is thus safe to
  // optimize when a class has been fully verified before.
  if (klass->IsVerified()) {
    // Class is verified so we can enable DEX-to-DEX compilation for performance.
    return optimizer::DexToDexCompilationLevel::kOptimize;
  } else if (klass->IsCompileTimeVerified()) {
    // Class verification has soft-failed. Anyway, ensure at least correctness.
    DCHECK_EQ(klass->GetStatus(), mirror::Class::kStatusRetryVerificationAtRuntime);
    return optimizer::DexToDexCompilationLevel::kRequired;
  } else {
    // Class verification has failed: do not run DEX-to-DEX compilation.
    return optimizer::DexToDexCompilationLevel::kDontDexToDexCompile;
  }
}

static optimizer::DexToDexCompilationLevel GetDexToDexCompilationLevel(
    Thread* self,
    const CompilerDriver& driver,
    jobject jclass_loader,
    const DexFile& dex_file,
    const DexFile::ClassDef& class_def) {
  ScopedObjectAccess soa(self);
  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(
      hs.NewHandle(soa.Decode<mirror::ClassLoader*>(jclass_loader)));
  return GetDexToDexCompilationLevel(self, driver, class_loader, dex_file, class_def);
}

// Does the runtime for the InstructionSet provide an implementation returned by
// GetQuickGenericJniStub allowing down calls that aren't compiled using a JNI compiler?
static bool InstructionSetHasGenericJniStub(InstructionSet isa) {
  switch (isa) {
    case kArm:
    case kArm64:
    case kThumb2:
    case kMips:
    case kMips64:
    case kX86:
    case kX86_64: return true;
    default: return false;
  }
}

static void CompileMethod(Thread* self,
                          CompilerDriver* driver,
                          const DexFile::CodeItem* code_item,
                          uint32_t access_flags,
                          InvokeType invoke_type,
                          uint16_t class_def_idx,
                          uint32_t method_idx,
                          jobject class_loader,
                          const DexFile& dex_file,
                          optimizer::DexToDexCompilationLevel dex_to_dex_compilation_level,
                          bool compilation_enabled,
                          Handle<mirror::DexCache> dex_cache)
    REQUIRES(!driver->compiled_methods_lock_) {
  DCHECK(driver != nullptr);
  CompiledMethod* compiled_method = nullptr;
  uint64_t start_ns = kTimeCompileMethod ? NanoTime() : 0;
  MethodReference method_ref(&dex_file, method_idx);

  if (driver->GetCurrentDexToDexMethods() != nullptr) {
    // This is the second pass when we dex-to-dex compile previously marked methods.
    // TODO: Refactor the compilation to avoid having to distinguish the two passes
    // here. That should be done on a higher level. http://b/29089975
    if (driver->GetCurrentDexToDexMethods()->IsBitSet(method_idx)) {
      const VerifiedMethod* verified_method =
          driver->GetVerificationResults()->GetVerifiedMethod(method_ref);
      // Do not optimize if a VerifiedMethod is missing. SafeCast elision,
      // for example, relies on it.
      compiled_method = optimizer::ArtCompileDEX(
          driver,
          code_item,
          access_flags,
          invoke_type,
          class_def_idx,
          method_idx,
          class_loader,
          dex_file,
          (verified_method != nullptr)
              ? dex_to_dex_compilation_level
              : optimizer::DexToDexCompilationLevel::kRequired);
    }
  } else if ((access_flags & kAccNative) != 0) {
    // Are we extracting only and have support for generic JNI down calls?
    if (!driver->GetCompilerOptions().IsJniCompilationEnabled() &&
        InstructionSetHasGenericJniStub(driver->GetInstructionSet())) {
      // Leaving this empty will trigger the generic JNI version
    } else {
      compiled_method = driver->GetCompiler()->JniCompile(access_flags, method_idx, dex_file);
      CHECK(compiled_method != nullptr);
    }
  } else if ((access_flags & kAccAbstract) != 0) {
    // Abstract methods don't have code.
  } else {
    const VerifiedMethod* verified_method =
        driver->GetVerificationResults()->GetVerifiedMethod(method_ref);
    bool compile = compilation_enabled &&
        // Basic checks, e.g., not <clinit>.
        driver->GetVerificationResults()
            ->IsCandidateForCompilation(method_ref, access_flags) &&
        // Did not fail to create VerifiedMethod metadata.
        verified_method != nullptr &&
        // Do not have failures that should punt to the interpreter.
        !verified_method->HasRuntimeThrow() &&
        (verified_method->GetEncounteredVerificationFailures() &
            (verifier::VERIFY_ERROR_FORCE_INTERPRETER | verifier::VERIFY_ERROR_LOCKING)) == 0 &&
        // Is eligable for compilation by methods-to-compile filter.
        driver->IsMethodToCompile(method_ref) &&
        driver->ShouldCompileBasedOnProfile(method_ref);

    if (compile) {
      // NOTE: if compiler declines to compile this method, it will return null.
      compiled_method = driver->GetCompiler()->Compile(code_item, access_flags, invoke_type,
                                                       class_def_idx, method_idx, class_loader,
                                                       dex_file, dex_cache);
    }
    if (compiled_method == nullptr &&
        dex_to_dex_compilation_level != optimizer::DexToDexCompilationLevel::kDontDexToDexCompile) {
      DCHECK(!Runtime::Current()->UseJitCompilation());
      // TODO: add a command-line option to disable DEX-to-DEX compilation ?
      driver->MarkForDexToDexCompilation(self, method_ref);
    }
  }
  if (kTimeCompileMethod) {
    uint64_t duration_ns = NanoTime() - start_ns;
    if (duration_ns > MsToNs(driver->GetCompiler()->GetMaximumCompilationTimeBeforeWarning())) {
      LOG(WARNING) << "Compilation of " << PrettyMethod(method_idx, dex_file)
                   << " took " << PrettyDuration(duration_ns);
    }
  }

  if (compiled_method != nullptr) {
    // Count non-relative linker patches.
    size_t non_relative_linker_patch_count = 0u;
    for (const LinkerPatch& patch : compiled_method->GetPatches()) {
      if (!patch.IsPcRelative()) {
        ++non_relative_linker_patch_count;
      }
    }
    bool compile_pic = driver->GetCompilerOptions().GetCompilePic();  // Off by default
    // When compiling with PIC, there should be zero non-relative linker patches
    CHECK(!compile_pic || non_relative_linker_patch_count == 0u);

    driver->AddCompiledMethod(method_ref, compiled_method, non_relative_linker_patch_count);
  }

  if (self->IsExceptionPending()) {
    ScopedObjectAccess soa(self);
    LOG(FATAL) << "Unexpected exception compiling: " << PrettyMethod(method_idx, dex_file) << "\n"
        << self->GetException()->Dump();
  }
}

void CompilerDriver::CompileOne(Thread* self, ArtMethod* method, TimingLogger* timings) {
  DCHECK(!Runtime::Current()->IsStarted());
  jobject jclass_loader;
  const DexFile* dex_file;
  uint16_t class_def_idx;
  uint32_t method_idx = method->GetDexMethodIndex();
  uint32_t access_flags = method->GetAccessFlags();
  InvokeType invoke_type = method->GetInvokeType();
  StackHandleScope<1> hs(self);
  Handle<mirror::DexCache> dex_cache(hs.NewHandle(method->GetDexCache()));
  {
    ScopedObjectAccessUnchecked soa(self);
    ScopedLocalRef<jobject> local_class_loader(
        soa.Env(), soa.AddLocalReference<jobject>(method->GetDeclaringClass()->GetClassLoader()));
    jclass_loader = soa.Env()->NewGlobalRef(local_class_loader.get());
    // Find the dex_file
    dex_file = method->GetDexFile();
    class_def_idx = method->GetClassDefIndex();
  }
  const DexFile::CodeItem* code_item = dex_file->GetCodeItem(method->GetCodeItemOffset());

  // Go to native so that we don't block GC during compilation.
  ScopedThreadSuspension sts(self, kNative);

  std::vector<const DexFile*> dex_files;
  dex_files.push_back(dex_file);

  InitializeThreadPools();

  PreCompile(jclass_loader, dex_files, timings);

  // Can we run DEX-to-DEX compiler on this class ?
  optimizer::DexToDexCompilationLevel dex_to_dex_compilation_level =
      GetDexToDexCompilationLevel(self,
                                  *this,
                                  jclass_loader,
                                  *dex_file,
                                  dex_file->GetClassDef(class_def_idx));

  DCHECK(current_dex_to_dex_methods_ == nullptr);
  CompileMethod(self,
                this,
                code_item,
                access_flags,
                invoke_type,
                class_def_idx,
                method_idx,
                jclass_loader,
                *dex_file,
                dex_to_dex_compilation_level,
                true,
                dex_cache);

  ArrayRef<DexFileMethodSet> dex_to_dex_references;
  {
    // From this point on, we shall not modify dex_to_dex_references_, so
    // just grab a reference to it that we use without holding the mutex.
    MutexLock lock(Thread::Current(), dex_to_dex_references_lock_);
    dex_to_dex_references = ArrayRef<DexFileMethodSet>(dex_to_dex_references_);
  }
  if (!dex_to_dex_references.empty()) {
    DCHECK_EQ(dex_to_dex_references.size(), 1u);
    DCHECK(&dex_to_dex_references[0].GetDexFile() == dex_file);
    current_dex_to_dex_methods_ = &dex_to_dex_references.front().GetMethodIndexes();
    DCHECK(current_dex_to_dex_methods_->IsBitSet(method_idx));
    DCHECK_EQ(current_dex_to_dex_methods_->NumSetBits(), 1u);
    CompileMethod(self,
                  this,
                  code_item,
                  access_flags,
                  invoke_type,
                  class_def_idx,
                  method_idx,
                  jclass_loader,
                  *dex_file,
                  dex_to_dex_compilation_level,
                  true,
                  dex_cache);
    current_dex_to_dex_methods_ = nullptr;
  }

  FreeThreadPools();

  self->GetJniEnv()->DeleteGlobalRef(jclass_loader);
}

void CompilerDriver::Resolve(jobject class_loader,
                             const std::vector<const DexFile*>& dex_files,
                             TimingLogger* timings) {
  // Resolution allocates classes and needs to run single-threaded to be deterministic.
  bool force_determinism = GetCompilerOptions().IsForceDeterminism();
  ThreadPool* resolve_thread_pool = force_determinism
                                     ? single_thread_pool_.get()
                                     : parallel_thread_pool_.get();
  size_t resolve_thread_count = force_determinism ? 1U : parallel_thread_count_;

  for (size_t i = 0; i != dex_files.size(); ++i) {
    const DexFile* dex_file = dex_files[i];
    CHECK(dex_file != nullptr);
    ResolveDexFile(class_loader,
                   *dex_file,
                   dex_files,
                   resolve_thread_pool,
                   resolve_thread_count,
                   timings);
  }
}

// Resolve const-strings in the code. Done to have deterministic allocation behavior. Right now
// this is single-threaded for simplicity.
// TODO: Collect the relevant string indices in parallel, then allocate them sequentially in a
//       stable order.

static void ResolveConstStrings(CompilerDriver* driver,
                                const DexFile& dex_file,
                                const DexFile::CodeItem* code_item) {
  if (code_item == nullptr) {
    // Abstract or native method.
    return;
  }

  const uint16_t* code_ptr = code_item->insns_;
  const uint16_t* code_end = code_item->insns_ + code_item->insns_size_in_code_units_;

  while (code_ptr < code_end) {
    const Instruction* inst = Instruction::At(code_ptr);
    switch (inst->Opcode()) {
      case Instruction::CONST_STRING: {
        uint32_t string_index = inst->VRegB_21c();
        driver->CanAssumeStringIsPresentInDexCache(dex_file, string_index);
        break;
      }
      case Instruction::CONST_STRING_JUMBO: {
        uint32_t string_index = inst->VRegB_31c();
        driver->CanAssumeStringIsPresentInDexCache(dex_file, string_index);
        break;
      }

      default:
        break;
    }

    code_ptr += inst->SizeInCodeUnits();
  }
}

static void ResolveConstStrings(CompilerDriver* driver,
                                const std::vector<const DexFile*>& dex_files,
                                TimingLogger* timings) {
  for (const DexFile* dex_file : dex_files) {
    TimingLogger::ScopedTiming t("Resolve const-string Strings", timings);

    size_t class_def_count = dex_file->NumClassDefs();
    for (size_t class_def_index = 0; class_def_index < class_def_count; ++class_def_index) {
      const DexFile::ClassDef& class_def = dex_file->GetClassDef(class_def_index);

      const uint8_t* class_data = dex_file->GetClassData(class_def);
      if (class_data == nullptr) {
        // empty class, probably a marker interface
        continue;
      }

      ClassDataItemIterator it(*dex_file, class_data);
      // Skip fields
      while (it.HasNextStaticField()) {
        it.Next();
      }
      while (it.HasNextInstanceField()) {
        it.Next();
      }

      bool compilation_enabled = driver->IsClassToCompile(
          dex_file->StringByTypeIdx(class_def.class_idx_));
      if (!compilation_enabled) {
        // Compilation is skipped, do not resolve const-string in code of this class.
        // TODO: Make sure that inlining honors this.
        continue;
      }

      // Direct methods.
      int64_t previous_direct_method_idx = -1;
      while (it.HasNextDirectMethod()) {
        uint32_t method_idx = it.GetMemberIndex();
        if (method_idx == previous_direct_method_idx) {
          // smali can create dex files with two encoded_methods sharing the same method_idx
          // http://code.google.com/p/smali/issues/detail?id=119
          it.Next();
          continue;
        }
        previous_direct_method_idx = method_idx;
        ResolveConstStrings(driver, *dex_file, it.GetMethodCodeItem());
        it.Next();
      }
      // Virtual methods.
      int64_t previous_virtual_method_idx = -1;
      while (it.HasNextVirtualMethod()) {
        uint32_t method_idx = it.GetMemberIndex();
        if (method_idx == previous_virtual_method_idx) {
          // smali can create dex files with two encoded_methods sharing the same method_idx
          // http://code.google.com/p/smali/issues/detail?id=119
          it.Next();
          continue;
        }
        previous_virtual_method_idx = method_idx;
        ResolveConstStrings(driver, *dex_file, it.GetMethodCodeItem());
        it.Next();
      }
      DCHECK(!it.HasNext());
    }
  }
}

inline void CompilerDriver::CheckThreadPools() {
  DCHECK(parallel_thread_pool_ != nullptr);
  DCHECK(single_thread_pool_ != nullptr);
}

void CompilerDriver::PreCompile(jobject class_loader,
                                const std::vector<const DexFile*>& dex_files,
                                TimingLogger* timings) {
  CheckThreadPools();

  LoadImageClasses(timings);
  VLOG(compiler) << "LoadImageClasses: " << GetMemoryUsageString(false);

  const bool verification_enabled = compiler_options_->IsVerificationEnabled();
  const bool never_verify = compiler_options_->NeverVerify();
  const bool verify_only_profile = compiler_options_->VerifyOnlyProfile();

  // We need to resolve for never_verify since it needs to run dex to dex to add the
  // RETURN_VOID_NO_BARRIER.
  // Let the verifier resolve as needed for the verify_only_profile case.
  if ((never_verify || verification_enabled) && !verify_only_profile) {
    Resolve(class_loader, dex_files, timings);
    VLOG(compiler) << "Resolve: " << GetMemoryUsageString(false);
  }

  if (never_verify) {
    VLOG(compiler) << "Verify none mode specified, skipping verification.";
    SetVerified(class_loader, dex_files, timings);
  }

  if (!verification_enabled) {
    return;
  }

  if (GetCompilerOptions().IsForceDeterminism() && IsBootImage()) {
    // Resolve strings from const-string. Do this now to have a deterministic image.
    ResolveConstStrings(this, dex_files, timings);
    VLOG(compiler) << "Resolve const-strings: " << GetMemoryUsageString(false);
  }

  Verify(class_loader, dex_files, timings);
  VLOG(compiler) << "Verify: " << GetMemoryUsageString(false);

  if (had_hard_verifier_failure_ && GetCompilerOptions().AbortOnHardVerifierFailure()) {
    LOG(FATAL) << "Had a hard failure verifying all classes, and was asked to abort in such "
               << "situations. Please check the log.";
  }

  InitializeClasses(class_loader, dex_files, timings);
  VLOG(compiler) << "InitializeClasses: " << GetMemoryUsageString(false);

  UpdateImageClasses(timings);
  VLOG(compiler) << "UpdateImageClasses: " << GetMemoryUsageString(false);
}

bool CompilerDriver::IsImageClass(const char* descriptor) const {
  if (image_classes_ != nullptr) {
    // If we have a set of image classes, use those.
    return image_classes_->find(descriptor) != image_classes_->end();
  }
  // No set of image classes, assume we include all the classes.
  // NOTE: Currently only reachable from InitImageMethodVisitor for the app image case.
  return !IsBootImage();
}

bool CompilerDriver::IsClassToCompile(const char* descriptor) const {
  if (kRestrictCompilationFiltersToImage && !IsBootImage()) {
    return true;
  }

  if (classes_to_compile_ == nullptr) {
    return true;
  }
  return classes_to_compile_->find(descriptor) != classes_to_compile_->end();
}

bool CompilerDriver::IsMethodToCompile(const MethodReference& method_ref) const {
  if (kRestrictCompilationFiltersToImage && !IsBootImage()) {
    return true;
  }

  if (methods_to_compile_ == nullptr) {
    return true;
  }

  std::string tmp = PrettyMethod(method_ref.dex_method_index, *method_ref.dex_file, true);
  return methods_to_compile_->find(tmp.c_str()) != methods_to_compile_->end();
}

bool CompilerDriver::ShouldCompileBasedOnProfile(const MethodReference& method_ref) const {
  if (profile_compilation_info_ == nullptr) {
    // If we miss profile information it means that we don't do a profile guided compilation.
    // Return true, and let the other filters decide if the method should be compiled.
    return true;
  }
  bool result = profile_compilation_info_->ContainsMethod(method_ref);

  if (kDebugProfileGuidedCompilation) {
    LOG(INFO) << "[ProfileGuidedCompilation] "
        << (result ? "Compiled" : "Skipped") << " method:"
        << PrettyMethod(method_ref.dex_method_index, *method_ref.dex_file, true);
  }
  return result;
}

bool CompilerDriver::ShouldVerifyClassBasedOnProfile(const DexFile& dex_file,
                                                     uint16_t class_idx) const {
  if (!compiler_options_->VerifyOnlyProfile()) {
    // No profile, verify everything.
    return true;
  }
  DCHECK(profile_compilation_info_ != nullptr);
  bool result = profile_compilation_info_->ContainsClass(dex_file, class_idx);
  if (kDebugProfileGuidedCompilation) {
    LOG(INFO) << "[ProfileGuidedCompilation] "
        << (result ? "Verified" : "Skipped") << " method:"
        << dex_file.GetClassDescriptor(dex_file.GetClassDef(class_idx));
  }
  return result;
}

class ResolveCatchBlockExceptionsClassVisitor : public ClassVisitor {
 public:
  ResolveCatchBlockExceptionsClassVisitor(
      std::set<std::pair<uint16_t, const DexFile*>>& exceptions_to_resolve)
     : exceptions_to_resolve_(exceptions_to_resolve) {}

  virtual bool operator()(mirror::Class* c) OVERRIDE SHARED_REQUIRES(Locks::mutator_lock_) {
    const auto pointer_size = Runtime::Current()->GetClassLinker()->GetImagePointerSize();
    for (auto& m : c->GetMethods(pointer_size)) {
      ResolveExceptionsForMethod(&m, pointer_size);
    }
    return true;
  }

 private:
  void ResolveExceptionsForMethod(ArtMethod* method_handle, size_t pointer_size)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    const DexFile::CodeItem* code_item = method_handle->GetCodeItem();
    if (code_item == nullptr) {
      return;  // native or abstract method
    }
    if (code_item->tries_size_ == 0) {
      return;  // nothing to process
    }
    const uint8_t* encoded_catch_handler_list = DexFile::GetCatchHandlerData(*code_item, 0);
    size_t num_encoded_catch_handlers = DecodeUnsignedLeb128(&encoded_catch_handler_list);
    for (size_t i = 0; i < num_encoded_catch_handlers; i++) {
      int32_t encoded_catch_handler_size = DecodeSignedLeb128(&encoded_catch_handler_list);
      bool has_catch_all = false;
      if (encoded_catch_handler_size <= 0) {
        encoded_catch_handler_size = -encoded_catch_handler_size;
        has_catch_all = true;
      }
      for (int32_t j = 0; j < encoded_catch_handler_size; j++) {
        uint16_t encoded_catch_handler_handlers_type_idx =
            DecodeUnsignedLeb128(&encoded_catch_handler_list);
        // Add to set of types to resolve if not already in the dex cache resolved types
        if (!method_handle->IsResolvedTypeIdx(encoded_catch_handler_handlers_type_idx,
                                              pointer_size)) {
          exceptions_to_resolve_.emplace(encoded_catch_handler_handlers_type_idx,
                                         method_handle->GetDexFile());
        }
        // ignore address associated with catch handler
        DecodeUnsignedLeb128(&encoded_catch_handler_list);
      }
      if (has_catch_all) {
        // ignore catch all address
        DecodeUnsignedLeb128(&encoded_catch_handler_list);
      }
    }
  }

  std::set<std::pair<uint16_t, const DexFile*>>& exceptions_to_resolve_;
};

class RecordImageClassesVisitor : public ClassVisitor {
 public:
  explicit RecordImageClassesVisitor(std::unordered_set<std::string>* image_classes)
      : image_classes_(image_classes) {}

  bool operator()(mirror::Class* klass) OVERRIDE SHARED_REQUIRES(Locks::mutator_lock_) {
    std::string temp;
    image_classes_->insert(klass->GetDescriptor(&temp));
    return true;
  }

 private:
  std::unordered_set<std::string>* const image_classes_;
};

// Make a list of descriptors for classes to include in the image
void CompilerDriver::LoadImageClasses(TimingLogger* timings) {
  CHECK(timings != nullptr);
  if (!IsBootImage()) {
    return;
  }

  TimingLogger::ScopedTiming t("LoadImageClasses", timings);
  // Make a first class to load all classes explicitly listed in the file
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  CHECK(image_classes_.get() != nullptr);
  for (auto it = image_classes_->begin(), end = image_classes_->end(); it != end;) {
    const std::string& descriptor(*it);
    StackHandleScope<1> hs(self);
    Handle<mirror::Class> klass(
        hs.NewHandle(class_linker->FindSystemClass(self, descriptor.c_str())));
    if (klass.Get() == nullptr) {
      VLOG(compiler) << "Failed to find class " << descriptor;
      image_classes_->erase(it++);
      self->ClearException();
    } else {
      ++it;
    }
  }

  // Resolve exception classes referenced by the loaded classes. The catch logic assumes
  // exceptions are resolved by the verifier when there is a catch block in an interested method.
  // Do this here so that exception classes appear to have been specified image classes.
  std::set<std::pair<uint16_t, const DexFile*>> unresolved_exception_types;
  StackHandleScope<1> hs(self);
  Handle<mirror::Class> java_lang_Throwable(
      hs.NewHandle(class_linker->FindSystemClass(self, "Ljava/lang/Throwable;")));
  do {
    unresolved_exception_types.clear();
    ResolveCatchBlockExceptionsClassVisitor visitor(unresolved_exception_types);
    class_linker->VisitClasses(&visitor);
    for (const std::pair<uint16_t, const DexFile*>& exception_type : unresolved_exception_types) {
      uint16_t exception_type_idx = exception_type.first;
      const DexFile* dex_file = exception_type.second;
      StackHandleScope<2> hs2(self);
      Handle<mirror::DexCache> dex_cache(hs2.NewHandle(class_linker->RegisterDexFile(*dex_file,
                                                                                     nullptr)));
      Handle<mirror::Class> klass(hs2.NewHandle(
          class_linker->ResolveType(*dex_file,
                                    exception_type_idx,
                                    dex_cache,
                                    ScopedNullHandle<mirror::ClassLoader>())));
      if (klass.Get() == nullptr) {
        const DexFile::TypeId& type_id = dex_file->GetTypeId(exception_type_idx);
        const char* descriptor = dex_file->GetTypeDescriptor(type_id);
        LOG(FATAL) << "Failed to resolve class " << descriptor;
      }
      DCHECK(java_lang_Throwable->IsAssignableFrom(klass.Get()));
    }
    // Resolving exceptions may load classes that reference more exceptions, iterate until no
    // more are found
  } while (!unresolved_exception_types.empty());

  // We walk the roots looking for classes so that we'll pick up the
  // above classes plus any classes them depend on such super
  // classes, interfaces, and the required ClassLinker roots.
  RecordImageClassesVisitor visitor(image_classes_.get());
  class_linker->VisitClasses(&visitor);

  CHECK_NE(image_classes_->size(), 0U);
}

static void MaybeAddToImageClasses(Handle<mirror::Class> c,
                                   std::unordered_set<std::string>* image_classes)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  Thread* self = Thread::Current();
  StackHandleScope<1> hs(self);
  // Make a copy of the handle so that we don't clobber it doing Assign.
  MutableHandle<mirror::Class> klass(hs.NewHandle(c.Get()));
  std::string temp;
  const size_t pointer_size = Runtime::Current()->GetClassLinker()->GetImagePointerSize();
  while (!klass->IsObjectClass()) {
    const char* descriptor = klass->GetDescriptor(&temp);
    std::pair<std::unordered_set<std::string>::iterator, bool> result =
        image_classes->insert(descriptor);
    if (!result.second) {  // Previously inserted.
      break;
    }
    VLOG(compiler) << "Adding " << descriptor << " to image classes";
    for (size_t i = 0; i < klass->NumDirectInterfaces(); ++i) {
      StackHandleScope<1> hs2(self);
      MaybeAddToImageClasses(hs2.NewHandle(mirror::Class::GetDirectInterface(self, klass, i)),
                             image_classes);
    }
    for (auto& m : c->GetVirtualMethods(pointer_size)) {
      StackHandleScope<1> hs2(self);
      MaybeAddToImageClasses(hs2.NewHandle(m.GetDeclaringClass()), image_classes);
    }
    if (klass->IsArrayClass()) {
      StackHandleScope<1> hs2(self);
      MaybeAddToImageClasses(hs2.NewHandle(klass->GetComponentType()), image_classes);
    }
    klass.Assign(klass->GetSuperClass());
  }
}

// Keeps all the data for the update together. Also doubles as the reference visitor.
// Note: we can use object pointers because we suspend all threads.
class ClinitImageUpdate {
 public:
  static ClinitImageUpdate* Create(std::unordered_set<std::string>* image_class_descriptors,
                                   Thread* self, ClassLinker* linker, std::string* error_msg) {
    std::unique_ptr<ClinitImageUpdate> res(new ClinitImageUpdate(image_class_descriptors, self,
                                                                 linker));
    if (res->dex_cache_class_ == nullptr) {
      *error_msg = "Could not find DexCache class.";
      return nullptr;
    }

    return res.release();
  }

  ~ClinitImageUpdate() {
    // Allow others to suspend again.
    self_->EndAssertNoThreadSuspension(old_cause_);
  }

  // Visitor for VisitReferences.
  void operator()(mirror::Object* object, MemberOffset field_offset, bool /* is_static */) const
      SHARED_REQUIRES(Locks::mutator_lock_) {
    mirror::Object* ref = object->GetFieldObject<mirror::Object>(field_offset);
    if (ref != nullptr) {
      VisitClinitClassesObject(ref);
    }
  }

  // java.lang.Reference visitor for VisitReferences.
  void operator()(mirror::Class* klass ATTRIBUTE_UNUSED, mirror::Reference* ref ATTRIBUTE_UNUSED)
      const {}

  // Ignore class native roots.
  void VisitRootIfNonNull(mirror::CompressedReference<mirror::Object>* root ATTRIBUTE_UNUSED)
      const {}
  void VisitRoot(mirror::CompressedReference<mirror::Object>* root ATTRIBUTE_UNUSED) const {}

  void Walk() SHARED_REQUIRES(Locks::mutator_lock_) {
    // Use the initial classes as roots for a search.
    for (mirror::Class* klass_root : image_classes_) {
      VisitClinitClassesObject(klass_root);
    }
  }

 private:
  class FindImageClassesVisitor : public ClassVisitor {
   public:
    explicit FindImageClassesVisitor(ClinitImageUpdate* data) : data_(data) {}

    bool operator()(mirror::Class* klass) OVERRIDE SHARED_REQUIRES(Locks::mutator_lock_) {
      std::string temp;
      const char* name = klass->GetDescriptor(&temp);
      if (data_->image_class_descriptors_->find(name) != data_->image_class_descriptors_->end()) {
        data_->image_classes_.push_back(klass);
      } else {
        // Check whether it is initialized and has a clinit. They must be kept, too.
        if (klass->IsInitialized() && klass->FindClassInitializer(
            Runtime::Current()->GetClassLinker()->GetImagePointerSize()) != nullptr) {
          data_->image_classes_.push_back(klass);
        }
      }
      return true;
    }

   private:
    ClinitImageUpdate* const data_;
  };

  ClinitImageUpdate(std::unordered_set<std::string>* image_class_descriptors, Thread* self,
                    ClassLinker* linker)
      SHARED_REQUIRES(Locks::mutator_lock_) :
      image_class_descriptors_(image_class_descriptors), self_(self) {
    CHECK(linker != nullptr);
    CHECK(image_class_descriptors != nullptr);

    // Make sure nobody interferes with us.
    old_cause_ = self->StartAssertNoThreadSuspension("Boot image closure");

    // Find the interesting classes.
    dex_cache_class_ = linker->LookupClass(self, "Ljava/lang/DexCache;",
        ComputeModifiedUtf8Hash("Ljava/lang/DexCache;"), nullptr);

    // Find all the already-marked classes.
    WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
    FindImageClassesVisitor visitor(this);
    linker->VisitClasses(&visitor);
  }

  void VisitClinitClassesObject(mirror::Object* object) const
      SHARED_REQUIRES(Locks::mutator_lock_) {
    DCHECK(object != nullptr);
    if (marked_objects_.find(object) != marked_objects_.end()) {
      // Already processed.
      return;
    }

    // Mark it.
    marked_objects_.insert(object);

    if (object->IsClass()) {
      // If it is a class, add it.
      StackHandleScope<1> hs(self_);
      MaybeAddToImageClasses(hs.NewHandle(object->AsClass()), image_class_descriptors_);
    } else {
      // Else visit the object's class.
      VisitClinitClassesObject(object->GetClass());
    }

    // If it is not a DexCache, visit all references.
    mirror::Class* klass = object->GetClass();
    if (klass != dex_cache_class_) {
      object->VisitReferences(*this, *this);
    }
  }

  mutable std::unordered_set<mirror::Object*> marked_objects_;
  std::unordered_set<std::string>* const image_class_descriptors_;
  std::vector<mirror::Class*> image_classes_;
  const mirror::Class* dex_cache_class_;
  Thread* const self_;
  const char* old_cause_;

  DISALLOW_COPY_AND_ASSIGN(ClinitImageUpdate);
};

void CompilerDriver::UpdateImageClasses(TimingLogger* timings) {
  if (IsBootImage()) {
    TimingLogger::ScopedTiming t("UpdateImageClasses", timings);

    Runtime* runtime = Runtime::Current();

    // Suspend all threads.
    ScopedSuspendAll ssa(__FUNCTION__);

    std::string error_msg;
    std::unique_ptr<ClinitImageUpdate> update(ClinitImageUpdate::Create(image_classes_.get(),
                                                                        Thread::Current(),
                                                                        runtime->GetClassLinker(),
                                                                        &error_msg));
    CHECK(update.get() != nullptr) << error_msg;  // TODO: Soft failure?

    // Do the marking.
    update->Walk();
  }
}

bool CompilerDriver::CanAssumeClassIsLoaded(mirror::Class* klass) {
  Runtime* runtime = Runtime::Current();
  if (!runtime->IsAotCompiler()) {
    DCHECK(runtime->UseJitCompilation());
    // Having the klass reference here implies that the klass is already loaded.
    return true;
  }
  if (!IsBootImage()) {
    // Assume loaded only if klass is in the boot image. App classes cannot be assumed
    // loaded because we don't even know what class loader will be used to load them.
    bool class_in_image = runtime->GetHeap()->FindSpaceFromObject(klass, false)->IsImageSpace();
    return class_in_image;
  }
  std::string temp;
  const char* descriptor = klass->GetDescriptor(&temp);
  return IsImageClass(descriptor);
}

void CompilerDriver::MarkForDexToDexCompilation(Thread* self, const MethodReference& method_ref) {
  MutexLock lock(self, dex_to_dex_references_lock_);
  // Since we're compiling one dex file at a time, we need to look for the
  // current dex file entry only at the end of dex_to_dex_references_.
  if (dex_to_dex_references_.empty() ||
      &dex_to_dex_references_.back().GetDexFile() != method_ref.dex_file) {
    dex_to_dex_references_.emplace_back(*method_ref.dex_file);
  }
  dex_to_dex_references_.back().GetMethodIndexes().SetBit(method_ref.dex_method_index);
}

bool CompilerDriver::CanAssumeTypeIsPresentInDexCache(Handle<mirror::DexCache> dex_cache,
                                                      uint32_t type_idx) {
  bool result = false;
  if ((IsBootImage() &&
       IsImageClass(dex_cache->GetDexFile()->StringDataByIdx(
           dex_cache->GetDexFile()->GetTypeId(type_idx).descriptor_idx_))) ||
      Runtime::Current()->UseJitCompilation()) {
    mirror::Class* resolved_class = dex_cache->GetResolvedType(type_idx);
    result = (resolved_class != nullptr);
  }

  if (result) {
    stats_->TypeInDexCache();
  } else {
    stats_->TypeNotInDexCache();
  }
  return result;
}

bool CompilerDriver::CanAssumeStringIsPresentInDexCache(const DexFile& dex_file,
                                                        uint32_t string_idx) {
  // See also Compiler::ResolveDexFile

  bool result = false;
  if (IsBootImage() || Runtime::Current()->UseJitCompilation()) {
    ScopedObjectAccess soa(Thread::Current());
    StackHandleScope<1> hs(soa.Self());
    ClassLinker* const class_linker = Runtime::Current()->GetClassLinker();
    Handle<mirror::DexCache> dex_cache(hs.NewHandle(class_linker->FindDexCache(
        soa.Self(), dex_file, false)));
    if (IsBootImage()) {
      // We resolve all const-string strings when building for the image.
      class_linker->ResolveString(dex_file, string_idx, dex_cache);
      result = true;
    } else {
      // Just check whether the dex cache already has the string.
      DCHECK(Runtime::Current()->UseJitCompilation());
      result = (dex_cache->GetResolvedString(string_idx) != nullptr);
    }
  }
  if (result) {
    stats_->StringInDexCache();
  } else {
    stats_->StringNotInDexCache();
  }
  return result;
}

bool CompilerDriver::CanAccessTypeWithoutChecks(uint32_t referrer_idx,
                                                Handle<mirror::DexCache> dex_cache,
                                                uint32_t type_idx) {
  // Get type from dex cache assuming it was populated by the verifier
  mirror::Class* resolved_class = dex_cache->GetResolvedType(type_idx);
  if (resolved_class == nullptr) {
    stats_->TypeNeedsAccessCheck();
    return false;  // Unknown class needs access checks.
  }
  const DexFile::MethodId& method_id = dex_cache->GetDexFile()->GetMethodId(referrer_idx);
  bool is_accessible = resolved_class->IsPublic();  // Public classes are always accessible.
  if (!is_accessible) {
    mirror::Class* referrer_class = dex_cache->GetResolvedType(method_id.class_idx_);
    if (referrer_class == nullptr) {
      stats_->TypeNeedsAccessCheck();
      return false;  // Incomplete referrer knowledge needs access check.
    }
    // Perform access check, will return true if access is ok or false if we're going to have to
    // check this at runtime (for example for class loaders).
    is_accessible = referrer_class->CanAccess(resolved_class);
  }
  if (is_accessible) {
    stats_->TypeDoesntNeedAccessCheck();
  } else {
    stats_->TypeNeedsAccessCheck();
  }
  return is_accessible;
}

bool CompilerDriver::CanAccessInstantiableTypeWithoutChecks(uint32_t referrer_idx,
                                                            Handle<mirror::DexCache> dex_cache,
                                                            uint32_t type_idx,
                                                            bool* finalizable) {
  // Get type from dex cache assuming it was populated by the verifier.
  mirror::Class* resolved_class = dex_cache->GetResolvedType(type_idx);
  if (resolved_class == nullptr) {
    stats_->TypeNeedsAccessCheck();
    // Be conservative.
    *finalizable = true;
    return false;  // Unknown class needs access checks.
  }
  *finalizable = resolved_class->IsFinalizable();
  const DexFile::MethodId& method_id = dex_cache->GetDexFile()->GetMethodId(referrer_idx);
  bool is_accessible = resolved_class->IsPublic();  // Public classes are always accessible.
  if (!is_accessible) {
    mirror::Class* referrer_class = dex_cache->GetResolvedType(method_id.class_idx_);
    if (referrer_class == nullptr) {
      stats_->TypeNeedsAccessCheck();
      return false;  // Incomplete referrer knowledge needs access check.
    }
    // Perform access and instantiable checks, will return true if access is ok or false if we're
    // going to have to check this at runtime (for example for class loaders).
    is_accessible = referrer_class->CanAccess(resolved_class);
  }
  bool result = is_accessible && resolved_class->IsInstantiable();
  if (result) {
    stats_->TypeDoesntNeedAccessCheck();
  } else {
    stats_->TypeNeedsAccessCheck();
  }
  return result;
}

bool CompilerDriver::CanEmbedTypeInCode(const DexFile& dex_file, uint32_t type_idx,
                                        bool* is_type_initialized, bool* use_direct_type_ptr,
                                        uintptr_t* direct_type_ptr, bool* out_is_finalizable) {
  ScopedObjectAccess soa(Thread::Current());
  Runtime* runtime = Runtime::Current();
  mirror::DexCache* dex_cache = runtime->GetClassLinker()->FindDexCache(
      soa.Self(), dex_file, false);
  mirror::Class* resolved_class = dex_cache->GetResolvedType(type_idx);
  if (resolved_class == nullptr) {
    return false;
  }
  if (GetCompilerOptions().GetCompilePic()) {
    // Do not allow a direct class pointer to be used when compiling for position-independent
    return false;
  }
  *out_is_finalizable = resolved_class->IsFinalizable();
  gc::Heap* heap = runtime->GetHeap();
  const bool compiling_boot = heap->IsCompilingBoot();
  const bool support_boot_image_fixup = GetSupportBootImageFixup();
  if (compiling_boot) {
    // boot -> boot class pointers.
    // True if the class is in the image at boot compiling time.
    const bool is_image_class = IsBootImage() && IsImageClass(
        dex_file.StringDataByIdx(dex_file.GetTypeId(type_idx).descriptor_idx_));
    // True if pc relative load works.
    if (is_image_class && support_boot_image_fixup) {
      *is_type_initialized = resolved_class->IsInitialized();
      *use_direct_type_ptr = false;
      *direct_type_ptr = 0;
      return true;
    } else {
      return false;
    }
  } else if (runtime->UseJitCompilation() && !heap->IsMovableObject(resolved_class)) {
    *is_type_initialized = resolved_class->IsInitialized();
    // If the class may move around, then don't embed it as a direct pointer.
    *use_direct_type_ptr = true;
    *direct_type_ptr = reinterpret_cast<uintptr_t>(resolved_class);
    return true;
  } else {
    // True if the class is in the image at app compiling time.
    const bool class_in_image = heap->FindSpaceFromObject(resolved_class, false)->IsImageSpace();
    if (class_in_image && support_boot_image_fixup) {
      // boot -> app class pointers.
      *is_type_initialized = resolved_class->IsInitialized();
      // TODO This is somewhat hacky. We should refactor all of this invoke codepath.
      *use_direct_type_ptr = !GetCompilerOptions().GetIncludePatchInformation();
      *direct_type_ptr = reinterpret_cast<uintptr_t>(resolved_class);
      return true;
    } else {
      // app -> app class pointers.
      // Give up because app does not have an image and class
      // isn't created at compile time.  TODO: implement this
      // if/when each app gets an image.
      return false;
    }
  }
}

bool CompilerDriver::CanEmbedReferenceTypeInCode(ClassReference* ref,
                                                 bool* use_direct_ptr,
                                                 uintptr_t* direct_type_ptr) {
  CHECK(ref != nullptr);
  CHECK(use_direct_ptr != nullptr);
  CHECK(direct_type_ptr != nullptr);

  ScopedObjectAccess soa(Thread::Current());
  mirror::Class* reference_class = mirror::Reference::GetJavaLangRefReference();
  bool is_initialized = false;
  bool unused_finalizable;
  // Make sure we have a finished Reference class object before attempting to use it.
  if (!CanEmbedTypeInCode(*reference_class->GetDexCache()->GetDexFile(),
                          reference_class->GetDexTypeIndex(), &is_initialized,
                          use_direct_ptr, direct_type_ptr, &unused_finalizable) ||
      !is_initialized) {
    return false;
  }
  ref->first = &reference_class->GetDexFile();
  ref->second = reference_class->GetDexClassDefIndex();
  return true;
}

uint32_t CompilerDriver::GetReferenceSlowFlagOffset() const {
  ScopedObjectAccess soa(Thread::Current());
  mirror::Class* klass = mirror::Reference::GetJavaLangRefReference();
  DCHECK(klass->IsInitialized());
  return klass->GetSlowPathFlagOffset().Uint32Value();
}

uint32_t CompilerDriver::GetReferenceDisableFlagOffset() const {
  ScopedObjectAccess soa(Thread::Current());
  mirror::Class* klass = mirror::Reference::GetJavaLangRefReference();
  DCHECK(klass->IsInitialized());
  return klass->GetDisableIntrinsicFlagOffset().Uint32Value();
}

DexCacheArraysLayout CompilerDriver::GetDexCacheArraysLayout(const DexFile* dex_file) {
  return ContainsElement(GetDexFilesForOatFile(), dex_file)
      ? DexCacheArraysLayout(GetInstructionSetPointerSize(instruction_set_), dex_file)
      : DexCacheArraysLayout();
}

void CompilerDriver::ProcessedInstanceField(bool resolved) {
  if (!resolved) {
    stats_->UnresolvedInstanceField();
  } else {
    stats_->ResolvedInstanceField();
  }
}

void CompilerDriver::ProcessedStaticField(bool resolved, bool local) {
  if (!resolved) {
    stats_->UnresolvedStaticField();
  } else if (local) {
    stats_->ResolvedLocalStaticField();
  } else {
    stats_->ResolvedStaticField();
  }
}

void CompilerDriver::ProcessedInvoke(InvokeType invoke_type, int flags) {
  stats_->ProcessedInvoke(invoke_type, flags);
}

ArtField* CompilerDriver::ComputeInstanceFieldInfo(uint32_t field_idx,
                                                   const DexCompilationUnit* mUnit, bool is_put,
                                                   const ScopedObjectAccess& soa) {
  // Try to resolve the field and compiling method's class.
  ArtField* resolved_field;
  mirror::Class* referrer_class;
  Handle<mirror::DexCache> dex_cache(mUnit->GetDexCache());
  {
    StackHandleScope<1> hs(soa.Self());
    Handle<mirror::ClassLoader> class_loader_handle(
        hs.NewHandle(soa.Decode<mirror::ClassLoader*>(mUnit->GetClassLoader())));
    resolved_field = ResolveField(soa, dex_cache, class_loader_handle, mUnit, field_idx, false);
    referrer_class = resolved_field != nullptr
        ? ResolveCompilingMethodsClass(soa, dex_cache, class_loader_handle, mUnit) : nullptr;
  }
  bool can_link = false;
  if (resolved_field != nullptr && referrer_class != nullptr) {
    std::pair<bool, bool> fast_path = IsFastInstanceField(
        dex_cache.Get(), referrer_class, resolved_field, field_idx);
    can_link = is_put ? fast_path.second : fast_path.first;
  }
  ProcessedInstanceField(can_link);
  return can_link ? resolved_field : nullptr;
}

bool CompilerDriver::ComputeInstanceFieldInfo(uint32_t field_idx, const DexCompilationUnit* mUnit,
                                              bool is_put, MemberOffset* field_offset,
                                              bool* is_volatile) {
  ScopedObjectAccess soa(Thread::Current());
  ArtField* resolved_field = ComputeInstanceFieldInfo(field_idx, mUnit, is_put, soa);

  if (resolved_field == nullptr) {
    // Conservative defaults.
    *is_volatile = true;
    *field_offset = MemberOffset(static_cast<size_t>(-1));
    return false;
  } else {
    *is_volatile = resolved_field->IsVolatile();
    *field_offset = resolved_field->GetOffset();
    return true;
  }
}

void CompilerDriver::GetCodeAndMethodForDirectCall(InvokeType* type, InvokeType sharp_type,
                                                   bool no_guarantee_of_dex_cache_entry,
                                                   const mirror::Class* referrer_class,
                                                   ArtMethod* method,
                                                   int* stats_flags,
                                                   MethodReference* target_method,
                                                   uintptr_t* direct_code,
                                                   uintptr_t* direct_method) {
  // For direct and static methods compute possible direct_code and direct_method values, ie
  // an address for the Method* being invoked and an address of the code for that Method*.
  // For interface calls compute a value for direct_method that is the interface method being
  // invoked, so this can be passed to the out-of-line runtime support code.
  *direct_code = 0;
  *direct_method = 0;
  Runtime* const runtime = Runtime::Current();
  gc::Heap* const heap = runtime->GetHeap();
  auto* cl = runtime->GetClassLinker();
  const auto pointer_size = cl->GetImagePointerSize();
  bool use_dex_cache = GetCompilerOptions().GetCompilePic();  // Off by default
  const bool compiling_boot = heap->IsCompilingBoot();
  // TODO This is somewhat hacky. We should refactor all of this invoke codepath.
  const bool force_relocations = (compiling_boot ||
                                  GetCompilerOptions().GetIncludePatchInformation());
  if (sharp_type != kStatic && sharp_type != kDirect) {
    return;
  }
  // TODO: support patching on all architectures.
  use_dex_cache = use_dex_cache || (force_relocations && !support_boot_image_fixup_);
  mirror::Class* declaring_class = method->GetDeclaringClass();
  bool method_code_in_boot = declaring_class->GetClassLoader() == nullptr;
  if (!use_dex_cache) {
    if (!method_code_in_boot) {
      use_dex_cache = true;
    } else {
      bool has_clinit_trampoline =
          method->IsStatic() && !declaring_class->IsInitialized();
      if (has_clinit_trampoline && declaring_class != referrer_class) {
        // Ensure we run the clinit trampoline unless we are invoking a static method in the same
        // class.
        use_dex_cache = true;
      }
    }
  }
  if (runtime->UseJitCompilation()) {
    // If we are the JIT, then don't allow a direct call to the interpreter bridge since this will
    // never be updated even after we compile the method.
    if (cl->IsQuickToInterpreterBridge(
        reinterpret_cast<const void*>(compiler_->GetEntryPointOf(method)))) {
      use_dex_cache = true;
    }
  }
  if (method_code_in_boot) {
    *stats_flags |= kFlagDirectCallToBoot | kFlagDirectMethodToBoot;
  }
  if (!use_dex_cache && force_relocations) {
    bool is_in_image;
    if (IsBootImage()) {
      is_in_image = IsImageClass(method->GetDeclaringClassDescriptor());
    } else {
      is_in_image = instruction_set_ != kX86 && instruction_set_ != kX86_64 &&
                    heap->FindSpaceFromObject(method->GetDeclaringClass(), false)->IsImageSpace() &&
                    !cl->IsQuickToInterpreterBridge(
                        reinterpret_cast<const void*>(compiler_->GetEntryPointOf(method)));
    }
    if (!is_in_image) {
      // We can only branch directly to Methods that are resolved in the DexCache.
      // Otherwise we won't invoke the resolution trampoline.
      use_dex_cache = true;
    }
  }
  // The method is defined not within this dex file. We need a dex cache slot within the current
  // dex file or direct pointers.
  bool must_use_direct_pointers = false;
  mirror::DexCache* dex_cache = declaring_class->GetDexCache();
  if (target_method->dex_file == dex_cache->GetDexFile() &&
    !(runtime->UseJitCompilation() && dex_cache->GetResolvedMethod(
        method->GetDexMethodIndex(), pointer_size) == nullptr)) {
    target_method->dex_method_index = method->GetDexMethodIndex();
  } else {
    if (no_guarantee_of_dex_cache_entry) {
      // See if the method is also declared in this dex cache.
      uint32_t dex_method_idx = method->FindDexMethodIndexInOtherDexFile(
          *target_method->dex_file, target_method->dex_method_index);
      if (dex_method_idx != DexFile::kDexNoIndex) {
        target_method->dex_method_index = dex_method_idx;
      } else {
        if (force_relocations && !use_dex_cache) {
          target_method->dex_method_index = method->GetDexMethodIndex();
          target_method->dex_file = dex_cache->GetDexFile();
        }
        must_use_direct_pointers = true;
      }
    }
  }
  if (use_dex_cache) {
    if (must_use_direct_pointers) {
      // Fail. Test above showed the only safe dispatch was via the dex cache, however, the direct
      // pointers are required as the dex cache lacks an appropriate entry.
      VLOG(compiler) << "Dex cache devirtualization failed for: " << PrettyMethod(method);
    } else {
      *type = sharp_type;
    }
  } else {
    bool method_in_image = false;
    const std::vector<gc::space::ImageSpace*> image_spaces = heap->GetBootImageSpaces();
    for (gc::space::ImageSpace* image_space : image_spaces) {
      const auto& method_section = image_space->GetImageHeader().GetMethodsSection();
      if (method_section.Contains(reinterpret_cast<uint8_t*>(method) - image_space->Begin())) {
        method_in_image = true;
        break;
      }
    }
    if (method_in_image || compiling_boot || runtime->UseJitCompilation()) {
      // We know we must be able to get to the method in the image, so use that pointer.
      // In the case where we are the JIT, we can always use direct pointers since we know where
      // the method and its code are / will be. We don't sharpen to interpreter bridge since we
      // check IsQuickToInterpreterBridge above.
      CHECK(!method->IsAbstract());
      *type = sharp_type;
      *direct_method = force_relocations ? -1 : reinterpret_cast<uintptr_t>(method);
      *direct_code = force_relocations ? -1 : compiler_->GetEntryPointOf(method);
      target_method->dex_file = method->GetDeclaringClass()->GetDexCache()->GetDexFile();
      target_method->dex_method_index = method->GetDexMethodIndex();
    } else if (!must_use_direct_pointers) {
      // Set the code and rely on the dex cache for the method.
      *type = sharp_type;
      if (force_relocations) {
        *direct_code = -1;
        target_method->dex_file = method->GetDeclaringClass()->GetDexCache()->GetDexFile();
        target_method->dex_method_index = method->GetDexMethodIndex();
      } else {
        *direct_code = compiler_->GetEntryPointOf(method);
      }
    } else {
      // Direct pointers were required but none were available.
      VLOG(compiler) << "Dex cache devirtualization failed for: " << PrettyMethod(method);
    }
  }
}

bool CompilerDriver::ComputeInvokeInfo(const DexCompilationUnit* mUnit, const uint32_t dex_pc,
                                       bool update_stats, bool enable_devirtualization,
                                       InvokeType* invoke_type, MethodReference* target_method,
                                       int* vtable_idx, uintptr_t* direct_code,
                                       uintptr_t* direct_method) {
  InvokeType orig_invoke_type = *invoke_type;
  int stats_flags = 0;
  ScopedObjectAccess soa(Thread::Current());
  // Try to resolve the method and compiling method's class.
  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::DexCache> dex_cache(mUnit->GetDexCache());
  Handle<mirror::ClassLoader> class_loader(hs.NewHandle(
      soa.Decode<mirror::ClassLoader*>(mUnit->GetClassLoader())));
  uint32_t method_idx = target_method->dex_method_index;
  ArtMethod* resolved_method = ResolveMethod(
      soa, dex_cache, class_loader, mUnit, method_idx, orig_invoke_type);
  auto h_referrer_class = hs.NewHandle(resolved_method != nullptr ?
      ResolveCompilingMethodsClass(soa, dex_cache, class_loader, mUnit) : nullptr);
  bool result = false;
  if (resolved_method != nullptr) {
    *vtable_idx = GetResolvedMethodVTableIndex(resolved_method, orig_invoke_type);

    if (enable_devirtualization && mUnit->GetVerifiedMethod() != nullptr) {
      const MethodReference* devirt_target = mUnit->GetVerifiedMethod()->GetDevirtTarget(dex_pc);

      stats_flags = IsFastInvoke(
          soa, dex_cache, class_loader, mUnit, h_referrer_class.Get(), resolved_method,
          invoke_type, target_method, devirt_target, direct_code, direct_method);
      result = stats_flags != 0;
    } else {
      // Devirtualization not enabled. Inline IsFastInvoke(), dropping the devirtualization parts.
      if (UNLIKELY(h_referrer_class.Get() == nullptr) ||
          UNLIKELY(!h_referrer_class->CanAccessResolvedMethod(resolved_method->GetDeclaringClass(),
                                                            resolved_method, dex_cache.Get(),
                                                            target_method->dex_method_index)) ||
          *invoke_type == kSuper) {
        // Slow path. (Without devirtualization, all super calls go slow path as well.)
      } else {
        // Sharpening failed so generate a regular resolved method dispatch.
        stats_flags = kFlagMethodResolved;
        GetCodeAndMethodForDirectCall(
            invoke_type, *invoke_type, false, h_referrer_class.Get(), resolved_method, &stats_flags,
            target_method, direct_code, direct_method);
        result = true;
      }
    }
  }
  if (!result) {
    // Conservative defaults.
    *vtable_idx = -1;
    *direct_code = 0u;
    *direct_method = 0u;
  }
  if (update_stats) {
    ProcessedInvoke(orig_invoke_type, stats_flags);
  }
  return result;
}

const VerifiedMethod* CompilerDriver::GetVerifiedMethod(const DexFile* dex_file,
                                                        uint32_t method_idx) const {
  MethodReference ref(dex_file, method_idx);
  return verification_results_->GetVerifiedMethod(ref);
}

bool CompilerDriver::IsSafeCast(const DexCompilationUnit* mUnit, uint32_t dex_pc) {
  if (!compiler_options_->IsVerificationEnabled()) {
    // If we didn't verify, every cast has to be treated as non-safe.
    return false;
  }
  DCHECK(mUnit->GetVerifiedMethod() != nullptr);
  bool result = mUnit->GetVerifiedMethod()->IsSafeCast(dex_pc);
  if (result) {
    stats_->SafeCast();
  } else {
    stats_->NotASafeCast();
  }
  return result;
}

class CompilationVisitor {
 public:
  virtual ~CompilationVisitor() {}
  virtual void Visit(size_t index) = 0;
};

class ParallelCompilationManager {
 public:
  ParallelCompilationManager(ClassLinker* class_linker,
                             jobject class_loader,
                             CompilerDriver* compiler,
                             const DexFile* dex_file,
                             const std::vector<const DexFile*>& dex_files,
                             ThreadPool* thread_pool)
    : index_(0),
      class_linker_(class_linker),
      class_loader_(class_loader),
      compiler_(compiler),
      dex_file_(dex_file),
      dex_files_(dex_files),
      thread_pool_(thread_pool) {}

  ClassLinker* GetClassLinker() const {
    CHECK(class_linker_ != nullptr);
    return class_linker_;
  }

  jobject GetClassLoader() const {
    return class_loader_;
  }

  CompilerDriver* GetCompiler() const {
    CHECK(compiler_ != nullptr);
    return compiler_;
  }

  const DexFile* GetDexFile() const {
    CHECK(dex_file_ != nullptr);
    return dex_file_;
  }

  const std::vector<const DexFile*>& GetDexFiles() const {
    return dex_files_;
  }

  void ForAll(size_t begin, size_t end, CompilationVisitor* visitor, size_t work_units)
      REQUIRES(!*Locks::mutator_lock_) {
    Thread* self = Thread::Current();
    self->AssertNoPendingException();
    CHECK_GT(work_units, 0U);

    index_.StoreRelaxed(begin);
    for (size_t i = 0; i < work_units; ++i) {
      thread_pool_->AddTask(self, new ForAllClosure(this, end, visitor));
    }
    thread_pool_->StartWorkers(self);

    // Ensure we're suspended while we're blocked waiting for the other threads to finish (worker
    // thread destructor's called below perform join).
    CHECK_NE(self->GetState(), kRunnable);

    // Wait for all the worker threads to finish.
    thread_pool_->Wait(self, true, false);

    // And stop the workers accepting jobs.
    thread_pool_->StopWorkers(self);
  }

  size_t NextIndex() {
    return index_.FetchAndAddSequentiallyConsistent(1);
  }

 private:
  class ForAllClosure : public Task {
   public:
    ForAllClosure(ParallelCompilationManager* manager, size_t end, CompilationVisitor* visitor)
        : manager_(manager),
          end_(end),
          visitor_(visitor) {}

    virtual void Run(Thread* self) {
      while (true) {
        const size_t index = manager_->NextIndex();
        if (UNLIKELY(index >= end_)) {
          break;
        }
        visitor_->Visit(index);
        self->AssertNoPendingException();
      }
    }

    virtual void Finalize() {
      delete this;
    }

   private:
    ParallelCompilationManager* const manager_;
    const size_t end_;
    CompilationVisitor* const visitor_;
  };

  AtomicInteger index_;
  ClassLinker* const class_linker_;
  const jobject class_loader_;
  CompilerDriver* const compiler_;
  const DexFile* const dex_file_;
  const std::vector<const DexFile*>& dex_files_;
  ThreadPool* const thread_pool_;

  DISALLOW_COPY_AND_ASSIGN(ParallelCompilationManager);
};

// A fast version of SkipClass above if the class pointer is available
// that avoids the expensive FindInClassPath search.
static bool SkipClass(jobject class_loader, const DexFile& dex_file, mirror::Class* klass)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  DCHECK(klass != nullptr);
  const DexFile& original_dex_file = *klass->GetDexCache()->GetDexFile();
  if (&dex_file != &original_dex_file) {
    if (class_loader == nullptr) {
      LOG(WARNING) << "Skipping class " << PrettyDescriptor(klass) << " from "
                   << dex_file.GetLocation() << " previously found in "
                   << original_dex_file.GetLocation();
    }
    return true;
  }
  return false;
}

static void CheckAndClearResolveException(Thread* self)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  CHECK(self->IsExceptionPending());
  mirror::Throwable* exception = self->GetException();
  std::string temp;
  const char* descriptor = exception->GetClass()->GetDescriptor(&temp);
  const char* expected_exceptions[] = {
      "Ljava/lang/IllegalAccessError;",
      "Ljava/lang/IncompatibleClassChangeError;",
      "Ljava/lang/InstantiationError;",
      "Ljava/lang/LinkageError;",
      "Ljava/lang/NoClassDefFoundError;",
      "Ljava/lang/NoSuchFieldError;",
      "Ljava/lang/NoSuchMethodError;"
  };
  bool found = false;
  for (size_t i = 0; (found == false) && (i < arraysize(expected_exceptions)); ++i) {
    if (strcmp(descriptor, expected_exceptions[i]) == 0) {
      found = true;
    }
  }
  if (!found) {
    LOG(FATAL) << "Unexpected exception " << exception->Dump();
  }
  self->ClearException();
}

bool CompilerDriver::RequiresConstructorBarrier(const DexFile& dex_file,
                                                uint16_t class_def_idx) const {
  const DexFile::ClassDef& class_def = dex_file.GetClassDef(class_def_idx);
  const uint8_t* class_data = dex_file.GetClassData(class_def);
  if (class_data == nullptr) {
    // Empty class such as a marker interface.
    return false;
  }
  ClassDataItemIterator it(dex_file, class_data);
  while (it.HasNextStaticField()) {
    it.Next();
  }
  // We require a constructor barrier if there are final instance fields.
  while (it.HasNextInstanceField()) {
    if (it.MemberIsFinal()) {
      return true;
    }
    it.Next();
  }
  return false;
}

class ResolveClassFieldsAndMethodsVisitor : public CompilationVisitor {
 public:
  explicit ResolveClassFieldsAndMethodsVisitor(const ParallelCompilationManager* manager)
      : manager_(manager) {}

  void Visit(size_t class_def_index) OVERRIDE REQUIRES(!Locks::mutator_lock_) {
    ATRACE_CALL();
    Thread* const self = Thread::Current();
    jobject jclass_loader = manager_->GetClassLoader();
    const DexFile& dex_file = *manager_->GetDexFile();
    ClassLinker* class_linker = manager_->GetClassLinker();

    // If an instance field is final then we need to have a barrier on the return, static final
    // fields are assigned within the lock held for class initialization. Conservatively assume
    // constructor barriers are always required.
    bool requires_constructor_barrier = true;

    // Method and Field are the worst. We can't resolve without either
    // context from the code use (to disambiguate virtual vs direct
    // method and instance vs static field) or from class
    // definitions. While the compiler will resolve what it can as it
    // needs it, here we try to resolve fields and methods used in class
    // definitions, since many of them many never be referenced by
    // generated code.
    const DexFile::ClassDef& class_def = dex_file.GetClassDef(class_def_index);
    ScopedObjectAccess soa(self);
    StackHandleScope<2> hs(soa.Self());
    Handle<mirror::ClassLoader> class_loader(
        hs.NewHandle(soa.Decode<mirror::ClassLoader*>(jclass_loader)));
    Handle<mirror::DexCache> dex_cache(hs.NewHandle(class_linker->FindDexCache(
        soa.Self(), dex_file, false)));
    // Resolve the class.
    mirror::Class* klass = class_linker->ResolveType(dex_file, class_def.class_idx_, dex_cache,
                                                     class_loader);
    bool resolve_fields_and_methods;
    if (klass == nullptr) {
      // Class couldn't be resolved, for example, super-class is in a different dex file. Don't
      // attempt to resolve methods and fields when there is no declaring class.
      CheckAndClearResolveException(soa.Self());
      resolve_fields_and_methods = false;
    } else {
      // We successfully resolved a class, should we skip it?
      if (SkipClass(jclass_loader, dex_file, klass)) {
        return;
      }
      // We want to resolve the methods and fields eagerly.
      resolve_fields_and_methods = true;
    }
    // Note the class_data pointer advances through the headers,
    // static fields, instance fields, direct methods, and virtual
    // methods.
    const uint8_t* class_data = dex_file.GetClassData(class_def);
    if (class_data == nullptr) {
      // Empty class such as a marker interface.
      requires_constructor_barrier = false;
    } else {
      ClassDataItemIterator it(dex_file, class_data);
      while (it.HasNextStaticField()) {
        if (resolve_fields_and_methods) {
          ArtField* field = class_linker->ResolveField(dex_file, it.GetMemberIndex(),
                                                               dex_cache, class_loader, true);
          if (field == nullptr) {
            CheckAndClearResolveException(soa.Self());
          }
        }
        it.Next();
      }
      // We require a constructor barrier if there are final instance fields.
      requires_constructor_barrier = false;
      while (it.HasNextInstanceField()) {
        if (it.MemberIsFinal()) {
          requires_constructor_barrier = true;
        }
        if (resolve_fields_and_methods) {
          ArtField* field = class_linker->ResolveField(dex_file, it.GetMemberIndex(),
                                                               dex_cache, class_loader, false);
          if (field == nullptr) {
            CheckAndClearResolveException(soa.Self());
          }
        }
        it.Next();
      }
      if (resolve_fields_and_methods) {
        while (it.HasNextDirectMethod()) {
          ArtMethod* method = class_linker->ResolveMethod<ClassLinker::kNoICCECheckForCache>(
              dex_file, it.GetMemberIndex(), dex_cache, class_loader, nullptr,
              it.GetMethodInvokeType(class_def));
          if (method == nullptr) {
            CheckAndClearResolveException(soa.Self());
          }
          it.Next();
        }
        while (it.HasNextVirtualMethod()) {
          ArtMethod* method = class_linker->ResolveMethod<ClassLinker::kNoICCECheckForCache>(
              dex_file, it.GetMemberIndex(), dex_cache, class_loader, nullptr,
              it.GetMethodInvokeType(class_def));
          if (method == nullptr) {
            CheckAndClearResolveException(soa.Self());
          }
          it.Next();
        }
        DCHECK(!it.HasNext());
      }
    }
    manager_->GetCompiler()->SetRequiresConstructorBarrier(self,
                                                           &dex_file,
                                                           class_def_index,
                                                           requires_constructor_barrier);
  }

 private:
  const ParallelCompilationManager* const manager_;
};

class ResolveTypeVisitor : public CompilationVisitor {
 public:
  explicit ResolveTypeVisitor(const ParallelCompilationManager* manager) : manager_(manager) {
  }
  virtual void Visit(size_t type_idx) OVERRIDE REQUIRES(!Locks::mutator_lock_) {
  // Class derived values are more complicated, they require the linker and loader.
    ScopedObjectAccess soa(Thread::Current());
    ClassLinker* class_linker = manager_->GetClassLinker();
    const DexFile& dex_file = *manager_->GetDexFile();
    StackHandleScope<2> hs(soa.Self());
    Handle<mirror::ClassLoader> class_loader(
        hs.NewHandle(soa.Decode<mirror::ClassLoader*>(manager_->GetClassLoader())));
    Handle<mirror::DexCache> dex_cache(hs.NewHandle(class_linker->RegisterDexFile(
        dex_file,
        class_loader.Get())));
    mirror::Class* klass = class_linker->ResolveType(dex_file, type_idx, dex_cache, class_loader);

    if (klass == nullptr) {
      soa.Self()->AssertPendingException();
      mirror::Throwable* exception = soa.Self()->GetException();
      VLOG(compiler) << "Exception during type resolution: " << exception->Dump();
      if (exception->GetClass()->DescriptorEquals("Ljava/lang/OutOfMemoryError;")) {
        // There's little point continuing compilation if the heap is exhausted.
        LOG(FATAL) << "Out of memory during type resolution for compilation";
      }
      soa.Self()->ClearException();
    }
  }

 private:
  const ParallelCompilationManager* const manager_;
};

void CompilerDriver::ResolveDexFile(jobject class_loader,
                                    const DexFile& dex_file,
                                    const std::vector<const DexFile*>& dex_files,
                                    ThreadPool* thread_pool,
                                    size_t thread_count,
                                    TimingLogger* timings) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();

  // TODO: we could resolve strings here, although the string table is largely filled with class
  //       and method names.

  ParallelCompilationManager context(class_linker, class_loader, this, &dex_file, dex_files,
                                     thread_pool);
  if (IsBootImage()) {
    // For images we resolve all types, such as array, whereas for applications just those with
    // classdefs are resolved by ResolveClassFieldsAndMethods.
    TimingLogger::ScopedTiming t("Resolve Types", timings);
    ResolveTypeVisitor visitor(&context);
    context.ForAll(0, dex_file.NumTypeIds(), &visitor, thread_count);
  }

  TimingLogger::ScopedTiming t("Resolve MethodsAndFields", timings);
  ResolveClassFieldsAndMethodsVisitor visitor(&context);
  context.ForAll(0, dex_file.NumClassDefs(), &visitor, thread_count);
}

void CompilerDriver::SetVerified(jobject class_loader,
                                 const std::vector<const DexFile*>& dex_files,
                                 TimingLogger* timings) {
  // This can be run in parallel.
  for (const DexFile* dex_file : dex_files) {
    CHECK(dex_file != nullptr);
    SetVerifiedDexFile(class_loader,
                       *dex_file,
                       dex_files,
                       parallel_thread_pool_.get(),
                       parallel_thread_count_,
                       timings);
  }
}

void CompilerDriver::Verify(jobject class_loader,
                            const std::vector<const DexFile*>& dex_files,
                            TimingLogger* timings) {
  // Note: verification should not be pulling in classes anymore when compiling the boot image,
  //       as all should have been resolved before. As such, doing this in parallel should still
  //       be deterministic.
  for (const DexFile* dex_file : dex_files) {
    CHECK(dex_file != nullptr);
    VerifyDexFile(class_loader,
                  *dex_file,
                  dex_files,
                  parallel_thread_pool_.get(),
                  parallel_thread_count_,
                  timings);
  }
}

class VerifyClassVisitor : public CompilationVisitor {
 public:
  VerifyClassVisitor(const ParallelCompilationManager* manager, LogSeverity log_level)
     : manager_(manager), log_level_(log_level) {}

  virtual void Visit(size_t class_def_index) REQUIRES(!Locks::mutator_lock_) OVERRIDE {
    ATRACE_CALL();
    ScopedObjectAccess soa(Thread::Current());
    const DexFile& dex_file = *manager_->GetDexFile();
    if (!manager_->GetCompiler()->ShouldVerifyClassBasedOnProfile(dex_file, class_def_index)) {
      // Skip verification since the class is not in the profile.
      return;
    }
    const DexFile::ClassDef& class_def = dex_file.GetClassDef(class_def_index);
    const char* descriptor = dex_file.GetClassDescriptor(class_def);
    ClassLinker* class_linker = manager_->GetClassLinker();
    jobject jclass_loader = manager_->GetClassLoader();
    StackHandleScope<3> hs(soa.Self());
    Handle<mirror::ClassLoader> class_loader(
        hs.NewHandle(soa.Decode<mirror::ClassLoader*>(jclass_loader)));
    Handle<mirror::Class> klass(
        hs.NewHandle(class_linker->FindClass(soa.Self(), descriptor, class_loader)));
    if (klass.Get() == nullptr) {
      CHECK(soa.Self()->IsExceptionPending());
      soa.Self()->ClearException();

      /*
       * At compile time, we can still structurally verify the class even if FindClass fails.
       * This is to ensure the class is structurally sound for compilation. An unsound class
       * will be rejected by the verifier and later skipped during compilation in the compiler.
       */
      Handle<mirror::DexCache> dex_cache(hs.NewHandle(class_linker->FindDexCache(
          soa.Self(), dex_file, false)));
      std::string error_msg;
      if (verifier::MethodVerifier::VerifyClass(soa.Self(),
                                                &dex_file,
                                                dex_cache,
                                                class_loader,
                                                &class_def,
                                                Runtime::Current()->GetCompilerCallbacks(),
                                                true /* allow soft failures */,
                                                log_level_,
                                                &error_msg) ==
                                                    verifier::MethodVerifier::kHardFailure) {
        LOG(ERROR) << "Verification failed on class " << PrettyDescriptor(descriptor)
                   << " because: " << error_msg;
        manager_->GetCompiler()->SetHadHardVerifierFailure();
      }
    } else if (!SkipClass(jclass_loader, dex_file, klass.Get())) {
      CHECK(klass->IsResolved()) << PrettyClass(klass.Get());
      class_linker->VerifyClass(soa.Self(), klass, log_level_);

      if (klass->IsErroneous()) {
        // ClassLinker::VerifyClass throws, which isn't useful in the compiler.
        CHECK(soa.Self()->IsExceptionPending());
        soa.Self()->ClearException();
        manager_->GetCompiler()->SetHadHardVerifierFailure();
      }

      CHECK(klass->IsCompileTimeVerified() || klass->IsErroneous())
          << PrettyDescriptor(klass.Get()) << ": state=" << klass->GetStatus();

      // It is *very* problematic if there are verification errors in the boot classpath. For example,
      // we rely on things working OK without verification when the decryption dialog is brought up.
      // So abort in a debug build if we find this violated.
      DCHECK(!manager_->GetCompiler()->IsBootImage() || klass->IsVerified())
          << "Boot classpath class " << PrettyClass(klass.Get()) << " failed to fully verify.";
    }
    soa.Self()->AssertNoPendingException();
  }

 private:
  const ParallelCompilationManager* const manager_;
  const LogSeverity log_level_;
};

void CompilerDriver::VerifyDexFile(jobject class_loader,
                                   const DexFile& dex_file,
                                   const std::vector<const DexFile*>& dex_files,
                                   ThreadPool* thread_pool,
                                   size_t thread_count,
                                   TimingLogger* timings) {
  TimingLogger::ScopedTiming t("Verify Dex File", timings);
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  ParallelCompilationManager context(class_linker, class_loader, this, &dex_file, dex_files,
                                     thread_pool);
  LogSeverity log_level = GetCompilerOptions().AbortOnHardVerifierFailure()
                              ? LogSeverity::INTERNAL_FATAL
                              : LogSeverity::WARNING;
  VerifyClassVisitor visitor(&context, log_level);
  context.ForAll(0, dex_file.NumClassDefs(), &visitor, thread_count);
}

class SetVerifiedClassVisitor : public CompilationVisitor {
 public:
  explicit SetVerifiedClassVisitor(const ParallelCompilationManager* manager) : manager_(manager) {}

  virtual void Visit(size_t class_def_index) REQUIRES(!Locks::mutator_lock_) OVERRIDE {
    ATRACE_CALL();
    ScopedObjectAccess soa(Thread::Current());
    const DexFile& dex_file = *manager_->GetDexFile();
    const DexFile::ClassDef& class_def = dex_file.GetClassDef(class_def_index);
    const char* descriptor = dex_file.GetClassDescriptor(class_def);
    ClassLinker* class_linker = manager_->GetClassLinker();
    jobject jclass_loader = manager_->GetClassLoader();
    StackHandleScope<3> hs(soa.Self());
    Handle<mirror::ClassLoader> class_loader(
        hs.NewHandle(soa.Decode<mirror::ClassLoader*>(jclass_loader)));
    Handle<mirror::Class> klass(
        hs.NewHandle(class_linker->FindClass(soa.Self(), descriptor, class_loader)));
    // Class might have failed resolution. Then don't set it to verified.
    if (klass.Get() != nullptr) {
      // Only do this if the class is resolved. If even resolution fails, quickening will go very,
      // very wrong.
      if (klass->IsResolved()) {
        if (klass->GetStatus() < mirror::Class::kStatusVerified) {
          ObjectLock<mirror::Class> lock(soa.Self(), klass);
          // Set class status to verified.
          mirror::Class::SetStatus(klass, mirror::Class::kStatusVerified, soa.Self());
          // Mark methods as pre-verified. If we don't do this, the interpreter will run with
          // access checks.
          klass->SetSkipAccessChecksFlagOnAllMethods(
              GetInstructionSetPointerSize(manager_->GetCompiler()->GetInstructionSet()));
          klass->SetVerificationAttempted();
        }
        // Record the final class status if necessary.
        ClassReference ref(manager_->GetDexFile(), class_def_index);
        manager_->GetCompiler()->RecordClassStatus(ref, klass->GetStatus());
      }
    } else {
      Thread* self = soa.Self();
      DCHECK(self->IsExceptionPending());
      self->ClearException();
    }
  }

 private:
  const ParallelCompilationManager* const manager_;
};

void CompilerDriver::SetVerifiedDexFile(jobject class_loader,
                                        const DexFile& dex_file,
                                        const std::vector<const DexFile*>& dex_files,
                                        ThreadPool* thread_pool,
                                        size_t thread_count,
                                        TimingLogger* timings) {
  TimingLogger::ScopedTiming t("Verify Dex File", timings);
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  ParallelCompilationManager context(class_linker, class_loader, this, &dex_file, dex_files,
                                     thread_pool);
  SetVerifiedClassVisitor visitor(&context);
  context.ForAll(0, dex_file.NumClassDefs(), &visitor, thread_count);
}

class InitializeClassVisitor : public CompilationVisitor {
 public:
  explicit InitializeClassVisitor(const ParallelCompilationManager* manager) : manager_(manager) {}

  virtual void Visit(size_t class_def_index) REQUIRES(!Locks::mutator_lock_) OVERRIDE {
    ATRACE_CALL();
    jobject jclass_loader = manager_->GetClassLoader();
    const DexFile& dex_file = *manager_->GetDexFile();
    const DexFile::ClassDef& class_def = dex_file.GetClassDef(class_def_index);
    const DexFile::TypeId& class_type_id = dex_file.GetTypeId(class_def.class_idx_);
    const char* descriptor = dex_file.StringDataByIdx(class_type_id.descriptor_idx_);

    ScopedObjectAccess soa(Thread::Current());
    StackHandleScope<3> hs(soa.Self());
    Handle<mirror::ClassLoader> class_loader(
        hs.NewHandle(soa.Decode<mirror::ClassLoader*>(jclass_loader)));
    Handle<mirror::Class> klass(
        hs.NewHandle(manager_->GetClassLinker()->FindClass(soa.Self(), descriptor, class_loader)));

    if (klass.Get() != nullptr && !SkipClass(jclass_loader, dex_file, klass.Get())) {
      // Only try to initialize classes that were successfully verified.
      if (klass->IsVerified()) {
        // Attempt to initialize the class but bail if we either need to initialize the super-class
        // or static fields.
        manager_->GetClassLinker()->EnsureInitialized(soa.Self(), klass, false, false);
        if (!klass->IsInitialized()) {
          // We don't want non-trivial class initialization occurring on multiple threads due to
          // deadlock problems. For example, a parent class is initialized (holding its lock) that
          // refers to a sub-class in its static/class initializer causing it to try to acquire the
          // sub-class' lock. While on a second thread the sub-class is initialized (holding its lock)
          // after first initializing its parents, whose locks are acquired. This leads to a
          // parent-to-child and a child-to-parent lock ordering and consequent potential deadlock.
          // We need to use an ObjectLock due to potential suspension in the interpreting code. Rather
          // than use a special Object for the purpose we use the Class of java.lang.Class.
          Handle<mirror::Class> h_klass(hs.NewHandle(klass->GetClass()));
          ObjectLock<mirror::Class> lock(soa.Self(), h_klass);
          // Attempt to initialize allowing initialization of parent classes but still not static
          // fields.
          manager_->GetClassLinker()->EnsureInitialized(soa.Self(), klass, false, true);
          if (!klass->IsInitialized()) {
            // We need to initialize static fields, we only do this for image classes that aren't
            // marked with the $NoPreloadHolder (which implies this should not be initialized early).
            bool can_init_static_fields = manager_->GetCompiler()->IsBootImage() &&
                manager_->GetCompiler()->IsImageClass(descriptor) &&
                !StringPiece(descriptor).ends_with("$NoPreloadHolder;");
            if (can_init_static_fields) {
              VLOG(compiler) << "Initializing: " << descriptor;
              // TODO multithreading support. We should ensure the current compilation thread has
              // exclusive access to the runtime and the transaction. To achieve this, we could use
              // a ReaderWriterMutex but we're holding the mutator lock so we fail mutex sanity
              // checks in Thread::AssertThreadSuspensionIsAllowable.
              Runtime* const runtime = Runtime::Current();
              Transaction transaction;

              // Run the class initializer in transaction mode.
              runtime->EnterTransactionMode(&transaction);
              const mirror::Class::Status old_status = klass->GetStatus();
              bool success = manager_->GetClassLinker()->EnsureInitialized(soa.Self(), klass, true,
                                                                           true);
              // TODO we detach transaction from runtime to indicate we quit the transactional
              // mode which prevents the GC from visiting objects modified during the transaction.
              // Ensure GC is not run so don't access freed objects when aborting transaction.

              ScopedAssertNoThreadSuspension ants(soa.Self(), "Transaction end");
              runtime->ExitTransactionMode();

              if (!success) {
                CHECK(soa.Self()->IsExceptionPending());
                mirror::Throwable* exception = soa.Self()->GetException();
                VLOG(compiler) << "Initialization of " << descriptor << " aborted because of "
                    << exception->Dump();
                std::ostream* file_log = manager_->GetCompiler()->
                    GetCompilerOptions().GetInitFailureOutput();
                if (file_log != nullptr) {
                  *file_log << descriptor << "\n";
                  *file_log << exception->Dump() << "\n";
                }
                soa.Self()->ClearException();
                transaction.Rollback();
                CHECK_EQ(old_status, klass->GetStatus()) << "Previous class status not restored";
              }
            }
          }
          soa.Self()->AssertNoPendingException();
        }
      }
      // Record the final class status if necessary.
      ClassReference ref(manager_->GetDexFile(), class_def_index);
      manager_->GetCompiler()->RecordClassStatus(ref, klass->GetStatus());
    }
    // Clear any class not found or verification exceptions.
    soa.Self()->ClearException();
  }

 private:
  const ParallelCompilationManager* const manager_;
};

void CompilerDriver::InitializeClasses(jobject jni_class_loader,
                                       const DexFile& dex_file,
                                       const std::vector<const DexFile*>& dex_files,
                                       TimingLogger* timings) {
  TimingLogger::ScopedTiming t("InitializeNoClinit", timings);

  // Initialization allocates objects and needs to run single-threaded to be deterministic.
  bool force_determinism = GetCompilerOptions().IsForceDeterminism();
  ThreadPool* init_thread_pool = force_determinism
                                     ? single_thread_pool_.get()
                                     : parallel_thread_pool_.get();
  size_t init_thread_count = force_determinism ? 1U : parallel_thread_count_;

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  ParallelCompilationManager context(class_linker, jni_class_loader, this, &dex_file, dex_files,
                                     init_thread_pool);
  if (IsBootImage()) {
    // TODO: remove this when transactional mode supports multithreading.
    init_thread_count = 1U;
  }
  InitializeClassVisitor visitor(&context);
  context.ForAll(0, dex_file.NumClassDefs(), &visitor, init_thread_count);
}

class InitializeArrayClassesAndCreateConflictTablesVisitor : public ClassVisitor {
 public:
  virtual bool operator()(mirror::Class* klass) OVERRIDE SHARED_REQUIRES(Locks::mutator_lock_) {
    if (Runtime::Current()->GetHeap()->ObjectIsInBootImageSpace(klass)) {
      return true;
    }
    if (klass->IsArrayClass()) {
      StackHandleScope<1> hs(Thread::Current());
      Runtime::Current()->GetClassLinker()->EnsureInitialized(hs.Self(),
                                                              hs.NewHandle(klass),
                                                              true,
                                                              true);
    }
    // Create the conflict tables.
    FillIMTAndConflictTables(klass);
    return true;
  }

 private:
  void FillIMTAndConflictTables(mirror::Class* klass) SHARED_REQUIRES(Locks::mutator_lock_) {
    if (!klass->ShouldHaveImt()) {
      return;
    }
    if (visited_classes_.find(klass) != visited_classes_.end()) {
      return;
    }
    if (klass->HasSuperClass()) {
      FillIMTAndConflictTables(klass->GetSuperClass());
    }
    if (!klass->IsTemp()) {
      Runtime::Current()->GetClassLinker()->FillIMTAndConflictTables(klass);
    }
    visited_classes_.insert(klass);
  }

  std::set<mirror::Class*> visited_classes_;
};

void CompilerDriver::InitializeClasses(jobject class_loader,
                                       const std::vector<const DexFile*>& dex_files,
                                       TimingLogger* timings) {
  for (size_t i = 0; i != dex_files.size(); ++i) {
    const DexFile* dex_file = dex_files[i];
    CHECK(dex_file != nullptr);
    InitializeClasses(class_loader, *dex_file, dex_files, timings);
  }
  if (boot_image_ || app_image_) {
    // Make sure that we call EnsureIntiailized on all the array classes to call
    // SetVerificationAttempted so that the access flags are set. If we do not do this they get
    // changed at runtime resulting in more dirty image pages.
    // Also create conflict tables.
    // Only useful if we are compiling an image (image_classes_ is not null).
    ScopedObjectAccess soa(Thread::Current());
    InitializeArrayClassesAndCreateConflictTablesVisitor visitor;
    Runtime::Current()->GetClassLinker()->VisitClassesWithoutClassesLock(&visitor);
  }
  if (IsBootImage()) {
    // Prune garbage objects created during aborted transactions.
    Runtime::Current()->GetHeap()->CollectGarbage(true);
  }
}

void CompilerDriver::Compile(jobject class_loader,
                             const std::vector<const DexFile*>& dex_files,
                             TimingLogger* timings) {
  if (kDebugProfileGuidedCompilation) {
    LOG(INFO) << "[ProfileGuidedCompilation] " <<
        ((profile_compilation_info_ == nullptr)
            ? "null"
            : profile_compilation_info_->DumpInfo(&dex_files));
  }

  DCHECK(current_dex_to_dex_methods_ == nullptr);
  for (const DexFile* dex_file : dex_files) {
    CHECK(dex_file != nullptr);
    CompileDexFile(class_loader,
                   *dex_file,
                   dex_files,
                   parallel_thread_pool_.get(),
                   parallel_thread_count_,
                   timings);
    const ArenaPool* const arena_pool = Runtime::Current()->GetArenaPool();
    const size_t arena_alloc = arena_pool->GetBytesAllocated();
    max_arena_alloc_ = std::max(arena_alloc, max_arena_alloc_);
    Runtime::Current()->ReclaimArenaPoolMemory();
  }

  ArrayRef<DexFileMethodSet> dex_to_dex_references;
  {
    // From this point on, we shall not modify dex_to_dex_references_, so
    // just grab a reference to it that we use without holding the mutex.
    MutexLock lock(Thread::Current(), dex_to_dex_references_lock_);
    dex_to_dex_references = ArrayRef<DexFileMethodSet>(dex_to_dex_references_);
  }
  for (const auto& method_set : dex_to_dex_references) {
    current_dex_to_dex_methods_ = &method_set.GetMethodIndexes();
    CompileDexFile(class_loader,
                   method_set.GetDexFile(),
                   dex_files,
                   parallel_thread_pool_.get(),
                   parallel_thread_count_,
                   timings);
  }
  current_dex_to_dex_methods_ = nullptr;

  VLOG(compiler) << "Compile: " << GetMemoryUsageString(false);
}

class CompileClassVisitor : public CompilationVisitor {
 public:
  explicit CompileClassVisitor(const ParallelCompilationManager* manager) : manager_(manager) {}

  virtual void Visit(size_t class_def_index) REQUIRES(!Locks::mutator_lock_) OVERRIDE {
    ATRACE_CALL();
    const DexFile& dex_file = *manager_->GetDexFile();
    const DexFile::ClassDef& class_def = dex_file.GetClassDef(class_def_index);
    ClassLinker* class_linker = manager_->GetClassLinker();
    jobject jclass_loader = manager_->GetClassLoader();
    ClassReference ref(&dex_file, class_def_index);
    // Skip compiling classes with generic verifier failures since they will still fail at runtime
    if (manager_->GetCompiler()->verification_results_->IsClassRejected(ref)) {
      return;
    }
    // Use a scoped object access to perform to the quick SkipClass check.
    const char* descriptor = dex_file.GetClassDescriptor(class_def);
    ScopedObjectAccess soa(Thread::Current());
    StackHandleScope<3> hs(soa.Self());
    Handle<mirror::ClassLoader> class_loader(
        hs.NewHandle(soa.Decode<mirror::ClassLoader*>(jclass_loader)));
    Handle<mirror::Class> klass(
        hs.NewHandle(class_linker->FindClass(soa.Self(), descriptor, class_loader)));
    Handle<mirror::DexCache> dex_cache;
    if (klass.Get() == nullptr) {
      soa.Self()->AssertPendingException();
      soa.Self()->ClearException();
      dex_cache = hs.NewHandle(class_linker->FindDexCache(soa.Self(), dex_file));
    } else if (SkipClass(jclass_loader, dex_file, klass.Get())) {
      return;
    } else {
      dex_cache = hs.NewHandle(klass->GetDexCache());
    }

    const uint8_t* class_data = dex_file.GetClassData(class_def);
    if (class_data == nullptr) {
      // empty class, probably a marker interface
      return;
    }

    // Go to native so that we don't block GC during compilation.
    ScopedThreadSuspension sts(soa.Self(), kNative);

    CompilerDriver* const driver = manager_->GetCompiler();

    // Can we run DEX-to-DEX compiler on this class ?
    optimizer::DexToDexCompilationLevel dex_to_dex_compilation_level =
        GetDexToDexCompilationLevel(soa.Self(), *driver, jclass_loader, dex_file, class_def);

    ClassDataItemIterator it(dex_file, class_data);
    // Skip fields
    while (it.HasNextStaticField()) {
      it.Next();
    }
    while (it.HasNextInstanceField()) {
      it.Next();
    }

    bool compilation_enabled = driver->IsClassToCompile(
        dex_file.StringByTypeIdx(class_def.class_idx_));

    // Compile direct methods
    int64_t previous_direct_method_idx = -1;
    while (it.HasNextDirectMethod()) {
      uint32_t method_idx = it.GetMemberIndex();
      if (method_idx == previous_direct_method_idx) {
        // smali can create dex files with two encoded_methods sharing the same method_idx
        // http://code.google.com/p/smali/issues/detail?id=119
        it.Next();
        continue;
      }
      previous_direct_method_idx = method_idx;
      CompileMethod(soa.Self(), driver, it.GetMethodCodeItem(), it.GetMethodAccessFlags(),
                    it.GetMethodInvokeType(class_def), class_def_index,
                    method_idx, jclass_loader, dex_file, dex_to_dex_compilation_level,
                    compilation_enabled, dex_cache);
      it.Next();
    }
    // Compile virtual methods
    int64_t previous_virtual_method_idx = -1;
    while (it.HasNextVirtualMethod()) {
      uint32_t method_idx = it.GetMemberIndex();
      if (method_idx == previous_virtual_method_idx) {
        // smali can create dex files with two encoded_methods sharing the same method_idx
        // http://code.google.com/p/smali/issues/detail?id=119
        it.Next();
        continue;
      }
      previous_virtual_method_idx = method_idx;
      CompileMethod(soa.Self(), driver, it.GetMethodCodeItem(), it.GetMethodAccessFlags(),
                    it.GetMethodInvokeType(class_def), class_def_index,
                    method_idx, jclass_loader, dex_file, dex_to_dex_compilation_level,
                    compilation_enabled, dex_cache);
      it.Next();
    }
    DCHECK(!it.HasNext());
  }

 private:
  const ParallelCompilationManager* const manager_;
};

void CompilerDriver::CompileDexFile(jobject class_loader,
                                    const DexFile& dex_file,
                                    const std::vector<const DexFile*>& dex_files,
                                    ThreadPool* thread_pool,
                                    size_t thread_count,
                                    TimingLogger* timings) {
  TimingLogger::ScopedTiming t("Compile Dex File", timings);
  ParallelCompilationManager context(Runtime::Current()->GetClassLinker(), class_loader, this,
                                     &dex_file, dex_files, thread_pool);
  CompileClassVisitor visitor(&context);
  context.ForAll(0, dex_file.NumClassDefs(), &visitor, thread_count);
}

void CompilerDriver::AddCompiledMethod(const MethodReference& method_ref,
                                       CompiledMethod* const compiled_method,
                                       size_t non_relative_linker_patch_count) {
  DCHECK(GetCompiledMethod(method_ref) == nullptr)
      << PrettyMethod(method_ref.dex_method_index, *method_ref.dex_file);
  {
    MutexLock mu(Thread::Current(), compiled_methods_lock_);
    compiled_methods_.Put(method_ref, compiled_method);
    non_relative_linker_patch_count_ += non_relative_linker_patch_count;
  }
  DCHECK(GetCompiledMethod(method_ref) != nullptr)
      << PrettyMethod(method_ref.dex_method_index, *method_ref.dex_file);
}

void CompilerDriver::RemoveCompiledMethod(const MethodReference& method_ref) {
  CompiledMethod* compiled_method = nullptr;
  {
    MutexLock mu(Thread::Current(), compiled_methods_lock_);
    auto it = compiled_methods_.find(method_ref);
    if (it != compiled_methods_.end()) {
      compiled_method = it->second;
      compiled_methods_.erase(it);
    }
  }
  if (compiled_method != nullptr) {
    CompiledMethod::ReleaseSwapAllocatedCompiledMethod(this, compiled_method);
  }
}

CompiledClass* CompilerDriver::GetCompiledClass(ClassReference ref) const {
  MutexLock mu(Thread::Current(), compiled_classes_lock_);
  ClassTable::const_iterator it = compiled_classes_.find(ref);
  if (it == compiled_classes_.end()) {
    return nullptr;
  }
  CHECK(it->second != nullptr);
  return it->second;
}

void CompilerDriver::RecordClassStatus(ClassReference ref, mirror::Class::Status status) {
  MutexLock mu(Thread::Current(), compiled_classes_lock_);
  auto it = compiled_classes_.find(ref);
  if (it == compiled_classes_.end() || it->second->GetStatus() != status) {
    // An entry doesn't exist or the status is lower than the new status.
    if (it != compiled_classes_.end()) {
      CHECK_GT(status, it->second->GetStatus());
      delete it->second;
    }
    switch (status) {
      case mirror::Class::kStatusNotReady:
      case mirror::Class::kStatusError:
      case mirror::Class::kStatusRetryVerificationAtRuntime:
      case mirror::Class::kStatusVerified:
      case mirror::Class::kStatusInitialized:
      case mirror::Class::kStatusResolved:
        break;  // Expected states.
      default:
        LOG(FATAL) << "Unexpected class status for class "
            << PrettyDescriptor(ref.first->GetClassDescriptor(ref.first->GetClassDef(ref.second)))
            << " of " << status;
    }
    CompiledClass* compiled_class = new CompiledClass(status);
    compiled_classes_.Overwrite(ref, compiled_class);
  }
}

CompiledMethod* CompilerDriver::GetCompiledMethod(MethodReference ref) const {
  MutexLock mu(Thread::Current(), compiled_methods_lock_);
  MethodTable::const_iterator it = compiled_methods_.find(ref);
  if (it == compiled_methods_.end()) {
    return nullptr;
  }
  CHECK(it->second != nullptr);
  return it->second;
}

bool CompilerDriver::IsMethodVerifiedWithoutFailures(uint32_t method_idx,
                                                     uint16_t class_def_idx,
                                                     const DexFile& dex_file) const {
  const VerifiedMethod* verified_method = GetVerifiedMethod(&dex_file, method_idx);
  if (verified_method != nullptr) {
    return !verified_method->HasVerificationFailures();
  }

  // If we can't find verification metadata, check if this is a system class (we trust that system
  // classes have their methods verified). If it's not, be conservative and assume the method
  // has not been verified successfully.

  // TODO: When compiling the boot image it should be safe to assume that everything is verified,
  // even if methods are not found in the verification cache.
  const char* descriptor = dex_file.GetClassDescriptor(dex_file.GetClassDef(class_def_idx));
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);
  bool is_system_class = class_linker->FindSystemClass(self, descriptor) != nullptr;
  if (!is_system_class) {
    self->ClearException();
  }
  return is_system_class;
}

size_t CompilerDriver::GetNonRelativeLinkerPatchCount() const {
  MutexLock mu(Thread::Current(), compiled_methods_lock_);
  return non_relative_linker_patch_count_;
}

void CompilerDriver::SetRequiresConstructorBarrier(Thread* self,
                                                   const DexFile* dex_file,
                                                   uint16_t class_def_index,
                                                   bool requires) {
  WriterMutexLock mu(self, requires_constructor_barrier_lock_);
  requires_constructor_barrier_.emplace(ClassReference(dex_file, class_def_index), requires);
}

bool CompilerDriver::RequiresConstructorBarrier(Thread* self,
                                                const DexFile* dex_file,
                                                uint16_t class_def_index) {
  ClassReference class_ref(dex_file, class_def_index);
  {
    ReaderMutexLock mu(self, requires_constructor_barrier_lock_);
    auto it = requires_constructor_barrier_.find(class_ref);
    if (it != requires_constructor_barrier_.end()) {
      return it->second;
    }
  }
  WriterMutexLock mu(self, requires_constructor_barrier_lock_);
  const bool requires = RequiresConstructorBarrier(*dex_file, class_def_index);
  requires_constructor_barrier_.emplace(class_ref, requires);
  return requires;
}

std::string CompilerDriver::GetMemoryUsageString(bool extended) const {
  std::ostringstream oss;
  const gc::Heap* const heap = Runtime::Current()->GetHeap();
  const size_t java_alloc = heap->GetBytesAllocated();
  oss << "arena alloc=" << PrettySize(max_arena_alloc_) << " (" << max_arena_alloc_ << "B)";
  oss << " java alloc=" << PrettySize(java_alloc) << " (" << java_alloc << "B)";
#if defined(__BIONIC__) || defined(__GLIBC__)
  const struct mallinfo info = mallinfo();
  const size_t allocated_space = static_cast<size_t>(info.uordblks);
  const size_t free_space = static_cast<size_t>(info.fordblks);
  oss << " native alloc=" << PrettySize(allocated_space) << " (" << allocated_space << "B)"
      << " free=" << PrettySize(free_space) << " (" << free_space << "B)";
#endif
  compiled_method_storage_.DumpMemoryUsage(oss, extended);
  return oss.str();
}

bool CompilerDriver::IsStringTypeIndex(uint16_t type_index, const DexFile* dex_file) {
  const char* type = dex_file->GetTypeDescriptor(dex_file->GetTypeId(type_index));
  return strcmp(type, "Ljava/lang/String;") == 0;
}

bool CompilerDriver::IsStringInit(uint32_t method_index, const DexFile* dex_file, int32_t* offset) {
  DexFileMethodInliner* inliner = GetMethodInlinerMap()->GetMethodInliner(dex_file);
  size_t pointer_size = InstructionSetPointerSize(GetInstructionSet());
  *offset = inliner->GetOffsetForStringInit(method_index, pointer_size);
  return inliner->IsStringInitMethodIndex(method_index);
}

bool CompilerDriver::MayInlineInternal(const DexFile* inlined_from,
                                       const DexFile* inlined_into) const {
  // We're not allowed to inline across dex files if we're the no-inline-from dex file.
  if (inlined_from != inlined_into &&
      compiler_options_->GetNoInlineFromDexFile() != nullptr &&
      ContainsElement(*compiler_options_->GetNoInlineFromDexFile(), inlined_from)) {
    return false;
  }

  return true;
}

void CompilerDriver::InitializeThreadPools() {
  size_t parallel_count = parallel_thread_count_ > 0 ? parallel_thread_count_ - 1 : 0;
  parallel_thread_pool_.reset(
      new ThreadPool("Compiler driver thread pool", parallel_count));
  single_thread_pool_.reset(new ThreadPool("Single-threaded Compiler driver thread pool", 0));
}

void CompilerDriver::FreeThreadPools() {
  parallel_thread_pool_.reset();
  single_thread_pool_.reset();
}

}  // namespace art
