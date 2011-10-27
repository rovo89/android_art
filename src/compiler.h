// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_COMPILER_H_
#define ART_SRC_COMPILER_H_

#include "compiled_method.h"
#include "constants.h"
#include "dex_file.h"
#include "jni_compiler.h"
#include "oat_file.h"
#include "object.h"
#include "runtime.h"
#include "unordered_map.h"

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

  const CompiledMethod* GetCompiledMethod(const Method* method) const;
  const CompiledInvokeStub* GetCompiledInvokeStub(const Method* method) const;

  // Callbacks from OAT/ART compiler to see what runtime checks must be generated
  bool CanAssumeTypeIsPresentInDexCache(const Method* referrer, uint32_t type_idx) const {
    return IsImage() && referrer->GetDexCacheResolvedTypes()->Get(type_idx) != NULL;
  }
  bool CanAssumeStringIsPresentInDexCache(const Method* referrer, uint32_t string_idx) const {
    return IsImage() && referrer->GetDexCacheStrings()->Get(string_idx) != NULL;
  }
  bool CanAccessTypeWithoutChecks(const Method* referrer, uint32_t type_idx) const {
    Class* resolved_class = referrer->GetDexCacheResolvedTypes()->Get(type_idx);
    // We should never ask whether a type needs access checks to raise a verification error,
    // all other cases where this following test could fail should have been rewritten by the
    // verifier to verification errors.
    DCHECK(resolved_class == NULL || referrer->GetDeclaringClass()->CanAccess(resolved_class));
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
  void CompileClass(const Class* klass);
  void CompileMethod(const Method* method);

  // After compiling, walk all the DexCaches and set the code and
  // method pointers of CodeAndDirectMethods entries in the DexCaches.
  void SetCodeAndDirectMethods(const ClassLoader* class_loader);
  void SetCodeAndDirectMethodsDexFile(const DexFile& dex_file);

  InstructionSet instruction_set_;
  JniCompiler jni_compiler_;

  typedef std::tr1::unordered_map<const Method*, CompiledMethod*, ObjectIdentityHash> MethodTable;
  MethodTable compiled_methods_;

  typedef std::tr1::unordered_map<const Method*, CompiledInvokeStub*, ObjectIdentityHash> InvokeStubTable;
  InvokeStubTable compiled_invoke_stubs_;

  bool image_;

  bool verbose_;

  DISALLOW_COPY_AND_ASSIGN(Compiler);
};

}  // namespace art

#endif  // ART_SRC_COMPILER_H_
