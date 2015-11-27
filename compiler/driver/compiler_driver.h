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
#include "invoke_type.h"
#include "method_reference.h"
#include "mirror/class.h"  // For mirror::Class::Status.
#include "os.h"
#include "profiler.h"
#include "runtime.h"
#include "safe_map.h"
#include "thread_pool.h"
#include "utils/array_ref.h"
#include "utils/dedupe_set.h"
#include "utils/dex_cache_arrays_layout.h"
#include "utils/swap_space.h"

namespace art {

namespace mirror {
class DexCache;
}  // namespace mirror

namespace verifier {
class MethodVerifier;
}  // namespace verifier

class CompiledClass;
class CompiledMethod;
class CompilerOptions;
class DexCompilationUnit;
class DexFileToMethodInlinerMap;
struct InlineIGetIPutData;
class InstructionSetFeatures;
class OatWriter;
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

enum DexToDexCompilationLevel {
  kDontDexToDexCompile,   // Only meaning wrt image time interpretation.
  kRequired,              // Dex-to-dex compilation required for correctness.
  kOptimize               // Perform required transformation and peep-hole optimizations.
};
std::ostream& operator<<(std::ostream& os, const DexToDexCompilationLevel& rhs);

static constexpr bool kUseMurmur3Hash = true;

class CompilerDriver {
 public:
  // Create a compiler targeting the requested "instruction_set".
  // "image" should be true if image specific optimizations should be
  // enabled.  "image_classes" lets the compiler know what classes it
  // can assume will be in the image, with null implying all available
  // classes.
  explicit CompilerDriver(const CompilerOptions* compiler_options,
                          VerificationResults* verification_results,
                          DexFileToMethodInlinerMap* method_inliner_map,
                          Compiler::Kind compiler_kind,
                          InstructionSet instruction_set,
                          const InstructionSetFeatures* instruction_set_features,
                          bool image, std::unordered_set<std::string>* image_classes,
                          std::unordered_set<std::string>* compiled_classes,
                          std::unordered_set<std::string>* compiled_methods,
                          size_t thread_count, bool dump_stats, bool dump_passes,
                          const std::string& dump_cfg_file_name,
                          CumulativeLogger* timer, int swap_fd,
                          const std::string& profile_file);

  ~CompilerDriver();

  void CompileAll(jobject class_loader, const std::vector<const DexFile*>& dex_files,
                  TimingLogger* timings)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  CompiledMethod* CompileMethod(Thread* self, ArtMethod*)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) WARN_UNUSED;

