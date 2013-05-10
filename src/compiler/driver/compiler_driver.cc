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

#include <vector>

#include <dlfcn.h>
#include <unistd.h>

#include "base/stl_util.h"
#include "base/timing_logger.h"
#include "class_linker.h"
#include "dex_compilation_unit.h"
#include "dex_file-inl.h"
#include "jni_internal.h"
#include "oat_file.h"
#include "object_utils.h"
#include "runtime.h"
#include "gc/card_table-inl.h"
#include "gc/space.h"
#include "mirror/class_loader.h"
#include "mirror/class-inl.h"
#include "mirror/dex_cache.h"
#include "mirror/field-inl.h"
#include "mirror/abstract_method-inl.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/throwable.h"
#include "scoped_thread_state_change.h"
#include "ScopedLocalRef.h"
#include "thread.h"
#include "thread_pool.h"
#include "verifier/method_verifier.h"

#if defined(__APPLE__)
#include <mach-o/dyld.h>
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

class AOTCompilationStats {
 public:
  AOTCompilationStats()
      : stats_lock_("AOT compilation statistics lock"),
        types_in_dex_cache_(0), types_not_in_dex_cache_(0),
        strings_in_dex_cache_(0), strings_not_in_dex_cache_(0),
        resolved_types_(0), unresolved_types_(0),
        resolved_instance_fields_(0), unresolved_instance_fields_(0),
        resolved_local_static_fields_(0), resolved_static_fields_(0), unresolved_static_fields_(0),
        type_based_devirtualization_(0) {
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
    DumpStat(type_based_devirtualization_,virtual_made_direct_[kInterface] + virtual_made_direct_[kVirtual]
             - type_based_devirtualization_, "sharpened calls based on type information");

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

  void PreciseTypeDevirtualization() {
    STATS_LOCK();
    type_based_devirtualization_++;
  }
  void ResolvedMethod(InvokeType type) {
    DCHECK_LE(type, kMaxInvokeType);
    STATS_LOCK();
    resolved_methods_[type]++;
  }

  void UnresolvedMethod(InvokeType type) {
    DCHECK_LE(type, kMaxInvokeType);
    STATS_LOCK();
    unresolved_methods_[type]++;
  }

  void VirtualMadeDirect(InvokeType type) {
    DCHECK_LE(type, kMaxInvokeType);
    STATS_LOCK();
    virtual_made_direct_[type]++;
  }

  void DirectCallsToBoot(InvokeType type) {
    DCHECK_LE(type, kMaxInvokeType);
    STATS_LOCK();
    direct_calls_to_boot_[type]++;
  }

  void DirectMethodsToBoot(InvokeType type) {
    DCHECK_LE(type, kMaxInvokeType);
    STATS_LOCK();
    direct_methods_to_boot_[type]++;
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

  DISALLOW_COPY_AND_ASSIGN(AOTCompilationStats);
};

static std::string MakeCompilerSoName(CompilerBackend compiler_backend) {

  // Bad things happen if we pull in the libartd-compiler to a libart dex2oat or vice versa,
  // because we end up with both libart and libartd in the same address space!
  const char* suffix = (kIsDebugBuild ? "d" : "");

  // Work out the filename for the compiler library.
  std::string library_name(StringPrintf("art%s-compiler", suffix));
  std::string filename(StringPrintf(OS_SHARED_LIB_FORMAT_STR, library_name.c_str()));

#if defined(__APPLE__)
  // On Linux, dex2oat will have been built with an RPATH of $ORIGIN/../lib, so dlopen(3) will find
  // the .so by itself. On Mac OS, there isn't really an equivalent, so we have to manually do the
  // same work.
  uint32_t executable_path_length = 0;
  _NSGetExecutablePath(NULL, &executable_path_length);
  std::string path(executable_path_length, static_cast<char>(0));
  CHECK_EQ(_NSGetExecutablePath(&path[0], &executable_path_length), 0);

  // Strip the "/dex2oat".
  size_t last_slash = path.find_last_of('/');
  CHECK_NE(last_slash, std::string::npos) << path;
  path.resize(last_slash);

  // Strip the "/bin".
  last_slash = path.find_last_of('/');
  path.resize(last_slash);

  filename = path + "/lib/" + filename;
#endif
  return filename;
}

template<typename Fn>
static Fn FindFunction(const std::string& compiler_so_name, void* library, const char* name) {
  Fn fn = reinterpret_cast<Fn>(dlsym(library, name));
  if (fn == NULL) {
    LOG(FATAL) << "Couldn't find \"" << name << "\" in compiler library " << compiler_so_name << ": " << dlerror();
  }
  VLOG(compiler) << "Found \"" << name << "\" at " << reinterpret_cast<void*>(fn);
  return fn;
}

CompilerDriver::CompilerDriver(CompilerBackend compiler_backend, InstructionSet instruction_set,
                               bool image, size_t thread_count, bool support_debugging,
                               const std::set<std::string>* image_classes,
                               bool dump_stats, bool dump_timings)
    : compiler_backend_(compiler_backend),
      instruction_set_(instruction_set),
      freezing_constructor_lock_("freezing constructor lock"),
      compiled_classes_lock_("compiled classes lock"),
      compiled_methods_lock_("compiled method lock"),
      image_(image),
      thread_count_(thread_count),
      support_debugging_(support_debugging),
      start_ns_(0),
      stats_(new AOTCompilationStats),
      dump_stats_(dump_stats),
      dump_timings_(dump_timings),
      image_classes_(image_classes),
      compiler_library_(NULL),
      compiler_(NULL),
      compiler_context_(NULL),
      jni_compiler_(NULL),
      compiler_get_method_code_addr_(NULL)
{
  std::string compiler_so_name(MakeCompilerSoName(compiler_backend_));
  compiler_library_ = dlopen(compiler_so_name.c_str(), RTLD_LAZY);
  if (compiler_library_ == NULL) {
    LOG(FATAL) << "Couldn't find compiler library " << compiler_so_name << ": " << dlerror();
  }
  VLOG(compiler) << "dlopen(\"" << compiler_so_name << "\", RTLD_LAZY) returned " << compiler_library_;

  CHECK_PTHREAD_CALL(pthread_key_create, (&tls_key_, NULL), "compiler tls key");

  // TODO: more work needed to combine initializations and allow per-method backend selection
  typedef void (*InitCompilerContextFn)(CompilerDriver&);
  InitCompilerContextFn init_compiler_context;
  if (compiler_backend_ == kPortable){
    // Initialize compiler_context_
    init_compiler_context = FindFunction<void (*)(CompilerDriver&)>(compiler_so_name,
                                                  compiler_library_, "ArtInitCompilerContext");
    compiler_ = FindFunction<CompilerFn>(compiler_so_name, compiler_library_, "ArtCompileMethod");
  } else {
    init_compiler_context = FindFunction<void (*)(CompilerDriver&)>(compiler_so_name,
                                                  compiler_library_, "ArtInitQuickCompilerContext");
    compiler_ = FindFunction<CompilerFn>(compiler_so_name, compiler_library_, "ArtQuickCompileMethod");
  }

  init_compiler_context(*this);

  if (compiler_backend_ == kPortable) {
    jni_compiler_ = FindFunction<JniCompilerFn>(compiler_so_name, compiler_library_, "ArtLLVMJniCompileMethod");
  } else {
    jni_compiler_ = FindFunction<JniCompilerFn>(compiler_so_name, compiler_library_, "ArtQuickJniCompileMethod");
  }

  CHECK(!Runtime::Current()->IsStarted());
  if (!image_) {
    CHECK(image_classes_ == NULL);
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
  CHECK_PTHREAD_CALL(pthread_key_delete, (tls_key_), "delete tls key");
  typedef void (*UninitCompilerContextFn)(CompilerDriver&);
  std::string compiler_so_name(MakeCompilerSoName(compiler_backend_));
  UninitCompilerContextFn uninit_compiler_context;
  // Uninitialize compiler_context_
  // TODO: rework to combine initialization/uninitialization
  if (compiler_backend_ == kPortable) {
    uninit_compiler_context = FindFunction<void (*)(CompilerDriver&)>(compiler_so_name,
                                                    compiler_library_, "ArtUnInitCompilerContext");
  } else {
    uninit_compiler_context = FindFunction<void (*)(CompilerDriver&)>(compiler_so_name,
                                                    compiler_library_, "ArtUnInitQuickCompilerContext");
  }
  uninit_compiler_context(*this);
#if 0
  if (compiler_library_ != NULL) {
    VLOG(compiler) << "dlclose(" << compiler_library_ << ")";
    /*
     * FIXME: Temporary workaround
     * Apparently, llvm is adding dctors to atexit, but if we unload
     * the library here the code will no longer be around at exit time
     * and we die a flaming death in __cxa_finalize().  Apparently, some
     * dlclose() implementations will scan the atexit list on unload and
     * handle any associated with the soon-to-be-unloaded library.
     * However, this is not required by POSIX and we don't do it.
     * See: http://b/issue?id=4998315
     * What's the right thing to do here?
     *
     * This has now been completely disabled because mclinker was
     * closing stdout on exit, which was affecting both quick and
     * portable.
     */
    dlclose(compiler_library_);
  }
#endif
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

void CompilerDriver::CompileAll(jobject class_loader,
                                const std::vector<const DexFile*>& dex_files) {
  DCHECK(!Runtime::Current()->IsStarted());

  UniquePtr<ThreadPool> thread_pool(new ThreadPool(thread_count_));
  TimingLogger timings("compiler", false);

  PreCompile(class_loader, dex_files, *thread_pool.get(), timings);

  Compile(class_loader, dex_files, *thread_pool.get(), timings);

  if (dump_timings_ && timings.GetTotalNs() > MsToNs(1000)) {
    LOG(INFO) << Dumpable<TimingLogger>(timings);
  }

  if (dump_stats_) {
    stats_->Dump();
  }
}

void CompilerDriver::CompileOne(const mirror::AbstractMethod* method) {
  DCHECK(!Runtime::Current()->IsStarted());
  Thread* self = Thread::Current();
  jobject class_loader;
  const DexFile* dex_file;
  uint32_t class_def_idx;
  {
    ScopedObjectAccessUnchecked soa(self);
    ScopedLocalRef<jobject>
      local_class_loader(soa.Env(),
                    soa.AddLocalReference<jobject>(method->GetDeclaringClass()->GetClassLoader()));
    class_loader = soa.Env()->NewGlobalRef(local_class_loader.get());
    // Find the dex_file
    MethodHelper mh(method);
    dex_file = &mh.GetDexFile();
    class_def_idx = mh.GetClassDefIndex();
  }
  self->TransitionFromRunnableToSuspended(kNative);

  std::vector<const DexFile*> dex_files;
  dex_files.push_back(dex_file);

  UniquePtr<ThreadPool> thread_pool(new ThreadPool(1U));
  TimingLogger timings("CompileOne", false);
  PreCompile(class_loader, dex_files, *thread_pool.get(), timings);

  uint32_t method_idx = method->GetDexMethodIndex();
  const DexFile::CodeItem* code_item = dex_file->GetCodeItem(method->GetCodeItemOffset());
  CompileMethod(code_item, method->GetAccessFlags(), method->GetInvokeType(),
                class_def_idx, method_idx, class_loader, *dex_file);

  self->GetJniEnv()->DeleteGlobalRef(class_loader);

  self->TransitionFromSuspendedToRunnable();
}

void CompilerDriver::Resolve(jobject class_loader, const std::vector<const DexFile*>& dex_files,
                             ThreadPool& thread_pool, TimingLogger& timings) {
  for (size_t i = 0; i != dex_files.size(); ++i) {
    const DexFile* dex_file = dex_files[i];
    CHECK(dex_file != NULL);
    ResolveDexFile(class_loader, *dex_file, thread_pool, timings);
  }
}

void CompilerDriver::PreCompile(jobject class_loader, const std::vector<const DexFile*>& dex_files,
                                ThreadPool& thread_pool, TimingLogger& timings) {
  Resolve(class_loader, dex_files, thread_pool, timings);

  Verify(class_loader, dex_files, thread_pool, timings);

  InitializeClasses(class_loader, dex_files, thread_pool, timings);
}

bool CompilerDriver::IsImageClass(const std::string& descriptor) const {
  if (image_classes_ == NULL) {
    return true;
  }
  return image_classes_->find(descriptor) != image_classes_->end();
}

void CompilerDriver::RecordClassStatus(ClassReference ref, CompiledClass* compiled_class) {
  MutexLock mu(Thread::Current(), CompilerDriver::compiled_classes_lock_);
  compiled_classes_.Put(ref, compiled_class);
}

bool CompilerDriver::CanAssumeTypeIsPresentInDexCache(const DexFile& dex_file,
                                                      uint32_t type_idx) {
  ScopedObjectAccess soa(Thread::Current());
  mirror::DexCache* dex_cache = Runtime::Current()->GetClassLinker()->FindDexCache(dex_file);
  if (!IsImage()) {
    stats_->TypeNotInDexCache();
    return false;
  }
  mirror::Class* resolved_class = dex_cache->GetResolvedType(type_idx);
  if (resolved_class == NULL) {
    stats_->TypeNotInDexCache();
    return false;
  }
  bool result = IsImageClass(ClassHelper(resolved_class).GetDescriptor());
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
  if (IsImage()) {
    // We resolve all const-string strings when building for the image.
    ScopedObjectAccess soa(Thread::Current());
    mirror::DexCache* dex_cache = Runtime::Current()->GetClassLinker()->FindDexCache(dex_file);
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
                                                uint32_t type_idx) {
  ScopedObjectAccess soa(Thread::Current());
  mirror::DexCache* dex_cache = Runtime::Current()->GetClassLinker()->FindDexCache(dex_file);
  // Get type from dex cache assuming it was populated by the verifier
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
  // Perform access check, will return true if access is ok or false if we're going to have to
  // check this at runtime (for example for class loaders).
  bool result = referrer_class->CanAccess(resolved_class);
  if (result) {
    stats_->TypeDoesntNeedAccessCheck();
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

static mirror::Class* ComputeCompilingMethodsClass(ScopedObjectAccess& soa,
                                                   const DexCompilationUnit* mUnit)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::DexCache* dex_cache = mUnit->GetClassLinker()->FindDexCache(*mUnit->GetDexFile());
  mirror::ClassLoader* class_loader = soa.Decode<mirror::ClassLoader*>(mUnit->GetClassLoader());
  const DexFile::MethodId& referrer_method_id = mUnit->GetDexFile()->GetMethodId(mUnit->GetDexMethodIndex());
  return mUnit->GetClassLinker()->ResolveType(*mUnit->GetDexFile(), referrer_method_id.class_idx_,
                                              dex_cache, class_loader);
}

static mirror::Field* ComputeFieldReferencedFromCompilingMethod(ScopedObjectAccess& soa,
                                                                const DexCompilationUnit* mUnit,
                                                                uint32_t field_idx)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::DexCache* dex_cache = mUnit->GetClassLinker()->FindDexCache(*mUnit->GetDexFile());
  mirror::ClassLoader* class_loader = soa.Decode<mirror::ClassLoader*>(mUnit->GetClassLoader());
  return mUnit->GetClassLinker()->ResolveField(*mUnit->GetDexFile(), field_idx, dex_cache,
                                               class_loader, false);
}

static mirror::AbstractMethod* ComputeMethodReferencedFromCompilingMethod(ScopedObjectAccess& soa,
                                                                          const DexCompilationUnit* mUnit,
                                                                          uint32_t method_idx,
                                                                          InvokeType type)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::DexCache* dex_cache = mUnit->GetClassLinker()->FindDexCache(*mUnit->GetDexFile());
  mirror::ClassLoader* class_loader = soa.Decode<mirror::ClassLoader*>(mUnit->GetClassLoader());
  return mUnit->GetClassLinker()->ResolveMethod(*mUnit->GetDexFile(), method_idx, dex_cache,
                                                class_loader, NULL, type);
}

bool CompilerDriver::ComputeInstanceFieldInfo(uint32_t field_idx, const DexCompilationUnit* mUnit,
                                              int& field_offset, bool& is_volatile, bool is_put) {
  ScopedObjectAccess soa(Thread::Current());
  // Conservative defaults.
  field_offset = -1;
  is_volatile = true;
  // Try to resolve field and ignore if an Incompatible Class Change Error (ie is static).
  mirror::Field* resolved_field = ComputeFieldReferencedFromCompilingMethod(soa, mUnit, field_idx);
  if (resolved_field != NULL && !resolved_field->IsStatic()) {
    mirror::Class* referrer_class = ComputeCompilingMethodsClass(soa, mUnit);
    if (referrer_class != NULL) {
      mirror::Class* fields_class = resolved_field->GetDeclaringClass();
      bool access_ok = referrer_class->CanAccess(fields_class) &&
                       referrer_class->CanAccessMember(fields_class,
                                                       resolved_field->GetAccessFlags());
      if (!access_ok) {
        // The referring class can't access the resolved field, this may occur as a result of a
        // protected field being made public by a sub-class. Resort to the dex file to determine
        // the correct class for the access check.
        const DexFile& dex_file = *referrer_class->GetDexCache()->GetDexFile();
        mirror::Class* dex_fields_class = mUnit->GetClassLinker()->ResolveType(dex_file,
                                                         dex_file.GetFieldId(field_idx).class_idx_,
                                                         referrer_class);
        access_ok = referrer_class->CanAccess(dex_fields_class) &&
                    referrer_class->CanAccessMember(dex_fields_class,
                                                    resolved_field->GetAccessFlags());
      }
      bool is_write_to_final_from_wrong_class = is_put && resolved_field->IsFinal() &&
          fields_class != referrer_class;
      if (access_ok && !is_write_to_final_from_wrong_class) {
        field_offset = resolved_field->GetOffset().Int32Value();
        is_volatile = resolved_field->IsVolatile();
        stats_->ResolvedInstanceField();
        return true;  // Fast path.
      }
    }
  }
  // Clean up any exception left by field/type resolution
  if (soa.Self()->IsExceptionPending()) {
    soa.Self()->ClearException();
  }
  stats_->UnresolvedInstanceField();
  return false;  // Incomplete knowledge needs slow path.
}

bool CompilerDriver::ComputeStaticFieldInfo(uint32_t field_idx, const DexCompilationUnit* mUnit,
                                            int& field_offset, int& ssb_index,
                                            bool& is_referrers_class, bool& is_volatile,
                                            bool is_put) {
  ScopedObjectAccess soa(Thread::Current());
  // Conservative defaults.
  field_offset = -1;
  ssb_index = -1;
  is_referrers_class = false;
  is_volatile = true;
  // Try to resolve field and ignore if an Incompatible Class Change Error (ie isn't static).
  mirror::Field* resolved_field = ComputeFieldReferencedFromCompilingMethod(soa, mUnit, field_idx);
  if (resolved_field != NULL && resolved_field->IsStatic()) {
    mirror::Class* referrer_class = ComputeCompilingMethodsClass(soa, mUnit);
    if (referrer_class != NULL) {
      mirror::Class* fields_class = resolved_field->GetDeclaringClass();
      if (fields_class == referrer_class) {
        is_referrers_class = true;  // implies no worrying about class initialization
        field_offset = resolved_field->GetOffset().Int32Value();
        is_volatile = resolved_field->IsVolatile();
        stats_->ResolvedLocalStaticField();
        return true;  // fast path
      } else {
        bool access_ok = referrer_class->CanAccess(fields_class) &&
                         referrer_class->CanAccessMember(fields_class,
                                                         resolved_field->GetAccessFlags());
        if (!access_ok) {
          // The referring class can't access the resolved field, this may occur as a result of a
          // protected field being made public by a sub-class. Resort to the dex file to determine
          // the correct class for the access check. Don't change the field's class as that is
          // used to identify the SSB.
          const DexFile& dex_file = *referrer_class->GetDexCache()->GetDexFile();
          mirror::Class* dex_fields_class =
              mUnit->GetClassLinker()->ResolveType(dex_file,
                                                   dex_file.GetFieldId(field_idx).class_idx_,
                                                   referrer_class);
          access_ok = referrer_class->CanAccess(dex_fields_class) &&
                      referrer_class->CanAccessMember(dex_fields_class,
                                                      resolved_field->GetAccessFlags());
        }
        bool is_write_to_final_from_wrong_class = is_put && resolved_field->IsFinal();
        if (access_ok && !is_write_to_final_from_wrong_class) {
          // We have the resolved field, we must make it into a ssbIndex for the referrer
          // in its static storage base (which may fail if it doesn't have a slot for it)
          // TODO: for images we can elide the static storage base null check
          // if we know there's a non-null entry in the image
          mirror::DexCache* dex_cache = mUnit->GetClassLinker()->FindDexCache(*mUnit->GetDexFile());
          if (fields_class->GetDexCache() == dex_cache) {
            // common case where the dex cache of both the referrer and the field are the same,
            // no need to search the dex file
            ssb_index = fields_class->GetDexTypeIndex();
            field_offset = resolved_field->GetOffset().Int32Value();
            is_volatile = resolved_field->IsVolatile();
            stats_->ResolvedStaticField();
            return true;
          }
          // Search dex file for localized ssb index, may fail if field's class is a parent
          // of the class mentioned in the dex file and there is no dex cache entry.
          std::string descriptor(FieldHelper(resolved_field).GetDeclaringClassDescriptor());
          const DexFile::StringId* string_id =
          mUnit->GetDexFile()->FindStringId(descriptor);
          if (string_id != NULL) {
            const DexFile::TypeId* type_id =
               mUnit->GetDexFile()->FindTypeId(mUnit->GetDexFile()->GetIndexForStringId(*string_id));
            if (type_id != NULL) {
              // medium path, needs check of static storage base being initialized
              ssb_index = mUnit->GetDexFile()->GetIndexForTypeId(*type_id);
              field_offset = resolved_field->GetOffset().Int32Value();
              is_volatile = resolved_field->IsVolatile();
              stats_->ResolvedStaticField();
              return true;
            }
          }
        }
      }
    }
  }
  // Clean up any exception left by field/type resolution
  if (soa.Self()->IsExceptionPending()) {
    soa.Self()->ClearException();
  }
  stats_->UnresolvedStaticField();
  return false;  // Incomplete knowledge needs slow path.
}

void CompilerDriver::GetCodeAndMethodForDirectCall(InvokeType type, InvokeType sharp_type,
                                                   mirror::Class* referrer_class,
                                                   mirror::AbstractMethod* method,
                                                   uintptr_t& direct_code,
                                                   uintptr_t& direct_method) {
  // For direct and static methods compute possible direct_code and direct_method values, ie
  // an address for the Method* being invoked and an address of the code for that Method*.
  // For interface calls compute a value for direct_method that is the interface method being
  // invoked, so this can be passed to the out-of-line runtime support code.
  direct_code = 0;
  direct_method = 0;
  if (compiler_backend_ == kPortable) {
    if (sharp_type != kStatic && sharp_type != kDirect) {
      return;
    }
  } else {
    if (sharp_type != kStatic && sharp_type != kDirect && sharp_type != kInterface) {
      return;
    }
  }
  bool method_code_in_boot = method->GetDeclaringClass()->GetClassLoader() == NULL;
  if (!method_code_in_boot) {
    return;
  }
  bool has_clinit_trampoline = method->IsStatic() && !method->GetDeclaringClass()->IsInitialized();
  if (has_clinit_trampoline && (method->GetDeclaringClass() != referrer_class)) {
    // Ensure we run the clinit trampoline unless we are invoking a static method in the same class.
    return;
  }
  if (sharp_type != kInterface) {  // Interfaces always go via a trampoline.
    stats_->DirectCallsToBoot(type);
  }
  stats_->DirectMethodsToBoot(type);
  bool compiling_boot = Runtime::Current()->GetHeap()->GetSpaces().size() == 1;
  if (compiling_boot) {
    const bool kSupportBootImageFixup = true;
    if (kSupportBootImageFixup) {
      MethodHelper mh(method);
      if (IsImageClass(mh.GetDeclaringClassDescriptor())) {
        // We can only branch directly to Methods that are resolved in the DexCache.
        // Otherwise we won't invoke the resolution trampoline.
        direct_method = -1;
        direct_code = -1;
      }
    }
  } else {
    if (Runtime::Current()->GetHeap()->FindSpaceFromObject(method)->IsImageSpace()) {
      direct_method = reinterpret_cast<uintptr_t>(method);
    }
    direct_code = reinterpret_cast<uintptr_t>(method->GetCode());
  }
}

bool CompilerDriver::ComputeInvokeInfo(uint32_t method_idx,const uint32_t dex_pc,
                                       const DexCompilationUnit* mUnit, InvokeType& type,
                                       int& vtable_idx, uintptr_t& direct_code,
                                       uintptr_t& direct_method) {
  ScopedObjectAccess soa(Thread::Current());

  const bool kEnableVerifierBasedSharpening = true;
  const CompilerDriver::MethodReference ref_caller(mUnit->GetDexFile(), mUnit->GetDexMethodIndex());
  const CompilerDriver::MethodReference* ref_sharpen = verifier::MethodVerifier::GetDevirtMap(ref_caller, dex_pc);
  bool can_devirtualize = (dex_pc != art::kDexPCNotReady) && (ref_sharpen != NULL);
  vtable_idx = -1;
  direct_code = 0;
  direct_method = 0;
  mirror::AbstractMethod* resolved_method =
      ComputeMethodReferencedFromCompilingMethod(soa, mUnit, method_idx, type);
  if (resolved_method != NULL) {
    // Don't try to fast-path if we don't understand the caller's class or this appears to be an
    // Incompatible Class Change Error.
    mirror::Class* referrer_class = ComputeCompilingMethodsClass(soa, mUnit);
    bool icce = resolved_method->CheckIncompatibleClassChange(type);
    if (referrer_class != NULL && !icce) {
      mirror::Class* methods_class = resolved_method->GetDeclaringClass();
      if (!referrer_class->CanAccess(methods_class) ||
          !referrer_class->CanAccessMember(methods_class,
                                           resolved_method->GetAccessFlags())) {
        // The referring class can't access the resolved method, this may occur as a result of a
        // protected method being made public by implementing an interface that re-declares the
        // method public. Resort to the dex file to determine the correct class for the access
        // check.
        const DexFile& dex_file = *referrer_class->GetDexCache()->GetDexFile();
        methods_class =
            mUnit->GetClassLinker()->ResolveType(dex_file,
                                                 dex_file.GetMethodId(method_idx).class_idx_,
                                                 referrer_class);
      }
      if (referrer_class->CanAccess(methods_class) &&
          referrer_class->CanAccessMember(methods_class, resolved_method->GetAccessFlags())) {
        vtable_idx = resolved_method->GetMethodIndex();
        const bool kEnableSharpening = true;
        // Sharpen a virtual call into a direct call when the target is known.
        bool can_sharpen = type == kVirtual && (resolved_method->IsFinal() ||
            methods_class->IsFinal());
        // Ensure the vtable index will be correct to dispatch in the vtable of the super class.
        can_sharpen = can_sharpen || (type == kSuper && referrer_class != methods_class &&
            referrer_class->IsSubClass(methods_class) &&
            vtable_idx < methods_class->GetVTable()->GetLength() &&
            methods_class->GetVTable()->Get(vtable_idx) == resolved_method);

        if (kEnableSharpening && can_sharpen) {
          stats_->ResolvedMethod(type);
          // Sharpen a virtual call into a direct call. The method_idx is into referrer's
          // dex cache, check that this resolved method is where we expect it.
          CHECK(referrer_class->GetDexCache()->GetResolvedMethod(method_idx) == resolved_method)
              << PrettyMethod(resolved_method);
          stats_->VirtualMadeDirect(type);
          GetCodeAndMethodForDirectCall(type, kDirect, referrer_class, resolved_method,
                                        direct_code, direct_method);
          type = kDirect;
          return true;
        } else if(can_devirtualize && kEnableSharpening && kEnableVerifierBasedSharpening) {
            // If traditional sharpening fails, try the sharpening based on type information (Devirtualization)
            mirror::DexCache* dex_cache = mUnit->GetClassLinker()->FindDexCache(*ref_sharpen->first);
            mirror::ClassLoader* class_loader = soa.Decode<mirror::ClassLoader*>(mUnit->GetClassLoader());
            mirror::AbstractMethod* concrete_method = mUnit->GetClassLinker()->ResolveMethod(
                *ref_sharpen->first, ref_sharpen->second, dex_cache, class_loader, NULL, kVirtual);
            CHECK(concrete_method != NULL);
            CHECK(!concrete_method->IsAbstract());
            // TODO: fix breakage in image patching to be able to devirtualize cases with different
            // resolved and concrete methods.
            if(resolved_method == concrete_method) {
              GetCodeAndMethodForDirectCall(type, kDirect, referrer_class, concrete_method, direct_code, direct_method);
              stats_->VirtualMadeDirect(type);
              type = kDirect;
              stats_->PreciseTypeDevirtualization();
            }
            stats_->ResolvedMethod(type);
            return true;
        }
        else if (type == kSuper) {
          // Unsharpened super calls are suspicious so go slow-path.
        } else {
          stats_->ResolvedMethod(type);
          GetCodeAndMethodForDirectCall(type, type, referrer_class, resolved_method,
                                        direct_code, direct_method);
          return true;
        }
      }
    }
  }
  // Clean up any exception left by method/type resolution
  if (soa.Self()->IsExceptionPending()) {
      soa.Self()->ClearException();
  }
  stats_->UnresolvedMethod(type);
  return false;  // Incomplete knowledge needs slow path.
}

void CompilerDriver::AddCodePatch(const DexFile* dex_file,
                            uint32_t referrer_method_idx,
                            InvokeType referrer_invoke_type,
                            uint32_t target_method_idx,
                            InvokeType target_invoke_type,
                            size_t literal_offset) {
  MutexLock mu(Thread::Current(), compiled_methods_lock_);
  code_to_patch_.push_back(new PatchInformation(dex_file,
                                                referrer_method_idx,
                                                referrer_invoke_type,
                                                target_method_idx,
                                                target_invoke_type,
                                                literal_offset));
}
void CompilerDriver::AddMethodPatch(const DexFile* dex_file,
                              uint32_t referrer_method_idx,
                              InvokeType referrer_invoke_type,
                              uint32_t target_method_idx,
                              InvokeType target_invoke_type,
                              size_t literal_offset) {
  MutexLock mu(Thread::Current(), compiled_methods_lock_);
  methods_to_patch_.push_back(new PatchInformation(dex_file,
                                                   referrer_method_idx,
                                                   referrer_invoke_type,
                                                   target_method_idx,
                                                   target_invoke_type,
                                                   literal_offset));
}

class ParallelCompilationManager {
 public:
  typedef void Callback(const ParallelCompilationManager* manager, size_t index);

  ParallelCompilationManager(ClassLinker* class_linker,
                             jobject class_loader,
                             CompilerDriver* compiler,
                             const DexFile* dex_file,
                             ThreadPool& thread_pool)
    : class_linker_(class_linker),
      class_loader_(class_loader),
      compiler_(compiler),
      dex_file_(dex_file),
      thread_pool_(&thread_pool) {}

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

    std::vector<ForAllClosure*> closures(work_units);
    for (size_t i = 0; i < work_units; ++i) {
      closures[i] = new ForAllClosure(this, begin + i, end, callback, work_units);
      thread_pool_->AddTask(self, closures[i]);
    }
    thread_pool_->StartWorkers(self);

    // Ensure we're suspended while we're blocked waiting for the other threads to finish (worker
    // thread destructor's called below perform join).
    CHECK_NE(self->GetState(), kRunnable);

    // Wait for all the worker threads to finish.
    thread_pool_->Wait(self);
  }

 private:

  class ForAllClosure : public Task {
   public:
    ForAllClosure(ParallelCompilationManager* manager, size_t begin, size_t end, Callback* callback,
                  size_t stripe)
        : manager_(manager),
          begin_(begin),
          end_(end),
          callback_(callback),
          stripe_(stripe)
    {

    }

    virtual void Run(Thread* self) {
      for (size_t i = begin_; i < end_; i += stripe_) {
        callback_(manager_, i);
        self->AssertNoPendingException();
      }
    }

    virtual void Finalize() {
      delete this;
    }
   private:
    const ParallelCompilationManager* const manager_;
    const size_t begin_;
    const size_t end_;
    const Callback* const callback_;
    const size_t stripe_;
  };

  ClassLinker* const class_linker_;
  const jobject class_loader_;
  CompilerDriver* const compiler_;
  const DexFile* const dex_file_;
  ThreadPool* const thread_pool_;
};

// Return true if the class should be skipped during compilation. We
// never skip classes in the boot class loader. However, if we have a
// non-boot class loader and we can resolve the class in the boot
// class loader, we do skip the class. This happens if an app bundles
// classes found in the boot classpath. Since at runtime we will
// select the class from the boot classpath, do not attempt to resolve
// or compile it now.
static bool SkipClass(mirror::ClassLoader* class_loader,
                      const DexFile& dex_file,
                      const DexFile::ClassDef& class_def)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (class_loader == NULL) {
    return false;
  }
  const char* descriptor = dex_file.GetClassDescriptor(class_def);
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  mirror::Class* klass = class_linker->FindClass(descriptor, NULL);
  if (klass == NULL) {
    Thread* self = Thread::Current();
    CHECK(self->IsExceptionPending());
    self->ClearException();
    return false;
  }
  return true;
}

static void ResolveClassFieldsAndMethods(const ParallelCompilationManager* manager, size_t class_def_index)
    LOCKS_EXCLUDED(Locks::mutator_lock_) {
  ScopedObjectAccess soa(Thread::Current());
  mirror::ClassLoader* class_loader = soa.Decode<mirror::ClassLoader*>(manager->GetClassLoader());
  const DexFile& dex_file = *manager->GetDexFile();

  // Method and Field are the worst. We can't resolve without either
  // context from the code use (to disambiguate virtual vs direct
  // method and instance vs static field) or from class
  // definitions. While the compiler will resolve what it can as it
  // needs it, here we try to resolve fields and methods used in class
  // definitions, since many of them many never be referenced by
  // generated code.
  const DexFile::ClassDef& class_def = dex_file.GetClassDef(class_def_index);
  if (SkipClass(class_loader, dex_file, class_def)) {
    return;
  }

  // Note the class_data pointer advances through the headers,
  // static fields, instance fields, direct methods, and virtual
  // methods.
  const byte* class_data = dex_file.GetClassData(class_def);
  if (class_data == NULL) {
    // empty class such as a marker interface
    return;
  }
  Thread* self = Thread::Current();
  ClassLinker* class_linker = manager->GetClassLinker();
  mirror::DexCache* dex_cache = class_linker->FindDexCache(dex_file);
  ClassDataItemIterator it(dex_file, class_data);
  while (it.HasNextStaticField()) {
    mirror::Field* field = class_linker->ResolveField(dex_file, it.GetMemberIndex(), dex_cache,
                                                      class_loader, true);
    if (field == NULL) {
      CHECK(self->IsExceptionPending());
      self->ClearException();
    }
    it.Next();
  }
  // If an instance field is final then we need to have a barrier on the return, static final
  // fields are assigned within the lock held for class initialization.
  bool requires_constructor_barrier = false;
  while (it.HasNextInstanceField()) {
    if ((it.GetMemberAccessFlags() & kAccFinal) != 0) {
      requires_constructor_barrier = true;
    }

    mirror::Field* field = class_linker->ResolveField(dex_file, it.GetMemberIndex(), dex_cache,
                                                      class_loader, false);
    if (field == NULL) {
      CHECK(self->IsExceptionPending());
      self->ClearException();
    }
    it.Next();
  }
  if (requires_constructor_barrier) {
    manager->GetCompiler()->AddRequiresConstructorBarrier(soa.Self(), manager->GetDexFile(),
                                                          class_def_index);
  }
  while (it.HasNextDirectMethod()) {
    mirror::AbstractMethod* method = class_linker->ResolveMethod(dex_file, it.GetMemberIndex(),
                                                                 dex_cache, class_loader, NULL,
                                                                 it.GetMethodInvokeType(class_def));
    if (method == NULL) {
      CHECK(self->IsExceptionPending());
      self->ClearException();
    }
    it.Next();
  }
  while (it.HasNextVirtualMethod()) {
    mirror::AbstractMethod* method = class_linker->ResolveMethod(dex_file, it.GetMemberIndex(),
                                                                 dex_cache, class_loader, NULL,
                                                                 it.GetMethodInvokeType(class_def));
    if (method == NULL) {
      CHECK(self->IsExceptionPending());
      self->ClearException();
    }
    it.Next();
  }
  DCHECK(!it.HasNext());
}

static void ResolveType(const ParallelCompilationManager* manager, size_t type_idx)
    LOCKS_EXCLUDED(Locks::mutator_lock_) {
  // Class derived values are more complicated, they require the linker and loader.
  ScopedObjectAccess soa(Thread::Current());
  ClassLinker* class_linker = manager->GetClassLinker();
  const DexFile& dex_file = *manager->GetDexFile();
  mirror::DexCache* dex_cache = class_linker->FindDexCache(dex_file);
  mirror::ClassLoader* class_loader = soa.Decode<mirror::ClassLoader*>(manager->GetClassLoader());
  mirror::Class* klass = class_linker->ResolveType(dex_file, type_idx, dex_cache, class_loader);

  if (klass == NULL) {
    CHECK(soa.Self()->IsExceptionPending());
    Thread::Current()->ClearException();
  }
}

void CompilerDriver::ResolveDexFile(jobject class_loader, const DexFile& dex_file,
                                    ThreadPool& thread_pool, TimingLogger& timings) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();

  // TODO: we could resolve strings here, although the string table is largely filled with class
  //       and method names.

  ParallelCompilationManager context(class_linker, class_loader, this, &dex_file, thread_pool);
  context.ForAll(0, dex_file.NumTypeIds(), ResolveType, thread_count_);
  timings.AddSplit("Resolve " + dex_file.GetLocation() + " Types");

  context.ForAll(0, dex_file.NumClassDefs(), ResolveClassFieldsAndMethods, thread_count_);
  timings.AddSplit("Resolve " + dex_file.GetLocation() + " MethodsAndFields");
}

void CompilerDriver::Verify(jobject class_loader, const std::vector<const DexFile*>& dex_files,
                            ThreadPool& thread_pool, TimingLogger& timings) {
  for (size_t i = 0; i != dex_files.size(); ++i) {
    const DexFile* dex_file = dex_files[i];
    CHECK(dex_file != NULL);
    VerifyDexFile(class_loader, *dex_file, thread_pool, timings);
  }
}

static void VerifyClass(const ParallelCompilationManager* manager, size_t class_def_index)
    LOCKS_EXCLUDED(Locks::mutator_lock_) {
  ScopedObjectAccess soa(Thread::Current());
  const DexFile::ClassDef& class_def = manager->GetDexFile()->GetClassDef(class_def_index);
  const char* descriptor = manager->GetDexFile()->GetClassDescriptor(class_def);
  mirror::Class* klass =
      manager->GetClassLinker()->FindClass(descriptor,
                                           soa.Decode<mirror::ClassLoader*>(manager->GetClassLoader()));
  if (klass == NULL) {
    CHECK(soa.Self()->IsExceptionPending());
    soa.Self()->ClearException();

    /*
     * At compile time, we can still structurally verify the class even if FindClass fails.
     * This is to ensure the class is structurally sound for compilation. An unsound class
     * will be rejected by the verifier and later skipped during compilation in the compiler.
     */
    mirror::DexCache* dex_cache =  manager->GetClassLinker()->FindDexCache(*manager->GetDexFile());
    std::string error_msg;
    if (verifier::MethodVerifier::VerifyClass(manager->GetDexFile(),
                                              dex_cache,
                                              soa.Decode<mirror::ClassLoader*>(manager->GetClassLoader()),
                                              class_def_index, error_msg, true) ==
                                                  verifier::MethodVerifier::kHardFailure) {
      const DexFile::ClassDef& class_def = manager->GetDexFile()->GetClassDef(class_def_index);
      LOG(ERROR) << "Verification failed on class "
                 << PrettyDescriptor(manager->GetDexFile()->GetClassDescriptor(class_def))
                 << " because: " << error_msg;
    }
    return;
  }
  CHECK(klass->IsResolved()) << PrettyClass(klass);
  manager->GetClassLinker()->VerifyClass(klass);

  if (klass->IsErroneous()) {
    // ClassLinker::VerifyClass throws, which isn't useful in the compiler.
    CHECK(soa.Self()->IsExceptionPending());
    soa.Self()->ClearException();
  }

  CHECK(klass->IsCompileTimeVerified() || klass->IsErroneous())
      << PrettyDescriptor(klass) << ": state=" << klass->GetStatus();
  soa.Self()->AssertNoPendingException();
}

void CompilerDriver::VerifyDexFile(jobject class_loader, const DexFile& dex_file,
                                   ThreadPool& thread_pool, TimingLogger& timings) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  ParallelCompilationManager context(class_linker, class_loader, this, &dex_file, thread_pool);
  context.ForAll(0, dex_file.NumClassDefs(), VerifyClass, thread_count_);
  timings.AddSplit("Verify " + dex_file.GetLocation());
}

