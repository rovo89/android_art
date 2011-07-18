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
  // Initializes the class linker.
  static ClassLinker* Create();

  ~ClassLinker() {}

  Class* AllocClass(DexFile* dex_file);
  StaticField* AllocStaticField();
  InstanceField* AllocInstanceField();
  Method* AllocMethod();

  // Finds a class by its descriptor name.
  Class* FindClass(const char* descriptor,
                   Object* class_loader,
                   DexFile* dex_file);

  Class* FindSystemClass(const char* descriptor) {
    return FindClass(descriptor, NULL, NULL);
  }

  bool LoadClass(const char* descriptor, Class* klass);

  bool LoadClass(const RawDexFile::ClassDef& class_def, Class* klass);

  Class* FindPrimitiveClass(JType type);

  bool InitializeClass(Class* klass);

  Class* LookupClass(const char* descriptor, Object* class_loader);

  Class* ResolveClass(const Class* referring, uint32_t class_idx);

  String* ResolveString(const Class* referring, uint32_t string_idx);

  typedef std::pair<DexFile*, const RawDexFile::ClassDef*> ClassPathEntry;

  ClassPathEntry FindInClassPath(const char* descriptor);

  void AppendToClassPath(DexFile* dex_file);

 private:
  ClassLinker() {}

  void Init();

  Class* CreatePrimitiveClass(JType type, const char* descriptor);

  void LoadInterfaces(const RawDexFile::ClassDef& class_def, Class *klass);

  void LoadField(Class* klass, const RawDexFile::Field& src, Field* dst);

  void LoadMethod(Class* klass, const RawDexFile::Method& src, Method* dst);

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

  Class* primitive_boolean_;
  Class* primitive_char_;
  Class* primitive_float_;
  Class* primitive_double_;
  Class* primitive_byte_;
  Class* primitive_short_;
  Class* primitive_int_;
  Class* primitive_long_;
  Class* primitive_void_;

  Class* java_lang_Class_;
  Class* java_lang_Object_;
  Class* java_lang_ref_Field_;
  Class* java_lang_ref_Method_;
  Class* java_lang_String_;
  Class* char_array_class_;

  DISALLOW_COPY_AND_ASSIGN(ClassLinker);
};

}  // namespace art

#endif  // ART_SRC_CLASS_LINKER_H_