  // Compile a single Method.
  void CompileOne(Thread* self, ArtMethod* method, TimingLogger* timings)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  VerificationResults* GetVerificationResults() const {
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

  bool ProfilePresent() const {
    return profile_present_;
  }

  // Are we compiling and creating an image file?
  bool IsImage() const {
    return image_;
  }

  const std::unordered_set<std::string>* GetImageClasses() const {
    return image_classes_.get();
  }

  // Generate the trampolines that are invoked by unresolved direct methods.
  const std::vector<uint8_t>* CreateInterpreterToInterpreterBridge() const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  const std::vector<uint8_t>* CreateInterpreterToCompiledCodeBridge() const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  const std::vector<uint8_t>* CreateJniDlsymLookup() const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  const std::vector<uint8_t>* CreateQuickGenericJniTrampoline() const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  const std::vector<uint8_t>* CreateQuickImtConflictTrampoline() const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  const std::vector<uint8_t>* CreateQuickResolutionTrampoline() const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  const std::vector<uint8_t>* CreateQuickToInterpreterBridge() const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  CompiledClass* GetCompiledClass(ClassReference ref) const
      LOCKS_EXCLUDED(compiled_classes_lock_);

  CompiledMethod* GetCompiledMethod(MethodReference ref) const
      LOCKS_EXCLUDED(compiled_methods_lock_);
  size_t GetNonRelativeLinkerPatchCount() const
      LOCKS_EXCLUDED(compiled_methods_lock_);

  // Remove and delete a compiled method.
  void RemoveCompiledMethod(const MethodReference& method_ref);

  void AddRequiresConstructorBarrier(Thread* self, const DexFile* dex_file,
                                     uint16_t class_def_index);
  bool RequiresConstructorBarrier(Thread* self, const DexFile* dex_file,
                                  uint16_t class_def_index) const;

  // Callbacks from compiler to see what runtime checks must be generated.

  bool CanAssumeTypeIsPresentInDexCache(const DexFile& dex_file, uint32_t type_idx);

  bool CanAssumeStringIsPresentInDexCache(const DexFile& dex_file, uint32_t string_idx)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  // Are runtime access checks necessary in the compiled code?
  bool CanAccessTypeWithoutChecks(uint32_t referrer_idx, const DexFile& dex_file,
                                  uint32_t type_idx, bool* type_known_final = nullptr,
                                  bool* type_known_abstract = nullptr,
                                  bool* equals_referrers_class = nullptr)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  // Are runtime access and instantiable checks necessary in the code?
  bool CanAccessInstantiableTypeWithoutChecks(uint32_t referrer_idx, const DexFile& dex_file,
                                              uint32_t type_idx)
     LOCKS_EXCLUDED(Locks::mutator_lock_);

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
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  mirror::ClassLoader* GetClassLoader(ScopedObjectAccess& soa, const DexCompilationUnit* mUnit)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Resolve compiling method's class. Returns null on failure.
  mirror::Class* ResolveCompilingMethodsClass(
      const ScopedObjectAccess& soa, Handle<mirror::DexCache> dex_cache,
      Handle<mirror::ClassLoader> class_loader, const DexCompilationUnit* mUnit)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  mirror::Class* ResolveClass(
      const ScopedObjectAccess& soa, Handle<mirror::DexCache> dex_cache,
      Handle<mirror::ClassLoader> class_loader, uint16_t type_index,
      const DexCompilationUnit* mUnit)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Resolve a field. Returns null on failure, including incompatible class change.
  // NOTE: Unlike ClassLinker's ResolveField(), this method enforces is_static.
  ArtField* ResolveField(
      const ScopedObjectAccess& soa, Handle<mirror::DexCache> dex_cache,
      Handle<mirror::ClassLoader> class_loader, const DexCompilationUnit* mUnit,
      uint32_t field_idx, bool is_static)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Resolve a field with a given dex file.
  ArtField* ResolveFieldWithDexFile(
      const ScopedObjectAccess& soa, Handle<mirror::DexCache> dex_cache,
      Handle<mirror::ClassLoader> class_loader, const DexFile* dex_file,
      uint32_t field_idx, bool is_static)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Get declaration location of a resolved field.
  void GetResolvedFieldDexFileLocation(
      ArtField* resolved_field, const DexFile** declaring_dex_file,
      uint16_t* declaring_class_idx, uint16_t* declaring_field_idx)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  bool IsFieldVolatile(ArtField* field) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  MemberOffset GetFieldOffset(ArtField* field) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Find a dex cache for a dex file.
  inline mirror::DexCache* FindDexCache(const DexFile* dex_file)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Can we fast-path an IGET/IPUT access to an instance field? If yes, compute the field offset.
  std::pair<bool, bool> IsFastInstanceField(
      mirror::DexCache* dex_cache, mirror::Class* referrer_class,
      ArtField* resolved_field, uint16_t field_idx)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Can we fast-path an SGET/SPUT access to a static field? If yes, compute the type index
  // of the declaring class in the referrer's dex file.
  std::pair<bool, bool> IsFastStaticField(
      mirror::DexCache* dex_cache, mirror::Class* referrer_class,
      ArtField* resolved_field, uint16_t field_idx, uint32_t* storage_index)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

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
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Is static field's in referrer's class?
  bool IsStaticFieldInReferrerClass(mirror::Class* referrer_class, ArtField* resolved_field)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Is static field's class initialized?
  bool IsStaticFieldsClassInitialized(mirror::Class* referrer_class,
                                      ArtField* resolved_field)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Resolve a method. Returns null on failure, including incompatible class change.
  ArtMethod* ResolveMethod(
      ScopedObjectAccess& soa, Handle<mirror::DexCache> dex_cache,
      Handle<mirror::ClassLoader> class_loader, const DexCompilationUnit* mUnit,
      uint32_t method_idx, InvokeType invoke_type, bool check_incompatible_class_change = true)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Get declaration location of a resolved field.
  void GetResolvedMethodDexFileLocation(
      ArtMethod* resolved_method, const DexFile** declaring_dex_file,
      uint16_t* declaring_class_idx, uint16_t* declaring_method_idx)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Get the index in the vtable of the method.
  uint16_t GetResolvedMethodVTableIndex(
      ArtMethod* resolved_method, InvokeType type)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Can we fast-path an INVOKE? If no, returns 0. If yes, returns a non-zero opaque flags value
  // for ProcessedInvoke() and computes the necessary lowering info.
  int IsFastInvoke(
      ScopedObjectAccess& soa, Handle<mirror::DexCache> dex_cache,
      Handle<mirror::ClassLoader> class_loader, const DexCompilationUnit* mUnit,
      mirror::Class* referrer_class, ArtMethod* resolved_method, InvokeType* invoke_type,
      MethodReference* target_method, const MethodReference* devirt_target,
      uintptr_t* direct_code, uintptr_t* direct_method, bool is_quickened = false)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Is method's class initialized for an invoke?
  // For static invokes to determine whether we need to consider potential call to <clinit>().
  // For non-static invokes, assuming a non-null reference, the class is always initialized.
  bool IsMethodsClassInitialized(mirror::Class* referrer_class, ArtMethod* resolved_method)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

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
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Can we fast path instance field access? Computes field's offset and volatility.
  bool ComputeInstanceFieldInfo(uint32_t field_idx, const DexCompilationUnit* mUnit, bool is_put,
                                MemberOffset* field_offset, bool* is_volatile)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  ArtField* ComputeInstanceFieldInfo(uint32_t field_idx,
                                             const DexCompilationUnit* mUnit,
                                             bool is_put,
                                             const ScopedObjectAccess& soa)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);


