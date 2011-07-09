// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_CLASS_LINKER_H_
#define ART_SRC_CLASS_LINKER_H_

#include <map>
#include <utility>
#include <vector>

#include "src/macros.h"
#include "src/thread.h"
#include "src/object.h"

namespace art {

class ClassLinker {
 public:
  ClassLinker() {}
  ~ClassLinker() {}

  // Finds a class by its descriptor name.
  Class* FindClass(const char* descriptor,
                   Object* class_loader,
                   DexFile* dex_file);

  Class* FindPrimitiveClass(const char* descriptor);

  bool InitializeClass(Class* klass);

  Class* LookupClass(const char* descriptor, Object* class_loader);

  Class* ResolveClass(const Class* referring, uint32_t class_idx);

  String* ResolveString(const Class* referring, uint32_t string_idx);

  DexFile* FindInClassPath(const char* descriptor);

  void AppendToClassPath(DexFile* dex_file);

 private:
  // Inserts a class into the class table.  Returns true if the class
  // was inserted.
  bool InsertClass(Class* klass);

  bool InitializeSuperClass(Class* klass);

  void InitializeStaticFields(Class* klass);

  bool ValidateSuperClassDescriptors(const Class* klass);

  bool HasSameDescriptorClasses(const char* descriptor,
                                const Class* klass1,
                                const Class* klass2);

  bool HasSameMethodDescriptorClasses(const Method* descriptor,
                                      const Class* klass1,
                                      const Class* klass2);

  bool LinkClass(Class* klass);

  bool LinkSuperClass(Class* klass);

  bool LinkInterfaces(Class* klass);

  bool LinkMethods(Class* klass);

  bool LinkVirtualMethods(Class* klass);

  bool LinkInterfaceMethods(Class* klass);

  void LinkAbstractMethods(Class* klass);

  bool LinkInstanceFields(Class* klass);

  void CreateReferenceOffsets(Class* klass);

  std::vector<DexFile*> class_path_;

  // TODO: multimap
  typedef std::map<const char*, Class*, CStringLt> Table;

  Table classes_;

  Mutex* classes_lock_;

  // TODO: classpath

  DISALLOW_COPY_AND_ASSIGN(ClassLinker);
};

}  // namespace art

#endif  // ART_SRC_CLASS_LINKER_H_
