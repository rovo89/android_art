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

#ifndef ART_COMPILER_DRIVER_COMPILER_DRIVER_H_
#define ART_COMPILER_DRIVER_COMPILER_DRIVER_H_

#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "arch/instruction_set.h"
#include "base/arena_allocator.h"
#include "base/bit_utils.h"
#include "base/mutex.h"
#include "base/timing_logger.h"
#include "class_reference.h"
#include "compiler.h"
#include "dex_file.h"
#include "driver/compiled_method_storage.h"
#include "jit/offline_profiling_info.h"
#include "invoke_type.h"
#include "method_reference.h"
#include "mirror/class.h"  // For mirror::Class::Status.
#include "os.h"
#include "runtime.h"
#include "safe_map.h"
#include "thread_pool.h"
#include "utils/array_ref.h"
#include "utils/dex_cache_arrays_layout.h"

namespace art {

namespace mirror {
class DexCache;
}  // namespace mirror

namespace verifier {
class MethodVerifier;
}  // namespace verifier

class BitVector;
class CompiledClass;
class CompiledMethod;
class CompilerOptions;
class DexCompilationUnit;
class DexFileToMethodInlinerMap;
struct InlineIGetIPutData;
class InstructionSetFeatures;
class ParallelCompilationManager;
class ScopedObjectAccess;
template <class Allocator> class SrcMap;
class SrcMapElem;
using SwapSrcMap = SrcMap<SwapAllocator<SrcMapElem>>;
template<class T> class Handle;
class TimingLogger;
class VerificationResults;
class VerifiedMethod;

enum EntryPointCallingConvention {
  // ABI of invocations to a method's interpreter entry point.
  kInterpreterAbi,
  // ABI of calls to a method's native code, only used for native methods.
  kJniAbi,
  // ABI of calls to a method's quick code entry point.
  kQuickAbi
};

class CompilerDriver {
 public:
  // Create a compiler targeting the requested "instruction_set".
  // "image" should be true if image specific optimizations should be
  // enabled.  "image_classes" lets the compiler know what classes it
  // can assume will be in the image, with null implying all available
  // classes.
  CompilerDriver(const CompilerOptions* compiler_options,
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
                 const ProfileCompilationInfo* profile_compilation_info);

  ~CompilerDriver();

  // Set dex files that will be stored in the oat file after being compiled.
  void SetDexFilesForOatFile(const std::vector<const DexFile*>& dex_files) {
    dex_files_for_oat_file_ = &dex_files;
  }

  // Get dex file that will be stored in the oat file after being compiled.
  ArrayRef<const DexFile* const> GetDexFilesForOatFile() const {
    return (dex_files_for_oat_file_ != nullptr)
        ? ArrayRef<const DexFile* const>(*dex_files_for_oat_file_)
        : ArrayRef<const DexFile* const>();
  }

  void CompileAll(jobject class_loader,
                  const std::vector<const DexFile*>& dex_files,
                  TimingLogger* timings)
      REQUIRES(!Locks::mutator_lock_, !compiled_classes_lock_, !dex_to_dex_references_lock_);

  // Compile a single Method.
  void CompileOne(Thread* self, ArtMethod* method, TimingLogger* timings)
      SHARED_REQUIRES(Locks::mutator_lock_)
      REQUIRES(!compiled_methods_lock_, !compiled_classes_lock_, !dex_to_dex_references_lock_);

  VerificationResults* GetVerificationResults() const {
    DCHECK(Runtime::Current()->IsAotCompiler());
    return verification_results_;
  }

  DexFileToMethodInlinerMap* GetMethodInlinerMap() const {
    return method_inliner_map_;
  }

  InstructionSet GetInstructionSet() const {
    return instruction_set_;
  }

  const InstructionSetFeatures* GetInstructionSetFeatures() const {
    return instruction_set_features_;
  }

  const CompilerOptions& GetCompilerOptions() const {
    return *compiler_options_;
  }

  Compiler* GetCompiler() const {
    return compiler_.get();
  }

  // Are we compiling and creating an image file?
  bool IsBootImage() const {
    return boot_image_;
  }

  const std::unordered_set<std::string>* GetImageClasses() const {
    return image_classes_.get();
  }

