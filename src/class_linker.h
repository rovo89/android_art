// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_CLASS_LINKER_H_
#define ART_SRC_CLASS_LINKER_H_

#include <map>
#include <utility>
#include <vector>

#include "dex_file.h"
#include "heap.h"
#include "macros.h"
#include "object.h"
#include "thread.h"
#include "unordered_map.h"
#include "unordered_set.h"

#include "gtest/gtest.h"

namespace art {

class ClassLoader;
class InternTable;

class ClassLinker {
 public:
  // Initializes the class linker using DexFile and an optional boot Space.
  static ClassLinker* Create(const std::vector<const DexFile*>& boot_class_path,
      InternTable* intern_table, Space* boot_space);

  ~ClassLinker();

  // Finds a class by its descriptor name.
  // If class_loader is null, searches boot_class_path_.
  Class* FindClass(const StringPiece& descriptor,
                   const ClassLoader* class_loader);

  Class* FindPrimitiveClass(char type);

  Class* FindSystemClass(const StringPiece& descriptor) {
    return FindClass(descriptor, NULL);
  }

  size_t NumLoadedClasses() const;

  // Resolve a String with the given index from the DexFile, storing the
  // result in the DexCache.
  const String* ResolveString(const DexFile& dex_file, uint32_t string_idx, DexCache* dex_cache);

  // Resolve a Type with the given index from the DexFile, storing the
  // result in the DexCache. The referrer is used to identity the
  // target DexCache and ClassLoader to use for resolution.
  Class* ResolveType(const DexFile& dex_file,
                     uint32_t type_idx,
                     const Class* referrer) {
    return ResolveType(dex_file,
                       type_idx,
                       referrer->GetDexCache(),
                       referrer->GetClassLoader());
  }

  // Resolve a Type with the given index from the DexFile, storing the
  // result in the DexCache. The referrer is used to identity the
  // target DexCache and ClassLoader to use for resolution.
  Class* ResolveType(uint32_t type_idx, const Method* referrer) {
    Class* declaring_class = referrer->GetDeclaringClass();
    DexCache* dex_cache = declaring_class->GetDexCache();
    const ClassLoader* class_loader = declaring_class->GetClassLoader();
    const DexFile& dex_file = FindDexFile(dex_cache);
    return ResolveType(dex_file, type_idx, dex_cache, class_loader);
  }

  // Resolve a type with the given ID from the DexFile, storing the
  // result in DexCache. The ClassLoader is used to search for the
  // type, since it may be referenced from but not contained within
  // the given DexFile.
  Class* ResolveType(const DexFile& dex_file,
                     uint32_t type_idx,
                     DexCache* dex_cache,
                     const ClassLoader* class_loader);

  static StaticStorageBase* InitializeStaticStorageFromCode(uint32_t type_idx,
                                                            const Method* referrer);

  // Resolve a method with a given ID from the DexFile, storing the
  // result in DexCache. The ClassLinker and ClassLoader are used as
  // in ResolveType. What is unique is the method type argument which
  // is used to determine if this method is a direct, static, or
  // virtual method.
  Method* ResolveMethod(const DexFile& dex_file,
                        uint32_t method_idx,
                        DexCache* dex_cache,
                        const ClassLoader* class_loader,
                        bool is_direct);

  Field* ResolveField(uint32_t field_idx, const Method* referrer) {
    Class* declaring_class = referrer->GetDeclaringClass();
    DexCache* dex_cache = declaring_class->GetDexCache();
    const ClassLoader* class_loader = declaring_class->GetClassLoader();
    const DexFile& dex_file = FindDexFile(dex_cache);
    return ResolveField(dex_file, field_idx, dex_cache, class_loader, true);
  }

  // Resolve a method with a given ID from the DexFile, storing the
  // result in DexCache. The ClassLinker and ClassLoader are used as
  // in ResolveType. What is unique is the is_static argument which is
  // used to determine if we are resolving a static or non-static
  // field.
  Field* ResolveField(const DexFile& dex_file,
                      uint32_t field_idx,
                      DexCache* dex_cache,
                      const ClassLoader* class_loader,
                      bool is_static);

  // Returns true on success, false if there's an exception pending.
  bool EnsureInitialized(Class* c);

  void RegisterDexFile(const DexFile& dex_file);
  void RegisterDexFile(const DexFile& dex_file, DexCache* dex_cache);

  const std::vector<const DexFile*>& GetBootClassPath() {
    return boot_class_path_;
  }

  void VisitRoots(Heap::RootVistor* root_visitor, void* arg) const;

  const DexFile& FindDexFile(const DexCache* dex_cache) const;
  DexCache* FindDexCache(const DexFile& dex_file) const;

  template <class T>
  ObjectArray<T>* AllocObjectArray(size_t length) {
    return ObjectArray<T>::Alloc(GetClassRoot(kObjectArrayClass), length);
  }

  ObjectArray<StackTraceElement>* AllocStackTraceElementArray(size_t length);

 private:
  ClassLinker(InternTable*);

  // Initialize class linker from DexFile instances.
  void Init(const std::vector<const DexFile*>& boot_class_path_);

  // Initialize class linker from pre-initialized space.
  void Init(const std::vector<const DexFile*>& boot_class_path_, Space* space);
  static void InitCallback(Object* obj, void *arg);
  struct InitCallbackState;

  void FinishInit();

  bool InitializeClass(Class* klass);

  // For early bootstrapping by Init
  Class* AllocClass(Class* java_lang_Class, size_t class_size);