  // Can we fastpath static field access? Computes field's offset, volatility and whether the
  // field is within the referrer (which can avoid checking class initialization).
  bool ComputeStaticFieldInfo(uint32_t field_idx, const DexCompilationUnit* mUnit, bool is_put,
                              MemberOffset* field_offset, uint32_t* storage_index,
                              bool* is_referrers_class, bool* is_volatile, bool* is_initialized,
                              Primitive::Type* type)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  // Can we fastpath a interface, super class or virtual method call? Computes method's vtable
  // index.
  bool ComputeInvokeInfo(const DexCompilationUnit* mUnit, const uint32_t dex_pc,
                         bool update_stats, bool enable_devirtualization,
                         InvokeType* type, MethodReference* target_method, int* vtable_idx,
                         uintptr_t* direct_code, uintptr_t* direct_method)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  const VerifiedMethod* GetVerifiedMethod(const DexFile* dex_file, uint32_t method_idx) const;
  bool IsSafeCast(const DexCompilationUnit* mUnit, uint32_t dex_pc);

  bool GetSupportBootImageFixup() const {
    return support_boot_image_fixup_;
  }

  void SetSupportBootImageFixup(bool support_boot_image_fixup) {
    support_boot_image_fixup_ = support_boot_image_fixup;
  }

  SwapAllocator<void>& GetSwapSpaceAllocator() {
    return *swap_space_allocator_.get();
  }

  bool WriteElf(const std::string& android_root,
                bool is_host,
                const std::vector<const DexFile*>& dex_files,
                OatWriter* oat_writer,
                File* file);

  void SetCompilerContext(void* compiler_context) {
    compiler_context_ = compiler_context;
  }

  void* GetCompilerContext() const {
    return compiler_context_;
  }

  size_t GetThreadCount() const {
    return thread_count_;
  }

  bool GetDumpStats() const {
    return dump_stats_;
  }

  bool GetDumpPasses() const {
    return dump_passes_;
  }

  const std::string& GetDumpCfgFileName() const {
    return dump_cfg_file_name_;
  }

  CumulativeLogger* GetTimingsLogger() const {
    return timings_logger_;
  }

  void SetDedupeEnabled(bool dedupe_enabled) {
    dedupe_enabled_ = dedupe_enabled;
  }
  bool DedupeEnabled() const {
    return dedupe_enabled_;
  }

