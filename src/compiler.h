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
#include "jni_compiler.h"
#include "oat_file.h"
#include "object.h"
#include "runtime.h"

namespace art {

class Context;
class TimingLogger;

class Compiler {
 public:
  // Create a compiler targeting the requested "instruction_set".
  // "image" should be true if image specific optimizations should be
  // enabled.  "image_classes" lets the compiler know what classes it
  // can assume will be in the image, with NULL implying all available
  // classes.
  explicit Compiler(InstructionSet instruction_set, bool image, size_t thread_count,
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

  static ByteArray* CreateJniDlsymLookupStub(InstructionSet instruction_set);

  // A class is uniquely located by its DexFile and the class_defs_ table index into that DexFile
  typedef std::pair<const DexFile*, uint32_t> ClassReference;

  CompiledClass* GetCompiledClass(ClassReference ref) const;

  // A method is uniquely located by its DexFile and the method_ids_ table index into that DexFile
  typedef std::pair<const DexFile*, uint32_t> MethodReference;

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
    if (resolved_class == NULL) {
      return false;  // Unknown class needs access checks.
    }
    const DexFile::MethodId& method_id = dex_file.GetMethodId(referrer_idx);
    Class* referrer_class = dex_cache->GetResolvedType(method_id.class_idx_);
    if (referrer_class == NULL) {
      return false;  // Incomplete referrer knowledge needs access check.
    }
    // Perform access check, will return true if access is ok or false if we're going to have to
    // check this at runtime (for example for class loaders).
    return referrer_class->CanAccess(resolved_class);
  }

 private:

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

  // After compiling, walk all the DexCaches and set the code and
  // method pointers of CodeAndDirectMethods entries in the DexCaches.
  void SetCodeAndDirectMethods(const std::vector<const DexFile*>& dex_files);
  void SetCodeAndDirectMethodsDexFile(const DexFile& dex_file);

  void InsertInvokeStub(bool is_static, const char* shorty,
                        const CompiledInvokeStub* compiled_invoke_stub);

  InstructionSet instruction_set_;
  JniCompiler jni_compiler_;

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
  uint64_t start_ns_;

  const std::set<std::string>* image_classes_;

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