  // Generate the trampolines that are invoked by unresolved direct methods.
  std::unique_ptr<const std::vector<uint8_t>> CreateJniDlsymLookup() const;
  std::unique_ptr<const std::vector<uint8_t>> CreateQuickGenericJniTrampoline() const;
  std::unique_ptr<const std::vector<uint8_t>> CreateQuickImtConflictTrampoline() const;
  std::unique_ptr<const std::vector<uint8_t>> CreateQuickResolutionTrampoline() const;
  std::unique_ptr<const std::vector<uint8_t>> CreateQuickToInterpreterBridge() const;

  CompiledClass* GetCompiledClass(ClassReference ref) const
      REQUIRES(!compiled_classes_lock_);

  CompiledMethod* GetCompiledMethod(MethodReference ref) const
      REQUIRES(!compiled_methods_lock_);
  size_t GetNonRelativeLinkerPatchCount() const
      REQUIRES(!compiled_methods_lock_);

  // Add a compiled method.
  void AddCompiledMethod(const MethodReference& method_ref,
                         CompiledMethod* const compiled_method,
                         size_t non_relative_linker_patch_count)
      REQUIRES(!compiled_methods_lock_);
  // Remove and delete a compiled method.
  void RemoveCompiledMethod(const MethodReference& method_ref) REQUIRES(!compiled_methods_lock_);

  void SetRequiresConstructorBarrier(Thread* self,
                                     const DexFile* dex_file,
                                     uint16_t class_def_index,
                                     bool requires)
      REQUIRES(!requires_constructor_barrier_lock_);
  bool RequiresConstructorBarrier(Thread* self,
                                  const DexFile* dex_file,
                                  uint16_t class_def_index)
      REQUIRES(!requires_constructor_barrier_lock_);

  // Callbacks from compiler to see what runtime checks must be generated.

  bool CanAssumeTypeIsPresentInDexCache(Handle<mirror::DexCache> dex_cache,
                                        uint32_t type_idx)
      SHARED_REQUIRES(Locks::mutator_lock_);

  bool CanAssumeStringIsPresentInDexCache(const DexFile& dex_file, uint32_t string_idx)
      REQUIRES(!Locks::mutator_lock_);

  // Are runtime access checks necessary in the compiled code?
  bool CanAccessTypeWithoutChecks(uint32_t referrer_idx,
                                  Handle<mirror::DexCache> dex_cache,
                                  uint32_t type_idx)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Are runtime access and instantiable checks necessary in the code?
  // out_is_finalizable is set to whether the type is finalizable.
  bool CanAccessInstantiableTypeWithoutChecks(uint32_t referrer_idx,
                                              Handle<mirror::DexCache> dex_cache,
                                              uint32_t type_idx,
                                              bool* out_is_finalizable)
      SHARED_REQUIRES(Locks::mutator_lock_);

  bool CanEmbedTypeInCode(const DexFile& dex_file, uint32_t type_idx,
                          bool* is_type_initialized, bool* use_direct_type_ptr,
                          uintptr_t* direct_type_ptr, bool* out_is_finalizable);

  // Query methods for the java.lang.ref.Reference class.
  bool CanEmbedReferenceTypeInCode(ClassReference* ref,
                                   bool* use_direct_type_ptr, uintptr_t* direct_type_ptr);
  uint32_t GetReferenceSlowFlagOffset() const;
  uint32_t GetReferenceDisableFlagOffset() const;

  // Get the DexCache for the
  mirror::DexCache* GetDexCache(const DexCompilationUnit* mUnit)
    SHARED_REQUIRES(Locks::mutator_lock_);

  mirror::ClassLoader* GetClassLoader(const ScopedObjectAccess& soa,
                                      const DexCompilationUnit* mUnit)
    SHARED_REQUIRES(Locks::mutator_lock_);

  // Resolve compiling method's class. Returns null on failure.
  mirror::Class* ResolveCompilingMethodsClass(
      const ScopedObjectAccess& soa, Handle<mirror::DexCache> dex_cache,
      Handle<mirror::ClassLoader> class_loader, const DexCompilationUnit* mUnit)
      SHARED_REQUIRES(Locks::mutator_lock_);

