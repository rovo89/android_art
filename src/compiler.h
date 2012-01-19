// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_COMPILER_H_
#define ART_SRC_COMPILER_H_

#include <map>

#include "compiled_class.h"
#include "compiled_method.h"
#include "constants.h"
#include "dex_cache.h"
#include "dex_file.h"
#include "jni_compiler.h"
#include "oat_file.h"
#include "object.h"
#include "runtime.h"
#include "unordered_map.h"

#include <set>
#include <string>

namespace art {

class Compiler {
 public:
  // Create a compiler targeting the requested "instruction_set".
  // "image" should be true if image specific optimizations should be
  // enabled.  "image_classes" lets the compiler know what classes it
  // can assume will be in the image, with NULL implying all available
  // classes.
  explicit Compiler(InstructionSet instruction_set,
                    bool image,
                    const std::set<std::string>* image_classes);

  ~Compiler();

  void CompileAll(const ClassLoader* class_loader,
                  const std::vector<const DexFile*>& dex_files);

  // Compile a single Method
  void CompileOne(const Method* method);

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

  static ByteArray* CreateJniDlysmLookupStub(InstructionSet instruction_set);

  // A class is uniquely located by its DexFile and the class_defs_ table index into that DexFile
  typedef std::pair<const DexFile*, uint32_t> ClassReference;
  struct ClassReferenceHash {
    size_t operator()(const ClassReference& id) const {
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
  CompiledClass* GetCompiledClass(ClassReference ref) const;

  // A method is uniquely located by its DexFile and the method_ids_ table index into that DexFile
  typedef std::pair<const DexFile*, uint32_t> MethodReference;
  struct MethodReferenceHash {
    size_t operator()(const MethodReference& id) const {
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
  CompiledMethod* GetCompiledMethod(MethodReference ref) const;

  const CompiledInvokeStub* FindInvokeStub(bool is_static, const char* shorty) const;

  // Callbacks from OAT/ART compiler to see what runtime checks must be generated
  bool CanAssumeTypeIsPresentInDexCache(const DexCache* dex_cache, uint32_t type_idx) const;
  bool CanAssumeStringIsPresentInDexCache(const DexCache* dex_cache, uint32_t string_idx) const {
    // TODO: Add support for loading strings referenced by image_classes_
    // See also Compiler::ResolveDexFile
    return IsImage() && image_classes_ == NULL && dex_cache->GetResolvedString(string_idx) != NULL;
  }
  bool CanAccessTypeWithoutChecks(uint32_t referrer_idx, const DexCache* dex_cache,
                                  const DexFile& dex_file, uint32_t type_idx) const {
    Class* resolved_class = dex_cache->GetResolvedType(type_idx);
    // We should never ask whether a type needs access checks to raise a verification error,
    // all other cases where this following test could fail should have been rewritten by the
    // verifier to verification errors. Also need to handle a lack of knowledge at compile time.
#ifndef NDEBUG
    const DexFile::MethodId& method_id = dex_file.GetMethodId(referrer_idx);
    Class* referrer_class = dex_cache->GetResolvedType(method_id.class_idx_);
    DCHECK(resolved_class == NULL || referrer_class == NULL ||
           referrer_class->CanAccess(resolved_class));
#endif
    return resolved_class != NULL;
  }

 private:

  // Checks if class specified by type_idx is one of the image_classes_
  bool IsImageClass(const std::string& descriptor) const;

  void PreCompile(const ClassLoader* class_loader, const std::vector<const DexFile*>& dex_files);
  void PostCompile(const ClassLoader* class_loader, const std::vector<const DexFile*>& dex_files);

  // Attempt to resolve all type, methods, fields, and strings
  // referenced from code in the dex file following PathClassLoader
  // ordering semantics.
  void Resolve(const ClassLoader* class_loader, const std::vector<const DexFile*>& dex_files);
  void ResolveDexFile(const ClassLoader* class_loader, const DexFile& dex_file);

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

  void SetGcMaps(const ClassLoader* class_loader, const std::vector<const DexFile*>& dex_files);
  void SetGcMapsDexFile(const ClassLoader* class_loader, const DexFile& dex_file);
  void SetGcMapsMethod(const DexFile& dex_file, Method* method);

  // After compiling, walk all the DexCaches and set the code and
  // method pointers of CodeAndDirectMethods entries in the DexCaches.
  void SetCodeAndDirectMethods(const std::vector<const DexFile*>& dex_files);
  void SetCodeAndDirectMethodsDexFile(const DexFile& dex_file);

  void InsertInvokeStub(bool is_static, const char* shorty,
                        const CompiledInvokeStub* compiled_invoke_stub);

  InstructionSet instruction_set_;
  JniCompiler jni_compiler_;

  typedef std::tr1::unordered_map<const ClassReference, CompiledClass*, ClassReferenceHash> ClassTable;
  // All class references that this compiler has compiled
  ClassTable compiled_classes_;

  typedef std::tr1::unordered_map<const MethodReference, CompiledMethod*, MethodReferenceHash> MethodTable;
  // All method references that this compiler has compiled
  MethodTable compiled_methods_;

  typedef std::map<std::string, const CompiledInvokeStub*> InvokeStubTable;
  // Invocation stubs created to allow invocation of the compiled methods
  InvokeStubTable compiled_invoke_stubs_;

  bool image_;

  const std::set<std::string>* image_classes_;

  DISALLOW_COPY_AND_ASSIGN(Compiler);
};

}  // namespace art

#endif  // ART_SRC_COMPILER_H_
