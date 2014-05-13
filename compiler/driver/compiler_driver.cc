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

#define ATRACE_TAG ATRACE_TAG_DALVIK
#include <utils/Trace.h>

#include <vector>
#include <unistd.h>

#include "base/stl_util.h"
#include "base/timing_logger.h"
#include "class_linker.h"
#include "compiler.h"
#include "compiler_driver-inl.h"
#include "dex_compilation_unit.h"
#include "dex_file-inl.h"
#include "dex/verification_results.h"
#include "dex/verified_method.h"
#include "dex/quick/dex_file_method_inliner.h"
#include "driver/compiler_options.h"
#include "jni_internal.h"
#include "object_utils.h"
#include "runtime.h"
#include "gc/accounting/card_table-inl.h"
#include "gc/accounting/heap_bitmap.h"
#include "gc/space/space.h"
#include "mirror/art_field-inl.h"
#include "mirror/art_method-inl.h"
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
#include "thread_pool.h"
#include "trampolines/trampoline_compiler.h"
#include "transaction.h"
#include "verifier/method_verifier.h"
#include "verifier/method_verifier-inl.h"

#ifdef HAVE_ANDROID_OS
#include "cutils/properties.h"
#endif

namespace art {

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

  void TypeInDexCache() {
    STATS_LOCK();
    types_in_dex_cache_++;
  }

  void TypeNotInDexCache() {
    STATS_LOCK();
    types_not_in_dex_cache_++;
  }

  void StringInDexCache() {
    STATS_LOCK();
    strings_in_dex_cache_++;
  }

  void StringNotInDexCache() {
    STATS_LOCK();
    strings_not_in_dex_cache_++;
  }

  void TypeDoesntNeedAccessCheck() {
    STATS_LOCK();
    resolved_types_++;
  }

  void TypeNeedsAccessCheck() {
    STATS_LOCK();
    unresolved_types_++;
  }

  void ResolvedInstanceField() {
    STATS_LOCK();
    resolved_instance_fields_++;
  }

  void UnresolvedInstanceField() {
    STATS_LOCK();
    unresolved_instance_fields_++;
  }

  void ResolvedLocalStaticField() {
    STATS_LOCK();
    resolved_local_static_fields_++;
  }

  void ResolvedStaticField() {
    STATS_LOCK();
    resolved_static_fields_++;
  }

  void UnresolvedStaticField() {
    STATS_LOCK();
    unresolved_static_fields_++;
  }

  // Indicate that type information from the verifier led to devirtualization.
  void PreciseTypeDevirtualization() {
    STATS_LOCK();
    type_based_devirtualization_++;
  }

  // Indicate that a method of the given type was resolved at compile time.
  void ResolvedMethod(InvokeType type) {
    DCHECK_LE(type, kMaxInvokeType);
    STATS_LOCK();
    resolved_methods_[type]++;
  }

  // Indicate that a method of the given type was unresolved at compile time as it was in an
  // unknown dex file.
  void UnresolvedMethod(InvokeType type) {
    DCHECK_LE(type, kMaxInvokeType);
    STATS_LOCK();
    unresolved_methods_[type]++;
  }

  // Indicate that a type of virtual method dispatch has been converted into a direct method
  // dispatch.
  void VirtualMadeDirect(InvokeType type) {
    DCHECK(type == kVirtual || type == kInterface || type == kSuper);
    STATS_LOCK();
    virtual_made_direct_[type]++;
  }

  // Indicate that a method of the given type was able to call directly into boot.
  void DirectCallsToBoot(InvokeType type) {
    DCHECK_LE(type, kMaxInvokeType);
    STATS_LOCK();
    direct_calls_to_boot_[type]++;
  }

  // Indicate that a method of the given type was able to be resolved directly from boot.
  void DirectMethodsToBoot(InvokeType type) {
    DCHECK_LE(type, kMaxInvokeType);
    STATS_LOCK();
    direct_methods_to_boot_[type]++;
  }

