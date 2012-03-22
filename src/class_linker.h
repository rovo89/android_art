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
#include <string>
#include <utility>
#include <vector>

#include "dex_cache.h"
#include "dex_file.h"
#include "gtest/gtest.h"
#include "heap.h"
#include "macros.h"
#include "mutex.h"
#include "oat_file.h"
#include "object.h"
#include "stack_indirect_reference_table.h"

namespace art {

class ClassLoader;
class ImageSpace;
class InternTable;
class ObjectLock;

typedef bool (ClassVisitor)(Class* c, void* arg);

class ClassLinker {
 public:
  // Creates the class linker by boot strapping from dex files.
  static ClassLinker* CreateFromCompiler(const std::vector<const DexFile*>& boot_class_path,
                                         InternTable* intern_table);

  // Creates the class linker from an image.
  static ClassLinker* CreateFromImage(InternTable* intern_table);

  ~ClassLinker();

  // Finds a class by its descriptor, loading it if necessary.
  // If class_loader is null, searches boot_class_path_.
  Class* FindClass(const char* descriptor, const ClassLoader* class_loader);

  Class* FindSystemClass(const char* descriptor);

  // Define a new a class based on a ClassDef from a DexFile
  Class* DefineClass(const StringPiece& descriptor, const ClassLoader* class_loader,
                     const DexFile& dex_file, const DexFile::ClassDef& dex_class_def);

  // Finds a class by its descriptor, returning NULL if it isn't wasn't loaded
  // by the given 'class_loader'.
  Class* LookupClass(const char* descriptor, const ClassLoader* class_loader);

  // Finds all the classes with the given descriptor, regardless of ClassLoader.
  void LookupClasses(const char* descriptor, std::vector<Class*>& classes);

  Class* FindPrimitiveClass(char type);

  // General class unloading is not supported, this is used to prune
  // unwanted classes during image writing.
  bool RemoveClass(const char* descriptor, const ClassLoader* class_loader);

  void DumpAllClasses(int flags) const;

  void DumpForSigQuit(std::ostream& os) const;

  size_t NumLoadedClasses() const;

  // Resolve a String with the given index from the DexFile, storing the
  // result in the DexCache. The referrer is used to identify the
  // target DexCache and ClassLoader to use for resolution.
  String* ResolveString(uint32_t string_idx, const Method* referrer) {
    String* resolved_string = referrer->GetDexCacheStrings()->Get(string_idx);
    if (UNLIKELY(resolved_string == NULL)) {
      Class* declaring_class = referrer->GetDeclaringClass();
      DexCache* dex_cache = declaring_class->GetDexCache();
      const DexFile& dex_file = FindDexFile(dex_cache);
      resolved_string = ResolveString(dex_file, string_idx, dex_cache);
    }
    return resolved_string;
  }

  // Resolve a String with the given index from the DexFile, storing the
  // result in the DexCache.
  String* ResolveString(const DexFile& dex_file, uint32_t string_idx, DexCache* dex_cache);

  // Resolve a Type with the given index from the DexFile, storing the
  // result in the DexCache. The referrer is used to identity the
  // target DexCache and ClassLoader to use for resolution.
  Class* ResolveType(const DexFile& dex_file,
                     uint16_t type_idx,
                     const Class* referrer) {
    return ResolveType(dex_file,
                       type_idx,
                       referrer->GetDexCache(),
                       referrer->GetClassLoader());
  }

  // Resolve a Type with the given index from the DexFile, storing the
  // result in the DexCache. The referrer is used to identify the
  // target DexCache and ClassLoader to use for resolution.
  Class* ResolveType(uint16_t type_idx, const Method* referrer) {
    Class* resolved_type = referrer->GetDexCacheResolvedTypes()->Get(type_idx);
    if (UNLIKELY(resolved_type == NULL)) {
      Class* declaring_class = referrer->GetDeclaringClass();
      DexCache* dex_cache = declaring_class->GetDexCache();
      const ClassLoader* class_loader = declaring_class->GetClassLoader();
      const DexFile& dex_file = FindDexFile(dex_cache);
      resolved_type = ResolveType(dex_file, type_idx, dex_cache, class_loader);
    }
    return resolved_type;
  }

