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
#include <sys/mman.h>
#include <unistd.h>

#include "assembler.h"
#include "class_linker.h"
#include "class_loader.h"
#include "dex_cache.h"
#include "dex_verifier.h"
#include "jni_internal.h"
#include "oat_compilation_unit.h"
#include "oat_file.h"
#include "object_utils.h"
#include "runtime.h"
#include "space.h"
#include "stl_util.h"
#include "timing_logger.h"

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace art {

namespace arm {
  ByteArray* CreateAbstractMethodErrorStub();
  ByteArray* ArmCreateResolutionTrampoline(Runtime::TrampolineType type);
  ByteArray* CreateJniDlsymLookupStub();
}
namespace x86 {
  ByteArray* CreateAbstractMethodErrorStub();
  ByteArray* X86CreateResolutionTrampoline(Runtime::TrampolineType type);
  ByteArray* CreateJniDlsymLookupStub();
}

static double Percentage(size_t x, size_t y) {
  return 100.0 * ((double)x) / ((double)(x + y));
}

static void DumpStat(size_t x, size_t y, const char* str) {
  if (x == 0 && y == 0) {
    return;
  }
  LOG(INFO) << Percentage(x, y) << "% of " << str << " for " << (x + y) << " cases";
}

class AOTCompilationStats {
 public:
  AOTCompilationStats() : stats_lock_("AOT compilation statistics lock"),
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

  void UnresolvedInstanceField(){
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

  DISALLOW_COPY_AND_ASSIGN(AOTCompilationStats);;
};

static std::string MakeCompilerSoName(InstructionSet instruction_set) {
  // TODO: is the ARM/Thumb2 instruction set distinction really buying us anything,
  // or just causing hassle like this?
  if (instruction_set == kThumb2) {
    instruction_set = kArm;
  }

  // Capitalize the instruction set, because that's what we do in the build system.
  std::ostringstream instruction_set_name_os;
  instruction_set_name_os << instruction_set;
  std::string instruction_set_name(instruction_set_name_os.str());
  for (size_t i = 0; i < instruction_set_name.size(); ++i) {
    instruction_set_name[i] = toupper(instruction_set_name[i]);
  }

  // Bad things happen if we pull in the libartd-compiler to a libart dex2oat or vice versa,
  // because we end up with both libart and libartd in the same address space!
#ifndef NDEBUG
  const char* suffix = "d";
#else
  const char* suffix = "";
#endif

  // Work out the filename for the compiler library.
#if !defined(ART_USE_LLVM_COMPILER)
  std::string library_name(StringPrintf("art%s-compiler-%s", suffix, instruction_set_name.c_str()));
#else
  std::string library_name(StringPrintf("art%s-compiler-llvm", suffix));
#endif
  std::string filename(StringPrintf(OS_SHARED_LIB_FORMAT_STR, library_name.c_str()));

#if defined(__APPLE__)
  // On Linux, dex2oat will have been built with an RPATH of $ORIGIN/../lib, so dlopen(3) will find
  // the .so by itself. On Mac OS, there isn't really an equivalent, so we have to manually do the
  // same work.
  std::vector<char> executable_path(1);
  uint32_t executable_path_length = 0;
  _NSGetExecutablePath(&executable_path[0], &executable_path_length);
  while (_NSGetExecutablePath(&executable_path[0], &executable_path_length) == -1) {
    executable_path.resize(executable_path_length);
  }

  executable_path.resize(executable_path.size() - 1); // Strip trailing NUL.
  std::string path(&executable_path[0]);

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
                   bool support_debugging, const std::set<std::string>* image_classes)
    : instruction_set_(instruction_set),
      compiled_classes_lock_("compiled classes lock"),
      compiled_methods_lock_("compiled method lock"),
      compiled_invoke_stubs_lock_("compiled invoke stubs lock"),
      image_(image),
      thread_count_(thread_count),
      support_debugging_(support_debugging),
      stats_(new AOTCompilationStats),
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

  compiler_ = FindFunction<CompilerFn>(compiler_so_name, compiler_library_, "ArtCompileMethod");
  jni_compiler_ = FindFunction<JniCompilerFn>(compiler_so_name, compiler_library_, "ArtJniCompileMethod");
  create_invoke_stub_ = FindFunction<CreateInvokeStubFn>(compiler_so_name, compiler_library_, "ArtCreateInvokeStub");

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
  {
    MutexLock mu(compiled_methods_lock_);
    STLDeleteElements(&code_to_patch_);
  }
  {
    MutexLock mu(compiled_methods_lock_);
    STLDeleteElements(&methods_to_patch_);
  }
#if defined(ART_USE_LLVM_COMPILER)
  CompilerCallbackFn f = FindFunction<CompilerCallbackFn>(MakeCompilerSoName(instruction_set_),
                                                          compiler_library_,
                                                          "compilerLLVMDispose");
  (*f)(*this);
#endif
  if (compiler_library_ != NULL) {
    VLOG(compiler) << "dlclose(" << compiler_library_ << ")";
    dlclose(compiler_library_);
  }
}

ByteArray* Compiler::CreateResolutionStub(InstructionSet instruction_set,
                                          Runtime::TrampolineType type) {
  if (instruction_set == kX86) {
    return x86::X86CreateResolutionTrampoline(type);
  } else {
    CHECK(instruction_set == kArm || instruction_set == kThumb2);
    // Generates resolution stub using ARM instruction set
    return arm::ArmCreateResolutionTrampoline(type);
  }
}

ByteArray* Compiler::CreateJniDlsymLookupStub(InstructionSet instruction_set) {
  switch (instruction_set) {
    case kArm:
    case kThumb2:
      return arm::CreateJniDlsymLookupStub();
    case kX86:
      return x86::CreateJniDlsymLookupStub();
    default:
      LOG(FATAL) << "Unknown InstructionSet: " << instruction_set;
      return NULL;
  }
}

ByteArray* Compiler::CreateAbstractMethodErrorStub(InstructionSet instruction_set) {
  if (instruction_set == kX86) {
    return x86::CreateAbstractMethodErrorStub();
  } else {
    CHECK(instruction_set == kArm || instruction_set == kThumb2);
    // Generates resolution stub using ARM instruction set
    return arm::CreateAbstractMethodErrorStub();
  }
}

void Compiler::CompileAll(const ClassLoader* class_loader,
                          const std::vector<const DexFile*>& dex_files) {
  DCHECK(!Runtime::Current()->IsStarted());

  TimingLogger timings("compiler");

  PreCompile(class_loader, dex_files, timings);

  Compile(class_loader, dex_files);
  timings.AddSplit("Compile");

  PostCompile(class_loader, dex_files);
  timings.AddSplit("PostCompile");

  if (timings.GetTotalNs() > MsToNs(1000)) {
    timings.Dump();
  }

  stats_->Dump();
}

void Compiler::CompileOne(const Method* method) {
  DCHECK(!Runtime::Current()->IsStarted());

  const ClassLoader* class_loader = method->GetDeclaringClass()->GetClassLoader();

  // Find the dex_file
  const DexCache* dex_cache = method->GetDeclaringClass()->GetDexCache();
  const DexFile& dex_file = Runtime::Current()->GetClassLinker()->FindDexFile(dex_cache);
  std::vector<const DexFile*> dex_files;
  dex_files.push_back(&dex_file);

  TimingLogger timings("CompileOne");
  PreCompile(class_loader, dex_files, timings);

  uint32_t method_idx = method->GetDexMethodIndex();
  const DexFile::CodeItem* code_item = dex_file.GetCodeItem(method->GetCodeItemOffset());
  CompileMethod(code_item, method->GetAccessFlags(), method_idx, class_loader, dex_file);

  PostCompile(class_loader, dex_files);
}

void Compiler::Resolve(const ClassLoader* class_loader,
                       const std::vector<const DexFile*>& dex_files, TimingLogger& timings) {
  for (size_t i = 0; i != dex_files.size(); ++i) {
    const DexFile* dex_file = dex_files[i];
    CHECK(dex_file != NULL);
    ResolveDexFile(class_loader, *dex_file, timings);
  }
}

void Compiler::PreCompile(const ClassLoader* class_loader,
                          const std::vector<const DexFile*>& dex_files, TimingLogger& timings) {
  Resolve(class_loader, dex_files, timings);

  Verify(class_loader, dex_files);
  timings.AddSplit("PreCompile.Verify");

  InitializeClassesWithoutClinit(class_loader, dex_files);
  timings.AddSplit("PreCompile.InitializeClassesWithoutClinit");
}

void Compiler::PostCompile(const ClassLoader* class_loader,
                           const std::vector<const DexFile*>& dex_files) {
  SetGcMaps(class_loader, dex_files);
#if defined(ART_USE_LLVM_COMPILER)
  CompilerCallbackFn f = FindFunction<CompilerCallbackFn>(MakeCompilerSoName(instruction_set_),
                                                          compiler_library_,
                                                          "compilerLLVMMaterializeRemainder");
  (*f)(*this);
#endif
}

bool Compiler::IsImageClass(const std::string& descriptor) const {
  if (image_classes_ == NULL) {
    return true;
  }
  return image_classes_->find(descriptor) != image_classes_->end();
}

bool Compiler::CanAssumeTypeIsPresentInDexCache(const DexCache* dex_cache,
                                                uint32_t type_idx) {
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

bool Compiler::CanAssumeStringIsPresentInDexCache(const DexCache* dex_cache,
                                                  uint32_t string_idx) {
  // TODO: Add support for loading strings referenced by image_classes_
  // See also Compiler::ResolveDexFile

  // The following is a test saying that if we're building the image without a restricted set of
  // image classes then we can assume the string is present in the dex cache if it is there now
  bool result = IsImage() && image_classes_ == NULL && dex_cache->GetResolvedString(string_idx) != NULL;
  if (result) {
    stats_->StringInDexCache();
  } else {
    stats_->StringNotInDexCache();
  }
  return result;
}

bool Compiler::CanAccessTypeWithoutChecks(uint32_t referrer_idx, const DexCache* dex_cache,
                                          const DexFile& dex_file, uint32_t type_idx) {
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
                                                      const DexCache* dex_cache,
                                                      const DexFile& dex_file,
                                                      uint32_t type_idx) {
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

static Class* ComputeReferrerClass(OatCompilationUnit* mUnit) {
  const DexFile::MethodId& referrer_method_id =
    mUnit->dex_file_->GetMethodId(mUnit->method_idx_);

  return mUnit->class_linker_->ResolveType(
    *mUnit->dex_file_, referrer_method_id.class_idx_,
    mUnit->dex_cache_, mUnit->class_loader_);
}

static Field* ComputeReferrerField(OatCompilationUnit* mUnit, uint32_t field_idx) {
  return mUnit->class_linker_->ResolveField(
    *mUnit->dex_file_, field_idx, mUnit->dex_cache_,
    mUnit->class_loader_, false);
}

static Method* ComputeReferrerMethod(OatCompilationUnit* mUnit, uint32_t method_idx) {
  return mUnit->class_linker_->ResolveMethod(
    *mUnit->dex_file_, method_idx, mUnit->dex_cache_,
    mUnit->class_loader_, true);
}

bool Compiler::ComputeInstanceFieldInfo(uint32_t field_idx, OatCompilationUnit* mUnit,
                                        int& field_offset, bool& is_volatile, bool is_put) {
  // Conservative defaults
  field_offset = -1;
  is_volatile = true;
  // Try to resolve field
  Field* resolved_field = ComputeReferrerField(mUnit, field_idx);
  if (resolved_field != NULL) {
    Class* referrer_class = ComputeReferrerClass(mUnit);
    // Try to resolve referring class then access check, failure to pass the
    Class* fields_class = resolved_field->GetDeclaringClass();
    bool is_write_to_final_from_wrong_class = is_put && resolved_field->IsFinal() &&
                                              fields_class != referrer_class;
    if (referrer_class != NULL && referrer_class->CanAccess(fields_class) &&
        referrer_class->CanAccessMember(fields_class, resolved_field->GetAccessFlags()) &&
        !is_write_to_final_from_wrong_class) {
      field_offset = resolved_field->GetOffset().Int32Value();
      is_volatile = resolved_field->IsVolatile();
      stats_->ResolvedInstanceField();
      return true;  // Fast path.
    }
  }
  // Clean up any exception left by field/type resolution
  Thread* thread = Thread::Current();
  if (thread->IsExceptionPending()) {
      thread->ClearException();
  }
  stats_->UnresolvedInstanceField();
  return false;  // Incomplete knowledge needs slow path.
}

bool Compiler::ComputeStaticFieldInfo(uint32_t field_idx, OatCompilationUnit* mUnit,
                                      int& field_offset, int& ssb_index,
                                      bool& is_referrers_class, bool& is_volatile, bool is_put) {
  // Conservative defaults
  field_offset = -1;
  ssb_index = -1;
  is_referrers_class = false;
  is_volatile = true;
  // Try to resolve field
  Field* resolved_field = ComputeReferrerField(mUnit, field_idx);
  if (resolved_field != NULL) {
    DCHECK(resolved_field->IsStatic());
    Class* referrer_class = ComputeReferrerClass(mUnit);
    if (referrer_class != NULL) {
      Class* fields_class = resolved_field->GetDeclaringClass();
      if (fields_class == referrer_class) {
        is_referrers_class = true;  // implies no worrying about class initialization
        field_offset = resolved_field->GetOffset().Int32Value();
        is_volatile = resolved_field->IsVolatile();
        stats_->ResolvedLocalStaticField();
        return true;  // fast path
      } else {
        bool is_write_to_final_from_wrong_class = is_put && resolved_field->IsFinal();
        if (referrer_class->CanAccess(fields_class) &&
            referrer_class->CanAccessMember(fields_class, resolved_field->GetAccessFlags()) &&
            !is_write_to_final_from_wrong_class) {
          // We have the resolved field, we must make it into a ssbIndex for the referrer
          // in its static storage base (which may fail if it doesn't have a slot for it)
          // TODO: for images we can elide the static storage base null check
          // if we know there's a non-null entry in the image
          if (fields_class->GetDexCache() == mUnit->dex_cache_) {
            // common case where the dex cache of both the referrer and the field are the same,
            // no need to search the dex file
            ssb_index = fields_class->GetDexTypeIndex();
            field_offset = resolved_field->GetOffset().Int32Value();
            is_volatile = resolved_field->IsVolatile();
            stats_->ResolvedStaticField();
            return true;
          }
          // Search dex file for localized ssb index
          std::string descriptor(FieldHelper(resolved_field).GetDeclaringClassDescriptor());
          const DexFile::StringId* string_id =
          mUnit->dex_file_->FindStringId(descriptor);
          if (string_id != NULL) {
            const DexFile::TypeId* type_id =
               mUnit->dex_file_->FindTypeId(mUnit->dex_file_->GetIndexForStringId(*string_id));
            if(type_id != NULL) {
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
  Thread* thread = Thread::Current();
  if (thread->IsExceptionPending()) {
      thread->ClearException();
  }
  stats_->UnresolvedStaticField();
  return false;  // Incomplete knowledge needs slow path.
}

void Compiler::GetCodeAndMethodForDirectCall(InvokeType type, InvokeType sharp_type, Method* method,
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
    if (Runtime::Current()->GetHeap()->GetImageSpace()->Contains(method)) {
      direct_method = reinterpret_cast<uintptr_t>(method);
    }
    direct_code = reinterpret_cast<uintptr_t>(method->GetCode());
  }
}

bool Compiler::ComputeInvokeInfo(uint32_t method_idx, OatCompilationUnit* mUnit, InvokeType& type,
                                 int& vtable_idx, uintptr_t& direct_code,
                                 uintptr_t& direct_method) {
  vtable_idx = -1;
  direct_code = 0;
  direct_method = 0;
  Method* resolved_method = ComputeReferrerMethod(mUnit, method_idx);
  if (resolved_method != NULL) {
    Class* referrer_class = ComputeReferrerClass(mUnit);
    if (referrer_class != NULL) {
      Class* methods_class = resolved_method->GetDeclaringClass();
      if (!referrer_class->CanAccess(methods_class) ||
          !referrer_class->CanAccessMember(methods_class,
                                           resolved_method->GetAccessFlags())) {
        // The referring class can't access the resolved method, this may occur as a result of a
        // protected method being made public by implementing an interface that re-declares the
        // method public. Resort to the dex file to determine the correct class for the access check
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
        // ensure the vtable index will be correct to dispatch in the vtable of the super class
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
          // Unsharpened super calls are suspicious so go slowpath.
        } else {
          stats_->ResolvedMethod(type);
          GetCodeAndMethodForDirectCall(type, type, resolved_method, direct_code, direct_method);
          return true;
        }
      }
    }
  }
  // Clean up any exception left by method/type resolution
  Thread* thread = Thread::Current();
  if (thread->IsExceptionPending()) {
      thread->ClearException();
  }
  stats_->UnresolvedMethod(type);
  return false;  // Incomplete knowledge needs slow path.
}

void Compiler::AddCodePatch(DexCache* dex_cache,
                            const DexFile* dex_file,
                            uint32_t referrer_method_idx,
                            uint32_t referrer_access_flags,
                            uint32_t target_method_idx,
                            bool target_is_direct,
                            size_t literal_offset) {
  MutexLock mu(compiled_methods_lock_);
  code_to_patch_.push_back(new PatchInformation(dex_cache,
                                                dex_file,
                                                referrer_method_idx,
                                                referrer_access_flags,
                                                target_method_idx,
                                                target_is_direct,
                                                literal_offset));
}
void Compiler::AddMethodPatch(DexCache* dex_cache,
                              const DexFile* dex_file,
                              uint32_t referrer_method_idx,
                              uint32_t referrer_access_flags,
                              uint32_t target_method_idx,
                              bool target_is_direct,
                              size_t literal_offset) {
  MutexLock mu(compiled_methods_lock_);
  methods_to_patch_.push_back(new PatchInformation(dex_cache,
                                                   dex_file,
                                                   referrer_method_idx,
                                                   referrer_access_flags,
                                                   target_method_idx,
                                                   target_is_direct,
                                                   literal_offset));
}

// Return true if the class should be skipped during compilation. We
// never skip classes in the boot class loader. However, if we have a
// non-boot class loader and we can resolve the class in the boot
// class loader, we do skip the class. This happens if an app bundles
// classes found in the boot classpath. Since at runtime we will
// select the class from the boot classpath, do not attempt to resolve
// or compile it now.
static bool SkipClass(const ClassLoader* class_loader,
                      const DexFile& dex_file,
                      const DexFile::ClassDef& class_def) {
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

class Context {
 public:
  Context(ClassLinker* class_linker,
          const ClassLoader* class_loader,
          Compiler* compiler,
          DexCache* dex_cache,
          const DexFile* dex_file)
    : class_linker_(class_linker),
      class_loader_(class_loader),
      compiler_(compiler),
      dex_cache_(dex_cache),
      dex_file_(dex_file) {}

  ClassLinker* GetClassLinker() {
    CHECK(class_linker_ != NULL);
    return class_linker_;
  }
  const ClassLoader* GetClassLoader() {
    return class_loader_;
  }
  Compiler* GetCompiler() {
    CHECK(compiler_ != NULL);
    return compiler_;
  }
  DexCache* GetDexCache() {
    CHECK(dex_cache_ != NULL);
    return dex_cache_;
  }
  const DexFile* GetDexFile() {
    CHECK(dex_file_ != NULL);
    return dex_file_;
  }

 private:
  ClassLinker* class_linker_;
  const ClassLoader* class_loader_;
  Compiler* compiler_;
  DexCache* dex_cache_;
  const DexFile* dex_file_;
};

typedef void Callback(Context* context, size_t index);

class WorkerThread {
 public:
  WorkerThread(Context* context, size_t begin, size_t end, Callback callback, size_t stripe, bool spawn)
      : spawn_(spawn), context_(context), begin_(begin), end_(end), callback_(callback), stripe_(stripe) {
    if (spawn_) {
      CHECK_PTHREAD_CALL(pthread_create, (&pthread_, NULL, &Go, this), "compiler worker thread");
    }
  }

  ~WorkerThread() {
    if (spawn_) {
      CHECK_PTHREAD_CALL(pthread_join, (pthread_, NULL), "compiler worker shutdown");
    }
  }

 private:
  static void* Go(void* arg) {
    WorkerThread* worker = reinterpret_cast<WorkerThread*>(arg);
    Runtime* runtime = Runtime::Current();
    if (worker->spawn_) {
      runtime->AttachCurrentThread("Compiler Worker", true, NULL);
    }
    Thread::Current()->SetState(Thread::kRunnable);
    worker->Run();
    if (worker->spawn_) {
      Thread::Current()->SetState(Thread::kNative);
      runtime->DetachCurrentThread();
    }
    return NULL;
  }

  void Go() {
    Go(this);
  }

  void Run() {
    for (size_t i = begin_; i < end_; i += stripe_) {
      callback_(context_, i);
    }
  }

  pthread_t pthread_;
  bool spawn_;

  Context* context_;
  size_t begin_;
  size_t end_;
  Callback* callback_;
  size_t stripe_;

  friend void ForAll(Context*, size_t, size_t, Callback, size_t);
};

void ForAll(Context* context, size_t begin, size_t end, Callback callback, size_t thread_count) {
  CHECK_GT(thread_count, 0U);

  std::vector<WorkerThread*> threads;
  for (size_t i = 0; i < thread_count; ++i) {
    threads.push_back(new WorkerThread(context, begin + i, end, callback, thread_count, (i != 0)));
  }
  threads[0]->Go();

  // Switch to kVmWait while we're blocked waiting for the other threads to finish.
  ScopedThreadStateChange tsc(Thread::Current(), Thread::kVmWait);
  STLDeleteElements(&threads);
}

static void ResolveClassFieldsAndMethods(Context* context, size_t class_def_index) {
  const DexFile& dex_file = *context->GetDexFile();

  // Method and Field are the worst. We can't resolve without either
  // context from the code use (to disambiguate virtual vs direct
  // method and instance vs static field) or from class
  // definitions. While the compiler will resolve what it can as it
  // needs it, here we try to resolve fields and methods used in class
  // definitions, since many of them many never be referenced by
  // generated code.
  const DexFile::ClassDef& class_def = dex_file.GetClassDef(class_def_index);
  if (SkipClass(context->GetClassLoader(), dex_file, class_def)) {
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
                                              context->GetClassLoader(), true);
    if (field == NULL) {
      CHECK(self->IsExceptionPending());
      self->ClearException();
    }
    it.Next();
  }
  while (it.HasNextInstanceField()) {
    Field* field = class_linker->ResolveField(dex_file, it.GetMemberIndex(), dex_cache,
                                              context->GetClassLoader(), false);
    if (field == NULL) {
      CHECK(self->IsExceptionPending());
      self->ClearException();
    }
    it.Next();
  }
  while (it.HasNextDirectMethod()) {
    Method* method = class_linker->ResolveMethod(dex_file, it.GetMemberIndex(), dex_cache,
                                                 context->GetClassLoader(), true);
    if (method == NULL) {
      CHECK(self->IsExceptionPending());
      self->ClearException();
    }
    it.Next();
  }
  while (it.HasNextVirtualMethod()) {
    Method* method = class_linker->ResolveMethod(dex_file, it.GetMemberIndex(), dex_cache,
                                                 context->GetClassLoader(), false);
    if (method == NULL) {
      CHECK(self->IsExceptionPending());
      self->ClearException();
    }
    it.Next();
  }
  DCHECK(!it.HasNext());
}

static void ResolveType(Context* context, size_t type_idx) {
  // Class derived values are more complicated, they require the linker and loader.
  Thread* self = Thread::Current();
  Class* klass = context->GetClassLinker()->ResolveType(*context->GetDexFile(),
                                                        type_idx,
                                                        context->GetDexCache(),
                                                        context->GetClassLoader());
  if (klass == NULL) {
    CHECK(self->IsExceptionPending());
    Thread::Current()->ClearException();
  }
}

void Compiler::ResolveDexFile(const ClassLoader* class_loader, const DexFile& dex_file, TimingLogger& timings) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  DexCache* dex_cache = class_linker->FindDexCache(dex_file);

  // Strings are easy in that they always are simply resolved to literals in the same file
  if (image_ && image_classes_ == NULL) {
    // TODO: Add support for loading strings referenced by image_classes_
    // See also Compiler::CanAssumeTypeIsPresentInDexCache.
    for (size_t string_idx = 0; string_idx < dex_cache->NumStrings(); string_idx++) {
      class_linker->ResolveString(dex_file, string_idx, dex_cache);
    }
    timings.AddSplit("Resolve " + dex_file.GetLocation() + " Strings");
  }

  Context context(class_linker, class_loader, this, dex_cache, &dex_file);
  ForAll(&context, 0, dex_cache->NumResolvedTypes(), ResolveType, thread_count_);
  timings.AddSplit("Resolve " + dex_file.GetLocation() + " Types");

  ForAll(&context, 0, dex_file.NumClassDefs(), ResolveClassFieldsAndMethods, thread_count_);
  timings.AddSplit("Resolve " + dex_file.GetLocation() + " MethodsAndFields");
}

void Compiler::Verify(const ClassLoader* class_loader,
                      const std::vector<const DexFile*>& dex_files) {
  for (size_t i = 0; i != dex_files.size(); ++i) {
    const DexFile* dex_file = dex_files[i];
    CHECK(dex_file != NULL);
    VerifyDexFile(class_loader, *dex_file);
  }
}

static void VerifyClass(Context* context, size_t class_def_index) {
  const DexFile::ClassDef& class_def = context->GetDexFile()->GetClassDef(class_def_index);
  const char* descriptor = context->GetDexFile()->GetClassDescriptor(class_def);
  Class* klass = context->GetClassLinker()->FindClass(descriptor, context->GetClassLoader());
  if (klass == NULL) {
    Thread* self = Thread::Current();
    CHECK(self->IsExceptionPending());
    self->ClearException();

    /*
     * At compile time, we can still structurally verify the class even if FindClass fails.
     * This is to ensure the class is structurally sound for compilation. An unsound class
     * will be rejected by the verifier and later skipped during compilation in the compiler.
     */
    std::string error_msg;
    if (!verifier::DexVerifier::VerifyClass(context->GetDexFile(), context->GetDexCache(),
        context->GetClassLoader(), class_def_index, error_msg)) {
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
    art::Compiler::ClassReference ref(context->GetDexFile(), class_def_index);
    if (!verifier::DexVerifier::IsClassRejected(ref)) {
      // If the erroneous class wasn't rejected by the verifier, it was a soft error. We want
      // to try verification again at run-time, so move back into the resolved state.
      klass->SetStatus(Class::kStatusResolved);
    }
  }

  CHECK(klass->IsVerified() || klass->IsResolved() || klass->IsErroneous()) << PrettyClass(klass);
  CHECK(!Thread::Current()->IsExceptionPending()) << PrettyTypeOf(Thread::Current()->GetException());
}

void Compiler::VerifyDexFile(const ClassLoader* class_loader, const DexFile& dex_file) {
  dex_file.ChangePermissions(PROT_READ | PROT_WRITE);

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Context context(class_linker, class_loader, this, class_linker->FindDexCache(dex_file), &dex_file);
  ForAll(&context, 0, dex_file.NumClassDefs(), VerifyClass, thread_count_);

  dex_file.ChangePermissions(PROT_READ);
}

void Compiler::InitializeClassesWithoutClinit(const ClassLoader* class_loader,
                                              const std::vector<const DexFile*>& dex_files) {
  for (size_t i = 0; i != dex_files.size(); ++i) {
    const DexFile* dex_file = dex_files[i];
    CHECK(dex_file != NULL);
    InitializeClassesWithoutClinit(class_loader, *dex_file);
  }
}

void Compiler::InitializeClassesWithoutClinit(const ClassLoader* class_loader, const DexFile& dex_file) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  for (size_t class_def_index = 0; class_def_index < dex_file.NumClassDefs(); class_def_index++) {
    const DexFile::ClassDef& class_def = dex_file.GetClassDef(class_def_index);
    const char* descriptor = dex_file.GetClassDescriptor(class_def);
    Class* klass = class_linker->FindClass(descriptor, class_loader);
    if (klass != NULL) {
      if (klass->IsVerified()) {
        // Only try to initialize classes that were successfully verified.
        class_linker->EnsureInitialized(klass, false);
      }
      // record the final class status if necessary
      Class::Status status = klass->GetStatus();
      ClassReference ref(&dex_file, class_def_index);
      MutexLock mu(compiled_classes_lock_);
      CompiledClass* compiled_class = GetCompiledClass(ref);
      if (compiled_class == NULL) {
        compiled_class = new CompiledClass(status);
        compiled_classes_[ref] = compiled_class;
      } else {
        DCHECK_EQ(status, compiled_class->GetStatus());
      }
    }
    // clear any class not found or verification exceptions
    Thread::Current()->ClearException();
  }

  DexCache* dex_cache = class_linker->FindDexCache(dex_file);
  for (size_t type_idx = 0; type_idx < dex_cache->NumResolvedTypes(); type_idx++) {
    Class* klass = class_linker->ResolveType(dex_file, type_idx, dex_cache, class_loader);
    if (klass == NULL) {
      Thread::Current()->ClearException();
    } else if (klass->IsInitialized()) {
      dex_cache->GetInitializedStaticStorage()->Set(type_idx, klass);
    }
  }
}

void Compiler::Compile(const ClassLoader* class_loader,
                       const std::vector<const DexFile*>& dex_files) {
  for (size_t i = 0; i != dex_files.size(); ++i) {
    const DexFile* dex_file = dex_files[i];
    CHECK(dex_file != NULL);
    CompileDexFile(class_loader, *dex_file);
  }
}

void Compiler::CompileClass(Context* context, size_t class_def_index) {
  const ClassLoader* class_loader = context->GetClassLoader();
  const DexFile& dex_file = *context->GetDexFile();
  const DexFile::ClassDef& class_def = dex_file.GetClassDef(class_def_index);
#if defined(ART_USE_LLVM_COMPILER)
  // TODO: Remove this.  We should not lock the compiler_lock_ in CompileClass()
  // However, without this mutex lock, we will get segmentation fault before
  // LLVM becomes multithreaded.
  Compiler* cmplr = context->GetCompiler();
  CompilerMutexLockFn f =
      FindFunction<CompilerMutexLockFn>(MakeCompilerSoName(cmplr->GetInstructionSet()),
                                        cmplr->compiler_library_,
                                        "compilerLLVMMutexLock");
  UniquePtr<MutexLock> GUARD((*f)(*cmplr));
#endif

  if (SkipClass(class_loader, dex_file, class_def)) {
    return;
  }
  ClassReference ref(&dex_file, class_def_index);
  // Skip compiling classes with generic verifier failures since they will still fail at runtime
  if (verifier::DexVerifier::IsClassRejected(ref)) {
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
  while (it.HasNextDirectMethod()) {
    context->GetCompiler()->CompileMethod(it.GetMethodCodeItem(), it.GetMemberAccessFlags(),
                                          it.GetMemberIndex(), class_loader, dex_file);
    it.Next();
  }
  // Compile virtual methods
  while (it.HasNextVirtualMethod()) {
    context->GetCompiler()->CompileMethod(it.GetMethodCodeItem(), it.GetMemberAccessFlags(),
                                          it.GetMemberIndex(), class_loader, dex_file);
    it.Next();
  }
  DCHECK(!it.HasNext());
}

void Compiler::CompileDexFile(const ClassLoader* class_loader, const DexFile& dex_file) {
  Context context(NULL, class_loader, this, NULL, &dex_file);
  ForAll(&context, 0, dex_file.NumClassDefs(), Compiler::CompileClass, thread_count_);
}

void Compiler::CompileMethod(const DexFile::CodeItem* code_item, uint32_t access_flags,
                             uint32_t method_idx, const ClassLoader* class_loader,
                             const DexFile& dex_file) {
  CompiledMethod* compiled_method = NULL;
  uint64_t start_ns = NanoTime();

  if ((access_flags & kAccNative) != 0) {
    compiled_method = (*jni_compiler_)(*this, access_flags, method_idx, class_loader, dex_file);
    CHECK(compiled_method != NULL);
  } else if ((access_flags & kAccAbstract) != 0) {
  } else {
    compiled_method = (*compiler_)(*this, code_item, access_flags, method_idx, class_loader,
                                   dex_file);
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
    MutexLock mu(compiled_methods_lock_);
    compiled_methods_[ref] = compiled_method;
    DCHECK(GetCompiledMethod(ref) != NULL) << PrettyMethod(method_idx, dex_file);
  }

  uint32_t shorty_len;
  const char* shorty = dex_file.GetMethodShorty(dex_file.GetMethodId(method_idx), &shorty_len);
  bool is_static = (access_flags & kAccStatic) != 0;
  const CompiledInvokeStub* compiled_invoke_stub = FindInvokeStub(is_static, shorty);
  if (compiled_invoke_stub == NULL) {
    compiled_invoke_stub = (*create_invoke_stub_)(*this, is_static, shorty, shorty_len);
    CHECK(compiled_invoke_stub != NULL);
    InsertInvokeStub(is_static, shorty, compiled_invoke_stub);
  }
  CHECK(!Thread::Current()->IsExceptionPending()) << PrettyMethod(method_idx, dex_file);
}

static std::string MakeInvokeStubKey(bool is_static, const char* shorty) {
  std::string key(shorty);
  if (is_static) {
    key += "$";  // Must not be a shorty type character.
  }
  return key;
}

const CompiledInvokeStub* Compiler::FindInvokeStub(bool is_static, const char* shorty) const {
  MutexLock mu(compiled_invoke_stubs_lock_);
  const std::string key(MakeInvokeStubKey(is_static, shorty));
  InvokeStubTable::const_iterator it = compiled_invoke_stubs_.find(key);
  if (it == compiled_invoke_stubs_.end()) {
    return NULL;
  } else {
    DCHECK(it->second != NULL);
    return it->second;
  }
}

void Compiler::InsertInvokeStub(bool is_static, const char* shorty,
                                const CompiledInvokeStub* compiled_invoke_stub) {
  MutexLock mu(compiled_invoke_stubs_lock_);
  std::string key(MakeInvokeStubKey(is_static, shorty));
  compiled_invoke_stubs_[key] = compiled_invoke_stub;
}

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

void Compiler::SetGcMaps(const ClassLoader* class_loader, const std::vector<const DexFile*>& dex_files) {
  for (size_t i = 0; i != dex_files.size(); ++i) {
    const DexFile* dex_file = dex_files[i];
    CHECK(dex_file != NULL);
    SetGcMapsDexFile(class_loader, *dex_file);
  }
}

void Compiler::SetGcMapsDexFile(const ClassLoader* class_loader, const DexFile& dex_file) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  DexCache* dex_cache = class_linker->FindDexCache(dex_file);
  for (size_t class_def_index = 0; class_def_index < dex_file.NumClassDefs(); class_def_index++) {
    const DexFile::ClassDef& class_def = dex_file.GetClassDef(class_def_index);
    const char* descriptor = dex_file.GetClassDescriptor(class_def);
    Class* klass = class_linker->FindClass(descriptor, class_loader);
    if (klass == NULL || !klass->IsVerified()) {
      Thread::Current()->ClearException();
      continue;
    }
    const byte* class_data = dex_file.GetClassData(class_def);
    if (class_data == NULL) {
      // empty class such as a marker interface
      continue;
    }
    ClassDataItemIterator it(dex_file, class_data);
    while (it.HasNextStaticField()) {
      it.Next();
    }
    while (it.HasNextInstanceField()) {
      it.Next();
    }
    while (it.HasNextDirectMethod()) {
      Method* method = class_linker->ResolveMethod(dex_file, it.GetMemberIndex(), dex_cache,
                                                   class_loader, true);
      SetGcMapsMethod(dex_file, method);
      it.Next();
    }
    while (it.HasNextVirtualMethod()) {
      Method* method = class_linker->ResolveMethod(dex_file, it.GetMemberIndex(), dex_cache,
                                                   class_loader, false);
      SetGcMapsMethod(dex_file, method);
      it.Next();
    }
  }
}

void Compiler::SetGcMapsMethod(const DexFile& dex_file, Method* method) {
  if (method == NULL) {
    Thread::Current()->ClearException();
    return;
  }
  uint16_t method_idx = method->GetDexMethodIndex();
  MethodReference ref(&dex_file, method_idx);
  CompiledMethod* compiled_method = GetCompiledMethod(ref);
  if (compiled_method == NULL) {
    return;
  }
  const std::vector<uint8_t>* gc_map = verifier::DexVerifier::GetGcMap(ref);
  if (gc_map == NULL) {
    return;
  }
  compiled_method->SetGcMap(*gc_map);
}

#if defined(ART_USE_LLVM_COMPILER)
void Compiler::SetElfFileName(std::string const& filename) {
  elf_filename_ = filename;
}

void Compiler::SetBitcodeFileName(std::string const& filename) {
  bitcode_filename_ = filename;
}
std::string const& Compiler::GetElfFileName() {
  return elf_filename_;
}
std::string const& Compiler::GetBitcodeFileName() {
  return bitcode_filename_;
}
#endif

}  // namespace art
