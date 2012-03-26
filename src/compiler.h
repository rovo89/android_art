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

#ifndef ART_SRC_COMPILER_H_
#define ART_SRC_COMPILER_H_

#include <map>
#include <set>
#include <string>

#include "compiled_class.h"
#include "compiled_method.h"
#include "constants.h"
#include "dex_cache.h"
#include "dex_file.h"
#include "oat_file.h"
#include "object.h"
#include "runtime.h"

namespace art {

class AOTCompilationStats;
class Context;
class OatCompilationUnit;
class TimingLogger;

class Compiler {
 public:
  // Create a compiler targeting the requested "instruction_set".
  // "image" should be true if image specific optimizations should be
  // enabled.  "image_classes" lets the compiler know what classes it
  // can assume will be in the image, with NULL implying all available
  // classes.
  explicit Compiler(InstructionSet instruction_set, bool image, size_t thread_count,
                    bool support_debugging, const std::set<std::string>* image_classes,
                    bool dump_stats, bool dump_timings);

  ~Compiler();

  void CompileAll(const ClassLoader* class_loader,
                  const std::vector<const DexFile*>& dex_files);

  // Compile a single Method
  void CompileOne(const Method* method);

  bool IsDebuggingSupported() {
    return support_debugging_;
  }

  InstructionSet GetInstructionSet() const {
    return instruction_set_;
  }

  bool IsImage() const {
    return image_;
  }

  // Stub to throw AbstractMethodError
  static ByteArray* CreateAbstractMethodErrorStub(InstructionSet instruction_set);


  // Generate the trampoline that's invoked by unresolved direct methods
  static ByteArray* CreateResolutionStub(InstructionSet instruction_set,
                                         Runtime::TrampolineType type);

  static ByteArray* CreateJniDlsymLookupStub(InstructionSet instruction_set);

  // A class is uniquely located by its DexFile and the class_defs_ table index into that DexFile
  typedef std::pair<const DexFile*, uint32_t> ClassReference;

  CompiledClass* GetCompiledClass(ClassReference ref) const;

  // A method is uniquely located by its DexFile and the method_ids_ table index into that DexFile
  typedef std::pair<const DexFile*, uint32_t> MethodReference;

  CompiledMethod* GetCompiledMethod(MethodReference ref) const;

  const CompiledInvokeStub* FindInvokeStub(bool is_static, const char* shorty) const;

  // Callbacks from OAT/ART compiler to see what runtime checks must be generated

  bool CanAssumeTypeIsPresentInDexCache(const DexCache* dex_cache, uint32_t type_idx);

  bool CanAssumeStringIsPresentInDexCache(const DexCache* dex_cache, uint32_t string_idx);

  // Are runtime access checks necessary in the compiled code?
  bool CanAccessTypeWithoutChecks(uint32_t referrer_idx, const DexCache* dex_cache,
                                  const DexFile& dex_file, uint32_t type_idx);

  // Are runtime access and instantiable checks necessary in the code?
  bool CanAccessInstantiableTypeWithoutChecks(uint32_t referrer_idx, const DexCache* dex_cache,
                                              const DexFile& dex_file, uint32_t type_idx);

  // Can we fast path instance field access? Computes field's offset and volatility
  bool ComputeInstanceFieldInfo(uint32_t field_idx, OatCompilationUnit* mUnit,
                                int& field_offset, bool& is_volatile, bool is_put);

  // Can we fastpath static field access? Computes field's offset, volatility and whether the
  // field is within the referrer (which can avoid checking class initialization)
  bool ComputeStaticFieldInfo(uint32_t field_idx, OatCompilationUnit* mUnit,
                              int& field_offset, int& ssb_index,
                              bool& is_referrers_class, bool& is_volatile, bool is_put);

  // Can we fastpath a interface, super class or virtual method call? Computes method's vtable index
  bool ComputeInvokeInfo(uint32_t method_idx, OatCompilationUnit* mUnit, InvokeType& type,
                         int& vtable_idx, uintptr_t& direct_code, uintptr_t& direct_method);

