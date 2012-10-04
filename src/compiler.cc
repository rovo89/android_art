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

#include "compiler.h"

#include <vector>

#include <dlfcn.h>
#include <unistd.h>

#include "class_linker.h"
#include "class_loader.h"
#include "dex_cache.h"
#include "jni_internal.h"
#include "oat_compilation_unit.h"
#include "oat_file.h"
#include "oat/runtime/stub.h"
#include "object_utils.h"
#include "runtime.h"
#include "space.h"
#include "scoped_thread_state_change.h"
#include "ScopedLocalRef.h"
#include "stl_util.h"
#include "timing_logger.h"
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
        resolved_local_static_fields_(0), resolved_static_fields_(0), unresolved_static_fields_(0) {
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

// Allow lossy statistics in non-debug builds
#ifndef NDEBUG
#define STATS_LOCK() MutexLock mu(stats_lock_)
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

  size_t resolved_methods_[kMaxInvokeType + 1];
  size_t unresolved_methods_[kMaxInvokeType + 1];
  size_t virtual_made_direct_[kMaxInvokeType + 1];
  size_t direct_calls_to_boot_[kMaxInvokeType + 1];
  size_t direct_methods_to_boot_[kMaxInvokeType + 1];

  DISALLOW_COPY_AND_ASSIGN(AOTCompilationStats);
};