  // Checks if class specified by type_idx is one of the image_classes_
  bool IsImageClass(const char* descriptor) const;

  // Checks whether the provided class should be compiled, i.e., is in classes_to_compile_.
  bool IsClassToCompile(const char* descriptor) const;

  // Checks whether the provided method should be compiled, i.e., is in method_to_compile_.
  bool IsMethodToCompile(const MethodReference& method_ref) const;

  void RecordClassStatus(ClassReference ref, mirror::Class::Status status)
      LOCKS_EXCLUDED(compiled_classes_lock_);

  // Checks if the specified method has been verified without failures. Returns
  // false if the method is not in the verification results (GetVerificationResults).
  bool IsMethodVerifiedWithoutFailures(uint32_t method_idx,
                                       uint16_t class_def_idx,
                                       const DexFile& dex_file) const;

  SwapVector<uint8_t>* DeduplicateCode(const ArrayRef<const uint8_t>& code);
  SwapSrcMap* DeduplicateSrcMappingTable(const ArrayRef<SrcMapElem>& src_map);
  SwapVector<uint8_t>* DeduplicateMappingTable(const ArrayRef<const uint8_t>& code);
  SwapVector<uint8_t>* DeduplicateVMapTable(const ArrayRef<const uint8_t>& code);
  SwapVector<uint8_t>* DeduplicateGCMap(const ArrayRef<const uint8_t>& code);
  SwapVector<uint8_t>* DeduplicateCFIInfo(const ArrayRef<const uint8_t>& cfi_info);

  // Should the compiler run on this method given profile information?
  bool SkipCompilation(const std::string& method_name);

  // Get memory usage during compilation.
  std::string GetMemoryUsageString(bool extended) const;

  bool IsStringTypeIndex(uint16_t type_index, const DexFile* dex_file);
  bool IsStringInit(uint32_t method_index, const DexFile* dex_file, int32_t* offset);