  void ProcessedInvoke(InvokeType type, int flags) {
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
  void SafeCast() {
    STATS_LOCK();
    safe_casts_++;
  }

  // A check-cast couldn't be eliminated due to verifier type analysis.
  void NotASafeCast() {
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


extern "C" art::CompiledMethod* ArtCompileDEX(art::CompilerDriver& compiler,
                                              const art::DexFile::CodeItem* code_item,
                                              uint32_t access_flags,
                                              art::InvokeType invoke_type,
                                              uint16_t class_def_idx,
                                              uint32_t method_idx,
                                              jobject class_loader,
                                              const art::DexFile& dex_file);

CompilerDriver::CompilerDriver(const CompilerOptions* compiler_options,
                               VerificationResults* verification_results,
                               DexFileToMethodInlinerMap* method_inliner_map,
                               Compiler::Kind compiler_kind,
                               InstructionSet instruction_set,
                               InstructionSetFeatures instruction_set_features,
                               bool image, DescriptorSet* image_classes, size_t thread_count,
                               bool dump_stats, bool dump_passes, CumulativeLogger* timer,
                               std::string profile_file)
    : profile_ok_(false), compiler_options_(compiler_options),
      verification_results_(verification_results),
      method_inliner_map_(method_inliner_map),
      compiler_(Compiler::Create(this, compiler_kind)),
      instruction_set_(instruction_set),
      instruction_set_features_(instruction_set_features),
      freezing_constructor_lock_("freezing constructor lock"),
      compiled_classes_lock_("compiled classes lock"),
      compiled_methods_lock_("compiled method lock"),
      image_(image),
      image_classes_(image_classes),
      thread_count_(thread_count),
      start_ns_(0),
      stats_(new AOTCompilationStats),
      dump_stats_(dump_stats),
      dump_passes_(dump_passes),
      timings_logger_(timer),
      compiler_library_(NULL),
      compiler_context_(NULL),
      compiler_enable_auto_elf_loading_(NULL),
      compiler_get_method_code_addr_(NULL),
      support_boot_image_fixup_(instruction_set != kMips),
      cfi_info_(nullptr),
      dedupe_code_("dedupe code"),
      dedupe_mapping_table_("dedupe mapping table"),
      dedupe_vmap_table_("dedupe vmap table"),
      dedupe_gc_map_("dedupe gc map"),
      dedupe_cfi_info_("dedupe cfi info") {
  DCHECK(compiler_options_ != nullptr);
  DCHECK(verification_results_ != nullptr);
  DCHECK(method_inliner_map_ != nullptr);

  CHECK_PTHREAD_CALL(pthread_key_create, (&tls_key_, NULL), "compiler tls key");

  // Read the profile file if one is provided.
  if (profile_file != "") {
    profile_ok_ = ProfileHelper::LoadProfileMap(profile_map_, profile_file);
  }

  dex_to_dex_compiler_ = reinterpret_cast<DexToDexCompilerFn>(ArtCompileDEX);

  compiler_->Init();

  CHECK(!Runtime::Current()->IsStarted());
  if (image_) {
    CHECK(image_classes_.get() != nullptr);
  } else {
    CHECK(image_classes_.get() == nullptr);
  }

  // Are we generating CFI information?
  if (compiler_options->GetGenerateGDBInformation()) {
    cfi_info_.reset(compiler_->GetCallFrameInformationInitialization(*this));
  }
}

std::vector<uint8_t>* CompilerDriver::DeduplicateCode(const std::vector<uint8_t>& code) {
  return dedupe_code_.Add(Thread::Current(), code);
}

std::vector<uint8_t>* CompilerDriver::DeduplicateMappingTable(const std::vector<uint8_t>& code) {
  return dedupe_mapping_table_.Add(Thread::Current(), code);
}

std::vector<uint8_t>* CompilerDriver::DeduplicateVMapTable(const std::vector<uint8_t>& code) {
  return dedupe_vmap_table_.Add(Thread::Current(), code);
}

std::vector<uint8_t>* CompilerDriver::DeduplicateGCMap(const std::vector<uint8_t>& code) {
  return dedupe_gc_map_.Add(Thread::Current(), code);
}

std::vector<uint8_t>* CompilerDriver::DeduplicateCFIInfo(const std::vector<uint8_t>* cfi_info) {
  if (cfi_info == nullptr) {
    return nullptr;
  }
  return dedupe_cfi_info_.Add(Thread::Current(), *cfi_info);
}

CompilerDriver::~CompilerDriver() {
  Thread* self = Thread::Current();
  {
    MutexLock mu(self, compiled_classes_lock_);
    STLDeleteValues(&compiled_classes_);
  }
  {
    MutexLock mu(self, compiled_methods_lock_);
    STLDeleteValues(&compiled_methods_);
  }
  {
    MutexLock mu(self, compiled_methods_lock_);
    STLDeleteElements(&code_to_patch_);
  }
  {
    MutexLock mu(self, compiled_methods_lock_);
    STLDeleteElements(&methods_to_patch_);
  }
  {
    MutexLock mu(self, compiled_methods_lock_);
    STLDeleteElements(&classes_to_patch_);
  }
  CHECK_PTHREAD_CALL(pthread_key_delete, (tls_key_), "delete tls key");
  compiler_->UnInit();
}

CompilerTls* CompilerDriver::GetTls() {
  // Lazily create thread-local storage
  CompilerTls* res = static_cast<CompilerTls*>(pthread_getspecific(tls_key_));
  if (res == NULL) {
    res = new CompilerTls();
    CHECK_PTHREAD_CALL(pthread_setspecific, (tls_key_, res), "compiler tls");
  }
  return res;
}

#define CREATE_TRAMPOLINE(type, abi, offset) \
    if (Is64BitInstructionSet(instruction_set_)) { \
      return CreateTrampoline64(instruction_set_, abi, \
                                type ## _ENTRYPOINT_OFFSET(8, offset)); \
    } else { \
      return CreateTrampoline32(instruction_set_, abi, \
                                type ## _ENTRYPOINT_OFFSET(4, offset)); \
    }

const std::vector<uint8_t>* CompilerDriver::CreateInterpreterToInterpreterBridge() const {
  CREATE_TRAMPOLINE(INTERPRETER, kInterpreterAbi, pInterpreterToInterpreterBridge)
}

const std::vector<uint8_t>* CompilerDriver::CreateInterpreterToCompiledCodeBridge() const {
  CREATE_TRAMPOLINE(INTERPRETER, kInterpreterAbi, pInterpreterToCompiledCodeBridge)
}

const std::vector<uint8_t>* CompilerDriver::CreateJniDlsymLookup() const {
  CREATE_TRAMPOLINE(JNI, kJniAbi, pDlsymLookup)
}

const std::vector<uint8_t>* CompilerDriver::CreatePortableImtConflictTrampoline() const {
  CREATE_TRAMPOLINE(PORTABLE, kPortableAbi, pPortableImtConflictTrampoline)
}

const std::vector<uint8_t>* CompilerDriver::CreatePortableResolutionTrampoline() const {
  CREATE_TRAMPOLINE(PORTABLE, kPortableAbi, pPortableResolutionTrampoline)
}

const std::vector<uint8_t>* CompilerDriver::CreatePortableToInterpreterBridge() const {
  CREATE_TRAMPOLINE(PORTABLE, kPortableAbi, pPortableToInterpreterBridge)
}

const std::vector<uint8_t>* CompilerDriver::CreateQuickGenericJniTrampoline() const {
  CREATE_TRAMPOLINE(QUICK, kQuickAbi, pQuickGenericJniTrampoline)
}

const std::vector<uint8_t>* CompilerDriver::CreateQuickImtConflictTrampoline() const {
  CREATE_TRAMPOLINE(QUICK, kQuickAbi, pQuickImtConflictTrampoline)
}

const std::vector<uint8_t>* CompilerDriver::CreateQuickResolutionTrampoline() const {
  CREATE_TRAMPOLINE(QUICK, kQuickAbi, pQuickResolutionTrampoline)
}

const std::vector<uint8_t>* CompilerDriver::CreateQuickToInterpreterBridge() const {
  CREATE_TRAMPOLINE(QUICK, kQuickAbi, pQuickToInterpreterBridge)
}
#undef CREATE_TRAMPOLINE

void CompilerDriver::CompileAll(jobject class_loader,
                                const std::vector<const DexFile*>& dex_files,
                                TimingLogger* timings) {
  DCHECK(!Runtime::Current()->IsStarted());
  UniquePtr<ThreadPool> thread_pool(new ThreadPool("Compiler driver thread pool", thread_count_ - 1));
  PreCompile(class_loader, dex_files, thread_pool.get(), timings);
  Compile(class_loader, dex_files, thread_pool.get(), timings);
  if (dump_stats_) {
    stats_->Dump();
  }
}

static DexToDexCompilationLevel GetDexToDexCompilationlevel(
    Thread* self, Handle<mirror::ClassLoader>& class_loader, const DexFile& dex_file,
    const DexFile::ClassDef& class_def) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  const char* descriptor = dex_file.GetClassDescriptor(class_def);
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  mirror::Class* klass = class_linker->FindClass(self, descriptor, class_loader);
  if (klass == NULL) {
    CHECK(self->IsExceptionPending());
    self->ClearException();
    return kDontDexToDexCompile;
  }
  // The verifier can only run on "quick" instructions at runtime (see usage of
  // FindAccessedFieldAtDexPc and FindInvokedMethodAtDexPc in ThrowNullPointerExceptionFromDexPC
  // function). Since image classes can be verified again while compiling an application,
  // we must prevent the DEX-to-DEX compiler from introducing them.
  // TODO: find a way to enable "quick" instructions for image classes and remove this check.
  bool compiling_image_classes = class_loader.Get() == nullptr;
  if (compiling_image_classes) {
    return kRequired;
  } else if (klass->IsVerified()) {
    // Class is verified so we can enable DEX-to-DEX compilation for performance.
    return kOptimize;
  } else if (klass->IsCompileTimeVerified()) {
    // Class verification has soft-failed. Anyway, ensure at least correctness.
    DCHECK_EQ(klass->GetStatus(), mirror::Class::kStatusRetryVerificationAtRuntime);
    return kRequired;
  } else {
    // Class verification has failed: do not run DEX-to-DEX compilation.
    return kDontDexToDexCompile;
  }
}

void CompilerDriver::CompileOne(mirror::ArtMethod* method, TimingLogger* timings) {
  DCHECK(!Runtime::Current()->IsStarted());
  Thread* self = Thread::Current();
  jobject jclass_loader;
  const DexFile* dex_file;
  uint16_t class_def_idx;
  uint32_t method_idx = method->GetDexMethodIndex();
  uint32_t access_flags = method->GetAccessFlags();
  InvokeType invoke_type = method->GetInvokeType();
  {
    ScopedObjectAccessUnchecked soa(self);
    ScopedLocalRef<jobject>
      local_class_loader(soa.Env(),
                    soa.AddLocalReference<jobject>(method->GetDeclaringClass()->GetClassLoader()));
    jclass_loader = soa.Env()->NewGlobalRef(local_class_loader.get());
    // Find the dex_file
    MethodHelper mh(method);
    dex_file = &mh.GetDexFile();
    class_def_idx = mh.GetClassDefIndex();
  }
  const DexFile::CodeItem* code_item = dex_file->GetCodeItem(method->GetCodeItemOffset());
  self->TransitionFromRunnableToSuspended(kNative);

  std::vector<const DexFile*> dex_files;
  dex_files.push_back(dex_file);

  UniquePtr<ThreadPool> thread_pool(new ThreadPool("Compiler driver thread pool", 0U));
  PreCompile(jclass_loader, dex_files, thread_pool.get(), timings);

  // Can we run DEX-to-DEX compiler on this class ?
  DexToDexCompilationLevel dex_to_dex_compilation_level = kDontDexToDexCompile;
  {
    ScopedObjectAccess soa(Thread::Current());
    const DexFile::ClassDef& class_def = dex_file->GetClassDef(class_def_idx);
    StackHandleScope<1> hs(soa.Self());
    Handle<mirror::ClassLoader> class_loader(
        hs.NewHandle(soa.Decode<mirror::ClassLoader*>(jclass_loader)));
    dex_to_dex_compilation_level = GetDexToDexCompilationlevel(self, class_loader, *dex_file,
                                                               class_def);
  }
  CompileMethod(code_item, access_flags, invoke_type, class_def_idx, method_idx, jclass_loader,
                *dex_file, dex_to_dex_compilation_level);

  self->GetJniEnv()->DeleteGlobalRef(jclass_loader);

  self->TransitionFromSuspendedToRunnable();
}

void CompilerDriver::Resolve(jobject class_loader, const std::vector<const DexFile*>& dex_files,
                             ThreadPool* thread_pool, TimingLogger* timings) {
  for (size_t i = 0; i != dex_files.size(); ++i) {
    const DexFile* dex_file = dex_files[i];
    CHECK(dex_file != nullptr);
    ResolveDexFile(class_loader, *dex_file, thread_pool, timings);
  }
}

void CompilerDriver::PreCompile(jobject class_loader, const std::vector<const DexFile*>& dex_files,
                                ThreadPool* thread_pool, TimingLogger* timings) {
  LoadImageClasses(timings);

  if (!compiler_options_->IsVerificationEnabled()) {
    VLOG(compiler) << "Verify none mode specified, skipping pre-compilation";
    return;
  }

  Resolve(class_loader, dex_files, thread_pool, timings);

  Verify(class_loader, dex_files, thread_pool, timings);

  InitializeClasses(class_loader, dex_files, thread_pool, timings);

  UpdateImageClasses(timings);
}

bool CompilerDriver::IsImageClass(const char* descriptor) const {
  if (!IsImage()) {
    return true;
  } else {
    return image_classes_->find(descriptor) != image_classes_->end();
  }
}

static void ResolveExceptionsForMethod(MethodHelper* mh,
    std::set<std::pair<uint16_t, const DexFile*> >& exceptions_to_resolve)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  const DexFile::CodeItem* code_item = mh->GetCodeItem();
  if (code_item == NULL) {
    return;  // native or abstract method
  }
  if (code_item->tries_size_ == 0) {
    return;  // nothing to process
  }
  const byte* encoded_catch_handler_list = DexFile::GetCatchHandlerData(*code_item, 0);
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
      if (!mh->IsResolvedTypeIdx(encoded_catch_handler_handlers_type_idx)) {
        exceptions_to_resolve.insert(
            std::pair<uint16_t, const DexFile*>(encoded_catch_handler_handlers_type_idx,
                                                &mh->GetDexFile()));
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

static bool ResolveCatchBlockExceptionsClassVisitor(mirror::Class* c, void* arg)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  std::set<std::pair<uint16_t, const DexFile*> >* exceptions_to_resolve =
      reinterpret_cast<std::set<std::pair<uint16_t, const DexFile*> >*>(arg);
  MethodHelper mh;
  for (size_t i = 0; i < c->NumVirtualMethods(); ++i) {
    mirror::ArtMethod* m = c->GetVirtualMethod(i);
    mh.ChangeMethod(m);
    ResolveExceptionsForMethod(&mh, *exceptions_to_resolve);
  }
  for (size_t i = 0; i < c->NumDirectMethods(); ++i) {
    mirror::ArtMethod* m = c->GetDirectMethod(i);
    mh.ChangeMethod(m);
    ResolveExceptionsForMethod(&mh, *exceptions_to_resolve);
  }
  return true;
}

static bool RecordImageClassesVisitor(mirror::Class* klass, void* arg)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  CompilerDriver::DescriptorSet* image_classes =
      reinterpret_cast<CompilerDriver::DescriptorSet*>(arg);
  image_classes->insert(ClassHelper(klass).GetDescriptor());
  return true;
}

// Make a list of descriptors for classes to include in the image
void CompilerDriver::LoadImageClasses(TimingLogger* timings)
      LOCKS_EXCLUDED(Locks::mutator_lock_) {
  CHECK(timings != nullptr);
  if (!IsImage()) {
    return;
  }

  timings->NewSplit("LoadImageClasses");
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
    if (klass.Get() == NULL) {
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
  std::set<std::pair<uint16_t, const DexFile*> > unresolved_exception_types;
  StackHandleScope<1> hs(self);
  Handle<mirror::Class> java_lang_Throwable(
      hs.NewHandle(class_linker->FindSystemClass(self, "Ljava/lang/Throwable;")));
  do {
    unresolved_exception_types.clear();
    class_linker->VisitClasses(ResolveCatchBlockExceptionsClassVisitor,
                               &unresolved_exception_types);
    for (const std::pair<uint16_t, const DexFile*>& exception_type : unresolved_exception_types) {
      uint16_t exception_type_idx = exception_type.first;
      const DexFile* dex_file = exception_type.second;
      StackHandleScope<3> hs(self);
      Handle<mirror::DexCache> dex_cache(hs.NewHandle(class_linker->FindDexCache(*dex_file)));
      auto class_loader(hs.NewHandle<mirror::ClassLoader>(nullptr));
      Handle<mirror::Class> klass(hs.NewHandle(
          class_linker->ResolveType(*dex_file, exception_type_idx, dex_cache, class_loader)));
      if (klass.Get() == NULL) {
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
  class_linker->VisitClasses(RecordImageClassesVisitor, image_classes_.get());

  CHECK_NE(image_classes_->size(), 0U);
}

static void MaybeAddToImageClasses(mirror::Class* klass, CompilerDriver::DescriptorSet* image_classes)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  while (!klass->IsObjectClass()) {
    ClassHelper kh(klass);
    const char* descriptor = kh.GetDescriptor();
    std::pair<CompilerDriver::DescriptorSet::iterator, bool> result =
        image_classes->insert(descriptor);
    if (result.second) {
        VLOG(compiler) << "Adding " << descriptor << " to image classes";
    } else {
      return;
    }
    for (size_t i = 0; i < kh.NumDirectInterfaces(); ++i) {
      MaybeAddToImageClasses(kh.GetDirectInterface(i), image_classes);
    }
    if (klass->IsArrayClass()) {
      MaybeAddToImageClasses(klass->GetComponentType(), image_classes);
    }
    klass = klass->GetSuperClass();
  }
}

void CompilerDriver::FindClinitImageClassesCallback(mirror::Object* object, void* arg) {
  DCHECK(object != NULL);
  DCHECK(arg != NULL);
  CompilerDriver* compiler_driver = reinterpret_cast<CompilerDriver*>(arg);
  MaybeAddToImageClasses(object->GetClass(), compiler_driver->image_classes_.get());
}

void CompilerDriver::UpdateImageClasses(TimingLogger* timings) {
  if (IsImage()) {
    timings->NewSplit("UpdateImageClasses");

    // Update image_classes_ with classes for objects created by <clinit> methods.
    Thread* self = Thread::Current();
    const char* old_cause = self->StartAssertNoThreadSuspension("ImageWriter");
    gc::Heap* heap = Runtime::Current()->GetHeap();
    // TODO: Image spaces only?
    ScopedObjectAccess soa(Thread::Current());
    WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
    heap->VisitObjects(FindClinitImageClassesCallback, this);
    self->EndAssertNoThreadSuspension(old_cause);
  }
}

bool CompilerDriver::CanAssumeTypeIsPresentInDexCache(const DexFile& dex_file, uint32_t type_idx) {
  if (IsImage() &&
      IsImageClass(dex_file.StringDataByIdx(dex_file.GetTypeId(type_idx).descriptor_idx_))) {
    if (kIsDebugBuild) {
      ScopedObjectAccess soa(Thread::Current());
      mirror::DexCache* dex_cache = Runtime::Current()->GetClassLinker()->FindDexCache(dex_file);
      mirror::Class* resolved_class = dex_cache->GetResolvedType(type_idx);
      CHECK(resolved_class != NULL);
    }
    stats_->TypeInDexCache();
    return true;
  } else {
    stats_->TypeNotInDexCache();
    return false;
  }
}

bool CompilerDriver::CanAssumeStringIsPresentInDexCache(const DexFile& dex_file,
                                                        uint32_t string_idx) {
  // See also Compiler::ResolveDexFile

  bool result = false;
  if (IsImage()) {
    // We resolve all const-string strings when building for the image.
    ScopedObjectAccess soa(Thread::Current());
    StackHandleScope<1> hs(soa.Self());
    Handle<mirror::DexCache> dex_cache(
        hs.NewHandle(Runtime::Current()->GetClassLinker()->FindDexCache(dex_file)));
    Runtime::Current()->GetClassLinker()->ResolveString(dex_file, string_idx, dex_cache);
    result = true;
  }
  if (result) {
    stats_->StringInDexCache();
  } else {
    stats_->StringNotInDexCache();
  }
  return result;
}

bool CompilerDriver::CanAccessTypeWithoutChecks(uint32_t referrer_idx, const DexFile& dex_file,
                                                uint32_t type_idx,
                                                bool* type_known_final, bool* type_known_abstract,
                                                bool* equals_referrers_class) {
  if (type_known_final != NULL) {
    *type_known_final = false;
  }
  if (type_known_abstract != NULL) {
    *type_known_abstract = false;
  }
  if (equals_referrers_class != NULL) {
    *equals_referrers_class = false;
  }
  ScopedObjectAccess soa(Thread::Current());
  mirror::DexCache* dex_cache = Runtime::Current()->GetClassLinker()->FindDexCache(dex_file);
  // Get type from dex cache assuming it was populated by the verifier
  mirror::Class* resolved_class = dex_cache->GetResolvedType(type_idx);
  if (resolved_class == NULL) {
    stats_->TypeNeedsAccessCheck();
    return false;  // Unknown class needs access checks.
  }
  const DexFile::MethodId& method_id = dex_file.GetMethodId(referrer_idx);
  if (equals_referrers_class != NULL) {
    *equals_referrers_class = (method_id.class_idx_ == type_idx);
  }
  mirror::Class* referrer_class = dex_cache->GetResolvedType(method_id.class_idx_);
  if (referrer_class == NULL) {
    stats_->TypeNeedsAccessCheck();
    return false;  // Incomplete referrer knowledge needs access check.
  }
  // Perform access check, will return true if access is ok or false if we're going to have to
  // check this at runtime (for example for class loaders).
  bool result = referrer_class->CanAccess(resolved_class);
  if (result) {
    stats_->TypeDoesntNeedAccessCheck();
    if (type_known_final != NULL) {
      *type_known_final = resolved_class->IsFinal() && !resolved_class->IsArrayClass();
    }
    if (type_known_abstract != NULL) {
      *type_known_abstract = resolved_class->IsAbstract() && !resolved_class->IsArrayClass();
    }
  } else {
    stats_->TypeNeedsAccessCheck();
  }
  return result;
}

bool CompilerDriver::CanAccessInstantiableTypeWithoutChecks(uint32_t referrer_idx,
                                                            const DexFile& dex_file,
                                                            uint32_t type_idx) {
  ScopedObjectAccess soa(Thread::Current());
  mirror::DexCache* dex_cache = Runtime::Current()->GetClassLinker()->FindDexCache(dex_file);
  // Get type from dex cache assuming it was populated by the verifier.
  mirror::Class* resolved_class = dex_cache->GetResolvedType(type_idx);
  if (resolved_class == NULL) {
    stats_->TypeNeedsAccessCheck();
    return false;  // Unknown class needs access checks.
  }
  const DexFile::MethodId& method_id = dex_file.GetMethodId(referrer_idx);
  mirror::Class* referrer_class = dex_cache->GetResolvedType(method_id.class_idx_);
  if (referrer_class == NULL) {
    stats_->TypeNeedsAccessCheck();
    return false;  // Incomplete referrer knowledge needs access check.
  }
  // Perform access and instantiable checks, will return true if access is ok or false if we're
  // going to have to check this at runtime (for example for class loaders).
  bool result = referrer_class->CanAccess(resolved_class) && resolved_class->IsInstantiable();
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
  mirror::DexCache* dex_cache = Runtime::Current()->GetClassLinker()->FindDexCache(dex_file);
  mirror::Class* resolved_class = dex_cache->GetResolvedType(type_idx);
  if (resolved_class == nullptr) {
    return false;
  }
  *out_is_finalizable = resolved_class->IsFinalizable();
  const bool compiling_boot = Runtime::Current()->GetHeap()->IsCompilingBoot();
  if (compiling_boot) {
    // boot -> boot class pointers.
    // True if the class is in the image at boot compiling time.
    const bool is_image_class = IsImage() && IsImageClass(
        dex_file.StringDataByIdx(dex_file.GetTypeId(type_idx).descriptor_idx_));
    // True if pc relative load works.
    const bool support_boot_image_fixup = GetSupportBootImageFixup();
    if (is_image_class && support_boot_image_fixup) {
      *is_type_initialized = resolved_class->IsInitialized();
      *use_direct_type_ptr = false;
      *direct_type_ptr = 0;
      return true;
    } else {
      return false;
    }
  } else {
    // True if the class is in the image at app compiling time.
    const bool class_in_image =
        Runtime::Current()->GetHeap()->FindSpaceFromObject(resolved_class, false)->IsImageSpace();
    if (class_in_image) {
      // boot -> app class pointers.
      *is_type_initialized = resolved_class->IsInitialized();
      *use_direct_type_ptr = true;
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

bool CompilerDriver::ComputeInstanceFieldInfo(uint32_t field_idx, const DexCompilationUnit* mUnit,
                                              bool is_put, MemberOffset* field_offset,
                                              bool* is_volatile) {
  ScopedObjectAccess soa(Thread::Current());
  // Try to resolve the field and compiling method's class.
  mirror::ArtField* resolved_field;
  mirror::Class* referrer_class;
  mirror::DexCache* dex_cache;
  {
    StackHandleScope<3> hs(soa.Self());
    Handle<mirror::DexCache> dex_cache_handle(
        hs.NewHandle(mUnit->GetClassLinker()->FindDexCache(*mUnit->GetDexFile())));
    Handle<mirror::ClassLoader> class_loader_handle(
        hs.NewHandle(soa.Decode<mirror::ClassLoader*>(mUnit->GetClassLoader())));
    Handle<mirror::ArtField> resolved_field_handle(hs.NewHandle(
        ResolveField(soa, dex_cache_handle, class_loader_handle, mUnit, field_idx, false)));
    referrer_class = (resolved_field_handle.Get() != nullptr)
        ? ResolveCompilingMethodsClass(soa, dex_cache_handle, class_loader_handle, mUnit) : nullptr;
    resolved_field = resolved_field_handle.Get();
    dex_cache = dex_cache_handle.Get();
  }
  bool result = false;
  if (resolved_field != nullptr && referrer_class != nullptr) {
    *is_volatile = IsFieldVolatile(resolved_field);
    std::pair<bool, bool> fast_path = IsFastInstanceField(
        dex_cache, referrer_class, resolved_field, field_idx, field_offset);
    result = is_put ? fast_path.second : fast_path.first;
  }
  if (!result) {
    // Conservative defaults.
    *is_volatile = true;
    *field_offset = MemberOffset(static_cast<size_t>(-1));
  }
  ProcessedInstanceField(result);
  return result;
}

bool CompilerDriver::ComputeStaticFieldInfo(uint32_t field_idx, const DexCompilationUnit* mUnit,
                                            bool is_put, MemberOffset* field_offset,
                                            uint32_t* storage_index, bool* is_referrers_class,
                                            bool* is_volatile, bool* is_initialized) {
  ScopedObjectAccess soa(Thread::Current());
  // Try to resolve the field and compiling method's class.
  mirror::ArtField* resolved_field;
  mirror::Class* referrer_class;
  mirror::DexCache* dex_cache;
  {
    StackHandleScope<3> hs(soa.Self());
    Handle<mirror::DexCache> dex_cache_handle(
        hs.NewHandle(mUnit->GetClassLinker()->FindDexCache(*mUnit->GetDexFile())));
    Handle<mirror::ClassLoader> class_loader_handle(
        hs.NewHandle(soa.Decode<mirror::ClassLoader*>(mUnit->GetClassLoader())));
    Handle<mirror::ArtField> resolved_field_handle(hs.NewHandle(
        ResolveField(soa, dex_cache_handle, class_loader_handle, mUnit, field_idx, true)));
    referrer_class = (resolved_field_handle.Get() != nullptr)
        ? ResolveCompilingMethodsClass(soa, dex_cache_handle, class_loader_handle, mUnit) : nullptr;
    resolved_field = resolved_field_handle.Get();
    dex_cache = dex_cache_handle.Get();
  }
  bool result = false;
  if (resolved_field != nullptr && referrer_class != nullptr) {
    *is_volatile = IsFieldVolatile(resolved_field);
    std::pair<bool, bool> fast_path = IsFastStaticField(
        dex_cache, referrer_class, resolved_field, field_idx, field_offset,
        storage_index, is_referrers_class, is_initialized);
    result = is_put ? fast_path.second : fast_path.first;
  }
  if (!result) {
    // Conservative defaults.
    *is_volatile = true;
    *field_offset = MemberOffset(static_cast<size_t>(-1));
    *storage_index = -1;
    *is_referrers_class = false;
    *is_initialized = false;
  }
  ProcessedStaticField(result, *is_referrers_class);
  return result;
}

void CompilerDriver::GetCodeAndMethodForDirectCall(InvokeType* type, InvokeType sharp_type,
                                                   bool no_guarantee_of_dex_cache_entry,
                                                   mirror::Class* referrer_class,
                                                   mirror::ArtMethod* method,
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
  bool use_dex_cache = false;
  const bool compiling_boot = Runtime::Current()->GetHeap()->IsCompilingBoot();
  if (compiler_->IsPortable()) {
    if (sharp_type != kStatic && sharp_type != kDirect) {
      return;
    }
    use_dex_cache = true;
  } else {
    if (sharp_type != kStatic && sharp_type != kDirect) {
      return;
    }
    // TODO: support patching on all architectures.
    use_dex_cache = compiling_boot && !support_boot_image_fixup_;
  }
  bool method_code_in_boot = (method->GetDeclaringClass()->GetClassLoader() == nullptr);
  if (!use_dex_cache) {
    if (!method_code_in_boot) {
      use_dex_cache = true;
    } else {
      bool has_clinit_trampoline =
          method->IsStatic() && !method->GetDeclaringClass()->IsInitialized();
      if (has_clinit_trampoline && (method->GetDeclaringClass() != referrer_class)) {
        // Ensure we run the clinit trampoline unless we are invoking a static method in the same
        // class.
        use_dex_cache = true;
      }
    }
  }
  if (method_code_in_boot) {
    *stats_flags |= kFlagDirectCallToBoot | kFlagDirectMethodToBoot;
  }
  if (!use_dex_cache && compiling_boot) {
    MethodHelper mh(method);
    if (!IsImageClass(mh.GetDeclaringClassDescriptor())) {
      // We can only branch directly to Methods that are resolved in the DexCache.
      // Otherwise we won't invoke the resolution trampoline.
      use_dex_cache = true;
    }
  }
  // The method is defined not within this dex file. We need a dex cache slot within the current
  // dex file or direct pointers.
  bool must_use_direct_pointers = false;
  if (target_method->dex_file == method->GetDeclaringClass()->GetDexCache()->GetDexFile()) {
    target_method->dex_method_index = method->GetDexMethodIndex();
  } else {
    if (no_guarantee_of_dex_cache_entry) {
      // See if the method is also declared in this dex cache.
      uint32_t dex_method_idx = MethodHelper(method).FindDexMethodIndexInOtherDexFile(
          *target_method->dex_file, target_method->dex_method_index);
      if (dex_method_idx != DexFile::kDexNoIndex) {
        target_method->dex_method_index = dex_method_idx;
      } else {
        if (compiling_boot) {
          target_method->dex_method_index = method->GetDexMethodIndex();
          target_method->dex_file = method->GetDeclaringClass()->GetDexCache()->GetDexFile();
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
    if (compiling_boot) {
      *type = sharp_type;
      *direct_method = -1;
      *direct_code = -1;
    } else {
      bool method_in_image =
          Runtime::Current()->GetHeap()->FindSpaceFromObject(method, false)->IsImageSpace();
      if (method_in_image) {
        CHECK(!method->IsAbstract());
        *type = sharp_type;
        *direct_method = reinterpret_cast<uintptr_t>(method);
        *direct_code = compiler_->GetEntryPointOf(method);
        target_method->dex_file = method->GetDeclaringClass()->GetDexCache()->GetDexFile();
        target_method->dex_method_index = method->GetDexMethodIndex();
      } else if (!must_use_direct_pointers) {
        // Set the code and rely on the dex cache for the method.
        *type = sharp_type;
        *direct_code = compiler_->GetEntryPointOf(method);
      } else {
        // Direct pointers were required but none were available.
        VLOG(compiler) << "Dex cache devirtualization failed for: " << PrettyMethod(method);
      }
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
  mirror::ArtMethod* resolved_method;
  mirror::Class* referrer_class;
  StackHandleScope<3> hs(soa.Self());
  Handle<mirror::DexCache> dex_cache(
      hs.NewHandle(mUnit->GetClassLinker()->FindDexCache(*mUnit->GetDexFile())));
  Handle<mirror::ClassLoader> class_loader(hs.NewHandle(
      soa.Decode<mirror::ClassLoader*>(mUnit->GetClassLoader())));
  {
    uint32_t method_idx = target_method->dex_method_index;
    Handle<mirror::ArtMethod> resolved_method_handle(hs.NewHandle(
        ResolveMethod(soa, dex_cache, class_loader, mUnit, method_idx, orig_invoke_type)));
    referrer_class = (resolved_method_handle.Get() != nullptr)
        ? ResolveCompilingMethodsClass(soa, dex_cache, class_loader, mUnit) : nullptr;
    resolved_method = resolved_method_handle.Get();
  }
  bool result = false;
  if (resolved_method != nullptr) {
    *vtable_idx = GetResolvedMethodVTableIndex(resolved_method, orig_invoke_type);

    if (enable_devirtualization) {
      DCHECK(mUnit->GetVerifiedMethod() != nullptr);
      const MethodReference* devirt_target = mUnit->GetVerifiedMethod()->GetDevirtTarget(dex_pc);

      stats_flags = IsFastInvoke(
          soa, dex_cache, class_loader, mUnit, referrer_class, resolved_method,
          invoke_type, target_method, devirt_target, direct_code, direct_method);
      result = stats_flags != 0;
    } else {
      // Devirtualization not enabled. Inline IsFastInvoke(), dropping the devirtualization parts.
      if (UNLIKELY(referrer_class == nullptr) ||
          UNLIKELY(!referrer_class->CanAccessResolvedMethod(resolved_method->GetDeclaringClass(),
                                                            resolved_method, dex_cache.Get(),
                                                            target_method->dex_method_index)) ||
          *invoke_type == kSuper) {
        // Slow path. (Without devirtualization, all super calls go slow path as well.)
      } else {
        // Sharpening failed so generate a regular resolved method dispatch.
        stats_flags = kFlagMethodResolved;
        GetCodeAndMethodForDirectCall(invoke_type, *invoke_type, false, referrer_class, resolved_method,
                                      &stats_flags, target_method, direct_code, direct_method);
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
  DCHECK(mUnit->GetVerifiedMethod() != nullptr);
  bool result = mUnit->GetVerifiedMethod()->IsSafeCast(dex_pc);
  if (result) {
    stats_->SafeCast();
  } else {
    stats_->NotASafeCast();
  }
  return result;
}

void CompilerDriver::AddCodePatch(const DexFile* dex_file,
                                  uint16_t referrer_class_def_idx,
                                  uint32_t referrer_method_idx,
                                  InvokeType referrer_invoke_type,
                                  uint32_t target_method_idx,
                                  const DexFile* target_dex_file,
                                  InvokeType target_invoke_type,
                                  size_t literal_offset) {
  MutexLock mu(Thread::Current(), compiled_methods_lock_);
  code_to_patch_.push_back(new CallPatchInformation(dex_file,
                                                    referrer_class_def_idx,
                                                    referrer_method_idx,
                                                    referrer_invoke_type,
                                                    target_method_idx,
                                                    target_dex_file,
                                                    target_invoke_type,
                                                    literal_offset));
}
void CompilerDriver::AddRelativeCodePatch(const DexFile* dex_file,
                                          uint16_t referrer_class_def_idx,
                                          uint32_t referrer_method_idx,
                                          InvokeType referrer_invoke_type,
                                          uint32_t target_method_idx,
                                          const DexFile* target_dex_file,
                                          InvokeType target_invoke_type,
                                          size_t literal_offset,
                                          int32_t pc_relative_offset) {
  MutexLock mu(Thread::Current(), compiled_methods_lock_);
  code_to_patch_.push_back(new RelativeCallPatchInformation(dex_file,
                                                            referrer_class_def_idx,
                                                            referrer_method_idx,
                                                            referrer_invoke_type,
                                                            target_method_idx,
                                                            target_dex_file,
                                                            target_invoke_type,
                                                            literal_offset,
                                                            pc_relative_offset));
}
void CompilerDriver::AddMethodPatch(const DexFile* dex_file,
                                    uint16_t referrer_class_def_idx,
                                    uint32_t referrer_method_idx,
                                    InvokeType referrer_invoke_type,
                                    uint32_t target_method_idx,
                                    const DexFile* target_dex_file,
                                    InvokeType target_invoke_type,
                                    size_t literal_offset) {
  MutexLock mu(Thread::Current(), compiled_methods_lock_);
  methods_to_patch_.push_back(new CallPatchInformation(dex_file,
                                                       referrer_class_def_idx,
                                                       referrer_method_idx,
                                                       referrer_invoke_type,
                                                       target_method_idx,
                                                       target_dex_file,
                                                       target_invoke_type,
                                                       literal_offset));
}
void CompilerDriver::AddClassPatch(const DexFile* dex_file,
                                    uint16_t referrer_class_def_idx,
                                    uint32_t referrer_method_idx,
                                    uint32_t target_type_idx,
                                    size_t literal_offset) {
  MutexLock mu(Thread::Current(), compiled_methods_lock_);
  classes_to_patch_.push_back(new TypePatchInformation(dex_file,
                                                       referrer_class_def_idx,
                                                       referrer_method_idx,
                                                       target_type_idx,
                                                       literal_offset));
}

class ParallelCompilationManager {
 public:
  typedef void Callback(const ParallelCompilationManager* manager, size_t index);

  ParallelCompilationManager(ClassLinker* class_linker,
                             jobject class_loader,
                             CompilerDriver* compiler,
                             const DexFile* dex_file,
                             ThreadPool* thread_pool)
    : index_(0),
      class_linker_(class_linker),
      class_loader_(class_loader),
      compiler_(compiler),
      dex_file_(dex_file),
      thread_pool_(thread_pool) {}

  ClassLinker* GetClassLinker() const {
    CHECK(class_linker_ != NULL);
    return class_linker_;
  }

  jobject GetClassLoader() const {
    return class_loader_;
  }

  CompilerDriver* GetCompiler() const {
    CHECK(compiler_ != NULL);
    return compiler_;
  }

  const DexFile* GetDexFile() const {
    CHECK(dex_file_ != NULL);
    return dex_file_;
  }

  void ForAll(size_t begin, size_t end, Callback callback, size_t work_units) {
    Thread* self = Thread::Current();
    self->AssertNoPendingException();
    CHECK_GT(work_units, 0U);

    index_ = begin;
    for (size_t i = 0; i < work_units; ++i) {
      thread_pool_->AddTask(self, new ForAllClosure(this, end, callback));
    }
    thread_pool_->StartWorkers(self);

    // Ensure we're suspended while we're blocked waiting for the other threads to finish (worker
    // thread destructor's called below perform join).
    CHECK_NE(self->GetState(), kRunnable);

    // Wait for all the worker threads to finish.
    thread_pool_->Wait(self, true, false);
  }

  size_t NextIndex() {
    return index_.FetchAndAdd(1);
  }

 private:
  class ForAllClosure : public Task {
   public:
    ForAllClosure(ParallelCompilationManager* manager, size_t end, Callback* callback)
        : manager_(manager),
          end_(end),
          callback_(callback) {}

    virtual void Run(Thread* self) {
      while (true) {
        const size_t index = manager_->NextIndex();
        if (UNLIKELY(index >= end_)) {
          break;
        }
        callback_(manager_, index);
        self->AssertNoPendingException();
      }
    }

    virtual void Finalize() {
      delete this;
    }

   private:
    ParallelCompilationManager* const manager_;
    const size_t end_;
    Callback* const callback_;
  };

  AtomicInteger index_;
  ClassLinker* const class_linker_;
  const jobject class_loader_;
  CompilerDriver* const compiler_;
  const DexFile* const dex_file_;
  ThreadPool* const thread_pool_;

  DISALLOW_COPY_AND_ASSIGN(ParallelCompilationManager);
};

// Return true if the class should be skipped during compilation.
//
// The first case where we skip is for redundant class definitions in
// the boot classpath. We skip all but the first definition in that case.
//
// The second case where we skip is when an app bundles classes found
// in the boot classpath. Since at runtime we will select the class from
// the boot classpath, we ignore the one from the app.
static bool SkipClass(ClassLinker* class_linker, jobject class_loader, const DexFile& dex_file,
                      const DexFile::ClassDef& class_def) {
  const char* descriptor = dex_file.GetClassDescriptor(class_def);
  if (class_loader == NULL) {
    DexFile::ClassPathEntry pair = DexFile::FindInClassPath(descriptor, class_linker->GetBootClassPath());
    CHECK(pair.second != NULL);
    if (pair.first != &dex_file) {
      LOG(WARNING) << "Skipping class " << descriptor << " from " << dex_file.GetLocation()
                   << " previously found in " << pair.first->GetLocation();
      return true;
    }
    return false;
  }
  return class_linker->IsInBootClassPath(descriptor);
}

// A fast version of SkipClass above if the class pointer is available
// that avoids the expensive FindInClassPath search.
static bool SkipClass(jobject class_loader, const DexFile& dex_file, mirror::Class* klass)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DCHECK(klass != NULL);
  const DexFile& original_dex_file = *klass->GetDexCache()->GetDexFile();
  if (&dex_file != &original_dex_file) {
    if (class_loader == NULL) {
      LOG(WARNING) << "Skipping class " << PrettyDescriptor(klass) << " from "
                   << dex_file.GetLocation() << " previously found in "
                   << original_dex_file.GetLocation();
    }
    return true;
  }
  return false;
}

static void ResolveClassFieldsAndMethods(const ParallelCompilationManager* manager,
                                         size_t class_def_index)
    LOCKS_EXCLUDED(Locks::mutator_lock_) {
  ATRACE_CALL();
  Thread* self = Thread::Current();
  jobject jclass_loader = manager->GetClassLoader();
  const DexFile& dex_file = *manager->GetDexFile();
  ClassLinker* class_linker = manager->GetClassLinker();

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
  if (!SkipClass(class_linker, jclass_loader, dex_file, class_def)) {
    ScopedObjectAccess soa(self);
    StackHandleScope<2> hs(soa.Self());
    Handle<mirror::ClassLoader> class_loader(
        hs.NewHandle(soa.Decode<mirror::ClassLoader*>(jclass_loader)));
    Handle<mirror::DexCache> dex_cache(hs.NewHandle(class_linker->FindDexCache(dex_file)));
    // Resolve the class.
    mirror::Class* klass = class_linker->ResolveType(dex_file, class_def.class_idx_, dex_cache,
                                                     class_loader);
    bool resolve_fields_and_methods;
    if (klass == NULL) {
      // Class couldn't be resolved, for example, super-class is in a different dex file. Don't
      // attempt to resolve methods and fields when there is no declaring class.
      CHECK(soa.Self()->IsExceptionPending());
      soa.Self()->ClearException();
      resolve_fields_and_methods = false;
    } else {
      resolve_fields_and_methods = manager->GetCompiler()->IsImage();
    }
    // Note the class_data pointer advances through the headers,
    // static fields, instance fields, direct methods, and virtual
    // methods.
    const byte* class_data = dex_file.GetClassData(class_def);
    if (class_data == NULL) {
      // Empty class such as a marker interface.
      requires_constructor_barrier = false;
    } else {
      ClassDataItemIterator it(dex_file, class_data);
      while (it.HasNextStaticField()) {
        if (resolve_fields_and_methods) {
          mirror::ArtField* field = class_linker->ResolveField(dex_file, it.GetMemberIndex(),
                                                               dex_cache, class_loader, true);
          if (field == NULL) {
            CHECK(soa.Self()->IsExceptionPending());
            soa.Self()->ClearException();
          }
        }
        it.Next();
      }
      // We require a constructor barrier if there are final instance fields.
      requires_constructor_barrier = false;
      while (it.HasNextInstanceField()) {
        if ((it.GetMemberAccessFlags() & kAccFinal) != 0) {
          requires_constructor_barrier = true;
        }
        if (resolve_fields_and_methods) {
          mirror::ArtField* field = class_linker->ResolveField(dex_file, it.GetMemberIndex(),
                                                               dex_cache, class_loader, false);
          if (field == NULL) {
            CHECK(soa.Self()->IsExceptionPending());
            soa.Self()->ClearException();
          }
        }
        it.Next();
      }
      if (resolve_fields_and_methods) {
        while (it.HasNextDirectMethod()) {
          mirror::ArtMethod* method = class_linker->ResolveMethod(dex_file, it.GetMemberIndex(),
                                                                  dex_cache, class_loader, NULL,
                                                                  it.GetMethodInvokeType(class_def));
          if (method == NULL) {
            CHECK(soa.Self()->IsExceptionPending());
            soa.Self()->ClearException();
          }
          it.Next();
        }
        while (it.HasNextVirtualMethod()) {
          mirror::ArtMethod* method = class_linker->ResolveMethod(dex_file, it.GetMemberIndex(),
                                                                  dex_cache, class_loader, NULL,
                                                                  it.GetMethodInvokeType(class_def));
          if (method == NULL) {
            CHECK(soa.Self()->IsExceptionPending());
            soa.Self()->ClearException();
          }
          it.Next();
        }
        DCHECK(!it.HasNext());
      }
    }
  }
  if (requires_constructor_barrier) {
    manager->GetCompiler()->AddRequiresConstructorBarrier(self, &dex_file, class_def_index);
  }
}

static void ResolveType(const ParallelCompilationManager* manager, size_t type_idx)
    LOCKS_EXCLUDED(Locks::mutator_lock_) {
  // Class derived values are more complicated, they require the linker and loader.
  ScopedObjectAccess soa(Thread::Current());
  ClassLinker* class_linker = manager->GetClassLinker();
  const DexFile& dex_file = *manager->GetDexFile();
  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::DexCache> dex_cache(hs.NewHandle(class_linker->FindDexCache(dex_file)));
  Handle<mirror::ClassLoader> class_loader(
      hs.NewHandle(soa.Decode<mirror::ClassLoader*>(manager->GetClassLoader())));
  mirror::Class* klass = class_linker->ResolveType(dex_file, type_idx, dex_cache, class_loader);

  if (klass == NULL) {
    CHECK(soa.Self()->IsExceptionPending());
    mirror::Throwable* exception = soa.Self()->GetException(NULL);
    VLOG(compiler) << "Exception during type resolution: " << exception->Dump();
    if (strcmp("Ljava/lang/OutOfMemoryError;",
               ClassHelper(exception->GetClass()).GetDescriptor()) == 0) {
      // There's little point continuing compilation if the heap is exhausted.
      LOG(FATAL) << "Out of memory during type resolution for compilation";
    }
    soa.Self()->ClearException();
  }
}

void CompilerDriver::ResolveDexFile(jobject class_loader, const DexFile& dex_file,
                                    ThreadPool* thread_pool, TimingLogger* timings) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();

  // TODO: we could resolve strings here, although the string table is largely filled with class
  //       and method names.

  ParallelCompilationManager context(class_linker, class_loader, this, &dex_file, thread_pool);
  if (IsImage()) {
    // For images we resolve all types, such as array, whereas for applications just those with
    // classdefs are resolved by ResolveClassFieldsAndMethods.
    timings->NewSplit("Resolve Types");
    context.ForAll(0, dex_file.NumTypeIds(), ResolveType, thread_count_);
  }

  timings->NewSplit("Resolve MethodsAndFields");
  context.ForAll(0, dex_file.NumClassDefs(), ResolveClassFieldsAndMethods, thread_count_);
}

void CompilerDriver::Verify(jobject class_loader, const std::vector<const DexFile*>& dex_files,
                            ThreadPool* thread_pool, TimingLogger* timings) {
  for (size_t i = 0; i != dex_files.size(); ++i) {
    const DexFile* dex_file = dex_files[i];
    CHECK(dex_file != NULL);
    VerifyDexFile(class_loader, *dex_file, thread_pool, timings);
  }
}

static void VerifyClass(const ParallelCompilationManager* manager, size_t class_def_index)
    LOCKS_EXCLUDED(Locks::mutator_lock_) {
  ATRACE_CALL();
  ScopedObjectAccess soa(Thread::Current());
  const DexFile& dex_file = *manager->GetDexFile();
  const DexFile::ClassDef& class_def = dex_file.GetClassDef(class_def_index);
  const char* descriptor = dex_file.GetClassDescriptor(class_def);
  ClassLinker* class_linker = manager->GetClassLinker();
  jobject jclass_loader = manager->GetClassLoader();
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
    Handle<mirror::DexCache> dex_cache(hs.NewHandle(class_linker->FindDexCache(dex_file)));
    std::string error_msg;
    if (verifier::MethodVerifier::VerifyClass(&dex_file, dex_cache, class_loader, &class_def, true,
                                              &error_msg) ==
                                                  verifier::MethodVerifier::kHardFailure) {
      LOG(ERROR) << "Verification failed on class " << PrettyDescriptor(descriptor)
                 << " because: " << error_msg;
    }
  } else if (!SkipClass(jclass_loader, dex_file, klass.Get())) {
    CHECK(klass->IsResolved()) << PrettyClass(klass.Get());
    class_linker->VerifyClass(klass);

    if (klass->IsErroneous()) {
      // ClassLinker::VerifyClass throws, which isn't useful in the compiler.
      CHECK(soa.Self()->IsExceptionPending());
      soa.Self()->ClearException();
    }

    CHECK(klass->IsCompileTimeVerified() || klass->IsErroneous())
        << PrettyDescriptor(klass.Get()) << ": state=" << klass->GetStatus();
  }
  soa.Self()->AssertNoPendingException();
}

void CompilerDriver::VerifyDexFile(jobject class_loader, const DexFile& dex_file,
                                   ThreadPool* thread_pool, TimingLogger* timings) {
  timings->NewSplit("Verify Dex File");
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  ParallelCompilationManager context(class_linker, class_loader, this, &dex_file, thread_pool);
  context.ForAll(0, dex_file.NumClassDefs(), VerifyClass, thread_count_);
}

static void InitializeClass(const ParallelCompilationManager* manager, size_t class_def_index)
    LOCKS_EXCLUDED(Locks::mutator_lock_) {
  ATRACE_CALL();
  jobject jclass_loader = manager->GetClassLoader();
  const DexFile& dex_file = *manager->GetDexFile();
  const DexFile::ClassDef& class_def = dex_file.GetClassDef(class_def_index);
  const DexFile::TypeId& class_type_id = dex_file.GetTypeId(class_def.class_idx_);
  const char* descriptor = dex_file.StringDataByIdx(class_type_id.descriptor_idx_);

  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<3> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(
      hs.NewHandle(soa.Decode<mirror::ClassLoader*>(jclass_loader)));
  Handle<mirror::Class> klass(
      hs.NewHandle(manager->GetClassLinker()->FindClass(soa.Self(), descriptor, class_loader)));

  if (klass.Get() != nullptr && !SkipClass(jclass_loader, dex_file, klass.Get())) {
    // Only try to initialize classes that were successfully verified.
    if (klass->IsVerified()) {
      // Attempt to initialize the class but bail if we either need to initialize the super-class
      // or static fields.
      manager->GetClassLinker()->EnsureInitialized(klass, false, false);
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
        ObjectLock<mirror::Class> lock(soa.Self(), &h_klass);
        // Attempt to initialize allowing initialization of parent classes but still not static
        // fields.
        manager->GetClassLinker()->EnsureInitialized(klass, false, true);
        if (!klass->IsInitialized()) {
          // We need to initialize static fields, we only do this for image classes that aren't
          // marked with the $NoPreloadHolder (which implies this should not be initialized early).
          bool can_init_static_fields = manager->GetCompiler()->IsImage() &&
              manager->GetCompiler()->IsImageClass(descriptor) &&
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
            bool success = manager->GetClassLinker()->EnsureInitialized(klass, true, true);
            // TODO we detach transaction from runtime to indicate we quit the transactional
            // mode which prevents the GC from visiting objects modified during the transaction.
            // Ensure GC is not run so don't access freed objects when aborting transaction.
            const char* old_casue = soa.Self()->StartAssertNoThreadSuspension("Transaction end");
            runtime->ExitTransactionMode();

            if (!success) {
              CHECK(soa.Self()->IsExceptionPending());
              ThrowLocation throw_location;
              mirror::Throwable* exception = soa.Self()->GetException(&throw_location);
              VLOG(compiler) << "Initialization of " << descriptor << " aborted because of "
                  << exception->Dump();
              soa.Self()->ClearException();
              transaction.Abort();
              CHECK_EQ(old_status, klass->GetStatus()) << "Previous class status not restored";
            }
            soa.Self()->EndAssertNoThreadSuspension(old_casue);
          }
        }
        soa.Self()->AssertNoPendingException();
      }
    }
    // Record the final class status if necessary.
    ClassReference ref(manager->GetDexFile(), class_def_index);
    manager->GetCompiler()->RecordClassStatus(ref, klass->GetStatus());
  }
  // Clear any class not found or verification exceptions.
  soa.Self()->ClearException();
}

void CompilerDriver::InitializeClasses(jobject jni_class_loader, const DexFile& dex_file,
                                       ThreadPool* thread_pool, TimingLogger* timings) {
  timings->NewSplit("InitializeNoClinit");
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  ParallelCompilationManager context(class_linker, jni_class_loader, this, &dex_file, thread_pool);
  size_t thread_count;
  if (IsImage()) {
    // TODO: remove this when transactional mode supports multithreading.
    thread_count = 1U;
  } else {
    thread_count = thread_count_;
  }
  context.ForAll(0, dex_file.NumClassDefs(), InitializeClass, thread_count);
  if (IsImage()) {
    // Prune garbage objects created during aborted transactions.
    Runtime::Current()->GetHeap()->CollectGarbage(true);
  }
}

void CompilerDriver::InitializeClasses(jobject class_loader,
                                       const std::vector<const DexFile*>& dex_files,
                                       ThreadPool* thread_pool, TimingLogger* timings) {
  for (size_t i = 0; i != dex_files.size(); ++i) {
    const DexFile* dex_file = dex_files[i];
    CHECK(dex_file != NULL);
    InitializeClasses(class_loader, *dex_file, thread_pool, timings);
  }
}

void CompilerDriver::Compile(jobject class_loader, const std::vector<const DexFile*>& dex_files,
                             ThreadPool* thread_pool, TimingLogger* timings) {
  for (size_t i = 0; i != dex_files.size(); ++i) {
    const DexFile* dex_file = dex_files[i];
    CHECK(dex_file != NULL);
    CompileDexFile(class_loader, *dex_file, thread_pool, timings);
  }
}

void CompilerDriver::CompileClass(const ParallelCompilationManager* manager, size_t class_def_index) {
  ATRACE_CALL();
  jobject jclass_loader = manager->GetClassLoader();
  const DexFile& dex_file = *manager->GetDexFile();
  const DexFile::ClassDef& class_def = dex_file.GetClassDef(class_def_index);
  ClassLinker* class_linker = manager->GetClassLinker();
  if (SkipClass(class_linker, jclass_loader, dex_file, class_def)) {
    return;
  }
  ClassReference ref(&dex_file, class_def_index);
  // Skip compiling classes with generic verifier failures since they will still fail at runtime
  if (manager->GetCompiler()->verification_results_->IsClassRejected(ref)) {
    return;
  }
  const byte* class_data = dex_file.GetClassData(class_def);
  if (class_data == NULL) {
    // empty class, probably a marker interface
    return;
  }

  // Can we run DEX-to-DEX compiler on this class ?
  DexToDexCompilationLevel dex_to_dex_compilation_level = kDontDexToDexCompile;
  {
    ScopedObjectAccess soa(Thread::Current());
    StackHandleScope<1> hs(soa.Self());
    Handle<mirror::ClassLoader> class_loader(
        hs.NewHandle(soa.Decode<mirror::ClassLoader*>(jclass_loader)));
    dex_to_dex_compilation_level = GetDexToDexCompilationlevel(soa.Self(), class_loader, dex_file,
                                                               class_def);
  }
  ClassDataItemIterator it(dex_file, class_data);
  // Skip fields
  while (it.HasNextStaticField()) {
    it.Next();
  }
  while (it.HasNextInstanceField()) {
    it.Next();
  }
  CompilerDriver* driver = manager->GetCompiler();
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
    driver->CompileMethod(it.GetMethodCodeItem(), it.GetMemberAccessFlags(),
                          it.GetMethodInvokeType(class_def), class_def_index,
                          method_idx, jclass_loader, dex_file, dex_to_dex_compilation_level);
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
    driver->CompileMethod(it.GetMethodCodeItem(), it.GetMemberAccessFlags(),
                          it.GetMethodInvokeType(class_def), class_def_index,
                          method_idx, jclass_loader, dex_file, dex_to_dex_compilation_level);
    it.Next();
  }
  DCHECK(!it.HasNext());
}

void CompilerDriver::CompileDexFile(jobject class_loader, const DexFile& dex_file,
                                    ThreadPool* thread_pool, TimingLogger* timings) {
  timings->NewSplit("Compile Dex File");
  ParallelCompilationManager context(Runtime::Current()->GetClassLinker(), class_loader, this,
                                     &dex_file, thread_pool);
  context.ForAll(0, dex_file.NumClassDefs(), CompilerDriver::CompileClass, thread_count_);
}

void CompilerDriver::CompileMethod(const DexFile::CodeItem* code_item, uint32_t access_flags,
                                   InvokeType invoke_type, uint16_t class_def_idx,
                                   uint32_t method_idx, jobject class_loader,
                                   const DexFile& dex_file,
                                   DexToDexCompilationLevel dex_to_dex_compilation_level) {
  CompiledMethod* compiled_method = NULL;
  uint64_t start_ns = NanoTime();

  if ((access_flags & kAccNative) != 0) {
    // Are we interpreting only and have support for generic JNI down calls?
    if (!compiler_options_->IsCompilationEnabled() &&
        (instruction_set_ == kX86_64 || instruction_set_ == kArm64)) {
      // Leaving this empty will trigger the generic JNI version
    } else {
      compiled_method = compiler_->JniCompile(access_flags, method_idx, dex_file);
      CHECK(compiled_method != NULL);
    }
  } else if ((access_flags & kAccAbstract) != 0) {
  } else {
    MethodReference method_ref(&dex_file, method_idx);
    bool compile = verification_results_->IsCandidateForCompilation(method_ref, access_flags);
    if (compile) {
      // NOTE: if compiler declines to compile this method, it will return NULL.
      compiled_method = compiler_->Compile(code_item, access_flags, invoke_type, class_def_idx,
                                           method_idx, class_loader, dex_file);
    }
    if (compiled_method == nullptr && dex_to_dex_compilation_level != kDontDexToDexCompile) {
      // TODO: add a command-line option to disable DEX-to-DEX compilation ?
      (*dex_to_dex_compiler_)(*this, code_item, access_flags,
                              invoke_type, class_def_idx,
                              method_idx, class_loader, dex_file,
                              dex_to_dex_compilation_level);
    }
  }
  uint64_t duration_ns = NanoTime() - start_ns;
  if (duration_ns > MsToNs(compiler_->GetMaximumCompilationTimeBeforeWarning())) {
    LOG(WARNING) << "Compilation of " << PrettyMethod(method_idx, dex_file)
                 << " took " << PrettyDuration(duration_ns);
  }

  Thread* self = Thread::Current();
  if (compiled_method != NULL) {
    MethodReference ref(&dex_file, method_idx);
    DCHECK(GetCompiledMethod(ref) == NULL) << PrettyMethod(method_idx, dex_file);
    {
      MutexLock mu(self, compiled_methods_lock_);
      compiled_methods_.Put(ref, compiled_method);
    }
    DCHECK(GetCompiledMethod(ref) != NULL) << PrettyMethod(method_idx, dex_file);
  }

  if (self->IsExceptionPending()) {
    ScopedObjectAccess soa(self);
    LOG(FATAL) << "Unexpected exception compiling: " << PrettyMethod(method_idx, dex_file) << "\n"
        << self->GetException(NULL)->Dump();
  }
}

CompiledClass* CompilerDriver::GetCompiledClass(ClassReference ref) const {
  MutexLock mu(Thread::Current(), compiled_classes_lock_);
  ClassTable::const_iterator it = compiled_classes_.find(ref);
  if (it == compiled_classes_.end()) {
    return NULL;
  }
  CHECK(it->second != NULL);
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
    return NULL;
  }
  CHECK(it->second != NULL);
  return it->second;
}

void CompilerDriver::AddRequiresConstructorBarrier(Thread* self, const DexFile* dex_file,
                                                   uint16_t class_def_index) {
  WriterMutexLock mu(self, freezing_constructor_lock_);
  freezing_constructor_classes_.insert(ClassReference(dex_file, class_def_index));
}

bool CompilerDriver::RequiresConstructorBarrier(Thread* self, const DexFile* dex_file,
                                                uint16_t class_def_index) {
  ReaderMutexLock mu(self, freezing_constructor_lock_);
  return freezing_constructor_classes_.count(ClassReference(dex_file, class_def_index)) != 0;
}

bool CompilerDriver::WriteElf(const std::string& android_root,
                              bool is_host,
                              const std::vector<const art::DexFile*>& dex_files,
                              OatWriter* oat_writer,
                              art::File* file)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return compiler_->WriteElf(file, oat_writer, dex_files, android_root, is_host);
}
void CompilerDriver::InstructionSetToLLVMTarget(InstructionSet instruction_set,
                                                std::string* target_triple,
                                                std::string* target_cpu,
                                                std::string* target_attr) {
  switch (instruction_set) {
    case kThumb2:
      *target_triple = "thumb-none-linux-gnueabi";
      *target_cpu = "cortex-a9";
      *target_attr = "+thumb2,+neon,+neonfp,+vfp3,+db";
      break;

    case kArm:
      *target_triple = "armv7-none-linux-gnueabi";
      // TODO: Fix for Nexus S.
      *target_cpu = "cortex-a9";
      // TODO: Fix for Xoom.
      *target_attr = "+v7,+neon,+neonfp,+vfp3,+db";
      break;

    case kX86:
      *target_triple = "i386-pc-linux-gnu";
      *target_attr = "";
      break;

    case kX86_64:
      *target_triple = "x86_64-pc-linux-gnu";
      *target_attr = "";
      break;

    case kMips:
      *target_triple = "mipsel-unknown-linux";
      *target_attr = "mips32r2";
      break;

    default:
      LOG(FATAL) << "Unknown instruction set: " << instruction_set;
    }
  }

bool CompilerDriver::SkipCompilation(const std::string& method_name) {
  if (!profile_ok_) {
    return false;
  }
  // Methods that comprise topKPercentThreshold % of the total samples will be compiled.
  double topKPercentThreshold = 90.0;
#ifdef HAVE_ANDROID_OS
  char buf[PROP_VALUE_MAX];
  property_get("dalvik.vm.profile.compile_thr", buf, "90.0");
  topKPercentThreshold = strtod(buf, nullptr);
#endif
  // Test for reasonable thresholds.
  if (topKPercentThreshold < 10.0 || topKPercentThreshold > 90.0) {
    topKPercentThreshold = 90.0;
  }

  // First find the method in the profile map.
  ProfileMap::iterator i = profile_map_.find(method_name);
  if (i == profile_map_.end()) {
    // Not in profile, no information can be determined.
    VLOG(compiler) << "not compiling " << method_name << " because it's not in the profile";
    return true;
  }
  const ProfileData& data = i->second;

  // Compare against the start of the topK percentage bucket just in case the threshold
  // falls inside a bucket.
  bool compile = data.GetTopKUsedPercentage() - data.GetUsedPercent() <= topKPercentThreshold;
  if (compile) {
    LOG(INFO) << "compiling method " << method_name << " because its usage is part of top "
        << data.GetTopKUsedPercentage() << "% with a percent of " << data.GetUsedPercent() << "%";
  } else {
    VLOG(compiler) << "not compiling method " << method_name << " because it's not part of leading "
        << topKPercentThreshold << "% samples)";
  }
  return !compile;
}
}  // namespace art