  // Record patch information for later fix up
  void AddCodePatch(DexCache* dex_cache,
                    const DexFile* dex_file,
                    uint32_t referrer_method_idx,
                    uint32_t referrer_access_flags,
                    uint32_t target_method_idx,
                    bool target_is_direct,
                    size_t literal_offset);
  void AddMethodPatch(DexCache* dex_cache,
                      const DexFile* dex_file,
                      uint32_t referrer_method_idx,
                      uint32_t referrer_access_flags,
                      uint32_t target_method_idx,
                      bool target_is_direct,
                      size_t literal_offset);

#if defined(ART_USE_LLVM_COMPILER)
  void SetElfFileName(std::string const& filename);
  void SetBitcodeFileName(std::string const& filename);
  std::string const& GetElfFileName();
  std::string const& GetBitcodeFileName();
#endif

  void SetCompilerContext(void* compiler_context) {
    compiler_context_ = compiler_context;
  }

  void* GetCompilerContext() const {
    return compiler_context_;
  }

  class PatchInformation {
   public:
    DexCache* GetDexCache() const {
      return dex_cache_;
    }
    const DexFile& GetDexFile() const {
      return *dex_file_;
    }
    uint32_t GetReferrerMethodIdx() const {
      return referrer_method_idx_;
    }
    bool GetReferrerIsDirect() const {
      return referrer_is_direct_;
    }
    uint32_t GetTargetMethodIdx() const {
      return target_method_idx_;
    }
    bool GetTargetIsDirect() const {
      return target_is_direct_;
    }
    size_t GetLiteralOffset() const {;
      return literal_offset_;
    }

   private:
    PatchInformation(DexCache* dex_cache,
                     const DexFile* dex_file,
                     uint32_t referrer_method_idx,
                     uint32_t referrer_access_flags,
                     uint32_t target_method_idx,
                     uint32_t target_is_direct,
                     size_t literal_offset)
      : dex_cache_(dex_cache),
        dex_file_(dex_file),
        referrer_method_idx_(referrer_method_idx),
        referrer_is_direct_(Method::IsDirect(referrer_access_flags)),
        target_method_idx_(target_method_idx),
        target_is_direct_(target_is_direct),
        literal_offset_(literal_offset) {
      CHECK(dex_file_ != NULL);
    }

    DexCache* dex_cache_;
    const DexFile* dex_file_;
    uint32_t referrer_method_idx_;
    bool referrer_is_direct_;
    uint32_t target_method_idx_;
    bool target_is_direct_;
    size_t literal_offset_;

    friend class Compiler;
    DISALLOW_COPY_AND_ASSIGN(PatchInformation);
  };

  const std::vector<const PatchInformation*>& GetCodeToPatch() const {
    return code_to_patch_;
  }
  const std::vector<const PatchInformation*>& GetMethodsToPatch() const {
    return methods_to_patch_;
  }

 private:

  // Compute constant code and method pointers when possible
  void GetCodeAndMethodForDirectCall(InvokeType type, InvokeType sharp_type, Method* method,
                                     uintptr_t& direct_code, uintptr_t& direct_method);

  // Checks if class specified by type_idx is one of the image_classes_
  bool IsImageClass(const std::string& descriptor) const;

  void PreCompile(const ClassLoader* class_loader, const std::vector<const DexFile*>& dex_files, TimingLogger& timings);
  void PostCompile(const ClassLoader* class_loader, const std::vector<const DexFile*>& dex_files);

  // Attempt to resolve all type, methods, fields, and strings
  // referenced from code in the dex file following PathClassLoader
  // ordering semantics.
  void Resolve(const ClassLoader* class_loader, const std::vector<const DexFile*>& dex_files, TimingLogger& timings);
  void ResolveDexFile(const ClassLoader* class_loader, const DexFile& dex_file, TimingLogger& timings);

  void Verify(const ClassLoader* class_loader, const std::vector<const DexFile*>& dex_files);
  void VerifyDexFile(const ClassLoader* class_loader, const DexFile& dex_file);