  Class* ResolveType(uint16_t type_idx, const Field* referrer) {
    Class* declaring_class = referrer->GetDeclaringClass();
    DexCache* dex_cache = declaring_class->GetDexCache();
    Class* resolved_type = dex_cache->GetResolvedType(type_idx);
    if (UNLIKELY(resolved_type == NULL)) {
      const ClassLoader* class_loader = declaring_class->GetClassLoader();
      const DexFile& dex_file = FindDexFile(dex_cache);
      resolved_type = ResolveType(dex_file, type_idx, dex_cache, class_loader);
    }
    return resolved_type;
  }

  // Resolve a type with the given ID from the DexFile, storing the
  // result in DexCache. The ClassLoader is used to search for the
  // type, since it may be referenced from but not contained within
  // the given DexFile.
  Class* ResolveType(const DexFile& dex_file,
                     uint16_t type_idx,
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
    Method* resolved_method = referrer->GetDexCacheResolvedMethods()->Get(method_idx);
    if (UNLIKELY(resolved_method == NULL || resolved_method->IsRuntimeMethod())) {
      Class* declaring_class = referrer->GetDeclaringClass();
      DexCache* dex_cache = declaring_class->GetDexCache();
      const ClassLoader* class_loader = declaring_class->GetClassLoader();
      const DexFile& dex_file = FindDexFile(dex_cache);
      resolved_method = ResolveMethod(dex_file, method_idx, dex_cache, class_loader, is_direct);
    }
    return resolved_method;
  }