  mirror::Class* ResolveClass(
      const ScopedObjectAccess& soa, Handle<mirror::DexCache> dex_cache,
      Handle<mirror::ClassLoader> class_loader, uint16_t type_index,
      const DexCompilationUnit* mUnit)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Resolve a field. Returns null on failure, including incompatible class change.
  // NOTE: Unlike ClassLinker's ResolveField(), this method enforces is_static.
  ArtField* ResolveField(
      const ScopedObjectAccess& soa, Handle<mirror::DexCache> dex_cache,
      Handle<mirror::ClassLoader> class_loader, const DexCompilationUnit* mUnit,
      uint32_t field_idx, bool is_static)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Resolve a field with a given dex file.
  ArtField* ResolveFieldWithDexFile(
      const ScopedObjectAccess& soa, Handle<mirror::DexCache> dex_cache,
      Handle<mirror::ClassLoader> class_loader, const DexFile* dex_file,
      uint32_t field_idx, bool is_static)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Get declaration location of a resolved field.
  void GetResolvedFieldDexFileLocation(
      ArtField* resolved_field, const DexFile** declaring_dex_file,
      uint16_t* declaring_class_idx, uint16_t* declaring_field_idx)
      SHARED_REQUIRES(Locks::mutator_lock_);

  bool IsFieldVolatile(ArtField* field) SHARED_REQUIRES(Locks::mutator_lock_);
  MemberOffset GetFieldOffset(ArtField* field) SHARED_REQUIRES(Locks::mutator_lock_);

  // Find a dex cache for a dex file.
  inline mirror::DexCache* FindDexCache(const DexFile* dex_file)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Can we fast-path an IGET/IPUT access to an instance field? If yes, compute the field offset.
  std::pair<bool, bool> IsFastInstanceField(
      mirror::DexCache* dex_cache, mirror::Class* referrer_class,
      ArtField* resolved_field, uint16_t field_idx)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Can we fast-path an SGET/SPUT access to a static field? If yes, compute the type index
  // of the declaring class in the referrer's dex file.
  std::pair<bool, bool> IsFastStaticField(
      mirror::DexCache* dex_cache, mirror::Class* referrer_class,
      ArtField* resolved_field, uint16_t field_idx, uint32_t* storage_index)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Return whether the declaring class of `resolved_method` is
  // available to `referrer_class`. If this is true, compute the type
  // index of the declaring class in the referrer's dex file and
  // return it through the out argument `storage_index`; otherwise
  // return DexFile::kDexNoIndex through `storage_index`.
  bool IsClassOfStaticMethodAvailableToReferrer(mirror::DexCache* dex_cache,
                                                mirror::Class* referrer_class,
                                                ArtMethod* resolved_method,
                                                uint16_t method_idx,
                                                uint32_t* storage_index)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Is static field's in referrer's class?
  bool IsStaticFieldInReferrerClass(mirror::Class* referrer_class, ArtField* resolved_field)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Is static field's class initialized?
  bool IsStaticFieldsClassInitialized(mirror::Class* referrer_class,
                                      ArtField* resolved_field)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Resolve a method. Returns null on failure, including incompatible class change.
  ArtMethod* ResolveMethod(
      ScopedObjectAccess& soa, Handle<mirror::DexCache> dex_cache,
      Handle<mirror::ClassLoader> class_loader, const DexCompilationUnit* mUnit,
      uint32_t method_idx, InvokeType invoke_type, bool check_incompatible_class_change = true)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Get declaration location of a resolved field.
  void GetResolvedMethodDexFileLocation(
      ArtMethod* resolved_method, const DexFile** declaring_dex_file,
      uint16_t* declaring_class_idx, uint16_t* declaring_method_idx)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Get the index in the vtable of the method.
  uint16_t GetResolvedMethodVTableIndex(
      ArtMethod* resolved_method, InvokeType type)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Can we fast-path an INVOKE? If no, returns 0. If yes, returns a non-zero opaque flags value
  // for ProcessedInvoke() and computes the necessary lowering info.
  int IsFastInvoke(
      ScopedObjectAccess& soa, Handle<mirror::DexCache> dex_cache,
      Handle<mirror::ClassLoader> class_loader, const DexCompilationUnit* mUnit,
      mirror::Class* referrer_class, ArtMethod* resolved_method, InvokeType* invoke_type,
      MethodReference* target_method, const MethodReference* devirt_target,
      uintptr_t* direct_code, uintptr_t* direct_method)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Is method's class initialized for an invoke?
  // For static invokes to determine whether we need to consider potential call to <clinit>().
  // For non-static invokes, assuming a non-null reference, the class is always initialized.
  bool IsMethodsClassInitialized(mirror::Class* referrer_class, ArtMethod* resolved_method)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Get the layout of dex cache arrays for a dex file. Returns invalid layout if the
  // dex cache arrays don't have a fixed layout.
  DexCacheArraysLayout GetDexCacheArraysLayout(const DexFile* dex_file);