static const char* class_initializer_black_list[] = {
  "Landroid/app/ActivityThread;", // Calls regex.Pattern.compile -..-> regex.Pattern.compileImpl.
  "Landroid/bluetooth/BluetoothAudioGateway;", // Calls android.bluetooth.BluetoothAudioGateway.classInitNative().
  "Landroid/bluetooth/HeadsetBase;", // Calls android.bluetooth.HeadsetBase.classInitNative().
  "Landroid/content/res/CompatibilityInfo;", // Requires android.util.DisplayMetrics -..-> android.os.SystemProperties.native_get_int.
  "Landroid/content/res/CompatibilityInfo$1;", // Requires android.util.DisplayMetrics -..-> android.os.SystemProperties.native_get_int.
  "Landroid/content/UriMatcher;", // Calls regex.Pattern.compile -..-> regex.Pattern.compileImpl.
  "Landroid/database/CursorWindow;", // Requires android.util.DisplayMetrics -..-> android.os.SystemProperties.native_get_int.
  "Landroid/database/sqlite/SQLiteConnection;", // Calls regex.Pattern.compile -..-> regex.Pattern.compileImpl.
  "Landroid/database/sqlite/SQLiteConnection$Operation;", // Requires SimpleDateFormat -> java.util.Locale.
  "Landroid/database/sqlite/SQLiteDatabaseConfiguration;", // Calls regex.Pattern.compile -..-> regex.Pattern.compileImpl.
  "Landroid/database/sqlite/SQLiteDebug;", // Calls android.util.Log.isLoggable.
  "Landroid/database/sqlite/SQLiteOpenHelper;", // Calls Class.getSimpleName -> Class.isAnonymousClass -> Class.getDex.
  "Landroid/database/sqlite/SQLiteQueryBuilder;", // Calls regex.Pattern.compile -..-> regex.Pattern.compileImpl.
  "Landroid/drm/DrmManagerClient;", // Calls System.loadLibrary.
  "Landroid/graphics/drawable/AnimatedRotateDrawable;", // Sub-class of Drawable.
  "Landroid/graphics/drawable/AnimationDrawable;", // Sub-class of Drawable.
  "Landroid/graphics/drawable/BitmapDrawable;", // Sub-class of Drawable.
  "Landroid/graphics/drawable/ClipDrawable;", // Sub-class of Drawable.
  "Landroid/graphics/drawable/ColorDrawable;", // Sub-class of Drawable.
  "Landroid/graphics/drawable/Drawable;", // Requires android.graphics.Rect.
  "Landroid/graphics/drawable/DrawableContainer;", // Sub-class of Drawable.
  "Landroid/graphics/drawable/GradientDrawable;", // Sub-class of Drawable.
  "Landroid/graphics/drawable/LayerDrawable;", // Sub-class of Drawable.
  "Landroid/graphics/drawable/NinePatchDrawable;", // Sub-class of Drawable.
  "Landroid/graphics/drawable/RotateDrawable;", // Sub-class of Drawable.
  "Landroid/graphics/drawable/ScaleDrawable;", // Sub-class of Drawable.
  "Landroid/graphics/drawable/ShapeDrawable;", // Sub-class of Drawable.
  "Landroid/graphics/drawable/StateListDrawable;", // Sub-class of Drawable.
  "Landroid/graphics/drawable/TransitionDrawable;", // Sub-class of Drawable.
  "Landroid/graphics/Matrix;", // Calls android.graphics.Matrix.native_create.
  "Landroid/graphics/Matrix$1;", // Requires Matrix.
  "Landroid/graphics/PixelFormat;", // Calls android.graphics.PixelFormat.nativeClassInit().
  "Landroid/graphics/Rect;", // Calls regex.Pattern.compile -..-> regex.Pattern.compileImpl.
  "Landroid/graphics/SurfaceTexture;", // Calls android.graphics.SurfaceTexture.nativeClassInit().
  "Landroid/graphics/Typeface;", // Calls android.graphics.Typeface.nativeCreate.
  "Landroid/inputmethodservice/ExtractEditText;", // Requires android.widget.TextView.
  "Landroid/media/AmrInputStream;", // Calls OsConstants.initConstants.
  "Landroid/media/CamcorderProfile;", // Calls OsConstants.initConstants.
  "Landroid/media/CameraProfile;", // Calls System.loadLibrary.
  "Landroid/media/DecoderCapabilities;", // Calls System.loadLibrary.
  "Landroid/media/EncoderCapabilities;", // Calls OsConstants.initConstants.
  "Landroid/media/ExifInterface;", // Calls OsConstants.initConstants.
  "Landroid/media/MediaCodec;", // Calls OsConstants.initConstants.
  "Landroid/media/MediaCodecList;", // Calls OsConstants.initConstants.
  "Landroid/media/MediaCrypto;", // Calls OsConstants.initConstants.
  "Landroid/media/MediaDrm;", // Calls OsConstants.initConstants.
  "Landroid/media/MediaExtractor;", // Calls OsConstants.initConstants.
  "Landroid/media/MediaFile;", // Requires DecoderCapabilities.
  "Landroid/media/MediaMetadataRetriever;", // Calls OsConstants.initConstants.
  "Landroid/media/MediaMuxer;", // Calls OsConstants.initConstants.
  "Landroid/media/MediaPlayer;", // Calls System.loadLibrary.
  "Landroid/media/MediaRecorder;", // Calls System.loadLibrary.
  "Landroid/media/MediaScanner;", // Calls System.loadLibrary.
  "Landroid/media/ResampleInputStream;", // Calls OsConstants.initConstants.
  "Landroid/media/SoundPool;", // Calls OsConstants.initConstants.
  "Landroid/media/videoeditor/MediaArtistNativeHelper;", // Calls OsConstants.initConstants.
  "Landroid/media/videoeditor/VideoEditorProfile;", // Calls OsConstants.initConstants.
  "Landroid/mtp/MtpDatabase;", // Calls OsConstants.initConstants.
  "Landroid/mtp/MtpDevice;", // Calls OsConstants.initConstants.
  "Landroid/mtp/MtpServer;", // Calls OsConstants.initConstants.
  "Landroid/net/NetworkInfo;", // Calls java.util.EnumMap.<init> -> java.lang.Enum.getSharedConstants -> System.identityHashCode.
  "Landroid/net/Proxy;", // Calls regex.Pattern.compile -..-> regex.Pattern.compileImpl.
  "Landroid/net/SSLCertificateSocketFactory;", // Requires javax.net.ssl.HttpsURLConnection.
  "Landroid/net/Uri;", // Calls Class.getSimpleName -> Class.isAnonymousClass -> Class.getDex.
  "Landroid/net/Uri$AbstractHierarchicalUri;", // Requires Uri.
  "Landroid/net/Uri$HierarchicalUri;", // Requires Uri.
  "Landroid/net/Uri$OpaqueUri;", // Requires Uri.
  "Landroid/net/Uri$StringUri;", // Requires Uri.
  "Landroid/net/WebAddress;", // Calls regex.Pattern.compile -..-> regex.Pattern.compileImpl.
  "Landroid/nfc/NdefRecord;", // Calls String.getBytes -> java.nio.charset.Charset.
  "Landroid/opengl/EGL14;", // Calls android.opengl.EGL14._nativeClassInit.
  "Landroid/opengl/GLES10;", // Calls android.opengl.GLES10._nativeClassInit.
  "Landroid/opengl/GLES10Ext;", // Calls android.opengl.GLES10Ext._nativeClassInit.
  "Landroid/opengl/GLES11;", // Requires GLES10.
  "Landroid/opengl/GLES11Ext;", // Calls android.opengl.GLES11Ext._nativeClassInit.
  "Landroid/opengl/GLES20;", // Calls android.opengl.GLES20._nativeClassInit.
  "Landroid/opengl/GLUtils;", // Calls android.opengl.GLUtils.nativeClassInit.
  "Landroid/os/Build;", // Calls -..-> android.os.SystemProperties.native_get.
  "Landroid/os/Build$VERSION;", // Requires Build.
  "Landroid/os/Debug;", // Requires android.os.Environment.
  "Landroid/os/Environment;", // Calls System.getenv.
  "Landroid/os/FileUtils;", // Calls regex.Pattern.compile -..-> regex.Pattern.compileImpl.
  "Landroid/os/StrictMode;", // Calls android.util.Log.isLoggable.
  "Landroid/os/StrictMode$VmPolicy;", // Requires StrictMode.
  "Landroid/os/Trace;", // Calls android.os.Trace.nativeGetEnabledTags.
  "Landroid/os/UEventObserver;", // Calls Class.getSimpleName -> Class.isAnonymousClass -> Class.getDex.
  "Landroid/provider/ContactsContract;", // Calls OsConstants.initConstants.
  "Landroid/provider/Settings$Global;", // Calls OsConstants.initConstants.
  "Landroid/provider/Settings$Secure;", // Requires android.net.Uri.
  "Landroid/provider/Settings$System;", // Requires android.net.Uri.
  "Landroid/renderscript/RenderScript;", // Calls System.loadLibrary.
  "Landroid/server/BluetoothService;", // Calls android.server.BluetoothService.classInitNative.
  "Landroid/server/BluetoothEventLoop;", // Calls android.server.BluetoothEventLoop.classInitNative.
  "Landroid/telephony/PhoneNumberUtils;", // Calls regex.Pattern.compile -..-> regex.Pattern.compileImpl.
  "Landroid/telephony/TelephonyManager;", // Calls OsConstants.initConstants.
  "Landroid/text/AutoText;", // Requires android.util.DisplayMetrics -..-> android.os.SystemProperties.native_get_int.
  "Landroid/text/Layout;", // Calls com.android.internal.util.ArrayUtils.emptyArray -> System.identityHashCode.
  "Landroid/text/BoringLayout;", // Requires Layout.
  "Landroid/text/DynamicLayout;", // Requires Layout.
  "Landroid/text/Html$HtmlParser;", // Calls -..-> String.toLowerCase -> java.util.Locale.
  "Landroid/text/StaticLayout;", // Requires Layout.
  "Landroid/text/TextUtils;", // Requires android.util.DisplayMetrics.
  "Landroid/util/DisplayMetrics;", // Calls SystemProperties.native_get_int.
  "Landroid/util/Patterns;", // Calls regex.Pattern.compile -..-> regex.Pattern.compileImpl.
  "Landroid/view/Choreographer;", // Calls SystemProperties.native_get_boolean.
  "Landroid/util/Patterns;", // Calls regex.Pattern.compile -..-> regex.Pattern.compileImpl.
  "Landroid/view/GLES20Canvas;", // Calls GLES20Canvas.nIsAvailable().
  "Landroid/view/GLES20RecordingCanvas;", // Requires android.view.GLES20Canvas.
  "Landroid/view/GestureDetector;", // Calls android.view.GLES20Canvas.nIsAvailable.
  "Landroid/view/HardwareRenderer$Gl20Renderer;", // Requires SystemProperties.native_get.
  "Landroid/view/HardwareRenderer$GlRenderer;", // Requires SystemProperties.native_get.
  "Landroid/view/InputEventConsistencyVerifier;", // Requires android.os.Build.
  "Landroid/view/Surface;", // Requires SystemProperties.native_get.
  "Landroid/view/SurfaceControl;", // Calls OsConstants.initConstants.
  "Landroid/view/animation/AlphaAnimation;", // Requires Animation.
  "Landroid/view/animation/Animation;", // Calls SystemProperties.native_get_boolean.
  "Landroid/view/animation/AnimationSet;", // Calls OsConstants.initConstants.
  "Landroid/view/textservice/SpellCheckerSubtype;", // Calls Class.getDex().
  "Landroid/webkit/JniUtil;", // Calls System.loadLibrary.
  "Landroid/webkit/PluginManager;", // // Calls OsConstants.initConstants.
  "Landroid/webkit/WebViewCore;", // Calls System.loadLibrary.
  "Landroid/webkit/WebViewInputDispatcher;", // Calls Calls regex.Pattern.compile -..-> regex.Pattern.compileImpl.
  "Landroid/webkit/URLUtil;", // Calls Calls regex.Pattern.compile -..-> regex.Pattern.compileImpl.
  "Landroid/widget/AutoCompleteTextView;", // Requires TextView.
  "Landroid/widget/Button;", // Requires TextView.
  "Landroid/widget/CheckBox;", // Requires TextView.
  "Landroid/widget/CheckedTextView;", // Requires TextView.
  "Landroid/widget/CompoundButton;", // Requires TextView.
  "Landroid/widget/EditText;", // Requires TextView.
  "Landroid/widget/NumberPicker;", // Requires java.util.Locale.
  "Landroid/widget/ScrollBarDrawable;", // Sub-class of Drawable.
  "Landroid/widget/SearchView$SearchAutoComplete;", // Requires TextView.
  "Landroid/widget/Switch;", // Requires TextView.
  "Landroid/widget/TextView;", // Calls Paint.<init> -> Paint.native_init.
  "Lcom/android/i18n/phonenumbers/AsYouTypeFormatter;", // Calls regex.Pattern.compile -..-> regex.Pattern.compileImpl.
  "Lcom/android/i18n/phonenumbers/PhoneNumberMatcher;", // Calls regex.Pattern.compile -..-> regex.Pattern.compileImpl.
  "Lcom/android/i18n/phonenumbers/PhoneNumberUtil;", // Requires java.util.logging.LogManager.
  "Lcom/android/internal/os/SamplingProfilerIntegration;", // Calls SystemProperties.native_get_int.
  "Lcom/android/internal/policy/impl/PhoneWindow;", // Calls android.os.Binder.init.
  "Lcom/android/internal/view/menu/ActionMenuItemView;", // Requires TextView.
  "Lcom/android/internal/widget/DialogTitle;", // Requires TextView.
  "Lcom/android/org/bouncycastle/asn1/StreamUtil;", // Calls Runtime.getRuntime().maxMemory().
  "Lcom/android/org/bouncycastle/crypto/digests/OpenSSLDigest$SHA1;", // Requires com.android.org.conscrypt.NativeCrypto.
  "Lcom/android/org/bouncycastle/crypto/engines/RSABlindedEngine;", // Calls native ... -> java.math.NativeBN.BN_new().
  "Lcom/android/org/bouncycastle/jce/provider/CertBlacklist;", // Calls System.getenv -> OsConstants.initConstants.
  "Lcom/android/org/bouncycastle/jce/provider/PKIXCertPathValidatorSpi;", // Calls System.getenv -> OsConstants.initConstants.
  "Lcom/android/org/conscrypt/NativeCrypto;", // Calls native NativeCrypto.clinit().
  "Lcom/android/org/conscrypt/OpenSSLECKeyPairGenerator;", // Calls OsConstants.initConstants.
  "Lcom/android/org/conscrypt/OpenSSLMac$HmacMD5;", // Calls native NativeCrypto.clinit().
  "Lcom/android/org/conscrypt/OpenSSLMac$HmacSHA1;", // Calls native NativeCrypto.clinit().
  "Lcom/android/org/conscrypt/OpenSSLMac$HmacSHA256;", // Calls native NativeCrypto.clinit().
  "Lcom/android/org/conscrypt/OpenSSLMac$HmacSHA384;", // Calls native NativeCrypto.clinit().
  "Lcom/android/org/conscrypt/OpenSSLMac$HmacSHA512;", // Calls native NativeCrypto.clinit().
  "Lcom/android/org/conscrypt/OpenSSLMessageDigestJDK$MD5;", // Requires com.android.org.conscrypt.NativeCrypto.
  "Lcom/android/org/conscrypt/OpenSSLMessageDigestJDK$SHA1;", // Requires com.android.org.conscrypt.NativeCrypto.
  "Lcom/android/org/conscrypt/OpenSSLMessageDigestJDK$SHA512;", // Requires com.android.org.conscrypt.NativeCrypto.
  "Lcom/android/org/conscrypt/OpenSSLX509CertPath;", // Calls OsConstants.initConstants.
  "Lcom/android/org/conscrypt/OpenSSLX509CertificateFactory;", // Calls OsConstants.initConstants.
  "Lcom/android/org/conscrypt/TrustedCertificateStore;", // Calls System.getenv -> OsConstants.initConstants.
  "Lcom/google/android/gles_jni/EGLContextImpl;", // Calls com.google.android.gles_jni.EGLImpl._nativeClassInit.
  "Lcom/google/android/gles_jni/EGLImpl;", // Calls com.google.android.gles_jni.EGLImpl._nativeClassInit.
  "Lcom/google/android/gles_jni/GLImpl;", // Calls com.google.android.gles_jni.GLImpl._nativeClassInit.
  "Ljava/io/Console;", // Has FileDescriptor(s).
  "Ljava/io/File;", // Calls to Random.<init> -> System.currentTimeMillis -> OsConstants.initConstants.
  "Ljava/io/FileDescriptor;", // Requires libcore.io.OsConstants.
  "Ljava/io/ObjectInputStream;", // Requires java.lang.ClassLoader$SystemClassLoader.
  "Ljava/io/ObjectStreamClass;",  // Calls to Class.forName -> java.io.FileDescriptor.
  "Ljava/io/ObjectStreamConstants;", // Instance of non-image class SerializablePermission.
  "Ljava/lang/ClassLoader$SystemClassLoader;", // Calls System.getProperty -> OsConstants.initConstants.
  "Ljava/lang/Runtime;", // Calls System.getProperty -> OsConstants.initConstants.
  "Ljava/lang/System;", // Calls OsConstants.initConstants.
  "Ljava/math/BigDecimal;", // Calls native ... -> java.math.NativeBN.BN_new().
  "Ljava/math/BigInteger;", // Calls native ... -> java.math.NativeBN.BN_new().
  "Ljava/math/Multiplication;", // Calls native ... -> java.math.NativeBN.BN_new().
  "Ljava/net/InetAddress;", // Requires libcore.io.OsConstants.
  "Ljava/net/Inet4Address;", // Sub-class of InetAddress.
  "Ljava/net/Inet6Address;", // Sub-class of InetAddress.
  "Ljava/net/InetUnixAddress;", // Sub-class of InetAddress.
  "Ljava/nio/charset/Charset;", // Calls Charset.getDefaultCharset -> System.getProperty -> OsConstants.initConstants.
  "Ljava/nio/charset/CharsetICU;", // Sub-class of Charset.
  "Ljava/nio/charset/Charsets;", // Calls Charset.forName.
  "Ljava/security/KeyPairGenerator;", // Calls OsConstants.initConstants.
  "Ljava/security/Security;", // Tries to do disk IO for "security.properties".
  "Ljava/sql/Date;", // Calls OsConstants.initConstants.
  "Ljava/util/Date;", // Calls Date.<init> -> System.currentTimeMillis -> OsConstants.initConstants.
  "Ljava/util/Locale;", // Calls System.getProperty -> OsConstants.initConstants.
  "Ljava/util/SimpleTimeZone;", // Sub-class of TimeZone.
  "Ljava/util/TimeZone;", // Calls regex.Pattern.compile -..-> regex.Pattern.compileImpl.
  "Ljava/util/concurrent/ConcurrentHashMap$Segment;", // Calls Runtime.getRuntime().availableProcessors().
  "Ljava/util/logging/LogManager;", // Calls System.getProperty -> OsConstants.initConstants.
  "Ljavax/microedition/khronos/egl/EGL10;", // Requires EGLContext.
  "Ljavax/microedition/khronos/egl/EGLContext;", // Requires com.google.android.gles_jni.EGLImpl.
  "Ljavax/net/ssl/HttpsURLConnection;", // Calls SSLSocketFactory.getDefault -> java.security.Security.getProperty.
  "Llibcore/icu/LocaleData;", // Requires java.util.Locale.
  "Llibcore/icu/TimeZoneNames;", // Requires java.util.TimeZone.
  "Llibcore/io/IoUtils;",  // Calls Random.<init> -> System.currentTimeMillis -> FileDescriptor -> OsConstants.initConstants.
  "Llibcore/io/OsConstants;", // Platform specific.
  "Llibcore/net/MimeUtils;", // Calls libcore.net.MimeUtils.getContentTypesPropertiesStream -> System.getProperty.
  "Llibcore/util/ZoneInfo;", // Sub-class of TimeZone.
  "Llibcore/util/ZoneInfoDB;", // Calls System.getenv -> OsConstants.initConstants.
  "Lorg/apache/commons/logging/LogFactory;", // Calls System.getProperty.
  "Lorg/apache/harmony/security/fortress/Services;", // Calls ClassLoader.getSystemClassLoader -> System.getProperty.
  "Lorg/apache/harmony/security/provider/cert/X509CertFactoryImpl;", // Requires java.nio.charsets.Charsets.
  "Lorg/apache/harmony/security/provider/crypto/RandomBitsSupplier;", // Requires java.io.File.
  "Lorg/apache/harmony/security/utils/AlgNameMapper;", // Requires java.util.Locale.
  "Lorg/apache/harmony/security/x501/AttributeTypeAndValue;", // Calls IntegralToString.convertInt -> Thread.currentThread.
  "Lorg/apache/harmony/security/x501/DirectoryString;", // Requires BigInteger.
  "Lorg/apache/harmony/security/x501/Name;", // Requires org.apache.harmony.security.x501.AttributeTypeAndValue.
  "Lorg/apache/harmony/security/x509/Certificate;", // Requires org.apache.harmony.security.x509.TBSCertificate.
  "Lorg/apache/harmony/security/x509/TBSCertificate;",  // Requires org.apache.harmony.security.x501.Name.
  "Lorg/apache/harmony/security/x509/EDIPartyName;", // Calls native ... -> java.math.NativeBN.BN_new().
  "Lorg/apache/harmony/security/x509/GeneralName;", // Requires org.apache.harmony.security.x501.Name.
  "Lorg/apache/harmony/security/x509/GeneralNames;", // Requires GeneralName.
  "Lorg/apache/harmony/security/x509/Time;", // Calls native ... -> java.math.NativeBN.BN_new().
  "Lorg/apache/harmony/security/x509/Validity;", // Requires x509.Time.
  "Lorg/apache/harmony/xml/ExpatParser;", // Calls native ExpatParser.staticInitialize.
  "Lorg/apache/http/conn/params/ConnRouteParams;", // Requires java.util.Locale.
  "Lorg/apache/http/conn/ssl/SSLSocketFactory;", // Calls java.security.Security.getProperty.
  "Lorg/apache/http/conn/util/InetAddressUtils;", // Calls regex.Pattern.compile -..-> regex.Pattern.compileImpl.
};