  // Alloc* convenience functions to avoid needing to pass in Class*
  // values that are known to the ClassLinker such as
  // kObjectArrayClass and kJavaLangString etc.
  Class* AllocClass(size_t class_size);
  DexCache* AllocDexCache(const DexFile& dex_file);
  Field* AllocField();
  Method* AllocMethod();
  CodeAndDirectMethods* AllocCodeAndDirectMethods(size_t length);

  Class* CreatePrimitiveClass(const char* descriptor);

  Class* CreateArrayClass(const StringPiece& descriptor,
                          const ClassLoader* class_loader);

  void AppendToBootClassPath(const DexFile& dex_file);
  void AppendToBootClassPath(const DexFile& dex_file, DexCache* dex_cache);

  size_t SizeOfClass(const DexFile& dex_file,
                     const DexFile::ClassDef& dex_class_def);

  void LoadClass(const DexFile& dex_file,
                 const DexFile::ClassDef& dex_class_def,
                 Class* klass,
                 const ClassLoader* class_loader);

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

  Class* LookupClass(const StringPiece& descriptor, const ClassLoader* class_loader);

  // Inserts a class into the class table.  Returns true if the class
  // was inserted.
  bool InsertClass(const StringPiece& descriptor, Class* klass);

  bool InitializeSuperClass(Class* klass);

  void InitializeStaticFields(Class* klass);

  bool ValidateSuperClassDescriptors(const Class* klass);

  bool HasSameDescriptorClasses(const char* descriptor,
                                const Class* klass1,
                                const Class* klass2);

  bool HasSameMethodDescriptorClasses(const Method* descriptor,
                                      const Class* klass1,
                                      const Class* klass2);

  bool LinkClass(Class* klass, const DexFile& dex_file);

  bool LinkSuperClass(Class* klass);

  bool LoadSuperAndInterfaces(Class* klass, const DexFile& dex_file);

  bool LinkMethods(Class* klass);

  bool LinkVirtualMethods(Class* klass);

  bool LinkInterfaceMethods(Class* klass);

  void LinkAbstractMethods(Class* klass);

  bool LinkStaticFields(Class* klass);
  bool LinkInstanceFields(Class* klass);
  bool LinkFields(size_t field_offset,
                  size_t& num_reference_fields,
                  size_t num_fields,
                  ObjectArray<Field>* fields,
                  size_t& size);

  void CreateReferenceInstanceOffsets(Class* klass);
  void CreateReferenceStaticOffsets(Class* klass);
  void CreateReferenceOffsets(uint32_t& reference_offsets,
                              size_t num_reference_fields,
                              const ObjectArray<Field>* fields);

  std::vector<const DexFile*> boot_class_path_;

  std::vector<const DexFile*> dex_files_;

  std::vector<DexCache*> dex_caches_;

  // multimap from a StringPiece hash code of a class descriptor to
  // Class* instances. Results should be compared for a matching
  // Class::descriptor_ and Class::class_loader_.
  typedef std::tr1::unordered_multimap<size_t, Class*> Table;
  Table classes_;
  Mutex* classes_lock_;

  // indexes into class_roots_.
  // needs to be kept in sync with class_roots_descriptors_.
  enum ClassRoot {
    kJavaLangClass,
    kJavaLangObject,
    kObjectArrayClass,
    kJavaLangString,
    kJavaLangReflectField,
    kJavaLangReflectMethod,
    kJavaLangClassLoader,
    kDalvikSystemBaseDexClassLoader,
    kDalvikSystemPathClassLoader,
    kJavaLangStackTraceElement,
    kPrimitiveBoolean,
    kPrimitiveByte,
    kPrimitiveChar,
    kPrimitiveDouble,
    kPrimitiveFloat,
    kPrimitiveInt,
    kPrimitiveLong,
    kPrimitiveShort,
    kPrimitiveVoid,
    kBooleanArrayClass,
    kByteArrayClass,
    kCharArrayClass,
    kDoubleArrayClass,
    kFloatArrayClass,
    kIntArrayClass,
    kLongArrayClass,
    kShortArrayClass,
    kJavaLangStackTraceElementArrayClass,
    kClassRootsMax,
  };
  ObjectArray<Class>* class_roots_;

  Class* GetClassRoot(ClassRoot class_root) {
    DCHECK(class_roots_ != NULL);
    Class* klass = class_roots_->Get(class_root);
    DCHECK(klass != NULL);
    return klass;
  }

  void SetClassRoot(ClassRoot class_root, Class* klass) {
    DCHECK(!init_done_);

    DCHECK(klass != NULL);
    DCHECK(klass->class_loader_ == NULL);
    DCHECK(klass->descriptor_ != NULL);
    DCHECK(klass->descriptor_->Equals(GetClassRootDescriptor(class_root)));

    DCHECK(class_roots_ != NULL);
    DCHECK(class_roots_->Get(class_root) == NULL);
    class_roots_->Set(class_root, klass);
  }

  static const char* class_roots_descriptors_[kClassRootsMax];

  const char* GetClassRootDescriptor(ClassRoot class_root) {
    const char* descriptor = class_roots_descriptors_[class_root];
    CHECK(descriptor != NULL);
    return descriptor;
  }

  ObjectArray<Class>* array_interfaces_;
  InterfaceEntry* array_iftable_;

  bool init_done_;

  InternTable* intern_table_;

  friend class CommonTest;
  FRIEND_TEST(DexCacheTest, Open);
  friend class ObjectTest;
  FRIEND_TEST(ObjectTest, AllocObjectArray);
  FRIEND_TEST(ExceptionTest, FindExceptionHandler);
  DISALLOW_COPY_AND_ASSIGN(ClassLinker);
};

}  // namespace art

#endif  // ART_SRC_CLASS_LINKER_H_
