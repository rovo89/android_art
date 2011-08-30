// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_COMPILER_H_
#define ART_SRC_COMPILER_H_

#include "dex_file.h"
#include "object.h"

namespace art {

class Compiler {
 public:
  // Compile all Methods of all the Classes of all the DexFiles that are part of a ClassLoader.
  void CompileAll(const ClassLoader* class_loader);

  // Compile a single Method
  void CompileOne(Method* method);

 private:
  // Attempt to resolve all type, methods, fields, and strings
  // referenced from code in the dex file following PathClassLoader
  // ordering semantics.
  void Resolve(const ClassLoader* class_loader);
  void ResolveDexFile(const ClassLoader* class_loader, const DexFile& dex_file);

  void Compile(const ClassLoader* class_loader);
  void CompileDexFile(const ClassLoader* class_loader, const DexFile& dex_file);
  void CompileClass(Class* klass);
  void CompileMethod(Method* klass);

  // After compiling, walk all the DexCaches and set the code and
  // method pointers of CodeAndDirectMethods entries in the DexCaches.
  void SetCodeAndDirectMethods(const ClassLoader* class_loader);
  void SetCodeAndDirectMethodsDexFile(const DexFile& dex_file);
};

}  // namespace art

#endif  // ART_SRC_COMPILER_H_
