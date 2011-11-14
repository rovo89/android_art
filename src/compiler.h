// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_COMPILER_H_
#define ART_SRC_COMPILER_H_

#include "compiled_method.h"
#include "constants.h"
#include "dex_cache.h"
#include "dex_file.h"
#include "jni_compiler.h"
#include "oat_file.h"
#include "object.h"
#include "runtime.h"
#include "unordered_map.h"

#include <string>

namespace art {

class Compiler {
 public:
  // Create a compiler targeting the requested "instruction_set".
  // "image" should be true if image specific optimizations should be enabled.
  explicit Compiler(InstructionSet instruction_set, bool image);

  ~Compiler();

  // Compile all Methods of all the Classes of all the DexFiles that are part of a ClassLoader.
  void CompileAll(const ClassLoader* class_loader);

  // Compile a single Method
  void CompileOne(const Method* method);

  InstructionSet GetInstructionSet() const {
    return instruction_set_;
  }

  bool IsImage() const {
    return image_;
  }

  void SetVerbose(bool verbose) {
    verbose_ = verbose;
  }

  bool IsVerbose() const {
    return verbose_;
  }

  // Stub to throw AbstractMethodError
  static ByteArray* CreateAbstractMethodErrorStub(InstructionSet instruction_set);


  // Generate the trampoline that's invoked by unresolved direct methods
  static ByteArray* CreateResolutionStub(InstructionSet instruction_set,
                                         Runtime::TrampolineType type);

  static ByteArray* CreateJniDlysmLookupStub(InstructionSet instruction_set);

  // A method is uniquely located by its DexFile and index into the method_id table of that dex file
  typedef std::pair<const DexFile*, uint32_t> MethodReference;

  CompiledMethod* GetCompiledMethod(MethodReference ref) const;
  const CompiledInvokeStub* FindInvokeStub(bool is_static, const char* shorty) const;

  // Callbacks from OAT/ART compiler to see what runtime checks must be generated
  bool CanAssumeTypeIsPresentInDexCache(const DexCache* dex_cache, uint32_t type_idx) const {
    return IsImage() && dex_cache->GetResolvedTypes()->Get(type_idx) != NULL;
  }
  bool CanAssumeStringIsPresentInDexCache(const DexCache* dex_cache, uint32_t string_idx) const {
    return IsImage() && dex_cache->GetStrings()->Get(string_idx) != NULL;
  }
  bool CanAccessTypeWithoutChecks(uint32_t referrer_idx, const DexCache* dex_cache,
                                  const DexFile& dex_file, uint32_t type_idx) const {
    Class* resolved_class = dex_cache->GetResolvedTypes()->Get(type_idx);
    // We should never ask whether a type needs access checks to raise a verification error,
    // all other cases where this following test could fail should have been rewritten by the
    // verifier to verification errors. Also need to handle a lack of knowledge at compile time.
#ifndef NDEBUG
    Class* referrer_class = dex_cache->GetResolvedTypes()
        ->Get(dex_file.GetMethodId(referrer_idx).class_idx_);
    DCHECK(resolved_class == NULL || referrer_class == NULL ||
           referrer_class->CanAccess(resolved_class));
#endif
    return resolved_class != NULL;
  }

 private:
  // Attempt to resolve all type, methods, fields, and strings
  // referenced from code in the dex file following PathClassLoader
  // ordering semantics.
  void Resolve(const ClassLoader* class_loader);
  void ResolveDexFile(const ClassLoader* class_loader, const DexFile& dex_file);

  void Verify(const ClassLoader* class_loader);
  void VerifyDexFile(const ClassLoader* class_loader, const DexFile& dex_file);

  void InitializeClassesWithoutClinit(const ClassLoader* class_loader);
  void InitializeClassesWithoutClinit(const ClassLoader* class_loader, const DexFile& dex_file);

  void Compile(const ClassLoader* class_loader);
  void CompileDexFile(const ClassLoader* class_loader, const DexFile& dex_file);
  void CompileClass(const DexFile::ClassDef& class_def, const ClassLoader* class_loader,
                    const DexFile& dex_file);
  void CompileMethod(const DexFile::CodeItem* code_item, uint32_t access_flags, uint32_t method_idx,
                     const ClassLoader* class_loader, const DexFile& dex_file);

  // After compiling, walk all the DexCaches and set the code and
  // method pointers of CodeAndDirectMethods entries in the DexCaches.
  void SetCodeAndDirectMethods(const ClassLoader* class_loader);
  void SetCodeAndDirectMethodsDexFile(const DexFile& dex_file);

  void InsertInvokeStub(bool is_static, const char* shorty,
                        const CompiledInvokeStub* compiled_invoke_stub);

  InstructionSet instruction_set_;
  JniCompiler jni_compiler_;

  struct MethodReferenceHash {
    size_t operator()(const MethodReference id) const {
      size_t dex = reinterpret_cast<size_t>(id.first);
      DCHECK_NE(dex, static_cast<size_t>(0));
      dex += 33;  // dex is an aligned pointer, get some non-zero low bits
      size_t idx = id.second;
      if (idx == 0) {  // special case of a method index of 0
        return dex * 5381;
      } else {
        return dex * idx;
      }
    }
  };
  typedef std::tr1::unordered_map<const MethodReference, CompiledMethod*, MethodReferenceHash> MethodTable;
  // All method references that this compiler has compiled
  MethodTable compiled_methods_;

  typedef std::tr1::unordered_map<std::string, const CompiledInvokeStub*> InvokeStubTable;
  // Invocation stubs created to allow invocation of the compiled methods
  InvokeStubTable compiled_invoke_stubs_;

  bool image_;

  bool verbose_;

  DISALLOW_COPY_AND_ASSIGN(Compiler);
};

}  // namespace art

#endif  // ART_SRC_COMPILER_H_