static void InitializeClass(const ParallelCompilationManager* manager, size_t class_def_index)
    LOCKS_EXCLUDED(Locks::mutator_lock_) {
  const DexFile::ClassDef& class_def = manager->GetDexFile()->GetClassDef(class_def_index);
  ScopedObjectAccess soa(Thread::Current());
  mirror::ClassLoader* class_loader = soa.Decode<mirror::ClassLoader*>(manager->GetClassLoader());
  const char* descriptor = manager->GetDexFile()->GetClassDescriptor(class_def);
  mirror::Class* klass = manager->GetClassLinker()->FindClass(descriptor, class_loader);
  bool compiling_boot = Runtime::Current()->GetHeap()->GetSpaces().size() == 1;
  bool can_init_static_fields = compiling_boot &&
      manager->GetCompiler()->IsImageClass(descriptor);
  if (klass != NULL) {
    // We don't want class initialization occurring on multiple threads due to deadlock problems.
    // For example, a parent class is initialized (holding its lock) that refers to a sub-class
    // in its static/class initializer causing it to try to acquire the sub-class' lock. While
    // on a second thread the sub-class is initialized (holding its lock) after first initializing
    // its parents, whose locks are acquired. This leads to a parent-to-child and a child-to-parent
    // lock ordering and consequent potential deadlock.
    static Mutex lock1("Initializer lock", kMonitorLock);
    MutexLock mu(soa.Self(), lock1);
    // The lock required to initialize the class.
    ObjectLock lock2(soa.Self(), klass);
    // Only try to initialize classes that were successfully verified.
    if (klass->IsVerified()) {
      manager->GetClassLinker()->EnsureInitialized(klass, false, can_init_static_fields);
      if (!klass->IsInitialized()) {
        if (can_init_static_fields) {
          bool is_black_listed = false;
          for (size_t i = 0; i < arraysize(class_initializer_black_list); ++i) {
            if (StringPiece(descriptor) == class_initializer_black_list[i]) {
              is_black_listed = true;
              break;
            }
          }
          if (!is_black_listed) {
            LOG(INFO) << "Initializing: " << descriptor;
            if (StringPiece(descriptor) == "Ljava/lang/Void;"){
              // Hand initialize j.l.Void to avoid Dex file operations in un-started runtime.
              mirror::ObjectArray<mirror::Field>* fields = klass->GetSFields();
              CHECK_EQ(fields->GetLength(), 1);
              fields->Get(0)->SetObj(klass, manager->GetClassLinker()->FindPrimitiveClass('V'));
              klass->SetStatus(mirror::Class::kStatusInitialized);
            } else {
              manager->GetClassLinker()->EnsureInitialized(klass, true, can_init_static_fields);
            }
            soa.Self()->AssertNoPendingException();
          }
        }
      }
      // If successfully initialized place in SSB array.
      if (klass->IsInitialized()) {
        klass->GetDexCache()->GetInitializedStaticStorage()->Set(klass->GetDexTypeIndex(), klass);
      }
    }
    // Record the final class status if necessary.
    mirror::Class::Status status = klass->GetStatus();
    CompilerDriver::ClassReference ref(manager->GetDexFile(), class_def_index);
    CompiledClass* compiled_class = manager->GetCompiler()->GetCompiledClass(ref);
    if (compiled_class == NULL) {
      compiled_class = new CompiledClass(status);
      manager->GetCompiler()->RecordClassStatus(ref, compiled_class);
    } else {
      DCHECK_EQ(status, compiled_class->GetStatus());
    }
  }
  // Clear any class not found or verification exceptions.
  soa.Self()->ClearException();
}

