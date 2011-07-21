// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_CLASS_LINKER_H_
#define ART_SRC_CLASS_LINKER_H_

#include <map>
#include <utility>
#include <vector>

#include "macros.h"
#include "raw_dex_file.h"
#include "thread.h"
#include "object.h"
#include "gtest/gtest.h"

namespace art {

class ClassLinker {
 public:
  // Initializes the class linker.
  static ClassLinker* Create(std::vector<RawDexFile*> boot_class_path);

  ~ClassLinker() {}

  DexFile* AllocDexFile();
  Class* AllocClass(DexFile* dex_file);
  StaticField* AllocStaticField();
  InstanceField* AllocInstanceField();
  Method* AllocMethod();
  ObjectArray* AllocObjectArray(size_t length);

  // Finds a class by its descriptor name.
  // If raw_dex_file is null, searches boot_class_path_.
  Class* FindClass(const StringPiece& descriptor,
                   Object* class_loader,
                   const RawDexFile* raw_dex_file);

  Class* FindSystemClass(const StringPiece& descriptor) {
    return FindClass(descriptor, NULL, NULL);
  }

  bool InitializeClass(Class* klass);

  Class* LookupClass(const StringPiece& descriptor, Object* class_loader);

  Class* ResolveClass(const Class* referring,
                      uint32_t class_idx,
                      const RawDexFile* raw_dex_file);

  String* ResolveString(const Class* referring, uint32_t string_idx);

  void RegisterDexFile(RawDexFile* raw_dex_file);

 private:
  ClassLinker() {}

  void Init(std::vector<RawDexFile*> boot_class_path_);

  Class* CreatePrimitiveClass(const StringPiece& descriptor);

  Class* CreateArrayClass(const StringPiece& descriptor,
                          Object* class_loader,
                          const RawDexFile* raw_dex_file);

  Class* FindPrimitiveClass(char type);

  const RawDexFile* FindRawDexFile(const DexFile* dex_file) const;

  DexFile* FindDexFile(const RawDexFile* raw_dex_file) const;

  typedef std::pair<const RawDexFile*, const RawDexFile::ClassDef*> ClassPathEntry;

  void AppendToBootClassPath(RawDexFile* raw_dex_file);

  ClassPathEntry FindInBootClassPath(const StringPiece& descriptor);

  void LoadClass(const RawDexFile& raw_dex_file,
                 const RawDexFile::ClassDef& class_def,
                 Class* klass);

  void LoadInterfaces(const RawDexFile& raw_dex_file,
                      const RawDexFile::ClassDef& class_def,
                      Class *klass);

  void LoadField(const RawDexFile& raw_dex_file,
                 const RawDexFile::Field& src,
                 Class* klass,
                 Field* dst);

  void LoadMethod(const RawDexFile& raw_dex_file,
                  const RawDexFile::Method& src,
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

  bool LinkClass(Class* klass, const RawDexFile* raw_dex_file);

  bool LinkSuperClass(Class* klass);

  bool LinkInterfaces(Class* klass, const RawDexFile* raw_dex_file);

  bool LinkMethods(Class* klass);

  bool LinkVirtualMethods(Class* klass);

  bool LinkInterfaceMethods(Class* klass);

  void LinkAbstractMethods(Class* klass);

  bool LinkInstanceFields(Class* klass);

  void CreateReferenceOffsets(Class* klass);

  std::vector<RawDexFile*> boot_class_path_;

  std::vector<RawDexFile*> raw_dex_files_;

  std::vector<DexFile*> dex_files_;

  // TODO: multimap
  typedef std::map<const StringPiece, Class*> Table;

  Table classes_;

  Mutex* classes_lock_;

  // TODO: classpath

  Class* java_lang_Class_;
  Class* java_lang_Object_;
  Class* java_lang_ref_Field_;
  Class* java_lang_ref_Method_;
  Class* java_lang_Cloneable_;
  Class* java_io_Serializable_;
  Class* java_lang_String_;

  Class* primitive_boolean_;
  Class* primitive_char_;
  Class* primitive_float_;
  Class* primitive_double_;
  Class* primitive_byte_;
  Class* primitive_short_;
  Class* primitive_int_;
  Class* primitive_long_;
  Class* primitive_void_;

  Class* object_array_class_;
  Class* char_array_class_;

  FRIEND_TEST(ClassLinkerTest, ProtoCompare);
  FRIEND_TEST(ClassLinkerTest, ProtoCompare2);
  DISALLOW_COPY_AND_ASSIGN(ClassLinker);
};

}  // namespace art

#endif  // ART_SRC_CLASS_LINKER_H_
