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

#ifndef ART_SRC_CLASS_LINKER_H_
#define ART_SRC_CLASS_LINKER_H_

#include <map>
#include <utility>
#include <vector>

#include "dex_file.h"
#include "heap.h"
#include "macros.h"
#include "mutex.h"
#include "oat_file.h"
#include "object.h"
#include "unordered_map.h"
#include "unordered_set.h"

#include "gtest/gtest.h"

namespace art {

class ClassLoader;
class InternTable;
class ObjectLock;

class ClassLinker {
 public:
  // Creates the class linker by boot strapping from dex files.
  static ClassLinker* Create(const std::string& boot_class_path, InternTable* intern_table);

  // Creates the class linker from one or more images.
  static ClassLinker* Create(InternTable* intern_table);

  ~ClassLinker();

  // Finds a class by its descriptor, loading it if necessary.
  // If class_loader is null, searches boot_class_path_.
  Class* FindClass(const StringPiece& descriptor, const ClassLoader* class_loader);

  // Finds a class by its descriptor, returning NULL if it isn't wasn't loaded
  // by the given 'class_loader'.
  Class* LookupClass(const StringPiece& descriptor, const ClassLoader* class_loader);

  Class* FindPrimitiveClass(char type);

  Class* FindSystemClass(const StringPiece& descriptor) {
    return FindClass(descriptor, NULL);
  }

  void DumpAllClasses(int flags) const;

  size_t NumLoadedClasses() const;

  // Resolve a String with the given index from the DexFile, storing the
  // result in the DexCache.
  String* ResolveString(const DexFile& dex_file, uint32_t string_idx, DexCache* dex_cache);

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
  // result in the DexCache. The referrer is used to identify the
  // target DexCache and ClassLoader to use for resolution.
  Class* ResolveType(uint32_t type_idx, const Method* referrer) {
    Class* declaring_class = referrer->GetDeclaringClass();
    DexCache* dex_cache = declaring_class->GetDexCache();
    // TODO: we could check for a dex cache hit here
    const ClassLoader* class_loader = declaring_class->GetClassLoader();
    const DexFile& dex_file = FindDexFile(dex_cache);
    return ResolveType(dex_file, type_idx, dex_cache, class_loader);
  }