void CompilerDriver::InitializeClasses(jobject jni_class_loader, const DexFile& dex_file,
                                       ThreadPool& thread_pool, TimingLogger& timings) {
#ifndef NDEBUG
  for (size_t i = 0; i < arraysize(class_initializer_black_list); ++i) {
    const char* descriptor = class_initializer_black_list[i];
    CHECK(IsValidDescriptor(descriptor)) << descriptor;
  }
#endif
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  ParallelCompilationManager context(class_linker, jni_class_loader, this, &dex_file, thread_pool);
  context.ForAll(0, dex_file.NumClassDefs(), InitializeClass, thread_count_);
  timings.AddSplit("InitializeNoClinit " + dex_file.GetLocation());
}

void CompilerDriver::InitializeClasses(jobject class_loader,
                                       const std::vector<const DexFile*>& dex_files,
                                       ThreadPool& thread_pool, TimingLogger& timings) {
  for (size_t i = 0; i != dex_files.size(); ++i) {
    const DexFile* dex_file = dex_files[i];
    CHECK(dex_file != NULL);
    InitializeClasses(class_loader, *dex_file, thread_pool, timings);
  }
}

void CompilerDriver::Compile(jobject class_loader, const std::vector<const DexFile*>& dex_files,
                       ThreadPool& thread_pool, TimingLogger& timings) {
  for (size_t i = 0; i != dex_files.size(); ++i) {
    const DexFile* dex_file = dex_files[i];
    CHECK(dex_file != NULL);
    CompileDexFile(class_loader, *dex_file, thread_pool, timings);
  }
}