  void ProcessedInstanceField(bool resolved);
  void ProcessedStaticField(bool resolved, bool local);
  void ProcessedInvoke(InvokeType invoke_type, int flags);

  void ComputeFieldInfo(uint32_t field_idx, const DexCompilationUnit* mUnit,
                        const ScopedObjectAccess& soa, bool is_static,
                        ArtField** resolved_field,
                        mirror::Class** referrer_class,
                        mirror::DexCache** dex_cache)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Can we fast path instance field access? Computes field's offset and volatility.
  bool ComputeInstanceFieldInfo(uint32_t field_idx, const DexCompilationUnit* mUnit, bool is_put,
                                MemberOffset* field_offset, bool* is_volatile)
      REQUIRES(!Locks::mutator_lock_);

  ArtField* ComputeInstanceFieldInfo(uint32_t field_idx,
                                             const DexCompilationUnit* mUnit,
                                             bool is_put,
                                             const ScopedObjectAccess& soa)
      SHARED_REQUIRES(Locks::mutator_lock_);


  // Can we fastpath a interface, super class or virtual method call? Computes method's vtable
  // index.
  bool ComputeInvokeInfo(const DexCompilationUnit* mUnit, const uint32_t dex_pc,
                         bool update_stats, bool enable_devirtualization,
                         InvokeType* type, MethodReference* target_method, int* vtable_idx,
                         uintptr_t* direct_code, uintptr_t* direct_method)
      REQUIRES(!Locks::mutator_lock_);

  const VerifiedMethod* GetVerifiedMethod(const DexFile* dex_file, uint32_t method_idx) const;
  bool IsSafeCast(const DexCompilationUnit* mUnit, uint32_t dex_pc);

  bool GetSupportBootImageFixup() const {
    return support_boot_image_fixup_;
  }

  void SetSupportBootImageFixup(bool support_boot_image_fixup) {
    support_boot_image_fixup_ = support_boot_image_fixup;
  }

  void SetCompilerContext(void* compiler_context) {
    compiler_context_ = compiler_context;
  }

  void* GetCompilerContext() const {
    return compiler_context_;
  }

  size_t GetThreadCount() const {
    return parallel_thread_count_;
  }

  bool GetDumpStats() const {
    return dump_stats_;
  }

  bool GetDumpPasses() const {
    return dump_passes_;
  }

  CumulativeLogger* GetTimingsLogger() const {
    return timings_logger_;
  }

  void SetDedupeEnabled(bool dedupe_enabled) {
    compiled_method_storage_.SetDedupeEnabled(dedupe_enabled);
  }
  bool DedupeEnabled() const {
    return compiled_method_storage_.DedupeEnabled();
  }

  // Checks if class specified by type_idx is one of the image_classes_
  bool IsImageClass(const char* descriptor) const;

  // Checks whether the provided class should be compiled, i.e., is in classes_to_compile_.
  bool IsClassToCompile(const char* descriptor) const;

  // Checks whether the provided method should be compiled, i.e., is in method_to_compile_.
  bool IsMethodToCompile(const MethodReference& method_ref) const;

  // Checks whether profile guided compilation is enabled and if the method should be compiled
  // according to the profile file.
  bool ShouldCompileBasedOnProfile(const MethodReference& method_ref) const;