  Field* ResolveField(uint32_t field_idx, const Method* referrer, bool is_static) {
    Field* resolved_field =
        referrer->GetDeclaringClass()->GetDexCache()->GetResolvedField(field_idx);
    if (UNLIKELY(resolved_field == NULL)) {
      Class* declaring_class = referrer->GetDeclaringClass();
      DexCache* dex_cache = declaring_class->GetDexCache();
      const ClassLoader* class_loader = declaring_class->GetClassLoader();
      const DexFile& dex_file = FindDexFile(dex_cache);
      resolved_field = ResolveField(dex_file, field_idx, dex_cache, class_loader, is_static);
    }
    return resolved_field;
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

  Field* ResolveFieldJLS(uint32_t field_idx, const Method* referrer) {
    Field* resolved_field =
        referrer->GetDeclaringClass()->GetDexCache()->GetResolvedField(field_idx);
    if (UNLIKELY(resolved_field == NULL)) {
      Class* declaring_class = referrer->GetDeclaringClass();
      DexCache* dex_cache = declaring_class->GetDexCache();
      const ClassLoader* class_loader = declaring_class->GetClassLoader();
      const DexFile& dex_file = FindDexFile(dex_cache);
      resolved_field = ResolveFieldJLS(dex_file, field_idx, dex_cache, class_loader);
    }
    return resolved_field;
  }

  // Resolve a field with a given ID from the DexFile, storing the
  // result in DexCache. The ClassLinker and ClassLoader are used as
  // in ResolveType. No is_static argument is provided so that Java
  // field resolution semantics are followed.
  Field* ResolveFieldJLS(const DexFile& dex_file,
                         uint32_t field_idx,
                         DexCache* dex_cache,
                         const ClassLoader* class_loader);

  // Get shorty from method index without resolution. Used to do handlerization.
  const char* MethodShorty(uint32_t method_idx, Method* referrer, uint32_t* length);

  // Returns true on success, false if there's an exception pending.
  // can_run_clinit=false allows the compiler to attempt to init a class,
  // given the restriction that no <clinit> execution is possible.
  bool EnsureInitialized(Class* c, bool can_run_clinit);

  // Initializes classes that have instances in the image but that have
  // <clinit> methods so they could not be initialized by the compiler.
  void RunRootClinits();

  void RegisterDexFile(const DexFile& dex_file);
  void RegisterDexFile(const DexFile& dex_file, SirtRef<DexCache>& dex_cache);

  void RegisterOatFile(const OatFile& oat_file);

  const std::vector<const DexFile*>& GetBootClassPath() {
    return boot_class_path_;
  }

  void VisitClasses(ClassVisitor* visitor, void* arg) const;

  void VisitRoots(Heap::RootVisitor* visitor, void* arg) const;

  const DexFile& FindDexFile(const DexCache* dex_cache) const;
  DexCache* FindDexCache(const DexFile& dex_file) const;
  bool IsDexFileRegistered(const DexFile& dex_file) const;
  void FixupDexCaches(Method* resolution_method) const;

  // Generate an oat file from a dex file
  bool GenerateOatFile(const std::string& dex_filename,
                       int oat_fd,
                       const std::string& oat_cache_filename);

  const OatFile* FindOatFileFromOatLocation(const std::string& location);

  // Finds the oat file for a dex location, generating the oat file if
  // it is missing or out of date. Returns the DexFile from within the
  // created oat file.
  const DexFile* FindOrCreateOatFileForDexLocation(const std::string& dex_location,
                                                   const std::string& oat_location);
  // Find a DexFile within an OatFile given a DexFile location. Note
  // that this returns null if the location checksum of the DexFile
  // does not match the OatFile.
  const DexFile* FindDexFileInOatFileFromDexLocation(const std::string& location);


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
  bool VerifyClassUsingOatFile(const DexFile& dex_file, Class* klass,
                               Class::Status& oat_file_class_status);
  void ResolveClassExceptionHandlerTypes(const DexFile& dex_file, Class* klass);
  void ResolveMethodExceptionHandlerTypes(const DexFile& dex_file, Method* klass);

  Class* CreateProxyClass(String* name, ObjectArray<Class>* interfaces, ClassLoader* loader,
                          ObjectArray<Method>* methods, ObjectArray<ObjectArray<Class> >* throws);
  std::string GetDescriptorForProxy(const Class* proxy_class);
  Method* FindMethodForProxy(const Class* proxy_class, const Method* proxy_method);

  // Get the oat code for a method when its class isn't yet initialized
  const void* GetOatCodeFor(const Method* method);

  pid_t GetClassesLockOwner(); // For SignalCatcher.
  pid_t GetDexLockOwner(); // For SignalCatcher.

 private:
  explicit ClassLinker(InternTable*);

  // Initialize class linker by bootstraping from dex files
  void InitFromCompiler(const std::vector<const DexFile*>& boot_class_path);

  // Initialize class linker from one or more images.
  void InitFromImage();
  OatFile* OpenOat(const ImageSpace* space);
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

  InterfaceEntry* AllocInterfaceEntry(Class* interface);

  Class* CreatePrimitiveClass(const char* descriptor, Primitive::Type type) {
    return InitializePrimitiveClass(AllocClass(sizeof(Class)), descriptor, type);
  }
  Class* InitializePrimitiveClass(Class* primitive_class,
                                  const char* descriptor,
                                  Primitive::Type type);


  Class* CreateArrayClass(const std::string& descriptor, const ClassLoader* class_loader);

  void AppendToBootClassPath(const DexFile& dex_file);
  void AppendToBootClassPath(const DexFile& dex_file, SirtRef<DexCache>& dex_cache);

  void ConstructFieldMap(const DexFile& dex_file, const DexFile::ClassDef& dex_class_def,
                         Class* c, std::map<uint32_t, Field*>& field_map);

  size_t SizeOfClass(const DexFile& dex_file,
                     const DexFile::ClassDef& dex_class_def);

  void LoadClass(const DexFile& dex_file,
                 const DexFile::ClassDef& dex_class_def,
                 SirtRef<Class>& klass,
                 const ClassLoader* class_loader);

  void LoadField(const DexFile& dex_file, const ClassDataItemIterator& it, SirtRef<Class>& klass,
                 SirtRef<Field>& dst);

  void LoadMethod(const DexFile& dex_file, const ClassDataItemIterator& dex_method,
                  SirtRef<Class>& klass, SirtRef<Method>& dst);

  void FixupStaticTrampolines(Class* klass);

  // Finds the associated oat class for a dex_file and descriptor
  const OatFile::OatClass* GetOatClass(const DexFile& dex_file, const char* descriptor);

  // Attempts to insert a class into a class table.  Returns NULL if
  // the class was inserted, otherwise returns an existing class with
  // the same descriptor and ClassLoader.
  Class* InsertClass(const StringPiece& descriptor, Class* klass, bool image_class);

  void RegisterDexFileLocked(const DexFile& dex_file, SirtRef<DexCache>& dex_cache);
  bool IsDexFileRegisteredLocked(const DexFile& dex_file) const;
  void RegisterOatFileLocked(const OatFile& oat_file);

  bool InitializeClass(Class* klass, bool can_run_clinit);
  bool WaitForInitializeClass(Class* klass, Thread* self, ObjectLock& lock);
  bool ValidateSuperClassDescriptors(const Class* klass);
  bool InitializeSuperClass(Class* klass, bool can_run_clinit);
  void InitializeStaticFields(Class* klass);

  bool IsSameDescriptorInDifferentClassContexts(const char* descriptor,
                                                const Class* klass1,
                                                const Class* klass2);

  bool IsSameMethodSignatureInDifferentClassContexts(const Method* descriptor,
                                                     const Class* klass1,
                                                     const Class* klass2);

  bool LinkClass(SirtRef<Class>& klass, ObjectArray<Class>* interfaces);

  bool LinkSuperClass(SirtRef<Class>& klass);

  bool LoadSuperAndInterfaces(SirtRef<Class>& klass, const DexFile& dex_file);

  bool LinkMethods(SirtRef<Class>& klass, ObjectArray<Class>* interfaces);

  bool LinkVirtualMethods(SirtRef<Class>& klass);

  bool LinkInterfaceMethods(SirtRef<Class>& klass, ObjectArray<Class>* interfaces);

  bool LinkStaticFields(SirtRef<Class>& klass);
  bool LinkInstanceFields(SirtRef<Class>& klass);
  bool LinkFields(SirtRef<Class>& klass, bool is_static);


  void CreateReferenceInstanceOffsets(SirtRef<Class>& klass);
  void CreateReferenceStaticOffsets(SirtRef<Class>& klass);
  void CreateReferenceOffsets(SirtRef<Class>& klass, bool is_static,
                              uint32_t reference_offsets);

  // For use by ImageWriter to find DexCaches for its roots
  const std::vector<DexCache*>& GetDexCaches() {
    return dex_caches_;
  }

  const OatFile* FindOpenedOatFileForDexFile(const DexFile& dex_file);
  const OatFile* FindOpenedOatFileFromDexLocation(const std::string& dex_location);
  const OatFile* FindOpenedOatFileFromOatLocation(const std::string& oat_location);

  Method* CreateProxyConstructor(SirtRef<Class>& klass, Class* proxy_class);
  Method* CreateProxyMethod(SirtRef<Class>& klass, SirtRef<Method>& prototype);

  std::vector<const DexFile*> boot_class_path_;

  std::vector<const DexFile*> dex_files_;
  std::vector<DexCache*> dex_caches_;
  std::vector<const OatFile*> oat_files_;
  // lock to protect concurrent access to dex_files_, dex_caches_, and oat_files_
  mutable Mutex dex_lock_;


  // multimap from a string hash code of a class descriptor to
  // Class* instances. Results should be compared for a matching
  // Class::descriptor_ and Class::class_loader_.
  // Protected by classes_lock_
  typedef std::multimap<size_t, Class*> Table;
  Class* LookupClass(const char* descriptor, const ClassLoader* class_loader,
                     size_t hash, const Table& classes);
  Table image_classes_;
  Table classes_;
  mutable Mutex classes_lock_;

  // indexes into class_roots_.
  // needs to be kept in sync with class_roots_descriptors_.
  enum ClassRoot {
    kJavaLangClass,
    kJavaLangObject,
    kClassArrayClass,
    kObjectArrayClass,
    kJavaLangString,
    kJavaLangRefReference,
    kJavaLangReflectConstructor,
    kJavaLangReflectField,
    kJavaLangReflectMethod,
    kJavaLangReflectProxy,
    kJavaLangClassLoader,
    kDalvikSystemBaseDexClassLoader,
    kDalvikSystemPathClassLoader,
    kJavaLangThrowable,
    kJavaLangClassNotFoundException,
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

  void SetClassRoot(ClassRoot class_root, Class* klass);

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

  ObjectArray<InterfaceEntry>* array_iftable_;

  bool init_done_;

  InternTable* intern_table_;

  friend class CommonTest;
  friend class ImageWriter;  // for GetClassRoots
  friend class ObjectTest;
  FRIEND_TEST(ClassLinkerTest, ClassRootDescriptors);
  FRIEND_TEST(DexCacheTest, Open);
  FRIEND_TEST(ExceptionTest, FindExceptionHandler);
  FRIEND_TEST(ObjectTest, AllocObjectArray);
  DISALLOW_COPY_AND_ASSIGN(ClassLinker);
};

}  // namespace art

#endif  // ART_SRC_CLASS_LINKER_H_