  Class* ResolveType(uint32_t type_idx, const Field* referrer) {
    Class* declaring_class = referrer->GetDeclaringClass();
    DexCache* dex_cache = declaring_class->GetDexCache();
    // TODO: we could check for a dex cache hit here
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

  Method* ResolveMethod(uint32_t method_idx, const Method* referrer, bool is_direct) {
    Class* declaring_class = referrer->GetDeclaringClass();
    DexCache* dex_cache = declaring_class->GetDexCache();
    // TODO: we could check for a dex cache hit here
    const ClassLoader* class_loader = declaring_class->GetClassLoader();
    const DexFile& dex_file = FindDexFile(dex_cache);
    return ResolveMethod(dex_file, method_idx, dex_cache, class_loader, is_direct);
  }

  Field* ResolveField(uint32_t field_idx, const Method* referrer, bool is_static) {
    Class* declaring_class = referrer->GetDeclaringClass();
    DexCache* dex_cache = declaring_class->GetDexCache();
    // TODO: we could check for a dex cache hit here
    const ClassLoader* class_loader = declaring_class->GetClassLoader();
    const DexFile& dex_file = FindDexFile(dex_cache);
    return ResolveField(dex_file, field_idx, dex_cache, class_loader, is_static);
  }

  // Resolve a field with a given ID from the DexFile, storing the
  // result in DexCache. The ClassLinker and ClassLoader are used as
  // in ResolveType. What is unique is the is_static argument which is
  // used to determine if we are resolving a static or non-static
  // field.
  Field* ResolveField(const DexFile& dex_file,
                      uint32_t field_idx,
                      DexCache* dex_cache,
                      const ClassLoader* class_loader,
                      bool is_static);

  // Get shorty from method index without resolution. Used to do handlerization.
  const char* MethodShorty(uint32_t method_idx, Method* referrer);

  // Returns true on success, false if there's an exception pending.
  // can_run_clinit=false allows the compiler to attempt to init a class,
  // given the restriction that no <clinit> execution is possible.
  bool EnsureInitialized(Class* c, bool can_run_clinit);

  // Initializes classes that have instances in the image but that have
  // <clinit> methods so they could not be initialized by the compiler.
  void RunRootClinits();

  void RegisterDexFile(const DexFile& dex_file);
  void RegisterDexFile(const DexFile& dex_file, DexCache* dex_cache);

  const std::vector<const DexFile*>& GetBootClassPath() {
    return boot_class_path_;
  }

  void VisitRoots(Heap::RootVisitor* visitor, void* arg) const;

  const DexFile& FindDexFile(const DexCache* dex_cache) const;
  DexCache* FindDexCache(const DexFile& dex_file) const;

  // TODO: replace this with multiple methods that allocate the correct managed type.
  template <class T>
  ObjectArray<T>* AllocObjectArray(size_t length) {
    return ObjectArray<T>::Alloc(GetClassRoot(kObjectArrayClass), length);
  }

  ObjectArray<Class>* AllocClassArray(size_t length) {
    return ObjectArray<Class>::Alloc(GetClassRoot(kClassArrayClass), length);
  }

  ObjectArray<StackTraceElement>* AllocStackTraceElementArray(size_t length);

  void VerifyClass(Class* klass);

 private:
  ClassLinker(InternTable*);

  // Initialize class linker by bootstraping from dex files
  void Init(const std::string& boot_class_path);

  // Initialize class linker from one or more images.
  void InitFromImage();
  OatFile* OpenOat(const Space* space);
  static void InitFromImageCallback(Object* obj, void* arg);
  struct InitFromImageCallbackState;

  void FinishInit();

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
  InterfaceEntry* AllocInterfaceEntry(Class* interface);

  Class* CreatePrimitiveClass(const char* descriptor,
                              Class::PrimitiveType type) {
    return InitializePrimitiveClass(AllocClass(sizeof(Class)), descriptor, type);
  }
  Class* InitializePrimitiveClass(Class* primitive_class,
                                  const char* descriptor,
                                  Class::PrimitiveType type);


  Class* CreateArrayClass(const StringPiece& descriptor,
                          const ClassLoader* class_loader);

  void AppendToBootClassPath(const DexFile& dex_file);
  void AppendToBootClassPath(const DexFile& dex_file, DexCache* dex_cache);

  void ConstructFieldMap(const DexFile& dex_file, const DexFile::ClassDef& dex_class_def,
      Class* c, std::map<int, Field*>& field_map);

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

  // Inserts a class into the class table.  Returns true if the class
  // was inserted.
  bool InsertClass(const StringPiece& descriptor, Class* klass);

  bool InitializeClass(Class* klass, bool can_run_clinit);
  bool WaitForInitializeClass(Class* klass, Thread* self, ObjectLock& lock);
  bool ValidateSuperClassDescriptors(const Class* klass);
  bool InitializeSuperClass(Class* klass, bool can_run_clinit);
  void InitializeStaticFields(Class* klass);

  bool HasSameDescriptorClasses(const char* descriptor,
                                const Class* klass1,
                                const Class* klass2);

  bool HasSameMethodDescriptorClasses(const Method* descriptor,
                                      const Class* klass1,
                                      const Class* klass2);

  bool LinkClass(Class* klass);

  bool LinkSuperClass(Class* klass);

  bool LoadSuperAndInterfaces(Class* klass, const DexFile& dex_file);

  bool LinkMethods(Class* klass);

  bool LinkVirtualMethods(Class* klass);

  bool LinkInterfaceMethods(Class* klass);

  bool LinkStaticFields(Class* klass);
  bool LinkInstanceFields(Class* klass);
  bool LinkFields(Class *klass, bool is_static);


  void CreateReferenceInstanceOffsets(Class* klass);
  void CreateReferenceStaticOffsets(Class* klass);
  void CreateReferenceOffsets(Class *klass, bool is_static,
                              uint32_t reference_offsets);

  // For use by ImageWriter to find DexCaches for its roots
  const std::vector<DexCache*>& GetDexCaches() {
    return dex_caches_;
  }

  // lock to protect ClassLinker state
  mutable Mutex lock_;

  std::vector<const OatFile*> oat_files_;

  std::vector<const DexFile*> boot_class_path_;

  std::vector<const DexFile*> dex_files_;
  std::vector<DexCache*> dex_caches_;

  // multimap from a StringPiece hash code of a class descriptor to
  // Class* instances. Results should be compared for a matching
  // Class::descriptor_ and Class::class_loader_.
  typedef std::tr1::unordered_multimap<size_t, Class*> Table;
  Table classes_;

  // indexes into class_roots_.
  // needs to be kept in sync with class_roots_descriptors_.
  enum ClassRoot {
    kJavaLangClass,
    kJavaLangObject,
    kClassArrayClass,
    kObjectArrayClass,
    kJavaLangString,
    kJavaLangReflectConstructor,
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
    DCHECK(klass->GetClassLoader() == NULL);
    DCHECK(klass->GetDescriptor() != NULL);
    DCHECK(klass->GetDescriptor()->Equals(GetClassRootDescriptor(class_root)));

    DCHECK(class_roots_ != NULL);
    DCHECK(class_roots_->Get(class_root) == NULL);
    class_roots_->Set(class_root, klass);
  }

  ObjectArray<Class>* GetClassRoots() {
    DCHECK(class_roots_ != NULL);
    return class_roots_;
  }

  static const char* class_roots_descriptors_[];

  const char* GetClassRootDescriptor(ClassRoot class_root) {
    const char* descriptor = class_roots_descriptors_[class_root];
    CHECK(descriptor != NULL);
    return descriptor;
  }

  ObjectArray<Class>* array_interfaces_;
  ObjectArray<InterfaceEntry>* array_iftable_;

  bool init_done_;

  InternTable* intern_table_;

  friend class CommonTest;
  FRIEND_TEST(DexCacheTest, Open);
  friend class ObjectTest;
  FRIEND_TEST(ObjectTest, AllocObjectArray);
  FRIEND_TEST(ExceptionTest, FindExceptionHandler);
  friend class ImageWriter;  // for GetClassRoots
  DISALLOW_COPY_AND_ASSIGN(ClassLinker);
};

}  // namespace art

#endif  // ART_SRC_CLASS_LINKER_H_