  // Checks whether profile guided verification is enabled and if the method should be verified
  // according to the profile file.
  bool ShouldVerifyClassBasedOnProfile(const DexFile& dex_file, uint16_t class_idx) const;

  void RecordClassStatus(ClassReference ref, mirror::Class::Status status)
      REQUIRES(!compiled_classes_lock_);

  // Checks if the specified method has been verified without failures. Returns
  // false if the method is not in the verification results (GetVerificationResults).
  bool IsMethodVerifiedWithoutFailures(uint32_t method_idx,
                                       uint16_t class_def_idx,
                                       const DexFile& dex_file) const;

  // Get memory usage during compilation.
  std::string GetMemoryUsageString(bool extended) const;

  bool IsStringTypeIndex(uint16_t type_index, const DexFile* dex_file);
  bool IsStringInit(uint32_t method_index, const DexFile* dex_file, int32_t* offset);

  void SetHadHardVerifierFailure() {
    had_hard_verifier_failure_ = true;
  }

  Compiler::Kind GetCompilerKind() {
    return compiler_kind_;
  }

  CompiledMethodStorage* GetCompiledMethodStorage() {
    return &compiled_method_storage_;
  }

  // Can we assume that the klass is loaded?
  bool CanAssumeClassIsLoaded(mirror::Class* klass)
      SHARED_REQUIRES(Locks::mutator_lock_);

  bool MayInline(const DexFile* inlined_from, const DexFile* inlined_into) const {
    if (!kIsTargetBuild) {
      return MayInlineInternal(inlined_from, inlined_into);
    }
    return true;
  }

  void MarkForDexToDexCompilation(Thread* self, const MethodReference& method_ref)
      REQUIRES(!dex_to_dex_references_lock_);

  const BitVector* GetCurrentDexToDexMethods() const {
    return current_dex_to_dex_methods_;
  }

 private:
  // Return whether the declaring class of `resolved_member` is
  // available to `referrer_class` for read or write access using two
  // Boolean values returned as a pair. If is true at least for read
  // access, compute the type index of the declaring class in the
  // referrer's dex file and return it through the out argument
  // `storage_index`; otherwise return DexFile::kDexNoIndex through
  // `storage_index`.
  template <typename ArtMember>
  std::pair<bool, bool> IsClassOfStaticMemberAvailableToReferrer(mirror::DexCache* dex_cache,
                                                                 mirror::Class* referrer_class,
                                                                 ArtMember* resolved_member,
                                                                 uint16_t member_idx,
                                                                 uint32_t* storage_index)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Can `referrer_class` access the resolved `member`?
  // Dispatch call to mirror::Class::CanAccessResolvedField or
  // mirror::Class::CanAccessResolvedMember depending on the value of
  // ArtMember.
  template <typename ArtMember>
  static bool CanAccessResolvedMember(mirror::Class* referrer_class,
                                      mirror::Class* access_to,
                                      ArtMember* member,
                                      mirror::DexCache* dex_cache,
                                      uint32_t field_idx)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Can we assume that the klass is initialized?
  bool CanAssumeClassIsInitialized(mirror::Class* klass)
      SHARED_REQUIRES(Locks::mutator_lock_);
  bool CanReferrerAssumeClassIsInitialized(mirror::Class* referrer_class, mirror::Class* klass)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // These flags are internal to CompilerDriver for collecting INVOKE resolution statistics.
  // The only external contract is that unresolved method has flags 0 and resolved non-0.
  enum {
    kBitMethodResolved = 0,
    kBitVirtualMadeDirect,
    kBitPreciseTypeDevirtualization,
    kBitDirectCallToBoot,
    kBitDirectMethodToBoot
  };
  static constexpr int kFlagMethodResolved              = 1 << kBitMethodResolved;
  static constexpr int kFlagVirtualMadeDirect           = 1 << kBitVirtualMadeDirect;
  static constexpr int kFlagPreciseTypeDevirtualization = 1 << kBitPreciseTypeDevirtualization;
  static constexpr int kFlagDirectCallToBoot            = 1 << kBitDirectCallToBoot;
  static constexpr int kFlagDirectMethodToBoot          = 1 << kBitDirectMethodToBoot;
  static constexpr int kFlagsMethodResolvedVirtualMadeDirect =
      kFlagMethodResolved | kFlagVirtualMadeDirect;
  static constexpr int kFlagsMethodResolvedPreciseTypeDevirtualization =
      kFlagsMethodResolvedVirtualMadeDirect | kFlagPreciseTypeDevirtualization;

