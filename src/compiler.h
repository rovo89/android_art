// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_COMPILER_H_
#define ART_SRC_COMPILER_H_

#include "dex_file.h"
#include "object.h"

namespace art {

class Compiler {
 public:
  void Compile(std::vector<const DexFile*> class_path);

 private:
  // Attempt to resolve all type, methods, fields, and strings
  // referenced from code in the dex file following PathClassLoader
  // ordering semantics.
  void Resolve(const ClassLoader* class_loader);
  void ResolveDexFile(const ClassLoader* class_loader, const DexFile& dex_file);

  void CompileDexFile(const ClassLoader* class_loader, const DexFile& dex_file);
  void CompileClass(Class* klass);
  void CompileMethod(Method* klass);
};

}  // namespace art

#endif  // ART_SRC_COMPILER_H_