static std::string MakeCompilerSoName(InstructionSet instruction_set) {
  // TODO: is the ARM/Thumb2 instruction set distinction really buying us anything,
  // or just causing hassle like this?
  if (instruction_set == kThumb2) {
    instruction_set = kArm;
  }

  // Lower case the instruction set, because that's what we do in the build system.
  std::string instruction_set_name(ToStr<InstructionSet>(instruction_set).str());
  for (size_t i = 0; i < instruction_set_name.size(); ++i) {
    instruction_set_name[i] = tolower(instruction_set_name[i]);
  }

  // Bad things happen if we pull in the libartd-compiler to a libart dex2oat or vice versa,
  // because we end up with both libart and libartd in the same address space!
  const char* suffix = (kIsDebugBuild ? "d" : "");

  // Work out the filename for the compiler library.
#if defined(ART_USE_LLVM_COMPILER)
  std::string library_name(StringPrintf("art%s-compiler-llvm", suffix));
#elif defined(ART_USE_GREENLAND_COMPILER)
  std::string library_name(StringPrintf("art%s-compiler-greenland", suffix));
#else
  std::string library_name(StringPrintf("art%s-compiler-%s", suffix, instruction_set_name.c_str()));
#endif
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

Compiler::Compiler(InstructionSet instruction_set, bool image, size_t thread_count,
                   bool support_debugging, const std::set<std::string>* image_classes,
                   bool dump_stats, bool dump_timings)
    : instruction_set_(instruction_set),
      compiled_classes_lock_("compiled classes lock"),
      compiled_methods_lock_("compiled method lock"),
      compiled_invoke_stubs_lock_("compiled invoke stubs lock"),
#if defined(ART_USE_LLVM_COMPILER)
      compiled_proxy_stubs_lock_("compiled proxy stubs lock"),
#endif
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
      create_invoke_stub_(NULL)
{
  std::string compiler_so_name(MakeCompilerSoName(instruction_set_));
  compiler_library_ = dlopen(compiler_so_name.c_str(), RTLD_LAZY);
  if (compiler_library_ == NULL) {
    LOG(FATAL) << "Couldn't find compiler library " << compiler_so_name << ": " << dlerror();
  }
  VLOG(compiler) << "dlopen(\"" << compiler_so_name << "\", RTLD_LAZY) returned " << compiler_library_;

#if defined(ART_USE_LLVM_COMPILER) || defined(ART_USE_GREENLAND_COMPILER)
  // Initialize compiler_context_
  typedef void (*InitCompilerContextFn)(Compiler&);

  InitCompilerContextFn init_compiler_context =
    FindFunction<void (*)(Compiler&)>(compiler_so_name,
                                      compiler_library_,
                                      "ArtInitCompilerContext");

  init_compiler_context(*this);
#elif defined(ART_USE_QUICK_COMPILER)
  // Initialize compiler_context_
  typedef void (*InitCompilerContextFn)(Compiler&);

  InitCompilerContextFn init_compiler_context =
    FindFunction<void (*)(Compiler&)>(compiler_so_name,
                                      compiler_library_,
                                      "ArtInitQuickCompilerContext");

  init_compiler_context(*this);
#endif

  compiler_ = FindFunction<CompilerFn>(compiler_so_name, compiler_library_, "ArtCompileMethod");
  jni_compiler_ = FindFunction<JniCompilerFn>(compiler_so_name, compiler_library_, "ArtJniCompileMethod");
  create_invoke_stub_ = FindFunction<CreateInvokeStubFn>(compiler_so_name, compiler_library_, "ArtCreateInvokeStub");

#if defined(ART_USE_LLVM_COMPILER)
  create_proxy_stub_ = FindFunction<CreateProxyStubFn>(
      compiler_so_name, compiler_library_, "ArtCreateProxyStub");
#endif

  CHECK(!Runtime::Current()->IsStarted());
  if (!image_) {
    CHECK(image_classes_ == NULL);
  }
}

Compiler::~Compiler() {
  {
    MutexLock mu(compiled_classes_lock_);
    STLDeleteValues(&compiled_classes_);
  }
  {
    MutexLock mu(compiled_methods_lock_);
    STLDeleteValues(&compiled_methods_);
  }
  {
    MutexLock mu(compiled_invoke_stubs_lock_);
    STLDeleteValues(&compiled_invoke_stubs_);
  }
#if defined(ART_USE_LLVM_COMPILER)
  {
    MutexLock mu(compiled_proxy_stubs_lock_);
    STLDeleteValues(&compiled_proxy_stubs_);
  }
#endif
  {
    MutexLock mu(compiled_methods_lock_);
    STLDeleteElements(&code_to_patch_);
  }
  {
    MutexLock mu(compiled_methods_lock_);
    STLDeleteElements(&methods_to_patch_);
  }
#if defined(ART_USE_LLVM_COMPILER)
  // Uninitialize compiler_context_
  typedef void (*UninitCompilerContextFn)(Compiler&);

  std::string compiler_so_name(MakeCompilerSoName(instruction_set_));

  UninitCompilerContextFn uninit_compiler_context =
    FindFunction<void (*)(Compiler&)>(compiler_so_name,
                                      compiler_library_,
                                      "ArtUnInitCompilerContext");

  uninit_compiler_context(*this);
#elif defined(ART_USE_QUICK_COMPILER)
  // Uninitialize compiler_context_
  typedef void (*UninitCompilerContextFn)(Compiler&);

  std::string compiler_so_name(MakeCompilerSoName(instruction_set_));

  UninitCompilerContextFn uninit_compiler_context =
    FindFunction<void (*)(Compiler&)>(compiler_so_name,
                                      compiler_library_,
                                      "ArtUnInitQuickCompilerContext");

  uninit_compiler_context(*this);
#endif
  if (compiler_library_ != NULL) {
    VLOG(compiler) << "dlclose(" << compiler_library_ << ")";
#if !defined(ART_USE_QUICK_COMPILER)
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
     */
    dlclose(compiler_library_);
#endif
  }
}

ByteArray* Compiler::CreateResolutionStub(InstructionSet instruction_set,
                                          Runtime::TrampolineType type) {
  switch (instruction_set) {
    case kArm:
    case kThumb2:
      return arm::ArmCreateResolutionTrampoline(type);
    case kMips:
      return mips::MipsCreateResolutionTrampoline(type);
    case kX86:
      return x86::X86CreateResolutionTrampoline(type);
    default:
      LOG(FATAL) << "Unknown InstructionSet: " << instruction_set;
      return NULL;
  }
}

ByteArray* Compiler::CreateJniDlsymLookupStub(InstructionSet instruction_set) {
  switch (instruction_set) {
    case kArm:
    case kThumb2:
      return arm::CreateJniDlsymLookupStub();
    case kMips:
      return mips::CreateJniDlsymLookupStub();
    case kX86:
      return x86::CreateJniDlsymLookupStub();
    default:
      LOG(FATAL) << "Unknown InstructionSet: " << instruction_set;
      return NULL;
  }
}

ByteArray* Compiler::CreateAbstractMethodErrorStub(InstructionSet instruction_set) {
  switch (instruction_set) {
    case kArm:
    case kThumb2:
      return arm::CreateAbstractMethodErrorStub();
    case kMips:
      return mips::CreateAbstractMethodErrorStub();
    case kX86:
      return x86::CreateAbstractMethodErrorStub();
    default:
      LOG(FATAL) << "Unknown InstructionSet: " << instruction_set;
      return NULL;
  }
}

void Compiler::CompileAll(jobject class_loader,
                          const std::vector<const DexFile*>& dex_files) {
  DCHECK(!Runtime::Current()->IsStarted());

  TimingLogger timings("compiler");

  PreCompile(class_loader, dex_files, timings);

  Compile(class_loader, dex_files, timings);

  if (dump_timings_ && timings.GetTotalNs() > MsToNs(1000)) {
    timings.Dump();
  }

  if (dump_stats_) {
    stats_->Dump();
  }
}

void Compiler::CompileOne(const AbstractMethod* method) {
  DCHECK(!Runtime::Current()->IsStarted());
  Thread* self = Thread::Current();
  jobject class_loader;
  const DexCache* dex_cache;
  const DexFile* dex_file;
  {
    ScopedObjectAccessUnchecked soa(self);
    ScopedLocalRef<jobject>
      local_class_loader(soa.Env(),
                    soa.AddLocalReference<jobject>(method->GetDeclaringClass()->GetClassLoader()));
    class_loader = soa.Env()->NewGlobalRef(local_class_loader.get());
    // Find the dex_file
    dex_cache = method->GetDeclaringClass()->GetDexCache();
    dex_file = &Runtime::Current()->GetClassLinker()->FindDexFile(dex_cache);
  }
  self->TransitionFromRunnableToSuspended(kNative);

  std::vector<const DexFile*> dex_files;
  dex_files.push_back(dex_file);

  TimingLogger timings("CompileOne");
  PreCompile(class_loader, dex_files, timings);

  uint32_t method_idx = method->GetDexMethodIndex();
  const DexFile::CodeItem* code_item = dex_file->GetCodeItem(method->GetCodeItemOffset());
  CompileMethod(code_item, method->GetAccessFlags(), method->GetInvokeType(),
                method_idx, class_loader, *dex_file);

  self->GetJniEnv()->DeleteGlobalRef(class_loader);

  self->TransitionFromSuspendedToRunnable();
}

void Compiler::Resolve(jobject class_loader, const std::vector<const DexFile*>& dex_files,
                       TimingLogger& timings) {
  for (size_t i = 0; i != dex_files.size(); ++i) {
    const DexFile* dex_file = dex_files[i];
    CHECK(dex_file != NULL);
    ResolveDexFile(class_loader, *dex_file, timings);
  }
}

void Compiler::PreCompile(jobject class_loader, const std::vector<const DexFile*>& dex_files,
                          TimingLogger& timings) {
  Resolve(class_loader, dex_files, timings);

  Verify(class_loader, dex_files, timings);

  InitializeClassesWithoutClinit(class_loader, dex_files, timings);
}

bool Compiler::IsImageClass(const std::string& descriptor) const {
  if (image_classes_ == NULL) {
    return true;
  }
  return image_classes_->find(descriptor) != image_classes_->end();
}

void Compiler::RecordClassStatus(ClassReference ref, CompiledClass* compiled_class) {
  MutexLock mu(Compiler::compiled_classes_lock_);
  compiled_classes_.Put(ref, compiled_class);
}

bool Compiler::CanAssumeTypeIsPresentInDexCache(const DexFile& dex_file,
                                                uint32_t type_idx) {
  ScopedObjectAccess soa(Thread::Current());
  DexCache* dex_cache = Runtime::Current()->GetClassLinker()->FindDexCache(dex_file);
  if (!IsImage()) {
    stats_->TypeNotInDexCache();
    return false;
  }
  Class* resolved_class = dex_cache->GetResolvedType(type_idx);
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

bool Compiler::CanAssumeStringIsPresentInDexCache(const DexFile& dex_file,
                                                  uint32_t string_idx) {
  // TODO: Add support for loading strings referenced by image_classes_
  // See also Compiler::ResolveDexFile

  // The following is a test saying that if we're building the image without a restricted set of
  // image classes then we can assume the string is present in the dex cache if it is there now
  bool result = IsImage() && image_classes_ == NULL;
  if (result) {
    ScopedObjectAccess soa(Thread::Current());
    DexCache* dex_cache = Runtime::Current()->GetClassLinker()->FindDexCache(dex_file);
    result = dex_cache->GetResolvedString(string_idx) != NULL;
  }
  if (result) {
    stats_->StringInDexCache();
  } else {
    stats_->StringNotInDexCache();
  }
  return result;
}

bool Compiler::CanAccessTypeWithoutChecks(uint32_t referrer_idx, const DexFile& dex_file,
                                          uint32_t type_idx) {
  ScopedObjectAccess soa(Thread::Current());
  DexCache* dex_cache = Runtime::Current()->GetClassLinker()->FindDexCache(dex_file);
  // Get type from dex cache assuming it was populated by the verifier
  Class* resolved_class = dex_cache->GetResolvedType(type_idx);
  if (resolved_class == NULL) {
    stats_->TypeNeedsAccessCheck();
    return false;  // Unknown class needs access checks.
  }
  const DexFile::MethodId& method_id = dex_file.GetMethodId(referrer_idx);
  Class* referrer_class = dex_cache->GetResolvedType(method_id.class_idx_);
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

bool Compiler::CanAccessInstantiableTypeWithoutChecks(uint32_t referrer_idx,
                                                      const DexFile& dex_file,
                                                      uint32_t type_idx) {
  ScopedObjectAccess soa(Thread::Current());
  DexCache* dex_cache = Runtime::Current()->GetClassLinker()->FindDexCache(dex_file);
  // Get type from dex cache assuming it was populated by the verifier.
  Class* resolved_class = dex_cache->GetResolvedType(type_idx);
  if (resolved_class == NULL) {
    stats_->TypeNeedsAccessCheck();
    return false;  // Unknown class needs access checks.
  }
  const DexFile::MethodId& method_id = dex_file.GetMethodId(referrer_idx);
  Class* referrer_class = dex_cache->GetResolvedType(method_id.class_idx_);
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

static Class* ComputeCompilingMethodsClass(ScopedObjectAccess& soa,
                                           OatCompilationUnit* mUnit)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DexCache* dex_cache = mUnit->class_linker_->FindDexCache(*mUnit->dex_file_);
  ClassLoader* class_loader = soa.Decode<ClassLoader*>(mUnit->class_loader_);
  const DexFile::MethodId& referrer_method_id = mUnit->dex_file_->GetMethodId(mUnit->method_idx_);
  return mUnit->class_linker_->ResolveType(*mUnit->dex_file_, referrer_method_id.class_idx_,
                                           dex_cache, class_loader);
}

static Field* ComputeFieldReferencedFromCompilingMethod(ScopedObjectAccess& soa,
                                                        OatCompilationUnit* mUnit,
                                                        uint32_t field_idx)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DexCache* dex_cache = mUnit->class_linker_->FindDexCache(*mUnit->dex_file_);
  ClassLoader* class_loader = soa.Decode<ClassLoader*>(mUnit->class_loader_);
  return mUnit->class_linker_->ResolveField(*mUnit->dex_file_, field_idx, dex_cache,
                                            class_loader, false);
}

static AbstractMethod* ComputeMethodReferencedFromCompilingMethod(ScopedObjectAccess& soa,
                                                          OatCompilationUnit* mUnit,
                                                          uint32_t method_idx,
                                                          InvokeType type)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DexCache* dex_cache = mUnit->class_linker_->FindDexCache(*mUnit->dex_file_);
  ClassLoader* class_loader = soa.Decode<ClassLoader*>(mUnit->class_loader_);
  return mUnit->class_linker_->ResolveMethod(*mUnit->dex_file_, method_idx, dex_cache,
                                             class_loader, NULL, type);
}

bool Compiler::ComputeInstanceFieldInfo(uint32_t field_idx, OatCompilationUnit* mUnit,
                                        int& field_offset, bool& is_volatile, bool is_put) {
  ScopedObjectAccess soa(Thread::Current());
  // Conservative defaults.
  field_offset = -1;
  is_volatile = true;
  // Try to resolve field and ignore if an Incompatible Class Change Error (ie is static).
  Field* resolved_field = ComputeFieldReferencedFromCompilingMethod(soa, mUnit, field_idx);
  if (resolved_field != NULL && !resolved_field->IsStatic()) {
    Class* referrer_class = ComputeCompilingMethodsClass(soa, mUnit);
    if (referrer_class != NULL) {
      Class* fields_class = resolved_field->GetDeclaringClass();
      bool access_ok = referrer_class->CanAccess(fields_class) &&
                       referrer_class->CanAccessMember(fields_class,
                                                       resolved_field->GetAccessFlags());
      if (!access_ok) {
        // The referring class can't access the resolved field, this may occur as a result of a
        // protected field being made public by a sub-class. Resort to the dex file to determine
        // the correct class for the access check.
        const DexFile& dex_file = mUnit->class_linker_->FindDexFile(referrer_class->GetDexCache());
        Class* dex_fields_class = mUnit->class_linker_->ResolveType(dex_file,
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

bool Compiler::ComputeStaticFieldInfo(uint32_t field_idx, OatCompilationUnit* mUnit,
                                      int& field_offset, int& ssb_index,
                                      bool& is_referrers_class, bool& is_volatile, bool is_put) {
  ScopedObjectAccess soa(Thread::Current());
  // Conservative defaults.
  field_offset = -1;
  ssb_index = -1;
  is_referrers_class = false;
  is_volatile = true;
  // Try to resolve field and ignore if an Incompatible Class Change Error (ie isn't static).
  Field* resolved_field = ComputeFieldReferencedFromCompilingMethod(soa, mUnit, field_idx);
  if (resolved_field != NULL && resolved_field->IsStatic()) {
    Class* referrer_class = ComputeCompilingMethodsClass(soa, mUnit);
    if (referrer_class != NULL) {
      Class* fields_class = resolved_field->GetDeclaringClass();
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
          const DexFile& dex_file = mUnit->class_linker_->FindDexFile(referrer_class->GetDexCache());
          Class* dex_fields_class =
              mUnit->class_linker_->ResolveType(dex_file,
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
          DexCache* dex_cache = mUnit->class_linker_->FindDexCache(*mUnit->dex_file_);
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
          mUnit->dex_file_->FindStringId(descriptor);
          if (string_id != NULL) {
            const DexFile::TypeId* type_id =
               mUnit->dex_file_->FindTypeId(mUnit->dex_file_->GetIndexForStringId(*string_id));
            if (type_id != NULL) {
              // medium path, needs check of static storage base being initialized
              ssb_index = mUnit->dex_file_->GetIndexForTypeId(*type_id);
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

void Compiler::GetCodeAndMethodForDirectCall(InvokeType type, InvokeType sharp_type, AbstractMethod* method,
                                             uintptr_t& direct_code, uintptr_t& direct_method) {
  direct_code = 0;
  direct_method = 0;
  if (sharp_type != kStatic && sharp_type != kDirect) {
    return;
  }
  bool method_code_in_boot = method->GetDeclaringClass()->GetClassLoader() == NULL;
  if (!method_code_in_boot) {
    return;
  }
  bool has_clinit_trampoline = method->IsStatic() && !method->GetDeclaringClass()->IsInitialized();
  if (has_clinit_trampoline) {
    return;
  }
  stats_->DirectCallsToBoot(type);
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

bool Compiler::ComputeInvokeInfo(uint32_t method_idx, OatCompilationUnit* mUnit, InvokeType& type,
                                 int& vtable_idx, uintptr_t& direct_code,
                                 uintptr_t& direct_method) {
  ScopedObjectAccess soa(Thread::Current());
  vtable_idx = -1;
  direct_code = 0;
  direct_method = 0;
  AbstractMethod* resolved_method =
      ComputeMethodReferencedFromCompilingMethod(soa, mUnit, method_idx, type);
  if (resolved_method != NULL) {
    // Don't try to fast-path if we don't understand the caller's class or this appears to be an
    // Incompatible Class Change Error.
    Class* referrer_class = ComputeCompilingMethodsClass(soa, mUnit);
    bool icce = resolved_method->CheckIncompatibleClassChange(type);
    if (referrer_class != NULL && !icce) {
      Class* methods_class = resolved_method->GetDeclaringClass();
      if (!referrer_class->CanAccess(methods_class) ||
          !referrer_class->CanAccessMember(methods_class,
                                           resolved_method->GetAccessFlags())) {
        // The referring class can't access the resolved method, this may occur as a result of a
        // protected method being made public by implementing an interface that re-declares the
        // method public. Resort to the dex file to determine the correct class for the access
        // check.
        const DexFile& dex_file = mUnit->class_linker_->FindDexFile(referrer_class->GetDexCache());
        methods_class =
            mUnit->class_linker_->ResolveType(dex_file,
                                              dex_file.GetMethodId(method_idx).class_idx_,
                                              referrer_class);
      }
      if (referrer_class->CanAccess(methods_class) &&
          referrer_class->CanAccessMember(methods_class,
                                          resolved_method->GetAccessFlags())) {
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
          GetCodeAndMethodForDirectCall(type, kDirect, resolved_method, direct_code, direct_method);
          type = kDirect;
          return true;
        } else if (type == kSuper) {
          // Unsharpened super calls are suspicious so go slow-path.
        } else {
          stats_->ResolvedMethod(type);
          GetCodeAndMethodForDirectCall(type, type, resolved_method, direct_code, direct_method);
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

void Compiler::AddCodePatch(const DexFile* dex_file,
                            uint32_t referrer_method_idx,
                            InvokeType referrer_invoke_type,
                            uint32_t target_method_idx,
                            InvokeType target_invoke_type,
                            size_t literal_offset) {
  MutexLock mu(compiled_methods_lock_);
  code_to_patch_.push_back(new PatchInformation(dex_file,
                                                referrer_method_idx,
                                                referrer_invoke_type,
                                                target_method_idx,
                                                target_invoke_type,
                                                literal_offset));
}
void Compiler::AddMethodPatch(const DexFile* dex_file,
                              uint32_t referrer_method_idx,
                              InvokeType referrer_invoke_type,
                              uint32_t target_method_idx,
                              InvokeType target_invoke_type,
                              size_t literal_offset) {
  MutexLock mu(compiled_methods_lock_);
  methods_to_patch_.push_back(new PatchInformation(dex_file,
                                                   referrer_method_idx,
                                                   referrer_invoke_type,
                                                   target_method_idx,
                                                   target_invoke_type,
                                                   literal_offset));
}

class CompilationContext {
 public:
  CompilationContext(ClassLinker* class_linker,
          jobject class_loader,
          Compiler* compiler,
          const DexFile* dex_file)
    : class_linker_(class_linker),
      class_loader_(class_loader),
      compiler_(compiler),
      dex_file_(dex_file) {}

  ClassLinker* GetClassLinker() const {
    CHECK(class_linker_ != NULL);
    return class_linker_;
  }

  jobject GetClassLoader() const {
    return class_loader_;
  }

  Compiler* GetCompiler() const {
    CHECK(compiler_ != NULL);
    return compiler_;
  }

  const DexFile* GetDexFile() const {
    CHECK(dex_file_ != NULL);
    return dex_file_;
  }

 private:
  ClassLinker* const class_linker_;
  const jobject class_loader_;
  Compiler* const compiler_;
  const DexFile* const dex_file_;
};

typedef void Callback(const CompilationContext* context, size_t index);

static void ForAll(CompilationContext* context, size_t begin, size_t end, Callback callback,
                   size_t thread_count);

class WorkerThread {
 public:
  WorkerThread(CompilationContext* context, size_t begin, size_t end, Callback callback, size_t stripe, bool spawn)
      : spawn_(spawn), context_(context), begin_(begin), end_(end), callback_(callback), stripe_(stripe) {
    if (spawn_) {
      // Mac OS stacks are only 512KiB. Make sure we have the same stack size on all platforms.
      pthread_attr_t attr;
      CHECK_PTHREAD_CALL(pthread_attr_init, (&attr), "new compiler worker thread");
      CHECK_PTHREAD_CALL(pthread_attr_setstacksize, (&attr, 1*MB), "new compiler worker thread");
      CHECK_PTHREAD_CALL(pthread_create, (&pthread_, &attr, &Go, this), "new compiler worker thread");
      CHECK_PTHREAD_CALL(pthread_attr_destroy, (&attr), "new compiler worker thread");
    }
  }

  ~WorkerThread() {
    if (spawn_) {
      CHECK_PTHREAD_CALL(pthread_join, (pthread_, NULL), "compiler worker shutdown");
    }
  }

 private:
  static void* Go(void* arg) LOCKS_EXCLUDED(Locks::mutator_lock_) {
    WorkerThread* worker = reinterpret_cast<WorkerThread*>(arg);
    Runtime* runtime = Runtime::Current();
    if (worker->spawn_) {
      CHECK(runtime->AttachCurrentThread("Compiler Worker", true, NULL));
    }
    worker->Run();
    if (worker->spawn_) {
      runtime->DetachCurrentThread();
    }
    return NULL;
  }

  void Go() LOCKS_EXCLUDED(Locks::mutator_lock_) {
    Go(this);
  }

  void Run() LOCKS_EXCLUDED(Locks::mutator_lock_) {
    Thread* self = Thread::Current();
    for (size_t i = begin_; i < end_; i += stripe_) {
      callback_(context_, i);
      self->AssertNoPendingException();
    }
  }

  pthread_t pthread_;
  // Was this thread spawned or is it the main thread?
  const bool spawn_;

  const CompilationContext* const context_;
  const size_t begin_;
  const size_t end_;
  Callback* callback_;
  const size_t stripe_;

  friend void ForAll(CompilationContext*, size_t, size_t, Callback, size_t);
};

static void ForAll(CompilationContext* context, size_t begin, size_t end, Callback callback,
                   size_t thread_count)
    LOCKS_EXCLUDED(Locks::mutator_lock_) {
  Thread* self = Thread::Current();
  self->AssertNoPendingException();
  CHECK_GT(thread_count, 0U);

  std::vector<WorkerThread*> threads;
  for (size_t i = 0; i < thread_count; ++i) {
    threads.push_back(new WorkerThread(context, begin + i, end, callback, thread_count, (i != 0)));
  }
  threads[0]->Go();

  // Ensure we're suspended while we're blocked waiting for the other threads to finish (worker
  // thread destructor's called below perform join).
  {
    MutexLock mu(*Locks::thread_suspend_count_lock_);
    CHECK_NE(self->GetState(), kRunnable);
  }
  STLDeleteElements(&threads);
}

// Return true if the class should be skipped during compilation. We
// never skip classes in the boot class loader. However, if we have a
// non-boot class loader and we can resolve the class in the boot
// class loader, we do skip the class. This happens if an app bundles
// classes found in the boot classpath. Since at runtime we will
// select the class from the boot classpath, do not attempt to resolve
// or compile it now.
static bool SkipClass(ClassLoader* class_loader,
                      const DexFile& dex_file,
                      const DexFile::ClassDef& class_def)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (class_loader == NULL) {
    return false;
  }
  const char* descriptor = dex_file.GetClassDescriptor(class_def);
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Class* klass = class_linker->FindClass(descriptor, NULL);
  if (klass == NULL) {
    Thread* self = Thread::Current();
    CHECK(self->IsExceptionPending());
    self->ClearException();
    return false;
  }
  return true;
}

static void ResolveClassFieldsAndMethods(const CompilationContext* context, size_t class_def_index)
    LOCKS_EXCLUDED(Locks::mutator_lock_) {
  ScopedObjectAccess soa(Thread::Current());
  ClassLoader* class_loader = soa.Decode<ClassLoader*>(context->GetClassLoader());
  const DexFile& dex_file = *context->GetDexFile();

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
  ClassLinker* class_linker = context->GetClassLinker();
  DexCache* dex_cache = class_linker->FindDexCache(dex_file);
  ClassDataItemIterator it(dex_file, class_data);
  while (it.HasNextStaticField()) {
    Field* field = class_linker->ResolveField(dex_file, it.GetMemberIndex(), dex_cache,
                                              class_loader, true);
    if (field == NULL) {
      CHECK(self->IsExceptionPending());
      self->ClearException();
    }
    it.Next();
  }
  while (it.HasNextInstanceField()) {
    Field* field = class_linker->ResolveField(dex_file, it.GetMemberIndex(), dex_cache,
                                              class_loader, false);
    if (field == NULL) {
      CHECK(self->IsExceptionPending());
      self->ClearException();
    }
    it.Next();
  }
  while (it.HasNextDirectMethod()) {
    AbstractMethod* method = class_linker->ResolveMethod(dex_file, it.GetMemberIndex(), dex_cache,
                                                 class_loader, NULL, it.GetMethodInvokeType(class_def));
    if (method == NULL) {
      CHECK(self->IsExceptionPending());
      self->ClearException();
    }
    it.Next();
  }
  while (it.HasNextVirtualMethod()) {
    AbstractMethod* method = class_linker->ResolveMethod(dex_file, it.GetMemberIndex(), dex_cache,
                                                 class_loader, NULL, it.GetMethodInvokeType(class_def));
    if (method == NULL) {
      CHECK(self->IsExceptionPending());
      self->ClearException();
    }
    it.Next();
  }
  DCHECK(!it.HasNext());
}

static void ResolveType(const CompilationContext* context, size_t type_idx)
    LOCKS_EXCLUDED(Locks::mutator_lock_) {
  // Class derived values are more complicated, they require the linker and loader.
  ScopedObjectAccess soa(Thread::Current());
  ClassLinker* class_linker = context->GetClassLinker();
  const DexFile& dex_file = *context->GetDexFile();
  DexCache* dex_cache = class_linker->FindDexCache(dex_file);
  ClassLoader* class_loader = soa.Decode<ClassLoader*>(context->GetClassLoader());
  Class* klass = class_linker->ResolveType(dex_file, type_idx, dex_cache, class_loader);

  if (klass == NULL) {
    CHECK(soa.Self()->IsExceptionPending());
    Thread::Current()->ClearException();
  }
}

void Compiler::ResolveDexFile(jobject class_loader, const DexFile& dex_file,
                              TimingLogger& timings) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();

  // TODO: we could resolve strings here, although the string table is largely filled with class
  //       and method names.

  CompilationContext context(class_linker, class_loader, this, &dex_file);
  ForAll(&context, 0, dex_file.NumTypeIds(), ResolveType, thread_count_);
  timings.AddSplit("Resolve " + dex_file.GetLocation() + " Types");

  ForAll(&context, 0, dex_file.NumClassDefs(), ResolveClassFieldsAndMethods, thread_count_);
  timings.AddSplit("Resolve " + dex_file.GetLocation() + " MethodsAndFields");
}

void Compiler::Verify(jobject class_loader, const std::vector<const DexFile*>& dex_files,
                      TimingLogger& timings) {
  for (size_t i = 0; i != dex_files.size(); ++i) {
    const DexFile* dex_file = dex_files[i];
    CHECK(dex_file != NULL);
    VerifyDexFile(class_loader, *dex_file, timings);
  }
}

static void VerifyClass(const CompilationContext* context, size_t class_def_index)
    LOCKS_EXCLUDED(Locks::mutator_lock_) {
  ScopedObjectAccess soa(Thread::Current());
  const DexFile::ClassDef& class_def = context->GetDexFile()->GetClassDef(class_def_index);
  const char* descriptor = context->GetDexFile()->GetClassDescriptor(class_def);
  Class* klass =
      context->GetClassLinker()->FindClass(descriptor,
                                           soa.Decode<ClassLoader*>(context->GetClassLoader()));
  if (klass == NULL) {
    Thread* self = Thread::Current();
    CHECK(self->IsExceptionPending());
    self->ClearException();

    /*
     * At compile time, we can still structurally verify the class even if FindClass fails.
     * This is to ensure the class is structurally sound for compilation. An unsound class
     * will be rejected by the verifier and later skipped during compilation in the compiler.
     */
    DexCache* dex_cache =  context->GetClassLinker()->FindDexCache(*context->GetDexFile());
    std::string error_msg;
    if (verifier::MethodVerifier::VerifyClass(context->GetDexFile(),
                                              dex_cache,
                                              soa.Decode<ClassLoader*>(context->GetClassLoader()),
                                              class_def_index, error_msg) ==
                                                  verifier::MethodVerifier::kHardFailure) {
      const DexFile::ClassDef& class_def = context->GetDexFile()->GetClassDef(class_def_index);
      LOG(ERROR) << "Verification failed on class "
                 << PrettyDescriptor(context->GetDexFile()->GetClassDescriptor(class_def))
                 << " because: " << error_msg;
    }
    return;
  }
  CHECK(klass->IsResolved()) << PrettyClass(klass);
  context->GetClassLinker()->VerifyClass(klass);

  if (klass->IsErroneous()) {
    // ClassLinker::VerifyClass throws, which isn't useful in the compiler.
    CHECK(Thread::Current()->IsExceptionPending());
    Thread::Current()->ClearException();
  }

  CHECK(klass->IsCompileTimeVerified() || klass->IsErroneous())
      << PrettyDescriptor(klass) << ": state=" << klass->GetStatus();
  CHECK(!Thread::Current()->IsExceptionPending()) << PrettyTypeOf(Thread::Current()->GetException());
}

void Compiler::VerifyDexFile(jobject class_loader, const DexFile& dex_file, TimingLogger& timings) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  CompilationContext context(class_linker, class_loader, this, &dex_file);
  ForAll(&context, 0, dex_file.NumClassDefs(), VerifyClass, thread_count_);
  timings.AddSplit("Verify " + dex_file.GetLocation());
}

static void InitializeClassWithoutClinit(const CompilationContext* context,
                                         size_t class_def_index)
    LOCKS_EXCLUDED(Locks::mutator_lock_) {
  const DexFile::ClassDef& class_def = context->GetDexFile()->GetClassDef(class_def_index);
  ScopedObjectAccess soa(Thread::Current());
  ClassLoader* class_loader = soa.Decode<ClassLoader*>(context->GetClassLoader());
  const char* descriptor = context->GetDexFile()->GetClassDescriptor(class_def);
  Class* klass = context->GetClassLinker()->FindClass(descriptor, class_loader);
  Thread* self = Thread::Current();
  if (klass != NULL) {
    ObjectLock lock(self, klass);
    if (klass->IsVerified()) {
      // Only try to initialize classes that were successfully verified.
      bool compiling_boot = Runtime::Current()->GetHeap()->GetSpaces().size() == 1;
      bool can_init_static_fields = compiling_boot &&
          context->GetCompiler()->IsImageClass(descriptor);
      context->GetClassLinker()->EnsureInitialized(klass, false, can_init_static_fields);
      // If successfully initialized place in SSB array.
      if (klass->IsInitialized()) {
        klass->GetDexCache()->GetInitializedStaticStorage()->Set(klass->GetDexTypeIndex(), klass);
      }
    }
    // Record the final class status if necessary.
    Class::Status status = klass->GetStatus();
    Compiler::ClassReference ref(context->GetDexFile(), class_def_index);
    CompiledClass* compiled_class = context->GetCompiler()->GetCompiledClass(ref);
    if (compiled_class == NULL) {
      compiled_class = new CompiledClass(status);
      context->GetCompiler()->RecordClassStatus(ref, compiled_class);
    } else {
      DCHECK_EQ(status, compiled_class->GetStatus());
    }
  }
  // Clear any class not found or verification exceptions.
  self->ClearException();
}

void Compiler::InitializeClassesWithoutClinit(jobject jni_class_loader, const DexFile& dex_file,
                                              TimingLogger& timings) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  CompilationContext context(class_linker, jni_class_loader, this, &dex_file);
  ForAll(&context, 0, dex_file.NumClassDefs(), InitializeClassWithoutClinit, thread_count_);
  timings.AddSplit("InitializeNoClinit " + dex_file.GetLocation());
}

void Compiler::InitializeClassesWithoutClinit(jobject class_loader,
                                              const std::vector<const DexFile*>& dex_files,
                                              TimingLogger& timings) {
  for (size_t i = 0; i != dex_files.size(); ++i) {
    const DexFile* dex_file = dex_files[i];
    CHECK(dex_file != NULL);
    InitializeClassesWithoutClinit(class_loader, *dex_file, timings);
  }
}

void Compiler::Compile(jobject class_loader, const std::vector<const DexFile*>& dex_files,
                       TimingLogger& timings) {
  for (size_t i = 0; i != dex_files.size(); ++i) {
    const DexFile* dex_file = dex_files[i];
    CHECK(dex_file != NULL);
    CompileDexFile(class_loader, *dex_file, timings);
  }
}

void Compiler::CompileClass(const CompilationContext* context, size_t class_def_index) {
  jobject class_loader = context->GetClassLoader();
  const DexFile& dex_file = *context->GetDexFile();
  const DexFile::ClassDef& class_def = dex_file.GetClassDef(class_def_index);
  {
    ScopedObjectAccess soa(Thread::Current());
    ClassLoader* class_loader = soa.Decode<ClassLoader*>(context->GetClassLoader());
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
    context->GetCompiler()->CompileMethod(it.GetMethodCodeItem(), it.GetMemberAccessFlags(),
                                          it.GetMethodInvokeType(class_def), method_idx,
                                          class_loader, dex_file);
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
    context->GetCompiler()->CompileMethod(it.GetMethodCodeItem(), it.GetMemberAccessFlags(),
                                          it.GetMethodInvokeType(class_def), method_idx,
                                          class_loader, dex_file);
    it.Next();
  }
  DCHECK(!it.HasNext());
}

void Compiler::CompileDexFile(jobject class_loader, const DexFile& dex_file,
                              TimingLogger& timings) {
  CompilationContext context(NULL, class_loader, this, &dex_file);
  ForAll(&context, 0, dex_file.NumClassDefs(), Compiler::CompileClass, thread_count_);
  timings.AddSplit("Compile " + dex_file.GetLocation());
}

static std::string MakeInvokeStubKey(bool is_static, const char* shorty) {
  std::string key(shorty);
  if (is_static) {
    key += "$";  // Must not be a shorty type character.
  }
  return key;
}

void Compiler::CompileMethod(const DexFile::CodeItem* code_item, uint32_t access_flags,
                             InvokeType invoke_type, uint32_t method_idx, jobject class_loader,
                             const DexFile& dex_file) {
  CompiledMethod* compiled_method = NULL;
  uint64_t start_ns = NanoTime();

  if ((access_flags & kAccNative) != 0) {
    compiled_method = (*jni_compiler_)(*this, access_flags, method_idx, dex_file);
    CHECK(compiled_method != NULL);
  } else if ((access_flags & kAccAbstract) != 0) {
  } else {
    compiled_method = (*compiler_)(*this, code_item, access_flags, invoke_type, method_idx,
                                   class_loader, dex_file);
    CHECK(compiled_method != NULL) << PrettyMethod(method_idx, dex_file);
  }
  uint64_t duration_ns = NanoTime() - start_ns;
  if (duration_ns > MsToNs(100)) {
    LOG(WARNING) << "Compilation of " << PrettyMethod(method_idx, dex_file)
                 << " took " << PrettyDuration(duration_ns);
  }

  if (compiled_method != NULL) {
    MethodReference ref(&dex_file, method_idx);
    CHECK(GetCompiledMethod(ref) == NULL) << PrettyMethod(method_idx, dex_file);
    {
      MutexLock mu(compiled_methods_lock_);
      compiled_methods_.Put(ref, compiled_method);
    }
    DCHECK(GetCompiledMethod(ref) != NULL) << PrettyMethod(method_idx, dex_file);
  }

  uint32_t shorty_len;
  const char* shorty = dex_file.GetMethodShorty(dex_file.GetMethodId(method_idx), &shorty_len);
  bool is_static = (access_flags & kAccStatic) != 0;
  std::string key(MakeInvokeStubKey(is_static, shorty));
  const CompiledInvokeStub* compiled_invoke_stub = FindInvokeStub(key);
  if (compiled_invoke_stub == NULL) {
    compiled_invoke_stub = (*create_invoke_stub_)(*this, is_static, shorty, shorty_len);
    CHECK(compiled_invoke_stub != NULL);
    InsertInvokeStub(key, compiled_invoke_stub);
  }

#if defined(ART_USE_LLVM_COMPILER)
  if (!is_static) {
    const CompiledInvokeStub* compiled_proxy_stub = FindProxyStub(shorty);
    if (compiled_proxy_stub == NULL) {
      compiled_proxy_stub = (*create_proxy_stub_)(*this, shorty, shorty_len);
      CHECK(compiled_proxy_stub != NULL);
      InsertProxyStub(shorty, compiled_proxy_stub);
    }
  }
#endif

  if (Thread::Current()->IsExceptionPending()) {
    ScopedObjectAccess soa(Thread::Current());
    LOG(FATAL) << "Unexpected exception compiling: " << PrettyMethod(method_idx, dex_file) << "\n"
        << Thread::Current()->GetException()->Dump();
  }
}

const CompiledInvokeStub* Compiler::FindInvokeStub(bool is_static, const char* shorty) const {
  const std::string key(MakeInvokeStubKey(is_static, shorty));
  return FindInvokeStub(key);
}

const CompiledInvokeStub* Compiler::FindInvokeStub(const std::string& key) const {
  MutexLock mu(compiled_invoke_stubs_lock_);
  InvokeStubTable::const_iterator it = compiled_invoke_stubs_.find(key);
  if (it == compiled_invoke_stubs_.end()) {
    return NULL;
  } else {
    DCHECK(it->second != NULL);
    return it->second;
  }
}

void Compiler::InsertInvokeStub(const std::string& key,
                                const CompiledInvokeStub* compiled_invoke_stub) {
  MutexLock mu(compiled_invoke_stubs_lock_);
  InvokeStubTable::iterator it = compiled_invoke_stubs_.find(key);
  if (it != compiled_invoke_stubs_.end()) {
    // Someone else won the race.
    delete compiled_invoke_stub;
  } else {
    compiled_invoke_stubs_.Put(key, compiled_invoke_stub);
  }
}

#if defined(ART_USE_LLVM_COMPILER)
const CompiledInvokeStub* Compiler::FindProxyStub(const char* shorty) const {
  MutexLock mu(compiled_proxy_stubs_lock_);
  ProxyStubTable::const_iterator it = compiled_proxy_stubs_.find(shorty);
  if (it == compiled_proxy_stubs_.end()) {
    return NULL;
  } else {
    DCHECK(it->second != NULL);
    return it->second;
  }
}

void Compiler::InsertProxyStub(const char* shorty,
                               const CompiledInvokeStub* compiled_proxy_stub) {
  MutexLock mu(compiled_proxy_stubs_lock_);
  InvokeStubTable::iterator it = compiled_proxy_stubs_.find(shorty);
  if (it != compiled_proxy_stubs_.end()) {
    // Someone else won the race.
    delete compiled_proxy_stub;
  } else {
    compiled_proxy_stubs_.Put(shorty, compiled_proxy_stub);
  }
}
#endif

CompiledClass* Compiler::GetCompiledClass(ClassReference ref) const {
  MutexLock mu(compiled_classes_lock_);
  ClassTable::const_iterator it = compiled_classes_.find(ref);
  if (it == compiled_classes_.end()) {
    return NULL;
  }
  CHECK(it->second != NULL);
  return it->second;
}

CompiledMethod* Compiler::GetCompiledMethod(MethodReference ref) const {
  MutexLock mu(compiled_methods_lock_);
  MethodTable::const_iterator it = compiled_methods_.find(ref);
  if (it == compiled_methods_.end()) {
    return NULL;
  }
  CHECK(it->second != NULL);
  return it->second;
}

#if defined(ART_USE_LLVM_COMPILER) || defined(ART_USE_QUICK_COMPILER)
void Compiler::SetBitcodeFileName(std::string const& filename) {
  typedef void (*SetBitcodeFileNameFn)(Compiler&, std::string const&);

  SetBitcodeFileNameFn set_bitcode_file_name =
    FindFunction<SetBitcodeFileNameFn>(MakeCompilerSoName(instruction_set_),
                                       compiler_library_,
                                       "compilerLLVMSetBitcodeFileName");

  set_bitcode_file_name(*this, filename);
}
#endif

}  // namespace art