 public:  // TODO make private or eliminate.
  // Compute constant code and method pointers when possible.
  void GetCodeAndMethodForDirectCall(/*out*/InvokeType* type,
                                     InvokeType sharp_type,
                                     bool no_guarantee_of_dex_cache_entry,
                                     const mirror::Class* referrer_class,
                                     ArtMethod* method,
                                     /*out*/int* stats_flags,
                                     MethodReference* target_method,
                                     uintptr_t* direct_code, uintptr_t* direct_method)
      SHARED_REQUIRES(Locks::mutator_lock_);

 private:
  void PreCompile(jobject class_loader,
                  const std::vector<const DexFile*>& dex_files,
                  TimingLogger* timings)
      REQUIRES(!Locks::mutator_lock_, !compiled_classes_lock_);

  void LoadImageClasses(TimingLogger* timings) REQUIRES(!Locks::mutator_lock_);

  // Attempt to resolve all type, methods, fields, and strings
  // referenced from code in the dex file following PathClassLoader
  // ordering semantics.
  void Resolve(jobject class_loader,
               const std::vector<const DexFile*>& dex_files,
               TimingLogger* timings)
      REQUIRES(!Locks::mutator_lock_);
  void ResolveDexFile(jobject class_loader,
                      const DexFile& dex_file,
                      const std::vector<const DexFile*>& dex_files,
                      ThreadPool* thread_pool,
                      size_t thread_count,
                      TimingLogger* timings)
      REQUIRES(!Locks::mutator_lock_);

  void Verify(jobject class_loader,
              const std::vector<const DexFile*>& dex_files,
              TimingLogger* timings);
  void VerifyDexFile(jobject class_loader,
                     const DexFile& dex_file,
                     const std::vector<const DexFile*>& dex_files,
                     ThreadPool* thread_pool,
                     size_t thread_count,
                     TimingLogger* timings)
      REQUIRES(!Locks::mutator_lock_);

  void SetVerified(jobject class_loader,
                   const std::vector<const DexFile*>& dex_files,
                   TimingLogger* timings);
  void SetVerifiedDexFile(jobject class_loader,
                          const DexFile& dex_file,
                          const std::vector<const DexFile*>& dex_files,
                          ThreadPool* thread_pool,
                          size_t thread_count,
                          TimingLogger* timings)
      REQUIRES(!Locks::mutator_lock_);

  void InitializeClasses(jobject class_loader,
                         const std::vector<const DexFile*>& dex_files,
                         TimingLogger* timings)
      REQUIRES(!Locks::mutator_lock_, !compiled_classes_lock_);
  void InitializeClasses(jobject class_loader,
                         const DexFile& dex_file,
                         const std::vector<const DexFile*>& dex_files,
                         TimingLogger* timings)
      REQUIRES(!Locks::mutator_lock_, !compiled_classes_lock_);

  void UpdateImageClasses(TimingLogger* timings) REQUIRES(!Locks::mutator_lock_);
  static void FindClinitImageClassesCallback(mirror::Object* object, void* arg)
      SHARED_REQUIRES(Locks::mutator_lock_);

  void Compile(jobject class_loader,
               const std::vector<const DexFile*>& dex_files,
               TimingLogger* timings) REQUIRES(!dex_to_dex_references_lock_);
  void CompileDexFile(jobject class_loader,
                      const DexFile& dex_file,
                      const std::vector<const DexFile*>& dex_files,
                      ThreadPool* thread_pool,
                      size_t thread_count,
                      TimingLogger* timings)
      REQUIRES(!Locks::mutator_lock_);

  bool MayInlineInternal(const DexFile* inlined_from, const DexFile* inlined_into) const;

  void InitializeThreadPools();
  void FreeThreadPools();
  void CheckThreadPools();

  bool RequiresConstructorBarrier(const DexFile& dex_file, uint16_t class_def_idx) const;

