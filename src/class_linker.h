// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_CLASS_LINKER_H_
#define ART_SRC_CLASS_LINKER_H_

#include <map>
#include <utility>
#include <vector>

#include "heap.h"
#include "macros.h"
#include "dex_file.h"
#include "thread.h"
#include "object.h"
#include "gtest/gtest.h"

namespace art {

class ClassLinker {
 public:
  // Initializes the class linker.
  static ClassLinker* Create(const std::vector<DexFile*>& boot_class_path);

  ~ClassLinker() {}

  // Finds a class by its descriptor name.
  // If dex_file is null, searches boot_class_path_.
  Class* FindClass(const StringPiece& descriptor,
                   Object* class_loader,
                   const DexFile* dex_file);

  Class* FindSystemClass(const StringPiece& descriptor) {
    return FindClass(descriptor, NULL, NULL);
  }

  bool InitializeClass(Class* klass);

  Class* LookupClass(const StringPiece& descriptor, Object* class_loader);

  Class* ResolveClass(const Class* referring,
                      uint32_t class_idx,
                      const DexFile* dex_file);

  String* ResolveString(const Class* referring, uint32_t string_idx);

  void RegisterDexFile(const DexFile* dex_file);

  // TODO replace with heap interface
  typedef void (RootVistor)(Object* root, void* arg);
  void VisitRoots(RootVistor* root_visitor, void* arg);

 private:
  ClassLinker() {}

  void Init(const std::vector<DexFile*>& boot_class_path_);

  // For early bootstrapping by Init
  Class* AllocClass(Class* java_lang_Class);

  // Alloc* convenience functions to avoid needing to pass in Class*
  // values that are known to the ClassLinker such as
  // kObjectArrayClass and kJavaLangString etc.
  Class* AllocClass();
  DexCache* AllocDexCache();
  StaticField* AllocStaticField();
  InstanceField* AllocInstanceField();
  Method* AllocMethod();
  String* AllocStringFromModifiedUtf8(int32_t utf16_length, const char* utf8_data_in);
  template <class T>
  ObjectArray<T>* AllocObjectArray(size_t length) {
    return ObjectArray<T>::Alloc(class_roots_->Get(kObjectArrayClass), length);
  }

  Class* CreatePrimitiveClass(const StringPiece& descriptor);

  Class* CreateArrayClass(const StringPiece& descriptor,
                          Object* class_loader,
                          const DexFile* dex_file);

  Class* FindPrimitiveClass(char type);

  const DexFile* FindDexFile(const DexCache* dex_cache) const;

  DexCache* FindDexCache(const DexFile* dex_file) const;

  typedef std::pair<const DexFile*, const DexFile::ClassDef*> ClassPathEntry;

  void AppendToBootClassPath(DexFile* dex_file);

  ClassPathEntry FindInBootClassPath(const StringPiece& descriptor);

  void LoadClass(const DexFile& dex_file,
                 const DexFile::ClassDef& dex_class_def,
                 Class* klass);

  void LoadInterfaces(const DexFile& dex_file,
                      const DexFile::ClassDef& dex_class_def,
                      Class *klass);

  void LoadField(const DexFile& dex_file,
                 const DexFile::Field& dex_field,
                 Class* klass,
                 Field* dst);

  void LoadMethod(const DexFile& dex_file,
                  const DexFile::Method& dex_method,
                  Class* klass,
                  Method* dst);

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

  bool HasSameNameAndPrototype(const Method* m1, const Method* m2) const {
    return HasSameName(m1, m2) && HasSamePrototype(m1, m2);
  }

  bool HasSameName(const Method* m1, const Method* m2) const {
    return m1->GetName() == m2->GetName();
  }

  bool HasSamePrototype(const Method* m1, const Method* m2) const {
    return HasSameReturnType(m1, m2) && HasSameArgumentTypes(m1, m2);
  }

  bool HasSameReturnType(const Method* m1, const Method* m2) const;

  bool HasSameArgumentTypes(const Method* m1, const Method* m2) const;

  bool LinkClass(Class* klass, const DexFile* dex_file);

  bool LinkSuperClass(Class* klass);

  bool LinkInterfaces(Class* klass, const DexFile* dex_file);

  bool LinkMethods(Class* klass);

  bool LinkVirtualMethods(Class* klass);

  bool LinkInterfaceMethods(Class* klass);

  void LinkAbstractMethods(Class* klass);

  bool LinkInstanceFields(Class* klass);

  void CreateReferenceOffsets(Class* klass);

  std::vector<const DexFile*> boot_class_path_;

  std::vector<const DexFile*> dex_files_;

  std::vector<DexCache*> dex_caches_;

  // TODO: multimap
  typedef std::map<const StringPiece, Class*> Table;

  Table classes_;

  Mutex* classes_lock_;

  // TODO: classpath

  // indexes into class_roots_
  enum ClassRoots {
    kJavaLangClass,
    kJavaLangObject,
    kJavaLangReflectField,
    kJavaLangReflectMethod,
    kJavaLangString,
    kPrimitiveBoolean,
    kPrimitiveChar,
    kPrimitiveFloat,
    kPrimitiveDouble,
    kPrimitiveByte,
    kPrimitiveShort,
    kPrimitiveInt,
    kPrimitiveLong,
    kPrimitiveVoid,
    kObjectArrayClass,
    kCharArrayClass,
    kClassRootsMax,
  };
  ObjectArray<Class>* class_roots_;

  ObjectArray<Class>* array_interfaces_;
  InterfaceEntry* array_iftable_;

  bool init_done_;

  FRIEND_TEST(ClassLinkerTest, ProtoCompare);
  FRIEND_TEST(ClassLinkerTest, ProtoCompare2);
  FRIEND_TEST(DexCacheTest, Open);
  friend class ObjectTest;
  FRIEND_TEST(ObjectTest, AllocObjectArray);
  DISALLOW_COPY_AND_ASSIGN(ClassLinker);
};

}  // namespace art

#endif  // ART_SRC_CLASS_LINKER_H_