  void SetHadHardVerifierFailure() {
    had_hard_verifier_failure_ = true;
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
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

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
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Can we assume that the klass is initialized?
  bool CanAssumeClassIsInitialized(mirror::Class* klass)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  bool CanReferrerAssumeClassIsInitialized(mirror::Class* referrer_class, mirror::Class* klass)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Can we assume that the klass is loaded?
  bool CanAssumeClassIsLoaded(mirror::Class* klass)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

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
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

 private:
  DexToDexCompilationLevel GetDexToDexCompilationlevel(
      Thread* self, Handle<mirror::ClassLoader> class_loader, const DexFile& dex_file,
      const DexFile::ClassDef& class_def) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void PreCompile(jobject class_loader, const std::vector<const DexFile*>& dex_files,
                  ThreadPool* thread_pool, TimingLogger* timings)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  void LoadImageClasses(TimingLogger* timings);

  // Attempt to resolve all type, methods, fields, and strings
  // referenced from code in the dex file following PathClassLoader
  // ordering semantics.
  void Resolve(jobject class_loader, const std::vector<const DexFile*>& dex_files,
               ThreadPool* thread_pool, TimingLogger* timings)
      LOCKS_EXCLUDED(Locks::mutator_lock_);
  void ResolveDexFile(jobject class_loader, const DexFile& dex_file,
                      const std::vector<const DexFile*>& dex_files,
                      ThreadPool* thread_pool, TimingLogger* timings)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  void Verify(jobject class_loader, const std::vector<const DexFile*>& dex_files,
              ThreadPool* thread_pool, TimingLogger* timings);
  void VerifyDexFile(jobject class_loader, const DexFile& dex_file,
                     const std::vector<const DexFile*>& dex_files,
                     ThreadPool* thread_pool, TimingLogger* timings)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  void SetVerified(jobject class_loader, const std::vector<const DexFile*>& dex_files,
                   ThreadPool* thread_pool, TimingLogger* timings);
  void SetVerifiedDexFile(jobject class_loader, const DexFile& dex_file,
                          const std::vector<const DexFile*>& dex_files,
                          ThreadPool* thread_pool, TimingLogger* timings)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  void InitializeClasses(jobject class_loader, const std::vector<const DexFile*>& dex_files,
                         ThreadPool* thread_pool, TimingLogger* timings)
      LOCKS_EXCLUDED(Locks::mutator_lock_);
  void InitializeClasses(jobject class_loader, const DexFile& dex_file,
                         const std::vector<const DexFile*>& dex_files,
                         ThreadPool* thread_pool, TimingLogger* timings)
      LOCKS_EXCLUDED(Locks::mutator_lock_, compiled_classes_lock_);

  void UpdateImageClasses(TimingLogger* timings) LOCKS_EXCLUDED(Locks::mutator_lock_);
  static void FindClinitImageClassesCallback(mirror::Object* object, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void Compile(jobject class_loader, const std::vector<const DexFile*>& dex_files,
               ThreadPool* thread_pool, TimingLogger* timings);
  void CompileDexFile(jobject class_loader, const DexFile& dex_file,
                      const std::vector<const DexFile*>& dex_files,
                      ThreadPool* thread_pool, TimingLogger* timings)
      LOCKS_EXCLUDED(Locks::mutator_lock_);
  void CompileMethod(Thread* self, const DexFile::CodeItem* code_item, uint32_t access_flags,
                     InvokeType invoke_type, uint16_t class_def_idx, uint32_t method_idx,
                     jobject class_loader, const DexFile& dex_file,
                     DexToDexCompilationLevel dex_to_dex_compilation_level,
                     bool compilation_enabled)
      LOCKS_EXCLUDED(compiled_methods_lock_);

  static void CompileClass(const ParallelCompilationManager* context, size_t class_def_index)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  // Swap pool and allocator used for native allocations. May be file-backed. Needs to be first
  // as other fields rely on this.
  std::unique_ptr<SwapSpace> swap_space_;
  std::unique_ptr<SwapAllocator<void> > swap_space_allocator_;

  ProfileFile profile_file_;
  bool profile_present_;

  const CompilerOptions* const compiler_options_;
  VerificationResults* const verification_results_;
  DexFileToMethodInlinerMap* const method_inliner_map_;

  std::unique_ptr<Compiler> compiler_;
  Compiler::Kind compiler_kind_;

  const InstructionSet instruction_set_;
  const InstructionSetFeatures* const instruction_set_features_;

  // All class references that require
  mutable ReaderWriterMutex freezing_constructor_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  std::set<ClassReference> freezing_constructor_classes_ GUARDED_BY(freezing_constructor_lock_);

  typedef SafeMap<const ClassReference, CompiledClass*> ClassTable;
  // All class references that this compiler has compiled.
  mutable Mutex compiled_classes_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  ClassTable compiled_classes_ GUARDED_BY(compiled_classes_lock_);

  typedef SafeMap<const MethodReference, CompiledMethod*, MethodReferenceComparator> MethodTable;
  // All method references that this compiler has compiled.
  mutable Mutex compiled_methods_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  MethodTable compiled_methods_ GUARDED_BY(compiled_methods_lock_);
  // Number of non-relative patches in all compiled methods. These patches need space
  // in the .oat_patches ELF section if requested in the compiler options.
  size_t non_relative_linker_patch_count_ GUARDED_BY(compiled_methods_lock_);

  const bool image_;

  // If image_ is true, specifies the classes that will be included in
  // the image. Note if image_classes_ is null, all classes are
  // included in the image.
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

  size_t thread_count_;

  class AOTCompilationStats;
  std::unique_ptr<AOTCompilationStats> stats_;

  bool dedupe_enabled_;
  bool dump_stats_;
  const bool dump_passes_;
  const std::string& dump_cfg_file_name_;

  CumulativeLogger* const timings_logger_;

  typedef void (*CompilerCallbackFn)(CompilerDriver& driver);
  typedef MutexLock* (*CompilerMutexLockFn)(CompilerDriver& driver);

  typedef void (*DexToDexCompilerFn)(CompilerDriver& driver,
                                     const DexFile::CodeItem* code_item,
                                     uint32_t access_flags, InvokeType invoke_type,
                                     uint32_t class_dex_idx, uint32_t method_idx,
                                     jobject class_loader, const DexFile& dex_file,
                                     DexToDexCompilationLevel dex_to_dex_compilation_level);
  DexToDexCompilerFn dex_to_dex_compiler_;

  void* compiler_context_;

  bool support_boot_image_fixup_;

  // DeDuplication data structures, these own the corresponding byte arrays.
  template <typename ContentType>
  class DedupeHashFunc {
   public:
    size_t operator()(const ArrayRef<ContentType>& array) const {
      const uint8_t* data = reinterpret_cast<const uint8_t*>(array.data());
      static_assert(IsPowerOfTwo(sizeof(ContentType)),
          "ContentType is not power of two, don't know whether array layout is as assumed");
      uint32_t len = sizeof(ContentType) * array.size();
      if (kUseMurmur3Hash) {
        static constexpr uint32_t c1 = 0xcc9e2d51;
        static constexpr uint32_t c2 = 0x1b873593;
        static constexpr uint32_t r1 = 15;
        static constexpr uint32_t r2 = 13;
        static constexpr uint32_t m = 5;
        static constexpr uint32_t n = 0xe6546b64;

        uint32_t hash = 0;

        const int nblocks = len / 4;
        typedef __attribute__((__aligned__(1))) uint32_t unaligned_uint32_t;
        const unaligned_uint32_t *blocks = reinterpret_cast<const uint32_t*>(data);
        int i;
        for (i = 0; i < nblocks; i++) {
          uint32_t k = blocks[i];
          k *= c1;
          k = (k << r1) | (k >> (32 - r1));
          k *= c2;

          hash ^= k;
          hash = ((hash << r2) | (hash >> (32 - r2))) * m + n;
        }

        const uint8_t *tail = reinterpret_cast<const uint8_t*>(data + nblocks * 4);
        uint32_t k1 = 0;

        switch (len & 3) {
          case 3:
            k1 ^= tail[2] << 16;
            FALLTHROUGH_INTENDED;
          case 2:
            k1 ^= tail[1] << 8;
            FALLTHROUGH_INTENDED;
          case 1:
            k1 ^= tail[0];

            k1 *= c1;
            k1 = (k1 << r1) | (k1 >> (32 - r1));
            k1 *= c2;
            hash ^= k1;
        }

        hash ^= len;
        hash ^= (hash >> 16);
        hash *= 0x85ebca6b;
        hash ^= (hash >> 13);
        hash *= 0xc2b2ae35;
        hash ^= (hash >> 16);

        return hash;
      } else {
        size_t hash = 0x811c9dc5;
        for (uint32_t i = 0; i < len; ++i) {
          hash = (hash * 16777619) ^ data[i];
        }
        hash += hash << 13;
        hash ^= hash >> 7;
        hash += hash << 3;
        hash ^= hash >> 17;
        hash += hash << 5;
        return hash;
      }
    }
  };

  DedupeSet<ArrayRef<const uint8_t>,
            SwapVector<uint8_t>, size_t, DedupeHashFunc<const uint8_t>, 4> dedupe_code_;
  DedupeSet<ArrayRef<SrcMapElem>,
            SwapSrcMap, size_t, DedupeHashFunc<SrcMapElem>, 4> dedupe_src_mapping_table_;
  DedupeSet<ArrayRef<const uint8_t>,
            SwapVector<uint8_t>, size_t, DedupeHashFunc<const uint8_t>, 4> dedupe_mapping_table_;
  DedupeSet<ArrayRef<const uint8_t>,
            SwapVector<uint8_t>, size_t, DedupeHashFunc<const uint8_t>, 4> dedupe_vmap_table_;
  DedupeSet<ArrayRef<const uint8_t>,
            SwapVector<uint8_t>, size_t, DedupeHashFunc<const uint8_t>, 4> dedupe_gc_map_;
  DedupeSet<ArrayRef<const uint8_t>,
            SwapVector<uint8_t>, size_t, DedupeHashFunc<const uint8_t>, 4> dedupe_cfi_info_;

  DISALLOW_COPY_AND_ASSIGN(CompilerDriver);
};

}  // namespace art

#endif  // ART_COMPILER_DRIVER_COMPILER_DRIVER_H_