  const CompilerOptions* const compiler_options_;
  VerificationResults* const verification_results_;
  DexFileToMethodInlinerMap* const method_inliner_map_;

  std::unique_ptr<Compiler> compiler_;
  Compiler::Kind compiler_kind_;

  const InstructionSet instruction_set_;
  const InstructionSetFeatures* const instruction_set_features_;

  // All class references that require constructor barriers. If the class reference is not in the
  // set then the result has not yet been computed.
  mutable ReaderWriterMutex requires_constructor_barrier_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  std::map<ClassReference, bool> requires_constructor_barrier_
      GUARDED_BY(requires_constructor_barrier_lock_);

  typedef SafeMap<const ClassReference, CompiledClass*> ClassTable;
  // All class references that this compiler has compiled.
  mutable Mutex compiled_classes_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  ClassTable compiled_classes_ GUARDED_BY(compiled_classes_lock_);

  typedef SafeMap<const MethodReference, CompiledMethod*, MethodReferenceComparator> MethodTable;

 public:
  // Lock is public so that non-members can have lock annotations.
  mutable Mutex compiled_methods_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;

 private:
  // All method references that this compiler has compiled.
  MethodTable compiled_methods_ GUARDED_BY(compiled_methods_lock_);
  // Number of non-relative patches in all compiled methods. These patches need space
  // in the .oat_patches ELF section if requested in the compiler options.
  size_t non_relative_linker_patch_count_ GUARDED_BY(compiled_methods_lock_);

  const bool boot_image_;
  const bool app_image_;

  // If image_ is true, specifies the classes that will be included in the image.
  // Note if image_classes_ is null, all classes are included in the image.
  std::unique_ptr<std::unordered_set<std::string>> image_classes_;

  // Specifies the classes that will be compiled. Note that if classes_to_compile_ is null,
  // all classes are eligible for compilation (duplication filters etc. will still apply).
  // This option may be restricted to the boot image, depending on a flag in the implementation.
  std::unique_ptr<std::unordered_set<std::string>> classes_to_compile_;

  // Specifies the methods that will be compiled. Note that if methods_to_compile_ is null,
  // all methods are eligible for compilation (compilation filters etc. will still apply).
  // This option may be restricted to the boot image, depending on a flag in the implementation.
  std::unique_ptr<std::unordered_set<std::string>> methods_to_compile_;

  bool had_hard_verifier_failure_;

  // A thread pool that can (potentially) run tasks in parallel.
  std::unique_ptr<ThreadPool> parallel_thread_pool_;
  size_t parallel_thread_count_;

  // A thread pool that guarantees running single-threaded on the main thread.
  std::unique_ptr<ThreadPool> single_thread_pool_;

  class AOTCompilationStats;
  std::unique_ptr<AOTCompilationStats> stats_;

  bool dump_stats_;
  const bool dump_passes_;

  CumulativeLogger* const timings_logger_;

  typedef void (*CompilerCallbackFn)(CompilerDriver& driver);
  typedef MutexLock* (*CompilerMutexLockFn)(CompilerDriver& driver);

  void* compiler_context_;

  bool support_boot_image_fixup_;

  // List of dex files that will be stored in the oat file.
  const std::vector<const DexFile*>* dex_files_for_oat_file_;

  CompiledMethodStorage compiled_method_storage_;

  // Info for profile guided compilation.
  const ProfileCompilationInfo* const profile_compilation_info_;

  size_t max_arena_alloc_;

  // Data for delaying dex-to-dex compilation.
  Mutex dex_to_dex_references_lock_;
  // In the first phase, dex_to_dex_references_ collects methods for dex-to-dex compilation.
  class DexFileMethodSet;
  std::vector<DexFileMethodSet> dex_to_dex_references_ GUARDED_BY(dex_to_dex_references_lock_);
  // In the second phase, current_dex_to_dex_methods_ points to the BitVector with method
  // indexes for dex-to-dex compilation in the current dex file.
  const BitVector* current_dex_to_dex_methods_;

  friend class CompileClassVisitor;
  DISALLOW_COPY_AND_ASSIGN(CompilerDriver);
};

}  // namespace art

#endif  // ART_COMPILER_DRIVER_COMPILER_DRIVER_H_