void CompilerDriver::CompileClass(const ParallelCompilationManager* manager, size_t class_def_index) {
  jobject class_loader = manager->GetClassLoader();
  const DexFile& dex_file = *manager->GetDexFile();
  const DexFile::ClassDef& class_def = dex_file.GetClassDef(class_def_index);
  {
    ScopedObjectAccess soa(Thread::Current());
    mirror::ClassLoader* class_loader = soa.Decode<mirror::ClassLoader*>(manager->GetClassLoader());
    if (SkipClass(class_loader, dex_file, class_def)) {
      return;
    }
  }
  ClassReference ref(&dex_file, class_def_index);
  // Skip compiling classes with generic verifier failures since they will still fail at runtime
  if (verifier::MethodVerifier::IsClassRejected(ref)) {
    return;
  }
  const byte* class_data = dex_file.GetClassData(class_def);
  if (class_data == NULL) {
    // empty class, probably a marker interface
    return;
  }
  ClassDataItemIterator it(dex_file, class_data);
  // Skip fields
  while (it.HasNextStaticField()) {
    it.Next();
  }
  while (it.HasNextInstanceField()) {
    it.Next();
  }
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
    manager->GetCompiler()->CompileMethod(it.GetMethodCodeItem(), it.GetMemberAccessFlags(),
                                          it.GetMethodInvokeType(class_def), class_def_index,
                                          method_idx, class_loader, dex_file);
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
    manager->GetCompiler()->CompileMethod(it.GetMethodCodeItem(), it.GetMemberAccessFlags(),
                                          it.GetMethodInvokeType(class_def), class_def_index,
                                          method_idx, class_loader, dex_file);
    it.Next();
  }
  DCHECK(!it.HasNext());
}