  void InitializeClassesWithoutClinit(const ClassLoader* class_loader, const std::vector<const DexFile*>& dex_files);
  void InitializeClassesWithoutClinit(const ClassLoader* class_loader, const DexFile& dex_file);

  void Compile(const ClassLoader* class_loader,
               const std::vector<const DexFile*>& dex_files);
  void CompileDexFile(const ClassLoader* class_loader, const DexFile& dex_file);
  void CompileClass(const DexFile::ClassDef& class_def, const ClassLoader* class_loader,
                    const DexFile& dex_file);
  void CompileMethod(const DexFile::CodeItem* code_item, uint32_t access_flags, uint32_t method_idx,
                     const ClassLoader* class_loader, const DexFile& dex_file);

  static void CompileClass(Context* context, size_t class_def_index);

  void SetGcMaps(const ClassLoader* class_loader, const std::vector<const DexFile*>& dex_files);
  void SetGcMapsDexFile(const ClassLoader* class_loader, const DexFile& dex_file);
  void SetGcMapsMethod(const DexFile& dex_file, Method* method);

  void InsertInvokeStub(bool is_static, const char* shorty,
                        const CompiledInvokeStub* compiled_invoke_stub);

  std::vector<const PatchInformation*> code_to_patch_;
  std::vector<const PatchInformation*> methods_to_patch_;

  InstructionSet instruction_set_;

  typedef std::map<const ClassReference, CompiledClass*> ClassTable;
  // All class references that this compiler has compiled
  mutable Mutex compiled_classes_lock_;
  ClassTable compiled_classes_;

  typedef std::map<const MethodReference, CompiledMethod*> MethodTable;
  // All method references that this compiler has compiled
  mutable Mutex compiled_methods_lock_;
  MethodTable compiled_methods_;

  typedef std::map<std::string, const CompiledInvokeStub*> InvokeStubTable;
  // Invocation stubs created to allow invocation of the compiled methods
  mutable Mutex compiled_invoke_stubs_lock_;
  InvokeStubTable compiled_invoke_stubs_;

  bool image_;
  size_t thread_count_;
  bool support_debugging_;
  uint64_t start_ns_;

  UniquePtr<AOTCompilationStats> stats_;

  bool dump_stats_;
  bool dump_timings_;

  const std::set<std::string>* image_classes_;

#if defined(ART_USE_LLVM_COMPILER)
  std::string elf_filename_;
  std::string bitcode_filename_;
  typedef void (*CompilerCallbackFn)(Compiler& compiler);
  typedef MutexLock* (*CompilerMutexLockFn)(Compiler& compiler);
#endif
  void* compiler_library_;

  typedef CompiledMethod* (*CompilerFn)(Compiler& compiler,
                                        const DexFile::CodeItem* code_item,
                                        uint32_t access_flags, uint32_t method_idx,
                                        const ClassLoader* class_loader,
                                        const DexFile& dex_file);
  CompilerFn compiler_;

  void* compiler_context_;

  typedef CompiledMethod* (*JniCompilerFn)(Compiler& compiler,
                                           uint32_t access_flags, uint32_t method_idx,
                                           const ClassLoader* class_loader,
                                           const DexFile& dex_file);
  JniCompilerFn jni_compiler_;
  typedef CompiledInvokeStub* (*CreateInvokeStubFn)(Compiler& compiler, bool is_static,
                                                    const char* shorty, uint32_t shorty_len);
  CreateInvokeStubFn create_invoke_stub_;

  DISALLOW_COPY_AND_ASSIGN(Compiler);
};

inline bool operator<(const Compiler::ClassReference& lhs, const Compiler::ClassReference& rhs) {
  if (lhs.second < rhs.second) {
    return true;
  } else if (lhs.second > rhs.second) {
    return false;
  } else {
    return (lhs.first < rhs.first);
  }
}

}  // namespace art

#endif  // ART_SRC_COMPILER_H_