void CompilerDriver::CompileDexFile(jobject class_loader, const DexFile& dex_file,
                                    ThreadPool& thread_pool, TimingLogger& timings) {
  ParallelCompilationManager context(NULL, class_loader, this, &dex_file, thread_pool);
  context.ForAll(0, dex_file.NumClassDefs(), CompilerDriver::CompileClass, thread_count_);
  timings.AddSplit("Compile " + dex_file.GetLocation());
}

void CompilerDriver::CompileMethod(const DexFile::CodeItem* code_item, uint32_t access_flags,
                                   InvokeType invoke_type, uint32_t class_def_idx,
                                   uint32_t method_idx, jobject class_loader,
                                   const DexFile& dex_file) {
  CompiledMethod* compiled_method = NULL;
  uint64_t start_ns = NanoTime();

  if ((access_flags & kAccNative) != 0) {
    compiled_method = (*jni_compiler_)(*this, access_flags, method_idx, dex_file);
    CHECK(compiled_method != NULL);
  } else if ((access_flags & kAccAbstract) != 0) {
  } else {
    // In small mode we only compile image classes.
    bool dont_compile = Runtime::Current()->IsSmallMode() && ((image_classes_ == NULL) || (image_classes_->size() == 0));

    // Don't compile class initializers, ever.
    if (((access_flags & kAccConstructor) != 0) && ((access_flags & kAccStatic) != 0)) {
      dont_compile = true;
    } else if (code_item->insns_size_in_code_units_ < Runtime::Current()->GetSmallModeMethodDexSizeLimit()) {
    // Do compile small methods.
      dont_compile = false;
      LOG(INFO) << "Compiling a small method: " << PrettyMethod(method_idx, dex_file);
    }

    if (!dont_compile) {
      compiled_method = (*compiler_)(*this, code_item, access_flags, invoke_type, class_def_idx,
                                     method_idx, class_loader, dex_file);
      CHECK(compiled_method != NULL) << PrettyMethod(method_idx, dex_file);
    }
  }
  uint64_t duration_ns = NanoTime() - start_ns;
#ifdef ART_USE_PORTABLE_COMPILER
  const uint64_t kWarnMilliSeconds = 1000;
#else
  const uint64_t kWarnMilliSeconds = 100;
#endif
  if (duration_ns > MsToNs(kWarnMilliSeconds)) {
    LOG(WARNING) << "Compilation of " << PrettyMethod(method_idx, dex_file)
                 << " took " << PrettyDuration(duration_ns);
  }

  Thread* self = Thread::Current();
  if (compiled_method != NULL) {
    MethodReference ref(&dex_file, method_idx);
    CHECK(GetCompiledMethod(ref) == NULL) << PrettyMethod(method_idx, dex_file);
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

CompiledMethod* CompilerDriver::GetCompiledMethod(MethodReference ref) const {
  MutexLock mu(Thread::Current(), compiled_methods_lock_);
  MethodTable::const_iterator it = compiled_methods_.find(ref);
  if (it == compiled_methods_.end()) {
    return NULL;
  }
  CHECK(it->second != NULL);
  return it->second;
}

void CompilerDriver::SetBitcodeFileName(std::string const& filename) {
  typedef void (*SetBitcodeFileNameFn)(CompilerDriver&, std::string const&);

  SetBitcodeFileNameFn set_bitcode_file_name =
    FindFunction<SetBitcodeFileNameFn>(MakeCompilerSoName(compiler_backend_), compiler_library_,
                                       "compilerLLVMSetBitcodeFileName");

  set_bitcode_file_name(*this, filename);
}


void CompilerDriver::AddRequiresConstructorBarrier(Thread* self, const DexFile* dex_file,
                                             size_t class_def_index) {
  MutexLock mu(self, freezing_constructor_lock_);
  freezing_constructor_classes_.insert(ClassReference(dex_file, class_def_index));
}

bool CompilerDriver::RequiresConstructorBarrier(Thread* self, const DexFile* dex_file,
                                          size_t class_def_index) {
  MutexLock mu(self, freezing_constructor_lock_);
  return freezing_constructor_classes_.count(ClassReference(dex_file, class_def_index)) != 0;
}

bool CompilerDriver::WriteElf(const std::string& android_root,
                              bool is_host,
                              const std::vector<const DexFile*>& dex_files,
                              std::vector<uint8_t>& oat_contents,
                              File* file) {
  typedef bool (*WriteElfFn)(CompilerDriver&,
                             const std::string& android_root,
                             bool is_host,
                             const std::vector<const DexFile*>& dex_files,
                             std::vector<uint8_t>&,
                             File*);
  WriteElfFn WriteElf =
    FindFunction<WriteElfFn>(MakeCompilerSoName(compiler_backend_), compiler_library_, "WriteElf");
  Locks::mutator_lock_->AssertSharedHeld(Thread::Current());
  return WriteElf(*this, android_root, is_host, dex_files, oat_contents, file);
}

bool CompilerDriver::FixupElf(File* file, uintptr_t oat_data_begin) const {
  typedef bool (*FixupElfFn)(File*, uintptr_t oat_data_begin);
  FixupElfFn FixupElf =
    FindFunction<FixupElfFn>(MakeCompilerSoName(compiler_backend_), compiler_library_, "FixupElf");
  return FixupElf(file, oat_data_begin);
}

void CompilerDriver::GetOatElfInformation(File* file,
                                          size_t& oat_loaded_size,
                                          size_t& oat_data_offset) const {
  typedef bool (*GetOatElfInformationFn)(File*, size_t& oat_loaded_size, size_t& oat_data_offset);
  GetOatElfInformationFn GetOatElfInformation =
    FindFunction<GetOatElfInformationFn>(MakeCompilerSoName(compiler_backend_), compiler_library_,
                                         "GetOatElfInformation");
  GetOatElfInformation(file, oat_loaded_size, oat_data_offset);
}

bool CompilerDriver::StripElf(File* file) const {
  typedef bool (*StripElfFn)(File*);
  StripElfFn StripElf =
    FindFunction<StripElfFn>(MakeCompilerSoName(compiler_backend_), compiler_library_, "StripElf");
  return StripElf(file);
}

void CompilerDriver::InstructionSetToLLVMTarget(InstructionSet instruction_set,
                                                std::string& target_triple,
                                                std::string& target_cpu,
                                                std::string& target_attr) {
  switch (instruction_set) {
    case kThumb2:
      target_triple = "thumb-none-linux-gnueabi";
      target_cpu = "cortex-a9";
      target_attr = "+thumb2,+neon,+neonfp,+vfp3,+db";
      break;

    case kArm:
      target_triple = "armv7-none-linux-gnueabi";
      // TODO: Fix for Nexus S.
      target_cpu = "cortex-a9";
      // TODO: Fix for Xoom.
      target_attr = "+v7,+neon,+neonfp,+vfp3,+db";
      break;

    case kX86:
      target_triple = "i386-pc-linux-gnu";
      target_attr = "";
      break;

    case kMips:
      target_triple = "mipsel-unknown-linux";
      target_attr = "mips32r2";
      break;

    default:
      LOG(FATAL) << "Unknown instruction set: " << instruction_set;
    }
  }
}  // namespace art
